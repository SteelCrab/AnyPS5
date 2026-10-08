#include "MetalDevice.hpp"
#include "ControlFlow/RequestSerializer.hpp"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: agc_metal_shader <shader.spv|shader.req> <output.metal>\n");
        return 1;
    }
    try {
        std::ifstream input(argv[1], std::ios::binary);
        if (!input) throw std::runtime_error("Cannot open input shader");
        const std::string bytes{std::istreambuf_iterator<char>(input), {}};
        std::vector<std::uint32_t> words;
        if (std::string_view(argv[1]).ends_with(".req")) {
            auto captured = ShaderRecompiler::RequestSerializer{}.Deserialize(bytes);
            captured.request.target = AnyPS5::Metal::RecompileTarget();
            captured.request.useCache = false;
            words = ShaderRecompiler::Recompile(captured.request).spirv.Words();
        } else {
            if (bytes.size() % sizeof(std::uint32_t) != 0) throw std::runtime_error("SPIR-V file size is not a multiple of four bytes");
            words.resize(bytes.size() / sizeof(std::uint32_t));
            if (!bytes.empty()) std::memcpy(words.data(), bytes.data(), bytes.size());
        }
        const auto shader = AnyPS5::Metal::Translate(words);
        AnyPS5::Metal::Device device;
        if (shader.stage == AnyPS5::Metal::Stage::Compute) {
            static_cast<void>(device.CreateComputePipeline(shader));
        } else {
            device.Validate(shader);
        }
        std::ofstream output(argv[2], std::ios::binary | std::ios::trunc);
        output.write(shader.source.data(), static_cast<std::streamsize>(shader.source.size()));
        output.close();
        if (!output) throw std::runtime_error("Cannot write Metal source");
        std::printf("%s: validated Metal shader %s on %s, %zu resource bindings\n", argv[1], shader.entryPoint.c_str(), device.Name().c_str(), shader.resources.size());
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 2;
    }
}
