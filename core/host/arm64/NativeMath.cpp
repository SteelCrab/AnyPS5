#include "NativeMath.hpp"
#include <array>
#include <cerrno>
#include <cmath>
#include <cfenv>
#include <cstring>
#include <type_traits>
#include <stdexcept>

namespace AnyPS5::Arm64 {

namespace {

constexpr std::uint64_t functionBase = 0x770000000;

template<class TFloat>
TFloat argument(const HostCall& call, std::size_t index) {
    TFloat value;
    std::memcpy(&value, call.state.xmm[index].data(), sizeof(value));
    return value;
}

template<class TFloat, auto TFunction, int TArguments = 1>
std::uint64_t compute(const HostCall& call, void* context) {
    std::fenv_t environment;
    if (std::feholdexcept(&environment) != 0) throw std::runtime_error("Cannot preserve native math floating-point environment");
    errno = 0;
    const auto value = [&] {
        if constexpr (TArguments == 1) return TFunction(argument<TFloat>(call, 0));
        else if constexpr (TArguments == 2) return TFunction(argument<TFloat>(call, 0), argument<TFloat>(call, 1));
        else return TFunction(argument<TFloat>(call, 0), static_cast<std::int32_t>(call.arguments[0]));
    }();
    auto error = errno;
    const auto raised = std::fetestexcept(FE_INVALID | FE_DIVBYZERO | FE_OVERFLOW | FE_UNDERFLOW);
    if (std::feupdateenv(&environment) != 0) throw std::runtime_error("Cannot restore native math floating-point environment");
    if (error == 0) {
        if ((raised & FE_INVALID) != 0) error = 33;
        else if (raised != 0) error = 34;
    }
    if (error != 0) static_cast<NativeLibc*>(context)->SetError(call.memory, error);
    if constexpr (std::is_floating_point_v<decltype(value)>) {
        std::memcpy(call.state.xmm[0].data(), &value, sizeof(value));
        return 0;
    } else return static_cast<std::uint64_t>(value);
}

struct Function { std::string_view name; HostFunction function; };
const auto functions = std::to_array<Function>({
    {"acos", compute<double, static_cast<double(*)(double)>(std::acos)>},
    {"acosf", compute<float, static_cast<float(*)(float)>(std::acos)>},
    {"asin", compute<double, static_cast<double(*)(double)>(std::asin)>},
    {"asinf", compute<float, static_cast<float(*)(float)>(std::asin)>},
    {"atan", compute<double, static_cast<double(*)(double)>(std::atan)>},
    {"atanf", compute<float, static_cast<float(*)(float)>(std::atan)>},
    {"cos", compute<double, static_cast<double(*)(double)>(std::cos)>},
    {"cosf", compute<float, static_cast<float(*)(float)>(std::cos)>},
    {"sin", compute<double, static_cast<double(*)(double)>(std::sin)>},
    {"sinf", compute<float, static_cast<float(*)(float)>(std::sin)>},
    {"tan", compute<double, static_cast<double(*)(double)>(std::tan)>},
    {"tanf", compute<float, static_cast<float(*)(float)>(std::tan)>},
    {"exp", compute<double, static_cast<double(*)(double)>(std::exp)>},
    {"expf", compute<float, static_cast<float(*)(float)>(std::exp)>},
    {"exp2", compute<double, static_cast<double(*)(double)>(std::exp2)>},
    {"exp2f", compute<float, static_cast<float(*)(float)>(std::exp2)>},
    {"log", compute<double, static_cast<double(*)(double)>(std::log)>},
    {"logf", compute<float, static_cast<float(*)(float)>(std::log)>},
    {"log10", compute<double, static_cast<double(*)(double)>(std::log10)>},
    {"log10f", compute<float, static_cast<float(*)(float)>(std::log10)>},
    {"sqrt", compute<double, static_cast<double(*)(double)>(std::sqrt)>},
    {"sqrtf", compute<float, static_cast<float(*)(float)>(std::sqrt)>},
    {"floor", compute<double, static_cast<double(*)(double)>(std::floor)>},
    {"floorf", compute<float, static_cast<float(*)(float)>(std::floor)>},
    {"ceil", compute<double, static_cast<double(*)(double)>(std::ceil)>},
    {"ceilf", compute<float, static_cast<float(*)(float)>(std::ceil)>},
    {"trunc", compute<double, static_cast<double(*)(double)>(std::trunc)>},
    {"truncf", compute<float, static_cast<float(*)(float)>(std::trunc)>},
    {"round", compute<double, static_cast<double(*)(double)>(std::round)>},
    {"roundf", compute<float, static_cast<float(*)(float)>(std::round)>},
    {"fabs", compute<double, static_cast<double(*)(double)>(std::fabs)>},
    {"fabsf", compute<float, static_cast<float(*)(float)>(std::fabs)>},
    {"lround", compute<double, static_cast<long(*)(double)>(std::lround)>},
    {"lroundf", compute<float, static_cast<long(*)(float)>(std::lround)>},
    {"llround", compute<double, static_cast<long long(*)(double)>(std::llround)>},
    {"llroundf", compute<float, static_cast<long long(*)(float)>(std::llround)>},
    {"atan2", compute<double, static_cast<double(*)(double, double)>(std::atan2), 2>},
    {"atan2f", compute<float, static_cast<float(*)(float, float)>(std::atan2), 2>},
    {"fmod", compute<double, static_cast<double(*)(double, double)>(std::fmod), 2>},
    {"fmodf", compute<float, static_cast<float(*)(float, float)>(std::fmod), 2>},
    {"pow", compute<double, static_cast<double(*)(double, double)>(std::pow), 2>},
    {"powf", compute<float, static_cast<float(*)(float, float)>(std::pow), 2>},
    {"scalbn", compute<double, static_cast<double(*)(double, int)>(std::scalbn), 3>},
    {"scalbnf", compute<float, static_cast<float(*)(float, int)>(std::scalbn), 3>}
});

}

std::uint64_t NativeMath::Resolve(const ElfImport& symbol) {
    if (symbol.type != 0 && symbol.type != 2) return 0;
    for (std::size_t i = 0; i < functions.size(); ++i) if (functions[i].name == symbol.name) return functionBase + i * 16;
    return 0;
}

void NativeMath::Bind(Cpu& cpu) {
    for (std::size_t i = 0; i < functions.size(); ++i) cpu.Bind(functionBase + i * 16, functions[i].function, &libc);
}

}
