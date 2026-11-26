// video.cpp - GPU do Xbox 360 (v1): vsync a 60 Hz, processador de comandos PM4 minimo e relatorio do estado.
#include "ngbc.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <thread>

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
    case 0: { uint32_t count = ((h >> 16) & 0x3FFF) + 1; for (uint32_t i = 0; i < count; i++) r.next(); break; }
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
        case 0x64: g_swaps++; break;                           // XE_SWAP: troca de quadro
        case 0x22: case 0x36: g_draws++; break;                // DRAW_INDX / DRAW_INDX_2
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