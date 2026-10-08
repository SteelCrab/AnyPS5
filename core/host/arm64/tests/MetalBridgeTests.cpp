#include "Cpu.hpp"
#include "MetalDevice.hpp"
#include "TestShaders.hpp"
#include <cstdio>

using namespace AnyPS5;

namespace {

struct Context {
    Metal::Device device;
    Metal::ComputePipeline pipeline;
    Context() : pipeline(device.CreateComputePipeline(Metal::Translate(CompileGlsl(ComputeSource, EShLangCompute)))) {}
};

std::uint64_t dispatch(const Arm64::HostCall& call, void* context) {
    auto& native = *static_cast<Context*>(context);
    const auto count = call.arguments[2];
    Require(count != 0 && count <= 1024 * 1024 && count % 32 == 0, "Native Metal bridge: invalid element count");
    std::vector<std::uint32_t> data(static_cast<std::size_t>(count));
    Require(call.memory.TryRead(call.arguments[0], std::as_writable_bytes(std::span(data))), "Native Metal bridge: unreadable guest input");
    auto input = native.device.CreateBuffer(data.size() * sizeof(data[0]));
    auto output = native.device.CreateBuffer(input.Size());
    auto parameters = native.device.CreateBuffer(16);
    input.Write(0, std::as_bytes(std::span(data)));
    const std::array<std::uint32_t, 1> bias{static_cast<std::uint32_t>(call.arguments[3])};
    parameters.Write(0, std::as_bytes(std::span(bias)));
    const std::array<Metal::BufferBinding, 3> buffers{{{0, input}, {1, output}, {2, parameters}}};
    native.device.Dispatch(native.pipeline, buffers, {static_cast<std::uint32_t>(count / 32), 1, 1});
    output.Read(0, std::as_writable_bytes(std::span(data)));
    Require(call.memory.TryWrite(call.arguments[1], std::as_bytes(std::span(data))), "Native Metal bridge: unwritable guest output");
    return count;
}

void move(std::vector<std::uint8_t>& code, std::uint8_t reg, std::uint64_t value) {
    code.push_back(0x48 | (reg >= 8 ? 1 : 0));
    code.push_back(0xb8 | (reg & 7));
    for (std::size_t i = 0; i < 8; ++i) code.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
}

void testRenderBridge(Metal::Device& device) {
    struct RenderContext {
        Metal::Device& device;
        Metal::Shader vertex, fragment;
    } context{device, Metal::Translate(CompileGlsl(VertexSource, EShLangVertex)), CompileRdnaFragment()};
    std::vector<std::uint8_t> code;
    move(code, 7, 37);
    move(code, 6, 19);
    move(code, 2, 0x3000);
    move(code, 0, 0x70000080);
    code.insert(code.end(), {0xff, 0xd0, 0x8b, 0x02, 0xc3});
    Arm64::GuestMemory memory;
    memory.Map(0x1000, code.size(), Arm64::Read | Arm64::Execute, std::as_bytes(std::span(code)));
    memory.Map(0x3000, 37 * 19 * 4, Arm64::Read | Arm64::Write);
    memory.Map(0x8000, 0x1000, Arm64::Read | Arm64::Write);
    Arm64::CpuState state;
    state.rip = 0x1000;
    state.registers[4] = 0x8ff8;
    Arm64::Cpu cpu(memory);
    cpu.Bind(0x70000080, +[](const Arm64::HostCall& call, void* context) -> std::uint64_t {
        auto& native = *static_cast<RenderContext*>(context);
        Require(call.arguments[0] <= 16384 && call.arguments[1] <= 16384, "Guest render dimensions are outside device limits");
        const auto pixels = native.device.Render(native.vertex, native.fragment,
            static_cast<std::uint32_t>(call.arguments[0]), static_cast<std::uint32_t>(call.arguments[1]));
        Require(call.memory.TryWrite(call.arguments[2], pixels), "Native Metal render bridge: unwritable guest output");
        return pixels.size();
    }, &context);
    cpu.Run(state);
    std::array<std::byte, 37 * 19 * 4> pixels{};
    Require(memory.TryRead(0x3000, pixels), "Could not read rendered guest pixels");
    const std::array<int, 4> expected{64, 128, 191, 255};
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        const int difference = std::to_integer<int>(pixels[i]) - expected[i % 4];
        Require(difference >= -1 && difference <= 1, "Guest Metal render differs from CPU reference");
    }
    std::uint32_t first = 0;
    Require(memory.TryRead(0x3000, std::as_writable_bytes(std::span(&first, 1))) && state.registers[0] == first,
            "Translated guest did not read the rendered pixel");
}

}

int main() {
    try {
        std::vector<std::uint8_t> code;
        move(code, 7, 0x2000);
        move(code, 6, 0x3000);
        move(code, 2, 128);
        move(code, 1, 0x12345678);
        move(code, 0, 0x70000000);
        code.insert(code.end(), {0xff, 0xd0, 0x8b, 0x06, 0xc3});
        std::array<std::uint32_t, 128> input{};
        for (std::uint32_t i = 0; i < input.size(); ++i) input[i] = i + 3;
        Arm64::GuestMemory memory;
        memory.Map(0x1000, code.size(), Arm64::Read | Arm64::Execute, std::as_bytes(std::span(code)));
        memory.Map(0x2000, sizeof(input), Arm64::Read, std::as_bytes(std::span(input)));
        memory.Map(0x3000, sizeof(input), Arm64::Read | Arm64::Write);
        memory.Map(0x8000, 0x1000, Arm64::Read | Arm64::Write);
        Arm64::CpuState state;
        state.rip = 0x1000;
        state.registers[4] = 0x8ff8;
        Context context;
        Arm64::Cpu cpu(memory);
        cpu.Bind(0x70000000, dispatch, &context);
        cpu.Run(state);
        std::array<std::uint32_t, 128> output{};
        Require(memory.TryRead(0x3000, std::as_writable_bytes(std::span(output))), "Could not read guest Metal output");
        for (std::size_t i = 0; i < output.size(); ++i) Require(output[i] == ((input[i] * input[i]) ^ 0x12345678u), "Guest Metal result differs from CPU reference");
        Require(state.registers[0] == output[0], "Translated guest did not read the Metal result");
        testRenderBridge(context.device);
        std::printf("x86 guest -> ARM64 JIT -> native Metal compute/RDNA rendering on %s -> guest memory passed\n", context.device.Name().c_str());
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
