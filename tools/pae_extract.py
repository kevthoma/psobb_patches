#!/usr/bin/env python3
"""
pae_extract.py - decode PSOBB `.pae` opening/attract movies.

A `.pae` (e.g. data/openning_e.pae) is NOT a video. It is:

    [0x20-byte header][PRS-compressed body]

    header:
        0x00 u32  magic/version   (0x00010001)
        0x04 u32  decompressed size
        0x08 u32  0
        0x0c u32  0
        0x10 u32  offset of a section in the decompressed blob
        0x14 u32  (same value seen in openning_e)
        0x18 u32  size of a following section
        0x1c u32  offset of the XVM texture archive in the decompressed blob
        0x20 ...  PRS stream begins here

The decompressed blob is a ~200 KB timeline/script section (frame order,
positions, pans, fades, timing) followed by an XVM archive:

    XVMH  ... directory ...
    XVRT  (repeated) - one Xbox XVR still texture per frame

    XVRT entry:
        0x00 "XVRT"
        0x04 u32  data size (from 0x08 to end of block)
        0x0c u32  pixel format   (6 = DXT1/S3TC for almost all frames)
        0x14 u16  width
        0x16 u16  height
        0x40 ...  pixel data (DXT1 = w*h/2 bytes)

openning_e.pae decodes to 379 stills: 257x(256x256) + 121x(128x128) + 1x(32x32),
DXT1. That is the whole "movie" - the client just pans/fades over these 2D stills,
which is why it looks barely animated compared to the GameCube CG FMV.

PRS is Sega's LZ used across PSO (same codec as newserv's prs.cc / Puyo Tools).

Subcommands:
    info   <file.pae>                  header + frame/dimension/format histogram
    dec    <file.pae> [out.dec]        write the raw decompressed blob
    frames <file.pae> <outdir>         decode every frame to PNG
    sheet  <file.pae> <out.png> [cols] contact sheet of all frames  (needs Pillow)

Single-frame PNG writing is dependency-free (zlib + struct). The contact sheet
uses Pillow if available.
"""
import struct
import sys
import os
import zlib
import collections


# ---------------------------------------------------------------- PRS
def prs_decompress(data, start=0, max_out=128 * 1024 * 1024):
    out = bytearray()
    i = start
    n = len(data)
    ctrl = 0
    bits = 0

    def bit():
        nonlocal ctrl, bits, i
        if bits == 0:
            ctrl = data[i]
            i += 1
            bits = 8
        b = ctrl & 1
        ctrl >>= 1
        bits -= 1
        return b

    while i < n and len(out) < max_out:
        if bit():                                   # literal byte
            out.append(data[i])
            i += 1
        elif bit():                                 # long copy
            lo = data[i]
            hi = data[i + 1]
            i += 2
            if lo == 0 and hi == 0:
                break                               # end-of-stream marker
            offset = ((hi << 8) | lo) >> 3
            size = lo & 7
            if size == 0:
                size = data[i] + 1
                i += 1
            else:
                size += 2
            pos = len(out) - 0x2000 + offset
            for _ in range(size):
                out.append(out[pos])
                pos += 1
        else:                                       # short copy
            size = ((bit() << 1) | bit()) + 2
            offset = data[i] - 0x100
            i += 1
            pos = len(out) + offset
            for _ in range(size):
                out.append(out[pos])
                pos += 1
    return bytes(out)


# ---------------------------------------------------------------- .pae parse
def load_pae(path):
    """Return (header_dict, decompressed_blob)."""
    d = open(path, "rb").read()
    hdr = {
        "magic": struct.unpack_from("<I", d, 0)[0],
        "decomp_size": struct.unpack_from("<I", d, 4)[0],
        "sectionA_off": struct.unpack_from("<I", d, 0x10)[0],
        "xvm_off_field": struct.unpack_from("<I", d, 0x1c)[0],
        "file_size": len(d),
    }
    blob = prs_decompress(d, 0x20)
    return hdr, blob


def xvr_entries(blob):
    """List of dicts for each XVRT: off, datasize, fmt, w, h."""
    offs = []
    st = 0
    while True:
        j = blob.find(b"XVRT", st)
        if j < 0:
            break
        offs.append(j)
        st = j + 4
    out = []
    for o in offs:
        out.append({
            "off": o,
            "datasize": struct.unpack_from("<I", blob, o + 4)[0],
            "fmt": struct.unpack_from("<I", blob, o + 0x0c)[0],
            "w": struct.unpack_from("<H", blob, o + 0x14)[0],
            "h": struct.unpack_from("<H", blob, o + 0x16)[0],
        })
    return out


