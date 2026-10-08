#include "Cpu.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace AnyPS5::Arm64;

namespace {

constexpr std::uint64_t FlagMask = 0x8d5;

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

std::uint64_t mask(unsigned width) { return width == 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << width) - 1; }

std::uint64_t registerResult(std::uint64_t original, std::uint64_t result, unsigned width) {
    return (width < 32 ? original & ~mask(width) : 0) | (result & mask(width));
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
    void Run() { cpu.Run(state, 10000); }
};

std::vector<std::uint8_t> prefix(unsigned width) {
    if (width == 16) return {0x66};
    if (width == 64) return {0x48};
    return {};
}

struct ArithmeticResult { std::uint64_t value, flags; };

ArithmeticResult addBits(std::uint64_t left, std::uint64_t right, unsigned width, bool subtract, bool carry) {
    ArithmeticResult result{};
    bool nextCarry = subtract ? !carry : carry;
    bool auxiliary = false, overflow = false;
    for (unsigned bit = 0; bit < width; ++bit) {
        const bool a = ((left >> bit) & 1) != 0;
        const bool b = (((right >> bit) & 1) != 0) != subtract;
        const bool incoming = nextCarry;
        if ((a != b) != incoming) result.value |= std::uint64_t{1} << bit;
        nextCarry = (a && b) || (a && incoming) || (b && incoming);
        if (bit == 3) auxiliary = nextCarry != subtract;
        if (bit == width - 1) overflow = incoming != nextCarry;
    }
    if (nextCarry != subtract) result.flags |= 1;
    if (std::popcount(static_cast<std::uint8_t>(result.value)) % 2 == 0) result.flags |= 4;
    if (auxiliary) result.flags |= 16;
    if (result.value == 0) result.flags |= 64;
    if ((result.value >> (width - 1)) != 0) result.flags |= 128;
    if (overflow) result.flags |= 2048;
    return result;
}

void testArithmeticWidths() {
    for (const unsigned width : {8, 16, 32, 64}) {
        const auto sign = std::uint64_t{1} << (width - 1);
        std::vector<std::uint64_t> values{0, 1, mask(width), sign, sign - 1, 15, 16, mask(width) - 15, 0x123456789abcdef0 & mask(width)};
        if (width == 8) {
            values.clear();
            for (unsigned value = 0; value < 256; ++value) values.push_back(value);
        }
        for (const std::uint8_t opcode : {0x00, 0x10, 0x18, 0x28}) {
            auto code = prefix(width);
            code.insert(code.end(), {static_cast<std::uint8_t>(opcode + (width != 8)), 0xc8, 0xc3});
            Fixture fixture(code);
            for (const auto left : values) for (const auto right : values) for (const bool carry : {false, true}) {
                fixture.Reset();
                const auto original = (0xfedcba9876543200u & ~mask(width)) | left;
                fixture.state.registers[0] = original;
                fixture.state.registers[1] = right;
                fixture.state.rflags = 0xed6 | carry;
                const auto expected = addBits(original, right, width, opcode == 0x18 || opcode == 0x28,
                                              (opcode == 0x10 || opcode == 0x18) && carry);
                fixture.Run();
                require(fixture.state.registers[0] == registerResult(original, expected.value, width), "Integer arithmetic or partial register write failed");
                require((fixture.state.rflags & FlagMask) == expected.flags, "Integer carry, borrow or overflow differs from full-adder reference");
                require((fixture.state.rflags & ~FlagMask) == (0xed6 & ~FlagMask), "Integer arithmetic changed unrelated flags");
            }
            require(fixture.cpu.CompiledBlocks() == 1, "Integer cases did not reuse their native block");
        }
    }
}

