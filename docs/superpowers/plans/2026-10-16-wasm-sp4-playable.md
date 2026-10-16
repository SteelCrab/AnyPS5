# SP4 브라우저 플레이 (입력, 오디오, 세이브) 구현 계획

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Chrome에서 Dreaming Sarah를 New game으로 시작해 키보드·게임패드로 첫 장면을 플레이하고, 소리가 나고, 새로고침 뒤 Continue로 이어지게 한다.

**Architecture:** 런타임에 플랫폼 계층(`Platform.hpp`: `WasmInput`, `WasmAudio`, `WasmStorage`)을 두고 웹 빌드와 Node 빌드에 서로 다른 구현을 링크한다. 브라우저 메인 스레드·AudioWorklet과 런타임은 wasm 공유 메모리의 링 버퍼로 직접 주고받는다. 링 주소는 런타임이 `MAIN_THREAD_ASYNC_EM_ASM`으로 `globalThis.aps5Platform`에 알린다. 세이브는 IDBFS다.

**Tech Stack:** C++20, Emscripten 6.0.12 (wasm64, pthreads), WebGPU(emdawnwebgpu), JavaScript ES 모듈, AudioWorklet, Node 26 `node:test`, Playwright(헤드리스 Chromium).

**Spec:** [docs/superpowers/specs/2026-10-16-wasm-sp4-playable-design.md](../specs/2026-10-16-wasm-sp4-playable-design.md)

## Global Constraints

- 코드에 주석을 쓰지 않는다 (`docs/dev/CONVENTIONS.md`: technical debt, `#endif`, namespace 끝만 허용). JS·CMake·Python도 같다.
- 이름: 클래스와 public 함수 PascalCase, private 멤버·인자 camelCase, 인터페이스는 `I` 접두사.
- 함수는 하는 일을 정확히 하거나 던진다. 조용히 넘어가는 대체 동작을 넣지 않는다 (CONTRIBUTING).
- 라이브러리(`core/libs/prx/*`) 변경은 `#ifdef __EMSCRIPTEN__` 안에만 둔다. 네이티브 빌드는 바뀌지 않는다.
- 공유 메모리 형식 (C++과 JS가 같아야 한다):
  - 입력 링: 헤더 8바이트(`write` u32 @0, `read` u32 @4), 칸 64개 × 12바이트 @8. 칸: `buttons` u32, `sticks` u8×4, `l2`, `r2`, `touch`(1 왼쪽, 2 오른쪽, 4 가운데), `device`(0 없음, 1 DualSense, 2 DUALSHOCK 4, 3 기타).
  - 오디오 상태: 헤더 32바이트(`running` u32 @0), 링 4개 @32. 링: 헤더 32바이트(`active` @0, `write` @4, `read` @8, `underruns` @12, `consumed` @16), float32 스테레오 8192프레임 @32. 링 하나 = 32 + 65536바이트.
- 오디오: 48000 Hz만 지원한다. 출력 속도 상수는 `TARGET_LATENCY_US` 40000, `DRAIN_SLEEP_US` 1000, `DRAIN_TIMEOUT_US` 200000 (AudioOut.cpp 기존 값).
- 다운믹스: L = FL + 0.707·(C + SL + BL), R = FR + 0.707·(C + SR + BR), LFE 버림, [-1, 1] 클램프, 모노는 양쪽 같은 값, int16은 /32768.
- 키 매핑은 네이티브 기본값(`core/libs/prx/libScePad/include/InputMapping.hpp`)과 같다. 키보드로 누른 축은 게임패드 축보다 우선한다.
- git: 이 저장소의 `.git`은 깨져 있다 (HEAD·refs 없음). 각 Task의 Commit 단계는 사용자가 저장소를 복구한 경우에만 실행하고, 아니면 건너뛰고 건너뛰었다고 보고한다. 저장소 복구는 직접 하지 않는다.

## Shared Commands

모든 Task는 저장소 루트(`/home/pista/AnyPS5`)에서 아래 환경으로 실행한다.

```bash
source ~/emsdk/emsdk_env.sh
export NODE=$HOME/.local/opt/node-v26.12.0-linux-x64/bin/node
export CHECKS=$PWD/build-wasm-aot/sp4-checks
mkdir -p $CHECKS
export HEADLESS="sudo -n -u $USER -g render env HOME=$HOME USE_VULKAN=native VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/intel_icd.json PLAYWRIGHT_CORE=$HOME/firecrab-projects/firecrab-page/node_modules/playwright-core CHROMIUM=$HOME/.cache/ms-playwright/chromium-1234/chrome-linux64/chrome $NODE core/wasm-runtime/web/headless.cjs"
```

- 빌드: `cmake --build build-wasm-linux --target <대상> -j16`
- 테스트: `ctest --test-dir build-wasm-linux -R wasm_ --output-on-failure`
- 개발 서버 (백그라운드, `serve.py`를 바꾸면 다시 띄운다):
  `python3 core/wasm-runtime/web/serve.py --runtime build-wasm-linux/core/wasm-runtime --game build-wasm-aot/dreaming-sarah/web --port 8080`
- 헤드리스 확인은 Intel UHD(Mesa ANV)에서 돈다. 타이틀 메뉴는 약 8–13초에 나온다. 브라우저의 MEMFS는 매번 새로 시작하므로 세이브가 없는 상태다 (Task 8 전까지).
- `$HEADLESS <url> [콘솔 패턴] [제한 ms] [캔버스 png] [옵션]`의 옵션은 Task 3에서 추가한다: `--keys <ms>:<code>[:<누름 ms>],…`, `--snap <ms>:<png>,…`, `--audio`, `--autoplay`, `--profile <디렉터리>`.

## Review Focus

1. 키를 누른 채 창이 포커스를 잃으면(alt-tab) 그 키가 계속 눌린 상태로 남지 않아야 한다 → Task 3 `input.test.mjs` "a key press and the blur that follows".
2. 첫 클릭이나 키 입력 전(AudioContext `suspended`)에도 게임이 멈추지 않아야 한다 → Task 7 Step 6 (자동 재생 없이 present가 계속 늘어나는지).
3. 키를 누르고 있을 때 생기는 자동 반복 `keydown`이 입력 링을 채우면 안 된다 → Task 3 `input.test.mjs` "auto-repeat adds no slots".
4. 게임 키(Tab, Space, 방향키, Backspace)가 브라우저 동작(포커스 이동, 스크롤, 뒤로 가기)을 일으키면 안 된다 → Task 3 `input.test.mjs` "game keys keep the browser from acting on them".
5. 런타임이 잠시 입력을 못 읽어 링이 가득 차도 마지막 상태는 전달되어야 한다 → Task 3 `input.test.mjs` "a full ring keeps the latest state".

---

### Task 1: 플랫폼 인터페이스, Node 구현, 입력 링(C++)

**Files:**
- Create: `core/wasm-runtime/include/wasm-runtime/Platform.hpp`
- Create: `core/wasm-runtime/platform/NullPlatform.cpp`
- Create: `core/wasm-runtime/platform/InputRing.hpp`
- Create: `core/wasm-runtime/tests/PlatformTests.cpp`
- Modify: `CMakeLists.txt:24-25` (`enable_testing()`)
- Modify: `core/wasm-runtime/CMakeLists.txt` (테스트 실행 파일, null 플랫폼, Node 런타임 링크)

**Interfaces:**
- Produces:
  - `WasmInput::Start()`, `WasmAudio::{Frequency, Samples, Open, Running, Queued, Write, Close}`, `WasmStorage::Commit()` (`wasm-runtime/Platform.hpp`)
  - `WasmPlatform::InputSlot`, `WasmPlatform::InputRing`, `WasmPlatform::InputSlots`, `WasmPlatform::TouchLeft/TouchRight/TouchCenter`
  - `bool WasmPlatform::PopInput(InputRing& ring, std::uint32_t written, InputSlot& slot)`
  - `PadInputState WasmPlatform::ToPadInputState(const InputSlot& slot)`
  - CMake 대상 `anyps5-platform-tests`, `aps5_wasm_null_platform`, ctest `wasm_platform`, 캐시 변수 `ANYPS5_NODE`

- [ ] **Step 1: 실패하는 테스트 작성**

`core/wasm-runtime/tests/PlatformTests.cpp`:

```cpp
#include "InputRing.hpp"
#include <array>
#include <cstdint>
#include <cstdlib>

namespace {

void Require(bool value) {
    if (!value) std::abort();
}

void PushSlot(WasmPlatform::InputRing& ring, const WasmPlatform::InputSlot& slot) {
    const auto write = ring.write.load();
    ring.slots[write % WasmPlatform::InputSlots] = slot;
    ring.write.store(write + 1);
}

void TestInputRingKeepsOrder() {
    WasmPlatform::InputRing ring;
    for (std::uint32_t index = 0; index < 3; ++index) PushSlot(ring, {index + 1});
    WasmPlatform::InputSlot slot;
    for (std::uint32_t index = 0; index < 3; ++index) {
        Require(WasmPlatform::PopInput(ring, ring.write.load(), slot));
        Require(slot.buttons == index + 1);
    }
    Require(!WasmPlatform::PopInput(ring, ring.write.load(), slot));
    Require(ring.read.load() == 3);
}

void TestInputRingWraps() {
    WasmPlatform::InputRing ring;
    WasmPlatform::InputSlot slot;
    for (std::uint32_t index = 0; index < 3 * WasmPlatform::InputSlots + 5; ++index) {
        PushSlot(ring, {index});
        Require(WasmPlatform::PopInput(ring, ring.write.load(), slot));
        Require(slot.buttons == index);
    }
}

void TestInputRingWrapsAroundTheIndexRange() {
    WasmPlatform::InputRing ring;
    ring.write.store(0xffffffffu);
    ring.read.store(0xffffffffu);
    PushSlot(ring, {7});
    Require(ring.write.load() == 0);
    WasmPlatform::InputSlot slot;
    Require(WasmPlatform::PopInput(ring, ring.write.load(), slot) && slot.buttons == 7);
    Require(ring.read.load() == 0);
}

void TestInputRingStopsAtObservedWrite() {
    WasmPlatform::InputRing ring;
    PushSlot(ring, {1});
    const auto observed = ring.write.load();
    PushSlot(ring, {2});
    WasmPlatform::InputSlot slot;
    Require(WasmPlatform::PopInput(ring, observed, slot) && slot.buttons == 1);
    Require(!WasmPlatform::PopInput(ring, observed, slot));
}

void TestPadStateConversion() {
    const WasmPlatform::InputSlot slot{0x4010, {0, 255, 128, 64}, 10, 255, WasmPlatform::TouchLeft | WasmPlatform::TouchCenter, 1};
    const auto state = WasmPlatform::ToPadInputState(slot);
    Require(state.buttons == 0x4010);
    Require(state.sticks == std::array<std::uint8_t, 4>{0, 255, 128, 64});
    Require(state.analogButtonsL2 == 10 && state.analogButtonsR2 == 255);
    Require(state.touchLeft && !state.touchRight);
    Require(state.touch[0].active && state.touch[0].x == 960 && state.touch[0].y == 471 && state.touch[0].id == 0);
    Require(!state.touch[1].active);
    Require(state.deviceKind == 1 && !state.hasMotion);
}

void TestNeutralSlot() {
    const auto state = WasmPlatform::ToPadInputState({});
    Require(state.buttons == 0 && state.sticks == std::array<std::uint8_t, 4>{128, 128, 128, 128});
    Require(state.analogButtonsL2 == 0 && state.analogButtonsR2 == 0);
    Require(!state.touchLeft && !state.touchRight && !state.touch[0].active && state.deviceKind == 0);
}

}  // namespace

int main() {
    TestInputRingKeepsOrder();
    TestInputRingWraps();
    TestInputRingWrapsAroundTheIndexRange();
    TestInputRingStopsAtObservedWrite();
    TestPadStateConversion();
    TestNeutralSlot();
    return 0;
}
```

`CMakeLists.txt` (루트)의 WASM 런타임 분기에서 `add_subdirectory(core/wasm-runtime)` 바로 앞에 한 줄을 넣는다:

```cmake
    enable_testing()
    add_subdirectory(core/wasm-runtime)
```

`core/wasm-runtime/CMakeLists.txt` 끝에 붙인다:

```cmake
set(ANYPS5_NODE node CACHE FILEPATH "Node.js 26 or newer for the WebAssembly tests")

add_executable(anyps5-platform-tests tests/PlatformTests.cpp)
set_target_properties(anyps5-platform-tests PROPERTIES SUFFIX .js)
target_include_directories(anyps5-platform-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/platform ${ANYPS5_LIBS_DIR})
target_link_options(anyps5-platform-tests PRIVATE -sENVIRONMENT=node -sEXIT_RUNTIME)
add_test(NAME wasm_platform COMMAND ${ANYPS5_NODE} $<TARGET_FILE:anyps5-platform-tests>)
```

- [ ] **Step 2: 실패 확인**

```bash
emcmake cmake -S . -B build-wasm-linux -DANYPS5_BUILD_WASM_RUNTIME=ON -DCMAKE_BUILD_TYPE=Release -DANYPS5_NODE=$NODE
cmake --build build-wasm-linux --target anyps5-platform-tests -j16
```

Expected: 컴파일 실패 `'InputRing.hpp' file not found`.

- [ ] **Step 3: 구현**

`core/wasm-runtime/include/wasm-runtime/Platform.hpp`:

