#include "ElfImage.hpp"
#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace AnyPS5::Arm64 {

namespace {

std::uint64_t read(std::span<const std::byte> bytes, std::size_t offset, std::size_t size) {
    if (offset > bytes.size() || size > bytes.size() - offset) throw std::runtime_error("ELF field is outside the input file");
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < size; ++i) value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[offset + i])) << (i * 8);
    return value;
}

std::uint64_t add(std::uint64_t left, std::uint64_t right) {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) throw std::runtime_error("ELF address overflows");
    return left + right;
}

std::uint64_t addSigned(std::uint64_t base, std::int64_t value) {
    if (value >= 0) return add(base, static_cast<std::uint64_t>(value));
    const auto magnitude = std::uint64_t{0} - static_cast<std::uint64_t>(value);
    if (base < magnitude) throw std::runtime_error("ELF address underflows");
    return base - magnitude;
}

struct Segment {
    std::uint64_t address;
    std::uint32_t permissions;
    std::vector<std::byte> bytes;
};

class ImageBytes {
public:
    std::vector<Segment> segments;
    std::uint64_t Read(std::uint64_t address, std::size_t size) const {
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < size; ++i) value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(At(add(address, i)))) << (i * 8);
        return value;
    }
    void Write(std::uint64_t address, std::uint64_t value, std::size_t size) {
        for (std::size_t i = 0; i < size; ++i) static_cast<void>(At(add(address, i)));
        for (std::size_t i = 0; i < size; ++i) {
            const auto target = add(address, i);
            for (auto& segment : segments) {
                if (target >= segment.address && target - segment.address < segment.bytes.size()) {
                    segment.bytes[target - segment.address] = static_cast<std::byte>(value >> (i * 8));
                    break;
                }
            }
        }
    }
    std::string String(std::uint64_t table, std::uint64_t size, std::uint64_t offset) const {
        if (offset >= size) throw std::runtime_error("ELF string offset is outside the string table");
        std::string value;
        for (; offset < size; ++offset) {
            const auto character = static_cast<char>(std::to_integer<std::uint8_t>(At(add(table, offset))));
            if (character == 0) return value;
            value.push_back(character);
        }
        throw std::runtime_error("ELF string is not terminated");
    }
    std::byte At(std::uint64_t address) const {
        for (const auto& segment : segments) {
            if (address >= segment.address && address - segment.address < segment.bytes.size()) return segment.bytes[address - segment.address];
        }
        throw std::runtime_error("ELF dynamic address is outside load segments");
    }
};

