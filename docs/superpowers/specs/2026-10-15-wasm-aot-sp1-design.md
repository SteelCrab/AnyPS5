# SP1: anyps5-wasm-aot와 게스트 ABI 계약

- 날짜: 2026-10-15
- 브랜치: `feat/wasm-build`
- 상태: 구현 완료 (완료 기준 1~3 충족). 구현 중 바뀐 설계는 11절에 정리했다.

## 1. 목적

PS5 게임을 설치 없이 브라우저에서 실행한다. 경로는 다음과 같다.

```
PS5 ELF ──relinker (C++, 기존)──▶ Linux x86-64 ELF (NID 이름 import)
                                        │
                                        ▼
                         anyps5-wasm-aot (Rust, 새로 만듦)
                                        │
                                        ▼
                                    game.wasm

[실행: 브라우저]
game.wasm ──import (NID 이름)──▶ 시스템 라이브러리 wasm (C++, Emscripten)
    ▲                                   │
    └────── 콜백 (aps5_guest_call) ──────┘
                                        ▼
                              WebGPU 렌더러 (C++)
```

최종 목표는 **Dreaming Sarah (PPSA02929)를 Chromium 브라우저에서 WebGPU로 플레이하는 것**이다.

## 2. 전체 로드맵

작업이 커서 4개의 하위 프로젝트(SP)로 나눈다. SP마다 spec → plan → 구현 사이클을 따로 거친다. 이 문서는 **SP1만** 다룬다.

| SP | 내용 | 완료 기준 |
|---|---|---|
| **SP1** | `anyps5-wasm-aot`(Rust) + 게스트 ABI 계약 | 4절 참고 |
| SP2 | wasm 런타임 + 시스템 라이브러리 Emscripten 포팅 (화면 없음) | Dreaming Sarah가 브라우저에서 첫 GPU 명령 제출까지 진행 (로그로 확인) |
| SP3 | WebGPU 렌더러 (`libSceAgcDriver`의 Vulkan 백엔드 대체, 셰이더 IR → WGSL) | 첫 화면 출력 |
| SP4 | 브라우저 호스트 완성 (파일 시스템, 입력, 오디오, 세이브) | Dreaming Sarah 플레이 가능 |

## 3. 확정된 결정

| 항목 | 결정 | 이유 |
|---|---|---|
| 대상 게임 | Dreaming Sarah (PPSA02929) | 2D, eboot 약 7.7MB, 번들 모듈은 `libc.prx` 하나 |
| 네이티브 기준선 | 없음 | 네이티브 경로에서 실행 확인이 안 됨. 그래서 AOT는 독립 오라클(Unicorn)로 검증한다 |
| 브라우저 | Chromium 계열(Chrome/Edge 데스크톱)만 | memory64, 공유 메모리 스레드, WebGPU가 모두 안정적 |
| 메모리 모델 | wasm64 (memory64) | 라이브러리 코드가 LP64 구조체 레이아웃을 가정한다. wasm32에서는 포인터 필드 크기가 달라져 깨진다 |
| 링크 방식 | 자체 런타임 링커 | Emscripten의 SIDE_MODULE + pthreads 조합은 불안정하다. game.wasm은 일반 wasm 모듈로 두고, 스레드마다 직접 인스턴스화한다 |
| AOT 실행 위치 | 네이티브 CLI 먼저 | 핵심은 파일 I/O 없는 라이브러리로 만들어 나중에 브라우저용 wasm 빌드가 가능하게 한다 |

### 알려진 제약

- Chrome의 memory64 상한은 16GB이다. 게스트 주소가 이 범위를 넘으면 실행할 수 없다.
- wasm64는 wasm32보다 느릴 수 있다 (작업에 따라 10%~100%). 성능 최적화는 SP2에서 측정한 뒤에 한다.

## 4. SP1 목표

### 완료 기준

1. Unicorn 차등 테스트가 모두 통과한다.
2. Dreaming Sarah의 relinker 출력(`eboot` + `libc.prx`)이 전부 변환된다.
   - 지원하지 않는 명령어가 0개이다.
   - 결과 wasm이 `wasmparser` 검증과 wasmtime 컴파일을 통과한다.
3. 작은 테스트 ELF가 wasmtime의 스텁 호스트에서 NID 호출과 콜백을 정상 수행한다.

### 범위 밖 (SP2 이후)

- Emscripten 라이브러리 빌드와 NID 썽크 생성.
- 브라우저 로더와 스레드별 인스턴스화.
- 렌더링, 오디오, 입력, 파일 시스템.
- 성능 최적화 (구조화된 제어 흐름 복원, 레지스터 flush 최소화).

## 5. AOT 변환기 구조

