#include "NativeVideoOut.hpp"
#include "MetalPresenter.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <unordered_map>

namespace AnyPS5::Arm64 {

namespace {

constexpr std::uint64_t functionBase = 0x740000000;
constexpr int invalidHandle = -2144796661;

struct Attribute {
    std::uint32_t reserved;
    std::uint32_t tiling;
    std::uint32_t aspect;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t pitch;
    std::uint64_t option;
    std::uint64_t format;
    std::uint64_t clearColor;
    std::uint32_t compression;
    std::uint32_t padding;
    std::array<std::uint64_t, 3> extra;
};
static_assert(sizeof(Attribute) == 80);

struct Buffer {
    std::uint64_t address;
    std::uint64_t metadata;
    std::array<std::uint64_t, 2> reserved;
};
static_assert(sizeof(Buffer) == 32);

template<class TValue>
TValue read(const GuestMemory& memory, std::uint64_t address) {
    TValue result{};
    if (!memory.TryRead(address, std::as_writable_bytes(std::span(&result, 1)))) throw std::runtime_error("Metal VideoOut argument is outside readable guest memory");
    return result;
}

template<class TValue>
TValue readStack(const HostCall& call, std::uint64_t offset) {
    if (offset > std::numeric_limits<std::uint64_t>::max() - call.stackArguments) throw std::runtime_error("Metal VideoOut stack argument address overflows");
    return read<TValue>(call.memory, call.stackArguments + offset);
}

void validate(const Attribute& attribute) {
    if (attribute.reserved != 0 || attribute.aspect != 0 || attribute.padding != 0 ||
        std::any_of(attribute.extra.begin(), attribute.extra.end(), [](auto value) { return value != 0; })) throw std::runtime_error("Metal VideoOut reserved attribute fields are set");
    if (attribute.tiling != 1 || attribute.format != 0x8000000000000000u || attribute.option != 0 ||
        attribute.compression != 0 || attribute.clearColor != 0) throw std::runtime_error("Metal VideoOut supports linear uncompressed BGRA8 buffers with option zero");
    if (attribute.width == 0 || attribute.height == 0 || attribute.width > 16384 || attribute.height > 16384 ||
        (attribute.pitch != 0 && (attribute.pitch < attribute.width || attribute.pitch > 16384))) throw std::runtime_error("Metal VideoOut dimensions or pitch are invalid");
}

std::size_t rowBytes(const Attribute& attribute) {
    return static_cast<std::size_t>(attribute.pitch == 0 ? attribute.width : attribute.pitch) * 4;
}

std::size_t byteCount(const Attribute& attribute) {
    const auto row = rowBytes(attribute);
    if (row > std::numeric_limits<std::size_t>::max() / attribute.height) throw std::runtime_error("Metal VideoOut buffer size overflows");
    return row * attribute.height;
}

std::uint64_t setAttribute(const HostCall& call, void*) {
    const Attribute attribute{0, static_cast<std::uint32_t>(call.arguments[2]), 0,
        static_cast<std::uint32_t>(call.arguments[3]), static_cast<std::uint32_t>(call.arguments[4]), 0,
        call.arguments[5], call.arguments[1], readStack<std::uint64_t>(call, 8),
        readStack<std::uint32_t>(call, 0), 0, {}};
    if (!call.memory.TryWrite(call.arguments[0], std::as_bytes(std::span(&attribute, 1)))) throw std::runtime_error("Metal VideoOut attribute is outside writable guest memory");
    return 0;
}

struct Function {
    std::string_view name;
    HostFunction function;
};

const auto functions = std::to_array<Function>({
    {"sceVideoOutOpen", +[](const HostCall& call, void* context) -> std::uint64_t {
        return static_cast<NativeVideoOut*>(context)->Open(static_cast<int>(call.arguments[0]), static_cast<int>(call.arguments[1]), static_cast<int>(call.arguments[2]), call.arguments[3]);
    }},
    {"sceVideoOutClose", +[](const HostCall& call, void* context) -> std::uint64_t {
        return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<NativeVideoOut*>(context)->Close(static_cast<int>(call.arguments[0]))));
    }},
    {"sceVideoOutRegisterBuffers2", +[](const HostCall& call, void* context) -> std::uint64_t {
        static_cast<NativeVideoOut*>(context)->Register(call.memory, call);
        return 0;
    }},
    {"sceVideoOutSetBufferAttribute2", setAttribute},
    {"sceVideoOutSubmitFlip", +[](const HostCall& call, void* context) -> std::uint64_t {
        static_cast<NativeVideoOut*>(context)->Flip(call.memory, static_cast<int>(call.arguments[0]), static_cast<int>(call.arguments[1]), static_cast<int>(call.arguments[2]), std::bit_cast<std::int64_t>(call.arguments[3]));
        return 0;
    }},
    {"sceVideoOutSetFlipRate", +[](const HostCall& call, void* context) -> std::uint64_t {
        static_cast<NativeVideoOut*>(context)->SetRate(static_cast<int>(call.arguments[0]), static_cast<int>(call.arguments[1]));
        return 0;
    }},
    {"sceVideoOutAddFlipEvent", +[](const HostCall& call, void* context) -> std::uint64_t {
        return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<NativeVideoOut*>(context)->AddFlipEvent(call.arguments[0], static_cast<int>(call.arguments[1]), call.arguments[2])));
    }},
    {"sceVideoOutDeleteFlipEvent", +[](const HostCall& call, void* context) -> std::uint64_t {
        return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<NativeVideoOut*>(context)->DeleteFlipEvent(call.arguments[0], static_cast<int>(call.arguments[1]))));
    }}
});

}

