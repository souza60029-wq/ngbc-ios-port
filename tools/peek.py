#!/usr/bin/env python3
"""Espia os arquivos extraidos: cabecalhos, entropia e dados do XEX.
Uso: python tools/peek.py <pasta_extraida>
"""
import math
import os
import re
import sys
from collections import Counter

ALVOS = [
    "data/ps/ps_x.grc", "data/vs/vs_x.grc",
    "data/pl1/0000.BIN", "data/pl1/0028.BIN",
    "data/fix.bin", "data/pal/pal.bin", "data/pal_tbl/pal_tbl.bin",
    "data/menu/CSET_H.DAT", "data/menu/command.bin", "data/fight/fight.bin",
    "data/bg_bin/stg_a1.bin", "data/chall_00.lz", "data/fight/gm_new01.lz",
    "data/demox/OPENING.bin", "data/vstable.bin", "data/cut_data.bin",
    "media/resource.xpr", "media/xui.xzp",
    "media/sounds/InMemoryWaveBank.xwb", "media/sounds/StreamingWaveBank.xwb",
    "media/sounds/NBC_360.xgs", "media/sounds/NBC_SoundBank.xsb",
]


def entropia(b):
    if not b:
        return 0.0
    c = Counter(b)
    n = len(b)
    return -sum(v / n * math.log2(v / n) for v in c.values())


def hexdump(b, n=96):
    out = []
    for i in range(0, min(n, len(b)), 16):
        ch = b[i:i + 16]
        hx = " ".join("%02x" % x for x in ch)
        asc = "".join(chr(x) if 32 <= x < 127 else "." for x in ch)
        out.append("   %04x  %-47s  %s" % (i, hx, asc))
    return "\n".join(out)


def peek(root, rel):
    p = os.path.join(root, rel)
    print("\n--- %s" % rel)
    if not os.path.exists(p):
        print("   (nao existe)")
        return
    size = os.path.getsize(p)
    with open(p, "rb") as f:
        b = f.read(65536)
    print("   tamanho %d | entropia(64KB) %.2f bits/byte" % (size, entropia(b)))
    print(hexdump(b))
    strs = re.findall(rb"[\x20-\x7e]{6,}", b[:16384])[:6]
    if strs:
        print("   strings:", [s.decode()[:40] for s in strs])


def u32(b, o):
    return int.from_bytes(b[o:o + 4], "big")


def xex(root):
    p = os.path.join(root, "default.xex")
    print("\n=== default.xex ===")
    with open(p, "rb") as f:
        h = f.read(0x4000)
    sec = u32(h, 0x10)
    print("security info em 0x%X | image size (sec+4) = %d (0x%X) | load addr (sec+0x110) = 0x%08X" % (
        sec, u32(h, sec + 4), u32(h, sec + 4), u32(h, sec + 0x110)))
    cnt = u32(h, 0x14)
    for i in range(cnt):
        key, val = u32(h, 0x18 + i * 8), u32(h, 0x1C + i * 8)
        if key == 0x3FF:
            print("FileFormatInfo @0x%X: %s" % (val, h[val:val + 32].hex(" ")))
            enc = int.from_bytes(h[val + 4:val + 6], "big")
            comp = int.from_bytes(h[val + 6:val + 8], "big")
            print("   info_size=%d  encryption=%d (0 nenhuma/1 normal)  compression=%d (0 nenhuma/1 basic/2 LZX/3 delta)" % (
                u32(h, val), enc, comp))
        if key == 0x40006:
            print("ExecutionInfo: media_id=%08X version=%08X base=%08X title_id=%08X platform=%d type=%d disc=%d/%d" % (
                u32(h, val), u32(h, val + 4), u32(h, val + 8), u32(h, val + 12),
                h[val + 16], h[val + 17], h[val + 18], h[val + 19]))
    with open(p, "rb") as f:
        f.seek(u32(h, 8))
        pay = f.read(65536)
    print("entropia do payload (64KB apos PE offset): %.2f bits/byte" % entropia(pay))


def main():
    root = sys.argv[1]
    xex(root)
    for a in ALVOS:
        peek(root, a)


if __name__ == "__main__":
    main()