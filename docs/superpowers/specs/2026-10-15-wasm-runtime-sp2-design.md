# SP2: wasm 런타임 + 시스템 라이브러리 + WebGPU 렌더러

- 날짜: 2026-10-15
- 브랜치: `feat/wasm-build`
- 선행: [SP1 설계](2026-10-15-wasm-aot-sp1-design.md) (game.wasm과 게스트 ABI)
- 범위: 원래 로드맵의 SP2(런타임·라이브러리)와 SP3(WebGPU)를 합친 단계

## 1. 목표와 완료 기준

- **완료 기준**: Chrome에서 Dreaming Sarah의 타이틀 화면이 WebGPU로 그려진다.
- 범위 밖 (SP4): 입력, 오디오, 영구 세이브, 배포용 로더 UI.

## 2. 확정된 결정

| 항목 | 결정 |
|---|---|
| GPU | 이번 단계에 포함. Null 드라이버 단계 없음 |
| 렌더러 방식 | 새 WebGPU 실행 계층 + 재컴파일러 IR → WGSL 백엔드. GPU 무관 부분은 재사용 |
| 도구 | Emscripten 6.0.12 (Homebrew), wgpu-native v29.0.1.1 (macOS 네이티브 테스트용, `build-wasm-aot/third_party/`) |
| 메모리 | wasm64 공유 메모리, 최대 16 GiB |
| 링크 | SP1 방식 A: game.wasm과 런타임 모듈은 따로이고, import가 런타임 export에 직접 연결된다 |

### 실현 가능성 확인 (버릴 코드로 확인)
- Emscripten wasm64 + pthreads가 Node 26에서 동작한다.
- `emdawnwebgpu` 포트가 wasm64 + 공유 메모리로 빌드된다. `-sMEMORY64` 대신 `--target=wasm64` 표기를 쓴다.

## 3. 대상 라이브러리

Dreaming Sarah의 import를 소스의 export 이름(NID 계산)과 대조한 결과다.

| 라이브러리 | NID 수 |
|---|---|
| libSceAgc | 182 |
| libkernel | 50 (+ 게임의 libc.prx가 93) |
| libSceAgcDriver | 16 |
| libSceNpUniversalDataSystem | 12 |
| libSceSaveData.native | 9 (+2) |
| libScePad | 6 |
| libSceAudioOut | 5 |
| libSceSaveDataDialog(.native) | 6 |
| libSceSystemService, libSceUserService, libSceVideoOut | 각 4 |
| libSceSysmodule, libSceNpGameIntent | 각 3 |
| libSceNpTrophy2, libSceUlt | 각 2 |
| libc, libSceCommonDialog, libSceLibcInternal | 각 1~4 |

- import 484개 중 170개는 게임의 `libc.prx`(game.wasm 안)가 제공한다.
- 이름을 못 찾은 NID: `Ou3iL1abvng`, `f7uOxY9mM1U`, `djxxOmW6-aw` (런타임 구현 시 확인).

## 4. 구조

| 구성 | 위치 | 역할 |
|---|---|---|
| wasm 런타임 (C++) | `core/wasm-runtime/` | 이미지 적재·재배치, 정적 TLS, 게스트 스레드, `aps5_guest_call`, 게스트 진입, JS 로더 |
| NID 썽크 생성기 | `tools/` | export 소스의 clang AST(JSON)로 시그니처를 읽어 썽크 C++를 생성 |
| 라이브러리 | 기존 `core/libs/prx/*` | Emscripten으로 컴파일해 런타임과 하나의 모듈로 정적 링크 |
| WGSL 백엔드 | `core/shader/recompiler/WgslBackend/` | IR + 자원 계획 → WGSL |
| WebGPU 실행 계층 | `libSceAgcDriver` 안 새 계층 | PM4 → draw·dispatch·복사·메모리 쓰기·flip |
| 빌드 | CMake 옵션 (Emscripten 전용) | 기존 Linux/Windows/macOS 빌드는 바꾸지 않는다 |

