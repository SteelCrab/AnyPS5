#pragma once

#include "GuestMemory.hpp"
#include <functional>
#include <string>
#include <string_view>

namespace AnyPS5::Arm64 {

struct ElfImport {
    std::string_view name;
    std::uint8_t type;
    std::uint8_t binding;
    std::uint64_t size;
};

struct ElfOptions {
    std::uint64_t loadBias = 0x40000000;
    std::function<std::uint64_t(const ElfImport&)> resolveImport;
};

struct ElfImage {
    GuestMemory memory;
    std::uint64_t entry;
    std::uint64_t loadBias;
    std::vector<std::string> libraries;
    std::vector<std::uint64_t> initializers;
    std::vector<std::uint64_t> finalizers;
};

[[nodiscard]] ElfImage LoadElf(std::span<const std::byte> bytes, const ElfOptions& options = {});

}
