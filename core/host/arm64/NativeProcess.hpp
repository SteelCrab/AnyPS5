#pragma once

#include "NativeLibc.hpp"
#include <mutex>
#include <vector>

namespace AnyPS5::Arm64 {

class GuestExit final : public std::runtime_error {
public:
    explicit GuestExit(int status, bool aborted = false);
    [[nodiscard]] int Status() const;
    [[nodiscard]] bool Aborted() const;
private:
    int status;
    bool aborted;
};

class NativeProcess {
public:
    explicit NativeProcess(NativeLibc& libc);
    [[nodiscard]] static std::uint64_t Resolve(const ElfImport& symbol);
    [[nodiscard]] static std::uint64_t CompletionAddress();
    void Bind(Cpu& cpu);
    void SetFinalizers(std::vector<std::uint64_t> functions);
private:
    struct Callback {
        std::uint64_t function;
        std::uint64_t argument;
        std::uint64_t dso;
    };
    NativeLibc& libc;
    std::mutex mutex;
    std::vector<Callback> callbacks;
    std::vector<std::uint64_t> finalizers;
    std::size_t nextFinalizer = 0;
    bool finalizersSet = false;
    std::uint64_t invoke(const HostCall& call, std::size_t operation);
    void finalize(const HostCall& call, std::uint64_t dso);
    void finish(const HostCall& call);
};

}
