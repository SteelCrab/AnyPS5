#include "Cpu.hpp"
#include "ElfImage.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <fstream>
#include <filesystem>
#include <stdexcept>
#include <string>

using namespace AnyPS5::Arm64;

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<typename TAction>
void rejects(TAction action, const char* reason) {
    try {
        action();
    } catch (const std::exception& error) {
        if (std::string_view(error.what()).find(reason) != std::string_view::npos) return;
        throw std::runtime_error(std::string("Unexpected failure: ") + error.what());
    }
    throw std::runtime_error(std::string("Expected failure: ") + reason);
}

std::span<const std::byte> bytes(const std::vector<std::uint8_t>& code) { return std::as_bytes(std::span(code)); }

struct Fixture {
    GuestMemory memory;
    CpuState state;
    explicit Fixture(const std::vector<std::uint8_t>& code) {
        memory.Map(0x1000, code.size(), Read | Execute, bytes(code));
        memory.Map(0x8000, 0x1000, Read | Write);
        state.rip = 0x1000;
        state.registers[4] = 0x8ff8;
    }
    void Run(std::uint64_t budget = 1000000) { Cpu(memory).Run(state, budget); }
};

void append(std::vector<std::uint8_t>& code, std::uint64_t value, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) code.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
}

void move(std::vector<std::uint8_t>& code, std::uint8_t reg, std::uint64_t value) {
    code.push_back(0x48 | (reg >= 8 ? 1 : 0));
    code.push_back(0xb8 | (reg & 7));
    append(code, value, 8);
}

void testArithmeticAndFlags() {
    struct Case { std::uint8_t opcode; std::uint64_t left, right, result, flags; };
    const Case cases[]{
        {0x01, 0x7fffffffffffffffu, 1, 0x8000000000000000u, 0x894},
        {0x01, 0xffffffffffffffffu, 1, 0, 0x55},
        {0x29, 0, 1, 0xffffffffffffffffu, 0x95},
        {0x29, 0x8000000000000000u, 1, 0x7fffffffffffffffu, 0x814},
        {0x31, 0xf0, 0x0f, 0xff, 0x04},
        {0x21, 0xf0, 0x0f, 0, 0x44},
        {0x09, 0x80, 1, 0x81, 0x04}
    };
    for (const auto& test : cases) {
        std::vector<std::uint8_t> code;
        move(code, 0, test.left);
        move(code, 1, test.right);
        code.insert(code.end(), {0x48, test.opcode, 0xc8, 0xc3});
        Fixture fixture(code);
        fixture.Run();
        require(fixture.state.registers[0] == test.result, "ARM64 arithmetic result differs from x86 reference");
        require((fixture.state.rflags & 0x8d5) == test.flags, "ARM64 arithmetic flags differ from x86 reference");
    }
    std::vector<std::uint8_t> code;
    move(code, 0, ~std::uint64_t{0});
    code.insert(code.end(), {0xb8, 0xff, 0xff, 0xff, 0x7f, 0x83, 0xc0, 0x01, 0xc3});
    Fixture narrow(code);
    narrow.Run();
    require(narrow.state.registers[0] == 0x80000000u && (narrow.state.rflags & 0x8d5) == 0x894, "32-bit register write did not zero extend or set 32-bit flags");
    Fixture increment({0x48, 0xff, 0xc0, 0x48, 0xff, 0xc8, 0xc3});
    increment.state.registers[0] = 0xffffffffffffffffu;
    increment.state.rflags = 3;
    increment.Run();
    require(increment.state.registers[0] == 0xffffffffffffffffu && (increment.state.rflags & 1) != 0, "INC/DEC changed the carry flag");
}

void testBranches() {
    Fixture loop({0xb9, 10, 0, 0, 0, 0x31, 0xc0, 0x01, 0xc8, 0x83, 0xe9, 1, 0x75, 0xf9, 0xc3});
    Cpu cpu(loop.memory);
    cpu.Run(loop.state);
    require(loop.state.registers[0] == 55 && loop.state.registers[1] == 0 && cpu.CompiledBlocks() == 3, "Translated loop or branch cache failed");
    const std::array<std::uint64_t, 6> flags{2, 3, 0x46, 0x82, 0x802, 0x886};
    for (const auto bits : flags) {
        const bool c = (bits & 1) != 0, p = (bits & 4) != 0, z = (bits & 64) != 0;
        const bool s = (bits & 128) != 0, o = (bits & 2048) != 0;
        const std::array<bool, 16> expected{o, !o, c, !c, z, !z, c || z, !c && !z, s, !s, p, !p, s != o, s == o, z || s != o, !z && s == o};
        for (std::uint8_t condition = 0; condition < 16; ++condition) {
            Fixture branch({static_cast<std::uint8_t>(0x70 | condition), 6, 0xb8, 0, 0, 0, 0, 0xc3, 0xb8, 1, 0, 0, 0, 0xc3});
            branch.state.rflags = bits;
            branch.Run();
            require(branch.state.registers[0] == expected[condition], "Translated x86 condition code failed");
        }
    }
    Fixture forever({0xeb, 0xfe});
    rejects([&] { forever.Run(30); }, "budget");
}

