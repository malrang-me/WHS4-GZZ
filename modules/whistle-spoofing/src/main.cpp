/*
 * whistle.dll — v0.1  ProcessEvent 로거
 *
 * 목적: T 키를 눌렀을 때 게임이 실제로 어떤 UFunction 을 호출하는지 확정한다.
 *
 * SDK 덤프는 "어떤 함수가 존재하는가"만 알려준다. 이름이 Whistle 이 아닐 수도 있고
 * (정적 조사 결과 BPI_cLeonRandomCall 이 유력 후보), 후보가 여러 개일 수도 있다.
 * ProcessEvent 를 후킹해 실제 호출을 로깅하면 그 모호함이 사라진다.
 *
 * 후킹 방식: vtable 후킹 (docs/12 §5)
 *   - 함수 본문(.text)을 건드리지 않으므로 코드 영역 해시 검사에 걸리지 않는다
 *   - 대신 클래스마다 vtable 이 따로 있으므로, GObjects 를 순회하며
 *     모든 UClass 의 vtable 을 찾아 ProcessEvent 슬롯을 전부 교체한다
 *   - vtable 포인터는 클래스 간에 공유되는 경우가 많아 중복 제거를 한다
 *
 * 조작:
 *   F9   로깅 on/off
 *   F10  지금까지 본 함수 요약 출력
 *   F11  필터 on/off (매 프레임 호출되는 Tick 류를 숨김/표시)
 *   END  후킹 해제 + 언로드
 */

#include <Windows.h>
#include <cstdio>
#include <cmath>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <mutex>
#include <shared_mutex>
#include <chrono>
#include <fstream>
#include <algorithm>

#include "SDK/Basic.hpp"
#include "SDK/CoreUObject_classes.hpp"
#include "SDK/Engine_classes.hpp"

/* 네임스페이스는 Dumper-7.ini 의 SDKNamespaceName 으로 정한 값이다.
   기본값은 SDK 지만 이 프로젝트는 CLeonSDK 로 뽑았다. */
using namespace CLeonSDK;

// ─────────────────────────────────────────────────────────────
// 설정
// ─────────────────────────────────────────────────────────────

static const char* kLogPath = "C:/Dumper-7/whistle-pe-log.txt";

/* 매 프레임 불려서 로그를 뒤덮는 함수들. 이름에 이 조각이 있으면 기본적으로 숨긴다. */
static const char* kNoise[] = {
    "Tick", "Update", "ReceiveTick", "BlueprintUpdate", "AnimNotify",
    "OnRep_", "K2_Tick", "ExecuteUbergraph", "BndEvt", "GetTime",
    "Timeline", "Interp", "SequenceEvent", "OnMouse", "MouseMove",
};

/* 이 조각이 이름에 있으면 노이즈 필터를 무시하고 무조건 보여준다.
   덤프(docs/19)로 확정된 실제 이름들이다. 개발사가 일본 인디라
   내부 명칭이 일본어 기반이었고, whistle/taunt 로는 잡히지 않았다.

     Provocation  도발 — 휘파람 본체
     MOUIIYO      もういいよ = "다 됐어" (준비 완료 신호)
     EEYAN        ええやん = "좋다" (칭찬 포인트) */
static const char* kAlways[] = {
    "Provocation", "MOUIIYO", "MOUEEYO", "Mouiiyo", "EEYAN",
    "Emote", "Voice", "Sound", "Audio",
    /* 입력 이벤트 전부. 어떤 키가 어떤 함수를 부르는지 보려고 넣었다.
       출시 빌드에 Ctrl+Alt+Shift+U/Y/T/R 같은 개발자 단축키가 남아 있어
       이게 실제로 무엇을 호출하는지 확인하는 용도이기도 하다. */
    "InpActEvt",
};

// ─────────────────────────────────────────────────────────────
// 상태
// ─────────────────────────────────────────────────────────────

using ProcessEventFn = void(*)(void*, class UFunction*, void*);

/* ProcessEvent 구현은 하나가 아니다.
   언리얼은 UObject::ProcessEvent 를 AActor 가 오버라이드한다.
   따라서 vtable 슬롯에 들어있는 원본 포인터가 클래스 계열마다 다르다.

     UObject 계열 (위젯 등)   →  UObject::ProcessEvent
     AActor  계열 (캐릭터 등) →  AActor::ProcessEvent

   원본을 하나만 기억하면 액터 계열이 통째로 후킹에서 빠진다.
   실제로 그것 때문에 캐릭터 함수가 하나도 안 잡혔다.
   그래서 vtable 마다 그 vtable 의 원본을 따로 보관한다. */
static std::unordered_map<void**, ProcessEventFn> gOriginalByVTable;
static std::shared_mutex                          gOriginalLock;

static std::vector<void**>   gPatchedVTables;      // 되돌리기용
static std::mutex            gMutex;

static volatile bool gLogging  = true;
static volatile bool gFiltered = true;
static volatile bool gRunning  = true;

struct CallInfo
{
    std::string FullName;
    uint32_t    Flags = 0;
    uint64_t    Count = 0;
    std::string CallerClass;
};

static std::unordered_map<std::string, CallInfo> gSeen;
static std::ofstream gLogFile;

// ─────────────────────────────────────────────────────────────
// 헬퍼
// ─────────────────────────────────────────────────────────────

static bool Contains(const std::string& Haystack, const char* Needle)
{
    return Haystack.find(Needle) != std::string::npos;
}

/* EFunctionFlags 를 사람이 읽을 수 있는 문자열로. 체크리스트 1번의 답이 여기서 나온다. */
static std::string FlagsToString(uint32_t Flags)
{
    struct { uint32_t Bit; const char* Name; } Table[] = {
        { 0x00000001, "Final"          }, { 0x00000004, "BlueprintAuthorityOnly" },
        { 0x00000008, "BlueprintCosmetic" },
        { 0x00000040, "Net"            }, { 0x00000080, "NetReliable"   },
        { 0x00000100, "NetRequest"     }, { 0x00000200, "Exec"          },
        { 0x00000400, "Native"         }, { 0x00000800, "Event"         },
        { 0x00001000, "NetResponse"    }, { 0x00002000, "Static"        },
        { 0x00004000, "NetMulticast"   }, { 0x00008000, "UbergraphFunction" },
        { 0x00010000, "MulticastDelegate" },
        { 0x00020000, "Public"         }, { 0x00040000, "Private"       },
        { 0x00080000, "Protected"      }, { 0x00100000, "Delegate"      },
        { 0x00200000, "NetServer"      }, { 0x00400000, "HasOutParms"   },
        { 0x00800000, "HasDefaults"    }, { 0x01000000, "NetClient"     },
        { 0x04000000, "BlueprintCallable" }, { 0x08000000, "BlueprintEvent" },
    };

    std::string Out;
    for (auto& E : Table)
    {
        if (Flags & E.Bit)
        {
            if (!Out.empty()) Out += ", ";
            Out += E.Name;
        }
    }
    return Out.empty() ? "(없음)" : Out;
}

/* 네트워크 성격 한 줄 요약. ★ 가 붙으면 클라이언트에서 위조를 시도할 수 있다는 뜻. */
static const char* NetKind(uint32_t Flags)
{
    if (Flags & 0x00200000) return "NetServer  ★ 클라->서버, 파라미터 위조 대상";
    if (Flags & 0x00004000) return "NetMulticast  서버->전원, 호스트만 조작 가능";
    if (Flags & 0x01000000) return "NetClient  서버->특정 클라";
    if (Flags & 0x00000040) return "Net";
    return "-";
}

static void Log(const std::string& Line)
{
    fputs(Line.c_str(), stdout);
    fputc('\n', stdout);
    if (gLogFile.is_open())
    {
        gLogFile << Line << '\n';
        gLogFile.flush();
    }
}

// ─────────────────────────────────────────────────────────────
// 후킹된 ProcessEvent
// ─────────────────────────────────────────────────────────────

/* 추적 대상. 이 이름이 들어간 함수는 "처음 본 것"이 아니어도 매번 기록한다.
   호출 순서와 시각을 알아야 체인을 재구성할 수 있기 때문이다.

   [NEW] 방식만으로는 한계가 명확했다. 1번키를 눌렀을 때
   Provocation(Client) 는 찍혔는데 Provocation(Server) 는 이미 본 함수라
   출력되지 않아, 체인이 Local→Server→Client 로 갔는지 알 수 없었다. */
static const char* kTrace[] = {
    "Provocation", "MOUIIYO", "MOUEEYO", "EEYAN",
    "InpActEvt_IA_Provocation", "InpActEvt_IA_MOUIIYO",
    "Ctrl+Alt+Shift",
};

