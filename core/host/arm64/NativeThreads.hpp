#pragma once

#include "NativeLibc.hpp"
#include <functional>

namespace AnyPS5::Arm64 {

class NativeSignals;

class NativeThreads {
public:
    NativeThreads(NativeLibc& libc, GuestMemory& memory, std::uint64_t instructionBudget = 1000000);
    ~NativeThreads();
    NativeThreads(const NativeThreads&) = delete;
    NativeThreads& operator=(const NativeThreads&) = delete;
    [[nodiscard]] static std::uint64_t Resolve(const ElfImport& symbol);
    void Bind(Cpu& cpu);
    void SetBindings(std::function<void(Cpu&)> attach, std::function<void()> shutdown = {});
    void SetSignals(NativeSignals& signals);
    void Finish();
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    std::uint64_t invoke(const HostCall& call, std::size_t operation);
};

}
