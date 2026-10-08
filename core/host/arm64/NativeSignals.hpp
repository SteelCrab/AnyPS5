#pragma once

#include "NativeLibc.hpp"

namespace AnyPS5::Arm64 {

class GuestSignalExit final : public std::runtime_error {
public:
    explicit GuestSignalExit(int signal);
    [[nodiscard]] int Signal() const;
    [[nodiscard]] int Status() const;
private:
    int signal;
};

class NativeSignals {
public:
    explicit NativeSignals(NativeLibc& libc);
    ~NativeSignals();
    NativeSignals(const NativeSignals&) = delete;
    NativeSignals& operator=(const NativeSignals&) = delete;
    [[nodiscard]] static std::uint64_t Resolve(const ElfImport& symbol);
    void RegisterThread(std::uint64_t thread, std::uint64_t parent = 0);
    void UnregisterThread(std::uint64_t thread);
    void Bind(Cpu& cpu, std::uint64_t thread);
    void QueueProcess(int signal);
    void Abort(const HostCall& call);
    void Shutdown();
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    std::uint64_t invoke(const HostCall& call, std::uint64_t thread, std::size_t operation);
    void poll(Cpu& cpu, CpuState& state, std::uint64_t thread);
};

}