```cpp
#ifndef CORE_WASM_RUNTIME_INCLUDE_WASM_RUNTIME_PLATFORM_HPP
#define CORE_WASM_RUNTIME_INCLUDE_WASM_RUNTIME_PLATFORM_HPP

#include <cstdint>

namespace WasmInput {
void Start();
}

namespace WasmAudio {
inline constexpr std::uint32_t Frequency = 48000;

struct Samples {
    const void* data = nullptr;
    bool isFloat = false;
    std::uint32_t channels = 0;
    std::uint32_t frames = 0;
};

int Open(std::uint32_t frequency);
bool Running();
std::uint32_t Queued(int stream);
std::uint32_t Write(int stream, const Samples& samples);
void Close(int stream);
}

namespace WasmStorage {
void Commit();
}

#endif
```

`core/wasm-runtime/platform/InputRing.hpp`:

```cpp
#ifndef CORE_WASM_RUNTIME_PLATFORM_INPUTRING_HPP
#define CORE_WASM_RUNTIME_PLATFORM_INPUTRING_HPP

#include "prx/libScePad/include/PadState.hpp"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace WasmPlatform {

inline constexpr std::uint32_t InputSlots = 64;
inline constexpr std::uint8_t TouchLeft = 1;
inline constexpr std::uint8_t TouchRight = 2;
inline constexpr std::uint8_t TouchCenter = 4;

struct InputSlot {
    std::uint32_t buttons = 0;
    std::array<std::uint8_t, 4> sticks{128, 128, 128, 128};
    std::uint8_t l2 = 0;
    std::uint8_t r2 = 0;
    std::uint8_t touch = 0;
    std::uint8_t device = 0;
};

struct InputRing {
    std::atomic<std::uint32_t> write{0};
    std::atomic<std::uint32_t> read{0};
    std::array<InputSlot, InputSlots> slots{};
};

static_assert(sizeof(InputSlot) == 12);
static_assert(offsetof(InputRing, slots) == 8);
static_assert(sizeof(InputRing) == 8 + InputSlots * sizeof(InputSlot));

inline bool PopInput(InputRing& ring, std::uint32_t written, InputSlot& slot) {
    const auto read = ring.read.load(std::memory_order_relaxed);
    if (read == written) return false;
    slot = ring.slots[read % InputSlots];
    ring.read.store(read + 1, std::memory_order_release);
    return true;
}

inline PadInputState ToPadInputState(const InputSlot& slot) {
    PadInputState state;
    state.buttons = slot.buttons;
    state.sticks = slot.sticks;
    state.analogButtonsL2 = slot.l2;
    state.analogButtonsR2 = slot.r2;
    state.touchLeft = (slot.touch & TouchLeft) != 0;
    state.touchRight = (slot.touch & TouchRight) != 0;
    if ((slot.touch & TouchCenter) != 0) state.touch[0] = {true, 960, 471, 0};
    state.deviceKind = slot.device;
    return state;
}

}  // namespace WasmPlatform

#endif
```

`core/wasm-runtime/platform/NullPlatform.cpp`:

```cpp
#include "wasm-runtime/Platform.hpp"
#include <stdexcept>

void WasmInput::Start() {
}

int WasmAudio::Open(std::uint32_t) {
    return -1;
}

bool WasmAudio::Running() {
    return false;
}

std::uint32_t WasmAudio::Queued(int) {
    throw std::logic_error("WasmAudio: the Node runtime has no audio streams");
}

std::uint32_t WasmAudio::Write(int, const Samples&) {
    throw std::logic_error("WasmAudio: the Node runtime has no audio streams");
}

void WasmAudio::Close(int) {
    throw std::logic_error("WasmAudio: the Node runtime has no audio streams");
}

void WasmStorage::Commit() {
}
```

`core/wasm-runtime/CMakeLists.txt`: `aps5_wasm_null_backend` 정의 바로 뒤에 넣는다.

```cmake
add_library(aps5_wasm_null_platform OBJECT platform/NullPlatform.cpp)
target_include_directories(aps5_wasm_null_platform PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
```

그리고 `anyps5-runtime-node` 줄의 목록 끝에 `$<TARGET_OBJECTS:aps5_wasm_null_platform>`를 더한다:

```cmake
anyps5_wasm_runtime_executable(anyps5-runtime-node ${ANYPS5_WASM_LIBRARY_OBJECTS} $<TARGET_OBJECTS:aps5_wasm_recompiler> $<TARGET_OBJECTS:aps5_wasm_gpu_state> $<TARGET_OBJECTS:aps5_wasm_null_backend> $<TARGET_OBJECTS:aps5_wasm_null_platform>)
```

- [ ] **Step 4: 통과 확인**

```bash
cmake --build build-wasm-linux --target anyps5-platform-tests anyps5-runtime-node -j16
ctest --test-dir build-wasm-linux -R wasm_platform --output-on-failure
```

Expected: `wasm_platform ... Passed`, Node 런타임 빌드 성공.

- [ ] **Step 5: Commit** (저장소가 복구된 경우에만)

```bash
git add CMakeLists.txt core/wasm-runtime/CMakeLists.txt core/wasm-runtime/include/wasm-runtime/Platform.hpp core/wasm-runtime/platform core/wasm-runtime/tests/PlatformTests.cpp
git commit -m "feat(wasm-runtime): add the platform layer and the input ring"
```

---

### Task 2: 입력 매핑과 링 쓰기 (JS)

**Files:**
- Create: `core/wasm-runtime/web/input-map.mjs`
- Create: `core/wasm-runtime/web/input-ring.mjs`
- Create: `core/wasm-runtime/web/tests/input-map.test.mjs`
- Create: `core/wasm-runtime/web/tests/input-ring.test.mjs`
- Modify: `core/wasm-runtime/CMakeLists.txt` (ctest `wasm_web_scripts`)

**Interfaces:**
- Consumes: Global Constraints의 입력 링 형식 (Task 1의 `InputRing`과 같은 바이트 배치).
- Produces:
  - `input-map.mjs`: `PadButton`, `Touch`, `Device`, `GameKeyCodes: Set<string>`, `stickByte(axis: number): number`, `deviceKind(id: string): number`, `composePadState(pressed: Set<string>, gamepad: Gamepad|null): PadState`, `samePadState(a, b): boolean`. `PadState = { buttons, sticks: [4], l2, r2, touch, device }`.
  - `input-ring.mjs`: `InputSlots`(64), `InputSlotBytes`(12), `InputHeaderBytes`(8), `class InputRingWriter { constructor(buffer, address); push(state): boolean }`.

- [ ] **Step 1: 실패하는 테스트 작성**

`core/wasm-runtime/web/tests/input-map.test.mjs`:

```js
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { composePadState, samePadState, stickByte, deviceKind, GameKeyCodes, PadButton, Touch, Device } from '../input-map.mjs';

const none = new Set();

function pad(id, pressed = [], values = {}, axes = [0, 0, 0, 0]) {
  const buttons = Array.from({ length: 18 }, (_, index) => ({ pressed: pressed.includes(index), value: values[index] ?? (pressed.includes(index) ? 1 : 0) }));
  return { id, buttons, axes, connected: true };
}

test('neutral state without input', () => {
  assert.deepEqual(composePadState(none, null), { buttons: 0, sticks: [128, 128, 128, 128], l2: 0, r2: 0, touch: 0, device: Device.None });
});

test('keyboard buttons follow the native default mapping', () => {
  const state = composePadState(new Set(['Enter', 'ArrowLeft', 'KeyQ', 'AltRight', 'Escape']), null);
  assert.equal(state.buttons, PadButton.Cross | PadButton.Left | PadButton.L1 | PadButton.R1 | PadButton.Options);
  assert.equal(composePadState(new Set(['Space']), null).buttons, PadButton.Cross);
});

test('keyboard sticks tilt fully and opposite keys cancel', () => {
  assert.deepEqual(composePadState(new Set(['KeyA', 'KeyS']), null).sticks, [0, 255, 128, 128]);
  assert.deepEqual(composePadState(new Set(['KeyA', 'KeyD', 'KeyT']), null).sticks, [128, 128, 128, 0]);
});

test('touch keys press the touch pad halves', () => {
  const state = composePadState(new Set(['Backspace', 'Tab']), null);
  assert.equal(state.touch, Touch.Left | Touch.Right);
  assert.equal(state.buttons, 0);
});

test('stick bytes match the native conversion', () => {
  assert.equal(stickByte(-32768), 0);
  assert.equal(stickByte(0), 128);
  assert.equal(stickByte(32767), 255);
});

test('standard gamepad buttons, triggers and axes', () => {
  const state = composePadState(none, pad('Xbox Wireless Controller (STANDARD GAMEPAD)', [0, 3, 9, 12], { 6: 0.5 }, [-1, 1, 0, 0]));
  assert.equal(state.buttons, PadButton.Cross | PadButton.Triangle | PadButton.Options | PadButton.Up | PadButton.L2);
  assert.equal(state.l2, 128);
  assert.equal(state.r2, 0);
  assert.deepEqual(state.sticks, [0, 255, 128, 128]);
  assert.equal(state.device, Device.Other);
});

test('view is the touch pad on other pads, the touch pad click on PlayStation pads', () => {
  const other = composePadState(none, pad('Xbox Wireless Controller', [8]));
  assert.equal(other.buttons, PadButton.TouchPad);
  assert.equal(other.touch, Touch.Center);
  const dualSense = composePadState(none, pad('DualSense Wireless Controller (STANDARD GAMEPAD Vendor: 054c Product: 0ce6)', [8, 17]));
  assert.equal(dualSense.buttons, PadButton.TouchPad);
  assert.equal(dualSense.touch, 0);
  assert.equal(dualSense.device, Device.DualSense);
});

test('keyboard axes override the gamepad axes', () => {
  const state = composePadState(new Set(['KeyD']), pad('Xbox Wireless Controller', [], {}, [-1, 0.5, 0, 0]));
  assert.deepEqual(state.sticks, [255, stickByte(Math.round(0.5 * 32767)), 128, 128]);
});

test('device kinds come from the gamepad id', () => {
  assert.equal(deviceKind('Wireless Controller (STANDARD GAMEPAD Vendor: 054c Product: 09cc)'), Device.DualShock4);
  assert.equal(deviceKind('054c-0ce6-DualSense Wireless Controller'), Device.DualSense);
  assert.equal(deviceKind('Xbox Wireless Controller (STANDARD GAMEPAD Vendor: 045e Product: 0b13)'), Device.Other);
});

test('game keys are exactly the mapped keys', () => {
  for (const code of ['Enter', 'Space', 'Tab', 'Backspace', 'ArrowDown', 'KeyW', 'Escape']) assert.ok(GameKeyCodes.has(code), code);
  for (const code of ['F5', 'KeyZ', 'Digit1']) assert.ok(!GameKeyCodes.has(code), code);
});

test('samePadState compares every field', () => {
  const base = composePadState(none, null);
  assert.ok(samePadState(base, composePadState(none, null)));
  for (const change of [{ buttons: 1 }, { l2: 1 }, { r2: 1 }, { touch: 1 }, { device: 1 }, { sticks: [128, 128, 128, 0] }]) {
    assert.ok(!samePadState(base, { ...base, ...change }), JSON.stringify(change));
  }
});
```

`core/wasm-runtime/web/tests/input-ring.test.mjs`:

```js
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { InputRingWriter, InputSlots, InputSlotBytes, InputHeaderBytes } from '../input-ring.mjs';

const neutral = { buttons: 0, sticks: [128, 128, 128, 128], l2: 0, r2: 0, touch: 0, device: 0 };
const address = 64;

function ring() {
  const buffer = new SharedArrayBuffer(4096);
  return { buffer, writer: new InputRingWriter(buffer, address), header: new Uint32Array(buffer, address, 2) };
}

function slotBytes(buffer, index) {
  return [...new Uint8Array(buffer, address + InputHeaderBytes + index * InputSlotBytes, InputSlotBytes)];
}

test('writes the slot layout the runtime reads', () => {
  const { buffer, writer, header } = ring();
  assert.ok(writer.push({ buttons: 0x104010, sticks: [1, 2, 3, 4], l2: 5, r2: 6, touch: 4, device: 1 }));
  assert.deepEqual([header[0], header[1]], [1, 0]);
  assert.deepEqual(slotBytes(buffer, 0), [0x10, 0x40, 0x10, 0x00, 1, 2, 3, 4, 5, 6, 4, 1]);
});

test('refuses to overwrite unread slots', () => {
  const { buffer, writer, header } = ring();
  for (let index = 0; index < InputSlots; index += 1) assert.ok(writer.push(neutral));
  assert.ok(!writer.push({ ...neutral, buttons: 9 }));
  assert.equal(header[0], InputSlots);
  Atomics.store(header, 1, 1);
  assert.ok(writer.push({ ...neutral, buttons: 9 }));
  assert.equal(slotBytes(buffer, 0)[0], 9);
});

test('indexes wrap around the 32-bit range', () => {
  const { buffer, writer, header } = ring();
  Atomics.store(header, 0, 0xffffffff);
  Atomics.store(header, 1, 0xffffffff);
  assert.ok(writer.push({ ...neutral, buttons: 7 }));
  assert.equal(header[0], 0);
  assert.equal(slotBytes(buffer, InputSlots - 1)[0], 7);
});
```

`core/wasm-runtime/CMakeLists.txt` 끝에 붙인다:

```cmake
add_test(NAME wasm_web_scripts COMMAND ${ANYPS5_NODE} --test ${CMAKE_CURRENT_SOURCE_DIR}/web/tests/*.test.mjs)
```

