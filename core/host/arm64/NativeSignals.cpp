#include "NativeSignals.hpp"
#include <array>
#include <bit>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace AnyPS5::Arm64 {

namespace {

constexpr std::uint64_t functionBase = 0x7d0000000;
enum class Operation { Empty, Fill, Add, Delete, Member, Action, Signal, Mask, ThreadMask, Pending, Raise, Kill, ThreadKill, Pid, ParentPid, Group };
struct Export { std::string_view name; Operation operation; };
const auto exports = std::to_array<Export>({
    {"sigemptyset", Operation::Empty}, {"sigfillset", Operation::Fill}, {"sigaddset", Operation::Add},
    {"sigdelset", Operation::Delete}, {"sigismember", Operation::Member}, {"sigaction", Operation::Action},
    {"signal", Operation::Signal}, {"sigprocmask", Operation::Mask}, {"pthread_sigmask", Operation::ThreadMask},
    {"sigpending", Operation::Pending}, {"raise", Operation::Raise}, {"kill", Operation::Kill},
    {"pthread_kill", Operation::ThreadKill}, {"getpid", Operation::Pid}, {"getppid", Operation::ParentPid},
    {"getpgrp", Operation::Group}
});

using SignalSet = std::array<std::uint32_t, 4>;
struct Action { std::uint64_t handler = 0; std::uint32_t flags = 2; SignalSet mask{}; };
static_assert(sizeof(Action) == 32 && offsetof(Action, mask) == 12);
constexpr std::uint32_t unmaskable = (1u << 8) | (1u << 16);
constexpr std::uint32_t stopSignals = (1u << 16) | (1u << 17) | (1u << 20) | (1u << 21);

bool defaultIgnored(int signal) { return signal == 16 || signal == 19 || signal == 20 || signal == 23 || signal == 28 || signal == 29; }
bool stopSignal(int signal) { return signal == 17 || signal == 18 || signal == 21 || signal == 22; }
std::uint32_t signalBit(int signal) { return 1u << (signal - 1); }

template<class TValue>
TValue read(const GuestMemory& memory, std::uint64_t address) {
    TValue result{};
    if (!memory.TryRead(address, std::as_writable_bytes(std::span(&result, 1)))) throw std::runtime_error("Native signal argument became unreadable");
    return result;
}

template<class TValue>
void write(GuestMemory& memory, std::uint64_t address, const TValue& value) {
    if (!memory.TryWrite(address, std::as_bytes(std::span(&value, 1)))) throw std::runtime_error("Native signal argument became unwritable");
}

}

GuestSignalExit::GuestSignalExit(int signal) : std::runtime_error("Guest process terminated by signal " + std::to_string(signal)), signal(signal) {}
int GuestSignalExit::Signal() const { return signal; }
int GuestSignalExit::Status() const { return 128 + signal; }

struct NativeSignals::Impl {
    struct Thread { SignalSet mask{}; std::uint32_t pending = 0; };
    struct Binding { NativeSignals* owner; Cpu* cpu; std::uint64_t thread; };
    NativeLibc& libc;
    std::mutex gate;
    std::condition_variable changed;
    std::array<Action, 32> actions{};
    std::unordered_map<std::uint64_t, Thread> threads;
    std::vector<std::unique_ptr<Binding>> bindings;
    std::uint32_t pending = 0;
    bool stopped = false;
    bool shutdown = false;

    explicit Impl(NativeLibc& libc) : libc(libc) {}

    bool ignored(int signal) const {
        return actions[signal].handler == 1 || (actions[signal].handler == 0 && defaultIgnored(signal));
    }
    void discard(std::uint32_t bits) {
        pending &= ~bits;
        for (auto& [id, thread] : threads) thread.pending &= ~bits;
    }
    void prepare(int signal) {
        if (signal == 19) {
            stopped = false;
            discard(stopSignals);
        } else if (stopSignal(signal)) discard(signalBit(19));
        changed.notify_all();
    }
};

NativeSignals::NativeSignals(NativeLibc& libc) : impl(std::make_unique<Impl>(libc)) {}
NativeSignals::~NativeSignals() = default;

std::uint64_t NativeSignals::Resolve(const ElfImport& symbol) {
    if (symbol.type != 0 && symbol.type != 2) return 0;
    for (std::size_t i = 0; i < exports.size(); ++i) if (exports[i].name == symbol.name) return functionBase + i * 16;
    return 0;
}

void NativeSignals::RegisterThread(std::uint64_t thread, std::uint64_t parent) {
    std::lock_guard lock(impl->gate);
    if (impl->shutdown) throw ExecutionStopped();
    Impl::Thread state;
    if (parent != 0) state.mask = impl->threads.at(parent).mask;
    if (thread == 0 || !impl->threads.emplace(thread, state).second) throw std::runtime_error("Native signal thread registration is invalid");
}

void NativeSignals::UnregisterThread(std::uint64_t thread) {
    std::lock_guard lock(impl->gate);
    impl->threads.erase(thread);
}

