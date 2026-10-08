#include "NativeProcess.hpp"
#include <array>
#include <atomic>
#include <stdexcept>
#include <thread>

using namespace AnyPS5::Arm64;

namespace {

void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }

struct Fixture {
    GuestMemory memory;
    NativeLibc libc;
    NativeProcess process{libc};
    Cpu cpu{memory};
    std::vector<std::uint64_t> trace;
    Fixture() {
        memory.Map(0x1000, 0x1000, Read | Write);
        memory.Map(0x8000, 0x1000, Read | Write);
        libc.Bind(cpu, memory);
        process.Bind(cpu);
        cpu.Bind(0x9000, +[](const HostCall& call, void* context) {
            static_cast<Fixture*>(context)->trace.push_back(call.arguments[0]);
            return std::uint64_t{0};
        }, this);
        cpu.Bind(0x9010, +[](const HostCall& call, void* context) {
            auto& fixture = *static_cast<Fixture*>(context);
            fixture.trace.push_back(10);
            static_cast<void>(call.cpu.Invoke(call.state, NativeProcess::Resolve({"atexit", 2, 1, 0}), {0x9020}));
            return std::uint64_t{0};
        }, this);
        cpu.Bind(0x9020, +[](const HostCall&, void* context) {
            static_cast<Fixture*>(context)->trace.push_back(20);
            return std::uint64_t{0};
        }, this);
    }
    void Run(std::uint64_t address, std::uint64_t& budget, std::array<std::uint64_t, 6> arguments = {}) {
        CpuState state;
        state.rip = address;
        state.registers[4] = 0x8ff8;
        const std::array<std::uint8_t, 6> registers{7, 6, 2, 1, 8, 9};
        for (std::size_t i = 0; i < arguments.size(); ++i) state.registers[registers[i]] = arguments[i];
        cpu.RunRemaining(state, budget);
        require(state.registers[0] == 0, "Guest callback registration failed");
    }
    void Call(std::string_view name, std::array<std::uint64_t, 6> arguments = {}) {
        std::uint64_t budget = 1000;
        Run(NativeProcess::Resolve({name, 2, 1, 0}), budget, arguments);
    }
};

void testFinalization() {
    Fixture fixture;
    fixture.Call("__cxa_atexit", {0x9000, 1, 100});
    fixture.Call("atexit", {0x9010});
    fixture.Call("__cxa_atexit", {0x9000, 2, 200});
    fixture.Call("__cxa_atexit", {0x9000, 3, 100});
    fixture.Call("__cxa_finalize", {100});
    fixture.Call("__cxa_finalize", {100});
    require(fixture.trace == std::vector<std::uint64_t>{3, 1}, "DSO destructors did not run once in reverse order");
    fixture.Call("__cxa_finalize");
    require(fixture.trace == std::vector<std::uint64_t>{3, 1, 2, 10, 20}, "Global C++ finalization omitted a C atexit callback");
    fixture.process.SetFinalizers({0x9020});
    std::uint64_t budget = 100;
    fixture.Run(NativeProcess::CompletionAddress(), budget);
    require(fixture.trace == std::vector<std::uint64_t>{3, 1, 2, 10, 20, 20}, "Completion lost a newly registered callback or ELF finalizer");
    fixture.Run(NativeProcess::CompletionAddress(), budget);
    require(fixture.trace.size() == 6, "Completion repeated an ELF finalizer");
}

void testExitModes() {
    for (const auto name : {"exit", "_Exit", "_exit", "abort"}) {
        Fixture fixture;
        fixture.Call("__cxa_atexit", {0x9000, 7, 1});
        fixture.process.SetFinalizers({0x9020});
        bool terminated = false;
        std::uint64_t budget = 100;
        try { fixture.Run(NativeProcess::Resolve({name, 2, 1, 0}), budget, {259}); }
        catch (const GuestExit& exit) {
            terminated = true;
            require(exit.Status() == (std::string_view(name) == "abort" ? 134 : 3), "Guest status did not preserve its low eight bits");
            require(exit.Aborted() == (std::string_view(name) == "abort"), "Guest abort reason changed");
        }
        require(terminated, "Guest termination returned to the caller");
        require(fixture.trace == (std::string_view(name) == "exit" ? std::vector<std::uint64_t>{7, 20} : std::vector<std::uint64_t>{}), "Immediate termination ran cleanup callbacks");
        require(budget == (std::string_view(name) == "exit" ? 97 : 99), "Termination lost nested instruction consumption");
    }
}

void testCleanupBudget() {
    Fixture fixture;
    fixture.Call("__cxa_atexit", {0x9000, 1, 1});
    fixture.Call("__cxa_atexit", {0x9000, 2, 1});
    std::uint64_t budget = 2;
    bool exhausted = false;
    try { fixture.Run(NativeProcess::Resolve({"exit", 2, 1, 0}), budget); }
    catch (const GuestExit&) { throw std::runtime_error("Exit bypassed its cleanup budget"); }
    catch (const std::runtime_error&) { exhausted = true; }
    require(exhausted && budget == 0 && fixture.trace == std::vector<std::uint64_t>{2}, "Cleanup failure lost its instruction budget or ran extra callbacks");
}

void testConcurrentFinalization() {
    Fixture fixture;
    std::atomic<unsigned> called{0};
    fixture.cpu.Bind(0x9030, +[](const HostCall&, void* context) { ++*static_cast<std::atomic<unsigned>*>(context); return std::uint64_t{0}; }, &called);
    for (int i = 0; i < 100; ++i) fixture.Call("__cxa_atexit", {0x9030, 0, 1});
    fixture.memory.Map(0xa000, 0x1000, Read | Write);
    Cpu worker(fixture.memory);
    fixture.process.Bind(worker);
    worker.Bind(0x9030, +[](const HostCall&, void* context) { ++*static_cast<std::atomic<unsigned>*>(context); return std::uint64_t{0}; }, &called);
    std::thread thread([&] {
        CpuState state;
        state.rip = NativeProcess::Resolve({"__cxa_finalize", 2, 1, 0});
        state.registers[4] = 0xaff8;
        worker.Run(state);
    });
    fixture.Call("__cxa_finalize");
    thread.join();
    require(called == 100, "Concurrent finalization repeated a destructor");
}

}

int main() {
    try { testFinalization(); testExitModes(); testCleanupBudget(); testConcurrentFinalization(); }
    catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
    return 0;
}
