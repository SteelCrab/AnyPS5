#include "GuestMemory.hpp"
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <sys/mman.h>

namespace AnyPS5::Arm64 {

struct GuestMemory::Impl {
    struct Region {
        std::uint64_t address;
        std::size_t size;
        std::uint32_t permissions;
        void* data;
    };
    std::vector<Region> regions;
    ~Impl() {
        for (const auto& region : regions) munmap(region.data, region.size);
    }
    const Region* find(std::uint64_t address) const noexcept {
        for (const auto& region : regions) {
            if (address >= region.address && address - region.address < region.size) return &region;
        }
        return nullptr;
    }
    bool range(std::uint64_t address, std::size_t size, std::uint32_t permission) const noexcept {
        if (size > std::numeric_limits<std::uint64_t>::max() - address) return false;
        while (size != 0) {
            const auto* region = find(address);
            if (!region || (region->permissions & permission) != permission) return false;
            const auto chunk = std::min(size, region->size - static_cast<std::size_t>(address - region->address));
            size -= chunk;
            address += chunk;
        }
        return true;
    }
};

GuestMemory::GuestMemory() : impl(std::make_unique<Impl>()) {}
GuestMemory::~GuestMemory() = default;
GuestMemory::GuestMemory(GuestMemory&&) noexcept = default;
GuestMemory& GuestMemory::operator=(GuestMemory&&) noexcept = default;

void GuestMemory::Map(std::uint64_t address, std::size_t size, std::uint32_t permissions, std::span<const std::byte> initial) {
    if (!impl) throw std::runtime_error("Guest memory was moved from");
    if (size == 0 || size > std::numeric_limits<std::uint64_t>::max() - address || initial.size() > size || (permissions & ~7u) != 0) {
        throw std::runtime_error("Invalid guest memory mapping");
    }
    if ((permissions & (Write | Execute)) == (Write | Execute)) throw std::runtime_error("Writable executable guest memory is not implemented");
    for (const auto& region : impl->regions) {
        if (address < region.address + region.size && region.address < address + size) throw std::runtime_error("Guest memory mappings overlap");
    }
    impl->regions.reserve(impl->regions.size() + 1);
    void* data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (data == MAP_FAILED) throw std::runtime_error("Guest memory allocation failed");
    if (!initial.empty()) std::memcpy(data, initial.data(), initial.size());
    impl->regions.push_back({address, size, permissions, data});
}

bool GuestMemory::TryRead(std::uint64_t address, std::span<std::byte> bytes) const noexcept {
    if (!impl) return false;
    if (!impl->range(address, bytes.size(), Read)) return false;
    while (!bytes.empty()) {
        const auto* region = impl->find(address);
        const auto offset = static_cast<std::size_t>(address - region->address);
        const auto size = std::min(bytes.size(), region->size - offset);
        std::memcpy(bytes.data(), static_cast<const std::byte*>(region->data) + offset, size);
        bytes = bytes.subspan(size);
        address += size;
    }
    return true;
}

bool GuestMemory::TryWrite(std::uint64_t address, std::span<const std::byte> bytes) noexcept {
    if (!impl) return false;
    if (!impl->range(address, bytes.size(), Write)) return false;
    while (!bytes.empty()) {
        const auto* region = impl->find(address);
        const auto offset = static_cast<std::size_t>(address - region->address);
        const auto size = std::min(bytes.size(), region->size - offset);
        std::memcpy(static_cast<std::byte*>(region->data) + offset, bytes.data(), size);
        bytes = bytes.subspan(size);
        address += size;
    }
    return true;
}

std::vector<std::uint8_t> GuestMemory::Fetch(std::uint64_t address, std::size_t maximum) const {
    if (!impl) throw std::runtime_error("Guest memory was moved from");
    std::vector<std::uint8_t> bytes;
    while (bytes.size() < maximum) {
        const auto* region = impl->find(address);
        if (!region || (region->permissions & Execute) == 0) break;
        const auto offset = static_cast<std::size_t>(address - region->address);
        const auto size = std::min(maximum - bytes.size(), region->size - offset);
        const auto* source = static_cast<const std::uint8_t*>(region->data) + offset;
        bytes.insert(bytes.end(), source, source + size);
        address += size;
        if (address == 0) break;
    }
    return bytes;
}

}
