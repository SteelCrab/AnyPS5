#pragma once

#include "MetalShader.hpp"
#include <memory>

namespace AnyPS5::Metal {

class Buffer {
public:
    [[nodiscard]] std::size_t Size() const;
    void Write(std::size_t offset, std::span<const std::byte> bytes);
    void Read(std::size_t offset, std::span<std::byte> bytes) const;
private:
    struct Impl;
    std::shared_ptr<Impl> impl;
    explicit Buffer(std::shared_ptr<Impl> impl);
    friend class Device;
};

class ComputePipeline {
private:
    struct Impl;
    std::shared_ptr<Impl> impl;
    explicit ComputePipeline(std::shared_ptr<Impl> impl);
    friend class Device;
};

class Texture {
public:
    [[nodiscard]] std::uint32_t Width() const;
    [[nodiscard]] std::uint32_t Height() const;
    void Write(std::span<const std::byte> rgba);
    void Read(std::span<std::byte> rgba) const;
private:
    struct Impl;
    std::shared_ptr<Impl> impl;
    explicit Texture(std::shared_ptr<Impl> impl);
    friend class Device;
};

enum class Filter { Nearest, Linear };
enum class AddressMode { ClampToEdge, Repeat, MirrorRepeat };

class Sampler {
private:
    struct Impl;
    std::shared_ptr<Impl> impl;
    explicit Sampler(std::shared_ptr<Impl> impl);
    friend class Device;
};

struct BufferBinding {
    std::uint32_t metalIndex;
    Buffer buffer;
    std::size_t offset = 0;
};

struct TextureBinding {
    std::uint32_t metalIndex;
    Texture texture;
};

struct SamplerBinding {
    std::uint32_t metalIndex;
    Sampler sampler;
};

struct ResourceBindings {
    std::span<const BufferBinding> buffers;
    std::span<const TextureBinding> textures;
    std::span<const SamplerBinding> samplers;
};

class Device {
public:
    Device();
    ~Device();
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    [[nodiscard]] std::string Name() const;
    [[nodiscard]] Buffer CreateBuffer(std::size_t size);
    [[nodiscard]] Texture CreateTexture(std::uint32_t width, std::uint32_t height);
    [[nodiscard]] Sampler CreateSampler(Filter filter = Filter::Nearest, AddressMode address = AddressMode::ClampToEdge);
    void Validate(const Shader& shader) const;
    [[nodiscard]] ComputePipeline CreateComputePipeline(const Shader& shader) const;
    void Dispatch(const ComputePipeline& pipeline, std::span<const BufferBinding> buffers,
                  std::array<std::uint32_t, 3> groups, std::span<const TextureBinding> textures = {},
                  std::span<const SamplerBinding> samplers = {});
    [[nodiscard]] std::vector<std::byte> Render(const Shader& vertex, const Shader& fragment,
                                               std::uint32_t width, std::uint32_t height,
                                               ResourceBindings vertexResources = {}, ResourceBindings fragmentResources = {});
private:
    void validateResources(const Shader& shader, ResourceBindings resources) const;
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}
