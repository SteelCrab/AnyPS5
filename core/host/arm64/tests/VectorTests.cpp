#include "Cpu.hpp"
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

using namespace AnyPS5::Arm64;

namespace {

using Vector = std::array<std::byte, 16>;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<typename TAction>
void rejects(TAction action, const char* reason) {
    try { action(); }
    catch (const std::runtime_error& error) {
        if (std::string_view(error.what()).find(reason) != std::string_view::npos) return;
        throw;
    }
    throw std::runtime_error(std::string("Expected failure: ") + reason);
}

struct Fixture {
    GuestMemory memory;
    Cpu cpu;
    CpuState state;
    explicit Fixture(std::vector<std::uint8_t> code) : cpu(memory) {
        memory.Map(0x1000, code.size(), Read | Execute, std::as_bytes(std::span(code)));
        memory.Map(0x8000, 0x1000, Read | Write);
        Reset();
    }
    void Reset() {
        state = {};
        state.rip = 0x1000;
        state.registers[4] = 0x8ff8;
        state.rflags = 0xed7;
    }
    void Run() { cpu.Run(state); }
};

std::uint64_t lane(const Vector& value, unsigned position, unsigned width) {
    std::uint64_t result = 0;
    for (unsigned byte = 0; byte < width / 8; ++byte) result |= std::uint64_t{std::to_integer<std::uint8_t>(value[position * width / 8 + byte])} << (byte * 8);
    return result;
}

void putLane(Vector& value, unsigned position, unsigned width, std::uint64_t input) {
    for (unsigned byte = 0; byte < width / 8; ++byte) value[position * width / 8 + byte] = static_cast<std::byte>(input >> (byte * 8));
}

Vector reference(const Vector& left, const Vector& right, std::uint8_t opcode, unsigned width) {
    Vector result{};
    for (unsigned i = 0; i < 128 / width; ++i) {
        const auto a = lane(left, i, width), b = lane(right, i, width);
        const auto sign = std::uint64_t{1} << (width - 1);
        const auto bits = width == 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << width) - 1;
        std::uint64_t value = 0;
        switch (opcode) {
        case 0xdb: value = a & b; break;
        case 0xdf: value = ~a & b; break;
        case 0xeb: value = a | b; break;
        case 0xef: value = a ^ b; break;
        case 0xfc: case 0xfd: case 0xfe: case 0xd4: value = a + b; break;
        case 0xf8: case 0xf9: case 0xfa: case 0xfb: value = a - b; break;
        case 0x74: case 0x75: case 0x76: value = a == b ? bits : 0; break;
        case 0x64: case 0x65: case 0x66: value = (a ^ sign) > (b ^ sign) ? bits : 0; break;
        case 0xd5: value = a * b; break;
        default: throw std::runtime_error("Unknown vector reference operation");
        }
        putLane(result, i, width, value);
    }
    return result;
}

void testPackedArithmetic() {
    struct Case { std::uint8_t opcode; unsigned width; };
    const Case cases[]{{0xdb, 8}, {0xdf, 8}, {0xeb, 8}, {0xef, 8}, {0xfc, 8}, {0xfd, 16}, {0xfe, 32}, {0xd4, 64},
        {0xf8, 8}, {0xf9, 16}, {0xfa, 32}, {0xfb, 64}, {0x74, 8}, {0x75, 16}, {0x76, 32}, {0x64, 8}, {0x65, 16}, {0x66, 32}, {0xd5, 16}};
    std::uint64_t random = 0x123456789abcdef0;
    for (const auto test : cases) {
        Fixture fixture({0x66, 0x0f, test.opcode, 0xc1, 0xc3});
        for (unsigned iteration = 0; iteration < 100; ++iteration) {
            fixture.Reset();
            for (unsigned byte = 0; byte < 16; ++byte) {
                random ^= random << 13;
                random ^= random >> 7;
                random ^= random << 17;
                fixture.state.xmm[0][byte] = static_cast<std::byte>(random);
                fixture.state.xmm[1][byte] = iteration < 3 ? fixture.state.xmm[0][byte] : static_cast<std::byte>(random >> 8);
            }
            const auto left = fixture.state.xmm[0], right = fixture.state.xmm[1];
            const auto expected = reference(left, right, test.opcode, test.width);
            fixture.Run();
            require(fixture.state.xmm[0] == expected && fixture.state.xmm[1] == right, "Packed SSE2 operation differs from scalar lane reference");
            require(fixture.state.rflags == 0xed7, "Packed SSE2 integer operation changed flags");
        }
    }
    for (const std::uint8_t opcode : {0x54, 0x55, 0x56, 0x57}) for (const bool packedDouble : {false, true}) {
        std::vector<std::uint8_t> code;
        if (packedDouble) code.push_back(0x66);
        code.insert(code.end(), {0x0f, opcode, 0xc1, 0xc3});
        Fixture fixture(code);
        for (unsigned byte = 0; byte < 16; ++byte) {
            fixture.state.xmm[0][byte] = static_cast<std::byte>(byte * 11);
            fixture.state.xmm[1][byte] = static_cast<std::byte>(byte * 13 + 1);
        }
        const std::uint8_t integerOpcodes[]{0xdb, 0xdf, 0xeb, 0xef};
        const auto expected = reference(fixture.state.xmm[0], fixture.state.xmm[1], integerOpcodes[opcode - 0x54], 8);
        fixture.Run();
        require(fixture.state.xmm[0] == expected, "Packed float bitwise operation changed its bit representation");
    }
}

