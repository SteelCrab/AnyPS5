#include "NativeThreads.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <limits>
#include <mutex>
#include <pthread.h>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>

namespace AnyPS5::Arm64 {

namespace {

constexpr std::uint64_t functionBase = 0x760000000;
enum class Operation {
    Create, Join, Detach, Self, Equal, Name, AttrInit, AttrDestroy, AttrDetach, AttrStack,
    MutexAttrInit, MutexAttrDestroy, MutexAttrType, MutexInit, MutexDestroy, MutexLock, MutexTry, MutexUnlock,
    CondInit, CondDestroy, CondSignal, CondBroadcast, CondWait, CondTimed,
    RwInit, RwDestroy, RwRead, RwWrite, RwTryRead, RwTryWrite, RwUnlock,
    KeyCreate, KeyDelete, GetSpecific, SetSpecific, Once,
    SemInit, SemDestroy, SemTry, SemWait, SemTimed, SemValue, SemPost
};
struct Export { std::string_view name; Operation operation; };
const auto exports = std::to_array<Export>({
    {"pthread_create", Operation::Create}, {"pthread_join", Operation::Join}, {"pthread_detach", Operation::Detach},
    {"pthread_self", Operation::Self}, {"pthread_equal", Operation::Equal}, {"pthread_set_name_np", Operation::Name},
    {"pthread_attr_init", Operation::AttrInit}, {"pthread_attr_destroy", Operation::AttrDestroy},
    {"pthread_attr_setdetachstate", Operation::AttrDetach}, {"pthread_attr_setstacksize", Operation::AttrStack},
    {"pthread_mutexattr_init", Operation::MutexAttrInit}, {"pthread_mutexattr_destroy", Operation::MutexAttrDestroy},
    {"pthread_mutexattr_settype", Operation::MutexAttrType}, {"pthread_mutex_init", Operation::MutexInit},
    {"pthread_mutex_destroy", Operation::MutexDestroy}, {"pthread_mutex_lock", Operation::MutexLock},
    {"pthread_mutex_trylock", Operation::MutexTry}, {"pthread_mutex_unlock", Operation::MutexUnlock},
    {"pthread_cond_init", Operation::CondInit}, {"pthread_cond_destroy", Operation::CondDestroy},
    {"pthread_cond_signal", Operation::CondSignal}, {"pthread_cond_broadcast", Operation::CondBroadcast},
    {"pthread_cond_wait", Operation::CondWait}, {"pthread_cond_timedwait", Operation::CondTimed},
    {"pthread_rwlock_init", Operation::RwInit}, {"pthread_rwlock_destroy", Operation::RwDestroy},
    {"pthread_rwlock_rdlock", Operation::RwRead}, {"pthread_rwlock_wrlock", Operation::RwWrite},
    {"pthread_rwlock_tryrdlock", Operation::RwTryRead}, {"pthread_rwlock_trywrlock", Operation::RwTryWrite},
    {"pthread_rwlock_unlock", Operation::RwUnlock}, {"pthread_key_create", Operation::KeyCreate},
    {"pthread_key_delete", Operation::KeyDelete}, {"pthread_getspecific", Operation::GetSpecific},
    {"pthread_setspecific", Operation::SetSpecific}, {"pthread_once", Operation::Once},
    {"sem_init", Operation::SemInit}, {"sem_destroy", Operation::SemDestroy}, {"sem_trywait", Operation::SemTry},
    {"sem_wait", Operation::SemWait}, {"sem_timedwait", Operation::SemTimed}, {"sem_getvalue", Operation::SemValue},
    {"sem_post", Operation::SemPost}
});

template<class TValue>
TValue read(const GuestMemory& memory, std::uint64_t address) {
    TValue value{};
    if (!memory.TryRead(address, std::as_writable_bytes(std::span(&value, 1)))) throw std::runtime_error("Native thread argument is outside readable guest memory");
    return value;
}

template<class TValue>
void write(GuestMemory& memory, std::uint64_t address, const TValue& value) {
    if (!memory.TryWrite(address, std::as_bytes(std::span(&value, 1)))) throw std::runtime_error("Native thread argument is outside writable guest memory");
}

std::uint64_t integer(int value) { return static_cast<std::uint64_t>(static_cast<std::int64_t>(value)); }

struct Timespec { std::int64_t seconds, nanoseconds; };
std::chrono::system_clock::time_point deadline(const GuestMemory& memory, std::uint64_t address) {
    const auto time = read<Timespec>(memory, address);
    if (time.seconds < 0 || time.nanoseconds < 0 || time.nanoseconds >= 1000000000 || time.seconds > (std::numeric_limits<std::int64_t>::max() - time.nanoseconds) / 1000000000) throw std::invalid_argument("Invalid native thread absolute deadline");
    return std::chrono::system_clock::time_point(std::chrono::ceil<std::chrono::system_clock::duration>(std::chrono::nanoseconds(time.seconds * 1000000000 + time.nanoseconds)));
}

}

struct NativeThreads::Impl {
    struct Mutex {
        std::mutex gate;
        std::condition_variable changed;
        std::thread::id owner;
        std::size_t depth = 0, waiters = 0;
        int type = 1;
        bool destroyed = false;
    };
    struct Condition {
        std::mutex gate;
        std::condition_variable changed;
        std::size_t waiters = 0;
        std::uint64_t epoch = 0;
        bool destroyed = false;
    };
    struct Rwlock {
        std::mutex gate;
        std::condition_variable changed;
        std::thread::id writer;
        std::unordered_map<std::thread::id, std::size_t> readers;
        std::size_t waiters = 0, writersWaiting = 0;
        bool destroyed = false;
    };
    struct Semaphore {
        std::mutex gate;
        std::condition_variable changed;
        unsigned value = 0;
        std::size_t waiters = 0;
        bool destroyed = false;
    };
    struct Attribute { bool mutex; int type = 1; bool detached = false; std::size_t stackSize = 8 * 1024 * 1024; };
    struct Thread {
        std::mutex gate;
        std::condition_variable changed;
        std::thread native;
        std::mutex joinMutex;
        std::uint64_t result = 0;
        std::exception_ptr failure;
        bool finished = false, detached = false, claimed = false, adopted = false;
        std::unordered_map<int, std::uint64_t> values;
    };
    struct Binding { NativeThreads* owner; Operation operation; };
    NativeLibc& libc;
    GuestMemory& memory;
    const std::uint64_t budget;
    std::mutex registry;
    std::condition_variable onceChanged;
    std::unordered_map<std::uint64_t, std::shared_ptr<Mutex>> mutexes;
    std::unordered_map<std::uint64_t, std::shared_ptr<Condition>> conditions;
    std::unordered_map<std::uint64_t, std::shared_ptr<Rwlock>> rwlocks;
    std::unordered_map<std::uint64_t, std::shared_ptr<Semaphore>> semaphores;
    std::unordered_map<std::uint64_t, Attribute> attributes;
    std::unordered_map<std::uint64_t, std::shared_ptr<Thread>> threads;
    std::unordered_map<std::thread::id, std::uint64_t> identities;
    std::unordered_map<int, std::uint64_t> keys;
    int nextKey = 0;
    std::uint64_t nextObject = 0x760100000, nextStack = 0x7f0000000000;
    std::array<Binding, exports.size()> bindings{};
    std::function<void(Cpu&)> attach;
    std::function<void()> shutdown;
    std::stop_source stop;
    std::atomic<bool> failed{false};
    std::mutex failureMutex;
    std::exception_ptr processFailure;

