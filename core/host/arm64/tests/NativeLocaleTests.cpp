#include "NativeLocale.hpp"
#include "NativePosix.hpp"
#include <array>
#include <clocale>
#include <cstring>
#include <ctime>
#include <stdexcept>

using namespace AnyPS5::Arm64;

namespace {

void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }

struct Fixture {
    GuestMemory memory;
    NativeLibc libc;
    NativePosix posix{libc};
    NativeLocale locale{libc, posix};
    Cpu cpu{memory};
    Fixture() {
        memory.Map(0x1000, 0x2000, Read | Write);
        memory.Map(0x8000, 0x1000, Read | Write);
        libc.Bind(cpu, memory);
        posix.Bind(cpu);
        locale.Bind(cpu, memory);
    }
    std::uint64_t Call(std::string_view name, std::array<std::uint64_t, 6> arguments = {}) {
        CpuState state;
        state.rip = locale.Resolve({name, 2, 1, 0});
        if (state.rip == 0) state.rip = posix.Resolve({name, 2, 1, 0});
        if (state.rip == 0) state.rip = libc.Resolve({name, 2, 1, 0});
        require(state.rip != 0, "Locale function was not resolved");
        state.registers[4] = 0x8ff8;
        const std::array<std::uint8_t, 6> registers{7, 6, 2, 1, 8, 9};
        for (std::size_t i = 0; i < arguments.size(); ++i) state.registers[registers[i]] = arguments[i];
        cpu.Run(state);
        return state.registers[0];
    }
    template<class TValue>
    TValue Value(std::uint64_t address) {
        TValue value{};
        require(memory.TryRead(address, std::as_writable_bytes(std::span(&value, 1))), "Locale guest value read failed");
        return value;
    }
    template<class TValue>
    void Value(std::uint64_t address, const TValue& value) {
        require(memory.TryWrite(address, std::as_bytes(std::span(&value, 1))), "Locale guest value write failed");
    }
    void String(std::uint64_t address, const std::string& value) {
        require(memory.TryWrite(address, std::as_bytes(std::span(value.c_str(), value.size() + 1))), "Locale guest string write failed");
    }
    std::string String(std::uint64_t address) {
        std::string result;
        for (;;) { const auto byte = Value<char>(address++); if (byte == 0) return result; result.push_back(byte); }
    }
    int Error() { return Value<int>(libc.ErrorAddress(memory)); }
    void Utf8() {
        String(0x1000, "en_US.UTF-8");
        require(Call("setlocale", {0, 0x1000}) != 0, "Cannot activate the guest UTF-8 locale");
    }
};

