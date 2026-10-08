#include "NativeProcess.hpp"
#include <algorithm>
#include <array>
#include <iterator>
#include <new>
#include <utility>

namespace AnyPS5::Arm64 {

namespace {

constexpr std::uint64_t functionBase = 0x7a0000000;
enum class Operation { AtExit, CxaAtExit, CxaFinalize, Exit, ImmediateExit, Abort, Complete };
struct Export { std::string_view name; Operation operation; };
const auto exports = std::to_array<Export>({
    {"atexit", Operation::AtExit}, {"__cxa_atexit", Operation::CxaAtExit},
    {"__cxa_finalize", Operation::CxaFinalize}, {"exit", Operation::Exit},
    {"_Exit", Operation::ImmediateExit}, {"_exit", Operation::ImmediateExit}, {"abort", Operation::Abort}
});

}

GuestExit::GuestExit(int status, bool aborted) : std::runtime_error(aborted ? "Guest process aborted" : "Guest process exited"), status(status & 255), aborted(aborted) {}
int GuestExit::Status() const { return status; }
bool GuestExit::Aborted() const { return aborted; }

NativeProcess::NativeProcess(NativeLibc& libc) : libc(libc) {}

std::uint64_t NativeProcess::Resolve(const ElfImport& symbol) {
    if (symbol.type != 0 && symbol.type != 2) return 0;
    for (std::size_t i = 0; i < exports.size(); ++i) if (exports[i].name == symbol.name) return functionBase + i * 16;
    return 0;
}

std::uint64_t NativeProcess::CompletionAddress() { return functionBase + exports.size() * 16; }

void NativeProcess::Bind(Cpu& cpu) {
    [&]<std::size_t... TIndex>(std::index_sequence<TIndex...>) {
        (cpu.Bind(functionBase + TIndex * 16, +[](const HostCall& call, void* context) {
            return static_cast<NativeProcess*>(context)->invoke(call, static_cast<std::size_t>(exports[TIndex].operation));
        }, this), ...);
    }(std::make_index_sequence<exports.size()>{});
    cpu.Bind(CompletionAddress(), +[](const HostCall& call, void* context) {
        return static_cast<NativeProcess*>(context)->invoke(call, static_cast<std::size_t>(Operation::Complete));
    }, this);
}

void NativeProcess::SetFinalizers(std::vector<std::uint64_t> functions) {
    std::lock_guard lock(mutex);
    if (finalizersSet) throw std::runtime_error("Guest ELF finalizers were already set");
    if (std::ranges::find(functions, 0) != functions.end()) throw std::runtime_error("Guest ELF finalizer is null");
    finalizers = std::move(functions);
    finalizersSet = true;
}

void NativeProcess::finalize(const HostCall& call, std::uint64_t dso) {
    for (;;) {
        Callback callback{};
        {
            std::lock_guard lock(mutex);
            const auto found = std::find_if(callbacks.rbegin(), callbacks.rend(), [&](const Callback& value) {
                return dso == 0 || value.dso == dso;
            });
            if (found == callbacks.rend()) return;
            callback = *found;
            callbacks.erase(std::next(found).base());
        }
        static_cast<void>(call.cpu.Invoke(call.state, callback.function, {callback.argument}));
    }
}

void NativeProcess::finish(const HostCall& call) {
    finalize(call, 0);
    for (;;) {
        std::uint64_t function;
        {
            std::lock_guard lock(mutex);
            if (nextFinalizer == finalizers.size()) break;
            function = finalizers[nextFinalizer++];
        }
        static_cast<void>(call.cpu.Invoke(call.state, function));
    }
    finalize(call, 0);
    static_cast<void>(libc.FlushAll(call.memory));
}

std::uint64_t NativeProcess::invoke(const HostCall& call, std::size_t value) {
    const auto operation = static_cast<Operation>(value);
    switch (operation) {
    case Operation::AtExit:
    case Operation::CxaAtExit: {
        if (call.arguments[0] == 0) throw std::runtime_error("Guest termination callback is null");
        const bool destructor = operation == Operation::CxaAtExit;
        try {
            std::lock_guard lock(mutex);
            callbacks.push_back({call.arguments[0], destructor ? call.arguments[1] : 0, destructor ? call.arguments[2] : 0});
        } catch (const std::bad_alloc&) { libc.SetError(call.memory, 12); return UINT64_MAX; }
        return 0;
    }
    case Operation::CxaFinalize:
        finalize(call, call.arguments[0]);
        return 0;
    case Operation::Exit:
        finish(call);
        throw GuestExit(static_cast<int>(call.arguments[0]));
    case Operation::ImmediateExit:
        libc.DiscardBufferedOutput();
        throw GuestExit(static_cast<int>(call.arguments[0]));
    case Operation::Abort:
        libc.DiscardBufferedOutput();
        throw GuestExit(134, true);
    case Operation::Complete:
        finish(call);
        return 0;
    }
    throw std::runtime_error("Invalid native process operation");
}

}