### 5.1 위치와 빌드

- crate 경로: `core/wasm-aot/`. 패키지 이름: `anyps5-wasm-aot`.
- 처음에는 CMake와 별도로 `cargo`로 빌드한다.
- 핵심 로직은 라이브러리(`lib.rs`)에 둔다. 파일 I/O는 CLI(`main.rs`)에서만 한다.

### 5.2 입력

relinker의 Linux 출력 디렉터리이다.

```
<out>/
    app.elf                    # 메인 실행 파일 (NID 이름 import)
    app0/sce_module/<모듈>     # 변환된 게스트 모듈 (예: libc.prx에서 만든 것)
```

CLI:

```
anyps5-wasm-aot <app.elf> -o game.wasm [--report report.json] [--allow-unsupported]
```

- 게스트 모듈은 `app.elf`의 `DT_NEEDED`(`$ORIGIN/app0/...`)를 따라 찾는다.

### 5.3 파이프라인

| 단계 | 모듈 | 하는 일 |
|---|---|---|
| load | `elf` | PT_LOAD, 동적 심볼(NID 이름), 재배치, PT_TLS, PT_GNU_EH_FRAME, init/fini 배열, eh_frame을 읽는다 |
| bind | `bind` | import를 해석한다 (5.4절) |
| discover | `discover` | 코드 위치를 찾고 디코딩한다 (5.5절) |
| lift | `codegen/ops` | x86 명령어를 wasm으로 바로 바꾼다 (별도 IR 없음, 11절). 명령어 계열별 파일로 나눈다: 정수 연산, BMI, 데이터 이동, 원자 연산, 문자열, 제어, 시스템, SIMD, x87 |
| optimize | `opt` | 플래그 생존 분석으로 죽은 플래그 계산을 지운다 |
| emit | `emit` | game.wasm을 쓰고 `wasmparser`로 검증한다 |
| report | `report` | 함수 수, 미지원 명령어, 간접 분기 통계를 JSON으로 쓴다 |
| — | `abi` | ABI 상수와 이미지 메타데이터 형식. C 헤더 `aps5_wasm_abi.h`를 생성한다 |

### 5.4 바인딩 규칙

- 심볼 검색 순서는 ld.so와 같다: exe 자신 → `DT_NEEDED` 너비 우선.
- relinker는 시작 시 필요한 게스트 모듈을 호스트 라이브러리보다 먼저 `DT_NEEDED`에 넣는다 (`core/relinker/relinker/src/guest/GuestModuleBuilder.cpp:286`). 그래서 같은 NID가 있으면 게스트 모듈이 이긴다.
- 게스트 이미지의 export로 해석되면: 직접 wasm 호출로 연결한다.
- 해석되지 않으면: `nid` import가 된다 (시스템 라이브러리 wasm이 제공).
- import 심볼의 종류는 `st_type`으로 정한다.
  - `STT_FUNC` → 함수 import.
  - `STT_OBJECT` → 데이터 import (재배치 종류 `HostData`).
  - `STT_NOTYPE` → `R_X86_64_JUMP_SLOT`이면 함수, 그 외에는 변환 오류.
- 실행 중에 불러오는 모듈(`sceKernelLoadStartModule`)은 AOT 시점에 출력 디렉터리에 있는 것만 지원한다. 이 모듈의 import는 시작 시 범위 + 자기 자신 + 자기 `DT_NEEDED`로 해석한다.

### 5.5 코드 찾기

다음 주소에서 재귀 하강 디코딩(`iced-x86`)을 시작한다.

- ELF 진입점, export된 함수, `DT_INIT`/`DT_FINI`, init/fini 배열 항목.
- eh_frame FDE 시작 주소.
- 실행 세그먼트를 가리키는 재배치 대상 (C++ vtable 포함).
- 실행 세그먼트를 가리키는 `lea reg, [rip+disp]` 대상.
- 직접 호출 대상.

점프 테이블은 다음 패턴으로 복원한다: 범위 검사(`cmp idx, N` + `ja`) → 테이블 읽기(`movsxd`/`mov` + 기준 주소) → `jmp reg`. 복원에 실패한 간접 `jmp`은 실행 시 주소 조회로 처리한다.

### 5.6 사용하는 crate

| 용도 | crate |
|---|---|
| x86 디코딩 | `iced-x86` |
| eh_frame | `gimli` |
| wasm 출력 | `wasm-encoder` |
| wasm 검증 | `wasmparser` |
| 테스트 오라클 | `unicorn-engine` (dev-dependency) |
| 테스트 실행 | `wasmtime` (dev-dependency) |

## 6. 게스트 ABI 계약

