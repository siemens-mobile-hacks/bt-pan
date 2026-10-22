#!/usr/bin/env python3
"""Port bt-pan-mtu.vkp (EL71 sw45) to E71 sw45 -> bt-pan-mtu_E71v45.vkp.

Every block of the EL71 patch is located in flashes/E71v45.bin:
  - by a unique byte context (+-8..64 bytes) around the block, or
  - by the shift of a neighbouring block of the same function (then +-48 bytes are compared;
    only BL/B offsets may differ), or
  - heap descriptors by their name strings (BT_RX_ACL_HEAP / BT_TX_ACL_HEAP).
The two hooks and their stubs are rebuilt: branch offsets are re-encoded for the E71 sites,
the Thumb stub's literal and the ARM stub's call/return targets are read from E71 code.
Old bytes must match the E71 dump, the caves must be erased (FF).
"""
import re
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
EL = (ROOT / "flashes" / "EL71v45.bin").read_bytes()
E7 = (ROOT / "flashes" / "E71v45.bin").read_bytes()
SRC = HERE / "bt-pan-mtu.vkp"
OUT = HERE / "bt-pan-mtu_E71v45.vkp"
BASE = 0xA0000000


def die(msg):
    sys.exit("port-mtu-e71: " + msg)


def unique(hay, pat):
    i = hay.find(pat)
    return i if i >= 0 and hay.find(pat, i + 1) < 0 else None


def by_context(a, n):
    for k in (8, 16, 24, 32, 48, 64):
        i = unique(E7, EL[a - k:a + n + k])
        if i is not None:
            return i + k
    return None


def similar(a, b, k=48):
    return sum(x == y for x, y in zip(EL[a - k:a + k], E7[b - k:b + k]))


def heap_entry(a):
    """size field of a heap descriptor: {u32 size, u32 count, char *name}"""
    name_ptr = struct.unpack_from("<I", EL, a + 8)[0] - BASE
    name = EL[name_ptr:EL.index(b"\0", name_ptr) + 1]
    hits = []
    i = E7.find(name)
    while i >= 0:
        p = struct.pack("<I", BASE + i)
        j = E7.find(p)
        while j >= 0:
            if E7[j - 8:j] == EL[a:a + 8]:
                hits.append(j - 8)
            j = E7.find(p, j + 1)
        i = E7.find(name, i + 1)
    return hits[0] if len(hits) == 1 else None


def thumb_bl(src, dst):
    off = dst - (src + 4)
    hi = 0xF000 | ((off >> 12) & 0x7FF)
    lo = 0xF800 | ((off >> 1) & 0x7FF)
    return struct.pack("<HH", hi, lo)


def arm_branch(src, dst, cond_op):
    """cond_op: top byte, e.g. 0xEA = B, 0xEB = BL, 0x1A = BNE"""
    off = ((dst - (src + 8)) >> 2) & 0xFFFFFF
    return struct.pack("<I", (cond_op << 24) | off)


def arm_target(at):
    w = struct.unpack_from("<I", E7, at)[0]
    off = w & 0xFFFFFF
    if off & 0x800000:
        off -= 1 << 24
    return at + 8 + off * 4


# ---- parse the EL71 patch, keeping comments
lines = SRC.read_text(encoding="utf-8").splitlines()
blocks = []
for n, line in enumerate(lines):
    m = re.match(r"0x([0-9A-Fa-f]+):\s*([0-9A-Fa-f]+)\s+([0-9A-Fa-f]+)\s*$", line)
    if m:
        blocks.append((n, int(m.group(1), 16), bytes.fromhex(m.group(2)), bytes.fromhex(m.group(3))))

HOOK_THUMB, HOOK_ARM = 0x9227F2, 0xA0ED74
CAVE_THUMB, CAVE_ARM = 0xC5CC1C, 0xC5CC2C
HEAPS = (0x12E2040, 0x12E1FF4)

# ---- locate every block
where = {}
for _, a, old, new in blocks:
    if a in HEAPS:
        where[a] = heap_entry(a)
    else:
        where[a] = by_context(a, len(old))
