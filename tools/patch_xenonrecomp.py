#!/usr/bin/env python3
"""Adiciona ao XenonRecomp as instrucoes PowerPC/VMX que o jogo usa e ele nao implementa.
Uso: python tools/patch_xenonrecomp.py <pasta_XenonRecomp>
Idempotente: pula o que ja existir.
"""
import os
import re
import sys

MARK = "// ===== extras NGBC (patch_xenonrecomp.py) ====="

OPS = "insn.operands"


def ld_d(dt, load):
    return f'''        println("\\t{{}} = {{}} + {{}}.u32;", ea(), int32_t({OPS}[1]), r({OPS}[2]));
        println("\\t{{}}.{dt} = {load};", r({OPS}[0]), ea());
        println("\\t{{}}.u32 = {{}};", r({OPS}[2]), ea());
        break;
'''


def ld_x(dt, load):
    return f'''        println("\\t{{}} = {{}}.u32 + {{}}.u32;", ea(), r({OPS}[1]), r({OPS}[2]));
        println("\\t{{}}.{dt} = {load};", r({OPS}[0]), ea());
        println("\\t{{}}.u32 = {{}};", r({OPS}[1]), ea());
        break;
'''


def st_d(macro, member):
    return f'''        println("\\t{{}} = {{}} + {{}}.u32;", ea(), int32_t({OPS}[1]), r({OPS}[2]));
        println("\\t{macro}({{}}, {{}}.{member});", ea(), r({OPS}[0]));
        println("\\t{{}}.u32 = {{}};", r({OPS}[2]), ea());
        break;
'''


def st_x(macro, member):
    return f'''        println("\\t{{}} = {{}}.u32 + {{}}.u32;", ea(), r({OPS}[1]), r({OPS}[2]));
        println("\\t{macro}({{}}, {{}}.{member});", ea(), r({OPS}[0]));
        println("\\t{{}}.u32 = {{}};", r({OPS}[1]), ea());
        break;
'''


def simde3(fn, ld, st=None):
    st = st or ld
    return f'''        println("\\tsimde_mm_store_si128((simde__m128i*){{}}.{st}, {fn}(simde_mm_load_si128((simde__m128i*){{}}.{ld}), simde_mm_load_si128((simde__m128i*){{}}.{ld})));", v({OPS}[0]), v({OPS}[1]), v({OPS}[2]));
        break;
'''


def simde_cmp(fn, ld):
    return f'''        println("\\tsimde_mm_store_si128((simde__m128i*){{}}.u8, {fn}(simde_mm_load_si128((simde__m128i*){{}}.{ld}), simde_mm_load_si128((simde__m128i*){{}}.{ld})));", v({OPS}[0]), v({OPS}[1]), v({OPS}[2]));
        if (strchr(insn.opcode->name, '.'))
            println("\\t{{}}.setFromMask(simde_mm_load_si128((simde__m128i*){{}}.u8), 0xFFFF);", cr(6), v({OPS}[0]));
        break;
'''


def simde_pack(fn, ld):
    # vetor invertido por inteiro: os argumentos trocam de ordem (igual ao VPKSHUS existente)
    return f'''        println("\\tsimde_mm_store_si128((simde__m128i*){{}}.u8, {fn}(simde_mm_load_si128((simde__m128i*){{}}.{ld}), simde_mm_load_si128((simde__m128i*){{}}.{ld})));", v({OPS}[0]), v({OPS}[2]), v({OPS}[1]));
        break;
'''


def lane_loop(n, stmt, extra=""):
    return f'''        for (size_t i = 0; i < {n}; i++)
            println("{stmt}", {extra});
        break;
'''


CASES = []


def add(names, body):
    CASES.append((names, body))


# ---------- loads/stores com atualizacao ----------
add(["LHZU"], ld_d("u64", "PPC_LOAD_U16({})"))
add(["LHAU"], ld_d("s64", "int16_t(PPC_LOAD_U16({}))"))
add(["STHU"], st_d("PPC_STORE_U16", "u16"))
add(["LBZUX"], ld_x("u64", "PPC_LOAD_U8({})"))
add(["LHZUX"], ld_x("u64", "PPC_LOAD_U16({})"))
add(["LHAUX"], ld_x("s64", "int16_t(PPC_LOAD_U16({}))"))
add(["LWZUX"], ld_x("u64", "PPC_LOAD_U32({})"))
add(["LDUX"], ld_x("u64", "PPC_LOAD_U64({})"))
add(["STBUX"], st_x("PPC_STORE_U8", "u8"))
add(["STHUX"], st_x("PPC_STORE_U16", "u16"))
add(["STDUX"], st_x("PPC_STORE_U64", "u64"))

