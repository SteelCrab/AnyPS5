#pragma once

#include "NativeLibc.hpp"

namespace AnyPS5::Arm64 {

enum class FormatOutput { Standard, File, Buffer, BoundedBuffer, AllocatedBuffer };
[[nodiscard]] std::uint64_t formatOutput(const HostCall& call, NativeLibc& libc, FormatOutput output, bool vaList);

}
