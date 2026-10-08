#include "NativeSignals.hpp"
#include "NativeThreads.hpp"
#include "NativeProcess.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <thread>

using namespace AnyPS5::Arm64;

namespace {

void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
using SignalSet = std::array<std::uint32_t, 4>;
struct Action { std::uint64_t handler; std::uint32_t flags; SignalSet mask; };

struct Fixture {
    GuestMemory memory;
    NativeLibc libc;
    NativeSignals signals{libc};
    NativeProcess process{libc};
    NativeThreads threads{libc, memory};
    Cpu cpu{memory};
    unsigned received = 0, depth = 0, maximumDepth = 0;
    bool repeat = false;
    Fixture() {
        memory.Map(0x1000, 0x2000, Read | Write);
        memory.Map(0x8000, 0x2000, Read | Write);
        libc.Bind(cpu, memory);
        threads.SetSignals(signals);
        threads.Bind(cpu);
        process.SetSignals(signals);
        process.Bind(cpu);
        cpu.Bind(0xb000, +[](const HostCall& call, void* context) -> std::uint64_t {
            auto& fixture = *static_cast<Fixture*>(context);
            require(call.arguments[0] == 30 || call.arguments[0] == 6, "Signal handler received the wrong signal number");
            ++fixture.received;
            ++fixture.depth;
            fixture.maximumDepth = std::max(fixture.maximumDepth, fixture.depth);
            if (fixture.repeat && fixture.received == 1) static_cast<void>(call.cpu.Invoke(call.state, NativeSignals::Resolve({"raise", 2, 1, 0}), {30}));
            --fixture.depth;
            return 77;
        }, this);
    }
    template<class TValue> void Value(std::uint64_t address, const TValue& value) {
        require(memory.TryWrite(address, std::as_bytes(std::span(&value, 1))), "Signal fixture write failed");
    }
    template<class TValue> TValue Value(std::uint64_t address) {
        TValue value{};
        require(memory.TryRead(address, std::as_writable_bytes(std::span(&value, 1))), "Signal fixture read failed");
        return value;
    }
    std::uint64_t Call(std::string_view name, std::array<std::uint64_t, 6> args = {}, std::uint64_t budget = 1000) {
        CpuState state;
        state.rip = NativeSignals::Resolve({name, 2, 1, 0});
        if (!state.rip) state.rip = NativeProcess::Resolve({name, 2, 1, 0});
        if (!state.rip) state.rip = libc.Resolve({name, 2, 1, 0});
        require(state.rip != 0, "Signal fixture function is missing");
        state.registers[4] = 0x9ff8;
        Value<std::uint64_t>(state.registers[4], 0);
        constexpr std::array<unsigned, 6> registers{7, 6, 2, 1, 8, 9};
        for (unsigned i = 0; i < args.size(); ++i) state.registers[registers[i]] = args[i];
        cpu.Run(state, budget);
        return state.registers[0];
    }
    int Error() { return Value<int>(Call("__error")); }
};

void testSetsAndMasks() {
    Fixture fixture;
    require(fixture.Call("sigemptyset", {0x1000}) == 0, "Cannot clear a signal set");
    for (int signal : {1, 32, 33, 64, 65, 128}) {
        require(fixture.Call("sigaddset", {0x1000, static_cast<unsigned>(signal)}) == 0 && fixture.Call("sigismember", {0x1000, static_cast<unsigned>(signal)}) == 1, "Signal set bit position is wrong");
    }
    require(fixture.Value<SignalSet>(0x1000) == SignalSet{0x80000001, 0x80000001, 1, 0x80000000}, "FreeBSD signal set words are wrong");
    require(fixture.Call("sigdelset", {0x1000, 64}) == 0 && fixture.Call("sigismember", {0x1000, 64}) == 0, "Signal deletion changed another bit");
    require(fixture.Call("sigfillset", {0x1000}) == 0 && fixture.Value<SignalSet>(0x1000) == SignalSet{UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX}, "Signal fill truncated the set");
    require(fixture.Call("pthread_sigmask", {3, 0x1000, 0x1100}) == 0 && fixture.Value<SignalSet>(0x1100) == SignalSet{}, "Initial thread mask is not empty");
    require(fixture.Call("sigprocmask", {999, 0, 0x1100}) == 0 && fixture.Value<SignalSet>(0x1100)[0] == (UINT32_MAX & ~0x10100u), "Uncatchable signals were blocked or query rejected its unused operation");
    fixture.Value<int>(fixture.Call("__error"), 44);
    require(fixture.Call("pthread_sigmask", {9, 0x1000, 0}) == 22 && fixture.Error() == 44, "pthread_sigmask changed errno or accepted an invalid operation");
    require(fixture.Call("sigprocmask", {9, 0x1000, 0}) == UINT64_MAX && fixture.Error() == 22, "sigprocmask did not set errno");
    require(fixture.Call("sigaddset", {0x1000, 129}) == UINT64_MAX && fixture.Error() == 22, "Out-of-range signal set index succeeded");
    require(fixture.Call("sigemptyset", {0x2ff8}) == UINT64_MAX && fixture.Error() == 14, "Partial signal set pointer succeeded");
}

void testPendingAndActions() {
    Fixture fixture;
    require(fixture.Call("sigaction", {30, 0, 0x1300}) == 0 && fixture.Value<Action>(0x1300).flags == 2, "Initial signal restart disposition differs from FreeBSD");
    require(fixture.Call("signal", {30, 0xb000}) == 0, "Initial signal disposition is wrong");
    fixture.Value(0x1000, SignalSet{1u << 29, 0, 0, 0});
    require(fixture.Call("pthread_sigmask", {1, 0x1000, 0}) == 0 && fixture.Call("raise", {30}) == 0 && fixture.Call("raise", {30}) == 0 && fixture.received == 0, "Blocked signal was delivered");
    require(fixture.Call("sigpending", {0x1100}) == 0 && fixture.Value<SignalSet>(0x1100)[0] == (1u << 29), "Blocked signal was lost");
    require(fixture.Call("pthread_sigmask", {2, 0x1000, 0}) == 0 && fixture.received == 1, "Unblocking did not deliver one coalesced signal before returning");
    fixture.repeat = true;
    fixture.received = 0;
    require(fixture.Call("raise", {30}) == 0 && fixture.received == 2 && fixture.maximumDepth == 1, "A signal recursively delivered itself without SA_NODEFER");
    fixture.Value(0x1200, Action{0xb000, 0x10, {}});
    require(fixture.Call("sigaction", {30, 0x1200, 0x1300}) == 0 && fixture.Value<Action>(0x1300).flags == 2, "sigaction flags or ABI offsets changed");
    fixture.received = fixture.maximumDepth = 0;
    require(fixture.Call("raise", {30}) == 0 && fixture.received == 2 && fixture.maximumDepth == 2, "SA_NODEFER did not permit nested delivery");
    fixture.Value(0x1200, Action{0xb000, 4, {}});
    require(fixture.Call("sigaction", {30, 0x1200, 0}) == 0, "Cannot set one-shot signal disposition");
    fixture.repeat = false;
    fixture.received = 0;
    require(fixture.Call("raise", {30}) == 0 && fixture.Call("sigaction", {30, 0, 0x1300}) == 0 && fixture.Value<Action>(0x1300).handler == 0 && fixture.Value<Action>(0x1300).flags == 4 && fixture.received == 1, "SA_RESETHAND did not reset the handler and retain the action flags");
    fixture.Call("signal", {30, 0xb000});
    fixture.Call("pthread_sigmask", {1, 0x1000});
    fixture.Call("raise", {30});
    require(fixture.Call("signal", {30, 1}) == 0xb000 && fixture.Call("sigpending", {0x1100}) == 0 && fixture.Value<SignalSet>(0x1100)[0] == 0, "Ignoring a signal kept its pending instance");
    require(fixture.Call("sigaction", {9, 0, 0x1300}) == 0 && fixture.Value<Action>(0x1300).handler == 0 && fixture.Call("signal", {17, 0}) == 0, "Uncatchable signals cannot query or set their default disposition");
    require(fixture.Call("signal", {9, 1}) == UINT64_MAX && fixture.Error() == 22, "SIGKILL was ignored");
    fixture.Value(0x1200, Action{0xb000, 0x40, {}});
    try { fixture.Call("sigaction", {30, 0x1200, 0}); throw std::logic_error("Unsupported siginfo action succeeded"); }
    catch (const std::runtime_error& error) { require(std::string(error.what()).find("unsupported") != std::string::npos, error.what()); }
    require(fixture.Call("signal", {30, 1}) == 1, "Unsupported signal action mutated the previous disposition");
    fixture.Value(0x1200, Action{0xb000, 0, {}});
    require(fixture.Call("sigaction", {30, 0x1200, 0x2ff8}) == UINT64_MAX && fixture.Error() == 14 && fixture.Call("signal", {30, 1}) == 1, "Failed action preflight mutated the disposition");
}

void testInterruptedStateAndBudget() {
    Fixture fixture;
    const std::array<std::byte, 38> handler{
        std::byte{0x48}, std::byte{0xb8}, std::byte{77}, {}, {}, {}, {}, {}, {}, {},
        std::byte{0x48}, std::byte{0xb9}, std::byte{88}, {}, {}, {}, {}, {}, {}, {},
        std::byte{0x48}, std::byte{0xc7}, std::byte{0x44}, std::byte{0x24}, std::byte{0xf8}, std::byte{99}, {}, {}, {},
        std::byte{0x66}, std::byte{0x0f}, std::byte{0xef}, std::byte{0xc0},
        std::byte{0x48}, std::byte{0x83}, std::byte{0xc0}, std::byte{1}, std::byte{0xc3}
    };
    fixture.memory.Map(0x4000, handler.size(), Read | Execute, handler);
    fixture.Call("signal", {30, 0x4000});
    CpuState expected;
    fixture.cpu.Bind(0xc000, +[](const HostCall& call, void* context) -> std::uint64_t {
        const auto& expected = *static_cast<const CpuState*>(context);
        require(call.state.registers == expected.registers && call.state.rflags == expected.rflags && call.state.xmm == expected.xmm, "Signal handler changed interrupted registers");
        return 42;
    }, &expected);
    for (const auto rsp : {0x9f00u, 0x9f08u}) {
        CpuState state;
        for (unsigned i = 0; i < state.registers.size(); ++i) state.registers[i] = 0x123000 + i;
        state.registers[4] = rsp;
        state.rip = 0xc000;
        state.rflags = 0x8d7;
        state.xmm[0].fill(std::byte{0xee});
        fixture.Value<std::uint64_t>(rsp, 0);
        const std::array<std::byte, 128> redZone = [] { std::array<std::byte, 128> bytes; bytes.fill(std::byte{0xac}); return bytes; }();
        fixture.Value(rsp - 128, redZone);
        const auto original = state;
        expected = state;
        fixture.signals.QueueProcess(30);
        const auto count = fixture.cpu.Run(state);
        require(count == 7 && state.registers[0] == 42 && fixture.Value<std::array<std::byte, 128>>(rsp - 128) == redZone, "Signal delivery corrupted the red zone or instruction count");
        require(original.registers[1] == state.registers[1], "Signal clobbered a caller-saved register");
    }
    fixture.Call("signal", {30, 0xb000});
    fixture.received = 0;
    fixture.signals.QueueProcess(30);
    try { fixture.Call("getpid", {}, 1); throw std::logic_error("Signal handler escaped the shared instruction budget"); }
    catch (const std::runtime_error& error) { require(std::string(error.what()).find("budget exceeded") != std::string::npos, error.what()); }
    fixture.signals.QueueProcess(30);
    require(fixture.Call("getpid") == 1 && fixture.received == 2, "Failed signal callback kept its automatic signal mask");
}

void testTerminationAndStop() {
    Fixture fixture;
    require(fixture.Call("getpid") == 1 && fixture.Call("getppid") == 0 && fixture.Call("getpgrp") == 1 && fixture.Call("kill", {1, 0}) == 0, "Guest process identity is inconsistent");
    require(fixture.Call("kill", {999999, 15}) == UINT64_MAX && fixture.Error() == 3, "Guest kill reached an unrelated host process");
    require(fixture.Call("pthread_kill", {999999, 0}) == 3, "Missing thread signal probe succeeded");
    const auto start = std::chrono::steady_clock::now();
    std::jthread resume([&] { std::this_thread::sleep_for(std::chrono::milliseconds(50)); fixture.signals.QueueProcess(19); });
    require(fixture.Call("raise", {17}) == 0 && std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(30), "Default SIGSTOP did not suspend guest execution");
    try { fixture.Call("kill", {1, 15}); throw std::logic_error("Default termination signal returned"); }
    catch (const GuestSignalExit& exit) { require(exit.Signal() == 15 && exit.Status() == 143, "Guest signal exit status is wrong"); }
}

void testAbort() {
    Fixture fixture;
    fixture.Call("signal", {6, 0xb000});
    fixture.Value(0x1000, SignalSet{1u << 5, 0, 0, 0});
    fixture.Call("pthread_sigmask", {1, 0x1000});
    try { fixture.Call("abort"); throw std::logic_error("Abort returned after its handler"); }
    catch (const GuestExit& exit) { require(exit.Status() == 134 && exit.Aborted() && fixture.received == 1, "Abort did not unblock its signal, call the handler and terminate"); }
}

void testStoppedKillAndShutdown() {
    {
        Fixture fixture;
        fixture.Call("signal", {1, 0xb000});
        std::jthread kill([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            fixture.signals.QueueProcess(1);
            fixture.signals.QueueProcess(9);
        });
        try { fixture.Call("raise", {17}); throw std::logic_error("Stopped guest ignored SIGKILL"); }
        catch (const GuestSignalExit& exit) { require(exit.Signal() == 9 && fixture.received == 0, "Stopped guest ran an ordinary handler before SIGKILL"); }
    }
    {
        Fixture fixture;
        std::jthread shutdown([&] { std::this_thread::sleep_for(std::chrono::milliseconds(50)); fixture.signals.Shutdown(); });
        try { fixture.Call("raise", {17}); throw std::logic_error("Stopped guest did not wake during shutdown"); }
        catch (const ExecutionStopped&) {}
    }
}

}

int main() {
    try { testSetsAndMasks(); testPendingAndActions(); testInterruptedStateAndBudget(); testTerminationAndStop(); testAbort(); testStoppedKillAndShutdown(); }
    catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
    return 0;
}
