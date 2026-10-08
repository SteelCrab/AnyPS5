#include "NativeAudio.hpp"
#include <AudioToolbox/AudioQueue.h>
#include <CoreAudio/CoreAudio.h>
#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace AnyPS5::Arm64 {

namespace {

constexpr std::uint64_t functionBase = 0x7c0000000;
constexpr int invalidHandle = -2144993277;
constexpr int invalidLength = -2144993274;
constexpr int invalidFrequency = -2144993272;
constexpr int invalidType = -2144993270;
constexpr int portsFull = -2144993275;
const auto names = std::to_array<std::string_view>({"sceAudioOutInit", "sceAudioOutOpen", "sceAudioOutOutput", "sceAudioOutClose"});

std::uint64_t integer(int value) { return static_cast<std::uint64_t>(static_cast<std::int64_t>(value)); }

void check(OSStatus status, const char* operation) {
    if (status != noErr) throw std::runtime_error(std::string(operation) + " failed with Core Audio status " + std::to_string(status));
}

struct Port {
    struct Buffer { AudioQueueBufferRef native = nullptr; bool queued = false; };
    AudioQueueRef queue = nullptr;
    const std::uint32_t frames;
    const std::uint32_t bytes;
    std::array<Buffer, 3> buffers{};
    std::mutex writer;
    std::mutex gate;
    std::condition_variable changed;
    bool closing = false;
    bool stopped = false;
    bool started = false;
    bool running = false;
    OSStatus failure = noErr;

    Port(std::uint32_t frames, std::uint32_t bytes) : frames(frames), bytes(bytes) {}
    ~Port() { if (queue) AudioQueueDispose(queue, true); }

    void Open(const AudioStreamBasicDescription& format) {
        check(AudioQueueNewOutput(&format, +[](void* context, AudioQueueRef, AudioQueueBufferRef buffer) {
            auto& port = *static_cast<Port*>(context);
            std::lock_guard lock(port.gate);
            const auto found = std::ranges::find_if(port.buffers, [&](const Buffer& entry) { return entry.native == buffer; });
            if (found == port.buffers.end() || !found->queued) port.failure = kAudioQueueErr_InvalidBuffer;
            else found->queued = false;
            port.changed.notify_all();
        }, this, nullptr, nullptr, 0, &queue), "AudioQueueNewOutput");
        check(AudioQueueAddPropertyListener(queue, kAudioQueueProperty_IsRunning, +[](void* context, AudioQueueRef queue, AudioQueuePropertyID property) {
            auto& port = *static_cast<Port*>(context);
            UInt32 running = 0;
            UInt32 size = sizeof(running);
            const auto status = AudioQueueGetProperty(queue, property, &running, &size);
            std::lock_guard lock(port.gate);
            if (status != noErr) port.failure = status;
            else port.running = running != 0;
            port.changed.notify_all();
        }, this), "AudioQueueAddPropertyListener");
        for (auto& buffer : buffers) check(AudioQueueAllocateBuffer(queue, bytes, &buffer.native), "AudioQueueAllocateBuffer");
    }

    void Wake() {
        std::lock_guard lock(gate);
        stopped = true;
        changed.notify_all();
    }

    void Close() {
        {
            std::lock_guard lock(gate);
            closing = true;
            changed.notify_all();
        }
        std::lock_guard lock(writer);
        if (!queue) return;
        check(AudioQueueDispose(queue, true), "AudioQueueDispose");
        queue = nullptr;
    }

    bool Alive() {
        if (stopped) throw ExecutionStopped();
        check(failure, "Core Audio output callback");
        return !closing;
    }

    std::uint64_t Output(std::span<const std::byte> samples) {
        std::lock_guard writing(writer);
        std::unique_lock lock(gate);
        if (!Alive()) return integer(invalidHandle);
        if (samples.empty()) {
            if (!started) return frames;
            lock.unlock();
            check(AudioQueueStop(queue, false), "AudioQueueStop");
            lock.lock();
            changed.wait(lock, [&] { return stopped || closing || failure != noErr || !running; });
            if (!Alive()) return integer(invalidHandle);
            started = false;
            return frames;
        }
        changed.wait(lock, [&] { return stopped || closing || failure != noErr || std::ranges::any_of(buffers, [](const Buffer& buffer) { return !buffer.queued; }); });
        if (!Alive()) return integer(invalidHandle);
        auto& buffer = *std::ranges::find_if(buffers, [](const Buffer& entry) { return !entry.queued; });
        std::memcpy(buffer.native->mAudioData, samples.data(), samples.size());
        buffer.native->mAudioDataByteSize = static_cast<UInt32>(samples.size());
        buffer.queued = true;
        lock.unlock();
        const auto status = AudioQueueEnqueueBuffer(queue, buffer.native, 0, nullptr);
        if (status != noErr) {
            lock.lock();
            buffer.queued = false;
            failure = status;
            changed.notify_all();
            check(status, "AudioQueueEnqueueBuffer");
        }
        if (!started) {
            {
                std::lock_guard starting(gate);
                running = true;
            }
            check(AudioQueueStart(queue, nullptr), "AudioQueueStart");
            started = true;
        }
        return frames;
    }
};

}

