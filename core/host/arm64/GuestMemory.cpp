#include "GuestMemory.hpp"
#include <algorithm>
#include <cstring>
#include <cerrno>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <sys/mman.h>

namespace AnyPS5::Arm64 {

struct GuestMemory::Impl {
    struct Region {
        std::uint64_t address;
        std::size_t size;
        std::uint32_t permissions;
        void* data;
        std::shared_ptr<void> backing;
    };
    std::vector<Region> regions;
    std::uint64_t codeVersion = 0;
    mutable std::mutex mutex;
    bool vacant(std::uint64_t address, std::size_t size) const noexcept {
        if (size == 0 || size > std::numeric_limits<std::uint64_t>::max() - address) return false;
        for (const auto& region : regions) {
            if (address < region.address + region.size && region.address < address + size) return false;
        }
        return true;
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
    std::lock_guard lock(impl->mutex);
    if (size == 0 || size > std::numeric_limits<std::uint64_t>::max() - address || initial.size() > size || (permissions & ~7u) != 0) {
        throw std::runtime_error("Invalid guest memory mapping");
    }
    if (!impl->vacant(address, size)) throw std::runtime_error("Guest memory mappings overlap");
    impl->regions.reserve(impl->regions.size() + 1);
    void* data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (data == MAP_FAILED) throw std::system_error(errno, std::generic_category(), "Guest memory allocation failed");
    std::shared_ptr<void> backing(data, [size](void* storage) { munmap(storage, size); });
    if (!initial.empty()) std::memcpy(data, initial.data(), initial.size());
    impl->regions.push_back({address, size, permissions, data, std::move(backing)});
    if ((permissions & Execute) != 0) ++impl->codeVersion;
}

bool GuestMemory::TryRead(std::uint64_t address, std::span<std::byte> bytes) const noexcept {
    if (!impl) return false;
    std::lock_guard lock(impl->mutex);
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

void GuestMemory::Unmap(std::uint64_t address) {
    if (!impl) throw std::runtime_error("Guest memory was moved from");
    std::lock_guard lock(impl->mutex);
    const auto found = std::find_if(impl->regions.begin(), impl->regions.end(), [address](const auto& region) { return region.address == address; });
    if (found == impl->regions.end()) throw std::runtime_error("Guest memory mapping does not exist");
    if ((found->permissions & Execute) != 0) ++impl->codeVersion;
    impl->regions.erase(found);
}

bool GuestMemory::TryWrite(std::uint64_t address, std::span<const std::byte> bytes) noexcept {
    if (!impl) return false;
    std::lock_guard lock(impl->mutex);
    if (!impl->range(address, bytes.size(), Write)) return false;
    bool changedCode = false;
    while (!bytes.empty()) {
        const auto* region = impl->find(address);
        const auto offset = static_cast<std::size_t>(address - region->address);
        const auto size = std::min(bytes.size(), region->size - offset);
        const bool executable = std::any_of(impl->regions.begin(), impl->regions.end(), [region](const auto& alias) {
            return alias.backing == region->backing && (alias.permissions & Execute) != 0;
        });
        if (executable && std::memcmp(static_cast<const std::byte*>(region->data) + offset, bytes.data(), size) != 0) changedCode = true;
        std::memcpy(static_cast<std::byte*>(region->data) + offset, bytes.data(), size);
        bytes = bytes.subspan(size);
        address += size;
    }
    if (changedCode) ++impl->codeVersion;
    return true;
}

std::uint64_t GuestMemory::CodeVersion() const noexcept {
    if (!impl) return 0;
    std::lock_guard lock(impl->mutex);
    return impl->codeVersion;
}

bool GuestMemory::Contains(std::uint64_t address, std::size_t size, std::uint32_t permissions) const noexcept {
    if (!impl) return false;
    std::lock_guard lock(impl->mutex);
    return (permissions & ~7u) == 0 && impl->range(address, size, permissions);
}

std::vector<std::uint8_t> GuestMemory::Fetch(std::uint64_t address, std::size_t maximum) const {
    if (!impl) throw std::runtime_error("Guest memory was moved from");
    std::lock_guard lock(impl->mutex);
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

bool GuestMemory::Vacant(std::uint64_t address, std::size_t size) const noexcept {
    if (!impl) return false;
    std::lock_guard lock(impl->mutex);
    return impl->vacant(address, size);
}

void GuestMemory::MapAlias(std::uint64_t address, std::size_t size, std::uint32_t permissions, std::uint64_t source) {
    if (!impl) throw std::runtime_error("Guest memory was moved from");
    std::lock_guard lock(impl->mutex);
    if ((permissions & ~7u) != 0 || !impl->vacant(address, size)) throw std::runtime_error("Invalid or overlapping guest alias mapping");
    const auto* region = impl->find(source);
    if (!region || size > region->size - (source - region->address)) throw std::runtime_error("Guest alias source is outside one mapped backing");
    const Impl::Region alias{address, size, permissions, static_cast<std::byte*>(region->data) + (source - region->address), region->backing};
    impl->regions.push_back(alias);
    if ((permissions & Execute) != 0) ++impl->codeVersion;
}

void GuestMemory::Unmap(std::uint64_t address, std::size_t size) {
    if (!impl) throw std::runtime_error("Guest memory was moved from");
    std::lock_guard lock(impl->mutex);
    if (size == 0 || !impl->range(address, size, 0)) throw std::runtime_error("Guest unmap range is not fully mapped");
    const auto end = address + size;
    std::vector<Impl::Region> remaining;
    remaining.reserve(impl->regions.size() + 1);
    bool changedCode = false;
    for (const auto& region : impl->regions) {
        const auto regionEnd = region.address + region.size;
        if (regionEnd <= address || region.address >= end) { remaining.push_back(region); continue; }
        changedCode |= (region.permissions & Execute) != 0;
        if (region.address < address) remaining.push_back({region.address, static_cast<std::size_t>(address - region.address), region.permissions, region.data, region.backing});
        if (regionEnd > end) remaining.push_back({end, static_cast<std::size_t>(regionEnd - end), region.permissions, static_cast<std::byte*>(region.data) + (end - region.address), region.backing});
    }
    impl->regions.swap(remaining);
    if (changedCode) ++impl->codeVersion;
}

}
