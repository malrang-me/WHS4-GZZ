#!/usr/bin/env python3
"""
UE5 External SDK Dumper  (MECCHA CHAMELEON / UE 5.6.1)

인젝션 없이 ReadProcessMemory 만으로 언리얼 런타임 구조를 덤프한다.
게임 프로세스에 아무것도 쓰지 않으므로 크래시 위험이 없다.

사용:
    python ue_sdk_dumper.py scan                 GNames / GObjects 탐색
    python ue_sdk_dumper.py names [N]            이름 테이블 앞 N개 출력
    python ue_sdk_dumper.py find <문자열>         이름에 문자열이 들어간 객체 검색
    python ue_sdk_dumper.py classes [N]          클래스 목록
    python ue_sdk_dumper.py dump <클래스명>       클래스의 프로퍼티/함수 덤프
    python ue_sdk_dumper.py dumpall <출력파일>    전체 SDK 덤프

전제: 게임이 실행 중이어야 한다 (런타임에만 이름 테이블이 존재).
"""

import sys
import struct
import argparse

try:
    import pymem
    import pymem.process
except ImportError:
    print("pymem 이 없습니다.  pip install pymem")
    sys.exit(1)


# ---------------------------------------------------------------- 설정

PROCESS_NAME = "PenguinHotel-Win64-Shipping.exe"

# UE 5.6 기준 구조체 오프셋. 버전이 다르면 여기를 조정한다.
OFF = {
    # FNameEntryAllocator
    "name_blocks":          0x10,   # FRWLock(8) + CurrentBlock(4) + CurrentByteCursor(4)
    "name_block_bits":      16,     # FNameBlockOffsetBits
    "name_stride":          2,      # 엔트리 정렬 단위

    # FUObjectArray -> FChunkedFixedUObjectArray
    "objarray_chunks":      0x10,   # FUObjectArray 안의 ObjObjects 위치
    "chunk_objects":        0x00,
    "chunk_max_elements":   0x10,
    "chunk_num_elements":   0x14,
    "chunk_max_chunks":     0x18,
    "chunk_num_chunks":     0x1C,
    "objects_per_chunk":    64 * 1024,
    "uobject_item_size":    24,     # {UObject*, Flags, ClusterRootIndex, SerialNumber}

    # UObject
    "obj_flags":            0x08,
    "obj_internal_index":   0x0C,
    "obj_class":            0x10,
    "obj_name":             0x18,
    "obj_outer":            0x20,

    # UStruct
    "struct_super":         0x40,
    "struct_children":      0x48,   # UField*  (함수/열거형 등)
    "struct_child_props":   0x50,   # FField*  (프로퍼티)
    "struct_props_size":    0x58,

    # UField
    "field_next":           0x28,

    # FField  (UE 4.25+ 프로퍼티 계층)
    "ffield_class":         0x08,
    "ffield_next":          0x20,
    "ffield_name":          0x28,

    # FProperty
    "prop_array_dim":       0x34,
    "prop_element_size":    0x38,
    "prop_flags":           0x40,
    "prop_offset":          0x4C,

    # UFunction
    "func_flags":           0xB0,
}

# UFunction::FunctionFlags 중 네트워크 관련 비트
FUNC_FLAGS = [
    (0x00000001, "Final"),
    (0x00000004, "BlueprintAuthorityOnly"),
    (0x00000020, "Exec"),
    (0x00000040, "Native"),
    (0x00000080, "Event"),
    (0x00000200, "Net"),                 # 네트워크 함수
    (0x00000400, "NetReliable"),
    (0x00000800, "NetRequest"),
    (0x00001000, "Ptr"),
    (0x00002000, "NetResponse"),
    (0x00004000, "Static"),
    (0x00008000, "NetMulticast"),        # 서버 -> 전원
    (0x00200000, "NetServer"),           # 클라 -> 서버
    (0x01000000, "NetClient"),           # 서버 -> 특정 클라
    (0x04000000, "BlueprintCallable"),
]


# ---------------------------------------------------------------- 메모리

