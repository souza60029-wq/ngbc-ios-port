#!/usr/bin/env python3
"""Acha, no C++ gerado pelo XenonRecomp, as funcoes auxiliares de salvar/restaurar registradores
e imprime as linhas prontas para o TOML.
Uso: python tools/find_helpers.py <pasta_ppc_out>
"""
import glob
import os
import re
import sys

FUNC = re.compile(r"PPC_FUNC_IMPL\(__imp__sub_([0-9A-Fa-f]+)\)")

ALVOS = {
    "savegprlr_14_address": lambda c: c[0] == "std r14,-152(r1)",
    "restgprlr_14_address": lambda c: c[0] == "ld r14,-152(r1)",
    "savefpr_14_address": lambda c: c[0] == "stfd f14,-144(r12)",
    "restfpr_14_address": lambda c: c[0] == "lfd f14,-144(r12)",
    "savevmx_14_address": lambda c: c[0] == "li r11,-288" and len(c) > 1 and c[1].startswith("stvx ") and "v14," in c[1],
    "restvmx_14_address": lambda c: c[0] == "li r11,-288" and len(c) > 1 and c[1].startswith("lvx ") and "v14," in c[1],
    "savevmx_64_address": lambda c: c[0] == "li r11,-1024" and len(c) > 1 and c[1].startswith("stvx128 ") and "v64," in c[1],
    "restvmx_64_address": lambda c: c[0] == "li r11,-1024" and len(c) > 1 and c[1].startswith("lvx128 ") and "v64," in c[1],
}


def main():
    pasta = sys.argv[1]
    achados = {k: [] for k in ALVOS}
    for path in sorted(glob.glob(os.path.join(pasta, "ppc_recomp.*.cpp"))):
        addr = None
        coments = []
        with open(path, encoding="utf-8", errors="ignore") as f:
            for line in f:
                m = FUNC.search(line)
                if m:
                    addr, coments = m.group(1).upper(), []
                    continue
                if addr is not None:
                    st = line.strip()
                    if st.startswith("// "):
                        coments.append(st[3:])
                        if len(coments) <= 2:
                            for k, fn in ALVOS.items():
                                if ("vmx" in k) == (len(coments) == 2) and fn(coments):
                                    achados[k].append(addr)
                        if len(coments) >= 2:
                            addr = None
    faltou = []
    for k in ALVOS:
        if achados[k]:
            extra = "  # outros candidatos: " + ", ".join(achados[k][1:]) if len(achados[k]) > 1 else ""
            print("%s = 0x%s%s" % (k, achados[k][0], extra))
        else:
            faltou.append(k)
            print("# %s: NAO ENCONTRADO" % k)
    if faltou:
        sys.stderr.write("faltaram: %s\n" % ", ".join(faltou))


if __name__ == "__main__":
    main()