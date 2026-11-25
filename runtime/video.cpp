// video.cpp - GPU do Xbox 360 (v0 exploratoria): interrupcao de vsync, argumentos Vd* e um relatorio do estado.
#include "ngbc.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#define KTRACE(n) trace_import(#n, ctx)

static std::atomic<uint32_t> g_cb{0}, g_cb_ctx{0};
static std::atomic<bool> g_vsync_started{false};
static uint32_t g_ring_base = 0, g_ring_log2 = 0, g_wb_ptr = 0, g_wb_arg = 0, g_sys_fence = 0;
static std::atomic<uint64_t> g_vsyncs{0};

static void report_thread()
{
    std::this_thread::sleep_for(std::chrono::seconds(20));
    fprintf(stderr, "\n===== relatorio de video =====\n");
    fprintf(stderr, "anel: base=0x%08X log2=%u | writeback ptr=0x%08X arg=0x%08X | fence do sistema=0x%08X | vsyncs entregues=%llu\n",
        g_ring_base, g_ring_log2, g_wb_ptr, g_wb_arg, g_sys_fence, (unsigned long long)g_vsyncs.load());
    fprintf(stderr, "registradores da GPU (0x7FC80000) nao zerados:\n");
    int shown = 0;
    for (uint32_t off = 0; off < 0x10000 && shown < 80; off += 4)
    {
        uint32_t v = rd32(0x7FC80000 + off);
        if (v) { fprintf(stderr, "  reg 0x%04X (off 0x%04X) = 0x%08X\n", off / 4, off, v); shown++; }
    }
    if (g_ring_base)
    {
        fprintf(stderr, "primeiros dwords do anel:\n");
        for (int i = 0; i < 48; i += 4)
            fprintf(stderr, "  +%03X: %08X %08X %08X %08X\n", i * 4, rd32(g_ring_base + i * 4), rd32(g_ring_base + i * 4 + 4), rd32(g_ring_base + i * 4 + 8), rd32(g_ring_base + i * 4 + 12));
    }
    if (g_wb_ptr) fprintf(stderr, "valor no writeback: 0x%08X\n", rd32(g_wb_ptr));
    fflush(stderr);
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
        kernel_spawn_thread([](PPCContext& ctx, uint8_t* base) {   // um "vsync" a 60 Hz
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
    g_ring_base = ctx.r3.u32; g_ring_log2 = ctx.r4.u32;
    fprintf(stderr, "[video] anel de comandos: base=0x%08X log2=%u\n", g_ring_base, g_ring_log2);
}
PPC_FUNC(__imp__VdEnableRingBufferRPtrWriteBack)
{
    KTRACE(VdEnableRingBufferRPtrWriteBack);
    g_wb_ptr = ctx.r3.u32; g_wb_arg = ctx.r4.u32;
    fprintf(stderr, "[video] writeback do ponteiro de leitura: 0x%08X arg 0x%08X\n", g_wb_ptr, g_wb_arg);
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