# Whistle Spoofing (휘파람 조작)

## 기능명

**Whistle Spoofing** — 게임 내 "도발(Provocation)" = 휘파람 기능의 관측·조작.

| 세부 기능 | 설명 |
|---|---|
| 휘파람 위치 추출 | 도발이 울린 액터의 월드 좌표·거리·플레이어 이름 |
| 역할 위조 | 술래가 휘파람을 냄 (원래 불가능한 동작) |
| 임의 대상 강제 발동 | 관전 상태에서 **다른 플레이어**를 지목해 휘파람 강제 |
| 소리 교체 | 도발 사운드를 임의 사운드로 (로컬 전용) |
| ProcessEvent 로거 | 블루프린트 이벤트·RPC 실시간 추적 |

> 게임 내부 명칭이 일본어 기반이라 `whistle` 로는 검색되지 않는다.
> 휘파람 = **`Provocation`**, 사운드 자산은 **`SC_Provoaction`** (개발사 오타, `c` 누락).

---

## 구현 방식

**C++ DLL 인젝션.** UE4SS 미사용.

후킹은 세 가지를 쓴다.

| 기법 | 대상 | 용도 |
|---|---|---|
| vtable 후킹 | `UObject::ProcessEvent` / `AActor::ProcessEvent` | 블루프린트 이벤트·RPC 관측 |
| 직접 `ProcessEvent` 호출 | 임의 `UFunction` | 입력 핸들러 게이트 우회 |
| `ExecFunction` 포인터 후킹 | `UAudioComponent::Play` | 사운드 교체 |

SDK 는 **Dumper-7 이 생성한 C++ 헤더**를 그대로 include 한다 (하드코딩 오프셋 아님).

---

## 필요한 프로그램 및 라이브러리

| | 비고 |
|---|---|
| **Visual Studio 2022 Build Tools** | Desktop C++ 워크로드 |
| **CMake** | Build Tools 의 `C++ CMake tools` 컴포넌트로 설치 가능 |
| **Python 3** | 인젝터·SDK 질의 도구용. 표준 라이브러리만 사용 (`ctypes`) |
| **Dumper-7 SDK** | 저장소에 없음. 아래 참조 |

외부 라이브러리 의존 없음 (MinHook·ImGui 등 사용 안 함).

### SDK 준비 — 필수

`CppSDK/` 는 **게임 바이너리 파생물이라 커밋하지 않는다.**
팀에서 공유한 4.0.2 덤프를 받거나 Dumper-7 로 직접 뽑아 이 폴더 아래에 넣는다.

```
modules/whistle-spoofing/
└── CppSDK/
    └── SDK/
        ├── Basic.hpp
        ├── CoreUObject_classes.hpp
        ├── Engine_classes.hpp
        └── ...
```

> ⚠️ **덤프는 반드시 매치 진입 상태에서 뜬 것이어야 한다.**
> 로비에서 뜨면 `cLeon_Character` / `Survivor` / `Hunter` 클래스가 통째로 빠진다.
> 로비 폰(`BP_FirstPersonCharacter_Main_C`)과 매치 폰(`..._cLeon_Character_C`)은
> 서로 다른 클래스이고, 후자는 매치가 시작돼야 로드된다.

---

## 실행 방법

### 1. 빌드

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

산출물: `bin/Release/whistle_v14.dll`

> `CMakeLists.txt` 의 `WHISTLE_VERSION` 을 올리면 파일명이 바뀐다.
> 이미 주입된 DLL 은 프로세스에 매핑돼 있어 링커가 덮어쓰지 못하므로(LNK1104),
> 코드를 고칠 때마다 번호를 올리면 게임 재시작 없이 재주입할 수 있다.

### 2. 주입

**관리자 권한 터미널**에서. 게임은 **로비 진입 후**에 주입한다.

```bash
python tools/inject.py bin/Release/whistle_v14.dll
```

