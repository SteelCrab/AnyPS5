#include "NativeLibc.hpp"
#include "NativeFormatting.hpp"
#include "NativeScanning.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <unistd.h>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <xlocale.h>

namespace AnyPS5::Arm64 {

struct NativeStreamDescriptor {
    int native = -1;
    int guest = -1;
    ~NativeStreamDescriptor() { if (native >= 0) ::close(native); }
};

namespace {

constexpr std::uint64_t functionBase = 0x700000000;
constexpr std::uint64_t globalBase = 0x710000000;

std::FILE* stream(const std::shared_ptr<NativeStreamDescriptor>& descriptor, bool readable, bool writable) {
    return ::funopen(descriptor.get(), readable ? +[](void* context, char* bytes, int count) -> int {
        return static_cast<int>(::read(static_cast<NativeStreamDescriptor*>(context)->native, bytes, static_cast<std::size_t>(count)));
    } : nullptr, writable ? +[](void* context, const char* bytes, int count) -> int {
        return static_cast<int>(::write(static_cast<NativeStreamDescriptor*>(context)->native, bytes, static_cast<std::size_t>(count)));
    } : nullptr, +[](void* context, fpos_t offset, int origin) -> fpos_t {
        return ::lseek(static_cast<NativeStreamDescriptor*>(context)->native, offset, origin);
    }, +[](void* context) -> int {
        auto& native = static_cast<NativeStreamDescriptor*>(context)->native;
        if (native < 0) { errno = EBADF; return EOF; }
        const auto result = ::close(native);
        native = -1;
        return result;
    });
}

class ThreadLocale {
public:
    explicit ThreadLocale(locale_t locale) : previous(locale ? ::uselocale(locale) : nullptr) {
        if (locale && !previous) throw std::runtime_error("Cannot activate guest libc locale");
    }
    ~ThreadLocale() { if (previous) ::uselocale(previous); }
private:
    locale_t previous;
};

void range(const GuestMemory& memory, std::uint64_t address, std::size_t size, std::uint32_t permissions) {
    if (!memory.Contains(address, size, permissions)) throw std::runtime_error("Native libc argument is outside permitted guest memory");
}

std::vector<std::byte> readBytes(const GuestMemory& memory, std::uint64_t address, std::size_t size) {
    range(memory, address, size, Read);
    std::vector<std::byte> bytes(size);
    if (!memory.TryRead(address, bytes)) throw std::runtime_error("Native libc guest read failed");
    return bytes;
}

void writeBytes(GuestMemory& memory, std::uint64_t address, std::span<const std::byte> bytes) {
    if (!memory.TryWrite(address, bytes)) throw std::runtime_error("Native libc argument is outside writable guest memory");
}

template<class TCharacter>
std::basic_string<TCharacter> readString(const GuestMemory& memory, std::uint64_t address, std::size_t maximum = std::numeric_limits<std::size_t>::max()) {
    std::basic_string<TCharacter> string;
    for (std::size_t i = 0; i < maximum; ++i) {
        TCharacter character{};
        if (!memory.TryRead(address, std::as_writable_bytes(std::span(&character, 1)))) throw std::runtime_error("Native libc string is outside readable guest memory");
        if (character == 0) break;
        string.push_back(character);
        if (address > std::numeric_limits<std::uint64_t>::max() - sizeof(character)) throw std::runtime_error("Native libc string address overflows");
        address += sizeof(character);
    }
    return string;
}

template<class TCharacter>
void writeString(GuestMemory& memory, std::uint64_t address, const std::basic_string<TCharacter>& string) {
    writeBytes(memory, address, std::as_bytes(std::span(string.c_str(), string.size() + 1)));
}

std::uint64_t integer(int value) { return static_cast<std::uint64_t>(static_cast<std::int64_t>(value)); }

std::uint64_t allocate(const HostCall& call, void* context) { return static_cast<NativeLibc*>(context)->Allocate(call.memory, call.arguments[0]); }
std::uint64_t alignedAllocate(const HostCall& call, void* context) {
    auto& libc = *static_cast<NativeLibc*>(context);
    const auto alignment = call.arguments[0];
    if (alignment == 0 || (alignment & (alignment - 1)) != 0 || call.arguments[1] % alignment != 0) { libc.SetError(call.memory, 22); return 0; }
    return libc.AllocateAligned(call.memory, call.arguments[1], alignment);
}
std::uint64_t release(const HostCall& call, void* context) {
    static_cast<NativeLibc*>(context)->Free(call.memory, call.arguments[0]);
    return 0;
}
std::uint64_t resize(const HostCall& call, void* context) { return static_cast<NativeLibc*>(context)->Resize(call.memory, call.arguments[0], call.arguments[1]); }
std::uint64_t allocateZero(const HostCall& call, void* context) {
    if (call.arguments[0] != 0 && call.arguments[1] > std::numeric_limits<std::size_t>::max() / call.arguments[0]) {
        static_cast<NativeLibc*>(context)->SetError(call.memory, 12);
        return 0;
    }
    return static_cast<NativeLibc*>(context)->Allocate(call.memory, call.arguments[0] * call.arguments[1]);
}
std::uint64_t copy(const HostCall& call, void*) {
    range(call.memory, call.arguments[0], call.arguments[2], Write);
    const auto bytes = readBytes(call.memory, call.arguments[1], call.arguments[2]);
    writeBytes(call.memory, call.arguments[0], bytes);
    return call.arguments[0];
}
std::uint64_t fill(const HostCall& call, void*) {
    range(call.memory, call.arguments[0], call.arguments[2], Write);
    const std::vector<std::byte> bytes(call.arguments[2], static_cast<std::byte>(call.arguments[1]));
    writeBytes(call.memory, call.arguments[0], bytes);
    return call.arguments[0];
}
std::uint64_t compareBytes(const HostCall& call, void*) {
    if (call.arguments[2] == 0) return 0;
    const auto left = readBytes(call.memory, call.arguments[0], call.arguments[2]);
    const auto right = readBytes(call.memory, call.arguments[1], call.arguments[2]);
    return integer(std::memcmp(left.data(), right.data(), left.size()));
}
std::uint64_t findByte(const HostCall& call, void*) {
    const auto bytes = readBytes(call.memory, call.arguments[0], call.arguments[2]);
    const auto found = std::find(bytes.begin(), bytes.end(), static_cast<std::byte>(call.arguments[1]));
    return found == bytes.end() ? 0 : call.arguments[0] + static_cast<std::uint64_t>(found - bytes.begin());
}

template<class TCharacter>
std::uint64_t length(const HostCall& call, void*) { return readString<TCharacter>(call.memory, call.arguments[0]).size(); }
std::uint64_t boundedLength(const HostCall& call, void*) { return readString<char>(call.memory, call.arguments[0], call.arguments[1]).size(); }

template<class TCharacter, bool Bounded, bool IgnoreCase>
std::uint64_t compareStrings(const HostCall& call, void*) {
    const auto maximum = Bounded ? call.arguments[2] : std::numeric_limits<std::size_t>::max();
    const auto left = readString<TCharacter>(call.memory, call.arguments[0], maximum);
    const auto right = readString<TCharacter>(call.memory, call.arguments[1], maximum);
    for (std::size_t i = 0; i < std::max(left.size(), right.size()); ++i) {
        std::uint32_t a = i < left.size() ? static_cast<std::make_unsigned_t<TCharacter>>(left[i]) : 0;
        std::uint32_t b = i < right.size() ? static_cast<std::make_unsigned_t<TCharacter>>(right[i]) : 0;
        if constexpr (IgnoreCase) {
            a = a >= 'A' && a <= 'Z' ? a + ('a' - 'A') : a;
            b = b >= 'A' && b <= 'Z' ? b + ('a' - 'A') : b;
        }
        if (a != b) return integer(a < b ? -1 : 1);
    }
    return 0;
}

template<class TCharacter>
std::uint64_t substring(const HostCall& call, void*) {
    const auto string = readString<TCharacter>(call.memory, call.arguments[0]);
    const auto pattern = readString<TCharacter>(call.memory, call.arguments[1]);
    const auto position = string.find(pattern);
    return position == std::basic_string<TCharacter>::npos ? 0 : call.arguments[0] + position * sizeof(TCharacter);
}

template<bool Last>
std::uint64_t findCharacter(const HostCall& call, void*) {
    const auto string = readString<char>(call.memory, call.arguments[0]);
    const auto character = static_cast<char>(call.arguments[1]);
    if (character == 0) return call.arguments[0] + string.size();
    const auto position = Last ? string.rfind(character) : string.find(character);
    return position == std::string::npos ? 0 : call.arguments[0] + position;
}
std::uint64_t copyString(const HostCall& call, void*) {
    const auto string = readString<char>(call.memory, call.arguments[1]);
    writeString(call.memory, call.arguments[0], string);
    return call.arguments[0];
}
std::uint64_t boundedCopy(const HostCall& call, void*) {
    range(call.memory, call.arguments[0], call.arguments[2], Write);
    const auto string = readString<char>(call.memory, call.arguments[1], call.arguments[2]);
    std::vector<std::byte> bytes(call.arguments[2]);
    if (!string.empty()) std::memcpy(bytes.data(), string.data(), string.size());
    writeBytes(call.memory, call.arguments[0], bytes);
    return call.arguments[0];
}
std::uint64_t appendString(const HostCall& call, void*) {
    const auto left = readString<char>(call.memory, call.arguments[0]);
    const auto right = readString<char>(call.memory, call.arguments[1]);
    writeString(call.memory, call.arguments[0], left + right);
    return call.arguments[0];
}
std::uint64_t safeCopy(const HostCall& call, void*) {
    const auto string = readString<char>(call.memory, call.arguments[1]);
    if (call.arguments[2] != 0) writeString(call.memory, call.arguments[0], string.substr(0, call.arguments[2] - 1));
    return string.size();
}
std::uint64_t safeAppend(const HostCall& call, void*) {
    const auto maximum = call.arguments[2];
    const auto left = readString<char>(call.memory, call.arguments[0], maximum);
    const auto right = readString<char>(call.memory, call.arguments[1]);
    if (left.size() < maximum) writeString(call.memory, call.arguments[0], left + right.substr(0, maximum - left.size() - 1));
    return left.size() + right.size();
}
std::uint64_t duplicate(const HostCall& call, void* context) {
    const auto string = readString<char>(call.memory, call.arguments[0]);
    const auto address = static_cast<NativeLibc*>(context)->Allocate(call.memory, string.size() + 1);
    if (address != 0) writeString(call.memory, address, string);
    return address;
}

template<int (*TFunction)(int)>
std::uint64_t character(const HostCall& call, void*) {
    const auto value = static_cast<std::int32_t>(call.arguments[0]);
    if (value != EOF && (value < 0 || value > UCHAR_MAX)) throw std::runtime_error("Native libc character argument is outside unsigned char range");
    return integer(TFunction(value));
}

std::uint64_t parseInteger(const HostCall& call, void* context, bool signedValue, std::size_t width, bool withEnd) {
    const auto string = readString<char>(call.memory, call.arguments[0]);
    const auto base = withEnd ? static_cast<int>(call.arguments[2]) : 10;
    if (withEnd && call.arguments[1] != 0) range(call.memory, call.arguments[1], 8, Write);
    if (base != 0 && (base < 2 || base > 36)) {
        static_cast<NativeLibc*>(context)->SetError(call.memory, 22);
        if (withEnd && call.arguments[1] != 0) writeBytes(call.memory, call.arguments[1], std::as_bytes(std::span(&call.arguments[0], 1)));
        return 0;
    }
    char* end = nullptr;
    errno = 0;
    auto value = signedValue ? static_cast<std::uint64_t>(std::strtoll(string.c_str(), &end, base)) : std::strtoull(string.c_str(), &end, base);
    const int error = errno;
    if (signedValue && width == 32) value = integer(static_cast<std::int32_t>(value));
    if (withEnd && call.arguments[1] != 0) {
        const auto endAddress = call.arguments[0] + static_cast<std::uint64_t>(end - string.c_str());
        writeBytes(call.memory, call.arguments[1], std::as_bytes(std::span(&endAddress, 1)));
    }
    if (error != 0) static_cast<NativeLibc*>(context)->SetError(call.memory, error);
    return value;
}

template<class TFloat>
std::uint64_t parseFloat(const HostCall& call, void* context, bool withEnd) {
    const auto string = readString<char>(call.memory, call.arguments[0]);
    if (withEnd && call.arguments[1] != 0) range(call.memory, call.arguments[1], 8, Write);
    char* end = nullptr;
    errno = 0;
    const auto value = [&] {
        if constexpr (std::is_same_v<TFloat, float>) return std::strtof(string.c_str(), &end);
        else return std::strtod(string.c_str(), &end);
    }();
    const auto error = errno;
    if (withEnd && call.arguments[1] != 0) {
        const auto endAddress = call.arguments[0] + static_cast<std::uint64_t>(end - string.c_str());
        writeBytes(call.memory, call.arguments[1], std::as_bytes(std::span(&endAddress, 1)));
    }
    if (error != 0) static_cast<NativeLibc*>(context)->SetError(call.memory, error);
    std::memcpy(call.state.xmm[0].data(), &value, sizeof(value));
    return 0;
}

std::uint64_t openFile(const HostCall& call, void* context) {
    const auto path = readString<char>(call.memory, call.arguments[0]);
    const auto mode = readString<char>(call.memory, call.arguments[1]);
    return static_cast<NativeLibc*>(context)->Open(call.memory, path.c_str(), mode.c_str());
}
std::uint64_t closeFile(const HostCall& call, void* context) {
    return integer(static_cast<NativeLibc*>(context)->Close(call.memory, call.arguments[0]));
}
std::uint64_t flushFile(const HostCall& call, void* context) {
    auto& libc = *static_cast<NativeLibc*>(context);
    const auto result = call.arguments[0] == 0 ? libc.FlushAll(call.memory) : std::fflush(libc.File(call.arguments[0]));
    if (result == EOF) libc.SetError(call.memory, errno);
    if (call.arguments[0] != 0) libc.SyncFile(call.memory, call.arguments[0]);
    return integer(result);
}
std::uint64_t readFile(const HostCall& call, void* context) {
    auto& libc = *static_cast<NativeLibc*>(context);
    auto* file = libc.File(call.arguments[3]);
    const auto size = call.arguments[1], count = call.arguments[2];
    if (size == 0 || count == 0) return 0;
    if (size > std::numeric_limits<std::size_t>::max() / count) { libc.SetError(call.memory, 22); return 0; }
    range(call.memory, call.arguments[0], size * count, Write);
    std::vector<std::byte> bytes(size * count);
    const auto read = std::fread(bytes.data(), 1, bytes.size(), file);
    const auto error = errno;
    writeBytes(call.memory, call.arguments[0], std::span(bytes).first(read));
    if (std::ferror(file)) libc.SetError(call.memory, error);
    libc.SyncFile(call.memory, call.arguments[3]);
    return read / size;
}
std::uint64_t writeFile(const HostCall& call, void* context) {
    auto& libc = *static_cast<NativeLibc*>(context);
    auto* file = libc.File(call.arguments[3]);
    const auto size = call.arguments[1], count = call.arguments[2];
    if (size == 0 || count == 0) return 0;
    if (size > std::numeric_limits<std::size_t>::max() / count) { libc.SetError(call.memory, 22); return 0; }
    const auto bytes = readBytes(call.memory, call.arguments[0], size * count);
    const auto written = std::fwrite(bytes.data(), 1, bytes.size(), file);
    if (std::ferror(file)) libc.SetError(call.memory, errno);
    libc.SyncFile(call.memory, call.arguments[3]);
    return written / size;
}
std::uint64_t seekFile(const HostCall& call, void* context) {
    auto& libc = *static_cast<NativeLibc*>(context);
    const auto result = ::fseeko(libc.File(call.arguments[0]), std::bit_cast<std::int64_t>(call.arguments[1]), static_cast<int>(call.arguments[2]));
    if (result != 0) libc.SetError(call.memory, errno);
    libc.SyncFile(call.memory, call.arguments[0]);
    return integer(result);
}
std::uint64_t tellFile(const HostCall& call, void* context) {
    auto& libc = *static_cast<NativeLibc*>(context);
    const auto result = ::ftello(libc.File(call.arguments[0]));
    if (result == -1) libc.SetError(call.memory, errno);
    return static_cast<std::uint64_t>(result);
}
std::uint64_t putCharacter(const HostCall& call, void* context) {
    auto& libc = *static_cast<NativeLibc*>(context);
    const auto result = std::fputc(static_cast<int>(call.arguments[0]), libc.File(call.arguments[1]));
    if (result == EOF) libc.SetError(call.memory, errno);
    libc.SyncFile(call.memory, call.arguments[1]);
    return integer(result);
}
std::uint64_t getCharacter(const HostCall& call, void* context) {
    auto& libc = *static_cast<NativeLibc*>(context);
    auto* file = libc.File(call.arguments[0]);
    const auto result = std::fgetc(file);
    if (std::ferror(file)) libc.SetError(call.memory, errno);
    libc.SyncFile(call.memory, call.arguments[0]);
    return integer(result);
}
std::uint64_t putString(const HostCall& call, void* context) {
    auto& libc = *static_cast<NativeLibc*>(context);
    const auto string = readString<char>(call.memory, call.arguments[0]);
    const auto result = std::fputs(string.c_str(), libc.File(call.arguments[1]));
    if (result == EOF) libc.SetError(call.memory, errno);
    libc.SyncFile(call.memory, call.arguments[1]);
    return integer(result);
}
std::uint64_t getString(const HostCall& call, void* context) {
    auto& libc = *static_cast<NativeLibc*>(context);
    const auto count = static_cast<std::int32_t>(call.arguments[1]);
    if (count <= 0) return 0;
    range(call.memory, call.arguments[0], count, Write);
    auto* file = libc.File(call.arguments[2]);
    std::vector<char> bytes;
    while (bytes.size() < static_cast<std::size_t>(count - 1)) {
        const auto character = std::fgetc(file);
        if (character == EOF) break;
        bytes.push_back(static_cast<char>(character));
        if (character == '\n') break;
    }
    if (std::ferror(file)) libc.SetError(call.memory, errno);
    libc.SyncFile(call.memory, call.arguments[2]);
    if (bytes.empty() && count != 1) return 0;
    bytes.push_back(0);
    writeBytes(call.memory, call.arguments[0], std::as_bytes(std::span(bytes)));
    return call.arguments[0];
}

struct Function {
    std::string_view name;
    HostFunction function;
};

template<FormatOutput TOutput, bool TVaList>
std::uint64_t print(const HostCall& call, void* context) { return formatOutput(call, *static_cast<NativeLibc*>(context), TOutput, TVaList); }

template<ScanInput TInput, bool TVaList>
std::uint64_t scan(const HostCall& call, void* context) { return scanInput(call, *static_cast<NativeLibc*>(context), TInput, TVaList); }

const auto functions = std::to_array<Function>({
    {"sscanf", scan<ScanInput::String, false>}, {"vsscanf", scan<ScanInput::String, true>},
    {"fscanf", scan<ScanInput::File, false>}, {"vfscanf", scan<ScanInput::File, true>},
    {"scanf", scan<ScanInput::Standard, false>}, {"vscanf", scan<ScanInput::Standard, true>},
    {"printf", print<FormatOutput::Standard, false>}, {"vprintf", print<FormatOutput::Standard, true>},
    {"fprintf", print<FormatOutput::File, false>}, {"vfprintf", print<FormatOutput::File, true>},
    {"sprintf", print<FormatOutput::Buffer, false>}, {"vsprintf", print<FormatOutput::Buffer, true>},
    {"snprintf", print<FormatOutput::BoundedBuffer, false>}, {"vsnprintf", print<FormatOutput::BoundedBuffer, true>},
    {"asprintf", print<FormatOutput::AllocatedBuffer, false>}, {"vasprintf", print<FormatOutput::AllocatedBuffer, true>},
    {"malloc", allocate}, {"calloc", allocateZero}, {"realloc", resize}, {"free", release}, {"aligned_alloc", alignedAllocate},
    {"memcpy", copy}, {"memmove", copy}, {"memset", fill}, {"memcmp", compareBytes}, {"memchr", findByte},
    {"strlen", length<char>}, {"strnlen", boundedLength}, {"wcslen", length<wchar_t>},
    {"strcmp", compareStrings<char, false, false>}, {"strncmp", compareStrings<char, true, false>},
    {"strcasecmp", compareStrings<char, false, true>}, {"strncasecmp", compareStrings<char, true, true>},
    {"wcscmp", compareStrings<wchar_t, false, false>}, {"wcsncmp", compareStrings<wchar_t, true, false>},
    {"strstr", substring<char>}, {"wcsstr", substring<wchar_t>}, {"strchr", findCharacter<false>}, {"strrchr", findCharacter<true>},
    {"strcpy", copyString}, {"strncpy", boundedCopy}, {"strcat", appendString}, {"strlcpy", safeCopy}, {"strlcat", safeAppend}, {"strdup", duplicate},
    {"isalpha", character<std::isalpha>}, {"isalnum", character<std::isalnum>}, {"isxdigit", character<std::isxdigit>},
    {"ispunct", character<std::ispunct>}, {"isspace", character<std::isspace>}, {"isupper", character<std::isupper>},
    {"islower", character<std::islower>}, {"isprint", character<std::isprint>}, {"isgraph", character<std::isgraph>},
    {"iscntrl", character<std::iscntrl>}, {"isblank", character<std::isblank>}, {"toupper", character<std::toupper>}, {"tolower", character<std::tolower>},
    {"atoi", +[](const HostCall& call, void* context) { return parseInteger(call, context, true, 32, false); }},
    {"atol", +[](const HostCall& call, void* context) { return parseInteger(call, context, true, 64, false); }},
    {"atoll", +[](const HostCall& call, void* context) { return parseInteger(call, context, true, 64, false); }},
    {"strtol", +[](const HostCall& call, void* context) { return parseInteger(call, context, true, 64, true); }},
    {"strtoll", +[](const HostCall& call, void* context) { return parseInteger(call, context, true, 64, true); }},
    {"strtoul", +[](const HostCall& call, void* context) { return parseInteger(call, context, false, 64, true); }},
    {"strtoull", +[](const HostCall& call, void* context) { return parseInteger(call, context, false, 64, true); }},
    {"strtod", +[](const HostCall& call, void* context) { return parseFloat<double>(call, context, true); }},
    {"strtof", +[](const HostCall& call, void* context) { return parseFloat<float>(call, context, true); }},
    {"atof", +[](const HostCall& call, void* context) { return parseFloat<double>(call, context, false); }},
    {"fopen", openFile}, {"fclose", closeFile}, {"fflush", flushFile}, {"fread", readFile}, {"fwrite", writeFile},
    {"fseek", seekFile}, {"fseeko", seekFile}, {"ftell", tellFile}, {"ftello", tellFile},
    {"putc", putCharacter}, {"fputc", putCharacter}, {"__swbuf", putCharacter}, {"getc", getCharacter}, {"fgetc", getCharacter},
    {"fputs", putString}, {"fgets", getString},
    {"feof", +[](const HostCall& call, void* context) { return integer(std::feof(static_cast<NativeLibc*>(context)->File(call.arguments[0]))); }},
    {"ferror", +[](const HostCall& call, void* context) { return integer(std::ferror(static_cast<NativeLibc*>(context)->File(call.arguments[0]))); }},
    {"fileno", +[](const HostCall& call, void* context) {
        auto& libc = *static_cast<NativeLibc*>(context);
        const auto result = libc.FileDescriptor(call.arguments[0]);
        if (result < 0) libc.SetError(call.memory, 9);
        return integer(result);
    }},
    {"ungetc", +[](const HostCall& call, void* context) -> std::uint64_t {
        auto& libc = *static_cast<NativeLibc*>(context);
        const auto result = std::ungetc(static_cast<int>(call.arguments[0]), libc.File(call.arguments[1]));
        if (result == EOF) libc.SetError(call.memory, errno);
        libc.SyncFile(call.memory, call.arguments[1]);
        return integer(result);
    }},
    {"puts", +[](const HostCall& call, void* context) {
        auto& libc = *static_cast<NativeLibc*>(context);
        const auto string = readString<char>(call.memory, call.arguments[0]);
        auto* file = libc.File(libc.StandardStream(1));
        const auto result = std::fputs(string.c_str(), file) == EOF || std::fputc('\n', file) == EOF ? EOF : 0;
        if (result == EOF) libc.SetError(call.memory, errno);
        libc.SyncFile(call.memory, libc.StandardStream(1));
        return integer(result);
    }},
    {"putchar", +[](const HostCall& call, void* context) {
        auto& libc = *static_cast<NativeLibc*>(context);
        const auto result = std::fputc(static_cast<int>(call.arguments[0]), libc.File(libc.StandardStream(1)));
        if (result == EOF) libc.SetError(call.memory, errno);
        libc.SyncFile(call.memory, libc.StandardStream(1));
        return integer(result);
    }},
    {"__error", +[](const HostCall& call, void* context) { return static_cast<NativeLibc*>(context)->ErrorAddress(call.memory); }}
});

}

std::uint64_t NativeLibc::Resolve(const ElfImport& symbol) const {
    if (symbol.name == "__stdinp") return symbol.type <= 1 ? globalBase + 16 : 0;
    if (symbol.name == "__stdoutp") return symbol.type <= 1 ? globalBase + 24 : 0;
    if (symbol.name == "__stderrp") return symbol.type <= 1 ? globalBase + 32 : 0;
    if (symbol.name == "__isthreaded") return symbol.type <= 1 ? globalBase + 8 : 0;
    if (symbol.type != 0 && symbol.type != 2) return 0;
    for (std::size_t i = 0; i < functions.size(); ++i) {
        if (functions[i].name == symbol.name) return functionBase + i * 16;
    }
    return 0;
}

void NativeLibc::Bind(Cpu& cpu, GuestMemory& memory) {
    std::lock_guard lock(mutex);
    if (bound) throw std::runtime_error("Native libc was already bound");
    memory.Map(globalBase, 0x1000, Read | Write);
    const std::array<std::FILE*, 3> standard{stdin, stdout, stderr};
    for (std::size_t i = 0; i < standard.size(); ++i) {
        const auto address = globalBase + 0x100 + i * 0x200;
        auto descriptor = std::make_shared<NativeStreamDescriptor>();
        const auto native = ::dup(::fileno(standard[i]));
        if (native < 0) throw std::runtime_error("Cannot duplicate a guest standard file descriptor");
        descriptor->native = native;
        descriptor->guest = static_cast<int>(i);
        auto* file = stream(descriptor, i == 0, i != 0);
        if (!file) throw std::runtime_error("Cannot initialize a guest standard stream");
        try { streams.emplace(address, Stream{file, false, i != 0, descriptor}); }
        catch (...) { std::fclose(file); throw; }
        std::setvbuf(file, nullptr, i == 2 ? _IONBF : _IOLBF, 0);
        writeBytes(memory, globalBase + 16 + i * 8, std::as_bytes(std::span(&address, 1)));
        SyncFile(memory, address);
    }
    guestMemory = &memory;
    errorCells.emplace(std::this_thread::get_id(), globalBase);
    bound = true;
    Attach(cpu, memory);
}

void NativeLibc::Attach(Cpu& cpu, GuestMemory& memory) {
    std::lock_guard lock(mutex);
    if (!bound || guestMemory != &memory) throw std::runtime_error("Native libc attachment requires its initialized guest memory");
    const auto address = ErrorAddress(memory);
    const int zero = 0;
    writeBytes(memory, address, std::as_bytes(std::span(&zero, 1)));
    const int threaded = errorCells.size() > 1;
    writeBytes(memory, globalBase + 8, std::as_bytes(std::span(&threaded, 1)));
    [&]<std::size_t... TIndex>(std::index_sequence<TIndex...>) {
        (cpu.Bind(functionBase + TIndex * 16, +[](const HostCall& call, void* context) {
            return static_cast<NativeLibc*>(context)->Invoke(call, functions[TIndex].function);
        }, this), ...);
    }(std::make_index_sequence<functions.size()>{});
}

std::uint64_t NativeLibc::Invoke(const HostCall& call, HostFunction function) {
    std::lock_guard lock(mutex);
    const ThreadLocale scope(activeLocale);
    return function(call, this);
}

std::uint64_t NativeLibc::ErrorAddress(GuestMemory& memory) const {
    std::lock_guard lock(mutex);
    if (guestMemory != &memory) throw std::runtime_error("Native libc errno requires its initialized guest memory");
    const auto id = std::this_thread::get_id();
    const auto found = errorCells.find(id);
    if (found != errorCells.end()) return found->second;
    const auto address = nextError;
    memory.Map(address, 0x1000, Read | Write);
    try { errorCells.emplace(id, address); }
    catch (...) { memory.Unmap(address); throw; }
    nextError += 0x1000;
    return address;
}

std::uint64_t NativeLibc::Allocate(GuestMemory& memory, std::size_t size) {
    return AllocateAligned(memory, size, 16);
}

std::uint64_t NativeLibc::AllocateAligned(GuestMemory& memory, std::size_t size, std::size_t alignment) {
    std::lock_guard lock(mutex);
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) { SetError(memory, 22); return 0; }
    const auto mapped = std::max<std::size_t>(size, 1);
    constexpr std::uint64_t heapEnd = 0x700000000000;
    if (alignment - 1 > heapEnd - nextAllocation) { SetError(memory, 12); return 0; }
    const auto address = (nextAllocation + alignment - 1) & ~static_cast<std::uint64_t>(alignment - 1);
    if (address > heapEnd - 16 || mapped > heapEnd - address - 16) { SetError(memory, 12); return 0; }
    try { allocations.emplace(address, mapped); memory.Map(address, mapped, Read | Write); }
    catch (const std::bad_alloc&) { allocations.erase(address); SetError(memory, 12); return 0; }
    catch (const std::system_error&) { allocations.erase(address); SetError(memory, 12); return 0; }
    catch (...) { allocations.erase(address); throw; }
    nextAllocation = address + ((mapped + 15) & ~std::uint64_t{15});
    return address;
}

