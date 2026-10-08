#include "NativeLibc.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <clocale>
#include <unistd.h>
#include <stdexcept>
#include <string>

using namespace AnyPS5::Arm64;

namespace {

void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }

template<class TFunction>
void rejects(TFunction function, const char* expected) {
    try { function(); } catch (const std::runtime_error& error) {
        require(std::string(error.what()).find(expected) != std::string::npos, error.what());
        return;
    }
    throw std::runtime_error("Invalid native libc operation succeeded");
}

struct Fixture {
    GuestMemory memory;
    NativeLibc libc;
    Cpu cpu{memory};
    std::array<std::byte, 16> returnedVector{};
    Fixture() {
        memory.Map(0x1000, 0x1000, Read | Write);
        memory.Map(0x8000, 0x1000, Read | Write);
        libc.Bind(cpu, memory);
    }
    std::uint64_t Call(std::string_view name, std::array<std::uint64_t, 6> arguments = {}) {
        CpuState state;
        state.rip = libc.Resolve({name, 2, 1, 0});
        require(state.rip != 0, "Requested libc function was not resolved");
        state.registers[4] = 0x8ff8;
        const std::array<std::uint8_t, 6> registers{7, 6, 2, 1, 8, 9};
        for (std::size_t i = 0; i < arguments.size(); ++i) state.registers[registers[i]] = arguments[i];
        cpu.Run(state);
        returnedVector = state.xmm[0];
        return state.registers[0];
    }
    void String(std::uint64_t address, const std::string& string) {
        require(memory.TryWrite(address, std::as_bytes(std::span(string.c_str(), string.size() + 1))), "Could not initialize a guest string");
    }
    std::vector<std::byte> Bytes(std::uint64_t address, std::size_t size) {
        std::vector<std::byte> bytes(size);
        require(memory.TryRead(address, bytes), "Could not read guest bytes");
        return bytes;
    }
};

void testHeap() {
    Fixture fixture;
    auto address = fixture.Call("calloc", {4, 8});
    require(address != 0 && fixture.Bytes(address, 32) == std::vector<std::byte>(32), "Guest calloc did not allocate zeroed bytes");
    fixture.Call("memset", {address, 0xa5, 32});
    const auto resized = fixture.Call("realloc", {address, 64});
    require(resized != 0 && !fixture.memory.Contains(address, 1, Read), "Guest realloc retained the old allocation");
    require(fixture.Bytes(resized, 32) == std::vector<std::byte>(32, std::byte{0xa5}), "Guest realloc did not preserve the previous contents");
    fixture.Call("free", {resized});
    require(!fixture.memory.Contains(resized, 1, Read), "Guest free retained readable memory");
    rejects([&] { fixture.Call("free", {resized}); }, "live guest allocation");
    fixture.Call("free", {0});
    const auto zero = fixture.Call("malloc", {0});
    require(zero != 0 && fixture.memory.Contains(zero, 1, Write), "Zero-size guest malloc did not return a freeable allocation");
    require(fixture.Call("realloc", {zero, 0}) == 0, "Zero-size guest realloc did not free its input");
    require(fixture.Call("calloc", {std::numeric_limits<std::uint64_t>::max(), 2}) == 0, "Overflowing calloc succeeded");
    const auto error = fixture.Call("__error");
    int value = 0;
    require(fixture.memory.TryRead(error, std::as_writable_bytes(std::span(&value, 1))) && value == 12, "Guest calloc did not set ENOMEM");
    require(fixture.Call("malloc", {std::numeric_limits<std::uint64_t>::max()}) == 0, "Overflowing malloc succeeded");
}

