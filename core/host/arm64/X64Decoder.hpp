#pragma once

#include <cstdint>
#include <span>

namespace AnyPS5::Arm64 {

enum class Operation : std::uint8_t {
    Move, Lea, Exchange, Add, Subtract, And, Or, Xor, Compare, Test, Increment, Decrement,
    Push, Pop, Jump, ConditionalJump, Call, Return, Nop
};
enum class OperandKind { Register, Immediate, Memory };

struct Operand {
    OperandKind kind = OperandKind::Immediate;
    std::uint8_t reg = 0;
    std::uint64_t immediate = 0;
    std::int8_t base = -1;
    std::int8_t index = -1;
    std::uint8_t scale = 0;
    std::int64_t displacement = 0;
    bool ripRelative = false;
};

struct Instruction {
    Operation operation;
    Operand destination;
    Operand source;
    std::uint8_t width = 32;
    std::uint8_t condition = 0;
    std::uint8_t size = 0;
    std::uint64_t address = 0;
    [[nodiscard]] bool TerminatesBlock() const;
};

[[nodiscard]] Instruction Decode(std::span<const std::uint8_t> code, std::uint64_t address);

}
