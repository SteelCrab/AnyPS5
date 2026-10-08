#include "NativeThreads.hpp"
#include <array>
#include <cstdio>
#include <limits>
#include <pthread.h>
#include <sched.h>
#include <stdexcept>
#include <string>

using namespace AnyPS5::Arm64;

namespace {

void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }

struct Fixture {
    GuestMemory memory;
    NativeLibc libc;
    Cpu cpu{memory};
    NativeThreads threads{libc, memory};
    Fixture() {
        memory.Map(0x1000, 0x1000, Read | Write);
        memory.Map(0x8000, 0x1000, Read | Write);
        libc.Bind(cpu, memory);
        threads.Bind(cpu);
    }
    template<class TValue>
    TValue Load(std::uint64_t address) {
        TValue value{};
        require(memory.TryRead(address, std::as_writable_bytes(std::span(&value, 1))), "Native synchronization guest read failed");
        return value;
    }
    std::uint64_t Call(std::string_view name, std::array<std::uint64_t, 6> args = {}, std::uint64_t budget = 1000000) {
        CpuState state;
        state.rip = threads.Resolve({name, 2, 1, 0});
        if (state.rip == 0) state.rip = libc.Resolve({name, 2, 1, 0});
        require(state.rip != 0, "Native synchronization function was not resolved");
        state.registers[4] = 0x8ff8;
        const std::array<std::uint8_t, 6> registers{7, 6, 2, 1, 8, 9};
        for (std::size_t i = 0; i < args.size(); ++i) state.registers[registers[i]] = args[i];
        cpu.Run(state, budget);
        return state.registers[0];
    }
};

void testMutexes() {
    Fixture fixture;
    require(fixture.threads.Resolve({"pthread_unknown", 2, 1, 0}) == 0, "Unknown thread function was falsely resolved");
    require(fixture.Call("pthread_mutex_lock", {0x1000}) == 0, "Static mutex initialization failed");
    require(fixture.Call("pthread_mutex_lock", {0x1000}) == 11 && fixture.Call("pthread_mutex_trylock", {0x1000}) == 16, "Owned default mutex did not detect deadlock or contention");
    require(fixture.Call("pthread_mutex_destroy", {0x1000}) == 16, "Owned mutex was destroyed");
    require(fixture.Call("pthread_mutex_unlock", {0x1000}) == 0 && fixture.Call("pthread_mutex_unlock", {0x1000}) == 1, "Mutex ownership errors changed");
    require(fixture.Call("pthread_mutex_destroy", {0x1000}) == 0 && fixture.Call("pthread_mutex_lock", {0x1000}) == 22, "Destroyed mutex was reinitialized by a lock operation");
    require(fixture.Call("pthread_mutexattr_init", {0x1100}) == 0 && fixture.Call("pthread_mutexattr_settype", {0x1100, 2}) == 0, "Recursive mutex attributes could not be created");
    require(fixture.Call("pthread_mutex_init", {0x1000, 0x1100}) == 0 && fixture.Call("pthread_mutex_lock", {0x1000}) == 0 && fixture.Call("pthread_mutex_lock", {0x1000}) == 0, "Recursive mutex did not allow nested acquisition");
    require(fixture.Call("pthread_mutex_unlock", {0x1000}) == 0 && fixture.Call("pthread_mutex_destroy", {0x1000}) == 16, "Recursive mutex lost its remaining lock depth");
    require(fixture.Call("pthread_mutex_unlock", {0x1000}) == 0 && fixture.Call("pthread_mutex_destroy", {0x1000}) == 0, "Recursive mutex release failed");
    require(fixture.Call("pthread_mutexattr_settype", {0x1100, 0}) == 22 && fixture.Call("pthread_mutexattr_destroy", {0x1100}) == 0, "Invalid mutex type was accepted");
    require(fixture.Call("pthread_attr_init", {0x1200}) == 0 && fixture.Call("pthread_attr_setstacksize", {0x1200, 1}) == 22 && fixture.Call("pthread_attr_setdetachstate", {0x1200, 2}) == 22, "Invalid thread attributes were accepted");
    require(fixture.Call("pthread_attr_destroy", {0x1200}) == 0, "Thread attribute destruction failed");
}

