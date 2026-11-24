// kernel.cpp - importacoes do kernel do Xbox 360 com comportamento real:
// memoria, tempo, TLS, eventos/semaforos/esperas, secoes criticas, threads e arquivos do jogo.
#include "ngbc.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <thread>
#include <mutex>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define KTRACE(n) trace_import(#n, ctx)

static const uint32_t ST_OK = 0, ST_TIMEOUT = 0x102, ST_INVALID_INFO = 0xC0000003, ST_NO_SUCH_FILE = 0xC000000F,
                      ST_EOF = 0xC0000011, ST_NAME_NOT_FOUND = 0xC0000034, ST_NO_MORE_FILES = 0x80000006,
                      ST_ACCESS_DENIED = 0xC0000022;

static uint32_t align_up32(uint64_t v, uint64_t a) { return uint32_t((v + a - 1) & ~(a - 1)); }

// ======================= memoria =======================
PPC_FUNC(__imp__NtAllocateVirtualMemory)
{
    KTRACE(NtAllocateVirtualMemory);
    uint32_t pbase = ctx.r3.u32, psize = ctx.r4.u32;
    uint32_t want = rd32(pbase), size = rd32(psize);
    uint32_t addr = want ? want : guest_alloc(size ? size : 0x1000, 0x10000);
    if (want) commit(want, size ? size : 0x1000);
    wr32(pbase, addr);
    wr32(psize, align_up32(size, 0x1000));
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__NtFreeVirtualMemory) { KTRACE(NtFreeVirtualMemory); ctx.r3.u64 = 0; }
PPC_FUNC(__imp__MmAllocatePhysicalMemoryEx) { KTRACE(MmAllocatePhysicalMemoryEx); ctx.r3.u64 = guest_alloc_phys(ctx.r4.u32, ctx.r8.u32); }
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

// ======================= tempo, TLS, misc =======================
static thread_local uint32_t t_tls[64];
static std::atomic<uint32_t> g_tls_next{0};
PPC_FUNC(__imp__KeTlsAlloc) { KTRACE(KeTlsAlloc); ctx.r3.u64 = g_tls_next++ % 64; }
PPC_FUNC(__imp__KeTlsFree) { KTRACE(KeTlsFree); ctx.r3.u64 = 1; }
PPC_FUNC(__imp__KeTlsGetValue) { KTRACE(KeTlsGetValue); ctx.r3.u64 = t_tls[ctx.r3.u32 % 64]; }
PPC_FUNC(__imp__KeTlsSetValue) { KTRACE(KeTlsSetValue); t_tls[ctx.r3.u32 % 64] = ctx.r4.u32; ctx.r3.u64 = 1; }

static uint64_t filetime_now()
{
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t(ts.tv_sec) + 11644473600ull) * 10000000ull + ts.tv_nsec / 100;
}
PPC_FUNC(__imp__KeQueryPerformanceFrequency) { KTRACE(KeQueryPerformanceFrequency); ctx.r3.u64 = 50000000ull; }
PPC_FUNC(__imp__KeQuerySystemTime) { KTRACE(KeQuerySystemTime); wr64(ctx.r3.u32, filetime_now()); }
PPC_FUNC(__imp__KeGetCurrentProcessType) { KTRACE(KeGetCurrentProcessType); ctx.r3.u64 = 1; }
PPC_FUNC(__imp__XGetLanguage) { KTRACE(XGetLanguage); ctx.r3.u64 = 1; }
PPC_FUNC(__imp__HalReturnToFirmware)
{
    KTRACE(HalReturnToFirmware);
    ngbc_print_summary("HalReturnToFirmware");
    _exit(0);
}

// intervalo do convidado (LARGE_INTEGER*): 0 = nao espera; negativo = relativo (100 ns); positivo = absoluto.
struct Timeout { bool infinite; std::chrono::nanoseconds dur; };
static Timeout read_timeout(uint32_t ptr)
{
    if (ptr == 0) return { true, {} };
    int64_t t = (int64_t)rd64(ptr);
    if (t == 0) return { false, std::chrono::nanoseconds(0) };
    if (t < 0) return { false, std::chrono::nanoseconds((-t) * 100) };
    int64_t d = t - (int64_t)filetime_now();
    return { false, std::chrono::nanoseconds(d > 0 ? d * 100 : 0) };
}