void testPartialRegistersAndExtensions() {
    Fixture high({0xb4, 0x81, 0x88, 0xe3, 0x86, 0xc4, 0xc3});
    high.state.registers[0] = 0x1122334455667788;
    high.state.registers[3] = 0xff00;
    high.Run();
    require(high.state.registers[0] == 0x1122334455668881 && high.state.registers[3] == 0xff81, "High-byte registers or byte exchange failed");
    Fixture rex({0x40, 0x88, 0xe0, 0x41, 0x88, 0xe4, 0xc3});
    rex.state.registers[0] = 0x1234;
    rex.state.registers[12] = 0xabcd;
    rex.Run();
    require(rex.state.registers[0] == 0x12f8 && rex.state.registers[12] == 0xabf8, "REX byte registers aliased AH");
    for (const unsigned width : {16, 32, 64}) for (const unsigned sourceWidth : {8, 16}) for (const bool signedValue : {false, true}) {
        auto code = prefix(width);
        code.insert(code.end(), {0x0f, static_cast<std::uint8_t>((signedValue ? 0xbe : 0xb6) + (sourceWidth == 16)), 0xc1, 0xc3});
        Fixture fixture(code);
        fixture.state.registers[0] = 0x1122334455667788;
        fixture.state.registers[1] = sourceWidth == 8 ? 0xff81 : 0xff8001;
        const auto input = fixture.state.registers[1] & mask(sourceWidth);
        const auto expected = signedValue ? input | ~mask(sourceWidth) : input;
        fixture.Run();
        require(fixture.state.registers[0] == registerResult(0x1122334455667788, expected, width), "MOVSX/MOVZX result or upper register preservation failed");
        require(fixture.state.rflags == 0xed7, "MOVSX/MOVZX changed flags");
    }
    for (const unsigned width : {16, 32, 64}) {
        auto code = prefix(width);
        code.push_back(0x98);
        const auto nextPrefix = prefix(width);
        code.insert(code.end(), nextPrefix.begin(), nextPrefix.end());
        code.insert(code.end(), {0x99, 0xc3});
        Fixture fixture(code);
        const auto original = 0xaabbccdd00000000u | (std::uint64_t{1} << (width / 2 - 1));
        fixture.state.registers[0] = original;
        fixture.state.registers[2] = 0x123456789abcdef0;
        fixture.Run();
        const auto signExtended = (original & mask(width / 2)) | ~mask(width / 2);
        require(fixture.state.registers[0] == registerResult(original, signExtended, width), "CBW/CWDE/CDQE failed");
        require(fixture.state.registers[2] == registerResult(0x123456789abcdef0, mask(width), width), "CWD/CDQ/CQO failed");
    }
}

void testMemoryWidthsAndSegments() {
    Fixture fixture({0x88, 0x03, 0x66, 0x89, 0x4b, 1, 0x64, 0x67, 0x8b, 0x13, 0x65, 0x8b, 0x73, 8,
                     0x64, 0x67, 0x48, 0x8d, 0x3b, 0xc3});
    fixture.memory.Map(0x2000, 32, Read | Write);
    const std::array<std::uint32_t, 8> original{0xdeadbeef, 0x12345678, 0x87654321, 0, 0, 0, 0, 0};
    require(fixture.memory.TryWrite(0x2000, std::as_bytes(std::span(original))), "Cannot initialize memory width fixture");
    fixture.state.registers[0] = 0x123481;
    fixture.state.registers[1] = 0xabcd;
    fixture.state.registers[3] = 0x2000;
    fixture.state.fsBase = 4;
    fixture.state.gsBase = 0;
    fixture.Run();
    std::uint32_t stored = 0;
    require(fixture.memory.TryRead(0x2000, std::as_writable_bytes(std::span(&stored, 1))) && stored == 0xdeabcd81, "Byte/word stores changed adjacent memory");
    require(fixture.state.registers[2] == 0x12345678 && fixture.state.registers[6] == 0x87654321, "FS/GS memory base failed");
    require(fixture.state.registers[7] == 0x2000, "LEA incorrectly added a segment base");
    Fixture wrap({0x67, 0x8b, 0x03, 0xc3});
    wrap.memory.Map(0x2000, 4, Read, std::as_bytes(std::span(original)).first(4));
    wrap.state.registers[3] = 0x123400002000;
    wrap.Run();
    require(wrap.state.registers[0] == original[0], "Address-size prefix did not truncate the effective address");
    Fixture relative({0x67, 0x8b, 0x05, 0xf9, 0x0f, 0, 0, 0xc3});
    relative.memory.Map(0x2000, 4, Read, std::as_bytes(std::span(original)).first(4));
    relative.Run();
    require(relative.state.registers[0] == original[0], "EIP-relative addressing failed");
}

