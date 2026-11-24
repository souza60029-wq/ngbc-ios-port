// runtime v0 do port NGBC (Linux de teste e iOS).
// Memoria do convidado (4 GB), tabela de funcoes, importacoes do kernel com trace.
#include "ppc_recomp_shared.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#if defined(__linux__)
#include <execinfo.h>
#endif

#ifndef NGBC_ENTRY
#define NGBC_ENTRY sub_822C64A0
#endif

static uint8_t* g_base = nullptr;

// ---------------- acesso a memoria do convidado (big-endian) ----------------
static inline uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, g_base + a, 4); return __builtin_bswap32(v); }
static inline void wr32(uint32_t a, uint32_t v) { v = __builtin_bswap32(v); memcpy(g_base + a, &v, 4); }
static inline void wr64(uint32_t a, uint64_t v) { v = __builtin_bswap64(v); memcpy(g_base + a, &v, 8); }

static void commit(uint32_t addr, uint64_t size)
{
    uint64_t a0 = addr & ~0xFFFull;
    uint64_t a1 = (uint64_t(addr) + size + 0xFFF) & ~0xFFFull;
    if (mprotect(g_base + a0, a1 - a0, PROT_READ | PROT_WRITE) != 0)
    {
        fprintf(stderr, "[!] mprotect falhou: guest 0x%llX tam 0x%llX\n", (unsigned long long)a0, (unsigned long long)(a1 - a0));
        _exit(10);
    }
}

// ---------------- alocadores simples ----------------
static std::mutex g_alloc_mu;
static uint64_t g_heap_next = 0x40000000ull;   // memoria virtual
static uint64_t g_phys_next = 0xA0000000ull;   // memoria fisica (GPU, audio)
static uint64_t g_stack_next = 0x70000000ull;  // pilhas de threads

static uint32_t align_up(uint64_t v, uint64_t a) { return uint32_t((v + a - 1) & ~(a - 1)); }

static uint32_t guest_alloc(uint64_t size, uint64_t align = 0x1000)
{
    std::lock_guard<std::mutex> l(g_alloc_mu);
    uint32_t a = align_up(g_heap_next, align);
    g_heap_next = uint64_t(a) + ((size + 0xFFF) & ~0xFFFull);
    commit(a, size);
    return a;
}

static uint32_t guest_alloc_phys(uint64_t size, uint64_t align)
{
    std::lock_guard<std::mutex> l(g_alloc_mu);
    if (align < 0x1000) align = 0x1000;
    uint32_t a = align_up(g_phys_next, align);
    g_phys_next = uint64_t(a) + ((size + 0xFFF) & ~0xFFFull);
    commit(a, size);
    return a;
}

static uint32_t guest_alloc_stack(uint64_t size)
{
    std::lock_guard<std::mutex> l(g_alloc_mu);
    uint32_t a = align_up(g_stack_next, 0x10000);
    g_stack_next = uint64_t(a) + size + 0x10000;
    commit(a, size);
    return a;
}

// ---------------- trace das importacoes ----------------
static std::mutex g_trace_mu;
static std::map<std::string, uint64_t> g_counts;
static std::vector<std::string> g_ring;
static uint64_t g_total = 0;

void trace_import(const char* name, PPCContext& ctx)
{
    std::lock_guard<std::mutex> l(g_trace_mu);
    g_total++;
    g_counts[name]++;
    char buf[256];
    snprintf(buf, sizeof buf, "%s(r3=%08X r4=%08X r5=%08X r6=%08X) lr=%08X", name, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, (uint32_t)ctx.lr);
    if (g_total <= 300)
        fprintf(stderr, "[imp] %s\n", buf);
    if (g_ring.size() < 40)
        g_ring.push_back(buf);
    else
        g_ring[g_total % 40] = buf;
}

static void print_summary(const char* motivo)
{
    std::lock_guard<std::mutex> l(g_trace_mu);
    fprintf(stderr, "\n===== resumo (%s) =====\nchamadas de importacao: %llu\n", motivo, (unsigned long long)g_total);
    std::vector<std::pair<uint64_t, std::string>> v;
    for (auto& kv : g_counts) v.push_back({kv.second, kv.first});
    std::sort(v.rbegin(), v.rend());
    for (size_t i = 0; i < v.size() && i < 40; i++)
        fprintf(stderr, "  %8llu  %s\n", (unsigned long long)v[i].first, v[i].second.c_str());
    fprintf(stderr, "ultimas chamadas:\n");
    for (auto& s : g_ring) fprintf(stderr, "  %s\n", s.c_str());
    fflush(stderr);
}