PPC_FUNC(__imp__KeDelayExecutionThread)
{
    KTRACE(KeDelayExecutionThread);
    Timeout t = read_timeout(ctx.r5.u32);
    if (!t.infinite) std::this_thread::sleep_for(t.dur);
    else for (;;) sleep(3600);
    ctx.r3.u64 = 0;
}

// ======================= objetos do kernel =======================
enum class KType { Event, Semaphore, Thread, File };
struct KObj
{
    KType type = KType::Event;
    bool manual = false, signaled = false;       // evento
    int32_t count = 0, maxc = 0x7FFFFFFF;        // semaforo
    bool finished = false, suspended = false;    // thread
    uint32_t exit_code = 0;
    int fd = -1; uint64_t pos = 0, size = 0; bool dir = false; std::string path;  // arquivo
    ~KObj() { if (fd >= 0) close(fd); }
};
typedef std::shared_ptr<KObj> KRef;

static std::mutex g_mu;                 // protege o estado de TODOS os objetos
static std::condition_variable g_cv;
static std::map<uint32_t, KRef> g_handles;
static std::map<uint32_t, KRef> g_ptrobjs;
static uint32_t g_next_handle = 0xF8000004;

static uint32_t add_handle(KRef o)
{
    std::lock_guard<std::mutex> l(g_mu);
    uint32_t h = g_next_handle; g_next_handle += 4;
    g_handles[h] = o;
    return h;
}
static KRef get_handle(uint32_t h)
{
    std::lock_guard<std::mutex> l(g_mu);
    auto it = g_handles.find(h);
    return it == g_handles.end() ? nullptr : it->second;
}
// handle ou ponteiro para objeto despachavel (KEVENT etc.) na memoria do convidado
static KRef get_dispatch(uint32_t v)
{
    {
        std::lock_guard<std::mutex> l(g_mu);
        auto it = g_handles.find(v);
        if (it != g_handles.end()) return it->second;
        auto jt = g_ptrobjs.find(v);
        if (jt != g_ptrobjs.end()) return jt->second;
    }
    auto o = std::make_shared<KObj>();
    uint8_t type = rd8(v);   // 0 = evento manual, 1 = evento automatico, 5 = semaforo
    if (type == 5) { o->type = KType::Semaphore; o->count = (int32_t)rd32(v + 4); o->maxc = (int32_t)rd32(v + 0x10); if (o->maxc <= 0) o->maxc = 0x7FFFFFFF; }
    else { o->type = KType::Event; o->manual = (type == 0); o->signaled = rd32(v + 4) != 0; }
    std::lock_guard<std::mutex> l(g_mu);
    auto& slot = g_ptrobjs[v];
    if (!slot) slot = o;
    return slot;
}

static bool is_signaled(KObj& o)
{
    switch (o.type)
    {
    case KType::Event: return o.signaled;
    case KType::Semaphore: return o.count > 0;
    case KType::Thread: return o.finished;
    default: return true;
    }
}
static void consume(KObj& o)
{
    if (o.type == KType::Event && !o.manual) o.signaled = false;
    if (o.type == KType::Semaphore) o.count--;
}

static uint32_t do_wait(const std::vector<KRef>& objs, bool all, Timeout t)
{
    std::unique_lock<std::mutex> l(g_mu);
    auto deadline = std::chrono::steady_clock::now() + t.dur;
    for (;;)
    {
        if (all)
        {
            bool ok = true;
            for (auto& o : objs) if (!is_signaled(*o)) ok = false;
            if (ok) { for (auto& o : objs) consume(*o); return 0; }
        }
        else
        {
            for (size_t i = 0; i < objs.size(); i++)
                if (is_signaled(*objs[i])) { consume(*objs[i]); return (uint32_t)i; }
        }
        if (!t.infinite && std::chrono::steady_clock::now() >= deadline) return ST_TIMEOUT;
        if (t.infinite) g_cv.wait(l); else g_cv.wait_until(l, deadline);
    }
}