void testBitTest() {
    for (const unsigned width : {16u, 32u, 64u}) {
        for (const bool immediate : {false, true}) {
            auto code = prefix(width);
            code.insert(code.end(), immediate ? std::initializer_list<std::uint8_t>{0x0f, 0xba, 0xe0, 0xff, 0xc3} : std::initializer_list<std::uint8_t>{0x0f, 0xa3, 0xc8, 0xc3});
            Fixture fixture(code);
            for (const auto index : {0ull, 1ull, 15ull, 16ull, 31ull, 32ull, 63ull, 64ull, ~0ull}) {
                fixture.Reset();
                fixture.state.registers[0] = 0xa5f033ccdeadbeefu;
                fixture.state.registers[1] = index;
                const auto before = fixture.state;
                const auto selected = (immediate ? 255 : index) & (width - 1);
                const auto expected = (before.registers[0] >> selected) & 1;
                fixture.Run();
                require(fixture.state.registers[0] == before.registers[0] && fixture.state.registers[1] == before.registers[1], "BT modified a source register");
                require(fixture.state.rflags == ((before.rflags & ~1ull) | expected), "BT changed ZF/undefined flags or selected the wrong bit");
            }
        }
        auto code = prefix(width);
        code.insert(code.end(), {0x0f, 0xa3, 0x08, 0xc3});
        Fixture fixture(code);
        std::array<std::uint8_t, 512> bytes{};
        for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<std::uint8_t>(i * 73 + 41);
        fixture.memory.Map(0x4000, bytes.size(), Read, std::as_bytes(std::span(bytes)));
        for (const auto index : {-65, -64, -33, -32, -17, -16, -1, 0, 17, 31, 32, 63, 64, 127}) {
            fixture.Reset();
            fixture.state.registers[0] = 0x4080;
            fixture.state.registers[1] = static_cast<std::uint64_t>(index);
            const auto expected = (bytes[128 + (index + 128) / 8 - 16] >> (index & 7)) & 1;
            const auto flags = fixture.state.rflags;
            fixture.Run();
            require(fixture.state.rflags == ((flags & ~1ull) | expected), "BT did not select a signed memory bit offset");
        }
        std::array<std::uint8_t, 512> after{};
        require(fixture.memory.TryRead(0x4000, std::as_writable_bytes(std::span(after))) && after == bytes, "BT changed read-only memory");
        auto immediateCode = prefix(width);
        immediateCode.insert(immediateCode.end(), {0x0f, 0xba, 0x20, 0xff, 0xc3});
        Fixture immediateMemory(immediateCode);
        const auto highest = 1ull << (width - 1);
        immediateMemory.memory.Map(0x5000, width / 8, Read, std::as_bytes(std::span(&highest, 1)).first(width / 8));
        immediateMemory.state.registers[0] = 0x5000;
        immediateMemory.Run();
        require((immediateMemory.state.rflags & 1) != 0, "BT memory immediate did not ignore high index bits");
    }
    Fixture fault({0x0f, 0xa3, 0x08, 0xc3});
    const std::array<std::uint8_t, 1> byte{1};
    fault.memory.Map(0x5000, 1, Read, std::as_bytes(std::span(byte)));
    fault.state.registers[0] = 0x5000;
    fault.state.rflags &= ~1ull;
    const auto flags = fault.state.rflags;
    rejects([&] { fault.Run(); }, "memory read fault");
    require(fault.state.rflags == flags, "BT memory fault changed flags");
    Fixture extended({0x4d, 0x0f, 0xa3, 0xc8, 0xc3});
    extended.state.registers[8] = 1ull << 63;
    extended.state.registers[9] = 63;
    extended.Run();
    require((extended.state.rflags & 1) != 0 && extended.state.registers[8] == (1ull << 63), "BT ignored an extended register");
    Fixture segmented({0x64, 0x67, 0x0f, 0xa3, 0x08, 0xc3});
    const std::array<std::uint32_t, 1> bit{0x80000000u};
    segmented.memory.Map(0x6000ull + 0xfffffffcu, 4, Read, std::as_bytes(std::span(bit)));
    segmented.state.fsBase = 0x6000;
    segmented.state.registers[0] = 0x100000000u;
    segmented.state.registers[1] = UINT32_MAX;
    segmented.Run();
    require((segmented.state.rflags & 1) != 0, "BT did not wrap its 32-bit address before adding FS base");
}

