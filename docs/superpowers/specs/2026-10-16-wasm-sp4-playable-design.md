# SP4: 브라우저에서 플레이 가능 (입력, 오디오, 세이브)

- 날짜: 2026-10-16
- 브랜치: `feat/wasm-build`
- 선행: [SP2 설계](2026-10-15-wasm-runtime-sp2-design.md) (런타임, WebGPU 렌더러, 타이틀 화면)

## 1. 목표와 완료 기준

- **완료 기준**: Chrome에서 Dreaming Sarah를 타이틀에서 New game으로 시작해 첫 장면을 키보드나 게임패드로 플레이한다. 소리가 나고, 새로고침 뒤 Continue로 이어진다.
- 게임 진입에 필요한 GPU 기능은 이 단계에 포함한다.
- 범위 밖 (다음 단계): 필요할 때 읽는 파일 시스템, 렌더링 성능 개선(draw 묶기, 버퍼 재사용), 첫 장면에 필요 없는 GPU 기능, 배포용 로더 UI, 키 매핑 설정 UI.

## 2. 확정된 결정

| 항목 | 결정 |
|---|---|
| 범위 | 입력 + 오디오 + 영구 세이브 + 게임 진입에 필요한 GPU 기능 |
| 입력·오디오 전달 | wasm 공유 메모리의 링 버퍼로 직접 주고받는다. postMessage와 Emscripten SDL2 포트는 쓰지 않는다 |
| 세이브 저장소 | 웹은 IDBFS, Node는 지금처럼 NODEFS |
| 키 매핑 | 네이티브 기본 매핑(`InputMapping.hpp`)과 같은 키. 설정 파일은 읽지 않는다 |

### 2.1 현재 상태 (2026-10-16 확인)

- 입력: `libScePad`는 `PadPublishInput_nid_postfix(const PadInputState&)`로 바뀐 상태를 받아 큐에 넣는다. 네이티브는 `libSceVideoOut`의 SDL `PadInput`이 이 함수를 부른다. wasm 빌드는 이 파일을 빼서 패드가 항상 중립이다.
- 오디오: wasm 빌드의 `AudioOut.cpp`는 장치를 열지 않는 가상 포트로 출력 시간만 맞추고 소리를 버린다.
- 게임의 오디오 포트: 메인 포트 1개, 256샘플(5.3 ms), 48 kHz, float 스테레오(param 4). `APS5_TRACE_AUDIOOUT`의 `format`은 param이 아니라 내부 `Format` 열거 값(5 = `F32Stereo`)이다.
- 세이브: `SAVEDATA00`을 `/savedata0`에 마운트(mode `CREATE2`)한다. 실제 경로는 현재 디렉터리의 `_sd/SAVEDATA00`이다. 웹에서는 메모리 FS의 `/_sd`라서 새로고침하면 사라진다.
  - New game을 고르면 게임이 마운트 → `localStorage.json`, `-saveindex` 쓰기 → `sceSaveDataUmount2`를 한다. `[SAVEDATA:native]` 트레이스는 횟수 제한이 있어 이후 호출은 찍히지 않는다.
- 스크립트 입력: `APS5_PAD_INPUT_FILE=<경로>`가 이미 있다 (`InputCommandFile`, 첫 줄 `<순번> <버튼> <ms>`를 100 ms마다 읽는다). 웹에서는 `?env=APS5_PAD_INPUT_FILE=/input.txt`와 페이지의 `FS.writeFile`로 쓸 수 있다.

## 3. 구조

SP2의 GPU 백엔드(`WebGpuBackend`/`NullBackend`)와 같은 방식으로 런타임에 플랫폼 계층을 둔다.

| 구성 | 웹 빌드 (`anyps5-runtime-web`) | Node 빌드 (`anyps5-runtime-node`) |
|---|---|---|
| 입력 | `platform/WebInput.cpp` + `web/input.js` | 없음 (패드 중립) |
| 오디오 | `platform/WebAudio.cpp` + `web/audio-worklet.js` | `platform/NullAudio.cpp` (지금의 가상 포트 동작) |
| 세이브 동기화 | `platform/WebStorage.cpp` (IDBFS) | `platform/NullStorage.cpp` (할 일 없음) |

