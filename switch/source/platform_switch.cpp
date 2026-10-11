// Switch platform layer: process entry, data directory, env file, logging, guest memory window,
// thread stacks. GPU / audio backends live in their own files (null_backends.cpp for M2).
#include <switch.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <typeinfo>

#ifndef LSW_DATA_DIR
#define LSW_DATA_DIR "sdmc:/lswtcs"
#endif

// The runtime's main() is compiled as lsw_runtime_main (-Dmain=lsw_runtime_main on main.cpp).
extern int lsw_runtime_main();

namespace {
FILE* g_stdout_log;

uint64_t info_u64(uint32_t id) { uint64_t v = 0; svcGetInfo(&v, id, CUR_PROCESS_HANDLE, 0); return v; }

// env.txt: one NAME=VALUE per line, '#' comments. Replaces the PC launch scripts' environment.
void load_env_file(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) { printf("[NX] no %s (using defaults)\n", path); return; }
    char line[512];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        char* s = line;
        while (*s == ' ' || *s == '\t') ++s;
        if (*s == '#' || *s == '\n' || *s == '\r' || !*s) continue;
        char* e = s + strlen(s);
        while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
        char* eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        setenv(s, eq + 1, 1);
        printf("[NX] env %s=%s\n", s, eq + 1);
        ++n;
    }
    fclose(f);
    printf("[NX] %d env settings from %s\n", n, path);
}
}  // namespace

// ---- thread stacks --------------------------------------------------------------------------
// Each guest call is a host call, so deep guest recursion needs a deep host stack. std::thread
// passes no attributes; give every thread at least kMinStack (linked with --wrap=pthread_create).
static constexpr size_t kMinStack = 8u << 20;
extern "C" int __real_pthread_create(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);
// Core hint for threads that third-party code creates (Mesa's glthread worker): libnx starts every thread on the
// process's default core (0 = the guest threads). While lsw_set_new_thread_core(c) is set, new threads first pin
// themselves to core c.
// The hint is per creating thread (thread_local): other threads creating threads meanwhile (shader-compile workers,
// Mesa queues) are not affected. Pinned threads get priority 0x2C: below the audio output thread (0x2B, also core 2;
// with LSWTCS_GLPIPE the worker spins in nouveau's GPU fence wait, which made audio extremely choppy on hardware,
// 2026-10-09) and above the core-2 shader-compile worker (0x2D): Horizon does not time-slice equal priorities.
static thread_local int g_new_thread_core = -1;
extern "C" void lsw_set_new_thread_core(int core) { g_new_thread_core = core; }
namespace {
struct PinnedStart { void* (*fn)(void*); void* arg; int core; };
void* pinned_start(void* p) {
    PinnedStart s = *static_cast<PinnedStart*>(p);
    delete static_cast<PinnedStart*>(p);
    const Result rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, s.core, 1u << s.core);
    const Result rp = svcSetThreadPriority(CUR_THREAD_HANDLE, 0x2C);
    printf("[NX] new thread pinned to core %d, priority 0x2C (rc 0x%X / 0x%X)\n", s.core, rc, rp);
    fflush(stdout);
    return s.fn(s.arg);
}
}  // namespace
extern "C" int __wrap_pthread_create(pthread_t* t, const pthread_attr_t* attr, void* (*fn)(void*), void* arg) {
    pthread_attr_t a;
    if (attr) a = *attr; else pthread_attr_init(&a);
    size_t sz = 0;
    pthread_attr_getstacksize(&a, &sz);
    if (sz < kMinStack) pthread_attr_setstacksize(&a, kMinStack);
    int rc;
    if (const int core = g_new_thread_core; core >= 0) {
        auto* s = new PinnedStart{fn, arg, core};
        rc = __real_pthread_create(t, &a, pinned_start, s);
        if (rc) delete s;
    } else {
        rc = __real_pthread_create(t, &a, fn, arg);
    }
    if (!attr) pthread_attr_destroy(&a);
    return rc;
}