void testShiftAndRotate() {
    for (const unsigned width : {8, 16, 32, 64}) for (const std::uint8_t extension : {0, 1, 4, 5, 7}) {
        auto code = prefix(width);
        code.insert(code.end(), {static_cast<std::uint8_t>(width == 8 ? 0xd2 : 0xd3), static_cast<std::uint8_t>(0xc0 | (extension << 3)), 0xc3});
        Fixture fixture(code);
        for (const auto original : {std::uint64_t{0}, std::uint64_t{1}, mask(width), std::uint64_t{1} << (width - 1), std::uint64_t{0xa13579bdf02468ce}}) {
            for (const unsigned inputCount : {0, 1, 7, 8, 15, 16, 31, 32, 63, 64, 255}) {
                fixture.Reset();
                fixture.state.registers[0] = original;
                fixture.state.registers[1] = inputCount;
                auto expected = original & mask(width);
                bool carry = true;
                const auto count = inputCount & (width == 64 ? 63 : 31);
                for (unsigned step = 0; step < count; ++step) {
                    const bool high = (expected & (std::uint64_t{1} << (width - 1))) != 0;
                    const bool low = (expected & 1) != 0;
                    if (extension == 0 || extension == 4) {
                        carry = high;
                        expected = ((expected << 1) | (extension == 0 && high)) & mask(width);
                    } else {
                        carry = low;
                        expected = (expected >> 1) | ((extension == 1 && low) || (extension == 7 && high) ? std::uint64_t{1} << (width - 1) : 0);
                    }
                }
                fixture.Run();
                require(fixture.state.registers[0] == registerResult(original, expected, width), "Shift/rotate differs from one-bit reference");
                const auto flags = fixture.state.rflags;
                if (count == 0) require(flags == 0xed7, "Zero-count shift/rotate changed flags");
                else {
                    if (extension < 2 || count < width || extension == 7) require((flags & 1) == carry, "Shift/rotate carry failed");
                    if (extension >= 4) {
                        require(((flags & 64) != 0) == (expected == 0), "Shift zero flag failed");
                        require(((flags & 128) != 0) == ((expected >> (width - 1)) != 0), "Shift sign flag failed");
                        require(((flags & 4) != 0) == (std::popcount(static_cast<std::uint8_t>(expected)) % 2 == 0), "Shift parity flag failed");
                    } else require((flags & 0xd4) == (0xed7 & 0xd4), "Rotate changed parity, auxiliary, zero or sign flags");
                    if (count == 1) {
                        const bool high = (expected >> (width - 1)) != 0;
                        const bool overflow = extension == 0 || extension == 4 ? high != carry : extension == 1 ? high != ((expected >> (width - 2)) & 1) : extension == 5 ? ((original >> (width - 1)) & 1) != 0 : false;
                        require(((flags & 2048) != 0) == overflow, "One-bit shift/rotate overflow failed");
                    }
                    require((flags & ~FlagMask) == (0xed7 & ~FlagMask), "Shift/rotate changed unrelated flags");
                }
            }
        }
    }
}

