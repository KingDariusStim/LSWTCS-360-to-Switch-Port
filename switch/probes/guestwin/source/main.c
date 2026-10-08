// LSWTCS Switch probe #2: how do we build the 4 GB guest window without overcommit?
//
// The runtime addresses guest memory as base + guest_addr over a 0x104000000-byte window
// (4 GB guest space + the 64 MB dispatch table at 0x100000000). Horizon has no overcommit, so
// only the ranges the game uses may be backed. Candidates, tested here:
//   T1  svcMapPhysicalMemory at chosen addresses inside the alias region (fresh memory, no source).
//       Checks: alignment rules, R/W, UsedMemory accounting, unmap.
//   T2  Demand paging: an access to an unmapped page faults, __libnx_exception_handler maps the
//       2 MB block with svcMapPhysicalMemory and resumes. Would remove the need to know the
//       ranges in advance.
//   T3  Fallback: heap block -> code memory -> MapOwner (RW) at a chosen ASLR address.
// Results: screen + sdmc:/lswtcs_probe2.txt (flushed per line).
#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE* g_log;
#define LOG(...) do { printf(__VA_ARGS__); if (g_log) { fprintf(g_log, __VA_ARGS__); fflush(g_log); } consoleUpdate(NULL); } while (0)

#define WIN_SIZE 0x104000000ull
#define BLOCK    0x200000ull          // demand-paging granule (2 MB)

static u64 info_u64(u32 id) { u64 v = 0; svcGetInfo(&v, id, CUR_PROCESS_HANDLE, 0); return v; }
static u64 used_mb(void) { return info_u64(InfoType_UsedMemorySize) >> 20; }

static void show(const char* tag, u64 a) {
    MemoryInfo mi; u32 pi;
    Result rc = svcQueryMemory(&mi, &pi, a);
    LOG("    %-12s 0x%010lX: rc=0x%X type=0x%X perm=%X base=0x%lX size=0x%lX\n", tag, (unsigned long)a, rc,
        mi.type, mi.perm, (unsigned long)mi.addr, (unsigned long)mi.size);
}

// First free 4 GB-aligned window inside [lo, lo+size).
static u64 find_window(u64 lo, u64 size) {
    for (u64 a = (lo + 0xFFFFFFFFull) & ~0xFFFFFFFFull; a + WIN_SIZE <= lo + size; a += 0x100000000ull) {
        MemoryInfo mi; u32 pi;
        if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, a)) && mi.type == MemType_Unmapped && mi.addr + mi.size >= a + WIN_SIZE)
            return a;
    }
    return 0;
}

static u8* g_win;

// ---------------- T1 ----------------
static int test_physmap(void) {
    LOG("== T1 svcMapPhysicalMemory in alias region\n");
    u64 lo = info_u64(InfoType_AliasRegionAddress), sz = info_u64(InfoType_AliasRegionSize);
    g_win = (u8*)find_window(lo, sz);
    LOG("  window at %p (alias region 0x%lX + 0x%lX), used=%lu MB\n", g_win, (unsigned long)lo, (unsigned long)sz, (unsigned long)used_mb());
    if (!g_win) return 0;
    struct { u64 off, size; const char* what; } r[] = {
        { 0x80000000ull, 0x20000000ull, "phys 512 MB" },
        { 0x40000000ull, 0x04000000ull, "vheap 64 MB" },
        { 0x100000000ull, 0x04000000ull, "functable 64 MB" },
        { 0x7FC80000ull, 0x00010000ull, "64 KB (unaligned to 2 MB)" },
        { 0x7FEA0000ull, 0x00001000ull, "4 KB" },
    };
    int ok = 1;
    for (unsigned i = 0; i < sizeof r / sizeof r[0]; ++i) {
        u64 before = used_mb();
        Result rc = svcMapPhysicalMemory(g_win + r[i].off, r[i].size);
        LOG("  map +0x%09lX %-26s rc=0x%X used %lu -> %lu MB\n", (unsigned long)r[i].off, r[i].what, rc,
            (unsigned long)before, (unsigned long)used_mb());
        if (R_FAILED(rc)) { if (i < 3) ok = 0; continue; }
        volatile u32* p = (volatile u32*)(g_win + r[i].off);
        p[0] = 0x4C535731u; p[r[i].size / 4 - 1] = 0x4C535732u;
        int rw = p[0] == 0x4C535731u && p[r[i].size / 4 - 1] == 0x4C535732u;
        LOG("    rw=%d\n", rw);
        if (!rw) ok = 0;
        show("mapped", (u64)(g_win + r[i].off));
    }
    // Leave the 512 MB + vheap + table mapped for T2's neighbourhood; unmap the small ones.
    Result rc = svcUnmapPhysicalMemory(g_win + 0x7FC80000ull, 0x10000);
    LOG("  unmap 64 KB rc=0x%X used=%lu MB\n", rc, (unsigned long)used_mb());
    LOG("  T1 result: %s\n", ok ? "WORKS" : "no");
    return ok;
}

// ---------------- T2 ----------------
alignas(16) u8 __nx_exception_stack[0x4000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);
static volatile int g_faults, g_mapped;
static volatile u64 g_last_far;
static volatile Result g_last_rc;

