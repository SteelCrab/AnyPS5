#pragma once

#include "NativeLibc.hpp"

namespace AnyPS5::Arm64 {

enum class ScanInput { String, File, Standard };
[[nodiscard]] std::uint64_t scanInput(const HostCall& call, NativeLibc& libc, ScanInput input, bool vaList);

}