- 위치: `core/wasm-runtime/platform/` (C++), `core/wasm-runtime/web/` (JS).
- 라이브러리는 런타임 헤더 `wasm-runtime/Platform.hpp`의 인터페이스만 부른다. 라이브러리 쪽 변경은 `__EMSCRIPTEN__` 분기 안에 둔다.

```cpp
namespace WasmInput {
void Start();                                     // 웹: 입력 스레드를 한 번 만든다. Node: 할 일 없음
}
namespace WasmAudio {
inline constexpr std::uint32_t Frequency = 48000;
struct Samples {
    const void* data;
    bool isFloat;
    std::uint32_t channels;                       // 1, 2, 8
    std::uint32_t frames;
};
int Open(std::uint32_t frequency);                // 스트림 번호. Node는 -1 (가상 포트)
bool Running();                                   // 브라우저 AudioContext가 돌고 있다
std::uint32_t Queued(int stream);                 // 아직 재생되지 않은 프레임 수
std::uint32_t Write(int stream, const Samples& samples);  // 스테레오로 다운믹스해 들어가는 만큼 쓰고, 쓴 프레임 수를 돌려준다
void Close(int stream);
}
namespace WasmStorage {
void Commit();                                    // 세이브 디렉터리를 영구 저장소로 보낸다
}
```

## 4. 입력

### 4.1 흐름

1. 메인 스레드의 `input.js`가 `keydown`/`keyup`을 받고, `requestAnimationFrame`마다 `navigator.getGamepads()`를 읽는다.
2. 키보드와 첫 번째 연결된 게임패드를 합친 상태가 바뀌면 공유 메모리의 입력 링에 한 칸을 쓰고 쓰기 위치에 `Atomics.notify`를 한다.
3. 런타임의 입력 스레드(웹 빌드에서 시작 시 하나 만든다)가 `__builtin_wasm_memory_atomic_wait32`로 쓰기 위치를 기다렸다가, 칸마다 `PadInputState`를 만들어 `PadPublishInput_nid_postfix`를 부른다.

- 눌렀다 바로 뗀 키도 상태 변화 두 칸으로 전달되므로 놓치지 않는다.
- 링은 정적 변수다. 입력 스레드를 만들 때 런타임이 `MAIN_THREAD_ASYNC_EM_ASM`으로 `aps5Platform.attachInput(wasmMemory.buffer, 주소)`를 부른다. 오디오(`attachAudio`)와 세이브(`commitStorage`)도 같은 방식이라 export 함수가 필요 없다.

### 4.2 링 형식

| 필드 | 형식 |
|---|---|
| 헤더 | `write`(u32), `read`(u32): 칸 번호, 단조 증가 |
| 칸 (64개) | `buttons`(u32), `sticks`(u8 × 4: LX, LY, RX, RY), `l2`(u8), `r2`(u8), `touch`(u8: 1 왼쪽, 2 오른쪽, 4 가운데), `device`(u8: 0 없음, 1 DualSense, 2 DUALSHOCK 4, 3 기타) = 12바이트 |

- 단일 생산자(메인 스레드), 단일 소비자(입력 스레드).
- 링이 가득 차면 JS는 최신 상태 하나만 들고 있다가 자리가 나면 쓴다.

### 4.3 매핑

- 키보드 (네이티브 기본값과 같다. `KeyboardEvent.code` 기준):

| 키 | 패드 |
|---|---|
| Enter, Space | ✕ (`0x4000`) |
| Escape | Options (`0x0008`) |
| I | △ (`0x1000`) |
| C | ○ (`0x2000`) |
| Q / E, Alt | L1 / R1 |
| Shift / Ctrl | L3 / R3 |
| 방향키 | 십자키 |
| WASD | 왼쪽 스틱 (끝까지 기울임: 0 또는 255) |
| F, H, T, G | 오른쪽 스틱 |
| Backspace / Tab | 터치패드 왼쪽 / 오른쪽 (`touch` 1 / 2) |

