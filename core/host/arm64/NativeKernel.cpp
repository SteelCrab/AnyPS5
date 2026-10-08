#include "NativeKernel.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>

namespace AnyPS5::Arm64 {

namespace {

constexpr std::uint64_t functionBase = 0x750000000;
constexpr std::uint64_t physicalBase = 0x600000000000;
constexpr std::uint64_t physicalSize = 13824ull * 1024 * 1024;
constexpr std::uint64_t pageSize = 0x4000;

constexpr int error(unsigned value) { return std::bit_cast<std::int32_t>(0x80020000u | value); }
std::uint64_t result(int value) { return static_cast<std::uint64_t>(static_cast<std::int64_t>(value)); }

template<class TValue>
bool read(const GuestMemory& memory, std::uint64_t address, TValue& value) {
    return memory.TryRead(address, std::as_writable_bytes(std::span(&value, 1)));
}

template<class TValue>
bool write(GuestMemory& memory, std::uint64_t address, const TValue& value) {
    return memory.TryWrite(address, std::as_bytes(std::span(&value, 1)));
}

bool alignment(std::uint64_t value) { return value == 0 || (value >= pageSize && (value & (value - 1)) == 0); }
std::uint64_t align(std::uint64_t value, std::uint64_t boundary) {
    if (value > std::numeric_limits<std::uint64_t>::max() - (boundary - 1)) throw std::overflow_error("Native kernel address alignment overflows");
    return (value + boundary - 1) & ~(boundary - 1);
}

struct Event {
    std::uint64_t ident;
    std::int16_t filter;
    std::uint16_t flags;
    std::uint32_t count;
    std::uint64_t data;
    std::uint64_t userData;
};
static_assert(sizeof(Event) == 32);

struct Function { std::string_view name; HostFunction function; };
const auto functions = std::to_array<Function>({
    {"sceKernelAllocateMainDirectMemory", +[](const HostCall& call, void* context) { return result(static_cast<NativeKernel*>(context)->AllocateDirect(call, true)); }},
    {"sceKernelAllocateDirectMemory", +[](const HostCall& call, void* context) { return result(static_cast<NativeKernel*>(context)->AllocateDirect(call, false)); }},
    {"sceKernelMapDirectMemory", +[](const HostCall& call, void* context) { return result(static_cast<NativeKernel*>(context)->MapDirect(call)); }},
    {"sceKernelReleaseDirectMemory", +[](const HostCall& call, void* context) { return result(static_cast<NativeKernel*>(context)->ReleaseDirect(call)); }},
    {"sceKernelMunmap", +[](const HostCall& call, void* context) { return result(static_cast<NativeKernel*>(context)->Unmap(call)); }},
    {"sceKernelGetDirectMemorySize", +[](const HostCall&, void*) { return physicalSize; }},
    {"sceKernelCreateEqueue", +[](const HostCall& call, void* context) { return result(static_cast<NativeKernel*>(context)->CreateQueue(call)); }},
    {"sceKernelDeleteEqueue", +[](const HostCall& call, void* context) { return result(static_cast<NativeKernel*>(context)->DeleteQueue(call.arguments[0])); }},
    {"sceKernelWaitEqueue", +[](const HostCall& call, void* context) { return result(static_cast<NativeKernel*>(context)->WaitQueue(call)); }}
});

}

struct NativeKernel::Impl {
    struct Allocation { std::size_t size; int type; };
    struct Mapping { std::size_t size; std::uint64_t physical; };
    struct Pending { int output; Event event; };
    struct Queue {
        std::mutex mutex;
        std::condition_variable changed;
        std::unordered_map<int, std::uint64_t> flips;
        std::deque<Pending> pending;
        std::string name;
        bool closed = false;
    };
    std::mutex memoryMutex;
    std::map<std::uint64_t, Allocation> allocations;
    std::map<std::uint64_t, Mapping> mappings;
    std::uint64_t nextVirtual = 0x4000000000;
    GuestMemory* memory = nullptr;
    std::mutex queueMutex;
    std::unordered_map<std::uint64_t, std::shared_ptr<Queue>> queues;
    std::uint64_t nextQueue = 1;
    std::shared_ptr<Queue> queue(std::uint64_t handle) {
        std::lock_guard lock(queueMutex);
        const auto found = queues.find(handle);
        return found == queues.end() ? nullptr : found->second;
    }
    void requireMemory(GuestMemory& supplied) {
        if (memory && memory != &supplied) throw std::runtime_error("Native kernel cannot share physical allocations across guest images");
        memory = &supplied;
    }
};

NativeKernel::NativeKernel() : impl(std::make_unique<Impl>()) {}
NativeKernel::~NativeKernel() = default;

std::uint64_t NativeKernel::Resolve(const ElfImport& symbol) const {
    if (symbol.type != 0 && symbol.type != 2) return 0;
    for (std::size_t i = 0; i < functions.size(); ++i) if (functions[i].name == symbol.name) return functionBase + i * 16;
    return 0;
}

void NativeKernel::Bind(Cpu& cpu) {
    for (std::size_t i = 0; i < functions.size(); ++i) cpu.Bind(functionBase + i * 16, functions[i].function, this);
}

int NativeKernel::AllocateDirect(const HostCall& call, bool mainMemory) {
    const auto first = mainMemory ? 0 : call.arguments[0];
    const auto last = mainMemory ? physicalSize : call.arguments[1];
    const auto size = call.arguments[mainMemory ? 0 : 2];
    const auto requestedAlignment = call.arguments[mainMemory ? 1 : 3];
    const auto type = static_cast<int>(call.arguments[mainMemory ? 2 : 4]);
    const auto output = call.arguments[mainMemory ? 3 : 5];
    if (first >= last || last > physicalSize || size == 0 || (size & (pageSize - 1)) != 0 || !alignment(requestedAlignment) || type < 0 || type > 10 || output == 0) return error(22);
    if (!call.memory.Contains(output, 8, Write)) return error(14);
    const auto boundary = std::max(requestedAlignment, pageSize);
    std::lock_guard lock(impl->memoryMutex);
    impl->requireMemory(call.memory);
    auto candidate = align(first, boundary);
    for (const auto& [offset, allocation] : impl->allocations) {
        if (offset + allocation.size <= candidate) continue;
        if (candidate <= offset && size <= offset - candidate) break;
        candidate = align(offset + allocation.size, boundary);
    }
    if (candidate >= last || size > last - candidate) return error(12);
    try {
        impl->allocations.emplace(candidate, Impl::Allocation{static_cast<std::size_t>(size), type});
        call.memory.Map(physicalBase + candidate, size, Read | Write);
    } catch (const std::bad_alloc&) { impl->allocations.erase(candidate); return error(12); }
    catch (const std::system_error&) { impl->allocations.erase(candidate); return error(12); }
    catch (...) { impl->allocations.erase(candidate); throw; }
    if (!write(call.memory, output, candidate)) {
        call.memory.Unmap(physicalBase + candidate);
        impl->allocations.erase(candidate);
        return error(14);
    }
    return 0;
}

int NativeKernel::MapDirect(const HostCall& call) {
    const auto size = call.arguments[1], physical = call.arguments[4], requestedAlignment = call.arguments[5];
    const auto protection = static_cast<std::uint32_t>(call.arguments[2]), flags = static_cast<std::uint32_t>(call.arguments[3]);
    std::uint64_t requested = 0;
    if (call.arguments[0] == 0 || size == 0 || (size & (pageSize - 1)) != 0 || (physical & (pageSize - 1)) != 0 || !alignment(requestedAlignment) || (protection & ~0x37u) != 0 || (flags & ~0x90u) != 0) return error(22);
    if (!read(call.memory, call.arguments[0], requested) || !call.memory.Contains(call.arguments[0], 8, Write)) return error(14);
    std::lock_guard lock(impl->memoryMutex);
    impl->requireMemory(call.memory);
    const auto next = impl->allocations.upper_bound(physical);
    if (next == impl->allocations.begin()) return error(22);
    const auto allocation = std::prev(next);
    const auto offset = physical - allocation->first;
    if (offset >= allocation->second.size || size > allocation->second.size - offset) return error(22);
    const auto boundary = std::max(requestedAlignment, pageSize);
    std::uint64_t address = requested;
    if ((flags & 0x10u) != 0) {
        if (address == 0 || (address & (boundary - 1)) != 0) return error(22);
        if (!call.memory.Vacant(address, size)) return error(12);
    } else {
        if (address == 0 || (address & (boundary - 1)) != 0 || !call.memory.Vacant(address, size)) address = align(impl->nextVirtual, boundary);
        while (!call.memory.Vacant(address, size)) {
            if (address >= 0x10000000000 || size > 0x10000000000 - address) return error(12);
            address = align(address + size, boundary);
        }
        if (address >= 0x10000000000 || size > 0x10000000000 - address) return error(12);
    }
    const std::uint32_t permission = ((protection & 1) ? Read : 0) | ((protection & 2) ? Write : 0) | ((protection & 4) ? Execute : 0);
    try {
        impl->mappings.emplace(address, Impl::Mapping{static_cast<std::size_t>(size), physical});
        call.memory.MapAlias(address, size, permission, physicalBase + physical);
    } catch (...) { impl->mappings.erase(address); throw; }
    if (!write(call.memory, call.arguments[0], address)) {
        call.memory.Unmap(address);
        impl->mappings.erase(address);
        return error(14);
    }
    if ((flags & 0x10u) == 0) impl->nextVirtual = address + size;
    return 0;
}

int NativeKernel::ReleaseDirect(const HostCall& call) {
    const auto first = call.arguments[0], size = call.arguments[1];
    if ((first & (pageSize - 1)) != 0 || size == 0 || (size & (pageSize - 1)) != 0 || first >= physicalSize || size > physicalSize - first) return error(22);
    std::lock_guard lock(impl->memoryMutex);
    impl->requireMemory(call.memory);
    const auto next = impl->allocations.upper_bound(first);
    if (next == impl->allocations.begin()) return error(22);
    const auto found = std::prev(next);
    if (first - found->first >= found->second.size || size > found->second.size - (first - found->first)) return error(22);
    const auto last = first + size;
    std::map<std::uint64_t, Impl::Mapping> remaining;
    for (const auto& [address, mapping] : impl->mappings) {
        const auto mappingEnd = mapping.physical + mapping.size;
        if (mappingEnd <= first || mapping.physical >= last) { remaining.emplace(address, mapping); continue; }
        const auto start = std::max(first, mapping.physical), end = std::min(last, mappingEnd);
        if (mapping.physical < start) remaining.emplace(address, Impl::Mapping{static_cast<std::size_t>(start - mapping.physical), mapping.physical});
        if (mappingEnd > end) remaining.emplace(address + end - mapping.physical, Impl::Mapping{static_cast<std::size_t>(mappingEnd - end), end});
    }
    auto allocations = impl->allocations;
    const auto allocationStart = found->first, allocationEnd = found->first + found->second.size;
    const auto type = found->second.type;
    allocations.erase(allocationStart);
    if (allocationStart < first) allocations.emplace(allocationStart, Impl::Allocation{static_cast<std::size_t>(first - allocationStart), type});
    if (allocationEnd > last) allocations.emplace(last, Impl::Allocation{static_cast<std::size_t>(allocationEnd - last), type});
    for (const auto& [address, mapping] : impl->mappings) {
        const auto start = std::max(first, mapping.physical), end = std::min(last, mapping.physical + mapping.size);
        if (start < end) call.memory.Unmap(address + start - mapping.physical, end - start);
    }
    call.memory.Unmap(physicalBase + first, size);
    impl->allocations.swap(allocations);
    impl->mappings.swap(remaining);
    return 0;
}

int NativeKernel::Unmap(const HostCall& call) {
    const auto address = call.arguments[0], size = call.arguments[1];
    if (size == 0 || (address & (pageSize - 1)) != 0 || (size & (pageSize - 1)) != 0) return error(22);
    std::lock_guard lock(impl->memoryMutex);
    impl->requireMemory(call.memory);
    const auto next = impl->mappings.upper_bound(address);
    if (next == impl->mappings.begin()) return error(22);
    const auto found = std::prev(next);
    const auto start = found->first;
    const auto mapping = found->second;
    if (address - start >= mapping.size || size > mapping.size - (address - start)) return error(22);
    auto remaining = impl->mappings;
    remaining.erase(start);
    if (start < address) remaining.emplace(start, Impl::Mapping{static_cast<std::size_t>(address - start), mapping.physical});
    if (start + mapping.size > address + size) remaining.emplace(address + size, Impl::Mapping{static_cast<std::size_t>(start + mapping.size - address - size), mapping.physical + address + size - start});
    call.memory.Unmap(address, size);
    impl->mappings.swap(remaining);
    return 0;
}

int NativeKernel::CreateQueue(const HostCall& call) {
    if (call.arguments[0] == 0 || call.arguments[1] == 0) return error(22);
    if (!call.memory.Contains(call.arguments[0], 8, Write)) return error(14);
    auto owner = std::make_shared<Impl::Queue>();
    auto address = call.arguments[1];
    for (;;) {
        char character = 0;
        if (!read(call.memory, address, character)) return error(14);
        if (character == 0) break;
        owner->name.push_back(character);
        if (++address == 0) return error(14);
    }
    std::lock_guard lock(impl->queueMutex);
    if (impl->nextQueue > std::numeric_limits<std::int64_t>::max()) return error(12);
    const auto handle = impl->nextQueue;
    impl->queues.emplace(handle, owner);
    if (!write(call.memory, call.arguments[0], handle)) { impl->queues.erase(handle); return error(14); }
    ++impl->nextQueue;
    return 0;
}

int NativeKernel::DeleteQueue(std::uint64_t handle) {
    std::shared_ptr<Impl::Queue> owner;
    {
        std::lock_guard lock(impl->queueMutex);
        const auto found = impl->queues.find(handle);
        if (found == impl->queues.end()) return error(9);
        owner = found->second;
        impl->queues.erase(found);
    }
    {
        std::lock_guard lock(owner->mutex);
        owner->closed = true;
        owner->flips.clear();
        owner->pending.clear();
    }
    owner->changed.notify_all();
    return 0;
}

int NativeKernel::WaitQueue(const HostCall& call) {
    const auto owner = impl->queue(call.arguments[0]);
    if (!owner) return error(9);
    const auto count = static_cast<std::int32_t>(call.arguments[2]);
    if (count < 1 || call.arguments[3] == 0) return error(22);
    if (call.arguments[1] == 0 || !call.memory.Contains(call.arguments[1], static_cast<std::size_t>(count) * sizeof(Event), Write) || !call.memory.Contains(call.arguments[3], 4, Write)) return error(14);
    std::uint32_t timeout = 0;
    if (call.arguments[4] != 0 && !read(call.memory, call.arguments[4], timeout)) return error(14);
    std::unique_lock lock(owner->mutex);
    const auto ready = [&] { return owner->closed || !owner->pending.empty(); };
    if (call.arguments[4] == 0) owner->changed.wait(lock, ready);
    else if (timeout != 0) owner->changed.wait_for(lock, std::chrono::microseconds(timeout), ready);
    if (owner->closed) return error(9);
    const auto available = static_cast<std::int32_t>(std::min(owner->pending.size(), static_cast<std::size_t>(count)));
    if (!call.memory.Contains(call.arguments[1], static_cast<std::size_t>(available) * sizeof(Event), Write) || !write(call.memory, call.arguments[3], available)) return error(14);
    if (available == 0) return error(60);
    std::vector<Event> events;
    events.reserve(available);
    for (std::int32_t i = 0; i < available; ++i) events.push_back(owner->pending[i].event);
    if (!call.memory.TryWrite(call.arguments[1], std::as_bytes(std::span(events)))) return error(14);
    for (std::int32_t i = 0; i < available; ++i) owner->pending.pop_front();
    return 0;
}

int NativeKernel::AddFlip(std::uint64_t queue, int output, std::uint64_t userData) {
    const auto owner = impl->queue(queue);
    if (!owner) return error(9);
    std::lock_guard lock(owner->mutex);
    if (owner->closed) return error(9);
    owner->flips.insert_or_assign(output, userData);
    return 0;
}

int NativeKernel::DeleteFlip(std::uint64_t queue, int output) {
    const auto owner = impl->queue(queue);
    if (!owner) return error(9);
    std::lock_guard lock(owner->mutex);
    if (owner->closed) return error(9);
    owner->flips.erase(output);
    std::erase_if(owner->pending, [output](const auto& pending) { return pending.output == output; });
    return 0;
}

void NativeKernel::TriggerFlip(int output, std::int64_t argument) {
    const auto timestamp = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    std::lock_guard registry(impl->queueMutex);
    for (const auto& [handle, owner] : impl->queues) {
        std::lock_guard lock(owner->mutex);
        const auto found = owner->flips.find(output);
        if (owner->closed || found == owner->flips.end()) continue;
        const auto data = (timestamp & 0xfff) | 0x1000 | (static_cast<std::uint64_t>(argument) << 16);
        owner->pending.push_back({output, Event{0, -13, 1, 1, data, found->second}});
        owner->changed.notify_all();
    }
}

void NativeKernel::CloseOutput(int output) {
    std::lock_guard registry(impl->queueMutex);
    for (const auto& [handle, owner] : impl->queues) {
        std::lock_guard lock(owner->mutex);
        owner->flips.erase(output);
        std::erase_if(owner->pending, [output](const auto& pending) { return pending.output == output; });
    }
}

void NativeKernel::Shutdown() {
    std::lock_guard registry(impl->queueMutex);
    for (const auto& [handle, owner] : impl->queues) {
        std::lock_guard lock(owner->mutex);
        owner->closed = true;
        owner->pending.clear();
        owner->flips.clear();
        owner->changed.notify_all();
    }
    impl->queues.clear();
}

}
