#include "X64Decoder.hpp"
#include <codegen/x86/X64InstructionDecoder.hpp>
#include <stdexcept>
#include <utility>

namespace AnyPS5::Arm64 {

bool Instruction::TerminatesBlock() const {
    if (operation == Operation::Jump || operation == Operation::ConditionalJump ||
        operation == Operation::Call || operation == Operation::Return || operation == Operation::Push) return true;
    return destination.kind == OperandKind::Memory && operation != Operation::Compare &&
           operation != Operation::Test && operation != Operation::BitTest && operation != Operation::Lea;
}

namespace {

struct Prefixes {
    std::uint8_t rex = 0;
    std::uint8_t repeat = 0;
    bool word = false;
    bool address32 = false;
    Segment segment = Segment::None;
};

Operand reg(std::uint8_t index, std::uint8_t width = 64, std::uint8_t rex = 0) {
    Operand operand;
    operand.kind = OperandKind::Register;
    operand.highByte = width == 8 && rex == 0 && index >= 4 && index < 8;
    operand.reg = operand.highByte ? index - 4 : index;
    return operand;
}

Operand immediate(std::uint64_t value) {
    Operand operand;
    operand.immediate = value;
    return operand;
}

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> code) : code(code) {}
    std::uint8_t Byte() {
        if (position == code.size()) throw std::runtime_error("Truncated x86-64 instruction");
        return code[position++];
    }
    std::uint64_t Unsigned(std::size_t size) {
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < size; ++i) value |= static_cast<std::uint64_t>(Byte()) << (i * 8);
        return value;
    }
    std::int64_t Signed(std::size_t size) {
        const auto value = Unsigned(size);
        const auto sign = std::uint64_t{1} << (size * 8 - 1);
        return static_cast<std::int64_t>((value ^ sign) - sign);
    }
    std::size_t Position() const { return position; }
private:
    std::span<const std::uint8_t> code;
    std::size_t position = 0;
};

std::uint8_t prefixes(Reader& reader, Prefixes& prefix) {
    for (;;) {
        const auto byte = reader.Byte();
        if (byte >= 0x40 && byte <= 0x4f) {
            prefix.rex = byte;
            continue;
        }
        switch (byte) {
        case 0x66: prefix.word = true; break;
        case 0x67: prefix.address32 = true; break;
        case 0x64: prefix.segment = Segment::Fs; break;
        case 0x65: prefix.segment = Segment::Gs; break;
        case 0x26: case 0x2e: case 0x36: case 0x3e: prefix.segment = Segment::None; break;
        case 0xf2: case 0xf3: prefix.repeat = byte; break;
        case 0xf0: throw std::runtime_error("LOCK instruction translation is not implemented");
        default: return byte;
        }
        prefix.rex = 0;
    }
}

Operand rm(Reader& reader, std::uint8_t modrm, Prefixes prefix, std::uint8_t width) {
    const auto mode = modrm >> 6;
    const auto low = modrm & 7;
    if (mode == 3) return reg(low | ((prefix.rex & 1) << 3), width, prefix.rex);
    Operand operand;
    operand.kind = OperandKind::Memory;
    operand.address32 = prefix.address32;
    operand.segment = prefix.segment;
    if (low == 4) {
        const auto sib = reader.Byte();
        const auto base = sib & 7;
        const auto index = (sib >> 3) & 7;
        operand.scale = sib >> 6;
        if (index != 4 || (prefix.rex & 2) != 0) operand.index = index | ((prefix.rex & 2) << 2);
        if (base == 5 && mode == 0) operand.displacement = reader.Signed(4);
        else operand.base = base | ((prefix.rex & 1) << 3);
    } else if (low == 5 && mode == 0) {
        operand.ripRelative = true;
        operand.displacement = reader.Signed(4);
    } else {
        operand.base = low | ((prefix.rex & 1) << 3);
    }
    if (mode == 1) operand.displacement = reader.Signed(1);
    if (mode == 2) operand.displacement = reader.Signed(4);
    return operand;
}