add(["LFSU"], f'''        printSetFlushMode(false);
        println("\\t{{}} = {{}} + {{}}.u32;", ea(), int32_t({OPS}[1]), r({OPS}[2]));
        println("\\t{{}}.u32 = PPC_LOAD_U32({{}});", temp(), ea());
        println("\\t{{}}.f64 = double({{}}.f32);", f({OPS}[0]), temp());
        println("\\t{{}}.u32 = {{}};", r({OPS}[2]), ea());
        break;
''')
add(["STFSU"], f'''        printSetFlushMode(false);
        println("\\t{{}} = {{}} + {{}}.u32;", ea(), int32_t({OPS}[1]), r({OPS}[2]));
        println("\\t{{}}.f32 = float({{}}.f64);", temp(), f({OPS}[0]));
        println("\\tPPC_STORE_U32({{}}, {{}}.u32);", ea(), temp());
        println("\\t{{}}.u32 = {{}};", r({OPS}[2]), ea());
        break;
''')
add(["LFDU"], f'''        printSetFlushMode(false);
        println("\\t{{}} = {{}} + {{}}.u32;", ea(), int32_t({OPS}[1]), r({OPS}[2]));
        println("\\t{{}}.u64 = PPC_LOAD_U64({{}});", f({OPS}[0]), ea());
        println("\\t{{}}.u32 = {{}};", r({OPS}[2]), ea());
        break;
''')
add(["STFDU"], f'''        printSetFlushMode(false);
        println("\\t{{}} = {{}} + {{}}.u32;", ea(), int32_t({OPS}[1]), r({OPS}[2]));
        println("\\tPPC_STORE_U64({{}}, {{}}.u64);", ea(), f({OPS}[0]));
        println("\\t{{}}.u32 = {{}};", r({OPS}[2]), ea());
        break;
''')
add(["LFSUX"], f'''        printSetFlushMode(false);
        println("\\t{{}} = {{}}.u32 + {{}}.u32;", ea(), r({OPS}[1]), r({OPS}[2]));
        println("\\t{{}}.u32 = PPC_LOAD_U32({{}});", temp(), ea());
        println("\\t{{}}.f64 = double({{}}.f32);", f({OPS}[0]), temp());
        println("\\t{{}}.u32 = {{}};", r({OPS}[1]), ea());
        break;
''')
add(["STFSUX"], f'''        printSetFlushMode(false);
        println("\\t{{}} = {{}}.u32 + {{}}.u32;", ea(), r({OPS}[1]), r({OPS}[2]));
        println("\\t{{}}.f32 = float({{}}.f64);", temp(), f({OPS}[0]));
        println("\\tPPC_STORE_U32({{}}, {{}}.u32);", ea(), temp());
        println("\\t{{}}.u32 = {{}};", r({OPS}[1]), ea());
        break;
''')
add(["LFDUX"], f'''        printSetFlushMode(false);
        println("\\t{{}} = {{}}.u32 + {{}}.u32;", ea(), r({OPS}[1]), r({OPS}[2]));
        println("\\t{{}}.u64 = PPC_LOAD_U64({{}});", f({OPS}[0]), ea());
        println("\\t{{}}.u32 = {{}};", r({OPS}[1]), ea());
        break;
''')
add(["STFDUX"], f'''        printSetFlushMode(false);
        println("\\t{{}} = {{}}.u32 + {{}}.u32;", ea(), r({OPS}[1]), r({OPS}[2]));
        println("\\tPPC_STORE_U64({{}}, {{}}.u64);", ea(), f({OPS}[0]));
        println("\\t{{}}.u32 = {{}};", r({OPS}[1]), ea());
        break;
''')

add(["LHBRX"], f'''        print("\\t{{}}.u64 = __builtin_bswap16(PPC_LOAD_U16(", r({OPS}[0]));
        if ({OPS}[1] != 0)
            print("{{}}.u32 + ", r({OPS}[1]));
        println("{{}}.u32));", r({OPS}[2]));
        break;
''')