// Host-only bursts of work by a guest thread that has left the scheduler rotation (native_reverb.cpp,
// LSWTCS_REVERBOFF): every guest thread is on core 0, so the work moves to core 2 at the audio output thread's
// priority (0x2B: ahead of the glthread worker 0x2C and the compile worker 0x2D, which would otherwise keep the core;
// Horizon does not time-slice equal priorities) and comes back afterwards.
static thread_local s32 g_offcore_prev_core = -1;
static thread_local u64 g_offcore_prev_mask = 0;
static thread_local s32 g_offcore_prev_prio = -1;
extern "C" void lsw_offcore_begin_prio(int prio);
extern "C" void lsw_offcore_begin() { lsw_offcore_begin_prio(0x2B); }
// Same with a chosen priority (the PM4 walk, kernel_stubs.cpp LSWTCS_WALKOFF: its multi-ms bursts at 0x2B starved the
// glthread worker on core 2 and with it the render thread).
extern "C" void lsw_offcore_begin_prio(int new_prio) {
    s32 core = 0; u64 mask = 0;
    if (R_FAILED(svcGetThreadCoreMask(&core, &mask, CUR_THREAD_HANDLE))) { g_offcore_prev_core = -1; return; }
    s32 prio = 0;
    g_offcore_prev_prio = R_SUCCEEDED(svcGetThreadPriority(&prio, CUR_THREAD_HANDLE)) ? prio : -1;
    g_offcore_prev_core = core; g_offcore_prev_mask = mask;
    svcSetThreadPriority(CUR_THREAD_HANDLE, new_prio);
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, 2, 1u << 2);
}
extern "C" void lsw_offcore_end() {
    if (g_offcore_prev_core < 0) return;
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, g_offcore_prev_core, g_offcore_prev_mask);
    if (g_offcore_prev_prio >= 0) svcSetThreadPriority(CUR_THREAD_HANDLE, g_offcore_prev_prio);
    g_offcore_prev_core = -1;
}

// C11 thrd_create for Mesa: newlib defines thrd_success = 4, but switch-mesa 20.1's u_thread_create() does
// `if (ret) return 0;` (assumes thrd_success == 0), so every Mesa worker thread was created and then reported as
// failed: util_queue_init() returned false -> glthread never enabled, Mesa's other queues (e.g. the shader disk-cache
// writer) never ran. Only Mesa uses C11 threads here (our code and libstdc++ use pthreads).
extern "C" int __real_thrd_create(void* thr, int (*func)(void*), void* arg);
extern "C" int __wrap_thrd_create(void* thr, int (*func)(void*), void* arg) {
    const int r = __real_thrd_create(thr, func, arg);
    return r == 4 /* newlib thrd_success */ ? 0 : (r ? r : 2 /* thrd_error */);
}