static uint64_t gStartTick = 0;

/* 아래 설명 함수들이 참조하는 것들. 정의는 뒤쪽 절에 있다. */
static APawn* GetLocalPawn();
static std::vector<UObject*> FindLiveCharacters();
static std::string PawnPlayerName(UObject* Obj);

/* 추적 로그에서 "누구의 폰인가"를 밝힌다.
   주소만 찍으면 안 되는 이유가 실제로 드러났다 — 폰이 파괴되고 그 메모리를
   다른 플레이어의 폰이 재사용하면 같은 주소로 보인다. 2인 테스트에서
   내 도발인지 상대 도발인지 구분이 안 되면 결과를 읽을 수 없다.

   Pawn → PlayerState → PlayerNamePrivate 로 이름을 얻고,
   AActor::Role 로 내 소유(AutonomousProxy)인지 남의 것(SimulatedProxy)인지 본다. */
static std::string DescribeOwner(void* Object)
{
    if (!Object) return "";

    UObject* Obj = static_cast<UObject*>(Object);
    if (!Obj->IsA(EClassCastFlags::Pawn)) return "";

    APawn* Pawn = static_cast<APawn*>(Obj);

    std::string Name = "?";
    if (Pawn->PlayerState)
    {
        Name = Pawn->PlayerState->PlayerNamePrivate.ToString();
        if (Name.empty()) Name = "(이름없음)";
        Name += " #" + std::to_string(Pawn->PlayerState->PlayerId);
    }
    else
    {
        Name = "(PlayerState 없음)";
    }

    /* ENetRole: 0=None 1=SimulatedProxy 2=AutonomousProxy 3=Authority */
    const char* RoleStr = "?";
    switch (static_cast<int>(Pawn->Role))
    {
        case 1: RoleStr = "SimulatedProxy(남의 폰)";   break;
        case 2: RoleStr = "AutonomousProxy(내 폰)";    break;
        case 3: RoleStr = "Authority(서버권한)";        break;
        case 0: RoleStr = "None";                      break;
    }

    return "  [" + Name + " / " + RoleStr + "]";
}

/* 도발이 일어난 액터의 월드 좌표와, 나로부터의 거리를 구한다.
   이게 이 파트의 핵심 논지다 — 서버는 도발 RPC 에 좌표를 싣지 않지만,
   액터 위치는 이미 복제돼 있으므로 클라이언트가 정확한 좌표를 안다.
   "희미한 소리 단서"로 설계된 것이 클라이언트에서는 정밀한 수치가 된다. */
static std::string DescribeLocation(void* Object)
{
    if (!Object) return "";

    UObject* Obj = static_cast<UObject*>(Object);
    if (!Obj->IsA(EClassCastFlags::Actor)) return "";

    AActor* Actor = static_cast<AActor*>(Obj);
    const FVector Loc = Actor->K2_GetActorLocation();

    auto DistTo = [&Loc](AActor* Other) -> double {
        const FVector O = Other->K2_GetActorLocation();
        const double dx = Loc.X - O.X, dy = Loc.Y - O.Y, dz = Loc.Z - O.Z;
        return sqrt(dx * dx + dy * dy + dz * dz) / 100.0;   // cm → m
    };

    std::string Out = "  pos=(" + std::to_string((int)Loc.X) + ", "
                    + std::to_string((int)Loc.Y) + ", "
                    + std::to_string((int)Loc.Z) + ")";

    char Buf[128];
    APawn* Me = GetLocalPawn();
    if (Me && static_cast<UObject*>(Me) != Obj)
    {
        snprintf(Buf, sizeof(Buf), "  내거리=%.1fm", DistTo(Me));
        Out += Buf;
    }

    /* ★ 술래로부터의 거리.
       청취 가능 범위를 재려면 "관전자로부터의 거리"가 아니라
       "술래로부터의 거리"가 필요하다. 관전자는 대상을 따라다니므로
       내 거리는 0 에 붙어 있어 측정에 쓸 수 없다. */
    for (UObject* C : FindLiveCharacters())
    {
        if (C == Obj) continue;
        if (!Contains(C->Class->GetName(), "Hunter")) continue;
        if (PawnPlayerName(C).empty()) continue;      /* 찌꺼기 제외 */
        snprintf(Buf, sizeof(Buf), "  ★술래거리=%.1fm", DistTo(static_cast<AActor*>(C)));
        Out += Buf;
        break;
    }

    return Out;
}

/* 로깅 본체. std::string / lock_guard 처럼 소멸자가 있는 객체를 쓰므로
   여기에는 __try 를 넣을 수 없다 (C2712). SEH 는 호출하는 쪽에 둔다. */
static void LogCall(void* Object, UFunction* Function)
{
    const std::string Full = Function->GetFullName();

    /* ── 추적 모드 ── 매번 시각과 함께 남긴다 */
    for (auto* K : kTrace)
    {
        if (!Contains(Full, K)) continue;

        const uint64_t Ms = GetTickCount64() - gStartTick;
        std::string Caller = Object
            ? static_cast<UObject*>(Object)->Class->GetName() : "?";

        /* 도발일 때만 좌표까지 붙인다. 다른 함수는 소음이 된다. */
        const std::string Where = Contains(Full, "Provocation")
                                ? DescribeLocation(Object) : std::string();

        char Buf[1024];
        snprintf(Buf, sizeof(Buf), "[TRACE %4llu.%03llus] %-46s obj=%p %s%s%s",
                 (unsigned long long)(Ms / 1000), (unsigned long long)(Ms % 1000),
                 Full.substr(Full.rfind('.') == std::string::npos
                             ? 0 : Full.rfind('.') + 1).c_str(),
                 Object, Caller.c_str(), DescribeOwner(Object).c_str(),
                 Where.c_str());
        {
            std::lock_guard<std::mutex> Lock(gMutex);
            Log(Buf);
        }
        break;
    }

    bool bAlways = false;
    for (auto* K : kAlways)
        if (Contains(Full, K)) { bAlways = true; break; }

    if (!bAlways && gFiltered)
        for (auto* K : kNoise)
            if (Contains(Full, K)) return;

    std::lock_guard<std::mutex> Lock(gMutex);

    auto It = gSeen.find(Full);
    if (It != gSeen.end())
    {
        It->second.Count++;
        return;
    }

    CallInfo Info;
    Info.FullName = Full;
    Info.Flags    = static_cast<uint32_t>(Function->FunctionFlags);
    Info.Count    = 1;
    Info.CallerClass = Object ? static_cast<UObject*>(Object)->Class->GetName() : "?";
    gSeen[Full] = Info;

    /* 처음 보는 함수만 즉시 출력한다. 같은 함수가 반복돼도 로그가 안 밀린다. */
    char Buf[1024];
    snprintf(Buf, sizeof(Buf),
             "\n[NEW] %s\n"
             "      caller : %s\n"
             "      flags  : %s\n"
             "      net    : %s",
             Full.c_str(), Info.CallerClass.c_str(),
             FlagsToString(Info.Flags).c_str(),
             NetKind(Info.Flags));
    Log(Buf);
}

/* 이 객체의 vtable 에 대응하는 원본 ProcessEvent 를 찾는다.
   못 찾으면 nullptr — 그 경우 호출하지 않는 것 외에 안전한 선택지가 없다. */
static ProcessEventFn FindOriginal(void* Object)
{
    void** VTable = *reinterpret_cast<void***>(Object);
    std::shared_lock<std::shared_mutex> Lock(gOriginalLock);
    auto It = gOriginalByVTable.find(VTable);
    return It == gOriginalByVTable.end() ? nullptr : It->second;
}

/* 재진입 가드.
   로깅 중에 SDK 함수(K2_GetActorLocation 등)를 부르면 그것도 ProcessEvent 를
   거치므로 후크에 다시 들어온다. 이 플래그가 서 있는 동안은 로깅을 건너뛰어
   무한 재귀와 로그 오염을 막는다. */
static thread_local int gInHook = 0;

static void HookedProcessEvent(void* Object, UFunction* Function, void* Parms)
{
    /* 원본을 먼저 확보한다. 이걸 못 부르면 게임이 망가지므로 최우선이다. */
    ProcessEventFn Original = Object ? FindOriginal(Object) : nullptr;

    if (gInHook > 0)
    {
        if (Original) Original(Object, Function, Parms);
        return;
    }

    /* 여기서 예외가 나도 게임이 죽으면 안 된다.
       이 함수 자체에는 소멸자를 가진 지역 객체가 없어야 __try 를 쓸 수 있다. */
    if (gLogging && Function)
    {
        gInHook++;
        __try
        {
            LogCall(Object, Function);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            /* 로깅 실패는 무시한다 */
        }
        gInHook--;
    }

    if (Original)
        Original(Object, Function, Parms);
}