- 마우스 매핑과 마우스 시점 모드는 넣지 않는다.
- 게임패드는 standard 매핑을 따른다 (네이티브 `PadInput::sampleController`와 같은 의미):
  - 버튼 0 ✕, 1 ○, 2 □, 3 △, 4/5 L1/R1, 9 Options, 10/11 L3/R3, 12–15 십자키 위·아래·왼쪽·오른쪽.
  - 6/7은 아날로그 값을 `l2`/`r2`(0–255)에 쓰고, 0이 아니면 L2/R2 비트를 켠다.
  - PlayStation 패드는 17(터치패드 클릭)을 터치패드 버튼으로, 그 밖의 패드는 8(View)을 터치패드 버튼 + 가운데 터치로 쓴다.
  - 축 0–3은 스틱이다. 바이트 변환은 네이티브 `stickByte`와 같고, 데드존은 기본값 0%다.
  - `device`는 `Gamepad.id`로 정한다 (DualSense, DUALSHOCK 4, 기타).
- 키보드와 게임패드는 버튼을 비트 OR로 합친다. 키보드로 누른 축은 키보드 값이 이긴다 (반대 방향을 함께 누르면 128). 네이티브 `KeyboardMouse::Compose`와 같다.
- 창이 포커스를 잃으면(`blur`) 키보드 상태를 모두 뗀 것으로 바꾼다.
- 게임이 쓰는 키(방향키, Space, Tab, Backspace)는 `preventDefault()`로 브라우저 동작을 막는다.

## 5. 오디오

### 5.1 흐름

1. `index.html`이 `AudioContext({sampleRate: 48000, latencyHint: 'interactive'})`를 만들고 `audio-worklet.js`를 불러온다.
2. 자동 재생 정책 때문에 컨텍스트는 처음에 `suspended`다. 첫 `pointerdown`이나 `keydown`에서 `resume()`한다.
3. 첫 `WasmAudio::Open`이 `aps5Platform.attachAudio(wasmMemory.buffer, 주소)`로 오디오 상태(링 4개)의 주소를 알린다. 메인 스레드는 worklet에 버퍼와 주소를 보내고, worklet이 받았다고 답하면 `running`을 쓴다. `Open`은 비활성 링 하나를 활성으로 바꾼다.
4. `sceAudioOutOutput`이 PCM을 링에 쓰고, AudioWorklet의 `process()`가 같은 SharedArrayBuffer에서 읽는다.

### 5.2 링 형식

| 필드 | 형식 |
|---|---|
| 헤더 | `write`, `read` (u32, 프레임 번호, 단조 증가), `running` (u32), `underruns` (u32), `consumed` (u32, 0이 아닌 샘플이 있던 프레임 수) |
| 데이터 | float32 스테레오 인터리브, 8192프레임 (약 170 ms) |

- 링은 최대 4개다 (정적 배열). worklet은 활성 링을 모두 더해 출력한다.
- 데이터가 모자라면 남은 부분은 무음으로 채우고 `underruns`를 하나 늘린다.
- `running`은 메인 스레드가 쓴다: worklet이 붙었고 컨텍스트가 `running`일 때만 1이다 (`statechange`마다 다시 쓴다). worklet이 붙기 전에 1이 되면 아무도 링을 비우지 않아 출력이 매번 200 ms씩 막힌다.

### 5.3 `sceAudioOutOutput` (웹)

1. `WasmAudio::Running()`이 false면 지금의 가상 포트 방식(`paceVirtualPort`)으로 시간만 맞추고 소리를 버린다. 첫 입력 전에도 게임이 멈추지 않는다.
2. 기존 `prepareBuffer`로 볼륨과 std 8채널 순서를 처리한다.
3. `Queued`가 `TARGET_LATENCY_US`(40 ms = 1920프레임)보다 크면 `DRAIN_SLEEP_US`(1 ms)씩 잠든다. `DRAIN_TIMEOUT_US`(200 ms)를 넘기면 기다림을 멈춘다. SDL 경로와 같은 상수다.
4. `WasmAudio::Write`가 스테레오로 다운믹스해 링에 쓴다: L = FL + 0.707·(C + SL + BL), R = FR + 0.707·(C + SR + BR). LFE는 버리고 [-1, 1]로 클램프한다. 모노는 양쪽에 같은 값을 쓴다. 정수 형식은 32768로 나눈다. 링이 가득 차면 들어가는 만큼만 쓴다 (worklet이 멈춘 경우).