void testMultiply() {
    for (const unsigned width : {16, 32, 64}) {
        const auto sign = std::uint64_t{1} << (width - 1);
        struct Case { std::uint64_t left, right, result; bool overflow; };
        const std::array<Case, 7> cases{{{0, sign, 0, false}, {7, 6, 42, false}, {mask(width), 7, mask(width) - 6, false},
            {sign, 1, sign, false}, {sign, mask(width), sign, true}, {sign - 1, 2, mask(width) - 1, true}, {sign / 2, 2, sign, true}}};
        auto code = prefix(width);
        code.insert(code.end(), {0x0f, 0xaf, 0xc1, 0xc3});
        Fixture fixture(code);
        for (const auto& test : cases) {
            fixture.Reset();
            fixture.state.registers[0] = test.left;
            fixture.state.registers[1] = test.right;
            fixture.Run();
            require(fixture.state.registers[0] == registerResult(test.left, test.result, width), "IMUL product failed");
            require((fixture.state.rflags & 0x801) == (test.overflow ? 0x801 : 0), "IMUL overflow/carry failed");
        }
        code = prefix(width);
        code.insert(code.end(), {0x6b, 0xc1, 0xf9, 0xc3});
        Fixture immediate(code);
        immediate.state.registers[1] = 6;
        immediate.Run();
        require(immediate.state.registers[0] == registerResult(0, 0 - std::uint64_t{42}, width), "IMUL immediate was not sign extended");
    }
}

void testConditionalWrites() {
    const std::array<std::uint64_t, 6> profiles{2, 3, 0x46, 0x82, 0x802, 0x886};
    for (const unsigned width : {16, 32, 64}) for (std::uint8_t condition = 0; condition < 16; ++condition) {
        auto code = prefix(width);
        code.insert(code.end(), {0x0f, static_cast<std::uint8_t>(0x40 | condition), 0xc1, 0x0f, static_cast<std::uint8_t>(0x90 | condition), 0xc2, 0xc3});
        Fixture fixture(code);
        for (const auto flags : profiles) {
            fixture.Reset();
            fixture.state.rflags = flags;
            fixture.state.registers[0] = 0x1122334455667788;
            fixture.state.registers[1] = 0x8877665544332211;
            fixture.state.registers[2] = 0xff00;
            const bool c = (flags & 1) != 0, p = (flags & 4) != 0, z = (flags & 64) != 0, s = (flags & 128) != 0, o = (flags & 2048) != 0;
            const std::array<bool, 16> conditions{o, !o, c, !c, z, !z, c || z, !c && !z, s, !s, p, !p, s != o, s == o, z || s != o, !z && s == o};
            fixture.Run();
            require(fixture.state.registers[0] == registerResult(0x1122334455667788, conditions[condition] ? 0x8877665544332211 : 0x1122334455667788, width), "CMOV condition or register width failed");
            require(fixture.state.registers[2] == 0xff00u + conditions[condition], "SETcc did not preserve the rest of its register");
            require(fixture.state.rflags == flags, "CMOV/SETcc changed flags");
        }
    }
    Fixture fault({0x0f, 0x44, 0x03, 0xc3});
    fault.state.rflags = 2;
    fault.state.registers[0] = 0xffff112233445566;
    fault.state.registers[3] = 0x2000;
    rejects([&] { fault.Run(); }, "read fault");
    require(fault.state.registers[0] == 0xffff112233445566 && fault.state.rip == 0x1000, "Untaken CMOV did not fault before register write");
}