Operation group(std::uint8_t opcode) {
    static constexpr Operation operations[]{Operation::Add, Operation::Or, Operation::AddCarry, Operation::SubtractBorrow,
        Operation::And, Operation::Subtract, Operation::Xor, Operation::Compare};
    return operations[opcode & 7];
}

Operation shift(std::uint8_t extension) {
    switch (extension) {
    case 0: return Operation::RotateLeft;
    case 1: return Operation::RotateRight;
    case 4: case 6: return Operation::ShiftLeft;
    case 5: return Operation::ShiftRight;
    case 7: return Operation::ShiftArithmetic;
    default: throw std::runtime_error("Rotate through carry translation is not implemented");
    }
}

}

Instruction Decode(std::span<const std::uint8_t> code, std::uint64_t address) {
    const auto length = Codegen::X64InstructionDecoder{}.Decode(code.data(), code.size());
    if (length > 15) throw std::runtime_error("x86-64 instruction exceeds 15 bytes");
    Reader reader(code.first(length));
    Prefixes prefix;
    const auto opcode = prefixes(reader, prefix);
    if (prefix.word && (opcode == 0xc2 || opcode == 0xc3 || opcode == 0xe8)) {
        throw std::runtime_error("16-bit near control transfer translation is not implemented");
    }
    if (prefix.repeat != 0 && (prefix.rex & 1) != 0 && opcode == 0x90) {
        throw std::runtime_error("Extended-register PAUSE encoding is not implemented");
    }
    if (prefix.repeat != 0 && opcode != 0x0f && !(prefix.repeat == 0xf3 && (opcode == 0x90 || opcode == 0xc3 || opcode == 0xc2))) {
        throw std::runtime_error("REP instruction translation is not implemented");
    }
    Instruction result{};
    result.width = (prefix.rex & 8) != 0 ? 64 : prefix.word ? 16 : 32;
    result.address = address;
    result.size = static_cast<std::uint8_t>(length);
    const auto modOperands = [&](std::uint8_t width = 0, std::uint8_t regWidth = 0) {
        if (width == 0) width = result.width;
        if (regWidth == 0) regWidth = width;
        const auto modrm = reader.Byte();
        result.destination = rm(reader, modrm, prefix, width);
        result.source = reg(((modrm >> 3) & 7) | ((prefix.rex & 4) << 1), regWidth, prefix.rex);
        return static_cast<std::uint8_t>((modrm >> 3) & 7);
    };
    const auto immediateBytes = [&] { return result.width == 16 ? 2 : 4; };
    if (opcode >= 0xb0 && opcode <= 0xbf) {
        if (opcode < 0xb8) result.width = 8;
        result.operation = Operation::Move;
        result.destination = reg((opcode & 7) | ((prefix.rex & 1) << 3), result.width, prefix.rex);
        result.source = immediate(reader.Unsigned(result.width / 8));
    } else if (opcode >= 0x88 && opcode <= 0x8d && opcode != 0x8c) {
        if (opcode == 0x88 || opcode == 0x8a) result.width = 8;
        modOperands();
        result.operation = opcode == 0x8d ? Operation::Lea : Operation::Move;
        if (opcode == 0x8a || opcode == 0x8b || opcode == 0x8d) std::swap(result.destination, result.source);
        if (opcode == 0x8d && result.source.kind != OperandKind::Memory) throw std::runtime_error("LEA requires a memory address operand");
    } else if (opcode == 0x86 || opcode == 0x87) {
        if (opcode == 0x86) result.width = 8;
        modOperands();
        if (result.destination.kind != OperandKind::Register) throw std::runtime_error("Atomic memory XCHG translation is not implemented");
        result.operation = Operation::Exchange;
    } else if (opcode >= 0x90 && opcode <= 0x97) {
        result.operation = opcode == 0x90 && (prefix.rex & 1) == 0 ? Operation::Nop : Operation::Exchange;
        result.destination = reg(0);
        result.source = reg((opcode & 7) | ((prefix.rex & 1) << 3));
    } else if (opcode == 0x98 || opcode == 0x99) {
        result.operation = opcode == 0x98 ? Operation::MoveSigned : Operation::SignHigh;
        result.destination = reg(opcode == 0x98 ? 0 : 2);
        result.source = reg(0);
        result.sourceWidth = opcode == 0x98 ? result.width / 2 : result.width;
    } else if (opcode == 0x63) {
        modOperands(result.width == 16 ? 16 : 32, result.width);
        std::swap(result.destination, result.source);
        result.operation = Operation::MoveSigned;
        result.sourceWidth = result.width == 16 ? 16 : 32;
    } else if (opcode == 0xc6 || opcode == 0xc7) {
        if (opcode == 0xc6) result.width = 8;
        if (modOperands() != 0) throw std::runtime_error("C6/C7 extension is not implemented");
        result.operation = Operation::Move;
        result.source = immediate(static_cast<std::uint64_t>(reader.Signed(result.width == 8 ? 1 : immediateBytes())));
    } else if (opcode == 0x80 || opcode == 0x81 || opcode == 0x83) {
        if (opcode == 0x80) result.width = 8;
        result.operation = group(modOperands());
        result.source = immediate(static_cast<std::uint64_t>(reader.Signed(opcode == 0x81 ? immediateBytes() : 1)));
    } else if (opcode <= 0x3b && (opcode & 7) <= 3) {
        if ((opcode & 1) == 0) result.width = 8;
        modOperands();
        result.operation = group(opcode >> 3);
        if ((opcode & 2) != 0) std::swap(result.destination, result.source);
    } else if (opcode <= 0x3d && ((opcode & 7) == 4 || (opcode & 7) == 5)) {
        if ((opcode & 1) == 0) result.width = 8;
        result.operation = group(opcode >> 3);
        result.destination = reg(0);
        result.source = immediate(static_cast<std::uint64_t>(reader.Signed(result.width == 8 ? 1 : immediateBytes())));
    } else if (opcode == 0x84 || opcode == 0x85) {
        if (opcode == 0x84) result.width = 8;
        modOperands();
        result.operation = Operation::Test;
    } else if (opcode == 0xa8 || opcode == 0xa9) {
        if (opcode == 0xa8) result.width = 8;
        result.operation = Operation::Test;
        result.destination = reg(0);
        result.source = immediate(static_cast<std::uint64_t>(reader.Signed(result.width == 8 ? 1 : immediateBytes())));
    } else if (opcode == 0xf6 || opcode == 0xf7) {
        if (opcode == 0xf6) result.width = 8;
        const auto extension = modOperands();
        if (extension == 0) {
            result.operation = Operation::Test;
            result.source = immediate(static_cast<std::uint64_t>(reader.Signed(result.width == 8 ? 1 : immediateBytes())));
        } else if (extension == 2 || extension == 3) {
            result.operation = extension == 2 ? Operation::Not : Operation::Negate;
        } else if (extension >= 4) {
            static constexpr Operation operations[]{Operation::MultiplyUnsignedWide, Operation::MultiplySignedWide,
                Operation::DivideUnsigned, Operation::DivideSigned};
            result.operation = operations[extension - 4];
            result.source = result.destination;
        } else throw std::runtime_error("F6/F7 extension is not implemented");
    } else if (opcode == 0xc0 || opcode == 0xc1 || (opcode >= 0xd0 && opcode <= 0xd3)) {
        if (opcode == 0xc0 || opcode == 0xd0 || opcode == 0xd2) result.width = 8;
        result.operation = shift(modOperands());
        result.source = opcode == 0xc0 || opcode == 0xc1 ? immediate(reader.Unsigned(1)) :
            opcode == 0xd0 || opcode == 0xd1 ? immediate(1) : reg(1);
        result.sourceWidth = 8;
    } else if (opcode == 0x69 || opcode == 0x6b) {
        modOperands();
        std::swap(result.destination, result.source);
        result.operation = Operation::MultiplyImmediate;
        result.extra = immediate(static_cast<std::uint64_t>(reader.Signed(opcode == 0x6b ? 1 : immediateBytes())));
    } else if (opcode >= 0x50 && opcode <= 0x5f) {
        result.width = prefix.word ? 16 : 64;
        result.operation = opcode < 0x58 ? Operation::Push : Operation::Pop;
        result.destination = reg((opcode & 7) | ((prefix.rex & 1) << 3));
        result.source = result.destination;
    } else if (opcode == 0x68 || opcode == 0x6a) {
        result.operation = Operation::Push;
        result.width = prefix.word ? 16 : 64;
        result.source = immediate(static_cast<std::uint64_t>(reader.Signed(opcode == 0x6a ? 1 : prefix.word ? 2 : 4)));
    } else if (opcode == 0x8f) {
        result.width = prefix.word ? 16 : 64;
        if (modOperands() != 0) throw std::runtime_error("8F extension is not implemented");
        result.operation = Operation::Pop;
    } else if (opcode == 0xe8 || opcode == 0xe9 || opcode == 0xeb) {
        result.operation = opcode == 0xe8 ? Operation::Call : Operation::Jump;
        result.source = immediate(address + length + static_cast<std::uint64_t>(reader.Signed(opcode == 0xeb ? 1 : 4)));
    } else if (opcode >= 0x70 && opcode <= 0x7f) {
        result.operation = Operation::ConditionalJump;
        result.condition = opcode & 15;
        result.source = immediate(address + length + static_cast<std::uint64_t>(reader.Signed(1)));
    } else if (opcode == 0x0f) {
        const auto extension = reader.Byte();
        const bool scalarMove = (prefix.repeat == 0xf3 || prefix.repeat == 0xf2) && !prefix.word && (extension == 0x10 || extension == 0x11);
        const bool vectorQuadMove = (prefix.repeat == 0xf3 && !prefix.word && extension == 0x7e) || (prefix.word && prefix.repeat == 0 && extension == 0xd6);
        if (prefix.repeat != 0 && !scalarMove && !vectorQuadMove && !(prefix.repeat == 0xf3 && !prefix.word && (extension == 0x6f || extension == 0x7f))) {
            throw std::runtime_error("SSE mandatory prefix or REP instruction is not implemented");
        }
        if (extension == 0xa3 || extension == 0xba) {
            const auto group = modOperands();
            if (extension == 0xba) {
                if (group != 4) throw std::runtime_error("Bit modification instruction translation is not implemented");
                result.source = immediate(reader.Unsigned(1));
            }
            result.operation = Operation::BitTest;
        } else if (vectorQuadMove) {
            result.width = 64;
            modOperands(64, 128);
            if (extension == 0x7e) std::swap(result.destination, result.source);
            result.operation = Operation::VectorMoveLowZero;
        } else if (extension == 0x10 || extension == 0x11 || extension == 0x28 || extension == 0x29 || extension == 0x6f || extension == 0x7f) {
            if ((extension == 0x6f || extension == 0x7f) && !prefix.word && prefix.repeat == 0) {
                throw std::runtime_error("MMX instruction translation is not implemented");
            }
            result.width = scalarMove ? (prefix.repeat == 0xf2 ? 64 : 32) : 128;
            modOperands(result.width, 128);
            if (extension == 0x10 || extension == 0x28 || extension == 0x6f) std::swap(result.destination, result.source);
            result.operation = scalarMove ? Operation::VectorMoveScalar : Operation::VectorMove;
            result.aligned = extension == 0x28 || extension == 0x29 || ((extension == 0x6f || extension == 0x7f) && prefix.word);
        } else if (extension >= 0x54 && extension <= 0x57) {
            static constexpr Operation operations[]{Operation::VectorAnd, Operation::VectorAndNot, Operation::VectorOr, Operation::VectorXor};
            result.width = 128;
            modOperands();
            std::swap(result.destination, result.source);
            result.operation = operations[extension - 0x54];
        } else if (prefix.word && (extension == 0xdb || extension == 0xdf || extension == 0xeb || extension == 0xef ||
                   extension == 0xd4 || (extension >= 0xf8 && extension <= 0xfe) || (extension >= 0x74 && extension <= 0x76) ||
                   (extension >= 0x64 && extension <= 0x66) || extension == 0xd5 || extension == 0x70)) {
            result.width = 128;
            modOperands();
            std::swap(result.destination, result.source);
            if (extension == 0xdb) result.operation = Operation::VectorAnd;
            else if (extension == 0xdf) result.operation = Operation::VectorAndNot;
            else if (extension == 0xeb) result.operation = Operation::VectorOr;
            else if (extension == 0xef) result.operation = Operation::VectorXor;
            else if (extension == 0xd5) {
                result.operation = Operation::VectorMultiply;
                result.sourceWidth = 16;
            } else if (extension == 0x70) {
                result.operation = Operation::VectorShuffle;
                result.extra = immediate(reader.Unsigned(1));
            } else if (extension >= 0x74 && extension <= 0x76) {
                result.operation = Operation::VectorEqual;
                result.sourceWidth = 8 << (extension - 0x74);
            } else if (extension >= 0x64 && extension <= 0x66) {
                result.operation = Operation::VectorGreater;
                result.sourceWidth = 8 << (extension - 0x64);
            } else {
                result.operation = extension >= 0xfc || extension == 0xd4 ? Operation::VectorAdd : Operation::VectorSubtract;
                result.sourceWidth = extension == 0xd4 || extension == 0xfb ? 64 : 8 << (extension & 3);
            }
        } else if (prefix.word && (extension == 0x6e || extension == 0x7e)) {
            result.width = (prefix.rex & 8) != 0 ? 64 : 32;
            modOperands(result.width, 128);
            result.operation = extension == 0x6e ? Operation::MoveToVector : Operation::MoveFromVector;
            if (extension == 0x6e) std::swap(result.destination, result.source);
        } else if (extension >= 0x80 && extension <= 0x8f) {
            result.operation = Operation::ConditionalJump;
            result.condition = extension & 15;
            result.source = immediate(address + length + static_cast<std::uint64_t>(reader.Signed(4)));
        } else if (extension >= 0x40 && extension <= 0x4f) {
            modOperands();
            std::swap(result.destination, result.source);
            result.operation = Operation::ConditionalMove;
            result.condition = extension & 15;
        } else if (extension >= 0x90 && extension <= 0x9f) {
            result.width = 8;
            modOperands();
            result.operation = Operation::SetCondition;
            result.condition = extension & 15;
        } else if (extension == 0xb6 || extension == 0xb7 || extension == 0xbe || extension == 0xbf) {
            result.sourceWidth = (extension & 1) == 0 ? 8 : 16;
            modOperands(result.sourceWidth, result.width);
            std::swap(result.destination, result.source);
            result.operation = extension < 0xbe ? Operation::Move : Operation::MoveSigned;
        } else if (extension == 0xaf) {
            modOperands();
            std::swap(result.destination, result.source);
            result.operation = Operation::Multiply;
        } else if (extension == 0x1f) {
            if (modOperands() != 0) throw std::runtime_error("NOP extension is invalid");
            result.operation = Operation::Nop;
        } else throw std::runtime_error("x86-64 two-byte instruction translation is not implemented");
    } else if (opcode == 0xfe || opcode == 0xff) {
        if (opcode == 0xfe) result.width = 8;
        const auto extension = modOperands();
        result.source = result.destination;
        if (extension == 0 || extension == 1) {
            result.operation = extension == 0 ? Operation::Increment : Operation::Decrement;
            result.source = immediate(1);
        } else if (opcode == 0xff && (extension == 2 || extension == 4 || extension == 6)) {
            if (prefix.word && extension != 6) throw std::runtime_error("16-bit near control transfer translation is not implemented");
            result.operation = extension == 2 ? Operation::Call : extension == 4 ? Operation::Jump : Operation::Push;
            result.width = extension == 6 && prefix.word ? 16 : 64;
        } else throw std::runtime_error("FE/FF extension is not implemented");
    } else if (opcode == 0xc3 || opcode == 0xc2) {
        result.operation = Operation::Return;
        result.source = immediate(opcode == 0xc2 ? reader.Unsigned(2) : 0);
    } else {
        throw std::runtime_error("x86-64 opcode or prefix translation is not implemented");
    }
    if (reader.Position() != length) throw std::runtime_error("x86-64 semantic decoder disagrees with instruction length");
    return result;
}

}
