#include "MetalDevice.hpp"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace AnyPS5::Metal {

namespace {

std::runtime_error metalError(const char* operation, NSError* error) {
    return std::runtime_error(std::string(operation) + ": " + (error ? error.localizedDescription.UTF8String : "Metal returned no object"));
}

id<MTLFunction> compileFunction(id<MTLDevice> device, const Shader& shader) {
    @autoreleasepool {
        NSString* source = [[NSString alloc] initWithBytes:shader.source.data() length:shader.source.size() encoding:NSUTF8StringEncoding];
        NSString* entry = [[NSString alloc] initWithBytes:shader.entryPoint.data() length:shader.entryPoint.size() encoding:NSUTF8StringEncoding];
        if (!source || !entry) throw std::runtime_error("Metal shader source or entry point is not UTF-8");
        MTLCompileOptions* options = [MTLCompileOptions new];
        options.languageVersion = MTLLanguageVersion3_0;
        options.mathMode = MTLMathModeSafe;
        options.mathFloatingPointFunctions = MTLMathFloatingPointFunctionsPrecise;
        NSError* error = nil;
        id<MTLLibrary> library = [device newLibraryWithSource:source options:options error:&error];
        if (!library) throw metalError("Compile Metal shader", error);
        id<MTLFunction> function = [library newFunctionWithName:entry];
        if (!function) throw std::runtime_error("Metal shader entry point was not found: " + shader.entryPoint);
        const auto expected = shader.stage == Stage::Compute ? MTLFunctionTypeKernel :
            shader.stage == Stage::Vertex ? MTLFunctionTypeVertex : MTLFunctionTypeFragment;
        if (function.functionType != expected) throw std::runtime_error("Metal shader entry point has a different stage");
        return function;
    }
}

void finish(id<MTLCommandBuffer> command) {
    [command commit];
    [command waitUntilCompleted];
    if (command.status != MTLCommandBufferStatusCompleted) throw metalError("Execute Metal command buffer", command.error);
}

void checkRange(std::size_t size, std::size_t offset, std::size_t count) {
    if (offset > size || count > size - offset) throw std::runtime_error("Metal buffer access is out of bounds");
}

template<typename TBinding>
const TBinding& binding(std::span<const TBinding> bindings, std::uint32_t index) {
    const TBinding* result = nullptr;
    for (const auto& candidate : bindings) {
        if (candidate.metalIndex != index) continue;
        if (result) throw std::runtime_error("Metal resource binding is duplicated");
        result = &candidate;
    }
    if (!result) throw std::runtime_error("Metal resource binding is missing");
    return *result;
}

}

struct Buffer::Impl {
    id<MTLBuffer> buffer;
};

struct ComputePipeline::Impl {
    id<MTLComputePipelineState> pipeline;
    Shader shader;
};

struct Texture::Impl {
    id<MTLTexture> texture;
};

struct Sampler::Impl {
    id<MTLSamplerState> sampler;
};

struct Device::Impl {
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
};

Buffer::Buffer(std::shared_ptr<Impl> impl) : impl(std::move(impl)) {}

std::size_t Buffer::Size() const {
    if (!impl) throw std::runtime_error("Metal buffer was moved from");
    return impl->buffer.length;
}

void Buffer::Write(std::size_t offset, std::span<const std::byte> bytes) {
    checkRange(Size(), offset, bytes.size());
    if (!bytes.empty()) std::memcpy(static_cast<std::byte*>(impl->buffer.contents) + offset, bytes.data(), bytes.size());
}

void Buffer::Read(std::size_t offset, std::span<std::byte> bytes) const {
    checkRange(Size(), offset, bytes.size());
    if (!bytes.empty()) std::memcpy(bytes.data(), static_cast<const std::byte*>(impl->buffer.contents) + offset, bytes.size());
}

ComputePipeline::ComputePipeline(std::shared_ptr<Impl> impl) : impl(std::move(impl)) {}

Texture::Texture(std::shared_ptr<Impl> impl) : impl(std::move(impl)) {}

std::uint32_t Texture::Width() const {
    if (!impl) throw std::runtime_error("Metal texture was moved from");
    return static_cast<std::uint32_t>(impl->texture.width);
}

std::uint32_t Texture::Height() const {
    if (!impl) throw std::runtime_error("Metal texture was moved from");
    return static_cast<std::uint32_t>(impl->texture.height);
}

