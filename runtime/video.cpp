// video.cpp - GPU do Xbox 360 (v2): vsync a 60 Hz, processador de comandos PM4, estado da GPU e gravador de desenhos.
#include "ngbc.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#define KTRACE(n) trace_import(#n, ctx)

static std::atomic<uint32_t> g_cb{0}, g_cb_ctx{0};
static std::atomic<bool> g_vsync_started{false}, g_cp_started{false};
static uint32_t g_ring_phys = 0, g_ring_bytes = 0, g_wb_phys = 0, g_sys_fence = 0;
static std::atomic<uint64_t> g_vsyncs{0}, g_packets{0}, g_interrupts{0}, g_fences{0}, g_swaps{0}, g_draws{0};
static std::mutex g_ops_mu;
static std::map<uint32_t, uint64_t> g_ops;   // histograma de opcodes PM4 (tipo 3)

static const uint32_t GPU_REGS = 0x7FC80000;       // registradores da GPU (mapeados na memoria do convidado)
static const uint32_t REG_CP_RB_WPTR = 0x01C5;     // ponteiro de escrita do anel (em dwords)

static uint32_t phys2virt(uint32_t p) { return p < 0x20000000 ? (p | 0xA0000000) : p; }


// ---------------- estado da GPU (registradores e shaders) ----------------
static uint32_t g_reg[0x10000];            // registradores, indice em dwords
static uint32_t g_prev[0x400];             // copia de 0x2000..0x23FF para mostrar so as mudancas
struct ShaderRef { uint32_t addr = 0, size = 0; };
static ShaderRef g_shader[2];              // 0 = vertex, 1 = pixel
static const char* g_dump_dir = nullptr;   // pasta para despejar dados crus (NGBC_DUMP_DIR); nulo no iPhone
static std::set<uint64_t> g_seen;
static int g_draw_n = 0, g_tex_dumped = 0, g_vb_dumped = 0;