void NativeLibc::Free(GuestMemory& memory, std::uint64_t address) {
    std::lock_guard lock(mutex);
    if (address == 0) return;
    const auto found = allocations.find(address);
    if (found == allocations.end()) throw std::runtime_error("Native libc free requires a live guest allocation");
    memory.Unmap(address);
    allocations.erase(found);
}

std::uint64_t NativeLibc::Resize(GuestMemory& memory, std::uint64_t address, std::size_t size) {
    std::lock_guard lock(mutex);
    if (address == 0) return Allocate(memory, size);
    const auto found = allocations.find(address);
    if (found == allocations.end()) throw std::runtime_error("Native libc realloc requires a live guest allocation");
    if (size == 0) { Free(memory, address); return 0; }
    const auto count = std::min(size, found->second);
    const auto result = Allocate(memory, size);
    if (result == 0) return 0;
    const auto bytes = readBytes(memory, address, count);
    writeBytes(memory, result, bytes);
    Free(memory, address);
    return result;
}

void NativeLibc::SetError(GuestMemory& memory, int error) const {
    std::lock_guard lock(mutex);
    const auto address = ErrorAddress(memory);
    writeBytes(memory, address, std::as_bytes(std::span(&error, 1)));
}

NativeLibc::~NativeLibc() {
    for (const auto& [address, stream] : streams) {
        if (discardOnClose) ::fpurge(stream.file);
        std::fclose(stream.file);
    }
}