SP2의 시스템 라이브러리 런타임이 그대로 구현해야 하는 인터페이스이다. 상수는 AOT의 `abi` 모듈이 원본이고, C 헤더 `aps5_wasm_abi.h`를 생성해서 양쪽이 같은 값을 쓴다. 계약 버전은 `APS5_WASM_ABI_VERSION = 1`이다.

### 6.1 메모리

- game.wasm은 `env.memory`를 import한다 (memory64, 기본 shared).
  - 테스트용으로 emit 옵션 `shared_memory = false`를 둔다. 원자 명령어는 공유되지 않는 메모리에서도 유효하다.
- **게스트 주소 = wasm 선형 메모리 주소**이다. 주소 변환이 없다. 그래서 라이브러리 C++ 코드는 게스트 포인터를 지금처럼 바로 쓴다.
- 이미지 기준 주소는 import된 불변 global로 받는다: `env.__aps5_image_base_<i>` (i64). `<i>`는 메타데이터의 이미지 순서이다.
- 런타임은 메모리를 할당하고, 이미지를 복사·재배치한 뒤에 game.wasm을 인스턴스화한다.

### 6.2 CPU 컨텍스트 `GuestCpu`

게스트 스레드마다 1개이고 선형 메모리 안에 있다. 64바이트 정렬, 크기 0x400.

| 오프셋 | 필드 | 크기 | 내용 |
|---|---|---|---|
| 0x000 | `gpr[16]` | 8×16 | x86 인코딩 순서: rax, rcx, rdx, rbx, rsp, rbp, rsi, rdi, r8…r15 |
| 0x080 | `rip` | 8 | 함수에 들어갈 때 들어가는 주소, `ret` 후 복귀 주소 |
| 0x088 | `rflags` | 8 | `pushf`/`popf`와 디버깅용으로만 실체화 |
| 0x090 | `fs_base` | 8 | TLS 기준 주소 |
| 0x098 | `gs_base` | 8 | |
| 0x0A0 | `stop_rip` | 8 | 디스패처 종료 주소 |
| 0x0A8 | `mxcsr` | 4 | |
| 0x0AC | 예약 | 0x14 | 0으로 둔다 |
| 0x0C0 | `ymm[16]` | 32×16 | xmm은 각 항목의 하위 16바이트 |
| 0x2C0 | `x87_st[8]` | 8×8 | x87 레지스터 (f64, 물리 순서: st(i) = `(top + i) & 7`) |
| 0x300 | `x87_top` | 4 | x87 스택 TOP |
| 0x304 | `x87_fcw` | 2 | x87 제어 워드. 초기값 0x037F |
| 0x306 | `x87_fsw` | 2 | x87 상태 워드 (TOP 제외) |
| 0x308 | `x87_tags` | 2 | 비트 i = 물리 레지스터 i에 값이 있음 |
| 0x30A | 예약 | 0xF6 | 0으로 둔다 |

- 런타임은 스레드를 만들 때 `mxcsr = 0x1F80`, `x87_fcw = 0x037F`, 나머지 0으로 초기화한다.

### 6.3 import

| 모듈.이름 | 타입 | 역할 |
|---|---|---|
| `env.memory` | memory64 | 공유 선형 메모리 |
| `env.__aps5_image_base_<i>` | global i64 | 이미지 기준 주소 |
| `env.__aps5_stub_base` | global i64 | 함수 포인터로 쓰인 import의 가짜 주소 영역 시작 |
| `env.__aps5_addr_map` | global i64 | 주소 조회 해시 테이블 위치 (6.7절) |
| `nid.<NID>` | `(ctx: i64) → ()` | 시스템 라이브러리 함수. 모든 NID가 같은 타입 |
| `aps5.cpuid` | `(ctx: i64) → ()` | eax/ecx 입력으로 Zen 2 정보를 채운다 |
| `aps5.rdtsc` | `(ctx: i64) → ()` | edx:eax에 시간 카운터를 쓴다 |
| `aps5.cmpxchg16b` | `(ctx: i64, addr: i64) → ()` | 전역 잠금으로 128비트 비교 교환 |
| `aps5.unresolved_indirect` | `(ctx: i64, addr: i64) → ()` | 변환 안 된 주소로 간접 분기. 치명 오류 |
| `aps5.unsupported` | `(ctx: i64, addr: i64) → ()` | 트랩 명령(`ud2`, `int3`, `hlt`, 디코딩 실패)과 `--allow-unsupported`의 미지원 명령. 치명 오류 |

### 6.4 export

| 이름 | 타입 | 역할 |
|---|---|---|
| `__aps5_dispatch` | `(ctx: i64) → ()` | `ctx.rip`부터 실행한다. `rip == stop_rip`이 되면 반환한다 |
| `__aps5_abi_version` | `() → i32` | `APS5_WASM_ABI_VERSION`을 반환한다 |