struct NativeAudio::Impl {
    std::mutex registry;
    std::unordered_map<int, std::shared_ptr<Port>> ports;
    int nextPort = 1;
    bool initialized = false;
    bool stopped = false;
};

NativeAudio::NativeAudio() : impl(std::make_unique<Impl>()) {}
NativeAudio::~NativeAudio() {
    Shutdown();
    for (const auto& [handle, port] : impl->ports) { try { port->Close(); } catch (...) {} }
}

std::uint64_t NativeAudio::Resolve(const ElfImport& symbol) {
    if (symbol.type != 0 && symbol.type != 2) return 0;
    for (std::size_t i = 0; i < names.size(); ++i) if (names[i] == symbol.name) return functionBase + i * 16;
    return 0;
}

void NativeAudio::Bind(Cpu& cpu) {
    [&]<std::size_t... TIndex>(std::index_sequence<TIndex...>) {
        (cpu.Bind(functionBase + TIndex * 16, +[](const HostCall& call, void* context) {
            return static_cast<NativeAudio*>(context)->invoke(call, TIndex);
        }, this), ...);
    }(std::make_index_sequence<names.size()>{});
}

void NativeAudio::Shutdown() {
    std::lock_guard lock(impl->registry);
    impl->stopped = true;
    for (const auto& [handle, port] : impl->ports) port->Wake();
}

std::uint64_t NativeAudio::invoke(const HostCall& call, std::size_t operation) {
    const auto& args = call.arguments;
    if (operation < 2) {
        std::lock_guard lock(impl->registry);
        if (impl->stopped) throw ExecutionStopped();
        if (operation == 0) {
            AudioObjectPropertyAddress property{kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
            AudioDeviceID device = kAudioObjectUnknown;
            UInt32 size = sizeof(device);
            check(AudioObjectGetPropertyData(kAudioObjectSystemObject, &property, 0, nullptr, &size, &device), "AudioObjectGetPropertyData");
            if (device == kAudioObjectUnknown) throw std::runtime_error("No native Core Audio output device is available");
            impl->initialized = true;
            return 0;
        }
        if (!impl->initialized) throw std::runtime_error("Native Core Audio must be initialized before opening a port");
        const auto type = static_cast<std::int32_t>(args[1]);
        if (type < 0 || (type > 4 && type != 10 && type != 126 && type != 127)) return integer(invalidType);
        if (type != 0 && type != 1) throw std::runtime_error("Native Core Audio supports main and BGM output ports");
        if (static_cast<std::uint32_t>(args[2]) != 0) throw std::runtime_error("Native Core Audio supports output index zero");
        const auto frames = static_cast<std::uint32_t>(args[3]);
        const auto frequency = static_cast<std::uint32_t>(args[4]);
        const auto param = static_cast<std::uint32_t>(args[5]);
        if (frequency == 0) return integer(invalidFrequency);
        if (param != 0 && param != 1 && param != 3 && param != 4) throw std::runtime_error("Native Core Audio supports interleaved S16/F32 mono or stereo with option zero");
        const bool floating = param == 3 || param == 4;
        const std::uint32_t channels = param == 0 || param == 3 ? 1 : 2;
        const std::uint32_t frameBytes = channels * (floating ? 4 : 2);
        if (frames == 0 || frames > UINT32_MAX / frameBytes) return integer(invalidLength);
        if (impl->ports.size() >= 32 || impl->nextPort == INT32_MAX) return integer(portsFull);
        AudioStreamBasicDescription format{};
        format.mSampleRate = frequency;
        format.mFormatID = kAudioFormatLinearPCM;
        format.mFormatFlags = kAudioFormatFlagIsPacked | (floating ? kAudioFormatFlagIsFloat : kAudioFormatFlagIsSignedInteger);
        format.mBytesPerPacket = frameBytes;
        format.mFramesPerPacket = 1;
        format.mBytesPerFrame = frameBytes;
        format.mChannelsPerFrame = channels;
        format.mBitsPerChannel = floating ? 32 : 16;
        auto port = std::make_shared<Port>(frames, frames * frameBytes);
        port->Open(format);
        const auto handle = impl->nextPort++;
        impl->ports.emplace(handle, std::move(port));
        return handle;
    }
    std::shared_ptr<Port> port;
    {
        std::lock_guard lock(impl->registry);
        if (impl->stopped) throw ExecutionStopped();
        const auto found = impl->ports.find(static_cast<std::int32_t>(args[0]));
        if (found == impl->ports.end()) return integer(invalidHandle);
        port = found->second;
        if (operation == 3) impl->ports.erase(found);
    }
    if (operation == 3) { port->Close(); return 0; }
    if (operation != 2) throw std::runtime_error("Invalid native Core Audio operation");
    std::vector<std::byte> samples;
    if (args[1] != 0) {
        if (!call.memory.Contains(args[1], port->bytes, Read)) throw std::runtime_error("Native Core Audio PCM is outside readable guest memory");
        samples.resize(port->bytes);
        if (!call.memory.TryRead(args[1], samples)) throw std::runtime_error("Native Core Audio PCM read failed");
    }
    return port->Output(samples);
}

}
