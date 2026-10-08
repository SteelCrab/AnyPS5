#include "NativeLocale.hpp"
#include "NativePosix.hpp"
#include <array>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <ctime>
#include <limits>
#include <locale.h>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <xlocale.h>

namespace AnyPS5::Arm64 {

namespace {

constexpr std::uint64_t functionBase = 0x790000000;
constexpr std::uint64_t maximumAddress = 0x790100000;
constexpr auto failed = std::numeric_limits<std::uint64_t>::max();
constexpr auto incomplete = failed - 1;
enum class Operation { Set, Decode, Length, LegacyDecode, Encode, WideToBytes, BytesToWide, Compare, WideCompare, Transform, WideTransform, FormatTime, LocalTime, Initial };
struct Export { std::string_view name; Operation operation; };
const auto exports = std::to_array<Export>({
    {"setlocale", Operation::Set}, {"mbrtowc", Operation::Decode}, {"mbrlen", Operation::Length},
    {"mbtowc", Operation::LegacyDecode}, {"wcrtomb", Operation::Encode}, {"wcstombs", Operation::WideToBytes},
    {"mbsrtowcs", Operation::BytesToWide}, {"strcoll", Operation::Compare}, {"wcscoll", Operation::WideCompare},
    {"strxfrm", Operation::Transform}, {"wcsxfrm", Operation::WideTransform}, {"strftime", Operation::FormatTime},
    {"localtime", Operation::LocalTime}, {"mbsinit", Operation::Initial}
});
const std::array<int, 7> masks{LC_ALL_MASK, LC_COLLATE_MASK, LC_CTYPE_MASK, LC_MONETARY_MASK, LC_NUMERIC_MASK, LC_TIME_MASK, LC_MESSAGES_MASK};
const std::array<const char*, 7> categories{"LC_ALL", "LC_COLLATE", "LC_CTYPE", "LC_MONETARY", "LC_NUMERIC", "LC_TIME", "LC_MESSAGES"};
static_assert(sizeof(wchar_t) == 4 && sizeof(std::mbstate_t) == 128);

template<class TValue>
TValue read(const GuestMemory& memory, std::uint64_t address) {
    TValue value{};
    if (!memory.TryRead(address, std::as_writable_bytes(std::span(&value, 1)))) throw std::runtime_error("Native locale argument is outside readable guest memory");
    return value;
}

template<class TValue>
void write(GuestMemory& memory, std::uint64_t address, const TValue& value) {
    if (!memory.TryWrite(address, std::as_bytes(std::span(&value, 1)))) throw std::runtime_error("Native locale argument is outside writable guest memory");
}

template<class TCharacter>
std::basic_string<TCharacter> string(const GuestMemory& memory, std::uint64_t address) {
    std::basic_string<TCharacter> value;
    for (;;) {
        const auto character = read<TCharacter>(memory, address);
        if (character == 0) return value;
        value.push_back(character);
        if (address > failed - sizeof(character)) throw std::runtime_error("Native locale string address overflows");
        address += sizeof(character);
    }
}

template<class TCharacter>
void writeString(GuestMemory& memory, std::uint64_t address, const std::basic_string<TCharacter>& value) {
    if (!memory.TryWrite(address, std::as_bytes(std::span(value.c_str(), value.size() + 1)))) throw std::runtime_error("Native locale string is outside writable guest memory");
}

std::size_t decode(const GuestMemory& memory, std::uint64_t source, std::size_t count, wchar_t& output, std::mbstate_t& state, locale_t locale) {
    if (source == 0) { const char zero = 0; return ::mbrtowc_l(nullptr, &zero, 1, &state, locale); }
    for (std::size_t i = 0; i < count && i < MB_LEN_MAX; ++i) {
        const auto byte = read<char>(memory, source + i);
        const auto result = ::mbrtowc_l(&output, &byte, 1, &state, locale);
        if (result == failed || result == 0) return result;
        if (result != incomplete) return i + 1;
    }
    return incomplete;
}

struct Time {
    std::array<std::int32_t, 9> fields;
    std::int32_t padding;
    std::int64_t offset;
    std::uint64_t zone;
};
static_assert(sizeof(Time) == 56 && offsetof(Time, offset) == 40);

}

struct NativeLocale::Impl {
    struct Binding { NativeLocale* owner; Operation operation; };
    NativeLibc& libc;
    NativePosix& posix;
    std::recursive_mutex mutex;
    locale_t locale;
    GuestMemory* memory = nullptr;
    std::array<Binding, exports.size()> bindings{};
    std::unordered_map<std::thread::id, std::array<std::mbstate_t, 5>> states;
    std::unordered_map<std::thread::id, std::uint64_t> times;
    std::uint64_t query = 0;
    Impl(NativeLibc& libc, NativePosix& posix) : libc(libc), posix(posix), locale(::newlocale(LC_ALL_MASK, "C", nullptr)) {
        if (!locale) throw std::runtime_error("Cannot initialize the guest C locale");
        libc.SetLocale(locale);
    }
    ~Impl() { libc.SetLocale(nullptr); ::freelocale(locale); }
    std::uint64_t copy(GuestMemory& memory, const std::string& value) {
        const auto address = libc.Allocate(memory, value.size() + 1);
        if (address != 0) writeString(memory, address, value);
        return address;
    }
    std::uint64_t fail(GuestMemory& memory, int error) { libc.SetError(memory, error); return failed; }
    std::string localeName(GuestMemory& memory, int category) {
        for (const auto* variable : {"LC_ALL", categories[category], "LANG"}) {
            const auto value = posix.GetEnvironment(memory, variable);
            if (value && !value->empty()) return *value;
        }
        return "C";
    }
};

NativeLocale::NativeLocale(NativeLibc& libc, NativePosix& posix) : impl(std::make_unique<Impl>(libc, posix)) {
    for (std::size_t i = 0; i < exports.size(); ++i) impl->bindings[i] = {this, exports[i].operation};
}
NativeLocale::~NativeLocale() = default;

std::uint64_t NativeLocale::Resolve(const ElfImport& symbol) {
    if (symbol.name == "__mb_cur_max" && (symbol.type == 0 || symbol.type == 1) && symbol.size <= 4) return maximumAddress;
    if (symbol.type != 0 && symbol.type != 2) return 0;
    for (std::size_t i = 0; i < exports.size(); ++i) if (exports[i].name == symbol.name) return functionBase + i * 16;
    return 0;
}

void NativeLocale::Bind(Cpu& cpu, GuestMemory& memory) {
    std::lock_guard lock(impl->mutex);
    if (impl->memory && impl->memory != &memory) throw std::runtime_error("Native locale requires a single guest address space");
    if (!impl->memory) {
        memory.Map(maximumAddress, 4, Read | Write);
        write(memory, maximumAddress, std::int32_t{1});
        impl->memory = &memory;
    }
    for (std::size_t i = 0; i < exports.size(); ++i) cpu.Bind(functionBase + i * 16, +[](const HostCall& call, void* context) {
        const auto& binding = *static_cast<Impl::Binding*>(context);
        return binding.owner->invoke(call, static_cast<std::size_t>(binding.operation));
    }, &impl->bindings[i]);
}

std::uint64_t NativeLocale::invoke(const HostCall& call, std::size_t operation) {
    const auto op = static_cast<Operation>(operation);
    const auto& args = call.arguments;
    auto& memory = call.memory;
    std::lock_guard lock(impl->mutex);
    if (op == Operation::Set) {
        const auto category = static_cast<int>(args[0]);
        if (category < 0 || category > 6) { impl->libc.SetError(memory, 22); return 0; }
        if (args[1] != 0) {
            const auto requested = string<char>(memory, args[1]);
            std::unique_ptr<std::remove_pointer_t<locale_t>, decltype(&::freelocale)> base(::duplocale(impl->locale), &::freelocale);
            if (!base) { impl->libc.SetError(memory, guestError(errno)); return 0; }
            const auto first = requested.empty() && category == 0 ? 1 : category;
            const auto last = requested.empty() && category == 0 ? 6 : category;
            for (int i = first; i <= last; ++i) {
                auto name = requested.empty() ? impl->localeName(memory, i) : requested;
                if (name == "C.UTF-8" || name == "C.utf8") name = "UTF-8";
                const auto next = ::newlocale(masks[i], name.c_str(), base.get());
                if (!next) { impl->libc.SetError(memory, guestError(errno)); return 0; }
                base.release();
                base.reset(next);
            }
            impl->libc.SetLocale(base.get());
            ::freelocale(impl->locale);
            impl->locale = base.release();
            const auto previous = ::uselocale(impl->locale);
            const auto maximum = static_cast<std::int32_t>(MB_CUR_MAX);
            ::uselocale(previous);
            write(memory, maximumAddress, maximum);
        }
        std::string name;
        if (category == 0) {
            const std::string first = ::querylocale(LC_COLLATE_MASK, impl->locale);
            bool same = true;
            for (int i = 1; i <= 6; ++i) { const std::string current = ::querylocale(masks[i], impl->locale); same = same && current == first; if (i > 1) name += '/'; name += current; }
            if (same) name = first;
        } else name = ::querylocale(masks[category], impl->locale);
        const auto result = impl->copy(memory, name);
        if (result == 0) return 0;
        if (impl->query != 0) impl->libc.Free(memory, impl->query);
        impl->query = result;
        return result;
    }
    if (op == Operation::Decode || op == Operation::Length || op == Operation::LegacyDecode) {
        const bool length = op == Operation::Length;
        const auto destination = length ? 0 : args[0], source = args[length ? 0 : 1], count = args[length ? 1 : 2];
        const auto address = op == Operation::LegacyDecode ? 0 : args[3 - (length ? 1 : 0)];
        auto& implicit = impl->states[std::this_thread::get_id()][op == Operation::Decode ? 0 : length ? 1 : 2];
        auto state = address == 0 ? implicit : read<std::mbstate_t>(memory, address);
        if (address != 0 && !memory.Contains(address, 128, Write)) throw std::runtime_error("Native multibyte state is outside writable guest memory");
        if (destination != 0 && source != 0 && !memory.Contains(destination, 4, Write)) throw std::runtime_error("Native wide character is outside writable guest memory");
        if (op == Operation::LegacyDecode && source == 0) { implicit = {}; return 0; }
        wchar_t output = 0;
        const auto result = decode(memory, source, count, output, state, impl->locale);
        if (address == 0) implicit = state; else write(memory, address, state);
        if (result == failed || (op == Operation::LegacyDecode && result == incomplete)) return impl->fail(memory, 86);
        if (result != incomplete && destination != 0 && source != 0) write(memory, destination, output);
        return result;
    }
    if (op == Operation::Initial) {
        if (args[0] == 0) return 1;
        const auto state = read<std::mbstate_t>(memory, args[0]);
        return ::mbsinit_l(&state, impl->locale);
    }
    if (op == Operation::Encode) {
        auto& implicit = impl->states[std::this_thread::get_id()][3];
        auto state = args[2] == 0 ? implicit : read<std::mbstate_t>(memory, args[2]);
        if (args[2] != 0 && !memory.Contains(args[2], 128, Write)) throw std::runtime_error("Native multibyte state is outside writable guest memory");
        std::array<char, MB_LEN_MAX> bytes{};
        const auto count = ::wcrtomb_l(bytes.data(), args[0] == 0 ? 0 : static_cast<wchar_t>(args[1]), &state, impl->locale);
        if (count == failed) return impl->fail(memory, 86);
        if (args[0] != 0 && !memory.Contains(args[0], count, Write)) throw std::runtime_error("Native encoded character is outside writable guest memory");
        if (args[0] != 0 && !memory.TryWrite(args[0], std::as_bytes(std::span(bytes).first(count)))) throw std::runtime_error("Native encoded character write failed");
        if (args[2] == 0) implicit = state; else write(memory, args[2], state);
        return count;
    }
    if (op == Operation::WideToBytes) {
        std::mbstate_t state{};
        std::size_t written = 0;
        for (auto source = args[1]; args[0] == 0 || written < args[2]; source += 4) {
            const auto character = read<wchar_t>(memory, source);
            std::array<char, MB_LEN_MAX> bytes{};
            const auto count = ::wcrtomb_l(bytes.data(), character, &state, impl->locale);
            if (count == failed) return impl->fail(memory, 86);
            if (args[0] != 0) {
                if (count > args[2] - written) break;
                if (!memory.TryWrite(args[0] + written, std::as_bytes(std::span(bytes).first(count)))) throw std::runtime_error("Native wide conversion destination is outside writable guest memory");
            }
            if (character == 0) return written + count - 1;
            written += count;
        }
        return written;
    }
    if (op == Operation::BytesToWide) {
        auto source = read<std::uint64_t>(memory, args[1]);
        auto state = args[3] == 0 ? impl->states[std::this_thread::get_id()][4] : read<std::mbstate_t>(memory, args[3]);
        if (args[0] != 0 && (!memory.Contains(args[1], 8, Write) || (args[3] != 0 && !memory.Contains(args[3], 128, Write)))) throw std::runtime_error("Native multibyte conversion state is outside writable guest memory");
        const auto save = [&] {
            if (args[0] == 0) return;
            write(memory, args[1], source);
            if (args[3] == 0) impl->states[std::this_thread::get_id()][4] = state; else write(memory, args[3], state);
        };
        std::size_t converted = 0;
        while (args[0] == 0 || converted < args[2]) {
            wchar_t character = 0;
            const auto count = decode(memory, source, MB_LEN_MAX, character, state, impl->locale);
            if (count == failed || count == incomplete) { save(); return impl->fail(memory, 86); }
            if (args[0] != 0) write(memory, args[0] + converted * 4, character);
            if (count == 0) { source = 0; save(); return converted; }
            source += count;
            ++converted;
        }
        save();
        return converted;
    }
    if (op == Operation::Compare) return static_cast<std::uint64_t>(static_cast<std::int64_t>(::strcoll_l(string<char>(memory, args[0]).c_str(), string<char>(memory, args[1]).c_str(), impl->locale)));
    if (op == Operation::WideCompare) return static_cast<std::uint64_t>(static_cast<std::int64_t>(::wcscoll_l(string<wchar_t>(memory, args[0]).c_str(), string<wchar_t>(memory, args[1]).c_str(), impl->locale)));
    if (op == Operation::Transform || op == Operation::WideTransform) {
        const auto width = op == Operation::Transform ? 1u : 4u;
        if (args[2] > failed / width || (args[2] != 0 && !memory.Contains(args[0], args[2] * width, Write))) throw std::runtime_error("Native collation transform destination is outside writable guest memory");
        if (op == Operation::Transform) {
            std::vector<char> buffer(args[2]);
            const auto count = ::strxfrm_l(buffer.data(), string<char>(memory, args[1]).c_str(), buffer.size(), impl->locale);
            if (!memory.TryWrite(args[0], std::as_bytes(std::span(buffer)))) throw std::runtime_error("Native collation transform write failed");
            return count;
        }
        std::vector<wchar_t> buffer(args[2]);
        const auto count = ::wcsxfrm_l(buffer.data(), string<wchar_t>(memory, args[1]).c_str(), buffer.size(), impl->locale);
        if (!memory.TryWrite(args[0], std::as_bytes(std::span(buffer)))) throw std::runtime_error("Native wide collation transform write failed");
        return count;
    }
    if (op == Operation::FormatTime) {
        const auto guest = read<Time>(memory, args[3]);
        auto zone = guest.zone == 0 ? std::string{} : string<char>(memory, guest.zone);
        std::tm time{guest.fields[0], guest.fields[1], guest.fields[2], guest.fields[3], guest.fields[4], guest.fields[5], guest.fields[6], guest.fields[7], guest.fields[8], guest.offset, guest.zone == 0 ? nullptr : zone.data()};
        if (args[1] != 0 && !memory.Contains(args[0], args[1], Write)) throw std::runtime_error("Native time format destination is outside writable guest memory");
        std::vector<char> bytes(args[1]);
        const auto count = ::strftime_l(bytes.data(), bytes.size(), string<char>(memory, args[2]).c_str(), &time, impl->locale);
        if (count != 0 && !memory.TryWrite(args[0], std::as_bytes(std::span(bytes).first(count + 1)))) throw std::runtime_error("Native time format write failed");
        return count;
    }
    if (op == Operation::LocalTime) {
        const auto epoch = read<std::time_t>(memory, args[0]);
        std::tm time{};
        if (!::localtime_r(&epoch, &time)) { impl->libc.SetError(memory, guestError(errno)); return 0; }
        const auto zone = impl->copy(memory, time.tm_zone ? time.tm_zone : "");
        if (zone == 0) return 0;
        auto& address = impl->times[std::this_thread::get_id()];
        if (address == 0) address = impl->libc.Allocate(memory, sizeof(Time));
        if (address == 0) { impl->libc.Free(memory, zone); return 0; }
        const auto oldZone = read<Time>(memory, address).zone;
        write(memory, address, Time{{time.tm_sec, time.tm_min, time.tm_hour, time.tm_mday, time.tm_mon, time.tm_year, time.tm_wday, time.tm_yday, time.tm_isdst}, 0, time.tm_gmtoff, zone});
        if (oldZone != 0) impl->libc.Free(memory, oldZone);
        return address;
    }
    throw std::runtime_error("Invalid native locale operation");
}

}
