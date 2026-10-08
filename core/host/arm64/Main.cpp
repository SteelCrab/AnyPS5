#include "Cpu.hpp"
#include "ElfImage.hpp"
#include "NativeLibc.hpp"
#include "NativeVideoOut.hpp"
#include "NativeThreads.hpp"
#include "NativeMath.hpp"
#include "NativePosix.hpp"
#include "NativeLocale.hpp"
#include "NativeProcess.hpp"
#include "NativeSystem.hpp"
#include "NativeAudio.hpp"
#include <charconv>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: anyps5_arm64_run <x86-64.elf> [instruction-budget] [-- guest-argument ...]\n");
        return 1;
    }
    try {
        std::uint64_t budget = 1000000;
        int argument = 2;
        if (argument < argc && std::string_view(argv[argument]) != "--") {
            const std::string_view value(argv[2]);
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), budget);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || budget == 0) throw std::runtime_error("Instruction budget must be a positive integer");
            ++argument;
        }
        if (argument < argc && std::string_view(argv[argument++]) != "--") throw std::runtime_error("Guest arguments must follow --");
        std::vector<std::string> guestArguments{argv[1]};
        for (; argument < argc; ++argument) guestArguments.emplace_back(argv[argument]);
        std::ifstream input(argv[1], std::ios::binary);
        if (!input) throw std::runtime_error("Cannot open guest ELF");
        const std::string bytes{std::istreambuf_iterator<char>(input), {}};
        AnyPS5::Arm64::NativeLibc libc;
        AnyPS5::Arm64::NativeMath math(libc);
        AnyPS5::Arm64::NativePosix posix(libc);
        AnyPS5::Arm64::NativeLocale locale(libc, posix);
        AnyPS5::Arm64::NativeProcess process(libc);
        AnyPS5::Arm64::NativeSystem system(libc);
        AnyPS5::Arm64::NativeAudio audio;
        AnyPS5::Arm64::NativeKernel kernel;
        AnyPS5::Arm64::NativeVideoOut video(kernel);
        AnyPS5::Arm64::ElfOptions options;
        options.resolveImport = [&libc, &kernel, &video](const auto& symbol) {
            const auto resolved = libc.Resolve(symbol);
            if (resolved != 0) return resolved;
            const auto threadResolved = AnyPS5::Arm64::NativeThreads::Resolve(symbol);
            if (threadResolved != 0) return threadResolved;
            const auto mathResolved = AnyPS5::Arm64::NativeMath::Resolve(symbol);
            if (mathResolved != 0) return mathResolved;
            const auto posixResolved = AnyPS5::Arm64::NativePosix::Resolve(symbol);
            if (posixResolved != 0) return posixResolved;
            const auto localeResolved = AnyPS5::Arm64::NativeLocale::Resolve(symbol);
            if (localeResolved != 0) return localeResolved;
            const auto processResolved = AnyPS5::Arm64::NativeProcess::Resolve(symbol);
            if (processResolved != 0) return processResolved;
            const auto systemResolved = AnyPS5::Arm64::NativeSystem::Resolve(symbol);
            if (systemResolved != 0) return systemResolved;
            const auto audioResolved = AnyPS5::Arm64::NativeAudio::Resolve(symbol);
            if (audioResolved != 0) return audioResolved;
            const auto kernelResolved = kernel.Resolve(symbol);
            return kernelResolved != 0 ? kernelResolved : video.Resolve(symbol);
        };
        auto image = AnyPS5::Arm64::LoadElf(std::as_bytes(std::span(bytes.data(), bytes.size())), options);
        constexpr std::uint64_t stack = 0x7fffff000000u;
        constexpr std::size_t stackSize = 8 * 1024 * 1024;
        image.memory.Map(stack, stackSize, AnyPS5::Arm64::Read | AnyPS5::Arm64::Write);
        AnyPS5::Arm64::CpuState state;
        state.rip = image.entry;
        state.registers[4] = stack + stackSize - 8;
        AnyPS5::Arm64::Cpu cpu(image.memory);
        libc.Bind(cpu, image.memory);
        posix.SetArguments(image.memory, guestArguments);
        posix.Bind(cpu);
        locale.Bind(cpu, image.memory);
        kernel.Bind(cpu);
        video.Bind(cpu);
        math.Bind(cpu);
        process.SetFinalizers(image.finalizers);
        process.Bind(cpu);
        system.Bind(cpu);
        audio.Bind(cpu);
        AnyPS5::Arm64::NativeThreads threads(libc, image.memory, budget);
        threads.SetBindings([&](auto& threadCpu) { kernel.Bind(threadCpu); video.Bind(threadCpu); math.Bind(threadCpu); posix.Bind(threadCpu); locale.Bind(threadCpu, image.memory); process.Bind(threadCpu); system.Bind(threadCpu); audio.Bind(threadCpu); }, [&] { kernel.Shutdown(); posix.Shutdown(); audio.Shutdown(); });
        threads.Bind(cpu);
        const auto initialBudget = budget;
        const auto run = [&](std::uint64_t address) {
            state = {};
            state.rip = address;
            state.registers[4] = stack + stackSize - 8;
            const std::uint64_t sentinel = 0;
            if (!image.memory.TryWrite(state.registers[4], std::as_bytes(std::span(&sentinel, 1)))) throw std::runtime_error("Cannot initialize guest return sentinel");
            cpu.RunRemaining(state, budget);
        };
        std::uint64_t result = 0;
        std::exception_ptr failure;
        bool interrupted = false;
        try {
            for (const auto initializer : image.initializers) run(initializer);
            run(image.entry);
            result = state.registers[0];
            run(AnyPS5::Arm64::NativeProcess::CompletionAddress());
        } catch (const AnyPS5::Arm64::ExecutionStopped&) {
            interrupted = true;
            failure = std::current_exception();
        } catch (...) { failure = std::current_exception(); }
        try { threads.Finish(); }
        catch (...) { if (!failure || interrupted) failure = std::current_exception(); }
        try {
            if (failure) std::rethrow_exception(failure);
        } catch (const AnyPS5::Arm64::GuestExit& exit) {
            std::printf("Native ARM64 execution: guest %s with status %d, %zu translated blocks, %llu instructions\n",
                exit.Aborted() ? "aborted" : "exited", exit.Status(), cpu.CompiledBlocks(), static_cast<unsigned long long>(initialBudget - budget));
            return exit.Status();
        }
        std::printf("Native ARM64 execution: guest returned 0x%llx, %zu translated blocks, %llu instructions\n",
            static_cast<unsigned long long>(result), cpu.CompiledBlocks(), static_cast<unsigned long long>(initialBudget - budget));
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 2;
    }
}