# ---------------------------------------------------------------- DXT1
def decode_dxt1(data, w, h):
    """DXT1/S3TC -> RGB bytes (no alpha; punch-through treated as black)."""
    out = bytearray(w * h * 3)
    p = 0
    for by in range(h // 4):
        for bx in range(w // 4):
            c0, c1 = struct.unpack_from("<HH", data, p)
            bits = struct.unpack_from("<I", data, p + 4)[0]
            p += 8

            def rgb(c):
                r = (c >> 11) & 0x1f
                g = (c >> 5) & 0x3f
                b = c & 0x1f
                return [r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2]

            c = [rgb(c0), rgb(c1)]
            if c0 > c1:
                c.append([(2 * c[0][k] + c[1][k]) // 3 for k in range(3)])
                c.append([(c[0][k] + 2 * c[1][k]) // 3 for k in range(3)])
            else:
                c.append([(c[0][k] + c[1][k]) // 2 for k in range(3)])
                c.append([0, 0, 0])
            for py in range(4):
                for px in range(4):
                    idx = (bits >> (2 * (4 * py + px))) & 3
                    X = bx * 4 + px
                    Y = by * 4 + py
                    o3 = (Y * w + X) * 3
                    out[o3:o3 + 3] = bytes(c[idx])
    return bytes(out)


def write_png(path, w, h, rgb):
    def chunk(t, data):
        c = t + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xffffffff)
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw += rgb[y * w * 3:(y + 1) * w * 3]
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    open(path, "wb").write(png)


def frame_rgb(blob, e):
    w, h = e["w"], e["h"]
    if w == 0 or h == 0 or w % 4 or h % 4:
        return None
    need = w * h // 2                       # DXT1
    data = blob[e["off"] + 0x40: e["off"] + 0x40 + need]
    if len(data) < need:
        return None
    return decode_dxt1(data, w, h)


# ---------------------------------------------------------------- commands
def cmd_info(path):
    hdr, blob = load_pae(path)
    ents = xvr_entries(blob)
    dims = collections.Counter((e["w"], e["h"]) for e in ents)
    fmts = collections.Counter(e["fmt"] for e in ents)
    xvm = blob.find(b"XVMH")
    print(f"file             {path}")
    print(f"file size        {hdr['file_size']:,}")
    print(f"magic            0x{hdr['magic']:08x}")
    print(f"decomp size      {hdr['decomp_size']:,}  (actual {len(blob):,})")
    print(f"XVM archive at   0x{xvm:x} ({xvm:,}) -> {xvm:,}-byte timeline/script section precedes it")
    print(f"frames (XVRT)    {len(ents)}")
    print(f"dimensions       {dict(dims)}")
    print(f"pixel formats    {dict(fmts)}   (6 = DXT1)")


def cmd_dec(path, out=None):
    hdr, blob = load_pae(path)
    out = out or path + ".dec"
    open(out, "wb").write(blob)
    print(f"wrote {out}  ({len(blob):,} bytes)")


def cmd_frames(path, outdir):
    hdr, blob = load_pae(path)
    ents = xvr_entries(blob)
    os.makedirs(outdir, exist_ok=True)
    ok = 0
    for i, e in enumerate(ents):
        rgb = frame_rgb(blob, e)
        if rgb is None:
            continue
        write_png(os.path.join(outdir, f"f{i:03d}_{e['w']}x{e['h']}.png"), e["w"], e["h"], rgb)
        ok += 1
    print(f"wrote {ok}/{len(ents)} frames to {outdir}/")


def cmd_sheet(path, out, cols=20):
    try:
        from PIL import Image
    except ImportError:
        sys.exit("sheet needs Pillow (pip install pillow); use `frames` for dependency-free PNGs")
    hdr, blob = load_pae(path)
    ents = xvr_entries(blob)
    cell = 90
    rows = (len(ents) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * cell, rows * cell), (20, 20, 24))
    ok = 0
    for i, e in enumerate(ents):
        rgb = frame_rgb(blob, e)
        if rgb is None:
            continue
        im = Image.frombytes("RGB", (e["w"], e["h"]), rgb).resize((cell - 2, cell - 2))
        sheet.paste(im, ((i % cols) * cell + 1, (i // cols) * cell + 1))
        ok += 1
    sheet.save(out)
    print(f"pasted {ok}/{len(ents)} frames -> {out} ({sheet.size[0]}x{sheet.size[1]})")


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        print("usage: pae_extract.py {info|dec|frames|sheet} <file.pae> [args]")
        return 1
    cmd, path = argv[1], argv[2]
    if cmd == "info":
        cmd_info(path)
    elif cmd == "dec":
        cmd_dec(path, argv[3] if len(argv) > 3 else None)
    elif cmd == "frames":
        cmd_frames(path, argv[3] if len(argv) > 3 else "frames")
    elif cmd == "sheet":
        cmd_sheet(path, argv[3] if len(argv) > 3 else "contact_sheet.png",
                  int(argv[4]) if len(argv) > 4 else 20)
    else:
        print(f"unknown command: {cmd}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