### 4.1 NID 연결
- 썽크마다 `export_name("nid.<NID>")`를 붙인다. game.wasm의 `nid.<NID>` import가 이 export에 직접 연결된다.
- 썽크는 `GuestCpu`에서 SysV 규칙으로 인자를 읽고(rdi, rsi, rdx, rcx, r8, r9, xmm0-7, 스택), 결과를 rax/rdx/xmm0에 쓰고, 복귀 주소를 pop한다 (SP1 6.5절).
- 가변 인자 export는 썽크가 게스트 SysV `va_list`(`VaList`)를 만들어 넘긴다. 라이브러리는 기존 Windows용 `VaList` 읽기 경로를 쓴다.
- 데이터 NID는 생성된 표(NID → 주소)로 런타임이 재배치(`HostData`)를 채운다.
- 런타임에 없는 NID는 JS 로더가 NID 이름을 보고하는 트랩 함수로 채운다.
- 구조체 값 전달처럼 썽크가 처리하지 못하는 시그니처는 생성 단계에서 오류로 보고한다.

### 4.2 메모리
| 영역 | 주소 |
|---|---|
| 호스트 (Emscripten 정적 데이터, 스택, malloc) | 0 ~ 8 GiB |
| 게스트 arena (이미지, 게스트 스택, direct memory) | 8 ~ 16 GiB |

- `GuestArena.cpp`의 `__EMSCRIPTEN__` 분기에서 범위를 바꾼다. 메모리는 `emscripten_resize_heap`으로 필요할 때 늘린다.

### 4.3 시작 순서
1. JS 로더가 런타임 모듈을 띄우고 game.wasm을 컴파일한다. custom section `anyps5.images`를 런타임에 넘긴다.
2. 런타임이 이미지를 arena에 복사하고 재배치한다. TLS 배치와 주소 조회 표를 준비한다 (SP1 테스트 호스트와 같은 단계).
3. 게스트 모듈(`libc.prx`)부터 init을 실행하고, exe 진입점을 Linux 초기 스택(argc, argv, envp, auxv)으로 호출한다.

### 4.4 스레드
- 게스트 스레드는 Emscripten pthread다 (libkernel의 기존 경로).
- 스레드마다 `GuestCpu`(초기값: MXCSR 0x1F80, x87 FCW 0x037F)와 TLS 블록(variant II)을 만든다.
- game.wasm 인스턴스는 Worker마다 하나다. 메인에서 컴파일한 `WebAssembly.Module`을 모든 Worker에 postMessage로 보내고, Worker는 첫 게스트 진입 때 인스턴스를 만든다.
- 런타임 전체는 Worker에서 실행한다 (`PROXY_TO_PTHREAD`). 브라우저 메인 스레드는 블록할 수 없기 때문이다.

### 4.5 C++ 예외
- 시스템 libc 언와인더(`Unwind.cpp`)에 Emscripten 분기를 둔다. `GuestCpu` + 게스트 스택 + 이미지의 eh_frame/LSDA로 되감는다 (SP1 `tests/common/unwind.rs`와 같은 방식).

### 4.6 파일
| 환경 | 방식 |
|---|---|
| Node | 게임 `app0`를 NODEFS로 마운트 |
| 브라우저 | 개발 서버의 파일을 필요할 때 동기 요청으로 읽는 읽기 전용 FS. 쓰기는 메모리 FS |

### 4.7 OS 기능 차이
- `mprotect`/SIGSEGV 쓰기 감시 없음 → GPU 계층은 업로드 전 해시 비교.
- x86 intrinsic을 쓰는 파일 4개 (`GuestMemory.cpp`, `WaitMemory.cpp`, `UserContext.cpp`, `Math.cpp`)는 Emscripten 분기가 필요하다.
- 소켓과 호스트 `dlopen`은 쓰지 않는다.

## 5. GPU

### 5.1 재사용과 새 코드
| 부분 | 처리 |
|---|---|
| PM4 해석, 레지스터 상태 | 재사용 |
| 재컴파일러 앞단 (RDNA 디코더, IR, 제어 흐름, 번역, 최적화, 자원 계획) | 재사용 (Vulkan·x86 의존 없음) |
| 기존 Execution 드라이버, Graphics(Vulkan) | 쓰지 않음 |
| WebGPU 실행 계층, WGSL 백엔드 | 새로 작성 |

