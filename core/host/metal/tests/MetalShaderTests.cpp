#include "TestShaders.hpp"
#include <cstdio>
#include <filesystem>
#include <fstream>

int main(int argc, char** argv) {
    using namespace AnyPS5::Metal;
    try {
        const auto shader = Translate(CompileGlsl(ComputeSource, EShLangCompute));
        Require(shader.stage == Stage::Compute && shader.workgroupSize == std::array<std::uint32_t, 3>{32, 1, 1}, "Compute workgroup changed in Metal translation");
        Require(shader.resources.size() == 3, "Compute resource reflection lost a binding");
        Require(shader.resources[0].descriptorSet == 3 && shader.resources[0].binding == 5 && shader.resources[0].metalIndex == 0, "Input buffer binding mapping changed");
        Require(shader.resources[1].descriptorSet == 7 && shader.resources[1].binding == 11 && shader.resources[1].metalIndex == 1, "Output buffer binding mapping changed");
        Require(shader.resources[2].kind == ResourceKind::PushConstants && shader.resources[2].metalIndex == 2 && shader.resources[2].minimumBytes == 4, "Push constant reflection changed");
        const auto vertex = Translate(CompileGlsl(VertexSource, EShLangVertex));
        const auto fragment = Translate(CompileGlsl(FragmentSource, EShLangFragment));
        Require(vertex.stage == Stage::Vertex && fragment.stage == Stage::Fragment, "Graphics stage translation changed");
        Require(CompileRdnaFragment().stage == Stage::Fragment, "RDNA fragment translation failed");
        const auto texture = Translate(CompileGlsl(TextureSource, EShLangCompute));
        Require(texture.resources.size() == 4 && texture.resources[0].kind == ResourceKind::Buffer &&
                texture.resources[1].kind == ResourceKind::Texture && texture.resources[1].metalIndex == 0 &&
                texture.resources[2].kind == ResourceKind::Sampler && texture.resources[2].metalIndex == 0 &&
                texture.resources[3].kind == ResourceKind::Texture && texture.resources[3].metalIndex == 1,
                "Texture, sampler and buffer binding namespaces collided");
        Rejects([] { static_cast<void>(Translate(CompileGlsl(R"(#version 450
layout(local_size_x = 1) in;
layout(set = 0, binding = 0, rgba32f) uniform writeonly image2D outputTexture;
void main() { imageStore(outputTexture, ivec2(0), vec4(1.0)); }
)", EShLangCompute))); }, "RGBA8");
        Rejects([] { static_cast<void>(Translate(std::array<std::uint32_t, 1>{0})); }, "header");
        Rejects([] { static_cast<void>(Translate(CompileGlsl(R"(#version 450
layout(local_size_x = 1) in;
layout(set = 0, binding = 0, std430) buffer Data { double value; } data;
void main() { data.value *= 2.0; }
)", EShLangCompute))); }, "64-bit");
        Rejects([] { static_cast<void>(Translate(CompileGlsl(R"(#version 450
layout(local_size_x = 1) in;
layout(set = 0, binding = 0, std430) buffer Data { uint value; } data[2];
void main() { data[1].value = data[0].value; }
)", EShLangCompute))); }, "descriptor arrays");
        Rejects([] { static_cast<void>(Translate(CompileGlsl(R"(#version 450
layout(local_size_x = 1) in;
layout(constant_id = 0) const uint multiplier = 2;
layout(set = 0, binding = 0, std430) buffer Data { uint value; } data;
void main() { data.value *= multiplier; }
)", EShLangCompute))); }, "specialization");
        const std::array<std::uint32_t, 1> code{0xbf810000u};
        ShaderRecompiler::RecompileRequest request{};
        request.shader = {ShaderRecompiler::ShaderStage::Compute, 0x10000, code, 0, {}};
        request.context.waveSize = 32;
        request.context.compute = ShaderRecompiler::ShaderComputeStageInfo{{32, 1, 1}, 0, {false, false, false}, false, 1};
        request.target = RecompileTarget();
        request.layout.pushConstantSizeBytes = 128;
        request.useCache = false;
        const auto rdna = Translate(ShaderRecompiler::Recompile(request).spirv.Words());
        Require(rdna.stage == Stage::Compute && rdna.workgroupSize[0] == 32, "RDNA to Metal pipeline failed");
        if (argc == 2) {
            const std::filesystem::path directory(argv[1]);
            const auto words = CompileGlsl(ComputeSource, EShLangCompute);
            std::ofstream binary(directory / "compute.spv", std::ios::binary);
            binary.write(reinterpret_cast<const char*>(words.data()), static_cast<std::streamsize>(words.size() * sizeof(words[0])));
            binary.close();
            Require(static_cast<bool>(binary), "Could not write SPIR-V fixture");
            std::ofstream captured(directory / "fragment.req", std::ios::binary);
            captured << ShaderRecompiler::RequestSerializer{}.Serialize(RdnaFragmentRequest());
            captured.close();
            Require(static_cast<bool>(captured), "Could not write RDNA fixture");
        }
        std::puts("Metal translation, binding reflection and unsupported-feature rejection passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