void testMemoryAndStrings() {
    Fixture fixture;
    fixture.String(0x1000, "0123456789");
    require(fixture.Call("memmove", {0x1002, 0x1000, 8}) == 0x1002, "Guest memmove returned a host pointer");
    require(fixture.Bytes(0x1000, 10) == std::vector<std::byte>{std::byte{'0'}, std::byte{'1'}, std::byte{'0'}, std::byte{'1'}, std::byte{'2'}, std::byte{'3'}, std::byte{'4'}, std::byte{'5'}, std::byte{'6'}, std::byte{'7'}}, "Overlapping guest memmove corrupted bytes");
    require(fixture.Call("memchr", {0x1000, '4', 10}) == 0x1006, "Guest memchr returned an incorrect guest address");
    const auto original = fixture.Bytes(0x1ffc, 4);
    rejects([&] { fixture.Call("memcpy", {0x1ffc, 0x1000, 8}); }, "permitted guest memory");
    require(fixture.Bytes(0x1ffc, 4) == original, "Faulting native memcpy partially wrote the destination");
    fixture.String(0x1000, "metal");
    fixture.String(0x1100, "METAL");
    require(fixture.Call("strlen", {0x1000}) == 5 && fixture.Call("strnlen", {0x1000, 3}) == 3, "Guest string length changed");
    require(fixture.Call("strcasecmp", {0x1000, 0x1100}) == 0, "Guest case-insensitive comparison changed");
    require(fixture.Call("strncmp", {0, 0, 0}) == 0, "Zero-length string comparison dereferenced its inputs");
    require(fixture.Call("strlcpy", {0x1200, 0x1000, 3}) == 5 && fixture.Bytes(0x1200, 3) == std::vector<std::byte>{std::byte{'m'}, std::byte{'e'}, {}}, "Guest strlcpy truncation changed");
    require(fixture.Call("strlcat", {0x1200, 0x1000, 6}) == 7 && fixture.Call("strlen", {0x1200}) == 5, "Guest strlcat length changed");
    const auto duplicate = fixture.Call("strdup", {0x1000});
    require(duplicate != 0 && fixture.Call("strcmp", {0x1000, duplicate}) == 0, "Guest strdup did not create a readable guest allocation");
    require(fixture.Call("strchr", {0x1000, 0}) == 0x1005 && fixture.Call("strrchr", {0x1000, 't'}) == 0x1002, "Guest strchr address changed");
    fixture.String(0x1100, "tal");
    require(fixture.Call("strstr", {0x1000, 0x1100}) == 0x1002, "Guest substring returned an incorrect address");
    fixture.Call("strncpy", {0x1300, 0x1000, 8});
    require(fixture.Bytes(0x1305, 3) == std::vector<std::byte>(3), "Guest strncpy did not pad its output");
    rejects([&] { fixture.Call("strlen", {0x2000}); }, "outside readable guest memory");
    require(fixture.libc.Resolve({"unimplemented_function", 2, 1, 0}) == 0, "Unknown libc symbol was falsely resolved");
    require(fixture.libc.Resolve({"malloc", 1, 1, 8}) == 0, "Object import resolved to a native function");
    rejects([&] { fixture.libc.Bind(fixture.cpu, fixture.memory); }, "already bound");
}