- [ ] **Step 2: 실패 확인**

```bash
emcmake cmake -S . -B build-wasm-linux
ctest --test-dir build-wasm-linux -R wasm_web_scripts --output-on-failure
```

Expected: FAIL, `Cannot find module '.../input-map.mjs'`.

- [ ] **Step 3: 구현**

`core/wasm-runtime/web/input-map.mjs`:

```js
export const PadButton = Object.freeze({
  L3: 0x2, R3: 0x4, Options: 0x8, Up: 0x10, Right: 0x20, Down: 0x40, Left: 0x80,
  L2: 0x100, R2: 0x200, L1: 0x400, R1: 0x800, Triangle: 0x1000, Circle: 0x2000, Cross: 0x4000, Square: 0x8000,
  TouchPad: 0x100000,
});

export const Touch = Object.freeze({ Left: 1, Right: 2, Center: 4 });

export const Device = Object.freeze({ None: 0, DualSense: 1, DualShock4: 2, Other: 3 });

const KeyButtons = new Map([
  ['Enter', PadButton.Cross], ['Space', PadButton.Cross], ['Escape', PadButton.Options],
  ['KeyI', PadButton.Triangle], ['KeyC', PadButton.Circle], ['KeyQ', PadButton.L1], ['KeyE', PadButton.R1],
  ['AltLeft', PadButton.R1], ['AltRight', PadButton.R1], ['ShiftLeft', PadButton.L3], ['ShiftRight', PadButton.L3],
  ['ControlLeft', PadButton.R3], ['ControlRight', PadButton.R3],
  ['ArrowUp', PadButton.Up], ['ArrowRight', PadButton.Right], ['ArrowDown', PadButton.Down], ['ArrowLeft', PadButton.Left],
]);

const KeyAxes = new Map([
  ['KeyA', [0, -1]], ['KeyD', [0, 1]], ['KeyW', [1, -1]], ['KeyS', [1, 1]],
  ['KeyF', [2, -1]], ['KeyH', [2, 1]], ['KeyT', [3, -1]], ['KeyG', [3, 1]],
]);

const KeyTouch = new Map([['Backspace', Touch.Left], ['Tab', Touch.Right]]);

const GamepadButtons = [
  PadButton.Cross, PadButton.Circle, PadButton.Square, PadButton.Triangle, PadButton.L1, PadButton.R1, 0, 0, 0,
  PadButton.Options, PadButton.L3, PadButton.R3, PadButton.Up, PadButton.Down, PadButton.Left, PadButton.Right,
];

export const GameKeyCodes = new Set([...KeyButtons.keys(), ...KeyAxes.keys(), ...KeyTouch.keys()]);

export function stickByte(axis) {
  return Math.floor(((axis + 32768) * 255 + 32767) / 65535);
}

function axisByte(value) {
  return stickByte(Math.round(Math.max(-1, Math.min(1, value)) * 32767));
}

function triggerByte(value) {
  return Math.round(Math.max(0, Math.min(1, value)) * 255);
}

export function deviceKind(id) {
  const text = id.toLowerCase();
  const sony = text.includes('054c');
  if (text.includes('dualsense') || (sony && /0ce6|0df2/.test(text))) return Device.DualSense;
  if (text.includes('dualshock') || (sony && /05c4|09cc|0ba0/.test(text))) return Device.DualShock4;
  return Device.Other;
}

function pressedAt(gamepad, index) {
  return gamepad.buttons[index]?.pressed === true;
}

export function composePadState(pressed, gamepad) {
  const state = { buttons: 0, sticks: [128, 128, 128, 128], l2: 0, r2: 0, touch: 0, device: Device.None };
  if (gamepad !== null) {
    state.device = deviceKind(gamepad.id);
    GamepadButtons.forEach((button, index) => {
      if (button !== 0 && pressedAt(gamepad, index)) state.buttons |= button;
    });
    state.l2 = triggerByte(gamepad.buttons[6]?.value ?? 0);
    state.r2 = triggerByte(gamepad.buttons[7]?.value ?? 0);
    const playStation = state.device === Device.DualSense || state.device === Device.DualShock4;
    if (playStation && pressedAt(gamepad, 17)) state.buttons |= PadButton.TouchPad;
    if (!playStation && pressedAt(gamepad, 8)) {
      state.buttons |= PadButton.TouchPad;
      state.touch |= Touch.Center;
    }
    for (let axis = 0; axis < 4; axis += 1) state.sticks[axis] = axisByte(gamepad.axes[axis] ?? 0);
  }
  const negative = [false, false, false, false];
  const positive = [false, false, false, false];
  for (const code of pressed) {
    state.buttons |= KeyButtons.get(code) ?? 0;
    state.touch |= KeyTouch.get(code) ?? 0;
    const axis = KeyAxes.get(code);
    if (axis !== undefined) (axis[1] < 0 ? negative : positive)[axis[0]] = true;
  }
  for (let axis = 0; axis < 4; axis += 1) {
    if (negative[axis] || positive[axis]) state.sticks[axis] = negative[axis] === positive[axis] ? 128 : negative[axis] ? 0 : 255;
  }
  if (state.l2 !== 0) state.buttons |= PadButton.L2;
  if (state.r2 !== 0) state.buttons |= PadButton.R2;
  state.buttons >>>= 0;
  return state;
}

export function samePadState(first, second) {
  return first.buttons === second.buttons && first.l2 === second.l2 && first.r2 === second.r2 && first.touch === second.touch
    && first.device === second.device && first.sticks.every((value, index) => value === second.sticks[index]);
}
```

`core/wasm-runtime/web/input-ring.mjs`:

```js
export const InputSlots = 64;
export const InputSlotBytes = 12;
export const InputHeaderBytes = 8;

const Write = 0;
const Read = 1;

export class InputRingWriter {
  constructor(buffer, address) {
    this.header = new Int32Array(buffer, address, 2);
    this.slots = new DataView(buffer, address + InputHeaderBytes, InputSlots * InputSlotBytes);
  }

  push(state) {
    const write = Atomics.load(this.header, Write);
    const read = Atomics.load(this.header, Read);
    if ((write - read) >>> 0 >= InputSlots) return false;
    const offset = ((write >>> 0) % InputSlots) * InputSlotBytes;
    this.slots.setUint32(offset, state.buttons >>> 0, true);
    state.sticks.forEach((value, index) => this.slots.setUint8(offset + 4 + index, value));
    this.slots.setUint8(offset + 8, state.l2);
    this.slots.setUint8(offset + 9, state.r2);
    this.slots.setUint8(offset + 10, state.touch);
    this.slots.setUint8(offset + 11, state.device);
    Atomics.store(this.header, Write, (write + 1) | 0);
    Atomics.notify(this.header, Write);
    return true;
  }
}
```

- [ ] **Step 4: 통과 확인**

```bash
ctest --test-dir build-wasm-linux -R wasm_ --output-on-failure
```

Expected: `wasm_platform`, `wasm_web_scripts` 모두 Passed.

- [ ] **Step 5: Commit** (저장소가 복구된 경우에만)

```bash
git add core/wasm-runtime/web/input-map.mjs core/wasm-runtime/web/input-ring.mjs core/wasm-runtime/web/tests core/wasm-runtime/CMakeLists.txt
git commit -m "feat(wasm-runtime): map keyboard and gamepad state for the browser"
```

---

### Task 3: 입력 연결 (M1)

**Files:**
- Create: `core/wasm-runtime/web/input.mjs`
- Create: `core/wasm-runtime/web/tests/input.test.mjs`
- Create: `core/wasm-runtime/platform/WebInput.cpp`
- Modify: `core/libs/prx/libScePad/src/PadState.cpp:1-12, 104-113` (`Pad::Initialize`)
- Modify: `core/wasm-runtime/CMakeLists.txt` (`aps5_wasm_web_platform`, 웹 런타임 링크)
- Modify: `core/wasm-runtime/web/index.html` (모듈 스크립트, `aps5Platform`)
- Modify: `core/wasm-runtime/web/serve.py` (`.mjs` MIME)
- Modify: `core/wasm-runtime/web/headless.cjs` (옵션)
- Modify: `docs/superpowers/specs/2026-10-16-wasm-sp4-playable-design.md` (진행 상황 절)

**Interfaces:**
- Consumes: Task 1 `WasmPlatform::PopInput`, `ToPadInputState`, `InputRing`; Task 2 `composePadState`, `samePadState`, `GameKeyCodes`, `InputRingWriter`. libScePad의 `PadPublishInput_nid_postfix(const PadInputState&)`.
- Produces:
  - `input.mjs`: `startInput(target, gamepads = connectedPads): { attach(buffer, address) }`. `target`은 `addEventListener`와 `requestAnimationFrame`이 있는 객체(브라우저에서는 `window`).
  - `globalThis.aps5Platform.attachInput(buffer, address)` (index.html). Task 7·8이 같은 객체에 `attachAudio`, `audioStats`, `commitStorage`를 더한다.
  - 웹 런타임의 `WasmInput::Start()` (입력 스레드 1개).
  - `headless.cjs` 옵션 `--keys`, `--snap`, `--audio`, `--autoplay`, `--profile`.

- [ ] **Step 1: 실패하는 테스트 작성**

`core/wasm-runtime/web/tests/input.test.mjs`:

```js
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { startInput } from '../input.mjs';
import { InputHeaderBytes, InputSlotBytes, InputSlots } from '../input-ring.mjs';
import { PadButton } from '../input-map.mjs';

class FakeWindow extends EventTarget {
  constructor() {
    super();
    this.callbacks = [];
  }

  requestAnimationFrame(callback) {
    this.callbacks.push(callback);
    return this.callbacks.length;
  }

  frame() {
    const callbacks = this.callbacks;
    this.callbacks = [];
    for (const callback of callbacks) callback(0);
  }
}

function keyEvent(type, code, repeat = false) {
  return Object.assign(new Event(type, { cancelable: true }), { code, repeat });
}

function attached(gamepads = () => []) {
  const window = new FakeWindow();
  const input = startInput(window, gamepads);
  const buffer = new SharedArrayBuffer(4096);
  const address = 256;
  input.attach(buffer, address);
  const header = new Uint32Array(buffer, address, 2);
  const slots = new DataView(buffer, address + InputHeaderBytes, InputSlots * InputSlotBytes);
  const buttons = (index) => slots.getUint32((index % InputSlots) * InputSlotBytes, true);
  return { window, header, buttons };
}

test('attaching sends the current state', () => {
  const { header, buttons } = attached();
  assert.equal(header[0], 1);
  assert.equal(buttons(0), 0);
});

test('a key press and the blur that follows reach the ring in order', () => {
  const { window, header, buttons } = attached();
  window.dispatchEvent(keyEvent('keydown', 'Enter'));
  window.dispatchEvent(new Event('blur'));
  assert.equal(header[0], 3);
  assert.equal(buttons(1), PadButton.Cross);
  assert.equal(buttons(2), 0);
});

test('a key release reaches the ring', () => {
  const { window, header, buttons } = attached();
  window.dispatchEvent(keyEvent('keydown', 'ArrowLeft'));
  window.dispatchEvent(keyEvent('keyup', 'ArrowLeft'));
  assert.equal(header[0], 3);
  assert.equal(buttons(1), PadButton.Left);
  assert.equal(buttons(2), 0);
});

test('auto-repeat adds no slots', () => {
  const { window, header } = attached();
  window.dispatchEvent(keyEvent('keydown', 'ArrowRight'));
  for (let index = 0; index < 5; index += 1) window.dispatchEvent(keyEvent('keydown', 'ArrowRight', true));
  window.frame();
  assert.equal(header[0], 2);
});

test('game keys keep the browser from acting on them', () => {
  const { window } = attached();
  const tab = keyEvent('keydown', 'Tab');
  const space = keyEvent('keyup', 'Space');
  const reload = keyEvent('keydown', 'F5');
  window.dispatchEvent(tab);
  window.dispatchEvent(space);
  window.dispatchEvent(reload);
  assert.ok(tab.defaultPrevented);
  assert.ok(space.defaultPrevented);
  assert.ok(!reload.defaultPrevented);
});

test('a full ring keeps the latest state until the runtime catches up', () => {
  const { window, header, buttons } = attached();
  Atomics.store(header, 0, InputSlots);
  window.dispatchEvent(keyEvent('keydown', 'ArrowUp'));
  window.dispatchEvent(keyEvent('keydown', 'Enter'));
  assert.equal(header[0], InputSlots);
  Atomics.store(header, 1, InputSlots);
  window.frame();
  assert.equal(header[0], InputSlots + 1);
  assert.equal(buttons(InputSlots), PadButton.Up | PadButton.Cross);
});

test('gamepads are read every frame', () => {
  const pad = { id: 'Xbox Wireless Controller', connected: true, axes: [0, 0, 0, 0], buttons: Array.from({ length: 17 }, () => ({ pressed: false, value: 0 })) };
  const { window, header, buttons } = attached(() => [null, pad]);
  pad.buttons[1] = { pressed: true, value: 1 };
  window.frame();
  assert.equal(header[0], 2);
  assert.equal(buttons(1), PadButton.Circle);
});

test('polling keeps running before the runtime attaches', () => {
  const window = new FakeWindow();
  startInput(window, () => []);
  window.dispatchEvent(keyEvent('keydown', 'Enter'));
  window.frame();
  assert.equal(window.callbacks.length, 1);
});
```

- [ ] **Step 2: 실패 확인**