콘솔 창이 뜨고 `[HOOK] 신규패치 3600+` 이 나오면 성공.
`후킹 실패` 가 뜨면 SDK 와 게임 빌드가 안 맞는 것이다.

---

## 사용 방법 / 단축키

### 발동

| 키 | 동작 | 필요 상태 |
|---|---|---|
| **F5** | `Provocation(Server)` 직접 호출 | 캐릭터 폰 |
| F4 / F6 | `Provocation(Local)` / `(Client)` 호출 | 캐릭터 폰 |
| **F3** | `ProvocationRemote` — 관전 중이 **아닌** 대상 지목 | **관전(사망) 상태** |
| F2 | `ProvocationRemote` — 관전 중인 대상 지목 | **관전(사망) 상태** |

F3 는 누를 때마다 다음 대상으로 순환한다. `PlayerState` 가 있는 실제 플레이어만 후보.

### 소리

| 키 | 동작 |
|---|---|
| DEL | 소리 교체 — 로드된 사운드 순환 (실측 239개), 한 바퀴 돌면 원복 |
| BKSP | `SC_Provoaction_HIKAKIN` 으로 지정 |
| INS | 볼륨 10배 토글 |
| TAB | 변수 직접 쓰기 — 덮어써지는 것 확인용 (비교 실험) |

### 조회

| 키 | 동작 |
|---|---|
| F1 | 관전 상태 + 지목 가능 대상 목록 (이름·역할·유효성) |
| F12 | 오디오 컴포넌트 상태 + `ForceProvocationInverval` |
| HOME / PgUp / PgDn | 사운드 목록 (전체 / `Provoaction` / `SC_*`) |
| F7 | 클래스별 후킹 상태 진단 |
| F8 | vtable 수동 재스캔 (2초마다 자동 실행됨) |
| F10 | 함수 호출 요약 |

로그 파일: `C:\Dumper-7\whistle-pe-log.txt`

---

## ON / OFF 방법

| | |
|---|---|
| **ON** | 주입 즉시 관측(로깅)은 자동 시작. 조작 기능은 각 단축키로 개별 발동 |
| **로깅 OFF** | **F9** |
| **소리 교체 OFF** | **DEL** 을 목록 끝까지 눌러 한 바퀴 돌리면 원본 복원 |
| **전체 OFF / 언로드** | **END** — vtable 과 exec 포인터를 전부 원복하고 언로드 |

> `END` 로 후킹은 해제되지만 DLL 모듈 자체는 언로드되지 않는 경우가 있다.
> 그래서 빌드 버전 번호를 올려 새 파일명으로 뽑는 방식을 쓴다.

---

## 테스트한 게임 버전

**4.0.2** — Steam `buildid 24929792` / Unreal Engine 5.6.1

- SDK 덤프도 같은 빌드에서 **매치 진입 상태**로 생성
- 이후 게임이 업데이트되면 오프셋이 전부 무효가 되며, 도구는 **후킹을 거부**한다
  (잘못된 주소로 진행해 크래시하지 않도록 fail-closed 설계)
- 롤백 방법: Steam 콘솔 `download_depot 4704690 4704691 2667002530778402204`
  → 받은 파일을 게임 폴더에 덮어쓰고, **Steam 라이브러리 대신 exe 직접 실행**
  (`Chameleon/Binaries/Win64/PenguinHotel-Win64-Shipping.exe`,
  같은 폴더에 `steam_appid.txt` 에 `4704690` 기록)

---

## 호스트 / 클라이언트 동작 차이 ★ 중요

**이 모듈은 클라이언트에서 실행해야 의미가 있다.**

P2P 구조라 호스트가 곧 서버다. 언리얼에서 **권한을 가진 쪽이 `NetServer` RPC 를
호출하면 네트워크를 타지 않고 곧바로 로컬 실행**된다. 서버의 수신 측 검증 경로를
아예 거치지 않는다.