class Mem:
    def __init__(self, process_name):
        try:
            self.pm = pymem.Pymem(process_name)
        except Exception as e:
            print(f"[!] 프로세스를 열 수 없습니다: {process_name}")
            print(f"    게임이 실행 중인지, 관리자 권한으로 실행했는지 확인하세요.")
            print(f"    ({e})")
            sys.exit(1)

        self.module = pymem.process.module_from_name(
            self.pm.process_handle, process_name)
        self.base = self.module.lpBaseOfDll
        self.size = self.module.SizeOfImage
        print(f"[+] {process_name}  pid={self.pm.process_id}")
        print(f"[+] base=0x{self.base:X}  size=0x{self.size:X}")

    def read(self, addr, size):
        try:
            return self.pm.read_bytes(addr, size)
        except Exception:
            return None

    def u8(self, a):
        b = self.read(a, 1)
        return b[0] if b else 0

    def u16(self, a):
        b = self.read(a, 2)
        return struct.unpack("<H", b)[0] if b else 0

    def i32(self, a):
        b = self.read(a, 4)
        return struct.unpack("<i", b)[0] if b else 0

    def u32(self, a):
        b = self.read(a, 4)
        return struct.unpack("<I", b)[0] if b else 0

    def u64(self, a):
        b = self.read(a, 8)
        return struct.unpack("<Q", b)[0] if b else 0

    def valid_ptr(self, p):
        """사용자 영역 포인터로 그럴듯한지 대충 거른다."""
        return 0x10000 < p < 0x7FFFFFFFFFFF and (p & 0x7) == 0

    def in_module(self, p):
        return self.base <= p < self.base + self.size


# ---------------------------------------------------------------- 이름 테이블

class Names:
    """
    FNamePool (UE 4.23+) 블록 방식 이름 테이블.

    index -> block = index >> 16
             offset = (index & 0xFFFF) * 2
    엔트리 헤더(uint16): bIsWide:1 | LowercaseProbeHash:5 | Len:10
    """

    def __init__(self, mem, pool_addr):
        self.m = mem
        self.pool = pool_addr
        self.blocks = pool_addr + OFF["name_blocks"]
        self.cache = {}

    def block_ptr(self, i):
        return self.m.u64(self.blocks + i * 8)

    def get(self, index):
        if index in self.cache:
            return self.cache[index]

        block = index >> OFF["name_block_bits"]
        offset = index & ((1 << OFF["name_block_bits"]) - 1)

        bp = self.block_ptr(block)
        if not self.m.valid_ptr(bp):
            return None

        entry = bp + offset * OFF["name_stride"]
        header = self.m.u16(entry)
        length = header >> 6
        is_wide = header & 1

        if length == 0 or length > 1024:
            return None

        if is_wide:
            raw = self.m.read(entry + 2, length * 2)
            s = raw.decode("utf-16-le", "replace") if raw else None
        else:
            raw = self.m.read(entry + 2, length)
            s = raw.decode("utf-8", "replace") if raw else None

        self.cache[index] = s
        return s

    def fname(self, addr):
        """FName{ComparisonIndex:u32, Number:u32} 를 읽어 문자열로."""
        idx = self.m.u32(addr)
        num = self.m.u32(addr + 4)
        s = self.get(idx)
        if s is None:
            return None
        return s if num == 0 else f"{s}_{num - 1}"

    def self_test(self):
        """FName 인덱스 0 은 언리얼에서 항상 'None' 이다."""
        return self.get(0) == "None"


# ---------------------------------------------------------------- 탐색

def scan_gnames(mem):
    """
    .data 영역에서 FNamePool 후보를 찾는다.
    검증 기준: 인덱스 0 이 'None' 으로 읽히는가.
    패턴 매칭보다 이 구조적 검증이 버전 변화에 훨씬 강하다.
    """
    print("[*] GNames 탐색 중 ...")
    step = 8
    for addr in range(mem.base, mem.base + mem.size - 0x1000, step):
        first_block = mem.u64(addr + OFF["name_blocks"])
        if not mem.valid_ptr(first_block) or mem.in_module(first_block):
            continue
        n = Names(mem, addr)
        if n.self_test():
            print(f"[+] GNames = 0x{addr:X}   (base+0x{addr - mem.base:X})")
            return n
    print("[!] GNames 를 찾지 못했습니다. OFF['name_blocks'] 를 확인하세요.")
    return None