int NativeLibc::FlushAll(GuestMemory& memory) {
    std::lock_guard lock(mutex);
    int error = 0;
    for (const auto& [address, stream] : streams) {
        if (!stream.writable) continue;
        if (std::fflush(stream.file) == EOF && error == 0) error = errno;
        SyncFile(memory, address);
    }
    if (error != 0) { SetError(memory, error); errno = error; return EOF; }
    return 0;
}

void NativeLibc::DiscardBufferedOutput() {
    std::lock_guard lock(mutex);
    discardOnClose = true;
    for (const auto& [address, stream] : streams) if (stream.writable) ::fpurge(stream.file);
}

std::uint64_t NativeLibc::Open(GuestMemory& memory, const char* path, const char* mode) {
    std::lock_guard lock(mutex);
    auto descriptor = std::make_shared<NativeStreamDescriptor>();
    auto* file = std::fopen(path, mode);
    if (!file) { SetError(memory, errno); return 0; }
    const auto native = ::dup(::fileno(file));
    const auto error = errno;
    std::fclose(file);
    if (native < 0) { SetError(memory, error); return 0; }
    descriptor->native = native;
    descriptor->guest = native;
    const bool both = std::strchr(mode, '+') != nullptr;
    file = stream(descriptor, mode[0] == 'r' || both, mode[0] != 'r' || both);
    if (!file) { SetError(memory, errno); return 0; }
    const auto address = nextStream;
    bool mapped = false;
    try {
        memory.Map(address, 0x200, Read | Write);
        mapped = true;
        streams.emplace(address, Stream{file, true, mode[0] != 'r' || both, descriptor});
    } catch (...) {
        std::fclose(file);
        if (mapped) memory.Unmap(address);
        throw;
    }
    nextStream += 0x200;
    SyncFile(memory, address);
    return address;
}

