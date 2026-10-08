#include "NativeMath.hpp"
#include <array>
#include <cmath>
#include <cfenv>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>

using namespace AnyPS5::Arm64;

namespace {

void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }

struct Fixture {
    GuestMemory memory;
    NativeLibc libc;
    NativeMath math{libc};
    Cpu cpu{memory};
    Fixture() {
        memory.Map(0x8000, 0x1000, Read | Write);
        libc.Bind(cpu, memory);
        math.Bind(cpu);
    }
    template<class TFloat>
    std::pair<TFloat, std::uint64_t> Call(std::string_view name, TFloat left, TFloat right = 0, int integer = 0) {
        CpuState state;
        state.rip = math.Resolve({name, 2, 1, 0});
        require(state.rip != 0, "Native math import was not resolved");
        state.registers[4] = 0x8ff8;
        state.registers[7] = static_cast<std::uint64_t>(integer);
        std::memcpy(state.xmm[0].data(), &left, sizeof(left));
        std::memcpy(state.xmm[1].data(), &right, sizeof(right));
        cpu.Run(state);
        TFloat result;
        std::memcpy(&result, state.xmm[0].data(), sizeof(result));
        return {result, state.registers[0]};
    }
};

void testNativeMathAbi() {
    Fixture fixture;
    require(fixture.math.Resolve({"pow", 1, 1, 8}) == 0 && fixture.math.Resolve({"unsupported_math", 2, 1, 0}) == 0, "Invalid math import was falsely resolved");
    const std::array<std::pair<const char*, double>, 6> zeros{{{"sin", 0}, {"tan", 0}, {"asin", 0}, {"atan", 0}, {"log", 1}, {"log10", 1}}};
    for (const auto& [name, input] : zeros) require(fixture.Call(name, input).first == 0, "Unary double math did not return through XMM0");
    require(fixture.Call("cos", 0.0).first == 1 && fixture.Call("exp", 0.0).first == 1 && fixture.Call("exp2", -3.0).first == 0.125, "Unary double math identity changed");
    require(fixture.Call("sinf", -0.0f).first == 0 && std::signbit(fixture.Call("sinf", -0.0f).first), "Float math lost signed zero");
    require(fixture.Call("cosf", 0.0f).first == 1 && fixture.Call("powf", 2.0f, 5.0f).first == 32, "Float register argument or return ABI changed");
    require(fixture.Call("pow", 2.0, 5.0).first == 32 && fixture.Call("fmod", 17.0, 5.0).first == 2 && fixture.Call("atan2", 0.0, 1.0).first == 0, "Binary double math did not read XMM1");
    require(fixture.Call("scalbn", 1.5, 0.0, 3).first == 12 && fixture.Call("scalbnf", 1.5f, 0.0f, -1).first == 0.75f, "Mixed floating/integer math argument ABI changed");
    require(static_cast<std::int64_t>(fixture.Call("lround", -2.5).second) == -3 && fixture.Call("lroundf", 2.5f).second == 3, "Integer math return did not use RAX or half-away rounding");
    require(fixture.Call("floor", -2.5).first == -3 && fixture.Call("ceil", -2.5).first == -2 && fixture.Call("trunc", -2.5).first == -2 && fixture.Call("round", -2.5).first == -3, "Native rounding functions changed their defined behavior");
    require(fixture.Call("fabs", -3.5).first == 3.5 && fixture.Call("sqrt", 16.0).first == 4, "Native magnitude or square root changed");
    require(std::isnan(fixture.Call("acos", 2.0).first), "Native math domain failure did not return NaN");
    const auto address = fixture.libc.ErrorAddress(fixture.memory);
    int error = 0;
    require(fixture.memory.TryRead(address, std::as_writable_bytes(std::span(&error, 1))) && error == 33, "Native math domain failure did not set guest EDOM");
    require(std::isinf(fixture.Call("exp", 10000.0).first), "Native math overflow did not return infinity");
    require(fixture.memory.TryRead(address, std::as_writable_bytes(std::span(&error, 1))) && error == 34, "Native math overflow did not set guest ERANGE");
    require(std::isnan(fixture.Call("sin", std::numeric_limits<double>::quiet_NaN()).first), "Native math lost a NaN input");
    require(std::feclearexcept(FE_ALL_EXCEPT) == 0 && std::feraiseexcept(FE_INVALID) == 0, "Could not initialize floating environment regression");
    fixture.libc.SetError(fixture.memory, 77);
    require(fixture.Call("sin", 0.0).first == 0 && fixture.memory.TryRead(address, std::as_writable_bytes(std::span(&error, 1))) && error == 77, "Existing floating exception incorrectly changed guest errno");
    require((std::fetestexcept(FE_INVALID) & FE_INVALID) != 0, "Native math discarded the caller's floating exception flags");
    std::feclearexcept(FE_ALL_EXCEPT);
}

}

int main() {
    try { testNativeMathAbi(); std::puts("Native float/double math ABI, mixed arguments, integer returns and domain/range errors passed"); return 0; }
    catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