static uint32_t wait_list(uint32_t count, uint32_t list, uint32_t waittype, uint32_t timeout_ptr)
{
    std::vector<KRef> v;
    for (uint32_t i = 0; i < count && i < 64; i++)
    {
        KRef o = get_dispatch(rd32(list + i * 4));
        if (!o) return 0xC0000008;  // STATUS_INVALID_HANDLE
        v.push_back(o);
    }
    return do_wait(v, waittype == 0, read_timeout(timeout_ptr));
}

PPC_FUNC(__imp__NtCreateEvent)
{
    KTRACE(NtCreateEvent);
    auto o = std::make_shared<KObj>();
    o->type = KType::Event; o->manual = (ctx.r5.u32 == 0); o->signaled = ctx.r6.u32 != 0;
    if (ctx.r3.u32) wr32(ctx.r3.u32, add_handle(o));
    ctx.r3.u64 = 0;
}
static uint32_t event_set(KRef o, bool value)
{
    std::lock_guard<std::mutex> l(g_mu);
    uint32_t prev = o->signaled;
    o->signaled = value;
    g_cv.notify_all();
    return prev;
}
PPC_FUNC(__imp__NtSetEvent)
{
    KTRACE(NtSetEvent);
    KRef o = get_dispatch(ctx.r3.u32);
    uint32_t prev = o ? event_set(o, true) : 0;
    if (ctx.r4.u32) wr32(ctx.r4.u32, prev);
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__NtClearEvent) { KTRACE(NtClearEvent); KRef o = get_dispatch(ctx.r3.u32); if (o) event_set(o, false); ctx.r3.u64 = 0; }
PPC_FUNC(__imp__KeSetEvent) { KTRACE(KeSetEvent); KRef o = get_dispatch(ctx.r3.u32); ctx.r3.u64 = o ? event_set(o, true) : 0; }
PPC_FUNC(__imp__KeResetEvent) { KTRACE(KeResetEvent); KRef o = get_dispatch(ctx.r3.u32); ctx.r3.u64 = o ? event_set(o, false) : 0; }

PPC_FUNC(__imp__NtCreateSemaphore)
{
    KTRACE(NtCreateSemaphore);
    auto o = std::make_shared<KObj>();
    o->type = KType::Semaphore; o->count = (int32_t)ctx.r5.u32; o->maxc = ctx.r6.u32 ? (int32_t)ctx.r6.u32 : 0x7FFFFFFF;
    if (ctx.r3.u32) wr32(ctx.r3.u32, add_handle(o));
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__NtReleaseSemaphore)
{
    KTRACE(NtReleaseSemaphore);
    KRef o = get_dispatch(ctx.r3.u32);
    uint32_t prev = 0;
    if (o)
    {
        std::lock_guard<std::mutex> l(g_mu);
        prev = (uint32_t)o->count;
        int64_t n = int64_t(o->count) + int32_t(ctx.r4.u32);
        o->count = n > o->maxc ? o->maxc : (int32_t)n;
        g_cv.notify_all();
    }
    if (ctx.r5.u32) wr32(ctx.r5.u32, prev);
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__NtDuplicateObject) { KTRACE(NtDuplicateObject); if (ctx.r4.u32) wr32(ctx.r4.u32, ctx.r3.u32); ctx.r3.u64 = 0; }
PPC_FUNC(__imp__NtClose)
{
    KTRACE(NtClose);
    std::lock_guard<std::mutex> l(g_mu);
    g_handles.erase(ctx.r3.u32);
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__NtWaitForSingleObjectEx)
{
    KTRACE(NtWaitForSingleObjectEx);
    KRef o = get_dispatch(ctx.r3.u32);
    ctx.r3.u64 = o ? do_wait({ o }, false, read_timeout(ctx.r6.u32)) : 0xC0000008;
}
PPC_FUNC(__imp__KeWaitForSingleObject)
{
    KTRACE(KeWaitForSingleObject);
    KRef o = get_dispatch(ctx.r3.u32);
    ctx.r3.u64 = o ? do_wait({ o }, false, read_timeout(ctx.r7.u32)) : 0xC0000008;
}
PPC_FUNC(__imp__NtWaitForMultipleObjectsEx)
{
    KTRACE(NtWaitForMultipleObjectsEx);
    ctx.r3.u64 = wait_list(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r8.u32);
}
PPC_FUNC(__imp__KeWaitForMultipleObjects)
{
    KTRACE(KeWaitForMultipleObjects);
    ctx.r3.u64 = wait_list(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r9.u32);
}