// ─────────────────────────────────────────────────────────────
// vtable 후킹 / 복원
// ─────────────────────────────────────────────────────────────

static bool PatchSlot(void** VTable, int Index, void* NewFn, void** OutOld)
{
    DWORD Old = 0;
    if (!VirtualProtect(&VTable[Index], sizeof(void*), PAGE_EXECUTE_READWRITE, &Old))
        return false;

    if (OutOld) *OutOld = VTable[Index];
    VTable[Index] = NewFn;

    VirtualProtect(&VTable[Index], sizeof(void*), Old, &Old);
    return true;
}

/* 이미 패치한 vtable. 재스캔 때 중복 작업을 피한다. */
static std::unordered_set<void**> gKnownVTables;

/* GObjects 는 뒤로만 늘어나므로, 지난번에 본 지점부터 이어서 스캔하면 된다.
   매치가 시작되며 새 클래스가 로드돼도 이 재스캔이 잡아낸다. */
static int gLastScannedIndex = 0;

/* 진짜 UObject::ProcessEvent 의 주소.
   앞 버전은 "처음 만난 클래스의 vtable 슬롯 값"을 원본으로 삼았는데,
   그 첫 클래스가 어떤 이유로든 다른 값을 갖고 있으면
   이후 모든 클래스가 "재정의됨"으로 오판돼 통째로 건너뛰어진다.
   Dumper-7 이 뽑아준 Offsets::ProcessEvent 를 권위 있는 기준으로 쓴다. */
static void* ResolveRealProcessEvent()
{
    HMODULE Base = GetModuleHandleW(nullptr);
    return reinterpret_cast<uint8_t*>(Base) + Offsets::ProcessEvent;
}

/* 게임 모듈의 주소 범위. vtable 슬롯에 들어있는 값이 정말 코드 포인터인지
   확인하는 데 쓴다. 쓰레기 값을 원본으로 저장하면 호출 순간 게임이 죽는다. */
static uint8_t* gModuleBase = nullptr;
static size_t   gModuleSize = 0;

static void ResolveModuleRange()
{
    gModuleBase = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto* Dos = reinterpret_cast<IMAGE_DOS_HEADER*>(gModuleBase);
    auto* Nt  = reinterpret_cast<IMAGE_NT_HEADERS64*>(gModuleBase + Dos->e_lfanew);
    gModuleSize = Nt->OptionalHeader.SizeOfImage;
}

static bool InModule(void* P)
{
    auto* B = reinterpret_cast<uint8_t*>(P);
    return B >= gModuleBase && B < gModuleBase + gModuleSize;
}

static int InstallHooks()
{
    const int Idx = Offsets::ProcessEventIdx;
    std::unordered_set<void**>& Unique = gKnownVTables;
    int Patched = 0, Rejected = 0, NoCDO = 0, Classes = 0;

    if (!gModuleBase) ResolveModuleRange();
    void* RealPE = ResolveRealProcessEvent();

    /* 발견한 서로 다른 원본 구현들. 보통 UObject::ProcessEvent 와
       AActor::ProcessEvent 두 개가 나온다. 진단용으로 집계한다. */
    static std::unordered_map<void*, int> sOriginals;

    for (int i = gLastScannedIndex; i < UObject::GObjects->Num(); i++)
    {
        UObject* Obj = UObject::GObjects->GetByIndex(i);
        if (!Obj || !Obj->IsA(EClassCastFlags::Class))
            continue;

        Classes++;
        UClass* Cls = static_cast<UClass*>(Obj);
        UObject* CDO = Cls->ClassDefaultObject;
        if (!CDO) { NoCDO++; continue; }

        void** VTable = *reinterpret_cast<void***>(CDO);
        if (!VTable || Unique.count(VTable))
            continue;

        void* Current = VTable[Idx];

        /* 이미 우리가 패치한 vtable 이면 건너뛴다. */
        if (Current == reinterpret_cast<void*>(&HookedProcessEvent))
            continue;

        /* 게임 모듈 안의 코드 포인터가 아니면 손대지 않는다.
           쓰레기 값을 원본으로 저장하면 호출하는 순간 게임이 죽는다. */
        if (!InModule(Current))
        {
            Rejected++;
            continue;
        }

        {
            std::unique_lock<std::shared_mutex> Lock(gOriginalLock);
            gOriginalByVTable[VTable] = reinterpret_cast<ProcessEventFn>(Current);
        }
        sOriginals[Current]++;

        if (PatchSlot(VTable, Idx, reinterpret_cast<void*>(&HookedProcessEvent), nullptr))
        {
            Unique.insert(VTable);
            gPatchedVTables.push_back(VTable);
            Patched++;
        }
    }

    gLastScannedIndex = UObject::GObjects->Num();

    /* 콘솔이 아니라 로그 파일에도 남긴다. 원격에서 상태를 확인해야 하기 때문이다. */
    if (Patched || Rejected)
    {
        char Buf[1024];
        snprintf(Buf, sizeof(Buf),
                 "\n[HOOK] 신규패치 %d / 거부 %d / CDO없음 %d / 검사한클래스 %d\n"
                 "       누적 vtable %zu · Idx=0x%X · Dumper-7의 ProcessEvent=%p",
                 Patched, Rejected, NoCDO, Classes,
                 gKnownVTables.size(), Idx, RealPE);
        Log(Buf);

        std::string Impls;
        for (auto& [Ptr, Count] : sOriginals)
        {
            char L[160];
            snprintf(L, sizeof(L), "\n      %p  (vtable %d개)%s",
                     Ptr, Count, Ptr == RealPE ? "  ← Dumper-7이 지목한 것" : "");
            Impls += L;
        }
        Log("       발견된 원본 ProcessEvent 구현:" + Impls);
    }
    return Patched;
}

/* 특정 클래스가 실제로 후킹돼 있는지 이름으로 조회한다.
   "캐릭터 함수가 안 찍힌다"가 후킹 실패인지 그냥 호출이 없는 건지
   추측으로 갈리지 않게 하려고 만든 진단이다. */
static void DiagnoseClasses()
{
    static const char* kCheck[] = {
        "BP_FirstPersonCharacter_Main_C",
        "BP_FirstPersonCharacter_cLeon_Character_C",
        "BP_FirstPersonCharacter_cLeon_Character_Hunter_C",
        "BP_FirstPersonCharacter_cLeon_Character_Survivor_C",
        "BP_PlayerController_cLeon_C",
        "BP_GameState_cLeon_C",
        "BP_SpectatePawn_cLeon_C",
        "WBP_cLeonMain_C",
    };

    const int Idx = Offsets::ProcessEventIdx;
    void* RealPE = ResolveRealProcessEvent();

    Log("\n======================================================================");
    Log("클래스별 후킹 상태");
    Log("======================================================================");

    for (auto* Want : kCheck)
    {
        bool bFound = false;

        for (int i = 0; i < UObject::GObjects->Num(); i++)
        {
            UObject* Obj = UObject::GObjects->GetByIndex(i);
            if (!Obj || !Obj->IsA(EClassCastFlags::Class))
                continue;
            if (Obj->GetName() != Want)
                continue;

            bFound = true;
            UClass* Cls = static_cast<UClass*>(Obj);
            UObject* CDO = Cls->ClassDefaultObject;

            char Buf[512];
            if (!CDO)
            {
                snprintf(Buf, sizeof(Buf), "  %-52s CDO 없음", Want);
            }
            else
            {
                void** VT = *reinterpret_cast<void***>(CDO);
                void* Slot = VT ? VT[Idx] : nullptr;
                const char* State =
                    Slot == reinterpret_cast<void*>(&HookedProcessEvent) ? "후킹됨 OK"
                  : Slot == RealPE                        ? "미후킹 (UObject::ProcessEvent)"
                  : InModule(Slot)                        ? "미후킹 (다른 구현 — AActor 계열?)"
                  :                                         "슬롯이 코드 포인터가 아님";
                snprintf(Buf, sizeof(Buf), "  %-52s %s\n        vtable=%p slot=%p",
                         Want, State, (void*)VT, Slot);
            }
            Log(Buf);
            break;
        }

        if (!bFound)
        {
            char Buf[512];
            snprintf(Buf, sizeof(Buf), "  %-52s 클래스가 아직 로드되지 않음", Want);
            Log(Buf);
        }
    }
    Log("");
}

// ─────────────────────────────────────────────────────────────
// 직접 호출 — 입력 핸들러의 게이트를 건너뛴다
// ─────────────────────────────────────────────────────────────

