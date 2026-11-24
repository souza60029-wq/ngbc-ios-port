// runtime do port NGBC - nucleo: memoria do convidado, alocadores, trace, sinais, main.
#include "ngbc.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
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

uint8_t* g_base = nullptr;
std::string g_data_root = ".";

void commit(uint32_t addr, uint64_t size)
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
static uint64_t g_heap_next = 0x40000000ull;
static uint64_t g_phys_next = 0xA0000000ull;
static uint64_t g_stack_next = 0x70000000ull;

static uint32_t align_up(uint64_t v, uint64_t a) { return uint32_t((v + a - 1) & ~(a - 1)); }

uint32_t guest_alloc(uint64_t size, uint64_t align)
{
    std::lock_guard<std::mutex> l(g_alloc_mu);
    uint32_t a = align_up(g_heap_next, align);
    g_heap_next = uint64_t(a) + ((size + 0xFFF) & ~0xFFFull);
    commit(a, size);
    return a;
}

uint32_t guest_alloc_phys(uint64_t size, uint64_t align)
{
    std::lock_guard<std::mutex> l(g_alloc_mu);
    if (align < 0x1000) align = 0x1000;
    uint32_t a = align_up(g_phys_next, align);
    g_phys_next = uint64_t(a) + ((size + 0xFFF) & ~0xFFFull);
    commit(a, size);
    return a;
}

uint32_t guest_alloc_stack(uint64_t size)
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

void ngbc_print_summary(const char* motivo)
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

// ---------------- chamada indireta nao resolvida ----------------
PPC_FUNC(ngbc_unresolved)
{
    fprintf(stderr, "[!] chamada indireta nao resolvida: ctr=%08X lr=%08X\n", ctx.ctr.u32, (uint32_t)ctx.lr);
    ngbc_print_summary("chamada indireta nao resolvida");
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
    ngbc_print_summary("falha de segmentacao");
    _exit(2);
}

static void entry_thread(PPCContext& ctx, uint8_t* base)
{
    fprintf(stderr, "[runtime] iniciando o jogo: entrada %s\n", PPC_STRINGIFY(NGBC_ENTRY));
    NGBC_ENTRY(ctx, base);
    fprintf(stderr, "[runtime] a funcao de entrada retornou r3=%08X\n", ctx.r3.u32);
    ngbc_print_summary("entrada retornou");
    _exit(0);
}

// Inicia o jogo. seconds > 0: encerra apos esse tempo (modo de teste). seconds == 0: roda para sempre (app).
int ngbc_run(const char* image_path, const char* data_root, int seconds)
{
    g_data_root = data_root ? data_root : ".";
    const uint64_t kSize = 0x100000000ull;
    void* m = mmap(nullptr, kSize, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (m == MAP_FAILED)
    {
        fprintf(stderr, "[!] nao consegui reservar 4 GB de enderecos\n");
        return 11;
    }
    g_base = (uint8_t*)m;
    fprintf(stderr, "[runtime] memoria do convidado reservada em %p\n", (void*)g_base);

    commit(PPC_IMAGE_BASE, PPC_IMAGE_SIZE);
    FILE* f = fopen(image_path, "rb");
    if (!f) { fprintf(stderr, "[!] nao abri %s\n", image_path); return 12; }
    size_t got = fread(g_base + PPC_IMAGE_BASE, 1, PPC_IMAGE_SIZE, f);
    fclose(f);
    fprintf(stderr, "[runtime] imagem carregada: %zu bytes; arquivos do jogo em %s\n", got, g_data_root.c_str());

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

    kernel_spawn_main(entry_thread);

    if (seconds > 0)
    {
        struct timespec ts{ seconds, 0 };
        nanosleep(&ts, nullptr);
        ngbc_print_summary("tempo esgotado");
        _exit(0);
    }
    for (;;) sleep(3600);
    return 0;
}

#ifdef NGBC_HOST
int main(int argc, char** argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "uso: ngbc_host imagem.bin pasta_do_jogo [segundos]\n");
        return 1;
    }
    return ngbc_run(argv[1], argv[2], argc > 3 ? atoi(argv[3]) : 20);
}
#endif