void NativeSignals::Bind(Cpu& cpu, std::uint64_t thread) {
    Impl::Binding* binding;
    {
        std::lock_guard lock(impl->gate);
        if (!impl->threads.contains(thread)) throw std::runtime_error("Native signal thread is not registered");
        auto context = std::make_unique<Impl::Binding>(Impl::Binding{this, &cpu, thread});
        binding = context.get();
        impl->bindings.push_back(std::move(context));
    }
    cpu.AddObserver(+[](Cpu& cpu, CpuState& state, void* context) {
        const auto& binding = *static_cast<Impl::Binding*>(context);
        binding.owner->poll(cpu, state, binding.thread);
    }, binding);
    [&]<std::size_t... TIndex>(std::index_sequence<TIndex...>) {
        (cpu.Bind(functionBase + TIndex * 16, +[](const HostCall& call, void* context) {
            const auto& binding = *static_cast<Impl::Binding*>(context);
            return binding.owner->invoke(call, binding.thread, static_cast<std::size_t>(exports[TIndex].operation));
        }, binding), ...);
    }(std::make_index_sequence<exports.size()>{});
}

void NativeSignals::QueueProcess(int signal) {
    if (signal < 1 || signal > 31) throw std::invalid_argument("Native guest signal delivery supports signals 1 through 31");
    std::lock_guard lock(impl->gate);
    if (impl->shutdown) throw ExecutionStopped();
    impl->prepare(signal);
    if (!impl->ignored(signal)) impl->pending |= signalBit(signal);
}

void NativeSignals::Shutdown() {
    std::lock_guard lock(impl->gate);
    impl->shutdown = true;
    impl->changed.notify_all();
}

void NativeSignals::poll(Cpu& cpu, CpuState& state, std::uint64_t thread) {
    for (;;) {
        Action action;
        SignalSet previous;
        int signal;
        {
            std::unique_lock lock(impl->gate);
            impl->changed.wait(lock, [&] { return !impl->stopped || impl->shutdown || ((impl->pending | impl->threads.at(thread).pending) & signalBit(9)) != 0; });
            if (impl->shutdown) throw ExecutionStopped();
            auto& target = impl->threads.at(thread);
            const auto available = (target.pending | impl->pending) & ~target.mask[0] & (impl->stopped ? signalBit(9) : UINT32_MAX);
            if (available == 0) return;
            signal = std::countr_zero(available) + 1;
            const auto bit = signalBit(signal);
            if ((target.pending & bit) != 0) target.pending &= ~bit;
            else impl->pending &= ~bit;
            action = impl->actions[signal];
            if (impl->ignored(signal)) continue;
            if (action.handler == 0 && stopSignal(signal)) { impl->stopped = true; continue; }
            if (action.handler > 1) {
                previous = target.mask;
                for (std::size_t i = 0; i < previous.size(); ++i) target.mask[i] |= action.mask[i];
                if ((action.flags & 0x10) == 0) target.mask[0] |= bit;
                target.mask[0] &= ~unmaskable;
                if ((action.flags & 4) != 0) impl->actions[signal].handler = 0;
            }
        }
        if (action.handler == 0) {
            impl->libc.DiscardBufferedOutput();
            throw GuestSignalExit(signal);
        }
        struct Restore {
            Impl& impl;
            std::uint64_t thread;
            SignalSet mask;
            ~Restore() { std::lock_guard lock(impl.gate); impl.threads.at(thread).mask = mask; }
        } restore{*impl, thread, previous};
        static_cast<void>(cpu.Invoke(state, action.handler, {static_cast<std::uint64_t>(signal)}, true));
    }
}

void NativeSignals::Abort(const HostCall& call) {
    std::uint64_t thread = 0;
    {
        std::lock_guard lock(impl->gate);
        for (const auto& binding : impl->bindings) {
            if (binding->cpu == &call.cpu && impl->threads.contains(binding->thread)) {
                thread = binding->thread;
                break;
            }
        }
        if (thread == 0) throw std::runtime_error("Native abort signal thread is not registered");
        impl->threads.at(thread).mask[0] &= ~signalBit(6);
        if (impl->actions[6].handler == 1) impl->actions[6] = {};
        impl->threads.at(thread).pending |= signalBit(6);
    }
    poll(call.cpu, call.state, thread);
    {
        std::lock_guard lock(impl->gate);
        impl->actions[6] = {};
        impl->threads.at(thread).mask[0] &= ~signalBit(6);
    }
    impl->libc.DiscardBufferedOutput();
    throw GuestSignalExit(6);
}