#define KTRACE(n) trace_import(#n, ctx)

// ---------------- importacoes com comportamento real ----------------
PPC_FUNC(__imp__NtAllocateVirtualMemory)
{
    KTRACE(NtAllocateVirtualMemory);
    uint32_t pbase = ctx.r3.u32, psize = ctx.r4.u32;
    uint32_t want = rd32(pbase), size = rd32(psize);
    uint32_t addr = want ? want : guest_alloc(size ? size : 0x1000, 0x10000);
    if (want) commit(want, size ? size : 0x1000);
    wr32(pbase, addr);
    wr32(psize, align_up(size, 0x1000));
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__NtFreeVirtualMemory) { KTRACE(NtFreeVirtualMemory); ctx.r3.u64 = 0; }

PPC_FUNC(__imp__MmAllocatePhysicalMemoryEx)
{
    KTRACE(MmAllocatePhysicalMemoryEx);
    ctx.r3.u64 = guest_alloc_phys(ctx.r4.u32, ctx.r8.u32);
}

PPC_FUNC(__imp__MmFreePhysicalMemory) { KTRACE(MmFreePhysicalMemory); }
PPC_FUNC(__imp__MmGetPhysicalAddress) { KTRACE(MmGetPhysicalAddress); ctx.r3.u64 = ctx.r3.u32 & 0x1FFFFFFFu; }

PPC_FUNC(__imp__XamAlloc)
{
    KTRACE(XamAlloc);
    wr32(ctx.r5.u32, guest_alloc(ctx.r4.u32 ? ctx.r4.u32 : 0x10, 0x10));
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__XamFree) { KTRACE(XamFree); ctx.r3.u64 = 0; }

PPC_FUNC(__imp__ExAllocatePoolTypeWithTag) { KTRACE(ExAllocatePoolTypeWithTag); ctx.r3.u64 = guest_alloc(ctx.r3.u32 ? ctx.r3.u32 : 0x10, 0x10); }
PPC_FUNC(__imp__ExAllocatePoolWithTag) { KTRACE(ExAllocatePoolWithTag); ctx.r3.u64 = guest_alloc(ctx.r3.u32 ? ctx.r3.u32 : 0x10, 0x10); }
PPC_FUNC(__imp__ExFreePool) { KTRACE(ExFreePool); }

static thread_local uint32_t t_tls[64];
static std::atomic<uint32_t> g_tls_next{0};
PPC_FUNC(__imp__KeTlsAlloc) { KTRACE(KeTlsAlloc); ctx.r3.u64 = g_tls_next++ % 64; }
PPC_FUNC(__imp__KeTlsFree) { KTRACE(KeTlsFree); ctx.r3.u64 = 1; }
PPC_FUNC(__imp__KeTlsGetValue) { KTRACE(KeTlsGetValue); ctx.r3.u64 = t_tls[ctx.r3.u32 % 64]; }
PPC_FUNC(__imp__KeTlsSetValue) { KTRACE(KeTlsSetValue); t_tls[ctx.r3.u32 % 64] = ctx.r4.u32; ctx.r3.u64 = 1; }

PPC_FUNC(__imp__KeQueryPerformanceFrequency) { KTRACE(KeQueryPerformanceFrequency); ctx.r3.u64 = 50000000ull; }
PPC_FUNC(__imp__KeQuerySystemTime)
{
    KTRACE(KeQuerySystemTime);
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t ft = (uint64_t(ts.tv_sec) + 11644473600ull) * 10000000ull + ts.tv_nsec / 100;
    wr64(ctx.r3.u32, ft);
}
PPC_FUNC(__imp__KeGetCurrentProcessType) { KTRACE(KeGetCurrentProcessType); ctx.r3.u64 = 1; }
PPC_FUNC(__imp__KeDelayExecutionThread) { KTRACE(KeDelayExecutionThread); usleep(1000); ctx.r3.u64 = 0; }
PPC_FUNC(__imp__RtlTryEnterCriticalSection) { KTRACE(RtlTryEnterCriticalSection); ctx.r3.u64 = 1; }
PPC_FUNC(__imp__XGetLanguage) { KTRACE(XGetLanguage); ctx.r3.u64 = 1; }

PPC_FUNC(__imp__HalReturnToFirmware)
{
    KTRACE(HalReturnToFirmware);
    print_summary("HalReturnToFirmware");
    _exit(0);
}

// ---------------- chamada indireta nao resolvida ----------------
PPC_FUNC(ngbc_unresolved)
{
    fprintf(stderr, "[!] chamada indireta nao resolvida: ctr=%08X lr=%08X\n", ctx.ctr.u32, (uint32_t)ctx.lr);
    print_summary("chamada indireta nao resolvida");
    _exit(3);
}