void testShuffleAndExtendedRegisters() {
    for (unsigned immediate = 0; immediate < 256; ++immediate) {
        Fixture fixture({0x66, 0x45, 0x0f, 0x70, 0xc1, static_cast<std::uint8_t>(immediate), 0xc3});
        Vector expected{};
        for (unsigned laneIndex = 0; laneIndex < 4; ++laneIndex) putLane(fixture.state.xmm[9], laneIndex, 32, 0x12345670 + laneIndex);
        for (unsigned laneIndex = 0; laneIndex < 4; ++laneIndex) putLane(expected, laneIndex, 32, 0x12345670 + ((immediate >> (laneIndex * 2)) & 3));
        fixture.Run();
        require(fixture.state.xmm[8] == expected, "PSHUFD immediate or extended XMM register failed");
    }
    Fixture exchange({0x66, 0x45, 0x0f, 0xef, 0xc1, 0xc3});
    exchange.state.xmm[8].fill(std::byte{0x55});
    exchange.state.xmm[9].fill(std::byte{0xaa});
    exchange.Run();
    Vector expected;
    expected.fill(std::byte{0xff});
    require(exchange.state.xmm[8] == expected, "REX extended XMM arithmetic failed");
}

void testVectorMemory() {
    for (const bool aligned : {false, true}) for (const bool integer : {false, true}) {
        std::vector<std::uint8_t> code;
        if (integer) code.push_back(aligned ? 0x66 : 0xf3);
        code.insert(code.end(), {0x44, 0x0f, static_cast<std::uint8_t>(integer ? 0x6f : aligned ? 0x28 : 0x10), 0x03});
        if (integer) code.push_back(aligned ? 0x66 : 0xf3);
        code.insert(code.end(), {0x44, 0x0f, static_cast<std::uint8_t>(integer ? 0x7f : aligned ? 0x29 : 0x11), 0x06, 0xc3});
        Fixture fixture(code);
        std::array<std::byte, 64> data;
        for (unsigned byte = 0; byte < data.size(); ++byte) data[byte] = static_cast<std::byte>(byte * 7);
        fixture.memory.Map(0x2000, data.size(), Read | Write, data);
        const auto offset = aligned ? 0 : 1;
        fixture.state.registers[3] = 0x2000 + offset;
        fixture.state.registers[6] = 0x2020 + offset;
        fixture.Run();
        Vector expected{}, actual{};
        std::memcpy(expected.data(), data.data() + offset, 16);
        require(fixture.memory.TryRead(0x2020 + offset, actual) && actual == expected && fixture.state.xmm[8] == expected, "Aligned/unaligned SSE memory access failed");
    }
    Fixture alignment({0x0f, 0x28, 0x03, 0xc3});
    alignment.memory.Map(0x2000, 32, Read);
    alignment.state.registers[3] = 0x2001;
    alignment.state.xmm[0].fill(std::byte{0x55});
    const auto original = alignment.state.xmm[0];
    rejects([&] { alignment.Run(); }, "alignment fault");
    require(alignment.state.xmm[0] == original && alignment.state.rip == 0x1000, "Faulting aligned vector load changed guest state");
    Fixture unreadable({0xf3, 0x0f, 0x6f, 0x03, 0xc3});
    unreadable.state.registers[3] = 0x2000;
    rejects([&] { unreadable.Run(); }, "read fault");
    Fixture unwritable({0xf3, 0x0f, 0x7f, 0x03, 0xc3});
    unwritable.memory.Map(0x2000, 8, Read | Write);
    unwritable.memory.Map(0x2008, 8, Read);
    unwritable.state.registers[3] = 0x2000;
    unwritable.state.xmm[0].fill(std::byte{0xff});
    rejects([&] { unwritable.Run(); }, "write fault");
    Vector unchanged{};
    require(unwritable.memory.TryRead(0x2000, unchanged) && unchanged == Vector{}, "Faulting vector store partially changed memory");
}