void testTimedSynchronization() {
    Fixture fixture;
    require(fixture.Call("pthread_mutex_lock", {0x1000}) == 0, "Timed condition mutex lock failed");
    require(fixture.Call("pthread_cond_timedwait", {0x1010, 0x1000, 0x1100}) == 60, "Expired condition wait did not time out");
    require(fixture.Call("pthread_mutex_trylock", {0x1000}) == 16 && fixture.Call("pthread_mutex_unlock", {0x1000}) == 0, "Timed condition wait did not reacquire its mutex");
    const std::array<std::int64_t, 2> invalid{-1, 0};
    require(fixture.memory.TryWrite(0x1100, std::as_bytes(std::span(invalid))), "Invalid deadline write failed");
    require(fixture.Call("pthread_cond_timedwait", {0x1010, 0x1000, 0x1100}) == 22, "Invalid absolute condition deadline was accepted");
    require(fixture.Call("pthread_rwlock_rdlock", {0x1020}) == 0 && fixture.Call("pthread_rwlock_rdlock", {0x1020}) == 0, "Reader lock did not allow repeated acquisition");
    require(fixture.Call("pthread_rwlock_trywrlock", {0x1020}) == 16 && fixture.Call("pthread_rwlock_wrlock", {0x1020}) == 11, "Reader-to-writer upgrade did not report contention or deadlock");
    require(fixture.Call("pthread_rwlock_destroy", {0x1020}) == 16 && fixture.Call("pthread_rwlock_unlock", {0x1020}) == 0 && fixture.Call("pthread_rwlock_unlock", {0x1020}) == 0, "Reader ownership or depth changed");
    require(fixture.Call("pthread_rwlock_wrlock", {0x1020}) == 0 && fixture.Call("pthread_rwlock_tryrdlock", {0x1020}) == 16 && fixture.Call("pthread_rwlock_unlock", {0x1020}) == 0, "Writer ownership errors changed");
    require(fixture.Call("pthread_rwlock_destroy", {0x1020}) == 0, "Reader-writer lock destruction failed");
    require(fixture.Call("sem_init", {0x1040, 0, 0}) == 0 && fixture.Call("sem_trywait", {0x1040}) == std::numeric_limits<std::uint64_t>::max(), "Empty semaphore try-wait did not fail");
    const auto error = fixture.Call("__error");
    require(fixture.Load<int>(error) == 35, "Semaphore try-wait did not set guest EAGAIN");
    require(fixture.Call("sem_timedwait", {0x1040, 0x1120}) == std::numeric_limits<std::uint64_t>::max() && fixture.Load<int>(error) == 60, "Expired semaphore wait did not set guest ETIMEDOUT");
    require(fixture.Call("sem_post", {0x1040}) == 0 && fixture.Call("sem_getvalue", {0x1040, 0x1130}) == 0 && fixture.Load<int>(0x1130) == 1, "Semaphore post did not preserve its count");
    require(fixture.Call("sem_wait", {0x1040}) == 0 && fixture.Call("sem_destroy", {0x1040}) == 0, "Semaphore consumption or destruction failed");
    require(fixture.Call("sem_post", {0x1040}) == std::numeric_limits<std::uint64_t>::max() && fixture.Load<int>(error) == 22, "Destroyed semaphore did not report guest EINVAL");
}