    Impl(NativeLibc& libc, GuestMemory& memory, std::uint64_t budget) : libc(libc), memory(memory), budget(budget) {
        if (budget == 0) throw std::invalid_argument("Native thread instruction budget is zero");
        auto main = std::make_shared<Thread>();
        main->adopted = true;
        identities.emplace(std::this_thread::get_id(), nextObject);
        threads.emplace(nextObject++, main);
    }
    std::uint64_t self() {
        std::lock_guard lock(registry);
        const auto found = identities.find(std::this_thread::get_id());
        if (found == identities.end()) throw std::runtime_error("Host thread is not registered with the native guest process");
        return found->second;
    }
    void running() {
        if (stop.stop_requested()) throw ExecutionStopped();
    }
    void check() {
        if (!failed.load(std::memory_order_acquire)) return;
        std::lock_guard lock(failureMutex);
        std::rethrow_exception(processFailure);
    }
    void fail(std::exception_ptr failure) {
        std::lock_guard lock(failureMutex);
        if (!processFailure) processFailure = failure;
        failed.store(true, std::memory_order_release);
    }
    template<class TObject>
    std::shared_ptr<TObject> object(std::unordered_map<std::uint64_t, std::shared_ptr<TObject>>& objects, std::uint64_t address, bool initialize) {
        std::lock_guard lock(registry);
        if (address == 0 || !memory.Contains(address, 8, Read | Write)) return nullptr;
        const auto handle = read<std::uint64_t>(memory, address);
        const auto found = objects.find(handle);
        if (found != objects.end()) return found->second;
        const bool adaptive = std::is_same_v<TObject, Mutex> && handle == 1;
        if (!initialize || (handle != 0 && !adaptive)) return nullptr;
        auto created = std::make_shared<TObject>();
        if constexpr (std::is_same_v<TObject, Mutex>) if (adaptive) created->type = 4;
        const auto token = nextObject++;
        objects.emplace(token, created);
        try { write(memory, address, token); } catch (...) { objects.erase(token); throw; }
        return created;
    }
    int lock(const std::shared_ptr<Mutex>& mutex, bool tryOnly) {
        if (!mutex) return 22;
        std::unique_lock lock(mutex->gate);
        if (mutex->destroyed) return 22;
        const auto id = std::this_thread::get_id();
        if (mutex->owner == id) {
            if (mutex->type == 2) {
                if (mutex->depth == std::numeric_limits<std::size_t>::max()) return 35;
                ++mutex->depth;
                return 0;
            }
            if (tryOnly) return 16;
            if (mutex->type == 1 || mutex->type == 4) return 11;
        }
        if (tryOnly && mutex->depth != 0) return 16;
        ++mutex->waiters;
        mutex->changed.wait(lock, [&] { return stop.stop_requested() || mutex->depth == 0; });
        --mutex->waiters;
        running();
        mutex->owner = id;
        mutex->depth = 1;
        return 0;
    }
    int unlock(const std::shared_ptr<Mutex>& mutex) {
        if (!mutex) return 22;
        std::lock_guard lock(mutex->gate);
        if (mutex->destroyed) return 22;
        if (mutex->depth == 0 || mutex->owner != std::this_thread::get_id()) return 1;
        if (--mutex->depth == 0) { mutex->owner = {}; mutex->changed.notify_one(); }
        return 0;
    }
    void finishKeys(Cpu& cpu, CpuState& state, const std::shared_ptr<Thread>& thread, std::uint64_t& remaining) {
        for (int pass = 0; pass < 4; ++pass) {
            std::vector<std::pair<std::uint64_t, std::uint64_t>> callbacks;
            {
                std::lock_guard lock(registry);
                for (auto& [key, value] : thread->values) {
                    const auto found = keys.find(key);
                    if (value != 0 && found != keys.end() && found->second != 0) { callbacks.emplace_back(found->second, value); value = 0; }
                }
            }
            if (callbacks.empty()) return;
            for (const auto& [function, value] : callbacks) {
                state.rip = function;
                state.registers[7] = value;
                const std::uint64_t sentinel = 0;
                write(memory, state.registers[4], sentinel);
                remaining -= cpu.Run(state, remaining, stop.get_token());
                state.registers[4] -= 8;
            }
        }
    }
};

NativeThreads::NativeThreads(NativeLibc& libc, GuestMemory& memory, std::uint64_t instructionBudget) : impl(std::make_unique<Impl>(libc, memory, instructionBudget)) {
    for (std::size_t i = 0; i < exports.size(); ++i) impl->bindings[i] = {this, exports[i].operation};
}

NativeThreads::~NativeThreads() {
    try { Finish(); } catch (...) {}
}

void NativeThreads::Finish() {
    impl->stop.request_stop();
    if (impl->shutdown) impl->shutdown();
    std::vector<std::shared_ptr<Impl::Thread>> threads;
    {
        std::lock_guard lock(impl->registry);
        for (const auto& [handle, thread] : impl->threads) {
            threads.push_back(thread);
            std::lock_guard lock(thread->gate);
            thread->changed.notify_all();
        }
        const auto wake = [](auto& objects) {
            for (const auto& [handle, object] : objects) { std::lock_guard lock(object->gate); object->changed.notify_all(); }
        };
        wake(impl->mutexes);
        wake(impl->conditions);
        wake(impl->rwlocks);
        wake(impl->semaphores);
    }
    impl->onceChanged.notify_all();
    for (const auto& thread : threads) {
        std::lock_guard lock(thread->joinMutex);
        if (thread->native.joinable()) thread->native.join();
    }
    impl->check();
}

std::uint64_t NativeThreads::Resolve(const ElfImport& symbol) {
    if (symbol.type != 0 && symbol.type != 2) return 0;
    for (std::size_t i = 0; i < exports.size(); ++i) if (exports[i].name == symbol.name) return functionBase + i * 16;
    return 0;
}

void NativeThreads::Bind(Cpu& cpu) {
    cpu.SetObserver(+[](void* context) { static_cast<Impl*>(context)->check(); }, impl.get());
    for (std::size_t i = 0; i < exports.size(); ++i) cpu.Bind(functionBase + i * 16, +[](const HostCall& call, void* context) {
        const auto& binding = *static_cast<Impl::Binding*>(context);
        return binding.owner->invoke(call, static_cast<std::size_t>(binding.operation));
    }, &impl->bindings[i]);
}

void NativeThreads::SetBindings(std::function<void(Cpu&)> attach, std::function<void()> shutdown) {
    std::lock_guard lock(impl->registry);
    if (impl->threads.size() != 1) throw std::runtime_error("Native thread providers must be configured before creating threads");
    impl->attach = std::move(attach);
    impl->shutdown = std::move(shutdown);
}

std::uint64_t NativeThreads::invoke(const HostCall& call, std::size_t operation) {
    if (&call.memory != &impl->memory) throw std::runtime_error("Native thread binding belongs to a different guest process");
    impl->running();
    const auto op = static_cast<Operation>(operation);
    const auto& args = call.arguments;
    auto& memory = call.memory;
    switch (op) {
    case Operation::Self: return impl->self();
    case Operation::Equal: return args[0] == args[1];
    case Operation::Name: {
        if (args[0] != impl->self()) return 45;
        std::string name;
        for (auto address = args[1];; ++address) {
            const auto character = read<char>(memory, address);
            if (character == 0) break;
            name.push_back(character);
            if (name.size() >= 64) return 34;
        }
        return pthread_setname_np(name.c_str());
    }
    case Operation::AttrInit:
    case Operation::MutexAttrInit: {
        if (!memory.Contains(args[0], 8, Write)) return 22;
        std::lock_guard lock(impl->registry);
        const auto token = impl->nextObject++;
        impl->attributes.emplace(token, Impl::Attribute{op == Operation::MutexAttrInit});
        try { write(memory, args[0], token); } catch (...) { impl->attributes.erase(token); throw; }
        return 0;
    }
    case Operation::AttrDestroy:
    case Operation::MutexAttrDestroy:
    case Operation::AttrDetach:
    case Operation::AttrStack:
    case Operation::MutexAttrType: {
        if (!memory.Contains(args[0], 8, Read | Write)) return 22;
        std::lock_guard lock(impl->registry);
        const auto handle = read<std::uint64_t>(memory, args[0]);
        const auto found = impl->attributes.find(handle);
        const bool mutex = op == Operation::MutexAttrDestroy || op == Operation::MutexAttrType;
        if (found == impl->attributes.end() || found->second.mutex != mutex) return 22;
        if (op == Operation::AttrDestroy || op == Operation::MutexAttrDestroy) { impl->attributes.erase(found); write<std::uint64_t>(memory, args[0], 0); }
        else if (op == Operation::AttrDetach) { if (args[1] > 1) return 22; found->second.detached = args[1] != 0; }
        else if (op == Operation::AttrStack) { if (args[1] < 0x4000 || args[1] > 0x100000000ull) return 22; found->second.stackSize = (args[1] + 15) & ~std::uint64_t{15}; }
        else { if (args[1] < 1 || args[1] > 4) return 22; found->second.type = static_cast<int>(args[1]); }
        return 0;
    }
    case Operation::Create: {
        if (!memory.Contains(args[0], 8, Write) || args[2] == 0) return 22;
        auto thread = std::make_shared<Impl::Thread>();
        std::unique_lock lock(impl->registry);
        for (auto entry = impl->threads.begin(); entry != impl->threads.end();) {
            const auto existing = entry->second;
            bool reap = false;
            { std::lock_guard lock(existing->gate); reap = existing->detached && existing->finished && !existing->claimed; }
            if (!reap) { ++entry; continue; }
            { std::lock_guard lock(existing->joinMutex); if (existing->native.joinable()) existing->native.join(); }
            entry = impl->threads.erase(entry);
        }
        Impl::Attribute attribute{false};
        if (args[1] != 0) {
            const auto found = impl->attributes.find(read<std::uint64_t>(memory, args[1]));
            if (found == impl->attributes.end() || found->second.mutex) return 22;
            attribute = found->second;
        }
        if (impl->nextStack > 0x7fff00000000ull - attribute.stackSize - 0x4000) return 35;
        const auto stack = impl->nextStack, handle = impl->nextObject++;
        impl->nextStack += attribute.stackSize + 0x4000;
        thread->detached = attribute.detached;
        memory.Map(stack, attribute.stackSize, Read | Write);
        try {
            impl->threads.emplace(handle, thread);
            write(memory, args[0], handle);
            const auto entry = args[2], argument = args[3];
            thread->native = std::thread([this, thread, handle, stack, attribute, entry, argument] {
                std::uint64_t returned = 0;
                std::exception_ptr failure;
                {
                    std::lock_guard lock(impl->registry);
                    impl->identities.emplace(std::this_thread::get_id(), handle);
                }
                try {
                    Cpu cpu(impl->memory);
                    impl->libc.Attach(cpu, impl->memory);
                    Bind(cpu);
                    if (impl->attach) impl->attach(cpu);
                    CpuState state;
                    state.rip = entry;
                    state.registers[4] = stack + attribute.stackSize - 8;
                    state.registers[7] = argument;
                    auto remaining = impl->budget;
                    remaining -= cpu.Run(state, remaining, impl->stop.get_token());
                    returned = state.registers[0];
                    state.registers[4] = stack + attribute.stackSize - 8;
                    impl->finishKeys(cpu, state, thread, remaining);
                } catch (const ExecutionStopped&) {}
                catch (...) { failure = std::current_exception(); }
                try { impl->memory.Unmap(stack); } catch (...) { if (!failure) failure = std::current_exception(); }
                if (failure) impl->fail(failure);
                {
                    std::lock_guard lock(impl->registry);
                    impl->identities.erase(std::this_thread::get_id());
                }
                {
                    std::lock_guard lock(thread->gate);
                    thread->result = returned;
                    thread->failure = failure;
                    thread->finished = true;
                }
                thread->changed.notify_all();
            });
        } catch (...) {
            impl->threads.erase(handle);
            memory.Unmap(stack);
            write<std::uint64_t>(memory, args[0], 0);
            throw;
        }
        return 0;
    }
    case Operation::Join:
    case Operation::Detach: {
        std::shared_ptr<Impl::Thread> thread;
        {
            std::lock_guard lock(impl->registry);
            const auto found = impl->threads.find(args[0]);
            if (found == impl->threads.end()) return 3;
            thread = found->second;
        }
        if (args[0] == impl->self() && op == Operation::Join) return 11;
        if (args[1] != 0 && op == Operation::Join && !memory.Contains(args[1], 8, Write)) return 22;
        std::unique_lock lock(thread->gate);
        if (thread->adopted || thread->detached || thread->claimed) return 22;
        if (op == Operation::Detach) { thread->detached = true; return 0; }
        thread->claimed = true;
        thread->changed.wait(lock, [&] { return impl->stop.stop_requested() || thread->finished; });
        impl->running();
        const auto returned = thread->result;
        const auto failure = thread->failure;
        lock.unlock();
        {
            std::lock_guard join(thread->joinMutex);
            if (thread->native.joinable()) thread->native.join();
        }
        {
            std::lock_guard registry(impl->registry);
            impl->threads.erase(args[0]);
        }
        if (failure) std::rethrow_exception(failure);
        if (args[1] != 0) write(memory, args[1], returned);
        return 0;
    }
    case Operation::MutexInit:
    case Operation::CondInit:
    case Operation::RwInit:
    case Operation::SemInit: {
        if (!memory.Contains(args[0], 8, Write)) {
            if (op != Operation::SemInit) return 22;
            impl->libc.SetError(memory, 22);
            return integer(-1);
        }
        std::lock_guard lock(impl->registry);
        if (op == Operation::SemInit && (args[1] != 0 || args[2] > std::numeric_limits<int>::max())) {
            impl->libc.SetError(memory, args[1] != 0 ? 45 : 22);
            return integer(-1);
        }
        int type = 1;
        if (args[1] != 0 && op != Operation::SemInit) {
            if (op != Operation::MutexInit) return 45;
            const auto found = impl->attributes.find(read<std::uint64_t>(memory, args[1]));
            if (found == impl->attributes.end() || !found->second.mutex) return 22;
            type = found->second.type;
        }
        const auto token = impl->nextObject++;
        if (op == Operation::MutexInit) { auto object = std::make_shared<Impl::Mutex>(); object->type = type; impl->mutexes.emplace(token, object); }
        else if (op == Operation::CondInit) impl->conditions.emplace(token, std::make_shared<Impl::Condition>());
        else if (op == Operation::RwInit) impl->rwlocks.emplace(token, std::make_shared<Impl::Rwlock>());
        else { auto object = std::make_shared<Impl::Semaphore>(); object->value = static_cast<unsigned>(args[2]); impl->semaphores.emplace(token, object); }
        write(memory, args[0], token);
        return 0;
    }
    case Operation::MutexLock: return impl->lock(impl->object(impl->mutexes, args[0], true), false);
    case Operation::MutexTry: return impl->lock(impl->object(impl->mutexes, args[0], true), true);
    case Operation::MutexUnlock: return impl->unlock(impl->object(impl->mutexes, args[0], false));
    case Operation::MutexDestroy:
    case Operation::CondDestroy:
    case Operation::RwDestroy:
    case Operation::SemDestroy: {
        const auto destroy = [&]<class TObject>(std::unordered_map<std::uint64_t, std::shared_ptr<TObject>>& objects) {
            std::lock_guard registry(impl->registry);
            if (!memory.Contains(args[0], 8, Read | Write)) return 22;
            const auto handle = read<std::uint64_t>(memory, args[0]);
            const auto found = objects.find(handle);
            if (found == objects.end()) {
                if (handle != 0 || op == Operation::SemDestroy) return 22;
                write<std::uint64_t>(memory, args[0], 2);
                return 0;
            }
            const auto object = found->second;
            std::lock_guard lock(object->gate);
            if (object->waiters != 0) return 16;
            if constexpr (std::is_same_v<TObject, Impl::Mutex>) { if (object->depth != 0) return 16; }
            if constexpr (std::is_same_v<TObject, Impl::Rwlock>) { if (object->writer != std::thread::id{} || !object->readers.empty()) return 16; }
            object->destroyed = true;
            objects.erase(found);
            write<std::uint64_t>(memory, args[0], 2);
            return 0;
        };
        if (op == Operation::MutexDestroy) return destroy(impl->mutexes);
        if (op == Operation::CondDestroy) return destroy(impl->conditions);
        if (op == Operation::RwDestroy) return destroy(impl->rwlocks);
        const auto status = destroy(impl->semaphores);
        if (status != 0) { impl->libc.SetError(memory, status); return integer(-1); }
        return 0;
    }
    case Operation::CondSignal:
    case Operation::CondBroadcast: {
        const auto condition = impl->object(impl->conditions, args[0], true);
        if (!condition) return 22;
        std::lock_guard lock(condition->gate);
        if (condition->destroyed) return 22;
        ++condition->epoch;
        if (op == Operation::CondSignal) condition->changed.notify_one();
        else condition->changed.notify_all();
        return 0;
    }
    case Operation::CondWait:
    case Operation::CondTimed: {
        const auto condition = impl->object(impl->conditions, args[0], true);
        const auto mutex = impl->object(impl->mutexes, args[1], false);
        if (!condition || !mutex) return 22;
        std::chrono::system_clock::time_point until{};
        if (op == Operation::CondTimed) {
            try { until = deadline(memory, args[2]); } catch (const std::invalid_argument&) { return 22; }
        }
        std::unique_lock lock(condition->gate);
        if (condition->destroyed) return 22;
        {
            std::lock_guard ownership(mutex->gate);
            if (mutex->owner != std::this_thread::get_id() || mutex->depth != 1) return 1;
        }
        const auto epoch = condition->epoch;
        ++condition->waiters;
        if (const auto status = impl->unlock(mutex); status != 0) { --condition->waiters; return status; }
        const auto ready = [&] { return impl->stop.stop_requested() || condition->epoch != epoch; };
        bool signaled = true;
        if (op == Operation::CondTimed) signaled = condition->changed.wait_until(lock, until, ready);
        else condition->changed.wait(lock, ready);
        --condition->waiters;
        lock.unlock();
        impl->running();
        if (const auto status = impl->lock(mutex, false); status != 0) return status;
        return signaled ? 0 : 60;
    }
    case Operation::RwRead:
    case Operation::RwWrite:
    case Operation::RwTryRead:
    case Operation::RwTryWrite:
    case Operation::RwUnlock: {
        const auto rwlock = impl->object(impl->rwlocks, args[0], op != Operation::RwUnlock);
        if (!rwlock) return 22;
        std::unique_lock lock(rwlock->gate);
        if (rwlock->destroyed) return 22;
        const auto id = std::this_thread::get_id();
        const bool writing = op == Operation::RwWrite || op == Operation::RwTryWrite;
        const bool tryOnly = op == Operation::RwTryRead || op == Operation::RwTryWrite;
        if (op == Operation::RwUnlock) {
            if (rwlock->writer == id) rwlock->writer = {};
            else {
                const auto found = rwlock->readers.find(id);
                if (found == rwlock->readers.end()) return 1;
                if (--found->second == 0) rwlock->readers.erase(found);
            }
            rwlock->changed.notify_all();
            return 0;
        }
        if (rwlock->writer == id || (writing && rwlock->readers.contains(id))) return tryOnly ? 16 : 11;
        const auto available = [&] { return rwlock->writer == std::thread::id{} && (writing ? rwlock->readers.empty() : rwlock->writersWaiting == 0 || rwlock->readers.contains(id)); };
        if (tryOnly && !available()) return 16;
        ++rwlock->waiters;
        if (writing) ++rwlock->writersWaiting;
        rwlock->changed.wait(lock, [&] { return impl->stop.stop_requested() || available(); });
        --rwlock->waiters;
        if (writing) --rwlock->writersWaiting;
        impl->running();
        if (writing) rwlock->writer = id;
        else ++rwlock->readers[id];
        return 0;
    }
    case Operation::KeyCreate: {
        if (!memory.Contains(args[0], 4, Write)) return 22;
        std::lock_guard lock(impl->registry);
        if (impl->keys.size() == 256 || impl->nextKey == std::numeric_limits<int>::max()) return 35;
        const auto key = impl->nextKey++;
        impl->keys.emplace(key, args[1]);
        write(memory, args[0], key);
        return 0;
    }
    case Operation::KeyDelete:
    case Operation::GetSpecific:
    case Operation::SetSpecific: {
        const auto id = impl->self();
        const auto key = static_cast<std::int32_t>(args[0]);
        std::lock_guard lock(impl->registry);
        const auto found = impl->keys.find(key);
        if (found == impl->keys.end()) return op == Operation::GetSpecific ? 0 : 22;
        if (op == Operation::KeyDelete) { impl->keys.erase(found); return 0; }
        auto& values = impl->threads.at(id)->values;
        if (op == Operation::SetSpecific) { values.insert_or_assign(key, args[1]); return 0; }
        const auto value = values.find(key);
        return value == values.end() ? 0 : value->second;
    }
    case Operation::Once: {
        if (!memory.Contains(args[0], 16, Read | Write) || args[1] == 0) return 22;
        std::unique_lock lock(impl->registry);
        impl->onceChanged.wait(lock, [&] { return impl->stop.stop_requested() || read<std::int32_t>(memory, args[0]) != 2; });
        impl->running();
        const auto state = read<std::int32_t>(memory, args[0]);
        if (state == 1) return 0;
        if (state != 0) return 22;
        write<std::int32_t>(memory, args[0], 2);
        lock.unlock();
        try { static_cast<void>(call.cpu.Invoke(call.state, args[1])); }
        catch (...) {
            lock.lock(); write<std::int32_t>(memory, args[0], 0); lock.unlock(); impl->onceChanged.notify_all(); throw;
        }
        lock.lock(); write<std::int32_t>(memory, args[0], 1); lock.unlock(); impl->onceChanged.notify_all();
        return 0;
    }
    case Operation::SemWait:
    case Operation::SemTry:
    case Operation::SemTimed:
    case Operation::SemPost:
    case Operation::SemValue: {
        const auto fail = [&](int error) { impl->libc.SetError(memory, error); return integer(-1); };
        const auto semaphore = impl->object(impl->semaphores, args[0], false);
        if (!semaphore) return fail(22);
        std::chrono::system_clock::time_point until{};
        if (op == Operation::SemTimed) {
            try { until = deadline(memory, args[1]); } catch (const std::invalid_argument&) { return fail(22); }
        }
        std::unique_lock lock(semaphore->gate);
        if (semaphore->destroyed) return fail(22);
        if (op == Operation::SemPost) {
            if (semaphore->value == std::numeric_limits<int>::max()) return fail(84);
            ++semaphore->value;
            semaphore->changed.notify_one();
            return 0;
        }
        if (op == Operation::SemValue) {
            write(memory, args[1], static_cast<int>(semaphore->value));
            return 0;
        }
        if (op == Operation::SemTry && semaphore->value == 0) return fail(35);
        ++semaphore->waiters;
        const auto ready = [&] { return impl->stop.stop_requested() || semaphore->value != 0; };
        bool signaled = true;
        if (op == Operation::SemTimed) signaled = semaphore->changed.wait_until(lock, until, ready);
        else semaphore->changed.wait(lock, ready);
        --semaphore->waiters;
        impl->running();
        if (!signaled) return fail(60);
        --semaphore->value;
        return 0;
    }
    }
    throw std::runtime_error("Invalid native thread operation");
}

}