// ======================= secoes criticas / locks =======================
static std::mutex g_cs_mu;
static std::map<uint32_t, std::unique_ptr<std::recursive_mutex>> g_cs;
static std::recursive_mutex* cs_of(uint32_t p)
{
    std::lock_guard<std::mutex> l(g_cs_mu);
    auto& u = g_cs[p];
    if (!u) u.reset(new std::recursive_mutex);
    return u.get();
}
PPC_FUNC(__imp__RtlInitializeCriticalSection) { KTRACE(RtlInitializeCriticalSection); cs_of(ctx.r3.u32); }
PPC_FUNC(__imp__RtlInitializeCriticalSectionAndSpinCount) { KTRACE(RtlInitializeCriticalSectionAndSpinCount); cs_of(ctx.r3.u32); }
PPC_FUNC(__imp__RtlEnterCriticalSection) { KTRACE(RtlEnterCriticalSection); cs_of(ctx.r3.u32)->lock(); }
PPC_FUNC(__imp__RtlLeaveCriticalSection) { KTRACE(RtlLeaveCriticalSection); cs_of(ctx.r3.u32)->unlock(); }
PPC_FUNC(__imp__RtlTryEnterCriticalSection) { KTRACE(RtlTryEnterCriticalSection); ctx.r3.u64 = cs_of(ctx.r3.u32)->try_lock() ? 1 : 0; }
PPC_FUNC(__imp__ExInitializeReadWriteLock) { KTRACE(ExInitializeReadWriteLock); cs_of(ctx.r3.u32); }
PPC_FUNC(__imp__ExAcquireReadWriteLockExclusive) { KTRACE(ExAcquireReadWriteLockExclusive); cs_of(ctx.r3.u32)->lock(); }
PPC_FUNC(__imp__ExAcquireReadWriteLockShared) { KTRACE(ExAcquireReadWriteLockShared); cs_of(ctx.r3.u32)->lock(); }
PPC_FUNC(__imp__ExReleaseReadWriteLock) { KTRACE(ExReleaseReadWriteLock); cs_of(ctx.r3.u32)->unlock(); }

// ======================= threads =======================
struct ThreadInit
{
    KRef obj;
    std::function<void(PPCContext&, uint8_t*)> body;
    uint32_t stack_size;
};
static thread_local KObj* t_self = nullptr;

static void finish_thread(KObj* o, uint32_t code)
{
    if (!o) return;
    std::lock_guard<std::mutex> l(g_mu);
    o->finished = true; o->exit_code = code;
    g_cv.notify_all();
}

static void* thread_entry(void* p)
{
    std::unique_ptr<ThreadInit> ti((ThreadInit*)p);
    t_self = ti->obj.get();
    if (ti->obj)
    {
        std::unique_lock<std::mutex> l(g_mu);
        while (ti->obj->suspended) g_cv.wait(l);
    }
    uint32_t ssz = ti->stack_size < 0x40000 ? 0x40000 : ti->stack_size;
    uint32_t sb = guest_alloc_stack(ssz), st = sb + ssz;
    uint32_t pcr = guest_alloc(0x2000, 0x1000), tls = guest_alloc(0x10000, 0x1000), thr = guest_alloc(0x1000, 0x1000);
    wr32(pcr + 0x00, tls);
    wr32(pcr + 0x30, pcr);
    wr32(pcr + 0x70, st);
    wr32(pcr + 0x74, sb);
    wr32(pcr + 0x100, thr);

    PPCContext ctx{};
    ctx.fpscr.loadFromHost();
    ctx.r1.u64 = st - 0x100;
    ctx.r13.u64 = pcr;
    ti->body(ctx, g_base);
    finish_thread(ti->obj.get(), ctx.r3.u32);
    return nullptr;
}