// ---- crash reports --------------------------------------------------------------------------
// libnx calls this for CPU exceptions (alignment, undefined instruction, brk, host data aborts).
// NOTE: Eden does not raise data aborts for unmapped addresses (EDEN_MEMORY.md probe #2).
// Offsets are relative to _start (ELF address 0 = NRO load base; libnx's __start__ is an absolute,
// unrelocated symbol). Symbolize with: aarch64-none-elf-addr2line -fCe switch/build/lswtcs.elf <offset>
extern "C" void _start();
#define __start__ (*(char*)_start)
alignas(16) u8 __nx_exception_stack[0x10000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);
extern "C" void __libnx_exception_handler(ThreadExceptionDump* ctx) {
    const u64 b = (u64)&__start__;
    FILE* f = fopen(LSW_DATA_DIR "/crash.log", "w");
    FILE* outs[2] = {f, stdout};
    for (FILE* o : outs) {
        if (!o) continue;
        fprintf(o, "[CRASH] desc=0x%X pc=0x%llX (+0x%llX) lr=0x%llX (+0x%llX) far=0x%llX sp=0x%llX\n",
                ctx->error_desc, (unsigned long long)ctx->pc.x, (unsigned long long)(ctx->pc.x - b),
                (unsigned long long)ctx->lr.x, (unsigned long long)(ctx->lr.x - b),
                (unsigned long long)ctx->far.x, (unsigned long long)ctx->sp.x);
        for (int i = 0; i < 29; ++i) fprintf(o, "[CRASH] x%-2d=0x%016llX\n", i, (unsigned long long)ctx->cpu_gprs[i].x);
        // Frame-pointer walk (x29 chain), offsets only.
        u64 fp = ctx->fp.x;
        for (int d = 0; d < 32 && fp && !(fp & 7); ++d) {
            MemoryInfo mi; u32 pi;
            if (R_FAILED(svcQueryMemory(&mi, &pi, fp)) || !(mi.perm & Perm_R)) break;
            u64 ret = ((u64*)fp)[1];
            fprintf(o, "[CRASH]   frame %2d ret=+0x%llX\n", d, (unsigned long long)(ret - b));
            fp = ((u64*)fp)[0];
        }
        fflush(o);
    }
    if (f) fclose(f);
    svcExitProcess();
}

// exit/abort wrappers (-Wl,--wrap=...): log who ended the process. Symbolize like crash offsets.
extern "C" [[noreturn]] void __real_exit(int);
extern "C" [[noreturn]] void __real_abort(void);
extern "C" [[noreturn]] void __wrap_exit(int code) {
    printf("[NX] exit(%d) called from +0x%llX\n", code,
           (unsigned long long)((u64)__builtin_return_address(0) - (u64)&__start__));
    fflush(stdout);
    __real_exit(code);
}
extern "C" [[noreturn]] void __wrap_abort(void) {
    printf("[NX] abort() called from +0x%llX\n",
           (unsigned long long)((u64)__builtin_return_address(0) - (u64)&__start__));
    fflush(stdout);
    __real_abort();
}

// C++ throws (-Wl,--wrap=__cxa_throw): the runtime is built without exception handling, so any
// libstdc++ throw ends in std::terminate. Log the type and the thrower's frames first.
static void log_backtrace(const char* tag) {
    const u64 b = (u64)&__start__;
    u64 fp = (u64)__builtin_frame_address(0);
    for (int d = 0; d < 24 && fp && !(fp & 7); ++d) {
        MemoryInfo mi; u32 pi;
        if (R_FAILED(svcQueryMemory(&mi, &pi, fp)) || !(mi.perm & Perm_R)) break;
        printf("[NX] %s frame %2d ret=+0x%llX\n", tag, d, (unsigned long long)(((u64*)fp)[1] - b));
        fp = ((u64*)fp)[0];
    }
    fflush(stdout);
}
extern "C" [[noreturn]] void __real___cxa_throw(void*, std::type_info*, void (*)(void*));
extern "C" [[noreturn]] void __wrap___cxa_throw(void* obj, std::type_info* type, void (*dtor)(void*)) {
    printf("[NX] C++ throw of %s\n", type ? type->name() : "?");
    log_backtrace("throw");
    __real___cxa_throw(obj, type, dtor);
}

// ---- C++ runtime self-test ------------------------------------------------------------------
// The runtime relies on std::call_once / std::thread / std::condition_variable (dbg_ram, the GIL
// scheduler). Exercise each once at startup and log, so a libstdc++/libnx gap shows up by name.
static void runtime_selftest() {
    printf("[NX] selftest: call_once..."); fflush(stdout);
    static std::once_flag once; int hits = 0;
    std::call_once(once, [&] { ++hits; });
    std::call_once(once, [&] { ++hits; });
    printf(" ok (%d)\n[NX] selftest: thread...", hits); fflush(stdout);
    int v = 0;
    std::thread t([&] { v = 42; });
    t.join();
    printf(" ok (%d)\n[NX] selftest: condvar...", v); fflush(stdout);
    std::mutex m; std::condition_variable cv; bool ready = false;
    std::thread t2([&] { std::lock_guard<std::mutex> lk(m); ready = true; cv.notify_all(); });
    { std::unique_lock<std::mutex> lk(m); cv.wait_for(lk, std::chrono::seconds(2), [&] { return ready; }); }
    t2.join();
    printf(" ok (%d)\n", (int)ready); fflush(stdout);
}

