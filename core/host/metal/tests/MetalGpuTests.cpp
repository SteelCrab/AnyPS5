#include "MetalDevice.hpp"
#include "TestShaders.hpp"
#include <cstdio>
#include <cmath>

namespace {

void testTextures(AnyPS5::Metal::Device& device) {
    using namespace AnyPS5::Metal;
    const auto shader = Translate(CompileGlsl(TextureSource, EShLangCompute));
    const auto pipeline = device.CreateComputePipeline(shader);
    auto source = device.CreateTexture(4, 2);
    auto target = device.CreateTexture(4, 2);
    auto output = device.CreateBuffer(3 * 4 * sizeof(float));
    std::array<std::byte, 32> rgba{}, rendered{};
    for (std::size_t i = 0; i < rgba.size(); ++i) rgba[i] = static_cast<std::byte>(i * 7 + 13);
    source.Write(rgba);
    source.Read(rendered);
    Require(rendered == rgba, "Metal texture upload or CPU readback changed pixel bytes");
    const std::array<BufferBinding, 1> buffers{{{0, output}}};
    const std::array<TextureBinding, 2> textures{{{0, source}, {1, target}}};
    for (const auto filter : {Filter::Nearest, Filter::Linear}) {
        for (const auto address : {AddressMode::ClampToEdge, AddressMode::Repeat, AddressMode::MirrorRepeat}) {
            const std::array<SamplerBinding, 1> samplers{{{0, device.CreateSampler(filter, address)}}};
            device.Dispatch(pipeline, buffers, {1, 1, 1}, textures, samplers);
            target.Read(rendered);
            const std::array<std::size_t, 4> channels{2, 1, 0, 3};
            for (std::size_t i = 0; i < rendered.size(); ++i) {
                const int difference = std::to_integer<int>(rendered[i]) - std::to_integer<int>(rgba[(i / 4) * 4 + channels[i % 4]]);
                Require(difference >= -1 && difference <= 1, "Metal imageStore or texel-center sampling differs from CPU reference");
            }
            std::array<float, 12> samples{};
            output.Read(0, std::as_writable_bytes(std::span(samples)));
            for (std::size_t channel = 0; channel < 4; ++channel) {
                const auto pixel = [&](std::size_t index) { return static_cast<float>(std::to_integer<unsigned>(rgba[index * 4 + channel])) / 255.0f; };
                const auto expected = filter == Filter::Nearest ? pixel(2) : 0.25f * pixel(1) + 0.75f * pixel(2);
                Require(std::fabs(samples[channel] - expected) < 0.00002f, "Metal nearest or linear sampling differs from CPU reference");
                const auto positive = address == AddressMode::Repeat ? 0 : 3;
                const auto negative = address == AddressMode::ClampToEdge ? 0 : address == AddressMode::Repeat ? 2 : 1;
                Require(std::fabs(samples[4 + channel] - pixel(positive)) < 0.00002f &&
                        std::fabs(samples[8 + channel] - pixel(negative)) < 0.00002f, "Metal texture addressing differs from CPU reference");
            }
        }
    }
    const auto vertex = Translate(CompileGlsl(R"(#version 450
layout(set = 5, binding = 2, std140) uniform Transform { vec4 value; } transform;
void main() {
    vec2 position = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4((position * 2.0 - 1.0) * transform.value.xy + transform.value.zw, 0.5, 1.0);
}
)", EShLangVertex));
    const auto fragment = Translate(CompileGlsl(R"(#version 450
layout(set = 3, binding = 6) uniform texture2D sourceTexture;
layout(set = 4, binding = 10) uniform sampler sourceSampler;
layout(push_constant) uniform Tint { vec4 value; } tint;
layout(location = 0) out vec4 color;
void main() {
    ivec2 pixel = ivec2(gl_FragCoord.xy) % ivec2(4, 2);
    vec2 uv = (vec2(pixel) + vec2(0.5)) / vec2(4.0, 2.0);
    color = textureLod(sampler2D(sourceTexture, sourceSampler), uv, 0.0) * tint.value;
}
)", EShLangFragment));
    auto transform = device.CreateBuffer(16), tint = device.CreateBuffer(16);
    const std::array<float, 4> transformData{1, 1, 0, 0}, tintData{1, 1, 1, 1};
    transform.Write(0, std::as_bytes(std::span(transformData)));
    tint.Write(0, std::as_bytes(std::span(tintData)));
    const std::array<BufferBinding, 1> vertexBuffers{{{0, transform}}}, fragmentBuffers{{{0, tint}}};
    const std::array<TextureBinding, 1> fragmentTextures{{{0, source}}};
    const std::array<SamplerBinding, 1> fragmentSamplers{{{0, device.CreateSampler()}}};
    const auto pixels = device.Render(vertex, fragment, 13, 7, {vertexBuffers}, {fragmentBuffers, fragmentTextures, fragmentSamplers});
    for (std::size_t y = 0; y < 7; ++y) {
        for (std::size_t x = 0; x < 13; ++x) {
            for (std::size_t channel = 0; channel < 4; ++channel) {
                const int difference = std::to_integer<int>(pixels[(y * 13 + x) * 4 + channel]) - std::to_integer<int>(rgba[((y % 2) * 4 + x % 4) * 4 + channel]);
                Require(difference >= -1 && difference <= 1, "Metal vertex/fragment resource binding or textured rendering differs from CPU reference");
            }
        }
    }
    Rejects([&] { static_cast<void>(device.Render(vertex, fragment, 13, 7)); }, "bindings");
    Rejects([&] { device.Dispatch(pipeline, buffers, {1, 1, 1}); }, "bindings");
    auto duplicated = textures;
    duplicated[1].metalIndex = 0;
    const std::array<SamplerBinding, 1> samplers{{{0, device.CreateSampler()}}};
    Rejects([&] { device.Dispatch(pipeline, buffers, {1, 1, 1}, duplicated, samplers); }, "duplicated");
    Rejects([&] { source.Write(std::span(rgba).first(4)); }, "byte count");
    Rejects([&] { source.Read(std::span(rendered).first(4)); }, "byte count");
    source.Read(rendered);
    Require(rendered == rgba, "Rejected texture access changed source pixels");
    Rejects([&] { static_cast<void>(device.CreateTexture(0, 2)); }, "dimensions");
    Rejects([&] { static_cast<void>(device.CreateSampler(static_cast<Filter>(99))); }, "filter");
    Rejects([&] { static_cast<void>(device.CreateSampler(Filter::Nearest, static_cast<AddressMode>(99))); }, "address mode");
}

}

int main() {
    using namespace AnyPS5::Metal;
    try {
        Device device;
        std::printf("Native Metal GPU: %s\n", device.Name().c_str());
        testTextures(device);
        const auto shader = Translate(CompileGlsl(ComputeSource, EShLangCompute));
        const auto pipeline = device.CreateComputePipeline(shader);
        auto input = device.CreateBuffer(128 * sizeof(std::uint32_t));
        auto output = device.CreateBuffer(input.Size());
        auto parameters = device.CreateBuffer(16);
        std::array<std::uint32_t, 128> data{};
        std::array<std::uint32_t, 128> result{};
        const std::array<std::uint32_t, 1> bias{0x12345678};
        parameters.Write(0, std::as_bytes(std::span(bias)));
        const std::array<BufferBinding, 3> buffers{{{0, input}, {1, output}, {2, parameters}}};
        for (std::uint32_t pass = 0; pass < 2; ++pass) {
            for (std::uint32_t i = 0; i < data.size(); ++i) data[i] = i + pass * 37;
            input.Write(0, std::as_bytes(std::span(data)));
            device.Dispatch(pipeline, buffers, {4, 1, 1});
            output.Read(0, std::as_writable_bytes(std::span(result)));
            for (std::size_t i = 0; i < data.size(); ++i) Require(result[i] == ((data[i] * data[i]) ^ bias[0]), "Metal compute result differs from CPU reference");
        }
        const auto subgroup = Translate(CompileGlsl(R"(#version 450
#extension GL_KHR_shader_subgroup_basic : require
#extension GL_KHR_shader_subgroup_shuffle : require
#extension GL_KHR_shader_subgroup_ballot : require
layout(local_size_x = 32) in;
layout(set = 0, binding = 0, std430) buffer Output { uvec2 values[]; } outputData;
void main() {
    uint i = gl_GlobalInvocationID.x;
    outputData.values[i] = uvec2(subgroupShuffleXor(i, 1), subgroupBallot((i & 1) == 0).x);
}
)", EShLangCompute));
        const auto subgroupPipeline = device.CreateComputePipeline(subgroup);
        auto subgroupOutput = device.CreateBuffer(128 * 2 * sizeof(std::uint32_t));
        const std::array<BufferBinding, 1> subgroupBuffers{{{subgroup.resources.at(0).metalIndex, subgroupOutput}}};
        device.Dispatch(subgroupPipeline, subgroupBuffers, {4, 1, 1});
        std::array<std::uint32_t, 256> subgroupResult{};
        subgroupOutput.Read(0, std::as_writable_bytes(std::span(subgroupResult)));
        for (std::uint32_t i = 0; i < 128; ++i) {
            Require(subgroupResult[i * 2] == (i ^ 1) && subgroupResult[i * 2 + 1] == 0x55555555u, "Metal SIMD shuffle or ballot differs from reference");
        }
        const auto floatingPoint = Translate(CompileGlsl(R"(#version 450
layout(local_size_x = 1) in;
layout(set = 0, binding = 0, std430) readonly buffer Input { float value; } inputData;
layout(set = 0, binding = 1, std430) buffer Output { uvec3 value; } outputData;
void main() {
    outputData.value = uvec3(floatBitsToUint(-0.0 * inputData.value),
        isinf(1.0 / (inputData.value - inputData.value)) ? 1 : 0,
        isnan((inputData.value - inputData.value) / (inputData.value - inputData.value)) ? 1 : 0);
}
)", EShLangCompute));
        auto floatInput = device.CreateBuffer(16);
        auto floatOutput = device.CreateBuffer(16);
        const std::array<float, 1> floatValue{2.0f};
        floatInput.Write(0, std::as_bytes(std::span(floatValue)));
        const std::array<BufferBinding, 2> floatBuffers{{{0, floatInput}, {1, floatOutput}}};
        device.Dispatch(device.CreateComputePipeline(floatingPoint), floatBuffers, {1, 1, 1});
        std::array<std::uint32_t, 3> floatResult{};
        floatOutput.Read(0, std::as_writable_bytes(std::span(floatResult)));
        Require(floatResult == std::array<std::uint32_t, 3>{0x80000000u, 1, 1}, "Metal compilation changed signed zero, infinity or NaN behavior");
        Rejects([&] { device.Dispatch(pipeline, {}, {4, 1, 1}); }, "bindings");
        Rejects([&] { device.Dispatch(pipeline, buffers, {0, 1, 1}); }, "dimensions");
        auto duplicated = buffers;
        duplicated[1].metalIndex = 0;
        Rejects([&] { device.Dispatch(pipeline, duplicated, {4, 1, 1}); }, "duplicated");
        auto outOfBounds = buffers;
        outOfBounds[0].offset = input.Size();
        Rejects([&] { device.Dispatch(pipeline, outOfBounds, {4, 1, 1}); }, "bounds");
        Rejects([&] { input.Write(input.Size(), std::as_bytes(std::span(bias))); }, "bounds");
        Rejects([&] { static_cast<void>(device.CreateBuffer(0)); }, "size");
        auto badSource = shader;
        badSource.source = "this is not Metal shader source";
        Rejects([&] { device.Validate(badSource); }, "Compile Metal shader");
        const auto vertex = Translate(CompileGlsl(VertexSource, EShLangVertex));
        const auto fragment = CompileRdnaFragment();
        const auto pixels = device.Render(vertex, fragment, 37, 19);
        const std::array<int, 4> expected{64, 128, 191, 255};
        Require(pixels.size() == 37 * 19 * 4, "Metal render readback has the wrong size");
        for (std::size_t i = 0; i < pixels.size(); ++i) {
            const int difference = std::to_integer<int>(pixels[i]) - expected[i % 4];
            Require(difference >= -1 && difference <= 1, "Metal rendered pixel differs from reference");
        }
        Rejects([&] { static_cast<void>(device.Render(vertex, fragment, 0, 19)); }, "dimensions");
        std::puts("Metal compute, textures, sampling, SIMD shuffle/ballot, float controls, RDNA rendering and error handling passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
