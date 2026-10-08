#include "NativeKernel.hpp"
#include <array>
#include <bit>
#include <chrono>
#include <cstdio>
#include <future>
#include <limits>
#include <stdexcept>
#include <thread>

using namespace AnyPS5::Arm64;

namespace {

void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
constexpr std::uint64_t failure(unsigned error) { return static_cast<std::uint64_t>(static_cast<std::int64_t>(std::bit_cast<std::int32_t>(0x80020000u | error))); }

struct Fixture {
    GuestMemory memory;
    NativeKernel kernel;
    Cpu cpu{memory};
    Fixture() {
        memory.Map(0x1000, 0x4000, AnyPS5::Arm64::Read | AnyPS5::Arm64::Write);
        memory.Map(0x8000, 0x1000, AnyPS5::Arm64::Read | AnyPS5::Arm64::Write);
        kernel.Bind(cpu);
    }
    std::uint64_t Call(std::string_view name, std::array<std::uint64_t, 6> arguments = {}) {
        CpuState state;
        state.rip = kernel.Resolve({name, 2, 1, 0});
        require(state.rip != 0, "Native kernel import was not resolved");
        state.registers[4] = 0x8ff8;
        const std::array<std::uint8_t, 6> registers{7, 6, 2, 1, 8, 9};
        for (std::size_t i = 0; i < arguments.size(); ++i) state.registers[registers[i]] = arguments[i];
        cpu.Run(state);
        return state.registers[0];
    }
    template<class TValue>
    TValue Read(std::uint64_t address) {
        TValue value{};
        require(memory.TryRead(address, std::as_writable_bytes(std::span(&value, 1))), "Native kernel guest read failed");
        return value;
    }
    template<class TValue>
    void Write(std::uint64_t address, const TValue& value) {
        require(memory.TryWrite(address, std::as_bytes(std::span(&value, 1))), "Native kernel guest write failed");
    }
    std::uint64_t Allocate(std::size_t size, std::size_t alignment = 0) {
        require(Call("sceKernelAllocateMainDirectMemory", {size, alignment, 3, 0x1000}) == 0, "Native physical allocation failed");
        return Read<std::uint64_t>(0x1000);
    }
    std::uint64_t Map(std::uint64_t physical, std::size_t size, unsigned protection = 0x33) {
        Write<std::uint64_t>(0x1008, 0);
        require(Call("sceKernelMapDirectMemory", {0x1008, size, protection, 0, physical, 0x10000}) == 0, "Native physical mapping failed");
        return Read<std::uint64_t>(0x1008);
    }
    std::uint64_t Queue() {
        const char name[] = "Native event queue";
        require(memory.TryWrite(0x1100, std::as_bytes(std::span(name))), "Queue name could not be written");
        require(Call("sceKernelCreateEqueue", {0x1010, 0x1100}) == 0, "Native event queue creation failed");
        return Read<std::uint64_t>(0x1010);
    }
};

void testSharedMemory() {
    Fixture fixture;
    require(fixture.kernel.Resolve({"sceKernelUnknown", 2, 1, 0}) == 0, "Unknown kernel function was falsely resolved");
    require(fixture.kernel.Resolve({"sceKernelMapDirectMemory", 1, 1, 8}) == 0, "Kernel function resolved as a data object");
    const auto physical = fixture.Allocate(0xc000, 0x10000);
    require((physical & 65535) == 0, "Physical allocation alignment failed");
    const auto first = fixture.Map(physical, 0xc000), second = fixture.Map(physical, 0xc000);
    fixture.Write<std::uint64_t>(first + 0x100, 0xdeadbeef);
    require(fixture.Read<std::uint64_t>(second + 0x100) == 0xdeadbeef, "Direct mappings do not share physical bytes");
    fixture.Write<std::uint64_t>(second + 0x8100, 0x12345678);
    require(fixture.Read<std::uint64_t>(first + 0x8100) == 0x12345678, "Shared mapping writes are not bidirectional");
    const auto readOnly = fixture.Map(physical, 0x4000, 1);
    const std::uint64_t value = 7;
    require(!fixture.memory.TryWrite(readOnly, std::as_bytes(std::span(&value, 1))), "Read-only direct mapping accepted a write");
    require(fixture.Call("sceKernelReleaseDirectMemory", {physical + 0x4000, 0x4000}) == 0, "Partial physical release failed");
    require(!fixture.memory.Contains(first + 0x4000, 1, Read) && !fixture.memory.Contains(second + 0x4000, 1, Read), "Physical release did not remove every alias");
    require(fixture.Read<std::uint64_t>(first + 0x100) == 0xdeadbeef && fixture.Read<std::uint64_t>(second + 0x8100) == 0x12345678, "Partial release discarded neighboring data");
    const auto reused = fixture.Allocate(0x4000);
    require(reused == physical + 0x4000, "Physical allocation did not reuse its free range");
    const auto fresh = fixture.Map(reused, 0x4000);
    require(fixture.Read<std::uint64_t>(fresh) == 0, "Reused physical storage was not zero initialized");
    require(fixture.Call("sceKernelMunmap", {first + 0x8000, 0x4000}) == 0, "Direct alias unmap failed");
    require(!fixture.memory.Contains(first + 0x8000, 1, Read) && fixture.Read<std::uint64_t>(second + 0x8100) == 0x12345678, "Unmapping one alias affected another alias");
    require(fixture.Call("sceKernelReleaseDirectMemory", {physical + 0x8000, 0x4000}) == 0, "Remaining physical tail release failed");
    require(fixture.Call("sceKernelReleaseDirectMemory", {physical, 0x4000}) == 0 && !fixture.memory.Contains(readOnly, 1, Read), "Physical head release retained a read-only alias");
    require(fixture.Call("sceKernelReleaseDirectMemory", {reused, 0x4000}) == 0, "Reused physical allocation release failed");
    require(fixture.Call("sceKernelReleaseDirectMemory", {reused, 0x4000}) == failure(22), "Double physical release succeeded");
}

void testMappingErrors() {
    Fixture fixture;
    require(fixture.Call("sceKernelAllocateMainDirectMemory", {1, 0, 3, 0x1000}) == failure(22), "Unaligned physical size was accepted");
    require(fixture.Call("sceKernelAllocateMainDirectMemory", {0x4000, 0xc000, 3, 0x1000}) == failure(22), "Non-power-of-two alignment was accepted");
    require(fixture.Call("sceKernelAllocateMainDirectMemory", {0x4000, 0, 3, 0x5000}) == failure(14), "Invalid physical output pointer was accepted");
    const auto physical = fixture.Allocate(0x4000);
    require(fixture.Call("sceKernelMapDirectMemory", {0x1008, 0x8000, 3, 0, physical, 0}) == failure(22), "Mapping outside a physical allocation succeeded");
    fixture.Write<std::uint64_t>(0x1008, 0x1000);
    require(fixture.Call("sceKernelMapDirectMemory", {0x1008, 0x4000, 3, 0x10, physical, 0}) == failure(22), "Misaligned fixed mapping succeeded");
    fixture.Write<std::uint64_t>(0x1008, 0x10000);
    require(fixture.Call("sceKernelMapDirectMemory", {0x1008, 0x4000, 3, 0x90, physical, 0}) == 0, "Aligned fixed mapping failed");
    require(fixture.Call("sceKernelMapDirectMemory", {0x1008, 0x4000, 3, 0x90, physical, 0}) == failure(12), "Fixed mapping overwrote existing guest memory");
    require(fixture.Call("sceKernelMunmap", {0x10000, 0x8000}) == failure(22), "Unmap outside one live mapping succeeded");
    require(fixture.Call("sceKernelMunmap", {0x10000, 0x4000}) == 0 && fixture.Call("sceKernelReleaseDirectMemory", {physical, 0x4000}) == 0, "Unmapped physical allocation could not be freed");
    fixture.Write<std::uint64_t>(0x1008, std::numeric_limits<std::uint64_t>::max() - 0x3fff);
    const auto other = fixture.Allocate(0x4000);
    require(fixture.Call("sceKernelMapDirectMemory", {0x1008, 0x4000, 3, 0x10, other, 0}) == failure(12), "Overflowing fixed address was accepted");
}

void testExecutableAliases() {
    GuestMemory memory;
    const std::array<std::byte, 6> code{std::byte{0xb8}, std::byte{42}, {}, {}, {}, std::byte{0xc3}};
    memory.Map(0x1000, 0x1000, Read | Write, code);
    memory.MapAlias(0x2000, 0x1000, Read | Execute, 0x1000);
    memory.Map(0x8000, 0x1000, Read | Write);
    Cpu cpu(memory);
    CpuState state;
    state.rip = 0x2000;
    state.registers[4] = 0x8ff8;
    cpu.Run(state);
    require(state.registers[0] == 42, "Executable physical alias did not run through ARM64 translation");
    const auto version = memory.CodeVersion();
    const std::byte value{77};
    require(memory.TryWrite(0x1001, std::span(&value, 1)) && memory.CodeVersion() > version, "Writable alias did not invalidate executable code");
    state = {};
    state.rip = 0x2000;
    state.registers[4] = 0x8ff8;
    cpu.Run(state);
    require(state.registers[0] == 77, "Executable alias used stale translated code");
    memory.Unmap(0x1000);
    require(memory.Contains(0x2000, code.size(), Read), "Unmapping the source discarded a surviving alias backing");
}

void testQueues() {
    Fixture fixture;
    const auto queue = fixture.Queue();
    fixture.Write<std::uint32_t>(0x1300, 0);
    require(fixture.Call("sceKernelWaitEqueue", {queue, 0x1200, 2, 0x12f0, 0x1300}) == failure(60) && fixture.Read<int>(0x12f0) == 0, "Polling an empty event queue did not time out");
    require(fixture.kernel.AddFlip(queue, 7, 0x9999) == 0, "Flip registration failed");
    fixture.kernel.TriggerFlip(7, 42);
    fixture.kernel.TriggerFlip(7, -2);
    require(fixture.Call("sceKernelWaitEqueue", {queue, 0x5000, 1, 0x12f0, 0x1300}) == failure(14), "Invalid event destination was accepted");
    require(fixture.Call("sceKernelWaitEqueue", {queue, 0x1200, 2, 0x12f0, 0x1300}) == 0 && fixture.Read<int>(0x12f0) == 2, "Pending flip events were lost or merged");
    const auto first = fixture.Read<std::array<std::uint64_t, 4>>(0x1200);
    const auto second = fixture.Read<std::array<std::uint64_t, 4>>(0x1220);
    require(first[0] == 0 && (first[1] & 65535) == 0xfff3 && (first[2] >> 16) == 42 && first[3] == 0x9999 && (second[2] >> 16) == 0xfffffffffffe, "Flip event payload or user data was corrupted");
    fixture.kernel.TriggerFlip(7, 43);
    require(fixture.kernel.DeleteFlip(queue, 7) == 0, "Flip deletion failed");
    require(fixture.Call("sceKernelWaitEqueue", {queue, 0x1200, 1, 0x12f0, 0x1300}) == failure(60), "Deleted flip registration retained pending events");
    fixture.Write<std::uint32_t>(0x1300, 2000);
    const auto started = std::chrono::steady_clock::now();
    require(fixture.Call("sceKernelWaitEqueue", {queue, 0x1200, 1, 0x12f0, 0x1300}) == failure(60) && std::chrono::steady_clock::now() - started >= std::chrono::milliseconds(1), "Timed event wait returned before its deadline");
    require(fixture.kernel.AddFlip(queue, 7, 0x8888) == 0, "Replacement flip registration failed");
    auto waiter = std::async(std::launch::async, [&] {
        Cpu cpu(fixture.memory);
        fixture.kernel.Bind(cpu);
        fixture.memory.Map(0xa000, 0x1000, Read | Write);
        CpuState state;
        HostCall call{{queue, 0x1400, 1, 0x14f0, 0, 0}, 0xa000, fixture.memory, state, cpu};
        return fixture.kernel.WaitQueue(call);
    });
    fixture.kernel.TriggerFlip(7, 44);
    require(waiter.get() == 0 && fixture.Read<std::array<std::uint64_t, 4>>(0x1400)[3] == 0x8888, "Blocking wait did not receive a concurrent flip event");
    auto closing = std::async(std::launch::async, [&] {
        Cpu cpu(fixture.memory);
        CpuState state;
        HostCall call{{queue, 0x1400, 1, 0x14f0, 0, 0}, 0xa000, fixture.memory, state, cpu};
        return fixture.kernel.WaitQueue(call);
    });
    require(fixture.kernel.DeleteQueue(queue) == 0 && closing.get() == std::bit_cast<std::int32_t>(0x80020009u), "Queue deletion did not wake its blocked waiter");
    require(fixture.Call("sceKernelDeleteEqueue", {queue}) == failure(9) && fixture.kernel.AddFlip(queue, 7, 0) != 0, "Deleted queue handle remained usable");
}

}

int main() {
    try {
        testSharedMemory();
        testMappingErrors();
        testExecutableAliases();
        testQueues();
        std::puts("Native direct memory aliases, code invalidation, partial releases and concurrent event queues passed");
        return 0;
    } catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
