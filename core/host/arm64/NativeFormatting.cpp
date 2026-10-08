#include "NativeFormatting.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <climits>
#include <cerrno>
#include <cwchar>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace AnyPS5::Arm64 {

namespace {

void write(GuestMemory& memory, std::uint64_t address, std::span<const std::byte> bytes) {
    if (!memory.TryWrite(address, bytes)) throw std::runtime_error("Guest format destination is outside writable guest memory");
}

template<class TValue>
TValue read(const GuestMemory& memory, std::uint64_t address) {
    TValue value{};
    if (!memory.TryRead(address, std::as_writable_bytes(std::span(&value, 1)))) throw std::runtime_error("Guest format argument is outside readable guest memory");
    return value;
}

template<class TCharacter>
std::basic_string<TCharacter> string(const GuestMemory& memory, std::uint64_t address, std::size_t maximum = std::numeric_limits<std::size_t>::max()) {
    std::basic_string<TCharacter> value;
    for (std::size_t i = 0; i < maximum; ++i) {
        const auto character = read<TCharacter>(memory, address);
        if (character == 0) break;
        value.push_back(character);
        if (address > std::numeric_limits<std::uint64_t>::max() - sizeof(character)) throw std::runtime_error("Guest format string address overflows");
        address += sizeof(character);
    }
    return value;
}

std::string wideString(const GuestMemory& memory, std::uint64_t address, std::size_t maximum) {
    std::string value;
    std::mbstate_t state{};
    while (value.size() < maximum) {
        const auto character = read<wchar_t>(memory, address);
        if (character == 0) break;
        std::array<char, MB_LEN_MAX> encoded{};
        const auto count = std::wcrtomb(encoded.data(), character, &state);
        if (count == std::numeric_limits<std::size_t>::max()) throw std::runtime_error("Guest wide format character cannot be converted in the host locale");
        if (count > maximum - value.size()) break;
        value.append(encoded.data(), count);
        if (value.size() == maximum) break;
        if (address > std::numeric_limits<std::uint64_t>::max() - sizeof(character)) throw std::runtime_error("Guest format string address overflows");
        address += sizeof(character);
    }
    return value;
}

struct VaList {
    std::uint32_t generalOffset;
    std::uint32_t vectorOffset;
    std::uint64_t stack;
    std::uint64_t registers;
};

class Arguments {
public:
    Arguments(const HostCall& call, std::size_t first) : call(call), general(first), stack(call.stackArguments) {}
    Arguments(const HostCall& call, VaList list) : call(call), list(list), saved(true) {
        if (list.generalOffset > 48 || list.generalOffset % 8 != 0 || list.vectorOffset < 48 || list.vectorOffset > 176 || (list.vectorOffset - 48) % 16 != 0) {
            throw std::runtime_error("Guest x86-64 va_list offsets are invalid");
        }
        stack = list.stack;
    }
    std::uint64_t Integer() {
        if (saved && list.generalOffset < 48) {
            const auto address = add(list.registers, list.generalOffset);
            list.generalOffset += 8;
            return read<std::uint64_t>(call.memory, address);
        }
        if (!saved && general < 6) return call.arguments[general++];
        return stackWord();
    }
    double Float() {
        if (saved && list.vectorOffset < 176) {
            const auto address = add(list.registers, list.vectorOffset);
            list.vectorOffset += 16;
            return read<double>(call.memory, address);
        }
        if (!saved && vector < 8) {
            double result;
            std::memcpy(&result, call.state.xmm[vector++].data(), sizeof(result));
            return result;
        }
        return std::bit_cast<double>(stackWord());
    }
private:
    const HostCall& call;
    std::size_t general = 0, vector = 0;
    std::uint64_t stack;
    VaList list{};
    bool saved = false;
    static std::uint64_t add(std::uint64_t address, std::uint64_t offset) {
        if (offset > std::numeric_limits<std::uint64_t>::max() - address) throw std::runtime_error("Guest va_list address overflows");
        return address + offset;
    }
    std::uint64_t stackWord() {
        const auto value = read<std::uint64_t>(call.memory, stack);
        stack = add(stack, 8);
        return value;
    }
};

template<class TValue>
std::string render(const std::string& token, TValue value) {
    const auto count = std::snprintf(nullptr, 0, token.c_str(), value);
    if (count < 0) throw std::runtime_error("Native format conversion failed");
    std::string result(static_cast<std::size_t>(count), '\0');
    if (std::snprintf(result.data(), result.size() + 1, token.c_str(), value) != count) throw std::runtime_error("Native format conversion length changed");
    return result;
}

std::string format(GuestMemory& memory, const std::string& input, Arguments& arguments) {
    std::string result;
    for (std::size_t position = 0; position < input.size();) {
        if (input[position] != '%') { result.push_back(input[position++]); continue; }
        if (++position < input.size() && input[position] == '%') { result.push_back('%'); ++position; continue; }
        std::string token = "%";
        while (position < input.size() && std::string_view("-+ #0'").find(input[position]) != std::string_view::npos) token.push_back(input[position++]);
        if (position < input.size() && input[position] == '*') {
            token += std::to_string(static_cast<std::int32_t>(arguments.Integer()));
            ++position;
        } else {
            while (position < input.size() && input[position] >= '0' && input[position] <= '9') token.push_back(input[position++]);
        }
        std::size_t precision = std::numeric_limits<std::size_t>::max();
        if (position < input.size() && input[position] == '.') {
            ++position;
            if (position < input.size() && input[position] == '*') {
                const auto supplied = static_cast<std::int32_t>(arguments.Integer());
                if (supplied >= 0) { precision = supplied; token += "." + std::to_string(supplied); }
                ++position;
            } else {
                std::string digits;
                while (position < input.size() && input[position] >= '0' && input[position] <= '9') digits.push_back(input[position++]);
                token += "." + digits;
                precision = 0;
                for (const auto digit : digits) {
                    if (precision > (std::numeric_limits<std::size_t>::max() - 9) / 10) throw std::runtime_error("Guest format precision overflows");
                    precision = precision * 10 + static_cast<std::size_t>(digit - '0');
                }
            }
        }
        std::string length;
        if (position < input.size() && std::string_view("hljztL").find(input[position]) != std::string_view::npos) {
            length.push_back(input[position++]);
            if (position < input.size() && (length == "h" || length == "l") && input[position] == length[0]) length.push_back(input[position++]);
        }
        if (position == input.size()) throw std::runtime_error("Guest format conversion is truncated");
        const auto conversion = input[position++];
        if (conversion == '$') throw std::runtime_error("Positional guest format arguments are not implemented");
        if (std::string_view("diuoxXn").find(conversion) != std::string_view::npos) {
            if (length == "L") throw std::runtime_error("Guest integer format length is invalid");
            const auto width = length == "hh" ? 8 : length == "h" ? 16 : length.empty() ? 32 : 64;
            const auto value = arguments.Integer();
            if (conversion == 'n') {
                const auto count = static_cast<std::uint64_t>(result.size());
                write(memory, value, std::as_bytes(std::span(&count, 1)).first(width / 8));
            } else if (conversion == 'd' || conversion == 'i') {
                const auto signedValue = width == 8 ? static_cast<std::int8_t>(value) : width == 16 ? static_cast<std::int16_t>(value) : width == 32 ? static_cast<std::int32_t>(value) : std::bit_cast<std::int64_t>(value);
                result += render(token + "ll" + conversion, static_cast<long long>(signedValue));
            } else {
                const auto mask = width == 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << width) - 1;
                result += render(token + "ll" + conversion, static_cast<unsigned long long>(value & mask));
            }
        } else if (std::string_view("fFeEgGaA").find(conversion) != std::string_view::npos) {
            if (!length.empty() && length != "l") throw std::runtime_error("Guest extended floating-point format is not implemented");
            result += render(token + conversion, arguments.Float());
        } else if (conversion == 's') {
            const auto address = arguments.Integer();
            if (length == "l") {
                const auto value = wideString(memory, address, precision);
                result += render(token + "s", value.c_str());
            } else if (length.empty()) {
                const auto value = string<char>(memory, address, precision);
                result += render(token + "s", value.c_str());
            } else throw std::runtime_error("Guest string format length is invalid");
        } else if (conversion == 'c') {
            if (length == "l") result += render(token + "lc", static_cast<wint_t>(arguments.Integer()));
            else if (length.empty()) result += render(token + "c", static_cast<int>(arguments.Integer()));
            else throw std::runtime_error("Guest character format length is invalid");
        } else if (conversion == 'p' && length.empty()) {
            result += render(token + "p", reinterpret_cast<void*>(arguments.Integer()));
        } else throw std::runtime_error("Guest format conversion is not implemented");
        if (result.size() > INT_MAX) throw std::runtime_error("Guest formatted output exceeds INT_MAX");
    }
    return result;
}

}

