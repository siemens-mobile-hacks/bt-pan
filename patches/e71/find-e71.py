#!/usr/bin/env python3
"""Find the E71 sw45 counterparts of the EL71 addresses hardcoded in bt-pan/src.

Functions: bytes from the EL71 entry (BL masked) are searched in flashes/E71v45.bin; the pattern
grows until it is unique. RAM addresses and flash pool words: each EL71 PC-relative LDR that loads
the value is matched in E71 the same way and the E71 literal of that LDR is read; sites must agree.
"""
import re
import struct
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EL = (ROOT / "flashes/EL71v45.bin").read_bytes()
E7 = (ROOT / "flashes/E71v45.bin").read_bytes()
B = 0xA0000000
SRC = ROOT / "bt-pan/src"


def masked(buf, off, n, thumb):
    """regex for buf[off:off+n] with branch offsets wildcarded"""
    parts = []
    i = 0
    while i < n:
        if thumb:
            h = struct.unpack_from("<H", buf, off + i)[0]
            h2 = struct.unpack_from("<H", buf, off + i + 2)[0]
            if h & 0xF800 == 0xF000 and h2 & 0xE800 == 0xE800:
                parts.append(b"...."); i += 4; continue
            parts.append(re.escape(buf[off + i:off + i + 2])); i += 2
        else:
            w = struct.unpack_from("<I", buf, off + i)[0]
            if (w >> 25) & 7 == 5:
                parts.append(b"..." + re.escape(bytes([w >> 24])))
            else:
                parts.append(re.escape(buf[off + i:off + i + 4]))
            i += 4
    return re.compile(b"".join(parts), re.S)


def find_code(off, thumb, before=0):
    """E71 offset matching EL71 code at off (pattern may start `before` bytes earlier)"""
    for n in (16, 24, 32, 48, 64, 96, 128, 192, 256):
        rx = masked(EL, off - before, n + before, thumb)
        hits = [m.start() + before for m in rx.finditer(E7)]
        if len(hits) == 1:
            return hits[0], n
        if not hits:
            return None, n
    return None, -1


def ldr_sites(value):
    """EL71 (ldr_offset, thumb, pool_offset) for PC-relative loads of `value`"""
    word = struct.pack("<I", value)
    sites = []
    p = EL.find(word)
    while p >= 0:
        if p % 4 == 0:
            for back in range(4, 1024 + 4, 2):          # Thumb ldr rX,[pc,#imm8*4]
                a = p - back
                if a < 0:
                    break
                h = struct.unpack_from("<H", EL, a)[0]
                if h & 0xF800 == 0x4800 and ((a + 4) & ~3) + (h & 0xFF) * 4 == p:
                    sites.append((a, True, p))
            for back in range(8, 4096 + 8, 4):          # ARM ldr rX,[pc,#imm12]
                a = p - back
                if a < 0:
                    break
                w = struct.unpack_from("<I", EL, a)[0]
                if w & 0x0F7F0000 == 0x051F0000 and w & 0x00800000 and a + 8 + (w & 0xFFF) == p:
                    sites.append((a, False, p))
        p = EL.find(word, p + 1)
    return sites


def e71_literal(a, thumb):
    if thumb:
        h = struct.unpack_from("<H", E7, a)[0]
        if h & 0xF800 != 0x4800:
            return None
        pool = ((a + 4) & ~3) + (h & 0xFF) * 4
    else:
        w = struct.unpack_from("<I", E7, a)[0]
        if w & 0x0F7F0000 != 0x051F0000:
            return None
        pool = a + 8 + (w & 0xFFF) * (1 if w & 0x00800000 else -1)
    return pool, struct.unpack_from("<I", E7, pool)[0]


def find_value(value, want_pool=False):
    """E71 value (or pool location) via all EL71 code loading it; also tries base-offset loads"""
    votes = Counter()
    for delta in (0, 4, 8, 0x10, 0x20, 0x40, 0x80, 0x100):
        for a, thumb, pool in ldr_sites(value - delta):
            for before in (0, 8, 16):
                e, n = find_code(a, thumb, before)
                if e is not None:
                    r = e71_literal(e, thumb)
                    if r:
                        votes[(r[0] + 0 if want_pool else r[1] + delta)] += 1
                    break
        if votes:
            break
    return votes


def macros():
    rx = re.compile(r"#define\s+(\w+)\s+.*?(FW_THUMB|FW_ARM)\(0x([0-9a-fA-F]{8})u?\)")
    for f in sorted(SRC.glob("*.[ch]")):
        for line in f.read_text(encoding="utf-8", errors="replace").splitlines():
            m = rx.search(line)
            if m:
                yield m.group(1), int(m.group(3), 16), m.group(2) == "FW_THUMB"


out = []
for name, addr, thumb in macros():
    e, n = find_code(addr - B, thumb)
    out.append((name, addr, None if e is None else e + B, f"code {n}" if e is not None else "NOT FOUND"))

for name, addr in (("PDP_DRIVERS", 0xA8E9E9CC), ("BTLP_Connections", 0xA8DA5500),
                   ("L2CAP_ChannelState", 0xA8DA2080)):
    v = find_value(addr)
    best = v.most_common(1)
    out.append((name, addr, best[0][0] if best else None, f"ldr votes {dict((hex(k), c) for k, c in v.items())}"))

# JSApi_ConnectionsList: a flash pool word holding the list pointer
pool = 0xA0A0D908
ptr = struct.unpack_from("<I", EL, pool - B)[0]
v = Counter()
for a, thumb, p in ldr_sites(ptr):
    if p == pool - B:
        e, n = find_code(a, thumb, 8)
        if e is not None:
            r = e71_literal(e, thumb)
            if r:
                v[r[0] + B] += 1
best = v.most_common(1)
out.append(("JSApi_ConnectionsList(pool)", pool, best[0][0] if best else None, f"pool votes {len(v)} ptr EL {ptr:08X}"))

for name, el, e7, how in out:
    d = "" if e7 is None else f"{(e7 - el) & 0xFFFFFFFF:08X}"
    print(f"{name}\t{el:08X}\t{'--------' if e7 is None else f'{e7:08X}'}\t{d}\t{how}")
