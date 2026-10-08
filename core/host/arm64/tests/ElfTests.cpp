#include "Cpu.hpp"
#include "ElfImage.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <string>

using namespace AnyPS5::Arm64;

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Function>
void rejects(Function function, const char* text) {
    try { function(); } catch (const std::runtime_error& error) {
        if (std::string(error.what()).find(text) == std::string::npos) throw std::runtime_error("Expected " + std::string(text) + ": " + error.what());
        return;
    }
    throw std::runtime_error("Invalid dynamic ELF was accepted");
}

void field(std::vector<std::byte>& bytes, std::size_t offset, std::uint64_t value, std::size_t size = 8) {
    for (std::size_t i = 0; i < size; ++i) bytes.at(offset + i) = static_cast<std::byte>(value >> (i * 8));
}

std::vector<std::byte> fixture() {
    std::vector<std::byte> bytes(0x1000);
    field(bytes, 0, 0x010102464c457f, 7);
    field(bytes, 16, 3, 2);
    field(bytes, 18, 62, 2);
    field(bytes, 20, 1, 4);
    field(bytes, 24, 0x180);
    field(bytes, 32, 64);
    field(bytes, 52, 64, 2);
    field(bytes, 54, 56, 2);
    field(bytes, 56, 2, 2);
    field(bytes, 64, 1, 4);
    field(bytes, 68, Read | Write | Execute, 4);
    field(bytes, 96, bytes.size());
    field(bytes, 104, bytes.size() + 0x100);
    field(bytes, 112, 0x1000);
    field(bytes, 120, 2, 4);
    field(bytes, 124, Read, 4);
    field(bytes, 128, 0x200);
    field(bytes, 136, 0x200);
    field(bytes, 152, 0x121);
    field(bytes, 160, 0x121);
    const std::array<std::uint8_t, 14> code{0x48, 0x8b, 0x05, 0x71, 0x02, 0, 0, 0xff, 0x10, 0x48, 0x83, 0xc0, 7, 0xc3};
    for (std::size_t i = 0; i < code.size(); ++i) bytes[0x180 + i] = static_cast<std::byte>(code[i]);
    bytes[0x1a0] = std::byte{0xc3};
    bytes[0x1b0] = std::byte{0xc3};
    const std::array<std::array<std::uint64_t, 2>, 17> tags{{
        {1, 1}, {5, 0x400}, {10, 38}, {6, 0x380}, {11, 24}, {7, 0x600}, {8, 6 * 24}, {9, 24},
        {25, 0x500}, {27, 8}, {26, 0x508}, {28, 8}, {32, 0x510}, {33, 8}, {12, 0x1a0}, {13, 0x1b0}, {0, 0}
    }};
    for (std::size_t i = 0; i < tags.size(); ++i) {
        field(bytes, 0x200 + 16 * i, tags[i][0]);
        field(bytes, 0x208 + 16 * i, tags[i][1]);
    }
    const std::string strings("\0libnative.sprx\0native_answer\0defined\0", 38);
    for (std::size_t i = 0; i < strings.size(); ++i) bytes[0x400 + i] = static_cast<std::byte>(strings[i]);
    field(bytes, 0x398, 16, 4);
    field(bytes, 0x39c, 0x12, 1);
    field(bytes, 0x3b0, 30, 4);
    field(bytes, 0x3b4, 0x11, 1);
    field(bytes, 0x3b6, 1, 2);
    field(bytes, 0x3b8, 0x800);
    field(bytes, 0x3c0, 8);
    const std::array<std::array<std::uint64_t, 3>, 6> relocations{{
        {0x3f8, 8, 0x480}, {0x480, (std::uint64_t{1} << 32) | 6, 99},
        {0x488, (std::uint64_t{2} << 32) | 1, 5}, {0x500, 8, 0x1a0}, {0x508, 8, 0x1b0}, {0x510, 8, 0x1b0}
    }};
    for (std::size_t i = 0; i < relocations.size(); ++i) {
        for (std::size_t j = 0; j < 3; ++j) field(bytes, 0x600 + i * 24 + j * 8, relocations[i][j]);
    }
    return bytes;
}

std::uint64_t word(const GuestMemory& memory, std::uint64_t address) {
    std::uint64_t result = 0;
    require(memory.TryRead(address, std::as_writable_bytes(std::span(&result, 1))), "Relocation destination was not mapped");
    return result;
}

ElfOptions options() {
    return {0x20000000, [](const ElfImport& symbol) -> std::uint64_t {
        require(symbol.name == "native_answer" && symbol.type == 2 && symbol.binding == 1 && symbol.size == 0, "Import metadata changed");
        return 0x70000000;
    }};
}

