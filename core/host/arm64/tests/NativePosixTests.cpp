#include "NativePosix.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <stdexcept>
#include <unistd.h>
#include <thread>

using namespace AnyPS5::Arm64;

namespace {

void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }

struct Fixture {
    GuestMemory memory;
    NativeLibc libc;
    NativePosix posix{libc};
    Cpu cpu{memory};
    Fixture() {
        memory.Map(0x1000, 0x2000, Read | Write);
        memory.Map(0x8000, 0x1000, Read | Write);
        libc.Bind(cpu, memory);
        posix.Bind(cpu);
    }
    std::uint64_t Call(std::string_view name, std::array<std::uint64_t, 6> arguments = {}, std::uint64_t budget = 10000) {
        CpuState state;
        state.rip = posix.Resolve({name, 2, 1, 0});
        if (state.rip == 0) state.rip = libc.Resolve({name, 2, 1, 0});
        require(state.rip != 0, "POSIX function is unresolved");
        state.registers[4] = 0x8ff8;
        const std::array<std::uint8_t, 6> registers{7, 6, 2, 1, 8, 9};
        for (std::size_t i = 0; i < arguments.size(); ++i) state.registers[registers[i]] = arguments[i];
        cpu.Run(state, budget);
        return state.registers[0];
    }
    void String(std::uint64_t address, const std::string& value) {
        require(memory.TryWrite(address, std::as_bytes(std::span(value.c_str(), value.size() + 1))), "Guest string write failed");
    }
    std::string String(std::uint64_t address) {
        std::string value;
        for (;;) { const auto byte = Value<char>(address++); if (byte == 0) return value; value.push_back(byte); }
    }
    template<class TValue>
    TValue Value(std::uint64_t address) {
        TValue value{};
        require(memory.TryRead(address, std::as_writable_bytes(std::span(&value, 1))), "Guest value read failed");
        return value;
    }
    template<class TValue>
    void Value(std::uint64_t address, const TValue& value) {
        require(memory.TryWrite(address, std::as_bytes(std::span(&value, 1))), "Guest value write failed");
    }
    int Error() { return Value<int>(libc.ErrorAddress(memory)); }
};

struct Directory {
    std::filesystem::path path;
    Directory() {
        std::array<char, 64> pattern{};
        std::strcpy(pattern.data(), "/tmp/anyps5-arm64-posix-XXXXXX");
        const auto result = ::mkdtemp(pattern.data());
        require(result != nullptr, "Cannot create POSIX test directory");
        path = result;
    }
    ~Directory() { std::error_code error; std::filesystem::remove_all(path, error); }
};