- 48 kHz가 아닌 포트는 `WasmAudio::Open`이 주파수를 담은 `std::runtime_error`를 던진다 (CONTRIBUTING: 하는 일을 정확히 하거나 던진다). 리샘플러는 넣지 않는다.
- `data == nullptr`(드레인)은 링이 빌 때까지 기다리되 `DRAIN_TIMEOUT_US`(200 ms)를 넘기지 않는다.

## 6. 세이브

- `index.html` preRun:
  1. `/_sd`(마운트형 세이브)와 `/_sd_mem`(SaveDataMemory)을 만들고 각각 IDBFS를 마운트한다.
  2. `addRunDependency('idbfs')` 후 `FS.syncfs(true, …)`가 끝나면 의존성을 푼다. 저장된 세이브가 게임 시작 전에 보인다.
- `sceSaveDataUmount2`, `sceSaveDataDelete`, `sceSaveDataCommit`, `sceSaveDataSetSaveDataMemory2`, `sceSaveDataSetupSaveDataMemory2`가 성공하면 `__EMSCRIPTEN__` 분기에서 `WasmStorage::Commit()`을 부른다.
  - 웹: `MAIN_THREAD_ASYNC_EM_ASM`으로 메인 스레드에 `FS.syncfs(false, …)`를 요청한다. 진행 중이면 끝난 뒤 한 번 더 한다.
  - Node: 할 일이 없다 (`NODERAWFS`가 바로 디스크에 쓴다).
- 커밋은 저장된 세이브를 다 불러온 뒤에만 한다. 불러오는 중에 온 요청은 불러오기가 끝난 뒤 한 번 실행하고, 불러오기에 실패하면 커밋을 거부하고 로그를 남긴다. IDBFS의 `syncfs(false)`는 메모리에 없는 항목을 IndexedDB에서 지우므로, 불러오지 않은 상태로 보내면 저장된 세이브가 지워진다.
- `pagehide` 동기화는 하지 않는다. 세이브가 마운트된 채 반쯤 쓰인 상태를 보내 마지막으로 온전한 세이브를 덮어쓸 수 있고, 온전한 세이브는 umount 때 이미 보낸다 (최종 리뷰 반영).
- 동기화가 끝나면 콘솔에 `anyps5 storage: saved`를 남긴다 (테스트가 이 줄을 기다린다). 진행 중에 다시 요청되면 끝난 뒤 한 번만 더 한다.
- 동기화 실패는 콘솔에 남기고 게임은 계속 진행한다.

## 7. 게임 진입에 필요한 GPU 기능

- 조사 (2026-10-16, `APS5_PAD_INPUT_FILE`로 New game, 헤드리스 Chromium + Intel UHD): 첫 장면(숲, layout `M0T0`)까지 들어가고 오른쪽·점프에 맞춰 카메라가 움직인다. 플레이어(Sarah)가 보이지 않는다. 건너뛴 draw는 두 종류다.
  - `VkFormat 76 has no texture format`: `VK_FORMAT_R16_SFLOAT`.
  - `FPOrdLessThan32 is not implemented in WGSL` (PS `0x106c96b00`).