void testDynamicExecution() {
    const auto source = fixture();
    auto image = LoadElf(source, options());
    constexpr auto base = std::uint64_t{0x20000000};
    require(image.entry == base + 0x180 && image.loadBias == base, "ELF load bias was not applied");
    require(image.libraries == std::vector<std::string>{"libnative.sprx"}, "ELF library names changed");
    require(word(image.memory, base + 0x3f8) == base + 0x480, "Relative relocation failed");
    require(word(image.memory, base + 0x480) == 0x70000000, "GLOB_DAT incorrectly applied its addend");
    require(word(image.memory, base + 0x488) == base + 0x805, "Defined symbol relocation failed");
    require(word(image.memory, base + 0x1000) == 0, "Dynamic image BSS was not zero initialized");
    require(image.initializers == std::vector<std::uint64_t>{base + 0x1b0, base + 0x1a0, base + 0x1a0}, "ELF initialization order changed");
    require(image.finalizers == std::vector<std::uint64_t>{base + 0x1b0, base + 0x1b0}, "ELF finalization order changed");
    image.memory.Map(0x8000, 0x1000, Read | Write);
    Cpu cpu(image.memory);
    cpu.Bind(0x70000000, +[](const HostCall&, void*) -> std::uint64_t { return 35; });
    CpuState state;
    state.rip = image.entry;
    state.registers[4] = 0x8ff8;
    cpu.Run(state);
    require(state.registers[0] == 42 && state.rip == 0, "Dynamic x86 ELF did not call native ARM64 code");
    require(source == fixture(), "Dynamic loading modified the source ELF");
}

void testInvalidImages() {
    rejects([] { static_cast<void>(LoadElf(fixture())); }, "native_answer");
    auto bytes = fixture();
    field(bytes, 0x398, 1000, 4);
    rejects([&] { static_cast<void>(LoadElf(bytes, options())); }, "string offset");
    bytes = fixture();
    field(bytes, 0x600, 0x2000);
    rejects([&] { static_cast<void>(LoadElf(bytes, options())); }, "outside load segments");
    bytes = fixture();
    field(bytes, 0x608, 5);
    rejects([&] { static_cast<void>(LoadElf(bytes, options())); }, "relocation type 5");
    bytes = fixture();
    field(bytes, 0x390, 0x1000000);
    field(bytes, 0x218, 0x1000000);
    rejects([&] { static_cast<void>(LoadElf(bytes, options())); }, "outside load segments");
    bytes = fixture();
    field(bytes, 0x270, 23);
    rejects([&] { static_cast<void>(LoadElf(bytes, options())); }, "RELA table");
    bytes = fixture();
    field(bytes, 0x600, 0x100f);
    field(bytes, 0x60c, 1, 1);
    rejects([&] { static_cast<void>(LoadElf(bytes, options())); }, "relative relocation has a symbol");
    bytes = fixture();
    field(bytes, 0x300, 1);
    field(bytes, 152, 0x110);
    rejects([&] { static_cast<void>(LoadElf(bytes, options())); }, "not terminated");
    bytes = fixture();
    field(bytes, 0x610, 0x4000);
    auto high = options();
    high.loadBias = ~std::uint64_t{0} - 0x2000;
    rejects([&] { static_cast<void>(LoadElf(bytes, high)); }, "overflows");
    bytes = fixture();
    field(bytes, 0x658, 0x2000);
    rejects([&] { static_cast<void>(LoadElf(bytes, options())); }, "initializer");
}

void testWeakAndPlt() {
    auto bytes = fixture();
    field(bytes, 0x39c, 0x22, 1);
    auto image = LoadElf(bytes);
    require(word(image.memory, image.loadBias + 0x480) == 0, "Undefined weak import did not resolve to zero");
    bytes = fixture();
    field(bytes, 0x620, (std::uint64_t{1} << 32) | 7);
    image = LoadElf(bytes, options());
    require(word(image.memory, image.loadBias + 0x480) == 0x70000000, "JUMP_SLOT relocation failed");
    bytes = fixture();
    field(bytes, 0x2e0, 0x6ffffff0);
    field(bytes, 0x2e8, 0x400);
    rejects([&] { static_cast<void>(LoadElf(bytes, options())); }, "versioned imports");
}

}

int main() {
    try {
        testDynamicExecution();
        testInvalidImages();
        testWeakAndPlt();
        std::puts("Dynamic ELF relocation, imports, initialization and invalid-image tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
