#!/usr/bin/env python3
"""Extrator de pacotes STFS (XBLA/GOD) + inspecao do default.xex.
Uso: python tools/stfs_extract.py <pacote> <pasta_saida> [--list]
"""
import os
import sys

BLOCK = 0x1000


def u32(b, o):
    return int.from_bytes(b[o:o + 4], "big")


def u24le(b, o):
    return int.from_bytes(b[o:o + 3], "little")


class Stfs:
    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")
        self.size = os.path.getsize(path)
        h = self.f.read(0x2000)
        self.h = h
        self.magic = h[:4]
        if self.magic not in (b"LIVE", b"PIRS", b"CON "):
            raise SystemExit("ERRO: nao e um pacote STFS. Magic = %r (hex %s)" % (self.magic, self.magic.hex()))
        self.content_type = u32(h, 0x344)
        self.title_id = u32(h, 0x360)
        self.header_size = u32(h, 0x340)
        vd = 0x379
        self.vd_size = h[vd]
        self.block_sep = h[vd + 2]
        self.ft_count = int.from_bytes(h[vd + 3:vd + 5], "little")
        self.ft_block = u24le(h, vd + 5)
        self.total_alloc = u32(h, vd + 0x1C)
        self.total_unalloc = u32(h, vd + 0x20)
        self.first_hash = (self.header_size + 0xFFF) & 0xF000
        self.shift = None
        try:
            self.title_name = h[0x411:0x411 + 0x80].decode("utf-16-be", "ignore").split("\x00")[0]
        except Exception:
            self.title_name = "?"

    def describe(self):
        print("=== CABECALHO STFS ===")
        print("magic            :", self.magic.decode("ascii", "replace"))
        print("tamanho arquivo  : %d bytes (%.1f MB)" % (self.size, self.size / 1048576))
        print("content type     : 0x%08X" % self.content_type)
        print("title id         : %08X" % self.title_id)
        print("titulo           :", self.title_name)
        print("header size      : 0x%X  -> primeira hash table em 0x%X" % (self.header_size, self.first_hash))
        print("vol.desc size    : 0x%X  block_sep=0x%02X" % (self.vd_size, self.block_sep))
        print("file table       : bloco %d, %d bloco(s)" % (self.ft_block, self.ft_count))
        print("blocos alocados  : %d  (nao alocados: %d)" % (self.total_alloc, self.total_unalloc))

    # --- mapeamento de blocos ---
    def backing(self, n):
        s = self.shift
        b = (((n + 0xAA) // 0xAA) << s) + n
        if n < 0xAA:
            return b
        b += ((n + 0x70E4) // 0x70E4) << s
        if n < 0x70E4:
            return b
        return b + (1 << s)

    def addr(self, n):
        return self.first_hash + (self.backing(n) << 12)

    def l0_backing(self, n):
        if n < 0xAA:
            return 0
        s = self.shift
        t = 1 << s
        num = (n // 0xAA) * (0xAA + t)
        num += ((n // 0x70E4) + 1) << s
        if n // 0x70E4 == 0:
            return num
        return num + t

    def next_block(self, n):
        a = self.first_hash + (self.l0_backing(n) << 12) + (n % 0xAA) * 0x18
        self.f.seek(a)
        e = self.f.read(0x18)
        if len(e) < 0x18:
            return 0xFFFFFF
        return int.from_bytes(e[21:24], "big")

    def read_block(self, n):
        self.f.seek(self.addr(n))
        return self.f.read(BLOCK)

    # --- file table ---
    def parse_entries(self):
        entries = []
        for i in range(self.ft_count):
            blk = self.read_block(self.ft_block + i)
            if len(blk) < BLOCK:
                break
            for k in range(64):
                e = blk[k * 0x40:(k + 1) * 0x40]
                flags = e[0x28]
                nlen = flags & 0x3F
                if nlen == 0:
                    return entries
                name = e[:nlen].decode("latin-1")
                entries.append({
                    "name": name,
                    "consecutive": bool(flags & 0x40),
                    "dir": bool(flags & 0x80),
                    "blocks": u24le(e, 0x29),
                    "start": u24le(e, 0x2F),
                    "parent": int.from_bytes(e[0x32:0x34], "big"),
                    "size": u32(e, 0x34),
                })
        return entries

    def valid_table(self, entries):
        if not entries:
            return False
        for e in entries[:8]:
            if not e["name"].isprintable() or e["start"] > max(self.total_alloc, 1) + 64:
                return False
        return True

    def open_table(self):
        order = [0, 1] if (self.block_sep & 1) else [1, 0]
        for s in order:
            self.shift = s
            ent = self.parse_entries()
            if self.valid_table(ent):
                print("shift da hash table = %d (tabelas de %d bloco(s))" % (s, 1 << s))
                return ent
        raise SystemExit("ERRO: nao consegui ler a file table (nenhum shift valido).")

    def path_of(self, entries, i):
        parts = []
        seen = 0
        while i != 0xFFFF and seen < 64:
            parts.append(entries[i]["name"])
            i = entries[i]["parent"]
            seen += 1
        return "/".join(reversed(parts))

    def blocks_of(self, e):
        n = e["blocks"]
        if e["consecutive"]:
            return range(e["start"], e["start"] + n)
        chain, b = [], e["start"]
        while len(chain) < n and b != 0xFFFFFF:
            chain.append(b)
            b = self.next_block(b)
        return chain

    def extract(self, e, dest):
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        remaining = e["size"]
        with open(dest, "wb") as out:
            run_start, run_len, prev = None, 0, None
            runs = []
            for b in self.blocks_of(e):
                a = self.addr(b)
                if prev is not None and a == prev + BLOCK:
                    run_len += 1
                else:
                    if run_start is not None:
                        runs.append((run_start, run_len))
                    run_start, run_len = a, 1
                prev = a
            if run_start is not None:
                runs.append((run_start, run_len))
            for a, cnt in runs:
                self.f.seek(a)
                want = min(cnt * BLOCK, remaining)
                while want > 0:
                    chunk = self.f.read(min(want, 8 * 1024 * 1024))
                    if not chunk:
                        break
                    out.write(chunk)
                    want -= len(chunk)
                    remaining -= len(chunk)
        return e["size"] - remaining


def xex_info(path):
    print("\n=== INSPECAO %s ===" % path)
    with open(path, "rb") as f:
        h = f.read(0x1000)
    print("magic:", h[:4], h[:4].hex())
    if h[:4] not in (b"XEX2", b"XEX1"):
        print("nao parece XEX (veja o magic acima)")
        return
    print("module flags : 0x%08X" % u32(h, 4))
    print("PE offset    : 0x%X" % u32(h, 8))
    print("security off : 0x%X" % u32(h, 0x10))
    cnt = u32(h, 0x14)
    print("opt headers  : %d" % cnt)
    names = {0x10100: "entry point", 0x10201: "image base", 0x3FF: "file format info",
             0x40006: "execution info", 0x18002: "default stack size?", 0x10001: "import libs?"}
    for i in range(min(cnt, 64)):
        key = u32(h, 0x18 + i * 8)
        val = u32(h, 0x1C + i * 8)
        label = names.get(key, "")
        print("  key 0x%08X val 0x%08X %s" % (key, val, label))
        if key == 0x3FF and val + 8 <= len(h):
            enc = int.from_bytes(h[val + 4:val + 6], "big")
            comp = int.from_bytes(h[val + 6:val + 8], "big")
            print("    -> encryption=%d (0 nenhuma,1 normal)  compression=%d (0 nenhuma,1 basic,2 LZX,3 delta)" % (enc, comp))


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    pkg, outdir = sys.argv[1], sys.argv[2]
    only_list = "--list" in sys.argv
    s = Stfs(pkg)
    s.describe()
    ent = s.open_table()
    print("\n=== ARQUIVOS (%d entradas) ===" % len(ent))
    total = 0
    for i, e in enumerate(ent):
        p = s.path_of(ent, i)
        tag = "DIR " if e["dir"] else ("cons" if e["consecutive"] else "enc ")
        print("%s %12d  %s" % (tag, e["size"], p))
        if not e["dir"]:
            total += e["size"]
    print("soma dos arquivos: %.1f MB" % (total / 1048576))
    if only_list:
        return
    xexes = []
    for i, e in enumerate(ent):
        if e["dir"]:
            continue
        p = s.path_of(ent, i)
        dest = os.path.join(outdir, p)
        got = s.extract(e, dest)
        if got != e["size"]:
            print("AVISO: %s incompleto (%d de %d)" % (p, got, e["size"]))
        if p.lower().endswith(".xex"):
            xexes.append(dest)
    print("\nextraido em:", outdir)
    for x in xexes:
        xex_info(x)
    if not xexes:
        print("\nNenhum .xex encontrado no pacote.")


if __name__ == "__main__":
    main()