```bash
ctest --test-dir build-wasm-linux -R wasm_web_scripts --output-on-failure
```

Expected: FAIL, `Cannot find module '.../input.mjs'`.

- [ ] **Step 3: `input.mjs` 구현**

```js
import { composePadState, samePadState, GameKeyCodes } from './input-map.mjs';
import { InputRingWriter } from './input-ring.mjs';

function connectedPads() {
  return typeof navigator.getGamepads === 'function' ? navigator.getGamepads() : [];
}

export function startInput(target, gamepads = connectedPads) {
  const pressed = new Set();
  let writer = null;
  let last = null;
  let pending = null;
  const drain = () => {
    if (writer !== null && pending !== null && writer.push(pending)) pending = null;
  };
  const update = () => {
    const pad = Array.from(gamepads()).find((candidate) => candidate !== null && candidate.connected) ?? null;
    const state = composePadState(pressed, pad);
    if (last === null || !samePadState(last, state)) {
      last = state;
      pending = state;
    }
    drain();
  };
  const frame = () => {
    update();
    target.requestAnimationFrame(frame);
  };
  target.addEventListener('keydown', (event) => {
    if (GameKeyCodes.has(event.code)) event.preventDefault();
    if (event.repeat) return;
    pressed.add(event.code);
    update();
  });
  target.addEventListener('keyup', (event) => {
    if (GameKeyCodes.has(event.code)) event.preventDefault();
    pressed.delete(event.code);
    update();
  });
  target.addEventListener('blur', () => {
    pressed.clear();
    update();
  });
  target.requestAnimationFrame(frame);
  return {
    attach(buffer, address) {
      writer = new InputRingWriter(buffer, address);
      update();
    },
  };
}
```

- [ ] **Step 4: JS 테스트 통과 확인**

```bash
ctest --test-dir build-wasm-linux -R wasm_web_scripts --output-on-failure
```

Expected: Passed.

- [ ] **Step 5: 런타임 쪽 연결**

`core/wasm-runtime/platform/WebInput.cpp`:

```cpp
#include "InputRing.hpp"
#include "wasm-runtime/Platform.hpp"
#include <emscripten/em_asm.h>
#include <emscripten/threading.h>
#include <cstdint>
#include <limits>
#include <mutex>
#include <thread>

namespace {

WasmPlatform::InputRing ring;

void Consume() {
    for (;;) {
        const auto written = ring.write.load(std::memory_order_acquire);
        WasmPlatform::InputSlot slot;
        if (WasmPlatform::PopInput(ring, written, slot)) {
            PadPublishInput_nid_postfix(WasmPlatform::ToPadInputState(slot));
        } else {
            emscripten_futex_wait(&ring.write, written, std::numeric_limits<double>::infinity());
        }
    }
}

}  // namespace

void WasmInput::Start() {
    static std::once_flag started;
    std::call_once(started, [] {
        std::thread(Consume).detach();
        MAIN_THREAD_ASYNC_EM_ASM({ globalThis.aps5Platform.attachInput(wasmMemory.buffer, $0); }, static_cast<double>(reinterpret_cast<std::uintptr_t>(&ring)));
    });
}
```

`core/libs/prx/libScePad/src/PadState.cpp`: 마지막 `#include <stdexcept>` 뒤에 넣는다.

```cpp
#ifdef __EMSCRIPTEN__
#include "wasm-runtime/Platform.hpp"
#endif
```

`Pad::Initialize`를 다음으로 바꾼다 (잠금 밖에서 입력 스레드를 시작한다):

```cpp
void Pad::Initialize() {
    {
        std::lock_guard lock(stateMutex);
        if (failure) std::rethrow_exception(failure);
        if (!initialized) {
            timestamp = sceKernelGetProcessTime();
            lastFuseTime = timestamp;
            queue.clear();
        }
        initialized = true;
    }
#ifdef __EMSCRIPTEN__
    WasmInput::Start();
#endif
}
```

`core/wasm-runtime/CMakeLists.txt`: `aps5_wasm_webgpu_backend` 정의 뒤에 넣는다.

```cmake
add_library(aps5_wasm_web_platform OBJECT platform/WebInput.cpp)
target_include_directories(aps5_wasm_web_platform PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/platform ${ANYPS5_LIBS_DIR})
```

`anyps5-runtime-web` 줄의 목록 끝에 `$<TARGET_OBJECTS:aps5_wasm_web_platform>`를 더한다:

```cmake
anyps5_wasm_runtime_executable(anyps5-runtime-web ${ANYPS5_WASM_LIBRARY_OBJECTS} $<TARGET_OBJECTS:aps5_wasm_recompiler> $<TARGET_OBJECTS:aps5_wasm_gpu_state> $<TARGET_OBJECTS:aps5_wasm_webgpu_backend> $<TARGET_OBJECTS:aps5_wasm_web_platform>)
```

- [ ] **Step 6: 페이지, 서버, 헤드리스 도구**

`core/wasm-runtime/web/index.html`의 `<script>` 블록 전체를 다음으로 바꾼다:

```html
<script type="module">
import { startInput } from './input.mjs';

function load(source) {
  return new Promise((resolve, reject) => {
    const script = document.createElement('script');
    script.src = source;
    script.onload = resolve;
    script.onerror = () => reject(new Error('cannot load ' + source));
    document.body.appendChild(script);
  });
}

const input = startInput(window);
globalThis.aps5Platform = {
  attachInput: (buffer, address) => input.attach(buffer, address),
};

(async () => {
  const parameters = new URLSearchParams(location.search);
  const game = parameters.get('game') || 'game/game.wasm';
  const data = parameters.get('data') || 'game/game.data.js';
  const runtime = parameters.get('runtime') || 'runtime/anyps5-runtime-web.js';
  globalThis.Module = {
    arguments: [game],
    canvas: document.getElementById('canvas'),
    print: (text) => console.log(text),
    printErr: (text) => console.log(text),
    locateFile: (path, prefix) => path.endsWith('.data') ? new URL(path, new URL(data, location.href)).href : prefix + path,
    preRun: [() => {
      for (const directory of ['/data', '/_sd']) {
        try { FS.mkdir(directory); } catch (error) {}
      }
      for (const entry of parameters.getAll('env')) {
        const separator = entry.indexOf('=');
        if (separator > 0) ENV[entry.slice(0, separator)] = entry.slice(separator + 1);
      }
    }],
  };
  globalThis.aps5GameModule = await WebAssembly.compileStreaming(fetch(game));
  await load(data);
  await load(runtime);
})().catch((error) => console.log('anyps5 loader: ' + (error.stack || error)));
</script>
```

`core/wasm-runtime/web/serve.py`의 `Handler.extensions_map[".wasm"] = "application/wasm"` 다음 줄에 넣는다:

```python
    Handler.extensions_map[".mjs"] = "text/javascript"
```

`core/wasm-runtime/web/headless.cjs` 전체를 다음으로 바꾼다 (위치 인자 4개는 그대로다):

```js
const fs = require('fs');
const { chromium } = require(process.env.PLAYWRIGHT_CORE || 'playwright-core');

const usage = 'usage: node headless.cjs <url> [console pattern] [timeout ms] [canvas png] [--keys <ms>:<code>[:<hold ms>],...] [--snap <ms>:<png>,...] [--audio] [--autoplay] [--profile <directory>]';

function parseArguments(argv) {
  const options = { positional: [], actions: [], audio: false, autoplay: false, profile: null };
  for (let index = 0; index < argv.length; index += 1) {
    const argument = argv[index];
    if (argument === '--keys') {
      for (const entry of argv[++index].split(',')) {
        const [at, code, hold] = entry.split(':');
        options.actions.push({ at: Number(at), kind: 'down', code });
        options.actions.push({ at: Number(at) + Number(hold || 100), kind: 'up', code });
      }
    } else if (argument === '--snap') {
      for (const entry of argv[++index].split(',')) {
        const separator = entry.indexOf(':');
        options.actions.push({ at: Number(entry.slice(0, separator)), kind: 'snap', file: entry.slice(separator + 1) });
      }
    } else if (argument === '--audio') {
      options.audio = true;
    } else if (argument === '--autoplay') {
      options.autoplay = true;
    } else if (argument === '--profile') {
      options.profile = argv[++index];
    } else {
      options.positional.push(argument);
    }
  }
  options.actions.sort((first, second) => first.at - second.at);
  return options;
}

async function saveCanvas(page, file) {
  const data = await page.evaluate(() => {
    const canvas = document.getElementById('canvas');
    const copy = document.createElement('canvas');
    copy.width = canvas.width;
    copy.height = canvas.height;
    copy.getContext('2d').drawImage(canvas, 0, 0);
    return copy.toDataURL('image/png');
  });
  fs.writeFileSync(file, Buffer.from(data.split(',')[1], 'base64'));
}

const options = parseArguments(process.argv.slice(2));
const [url, pattern, timeout, image] = options.positional;
if (!url) {
  console.error(usage);
  process.exit(2);
}

(async () => {
  const args = ['--enable-unsafe-webgpu', '--enable-features=Vulkan', '--use-angle=vulkan', `--use-vulkan=${process.env.USE_VULKAN || 'swiftshader'}`, '--ignore-gpu-blocklist'];
  if (options.autoplay) args.push('--autoplay-policy=no-user-gesture-required');
  const launch = { executablePath: process.env.CHROMIUM, headless: true, args };
  const browser = options.profile ? null : await chromium.launch(launch);
  const context = browser ? await browser.newContext() : await chromium.launchPersistentContext(options.profile, launch);
  const page = await context.newPage();
  const expression = pattern ? new RegExp(pattern) : null;
  let matched = false;
  const started = Date.now();
  const elapsed = () => ((Date.now() - started) / 1000).toFixed(1);
  page.on('console', (message) => {
    const text = message.text();
    console.log(`[${elapsed()}s] ${text}`);
    if (expression && expression.test(text)) matched = true;
  });
  page.on('pageerror', (error) => console.log('[pageerror] ' + error.message));
  const printAudio = async () => {
    const stats = await page.evaluate(() => globalThis.aps5Platform.audioStats());
    console.log(`[${elapsed()}s] [audio] ${JSON.stringify(stats)}`);
  };
  await page.goto(url);
  const deadline = started + Number(timeout || 60000);
  const pending = [...options.actions];
  while (Date.now() < deadline && ((expression && !matched) || pending.length > 0)) {
    while (pending.length > 0 && Date.now() - started >= pending[0].at) {
      const action = pending.shift();
      if (action.kind === 'down') {
        await page.keyboard.down(action.code);
      } else if (action.kind === 'up') {
        await page.keyboard.up(action.code);
      } else {
        await saveCanvas(page, action.file);
        console.log(`[${elapsed()}s] snap ${action.file}`);
        if (options.audio) await printAudio();
      }
    }
    await page.waitForTimeout(50);
  }
  if (image) {
    await page.waitForTimeout(1000);
    await saveCanvas(page, image);
  }
  if (options.audio) await printAudio();
  await context.close();
  if (browser) await browser.close();
  process.exit((!expression || matched) && pending.length === 0 ? 0 : 1);
})().catch((error) => {
  console.error(error);
  process.exit(1);
});
```

- [ ] **Step 7: 빌드와 Node 회귀**

```bash
cmake --build build-wasm-linux --target anyps5-runtime-web anyps5-runtime-node -j16
ctest --test-dir build-wasm-linux -R wasm_ --output-on-failure
cd build-wasm-aot/dreaming-sarah/linux && timeout 30 $NODE ../../../build-wasm-linux/core/wasm-runtime/anyps5-runtime-node.js ../game.wasm 2>&1 | grep -E "^\[gpu\] present 1:|anyps5 runtime"; cd -
```

Expected: 테스트 Passed. Node 출력에 `[gpu] present 1:`이 있고 `anyps5 runtime:` 오류 줄은 없다.

- [ ] **Step 8: 브라우저 확인 (M1)**

개발 서버를 다시 띄운 뒤(Shared Commands) 실행한다:

```bash
$HEADLESS http://127.0.0.1:8080/ "" 40000 "" --keys 10500:Enter,14000:ArrowDown --snap 13500:$CHECKS/m1-menu.png,15500:$CHECKS/m1-down.png 2>&1 | grep -E "pageerror|anyps5 loader|snap"
$HEADLESS http://127.0.0.1:8080/ "" 45000 "" --keys 10500:Enter,16000:Enter --snap 40000:$CHECKS/m1-game.png 2>&1 | grep -E "pageerror|anyps5 loader|Go to layout|snap"
```

Expected:
- `pageerror`, `anyps5 loader` 줄이 없다.
- `m1-menu.png`는 메뉴에서 `New game`이 선택되어 있고, `m1-down.png`는 선택 표시(▪)가 다른 항목으로 옮겨 가 있다 (Read 도구로 PNG를 열어 본다).
- 두 번째 실행에 `Go to layout: M0T0`이 있고 `m1-game.png`가 숲 장면이다.

- [ ] **Step 9: 진행 상황 기록**

spec 끝에 절을 더한다 (확인한 내용을 실제 결과로 채운다):

```markdown
## 12. 진행 상황

| 단계 | 상태 | 확인 |
|---|---|---|
| M1 | 완료 | 헤드리스 Chromium(Intel UHD)에서 Enter로 메뉴, 아래 방향키로 선택 이동, Enter 두 번으로 `M0T0` 진입 |
```

- [ ] **Step 10: Commit** (저장소가 복구된 경우에만)

