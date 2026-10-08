#include "Cpu.hpp"
#include "ElfImage.hpp"
#include "NativeLibc.hpp"
#include "NativeVideoOut.hpp"
#include <charconv>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        std::fprintf(stderr, "usage: anyps5_arm64_run <x86-64.elf> [instruction-budget]\n");
        return 1;
    }
    try {
        std::uint64_t budget = 1000000;
        if (argc == 3) {
            const std::string_view value(argv[2]);
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), budget);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || budget == 0) throw std::runtime_error("Instruction budget must be a positive integer");
        }
        std::ifstream input(argv[1], std::ios::binary);
        if (!input) throw std::runtime_error("Cannot open guest ELF");
        const std::string bytes{std::istreambuf_iterator<char>(input), {}};
        AnyPS5::Arm64::NativeLibc libc;
        AnyPS5::Arm64::NativeVideoOut video;
        AnyPS5::Arm64::ElfOptions options;
        options.resolveImport = [&libc, &video](const auto& symbol) {
            const auto resolved = libc.Resolve(symbol);
            return resolved != 0 ? resolved : video.Resolve(symbol);
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
        video.Bind(cpu);
        const auto initialBudget = budget;
        const auto run = [&](std::uint64_t address) {
            state = {};
            state.rip = address;
            state.registers[4] = stack + stackSize - 8;
            const std::uint64_t sentinel = 0;
            if (!image.memory.TryWrite(state.registers[4], std::as_bytes(std::span(&sentinel, 1)))) throw std::runtime_error("Cannot initialize guest return sentinel");
            budget -= cpu.Run(state, budget);
        };
        for (const auto initializer : image.initializers) {
            run(initializer);
        }
        run(image.entry);
        const auto result = state.registers[0];
        for (const auto finalizer : image.finalizers) {
            run(finalizer);
        }
        std::printf("Native ARM64 execution: guest returned 0x%llx, %zu translated blocks, %llu instructions\n",
            static_cast<unsigned long long>(result), cpu.CompiledBlocks(), static_cast<unsigned long long>(initialBudget - budget));
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 2;
    }
}
