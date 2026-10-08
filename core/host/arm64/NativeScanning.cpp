#include "NativeScanning.hpp"
#include "NativePosix.hpp"
#include <array>
#include <cctype>
#include <climits>
#include <cstring>
#include <cwchar>
#include <limits>
#include <stdexcept>

namespace AnyPS5::Arm64 {

namespace {

template<class TValue>
TValue read(const GuestMemory& memory, std::uint64_t address) {
    TValue value{};
    if (!memory.TryRead(address, std::as_writable_bytes(std::span(&value, 1)))) throw std::runtime_error("Native scanf argument is outside readable guest memory");
    return value;
}

void write(GuestMemory& memory, std::uint64_t address, std::span<const std::byte> bytes) {
    if (!memory.TryWrite(address, bytes)) throw std::runtime_error("Native scanf destination is outside writable guest memory");
}

std::string string(const GuestMemory& memory, std::uint64_t address) {
    std::string value;
    for (;;) {
        const auto byte = read<char>(memory, address);
        if (byte == 0) return value;
        value.push_back(byte);
        if (++address == 0) throw std::runtime_error("Native scanf string address overflows");
    }
}

struct VaList { std::uint32_t general, vector; std::uint64_t stack, registers; };

class Arguments {
public:
    Arguments(const HostCall& call, std::size_t first, bool saved) : call(call), general(first), stack(call.stackArguments), saved(saved) {
        if (!saved) return;
        list = read<VaList>(call.memory, call.arguments[first]);
        if (list.general > 48 || list.general % 8 != 0 || list.vector < 48 || list.vector > 176 || (list.vector - 48) % 16 != 0) throw std::runtime_error("Native scanf x86-64 va_list offsets are invalid");
        stack = list.stack;
    }
    std::uint64_t Next() {
        if (saved && list.general < 48) {
            const auto address = add(list.registers, list.general);
            list.general += 8;
            return read<std::uint64_t>(call.memory, address);
        }
        if (!saved && general < 6) return call.arguments[general++];
        const auto result = read<std::uint64_t>(call.memory, stack);
        stack = add(stack, 8);
        return result;
    }
private:
    const HostCall& call;
    std::size_t general;
    std::uint64_t stack;
    bool saved;
    VaList list{};
    static std::uint64_t add(std::uint64_t address, std::uint64_t size) {
        if (address > UINT64_MAX - size) throw std::runtime_error("Native scanf argument address overflows");
        return address + size;
    }
};

class Input {
public:
    explicit Input(std::FILE* file) : file(file) {}
    int Peek() {
        const auto byte = std::fgetc(file);
        if (byte != EOF && std::ungetc(byte, file) == EOF) throw std::runtime_error("Native scanf cannot retain an input byte");
        return byte;
    }
    int Take() { const auto byte = std::fgetc(file); if (byte != EOF) ++consumed; return byte; }
    void SkipSpace() { while (Peek() != EOF && std::isspace(static_cast<unsigned char>(Peek()))) Take(); }
    template<class TValue>
    int Number(const std::string& format, TValue& value) {
        int count = -1;
        const auto result = std::fscanf(file, (format + "%n").c_str(), &value, &count);
        if (count >= 0) consumed += static_cast<std::uint64_t>(count);
        return result;
    }
    std::uint64_t Count() const { return consumed; }
private:
    std::FILE* file;
    std::uint64_t consumed = 0;
};

std::size_t integerSize(const std::string& length) {
    if (length == "hh") return 1;
    if (length == "h") return 2;
    if (length.empty()) return 4;
    if (length == "l" || length == "ll" || length == "j" || length == "z" || length == "t") return 8;
    throw std::runtime_error("Native scanf integer length is unsupported");
}

void writeInteger(GuestMemory& memory, std::uint64_t address, std::uint64_t value, std::size_t size) {
    write(memory, address, std::as_bytes(std::span(&value, 1)).first(size));
}

struct Conversion {
    char type;
    std::string length;
    std::size_t width = std::numeric_limits<std::size_t>::max();
    bool suppressed = false;
    std::array<bool, 256> set{};
};

Conversion conversion(const std::string& format, std::size_t& position) {
    Conversion result{};
    if (position < format.size() && format[position] == '*') { result.suppressed = true; ++position; }
    if (position < format.size() && format[position] >= '0' && format[position] <= '9') {
        result.width = 0;
        while (position < format.size() && format[position] >= '0' && format[position] <= '9') {
            const auto digit = static_cast<std::size_t>(format[position++] - '0');
            if (result.width > (INT_MAX - digit) / 10) throw std::runtime_error("Native scanf field width exceeds INT_MAX");
            result.width = result.width * 10 + digit;
        }
        if (result.width == 0) throw std::runtime_error("Native scanf field width is zero");
    }
    if (position < format.size() && std::string_view("hljztL").find(format[position]) != std::string_view::npos) {
        result.length.push_back(format[position++]);
        if (position < format.size() && (result.length == "h" || result.length == "l") && format[position] == result.length[0]) result.length.push_back(format[position++]);
    }
    if (position == format.size()) throw std::runtime_error("Native scanf conversion is truncated");
    result.type = format[position++];
    if (result.type != '[') return result;
    const bool negate = position < format.size() && format[position] == '^';
    if (negate) ++position;
    if (position < format.size() && format[position] == ']') { result.set[']'] = true; ++position; }
    while (position < format.size() && format[position] != ']') {
        const auto first = static_cast<unsigned char>(format[position++]);
        if (position + 1 < format.size() && format[position] == '-' && format[position + 1] != ']' && first <= static_cast<unsigned char>(format[position + 1])) {
            const auto last = static_cast<unsigned char>(format[position + 1]);
            position += 2;
            for (unsigned value = first; value <= last; ++value) result.set[value] = true;
        } else result.set[first] = true;
    }
    if (position == format.size()) throw std::runtime_error("Native scanf character set is truncated");
    ++position;
    if (negate) for (auto& accepted : result.set) accepted = !accepted;
    return result;
}

int scan(GuestMemory& memory, NativeLibc& libc, Input& input, const std::string& format, Arguments& arguments) {
    int assigned = 0;
    bool completed = false;
    for (std::size_t position = 0; position < format.size();) {
        const auto literal = static_cast<unsigned char>(format[position++]);
        if (std::isspace(literal)) { input.SkipSpace(); continue; }
        if (literal != '%') {
            const auto next = input.Peek();
            if (next != literal) return next == EOF && !completed ? EOF : assigned;
            input.Take();
            continue;
        }
        if (position < format.size() && format[position] == '%') {
            ++position;
            const auto next = input.Peek();
            if (next != '%') return next == EOF && !completed ? EOF : assigned;
            input.Take();
            continue;
        }
        const auto field = conversion(format, position);
        const auto destination = field.suppressed ? 0 : arguments.Next();
        if (field.type == 'n') {
            if (!field.suppressed) writeInteger(memory, destination, input.Count(), integerSize(field.length));
            completed = true;
            continue;
        }
        if (field.type == 's' || field.type == '[' || field.type == 'c') {
            if (field.type == 's') input.SkipSpace();
            if (!field.length.empty() && field.length != "l") throw std::runtime_error("Native scanf character length is unsupported");
            if (field.type == '[' && field.length == "l") throw std::runtime_error("Native scanf wide character sets are not implemented");
            const auto width = field.type == 'c' && field.width == std::numeric_limits<std::size_t>::max() ? 1 : field.width;
            std::vector<std::byte> bytes;
            std::mbstate_t state{};
            std::size_t count = 0;
            while (count < width) {
                const auto next = input.Peek();
                if (next == EOF || (field.type == 's' && std::isspace(static_cast<unsigned char>(next))) || (field.type == '[' && !field.set[static_cast<unsigned char>(next)])) break;
                if (field.length.empty()) {
                    const auto byte = static_cast<std::byte>(input.Take());
                    if (!field.suppressed) bytes.push_back(byte);
                } else {
                    wchar_t character = 0;
                    std::size_t converted = static_cast<std::size_t>(-2);
                    while (converted == static_cast<std::size_t>(-2)) {
                        const auto value = input.Take();
                        if (value == EOF) { libc.SetError(memory, 86); return !completed ? EOF : assigned; }
                        const auto byte = static_cast<char>(value);
                        converted = std::mbrtowc(&character, &byte, 1, &state);
                    }
                    if (converted == static_cast<std::size_t>(-1)) { libc.SetError(memory, 86); return !completed ? EOF : assigned; }
                    if (!field.suppressed) { const auto value = std::as_bytes(std::span(&character, 1)); bytes.insert(bytes.end(), value.begin(), value.end()); }
                }
                ++count;
            }
            if (count == 0) return input.Peek() == EOF && !completed ? EOF : assigned;
            if (!field.suppressed) {
                if (field.type != 'c') bytes.insert(bytes.end(), field.length.empty() ? 1 : 4, std::byte{});
                write(memory, destination, bytes);
                ++assigned;
            }
            completed = true;
            continue;
        }
        std::string token = "%";
        if (field.width != std::numeric_limits<std::size_t>::max()) token += std::to_string(field.width);
        int result;
        std::array<std::byte, 8> bytes{};
        std::size_t size;
        if (std::string_view("diouxX").find(field.type) != std::string_view::npos) {
            token += "ll";
            token += field.type;
            size = integerSize(field.length);
            if (field.type == 'd' || field.type == 'i') {
                long long value = 0;
                result = input.Number(token, value);
                std::memcpy(bytes.data(), &value, 8);
            } else {
                unsigned long long value = 0;
                result = input.Number(token, value);
                std::memcpy(bytes.data(), &value, 8);
            }
        } else if (std::string_view("aAeEfFgG").find(field.type) != std::string_view::npos) {
            if (field.length == "l") {
                token += 'l'; token += field.type;
                double value = 0;
                result = input.Number(token, value);
                size = 8;
                std::memcpy(bytes.data(), &value, 8);
            } else if (field.length.empty()) {
                token += field.type;
                float value = 0;
                result = input.Number(token, value);
                size = 4;
                std::memcpy(bytes.data(), &value, 4);
            } else throw std::runtime_error("Native scanf extended floating conversion is not implemented");
        } else if (field.type == 'p' && field.length.empty()) {
            token += 'p';
            void* value = nullptr;
            result = input.Number(token, value);
            size = 8;
            std::memcpy(bytes.data(), &value, 8);
        } else throw std::runtime_error("Native scanf conversion is unsupported");
        if (result != 1) return result == EOF && !completed ? EOF : assigned;
        if (!field.suppressed) { write(memory, destination, std::span(bytes).first(size)); ++assigned; }
        completed = true;
    }
    return assigned;
}

}

std::uint64_t scanInput(const HostCall& call, NativeLibc& libc, ScanInput source, bool vaList) {
    const auto formatIndex = source == ScanInput::Standard ? 0u : 1u;
    const auto format = string(call.memory, call.arguments[formatIndex]);
    Arguments arguments(call, formatIndex + 1, vaList);
    std::string text;
    std::unique_ptr<std::FILE, decltype(&std::fclose)> owned(nullptr, &std::fclose);
    std::FILE* file;
    const auto guestFile = source == ScanInput::File ? call.arguments[0] : source == ScanInput::Standard ? libc.StandardStream(0) : 0;
    if (source == ScanInput::String) {
        text = string(call.memory, call.arguments[0]);
        owned.reset(::fmemopen(text.data(), text.size(), "r"));
        if (!owned) { libc.SetError(call.memory, guestError(errno)); return UINT64_MAX; }
        file = owned.get();
    } else file = libc.File(guestFile);
    Input input(file);
    const auto result = scan(call.memory, libc, input, format, arguments);
    if (std::ferror(file)) libc.SetError(call.memory, guestError(errno));
    if (source != ScanInput::String) libc.SyncFile(call.memory, guestFile);
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(result));
}

}