### 5.2 자원 모델
- 게스트 메모리 읽기(V#, 상수 버퍼, SRT): 자원 계획으로 범위를 알아내고, 그 범위를 스토리지 버퍼에 올린다. 셰이더는 고정 바인딩 + 기준 오프셋으로 읽는다.
- 64비트 주소 계산은 `vec2<u32>`로 한다.
- 텍스처: 게스트 표면을 detile해 업로드한다. 주소별 캐시.
- 렌더 타깃: 주소별 WebGPU 텍스처. 같은 주소를 텍스처로 읽으면 GPU 텍스처를 바로 쓴다. CPU가 읽을 때만 게스트 메모리에 되쓴다.
- 변경 감지: 업로드 전 내용 해시 비교.

### 5.3 스레드와 화면
- GPU 전용 스레드 하나가 장치·큐·canvas를 소유하고 모든 WebGPU 호출을 실행한다.
- `OffscreenCanvas`를 GPU 스레드로 넘긴다. VideoOut flip은 해당 주소의 렌더 타깃을 canvas로 복사한다.

## 6. 단계

| 단계 | 내용 | 확인 |
|---|---|---|
| M1 | Emscripten 빌드 기반, NID 썽크, 런타임 | SP1의 C/C++ 게스트 테스트 프로그램이 실제 런타임 위에서 Node로 같은 결과를 낸다 |
| M2 | Dreaming Sarah 기동 | Node에서 첫 GPU 명령 제출까지 진행 (로그) |
| M3 | WGSL 백엔드 | M2에서 수집한 셰이더가 WGSL로 변환되어 wgpu-native에서 검증·실행된다 |
| M4 | WebGPU 실행 계층 + 화면 | Chrome에서 타이틀 화면 (완료 기준) |

## 7. 테스트
- NID 썽크 생성기: 시그니처 분류 단위 테스트.
- 런타임: Node 통합 테스트 (M1, M2).
- WGSL 백엔드: macOS 네이티브 + wgpu-native (M3).
- 끝단: Chrome (M4).

## 8. 오류 처리
| 상황 | 동작 |
|---|---|
| 런타임에 없는 NID 호출 | NID 이름과 호출 위치를 보고하고 중단 |
| 썽크가 처리 못 하는 시그니처 | 생성 단계 오류 |
| 처음 보는 PM4 패킷·셰이더 기능·포맷 | 종류와 주소를 보고하고 중단 |
| 게스트 arena 부족 | 요청 크기와 함께 오류 |

## 9. 위험과 대응
| 위험 | 대응 |
|---|---|
| Worker마다 game.wasm 인스턴스화 | M1 첫 작업으로 검증 |
| Worker 안 WebGPU + OffscreenCanvas | M4 첫 작업으로 검증 |
| 라이브러리 이식 | 대상 17개 라이브러리만 |
| RDNA 셰이더 기능 vs WGSL | 커버리지 기반, 실제 셰이더로 테스트 |
| 성능 | M2부터 측정 |

## 10. 진행 상황 (2026-10-16)

| 단계 | 상태 | 확인 |
|---|---|---|
| M1 | 완료 | 런타임 테스트 빌드(`anyps5-runtime-test`)와 SP1 게스트 프로그램 |
| M2 | 완료 | Node 26에서 첫 셰이더 등록 → 첫 제출(58 dword) → 첫 draw → 첫 present(3840×2160)까지 진행하고 메인 루프에 들어간다 |
| M3 | 완료 | 수집한 draw 6개(VS 5개, PS 6개)가 전부 WGSL로 변환되고 naga 검증과 wgpu(Vulkan) 파이프라인 생성·draw를 통과한다 |
| M4 | 완료 (헤드리스 Chromium) | 타이틀 화면(로고, 메뉴, 물결 애니메이션)이 WebGPU로 그려진다. 실제 GPU의 데스크톱 Chrome 확인은 남았다 (10.7절) |

### 10.1 개발 환경 (Linux)

- 작업 환경이 macOS에서 Linux(Ubuntu 26.04)로 바뀌었다. Emscripten 6.0.12는 emsdk(`~/emsdk`)로 설치한다.
- Node 22는 memory64의 이전 JS API(`initial`/`maximum`이 Number)를 써서 런타임을 띄우지 못한다. Node 26을 쓴다.
- 빌드 디렉터리: wasm 런타임 `build-wasm-linux`, 네이티브 셰이더 도구 `build-shader-tools` (`-DANYPS5_BUILD_SHADER_TOOLS=ON`, 시스템 라이브러리 없이 재컴파일러만 빌드).
- 이 머신의 NVIDIA Vulkan ICD는 디스플레이 세션 없이 초기화되지 않는다. GPU 검사는 llvmpipe(Vulkan)에서 실행했다.

### 10.2 draw 캡처 (M3 입력)

- `APS5_CAPTURE_SHADERS=<디렉터리>`를 주면 Null 백엔드가 (VS 주소, PS 주소) 조합마다 처음 한 번 캡처한다.
  - `<vs>-<ps>.vs.b64`, `.ps.b64`: `RequestSerializer` 형식의 재컴파일 요청. 자원 분석이 읽은 게스트 메모리(`ShaderMemory`)를 포함한다.
  - `.regs`: context/shader/uconfig 레지스터.
  - `.vs.wgsl`, `.ps.wgsl`: wasm 안에서 만든 WGSL.
- 요청 구성은 네이티브 드라이버의 `DrawDecode`를 정점 셰이더 경로만으로 줄인 것이다 (`gpu/DrawStages.cpp`). 셰이더 레지스트리(`gpu/Shaders.cpp`)는 등록 시 코드·헤더를 복사한다.
- wasm 빌드에 재컴파일러 전체(`aps5_wasm_recompiler`)와 GPU 상태 디코더(`Graphics/State.cpp`, `ShaderInputState.cpp` 등, `aps5_wasm_gpu_state`)가 수정 없이 들어간다.
- 캡처 입력은 게임 데이터이므로 커밋하지 않는다.

### 10.3 WGSL 백엔드

| 항목 | 결정 | 이유 |
|---|---|---|
| 생성 시점 | variant 단계에서 IR을 보관하고(`CompiledVariant::wgsl`), materialize 단계에서 특수화 상수 값으로 WGSL을 만든다. 상수 목록과 바인딩 계획을 키로 메모이즈한다 | SPIR-V는 특수화 상수로 이미지 모드(숫자형 × 차원 6가지)와 힙 원소를 고른다. WebGPU는 정적으로 참조된 바인딩을 모두 요구하므로, 선택이 끝난 코드만 내보내야 한다 |
| 특수화 상수 | C++에서 값을 정해 리터럴로 쓴다. WGSL `override`는 쓰지 않는다 | 위와 같다 |
| 바인딩 | `@group`: VS 0, PS 1. `@binding`: 선택된 바인딩(`DescriptorBindingBuilder::Select`)의 원소를 계획 순서대로 연속 번호 | WebGPU에는 바인딩 배열이 없다. 런타임은 `RecompileResult::bindings` 순서와 개수로 같은 번호를 다시 계산한다 |
| user data | push data를 쓰지 않고(`pushConstantSizeBytes = 0`) ShaderData 스토리지 버퍼로 읽는다 | WebGPU에 push constant가 없다 |
| 서브그룹 | 단일 레인(`subgroupSize = 1`): `LaneId` = 0, `Ballot(p)` = `(p ? 1 : 0, 0, 0, 0)` | WebGPU 서브그룹은 선택 기능이다. SPIR-V 백엔드의 단일 레인 경로와 같다 |
| 정점 입력 | 내장 fetch의 성분 선택자와 숫자형(f32/i32/u32)을 상수 값으로 정해 `@location` 입력 타입을 고른다 | SPIR-V는 모듈 패치로 타입을 바꾼다 |
| 텍스처 스위즐 | T#의 dst_sel을 셰이더에서 적용한다 | WebGPU 텍스처 뷰는 성분 매핑이 없다 (Vulkan은 뷰가 적용) |
| 제어 흐름 | 선형 블록만 지원. 나머지는 오류 | Dreaming Sarah 셰이더는 전부 블록 2개, Phi 없음 |
| 수학 | SPIR-V 백엔드와 같은 방식: 비융합 mad, AMD 회전 단위 sin/cos(급수 보정), RTZ fp16 변환, 부호 비트 abs | 결과 비트를 맞춘다 |
| VS 출력 보간 | 기본 `perspective, center`. 런타임이 PS 입력에 맞춰 `SpecializeWgslVertexOutput`으로 바꾼다 | WebGPU는 VS 출력과 PS 입력의 보간 속성이 같아야 한다 |

- 구현한 IR 명령(커버리지 기준): 정수·비트·비교·선택·변환, `FPMad32`·`FPMul32`·`FPAdd32`·`FPSub32`·`FPAbs32`·`FPTrunc32`·`FPRecip32`·`FPSin`·`FPCos`·`FPOrdEqual32`·`PackFloat2x16Rtz`·`ConvertF32S32`, `Ballot`·`LaneId`, `GetUserData`·`GetBuiltin`·`GetAttribute`·`SetAttribute`, `ReadConstBuffer`(V# 범위 검사 포함), `ImageSampleRaw`(float 텍스처, 암시적/명시적 LOD·bias·grad·상수 offset, d16 결과).
- 처음 보는 명령·기능은 셰이더 해시와 함께 오류로 중단한다.
- 디스크 캐시: `CompiledShaderArtifact::wgsl`을 인코딩한다. WGSL variant는 IR을 들고 있으므로 디스크에 저장하지 않는다. 캐시 키에 `SpirvTarget::wgsl`을 넣었다.

### 10.4 검증 도구

| 도구 | 위치 | 하는 일 |
|---|---|---|
| `anyps5-wgsl` | `core/shader/recompiler/WgslBackend/tools` | 캡처한 VS/PS 요청을 WGSL로 바꾼다. PS는 VS의 `parameterExports`로 기본 입력을 정한다 (네이티브 `DefaultUnexportedInputs`) |
| `naga` | `cargo install naga-cli` | WGSL 검증 |
| `anyps5-wgsl-check` | `tools/wgsl-check` (wgpu 30) | VS/PS 쌍으로 자동 레이아웃 파이프라인을 만들고 더미 자원으로 draw 한다. WebGPU 기본 한도를 쓴다 |

- wasm에서 만든 WGSL과 네이티브에서 만든 WGSL은 값 이름을 정규화하면 11/12가 같다. 나머지 1개는 독립 명령의 순서만 다르다 (재컴파일러 패스의 순회 순서).

### 10.5 M4 실현 가능성 확인 (버릴 코드)

- wasm64 + pthread(`PROXY_TO_PTHREAD`) + `--use-port=emdawnwebgpu` 프로그램에서:
  - `emscripten_pthread_attr_settransferredcanvases`로 `#canvas`를 GPU 전용 pthread에 넘긴다.
  - GPU 스레드는 `emscripten_runtime_keepalive_push()` 후 반환해서 이벤트 루프로 돈다. 어댑터·디바이스 요청은 `WGPUCallbackMode_AllowSpontaneous` 콜백으로 끝난다.
  - 블로킹 스레드가 `emscripten_proxy_async`로 프레임 작업을 넘기고, GPU 스레드의 `wgpuQueueOnSubmittedWorkDone` 콜백이 원자 변수로 완료를 알린다. 3프레임 모두 진행됐고 DOM 캔버스 픽셀이 마지막 프레임 색과 같았다.
- 이 구조가 5.3절의 GPU 스레드 설계다. 게임의 GPU 큐 스레드(`Executor`)는 블로킹으로 남고, WebGPU 호출만 GPU 스레드로 넘긴다. `AfterWork` 완료는 위 콜백으로 구현한다.
- 헤드리스 Chromium(Playwright chromium-1234) 확인 방법:
  - 플래그: `--enable-unsafe-webgpu --enable-features=Vulkan --use-angle=vulkan --use-vulkan=swiftshader`. 다른 조합에서는 캔버스 내용이 비어 있다.
  - 스크린샷 대신 캔버스를 2D 캔버스에 `drawImage`해서 픽셀을 읽는다.
  - 개발 서버는 COOP/COEP 헤더(`same-origin`/`require-corp`)를 보내야 한다 (SharedArrayBuffer).

### 10.6 M4 구현

| 구성 | 위치 | 내용 |
|---|---|---|
| 웹 런타임 | `anyps5-runtime-web` (CMake) | `-sENVIRONMENT=web,worker -sFORCE_FILESYSTEM -sOFFSCREENCANVAS_SUPPORT --use-port=emdawnwebgpu`. 캔버스는 메인 pthread가 아니라 GPU 스레드가 받는다 (`OFFSCREENCANVASES_TO_PTHREAD=''`) |
| 로더 | `core/wasm-runtime/web/index.html` | game.wasm을 컴파일해 `globalThis.aps5GameModule`에 두고, 게임 데이터 패키지와 런타임을 순서대로 불러온다 |
| 게임 데이터 | Emscripten `file_packager` | `app0`를 `/app0`으로 미리 올린다 (Dreaming Sarah 104 MB). 필요할 때 읽는 방식은 SP4 |
| 개발 서버 | `core/wasm-runtime/web/serve.py` | `/runtime`, `/game`을 디렉터리에 연결하고 COOP/COEP 헤더를 보낸다 |
| 헤드리스 확인 | `core/wasm-runtime/web/headless.cjs` | Playwright로 페이지를 열고 콘솔 패턴을 기다린 뒤 캔버스를 PNG로 저장한다 |
| WebGPU 백엔드 | `core/wasm-runtime/gpu/WebGpuBackend.cpp` | Node 빌드는 `NullBackend`, 웹 빌드는 이것을 링크한다 |

- 실행기 스레드: draw 디코딩 → VS/PS WGSL 재컴파일 → 게스트 메모리 스냅샷(정점, 인덱스, 스토리지 버퍼, 선형 텍스처) → 작업을 GPU 스레드로 비동기 전달. 스냅샷을 복사하므로 게임이 이후 메모리를 바꿔도 된다.
- GPU 스레드: 렌더 타깃은 주소별 텍스처로 둔다. 같은 주소를 샘플링하면 업로드 없이 그 텍스처를 쓴다 (5.2절). 파이프라인은 WGSL 원문과 상태를 키로 캐시한다. present는 표시 버퍼 주소의 렌더 타깃을 캔버스(1920×1080)로 블릿한다. 렌더 타깃이 아니면 `ReadDisplayBuffer`로 CPU 디코딩한 것을 올린다.
- 동기화:
  - `AfterWork`(EOP 인터럽트, 플립 완료)는 GPU 스레드가 앞선 작업을 제출한 시점에 완료를 알린다. GPU 실행 완료를 기다리지 않는다. 입력 메모리는 draw 시점에 복사했고 렌더 타깃을 CPU로 되쓰지 않으므로, 게임에 GPU 완료를 알릴 이유가 없다.
  - 처음에는 `OnSubmittedWorkDone`까지 기다렸다. 게임이 프레임마다 여러 번 그 왕복을 기다려서, 브라우저 CPU 경로가 60 fps인데도 1.4 fps가 나왔다.
  - 게임이 앞서가는 양은 present가 막는다: 직전 present가 GPU에서 끝날 때까지 기다린다 (진행 중인 프레임 1개).
- 텍스처 업로드: 실행기 스레드가 게스트 메모리 내용을 해시해 같은 주소·크기·포맷의 직전 업로드와 같으면 복사와 `WriteTexture`를 건너뛴다.
- 디버그: 로더 URL의 `?env=이름=값`(여러 번 가능)은 `getenv`로 읽힌다. `APS5_WEBGPU_NULL=1`은 WebGPU 작업을 끄고 present 수만 센다.
- 상태 변환:
  - 뷰포트: 음수 높이(Vulkan의 y 반전)는 WebGPU 규약과 같으므로 양수로 바꾸기만 한다. 양수 높이면 VS의 `aps5_flip_y` 상수로 y를 뒤집고 앞면 방향을 바꾼다.
  - depth `negativeOneToOne`은 VS의 `aps5_depth_half` 상수로 z를 (z + w) / 2로 바꾼다.
  - rect list: VS를 `aps5_vertex` 함수로 감싸고, `aps5_rect` 진입점이 꼭짓점 3개를 계산해 직각 꼭짓점을 찾고 네 번째(a + b − c)를 더해 삼각형 2개로 그린다 (네이티브 `RectListShaders`와 같은 판정). 정점 버퍼가 없는 rect list만 지원한다.
  - VS 출력 보간은 PS WGSL의 입력 선언을 읽어 맞춘다.
- 지원하지 않는 경우(깊이 타깃, 타일 텍스처 업로드, 블록 압축, 인스턴스 fetch, PS 없는 draw, compute)는 메시지를 한 번 출력하고 그 draw를 건너뛴다.

### 10.7 M4 결과

- 헤드리스 Chromium(chromium-1234, SwiftShader WebGPU): 장치 준비 1.7초, 첫 present 2.3초. 타이틀 화면이 그려지고 애니메이션이 진행된다. WebGPU 검증 오류는 없다.
- 속도:
  - `APS5_WEBGPU_NULL=1`(GPU 작업 없음): 약 60 fps. 브라우저의 게임 CPU 경로는 병목이 아니다.
  - SwiftShader: 약 1 fps. 프레임당 GPU 작업(3840×2160 클리어·확대, 1280×720 draw 약 12개, 블릿)이 소프트웨어 래스터로 약 0.7초 걸린다. 실행기 준비는 프레임당 약 4 ms, GPU 스레드 인코딩은 약 3 ms다.
- 실제 GPU (2026-10-16): 헤드리스 Chromium + Intel UHD(TGL GT1, Mesa ANV)에서 타이틀 화면이 그려지고 약 27 fps가 나온다 (present 512→1024를 19초). 장치 준비 1.5초.
  - 실행: `USE_VULKAN=native VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/intel_icd.json`과 `render` 그룹 권한(`sudo -u $USER -g render ...`)으로 `headless.cjs`를 띄운다. 로그인 좌석 세션이 없으면 `/dev/dri/renderD*` ACL이 사용자에게 열리지 않는다.
  - 이 머신의 NVIDIA(RTX 3060 Laptop, 580 드라이버)는 root로도 Vulkan ICD가 `vkCreateInstance`를 내주지 않는다 (커널 로그: 디스플레이 감지 실패, 전원 관리 오류). 드라이버 문제라 확인 대상에서 뺐다.

실행 방법:

```
emcmake cmake -S . -B build-wasm-linux -DANYPS5_BUILD_WASM_RUNTIME=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-wasm-linux --target anyps5-runtime-web
cd <relinker 출력>/linux
python3 ~/emsdk/upstream/emscripten/tools/file_packager.py <game>/game.data --preload app0@/app0 --js-output=<game>/game.data.js --no-node
python3 core/wasm-runtime/web/serve.py --runtime build-wasm-linux/core/wasm-runtime --game <game> --port 8080
```

`<game>` 디렉터리에는 `game.wasm`, `game.data`, `game.data.js`가 있어야 한다. Chrome에서 `http://127.0.0.1:8080/`을 연다 (Linux는 `--enable-unsafe-webgpu --enable-features=Vulkan`).

### 10.8 SP4로 넘기는 일

- 입력, 오디오, 영구 세이브, 필요할 때 읽는 파일 시스템 (1절의 범위 밖 항목).
- 성능: draw마다 render pass와 제출을 따로 만들고, 버퍼도 draw마다 새로 만든다. 실제 GPU에서 측정한 뒤 묶는다.
- 렌더 타깃을 CPU가 읽는 경우의 되쓰기, 타일 텍스처 detile, 깊이 타깃, compute, 정수·큐브·깊이 비교 텍스처, 비선형 제어 흐름 (Dreaming Sarah 타이틀에는 필요 없음).