- 구현: WebGPU에 같은 형식이 있는 float·unorm·snorm 텍스처 형식, WGSL의 부동소수 비교 명령 묶음(`FPOrd*`, `FPUnord*`, `FPIsNan32`). 의미는 SPIR-V 백엔드(`OpFOrd*`/`OpFUnord*`/`OpIsNan`)와 같고, NaN은 비트로 판정한다.
- 이 둘을 고친 뒤에도 남는 skip이나 잘못 그려지는 것이 있으면 같은 방식으로 찾아 고친다.
- 구현 결과 (2026-10-16): Sarah의 PS(`0x106c96b00`)에는 비교 외에도 `LogicalNot`·`LogicalAnd`, `FPSaturate32`, `FPSqrt`, `Reference`와 조기 종료 분기(조건이 참이면 내보내기 1개 후 return, 아니면 본문)가 있었다.
  - WGSL 백엔드에 구조적 if/else 출력을 넣었다: merge 블록을 따라 중첩 선택문을 만들고, 분기 안의 return은 단계별 끝맺음(`aps5_valid` discard 검사, `return aps5_out;`)으로 바꾼다. 루프, 간접·goto 분기, Phi는 계속 오류다.
  - `FPSaturate32`는 SPIR-V 백엔드와 같은 비트 판정(양수일 때만 `min(bits, 1.0)`, 음수·NaN은 0), `Reference`는 값이 없는 명령, 논리 명령은 WGSL 논리 연산이다.
  - 기존에 수집한 셰이더 6쌍은 `FPOrdEqual32` 식만 바뀌고, 새 캡처 8쌍과 함께 naga와 `anyps5-wgsl-check`(Intel UHD)를 통과한다.
- 새 셰이더는 SP2 방식으로 검증한다: `APS5_CAPTURE_SHADERS` 캡처 → `anyps5-wgsl` → naga, `anyps5-wgsl-check`.
- 찾은 기능과 결정은 이 문서의 진행 상황 절에 적는다.

## 8. 단계

| 단계 | 내용 | 확인 |
|---|---|---|
| M1 | 입력 | 헤드리스 Chromium에서 타이틀에 Enter를 누르면 캔버스가 타이틀 화면과 달라진다 (New game 진행). 아래 방향키로 메뉴 선택 표시가 Continue로 옮겨 간다 |
| M2 | 게임 진입 GPU | New game 뒤 첫 장면이 skip 메시지 없이 그려진다 (캔버스 PNG) |
| M3 | 오디오 | `--autoplay-policy=no-user-gesture-required` 헤드리스에서 worklet이 0이 아닌 샘플을 소비하고, 타이틀 10초 동안 언더런이 시작 직후 외에는 없다 |
| M4 | 세이브 | 게임이 `SAVEDATA00`을 쓴 뒤 새로고침하면 게임 시작 전에 `/_sd/SAVEDATA00`이 있고, Continue로 이어진다 |
| 마무리 | 사용자 확인 | 데스크톱 Chrome에서 키보드와 게임패드로 첫 장면을 플레이하고 소리를 듣는다 |

## 9. 테스트

- 순수 로직 단위 테스트 (Node, `anyps5-runtime-test` 방식): 입력 링(가득 참, 순서), 다운믹스 공식, 오디오 링 산술(감김, 언더런).
- `headless.cjs` 확장:
  - `--keys <ms>:<code>[+<ms 유지>],…`: 지정 시각에 키를 누른다 (Playwright `keyboard`).
  - `--snap <ms>,…`: 지정 시각마다 캔버스 PNG를 저장한다.
  - `--audio`: 종료 전에 링 헤더(`consumed`, `underruns`)를 읽어 출력한다.
  - `USE_VULKAN=native`로 실제 GPU(이 머신은 Intel UHD)에서 돌린다 (SP2 10.7절).
- 세이브 확인: 같은 브라우저 프로필(Playwright persistent context)로 페이지를 두 번 연다.

## 10. 오류 처리

| 상황 | 동작 |
|---|---|
| 입력 링이 가득 참 | JS가 최신 상태 하나만 들고 있다가 자리가 나면 쓴다 |
| 오디오 언더런 | 무음을 내고 횟수를 센다 |
| 48 kHz가 아닌 오디오 포트 | 주파수를 담은 오류로 중단 |
| IDBFS 동기화 실패 | 콘솔에 남기고 계속 진행. 불러오기 실패 뒤에는 저장된 세이브를 지우지 않도록 커밋을 거부한다 |
| 처음 보는 GPU 기능 | 지금처럼 한 번 출력하고 건너뛴다. 첫 장면에 필요하면 M2에서 구현한다 |

## 11. 위험과 대응

