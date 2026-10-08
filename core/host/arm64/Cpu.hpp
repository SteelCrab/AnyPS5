#pragma once

#include "GuestMemory.hpp"
#include <array>
#include <memory>

namespace AnyPS5::Arm64 {

struct CpuState {
    std::array<std::uint64_t, 16> registers{};
    std::uint64_t rip = 0;
    std::uint64_t rflags = 2;
    GuestMemory* memory = nullptr;
    std::uint64_t fault = 0;
    std::uint64_t faultAddress = 0;
};

struct HostCall {
    std::array<std::uint64_t, 6> arguments;
    std::uint64_t stackArguments;
    GuestMemory& memory;
};

using HostFunction = std::uint64_t (*)(const HostCall& call, void* context);

class Cpu {
public:
    explicit Cpu(GuestMemory& memory);
    ~Cpu();
    Cpu(const Cpu&) = delete;
    Cpu& operator=(const Cpu&) = delete;
    void Run(CpuState& state, std::uint64_t instructionBudget = 1000000);
    void Bind(std::uint64_t guestAddress, HostFunction function, void* context = nullptr);
    [[nodiscard]] std::size_t CompiledBlocks() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}
