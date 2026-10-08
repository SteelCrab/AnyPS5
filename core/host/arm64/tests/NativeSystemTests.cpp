#include "NativeSystem.hpp"
#include <array>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <sys/sysctl.h>

using namespace AnyPS5::Arm64;

namespace {

void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }

struct Fixture {
    GuestMemory memory;
    NativeLibc libc;
    NativeSystem system{libc};
    Cpu cpu{memory};
    Fixture() {
        memory.Map(0x1000, 0x2000, Read | Write);
        memory.Map(0x8000, 0x1000, Read | Write);
        libc.Bind(cpu, memory);
        system.Bind(cpu);
    }
    std::uint64_t Call(std::string_view name, std::array<std::uint64_t, 6> arguments) {
        CpuState state;
        state.rip = system.Resolve({name, 2, 1, 0});
        state.registers[4] = 0x8ff8;
        const std::array<std::uint8_t, 6> registers{7, 6, 2, 1, 8, 9};
        for (std::size_t i = 0; i < arguments.size(); ++i) state.registers[registers[i]] = arguments[i];
        cpu.Run(state);
        return state.registers[0];
    }
    template<class TValue>
    void Value(std::uint64_t address, const TValue& value) {
        require(memory.TryWrite(address, std::as_bytes(std::span(&value, 1))), "Cannot store guest sysctl value");
    }
    template<class TValue>
    TValue Value(std::uint64_t address) {
        TValue value{};
        require(memory.TryRead(address, std::as_writable_bytes(std::span(&value, 1))), "Cannot read guest sysctl value");
        return value;
    }
    void Name(std::string_view name) {
        const std::string value(name);
        require(memory.TryWrite(0x1000, std::as_bytes(std::span(value.c_str(), value.size() + 1))), "Cannot store guest sysctl name");
    }
    int Error() { return Value<int>(libc.ErrorAddress(memory)); }
};

void testHostValues() {
    Fixture fixture;
    int cores = 0;
    std::size_t size = sizeof(cores);
    require(::sysctlbyname("hw.ncpu", &cores, &size, nullptr, 0) == 0, "Cannot query native CPU count");
    fixture.Name("hw.ncpu");
    fixture.Value<std::uint64_t>(0x1100, 99);
    require(fixture.Call("sysctlbyname", {0x1000, 0, 0x1100}) == 0 && fixture.Value<std::uint64_t>(0x1100) == 4, "Named sysctl size query is invalid");
    require(fixture.Call("sysctlbyname", {0x1000, 0x1200, 0x1100}) == 0 && fixture.Value<int>(0x1200) == cores, "Guest CPU count differs from native CPU count");
    fixture.Value(0x1000, std::array<std::int32_t, 2>{6, 3});
    fixture.Value<std::uint64_t>(0x1100, 4);
    require(fixture.Call("sysctl", {0x1000, 2, 0x1300, 0x1100}) == 0 && fixture.Value<int>(0x1300) == cores, "Numeric guest sysctl selected the wrong native node");
    std::uint64_t bytes = 0;
    size = sizeof(bytes);
    require(::sysctlbyname("hw.memsize", &bytes, &size, nullptr, 0) == 0, "Cannot query native memory size");
    fixture.Value(0x1000, std::array<std::int32_t, 2>{6, 12});
    fixture.Value<std::uint64_t>(0x1100, 8);
    require(fixture.Call("sysctl", {0x1000, 2, 0x1300, 0x1100}) == 0 && fixture.Value<std::uint64_t>(0x1300) == bytes, "FreeBSD HW_REALMEM was forwarded as a Darwin MIB");
    fixture.Name("hw.physmem");
    fixture.Value<std::uint64_t>(0x1100, 8);
    require(fixture.Call("sysctlbyname", {0x1000, 0x1300, 0x1100}) == 0 && fixture.Value<std::uint64_t>(0x1300) == bytes, "Physical memory was truncated to a Darwin 32-bit value");
}

void testBuffersAndErrors() {
    Fixture fixture;
    fixture.Name("hw.ncpu");
    fixture.Value<std::uint64_t>(0x1100, 1);
    fixture.Value<std::uint32_t>(0x1200, 0xdddddddd);
    require(fixture.Call("sysctlbyname", {0x1000, 0x1200, 0x1100}) == UINT64_MAX && fixture.Error() == 12 && fixture.Value<std::uint64_t>(0x1100) == 1 && (fixture.Value<std::uint32_t>(0x1200) & 0xffffff00) == 0xdddddd00, "Short sysctl did not preserve its partial-copy contract");
    fixture.Value<std::uint64_t>(0x1100, 4);
    require(fixture.Call("sysctlbyname", {0x1000, 0x2ffe, 0x1100}) == UINT64_MAX && fixture.Error() == 14 && fixture.Value<std::uint64_t>(0x1100) == 4, "Invalid sysctl output modified the length");
    require(fixture.Call("sysctlbyname", {0x1000, 0, 0xdead}) == UINT64_MAX && fixture.Error() == 14, "Invalid sysctl length pointer succeeded");
    fixture.Value<int>(0x1500, 1);
    require(fixture.Call("sysctlbyname", {0x1000, 0, 0x1100, 0x1500, 4}) == UINT64_MAX && fixture.Error() == 1, "Read-only sysctl accepted a host mutation");
    fixture.Name("hw.anyps5_missing");
    require(fixture.Call("sysctlbyname", {0x1000, 0, 0x1100}) == UINT64_MAX && fixture.Error() == 2, "Unknown sysctl returned a fabricated value");
    require(fixture.Call("sysctl", {0x1000, 25, 0, 0x1100}) == UINT64_MAX && fixture.Error() == 22, "Oversized MIB was accepted");
    fixture.Name("hw.machine_arch");
    fixture.Value<std::uint64_t>(0x1100, 64);
    require(fixture.Call("sysctlbyname", {0x1000, 0x1200, 0x1100}) == 0 && fixture.Value<std::uint64_t>(0x1100) > 1, "Machine architecture string is absent");
    require(fixture.Value<char>(0x1200 + fixture.Value<std::uint64_t>(0x1100) - 1) == 0, "System string is not NUL-terminated");
}

}

int main() {
    try { testHostValues(); testBuffersAndErrors(); }
    catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
    return 0;
}