/*
 * 관측 결과 1번키를 누르면 InpActEvt_IA_Provocation 은 발화하지만
 * Provocation(Local/Server/Client) 까지는 도달하지 않는다.
 * 입력 핸들러 안에서 게임 페이즈나 역할을 검사해 막는 것으로 보인다.
 *
 * 그 게이트를 건너뛰고 RPC 를 직접 호출하면, 그 검사가
 * 클라이언트에만 있는지 서버도 하는지 갈린다. 이게 핵심 실험이다.
 *
 * 호출은 SDK 함수를 링크하지 않고 이름으로 UFunction 을 찾아
 * ProcessEvent 로 넘긴다. 어떤 함수든 같은 방식으로 부를 수 있다.
 */

/* 로컬 플레이어가 조종 중인 폰을 얻는다.
   World → GameInstance → LocalPlayers[0] → PlayerController → Pawn */
static APawn* GetLocalPawn()
{
    UWorld* World = UWorld::GetWorld();
    if (!World || !World->OwningGameInstance) return nullptr;

    auto& Players = World->OwningGameInstance->LocalPlayers;
    if (Players.Num() <= 0) return nullptr;

    ULocalPlayer* LP = Players[0];
    if (!LP || !LP->PlayerController) return nullptr;

    return LP->PlayerController->Pawn;
}

/* 클래스 계층을 거슬러 올라가며 이름으로 UFunction 을 찾는다.
   블루프린트 함수는 부모 클래스에 정의돼 있을 수 있다. */
static UFunction* FindFunctionByName(UClass* Cls, const std::string& Name)
{
    for (UStruct* S = Cls; S; S = S->SuperStruct)
    {
        for (UField* F = S->Children; F; F = F->Next)
        {
            if (F->GetName() == Name && F->IsA(EClassCastFlags::Function))
                return static_cast<UFunction*>(F);
        }
    }
    return nullptr;
}

/* 이름으로 함수를 찾아 파라미터 없이 호출한다.
   Provocation 3종은 전부 void 이므로 Parms 가 필요 없다. */
static void CallOnLocalPawn(const char* FuncName)
{
    APawn* Pawn = GetLocalPawn();
    if (!Pawn)
    {
        Log("[CALL] 로컬 폰을 찾을 수 없습니다 (매치/로비에 들어가 있나요?)");
        return;
    }

    char Buf[512];
    snprintf(Buf, sizeof(Buf), "\n[CALL] 폰: %s (%p)",
             Pawn->Class->GetName().c_str(), (void*)Pawn);
    Log(Buf);

    UFunction* Fn = FindFunctionByName(Pawn->Class, FuncName);
    if (!Fn)
    {
        snprintf(Buf, sizeof(Buf), "[CALL] 함수를 찾을 수 없음: %s", FuncName);
        Log(Buf);
        return;
    }

    snprintf(Buf, sizeof(Buf), "[CALL] 호출: %s  flags=%s",
             FuncName, FlagsToString(static_cast<uint32_t>(Fn->FunctionFlags)).c_str());
    Log(Buf);

    /* UStruct::Size 는 이 함수의 로컬 프레임 크기다(파라미터 + 지역변수).
       0 으로 채운 버퍼를 넘긴다. 초기화 안 된 스택을 넘기면
       포인터 파라미터가 쓰레기 주소가 돼 게임이 죽는다. */
    std::vector<uint8_t> Frame(Fn->Size > 0 ? Fn->Size : 0, 0);

    snprintf(Buf, sizeof(Buf), "[CALL] 프레임 크기 %d바이트 (0으로 초기화)", Fn->Size);
    Log(Buf);

    Pawn->ProcessEvent(Fn, Frame.empty() ? nullptr : Frame.data());
    Log("[CALL] 반환됨 — 아래 로그에서 후속 호출을 확인하세요");
}

// ─────────────────────────────────────────────────────────────
// ProvocationRemote — 관전 폰의 임의 대상 지정 실험 (V-A)
// ─────────────────────────────────────────────────────────────

/*
 * BP_SpectatePawn_cLeon_C::ProvocationRemote(Target)  — (Net, NetServer)
 *
 * 클라이언트가 서버에게 "저 캐릭터가 도발하게 해라" 라고 지시하는 RPC 다.
 * 정상 용법은 지금 관전 중인 대상을 놀리는 것으로 보인다.
 *
 * 핵심 질문: 서버가 Target 을 검증하는가?
 *   검증한다면  → SpectateTarget 과 일치할 때만 동작
 *   안 한다면   → 임의 생존자를 지목해 위치를 노출시킬 수 있다 (취약점)
 *
 * 그래서 두 가지를 나눠 시험한다.
 *   F2 = 지금 보고 있는 대상   (정상 사용. 이게 되어야 실험이 성립)
 *   F3 = 보고 있지 않은 다른 캐릭터 (공격)
 */

/* BP_SpectatePawn_cLeon_C 의 멤버 오프셋 (덤프 기준, buildid 24929792) */
static constexpr int kSpectateTargetOffset = 0x370;   // ABP_FirstPersonCharacter_Main_C*
static constexpr int kMyMainBodyOffset     = 0x3A8;   // ABP_FirstPersonCharacter_Main_C*

static bool IsSpectatePawn(UObject* Pawn)
{
    return Pawn && Contains(Pawn->Class->GetName(), "SpectatePawn");
}

/* 캐릭터가 실제 플레이어의 폰인지 판별한다.
   3인 테스트에서 월드에 cLeon 캐릭터가 9개나 잡혔는데 대부분이
   이전 라운드의 잔재였다. PlayerState 가 붙어 있는 것만 실제 플레이어다.
   찌꺼기를 지목하면 서버가 조용히 무시해서 "방어됨"으로 오독하게 된다. */
static std::string PawnPlayerName(UObject* Obj)
{
    if (!Obj || !Obj->IsA(EClassCastFlags::Pawn)) return "";
    APawn* P = static_cast<APawn*>(Obj);
    if (!P->PlayerState) return "";
    std::string N = P->PlayerState->PlayerNamePrivate.ToString();
    if (N.empty()) N = "(이름없음)";
    return N + " #" + std::to_string(P->PlayerState->PlayerId);
}

/* 월드에 살아있는 cLeon 캐릭터들을 모은다. CDO(Default__*)는 제외한다. */
static std::vector<UObject*> FindLiveCharacters()
{
    std::vector<UObject*> Out;
    for (int i = 0; i < UObject::GObjects->Num(); i++)
    {
        UObject* Obj = UObject::GObjects->GetByIndex(i);
        if (!Obj || !Obj->Class) continue;

        const std::string ClsName = Obj->Class->GetName();
        if (!Contains(ClsName, "cLeon_Character")) continue;

        /* 클래스 자체와 CDO 는 건너뛴다 */
        if (Obj->IsA(EClassCastFlags::Class)) continue;
        if (Obj->GetName().rfind("Default__", 0) == 0) continue;

        Out.push_back(Obj);
    }
    return Out;
}

/* 현재 상태와 지목 가능한 대상들을 출력한다. 무엇을 지목할지 고르기 전에 본다. */
static void ShowSpectateState()
{
    Log("\n======================================================================");
    Log("관전 상태 / 지목 가능한 대상");
    Log("======================================================================");

    APawn* Pawn = GetLocalPawn();
    char Buf[512];

    if (!Pawn)
    {
        Log("  로컬 폰 없음");
        return;
    }

    snprintf(Buf, sizeof(Buf), "  내 폰 : %s (%p)%s",
             Pawn->Class->GetName().c_str(), (void*)Pawn,
             IsSpectatePawn(Pawn) ? "   [관전 폰 — ProvocationRemote 가능]" : "");
    Log(Buf);

    if (IsSpectatePawn(Pawn))
    {
        auto* Base = reinterpret_cast<uint8_t*>(Pawn);
        auto* Target = *reinterpret_cast<UObject**>(Base + kSpectateTargetOffset);
        auto* MyBody = *reinterpret_cast<UObject**>(Base + kMyMainBodyOffset);

        snprintf(Buf, sizeof(Buf), "  관전 대상 : %s (%p)",
                 Target ? Target->Class->GetName().c_str() : "(없음)", (void*)Target);
        Log(Buf);
        snprintf(Buf, sizeof(Buf), "  내 시체   : %s (%p)",
                 MyBody ? MyBody->Class->GetName().c_str() : "(없음)", (void*)MyBody);
        Log(Buf);
    }

    auto Chars = FindLiveCharacters();
    snprintf(Buf, sizeof(Buf), "\n  월드의 cLeon 캐릭터 %zu개:", Chars.size());
    Log(Buf);
    for (size_t i = 0; i < Chars.size() && i < 20; i++)
    {
        const std::string Who = PawnPlayerName(Chars[i]);
        const bool bReal = !Who.empty();
        const std::string Cls = Chars[i]->Class->GetName();
        const char* Role = Contains(Cls, "Hunter")   ? "술래"
                         : Contains(Cls, "Survivor") ? "생존자"
                         :                             "?";

        snprintf(Buf, sizeof(Buf), "   [%zu] %-8s %-22s %p  %s%s",
                 i, Role, bReal ? Who.c_str() : "(PlayerState 없음)",
                 (void*)Chars[i],
                 Chars[i] == static_cast<UObject*>(Pawn) ? "<- 내 폰 " : "",
                 bReal ? "" : "   ** 실제 플레이어 아님 — 지목해도 무시될 것 **");
        Log(Buf);
    }
    Log("\n  F3 를 누를 때마다 다음 대상으로 순환합니다. 이름이 있는 대상만 유효합니다.");
    Log("");
}

