// Project root (set by CMake); data files are resolved relative to it.
#ifndef LSW_ROOT_DIR
#define LSW_ROOT_DIR ".."
#endif
#include "../../Convert 360/LSWTCS/output/ppc_config.h"
#include "../../Convert 360/LSWTCS/output/ppc_context.h"
#include "../../Convert 360/LSWTCS/output/ppc_recomp_shared.h"
#include "gpu_d3d12.h"
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <signal.h>
#include <windows.h>

#define PPC_IMAGE_BASE  0x82000000u
#define PPC_IMAGE_SIZE  0x1460000u
#define PPC_CODE_BASE   0x82130000u
#define PPC_CODE_SIZE   0xD3527Cu

extern void _xstart(PPCContext& ctx, uint8_t* base);
extern "C" void lswtcs_gil_enter();   // kernel_stubs.cpp — main thread joins the GIL
extern "C" void lswtcs_cmdlog_install(uint8_t* base); // kernel_stubs.cpp — LSWTCS_CMDLOG veneer tracer
extern "C" void lswtcs_fntrace_install(uint8_t* base); // kernel_stubs.cpp — LSWTCS_FNTRACE generic fn tracer
extern void sub_8229F180(PPCContext& ctx, uint8_t* base);  // per-thread init (THREAD_ATTACH) — see apiThreadStartup
extern void sub_82533258(PPCContext& ctx, uint8_t* base);  // heap alloc(r3=size, r4=flags) -> r3=addr
extern void sub_825332F0(PPCContext& ctx, uint8_t* base);  // heap free(r3=addr, r4=flags)
extern "C" void dbg_ram(const char* fmt, ...);
extern "C" uint32_t ppc_alloc_main_thread_pcr();

// ── Small-virtual-heap bypass allocator ─────────────────────────────────────
// The guest virtual-heap free-list (sub_822A15D8) is degenerate from the Xenia
// mid-execution dump: its doubly-linked lists are inconsistent, so allocations
// never advance (proven via LSWTCS_HEAPTEST). We service SMALL virtual-heap
// allocations (the broken size class — covers the shader-compiler context/temps)
// from our own simple bump+free-list over a reserved guest region the game never
// uses ([0xE0000000,...): above g_phys (0x88M-0xC0M) and the virtual heap
// (0x82-0x84M), mapped, zeroed by the startup broad-scan). Larger allocs fall
// through to the (working) guest path. Gated by LSWTCS_HEAPBYPASS so it can be
// toggled for A/B testing.
#include <unordered_map>
#include <vector>
#include <mutex>
// Pool placement: default high (0xE0M, definitely-free) but the game's pointer guards
// (addr<0x94000000) reject high addrs for LARGER introspected blocks. LSWTCS_POOL_LOW=1
// moves it to the top of the heap range (0x90-0x94M, left unused by the low-handing
// free-list) so those guards pass — testing whether that fixes the wide-bypass hang.
static uint32_t LSWTCS_POOL_BASE = 0xE0000000u;
static uint32_t LSWTCS_POOL_END  = 0xF0000000u;   // 256 MB pool
static uint32_t LSWTCS_POOL_MAX = 512u;   // only handle allocs <= this (LSWTCS_POOL_MAX env overrides)
static uint32_t lswtcs_pool_bump = LSWTCS_POOL_BASE;
static std::mutex lswtcs_pool_mtx;
static std::unordered_map<uint32_t,uint32_t> lswtcs_pool_live;            // addr -> rounded size
static std::unordered_map<uint32_t,std::vector<uint32_t>> lswtcs_pool_fl; // rounded size -> free addrs
extern uint8_t* g_base;                                                   // guest base (defined below)
static bool lswtcs_bypass_on() {
    static int v = -1;
    if (v < 0) {
        // Default ON (narrow bypass, POOL_MAX=512): the corrupt Xenia-dump free-list makes the
        // no-bypass path layout-fragile (T10 hang/crash). The narrow bypass is validated safe
        // ([T]18, 0 crashes). Disable explicitly with LSWTCS_HEAPBYPASS=0.
        const char* hb = getenv("LSWTCS_HEAPBYPASS");
        v = (hb && hb[0] == '0') ? 0 : 1;
        const char* m = getenv("LSWTCS_POOL_MAX");
        if (m) { unsigned long mv = strtoul(m, nullptr, 0); if (mv) LSWTCS_POOL_MAX = (uint32_t)mv; }
        if (getenv("LSWTCS_POOL_LOW")) { LSWTCS_POOL_BASE = 0x90000000u; LSWTCS_POOL_END = 0x94000000u; lswtcs_pool_bump = LSWTCS_POOL_BASE; }
    }
    return v != 0;
}
// LSWTCS_POOL_NOZERO=1 disables the calloc-style zeroing in lswtcs_pool_alloc (A/B testing).
static bool lswtcs_pool_nozero() {
    static int v = -1;
    if (v < 0) { const char* e = getenv("LSWTCS_POOL_NOZERO"); v = (e && e[0] != '0') ? 1 : 0; }
    return v != 0;
}
// returns a guest addr, or 0 if not handled (caller falls through to guest allocator)
extern "C" uint32_t lswtcs_pool_alloc(uint32_t size) {
    if (!lswtcs_bypass_on()) return 0;
    if (size == 0) size = 1;
    // Wide bypass ONLY during a shader compile (g_compile_active): the compiler needs its large
    // (2448B) nodes from the clean pool, but routing general game allocs to large headerless pool
    // blocks breaks code that introspects guest heap headers (the T10 init hang). Scoping wide to
    // the compile keeps general code on the stable narrow path. LSWTCS_POOL_MAX still tunes general.
    uint32_t maxsz = g_compile_active ? (LSWTCS_POOL_MAX > 0x10000u ? LSWTCS_POOL_MAX : 0x10000u)
                                      : LSWTCS_POOL_MAX;
    if (size > maxsz) return 0;
    uint32_t rsize = (size + 15u) & ~15u;
    std::lock_guard<std::mutex> lk(lswtcs_pool_mtx);
    auto& fl = lswtcs_pool_fl[rsize];
    uint32_t addr;
    if (!fl.empty()) { addr = fl.back(); fl.pop_back(); }
    else {
        if (lswtcs_pool_bump + rsize > LSWTCS_POOL_END) return 0;  // pool full -> guest
        addr = lswtcs_pool_bump; lswtcs_pool_bump += rsize;
    }
    lswtcs_pool_live[addr] = rsize;
    // Zero on hand-out (calloc semantics). The bump region holds stale guest-image bytes and
    // free-list blocks carry STALE data from their prior use; the guest's allocator/constructor
    // expects clean memory. Uninitialized container fields are how the emitter's count [obj+112]
    // becomes garbage near 65535 -> assert 3528. Gated on g_base being mapped.
    if (g_base && (!lswtcs_pool_nozero())) memset(g_base + addr, 0, rsize);
    return addr;
}
// returns 1 if this addr belongs to our pool (freed/handled), 0 if not ours
extern "C" int lswtcs_pool_free(uint32_t addr) {
    if (addr < LSWTCS_POOL_BASE || addr >= LSWTCS_POOL_END) return 0;
    std::lock_guard<std::mutex> lk(lswtcs_pool_mtx);
    auto it = lswtcs_pool_live.find(addr);
    if (it == lswtcs_pool_live.end()) return 1;   // ours but double/unknown free — swallow
    uint32_t rsize = it->second;
    lswtcs_pool_live.erase(it);
    lswtcs_pool_fl[rsize].push_back(addr);
    return 1;
}