void testRegisterExchange() {
    for (const auto width : {32, 64}) {
        for (std::uint8_t reg = 1; reg < 16; ++reg) {
            std::vector<std::uint8_t> code{static_cast<std::uint8_t>(0x40 | (width == 64 ? 8 : 0) | (reg >= 8 ? 1 : 0)),
                                         static_cast<std::uint8_t>(0x90 | (reg & 7)), 0xe9};
            append(code, 0 - (0x1000 + code.size() + 4), 4);
            Fixture fixture(code);
            fixture.state.registers[0] = 0x1122334455667788;
            fixture.state.registers[reg] = 0x8877665544332211;
            fixture.state.rflags = 0x8d7;
            fixture.Run();
            require(fixture.state.registers[0] == (width == 64 ? 0x8877665544332211 : 0x44332211), "Accumulator XCHG lost the source register");
            require(fixture.state.registers[reg] == (width == 64 ? 0x1122334455667788 : 0x55667788), "Accumulator XCHG lost the destination register");
            require(fixture.state.rflags == 0x8d7, "Register XCHG changed flags");
        }
    }
    Fixture extended({0x49, 0x90, 0xc3});
    extended.state.registers[0] = 42;
    extended.state.registers[8] = 77;
    extended.Run();
    require(extended.state.registers[0] == 77 && extended.state.registers[8] == 42, "REX.B 0x90 was treated as a NOP");
    Fixture nop({0x90, 0x48, 0x90, 0xc3});
    nop.state.registers[0] = 0x1122334455667788;
    nop.Run();
    require(nop.state.registers[0] == 0x1122334455667788, "NOP truncated the accumulator");
    Fixture self({0x87, 0xc0, 0xc3});
    self.state.registers[0] = 0x1122334455667788;
    self.Run();
    require(self.state.registers[0] == 0x55667788, "ModRM XCHG did not write a 32-bit register");
    Fixture atomic({0x48, 0x87, 0x03});
    rejects([&] { atomic.Run(); }, "Atomic memory XCHG");
}

void testCallsAndMemory() {
    Fixture direct({0x55, 0x48, 0x89, 0xe5, 0xe8, 6, 0, 0, 0, 0x48, 0x83, 0xc0, 1, 0x5d, 0xc3, 0xb8, 41, 0, 0, 0, 0xc3});
    direct.state.registers[5] = 0x1234;
    direct.Run();
    require(direct.state.registers[0] == 42 && direct.state.registers[5] == 0x1234 && direct.state.registers[4] == 0x9000, "Direct call, frame or return failed");
    std::vector<std::uint8_t> indirect;
    move(indirect, 8, 0x1000 + 14);
    indirect.insert(indirect.end(), {0x41, 0xff, 0xd0, 0xc3, 0xb8, 77, 0, 0, 0, 0xc3});
    Fixture indirectCall(indirect);
    indirectCall.state.registers[8] = 0;
    indirectCall.Run();
    require(indirectCall.state.registers[0] == 77, "Indirect call through extended register failed");
    Fixture memory({0x48, 0x89, 0x44, 0xcb, 0xf8, 0x48, 0x83, 0x44, 0xcb, 0xf8, 3,
                    0x48, 0x8b, 0x54, 0xcb, 0xf8, 0x48, 0x8d, 0x74, 0xcb, 0xf8, 0xc3});
    memory.memory.Map(0x2000, 0x100, Read | Write);
    memory.state.registers[0] = 39;
    memory.state.registers[3] = 0x2000;
    memory.state.registers[1] = 2;
    memory.Run();
    require(memory.state.registers[2] == 42 && memory.state.registers[6] == 0x2008, "Scaled memory operands or LEA failed");
    std::uint64_t stored = 0;
    require(memory.memory.TryRead(0x2008, std::as_writable_bytes(std::span(&stored, 1))) && stored == 42, "Translated memory store failed");
    Fixture relative({0x48, 0x8b, 0x05, 0xf9, 0x0f, 0, 0, 0xc3});
    const std::uint64_t value = 0x1234567890abcdef;
    relative.memory.Map(0x2000, 8, Read, std::as_bytes(std::span(&value, 1)));
    relative.Run();
    require(relative.state.registers[0] == value, "RIP-relative load failed");
    Fixture narrowAddress({0x8d, 0x43, 0x10, 0xc3});
    narrowAddress.state.registers[3] = 0x100000000;
    narrowAddress.Run();
    require(narrowAddress.state.registers[0] == 16, "32-bit LEA did not zero extend the effective address");
    Fixture signedImmediate({0x49, 0xc7, 0xc0, 0xff, 0xff, 0xff, 0xff, 0x4c, 0x89, 0xc0, 0xc3});
    signedImmediate.Run();
    require(signedImmediate.state.registers[0] == ~std::uint64_t{0}, "64-bit C7 immediate was not sign extended");
    Fixture popStack({0x5c, 0xc3});
    const std::uint64_t newStack = 0x8fe0;
    require(popStack.memory.TryWrite(0x8ff8, std::as_bytes(std::span(&newStack, 1))), "Could not initialize POP RSP fixture");
    popStack.Run();
    require(popStack.state.registers[4] == newStack + 8, "POP RSP used the old stack pointer after writeback");
}

