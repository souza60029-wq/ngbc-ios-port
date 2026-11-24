#!/usr/bin/env python3
"""Acha funcoes que o XenonRecomp nao descobriu (folhas sem .pdata chamadas so por ponteiro)
e imprime a linha 'functions = [...]' para o TOML.
Uso: find_funcs.py imagem.bin manifest.txt pasta_ppc_out [extra.txt] > funcs.toml   (estatisticas em stderr)
"""
import bisect
import glob
import os
import re
import struct
import sys

img = open(sys.argv[1], "rb").read()
manifest = open(sys.argv[2]).read()
ppc_out = sys.argv[3]
extra = sys.argv[4] if len(sys.argv) > 4 else None
BASE = 0x82000000
MAX_CAND = 6000
MAX_SIZE = 0x2000


def u32(va):
    o = va - BASE
    return struct.unpack(">I", img[o:o + 4])[0] if 0 <= o <= len(img) - 4 else 0


secs = [(n, int(va, 16), int(sz, 16), int(fl))
        for n, va, sz, fl in re.findall(r"section (\S*) va=0x([0-9A-Fa-f]+) size=0x([0-9A-Fa-f]+) flags=(\d+)", manifest)]
code = [(va, va + sz) for n, va, sz, fl in secs if fl & 2]
data = [(va, sz) for n, va, sz, fl in secs if not (fl & 2) and n not in (".pdata", ".reloc", ".idata")]
pdata = [(va, sz) for n, va, sz, fl in secs if n == ".pdata"]


def in_code(a):
    return any(lo <= a < hi for lo, hi in code)


# funcoes conhecidas: .pdata (com tamanho) e o que o recompilador ja gerou
ranges = []
starts = set()
for va, sz in pdata:
    for i in range(sz // 8):
        b = u32(va + i * 8)
        d = u32(va + i * 8 + 4)
        ln = ((d >> 8) & 0x3FFFFF) * 4
        if b:
            ranges.append((b, b + ln))
            starts.add(b)
ranges.sort()
rstarts = [r[0] for r in ranges]
for f in glob.glob(os.path.join(ppc_out, "ppc_func_mapping.cpp")):
    for m in re.finditer(r"\{\s*0x([0-9A-Fa-f]+)\s*,", open(f).read()):
        starts.add(int(m.group(1), 16))


def covered(a):
    i = bisect.bisect_right(rstarts, a) - 1
    return i >= 0 and ranges[i][0] <= a < ranges[i][1]


cands = {}   # endereco -> origem


def consider(a, why):
    if a % 4 or not in_code(a) or a in starts or covered(a) or u32(a) == 0:
        return
    cands.setdefault(a, why)


# 1) ponteiros em secoes de dados
for va, sz in data:
    for off in range(0, sz - 3, 4):
        consider(u32(va + off), "dados")

# 2) lis + addi/ori em codigo (callbacks, rotinas de thread)
for lo, hi in code:
    for a in range(lo, hi - 4, 4):
        w = u32(a)
        if (w >> 26) == 15 and ((w >> 16) & 0x1F) == 0:        # lis rD, imm
            rd = (w >> 21) & 0x1F
            imm = w & 0xFFFF
            if not (0x820F <= imm <= 0x826B):
                continue
            for k in range(1, 7):
                x = u32(a + 4 * k)
                if (x >> 26) == 14 and ((x >> 16) & 0x1F) == rd:        # addi rX, rD, lo
                    lo16 = x & 0xFFFF
                    consider((imm << 16) + (lo16 - 0x10000 if lo16 & 0x8000 else lo16), "lis+addi")
                    break
                if (x >> 26) == 24 and ((x >> 21) & 0x1F) == rd:        # ori rA, rD, lo
                    consider((imm << 16) | (x & 0xFFFF), "lis+ori")
                    break

# 3) lista manual
if extra and os.path.exists(extra):
    for line in open(extra):
        line = line.split("#")[0].strip()
        if line:
            a = int(line, 16)
            if a % 4 == 0 and in_code(a) and a not in starts:
                cands[a] = "manual"

addrs = sorted(cands)[:MAX_CAND]
bounds = sorted(set(addrs) | starts | {r[0] for r in ranges})
out = []
for a in addrs:
    i = bisect.bisect_right(bounds, a)
    nxt = bounds[i] if i < len(bounds) else a + 0x40
    size = max(4, min(nxt - a, MAX_SIZE))
    out.append((a, size))

por = {}
for a in addrs:
    por[cands[a]] = por.get(cands[a], 0) + 1
sys.stderr.write("funcoes conhecidas: %d | candidatas novas: %d %s\n" % (len(starts), len(out), por))
if out:
    print("functions = [")
    for a, s in out:
        print("  { address = 0x%08X, size = 0x%X }," % (a, s))
    print("]")