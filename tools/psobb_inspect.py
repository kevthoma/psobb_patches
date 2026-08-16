"""Read-only live inspector for a running PSOBB client.

Confirms in a live process what static analysis only suggests. Deliberately READ-ONLY: it opens the
target with PROCESS_VM_READ only and never writes, injects, or allocates, so it cannot corrupt a
running client -- worst case a scan is slow.

PsoBB.exe has ASLR off and relocations stripped, so it always loads at 0x00400000 and every absolute
address is stable across runs and machines. An address confirmed once stays valid.

    python tools/psobb_inspect.py modules
    python tools/psobb_inspect.py read 0x008F812C 32
    python tools/psobb_inspect.py scan f32 0.25
    python tools/psobb_inspect.py narrow f32 0.5      # keep candidates now equal to 0.5
    python tools/psobb_inspect.py narrow changed      # keep candidates that changed since last pass
    python tools/psobb_inspect.py narrow unchanged
    python tools/psobb_inspect.py watch 0x00ABCDEF f32
    python tools/psobb_inspect.py find 8B0D????????85C9   # ?? = wildcard byte

The scan/narrow pair is the classic differential hunt: scan for a value you can see in-game, change it
in-game, narrow, repeat until one address remains. Candidates persist in .psobb_inspect_state.json
between invocations so narrowing works across separate runs of this script.
"""
from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import json
import os
import struct
import sys
import time

STATE_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), ".psobb_inspect_state.json")
DEFAULT_PROCESS = "PsoBB.exe"

k32 = ctypes.WinDLL("kernel32", use_last_error=True)

PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010
TH32CS_SNAPPROCESS = 0x00000002
TH32CS_SNAPMODULE = 0x00000008
TH32CS_SNAPMODULE32 = 0x00000010
MEM_COMMIT = 0x1000
PAGE_GUARD = 0x100
PAGE_NOACCESS = 0x01
READABLE = (0x02, 0x04, 0x20, 0x40)  # READONLY, READWRITE, EXECUTE_READ, EXECUTE_READWRITE

FMT = {  # name -> (struct code, size)
    "i32": ("<i", 4), "u32": ("<I", 4),
    "i16": ("<h", 2), "u16": ("<H", 2),
    "f32": ("<f", 4), "f64": ("<d", 8),
    "u8": ("<B", 1),
}


class PROCESSENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ProcessID", wt.DWORD),
                ("th32DefaultHeapID", ctypes.POINTER(ctypes.c_ulong)), ("th32ModuleID", wt.DWORD),
                ("cntThreads", wt.DWORD), ("th32ParentProcessID", wt.DWORD),
                ("pcPriClassBase", ctypes.c_long), ("dwFlags", wt.DWORD),
                ("szExeFile", ctypes.c_char * 260)]


class MODULEENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("th32ModuleID", wt.DWORD), ("th32ProcessID", wt.DWORD),
                ("GlblcntUsage", wt.DWORD), ("ProccntUsage", wt.DWORD),
                ("modBaseAddr", ctypes.POINTER(ctypes.c_byte)), ("modBaseSize", wt.DWORD),
                ("hModule", wt.HMODULE), ("szModule", ctypes.c_char * 256),
                ("szExePath", ctypes.c_char * 260)]


class MEMORY_BASIC_INFORMATION64(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_ulonglong), ("AllocationBase", ctypes.c_ulonglong),
                ("AllocationProtect", wt.DWORD), ("__alignment1", wt.DWORD),
                ("RegionSize", ctypes.c_ulonglong), ("State", wt.DWORD),
                ("Protect", wt.DWORD), ("Type", wt.DWORD), ("__alignment2", wt.DWORD)]


def find_pid(name: str) -> int:
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if snap == -1:
        raise OSError("CreateToolhelp32Snapshot failed")
    try:
        e = PROCESSENTRY32(); e.dwSize = ctypes.sizeof(e)
        ok = k32.Process32First(snap, ctypes.byref(e))
        while ok:
            if e.szExeFile.decode("latin1").lower() == name.lower():
                return e.th32ProcessID
            ok = k32.Process32Next(snap, ctypes.byref(e))
    finally:
        k32.CloseHandle(snap)
    raise SystemExit(f"process {name!r} is not running -- start the game first")