// libnx's pthread_detach fails, so std::thread::detach() throws (-> terminate). The runtime detaches
// long-lived workers (trace flusher, XMA, ...): report success; the thread runs on and its control
// block is simply never reclaimed (-Wl,--wrap=pthread_detach).
extern "C" int __wrap_pthread_detach(pthread_t) { return 0; }

// DIAG: time threads spend acquiring newlib's single malloc lock (-Wl,--wrap=__malloc_lock). Parallel
// shader builds ran 10-20x slower than one build alone (2026-10-08); this measures whether malloc
// contention is why. Acquisitions over ~1 us (20 ticks of the 19.2 MHz counter) count as contended.
struct _reent;
extern "C" void __real___malloc_lock(struct _reent*);
static uint64_t s_malloc_wait_ticks = 0, s_malloc_contended = 0, s_malloc_calls = 0;
extern "C" void __wrap___malloc_lock(struct _reent* r) {
    const uint64_t t0 = armGetSystemTick();
    __real___malloc_lock(r);
    const uint64_t d = armGetSystemTick() - t0;
    ++s_malloc_calls;   // under the lock: exact
    if (d > 20) { s_malloc_wait_ticks += d; ++s_malloc_contended; }
}
// Totals so far: wait in ns, contended acquisitions, all acquisitions.
extern "C" void lsw_malloc_lock_stats(uint64_t* wait_ns, uint64_t* contended, uint64_t* calls) {
    *wait_ns = armTicksToNs(s_malloc_wait_ticks);
    *contended = s_malloc_contended;
    *calls = s_malloc_calls;
}

// ---- init -----------------------------------------------------------------------------------
extern "C" void lsw_switch_init() {
    mkdir(LSW_DATA_DIR, 0777);
    chdir(LSW_DATA_DIR);
    // The previous session's logs are kept as *_prev.log (a relaunch used to overwrite them before they were read:
    // the [FTRING] of a camera capture was lost, 2026-10-09). Capture triggers left by a session that ended
    // mid-capture are dropped, or the first frame of this one is captured (a black shot_1.png).
    for (const char* n : {"stdout", "trace", "frames"}) {
        char cur[32], prev[32];
        snprintf(cur, sizeof cur, "%s.log", n);
        snprintf(prev, sizeof prev, "%s_prev.log", n);
        remove(prev);
        rename(cur, prev);
    }
    remove("shot_now");
    remove("resdump_now");
    // stdout/stderr -> log file (printf is the runtime's main log; dbg_ram writes trace.log).
    g_stdout_log = freopen(LSW_DATA_DIR "/stdout.log", "w", stdout);
    freopen(LSW_DATA_DIR "/stderr.log", "w", stderr);
    printf("[NX] LSWTCS Switch runtime\n");
    printf("[NX] TotalMemory %llu MB, UsedMemory %llu MB at start\n",
           (unsigned long long)(info_u64(InfoType_TotalMemorySize) >> 20),
           (unsigned long long)(info_u64(InfoType_UsedMemorySize) >> 20));
    load_env_file(LSW_DATA_DIR "/env.txt");
    runtime_selftest();
}