/* ProvocationRemote 를 지정한 대상으로 호출한다.
   bOther=false 면 관전 중인 대상, true 면 그 외의 캐릭터를 고른다. */
static void CallProvocationRemote(bool bOther)
{
    APawn* Pawn = GetLocalPawn();
    char Buf[640];

    if (!IsSpectatePawn(Pawn))
    {
        Log("\n[REMOTE] 관전 폰이 아닙니다. 죽어서 관전 상태여야 호출할 수 있습니다.");
        if (Pawn)
        {
            snprintf(Buf, sizeof(Buf), "         현재 폰: %s", Pawn->Class->GetName().c_str());
            Log(Buf);
        }
        return;
    }

    UFunction* Fn = FindFunctionByName(Pawn->Class, "ProvocationRemote");
    if (!Fn)
    {
        Log("[REMOTE] ProvocationRemote 함수를 찾을 수 없습니다.");
        return;
    }

    auto* Base = reinterpret_cast<uint8_t*>(Pawn);
    UObject* Spectated = *reinterpret_cast<UObject**>(Base + kSpectateTargetOffset);

    UObject* Target = nullptr;
    if (!bOther)
    {
        Target = Spectated;
    }
    else
    {
        /* 관전 중이 아닌 다른 캐릭터를 순환하며 고른다. 이게 공격 케이스다.
           "첫 번째"만 고르면 이전 라운드의 찌꺼기 객체를 잡아
           서버가 조용히 무시하고, 그걸 "방어됨"으로 오독하게 된다.
           그래서 PlayerState 가 붙은 실제 플레이어만 후보로 삼고 순환한다. */
        std::vector<UObject*> Cands;
        for (UObject* C : FindLiveCharacters())
        {
            if (C == Spectated) continue;
            if (C == static_cast<UObject*>(Pawn)) continue;
            if (PawnPlayerName(C).empty()) continue;   /* 찌꺼기 제외 */
            Cands.push_back(C);
        }

        if (Cands.empty())
        {
            Log("\n[REMOTE] 지목할 실제 플레이어가 없습니다 "
                "(F1 으로 목록을 확인하세요)");
            return;
        }

        static int sIdx = -1;
        sIdx = (sIdx + 1) % static_cast<int>(Cands.size());
        Target = Cands[sIdx];

        snprintf(Buf, sizeof(Buf), "[REMOTE] 후보 %d/%zu 순환 중",
                 sIdx + 1, Cands.size());
        Log(Buf);
    }

    if (!Target)
    {
        Log(bOther ? "[REMOTE] 관전 대상 외의 캐릭터를 찾지 못했습니다."
                   : "[REMOTE] 관전 대상이 없습니다.");
        return;
    }

    snprintf(Buf, sizeof(Buf),
             "\n[REMOTE] %s\n"
             "         내 폰   : %s (%p)\n"
             "         지목대상: %s (%p)\n"
             "                   플레이어: %s\n"
             "         관전대상: %s (%p)\n"
             "                   플레이어: %s\n"
             "         플래그  : %s",
             bOther ? "★ 공격 케이스 — 관전 중이 아닌 대상을 지목" : "정상 케이스 — 관전 중인 대상",
             Pawn->Class->GetName().c_str(), (void*)Pawn,
             Target->Class->GetName().c_str(), (void*)Target,
             PawnPlayerName(Target).empty() ? "(없음 — 찌꺼기일 수 있음)"
                                            : PawnPlayerName(Target).c_str(),
             Spectated ? Spectated->Class->GetName().c_str() : "(없음)", (void*)Spectated,
             Spectated ? (PawnPlayerName(Spectated).empty() ? "(없음)"
                                                            : PawnPlayerName(Spectated).c_str())
                       : "(없음)",
             FlagsToString(static_cast<uint32_t>(Fn->FunctionFlags)).c_str());
    Log(Buf);

    /* 프레임의 맨 앞 8바이트가 Target 포인터다 (파라미터가 하나뿐). */
    std::vector<uint8_t> Frame(Fn->Size > 8 ? Fn->Size : 8, 0);
    *reinterpret_cast<UObject**>(Frame.data()) = Target;

    Pawn->ProcessEvent(Fn, Frame.data());
    Log("[REMOTE] 반환됨 — 아래 TRACE 에서 Provocation(Client) 가 오는지 확인하세요");
}

// ─────────────────────────────────────────────────────────────
// 오디오 조사 — 휘파람 소리 변경이 어디까지 가능한가
// ─────────────────────────────────────────────────────────────

/*
 * 캐릭터에는 UAudioComponent* Audio 가 +0x0B40 에 있다.
 * UAudioComponent::Sound (+0x03F8) 가 재생할 USoundBase 를 가리킨다.
 *
 * 여기서 나오는 결론이 이 파트의 핵심 논지 절반이다.
 *   - 이 값은 각 클라이언트가 자기 메모리에 들고 있다
 *   - 따라서 바꿔도 "내가 듣는 소리"만 바뀐다
 *   - 남에게 다른 소리를 들려주려면 남의 클라이언트를 고쳐야 한다
 *
 * 즉 소리 변경은 가능하지만 스푸핑은 아니다. 그 경계를 실측으로 보인다.
 */

static constexpr int kCharacterAudioOffset = 0x0B40;   // UAudioComponent*

static UAudioComponent* GetCharacterAudio(UObject* Character)
{
    if (!Character) return nullptr;
    if (!Contains(Character->Class->GetName(), "cLeon_Character")) return nullptr;
    return *reinterpret_cast<UAudioComponent**>(
        reinterpret_cast<uint8_t*>(Character) + kCharacterAudioOffset);
}

/* 월드에 있는 모든 cLeon 캐릭터의 오디오 상태를 출력한다. */
static void DumpAudioState()
{
    Log("\n======================================================================");
    Log("캐릭터 오디오 컴포넌트 상태");
    Log("======================================================================");

    /* GameState 의 강제 도발 주기를 같이 읽는다.
       잔여 시간은 클라이언트에 없지만 주기는 복제된다(Net 플래그).
       exec 후크로 측정한 실제 간격과 대조하는 데 쓴다. */
    if (UWorld* W = UWorld::GetWorld())
    {
        if (AGameStateBase* GS = W->GameState)
        {
            char B[256];
            if (Contains(GS->Class->GetName(), "cLeon"))
            {
                const int32 Interval = *reinterpret_cast<int32*>(
                    reinterpret_cast<uint8_t*>(GS) + 0x03C0);   // ForceProvocationInverval
                snprintf(B, sizeof(B),
                         "  ForceProvocationInverval = %d  (GameState +0x3C0)", Interval);
            }
            else
            {
                snprintf(B, sizeof(B), "  GameState: %s (cLeon 아님)",
                         GS->Class->GetName().c_str());
            }
            Log(B);
            Log("");
        }
    }

    auto Chars = FindLiveCharacters();
    if (Chars.empty())
    {
        Log("  월드에 cLeon 캐릭터가 없습니다 (매치에 들어가 있나요?)");
        return;
    }

    APawn* Me = GetLocalPawn();
    char Buf[768];

    for (UObject* C : Chars)
    {
        UAudioComponent* Ac = GetCharacterAudio(C);
        snprintf(Buf, sizeof(Buf), "\n  %s%s\n    Audio 컴포넌트: %p",
                 C->Class->GetName().c_str(),
                 C == Me ? "   <- 내 캐릭터" : "",
                 (void*)Ac);
        Log(Buf);

        if (!Ac) continue;

        USoundBase* Snd = Ac->Sound;
        snprintf(Buf, sizeof(Buf),
                 "    Sound          : %s (%p)\n"
                 "    VolumeMultiplier: %.2f\n"
                 "    Pitch Mod      : %.2f ~ %.2f\n"
                 "    Volume Mod     : %.2f ~ %.2f",
                 Snd ? Snd->GetFullName().c_str() : "(없음)", (void*)Snd,
                 Ac->VolumeMultiplier,
                 Ac->PitchModulationMin, Ac->PitchModulationMax,
                 Ac->VolumeModulationMin, Ac->VolumeModulationMax);
        Log(Buf);
    }
    Log("");
}

