#include "NativePosix.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <climits>
#include <crt_externs.h>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>
#include <unordered_set>

namespace AnyPS5::Arm64 {

int guestError(int value) noexcept {
    if ((value >= 0 && value <= 81) || value == EOVERFLOW) return value;
    switch (value) {
    case EIDRM: return 82;
    case ENOMSG: return 83;
    case ECANCELED: return 85;
    case EILSEQ: return 86;
    case ENOATTR: return 87;
    case EBADMSG: return 89;
    case EMULTIHOP: return 90;
    case ENOLINK: return 91;
    case EPROTO: return 92;
    case EOPNOTSUPP: return 45;
    case ENOTRECOVERABLE: return 95;
    case EOWNERDEAD: return 96;
    case EBADEXEC: case EBADARCH: case ESHLIBVERS: case EBADMACHO: return 8;
    default: return 5;
    }
}

namespace {

constexpr std::uint64_t functionBase = 0x780000000;
enum class Operation {
    Open, Close, Read, Write, Seek, Stat, Fstat, Mkdir, Unlink, Remove, Rename,
    OpenDir, CloseDir, ReadDir, Cwd, Timeval, Nanosleep, Sleep, Time, Clock,
    Getenv, Setenv, Unsetenv, Putenv, Strerror, Perror, Sysconf, Getargv, Getargc,
    Strtok, StrtokR, Rand, Srand, Qsort, Bsearch
};
struct Export { std::string_view name; Operation operation; };
const auto exports = std::to_array<Export>({
    {"open", Operation::Open}, {"close", Operation::Close}, {"read", Operation::Read}, {"write", Operation::Write},
    {"lseek", Operation::Seek}, {"stat", Operation::Stat}, {"fstat", Operation::Fstat}, {"mkdir", Operation::Mkdir},
    {"unlink", Operation::Unlink}, {"remove", Operation::Remove}, {"rename", Operation::Rename},
    {"opendir", Operation::OpenDir}, {"closedir", Operation::CloseDir}, {"readdir", Operation::ReadDir},
    {"getcwd", Operation::Cwd}, {"gettimeofday", Operation::Timeval}, {"nanosleep", Operation::Nanosleep},
    {"sleep", Operation::Sleep}, {"time", Operation::Time}, {"clock_gettime", Operation::Clock},
    {"getenv", Operation::Getenv}, {"setenv", Operation::Setenv}, {"unsetenv", Operation::Unsetenv},
    {"putenv", Operation::Putenv}, {"strerror", Operation::Strerror}, {"perror", Operation::Perror},
    {"sysconf", Operation::Sysconf}, {"getargv", Operation::Getargv}, {"getargc", Operation::Getargc},
    {"strtok", Operation::Strtok}, {"strtok_r", Operation::StrtokR}, {"rand", Operation::Rand},
    {"srand", Operation::Srand},
    {"qsort", Operation::Qsort}, {"bsearch", Operation::Bsearch}
});

std::uint64_t integer(std::int64_t value) { return static_cast<std::uint64_t>(value); }

template<class TValue>
TValue read(const GuestMemory& memory, std::uint64_t address) {
    TValue value{};
    if (!memory.TryRead(address, std::as_writable_bytes(std::span(&value, 1)))) throw std::runtime_error("Native POSIX argument is outside readable guest memory");
    return value;
}

template<class TValue>
void write(GuestMemory& memory, std::uint64_t address, const TValue& value) {
    if (!memory.TryWrite(address, std::as_bytes(std::span(&value, 1)))) throw std::runtime_error("Native POSIX argument is outside writable guest memory");
}

std::string string(const GuestMemory& memory, std::uint64_t address) {
    std::string value;
    for (;;) {
        const auto character = read<char>(memory, address);
        if (character == 0) return value;
        value.push_back(character);
        if (++address == 0) throw std::runtime_error("Native POSIX string address overflows");
    }
}

void writeString(GuestMemory& memory, std::uint64_t address, const std::string& value) {
    if (!memory.TryWrite(address, std::as_bytes(std::span(value.c_str(), value.size() + 1)))) throw std::runtime_error("Native POSIX string is outside writable guest memory");
}

bool validName(const std::string& name) { return !name.empty() && name.find('=') == std::string::npos; }

struct Timespec { std::int64_t seconds, nanoseconds; };
struct Timeval { std::int64_t seconds, microseconds; };
struct Stat {
    std::uint32_t device, inode;
    std::uint16_t mode, links;
    std::uint32_t user, group, specialDevice;
    Timespec access, modification, change;
    std::int64_t size, blocks;
    std::uint32_t blockSize, flags, generation;
    std::int32_t spare;
    Timespec birth;
};
static_assert(sizeof(Stat) == 120 && offsetof(Stat, size) == 72 && offsetof(Stat, birth) == 104);
struct Dirent {
    std::uint32_t inode;
    std::uint16_t length;
    std::uint8_t type, nameLength;
    std::array<char, 256> name;
};
static_assert(sizeof(Dirent) == 264 && offsetof(Dirent, name) == 8);

Stat convert(const struct stat& value) {
    return {static_cast<std::uint32_t>(value.st_dev), static_cast<std::uint32_t>(value.st_ino), value.st_mode, value.st_nlink,
        value.st_uid, value.st_gid, static_cast<std::uint32_t>(value.st_rdev),
        {value.st_atimespec.tv_sec, value.st_atimespec.tv_nsec}, {value.st_mtimespec.tv_sec, value.st_mtimespec.tv_nsec},
        {value.st_ctimespec.tv_sec, value.st_ctimespec.tv_nsec}, value.st_size, value.st_blocks,
        static_cast<std::uint32_t>(value.st_blksize), value.st_flags, value.st_gen, 0,
        {value.st_birthtimespec.tv_sec, value.st_birthtimespec.tv_nsec}};
}

std::string errorText(int error) {
    if (error >= 0 && error <= 81) return std::strerror(error);
    switch (error) {
    case 82: return std::strerror(EIDRM);
    case 83: return std::strerror(ENOMSG);
    case 84: return std::strerror(EOVERFLOW);
    case 85: return std::strerror(ECANCELED);
    case 86: return std::strerror(EILSEQ);
    case 87: return std::strerror(ENOATTR);
    case 88: return "Programming error";
    case 89: return std::strerror(EBADMSG);
    case 90: return std::strerror(EMULTIHOP);
    case 91: return std::strerror(ENOLINK);
    case 92: return std::strerror(EPROTO);
    case 93: return "Insufficient capabilities";
    case 94: return "Operation not permitted in capability mode";
    case 95: return std::strerror(ENOTRECOVERABLE);
    case 96: return std::strerror(EOWNERDEAD);
    default: return "Unknown error: " + std::to_string(error);
    }
}

}

struct NativePosix::Impl {
    struct Directory {
        DIR* native;
        std::uint64_t buffer;
    };
    struct Environment {
        std::string value;
        std::uint64_t alias = 0;
        std::uint64_t pointer = 0;
    };
    struct Binding { NativePosix* owner; Operation operation; };
    NativeLibc& libc;
    std::recursive_mutex mutex;
    std::mutex sleepMutex;
    std::condition_variable_any sleepCondition;
    std::stop_source stop;
    std::unordered_set<int> descriptors;
    std::unordered_map<std::uint64_t, Directory> directories;
    std::unordered_map<std::string, Environment> environment;
    std::unordered_map<std::thread::id, std::uint64_t> errors, tokens;
    std::array<Binding, exports.size()> bindings{};
    std::uint64_t nextDirectory = 0x780100000;
    std::uint64_t argv = 0;
    int argc = 0;
    std::uint32_t randomState = 1;
    Impl(NativeLibc& libc) : libc(libc) {
        for (char** variable = *_NSGetEnviron(); variable && *variable; ++variable) {
            const std::string entry(*variable);
            const auto separator = entry.find('=');
            if (separator != std::string::npos) environment.insert_or_assign(entry.substr(0, separator), Environment{entry.substr(separator + 1)});
        }
    }
    ~Impl() {
        for (const auto& [handle, directory] : directories) ::closedir(directory.native);
        for (const auto descriptor : descriptors) ::close(descriptor);
    }
    std::uint64_t fail(GuestMemory& memory, int error, std::uint64_t result = integer(-1)) {
        libc.SetError(memory, error);
        return result;
    }
    std::uint64_t hostResult(GuestMemory& memory, std::int64_t value) {
        if (value < 0) return fail(memory, guestError(errno));
        return integer(value);
    }
    bool descriptor(int value) const {
        return descriptors.contains(value) || libc.HasDescriptor(value);
    }
    int nativeDescriptor(int value) const {
        return descriptors.contains(value) ? value : libc.NativeDescriptor(value);
    }
    std::uint64_t copy(GuestMemory& memory, const std::string& value) {
        const auto address = libc.Allocate(memory, value.size() + 1);
        if (address != 0) writeString(memory, address, value);
        return address;
    }
    void refreshEnvironment(const GuestMemory& memory) {
        std::vector<std::pair<std::string, Environment>> renamed;
        for (auto entry = environment.begin(); entry != environment.end();) {
            if (entry->second.alias == 0) { ++entry; continue; }
            const auto shared = string(memory, entry->second.alias);
            const auto separator = shared.find('=');
            if (separator == std::string::npos || separator == 0) { entry = environment.erase(entry); continue; }
            const auto name = shared.substr(0, separator);
            if (name == entry->first) { ++entry; continue; }
            renamed.emplace_back(name, entry->second);
            entry = environment.erase(entry);
        }
        for (auto& [name, value] : renamed) environment.insert_or_assign(std::move(name), std::move(value));
    }
};

NativePosix::NativePosix(NativeLibc& libc) : impl(std::make_unique<Impl>(libc)) {
    for (std::size_t i = 0; i < exports.size(); ++i) impl->bindings[i] = {this, exports[i].operation};
}
NativePosix::~NativePosix() = default;

void NativePosix::Shutdown() { impl->stop.request_stop(); }

std::optional<std::string> NativePosix::GetEnvironment(GuestMemory& memory, std::string_view name) {
    const auto streams = impl->libc.LockStreams();
    std::lock_guard lock(impl->mutex);
    impl->refreshEnvironment(memory);
    const auto found = impl->environment.find(std::string(name));
    if (found == impl->environment.end()) return std::nullopt;
    const auto& entry = found->second;
    if (entry.alias != 0) return string(memory, entry.alias).substr(name.size() + 1);
    if (entry.pointer != 0) return string(memory, entry.pointer);
    return entry.value;
}

std::uint64_t NativePosix::Resolve(const ElfImport& symbol) {
    if (symbol.type != 0 && symbol.type != 2) return 0;
    for (std::size_t i = 0; i < exports.size(); ++i) if (exports[i].name == symbol.name) return functionBase + i * 16;
    return 0;
}

void NativePosix::Bind(Cpu& cpu) {
    for (std::size_t i = 0; i < exports.size(); ++i) cpu.Bind(functionBase + i * 16, +[](const HostCall& call, void* context) {
        const auto& binding = *static_cast<Impl::Binding*>(context);
        return binding.owner->invoke(call, static_cast<std::size_t>(binding.operation));
    }, &impl->bindings[i]);
}

void NativePosix::SetArguments(GuestMemory& memory, const std::vector<std::string>& arguments) {
    if (arguments.size() > INT_MAX) throw std::runtime_error("Native POSIX argument count exceeds INT_MAX");
    const auto streams = impl->libc.LockStreams();
    std::lock_guard lock(impl->mutex);
    if (impl->argv != 0) throw std::runtime_error("Native POSIX process arguments were already set");
    std::vector<std::uint64_t> pointers;
    pointers.reserve(arguments.size() + 1);
    try {
        for (const auto& argument : arguments) {
            if (argument.find('\0') != std::string::npos) throw std::runtime_error("Native process argument contains NUL");
            const auto address = impl->copy(memory, argument);
            if (address == 0) throw std::bad_alloc();
            pointers.push_back(address);
        }
        pointers.push_back(0);
        const auto address = impl->libc.Allocate(memory, pointers.size() * 8);
        if (address == 0) throw std::bad_alloc();
        if (!memory.TryWrite(address, std::as_bytes(std::span(pointers)))) { impl->libc.Free(memory, address); throw std::runtime_error("Native process argument write failed"); }
        impl->argv = address;
        impl->argc = static_cast<int>(arguments.size());
    } catch (...) {
        for (const auto pointer : pointers) impl->libc.Free(memory, pointer);
        throw;
    }
}

std::uint64_t NativePosix::invoke(const HostCall& call, std::size_t operation) {
    const auto op = static_cast<Operation>(operation);
    const auto& args = call.arguments;
    auto& memory = call.memory;
    if (op == Operation::Nanosleep || op == Operation::Sleep) {
        auto time = op == Operation::Sleep ? Timespec{static_cast<unsigned>(args[0]), 0} : read<Timespec>(memory, args[0]);
        if (time.seconds < 0 || time.nanoseconds < 0 || time.nanoseconds >= 1000000000) return impl->fail(memory, 22);
        if (op == Operation::Nanosleep && args[1] != 0 && !memory.Contains(args[1], 16, Write)) return impl->fail(memory, 14);
        std::unique_lock lock(impl->sleepMutex);
        while (time.seconds != 0 || time.nanoseconds != 0) {
            const auto seconds = std::min<std::int64_t>(time.seconds, 60);
            const auto nanoseconds = time.seconds > 60 ? 0 : time.nanoseconds;
            impl->sleepCondition.wait_for(lock, impl->stop.get_token(), std::chrono::seconds(seconds) + std::chrono::nanoseconds(nanoseconds), [] { return false; });
            if (impl->stop.stop_requested()) throw ExecutionStopped();
            time.seconds -= seconds;
            time.nanoseconds -= nanoseconds;
        }
        return 0;
    }
    if (op == Operation::Qsort || op == Operation::Bsearch) {
        const auto base = args[op == Operation::Qsort ? 0 : 1];
        const auto count = args[op == Operation::Qsort ? 1 : 2], size = args[op == Operation::Qsort ? 2 : 3];
        const auto comparator = args[op == Operation::Qsort ? 3 : 4];
        if (count == 0 || (op == Operation::Qsort && count == 1)) return 0;
        if (size == 0 || comparator == 0 || count > std::numeric_limits<std::size_t>::max() / size || !memory.Contains(base, count * size, op == Operation::Qsort ? Read | Write : Read)) throw std::runtime_error("Native guest sort/search array is invalid");
        if (op == Operation::Bsearch) {
            std::uint64_t first = 0, last = count;
            while (first < last) {
                const auto middle = first + (last - first) / 2;
                const auto element = base + middle * size;
                const auto order = static_cast<std::int32_t>(call.cpu.Invoke(call.state, comparator, {args[0], element}));
                if (order == 0) return element;
                if (order < 0) last = middle;
                else first = middle + 1;
            }
            return 0;
        }
        std::vector<std::size_t> indices(count);
        std::iota(indices.begin(), indices.end(), 0);
        std::sort(indices.begin(), indices.end(), [&](auto left, auto right) { return static_cast<std::int32_t>(call.cpu.Invoke(call.state, comparator, {base + left * size, base + right * size})) < 0; });
        std::vector<std::byte> sorted(count * size);
        for (std::size_t i = 0; i < count; ++i) if (!memory.TryRead(base + indices[i] * size, std::span(sorted).subspan(i * size, size))) throw std::runtime_error("Native guest sort read failed");
        if (!memory.TryWrite(base, sorted)) throw std::runtime_error("Native guest sort write failed");
        return 0;
    }
    const auto streams = impl->libc.LockStreams();
    std::lock_guard lock(impl->mutex);
    if (op == Operation::Getenv || op == Operation::Setenv || op == Operation::Unsetenv || op == Operation::Putenv) impl->refreshEnvironment(memory);
    switch (op) {
    case Operation::Open: {
        const auto path = string(memory, args[0]);
        const auto flags = static_cast<std::uint32_t>(args[1]);
        if ((flags & ~0x21e8fu) != 0 || (flags & 3) == 3) return impl->fail(memory, 22);
        int nativeFlags = static_cast<int>(flags & 3);
        const std::array<std::pair<unsigned, int>, 8> translations{{{4, O_NONBLOCK}, {8, O_APPEND}, {0x80, O_SYNC}, {0x200, O_CREAT}, {0x400, O_TRUNC}, {0x800, O_EXCL}, {0x1000, O_DSYNC}, {0x20000, O_DIRECTORY}}};
        for (const auto& [guest, native] : translations) if ((flags & guest) != 0) nativeFlags |= native;
        const auto descriptor = ::open(path.c_str(), nativeFlags, static_cast<mode_t>(args[2]));
        if (descriptor < 0) return impl->hostResult(memory, descriptor);
        try { impl->descriptors.insert(descriptor); } catch (...) { ::close(descriptor); throw; }
        return integer(descriptor);
    }
    case Operation::Close: {
        const auto descriptor = static_cast<int>(args[0]);
        if (!impl->descriptor(descriptor)) return impl->fail(memory, 9);
        const auto result = impl->descriptors.contains(descriptor) ? ::close(descriptor) : impl->libc.CloseDescriptor(descriptor);
        impl->descriptors.erase(descriptor);
        return impl->hostResult(memory, result);
    }
    case Operation::Read:
    case Operation::Write: {
        const auto descriptor = static_cast<int>(args[0]);
        if (!impl->descriptor(descriptor)) return impl->fail(memory, 9);
        if (args[2] > static_cast<std::uint64_t>(SSIZE_MAX)) return impl->fail(memory, 22);
        if (!memory.Contains(args[1], args[2], op == Operation::Read ? Write : Read)) return impl->fail(memory, 14);
        std::vector<std::byte> bytes(args[2]);
        if (op == Operation::Write && !memory.TryRead(args[1], bytes)) return impl->fail(memory, 14);
        const auto native = impl->nativeDescriptor(descriptor);
        const auto result = op == Operation::Read ? ::read(native, bytes.data(), bytes.size()) : ::write(native, bytes.data(), bytes.size());
        if (result < 0) return impl->hostResult(memory, result);
        if (op == Operation::Read && !memory.TryWrite(args[1], std::span(bytes).first(result))) return impl->fail(memory, 14);
        return integer(result);
    }
    case Operation::Seek: {
        const auto descriptor = static_cast<int>(args[0]);
        if (!impl->descriptor(descriptor)) return impl->fail(memory, 9);
        return impl->hostResult(memory, ::lseek(impl->nativeDescriptor(descriptor), std::bit_cast<std::int64_t>(args[1]), static_cast<int>(args[2])));
    }
    case Operation::Stat:
    case Operation::Fstat: {
        if (!memory.Contains(args[1], sizeof(Stat), Write)) return impl->fail(memory, 14);
        struct stat status{};
        if (op == Operation::Fstat && !impl->descriptor(static_cast<int>(args[0]))) return impl->fail(memory, 9);
        const auto result = op == Operation::Stat ? ::stat(string(memory, args[0]).c_str(), &status) : ::fstat(impl->nativeDescriptor(static_cast<int>(args[0])), &status);
        if (result != 0) return impl->hostResult(memory, result);
        write(memory, args[1], convert(status));
        return 0;
    }
    case Operation::Mkdir: return impl->hostResult(memory, ::mkdir(string(memory, args[0]).c_str(), static_cast<mode_t>(args[1])));
    case Operation::Unlink: return impl->hostResult(memory, ::unlink(string(memory, args[0]).c_str()));
    case Operation::Remove: return impl->hostResult(memory, ::remove(string(memory, args[0]).c_str()));
    case Operation::Rename: return impl->hostResult(memory, ::rename(string(memory, args[0]).c_str(), string(memory, args[1]).c_str()));
    case Operation::OpenDir: {
        DIR* directory = ::opendir(string(memory, args[0]).c_str());
        if (!directory) return impl->fail(memory, guestError(errno), 0);
        const auto buffer = impl->libc.Allocate(memory, sizeof(Dirent));
        if (buffer == 0) { ::closedir(directory); return 0; }
        const auto handle = impl->nextDirectory++;
        try { impl->directories.emplace(handle, Impl::Directory{directory, buffer}); }
        catch (...) { ::closedir(directory); impl->libc.Free(memory, buffer); throw; }
        return handle;
    }
    case Operation::CloseDir: {
        const auto found = impl->directories.find(args[0]);
        if (found == impl->directories.end()) return impl->fail(memory, 9);
        const auto result = ::closedir(found->second.native);
        const auto error = errno;
        impl->libc.Free(memory, found->second.buffer);
        impl->directories.erase(found);
        return result != 0 ? impl->fail(memory, guestError(error)) : 0;
    }
    case Operation::ReadDir: {
        const auto found = impl->directories.find(args[0]);
        if (found == impl->directories.end()) return impl->fail(memory, 9, 0);
        errno = 0;
        const auto* entry = ::readdir(found->second.native);
        if (!entry) { if (errno != 0) return impl->fail(memory, guestError(errno), 0); return 0; }
        const auto size = std::strlen(entry->d_name);
        if (size > 255) return impl->fail(memory, 63, 0);
        Dirent converted{static_cast<std::uint32_t>(entry->d_ino), static_cast<std::uint16_t>(8 + ((size + 4) & ~std::size_t{3})), entry->d_type, static_cast<std::uint8_t>(size), {}};
        std::memcpy(converted.name.data(), entry->d_name, size);
        write(memory, found->second.buffer, converted);
        return found->second.buffer;
    }
    case Operation::Cwd: {
        char* directory = ::getcwd(nullptr, 0);
        if (!directory) return impl->fail(memory, guestError(errno), 0);
        const std::string value(directory);
        std::free(directory);
        auto address = args[0];
        if (address != 0) {
            if (args[1] == 0) return impl->fail(memory, 22, 0);
            if (args[1] <= value.size()) return impl->fail(memory, 34, 0);
            if (!memory.Contains(address, value.size() + 1, Write)) return impl->fail(memory, 14, 0);
        } else {
            if (args[1] != 0 && args[1] <= value.size()) return impl->fail(memory, 34, 0);
            address = impl->libc.Allocate(memory, args[1] == 0 ? value.size() + 1 : args[1]);
            if (address == 0) return 0;
        }
        writeString(memory, address, value);
        return address;
    }
    case Operation::Timeval: {
        if ((args[0] != 0 && !memory.Contains(args[0], 16, Write)) || (args[1] != 0 && !memory.Contains(args[1], 8, Write))) return impl->fail(memory, 14);
        struct timeval time{};
        struct timezone zone{};
        if (::gettimeofday(&time, args[1] == 0 ? nullptr : &zone) != 0) return impl->hostResult(memory, -1);
        if (args[0] != 0) write(memory, args[0], Timeval{time.tv_sec, time.tv_usec});
        if (args[1] != 0) write(memory, args[1], std::array<std::int32_t, 2>{zone.tz_minuteswest, zone.tz_dsttime});
        return 0;
    }
    case Operation::Time: {
        if (args[0] != 0 && !memory.Contains(args[0], 8, Write)) return impl->fail(memory, 14);
        const auto value = static_cast<std::int64_t>(::time(nullptr));
        if (value < 0) return impl->hostResult(memory, value);
        if (args[0] != 0) write(memory, args[0], value);
        return integer(value);
    }
    case Operation::Clock: {
        if (!memory.Contains(args[1], 16, Write)) return impl->fail(memory, 14);
        clockid_t native;
        switch (static_cast<int>(args[0])) {
        case 0: native = CLOCK_REALTIME; break;
        case 4: case 11: native = CLOCK_MONOTONIC; break;
        case 14: native = CLOCK_THREAD_CPUTIME_ID; break;
        case 15: native = CLOCK_PROCESS_CPUTIME_ID; break;
        default: return impl->fail(memory, 22);
        }
        struct timespec time{};
        if (::clock_gettime(native, &time) != 0) return impl->hostResult(memory, -1);
        write(memory, args[1], Timespec{time.tv_sec, time.tv_nsec});
        return 0;
    }
    case Operation::Getenv: {
        const auto name = string(memory, args[0]);
        if (!validName(name)) return 0;
        const auto found = impl->environment.find(name);
        if (found == impl->environment.end()) return 0;
        auto& entry = found->second;
        if (entry.alias != 0) {
            const auto shared = string(memory, entry.alias);
            const auto separator = shared.find('=');
            if (separator == std::string::npos || shared.substr(0, separator) != name) return 0;
            return entry.alias + separator + 1;
        }
        if (entry.pointer == 0) entry.pointer = impl->copy(memory, entry.value);
        return entry.pointer;
    }
    case Operation::Setenv:
    case Operation::Unsetenv: {
        const auto name = string(memory, args[0]);
        if (!validName(name)) return impl->fail(memory, 22);
        if (op == Operation::Unsetenv) { impl->environment.erase(name); return 0; }
        if (args[1] == 0) return impl->fail(memory, 22);
        if (args[2] == 0 && impl->environment.contains(name)) return 0;
        impl->environment.insert_or_assign(name, Impl::Environment{string(memory, args[1])});
        return 0;
    }
    case Operation::Putenv: {
        const auto shared = string(memory, args[0]);
        const auto separator = shared.find('=');
        if (separator == std::string::npos || separator == 0) return impl->fail(memory, 22);
        impl->environment.insert_or_assign(shared.substr(0, separator), Impl::Environment{{}, args[0], 0});
        return 0;
    }
    case Operation::Strerror: {
        const auto error = static_cast<int>(args[0]);
        auto& address = impl->errors[std::this_thread::get_id()];
        if (address == 0) address = impl->libc.Allocate(memory, 256);
        if (address == 0) return 0;
        writeString(memory, address, errorText(error));
        if (error < 0 || error > 96) impl->libc.SetError(memory, 22);
        return address;
    }
    case Operation::Perror: {
        const auto error = read<int>(memory, impl->libc.ErrorAddress(memory));
        const auto prefix = args[0] == 0 ? std::string{} : string(memory, args[0]);
        if (prefix.empty()) std::fprintf(stderr, "%s\n", errorText(error).c_str());
        else std::fprintf(stderr, "%s: %s\n", prefix.c_str(), errorText(error).c_str());
        return 0;
    }
    case Operation::Sysconf: {
        const auto name = static_cast<int>(args[0]);
        if (name == 47) return 0x4000;
        if (name == 57 || name == 58) return impl->hostResult(memory, ::sysconf(name == 57 ? _SC_NPROCESSORS_CONF : _SC_NPROCESSORS_ONLN));
        if (name == 121) {
            std::uint64_t capacity = 0;
            std::size_t size = sizeof(capacity);
            if (::sysctlbyname("hw.memsize", &capacity, &size, nullptr, 0) != 0) return impl->hostResult(memory, -1);
            return capacity / 0x4000;
        }
        return impl->fail(memory, 22);
    }
    case Operation::Getargv: {
        if (impl->argv == 0) throw std::runtime_error("Native process arguments were not initialized");
        return impl->argv;
    }
    case Operation::Getargc: return integer(impl->argc);
    case Operation::Strtok:
    case Operation::StrtokR: {
        const auto delimiters = string(memory, args[1]);
        if (op == Operation::StrtokR && !memory.Contains(args[2], 8, Read | Write)) throw std::runtime_error("Native strtok_r state is outside writable guest memory");
        auto& local = impl->tokens[std::this_thread::get_id()];
        auto address = args[0] != 0 ? args[0] : op == Operation::Strtok ? local : read<std::uint64_t>(memory, args[2]);
        const auto save = [&](std::uint64_t next) { if (op == Operation::Strtok) local = next; else write(memory, args[2], next); };
        if (address == 0) return 0;
        auto text = string(memory, address);
        const auto first = text.find_first_not_of(delimiters);
        if (first == std::string::npos) { save(address + text.size()); return 0; }
        const auto last = text.find_first_of(delimiters, first);
        if (last == std::string::npos) save(address + text.size());
        else { write<char>(memory, address + last, 0); save(address + last + 1); }
        return address + first;
    }
    case Operation::Srand: impl->randomState = static_cast<std::uint32_t>(args[0]); return 0;
    case Operation::Rand: {
        const auto x = static_cast<std::int64_t>(impl->randomState % 0x7ffffffeu) + 1;
        auto next = 16807 * (x % 127773) - 2836 * (x / 127773);
        if (next < 0) next += 0x7fffffff;
        impl->randomState = static_cast<std::uint32_t>(next - 1);
        return impl->randomState;
    }
    default: throw std::runtime_error("Invalid native POSIX operation");
    }
}

}