### 6.5 NID 호출 규약

1. 복귀 주소를 게스트 스택에 넣는다 (`rsp -= 8`, `[rsp] = 복귀 주소`). x86 `call`과 같다.
2. wasm 로컬의 레지스터를 모두 `ctx`에 쓴다 (SP1에서는 전부 flush).
3. `nid.<NID>(ctx)`를 호출한다.
   - 라이브러리 썽크는 SysV 규칙으로 인자를 읽는다: rdi, rsi, rdx, rcx, r8, r9, xmm0–7, 그리고 `[rsp+8]`부터 스택 인자.
   - 결과는 rax, rdx, xmm0, xmm1에 쓴다.
   - 썽크는 `ret`을 흉내 내서 `ctx.rip`에 복귀 주소를 pop한다.
4. 레지스터를 `ctx`에서 다시 읽는다.
5. `ctx.rip`이 예상 복귀 주소와 같은지 확인한다. 다르면 비지역 이동(6.8절)으로 처리한다.

- 모든 NID가 같은 타입이므로 AOT는 함수 시그니처를 몰라도 된다. 시그니처별 썽크는 SP2에서 C++ 템플릿으로 만든다.
- import는 `ctx`의 레지스터와 `rip`, `rsp`를 마음대로 바꿀 수 있다. 예: 시스템 libc의 언와인더(`core/libs/prx/libc/src/exception/Unwind.cpp`)가 예외 처리기로 이동할 때.

### 6.6 콜백 `aps5_guest_call` (SP2 런타임이 구현)

1. `ctx`의 현재 내용을 저장한다.
2. SysV 규칙으로 인자를 넣는다. 스택은 `(rsp - 128) & ~15`로 잡는다 (레드존 회피).
3. 가짜 복귀 주소를 push하고 `stop_rip`으로 지정한다. `rip`에 대상 주소를 넣는다.
4. `__aps5_dispatch(ctx)`를 호출한다.
5. rax(필요하면 xmm0)를 꺼내고 `ctx`를 복원한다.

- 중첩 디스패처 안에서 `rsp`가 진입 시점보다 위로 올라가면 (게스트가 라이브러리 프레임을 넘어 언와인드), 런타임은 치명 오류로 중단한다. 이것은 지원하지 않는다.

### 6.7 함수 포인터와 주소 조회

- 간접 분기는 주소 → 테이블 인덱스 조회 후 `call_indirect` 한다.
- 조회 순서:
  1. 이미지 범위 검사: `addr - base_i < span_i`인 이미지를 찾는다 (가짜 import 영역 포함).
  2. 해시 테이블에서 (이미지, 오프셋)을 찾는다. 테이블은 메타데이터에 있고 런타임이 `__aps5_addr_map` 위치로 복사한다.
  3. 없으면 `aps5.unresolved_indirect`를 호출한다.
- 해시 테이블은 2의 거듭제곱 크기, 선형 탐사, 항목은 `{key: u64 = (image << 48) | offset, index: u32, pad: u32}`이다.
- 함수 포인터로 쓰인 호스트 import는 가짜 주소를 받는다: `stub_base + 16 × k`. 조회하면 스텁 함수 `k`가 나온다. 스텁 함수는 `nid.<NID>(ctx)`를 호출한다.
- game.wasm은 자체 funcref 테이블을 정의한다. 라이브러리와 공유하지 않는다. 인스턴스마다 같은 내용으로 초기화된다.

### 6.8 비지역 이동 (예외, longjmp)

- 호출 후 `ctx.rip`이 예상 복귀 주소와 다르면, wasm 함수는 즉시 반환한다.
- 이 반환이 최상위 디스패처까지 이어진다. 디스패처는 `ctx.rip`이 속한 함수로 다시 들어간다.
- 이것이 동작하는 이유: 변환된 코드는 게스트 스택 내용(push, 프레임 배치)을 네이티브 실행과 똑같이 유지한다. 그래서 eh_frame 기반 언와인더가 게스트 프레임을 정상적으로 해석한다.

### 6.9 이미지 메타데이터 (custom section `anyps5.images`)

리틀 엔디언 바이너리이다. 문자열은 `u32 길이 + UTF-8`이다.

**헤더**

| 필드 | 타입 |
|---|---|
| magic | `"APS5IMG\0"` |
| version | u32 (= 1) |
| image_count | u32 |
| stub_count | u32 |
| addr_map_offset, addr_map_size | u64, u64 (blob 안 위치) |

**이미지 레코드** (image_count개)