static void spawn(KRef obj, std::function<void(PPCContext&, uint8_t*)> body, uint32_t stack_size, bool detached)
{
    auto* ti = new ThreadInit{ obj, std::move(body), stack_size };
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64 * 1024 * 1024);
    if (detached) pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t th;
    pthread_create(&th, &attr, thread_entry, ti);
}

void kernel_spawn_main(void (*fn)(PPCContext&, uint8_t*))
{
    spawn(nullptr, fn, 0x100000, true);
}

static std::atomic<uint32_t> g_next_tid{0x100};
PPC_FUNC(__imp__ExCreateThread)
{
    KTRACE(ExCreateThread);
    uint32_t xapi = ctx.r6.u32, start = ctx.r7.u32, context = ctx.r8.u32, flags = ctx.r9.u32;
    auto o = std::make_shared<KObj>();
    o->type = KType::Thread; o->suspended = (flags & 1) != 0;
    uint32_t h = add_handle(o);
    if (ctx.r3.u32) wr32(ctx.r3.u32, h);
    if (ctx.r5.u32) wr32(ctx.r5.u32, g_next_tid++);
    spawn(o, [xapi, start, context](PPCContext& ctx, uint8_t* base) {
        if (xapi) { ctx.r3.u64 = start; ctx.r4.u64 = context; PPC_CALL_INDIRECT_FUNC(xapi); }
        else { ctx.r3.u64 = context; PPC_CALL_INDIRECT_FUNC(start); }
    }, ctx.r4.u32, true);
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__NtResumeThread)
{
    KTRACE(NtResumeThread);
    KRef o = get_handle(ctx.r3.u32);
    uint32_t prev = 0;
    if (o)
    {
        std::lock_guard<std::mutex> l(g_mu);
        prev = o->suspended ? 1 : 0;
        o->suspended = false;
        g_cv.notify_all();
    }
    if (ctx.r4.u32) wr32(ctx.r4.u32, prev);
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__ExTerminateThread)
{
    KTRACE(ExTerminateThread);
    finish_thread(t_self, ctx.r3.u32);
    pthread_exit(nullptr);
}

// ======================= arquivos do jogo =======================
static std::string guest_ansi(uint32_t ptr)
{
    if (!ptr) return "";
    uint16_t len = rd16(ptr);
    uint32_t buf = rd32(ptr + 4);
    return std::string((const char*)(g_base + buf), len);
}

static std::string norm_rel(std::string p)
{
    for (auto& c : p) if (c == '\\') c = '/';
    std::string low = p;
    for (auto& c : low) c = (char)tolower((unsigned char)c);
    size_t i;
    if ((i = low.find("game:")) != std::string::npos) p = p.substr(i + 5);
    else if (low.rfind("/device/", 0) == 0)
    {
        size_t k = 8, e = low.find('/', k);              // /device/<x>
        std::string first = low.substr(k, e == std::string::npos ? std::string::npos : e - k);
        p = e == std::string::npos ? "" : p.substr(e + 1);
        if (first.rfind("harddisk", 0) == 0)             // /device/harddisk0/<partition>
        {
            size_t e2 = p.find('/');
            p = e2 == std::string::npos ? "" : p.substr(e2 + 1);
        }
    }
    else if ((i = p.find(':')) != std::string::npos) p = p.substr(i + 1);
    while (!p.empty() && p[0] == '/') p.erase(0, 1);
    return p;
}

