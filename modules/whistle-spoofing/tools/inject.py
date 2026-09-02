"""
DLL 인젝터 — CreateRemoteThread + LoadLibraryW 방식

컴파일러 없이 파이썬만으로 DLL을 대상 프로세스에 주입한다.
docs/12 §4(DLL 인젝션)에서 설명한 4단계를 그대로 구현한 것이다.

    1. OpenProcess          대상 프로세스 핸들 획득
    2. VirtualAllocEx       대상 프로세스 안에 메모리 확보
    3. WriteProcessMemory   그 메모리에 DLL 경로 문자열을 씀
    4. CreateRemoteThread   LoadLibraryW(경로) 를 대상 프로세스가 실행하게 함

사용:
    python tools/inject.py <dll경로> [프로세스명]

주의: 관리자 권한 터미널에서 실행할 것.
"""

import ctypes
import ctypes.wintypes as wt
import os
import sys

DEFAULT_PROCESS = "PenguinHotel-Win64-Shipping.exe"

PROCESS_ALL_ACCESS = 0x1F0FFF
MEM_COMMIT_RESERVE = 0x1000 | 0x2000
PAGE_READWRITE = 0x04
INFINITE = 0xFFFFFFFF

k32 = ctypes.WinDLL("kernel32", use_last_error=True)

k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k32.OpenProcess.restype = wt.HANDLE
k32.VirtualAllocEx.argtypes = [wt.HANDLE, wt.LPVOID, ctypes.c_size_t, wt.DWORD, wt.DWORD]
k32.VirtualAllocEx.restype = wt.LPVOID
k32.WriteProcessMemory.argtypes = [wt.HANDLE, wt.LPVOID, wt.LPCVOID, ctypes.c_size_t,
                                   ctypes.POINTER(ctypes.c_size_t)]
k32.WriteProcessMemory.restype = wt.BOOL
k32.GetModuleHandleW.argtypes = [wt.LPCWSTR]
k32.GetModuleHandleW.restype = wt.HMODULE
k32.GetProcAddress.argtypes = [wt.HMODULE, wt.LPCSTR]
k32.GetProcAddress.restype = wt.LPVOID
k32.CreateRemoteThread.argtypes = [wt.HANDLE, wt.LPVOID, ctypes.c_size_t, wt.LPVOID,
                                   wt.LPVOID, wt.DWORD, wt.LPVOID]
k32.CreateRemoteThread.restype = wt.HANDLE
k32.WaitForSingleObject.argtypes = [wt.HANDLE, wt.DWORD]
k32.WaitForSingleObject.restype = wt.DWORD
k32.GetExitCodeThread.argtypes = [wt.HANDLE, ctypes.POINTER(wt.DWORD)]
k32.GetExitCodeThread.restype = wt.BOOL
k32.CloseHandle.argtypes = [wt.HANDLE]

# argtypes/restype 를 반드시 지정한다. 안 하면 ctypes 가 반환값을 C int(32비트)로
# 취급해서 x64 핸들의 상위 32비트를 잘라버린다 — 조용히 실패하는 고전적인 버그다.
k32.CreateToolhelp32Snapshot.argtypes = [wt.DWORD, wt.DWORD]
k32.CreateToolhelp32Snapshot.restype = wt.HANDLE


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [
        ("dwSize", wt.DWORD), ("cntUsage", wt.DWORD),
        ("th32ProcessID", wt.DWORD), ("th32DefaultHeapID", ctypes.POINTER(ctypes.c_ulong)),
        ("th32ModuleID", wt.DWORD), ("cntThreads", wt.DWORD),
        ("th32ParentProcessID", wt.DWORD), ("pcPriClassBase", ctypes.c_long),
        ("dwFlags", wt.DWORD), ("szExeFile", ctypes.c_wchar * 260),
    ]


k32.Process32FirstW.argtypes = [wt.HANDLE, ctypes.POINTER(PROCESSENTRY32W)]
k32.Process32FirstW.restype = wt.BOOL
k32.Process32NextW.argtypes = [wt.HANDLE, ctypes.POINTER(PROCESSENTRY32W)]
k32.Process32NextW.restype = wt.BOOL

INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value


