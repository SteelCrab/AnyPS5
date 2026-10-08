#pragma once

#include "Cpu.hpp"
#include "ElfImage.hpp"
#include <memory>

namespace AnyPS5::Arm64 {

class NativeKernel {
public:
    NativeKernel();
    ~NativeKernel();
    NativeKernel(const NativeKernel&) = delete;
    NativeKernel& operator=(const NativeKernel&) = delete;
    [[nodiscard]] std::uint64_t Resolve(const ElfImport& symbol) const;
    void Bind(Cpu& cpu);
    [[nodiscard]] int AllocateDirect(const HostCall& call, bool mainMemory);
    [[nodiscard]] int MapDirect(const HostCall& call);
    [[nodiscard]] int ReleaseDirect(const HostCall& call);
    [[nodiscard]] int Unmap(const HostCall& call);
    [[nodiscard]] int CreateQueue(const HostCall& call);
    [[nodiscard]] int DeleteQueue(std::uint64_t handle);
    [[nodiscard]] int WaitQueue(const HostCall& call);
    [[nodiscard]] int AddFlip(std::uint64_t queue, int output, std::uint64_t userData);
    [[nodiscard]] int DeleteFlip(std::uint64_t queue, int output);
    void TriggerFlip(int output, std::int64_t argument);
    void CloseOutput(int output);
    void Shutdown();
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}