| | 호스트에서 실행 | 클라이언트에서 실행 |
|---|---|---|
| `Provocation(Server)` | 로컬 즉시 실행 (왕복 0ms) | **네트워크 경유** (실측 79~234ms) |
| 서버 검증 통과 여부 | **검증 안 됨 — 결과 무의미** | 실제로 검증을 통과했음이 증명됨 |

> **호스트에서 테스트하면 "서버가 검증하지 않는다"는 결론이 성립하지 않는다.**
> 반드시 다른 사람이 방을 만들고 이쪽이 클라이언트로 참가할 것.

### 그 외 상태 의존성

| 기능 | 필요 상태 |
|---|---|
| F5 / F4 / F6 | 캐릭터 폰 빙의 중 (로비 폰에는 `Provocation` 자체가 없음) |
| **F2 / F3** | **사망 후 관전 상태** — 관전 폰을 빙의해야 RPC 가 서버에서 폐기되지 않음 |
| 강제 발동 관측 | 생존자만 대상. 술래는 강제 발동을 받지 않음 |

---

## 다른 모듈과 동시 실행

**후킹 대상:**

```
UObject::ProcessEvent   vtable 슬롯  약 3,400개
AActor::ProcessEvent    vtable 슬롯  약   220개
UAudioComponent::Play   ExecFunction 포인터
```

⚠️ **`ProcessEvent` 를 후킹하는 다른 모듈과 동시 실행하면 위험하다.**
두 번째로 주입된 쪽이 첫 번째의 후크를 "원본"으로 저장하게 되어,
언로드 순서에 따라 죽은 주소를 호출하고 크래시한다.
**UE4SS 도 `ProcessEvent` 를 후킹하므로 함께 쓸 때 같은 문제가 있다.**

`PostRender` 나 단순 메모리 읽기/쓰기만 하는 모듈과는 충돌하지 않는다.

### 통합 시 참고 — 이 코드의 후킹 코어에 반영된 것

같은 함정을 다시 밟지 않도록 적어둔다. 넷 다 **크래시 없이 조용히 실패**한다.

- **`AActor` 가 `ProcessEvent` 를 오버라이드한다.**
  언리얼은 `ProcessEvent` 구현이 하나가 아니다. 원본을 전역 변수 하나에 저장하면
  **액터 계열(캐릭터·폰 전부)이 통째로 후킹에서 빠진다.** vtable 마다 원본을 따로 보관해야 한다.
- **주기적 재스캔이 필요하다.** 매치 진입 시 새 클래스가 로드된다.
  `GObjects` 는 뒤로만 늘어나므로 마지막 인덱스부터 이어서 훑으면 비용이 거의 없다.
- **재진입 가드.** 후크 안에서 SDK 함수(`K2_GetActorLocation` 등)를 호출하면
  그것도 `ProcessEvent` 를 거쳐 후크에 다시 들어온다.
- **모듈 주소 범위 검증.** 코드 포인터가 아닌 값을 원본으로 저장하면 호출 순간 죽는다.

---

## 보조 도구

| 파일 | 용도 |
|---|---|
| `tools/inject.py` | DLL 인젝터. `ctypes` 만 사용, 컴파일러 불필요 |
| `tools/sdk_query.py` | Dumper-7 SDK 질의 (함수·플래그·멤버 오프셋 검색) |
| `tools/ue_sdk_dumper.py` | External 방식 덤퍼 (교차 검증용) |

```bash
python tools/sdk_query.py net               # 네트워크 RPC 전부
python tools/sdk_query.py show Provocation  # 시그니처 + 플래그
python tools/sdk_query.py members WBP_cLeonMain_C
```

---

## 주의

- **비공개 방에서만** 사용한다. 공개 매치메이킹 금지
- 빌드 산출물(`*.dll`)과 SDK 덤프(`CppSDK/`)는 커밋하지 않는다
- 강제 사망 계열(`KillPlayer`, `DeathPlayer`)은 존재를 확인했으나
  프로젝트 제외 항목이므로 구현하지 않았다
