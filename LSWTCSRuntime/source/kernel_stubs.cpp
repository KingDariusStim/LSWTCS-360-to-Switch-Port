// Project root (set by CMake); data files are resolved relative to it.
#ifndef LSW_ROOT_DIR
#define LSW_ROOT_DIR ".."
#endif
#include <set>
#include "../../Convert 360/LSWTCS/output/ppc_context.h"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdarg>
#include <cstdlib>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <map>
#include <vector>
#include <deque>
#include <algorithm>
#include <atomic>
#include <chrono>
#ifdef _WIN32
#  include <windows.h>
#endif
#include "gpu_d3d12.h"
#include "xenos_vs.h"

// g_base is defined in main.cpp; VdSwap needs it to pass to the GPU renderer.
extern uint8_t* g_base;
extern "C" void lswtcs_set_vtable_watch();  // arms the fs-device vtable hardware watchpoint (main.cpp)
extern "C" void lswtcs_set_pending_shad(const char* hash);  // gpu_d3d12.cpp — harvester hash↔HLSL pairing

// If `path` is a SHAD-cache open (…\SHAD\0X<hash>), record the bare hash token so
// the next Path-B compile can be paired with the game's shader hash (harvester).
static void note_shad_open(const std::string& path) {
    size_t s = path.find("SHAD");
    if (s == std::string::npos) return;
    // take the trailing filename component after SHAD's separator
    size_t sep = path.find_last_of("\\/");
    const char* tok = (sep != std::string::npos) ? path.c_str() + sep + 1 : path.c_str() + s;
    if (tok[0]) lswtcs_set_pending_shad(tok);
}

// Diagnostic: logs indirect-call sites whose target is out-of-range or whose
// resolved host function is null (skipped by the guarded PPC_CALL_INDIRECT_FUNC
// macro). target = guest call target; lr = return addr (call site = lr-4).
extern "C" void ppc_indirect_skip_log(uint32_t target, uint32_t lr) {
    static std::mutex m; static std::unordered_map<uint64_t,uint32_t> seen; static int total = 0;
    std::lock_guard<std::mutex> lk(m);
    uint64_t key = ((uint64_t)lr << 32) | target;
    uint32_t n = ++seen[key];
    if (n <= 2 && total < 60) { ++total;
        dbg_ram("[INDSKIP] target=0x%08X callsite=0x%08X (#%u for this site)\n", target, lr - 4, n);

    }
}

// ── CMDLOG: script native-command veneer tracer (LSWTCS_CMDLOG=1) ──────────────
// The GSC script VM calls native commands through six identical 1669-entry ×
// 16-byte veneer ladders; call sites are PATCHED INTO SCRIPT DATA at load time
// (linker loop at 0x825F035C computes base+idx*16 per module), so there is no
// static dispatcher to instrument. Instead: wrap every ladder slot in the
// dispatch table with a tiny x64 thunk (mov r8d,id; jmp lswtcs_cmdlog_hit) that
// logs (cmd idx, ladder, ctx.lr = VM exec call site, r3/r4) then calls the
// original veneer. Goal: see which commands the frontend script issues and where
// it stalls short of #1213 (sub_8220EB58, the titles scene loader).
// 12 ladders: the six EB58-referencing ones found first, plus six more found
// via "which veneers form 0x8220D680" (frontend loop = command 736 in all 12;
// sets differ per module — e.g. only the first six bind cmd 1213 to EB58).
#define LSW_CMDLOG_L 12
static const uint32_t LSW_CMDLOG_BASES[LSW_CMDLOG_L] = {
    0x82AEFBB4, 0x82D345DC, 0x82D4458C, 0x82DB20C8, 0x82E10EAC, 0x82E581DC,
    0x82D83204, 0x82DC3ED4, 0x82DD890C, 0x82E263E0, 0x82E338E4, 0x82E43740 };
static const uint32_t LSW_CMDLOG_N = 1669;
static PPCFunc* g_cmdlog_orig[LSW_CMDLOG_L * 1669];
static uint32_t g_cmdlog_count[LSW_CMDLOG_L * 1669];

extern "C" void lswtcs_cmdlog_hit(PPCContext& ctx, uint8_t* base, uint32_t id) {
    uint32_t idx = id % LSW_CMDLOG_N, ladder = id / LSW_CMDLOG_N;
    uint32_t n = ++g_cmdlog_count[id];
    // first 4 hits per command always; then every 4096th; #1213 always
    if (n <= 4 || idx == 1213 || (n & 0xFFF) == 0) {
        printf("[CMDLOG] cmd=%u L%u #%u lr=%08X r3=%08X r4=%08X\n",
               idx, ladder + 1, n, (uint32_t)ctx.lr, ctx.r3.u32, ctx.r4.u32);
        fflush(stdout);
    }
    g_cmdlog_orig[id](ctx, base);
}

extern "C" void lswtcs_cmdlog_install(uint8_t* base) {
    const char* e = getenv("LSWTCS_CMDLOG");
    if (!e || e[0] == '0') return;
#ifdef _WIN32
    const size_t STUB = 32;
    uint8_t* pool = (uint8_t*)VirtualAlloc(nullptr, LSW_CMDLOG_L * LSW_CMDLOG_N * STUB,
                                           MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!pool) { printf("[CMDLOG] thunk pool alloc failed\n"); return; }
    int installed = 0, missing = 0;
    for (uint32_t L = 0; L < LSW_CMDLOG_L; L++) {
        for (uint32_t i = 0; i < LSW_CMDLOG_N; i++) {
            uint32_t guest = LSW_CMDLOG_BASES[L] + i * 16;
            PPCFunc** slot = &PPC_LOOKUP_FUNC(base, guest);
            PPCFunc* orig = *slot;
            if (!orig) { missing++; continue; }
            uint32_t id = L * LSW_CMDLOG_N + i;
            g_cmdlog_orig[id] = orig;
            uint8_t* st = pool + (size_t)id * STUB;
            st[0] = 0x41; st[1] = 0xB8; memcpy(st + 2, &id, 4);        // mov r8d, id
            uint64_t fp = (uint64_t)&lswtcs_cmdlog_hit;
            st[6] = 0x48; st[7] = 0xB8; memcpy(st + 8, &fp, 8);        // mov rax, imm64
            st[16] = 0xFF; st[17] = 0xE0;                              // jmp rax
            *slot = (PPCFunc*)st;
            installed++;
        }
    }
    FlushInstructionCache(GetCurrentProcess(), pool, LSW_CMDLOG_L * LSW_CMDLOG_N * STUB);
    printf("[CMDLOG] installed %d veneer thunks (%d slots empty)\n", installed, missing);

    // Companion probe: periodically scan guest RAM for BE u32 values that point
    // INTO a ladder (16-aligned to a veneer entry). If the script linker ran,
    // script data is full of such pointers; zero hits ⇒ scripts never linked
    // (blocker is upstream of the VM). Hits also reveal WHERE script data lives.
    std::thread([base]() {
        for (int pass = 1; pass <= 3; pass++) {
            std::this_thread::sleep_for(std::chrono::seconds(45));
            uint64_t hits = 0; int shown = 0;
            uint32_t hist1213 = 0;
            for (uint64_t a = 0x82000000ull; a < 0x96000000ull; a += 4) {
                uint32_t v = __builtin_bswap32(*(uint32_t*)(base + a));
                for (int L = 0; L < LSW_CMDLOG_L; L++) {
                    uint32_t b = LSW_CMDLOG_BASES[L];
                    if (v >= b && v < b + LSW_CMDLOG_N * 16 && ((v - b) & 15) == 0) {
                        // skip self-hits inside the ladders' own code bytes
                        bool inLadder = false;
                        for (int M = 0; M < LSW_CMDLOG_L; M++) {
                            uint32_t bm = LSW_CMDLOG_BASES[M];
                            if (a >= bm && a < bm + LSW_CMDLOG_N * 16) { inLadder = true; break; }
                        }
                        if (inLadder) break;
                        hits++;
                        uint32_t idx = (v - b) / 16;
                        if (idx == 1213) hist1213++;
                        if (shown < 24) {
                            printf("[LADSCAN%d] ptr at 0x%08llX -> L%d cmd=%u\n",
                                   pass, (unsigned long long)a, L + 1, idx);
                            shown++;
                        }
                        break;
                    }
                }
            }
            printf("[LADSCAN%d] total ladder-pointers=%llu (cmd1213 refs=%u)\n",
                   pass, (unsigned long long)hits, hist1213);
            fflush(stdout);
        }
    }).detach();
#endif
}

// ── FNTRACE: generic per-run function tracer (LSWTCS_FNTRACE=hex,hex,…) ────────
// Wraps the dispatch-table slots of the listed guest functions with logging
// thunks (same mechanism as CMDLOG). NOTE: only INDIRECT calls (vtable/function-
// pointer) go through the table — direct `bl` sites compile to direct host calls
// and are NOT seen. Use on indirect-dispatch roots.
static const int FT_MAX = 64;
static struct { uint32_t guest; PPCFunc* orig; uint32_t count; } g_ft[FT_MAX];

extern "C" void lswtcs_fntrace_hit(PPCContext& ctx, uint8_t* base, uint32_t i) {
    auto& e = g_ft[i];
    uint32_t n = ++e.count;
    if (n <= 8 || (n & 0x3FF) == 0) {
        printf("[FNTRACE] %08X #%u lr=%08X r3=%08X r4=%08X r5=%08X r6=%08X\n",
               e.guest, n, (uint32_t)ctx.lr, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
        fflush(stdout);
    }
    e.orig(ctx, base);
}

extern "C" void lswtcs_fntrace_install(uint8_t* base) {
    const char* env = getenv("LSWTCS_FNTRACE");
    if (!env || !*env) return;
#ifdef _WIN32
    const size_t STUB = 32;
    uint8_t* pool = (uint8_t*)VirtualAlloc(nullptr, FT_MAX * STUB,
                                           MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!pool) { printf("[FNTRACE] pool alloc failed\n"); return; }
    char buf[1024]; strncpy(buf, env, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
    int n = 0;
    for (char* tok = strtok(buf, ","); tok && n < FT_MAX; tok = strtok(nullptr, ",")) {
        uint32_t g = (uint32_t)strtoul(tok, nullptr, 16);
        PPCFunc** slot = &PPC_LOOKUP_FUNC(base, g);
        if (!*slot) { printf("[FNTRACE] %08X: no host fn in table\n", g); continue; }
        g_ft[n].guest = g; g_ft[n].orig = *slot; g_ft[n].count = 0;
        uint8_t* st = pool + (size_t)n * STUB;
        uint32_t id = (uint32_t)n;
        st[0] = 0x41; st[1] = 0xB8; memcpy(st + 2, &id, 4);            // mov r8d, id
        uint64_t fp = (uint64_t)&lswtcs_fntrace_hit;
        st[6] = 0x48; st[7] = 0xB8; memcpy(st + 8, &fp, 8);            // mov rax, imm64
        st[16] = 0xFF; st[17] = 0xE0;                                  // jmp rax
        *slot = (PPCFunc*)st;
        n++;
    }
    FlushInstructionCache(GetCurrentProcess(), pool, FT_MAX * STUB);
    printf("[FNTRACE] wrapped %d functions\n", n);
#endif
}

// ── RAM trace facility (replaces printf for timing-sensitive diagnostics) ───────
// printf+fflush in the GPU/load hot paths perturbs the timing-fragile async
// GAME.DAT load race (shifts where the game stalls). Instead, dbg_ram() appends to
// a preallocated RAM buffer at near-constant cost (no syscall/IO during the run)
// and the whole buffer is written ONCE to build/trace.log. A detached timer thread
// triggers the dump after LSWTCS_TRACE_SECS seconds (default 10) — this captures
// the pre-first-frame LOAD phase (where stalls happen) and survives a hard kill
// (taskkill /F skips atexit). Env-gated: set LSWTCS_TRACE=1 to enable.
static char*             g_dbg_buf     = nullptr;
static size_t            g_dbg_cap     = 0;
static size_t            g_dbg_len     = 0;
static bool              g_dbg_on      = false;
static std::mutex        g_dbg_mutex;
// Minimal-overhead hang tracker: emit callees set this on entry (one assignment, no
// dbg_ram → doesn't shift layout enough to tip the startup race). The trace-timer
// thread (separate from the hung compile thread) logs it each flush, so the LAST value
// during a hang = the callee that never returned. (LSWTCS_EMITTRACK gates the callee writes.)
extern "C" volatile int  g_emit_fn = 0;

// CYCLEGUARD (sub_8271ACE8 loc_8271ADC8 type-graph parent-walk): break a corrupted
// 2-node parent cycle (e.g. 0xA0037FDC ↔ 0xA0310718) that spins the compiler parser
// forever. Defined here (C++ linkage) to match the `extern int`/`extern uint32_t[]`
// decls in ppc_recomp.468.cpp. LSWTCS_CYCLEGUARD=0 disables. GIL ⇒ single-threaded.
int      _cycleguard_on = []{ const char* e = getenv("LSWTCS_CYCLEGUARD"); return (e && e[0]=='0') ? 0 : 1; }();
uint32_t _adc8_hist[1024];
int      _adc8_n = 0;

// Snapshot current buffer to build/trace.log (overwrite). Safe to call repeatedly;
// keeps logging active (used both for the periodic flush and the crash handler) so
// the file always holds the lead-up no matter how/when the process dies.
extern "C" void dbg_ram_dump() {
    if (!g_dbg_on) return;
    std::lock_guard<std::mutex> lk(g_dbg_mutex);
    FILE* f = fopen("trace.log", "wb");
    if (f) { fwrite(g_dbg_buf, 1, g_dbg_len, f); fclose(f); }
}

// Lazy init on first dbg_ram call (NOT static-init time — spawning std::thread
// before main() is unreliable under MinGW). call_once is thread-safe.
static std::once_flag g_dbg_once;
static void dbg_ram_do_init() {
    if (!getenv("LSWTCS_TRACE")) return;
    g_dbg_cap = 256u * 1024 * 1024;                 // 256 MB
    g_dbg_buf = (char*)malloc(g_dbg_cap);
    if (!g_dbg_buf) return;
    g_dbg_on = true;
    // Periodic flush so a SILENT death (no AV → crash handler won't fire) still
    // leaves a recent trace.log. Interval via LSWTCS_TRACE_MS (default 1000ms).
    int ms = 1000;
    if (const char* s = getenv("LSWTCS_TRACE_MS")) { int v = atoi(s); if (v > 0) ms = v; }
    std::thread([ms]{ for(;;){ std::this_thread::sleep_for(std::chrono::milliseconds(ms)); dbg_ram_dump(); } }).detach();

    // WATCH: tight host-poll watch on a type-graph node's link offsets (+0 firstChild, +4 next,
    // +32 prev/parent, +36 next-sibling). Logs each value change into the SAME ordered trace buffer,
    // so the function marker immediately preceding a [WATCH] line = the writer (or its callee).
    // Touches only this host file → guest alloc layout (and the node address) stays stable vs the
    // no-watch build. LSWTCS_WATCHADDR=0xA0037FDC arms it. GIL ⇒ guest writes are serialized.
    if (const char* ws = getenv("LSWTCS_WATCHADDR")) {
        uint32_t waddr = (uint32_t)strtoul(ws, nullptr, 0);
        std::thread([waddr]{
            const int offs[4] = {0, 4, 32, 36};
            uint32_t last[4] = {0xDEAD0000u,0xDEAD0001u,0xDEAD0002u,0xDEAD0003u};
            bool init = false;
            for(;;){
                if (g_base) {
                    for (int i=0;i<4;i++){
                        uint32_t v = __builtin_bswap32(*(volatile uint32_t*)(g_base + waddr + offs[i]));
                        if (!init || v != last[i]) {
                            if (init) dbg_ram("[WATCH] 0x%08X+%d : 0x%08X -> 0x%08X (tgt node 0x%08X)\n",
                                waddr, offs[i], last[i], v, (v>=0x24 && v<0xF0000000u)?((v&~1u)-36):0);
                            last[i] = v;
                        }
                    }
                    init = true;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        }).detach();
    }
}

extern "C" void dbg_ram(const char* fmt, ...) {
    std::call_once(g_dbg_once, dbg_ram_do_init);
    if (!g_dbg_on) return;
    std::lock_guard<std::mutex> lk(g_dbg_mutex);
    if (g_dbg_len + 1024 >= g_dbg_cap) return;
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(g_dbg_buf + g_dbg_len, 1000, fmt, ap);  // printf-compatible (fmt carries its own \n)
    va_end(ap);
    if (n > 0) g_dbg_len += (size_t)(n < 1000 ? n : 1000);
}

// Per-thread "did this guest thread run apiThreadStartup (per-thread init)?" flag,
// for diagnosing whether the compiler runs on an initialized thread vs the main thread.
static thread_local int g_thread_inited = 0;
extern "C" uint32_t dbg_tid()   { return (uint32_t)GetCurrentThreadId(); }
extern "C" int      dbg_tinit() { return g_thread_inited; }

// Milliseconds since module load (process start), for timing diagnostics.
static const unsigned long long g_proc_start_ms = (unsigned long long)GetTickCount64();
// Guest timebase for mftb (see ppc_recomp_shared.h): QPC scaled to 49.875 MHz, from process start.
// LSWTCS_MFTB_RAW=1 restores the raw x86 TSC.
extern "C" unsigned long long lswtcs_guest_timebase(void) {
    static int raw = -1; if (raw < 0) { const char* e = getenv("LSWTCS_MFTB_RAW"); raw = (e && e[0] == '1') ? 1 : 0; }
    if (raw) return __builtin_ia32_rdtsc();
    static LARGE_INTEGER f{}, t0{};
    if (!f.QuadPart) { QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0); }
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    unsigned long long d = (unsigned long long)(t.QuadPart - t0.QuadPart);
    return (unsigned long long)((__int128)d * 49875000 / f.QuadPart);
}
extern "C" unsigned long long _kstub_start_ms() {
    return (unsigned long long)GetTickCount64() - g_proc_start_ms;
}

// ── Deterministic virtual clock (LSWTCS_VCLOCK, default OFF) ─────────────────────
// All guest time sources (HalGetTickCount/KeQuerySystemTime/KPRCB tick) default to real
// wall-clock (GetTickCount64). That makes any time-based guest logic — load timeouts, the
// asset-worker-vs-deadline race — non-deterministic vs the instruction stream, which is the
// dominant non-determinism the fine GIL quantum exposes (proven: NOQUANTUM/coarse-QDIV are
// deterministic). VCLOCK ties guest time to GUEST PROGRESS instead: g_vclock_ms advances a
// fixed step each quantum boundary (every 1024 guest-fn entries), so it tracks instruction
// count, not real time → the same instruction stream yields the same clock every run, AND it
// still advances during spin-waits (the quantum fires there too) so timeouts still fire.
static std::atomic<uint64_t> g_vclock_ms{0};
// Default ON: this is the determinism fix (proven: VCLOCK runs are bit-identical after warmup
// AND render, vs wall-clock runs that stall non-deterministically). LSWTCS_VCLOCK=0 disables.
static int vclock_on() { static int v=-1; if(v<0){ const char* e=getenv("LSWTCS_VCLOCK"); v=(e&&e[0]=='0')?0:1; } return v; }
static inline uint64_t lswtcs_now_ms() {
    if (vclock_on()) return g_vclock_ms.load(std::memory_order_relaxed);
    return (uint64_t)GetTickCount64();
}
// Advance the virtual clock by 'ms' tenths-of-thousandths... (just ms). Called from the quantum.
static inline void vclock_advance_quantum() {
    if (!vclock_on()) return;
    // LSWTCS_VCLOCK_RATE = ms advanced per quantum boundary (default 1). ~1ms / 1024 fn-entries.
    static int rate = -1; if (rate < 0) { const char* e = getenv("LSWTCS_VCLOCK_RATE"); rate = (e ? atoi(e) : 1); if (rate < 1) rate = 1; }
    g_vclock_ms.fetch_add((uint64_t)rate, std::memory_order_relaxed);
}

// Called from sub_822AE698's spin loop so the KPRCB tick counter advances in
// real time, allowing sub_822AFB88 to detect the 5000-tick timeout even while
// no VdSwap call is pending.
extern "C" void ppc_advance_kprcb_tick(uint8_t* base, uint32_t pcr) {
    uint32_t kprcb = PPC_LOAD_U32(pcr + 256);
    if (kprcb) PPC_STORE_U32(kprcb + 88, (uint32_t)(lswtcs_now_ms() * 10000ULL));
}

// Called from sub_822AE698 (the GPU ring-buffer spin-wait) before any spin check.
//
// The GPU control object layout discovered from sub_822AE698:
//   obj + 10896 (0x2A90): guest pointer to the GET-register mirror location
//   obj + 10908 (0x2A9C): current PUT value (write offset, in ring-buffer block units)
//
// Xenia reference: CommandProcessor::UpdateWritePointer / CP_RB_RPTR mirror.
// On real hardware the CP advances GET as it consumes packets; here we advance
// it to PUT immediately since no CP thread runs — the D3D12 backend handles
// actual rendering via VdSwap.
//
// Switch note: this function is pure guest-memory r/w and needs no changes for
// other platforms; only the rendering backend (gpu_d3d12.cpp) is platform-specific.
static void pm4_walk_now(void* ctxp);
static int g_imlog_left_fwd();
// sub_822AE698 is D3D's BlockOnFence(dev=r3, fence=r4): returns once (put-fence) >= (put-get), where
// put = [dev+10908] (next segment fence) and get = *[dev+10896] (last fence the GPU completed).
// LSWTCS_GETMODE: 0 = old model, get = put (claims even the segment still being recorded is done, so
// D3D re-patches shader slots in place that the deferred walk has not read yet -> wrong vfetch strides
// in the shadow pass); 1 (default) = get = max(get, fence): only the awaited fence is completed.
extern "C" void ppc_ringbuf_advance_get(uint8_t* base, uint32_t gpu_obj_addr, void* ctxp) {
    pm4_walk_now(ctxp);   // consume what was submitted before claiming the GPU caught up
    static int mode = -1; if (mode < 0) { const char* e = getenv("LSWTCS_GETMODE"); mode = e ? atoi(e) : 1; }
    uint32_t get_mirror_addr = PPC_LOAD_U32(gpu_obj_addr + 10896);
    uint32_t put_val         = PPC_LOAD_U32(gpu_obj_addr + 10908);
    if (get_mirror_addr < 0x80000000u) return;
    uint32_t get = PPC_LOAD_U32(get_mirror_addr);
    uint32_t fence = ctxp ? static_cast<PPCContext*>(ctxp)->r4.u32 : put_val;
    if (g_imlog_left_fwd() > 0) { printf("[FENCEWAIT] fence=0x%X put=0x%X get=0x%X mode=%d\n", fence, put_val, get, mode); fflush(stdout); }
    if (mode == 0 || !fence) { PPC_STORE_U32(get_mirror_addr, put_val); return; }
    // fence may be ahead of put only if garbage; clamp to put. Advance get monotonically (wrap-safe).
    if ((uint32_t)(put_val - fence) > (uint32_t)(put_val - get)) return;   // already completed
    if ((uint32_t)(fence - put_val) < 0x80000000u && fence != put_val) fence = put_val;
    PPC_STORE_U32(get_mirror_addr, fence);
}

// Called at sub_822AD6A0 entry (hook in ppc_recomp.339.cpp). That function waits
// until the GPU control block's phase word [[dev+10896]+4] reaches r5's 2-bit
// phase. Only sub_822AE698 had a mirror-advancing hook, so whether this wait ever
// completed depended on scheduling/layout luck — the POST18 device-flush stall
// (2026-07-19): sub_82314CA0 → sub_82324240 → sub_8248C860 → sub_822CFDD0 →
// sub_822ADF40/ADD38/AD6A0 spun forever after a recomp-layout change. With no real
// CP thread the consistent model (same as ppc_ringbuf_advance_get) is "GPU already
// caught up": write the requested phase before the first poll. LSWTCS_GETPHASE=0 off.
// Called at sub_822AD778 entry (hook in ppc_recomp.339.cpp). Ring-wrap wait, same
// family as sub_822AD6A0: polls the ring GET pointer [[dev+10896]+60] until it is
// outside the window [r4, (r4+r5)&mask[dev+14884]) — i.e. until the GPU has consumed
// the range about to be overwritten. No CP thread ever advances that mirror except
// the sub_822AE698 hook, so the titles-loop render maintenance (sub_82314CA0 →
// sub_82324240 → … → sub_822AE4D8 → sub_822AD778) stalled deterministically after
// the table-relocation rebuild (2026-07-19), freezing draws at 3967 while the vsync
// pump kept presenting. Model "GPU already caught up": GET = window start, which
// takes the exit branch in both the normal (start<end: exit when start>=GET) and
// wrapped (start>=end: exit when GET>=start && GET>end) window cases.
// Shares the LSWTCS_GETPHASE gate (same structural class).
extern "C" void lswtcs_ad778_get(uint8_t* base, uint32_t dev, uint32_t start, void* ctxp) {
    static int on = -1;
    if (on < 0) { const char* e = getenv("LSWTCS_GETPHASE"); on = (!e || e[0] != '0') ? 1 : 0; }
    if (!on) return;
    pm4_walk_now(ctxp);   // ring-space wait: consume the ring so the window really is free
    uint32_t mirror = PPC_LOAD_U32(dev + 10896);
    if (mirror >= 0x80000000u)   // GPU physical space (0xA0xxxxxx) — no upper bound
        PPC_STORE_U32(mirror + 60, start);
}

extern "C" void lswtcs_ad6a0_phase(uint8_t* base, uint32_t dev, uint32_t phase, void* ctxp) {
    static int on = -1;
    if (on < 0) { const char* e = getenv("LSWTCS_GETPHASE"); on = (!e || e[0] != '0') ? 1 : 0; }
    if (!on) return;
    pm4_walk_now(ctxp);   // GPU-phase wait: execute what was submitted before reporting the phase reached
    uint32_t mirror = PPC_LOAD_U32(dev + 10896);
    // The mirror block lives in GPU physical memory (0xA0xxxxxx, e.g. 0xA0214000)
    // — same validity rule as ppc_ringbuf_advance_get: any high-half guest address.
    if (mirror >= 0x80000000u)
        PPC_STORE_U32(mirror + 4, phase);
}

// ── Ring buffer state (set by VdInitializeRingBuffer / VdEnableRingBufferRPtrWriteBack)
static uint32_t g_rb_base  = 0;
static uint32_t g_rb_size  = 0;
static uint32_t g_rb_rptr_writeback = 0;
static uint32_t g_rb_rptr_block_log2 = 6;  // default: 64-dword blocks
static uint32_t g_rb_rptr_dwords = 0;      // host read pointer (dwords from g_rb_base)

// ── Graphics interrupt callback (set by VdSetGraphicsInterruptCallback)
// Called per-frame: r3=source (0=VBlank, 1=CP swap done), r4=user_data
static uint32_t g_gfx_interrupt_callback = 0;
static uint32_t g_gfx_interrupt_data     = 0;

// ── Kernel object map (guest 32-bit handle → host HANDLE) ────────────────────
// Used by all Nt* sync primitives. Guest handles in 0xF001xxxx range.
// Switch note: replace HANDLE with platform event/mutex primitives.
static std::mutex                            g_kobj_mutex;
static std::unordered_map<uint32_t, HANDLE>  g_kobj_map;
static std::atomic<uint32_t>                 g_kobj_next{0xF0010000u};

static uint32_t kobj_new(HANDLE h) {
    uint32_t id = g_kobj_next.fetch_add(1u, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(g_kobj_mutex);
    g_kobj_map[id] = h;
    return id;
}
static HANDLE kobj_get(uint32_t id) {
    std::lock_guard<std::mutex> lk(g_kobj_mutex);
    auto it = g_kobj_map.find(id);
    return (it != g_kobj_map.end()) ? it->second : nullptr;
}
static void kobj_close(uint32_t id) {
    HANDLE h = nullptr;
    { std::lock_guard<std::mutex> lk(g_kobj_mutex); auto it = g_kobj_map.find(id); if (it != g_kobj_map.end()) { h = it->second; g_kobj_map.erase(it); } }
    if (h) CloseHandle(h);
}

// ── Per-handle read-ahead cache ─────────────────────────────────────────────────
// The game reads archive files (GAME.DAT/game1.DAT) one byte per NtReadFile call
// (fgetc-style). Without buffering that's a SetFilePointer+ReadFile syscall pair
// per byte → hundreds of thousands of syscalls, unusably slow. This cache tracks a
// logical position per kobj handle and refills in 64 KB chunks, turning the 1-byte
// reads into memcpys. The OS file pointer is positioned explicitly on each refill,
// so the cached position (FileRAH::cur) is the single source of truth shared with
// host_xfile_seek; we never rely on the live OS pointer for sequential reads.
struct FileRAH {
    uint64_t             cur         = 0;   // logical current position (authoritative)
    uint64_t             cache_start = 0;   // absolute file offset of cached data
    uint32_t             cache_len   = 0;   // valid bytes in `data`
    std::vector<uint8_t> data;              // read-ahead buffer (kRahSize)
};
static std::mutex                            g_rah_mutex;
static std::unordered_map<uint32_t, FileRAH> g_rah;          // keyed by kobj fd
static const uint32_t                        kRahSize = 65536;

// Read `len` bytes for kobj `fd` into host `dst`. Position is `off` if has_off,
// else the tracked current position. Advances current. Returns bytes read.
static uint32_t host_file_read(uint32_t fd, uint8_t* dst, uint32_t len,
                               uint64_t off, bool has_off) {
    HANDLE h = kobj_get(fd);
    if (!h || h == INVALID_HANDLE_VALUE) return 0;
    std::lock_guard<std::mutex> lk(g_rah_mutex);
    FileRAH& r = g_rah[fd];
    if (r.data.empty()) r.data.resize(kRahSize);
    uint64_t rp = has_off ? off : r.cur;
    uint32_t total = 0;
    while (total < len) {
        if (!(rp >= r.cache_start && rp < r.cache_start + r.cache_len)) {
            LARGE_INTEGER li; li.QuadPart = (LONGLONG)rp;
            if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) break;
            DWORD got = 0;
            if (!ReadFile(h, r.data.data(), kRahSize, &got, nullptr) || got == 0) break;
            r.cache_start = rp; r.cache_len = got;
        }
        uint32_t avail = (uint32_t)(r.cache_start + r.cache_len - rp);
        uint32_t take  = (len - total < avail) ? (len - total) : avail;
        memcpy(dst + total, r.data.data() + (size_t)(rp - r.cache_start), take);
        total += take; rp += take;
    }
    r.cur = rp;
    extern std::atomic<uint64_t> g_file_bytes_read; g_file_bytes_read += total;  // asset-load activity probe
    return total;
}
std::atomic<uint64_t> g_file_bytes_read{0};

// Query the tracked current position for a kobj fd (for FilePositionInformation).
static bool host_file_tell(uint32_t fd, uint64_t* out_pos) {
    std::lock_guard<std::mutex> lk(g_rah_mutex);
    auto it = g_rah.find(fd);
    if (it == g_rah.end()) return false;
    if (out_pos) *out_pos = it->second.cur;
    return true;
}

// Host-backed file seek for kernel-handle fds (id >= 0xF0010000).
// The guest FATX seek path (sub_8250D0F8) only understands slot indices
// 0x8000-0x800F; files opened via NtCreateFile carry real kobj handles and
// must be seeked through the host. whence: 0=SET, 1=CUR, 2=END.
// Updates the shared tracked position (FileRAH::cur). Returns 1 on success
// (writing the resulting absolute position to *out_pos), 0 on failure. fd values
// that are not kobj handles are rejected (return -1) → caller uses FATX path.
extern "C" int host_xfile_seek(uint32_t fd, int64_t offset, uint32_t whence, uint64_t* out_pos) {
    if (fd < 0xF0010000u) return -1;                 // not a kobj handle
    HANDLE h = kobj_get(fd);
    if (!h || h == INVALID_HANDLE_VALUE) return 0;
    std::lock_guard<std::mutex> lk(g_rah_mutex);
    FileRAH& r = g_rah[fd];
    uint64_t newpos;
    if (whence == 0) {                               // SET
        newpos = (uint64_t)offset;
    } else if (whence == 1) {                        // CUR (relative to tracked pos)
        newpos = r.cur + (uint64_t)offset;
    } else {                                         // END
        LARGE_INTEGER sz; if (!GetFileSizeEx(h, &sz)) return 0;
        newpos = (uint64_t)sz.QuadPart + (uint64_t)offset;
    }
    r.cur = newpos;
    if (out_pos) *out_pos = newpos;
    return 1;
}

// ── Guest thread management ───────────────────────────────────────────────────
// Each ExCreateThread spawns a real host thread with its own PPCContext.
// Switch note: replace std::thread + Windows resume_ev with nn::os::Thread.
struct GuestThread {
    uint32_t    entry;
    uint32_t    arg;
    uint32_t    api_startup;  // XapiThreadStartup: per-thread CRT/TLS init wrapper (ExCreateThread r6)
    uint32_t    toc;
    uint32_t    stack_ptr;
    uint32_t    pcr_addr;
    HANDLE      resume_ev;   // auto-reset; signaled → thread begins executing
    std::thread host_thread;
    int         sched_id = -1;   // cooperative-scheduler id (assigned at ExCreateThread)
};
static std::mutex                                  g_thread_mutex;
static std::unordered_map<uint32_t, GuestThread*>  g_thread_map;
static uint8_t                                     g_next_cpu_id = 1;

// Cooperative scheduler hooks (defined below, near critical sections):
static thread_local int g_sched_id = -1;   // this thread's scheduler id (main=0)
static void sched_register(int id);        // register + wait for first turn
static void sched_leave(int id);           // thread exiting: drop from rotation

static void guest_thread_body(GuestThread* gt) {
    WaitForSingleObject(gt->resume_ev, INFINITE);
    PPCFunc* fn = PPC_LOOKUP_FUNC(g_base, gt->entry);
    if (!fn) {
        dbg_ram("[Thread 0x%08X] entry not in func table — exiting\n", gt->entry);

        return;
    }
    dbg_ram("[Thread 0x%08X] arg=0x%08X starting\n", gt->entry, gt->arg);

    g_sched_id = gt->sched_id;     // deterministic id assigned at ExCreateThread
    sched_register(g_sched_id);    // join the cooperative scheduler, wait for our turn
    lswtcs_set_vtable_watch();  // arm vtable watchpoint on this guest thread
    PPCContext tctx{};
    tctx.r1.u32       = gt->stack_ptr;
    tctx.r2.u32       = gt->toc;
    tctx.r13.u32      = gt->pcr_addr;
    // Seed fpscr.csr so enableFlushModeUnconditional() preserves exception masks.
    // With csr=0 (default), enableFlushModeUnconditional sets MXCSR=FlushMask only,
    // clearing the 0x1F80 exception-mask bits and causing SIGFPE on any FP exception.
    tctx.fpscr.csr    = 0x1F80u;  // all FP exceptions masked, round-to-nearest
    // If the game supplied an apiThreadStartup wrapper (XapiThreadStartup), run THAT —
    // it performs the per-thread CRT/TLS initialization the thread expects, then calls
    // entry(arg). Skipping it (calling entry directly) leaves per-thread TLS unset, which
    // is why our TLS had to be global. XapiThreadStartup(startRoutine, startContext):
    // call it with r3=entry, r4=arg.
    PPCFunc* startup = gt->api_startup ? PPC_LOOKUP_FUNC(g_base, gt->api_startup) : nullptr;
    if (startup) {
        g_thread_inited = 1;   // this thread runs its per-thread init
        dbg_ram("[Thread 0x%08X] via apiStartup 0x%08X tid=%u\n", gt->entry, gt->api_startup, (uint32_t)GetCurrentThreadId());
        tctx.r3.u32 = gt->entry;
        tctx.r4.u32 = gt->arg;
        startup(tctx, g_base);
    } else {
        tctx.r3.u32 = gt->arg;
        fn(tctx, g_base);
    }
    sched_leave(g_sched_id);   // drop out of the scheduler rotation so it doesn't wait on us
    dbg_ram("[Thread 0x%08X] returned\n", gt->entry);

}

// ── Cooperative scheduler (LSWTCS_GIL=1) ────────────────────────────────────────
// Deterministic round-robin over guest threads: exactly ONE runs at a time and, at every
// yield point, the turn is handed to the NEXT runnable thread in a FIXED order (main = id 0,
// workers numbered by ExCreateThread order). This replaces the recursive_mutex GIL whose
// OS-decided handoff left frame counts non-deterministic. Host-blocking waits mark the thread
// not-runnable so the scheduler skips it (and a blocked thread can't be handed the turn);
// cooperative waits (KeWaitForSingleObject) poll under the scheduler. The PPC_FUNC_PROLOGUE
// quantum passes the turn so pure guest spin-waits yield too. All boundaries are count-based,
// so the interleaving is reproducible run-to-run.
static int gil_on() { static int v = -1; if (v < 0) { const char* e = getenv("LSWTCS_GIL"); v = (e && e[0] != '0') ? 1 : 0; } return v; }
static constexpr int            LSW_SCHED_MAX = 64;
static std::mutex               g_sched_mtx;
static std::condition_variable  g_sched_cv;
static int   g_sched_turn  = -1;          // id whose turn it is, or -1 = idle (claimable)
static int   g_sched_count = 0;           // highest registered id + 1
static bool  g_sched_runnable[LSW_SCHED_MAX] = {};
static bool  g_sched_alive[LSW_SCHED_MAX]    = {};
static std::atomic<int> g_sched_next_id{1};   // 0 reserved for the main thread
// ── SCHEDPROF (LSWTCS_SCHEDPROF=1, diagnostic): wall time each scheduler id holds the turn, and
// time with NO runnable thread (turn == -1, everyone blocked on host waits). Dumped every 5 s.
static uint32_t g_sp_entry[LSW_SCHED_MAX];           // guest entry of each id (0 = main)
static uint64_t g_sp_acc[LSW_SCHED_MAX + 1];         // [LSW_SCHED_MAX] = idle
static uint64_t g_sp_cnt[LSW_SCHED_MAX + 1];
static uint64_t g_sp_last = 0, g_sp_lastdump = 0, g_sp_switches = 0;
static int      g_sp_cur = -1;
static int schedprof_on() { static int v = -1; if (v < 0) { const char* e = getenv("LSWTCS_SCHEDPROF"); v = (e && e[0] == '1') ? 1 : 0; } return v; }
static void sched_turn_set(int n) {   // caller holds g_sched_mtx
    if (schedprof_on() && n != g_sched_turn) {
        static LARGE_INTEGER f{}; if (!f.QuadPart) QueryPerformanceFrequency(&f);
        LARGE_INTEGER t; QueryPerformanceCounter(&t);
        uint64_t now = uint64_t(t.QuadPart);
        if (g_sp_last) { int k = g_sched_turn < 0 ? LSW_SCHED_MAX : g_sched_turn; g_sp_acc[k] += now - g_sp_last; g_sp_cnt[k]++; }
        g_sp_last = now; ++g_sp_switches;
        if (!g_sp_lastdump) g_sp_lastdump = now;
        if (now - g_sp_lastdump > uint64_t(f.QuadPart) * 5) {
            double tot = double(now - g_sp_lastdump);
            char b[1600]; int k = snprintf(b, sizeof b, "[SCHEDPROF] t=%llums switches=%llu vclock=%llums |", (unsigned long long)_kstub_start_ms(),
                                         (unsigned long long)g_sp_switches, (unsigned long long)g_vclock_ms.load());
            for (int i = 0; i <= LSW_SCHED_MAX && k < 1500; ++i) {
                if (!g_sp_acc[i]) continue;
                double pct = 100.0 * double(g_sp_acc[i]) / tot;
                double avg_us = g_sp_cnt[i] ? 1e6 * double(g_sp_acc[i]) / double(f.QuadPart) / double(g_sp_cnt[i]) : 0;
                if (i == LSW_SCHED_MAX) k += snprintf(b + k, sizeof b - k, " IDLE=%.1f%%", pct);
                else if (pct >= 0.5) k += snprintf(b + k, sizeof b - k, " %d(%08X)=%.1f%%/%.0fus", i, g_sp_entry[i], pct, avg_us);
                g_sp_acc[i] = 0; g_sp_cnt[i] = 0;
            }
            dbg_ram("%s\n", b);
            g_sp_lastdump = now; g_sp_switches = 0;
        }
    }
    g_sched_turn = n;
}

// ── KPRCB tick registry (LAYOUT-LOTTERY 2nd-stall fix, 2026-06-16) ──────────────────────────────
// sub_822AFB88 is a guest 0.5ms timed-wait (polls [KPRCB+88] for a 5000-tick=0.5ms timeout) reached
// via the sub_822AEEB0 render path. That tick was advanced ONLY from the VdSwap/VBlank dispatch and
// the sub_822AE698 GPU-spin hook. Once the VdSwap-mutex deadlock was fixed the render path advanced
// and then RE-STALLED here: cdb shows BOTH thread0 (main) and the GPU worker spinning in
// sub_822AFB88→sub_82533320, BEFORE reaching VdSwap/sub_822AE698 → [KPRCB+88] frozen → the 0.5ms
// timeout never fires → livelock (VdSwap stops climbing). This was the 2nd "layout-lottery" stall.
// FIX: advance EVERY registered KPRCB's tick from the GIL quantum, so any spin-wait sees time pass
// regardless of code path. Driven by VCLOCK now_ms ⇒ deterministic + run-to-run stable. Host-only.
static uint32_t g_kprcb_list[LSW_SCHED_MAX];
static std::atomic<int> g_kprcb_count{0};
extern "C" void lswtcs_register_kprcb(uint32_t kprcb) {
    if (!kprcb) return;
    int n = g_kprcb_count.load(std::memory_order_acquire);
    for (int i = 0; i < n; i++) if (g_kprcb_list[i] == kprcb) return;     // already present
    if (n < LSW_SCHED_MAX) { g_kprcb_list[n] = kprcb; g_kprcb_count.store(n + 1, std::memory_order_release); }
}
static int ktick_on() { static int s=-1; if(s<0){ const char* e=getenv("LSWTCS_KTICK"); s=(e&&e[0]=='0')?0:1; } return s; }
static inline void lswtcs_advance_all_kprcb_ticks() {
    if (!ktick_on()) return;
    uint8_t* base = g_base; if (!base) return;
    uint32_t ticks = (uint32_t)(lswtcs_now_ms() * 10000ULL);
    int n = g_kprcb_count.load(std::memory_order_acquire);
    for (int i = 0; i < n; i++) PPC_STORE_U32(g_kprcb_list[i] + 88, ticks);
}

// next runnable id strictly after 'from' (round-robin); -1 if none runnable anywhere.
static int sched_next_locked(int from) {
    if (g_sched_count <= 0) return -1;
    for (int i = 1; i <= g_sched_count; i++) {
        int c = (from + i) % g_sched_count;
        if (g_sched_alive[c] && g_sched_runnable[c]) return c;
    }
    return (from >= 0 && g_sched_alive[from] && g_sched_runnable[from]) ? from : -1;
}
static void sched_register(int id) {
    if (!gil_on() || id < 0) return;
    std::unique_lock<std::mutex> lk(g_sched_mtx);
    if (id >= g_sched_count) g_sched_count = id + 1;
    g_sched_alive[id] = true; g_sched_runnable[id] = true;
    if (g_sched_turn == -1) sched_turn_set(id);         // idle → claim immediately
    g_sched_cv.wait(lk, [id]{ return g_sched_turn == id; });
}
// ⭐ DETERMINISM: add a worker's scheduler slot at the GUEST's create/resume point (called by the
// CREATOR thread, which KEEPS its turn). This makes rotation MEMBERSHIP equal to the guest's create/
// resume ORDER — independent of when the worker's host std::thread actually starts. Without this, a
// worker joined the rotation only when its host thread reached sched_register (host-spawn-timing-
// dependent) → the interleaving (and thus the whole run) was build-fragile. The worker's own
// sched_register then just waits for its turn (alive flag already set, idempotent). If the round-robin
// hands the turn to a slot whose host thread hasn't reached the turn-wait yet, the system cooperatively
// waits for it (deterministic outcome, only a brief real-time delay). (Switch: a create-barrier; real
// cores still join in guest create order.) See [[switch-multithreading-constraint]].
static void sched_add_slot(int id) {
    if (!gil_on() || id < 0 || id >= LSW_SCHED_MAX) return;
    std::unique_lock<std::mutex> lk(g_sched_mtx);
    if (id >= g_sched_count) g_sched_count = id + 1;
    g_sched_alive[id] = true; g_sched_runnable[id] = true;
    if (g_sched_turn == -1) sched_turn_set(id);
    g_sched_cv.notify_all();
}
static void sched_leave(int id) {
    if (!gil_on() || id < 0) return;
    std::unique_lock<std::mutex> lk(g_sched_mtx);
    g_sched_alive[id] = false; g_sched_runnable[id] = false;
    if (g_sched_turn == id) { int n = sched_next_locked(id); sched_turn_set(n); g_sched_cv.notify_all(); }
}
// Pass the turn to the next runnable thread, then wait until it's ours again (stay runnable).
static void sched_yield_turn() {
    int id = g_sched_id; if (!gil_on() || id < 0) return;
    std::unique_lock<std::mutex> lk(g_sched_mtx);
    int n = sched_next_locked(id);
    if (n == id || n < 0) return;                        // only us runnable → nothing to do
    sched_turn_set(n); g_sched_cv.notify_all();
    g_sched_cv.wait(lk, [id]{ return g_sched_turn == id; });
}
// RAII for a host-blocking call: mark this thread off the rotation (so the scheduler hands the
// turn elsewhere while we block on a host primitive), then rejoin + reclaim the turn after.
struct GilYield {
    int id; bool active = false;
    GilYield() : id(g_sched_id) {
        if (!gil_on() || id < 0) return; active = true;
        std::unique_lock<std::mutex> lk(g_sched_mtx);
        g_sched_runnable[id] = false;
        if (g_sched_turn == id) { int n = sched_next_locked(id); sched_turn_set(n); g_sched_cv.notify_all(); }
    }
    ~GilYield() {
        if (!active) return;
        std::unique_lock<std::mutex> lk(g_sched_mtx);
        g_sched_runnable[id] = true;
        if (g_sched_turn == -1) sched_turn_set(id);
        g_sched_cv.wait(lk, [this]{ return g_sched_turn == id; });
    }
};
// Host-side blocking call from another TU (gpu_d3d12.cpp GPU fence waits): runs fn off the
// cooperative rotation so guest threads keep running while the host blocks.
// LSWTCS_PRESENTYIELD=0 keeps the turn (old behaviour, for A/B).
extern "C" void lswtcs_gil_blocking(void (*fn)(void*), void* arg) {
    static int on = -1; if (on < 0) { const char* e = getenv("LSWTCS_PRESENTYIELD"); on = (e && e[0] == '0') ? 0 : 1; }
    if (!on) { fn(arg); return; }
    GilYield y; fn(arg);
}
// ⭐ LAYOUT-LOTTERY FIX (2026-06-16): GIL-cooperative std::mutex guard. A guest-callable host
// function that serializes with a raw std::mutex (e.g. VdSwap's g_vdswap_mutex) DEADLOCKS under the
// cooperative GIL: the holder can yield the turn (quantum) while locked, and a second thread blocks on
// the OS mutex via WaitForSingleObject while STILL HOLDING the cooperative turn → neither makes
// progress (cdb proof: main thread0 = ...VdSwap→sched_yield_turn holding the mutex; worker thread11 =
// sub_822D2AB0→VdSwap→std::mutex::lock hostage). Whether the two VdSwap calls overlap this way is set
// by quantum timing, which shifts with binary layout → the render-vs-stall "layout lottery".
// FIX = acquire EXACTLY like RtlEnterCriticalSection: under GIL never block on the host primitive;
// loop {sched_yield_turn(); try_lock()} so the holder runs + releases and acquisition stays guest-
// driven (a blocked waiter is never the turn-holder). Host-only ⇒ does not perturb the lottery.
struct GilMutexGuard {
    std::mutex& m; bool locked = false;
    explicit GilMutexGuard(std::mutex& mm) : m(mm) {
        if (m.try_lock()) { locked = true; return; }   // uncontended fast path (== old lock_guard)
        if (gil_on()) {
            for (int i = 0; i < 1000000; i++) { sched_yield_turn(); if (m.try_lock()) { locked = true; return; } }
        }
        GilYield y; m.lock(); locked = true;            // no-GIL path / loop fallback: drop turn, block
    }
    ~GilMutexGuard() { if (locked) m.unlock(); }
    GilMutexGuard(const GilMutexGuard&) = delete;
    GilMutexGuard& operator=(const GilMutexGuard&) = delete;
};

// DIAG: one-line scheduler snapshot (turn holder, alive/runnable ids), called from a host thread.
extern "C" void lswtcs_sched_snapshot() {
    if (!gil_on()) return;
    char b[512]; int k;
    { std::unique_lock<std::mutex> lk(g_sched_mtx);
      k = snprintf(b, sizeof b, "[SCHEDSNAP] turn=%d count=%d alive/runnable:", g_sched_turn, g_sched_count);
      for (int i = 0; i < g_sched_count && k < 480; ++i)
          if (g_sched_alive[i]) k += snprintf(b + k, sizeof b - k, " %d%s", i, g_sched_runnable[i] ? "R" : "-"); }
    dbg_ram("%s\n", b);
}
extern "C" void lswtcs_gil_enter() {     // main thread joins as id 0 (called before _xstart)
    g_sched_id = 0; sched_register(0);
}
// Fairness: PPC_FUNC_PROLOGUE bumps this every guest-function entry and calls the quantum
// every 1024th → a fixed-count yield so pure spin-waits hand off the turn deterministically.
extern "C" unsigned lswtcs_gil_ctr = 0;
// Quantum yield for spin-waits. BUT: while a shader compile is active (g_compile_active), do NOT yield
// — let the compile run ATOMICALLY. Mid-compile yields hand the turn to the GPU/VdSwap worker, which
// then reads the half-built command/shader state (garbage) → non-deterministic hang-vs-crash AND makes
// the outcome perturbable by instrumentation. Atomic compile = deterministic + instrumentable.
// (Forward note: on the multi-core Switch port this becomes a proper lock so the GPU worker never
// observes a partially-constructed shader — see [[switch-multithreading-constraint]].) Env-escape
// LSWTCS_COMPILE_YIELD=1 restores yielding for A/B testing.
extern "C" volatile int g_compile_active;
// ── Spinlock / raised-IRQL preemption depth (LSWTCS_SPINGUARD, default ON) ──────
// Guest KfAcquireSpinLock/KeAcquireSpinLockAtRaisedIrql/KeRaiseIrqlToDpcLevel were no-ops.
// On real Xbox 360 these DISABLE PREEMPTION (raise IRQL), so the protected critical section
// runs ATOMICALLY w.r.t. other threads on that core. Under our cooperative GIL the fn-entry
// QUANTUM could preempt mid-critical-section → another guest thread enters the same "locked"
// region → corrupts shared kernel structures (the GPU worker queue / device pointer, observed
// as [r28+0]=0x5F5F5F5F → worker crash/exit → rendering dies). Track held-lock depth; while >0
// the quantum does NOT yield, so spinlock/raised-IRQL sections are atomic (matches the console).
// Layout-INDEPENDENT determinism fix. Explicit blocking waits still yield (a thread holding a
// spinlock must not wait — illegal on console — so this is safe).
// PER-THREAD depth: only the thread holding a spinlock runs atomically (its quantum is
// suppressed); OTHER threads preempt normally — so a lock-holder that yields on a wait can be
// resumed (a GLOBAL counter deadlocks: it suppresses everyone while any lock is held).
thread_local int g_spinlock_depth = 0;
static int spinguard_on() { static int s=-1; if(s<0){ const char* e=getenv("LSWTCS_SPINGUARD"); s=(e&&e[0]=='0')?0:1; } return s; }
extern "C" void lswtcs_spin_enter() { if (spinguard_on()) g_spinlock_depth++; }
extern "C" void lswtcs_spin_leave() { if (spinguard_on() && g_spinlock_depth > 0) g_spinlock_depth--; }
extern "C" void lswtcs_gil_quantum() {
    // Advance the deterministic virtual clock (no-op unless LSWTCS_VCLOCK). Done FIRST, before
    // any early-return, so guest time tracks fn-entry count purely (1024 entries / boundary).
    vclock_advance_quantum();
    // Advance all KPRCB ticks so guest timed-waits (sub_822AFB88's 0.5ms poll) progress on ANY code
    // path, not just when VdSwap/sub_822AE698 is reached. See g_kprcb_list comment above.
    lswtcs_advance_all_kprcb_ticks();
    // While THIS thread holds a spinlock / raised IRQL, run its critical section atomically
    // (don't preempt → no shared-state corruption). BUT a real spinlock section is SHORT (spans
    // ≤1 quantum boundary); if a "lock" is still held after many quantum boundaries, it's not a
    // spinlock — it's a SPIN-WAIT holding raised IRQL (e.g. the GPU worker sub_822D2798 polling a
    // fence). Suppressing its yield deadlocks (the thread that would satisfy the wait can't run).
    // So: suppress for the first few boundaries (protect short sections), then force a PERIODIC
    // yield (break the spin-wait). This makes determinism (atomic spinlocks) + liveness coexist.
    {
        static thread_local int held = 0;
        if (g_spinlock_depth > 0) {
            if (++held <= 8) return;   // short critical section: atomic
            held = 0;                  // held too long = spin-wait → fall through and yield
        } else {
            held = 0;
        }
    }
    // LSWTCS_NOQUANTUM=1: disable the fn-entry-count yield entirely. The quantum's yield POINTS
    // depend on the global fn-entry counter, whose distribution across threads is sensitive to code
    // layout → build-fragile interleaving. Switching ONLY at guest sync points (waits/CS/yields) makes
    // the schedule purely guest-driven = build-stable. Risk: a pure guest spin-wait (no kernel call)
    // would hold the turn forever; if that happens we re-enable. Test which regime the game needs.
    { static int noq = -1; if (noq < 0) { const char* e = getenv("LSWTCS_NOQUANTUM"); noq = (e && e[0] != '0') ? 1 : 0; }
      if (noq) return; }
    if (g_compile_active) {
        static int allow = -1;
        if (allow < 0) { const char* e = getenv("LSWTCS_COMPILE_YIELD"); allow = (e && e[0] != '0') ? 1 : 0; }
        if (!allow) return;   // compile runs to completion without yielding the turn
    }
    // LSWTCS_QDIV=N: only actually yield every Nth quantum boundary (default 1 = every 1024
    // guest-fn entries). Coarser quanta = fewer yield points = fewer chances for a host-event-
    // woken thread to be sampled at a timing-dependent moment → more deterministic, while still
    // providing eventual liveness for guest spin-waits. Sweep to find determinism/liveness balance.
    { static int qdiv = -1; if (qdiv < 0) { const char* e = getenv("LSWTCS_QDIV"); qdiv = (e ? atoi(e) : 1); if (qdiv < 1) qdiv = 1; }
      if (qdiv > 1) { static thread_local int qc = 0; if (++qc < qdiv) return; qc = 0; } }
    sched_yield_turn();
}

// ── Critical Sections ─────────────────────────────────────────────────────────
// Each guest RTL_CRITICAL_SECTION address maps to a real recursive_mutex.
// recursive_mutex lets the same thread re-enter (Xbox 360 CS are recursive).
static std::mutex             g_cs_registry_lock;
static std::unordered_map<uint32_t, std::recursive_mutex*> g_cs_registry;

static std::recursive_mutex* cs_get(uint32_t addr) {
    if (addr < 0x80000000u || addr >= 0x94000000u) return nullptr;
    std::lock_guard<std::mutex> lk(g_cs_registry_lock);
    auto& slot = g_cs_registry[addr];
    if (!slot) slot = new std::recursive_mutex();
    return slot;
}

static void cs_init_guest(uint32_t cs, uint8_t* base) {
    PPC_STORE_U32(cs +  0, 0x00000000);  // DebugInfo
    PPC_STORE_U32(cs +  4, 0xFFFFFFFF);  // LockCount = -1 (unlocked)
    PPC_STORE_U32(cs +  8, 0x00000000);  // RecursionCount
    PPC_STORE_U32(cs + 12, 0x00000000);  // OwningThread
    PPC_STORE_U32(cs + 16, 0x00000000);  // LockSemaphore
    PPC_STORE_U32(cs + 20, 0x00000000);  // SpinCount
}

PPC_FUNC(__imp__RtlInitializeCriticalSection) {
    PPC_FUNC_PROLOGUE();
    if (ctx.r3.u32 >= 0x82000000u && ctx.r3.u32 < 0x94000000u) {
        cs_init_guest(ctx.r3.u32, base);
        cs_get(ctx.r3.u32);  // pre-allocate mutex
    }
}
PPC_FUNC(__imp__RtlEnterCriticalSection) {
    PPC_FUNC_PROLOGUE();
    auto* m = cs_get(ctx.r3.u32);
    if (m) {
        if (m->try_lock()) return;       // uncontended: keep the turn, fast path
        if (gil_on()) {
            // Cooperative: yield the turn to the holder (so it runs + releases), retry try_lock.
            // No host m->lock() block → acquisition order is guest-driven, not host-timed.
            bool got = false;
            for (int i = 0; i < 100000; i++) { sched_yield_turn(); if (m->try_lock()) { got = true; break; } }
            if (!got) {
                // NEVER return without the lock (it used to): when the holder is parked off the
                // run queue (GilYield: FPS limiter, GilMutexGuard, host wait) the yields above are
                // no-ops and the loop exhausts in microseconds. Two D3D workers then ran the device
                // swap (sub_822B7FD8, CS dev+14928) at once and wrote back stale cursors -> negative
                // segment lengths. Block like GilMutexGuard. LSWTCS_CSUNSAFE=1 restores old behaviour.
                static int unsafe = -1; if (unsafe < 0) { const char* e = getenv("LSWTCS_CSUNSAFE"); unsafe = (e && e[0] == '1') ? 1 : 0; }
                if (!unsafe) {
                    static uint64_t nblk = 0; if (++nblk <= 10 || (nblk % 1000) == 0)
                        dbg_ram("[CS] contended CS 0x%08X: blocking acquire #%llu\n", ctx.r3.u32, (unsigned long long)nblk);
                    GilYield y; m->lock();
                }
            }
        } else {
            GilYield y;                   // (no GIL) drop turn so the holder can run + release
            m->lock();
        }
    }
}
PPC_FUNC(__imp__RtlLeaveCriticalSection) {
    PPC_FUNC_PROLOGUE();
    auto* m = cs_get(ctx.r3.u32);
    if (m) m->unlock();
}
PPC_FUNC(__imp__RtlTryEnterCriticalSection) {
    PPC_FUNC_PROLOGUE();
    auto* m = cs_get(ctx.r3.u32);
    ctx.r3.u32 = (!m || m->try_lock()) ? 1 : 0;
}
PPC_FUNC(__imp__RtlInitializeCriticalSectionAndSpinCount) {
    PPC_FUNC_PROLOGUE();
    if (ctx.r3.u32 >= 0x82000000u && ctx.r3.u32 < 0x94000000u) {
        cs_init_guest(ctx.r3.u32, base);
        cs_get(ctx.r3.u32);
    }
    ctx.r3.u32 = 1;
}

// ── Spinlocks ─────────────────────────────────────────────────────────────────
extern "C" void lswtcs_spin_enter(); extern "C" void lswtcs_spin_leave();
PPC_FUNC(__imp__KfAcquireSpinLock) { PPC_FUNC_PROLOGUE(); lswtcs_spin_enter(); ctx.r3.u32 = 0; }  // returns old IRQL
PPC_FUNC(__imp__KfReleaseSpinLock) { PPC_FUNC_PROLOGUE(); lswtcs_spin_leave(); }

// ── TLS ───────────────────────────────────────────────────────────────────────
// Per-thread (correct: KeTls* is thread-local on the 360). This is now SAFE because
// guest threads run their apiThreadStartup (XapiThreadStartup) wrapper in
// guest_thread_body, which performs the per-thread CRT/TLS init — so each thread sets
// up its own slots instead of reading another thread's (the old global workaround).
// tls_next (index allocator) stays GLOBAL — TLS indices are shared across threads.
static thread_local uint32_t tls_slots[64] = {};
static uint32_t tls_next = 1;

PPC_FUNC(__imp__KeTlsAlloc) {
    PPC_FUNC_PROLOGUE();
    if (tls_next < 64) { ctx.r3.u32 = tls_next++; }
    else ctx.r3.u32 = 0xFFFFFFFF;
}

PPC_FUNC(__imp__KeTlsFree) {
    PPC_FUNC_PROLOGUE();
    ctx.r3.u32 = 1;
}

PPC_FUNC(__imp__KeTlsGetValue) {
    PPC_FUNC_PROLOGUE();
    uint32_t idx = ctx.r3.u32;
    ctx.r3.u32 = (idx < 64) ? tls_slots[idx] : 0;
}

PPC_FUNC(__imp__KeTlsSetValue) {
    PPC_FUNC_PROLOGUE();
    uint32_t idx = ctx.r3.u32;
    if (idx < 64) tls_slots[idx] = ctx.r4.u32;
    ctx.r3.u32 = 1;
}

// ── Memory ────────────────────────────────────────────────────────────────────
PPC_FUNC(__imp__NtAllocateVirtualMemory) {
    PPC_FUNC_PROLOGUE();
    // Xbox 360: r3=*BaseAddress (in/out), r4=*RegionSize (in/out), r5=AllocationType, r6=Protect
    uint32_t base_addr_ptr = ctx.r3.u32;
    uint32_t region_size_ptr = ctx.r4.u32;
    uint32_t region_size = region_size_ptr ? PPC_LOAD_U32(region_size_ptr) : 0x10000;
    // Return error for invalid (zero or negative/huge) sizes
    if (region_size == 0 || region_size > 0x10000000) {
        ctx.r3.u32 = 0xC000000D; // STATUS_INVALID_PARAMETER
        return;
    }
    region_size = (region_size + 0xFFFF) & ~0xFFFF;
    static uint32_t guest_heap = 0x84000000;
    // Honor requested base address if specified and within valid heap range
    uint32_t requested_base = base_addr_ptr ? PPC_LOAD_U32(base_addr_ptr) : 0;
    uint32_t alloc_addr;
    if (requested_base >= 0x84000000 && requested_base < 0x94000000) {
        alloc_addr = requested_base;
        // Advance bump pointer past this allocation if needed
        if (requested_base + region_size > guest_heap)
            guest_heap = requested_base + region_size;
    } else {
        alloc_addr = guest_heap;
        guest_heap += region_size;
    }
    if (base_addr_ptr) PPC_STORE_U32(base_addr_ptr, alloc_addr);
    if (region_size_ptr) PPC_STORE_U32(region_size_ptr, region_size);
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__NtFreeVirtualMemory)       { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }

PPC_FUNC(__imp__NtQueryVirtualMemory) {
    PPC_FUNC_PROLOGUE();
    uint32_t addr = ctx.r3.u32;
    uint32_t out_ptr = ctx.r4.u32;
    if (out_ptr) {
        PPC_STORE_U32(out_ptr + 0,  addr);
        PPC_STORE_U32(out_ptr + 4,  addr);
        PPC_STORE_U32(out_ptr + 8,  0x4);
        PPC_STORE_U32(out_ptr + 12, 0x100000);
        PPC_STORE_U32(out_ptr + 16, 0x1000);
        PPC_STORE_U32(out_ptr + 20, 0x4);
        PPC_STORE_U32(out_ptr + 24, 0x20000);
    }
    ctx.r3.u32 = 0;
}

// Guest bump allocator for physical memory requests (0x88000000 upward, page-aligned).
// Limit is 0xC0000000: the game allocates a 387 MB streaming buffer first
// (0x88001000 + 0x17201000 = 0x9F202000), then 16 MB command buffer that ends at
// ~0xA0214000, plus additional pool/thread buffers. 0xC0000000 is safe (896 MB space).
// Returns 0 for allocations too large to fit — callers handle 0 gracefully.
static uint32_t   g_phys_bump = 0x88000000u;
static std::mutex g_phys_mutex;
static uint32_t g_phys_alloc(uint32_t raw_size) {
    if (raw_size == 0) return 0;
    std::lock_guard<std::mutex> lk(g_phys_mutex);
    uint32_t size = (raw_size + 0xFFFu) & ~0xFFFu;
    // Physical address = va & 0x1FFFFFFF (0x80/0xA0/0xC0000000 are aliased views of one 512 MB).
    // A block must not straddle VA 0xA0000000: its physical range would wrap 0x1FFFFFFF -> 0 and
    // the D3D runtime computes negative segment lengths (end_phys < start_phys) -> every frame's
    // main command segment got a ~2^20 length and was dropped. Skip to the boundary instead.
    // Cap at 0xA8000000: beyond that, phys >= 0x08000000 aliases our own 0x88000000 start.
    uint32_t addr = g_phys_bump;
    if (addr < 0xA0000000u && addr + size > 0xA0000000u) addr = 0xA0000000u;
    if (addr + size > 0xA8000000u) return 0;
    g_phys_bump = addr;
    g_phys_bump += size;
    return addr;
}
// LSWTCS D070PROG (host-side so the LOGIC can be iterated without reshaping the 468.cpp recomp layout that
// triggers the pre-menu lottery). Called from sub_8271D070's call site (468.cpp) after sub_827144B8 returns
// nonzero. node = the just-processed type node; pre8 = its [+8] BEFORE the call. If [+8] is UNCHANGED (no real
// progress was made on this node — the corrupt-graph reprocess), set the PROCESSED skip-flag (bit25,
// 0x02000000) that sub_826AB148 checks, so the node is skipped next scan, and return 1 = "advance the sibling
// walk instead of restarting". Else return 0 = restart (the original behavior). LSWTCS_D070PROG=0 disables.
extern "C" int lswtcs_d070_advance(uint8_t* base, uint32_t node, uint32_t pre8) {
    static int en = -1; if (en < 0) { const char* e = getenv("LSWTCS_D070PROG"); en = (e && e[0]=='0') ? 0 : 1; }
    if (!en || node < 0xA0000000u || node >= 0xF0000000u) return 0;
    uint32_t cur = __builtin_bswap32(*(volatile uint32_t*)(base + node + 8));  // guest BE
    if (cur != pre8) return 0;   // real change happened → let the loop restart (legit fixpoint progress)
    static int _n = 0; if (_n++ < 16) dbg_ram("[D070PROG] node 0x%08X unchanged -> mark done(bit25)+advance\n", node);
    *(volatile uint32_t*)(base + node + 8) = __builtin_bswap32(cur | 0x02000000u);  // set skip-flag
    return 1;
}
// Allocate and initialize a PCR block for the main (host) thread.
// The main thread is not created via ExCreateThread, so its r13 is never set up;
// sub_822AFB88 reads r13+256 → KPRCB → tick, and with r13=0 the tick is always 0
// → sub_822AE698 never times out → infinite spin whenever T10 calls it.
extern "C" uint32_t ppc_alloc_main_thread_pcr() {
    uint8_t* base = g_base;  // PPC_STORE_* macros require a local named 'base'
    uint32_t pcr = g_phys_alloc(0x1000);
    if (pcr) {
        memset(g_base + pcr, 0, 0x1000);
        PPC_STORE_U8(pcr + 268, 0);              // cpu_id = 0
        PPC_STORE_U32(pcr + 256, pcr + 0x200);  // PCR+256 → KPRCB at PCR+0x200
        PPC_STORE_U32(pcr + 0x200 + 88, (uint32_t)(lswtcs_now_ms() * 10000ULL));
        lswtcs_register_kprcb(pcr + 0x200);     // quantum advances this tick (sub_822AFB88 timeout)
        dbg_ram("[PCR] Main thread PCR=0x%08X KPRCB=0x%08X\n", pcr, pcr + 0x200);

    }
    return pcr;
}

PPC_FUNC(__imp__MmAllocatePhysicalMemory) {
    PPC_FUNC_PROLOGUE();
    uint32_t addr = g_phys_alloc(ctx.r4.u32);
    dbg_ram("[MmAllocatePhysicalMemory] size=0x%X -> 0x%08X\n", ctx.r4.u32, addr);

    ctx.r3.u32 = addr;
}
PPC_FUNC(__imp__MmAllocatePhysicalMemoryEx) {
    PPC_FUNC_PROLOGUE();
    uint32_t addr = g_phys_alloc(ctx.r4.u32);
    dbg_ram("[MmAllocatePhysicalMemoryEx] size=0x%X -> 0x%08X\n", ctx.r4.u32, addr);

    ctx.r3.u32 = addr;
}
PPC_FUNC(__imp__MmFreePhysicalMemory)       { PPC_FUNC_PROLOGUE(); }
PPC_FUNC(__imp__MmQueryAddressProtect)      { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__ExAllocatePool)             { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__ExAllocatePoolTypeWithTag)  { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__ExAllocatePoolWithTag)      { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__ExFreePool)                 { PPC_FUNC_PROLOGUE(); }

// ── File I/O ──────────────────────────────────────────────────────────────────
//
// Xbox 360 kernel structures used by Nt file calls (Xenia layout, big-endian):
//
//   X_OBJECT_ATTRIBUTES (12 bytes):
//     +0  HANDLE   RootDirectory
//     +4  PWSTR*   ObjectName  (pointer to X_UNICODE_STRING)
//     +8  ULONG    Attributes
//
//   X_UNICODE_STRING (8 bytes):
//     +0  USHORT   Length        (byte count of Buffer)
//     +2  USHORT   MaximumLength
//     +4  PWSTR    Buffer        (UTF-16 big-endian chars in guest memory)
//
// All pointer/integer fields are read via PPC_LOAD_* which handles big-endian
// byte-swapping.  File HANDLE values reuse the existing g_kobj_map table.
//
// Path translation: strip any Xbox device prefix then open relative to CWD,
// which is "Convert 360/LSWTCS/" (where the runtime is launched from).

static std::string fileio_read_path(uint32_t obj_attr_addr) {
    uint8_t* base = g_base;
    if (!obj_attr_addr) return {};
    uint32_t us_ptr = PPC_LOAD_U32(obj_attr_addr + 4);  // ObjectName pointer
    if (!us_ptr) return {};
    uint16_t len = PPC_LOAD_U16(us_ptr + 0);
    uint32_t buf = PPC_LOAD_U32(us_ptr + 4);
    if (!buf || !len) return {};

    // Detect ANSI vs UTF-16 from the first byte of the buffer:
    // UTF-16 big-endian stores 0x00 as the high byte of ASCII codepoints.
    // ANSI stores the character directly (non-zero for printable chars).
    bool is_unicode = (len >= 2) && (PPC_LOAD_U8(buf) == 0);

    std::string s;
    s.reserve(len);
    if (is_unicode) {
        for (int i = 0; i < (int)(len / 2); i++) {
            uint16_t ch = PPC_LOAD_U16(buf + i * 2);
            if (ch == 0) break;
            s += (ch < 128) ? (char)ch : '?';
        }
    } else {
        // ANSI / Latin-1: 1 byte per char
        for (int i = 0; i < (int)len; i++) {
            uint8_t ch = PPC_LOAD_U8(buf + i);
            if (ch == 0) break;
            s += (char)ch;
        }
    }
    return s;
}

static std::string fileio_translate(const std::string& xbox) {
    static const char* prefixes[] = {
        "\\Device\\Harddisk0\\Partition1\\",
        "\\Device\\Harddisk0\\Partition0\\",
        "\\Device\\CdRom0\\", "\\Device\\Cdrom0\\", "\\Device\\CDROM0\\",
        "\\Device\\Flash\\", "\\Device\\BuiltinType\\",
        "game:\\", "game:", nullptr
    };
    std::string path = xbox;
    for (int i = 0; prefixes[i]; i++) {
        size_t n = strlen(prefixes[i]);
        if (path.size() >= n && _strnicmp(path.c_str(), prefixes[i], n) == 0) {
            path = path.substr(n); break;
        }
    }
    // Xbox 360 drive-letter paths (D: = game disc, E: = HDD, etc.)
    // Only translate paths that have an actual filename component after "X:\".
    // Bare drive letters ("D:") are NOT translated — keeping the raw "D:" string
    // causes CreateFileA to fail, which is correct: disc presence check should fail
    // so the game takes its disc-unavailable code path (which works).
    if (path.size() >= 3 && isalpha((unsigned char)path[0]) && path[1] == ':'
        && (path[2] == '\\' || path[2] == '/') && path.size() > 3) {
        path = path.substr(3);  // strip "D:\" and keep the filename
    }
    for (char& c : path) if (c == '\\') c = '/';
    // Paths resolve relative to the working directory (LSWTCSRuntime/build), which only holds
    // hand-copied files (game.DAT, SMBPATH.TXT, perm*.dds). If the file isn't there but exists
    // in the real game directory, use that — e.g. MOVIES/NTSC/DEMOINTRO.WMV, which the game
    // opens right after the titles load. LSWTCS_GAMEROOT overrides the directory.
    if (!path.empty() && GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        static std::string root = [] {
            const char* e = getenv("LSWTCS_GAMEROOT");
            std::string r = e ? e : LSW_ROOT_DIR "/Convert 360/LSWTCS/";
            if (!r.empty() && r.back() != '/' && r.back() != '\\') r += '/';
            return r;
        }();
        std::string alt = root + path;
        if (GetFileAttributesA(alt.c_str()) != INVALID_FILE_ATTRIBUTES) return alt;
        // Audio streams (Audio\_Music\*.xma, cutscene audio) are loose files in the game directory,
        // not in GAME.DAT. Resolve them there even when LSWTCS_GAMEROOT points elsewhere (the
        // usual run config disables the root to keep the attract movie away).
        // LSWTCS_AUDIOROOT overrides the directory.
        if (_strnicmp(path.c_str(), "audio/", 6) == 0) {
            static std::string aroot = [] {
                const char* e = getenv("LSWTCS_AUDIOROOT");
                std::string r = e ? e : LSW_ROOT_DIR "/Convert 360/LSWTCS/";
                if (!r.empty() && r.back() != '/' && r.back() != '\\') r += '/';
                return r;
            }();
            std::string a2 = aroot + path;
            if (GetFileAttributesA(a2.c_str()) != INVALID_FILE_ATTRIBUTES) return a2;
        }
    }
    return path;
}

static uint32_t fileio_open(const char* host, bool allow_create) {
    DWORD disp = allow_create ? OPEN_ALWAYS : OPEN_EXISTING;
    // Try read-write first, fall back to read-only
    HANDLE h = CreateFileA(host, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, disp, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        h = CreateFileA(host, GENERIC_READ, FILE_SHARE_READ,
                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    // If that still fails, try as a directory (for disc/drive presence checks like D:)
    if (h == INVALID_HANDLE_VALUE)
        h = CreateFileA(host, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        nullptr, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    return kobj_new(h);
}

static uint32_t s_fileio_logged = 0;  // limit console spam

PPC_FUNC(__imp__NtClose) {
    PPC_FUNC_PROLOGUE();
    kobj_close(ctx.r3.u32);
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__NtCreateFile) {
    PPC_FUNC_PROLOGUE();
    // r3=PHANDLE, r4=Access, r5=POBJECT_ATTRIBUTES, r6=PIOSB, r10=CreateDisposition
    uint32_t hptr = ctx.r3.u32, iosb = ctx.r6.u32;

    std::string xbox = fileio_read_path(ctx.r5.u32);
    std::string host = fileio_translate(xbox);
    note_shad_open(xbox);
    if (s_fileio_logged < 64) {
        dbg_ram("[NtCreateFile] '%s' -> '%s'\n", xbox.c_str(), host.c_str());
 s_fileio_logged++;
    }

    if (hptr) PPC_STORE_U32(hptr, 0xFFFFFFFFu);  // X_INVALID_HANDLE_VALUE
    if (iosb) { PPC_STORE_U32(iosb, 0xC000000Fu); PPC_STORE_U32(iosb + 4, 0); }
    ctx.r3.u32 = 0xC000000Fu;
    if (host.empty()) return;

    bool create = (ctx.r10.u32 == 2 || ctx.r10.u32 == 3 || ctx.r10.u32 == 5);
    uint32_t gh  = fileio_open(host.c_str(), create);
    if (!gh) {
        static std::set<std::string> failed; if (failed.size() < 200 && failed.insert(xbox).second)
            dbg_ram("[NtCreateFile] FAILED '%s' (host '%s', disp=%u)\n", xbox.c_str(), host.c_str(), ctx.r10.u32);
        return;
    }

    if (hptr) PPC_STORE_U32(hptr, gh);
    if (iosb) { PPC_STORE_U32(iosb, 0); PPC_STORE_U32(iosb + 4, 1); }
    ctx.r3.u32 = 0;
    if (s_fileio_logged < 512) {
        dbg_ram("[NtCreateFile] opened handle=0x%X\n", gh);
 s_fileio_logged++;
    }
}

PPC_FUNC(__imp__NtOpenFile) {
    PPC_FUNC_PROLOGUE();
    // r3=PHANDLE, r4=Access, r5=POBJECT_ATTRIBUTES, r6=PIOSB, r7=Share, r8=Options
    uint32_t hptr = ctx.r3.u32, iosb = ctx.r6.u32;

    std::string xbox = fileio_read_path(ctx.r5.u32);
    std::string host = fileio_translate(xbox);
    note_shad_open(xbox);
    if (s_fileio_logged < 512) {
        dbg_ram("[NtOpenFile] '%s' -> '%s'\n", xbox.c_str(), host.c_str());
 s_fileio_logged++;
    }

    if (hptr) PPC_STORE_U32(hptr, 0xFFFFFFFFu);  // X_INVALID_HANDLE_VALUE
    if (iosb) { PPC_STORE_U32(iosb, 0xC000000Fu); PPC_STORE_U32(iosb + 4, 0); }
    ctx.r3.u32 = 0xC000000Fu;
    if (host.empty()) return;

    uint32_t gh = fileio_open(host.c_str(), false);
    if (!gh) { return; }

    if (hptr) PPC_STORE_U32(hptr, gh);
    if (iosb) { PPC_STORE_U32(iosb, 0); PPC_STORE_U32(iosb + 4, 1); }
    ctx.r3.u32 = 0;
    if (s_fileio_logged < 512) {
        dbg_ram("[NtOpenFile] opened handle=0x%X\n", gh);
 s_fileio_logged++;
    }
}

PPC_FUNC(__imp__NtReadFile) {
    PPC_FUNC_PROLOGUE();
    // r3=handle, r4=Event, r5=APC, r6=APCctx, r7=PIOSB,
    // r8=Buffer, r9=Length, r10=PLARGE_INTEGER ByteOffset
    { static std::atomic<uint32_t> _rn{0}; uint32_t c=++_rn;
      if (c<=40) dbg_ram("[NtReadFile] call#%u handle=0x%08X len=0x%X buf=0x%08X\n", c, ctx.r3.u32, ctx.r9.u32, ctx.r8.u32); }
    uint32_t handle  = ctx.r3.u32;
    uint32_t iosb    = ctx.r7.u32;
    uint32_t buf_ptr = ctx.r8.u32;
    uint32_t length  = ctx.r9.u32;
    uint32_t off_ptr = ctx.r10.u32;
    auto fail = [&](uint32_t st) {
        ctx.r3.u32 = st;
        if (iosb) { PPC_STORE_U32(iosb, st); PPC_STORE_U32(iosb + 4, 0); }
    };

    HANDLE h = kobj_get(handle);
    if (!h || h == INVALID_HANDLE_VALUE) {
        uint64_t off = (off_ptr && off_ptr != 0xFFFFFFFFu)
                     ? (((uint64_t)PPC_LOAD_U32(off_ptr) << 32) | PPC_LOAD_U32(off_ptr+4)) : 0;
        static int _bad=0; if (++_bad <= 40) {
            dbg_ram("[BADREAD] handle=0x%X len=0x%X off=0x%llX caller(lr)=0x%08X\n",
                   handle, length, (unsigned long long)off, (uint32_t)ctx.lr); }
        fail(0xC0000008u); return;
    }
    if (!buf_ptr || !length) { fail(0); return; }

    bool     has_off = (off_ptr && off_ptr != 0xFFFFFFFFu);
    uint64_t offset  = 0;
    if (has_off) {
        // LARGE_INTEGER: high 32 at +0, low 32 at +4 (big-endian 64-bit)
        offset = ((uint64_t)PPC_LOAD_U32(off_ptr + 0) << 32)
               |  (uint64_t)PPC_LOAD_U32(off_ptr + 4);
    }

    // Read through the per-handle read-ahead cache (64 KB refills) so the game's
    // 1-byte fgetc-style reads don't each hit a syscall.
    uint32_t bytes_read = host_file_read(handle, base + buf_ptr, length, offset, has_off);

    ctx.r3.u32 = (bytes_read > 0) ? 0u : 0xC0000011u;  // 0 or STATUS_END_OF_FILE
    if (iosb) { PPC_STORE_U32(iosb, ctx.r3.u32); PPC_STORE_U32(iosb + 4, bytes_read); }
}

PPC_FUNC(__imp__NtWriteFile) {
    PPC_FUNC_PROLOGUE();
    uint32_t handle  = ctx.r3.u32;
    uint32_t iosb    = ctx.r7.u32;
    uint32_t buf_ptr = ctx.r8.u32;
    uint32_t length  = ctx.r9.u32;
    HANDLE h = kobj_get(handle);
    if (!h || !buf_ptr || !length) { ctx.r3.u32 = 0xC0000008u; return; }
    DWORD written = 0;
    WriteFile(h, base + buf_ptr, length, &written, nullptr);
    ctx.r3.u32 = 0;
    if (iosb) { PPC_STORE_U32(iosb, 0); PPC_STORE_U32(iosb + 4, written); }
}

PPC_FUNC(__imp__NtQueryInformationFile) {
    PPC_FUNC_PROLOGUE();
    // r3=handle, r4=PIOSB, r5=FileInformation, r6=Length, r7=FileInformationClass
    uint32_t handle     = ctx.r3.u32;
    uint32_t iosb       = ctx.r4.u32;
    uint32_t info_ptr   = ctx.r5.u32;
    uint32_t info_len   = ctx.r6.u32;
    uint32_t info_class = ctx.r7.u32;
    ctx.r3.u32 = 0xC0000008u;
    if (iosb) PPC_STORE_U32(iosb, 0xC0000008u);

    HANDLE h = kobj_get(handle);
    { static std::atomic<uint32_t> _qn{0}; uint32_t c=++_qn;
      if (c<=30) dbg_ram("[NtQueryInfoFile] call#%u handle=0x%08X class=%u valid=%d\n", c, handle, info_class, h?1:0); }
    if (!h) return;

    if (info_class == 5 && info_ptr && info_len >= 22) {
        // FileStandardInformation: AllocationSize(8)+EndOfFile(8)+Links(4)+Del(1)+Dir(1)
        LARGE_INTEGER fsz = {}; GetFileSizeEx(h, &fsz);
        uint64_t sz = (uint64_t)fsz.QuadPart;
        // Write as big-endian 64-bit (high 32 at +0, low 32 at +4)
        PPC_STORE_U32(info_ptr +  0, (uint32_t)(sz >> 32));
        PPC_STORE_U32(info_ptr +  4, (uint32_t)(sz & 0xFFFFFFFF));
        PPC_STORE_U32(info_ptr +  8, (uint32_t)(sz >> 32));
        PPC_STORE_U32(info_ptr + 12, (uint32_t)(sz & 0xFFFFFFFF));
        PPC_STORE_U32(info_ptr + 16, 1);
        PPC_STORE_U8 (info_ptr + 20, 0);
        PPC_STORE_U8 (info_ptr + 21, 0);
        ctx.r3.u32 = 0;
        if (iosb) { PPC_STORE_U32(iosb, 0); PPC_STORE_U32(iosb + 4, 22); }
    } else if (info_class == 4 && info_ptr && info_len >= 40) {
        // FileBasicInformation (timestamps) — return zeros
        memset(base + info_ptr, 0, 40);
        ctx.r3.u32 = 0;
        if (iosb) { PPC_STORE_U32(iosb, 0); PPC_STORE_U32(iosb + 4, 40); }
    } else if ((info_class == 14 || info_class == 11) && info_ptr && info_len >= 8) {
        // XFilePositionInformation (Xbox class 14; old code wrongly used 11) — current
        // byte offset as big-endian 64-bit. Use the tracked FileRAH position (the OS
        // pointer isn't authoritative for our cached reads).
        uint64_t p = 0; host_file_tell(handle, &p);
        PPC_STORE_U32(info_ptr + 0, (uint32_t)(p >> 32));
        PPC_STORE_U32(info_ptr + 4, (uint32_t)(p & 0xFFFFFFFF));
        ctx.r3.u32 = 0;
        if (iosb) { PPC_STORE_U32(iosb, 0); PPC_STORE_U32(iosb + 4, 8); }
    } else if (info_class == 34 && info_ptr && info_len >= 56) {
        // FileNetworkOpenInformation (34): times(4x8) + AllocationSize(8) + EndOfFile(8) +
        // Attributes(4). The game uses this to get a file's SIZE in one call; the old stub
        // erroring out here was why GAME.DAT etc. opened but were never read.
        LARGE_INTEGER fsz = {}; GetFileSizeEx(h, &fsz);
        uint64_t sz = (uint64_t)fsz.QuadPart;
        for (int i = 0; i < 32; i += 4) PPC_STORE_U32(info_ptr + i, 0);   // 4 timestamps = 0
        PPC_STORE_U32(info_ptr + 32, (uint32_t)(sz >> 32));               // AllocationSize
        PPC_STORE_U32(info_ptr + 36, (uint32_t)(sz & 0xFFFFFFFF));
        PPC_STORE_U32(info_ptr + 40, (uint32_t)(sz >> 32));               // EndOfFile (size)
        PPC_STORE_U32(info_ptr + 44, (uint32_t)(sz & 0xFFFFFFFF));
        PPC_STORE_U32(info_ptr + 48, 0x80);                              // FILE_ATTRIBUTE_NORMAL
        ctx.r3.u32 = 0;
        if (iosb) { PPC_STORE_U32(iosb, 0); PPC_STORE_U32(iosb + 4, 56); }
    } else {
        ctx.r3.u32 = 0xC0000010u;  // STATUS_INVALID_INFO_CLASS
    }
}

PPC_FUNC(__imp__NtQueryVolumeInformationFile) {
    PPC_FUNC_PROLOGUE();
    ctx.r3.u32 = 0;
    // Return zeroed buffer — game mostly ignores volume info
    if (ctx.r5.u32 && ctx.r6.u32 && ctx.r6.u32 <= 256)
        memset(base + ctx.r5.u32, 0, ctx.r6.u32);
}

PPC_FUNC(__imp__NtQueryFullAttributesFile) {
    PPC_FUNC_PROLOGUE();
    // r3=POBJECT_ATTRIBUTES, r4=PFILE_NETWORK_OPEN_INFORMATION (56 bytes)
    std::string xbox = fileio_read_path(ctx.r3.u32);
    std::string host = fileio_translate(xbox);
    if (s_fileio_logged < 512) {
        dbg_ram("[NtQueryFullAttribs] '%s' -> '%s'\n", xbox.c_str(), host.c_str());
 s_fileio_logged++;
    }
    ctx.r3.u32 = 0xC000000Fu;
    if (host.empty()) return;
    DWORD attr = GetFileAttributesA(host.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return;
    // FILE_NETWORK_OPEN_INFORMATION — zero timestamps, fill FileAttributes (+48)
    if (ctx.r4.u32) {
        memset(base + ctx.r4.u32, 0, 56);
        PPC_STORE_U32(ctx.r4.u32 + 48,
                      (attr & FILE_ATTRIBUTE_DIRECTORY) ? 0x10u : 0x20u);
    }
    ctx.r3.u32 = 0;
}

PPC_FUNC(__imp__NtDuplicateObject)          { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }

// ── Blocking waits (LSWTCS_BLOCKWAIT, default ON; =0 = old cooperative polling) ─────────────────
// A guest thread whose wait is not satisfied leaves the scheduler rotation and blocks on the host
// (real time) instead of polling at every turn. SCHEDPROF showed ~9 waiting threads splitting the
// single run slot evenly (~47 us turns, ~20k OS context switches/s), leaving the loader ~12% of
// wall time. Guest dispatcher objects (KEVENT etc. in guest memory) have no host object: waiters
// sleep on a condition variable that KeSetEvent/KePulseEvent/KeReleaseSemaphore notify, re-checking
// every 1 ms for objects signalled by direct guest stores.
static int blockwait_on() { static int v = -1; if (v < 0) { const char* e = getenv("LSWTCS_BLOCKWAIT"); v = (e && e[0] == '0') ? 0 : 1; } return v; }
static std::mutex              g_kw_mtx;
static std::condition_variable g_kw_cv;
static uint64_t                g_kw_gen = 0;
static void kw_notify() {
    if (!blockwait_on()) return;
    { std::lock_guard<std::mutex> lk(g_kw_mtx); ++g_kw_gen; }
    g_kw_cv.notify_all();
}
// Off the rotation, wait until `ready()` (a racy read-only peek) is true or `ms` elapse. The caller
// re-checks and acquires after the turn is back.
template <typename F> static void kw_block(F ready, uint32_t ms) {
    GilYield y;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    std::unique_lock<std::mutex> lk(g_kw_mtx);
    while (!ready()) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;
        uint64_t gen = g_kw_gen;
        auto step = std::min<std::chrono::steady_clock::duration>(deadline - now, std::chrono::milliseconds(1));
        g_kw_cv.wait_for(lk, step, [&] { return g_kw_gen != gen; });
    }
}
// Guest relative timeout (LARGE_INTEGER*, 100 ns, negative = relative) -> ms; nullptr = infinite.
static uint32_t guest_timeout_ms(uint8_t* base, uint32_t p, uint32_t infinite_ms) {
    if (p < 0x80000000u || p >= 0xC0000000u) return infinite_ms;
    int64_t t = (int64_t)PPC_LOAD_U64(p);
    if (t == 0) return 0;
    if (t < 0) { uint64_t ms = uint64_t(-t) / 10000u; return ms ? uint32_t(std::min<uint64_t>(ms, 0x7FFFFFFF)) : 1u; }
    return 1;   // absolute times: treat as 'soon'
}

// ── Sync ──────────────────────────────────────────────────────────────────────
// The 360 kernel object-creation ABI is (PHANDLE r3, POBJECT_ATTRIBUTES r4, args r5, r6…).
// These stubs originally read the args one register too high (r6/r7), which made
// NtCreateSemaphore(initial=r5,max=r6) call CreateSemaphore(initial=<max>, max=<garbage r7>)
// — failing outright for the streaming workers' semaphores (returned handle 0, so their
// waits fell through and releases went nowhere). LSWTCS_SYNCABI=0 restores the old registers.
static bool lswtcs_syncabi() {
    static int v = -1;
    if (v < 0) { const char* e = getenv("LSWTCS_SYNCABI"); v = (!e || e[0] != '0') ? 1 : 0; }
    return v != 0;
}
PPC_FUNC(__imp__NtCreateEvent) {
    PPC_FUNC_PROLOGUE();
    // r3=*handle_out, r4=objattr, r5=EventType(0=Notification/manual-reset, 1=Sync/auto-reset), r6=InitialState
    // Switch: replace CreateEvent with nn::os::EventType init
    bool manual, initial;
    if (lswtcs_syncabi()) { manual = (ctx.r5.u32 == 0); initial = (ctx.r6.u32 != 0); }
    else                  { manual = (ctx.r6.u32 == 0); initial = (ctx.r7.u32 != 0); }
    HANDLE h = CreateEvent(nullptr, manual, initial, nullptr);
    if (!h) { ctx.r3.u32 = 0xC0000001u; return; }
    uint32_t id = kobj_new(h);
    if (ctx.r3.u32 >= 0x80000000u && ctx.r3.u32 < 0x94000000u) PPC_STORE_U32(ctx.r3.u32, id);
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__NtSetEvent) {
    PPC_FUNC_PROLOGUE();
    HANDLE h = kobj_get(ctx.r3.u32);
    if (h) SetEvent(h);
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__NtClearEvent) {
    PPC_FUNC_PROLOGUE();
    HANDLE h = kobj_get(ctx.r3.u32);
    if (h) ResetEvent(h);
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__NtWaitForSingleObjectEx) {
    PPC_FUNC_PROLOGUE();
    // r3=handle, r6=*timeout (int64 100ns; negative=relative from now)
    // Switch: replace with nn::os::WaitEvent / svcWaitSynchronization
    HANDLE h = kobj_get(ctx.r3.u32);
    if (h) {
        DWORD ms = INFINITE;
        if (ctx.r6.u32 >= 0x80000000u && ctx.r6.u32 < 0x94000000u) {
            int64_t t = (int64_t)PPC_LOAD_U64(ctx.r6.u32);
            if (t == 0) ms = 0;
            else if (t < 0) { uint64_t rel = (uint64_t)(-t); ms = (DWORD)(rel / 10000u); if (!ms) ms = 1; }
        }
        if (gil_on() && blockwait_on()) {
            if (ms != 0 && WaitForSingleObject(h, 0) != WAIT_OBJECT_0) { GilYield y; WaitForSingleObject(h, ms); }
        } else if (gil_on()) {
            // Cooperative wait: POLL the host object non-blocking (WAIT 0) and yield the turn between
            // checks. The poll consumes the signal when ready (auto-reset/semaphore semantics preserved),
            // but we NEVER host-block → the wait's RETURN point is determined by the round-robin (guest
            // signaling order), not host timing → build-stable determinism. budget caps a never-signaled
            // INFINITE wait (a guest deadlock) so debug runs stay bounded. (Switch: real svcWait.)
            int budget = (ms == 0) ? 1 : (ms == INFINITE ? 100000 : (int)ms * 4 + 4);
            for (int i = 0; i < budget; i++) {
                if (WaitForSingleObject(h, 0) == WAIT_OBJECT_0) break;
                if (ms == 0) break;
                sched_yield_turn();
            }
        } else {
            GilYield y;   // (no GIL) release during the host block
            WaitForSingleObject(h, ms);
        }
    }
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__NtWaitForMultipleObjectsEx) {
    PPC_FUNC_PROLOGUE();
    // r3=count, r4=*handles_array, r5=wait_type(0=all,1=any), r8=*timeout
    // Switch: replace with svcWaitSynchronizationN
    uint32_t count = ctx.r3.u32;
    if (!count || count > 64 || !ctx.r4.u32) { ctx.r3.u32 = 0; return; }
    HANDLE hs[64]; DWORD valid = 0;
    for (uint32_t i = 0; i < count; i++) {
        HANDLE h = kobj_get(PPC_LOAD_U32(ctx.r4.u32 + i * 4));
        if (h) hs[valid++] = h;
    }
    if (valid) {
        DWORD ms = INFINITE;
        if (ctx.r8.u32 >= 0x80000000u && ctx.r8.u32 < 0x94000000u) {
            int64_t t = (int64_t)PPC_LOAD_U64(ctx.r8.u32);
            if (t == 0) ms = 0;
            else if (t < 0) { uint64_t rel = (uint64_t)(-t); ms = (DWORD)(rel / 10000u); if (!ms) ms = 1; }
        }
        if (gil_on() && blockwait_on()) {
            BOOL all = (ctx.r5.u32 == 0);
            if (ms != 0 && WaitForMultipleObjects(valid, hs, all, 0) == WAIT_TIMEOUT) { GilYield y; WaitForMultipleObjects(valid, hs, all, ms); }
        } else if (gil_on()) {
            // Cooperative poll (see NtWaitForSingleObjectEx) — non-blocking WaitForMultipleObjects(0)
            // + round-robin yield, so the return point is guest-signaling-ordered, not host-timed.
            BOOL all = (ctx.r5.u32 == 0);
            int budget = (ms == 0) ? 1 : (ms == INFINITE ? 100000 : (int)ms * 4 + 4);
            for (int i = 0; i < budget; i++) {
                if (WaitForMultipleObjects(valid, hs, all, 0) != WAIT_TIMEOUT) break;
                if (ms == 0) break;
                sched_yield_turn();
            }
        } else {
            GilYield y;
            WaitForMultipleObjects(valid, hs, ctx.r5.u32 == 0, ms);
        }
    }
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__NtCreateSemaphore) {
    PPC_FUNC_PROLOGUE();
    // r3=*handle_out, r4=objattr, r5=initial_count, r6=max_count
    // Switch: replace with nn::os::SemaphoreType
    LONG init = (LONG)(lswtcs_syncabi() ? ctx.r5.u32 : ctx.r6.u32);
    uint32_t maxr = lswtcs_syncabi() ? ctx.r6.u32 : ctx.r7.u32;
    LONG maxc = maxr ? (LONG)maxr : 0x7FFFFFFF;
    if (maxc < init) maxc = init ? init : 1;
    HANDLE h = CreateSemaphore(nullptr, init, maxc, nullptr);
    if (!h) { ctx.r3.u32 = 0xC0000001u; return; }
    uint32_t id = kobj_new(h);
    if (ctx.r3.u32 >= 0x80000000u && ctx.r3.u32 < 0x94000000u) PPC_STORE_U32(ctx.r3.u32, id);
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__NtReleaseSemaphore) {
    PPC_FUNC_PROLOGUE();
    // r3=handle, r4=release_count, r5=*prev_count
    HANDLE h = kobj_get(ctx.r3.u32);
    if (h) {
        LONG prev = 0;
        ReleaseSemaphore(h, ctx.r4.u32 ? (LONG)ctx.r4.u32 : 1, &prev);
        if (ctx.r5.u32 >= 0x80000000u && ctx.r5.u32 < 0x94000000u) PPC_STORE_U32(ctx.r5.u32, (uint32_t)prev);
    }
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__NtCreateMutant) {
    PPC_FUNC_PROLOGUE();
    // r3=*handle_out, r4=objattr, r5=initial_owner
    // Switch: replace with nn::os::MutexType
    uint32_t owner = lswtcs_syncabi() ? ctx.r5.u32 : ctx.r6.u32;
    HANDLE h = CreateMutex(nullptr, owner ? TRUE : FALSE, nullptr);
    if (!h) { ctx.r3.u32 = 0xC0000001u; return; }
    uint32_t id = kobj_new(h);
    if (ctx.r3.u32 >= 0x80000000u && ctx.r3.u32 < 0x94000000u) PPC_STORE_U32(ctx.r3.u32, id);
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__NtReleaseMutant) {
    PPC_FUNC_PROLOGUE();
    HANDLE h = kobj_get(ctx.r3.u32);
    if (h) ReleaseMutex(h);
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__NtCancelTimer) {
    PPC_FUNC_PROLOGUE();
    HANDLE h = kobj_get(ctx.r3.u32);
    if (h) CancelWaitableTimer(h);
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__NtCreateTimer) {
    PPC_FUNC_PROLOGUE();
    // r3=*handle_out, r4=objattr, r5=timer_type(0=SyncTimer/auto-reset, 1=NotifTimer/manual-reset)
    // Switch: replace with nn::os::TimerEventType
    uint32_t ttype = lswtcs_syncabi() ? ctx.r5.u32 : ctx.r6.u32;
    HANDLE h = CreateWaitableTimer(nullptr, ttype != 0, nullptr);
    if (!h) { ctx.r3.u32 = 0xC0000001u; return; }
    uint32_t id = kobj_new(h);
    if (ctx.r3.u32 >= 0x80000000u && ctx.r3.u32 < 0x94000000u) PPC_STORE_U32(ctx.r3.u32, id);
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__NtSetTimerEx) {
    PPC_FUNC_PROLOGUE();
    // r3=handle, r4=*due_time (LARGE_INTEGER 100ns), r7=period_ms
    HANDLE h = kobj_get(ctx.r3.u32);
    if (h && ctx.r4.u32 >= 0x80000000u && ctx.r4.u32 < 0x94000000u) {
        LARGE_INTEGER li;
        li.QuadPart = (LONGLONG)(int64_t)PPC_LOAD_U64(ctx.r4.u32);
        SetWaitableTimer(h, &li, (LONG)ctx.r7.u32, nullptr, nullptr, FALSE);
    }
    ctx.r3.u32 = 0;
}
// Guest-pointer range for the event/wait functions below: 0x80000000-0xBFFFFFFF. The physical
// views (0xA0000000+) are valid memory; host-created guest threads (e.g. the XAudio render-driver
// client) have stacks there, and their wait arrays/events used to be rejected (instant 258).
PPC_FUNC(__imp__KeSetEvent) {
    PPC_FUNC_PROLOGUE();
    // r3=*KEVENT (DISPATCHER_HEADER+...), r4=increment, r5=wait
    // SignalState is at +4 in DISPATCHER_HEADER (big-endian int32)
    uint32_t ptr = ctx.r3.u32;
    if (ptr >= 0x83212790u && ptr < 0x832127E0u) { static int n = 0; if (n++ < 60)   // DIAG: XAudio worker sync objects
        dbg_ram("[XAEV] set 0x%08X type=%u state=%d lr=0x%08X\n", ptr, PPC_LOAD_U8(ptr), (int32_t)PPC_LOAD_U32(ptr + 4), (uint32_t)ctx.lr); }
    if (ptr >= 0x80000000u && ptr < 0xC0000000u)
        PPC_STORE_U32(ptr + 4, 1);
    kw_notify();
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__KePulseEvent) {
    PPC_FUNC_PROLOGUE();
    uint32_t ptr = ctx.r3.u32;
    if (ptr >= 0x80000000u && ptr < 0xC0000000u) {
        PPC_STORE_U32(ptr + 4, 1);
        PPC_STORE_U32(ptr + 4, 0);
    }
    kw_notify();
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__KeWaitForSingleObject) {
    PPC_FUNC_PROLOGUE();
    // r3=*KEVENT, r4=reason, r5=wait_mode, r6=alertable, r7=*timeout
    // Poll SignalState (+4) up to 16ms (one 60Hz frame period).
    // Returns 0 (WAIT_OBJECT_0) if signaled, 258 (STATUS_TIMEOUT) if not.
    // Returning 258 is essential: sub_822D2AB0 (worker) only calls VdSwap when
    // KeWaitForSingleObject returns STATUS_TIMEOUT (258), not when it returns 0.
    uint32_t ptr = ctx.r3.u32;
    bool signaled = false;
    if (ptr >= 0x80000000u && ptr < 0xC0000000u) {
        if (gil_on() && blockwait_on()) {
            signaled = PPC_LOAD_U32(ptr + 4) != 0;
            if (!signaled) {
                uint32_t ms = std::min<uint32_t>(16u, guest_timeout_ms(base, ctx.r7.u32, 16u));
                if (ms) kw_block([&] { return PPC_LOAD_U32(ptr + 4) != 0; }, ms);
                signaled = PPC_LOAD_U32(ptr + 4) != 0;
            }
        } else if (gil_on()) {
            // Cooperative poll: at OUR turn check the flag; if unset, hand the turn to other
            // guest threads (which set +4) and retry. No host Sleep → deterministic boundary.
            for (int i = 0; i < 32; i++) {
                if (PPC_LOAD_U32(ptr + 4) != 0) { signaled = true; break; }
                sched_yield_turn();
            }
        } else {
            GilYield y;   // (no GIL) release during the poll so the signaling thread can set +4
            for (int i = 0; i < 16; i++) {
                if (PPC_LOAD_U32(ptr + 4) != 0) { signaled = true; break; }
                Sleep(1);
            }
        }
        if (signaled && PPC_LOAD_U8(ptr) == 1)  // SynchronizationEvent: auto-reset
            PPC_STORE_U32(ptr + 4, 0);
    }
    if (signaled && ptr >= 0x83212790u && ptr < 0x832127E0u) { static int n = 0; if (n++ < 60)
        dbg_ram("[XAEV] wait1 0x%08X -> signaled lr=0x%08X\n", ptr, (uint32_t)ctx.lr); }
    ctx.r3.u32 = signaled ? 0u : 258u;
}
// Acquire a signaled dispatcher object (DISPATCHER_HEADER: +0 type u8, +4 SignalState be32).
// Types: 0 NotificationEvent (stays set), 1 SynchronizationEvent (auto-reset), 2 Mutant and
// 5 Semaphore (count-down), 8/9 timers (9 = synchronization, auto-reset).
static bool kobj_is_signaled(uint8_t* base, uint32_t o) { return (int32_t)PPC_LOAD_U32(o + 4) > 0; }
static void kobj_acquire(uint8_t* base, uint32_t o) {
    uint8_t type = PPC_LOAD_U8(o);
    if (type == 1 || type == 9) PPC_STORE_U32(o + 4, 0);
    else if (type == 2 || type == 5) PPC_STORE_U32(o + 4, PPC_LOAD_U32(o + 4) - 1);
}
PPC_FUNC(__imp__KeWaitForMultipleObjects) {
    PPC_FUNC_PROLOGUE();
    // r3=Count, r4=Object[] (guest ptrs), r5=WaitType (0=WaitAll, 1=WaitAny), r6=reason,
    // r7=mode, r8=alertable, r9=*timeout (0=infinite), r10=WaitBlockArray.
    // Was a stub returning 0 ("object 0 signaled") instantly. The NuSound stream workers
    // (sub_82359118, waiting via sub_827D8CD0 on {frame event, command event}) map index 0 to
    // the audio-feed path and index 1 to the command processor sub_82358F48 — so the command
    // index was never reported, queued commands (e.g. 11 "stop" cmds) were never processed,
    // and NuSoundKillAllAudioWaitEx spun forever. Same cooperative-poll policy as
    // KeWaitForSingleObject: poll at our turn, yield between polls, return STATUS_TIMEOUT
    // (258) after a bounded number of polls (callers loop). LSWTCS_KEWAITMULTI=0 = old stub.
    static int on = -1;
    if (on < 0) { const char* e = getenv("LSWTCS_KEWAITMULTI"); on = (e && e[0] == '0') ? 0 : 1; }
    if (!on) { ctx.r3.u32 = 0; return; }
    uint32_t count = ctx.r3.u32, arr = ctx.r4.u32;
    bool wait_all = (ctx.r5.u32 == 0);
    uint32_t tptr = ctx.r9.u32;
    bool poll_once = false;
    if (tptr >= 0x80000000u && tptr < 0xC0000000u && (int64_t)PPC_LOAD_U64(tptr) == 0) poll_once = true;
    if (count == 0 || count > 64 || arr < 0x80000000u || arr >= 0xC0000000u) { ctx.r3.u32 = 258u; return; }
    uint32_t objs[64];
    for (uint32_t i = 0; i < count; ++i) objs[i] = PPC_LOAD_U32(arr + i * 4);
    { static int n = 0; if (++n <= 12 || (n % 20000) == 0) {
        char b[256]; int k = snprintf(b, sizeof b, "[KWAITM] #%d count=%u all=%d lr=0x%08X:", n, count, wait_all, (uint32_t)ctx.lr);
        for (uint32_t i = 0; i < count && i < 4 && k < 220; ++i) {
            uint32_t o = objs[i];
            bool v = o >= 0x80000000u && o < 0xC0000000u;
            k += snprintf(b + k, sizeof b - k, " [%u]=0x%08X t=%u s=%d", i, o, v ? PPC_LOAD_U8(o) : 0xFF, v ? (int32_t)PPC_LOAD_U32(o + 4) : -999);
        }
        dbg_ram("%s\n", b); } }
    auto valid = [](uint32_t o) { return o >= 0x80000000u && o < 0xC0000000u; };
    if (gil_on() && blockwait_on()) {
        auto try_take = [&]() -> int {
            if (wait_all) {
                for (uint32_t i = 0; i < count; ++i) if (!valid(objs[i]) || !kobj_is_signaled(base, objs[i])) return -1;
                for (uint32_t i = 0; i < count; ++i) kobj_acquire(base, objs[i]);
                return 0;
            }
            for (uint32_t i = 0; i < count; ++i)
                if (valid(objs[i]) && kobj_is_signaled(base, objs[i])) { kobj_acquire(base, objs[i]); return int(i); }
            return -1;
        };
        auto peek = [&]() -> bool {
            if (wait_all) { for (uint32_t i = 0; i < count; ++i) if (!valid(objs[i]) || !kobj_is_signaled(base, objs[i])) return false; return true; }
            for (uint32_t i = 0; i < count; ++i) if (valid(objs[i]) && kobj_is_signaled(base, objs[i])) return true;
            return false;
        };
        int r = try_take();
        if (r < 0 && !poll_once) {
            uint32_t ms = std::min<uint32_t>(16u, guest_timeout_ms(base, tptr, 16u));
            if (ms) kw_block(peek, ms);
            r = try_take();
        }
        if (objs[0] >= 0x83212790u && objs[0] < 0x832127E0u) { static int n = 0; static uint32_t tos = 0;
            if (r < 0) ++tos; else if (n++ < 60)
                dbg_ram("[XAEV] waitN [0]=0x%08X(t=%u s=%d) -> %d after %u timeouts lr=0x%08X\n", objs[0], PPC_LOAD_U8(objs[0]), (int32_t)PPC_LOAD_U32(objs[0] + 4), r, tos, (uint32_t)ctx.lr);
            if (r < 0 && (tos % 500) == 1) dbg_ram("[XAEV] waitN [0]=0x%08X t=%u s=%d [1]=0x%08X t=%u s=%d still waiting (%u timeouts)\n",
                objs[0], PPC_LOAD_U8(objs[0]), (int32_t)PPC_LOAD_U32(objs[0] + 4), objs[1], PPC_LOAD_U8(objs[1]), (int32_t)PPC_LOAD_U32(objs[1] + 4), tos); }
        ctx.r3.u32 = r < 0 ? 258u : uint32_t(r);
        return;
    }
    const int polls = poll_once ? 1 : 32;
    for (int p = 0; p < polls; ++p) {
        if (wait_all) {
            bool all = true;
            for (uint32_t i = 0; i < count; ++i) if (!valid(objs[i]) || !kobj_is_signaled(base, objs[i])) { all = false; break; }
            if (all) { for (uint32_t i = 0; i < count; ++i) kobj_acquire(base, objs[i]); ctx.r3.u32 = 0; return; }
        } else {
            for (uint32_t i = 0; i < count; ++i)
                if (valid(objs[i]) && kobj_is_signaled(base, objs[i])) { kobj_acquire(base, objs[i]); ctx.r3.u32 = i; return; }
        }
        if (p + 1 < polls) {
            if (gil_on()) sched_yield_turn();
            else { GilYield y; Sleep(1); }
        }
    }
    ctx.r3.u32 = 258u;   // STATUS_TIMEOUT
}
PPC_FUNC(__imp__KeInsertQueueApc)           { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__KeQuerySystemTime) {
    PPC_FUNC_PROLOGUE();
    // Write current system time as 100-ns intervals since Jan 1 1601 (FILETIME format)
    uint64_t t;
    if (vclock_on()) {
        // Deterministic: fixed epoch + virtual ms (×10000 = 100-ns units). Constant base so the
        // value depends only on guest progress, not real time.
        t = 130000000000000000ULL + lswtcs_now_ms() * 10000ULL;
    } else {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    }
    if (ctx.r3.u32) PPC_STORE_U64(ctx.r3.u32, t);
}

// ── Threading ─────────────────────────────────────────────────────────────────
// Stub: write a fake handle so callers see success, but no thread actually runs.
static uint32_t g_fake_handle_counter = 0x0000C0DE;
PPC_FUNC(__imp__ExCreateThread) {
    PPC_FUNC_PROLOGUE();
    // r3=*handle_out, r4=stack_size, r5=*thread_id, r6=api_startup(ignored), r7=entry, r8=arg, r9=flags
    // flags: 1=CREATE_SUSPENDED (need KeResumeThread before thread runs)
    // Switch: replace std::thread + Windows resume_ev with nn::os::CreateThread
    uint32_t handle_ptr = ctx.r3.u32;
    uint32_t stack_size = ctx.r4.u32;
    uint32_t api_startup= ctx.r6.u32;   // XapiThreadStartup wrapper (per-thread CRT/TLS init)
    uint32_t entry      = ctx.r7.u32;
    uint32_t arg        = ctx.r8.u32;
    uint32_t flags      = ctx.r9.u32;

    uint32_t fake_handle = ++g_fake_handle_counter;
    if (handle_ptr >= 0x82000000u && handle_ptr < 0x94000000u)
        PPC_STORE_U32(handle_ptr, fake_handle);

    dbg_ram("[ExCreateThread] entry=0x%08X arg=0x%08X apiStartup=0x%08X flags=0x%X -> handle=0x%08X lr=0x%08X\n",
           entry, arg, api_startup, flags, fake_handle, (uint32_t)ctx.lr);

    // DIAGNOSTIC (LSWTCS_NOTHREADS): don't actually spawn worker threads — return a valid
    // handle so the game believes the thread was created, but it never runs. Tests whether
    // the run-to-run non-determinism is caused by host-thread races. If the game becomes
    // deterministic (or at least the parser stage is consistently reached), threading is the
    // root. The game may hang waiting on a worker; that's fine for a bounded diagnostic run.
    { static int _nt=-1; if(_nt<0){ const char* e=getenv("LSWTCS_NOTHREADS"); _nt=(e&&e[0]!='0')?1:0; }
      if(_nt){ dbg_ram("[ExCreateThread] NOTHREADS: not spawning entry=0x%08X\n", entry); ctx.r3.u32=0; return; } }


    if (entry) {
        // Guest stack: use requested size, clamp to [16KB, 256KB]
        uint32_t gs = (stack_size >= 0x4000 && stack_size <= 0x40000) ? stack_size : 0x40000;
        uint32_t stack_base = g_phys_alloc(gs);
        uint32_t sp = stack_base ? ((stack_base + gs - 0x100u) & ~0xFu) : 0u;

        // Minimal PCR: 4KB block, r13+268 = CPU_ID for interrupt callback mask
        uint32_t pcr = g_phys_alloc(0x1000);
        if (pcr) {
            memset(g_base + pcr, 0, 0x1000);
            uint8_t cpu_id;
            { std::lock_guard<std::mutex> lk(g_thread_mutex); cpu_id = g_next_cpu_id++; if (g_next_cpu_id > 5) g_next_cpu_id = 1; }
            // Creation flags bits 24-31 = hardware-thread affinity mask (Xenia: lowest set bit is the
            // thread's processor). XAudio pins its worker (sub_827DB318, flags 0x10000001 = CPU 4) and
            // rendezvous-spins in sub_827D9378 on the byte for [PCR+268]; with a round-robin id the
            // worker marked the wrong byte and spun forever (boot deadlock). LSWTCS_AFFINITY=0 = old.
            { static int aff = -1; if (aff < 0) { const char* e = getenv("LSWTCS_AFFINITY"); aff = (e && e[0] == '0') ? 0 : 1; }
              uint32_t mask = (flags >> 24) & 0x3Fu;
              if (aff && mask) cpu_id = (uint8_t)__builtin_ctz(mask); }
            PPC_STORE_U8(pcr + 268, cpu_id);
            // PCR+256 (offset 0x100) = pointer to KPRCB placed at PCR+0x200.
            // sub_822AFB88 reads: r9 = PPC_LOAD_U32(r13+256); tick = PPC_LOAD_U32(r9+88).
            // Without this, r9=0 → tick always 0 → timing check never expires → infinite spin.
            PPC_STORE_U32(pcr + 256, pcr + 0x200);
            // Seed KPRCB+88 with current wall-clock time in Xbox 360 tick units (10 MHz).
            // 1ms * 10000 ticks/ms = 10000 ticks; large initial value ensures first
            // sub_822AFB88 call sees delta = current - stored_timestamp >> 5000 → returns 0.
            PPC_STORE_U32(pcr + 0x200 + 88, (uint32_t)(lswtcs_now_ms() * 10000ULL));
            lswtcs_register_kprcb(pcr + 0x200);   // quantum advances this tick (sub_822AFB88 timeout)
        }

        GuestThread* gt = new GuestThread();
        gt->sched_id    = g_sched_next_id++;   // deterministic id (main thread assigns in order)
        gt->entry       = entry;
        if (gt->sched_id >= 0 && gt->sched_id < LSW_SCHED_MAX) g_sp_entry[gt->sched_id] = entry;
        gt->arg         = arg;
        gt->api_startup = api_startup;
        gt->toc         = ctx.r2.u32;
        gt->stack_ptr = sp;
        gt->pcr_addr  = pcr;
        gt->resume_ev = CreateEvent(nullptr, FALSE, FALSE, nullptr);

        { std::lock_guard<std::mutex> lk(g_thread_mutex); g_thread_map[fake_handle] = gt; }

        gt->host_thread = std::thread(guest_thread_body, gt);
        gt->host_thread.detach();

        if (!(flags & 1u)) {
            SetEvent(gt->resume_ev);          // start immediately if not suspended
            sched_add_slot(gt->sched_id);     // deterministic: join rotation at THIS guest point
        }
    }

    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__ExTerminateThread)          { PPC_FUNC_PROLOGUE(); }
PPC_FUNC(__imp__KeGetCurrentProcessType)    { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 1; }
PPC_FUNC(__imp__KeResumeThread) {
    PPC_FUNC_PROLOGUE();
    // r3=handle — signal the thread's resume event so it begins executing
    GuestThread* gt = nullptr;
    { std::lock_guard<std::mutex> lk(g_thread_mutex); auto it = g_thread_map.find(ctx.r3.u32); if (it != g_thread_map.end()) gt = it->second; }
    if (gt) { SetEvent(gt->resume_ev); sched_add_slot(gt->sched_id); dbg_ram("[KeResumeThread] handle=0x%08X\n", ctx.r3.u32); }
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__KeSuspendThread)            { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__KeSetAffinityThread)        { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__KeSetDisableBoostThread)    { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__KeSetBasePriorityThread)    { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }

// ── HAL ───────────────────────────────────────────────────────────────────────
PPC_FUNC(__imp__HalReturnToFirmware) {
    PPC_FUNC_PROLOGUE();
    printf("[EXIT] HalReturnToFirmware called! r3=0x%08X lr=0x%08X r4=0x%08X\n", ctx.r3.u32, (uint32_t)ctx.lr, ctx.r4.u32); fflush(stdout);
    exit(0);
}
PPC_FUNC(__imp__HalGetTickCount) {
    PPC_FUNC_PROLOGUE();
    // Returns milliseconds since game start, matching Xbox 360 HalGetTickCount semantics.
    static uint64_t start_ms = lswtcs_now_ms();
    ctx.r3.u32 = (uint32_t)(lswtcs_now_ms() - start_ms);
}

// ── Rtl ───────────────────────────────────────────────────────────────────────
PPC_FUNC(__imp__RtlImageXexHeaderField)     { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__RtlInitAnsiString) {
    PPC_FUNC_PROLOGUE();
    // r3=PANSI_STRING, r4=PCSZ (null-terminated source, or 0)
    uint32_t str_ptr = ctx.r3.u32, src_ptr = ctx.r4.u32;
    if (!str_ptr) return;
    if (!src_ptr) {
        PPC_STORE_U16(str_ptr + 0, 0);
        PPC_STORE_U16(str_ptr + 2, 0);
        PPC_STORE_U32(str_ptr + 4, 0);
        return;
    }
    uint32_t len = 0;
    while (PPC_LOAD_U8(src_ptr + len)) len++;
    PPC_STORE_U16(str_ptr + 0, (uint16_t)len);
    PPC_STORE_U16(str_ptr + 2, (uint16_t)(len + 1));
    PPC_STORE_U32(str_ptr + 4, src_ptr);
}
PPC_FUNC(__imp__RtlNtStatusToDosError)      { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__RtlTimeToTimeFields)        { PPC_FUNC_PROLOGUE(); }
PPC_FUNC(__imp__RtlUnicodeToMultiByteN)     { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__RtlUnwind)                  { PPC_FUNC_PROLOGUE(); }
PPC_FUNC(__imp__RtlMultiByteToUnicodeN)     { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }

// Silent XAM-family stubs log their first 4 calls with lr, so a flow that depends on one shows up.
#define LSW_XSTUB_LOG() do { static int _xs = 0; if (_xs < 4) { _xs++; dbg_ram("[XSTUB] %s #%d lr=0x%08X r3=0x%X r4=0x%X r5=0x%08X r6=0x%08X\n", __func__, _xs, (uint32_t)ctx.lr, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32); } } while (0)
// ── XAM ───────────────────────────────────────────────────────────────────────
PPC_FUNC(__imp__XamVoiceCreate)             { PPC_FUNC_PROLOGUE(); LSW_XSTUB_LOG(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XamVoiceClose)              { PPC_FUNC_PROLOGUE(); }
PPC_FUNC(__imp__XamVoiceSubmitPacket)       { PPC_FUNC_PROLOGUE(); LSW_XSTUB_LOG(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XamVoiceHeadsetPresent)     { PPC_FUNC_PROLOGUE(); LSW_XSTUB_LOG(); ctx.r3.u32 = 0; }
// ── XNotify listeners (Xenia xam_notify.cc, xnotifylistener.cc, kernel_state.cc) ──────────────
// Each listener is a real waitable handle (manual-reset event, signalled while its queue is
// non-empty) with its own queue; broadcasts go to every listener whose mask covers the id's
// area ((id >> 25) & 0x3F). Startup notifications match Xenia RegisterNotifyListener.
// System UI (XamShow*UI) posts XN_SYS_UI 1 on open and 0 ~100 ms after close.
// LSWTCS_NOTIFY_OLD=1 restores the old handle-0 / two-stage boot sequence.
struct LswNotification { uint32_t id, param; uint64_t due_ms; };
struct LswListener { uint32_t handle; uint64_t mask; std::deque<LswNotification> q; };
static std::mutex               g_notify_mutex;
static std::vector<LswListener> g_listeners;
static bool g_notified_startup = false, g_notified_live = false;
static bool notify_old() { static int v = -1; if (v < 0) { const char* e = getenv("LSWTCS_NOTIFY_OLD"); v = (e && e[0] == '1') ? 1 : 0; } return v != 0; }
static void notify_enqueue_locked(LswListener& l, uint32_t id, uint32_t param, uint64_t due_ms) {
    if (!(l.mask & (1ull << ((id >> 25) & 0x3F)))) return;
    l.q.push_back({id, param, due_ms});
    if (HANDLE h = kobj_get(l.handle)) SetEvent(h);
}
static void notify_broadcast(uint32_t id, uint32_t param, uint32_t delay_ms = 0) {
    uint64_t due = delay_ms ? lswtcs_now_ms() + delay_ms : 0;
    std::lock_guard<std::mutex> lk(g_notify_mutex);
    for (auto& l : g_listeners) notify_enqueue_locked(l, id, param, due);
    dbg_ram("[XNOTIFY] broadcast id=0x%08X param=%u delay=%ums listeners=%zu\n", id, param, delay_ms, g_listeners.size());
}
PPC_FUNC(__imp__XamNotifyCreateListener)    {
    PPC_FUNC_PROLOGUE();
    uint64_t mask = ctx.r3.u64;
    if (notify_old()) {
        static int n = 0; if (n < 8) { n++;
            dbg_ram("[XAM] XamNotifyCreateListener mask=0x%08X%08X -> handle 0\n", (uint32_t)(mask >> 32), (uint32_t)mask); }
        ctx.r3.u32 = 0;
        return;
    }
    uint32_t h = kobj_new(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    {
        std::lock_guard<std::mutex> lk(g_notify_mutex);
        g_listeners.push_back({h, mask, {}});
        LswListener& l = g_listeners.back();
        if (!g_notified_startup && (mask & 1)) {           // kXNotifySystem
            g_notified_startup = true;
            notify_enqueue_locked(l, 0x00000009u, 0, 0);   // XN_SYS_UI, IsUIActive() = 0
            notify_enqueue_locked(l, 0x0000000Au, 1, 0);   // XN_SYS_SIGNINCHANGED, user 0
        }
        if (!g_notified_live && (mask & 2)) {              // kXNotifyLive
            g_notified_live = true;
            notify_enqueue_locked(l, 0x02000001u, 0x001510F1u, 0);  // LIVE_CONNECTIONCHANGED: LOGON_DISCONNECTED
            notify_enqueue_locked(l, 0x02000003u, 0, 0);            // LIVE_LINK_STATE_CHANGED
        }
    }
    dbg_ram("[XAM] XamNotifyCreateListener mask=0x%08X%08X -> handle 0x%08X\n", (uint32_t)(mask >> 32), (uint32_t)mask, h);
    ctx.r3.u32 = h;
}
// XNotifyGetNext(hListener r3, match_id r4, pdwId r5, pParam r6) -> 1 if dequeued.
// match_id != 0 dequeues only that id. Notifications with a future due time stay queued.
PPC_FUNC(__imp__XNotifyGetNext) {
    PPC_FUNC_PROLOGUE();
    static std::atomic<uint32_t> calls{0};
    uint32_t n = ++calls;
    if (n <= 8 || n % 2000 == 0)
        dbg_ram("[XAM] XNotifyGetNext #%u hListener=0x%X filter=0x%08X id*=0x%08X param*=0x%08X\n",
                n, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
    if (!notify_old()) {
        uint32_t id_ptr = ctx.r5.u32, param_ptr = ctx.r6.u32, match = ctx.r4.u32;
        bool id_ok = id_ptr >= 0x80000000u && id_ptr < 0x94000000u;
        bool param_ok = param_ptr >= 0x80000000u && param_ptr < 0x94000000u;
        if (param_ok) PPC_STORE_U32(param_ptr, 0);
        if (!id_ok) { ctx.r3.u32 = 0; return; }
        PPC_STORE_U32(id_ptr, 0);
        bool got = false; uint32_t id = 0, param = 0;
        {
            std::lock_guard<std::mutex> lk(g_notify_mutex);
            for (auto& l : g_listeners) {
                if (l.handle != ctx.r3.u32) continue;
                uint64_t now = lswtcs_now_ms();
                for (auto it = l.q.begin(); it != l.q.end(); ++it) {
                    if (it->due_ms > now) continue;
                    if (match && it->id != match) continue;
                    got = true; id = it->id; param = it->param;
                    l.q.erase(it);
                    break;
                }
                if (l.q.empty()) if (HANDLE h = kobj_get(l.handle)) ResetEvent(h);
                break;
            }
        }
        if (got) {
            PPC_STORE_U32(id_ptr, id);
            if (param_ok) PPC_STORE_U32(param_ptr, param);
            dbg_ram("[XAM] XNotifyGetNext h=0x%08X delivered id=0x%08X param=0x%X (match=0x%X)\n", ctx.r3.u32, id, param, match);
        }
        ctx.r3.u32 = got ? 1u : 0u;
        return;
    }
    static int stage = 0;
    uint32_t deliver_id = 0, deliver_param = 0;
    uint32_t filter = ctx.r4.u32;
    if (stage == 0)      { deliver_id = 0x0000000Au; deliver_param = 1; }  // XN_SYS_SIGNINCHANGED, user0
    else if (stage == 1) { deliver_id = 0x00000009u; deliver_param = 0; }  // XN_SYS_UI closed
    if (deliver_id && (filter == 0 || filter == deliver_id)) {
        stage++;
        if (ctx.r5.u32 >= 0x80000000u) PPC_STORE_U32(ctx.r5.u32, deliver_id);
        if (ctx.r6.u32 >= 0x80000000u) PPC_STORE_U32(ctx.r6.u32, deliver_param);
        dbg_ram("[XAM] XNotifyGetNext delivered id=0x%08X param=%u (stage %d)\n", deliver_id, deliver_param, stage);
        ctx.r3.u32 = 1;
        return;
    }
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__XamUserGetSigninState)      {
    PPC_FUNC_PROLOGUE();
    // XamUserGetSigninState(dwUserIndex) -> XUSER_SIGNIN_STATE (0=NotSignedIn,
    // 1=SignedInLocally, 2=SignedInToLive). The old stub returned 0 for everyone, so a
    // game that gates loading on a signed-in profile would wait at the title forever.
    // Report user 0 as SignedInLocally so the title can advance to load.
    uint32_t user = ctx.r3.u32;
    static std::atomic<uint32_t> s_calls{0}; uint32_t s_n = ++s_calls;
    if (s_n <= 4 || s_n % 2000 == 0) dbg_ram("[XamSigninState] #%u user=%u -> %u\n", s_n, user, (user==0)?1u:0u);
    ctx.r3.u32 = (user == 0) ? 1u : 0u;
}
PPC_FUNC(__imp__XamUserReadProfileSettings) { PPC_FUNC_PROLOGUE(); LSW_XSTUB_LOG(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XamUserCheckPrivilege)      { PPC_FUNC_PROLOGUE(); LSW_XSTUB_LOG(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XamUserAreUsersFriends)     { PPC_FUNC_PROLOGUE(); LSW_XSTUB_LOG(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XamLoaderTerminateTitle)    { PPC_FUNC_PROLOGUE(); printf("[EXIT] XamLoaderTerminateTitle called! lr=0x%08X\n",(uint32_t)ctx.lr); fflush(stdout); exit(0); }
PPC_FUNC(__imp__XamSessionCreateHandle)     { PPC_FUNC_PROLOGUE(); LSW_XSTUB_LOG(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XamSessionRefObjByHandle)   { PPC_FUNC_PROLOGUE(); LSW_XSTUB_LOG(); ctx.r3.u32 = 0; }

// ── XMsg ──────────────────────────────────────────────────────────────────────
// XMsgStartIORequest(App r3, Message r4, XOVERLAPPED* r5, Buffer r6, cbBuffer r7).
// The old stub returned 0 (success) but never COMPLETED the overlapped — on real XAM the
// result is written into the XOVERLAPPED (+0 result, +4 length, +12 hEvent signaled) later.
// A title that polls overlapped.result for != ERROR_IO_INCOMPLETE waits FOREVER on the old
// stub — the prime suspect class for the post-mount frontend park. Complete it inline.
static void xmsg_complete_overlapped(uint8_t* base, uint32_t ovl, const char* who) {
    if (ovl < 0x80000000u || ovl >= 0x94000000u) return;
    uint32_t ev = PPC_LOAD_U32(ovl + 12);
    PPC_STORE_U32(ovl + 0, 0);   // InternalLow  = result 0 (success)
    PPC_STORE_U32(ovl + 4, 0);   // InternalHigh = length 0
    PPC_STORE_U32(ovl + 24, 0);  // dwExtendedError = 0
    bool signaled = false;
    if (ev) { HANDLE h = kobj_get(ev); if (h) { SetEvent(h); signaled = true; } }
    static int n = 0; if (n < 12) { n++;
        dbg_ram("[XAM] %s completed ovl=0x%08X (hEvent=0x%08X signaled=%d)\n", who, ovl, ev, (int)signaled); }
}
PPC_FUNC(__imp__XMsgStartIORequest) {
    PPC_FUNC_PROLOGUE();
    static int n = 0; if (n < 12) { n++;
        dbg_ram("[XAM] XMsgStartIORequest app=0x%X msg=0x%08X ovl=0x%08X buf=0x%08X len=%u\n",
                ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32); }
    xmsg_complete_overlapped(base, ctx.r5.u32, "XMsgStartIORequest");
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__XMsgCancelIORequest)        { PPC_FUNC_PROLOGUE(); LSW_XSTUB_LOG(); ctx.r3.u32 = 0; }

// Env flag read once per call site (the PM4 walker tests diagnostics flags per packet / per draw;
// a raw getenv there cost real time every frame).
#define LSW_ENV_SET(name) ([] { static const bool v = getenv(name) != nullptr; return v; }())

// ── PM4 ring buffer parser ─────────────────────────────────────────────────────
// Reads big-endian dword at ring-buffer dword index (wraps).
static inline uint32_t rb_read(uint32_t dw_idx) {
    uint32_t rb_dwords = g_rb_size >> 2;
    uint32_t idx = dw_idx % rb_dwords;
    return __builtin_bswap32(*(const volatile uint32_t*)(g_base + g_rb_base + idx * 4));
}

// Write a 32-bit value to a guest address (EOS fence completion).
static void gpu_shm_invalidate(uint32_t phys);
static inline void pm4_store32(uint32_t ga, uint32_t val) {
    gpu_shm_invalidate(ga & 0x1FFFFFFFu);
    *(volatile uint32_t*)(g_base + ga) = __builtin_bswap32(val);
}
// GPU memory write as Xenia's command processor does it (EVENT_WRITE_SHD / MEM_WRITE): the low 2
// address bits are the endian swap applied to the (host-order) value, the rest is a physical
// address; addresses above 512 MB fall in the CP writeback window (WRITEBACK_START/SIZE, mapped at
// virtual 0x7F000000 + offset).
static uint32_t* pm4_regs();
static void pm4_gpu_write(uint32_t addr_endian, uint32_t value) {
    uint32_t e = addr_endian & 3, addr = addr_endian & ~3u, v = value;
    switch (e) {
        case 1: v = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu); break;   // k8in16
        case 2: v = __builtin_bswap32(v); break;                                    // k8in32
        case 3: v = (v << 16) | (v >> 16); break;                                   // k16in32
        default: break;
    }
    uint8_t* dst;
    if (addr > 0x1FFFFFFFu) {
        uint32_t wb_base = pm4_regs()[0x0A04], wb_size = pm4_regs()[0x0A05], off = addr - wb_base;
        if (wb_base == 0 || off >= wb_size) return;
        dst = g_base + 0x7F000000ull + off;
    } else {
        dst = g_base + 0x80000000ull + addr;
        gpu_shm_invalidate(addr);
    }
    memcpy(dst, &v, 4);   // host little-endian store of the swapped value (xe::store)
}

// Read big-endian dword from flat IB guest memory.
static inline uint32_t ib_read(uint32_t guest_base, uint32_t offset) {
    return __builtin_bswap32(*(const uint32_t*)(g_base + guest_base + offset * 4));
}

// Forward-declared so pm4_process_ring can call it.
static PPCContext* g_interrupt_ctx = nullptr;
static void pm4_fire_interrupt(uint32_t cpu_mask);

// Process PM4 commands from a flat (non-circular) Indirect Buffer.
// phys_addr: GPU physical address; translate to guest by adding 0x80000000.
static void pm4_process_ib(uint32_t phys_addr, uint32_t dword_count, int depth = 0) {
    if (depth >= 4 || dword_count == 0 || dword_count > 0x10000u) return;
    uint32_t guest_base = phys_addr + 0x80000000u;
    if (guest_base < 0x80000000u || guest_base >= 0xA0000000u) return;

    static uint32_t s_ib_logged = 0;
    static uint32_t s_ib_interrupts = 0;
    uint32_t pos = 0;

    while (pos < dword_count) {
        uint32_t hdr  = ib_read(guest_base, pos);
        uint32_t type = hdr >> 30;

        if (type == 2) {
            pos++;
        } else if (type == 0) {
            uint32_t total = 1 + (hdr & 0x3FFF) + 1;
            if (pos + total > dword_count) break;
            pos += total;
        } else if (type == 3) {
            uint32_t body  = ((hdr >> 16) & 0x3FFF) + 1;
            uint32_t op    = (hdr >> 8) & 0xFF;
            uint32_t total = 1 + body;
            if (pos + total > dword_count) break;

            if (op == 0x64) {
                pos += total; break;

            } else if ((op == 0x46 || op == 0x58) && body >= 3) {
                uint32_t write_addr = ib_read(guest_base, pos + 2);
                uint32_t write_val  = ib_read(guest_base, pos + 3);
                if (write_addr >= 0x80000000u && write_addr < 0xA0000000u) {
                    pm4_store32(write_addr, write_val);
                    if (s_ib_logged < 32) {
                        dbg_ram("[IB] %s [0x%08X]=0x%08X\n",
                               op == 0x46 ? "EVENT_WRITE" : "EVENT_WRITE_SHD",
                               write_addr, write_val);
 s_ib_logged++;
                    }
                }

            } else if (op == 0x3d && body >= 1) {
                uint32_t write_addr = ib_read(guest_base, pos + 1) & ~3u;
                uint32_t data_cnt   = body - 1;
                if (write_addr >= 0x80000000u && write_addr < 0xA0000000u) {
                    for (uint32_t i = 0; i < data_cnt; i++)
                        pm4_store32(write_addr + i * 4, ib_read(guest_base, pos + 2 + i));
                    if (s_ib_logged < 32) {
                        dbg_ram("[IB] MEM_WRITE [0x%08X] x%u dwords\n", write_addr, data_cnt);
 s_ib_logged++;
                    }
                }

            } else if (op == 0x54 && body >= 1) {
                uint32_t cpu_mask = ib_read(guest_base, pos + 1);
                s_ib_interrupts++;
                dbg_ram("[IB] INTERRUPT cpu_mask=0x%02X (total=%u) -- NOT fired from IB\n",
                       cpu_mask, s_ib_interrupts);

                // Do NOT call pm4_fire_interrupt from IBs: IB interrupts are
                // GPU CP-side signals that the ring buffer interrupt covers.

            } else if ((op == 0x3f || op == 0x37) && body >= 2) {
                uint32_t nested_addr = ib_read(guest_base, pos + 1);
                uint32_t nested_len  = ib_read(guest_base, pos + 2) & 0xFFFFF;
                if (s_ib_logged < 8) {
                    dbg_ram("[IB] nested IB addr=0x%08X len=%u\n", nested_addr, nested_len);
 s_ib_logged++;
                }
                pm4_process_ib(nested_addr, nested_len, depth + 1);
            }

            pos += total;
        } else {
            pos++;
        }
    }
}

// ── PM4 reconnaissance: dump-only IB walker ─────────────────────────────────────
// Walks an Indirect Buffer (and nested IBs) and logs the command-stream structure
// WITHOUT executing anything. Used to map out what draw/state commands the game
// emits per frame before building the D3D12 translation. Bounded by g_pm4_dump_budget.
static int      g_pm4_dump_budget = 0;   // dwords-of-logging budget; set per dumped frame
static uint32_t g_pm4_draw_count  = 0;   // draws seen in the current dump

static const char* pm4_op_name(uint32_t op) {
    switch (op) {
        case 0x22: return "DRAW_INDX";
        case 0x36: return "DRAW_INDX_2";
        case 0x21: return "REG_RMW";        // NOT a draw — register read-modify-write
        case 0x2D: return "SET_CONSTANT";
        case 0x2F: return "LOAD_ALU_CONSTANT";
        case 0x2E: return "LOAD_CONSTANT_CONTEXT";
        case 0x27: return "IM_LOAD";
        case 0x2B: return "IM_LOAD_IMMEDIATE";
        case 0x2C: return "IM_STORE";
        case 0x50: return "INVALIDATE_STATE";
        case 0x3B: return "SET_BIN_MASK_LO";
        case 0x37: return "INDIRECT_BUFFER_PFD";
        case 0x3F: return "INDIRECT_BUFFER";
        case 0x46: return "EVENT_WRITE";
        case 0x58: return "EVENT_WRITE_SHD";
        case 0x3D: return "MEM_WRITE";
        case 0x54: return "INTERRUPT";
        case 0x64: return "XE_SWAP";
        case 0x00: return "ME_INIT";
        case 0x10: return "NOP";
        case 0x12: return "WAIT_REG_MEM";
        case 0x6F: return "CONTEXT_UPDATE";
        default:   return "?";
    }
}

static void pm4_dump_ib(uint32_t phys_addr, uint32_t dword_count, int depth) {
    if (depth >= 4 || dword_count == 0 || dword_count > 0x10000u) return;
    uint32_t guest_base = phys_addr + 0x80000000u;
    if (guest_base < 0x80000000u || guest_base >= 0xA0000000u) return;

    uint32_t pos = 0;
    while (pos < dword_count) {
        uint32_t hdr  = ib_read(guest_base, pos);
        uint32_t type = hdr >> 30;

        if (type == 2) { pos++; continue; }

        if (type == 0) {
            uint32_t count = ((hdr >> 16) & 0x3FFF) + 1;   // body dwords (bits[29:16])
            uint32_t reg0  = hdr & 0x7FFF;                 // base register index
            if (g_pm4_dump_budget-- > 0)
                dbg_ram("[IBDUMP%d]   T0 reg=0x%04X x%u\n", depth, reg0, count);
            pos += 1 + count;
            continue;
        }

        if (type == 3) {
            uint32_t body  = ((hdr >> 16) & 0x3FFF) + 1;
            uint32_t op    = (hdr >> 8) & 0xFF;
            uint32_t total = 1 + body;
            if (pos + total > dword_count) break;

            if (op == 0x2D && body >= 1) {              // SET_CONSTANT
                uint32_t d1   = ib_read(guest_base, pos + 1);
                uint32_t ctype= (d1 >> 16) & 0xFF;      // 0=ALU,1=fetch,2=bool,3=loop,4=reg
                uint32_t coff = d1 & 0x7FF;
                if (g_pm4_dump_budget-- > 0)
                    dbg_ram("[IBDUMP%d]   SET_CONSTANT type=%u off=0x%X x%u\n",
                           depth, ctype, coff, body - 1);
            } else if (op == 0x22 || op == 0x36 || op == 0x21) {  // DRAW_*
                uint32_t init = ib_read(guest_base, pos + (op == 0x22 ? 2 : 1));
                uint32_t prim = init & 0x3F;
                uint32_t nidx = (init >> 16) & 0xFFFF;
                g_pm4_draw_count++;
                if (g_pm4_dump_budget-- > 0)
                    dbg_ram("[IBDUMP%d]   %s prim=%u num_indices=%u (init=0x%08X)\n",
                           depth, pm4_op_name(op), prim, nidx, init);
            } else if (op == 0x3F || op == 0x37) {       // nested IB
                uint32_t na = ib_read(guest_base, pos + 1);
                uint32_t nl = ib_read(guest_base, pos + 2) & 0xFFFFF;
                if (g_pm4_dump_budget-- > 0)
                    dbg_ram("[IBDUMP%d]   -> nested IB addr=0x%08X len=%u\n", depth, na, nl);
                pm4_dump_ib(na, nl, depth + 1);
            } else {
                if (g_pm4_dump_budget-- > 0)
                    dbg_ram("[IBDUMP%d]   T3 op=0x%02X %s x%u\n", depth, op, pm4_op_name(op), body);
                if (op == 0x64) { pos += total; break; }  // XE_SWAP
            }
            pos += total;
            continue;
        }
        pos++;  // type 1 / unknown
    }
}

// ── Read-only render-state model ────────────────────────────────────────────────
// Xenos register/constant file, dword-indexed. Updated by walking IBs READ-ONLY
// (Type-0 register writes + SET_CONSTANT). Never writes guest memory, so unlike
// fence-servicing (pm4_process_ib) it cannot trip the game's GPU-completion paths.
static uint32_t g_xe_regs[0x6000] = {0};
static uint32_t* pm4_regs() { return g_xe_regs; }
uint32_t g_draw_mode_hist[8] = {0};   // histogram of RB_MODECONTROL mode at each DRAW
static uint32_t g_draw_total = 0;
static uint32_t g_resolve_total = 0;

// DIAG (2026-06-16): log the FIRST write to each render-state register (0x2000-0x23FF)
// during the IB walk, so we can tell whether the game ever sets MODECONTROL/COLOR_INFO
// in the command streams we process. If a register never appears in [REGSET], its setup
// is submitted via a stream we don't walk (system cmd buffer / device-init). Off unless
// LSWTCS_REGSET is set (cheap one-shot-per-register either way).
static uint8_t g_regset_seen[0x400] = {0};
static int g_imlog_left_fwd();
// DIAG: packet window. Opened when a rect-list draw (D3D clear) is recorded inside the imlog_now window,
// closed at the next quad-list draw; every packet the IB walker sees meanwhile is logged as [PKTWIN].
static int g_pktwin = 0;
static std::string g_pktwin_buf;     // buffered window; printed only if it ends in a 40-vertex quad list
static int g_pktwin_arm = 0;         // windows left to capture (file "pktwin_now" in the working dir arms 3)
static inline void regset_note(uint32_t reg, uint32_t val) {
    // DIAG: writes to fetch slot 0 (regs 0x4800-0x4805) while the imlog_now window is open.
    if (reg >= 0x4800 && reg < 0x4806 && g_imlog_left_fwd() > 0) {
        static int n = 0; if (n++ < 300) { printf("[FETCH0] reg=0x%04X = 0x%08X\n", reg, val); fflush(stdout); }
    }
    if (reg >= 0x2000 && reg < 0x2400 && !g_regset_seen[reg - 0x2000]) {
        g_regset_seen[reg - 0x2000] = 1;
        dbg_ram("[REGSET] reg=0x%04X = 0x%08X (draws=%u)\n", reg, val, g_draw_total);
    }
}

// Latest resolve target captured from a copy/resolve-mode draw, consumed by VdSwap
// to emulate the EDRAM→framebuffer resolve (currently: clear-color fill only).
static std::atomic<uint32_t> g_resolve_dest{0};    // guest phys of resolve destination
static std::atomic<uint32_t> g_resolve_clear{0};   // RB_COLOR_CLEAR at resolve time
static std::atomic<bool>     g_resolve_pending{false};

// Captured shader microcode loads. g_vs_addr/g_ps_addr are GUEST addresses.
static uint32_t g_vs_addr = 0, g_vs_size = 0, g_ps_addr = 0, g_ps_size = 0;
// DIAG: file "imlog_now" in the working dir -> log the next 400 VS loads ([IMLOG] packet, address,
// ucode hash, stride field of the first vfetch_full) plus each draw's index count.
static int g_imlog_left = 0;
static int g_imlog_left_fwd() { return g_imlog_left; }
static void imlog_poll() {
    static uint32_t n = 0;
    if ((++n & 1023) == 0 && GetFileAttributesA("imlog_now") != INVALID_FILE_ATTRIBUTES) { DeleteFileA("imlog_now"); g_imlog_left = 400; }
}
// DIAG LSWTCS_UCWATCH=1: write-watch on vertex-shader microcode. Every VS IM_LOAD range gets its host
// pages (physical views 0x80/0xA0/0xC0) made read-only; a vectored handler lets each write through via
// single-step and logs writes that land inside a known VS ucode range ([UCW] phys, dword index, old->new,
// writer RVA). Finds the code that patches shaders in place (e.g. vfetch strides).
struct UcwRange { uint32_t phys, bytes; };
static std::vector<UcwRange> g_ucw_ranges;
static std::vector<uintptr_t> g_ucw_pages;            // host page addresses currently protected
static thread_local uintptr_t g_ucw_pend_page = 0, g_ucw_pend_fa = 0, g_ucw_pend_rip = 0;
static thread_local uint32_t g_ucw_pend_old = 0;
static thread_local void* g_ucw_pend_bt[24]; static thread_local USHORT g_ucw_pend_nbt = 0;
static int g_ucw_logged = 0;
static int ucw_on() { static int v = -1; if (v < 0) { const char* e = getenv("LSWTCS_UCWATCH"); v = (e && e[0] == '1') ? 1 : 0; } return v; }
static bool ucw_page_watched(uintptr_t pg) { for (uintptr_t p : g_ucw_pages) if (p == pg) return true; return false; }
static LONG CALLBACK ucw_handler(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
        uintptr_t fa = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
        uintptr_t pg = fa & ~(uintptr_t)0xFFF;
        if (!g_base || !ucw_page_watched(pg)) return EXCEPTION_CONTINUE_SEARCH;
        DWORD o; VirtualProtect((void*)pg, 0x1000, PAGE_READWRITE, &o);
        g_ucw_pend_page = pg; g_ucw_pend_fa = fa; g_ucw_pend_rip = ep->ContextRecord->Rip;
        g_ucw_pend_old = *(volatile uint32_t*)(fa & ~(uintptr_t)3);
        g_ucw_pend_nbt = RtlCaptureStackBackTrace(0, 24, g_ucw_pend_bt, nullptr);
        ep->ContextRecord->EFlags |= 0x100;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == EXCEPTION_SINGLE_STEP && g_ucw_pend_page) {
        uintptr_t pg = g_ucw_pend_page, fa = g_ucw_pend_fa; g_ucw_pend_page = 0;
        uint32_t g = (uint32_t)(fa - (uintptr_t)g_base);
        uint32_t phys = g & 0x1FFFFFFFu;
        uint32_t nv = *(volatile uint32_t*)(fa & ~(uintptr_t)3);
        // Only small in-place edits of live microcode (stride-style patches): old and new non-zero and
        // differing in a few bits; at most 6 lines per writer RVA.
        uint32_t ob = __builtin_bswap32(g_ucw_pend_old), nb = __builtin_bswap32(nv);
        bool patchlike = ob && nb && ob != nb && __builtin_popcount(ob ^ nb) <= 12;
        static std::unordered_map<uintptr_t, int> per_rva;
        for (const UcwRange& r : g_ucw_ranges) {
            if (patchlike && phys >= r.phys && phys < r.phys + r.bytes && g_ucw_logged < 400 && per_rva[g_ucw_pend_rip]++ < 6) {
                ++g_ucw_logged;
                printf("[UCW] write g=0x%08X ucode@0x%08X dw%u  %08X -> %08X  RVA=0x%08llX tid=%lu\n", g & ~3u, r.phys,
                       (phys - r.phys) / 4, __builtin_bswap32(g_ucw_pend_old), __builtin_bswap32(nv),
                       (unsigned long long)(g_ucw_pend_rip - (uintptr_t)GetModuleHandle(NULL)), (unsigned long)GetCurrentThreadId());
                { uintptr_t ib = (uintptr_t)GetModuleHandle(NULL); printf("[UCW]   bt:");
                  for (USHORT i = 0; i < g_ucw_pend_nbt; ++i) { uintptr_t a = (uintptr_t)g_ucw_pend_bt[i];
                      if (a >= ib && a < ib + 0x10000000) printf(" 0x%llX", (unsigned long long)(a - ib + 0x140000000ull)); }
                  printf("\n"); }
                fflush(stdout);
                break;
            }
        }
        DWORD o; VirtualProtect((void*)pg, 0x1000, PAGE_READONLY, &o);
        ep->ContextRecord->EFlags &= ~0x100u;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
static void ucw_add(uint32_t gaddr, uint32_t dwords) {
    if (!ucw_on() || !g_base) return;
    uint32_t phys = gaddr & 0x1FFFFFFFu, bytes = dwords * 4;
    static uint32_t minp = 0xFFFFFFFFu; if (minp == 0xFFFFFFFFu) { const char* e = getenv("LSWTCS_UCWATCH_MIN"); minp = e ? (uint32_t)strtoul(e, nullptr, 0) : 0x0F000000u; }
    if (phys < minp) return;   // keep clear of image/heap pages that kernel-mode I/O writes into
    for (const UcwRange& r : g_ucw_ranges) if (r.phys == phys) return;
    if (g_ucw_ranges.size() >= 256) return;
    static bool reg = false; if (!reg) { reg = true; AddVectoredExceptionHandler(1, ucw_handler); printf("[UCW] armed\n"); }
    g_ucw_ranges.push_back({phys, bytes});
    static const uint32_t kViews[3] = {0x80000000u, 0xA0000000u, 0xC0000000u};
    for (uint32_t v : kViews)
        for (uintptr_t pg = ((uintptr_t)g_base + v + phys) & ~(uintptr_t)0xFFF; pg < (uintptr_t)g_base + v + phys + bytes; pg += 0x1000) {
            if (ucw_page_watched(pg)) continue;
            DWORD o; if (VirtualProtect((void*)pg, 0x1000, PAGE_READONLY, &o)) g_ucw_pages.push_back(pg);
        }
}
// DIAG: D3D vertex-shader slot reuse check (hook in sub_822B64D0, ppc_recomp.342.cpp). Logged while the
// imlog_now window is open: slot fence vs D3D PUT [dev+10908], GET mirror *[dev+10896], our walk
// position and the kicked CP_RB_WPTR.
extern "C" void lswtcs_slotchk(uint8_t* base, uint32_t dev, uint32_t slot, uint32_t fence, uint32_t idx, uint32_t shader) {
    if (g_imlog_left <= 0) return;
    uint32_t mirror = PPC_LOAD_U32(dev + 10896), put = PPC_LOAD_U32(dev + 10908);
    uint32_t get = mirror >= 0x80000000u ? PPC_LOAD_U32(mirror) : 0xFFFFFFFFu;
    bool busy = fence && (uint32_t)(put - fence) < (uint32_t)(put - get);
    printf("[SLOTCHK] shader=0x%08X slot#%u @0x%08X fence=0x%X put=0x%X get=0x%X walk=0x%X wptr=0x%X -> %s\n", shader, idx, slot,
           fence, put, get, g_rb_rptr_dwords, PPC_LOAD_U32(0x7FC80714u), fence ? (busy ? "BUSY (new slot)" : "reuse") : "no fence (reuse)");
    fflush(stdout);
}
// DIAG: entry of D3D's draw-time shader flush sub_822B6610 (hook in ppc_recomp.342.cpp), logged while the
// imlog_now window is open: dirty flags (r4; bit 0x00100000 = emit VS IM_LOAD), current VS object
// [dev+12688] and PS object [dev+12684], caller.
extern "C" void lswtcs_vsflush(uint8_t* base, uint32_t dev, uint64_t flags, uint32_t lr) {
    if (g_imlog_left <= 0) return;
    printf("[VSFLUSH] flags=0x%016llX vs=0x%08X ps=0x%08X lr=0x%08X\n", (unsigned long long)flags,
           PPC_LOAD_U32(dev + 12688), PPC_LOAD_U32(dev + 12684), lr);
    fflush(stdout);
}
// DIAG: D3D SetPixelShader (kind 0, sub_822BC780 -> dev+12684) / SetVertexShader (kind 1, sub_822BCA80 ->
// dev+12688) entries (hooks in ppc_recomp.344.cpp), logged while the imlog_now window is open.
extern "C" void lswtcs_setshader(uint8_t* base, int kind, uint32_t dev, uint32_t shader, uint32_t lr) {
    if (g_imlog_left <= 0) return;
    printf("[SETSH] %s=0x%08X lr=0x%08X\n", kind ? "VS" : "PS", shader, lr);
    fflush(stdout);
}
// DIAG: file "ucfind_now" -> scan guest physical memory for the microcode of shaders Xenia uses on the
// title but we never translate (dumped from Xenia: 2D sprite VS/PS etc.). Prints every address found.
struct UcfSig { const char* name; uint32_t n; uint32_t w[40]; };
static const UcfSig kUcfSigs[] = {
    {"VS_sprite", 30, {0x70153003u, 0x00001200u, 0xC2000000u, 0x00001006u, 0x00001200u, 0xC4000000u, 0x00002007u, 0x00002200u, 0x00000000u, 0x2DF82000u, 0x00393A88u, 0x00000006u, 0x05F81000u, 0x4006060Au, 0x00000306u, 0x05F80000u, 0x40253FC8u, 0x00000406u, 0xC80F803Eu, 0x00000000u, 0xC2020200u, 0xC8038000u, 0x00B0B000u, 0x80003500u, 0xC90F8001u, 0x00000000u, 0x81012800u, 0x00000000u, 0x00000000u, 0x00000000u}},
    {"PS_sprite", 27, {0x00011002u, 0x00001200u, 0xC4000000u, 0x00005003u, 0x00002200u, 0x00000000u, 0x10080001u, 0x1F1FF443u, 0x00004000u, 0xC8010102u, 0x001B6C00u, 0x01272C00u, 0xC80F0101u, 0x00FFFF00u, 0xC0010100u, 0xC8010100u, 0x006C6C00u, 0xC1020000u, 0xC8078000u, 0x00151500u, 0xC1010000u, 0xC8088000u, 0x006C6C00u, 0xC1000100u, 0x00000000u, 0x00000000u, 0x00000000u}},
    {"PS_textmrt", 33, {0x00011002u, 0x00001200u, 0xC4000000u, 0x00006003u, 0x10091200u, 0x22000000u, 0x10080001u, 0x1F1FF688u, 0x00004000u, 0xC8020201u, 0x001B6C00u, 0x01272C00u, 0xC80F0202u, 0x00000000u, 0xC0020200u, 0xC88FC001u, 0x006C6C00u, 0x02FFFF00u, 0xC8070202u, 0x01C0C0C0u, 0xCB02002Fu, 0xC8010200u, 0x00B11B00u, 0xC1010000u, 0xC8088000u, 0x006C1B00u, 0xC1000200u, 0xC8078000u, 0x00C06CC0u, 0xCB02012Fu, 0x00000000u, 0x00000000u, 0x00000000u}},
    {"VS_quad2d", 24, {0x30052003u, 0x00001200u, 0xC2000000u, 0x00001005u, 0x00001200u, 0xC4000000u, 0x00001006u, 0x00002200u, 0x00000000u, 0x1DF81000u, 0x00393A88u, 0x00000006u, 0x05F80000u, 0x4006060Au, 0x00000306u, 0xC80F803Eu, 0x00000000u, 0xC2010100u, 0xC90F8000u, 0x00000000u, 0x81002800u, 0x00000000u, 0x00000000u, 0x00000000u}},
};
static void ucfind_poll() {
    static uint32_t c = 0;
    if ((++c & 1023) != 0 || GetFileAttributesA("ucfind_now") == INVALID_FILE_ATTRIBUTES) return;
    DeleteFileA("ucfind_now");
    for (const UcfSig& s : kUcfSigs) {
        int hits = 0;
        for (uint32_t a = 0; a + s.n * 4 <= 0x20000000u && hits < 16; a += 4) {
            const uint8_t* p = g_base + 0x80000000ull + a;
            if (__builtin_bswap32(*(const uint32_t*)p) != s.w[0]) continue;
            bool ok = true;
            for (uint32_t i = 1; i < s.n && ok; ++i) ok = __builtin_bswap32(*(const uint32_t*)(p + i * 4)) == s.w[i];
            if (ok) { ++hits; printf("[UCFIND] %s found at guest 0x%08X\n", s.name, 0x80000000u + a); }
        }
        printf("[UCFIND] %s: %d hit(s)\n", s.name, hits);
    }
    fflush(stdout);
}
static void imlog_vs(const char* kind, uint32_t addr, uint32_t size) {
    imlog_poll();
    ucfind_poll();
    if (g_imlog_left <= 0 || addr < 0x80000000u || addr >= 0xA0000000u || !size || size >= 0x4000) return;
    --g_imlog_left;
    const uint8_t* p = g_base + addr; uint64_t h = 1469598103934665603ull;
    for (uint32_t i = 0; i < size * 4; ++i) h = (h ^ p[i]) * 1099511628211ull;
    printf("[IMLOG] %s VS addr=0x%08X size=%u hash=%016llX\n", kind, addr, size, (unsigned long long)h); fflush(stdout);
}
static uint32_t g_imload27 = 0, g_imload28 = 0;   // counts of IM_LOAD / IM_LOAD_IMMEDIATE
static int      g_draws_captured = 0;

// ── Geometry executor (LSWTCS_GEOM) ────────────────────────────────────────────
// Collect mode-4 rect-list UI quads as clip-space triangles for the D3D12 side to
// draw. The UI VS is a passthrough (decoded from Xenia disasm): POSITION float3 @off0,
// COLOR float4 @off3dw, stride 7 dwords; oPos = screen-space position (no matrix).
// Each emitted vertex = {x,y,z, r,g,b,a} (7 floats, clip space). Double-buffered:
// filled during the ring walk, consumed by gpu_d3d12_present (same VdSwap).
static constexpr uint32_t GEOM_MAX_VERTS = 300000;   // ~50k quads/frame cap
float    g_geom_verts[GEOM_MAX_VERTS * 7];
uint32_t g_geom_vert_count = 0;                       // verts staged this frame (build)
uint32_t g_geom_vert_ready = 0;                       // verts ready for present (snapshot)
float    g_geom_present_verts[GEOM_MAX_VERTS * 7];    // published frame (what gpu_geom_replay draws)
// Per-draw depth state as batches over the vertex stream: {first vertex, RB_DEPTHCONTROL bits}
// (z_enable 0x2, z_write 0x4, zfunc 0x70). The replay draws each batch with a matching PSO.
struct GeomBatch { uint32_t first; uint32_t dctl; };
static constexpr uint32_t GEOM_MAX_BATCHES = 8192;
static GeomBatch g_geom_batches[GEOM_MAX_BATCHES];
static uint32_t  g_geom_batch_count = 0;
GeomBatch g_geom_present_batches[GEOM_MAX_BATCHES];
uint32_t  g_geom_present_batch_count = 0;
uint32_t  g_geom_present_depth_clear = 0;           // RB_DEPTH_CLEAR at publish
// Frame-boundary publish. The game's draw IBs are NOT aligned with VdSwap calls, so a
// per-VdSwap window usually holds no (or half a) scene -> ships freeze between the rare
// windows that caught a full frame. Instead publish exactly when the command stream
// reaches the PM4_XE_SWAP packet (signature 'SWAP') that VdSwap itself wrote: everything
// drawn since the previous swap packet IS one game frame. LSWTCS_GEOMSWAPSYNC=0 reverts.
static uint64_t g_geom_swaps = 0;
static int      g_geom_swapsync = -1;
static void geom_set_dctl(uint32_t dctl) {
    if (g_geom_batch_count && g_geom_batches[g_geom_batch_count - 1].dctl == dctl) return;
    if (g_geom_batch_count && g_geom_batches[g_geom_batch_count - 1].first == g_geom_vert_count) {
        g_geom_batches[g_geom_batch_count - 1].dctl = dctl; return; }
    if (g_geom_batch_count < GEOM_MAX_BATCHES) g_geom_batches[g_geom_batch_count++] = { g_geom_vert_count, dctl };
}
static void geom_reset_build() { g_geom_vert_count = 0; g_geom_batch_count = 0; }
std::mutex g_geom_present_mtx;   // published GEOM batches: written here, drawn by the render thread
static void geom_publish() {
    std::lock_guard<std::mutex> lk(g_geom_present_mtx);
    memcpy(g_geom_present_batches, g_geom_batches, g_geom_batch_count * sizeof(GeomBatch));
    g_geom_present_batch_count = g_geom_batch_count;
    g_geom_present_depth_clear = g_xe_regs[0x231D];
    g_geom_vert_ready = g_geom_vert_count;
    memcpy(g_geom_present_verts, g_geom_verts, (size_t)g_geom_vert_count * 7u * sizeof(float));
}
#include "lsw_gpu_frame.h"
static bool gpudraw_on();
// ── FRAMEMAP (LSWTCS_FRAMEMAP=1, diagnostic): structure of one guest frame every 600 swaps (max 8).
// Run-length list of draws by (EDRAM mode, RB_SURFACE_INFO, RB_COLOR_INFO, RB_DEPTH_INFO, which
// earlier resolve destinations any type-2 texture fetch constant points at); every resolve (mode 6)
// is its own entry with copy control / dest base / pitch / info / window scissor.
struct FmRun { uint32_t mode, surf, color, depth, samples, n; uint32_t x[8]; };
static std::vector<FmRun> g_fm_runs;
static std::vector<uint32_t> g_fm_dests;
static bool g_fm_active = false;
static uint32_t g_fm_logged = 0;
static uint64_t g_fm_swap = 0;
static bool framemap_on() { static int v = -1; if (v < 0) { const char* e = getenv("LSWTCS_FRAMEMAP"); v = (e && e[0] == '1') ? 1 : 0; } return v != 0; }
static void framemap_event(uint32_t mode) {
    if (!g_fm_active) return;
    FmRun r{mode, g_xe_regs[0x2000], g_xe_regs[0x2001], g_xe_regs[0x2002], 0, 1, {}};
    if (mode == 6) {
        r.x[0] = g_xe_regs[0x2318]; r.x[1] = g_xe_regs[0x2319]; r.x[2] = g_xe_regs[0x231A]; r.x[3] = g_xe_regs[0x231B];
        r.x[4] = g_xe_regs[0x2081]; r.x[5] = g_xe_regs[0x2082]; r.x[6] = g_xe_regs[0x2080]; r.x[7] = g_xe_regs[0x231E];
        g_fm_dests.push_back(g_xe_regs[0x2319] & 0x1FFFF000u);
        g_fm_runs.push_back(r);
        return;
    }
    for (uint32_t i = 0; i < 32; ++i) {
        uint32_t dw0 = g_xe_regs[0x4800 + i * 6], dw1 = g_xe_regs[0x4801 + i * 6];
        if ((dw0 & 3) != 2) continue;
        uint32_t b = dw1 & 0x1FFFF000u;
        for (size_t k = 0; k < g_fm_dests.size() && k < 32; ++k) if (b && b == g_fm_dests[k]) r.samples |= 1u << k;
    }
    if (!g_fm_runs.empty()) {
        FmRun& l = g_fm_runs.back();
        if (l.mode == r.mode && l.surf == r.surf && l.color == r.color && l.depth == r.depth && l.samples == r.samples) { ++l.n; return; }
    }
    g_fm_runs.push_back(r);
}
static void framemap_swap(uint32_t swap_fb) {
    if (!framemap_on()) return;
    ++g_fm_swap;
    if (g_fm_active) {
        g_fm_active = false;
        uint32_t ri = 0, total = 0;
        for (auto& r : g_fm_runs) total += r.n;
        dbg_ram("[FRAMEMAP] ==== frame at swap %llu (swap fb 0x%08X): %zu runs, %u events ====\n", (unsigned long long)g_fm_swap, swap_fb, g_fm_runs.size(), total);
        for (size_t i = 0; i < g_fm_runs.size() && i < 400; ++i) {
            const FmRun& r = g_fm_runs[i];
            if (r.mode == 6)
                dbg_ram("[FRAMEMAP]  R%-2u RESOLVE surf pitch=%u msaa=%u | col base=%u fmt=%u | dep base=%u | ctl=0x%08X dest=0x%08X dpitch=0x%08X dinfo=0x%08X wsc=0x%08X..0x%08X woff=0x%08X clr=0x%08X\n",
                        ri++, r.surf & 0x3FFF, (r.surf >> 16) & 3, r.color & 0xFFF, (r.color >> 16) & 0xF, r.depth & 0xFFF,
                        r.x[0], r.x[1], r.x[2], r.x[3], r.x[4], r.x[5], r.x[6], r.x[7]);
            else
                dbg_ram("[FRAMEMAP]  %4u x mode=%u surf pitch=%u msaa=%u | col base=%u fmt=%u | dep base=%u fmt=%u | samplesR=0x%X\n",
                        r.n, r.mode, r.surf & 0x3FFF, (r.surf >> 16) & 3, r.color & 0xFFF, (r.color >> 16) & 0xF,
                        r.depth & 0xFFF, (r.depth >> 16) & 1, r.samples);
        }
        ++g_fm_logged;
    }
    static uint64_t start = ~0ull; if (start == ~0ull) { const char* e = getenv("LSWTCS_FRAMEMAP_START"); start = e ? strtoull(e, nullptr, 10) : 0; }
    static uint64_t every = 0; if (!every) { const char* e = getenv("LSWTCS_FRAMEMAP_EVERY"); every = e ? std::max<uint64_t>(1, strtoull(e, nullptr, 10)) : 600; }
    if (g_fm_logged < 8 && g_fm_swap >= start && (g_fm_swap % every) == 0) { g_fm_active = true; g_fm_runs.clear(); g_fm_dests.clear(); }
}
// BUFWATCH (LSWTCS_BUFWATCH="0xA,0xB,..." physical addresses, diagnostic): every 120 swaps,
// fingerprint the first 64 KB of each buffer and log when it changes (who fills the hub's
// screen buffers: CPU writes show up as changes, GPU resolves we never execute do not).
static void bufwatch_swap() {
    static std::vector<uint32_t> addrs; static std::vector<uint64_t> last; static int init = 0; static uint64_t sw = 0;
    if (!init) {
        init = 1;
        if (const char* e = getenv("LSWTCS_BUFWATCH")) {
            const char* p = e;
            while (*p) { char* end; uint32_t a = (uint32_t)strtoul(p, &end, 0); if (end == p) break; addrs.push_back(a & 0x1FFFFFFFu); p = end; while (*p == ',') ++p; }
        }
        last.assign(addrs.size(), 0);
    }
    if (addrs.empty() || (++sw % 120) != 0) return;
    for (size_t i = 0; i < addrs.size(); ++i) {
        const uint8_t* p = g_base + 0x80000000ull + addrs[i];
        uint64_t h = 1469598103934665603ull, nz = 0;
        for (uint32_t o = 0; o < 65536; o += 8) { uint64_t v; memcpy(&v, p + o, 8); h = (h ^ v) * 1099511628211ull; nz += v != 0; }
        if (h != last[i]) {
            uint32_t w0, w1; memcpy(&w0, p, 4); memcpy(&w1, p + 4096, 4);
            dbg_ram("[BUFWATCH] swap %llu buf 0x%08X CHANGED hash=%016llX nonzero_qwords=%llu/8192 w0=%08X w4k=%08X\n",
                    (unsigned long long)sw, addrs[i], (unsigned long long)h, (unsigned long long)nz, __builtin_bswap32(w0), __builtin_bswap32(w1));
            last[i] = h;
        }
    }
}
uint64_t g_geom_swaps_pub = 0;   // swap packets seen (all walkers)
static void gpu_shm_carry_uploads(const LswGpuFrame& from, LswGpuFrame& to);
static void geom_swap_packet(uint32_t swap_fb) {
    ++g_geom_swaps_pub;
    framemap_swap(swap_fb);
    bufwatch_swap();
    if (g_geom_swapsync < 0) { const char* e = getenv("LSWTCS_GEOMSWAPSYNC"); g_geom_swapsync = (e && e[0] == '0') ? 0 : 1; }
    if (!g_geom_swapsync) return;
    g_geom_swaps++;
    { extern uint32_t g_gpu_offscreen_drops, g_gpu_offscreen_pitch; static uint64_t ns = 0;
      if ((++ns % 300) == 0) dbg_ram("[FRAMEDIAG] swap#%llu recorded=%zu offscreen_dropped=%u (last surf=0x%08X)\n",
                                     (unsigned long long)ns, g_gpu_frame_build.draws.size(), g_gpu_offscreen_drops, g_gpu_offscreen_pitch);
      g_gpu_offscreen_drops = 0; }
    if (gpudraw_on() && !g_gpu_frame_build.draws.empty()) {
        g_gpu_frame_build.swap_fb = swap_fb;
        std::lock_guard<std::mutex> lk(g_gpu_frame_mtx);   // the render thread takes present under it
        if (g_gpu_frame_ready) gpu_shm_carry_uploads(g_gpu_frame_present, g_gpu_frame_build);
        std::swap(g_gpu_frame_build, g_gpu_frame_present); g_gpu_frame_build.clear(); g_gpu_frame_ready = true; }
    if (g_geom_vert_count == 0) return;   // duplicate swap marker with nothing drawn: keep last frame
    geom_publish();
    geom_reset_build();
}
static uint32_t g_geom_flat = 0, g_geom_tex = 0, g_geom_skip = 0;   // per-frame draw tallies
static int g_geom_on = -1;                            // env cache: LSWTCS_GEOM
static int g_geom_dbg = -1;                           // env cache: LSWTCS_GEOMDBG (force colors)

static inline float ib_readf(uint32_t vb, uint32_t i) {
    uint32_t u = ib_read(vb, i); float f; __builtin_memcpy(&f, &u, 4); return f;
}
// Emit one clip-space vertex into the geom buffer.
// The replay pipeline blends PREMULTIPLIED (ONE, INV_SRC_ALPHA) so opaque, alpha-blended and
// additive draws share one PSO: opaque=(rgb,1), alpha=(rgb*a,a), additive=(rgb,0).
static inline void geom_emit_pm(float x, float y, float z, float r, float g, float b, float a) {
    if (g_geom_vert_count >= GEOM_MAX_VERTS) return;
    float* v = &g_geom_verts[g_geom_vert_count * 7];
    v[0]=x; v[1]=y; v[2]=z; v[3]=r; v[4]=g; v[5]=b; v[6]=a;
    g_geom_vert_count++;
}
// Straight-alpha emit (UI quads etc.): premultiply here.
static inline void geom_emit(float x, float y, float z, float r, float g, float b, float a) {
    geom_emit_pm(x, y, z, r * a, g * a, b * a, a);
}
// Decoded vertex layout for a UI draw (from the VS's vfetch instructions).
struct VtxFmt { uint32_t stride; int pos_off; int col_off; int col_fmt; int uv_off; int uv_fmt; };

// Decode a UI VS's vertex layout by scanning its 3-dword fetch instructions.
// VertexFetchInstruction (xenia ucode.h): dword2 = stride:8 | offset:23 | pred:1;
// dword1 format @bit16 (6b); is_mini_fetch @bit30. Position = full fetch, fmt
// k_32_32_32_FLOAT(57), offset 0 → gives the stride; secondary attrs are mini fetches.
static bool geom_decode_vs(uint32_t vs, uint32_t vs_size, VtxFmt* o) {
    o->stride = 0; o->pos_off = 0; o->col_off = -1; o->col_fmt = -1; o->uv_off = -1; o->uv_fmt = -1;
    if (vs < 0x82000000u || vs >= 0xA0000000u || vs_size < 6) return false;
    uint32_t slots = vs_size / 3; bool found = false;
    for (uint32_t s = 0; s < slots; s++) {
        uint32_t d1 = ib_read(vs, s*3+1), d2 = ib_read(vs, s*3+2);
        uint32_t fmt = (d1 >> 16) & 0x3F, off = (d2 >> 8) & 0x7FFFFF, strd = d2 & 0xFF;
        bool mini = (d1 >> 30) & 1;
        if (!found) {                                  // looking for the position fetch
            if (!mini && fmt == 57 && off == 0 && strd >= 3 && strd <= 64) { o->stride = strd; found = true; }
            continue;
        }
        if (off >= o->stride) continue;                // attr offset must be within the vertex
        // color: 8_8_8_8(6) packed or 32_32_32_32_FLOAT(38)
        if (o->col_off < 0 && (fmt == 6 || fmt == 38)) { o->col_off = off; o->col_fmt = fmt; }
        // uv: 32_32_FLOAT(37) or 16_16_FLOAT(31)
        if (o->uv_off  < 0 && (fmt == 37 || fmt == 31)) { o->uv_off = off; o->uv_fmt = fmt; }
    }
    return found;
}

// Collect one mode-4 rect-list draw (3 verts) → 2 clip-space triangles, using the
// per-draw decoded vertex format. vs_addr/vs_size = the draw's bound VS microcode.
static void geom_collect_draw(uint32_t vb, uint32_t num_indices, uint32_t vs_addr, uint32_t vs_size) {
    geom_set_dctl(0);   // UI quads: no depth
    if (g_geom_on < 0) { g_geom_on  = getenv("LSWTCS_GEOM")    ? 1 : 0;
                         g_geom_dbg = getenv("LSWTCS_GEOMDBG") ? 1 : 0; }
    if (!g_geom_on || num_indices < 3) return;
    static int s_gd = -1; if (s_gd < 0) s_gd = getenv("LSWTCS_GEOMDIAG") ? 1 : 0;
    static int s_gdn = 0;
    if (vb < 0x82000000u || vb >= 0xA0000000u) {
        if (s_gd && s_gdn < 24) { s_gdn++; dbg_ram("[GEOMDIAG#%d] REJECT vb-range vb=0x%08X nidx=%u VS=0x%08X/%u\n", s_gdn, vb, num_indices, vs_addr, vs_size); }
        return;
    }
    VtxFmt vf;
    if (!geom_decode_vs(vs_addr, vs_size, &vf)) {
        if (s_gd && s_gdn < 24) { s_gdn++; dbg_ram("[GEOMDIAG#%d] REJECT vs-decode vb=0x%08X nidx=%u VS=0x%08X/%u vs[0..2]=%08X %08X %08X\n",
            s_gdn, vb, num_indices, vs_addr, vs_size, ib_read(vs_addr,0), ib_read(vs_addr,1), ib_read(vs_addr,2)); }
        g_geom_skip++; return;   // unknown VS shape
    }
    if (s_gd && s_gdn < 24) { s_gdn++; dbg_ram("[GEOMDIAG#%d] DECODE ok stride=%u pos_off=%u col_off=%d(fmt%d) uv_off=%d(fmt%d) vb=0x%08X p0=(%.1f,%.1f) p1=(%.1f,%.1f) p2=(%.1f,%.1f)\n",
        s_gdn, vf.stride, vf.pos_off, vf.col_off, vf.col_fmt, vf.uv_off, vf.uv_fmt, vb,
        ib_readf(vb, 0*vf.stride+vf.pos_off+0), ib_readf(vb, 0*vf.stride+vf.pos_off+1),
        ib_readf(vb, 1*vf.stride+vf.pos_off+0), ib_readf(vb, 1*vf.stride+vf.pos_off+1),
        ib_readf(vb, 2*vf.stride+vf.pos_off+0), ib_readf(vb, 2*vf.stride+vf.pos_off+1)); }
    if (vf.uv_off >= 0) g_geom_tex++; else g_geom_flat++;
    uint32_t st = vf.stride;
    float px[3], py[3], cr[3][4];
    // FRONTIER-2 DIAG: log the per-vertex color of the first few quads so we can tell whether the
    // menu is drawn in real flat colors (present dropping it) or genuinely black (no content).
    if (s_gd && s_gdn <= 24 && vf.col_fmt == 38) {
        dbg_ram("[GEOMCOL] vb=0x%08X col_off=%d v0_rgba=(%.2f,%.2f,%.2f,%.2f) v1=(%.2f,%.2f,%.2f,%.2f)\n",
            vb, vf.col_off,
            ib_readf(vb, vf.col_off+0), ib_readf(vb, vf.col_off+1), ib_readf(vb, vf.col_off+2), ib_readf(vb, vf.col_off+3),
            ib_readf(vb, st+vf.col_off+0), ib_readf(vb, st+vf.col_off+1), ib_readf(vb, st+vf.col_off+2), ib_readf(vb, st+vf.col_off+3));
    }
    for (int k = 0; k < 3; k++) {
        uint32_t base = (uint32_t)k * st;
        px[k] = ib_readf(vb, base + vf.pos_off + 0);
        py[k] = ib_readf(vb, base + vf.pos_off + 1);
        // color
        float r=1,g=1,b=1,a=1;
        if (vf.col_fmt == 38) {                          // 4x f32
            r = ib_readf(vb, base+vf.col_off+0); g = ib_readf(vb, base+vf.col_off+1);
            b = ib_readf(vb, base+vf.col_off+2); a = ib_readf(vb, base+vf.col_off+3);
        } else if (vf.col_fmt == 6) {                    // 8_8_8_8 packed (read raw bytes)
            const uint8_t* p = g_base + vb + (base + vf.col_off) * 4;  // 4 bytes, mem order
            // NU2 UI commonly stores ARGB (D3D9). bytes p[0..3] = B,G,R,A in LE → map to RGBA.
            b = p[0]/255.0f; g = p[1]/255.0f; r = p[2]/255.0f; a = p[3]/255.0f;
        }
        cr[k][0]=r; cr[k][1]=g; cr[k][2]=b; cr[k][3]=a;
    }
    // Reject misdecoded draws: window positions must be within a sane range. A wrong
    // stride sends vertices to huge/garbage coords → the diagonal/vertical line slivers.
    for (int k = 0; k < 3; k++)
        if (px[k] < -64.0f || px[k] > 2048.0f || py[k] < -64.0f || py[k] > 1152.0f) { g_geom_skip++; return; }
    float ar = (px[1]-px[0])*(py[2]-py[0]) - (px[2]-px[0])*(py[1]-py[0]);
    if (ar > -4.0f && ar < 4.0f) return;                 // degenerate (sliver)
    // Window→clip. The menu renders in a 640x360 coordinate space; map it to fill the
    // full output. (The PA_CL_VPORT regs read 1280x720 but belong to the output pass,
    // not these menu draws — empirically all menu geometry lives in 0..640 x 0..360.)
    const float VW = 640.0f, VH = 360.0f;
    auto cx = [&](float x){ return x / (VW*0.5f) - 1.0f; };
    auto cy = [&](float y){ return 1.0f - y / (VH*0.5f); };
    float p3x = px[0] + px[2] - px[1], p3y = py[0] + py[2] - py[1];
    static uint32_t qn = 0; qn++;
    bool textured = (vf.uv_off >= 0);   // shader samples a texture (we don't have it yet)
    auto col = [&](int k, int ch)->float {
        if (g_geom_dbg) { float h=(float)((qn*2654435761u)&0xFF)/255.0f;
            float t[4]={h,1.0f-h,0.5f+0.5f*h,1.0f}; return t[ch]; }
        if (textured) { const float ph[4]={0.55f,0.55f,0.60f,0.85f}; return ph[ch]; } // gray placeholder
        return cr[k][ch];
    };
    geom_emit(cx(px[0]),cy(py[0]),0, col(0,0),col(0,1),col(0,2),col(0,3));
    geom_emit(cx(px[1]),cy(py[1]),0, col(1,0),col(1,1),col(1,2),col(1,3));
    geom_emit(cx(px[2]),cy(py[2]),0, col(2,0),col(2,1),col(2,2),col(2,3));
    geom_emit(cx(px[0]),cy(py[0]),0, col(0,0),col(0,1),col(0,2),col(0,3));
    geom_emit(cx(px[2]),cy(py[2]),0, col(2,0),col(2,1),col(2,2),col(2,3));
    geom_emit(cx(p3x),  cy(p3y),  0, col(2,0),col(2,1),col(2,2),col(2,3));
}

// ── XVS: run the draw's real Xenos vertex shader on the CPU (vendored Xenia interpreter) ──
// Phase-3 first-pixels path (env LSWTCS_XVS=1). For DMA-indexed DRAW_INDX draws: execute the
// VS once per index with the real ALU/fetch constants, perspective-divide oPos, and assemble
// triangles by primitive type into the existing NDC triangle-list presenter. Colour = o0
// (usually vertex colour) when exported, else a per-draw debug tint (LSWTCS_XVS_TINT=1 forces
// tints, useful to see silhouettes). No pixel shader / textures / depth yet.
static int g_xvs_on = -1, g_xvs_tint = -1;
static uint64_t g_xvs_draws = 0, g_xvs_tris = 0, g_xvs_dropped = 0;
static void xvs_draw(uint32_t draw_initiator, uint32_t dma_base, uint32_t dma_size) {
    static LswXvsVertex buf[65536];
    if (g_vs_addr < 0x80000000u || g_vs_addr >= 0xA0000000u || g_vs_size == 0) return;
    // Per-vertex pixel shader (Gouraud approximation of the real PS lighting). LSWTCS_XPS=0 disables.
    { static int xps = -1; if (xps < 0) { const char* e = getenv("LSWTCS_XPS"); xps = (e && e[0] == '0') ? 0 : 1; }
      bool ok = xps && g_ps_addr >= 0x80000000u && g_ps_addr < 0xA0000000u && g_ps_size && g_ps_size < 0x4000;
      lswtcs_xvs_set_ps(ok ? reinterpret_cast<const uint32_t*>(g_base + g_ps_addr) : nullptr, ok ? g_ps_size : 0); }
    uint32_t n = lswtcs_xvs_run_draw(g_xe_regs, g_base + 0x80000000u,
                                     reinterpret_cast<const uint32_t*>(g_base + g_vs_addr), g_vs_size,
                                     draw_initiator, dma_base, dma_size, buf, 65536);
    g_xvs_draws++;
    if (g_xvs_draws <= 2) {   // input diagnostics for the first draws
        auto f = [](uint32_t u) { float x; memcpy(&x, &u, 4); return x; };
        dbg_ram("[XVSDIAG] VS c0=(%g %g %g %g) c1=(%g %g %g %g) c2=(%g %g %g %g) c3=(%g %g %g %g)\n",
                f(g_xe_regs[0x4000]), f(g_xe_regs[0x4001]), f(g_xe_regs[0x4002]), f(g_xe_regs[0x4003]),
                f(g_xe_regs[0x4004]), f(g_xe_regs[0x4005]), f(g_xe_regs[0x4006]), f(g_xe_regs[0x4007]),
                f(g_xe_regs[0x4008]), f(g_xe_regs[0x4009]), f(g_xe_regs[0x400A]), f(g_xe_regs[0x400B]),
                f(g_xe_regs[0x400C]), f(g_xe_regs[0x400D]), f(g_xe_regs[0x400E]), f(g_xe_regs[0x400F]));
        uint32_t a = g_xe_regs[0x48BE], b = g_xe_regs[0x48BF];
        uint32_t vbp = (a & ~3u) + 0x80000000u;
        dbg_ram("[XVSDIAG] fetch95=%08X %08X -> vb=0x%08X raw[0..5]=%08X %08X %08X %08X %08X %08X  VS ucode[0..5]=%08X %08X %08X %08X %08X %08X\n",
                a, b, vbp,
                ib_read(vbp, 0), ib_read(vbp, 1), ib_read(vbp, 2), ib_read(vbp, 3), ib_read(vbp, 4), ib_read(vbp, 5),
                ib_read(g_vs_addr, 0), ib_read(g_vs_addr, 1), ib_read(g_vs_addr, 2),
                ib_read(g_vs_addr, 3), ib_read(g_vs_addr, 4), ib_read(g_vs_addr, 5));
        dbg_ram("[XVSDIAG] c0 raw=%08X %08X %08X %08X c4 raw=%08X\n", g_xe_regs[0x4000], g_xe_regs[0x4001], g_xe_regs[0x4002], g_xe_regs[0x4003], g_xe_regs[0x4010]);
        { int nan_c = 0, zero_c = 0, val_c = 0; char lst[400]; int k = 0;
          for (int c = 0; c < 256; ++c) { uint32_t w0 = g_xe_regs[0x4000 + c*4];
            bool allz = !(g_xe_regs[0x4000+c*4]|g_xe_regs[0x4001+c*4]|g_xe_regs[0x4002+c*4]|g_xe_regs[0x4003+c*4]);
            if ((w0 & 0x7F800000u) == 0x7F800000u && (w0 & 0x7FFFFFu)) nan_c++; else if (allz) zero_c++; else { val_c++; if (k < 360) k += snprintf(lst+k, sizeof lst-k, " c%d", c); } }
          lst[k] = 0;
          dbg_ram("[XVSDIAG] VS consts: NaN=%d zero=%d valued=%d:%s\n", nan_c, zero_c, val_c, lst); }
        { static const int cs[] = {0,1,2,3,12,13,14,15,16,17,18,19,28,29,30,31,36,37,38,39,40,45,48,49,111,255};
          for (int c : cs) { auto f=[](uint32_t u){float x; memcpy(&x,&u,4); return x;};
            dbg_ram("[XVSDIAG]   c%-3d = %08X %08X %08X %08X  (%g %g %g %g)\n", c,
              g_xe_regs[0x4000+c*4],g_xe_regs[0x4001+c*4],g_xe_regs[0x4002+c*4],g_xe_regs[0x4003+c*4],
              f(g_xe_regs[0x4000+c*4]),f(g_xe_regs[0x4001+c*4]),f(g_xe_regs[0x4002+c*4]),f(g_xe_regs[0x4003+c*4])); } }
        if (g_xvs_draws == 1) {   // one-shot: find NaN-filled matrices in guest RAM (camera?)
            int found = 0; uint32_t run = 0, start = 0;
            for (uint32_t a = 0x82000000u; a < 0xA0000000u && found < 24; a += 4) {
                uint32_t w = ib_read(a, 0);
                bool nan = (w == 0xFFC00000u || w == 0x7FC00000u);
                if (nan) { if (!run) start = a; ++run; }
                else { if (run >= 9) { dbg_ram("[NANSCAN] run of %u NaN dwords at 0x%08X\n", run, start); ++found; } run = 0; }
            }
            dbg_ram("[NANSCAN] done, %d runs\n", found);
        }
        dbg_ram("[XVSDIAG] v0 pos=(%g %g %g %g) v1 pos=(%g %g %g %g) n=%u\n",
                buf[0].pos[0], buf[0].pos[1], buf[0].pos[2], buf[0].pos[3],
                buf[1].pos[0], buf[1].pos[1], buf[1].pos[2], buf[1].pos[3], n);
    }
    uint32_t prim = draw_initiator & 0x3F;
    uint32_t tint_seed = (g_vs_addr ^ (dma_base * 2654435761u)) * 2246822519u;
    float tint[4] = { 0.3f + ((tint_seed >> 8) & 0xFF) / 400.0f, 0.3f + ((tint_seed >> 16) & 0xFF) / 400.0f,
                      0.3f + ((tint_seed >> 24) & 0xFF) / 400.0f, 1.0f };
    // Xenos blend state for this draw -> premultiplied emit mode (see geom_emit_pm).
    // RB_BLENDCONTROL0 (0x2201): src[4:0], dst[12:8]; factors 0=ZERO 1=ONE 6=SRC_ALPHA
    // 7=ONE_MINUS_SRC_ALPHA. RB_COLOR_MASK (0x2104) low nibble = RT0 write mask.
    // Only replay draws aimed at the main 1280-wide framebuffer: RB_SURFACE_INFO (0x2000) pitch[13:0].
    // Offscreen passes (e.g. the 640-wide depth-clear / half-res targets) must not land on screen.
    // LSWTCS_XVS_ALLRT=1 replays everything.
    { static int allrt = -1; if (allrt < 0) { const char* e = getenv("LSWTCS_XVS_ALLRT"); allrt = (e && e[0] == '1') ? 1 : 0; }
      uint32_t pitch = g_xe_regs[0x2000] & 0x3FFF;
      if (!allrt && pitch != 1280) {
          static int nlog = 0; if (++nlog <= 8) dbg_ram("[XVSRT] skip draw to surface pitch %u (surf=0x%08X VS=0x%08X)\n", pitch, g_xe_regs[0x2000], g_vs_addr);
          return; } }
    enum { BM_OPAQUE, BM_ALPHA, BM_PREMUL, BM_ADD, BM_ADD_SA, BM_SKIP };
    int bm = BM_OPAQUE;
    { static int bl = -1; if (bl < 0) { const char* e = getenv("LSWTCS_XVS_BLEND"); bl = (e && e[0] == '0') ? 0 : 1; }
      if (bl) {
        uint32_t bc = g_xe_regs[0x2201], src = bc & 0x1F, dst = (bc >> 8) & 0x1F;
        if ((g_xe_regs[0x2104] & 0xF) == 0) bm = BM_SKIP;
        else if (src == 1 && dst == 0) bm = BM_OPAQUE;
        else if (src == 6 && dst == 7) bm = BM_ALPHA;
        else if (src == 1 && dst == 7) bm = BM_PREMUL;
        else if (src == 1 && dst == 1) bm = BM_ADD;
        else if (src == 6 && dst == 1) bm = BM_ADD_SA;
        else bm = BM_SKIP;
        static int nlog = 0;
        if (bm == BM_SKIP && ++nlog <= 20) dbg_ram("[XVSBLEND] skip draw: blend=0x%08X mask=0x%X (src=%u dst=%u)\n", bc, g_xe_regs[0x2104], src, dst);
        if (bm == BM_SKIP) return;
      } }
    // Xenos viewport transform (Xenia: PA_CL_VTE_CNTL 0x2206 + PA_CL_VPORT_* 0x210F..0x2114):
    // optional W-divide (VTX_XY_FMT/VTX_Z_FMT = already divided, VTX_W0_FMT = w holds 1/w), then
    // per-axis optional scale/offset -> SCREEN PIXELS; we map pixels to NDC for the replay. Draws
    // with the viewport transform disabled (full-screen backgrounds) output pixels directly.
    // LSWTCS_XVS_VTE=0 restores the old "output is clip space" assumption.
    static int vte_on = -1; if (vte_on < 0) { const char* e = getenv("LSWTCS_XVS_VTE"); vte_on = (e && e[0] == '0') ? 0 : 1; }
    const uint32_t vte = g_xe_regs[0x2206];
    auto regf = [](uint32_t r) { float f; uint32_t u = g_xe_regs[r]; memcpy(&f, &u, 4); return f; };
    const float vxs = regf(0x210F), vxo = regf(0x2110), vys = regf(0x2111), vyo = regf(0x2112), vzs = regf(0x2113), vzo = regf(0x2114);
    const float SW = 1280.0f, SH = 720.0f;
    auto emit = [&](const LswXvsVertex& v) {
        float w = v.pos[3];
        if (!(w > 1e-6f || w < -1e-6f)) w = 1.0f;
        float x, y, z;
        if (vte_on) {
            // VTX_W0_FMT=1: w output is the real W -> divide; =0: shader wrote 1/W -> multiply (Xenia).
            float rw = (vte & (1u << 10)) ? 1.0f / w : w;
            float px = v.pos[0], py = v.pos[1], pz = v.pos[2];
            if (!(vte & (1u << 8))) { px *= rw; py *= rw; }              // VTX_XY_FMT
            if (!(vte & (1u << 9))) pz *= rw;                           // VTX_Z_FMT
            float sx = ((vte & 1u) ? px * vxs : px) + ((vte & 2u) ? vxo : 0.f);
            float sy = ((vte & 4u) ? py * vys : py) + ((vte & 8u) ? vyo : 0.f);
            float sz = ((vte & 16u) ? pz * vzs : pz) + ((vte & 32u) ? vzo : 0.f);
            x = sx / (SW * 0.5f) - 1.0f;
            y = 1.0f - sy / (SH * 0.5f);
            z = sz;
        } else {
            x = v.pos[0] / w; y = v.pos[1] / w; z = 0.0f;
        }
        float c[4];
        // NU2 menu VS (xenia shdump 8870A45B4DED8DD5): o3 = vertex colour * c40, o0 = lit
        // normal, o1/o2 = fog terms. Prefer o3, fall back to o0.
        int ci = (v.interp_mask & 8) ? 3 : 0;
        if (!g_xvs_tint && v.has_color) {
            for (int i = 0; i < 4; ++i) c[i] = v.color[i] < 0.f ? 0.f : (v.color[i] > 1.f ? 1.f : v.color[i]);
        } else if (!g_xvs_tint && (v.interp_mask & (1u << ci))) {
            for (int i = 0; i < 4; ++i) c[i] = v.interp[ci][i] < 0.f ? 0.f : (v.interp[ci][i] > 1.f ? 1.f : v.interp[ci][i]);
        } else {
            for (int i = 0; i < 4; ++i) c[i] = tint[i];
        }
        switch (bm) {
            case BM_OPAQUE: geom_emit_pm(x, y, z, c[0], c[1], c[2], 1.0f); break;
            case BM_ALPHA:  geom_emit_pm(x, y, z, c[0] * c[3], c[1] * c[3], c[2] * c[3], c[3]); break;
            case BM_PREMUL: geom_emit_pm(x, y, z, c[0], c[1], c[2], c[3]); break;
            case BM_ADD:    geom_emit_pm(x, y, z, c[0], c[1], c[2], 0.0f); break;
            default:        geom_emit_pm(x, y, z, c[0] * c[3], c[1] * c[3], c[2] * c[3], 0.0f); break;  // ADD_SA
        }
    };
    auto tri = [&](uint32_t a, uint32_t b, uint32_t c) {
        if (a >= n || b >= n || c >= n) return;
        const LswXvsVertex &va = buf[a], &vb = buf[b], &vc = buf[c];
        if (!va.has_pos || !vb.has_pos || !vc.has_pos || va.killed || vb.killed || vc.killed) { g_xvs_dropped++; return; }
        emit(va); emit(vb); emit(vc); g_xvs_tris++;
    };
    switch (prim) {
        case 4:  for (uint32_t i = 0; i + 2 < n; i += 3) tri(i, i + 1, i + 2); break;           // triangle list
        case 5:  for (uint32_t i = 1; i + 1 < n; ++i) tri(0, i, i + 1); break;                   // triangle fan
        case 6:  for (uint32_t i = 0; i + 2 < n; ++i) (i & 1) ? tri(i + 1, i, i + 2) : tri(i, i + 1, i + 2); break; // strip
        case 8:  for (uint32_t i = 0; i + 2 < n; i += 3) {                                        // rectangle list
                     tri(i, i + 1, i + 2);
                     // 4th corner = v0 + v2 - v1, synthesised in a scratch slot
                     if (n + 1 <= 65535 && buf[i].has_pos && buf[i + 1].has_pos && buf[i + 2].has_pos) {
                         LswXvsVertex q = buf[i + 2];
                         for (int k = 0; k < 4; ++k) q.pos[k] = buf[i].pos[k] + buf[i + 2].pos[k] - buf[i + 1].pos[k];
                         emit(buf[i]); emit(buf[i + 2]); emit(q); g_xvs_tris++;
                     }
                 } break;
        case 13: for (uint32_t i = 0; i + 3 < n; i += 4) { tri(i, i + 1, i + 2); tri(i, i + 2, i + 3); } break; // quad list
        default: g_xvs_dropped++; break;
    }
    if ((g_xvs_draws % 2000) == 1) {
        const LswXvsVertex& v0 = buf[0];
        dbg_ram("[XVS] draw#%llu prim=%u n=%u tris=%llu dropped=%llu VS=0x%08X/%u v0.pos=(%.3f,%.3f,%.3f,%.3f) o0=(%.2f,%.2f,%.2f,%.2f) mask=0x%X\n",
                (unsigned long long)g_xvs_draws, prim, n, (unsigned long long)g_xvs_tris, (unsigned long long)g_xvs_dropped,
                g_vs_addr, g_vs_size, v0.pos[0], v0.pos[1], v0.pos[2], v0.pos[3],
                v0.interp[0][0], v0.interp[0][1], v0.interp[0][2], v0.interp[0][3], v0.interp_mask);
    }
}

// ── Translated-shader GPU path (LSWTCS_GPUDRAW=1): record draws for gpu_d3d12 replay ────────
// Each draw goes through the Xenia DXBC translator bridge (lsw_gpu_bridge.h); everything the
// draw reads is snapshotted into g_gpu_frame_build (lsw_gpu_frame.h), published at the swap.
#include "lsw_gpu_bridge.h"
#include "lsw_gpu_frame.h"
LswGpuFrame g_gpu_frame_build, g_gpu_frame_present, g_gpu_frame_render;
std::mutex g_gpu_frame_mtx;
// GPUPROF: host cost of each frame-pipeline stage in QPC ticks (GP_* indices), averaged per
// presented frame in the [PROF] line. Shared with gpu_d3d12.cpp (replay / present).
extern "C" { uint64_t g_gpuprof[GP_COUNT]; }
uint32_t g_gpu_offscreen_drops = 0, g_gpu_offscreen_pitch = 0;
bool g_gpu_frame_ready = false;
static int g_gpudraw_on = -1;
static bool gpudraw_on() {
    if (g_gpudraw_on < 0) { const char* e = getenv("LSWTCS_GPUDRAW"); g_gpudraw_on = (e && e[0] == '0') ? 0 : 1; }   // default ON
    return g_gpudraw_on == 1;
}
// EDRAM render targets on the GPU path (offscreen draws, depth-only draws, resolves).
// LSWTCS_GPURT=0: old behaviour (only 1280-pitch draws, straight to the backbuffer).
static bool gpurt_on() { static int v = -1; if (v < 0) { const char* e = getenv("LSWTCS_GPURT"); v = (e && e[0] == '0') ? 0 : 1; } return v == 1; }
// Records an EDRAM resolve (RB_MODECONTROL copy mode) into the draw stream, following Xenia
// draw_util::GetResolveInfo: rectangle from the 3 vertices in vertex fetch 0 (+ window offset),
// clamped to the window scissor, aligned to 8.
static void gpu_record_resolve() {
    if (!gpudraw_on() || !gpurt_on()) return;
    uint32_t ctl = g_xe_regs[0x2318];
    LswGpuDrawRec r{};
    r.kind = 1; r.edram_mode = 6;
    r.surf_info = g_xe_regs[0x2000]; r.depth_info = g_xe_regs[0x2002];
    r.rs_src = ctl & 7; r.rs_sample = (ctl >> 4) & 7; r.rs_copy_cmd = (ctl >> 20) & 3;
    static const uint32_t color_info_reg[4] = {0x2001, 0x2003, 0x2004, 0x2005};
    r.color_info = g_xe_regs[color_info_reg[r.rs_src < 4 ? r.rs_src : 0]];
    r.rs_clear_color = (ctl >> 8) & 1; r.rs_clear_depth = (ctl >> 9) & 1;
    r.rs_color_clear = g_xe_regs[0x231E]; r.rs_color_clear_lo = g_xe_regs[0x231F]; r.rs_depth_clear = g_xe_regs[0x231D];
    // Rectangle.
    uint32_t vf0 = g_xe_regs[0x4800], vf1 = g_xe_regs[0x4801];
    int32_t x0, y0, x1, y1;
    // Scissor as Xenia draw_util::GetScissor: window scissor + window offset (unless TL bit 31,
    // window_offset_disable), intersected with the screen scissor, clamped at 0. Predicated tiling
    // moves each tile into EDRAM space through the window offset.
    uint32_t tl = g_xe_regs[0x2081], br = g_xe_regs[0x2082], wo = g_xe_regs[0x2080];
    int32_t wox = int32_t(wo << 17) >> 17, woy = int32_t((wo >> 16) << 17) >> 17;
    int32_t sx0 = tl & 0x3FFF, sy0 = (tl >> 16) & 0x3FFF, sx1 = br & 0x3FFF, sy1 = (br >> 16) & 0x3FFF;
    if (!(tl & 0x80000000u)) { sx0 += wox; sx1 += wox; sy0 += woy; sy1 += woy; }
    { uint32_t stl = g_xe_regs[0x200E], sbr = g_xe_regs[0x200F];
      int32_t ssx0 = int32_t(stl << 17) >> 17, ssy0 = int32_t((stl >> 16) << 17) >> 17;
      int32_t ssx1 = int32_t(sbr << 17) >> 17, ssy1 = int32_t((sbr >> 16) << 17) >> 17;
      if (sbr) { sx0 = std::max(sx0, ssx0); sy0 = std::max(sy0, ssy0); sx1 = std::min(sx1, ssx1); sy1 = std::min(sy1, ssy1); } }
    sx0 = std::max(sx0, 0); sy0 = std::max(sy0, 0); sx1 = std::max(sx1, sx0); sy1 = std::max(sy1, sy0);
    { static int nl = 0; if (wo && ++nl <= 12) dbg_ram("[GPURES-SC] wo=%d,%d wsc=0x%08X..0x%08X screen=0x%08X..0x%08X vtxwo=%u -> scissor %d,%d..%d,%d\n",
                                                      wox, woy, tl, br, g_xe_regs[0x200E], g_xe_regs[0x200F], (g_xe_regs[0x2205] >> 16) & 1, sx0, sy0, sx1, sy1); }
    if ((vf0 & 3) == 3 && ((vf1 >> 2) & 0xFFFFFF) == 6) {
        uint32_t a = (vf0 & ~3u) & 0x1FFFFFFFu, endian = vf1 & 3;
        float half = (g_xe_regs[0x2302] & 1) ? 0.0f : 0.5f;   // PA_SU_VTX_CNTL pix_center kD3DZero
        int32_t fx[6];
        for (int i = 0; i < 6; ++i) {
            uint32_t raw = *(const uint32_t*)(g_base + 0x80000000ull + a + i * 4);   // guest bytes as stored
            uint32_t v;
            switch (endian) {
                case 1: v = ((raw & 0x00FF00FFu) << 8) | ((raw >> 8) & 0x00FF00FFu); break;   // k8in16
                case 2: v = __builtin_bswap32(raw); break;                                    // k8in32
                case 3: v = (raw << 16) | (raw >> 16); break;                                 // k16in32
                default: v = raw; break;
            }
            float f; memcpy(&f, &v, 4);
            fx[i] = int32_t((f + half) * 256.0f);
        }
        x0 = (std::min(std::min(fx[0], fx[2]), fx[4]) + 127) >> 8;
        y0 = (std::min(std::min(fx[1], fx[3]), fx[5]) + 127) >> 8;
        x1 = (std::max(std::max(fx[0], fx[2]), fx[4]) + 127) >> 8;
        y1 = (std::max(std::max(fx[1], fx[3]), fx[5]) + 127) >> 8;
        if (g_xe_regs[0x2205] & (1u << 16)) {                  // vtx_window_offset_enable
            x0 += wox; x1 += wox; y0 += woy; y1 += woy;
        }
        x0 = std::clamp(x0, sx0, sx1); x1 = std::clamp(x1, sx0, sx1);
        y0 = std::clamp(y0, sy0, sy1); y1 = std::clamp(y1, sy0, sy1);
    } else {
        x0 = sx0; y0 = sy0; x1 = sx1; y1 = sy1;
    }
    x0 &= ~7; y0 &= ~7; x1 = (x1 + 7) & ~7; y1 = (y1 + 7) & ~7;
    int32_t pitch = int32_t(r.surf_info & 0x3FFF) & ~7;
    if (x1 > pitch) { x1 = pitch; if (x0 > x1) x0 = x1; }
    r.rs_rect[0] = x0; r.rs_rect[1] = y0; r.rs_rect[2] = x1; r.rs_rect[3] = y1;
    // Destination.
    uint32_t dpitch = g_xe_regs[0x231A], dinfo = g_xe_regs[0x231B];
    r.rs_dest = g_xe_regs[0x2319] & 0x1FFFFFFFu;
    r.rs_dest_pitch = dpitch & 0x3FFF; r.rs_dest_height = (dpitch >> 16) & 0x3FFF;
    r.rs_dest_fmt = (dinfo >> 7) & 0x3F;
    r.rs_dest_x = x0; r.rs_dest_y = y0;                        // Xenia: the rectangle lands at (x0, y0) in the destination
    g_gpu_frame_build.draws.push_back(r);
    static std::unordered_map<uint32_t, int> per_dest; static int n = 0; ++n;
    if (++per_dest[r.rs_dest] <= 3)
        dbg_ram("[GPURES] #%d src=%u sample=%u cmd=%u rect=%d,%d..%d,%d surf=0x%08X col=0x%08X dep=0x%08X -> dest=0x%08X %ux%u fmt=%u clr(c=%u d=%u) vf0=%08X/%08X\n",
                n, r.rs_src, r.rs_sample, r.rs_copy_cmd, x0, y0, x1, y1, r.surf_info, r.color_info, r.depth_info,
                r.rs_dest, r.rs_dest_pitch, r.rs_dest_height, r.rs_dest_fmt, r.rs_clear_color, r.rs_clear_depth, vf0, vf1);
}
// ── Shared-memory residency, tracked at record time (4 KB pages) ─────────────────────────────
// GPU shared memory mirrors guest physical memory 1:1, so residency is tracked per page: the content
// hash last uploaded. A draw's vertex ranges are split into pages; changed pages are uploaded in
// coalesced runs, unchanged ones not at all. Deciding here, from guest memory, means unchanged vertex
// data is never copied into the frame arena nor hashed at replay. Pages are hashed once per walk
// epoch (guest memory cannot change inside one walk except through the hooks that bump the epoch:
// guest interrupt callbacks, tile-drain yields, CP memory writes). Earlier model (exact ranges keyed
// by start, overlap eviction) re-uploaded ~8 MB per hub frame: draws using overlapping slices of one
// buffer evicted each other every frame.
// A published frame that is replaced before it was replayed hands its uploads to the next frame
// (gpu_shm_carry_uploads). DIAG LSWTCS_SHMALWAYS=1: upload every page of every range every draw.
static constexpr uint32_t kShmPageShift = 12, kShmPageSize = 1u << kShmPageShift, kShmPages = 0x20000000u >> kShmPageShift;
struct GpuShmPage { uint64_t hash; uint32_t epoch; uint32_t valid; };
static std::vector<GpuShmPage> g_shm_pages;
static uint32_t g_shm_epoch = 1;
static inline void gpu_shm_new_epoch() { ++g_shm_epoch; }
static void gpu_shm_invalidate(uint32_t phys) {   // a CP write landed here: rehash before trusting the page
    if (!g_shm_pages.empty() && phys < 0x20000000u) g_shm_pages[phys >> kShmPageShift].epoch = 0;
}
static void gpu_shm_reset() { for (GpuShmPage& p : g_shm_pages) p.valid = 0; }
// Appends upload runs for the guest physical range [a, a + sz) to f.ranges.
static void gpu_shm_track(LswGpuFrame& f, uint32_t a, uint32_t sz) {
    static int always = -1; if (always < 0) { const char* e = getenv("LSWTCS_SHMALWAYS"); always = (e && e[0] == '1') ? 1 : 0; }
    if (g_shm_pages.empty()) g_shm_pages.assign(kShmPages, GpuShmPage{0, 0, 0});
    const uint8_t* phys = g_base + 0x80000000ull;
    uint32_t p0 = a >> kShmPageShift, p1 = (a + sz - 1) >> kShmPageShift, run = UINT32_MAX;
    auto flush = [&](uint32_t pend) {
        uint32_t start = run << kShmPageShift, bytes = (pend - run) << kShmPageShift;
        LswGpuRange rg{start, bytes, f.alloc_uninit(bytes, 16)};
        memcpy(f.arena.data() + rg.arena_off, phys + start, bytes);
        f.ranges.push_back(rg);
        g_gpuprof[GP_VBYTES] += bytes;
        run = UINT32_MAX;
    };
    for (uint32_t p = p0; p <= p1; ++p) {
        GpuShmPage& pg = g_shm_pages[p];
        bool dirty = always != 0;
        if (pg.epoch != g_shm_epoch) {
            uint64_t h = lsw_hash_bytes(phys + (size_t(p) << kShmPageShift), kShmPageSize);
            if (!pg.valid || pg.hash != h) dirty = true;
            pg.hash = h; pg.valid = 1; pg.epoch = g_shm_epoch;
        }
        if (dirty) { if (run == UINT32_MAX) run = p; }
        else if (run != UINT32_MAX) flush(p);
    }
    if (run != UINT32_MAX) flush(p1 + 1);
}
static void gpu_shm_carry_uploads(const LswGpuFrame& from, LswGpuFrame& to) {
    std::vector<LswGpuRange> carried;
    uint64_t bytes = 0;
    for (const LswGpuRange& rg : from.prelude) bytes += rg.size;
    if (bytes > (256u << 20)) {   // replay is not running at all: stop carrying, start over
        dbg_ram("[SHMRES] %llu MB of unreplayed uploads: residency reset\n", (unsigned long long)(bytes >> 20));
        gpu_shm_reset();
        return;
    }
    auto carry = [&](const LswGpuRange& rg) {
        if (rg.arena_off == kLswRangeResident) return;
        LswGpuRange c = rg;
        c.arena_off = to.alloc_uninit(rg.size, 16);
        memcpy(to.arena.data() + c.arena_off, from.arena.data() + rg.arena_off, rg.size);
        carried.push_back(c);
    };
    for (const LswGpuRange& rg : from.prelude) carry(rg);
    for (const LswGpuRange& rg : from.ranges) carry(rg);
    carried.insert(carried.end(), to.prelude.begin(), to.prelude.end());
    to.prelude.swap(carried);
    static uint64_t n = 0; if (++n <= 10 || (n % 1000) == 0)
        dbg_ram("[SHMRES] published frame replaced before replay (#%llu): %zu uploads carried over\n", (unsigned long long)n, to.prelude.size());
}
// Returns true if the draw was recorded (caller then skips the CPU XVS path).
static bool gpu_record_draw_impl(uint32_t initiator, uint32_t dma_base, uint32_t dma_size,
                                 const uint8_t* inline_idx, uint32_t inline_words, uint32_t edram_mode);
// DIAG wrapper: while the imlog_now window is open, log EVERY draw packet that reaches the recorder
// ([DRAWIN] prim, count, VS/PS address, mode) and whether it was recorded (rec=0 -> dropped/CPU path).
static bool gpu_record_draw(uint32_t initiator, uint32_t dma_base, uint32_t dma_size,
                            const uint8_t* inline_idx, uint32_t inline_words, uint32_t edram_mode = 4) {
    uint64_t gp0 = gp_now();
    bool rec = gpu_record_draw_impl(initiator, dma_base, dma_size, inline_idx, inline_words, edram_mode);
    g_gpuprof[GP_RECORD] += gp_now() - gp0; g_gpuprof[GP_DRAWS] += rec;
    // Window: from a D3D clear (rect list, depth mode) to the next quad list. Printed only when that quad
    // list has 40 vertices (the title's "Press START" text); armed by file "pktwin_now".
    { static uint32_t pc = 0;
      if ((++pc & 255) == 0 && GetFileAttributesA("pktwin_now") != INVALID_FILE_ATTRIBUTES) { DeleteFileA("pktwin_now"); g_pktwin_arm = 3; } }
    if (g_pktwin_arm > 0 && (initiator & 0x3F) == 8 && edram_mode == 5) { g_pktwin = 600; g_pktwin_buf.clear(); }
    else if ((initiator & 0x3F) == 13 && g_pktwin > 0) {
        if ((initiator >> 16) == 40) {
            printf("%s[PKTWIN] -- 40-vertex quad-list draw reached (vs=0x%08X ps=0x%08X)\n", g_pktwin_buf.c_str(), g_vs_addr, g_ps_addr); fflush(stdout);
            --g_pktwin_arm;
        }
        g_pktwin = 0; g_pktwin_buf.clear();
    }
    if (g_imlog_left > 0) { printf("[DRAWIN] prim=%u n=%u src=%u mode=%u vs=0x%08X/%u ps=0x%08X/%u rec=%d\n", initiator & 0x3F, initiator >> 16, (initiator >> 6) & 3,
                                   edram_mode, g_vs_addr, g_vs_size, g_ps_addr, g_ps_size, rec ? 1 : 0); fflush(stdout); }
    return rec;
}
static bool gpu_record_draw_impl(uint32_t initiator, uint32_t dma_base, uint32_t dma_size,
                                 const uint8_t* inline_idx, uint32_t inline_words, uint32_t edram_mode) {
    if (!gpudraw_on()) return false;
    if (g_vs_addr < 0x80000000u || g_vs_addr >= 0xA0000000u || !g_vs_size) return false;
    if (!gpurt_on() && (g_xe_regs[0x2000] & 0x3FFF) != 1280) {        // offscreen target: drop (as XVS)
        extern uint32_t g_gpu_offscreen_drops, g_gpu_offscreen_pitch;
        ++g_gpu_offscreen_drops; g_gpu_offscreen_pitch = g_xe_regs[0x2000];
        return true;
    }
    uint32_t prim = initiator & 0x3F, src = (initiator >> 6) & 3;
    // Xenos prim -> D3D topology (+ Xenia geometry shader for types D3D12 lacks).
    uint32_t topo, gs_type = 0; bool fan = false;
    switch (prim) {
        case 1: topo = 1; gs_type = 1; break;   // point list -> point sprites (GS)
        case 2: topo = 2; break;                // line list
        case 3: topo = 3; break;                // line strip
        case 4: topo = 4; break;                // triangle list
        case 5: topo = 4; fan = true; break;    // triangle fan -> list (indices rewritten)
        case 6: topo = 5; break;                // triangle strip
        case 8: topo = 4; gs_type = 2; break;   // rectangle list: triangles in, GS emits quads
        case 13: topo = 10; gs_type = 3; break; // quad list: LINELIST_ADJ in, GS emits quads
        default: return false;                  // line loops / polygons / patches: CPU path
    }
    { static int gsoff = -1; if (gsoff < 0) { const char* e = getenv("LSWTCS_GPUDRAW_NOGS"); gsoff = (e && e[0] == '1') ? 1 : 0; }
      if (gsoff && (gs_type || fan)) return false; }
    g_xe_regs[0x21FC] = initiator;  // VGT_DRAW_INITIATOR (the bridge reads prim type from it)
    bool idx32 = (initiator >> 11) & 1;
    uint32_t endian = (src == 0 || src == 1) ? (dma_size >> 30) : 0;
    if (src == 1) endian = idx32 ? 2u : 1u;
    bool ps_ok = g_ps_addr >= 0x80000000u && g_ps_addr < 0xA0000000u && g_ps_size && g_ps_size < 0x4000;
    LswGpuDraw d;
    uint64_t gp0 = gp_now();
    int prepared = lsw_gpu_prepare(g_xe_regs, reinterpret_cast<const uint32_t*>(g_base + g_vs_addr), g_vs_size,
                                   ps_ok ? reinterpret_cast<const uint32_t*>(g_base + g_ps_addr) : nullptr,
                                   ps_ok ? g_ps_size : 0, endian, 1280, 720, &d);
    g_gpuprof[GP_PREPARE] += gp_now() - gp0;
    if (!prepared) {
        static int nfail = 0; if (++nfail <= 10) dbg_ram("[GPUDRAW] prepare failed VS=0x%08X/%u PS=0x%08X/%u\n", g_vs_addr, g_vs_size, g_ps_addr, g_ps_size);
        return false;
    }
    LswGpuFrame& f = g_gpu_frame_build;
    LswGpuDrawRec r{};
    r.kind = 0; r.edram_mode = edram_mode;
    if (g_imlog_left > 0) { printf("[IMLOG]   draw mode=%u prim=%u n=%u vs=0x%08X key=%016llX ps=0x%08X/%u ps_ok=%d pskey=%016llX cmask=0x%X\n", edram_mode, initiator & 0x3F, initiator >> 16,
                                   g_vs_addr, (unsigned long long)d.vs_key, g_ps_addr, g_ps_size, ps_ok ? 1 : 0, (unsigned long long)d.ps_key, g_xe_regs[0x2104]); fflush(stdout); }
    r.surf_info = g_xe_regs[0x2000]; r.color_info = g_xe_regs[0x2001]; r.depth_info = g_xe_regs[0x2002];
    r.vs_dxbc = d.vs_dxbc; r.vs_size = uint32_t(d.vs_dxbc_size); r.vs_key = d.vs_key;
    r.ps_dxbc = d.ps_dxbc; r.ps_size = uint32_t(d.ps_dxbc_size); r.ps_key = d.ps_key;
    auto put = [&](const void* src0, uint32_t size, uint32_t& off_out) {
        off_out = f.alloc(size ? size : 16); if (size) memcpy(f.arena.data() + off_out, src0, size); };
    r.cb_sys_size = d.system_constants_size; put(d.system_constants, d.system_constants_size, r.cb_sys_off);
    // Float constants are bound as root CBVs, which have no size: a relative read c[a0+N] past the
    // block would return the next draw's arena bytes (NaN / huge -> exploded shadow-pass vertices).
    // A full 256-entry block means dynamic addressing, so reserve 512 entries. Xenos has ONE 512-entry
    // ALU constant file (VS c0-255, PS c256-511): a VS read past c255 sees the PS constants. The game's
    // skinning relies on it (bone slot 255 has non-zero weight and reads c[112+255] = PS c111), so the
    // VS padding is the live PS constant registers; PS padding (past c511) is zero.
    // LSWTCS_CBPAD: 0 = off, 1 = PS constants (shared 512-entry file), 2 = zeros (Xenia's sized CBV, DEFAULT),
    // 3 = wrap within the VS's 256 (c[256+k] = c[k]). DIAG: file "cbpad" in the working dir cycles
    // 1 -> 2 -> 3 -> 0 live (checked every 4096 draws), logged as [CBPAD].
    static int cbpad = -1; if (cbpad < 0) { const char* e = getenv("LSWTCS_CBPAD"); cbpad = e ? atoi(e) : 2; }
    { static uint32_t ncheck = 0;
      if ((++ncheck & 4095) == 0 && GetFileAttributesA("cbpad") != INVALID_FILE_ATTRIBUTES) {
          DeleteFileA("cbpad"); cbpad = (cbpad + 1) & 3;
          static const char* const kNames[4] = {"OFF (no padding)", "PS constants", "zeros (Xenia)", "wrap mod 256"};
          printf("[CBPAD] mode %d: %s\n", cbpad, kNames[cbpad]); fflush(stdout);
      } }
    auto put_float = [&](const float* src0, uint32_t count, const uint32_t* tail, uint32_t& size_out, uint32_t& off_out) {
        size_out = count * 16;
        if (!cbpad || count < 256) { put(src0, size_out, off_out); return; }
        off_out = f.alloc(512 * 16);
        memcpy(f.arena.data() + off_out, src0, size_out);
        if (tail && cbpad == 1) memcpy(f.arena.data() + off_out + size_out, tail, 512 * 16 - size_out);
        else if (tail && cbpad == 3) memcpy(f.arena.data() + off_out + size_out, src0, 512 * 16 - size_out);
        else memset(f.arena.data() + off_out + size_out, 0, 512 * 16 - size_out);
    };
    put_float(d.vs_float, d.vs_float_count, &g_xe_regs[0x4400], r.cb_vsf_size, r.cb_vsf_off);   // SHADER_CONSTANT_256_X
    put_float(d.ps_float, d.ps_float_count, nullptr, r.cb_psf_size, r.cb_psf_off);
    put(d.bool_loop, 40 * 4, r.cb_bool_off);
    put(d.fetch, 192 * 4, r.cb_fetch_off);
    // Vertex data snapshot (guest physical ranges -> shared memory at replay).
    r.first_range = uint32_t(f.ranges.size());
    gp0 = gp_now();
    for (uint32_t i = 0; i < d.vfetch_count; ++i) {
        uint32_t a = d.vfetch_addr[i] & 0x1FFFFFFFu, sz = d.vfetch_size[i];
        if (!sz || sz > 0x1000000u || a + sz > 0x20000000u) continue;
        gpu_shm_track(f, a, sz);
    }
    g_gpuprof[GP_VCOPY] += gp_now() - gp0;
    r.range_count = uint32_t(f.ranges.size()) - r.first_range;
    // DIAG: quad-list draws inside the imlog_now window -> vertex ranges and first guest dwords at record time.
    if (g_imlog_left > 0 && (initiator & 0x3F) == 13) {
        printf("[QUADVTX] n=%u vfetch_count=%u fetch0=%08X %08X %08X %08X %08X %08X\n", initiator >> 16, d.vfetch_count, g_xe_regs[0x4800], g_xe_regs[0x4801], g_xe_regs[0x4802], g_xe_regs[0x4803], g_xe_regs[0x4804], g_xe_regs[0x4805]);
        for (uint32_t i = 0; i < d.vfetch_count && i < 4; ++i) {
            uint32_t a = d.vfetch_addr[i] & 0x1FFFFFFFu;
            printf("[QUADVTX]   range 0x%08X +%u:", a, d.vfetch_size[i]);
            for (int k = 0; k < 8; ++k) printf(" %08X", __builtin_bswap32(*(const uint32_t*)(g_base + 0x80000000ull + a + k * 4)));
            printf("\n");
        }
        fflush(stdout);
    }
    // DIAG (LSWTCS_VCDUMP=1): first world draws (2x MSAA, 1280 pitch, 2 colour targets): raw vertex dwords.
    { static int vc = -1; if (vc < 0) { const char* e = getenv("LSWTCS_VCDUMP"); vc = (e && e[0] == '1') ? 1 : 0; }
      static int nd = 0;
      if (vc && nd < 6 && ((g_xe_regs[0x2000] >> 16) & 3) == 1 && (g_xe_regs[0x2000] & 0x3FFF) == 1280 && d.vfetch_count) {
          ++nd;
          uint32_t a = d.vfetch_addr[0] & 0x1FFFFFFFu;
          uint32_t vf0 = g_xe_regs[0x4800], vf1 = g_xe_regs[0x4801];
          dbg_ram("[VCDUMP] draw#%d vf0=%08X/%08X range=0x%08X+%u PS=0x%08X\n", nd, vf0, vf1, a, d.vfetch_size[0], g_ps_addr);
          for (int v = 0; v < 4; ++v) {
              char b[256]; int k = 0;
              for (int i = 0; i < 10; ++i) k += snprintf(b + k, sizeof b - k, " %08X", __builtin_bswap32(*(const uint32_t*)(g_base + 0x80000000ull + a + (v * 10 + i) * 4)));
              dbg_ram("[VCDUMP]   v%d:%s\n", v, b);
          }
          // PS float constants c40 (VS) and c47 (PS fog colour?) as the shader sees them.
          float c40[4], c47[4];
          memcpy(c40, &g_xe_regs[0x4000 + 40 * 4], 16); memcpy(c47, &g_xe_regs[0x4000 + (256 + 47) * 4], 16);
          dbg_ram("[VCDUMP]   VS c40=%g %g %g %g  PS c47=%g %g %g %g\n", c40[0], c40[1], c40[2], c40[3], c47[0], c47[1], c47[2], c47[3]);
      } }
    // Indices.
    uint32_t nidx = initiator >> 16;
    if (nidx == 2640) {   // DIAG: the repeated 2640-index post-process draws
        static int n2640 = 0;
        if (++n2640 <= 12) {
            char vb[400]; int k = 0;
            for (uint32_t i = 0; i < d.vfetch_count && k < 360; ++i) k += snprintf(vb + k, sizeof vb - k, " [%08X+%u]", d.vfetch_addr[i], d.vfetch_size[i]);
            dbg_ram("[D2640] #%d init=0x%08X prim=%u src=%u idx32=%u dma_base=0x%08X dma_size=0x%08X inline_words=%u VS=0x%08X/%u PS=0x%08X/%u surf=0x%08X vf:%s\n",
                    n2640, initiator, prim, src, idx32 ? 1 : 0, dma_base, dma_size, inline_words, g_vs_addr, g_vs_size, g_ps_addr, g_ps_size,
                    g_xe_regs[0x2000], k ? vb : " none");
        }
    }
    r.vertex_count = nidx; r.topology = topo; r.ib_32bit = idx32 ? 1 : 0;
    if (src == 0 || src == 1) {
        uint32_t bytes = nidx * (idx32 ? 4u : 2u);
        const uint8_t* srcp = (src == 0) ? (g_base + 0x80000000u + ((dma_base & 0x1FFFFFFFu) & ~(idx32 ? 3u : 1u))) : inline_idx;
        if (src == 1 && bytes > inline_words * 4) bytes = inline_words * 4;
        uint32_t esz = idx32 ? 4u : 2u, n = bytes / esz;
        if (fan) {
            // Rewrite as a list, copying RAW guest elements (the VS applies the endian swap).
            uint32_t tris = n >= 3 ? n - 2 : 0;
            r.ib_off = f.alloc(tris ? tris * 3 * esz : 4, 16);
            uint8_t* o = f.arena.data() + r.ib_off;
            for (uint32_t t = 0; t < tris; ++t) {
                memcpy(o, srcp, esz); memcpy(o + esz, srcp + (t + 1) * esz, esz); memcpy(o + 2 * esz, srcp + (t + 2) * esz, esz);
                o += 3 * esz;
            }
            r.ib_count = tris * 3;
        } else {
            r.ib_off = f.alloc(bytes ? bytes : 4, 16);
            if (bytes) memcpy(f.arena.data() + r.ib_off, srcp, bytes);
            r.ib_count = n;
        }
    } else if (fan) {
        // Auto-indexed fan: host-order 32-bit list (vertex_index_endian is 0 for auto draws).
        uint32_t tris = nidx >= 3 ? nidx - 2 : 0;
        r.ib_off = f.alloc(tris ? tris * 12 : 4, 16);
        uint32_t* o = reinterpret_cast<uint32_t*>(f.arena.data() + r.ib_off);
        for (uint32_t t = 0; t < tris; ++t) { o[3 * t] = 0; o[3 * t + 1] = t + 1; o[3 * t + 2] = t + 2; }
        r.ib_count = tris * 3; r.ib_32bit = 1;
    }
    if (gs_type) {
        size_t gsz = 0;
        r.gs_dxbc = lsw_gpu_geometry_shader(gs_type, d.gs_key_bits, &gsz);
        r.gs_size = uint32_t(gsz); r.gs_type = gs_type;
    }
    r.blendcontrol = g_xe_regs[0x2201]; r.color_mask = g_xe_regs[0x2104];
    { static const uint32_t ci[4] = {0x2001, 0x2003, 0x2004, 0x2005}, bc[4] = {0x2201, 0x2209, 0x220A, 0x220B};
      for (int i = 0; i < 4; ++i) { r.color_info_mrt[i] = g_xe_regs[ci[i]]; r.blend_mrt[i] = g_xe_regs[bc[i]]; }
      r.ps_rt_mask = d.ps_dxbc ? d.ps_color_targets : 0; }
    r.stencil_ref = g_xe_regs[0x210D]; r.stencil_ref_bf = g_xe_regs[0x210C];
    r.depthcontrol = g_xe_regs[0x2200]; r.sc_mode_cntl = g_xe_regs[0x2205];
    memcpy(r.viewport, d.viewport, sizeof(r.viewport)); memcpy(r.scissor, d.scissor, sizeof(r.scissor));
    r.ps_tex_count = d.ps_tex_count; r.ps_smp_count = d.ps_smp_count;
    r.vs_tex_count = d.vs_tex_count; r.vs_smp_count = d.vs_smp_count;
    for (uint32_t i = 0; i < d.ps_tex_count && i < 32; ++i) { r.ps_tex_fetch[i] = uint8_t(d.ps_tex[i].fetch_constant); r.ps_tex_dim[i] = uint8_t(d.ps_tex[i].dimension); }
    for (uint32_t i = 0; i < d.vs_tex_count && i < 32; ++i) { r.vs_tex_fetch[i] = uint8_t(d.vs_tex[i].fetch_constant); r.vs_tex_dim[i] = uint8_t(d.vs_tex[i].dimension); }
    for (uint32_t i = 0; i < d.ps_smp_count && i < 32; ++i) r.ps_smp[i] = d.ps_smp[i];
    for (uint32_t i = 0; i < d.vs_smp_count && i < 32; ++i) r.vs_smp[i] = d.vs_smp[i];
    f.draws.push_back(r);
    static uint64_t nrec = 0; if (++nrec <= 5 || (nrec % 5000) == 0)
        dbg_ram("[GPUDRAW] rec #%llu prim=%u n=%u ib=%u ranges=%u vsf=%u psf=%u tex=%u VS=%zuB PS=%zuB\n", (unsigned long long)nrec, prim, nidx, r.ib_count, r.range_count, d.vs_float_count, d.ps_float_count, d.ps_tex_count, d.vs_dxbc_size, d.ps_dxbc_size);
    return true;
}

// Key Xenos RB register dword-indices (xenia xenos.h).
enum {
    XE_RB_SURFACE_INFO   = 0x2000,  // pitch | msaa | (hi-z?)
    XE_RB_COLOR_INFO     = 0x2001,  // edram color base + color format
    XE_RB_DEPTH_INFO     = 0x2002,
    XE_SQ_PROGRAM_CNTL   = 0x2180,  // VS/PS GPR + size counts
    XE_SQ_CONTEXT_MISC   = 0x2181,
    XE_RB_MODECONTROL    = 0x2208,  // edram_mode bits[2:0]: 0=color+depth, 4=copy/resolve
    XE_RB_COPY_CONTROL   = 0x2318,
    XE_RB_COPY_DEST_BASE = 0x2319,  // resolve destination guest address (the framebuffer)
    XE_RB_COPY_DEST_PITCH= 0x231A,
    XE_RB_COPY_DEST_INFO = 0x231B,
    XE_RB_DEPTH_CLEAR    = 0x231D,
    XE_RB_COLOR_CLEAR    = 0x231E,  // clear color (packed per color format)
    XE_RB_COLOR_CLEAR_LO = 0x231F,
};

// ── PM4 command TRACE → RAM, dumped once (for offline review + Xenia compare) ────
// Env-gated (LSWTCS_PM4LOG=1), OFF by default. To keep per-packet cost CONSTANT
// (this laptop shares RAM between CPU/GPU; we mimic the GPU streaming commands and
// avoid file I/O that would perturb the timing-fragile async-load race), every
// command is appended to a preallocated RAM buffer. The whole buffer is written to
// pm4_trace.log in ONE pass when capture ends — triggered automatically at frame
// PM4_TRACE_DUMP_FRAME (a hard process kill skips atexit, so we cannot rely on it).
static char*           g_pm4_buf          = nullptr;   // preallocated RAM buffer
static size_t          g_pm4_buf_cap      = 0;
static size_t          g_pm4_buf_len      = 0;
static bool            g_pm4_trace_on     = false;
static bool            g_pm4_dumped       = false;
static uint32_t        g_pm4_trace_frame  = 0;
static uint32_t        g_pm4_dump_frame   = 300;        // dump after this many frames
static std::mutex      g_pm4_trace_mutex;

static void pm4_trace_open() {
    static bool init = false;
    if (init) return; init = true;
    if (!getenv("LSWTCS_PM4LOG")) return;
    if (const char* df = getenv("LSWTCS_PM4LOG_FRAMES")) { uint32_t v = (uint32_t)atoi(df); if (v) g_pm4_dump_frame = v; }
    g_pm4_buf_cap = 128u * 1024 * 1024;                 // 128 MB
    g_pm4_buf = (char*)malloc(g_pm4_buf_cap);
    if (g_pm4_buf) g_pm4_trace_on = true;
}

static void pm4_trace_dump() {
    if (!g_pm4_trace_on || g_pm4_dumped) return;
    g_pm4_dumped = true;                                 // stop logging (capture done)
    FILE* f = fopen("pm4_trace.log", "wb");
    if (f) { fwrite(g_pm4_buf, 1, g_pm4_buf_len, f); fclose(f); }
}

// Append one formatted command line (RAM only — constant cost, no file I/O).
static void pm4_trace(int depth, const char* fmt, ...) {
    if (!g_pm4_trace_on || g_pm4_dumped) return;
    std::lock_guard<std::mutex> lk(g_pm4_trace_mutex);
    if (g_pm4_buf_len + 320 >= g_pm4_buf_cap) { pm4_trace_dump(); return; }
    char* p = g_pm4_buf + g_pm4_buf_len;
    for (int i = 0; i < depth && i < 8; i++) { *p++ = ' '; *p++ = ' '; }
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(p, 256, fmt, ap); va_end(ap);
    if (n < 0) n = 0; if (n > 256) n = 256;
    p += n; *p++ = '\n';
    g_pm4_buf_len = (size_t)(p - g_pm4_buf);
}

// Per-frame marker (called from VdSwap). Triggers the one-shot dump at the limit.
static void pm4_trace_frame() {
    pm4_trace_open();
    if (!g_pm4_trace_on || g_pm4_dumped) return;
    pm4_trace(0, "==== FRAME %u ====", g_pm4_trace_frame);
    if (++g_pm4_trace_frame >= g_pm4_dump_frame) pm4_trace_dump();
}

// ── CP scratch-register writeback (Xenia command_processor.cc HandleSpecialRegisterWrite) ──
// The D3D runtime delivers deferred-callback (fn,arg) to the graphics ISR by writing
// SCRATCH_REG4/5 (0x57C/0x57D) in the command stream; the CP mirrors SCRATCH_REG0..7 to
// SCRATCH_ADDR (0x1DD) + 4*i for each bit set in SCRATCH_UMSK (0x1DC). Without it the ISR
// (sub_822ADA00) finds fn==0, the deferred-list worker (sub_822D2AB0) never wakes, and the
// game stops kicking the ring after boot ([dev+11000] stuck at 1). LSWTCS_SCRATCHWB=0 disables.
static int g_scratchwb = -1;
static uint64_t g_scratch_writes = 0;
static inline void pm4_reg_side_effect(uint32_t r, uint32_t v) {
    if (r < 0x578 || r > 0x57F) return;
    if (g_scratchwb < 0) { const char* e = getenv("LSWTCS_SCRATCHWB"); g_scratchwb = (e && e[0] == '0') ? 0 : 1; }
    if (!g_scratchwb || !g_gfx_interrupt_data) return;
    uint32_t i = r - 0x578;
    uint32_t umsk = g_xe_regs[0x1DC] ? g_xe_regs[0x1DC] : 0x33u;
    if (!(umsk & (1u << i))) return;
    uint8_t* base = g_base;
    uint32_t sa = PPC_LOAD_U32(g_gfx_interrupt_data + 10900);   // guest VA of scratch block
    if (sa < 0x82000000u || sa >= 0xC0000000u) return;
    PPC_STORE_U32(sa + i * 4, v);
    if (++g_scratch_writes <= 12 || (g_scratch_writes % 2000) == 0)
        dbg_ram("[SCRATCH] #%llu reg%u=0x%08X -> [0x%08X]\n", (unsigned long long)g_scratch_writes, i, v, sa + i * 4);
}
static uint64_t g_chain_depthcap = 0, g_chain_nested = 0;
// ── Bin predication (Xenia pm4_command_processor_implement.h ExecutePacketType3) ──────────────
// Type-3 packets with header bit 0 set are predicated: skipped when (bin_select & bin_mask) == 0.
// D3D's predicated tiling sets BIN_MASK/SELECT per tile, so per-tile packets (draws, resolves, and the
// tile-replay callback it records into the deferred stream) execute only for their tile.
// LSWTCS_PREDICATE=0 executes every packet (old behaviour).
static uint64_t g_bin_mask = 0xFFFFFFFFull, g_bin_select = 0xFFFFFFFFull;
// Returns true if the packet must be skipped. `rd(i)` reads body dword i (1-based).
template <typename RD> static bool pm4_bin_packet(uint32_t hdr, uint32_t op, uint32_t body, RD rd) {
    switch (op) {
        case 0x60: if (body >= 1) g_bin_mask = (g_bin_mask & 0xFFFFFFFF00000000ull) | rd(1); return false;
        case 0x61: if (body >= 1) g_bin_mask = (g_bin_mask & 0xFFFFFFFFull) | (uint64_t(rd(1)) << 32); return false;
        case 0x62: if (body >= 1) g_bin_select = (g_bin_select & 0xFFFFFFFF00000000ull) | rd(1); return false;
        case 0x63: if (body >= 1) g_bin_select = (g_bin_select & 0xFFFFFFFFull) | (uint64_t(rd(1)) << 32); return false;
        case 0x50: if (body >= 2) g_bin_mask = (uint64_t(rd(1)) << 32) | rd(2); return false;
        case 0x51: if (body >= 2) g_bin_select = (uint64_t(rd(1)) << 32) | rd(2); return false;
        default: break;
    }
    static int on = -1; if (on < 0) { const char* e = getenv("LSWTCS_PREDICATE"); on = (e && e[0] == '0') ? 0 : 1; }
    if (on && (hdr & 1u) && ((g_bin_select & g_bin_mask) == 0 || op == 0x64)) {
        static uint64_t n = 0; if ((++n % 20000) == 1) dbg_ram("[PREDSKIP] #%llu op=0x%02X mask=%016llX select=%016llX\n", (unsigned long long)n, op,
                                                                 (unsigned long long)g_bin_mask, (unsigned long long)g_bin_select);
        return true;
    }
    return false;
}
static void pm4_extract_ib(uint32_t phys_addr, uint32_t dword_count, int depth) {
    // IB rejects are counted and the first few logged (IBREJ): a silently skipped IB loses every
    // draw and resolve in it. Xenia masks IB pointers to physical (CpuToGpu: & 0x1FFFFFFF);
    // LSWTCS_IBMASK=0 restores the old unmasked check (pointers with upper bits were dropped).
    static int ibmask = -1; if (ibmask < 0) { const char* e = getenv("LSWTCS_IBMASK"); ibmask = (e && e[0] == '0') ? 0 : 1; }
    static uint64_t rej[4]; static uint64_t seen = 0;
    auto reject = [&](int why) {
        ++rej[why];
        static int nl = 0; if (++nl <= 30) dbg_ram("[IBREJ] why=%s addr=0x%08X len=%u depth=%d\n", why == 0 ? "depth" : why == 1 ? "len" : why == 2 ? "addr" : "?", phys_addr, dword_count, depth);
    };
    if ((++seen % 20000) == 0) dbg_ram("[IBREJ] after %llu IBs: depth=%llu len=%llu addr=%llu\n", (unsigned long long)seen, (unsigned long long)rej[0], (unsigned long long)rej[1], (unsigned long long)rej[2]);
    if (depth >= 4) { g_chain_depthcap++; reject(0); return; }
    if (dword_count == 0) return;
    if (dword_count > 0x20000u) { reject(1); return; }
    if (phys_addr & 0xE0000000u) {
        static uint64_t nm = 0; ++nm;
        if (nm <= 30 || (nm % 5000) == 0) dbg_ram("[IBREJ] upper-bit IB pointer #%llu addr=0x%08X len=%u depth=%d (%s)\n",
                                                  (unsigned long long)nm, phys_addr, dword_count, depth, ibmask ? "masked, executed" : "DROPPED");
    }
    if (ibmask) phys_addr &= 0x1FFFFFFFu;
    uint32_t guest_base = phys_addr + 0x80000000u;
    if (guest_base < 0x80000000u || guest_base >= 0xA0000000u) { reject(2); return; }
    uint32_t pos = 0;
    while (pos < dword_count) {
        uint32_t hdr  = ib_read(guest_base, pos);
        uint32_t type = hdr >> 30;
        if (g_pktwin > 0 && type != 2) {
            --g_pktwin;
            char b[256];
            snprintf(b, sizeof b, "[PKTWIN] ib=0x%08X pos=%u/%u depth=%d hdr=%08X type=%u op=0x%02X(%s) body=%u d1=%08X d2=%08X\n", guest_base, pos, dword_count, depth, hdr, type,
                     type == 3 ? (hdr >> 8) & 0x7F : 0, type == 3 ? pm4_op_name((hdr >> 8) & 0x7F) : "-", ((hdr >> 16) & 0x3FFF) + 1,
                     pos + 1 < dword_count ? ib_read(guest_base, pos + 1) : 0, pos + 2 < dword_count ? ib_read(guest_base, pos + 2) : 0);
            g_pktwin_buf += b;
        }
        if (type == 2) { pos++; continue; }
        if (type == 0) {
            uint32_t cnt = ((hdr >> 16) & 0x3FFF) + 1;
            uint32_t reg = hdr & 0x7FFF;
            if (g_pm4_trace_on) pm4_trace(depth, "T0 SET_REG base=0x%04X x%u  [first=%08X]",
                                          reg, cnt, cnt ? ib_read(guest_base, pos + 1) : 0);
            // Bit 15 = write_one_reg: every value goes to the SAME register (Xenia
            // WriteOneRegisterFromRing). It was ignored, smearing such writes across `cnt` regs.
            bool one_reg = (hdr >> 15) & 1u;
            for (uint32_t i = 0; i < cnt && pos + 1 + i < dword_count; i++) {
                uint32_t r = one_reg ? reg : reg + i;
                if (r < 0x6000) { uint32_t v = ib_read(guest_base, pos + 1 + i); g_xe_regs[r] = v; regset_note(r, v); pm4_reg_side_effect(r, v); }
            }
            if (reg < 0x4800 && reg + cnt > 0x4000 && LSW_ENV_SET("LSWTCS_XVS")) {
                static int m = 0;
                if (++m <= 12) dbg_ram("[T0-ALU] hdr=%08X oneReg=%u reg=0x%04X cnt=%u first=%08X %08X %08X %08X\n", hdr, (hdr >> 15) & 1u, reg, cnt,
                                       ib_read(guest_base, pos + 1), ib_read(guest_base, pos + 2), ib_read(guest_base, pos + 3), ib_read(guest_base, pos + 4));
            }
            pos += 1 + cnt; continue;
        }
        if (type == 3) {
            uint32_t body  = ((hdr >> 16) & 0x3FFF) + 1;
            uint32_t op    = (hdr >> 8) & 0x7F;
            uint32_t total = 1 + body;
            if (pos + total > dword_count) break;
            if (pm4_bin_packet(hdr, op, body, [&](uint32_t i) { return ib_read(guest_base, pos + i); })) { pos += total; continue; }
            if (op == 0x64 && body >= 1 && ib_read(guest_base, pos + 1) == 0x53574150u) geom_swap_packet(ib_read(guest_base, pos + 2));
            if (op == 0x54 && body >= 1) {   // INTERRUPT inside a draw IB
                // LSWTCS_IBINT: 0 = never fire (default), 1 = fire all (hangs the title load),
                // 2 = fire only D3D's predicated-tiling replay callback 0x822D2BA0 (EndTiling hands the
                //     recorded tile list to the per-CPU worker sub_822D2AB0, which replays it per tile).
                static int on = -1; if (on < 0) { const char* e = getenv("LSWTCS_IBINT"); on = e ? atoi(e) : 2; }   // default 2: tiling replay callback
                static uint64_t n = 0, nt = 0; ++n;
                // LSWTCS_IBINT_AFTER=<swap#>: only from that swap on (the loading screen stalls with it).
                static long after = -2; if (after == -2) { const char* e = getenv("LSWTCS_IBINT_AFTER"); after = e ? atol(e) : 0; }
                extern uint64_t g_geom_swaps_pub;
                bool armed = g_geom_swaps_pub >= uint64_t(after);
                bool fire = on == 1 && armed;
                if (on == 2 && armed && g_gfx_interrupt_data) {
                    uint8_t* base = g_base;
                    uint32_t sa = PPC_LOAD_U32(g_gfx_interrupt_data + 10900);
                    if (sa >= 0x82000000u && sa < 0xC0000000u && PPC_LOAD_U32(sa + 16) == 0x822D2BA0u) {
                        fire = true;
                        if (++nt <= 10 || (nt % 500) == 0) dbg_ram("[IBINT] tiling replay #%llu arg=0x%08X\n", (unsigned long long)nt, PPC_LOAD_U32(sa + 20));
                    }
                }
                if (n <= 5 || (n % 1000) == 0) dbg_ram("[IBINT] #%llu mask=0x%02X mode=%d fired=%d\n", (unsigned long long)n, ib_read(guest_base, pos + 1), on, (int)fire);
                if (fire) pm4_fire_interrupt(ib_read(guest_base, pos + 1));
            }
            if (g_pm4_trace_on) pm4_trace(depth, "T3 %-20s op=0x%02X x%u", pm4_op_name(op), op, body);
            // OPHIST (env LSWTCS_OPHIST, removable): histogram of type-3 opcodes reaching the
            // executor, to find constant-upload packets we drop (LOAD_ALU_CONSTANT 0x2F,
            // SET_CONSTANT2 0x55, SET_SHADER_CONSTANTS 0x56) and SET_CONSTANT sub-types.
            {
                static int on = -1; if (on < 0) on = getenv("LSWTCS_OPHIST") ? 1 : 0;
                if (on) {
                    static uint64_t cnt[128], sc_type[8], total = 0;
                    cnt[op & 0x7F]++;
                    if (op == 0x2D && body >= 1) sc_type[(ib_read(guest_base, pos + 1) >> 16) & 7]++;
                    if ((++total % 400000) == 0) {
                        char b[1024]; int k = snprintf(b, sizeof b, "[OPHIST] after %llu pkts:", (unsigned long long)total);
                        for (int o = 0; o < 128 && k < 900; ++o) if (cnt[o]) k += snprintf(b + k, sizeof b - k, " %s(0x%02X)=%llu", pm4_op_name((uint32_t)o), o, (unsigned long long)cnt[o]);
                        k += snprintf(b + k, sizeof b - k, " | SET_CONSTANT types:");
                        for (int t = 0; t < 8 && k < 1000; ++t) if (sc_type[t]) k += snprintf(b + k, sizeof b - k, " t%d=%llu", t, (unsigned long long)sc_type[t]);
                        dbg_ram("%s\n", b);
                    }
                }
            }
            // GPU->memory writes inside IBs (fences / completion markers the CPU polls). Xenia performs
            // them in order with the stream; LSWTCS_IBFENCE=0 skips them (old behaviour).
            { static int ibf = -1; if (ibf < 0) { const char* e = getenv("LSWTCS_IBFENCE"); ibf = (e && e[0] == '0') ? 0 : 1; }
              if (ibf && op == 0x58 && body >= 3) {                       // EVENT_WRITE_SHD
                  uint32_t init = ib_read(guest_base, pos + 1), a = ib_read(guest_base, pos + 2), v = ib_read(guest_base, pos + 3);
                  g_xe_regs[0x21F9] = init & 0x3F;                            // VGT_EVENT_INITIATOR
                  if ((init >> 31) & 1) { static uint32_t ctr = 0; v = ++ctr; }
                  pm4_gpu_write(a, v);
                  static int nl = 0; if (++nl <= 16) dbg_ram("[IBFENCE] EVENT_WRITE_SHD init=0x%08X addr=0x%08X value=0x%08X\n", init, a, v);
              } else if (ibf && op == 0x3D && body >= 2) {                 // MEM_WRITE
                  uint32_t a = ib_read(guest_base, pos + 1);
                  for (uint32_t i = 0; i + 1 < body; ++i) pm4_gpu_write(a + i * 4, ib_read(guest_base, pos + 2 + i));
                  static int nl = 0; if (++nl <= 16) dbg_ram("[IBFENCE] MEM_WRITE addr=0x%08X n=%u first=0x%08X\n", a, body - 1, ib_read(guest_base, pos + 2));
              } else if (op == 0x3C && body >= 5) {                        // WAIT_REG_MEM (Xenia semantics)
                  // Waits until (reg or memory & mask) <cmp> ref. The CP stalls here on hardware; we hand
                  // the turn to the CPU side (e.g. the tile worker that writes the acknowledgement) and
                  // re-check, bounded so a never-satisfied wait cannot deadlock. Default OFF since 2026-10-05 (froze the game after the hub); LSWTCS_WAITREG=1 enables.
                  static int wr = -1; if (wr < 0) { const char* e = getenv("LSWTCS_WAITREG"); wr = (e && e[0] == '1') ? 1 : 0; }
                  if (wr) {
                      uint32_t wi = ib_read(guest_base, pos + 1), addr = ib_read(guest_base, pos + 2), ref = ib_read(guest_base, pos + 3),
                               mask = ib_read(guest_base, pos + 4);
                      bool is_mem = (wi & 0x10) != 0;
                      auto value = [&]() -> uint32_t {
                          if (is_mem) {
                              uint32_t a = (addr & ~3u) & 0x1FFFFFFFu, v;
                              memcpy(&v, g_base + 0x80000000ull + a, 4);   // host load of the guest bytes
                              switch (addr & 3) {
                                  case 1: v = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu); break;
                                  case 2: v = __builtin_bswap32(v); break;
                                  case 3: v = (v << 16) | (v >> 16); break;
                                  default: break;
                              }
                              return v;
                          }
                          uint32_t r = addr & 0x7FFF;
                          if (r == 0x0A31) g_xe_regs[r] &= 0x7FFFFFFFu;   // COHER_STATUS_HOST: MakeCoherent clears busy
                          return r < 0x6000 ? g_xe_regs[r] : 0;
                      };
                      auto match = [&](uint32_t v) -> bool {
                          v &= mask;
                          uint32_t bits = (uint32_t(v < ref) << 1) | (uint32_t(v <= ref) << 2) | (uint32_t(v == ref) << 3) |
                                          (uint32_t(v != ref) << 4) | (uint32_t(v >= ref) << 5) | (uint32_t(v > ref) << 6) | (1u << 7);
                          return ((bits >> (wi & 7)) & 1) != 0;
                      };
                      int it = 0;
                      while (!match(value()) && it < 4000) { sched_yield_turn(); ++it; }
                      static uint64_t nw = 0, nfail = 0; ++nw;
                      bool ok = match(value()); if (!ok) ++nfail;
                      if (nw <= 12 || (it && nw <= 200) || (!ok && nfail <= 20) || (nw % 50000) == 0)
                          dbg_ram("[WAITREG] #%llu %s addr=0x%08X ref=0x%08X mask=0x%08X fn=%u value=0x%08X yields=%d %s (fails %llu)\n",
                                  (unsigned long long)nw, is_mem ? "mem" : "reg", addr, ref, mask, wi & 7, value(), it, ok ? "ok" : "GAVE UP", (unsigned long long)nfail);
                  }
              } else if (op == 0x5A && body >= 2) {                        // EVENT_WRITE_EXT: screen extents
                  // D3D's predicated tiling derives each draw's bin mask from the screen extent the GPU
                  // reports here. Xenia fakes 'whole screen' (draw lands in every tile): min x/y 0,
                  // max x/y 8192>>3, z 0..1, as big-endian u16 (LSWTCS_EXTENT=0 skips).
                  static int ext = -1; if (ext < 0) { const char* e = getenv("LSWTCS_EXTENT"); ext = (e && e[0] == '0') ? 0 : 1; }
                  g_xe_regs[0x21F9] = ib_read(guest_base, pos + 1) & 0x3F;
                  uint32_t a = ib_read(guest_base, pos + 2) & ~3u;
                  if (ext && a < 0x20000000u) {
                      const uint16_t v[6] = {0, 8192 >> 3, 0, 8192 >> 3, 0, 1};
                      uint8_t* d = g_base + 0x80000000ull + a;
                      for (int i = 0; i < 6; ++i) { d[i * 2] = uint8_t(v[i] >> 8); d[i * 2 + 1] = uint8_t(v[i]); }
                  }
                  static uint64_t nl = 0; if (++nl <= 4 || (nl % 100000) == 0) dbg_ram("[EXTENT] #%llu addr=0x%08X\n", (unsigned long long)nl, a);
              } else if (op == 0x46 && body >= 1) {
                  static int nl = 0; if (++nl <= 16) dbg_ram("[IBFENCE] EVENT_WRITE init=0x%08X body=%u w1=0x%08X w2=0x%08X\n", ib_read(guest_base, pos + 1), body,
                                                             body >= 2 ? ib_read(guest_base, pos + 2) : 0, body >= 3 ? ib_read(guest_base, pos + 3) : 0);
              } }
            if (op == 0x2D && body >= 1) {                 // SET_CONSTANT
                uint32_t d1    = ib_read(guest_base, pos + 1);
                uint32_t ctype = (d1 >> 16) & 0xFF;
                uint32_t coff  = d1 & 0x7FF;
                uint32_t base  = (ctype==0)?0x4000 : (ctype==1)?0x4800 :
                                 (ctype==2)?0x4900 : (ctype==3)?0x4908 : (ctype==4)?0x2000 : 0;
                if (base) for (uint32_t i = 0; i < body - 1 && pos + 2 + i < dword_count; i++)
                    if (base + coff + i < 0x6000) { uint32_t v = ib_read(guest_base, pos + 2 + i); g_xe_regs[base + coff + i] = v; regset_note(base + coff + i, v); }
            } else if (op == 0x2F && body >= 3) {          // LOAD_ALU_CONSTANT (from memory)
                // Matches Xenia ExecutePacketType3_LOAD_ALU_CONSTANT. This is how the game
                // uploads its matrices/fetch constants (15k packets per run, OPHIST); it was
                // previously DROPPED, so the register file never held the real constants.
                // LSWTCS_LOADALU=0 restores the old behaviour.
                static int on = -1; if (on < 0) { const char* e = getenv("LSWTCS_LOADALU"); on = (e && e[0] == '0') ? 0 : 1; }
                if (on) {
                    uint32_t addr  = ib_read(guest_base, pos + 1) & 0x3FFFFFFFu;
                    uint32_t ot    = ib_read(guest_base, pos + 2);
                    uint32_t size  = ib_read(guest_base, pos + 3) & 0xFFFu;
                    uint32_t index = ot & 0x7FFu, ctype = (ot >> 16) & 0xFFu;
                    uint32_t rbase = (ctype == 0) ? 0x4000u : (ctype == 1) ? 0x4800u : (ctype == 2) ? 0x4900u :
                                     (ctype == 3) ? 0x4908u : (ctype == 4) ? 0x2000u : 0u;
                    uint32_t src = addr + 0x80000000u;
                    if (rbase && src >= 0x80000000u && src < 0xA0000000u) {
                        for (uint32_t i = 0; i < size; ++i) {
                            uint32_t r = rbase + index + i;
                            if (r >= 0x6000) break;
                            uint32_t v = ib_read(src, i);
                            g_xe_regs[r] = v; regset_note(r, v);
                        }
                    }
                    // XVS diag: every load that covers VS c0..c3 (dwords 0..15) — the matrix.
                    if (ctype == 0 && index < 16 && LSW_ENV_SET("LSWTCS_XVS")) {
                        static int m = 0;
                        if (++m <= 10 && src >= 0x80000000u && src < 0xA0000000u)
                            dbg_ram("[LOADALU-C0] index=%u size=%u src=0x%08X words=%08X %08X %08X %08X %08X %08X %08X %08X\n", index, size, src,
                                    ib_read(src,0), ib_read(src,1), ib_read(src,2), ib_read(src,3), ib_read(src,4), ib_read(src,5), ib_read(src,6), ib_read(src,7));
                    }
                    static int n = 0;
                    if (LSW_ENV_SET("LSWTCS_OPHIST") && ++n <= 12)
                        dbg_ram("[LOADALU#%d] type=%u index=%u size=%u src=0x%08X first=%08X\n", n, ctype, index, size, src,
                                (src >= 0x80000000u && src < 0xA0000000u && size) ? ib_read(src, 0) : 0);
                }
            } else if ((op == 0x55 || op == 0x56) && body >= 2) {   // SET_CONSTANT2 / SET_SHADER_CONSTANTS
                uint32_t index = ib_read(guest_base, pos + 1) & 0xFFFFu;   // absolute register index
                for (uint32_t i = 0; i + 1 < body && pos + 2 + i < dword_count; ++i) {
                    uint32_t r = index + i; if (r >= 0x6000) break;
                    uint32_t v = ib_read(guest_base, pos + 2 + i); g_xe_regs[r] = v; regset_note(r, v);
                }
            } else if (op == 0x27 && body >= 2) {          // IM_LOAD (microcode by addr)
                g_imload27++;
                uint32_t d1 = ib_read(guest_base, pos + 1);
                uint32_t d2 = ib_read(guest_base, pos + 2);
                uint32_t stype = d1 & 0x3;                  // 0=VS, 1=PS
                uint32_t gaddr = (d1 & ~0xFu) + 0x80000000u;
                uint32_t ssize = d2 & 0xFFFF;
                if (g_pm4_trace_on) pm4_trace(depth, "   IM_LOAD %s addr=0x%08X size=%u dw",
                                              stype ? "PS" : "VS", gaddr, ssize);
                { static uint32_t nst = 0; uint32_t start = d2 >> 16;   // DIAG: instruction-memory start offset
                  if (start && nst < 60) { ++nst; printf("[IMSTART] IM_LOAD %s addr=0x%08X start=%u size=%u SQ_PROGRAM_CNTL=%08X SQ_VS_PROGRAM=%08X SQ_PS_PROGRAM=%08X\n",
                      stype ? "PS" : "VS", gaddr, start, ssize, g_xe_regs[0x2180], g_xe_regs[0x21F7], g_xe_regs[0x21F6]); fflush(stdout); } }
                if (stype == 0) { g_vs_addr = gaddr; g_vs_size = ssize; imlog_vs("IM_LOAD", gaddr, ssize); ucw_add(gaddr, ssize); }
                else            { g_ps_addr = gaddr; g_ps_size = ssize; if (g_imlog_left > 0) { printf("[IMLOG] IM_LOAD PS addr=0x%08X size=%u\n", gaddr, ssize); fflush(stdout); } }
                // DIAG (always on, rate-limited): the same VS address loaded with different microcode
                // (runtime shader patching, e.g. vfetch stride) -> [UCODE] line.
                if (stype == 0 && gaddr >= 0x80000000u && gaddr < 0xA0000000u && ssize && ssize < 0x4000) {
                    static std::unordered_map<uint32_t, uint64_t> seen; static uint32_t nlog = 0;
                    const uint8_t* p = g_base + gaddr; uint64_t h = 1469598103934665603ull;
                    for (uint32_t i = 0; i < ssize * 4; ++i) h = (h ^ p[i]) * 1099511628211ull;
                    auto it = seen.find(gaddr);
                    if (it != seen.end() && it->second != h && nlog < 200) {
                        ++nlog; printf("[UCODE] VS 0x%08X (%u dw) changed %016llX -> %016llX\n", gaddr, ssize,
                                       (unsigned long long)it->second, (unsigned long long)h); fflush(stdout);
                    }
                    seen[gaddr] = h;
                }
            } else if (op == 0x2B && body >= 2) {          // IM_LOAD_IMMEDIATE (inline microcode)
                // Xenos PM4_IM_LOAD_IMMEDIATE = 0x2B (NOT 0x28 — earlier code had the wrong
                // opcode, so this never fired and looked like "no shaders loaded"). Packet:
                // d0=shader_type(0=VS,1=PS), d1=start<<16|size_dwords, then inline microcode.
                g_imload28++;
                uint32_t d1 = ib_read(guest_base, pos + 1);  // shader_type | start
                uint32_t d2 = ib_read(guest_base, pos + 2);  // start<<16 | size_dwords
                uint32_t stype = d1 & 0x3;
                uint32_t gaddr = guest_base + (pos + 3) * 4;  // inline microcode location
                uint32_t ssize = d2 & 0xFFFF;
                { static uint32_t nst = 0; uint32_t start = d2 >> 16;   // DIAG: instruction-memory start offset
                  if (start && nst < 60) { ++nst; printf("[IMSTART] IM_LOAD_IMMEDIATE %s start=%u size=%u SQ_VS_PROGRAM=%08X SQ_PS_PROGRAM=%08X\n",
                      stype ? "PS" : "VS", start, ssize, g_xe_regs[0x21F7], g_xe_regs[0x21F6]); fflush(stdout); } }
                if (stype == 0) { g_vs_addr = gaddr; g_vs_size = ssize; imlog_vs("IM_LOAD_IMM", gaddr, ssize); }
                else            { g_ps_addr = gaddr; g_ps_size = ssize; if (g_imlog_left > 0) { printf("[IMLOG] IM_LOAD_IMM PS addr=0x%08X size=%u\n", gaddr, ssize); fflush(stdout); } }
            } else if (op == 0x21 && body >= 3) {            // REG_RMW (NOT a draw)
                // Register read-modify-write. Xenia semantics: reg = rmw_info & 0x1FFF;
                // value = regfile[reg]; if(bit31) value &= regfile[and_mask&0x1FFF] else value &= and_mask;
                // if(bit30) value |= regfile[or_mask&0x1FFF] else value |= or_mask; writeReg(reg,value).
                uint32_t rmw_info = ib_read(guest_base, pos + 1);
                uint32_t and_mask = ib_read(guest_base, pos + 2);
                uint32_t or_mask  = ib_read(guest_base, pos + 3);
                uint32_t reg = rmw_info & 0x1FFF;
                uint32_t v = g_xe_regs[reg];
                v &= (rmw_info & 0x80000000u) ? g_xe_regs[and_mask & 0x1FFF] : and_mask;
                v |= (rmw_info & 0x40000000u) ? g_xe_regs[or_mask & 0x1FFF]  : or_mask;
                g_xe_regs[reg] = v;
                regset_note(reg, v);
            } else if (op == 0x22 || op == 0x36) {           // DRAW_INDX / DRAW_INDX_2
                g_draw_total++;
                uint32_t mode = g_xe_regs[XE_RB_MODECONTROL] & 0x7;
                extern uint32_t g_draw_mode_hist[8]; g_draw_mode_hist[mode & 7]++;
                framemap_event(mode);
                // DIAG (2026-06-16): unconditionally log the first 16 DRAW packets so we can tell
                // whether these are REAL draws (sane prim/num_indices/VS addr) or misparsed dwords,
                // and at what IB depth they sit.
                if (LSW_ENV_SET("LSWTCS_DRAWLOG")) {
                    static int s_drawlog = 0;
                    if (s_drawlog < 16) { s_drawlog++;
                        uint32_t di = ib_read(guest_base, pos + (op == 0x22 ? 2 : 1));
                        dbg_ram("[DRAWLOG#%d] op=0x%02X depth=%d body=%u prim=%u num_indices=%u mode=%u "
                                "VS=0x%08X/%u PS=0x%08X/%u IB=0x%08X@%u\n",
                                s_drawlog, op, depth, body, di & 0x3F, (di >> 16) & 0xFFFF, mode,
                                g_vs_addr, g_vs_size, g_ps_addr, g_ps_size, guest_base, pos);
                    }
                }
                // DRAWLOG2 (env LSWTCS_DRAWLOG2, removable): the first 24 draws whose VS is NOT the
                // 27-dword system/fade shader — i.e. real menu geometry after the titles load. Dumps
                // the raw packet body (DRAW_INDX 0x22: [0]viz [1]initiator [2]index base [3]index
                // size/type; DRAW_INDX_2 0x36: [0]initiator then inline indices) plus the first
                // vertex fetch constant, to design general indexed-draw support (Phase 3).
                if (LSW_ENV_SET("LSWTCS_DRAWLOG2") && g_vs_size > 40 && mode == 4) {
                    static int s_d2 = 0;
                    if (s_d2 < 24) { s_d2++;
                        uint32_t w[6]; for (int i = 0; i < 6; ++i) w[i] = (uint32_t)i < body ? ib_read(guest_base, pos + 1 + i) : 0;
                        dbg_ram("[DRAWLOG2#%d] op=0x%02X body=%u mode=%u VS=0x%08X/%u PS=0x%08X/%u pkt=%08X %08X %08X %08X %08X %08X vfc0=%08X %08X vfc1=%08X %08X\n",
                                s_d2, op, body, mode, g_vs_addr, g_vs_size, g_ps_addr, g_ps_size,
                                w[0], w[1], w[2], w[3], w[4], w[5],
                                g_xe_regs[0x4800], g_xe_regs[0x4801], g_xe_regs[0x4802], g_xe_regs[0x4803]);
                        // Vertex fetch constants are 2-dword pairs with type (dw0 & 3) == 3:
                        // base = dw0 & ~3 (physical bytes), size = (dw1 >> 2) & 0xFFFFFF dwords,
                        // endian = dw1 & 3. List every type-3 pair across the 96 pair slots.
                        char vb[600]; int k = 0;
                        for (uint32_t pi = 48; pi < 96 && k < 560; ++pi) {
                            uint32_t a = g_xe_regs[0x4800 + pi * 2], b = g_xe_regs[0x4801 + pi * 2];
                            if ((a & 3u) == 3u)
                                k += snprintf(vb + k, sizeof vb - k, " [%u]base=0x%08X sz=%u end=%u", pi, a & ~3u, (b >> 2) & 0xFFFFFFu, b & 3u);
                        }
                        vb[k] = 0;
                        dbg_ram("[DRAWLOG2#%d]   vfetch-type3:%s\n", s_d2, k ? vb : " (none)");
                    }
                }
                if (g_pm4_trace_on) {
                    uint32_t di = ib_read(guest_base, pos + (op == 0x22 ? 2 : 1));
                    pm4_trace(depth, "   DRAW %s prim=%u num_indices=%u mode=%u VS=0x%08X/%u PS=0x%08X/%u",
                              pm4_op_name(op), di & 0x3F, (di >> 16) & 0xFFFF, mode,
                              g_vs_addr, g_vs_size, g_ps_addr, g_ps_size);
                }
                if (mode == 5 && g_vs_addr && gpurt_on()) {  // kDepth: depth-only pass (GPU path only)
                    uint32_t init5 = ib_read(guest_base, pos + (op == 0x22 ? 2 : 1));
                    uint32_t src5 = (init5 >> 6) & 3;
                    if (op == 0x22 && body >= 4)
                        gpu_record_draw(ib_read(guest_base, pos + 2), ib_read(guest_base, pos + 3), ib_read(guest_base, pos + 4), nullptr, 0, 5);
                    else if (src5 == 2)
                        gpu_record_draw(init5, 0, 0, nullptr, 0, 5);
                    else if (op == 0x36 && src5 == 1 && body >= 2)
                        gpu_record_draw(init5, 0, 0, reinterpret_cast<const uint8_t*>(g_base + guest_base + (pos + 2) * 4), body - 1, 5);
                }
                if (mode == 4 && g_vs_addr) {               // kColorDepth geometry → collect
                    uint32_t init = ib_read(guest_base, pos + (op == 0x22 ? 2 : 1));
                    uint32_t nidx = (init >> 16) & 0xFFFF;
                    uint32_t vb = (g_xe_regs[0x4800] & ~0x3u) + 0x80000000u;
                    // DIAG: log each DISTINCT (VS first-dword,size) menu shader once, with its PS
                    // and bound texture fetch const (0x4900 = SET_CONSTANT type2). Identifies what
                    // the menu draws are (match VS bytes to xenia/shdump) + whether textures bind.
                    if (LSW_ENV_SET("LSWTCS_SHADERID")) {
                        static uint32_t seen[32]; static int nseen=0;
                        uint32_t vd0 = (g_vs_addr>=0x82000000u&&g_vs_addr<0xA0000000u)?ib_read(g_vs_addr,0):0;
                        uint32_t key = vd0 ^ (g_vs_size<<24);
                        bool nw=true; for(int i=0;i<nseen;i++) if(seen[i]==key) nw=false;
                        if (nw && nseen<32) { seen[nseen++]=key;
                            uint32_t pd0=(g_ps_addr>=0x82000000u&&g_ps_addr<0xA0000000u)?ib_read(g_ps_addr,0):0;
                            VtxFmt vf; bool dec = geom_decode_vs(g_vs_addr, g_vs_size, &vf);
                            dbg_ram("[SHADERID#%d] VSd0=%08X sz=%u PSd0=%08X sz=%u nidx=%u | DECODE=%d stride=%u pos@%d col@%d(fmt%d) uv@%d(fmt%d)\n",
                                    nseen, vd0, g_vs_size, pd0, g_ps_size, nidx,
                                    dec?1:0, vf.stride, vf.pos_off, vf.col_off, vf.col_fmt, vf.uv_off, vf.uv_fmt);
                            // Full 6-dword texture fetch const 0 (0x4900): [0]type/sign/clamp, [1]base/pitch,
                            // [2]w/h, [3]format/swizzle, [4]filter, [5]mip — for texture decode.
                            dbg_ram("[TEXCONST#%d] fc0= %08X %08X %08X %08X %08X %08X\n", nseen,
                                    g_xe_regs[0x4900],g_xe_regs[0x4901],g_xe_regs[0x4902],
                                    g_xe_regs[0x4903],g_xe_regs[0x4904],g_xe_regs[0x4905]);
                        }
                    }
                    if (g_xvs_on < 0) {
                        { const char* e = getenv("LSWTCS_XVS"); g_xvs_on = (e && e[0] == '0') ? 0 : 1; }   // default ON (GPU-path fallback)
                        const char* t = getenv("LSWTCS_XVS_TINT"); g_xvs_tint = (t && t[0] == '1') ? 1 : 0;
                    }
                    // Route every draw through the real VS on the CPU. LSWTCS_XVS_ALLDRAWS=0 keeps the
                    // old behaviour (only DMA-indexed DRAW_INDX; the rest went to the UI-only path,
                    // which assumes a pass-through VS -- backgrounds/starfields vanished there).
                    static int alld = -1; if (alld < 0) { const char* e = getenv("LSWTCS_XVS_ALLDRAWS"); alld = (e && e[0] == '0') ? 0 : 1; }
                    uint32_t init0 = ib_read(guest_base, pos + (op == 0x22 ? 2 : 1));
                    uint32_t srcsel = (init0 >> 6) & 3;   // 0 = DMA indices, 1 = immediate, 2 = auto
                    bool gpu_done = false;
                    if (op == 0x22 && body >= 4)
                        gpu_done = gpu_record_draw(ib_read(guest_base, pos + 2), ib_read(guest_base, pos + 3), ib_read(guest_base, pos + 4), nullptr, 0);
                    else if (srcsel == 2)
                        gpu_done = gpu_record_draw(init0, 0, 0, nullptr, 0);
                    else if (op == 0x36 && srcsel == 1 && body >= 2)
                        gpu_done = gpu_record_draw(init0, 0, 0, reinterpret_cast<const uint8_t*>(g_base + guest_base + (pos + 2) * 4), body - 1);
                    if (!gpu_done) {   // census of draws the GPU path did not take
                        static uint32_t miss[64]; static uint32_t nmiss = 0;
                        uint32_t key = ((op == 0x36) ? 32u : 0u) | (srcsel << 3) | ((init0 & 0x3F) < 8 ? (init0 & 7) : 7);
                        miss[key & 63]++;
                        if ((++nmiss % 20000) == 0) {
                            char b[512]; int k = snprintf(b, sizeof b, "[GPUMISS] after %u:", nmiss);
                            for (int i = 0; i < 64 && k < 480; ++i) if (miss[i]) k += snprintf(b + k, sizeof b - k, " op%s/src%u/prim%u=%u", (i & 32) ? "36" : "22", (i >> 3) & 3, i & 7, miss[i]);
                            dbg_ram("%s\n", b);
                        }
                    }
                    if (gpu_done) {
                    } else
                    if (g_xvs_on && op == 0x22 && body >= 4) {
                        // Real VS on the CPU for DMA-indexed draws (the menu geometry).
                        xvs_draw(ib_read(guest_base, pos + 2), ib_read(guest_base, pos + 3),
                                 ib_read(guest_base, pos + 4));
                    } else if (g_xvs_on && alld && srcsel == 2) {
                        xvs_draw(init0, 0, 0);                                    // auto-indexed
                    } else if (g_xvs_on && alld && op == 0x36 && srcsel == 1 && body >= 2) {
                        // Inline indices follow the initiator inside the packet: present them as a
                        // DMA index buffer at the packet's physical address.
                        bool i32 = (init0 >> 11) & 1;
                        uint32_t phys = (guest_base + (pos + 2) * 4) - 0x80000000u;
                        uint32_t endian = i32 ? 2u : 1u;                          // k8in32 / k8in16
                        xvs_draw(init0 & ~0xC0u, phys, ((body - 1) * (i32 ? 1u : 2u)) | (endian << 30));
                    } else {
                        geom_collect_draw(vb, nidx, g_vs_addr, g_vs_size);
                    }
                }
                if (mode == 6) {                            // kCopy = EDRAM resolve
                    g_resolve_total++;
                    gpu_record_resolve();
                    uint32_t dest = g_xe_regs[XE_RB_COPY_DEST_BASE];
                    if (dest) {
                        g_resolve_dest.store(dest, std::memory_order_relaxed);
                        g_resolve_clear.store(g_xe_regs[XE_RB_COLOR_CLEAR], std::memory_order_relaxed);
                        g_resolve_pending.store(true, std::memory_order_relaxed);
                    }
                } else if (g_draws_captured < 6 && mode == 4 && g_vs_addr) {   // DIAG: capture first SHADED mode-4 draws
                    dbg_ram("[DRAWANY#%d] mode=%u MODECONTROL=0x%08X\n", g_draws_captured+1, mode, g_xe_regs[XE_RB_MODECONTROL]);
                    // Capture the first few steady-state GEOMETRY draws to scope the
                    // shader translator: draw params + shader/vertex state + microcode.
                    g_draws_captured++;
                    if (g_draws_captured == 1)
                        dbg_ram("[DRAWCAP] first geometry draw at ~%llu ms\n", _kstub_start_ms());
                    uint32_t init = ib_read(guest_base, pos + (op == 0x22 ? 2 : 1));
                    dbg_ram("[DRAWCAP#%d] op=0x%02X prim=%u num_indices=%u init=0x%08X imload27=%u imload28=%u\n",
                           g_draws_captured, op, init & 0x3F, (init >> 16) & 0xFFFF, init,
                           g_imload27, g_imload28);
                    dbg_ram("[DRAWCAP#%d] SQ_PROGRAM_CNTL=0x%08X SURFACE_INFO=0x%08X COLOR_INFO=0x%08X VS=0x%08X/%u PS=0x%08X/%u\n",
                           g_draws_captured, g_xe_regs[XE_SQ_PROGRAM_CNTL],
                           g_xe_regs[XE_RB_SURFACE_INFO], g_xe_regs[XE_RB_COLOR_INFO],
                           g_vs_addr, g_vs_size, g_ps_addr, g_ps_size);
                    for (uint32_t i = 0; i < 8; i += 2)
                        if (g_xe_regs[0x4800 + i] | g_xe_regs[0x4800 + i + 1])
                            dbg_ram("[DRAWCAP#%d]   vf[%u] %08X %08X\n", g_draws_captured, i/2,
                                   g_xe_regs[0x4800 + i], g_xe_regs[0x4800 + i + 1]);
                    // Dump the vertex buffer content (fetch const 0 base) to see the format.
                    // Include the GPU/ring region (0x9F2xxxxx) where UI vertex buffers live.
                    { uint32_t vb = (g_xe_regs[0x4800] & ~0x3u) + 0x80000000u;
                      uint32_t vsz = g_xe_regs[0x4801];   // fetch const dword1 (size/stride bits)
                      if (vb >= 0x82000000u && vb < 0xA0000000u) {
                        dbg_ram("[DRAWCAP#%d]   VB @0x%08X sizecst=0x%08X:\n", g_draws_captured, vb, vsz);
                        for (uint32_t i = 0; i < 24; i += 4)
                            dbg_ram("[DRAWCAP#%d]     %08X %08X %08X %08X\n", g_draws_captured,
                                   ib_read(vb,i), ib_read(vb,i+1), ib_read(vb,i+2), ib_read(vb,i+3));
                      } }
                    auto dump_shader = [&](const char* tag, uint32_t g, uint32_t sz){
                        if (g < 0x82000000u || g >= 0xA0000000u) return;  // incl GPU region 0x9F2xxxxx
                        uint32_t n = sz ? (sz < 30 ? sz : 30) : 30;
                        dbg_ram("[DRAWCAP#%d] %s @0x%08X (%u dw):\n", g_draws_captured, tag, g, n);
                        for (uint32_t i = 0; i + 2 < n; i += 3)
                            dbg_ram("[DRAWCAP#%d]   %08X %08X %08X\n", g_draws_captured,
                                   ib_read(g, i), ib_read(g, i+1), ib_read(g, i+2));
                    };
                    if (g_vs_addr) dump_shader("VS", g_vs_addr, g_vs_size);
                    if (g_ps_addr) dump_shader("PS", g_ps_addr, g_ps_size);

                }
            } else if (op == 0x3F || op == 0x37) {         // nested IB
                uint32_t na = ib_read(guest_base, pos + 1);
                uint32_t nl = ib_read(guest_base, pos + 2) & 0xFFFFF;
                if (g_pm4_trace_on) pm4_trace(depth, "   -> nested IB addr=0x%08X len=%u dw", na, nl);
                g_chain_nested++;
                pm4_extract_ib(na, nl, depth + 1);
            }
            pos += total; continue;
        }
        if (type == 1) {
            // Type-1 packet: header + two register writes (Xenia ExecutePacketType1).
            // Previously fell through to pos++, so the two DATA words were parsed as packet
            // headers — misparsing whatever followed. LSWTCS_PM4T1=0 restores the old skip.
            static int t1 = -1; if (t1 < 0) { const char* e = getenv("LSWTCS_PM4T1"); t1 = (e && e[0] == '0') ? 0 : 1; }
            if (t1 && pos + 2 < dword_count) {
                uint32_t r1 = hdr & 0x7FF, r2 = (hdr >> 11) & 0x7FF;
                uint32_t d1 = ib_read(guest_base, pos + 1), d2 = ib_read(guest_base, pos + 2);
                g_xe_regs[r1] = d1; regset_note(r1, d1); pm4_reg_side_effect(r1, d1);
                g_xe_regs[r2] = d2; regset_note(r2, d2); pm4_reg_side_effect(r2, d2);
                pos += 3; continue;
            }
        }
        pos++;
    }
}

// Dump the captured RB render-state once we've seen some draws (recon).
static void pm4_dump_state() {
    dbg_ram("[RBSTATE] draws=%u resolves=%u\n", g_draw_total, g_resolve_total);
    dbg_ram("[RBSTATE]  SURFACE_INFO=0x%08X COLOR_INFO=0x%08X DEPTH_INFO=0x%08X MODECONTROL=0x%08X\n",
           g_xe_regs[XE_RB_SURFACE_INFO], g_xe_regs[XE_RB_COLOR_INFO],
           g_xe_regs[XE_RB_DEPTH_INFO], g_xe_regs[XE_RB_MODECONTROL]);
    dbg_ram("[RBSTATE]  COPY_CONTROL=0x%08X COPY_DEST_BASE=0x%08X PITCH=0x%08X INFO=0x%08X\n",
           g_xe_regs[XE_RB_COPY_CONTROL], g_xe_regs[XE_RB_COPY_DEST_BASE],
           g_xe_regs[XE_RB_COPY_DEST_PITCH], g_xe_regs[XE_RB_COPY_DEST_INFO]);
    dbg_ram("[RBSTATE]  COLOR_CLEAR=0x%08X COLOR_CLEAR_LO=0x%08X DEPTH_CLEAR=0x%08X\n",
           g_xe_regs[XE_RB_COLOR_CLEAR], g_xe_regs[XE_RB_COLOR_CLEAR_LO],
           g_xe_regs[XE_RB_DEPTH_CLEAR]);

}

// Fire graphics interrupt for each set bit in cpu_mask.
// g_interrupt_ctx must be set by the caller (VdSwap sets it around pm4_process_ring).
static void pm4_fire_interrupt(uint32_t cpu_mask) {
    if (!g_interrupt_ctx || !g_gfx_interrupt_callback) return;
    PPCFunc* fn = PPC_LOOKUP_FUNC(g_base, g_gfx_interrupt_callback);
    if (!fn) return;
    uint32_t fired_cb = 0;
    if (g_gfx_interrupt_data) { uint8_t* base = g_base; uint32_t sa = PPC_LOAD_U32(g_gfx_interrupt_data + 10900);
        if (sa >= 0x82000000u && sa < 0xC0000000u) fired_cb = PPC_LOAD_U32(sa + 16); }
    // Guard: the D3D ISR traps (twi) when scratch4 holds the 0x0BADF00D poison the
    // runtime writes AFTER a serviced interrupt. Only a stale/replayed INTERRUPT can see it.
    if (g_gfx_interrupt_data) {
        uint8_t* base = g_base;
        uint32_t sa = PPC_LOAD_U32(g_gfx_interrupt_data + 10900);
        if (sa >= 0x82000000u && sa < 0xC0000000u) {
            uint32_t cb = PPC_LOAD_U32(sa + 16);
            static uint64_t n_ok = 0, n_poison = 0;
            if (cb == 0x0BADF00Du) { if (++n_poison <= 5 || (n_poison % 1000) == 0) dbg_ram("[IRQ] skip: scratch4=BADF00D (stale INTERRUPT) #%llu\n", (unsigned long long)n_poison); return; }
            if (++n_ok <= 10 || (n_ok % 500) == 0) dbg_ram("[IRQ] #%llu mask=0x%02X cb=0x%08X arg=0x%08X\n", (unsigned long long)n_ok, cpu_mask, cb, PPC_LOAD_U32(sa + 20));
        }
    }
    for (int n = 0; n < 6; n++) {
        if (!(cpu_mask & (1u << n))) continue;
        PPCContext icb = *g_interrupt_ctx;
        icb.r1.u32 -= 0x200;
        icb.r3.u32  = 1;
        icb.r4.u32  = g_gfx_interrupt_data;
        // Xenia EmulateCPInterruptDPC: SetActiveCpu(n) — the ISR and the deferred callback
        // read the CPU id at PCR+268 (worker slot dev+11316+cpu*80, ack bit 1<<cpu).
        uint8_t* base = g_base;
        uint32_t pcr = icb.r13.u32;
        uint8_t saved = (pcr >= 0x80000000u) ? PPC_LOAD_U8(pcr + 268) : 0;
        if (pcr >= 0x80000000u) PPC_STORE_U8(pcr + 268, (uint8_t)n);
        uint64_t gp0 = gp_now();
        fn(icb, g_base);
        g_gpuprof[GP_IRQ] += gp_now() - gp0;
        gpu_shm_new_epoch();
        if (pcr >= 0x80000000u) PPC_STORE_U8(pcr + 268, saved);
    }
    if (g_gfx_interrupt_data) {
        uint8_t* base = g_base;
        uint32_t mp = PPC_LOAD_U32(g_gfx_interrupt_data + 10900);
        if (mp >= 0x82000000u && mp < 0x94000000u)
            PPC_STORE_U32(mp, 0);
    }
    // TILEDRAIN (LSWTCS_TILEDRAIN, default on): the predicated-tiling callback 0x822D2BA0 only
    // queues the tile list (4-slot ring per CPU at dev+11316+cpu*80) and wakes worker sub_822D2AB0.
    // On hardware the worker runs at once; under our scheduler it lagged, the ring wrapped and it read
    // command-buffer chunks the producer had already recycled (garbage records -> null handler).
    // Interpret the queued lists right here, as the CP: sub_822D2798(slot) takes the slot lock, walks
    // each list (sub_822D2338 kicks its 0x81 segments into the ring) and pops the queue.
    if (fired_cb == 0x822D2BA0u && g_gfx_interrupt_data) {
        // LSWTCS_TILEDRAIN: 1 (default) = hand the turn to the real worker until every queue is empty
        // (exactly one list processor: running sub_822D2798 here as well corrupts its barrier);
        // 2 = interpret on this thread (only safe if the worker never runs); 0 = off.
        static int on = -1; if (on < 0) { const char* e = getenv("LSWTCS_TILEDRAIN"); on = e ? atoi(e) : 1; }
        if (on == 1) { extern bool g_tile_drain_pending; g_tile_drain_pending = true; }
        if (on == 3) {
            uint8_t* base = g_base;
            auto backlog = [&]() -> uint32_t {
                uint32_t b = 0;
                for (uint32_t c = 0; c < 6; ++c) { uint32_t slot = g_gfx_interrupt_data + 11316 + c * 80;
                    if (PPC_LOAD_U32(slot)) b += PPC_LOAD_U32(slot + 56) - PPC_LOAD_U32(slot + 60); }
                return b;
            };
            int spins = 0;
            while (backlog() && spins < 20000) { sched_yield_turn(); ++spins; }
            static uint64_t nd = 0; uint32_t left = backlog();
            if (++nd <= 12 || (nd % 1000) == 0 || left)
                { static int nl = 0; if (nl++ < 200) dbg_ram("[TILEDRAIN] #%llu yields=%d left=%u\n", (unsigned long long)nd, spins, left); }
        }
        PPCFunc* drain = on == 2 ? PPC_LOOKUP_FUNC(g_base, 0x822D2798u) : nullptr;
        if (drain) {
            uint8_t* base = g_base;
            for (uint32_t c = 0; c < 6; ++c) {
                uint32_t slot = g_gfx_interrupt_data + 11316 + c * 80;
                for (int it = 0; it < 8 && PPC_LOAD_U32(slot) != 0 && PPC_LOAD_U32(slot + 56) != PPC_LOAD_U32(slot + 60); ++it) {
                    uint32_t backlog = PPC_LOAD_U32(slot + 56) - PPC_LOAD_U32(slot + 60);
                    uint32_t q = PPC_LOAD_U32(slot + 64 + 4 * (PPC_LOAD_U32(slot + 60) & 3));
                    static uint64_t nd = 0;
                    if (++nd <= 12 || (nd % 1000) == 0)
                        dbg_ram("[TILEDRAIN] #%llu cpu=%u backlog=%u list=0x%08X first=0x%08X\n", (unsigned long long)nd, c, backlog, q,
                                (q >= 0x80000000u) ? PPC_LOAD_U32(q + 4) : 0);
                    PPCContext dc = *g_interrupt_ctx;
                    dc.r1.u32 -= 0x400;
                    dc.r3.u32 = slot;
                    uint32_t pcr = dc.r13.u32;
                    uint8_t saved = (pcr >= 0x80000000u) ? PPC_LOAD_U8(pcr + 268) : 0;
                    if (pcr >= 0x80000000u) PPC_STORE_U8(pcr + 268, (uint8_t)c);
                    drain(dc, g_base);
                    if (pcr >= 0x80000000u) PPC_STORE_U8(pcr + 268, saved);
                }
            }
        }
    }
}

// Walk the ring buffer from g_rb_rptr_dwords up to wptr_dwords, executing
// side-effects for fence/memory-write PM4 packets and stopping at PM4_XE_SWAP.
//
// Handled opcodes (Xenos — see xenia-canary/src/xenia/gpu/xenos.h):
//   0x46 PM4_EVENT_WRITE     — fence: write value to memory when event fires
//   0x58 PM4_EVENT_WRITE_SHD — VS|PS done fence: initiator|addr|value
//   0x3d PM4_MEM_WRITE       — write N dwords to guest memory
//   0x54 PM4_INTERRUPT       — fire graphics interrupt per cpu_mask bits
//   0x3f PM4_INDIRECT_BUFFER / 0x37 PM4_INDIRECT_BUFFER_PFD — recurse into IB
//   0x64 PM4_XE_SWAP         — Xenia end-of-frame marker; stop processing
//
// Type-0 (register writes) and Type-2 (NOPs) are silently skipped.
// g_interrupt_ctx is set by VdSwap so PM4_INTERRUPT can fire the callback inline.
static void pm4_process_ring(uint32_t wptr_dwords) {
    if (!g_rb_base || !g_rb_size) return;
    const uint32_t rb_dwords = g_rb_size >> 2;
    if (!rb_dwords) return;

    wptr_dwords %= rb_dwords;
    uint32_t pos = g_rb_rptr_dwords % rb_dwords;

    uint32_t dist = (wptr_dwords >= pos)
                  ? wptr_dwords - pos
                  : rb_dwords - pos + wptr_dwords;

    static uint32_t s_ev_logged = 0;
    uint32_t steps = 0;

    while (steps < dist) {
        uint32_t hdr  = rb_read(pos);
        uint32_t type = hdr >> 30;

        if (type == 2) {
            // Type-2: single NOP header
            pos = (pos + 1) % rb_dwords;
            steps++;
        } else if (type == 0) {
            // Type-0: register write header + body dwords — skip
            uint32_t body  = (hdr & 0x3FFF) + 1;
            uint32_t total = 1 + body;
            if (steps + total > dist) break;
            pos    = (pos + total) % rb_dwords;
            steps += total;
        } else if (type == 3) {
            uint32_t body  = ((hdr >> 16) & 0x3FFF) + 1;
            uint32_t op    = (hdr >> 8) & 0xFF;
            uint32_t total = 1 + body;
            if (steps + total > dist) break;

            if (op == 0x64) {
                // PM4_XE_SWAP — end of frame command stream
                pos    = (pos + total) % rb_dwords;
                steps += total;
                break;

            } else if (op == 0x46 && body >= 3) {
                // PM4_EVENT_WRITE — EOS fence write
                // Packet: header | initiator | write_addr | write_val
                uint32_t write_addr = rb_read((pos + 2) % rb_dwords);
                uint32_t write_val  = rb_read((pos + 3) % rb_dwords);
                if (write_addr >= 0x80000000u && write_addr < 0xA0000000u) {
                    pm4_store32(write_addr, write_val);
                    if (s_ev_logged < 32) {
                        uint32_t ev = rb_read((pos + 1) % rb_dwords) & 0x3F;
                        dbg_ram("[PM4] EVENT_WRITE ev=%u [0x%08X]=0x%08X\n",
                               ev, write_addr, write_val);

                        s_ev_logged++;
                    }
                }

            } else if (op == 0x58 && body >= 3) {
                // PM4_EVENT_WRITE_SHD — VS|PS done fence: header|initiator|addr|val
                uint32_t write_addr = rb_read((pos + 2) % rb_dwords);
                uint32_t write_val  = rb_read((pos + 3) % rb_dwords);
                if (write_addr >= 0x80000000u && write_addr < 0xA0000000u) {
                    pm4_store32(write_addr, write_val);
                    if (s_ev_logged < 32) {
                        dbg_ram("[PM4] EVENT_WRITE_SHD [0x%08X]=0x%08X\n",
                               write_addr, write_val);
 s_ev_logged++;
                    }
                }

            } else if (op == 0x3d && body >= 1) {
                // PM4_MEM_WRITE — write N dwords to guest memory
                // header | write_addr(bits[1:0]=endian) | data[0..N-1]
                uint32_t write_addr = rb_read((pos + 1) % rb_dwords) & ~3u;
                uint32_t data_cnt   = body - 1;
                if (write_addr >= 0x80000000u && write_addr < 0xA0000000u) {
                    for (uint32_t i = 0; i < data_cnt; i++) {
                        uint32_t val = rb_read((pos + 2 + i) % rb_dwords);
                        pm4_store32(write_addr + i * 4, val);
                    }
                    if (s_ev_logged < 32) {
                        dbg_ram("[PM4] MEM_WRITE [0x%08X] x%u dwords first=0x%08X\n",
                               write_addr, data_cnt,
                               rb_read((pos + 2) % rb_dwords));
 s_ev_logged++;
                    }
                }

            } else if (op == 0x54 && body >= 1) {
                // PM4_INTERRUPT — fire graphics interrupt per cpu_mask bits
                uint32_t cpu_mask = rb_read((pos + 1) % rb_dwords);
                if (s_ev_logged < 32) {
                    dbg_ram("[PM4] INTERRUPT cpu_mask=0x%02X\n", cpu_mask);
 s_ev_logged++;
                }
                pm4_fire_interrupt(cpu_mask);

            } else if ((op == 0x3f || op == 0x37) && body >= 2) {
                // PM4_INDIRECT_BUFFER / PM4_INDIRECT_BUFFER_PFD
                // IBs in this game contain only draw commands (no fence/interrupt
                // events), so parsing them provides no benefit yet and changes
                // OS thread scheduling enough to destabilise the audio thread.
                // Re-enable pm4_process_ib here once GPU draw emulation exists.
                uint32_t ib_addr = rb_read((pos + 1) % rb_dwords);
                uint32_t ib_len  = rb_read((pos + 2) % rb_dwords) & 0xFFFFF;
                // Reconnaissance: dump the IB command structure for the first few
                // frames so we can see what draw/state commands the game emits.
                static int s_dump_frames = 0;
                if (s_dump_frames < 3) {
                    s_dump_frames++;
                    g_pm4_dump_budget = 200;
                    g_pm4_draw_count  = 0;
                    dbg_ram("[IBDUMP] === frame IB addr=0x%08X len=%u ===\n", ib_addr, ib_len);
                    pm4_dump_ib(ib_addr, ib_len, 0);
                    dbg_ram("[IBDUMP] === end: %u draw packets ===\n", g_pm4_draw_count);

                } else if (s_ev_logged < 32) {
                    dbg_ram("[PM4] IB%s addr=0x%08X len=%u (skipped)\n",
                           op == 0x37 ? "_PFD" : "", ib_addr, ib_len);
 s_ev_logged++;
                }
                (void)ib_addr; (void)ib_len;
            }

            pos    = (pos + total) % rb_dwords;
            steps += total;
        } else {
            // Type-1 or unrecognised — skip one dword
            pos = (pos + 1) % rb_dwords;
            steps++;
        }
    }

    g_rb_rptr_dwords = pos;
}

// Walk the ring (the small g_rb_size buffer) modulo from our tracked read pointer,
// stopping at the NOP/zero write frontier. buffer_ptr is NOT a usable ring wptr
// (it's a non-monotonic IB-pool allocator head), so we self-pace off the frontier
// instead. Follows INDIRECT_BUFFER into draw IBs (pm4_extract_ib = read-only state
// capture; pm4_process_ib = fence servicing, gated off). Bounded to one ring lap.
static uint32_t s_rbflat_logged = 0;
static bool     g_pm4_exec_ibs  = false;   // service draw-IB fences? off (it crashes the game)
bool g_tile_drain_pending = false;
static bool g_pm4_walking = false;
static uint32_t pm4_tile_backlog() {
    if (!g_gfx_interrupt_data) return 0;
    uint8_t* base = g_base; uint32_t b = 0;
    for (uint32_t c = 0; c < 6; ++c) { uint32_t slot = g_gfx_interrupt_data + 11316 + c * 80;
        if (PPC_LOAD_U32(slot)) b += PPC_LOAD_U32(slot + 56) - PPC_LOAD_U32(slot + 60); }
    return b;
}
static void pm4_process_ring_flat_impl(bool allow_drain);
static void pm4_process_ring_flat() { pm4_process_ring_flat_impl(true); }
// Called from guest GPU-progress waits (ring space): act as the CP and consume the ring now.
// Re-entrant calls (the walker itself is running) fall back to the old "caught up" claim.
static void pm4_walk_now(void* ctxp) {
    static int on = -1; if (on < 0) { const char* e = getenv("LSWTCS_REALGET"); on = (e && e[0] == '0') ? 0 : 1; }
    if (!on || g_pm4_walking || !g_rb_base || !g_rb_size || !ctxp) return;
    PPCContext* saved = g_interrupt_ctx;
    if (!g_interrupt_ctx) g_interrupt_ctx = static_cast<PPCContext*>(ctxp);
    pm4_process_ring_flat_impl(false);
    g_interrupt_ctx = saved;
}
static void pm4_process_ring_flat_impl(bool allow_drain) {
    if (!g_rb_base || !g_rb_size) return;
    const uint32_t rbd = g_rb_size >> 2;        // 8192 dwords
    // ── Exact consumption up to the real write pointer (LSWTCS_RINGWPTR=1) ─────────────
    // Xenia ExecutePrimaryBuffer(read_ptr, write_ptr): every ring packet is executed exactly
    // once, in order. The NOP-frontier heuristic below re-walks stale ring entries (replaying
    // old IBs and their INTERRUPTs, whose deferred-list pointers are long recycled). The
    // kick (sub_822AE048) stores wptr (dwords) to CP_RB_WPTR = MMIO 0x7FC80714.
    { static int wm = -1; if (wm < 0) { const char* e = getenv("LSWTCS_RINGWPTR"); wm = (e && e[0] == '0') ? 0 : 1; }   // default ON (=0: old NOP-frontier walker)
      if (wm) {
        uint8_t* base = g_base;
        uint64_t gp_walk0 = gp_now();   // GPUPROF: drain-round yields (other threads' time) are excluded
        gpu_shm_new_epoch();
        g_pm4_walking = true;
        uint32_t w = PPC_LOAD_U32(0x7FC80714u) % rbd;
        uint32_t pos = g_rb_rptr_dwords % rbd, steps = 0, ibs = 0, ints = 0;
        for (int pass = 0; pass < 64; ++pass) {
        // Tile-list drain rounds (TILEDRAIN=1): after catching up, let the D3D tile worker interpret
        // the queued lists (it kicks their segments into the ring), then walk what it kicked.
        if (pass && pos == w && allow_drain && g_tile_drain_pending) {
            g_rb_rptr_dwords = pos;
            g_tile_drain_pending = false;
            g_pm4_walking = false;
            int spins = 0;
            g_gpuprof[GP_WALK] += gp_now() - gp_walk0;
            while (pm4_tile_backlog() && spins < 20000) { sched_yield_turn(); ++spins; }
            gp_walk0 = gp_now();
            gpu_shm_new_epoch();
            g_pm4_walking = true;
            pos = g_rb_rptr_dwords % rbd;   // the worker may have walked the ring itself (ring-space waits)
            static uint64_t nd = 0; if (++nd <= 12 || (nd % 1000) == 0 || pm4_tile_backlog())
                { static int nl = 0; if (nl++ < 200) dbg_ram("[TILEDRAIN] round #%llu yields=%d left=%u\n", (unsigned long long)nd, spins, pm4_tile_backlog()); }
            uint32_t w2 = PPC_LOAD_U32(0x7FC80714u) % rbd;
            if (w2 == pos) continue;
            w = w2;
        }
        else if (pass) { uint32_t w2 = PPC_LOAD_U32(0x7FC80714u) % rbd; if (w2 == w || pos != w) break; w = w2; }
        while (pos != w && steps < rbd) {
            uint32_t hdr = rb_read(pos), type = hdr >> 30, adv = 1;
            if (type == 0) {
                uint32_t cnt = ((hdr >> 16) & 0x3FFF) + 1, reg = hdr & 0x7FFF; bool one = (hdr >> 15) & 1u;
                for (uint32_t i = 0; i < cnt; ++i) { uint32_t r = one ? reg : reg + i, v = rb_read(pos + 1 + i);
                    if (r < 0x6000) { g_xe_regs[r] = v; pm4_reg_side_effect(r, v); } }
                adv = 1 + cnt;
            } else if (type == 1) {
                uint32_t r1 = hdr & 0x7FF, r2 = (hdr >> 11) & 0x7FF, d1 = rb_read(pos + 1), d2 = rb_read(pos + 2);
                g_xe_regs[r1] = d1; pm4_reg_side_effect(r1, d1); g_xe_regs[r2] = d2; pm4_reg_side_effect(r2, d2);
                adv = 3;
            } else if (type == 3) {
                uint32_t body = ((hdr >> 16) & 0x3FFF) + 1, op = (hdr >> 8) & 0x7F;
                adv = 1 + body;
                if (pm4_bin_packet(hdr, op, body, [&](uint32_t i) { return rb_read(pos + i); })) {}
                else if (op == 0x64 && rb_read(pos + 1) == 0x53574150u) geom_swap_packet(rb_read(pos + 2));
                else if ((op == 0x3F || op == 0x37) && body >= 2) {
                    pm4_extract_ib(rb_read(pos + 1), rb_read(pos + 2) & 0xFFFFF, 0);
                    if (g_pm4_exec_ibs) pm4_process_ib(rb_read(pos + 1), rb_read(pos + 2) & 0xFFFFF, 0);
                    ibs++;
                } else if (op == 0x54 && body >= 1) { pm4_fire_interrupt(rb_read(pos + 1)); ints++; }
                else if ((op == 0x46 || op == 0x58) && body >= 3 && g_pm4_exec_ibs) {
                    uint32_t wa = rb_read(pos + 2), wv = rb_read(pos + 3);
                    if (wa >= 0x80000000u && wa < 0xA0000000u) pm4_store32(wa, wv);
                }
            }
            pos = (pos + adv) % rbd; steps += adv;
        }
        }   // pass
        g_rb_rptr_dwords = pos;
        g_pm4_walking = false;
        g_gpuprof[GP_WALK] += gp_now() - gp_walk0;
        static uint64_t calls = 0, tot_ibs = 0; ++calls; tot_ibs += ibs;
        if (calls <= 5 || (calls % 600) == 0)
            dbg_ram("[RINGW] #%llu wptr=%u rptr=%u steps=%u ibs=%u ints=%u avgibs=%.2f\n",
                    (unsigned long long)calls, w, pos, steps, ibs, ints, (double)tot_ibs / calls);
        return;
      } }
    uint32_t pos     = g_rb_rptr_dwords % rbd;
    uint32_t scanned = 0, nop_run = 0, ibs = 0, fences = 0;

    while (scanned < rbd && ibs < 256) {
        uint32_t hdr = rb_read(pos);            // modulo read

        if (hdr == 0x80000000u || hdr == 0u) {  // NOP / unwritten → frontier
            if (++nop_run >= 16) break;
            pos = (pos + 1) % rbd; scanned++; continue;
        }
        nop_run = 0;
        uint32_t type = hdr >> 30;

        if (type == 2) { pos = (pos + 1) % rbd; scanned++; continue; }
        if (type == 0) {
            uint32_t cnt = ((hdr >> 16) & 0x3FFF) + 1;
            pos = (pos + 1 + cnt) % rbd; scanned += 1 + cnt; continue;
        }
        if (type == 3) {
            uint32_t body  = ((hdr >> 16) & 0x3FFF) + 1;
            uint32_t op    = (hdr >> 8) & 0x7F;
            uint32_t total = 1 + body;
            if (op == 0x64 && body >= 1 && rb_read((pos + 1) % rbd) == 0x53574150u) geom_swap_packet(rb_read((pos + 2) % rbd));
            if (op == 0x3F || op == 0x37) {             // INDIRECT_BUFFER → draw IB
                if (body >= 2) {
                    uint32_t ib_addr = rb_read((pos + 1) % rbd);
                    uint32_t ib_len  = rb_read((pos + 2) % rbd) & 0xFFFFF;
                    if (g_pm4_trace_on) pm4_trace(0, "RING -> IB addr=0x%08X len=%u dw", ib_addr, ib_len);
                    pm4_extract_ib(ib_addr, ib_len, 0);          // read-only capture
                    if (g_pm4_exec_ibs) pm4_process_ib(ib_addr, ib_len, 0);
                    ibs++;
                }
            } else if ((op == 0x46 || op == 0x58) && body >= 3 && g_pm4_exec_ibs) {
                uint32_t wa = rb_read((pos + 2) % rbd);
                uint32_t wv = rb_read((pos + 3) % rbd);
                if (wa >= 0x80000000u && wa < 0xA0000000u) { pm4_store32(wa, wv); fences++; }
            } else if (op == 0x54 && body >= 1) {                  // INTERRUPT
                pm4_fire_interrupt(rb_read((pos + 1) % rbd));
            }
            pos = (pos + total) % rbd; scanned += total; continue;
        }
        pos = (pos + 1) % rbd; scanned++;
    }

    g_rb_rptr_dwords = pos;

    if (s_rbflat_logged < 8 && ibs) {
        dbg_ram("[RBFLAT] scanned=%u ibs=%u fences=%u draws_total=%u\n",
               scanned, ibs, fences, g_draw_total);
 s_rbflat_logged++;
    }
    static uint32_t s_flatcalls = 0;
    static uint32_t s_last_dump_draws = 0xFFFFFFFFu;
    s_flatcalls++;
    // Dump RB state on the first few calls AND whenever the cumulative draw count changes
    // (each new plateau), so we capture MODECONTROL/COLOR_INFO at draws=13/24/35/46 — not
    // only the startup draws=2 snapshot.
    if (g_draw_total > 0 && ibs > 0 && (s_flatcalls <= 4 || g_draw_total != s_last_dump_draws)) {
        s_last_dump_draws = g_draw_total;
        pm4_dump_state();
    }
}

// ── Video ─────────────────────────────────────────────────────────────────────
//
// VdSwap (Xenia reference: src/xenia/kernel/xboxkrnl/xboxkrnl_video.cc)
//
// Parameters (PPC ABI — r3..r10 for args 1..8; args 9+ on stack):
//   r3  = buffer_ptr   — 64-dword region inside primary ring buffer to fill
//   r4  = fetch_ptr    — ptr to xe_gpu_texture_fetch_t (6 dwords, big-endian)
//   r5  = unk2         — system writeback ptr
//   r6  = unk3         — value from VdGetSystemCommandBuffer slot 0
//   r7  = unk4         — value from VdGetSystemCommandBuffer slot 1
//   r8  = frontbuffer_ptr   — ptr to guest uint32 holding frontbuffer phys addr
//   r9  = texture_format_ptr— ptr to guest uint32 holding TextureFormat enum
//   r10 = color_space_ptr   — ptr to guest uint32 (should be 0 = RGB)
//   (args 9,10 = width/height ptrs on stack — read from fetch constant instead)
//
// Writes the following PM4 commands into buffer_ptr:
//   [0]   MakePacketType0(0x4800, 6) — set shader fetch const registers
//   [1-6] The 6 dwords of the texture fetch constant
//   [7]   MakePacketType3(PM4_XE_SWAP=0x64, 4)
//   [8]   kSwapSignature = make_fourcc("SWAP") = 0x53574150
//   [9]   frontbuffer physical address
//   [10]  width
//   [11]  height
//   [12-63] NOP type-2 packets (0x80000000)
//
// Then calls gpu_d3d12_present() to blit the frontbuffer to the D3D12 window.
//
static std::mutex g_vdswap_mutex;  // serialize concurrent VdSwap calls from worker threads

PPC_FUNC(__imp__VdSwap) {
    PPC_FUNC_PROLOGUE();
    GilMutexGuard vdswap_lock(g_vdswap_mutex);   // GIL-cooperative (raw lock_guard deadlocks — see GilMutexGuard)

    uint32_t buffer_ptr = ctx.r3.u32;  // ring buffer write location
    uint32_t fetch_ptr  = ctx.r4.u32;  // xe_gpu_texture_fetch_t (6 dwords)
    // ctx.r8 = frontbuffer_ptr  (ptr to uint32 = frontbuffer virt/phys addr)
    // ctx.r9 = texture_format_ptr
    // ctx.r10 = color_space_ptr

    if (!buffer_ptr || !fetch_ptr) {
        dbg_ram("[VdSwap] null pointer (buffer=0x%08X fetch=0x%08X), skipping\n",
               buffer_ptr, fetch_ptr);

        return;
    }

    // Read the 6-dword texture fetch constant from guest memory (big-endian →
    // host LE via PPC_LOAD_U32).
    uint32_t d0 = PPC_LOAD_U32(fetch_ptr +  0);
    uint32_t d1 = PPC_LOAD_U32(fetch_ptr +  4);
    uint32_t d2 = PPC_LOAD_U32(fetch_ptr +  8);
    uint32_t d3 = PPC_LOAD_U32(fetch_ptr + 12);
    uint32_t d4 = PPC_LOAD_U32(fetch_ptr + 16);
    uint32_t d5 = PPC_LOAD_U32(fetch_ptr + 20);

    // xe_gpu_texture_fetch_t field extraction (host LE after PPC_LOAD_U32):
    //   dword_0 bits [30:22] = pitch (row_pitch_pixels >> 5)
    //   dword_0 bit  [31]    = tiled
    //   dword_1 bits [5:0]   = format (TextureFormat enum)
    //   dword_1 bits [31:12] = base_address (physical >> 12)
    //   dword_2 bits [12:0]  = size_2d.width  (stored as width-1)
    //   dword_2 bits [25:13] = size_2d.height (stored as height-1)
    uint32_t pitch_field     = (d0 >> 22) & 0x1FF;
    uint32_t tiled           = (d0 >> 31) & 1;
    uint32_t fmt             = d1 & 0x3F;
    uint32_t base_addr_field = (d1 >> 12) & 0xFFFFF;
    uint32_t frontbuffer_phys= base_addr_field << 12;
    uint32_t width           = (d2 & 0x1FFF) + 1;
    uint32_t height          = ((d2 >> 13) & 0x1FFF) + 1;
    // Row pitch: pitch field is row_pitch_pixels / 32, each pixel = 4 bytes
    uint32_t row_pitch       = (pitch_field > 0) ? (pitch_field * 32 * 4) : (width * 4);

    // r8 = frontbuffer_ptr (pointer to physical address).
    // If the texture fetch constant gave phys=0, fall back to dereferencing r8.
    if (frontbuffer_phys == 0 && ctx.r8.u32 >= 0x80000000u && ctx.r8.u32 < 0x94000000u) {
        uint32_t r8val = PPC_LOAD_U32(ctx.r8.u32);
        if (r8val >= 0x80000000u && r8val < 0x94000000u)
            frontbuffer_phys = r8val;
    }

    // GLOBWATCH: bracket the wild write that corrupts the device/fs globals
    // (dev table 0x83290B90 -> 0x830CB7E4 -> device; fs glob 0x82F211BC/0x82F211A0).
    // Values were good through frame ~240 then read 0x41B/garbage (runSYNC4).
    {
        static const uint32_t watch_addrs[4] = {0x83290B90u, 0x830CB7E4u, 0x82F211BCu, 0x82F211A0u};
        static uint32_t watch_prev[4];
        static bool watch_init = false, watch_dumped = false;
        for (int wi = 0; wi < 4; ++wi) {
            uint32_t v = PPC_LOAD_U32(watch_addrs[wi]);
            if (!watch_init) { watch_prev[wi] = v; continue; }
            if (v != watch_prev[wi]) {
                dbg_ram("[GLOBWATCH] present-tick: [0x%08X] 0x%08X -> 0x%08X\n", watch_addrs[wi], watch_prev[wi], v);
                if (!watch_dumped) {
                    watch_dumped = true;
                    for (uint32_t a = 0x830CB780u; a < 0x830CB880u; a += 16)
                        dbg_ram("[GLOBWATCH]   %08X: %08X %08X %08X %08X\n", a,
                               PPC_LOAD_U32(a), PPC_LOAD_U32(a+4), PPC_LOAD_U32(a+8), PPC_LOAD_U32(a+12));
                }
                watch_prev[wi] = v;
            }
        }
        watch_init = true;
    }
    static int count = 0;
    static bool first_nonzero_fb = false;
    bool is_first_nonzero = (frontbuffer_phys != 0 && !first_nonzero_fb);
    if (is_first_nonzero) first_nonzero_fb = true;
    if (count < 3 || count % 60 == 0 || is_first_nonzero) {
        uint32_t r8val = (ctx.r8.u32 >= 0x80000000u && ctx.r8.u32 < 0x94000000u)
                          ? PPC_LOAD_U32(ctx.r8.u32) : 0;
        dbg_ram("[FPS] present #%d at t=%llums\n", count, _kstub_start_ms());
        dbg_ram("[RGATE] enable[0x82E7EF5C]=0x%08X pause[0x82FCD16C]=0x%08X  (render needs enable!=0 && pause==0)\n",
               PPC_LOAD_U32(0x82E7EF5Cu), PPC_LOAD_U32(0x82FCD16Cu));
        { uint32_t glob   = PPC_LOAD_U32(0x82F211BCu);   // device ptr, expect 0x82F21190
          uint32_t m16    = (glob>=0x82000000u&&glob<0x94000000u) ? PPC_LOAD_U32(glob+16) : 0xDEAD;
          uint32_t direct = PPC_LOAD_U32(0x82F211A0u);   // static vtable[+16], expect 0x8213DBD0
          dbg_ram("[FSDEV] glob[0x82F211BC]=0x%08X *(glob+16)=0x%08X  *(0x82F211A0)=0x%08X (expect 0x8213DBD0)\n",
                 glob, m16, direct); }
        dbg_ram("[DRAWSTAT] #%d draws=%u resolves=%u imload27=%u imload28=%u geomCaptured=%u modes[0..7]=%u/%u/%u/%u/%u/%u/%u/%u\n",
               count, g_draw_total, g_resolve_total, g_imload27, g_imload28, g_draws_captured,
               g_draw_mode_hist[0],g_draw_mode_hist[1],g_draw_mode_hist[2],g_draw_mode_hist[3],
               g_draw_mode_hist[4],g_draw_mode_hist[5],g_draw_mode_hist[6],g_draw_mode_hist[7]);
        dbg_ram("[VdSwap] #%d fb=0x%08X %ux%u fmt=%u tiled=%u pitch=%u r8=0x%08X r8val=0x%08X fetch_ptr=0x%08X\n",
               count, frontbuffer_phys, width, height, fmt, tiled, pitch_field,
               ctx.r8.u32, r8val, ctx.r4.u32);
        if (frontbuffer_phys != 0 && frontbuffer_phys < 0x94000000u) {
            const uint8_t* fb = g_base + frontbuffer_phys;
            dbg_ram("[VdSwap]   px[0]=%02X%02X%02X%02X px[1]=%02X%02X%02X%02X px[640*360+0]=%02X%02X%02X%02X\n",
                   fb[0],fb[1],fb[2],fb[3], fb[4],fb[5],fb[6],fb[7],
                   fb[(640+360*1280)*4+0], fb[(640+360*1280)*4+1],
                   fb[(640+360*1280)*4+2], fb[(640+360*1280)*4+3]);
        }

    }
    ++count;

    // ── FLOWWATCH: per-present on-change log of known flow-state globals ──
    // 0x8327EA60 = app state (sub_824FC428/438), 0x82FD3968 = main-loop flag3,
    // 0x830C1EF0 = sub_8244E188 getter value, 0x82EF4DA4 = sub_8244E010 saved state.
    {
        static const uint32_t fw[4] = {0x8327EA60u, 0x82FD3968u, 0x830C1EF0u, 0x82EF4DA4u};
        static uint32_t fwprev[4];
        static bool fwinit = false;
        for (int i = 0; i < 4; ++i) {
            uint32_t v = PPC_LOAD_U32(fw[i]);
            if (fwinit && v != fwprev[i])
                dbg_ram("[FLOWWATCH] #%d [0x%08X] 0x%08X -> 0x%08X\n", count, fw[i], fwprev[i], v);
            fwprev[i] = v;
        }
        fwinit = true;
    }

    // ── FLOWSNAP: snapshot-diff to find the frontend-flow gate ────────────
    // Draws freeze deterministically between present ~#3420 and #4320 (boot fade
    // completes; the game then submits zero draws and never requests the menu
    // scene from the mounted archives). Snapshot the globals region at frames
    // A (before freeze), B (after), C (much later) and report u32 words that
    // CHANGED A->B but are FROZEN B->C — candidate flow-state latches the
    // kickoff polls. Contiguous candidate words collapse to one line (runs =
    // buffers/strings; a flow flag is an isolated word). LSWTCS_FLOWSNAP=A,B,C
    // (default 1800,6000,9600; 0 disables).
    {
        static int snapA = -2, snapB = 6000, snapC = 9600;
        if (snapA == -2) {
            snapA = 1800;
            if (const char* e = getenv("LSWTCS_FLOWSNAP")) {
                if (e[0] == '0' && !e[1]) snapA = -1;
                else sscanf(e, "%d,%d,%d", &snapA, &snapB, &snapC);
            }
        }
        constexpr uint32_t FS_LO = 0x82C00000u, FS_HI = 0x83400000u, FS_SZ = FS_HI - FS_LO;
        static uint8_t *bufA = nullptr, *bufB = nullptr;
        if (snapA > 0 && count == snapA && !bufA) {
            bufA = (uint8_t*)malloc(FS_SZ);
            if (bufA) memcpy(bufA, g_base + FS_LO, FS_SZ);
            dbg_ram("[FLOWSNAP] snapshot A at present #%d\n", count);
        }
        if (snapA > 0 && count == snapB && !bufB) {
            bufB = (uint8_t*)malloc(FS_SZ);
            if (bufB) memcpy(bufB, g_base + FS_LO, FS_SZ);
            dbg_ram("[FLOWSNAP] snapshot B at present #%d\n", count);
        }
        if (snapA > 0 && count == snapC && bufA && bufB) {
            int totalAB = 0, cand = 0, shown = 0, runlen = 0;
            uint32_t runstart = 0, runA = 0, runB = 0;
            for (uint32_t off = 0; off + 4 <= FS_SZ; off += 4) {
                uint32_t a = *(uint32_t*)(bufA + off);
                uint32_t b = *(uint32_t*)(bufB + off);
                bool isCand = false;
                if (a != b) {
                    ++totalAB;
                    uint32_t c = *(uint32_t*)(g_base + FS_LO + off);
                    if (b == c) { isCand = true; ++cand; }   // latched: changed then froze
                }
                if (isCand && runlen > 0 && FS_LO + off == runstart + 4u * runlen) {
                    ++runlen;                                 // extend contiguous run
                } else {
                    if (runlen > 0 && shown < 400) {
                        ++shown;
                        dbg_ram("[FLOWSNAP] cand [0x%08X] A=0x%08X -> B=C=0x%08X (run %d words)\n",
                                runstart, __builtin_bswap32(runA), __builtin_bswap32(runB), runlen);
                    }
                    runlen = 0;
                    if (isCand) { runstart = FS_LO + off; runA = a; runB = b; runlen = 1; }
                }
            }
            if (runlen > 0 && shown < 400) {
                ++shown;
                dbg_ram("[FLOWSNAP] cand [0x%08X] A=0x%08X -> B=C=0x%08X (run %d words)\n",
                        runstart, __builtin_bswap32(runA), __builtin_bswap32(runB), runlen);
            }
            dbg_ram("[FLOWSNAP] done: %d words changed A->B, %d latched, %d runs shown\n",
                    totalAB, cand, shown);
            free(bufA); free(bufB); bufA = bufB = nullptr;
        }
    }

    // ── Write PM4 commands into buffer_ptr ────────────────────────────────
    // MakePacketType0(reg=0x4800, count=6):
    //   (0<<30) | ((6-1)<<16) | 0x4800 = 0x00054800
    PPC_STORE_U32(buffer_ptr +  0*4, 0x00054800u);
    PPC_STORE_U32(buffer_ptr +  1*4, d0);
    PPC_STORE_U32(buffer_ptr +  2*4, d1);
    PPC_STORE_U32(buffer_ptr +  3*4, d2);
    PPC_STORE_U32(buffer_ptr +  4*4, d3);
    PPC_STORE_U32(buffer_ptr +  5*4, d4);
    PPC_STORE_U32(buffer_ptr +  6*4, d5);
    // MakePacketType3(PM4_XE_SWAP=0x64, count=4):
    //   (3<<30) | ((4-1)<<16) | (0x64<<8) = 0xC0036400
    PPC_STORE_U32(buffer_ptr +  7*4, 0xC0036400u);
    // kSwapSignature = make_fourcc("SWAP") = 'S'<<24|'W'<<16|'A'<<8|'P' = 0x53574150
    PPC_STORE_U32(buffer_ptr +  8*4, 0x53574150u);
    PPC_STORE_U32(buffer_ptr +  9*4, frontbuffer_phys);
    PPC_STORE_U32(buffer_ptr + 10*4, width);
    PPC_STORE_U32(buffer_ptr + 11*4, height);
    // Fill [12..63] with NOP type-2 packets (0x80000000)
    for (int i = 12; i < 64; i++)
        PPC_STORE_U32(buffer_ptr + i*4, 0x80000000u);

    // ── RECON: one-shot scan of the ring buffer + system command buffer ─────
    // buffer_ptr (system command buffer, ~0xA02xxxxx) is NOT the ring buffer
    // (g_rb_base ~0x9F2xxxxx). Find where the game's real draw IB references live.
    {
        static int s_scan = 0;
        // Pre-scan (only while still dumping): is there Type-3 content in the ring?
        bool has_t3 = false;
        if (s_scan < 4 && g_rb_base && g_rb_size) {
            uint32_t rbd0 = g_rb_size >> 2;
            for (uint32_t i = 0; i < rbd0; i++) { uint32_t h = rb_read(i); if ((h >> 30) == 3 && ((h>>8)&0xFF) != 0) { has_t3 = true; break; } }
        }
        if (s_scan < 4 && g_rb_base && g_rb_size && has_t3) {
            s_scan++;
            dbg_ram("[RBSCAN] buffer_ptr=0x%08X g_rb_base=0x%08X g_rb_size=0x%X rptr_dwords=%u\n",
                   buffer_ptr, g_rb_base, g_rb_size, g_rb_rptr_dwords);
            // Read the writeback RPTR the game thinks the GPU is at.
            if (g_rb_rptr_writeback) {
                uint32_t wbphys = g_rb_rptr_writeback;
                uint32_t wbg = (wbphys < 0x80000000u) ? (wbphys + 0x80000000u) : wbphys;
                if (wbg < 0x94000000u)
                    dbg_ram("[RBSCAN] rptr_writeback[0x%08X]=0x%08X (block_log2=%u)\n",
                           wbphys, PPC_LOAD_U32(wbg), g_rb_rptr_block_log2);
            }
            // Scan whole ring for Type-3 packets (esp. IB refs 0x3f/0x37 and draws).
            uint32_t rbd = g_rb_size >> 2;
            int shown = 0;
            for (uint32_t i = 0; i < rbd && shown < 40; i++) {
                uint32_t h = rb_read(i);
                if ((h >> 30) == 3) {
                    uint32_t op = (h >> 8) & 0xFF;
                    uint32_t bd = ((h >> 16) & 0x3FFF) + 1;
                    dbg_ram("[RBSCAN]  ring[%u] T3 op=0x%02X %s body=%u\n",
                           i, op, pm4_op_name(op), bd);
                    shown++;
                    // Follow INDIRECT_BUFFER references and dump the draw IB inside.
                    if ((op == 0x3F || op == 0x37) && bd >= 2) {
                        uint32_t ib_addr = rb_read(i + 1);
                        uint32_t ib_len  = rb_read(i + 2) & 0xFFFFF;
                        dbg_ram("[RBSCAN]   -> IB addr=0x%08X len=%u; decoded:\n", ib_addr, ib_len);
                        g_pm4_dump_budget = 120;
                        g_pm4_draw_count  = 0;
                        pm4_dump_ib(ib_addr, ib_len, 1);
                        dbg_ram("[RBSCAN]   -> IB draws=%u\n", g_pm4_draw_count);
                    }
                }
            }
            dbg_ram("[RBSCAN]  (%d type-3 packets found in ring)\n", shown);
            // Dump first 16 dwords of the system command buffer (where VdSwap wrote).
            for (int i = 0; i < 8; i++)
                dbg_ram("[RBSCAN]  sysbuf[%d]=0x%08X\n", i, PPC_LOAD_U32(buffer_ptr + i*4));

        }
    }

    // ── Process PM4 ring buffer (FLAT, deterministic) ──────────────────────
    // buffer_ptr is the game's ring write head. Walk the ring linearly from our
    // tracked read pointer up to it, following INDIRECT_BUFFER packets into draw
    // IBs and servicing GPU fences so the game's GPU-waits complete. This replaces
    // the old buffer_ptr-mod-ring_size scheme that processed a random slice/frame.
    auto _prof_t0 = std::chrono::high_resolution_clock::now();
    pm4_trace_frame();             // PM4 trace: per-frame marker + one-shot dump trigger
    const bool swap_synced = g_geom_swapsync == 1 && g_geom_swaps > 0;
    if (!swap_synced) geom_reset_build();   // GEOM: per-VdSwap window (fallback mode only)
    if (g_rb_base && g_rb_size) {
        g_interrupt_ctx = &ctx;
        pm4_process_ring_flat();   // self-paces off the ring NOP frontier; fills g_geom_verts
        g_interrupt_ctx = nullptr;
    }
    // GEOM: snapshot for the present replay. The per-VdSwap ring walk only sometimes
    // overlaps a frame's draw IBs (draw submission isn't synced to VdSwap), so most frames
    // capture 0 quads → the UI flickers to empty. Persist the LAST non-empty capture so the
    // (mostly-static) menu geometry stays on screen. LSWTCS_GEOMNOPERSIST=1 reverts to strict
    // per-frame.
    { static int s_persist = -1; if (s_persist < 0) s_persist = getenv("LSWTCS_GEOMNOPERSIST") ? 0 : 1;
      if (!swap_synced && (!s_persist || g_geom_vert_count > 0)) geom_publish(); }
    { static int _gf=0; if (g_geom_on==1 && (_gf++ % 60)==0)
        dbg_ram("[GEOMFRAME] verts=%u quads=%u (flat=%u textured=%u skipped=%u)\n",
                g_geom_vert_ready, g_geom_vert_ready/6, g_geom_flat, g_geom_tex, g_geom_skip);
      g_geom_flat=g_geom_tex=g_geom_skip=0; }
    // Asset-load probe: bytes read from disk per ~2s window. Non-zero in steady state ⇒
    // still streaming assets; ~0 ⇒ load done/idle (gate is elsewhere, not disk).
    { static int _rf=0; static uint64_t _last=0;
      if ((_rf++ % 120)==0) { uint64_t now=g_file_bytes_read.load();
          dbg_ram("[IOPROBE] read +%llu KB this window (total %llu KB)\n",
                  (unsigned long long)((now-_last)/1024), (unsigned long long)(now/1024));
          _last=now; } }
    auto _prof_t1 = std::chrono::high_resolution_clock::now();

    // ── Emulate the EDRAM→framebuffer resolve (clear-color stage) ──────────────
    // The game renders into EDRAM then resolves to the framebuffer. We don't
    // translate the draw geometry yet, but we DO honor the color-buffer clear:
    // once the game is doing clear/resolve passes, fill the presented framebuffer
    // with RB_COLOR_CLEAR so the window shows the game's actual cleared screen.
    // (Writing the framebuffer is safe — the game never branches on its contents.)
    // Guest k_8_8_8_8 is [B,G,R,A]; RB_COLOR_CLEAR is packed 0xAARRGGBB.
    uint32_t cc = g_xe_regs[XE_RB_COLOR_CLEAR];                        // 0xAARRGGBB
    uint64_t gp_fill0 = gp_now();
    if (cc != 0 && frontbuffer_phys >= 0x02000000u) {
        uint32_t destg = frontbuffer_phys + ((frontbuffer_phys < 0x80000000u) ? 0x80000000u : 0u);
        uint8_t  cb = cc & 0xFF, cg = (cc >> 8) & 0xFF, cr = (cc >> 16) & 0xFF, ca = (cc >> 24) & 0xFF;
        uint32_t bgra = ((uint32_t)ca << 24) | ((uint32_t)cr << 16) | ((uint32_t)cg << 8) | cb;
        if (destg >= 0x82000000u && destg + (uint64_t)width * height * 4 <= 0x94000000u) {
            uint32_t* p = (uint32_t*)(g_base + destg);
            uint32_t  n = width * height;
            for (uint32_t i = 0; i < n; i++) p[i] = bgra;
            static int _rl = 0;
            if (_rl < 3) { dbg_ram("[RESOLVE] fill fb=0x%08X %ux%u clear=0x%08X bgra=0x%08X resolves=%u\n",
                                  destg, width, height, cc, bgra, g_resolve_total); _rl++; }
        }
    }

    g_gpuprof[GP_FBFILL] += gp_now() - gp_fill0;

    // Simulate GPU read-pointer writeback (Xenia: RPTR = read_ptr_dwords >> block_log2).
    // Write the end of our written region so the game sees the ring buffer as consumed.
    if (g_rb_rptr_writeback && g_rb_base) {
        uint32_t end_dword = (buffer_ptr + 64*4 - g_rb_base) / 4;
        uint32_t rptr_val  = end_dword >> g_rb_rptr_block_log2;
        PPC_STORE_U32(g_rb_rptr_writeback, rptr_val);
    }

    // ── Present the framebuffer to the D3D12 window ───────────────────────
    // row_pitch must be 256-byte aligned for the D3D12 upload buffer.
    uint32_t aligned_pitch = (row_pitch + 255u) & ~255u;
    if (count == 47 || count == 48) { dbg_ram("[VdSwap#%d] entering gpu_d3d12_present phys=0x%X pitch=%u %ux%u\n", count, frontbuffer_phys, aligned_pitch, width, height); }
    auto _prof_t2 = std::chrono::high_resolution_clock::now();
    gpu_d3d12_present(g_base, frontbuffer_phys, aligned_pitch, width, height);
    if (count == 47 || count == 48) { dbg_ram("[VdSwap#%d] gpu_d3d12_present returned\n", count); }
    {   // PROFILE: accumulate per-phase runtime cost, report every 60 presents
        auto _prof_t3 = std::chrono::high_resolution_clock::now();
        auto us = [](auto a, auto b){ return std::chrono::duration_cast<std::chrono::microseconds>(b-a).count(); };
        static long long s_pm4=0, s_present=0; static int s_n=0;
        s_pm4 += us(_prof_t0,_prof_t1); s_present += us(_prof_t2,_prof_t3); s_n++;
        if (s_n >= 60) {
            dbg_ram("[PROF] avg per frame: pm4_ring=%.2fms present=%.2fms (runtime-side total=%.2fms) over %d frames\n",
                   s_pm4/1000.0/s_n, s_present/1000.0/s_n, (s_pm4+s_present)/1000.0/s_n, s_n);
 s_pm4=0; s_present=0; s_n=0;
            // GPUPROF: every pipeline stage (walk excludes the draw recording nested in it).
            static uint64_t gp_last[GP_COUNT], gp_t_last = 0;
            uint64_t gp[GP_COUNT], t = gp_now();
            for (int i = 0; i < GP_COUNT; ++i) { gp[i] = g_gpuprof[i] - gp_last[i]; gp_last[i] = g_gpuprof[i]; }
            double wall = gp_t_last ? (t - gp_t_last) / 1e6 / 60.0 : 0; gp_t_last = t;
            auto ms = [](uint64_t ns) { return ns / 1e6 / 60.0; };
            dbg_ram("[GPUPROF] guest wait for render thread=%.2fms\n", ms(gp[GP_RTWAIT]));
            dbg_ram("[GPUPROF] guest ISR inside walk=%.2fms | replay: arena->upload=%.2f diag=%.2f prepass=%.2f pso=%.2f bind=%.2f\n",
                    ms(gp[GP_IRQ]), ms(gp[GP_RP_ARENA]), ms(gp[GP_RP_DIAG]), ms(gp[GP_RP_PREPASS]), ms(gp[GP_RP_PSO]), ms(gp[GP_RP_BIND]));
            dbg_ram("[GPUPROF] per frame: wall=%.2fms busy=%.2f (limiter idle=%.2f) walk=%.2f record=%.2f (prepare=%.2f vhash+copy=%.2f) replay=%.2f "
                    "present=%.2f (fencewait=%.2f submit=%.2f) fbfill=%.2f | draws=%.0f vbytes=%.0fKB\n",
                    wall, wall - ms(gp[GP_LIMIT]), ms(gp[GP_LIMIT]), ms(gp[GP_WALK] - gp[GP_RECORD]), ms(gp[GP_RECORD]), ms(gp[GP_PREPARE]), ms(gp[GP_VCOPY]),
                    ms(gp[GP_REPLAY]), ms(gp[GP_PRESENT]), ms(gp[GP_FENCEWAIT]), ms(gp[GP_SUBMIT]),
                    ms(gp[GP_FBFILL]), gp[GP_DRAWS] / 60.0, gp[GP_VBYTES] / 1024.0 / 60.0);
        }
    }

    // ── Frame-rate limiter — lock to the Nintendo Switch Lite's 60 Hz display ──
    // Paces every frame to exactly 1/target_fps using an accumulating deadline
    // (smooth pacing, no drift). Hybrid sleep: coarse sleep to ~1ms before the
    // deadline, then a short busy-spin for sub-millisecond accuracy. Independent
    // of the host monitor's refresh (present runs without host vsync). Override
    // the target with env LSWTCS_FPS_CAP (e.g. 30 for a 30 fps lock; 0 = uncapped).
    {
        using clock = std::chrono::high_resolution_clock;
        static double  s_target_fps = 60.0;
        static bool    s_init       = false;
        static clock::time_point s_deadline;
        if (!s_init) {
            s_init = true;
            if (const char* e = getenv("LSWTCS_FPS_CAP")) { double v = atof(e); if (v >= 0) s_target_fps = v; }
#ifdef _WIN32
            // Raise the scheduler timer resolution to 1ms so sleep_until below is
            // accurate. Without this, Windows' default ~15.6ms tick makes the
            // coarse sleep overshoot the frame deadline (caps ~24fps instead of 60).
            timeBeginPeriod(1);
#endif
            s_deadline = clock::now();
            dbg_ram("[FPSCAP] frame limiter active: target=%.0f fps (%s)\n",
                   s_target_fps, s_target_fps > 0 ? "locked" : "uncapped");
        }
        if (s_target_fps > 0.0) {
            GpScope gp_limit(GP_LIMIT);   // GPUPROF: idle time the limiter adds (busy = wall - limit)
            auto period = std::chrono::duration_cast<clock::duration>(
                              std::chrono::duration<double>(1.0 / s_target_fps));
            s_deadline += period;
            auto now = clock::now();
            if (now < s_deadline) {
                // Pace OFF the cooperative scheduler: sleeping here while holding the turn
                // stalled every other guest thread for the whole frame budget, so worker-thread
                // asset loading crawled (~128 KB per 50 s at 60 fps vs. seconds uncapped).
                // LSWTCS_FPSCAP_HOLDGIL=1 restores the old (turn-holding) behaviour for A/B.
                static int hold = -1;
                if (hold < 0) { const char* e = getenv("LSWTCS_FPSCAP_HOLDGIL"); hold = (e && e[0] == '1') ? 1 : 0; }
                if (hold) {
                    if (s_deadline - now > std::chrono::milliseconds(2))
                        std::this_thread::sleep_until(s_deadline - std::chrono::milliseconds(1));
                    while (clock::now() < s_deadline) { /* busy-wait the final sub-ms */ }
                } else {
                    GilYield y;
                    if (s_deadline - now > std::chrono::milliseconds(2))
                        std::this_thread::sleep_until(s_deadline - std::chrono::milliseconds(1));
                    while (clock::now() < s_deadline) { /* busy-wait the final sub-ms */ }
                }
            } else {
                // Ran long (guest-bound this frame) — resync the deadline so we
                // don't accumulate debt and spiral into a catch-up burst.
                s_deadline = now;
            }
        }
    }

    // ── Fire graphics interrupt callback (Xenia: DispatchInterruptCallback) ─
    // source=1 = CP swap-done interrupt (fires after PM4_XE_SWAP processed).
    // source=0 = VBlank (also needed so the game loop advances its frame timer).
    // We fire both so the game unblocks from any WaitForVBlank / fence polls.
    if (g_gfx_interrupt_callback) {
        PPCFunc* fn = PPC_LOOKUP_FUNC(g_base, g_gfx_interrupt_callback);
        if (fn) {
            // CP swap-done interrupt (source=1)
            PPCContext icb{};
            icb.r1.u32    = ctx.r1.u32 - 0x200;  // fresh stack frame below caller
            icb.r2.u32    = ctx.r2.u32;
            icb.r13.u32   = ctx.r13.u32;          // real PCR so r13+268 CPU_ID reads work
            icb.r3.u32    = 1;                    // source: CP interrupt
            icb.r4.u32    = g_gfx_interrupt_data;
            icb.fpscr.csr = ctx.fpscr.getcsr();
            // Xenia never fires a synthetic source=1 per frame; with real scratch writeback it
            // would re-run an already-consumed deferred callback. LSWTCS_SYNTHINT=1 restores it.
            { static int syn = -1; if (syn < 0) { const char* e = getenv("LSWTCS_SYNTHINT"); syn = (e && e[0] == '1') ? 1 : 0; }
              if (syn) fn(icb, g_base); }

            // Force CPU waiting mask to 0.
            // sub_822ADA00 (the callback) clears only 1 bit: (1 << CPU_ID) from
            // PPC_LOAD_U32(user_data+10900)[0]. With icb.r13=0, CPU_ID=0, so only
            // bit 0 is cleared, leaving the game spinning on the remaining 5 bits.
            // Zero the mask directly to unblock the per-frame spin-wait.
            if (g_gfx_interrupt_data) {
                uint32_t mp = PPC_LOAD_U32(g_gfx_interrupt_data + 10900);
                if (mp >= 0x82000000u && mp < 0x94000000u)
                    PPC_STORE_U32(mp, 0);
            }

            // VBlank interrupt (source=0) — drives sub_822B7898 which increments the
            // VBlank counter at user_data+16532.  The main game loop polls that counter
            // and spins forever if it never advances.
            //
            // sub_822ADA00 source=0 checks: PPC_LOAD_U32(0x7FC86544) & 1
            // 0x7FC80000 = Xbox 360 GPU MMIO base; offset 0x6544 = VBlank enable flag.
            // In our port this memory region is zeroed, so the check always fails and
            // sub_822B7898 is never called.  Write 1 here before every VBlank dispatch.
            PPC_STORE_U32(0x7FC86544, 1);

            icb.r3.u32    = 0;                    // source: VBlank
            icb.r4.u32    = g_gfx_interrupt_data;
            icb.r13.u32   = ctx.r13.u32;          // propagate real PCR so r13+268 CPU_ID reads work
            fn(icb, g_base);
        }
    }

    // Advance KPRCB tick counter for this thread (Xbox 360 10 MHz; 1ms = 10000 ticks).
    // sub_822AFB88 polls KPRCB+88 to detect elapsed time; without this update the
    // counter never changes and sub_822AE698's spin loop never exits.
    uint32_t kprcb = PPC_LOAD_U32(ctx.r13.u32 + 256);
    if (kprcb) PPC_STORE_U32(kprcb + 88, (uint32_t)(lswtcs_now_ms() * 10000ULL));
}

PPC_FUNC(__imp__VdRetrainEDRAMWorker)       { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }

// ── Ex R/W Locks ──────────────────────────────────────────────────────────────
PPC_FUNC(__imp__ExAcquireReadWriteLockExclusive) { PPC_FUNC_PROLOGUE(); }
PPC_FUNC(__imp__ExAcquireReadWriteLockShared)    { PPC_FUNC_PROLOGUE(); }
PPC_FUNC(__imp__ExReleaseReadWriteLock)          { PPC_FUNC_PROLOGUE(); }
PPC_FUNC(__imp__ExInitializeReadWriteLock)       { PPC_FUNC_PROLOGUE(); }

// ── CRT ───────────────────────────────────────────────────────────────────────
PPC_FUNC(__imp___vsnprintf)                 { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp____C_specific_handler)       { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }

// ExGetXConfigSetting(category r3, setting r4, buffer r5, buffer_size r6, required_size* r7).
// Was: return success without touching the buffer -> the game read the user language etc. from
// uninitialised memory. Values follow Xenia's XConfig::SetDefaults (big-endian in guest memory):
// language English(1), country US(103), AV region NTSC-M(0x00400100); anything else is zero-filled.
// LSWTCS_XCONFIG=0 restores the old stub. Logs the first 64 calls as [XCONFIG].
PPC_FUNC(__imp__ExGetXConfigSetting) {
    PPC_FUNC_PROLOGUE();
    static int on = -1; if (on < 0) { const char* e = getenv("LSWTCS_XCONFIG"); on = (e && e[0] == '0') ? 0 : 1; }
    uint32_t cat = ctx.r3.u32 & 0xFFFF, set = ctx.r4.u32 & 0xFFFF, buf = ctx.r5.u32, size = ctx.r6.u32 & 0xFFFF, req = ctx.r7.u32;
    static int nlog = 0;
    if (nlog < 64) { ++nlog; printf("[XCONFIG] cat=0x%X setting=0x%X buf=0x%08X size=%u lr=0x%08X\n", cat, set, buf, size, (uint32_t)ctx.lr); fflush(stdout); }
    if (!on) { ctx.r3.u32 = 0; return; }
    uint32_t need = 0, v = 0;
    if (cat == 0x03 && set == 0x09) { need = 4; v = 1; }                 // XCONFIG_USER_LANGUAGE: English
    else if (cat == 0x03 && set == 0x0E) { need = 1; v = 103; }          // XCONFIG_USER_COUNTRY: United States
    else if (cat == 0x02 && set == 0x02) { need = 4; v = 0x00400100u; }  // XCONFIG_SECURED_AV_REGION: NTSC-M
    else need = size;                                                     // unknown: zero-filled
    if (buf && size) {
        if (size < need) { ctx.r3.u32 = 0xC0000023u; return; }          // STATUS_BUFFER_TOO_SMALL
        for (uint32_t i = 0; i < size; ++i) PPC_STORE_U8(buf + i, 0);
        if (need == 4 && v) PPC_STORE_U32(buf, v);
        else if (need == 1 && v) PPC_STORE_U8(buf, uint8_t(v));
    }
    if (req) PPC_STORE_U16(req, uint16_t(need));
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__ObDereferenceObject)      { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__ObReferenceObject)        { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
// ObReferenceObjectByHandle(r3 = handle, r4 = object type, r5 = void** out). Was a stub that never
// wrote *out. XAudio starts its worker thread (sub_827DB318, created CREATE_SUSPENDED) with
// ObReferenceObjectByHandle -> KeSetBasePriorityThread(obj) -> KeResumeThread(obj): with *out
// garbage the resume found no thread, the worker never ran, the render callback's voice processing
// never happened and all audio was silent. For guest-thread handles, hand back the handle itself as
// the "object" (our Ke*Thread functions resolve it through g_thread_map). Other handles: unchanged.
PPC_FUNC(__imp__ObReferenceObjectByHandle) {
    PPC_FUNC_PROLOGUE();
    uint32_t h = ctx.r3.u32, out = ctx.r5.u32;
    bool thread = false;
    { std::lock_guard<std::mutex> lk(g_thread_mutex); thread = g_thread_map.find(h) != g_thread_map.end(); }
    if (thread && out >= 0x80000000u && out < 0xA0000000u) PPC_STORE_U32(out, h);
    static int n = 0; if (n++ < 16) dbg_ram("[ObRef] handle=0x%08X type=0x%08X out=0x%08X thread=%d lr=0x%08X\n", h, ctx.r4.u32, out, thread ? 1 : 0, (uint32_t)ctx.lr);
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__KeInitializeApc)          { PPC_FUNC_PROLOGUE(); }
PPC_FUNC(__imp__XMsgInProcessCall)        { PPC_FUNC_PROLOGUE(); LSW_XSTUB_LOG(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XamGetSystemVersion)      { PPC_FUNC_PROLOGUE(); LSW_XSTUB_LOG(); ctx.r3.u32 = 0; }
// XGetVideoMode(X_VIDEO_MODE* r3): old stub returned without filling the struct →
// the game read garbage/zero video mode (0x0, standard 0). Fill 1280x720 NTSC 60Hz
// widescreen hi-def (mirrors VdQueryVideoMode). Video standard selects NTSC vs PAL
// resource paths (Movies/NTSC vs Movies/PAL).
PPC_FUNC(__imp__XGetVideoMode) {
    PPC_FUNC_PROLOGUE();
    uint32_t p = ctx.r3.u32;
    if (p >= 0x80000000u && p < 0x94000000u) {
        for (int i = 0; i < 48; i += 4) PPC_STORE_U32(p + i, 0);
        PPC_STORE_U32(p + 0, 1280);         // display width
        PPC_STORE_U32(p + 4, 720);          // display height
        PPC_STORE_U32(p + 8, 0);            // interlaced = false
        PPC_STORE_U32(p + 12, 1);           // widescreen = true
        PPC_STORE_U32(p + 16, 1);           // hi-def = true
        PPC_STORE_U32(p + 20, 0x42700000u); // refresh = 60.0f
        PPC_STORE_U32(p + 24, 1);           // video standard = NTSC-M
    }
    static int n = 0; if (n < 4) { n++; dbg_ram("[XAM] XGetVideoMode(0x%08X) -> 1280x720 NTSC 60Hz\n", p); }
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__DbgPrint)                 { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }

PPC_FUNC(__imp__XUsbcamSetCaptureMode)         { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XUsbcamReadFrame)              { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XUsbcamSnapshot)               { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__KeResetEvent) {
    PPC_FUNC_PROLOGUE();
    uint32_t ptr = ctx.r3.u32;
    if (ptr >= 0x80000000u && ptr < 0x94000000u)
        PPC_STORE_U32(ptr + 4, 0);
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__XeKeysGetKeyProperties)        { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XeKeysGetKey)                  { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XeCryptAesKey)                 { PPC_FUNC_PROLOGUE(); }
PPC_FUNC(__imp__XeCryptAesEcb)                 { PPC_FUNC_PROLOGUE(); }
PPC_FUNC(__imp__XeCryptBnQw_SwapDwQwLeBe)      { PPC_FUNC_PROLOGUE(); }
PPC_FUNC(__imp__XeKeysQwNeRsaPrvCrypt)         { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__vsprintf)                      { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XeCryptRandom)                 { PPC_FUNC_PROLOGUE(); }

PPC_FUNC(__imp__NetDll_XNetStartup)         { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_XNetCleanup)         { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XUsbcamCreate)              { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XUsbcamDestroy)             { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XUsbcamGetConfig)           { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XUsbcamGetState)            { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XUsbcamGetView)             { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XUsbcamReset)               { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XUsbcamSetConfig)           { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XUsbcamSetView)             { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XeCryptBnQwNeRsaKeyGen)     { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XeCryptBnQwNeRsaPrvCrypt)   { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XeCryptBnQwNeRsaPubCrypt)   { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XeCryptRc4Ecb)              { PPC_FUNC_PROLOGUE(); }
PPC_FUNC(__imp__XeCryptRc4Key)              { PPC_FUNC_PROLOGUE(); }
PPC_FUNC(__imp__XeCryptShaFinal)            { PPC_FUNC_PROLOGUE(); }
PPC_FUNC(__imp__XeCryptShaInit)             { PPC_FUNC_PROLOGUE(); }
PPC_FUNC(__imp__XeCryptShaUpdate)           { PPC_FUNC_PROLOGUE(); }
PPC_FUNC(__imp__XexGetModuleHandle)         { PPC_FUNC_PROLOGUE(); LSW_XSTUB_LOG(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XexGetProcedureAddress)     { PPC_FUNC_PROLOGUE(); LSW_XSTUB_LOG(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_XNetConnect) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_XNetCreateKey) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_XNetDnsLookup) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_XNetDnsRelease) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_XNetGetConnectStatus) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_XNetInAddrToServer) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_XNetInAddrToString) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_XNetInAddrToXnAddr) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_XNetQosListen) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_XNetQosLookup) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_XNetQosRelease) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_XNetQosServiceLookup) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_XNetRandom) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_XNetRegisterKey) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_XNetServerToInAddr) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_XNetTsAddrToInAddr) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_XNetUnregisterInAddr) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_XNetUnregisterKey) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_XNetXnAddrToInAddr) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_XNetXnAddrToMachineId) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_WSACleanup) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_WSAStartup) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_XNetGetBroadcastVersionStatus) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_XNetGetDebugXnAddr) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_XNetGetEthernetLinkStatus) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_XNetGetOpt) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_XNetGetTitleXnAddr) {
    PPC_FUNC_PROLOGUE();
    // (r3=caller id, r4=XNADDR* out). Returning 0 = XNET_GET_XNADDR_PENDING made the
    // frontend init poll forever (sub_827D7060 spin). Returning 8 (NONE/offline)
    // diverted [T]10 boot into a stall — the offline path is the game's rarely-tested
    // one. Mimic Xenia instead: STATIC (1) with a plausible XNADDR (the game boots to
    // the title screen with this answer under Xenia). LSWTCS_XNET=0 restores PENDING,
    // =8 the offline answer, for A/B.
    static int mode = -1;
    if (mode < 0) { const char* e = getenv("LSWTCS_XNET"); mode = e ? atoi(e) : 1; }
    if (mode != 0 && ctx.r4.u32 >= 0x80000000u && ctx.r4.u32 < 0x94000000u) {
        uint8_t* xn = base + ctx.r4.u32;
        memset(xn, 0, 36);
        if (mode == 1) {
            xn[0] = 192; xn[1] = 168; xn[2] = 1; xn[3] = 100;   // ina = 192.168.1.100
            static const uint8_t mac[6] = {0x00, 0x22, 0x48, 0x01, 0x02, 0x03};
            memcpy(xn + 10, mac, 6);                             // abEnet
        }
    }
    ctx.r3.u32 = (mode == 0) ? 0u : (uint32_t)mode;
}
PPC_FUNC(__imp__NetDll_XNetQosGetListenStats) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_XNetSetOpt) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_bind) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_closesocket) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_connect) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_getpeername) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_getsockname) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_getsockopt) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_ioctlsocket) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_listen) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_setsockopt) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_shutdown) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_socket) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_WSACancelOverlappedIO) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_WSACloseEvent) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_WSACreateEvent) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_WSAEventSelect) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_WSAGetLastError) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_WSAGetOverlappedResult) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_WSARecv) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_WSARecvFrom) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_WSAResetEvent) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_WSASend) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_WSASendTo) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_WSASetEvent) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_WSASetLastError) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_accept) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_inet_addr) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_recv) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_recvfrom) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_select) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_send) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll_sendto) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__ExRegisterTitleTerminateNotification) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__KeAcquireSpinLockAtRaisedIrql) { PPC_FUNC_PROLOGUE(); lswtcs_spin_enter(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__KeInitializeSemaphore) {
    PPC_FUNC_PROLOGUE();
    // r3=*KSEMAPHORE, r4=count, r5=limit. X_KSEMAPHORE = DISPATCHER_HEADER(16) + limit(+16).
    // Was a no-op, so in-memory semaphores kept garbage/zero state (Xenia: xboxkrnl_threading).
    uint32_t s = ctx.r3.u32;
    if (s >= 0x80000000u && s < 0x94000000u) {
        PPC_STORE_U8(s + 0, 5);                 // SemaphoreObject
        PPC_STORE_U8(s + 2, 5);                 // size in dwords (header 4 + limit 1)
        PPC_STORE_U32(s + 4, ctx.r4.u32);       // SignalState = initial count
        PPC_STORE_U32(s + 8, s + 8);            // empty wait list
        PPC_STORE_U32(s + 12, s + 8);
        PPC_STORE_U32(s + 16, ctx.r5.u32);      // limit
    }
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__KeQueryPerformanceFrequency) {
    PPC_FUNC_PROLOGUE();
    // Xbox 360 high-resolution timer: 49,875,000 Hz (CPU timebase / 64).
    // LARGE_INTEGER KeQueryPerformanceFrequency(void) RETURNS the value in r3 (Xenia:
    // return 50000000). It used to store through r3 as if it were an out-pointer, which
    // left r3 = garbage for callers (sub_822B7998 divides by freq/refresh -> host #DE) and
    // scribbled 8 bytes wherever r3 happened to point.
    ctx.r3.u64 = 49875000ull;
}
PPC_FUNC(__imp__KeRaiseIrqlToDpcLevel) { PPC_FUNC_PROLOGUE(); lswtcs_spin_enter(); ctx.r3.u32 = 0; }  // DPC level disables preemption
PPC_FUNC(__imp__KeReleaseSemaphore) {
    PPC_FUNC_PROLOGUE();
    // r3=*KSEMAPHORE, r4=increment (priority boost), r5=adjustment, r6=wait. Returns the
    // previous count (Xenia xeKeReleaseSemaphore). Was a no-op → releases were lost.
    uint32_t s = ctx.r3.u32, prev = 0;
    if (s >= 0x80000000u && s < 0x94000000u) {
        prev = PPC_LOAD_U32(s + 4);
        PPC_STORE_U32(s + 4, prev + ctx.r5.u32);
    }
    kw_notify();
    ctx.r3.u32 = prev;
}
PPC_FUNC(__imp__KeReleaseSpinLockFromRaisedIrql) { PPC_FUNC_PROLOGUE(); lswtcs_spin_leave(); }
PPC_FUNC(__imp__KeTryToAcquireSpinLockAtRaisedIrql) { PPC_FUNC_PROLOGUE(); lswtcs_spin_enter(); ctx.r3.u32 = 1; }  // always "acquires" under the GIL
PPC_FUNC(__imp__KfLowerIrql) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }  // not counted (see KeRaiseIrqlToDpcLevel)
PPC_FUNC(__imp__MmGetPhysicalAddress) { PPC_FUNC_PROLOGUE(); /* virt == phys in our port */ }
PPC_FUNC(__imp__NetDll_WSAWaitForMultipleEvents) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NetDll___WSAFDIsSet) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XAudioGetVoiceCategoryVolume) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XAudioGetVoiceCategoryVolumeChangeMask) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XAudioQueryDriverPerformance) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
// ── XAudio render-driver clients (modeled on Xenia apu/audio_system.cc) ────────
// The game's statically-linked XAudio engine registers a render callback and expects the
// "audio hardware" to call it once per audio frame (256 samples @ 48 kHz ≈ 5.33 ms); each
// call mixes one frame and hands it back via XAudioSubmitRenderDriverFrame. With the old
// no-op stubs the engine never ticked, so voices never reported "stopped" and the main
// thread spun forever in NuSoundKillAllAudioWaitEx (sub_82357998) right after the titles
// load. Here: Register records {callback, arg}, writes handle 0x41550000|idx, and spawns a
// dedicated guest worker thread that calls the callback once per credit (seeded with 8 =
// Xenia's apu_max_queued_frames). Submit discards the samples (no host audio output yet)
// and grants the next credit on a real-time 5.33 ms cadence. While waiting for a credit the
// worker sits OFF the cooperative scheduler (GilYield), so it never starves other threads.
// LSWTCS_XAUDIO=0 restores the old no-op stubs.
namespace {
struct AudioClient {
    bool                  in_use = false;
    uint32_t              callback = 0;      // guest fn
    uint32_t              callback_arg = 0;  // value at [wrapped]
    uint32_t              wrapped = 0;       // guest cell holding callback_arg; passed as r3
    int                   sched_id = -1;
    std::atomic<int>      credits{0};        // frames the worker may render now
    std::atomic<int>      pending{0};        // frames submitted, not yet "played"
    std::atomic<bool>     stop{false};
    HANDLE                wake = nullptr;    // auto-reset: submit/unregister nudges the worker
    bool                  host_out = false;  // frames go to a host XAudio2 voice (host_audio.cpp)
    std::thread           host_thread;
    uint64_t              calls = 0;
};
constexpr int kAudioMaxClients = 8;
AudioClient g_audio_clients[kAudioMaxClients];
}
// Host output (host_audio.cpp). When a client has a host voice, a frame becomes a credit when the
// DEVICE finishes playing it (OnBufferEnd) instead of on the 5.33 ms timer.
bool host_audio_open(int idx, void (*on_end)(int idx));
void host_audio_submit(int idx, const uint8_t* frame_be);
void host_audio_close(int idx);
static void audio_frame_played(int idx) {
    AudioClient& c = g_audio_clients[idx];
    if (c.pending.load() > 0) c.pending--;
    c.credits++;
    if (c.wake) SetEvent(c.wake);
}
static int xaudio_on() { static int v = -1; if (v < 0) { const char* e = getenv("LSWTCS_XAUDIO"); v = (e && e[0] == '0') ? 0 : 1; } return v; }

static void audio_worker_body(int idx, uint32_t sp, uint32_t pcr, uint32_t toc) {
    AudioClient& c = g_audio_clients[idx];
    g_sched_id = c.sched_id;
    sched_register(c.sched_id);
    PPCFunc* fn = PPC_LOOKUP_FUNC(g_base, c.callback);
    dbg_ram("[XAUDIO] worker %d started: callback=0x%08X arg=0x%08X fn=%p sched=%d\n",
            idx, c.callback, c.callback_arg, (void*)fn, c.sched_id);
    using clk = std::chrono::steady_clock;
    const auto frame = std::chrono::microseconds(5333);
    auto next_due = clk::now();
    while (!c.stop.load()) {
        if (c.credits.load() > 0 && fn) {
            c.credits--;
            PPCContext t{};
            t.r1.u32 = sp; t.r2.u32 = toc; t.r13.u32 = pcr; t.fpscr.csr = 0x1F80u;
            t.r3.u32 = c.wrapped;
            fn(t, g_base);
            if ((++c.calls % 2000) == 1)
                dbg_ram("[XAUDIO] client %d callback #%llu (pending=%d)\n", idx,
                        (unsigned long long)c.calls, c.pending.load());
            continue;
        }
        // Out of credits: turn one submitted frame into a credit once its 5.33 ms has elapsed.
        auto now = clk::now();
        if (!c.host_out && c.pending.load() > 0 && now >= next_due) {
            c.pending--; c.credits++;
            next_due = (now - next_due > frame * 8) ? now + frame : next_due + frame;
            continue;
        }
        GilYield y;   // wait off the scheduler
        DWORD ms = 1;
        if (c.host_out) {
            ms = 20;                                   // OnBufferEnd signals c.wake
        } else if (c.pending.load() > 0) {
            auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(next_due - now).count();
            ms = (DWORD)std::max<long long>(1, wait);
        } else {
            ms = 20;
        }
        WaitForSingleObject(c.wake, ms);
    }
    sched_leave(c.sched_id);
    dbg_ram("[XAUDIO] worker %d exiting after %llu callbacks\n", idx, (unsigned long long)c.calls);
}

PPC_FUNC(__imp__XAudioRegisterRenderDriverClient) {
    PPC_FUNC_PROLOGUE();
    // r3 = callback_ptr -> {callback, callback_arg}, r4 = driver handle out
    uint32_t cbp = ctx.r3.u32, outp = ctx.r4.u32;
    if (!xaudio_on()) { ctx.r3.u32 = 0; return; }
    if (cbp < 0x80000000u || cbp >= 0xA0000000u) { ctx.r3.u32 = 0x80070057u; return; }  // E_INVALIDARG
    uint32_t callback = PPC_LOAD_U32(cbp), callback_arg = PPC_LOAD_U32(cbp + 4);
    if (!callback) { ctx.r3.u32 = 0x80070057u; return; }
    int idx = -1;
    for (int i = 0; i < kAudioMaxClients; ++i) if (!g_audio_clients[i].in_use) { idx = i; break; }
    if (idx < 0) { ctx.r3.u32 = 0x8007000Eu; return; }   // E_OUTOFMEMORY
    AudioClient& c = g_audio_clients[idx];
    c.in_use = true; c.callback = callback; c.callback_arg = callback_arg;
    c.stop = false; c.calls = 0; c.pending = 0; c.credits = 8;   // Xenia apu_max_queued_frames
    c.wrapped = g_phys_alloc(16);
    if (c.wrapped) PPC_STORE_U32(c.wrapped, callback_arg);
    if (!c.wake) c.wake = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    uint32_t gs = 0x10000, stack = g_phys_alloc(gs);
    uint32_t sp = stack ? ((stack + gs - 0x100u) & ~0xFu) : 0u;
    uint32_t pcr = g_phys_alloc(0x1000);
    if (pcr) {
        memset(g_base + pcr, 0, 0x1000);
        PPC_STORE_U8(pcr + 268, 0);
        PPC_STORE_U32(pcr + 256, pcr + 0x200);
        PPC_STORE_U32(pcr + 0x200 + 88, (uint32_t)(lswtcs_now_ms() * 10000ULL));
        lswtcs_register_kprcb(pcr + 0x200);
    }
    c.sched_id = g_sched_next_id++;
    c.host_out = host_audio_open(idx, audio_frame_played);
    if (outp >= 0x80000000u && outp < 0xA0000000u) PPC_STORE_U32(outp, 0x41550000u | (uint32_t)idx);
    dbg_ram("[XAUDIO] RegisterRenderDriverClient idx=%d callback=0x%08X arg=0x%08X handle=0x%08X lr=0x%08X\n",
            idx, callback, callback_arg, 0x41550000u | (uint32_t)idx, (uint32_t)ctx.lr);
    c.host_thread = std::thread(audio_worker_body, idx, sp, pcr, ctx.r2.u32);
    c.host_thread.detach();
    sched_add_slot(c.sched_id);   // join the rotation at THIS guest point (deterministic)
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__XAudioSubmitRenderDriverFrame) {
    PPC_FUNC_PROLOGUE();
    // r3 = driver handle (0x41550000|idx), r4 = samples (discarded: no host audio output yet)
    uint32_t h = ctx.r3.u32;
    if (xaudio_on() && (h & 0xFFFF0000u) == 0x41550000u && (h & 0xFFFFu) < (uint32_t)kAudioMaxClients) {
        AudioClient& c = g_audio_clients[h & 0xFFFFu];
        if (c.in_use) {
            c.pending++;
            uint32_t s = ctx.r4.u32;
            if (c.host_out && s >= 0x10000u && s < 0xFFFF0000u) host_audio_submit(h & 0xFFFFu, g_base + s);
            else if (c.host_out) audio_frame_played(h & 0xFFFFu);   // bad pointer: keep the credit chain alive
            if (c.wake) SetEvent(c.wake);
        }
    }
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__XAudioUnregisterRenderDriverClient) {
    PPC_FUNC_PROLOGUE();
    uint32_t h = ctx.r3.u32;
    if (xaudio_on() && (h & 0xFFFF0000u) == 0x41550000u && (h & 0xFFFFu) < (uint32_t)kAudioMaxClients) {
        AudioClient& c = g_audio_clients[h & 0xFFFFu];
        dbg_ram("[XAUDIO] UnregisterRenderDriverClient idx=%u after %llu callbacks\n", h & 0xFFFFu, (unsigned long long)c.calls);
        c.stop = true; c.in_use = false; if (c.wake) SetEvent(c.wake);
        if (c.host_out) { host_audio_close(h & 0xFFFFu); c.host_out = false; }
    }
    ctx.r3.u32 = 0;
}
// XMA hardware decoder (xma/lsw_xma.cc). XMACreateContext(r3 = DWORD* out) hands out one of 320
// 64-byte hardware contexts in physical memory (Xenia XmaDecoder::AllocateContext); the game then
// drives them through the MMIO kick/lock/clear registers. LSWTCS_XMA=0 restores the old stubs.
extern "C" int lswtcs_xma_init(uint8_t* base, uint32_t ctx_va);
extern "C" uint32_t lswtcs_xma_alloc();
extern "C" void lswtcs_xma_release(uint32_t ptr);
static int xma_on() {
    static int v = -1;
    if (v < 0) { const char* e = getenv("LSWTCS_XMA"); v = (e && e[0] == '0') ? 0 : 1;
                 if (v) { uint32_t blk = g_phys_alloc(320 * 64 + 256);
                          if (!blk) { v = 0; dbg_ram("[XMA] context array alloc FAILED -> stubs\n"); }
                          else lswtcs_xma_init(g_base, (blk + 255) & ~255u); } }
    return v;
}
// Called from the game's XMA init (sub_827DBCD8, ppc_recomp.492.cpp) right before it reads the
// ContextArrayAddress register and caches it (used to compute every context's hardware index).
extern "C" void lswtcs_xma_boot() { xma_on(); }
PPC_FUNC(__imp__XMACreateContext) {
    PPC_FUNC_PROLOGUE();
    static int n = 0;
    uint32_t out = ctx.r3.u32, c = xma_on() ? lswtcs_xma_alloc() : 0;
    if (++n <= 16) dbg_ram("[XMA] XMACreateContext #%d out=0x%08X -> ctx 0x%08X lr=0x%08X\n", n, out, c, (uint32_t)ctx.lr);
    if (!xma_on()) { ctx.r3.u32 = 0; return; }
    if (out) PPC_STORE_U32(out, c);
    ctx.r3.u32 = c ? 0 : 0xC0000017u;   // X_STATUS_NO_MEMORY
}
PPC_FUNC(__imp__XMAReleaseContext) {
    PPC_FUNC_PROLOGUE();
    static int n = 0;
    if (++n <= 16) dbg_ram("[XMA] XMAReleaseContext #%d ctx=0x%08X lr=0x%08X\n", n, ctx.r3.u32, (uint32_t)ctx.lr);
    if (xma_on()) lswtcs_xma_release(ctx.r3.u32);
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__KeBugCheck) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__KeEnableFpuExceptions) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__KeRestoreFloatingPointState) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__KeSaveFloatingPointState) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NtSetInformationFile) {
    PPC_FUNC_PROLOGUE();
    // r3=handle, r4=IoStatus, r5=FileInformation, r6=Length, r7=FileInformationClass.
    // The old stub returned success WITHOUT performing the seek, so after the first read
    // the file pointer never moved → subsequent reads got the wrong data and the asset
    // load stalled. Class 14 (XFilePositionInformation) = seek to an absolute offset.
    uint32_t handle = ctx.r3.u32, iosb = ctx.r4.u32, info = ctx.r5.u32, len = ctx.r6.u32, cls = ctx.r7.u32;
    static std::atomic<uint32_t> _sn{0}; uint32_t c=++_sn;
    if (c <= 20) dbg_ram("[NtSetInfoFile] call#%u handle=0x%08X class=%u len=%u\n", c, handle, cls, len);
    if (cls == 14 && info && len >= 8) {
        uint64_t pos = ((uint64_t)PPC_LOAD_U32(info) << 32) | (uint64_t)PPC_LOAD_U32(info + 4);
        uint64_t out = 0; host_xfile_seek(handle, (int64_t)pos, 0u, &out);
    }
    ctx.r3.u32 = 0;
    if (iosb) { PPC_STORE_U32(iosb, 0); PPC_STORE_U32(iosb + 4, len); }
}
PPC_FUNC(__imp__RtlCaptureContext) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
// XGetLanguage: 0 is NOT a valid XC_LANGUAGE value (1=English..). Resource/path
// selection keyed off language can go wrong on 0. Return English + log.
PPC_FUNC(__imp__XGetLanguage) {
    PPC_FUNC_PROLOGUE();
    static int n = 0; if (n < 4) { n++; dbg_ram("[XAM] XGetLanguage -> 1 (English)\n"); }
    ctx.r3.u32 = 1;
}
PPC_FUNC(__imp__XMsgStartIORequestEx) {
    PPC_FUNC_PROLOGUE();
    static int n = 0; if (n < 12) { n++;
        dbg_ram("[XAM] XMsgStartIORequestEx app=0x%X msg=0x%08X ovl=0x%08X buf=0x%08X len=%u\n",
                ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32); }
    xmsg_complete_overlapped(base, ctx.r5.u32, "XMsgStartIORequestEx");
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__XMsgSystemProcessCall) {
    PPC_FUNC_PROLOGUE();
    static int n = 0; if (n < 12) { n++;
        dbg_ram("[XAM] XMsgSystemProcessCall app=0x%X msg=0x%08X buf=0x%08X len=%u\n",
                ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32); }
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__XNetLogonGetMachineID) { PPC_FUNC_PROLOGUE(); LSW_XSTUB_LOG(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XNetLogonGetTitleID) { PPC_FUNC_PROLOGUE(); LSW_XSTUB_LOG(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XamAlloc) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XamCreateEnumeratorHandle) { PPC_FUNC_PROLOGUE(); LSW_XSTUB_LOG(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XamFree) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XamGetPrivateEnumStructureFromHandle) { PPC_FUNC_PROLOGUE(); LSW_XSTUB_LOG(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XamUserGetMembershipTierFromXUID) { PPC_FUNC_PROLOGUE(); LSW_XSTUB_LOG(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XamUserGetOnlineCountryFromXUID) { PPC_FUNC_PROLOGUE(); LSW_XSTUB_LOG(); ctx.r3.u32 = 0; }
// XamUserGetXUID(user r3, type_mask r4, u64* xuid r5) — Xenia xam_user.cc. Only user 0 is
// signed in (offline profile, Xenia's default XUID).
PPC_FUNC(__imp__XamUserGetXUID) {
    PPC_FUNC_PROLOGUE();
    uint32_t user = ctx.r3.u32, p = ctx.r5.u32;
    static int n = 0; if (n < 4) { n++; dbg_ram("[XAM] XamUserGetXUID user=%u mask=0x%X out=0x%08X lr=0x%08X\n", user, ctx.r4.u32, p, (uint32_t)ctx.lr); }
    if (!(p >= 0x80000000u && p < 0x94000000u)) { ctx.r3.u32 = 0x80070057u; return; }   // E_INVALIDARG
    PPC_STORE_U64(p, 0);
    if (user >= 4) { ctx.r3.u32 = 0x80070057u; return; }
    if (user != 0) { ctx.r3.u32 = 0x80070525u; return; }                                 // NO_SUCH_USER
    PPC_STORE_U64(p, 0xB13EBABEBABEBABEull);
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__XamUserWriteProfileSettings) { PPC_FUNC_PROLOGUE(); LSW_XSTUB_LOG(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XexCheckExecutablePrivilege) { PPC_FUNC_PROLOGUE(); LSW_XSTUB_LOG(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NtResumeThread) {
    PPC_FUNC_PROLOGUE();
    GuestThread* gt = nullptr;
    { std::lock_guard<std::mutex> lk(g_thread_mutex); auto it = g_thread_map.find(ctx.r3.u32); if (it != g_thread_map.end()) gt = it->second; }
    if (gt) { SetEvent(gt->resume_ev); sched_add_slot(gt->sched_id); }
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__XamContentClose) { PPC_FUNC_PROLOGUE(); LSW_XSTUB_LOG(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XamContentCreateDeviceEnumerator) {
    PPC_FUNC_PROLOGUE();
    static int n = 0; if (n < 8) { n++;
        dbg_ram("[XAM] XamContentCreateDeviceEnumerator r3=0x%X r4=0x%X r5=0x%X r6=0x%08X r7=0x%08X\n",
                ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32); }
    ctx.r3.u32 = 0;
}
// XamContentCreateEnumerator(r3 user, r4 device_id, r5 content_type, r6 content_flags,
//   r7 items_per_enumerate, r8 buffer_size_ptr, r9 handle_out)  [Xenia xam_content.cc]
// No saved content exists: return a valid enumerator handle and the buffer size the caller must
// allocate (items * sizeof(XCONTENT_DATA) = 0x134); XamEnumerate then reports no more files.
static uint32_t g_content_enum_next = 0xE7E70001u;
PPC_FUNC(__imp__XamContentCreateEnumerator) {
    PPC_FUNC_PROLOGUE();
    static int n = 0; if (n < 8) { n++;
        dbg_ram("[XAM] XamContentCreateEnumerator user=0x%X dev=0x%X type=0x%X flags=0x%X items=%u cb*=0x%08X h*=0x%08X\n",
                ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32); }
    uint32_t items = ctx.r7.u32 ? ctx.r7.u32 : 1;
    if (ctx.r8.u32 >= 0x80000000u && ctx.r8.u32 < 0x94000000u) PPC_STORE_U32(ctx.r8.u32, items * 0x134u);
    if (ctx.r9.u32 >= 0x80000000u && ctx.r9.u32 < 0x94000000u) PPC_STORE_U32(ctx.r9.u32, g_content_enum_next++);
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__XamContentCreateEx) {
    PPC_FUNC_PROLOGUE();
    static int n = 0; if (n < 8) { n++;
        dbg_ram("[XAM] XamContentCreateEx user=0x%X root=0x%08X data=0x%08X flags=0x%X disp*=0x%08X lic*=0x%08X r9=0x%X r10=0x%X\n",
                ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32); }
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__XamContentDelete) { PPC_FUNC_PROLOGUE(); LSW_XSTUB_LOG(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XamContentFlush) { PPC_FUNC_PROLOGUE(); LSW_XSTUB_LOG(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XamContentGetCreator) { PPC_FUNC_PROLOGUE(); LSW_XSTUB_LOG(); ctx.r3.u32 = 0; }
// Content devices as in Xenia xam_content_device.cc: id 1 = "Dummy HDD" (20 GB, 10 GB free),
// id 2 = read-only ODD; anything else is DEVICE_NOT_CONNECTED (0x48F).
static bool xam_device_known(uint32_t id) { return id == 1 || id == 2; }
// XamContentGetDeviceData(device_id r3, X_CONTENT_DEVICE_DATA* r4): 0x50 bytes —
// id, type, total_bytes u64, free_bytes u64, name u16[28].
PPC_FUNC(__imp__XamContentGetDeviceData) {
    PPC_FUNC_PROLOGUE();
    uint32_t dev = ctx.r3.u32, p = ctx.r4.u32;
    static int n = 0; if (n < 8) { n++; dbg_ram("[XAM] XamContentGetDeviceData dev=0x%X out=0x%08X lr=0x%08X\n", dev, p, (uint32_t)ctx.lr); }
    if (!xam_device_known(dev)) { ctx.r3.u32 = 0x0000048Fu; return; }
    if (p >= 0x80000000u && p < 0x94000000u) {
        const uint64_t GB = 1024ull * 1024ull * 1024ull;
        bool hdd = dev == 1;
        for (uint32_t i = 0; i < 0x50; i += 4) PPC_STORE_U32(p + i, 0);
        PPC_STORE_U32(p + 0, dev);
        PPC_STORE_U32(p + 4, dev);                           // DeviceType HDD = 1, ODD = 2
        PPC_STORE_U64(p + 8, hdd ? 20 * GB : 7 * GB);
        PPC_STORE_U64(p + 16, hdd ? 10 * GB : 0);
        const char* name = hdd ? "Dummy HDD" : "Dummy ODD";
        for (uint32_t i = 0; name[i] && i < 27; ++i) PPC_STORE_U16(p + 24 + i * 2, (uint16_t)name[i]);
    }
    ctx.r3.u32 = 0;
}
// XamContentGetDeviceState(device_id r3, overlapped r4)
PPC_FUNC(__imp__XamContentGetDeviceState) {
    PPC_FUNC_PROLOGUE();
    uint32_t dev = ctx.r3.u32, ovl = ctx.r4.u32;
    static int n = 0; if (n < 8) { n++; dbg_ram("[XAM] XamContentGetDeviceState dev=0x%X ovl=0x%08X lr=0x%08X\n", dev, ovl, (uint32_t)ctx.lr); }
    uint32_t result = xam_device_known(dev) ? 0u : 0x0000048Fu;
    if (ovl >= 0x80000000u && ovl < 0x94000000u) {
        xmsg_complete_overlapped(base, ovl, "XamContentGetDeviceState");
        if (result) { PPC_STORE_U32(ovl + 0, 0x0000065Bu); PPC_STORE_U32(ovl + 24, result); }   // FUNCTION_FAILED + ext error
        ctx.r3.u32 = 0x000003E5u;
        return;
    }
    ctx.r3.u32 = result;
}
PPC_FUNC(__imp__XamContentGetLicenseMask) { PPC_FUNC_PROLOGUE(); LSW_XSTUB_LOG(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XamContentGetThumbnail) { PPC_FUNC_PROLOGUE(); LSW_XSTUB_LOG(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XamContentInstall) { PPC_FUNC_PROLOGUE(); LSW_XSTUB_LOG(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XamContentSetThumbnail) { PPC_FUNC_PROLOGUE(); LSW_XSTUB_LOG(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XamParseGamerTileKey) { PPC_FUNC_PROLOGUE(); LSW_XSTUB_LOG(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XamReadTileToTexture) { PPC_FUNC_PROLOGUE(); LSW_XSTUB_LOG(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XamUserCreateAchievementEnumerator) { PPC_FUNC_PROLOGUE(); LSW_XSTUB_LOG(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XamUserCreateStatsEnumerator) { PPC_FUNC_PROLOGUE(); LSW_XSTUB_LOG(); ctx.r3.u32 = 0; }
// XamUserGetName(user r3, char* buffer r4, buffer_len r5 incl. NUL) — Xenia xam_user.cc.
PPC_FUNC(__imp__XamUserGetName) {
    PPC_FUNC_PROLOGUE();
    uint32_t user = ctx.r3.u32, p = ctx.r4.u32, len = ctx.r5.u32;
    static int n = 0; if (n < 4) { n++; dbg_ram("[XAM] XamUserGetName user=%u buf=0x%08X len=%u lr=0x%08X\n", user, p, len, (uint32_t)ctx.lr); }
    bool p_ok = p >= 0x80000000u && p < 0x94000000u;
    if (user >= 4) { ctx.r3.u32 = 0x00000057u; return; }                      // INVALID_PARAMETER
    if (user != 0) { if (p_ok) PPC_STORE_U8(p, 0); ctx.r3.u32 = 0x00000525u; return; }
    const char name[] = "Player";
    uint32_t n_copy = len < sizeof(name) ? len : (uint32_t)sizeof(name);
    if (p_ok) for (uint32_t i = 0; i < n_copy; ++i) PPC_STORE_U8(p + i, (uint8_t)name[i]);
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__XamWriteGamerTile) { PPC_FUNC_PROLOGUE(); LSW_XSTUB_LOG(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__DbgBreakPoint) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__FscGetCacheElementCount) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__FscSetCacheElementCount) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__KeDelayExecutionThread) {
    PPC_FUNC_PROLOGUE();
    // Cooperative delay: hand the turn to other guest threads (deterministic) instead of a host
    // yield/Sleep (host-timed → build-fragile non-determinism). The guest uses this to let workers
    // make progress; the round-robin already gives them turns. (Switch: real sleep on real cores.)
    if (gil_on() && blockwait_on()) {
        uint32_t ms = guest_timeout_ms(base, ctx.r5.u32, 1u);
        if (ms == 0) sched_yield_turn();
        else { GilYield y; Sleep(std::min<uint32_t>(ms, 1000u)); }
    } else if (gil_on()) sched_yield_turn();
    else { GilYield y; std::this_thread::yield(); }
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__MmQueryAllocationSize) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__MmSetAddressProtect) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NtFlushBuffersFile) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NtPulseEvent) {
    PPC_FUNC_PROLOGUE();
    HANDLE h = kobj_get(ctx.r3.u32);
    if (h) PulseEvent(h);
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__NtSignalAndWaitForSingleObjectEx) {
    PPC_FUNC_PROLOGUE();
    // r3=signal_handle, r4=wait_handle, r7=*timeout
    // Switch: atomically signal + wait is nn::os::SignalEvent + WaitEvent
    HANDLE sh = kobj_get(ctx.r3.u32);
    HANDLE wh = kobj_get(ctx.r4.u32);
    if (sh) SetEvent(sh);
    if (wh) {
        DWORD ms = INFINITE;
        if (ctx.r7.u32 >= 0x80000000u && ctx.r7.u32 < 0x94000000u) {
            int64_t t = (int64_t)PPC_LOAD_U64(ctx.r7.u32);
            if (t == 0) ms = 0;
            else if (t < 0) { uint64_t rel = (uint64_t)(-t); ms = (DWORD)(rel / 10000u); if (!ms) ms = 1; }
        }
        WaitForSingleObject(wh, ms);
    }
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__ObOpenObjectByName) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__RtlRaiseException) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
// XGetGameRegion: 0 is not a real region. XC_GAME_REGION_NA = 0x00FF ("region free"
// per Xenia default is 0xFFFF; NA disc = 0x00FF). Region can select NTSC/PAL assets.
PPC_FUNC(__imp__XGetGameRegion) {
    PPC_FUNC_PROLOGUE();
    static int n = 0; if (n < 4) { n++; dbg_ram("[XAM] XGetGameRegion -> 0x00FF (NA)\n"); }
    ctx.r3.u32 = 0x00FFu;
}
// XamContentGetDeviceName(device_id r3, wchar buffer r4, capacity in chars r5)
PPC_FUNC(__imp__XamContentGetDeviceName) {
    PPC_FUNC_PROLOGUE();
    uint32_t dev = ctx.r3.u32, p = ctx.r4.u32, cap = ctx.r5.u32;
    static int n = 0; if (n < 8) { n++; dbg_ram("[XAM] XamContentGetDeviceName dev=0x%X buf=0x%08X cap=%u lr=0x%08X\n", dev, p, cap, (uint32_t)ctx.lr); }
    if (!xam_device_known(dev)) { ctx.r3.u32 = 0x0000048Fu; return; }
    const char* name = dev == 1 ? "Dummy HDD" : "Dummy ODD";
    uint32_t len = (uint32_t)strlen(name);
    if (cap < len + 1) { ctx.r3.u32 = 0x0000007Au; return; }   // ERROR_INSUFFICIENT_BUFFER
    if (p >= 0x80000000u && p < 0x94000000u)
        for (uint32_t i = 0; i <= len; ++i) PPC_STORE_U16(p + i * 2, (uint16_t)name[i]);
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__XamContentLaunchImage) { PPC_FUNC_PROLOGUE(); LSW_XSTUB_LOG(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XamContentResolve) {
    PPC_FUNC_PROLOGUE();
    // Return ERROR_PATH_NOT_FOUND so callers fall through to direct disc-path logic.
    // Returning 0 (success) with an unfilled buffer causes stale stack data to be
    // used as the resolved path, which breaks subsequent NtOpenFile calls.
    ctx.r3.u32 = 0x80070003u;  // HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)
}
// XamEnumerate(hEnum r3, flags r4, buffer r5, cbBuffer r6, pcItems r7, ovl r8).
// Old stub returned 0 (success) with NOTHING written — the caller reads a garbage item
// count / item data. Report "no more files" properly: *pcItems=0, ret ERROR_NO_MORE_FILES.
PPC_FUNC(__imp__XamEnumerate) {
    PPC_FUNC_PROLOGUE();
    static int n = 0; if (n < 12) { n++;
        dbg_ram("[XAM] XamEnumerate hEnum=0x%X flags=0x%X buf=0x%08X cb=%u pcItems=0x%08X ovl=0x%08X\n",
                ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32); }
    { static uint64_t calls = 0; if ((++calls % 600) == 0) dbg_ram("[XAM] XamEnumerate call #%llu hEnum=0x%X\n", (unsigned long long)calls, ctx.r3.u32); }
    if (ctx.r7.u32 >= 0x80000000u && ctx.r7.u32 < 0x94000000u) PPC_STORE_U32(ctx.r7.u32, 0);
    if (ctx.r8.u32 >= 0x80000000u && ctx.r8.u32 < 0x94000000u) {
        // Async: the result goes in the overlapped (InternalLow), the call returns IO_PENDING.
        xmsg_complete_overlapped(base, ctx.r8.u32, "XamEnumerate");
        PPC_STORE_U32(ctx.r8.u32 + 0, 0x00000012u);   // ERROR_NO_MORE_FILES
        ctx.r3.u32 = 0x000003E5u;                      // ERROR_IO_PENDING
        return;
    }
    ctx.r3.u32 = 0x00000012u;  // ERROR_NO_MORE_FILES
}
PPC_FUNC(__imp__XamInputGetCapabilities) {
    PPC_FUNC_PROLOGUE();
    // r3=user_index, r4=flags, r5=X_INPUT_CAPABILITIES*. Report a connected gamepad
    // for user 0 (so the game treats the pad as present) and zero the struct.
    if (ctx.r3.u32 != 0) { ctx.r3.u32 = 0x0000048Fu; return; }   // DEVICE_NOT_CONNECTED
    uint32_t p = ctx.r5.u32;
    if (p) { for (int i = 0; i < 20; i += 4) PPC_STORE_U32(p + i, 0);
             PPC_STORE_U8(p + 0, 1);   // type = XINPUT_DEVTYPE_GAMEPAD
             PPC_STORE_U8(p + 1, 1); } // sub_type = XINPUT_DEVSUBTYPE_GAMEPAD
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__XamInputGetKeystrokeEx) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0x000010DDu; } // ERROR_EMPTY (no key)
// ── Host input -> guest XamInputGetState ─────────────────────────────────────────────────────
// X_INPUT_STATE on the 360 has the same layout and button bits as PC XINPUT_STATE
// (packet u32, buttons u16, LT/RT u8, LX/LY/RX/RY s16), stored big-endian. Sources, OR'd together:
//   1. a real XInput controller (xinput1_4.dll, loaded dynamically; LSWTCS_NOPAD=1 ignores it)
//   2. keyboard while the game window has focus: Enter=Start, Esc=Back, Space/Z=A, X=B, C=X,
//      V=Y, arrows=D-pad, WASD=left stick, Q/E=shoulders, R/F=triggers
//   3. LSWTCS_AUTOSTART=1: periodic Start/A pulses (unattended runs)
//   4. DIAG: file "press_now" in the working dir -> whitespace-separated button names
//      (start back a b x y up down left right lb rb, or "wait"), each held 8 polls then released 8
struct LswPad { uint16_t buttons; uint8_t lt, rt; int16_t lx, ly, rx, ry; };
static bool lsw_host_pad(LswPad& out) {
    typedef DWORD (WINAPI *PFN_XIGS)(DWORD, void*);
    static PFN_XIGS xigs = nullptr; static int tried = 0;
    static int nopad = -1; if (nopad < 0) { const char* e = getenv("LSWTCS_NOPAD"); nopad = (e && e[0] == '1') ? 1 : 0; }
    if (nopad) return false;
    if (!tried) { tried = 1;
        HMODULE m = LoadLibraryA("xinput1_4.dll"); if (!m) m = LoadLibraryA("xinput9_1_0.dll");
        if (m) xigs = (PFN_XIGS)GetProcAddress(m, "XInputGetState"); }
    if (!xigs) return false;
    struct { DWORD packet; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } st{};
    for (DWORD i = 0; i < 4; ++i)
        if (xigs(i, &st) == 0) { out = {st.buttons, st.lt, st.rt, st.lx, st.ly, st.rx, st.ry}; return true; }
    return false;
}
static void lsw_keyboard_pad(LswPad& p) {
    HWND fg = GetForegroundWindow(); DWORD pid = 0;
    if (!fg) return;
    GetWindowThreadProcessId(fg, &pid);
    if (pid != GetCurrentProcessId()) return;
    auto k = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    if (k(VK_UP)) p.buttons |= 0x0001;   if (k(VK_DOWN)) p.buttons |= 0x0002;
    if (k(VK_LEFT)) p.buttons |= 0x0004; if (k(VK_RIGHT)) p.buttons |= 0x0008;
    if (k(VK_RETURN)) p.buttons |= 0x0010; if (k(VK_ESCAPE)) p.buttons |= 0x0020;
    if (k('Q')) p.buttons |= 0x0100; if (k('E')) p.buttons |= 0x0200;
    if (k(VK_SPACE) || k('Z')) p.buttons |= 0x1000; if (k('X')) p.buttons |= 0x2000;
    if (k('C')) p.buttons |= 0x4000; if (k('V')) p.buttons |= 0x8000;
    if (k('R')) p.lt = 255; if (k('F')) p.rt = 255;
    if (k('A')) p.lx = -32767; if (k('D')) p.lx = 32767;
    if (k('W')) p.ly = 32767;  if (k('S')) p.ly = -32767;
}
PPC_FUNC(__imp__XamInputGetState) {
    PPC_FUNC_PROLOGUE();
    static std::atomic<uint32_t> calls{0};
    uint32_t n = ++calls;
    if (ctx.r3.u32 != 0) { ctx.r3.u32 = 0x0000048Fu; return; }   // only player 0 connected
    uint32_t p = ctx.r5.u32;
    LswPad pad{};
    lsw_host_pad(pad);
    lsw_keyboard_pad(pad);
    { static int at = -1; if (at < 0) { const char* e = getenv("LSWTCS_AUTOSTART_AT"); at = e ? atoi(e) : 0; }
      if (at > 0 && n >= (uint32_t)at && n < (uint32_t)at + 6) pad.buttons |= 0x0010; }   // one Start press
    {   // DIAG: press_now script
        static std::vector<uint16_t> q; static uint32_t qpos = 0, qtick = 0, qpoll = 0;
        if ((++qpoll & 15) == 0 && q.empty() && GetFileAttributesA("press_now") != INVALID_FILE_ATTRIBUTES) {
            FILE* f = fopen("press_now", "rb"); char w[32];
            static const struct { const char* name; uint16_t bit; } kB[] = {
                {"up",0x0001},{"down",0x0002},{"left",0x0004},{"right",0x0008},{"start",0x0010},{"back",0x0020},
                {"lb",0x0100},{"rb",0x0200},{"a",0x1000},{"b",0x2000},{"x",0x4000},{"y",0x8000},{"wait",0}};
            if (f) { while (fscanf(f, "%31s", w) == 1) for (auto& b : kB) if (!_stricmp(w, b.name)) q.push_back(b.bit); fclose(f); }
            DeleteFileA("press_now"); qpos = 0; qtick = 0;
            dbg_ram("[PRESS] queued %u presses\n", (unsigned)q.size()); printf("[PRESS] queued %u presses\n", (unsigned)q.size()); fflush(stdout);
        }
        if (!q.empty()) {
            if (qtick < 8) pad.buttons |= q[qpos];
            if (++qtick >= 16) { qtick = 0; if (++qpos >= q.size()) { q.clear(); qpos = 0; } }
        }
    }
    static int s_auto = -1; if (s_auto < 0) s_auto = getenv("LSWTCS_AUTOSTART") ? 1 : 0;
    if (s_auto) {
        const uint32_t WARM = 300, PERIOD = 120, WIDTH = 6;
        if (n > WARM) { uint32_t ph = (n - WARM) % PERIOD;
            if (ph < WIDTH) pad.buttons |= ((n - WARM) / PERIOD) & 1 ? 0x1000 : 0x0010; }
    }
    // Packet number must change when the state changes; bump it whenever anything differs.
    static LswPad last{}; static uint32_t packet = 1;
    if (memcmp(&pad, &last, sizeof(pad)) != 0) { ++packet; last = pad; }
    if (p) {
        PPC_STORE_U32(p + 0, packet);
        PPC_STORE_U32(p + 4, ((uint32_t)pad.buttons << 16) | ((uint32_t)pad.lt << 8) | pad.rt);
        PPC_STORE_U32(p + 8, ((uint32_t)(uint16_t)pad.lx << 16) | (uint16_t)pad.ly);
        PPC_STORE_U32(p + 12, ((uint32_t)(uint16_t)pad.rx << 16) | (uint16_t)pad.ry);
    }
    static uint16_t lastb = 0;
    if (pad.buttons != lastb) { static int bl = 0; if (bl++ < 200) dbg_ram("[XInput] buttons=0x%04X call#%u\n", pad.buttons, n); lastb = pad.buttons; }
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__XamInputSetState) { PPC_FUNC_PROLOGUE(); LSW_XSTUB_LOG(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__RtlTimeFieldsToTime) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XamGetExecutionId) {
    PPC_FUNC_PROLOGUE();
    static int n = 0; if (n < 4) { n++; dbg_ram("[XAM] XamGetExecutionId out*=0x%08X (null-filled)\n", ctx.r3.u32); }
    if (ctx.r3.u32 >= 0x80000000u && ctx.r3.u32 < 0x94000000u) PPC_STORE_U32(ctx.r3.u32, 0);
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__XamShowAchievementsUI) { PPC_FUNC_PROLOGUE(); LSW_XSTUB_LOG(); ctx.r3.u32 = 0; }
// XamShowDeviceSelectorUI(user r3, contentType r4, flags r5, bytesRequested r6,
// pDeviceID r7, pOverlapped r8). The storage-device blade: titles call this at boot
// (save-game check) and WAIT on the overlapped. Write device id 1 + complete the ovl.
// Xenia xeXamDispatchHeadless: XN_SYS_UI 1, run, complete the overlapped with the result,
// XN_SYS_UI 0 after 100 ms. Returns ERROR_IO_PENDING when an overlapped was given.
static uint32_t xam_dispatch_headless(uint8_t* base, uint32_t ovl, uint32_t result, const char* who) {
    if (notify_old()) { xmsg_complete_overlapped(base, ovl, who); return 0; }
    notify_broadcast(0x00000009u, 1);
    xmsg_complete_overlapped(base, ovl, who);
    if (ovl >= 0x80000000u && ovl < 0x94000000u) PPC_STORE_U32(ovl + 0, result);
    notify_broadcast(0x00000009u, 0, 100);
    return (ovl >= 0x80000000u && ovl < 0x94000000u) ? 0x000003E5u : result;   // ERROR_IO_PENDING
}
PPC_FUNC(__imp__XamShowDeviceSelectorUI) {
    PPC_FUNC_PROLOGUE();
    static int n = 0; if (n < 8) { n++;
        dbg_ram("[XAM] XamShowDeviceSelectorUI user=0x%X type=0x%X flags=0x%X bytes=0x%llX dev*=0x%08X ovl=0x%08X lr=0x%08X\n",
                ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u64, ctx.r7.u32, ctx.r8.u32, (uint32_t)ctx.lr); }
    if (ctx.r7.u32 >= 0x80000000u && ctx.r7.u32 < 0x94000000u) PPC_STORE_U32(ctx.r7.u32, 1);   // HDD
    ctx.r3.u32 = xam_dispatch_headless(base, ctx.r8.u32, 0, "XamShowDeviceSelectorUI");
}
PPC_FUNC(__imp__XamShowFriendsUI) { PPC_FUNC_PROLOGUE(); LSW_XSTUB_LOG(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XamShowGamerCardUIForXUID) { PPC_FUNC_PROLOGUE(); LSW_XSTUB_LOG(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XamShowKeyboardUI) { PPC_FUNC_PROLOGUE(); LSW_XSTUB_LOG(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XamShowMarketplaceUI) { PPC_FUNC_PROLOGUE(); LSW_XSTUB_LOG(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XamShowMessageBoxUI) {
    PPC_FUNC_PROLOGUE();
    static int n = 0; if (n < 8) { n++;
        dbg_ram("[XAM] XamShowMessageBoxUI r3=0x%X r4=0x%08X r5=0x%08X r6=0x%X r7=0x%08X r8=0x%X r9=0x%X r10=0x%08X\n",
                ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32); }
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__XamShowMessagesUI) { PPC_FUNC_PROLOGUE(); LSW_XSTUB_LOG(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XamShowPlayerReviewUI) { PPC_FUNC_PROLOGUE(); LSW_XSTUB_LOG(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XamShowPlayersUI) { PPC_FUNC_PROLOGUE(); LSW_XSTUB_LOG(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XamShowQuickChatUI) { PPC_FUNC_PROLOGUE(); LSW_XSTUB_LOG(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XamShowSigninUI) { PPC_FUNC_PROLOGUE(); LSW_XSTUB_LOG(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XamShowSigninUIp) { PPC_FUNC_PROLOGUE(); LSW_XSTUB_LOG(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XamShowVoiceMailUI) { PPC_FUNC_PROLOGUE(); LSW_XSTUB_LOG(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XexGetModuleSection) { PPC_FUNC_PROLOGUE(); LSW_XSTUB_LOG(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XexLoadImage) { PPC_FUNC_PROLOGUE(); LSW_XSTUB_LOG(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XexUnloadImage) { PPC_FUNC_PROLOGUE(); LSW_XSTUB_LOG(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XexUnloadImageAndExitThread) { PPC_FUNC_PROLOGUE(); LSW_XSTUB_LOG(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__KeBugCheckEx) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__KeQueryBasePriorityThread) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NtQueueApcThread) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NtSuspendThread) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__ObLookupThreadByThreadId) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__ObOpenObjectByPointer) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__Refresh) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__RtlCompareMemory) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__RtlCompareMemoryUlong) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__RtlFillMemoryUlong) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__VdQuerySystemCommandBuffer) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__VdSetSystemCommandBuffer) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XGetAVPack) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XamLoaderLaunchTitle) { PPC_FUNC_PROLOGUE(); LSW_XSTUB_LOG(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XamShowDirtyDiscErrorUI) { PPC_FUNC_PROLOGUE(); LSW_XSTUB_LOG(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XamShowFriendRequestUI) { PPC_FUNC_PROLOGUE(); LSW_XSTUB_LOG(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XamShowGameInviteUI) { PPC_FUNC_PROLOGUE(); LSW_XSTUB_LOG(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XamShowMessageBoxUIEx) { PPC_FUNC_PROLOGUE(); LSW_XSTUB_LOG(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__XamShowMessageComposeUI) { PPC_FUNC_PROLOGUE(); LSW_XSTUB_LOG(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__sprintf) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__KeEnterCriticalRegion) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__KiApcNormalRoutineNop) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__VdCallGraphicsNotificationRoutines) {
    PPC_FUNC_PROLOGUE();
    dbg_ram("[Vd] VdCallGraphicsNotificationRoutines r3=0x%08X r4=0x%08X\n", ctx.r3.u32, ctx.r4.u32);
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__VdEnableDisableClockGating) {
    PPC_FUNC_PROLOGUE();
    dbg_ram("[Vd] VdEnableDisableClockGating r3=0x%08X\n", ctx.r3.u32);
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__VdEnableRingBufferRPtrWriteBack) {
    PPC_FUNC_PROLOGUE();
    // r3 = writeback ptr (game polls this address for GPU read-pointer updates)
    // r4 = log2(block_size), usually 6 → block_size = 64 dwords
    g_rb_rptr_writeback   = ctx.r3.u32;
    g_rb_rptr_block_log2  = ctx.r4.u32;
    dbg_ram("[Vd] VdEnableRingBufferRPtrWriteBack wb_ptr=0x%08X block_log2=%u\n",
           ctx.r3.u32, ctx.r4.u32);
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__VdGetCurrentDisplayGamma) {
    PPC_FUNC_PROLOGUE();
    // r3 = *type_ptr  (1=sRGB, 2=BT.709/HDTV, 3=power)
    // r4 = *power_ptr (float, only used for type 3)
    // Xenia: type=2 (BT.709), power=2.22222233f
    if (ctx.r3.u32) PPC_STORE_U32(ctx.r3.u32, 2);
    if (ctx.r4.u32) {
        float pwr = 2.22222233f;
        uint32_t pwr_bits; memcpy(&pwr_bits, &pwr, 4);
        PPC_STORE_U32(ctx.r4.u32, pwr_bits);
    }
    dbg_ram("[Vd] VdGetCurrentDisplayGamma -> type=2 power=2.222\n");
}
PPC_FUNC(__imp__VdGetCurrentDisplayInformation) {
    PPC_FUNC_PROLOGUE();
    // r3 = pointer to X_DISPLAY_INFO (0x58 bytes, mixed big-endian)
    // Layout matches Xenia's xboxkrnl_video.cc VdGetCurrentDisplayInformation_entry
    uint32_t ptr = ctx.r3.u32;
    if (!ptr) return;
    memset(base + ptr, 0, 0x58);
    // front_buffer_width/height (be<uint16_t> at +0, +2)
    PPC_STORE_U16(ptr + 0x00, 1280);
    PPC_STORE_U16(ptr + 0x02, 720);
    // scaler_parameters at +8: X_D3DPRIVATE_SCALER_PARAMETERS (0x38 bytes)
    //   scaler_source_rect (X_D3DPRIVATE_RECT, 4x uint32 be): x1,y1,x2,y2
    PPC_STORE_U32(ptr + 0x08 +  0, 0);     // x1
    PPC_STORE_U32(ptr + 0x08 +  4, 0);     // y1
    PPC_STORE_U32(ptr + 0x08 +  8, 1280);  // x2
    PPC_STORE_U32(ptr + 0x08 + 12, 720);   // y2
    //   scaled_output_width/height at +0x10, +0x14
    PPC_STORE_U32(ptr + 0x18, 1280);
    PPC_STORE_U32(ptr + 0x1C, 720);
    //   vertical_filter_type at +0x20, horizontal_filter_type at +0x30
    PPC_STORE_U32(ptr + 0x20, 1);
    PPC_STORE_U32(ptr + 0x30, 1);
    // overscan (be<uint16_t> at +0x40..+0x46)
    PPC_STORE_U16(ptr + 0x40, 320);   // overscan_left
    PPC_STORE_U16(ptr + 0x42, 180);   // overscan_top
    PPC_STORE_U16(ptr + 0x44, 320);   // overscan_right
    PPC_STORE_U16(ptr + 0x46, 180);   // overscan_bottom
    PPC_STORE_U16(ptr + 0x48, 1280);  // display_width
    PPC_STORE_U16(ptr + 0x4A, 720);   // display_height
    { float rf = 60.0f; uint32_t b; memcpy(&b, &rf, 4); PPC_STORE_U32(ptr + 0x4C, b); }  // refresh_rate
    PPC_STORE_U32(ptr + 0x50, 0);     // display_interlaced = false
    PPC_STORE_U16(ptr + 0x56, 1280);  // actual_display_width
    dbg_ram("[Vd] VdGetCurrentDisplayInformation -> 1280x720\n");
}
PPC_FUNC(__imp__VdGetSystemCommandBuffer) {
    PPC_FUNC_PROLOGUE();
    // r3 = ptr to be filled with system command buffer base (slot 0)
    // r4 = ptr to be filled with system command buffer token (slot 1)
    // Xenia writes 0xBEEF0000 / 0xBEEF0001 as sentinel values.
    // These are passed through to VdSwap as unk3/unk4 (r6/r7) — we don't use them.
    static bool logged = false;
    if (!logged) {
        dbg_ram("[Vd] VdGetSystemCommandBuffer r3=0x%08X r4=0x%08X\n",
               ctx.r3.u32, ctx.r4.u32);
        logged = true;
    }
    if (ctx.r3.u32 >= 0x80000000u && ctx.r3.u32 < 0x94000000u) {
        memset(g_base + ctx.r3.u32, 0, 0x94);        // zero 0x94 bytes like Xenia
        PPC_STORE_U32(ctx.r3.u32, 0xBEEF0000u);
    }
    if (ctx.r4.u32 >= 0x80000000u && ctx.r4.u32 < 0x94000000u)
        PPC_STORE_U32(ctx.r4.u32, 0xBEEF0001u);
}
PPC_FUNC(__imp__VdInitializeEngines) {
    PPC_FUNC_PROLOGUE();
    dbg_ram("[Vd] VdInitializeEngines r3=0x%08X r4=0x%08X r5=0x%08X r6=0x%08X\n",
           ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
    ctx.r3.u32 = 1;  // Xenia returns 1
}
PPC_FUNC(__imp__VdInitializeRingBuffer) {
    PPC_FUNC_PROLOGUE();
    // r3 = ring buffer physical base address (from MmAllocatePhysicalMemory)
    // r4 = log2(size_in_dwords), so size = (1<<r4)*4 bytes
    g_rb_base = ctx.r3.u32;
    g_rb_size = 1u << (ctx.r4.u32 + 3);   // Xenia InitializeRingBuffer: size_bytes = 1 << (size_log2 + 3) (was <<2: half the ring)
    dbg_ram("[Vd] VdInitializeRingBuffer base=0x%08X log2=%u size=0x%X bytes\n",
           g_rb_base, ctx.r4.u32, g_rb_size);
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__VdInitializeScalerCommandBuffer) {
    PPC_FUNC_PROLOGUE();
    // r3-r10 = args 1-8; args 9-12 passed on stack at SP+84,92,100,108 (observed in recomp.346)
    //   SP+100 = dest_ptr  (buffer to fill with NOP PM4 packets)
    //   SP+108 = dest_count (number of dwords to fill)
    // Xenia: fill dest[0..dest_count-1] with 0x80000000 (TYPE-2 NOP), return dest_count.
    uint32_t dest_ptr   = PPC_LOAD_U32(ctx.r1.u32 + 100);
    uint32_t dest_count = PPC_LOAD_U32(ctx.r1.u32 + 108);
    if (dest_ptr && dest_count && dest_count < 0x10000) {
        for (uint32_t i = 0; i < dest_count; i++)
            PPC_STORE_U32(dest_ptr + i * 4, 0x80000000u);
    }
    dbg_ram("[Vd] VdInitializeScalerCommandBuffer dest=0x%08X count=%u\n",
           dest_ptr, dest_count);
    ctx.r3.u32 = dest_count;
}
PPC_FUNC(__imp__VdIsHSIOTrainingSucceeded) {
    PPC_FUNC_PROLOGUE();
    // Return 0 (training NOT done) so the init code sets *(0x82FE0E00)=1,
    // which unblocks AEEB0's ring buffer submission path.
    // Returning 1 (done) sets the flag to 0 and permanently blocks AEEB0.
    dbg_ram("[Vd] VdIsHSIOTrainingSucceeded -> 0\n");
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__VdPersistDisplay) {
    PPC_FUNC_PROLOGUE();
    // r3 = unk0, r4 = *unk1_ptr (output: physical memory addr for MmFreePhysicalMemory)
    // Xenia: allocs 64 bytes of physical memory, writes address to *r4, returns 1
    if (ctx.r4.u32) {
        uint32_t phys = g_phys_alloc(64);
        if (phys) PPC_STORE_U32(ctx.r4.u32, phys);
    }
    dbg_ram("[Vd] VdPersistDisplay -> 1\n");
    ctx.r3.u32 = 1;
}
PPC_FUNC(__imp__VdQueryVideoFlags) {
    PPC_FUNC_PROLOGUE();
    // bit 0: is_widescreen, bit 1: hidef (>= 1280), bit 2: full-hidef (>= 1920)
    // 1280x720 widescreen => bits 0+1 = 3
    ctx.r3.u32 = 3;
}
PPC_FUNC(__imp__VdQueryVideoMode) {
    PPC_FUNC_PROLOGUE();
    // r3 = pointer to X_VIDEO_MODE (48 bytes, all big-endian)
    uint32_t ptr = ctx.r3.u32;
    if (!ptr) return;
    memset(base + ptr, 0, 48);
    PPC_STORE_U32(ptr +  0, 1280);   // display_width
    PPC_STORE_U32(ptr +  4, 720);    // display_height
    PPC_STORE_U32(ptr +  8, 0);      // is_interlaced
    PPC_STORE_U32(ptr + 12, 1);      // is_widescreen
    PPC_STORE_U32(ptr + 16, 1);      // is_hi_def (width >= 0x500)
    { float rf = 60.0f; uint32_t b; memcpy(&b, &rf, 4); PPC_STORE_U32(ptr + 20, b); }  // refresh_rate
    PPC_STORE_U32(ptr + 24, 1);      // video_standard = NTSCM
    PPC_STORE_U32(ptr + 28, 0x8A);   // pixel_rate
    PPC_STORE_U32(ptr + 32, 1);      // widescreen_flag
    dbg_ram("[Vd] VdQueryVideoMode -> 1280x720 widescreen 60Hz\n");
}
PPC_FUNC(__imp__VdRetrainEDRAM) {
    PPC_FUNC_PROLOGUE();
    static bool logged = false;
    if (!logged) { dbg_ram("[Vd] VdRetrainEDRAM (first call)\n"); logged = true; }
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__VdSetDisplayMode) {
    PPC_FUNC_PROLOGUE();
    dbg_ram("[Vd] VdSetDisplayMode r3=0x%08X r4=0x%08X r5=0x%08X r6=0x%08X r7=0x%08X\n",
           ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__VdSetGraphicsInterruptCallback) {
    PPC_FUNC_PROLOGUE();
    // r3 = callback function address (PPC code), r4 = user_data
    g_gfx_interrupt_callback = ctx.r3.u32;
    g_gfx_interrupt_data     = ctx.r4.u32;
    dbg_ram("[Vd] VdSetGraphicsInterruptCallback callback=0x%08X data=0x%08X\n",
           ctx.r3.u32, ctx.r4.u32);
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__VdSetSystemCommandBufferGpuIdentifierAddress) {
    PPC_FUNC_PROLOGUE();
    dbg_ram("[Vd] VdSetSystemCommandBufferGpuIdentifierAddress r3=0x%08X\n", ctx.r3.u32);
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__VdShutdownEngines) {
    PPC_FUNC_PROLOGUE();
    dbg_ram("[Vd] VdShutdownEngines\n");
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__KeLeaveCriticalRegion) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__KeLockL2) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__KeUnlockL2) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__NtQueryDirectoryFile) {
    PPC_FUNC_PROLOGUE();
    // STATUS_NO_MORE_FILES = 0x80000006: tells caller there are no directory entries.
    // Returning STATUS_SUCCESS (0) causes an infinite loop since the caller re-queries
    // expecting more entries; STATUS_NO_MORE_FILES terminates the enumeration.
    ctx.r3.u32 = 0x80000006u;
}
PPC_FUNC(__imp__NtReadFileScatter) {
    PPC_FUNC_PROLOGUE();
    // NtReadFileScatter(FileHandle r3, Event r4, ApcRoutine r5, ApcContext r6, IoStatus r7,
    //   SegmentArray r8, Length r9, ByteOffset r10, Key). If the game streams GAME.DAT through
    // this and we return 0 without filling segments, NO asset data loads → no menu content.
    static std::atomic<uint32_t> n{0}; uint32_t c = ++n;
    if (c <= 24) dbg_ram("[NtReadFileScatter] call#%u handle=0x%08X seg=0x%08X len=%u iosb=0x%08X\n",
                          c, ctx.r3.u32, ctx.r8.u32, ctx.r9.u32, ctx.r7.u32);
    ctx.r3.u32 = 0;
}
PPC_FUNC(__imp__ObCreateSymbolicLink) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }
PPC_FUNC(__imp__ObDeleteSymbolicLink) { PPC_FUNC_PROLOGUE(); ctx.r3.u32 = 0; }

