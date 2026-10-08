#include "NativeAudio.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <thread>

using namespace AnyPS5::Arm64;

namespace {

void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }

struct Fixture {
    GuestMemory memory;
    NativeAudio audio;
    Cpu cpu{memory};
    Fixture() {
        memory.Map(0x1000, 0x10000, Read | Write);
        memory.Map(0x800000, 0x1000, Read | Write);
        audio.Bind(cpu);
    }
    std::uint64_t Call(std::string_view name, std::array<std::uint64_t, 6> arguments = {}) {
        CpuState state;
        state.rip = audio.Resolve({name, 2, 1, 0});
        state.registers[4] = 0x800ff8;
        const std::array<std::uint8_t, 6> registers{7, 6, 2, 1, 8, 9};
        for (std::size_t i = 0; i < arguments.size(); ++i) state.registers[registers[i]] = arguments[i];
        cpu.Run(state);
        return state.registers[0];
    }
};

void testPlayback() {
    Fixture fixture;
    bool uninitialized = false;
    try { fixture.Call("sceAudioOutOpen", {1, 0, 0, 480, 48000, 1}); }
    catch (const std::runtime_error&) { uninitialized = true; }
    require(uninitialized, "Audio port opened before initialization");
    require(fixture.Call("sceAudioOutInit") == 0, "Native Core Audio initialization failed");
    for (const auto format : {0u, 1u, 3u, 4u}) {
        const auto handle = fixture.Call("sceAudioOutOpen", {1, format == 4 ? 1u : 0u, 0, 480, 48000, format});
        require(static_cast<std::int64_t>(handle) > 0, "Native Core Audio PCM port did not open");
        require(fixture.Call("sceAudioOutOutput", {handle, 0}) == 480, "Draining an empty port failed");
        const auto start = std::chrono::steady_clock::now();
        for (int block = 0; block < 10; ++block) require(fixture.Call("sceAudioOutOutput", {handle, 0x1000}) == 480, "Native PCM block was not accepted");
        require(fixture.Call("sceAudioOutOutput", {handle, 0}) == 480, "Native audio drain failed");
        require(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(60), "Audio queue did not pace or drain real playback");
        require(fixture.Call("sceAudioOutOutput", {handle, 0x1000}) == 480 && fixture.Call("sceAudioOutOutput", {handle, 0}) == 480, "Drained audio queue did not restart");
        bool invalid = false;
        try { fixture.Call("sceAudioOutOutput", {handle, 0x10fff}); }
        catch (const std::runtime_error&) { invalid = true; }
        require(invalid, "Audio output accepted PCM beyond its guest mapping");
        require(fixture.Call("sceAudioOutClose", {handle}) == 0, "Native audio port did not close");
        require(static_cast<std::int32_t>(fixture.Call("sceAudioOutOutput", {handle, 0x1000})) == -2144993277, "Closed audio port remained usable");
    }
    require(static_cast<std::int32_t>(fixture.Call("sceAudioOutOpen", {1, 0, 0, 0, 48000, 1})) == -2144993274, "Zero audio block size succeeded");
    require(static_cast<std::int32_t>(fixture.Call("sceAudioOutOpen", {1, 0, 0, 480, 0, 1})) == -2144993272, "Zero audio frequency succeeded");
    const auto converted = fixture.Call("sceAudioOutOpen", {1, 0, 0, 441, 44100, 1});
    require(static_cast<std::int64_t>(converted) > 0 && fixture.Call("sceAudioOutOutput", {converted, 0x1000}) == 441 && fixture.Call("sceAudioOutOutput", {converted, 0}) == 441 && fixture.Call("sceAudioOutClose", {converted}) == 0, "Native Core Audio sample-rate conversion failed");
    require(static_cast<std::int32_t>(fixture.Call("sceAudioOutOpen", {1, 99, 0, 480, 48000, 1})) == -2144993270, "Invalid audio port type succeeded");
    bool unsupported = false;
    try { fixture.Call("sceAudioOutOpen", {1, 0, 0, 480, 48000, 2}); }
    catch (const std::runtime_error&) { unsupported = true; }
    require(unsupported, "Unsupported surround format was silently accepted");
}

void testShutdown() {
    Fixture fixture;
    fixture.Call("sceAudioOutInit");
    const auto handle = fixture.Call("sceAudioOutOpen", {1, 0, 0, 480, 48000, 1});
    std::atomic<bool> begun = false;
    std::atomic<bool> stopped = false;
    std::exception_ptr failure;
    std::thread worker([&] {
        begun = true;
        try { for (int i = 0; i < 10000; ++i) fixture.Call("sceAudioOutOutput", {handle, 0x1000}); }
        catch (const ExecutionStopped&) { stopped = true; }
        catch (...) { failure = std::current_exception(); }
    });
    while (!begun) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto start = std::chrono::steady_clock::now();
    fixture.audio.Shutdown();
    worker.join();
    if (failure) std::rethrow_exception(failure);
    require(stopped && std::chrono::steady_clock::now() - start < std::chrono::seconds(1), "Native audio wait did not observe shutdown");
}

}

int main() {
    try { testPlayback(); testShutdown(); }
    catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
    return 0;
}
