#pragma once

#include "Cpu.hpp"
#include "ElfImage.hpp"
#include "NativeKernel.hpp"
#include <array>
#include <memory>

namespace AnyPS5::Arm64 {

class NativeVideoOut {
public:
    explicit NativeVideoOut(NativeKernel& kernel);
    ~NativeVideoOut();
    NativeVideoOut(const NativeVideoOut&) = delete;
    NativeVideoOut& operator=(const NativeVideoOut&) = delete;
    [[nodiscard]] std::uint64_t Resolve(const ElfImport& symbol) const;
    void Bind(Cpu& cpu);
    [[nodiscard]] int Open(int user, int bus, int index, std::uint64_t parameter);
    [[nodiscard]] int Close(int handle);
    void Register(GuestMemory& memory, const HostCall& call);
    void Flip(GuestMemory& memory, int handle, int index, int mode, std::int64_t argument);
    void SetRate(int handle, int rate);
    [[nodiscard]] int AddFlipEvent(std::uint64_t queue, int handle, std::uint64_t userData);
    [[nodiscard]] int DeleteFlipEvent(std::uint64_t queue, int handle);
    [[nodiscard]] std::vector<std::byte> Readback(int handle) const;
    [[nodiscard]] std::array<std::uint32_t, 2> FrameSize(int handle) const;
    [[nodiscard]] std::uint64_t FlipCount(int handle) const;
    [[nodiscard]] std::int64_t FlipArgument(int handle) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}