void Texture::Write(std::span<const std::byte> rgba) {
    const auto width = Width(), height = Height();
    if (rgba.size() != static_cast<std::size_t>(width) * height * 4) throw std::runtime_error("Metal texture upload has the wrong byte count");
    [impl->texture replaceRegion:MTLRegionMake2D(0, 0, width, height) mipmapLevel:0 withBytes:rgba.data() bytesPerRow:width * 4];
}

void Texture::Read(std::span<std::byte> rgba) const {
    const auto width = Width(), height = Height();
    if (rgba.size() != static_cast<std::size_t>(width) * height * 4) throw std::runtime_error("Metal texture readback has the wrong byte count");
    [impl->texture getBytes:rgba.data() bytesPerRow:width * 4 fromRegion:MTLRegionMake2D(0, 0, width, height) mipmapLevel:0];
}

Sampler::Sampler(std::shared_ptr<Impl> impl) : impl(std::move(impl)) {}

Device::Device() : impl(std::make_unique<Impl>()) {
    @autoreleasepool {
#if !defined(__aarch64__)
        throw std::runtime_error("The Metal backend requires native ARM64 execution");
#endif
        impl->device = MTLCreateSystemDefaultDevice();
        if (!impl->device) throw std::runtime_error("No native Metal GPU is available");
        if (![impl->device supportsFamily:MTLGPUFamilyApple7] || !impl->device.hasUnifiedMemory) {
            throw std::runtime_error("The Metal backend requires an Apple Silicon GPU with unified memory");
        }
        impl->queue = [impl->device newCommandQueue];
        if (!impl->queue) throw std::runtime_error("Metal command queue creation failed");
    }
}

Device::~Device() = default;

std::string Device::Name() const {
    return impl->device.name.UTF8String;
}

Buffer Device::CreateBuffer(std::size_t size) {
    @autoreleasepool {
        if (size == 0 || size > impl->device.maxBufferLength) throw std::runtime_error("Metal buffer size is outside device limits");
        auto buffer = std::make_shared<Buffer::Impl>();
        buffer->buffer = [impl->device newBufferWithLength:size options:MTLResourceStorageModeShared];
        if (!buffer->buffer) throw std::runtime_error("Metal buffer allocation failed");
        std::memset(buffer->buffer.contents, 0, size);
        return Buffer(std::move(buffer));
    }
}

Texture Device::CreateTexture(std::uint32_t width, std::uint32_t height) {
    @autoreleasepool {
        if (width == 0 || height == 0 || width > 16384 || height > 16384) throw std::runtime_error("Metal texture dimensions are outside device limits");
        MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:width height:height mipmapped:NO];
        descriptor.storageMode = MTLStorageModeShared;
        descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
        auto texture = std::make_shared<Texture::Impl>();
        texture->texture = [impl->device newTextureWithDescriptor:descriptor];
        if (!texture->texture) throw std::runtime_error("Metal texture allocation failed");
        const std::vector<std::byte> zeros(static_cast<std::size_t>(width) * height * 4);
        Texture result(std::move(texture));
        result.Write(zeros);
        return result;
    }
}

Sampler Device::CreateSampler(Filter filter, AddressMode address) {
    @autoreleasepool {
        MTLSamplerMinMagFilter nativeFilter;
        switch (filter) {
        case Filter::Nearest: nativeFilter = MTLSamplerMinMagFilterNearest; break;
        case Filter::Linear: nativeFilter = MTLSamplerMinMagFilterLinear; break;
        default: throw std::runtime_error("Metal sampler filter is invalid");
        }
        MTLSamplerAddressMode nativeAddress;
        switch (address) {
        case AddressMode::ClampToEdge: nativeAddress = MTLSamplerAddressModeClampToEdge; break;
        case AddressMode::Repeat: nativeAddress = MTLSamplerAddressModeRepeat; break;
        case AddressMode::MirrorRepeat: nativeAddress = MTLSamplerAddressModeMirrorRepeat; break;
        default: throw std::runtime_error("Metal sampler address mode is invalid");
        }
        MTLSamplerDescriptor* descriptor = [MTLSamplerDescriptor new];
        descriptor.minFilter = nativeFilter;
        descriptor.magFilter = nativeFilter;
        descriptor.mipFilter = MTLSamplerMipFilterNotMipmapped;
        descriptor.sAddressMode = nativeAddress;
        descriptor.tAddressMode = nativeAddress;
        descriptor.normalizedCoordinates = YES;
        auto sampler = std::make_shared<Sampler::Impl>();
        sampler->sampler = [impl->device newSamplerStateWithDescriptor:descriptor];
        if (!sampler->sampler) throw std::runtime_error("Metal sampler allocation failed");
        return Sampler(std::move(sampler));
    }
}

