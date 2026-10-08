#include "X64Decoder.hpp"
#include <codegen/x86/X64InstructionDecoder.hpp>
#include <stdexcept>
#include <utility>

namespace AnyPS5::Arm64 {

bool Instruction::TerminatesBlock() const {
    return operation == Operation::Jump || operation == Operation::ConditionalJump ||
           operation == Operation::Call || operation == Operation::Return;
}

namespace {

Operand reg(std::uint8_t index) {
    Operand operand;
    operand.kind = OperandKind::Register;
    operand.reg = index;
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

Operand rm(Reader& reader, std::uint8_t modrm, std::uint8_t rex) {
    const auto mode = modrm >> 6;
    const auto low = modrm & 7;
    if (mode == 3) return reg(low | ((rex & 1) << 3));
    Operand operand;
    operand.kind = OperandKind::Memory;
    if (low == 4) {
        const auto sib = reader.Byte();
        const auto base = sib & 7;
        const auto index = (sib >> 3) & 7;
        operand.scale = sib >> 6;
        if (index != 4 || (rex & 2) != 0) operand.index = index | ((rex & 2) << 2);
        if (base == 5 && mode == 0) operand.displacement = reader.Signed(4);
        else operand.base = base | ((rex & 1) << 3);
    } else if (low == 5 && mode == 0) {
        operand.ripRelative = true;
        operand.displacement = reader.Signed(4);
    } else {
        operand.base = low | ((rex & 1) << 3);
    }
    if (mode == 1) operand.displacement = reader.Signed(1);
    if (mode == 2) operand.displacement = reader.Signed(4);
    return operand;
}

Operation group(std::uint8_t opcode) {
    switch (opcode) {
    case 0: return Operation::Add;
    case 1: return Operation::Or;
    case 4: return Operation::And;
    case 5: return Operation::Subtract;
    case 6: return Operation::Xor;
    case 7: return Operation::Compare;
    default: throw std::runtime_error("x86-64 ADC/SBB instruction translation is not implemented");
    }
}

}

Instruction Decode(std::span<const std::uint8_t> code, std::uint64_t address) {
    const auto length = Codegen::X64InstructionDecoder{}.Decode(code.data(), code.size());
    if (length > 15) throw std::runtime_error("x86-64 instruction exceeds 15 bytes");
    Reader reader(code.first(length));
    auto opcode = reader.Byte();
    std::uint8_t rex = 0;
    if (opcode >= 0x40 && opcode <= 0x4f) {
        rex = opcode;
        opcode = reader.Byte();
    }
    Instruction result{};
    result.width = (rex & 8) != 0 ? 64 : 32;
    result.address = address;
    result.size = static_cast<std::uint8_t>(length);
    const auto modOperands = [&] {
        const auto modrm = reader.Byte();
        result.destination = rm(reader, modrm, rex);
        result.source = reg(((modrm >> 3) & 7) | ((rex & 4) << 1));
        return static_cast<std::uint8_t>((modrm >> 3) & 7);
    };
    if (opcode >= 0xb8 && opcode <= 0xbf) {
        result.operation = Operation::Move;
        result.destination = reg((opcode & 7) | ((rex & 1) << 3));
        result.source = immediate(reader.Unsigned(result.width / 8));
    } else if (opcode == 0x89 || opcode == 0x8b || opcode == 0x8d) {
        modOperands();
        result.operation = opcode == 0x8d ? Operation::Lea : Operation::Move;
        if (opcode != 0x89) std::swap(result.destination, result.source);
        if (opcode == 0x8d && result.source.kind != OperandKind::Memory) throw std::runtime_error("x86-64 LEA requires a memory address operand");
    } else if (opcode == 0x87) {
        modOperands();
        if (result.destination.kind != OperandKind::Register) throw std::runtime_error("Atomic memory XCHG translation is not implemented");
        result.operation = Operation::Exchange;
    } else if (opcode >= 0x90 && opcode <= 0x97) {
        result.operation = opcode == 0x90 && (rex & 1) == 0 ? Operation::Nop : Operation::Exchange;
        result.destination = reg(0);
        result.source = reg((opcode & 7) | ((rex & 1) << 3));
    } else if (opcode == 0xc7) {
        if (modOperands() != 0) throw std::runtime_error("x86-64 C7 extension is not implemented");
        result.operation = Operation::Move;
        result.source = immediate(static_cast<std::uint64_t>(reader.Signed(4)));
    } else if (opcode == 0x81 || opcode == 0x83) {
        result.operation = group(modOperands());
        result.source = immediate(static_cast<std::uint64_t>(reader.Signed(opcode == 0x83 ? 1 : 4)));
    } else if ((opcode <= 0x3b && (opcode & 7) == 1) || (opcode <= 0x3b && (opcode & 7) == 3) || opcode == 0x85) {
        modOperands();
        result.operation = opcode == 0x85 ? Operation::Test : group(opcode >> 3);
        if (opcode != 0x85 && (opcode & 2) != 0) std::swap(result.destination, result.source);
    } else if (opcode <= 0x3d && (opcode & 7) == 5) {
        result.operation = group(opcode >> 3);
        result.destination = reg(0);
        result.source = immediate(static_cast<std::uint64_t>(reader.Signed(4)));
    } else if (opcode >= 0x50 && opcode <= 0x5f) {
        result.width = 64;
        result.operation = opcode < 0x58 ? Operation::Push : Operation::Pop;
        result.destination = reg((opcode & 7) | ((rex & 1) << 3));
        result.source = result.destination;
    } else if (opcode == 0x68 || opcode == 0x6a) {
        result.operation = Operation::Push;
        result.width = 64;
        result.source = immediate(static_cast<std::uint64_t>(reader.Signed(opcode == 0x6a ? 1 : 4)));
    } else if (opcode == 0xe8 || opcode == 0xe9 || opcode == 0xeb) {
        result.operation = opcode == 0xe8 ? Operation::Call : Operation::Jump;
        result.source = immediate(address + length + static_cast<std::uint64_t>(reader.Signed(opcode == 0xeb ? 1 : 4)));
    } else if (opcode >= 0x70 && opcode <= 0x7f) {
        result.operation = Operation::ConditionalJump;
        result.condition = opcode & 15;
        result.source = immediate(address + length + static_cast<std::uint64_t>(reader.Signed(1)));
    } else if (opcode == 0x0f) {
        const auto extension = reader.Byte();
        if (extension < 0x80 || extension > 0x8f) throw std::runtime_error("x86-64 two-byte instruction translation is not implemented");
        result.operation = Operation::ConditionalJump;
        result.condition = extension & 15;
        result.source = immediate(address + length + static_cast<std::uint64_t>(reader.Signed(4)));
    } else if (opcode == 0xff) {
        const auto extension = modOperands();
        result.source = result.destination;
        if (extension == 0 || extension == 1) {
            result.operation = extension == 0 ? Operation::Increment : Operation::Decrement;
            result.source = immediate(1);
        } else if (extension == 2 || extension == 4 || extension == 6) {
            result.operation = extension == 2 ? Operation::Call : extension == 4 ? Operation::Jump : Operation::Push;
            result.width = 64;
        } else throw std::runtime_error("x86-64 FF extension is not implemented");
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