void testOnceBudget() {
    Fixture fixture;
    const std::array<std::byte, 6> initialize{std::byte{0xb8}, std::byte{42}, {}, {}, {}, std::byte{0xc3}};
    fixture.memory.Map(0x4000, initialize.size(), Read | Execute, initialize);
    try { fixture.Call("pthread_once", {0x1000, 0x4000}, 2); throw std::logic_error("Nested initializer exceeded its budget without failing"); }
    catch (const std::runtime_error& error) { require(std::string(error.what()).find("instruction budget exceeded") != std::string::npos, error.what()); }
    require(fixture.Load<int>(0x1000) == 0, "Failed once initializer did not reset its control state");
    require(fixture.Call("pthread_once", {0x1000, 0x4000}, 3) == 0 && fixture.Load<int>(0x1000) == 1, "Once initializer could not retry or did not complete");
    require(fixture.Call("pthread_once", {0x1000, 0x4000}, 1) == 0, "Completed once control ran its initializer again");
    const auto self = fixture.Call("pthread_self");
    require(self != 0 && fixture.Call("pthread_join", {self}) == 11, "Guest thread did not detect joining itself");
}

void testScheduling() {
    Fixture fixture;
    const auto self = fixture.Call("pthread_self");
    int policy = 0;
    sched_param parameters{};
    require(::pthread_getschedparam(pthread_self(), &policy, &parameters) == 0, "Cannot query the host scheduling policy");
    const auto guest = policy == SCHED_FIFO ? 1 : policy == SCHED_OTHER ? 2 : 3;
    require(fixture.Call("pthread_getschedparam", {self, 0x1100, 0x1110}) == 0 && fixture.Load<int>(0x1100) == guest && fixture.Load<int>(0x1110) == parameters.sched_priority, "Scheduling policy or priority was fabricated");
    for (int guestPolicy = 1; guestPolicy <= 3; ++guestPolicy) {
        const auto native = guestPolicy == 1 ? SCHED_FIFO : guestPolicy == 2 ? SCHED_OTHER : SCHED_RR;
        require(fixture.Call("sched_get_priority_min", {static_cast<unsigned>(guestPolicy)}) == static_cast<unsigned>(::sched_get_priority_min(native)) && fixture.Call("sched_get_priority_max", {static_cast<unsigned>(guestPolicy)}) == static_cast<unsigned>(::sched_get_priority_max(native)), "Guest priority query used the wrong native scheduling policy");
    }
    require(fixture.Call("pthread_setschedparam", {self, static_cast<unsigned>(guest), 0x1110}) == 0, "Cannot set the current native scheduling parameters");
    require(fixture.Call("pthread_getschedparam", {999999, 0x1100, 0x1110}) == 3 && fixture.Call("pthread_getschedparam", {self, 0xdead, 0x1110}) == 14, "Scheduling query accepted an invalid thread or pointer");
    require(fixture.Call("pthread_setschedparam", {self, 99, 0x1110}) == 22 && fixture.Call("sched_get_priority_max", {99}) == UINT64_MAX, "Unknown scheduling policy was accepted");
    require(fixture.Load<int>(fixture.Call("__error")) == 22, "Scheduling query error did not set guest errno");
    const auto maximum = ::sched_get_priority_max(policy) + 1;
    require(fixture.memory.TryWrite(0x1110, std::as_bytes(std::span(&maximum, 1))), "Cannot set invalid priority");
    require(fixture.Call("pthread_setschedparam", {self, static_cast<unsigned>(guest), 0x1110}) == 22, "Out-of-range native priority succeeded");
    sched_param after{};
    int afterPolicy = 0;
    require(::pthread_getschedparam(pthread_self(), &afterPolicy, &after) == 0 && policy == afterPolicy && parameters.sched_priority == after.sched_priority, "Invalid guest scheduling call changed host scheduling state");
}

}

int main() {
    try {
        testMutexes();
        testTimedSynchronization();
        testOnceBudget();
        testScheduling();
        std::puts("Native thread ownership, recursive locks, deadlines, semaphore errors and nested callback budgets passed");
        return 0;
    } catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
