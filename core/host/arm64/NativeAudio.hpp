#pragma once

#include "Cpu.hpp"
#include "ElfImage.hpp"

namespace AnyPS5::Arm64 {

class NativeAudio {
public:
    NativeAudio();
    ~NativeAudio();
    NativeAudio(const NativeAudio&) = delete;
    NativeAudio& operator=(const NativeAudio&) = delete;
    [[nodiscard]] static std::uint64_t Resolve(const ElfImport& symbol);
    void Bind(Cpu& cpu);
    void Shutdown();
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    std::uint64_t invoke(const HostCall& call, std::size_t operation);
};

}
