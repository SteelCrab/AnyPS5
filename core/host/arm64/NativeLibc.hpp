#pragma once

#include "Cpu.hpp"
#include "ElfImage.hpp"
#include <cstdio>
#include <string_view>
#include <unordered_map>
#include <mutex>
#include <thread>

namespace AnyPS5::Arm64 {

class NativeLibc {
public:
    NativeLibc() = default;
    ~NativeLibc();
    NativeLibc(const NativeLibc&) = delete;
    NativeLibc& operator=(const NativeLibc&) = delete;
    [[nodiscard]] std::uint64_t Resolve(const ElfImport& symbol) const;
    void Bind(Cpu& cpu, GuestMemory& memory);
    void Attach(Cpu& cpu, GuestMemory& memory);
    [[nodiscard]] std::uint64_t Invoke(const HostCall& call, HostFunction function);
    [[nodiscard]] std::uint64_t ErrorAddress(GuestMemory& memory) const;
    [[nodiscard]] std::uint64_t Allocate(GuestMemory& memory, std::size_t size);
    void Free(GuestMemory& memory, std::uint64_t address);
    [[nodiscard]] std::uint64_t Resize(GuestMemory& memory, std::uint64_t address, std::size_t size);
    void SetError(GuestMemory& memory, int error) const;
    [[nodiscard]] std::uint64_t Open(GuestMemory& memory, const char* path, const char* mode);
    [[nodiscard]] int Close(GuestMemory& memory, std::uint64_t address);
    [[nodiscard]] std::FILE* File(std::uint64_t address) const;
    void SyncFile(GuestMemory& memory, std::uint64_t address) const;
private:
    struct Stream {
        std::FILE* file;
        bool owned;
    };
    std::unordered_map<std::uint64_t, Stream> streams;
    std::unordered_map<std::uint64_t, std::size_t> allocations;
    std::uint64_t nextAllocation = 0x10000000000;
    std::uint64_t nextStream = 0x730000000;
    bool bound = false;
    GuestMemory* guestMemory = nullptr;
    mutable std::recursive_mutex mutex;
    mutable std::unordered_map<std::thread::id, std::uint64_t> errorCells;
    mutable std::uint64_t nextError = 0x710001000;
};

}