```bash
git add core/wasm-runtime core/libs/prx/libScePad/src/PadState.cpp docs/superpowers/specs/2026-10-16-wasm-sp4-playable-design.md
git commit -m "feat(wasm-runtime): deliver browser keyboard and gamepad input to libScePad"
```

---

### Task 4: 게임 진입 GPU 기능 (M2)

**Files:**
- Modify: `core/wasm-runtime/gpu/WebGpuBackend.cpp:69-84` (`TextureFormat`)
- Modify: `core/shader/recompiler/WgslBackend/src/WgslEmitter.cpp` (헬퍼 `IsNan`, `emit()`의 비교 명령)
- Modify: `docs/superpowers/specs/2026-10-16-wasm-sp4-playable-design.md` (12절)

**Interfaces:**
- Consumes: Task 3의 `$HEADLESS ... --keys --snap`.
- Produces: WGSL 헬퍼 `aps5_is_nan(value: f32) -> bool`. IR 명령 `FPOrd{Equal,NotEqual,LessThan,GreaterThan,LessThanEqual,GreaterThanEqual}32`, `FPUnord…32`(같은 6개), `FPIsNan32`의 WGSL 변환. 텍스처 형식 `VK_FORMAT_R16_SFLOAT`, `VK_FORMAT_R16G16_SFLOAT`.

- [ ] **Step 1: 실패 재현 (캡처)**

```bash
cmake --build build-shader-tools -j16
rm -rf $CHECKS/capture $CHECKS/pad.txt && mkdir -p $CHECKS/capture
cd build-wasm-aot/dreaming-sarah/linux
( sleep 15; echo "2 16384 200" > $CHECKS/pad.txt; sleep 6; echo "3 16384 200" > $CHECKS/pad.txt; sleep 6; echo "4 16384 200" > $CHECKS/pad.txt ) &
APS5_PAD_INPUT_FILE=$CHECKS/pad.txt APS5_CAPTURE_SHADERS=$CHECKS/capture timeout 70 $NODE ../../../build-wasm-linux/core/wasm-runtime/anyps5-runtime-node.js ../game.wasm 2>&1 | grep -E "captured|capture of|Go to layout"; wait; cd -
```

Expected: `Go to layout: M0T0` 다음에 `capture of .../106791d00-106c96b00 failed: ... opcode=FPOrdLessThan32 is not implemented in WGSL`.

- [ ] **Step 2: 텍스처 형식 추가**

`WebGpuBackend.cpp`의 `TextureFormat`에서 `case VK_FORMAT_R32_SFLOAT: return wgpu::TextureFormat::R32Float;` 앞에 두 줄을 넣는다 (둘 다 WebGPU 기본 기능에서 샘플링·렌더·블렌딩이 된다):

```cpp
    case VK_FORMAT_R16_SFLOAT: return wgpu::TextureFormat::R16Float;
    case VK_FORMAT_R16G16_SFLOAT: return wgpu::TextureFormat::RG16Float;
```

- [ ] **Step 3: WGSL 비교 명령 구현**

의미는 SPIR-V 백엔드의 `OpFOrd*`/`OpFUnord*`/`OpIsNan`과 같다 (`core/shader/recompiler/SpirvBackend/src/SpirvAlu/SpirvAluEmitterMath.cpp`). WGSL은 NaN이 없다고 가정해도 되므로 NaN 판정은 비트로 한다.

`WgslEmitter.cpp`의 `HalfTowardZero` 정의 뒤에 넣는다:

```cpp
constexpr const char* IsNan = R"(fn aps5_is_nan(value: f32) -> bool {
    return (bitcast<u32>(value) & 0x7fffffffu) > 0x7f800000u;
}
)";
```

`emit()`의 `binary` 람다 정의 바로 뒤에 넣는다:

```cpp
        const auto ordered = [&](const char* operation) {
            helper(IsNan);
            define(inst, "(!aps5_is_nan(" + argument(inst, 0) + ") && !aps5_is_nan(" + argument(inst, 1) + ") && (" + argument(inst, 0) + " " + operation + " " + argument(inst, 1) + "))");
        };
        const auto unordered = [&](const char* operation) {
            helper(IsNan);
            define(inst, "(aps5_is_nan(" + argument(inst, 0) + ") || aps5_is_nan(" + argument(inst, 1) + ") || (" + argument(inst, 0) + " " + operation + " " + argument(inst, 1) + "))");
        };
```

`case IrOpcode::FPOrdEqual32: return binary("==");` 한 줄을 다음으로 바꾼다:

```cpp
        case IrOpcode::FPOrdEqual32: return ordered("==");
        case IrOpcode::FPOrdNotEqual32: return ordered("!=");
        case IrOpcode::FPOrdLessThan32: return ordered("<");
        case IrOpcode::FPOrdGreaterThan32: return ordered(">");
        case IrOpcode::FPOrdLessThanEqual32: return ordered("<=");
        case IrOpcode::FPOrdGreaterThanEqual32: return ordered(">=");
        case IrOpcode::FPUnordEqual32: return unordered("==");
        case IrOpcode::FPUnordNotEqual32: return unordered("!=");
        case IrOpcode::FPUnordLessThan32: return unordered("<");
        case IrOpcode::FPUnordGreaterThan32: return unordered(">");
        case IrOpcode::FPUnordLessThanEqual32: return unordered("<=");
        case IrOpcode::FPUnordGreaterThanEqual32: return unordered(">=");
        case IrOpcode::FPIsNan32:
            helper(IsNan);
            define(inst, "aps5_is_nan(" + argument(inst, 0) + ")");
            return;
```

- [ ] **Step 4: 셰이더 검증**

```bash
cmake --build build-shader-tools -j16
cmake --build build-wasm-linux --target anyps5-runtime-node anyps5-runtime-web -j16
```

Step 1의 캡처를 다시 실행한다. Expected: `capture of ... failed` 줄이 없고 `captured .../106791d00-106c96b00`이 있다. 그다음:

```bash
rm -rf $CHECKS/wgsl && mkdir -p $CHECKS/wgsl
build-shader-tools/core/shader/recompiler/WgslBackend/tools/anyps5-wgsl -o $CHECKS/wgsl $(ls $CHECKS/capture/*.ps.b64 | sed 's/\.ps\.b64$//')
for f in $CHECKS/wgsl/*.wgsl; do naga "$f" >/dev/null || echo "FAIL $f"; done
sudo -n -u $USER -g render env VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/intel_icd.json tools/wgsl-check/target/release/anyps5-wgsl-check $(ls $CHECKS/wgsl/*.vs.wgsl | sed 's/\.vs\.wgsl$//')
sudo -n -u $USER -g render env VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/intel_icd.json tools/wgsl-check/target/release/anyps5-wgsl-check $(ls build-wasm-aot/dreaming-sarah/wgsl-final/*.vs.wgsl | sed 's/\.vs\.wgsl$//')
grep -c aps5_is_nan $CHECKS/wgsl/106791d00-106c96b00.ps.wgsl
```

Expected: `FAIL` 줄 없음. 두 `anyps5-wgsl-check`가 `adapter: Intel(R) UHD Graphics` 다음에 모든 줄 `OK`. 마지막 grep이 1 이상.

- [ ] **Step 5: 브라우저 확인 (M2)**

```bash
$HEADLESS http://127.0.0.1:8080/ "" 60000 "" --keys 10500:Enter,16000:Enter,45000:ArrowRight:2000,48000:Space:300 --snap 40000:$CHECKS/m2-scene.png,47500:$CHECKS/m2-walk.png,48400:$CHECKS/m2-jump.png 2>&1 | grep -E "skipped|webgpu\] error|pageerror|Go to layout|snap"
```

Expected:
- `skipped`, `[webgpu] error`, `pageerror` 줄이 없다.
- `m2-scene.png`에 플레이어(Sarah) 스프라이트가 보이고, `m2-walk.png`에서 위치나 화면이 오른쪽으로 바뀌어 있고, `m2-jump.png`에서 점프 중이다.

새 `skipped` 줄이 나오면 멈추고 그 메시지와 화면을 12절에 적은 뒤 사용자에게 보고한다 (spec 7절: 같은 방식으로 찾아 고치되, 양이 크면 범위를 다시 정한다).

- [ ] **Step 6: 진행 상황 기록**

spec 7절의 조사 항목 아래에 "구현 결과"를, 12절 표에 M2 행을 더한다:

```markdown
| M2 | 완료 | `R16_SFLOAT`·`R16G16_SFLOAT` 텍스처, WGSL 부동소수 비교 명령 묶음. 첫 장면 skip 없음, Sarah 보임, 걷기·점프 확인 |
```

- [ ] **Step 7: Commit** (저장소가 복구된 경우에만)

```bash
git add core/wasm-runtime/gpu/WebGpuBackend.cpp core/shader/recompiler/WgslBackend/src/WgslEmitter.cpp docs/superpowers/specs/2026-10-16-wasm-sp4-playable-design.md
git commit -m "feat(wgsl): emit float comparisons and accept 16-bit float textures"
```

---

### Task 5: 오디오 링 (C++)

**Files:**
- Create: `core/wasm-runtime/platform/AudioRing.hpp`
- Modify: `core/wasm-runtime/tests/PlatformTests.cpp`

**Interfaces:**
- Consumes: Task 1 `WasmAudio::Samples`.
- Produces:
  - `WasmPlatform::AudioRingFrames`(8192), `AudioRings`(4), `SurroundLevel`, `struct AudioRing`, `struct AudioState`
  - `std::array<float, 2> DownmixFrame(const WasmAudio::Samples&, std::uint32_t frame)` (채널 1·2·8 외에는 `std::invalid_argument`)
  - `int OpenRing(AudioState&)` (빈 링이 없으면 -1), `AudioRing& RingAt(AudioState&, int stream)` (닫힌 스트림은 `std::out_of_range`)
  - `std::uint32_t QueuedFrames(const AudioRing&)`, `std::uint32_t WriteFrames(AudioRing&, const WasmAudio::Samples&)`

- [ ] **Step 1: 실패하는 테스트 작성**

`PlatformTests.cpp`의 include에 `#include "AudioRing.hpp"`, `#include <cmath>`, `#include <cstdint>`, `#include <memory>`, `#include <stdexcept>`, `#include <vector>`를 더하고, 익명 namespace 안 `TestNeutralSlot` 뒤에 넣는다:

```cpp
bool Near(float value, float expected) {
    return std::fabs(value - expected) < 1e-5f;
}

template<typename TFunction>
bool Throws(TFunction function) {
    try {
        function();
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

void TestDownmixMonoAndStereo() {
    const float mono[] = {0.25f};
    const auto monoFrame = WasmPlatform::DownmixFrame({mono, true, 1, 1}, 0);
    Require(monoFrame[0] == 0.25f && monoFrame[1] == 0.25f);
    const float stereo[] = {0.5f, -0.25f, 0.125f, 0.75f};
    const auto second = WasmPlatform::DownmixFrame({stereo, true, 2, 2}, 1);
    Require(second[0] == 0.125f && second[1] == 0.75f);
}

void TestDownmixEightChannels() {
    const float frame[] = {0.1f, 0.2f, 0.3f, 0.9f, 0.1f, 0.2f, 0.1f, 0.0f};
    const auto mixed = WasmPlatform::DownmixFrame({frame, true, 8, 1}, 0);
    Require(Near(mixed[0], 0.1f + WasmPlatform::SurroundLevel * 0.5f));
    Require(Near(mixed[1], 0.2f + WasmPlatform::SurroundLevel * 0.5f));
}

void TestDownmixIntegersAndClamp() {
    const std::int16_t integers[] = {16384, -32768};
    const auto converted = WasmPlatform::DownmixFrame({integers, false, 2, 1}, 0);
    Require(converted[0] == 0.5f && converted[1] == -1.0f);
    const float loud[] = {0.9f, -0.9f, 0.9f, 0.0f, 0.9f, -0.9f, 0.9f, -0.9f};
    const auto clamped = WasmPlatform::DownmixFrame({loud, true, 8, 1}, 0);
    Require(clamped[0] == 1.0f && clamped[1] == -1.0f);
}

void TestDownmixRejectsOtherLayouts() {
    const float six[6] = {};
    Require(Throws([&] { WasmPlatform::DownmixFrame({six, true, 6, 1}, 0); }));
}

void TestRingsOpenAndClose() {
    auto state = std::make_unique<WasmPlatform::AudioState>();
    for (int index = 0; index < static_cast<int>(WasmPlatform::AudioRings); ++index) Require(WasmPlatform::OpenRing(*state) == index);
    Require(WasmPlatform::OpenRing(*state) == -1);
    state->rings[2].active.store(0);
    state->rings[2].read.store(100);
    Require(WasmPlatform::OpenRing(*state) == 2);
    Require(state->rings[2].write.load() == 100 && WasmPlatform::QueuedFrames(state->rings[2]) == 0);
    Require(Throws([&] { WasmPlatform::RingAt(*state, 7); }));
    state->rings[1].active.store(0);
    Require(Throws([&] { WasmPlatform::RingAt(*state, 1); }));
    Require(&WasmPlatform::RingAt(*state, 3) == &state->rings[3]);
}

void TestRingQueuesUpToItsSize() {
    auto state = std::make_unique<WasmPlatform::AudioState>();
    auto& ring = state->rings[static_cast<std::size_t>(WasmPlatform::OpenRing(*state))];
    std::vector<float> block(2 * WasmPlatform::AudioRingFrames, 0.5f);
    Require(WasmPlatform::WriteFrames(ring, {block.data(), true, 2, 256}) == 256);
    Require(WasmPlatform::QueuedFrames(ring) == 256);
    ring.read.store(200);
    Require(WasmPlatform::QueuedFrames(ring) == 56);
    Require(WasmPlatform::WriteFrames(ring, {block.data(), true, 2, WasmPlatform::AudioRingFrames}) == WasmPlatform::AudioRingFrames - 56);
    Require(WasmPlatform::QueuedFrames(ring) == WasmPlatform::AudioRingFrames);
    Require(WasmPlatform::WriteFrames(ring, {block.data(), true, 2, 1}) == 0);
}

void TestRingWritesAcrossItsEnd() {
    auto state = std::make_unique<WasmPlatform::AudioState>();
    auto& ring = state->rings[static_cast<std::size_t>(WasmPlatform::OpenRing(*state))];
    ring.read.store(WasmPlatform::AudioRingFrames - 2);
    ring.write.store(WasmPlatform::AudioRingFrames - 2);
    const float frames[] = {0.125f, 0.25f, 0.375f, 0.5f, 0.625f, 0.75f};
    Require(WasmPlatform::WriteFrames(ring, {frames, true, 2, 3}) == 3);
    Require(ring.samples[2 * (WasmPlatform::AudioRingFrames - 2)] == 0.125f);
    Require(ring.samples[2 * (WasmPlatform::AudioRingFrames - 1) + 1] == 0.5f);
    Require(ring.samples[0] == 0.625f && ring.samples[1] == 0.75f);
    Require(ring.write.load() == WasmPlatform::AudioRingFrames + 1);
}
```