void testLocaleAndIncrementalConversion() {
    const std::string hostLocale = std::setlocale(LC_ALL, nullptr);
    Fixture fixture;
    const auto maximum = fixture.locale.Resolve({"__mb_cur_max", 1, 1, 4});
    require(maximum != 0 && fixture.Value<int>(maximum) == 1 && fixture.String(fixture.Call("setlocale", {0, 0})) == "C", "Guest locale did not start in C");
    fixture.String(0x1100, "\xc3\xa9");
    require(fixture.Call("mbrtowc", {0x1200, 0x1100, 2, 0x1400}) == 1 && fixture.Value<wchar_t>(0x1200) == 0xc3, "C locale did not preserve its single-byte encoding");
    fixture.Value(0x1400, std::array<std::byte, 128>{});
    fixture.Utf8();
    require(fixture.Value<int>(maximum) >= 4 && hostLocale == std::setlocale(LC_ALL, nullptr), "Guest locale changed host settings or retained the wrong multibyte maximum");
    fixture.String(0x1100, "\xe2\x82\xac");
    require(fixture.Call("mbrtowc", {0x1200, 0x1100, 1, 0x1400}) == UINT64_MAX - 1 && fixture.Call("mbsinit", {0x1400}) == 0, "Incomplete UTF-8 did not retain its conversion state");
    require(fixture.Call("mbrtowc", {0x1200, 0x1101, 2, 0x1400}) == 2 && fixture.Value<std::uint32_t>(0x1200) == 0x20ac && fixture.Call("mbsinit", {0x1400}) != 0, "Incremental UTF-8 completion returned the wrong character or byte count");
    require(fixture.Call("wcrtomb", {0x1300, 0x1f642, 0x1500}) == 4 && fixture.String(0x1300) == "\xf0\x9f\x99\x82", "Non-BMP wide character did not encode to UTF-8");
    require(fixture.Call("wcrtomb", {0x1300, 0xd800, 0x1500}) == UINT64_MAX && fixture.Error() == 86, "UTF-8 encoded an invalid surrogate");
    fixture.String(0x1100, "\xc0\xaf");
    require(fixture.Call("mbrtowc", {0x1200, 0x1100, 2, 0x1400}) == UINT64_MAX && fixture.Error() == 86, "UTF-8 decoder accepted an overlong encoding");
    fixture.Value(0x1400, std::array<std::byte, 128>{});
    fixture.Value<char>(0x2fff, 'A');
    require(fixture.Call("mbrtowc", {0x1200, 0x2fff, 64, 0x1400}) == 1 && fixture.Value<wchar_t>(0x1200) == L'A', "UTF-8 decoder read past a complete character at a mapping edge");
    require(fixture.Call("mbrlen", {0, 0, 0x1400}) == 0 && fixture.Call("mbrlen", {0x3000, 0, 0x1400}) == UINT64_MAX - 1, "Zero-input multibyte conversion dereferenced a buffer");
    fixture.String(0x1000, "ANYPS5_UNKNOWN_LOCALE");
    require(fixture.Call("setlocale", {0, 0x1000}) == 0 && fixture.Value<int>(maximum) >= 4, "Failed locale selection changed the active locale");
}

void testWideStrings() {
    Fixture fixture;
    fixture.Utf8();
    fixture.Value(0x1100, std::array<wchar_t, 4>{L'A', 0x20ac, 0x1f642, 0});
    require(fixture.Call("wcstombs", {0, 0x1100, 0}) == 8, "Wide-to-byte size query changed");
    require(fixture.Call("wcstombs", {0x1200, 0x1100, 3}) == 1 && fixture.Value<char>(0x1200) == 'A' && fixture.Value<char>(0x1201) == 0, "Bounded conversion split a multibyte character");
    require(fixture.Call("wcstombs", {0x1200, 0x1100, 16}) == 8 && fixture.String(0x1200) == "A\xe2\x82\xac\xf0\x9f\x99\x82", "Wide-to-byte conversion changed UTF-8 bytes");
    fixture.Value<std::uint64_t>(0x1300, 0x1200);
    require(fixture.Call("mbsrtowcs", {0, 0x1300, 0, 0x1600}) == 3 && fixture.Value<std::uint64_t>(0x1300) == 0x1200, "Multibyte size query changed the source pointer");
    require(fixture.Call("mbsrtowcs", {0x1400, 0x1300, 2, 0x1600}) == 2 && fixture.Value<std::uint64_t>(0x1300) == 0x1204 && fixture.Value<std::array<wchar_t, 2>>(0x1400) == std::array<wchar_t, 2>{L'A', 0x20ac}, "Bounded wide conversion advanced by the wrong number of bytes");
    require(fixture.Call("mbsrtowcs", {0x1408, 0x1300, 2, 0x1600}) == 1 && fixture.Value<std::uint64_t>(0x1300) == 0 && fixture.Value<std::array<wchar_t, 4>>(0x1400) == std::array<wchar_t, 4>{L'A', 0x20ac, 0x1f642, 0}, "Wide conversion did not terminate or clear its source pointer");
    fixture.String(0x1700, "%ls");
    require(fixture.Call("snprintf", {0x1800, 32, 0x1700, 0x1100}) == 8 && fixture.String(0x1800) == fixture.String(0x1200), "Formatted wide output ignored the guest locale");
    fixture.String(0x1000, "C");
    fixture.Call("setlocale", {0, 0x1000});
    fixture.String(0x1900, "metal");
    fixture.String(0x1a00, "zinc");
    require(static_cast<std::int64_t>(fixture.Call("strcoll", {0x1900, 0x1a00})) < 0, "C locale collation changed");
    require(fixture.Call("strxfrm", {0, 0x1900, 0}) == 5 && fixture.Call("strxfrm", {0x1b00, 0x1900, 16}) == 5 && fixture.String(0x1b00) == "metal", "C locale collation transform changed");
    fixture.Value(0x1c00, std::array<wchar_t, 3>{L'a', L'b', 0});
    require(fixture.Call("wcsxfrm", {0x1d00, 0x1c00, 4}) == 2 && fixture.Value<std::array<wchar_t, 3>>(0x1d00) == std::array<wchar_t, 3>{L'a', L'b', 0}, "Wide collation transform used the wrong character width");
}

