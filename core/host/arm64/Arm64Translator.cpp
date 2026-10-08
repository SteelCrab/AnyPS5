#include "Arm64Translator.hpp"
#include <bit>
#include <cstddef>
#include <cstdio>
#include <stdexcept>
#include <type_traits>

namespace AnyPS5::Arm64 {

namespace {

static_assert(std::is_standard_layout_v<CpuState>);
static_assert(std::endian::native == std::endian::little);

constexpr std::uint64_t Carry = 1, Parity = 4, Auxiliary = 16, Zero = 64, Sign = 128, Overflow = 2048;

std::uint64_t mask(std::uint64_t width) noexcept { return width == 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << width) - 1; }

std::uint64_t signExtend(CpuState*, std::uint64_t value, std::uint64_t width) noexcept {
    const auto sign = std::uint64_t{1} << (width - 1);
    return ((value & mask(width)) ^ sign) - sign;
}

std::uint64_t signHigh(CpuState*, std::uint64_t value, std::uint64_t width) noexcept {
    return (value & (std::uint64_t{1} << (width - 1))) != 0 ? mask(width) : 0;
}

std::uint64_t readMemory(CpuState* state, std::uint64_t address, std::uint64_t bytes) noexcept {
    std::uint64_t value = 0;
    if (!state->memory->TryRead(address, std::as_writable_bytes(std::span(&value, 1)).first(bytes))) {
        state->fault = 1;
        state->faultAddress = address;
    }
    return value;
}

void writeMemory(CpuState* state, std::uint64_t address, std::uint64_t value, std::uint64_t bytes) noexcept {
    if (!state->memory->TryWrite(address, std::as_bytes(std::span(&value, 1)).first(bytes))) {
        state->fault = 2;
        state->faultAddress = address;
    }
}

void readVector(CpuState* state, std::uint64_t address, std::uint64_t slot, std::uint64_t aligned) noexcept {
    if (aligned && (address & 15) != 0) state->fault = 5;
    else if (!state->memory->TryRead(address, state->vectorScratch[slot])) state->fault = 1;
    if (state->fault != 0) state->faultAddress = address;
}

void writeVector(CpuState* state, std::uint64_t address, std::uint64_t slot, std::uint64_t aligned) noexcept {
    if (aligned && (address & 15) != 0) state->fault = 5;
    else if (!state->memory->TryWrite(address, state->vectorScratch[slot])) state->fault = 2;
    if (state->fault != 0) state->faultAddress = address;
}

void updateFlags(CpuState* state, std::uint64_t left, std::uint64_t right, std::uint64_t result, std::uint64_t mode) noexcept {
    const auto width = mode >> 8;
    const auto sign = std::uint64_t{1} << (width - 1);
    left &= mask(width);
    right &= mask(width);
    result &= mask(width);
    const auto operation = static_cast<Operation>(mode & 0xffu);
    auto flags = state->rflags & ~(Carry | Parity | Zero | Sign | Overflow);
    if (result == 0) flags |= Zero;
    if ((result & sign) != 0) flags |= Sign;
    if (std::popcount(static_cast<std::uint8_t>(result)) % 2 == 0) flags |= Parity;
    const bool addition = operation == Operation::Add || operation == Operation::AddCarry || operation == Operation::Increment;
    const bool subtraction = operation == Operation::Subtract || operation == Operation::SubtractBorrow ||
        operation == Operation::Compare || operation == Operation::Decrement || operation == Operation::Negate;
    if (addition || subtraction) {
        flags &= ~Auxiliary;
        if (((left ^ right ^ result) & 16) != 0) flags |= Auxiliary;
        const bool carry = (state->rflags & Carry) != 0;
        if (addition ? result < left || (operation == Operation::AddCarry && carry && result == left) :
            left < right || (operation == Operation::SubtractBorrow && carry && left == right)) flags |= Carry;
        if ((addition ? ~(left ^ right) & (left ^ result) : (left ^ right) & (left ^ result)) & sign) flags |= Overflow;
    }
    if (operation == Operation::Increment || operation == Operation::Decrement) flags = (flags & ~Carry) | (state->rflags & Carry);
    state->rflags = flags | 2;
}

void multiplyFlags(CpuState* state, std::uint64_t left, std::uint64_t right, std::uint64_t width) noexcept {
    const auto sign = std::uint64_t{1} << (width - 1);
    left &= mask(width);
    right &= mask(width);
    const bool negativeLeft = (left & sign) != 0, negativeRight = (right & sign) != 0;
    const auto magnitudeLeft = negativeLeft ? (0 - left) & mask(width) : left;
    const auto magnitudeRight = negativeRight ? (0 - right) & mask(width) : right;
    const auto limit = negativeLeft != negativeRight ? sign : sign - 1;
    const bool overflow = magnitudeRight != 0 && magnitudeLeft > limit / magnitudeRight;
    state->rflags = (state->rflags & ~(Carry | Overflow)) | (overflow ? Carry | Overflow : 0) | 2;
}

void writeInteger(CpuState* state, std::size_t index, std::uint64_t value, std::uint64_t width) noexcept {
    state->registers[index] = (width < 32 ? state->registers[index] & ~mask(width) : 0) | (value & mask(width));
}

void wideMultiply(CpuState* state, std::uint64_t low, std::uint64_t high, std::uint64_t width, std::uint64_t signedResult) noexcept {
    high = width == 64 ? high : (low >> width) & mask(width);
    const auto expected = signedResult && (low & (std::uint64_t{1} << (width - 1))) != 0 ? mask(width) : 0;
    const bool overflow = high != expected;
    if (width == 8) writeInteger(state, 0, low, 16);
    else {
        writeInteger(state, 0, low, width);
        writeInteger(state, 2, high, width);
    }
    state->rflags = (state->rflags & ~(Carry | Overflow)) | (overflow ? Carry | Overflow : 0) | 2;
}

void divide(CpuState* state, std::uint64_t divisor, std::uint64_t width, std::uint64_t signedDivision) noexcept {
    const auto bits = mask(width);
    const auto sign = std::uint64_t{1} << (width - 1);
    auto low = state->registers[0] & bits;
    auto high = (width == 8 ? state->registers[0] >> 8 : state->registers[2]) & bits;
    divisor &= bits;
    if (divisor == 0) {
        state->fault = 3;
        return;
    }
    const bool negativeDividend = signedDivision && (high & sign) != 0;
    const bool negativeDivisor = signedDivision && (divisor & sign) != 0;
    if (negativeDividend) {
        low = (0 - low) & bits;
        high = (~high + (low == 0)) & bits;
    }
    if (negativeDivisor) divisor = (0 - divisor) & bits;
    if (high >= divisor) {
        state->fault = 4;
        return;
    }
    std::uint64_t quotient = 0, remainder = high;
    for (std::uint64_t bit = width; bit != 0; --bit) {
        const bool carry = (remainder & sign) != 0;
        remainder = ((remainder << 1) | ((low >> (bit - 1)) & 1)) & bits;
        if (carry || remainder >= divisor) {
            remainder = (remainder - divisor) & bits;
            quotient |= std::uint64_t{1} << (bit - 1);
        }
    }
    const bool negativeQuotient = negativeDividend != negativeDivisor;
    if (signedDivision && quotient > (negativeQuotient ? sign : sign - 1)) {
        state->fault = 4;
        return;
    }
    if (negativeQuotient) quotient = (0 - quotient) & bits;
    if (negativeDividend) remainder = (0 - remainder) & bits;
    if (width == 8) writeInteger(state, 0, quotient | (remainder << 8), 16);
    else {
        writeInteger(state, 0, quotient, width);
        writeInteger(state, 2, remainder, width);
    }
}

void shiftFlags(CpuState* state, std::uint64_t original, std::uint64_t count, std::uint64_t result, std::uint64_t mode) noexcept {
    if (count == 0) return;
    const auto width = mode >> 8;
    const auto sign = std::uint64_t{1} << (width - 1);
    const auto operation = static_cast<Operation>(mode & 0xffu);
    original &= mask(width);
    result &= mask(width);
    auto flags = state->rflags;
    const bool rotate = operation == Operation::RotateLeft || operation == Operation::RotateRight;
    if (rotate) {
        flags = (flags & ~Carry) | ((operation == Operation::RotateLeft ? result & 1 : (result & sign) != 0) ? Carry : 0);
    } else {
        if (count < width || operation == Operation::ShiftArithmetic) {
            const auto carry = operation == Operation::ShiftLeft ? (original >> (width - count)) & 1 :
                count >= width ? (original & sign) != 0 : (original >> (count - 1)) & 1;
            flags = (flags & ~Carry) | (carry ? Carry : 0);
        }
        flags &= ~(Parity | Zero | Sign);
        if (result == 0) flags |= Zero;
        if ((result & sign) != 0) flags |= Sign;
        if (std::popcount(static_cast<std::uint8_t>(result)) % 2 == 0) flags |= Parity;
    }
    if (count == 1) {
        flags &= ~Overflow;
        const auto high = (result & sign) != 0;
        const bool overflow = operation == Operation::ShiftLeft || operation == Operation::RotateLeft ? high != ((flags & Carry) != 0) :
            operation == Operation::ShiftRight ? (original & sign) != 0 :
            operation == Operation::RotateRight ? high != ((result & (sign >> 1)) != 0) : false;
        if (overflow) flags |= Overflow;
    }
    state->rflags = flags | 2;
}

std::uint64_t condition(CpuState* state, std::uint64_t index) noexcept {
    const auto flags = state->rflags;
    const bool carry = (flags & Carry) != 0, zero = (flags & Zero) != 0;
    const bool sign = (flags & Sign) != 0, overflow = (flags & Overflow) != 0;
    bool result = false;
    switch (index >> 1) {
    case 0: result = overflow; break;
    case 1: result = carry; break;
    case 2: result = zero; break;
    case 3: result = carry || zero; break;
    case 4: result = sign; break;
    case 5: result = (flags & Parity) != 0; break;
    case 6: result = sign != overflow; break;
    case 7: result = zero || sign != overflow; break;
    }
    return result != ((index & 1) != 0);
}

class Emitter {
public:
    Emitter() {
        words = {0xa9bd53f3u, 0xa9015bf5u, 0xa9027bf7u, 0xaa0003f3u};
    }
    void Constant(std::uint8_t destination, std::uint64_t value) {
        word(0xd2800000u | ((value & 0xffffu) << 5) | destination);
        for (std::uint32_t half = 1; half < 4; ++half) {
            const auto part = static_cast<std::uint32_t>((value >> (half * 16)) & 0xffffu);
            if (part != 0) word(0xf2800000u | (half << 21) | (part << 5) | destination);
        }
    }
    void Move(std::uint8_t destination, std::uint8_t source) { word(0xaa0003e0u | (source << 16) | destination); }
    void Load(std::uint8_t destination, std::size_t offset, std::uint8_t width = 64) {
        const auto opcode = width == 64 ? 0xf9400000u : width == 32 ? 0xb9400000u : width == 16 ? 0x79400000u : 0x39400000u;
        word(opcode | (static_cast<std::uint32_t>(offset / (width / 8)) << 10) | (19 << 5) | destination);
    }
    void Store(std::uint8_t source, std::size_t offset, std::uint8_t width = 64) {
        const auto opcode = width == 64 ? 0xf9000000u : width == 32 ? 0xb9000000u : width == 16 ? 0x79000000u : 0x39000000u;
        word(opcode | (static_cast<std::uint32_t>(offset / (width / 8)) << 10) | (19 << 5) | source);
    }
    void Binary(std::uint32_t opcode, std::uint8_t destination, std::uint8_t left, std::uint8_t right, std::uint8_t width = 64, std::uint8_t shift = 0) {
        word((opcode | (width == 64 ? 0x80000000u : 0)) | (right << 16) | (shift << 10) | (left << 5) | destination);
    }
    template<typename TFunction>
    void Call(TFunction function) {
        Move(0, 19);
        Constant(16, reinterpret_cast<std::uintptr_t>(function));
        word(0xd63f0200u);
    }
    void Address(const Operand& operand, std::uint8_t destination, std::uint64_t next, bool segment = true, bool popAddress = false) {
        if (operand.ripRelative) Constant(destination, next);
        else if (operand.base == 4 && popAddress) Move(destination, 23);
        else if (operand.base >= 0) Load(destination, static_cast<std::size_t>(operand.base) * 8);
        else Constant(destination, 0);
        if (operand.index >= 0) {
            Load(16, static_cast<std::size_t>(operand.index) * 8);
            Binary(0x0b000000u, destination, destination, 16, 64, operand.scale);
        }
        if (operand.displacement != 0) {
            Constant(17, static_cast<std::uint64_t>(operand.displacement));
            Binary(0x0b000000u, destination, destination, 17);
        }
        if (operand.address32) word(0x2a0003e0u | (destination << 16) | destination);
        if (segment && operand.segment != Segment::None) {
            Load(16, operand.segment == Segment::Fs ? offsetof(CpuState, fsBase) : offsetof(CpuState, gsBase));
            Binary(0x0b000000u, destination, destination, 16);
        }
    }
    void Read(const Operand& operand, std::uint8_t destination, std::uint8_t width, std::uint64_t next) {
        switch (operand.kind) {
        case OperandKind::Immediate:
            Constant(destination, operand.immediate & mask(width));
            break;
        case OperandKind::Register: Load(destination, operand.reg * 8 + (operand.highByte ? 1 : 0), width); break;
        case OperandKind::Memory:
            Address(operand, 1, next);
            Constant(2, width / 8);
            Call(readMemory);
            faultGuard();
            Move(destination, 0);
            break;
        }
    }
    void Write(const Operand& operand, std::uint8_t source, std::uint8_t width, std::uint64_t next, bool popAddress = false) {
        if (operand.kind == OperandKind::Register) {
            if (width == 32) {
                word(0x2a0003f0u | (source << 16));
                Store(16, operand.reg * 8);
            } else Store(source, operand.reg * 8 + (operand.highByte ? 1 : 0), width);
        }
        else if (operand.kind == OperandKind::Memory) {
            Address(operand, 1, next, true, popAddress);
            Move(2, source);
            Constant(3, width / 8);
            Call(writeMemory);
            faultGuard();
        } else throw std::runtime_error("An immediate cannot be a guest instruction destination");
    }
    void LoadVector(std::uint8_t destination, std::size_t offset) {
        word(0x3dc00260u | (static_cast<std::uint32_t>(offset / 16) << 10) | destination);
    }
    void StoreVector(std::uint8_t source, std::size_t offset) {
        word(0x3d800260u | (static_cast<std::uint32_t>(offset / 16) << 10) | source);
    }
    void ReadVector(const Operand& operand, std::uint8_t slot, std::uint64_t next, bool aligned = false) {
        if (operand.kind == OperandKind::Register) {
            LoadVector(16, offsetof(CpuState, xmm) + operand.reg * 16);
            StoreVector(16, offsetof(CpuState, vectorScratch) + slot * 16);
        } else if (operand.kind == OperandKind::Memory) {
            Address(operand, 1, next);
            Constant(2, slot);
            Constant(3, aligned);
            Call(readVector);
            faultGuard();
        } else throw std::runtime_error("An immediate cannot be a vector instruction source");
    }
    void WriteVector(const Operand& operand, std::uint8_t source, std::uint64_t next, bool aligned = false) {
        if (operand.kind == OperandKind::Register) StoreVector(source, offsetof(CpuState, xmm) + operand.reg * 16);
        else if (operand.kind == OperandKind::Memory) {
            StoreVector(source, offsetof(CpuState, vectorScratch));
            Address(operand, 1, next);
            Constant(2, 0);
            Constant(3, aligned);
            Call(writeVector);
            faultGuard();
        } else throw std::runtime_error("An immediate cannot be a vector instruction destination");
    }
    void Push(std::uint8_t source, std::uint8_t width = 64) {
        Load(23, 4 * 8);
        Constant(17, width / 8);
        Binary(0x4b000000u, 23, 23, 17);
        Move(1, 23);
        Move(2, source);
        Constant(3, width / 8);
        Call(writeMemory);
        faultGuard();
        Store(23, 4 * 8);
    }
    void Pop(std::uint8_t destination, std::uint64_t extra = 0, std::uint8_t width = 64, bool commit = true) {
        Load(1, 4 * 8);
        Constant(2, width / 8);
        Call(readMemory);
        faultGuard();
        Move(destination, 0);
        Load(23, 4 * 8);
        Constant(17, width / 8 + extra);
        Binary(0x0b000000u, 23, 23, 17);
        if (commit) Store(23, 4 * 8);
    }
    void InstructionCode(const Instruction& instruction) {
        const auto next = instruction.address + instruction.size;
        Constant(9, instruction.address);
        Store(9, offsetof(CpuState, rip));
        switch (instruction.operation) {
        case Operation::Move:
        case Operation::MoveSigned:
        case Operation::SignHigh:
            Read(instruction.source, 20, instruction.sourceWidth == 0 ? instruction.width : instruction.sourceWidth, next);
            if (instruction.operation != Operation::Move) {
                Move(1, 20);
                Constant(2, instruction.sourceWidth);
                if (instruction.operation == Operation::MoveSigned) Call(signExtend);
                else Call(signHigh);
                Move(20, 0);
            }
            Write(instruction.destination, 20, instruction.width, next);
            break;
        case Operation::Exchange:
            Read(instruction.destination, 20, instruction.width, next);
            Read(instruction.source, 21, instruction.width, next);
            Write(instruction.destination, 21, instruction.width, next);
            Write(instruction.source, 20, instruction.width, next);
            break;
        case Operation::Lea:
            Address(instruction.source, 20, next, false);
            Write(instruction.destination, 20, instruction.width, next);
            break;
        case Operation::Add:
        case Operation::AddCarry:
        case Operation::Subtract:
        case Operation::SubtractBorrow:
        case Operation::And:
        case Operation::Or:
        case Operation::Xor:
        case Operation::Compare:
        case Operation::Test:
        case Operation::Increment:
        case Operation::Decrement: {
            Read(instruction.destination, 20, instruction.width, next);
            Read(instruction.source, 21, instruction.width, next);
            std::uint32_t opcode = 0;
            switch (instruction.operation) {
            case Operation::Add:
            case Operation::AddCarry:
            case Operation::Increment: opcode = 0x0b000000u; break;
            case Operation::Subtract:
            case Operation::SubtractBorrow:
            case Operation::Compare:
            case Operation::Decrement: opcode = 0x4b000000u; break;
            case Operation::And:
            case Operation::Test: opcode = 0x0a000000u; break;
            case Operation::Or: opcode = 0x2a000000u; break;
            case Operation::Xor: opcode = 0x4a000000u; break;
            default: break;
            }
            Binary(opcode, 22, 20, 21, instruction.width);
            if (instruction.operation == Operation::AddCarry || instruction.operation == Operation::SubtractBorrow) {
                Load(9, offsetof(CpuState, rflags));
                Constant(17, 1);
                Binary(0x0a000000u, 23, 9, 17);
                Binary(opcode, 22, 22, 23, instruction.width);
            }
            if (instruction.operation != Operation::Compare && instruction.operation != Operation::Test) {
                Write(instruction.destination, 22, instruction.width, next);
            }
            Move(1, 20);
            Move(2, 21);
            Move(3, 22);
            Constant(4, static_cast<std::uint64_t>(instruction.operation) | (instruction.width << 8));
            Call(updateFlags);
            break;
        }
        case Operation::Negate:
        case Operation::Not:
            Read(instruction.destination, 20, instruction.width, next);
            if (instruction.operation == Operation::Negate) Binary(0x4b000000u, 22, 31, 20, instruction.width);
            else {
                Constant(21, mask(instruction.width));
                Binary(0x4a000000u, 22, 20, 21, instruction.width);
            }
            Write(instruction.destination, 22, instruction.width, next);
            if (instruction.operation == Operation::Negate) {
                Constant(1, 0);
                Move(2, 20);
                Move(3, 22);
                Constant(4, static_cast<std::uint64_t>(instruction.operation) | (instruction.width << 8));
                Call(updateFlags);
            }
            break;
        case Operation::ShiftLeft:
        case Operation::ShiftRight:
        case Operation::ShiftArithmetic:
        case Operation::RotateLeft:
        case Operation::RotateRight: {
            Read(instruction.destination, 20, instruction.width, next);
            Read(instruction.source, 21, 8, next);
            Constant(17, instruction.width == 64 ? 63 : 31);
            Binary(0x0a000000u, 21, 21, 17);
            const auto nativeWidth = instruction.width == 64 ? 64 : 32;
            std::uint32_t opcode = 0x1ac02000u;
            Move(22, 20);
            if (instruction.operation == Operation::ShiftRight) opcode = 0x1ac02400u;
            if (instruction.operation == Operation::ShiftArithmetic) {
                opcode = 0x1ac02800u;
                if (instruction.width < 32) {
                    Move(1, 20);
                    Constant(2, instruction.width);
                    Call(signExtend);
                    Move(22, 0);
                }
            }
            if (instruction.operation == Operation::RotateLeft || instruction.operation == Operation::RotateRight) {
                opcode = 0x1ac02c00u;
                if (instruction.width < 32) {
                    Binary(0x2a000000u, 22, 22, 22, 32, instruction.width);
                    if (instruction.width == 8) Binary(0x2a000000u, 22, 22, 22, 32, 16);
                }
            }
            if (instruction.operation == Operation::RotateLeft) Binary(0x4b000000u, 17, 31, 21, nativeWidth);
            word(opcode | (nativeWidth == 64 ? 0x80000000u : 0) |
                ((instruction.operation == Operation::RotateLeft ? 17 : 21) << 16) | (22 << 5) | 22);
            Write(instruction.destination, 22, instruction.width, next);
            Move(1, 20);
            Move(2, 21);
            Move(3, 22);
            Constant(4, static_cast<std::uint64_t>(instruction.operation) | (instruction.width << 8));
            Call(shiftFlags);
            break;
        }
        case Operation::Multiply:
        case Operation::MultiplyImmediate:
            Read(instruction.operation == Operation::Multiply ? instruction.destination : instruction.source, 20, instruction.width, next);
            Read(instruction.operation == Operation::Multiply ? instruction.source : instruction.extra, 21, instruction.width, next);
            word(0x1b007c00u | (instruction.width == 64 ? 0x80000000u : 0) | (21 << 16) | (20 << 5) | 22);
            Write(instruction.destination, 22, instruction.width, next);
            Move(1, 20);
            Move(2, 21);
            Constant(3, instruction.width);
            Call(multiplyFlags);
            break;
        case Operation::MultiplyUnsignedWide:
        case Operation::MultiplySignedWide: {
            Load(20, 0, instruction.width);
            Read(instruction.source, 21, instruction.width, next);
            const bool signedResult = instruction.operation == Operation::MultiplySignedWide;
            if (signedResult && instruction.width < 64) {
                Move(1, 20);
                Constant(2, instruction.width);
                Call(signExtend);
                Move(20, 0);
                Move(1, 21);
                Constant(2, instruction.width);
                Call(signExtend);
                Move(21, 0);
            }
            word(0x9b007c00u | (21 << 16) | (20 << 5) | 22);
            word((signedResult ? 0x9b407c00u : 0x9bc07c00u) | (21 << 16) | (20 << 5) | 23);
            Move(1, 22);
            Move(2, 23);
            Constant(3, instruction.width);
            Constant(4, signedResult);
            Call(wideMultiply);
            break;
        }
        case Operation::DivideUnsigned:
        case Operation::DivideSigned:
            Read(instruction.source, 20, instruction.width, next);
            Move(1, 20);
            Constant(2, instruction.width);
            Constant(3, instruction.operation == Operation::DivideSigned);
            Call(divide);
            faultGuard();
            break;
        case Operation::VectorMove:
            ReadVector(instruction.source, 0, next, instruction.aligned);
            LoadVector(18, offsetof(CpuState, vectorScratch));
            WriteVector(instruction.destination, 18, next, instruction.aligned);
            break;
        case Operation::VectorMoveScalar:
        case Operation::VectorMoveLowZero: {
            const auto& source = instruction.source;
            const auto& destination = instruction.destination;
            if (source.kind == OperandKind::Memory) Read(source, 20, instruction.width, next);
            else Load(20, offsetof(CpuState, xmm) + source.reg * 16, instruction.width);
            if (destination.kind == OperandKind::Memory) Write(destination, 20, instruction.width, next);
            else if (source.kind == OperandKind::Memory || instruction.operation == Operation::VectorMoveLowZero) {
                word(0x4f00e412u);
                word((instruction.width == 64 ? 0x4e081c00u : 0x4e041c00u) | (20 << 5) | 18);
                StoreVector(18, offsetof(CpuState, xmm) + destination.reg * 16);
            } else Store(20, offsetof(CpuState, xmm) + destination.reg * 16, instruction.width);
            break;
        }
        case Operation::VectorAnd:
        case Operation::VectorAndNot:
        case Operation::VectorOr:
        case Operation::VectorXor:
        case Operation::VectorAdd:
        case Operation::VectorSubtract:
        case Operation::VectorEqual:
        case Operation::VectorGreater:
        case Operation::VectorMultiply: {
            ReadVector(instruction.destination, 0, next);
            ReadVector(instruction.source, 1, next);
            LoadVector(16, offsetof(CpuState, vectorScratch));
            LoadVector(17, offsetof(CpuState, vectorScratch) + 16);
            std::uint32_t opcode = 0;
            switch (instruction.operation) {
            case Operation::VectorAnd: opcode = 0x4e311e12u; break;
            case Operation::VectorAndNot: opcode = 0x4e701e32u; break;
            case Operation::VectorOr: opcode = 0x4eb11e12u; break;
            case Operation::VectorXor: opcode = 0x6e311e12u; break;
            case Operation::VectorAdd: opcode = 0x4e318612u; break;
            case Operation::VectorSubtract: opcode = 0x6e318612u; break;
            case Operation::VectorEqual: opcode = 0x6e318e12u; break;
            case Operation::VectorGreater: opcode = 0x4e313612u; break;
            case Operation::VectorMultiply: opcode = 0x4e319e12u; break;
            default: break;
            }
            if (instruction.sourceWidth != 0) {
                const auto lane = std::countr_zero(static_cast<unsigned>(instruction.sourceWidth)) - 3;
                opcode |= lane << 22;
            }
            word(opcode);
            WriteVector(instruction.destination, 18, next);
            break;
        }
        case Operation::VectorShuffle: {
            ReadVector(instruction.source, 0, next);
            for (std::size_t half = 0; half < 2; ++half) {
                std::uint64_t indexes = 0;
                for (std::size_t byte = 0; byte < 8; ++byte) {
                    const auto position = half * 8 + byte;
                    const auto lane = (instruction.extra.immediate >> ((position / 4) * 2)) & 3;
                    indexes |= (lane * 4 + position % 4) << (byte * 8);
                }
                Constant(9, indexes);
                Store(9, offsetof(CpuState, vectorScratch) + 16 + half * 8);
            }
            LoadVector(16, offsetof(CpuState, vectorScratch));
            LoadVector(17, offsetof(CpuState, vectorScratch) + 16);
            word(0x4e110212u);
            WriteVector(instruction.destination, 18, next);
            break;
        }
        case Operation::MoveToVector:
            Read(instruction.source, 20, instruction.width, next);
            word(0x4f00e412u);
            word((instruction.width == 64 ? 0x4e081c00u : 0x4e041c00u) | (20 << 5) | 18);
            WriteVector(instruction.destination, 18, next);
            break;
        case Operation::MoveFromVector:
            ReadVector(instruction.source, 0, next);
            LoadVector(16, offsetof(CpuState, vectorScratch));
            word((instruction.width == 64 ? 0x9e660000u : 0x1e260000u) | (16 << 5) | 20);
            Write(instruction.destination, 20, instruction.width, next);
            break;
        case Operation::ConditionalMove:
            Read(instruction.source, 20, instruction.width, next);
            Read(instruction.destination, 21, instruction.width, next);
            Constant(1, instruction.condition);
            Call(condition);
            word(0xf100001fu);
            word(0x9a800000u | (21 << 16) | (1 << 12) | (20 << 5) | 22);
            Write(instruction.destination, 22, instruction.width, next);
            break;
        case Operation::SetCondition:
            Constant(1, instruction.condition);
            Call(condition);
            Move(20, 0);
            Write(instruction.destination, 20, 8, next);
            break;
        case Operation::Push:
            Read(instruction.source, 20, instruction.width, next);
            Push(20, instruction.width);
            break;
        case Operation::Pop:
            Pop(20, 0, instruction.width, instruction.destination.kind != OperandKind::Memory);
            Write(instruction.destination, 20, instruction.width, next, true);
            if (instruction.destination.kind == OperandKind::Memory) Store(23, 4 * 8);
            break;
        case Operation::Jump:
            Read(instruction.source, 22, 64, next);
            Store(22, offsetof(CpuState, rip));
            return;
        case Operation::ConditionalJump:
            Constant(1, instruction.condition);
            Call(condition);
            Constant(21, instruction.source.immediate);
            Constant(22, next);
            word(0xf100001fu);
            word(0x9a800000u | (22 << 16) | (1 << 12) | (21 << 5) | 20);
            Store(20, offsetof(CpuState, rip));
            return;
        case Operation::Call:
            Read(instruction.source, 22, 64, next);
            Constant(20, next);
            Push(20);
            Store(22, offsetof(CpuState, rip));
            return;
        case Operation::Return:
            Pop(20, instruction.source.immediate);
            Store(20, offsetof(CpuState, rip));
            return;
        case Operation::Nop: break;
        }
        Constant(9, next);
        Store(9, offsetof(CpuState, rip));
    }
    std::vector<std::uint32_t> Finish() {
        const auto end = words.size();
        for (const auto branch : faultBranches) {
            const auto distance = end - branch;
            if (distance >= (1u << 18)) throw std::runtime_error("ARM64 fault branch exceeds its instruction range");
            words[branch] |= static_cast<std::uint32_t>(distance) << 5;
        }
        word(0xa9427bf7u);
        word(0xa9415bf5u);
        word(0xa8c353f3u);
        word(0xd65f03c0u);
        return std::move(words);
    }
private:
    void word(std::uint32_t word) { words.push_back(word); }
    void faultGuard() {
        Load(9, offsetof(CpuState, fault));
        faultBranches.push_back(words.size());
        word(0xb5000009u);
    }
    std::vector<std::uint32_t> words;
    std::vector<std::size_t> faultBranches;
};

}

TranslatedBlock Translate(const GuestMemory& memory, std::uint64_t address) {
    Emitter emitter;
    std::size_t instructions = 0;
    while (instructions < 32) {
        const auto code = memory.Fetch(address, 15);
        if (code.empty() && instructions != 0) break;
        Instruction instruction{};
        try {
            if (code.empty()) throw std::runtime_error("Instruction address is not executable guest memory");
            instruction = Decode(code, address);
        } catch (const std::runtime_error& error) {
            if (instructions != 0) break;
            char location[80];
            std::snprintf(location, sizeof(location), "ARM64 translation at guest 0x%llx (byte 0x%02x): ", static_cast<unsigned long long>(address), code.empty() ? 0 : code[0]);
            throw std::runtime_error(std::string(location) + error.what());
        }
        emitter.InstructionCode(instruction);
        ++instructions;
        address += instruction.size;
        if (instruction.TerminatesBlock()) break;
    }
    return {emitter.Finish(), instructions};
}

}
