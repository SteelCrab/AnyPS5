#pragma once

#include "NativeLibc.hpp"
#include <memory>

namespace AnyPS5::Arm64 {

class NativePosix;

class NativeLocale {
public:
    NativeLocale(NativeLibc& libc, NativePosix& posix);
    ~NativeLocale();
    NativeLocale(const NativeLocale&) = delete;
    NativeLocale& operator=(const NativeLocale&) = delete;
    [[nodiscard]] static std::uint64_t Resolve(const ElfImport& symbol);
    void Bind(Cpu& cpu, GuestMemory& memory);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    std::uint64_t invoke(const HostCall& call, std::size_t operation);
};

}