def modules(pid: int):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid)
    if snap == -1:
        raise OSError("module snapshot failed")
    out = []
    try:
        m = MODULEENTRY32(); m.dwSize = ctypes.sizeof(m)
        ok = k32.Module32First(snap, ctypes.byref(m))
        while ok:
            out.append((m.szModule.decode("latin1"),
                        ctypes.cast(m.modBaseAddr, ctypes.c_void_p).value or 0,
                        m.modBaseSize, m.szExePath.decode("latin1")))
            ok = k32.Module32Next(snap, ctypes.byref(m))
    finally:
        k32.CloseHandle(snap)
    return out


def open_target(name=None):
    # PSOBB_PROCESS overrides the target, for a renamed exe or for smoke-testing this script
    # against some other running process.
    name = name or os.environ.get("PSOBB_PROCESS") or DEFAULT_PROCESS
    pid = find_pid(name)
    h = k32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
    if not h:
        raise OSError(f"OpenProcess failed (error {ctypes.get_last_error()}) -- try an elevated shell")
    return pid, h


def read(h, addr: int, size: int):
    buf = (ctypes.c_char * size)()
    got = ctypes.c_size_t(0)
    if not k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, size, ctypes.byref(got)):
        return None
    return buf.raw[:got.value]


def regions(h, limit=0x7FFFFFFF):
    """Committed, readable, non-guard regions below `limit` (the 32-bit user space)."""
    addr = 0
    while addr < limit:
        mbi = MEMORY_BASIC_INFORMATION64()
        if not k32.VirtualQueryEx(h, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)):
            break
        size = int(mbi.RegionSize)
        if size <= 0:
            break
        if (mbi.State == MEM_COMMIT and not (mbi.Protect & PAGE_GUARD)
                and mbi.Protect != PAGE_NOACCESS and (mbi.Protect & 0xFF) in READABLE):
            yield int(mbi.BaseAddress), size
        addr = int(mbi.BaseAddress) + size


def load_state():
    if os.path.exists(STATE_FILE):
        with open(STATE_FILE, encoding="utf-8") as f:
            return json.load(f)
    return {}


def save_state(s):
    with open(STATE_FILE, "w", encoding="utf-8") as f:
        json.dump(s, f)


def cmd_modules(args):
    pid, h = open_target()
    print(f"pid {pid}")
    for name, base, size, path in modules(pid):
        print(f"  {name:<24} base 0x{base:08X}  size {size:>10}  {path}")
    k32.CloseHandle(h)


def cmd_read(args):
    addr = int(args[0], 0)
    size = int(args[1], 0) if len(args) > 1 else 64
    _, h = open_target()
    data = read(h, addr, size)
    if data is None:
        raise SystemExit(f"could not read 0x{addr:08X} (unmapped?)")
    for off in range(0, len(data), 16):
        chunk = data[off:off + 16]
        hexs = " ".join(f"{b:02X}" for b in chunk)
        text = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        print(f"  {addr + off:08X}  {hexs:<47}  {text}")
    for t in ("i32", "u32", "f32"):
        code, sz = FMT[t]
        if len(data) >= sz:
            print(f"  as {t}: {struct.unpack(code, data[:sz])[0]}")
    k32.CloseHandle(h)


def _pack(t, value):
    code, size = FMT[t]
    v = float(value) if t.startswith("f") else int(value, 0)
    return struct.pack(code, v), size