int NativeLibc::Close(GuestMemory& memory, std::uint64_t address) {
    std::lock_guard lock(mutex);
    const auto found = streams.find(address);
    if (found == streams.end()) throw std::runtime_error("Native libc FILE handle is invalid");
    const auto result = std::fclose(found->second.file);
    if (result != 0) SetError(memory, errno);
    if (found->second.allocated) memory.Unmap(address);
    streams.erase(found);
    return result;
}

std::FILE* NativeLibc::File(std::uint64_t address) const {
    std::lock_guard lock(mutex);
    const auto found = streams.find(address);
    if (found == streams.end()) throw std::runtime_error("Native libc FILE handle is invalid");
    return found->second.file;
}

void NativeLibc::SyncFile(GuestMemory& memory, std::uint64_t address) const {
    std::lock_guard lock(mutex);
    auto* file = File(address);
    const std::int32_t count = 0;
    const auto flags = static_cast<std::uint16_t>(0x12 | (std::feof(file) ? 0x20 : 0) | (std::ferror(file) ? 0x40 : 0));
    const auto descriptor = static_cast<std::int16_t>(FileDescriptor(address));
    writeBytes(memory, address + 8, std::as_bytes(std::span(&count, 1)));
    writeBytes(memory, address + 12, std::as_bytes(std::span(&count, 1)));
    writeBytes(memory, address + 16, std::as_bytes(std::span(&flags, 1)));
    writeBytes(memory, address + 18, std::as_bytes(std::span(&descriptor, 1)));
}