class DynamicImage {
public:
    DynamicImage(ImageBytes& bytes, ElfImage& image, const ElfOptions& options) : bytes(bytes), image(image), options(options) {}
    void Load(std::uint64_t address, std::uint64_t size) {
        if (size < 16) throw std::runtime_error("ELF dynamic table size is invalid");
        bool terminated = false;
        for (std::uint64_t offset = 0; offset <= size - 16; offset += 16) {
            const auto tag = bytes.Read(add(address, offset), 8);
            const auto value = bytes.Read(add(address, offset + 8), 8);
            if (tag == 0) { terminated = true; break; }
            if (tag == 1) { needed.push_back(value); continue; }
            if (!tags.emplace(tag, value).second) throw std::runtime_error("ELF dynamic tag is duplicated");
        }
        if (!terminated) throw std::runtime_error("ELF dynamic table is not terminated");
        for (const auto tag : {17u, 18u, 19u, 35u, 36u, 37u, 0x6ffffff0u, 0x6ffffffcu, 0x6ffffffeu}) {
            if (get(tag) != 0) throw std::runtime_error("ELF REL, RELR or versioned imports are not implemented");
        }
        for (auto offset : needed) image.libraries.push_back(bytes.String(get(5), get(10), offset));
        relocate(get(7), get(8), get(9));
        if (get(2) != 0) {
            if (get(20) != 7) throw std::runtime_error("ELF PLT relocations must use RELA");
            relocate(get(23), get(2), 24);
        }
        if (!missing.empty()) {
            std::string message = "Missing native ARM64 guest imports (" + std::to_string(missing.size()) + "):";
            for (const auto& name : missing) message += "\n  " + name;
            throw std::runtime_error(message);
        }
        appendArray(image.initializers, get(32), get(33));
        if (get(12) != 0) image.initializers.push_back(add(image.loadBias, get(12)));
        appendArray(image.initializers, get(25), get(27));
        appendArray(image.finalizers, get(26), get(28));
        std::reverse(image.finalizers.begin(), image.finalizers.end());
        if (get(13) != 0) image.finalizers.push_back(add(image.loadBias, get(13)));
    }
private:
    ImageBytes& bytes;
    ElfImage& image;
    const ElfOptions& options;
    std::unordered_map<std::uint64_t, std::uint64_t> tags;
    std::unordered_map<std::uint64_t, std::uint64_t> symbols;
    std::vector<std::uint64_t> needed;
    std::vector<std::string> missing;
    std::uint64_t get(std::uint64_t tag) const {
        const auto found = tags.find(tag);
        return found == tags.end() ? 0 : found->second;
    }
    std::uint64_t symbol(std::uint64_t index) {
        if (index == 0) return 0;
        if (const auto found = symbols.find(index); found != symbols.end()) return found->second;
        if (get(11) != 24 || get(6) == 0 || index > std::numeric_limits<std::uint64_t>::max() / 24) throw std::runtime_error("ELF dynamic symbol table is invalid");
        const auto address = add(get(6), index * 24);
        const auto nameOffset = bytes.Read(address, 4);
        const auto info = bytes.Read(add(address, 4), 1);
        const auto section = bytes.Read(add(address, 6), 2);
        const auto value = bytes.Read(add(address, 8), 8);
        const auto size = bytes.Read(add(address, 16), 8);
        const auto type = static_cast<std::uint8_t>(info & 15);
        const auto binding = static_cast<std::uint8_t>(info >> 4);
        if (type == 6 || type == 10 || section == 0xfff2) throw std::runtime_error("ELF TLS, IFUNC or common symbols are not implemented");
        std::uint64_t resolved = value;
        if (section == 0) {
            const auto name = bytes.String(get(5), get(10), nameOffset);
            if (name.empty() || binding == 0 || binding > 2) throw std::runtime_error("ELF undefined symbol is invalid");
            resolved = options.resolveImport ? options.resolveImport({name, type, binding, size}) : 0;
            if (resolved == 0 && binding != 2 && std::find(missing.begin(), missing.end(), name) == missing.end()) missing.push_back(name);
        } else if (section != 0xfff1) {
            if (section >= 0xff00) throw std::runtime_error("ELF symbol section is invalid");
            resolved = add(image.loadBias, value);
        }
        symbols.emplace(index, resolved);
        return resolved;
    }
    void relocate(std::uint64_t address, std::uint64_t size, std::uint64_t stride) {
        if (size == 0) return;
        if (address == 0 || stride != 24 || size % stride != 0) throw std::runtime_error("ELF RELA table is invalid");
        for (std::uint64_t offset = 0; offset < size; offset += stride) {
            const auto record = add(address, offset);
            const auto target = bytes.Read(record, 8);
            const auto info = bytes.Read(add(record, 8), 8);
            const auto addend = std::bit_cast<std::int64_t>(bytes.Read(add(record, 16), 8));
            const auto type = static_cast<std::uint32_t>(info);
            const auto symbolIndex = info >> 32;
            if (type == 0) continue;
            std::uint64_t value;
            std::size_t width = 8;
            if (type == 8) {
                if (symbolIndex != 0) throw std::runtime_error("ELF relative relocation has a symbol");
                value = addSigned(image.loadBias, addend);
            } else if (type == 6 || type == 7) {
                value = symbol(symbolIndex);
            } else if (type == 1 || type == 2 || type == 10 || type == 11) {
                value = symbol(symbolIndex) + static_cast<std::uint64_t>(addend);
                if (type == 2) {
                    const auto place = add(image.loadBias, target);
                    if ((value >= place && value - place > 0x7fffffffu) || (value < place && place - value > 0x80000000u)) throw std::runtime_error("ELF PC32 relocation overflows");
                    value -= place;
                } else if (type == 10 && value > 0xffffffffu) {
                    throw std::runtime_error("ELF 32-bit relocation overflows");
                } else if (type == 11 && value > 0x7fffffffu && value < 0xffffffff80000000u) {
                    throw std::runtime_error("ELF signed 32-bit relocation overflows");
                }
                if (type != 1) width = 4;
            } else {
                throw std::runtime_error("ELF relocation type " + std::to_string(type) + " is not implemented");
            }
            bytes.Write(target, value, width);
        }
    }
    void appendArray(std::vector<std::uint64_t>& functions, std::uint64_t address, std::uint64_t size) {
        if (size % 8 != 0 || (size != 0 && address == 0)) throw std::runtime_error("ELF initializer array is invalid");
        for (std::uint64_t offset = 0; offset < size; offset += 8) {
            const auto function = bytes.Read(add(address, offset), 8);
            if (function != 0 && function != std::numeric_limits<std::uint64_t>::max()) functions.push_back(function);
        }
    }
};

}

