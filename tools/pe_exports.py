#!/usr/bin/env python3
"""
pe_exports.py - read a PE's export and import tables without dumpbin.

Written for the proxy `dsound.dll`, where the ordinals are load-bearing: psobb.exe imports
DirectSoundCreate BY ORDINAL (#1), so a proxy whose export table does not reproduce the system
DLL's ordinal layout leaves that import unresolved and the game will not start at all. The `check`
mode turns that into a build-time failure instead of a launch-time one.

Also useful for answering "what does the client actually import, and where is the thunk" without
loading a disassembler -- which is how the entry in notes/psobb-client-map.md was corrected: the
address recorded there for the dsound import was the "dsound.dll" NAME STRING in .idata, not the
IAT slot the game calls through.

Usage:
    pe_exports.py exports <pefile>
        List every export as `@ordinal name`.

    pe_exports.py imports <pefile> <dllname>
        List what <pefile> imports from <dllname>, with the VA of each IAT slot -- i.e. the
        pointer the loader patches and the game calls through, which IS the address to hook if
        anyone ever wants an IAT hook rather than a proxy DLL.

    pe_exports.py check <built-pefile> <reference-pefile>
        Verify <built-pefile> exports everything <reference-pefile> does, at the SAME ordinals.
        Exits non-zero and prints the differences if not. Extra exports are allowed.

Both 32- and 64-bit PEs are handled, though everything in this repo is 32-bit (PSOBB is x86).
"""
import struct
import sys


def read(path):
    with open(path, "rb") as f:
        return f.read()


class PE:
    def __init__(self, data):
        self.d = data
        if data[:2] != b"MZ":
            raise SystemExit("not a PE (no MZ header)")
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        if data[pe:pe + 4] != b"PE\0\0":
            raise SystemExit("not a PE (no PE00 signature)")
        self.nsections, = struct.unpack_from("<H", data, pe + 6)
        opt = pe + 24
        self.magic, = struct.unpack_from("<H", data, opt)
        self.pe32plus = self.magic == 0x20B
        self.image_base = struct.unpack_from("<Q" if self.pe32plus else "<I", data,
                                             opt + (24 if self.pe32plus else 28))[0]
        nrva_off = opt + (108 if self.pe32plus else 92)
        self.nrva, = struct.unpack_from("<I", data, nrva_off)
        base = nrva_off + 4
        self.dirs = [struct.unpack_from("<II", data, base + i * 8) for i in range(self.nrva)]
        sec = base + self.nrva * 8
        self.sections = []
        for i in range(self.nsections):
            name, vsize, vaddr, rsize, raddr = struct.unpack_from("<8sIIII", data, sec + i * 40)
            self.sections.append(
                (name.rstrip(b"\0").decode(errors="replace"), vaddr, vsize, raddr, rsize))

    def off(self, rva):
        """RVA -> file offset."""
        for _name, vaddr, vsize, raddr, _rsize in self.sections:
            if vaddr <= rva < vaddr + max(vsize, _rsize):
                return raddr + (rva - vaddr)
        raise SystemExit(f"RVA {rva:#x} falls in no section")

    def cstr(self, rva):
        o = self.off(rva)
        return self.d[o:self.d.index(b"\0", o)].decode(errors="replace")


def exports(pe):
    """-> (internal dll name, {ordinal: name})."""
    rva, _size = pe.dirs[0]
    if not rva:
        raise SystemExit("no export directory")
    o = pe.off(rva)
    (_flags, _stamp, _maj, _min, name_rva, ordinal_base, naddr, nnames,
     addr_rva, names_rva, ords_rva) = struct.unpack_from("<IIHHIIIIIII", pe.d, o)

    named = {}
    for i in range(nnames):
        nrva, = struct.unpack_from("<I", pe.d, pe.off(names_rva) + i * 4)
        idx, = struct.unpack_from("<H", pe.d, pe.off(ords_rva) + i * 2)
        named[idx + ordinal_base] = pe.cstr(nrva)

    out = {}
    for i in range(naddr):
        func, = struct.unpack_from("<I", pe.d, pe.off(addr_rva) + i * 4)
        if func:  # a zero entry is a hole in the ordinal space, not an export
            out[i + ordinal_base] = named.get(i + ordinal_base, "<by ordinal only>")
    return pe.cstr(name_rva), out


def imports(pe, want):
    """-> [(imported name or 'ordinal #N', VA of its IAT slot)]."""
    rva, _size = pe.dirs[1]
    if not rva:
        raise SystemExit("no import directory")
    o = pe.off(rva)
    found = []
    while True:
        oft, _stamp, _fwd, name_rva, ft = struct.unpack_from("<IIIII", pe.d, o)
        if not (oft or name_rva or ft):
            break
        if pe.cstr(name_rva).lower() == want.lower():
            t = pe.off(oft or ft)
            slot_rva = ft  # the IAT entry the loader overwrites
            while True:
                v, = struct.unpack_from("<I", pe.d, t)
                if not v:
                    break
                label = f"ordinal #{v & 0xFFFF}" if v & 0x80000000 else pe.cstr(v + 2)
                found.append((label, pe.image_base + slot_rva))
                t += 4
                slot_rva += 4
        o += 20
    return found


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    mode, path = sys.argv[1], sys.argv[2]

    if mode == "exports":
        dllname, table = exports(PE(read(path)))
        print(f"{path}\ninternal name: {dllname}   exports: {len(table)}")
        for ordinal in sorted(table):
            print(f"  @{ordinal:<4} {table[ordinal]}")

    elif mode == "imports":
        if len(sys.argv) < 4:
            raise SystemExit(__doc__)
        pe = PE(read(path))
        found = imports(pe, sys.argv[3])
        print(f"{path}\nimports from {sys.argv[3]}: {len(found)}")
        for name, va in found:
            print(f"  {name:<28} IAT slot VA {va:#010x}")
        if not found:
            print("  (none)")

    elif mode == "check":
        if len(sys.argv) < 4:
            raise SystemExit(__doc__)
        _n, built = exports(PE(read(path)))
        _n, ref = exports(PE(read(sys.argv[3])))
        problems = []
        for ordinal, name in sorted(ref.items()):
            if ordinal not in built:
                problems.append(f"MISSING  @{ordinal} {name}")
            elif built[ordinal] != name:
                problems.append(f"MISMATCH @{ordinal} is {built[ordinal]!r}, reference has {name!r}")
        extra = sorted(set(built) - set(ref))
        print(f"built:     {path} ({len(built)} exports)")
        print(f"reference: {sys.argv[3]} ({len(ref)} exports)")
        for e in extra:
            print(f"  note: extra export @{e} {built[e]} (allowed)")
        if problems:
            print("\nEXPORT TABLE DOES NOT MATCH THE REFERENCE:")
            for p in problems:
                print("  " + p)
            sys.exit(1)
        print(f"\nOK: all {len(ref)} reference exports present at the same ordinals.")

    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