| 위험 | 대응 |
|---|---|
| memory64 공유 메모리의 SharedArrayBuffer를 AudioWorklet에 넘기기 | 확인함 (2026-10-16, 헤드리스 Chromium): `address: 'i64'` 공유 메모리의 버퍼를 worklet 포트로 보내면 worklet이 `Atomics`로 읽고 쓴다 |
| 메모리가 커지면 `wasmMemory.buffer` 객체가 바뀐다 | 확인함: `memory.grow` 뒤에도 worklet이 예전 버퍼로 계속 읽고 쓴다. 링은 정적 영역에 있다 |
| 게임 진입 후 필요한 GPU 기능이 많다 | M2에서 목록을 먼저 만들고, 양이 크면 범위를 다시 정한다 |
| 헤드리스에서 오디오 장치가 없다 | 확인함: 이 머신의 헤드리스 Chromium에서 `AudioContext`가 48 kHz `running`으로 돌고 `currentTime`이 늘어난다 (`--autoplay-policy=no-user-gesture-required`) |

## 12. 진행 상황

| 단계 | 상태 | 확인 |
|---|---|---|
| M1 | 완료 | 헤드리스 Chromium(Intel UHD)에서 Enter로 메뉴, 아래 방향키로 선택이 Continue로 이동(게임이 `select` 효과음 재생), Enter 두 번으로 `M0T0` 진입. 메뉴는 나타나는 동안(약 17초 전) 입력을 받지 않는다 |
| M2 | 완료 | `R16_SFLOAT`·`R16G16_SFLOAT` 텍스처, WGSL 부동소수 비교·논리·saturate·sqrt, 구조적 if/else. 첫 장면에 skip 없음, Sarah가 보이고 걷기·점프(`jump_03`)가 된다. 부팅 시간이 실행마다 2–3초 달라서 스크립트는 New game을 22초에 누른다 |
| M3 | 완료 | 헤드리스 Chromium(자동 재생 허용): 게임 안 40→55초에 worklet이 소리 있는 프레임 722,048개를 소비(약 48 kHz, 음악 재생), 언더런 0. 타이틀은 무음. 자동 재생 없이도(`suspended`) present 512까지 평소 속도로 진행 |
| M4 | 완료 (Continue는 사용자 확인) | 같은 브라우저 프로필로 다시 열면 게임 시작 전에 `SAVEDATA00`이 있고(`hit=1`), 게임이 `-saveindex`를 읽는다. `/_sd/SAVEDATA00`의 파일이 바이트 단위로 같다. New game 직후 세이브는 `localStorage.json`이 `null`이라(Node도 같다) 게임이 Continue에 오류음을 낸다. 진행 저장 뒤의 Continue는 데스크톱 확인에서 본다 |

- 끝단 확인 (2026-10-16, 헤드리스 Chromium + Intel UHD, 자동 재생 허용, 새 프로필): New game → `M0T0` → 걷기·점프. skip·WebGPU 오류·페이지 오류 없음, 세이브 커밋(`anyps5 storage: saved`), 오디오 `running` 1·언더런 0.
- 테스트: `ctest --test-dir build-wasm-linux -R wasm_` (C++ 플랫폼 테스트 13개, JS `node:test` 36개). `-DANYPS5_NODE=<Node 26>`으로 구성한다.
- 데스크톱 Chrome 확인 (사용자): `python3 core/wasm-runtime/web/serve.py --runtime build-wasm-linux/core/wasm-runtime --game build-wasm-aot/dreaming-sarah/web --port 8080`을 띄우고 `http://127.0.0.1:8080/`을 연다. 화면을 한 번 클릭하면 소리가 난다. 메뉴가 다 나타난 뒤(약 20초) Enter·방향키·WASD 또는 게임패드로 New game을 고르고 첫 장면을 걷는다. 세이브 지점에서 저장한 뒤 새로고침하고 Continue를 고른다.
- 남은 일 (다음 단계): 필요할 때 읽는 파일 시스템, 렌더링 성능(draw 묶기, 버퍼 재사용), 첫 장면 밖에서 필요한 GPU 기능(루프 등 다른 제어 흐름, Phi 포함).
