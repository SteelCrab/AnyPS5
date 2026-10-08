#include "MetalShader.hpp"
#include <spirv_msl.hpp>
#include <algorithm>
#include <stdexcept>

namespace AnyPS5::Metal {

Shader Translate(std::span<const std::uint32_t> spirv) {
    if (spirv.size() < 5 || spirv[0] != 0x07230203u) {
        throw std::runtime_error("Metal shader: invalid SPIR-V header");
    }
    spirv_cross::CompilerMSL compiler(spirv.data(), spirv.size());
    const auto entries = compiler.get_entry_points_and_stages();
    if (entries.size() != 1) throw std::runtime_error("Metal shader: exactly one entry point is required");
    const auto& entry = entries.front();
    Shader result{};
    switch (entry.execution_model) {
    case spv::ExecutionModelGLCompute: result.stage = Stage::Compute; break;
    case spv::ExecutionModelVertex: result.stage = Stage::Vertex; break;
    case spv::ExecutionModelFragment: result.stage = Stage::Fragment; break;
    default: throw std::runtime_error("Metal shader: this shader stage is not implemented");
    }
    compiler.set_entry_point(entry.name, entry.execution_model);
    const auto& capabilities = compiler.get_declared_capabilities();
    for (const auto capability : capabilities) {
        switch (capability) {
        case spv::CapabilityFloat64:
        case spv::CapabilityInt64Atomics:
        case spv::CapabilityInt64ImageEXT:
            throw std::runtime_error("Metal shader: 64-bit floating point or atomics are not supported");
        default: break;
        }
    }
    if (!compiler.get_specialization_constants().empty()) {
        throw std::runtime_error("Metal shader: specialization constants must be resolved before translation");
    }
    if (result.stage == Stage::Compute) {
        for (std::uint32_t axis = 0; axis < 3; ++axis) {
            result.workgroupSize[axis] = compiler.get_execution_mode_argument(spv::ExecutionModeLocalSize, axis);
            if (result.workgroupSize[axis] == 0) throw std::runtime_error("Metal shader: invalid compute workgroup size");
        }
    }
    spirv_cross::CompilerMSL::Options options;
    options.platform = spirv_cross::CompilerMSL::Options::macOS;
    options.set_msl_version(3, 0);
    options.fixed_subgroup_size = 32;
    options.texture_buffer_native = true;
    compiler.set_msl_options(options);
    const auto active = compiler.get_active_interface_variables();
    const auto resources = compiler.get_shader_resources(active);
    compiler.set_enabled_interface_variables(active);
    if (!resources.sampled_images.empty() || !resources.subpass_inputs.empty() ||
        !resources.atomic_counters.empty() || !resources.acceleration_structures.empty()) {
        throw std::runtime_error("Metal shader: combined images, subpasses, counters or acceleration structures need explicit lowering");
    }
    result.stageInputs = !resources.stage_inputs.empty();
    struct ResourceInfo {
        ResourceKind kind;
        std::uint32_t set;
        std::uint32_t binding;
        std::uint32_t index;
        std::size_t bytes;
    };
    std::vector<ResourceInfo> pending;
    const auto collect = [&](const auto& list, ResourceKind kind) {
        for (const auto& resource : list) {
            if (!compiler.get_type(resource.type_id).array.empty()) {
                throw std::runtime_error("Metal shader: descriptor arrays need Metal argument buffer lowering");
            }
            if (kind == ResourceKind::Texture) {
                const auto& image = compiler.get_type(resource.base_type_id).image;
                const auto& sample = compiler.get_type(image.type);
                if (image.dim != spv::Dim2D || image.arrayed || image.ms || image.depth ||
                    sample.basetype != spirv_cross::SPIRType::Float || sample.width != 32 ||
                    (image.format != spv::ImageFormatUnknown && image.format != spv::ImageFormatRgba8)) {
                    throw std::runtime_error("Metal shader: only non-array 2D float textures and RGBA8 storage images are implemented");
                }
            }
            const bool push = kind == ResourceKind::PushConstants;
            if (!push && (!compiler.has_decoration(resource.id, spv::DecorationDescriptorSet) ||
                          !compiler.has_decoration(resource.id, spv::DecorationBinding))) {
                throw std::runtime_error("Metal shader: resource is missing its descriptor set or binding");
            }
            const auto set = push ? spirv_cross::kPushConstDescSet : compiler.get_decoration(resource.id, spv::DecorationDescriptorSet);
            const auto binding = push ? spirv_cross::kPushConstBinding : compiler.get_decoration(resource.id, spv::DecorationBinding);
            const auto bytes = kind == ResourceKind::Buffer || push ? compiler.get_declared_struct_size(compiler.get_type(resource.base_type_id)) : 0;
            pending.push_back({kind, set, binding, 0, bytes});
        }
    };
    collect(resources.uniform_buffers, ResourceKind::Buffer);
    collect(resources.storage_buffers, ResourceKind::Buffer);
    collect(resources.push_constant_buffers, ResourceKind::PushConstants);
    collect(resources.separate_images, ResourceKind::Texture);
    collect(resources.storage_images, ResourceKind::Texture);
    collect(resources.separate_samplers, ResourceKind::Sampler);
    std::sort(pending.begin(), pending.end(), [](const auto& left, const auto& right) {
        if (left.set != right.set) return left.set < right.set;
        return left.binding < right.binding;
    });
    std::uint32_t bufferIndex = 0, textureIndex = 0, samplerIndex = 0;
    for (std::size_t i = 1; i < pending.size(); ++i) {
        if (pending[i].set == pending[i - 1].set && pending[i].binding == pending[i - 1].binding) {
            throw std::runtime_error("Metal shader: resource descriptor binding is duplicated");
        }
    }
    for (auto& resource : pending) {
        spirv_cross::MSLResourceBinding mapping{};
        mapping.stage = entry.execution_model;
        mapping.desc_set = resource.set;
        mapping.binding = resource.binding;
        switch (resource.kind) {
        case ResourceKind::Buffer:
        case ResourceKind::PushConstants:
            resource.index = bufferIndex++;
            mapping.msl_buffer = resource.index;
            if (bufferIndex > 31) throw std::runtime_error("Metal shader: too many buffer bindings");
            break;
        case ResourceKind::Texture: resource.index = textureIndex++; mapping.msl_texture = resource.index; break;
        case ResourceKind::Sampler: resource.index = samplerIndex++; mapping.msl_sampler = resource.index; break;
        }
        compiler.add_msl_resource_binding(mapping);
    }
    result.source = compiler.compile();
    result.entryPoint = compiler.get_cleansed_entry_point_name(entry.name, entry.execution_model);
    if (compiler.needs_buffer_size_buffer() || compiler.needs_swizzle_buffer() || compiler.needs_view_mask_buffer() ||
        compiler.needs_dispatch_base_buffer() || compiler.needs_output_buffer() || compiler.needs_patch_output_buffer()) {
        throw std::runtime_error("Metal shader: auxiliary shader buffers are not implemented");
    }
    for (const auto& resource : pending) {
        if (compiler.is_msl_resource_binding_used(entry.execution_model, resource.set, resource.binding)) {
            result.resources.push_back({resource.kind, resource.set, resource.binding, resource.index, resource.bytes});
        }
    }
    return result;
}

ShaderRecompiler::SpirvTarget RecompileTarget() {
    static constexpr std::uint32_t capabilities[]{
        spv::CapabilityShader, spv::CapabilityInt64, spv::CapabilityGroupNonUniform,
        spv::CapabilityGroupNonUniformBallot, spv::CapabilityGroupNonUniformShuffle,
        spv::CapabilityGroupNonUniformArithmetic, spv::CapabilityImageQuery,
        spv::CapabilityStorageImageReadWithoutFormat, spv::CapabilityStorageImageWriteWithoutFormat
    };
    ShaderRecompiler::SpirvTarget target{};
    target.vulkanVersion = 0x00401000u;
    target.spirvVersion = 0x00010300u;
    target.subgroupSize = 32;
    target.supportedCapabilities = capabilities;
    target.maxWorkgroupSize = {1024, 1024, 64};
    target.maxWorkgroupInvocations = 1024;
    target.maxWorkgroupSharedMemoryBytes = 32768;
    return target;
}

}