// ── Free-list self-check harness (gated LSWTCS_HEAPTEST) ─────────────────────
// Deterministic, independent of the flaky T18/compiler path: reproduces the proven
// "allocator hands a temp the LIVE held block's address" bug at startup ([T]8), so
// any free-list repair can be validated without reaching the shader compile.
//   VFLAGS 0x24810000 = virtual-heap path (bit31 clear) — exactly what sub_825D3710
//   (compiler context) and sub_8254CFA0 (macro temps) use.
void lswtcs_heaptest(PPCContext& ctx, uint8_t* base) {
    if (!getenv("LSWTCS_HEAPTEST")) return;
    static int done = 0; if (done) return; done = 1;
    const uint32_t VFLAGS = 0x24810000u;
    auto allocb = [&](uint32_t size) -> uint32_t {
        PPCContext c = ctx; c.r3.u32 = size; c.r4.u32 = VFLAGS; c.fpscr.csr = 0x1F80u;
        sub_82533258(c, base); return c.r3.u32;
    };
    auto freeb = [&](uint32_t addr) {
        PPCContext c = ctx; c.r3.u32 = addr; c.r4.u32 = VFLAGS; c.fpscr.csr = 0x1F80u;
        sub_825332F0(c, base);
    };
    // Pattern A — the exact bug shape: HOLD one block, churn alloc+free temps,
    // check if any temp is handed the held block's address while it's still live.
    uint32_t held = allocb(24);
    dbg_ram("[HEAPTEST] held block = 0x%08X; churning 300 temps...\n", held);
    int dupA = 0, nA = 0;
    for (int i = 0; i < 300; i++) {
        uint32_t t = allocb(20);
        if (!t) { dbg_ram("[HEAPTEST] A: temp #%d NULL\n", i); break; }
        nA++;
        if (t == held) { if (++dupA <= 5) dbg_ram("[HEAPTEST] *** A: temp #%d 0x%08X ALIASES live held block ***\n", i, t); }
        freeb(t);
    }
    dbg_ram("[HEAPTEST] PatternA: %d temps, %d aliased held block (0=healthy)\n", nA, dupA);
    freeb(held);
    // Pattern B — pure uniqueness: alloc many, HOLD all, assert distinct (tests that
    // alloc removes the block from the free-list at all).
    static uint32_t set_[512]; int n = 0, dupB = 0;
    for (int i = 0; i < 400; i++) {
        uint32_t a = allocb(20);
        if (!a) break;
        for (int j = 0; j < n; j++) if (set_[j] == a) { if (++dupB <= 5) dbg_ram("[HEAPTEST] B: alloc#%d 0x%08X dup of slot %d\n", i, a, j); break; }
        if (n < 512) set_[n++] = a;
    }
    dbg_ram("[HEAPTEST] PatternB: %d held allocs, %d duplicates (0=healthy)\n", n, dupB);
    // Inspect the free-list node header around the duplicated block to see WHY it never
    // advances (self-referential flink/blink would mean the unlink is a no-op).
    if (dupB > 0 && n > 0) {
        uint32_t blk = set_[0];
        auto rd = [&](uint32_t a) -> uint32_t { return __builtin_bswap32(*reinterpret_cast<volatile uint32_t*>(base + a)); };
        dbg_ram("[HEAPTEST] dup block 0x%08X hdr: [-8]=%08X [-4]=%08X [+0]=%08X [+4]=%08X [+8]=%08X [+12]=%08X [+16]=%08X\n",
            blk, rd(blk-8), rd(blk-4), rd(blk+0), rd(blk+4), rd(blk+8), rd(blk+12), rd(blk+16));
        // The unlink reads node.flink at [node+12], node.blink at [node+8]; node = block-8 (the LIST_ENTRY).
        // Consistency check it makes: flink.blink == node+8 AND blink.flink == node+8. Verify both.
        uint32_t node = blk - 8;
        uint32_t flink = rd(node + 12), blink = rd(node + 8);
        dbg_ram("[HEAPTEST] node=0x%08X flink=0x%08X blink=0x%08X | flink.blink[flink+4]=0x%08X blink.flink[blink+0]=0x%08X expect node+8=0x%08X\n",
            node, flink, blink, rd(flink + 4), rd(blink + 0), node + 8);
    }
    for (int i = 0; i < n; i++) freeb(set_[i]);
    dbg_ram("[HEAPTEST] done.\n");
}

static void ppc_noop_stub(PPCContext& ctx, uint8_t* base) { (void)ctx; (void)base; }

uint8_t* g_base = nullptr;

static uint32_t g_call_log[32];
static int g_call_log_idx = 0;

void ppc_log_call(uint32_t addr) {
    g_call_log[g_call_log_idx & 31] = addr;
    g_call_log_idx++;
}


extern "C" void dbg_ram_dump();   // flush the RAM trace (kernel_stubs.cpp) on crash

static LONG WINAPI crash_handler(EXCEPTION_POINTERS* ep) {
    EXCEPTION_RECORD* er = ep->ExceptionRecord;
    CONTEXT* ctx = ep->ContextRecord;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION &&
        er->ExceptionCode != EXCEPTION_ILLEGAL_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;
    dbg_ram_dump();   // flush RAM trace BEFORE the process dies, so we keep the lead-up
    FILE* cf = fopen("C:/Temp/lswtcs_crash.txt", "w");
    auto clog = [&](const char* fmt, ...) {
        va_list ap; va_start(ap, fmt);
        vfprintf(stderr, fmt, ap); va_end(ap);
        if (cf) { va_list ap2; va_start(ap2, fmt); vfprintf(cf, fmt, ap2); va_end(ap2); }
    };
    clog("\n=== CRASH: code=0x%08lX addr=0x%p ===\n",
         er->ExceptionCode, er->ExceptionAddress);
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        clog("  AV: %s addr 0x%p\n",
             er->ExceptionInformation[0] ? "WRITE" : "READ",
             (void*)er->ExceptionInformation[1]);
    }
    uintptr_t img_base = (uintptr_t)GetModuleHandle(NULL);
    uintptr_t rva = ctx->Rip - img_base;
    clog("  RIP=0x%016llX RSP=0x%016llX\n",
         (unsigned long long)ctx->Rip, (unsigned long long)ctx->Rsp);
    clog("  RVA=0x%08llX  addr2line: 0x%09llX\n",
         (unsigned long long)rva, (unsigned long long)(0x140000000ULL + rva));
    if (g_base && er->NumberParameters >= 2) {
        uintptr_t fa = (uintptr_t)er->ExceptionInformation[1];
        uintptr_t base = (uintptr_t)g_base;
        if (fa >= base && fa < base + 0x100000000ULL)
            clog("  Guest PPC addr: 0x%08llX\n", (unsigned long long)(fa - base));
    }
    fflush(stderr);
    if (cf) { fflush(cf); fclose(cf); }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void signal_handler(int sig) {
    printf("\n=== SIGNAL %d ===\n", sig);
    fflush(stdout);
    exit(1);
}

// ── In-process hardware watchpoint on the fs-device vtable slot (0x82F211A0) ──
// Catches the wild WRITE that corrupts it (0x8213DBD0 -> garbage like 0x1927),
// the root of the intermittent crash/non-determinism. Gated by LSWTCS_WATCH.
// Resolve the logged RVA against LSWTCSRuntime.map to name the guest function.
extern "C" void dbg_ram(const char* fmt, ...);   // RAM trace (kernel_stubs.cpp)
static volatile uint32_t g_watch_addr = 0x82F211A0u;
static bool g_pristine_data = false;   // LSWTCS_PRISTINE overlaid .data successfully   // guest addr being watched
static volatile bool     g_watch_registered = false;
// Reuse-robust mode: when the watched word becomes this value, log the writer and DISARM
// (so heap reuse of the same address can't add noise). 0x98DB0382 = host-LE of guest 0x8203DB98
// (the "HLSL_VERSION" static literal). Set non-zero to enable disarm-on-hit.
static volatile uint32_t g_watch_target = 0x98DB0382u;
extern "C" volatile uint32_t g_dbg_ctx_addr = 0;   // live compiler-context addr (alloc-aliasing check)

// LSWTCS shader-compile abort (see ppc_context.h). longjmp out of the compiler's assert handler
// (sub_82692AF8) when a compile is active, so an assert aborts the shader compile instead of
// falling off a function and corrupting callee-saved registers (which deadlocks the emitter).
extern "C" { jmp_buf g_compile_jmp; }   // definition (C linkage to match ppc_context.h decl)
extern "C" volatile int g_compile_active = 0;
// One-shot guest-memory dump to a file (robust vs RAM-trace wrap). Fires once per tag.
extern "C" void lswtcs_dump_mem(uint32_t guest_addr, uint32_t len, const char* tag) {
    static const char* seen[8] = {0}; static int nseen = 0;
    for (int i = 0; i < nseen; i++) if (seen[i] == tag) return;
    if (nseen < 8) seen[nseen++] = tag;
    if (!g_base || guest_addr < 0x82000000u || guest_addr >= 0xA0000000u) return;
    uint32_t n = len; if (n > 65536u) n = 65536u;
    char path[256]; snprintf(path, sizeof(path), LSW_ROOT_DIR "/LSWTCSRuntime/build/dump_%s.bin", tag);
    FILE* f = fopen(path, "wb");
    if (f) { fwrite((const void*)(g_base + guest_addr), 1, n, f); fclose(f); dbg_ram("[DUMPMEM] wrote %u bytes of 0x%08X to %s\n", n, guest_addr, path); }
}