static uint64_t fnv(const uint8_t* p, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

static bool dump_file(const char* name, const uint8_t* data, size_t n)
{
    if (!g_dump_dir || !n) return false;
    char path[512];
    snprintf(path, sizeof path, "%s/%s", g_dump_dir, name);
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    fwrite(data, 1, n, f);
    fclose(f);
    return true;
}

static void guest_copy(uint32_t addr, std::vector<uint8_t>& out, size_t n)
{
    out.resize(n);
    memcpy(out.data(), g_base + addr, n);   // memoria nao mapeada vira pagina zerada sob demanda
}

static void load_shader(uint32_t type, uint32_t addr_virt, uint32_t size_dw, const uint32_t* inline_words)
{
    if (type > 1 || size_dw == 0 || size_dw > 0x4000) return;
    std::vector<uint8_t> code(size_dw * 4);
    if (inline_words) for (uint32_t i = 0; i < size_dw; i++) { uint32_t w = __builtin_bswap32(inline_words[i]); memcpy(&code[i * 4], &w, 4); }
    else guest_copy(addr_virt, code, size_dw * 4);
    uint64_t h = fnv(code.data(), code.size());
    g_shader[type] = { addr_virt, size_dw };
    if (g_seen.insert(h ^ type).second && g_seen.size() < 600)
    {
        char name[96];
        snprintf(name, sizeof name, "shader_%s_%016llx.bin", type ? "ps" : "vs", (unsigned long long)h);
        dump_file(name, code.data(), code.size());
        fprintf(stderr, "[gpu] shader %s novo: %u dwords em 0x%08X (%s)\n", type ? "pixel" : "vertex", size_dw, addr_virt, name);
    }
}

static const uint32_t FETCH_BASE = 0x4800, ALU_BASE = 0x4000;

static void log_draw(uint32_t prim, uint32_t count, uint32_t src, bool idx32, const std::vector<uint32_t>& idx)
{
    int n = g_draw_n++;
    if (n >= 60) return;
    fprintf(stderr, "[gpu] DESENHO #%d prim=%u indices=%u origem=%u idx32=%d | vs=0x%08X/%u ps=0x%08X/%u\n", n, prim, count, src, idx32 ? 1 : 0,
        g_shader[0].addr, g_shader[0].size, g_shader[1].addr, g_shader[1].size);
    if (!idx.empty()) { fprintf(stderr, "[gpu]   indices:"); for (size_t i = 0; i < idx.size() && i < 24; i++) fprintf(stderr, " %u", idx[i]); fprintf(stderr, "\n"); }
    for (int i = 0; i < 4; i++)   // fetch de vertice: 2 dwords cada
    {
        uint32_t d0 = g_reg[FETCH_BASE + i * 2], d1 = g_reg[FETCH_BASE + i * 2 + 1];
        if (!d0 && !d1) continue;
        uint32_t addr = d0 & ~3u, size_dw = (d1 >> 2) & 0xFFFFFF;
        fprintf(stderr, "[gpu]   VF%d: %08X %08X -> endereco 0x%08X, %u dwords\n", i, d0, d1, addr, size_dw);
        uint32_t va = addr < 0x20000000 ? (addr | 0xA0000000) : addr;
        if (addr && size_dw && size_dw <= 0x40000)
        {
            std::vector<uint8_t> vb; guest_copy(va, vb, size_dw * 4);
            uint64_t h = fnv(vb.data(), vb.size());
            if (g_seen.insert(h).second && g_vb_dumped++ < 40)
            {
                char name[96]; snprintf(name, sizeof name, "vb_%03d_vf%d_%016llx.bin", n, i, (unsigned long long)h);
                dump_file(name, vb.data(), vb.size());
            }
            fprintf(stderr, "[gpu]     dados:"); for (int k = 0; k < 8 && k * 4 < (int)vb.size(); k++) { uint32_t w; memcpy(&w, &vb[k * 4], 4); fprintf(stderr, " %08X", __builtin_bswap32(w)); } fprintf(stderr, "\n");
        }
    }
    for (int i = 0; i < 4; i++)   // fetch de textura: 6 dwords cada
    {
        const uint32_t* t = &g_reg[FETCH_BASE + i * 6];
        if (!t[0] && !t[1] && !t[2]) continue;
        uint32_t fmt = t[1] & 0x3F, endian = (t[1] >> 6) & 3, base = (t[1] >> 12) << 12;
        uint32_t w = (t[2] & 0x1FFF) + 1, h = ((t[2] >> 13) & 0x1FFF) + 1, pitch = (t[0] >> 22) & 0x1FF, tiled = t[0] >> 31;
        fprintf(stderr, "[gpu]   TF%d: %08X %08X %08X %08X %08X %08X -> fmt=%u endian=%u base=0x%08X %ux%u pitch=%u tiled=%u\n", i, t[0], t[1], t[2], t[3], t[4], t[5], fmt, endian, base, w, h, pitch, tiled);
        uint32_t va = base < 0x20000000 ? (base | 0xA0000000) : base;
        size_t bytes = (size_t)((w + 31) & ~31u) * ((h + 31) & ~31u) * 4;   // limite superior (RGBA 32 bits)
        if (bytes > 0x200000) bytes = 0x200000;
        if (base && bytes)
        {
            std::vector<uint8_t> tex; guest_copy(va, tex, bytes);
            uint64_t hh = fnv(tex.data(), tex.size() < 4096 ? tex.size() : 4096) ^ base;
            if (g_seen.insert(hh).second && g_tex_dumped++ < 16)
            {
                char name[128]; snprintf(name, sizeof name, "tex_%03d_tf%d_fmt%u_%ux%u_t%u_%08X.bin", n, i, fmt, w, h, tiled, base);
                dump_file(name, tex.data(), tex.size());
            }
        }
    }
    fprintf(stderr, "[gpu]   constantes do vertex shader c0..c3:");
    for (int k = 0; k < 16; k++) { float f; memcpy(&f, &g_reg[ALU_BASE + k], 4); fprintf(stderr, "%s%g", k % 4 ? "," : " |", f); }
    fprintf(stderr, "\n[gpu]   constantes do pixel shader c0..c1:");
    for (int k = 0; k < 8; k++) { float f; memcpy(&f, &g_reg[ALU_BASE + 0x400 + k], 4); fprintf(stderr, "%s%g", k % 4 ? "," : " |", f); }
    fprintf(stderr, "\n");
    int shown = 0;
    for (uint32_t r = 0; r < 0x400 && shown < 40; r++)   // so os registradores que mudaram desde o desenho anterior
        if (g_reg[0x2000 + r] != g_prev[r]) { fprintf(stderr, "[gpu]   reg 0x%04X: %08X -> %08X\n", 0x2000 + r, g_prev[r], g_reg[0x2000 + r]); shown++; }
    memcpy(g_prev, &g_reg[0x2000], sizeof g_prev);
}

// ---------------- processador de comandos ----------------
struct Reader
{
    uint32_t base, size_dw, pos;
    bool ring;
    PPCContext* ctx;
    uint8_t* hostbase;
    uint32_t next()
    {
        uint32_t i = ring ? pos % size_dw : pos;
        pos++;
        return rd32(base + i * 4);
    }
};

static void fire_interrupt(Reader& r, uint32_t source)
{
    uint32_t cb = g_cb.load();
    if (!cb) return;
    PPCContext& ctx = *r.ctx;
    uint8_t* base = r.hostbase;
    uint64_t saved1 = ctx.r1.u64;
    ctx.r3.u64 = source;
    ctx.r4.u64 = g_cb_ctx.load();
    PPC_CALL_INDIRECT_FUNC(cb);
    ctx.r1.u64 = saved1;
    g_interrupts++;
}

static void exec_packet(Reader& r, int depth)
{
    uint32_t h = r.next();
    g_packets++;
    switch (h >> 30)
    {
    case 0:
    {
        uint32_t count = ((h >> 16) & 0x3FFF) + 1, base_index = h & 0x7FFF; bool one = (h >> 15) & 1;
        for (uint32_t i = 0; i < count; i++) { uint32_t val = r.next(); uint32_t ri = (one ? base_index : base_index + i) & 0xFFFF; g_reg[ri] = val; }
        break;
    }
    case 1: r.next(); r.next(); break;
    case 2: break;
    case 3:
    {
        uint32_t op = (h >> 8) & 0x7F, count = ((h >> 16) & 0x3FFF) + 1, start = r.pos;
        { std::lock_guard<std::mutex> l(g_ops_mu); g_ops[op]++; }
        switch (op)
        {
        case 0x3F: case 0x37:   // INDIRECT_BUFFER: executa outro buffer
        {
            uint32_t p = r.next(), len = r.next() & 0xFFFFF;
            if (depth < 4 && len < 0x100000)
            {
                Reader sub{ phys2virt(p & 0x1FFFFFFC), len, 0, false, r.ctx, r.hostbase };
                while (sub.pos < len) exec_packet(sub, depth + 1);
            }
            break;
        }
        case 0x58:   // EVENT_WRITE_SHD: escreve um valor (cerca) na memoria
        {
            uint32_t initiator = r.next(), address = r.next(), value = r.next();
            wr32(phys2virt(address & 0x1FFFFFFC), (initiator >> 31) ? (uint32_t)g_vsyncs.load() : value);
            g_fences++;
            break;
        }
        case 0x3D:   // MEM_WRITE
        {
            uint32_t address = phys2virt(r.next() & 0x1FFFFFFC);
            for (uint32_t i = 1; i < count; i++) wr32(address + (i - 1) * 4, r.next());
            break;
        }
        case 0x54: { r.next(); fire_interrupt(r, 1); break; }   // INTERRUPT: origem 1
        case 0x64:   // XE_SWAP: troca de quadro
        {
            uint64_t sn = g_swaps++;
            if (sn < 6) { fprintf(stderr, "[gpu] troca #%llu:", (unsigned long long)sn); for (uint32_t i = 0; i < count && i < 6; i++) fprintf(stderr, " %08X", r.next()); fprintf(stderr, "\n"); }
            break;
        }
        case 0x2D:   // SET_CONSTANT: escreve registradores (ALU, fetch, bool, loop, regs)
        {
            uint32_t ot = r.next(), index = ot & 0x7FF, type = (ot >> 16) & 0xFF;
            static const uint32_t bases[] = { 0x4000, 0x4800, 0x4900, 0x4908, 0x2000 };
            uint32_t b = type < 5 ? bases[type] : 0x2000;
            for (uint32_t i = 1; i < count; i++) g_reg[(b + index + i - 1) & 0xFFFF] = r.next();
            break;
        }
        case 0x55: case 0x56:   // SET_CONSTANT2 / SET_SHADER_CONSTANTS: indice direto
        {
            uint32_t index = r.next() & 0xFFFF;
            for (uint32_t i = 1; i < count; i++) g_reg[(index + i - 1) & 0xFFFF] = r.next();
            break;
        }
        case 0x2F:   // LOAD_ALU_CONSTANT: copia constantes da memoria
        {
            uint32_t address = phys2virt(r.next() & 0x1FFFFFFC), ot = r.next(), size = r.next() & 0xFFF;
            uint32_t index = ot & 0x7FF, type = (ot >> 16) & 0xFF;
            static const uint32_t bases[] = { 0x4000, 0x4800, 0x4900, 0x4908, 0x2000 };
            uint32_t b = type < 5 ? bases[type] : 0x4000;
            for (uint32_t i = 0; i < size && i < 0x800; i++) g_reg[(b + index + i) & 0xFFFF] = rd32(address + i * 4);
            break;
        }
        case 0x27:   // IM_LOAD: carrega shader da memoria
        {
            uint32_t at = r.next(), ss = r.next();
            load_shader(at & 3, phys2virt(at & 0x1FFFFFFC), ss & 0xFFFF, nullptr);
            break;
        }
        case 0x2B:   // IM_LOAD_IMMEDIATE: shader dentro do pacote
        {
            uint32_t d0 = r.next(), d1 = r.next(), sz = d1 & 0xFFFF;
            std::vector<uint32_t> words;
            for (uint32_t i = 0; i < sz && i + 2 < count; i++) words.push_back(__builtin_bswap32(r.next()));
            if (words.size() == sz) load_shader(d0 & 3, 0, sz, words.data());
            break;
        }
        case 0x22:   // DRAW_INDX
        {
            r.next();
            uint32_t init = r.next(), cnt = init >> 16, prim = init & 0x3F, src = (init >> 6) & 3; bool i32 = (init >> 11) & 1;
            std::vector<uint32_t> idx;
            if (src == 0 && count >= 4)
            {
                uint32_t ibase = phys2virt(r.next() & 0x1FFFFFFC); r.next();
                for (uint32_t i = 0; i < cnt && i < 24; i++) idx.push_back(i32 ? rd32(ibase + i * 4) : rd16(ibase + i * 2));
            }
            g_draws++;
            log_draw(prim, cnt, src, i32, idx);
            break;
        }
        case 0x36:   // DRAW_INDX_2
        {
            uint32_t init = r.next(), cnt = init >> 16, prim = init & 0x3F, src = (init >> 6) & 3; bool i32 = (init >> 11) & 1;
            std::vector<uint32_t> idx;
            if (src == 1)   // indices dentro do pacote
                for (uint32_t i = 0; i < cnt && i < 24; i++) { uint32_t w = rd32(r.base + ((r.ring ? (r.pos + i / (i32 ? 1 : 2)) % r.size_dw : r.pos + i / (i32 ? 1 : 2)) * 4)); idx.push_back(i32 ? w : (i & 1 ? (w & 0xFFFF) : (w >> 16))); }
            g_draws++;
            log_draw(prim, cnt, src, i32, idx);
            break;
        }
        default: break;
        }
        r.pos = start + count;
        break;
    }
    }
}

static void cp_body(PPCContext& ctx, uint8_t* base)
{
    uint32_t rptr = 0;
    for (;;)
    {
        uint32_t ring_dw = g_ring_bytes / 4;
        if (ring_dw)
        {
            uint32_t wptr = rd32(GPU_REGS + REG_CP_RB_WPTR * 4) % ring_dw;
            if (wptr != rptr)
            {
                Reader r{ phys2virt(g_ring_phys), ring_dw, rptr, true, &ctx, base };
                int guard = 0;
                while (r.pos % ring_dw != wptr && guard++ < 2000000) exec_packet(r, 0);
                rptr = r.pos % ring_dw;
                if (g_wb_phys) wr32(phys2virt(g_wb_phys), rptr);
            }
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}

// ---------------- relatorio ----------------
static void report_once()
{
    fprintf(stderr, "\n===== relatorio de video =====\n");
    fprintf(stderr, "anel: fisico=0x%08X bytes=0x%X | writeback=0x%08X | fence do sistema=0x%08X\n", g_ring_phys, g_ring_bytes, g_wb_phys, g_sys_fence);
    fprintf(stderr, "vsyncs=%llu pacotes=%llu interrupcoes=%llu cercas=%llu trocas=%llu desenhos=%llu\n",
        (unsigned long long)g_vsyncs.load(), (unsigned long long)g_packets.load(), (unsigned long long)g_interrupts.load(),
        (unsigned long long)g_fences.load(), (unsigned long long)g_swaps.load(), (unsigned long long)g_draws.load());
    {
        std::lock_guard<std::mutex> l(g_ops_mu);
        fprintf(stderr, "opcodes PM4 vistos:");
        for (auto& kv : g_ops) fprintf(stderr, " 0x%02X=%llu", kv.first, (unsigned long long)kv.second);
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "registradores da GPU nao zerados:\n");
    int shown = 0;
    for (uint32_t off = 0; off < 0x10000 && shown < 60; off += 4)
    {
        uint32_t v = rd32(GPU_REGS + off);
        if (v) { fprintf(stderr, "  reg 0x%04X = 0x%08X\n", off / 4, v); shown++; }
    }
    if (g_ring_phys)
    {
        uint32_t b = phys2virt(g_ring_phys);
        fprintf(stderr, "primeiros dwords do anel:\n");
        for (int i = 0; i < 32; i += 4)
            fprintf(stderr, "  +%03X: %08X %08X %08X %08X\n", i * 4, rd32(b + i * 4), rd32(b + i * 4 + 4), rd32(b + i * 4 + 8), rd32(b + i * 4 + 12));
    }
    if (g_wb_phys) fprintf(stderr, "valor no writeback: 0x%08X\n", rd32(phys2virt(g_wb_phys)));
    fflush(stderr);
}

static void report_thread()   // relatorios aos 3 s, 10 s e 20 s (o jogo pode cair antes dos 20 s)
{
    std::this_thread::sleep_for(std::chrono::seconds(3));
    report_once();
    std::this_thread::sleep_for(std::chrono::seconds(7));
    report_once();
    std::this_thread::sleep_for(std::chrono::seconds(10));
    report_once();
}

// ---------------- importacoes de video ----------------
static void start_cp_once()
{
    if (g_cp_started.exchange(true)) return;
    g_dump_dir = getenv("NGBC_DUMP_DIR");
    kernel_spawn_thread(cp_body);
}

PPC_FUNC(__imp__VdSetGraphicsInterruptCallback)
{
    KTRACE(VdSetGraphicsInterruptCallback);
    g_cb = ctx.r3.u32;
    g_cb_ctx = ctx.r4.u32;
    fprintf(stderr, "[video] callback de interrupcao: 0x%08X contexto 0x%08X\n", g_cb.load(), g_cb_ctx.load());
    if (g_cb && !g_vsync_started.exchange(true))
    {
        std::thread(report_thread).detach();
        kernel_spawn_thread([](PPCContext& ctx, uint8_t* base) {   // vsync a 60 Hz
            for (;;)
            {
                std::this_thread::sleep_for(std::chrono::microseconds(16667));
                ctx.r3.u64 = 0;                 // origem 0 = vsync
                ctx.r4.u64 = g_cb_ctx.load();
                PPC_CALL_INDIRECT_FUNC(g_cb.load());
                g_vsyncs++;
            }
        });
    }
}

PPC_FUNC(__imp__VdInitializeRingBuffer)
{
    KTRACE(VdInitializeRingBuffer);
    g_ring_phys = ctx.r3.u32;
    g_ring_bytes = 1u << (ctx.r4.u32 & 31);   // tamanho em bytes = 1 << log2
    fprintf(stderr, "[video] anel de comandos: base=0x%08X log2=%u (%u bytes)\n", g_ring_phys, ctx.r4.u32, g_ring_bytes);
    start_cp_once();
}
PPC_FUNC(__imp__VdEnableRingBufferRPtrWriteBack)
{
    KTRACE(VdEnableRingBufferRPtrWriteBack);
    g_wb_phys = ctx.r3.u32;
    fprintf(stderr, "[video] writeback do ponteiro de leitura: 0x%08X bloco 0x%X\n", g_wb_phys, ctx.r4.u32);
}
PPC_FUNC(__imp__VdSetSystemCommandBufferGpuIdentifierAddress)
{
    KTRACE(VdSetSystemCommandBufferGpuIdentifierAddress);
    g_sys_fence = ctx.r3.u32;
    fprintf(stderr, "[video] endereco do identificador do buffer do sistema: 0x%08X\n", g_sys_fence);
}

// X_VIDEO_MODE: 1280x720, widescreen, alta definicao, 60 Hz
PPC_FUNC(__imp__VdQueryVideoMode)
{
    KTRACE(VdQueryVideoMode);
    uint32_t p = ctx.r3.u32;
    wr32(p + 0x00, 1280); wr32(p + 0x04, 720); wr32(p + 0x08, 0); wr32(p + 0x0C, 1); wr32(p + 0x10, 1);
    wr32(p + 0x14, 0x42700000); wr32(p + 0x18, 1);
}