void testMemoryFaults() {
    Fixture read({0x48, 0x8b, 0x00, 0xc3});
    read.state.registers[0] = 0x123456;
    rejects([&] { read.Run(); }, "read fault");
    require(read.state.registers[0] == 0x123456 && read.state.rip == 0x1000, "Faulting load changed guest state");
    Fixture write({0x48, 0x89, 0x03, 0xc3});
    write.state.registers[0] = 88;
    write.state.registers[3] = 0x1000;
    rejects([&] { write.Run(); }, "write fault");
    require(write.state.rip == 0x1000, "Faulting store advanced the guest PC");
    Fixture unsupported({0x0f, 0x05});
    rejects([&] { unsupported.Run(); }, "translation at guest 0x1000");
    Fixture supportedPrefix({0xb8, 42, 0, 0, 0, 0x0f, 0x05});
    rejects([&] { supportedPrefix.Run(); }, "translation at guest 0x1005");
    require(supportedPrefix.state.rip == 0x1005 && supportedPrefix.state.registers[0] == 42, "Unsupported instruction prevented earlier guest instructions from executing");
    Fixture truncated({0x48, 0x8b});
    rejects([&] { truncated.Run(); }, "truncated");
    GuestMemory memory;
    memory.Map(0x1000, 4, Read | Write);
    memory.Map(0x1004, 4, Read);
    const std::uint64_t input = ~std::uint64_t{0};
    require(!memory.TryWrite(0x1000, std::as_bytes(std::span(&input, 1))), "Cross-region write ignored permissions");
    std::uint64_t output = 99;
    require(memory.TryRead(0x1000, std::as_writable_bytes(std::span(&output, 1))) && output == 0, "Faulting cross-region write partially changed memory");
    require(!memory.TryRead(~std::uint64_t{0} - 1, std::as_writable_bytes(std::span(&output, 1))), "Overflowing guest range was accepted");
    rejects([&] { memory.Map(0x1002, 8, Read); }, "overlap");
    memory.Map(0x3000, 4, Write | Execute);
}

void testNativeCallBridge() {
    std::vector<std::uint8_t> code;
    move(code, 0, 0x70000000);
    code.insert(code.end(), {0xff, 0xd0, 0xc3});
    Fixture fixture(code);
    const std::array<std::uint8_t, 6> registers{7, 6, 2, 1, 8, 9};
    for (std::size_t i = 0; i < registers.size(); ++i) fixture.state.registers[registers[i]] = i + 1;
    const auto initialStack = fixture.state.registers[4];
    Cpu cpu(fixture.memory);
    std::uint64_t called = 0;
    const auto function = +[](const HostCall& call, void* context) -> std::uint64_t {
        require(call.arguments == std::array<std::uint64_t, 6>{1, 2, 3, 4, 5, 6}, "Native guest integer argument order changed");
        require(call.stackArguments == 0x8ff8, "Native guest stack argument address changed");
        ++*static_cast<std::uint64_t*>(context);
        return 42;
    };
    cpu.Bind(0x70000000, function, &called);
    rejects([&] { cpu.Bind(0x70000000, function); }, "duplicated");
    rejects([&] { cpu.Bind(0x1000, function); }, "overlaps");
    rejects([&] { cpu.Bind(0, function); }, "invalid");
    cpu.Run(fixture.state);
    require(called == 1 && fixture.state.registers[0] == 42 && fixture.state.registers[4] == initialStack + 8, "Native guest callback or return bridge failed");
    Fixture changedReturn(code);
    Cpu changedCpu(changedReturn.memory);
    changedCpu.Bind(0x70000000, +[](const HostCall& call, void*) -> std::uint64_t {
        const std::uint64_t next = 0;
        require(call.memory.TryWrite(call.stackArguments - 8, std::as_bytes(std::span(&next, 1))), "Could not change guest callback return address");
        return 77;
    });
    changedCpu.Run(changedReturn.state);
    require(changedReturn.state.registers[0] == 77 && changedReturn.state.registers[4] == initialStack, "Native callback ignored a guest return-address write");
}