/* 월드에 로드된 사운드 자산 중 이름이 맞는 것을 찾는다.
   휘파람 자산은 정적 추출에서 확인됐다:
     Provocation.uasset
     freesound_community-wolf-whistle-6777.uasset */
static void ListSounds(const char* Filter)
{
    Log("\n======================================================================");
    char Head[256];
    snprintf(Head, sizeof(Head), "로드된 사운드 자산 검색: \"%s\"", Filter);
    Log(Head);
    Log("======================================================================");

    int Count = 0;
    for (int i = 0; i < UObject::GObjects->Num(); i++)
    {
        UObject* Obj = UObject::GObjects->GetByIndex(i);
        if (!Obj || !Obj->Class) continue;
        if (Obj->IsA(EClassCastFlags::Class)) continue;

        const std::string Cls = Obj->Class->GetName();
        if (!Contains(Cls, "Sound") && !Contains(Cls, "MetaSound")) continue;

        const std::string Name = Obj->GetName();
        if (Filter && *Filter && !Contains(Name, Filter)) continue;

        char Buf[512];
        snprintf(Buf, sizeof(Buf), "  %-34s %-50s %p", Cls.c_str(), Name.c_str(), (void*)Obj);
        Log(Buf);
        if (++Count >= 40) { Log("  ... (40개에서 끊음)"); break; }
    }
    if (Count == 0) Log("  일치하는 사운드 없음");
    Log("");
}

/* 내 캐릭터의 오디오 볼륨을 키운다.
   거리 감쇠는 그대로지만, 멀리서 나는 휘파람도 들리게 된다.
   이건 순수하게 로컬 효과다 — 남이 듣는 소리는 전혀 바뀌지 않는다. */
static bool gBoosted = false;
static void ToggleVolumeBoost()
{
    auto Chars = FindLiveCharacters();
    if (Chars.empty()) { Log("\n[AUDIO] 캐릭터가 없습니다."); return; }

    gBoosted = !gBoosted;
    const float Vol = gBoosted ? 10.0f : 1.0f;

    int N = 0;
    for (UObject* C : Chars)
    {
        if (UAudioComponent* Ac = GetCharacterAudio(C))
        {
            Ac->VolumeMultiplier = Vol;
            N++;
        }
    }

    char Buf[256];
    snprintf(Buf, sizeof(Buf),
             "\n[AUDIO] 볼륨 배수 %.1f 로 설정 — 캐릭터 %d개\n"
             "        (로컬 전용. 남이 듣는 소리는 바뀌지 않습니다)", Vol, N);
    Log(Buf);
}

/*
 * 휘파람 소리 교체.
 *
 * 캐릭터의 Audio 컴포넌트가 가리키는 USoundBase 를 다른 것으로 바꾼다.
 * 게임 자체가 SC_Provoaction 과 SC_Provoaction_HIKAKIN 두 종류를 갖고 있어
 * (HIKAKIN 콜라보 맵 테마), 소리가 하나로 고정된 구조가 아니다.
 *
 * ★ 이 조작의 범위를 정확히 이해할 것:
 *   이 포인터는 각 클라이언트가 자기 메모리에 들고 있다.
 *   따라서 바꿔도 "내가 듣는 소리"만 바뀐다. 남에게는 원래 소리가 그대로 들린다.
 *   소리 변경은 가능하지만 스푸핑은 아니다 — 그 경계를 실측으로 보이는 것이 목적이다.
 *
 * 블루프린트가 재생 직전에 SetSound 로 다시 지정한다면 이 값은 덮어써진다.
 * 그 경우 이 실험은 "실패"가 아니라 "재생 경로가 다르다"는 결과를 준다.
 */

static std::vector<UObject*> FindLoadedSoundCues()
{
    std::vector<UObject*> Out;
    for (int i = 0; i < UObject::GObjects->Num(); i++)
    {
        UObject* Obj = UObject::GObjects->GetByIndex(i);
        if (!Obj || !Obj->Class) continue;
        if (Obj->IsA(EClassCastFlags::Class)) continue;

        const std::string Cls = Obj->Class->GetName();
        if (Cls != "SoundCue" && Cls != "SoundWave" && Cls != "MetaSoundSource")
            continue;
        if (Obj->GetName().rfind("Default__", 0) == 0) continue;

        Out.push_back(Obj);
    }
    return Out;
}

static int         gSoundIndex    = -1;
static USoundBase* gOriginalSound = nullptr;

static void CycleWhistleSound()
{
    APawn* Me = GetLocalPawn();
    UAudioComponent* Ac = GetCharacterAudio(Me);
    if (!Ac)
    {
        Log("\n[SOUND] 내 캐릭터의 Audio 컴포넌트를 찾을 수 없습니다.");
        return;
    }

    if (!gOriginalSound) gOriginalSound = Ac->Sound;

    auto Cues = FindLoadedSoundCues();
    if (Cues.empty()) { Log("\n[SOUND] 로드된 사운드가 없습니다."); return; }

    char Buf[512];

    /* 한 바퀴 다 돌면 원본으로 되돌린다. */
    gSoundIndex++;
    if (gSoundIndex >= static_cast<int>(Cues.size()))
    {
        gSoundIndex = -1;
        Ac->Sound = gOriginalSound;
        snprintf(Buf, sizeof(Buf), "\n[SOUND] 원본으로 복원: %s",
                 gOriginalSound ? gOriginalSound->GetName().c_str() : "(없음)");
        Log(Buf);
        return;
    }

    USoundBase* New = static_cast<USoundBase*>(Cues[gSoundIndex]);
    Ac->Sound = New;

    snprintf(Buf, sizeof(Buf),
             "\n[SOUND] %d/%zu  →  %s  (%s)\n"
             "        F5 로 도발을 울려 확인하세요. 이 변경은 로컬 전용입니다.",
             gSoundIndex + 1, Cues.size(),
             New->GetName().c_str(), New->Class->GetName().c_str());
    Log(Buf);
}

/* 이름으로 사운드를 직접 지정한다. 후보를 알고 있을 때 쓴다. */
static void SetWhistleSoundByName(const char* Needle)
{
    APawn* Me = GetLocalPawn();
    UAudioComponent* Ac = GetCharacterAudio(Me);
    if (!Ac) { Log("\n[SOUND] Audio 컴포넌트 없음"); return; }

    if (!gOriginalSound) gOriginalSound = Ac->Sound;

    char Buf[512];
    for (UObject* S : FindLoadedSoundCues())
    {
        if (!Contains(S->GetName(), Needle)) continue;
        Ac->Sound = static_cast<USoundBase*>(S);
        snprintf(Buf, sizeof(Buf), "\n[SOUND] 설정: %s", S->GetName().c_str());
        Log(Buf);
        return;
    }
    snprintf(Buf, sizeof(Buf), "\n[SOUND] \"%s\" 로 로드된 사운드를 못 찾음 "
                               "(그 맵에서만 로드되는 자산일 수 있음)", Needle);
    Log(Buf);
}

// ─────────────────────────────────────────────────────────────
// 네이티브 exec 포인터 후킹 — 휘파람 소리 실제 교체
// ─────────────────────────────────────────────────────────────

/*
 * 왜 이게 필요한가
 *
 * Audio->Sound 를 직접 써봤더니 재생 직후 원본으로 되돌아가 있었다.
 * 블루프린트가 재생 직전에 SetSound 로 다시 지정하기 때문이다.
 * 그리고 캐릭터 클래스에는 도발 사운드를 담은 변수가 없다 —
 * SC_Provoaction 은 블루프린트 그래프에 상수로 박혀 있다.
 *
 * 그래서 변수를 바꾸는 방식으로는 안 되고, 호출 자체에 끼어들어야 한다.
 *
 * 네이티브 UFUNCTION 은 ProcessEvent 를 거치지 않는다.
 * UFunction 구조체의 ExecFunction(+0x00D8) 이 진입점이므로 그걸 교체한다.
 * 이번 프로젝트에서 쓰는 세 번째 후킹 기법이다.
 *
 *   1. vtable 후킹        — ProcessEvent (블루프린트 이벤트)
 *   2. 직접 ProcessEvent  — 우리가 함수를 부를 때
 *   3. ExecFunction 후킹  — 네이티브 UFUNCTION  ← 여기
 *
 * SetSound 대신 Play 를 후킹하는 이유: Play 의 Context 가 곧 AudioComponent 라
 * 스택에서 파라미터를 파싱할 필요 없이 Sound 를 덮어쓰면 된다.
 * 순서도 SetSound → Play 이므로 우리가 마지막에 이긴다.
 */