// ---- guest memory window ----------------------------------------------------------------------
// Probe #2 (Eden/EDEN_MEMORY.md): no overcommit, no svcMapPhysicalMemory, so each used range is
// heap memory turned into code memory and mapped RW (MapOwner) at its exact window offset.
// Unbacked parts of the window stay unmapped. NOTE: Eden does not fault on unmapped accesses
// (reads 0, drops writes); real hardware does.
// Measured on PC (kernel_stubs "memscan_now", hub, single-view): touched guest memory is
// 0x7FC/0x7FE MMIO pages, 0x80000000-0x9F400000 and the table; plus ONE page each at 0x00000000,
// 0x01000000 and 0x79600000 (near-null reads? not investigated yet; backed so hardware does not fault
// where Windows read zeros). The 0xE0000000 pool is skipped when LSWTCS_HEAPBYPASS=0 (the run config).
namespace {
struct Range { uint64_t off, size; const char* what; bool bypass_only; };
const Range kRanges[] = {
    { 0x00000000ull, 0x00100000ull, "stray page 0x00000000 (1 MB)", false },
    { 0x01000000ull, 0x00100000ull, "stray page 0x01000000 (1 MB)", false },
    { 0x79600000ull, 0x00100000ull, "stray page 0x79600000 (1 MB)", false },
    { 0x7F000000ull, 0x01000000ull, "GPU writeback + MMIO 0x7F000000", false },
    { 0x80000000ull, 0x20000000ull, "physical 512 MB (image, vheap, phys)", false },
    { 0xE0000000ull, 0x10000000ull, "small-alloc pool (unless LSWTCS_HEAPBYPASS=0)", true },
    { 0x100000000ull, 0x04000000ull, "dispatch table", false },
};
}

extern "C" uint8_t* lsw_switch_guest_window(size_t size) {
    virtmemLock();
    uint8_t* w = (uint8_t*)virtmemFindAslr(size, 0x10000);
    VirtmemReservation* res = w ? virtmemAddReservation(w, size) : nullptr;
    virtmemUnlock();
    if (!res) { printf("[NX] cannot reserve 0x%zX-byte guest window\n", size); return nullptr; }
    printf("[NX] guest window %p (0x%zX bytes)\n", (void*)w, size);
    const char* hb = getenv("LSWTCS_HEAPBYPASS");
    const bool bypass = !(hb && hb[0] == '0');   // same default as main.cpp lswtcs_bypass_on(): ON unless "0"
    for (const Range& r : kRanges) {
        if (r.off + r.size > size || (r.bypass_only && !bypass)) continue;
        void* src = aligned_alloc(0x1000, r.size);
        if (!src) { printf("[NX]   %-40s alloc 0x%llX FAILED\n", r.what, (unsigned long long)r.size); return nullptr; }
        Handle h;
        Result rc = svcCreateCodeMemory(&h, src, r.size);
        if (R_SUCCEEDED(rc)) rc = svcControlCodeMemory(h, CodeMapOperation_MapOwner, w + r.off, r.size, Perm_Rw);
        if (R_FAILED(rc)) { printf("[NX]   %-40s map FAILED rc=0x%X\n", r.what, rc); return nullptr; }
        memset(w + r.off, 0, r.size);
        printf("[NX]   +0x%09llX %-40s %4llu MB\n", (unsigned long long)r.off, r.what, (unsigned long long)(r.size >> 20));
    }
    printf("[NX] UsedMemory after window: %llu MB\n", (unsigned long long)(info_u64(InfoType_UsedMemorySize) >> 20));
    return w;
}

// ---- entry ----------------------------------------------------------------------------------
static void* runtime_thread(void*) { int rc = lsw_runtime_main(); printf("[NX] runtime main returned %d\n", rc); return nullptr; }

int main(int, char**) {
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 16u << 20);
    if (pthread_create(&t, &a, runtime_thread, nullptr) != 0) return 1;
    pthread_attr_destroy(&a);
    pthread_join(t, nullptr);
    if (g_stdout_log) fflush(g_stdout_log);
    return 0;
}