void __libnx_exception_handler(ThreadExceptionDump* ctx) {
    u64 far = ctx->far.x;
    g_last_far = far;
    ++g_faults;
    if (g_win && far >= (u64)g_win && far < (u64)g_win + WIN_SIZE) {
        u64 blk = far & ~(BLOCK - 1);
        Result rc = svcMapPhysicalMemory((void*)blk, BLOCK);
        g_last_rc = rc;
        if (R_SUCCEEDED(rc)) { ++g_mapped; return; }   // libnx restores ctx and resumes the access
    }
    // Not ours: skip the faulting instruction so the probe can report instead of dying.
    ctx->pc.x += 4;
}

static int test_demand(void) {
    LOG("== T2 demand paging via exception handler\n");
    if (!g_win) { LOG("  no window\n"); return 0; }
    volatile u32* p = (volatile u32*)(g_win + 0x50000000ull + 0x1234);   // never mapped
    u64 before = used_mb();
    p[0] = 0xCAFEF00Du;
    u32 v = p[0];
    LOG("  write/read 0x%08X faults=%d mapped=%d far=0x%lX rc=0x%X used %lu -> %lu MB\n", v, g_faults, g_mapped,
        (unsigned long)g_last_far, g_last_rc, (unsigned long)before, (unsigned long)used_mb());
    show("demand blk", (u64)p);
    // Second touch in another block, read first this time.
    volatile u32* q = (volatile u32*)(g_win + 0x0A000000ull);
    u32 z = q[0];
    LOG("  read-first block: value=0x%08X faults=%d mapped=%d\n", z, g_faults, g_mapped);
    int ok = v == 0xCAFEF00Du && g_mapped == 2 && z == 0;
    LOG("  T2 result: %s\n", ok ? "WORKS" : "no");
    return ok;
}

// ---------------- T3 ----------------
// Real sizes: the runtime's layout, all inside one reserved window.
static int test_codemem(void) {
    LOG("== T3 heap -> code memory MapOwner at chosen addresses (runtime sizes)\n");
    virtmemLock();
    u8* w = (u8*)virtmemFindAslr(WIN_SIZE, 0x10000);
    VirtmemReservation* res = w ? virtmemAddReservation(w, WIN_SIZE) : NULL;
    virtmemUnlock();
    LOG("  window %p reserved=%d\n", w, res != NULL);
    if (!res) return 0;
    struct { u64 off, size; const char* what; } r[] = {
        { 0x80000000ull, 0x20000000ull, "phys 512 MB" },
        { 0x40000000ull, 0x10000000ull, "vheap 256 MB" },
        { 0x100000000ull, 0x04000000ull, "functable 64 MB" },
        { 0x7FC80000ull, 0x00010000ull, "mmio 64 KB" },
    };
    int ok = 1;
    for (unsigned i = 0; i < sizeof r / sizeof r[0]; ++i) {
        void* src = aligned_alloc(0x1000, r[i].size);
        if (!src) { LOG("  %-16s aligned_alloc failed\n", r[i].what); ok = 0; continue; }
        Handle h; Result rc = svcCreateCodeMemory(&h, src, r[i].size);
        Result rc2 = R_SUCCEEDED(rc) ? svcControlCodeMemory(h, CodeMapOperation_MapOwner, w + r[i].off, r[i].size, Perm_Rw) : rc;
        int rw = 0;
        if (R_SUCCEEDED(rc2)) {
            volatile u32* p = (volatile u32*)(w + r[i].off);
            p[0] = 0x12345678u + i; p[r[i].size / 4 - 1] = 0x9ABCDEF0u + i;
            rw = p[0] == 0x12345678u + i && p[r[i].size / 4 - 1] == 0x9ABCDEF0u + i;
        }
        LOG("  %-16s create rc=0x%X MapOwner rc=0x%X rw=%d used=%lu MB\n", r[i].what, rc, rc2, rw, (unsigned long)used_mb());
        if (!rw) ok = 0;
    }
    show("phys view", (u64)(w + 0x80000000ull));
    // memset-heavy check: zero the whole 512 MB (the runtime does this at boot).
    u64 t0 = armGetSystemTick();
    memset(w + 0x80000000ull, 0, 0x20000000ull);
    LOG("  memset 512 MB: %lu ms\n", (unsigned long)(armTicksToNs(armGetSystemTick() - t0) / 1000000));
    LOG("  T3 result: %s\n", ok ? "WORKS" : "no");
    return ok;
}

int main(int argc, char** argv) {
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad; padInitializeDefault(&pad);
    g_log = fopen("sdmc:/lswtcs_probe2.txt", "w");
    LOG("LSWTCS guestwin probe (#2)\n");
    LOG("  TotalMemory %lu MB, UsedMemory %lu MB, SystemResource %lu KB\n",
        (unsigned long)(info_u64(InfoType_TotalMemorySize) >> 20), (unsigned long)used_mb(),
        (unsigned long)(info_u64(InfoType_SystemResourceSizeTotal) >> 10));
    int t1 = test_physmap(), t2 = test_demand(), t3 = test_codemem();
    LOG("== SUMMARY physmap=%d demand=%d codemem=%d\n", t1, t2, t3);
    LOG("DONE. Press + to exit (auto-exit in 10 s).\n");
    if (g_log) fclose(g_log);
    g_log = NULL;
    u64 t0 = armGetSystemTick();
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
        if (armTicksToNs(armGetSystemTick() - t0) > 10000000000ull) break;
        consoleUpdate(NULL);
    }
    consoleExit(NULL);
    return 0;
}