std::uint64_t formatOutput(const HostCall& call, NativeLibc& libc, FormatOutput output, bool vaList) {
    const std::size_t formatIndex = output == FormatOutput::Standard ? 0 : output == FormatOutput::BoundedBuffer ? 2 : 1;
    auto arguments = vaList ? Arguments(call, read<VaList>(call.memory, call.arguments[formatIndex + 1])) : Arguments(call, formatIndex + 1);
    const auto formatted = format(call.memory, string<char>(call.memory, call.arguments[formatIndex]), arguments);
    if (output == FormatOutput::Standard || output == FormatOutput::File) {
        auto* file = output == FormatOutput::Standard ? stdout : libc.File(call.arguments[0]);
        const auto written = std::fwrite(formatted.data(), 1, formatted.size(), file);
        if (output == FormatOutput::File) libc.SyncFile(call.memory, call.arguments[0]);
        if (written != formatted.size()) { libc.SetError(call.memory, errno); return ~std::uint64_t{0}; }
    } else if (output == FormatOutput::BoundedBuffer) {
        const auto size = call.arguments[1];
        if (size != 0) {
            const auto truncated = formatted.substr(0, size - 1);
            write(call.memory, call.arguments[0], std::as_bytes(std::span(truncated.c_str(), truncated.size() + 1)));
        }
    } else if (output == FormatOutput::AllocatedBuffer) {
        if (!call.memory.Contains(call.arguments[0], 8, Write)) throw std::runtime_error("Guest allocated format pointer is outside writable guest memory");
        const auto address = libc.Allocate(call.memory, formatted.size() + 1);
        write(call.memory, call.arguments[0], std::as_bytes(std::span(&address, 1)));
        if (address == 0) return ~std::uint64_t{0};
        write(call.memory, address, std::as_bytes(std::span(formatted.c_str(), formatted.size() + 1)));
    } else {
        write(call.memory, call.arguments[0], std::as_bytes(std::span(formatted.c_str(), formatted.size() + 1)));
    }
    return formatted.size();
}

}
