"""
Dumper-7 SDK 질의 도구

Dumper-7 이 뽑아낸 C++ 헤더는 사람이 눈으로 훑기에는 너무 크다
(보통 수백 개 파일, 수십만 줄). 이 스크립트는 그 결과물을 색인해서
docs/17 체크리스트의 질문에 바로 답할 수 있게 한다.

핵심은 Dumper-7 이 함수마다 이런 주석을 남긴다는 점이다.

    // Function BP_Foo.BP_Foo_C.DoThing
    // (Net, NetReliable, NetServer, BlueprintCallable)
    void ABP_Foo_C::DoThing(int32 Value)

두 번째 줄이 곧 FunctionFlags 이고, 체크리스트 1번 항목의 답이다.

사용:
    python tools/sdk_query.py find <정규식>        함수 이름 검색
    python tools/sdk_query.py cls <정규식>         클래스 이름 검색
    python tools/sdk_query.py net                  네트워크 함수 전부 나열
    python tools/sdk_query.py show <함수정규식>    함수 시그니처 + 플래그 + 파라미터
    python tools/sdk_query.py members <클래스명>   클래스 멤버 변수 + 오프셋
    python tools/sdk_query.py whistle              휘파람 체크리스트 자동 조사
"""

import os
import re
import sys
from collections import defaultdict

# Dumper-7 기본 출력 경로. 다른 곳에 뽑았으면 SDK_ROOT 환경변수로 덮어쓴다.
DEFAULT_ROOTS = [
    os.environ.get("SDK_ROOT", ""),
    r"C:\Dumper-7",
]

# 이 게임의 고유 접두사 (docs/16 §1)
GAME_PREFIX = "cLeon"

# 휘파람 관련 이름 조각. docs/19 에서 확정된 실제 명칭이다.
# 개발사가 일본 인디라 내부 명칭이 일본어 기반이었고,
# 정적 조사 때 쓴 whistle/taunt 류 영어 키워드로는 하나도 잡히지 않았다.
WHISTLE_HINTS = [
    "provocation",      # 도발 — 휘파람 본체
    "mouiiyo", "moueeyo",   # もういいよ = "다 됐어"
    "eeyan",            # ええやん = "좋다"
    "emote", "voice", "sound", "audio",
]

# 위치 파라미터로 볼 타입/이름. 체크리스트 2번의 판정 기준이다.
LOCATION_HINTS = ["FVector", "Location", "Position", "Origin", "Pos", "Transform"]

NET_FLAGS = ["Net", "NetServer", "NetClient", "NetMulticast", "NetReliable"]


# ─────────────────────────────────────────────────────────────
# SDK 위치 찾기
# ─────────────────────────────────────────────────────────────

def find_sdk_root():
    """Dumper-7 출력 폴더 안에서 실제 SDK 디렉터리를 찾는다.

    구조가 보통 이렇다:
        C:\\Dumper-7\\<Game>-<Ver>\\CppSDK\\SDK\\*.hpp
    버전 폴더 이름을 미리 알 수 없으므로 SDK 폴더를 탐색한다.
    """
    for root in DEFAULT_ROOTS:
        if not root or not os.path.isdir(root):
            continue
        for dirpath, dirnames, filenames in os.walk(root):
            if os.path.basename(dirpath) == "SDK" and any(
                f.endswith((".hpp", ".cpp")) for f in filenames
            ):
                return dirpath
        # SDK 하위폴더가 없으면 헤더가 바로 있는지 본다
        if any(f.endswith(".hpp") for f in os.listdir(root)):
            return root
    return None


# ─────────────────────────────────────────────────────────────
# 색인
# ─────────────────────────────────────────────────────────────