void testIntegersAndCharacters() {
    Fixture fixture;
    fixture.String(0x1000, "  -0x2a tail");
    require(fixture.Call("strtol", {0x1000, 0x1100, 0}) == static_cast<std::uint64_t>(-42), "Guest signed integer conversion changed");
    std::uint64_t end = 0;
    require(fixture.memory.TryRead(0x1100, std::as_writable_bytes(std::span(&end, 1))) && end == 0x1007, "Guest integer conversion returned a host end pointer");
    fixture.String(0x1000, "18446744073709551616");
    require(fixture.Call("strtoull", {0x1000, 0, 10}) == std::numeric_limits<std::uint64_t>::max(), "Guest unsigned integer overflow changed");
    int error = 0;
    require(fixture.memory.TryRead(fixture.Call("__error"), std::as_writable_bytes(std::span(&error, 1))) && error == 34, "Guest integer conversion did not set ERANGE");
    for (std::uint64_t value = 0; value < 256; ++value) {
        require((fixture.Call("isalpha", {value}) != 0) == ((value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z')), "Guest alphabetic character classification changed");
        require(fixture.Call("tolower", {value}) == ((value >= 'A' && value <= 'Z') ? value + 32 : value), "Guest lowercase conversion changed");
    }
    rejects([&] { fixture.Call("isalpha", {256}); }, "unsigned char range");
}

void testFloatingReturnAndFiles() {
    Fixture fixture;
    fixture.String(0x1000, "16.25x");
    fixture.Call("strtod", {0x1000, 0x1100});
    double value = 0;
    std::memcpy(&value, fixture.returnedVector.data(), sizeof(value));
    require(value == 16.25, "Native double return did not use guest XMM0");
    fixture.Call("strtof", {0x1000, 0});
    float narrow = 0;
    std::memcpy(&narrow, fixture.returnedVector.data(), sizeof(narrow));
    require(narrow == 16.25f, "Native float return did not use guest XMM0");
    std::array<char, 64> pattern{};
    const std::string prefix = "/tmp/anyps5-arm64-stdio-XXXXXX";
    std::copy(prefix.begin(), prefix.end(), pattern.begin());
    const auto descriptor = ::mkstemp(pattern.data());
    require(descriptor >= 0, "Could not create native stdio test file");
    ::close(descriptor);
    const std::filesystem::path path(pattern.data());
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error; std::filesystem::remove(path, error); }
    } cleanup{path};
    fixture.String(0x1000, path.string());
    fixture.String(0x1100, "w+b");
    const auto file = fixture.Call("fopen", {0x1000, 0x1100});
    require(file != 0, "Guest fopen failed for a writable file");
    fixture.String(0x1200, "metal\n");
    require(fixture.Call("fwrite", {0x1200, 2, 3, file}) == 3, "Guest fwrite returned an incorrect item count");
    require(fixture.Call("ftello", {file}) == 6, "Guest ftello returned an incorrect position");
    require(fixture.Call("fseeko", {file, 0, SEEK_SET}) == 0, "Guest fseeko failed");
    require(fixture.Call("fread", {0x1300, 4, 2, file}) == 1, "Partial guest fread returned an incorrect item count");
    require(fixture.Bytes(0x1300, 6) == fixture.Bytes(0x1200, 6), "Guest fread omitted the partial final item");
    require(fixture.Call("feof", {file}) != 0 && fixture.Call("ferror", {file}) == 0, "Guest EOF state changed");
    fixture.Call("fseek", {file, 0, SEEK_SET});
    require(fixture.Call("feof", {file}) == 0, "Guest fseek did not clear EOF");
    require(fixture.Call("fgets", {0x1400, 16, file}) == 0x1400 && fixture.Call("strcmp", {0x1200, 0x1400}) == 0, "Guest fgets did not return a guest string");
    fixture.Call("fseek", {file, 0, SEEK_SET});
    const auto original = fixture.Bytes(0x1ffe, 2);
    rejects([&] { fixture.Call("fread", {0x1ffe, 1, 6, file}); }, "permitted guest memory");
    require(fixture.Call("ftell", {file}) == 0 && fixture.Bytes(0x1ffe, 2) == original, "Faulting guest fread consumed file bytes or wrote partial data");
    fixture.Call("fclose", {file});
    rejects([&] { fixture.Call("fgetc", {file}); }, "FILE handle");
    fixture.String(0x1000, path.string() + ".missing");
    fixture.String(0x1100, "rb");
    require(fixture.Call("fopen", {0x1000, 0x1100}) == 0, "Guest fopen did not report a missing file");
    const auto stdoutAddress = fixture.libc.Resolve({"__stdoutp", 1, 1, 8});
    std::uint64_t stream = 0;
    require(fixture.memory.TryRead(stdoutAddress, std::as_writable_bytes(std::span(&stream, 1))) && fixture.libc.File(stream) == stdout, "Guest standard stream object was not initialized");
}

void testFormatting() {
    Fixture fixture;
    fixture.String(0x1000, "%hhd %-6.*s %%");
    fixture.String(0x1100, "abcdef");
    fixture.String(0x1500, "-128 abc    %");
    require(fixture.Call("snprintf", {0x1200, 32, 0x1000, 128, 3, 0x1100}) == 13, "Guest integer width or dynamic string precision changed");
    require(fixture.Call("strcmp", {0x1200, 0x1500}) == 0, "Guest formatted bytes changed");
    fixture.String(0x1000, "abc%n");
    require(fixture.Call("snprintf", {0, 0, 0x1000, 0x1600}) == 3, "Zero-size snprintf returned an incorrect count");
    int count = 0;
    require(fixture.memory.TryRead(0x1600, std::as_writable_bytes(std::span(&count, 1))) && count == 3, "Guest format count used a host pointer");
    fixture.String(0x1000, "%.0s");
    require(fixture.Call("snprintf", {0x1200, 1, 0x1000, 0xffffffffffffffff}) == 0, "Zero-precision guest string dereferenced its argument");
    fixture.String(0x1000, "abcdef");
    require(fixture.Call("snprintf", {0x1200, 3, 0x1000}) == 6 && fixture.Bytes(0x1200, 3) == std::vector<std::byte>{std::byte{'a'}, std::byte{'b'}, {}}, "Guest snprintf truncation or count changed");
    fixture.String(0x1000, "%u");
    require(fixture.Call("asprintf", {0x1600, 0x1000, 42}) == 2, "Guest allocating format failed");
    std::uint64_t allocated = 0;
    require(fixture.memory.TryRead(0x1600, std::as_writable_bytes(std::span(&allocated, 1))) && fixture.Call("strlen", {allocated}) == 2, "Guest allocating format did not write a guest pointer");
    fixture.String(0x1000, "%llu %llu %.1f %.1f");
    struct VaList {
        std::uint32_t generalOffset;
        std::uint32_t vectorOffset;
        std::uint64_t stack;
        std::uint64_t registers;
    } arguments{40, 160, 0x1400, 0x1300};
    require(fixture.memory.TryWrite(0x1100, std::as_bytes(std::span(&arguments, 1))), "Could not initialize guest va_list");
    const std::uint64_t first = 111, second = 222;
    const double a = 1.5, b = 2.5;
    require(fixture.memory.TryWrite(0x1328, std::as_bytes(std::span(&first, 1))) &&
            fixture.memory.TryWrite(0x1400, std::as_bytes(std::span(&second, 1))) &&
            fixture.memory.TryWrite(0x13a0, std::as_bytes(std::span(&a, 1))) &&
            fixture.memory.TryWrite(0x1408, std::as_bytes(std::span(&b, 1))), "Could not initialize register-save and stack arguments");
    fixture.String(0x1500, "111 222 1.5 2.5");
    require(fixture.Call("vsnprintf", {0x1200, 32, 0x1000, 0x1100}) == 15 && fixture.Call("strcmp", {0x1200, 0x1500}) == 0, "Guest variadic register exhaustion or stack argument order changed");
    arguments.generalOffset = 49;
    require(fixture.memory.TryWrite(0x1100, std::as_bytes(std::span(&arguments, 1))), "Could not initialize malformed guest va_list");
    const auto before = fixture.Bytes(0x1200, 32);
    rejects([&] { fixture.Call("vsnprintf", {0x1200, 32, 0x1000, 0x1100}); }, "va_list offsets");
    require(fixture.Bytes(0x1200, 32) == before, "Malformed va_list changed formatted output");
    fixture.String(0x1000, "%2$u");
    rejects([&] { fixture.Call("snprintf", {0x1200, 32, 0x1000, 42}); }, "Positional");
    fixture.String(0x1000, "%Lf");
    rejects([&] { fixture.Call("snprintf", {0x1200, 32, 0x1000}); }, "extended floating-point");
    require(std::setlocale(LC_CTYPE, "en_US.UTF-8") != nullptr, "Could not activate UTF-8 locale for wide precision regression");
    struct Locale { ~Locale() { std::setlocale(LC_CTYPE, "C"); } } locale;
    const wchar_t euro = L'\u20ac';
    require(fixture.memory.TryWrite(0x1ffc, std::as_bytes(std::span(&euro, 1))), "Could not initialize bounded wide string");
    fixture.String(0x1000, "%.2ls");
    require(fixture.Call("snprintf", {0x1200, 32, 0x1000, 0x1ffc}) == 0 && fixture.Bytes(0x1200, 1)[0] == std::byte{}, "Wide precision split a multibyte character or read past the required input");
    fixture.String(0x1000, "%.3ls");
    require(fixture.Call("snprintf", {0x1200, 32, 0x1000, 0x1ffc}) == 3 && fixture.Bytes(0x1200, 4) == std::vector<std::byte>{std::byte{0xe2}, std::byte{0x82}, std::byte{0xac}, {}}, "Wide precision did not emit one complete UTF-8 character");
}

}

int main() {
    try {
        testHeap();
        testMemoryAndStrings();
        testIntegersAndCharacters();
        testFloatingReturnAndFiles();
        testFormatting();
        std::puts("Native ARM64 libc guest allocation, memory, string and integer tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