static std::string resolve_path(const std::string& rel)
{
    std::string cur = g_data_root;
    size_t start = 0;
    while (start < rel.size())
    {
        size_t e = rel.find('/', start);
        std::string comp = rel.substr(start, e == std::string::npos ? std::string::npos : e - start);
        start = e == std::string::npos ? rel.size() : e + 1;
        if (comp.empty() || comp == ".") continue;
        DIR* d = opendir(cur.c_str());
        if (!d) return "";
        std::string found;
        while (auto* en = readdir(d))
            if (strcasecmp(en->d_name, comp.c_str()) == 0) { found = en->d_name; break; }
        closedir(d);
        if (found.empty()) return "";
        cur += "/" + found;
    }
    return cur;
}

static std::atomic<int> g_open_fail_logs{0};
static uint32_t open_game_file(uint32_t objattr, uint32_t* out_handle)
{
    std::string raw = guest_ansi(rd32(objattr + 4));
    std::string path = resolve_path(norm_rel(raw));
    struct stat sb;
    if (path.empty() || stat(path.c_str(), &sb) != 0)
    {
        if (g_open_fail_logs++ < 60) fprintf(stderr, "[arq] nao encontrado: '%s'\n", raw.c_str());
        return ST_NAME_NOT_FOUND;
    }
    auto o = std::make_shared<KObj>();
    o->type = KType::File; o->path = path;
    o->dir = S_ISDIR(sb.st_mode); o->size = (uint64_t)sb.st_size;
    if (!o->dir)
    {
        o->fd = open(path.c_str(), O_RDONLY);
        if (o->fd < 0) return ST_ACCESS_DENIED;
    }
    if (g_open_fail_logs < 60) fprintf(stderr, "[arq] aberto: '%s' -> %s (%llu bytes)\n", raw.c_str(), path.c_str(), (unsigned long long)o->size);
    *out_handle = add_handle(o);
    return ST_OK;
}
static void set_iosb(uint32_t iosb, uint32_t status, uint32_t info)
{
    if (!iosb) return;
    wr32(iosb, status);
    wr32(iosb + 4, info);
}

PPC_FUNC(__imp__NtCreateFile)
{
    KTRACE(NtCreateFile);
    uint32_t h = 0, st = open_game_file(ctx.r5.u32, &h);
    if (st == ST_OK && ctx.r3.u32) wr32(ctx.r3.u32, h);
    set_iosb(ctx.r6.u32, st, st == ST_OK ? 1 : 0);
    ctx.r3.u64 = st;
}
PPC_FUNC(__imp__NtOpenFile)
{
    KTRACE(NtOpenFile);
    uint32_t h = 0, st = open_game_file(ctx.r5.u32, &h);
    if (st == ST_OK && ctx.r3.u32) wr32(ctx.r3.u32, h);
    set_iosb(ctx.r6.u32, st, st == ST_OK ? 1 : 0);
    ctx.r3.u64 = st;
}
PPC_FUNC(__imp__NtReadFile)
{
    KTRACE(NtReadFile);
    KRef f = get_handle(ctx.r3.u32);
    uint32_t iosb = ctx.r7.u32, buf = ctx.r8.u32, len = ctx.r9.u32, offptr = ctx.r10.u32;
    if (!f || f->type != KType::File || f->fd < 0) { set_iosb(iosb, 0xC0000008, 0); ctx.r3.u64 = 0xC0000008; return; }
    uint64_t off = offptr ? rd64(offptr) : f->pos;
    if (off >= f->size) { set_iosb(iosb, ST_EOF, 0); ctx.r3.u64 = ST_EOF; return; }
    ssize_t n = pread(f->fd, g_base + buf, len, (off_t)off);
    if (n < 0) n = 0;
    f->pos = off + (uint64_t)n;
    set_iosb(iosb, ST_OK, (uint32_t)n);
    ctx.r3.u64 = ST_OK;
}
PPC_FUNC(__imp__NtWriteFile)
{
    KTRACE(NtWriteFile);
    set_iosb(ctx.r7.u32, ST_OK, ctx.r9.u32);   // gravacao descartada por enquanto
    ctx.r3.u64 = ST_OK;
}