`main()`에 호출을 더한다:

```cpp
    TestDownmixMonoAndStereo();
    TestDownmixEightChannels();
    TestDownmixIntegersAndClamp();
    TestDownmixRejectsOtherLayouts();
    TestRingsOpenAndClose();
    TestRingQueuesUpToItsSize();
    TestRingWritesAcrossItsEnd();
```

- [ ] **Step 2: 실패 확인**

```bash
cmake --build build-wasm-linux --target anyps5-platform-tests -j16
```

Expected: 컴파일 실패 `'AudioRing.hpp' file not found`.

- [ ] **Step 3: 구현**

`core/wasm-runtime/platform/AudioRing.hpp`:

```cpp
#ifndef CORE_WASM_RUNTIME_PLATFORM_AUDIORING_HPP
#define CORE_WASM_RUNTIME_PLATFORM_AUDIORING_HPP

#include "wasm-runtime/Platform.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace WasmPlatform {

inline constexpr std::uint32_t AudioRingFrames = 8192;
inline constexpr std::uint32_t AudioRings = 4;
inline constexpr float SurroundLevel = 0.70710678f;

struct AudioRing {
    std::atomic<std::uint32_t> active{0};
    std::atomic<std::uint32_t> write{0};
    std::atomic<std::uint32_t> read{0};
    std::atomic<std::uint32_t> underruns{0};
    std::atomic<std::uint32_t> consumed{0};
    std::array<std::uint32_t, 3> reserved{};
    std::array<float, AudioRingFrames * 2> samples{};
};

struct AudioState {
    std::atomic<std::uint32_t> running{0};
    std::array<std::uint32_t, 7> reserved{};
    std::array<AudioRing, AudioRings> rings{};
};

static_assert(offsetof(AudioRing, samples) == 32);
static_assert(sizeof(AudioRing) == 32 + AudioRingFrames * 2 * sizeof(float));
static_assert(offsetof(AudioState, rings) == 32);

inline float SampleAt(const WasmAudio::Samples& samples, std::uint32_t index) {
    if (samples.isFloat) return static_cast<const float*>(samples.data)[index];
    return static_cast<float>(static_cast<const std::int16_t*>(samples.data)[index]) / 32768.0f;
}

inline std::array<float, 2> DownmixFrame(const WasmAudio::Samples& samples, std::uint32_t frame) {
    const auto base = frame * samples.channels;
    const auto at = [&](std::uint32_t channel) { return SampleAt(samples, base + channel); };
    float left = 0.0f;
    float right = 0.0f;
    switch (samples.channels) {
    case 1:
        left = right = at(0);
        break;
    case 2:
        left = at(0);
        right = at(1);
        break;
    case 8:
        left = at(0) + SurroundLevel * (at(2) + at(4) + at(6));
        right = at(1) + SurroundLevel * (at(2) + at(5) + at(7));
        break;
    default:
        throw std::invalid_argument("WasmAudio: " + std::to_string(samples.channels) + " channels cannot be mixed to stereo");
    }
    return {std::clamp(left, -1.0f, 1.0f), std::clamp(right, -1.0f, 1.0f)};
}

inline int OpenRing(AudioState& state) {
    for (std::uint32_t index = 0; index < AudioRings; ++index) {
        auto& ring = state.rings[index];
        if (ring.active.load(std::memory_order_acquire) != 0) continue;
        ring.write.store(ring.read.load(std::memory_order_acquire), std::memory_order_release);
        ring.active.store(1, std::memory_order_release);
        return static_cast<int>(index);
    }
    return -1;
}

inline AudioRing& RingAt(AudioState& state, int stream) {
    if (stream < 0 || static_cast<std::uint32_t>(stream) >= AudioRings || state.rings[static_cast<std::size_t>(stream)].active.load(std::memory_order_acquire) == 0) {
        throw std::out_of_range("WasmAudio: stream " + std::to_string(stream) + " is not open");
    }
    return state.rings[static_cast<std::size_t>(stream)];
}

inline std::uint32_t QueuedFrames(const AudioRing& ring) {
    return ring.write.load(std::memory_order_acquire) - ring.read.load(std::memory_order_acquire);
}

inline std::uint32_t WriteFrames(AudioRing& ring, const WasmAudio::Samples& samples) {
    const auto write = ring.write.load(std::memory_order_relaxed);
    const auto room = AudioRingFrames - (write - ring.read.load(std::memory_order_acquire));
    const auto frames = std::min(samples.frames, room);
    for (std::uint32_t frame = 0; frame < frames; ++frame) {
        const auto mixed = DownmixFrame(samples, frame);
        const auto slot = ((write + frame) % AudioRingFrames) * 2;
        ring.samples[slot] = mixed[0];
        ring.samples[slot + 1] = mixed[1];
    }
    ring.write.store(write + frames, std::memory_order_release);
    return frames;
}

}  // namespace WasmPlatform

#endif
```

- [ ] **Step 4: 통과 확인**

```bash
cmake --build build-wasm-linux --target anyps5-platform-tests -j16
ctest --test-dir build-wasm-linux -R wasm_platform --output-on-failure
```

Expected: Passed.

- [ ] **Step 5: Commit** (저장소가 복구된 경우에만)

```bash
git add core/wasm-runtime/platform/AudioRing.hpp core/wasm-runtime/tests/PlatformTests.cpp
git commit -m "feat(wasm-runtime): add the stereo audio ring shared with the browser"
```

---

### Task 6: AudioWorklet 믹서 (JS)

**Files:**
- Create: `core/wasm-runtime/web/audio-mix.mjs`
- Create: `core/wasm-runtime/web/audio-worklet.mjs`
- Create: `core/wasm-runtime/web/tests/audio-mix.test.mjs`

**Interfaces:**
- Consumes: Global Constraints의 오디오 상태 형식 (Task 5의 `AudioState`와 같은 바이트 배치).
- Produces:
  - `audio-mix.mjs`: `AudioRingFrames`, `AudioRings`, `AudioHeaderBytes`, `AudioRingBytes`, `class AudioMixer { constructor(buffer, address); state: Uint32Array; mix(left: Float32Array, right: Float32Array); stats(): { running, active, consumed, underruns } }`
  - `audio-worklet.mjs`: 프로세서 이름 `'aps5-audio'`. 포트 메시지 `{ buffer, address }`를 받으면 `'attached'`로 답한다.

- [ ] **Step 1: 실패하는 테스트 작성**

`core/wasm-runtime/web/tests/audio-mix.test.mjs`:

```js
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { AudioMixer, AudioRingFrames, AudioRings, AudioHeaderBytes, AudioRingBytes } from '../audio-mix.mjs';

const address = 128;

function setup() {
  const buffer = new SharedArrayBuffer(address + AudioHeaderBytes + AudioRings * AudioRingBytes);
  const ring = (index) => {
    const base = address + AudioHeaderBytes + index * AudioRingBytes;
    return { header: new Uint32Array(buffer, base, AudioHeaderBytes / 4), samples: new Float32Array(buffer, base + AudioHeaderBytes, AudioRingFrames * 2) };
  };
  return { buffer, ring, mixer: new AudioMixer(buffer, address) };
}

function produce(ring, frames) {
  const write = ring.header[1];
  frames.forEach(([left, right], index) => {
    const slot = ((write + index) % AudioRingFrames) * 2;
    ring.samples[slot] = left;
    ring.samples[slot + 1] = right;
  });
  ring.header[1] = (write + frames.length) >>> 0;
}

function output(frames) {
  return [new Float32Array(frames), new Float32Array(frames)];
}

test('the byte layout matches the runtime', () => {
  assert.equal(AudioHeaderBytes, 32);
  assert.equal(AudioRingBytes, 32 + 8192 * 8);
});

test('inactive rings are silent and untouched', () => {
  const { ring, mixer } = setup();
  produce(ring(0), [[0.5, 0.5]]);
  const [left, right] = output(4);
  left.fill(1);
  mixer.mix(left, right);
  assert.deepEqual([...left, ...right], [0, 0, 0, 0, 0, 0, 0, 0]);
  assert.equal(ring(0).header[2], 0);
  assert.equal(ring(0).header[3], 0);
});

test('mixes queued frames and advances the read index', () => {
  const { ring, mixer } = setup();
  ring(0).header[0] = 1;
  produce(ring(0), [[0.5, -0.5], [0, 0], [0.25, 0.75], [-0.25, 0]]);
  const [left, right] = output(4);
  mixer.mix(left, right);
  assert.deepEqual([...left], [0.5, 0, 0.25, -0.25]);
  assert.deepEqual([...right], [-0.5, 0, 0.75, 0]);
  assert.equal(ring(0).header[2], 4);
  assert.equal(ring(0).header[3], 0);
  assert.equal(ring(0).header[4], 3);
});

test('counts an underrun when a ring runs dry', () => {
  const { ring, mixer } = setup();
  ring(1).header[0] = 1;
  produce(ring(1), [[0.5, 0.5], [0.5, 0.5]]);
  const [left, right] = output(4);
  mixer.mix(left, right);
  assert.deepEqual([...left], [0.5, 0.5, 0, 0]);
  assert.equal(ring(1).header[2], 2);
  assert.equal(ring(1).header[3], 1);
});

test('sums rings and clamps', () => {
  const { ring, mixer } = setup();
  ring(0).header[0] = 1;
  ring(3).header[0] = 1;
  produce(ring(0), [[0.75, -0.75]]);
  produce(ring(3), [[0.5, -0.5]]);
  const [left, right] = output(1);
  mixer.mix(left, right);
  assert.deepEqual([left[0], right[0]], [1, -1]);
});

test('reads across the end of the ring', () => {
  const { ring, mixer } = setup();
  const target = ring(2);
  target.header[0] = 1;
  target.header[1] = AudioRingFrames - 1;
  target.header[2] = AudioRingFrames - 1;
  produce(target, [[0.25, 0.25], [0.5, 0.5]]);
  const [left, right] = output(2);
  mixer.mix(left, right);
  assert.deepEqual([...left], [0.25, 0.5]);
  assert.equal(target.header[2], AudioRingFrames + 1);
});

test('stats sum the ring counters', () => {
  const { buffer, ring, mixer } = setup();
  new Uint32Array(buffer, address, 1)[0] = 1;
  ring(0).header[0] = 1;
  ring(0).header[3] = 2;
  ring(0).header[4] = 10;
  ring(1).header[3] = 1;
  ring(1).header[4] = 5;
  assert.deepEqual(mixer.stats(), { running: 1, active: 1, consumed: 15, underruns: 3 });
});
```

- [ ] **Step 2: 실패 확인**

```bash
ctest --test-dir build-wasm-linux -R wasm_web_scripts --output-on-failure
```

Expected: FAIL, `Cannot find module '.../audio-mix.mjs'`.

- [ ] **Step 3: 구현**

`core/wasm-runtime/web/audio-mix.mjs`:

```js
export const AudioRingFrames = 8192;
export const AudioRings = 4;
export const AudioHeaderBytes = 32;
export const AudioRingBytes = AudioHeaderBytes + AudioRingFrames * 2 * 4;

const Running = 0;
const Active = 0;
const Write = 1;
const Read = 2;
const Underruns = 3;
const Consumed = 4;

export class AudioMixer {
  constructor(buffer, address) {
    this.state = new Uint32Array(buffer, address, AudioHeaderBytes / 4);
    this.rings = [];
    for (let index = 0; index < AudioRings; index += 1) {
      const base = address + AudioHeaderBytes + index * AudioRingBytes;
      this.rings.push({ header: new Uint32Array(buffer, base, AudioHeaderBytes / 4), samples: new Float32Array(buffer, base + AudioHeaderBytes, AudioRingFrames * 2) });
    }
  }

  mix(left, right) {
    left.fill(0);
    right.fill(0);
    for (const ring of this.rings) {
      if (Atomics.load(ring.header, Active) === 0) continue;
      const read = Atomics.load(ring.header, Read);
      const available = (Atomics.load(ring.header, Write) - read) >>> 0;
      const frames = Math.min(available, left.length);
      let audible = 0;
      for (let frame = 0; frame < frames; frame += 1) {
        const slot = ((read + frame) % AudioRingFrames) * 2;
        const leftSample = ring.samples[slot];
        const rightSample = ring.samples[slot + 1];
        left[frame] += leftSample;
        right[frame] += rightSample;
        if (leftSample !== 0 || rightSample !== 0) audible += 1;
      }
      Atomics.store(ring.header, Read, (read + frames) >>> 0);
      if (audible > 0) Atomics.add(ring.header, Consumed, audible);
      if (frames < left.length) Atomics.add(ring.header, Underruns, 1);
    }
    for (let frame = 0; frame < left.length; frame += 1) {
      left[frame] = Math.max(-1, Math.min(1, left[frame]));
      right[frame] = Math.max(-1, Math.min(1, right[frame]));
    }
  }

  stats() {
    let active = 0;
    let consumed = 0;
    let underruns = 0;
    for (const ring of this.rings) {
      if (Atomics.load(ring.header, Active) !== 0) active += 1;
      consumed += Atomics.load(ring.header, Consumed);
      underruns += Atomics.load(ring.header, Underruns);
    }
    return { running: Atomics.load(this.state, Running), active, consumed, underruns };
  }
}
```

`core/wasm-runtime/web/audio-worklet.mjs`:

```js
import { AudioMixer } from './audio-mix.mjs';

class Aps5AudioProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.mixer = null;
    this.port.onmessage = (event) => {
      this.mixer = new AudioMixer(event.data.buffer, event.data.address);
      this.port.postMessage('attached');
    };
  }

  process(inputs, outputs) {
    const [left, right] = outputs[0];
    if (this.mixer === null) {
      left.fill(0);
      right.fill(0);
    } else {
      this.mixer.mix(left, right);
    }
    return true;
  }
}

registerProcessor('aps5-audio', Aps5AudioProcessor);
```

- [ ] **Step 4: 통과 확인**

```bash
ctest --test-dir build-wasm-linux -R wasm_web_scripts --output-on-failure
```

Expected: Passed.

- [ ] **Step 5: Commit** (저장소가 복구된 경우에만)

```bash
git add core/wasm-runtime/web/audio-mix.mjs core/wasm-runtime/web/audio-worklet.mjs core/wasm-runtime/web/tests/audio-mix.test.mjs
git commit -m "feat(wasm-runtime): mix the shared audio rings in an AudioWorklet"
```

---

### Task 7: 오디오 연결 (M3)

**Files:**
- Create: `core/wasm-runtime/platform/WebAudio.cpp`
- Create: `core/wasm-runtime/web/audio.mjs`
- Modify: `core/libs/prx/libSceAudioOut/src/AudioOut.cpp` (`__EMSCRIPTEN__` 분기 3곳)
- Modify: `core/wasm-runtime/CMakeLists.txt` (`aps5_wasm_web_platform`에 `WebAudio.cpp`)
- Modify: `core/wasm-runtime/web/index.html`
- Modify: `docs/superpowers/specs/2026-10-16-wasm-sp4-playable-design.md` (12절)

**Interfaces:**
- Consumes: Task 5 `OpenRing`, `RingAt`, `QueuedFrames`, `WriteFrames`, `AudioState`; Task 6 `AudioMixer`, 프로세서 `'aps5-audio'`.
- Produces: 웹 런타임의 `WasmAudio::*`. `audio.mjs`: `startAudio(target): { attach(buffer, address): Promise<void>, stats(): { state, running, active, consumed, underruns } }`. `aps5Platform.attachAudio`, `aps5Platform.audioStats`.

- [ ] **Step 1: `WebAudio.cpp`**

```cpp
#include "AudioRing.hpp"
#include "wasm-runtime/Platform.hpp"
#include <emscripten/em_asm.h>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>

namespace {

WasmPlatform::AudioState state;
std::mutex streams;

}  // namespace

int WasmAudio::Open(std::uint32_t frequency) {
    if (frequency != Frequency) {
        throw std::runtime_error("WasmAudio: " + std::to_string(frequency) + " Hz output is not supported, the browser stream runs at " + std::to_string(Frequency) + " Hz");
    }
    static std::once_flag attached;
    std::call_once(attached, [] {
        MAIN_THREAD_ASYNC_EM_ASM({ globalThis.aps5Platform.attachAudio(wasmMemory.buffer, $0); }, static_cast<double>(reinterpret_cast<std::uintptr_t>(&state)));
    });
    std::lock_guard lock(streams);
    const int stream = WasmPlatform::OpenRing(state);
    if (stream < 0) throw std::runtime_error("WasmAudio: all " + std::to_string(WasmPlatform::AudioRings) + " browser audio streams are open");
    return stream;
}

bool WasmAudio::Running() {
    return state.running.load(std::memory_order_acquire) != 0;
}

std::uint32_t WasmAudio::Queued(int stream) {
    return WasmPlatform::QueuedFrames(WasmPlatform::RingAt(state, stream));
}

std::uint32_t WasmAudio::Write(int stream, const Samples& samples) {
    return WasmPlatform::WriteFrames(WasmPlatform::RingAt(state, stream), samples);
}

void WasmAudio::Close(int stream) {
    std::lock_guard lock(streams);
    WasmPlatform::RingAt(state, stream).active.store(0, std::memory_order_release);
}
```

`core/wasm-runtime/CMakeLists.txt`의 `aps5_wasm_web_platform`을 다음으로 바꾼다:

```cmake
add_library(aps5_wasm_web_platform OBJECT platform/WebInput.cpp platform/WebAudio.cpp)
```

- [ ] **Step 2: `AudioOut.cpp`의 웹 분기**

include 블록을 바꾼다:

```cpp
#ifndef __EMSCRIPTEN__
#include "prx/libSceAudioOut/include/AudioOutSdl.hpp"
#else
#include "wasm-runtime/Platform.hpp"
#endif
```

`#ifdef __EMSCRIPTEN__`의 `openDevice`/`closeDevice`(지금은 `return false`와 초기화만 한다)를 바꾼다:

```cpp
#ifdef __EMSCRIPTEN__
static bool openDevice(Port& port) {
    const int stream = WasmAudio::Open(port.freq);
    if (stream < 0) return false;
    port.device = static_cast<AudioDeviceId>(stream) + 1;
    return true;
}

static void closeDevice(Port& port) {
    if (port.device != 0) WasmAudio::Close(static_cast<int>(port.device) - 1);
    port.device = 0;
    port.spec = {};
}
#else
```

`static void queueAudio(...)` 정의 바로 앞에 선언을 넣는다:

```cpp
static void paceVirtualPort(Port& port, bool hasData, std::unique_lock<std::mutex>& lock);
```

`queueAudio`의 `#ifdef __EMSCRIPTEN__` 부분(`static_cast<void>(port);`부터 `throw std::logic_error(...)`까지)을 바꾼다:

```cpp
#ifdef __EMSCRIPTEN__
    const int stream = static_cast<int>(port.device) - 1;
    if (!WasmAudio::Running()) {
        paceVirtualPort(port, data != nullptr, lock);
        return;
    }
    const std::uint32_t limit = data == nullptr ? 0u : static_cast<std::uint32_t>(TARGET_LATENCY_US * WasmAudio::Frequency / 1000000ULL);
    const std::uint64_t waitStart = sceKernelGetProcessTime();
    while (WasmAudio::Queued(stream) > limit && sceKernelGetProcessTime() - waitStart <= DRAIN_TIMEOUT_US) {
        sleepUs(lock, DRAIN_SLEEP_US);
    }
    if (data == nullptr) return;
    std::vector<std::uint8_t> prepareBuf;
    const void* prepared = prepareBuffer(port, data, prepareBuf);
    WasmAudio::Write(stream, {prepared, formatIsFloat(port.format), static_cast<std::uint32_t>(port.channels), port.samplesNum});
#else
```

- [ ] **Step 3: `audio.mjs`와 페이지**

`core/wasm-runtime/web/audio.mjs`:

```js
import { AudioMixer } from './audio-mix.mjs';

export function startAudio(target) {
  const context = new AudioContext({ sampleRate: 48000, latencyHint: 'interactive' });
  const node = context.audioWorklet.addModule(new URL('./audio-worklet.mjs', import.meta.url)).then(() => {
    const created = new AudioWorkletNode(context, 'aps5-audio', { numberOfInputs: 0, outputChannelCount: [2] });
    created.connect(context.destination);
    return created;
  });
  let mixer = null;
  let attached = false;
  const publish = () => {
    if (mixer !== null) Atomics.store(mixer.state, 0, attached && context.state === 'running' ? 1 : 0);
  };
  context.addEventListener('statechange', publish);
  const resume = () => {
    if (context.state === 'suspended') context.resume().catch((error) => console.log('anyps5 audio: ' + error));
  };
  target.addEventListener('pointerdown', resume);
  target.addEventListener('keydown', resume);
  return {
    async attach(buffer, address) {
      mixer = new AudioMixer(buffer, address);
      const worklet = await node;
      worklet.port.onmessage = (event) => {
        if (event.data !== 'attached') return;
        attached = true;
        publish();
      };
      worklet.port.postMessage({ buffer, address });
    },
    stats() {
      const counters = mixer === null ? { running: 0, active: 0, consumed: 0, underruns: 0 } : mixer.stats();
      return { state: context.state, ...counters };
    },
  };
}
```

`index.html`의 모듈 스크립트 머리 부분을 다음으로 바꾼다 (`load` 함수와 그 아래 `(async () => {…})()`는 그대로):

```js
import { startInput } from './input.mjs';
import { startAudio } from './audio.mjs';
```

```js
const input = startInput(window);
const audio = startAudio(window);
globalThis.aps5Platform = {
  attachInput: (buffer, address) => input.attach(buffer, address),
  attachAudio: (buffer, address) => audio.attach(buffer, address).catch((error) => console.log('anyps5 audio: ' + (error.stack || error))),
  audioStats: () => audio.stats(),
};
```

- [ ] **Step 4: 빌드와 회귀**

```bash
cmake --build build-wasm-linux --target anyps5-runtime-web anyps5-runtime-node anyps5-platform-tests -j16
ctest --test-dir build-wasm-linux -R wasm_ --output-on-failure
cd build-wasm-aot/dreaming-sarah/linux && timeout 30 $NODE ../../../build-wasm-linux/core/wasm-runtime/anyps5-runtime-node.js ../game.wasm 2>&1 | grep -E "^\[gpu\] present 1:|anyps5 runtime"; cd -
```

Expected: 테스트 Passed. Node에 `[gpu] present 1:`이 있고 오류 줄이 없다 (Node는 `NullPlatform`이라 가상 포트로 남는다).

- [ ] **Step 5: 브라우저 확인 (M3)**

```bash
$HEADLESS http://127.0.0.1:8080/ "" 50000 "" --audio --autoplay --keys 10500:Enter,16000:Enter --snap 30000:$CHECKS/m3-a.png,45000:$CHECKS/m3-b.png 2>&1 | grep -E "\[audio\]|anyps5 audio|pageerror|Go to layout"
```

Expected:
- `anyps5 audio`, `pageerror` 줄이 없다.
- `[audio]` 줄에 `"state":"running"`, `"running":1`, `"active":1`.
- 30초와 45초 줄 사이에 `consumed`가 늘어나고 (게임 안 음악 `introtest`/`m0`), `underruns`는 그대로다.

`underruns`가 늘면 두 값과 그 사이의 콘솔 로그(리소스 로딩 등)를 12절에 적고, 생산 쪽 대기(`Queued` 1920프레임)가 원인인지 확인한 뒤 사용자에게 보고한다.

- [ ] **Step 6: 첫 입력 전에도 게임이 진행되는지 (Review Focus 2)**

```bash
$HEADLESS http://127.0.0.1:8080/ "present 512:" 40000 "" --audio 2>&1 | grep -E "\[audio\]|present 512:"
```

Expected: `present 512:`이 나온다 (자동 재생 없이도 게임이 멈추지 않는다). 마지막 `[audio]` 줄은 `"state":"suspended"`, `"running":0`.

- [ ] **Step 7: 진행 상황 기록**

12절 표에 더한다:

```markdown
| M3 | 완료 | 헤드리스 Chromium(자동 재생 허용)에서 worklet 소비 프레임이 30→45초 사이 늘고 언더런이 늘지 않음. 자동 재생 없이도 present 512까지 진행 |
```

- [ ] **Step 8: Commit** (저장소가 복구된 경우에만)

```bash
git add core/wasm-runtime core/libs/prx/libSceAudioOut/src/AudioOut.cpp docs/superpowers/specs/2026-10-16-wasm-sp4-playable-design.md
git commit -m "feat(wasm-runtime): play libSceAudioOut ports through Web Audio"
```

---

### Task 8: 영구 세이브 (M4)

**Files:**
- Create: `core/wasm-runtime/web/storage.mjs`
- Create: `core/wasm-runtime/web/tests/storage.test.mjs`
- Create: `core/wasm-runtime/platform/WebStorage.cpp`
- Modify: `core/libs/prx/libSceSaveData.native/Export.cpp` (`sceSaveDataUmount2`, `sceSaveDataDelete`)
- Modify: `core/wasm-runtime/CMakeLists.txt` (`WebStorage.cpp`, `-lidbfs.js`)
- Modify: `core/wasm-runtime/web/index.html`
- Modify: `docs/superpowers/specs/2026-10-16-wasm-sp4-playable-design.md` (12절)