| 필드 | 내용 |
|---|---|
| name | 문자열 |
| span, align | u64. 기준 주소부터 마지막 세그먼트 끝까지 크기, 정렬 |
| segments | 개수 + `{vaddr u64, filesz u64, memsz u64, flags u32 (R/W/X), blob_offset u64}` |
| phdrs | 개수 + `{type u32, flags u32, vaddr u64, memsz u64}`. 런타임의 `dl_iterate_phdr` 대체 구현용 (PT_GNU_EH_FRAME 포함) |
| tls | 있음 u8 + `{init_vaddr u64, init_size u64, mem_size u64, align u64}` |
| entry | u64 vaddr. 없으면 `u64::MAX` |
| init, fini | u64 vaddr. 없으면 `u64::MAX` |
| init_array, fini_array | `{vaddr u64, count u32}` (재배치 후의 값을 런타임이 읽는다) |
| needed | 개수 + 문자열 |
| exports | 개수 + `{nid 문자열, vaddr u64, kind u8 (함수/데이터)}` |
| relocations | 개수 + `{offset u64, kind u8, addend i64, target u32}` |

**재배치 종류**

| kind | 이름 | 계산 |
|---|---|---|
| 1 | `Relative` | `*(base + offset) = base + addend` |
| 2 | `GuestSymbol` | `*(base + offset) = base_of(sym.image) + sym.vaddr + addend` (`target` = 심볼 표 인덱스) |
| 3 | `ImportStub` | `*(base + offset) = stub_base + 16 × target + addend` |
| 4 | `HostData` | `*(base + offset) = 라이브러리 데이터 심볼 주소(NID) + addend` |
| 5 | `TlsModuleId` | `*(base + offset) = 이미지의 TLS 모듈 ID` (`R_X86_64_DTPMOD64`) |
| 6 | `TlsOffset` | `*(base + offset) = 정적 TLS 오프셋 + sym.vaddr + addend` (`R_X86_64_TPOFF64`) |
| 7 | `DtpOffset` | `*(base + offset) = sym.vaddr + addend` (`R_X86_64_DTPOFF64`) |

- TLS 심볼(`TlsOffset`, `DtpOffset`)의 `sym.vaddr`는 그 이미지 TLS 블록 안의 오프셋(`st_value`)이다.

**뒤따르는 표**

- 심볼 표: `{image u32, vaddr u64}` 목록. `GuestSymbol`, `TlsOffset`, `DtpOffset`이 참조한다.
- 스텁 표: stub_count개의 NID 문자열.
- 호스트 데이터 표: `HostData`가 참조하는 NID 문자열.
- blob: 세그먼트 바이트와 주소 조회 해시 테이블.

### 6.10 스레드 안전성

- game.wasm에는 변경 가능한 전역 상태가 없다. 모든 상태는 `ctx`와 공유 메모리에 있다.
- 그래서 스레드(Web Worker)마다 같은 import로 인스턴스를 하나씩 만들면 된다.

## 7. 변환 모델

### 7.1 단위

- 게스트 함수 1개 → wasm 함수 1개 `(ctx: i64) → ()`.
- 들어갈 때 `ctx.rip`에는 들어가는 주소가 있다. 직접 호출하는 쪽이 이 값을 쓴다.
- 레지스터는 wasm 로컬에 둔다. 정수는 `i64`, 벡터는 `v128`이다. ymm은 `v128` 2개이다.
- 쓰는 레지스터만 들어갈 때 읽고, 호출·반환·나갈 때 `ctx`에 쓴다.
- wasm `name` 섹션의 함수 이름에 게스트 주소를 넣는다 (예: `g_0000000000401a20`). 브라우저 스택 추적에서 위치를 바로 알 수 있다.

### 7.2 함수 내부 제어 흐름

- 기본 블록을 `loop` + `br_table` 상태 기계로 만든다 (로컬 `$pc`).
- 진입 블록은 `ctx.rip`으로 고른다. 진입 가능한 블록: 함수 시작, 호출 복귀 지점, 점프 테이블 대상, 알려진 간접 분기 대상.
- 구조화된 제어 흐름 복원은 SP1에서 하지 않는다.

### 7.3 호출과 반환

| x86 | wasm |
|---|---|
| `call 직접주소` | 복귀 주소 push → flush → `ctx.rip = 대상` → `call $f` → reload → `ctx.rip == 복귀 주소` 확인 |
| `ret` / `ret imm16` | 복귀 주소를 `ctx.rip`으로 pop (`rsp += 8 + imm`) → flush → `return` |
| 다른 함수로 `jmp` (꼬리 호출) | flush → `ctx.rip = 대상` → `return_call $f` |
| `call reg/mem` | 주소 조회 → `call_indirect` → 복귀 확인 |
| `jmp reg/mem` (함수 밖) | 주소 조회 → `return_call_indirect` |
| PLT 스텁 호출 | GOT를 거치지 않고 대상에 직접 연결 (NID import 또는 게스트 함수) |