# Dumper-7 의 함수 블록은 이렇게 생겼다.
#
#     // Function Pkg.Class_C.Func
#     // (BlueprintCallable, Net, NetServer)
#     // Parameters:
#     // int32   Value   (Parm, ZeroConstructor)
#     <빈 줄>
#     void AClass_C::Func(int32 Value)
#
# 주석 블록과 시그니처 사이의 **빈 줄**을 허용해야 한다.
# 이걸 빼먹으면 전체 함수의 10% 정도밖에 안 잡힌다.
FUNC_RE = re.compile(
    r"^// Function (?P<full>\S+)[ \t]*\r?\n"        # // Function Pkg.Class.Func
    r"^// \((?P<flags>[^)]*)\)[ \t]*\r?\n"          # // (Flag, Flag, ...)
    r"(?P<params>(?:^//[^\n]*\r?\n|^[ \t]*\r?\n)*)" # 파라미터 주석 + 빈 줄
    r"^(?P<sig>[^\s/][^\n]*)",                      # 실제 시그니처 줄
    re.MULTILINE,
)

# 클래스/구조체 멤버:  Type Name;   // 0x0123(0x0008)(Flags)
MEMBER_RE = re.compile(
    r"^\s*(?P<type>[\w:<>,\s\*&\[\]]+?)\s+(?P<name>\w+)\s*(?P<arr>\[\d+\])?;\s*"
    r"//\s*0x(?P<off>[0-9A-Fa-f]+)\((?P<size>0x[0-9A-Fa-f]+)\)",
)

TYPE_DECL_RE = re.compile(
    r"^(?:class|struct)\s+(?:alignas\(\d+\)\s+)?(?P<name>\w+)"
    r"(?:\s*(?:final\s*)?:\s*public\s+(?P<base>[\w:]+))?",
    re.MULTILINE,
)


class Sdk:
    def __init__(self, root):
        self.root = root
        self.functions = []      # dict: full, flags(list), sig, params(str), file
        self.types = {}          # name -> dict(base, file, line)
        self._load()

    def _load(self):
        files = []
        for dirpath, _, filenames in os.walk(self.root):
            for f in filenames:
                if f.endswith((".hpp", ".cpp", ".h")):
                    files.append(os.path.join(dirpath, f))

        for path in files:
            try:
                text = open(path, "r", encoding="utf-8", errors="replace").read()
            except OSError:
                continue

            base = os.path.basename(path)

            if base.endswith("_functions.cpp") or "functions" in base:
                for m in FUNC_RE.finditer(text):
                    self.functions.append({
                        "full": m.group("full"),
                        "flags": [f.strip() for f in m.group("flags").split(",") if f.strip()],
                        "params": m.group("params").strip(),
                        "sig": m.group("sig").strip(),
                        "file": base,
                    })

            for m in TYPE_DECL_RE.finditer(text):
                name = m.group("name")
                if name not in self.types:
                    self.types[name] = {
                        "base": m.group("base"),
                        "file": base,
                        "path": path,
                        "pos": m.start(),
                    }

        self.functions.sort(key=lambda f: f["full"])

    # ── 조회 ────────────────────────────────────────────────

    def find_funcs(self, pattern):
        rx = re.compile(pattern, re.IGNORECASE)
        return [f for f in self.functions if rx.search(f["full"])]

    def find_types(self, pattern):
        rx = re.compile(pattern, re.IGNORECASE)
        return sorted(n for n in self.types if rx.search(n))

    def members(self, type_name):
        """클래스/구조체 본문을 잘라내 멤버 변수 목록을 뽑는다."""
        info = self.types.get(type_name)
        if not info:
            return None
        text = open(info["path"], "r", encoding="utf-8", errors="replace").read()
        start = text.find("{", info["pos"])
        if start < 0:
            return []
        # 중괄호 균형으로 본문 끝을 찾는다
        depth, i = 0, start
        while i < len(text):
            if text[i] == "{":
                depth += 1
            elif text[i] == "}":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        body = text[start:i]
        out = []
        for m in MEMBER_RE.finditer(body):
            out.append({
                "type": " ".join(m.group("type").split()),
                "name": m.group("name") + (m.group("arr") or ""),
                "offset": int(m.group("off"), 16),
                "size": int(m.group("size"), 16),
            })
        return out


# ─────────────────────────────────────────────────────────────
# 출력 헬퍼
# ─────────────────────────────────────────────────────────────

