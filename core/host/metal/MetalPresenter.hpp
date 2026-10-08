#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace AnyPS5::Metal {

enum class FrameFormat { Rgba8, Bgra8 };

class Presenter {
public:
    Presenter(std::uint32_t width, std::uint32_t height, std::string title);
    ~Presenter();
    Presenter(const Presenter&) = delete;
    Presenter& operator=(const Presenter&) = delete;
    [[nodiscard]] std::string DeviceName() const;
    [[nodiscard]] bool Poll();
    [[nodiscard]] bool KeyPressed(std::uint16_t key) const;
    [[nodiscard]] bool Present(std::span<const std::byte> pixels, std::size_t bytesPerRow, FrameFormat format);
    [[nodiscard]] std::array<std::uint32_t, 2> FrameSize() const;
    [[nodiscard]] std::vector<std::byte> Readback() const;
    void Close();
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}