void Device::Validate(const Shader& shader) const {
    static_cast<void>(compileFunction(impl->device, shader));
}

ComputePipeline Device::CreateComputePipeline(const Shader& shader) const {
    @autoreleasepool {
        if (shader.stage != Stage::Compute) throw std::runtime_error("Metal compute pipeline requires a compute shader");
        std::uint64_t threads = 1;
        const auto maximum = impl->device.maxThreadsPerThreadgroup;
        const std::array<std::size_t, 3> limits{maximum.width, maximum.height, maximum.depth};
        for (std::size_t axis = 0; axis < 3; ++axis) {
            if (shader.workgroupSize[axis] == 0 || shader.workgroupSize[axis] > limits[axis]) {
                throw std::runtime_error("Metal compute workgroup dimension is outside device limits");
            }
            threads *= shader.workgroupSize[axis];
        }
        auto pipeline = std::make_shared<ComputePipeline::Impl>();
        pipeline->shader = shader;
        NSError* error = nil;
        pipeline->pipeline = [impl->device newComputePipelineStateWithFunction:compileFunction(impl->device, shader) error:&error];
        if (!pipeline->pipeline) throw metalError("Create Metal compute pipeline", error);
        if (threads > pipeline->pipeline.maxTotalThreadsPerThreadgroup) throw std::runtime_error("Metal compute workgroup exceeds pipeline thread limit");
        if (pipeline->pipeline.threadExecutionWidth != 32) throw std::runtime_error("Metal compute pipeline does not have the required 32-lane SIMD group");
        return ComputePipeline(std::move(pipeline));
    }
}

void Device::validateResources(const Shader& shader, ResourceBindings resources) const {
    if (resources.buffers.size() + resources.textures.size() + resources.samplers.size() != shader.resources.size()) throw std::runtime_error("Metal resource bindings do not match the shader");
    for (const auto& resource : shader.resources) {
        switch (resource.kind) {
        case ResourceKind::Buffer:
        case ResourceKind::PushConstants: {
            const auto& value = binding(resources.buffers, resource.metalIndex);
            checkRange(value.buffer.Size(), value.offset, std::max<std::size_t>(resource.minimumBytes, 1));
            if (value.offset % 16 != 0) throw std::runtime_error("Metal buffer offset must be aligned to 16 bytes");
            if (value.buffer.impl->buffer.device != impl->device) throw std::runtime_error("Metal buffer belongs to another GPU");
            break;
        }
        case ResourceKind::Texture: {
            const auto& value = binding(resources.textures, resource.metalIndex).texture;
            static_cast<void>(value.Width());
            if (value.impl->texture.device != impl->device) throw std::runtime_error("Metal texture belongs to another GPU");
            break;
        }
        case ResourceKind::Sampler: {
            const auto& value = binding(resources.samplers, resource.metalIndex).sampler;
            if (!value.impl) throw std::runtime_error("Metal sampler was moved from");
            if (value.impl->sampler.device != impl->device) throw std::runtime_error("Metal sampler belongs to another GPU");
            break;
        }
        default: throw std::runtime_error("Metal shader resource kind is invalid");
        }
    }
}

void Device::Dispatch(const ComputePipeline& pipeline, std::span<const BufferBinding> buffers,
                      std::array<std::uint32_t, 3> groups, std::span<const TextureBinding> textures,
                      std::span<const SamplerBinding> samplers) {
    @autoreleasepool {
        if (!pipeline.impl) throw std::runtime_error("Metal compute pipeline was moved from");
        if (pipeline.impl->pipeline.device != impl->device) throw std::runtime_error("Metal compute pipeline belongs to another GPU");
        for (std::size_t axis = 0; axis < 3; ++axis) {
            if (groups[axis] == 0 || groups[axis] > std::numeric_limits<std::uint32_t>::max() / pipeline.impl->shader.workgroupSize[axis]) {
                throw std::runtime_error("Metal dispatch dimensions are zero or overflow the shader invocation ID");
            }
        }
        validateResources(pipeline.impl->shader, {buffers, textures, samplers});
        id<MTLCommandBuffer> command = [impl->queue commandBuffer];
        if (!command) throw std::runtime_error("Metal command buffer creation failed");
        id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
        if (!encoder) throw std::runtime_error("Metal compute encoder creation failed");
        [encoder setComputePipelineState:pipeline.impl->pipeline];
        for (const auto& binding : buffers) [encoder setBuffer:binding.buffer.impl->buffer offset:binding.offset atIndex:binding.metalIndex];
        for (const auto& binding : textures) [encoder setTexture:binding.texture.impl->texture atIndex:binding.metalIndex];
        for (const auto& binding : samplers) [encoder setSamplerState:binding.sampler.impl->sampler atIndex:binding.metalIndex];
        const auto& size = pipeline.impl->shader.workgroupSize;
        [encoder dispatchThreadgroups:MTLSizeMake(groups[0], groups[1], groups[2]) threadsPerThreadgroup:MTLSizeMake(size[0], size[1], size[2])];
        [encoder endEncoding];
        finish(command);
    }
}