// ---------------- sinais ----------------
static void on_segv(int sig, siginfo_t* si, void*)
{
    uintptr_t a = (uintptr_t)si->si_addr;
    fprintf(stderr, "\n[!] SIGNAL %d em %p", sig, si->si_addr);
    if (g_base && a >= (uintptr_t)g_base && a < (uintptr_t)g_base + 0x100000000ull)
        fprintf(stderr, " (endereco do convidado 0x%08X)", (uint32_t)(a - (uintptr_t)g_base));
    fprintf(stderr, "\n");
#if defined(__linux__)
    void* bt[40];
    int n = backtrace(bt, 40);
    backtrace_symbols_fd(bt, n, 2);
#endif
    print_summary("falha de segmentacao");
    _exit(2);
}

// ---------------- thread principal do convidado ----------------
struct GuestStart { uint32_t stack_top; uint32_t stack_bottom; };

static void* guest_main(void* p)
{
    auto* gs = (GuestStart*)p;
    uint32_t pcr = guest_alloc(0x2000, 0x1000);
    uint32_t tlsb = guest_alloc(0x10000, 0x1000);
    uint32_t thr = guest_alloc(0x1000, 0x1000);
    wr32(pcr + 0x00, tlsb);
    wr32(pcr + 0x30, pcr);
    wr32(pcr + 0x70, gs->stack_top);
    wr32(pcr + 0x74, gs->stack_bottom);
    wr32(pcr + 0x100, thr);

    PPCContext ctx{};
    ctx.fpscr.loadFromHost();
    ctx.r1.u64 = gs->stack_top - 0x100;
    ctx.r13.u64 = pcr;
    fprintf(stderr, "[runtime] iniciando o jogo: entrada %s, pilha 0x%08X, PCR 0x%08X\n", PPC_STRINGIFY(NGBC_ENTRY), gs->stack_top, pcr);
    NGBC_ENTRY(ctx, g_base);
    fprintf(stderr, "[runtime] a funcao de entrada retornou r3=%08X\n", ctx.r3.u32);
    print_summary("entrada retornou");
    _exit(0);
    return nullptr;
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "uso: ngbc_host imagem.bin [segundos]\n");
        return 1;
    }
    int seconds = argc > 2 ? atoi(argv[2]) : 20;

    const uint64_t kSize = 0x100000000ull;
    void* m = mmap(nullptr, kSize, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (m == MAP_FAILED)
    {
        fprintf(stderr, "[!] nao consegui reservar 4 GB de enderecos\n");
        return 11;
    }
    g_base = (uint8_t*)m;
    fprintf(stderr, "[runtime] memoria do convidado reservada em %p\n", (void*)g_base);

    // imagem do jogo
    commit(PPC_IMAGE_BASE, PPC_IMAGE_SIZE);
    FILE* f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "[!] nao abri %s\n", argv[1]); return 12; }
    size_t got = fread(g_base + PPC_IMAGE_BASE, 1, PPC_IMAGE_SIZE, f);
    fclose(f);
    fprintf(stderr, "[runtime] imagem carregada: %zu bytes\n", got);

    // tabela de funcoes (logo apos a imagem)
    commit(PPC_IMAGE_BASE + PPC_IMAGE_SIZE, PPC_CODE_SIZE * 2 + 16);
    for (uint64_t a = PPC_CODE_BASE; a < PPC_CODE_BASE + PPC_CODE_SIZE; a += 4)
        PPC_LOOKUP_FUNC(g_base, a) = ngbc_unresolved;
    size_t n = 0;
    for (; PPCFuncMappings[n].guest != 0 || PPCFuncMappings[n].host != nullptr; n++)
        PPC_LOOKUP_FUNC(g_base, PPCFuncMappings[n].guest) = PPCFuncMappings[n].host;
    fprintf(stderr, "[runtime] tabela de funcoes: %zu entradas\n", n);

    struct sigaction sa{};
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGILL, &sa, nullptr);
    sigaction(SIGFPE, &sa, nullptr);

    // pilha do convidado
    static GuestStart gs;
    const uint32_t stackSize = 0x100000;
    gs.stack_bottom = guest_alloc_stack(stackSize);
    gs.stack_top = gs.stack_bottom + stackSize;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64 * 1024 * 1024);
    pthread_t th;
    pthread_create(&th, &attr, guest_main, &gs);

    struct timespec ts{ seconds, 0 };
    nanosleep(&ts, nullptr);
    print_summary("tempo esgotado");
    _exit(0);
}