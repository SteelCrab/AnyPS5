#include "NativeSystem.hpp"
#include "NativePosix.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <string>
#include <sys/sysctl.h>
#include <vector>

namespace AnyPS5::Arm64 {

namespace {

constexpr std::uint64_t functionBase = 0x7b0000000;
enum class ValueType { Integer, Unsigned64, String };
struct Node {
    std::string_view name;
    std::array<std::int32_t, 2> mib;
    const char* nativeName;
    ValueType type;
};
const auto nodes = std::to_array<Node>({
    {"hw.machine", {6, 1}, "hw.machine", ValueType::String},
    {"hw.model", {6, 2}, "hw.model", ValueType::String},
    {"hw.ncpu", {6, 3}, "hw.ncpu", ValueType::Integer},
    {"hw.byteorder", {6, 4}, "hw.byteorder", ValueType::Integer},
    {"hw.physmem", {6, 5}, "hw.memsize", ValueType::Unsigned64},
    {"hw.pagesize", {6, 7}, "hw.pagesize", ValueType::Integer},
    {"hw.machine_arch", {6, 11}, "hw.machine", ValueType::String},
    {"hw.realmem", {6, 12}, "hw.memsize", ValueType::Unsigned64},
    {"kern.ostype", {1, 1}, "kern.ostype", ValueType::String},
    {"kern.osrelease", {1, 2}, "kern.osrelease", ValueType::String},
    {"kern.version", {1, 4}, "kern.version", ValueType::String}
});

int query(const Node& node, std::vector<std::byte>& output) {
    std::size_t size = node.type == ValueType::Integer ? 4 : 8;
    if (node.type == ValueType::String && ::sysctlbyname(node.nativeName, nullptr, &size, nullptr, 0) != 0) return errno;
    output.resize(size);
    if (::sysctlbyname(node.nativeName, output.data(), &size, nullptr, 0) != 0) return errno;
    if (size != output.size()) return EIO;
    if (node.type == ValueType::String && (output.empty() || output.back() != std::byte{0})) return EIO;
    return 0;
}

}

NativeSystem::NativeSystem(NativeLibc& libc) : libc(libc) {}

std::uint64_t NativeSystem::Resolve(const ElfImport& symbol) {
    if (symbol.type != 0 && symbol.type != 2) return 0;
    if (symbol.name == "sysctl") return functionBase;
    if (symbol.name == "sysctlbyname") return functionBase + 16;
    return 0;
}

void NativeSystem::Bind(Cpu& cpu) {
    cpu.Bind(functionBase, +[](const HostCall& call, void* context) { return static_cast<NativeSystem*>(context)->invoke(call, false); }, this);
    cpu.Bind(functionBase + 16, +[](const HostCall& call, void* context) { return static_cast<NativeSystem*>(context)->invoke(call, true); }, this);
}

std::uint64_t NativeSystem::invoke(const HostCall& call, bool named) {
    const auto fail = [&](int error) { libc.SetError(call.memory, guestError(error)); return UINT64_MAX; };
    const Node* node = nullptr;
    if (named) {
        std::string name;
        for (std::size_t i = 0; i < 1024; ++i) {
            char byte;
            if (call.arguments[0] > UINT64_MAX - i || !call.memory.TryRead(call.arguments[0] + i, std::as_writable_bytes(std::span(&byte, 1)))) return fail(EFAULT);
            if (byte == 0) {
                for (const auto& candidate : nodes) if (candidate.name == name) { node = &candidate; break; }
                break;
            }
            name.push_back(byte);
            if (i == 1023) return fail(ENAMETOOLONG);
        }
    } else {
        const auto count = static_cast<std::uint32_t>(call.arguments[1]);
        if (count < 2 || count > 24) return fail(EINVAL);
        std::array<std::int32_t, 24> mib{};
        if (!call.memory.TryRead(call.arguments[0], std::as_writable_bytes(std::span(mib.data(), count)))) return fail(EFAULT);
        if (count == 2) for (const auto& candidate : nodes) if (candidate.mib[0] == mib[0] && candidate.mib[1] == mib[1]) { node = &candidate; break; }
    }
    const auto output = call.arguments[named ? 1 : 2];
    const auto lengthAddress = call.arguments[named ? 2 : 3];
    const auto replacement = call.arguments[named ? 3 : 4];
    const auto replacementSize = call.arguments[named ? 4 : 5];
    std::uint64_t capacity = 0;
    if (lengthAddress != 0 && (!call.memory.Contains(lengthAddress, 8, Read | Write) || !call.memory.TryRead(lengthAddress, std::as_writable_bytes(std::span(&capacity, 1))))) return fail(EFAULT);
    if (output != 0 && !call.memory.Contains(output, capacity, Write)) return fail(EFAULT);
    if (replacement != 0 && !call.memory.Contains(replacement, replacementSize, Read)) return fail(EFAULT);
    if (!node) return fail(ENOENT);
    if (replacement != 0) return fail(EPERM);
    std::vector<std::byte> bytes;
    const auto error = query(*node, bytes);
    if (error != 0) return fail(error);
    const auto copied = output != 0 ? std::min<std::uint64_t>(capacity, bytes.size()) : bytes.size();
    if (output != 0 && !call.memory.TryWrite(output, std::span(bytes.data(), copied))) return fail(EFAULT);
    if (lengthAddress != 0 && !call.memory.TryWrite(lengthAddress, std::as_bytes(std::span(&copied, 1)))) return fail(EFAULT);
    return output != 0 && copied < bytes.size() ? fail(ENOMEM) : 0;
}

}
