#pragma once

#include "GuestMemory.hpp"

namespace AnyPS5::Arm64 {

struct ElfImage {
    GuestMemory memory;
    std::uint64_t entry;
};

[[nodiscard]] ElfImage LoadElf(std::span<const std::byte> bytes);

}
