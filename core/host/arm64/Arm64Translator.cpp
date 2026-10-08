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

void updateFlags(CpuState* state, std::uint64_t left, std::uint64_t right, std::uint64_t result, std::uint64_t mode) noexcept {
    const bool wide = (mode & 0x100u) != 0;
    const auto mask = wide ? ~std::uint64_t{0} : 0xffffffffu;
    const auto sign = wide ? std::uint64_t{1} << 63 : std::uint64_t{1} << 31;
    left &= mask;
    right &= mask;
    result &= mask;
    const auto operation = static_cast<Operation>(mode & 0xffu);
    auto flags = state->rflags & ~(Carry | Parity | Zero | Sign | Overflow);
    if (result == 0) flags |= Zero;
    if ((result & sign) != 0) flags |= Sign;
    if (std::popcount(static_cast<std::uint8_t>(result)) % 2 == 0) flags |= Parity;
    const bool addition = operation == Operation::Add || operation == Operation::Increment;
    const bool subtraction = operation == Operation::Subtract || operation == Operation::Compare || operation == Operation::Decrement;
    if (addition || subtraction) {
        flags &= ~Auxiliary;
        if (((left ^ right ^ result) & 16) != 0) flags |= Auxiliary;
        if (addition ? result < left : left < right) flags |= Carry;
        if ((addition ? ~(left ^ right) & (left ^ result) : (left ^ right) & (left ^ result)) & sign) flags |= Overflow;
    }
    if (operation == Operation::Increment || operation == Operation::Decrement) flags = (flags & ~Carry) | (state->rflags & Carry);
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
        word((width == 64 ? 0xf9400000u : 0xb9400000u) | (static_cast<std::uint32_t>(offset / (width / 8)) << 10) | (19 << 5) | destination);
    }
    void Store(std::uint8_t source, std::size_t offset) { word(0xf9000000u | (static_cast<std::uint32_t>(offset / 8) << 10) | (19 << 5) | source); }
    void Binary(std::uint32_t opcode, std::uint8_t destination, std::uint8_t left, std::uint8_t right, std::uint8_t width = 64, std::uint8_t shift = 0) {
        word((opcode | (width == 64 ? 0x80000000u : 0)) | (right << 16) | (shift << 10) | (left << 5) | destination);
    }
    template<typename TFunction>
    void Call(TFunction function) {
        Move(0, 19);
        Constant(16, reinterpret_cast<std::uintptr_t>(function));
        word(0xd63f0200u);
    }
    void Address(const Operand& operand, std::uint8_t destination, std::uint64_t next) {
        if (operand.ripRelative) Constant(destination, next);
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
    }
    void Read(const Operand& operand, std::uint8_t destination, std::uint8_t width, std::uint64_t next) {
        switch (operand.kind) {
        case OperandKind::Immediate:
            Constant(destination, width == 32 ? operand.immediate & 0xffffffffu : operand.immediate);
            break;
        case OperandKind::Register: Load(destination, operand.reg * 8, width); break;
        case OperandKind::Memory:
            Address(operand, 1, next);
            Constant(2, width / 8);
            Call(readMemory);
            faultGuard();
            Move(destination, 0);
            break;
        }
    }
    void Write(const Operand& operand, std::uint8_t source, std::uint8_t width, std::uint64_t next) {
        if (operand.kind == OperandKind::Register) Store(source, operand.reg * 8);
        else if (operand.kind == OperandKind::Memory) {
            Address(operand, 1, next);
            Move(2, source);
            Constant(3, width / 8);
            Call(writeMemory);
            faultGuard();
        } else throw std::runtime_error("An immediate cannot be a guest instruction destination");
    }
    void Push(std::uint8_t source) {
        Load(23, 4 * 8);
        Constant(17, 8);
        Binary(0x4b000000u, 23, 23, 17);
        Move(1, 23);
        Move(2, source);
        Constant(3, 8);
        Call(writeMemory);
        faultGuard();
        Store(23, 4 * 8);
    }
    void Pop(std::uint8_t destination, std::uint64_t extra = 0) {
        Load(1, 4 * 8);
        Constant(2, 8);
        Call(readMemory);
        faultGuard();
        Move(destination, 0);
        Load(23, 4 * 8);
        Constant(17, 8 + extra);
        Binary(0x0b000000u, 23, 23, 17);
        Store(23, 4 * 8);
    }
    void InstructionCode(const Instruction& instruction) {
        const auto next = instruction.address + instruction.size;
        Constant(9, instruction.address);
        Store(9, offsetof(CpuState, rip));
        switch (instruction.operation) {
        case Operation::Move:
            Read(instruction.source, 20, instruction.width, next);
            Write(instruction.destination, 20, instruction.width, next);
            break;
        case Operation::Exchange:
            Read(instruction.destination, 20, instruction.width, next);
            Read(instruction.source, 21, instruction.width, next);
            Write(instruction.destination, 21, instruction.width, next);
            Write(instruction.source, 20, instruction.width, next);
            break;
        case Operation::Lea:
            Address(instruction.source, 20, next);
            if (instruction.width == 32) word(0x2a1403f4u);
            Write(instruction.destination, 20, instruction.width, next);
            break;
        case Operation::Add:
        case Operation::Subtract:
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
            case Operation::Increment: opcode = 0x0b000000u; break;
            case Operation::Subtract:
            case Operation::Compare:
            case Operation::Decrement: opcode = 0x4b000000u; break;
            case Operation::And:
            case Operation::Test: opcode = 0x0a000000u; break;
            case Operation::Or: opcode = 0x2a000000u; break;
            case Operation::Xor: opcode = 0x4a000000u; break;
            default: break;
            }
            Binary(opcode, 22, 20, 21, instruction.width);
            if (instruction.operation != Operation::Compare && instruction.operation != Operation::Test) {
                Write(instruction.destination, 22, instruction.width, next);
            }
            Move(1, 20);
            Move(2, 21);
            Move(3, 22);
            Constant(4, static_cast<std::uint64_t>(instruction.operation) | (instruction.width == 64 ? 0x100u : 0));
            Call(updateFlags);
            break;
        }
        case Operation::Push:
            Read(instruction.source, 20, 64, next);
            Push(20);
            break;
        case Operation::Pop:
            Pop(20);
            Write(instruction.destination, 20, 64, next);
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
        } catch (const std::exception& error) {
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