using NativeExecFn = void(*)(void* Context, void* Stack, void* Result);

static UFunction*  gPlayFunc         = nullptr;
static NativeExecFn gOriginalPlayExec = nullptr;
static USoundBase*  gSoundOverride    = nullptr;   // null 이면 개입하지 않음

/* 이 AudioComponent 를 들고 있는 캐릭터를 역추적한다.
   도발은 자주 일어나지 않으므로 선형 탐색으로 충분하다. */
static UObject* FindOwnerOfAudio(UAudioComponent* Ac)
{
    for (UObject* C : FindLiveCharacters())
        if (GetCharacterAudio(C) == Ac) return C;
    return nullptr;
}

static uint64_t gLastProvoTick = 0;

/* std::string 을 쓰므로 __try 안에 둘 수 없다 (C2712). 별도 함수로 뺀다.
   교체뿐 아니라 "기록"도 여기서 한다.

   ★ 이 경로가 중요한 이유:
   자기 캐릭터의 도발은 ProcessEvent 를 거치지 않아 vtable 후크에 안 잡힌다.
   그런데 소리 재생은 반드시 UAudioComponent::Play 를 거치므로
   exec 후크로는 관측된다. ProcessEvent 의 사각지대를 여기서 메운다. */
static void OnProvocationPlay(void* Context)
{
    auto* Ac = static_cast<UAudioComponent*>(Context);

    /* 도발 사운드를 재생하려는 컴포넌트만 건드린다.
       발소리·BGM 까지 바꾸면 게임이 엉망이 된다. */
    if (!Ac->Sound) return;
    const std::string SndName = Ac->Sound->GetName();
    const bool bIsProvocation = Contains(SndName, "Provoaction")
                             || (gSoundOverride && Ac->Sound == gSoundOverride);
    if (!bIsProvocation) return;

    const uint64_t Ms   = GetTickCount64() - gStartTick;
    const uint64_t Gap  = gLastProvoTick ? (GetTickCount64() - gLastProvoTick) : 0;
    gLastProvoTick = GetTickCount64();

    UObject* Owner = FindOwnerOfAudio(Ac);

    char Buf[768];
    snprintf(Buf, sizeof(Buf),
             "[PLAY  %4llu.%03llus] 도발 재생  sound=%-28s %s%s",
             (unsigned long long)(Ms / 1000), (unsigned long long)(Ms % 1000),
             SndName.c_str(),
             Owner ? DescribeOwner(Owner).c_str() : "  [소유자 미상]",
             Owner ? DescribeLocation(Owner).c_str() : "");
    Log(Buf);

    if (Gap > 0)
    {
        snprintf(Buf, sizeof(Buf), "                     직전 도발과의 간격: %.2f초",
                 Gap / 1000.0);
        Log(Buf);
    }

    if (gSoundOverride) Ac->Sound = gSoundOverride;
}