def cmd_scan(args):
    t, value = args[0], args[1]
    needle, size = _pack(t, value)
    pid, h = open_target()
    hits, scanned = [], 0
    for base, rsize in regions(h):
        data = read(h, base, rsize)
        if not data:
            continue
        scanned += len(data)
        start = 0
        while True:
            i = data.find(needle, start)
            if i < 0:
                break
            if i % 4 == 0 or size < 4:          # value alignment: skip absurd unaligned hits
                hits.append(base + i)
            start = i + 1
        if len(hits) > 500000:
            print("  (over 500k hits -- pick a more distinctive value)")
            break
    save_state({"type": t, "candidates": hits})
    print(f"  scanned {scanned/1048576:.1f} MB, {len(hits)} candidate(s) for {t}={value}")
    for a in hits[:20]:
        print(f"    0x{a:08X}")
    if len(hits) > 20:
        print(f"    ... and {len(hits)-20} more")
    k32.CloseHandle(h)


def cmd_narrow(args):
    st = load_state()
    if not st.get("candidates"):
        raise SystemExit("no candidates -- run a scan first")
    t = st["type"]
    code, size = FMT[t]
    mode = args[0]
    _, h = open_target()
    prev = st.get("values")
    keep, values = [], []
    for a in st["candidates"]:
        data = read(h, a, size)
        if not data or len(data) < size:
            continue
        cur = struct.unpack(code, data)[0]
        if mode in ("changed", "unchanged"):
            if prev is None:
                raise SystemExit("no previous values recorded -- run 'narrow <value>' once first")
            was = prev.get(str(a))
            if was is None:
                continue
            same = (abs(cur - was) < 1e-6) if t.startswith("f") else (cur == was)
            if (mode == "unchanged") == same:
                keep.append(a); values.append(cur)
        else:
            want = float(mode) if t.startswith("f") else int(mode, 0)
            hit = abs(cur - want) < 1e-4 if t.startswith("f") else cur == want
            if hit:
                keep.append(a); values.append(cur)
    save_state({"type": t, "candidates": keep, "values": {str(a): v for a, v in zip(keep, values)}})
    print(f"  {len(st['candidates'])} -> {len(keep)} candidate(s)")
    for a, v in list(zip(keep, values))[:20]:
        print(f"    0x{a:08X} = {v}")
    k32.CloseHandle(h)


def cmd_watch(args):
    addr, t = int(args[0], 0), args[1]
    code, size = FMT[t]
    interval = float(args[2]) if len(args) > 2 else 0.2
    _, h = open_target()
    print(f"watching 0x{addr:08X} as {t} -- Ctrl+C to stop")
    last = object()
    try:
        while True:
            data = read(h, addr, size)
            if data and len(data) == size:
                cur = struct.unpack(code, data)[0]
                if cur != last:
                    print(f"  {time.strftime('%H:%M:%S')}  {cur}")
                    last = cur
            time.sleep(interval)
    except KeyboardInterrupt:
        pass
    k32.CloseHandle(h)


def cmd_find(args):
    """Byte-pattern search with ?? wildcards, e.g. 8B0D????????85C9."""
    pat = args[0].replace(" ", "")
    if len(pat) % 2:
        raise SystemExit("pattern must be whole bytes")
    parts = [pat[i:i + 2] for i in range(0, len(pat), 2)]
    mask = [p != "??" for p in parts]
    byts = [int(p, 16) if m else 0 for p, m in zip(parts, mask)]
    _, h = open_target()
    n = 0
    for base, rsize in regions(h):
        data = read(h, base, rsize)
        if not data:
            continue
        for i in range(len(data) - len(byts)):
            if all((not m) or data[i + j] == b for j, (b, m) in enumerate(zip(byts, mask))):
                print(f"    0x{base + i:08X}")
                n += 1
                if n >= 100:
                    print("    (stopping at 100 hits)")
                    k32.CloseHandle(h)
                    return
    print(f"  {n} match(es)")
    k32.CloseHandle(h)


COMMANDS = {"modules": cmd_modules, "read": cmd_read, "scan": cmd_scan,
            "narrow": cmd_narrow, "watch": cmd_watch, "find": cmd_find}

if __name__ == "__main__":
    if len(sys.argv) < 2 or sys.argv[1] not in COMMANDS:
        print(__doc__)
        raise SystemExit(1)
    COMMANDS[sys.argv[1]](sys.argv[2:])