def find_pid(name):
    """프로세스 이름으로 PID를 찾는다. 스냅샷을 순회하는 표준 방식."""
    snap = k32.CreateToolhelp32Snapshot(0x00000002, 0)   # TH32CS_SNAPPROCESS
    if not snap or snap == INVALID_HANDLE_VALUE:
        raise OSError(f"CreateToolhelp32Snapshot 실패 (err={ctypes.get_last_error()})")
    entry = PROCESSENTRY32W()
    entry.dwSize = ctypes.sizeof(entry)
    found = []
    ok = k32.Process32FirstW(snap, ctypes.byref(entry))
    while ok:
        if entry.szExeFile.lower() == name.lower():
            found.append(entry.th32ProcessID)
        ok = k32.Process32NextW(snap, ctypes.byref(entry))
    k32.CloseHandle(snap)
    return found


def inject(pid, dll_path):
    dll_path = os.path.abspath(dll_path)
    if not os.path.isfile(dll_path):
        raise FileNotFoundError(dll_path)

    # DLL 경로에 비ASCII 문자가 있으면 LoadLibraryW 가 받는 UTF-16 그대로 쓰면 되지만,
    # 게임에 따라 경로 길이 제한에 걸릴 수 있어 경고만 해둔다.
    if not dll_path.isascii():
        print(f"[!] 경로에 비ASCII 문자가 있습니다: {dll_path}")

    h = k32.OpenProcess(PROCESS_ALL_ACCESS, False, pid)
    if not h:
        raise OSError(f"OpenProcess 실패 (err={ctypes.get_last_error()}) "
                      f"— 관리자 권한으로 실행했는지 확인하세요")

    try:
        buf = (dll_path + "\0").encode("utf-16-le")
        addr = k32.VirtualAllocEx(h, None, len(buf), MEM_COMMIT_RESERVE, PAGE_READWRITE)
        if not addr:
            raise OSError(f"VirtualAllocEx 실패 (err={ctypes.get_last_error()})")

        written = ctypes.c_size_t(0)
        if not k32.WriteProcessMemory(h, addr, buf, len(buf), ctypes.byref(written)):
            raise OSError(f"WriteProcessMemory 실패 (err={ctypes.get_last_error()})")

        # kernel32 는 모든 프로세스에서 같은 주소에 로드되므로
        # 내 프로세스에서 구한 LoadLibraryW 주소를 그대로 쓸 수 있다.
        load_library = k32.GetProcAddress(k32.GetModuleHandleW("kernel32.dll"), b"LoadLibraryW")
        if not load_library:
            raise OSError("LoadLibraryW 주소 획득 실패")

        thread = k32.CreateRemoteThread(h, None, 0, load_library, addr, 0, None)
        if not thread:
            raise OSError(f"CreateRemoteThread 실패 (err={ctypes.get_last_error()})")

        k32.WaitForSingleObject(thread, INFINITE)
        code = wt.DWORD(0)
        k32.GetExitCodeThread(thread, ctypes.byref(code))
        k32.CloseHandle(thread)

        # 스레드의 반환값 = LoadLibraryW 의 반환값 = 로드된 모듈 핸들(하위 32비트)
        if code.value == 0:
            print("[!] LoadLibraryW 가 NULL 을 반환했습니다. "
                  "DLL 이 x64 인지, 의존 DLL 이 전부 있는지 확인하세요.")
            return False
        print(f"[+] 주입 성공 — 모듈 핸들 하위 32비트: 0x{code.value:08X}")
        return True
    finally:
        k32.CloseHandle(h)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1

    dll = sys.argv[1]
    proc = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_PROCESS

    pids = find_pid(proc)
    if not pids:
        print(f"[-] 프로세스를 찾을 수 없습니다: {proc}")
        print("    게임을 먼저 실행하세요.")
        return 1
    if len(pids) > 1:
        print(f"[!] 같은 이름의 프로세스가 {len(pids)}개 있습니다: {pids} — 첫 번째를 사용합니다")

    pid = pids[0]
    print(f"[*] 대상: {proc} (PID {pid})")
    print(f"[*] DLL : {dll}")
    return 0 if inject(pid, dll) else 1


if __name__ == "__main__":
    sys.exit(main())