void testScalarVectorTransfer() {
    for (const bool wide : {false, true}) {
        const auto rex = static_cast<std::uint8_t>(wide ? 0x4d : 0x45);
        Fixture fixture({0x66, rex, 0x0f, 0x6e, 0xc8, 0x66, rex, 0x0f, 0x7e, 0xc8, 0xc3});
        fixture.state.registers[8] = 0x1122334455667788;
        fixture.state.xmm[9].fill(std::byte{0xff});
        fixture.Run();
        Vector expected{};
        putLane(expected, 0, wide ? 64 : 32, 0x1122334455667788);
        require(fixture.state.xmm[9] == expected && fixture.state.registers[8] == (wide ? 0x1122334455667788 : 0x55667788), "MOVD/MOVQ scalar transfer or zero extension failed");
        require(fixture.state.rflags == 0xed7, "MOVD/MOVQ changed flags");
    }
    Fixture memory({0x66, 0x0f, 0x6e, 0x03, 0x66, 0x0f, 0x7e, 0x06, 0xc3});
    const std::uint64_t value = 0x1122334455667788;
    memory.memory.Map(0x2000, 8, Read, std::as_bytes(std::span(&value, 1)));
    memory.memory.Map(0x3000, 4, Read | Write);
    memory.state.registers[3] = 0x2000;
    memory.state.registers[6] = 0x3000;
    memory.Run();
    std::uint32_t result = 0;
    require(memory.memory.TryRead(0x3000, std::as_writable_bytes(std::span(&result, 1))) && result == 0x55667788, "Scalar vector transfer accessed past its memory operand");
    Fixture mmx({0x0f, 0x6f, 0xc1});
    rejects([&] { mmx.Run(); }, "MMX");
    Fixture scalarFloat({0xf3, 0x0f, 0x58, 0xc1});
    rejects([&] { scalarFloat.Run(); }, "mandatory prefix");
}

void testScalarVectorMoves() {
    for (const bool load : {true, false}) {
        Fixture quad({static_cast<std::uint8_t>(load ? 0xf3 : 0x66), 0x45, 0x0f, static_cast<std::uint8_t>(load ? 0x7e : 0xd6), 0xc1, 0xc3});
        quad.state.xmm[8].fill(std::byte{0x5a});
        quad.state.xmm[9].fill(std::byte{0xe3});
        Vector expected{};
        std::copy_n(quad.state.xmm[load ? 9 : 8].begin(), 8, expected.begin());
        quad.Run();
        require(quad.state.xmm[load ? 8 : 9] == expected, "XMM MOVQ did not clear the upper 64 bits");
    }
    for (const auto prefix : {std::uint8_t{0xf3}, std::uint8_t{0xf2}}) {
        const unsigned width = prefix == 0xf3 ? 32 : 64;
        for (const auto opcode : {std::uint8_t{0x10}, std::uint8_t{0x11}}) {
            Fixture fixture({prefix, 0x45, 0x0f, opcode, 0xc1, 0xc3});
            fixture.state.xmm[8].fill(std::byte{0x12});
            fixture.state.xmm[9].fill(std::byte{0xf7});
            auto expected = fixture.state.xmm[opcode == 0x10 ? 8 : 9];
            const auto source = fixture.state.xmm[opcode == 0x10 ? 9 : 8];
            std::copy_n(source.begin(), width / 8, expected.begin());
            fixture.Run();
            require(fixture.state.xmm[opcode == 0x10 ? 8 : 9] == expected, "Register MOVSS/MOVSD overwrote preserved upper lanes");
            require(fixture.state.xmm[opcode == 0x10 ? 9 : 8] == source && fixture.state.rflags == 0xed7, "Register MOVSS/MOVSD changed its source or flags");
        }
        Fixture memory({prefix, 0x0f, 0x10, 0x03, prefix, 0x0f, 0x11, 0x06, 0xc3});
        const std::uint64_t bits = 0x7ff0000081234567;
        memory.memory.Map(0x2001, width / 8, Read, std::as_bytes(std::span(&bits, 1)).first(width / 8));
        memory.memory.Map(0x3003, width / 8, Read | Write);
        memory.state.registers[3] = 0x2001;
        memory.state.registers[6] = 0x3003;
        memory.state.xmm[0].fill(std::byte{0xff});
        memory.Run();
        Vector expected{};
        putLane(expected, 0, width, bits);
        require(memory.state.xmm[0] == expected, "Memory MOVSS/MOVSD did not clear upper lanes or preserve NaN bits");
        std::uint64_t output = 0;
        require(memory.memory.TryRead(0x3003, std::as_writable_bytes(std::span(&output, 1)).first(width / 8)) && output == lane(expected, 0, width), "Scalar vector move accessed beyond its memory width");
        Fixture fault({prefix, 0x0f, 0x10, 0x03, 0xc3});
        fault.state.registers[3] = 0x2000;
        fault.state.xmm[0].fill(std::byte{0x41});
        const auto original = fault.state.xmm[0];
        rejects([&] { fault.Run(); }, "read fault");
        require(fault.state.xmm[0] == original && fault.state.rip == 0x1000, "Faulting MOVSS/MOVSD modified guest state");
    }
}

}

int main() {
    try {
        testPackedArithmetic();
        testShuffleAndExtendedRegisters();
        testVectorMemory();
        testScalarVectorTransfer();
        testScalarVectorMoves();
        std::puts("ARM64 NEON execution of SSE/SSE2 packed integers, bitwise operations, shuffles and vector memory passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