### 7.4 디스패처 `__aps5_dispatch`

```
loop:
    if ctx.rip == ctx.stop_rip: return
    idx = lookup(ctx.rip)
    call_indirect idx (ctx)
```

### 7.5 플래그

- CF, PF, AF, ZF, SF, OF는 각각 `i32` 로컬이다. DF는 별도 로컬이다.
- 생존 분석으로 나중에 읽히는 플래그만 계산한다.
- 가정: 함수 경계(호출, 반환, 함수 시작)에서 플래그는 죽어 있고 DF는 0이다 (SysV ABI).
- 검증: 함수 시작부터 정의 없이 플래그를 읽는 경로가 있으면 변환 오류이다.
- `pushf`, `popf`, `lahf`, `sahf`는 필요한 플래그를 실체화한다.

### 7.6 메모리와 원자 연산

- 주소는 `i64`로 계산하고 memory64 load/store를 쓴다.
- `fs:`/`gs:` 접두사는 `ctx.fs_base`/`ctx.gs_base`를 더한다.
- RIP 상대 주소는 `image_base + (명령어 vaddr 기준 계산값)`이다.
- `lock` 명령어 → wasm atomic RMW, `cmpxchg` → `atomic.rmw.cmpxchg`. 플래그는 비교 결과로 계산한다.
- `xchg mem, reg`는 암묵적 `lock`으로 처리한다.
- 정렬되지 않은 원자 접근은 wasm 트랩이 된다. 지원하지 않는다.
- `cmpxchg16b` → `aps5.cmpxchg16b`.
- `mfence`/`lfence`/`sfence` → `atomic.fence`. `prefetch*`, `pause`, `endbr64` → 아무것도 하지 않음.
- `rep movsb/stosb`(DF = 0) → `memory.copy`/`memory.fill`. 겹치는 `movs`는 x86의 앞에서부터 복사 결과와 같도록 바이트 루프로 처리한다.

### 7.7 SIMD와 부동소수점

- 대상: SSE, SSE2, SSE3, SSSE3, SSE4.1, SSE4.2, AVX, AVX2, FMA, F16C, BMI1, BMI2, LZCNT, POPCNT, MOVBE.
- ymm 연산은 `v128` 2개로 나눈다. VEX 128비트 연산은 ymm 상위를 0으로 만든다.
- x86 고유 결과를 흉내 낸다. 예: NaN·오버플로 정수 변환은 `0x80000000…`, `minps`/`maxps`의 피연산자 순서 규칙.
- 문서화된 차이:
  - MXCSR FTZ/DAZ는 무시한다 (비정규 수에서만 차이).
  - 기본(가장 가까운 값)이 아닌 반올림 모드를 설정하는 `ldmxcsr`는 실행 시 치명 오류이다.
  - FMA는 relaxed SIMD `madd`/`nmadd`로 계산한다. x86-64(FMA3)와 AArch64 엔진은 이를 융합 연산으로 실행한다.
  - 새로 생기는 NaN(예: 음수의 제곱근)의 부호는 x86에서 1, AArch64 호스트에서 0이다. 입력 NaN의 전파 규칙도 호스트를 따른다.
- x87은 f64로 계산한다 (11절).
- `cpuid`, `rdtsc` → 런타임 import. `cpuid`는 Zen 2 정보를 돌려준다.
- `ud2`, `int3`, `hlt` → 게스트 주소를 담은 트랩.
- `syscall` → 변환 오류 (relinker가 미리 거부한다).

### 7.8 명령어 범위

- 처음부터 x86 전체를 구현하지 않는다.
- Dreaming Sarah와 테스트 프로그램의 커버리지 보고서에 나온 명령어만 구현한다.

## 8. 테스트

### 8.1 도구

- x86-64 ELF: `clang --target=x86_64-unknown-linux-gnu`로 컴파일하고 rustup의 `rust-lld -flavor gnu`로 링크한다.
- 실행: `wasmtime` crate (memory64 사용).
- 오라클: `unicorn-engine` crate.

### 8.2 3단계

