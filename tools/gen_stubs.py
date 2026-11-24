#!/usr/bin/env python3
"""Gera stubs fracos para todas as importacoes. Uso: gen_stubs.py imports.txt saida.cpp"""
import sys
nomes = [l.strip() for l in open(sys.argv[1]) if l.strip().startswith("__imp__")]
out = ['#include "ppc_recomp_shared.h"', 'void trace_import(const char*, PPCContext&);', '']
for n in nomes:
    short = n[len("__imp__"):]
    out.append('__attribute__((weak)) PPC_FUNC(%s) { trace_import("%s", ctx); ctx.r3.u64 = 0; }' % (n, short))
open(sys.argv[2], "w").write("\n".join(out) + "\n")
print("stubs gerados:", len(nomes))