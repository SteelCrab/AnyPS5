#pragma once

#include "Cpu.hpp"
#include "X64Decoder.hpp"
#include <vector>

namespace AnyPS5::Arm64 {

struct TranslatedBlock {
    std::vector<std::uint32_t> words;
    std::size_t instructions;
};

[[nodiscard]] TranslatedBlock Translate(const GuestMemory& memory, std::uint64_t address);

}