void testFiles() {
    Directory directory;
    Fixture fixture;
    fixture.String(0x1000, (directory.path / "guest.dat").string());
    const auto descriptor = fixture.Call("open", {0x1000, 0x602, 0600});
    require(static_cast<std::int64_t>(descriptor) >= 0, "Guest open did not create a file");
    fixture.String(0x1200, "Metal ARM64");
    require(fixture.Call("write", {descriptor, 0x1200, 11}) == 11 && fixture.Call("lseek", {descriptor, 0, 0}) == 0, "Guest file write/seek failed");
    require(fixture.Call("read", {descriptor, 0x2ffc, 8}) == UINT64_MAX && fixture.Error() == 14, "Invalid read buffer did not return EFAULT");
    require(fixture.Call("lseek", {descriptor, 0, 1}) == 0, "Invalid read advanced the file offset");
    require(fixture.Call("read", {descriptor, 0x1300, 16}) == 11 && fixture.String(0x1300) == "Metal ARM64", "Guest file bytes changed");
    require(fixture.Call("fstat", {descriptor, 0x1400}) == 0 && fixture.Value<std::int64_t>(0x1448) == 11, "Guest fstat size is at the wrong ABI offset");
    require((fixture.Value<std::uint16_t>(0x1408) & 0170000) == 0100000, "Guest fstat file type changed");
    require(fixture.Call("stat", {0x1000, 0x1600}) == 0 && fixture.Value<std::int64_t>(0x1648) == 11, "Guest stat conversion failed");
    require(fixture.Call("close", {descriptor}) == 0 && fixture.Call("read", {descriptor, 0x1300, 1}) == UINT64_MAX && fixture.Error() == 9, "Closed descriptor remained usable");
    fixture.String(0x1100, (directory.path / "renamed.dat").string());
    require(fixture.Call("rename", {0x1000, 0x1100}) == 0, "Guest rename failed");
    fixture.String(0x1700, directory.path.string());
    const auto handle = fixture.Call("opendir", {0x1700});
    require(handle != 0, "Guest opendir failed");
    bool found = false;
    for (;;) {
        const auto entry = fixture.Call("readdir", {handle});
        if (entry == 0) break;
        const auto name = fixture.String(entry + 8);
        require(fixture.Value<std::uint8_t>(entry + 7) == name.size(), "Guest dirent name length changed");
        require(fixture.Value<std::uint16_t>(entry + 4) == 8 + ((name.size() + 4) & ~std::size_t{3}), "Guest dirent record length changed");
        if (name == "renamed.dat") { found = true; require(fixture.Value<std::uint8_t>(entry + 6) == 8, "Guest dirent regular-file type changed"); }
    }
    require(found && fixture.Call("closedir", {handle}) == 0, "Guest directory enumeration missed its file");
    require(fixture.Call("readdir", {handle}) == 0 && fixture.Error() == 9, "Closed directory remained usable");
    require(fixture.Call("unlink", {0x1100}) == 0 && !std::filesystem::exists(directory.path / "renamed.dat"), "Guest unlink failed");
    require(fixture.Call("stat", {0x1100, 0x1400}) == UINT64_MAX && fixture.Error() == 2, "Missing file did not return ENOENT");
    const auto cwd = fixture.Call("getcwd");
    require(cwd != 0 && fixture.String(cwd) == std::filesystem::current_path().string(), "Guest getcwd returned the wrong directory");
    fixture.Call("free", {cwd});
    require(fixture.Call("getcwd", {0x1800, 1}) == 0 && fixture.Error() == 34, "Short getcwd buffer did not fail");
}

void testEnvironmentAndArguments() {
    Fixture fixture;
    fixture.posix.SetArguments(fixture.memory, {"game.elf", "--datadir", "shareware"});
    const auto arguments = fixture.Call("getargv");
    require(fixture.Call("getargc") == 3 && fixture.String(fixture.Value<std::uint64_t>(arguments)) == "game.elf" && fixture.String(fixture.Value<std::uint64_t>(arguments + 16)) == "shareware" && fixture.Value<std::uint64_t>(arguments + 24) == 0, "Guest argv layout changed");
    fixture.String(0x1000, "ANYPS5_TEST_GUEST_ONLY");
    fixture.String(0x1100, "Metal");
    require(fixture.Call("setenv", {0x1000, 0x1100, 1}) == 0 && fixture.String(fixture.Call("getenv", {0x1000})) == "Metal", "Guest environment set/get failed");
    require(std::getenv("ANYPS5_TEST_GUEST_ONLY") == nullptr, "Guest environment modified the host environment");
    fixture.String(0x1100, "ARM64");
    require(fixture.Call("setenv", {0x1000, 0x1100, 0}) == 0 && fixture.String(fixture.Call("getenv", {0x1000})) == "Metal", "setenv overwrite=0 replaced an existing variable");
    fixture.String(0x1200, "ANYPS5_ALIAS=old");
    fixture.String(0x1300, "ANYPS5_ALIAS");
    require(fixture.Call("putenv", {0x1200}) == 0 && fixture.Call("getenv", {0x1300}) == 0x120d, "putenv did not preserve the caller's storage");
    fixture.String(0x1200, "ANYPS5_OTHER=new");
    fixture.String(0x1400, "ANYPS5_OTHER");
    require(fixture.Call("getenv", {0x1300}) == 0 && fixture.String(fixture.Call("getenv", {0x1400})) == "new", "putenv name mutation was not visible");
    require(fixture.Call("unsetenv", {0x1400}) == 0 && fixture.Call("getenv", {0x1400}) == 0, "unsetenv retained a variable");
    fixture.String(0x1500, "bad=name");
    require(fixture.Call("setenv", {0x1500, 0x1100, 1}) == UINT64_MAX && fixture.Error() == 22, "Invalid environment name succeeded");
}