void testStackAndUnary() {
    Fixture narrow({0x66, 0x50, 0x66, 0x59, 0xc3});
    narrow.state.registers[0] = 0x1234abcd;
    narrow.state.registers[1] = 0x1122334455667788;
    narrow.Run();
    require(narrow.state.registers[1] == 0x112233445566abcd && narrow.state.registers[4] == 0x9000, "16-bit PUSH/POP failed");
    Fixture pop({0x8f, 0x04, 0x24, 0xe9, 0xf8, 0xef, 0xff, 0xff});
    pop.state.registers[4] = 0x8fe0;
    const std::uint64_t value = 0x123456789abcdef0;
    require(pop.memory.TryWrite(0x8fe0, std::as_bytes(std::span(&value, 1))), "Cannot initialize POP memory fixture");
    pop.Run();
    std::uint64_t stored = 0;
    require(pop.memory.TryRead(0x8fe8, std::as_writable_bytes(std::span(&stored, 1))) && stored == value && pop.state.registers[4] == 0x8fe8, "POP memory did not use the advanced stack address");
    Fixture fault({0x8f, 0x04, 0x24, 0xc3});
    rejects([&] { fault.Run(); }, "write fault");
    require(fault.state.registers[4] == 0x8ff8 && fault.state.rip == 0x1000, "Faulting POP memory committed the stack pointer");
    for (const unsigned width : {8, 16, 32, 64}) {
        auto code = prefix(width);
        code.insert(code.end(), {static_cast<std::uint8_t>(width == 8 ? 0xf6 : 0xf7), 0xd8});
        const auto nextPrefix = prefix(width);
        code.insert(code.end(), nextPrefix.begin(), nextPrefix.end());
        code.insert(code.end(), {static_cast<std::uint8_t>(width == 8 ? 0xf6 : 0xf7), 0xd0, 0xc3});
        Fixture fixture(code);
        fixture.state.registers[0] = 0x1122334455667788;
        const auto expected = addBits(0, fixture.state.registers[0], width, true, false);
        fixture.Run();
        require(fixture.state.registers[0] == registerResult(0x1122334455667788, ~expected.value, width) && (fixture.state.rflags & FlagMask) == expected.flags, "NEG/NOT result or flags failed");
    }
    Fixture unsupported({0x66, 0xc3});
    rejects([&] { unsupported.Run(); }, "16-bit near control transfer");
}

void wideArithmetic() {
    std::array<std::unique_ptr<Fixture>, 16> fixtures;
    const std::array<unsigned, 4> widths{8, 16, 32, 64};
    for (std::size_t index = 0; index < fixtures.size(); ++index) {
        const auto width = widths[index / 4];
        auto code = prefix(width);
        code.insert(code.end(), {static_cast<std::uint8_t>(width == 8 ? 0xf6 : 0xf7), static_cast<std::uint8_t>(0xe1 | ((index % 4) << 3)), 0xe9});
        const auto displacement = 0 - (0x1000 + code.size() + 4);
        for (unsigned byte = 0; byte < 4; ++byte) code.push_back(static_cast<std::uint8_t>(displacement >> (byte * 8)));
        fixtures[index] = std::make_unique<Fixture>(code);
    }
    unsigned width = 0, operation = 0;
    std::uint64_t low = 0, high = 0, operand = 0;
    while (std::cin >> std::dec >> width >> operation >> std::hex >> low >> high >> operand) {
        const auto position = std::find(widths.begin(), widths.end(), width);
        require(position != widths.end() && operation < 4, "Invalid wide arithmetic test request");
        auto& fixture = *fixtures[static_cast<std::size_t>(position - widths.begin()) * 4 + operation];
        fixture.Reset();
        fixture.state.registers[0] = low;
        fixture.state.registers[1] = operand;
        fixture.state.registers[2] = high;
        try { fixture.Run(); }
        catch (const std::runtime_error&) { if (fixture.state.fault != 3 && fixture.state.fault != 4) throw; }
        std::cout << std::dec << fixture.state.fault << ' ' << std::hex << fixture.state.registers[0] << ' '
                  << fixture.state.registers[2] << ' ' << fixture.state.rflags << ' ' << fixture.state.rip << '\n';
    }
    require(std::cin.eof(), "Invalid wide arithmetic test input");
}

}

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--wide-arithmetic") {
            wideArithmetic();
            return 0;
        }
        testArithmeticWidths();
        testPartialRegistersAndExtensions();
        testMemoryWidthsAndSegments();
        testBitTest();
        testShiftAndRotate();
        testMultiply();
        testConditionalWrites();
        testStackAndUnary();
        std::puts("ARM64 byte/word arithmetic, carry/borrow, shifts, rotates, signed multiply, conditions and segmented memory passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