static void fill_network_open(uint32_t dst, uint64_t size, bool dir)
{
    for (int i = 0; i < 4; i++) wr64(dst + i * 8, 0);
    wr64(dst + 0x20, (size + 0xFFF) & ~0xFFFull);
    wr64(dst + 0x28, size);
    wr32(dst + 0x30, dir ? 0x10 : 0x80);
}
PPC_FUNC(__imp__NtQueryInformationFile)
{
    KTRACE(NtQueryInformationFile);
    KRef f = get_handle(ctx.r3.u32);
    uint32_t iosb = ctx.r4.u32, buf = ctx.r5.u32, cls = ctx.r7.u32;
    if (!f) { set_iosb(iosb, 0xC0000008, 0); ctx.r3.u64 = 0xC0000008; return; }
    uint32_t st = ST_OK, info = 0;
    if (cls == 5)        // FileStandardInformation
    {
        wr64(buf, (f->size + 0xFFF) & ~0xFFFull); wr64(buf + 8, f->size); wr32(buf + 0x10, 1);
        wr8(buf + 0x14, 0); wr8(buf + 0x15, f->dir ? 1 : 0); info = 0x18;
    }
    else if (cls == 14) { wr64(buf, f->pos); info = 8; }
    else if (cls == 34) { fill_network_open(buf, f->size, f->dir); info = 0x38; }
    else { st = ST_INVALID_INFO; fprintf(stderr, "[arq] NtQueryInformationFile classe %u nao suportada\n", cls); }
    set_iosb(iosb, st, info);
    ctx.r3.u64 = st;
}
PPC_FUNC(__imp__NtSetInformationFile)
{
    KTRACE(NtSetInformationFile);
    KRef f = get_handle(ctx.r3.u32);
    uint32_t st = ST_OK;
    if (f && ctx.r7.u32 == 14) f->pos = rd64(ctx.r5.u32);
    else if (!f) st = 0xC0000008;
    set_iosb(ctx.r4.u32, st, 0);
    ctx.r3.u64 = st;
}
PPC_FUNC(__imp__NtQueryFullAttributesFile)
{
    KTRACE(NtQueryFullAttributesFile);
    std::string raw = guest_ansi(rd32(ctx.r3.u32 + 4));
    std::string path = resolve_path(norm_rel(raw));
    struct stat sb;
    if (path.empty() || stat(path.c_str(), &sb) != 0) { ctx.r3.u64 = ST_NAME_NOT_FOUND; return; }
    fill_network_open(ctx.r4.u32, (uint64_t)sb.st_size, S_ISDIR(sb.st_mode));
    ctx.r3.u64 = ST_OK;
}
PPC_FUNC(__imp__NtQueryDirectoryFile) { KTRACE(NtQueryDirectoryFile); set_iosb(ctx.r5.u32, ST_NO_MORE_FILES, 0); ctx.r3.u64 = ST_NO_MORE_FILES; }

// ======================= strings =======================
PPC_FUNC(__imp__RtlInitAnsiString)
{
    KTRACE(RtlInitAnsiString);
    uint32_t src = ctx.r4.u32, n = 0;
    if (src) while (rd8(src + n)) n++;
    wr16(ctx.r3.u32, (uint16_t)n);
    wr16(ctx.r3.u32 + 2, (uint16_t)(src ? n + 1 : 0));
    wr32(ctx.r3.u32 + 4, src);
}
PPC_FUNC(__imp__RtlInitUnicodeString)
{
    KTRACE(RtlInitUnicodeString);
    uint32_t src = ctx.r4.u32, n = 0;
    if (src) while (rd16(src + n * 2)) n++;
    wr16(ctx.r3.u32, (uint16_t)(n * 2));
    wr16(ctx.r3.u32 + 2, (uint16_t)(src ? n * 2 + 2 : 0));
    wr32(ctx.r3.u32 + 4, src);
}