# ---------- inteiros / condicao / desvios ----------
RC = f'''        if (strchr(insn.opcode->name, '.'))
            println("\\t{{}}.compare<int32_t>({{}}.s32, 0, {{}});", cr(0), r({OPS}[0]), xer());
'''
add(["EQV"], f'''        println("\\t{{}}.u64 = ~({{}}.u64 ^ {{}}.u64);", r({OPS}[0]), r({OPS}[1]), r({OPS}[2]));
{RC}        break;
''')
add(["ADDC"], f'''        println("\\t{{}}.ca = ({{}}.u32 + {{}}.u32) < {{}}.u32;", xer(), r({OPS}[1]), r({OPS}[2]), r({OPS}[1]));
        println("\\t{{}}.u64 = {{}}.u64 + {{}}.u64;", r({OPS}[0]), r({OPS}[1]), r({OPS}[2]));
{RC}        break;
''')
add(["SUBFZE"], f'''        println("\\t{{}}.u8 = ({{}}.u32 == 0) & {{}}.ca;", temp(), r({OPS}[1]), xer());
        println("\\t{{}}.u64 = ~{{}}.u64 + {{}}.ca;", r({OPS}[0]), r({OPS}[1]), xer());
        println("\\t{{}}.ca = {{}}.u8;", xer(), temp());
{RC}        break;
''')
add(["ADDME"], f'''        println("\\t{{}}.u8 = ({{}}.u32 != 0) | {{}}.ca;", temp(), r({OPS}[1]), xer());
        println("\\t{{}}.s64 = {{}}.s64 + {{}}.ca - 1;", r({OPS}[0]), r({OPS}[1]), xer());
        println("\\t{{}}.ca = {{}}.u8;", xer(), temp());
{RC}        break;
''')
add(["BDZF"], f'''    {{
        static constexpr std::string_view crFields[] = {{ "lt", "gt", "eq", "so" }};
        println("\\t--{{}}.u64;", ctr());
        println("\\tif ({{}}.u32 == 0 && !{{}}.{{}}) goto loc_{{:X}};", ctr(), cr({OPS}[0] / 4), crFields[{OPS}[0] % 4], {OPS}[1]);
        break;
    }}
''')
add(["BDNZT"], f'''    {{
        static constexpr std::string_view crFields[] = {{ "lt", "gt", "eq", "so" }};
        println("\\t--{{}}.u64;", ctr());
        println("\\tif ({{}}.u32 != 0 && {{}}.{{}}) goto loc_{{:X}};", ctr(), cr({OPS}[0] / 4), crFields[{OPS}[0] % 4], {OPS}[1]);
        break;
    }}
''')
add(["CROR", "CRORC"], f'''    {{
        static constexpr std::string_view crFields[] = {{ "lt", "gt", "eq", "so" }};
        auto crBit = [&](uint32_t b) {{ return fmt::format("{{}}.{{}}", cr(b / 4), crFields[b % 4]); }};
        println("\\t{{}} = {{}} | {{}}{{}};", crBit({OPS}[0]), crBit({OPS}[1]), insn.opcode->id == PPC_INST_CRORC ? "!" : "", crBit({OPS}[2]));
        break;
    }}
''')

# ---------- VMX ----------
add(["VSLH"], lane_loop(8, "\\t{}.u16[{}] = {}.u16[{}] << ({}.u16[{}] & 0xF);",
                        f"v({OPS}[0]), i, v({OPS}[1]), i, v({OPS}[2]), i"))
add(["VSRH"], lane_loop(8, "\\t{}.u16[{}] = {}.u16[{}] >> ({}.u16[{}] & 0xF);",
                        f"v({OPS}[0]), i, v({OPS}[1]), i, v({OPS}[2]), i"))
add(["VSRAH"], lane_loop(8, "\\t{}.s16[{}] = {}.s16[{}] >> ({}.u16[{}] & 0xF);",
                         f"v({OPS}[0]), i, v({OPS}[1]), i, v({OPS}[2]), i"))
add(["VSRAB"], lane_loop(16, "\\t{}.s8[{}] = {}.s8[{}] >> ({}.u8[{}] & 0x7);",
                         f"v({OPS}[0]), i, v({OPS}[1]), i, v({OPS}[2]), i"))
add(["VRLH"], lane_loop(8, "\\t{}.u16[{}] = uint16_t(({}.u16[{}] << ({}.u16[{}] & 0xF)) | ({}.u16[{}] >> ((16 - ({}.u16[{}] & 0xF)) & 0xF)));",
                        f"v({OPS}[0]), i, v({OPS}[1]), i, v({OPS}[2]), i, v({OPS}[1]), i, v({OPS}[2]), i"))
