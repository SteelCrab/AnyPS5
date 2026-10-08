#pragma once

#include "GuestMemory.hpp"
#include <array>
#include <memory>
#include <stop_token>
#include <stdexcept>

namespace AnyPS5::Arm64 {

struct CpuState {
    std::array<std::uint64_t, 16> registers{};
    std::uint64_t rip = 0;
    std::uint64_t rflags = 2;
    GuestMemory* memory = nullptr;
    std::uint64_t fault = 0;
    std::uint64_t faultAddress = 0;
    std::uint64_t fsBase = 0;
    std::uint64_t gsBase = 0;
    alignas(16) std::array<std::array<std::byte, 16>, 16> xmm{};
    alignas(16) std::array<std::array<std::byte, 16>, 2> vectorScratch{};
};

class Cpu;

class ExecutionStopped final : public std::runtime_error {
public:
    ExecutionStopped() : std::runtime_error("ARM64 guest execution was stopped") {}
};

struct HostCall {
    std::array<std::uint64_t, 6> arguments;
    std::uint64_t stackArguments;
    GuestMemory& memory;
    CpuState& state;
    Cpu& cpu;
};

using HostFunction = std::uint64_t (*)(const HostCall& call, void* context);

class Cpu {
public:
    explicit Cpu(GuestMemory& memory);
    ~Cpu();
    Cpu(const Cpu&) = delete;
    Cpu& operator=(const Cpu&) = delete;
    std::uint64_t Run(CpuState& state, std::uint64_t instructionBudget = 1000000, std::stop_token stop = {});
    [[nodiscard]] std::uint64_t Invoke(const CpuState& caller, std::uint64_t address, std::array<std::uint64_t, 6> arguments = {});
    void Bind(std::uint64_t guestAddress, HostFunction function, void* context = nullptr);
    void SetObserver(void (*observer)(void*), void* context);
    [[nodiscard]] std::size_t CompiledBlocks() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}