std::vector<std::byte> Device::Render(const Shader& vertex, const Shader& fragment, std::uint32_t width, std::uint32_t height,
                                    ResourceBindings vertexResources, ResourceBindings fragmentResources) {
    @autoreleasepool {
        if (vertex.stage != Stage::Vertex || fragment.stage != Stage::Fragment) throw std::runtime_error("Metal rendering requires vertex and fragment shaders");
        if (vertex.stageInputs) throw std::runtime_error("Metal offscreen rendering with vertex attributes is not implemented");
        validateResources(vertex, vertexResources);
        validateResources(fragment, fragmentResources);
        if (width == 0 || height == 0 || width > 16384 || height > 16384) throw std::runtime_error("Metal render target dimensions are outside device limits");
        const std::size_t rowBytes = (static_cast<std::size_t>(width) * 4 + 255) & ~std::size_t(255);
        const std::size_t imageBytes = rowBytes * height;
        MTLTextureDescriptor* target = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:width height:height mipmapped:NO];
        target.usage = MTLTextureUsageRenderTarget;
        target.storageMode = MTLStorageModePrivate;
        id<MTLTexture> texture = [impl->device newTextureWithDescriptor:target];
        if (!texture) throw std::runtime_error("Metal render target allocation failed");
        NSError* error = nil;
        MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
        descriptor.vertexFunction = compileFunction(impl->device, vertex);
        descriptor.fragmentFunction = compileFunction(impl->device, fragment);
        descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        id<MTLRenderPipelineState> state = [impl->device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if (!state) throw metalError("Create Metal render pipeline", error);
        const auto readback = CreateBuffer(imageBytes);
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
        id<MTLCommandBuffer> command = [impl->queue commandBuffer];
        if (!command) throw std::runtime_error("Metal render command buffer creation failed");
        id<MTLRenderCommandEncoder> render = [command renderCommandEncoderWithDescriptor:pass];
        if (!render) throw std::runtime_error("Metal render encoder creation failed");
        [render setRenderPipelineState:state];
        for (const auto& binding : vertexResources.buffers) [render setVertexBuffer:binding.buffer.impl->buffer offset:binding.offset atIndex:binding.metalIndex];
        for (const auto& binding : vertexResources.textures) [render setVertexTexture:binding.texture.impl->texture atIndex:binding.metalIndex];
        for (const auto& binding : vertexResources.samplers) [render setVertexSamplerState:binding.sampler.impl->sampler atIndex:binding.metalIndex];
        for (const auto& binding : fragmentResources.buffers) [render setFragmentBuffer:binding.buffer.impl->buffer offset:binding.offset atIndex:binding.metalIndex];
        for (const auto& binding : fragmentResources.textures) [render setFragmentTexture:binding.texture.impl->texture atIndex:binding.metalIndex];
        for (const auto& binding : fragmentResources.samplers) [render setFragmentSamplerState:binding.sampler.impl->sampler atIndex:binding.metalIndex];
        [render drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [render endEncoding];
        id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
        if (!blit) throw std::runtime_error("Metal readback encoder creation failed");
        [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(width, height, 1)
                     toBuffer:readback.impl->buffer destinationOffset:0 destinationBytesPerRow:rowBytes destinationBytesPerImage:imageBytes];
        [blit endEncoding];
        finish(command);
        std::vector<std::byte> pixels(static_cast<std::size_t>(width) * height * 4);
        for (std::uint32_t row = 0; row < height; ++row) {
            readback.Read(row * rowBytes, std::span(pixels).subspan(static_cast<std::size_t>(row) * width * 4, static_cast<std::size_t>(width) * 4));
        }
        return pixels;
    }
}

}
