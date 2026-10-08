#include "MetalPresenter.hpp"
#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>

using namespace AnyPS5::Metal;

namespace {

void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }

template<class TFunction>
void rejects(TFunction function, const char* expected) {
    try { function(); } catch (const std::runtime_error& error) {
        if (std::string(error.what()).find(expected) != std::string::npos) return;
        throw;
    }
    throw std::runtime_error("Invalid Metal presentation operation was accepted");
}

void testPresentation() {
    rejects([] { Presenter invalid(0, 16, "invalid"); }, "dimensions");
    Presenter presenter(37, 29, "AnyPS5 native Metal validation");
    rejects([&] { static_cast<void>(presenter.Readback()); }, "no completed frame");
    for (const auto format : {FrameFormat::Rgba8, FrameFormat::Bgra8}) {
        constexpr std::size_t row = 40 * 4;
        std::vector<std::byte> pixels(row * 29, std::byte{0xff});
        for (std::uint32_t y = 0; y < 29; ++y) {
            for (std::uint32_t x = 0; x < 37; ++x) {
                const auto offset = y * row + x * 4;
                const auto red = static_cast<std::byte>(x * 5), blue = static_cast<std::byte>(y * 7);
                pixels[offset] = format == FrameFormat::Rgba8 ? red : blue;
                pixels[offset + 1] = static_cast<std::byte>(x ^ y);
                pixels[offset + 2] = format == FrameFormat::Rgba8 ? blue : red;
            }
        }
        require(presenter.Present(pixels, row, format), "Native Metal frame was not presented");
        const auto size = presenter.FrameSize();
        require(size[0] != 0 && size[1] != 0, "Metal drawable dimensions are empty");
        const auto result = presenter.Readback();
        for (std::uint32_t y = 0; y < size[1]; ++y) {
            for (std::uint32_t x = 0; x < size[0]; ++x) {
                const auto sourceX = static_cast<std::uint32_t>((std::uint64_t{2 * x + 1} * 37) / (2 * size[0]));
                const auto sourceY = static_cast<std::uint32_t>((std::uint64_t{2 * y + 1} * 29) / (2 * size[1]));
                const auto offset = (static_cast<std::size_t>(y) * size[0] + x) * 4;
                require(result[offset] == static_cast<std::byte>(sourceY * 7) &&
                        result[offset + 1] == static_cast<std::byte>(sourceX ^ sourceY) &&
                        result[offset + 2] == static_cast<std::byte>(sourceX * 5) && result[offset + 3] == std::byte{0xff},
                        "Presented Metal drawable has incorrect channels, row pitch, orientation or scaling");
            }
        }
        rejects([&] { static_cast<void>(presenter.Present(std::span(pixels).first(8), row, format)); }, "byte range");
    }
    bool rejected = false;
    std::thread worker([&] {
        try { static_cast<void>(presenter.Poll()); } catch (const std::runtime_error& error) {
            rejected = std::string(error.what()).find("main macOS thread") != std::string::npos;
        }
    });
    worker.join();
    require(rejected, "Metal window was accessed from a non-main thread");
    rejects([&] { static_cast<void>(presenter.KeyPressed(256)); }, "key code");
    presenter.Close();
    require(!presenter.Poll(), "Native window closure did not stop presentation");
    std::printf("Native Metal drawable, RGBA/BGRA, padded rows, scaling and window closure passed on %s\n", presenter.DeviceName().c_str());
}

}

int main() {
    try { testPresentation(); return 0; }
    catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