# the rest: shift of a located neighbour (closest address), checked on +-48 bytes
for _, a, old, new in blocks:
    if where[a] is not None:
        continue
    best = None
    for b, e in where.items():
        if e is not None and abs(b - a) < 0x2000 and (best is None or abs(b - a) < abs(best[0] - a)):
            best = (b, e)
    if best:
        cand = a + best[1] - best[0]
        if E7[cand:cand + len(old)] == EL[a:a + len(old)] and similar(a, cand) >= 72:
            where[a] = cand
for _, a, old, new in blocks:
    if where[a] is None:
        die(f"block {a:08X} not found in E71")

# ---- new bytes, rebuilding hooks and stubs
e_hook_t, e_hook_a = where[HOOK_THUMB], where[HOOK_ARM]
e_cave_t, e_cave_a = where[CAVE_THUMB], where[CAVE_ARM]

# Thumb stub literal = the value the replaced "ldr r0,[pc,#imm]" loaded on E71
ins = struct.unpack_from("<H", E7, e_hook_t + 2)[0]
if ins & 0xF800 != 0x4800:
    die(f"E71 {e_hook_t + 2:08X}: expected ldr r0,[pc,#]")
lit = ((BASE + e_hook_t + 2 + 4) & ~3) + (ins & 0xFF) * 4
literal = E7[lit - BASE:lit - BASE + 4]

# ARM stub: return points and the ring-write call, from the E71 copy of the stock sequence
# (site: moveq r0,r4 / moveq r1,sp / moveq r2,#2 / strheq r6,[sp] / bleq write / mov r2,r6 ...)
site = BASE + e_hook_a
write_fn = arm_target(e_hook_a + 0x10) + BASE
ret_cont = site + 0x14   # mov r2,r6 (after the conditional write)
ret_cmp = site + 0x28    # cmp r4,r6 (short-write path)
if struct.unpack_from("<I", E7, e_hook_a + 0x28)[0] != 0xE1540006:
    die("E71 ARM site layout differs (cmp r4,r6 not at +0x28)")

new_bytes = {}
for _, a, old, new in blocks:
    new_bytes[a] = new
new_bytes[HOOK_THUMB] = thumb_bl(BASE + e_hook_t, BASE + e_cave_t)
stub_t = bytearray(new_bytes[CAVE_THUMB])
stub_t[12:16] = literal
new_bytes[CAVE_THUMB] = bytes(stub_t)
new_bytes[HOOK_ARM] = arm_branch(site, BASE + e_cave_a, 0xEA)
stub_a = bytearray(new_bytes[CAVE_ARM])
ca = BASE + e_cave_a
stub_a[0x04:0x08] = arm_branch(ca + 0x04, ret_cont, 0x1A)   # bne -> continue
stub_a[0x2C:0x30] = arm_branch(ca + 0x2C, write_fn, 0xEB)   # bl ring write
stub_a[0x30:0x34] = arm_branch(ca + 0x30, ret_cont, 0xEA)   # b continue
stub_a[0x38:0x3C] = arm_branch(ca + 0x38, ret_cmp, 0xEA)    # b short-write path
new_bytes[CAVE_ARM] = bytes(stub_a)

# ---- checks: old data present in E71
for _, a, old, new in blocks:
    e = where[a]
    if E7[e:e + len(old)] != old:
        die(f"{a:08X} -> {e:08X}: old data differs in E71 ({E7[e:e + len(old)].hex()} != {old.hex()})")

# ---- write
out = [";E71v45", "; Ported from bt-pan-mtu.vkp (EL71sw45) by port-mtu-e71.py.",
       "; Every address re-located in flashes/E71v45.bin; hooks and stubs rebuilt for E71.",
       "; UNTESTED on a real E71. Comments below are the EL71 ones; addresses in them are EL71.", ";"]
by_line = {n: (a, old) for n, a, old, new in blocks}
for n, line in enumerate(lines):
    if n in by_line:
        a, old = by_line[n]
        out.append(f"0x{where[a]:08X}: {old.hex().upper()} {new_bytes[a].hex().upper()}"
                   f"   ; EL71 {a:08X}")
    else:
        out.append(line)
OUT.write_text("\n".join(out) + "\n", encoding="utf-8")
for _, a, old, new in blocks:
    print(f"{a:08X} -> {where[a]:08X}  {new_bytes[a].hex()}")
print(f"literal {literal[::-1].hex()}  write {write_fn:08X}  cont {ret_cont:08X}  cmp {ret_cmp:08X}")
print("written", OUT)
