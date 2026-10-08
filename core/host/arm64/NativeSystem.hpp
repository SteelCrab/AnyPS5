#pragma once

#include "NativeLibc.hpp"

namespace AnyPS5::Arm64 {

class NativeSystem {
public:
    explicit NativeSystem(NativeLibc& libc);
    [[nodiscard]] static std::uint64_t Resolve(const ElfImport& symbol);
    void Bind(Cpu& cpu);
private:
    NativeLibc& libc;
    std::uint64_t invoke(const HostCall& call, bool named);
};

}
