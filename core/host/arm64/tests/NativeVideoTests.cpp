#include "NativeVideoOut.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <string>

using namespace AnyPS5::Arm64;

namespace {

void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }

template<class TFunction>
void rejects(TFunction function, const char* expected) {
    try { function(); } catch (const std::runtime_error& error) {
        if (std::string(error.what()).find(expected) != std::string::npos) return;
        throw;
    }
    throw std::runtime_error("Invalid native VideoOut operation succeeded");
}

struct Fixture {
    GuestMemory memory;
    Cpu cpu{memory};
    NativeVideoOut video;
    Fixture() {
        memory.Map(0x2000, 0x1000, Read | Write);
        memory.Map(0x8000, 0x2000, Read | Write);
        video.Bind(cpu);
    }
    std::uint64_t Call(std::string_view name, std::array<std::uint64_t, 6> arguments, std::array<std::uint64_t, 2> stack = {}) {
        require(memory.TryWrite(0x9000, std::as_bytes(std::span(stack))), "Could not initialize native VideoOut stack arguments");
        CpuState state;
        state.rip = video.Resolve({name, 2, 1, 0});
        require(state.rip != 0, "Native VideoOut function was not resolved");
        state.registers[4] = 0x8ff8;
        const std::array<std::uint8_t, 6> registers{7, 6, 2, 1, 8, 9};
        for (std::size_t i = 0; i < arguments.size(); ++i) state.registers[registers[i]] = arguments[i];
        cpu.Run(state);
        return state.registers[0];
    }
};

void testGuestVideoOut() {
    Fixture fixture;
    require(fixture.video.Resolve({"sceVideoOutUnknown", 2, 1, 0}) == 0, "Missing VideoOut import was falsely resolved");
    const auto handle = fixture.Call("sceVideoOutOpen", {255, 0, 0, 0});
    rejects([&] { fixture.Call("sceVideoOutOpen", {255, 1, 0, 0}); }, "open parameters");
    fixture.Call("sceVideoOutSetBufferAttribute2", {0x2000, 0x8000000000000000, 1, 37, 29, 0});
    const std::array<std::uint64_t, 4> buffer{0x10000, 0, 0, 0};
    require(fixture.memory.TryWrite(0x2100, std::as_bytes(std::span(buffer))), "Could not initialize native VideoOut buffer descriptor");
    std::vector<std::byte> pixels(37 * 29 * 4);
    for (std::uint32_t y = 0; y < 29; ++y) {
        for (std::uint32_t x = 0; x < 37; ++x) {
            const auto offset = (y * 37 + x) * 4;
            pixels[offset] = static_cast<std::byte>(y * 7);
            pixels[offset + 1] = static_cast<std::byte>(x ^ y);
            pixels[offset + 2] = static_cast<std::byte>(x * 5);
            pixels[offset + 3] = std::byte{0xff};
        }
    }
    fixture.memory.Map(0x10000, pixels.size(), Read | Write, pixels);
    require(fixture.Call("sceVideoOutRegisterBuffers2", {handle, 0, 0, 0x2100, 1, 0x2000}) == 0, "Native VideoOut did not register guest buffers");
    rejects([&] { fixture.Call("sceVideoOutRegisterBuffers2", {handle, 0, 0, 0x2100, 1, 0x2000}); }, "occupied");
    rejects([&] { fixture.Call("sceVideoOutSubmitFlip", {handle, 1, 1, 0}); }, "buffer index");
    rejects([&] { fixture.Call("sceVideoOutSubmitFlip", {handle, 0, 2, 0}); }, "flip mode");
    fixture.Call("sceVideoOutSetFlipRate", {handle, 1});
    const auto function = fixture.video.Resolve({"sceVideoOutSubmitFlip", 2, 1, 0});
    std::vector<std::uint8_t> code{0x48, 0xb8};
    for (std::size_t i = 0; i < 8; ++i) code.push_back(static_cast<std::uint8_t>(function >> (i * 8)));
    code.insert(code.end(), {0xff, 0xd0, 0xc3});
    fixture.memory.Map(0x1000, code.size(), Read | Execute, std::as_bytes(std::span(code)));
    CpuState state;
    state.rip = 0x1000;
    state.registers[4] = 0x8ff8;
    state.registers[7] = handle;
    state.registers[6] = 0;
    state.registers[2] = 1;
    state.registers[1] = 0x1234;
    fixture.cpu.Run(state);
    require(state.registers[0] == 0 && fixture.cpu.CompiledBlocks() > 0 && fixture.video.FlipCount(static_cast<int>(handle)) == 1 && fixture.video.FlipArgument(static_cast<int>(handle)) == 0x1234, "Translated guest did not complete native Metal VideoOut flip");
    const auto result = fixture.video.Readback(static_cast<int>(handle));
    const auto size = fixture.video.FrameSize(static_cast<int>(handle));
    for (std::uint32_t y = 0; y < size[1]; ++y) {
        for (std::uint32_t x = 0; x < size[0]; ++x) {
            const auto sourceX = static_cast<std::uint32_t>((std::uint64_t{2 * x + 1} * 37) / (2 * size[0]));
            const auto sourceY = static_cast<std::uint32_t>((std::uint64_t{2 * y + 1} * 29) / (2 * size[1]));
            const auto expected = (sourceY * 37 + sourceX) * 4;
            const auto actual = (static_cast<std::size_t>(y) * size[0] + x) * 4;
            require(std::equal(pixels.begin() + expected, pixels.begin() + expected + 4, result.begin() + actual), "Guest VideoOut framebuffer differs from the displayed Metal drawable");
        }
    }
    require(fixture.Call("sceVideoOutClose", {handle}) == 0, "Native VideoOut close failed");
    require(static_cast<std::int32_t>(fixture.Call("sceVideoOutClose", {handle})) == -2144796661, "Closed native VideoOut handle was accepted");
}

}

int main() {
    try {
        testGuestVideoOut();
        std::puts("x86 guest to native ARM64 VideoOut to Metal window presentation passed");
        return 0;
    } catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