def scan_gobjects(mem, names):
    """
    FUObjectArray 후보를 구조적으로 검증하며 찾는다.
    - Objects 포인터가 유효
    - 0 < NumElements <= MaxElements
    - 청크 수가 합리적
    - 첫 객체의 ClassPrivate 이름이 정상적으로 읽힘
    """
    print("[*] GObjects 탐색 중 ...")
    for addr in range(mem.base, mem.base + mem.size - 0x1000, 8):
        c = addr + OFF["objarray_chunks"]
        objects = mem.u64(c + OFF["chunk_objects"])
        if not mem.valid_ptr(objects) or mem.in_module(objects):
            continue

        max_el = mem.i32(c + OFF["chunk_max_elements"])
        num_el = mem.i32(c + OFF["chunk_num_elements"])
        max_ch = mem.i32(c + OFF["chunk_max_chunks"])
        num_ch = mem.i32(c + OFF["chunk_num_chunks"])

        if not (0 < num_el <= max_el < 100_000_000):
            continue
        if not (0 < num_ch <= max_ch < 100_000):
            continue

        arr = Objects(mem, names, c)
        obj = arr.at(0)
        if not obj:
            continue
        nm = arr.obj_name(obj)
        if nm:
            print(f"[+] GObjects = 0x{addr:X}   (base+0x{addr - mem.base:X})")
            print(f"    객체 수 = {num_el:,}  청크 = {num_ch}")
            print(f"    검증: 0번 객체 = {nm}")
            return arr
    print("[!] GObjects 를 찾지 못했습니다. OFF['objarray_chunks'] 를 확인하세요.")
    return None


# ---------------------------------------------------------------- 객체 배열