struct NativeVideoOut::Impl {
    NativeKernel& kernel;
    explicit Impl(NativeKernel& kernel) : kernel(kernel) {}
    struct Slot {
        Buffer buffer{};
        int group = -1;
    };
    struct Output {
        std::array<Slot, 16> slots{};
        std::array<Attribute, 4> attributes{};
        std::array<bool, 4> occupied{};
        std::unique_ptr<Metal::Presenter> presenter;
        std::uint32_t width = 0, height = 0;
        std::uint64_t flips = 0;
        std::int64_t argument = 0;
        int rate = 0;
        std::chrono::steady_clock::time_point nextFlip = std::chrono::steady_clock::now();
    };
    std::unordered_map<int, Output> outputs;
    int nextHandle = 1;
    Output& output(int handle) {
        const auto found = outputs.find(handle);
        if (found == outputs.end()) throw std::runtime_error("Metal VideoOut handle is invalid");
        return found->second;
    }
};

NativeVideoOut::NativeVideoOut(NativeKernel& kernel) : impl(std::make_unique<Impl>(kernel)) {}
NativeVideoOut::~NativeVideoOut() = default;

std::uint64_t NativeVideoOut::Resolve(const ElfImport& symbol) const {
    if (symbol.type != 0 && symbol.type != 2) return 0;
    for (std::size_t i = 0; i < functions.size(); ++i) {
        if (functions[i].name == symbol.name) return functionBase + i * 16;
    }
    return 0;
}

void NativeVideoOut::Bind(Cpu& cpu) {
    for (std::size_t i = 0; i < functions.size(); ++i) cpu.Bind(functionBase + i * 16, functions[i].function, this);
}

int NativeVideoOut::Open(int user, int bus, int index, std::uint64_t parameter) {
    if ((user != 0 && user != 255) || bus != 0 || index != 0 || parameter != 0) throw std::runtime_error("Metal VideoOut open parameters are not implemented");
    if (impl->outputs.size() == 4 || impl->nextHandle == std::numeric_limits<int>::max()) throw std::runtime_error("Metal VideoOut output capacity is full");
    const auto handle = impl->nextHandle++;
    impl->outputs.try_emplace(handle);
    return handle;
}

int NativeVideoOut::Close(int handle) {
    const auto found = impl->outputs.find(handle);
    if (found == impl->outputs.end()) return invalidHandle;
    if (found->second.presenter) found->second.presenter->Close();
    impl->kernel.CloseOutput(handle);
    impl->outputs.erase(found);
    return 0;
}

