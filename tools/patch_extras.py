#!/usr/bin/env python3
"""Segundo patch no XenonRecomp:
 1) troca o 'alias' (nao existe em Mach-O/iOS) por um wrapper fraco;
 2) adiciona a ferramenta XenonDump (despeja a imagem do XEX + manifesto).
Uso: python tools/patch_extras.py <pasta_XenonRecomp>   (idempotente)
"""
import os
import sys

DUMP_MAIN = r'''// XenonDump: despeja a imagem descriptografada/descomprimida do XEX e um manifesto (secoes, imports).
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <image.h>
#include <xex.h>

static std::string limpa(const std::string& s)
{
    std::string r;
    for (size_t i = 0; i < s.size() && i < 8; i++)
    {
        if (s[i] >= 32 && s[i] < 127)
            r += s[i];
        else
            break;
    }
    return r;
}

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "uso: XenonDump default.xex prefixo_saida\n");
        return 1;
    }

    std::ifstream f(argv[1], std::ios::binary);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (data.empty())
    {
        std::fprintf(stderr, "erro: nao consegui ler %s\n", argv[1]);
        return 2;
    }

    Image img = Xex2LoadImage(data.data(), data.size());
    if (!img.data || img.size == 0)
    {
        std::fprintf(stderr, "erro: Xex2LoadImage falhou\n");
        return 3;
    }

    const std::string prefix = argv[2];
    {
        std::ofstream o(prefix + "_image.bin", std::ios::binary);
        o.write(reinterpret_cast<const char*>(img.data.get()), img.size);
    }

    FILE* m = std::fopen((prefix + "_manifest.txt").c_str(), "w");
    std::fprintf(m, "base 0x%zX\n", img.base);
    std::fprintf(m, "size 0x%X\n", img.size);
    std::fprintf(m, "entry 0x%zX\n", img.entry_point);
    for (const auto& s : img.sections)
        std::fprintf(m, "section %s va=0x%zX size=0x%X flags=%d\n", limpa(s.name).c_str(), s.base, s.size, (int)s.flags);
    size_t n = 0;
    for (const auto& sym : img.symbols)
    {
        std::fprintf(m, "symbol %s 0x%zX %zu %d\n", sym.name.c_str(), sym.address, sym.size, (int)sym.type);
        n++;
    }
    std::fclose(m);
    std::printf("imagem: base=0x%zX size=0x%X entry=0x%zX secoes=%zu simbolos=%zu\n",
        img.base, img.size, img.entry_point, img.sections.size(), n);
    return 0;
}
'''

root = sys.argv[1]

# ---- 1) alias -> wrapper fraco
path = os.path.join(root, "XenonRecomp", "recompiler.cpp")
src = open(path, encoding="utf-8").read()
OLD = r'''println("__attribute__((alias(\"__imp__{}\"))) PPC_WEAK_FUNC({});", name, name);'''
NEW = (r'''println("PPC_FUNC_IMPL(__imp__{});", name);''' + "\n    " +
       r'''println("PPC_WEAK_FUNC({}) {{ __imp__{}(ctx, base); }}", name, name);''')
if NEW in src:
    print("alias: ja corrigido")
elif src.count(OLD) == 1:
    open(path, "w", encoding="utf-8").write(src.replace(OLD, NEW))
    print("alias: trocado por wrapper fraco")
else:
    raise SystemExit("ERRO: linha do alias nao encontrada (%d ocorrencias)" % src.count(OLD))

# ---- 2) XenonDump
d = os.path.join(root, "XenonDump")
os.makedirs(d, exist_ok=True)
open(os.path.join(d, "main.cpp"), "w", encoding="utf-8").write(DUMP_MAIN)
open(os.path.join(d, "CMakeLists.txt"), "w", encoding="utf-8").write(
    'project("XenonDump")\n\nadd_executable(XenonDump main.cpp)\n'
    'target_compile_features(XenonDump PRIVATE cxx_std_20)\n'
    'target_link_libraries(XenonDump PRIVATE XenonUtils)\n')
cm = os.path.join(root, "CMakeLists.txt")
txt = open(cm, encoding="utf-8").read()
if "add_subdirectory(XenonDump)" not in txt:
    open(cm, "a", encoding="utf-8").write("\nadd_subdirectory(XenonDump)\n")
    print("XenonDump: adicionado ao CMake")
else:
    print("XenonDump: ja presente")