**Interfaces:**
- Consumes: Task 1 `WasmStorage::Commit()`.
- Produces: `storage.mjs`: `mountStorage(fs, idbfs, path, dependencies: { add(id), remove(id) }, log)`, `createStorage(fs, log): { commit() }`. `aps5Platform.commitStorage()`. 콘솔 줄 `anyps5 storage: saved`.

- [ ] **Step 1: 실패하는 테스트 작성**

`core/wasm-runtime/web/tests/storage.test.mjs`:

```js
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createStorage, mountStorage } from '../storage.mjs';

class FakeFs {
  constructor() {
    this.directories = [];
    this.mounts = [];
    this.syncs = [];
    this.callbacks = [];
  }

  mkdir(path) {
    this.directories.push(path);
  }

  mount(type, options, path) {
    this.mounts.push({ type, path });
  }

  syncfs(populate, callback) {
    this.syncs.push(populate);
    this.callbacks.push(callback);
  }

  finish(error = null) {
    this.callbacks.shift()(error);
  }
}

function recorder() {
  const lines = [];
  return { lines, log: (text) => lines.push(text) };
}

test('mounting loads the stored files before the game starts', () => {
  const fs = new FakeFs();
  const idbfs = {};
  const events = [];
  const { lines, log } = recorder();
  mountStorage(fs, idbfs, '/_sd', { add: (id) => events.push('add ' + id), remove: (id) => events.push('remove ' + id) }, log);
  assert.deepEqual(fs.directories, ['/_sd']);
  assert.deepEqual(fs.mounts, [{ type: idbfs, path: '/_sd' }]);
  assert.deepEqual(fs.syncs, [true]);
  assert.deepEqual(events, ['add aps5-storage']);
  fs.finish();
  assert.deepEqual(events, ['add aps5-storage', 'remove aps5-storage']);
  assert.deepEqual(lines, []);
});

test('a failed load is logged and the game still starts', () => {
  const fs = new FakeFs();
  const events = [];
  const { lines, log } = recorder();
  mountStorage(fs, {}, '/_sd', { add: (id) => events.push('add ' + id), remove: (id) => events.push('remove ' + id) }, log);
  fs.finish(new Error('blocked'));
  assert.deepEqual(events, ['add aps5-storage', 'remove aps5-storage']);
  assert.deepEqual(lines, ['anyps5 storage: blocked']);
});

test('commits run one at a time and a request during a commit runs once more', () => {
  const fs = new FakeFs();
  const { lines, log } = recorder();
  const storage = createStorage(fs, log);
  storage.commit();
  storage.commit();
  storage.commit();
  assert.deepEqual(fs.syncs, [false]);
  fs.finish();
  assert.deepEqual(fs.syncs, [false, false]);
  fs.finish();
  assert.deepEqual(fs.syncs, [false, false]);
  assert.deepEqual(lines, ['anyps5 storage: saved', 'anyps5 storage: saved']);
});

test('a failed commit is logged and the next one still runs', () => {
  const fs = new FakeFs();
  const { lines, log } = recorder();
  const storage = createStorage(fs, log);
  storage.commit();
  fs.finish(new Error('quota exceeded'));
  storage.commit();
  fs.finish();
  assert.deepEqual(lines, ['anyps5 storage: quota exceeded', 'anyps5 storage: saved']);
});
```

- [ ] **Step 2: 실패 확인**

```bash
ctest --test-dir build-wasm-linux -R wasm_web_scripts --output-on-failure
```

Expected: FAIL, `Cannot find module '.../storage.mjs'`.

- [ ] **Step 3: `storage.mjs` 구현**

```js
function describe(error) {
  return 'anyps5 storage: ' + (error.message || error);
}

export function mountStorage(fs, idbfs, path, dependencies, log) {
  fs.mkdir(path);
  fs.mount(idbfs, {}, path);
  dependencies.add('aps5-storage');
  fs.syncfs(true, (error) => {
    if (error) log(describe(error));
    dependencies.remove('aps5-storage');
  });
}

export function createStorage(fs, log) {
  let syncing = false;
  let again = false;
  const commit = () => {
    if (syncing) {
      again = true;
      return;
    }
    syncing = true;
    fs.syncfs(false, (error) => {
      log(error ? describe(error) : 'anyps5 storage: saved');
      syncing = false;
      if (again) {
        again = false;
        commit();
      }
    });
  };
  return { commit };
}
```

Run: `ctest --test-dir build-wasm-linux -R wasm_web_scripts --output-on-failure` → Passed.

- [ ] **Step 4: 런타임 쪽 연결**

`core/wasm-runtime/platform/WebStorage.cpp`:

```cpp
#include "wasm-runtime/Platform.hpp"
#include <emscripten/em_asm.h>

void WasmStorage::Commit() {
    MAIN_THREAD_ASYNC_EM_ASM({ globalThis.aps5Platform.commitStorage(); });
}
```

`core/libs/prx/libSceSaveData.native/Export.cpp`: `#include "prx/libSceSaveData/SaveDataFile.hpp"` 뒤에 넣는다.

```cpp
#ifdef __EMSCRIPTEN__
#include "wasm-runtime/Platform.hpp"
#endif
```

`sceSaveDataDelete`와 `sceSaveDataUmount2`를 다음으로 바꾼다:

```cpp
int APS5_VABI sceSaveDataDelete(const SaveDataDelete* del) {
    const int rc = deleteSave(del);
    SAVEDATA_TRACE("delete dir=%s -> 0x%08x", del != nullptr && del->dir_name != nullptr ? del->dir_name->data : "(null)", static_cast<unsigned>(rc));
#ifdef __EMSCRIPTEN__
    if (rc == SAVE_DATA_OK) WasmStorage::Commit();
#endif
    return rc;
}
```

```cpp
int APS5_VABI sceSaveDataUmount2(uint32_t mode, const SaveDataMountPoint* mount_point) {
    const int rc = umount2(mode, mount_point);
    SAVEDATA_TRACE("umount2 mode=0x%x point=%s -> 0x%08x", mode, mount_point != nullptr ? mount_point->data : "(null)", static_cast<unsigned>(rc));
#ifdef __EMSCRIPTEN__
    if (rc == SAVE_DATA_OK) WasmStorage::Commit();
#endif
    return rc;
}
```

`core/wasm-runtime/CMakeLists.txt`:

```cmake
add_library(aps5_wasm_web_platform OBJECT platform/WebInput.cpp platform/WebAudio.cpp platform/WebStorage.cpp)
```

`anyps5-runtime-web`의 `target_link_options`에 `-lidbfs.js`를 더한다 (`-sFORCE_FILESYSTEM` 다음 줄).

- [ ] **Step 5: 페이지**

`index.html` 모듈 스크립트에서:

```js
import { startInput } from './input.mjs';
import { startAudio } from './audio.mjs';
import { createStorage, mountStorage } from './storage.mjs';
```

```js
const input = startInput(window);
const audio = startAudio(window);
let storage = null;
globalThis.aps5Platform = {
  attachInput: (buffer, address) => input.attach(buffer, address),
  attachAudio: (buffer, address) => audio.attach(buffer, address).catch((error) => console.log('anyps5 audio: ' + (error.stack || error))),
  audioStats: () => audio.stats(),
  commitStorage: () => storage.commit(),
};
addEventListener('pagehide', () => {
  if (storage !== null) storage.commit();
});
```

`preRun`을 다음으로 바꾼다:

```js
    preRun: [() => {
      FS.mkdir('/data');
      const log = (text) => console.log(text);
      mountStorage(FS, FS.filesystems.IDBFS, '/_sd', { add: addRunDependency, remove: removeRunDependency }, log);
      storage = createStorage(FS, log);
      for (const entry of parameters.getAll('env')) {
        const separator = entry.indexOf('=');
        if (separator > 0) ENV[entry.slice(0, separator)] = entry.slice(separator + 1);
      }
    }],
```

- [ ] **Step 6: 빌드와 회귀**

```bash
cmake --build build-wasm-linux --target anyps5-runtime-web anyps5-runtime-node -j16
ctest --test-dir build-wasm-linux -R wasm_ --output-on-failure
cd build-wasm-aot/dreaming-sarah/linux && timeout 30 $NODE ../../../build-wasm-linux/core/wasm-runtime/anyps5-runtime-node.js ../game.wasm 2>&1 | grep -E "^\[gpu\] present 1:|anyps5 runtime"; cd -
```

Expected: 테스트 Passed, Node 정상.

- [ ] **Step 7: 브라우저 확인 (M4)**

첫 실행 (새 프로필, New game이 세이브를 쓴다):

```bash
rm -rf $CHECKS/profile
$HEADLESS http://127.0.0.1:8080/ "" 45000 "" --profile $CHECKS/profile --keys 10500:Enter,16000:Enter --snap 35000:$CHECKS/m4-first.png 2>&1 | grep -E "anyps5 storage|dirNameSearch|localStorage.json|pageerror"
```

Expected: `dirNameSearch ... hit=0`, 16초 이후의 `localStorage.json` 쓰기, 그 뒤 `anyps5 storage: saved`. `pageerror`와 `anyps5 storage:` 오류 줄은 없다.

두 번째 실행 (같은 프로필):

```bash
$HEADLESS http://127.0.0.1:8080/ "" 50000 "" --profile $CHECKS/profile --keys 10500:Enter,14000:ArrowDown,15500:Enter --snap 13500:$CHECKS/m4-menu.png,15000:$CHECKS/m4-select.png,45000:$CHECKS/m4-continue.png 2>&1 | grep -E "dirNameSearch|saveindex|Go to layout|pageerror"
```

Expected:
- `dirNameSearch ... hit=1`, `Could not open file /savedata0/-saveindex`가 없다.
- `m4-select.png`에서 Continue가 선택되어 있고 `m4-continue.png`가 게임 장면이다.
- `m4-menu.png`에서 이미 Continue가 기본 선택이면 ArrowDown 없이 `--keys 10500:Enter,15500:Enter`로 다시 실행한다.

- [ ] **Step 8: 진행 상황 기록**

12절 표에 더한다:

```markdown
| M4 | 완료 | 같은 브라우저 프로필로 다시 열면 `SAVEDATA00`이 시작 전에 있고(`hit=1`), Continue로 게임에 들어감 |
```

- [ ] **Step 9: Commit** (저장소가 복구된 경우에만)

```bash
git add core/wasm-runtime core/libs/prx/libSceSaveData.native/Export.cpp docs/superpowers/specs/2026-10-16-wasm-sp4-playable-design.md
git commit -m "feat(wasm-runtime): keep save data in IndexedDB"
```

---

### Task 9: 전체 회귀와 사용자 확인 준비

**Files:**
- Modify: `docs/superpowers/specs/2026-10-16-wasm-sp4-playable-design.md` (12절, 실행 방법)

**Interfaces:**
- Consumes: Task 1–8 전부.
- Produces: 12절의 완료 상태와 데스크톱 Chrome 확인 절차.

- [ ] **Step 1: 깨끗한 빌드와 테스트**

```bash
cmake --build build-wasm-linux --target anyps5-runtime-web anyps5-runtime-node anyps5-runtime-test anyps5-platform-tests -j16
ctest --test-dir build-wasm-linux -R wasm_ --output-on-failure
```

Expected: 빌드 경고 없이 성공 (새 경고가 있으면 고친다), 테스트 Passed.

- [ ] **Step 2: 셰이더 회귀**

Task 4 Step 1의 캡처와 Step 4의 `anyps5-wgsl`·naga·`anyps5-wgsl-check`를 다시 실행한다. Expected: 모두 OK.

- [ ] **Step 3: 끝단 확인 한 번에**

```bash
rm -rf $CHECKS/profile
$HEADLESS http://127.0.0.1:8080/ "" 60000 "" --audio --autoplay --profile $CHECKS/profile --keys 10500:Enter,16000:Enter,45000:ArrowRight:2000,48000:Space:300 --snap 40000:$CHECKS/final-scene.png,48400:$CHECKS/final-jump.png 2>&1 | grep -E "skipped|webgpu\] error|pageerror|anyps5 (storage|audio|loader)|\[audio\]|Go to layout"
```

Expected: `skipped`, `error`, `pageerror`, `anyps5 loader` 줄 없음. `anyps5 storage: saved` 있음. 마지막 `[audio]`가 `running` 1이고 `consumed` > 0. 두 PNG에 Sarah가 보인다.

- [ ] **Step 4: 문서 마무리**

spec 12절 표 아래에 다음을 더한다:

```markdown
- 데스크톱 Chrome 확인 (사용자): `python3 core/wasm-runtime/web/serve.py --runtime build-wasm-linux/core/wasm-runtime --game build-wasm-aot/dreaming-sarah/web --port 8080`을 띄우고 `http://127.0.0.1:8080/`을 연다. 화면을 한 번 클릭하면 소리가 난다. Enter·방향키·WASD 또는 게임패드로 New game을 고르고 첫 장면을 걷는다. 새로고침 뒤 Continue를 고른다.
- 남은 일 (다음 단계): 필요할 때 읽는 파일 시스템, 렌더링 성능(draw 묶기, 버퍼 재사용), 첫 장면 밖에서 필요한 GPU 기능.
```

- [ ] **Step 5: Commit** (저장소가 복구된 경우에만)

```bash
git add docs/superpowers/specs/2026-10-16-wasm-sp4-playable-design.md
git commit -m "docs(wasm): record SP4 results"
```
