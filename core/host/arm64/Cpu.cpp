#include "Cpu.hpp"
#include "Arm64Translator.hpp"
#include <libkern/OSCacheControl.h>
#include <sys/mman.h>
#include <pthread.h>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

namespace AnyPS5::Arm64 {

namespace {

using BlockFunction = void (*)(CpuState*);

class JitArena {
public:
    JitArena() {
        if (!pthread_jit_write_protect_supported_np()) throw std::runtime_error("Native ARM64 JIT write protection is unavailable");
        data = mmap(nullptr, capacity, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
        if (data == MAP_FAILED) throw std::runtime_error("Cannot allocate the native ARM64 JIT arena");
    }
    ~JitArena() { munmap(data, capacity); }
    BlockFunction Append(std::span<const std::uint32_t> words) {
        std::lock_guard lock(mutex);
        const auto size = words.size_bytes();
        if (size > capacity - used) throw std::runtime_error("Native ARM64 JIT arena is full");
        auto* destination = static_cast<std::byte*>(data) + used;
        pthread_jit_write_protect_np(0);
        std::memcpy(destination, words.data(), size);
        pthread_jit_write_protect_np(1);
        sys_icache_invalidate(destination, size);
        used += size;
        return reinterpret_cast<BlockFunction>(destination);
    }
private:
    static constexpr std::size_t capacity = 16 * 1024 * 1024;
    void* data;
    std::size_t used = 0;
    std::mutex mutex;
};

JitArena& arena() {
    static JitArena value;
    return value;
}

}

struct Cpu::Impl {
    struct Block {
        BlockFunction function;
        std::size_t instructions;
    };
    GuestMemory& memory;
    std::unordered_map<std::uint64_t, Block> blocks;
    struct Binding {
        HostFunction function;
        void* context;
    };
    std::unordered_map<std::uint64_t, Binding> functions;
    explicit Impl(GuestMemory& memory) : memory(memory) {}
};

Cpu::Cpu(GuestMemory& memory) : impl(std::make_unique<Impl>(memory)) {}
Cpu::~Cpu() = default;

void Cpu::Bind(std::uint64_t guestAddress, HostFunction function, void* context) {
    if (guestAddress == 0 || !function) throw std::runtime_error("Native guest function binding is invalid");
    if (!impl->memory.Fetch(guestAddress, 1).empty()) throw std::runtime_error("Native guest function binding overlaps executable guest memory");
    if (!impl->functions.emplace(guestAddress, Impl::Binding{function, context}).second) throw std::runtime_error("Native guest function binding is duplicated");
}

void Cpu::Run(CpuState& state, std::uint64_t instructionBudget) {
    state.memory = &impl->memory;
    state.fault = 0;
    while (state.rip != 0) {
        if (instructionBudget == 0) throw std::runtime_error("ARM64 guest instruction budget exceeded");
        const auto function = impl->functions.find(state.rip);
        if (function != impl->functions.end()) {
            if (!impl->memory.Fetch(state.rip, 1).empty()) throw std::runtime_error("Native guest function binding overlaps executable guest memory");
            std::uint64_t next = 0;
            if (!impl->memory.TryRead(state.registers[4], std::as_writable_bytes(std::span(&next, 1)))) {
                throw std::runtime_error("Native guest function return address is outside readable guest memory");
            }
            const HostCall call{{state.registers[7], state.registers[6], state.registers[2], state.registers[1], state.registers[8], state.registers[9]},
                                state.registers[4] + 8, impl->memory};
            const auto result = function->second.function(call, function->second.context);
            if (!impl->memory.TryRead(state.registers[4], std::as_writable_bytes(std::span(&next, 1)))) {
                throw std::runtime_error("Native guest function return address became unreadable");
            }
            state.registers[0] = result;
            state.registers[4] += 8;
            state.rip = next;
            --instructionBudget;
            continue;
        }
        auto block = impl->blocks.find(state.rip);
        if (block == impl->blocks.end()) {
            const auto translated = Translate(impl->memory, state.rip);
            block = impl->blocks.emplace(state.rip, Impl::Block{arena().Append(translated.words), translated.instructions}).first;
        }
        if (block->second.instructions > instructionBudget) throw std::runtime_error("ARM64 guest instruction budget exceeded");
        instructionBudget -= block->second.instructions;
        block->second.function(&state);
        if (state.fault != 0) {
            char message[160];
            std::snprintf(message, sizeof(message), "Guest memory %s fault at 0x%llx while executing 0x%llx",
                state.fault == 1 ? "read" : "write", static_cast<unsigned long long>(state.faultAddress), static_cast<unsigned long long>(state.rip));
            throw std::runtime_error(message);
        }
    }
}

std::size_t Cpu::CompiledBlocks() const { return impl->blocks.size(); }

}