std::uint64_t NativeSignals::invoke(const HostCall& call, std::uint64_t thread, std::size_t value) {
    const auto operation = static_cast<Operation>(value);
    const auto& args = call.arguments;
    auto& memory = call.memory;
    const auto error = [&](int code) -> std::uint64_t {
        if (operation == Operation::ThreadMask || operation == Operation::ThreadKill) return code;
        impl->libc.SetError(memory, code);
        return UINT64_MAX;
    };
    switch (operation) {
    case Operation::Pid:
    case Operation::Group: return 1;
    case Operation::ParentPid: return 0;
    case Operation::Empty:
    case Operation::Fill:
    case Operation::Add:
    case Operation::Delete:
    case Operation::Member: {
        const bool member = operation == Operation::Member;
        const bool modify = operation == Operation::Add || operation == Operation::Delete;
        const int signal = static_cast<std::int32_t>(args[1]);
        if ((modify || member) && (signal < 1 || signal > 128)) return error(22);
        if (!memory.Contains(args[0], sizeof(SignalSet), member ? Read : modify ? Read | Write : Write)) return error(14);
        SignalSet set{};
        if (member || modify) set = read<SignalSet>(memory, args[0]);
        if (operation == Operation::Fill) set.fill(UINT32_MAX);
        if (member || modify) {
            auto& word = set[(signal - 1) / 32];
            const auto bit = 1u << ((signal - 1) % 32);
            if (member) return (word & bit) != 0;
            if (operation == Operation::Add) word |= bit;
            else word &= ~bit;
        }
        write(memory, args[0], set);
        return 0;
    }
    case Operation::Action:
    case Operation::Signal: {
        const int signal = static_cast<std::int32_t>(args[0]);
        if (signal < 1 || signal > 128) return error(22);
        if (signal > 31) throw std::runtime_error("Native guest signal actions above signal 31 are not implemented");
        const bool simple = operation == Operation::Signal;
        const bool update = simple || args[1] != 0;
        if (!simple && ((args[1] && !memory.Contains(args[1], sizeof(Action), Read)) || (args[2] && !memory.Contains(args[2], sizeof(Action), Write)))) return error(14);
        Action action;
        if (simple) { action.handler = args[1]; action.flags = 2; }
        else if (update) action = read<Action>(memory, args[1]);
        if (update && action.handler == UINT64_MAX) return error(22);
        if (update && (signal == 9 || signal == 17) && action.handler != 0) return error(22);
        if (update && (action.flags & ~std::uint32_t{0x16}) != 0) throw std::runtime_error("Native signal action requires unsupported alternate-stack, siginfo or child-process behavior");
        action.mask[0] &= ~unmaskable;
        std::lock_guard lock(impl->gate);
        const auto previous = impl->actions[signal];
        if (!simple && args[2]) write(memory, args[2], previous);
        if (update) {
            impl->actions[signal] = action;
            if (impl->ignored(signal)) impl->discard(signalBit(signal));
        }
        return simple ? previous.handler : 0;
    }
    case Operation::Mask:
    case Operation::ThreadMask: {
        if ((args[1] && !memory.Contains(args[1], sizeof(SignalSet), Read)) || (args[2] && !memory.Contains(args[2], sizeof(SignalSet), Write))) return error(14);
        SignalSet set{};
        if (args[1]) set = read<SignalSet>(memory, args[1]);
        const auto how = static_cast<std::int32_t>(args[0]);
        if (args[1] && (how < 1 || how > 3)) return error(22);
        {
            std::lock_guard lock(impl->gate);
            auto& mask = impl->threads.at(thread).mask;
            if (args[2]) write(memory, args[2], mask);
            if (args[1]) {
                for (std::size_t i = 0; i < mask.size(); ++i) {
                    if (how == 1) mask[i] |= set[i];
                    else if (how == 2) mask[i] &= ~set[i];
                    else mask[i] = set[i];
                }
                mask[0] &= ~unmaskable;
            }
        }
        if (args[1]) poll(call.cpu, call.state, thread);
        return 0;
    }
    case Operation::Pending: {
        if (!memory.Contains(args[0], sizeof(SignalSet), Write)) return error(14);
        std::lock_guard lock(impl->gate);
        const SignalSet set{impl->pending | impl->threads.at(thread).pending, 0, 0, 0};
        write(memory, args[0], set);
        return 0;
    }
    case Operation::Raise:
    case Operation::Kill:
    case Operation::ThreadKill: {
        const int signal = static_cast<std::int32_t>(args[operation == Operation::Raise ? 0 : 1]);
        if (signal < (operation == Operation::Raise ? 1 : 0) || signal > 128) return error(22);
        if (signal > 31) throw std::runtime_error("Native queued and real-time signals above signal 31 are not implemented");
        if (operation == Operation::Kill) {
            const auto pid = static_cast<std::int32_t>(args[0]);
            if (pid != 1 && pid != 0 && pid != -1) return error(3);
            if (signal) QueueProcess(signal);
        } else {
            std::lock_guard lock(impl->gate);
            const auto target = impl->threads.find(operation == Operation::Raise ? thread : args[0]);
            if (target == impl->threads.end()) return error(3);
            if (signal) {
                impl->prepare(signal);
                if (!impl->ignored(signal)) target->second.pending |= signalBit(signal);
            }
        }
        if (signal) poll(call.cpu, call.state, thread);
        return 0;
    }
    }
    throw std::runtime_error("Unknown native signal operation");
}

}