void testEnvironmentSelectionAndFunctionState() {
    Fixture fixture;
    fixture.String(0x1000, "LC_ALL");
    fixture.Call("unsetenv", {0x1000});
    fixture.String(0x1100, "LC_CTYPE");
    fixture.String(0x1200, "en_US.UTF-8");
    fixture.String(0x1300, "");
    require(fixture.Call("setenv", {0x1100, 0x1200, 1}) == 0 && fixture.Call("setlocale", {2, 0x1300}) != 0, "Empty locale selection ignored the guest environment");
    const auto maximum = fixture.locale.Resolve({"__mb_cur_max", 1, 1, 4});
    require(fixture.Value<int>(maximum) >= 4, "Guest environment did not select UTF-8");
    fixture.String(0x1400, "\xe2\x82\xac");
    require(fixture.Call("mbrtowc", {0x1500, 0x1400, 1, 0}) == UINT64_MAX - 1, "Could not retain an implicit incomplete character");
    fixture.String(0x1600, "A");
    fixture.Value<std::uint64_t>(0x1700, 0x1600);
    require(fixture.Call("mbsrtowcs", {0x1800, 0x1700, 2, 0}) == 1 && fixture.Value<wchar_t>(0x1800) == L'A', "Separate multibyte functions shared implicit conversion state");
    require(fixture.Call("mbrtowc", {0x1500, 0x1401, 2, 0}) == 2 && fixture.Value<wchar_t>(0x1500) == 0x20ac, "Independent string conversion changed a pending character");
    fixture.String(0x1200, "C");
    require(fixture.Call("setenv", {0x1000, 0x1200, 1}) == 0 && fixture.Call("setlocale", {2, 0x1300}) != 0 && fixture.Value<int>(maximum) == 1, "Guest LC_ALL did not override LC_CTYPE");
}

void testTime() {
    Fixture fixture;
    struct Time { std::array<std::int32_t, 9> fields; std::int32_t padding; std::int64_t offset; std::uint64_t zone; };
    fixture.Value(0x1100, Time{{5, 4, 3, 2, 0, 124, 2, 1, 0}, 0, 0, 0});
    fixture.String(0x1200, "%Y-%m-%d %H:%M:%S");
    require(fixture.Call("strftime", {0x1300, 64, 0x1200, 0x1100}) == 19 && fixture.String(0x1300) == "2024-01-02 03:04:05", "Guest broken-down time ABI changed");
    require(fixture.Call("strftime", {0x1300, 3, 0x1200, 0x1100}) == 0, "Short time format buffer succeeded");
    const std::time_t epoch = 1704164645;
    fixture.Value(0x1400, epoch);
    const auto address = fixture.Call("localtime", {0x1400});
    std::tm native{};
    require(::localtime_r(&epoch, &native) != nullptr && address != 0, "Native localtime failed");
    const auto converted = fixture.Value<Time>(address);
    require(converted.fields[3] == native.tm_mday && converted.fields[5] == native.tm_year && converted.offset == native.tm_gmtoff && fixture.String(converted.zone) == native.tm_zone, "Guest localtime structure or timezone pointer changed");
}

}

int main() {
    try { testLocaleAndIncrementalConversion(); testWideStrings(); testEnvironmentSelectionAndFunctionState(); testTime(); std::puts("Guest locale/environment isolation, incremental UTF-8, 32-bit wide characters and time ABI passed"); return 0; }
    catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