def net_kind(flags):
    """플래그 목록에서 네트워크 성격을 한 단어로 요약한다."""
    if "NetServer" in flags:
        return "NetServer"      # 클라 → 서버.  파라미터 위조 가능성 ★
    if "NetMulticast" in flags:
        return "NetMulticast"   # 서버 → 전원.  호스트만 조작 가능
    if "NetClient" in flags:
        return "NetClient"      # 서버 → 특정 클라
    if "Net" in flags:
        return "Net"
    return "-"


def print_func(f, verbose=False):
    kind = net_kind(f["flags"])
    mark = " ★" if kind == "NetServer" else ""
    print(f"  {f['full']}{mark}")
    print(f"      flags : {', '.join(f['flags']) or '(없음)'}")
    print(f"      net   : {kind}")
    print(f"      sig   : {f['sig']}")
    if verbose and f["params"]:
        for line in f["params"].splitlines():
            print(f"      {line}")
    print()


# ─────────────────────────────────────────────────────────────
# 휘파람 자동 조사 — docs/17 체크리스트
# ─────────────────────────────────────────────────────────────

def cmd_whistle(sdk):
    print("=" * 70)
    print("휘파람 스푸핑 — 덤프 자동 조사 (docs/17 체크리스트)")
    print("=" * 70)

    hint_rx = re.compile("|".join(WHISTLE_HINTS), re.IGNORECASE)
    cands = [f for f in sdk.functions if hint_rx.search(f["full"])]
    game_cands = [f for f in cands if GAME_PREFIX.lower() in f["full"].lower()]

    print(f"\n[1] 휘파람 후보 함수 — 전체 {len(cands)}개 / "
          f"게임 고유({GAME_PREFIX}) {len(game_cands)}개\n")

    show = game_cands or cands
    if not show:
        print("  후보 없음. WHISTLE_HINTS 에 다른 키워드를 추가해 보세요.")
    else:
        # 네트워크 함수를 위로 올린다 — 그게 공격 가능한 것들이다
        show.sort(key=lambda f: (net_kind(f["flags"]) == "-", f["full"]))
        for f in show[:40]:
            print_func(f, verbose=True)
        if len(show) > 40:
            print(f"  ... 외 {len(show) - 40}개 (find 명령으로 좁혀서 보세요)\n")

    # ── 2번: 위치 파라미터 ────────────────────────────────
    loc_rx = re.compile("|".join(LOCATION_HINTS), re.IGNORECASE)
    with_loc = [f for f in show if loc_rx.search(f["sig"]) or loc_rx.search(f["params"])]

    print("-" * 70)
    print(f"\n[2] 위치 파라미터를 받는 후보 — {len(with_loc)}개\n")
    if with_loc:
        for f in with_loc:
            print(f"  ★ {f['full']}")
            print(f"      {f['sig']}")
        print("\n  → 위치가 파라미터로 실린다. 위조하면 술래를 엉뚱한 곳으로 유인 가능.")
    else:
        print("  없음.")
        print("  → 서버가 복제된 액터 위치에서 좌표를 가져온다는 뜻.")
        print("     위치 스푸핑 불가 → 사운드 ID 변조/전송 억제 경로로 전환.")

    # ── 3번: 타이머 ───────────────────────────────────────
    print("\n" + "-" * 70)
    print("\n[3] 타이머 후보 (float 멤버 중 이름에 Time/Timer/Cool/Charge 포함)\n")
    timer_rx = re.compile(r"time|timer|cool|charge|interval|delay|count", re.IGNORECASE)
    targets = [n for n in sdk.types
               if GAME_PREFIX.lower() in n.lower() or "CentorCharge" in n]
    hits = 0
    for tname in sorted(targets):
        mem = sdk.members(tname) or []
        for m in mem:
            if timer_rx.search(m["name"]) and "float" in m["type"].lower():
                print(f"  {tname}::{m['name']}  ({m['type']})  +0x{m['offset']:X}")
                hits += 1
    if not hits:
        print("  float 타이머 없음. 아래를 직접 확인하세요:")
        print("    python tools/sdk_query.py members WBP_CentorCharge_C")
        print("    python tools/sdk_query.py cls TimerValue")

    # ── 4번: 역할 판별 ────────────────────────────────────
    print("\n" + "-" * 70)
    print("\n[4] 역할 판별 — Hunter / Survivor 클래스\n")
    for kw in ("Hunter", "Survivor"):
        found = sdk.find_types(kw)
        print(f"  {kw}: {len(found)}개")
        for n in found[:6]:
            print(f"      {n}")
        if len(found) > 6:
            print(f"      ... 외 {len(found) - 6}개")
    print("\n  → 두 역할이 서로 다른 클래스이므로 팀 변수를 찾을 필요 없이")
    print("     Pawn 의 Class 이름만 비교하면 역할이 판별된다.")

    # ── 7번: 권한 ─────────────────────────────────────────
    print("\n" + "-" * 70)
    print("\n[7] 권한 구조\n")
    for kw in ("GameMode", "GameState", "AuthorityGameMode"):
        found = [n for n in sdk.find_types(kw) if GAME_PREFIX.lower() in n.lower()
                 or n in ("AGameModeBase", "AGameStateBase")]
        for n in found[:5]:
            print(f"  {n}")

    # ── 결론 ──────────────────────────────────────────────
    print("\n" + "=" * 70)
    print("판정")
    print("=" * 70)
    if not show:
        print("  휘파람 함수 미특정 → 먼저 ProcessEvent 후킹으로 실제 호출을 로깅할 것.")
        return
    kinds = {net_kind(f["flags"]) for f in show}
    if "NetServer" in kinds and with_loc:
        print("  NetServer + 위치 파라미터 있음")
        print("  → 위치 스푸핑(V5) 가능. 최상의 경우.")
    elif "NetServer" in kinds:
        print("  NetServer + 위치 파라미터 없음")
        print("  → 사운드 ID 변조(V1) / 전송 억제(V2) 만 가능.")
    elif "NetMulticast" in kinds:
        print("  NetMulticast 만 존재")
        print("  → 클라이언트에서 조작 불가. 호스트 권한 연구로 전환.")
    else:
        print("  네트워크 플래그 없음")
        print("  → 로컬 타이머 조작 + 보이스챗 경로로 전환 (docs/16 §5).")