void testStreamDescriptorOwnership() {
    Directory directory;
    Fixture fixture;
    fixture.String(0x1000, (directory.path / "stream.dat").string());
    fixture.String(0x1100, "w+b");
    const auto stream = fixture.Call("fopen", {0x1000, 0x1100});
    const auto descriptor = fixture.Call("fileno", {stream});
    fixture.String(0x1200, "ARM64");
    require(stream != 0 && fixture.Call("write", {descriptor, 0x1200, 5}) == 5 && fixture.Call("fstat", {descriptor, 0x1400}) == 0 && fixture.Value<std::int64_t>(0x1448) == 5, "POSIX operations did not share a FILE descriptor");
    require(fixture.Call("close", {descriptor}) == 0 && fixture.Call("fileno", {stream}) == UINT64_MAX && fixture.Error() == 9, "Closing a FILE descriptor retained a live descriptor");
    const auto replacement = fixture.Call("open", {0x1000, 2});
    require(static_cast<std::int64_t>(replacement) >= 0, "Cannot reopen a closed stream's file");
    require(fixture.Call("fclose", {stream}) == UINT64_MAX && fixture.Error() == 9, "fclose did not report its already closed descriptor");
    require(fixture.Call("read", {replacement, 0x1300, 5}) == 5 && fixture.String(0x1300) == "ARM64", "fclose closed a reused descriptor owned by another file");
    fixture.Call("close", {replacement});
    require(fixture.Call("close", {1}) == 0 && fixture.Call("write", {1, 0x1200, 5}) == UINT64_MAX && fixture.Error() == 9 && ::fcntl(STDOUT_FILENO, F_GETFD) >= 0, "Closing guest stdout affected the host descriptor");
    fixture.String(0x1500, "closed\n");
    require(fixture.Call("printf", {0x1500}) == UINT64_MAX && fixture.Error() == 9, "Guest printf bypassed a closed standard output");
}

void testTimeAndTokens() {
    Fixture fixture;
    require(fixture.Call("gettimeofday", {0x1000, 0}) == 0 && fixture.Value<std::int64_t>(0x1000) > 1700000000 && fixture.Value<std::int64_t>(0x1008) < 1000000, "Guest timeval ABI changed");
    const auto seconds = fixture.Call("time", {0x1100});
    require(fixture.Value<std::uint64_t>(0x1100) == seconds, "Guest time did not write its return value");
    require(fixture.Call("clock_gettime", {4, 0x1200}) == 0, "Guest monotonic clock failed");
    const auto before = fixture.Value<std::array<std::int64_t, 2>>(0x1200);
    fixture.Value(0x1300, std::array<std::int64_t, 2>{0, 1000000});
    require(fixture.Call("nanosleep", {0x1300, 0}) == 0 && fixture.Call("clock_gettime", {4, 0x1200}) == 0, "Guest sleep/clock failed");
    const auto after = fixture.Value<std::array<std::int64_t, 2>>(0x1200);
    require(after[0] * 1000000000 + after[1] - before[0] * 1000000000 - before[1] >= 1000000, "Guest nanosleep returned before its duration");
    fixture.Value(0x1300, std::array<std::int64_t, 2>{0, 1000000000});
    require(fixture.Call("nanosleep", {0x1300, 0}) == UINT64_MAX && fixture.Error() == 22, "Invalid nanoseconds succeeded");
    require(fixture.Call("sysconf", {47}) == 16384 && fixture.Call("sysconf", {58}) > 0, "Guest page size or CPU count is invalid");
    fixture.String(0x1400, ",one::two,three");
    fixture.String(0x1500, ",:");
    require(fixture.Call("strtok_r", {0x1400, 0x1500, 0x1600}) == 0x1401 && fixture.String(0x1401) == "one", "Guest tokenizer did not split the first token");
    require(fixture.String(fixture.Call("strtok_r", {0, 0x1500, 0x1600})) == "two" && fixture.String(fixture.Call("strtok_r", {0, 0x1500, 0x1600})) == "three" && fixture.Call("strtok_r", {0, 0x1500, 0x1600}) == 0, "Guest tokenizer continuation failed");
    fixture.Call("srand", {123});
    const auto first = fixture.Call("rand"), second = fixture.Call("rand");
    fixture.Call("srand", {123});
    require(fixture.Call("rand") == first && fixture.Call("rand") == second && first != second && second <= INT32_MAX, "Guest random sequence is not repeatable");
    require(guestError(EILSEQ) == 86 && guestError(ECANCELED) == 85 && guestError(EBADF) == 9, "Darwin errno translation changed");
}