void NativeVideoOut::Register(GuestMemory& memory, const HostCall& call) {
    auto& output = impl->output(static_cast<int>(call.arguments[0]));
    const auto set = static_cast<int>(call.arguments[1]), start = static_cast<int>(call.arguments[2]), count = static_cast<int>(call.arguments[4]);
    if (set < 0 || set >= 4 || start < 0 || start >= 16 || count < 1 || count > 16 - start) throw std::runtime_error("Metal VideoOut buffer indices are invalid");
    if (readStack<std::uint32_t>(call, 0) != 0 || readStack<std::uint64_t>(call, 8) != 0) throw std::runtime_error("Metal VideoOut compression category or register option is not implemented");
    if (output.occupied[set]) throw std::runtime_error("Metal VideoOut attribute group is occupied");
    const auto attribute = read<Attribute>(memory, call.arguments[5]);
    validate(attribute);
    const auto size = byteCount(attribute);
    std::array<Buffer, 16> buffers{};
    if (!memory.Contains(call.arguments[3], static_cast<std::size_t>(count) * sizeof(Buffer), Read)) throw std::runtime_error("Metal VideoOut buffer list is outside readable guest memory");
    for (int i = 0; i < count; ++i) {
        if (output.slots[start + i].group >= 0) throw std::runtime_error("Metal VideoOut buffer slot is occupied");
        buffers[i] = read<Buffer>(memory, call.arguments[3] + static_cast<std::size_t>(i) * sizeof(Buffer));
        if (buffers[i].metadata != 0 || buffers[i].reserved[0] != 0 || buffers[i].reserved[1] != 0) throw std::runtime_error("Metal VideoOut buffer metadata or reserved fields are set");
        if (buffers[i].address == 0 || (buffers[i].address & 65535) != 0) throw std::runtime_error("Metal VideoOut framebuffer requires 64 KiB alignment");
        if (!memory.Contains(buffers[i].address, size, Read)) throw std::runtime_error("Metal VideoOut framebuffer is outside readable guest memory");
    }
    if (!output.presenter) {
        output.presenter = std::make_unique<Metal::Presenter>(attribute.width, attribute.height, "AnyPS5");
        output.width = attribute.width;
        output.height = attribute.height;
    } else if (output.width != attribute.width || output.height != attribute.height) {
        throw std::runtime_error("Metal VideoOut groups with different dimensions are not implemented");
    }
    output.attributes[set] = attribute;
    output.occupied[set] = true;
    for (int i = 0; i < count; ++i) output.slots[start + i] = Impl::Slot{buffers[i], set};
}

void NativeVideoOut::Flip(GuestMemory& memory, int handle, int index, int mode, std::int64_t argument) {
    auto& output = impl->output(handle);
    if (mode != 1) throw std::runtime_error("Metal VideoOut flip mode is not implemented; VSYNC mode 1 is required");
    if (index < 0 || index >= 16 || output.slots[index].group < 0) throw std::runtime_error("Metal VideoOut flip buffer index is invalid");
    const auto& slot = output.slots[index];
    const auto& attribute = output.attributes[slot.group];
    const auto size = byteCount(attribute);
    if (!memory.Contains(slot.buffer.address, size, Read)) throw std::runtime_error("Metal VideoOut framebuffer is outside readable guest memory");
    std::vector<std::byte> pixels(size);
    if (!memory.TryRead(slot.buffer.address, pixels)) throw std::runtime_error("Metal VideoOut framebuffer read failed");
    const auto now = std::chrono::steady_clock::now();
    if (output.nextFlip > now) std::this_thread::sleep_until(output.nextFlip);
    if (!output.presenter->Present(pixels, rowBytes(attribute), Metal::FrameFormat::Bgra8)) throw std::runtime_error("Metal VideoOut window was closed");
    ++output.flips;
    output.argument = argument;
    impl->kernel.TriggerFlip(handle, argument);
    output.nextFlip = std::max(output.nextFlip + std::chrono::nanoseconds((1000000000ll * (output.rate + 1)) / 60), std::chrono::steady_clock::now());
}

void NativeVideoOut::SetRate(int handle, int rate) {
    auto& output = impl->output(handle);
    if (rate < 0 || rate > 2) throw std::runtime_error("Metal VideoOut flip rate is invalid");
    output.rate = rate;
}

int NativeVideoOut::AddFlipEvent(std::uint64_t queue, int handle, std::uint64_t userData) {
    impl->output(handle);
    return impl->kernel.AddFlip(queue, handle, userData);
}

int NativeVideoOut::DeleteFlipEvent(std::uint64_t queue, int handle) {
    impl->output(handle);
    return impl->kernel.DeleteFlip(queue, handle);
}

std::vector<std::byte> NativeVideoOut::Readback(int handle) const {
    const auto& output = impl->output(handle);
    if (!output.presenter) throw std::runtime_error("Metal VideoOut has no registered framebuffer");
    return output.presenter->Readback();
}

std::array<std::uint32_t, 2> NativeVideoOut::FrameSize(int handle) const {
    const auto& output = impl->output(handle);
    if (!output.presenter) throw std::runtime_error("Metal VideoOut has no registered framebuffer");
    return output.presenter->FrameSize();
}

std::uint64_t NativeVideoOut::FlipCount(int handle) const { return impl->output(handle).flips; }
std::int64_t NativeVideoOut::FlipArgument(int handle) const { return impl->output(handle).argument; }

}