// ── D3D semantic-name table repair ────────────────────────────────────────────────────────────────
// The HLSL semantic-binding table at guest 0x82F317E0 (20 entries x 12B: {name_ptr, f4, f8}) has its
// f4 field LAZILY COMPUTED per-semantic on first use (f4 = nameLen<<28 | class-bits, used by the input
// validator sub_8276C528 to (a) length-match the semantic name and (b) bounds-check the trailing index).
// Our Xenia memory-dump image captured this cache only PARTIALLY populated (3 of 20 entries: BINORMAL,
// SV_Depth, TEXCOORD); the rest are 0. A zeroed f4 means a length-0 strncmp false-matches BLENDINDICES
// (idx1) first, then the index/bounds check fails -> assert 3524 ("POSITION"), and the shader compile
// E_FAILs -> the game never sets up GPU state -> black screen. The values are deterministic engine data;
// we restore all 20 from Xenia ground truth (host=guest+0x110000000) so every semantic validates.
// Portable (CPU-side game data; recompiles to any target). One-shot. Idempotent.
extern "C" void lswtcs_fix_semtable() {
    static int done = 0; if (done || !g_base) return;
    const uint32_t TBL = 0x82F317E0u;            // base of the 20-entry semantic table
    // f4 values in semantic-table index order (idx 0..19), big-endian guest words:
    static const uint32_t f4[20] = {
        0x83C00000u, // 0  BINORMAL    (8)
        0xC1400000u, // 1  BLENDINDICES(12)
        0xB0C00000u, // 2  BLENDWEIGHT (11)
        0x55400000u, // 3  COLOR       (5)
        0x95400000u, // 4  SV_Target   (9)
        0x56400000u, // 5  DEPTH       (5)
        0x86400000u, // 6  SV_Depth    (8)
        0x35C00000u, // 7  FOG         (3)
        0x61C00000u, // 8  NORMAL      (6)
        0x80400000u, // 9  POSITION    (8)
        0xB0400000u, // 10 SV_Position (11)
        0x52400000u, // 11 PSIZE       (5)
        0x66C00000u, // 12 SAMPLE      (6)
        0x73400000u, // 13 TANGENT     (7)
        0xA4400000u, // 14 TESSFACTOR  (10)
        0x82C00000u, // 15 TEXCOORD    (8)
        0x94C00000u, // 16 POSITIONT   (9)
        0x58040000u, // 17 INDEX       (5)
        0xB7040000u, // 18 BARYCENTRIC (11)
        0x67840000u, // 19 QUADID      (6)
    };
    int patched = 0;
    for (int i = 0; i < 20; i++) {
        uint32_t ea = TBL + (uint32_t)i * 12u + 4u;          // f4 field of entry i
        uint32_t* p = (uint32_t*)(g_base + ea);
        uint32_t cur = __builtin_bswap32(*p);                // guest is big-endian
        if (cur != f4[i]) { *p = __builtin_bswap32(f4[i]); patched++; }
    }
    // The input-binding validator (sub_8276C528 / 469.cpp loc_8272940C) uses two more {name_ptr, flags}
    // tables (8-byte stride, flags @ +4) at 0x82F24418 (19 entries) and 0x82F244B0 (3 entries). Same
    // lazy-cache zeroing in our dump. flags top-nibble = name length; rest = register class/index range.
    // Ground truth from Xenia (host=guest+0x110000000):
    static const uint32_t fl19[19] = {
        0x80037000u,0xC0012000u,0xB0011000u,0x5004A000u,0x9004A080u,0x5001C000u,0x3001B000u,
        0x60033000u,0x8F8B0000u,0xBF8B0080u,0x80030000u,0x6001D000u,0x70036000u,0xA0018000u,
        0x80025000u,0x5FC90100u,0x8FC90280u,0xAFC90480u,0x50014000u,
    };
    static const uint32_t fl3[3] = { 0x50240000u, 0x5F490000u, 0x90240080u };
    for (int i = 0; i < 19; i++) {
        uint32_t* p = (uint32_t*)(g_base + 0x82F24418u + (uint32_t)i * 8u + 4u);
        if (__builtin_bswap32(*p) != fl19[i]) { *p = __builtin_bswap32(fl19[i]); patched++; }
    }
    for (int i = 0; i < 3; i++) {
        uint32_t* p = (uint32_t*)(g_base + 0x82F244B0u + (uint32_t)i * 8u + 4u);
        if (__builtin_bswap32(*p) != fl3[i]) { *p = __builtin_bswap32(fl3[i]); patched++; }
    }
    // The 3543 assert (sub_8270B930, 468.cpp) searches an 8-slot 16-bit "semantic compatibility" group at
    // 0x82F24404 for (idx,type) of each shader input element. Our image had only slots 2,3 populated
    // (idx16/t3, idx16/t4) — slots 0,1,4-7 ZEROED (same lazy-cache gap as the tables above). An input
    // asking for idx16/type1 (a legit request, present in Xenia slot0) found no match -> assert 3543.
    // Ground truth from live Xenia (0x82F24404, 16-bit BE): idx16 t1..t4, idx14 t1..t3, idx15 t1.
    static const uint16_t cmp8[8] = { 0x81E2u,0x8204u,0x8308u,0x8410u,0x7124u,0x7290u,0x7348u,0x79E0u };
    for (int i = 0; i < 8; i++) {
        uint16_t* p = (uint16_t*)(g_base + 0x82F24404u + (uint32_t)i * 2u);
        uint16_t want = __builtin_bswap16(cmp8[i]);
        if (*p != want) { *p = want; patched++; }
    }
    done = 1;
    dbg_ram("[SEMTBL] repaired %d fields (master20 + t19 + t3 + cmp8)\n", patched);
}
// EXPERIMENTAL (LSWTCS_SEMFIX=1): the runtime shader-source generator emits VertexInput attributes with
// EMPTY semantics ("float4 position: ;") -> D3DX X3000 syntax error -> compile E_FAILs. The semantics are
// supposed to be bound from the vertex declaration (a subsystem not yet wired up). As a test of the rest
// of the pipeline, inject the standard NU2->D3D semantic for each input attribute by NAME, just before the
// parser. Returns a new guest source buffer (or 0). Correct for standard attrs; lets the shader compile.
static const char* lswtcs_sem_for(const char* nm, int nl, int* tc) {
    static char buf[20];
    auto eq = [&](const char* k){ int kl=0; while(k[kl]) kl++; return kl==nl && memcmp(nm,k,nl)==0; };
    auto pre = [&](const char* k){ int kl=0; while(k[kl]) kl++; return nl>kl && memcmp(nm,k,kl)==0; };
    if (eq("position"))   return "POSITION";
    if (eq("normal"))     return "NORMAL";
    if (eq("tangent"))    return "TANGENT";
    if (eq("binormal"))   return "BINORMAL";
    if (pre("colorSet"))  { snprintf(buf,sizeof(buf),"COLOR%c", nm[8]); return buf; }
    if (pre("uvSet"))     { snprintf(buf,sizeof(buf),"TEXCOORD%c", nm[5]); return buf; }
    if (pre("blendWeight"))  return "BLENDWEIGHT";
    if (pre("blendIndices")) return "BLENDINDICES";
    snprintf(buf,sizeof(buf),"TEXCOORD%d", 8 + ((*tc)++)); return buf;   // fallback, high indices
}
extern "C" uint32_t lswtcs_fix_semantics(uint32_t src, uint32_t len, uint32_t* outlen) {
    static int en = -1;
    if (en < 0) { const char* e = getenv("LSWTCS_SEMFIX"); en = (e && e[0] != '0') ? 1 : 0; }
    if (!en || !g_base || src < 0x82000000u || src >= 0xF0000000u || len == 0 || len > 0x40000u) return 0;
    const char* s = (const char*)(g_base + src);
    uint32_t dst = lswtcs_pool_alloc(len + 1024);
    if (!dst) return 0;
    char* d = (char*)(g_base + dst);
    uint32_t di = 0; int inVI = 0, tc = 0;
    for (uint32_t i = 0; i < len && s[i]; ) {
        if (!inVI && i + 18 <= len && memcmp(s + i, "struct VertexInput", 18) == 0) inVI = 1;
        else if (inVI && s[i] == '}') inVI = 0;
        if (s[i] == ':') {
            uint32_t j = i + 1; while (j < len && (s[j] == ' ' || s[j] == '\t')) j++;
            if (j < len && s[j] == ';') {
                // empty NU2 annotation before ';'. In VertexInput: it's a required input SEMANTIC -> fill
                // it (name-based). Elsewhere (uniform/sampler): D3D doesn't need it -> drop the ": ".
                if (inVI) {
                    int nsx = (int)i;
                    while (nsx > 0) { char c = s[nsx-1]; if ((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_') nsx--; else break; }
                    const char* sem = lswtcs_sem_for(s + nsx, (int)i - nsx, &tc);
                    d[di++] = ':'; d[di++] = ' '; while (*sem) d[di++] = *sem++;
                }
                i = j; continue;   // land on ';' (emitted next iteration)
            } else if (j < len && s[j] == ':') {
                // empty annotation before a real binding ("view: : register(c12)") -> drop the empty ": "
                i = j; continue;   // land on the second ':'
            }
        }
        d[di++] = s[i++];
    }
    d[di] = 0;
    if (outlen) *outlen = di;
    { static int _once = 0; if (!_once) { _once = 1; FILE* fp = fopen("semfix_out.txt", "wb"); if (fp) { fwrite(d, 1, di, fp); fclose(fp); } } }
    dbg_ram("[SEMFIX] rewrote src 0x%08X len 0x%X -> 0x%08X len 0x%X\n", src, len, dst, di);
    return dst;
}
// EXPERIMENTAL (LSWTCS_STUBSRC=1): replace the source fed to the game's OWN compiler with a
// trivial valid NU2 shader, so its compiler succeeds (no emitter assert) and fills the shader
// object → the game proceeds + sets up GPU state. We compile the REAL HLSL to D3D12 separately
// (Path B) and bind that at the draw. Returns guest addr of the stub source (0 if disabled).
extern "C" uint32_t lswtcs_stub_source(uint32_t* outlen) {
    static int en = -1;
    if (en < 0) { const char* e = getenv("LSWTCS_STUBSRC"); en = (e && e[0] != '0') ? 1 : 0; }
    if (!en) return 0;
    static uint32_t cached = 0, clen = 0;
    if (cached) { if (outlen) *outlen = clen; return cached; }
    static const char* hlsl =
        "#define RENDER_TARGET_COUNT 1\n"
        "struct VertexInput  { float4 position: POSITION; };\n"
        "struct VertexOutput { float4 _varying_position: POSITION; };\n"
        "struct FragmentOutput { float4 color[RENDER_TARGET_COUNT] : COLOR0; };\n"
        "VertexInput vin; VertexOutput vout; FragmentOutput fout;\n"
        "VertexOutput vertexProgram() { vout._varying_position = vin.position; return vout; }\n"
        "FragmentOutput fragmentProgram() { fout.color[0] = float4(1,0,0,1); return fout; }\n"
        "technique TECHNIQUE_BUILTSHADER { pass p0 {\n"
        "  VertexShader = compile vs_3_0 vertexProgram();\n"
        "  PixelShader  = compile ps_3_0 fragmentProgram();\n"
        "} }\n";
    uint32_t len = (uint32_t)strlen(hlsl);
    uint32_t a = lswtcs_pool_alloc(len + 16);
    if (!a || !g_base) return 0;
    memcpy(g_base + a, hlsl, len + 1);
    cached = a; clen = len;
    if (outlen) *outlen = len;
    printf("[STUBSRC] trivial source at 0x%08X len %u\n", a, len); fflush(stdout);
    return a;
}
// Log compile result to stdout (h.log) so it survives the RAM-trace wrap.
extern "C" void lswtcs_logresult(uint32_t ret, uint32_t errstr_guest) {
    const char* e = (g_base && errstr_guest >= 0x82000000u && errstr_guest < 0xF0000000u) ? (const char*)(g_base + errstr_guest) : "";
    printf("[LOGRESULT] compile ret=0x%08X err=\"%.140s\"\n", ret, e);
    fflush(stdout);
}
extern "C" void lswtcs_logcmp(uint32_t key, uint32_t ent, uint32_t flags, uint32_t res) {
    static int n = 0;
    const char* k = (g_base && key >= 0x82000000u && key < 0xF0000000u) ? (const char*)(g_base + key) : "?";
    if (n >= 40 || k[0] != 'P' || k[1] != 'O') return;   // only the POSITION key lookups
    n++;
    const char* e = (g_base && ent >= 0x82000000u && ent < 0xF0000000u) ? (const char*)(g_base + ent) : "?";
    printf("[CMP] key=\"%.10s\" ent=\"%.10s\" flags=0x%08X res=0x%08X\n", k, e, flags, res);
    fflush(stdout);
}
extern "C" void lswtcs_logassert(uint32_t key, uint32_t count, uint32_t table) {
    auto str = [&](uint32_t a) -> const char* { return (g_base && a >= 0x82000000u && a < 0xF0000000u) ? (const char*)(g_base + a) : "?"; };
    auto ld  = [&](uint32_t a) -> uint32_t { return (g_base && a >= 0x82000000u && a < 0xF0000000u) ? __builtin_bswap32(*(uint32_t*)(g_base + a)) : 0; };
    printf("[A3524] key r22=0x%08X \"%.24s\" [+0]=0x%08X [+4]=0x%08X | count=%u table=0x%08X\n",
           key, str(key), ld(key), ld(key + 4), count, table);
    for (int e = 0; e < 22 && table; e++) {
        uint32_t nm = ld(table + e * 12);
        printf("  ent[%d] name=0x%08X \"%.20s\" f4=0x%08X f8=0x%08X\n", e, nm, str(nm), ld(table+e*12+4), ld(table+e*12+8));
    }
    fflush(stdout);
}
extern "C" int lswtcs_skip4801() {
    static int v = -1;
    if (v < 0) { const char* e = getenv("LSWTCS_SKIP4801"); v = (e && e[0] == '0') ? 0 : 1; }
    return v;
}
extern "C" void lswtcs_compile_abort(uint32_t code, uint32_t caller) {
    if (g_compile_active) {
        g_compile_active = 0;
        dbg_ram("[COMPILE] assert code=%u caller=0x%08X -> abort compile via longjmp\n", code, caller);
        printf("[COMPILE-ABORT] assert code=%u caller=0x%08X\n", code, caller); fflush(stdout);
        longjmp(g_compile_jmp, 1);
    }
}

static LONG CALLBACK watch_handler(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP) {
        uintptr_t rip  = ep->ContextRecord->Rip;
        uintptr_t imgb = (uintptr_t)GetModuleHandle(NULL);
        uint32_t  wa   = g_watch_addr;
        uint32_t  val  = g_base ? *(volatile uint32_t*)(g_base + wa) : 0;
        // LSWTCS_WATCHNAN=1: only log writes that leave a NaN float at the watched address
        // (guest big-endian → swap). Used to find who first poisons the camera matrix.
        static int nanmode = -1; if (nanmode < 0) nanmode = getenv("LSWTCS_WATCHNAN") ? 1 : 0;
        if (nanmode) {
            uint32_t g = __builtin_bswap32(val);
            bool isnan = ((g & 0x7F800000u) == 0x7F800000u) && (g & 0x007FFFFFu);
            static int nn = 0, all = 0;
            ++all;
            if (isnan && nn++ < 25)
                dbg_ram("[WATCHNAN] NaN 0x%08X written @0x%08X by RVA=0x%08llX tid=%lu (write #%d)\n",
                        g, wa, (unsigned long long)(rip - imgb), (unsigned long)GetCurrentThreadId(), all);
            else if (!isnan && all <= 5)
                dbg_ram("[WATCHNAN] ok 0x%08X written @0x%08X by RVA=0x%08llX (write #%d)\n",
                        g, wa, (unsigned long long)(rip - imgb), all);
            ep->ContextRecord->Dr6 = 0;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        static int n = 0;
        if (n++ < 40)
            dbg_ram("[WATCH] write @0x%08X  RVA=0x%08llX  newval=0x%08X  tid=%lu\n",
                    wa, (unsigned long long)(rip - imgb), val, (unsigned long)GetCurrentThreadId());
        // disarm on ANY 0x8203DBxx macro literal: guest 0x8203DBxx → host-LE val with
        // (val & 0x00FFFFFF)==0xDB0382. Robust to which macro name lands last.
        if ((val & 0x00FFFFFFu) == 0x00DB0382u) {
            dbg_ram("[WATCHHIT] @0x%08X set to literal 0x%08X by RVA=0x%08llX tid=%lu — DISARMING\n",
                    wa, val, (unsigned long long)(rip - imgb), (unsigned long)GetCurrentThreadId());
            ep->ContextRecord->Dr7 = 0;           // disarm: stop catching reuse writes
            ep->ContextRecord->Dr6 = 0;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        ep->ContextRecord->Dr6 = 0;               // clear status, keep DR7 armed
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

extern "C" void lswtcs_set_vtable_watch() {
    if (!getenv("LSWTCS_WATCH") || !g_base) return;
    // LSWTCS_WATCHADDR=0x... overrides the watched guest address (default: fs-device vtable).
    static uint32_t addr = 0;
    if (!addr) {
        const char* e = getenv("LSWTCS_WATCHADDR");
        addr = e ? (uint32_t)strtoul(e, nullptr, 0) : 0x82F211A0u;
        g_watch_addr = addr;
    }
    CONTEXT c{}; c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    c.Dr0 = (DWORD64)(uintptr_t)(g_base + addr);
    c.Dr7 = 0xD0001;  // L0 enable | R/W0=01(write) | LEN0=11(4 bytes)
    SetThreadContext(GetCurrentThread(), &c);
}

// General write-watch on an arbitrary guest address (gated by LSWTCS_CTXWATCH).
// Arms DR0 on the CALLING thread → use from the recomp where the watched object is
// created so the watch lands on the thread that will write it. Logs each writer's RVA.
extern "C" void lswtcs_watch_guest(uint32_t guest_addr) {
    if (!getenv("LSWTCS_CTXWATCH") || !g_base) return;
    g_watch_addr = guest_addr;
    if (!g_watch_registered) { g_watch_registered = true; AddVectoredExceptionHandler(1, watch_handler); }
    CONTEXT c{}; c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    c.Dr0 = (DWORD64)(uintptr_t)(g_base + guest_addr);
    c.Dr7 = 0xD0001;
    SetThreadContext(GetCurrentThread(), &c);
}

// ── LSWTCS memory "BUBBLE": page-guard software watchpoint over a guest ADDRESS RANGE ──
// Catches EVERY read and/or write that lands in the range — with the host RVA (→ guest fn via the
// .map / nm) and the accessed value — regardless of which exact address (so it's immune to the
// node-address layout lottery, unlike the 50µs poll WATCHADDR). Mechanism: mark the host pages
// backing the range PAGE_READONLY (write-catch) or PAGE_NOACCESS (read+write catch); on the access
// fault, record addr+RIP, un-protect + set the single-step flag, let the instruction run, then on
// the single-step trap re-protect and (for writes) read back the value just stored. NOTHREADS = one
// guest thread so the un-protect/step/re-protect dance has no cross-thread race.
//   Env: LSWTCS_BUBBLE="0xLO-0xHI[:rwv]"  e.g. "0xA026C000-0xA026E000:wv"  (node pages, writes, phantom-only)
//        r = catch reads, w = catch writes (default if neither), v = only log WRITES whose stored value
//            is a phantom parent (0xA0037F00..0xA0038200 or 0xA0000000) — the bug we are chasing.
//   Also: LSWTCS_BUBBLE_MAX=N caps log lines (default 200).
static uint8_t* g_bub_plo = nullptr; static size_t g_bub_psz = 0;
static uint32_t g_bub_glo = 0, g_bub_ghi = 0;
static int g_bub_rd = 0, g_bub_wr = 0, g_bub_vfilter = 0;
static DWORD g_bub_catch_prot = PAGE_READONLY;     // protection that traps the access we want
static int g_bub_max = 200; static int g_bub_n = 0;
static thread_local uintptr_t g_bub_pend_fa = 0;   // faulting host addr awaiting single-step
static thread_local uintptr_t g_bub_pend_rip = 0;
static thread_local int g_bub_pend_wr = 0, g_bub_pend_log = 0;
static inline int lswtcs_is_phantom(uint32_t v){ return (v>=0xA0037F00u && v<0xA0038200u) || v==0xA0000000u; }
static void lswtcs_bub_protect(DWORD prot){ DWORD o; if (g_bub_plo) VirtualProtect(g_bub_plo, g_bub_psz, prot, &o); }

static LONG CALLBACK bubble_handler(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2 && g_bub_plo) {
        int isWrite     = (int)ep->ExceptionRecord->ExceptionInformation[0] == 1;
        uintptr_t fa    = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
        if (fa >= (uintptr_t)g_bub_plo && fa < (uintptr_t)g_bub_plo + g_bub_psz) {
            uint32_t gaddr = (uint32_t)(fa - (uintptr_t)g_base);
            int inRange = (gaddr >= g_bub_glo && gaddr < g_bub_ghi);
            int want    = isWrite ? g_bub_wr : g_bub_rd;
            // MUST let the faulting instruction run: drop protection + arm single-step. We always do
            // this (even for accesses we don't log) so the guest makes progress; logging is filtered.
            lswtcs_bub_protect(PAGE_READWRITE);
            ep->ContextRecord->EFlags |= 0x100;   // TF: trap after the next instruction
            g_bub_pend_fa  = fa;
            g_bub_pend_rip = ep->ContextRecord->Rip;
            g_bub_pend_wr  = isWrite;
            g_bub_pend_log = (want && inRange);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (code == EXCEPTION_SINGLE_STEP && g_bub_pend_fa) {
        uintptr_t fa = g_bub_pend_fa; g_bub_pend_fa = 0;
        uint32_t gaddr = (uint32_t)(fa - (uintptr_t)g_base);
        uint32_t val   = __builtin_bswap32(*(volatile uint32_t*)(g_base + (gaddr & ~3u)));  // guest is BE
        int phantom = lswtcs_is_phantom(val);
        if (g_bub_pend_log && g_bub_n < g_bub_max &&
            (!g_bub_vfilter || (g_bub_pend_wr && phantom))) {
            g_bub_n++;
            uintptr_t imgb = (uintptr_t)GetModuleHandle(NULL);
            dbg_ram("[BUBBLE] %s g=0x%08X val=0x%08X RVA=0x%08llX%s\n",
                    g_bub_pend_wr ? "WR" : "RD", gaddr & ~3u, val,
                    (unsigned long long)(g_bub_pend_rip - imgb), phantom ? "  <PHANTOM>" : "");
        }
        lswtcs_bub_protect(g_bub_catch_prot);     // re-arm the trap
        ep->ContextRecord->EFlags &= ~0x100u;     // clear TF
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

extern "C" void lswtcs_bubble_init() {
    const char* s = getenv("LSWTCS_BUBBLE");
    if (!s || !g_base) return;
    // parse "0xLO-0xHI[:flags]"
    char* end = nullptr;
    unsigned long lo = strtoul(s, &end, 0);
    if (!end || *end != '-') { printf("[BUBBLE] bad spec '%s' (want 0xLO-0xHI[:rwv])\n", s); return; }
    unsigned long hi = strtoul(end + 1, &end, 0);
    const char* fl = (end && *end == ':') ? end + 1 : "";
    for (const char* p = fl; *p; p++) { if (*p=='r') g_bub_rd=1; else if (*p=='w') g_bub_wr=1; else if (*p=='v') g_bub_vfilter=1; }
    if (!g_bub_rd && !g_bub_wr) g_bub_wr = 1;                 // default: catch writes
    if (const char* m = getenv("LSWTCS_BUBBLE_MAX")) { int v = atoi(m); if (v>0) g_bub_max = v; }
    g_bub_glo = (uint32_t)lo; g_bub_ghi = (uint32_t)hi;
    // page-align the host range
    uintptr_t plo = ((uintptr_t)g_base + lo) & ~(uintptr_t)0xFFFu;
    uintptr_t phi = (((uintptr_t)g_base + hi) + 0xFFFu) & ~(uintptr_t)0xFFFu;
    g_bub_plo = (uint8_t*)plo; g_bub_psz = (size_t)(phi - plo);
    g_bub_catch_prot = g_bub_rd ? PAGE_NOACCESS : PAGE_READONLY;  // NOACCESS catches reads+writes
    AddVectoredExceptionHandler(1, bubble_handler);              // FIRST: runs before crash_handler
    lswtcs_bub_protect(g_bub_catch_prot);
    printf("[BUBBLE] armed guest [0x%08X,0x%08X) %zu pages  catch=%s%s%s  prot=%s  max=%d\n",
           g_bub_glo, g_bub_ghi, g_bub_psz/0x1000,
           g_bub_rd?"R":"", g_bub_wr?"W":"", g_bub_vfilter?"+phantom":"",
           g_bub_rd?"NOACCESS":"READONLY", g_bub_max);
    fflush(stdout);
}

int main() {
    setbuf(stdout, nullptr);
    AddVectoredExceptionHandler(1, crash_handler);
    //signal(SIGSEGV, signal_handler);  // disabled: let GDB catch raw crash
    //signal(SIGILL,  signal_handler);
    { void* _rsp; asm("mov %%rsp, %0":"=r"(_rsp)); printf("[MAIN] rsp=%p\n",_rsp); }

    // Allocate full 4GB guest address space: any valid 32-bit PPC pointer is safe to dereference.
    // Corrupted Xenia pointers that land outside the image simply read zero instead of crashing.
    // 4 GB guest space + the dispatch table, which lives just above 4 GB
    // (PPC_FUNC_TABLE_BASE = 0x100000000) so it no longer sits inside the 0xC0000000
    // physical view.
    size_t alloc_size = (size_t)(PPC_FUNC_TABLE_BASE + 0x4000000ULL);
    g_base = nullptr;
    // ── Physical-memory aliasing (Xbox 360 / Xenia memory model) ──────────────────
    // The game addresses physical page P through several views and converts between them
    // with phys = va & 0x1FFFFFFF: GPU command buffers allocated at 0xA0xxxxxx are handed
    // to the GPU as P, and the D3D deferred-chain worker (sub_822D2798) dereferences
    // 0xC0000000+P. Our physical allocator returns 0x88..0xA0+ VAs and the GPU side reads P
    // at 0x80000000+P. So 0x80000000, 0xA0000000 and 0xC0000000 must be the SAME 512 MB:
    // one pagefile section mapped three times. Everything else is private memory.
    // LSWTCS_PHYSALIAS=0 falls back to one flat allocation (views not aliased).
    {
        const char* pa = getenv("LSWTCS_PHYSALIAS");
        bool want_alias = !(pa && pa[0] == '0');
        for (int attempt = 0; want_alias && attempt < 8 && !g_base; ++attempt) {
            uint8_t* b = (uint8_t*)VirtualAlloc(nullptr, alloc_size, MEM_RESERVE, PAGE_NOACCESS);
            if (!b) break;
            VirtualFree(b, 0, MEM_RELEASE);   // found a free range; now rebuild it piecewise
            HANDLE sec = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT,
                                            0, 0x20000000u, nullptr);
            void* lo  = sec ? VirtualAlloc(b, 0x80000000ULL, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) : nullptr;
            void* v80 = lo  ? MapViewOfFileEx(sec, FILE_MAP_ALL_ACCESS, 0, 0, 0x20000000u, b + 0x80000000ULL) : nullptr;
            void* vA0 = v80 ? MapViewOfFileEx(sec, FILE_MAP_ALL_ACCESS, 0, 0, 0x20000000u, b + 0xA0000000ULL) : nullptr;
            void* vC0 = vA0 ? MapViewOfFileEx(sec, FILE_MAP_ALL_ACCESS, 0, 0, 0x20000000u, b + 0xC0000000ULL) : nullptr;
            void* hi  = vC0 ? VirtualAlloc(b + 0xE0000000ULL, alloc_size - 0xE0000000ULL,
                                           MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) : nullptr;
            if (lo == b && v80 && vA0 && vC0 && hi) {
                g_base = b;
                printf("[INIT] Guest memory (phys-aliased): %p  phys 512MB @0x80/0xA0/0xC0000000, table @0x%llX\n",
                       (void*)g_base, (unsigned long long)PPC_FUNC_TABLE_BASE);
            } else {
                printf("[INIT] phys-alias attempt %d failed (err %lu), retrying\n", attempt, GetLastError());
                if (hi)  VirtualFree(hi, 0, MEM_RELEASE);
                if (vC0) UnmapViewOfFile(vC0);
                if (vA0) UnmapViewOfFile(vA0);
                if (v80) UnmapViewOfFile(v80);
                if (lo)  VirtualFree(lo, 0, MEM_RELEASE);
            }
            if (sec) CloseHandle(sec);   // the views keep the section alive
        }
    }
    if (!g_base)
        g_base = (uint8_t*)VirtualAlloc(nullptr, alloc_size,
                                        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_base) {
        printf("VirtualAlloc failed (4GB): %lu\n", GetLastError());
        // Fall back to original smaller allocation
        alloc_size = (size_t)PPC_IMAGE_BASE + 0x12000000u;
        g_base = (uint8_t*)VirtualAlloc(nullptr, alloc_size,
                                         MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!g_base) {
            printf("VirtualAlloc fallback failed: %lu\n", GetLastError());
            return 1;
        }
        printf("[INIT] Guest memory (fallback): %p, size 0x%zX\n", g_base, alloc_size);
    } else {
        printf("[INIT] Guest memory (4GB): %p, size 0x%zX\n", g_base, alloc_size);
    }

    // [DEBUG] Pause before guest code so a debugger can attach and set a hardware
    // write-breakpoint on the heap free-list sentinel (g_base+0x84000180) to catch
    // whatever writes a code-range value (corruption root-cause hunt). Set LSWTCS_DBGWAIT=1.
    if (getenv("LSWTCS_DBGWAIT")) {
        printf("[DBGWAIT] g_base=%p  sentinel host addr=%p  -- pausing 30s for cdb attach\n",
               (void*)g_base, (void*)(g_base + 0x84000180u)); fflush(stdout);
        Sleep(30000);
        printf("[DBGWAIT] resuming\n"); fflush(stdout);
    }

    // Load image.bin
    FILE* f = fopen(LSW_ROOT_DIR "/Convert 360/LSWTCS/image.bin", "rb");
    if (!f) {
        printf("Failed to open image.bin\n");
        return 1;
    }
    size_t nread = fread(g_base + PPC_IMAGE_BASE, 1, PPC_IMAGE_SIZE, f);
    fclose(f);
    printf("[INIT] Loaded image.bin: 0x%zX bytes\n", nread);

    // image.bin is a RAM dump of a RUNNING Xenia instance. Diffed against the pristine
    // image decrypted from Default.xex (XenonUtils Image::ParseImage -> pristine.bin):
    // .text/.rdata/.pdata/.reloc/embedded code are byte-identical, .xidata/.idata differ
    // only by loader-resolved import thunks, but .data differs in ~144.6K words (~565KB)
    // of baked-in runtime state. Each "stale-dump artifact" found so far (stream seq,
    // job-system ctx, NuSound object ptr) is one of those words. LSWTCS_PRISTINE=1
    // overlays .data (0x82E70000, size 0x41491C, which also spans the .bss tail) with the
    // pristine bytes, keeping code and resolved imports from the dump.
    {
        const char* e = getenv("LSWTCS_PRISTINE");
        if (e && e[0] == '1') {
            const uint32_t DATA_VA = 0x82E70000u, DATA_SZ = 0x0041491Cu;
            FILE* pf = fopen(LSW_ROOT_DIR "/Convert 360/LSWTCS/pristine.bin", "rb");
            if (!pf) {
                printf("[PRISTINE] pristine.bin not found — skipping .data overlay\n");
            } else {
                fseek(pf, (long)(DATA_VA - PPC_IMAGE_BASE), SEEK_SET);
                size_t got = fread(g_base + DATA_VA, 1, DATA_SZ, pf);
                fclose(pf);
                g_pristine_data = (got == DATA_SZ);
                printf("[PRISTINE] overlaid .data 0x%08X..0x%08X with pristine XEX bytes (0x%zX bytes)\n",
                       DATA_VA, DATA_VA + DATA_SZ, got);
            }
        }
    }

    // Arm the fs-device vtable watchpoint AFTER image load (so the legit static
    // value 0x8213DBD0 is already in place and only corruption writes trip it).
    if (getenv("LSWTCS_WATCH")) {
        AddVectoredExceptionHandler(1, watch_handler);
        lswtcs_set_vtable_watch();
        printf("[WATCH] armed hardware watchpoint on guest 0x82F211A0 (main thread)\n");
    }

    // LSWTCS memory-bubble page-guard watchpoint over a guest range (LSWTCS_BUBBLE="0xLO-0xHI[:rwv]").
    lswtcs_bubble_init();

    // Patch Xenia corruption at 0x832127C8 if needed
    {
        uint32_t* p = (uint32_t*)(g_base + 0x832127C8);
        if (*p == 0x58454E00) {
            printf("[PATCH] Fixing XEN marker at 0x832127C8\n");
            *p = 0;
        }
    }

    // Patch Xenia corruption in RTL_CRITICAL_SECTION at 0x82E849BC
    // The critsec LockCount/RecursionCount and list sentinel are corrupted by Xenia
    {
        // Fix critsec fields: LockCount=-1 (unlocked), RecursionCount=0
        uint32_t* lock_count      = (uint32_t*)(g_base + 0x82E849C4);
        uint32_t* recursion_count = (uint32_t*)(g_base + 0x82E849C8);
        printf("[PATCH] CRITSEC at 0x82E849C4: LockCount=0x%08X RecursionCount=0x%08X -> patching\n",
               *lock_count, *recursion_count);
        *lock_count      = 0xFFFFFFFF; // -1 = unlocked
        *recursion_count = 0x00000000;

        // Fix list sentinel at 0x82E849D8/DC: stale Xenia data points to BSS node
        // At cold start this should be an empty circular list (self-referential)
        // Use PPC_STORE_U32 macro equivalent - write as big-endian
        // PPC_STORE_U32(addr, val) writes val in big-endian to guest memory
        // We write directly: need to byteswap for little-endian host
        uint32_t sentinel_addr = 0x82E849D8;
        uint32_t be_sentinel = __builtin_bswap32(sentinel_addr);
        uint32_t* sentinel_fwd = (uint32_t*)(g_base + 0x82E849D8);
        uint32_t* sentinel_bwd = (uint32_t*)(g_base + 0x82E849DC);
        printf("[PATCH] List sentinel: raw_fwd=0x%08X raw_bwd=0x%08X -> resetting to empty\n",
               __builtin_bswap32(*sentinel_fwd), __builtin_bswap32(*sentinel_bwd));
        *sentinel_fwd = be_sentinel; // points to self = empty list (big-endian)
        *sentinel_bwd = be_sentinel;
    }

    // Zero out BSS globals that were corrupted in the Xenia dump
    // 0x8320EF54: global struct whose first word ([+0]) must be null initially
    {
        uint32_t* p = (uint32_t*)(g_base + 0x8320EF54);
        printf("[PATCH] 0x8320EF54 = 0x%08X -> 0x00000000\n", __builtin_bswap32(*p));
        memset(p, 0, 64); // zero the whole struct
    }
    // 0x82FCD190: logging verbosity flag; must be 1 to skip broken debug log path in sub_8229DDB0
    // The bctr switch dispatch in sub_82516090 (vsnprintf) corrupts r1 — skip that path
    {
        uint32_t* p = (uint32_t*)(g_base + 0x82FCD190);
        printf("[PATCH] 0x82FCD190 = 0x%08X -> 0x00000001 (disable logging path)\n", __builtin_bswap32(*p));
        *p = __builtin_bswap32(1); // big-endian 1
    }

    // Initialize pool free-list registry at 0x83210F20 as empty circular doubly-linked list.
    // sub_8250C730 (pool region registrar) uses this list. The Xenia dump left garbage
    // pointers here; the list must be self-referential (flink=blink=self) before first use.
    {
        uint32_t list_head = 0x83210F20;
        uint32_t* p = (uint32_t*)(g_base + list_head);
        printf("[PATCH] Pool registry 0x83210F20: flink=0x%08X blink=0x%08X -> init as empty list\n",
               __builtin_bswap32(p[0]), __builtin_bswap32(p[1]));
        p[0] = __builtin_bswap32(list_head);  // flink = self
        p[1] = __builtin_bswap32(list_head);  // blink = self
        p[2] = 0;                              // used-count = 0
    }

    // Patch path_struct+16 (separator char) so BuildPath produces full paths.
    // path_struct is at 0x82F1F9D8; offset 16 should be '\\' (92). The Xenia
    // dump leaves it 0, causing sub_824F7028 to replace '\\' with '\0' and
    // truncate "D:\game.dat" to "D:".
    {
        g_base[0x82F1F9E8] = '\\';
        printf("[PATCH] path_struct+16 (0x82F1F9E8) = %d -> 92 (backslash)\n",
               (int)g_base[0x82F1F9E8]);
    }

    // 0x82E7EF5C: "game initialized" flag. The main loop sub_82264AA8 only takes its
    // init path (loc_822650EC -> sub_82264240, which registers the frontend and kicks
    // off the titles level / menu-scene load) when this is 0. The Xenia dump was taken
    // from a running instance, so it ships =1 and the frontend init is skipped forever
    // (the long-standing "menu scene never requested" blocker). Zero it so the first
    // main-loop iteration performs the game's real init. LSWTCS_FRONTINIT=0 disables.
    {
        const char* e = getenv("LSWTCS_FRONTINIT");
        if (!e || e[0] != '0') {
            uint32_t* p = (uint32_t*)(g_base + 0x82E7EF5C);
            printf("[PATCH] frontend-init flag 0x82E7EF5C = 0x%08X -> 0x00000000 (main loop will run sub_82264240 init)\n",
                   __builtin_bswap32(*p));
            *p = 0;
        }
    }

    // 0x8320C72C: streaming in-flight sequence number. sub_824F6E38 spins until it
    // equals the worker-completed counter [0x83208718] before issuing a stream request
    // and releasing the worker semaphores. The streaming init sub_82506E18 zeroes the
    // completed counter but NOT this one; the Xenia dump ships 0x8B here, so the very
    // first stream request (titles-level load) waits forever. Fresh .bss would be 0.
    {
        uint32_t* p = (uint32_t*)(g_base + 0x8320C72C);
        printf("[PATCH] stream in-flight seq 0x8320C72C = 0x%08X -> 0x00000000\n",
               __builtin_bswap32(*p));
        *p = 0;
    }

    // 0x8320F038..+0x50: the job-system context (third stale-dump artifact, same class as
    // the two above). The dump ships it fully INITIALISED from the running Xenia instance:
    // [ctx+64]=1 "initialised", [ctx+0]/[ctx+4] = Xenia event handles F8000064/F8000068,
    // [ctx+8] = a Xenia heap node pool, [ctx+48] = Xenia worker-thread handle F800006C.
    // So the init sub_82508D88 (called unconditionally from the main loop at 0x82215BC4)
    // sees "already initialised" and skips creating the events, pool and the worker thread
    // sub_82508C48. Meanwhile the async-mode flag [0x82F1F940] is also 1, so the submitter
    // sub_82509160 enqueues every job into the stale pool and signals a stale handle —
    // nothing ever runs them. One such job is sub_82208570 (D680 submits it at 0x8220DCB8),
    // which registers the texture banks, binds the level, calls EB58 and arms D680's exit
    // gate: the root of the gray titles frame. A fresh Xenia boot (xenia_t96) has the whole
    // block zeroed. LSWTCS_JOBINIT=0 disables. LSWTCS_JOBSYNC=1 additionally clears the
    // async flag so jobs run synchronously on the submitting thread (A/B only).
    {
        const char* e = getenv("LSWTCS_JOBINIT");
        if (!e || e[0] != '0') {
            uint32_t* ctxp = (uint32_t*)(g_base + 0x8320F038);
            printf("[PATCH] job-system ctx 0x8320F038: init=0x%08X evA=0x%08X evB=0x%08X pool=0x%08X thread=0x%08X -> zeroed (sub_82508D88 will create the worker)\n",
                   __builtin_bswap32(ctxp[16]), __builtin_bswap32(ctxp[0]), __builtin_bswap32(ctxp[1]),
                   __builtin_bswap32(ctxp[2]), __builtin_bswap32(ctxp[12]));
            memset(ctxp, 0, 0x50);
        }
        const char* s = getenv("LSWTCS_JOBSYNC");
        if (s && s[0] == '1') {
            uint32_t* f = (uint32_t*)(g_base + 0x82F1F940);
            printf("[PATCH] job async-mode flag 0x82F1F940 = 0x%08X -> 0 (jobs run synchronously)\n",
                   __builtin_bswap32(*f));
            *f = 0;
        }
    }

    // Build dispatch table from PPCFuncMappings (null-terminated)
    // Slot address: g_base + PPC_FUNC_TABLE_BASE + (addr - PPC_CODE_BASE) * 2
    // (relocated out of guest-usable memory — see ppc_context.h PPC_FUNC_TABLE_BASE)
    {
        if (alloc_size < PPC_FUNC_TABLE_BASE + (uint64_t)PPC_CODE_SIZE * 2) {
            printf("[FATAL] guest allocation 0x%zX too small for dispatch table at 0x%llX\n",
                   alloc_size, (unsigned long long)PPC_FUNC_TABLE_BASE);
            return 1;
        }
        int count = 0;
        for (PPCFuncMapping* m = PPCFuncMappings; m->host != nullptr; m++) {
            if (m->guest >= PPC_CODE_BASE &&
                m->guest < PPC_CODE_BASE + PPC_CODE_SIZE &&
                m->host != nullptr) {
                PPCFunc** slot = &PPC_LOOKUP_FUNC(g_base, m->guest);
                *slot = m->host;
                count++;
            }
        }
        printf("[INIT] Dispatch table built: %d entries\n", count);
    }

    // LSWTCS_CMDLOG=1: wrap the six script native-command veneer ladders with
    // logging thunks (kernel_stubs.cpp) — traces every GSC native command the
    // script VM issues. Must run before TABLEGUARD write-protects the table.
    lswtcs_cmdlog_install(g_base);

    // LSWTCS_FNTRACE=hex,hex,…: wrap arbitrary guest functions' dispatch slots
    // with logging thunks (indirect calls only — see kernel_stubs.cpp).
    lswtcs_fntrace_install(g_base);


    // sub_8229DDB0 previously no-op'd: 0x82FCD190=1 fixes vsnprintf path; broad-scan
    // zeroes vtable pointers so the null-guarded bctrl in sub_82508480 is skipped safely.

    // Override sub_823143D0: font-slot allocator that vtable-dispatches through the
    // global returned by sub_824A6890 (0x8319DEA0). Xenia dump stored a host pointer
    // there; after zeroing it the factory is null, so the vtable call at :5949 faults.
    // Redirect to no-op so font-slot init is skipped (graphics unimplemented anyway).
    PPC_LOOKUP_FUNC(g_base, 0x823143D0) = ppc_noop_stub;
    printf("[PATCH] sub_823143D0 redirected to no-op\n");

    // Override sub_824A5D00: factory/initializer that calls vtable[3] on objects
    // created by sub_82548568. Those objects have null vtables because the factory
    // chain produces zero-initialized output (no valid backing store for this subsystem).
    // sub_824AADE0 (the only caller) ignores the return value of sub_824A5D00 and
    // always returns 1 regardless — NOP is safe.
    // BUT: sub_824A5D00 IS the shader-compile entry — it calls the parser (sub_82549080),
    // which is where STUBSRC/Path-B inject. No-op'ing it kills the shader-parse path → the
    // game never compiles runtime shaders → never reaches GAME.DAT/menu (pre-menu stall).
    // The original null-vtable crash only fired because the parser was FAILING (returning a
    // garbage obj); with STUBSRC the parser succeeds + the bctrl at 0x824A5DB0 is null-guarded.
    // LSWTCS_RUNSHC=1 lets the real function run (the route toward the rendering pipeline).
    { const char* e = getenv("LSWTCS_RUNSHC");
      if (e && e[0] != '0') {
          printf("[PATCH] sub_824A5D00 LEFT INTACT (LSWTCS_RUNSHC) — shader-parse path active\n");
      } else {
          PPC_LOOKUP_FUNC(g_base, 0x824A5D00) = ppc_noop_stub;
          printf("[PATCH] sub_824A5D00 redirected to no-op\n");
      } }

    // Broad-scan: zero Xenia host pointer artifacts in the post-code data section only.
    // The pre-code [PPC_IMAGE_BASE, PPC_CODE_BASE) contains float constants (1/60, 1.0,
    // 3.0, etc.) that fall in [0x20000000,0x82000000) but are NOT Xenia pointers —
    // scanning them incorrectly zeroes timing constants that cripple the frame timer.
    // The post-code [PPC_CODE_BASE+PPC_CODE_SIZE, image_end) contains game globals and
    // object data that do contain Xenia truncated-pointer artifacts.
    {
        uint32_t post_start = PPC_CODE_BASE + PPC_CODE_SIZE;
        uint32_t post_end   = PPC_IMAGE_BASE + PPC_IMAGE_SIZE;
        int count = 0, kept = 0;
        for (uint32_t addr = post_start; addr < post_end; addr += 4) {
            // Pristine .data has no Xenia host ptrs; scanning it zeroes real float constants
            // (sinf limit 2.2e8 / +-1.0 at 0x82F1FDC0 -> all-NaN sine LUT 0x83239F20 -> NaN camera).
            static uint32_t zlo = 0, zhi = 0; static bool zinit = false;
            if (!zinit) { zinit = true;
                const char* a = getenv("LSWTCS_SCANZERO_LO"); const char* b = getenv("LSWTCS_SCANZERO_HI");
                if (a && b) { zlo = (uint32_t)strtoul(a, nullptr, 0); zhi = (uint32_t)strtoul(b, nullptr, 0); } }
            if (g_pristine_data && !getenv("LSWTCS_BROADSCAN_DATA") &&
                addr >= 0x82E70000u && addr < 0x82E70000u + 0x0041491Cu &&
                !(addr >= zlo && addr < zhi)) continue;
            uint32_t* p = (uint32_t*)(g_base + addr);
            uint32_t v = __builtin_bswap32(*p);
            // PRESERVE [0x84000000,0x94000000): valid GUEST pointers (guest heap / data) live
            // here and are INDISTINGUISHABLE from truncated Xenia host ptrs. Zeroing them shreds
            // the virtual-heap free-list (sub_822A15D8) → shader compiler can't alloc → no shaders.
            if ((v >= 0x84000000u && v < 0x94000000u)) { kept++; continue; }
            if ((v >= 0x20000000u && v < 0x82000000u) ||
                (v >= 0x94000000u && v < 0xFF000000u)) {
                // CRITICAL: do NOT zero ASCII text. 4 printable chars form a word in
                // [0x20202020,0x7E7E7E7E] which is INSIDE the low zero-range — zeroing it
                // destroys embedded string data, including the NU2 uber-shader HLSL template
                // (sub_824AE770 memcpys it for the runtime shader compile). Preserve words whose
                // bytes are all printable ASCII / common whitespace.
                const uint8_t* b = (const uint8_t*)p;
                bool is_text = true;
                for (int k = 0; k < 4; k++) {
                    uint8_t c = b[k];
                    if (!((c >= 0x20 && c <= 0x7E) || c == 0x09 || c == 0x0A || c == 0x0D)) { is_text = false; break; }
                }
                if (is_text) { kept++; continue; }
                *p = 0;
                count++;
            }
        }
        printf("[PATCH] Broad-scan zeroed %d host ptrs; kept %d guest-heap ptrs (0x84-0x94M)\n", count, kept);
    }

    // Patch global at 0x83290B90: the VdSwap worker (sub_822D2AB0) reads the D3D device via
    // *(*(0x83290B90) + 0).  The game's D3D init writes the device-table pointer there but it
    // never runs in our port.  The correct value is 0x830CB7E4 — a sibling global (lis(-31987)
    // - 18460) that sub_8248C5E8 already uses successfully to look up the same device object.
    // Writing 0x830CB7E4 here makes the double-indirection work:
    //   *(0x83290B90) = 0x830CB7E4   →   *(0x830CB7E4) = device_object_ptr (set by game init)
    {
        uint32_t* p = (uint32_t*)(g_base + 0x83290B90);
        printf("[PATCH] 0x83290B90 = 0x%08X -> 0x830CB7E4 (D3D device table ptr for VdSwap worker)\n",
               __builtin_bswap32(*p));
        *p = __builtin_bswap32(0x830CB7E4u);
    }

    // Patch global at 0x831758B0: the VdSwap gate in sub_82314CA0 checks *(r30+22704)
    // where r30 = lis(-31977) = 0x83170000, so addr = 0x831758B0.  The game sets this
    // via XNotifyGetNext (notification type 9) when the rendering subsystem initialises —
    // a path that never executes in our port.  The value is only used as a null check
    // (non-zero = rendering active), never dereferenced as a pointer, so setting it to 1
    // makes sub_82314CA0 progress to sub_8248CAD8 → sub_822AFDA0 → sets gate bit at
    // device+10942, allowing the D2AB0 VdSwap worker to call VdSwap each frame.
    // The second gate (*(0x8327EAE4) == 0) is satisfied by the broad-scan above.
    //
    // ⚠️ 2026-07-19 (session 3): forcing gate1=1 is WRONG for the titles state.
    // Ghidra + cdb proved sub_82314CA0 (called from the frontend loop D680 via
    // sub_82506A38) reads *(0x831758B0) TWICE with OPPOSITE effect:
    //   (a) entry:  if (gate==0) skip the whole render-wait block  (loc_82314DC8)
    //   (b) inside: loc_82314D9C loops (NtWaitForSingleObjectEx + pump) WHILE
    //       gate != 0 — i.e. it expects the render worker to CLEAR the gate.
    // With gate forced to 1 and no real init to clear it, the main thread enters
    // (a) then SPINS FOREVER in (b) — D680 never returns, the main game loop never
    // iterates, and NO script command (incl. #1213 titles-scene load) ever fires.
    // Xenia ground truth at the titles screen: [0x831758B0]=0 and [0x8327EAE4]=0.
    // ⇒ leave gate1 at its natural .bss 0 so sub_82314CA0 skips the block and
    // returns (matching Xenia). LSWTCS_VDGATE1=1 restores the old force-to-1 for
    // A/B. (If VdSwap frames stop with gate=0, the real fix is to run the render
    // init that both sets and clears it, not to force it.)
    {
        uint32_t* p = (uint32_t*)(g_base + 0x831758B0);
        uint32_t gate2 = __builtin_bswap32(*(uint32_t*)(g_base + 0x8327EAE4));
        const char* force = getenv("LSWTCS_VDGATE1");
        uint32_t newval = (force && force[0] == '1') ? 1u : 0u;
        printf("[PATCH] VdSwap gate1 0x831758B0 = 0x%08X -> 0x%08X, gate2 0x8327EAE4 = 0x%08X (need 0)\n",
               __builtin_bswap32(*p), newval, gate2);
        *p = __builtin_bswap32(newval);
    }

    // Set up initial context
    PPCContext ctx{};
    ctx.r1.u32 = 0x835FFFF0 - 0x10;
    ctx.r2.u32 = 0;
    // Initialize fpscr.csr from the host MXCSR so disableFlushMode/enableFlushMode
    // don't zero out exception-mask bits (which would unmask all SSE FP exceptions).
    ctx.fpscr.csr = ctx.fpscr.getcsr();
    printf("[INIT] Stack: 0x%08X\n", ctx.r1.u32);

    // Initialize D3D12 rendering window before entering the game loop.
    gpu_d3d12_init();

    // Set up PCR (Process Control Register block) for the main thread.
    // Worker threads get PCRs via ExCreateThread; the main thread (which runs the
    // T-sequence) is the host thread and never goes through ExCreateThread.
    // Without r13 pointing to a valid PCR, sub_822AFB88 reads tick=0 always and
    // sub_822AE698 never times out → infinite spin during T10 (sub_82306050).
    {
        ctx.r13.u32 = ppc_alloc_main_thread_pcr();
        printf("[INIT] Main thread r13 (PCR) = 0x%08X\n", ctx.r13.u32);
    }

    // Run the MAIN thread's per-thread init (THREAD_ATTACH) the same way apiThreadStartup does for
    // worker threads (it calls sub_8229F180 with r3=1). We jump straight to _xstart and otherwise skip
    // this, leaving the main thread's per-thread TLS/runtime state uninitialized — which is why the
    // shader compiler (which runs on the main thread during engine-shader setup) gets an intermittently
    // wrong per-thread context. Use a temp context (same r1/r2/r13) so _xstart's ctx is preserved.
    {
        PPCContext ictx = ctx;
        ictx.r3.u32    = 1;        // DLL_THREAD_ATTACH
        ictx.fpscr.csr = 0x1F80u;
        printf("[INIT] main-thread per-thread init (sub_8229F180 attach)...\n");
        sub_8229F180(ictx, g_base);
        printf("[INIT] main-thread per-thread init done\n");
    }

    // Opt-in tripwire (LSWTCS_TABLEGUARD=1): mark the dispatch table read-only once all
    // slots are final (build block + the two no-op redirects above). Any later write —
    // a host-side bug or a guest wild pointer landing in [0xC8000000, +CODE_SIZE*2) —
    // faults loudly at the writer instead of silently corrupting indirect dispatch.
    // Off by default: wild-but-harmless guest writes into this (formerly zero-filled,
    // writable) region would become crashes.
    if (getenv("LSWTCS_TABLEGUARD")) {
        DWORD oldp = 0;
        if (VirtualProtect(g_base + PPC_FUNC_TABLE_BASE, (SIZE_T)PPC_CODE_SIZE * 2,
                           PAGE_READONLY, &oldp))
            printf("[INIT] dispatch table marked read-only (LSWTCS_TABLEGUARD)\n");
        else
            printf("[INIT] TABLEGUARD VirtualProtect failed: %lu\n", GetLastError());
    }

    printf("[INIT] Calling _xstart at 0x8229E618\n");
    lswtcs_gil_enter();  // main thread joins the GIL (LSWTCS_GIL)
    _xstart(ctx, g_base);

    printf("[DONE] _xstart returned\n");
    return 0;
}
