#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace AnyPS5::Arm64 {

enum Permission : std::uint32_t { Execute = 1, Write = 2, Read = 4 };

class GuestMemory {
public:
    GuestMemory();
    ~GuestMemory();
    GuestMemory(GuestMemory&&) noexcept;
    GuestMemory& operator=(GuestMemory&&) noexcept;
    GuestMemory(const GuestMemory&) = delete;
    GuestMemory& operator=(const GuestMemory&) = delete;
    void Map(std::uint64_t address, std::size_t size, std::uint32_t permissions,
             std::span<const std::byte> initial = {});
    void Unmap(std::uint64_t address);
    void Unmap(std::uint64_t address, std::size_t size);
    void MapAlias(std::uint64_t address, std::size_t size, std::uint32_t permissions, std::uint64_t source);
    [[nodiscard]] bool Vacant(std::uint64_t address, std::size_t size) const noexcept;
    [[nodiscard]] bool TryRead(std::uint64_t address, std::span<std::byte> bytes) const noexcept;
    [[nodiscard]] bool TryWrite(std::uint64_t address, std::span<const std::byte> bytes) noexcept;
    [[nodiscard]] bool Contains(std::uint64_t address, std::size_t size, std::uint32_t permissions) const noexcept;
    [[nodiscard]] std::vector<std::uint8_t> Fetch(std::uint64_t address, std::size_t maximum) const;
    [[nodiscard]] std::uint64_t CodeVersion() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}
