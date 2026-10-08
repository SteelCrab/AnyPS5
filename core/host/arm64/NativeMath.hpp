#pragma once

#include "NativeLibc.hpp"

namespace AnyPS5::Arm64 {

class NativeMath {
public:
    explicit NativeMath(NativeLibc& libc) : libc(libc) {}
    [[nodiscard]] static std::uint64_t Resolve(const ElfImport& symbol);
    void Bind(Cpu& cpu);
private:
    NativeLibc& libc;
};

}