| 단계 | 방법 | 비교·검증 |
|---|---|---|
| 1. 명령어 차등 테스트 | `iced-x86` 인코더로 명령어 조합을 만든다. 무작위 초기 상태에서 Unicorn과 wasmtime(변환 결과)으로 실행한다 | GPR, 정의된 플래그, 메모리, xmm/ymm |
| 2. 통합 테스트 | C 테스트 프로그램 → x86-64 ELF (NID 이름 import 포함) → AOT → wasmtime + 스텁 호스트 | NID 호출, 콜백, 간접 호출, 점프 테이블, vtable, C++ 예외, longjmp, TLS |
| 3. 실제 게임 | Dreaming Sarah relinker 출력 → AOT | 미지원 명령어 0개, 검증 통과, wasmtime 컴파일 성공 |

- 1단계는 "출구에서 모든 플래그가 살아 있음" 모드로 변환한다. 플래그 제거 때문에 비교가 빠지는 것을 막는다.
- 정의되지 않은 플래그(예: `imul` 후 SF/ZF)는 비교에서 뺀다. 명령어별 정의 표를 둔다.
- 3단계 입력은 커밋하지 않는다. 경로는 환경 변수 `ANYPS5_DREAMING_SARAH_DIR`로 받는다. 없으면 테스트를 건너뛴다.

### 8.3 스텁 호스트 (Rust, 테스트 전용)

- SP2 런타임의 축소판이다: 이미지 복사, 재배치, TLS 준비, init 호출, `aps5_guest_call`, 주소 조회 테이블 복사.
- 제공하는 NID는 몇 개뿐이다: 출력, 메모리 할당, 콜백 호출, `__tls_get_addr`, 간단한 언와인드 이동.
- 역할: ABI 계약 검증과, SP2가 C++로 옮길 참조 구현.

## 9. 오류 처리

| 상황 | 시점 | 동작 |
|---|---|---|
| 미지원 명령어 | 변환 | 실패. 주소, 바이트, 니모닉 출력. `--allow-unsupported`면 `aps5.unsupported` 호출 삽입 |
| 정의 없이 플래그를 읽는 경로 | 변환 | 실패. 함수와 블록 주소 출력 |
| 해석 안 되는 재배치·심볼 종류 | 변환 | 실패 |
| `syscall` 명령어 | 변환 | 실패 |
| 변환 안 된 주소로 간접 분기 | 실행 | `aps5.unresolved_indirect` → 주소 출력 후 중단 |
| 정렬 안 된 원자 접근 | 실행 | wasm 트랩. 함수 이름으로 게스트 주소 확인 |
| 기본이 아닌 반올림 모드 | 실행 | 치명 오류 |
| 라이브러리 프레임을 넘는 언와인드 | 실행 | 런타임이 치명 오류로 중단 |
| ABI 버전 불일치 | 로드 | 런타임이 거부 |

## 10. 위험과 대응

| 위험 | 대응 |
|---|---|
| 명령어 수가 많다 (AVX2 등) | 커버리지 보고서 기준으로만 구현한다 |
| 간접 분기 대상을 놓친다 | eh_frame, 재배치, `lea` 대상, 점프 테이블을 모두 수집한다. 실행 시 놓친 주소는 명확히 보고한다 |
| wasmtime이 shared memory64를 지원하지 않을 수 있다 | 테스트는 `shared_memory = false`로 실행한다. 계획의 첫 작업에서 확인한다 |
| 네이티브 기준선이 없다 | AOT는 Unicorn으로 독립 검증한다. 라이브러리 문제는 SP2에서 다룬다 |
| 성능 (wasm64, `br_table` 루프, 전부 flush) | SP1에서는 정확성을 우선한다. SP2에서 측정 후 최적화한다 |

## 11. 구현 결과와 설계 변경

### 11.1 완료 기준 결과

| 기준 | 결과 |
|---|---|
| 1. 차등 테스트 | 정수 22개, SIMD 8개, x87 6개 그룹 통과 (그룹마다 무작위 상태 12~24개) |
| 2. Dreaming Sarah | 함수 10,904개, 명령어 약 89.8만 개, 미지원 명령 0개, wasm 검증 통과, wasmtime 컴파일(shared memory64) 성공. `tests/game.rs` |
| 3. 실제 ELF | NID 호출(정수·실수 인자), 콜백, 점프 테이블, vtable, TLS(GD·IE), 생성자, import에 의한 비지역 이동, 게스트 longjmp, C++ 예외 (cleanup + `catch (...)`). `tests/elf.rs` |

### 11.2 바뀐 설계