# ─────────────────────────────────────────────────────────────

def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1

    root = find_sdk_root()
    if not root:
        print("[-] SDK 를 찾을 수 없습니다.")
        print("    Dumper-7 을 주입해 덤프를 먼저 생성하거나,")
        print("    SDK_ROOT 환경변수로 경로를 지정하세요.")
        return 1

    print(f"[*] SDK: {root}")
    sdk = Sdk(root)
    print(f"[*] 함수 {len(sdk.functions)}개 / 타입 {len(sdk.types)}개 색인 완료\n")

    cmd = sys.argv[1]
    arg = sys.argv[2] if len(sys.argv) > 2 else None

    if cmd == "find" and arg:
        hits = sdk.find_funcs(arg)
        print(f"{len(hits)}개 일치\n")
        for f in hits[:60]:
            print_func(f)
    elif cmd == "cls" and arg:
        for n in sdk.find_types(arg):
            info = sdk.types[n]
            base = f" : {info['base']}" if info["base"] else ""
            print(f"  {n}{base}    [{info['file']}]")
    elif cmd == "net":
        hits = [f for f in sdk.functions if net_kind(f["flags"]) != "-"]
        game = [f for f in hits if GAME_PREFIX.lower() in f["full"].lower()]
        print(f"네트워크 함수 {len(hits)}개 (게임 고유 {len(game)}개)\n")
        for f in (game or hits)[:80]:
            print_func(f)
    elif cmd == "show" and arg:
        for f in sdk.find_funcs(arg):
            print_func(f, verbose=True)
    elif cmd == "members" and arg:
        mem = sdk.members(arg)
        if mem is None:
            print(f"[-] 타입을 찾을 수 없습니다: {arg}")
            print("    cls 명령으로 이름을 먼저 확인하세요.")
            return 1
        for m in mem:
            print(f"  +0x{m['offset']:04X}  {m['type']:<40} {m['name']}"
                  f"   (0x{m['size']:X})")
    elif cmd == "whistle":
        cmd_whistle(sdk)
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