ElfImage LoadElf(std::span<const std::byte> bytes, const ElfOptions& options) {
    if (bytes.size() < 64 || read(bytes, 0, 4) != 0x464c457fu || read(bytes, 4, 3) != 0x010102u ||
        read(bytes, 18, 2) != 62 || read(bytes, 20, 4) != 1 || read(bytes, 52, 2) != 64) {
        throw std::runtime_error("ARM64 guest loader requires a little-endian ELF64 x86-64 image");
    }
    const auto type = read(bytes, 16, 2);
    if (type != 2 && type != 3) throw std::runtime_error("This guest ELF type is not implemented");
    const auto offset = read(bytes, 32, 8);
    const auto count = read(bytes, 56, 2);
    const auto stride = read(bytes, 54, 2);
    if (count == 0 || stride != 56 || offset > bytes.size() || count > (bytes.size() - offset) / stride) throw std::runtime_error("ELF program header table is invalid");
    ElfImage image{{}, 0, type == 3 ? options.loadBias : 0, {}, {}, {}};
    image.entry = add(image.loadBias, read(bytes, 24, 8));
    ImageBytes loaded;
    std::uint64_t dynamicAddress = 0, dynamicSize = 0;
    bool hasDynamic = false;
    for (std::size_t i = 0; i < count; ++i) {
        const auto header = static_cast<std::size_t>(offset + i * stride);
        const auto segmentType = read(bytes, header, 4);
        if (segmentType == 3) throw std::runtime_error("An ELF interpreter is not implemented in the ARM64 guest loader");
        if (segmentType == 7) throw std::runtime_error("ELF TLS is not implemented in the ARM64 guest loader");
        if (segmentType == 2) {
            if (hasDynamic) throw std::runtime_error("ELF dynamic segment is duplicated");
            hasDynamic = true;
            dynamicAddress = read(bytes, header + 16, 8);
            dynamicSize = read(bytes, header + 32, 8);
        }
        if (segmentType != 1) continue;
        const auto flags = read(bytes, header + 4, 4);
        const auto fileOffset = read(bytes, header + 8, 8);
        const auto address = read(bytes, header + 16, 8);
        const auto fileSize = read(bytes, header + 32, 8);
        const auto memorySize = read(bytes, header + 40, 8);
        const auto alignment = read(bytes, header + 48, 8);
        if (fileSize > memorySize || fileOffset > bytes.size() || fileSize > bytes.size() - fileOffset ||
            memorySize > std::numeric_limits<std::size_t>::max() || flags > 7 ||
            (alignment > 1 && ((alignment & (alignment - 1)) != 0 || address % alignment != fileOffset % alignment))) throw std::runtime_error("ELF load segment is invalid");
        if (memorySize == 0) continue;
        static_cast<void>(add(add(image.loadBias, address), memorySize));
        for (const auto& segment : loaded.segments) {
            if (address < add(segment.address, segment.bytes.size()) && segment.address < add(address, memorySize)) throw std::runtime_error("ELF load segments overlap");
        }
        Segment segment{address, static_cast<std::uint32_t>(flags), std::vector<std::byte>(static_cast<std::size_t>(memorySize))};
        std::copy_n(bytes.begin() + fileOffset, static_cast<std::size_t>(fileSize), segment.bytes.begin());
        loaded.segments.push_back(std::move(segment));
    }
    if (hasDynamic) DynamicImage(loaded, image, options).Load(dynamicAddress, dynamicSize);
    for (const auto& segment : loaded.segments) image.memory.Map(add(image.loadBias, segment.address), segment.bytes.size(), segment.permissions, segment.bytes);
    if (image.entry == 0 || image.memory.Fetch(image.entry, 1).empty()) throw std::runtime_error("ELF entry point is outside executable guest memory");
    for (const auto function : image.initializers) {
        if (image.memory.Fetch(function, 1).empty()) throw std::runtime_error("ELF initializer is outside executable guest memory");
    }
    for (const auto function : image.finalizers) {
        if (image.memory.Fetch(function, 1).empty()) throw std::runtime_error("ELF finalizer is outside executable guest memory");
    }
    return image;
}

}