| 항목 | 원래 설계 | 구현 | 이유 |
|---|---|---|---|
| IR | x86 → IR → wasm | x86 → wasm 직접 생성 | 플래그 생존 분석은 iced의 명령어 정보로 충분하다. wasm 엔진이 로컬 최적화를 한다 |
| ELF 파싱 | `object` crate | 직접 파싱 (ELF64 고정 구조체) | crate API 변경에 묶이지 않는다 |
| FMA | f64 경유 | relaxed SIMD | 위 7.7절 |
| 메모리 import 최대값 | 항상 16 GiB | shared일 때만 16 GiB | 공유 메모리는 최대값이 필요하다. 테스트 메모리는 16 GiB를 넘는다 |
| 테스트 오라클 | Unicorn | 정수: Unicorn, SIMD·x87: 네이티브 실행 (Apple silicon에서는 Rosetta) | Unicorn 2.1(QEMU 5)은 AVX를 실행하지 못한다 |
| 트랩 | — | `ud2`, `int3`, `hlt`, 디코딩 실패는 `aps5.unsupported`를 호출한다 | 런타임이 게스트 주소를 보고한다 |

### 11.3 추가된 모듈 수준 도우미 함수

game.wasm 안에 들어간다. 런타임이 제공할 것은 없다.

| 이름 | 역할 |
|---|---|
| `umulh`, `smulh` | 64×64 곱의 상위 64비트 |
| `udiv128`, `sdiv128` | 128/64 나눗셈. 0으로 나누기와 몫 넘침은 트랩 (#DE) |
| `x80_load`, `x80_store` | 80비트 확장 실수 ↔ f64 |
| `scalbn` | `fscale`용 2의 거듭제곱 곱 |
| `pcmpstr` | SSE4.2 문자열 비교의 IntRes2 계산 |

### 11.4 코드 찾기 보강

- 경로(trace)를 따로 디코딩하고, 이미 디코딩한 코드와 겹치지 않을 때만 반영한다.
- 점프 테이블:
  - 경로 안에서 범위 검사를 못 찾으면, 디코딩이 끝난 뒤 주소 순서의 앞 256개 명령으로 다시 찾는다 (고정점 반복).
  - 인덱스 별칭: `mov`, `movzx`, `movsx`, `movsxd`. 16비트 비교 지원. `add base, off; jmp base` 형태 지원.
  - 점프 레지스터를 마지막으로 쓴 명령이 `add`가 아니면 테이블이 아니다 (포인터 꼬리 호출).
  - 범위로 읽은 대상은 모두 같은 함수(FDE 범위)의 코드여야 한다. 아니면 버린다.
  - 범위 검사가 없는 테이블은 마지막에 "추정"한다: 다음 데이터 레이블이나 기존 명령의 중간을 만나면 멈춘다. 추정에서 파생된 코드가 겹치면 오류 대신 버리고, 그 코드의 분기가 버려진 주소로 가면 실행 시 트랩을 넣는다.
- 보고서: `unresolved_jumps`(테이블 모양인데 못 찾음), `pointer_jumps`, `guessed_tables`. Dreaming Sarah: 0 / 352 / 57.

### 11.5 import 종류 (함수/데이터)

- relinker는 원래 TLS가 아닌 모든 import를 `STT_FUNC`로 썼다. 원본 PS5 심볼의 함수 여부를 쓰도록 고쳤다: 데이터 import는 `STT_OBJECT` (`SysVDynamicSectionBuilder.cpp`, 테스트 `test_import_symbol_types.py`). ld.so 바인딩에는 영향이 없다.
- AOT 규칙: `STT_FUNC` → 함수, `STT_OBJECT` → 데이터, `STT_NOTYPE` → 같은 이미지에 그 심볼의 `JUMP_SLOT`이 있으면 함수, 없으면 데이터.

### 11.6 x87 근사

- 값은 f64로 저장·계산한다. 80비트 메모리 형식은 정확히 변환한다 (f64로 반올림).
- `fprem`/`fprem1`은 한 번에 나머지를 구하고 C2를 0으로 둔다. C0·C3·C1은 몫의 하위 3비트다.
- `fxtract`는 0과 비정규 수를 따로 다루지 않는다.
- 반올림 모드(RC)는 `fistp`/`frndint`에만 적용한다.

### 11.7 SP2로 넘기는 참조 구현

- `tests/common/host.rs`: 이미지 적재, 재배치, 정적 TLS(variant II), `aps5_guest_call`, NID 호출 규약.
- `tests/common/unwind.rs`: `GuestCpu`와 게스트 스택만으로 CFI를 따라 프레임을 되감고 LSDA 착지 패드에서 재개하는 언와인더. 시스템 libc의 `_Unwind_RaiseException`/`_Unwind_Resume`이 같은 일을 해야 한다.

### 11.8 남은 일 (SP2 이후)

- 성능: 호출마다 사용 레지스터를 모두 저장·복원한다. game.wasm은 약 57 MB이다.
- 추정 점프 테이블 57개는 실행 중 `unresolved_indirect`로 검증해야 한다.
- x86의 NaN 부호를 맞추는 옵션은 필요할 때 추가한다.
