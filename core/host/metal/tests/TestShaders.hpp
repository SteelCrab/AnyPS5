#pragma once

#include "MetalShader.hpp"
#include "ControlFlow/RequestSerializer.hpp"
#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <SPIRV/GlslangToSpv.h>
#include <stdexcept>

inline std::vector<std::uint32_t> CompileGlsl(const char* source, EShLanguage stage) {
    static const bool initialized = glslang::InitializeProcess();
    if (!initialized) throw std::runtime_error("Initialize glslang failed");
    glslang::TShader shader(stage);
    shader.setStrings(&source, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 450);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
    const auto messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);
    if (!shader.parse(GetDefaultResources(), 450, false, messages)) throw std::runtime_error(shader.getInfoLog());
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages)) throw std::runtime_error(program.getInfoLog());
    std::vector<std::uint32_t> words;
    glslang::GlslangToSpv(*program.getIntermediate(stage), words);
    return words;
}

inline void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<typename TAction>
void Rejects(TAction action, const char* reason) {
    try {
        action();
    } catch (const std::exception& error) {
        if (std::string_view(error.what()).find(reason) != std::string_view::npos) return;
        throw std::runtime_error(std::string("Unexpected rejection: ") + error.what());
    }
    throw std::runtime_error(std::string("Expected rejection: ") + reason);
}

inline constexpr const char* ComputeSource = R"(#version 450
layout(local_size_x = 32) in;
layout(set = 3, binding = 5, std430) readonly buffer Input { uint values[]; } inputData;
layout(set = 7, binding = 11, std430) writeonly buffer Output { uint values[]; } outputData;
layout(push_constant) uniform Parameters { uint bias; } parameters;
void main() {
    uint i = gl_GlobalInvocationID.x;
    outputData.values[i] = (inputData.values[i] * inputData.values[i]) ^ parameters.bias;
}
)";

inline constexpr const char* VertexSource = R"(#version 450
void main() {
    vec2 position = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(position * 2.0 - 1.0, 0.5, 1.0);
}
)";

inline constexpr const char* TextureSource = R"(#version 450
layout(local_size_x = 32) in;
layout(set = 1, binding = 1, std430) buffer Output { vec4 values[3]; } outputData;
layout(set = 3, binding = 6) uniform texture2D sourceTexture;
layout(set = 4, binding = 10) uniform sampler sourceSampler;
layout(set = 7, binding = 1, rgba8) uniform writeonly image2D outputTexture;
void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i >= 8) return;
    ivec2 position = ivec2(i & 3, i >> 2);
    vec2 uv = (vec2(position) + vec2(0.5)) / vec2(4.0, 2.0);
    imageStore(outputTexture, position, textureLod(sampler2D(sourceTexture, sourceSampler), uv, 0.0).bgra);
    if (i < 3) {
        float u = i == 0 ? 0.5625 : (i == 1 ? 1.125 : -0.375);
        outputData.values[i] = textureLod(sampler2D(sourceTexture, sourceSampler), vec2(u, 0.25), 0.0);
    }
}
)";

inline constexpr const char* FragmentSource = R"(#version 450
layout(location = 0) out vec4 color;
void main() { color = vec4(0.25, 0.5, 0.75, 1.0); }
)";

inline ShaderRecompiler::RecompileRequest RdnaFragmentRequest() {
    using namespace ShaderRecompiler;
    static constexpr std::array<std::uint32_t, 11> code{
        0x7e0002ffu, 0x3e800000u, 0x7e0202ffu, 0x3f000000u,
        0x7e0402ffu, 0x3f400000u, 0x7e0602ffu, 0x3f800000u,
        0xf800180fu, 0x03020100u, 0xbf810000u
    };
    ShaderPixelStageInfo pixel{};
    pixel.targetOutputMode[0] = 9;
    pixel.targetExportMapping.fill(0xe4u);
    RecompileRequest request{};
    request.shader = {ShaderStage::Fragment, 0x30000, code, 0, {}};
    request.context.waveSize = 32;
    request.context.pixel = pixel;
    request.target = AnyPS5::Metal::RecompileTarget();
    request.layout.pushConstantSizeBytes = 128;
    request.useCache = false;
    return request;
}

inline AnyPS5::Metal::Shader CompileRdnaFragment() {
    return AnyPS5::Metal::Translate(ShaderRecompiler::Recompile(RdnaFragmentRequest()).spirv.Words());
}