void testGuestComparators() {
    Fixture fixture;
    const std::array<std::uint8_t, 5> comparator{0x8b, 0x07, 0x2b, 0x06, 0xc3};
    fixture.memory.Map(0x4000, comparator.size(), Read | Execute, std::as_bytes(std::span(comparator)));
    const std::array<std::int32_t, 5> values{12, -4, 9, 3, 0};
    fixture.Value(0x1000, values);
    fixture.Call("qsort", {0x1000, values.size(), 4, 0x4000});
    require(fixture.Value<std::array<std::int32_t, 5>>(0x1000) == std::array<std::int32_t, 5>{-4, 0, 3, 9, 12}, "Guest comparator did not sort the array");
    fixture.Value<std::int32_t>(0x1100, 9);
    require(fixture.Call("bsearch", {0x1100, 0x1000, 5, 4, 0x4000}) == 0x100c, "Guest bsearch returned a host address");
    fixture.Value<std::int32_t>(0x1100, 8);
    require(fixture.Call("bsearch", {0x1100, 0x1000, 5, 4, 0x4000}) == 0, "Guest bsearch matched an absent key");
    fixture.Value(0x1000, values);
    bool exhausted = false;
    try { fixture.Call("qsort", {0x1000, 5, 4, 0x4000}, 2); }
    catch (const std::runtime_error&) { exhausted = true; }
    require(exhausted && fixture.Value<std::array<std::int32_t, 5>>(0x1000) == values, "Guest sort bypassed its CPU budget or partially wrote on failure");
}

void testSleepCancellation() {
    Fixture fixture;
    std::atomic<bool> started = false, stopped = false;
    std::exception_ptr failure;
    std::thread waiter([&] {
        started = true;
        try { fixture.Call("sleep", {60}); }
        catch (const ExecutionStopped&) { stopped = true; }
        catch (...) { failure = std::current_exception(); }
    });
    while (!started) std::this_thread::yield();
    const auto start = std::chrono::steady_clock::now();
    fixture.posix.Shutdown();
    waiter.join();
    if (failure) std::rethrow_exception(failure);
    require(stopped && std::chrono::steady_clock::now() - start < std::chrono::seconds(1), "Stopped guest sleep delayed runtime teardown");
}

}

int main() {
    try { testFiles(); testStreamDescriptorOwnership(); testEnvironmentAndArguments(); testTimeAndTokens(); testGuestComparators(); testSleepCancellation(); std::puts("Guest POSIX file/directory ABI, descriptor ownership, environment, arguments, clocks, nested comparators and stopped waits passed"); return 0; }
    catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
