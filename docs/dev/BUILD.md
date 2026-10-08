# Build

```sh
git submodule update --init --recursive
```

## Requirements

- Git, CMake 3.22.1 or newer, C++20. The Linux/Windows guest runtime requires x86-64.
- macOS: macOS 15 or newer, Xcode Command Line Tools and CMake. Apple Silicon builds ARM64 host tools and the direct Metal backend with Apple Clang.
- Linux: GCC, G++, binutils. SDL's X11 backend requires X11 and Xext development headers (`libx11-dev` and `libxext-dev` on Debian/Ubuntu).
- Windows: only MinGW-w64 GCC 15.2.0 (WinLibs `x86_64-ucrt-posix-seh`, release `15.2.0posix-14.0.0-ucrt-r7`) is currently supported. Add its `mingw64/bin` directory to `PATH` before configuring.
- FFmpeg binaries are downloaded during configuration unless `FFMPEG_PREBUILT_DIR` is set. With the WinLibs CMake, the download fails with status 60 (`SSL peer certificate or SSH remote key was not OK`) unless `SSL_CERT_FILE` names a CA bundle, for example `C:\Program Files\Git\mingw64\etc\ssl\certs\ca-bundle.crt` from Git for Windows, as in CI.

## Commands

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++
cmake --build build --parallel
cmake --build build --target libs --parallel
```

`libs` is a custom target: every library under [core/libs/prx](../../core/libs/prx) is built with
`EXCLUDE_FROM_ALL`, so the first command alone does not produce them. Titles load the patched `.prx`
files from `build/core/libs/libs`, which only that second step refreshes. Running a title after a
library change without it therefore tests the previous binaries and can show no effect at all.

[Relinker usage and runtime layout](../user/USAGE.md).

## macOS and Apple Silicon

```sh
git submodule update --init --recursive
cmake -S . -B build-macos -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 -DBUILD_TESTING=ON -DANYPS5_ENABLE_SPIRV_TOOLS=ON
cmake --build build-macos --parallel
ctest --test-dir build-macos --output-on-failure
```

macOS defaults to `ANYPS5_BUILD_RUNTIME=OFF`. This builds native `relinker`, `nid_patcher`,
`agc_shader_replay`, `agc_metal_shader`, the shader recompiler, the direct Metal backend,
`anyps5_arm64_run`, the experimental CPU translator, and PNG/JPEG decoders. Relinker produces Linux
ELF or Windows PE output. It does not produce a macOS executable. Guest runtime libraries
and the `libs` target are disabled in this build.

Linux/Windows can also select `-DANYPS5_BUILD_RUNTIME=OFF` to build portable host tools.
The native CPU runner and Metal backend are macOS-only targets.
macOS cannot select `ON`: the runtime uses x86-64 guest code and Linux/Windows loader APIs.

macOS targets ARM64 only. An x86-64 or universal target is rejected. The Metal backend links
Apple's Metal and Foundation frameworks. SPIRV-Cross is built from its pinned submodule and
only generates MSL source; there is no Vulkan loader or MoltenVK in this backend.

`metal_shader` tests translation and resource reflection without a GPU. `metal_gpu` requires
an Apple Silicon GPU and checks compute results, shared-buffer synchronization, texture upload,
image writes, nearest/linear filtering, texture addressing, vertex/fragment resource binding,
rendered pixels, and error handling.
A missing GPU fails this test. On a machine without GPU access,
`ctest --test-dir build-macos -LE gpu` explicitly excludes it. CI excludes GPU tests;
hardware validation must run separately on an Apple Silicon Mac.

The [Metal shader tool](../user/USAGE.md#mac-apple-silicon) accepts SPIR-V or captured RDNA
requests and validates the generated MSL with the native Metal compiler.

The CPU runner uses Apple Silicon JIT memory and executes translated ARM64 blocks in the
same process. `arm64_cpu` checks integer instructions, flags, control flow, stack calls,
memory faults, native integer callbacks, and static ELF loading. `arm64_metal` checks an
x86-64 guest calls through the ARM64 callback bridge to Metal compute and RDNA rendering,
with results read back by the guest. It requires GPU access and has the `gpu` label.

`arm64_integer` checks narrow registers, arithmetic flags, shifts, rotates, conditional writes,
stack faults and segmented addresses. `arm64_wide_integer` compares MUL/IMUL/DIV/IDIV with
an arbitrary-precision reference. `arm64_vector` compares native SSE/SSE2-to-NEON operations
with scalar lanes and checks vector memory faults.
`arm64_compiled_kernel` builds the same C source for native ARM64 and x86-64 ELF at O0/O2/O3,
then compares execution results, including compiler-generated SSE2. It needs an ELF LLD tool
(`ld.lld` or `rust-lld`); install it with `brew install lld` or supply
`-DANYPS5_ELF_LINKER=/absolute/path/to/ld.lld`. This test is omitted with a configure message
when LLD is unavailable. LLD is only a test build tool; the runtime does not link to it.

CPU executables are ad-hoc signed with hardened runtime and the
[`allow-jit` entitlement](https://developer.apple.com/documentation/bundleresources/entitlements/com.apple.security.cs.allow-jit)
by default. This uses local `codesign` without a signing certificate. Set
`-DANYPS5_MACOS_HARDENED_JIT=OFF` for an ordinary development build. Distribution signing and
notarization are not configured.

The full CPU instruction set, dynamic guest loader, system PRX ABI, and AGC command submission
port remain unimplemented. Building these components does not enable game execution on macOS.

## CMake flags

Project switches accept `ON` or `OFF`:

| Flag                             | Default | Effect                                           |
|----------------------------------|---------|--------------------------------------------------|
| `-DBUILD_TESTING=ON`             | `OFF`   | Build and register tests.                        |
| `-DANYPS5_BUILD_RUNTIME=ON`     | `OFF` on macOS, `ON` elsewhere | Build Linux/Windows x86-64 runtime libraries. |
| `-DANYPS5_MACOS_HARDENED_JIT=ON` | `ON` on macOS | Ad-hoc sign CPU executables with hardened runtime and allow-jit. |
| `-DANYPS5_ENABLE_SPIRV_TOOLS=ON` | `OFF`   | Enable SPIR-V validation and optimization.       |
| `-DAPS5_ENABLE_TIMING_LOG=ON`    | `OFF`   | Compile frame timing logging.                    |
| `-DAPS5_AGC_CREATE_LOG=OFF`      | `ON`    | Disable successful `sceAgcCreateShader` logging. |
| `-DAGC_BUILD_VISUAL_TEST=ON`     | `OFF`   | Build the standalone AGC SPIR-V visual test.     |

Build configuration parameters:

| Flag                                   | Value                                                              |
|----------------------------------------|--------------------------------------------------------------------|
| `-DCMAKE_BUILD_TYPE=Release`           | `Debug`, `Release`, `RelWithDebInfo`, or `MinSizeRel`.             |
| `-DCMAKE_C_COMPILER=gcc`               | C compiler name or absolute path.                                  |
| `-DCMAKE_CXX_COMPILER=g++`             | C++ compiler name or absolute path.                                |
| `-DCMAKE_C_COMPILER_LAUNCHER=ccache`   | Optional C compiler cache; requires `ccache`.                      |
| `-DCMAKE_CXX_COMPILER_LAUNCHER=ccache` | Optional C++ compiler cache; requires `ccache`.                    |
| `-DFFMPEG_PREBUILT_DIR=<path>`         | Unpacked FFmpeg package for the target platform; empty by default. |

SDL and FreeType settings forced by the root `CMakeLists.txt` cannot be overridden with `-D`.

## Pipeline statistics

Set `APS5_PIPELINE_STATS=1` to capture and print driver statistics for each newly created graphics or compute pipeline. This requires `VK_KHR_pipeline_executable_properties` and `pipelineExecutableInfo`; an unsupported device fails with an error. Statistic names and units are driver-specific. Capturing statistics can increase pipeline compilation cost. The setting is disabled by default.

## Shader recompiler

The shader recompilation logic in [core/shader/recompiler](../../core/shader/recompiler) is isolated from the rest of the project and is a pure function of its input data, designed for integration into any other project. The current CMake target also includes cache support and links a supplied runtime target, glslang, and optionally SPIRV-Tools.
