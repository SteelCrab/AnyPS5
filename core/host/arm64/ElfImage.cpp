#include "ElfImage.hpp"
#include <limits>
#include <stdexcept>

namespace AnyPS5::Arm64 {

namespace {

std::uint64_t read(std::span<const std::byte> bytes, std::size_t offset, std::size_t size) {
    if (offset > bytes.size() || size > bytes.size() - offset) throw std::runtime_error("ELF field is outside the input file");
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < size; ++i) value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[offset + i])) << (i * 8);
    return value;
}

}

ElfImage LoadElf(std::span<const std::byte> bytes) {
    if (bytes.size() < 64 || read(bytes, 0, 4) != 0x464c457fu || read(bytes, 4, 3) != 0x010102u ||
        read(bytes, 18, 2) != 62 || read(bytes, 20, 4) != 1 || read(bytes, 52, 2) != 64) {
        throw std::runtime_error("ARM64 guest loader requires a little-endian ELF64 x86-64 image");
    }
    const auto type = read(bytes, 16, 2);
    if (type != 2 && type != 3) throw std::runtime_error("This guest ELF type is not implemented");
    const auto offset = read(bytes, 32, 8);
    const auto count = read(bytes, 56, 2);
    const auto stride = read(bytes, 54, 2);
    if (count == 0 || stride != 56 || offset > bytes.size() || count > (bytes.size() - offset) / stride) {
        throw std::runtime_error("ELF program header table is invalid");
    }
    for (std::size_t i = 0; i < count; ++i) {
        const auto segmentType = read(bytes, offset + i * stride, 4);
        if (segmentType == 2 || segmentType == 3 || segmentType == 7) {
            throw std::runtime_error("Dynamic imports, an interpreter or TLS are not implemented in the ARM64 guest loader");
        }
    }
    ElfImage image{{}, read(bytes, 24, 8)};
    for (std::size_t i = 0; i < count; ++i) {
        const auto header = static_cast<std::size_t>(offset + i * stride);
        if (read(bytes, header, 4) != 1) continue;
        const auto flags = read(bytes, header + 4, 4);
        const auto fileOffset = read(bytes, header + 8, 8);
        const auto address = read(bytes, header + 16, 8);
        const auto fileSize = read(bytes, header + 32, 8);
        const auto memorySize = read(bytes, header + 40, 8);
        const auto alignment = read(bytes, header + 48, 8);
        if (fileSize > memorySize || fileOffset > bytes.size() || fileSize > bytes.size() - fileOffset ||
            memorySize > std::numeric_limits<std::size_t>::max() || flags > 7 ||
            (alignment > 1 && ((alignment & (alignment - 1)) != 0 || address % alignment != fileOffset % alignment))) {
            throw std::runtime_error("ELF load segment is invalid");
        }
        if (memorySize == 0) continue;
        image.memory.Map(address, static_cast<std::size_t>(memorySize), static_cast<std::uint32_t>(flags),
                         bytes.subspan(static_cast<std::size_t>(fileOffset), static_cast<std::size_t>(fileSize)));
    }
    if (image.entry == 0 || image.memory.Fetch(image.entry, 1).empty()) throw std::runtime_error("ELF entry point is outside executable guest memory");
    return image;
}

}