add(["VSPLTISH"], f'''    {{
        int32_t imm = int32_t({OPS}[1]) & 0x1F;
        if (imm & 0x10)
            imm -= 0x20;
        println("\\tsimde_mm_store_si128((simde__m128i*){{}}.u16, simde_mm_set1_epi16(short({{}})));", v({OPS}[0]), imm);
        break;
    }}
''')
add(["VADDSWS"], f'''        for (size_t i = 0; i < 4; i++)
        {{
            println("\\t{{}}.s64 = int64_t({{}}.s32[{{}}]) + int64_t({{}}.s32[{{}}]);", temp(), v({OPS}[1]), i, v({OPS}[2]), i);
            println("\\t{{}}.s32[{{}}] = {{}}.s64 > INT_MAX ? INT_MAX : {{}}.s64 < INT_MIN ? INT_MIN : {{}}.s64;", v({OPS}[0]), i, temp(), temp(), temp());
        }}
        break;
''')
add(["VAVGSW"], f'''        for (size_t i = 0; i < 4; i++)
            println("\\t{{}}.s32[{{}}] = int32_t((int64_t({{}}.s32[{{}}]) + int64_t({{}}.s32[{{}}]) + 1) >> 1);", v({OPS}[0]), i, v({OPS}[1]), i, v({OPS}[2]), i);
        break;
''')
add(["VSUBSHS"], simde3("simde_mm_subs_epi16", "s16"))
add(["VADDSBS"], simde3("simde_mm_adds_epi8", "s8"))
add(["VSUBSBS"], simde3("simde_mm_subs_epi8", "s8"))
add(["VSUBUBM"], simde3("simde_mm_sub_epi8", "u8"))
add(["VMAXSH"], simde3("simde_mm_max_epi16", "s16"))
add(["VMINSH"], simde3("simde_mm_min_epi16", "s16"))
add(["VMINSW"], simde3("simde_mm_min_epi32", "s32"))
add(["VAVGUH"], simde3("simde_mm_avg_epu16", "u16"))
add(["VCMPGTSH"], simde_cmp("simde_mm_cmpgt_epi16", "s16"))
add(["VCMPGTSW"], simde_cmp("simde_mm_cmpgt_epi32", "s32"))
add(["VCMPEQUH"], simde_cmp("simde_mm_cmpeq_epi16", "u16"))
add(["VPKSHSS", "VPKSHSS128"], simde_pack("simde_mm_packs_epi16", "s16"))
add(["VPKSWSS", "VPKSWSS128"], simde_pack("simde_mm_packs_epi32", "s32"))
add(["VPKSWUS", "VPKSWUS128"], simde_pack("simde_mm_packus_epi32", "s32"))
add(["VPKUHUS", "VPKUHUS128"], f'''        for (size_t i = 0; i < 8; i++)
            println("\\t{{}}.u8[{{}}] = {{}}.u16[{{}}] > 0xFF ? 0xFF : {{}}.u16[{{}}];", vTemp(), i, v({OPS}[2]), i, v({OPS}[2]), i);
        for (size_t i = 0; i < 8; i++)
            println("\\t{{}}.u8[{{}}] = {{}}.u16[{{}}] > 0xFF ? 0xFF : {{}}.u16[{{}}];", vTemp(), i + 8, v({OPS}[1]), i, v({OPS}[1]), i);
        println("\\t{{}} = {{}};", v({OPS}[0]), vTemp());
        break;
''')
add(["VCFPUXWS128", "VCFPUXWS"], f'''        printSetFlushMode(true);
        for (size_t i = 0; i < 4; i++)
        {{
            println("\\t{{}}.f64 = double({{}}.f32[{{}}]) * {{}}.0;", temp(), v({OPS}[1]), i, 1ull << ({OPS}[2] & 0x1F));
            println("\\t{{}}.u32[{{}}] = {{}}.f64 >= 4294967295.0 ? 0xFFFFFFFFu : ({{}}.f64 > 0.0 ? uint32_t({{}}.f64) : 0u);", v({OPS}[0]), i, temp(), temp(), temp());
        }}
        break;
''')
add(["VSLO", "VSLO128"], f'''        println("\\t{{}}.u32 = ({{}}.u8[0] >> 3) & 0xF;", temp(), v({OPS}[2]));
        for (size_t i = 0; i < 16; i++)
            println("\\t{{}}.u8[{{}}] = {{}}.u32 <= {{}} ? {{}}.u8[{{}} - {{}}.u32] : 0;", vTemp(), i, temp(), i, v({OPS}[1]), i, temp());
        println("\\t{{}} = {{}};", v({OPS}[0]), vTemp());
        break;
''')
add(["VSEL128"], f'''        println("\\tsimde_mm_store_si128((simde__m128i*){{}}.u8, simde_mm_or_si128(simde_mm_andnot_si128(simde_mm_load_si128((simde__m128i*){{}}.u8), simde_mm_load_si128((simde__m128i*){{}}.u8)), simde_mm_and_si128(simde_mm_load_si128((simde__m128i*){{}}.u8), simde_mm_load_si128((simde__m128i*){{}}.u8))));", v({OPS}[0]), v({OPS}[0]), v({OPS}[1]), v({OPS}[0]), v({OPS}[2]));
        break;
''')
add(["LVEHX", "LVXL", "LVXL128"], f'''        print("\\tsimde_mm_store_si128((simde__m128i*){{}}.u8, simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)(base + ((", v({OPS}[0]));
        if ({OPS}[1] != 0)
            print("{{}}.u32 + ", r({OPS}[1]));
        println("{{}}.u32) & ~0xF))), simde_mm_load_si128((simde__m128i*)VectorMaskL)));", r({OPS}[2]));
        break;
''')
add(["STVLXL128", "STVLXL"], f'''        print("\\t{{}} = ", ea());
        if ({OPS}[1] != 0)
            print("{{}}.u32 + ", r({OPS}[1]));
        println("{{}}.u32;", r({OPS}[2]));
        println("\\tfor (size_t i = 0; i < (16 - ({{}} & 0xF)); i++)", ea());
        println("\\t\\tPPC_STORE_U8({{}} + i, {{}}.u8[15 - i]);", ea(), v({OPS}[0]));
        break;
''')
add(["STVEBX"], f'''        print("\\t{{}} = ", ea());
        if ({OPS}[1] != 0)
            print("{{}}.u32 + ", r({OPS}[1]));
        println("{{}}.u32;", r({OPS}[2]));
        println("\\tPPC_STORE_U8({{}}, {{}}.u8[15 - ({{}} & 0xF)]);", ea(), v({OPS}[0]), ea());
        break;
''')