bool NativeLibc::HasDescriptor(int descriptor) const {
    std::lock_guard lock(mutex);
    return NativeDescriptor(descriptor) >= 0;
}

int NativeLibc::NativeDescriptor(int descriptor) const {
    std::lock_guard lock(mutex);
    for (const auto& [address, stream] : streams) if (stream.descriptor->guest == descriptor && stream.descriptor->native >= 0) return stream.descriptor->native;
    return -1;
}

int NativeLibc::CloseDescriptor(int descriptor) {
    std::lock_guard lock(mutex);
    for (auto& [address, stream] : streams) {
        if (stream.descriptor->guest != descriptor || stream.descriptor->native < 0) continue;
        const auto result = ::close(stream.descriptor->native);
        stream.descriptor->native = -1;
        return result;
    }
    errno = EBADF;
    return -1;
}

int NativeLibc::FileDescriptor(std::uint64_t address) const {
    std::lock_guard lock(mutex);
    const auto found = streams.find(address);
    if (found == streams.end()) throw std::runtime_error("Native libc FILE handle is invalid");
    return found->second.descriptor->native >= 0 ? found->second.descriptor->guest : -1;
}

std::uint64_t NativeLibc::StandardStream(std::size_t index) const {
    if (index > 2) throw std::runtime_error("Native libc standard stream index is invalid");
    return globalBase + 0x100 + index * 0x200;
}

std::unique_lock<std::recursive_mutex> NativeLibc::LockStreams() const { return std::unique_lock(mutex); }

void NativeLibc::SetLocale(locale_t locale) {
    std::lock_guard lock(mutex);
    activeLocale = locale;
}

}