static void HookedPlayExec(void* Context, void* Stack, void* Result)
{
    /* 교체를 안 걸어놨어도 항상 기록한다.
       ProcessEvent 로는 볼 수 없는 자기 캐릭터의 도발을
       여기서만 관측할 수 있기 때문이다. */
    if (Context && gInHook == 0)
    {
        gInHook++;
        __try
        {
            OnProvocationPlay(Context);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        gInHook--;
    }

    if (gOriginalPlayExec)
        gOriginalPlayExec(Context, Stack, Result);
}

/* UFunction 의 ExecFunction 슬롯을 교체한다. */
static bool InstallPlayHook()
{
    if (gPlayFunc) return true;   // 이미 설치됨

    /* UAudioComponent 클래스를 찾아 그 안의 Play 함수를 얻는다. */
    UClass* AudioCls = nullptr;
    for (int i = 0; i < UObject::GObjects->Num(); i++)
    {
        UObject* Obj = UObject::GObjects->GetByIndex(i);
        if (Obj && Obj->IsA(EClassCastFlags::Class) && Obj->GetName() == "AudioComponent")
        {
            AudioCls = static_cast<UClass*>(Obj);
            break;
        }
    }
    if (!AudioCls) { Log("\n[EXEC] UAudioComponent 클래스를 못 찾음"); return false; }

    UFunction* Fn = FindFunctionByName(AudioCls, "Play");
    if (!Fn) { Log("\n[EXEC] UAudioComponent::Play 를 못 찾음"); return false; }

    gPlayFunc = Fn;
    gOriginalPlayExec = reinterpret_cast<NativeExecFn>(Fn->ExecFunction);

    DWORD Old = 0;
    VirtualProtect(&Fn->ExecFunction, sizeof(void*), PAGE_READWRITE, &Old);
    Fn->ExecFunction = reinterpret_cast<UFunction::FNativeFuncPtr>(&HookedPlayExec);
    VirtualProtect(&Fn->ExecFunction, sizeof(void*), Old, &Old);

    char Buf[256];
    snprintf(Buf, sizeof(Buf),
             "\n[EXEC] UAudioComponent::Play 후킹 완료\n"
             "       원본 ExecFunction = %p", (void*)gOriginalPlayExec);
    Log(Buf);
    return true;
}

static void RemovePlayHook()
{
    if (!gPlayFunc || !gOriginalPlayExec) return;
    DWORD Old = 0;
    VirtualProtect(&gPlayFunc->ExecFunction, sizeof(void*), PAGE_READWRITE, &Old);
    gPlayFunc->ExecFunction = reinterpret_cast<UFunction::FNativeFuncPtr>(gOriginalPlayExec);
    VirtualProtect(&gPlayFunc->ExecFunction, sizeof(void*), Old, &Old);
    gPlayFunc = nullptr;
    Log("[EXEC] Play 후크 복원");
}

/* 교체할 소리를 고르고 exec 후크를 켠다. */
static int gOverrideIndex = -1;
static void CycleSoundOverride()
{
    if (!InstallPlayHook()) return;

    auto Cues = FindLoadedSoundCues();
    if (Cues.empty()) { Log("\n[EXEC] 로드된 사운드가 없습니다."); return; }

    char Buf[512];

    gOverrideIndex++;
    if (gOverrideIndex >= static_cast<int>(Cues.size()))
    {
        gOverrideIndex  = -1;
        gSoundOverride  = nullptr;
        Log("\n[EXEC] 소리 교체 해제 — 원본 SC_Provoaction 으로 돌아감");
        return;
    }

    gSoundOverride = static_cast<USoundBase*>(Cues[gOverrideIndex]);
    snprintf(Buf, sizeof(Buf),
             "\n[EXEC] 교체 대상 %d/%zu → %s (%s)\n"
             "       이제 도발하면 이 소리가 납니다. 로컬 전용입니다.",
             gOverrideIndex + 1, Cues.size(),
             gSoundOverride->GetName().c_str(),
             gSoundOverride->Class->GetName().c_str());
    Log(Buf);
}

static void SetSoundOverrideByName(const char* Needle)
{
    if (!InstallPlayHook()) return;

    char Buf[512];
    for (UObject* S : FindLoadedSoundCues())
    {
        if (!Contains(S->GetName(), Needle)) continue;
        gSoundOverride = static_cast<USoundBase*>(S);
        snprintf(Buf, sizeof(Buf), "\n[EXEC] 교체 대상 설정: %s", S->GetName().c_str());
        Log(Buf);
        return;
    }
    snprintf(Buf, sizeof(Buf), "\n[EXEC] \"%s\" 를 못 찾음", Needle);
    Log(Buf);
}

static void RemoveHooks()
{
    const int Idx = Offsets::ProcessEventIdx;
    std::unique_lock<std::shared_mutex> Lock(gOriginalLock);

    int Restored = 0;
    for (void** VTable : gPatchedVTables)
    {
        auto It = gOriginalByVTable.find(VTable);
        if (It == gOriginalByVTable.end())
            continue;   /* 원본을 모르면 건드리지 않는다 */
        if (PatchSlot(VTable, Idx, reinterpret_cast<void*>(It->second), nullptr))
            Restored++;
    }

    printf("[+] vtable %d/%zu개 복원\n", Restored, gPatchedVTables.size());
    gPatchedVTables.clear();
    gKnownVTables.clear();
    gOriginalByVTable.clear();
}

// ─────────────────────────────────────────────────────────────
// 요약 출력
// ─────────────────────────────────────────────────────────────

static void PrintSummary()
{
    std::lock_guard<std::mutex> Lock(gMutex);

    std::vector<CallInfo> All;
    All.reserve(gSeen.size());
    for (auto& [K, V] : gSeen) All.push_back(V);

    /* 네트워크 함수를 위로. 그게 공격 가능한 것들이다. */
    std::sort(All.begin(), All.end(), [](const CallInfo& A, const CallInfo& B) {
        auto Rank = [](uint32_t F) {
            if (F & 0x00200000) return 0;   // NetServer
            if (F & 0x00004000) return 1;   // NetMulticast
            if (F & 0x01000000) return 2;   // NetClient
            if (F & 0x00000040) return 3;   // Net
            return 4;
        };
        int RA = Rank(A.Flags), RB = Rank(B.Flags);
        return RA != RB ? RA < RB : A.FullName < B.FullName;
    });

    Log("\n======================================================================");
    Log("호출된 함수 요약 — 네트워크 함수부터");
    Log("======================================================================");

    for (auto& Info : All)
    {
        char Buf[1024];
        snprintf(Buf, sizeof(Buf), "\n%s   (x%llu)\n  caller : %s\n  flags  : %s\n  net    : %s",
                 Info.FullName.c_str(), (unsigned long long)Info.Count,
                 Info.CallerClass.c_str(),
                 FlagsToString(Info.Flags).c_str(), NetKind(Info.Flags));
        Log(Buf);
    }

    char Tail[256];
    snprintf(Tail, sizeof(Tail), "\n총 %zu개 고유 함수. 로그: %s", All.size(), kLogPath);
    Log(Tail);
}

// ─────────────────────────────────────────────────────────────

static bool KeyPressed(int VK)
{
    /* 눌린 순간 한 번만 true. GetAsyncKeyState 의 최하위 비트가 "직전 호출 이후 눌림" 이다. */
    return (GetAsyncKeyState(VK) & 1) != 0;
}

static DWORD WINAPI MainThread(HMODULE Module)
{
    AllocConsole();
    FILE* Dummy;
    freopen_s(&Dummy, "CONOUT$", "w", stdout);
    freopen_s(&Dummy, "CONIN$",  "r", stdin);
    /* 소스가 UTF-8 이라 콘솔 코드페이지를 맞춰주지 않으면 한글이 깨진다. */
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleTitleA("whistle.dll - ProcessEvent Logger");

    gStartTick = GetTickCount64();
    gLogFile.open(kLogPath, std::ios::app);

    printf("======================================================\n");
    printf(" whistle.dll v0.1 — ProcessEvent 로거\n");
    printf("======================================================\n");
    printf(" [TRACE] Provocation/MOUIIYO/EEYAN/개발자키는 매 호출마다 기록\n");
    printf("------------------------------------------------------\n");
    printf(" F12  캐릭터 오디오 컴포넌트 상태\n");
    printf(" INS  휘파람 볼륨 증폭 토글 (로컬 전용)\n");
    printf(" HOME 로드된 사운드 전체 / PgUp: Provoaction / PgDn: SC_*\n");
    printf(" DEL  휘파람 소리 교체 - exec 후킹 방식 (로컬 전용)\n");
    printf(" BKSP 휘파람 소리를 HIKAKIN 버전으로\n");
    printf(" TAB  Sound 직접 쓰기 - 덮어써지는 것 확인용 (비교 실험)\n");
    printf(" F1   관전 상태 / 지목 가능 대상 목록\n");
    printf(" F2   ProvocationRemote(관전중인 대상)   정상 케이스\n");
    printf(" F3   ProvocationRemote(다른 대상)       <- 공격 케이스\n");
    printf(" F4   Provocation(Local)  직접 호출\n");
    printf(" F5   Provocation(Server) 직접 호출   <- 게이트 우회 실험\n");
    printf(" F6   Provocation(Client) 직접 호출\n");
    printf(" F7   클래스별 후킹 상태 진단\n");
    printf(" F8   vtable 수동 재스캔 (2초마다 자동으로도 돎)\n");
    printf(" F9   로깅 on/off\n");
    printf(" F10  요약 출력\n");
    printf(" F11  노이즈 필터 on/off\n");
    printf(" END  언로드\n");
    printf("------------------------------------------------------\n");
    printf(" 휘파람(Provocation)은 매치용 캐릭터 클래스에만 있습니다.\n");
    printf(" 로비 폰(BP_FirstPersonCharacter_Main_C)에는 없으므로\n");
    printf(" 반드시 매치에 진입한 뒤에 1번키를 누르세요.\n");
    printf("======================================================\n\n");

    if (InstallHooks() == 0)
    {
        printf("[-] 후킹 실패. GObjects 를 못 찾았거나 SDK 오프셋이 어긋났습니다.\n");
        return 0;
    }

    int Ticks = 0;
    while (gRunning)
    {
        /* 주기적 재스캔.
           주입 시점에 아직 로드되지 않은 클래스는 vtable 이 패치돼 있지 않다.
           이 게임은 로비 폰(BP_FirstPersonCharacter_Main_C)과
           매치 폰(BP_FirstPersonCharacter_cLeon_Character_C)이 다른 클래스라,
           매치 진입 시점에 새 클래스가 로드된다. 재스캔이 없으면 그걸 놓친다.

           GObjects 는 뒤로만 늘어나므로 지난번 지점부터만 훑는다. 비용이 거의 없다. */
        if (++Ticks % 100 == 0)          // 20ms * 100 = 약 2초
        {
            InstallHooks();
            /* 도발 재생을 항상 기록하려면 Play 후크가 상시 걸려 있어야 한다.
               GObjects 가 준비된 뒤에야 UAudioComponent 를 찾을 수 있어
               여기서 반복 시도한다(설치되면 즉시 반환된다). */
            InstallPlayHook();
        }

        if (KeyPressed(VK_F12))    DumpAudioState();
        if (KeyPressed(VK_INSERT)) ToggleVolumeBoost();
        if (KeyPressed(VK_HOME))   ListSounds("");
        /* 게임 자산 이름에 오타가 있다. Provocation 이 아니라 Provoaction 이다. */
        if (KeyPressed(VK_PRIOR))  ListSounds("Provoaction");
        if (KeyPressed(VK_NEXT))   ListSounds("SC_");
        if (KeyPressed(VK_DELETE)) CycleSoundOverride();
        if (KeyPressed(VK_BACK))   SetSoundOverrideByName("Provoaction_HIKAKIN");
        if (KeyPressed(VK_TAB))    CycleWhistleSound();   // 직접 쓰기 — 덮어써짐. 비교용

        if (KeyPressed(VK_F1))  ShowSpectateState();
        if (KeyPressed(VK_F2))  CallProvocationRemote(false);   // 관전 중인 대상
        if (KeyPressed(VK_F3))  CallProvocationRemote(true);    // 그 외 대상 ★
        if (KeyPressed(VK_F4))  CallOnLocalPawn("Provocation(Local)");
        if (KeyPressed(VK_F5))  CallOnLocalPawn("Provocation(Server)");
        if (KeyPressed(VK_F6))  CallOnLocalPawn("Provocation(Client)");

        if (KeyPressed(VK_F7))
        {
            DiagnoseClasses();
        }
        if (KeyPressed(VK_F8))
        {
            printf("\n[*] 수동 재스캔\n");
            if (InstallHooks() == 0)
                printf("    새로 패치할 vtable 없음 (누적 %zu)\n", gKnownVTables.size());
        }
        if (KeyPressed(VK_F9))
        {
            gLogging = !gLogging;
            printf("\n[*] 로깅 %s\n", gLogging ? "ON" : "OFF");
        }
        if (KeyPressed(VK_F10))  PrintSummary();
        if (KeyPressed(VK_F11))
        {
            gFiltered = !gFiltered;
            printf("\n[*] 노이즈 필터 %s\n", gFiltered ? "ON" : "OFF");
        }
        if (KeyPressed(VK_END))  gRunning = false;

        Sleep(20);
    }

    PrintSummary();
    RemovePlayHook();
    RemoveHooks();

    /* 후킹된 함수가 실행 중일 수 있으므로 잠깐 기다린 뒤 언로드한다. */
    Sleep(500);

    gLogFile.close();
    fclose(stdout);
    FreeConsole();
    FreeLibraryAndExitThread(Module, 0);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, (LPTHREAD_START_ROUTINE)MainThread, hModule, 0, nullptr);
    }
    return TRUE;
}