def main():
    root = sys.argv[1]
    path = os.path.join(root, "XenonRecomp", "recompiler.cpp")
    src = open(path, encoding="utf-8").read()
    if MARK in src:
        print("patch ja aplicado, nada a fazer")
        return

    # tokens PPC_INST_* que existem no resto do codigo (desassembler)
    known = set()
    for dp, _, fs in os.walk(root):
        for fn in fs:
            if fn.endswith((".h", ".hpp", ".c", ".cpp", ".inc")) and fn != "recompiler.cpp":
                try:
                    txt = open(os.path.join(dp, fn), encoding="utf-8", errors="ignore").read()
                except OSError:
                    continue
                known.update(re.findall(r"PPC_INST_[A-Z0-9_]+", txt))
    existing = set(re.findall(r"case PPC_INST_([A-Z0-9_]+):", src))

    out = ["    " + MARK + "\n"]
    added, skipped = [], []
    for names, body in CASES:
        labels = []
        for n in names:
            if n in existing:
                skipped.append(n + " (ja existe)")
            elif "PPC_INST_" + n not in known:
                skipped.append(n + " (opcode desconhecido)")
            else:
                labels.append(n)
        if not labels:
            continue
        for n in labels:
            out.append("    case PPC_INST_%s:\n" % n)
        out.append(body + "\n")
        added += labels

    anchor = "    default:\n        return false;\n    }\n"
    if src.count(anchor) != 1:
        raise SystemExit("ERRO: ancora do switch nao encontrada (%d ocorrencias)" % src.count(anchor))
    src = src.replace(anchor, "".join(out) + anchor)

    # RC do vcmpgtuh (existia sem atualizar CR6)
    m = re.search(r"(    case PPC_INST_VCMPGTUH:\n        println\([^\n]*\n)(        break;)", src)
    if m:
        rc = ('        if (strchr(insn.opcode->name, \'.\'))\n'
              '            println("\\t{}.setFromMask(simde_mm_load_si128((simde__m128i*){}.u8), 0xFFFF);", cr(6), v(insn.operands[0]));\n')
        src = src[:m.end(1)] + rc + src[m.start(2):]
        print("RC do VCMPGTUH corrigido")
    else:
        print("AVISO: VCMPGTUH nao encontrado para corrigir o RC")

    open(path, "w", encoding="utf-8").write(src)
    print("instrucoes adicionadas (%d): %s" % (len(added), " ".join(added)))
    if skipped:
        print("puladas: %s" % ", ".join(skipped))


if __name__ == "__main__":
    main()