void testMutableCode() {
    GuestMemory memory;
    const std::array<std::uint8_t, 13> code{0xc6, 0x05, 1, 0, 0, 0, 42, 0xb8, 1, 0, 0, 0, 0xc3};
    memory.Map(0x1000, code.size(), Read | Write | Execute, std::as_bytes(std::span(code)));
    memory.Map(0x8000, 0x1000, Read | Write);
    Cpu cpu(memory);
    CpuState state;
    state.rip = 0x1000;
    state.registers[4] = 0x8ff8;
    cpu.Run(state);
    require(state.registers[0] == 42, "Guest code write executed stale instructions from the same block");
    const auto version = memory.CodeVersion();
    const std::byte identical{42};
    require(memory.TryWrite(0x1008, std::span(&identical, 1)) && memory.CodeVersion() == version, "Identical code write invalidated native translations");
    const std::byte changed{77};
    require(memory.TryWrite(0x1008, std::span(&changed, 1)) && memory.CodeVersion() != version, "Code change did not invalidate native translations");
    state.rip = 0x1007;
    state.registers[4] = 0x8ff8;
    cpu.Run(state);
    require(state.registers[0] == 77, "Guest code write reused a stale block between runs");
    const auto beforeFault = memory.CodeVersion();
    const std::array<std::byte, 2> crossing{std::byte{0x90}, std::byte{0x90}};
    require(!memory.TryWrite(0x100c, crossing) && memory.CodeVersion() == beforeFault, "Faulting code write changed the code version");
    require(memory.Fetch(0x100c, 1) == std::vector<std::uint8_t>{0xc3}, "Faulting code write partially modified an instruction");
}

void field(std::vector<std::byte>& image, std::size_t offset, std::uint64_t value, std::size_t size) {
    for (std::size_t i = 0; i < size; ++i) image[offset + i] = static_cast<std::byte>(value >> (i * 8));
}

std::vector<std::byte> elf() {
    std::vector<std::byte> image(0x1006);
    field(image, 0, 0x010102464c457fu, 7);
    field(image, 16, 2, 2);
    field(image, 18, 62, 2);
    field(image, 20, 1, 4);
    field(image, 24, 0x4000, 8);
    field(image, 32, 64, 8);
    field(image, 52, 64, 2);
    field(image, 54, 56, 2);
    field(image, 56, 1, 2);
    field(image, 64, 1, 4);
    field(image, 68, Read | Execute, 4);
    field(image, 72, 0x1000, 8);
    field(image, 80, 0x4000, 8);
    field(image, 96, 6, 8);
    field(image, 104, 0x1000, 8);
    field(image, 112, 0x1000, 8);
    const std::array<std::byte, 6> code{std::byte{0xb8}, std::byte{42}, {}, {}, {}, std::byte{0xc3}};
    std::copy(code.begin(), code.end(), image.begin() + 0x1000);
    return image;
}

void testElfLoader() {
    auto source = elf();
    auto image = LoadElf(source);
    image.memory.Map(0x8000, 0x1000, Read | Write);
    CpuState state;
    state.rip = image.entry;
    state.registers[4] = 0x8ff8;
    Cpu(image.memory).Run(state);
    require(state.registers[0] == 42 && state.rip == 0, "Loaded x86 ELF did not execute through ARM64 translation");
    std::array<std::byte, 16> zeros{};
    require(image.memory.TryRead(0x4006, zeros) && std::all_of(zeros.begin(), zeros.end(), [](auto value) { return value == std::byte{}; }), "ELF BSS was not zero initialized");
    field(source, 64, 3, 4);
    rejects([&] { static_cast<void>(LoadElf(source)); }, "interpreter");
    source = elf();
    field(source, 104, 1, 8);
    rejects([&] { static_cast<void>(LoadElf(source)); }, "load segment");
    source = elf();
    field(source, 24, 0x9000, 8);
    rejects([&] { static_cast<void>(LoadElf(source)); }, "entry point");
}

}

int main(int argc, char** argv) {
    try {
        testArithmeticAndFlags();
        testBranches();
        testRegisterExchange();
        testCallsAndMemory();
        testMemoryFaults();
        testNativeCallBridge();
        testMutableCode();
        testElfLoader();
        if (argc == 2) {
            const auto image = elf();
            std::ofstream file(std::filesystem::path(argv[1]) / "homebrew.elf", std::ios::binary);
            file.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
            file.close();
            require(static_cast<bool>(file), "Could not write ELF fixture");
        }
        std::puts("ARM64 translated arithmetic, flags, branches, calls, memory faults and ELF loading passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
