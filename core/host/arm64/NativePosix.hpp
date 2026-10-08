#pragma once

#include "NativeLibc.hpp"
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace AnyPS5::Arm64 {

[[nodiscard]] int guestError(int hostError) noexcept;

class NativePosix {
public:
    explicit NativePosix(NativeLibc& libc);
    ~NativePosix();
    NativePosix(const NativePosix&) = delete;
    NativePosix& operator=(const NativePosix&) = delete;
    [[nodiscard]] static std::uint64_t Resolve(const ElfImport& symbol);
    void Bind(Cpu& cpu);
    void SetArguments(GuestMemory& memory, const std::vector<std::string>& arguments);
    void Shutdown();
    [[nodiscard]] std::optional<std::string> GetEnvironment(GuestMemory& memory, std::string_view name);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    std::uint64_t invoke(const HostCall& call, std::size_t operation);
};

}
