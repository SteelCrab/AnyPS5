#pragma once

#include <cstdint>
#include <span>

namespace AnyPS5::Arm64 {

enum class Operation : std::uint8_t {
    Move, MoveSigned, SignHigh, Lea, Exchange, Add, AddCarry, Subtract, SubtractBorrow,
    And, Or, Xor, Compare, Test, Increment, Decrement, Negate, Not,
    ShiftLeft, ShiftRight, ShiftArithmetic, RotateLeft, RotateRight,
    Multiply, MultiplyImmediate, MultiplyUnsignedWide, MultiplySignedWide, DivideUnsigned, DivideSigned,
    ConditionalMove, SetCondition,
    VectorMove, VectorMoveScalar, VectorMoveLowZero, VectorAnd, VectorAndNot, VectorOr, VectorXor, VectorAdd, VectorSubtract,
    VectorEqual, VectorGreater, VectorMultiply, VectorShuffle, MoveToVector, MoveFromVector,
    Push, Pop, Jump, ConditionalJump, Call, Return, Nop
};
enum class OperandKind { Register, Immediate, Memory };
enum class Segment { None, Fs, Gs };

struct Operand {
    OperandKind kind = OperandKind::Immediate;
    std::uint8_t reg = 0;
    std::uint64_t immediate = 0;
    std::int8_t base = -1;
    std::int8_t index = -1;
    std::uint8_t scale = 0;
    std::int64_t displacement = 0;
    bool ripRelative = false;
    bool highByte = false;
    bool address32 = false;
    Segment segment = Segment::None;
};

struct Instruction {
    Operation operation;
    Operand destination;
    Operand source;
    Operand extra;
    std::uint8_t width = 32;
    std::uint8_t sourceWidth = 0;
    std::uint8_t condition = 0;
    std::uint8_t size = 0;
    std::uint64_t address = 0;
    bool aligned = false;
    [[nodiscard]] bool TerminatesBlock() const;
};

[[nodiscard]] Instruction Decode(std::span<const std::uint8_t> code, std::uint64_t address);

}
