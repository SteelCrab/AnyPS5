#pragma once

#include "Recompiler.hpp"
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace AnyPS5::Metal {

enum class Stage { Compute, Vertex, Fragment };
enum class ResourceKind { Buffer, Texture, Sampler, PushConstants };

struct ShaderResource {
    ResourceKind kind;
    std::uint32_t descriptorSet;
    std::uint32_t binding;
    std::uint32_t metalIndex;
    std::size_t minimumBytes;
};

struct Shader {
    Stage stage;
    std::string source;
    std::string entryPoint;
    std::array<std::uint32_t, 3> workgroupSize{1, 1, 1};
    std::vector<ShaderResource> resources;
    bool stageInputs = false;
};

[[nodiscard]] Shader Translate(std::span<const std::uint32_t> spirv);
[[nodiscard]] ShaderRecompiler::SpirvTarget RecompileTarget();

}