class Objects:
    def __init__(self, mem, names, chunk_struct):
        self.m = mem
        self.n = names
        self.c = chunk_struct

    @property
    def count(self):
        return self.m.i32(self.c + OFF["chunk_num_elements"])

    def at(self, index):
        per = OFF["objects_per_chunk"]
        chunks = self.m.u64(self.c + OFF["chunk_objects"])
        chunk = self.m.u64(chunks + (index // per) * 8)
        if not self.m.valid_ptr(chunk):
            return 0
        item = chunk + (index % per) * OFF["uobject_item_size"]
        obj = self.m.u64(item)
        return obj if self.m.valid_ptr(obj) else 0

    # -- 객체 정보 -------------------------------------------------

    def obj_name(self, obj):
        return self.n.fname(obj + OFF["obj_name"])

    def obj_class(self, obj):
        return self.m.u64(obj + OFF["obj_class"])

    def obj_outer(self, obj):
        return self.m.u64(obj + OFF["obj_outer"])

    def full_name(self, obj):
        """Outer 체인을 거슬러 올라가 전체 경로를 만든다."""
        parts = []
        cur = obj
        for _ in range(16):
            if not self.m.valid_ptr(cur):
                break
            nm = self.obj_name(cur)
            if not nm:
                break
            parts.append(nm)
            cur = self.obj_outer(cur)
        cls = self.obj_class(obj)
        cls_name = self.obj_name(cls) if cls else "?"
        return f"{cls_name} {'.'.join(reversed(parts))}"

    def is_a(self, obj, class_name):
        """obj 의 클래스 상속 사슬에 class_name 이 있는가."""
        cls = self.obj_class(obj)
        for _ in range(32):
            if not self.m.valid_ptr(cls):
                return False
            if self.obj_name(cls) == class_name:
                return True
            cls = self.m.u64(cls + OFF["struct_super"])
        return False

    def iter_all(self):
        for i in range(self.count):
            o = self.at(i)
            if o:
                yield i, o

    def find_class(self, name):
        for _, o in self.iter_all():
            if self.obj_name(o) == name and self.is_a(o, "Class"):
                return o
        return 0


# ---------------------------------------------------------------- 덤프

def flag_names(flags):
    out = [n for bit, n in FUNC_FLAGS if flags & bit]
    return " | ".join(out) if out else "-"


def dump_class(arr, cls, out=sys.stdout):
    m, n = arr.m, arr.n

    name = arr.obj_name(cls)
    super_cls = m.u64(cls + OFF["struct_super"])
    super_name = arr.obj_name(super_cls) if super_cls else None
    size = m.i32(cls + OFF["struct_props_size"])

    print(f"\n// {'=' * 66}", file=out)
    print(f"// {name}"
          + (f" : {super_name}" if super_name else "")
          + f"   (size 0x{size:X})", file=out)
    print(f"// {'=' * 66}", file=out)

    # --- 프로퍼티 (FField 사슬) ---
    print(f"\n[properties]", file=out)
    prop = m.u64(cls + OFF["struct_child_props"])
    count = 0
    while m.valid_ptr(prop) and count < 4096:
        pname = n.fname(prop + OFF["ffield_name"])
        pcls = m.u64(prop + OFF["ffield_class"])
        ptype = n.fname(pcls) if m.valid_ptr(pcls) else "?"
        poff = m.i32(prop + OFF["prop_offset"])
        psize = m.i32(prop + OFF["prop_element_size"])
        pdim = m.i32(prop + OFF["prop_array_dim"])
        if pname:
            dim = f"[{pdim}]" if pdim > 1 else ""
            print(f"  0x{poff:04X}  {psize:>4}  {ptype:<24} {pname}{dim}", file=out)
        prop = m.u64(prop + OFF["ffield_next"])
        count += 1

    # --- 함수 (UField 사슬) ---
    print(f"\n[functions]", file=out)
    child = m.u64(cls + OFF["struct_children"])
    count = 0
    while m.valid_ptr(child) and count < 4096:
        fname = arr.obj_name(child)
        if fname and arr.is_a(child, "Function"):
            flags = m.u32(child + OFF["func_flags"])
            print(f"  {fname:<44} 0x{flags:08X}  {flag_names(flags)}", file=out)
        child = m.u64(child + OFF["field_next"])
        count += 1


# ---------------------------------------------------------------- 명령

def cmd_scan(mem):
    names = scan_gnames(mem)
    if not names:
        return
    print(f"    검증: FName[0] = '{names.get(0)}'  FName[1] = '{names.get(1)}'")
    scan_gobjects(mem, names)


def setup(mem):
    names = scan_gnames(mem)
    if not names:
        sys.exit(1)
    arr = scan_gobjects(mem, names)
    if not arr:
        sys.exit(1)
    return names, arr


def main():
    ap = argparse.ArgumentParser(description="UE5 External SDK Dumper")
    ap.add_argument("command",
                    choices=["scan", "names", "find", "classes", "dump", "dumpall"])
    ap.add_argument("arg", nargs="?", default=None)
    ap.add_argument("--process", default=PROCESS_NAME)
    a = ap.parse_args()

    mem = Mem(a.process)

    if a.command == "scan":
        cmd_scan(mem)
        return

    names, arr = setup(mem)

    if a.command == "names":
        limit = int(a.arg) if a.arg else 50
        for i in range(limit):
            s = names.get(i)
            if s:
                print(f"  [{i:5}] {s}")

    elif a.command == "find":
        if not a.arg:
            print("검색할 문자열을 지정하세요.")
            return
        key = a.arg.lower()
        hits = 0
        for i, o in arr.iter_all():
            nm = arr.obj_name(o)
            if nm and key in nm.lower():
                print(f"  [{i:7}] 0x{o:X}  {arr.full_name(o)}")
                hits += 1
                if hits >= 200:
                    print("  ... (200개에서 중단)")
                    break
        print(f"\n총 {hits}건")

    elif a.command == "classes":
        limit = int(a.arg) if a.arg else 100
        shown = 0
        for _, o in arr.iter_all():
            if arr.is_a(o, "Class"):
                print(f"  {arr.obj_name(o)}")
                shown += 1
                if shown >= limit:
                    break

    elif a.command == "dump":
        if not a.arg:
            print("클래스명을 지정하세요.")
            return
        cls = arr.find_class(a.arg)
        if not cls:
            print(f"클래스를 찾지 못했습니다: {a.arg}")
            return
        dump_class(arr, cls)

    elif a.command == "dumpall":
        path = a.arg or "sdk_dump.txt"
        with open(path, "w", encoding="utf-8") as f:
            print(f"// UE5 SDK dump  ({a.process})", file=f)
            n = 0
            for _, o in arr.iter_all():
                if arr.is_a(o, "Class"):
                    dump_class(arr, o, out=f)
                    n += 1
                    if n % 100 == 0:
                        print(f"  ... {n} 클래스")
            print(f"[+] {n} 클래스를 {path} 에 저장했습니다.")


if __name__ == "__main__":
    main()
