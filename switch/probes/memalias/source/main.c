// LSWTCS Switch probe #1: can a process see ONE block of memory at several virtual addresses?
//
// The PC runtime maps the 360's 512 MB physical memory at three guest ranges (0x80000000,
// 0xA0000000, 0xC0000000 = the same bytes). This probe tries every Horizon mechanism that might
// give such aliases and reports, for each: whether the SVCs succeed, the resulting permissions, and
// whether a write through one view is visible through the others. It also records the process's
// memory budget and address-space layout. Results: screen + sdmc:/lswtcs_probe.txt (appended as we
// go, so a fault still leaves a trail). Views are only read after svcQueryMemory says readable.
#include <switch.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

static FILE* g_log;
#define LOG(...) do { printf(__VA_ARGS__); if (g_log) { fprintf(g_log, __VA_ARGS__); fflush(g_log); } consoleUpdate(NULL); } while (0)

#define SZ (16u * 1024u * 1024u)   // test block size (16 MB)

static u32 perm_of(void* p) {
    MemoryInfo mi; u32 pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, (u64)p))) return 0xFFFFFFFFu;
    return mi.perm;
}
static void show(const char* tag, void* p) {
    MemoryInfo mi; u32 pi;
    Result rc = svcQueryMemory(&mi, &pi, (u64)p);
    LOG("    %-10s %p: query rc=0x%X type=0x%X perm=%X attr=%X size=0x%lX\n", tag, p, rc, mi.type, mi.perm, mi.attr, (unsigned long)mi.size);
}
// Write a pattern through `w`, check it through every readable view in `r`. Returns 1 if all alias.
static int check_alias(const char* name, volatile u32* w, volatile u32** r, int nr) {
    if (!(perm_of((void*)w) & Perm_W)) { LOG("  [%s] writer view not writable\n", name); return 0; }
    int ok = 1;
    for (u32 pass = 0; pass < 2; ++pass) {
        u32 pat = 0x4C535730u + pass;   // "LSW0"/"LSW1"
        w[0] = pat; w[SZ / 4 - 1] = ~pat; w[SZ / 8] = pat ^ 0x5A5A5A5Au;
        armDCacheFlush((void*)w, SZ);
        for (int i = 0; i < nr; ++i) {
            if (!(perm_of((void*)r[i]) & Perm_R)) { LOG("  [%s] view %d not readable\n", name, i); ok = 0; continue; }
            int same = r[i][0] == pat && r[i][SZ / 4 - 1] == ~pat && r[i][SZ / 8] == (pat ^ 0x5A5A5A5Au);
            if (!same) ok = 0;
            LOG("  [%s] pass %u: view %d %s (read %08X)\n", name, pass, i, same ? "SEES the write" : "does NOT see it", r[i][0]);
        }
    }
    return ok;
}

static void* find_free(size_t sz) {   // caller holds virtmemLock
    return virtmemFindAslr(sz, 0x1000);
}

static void info(void) {
    static const struct { const char* n; u32 id; } k[] = {
        {"AliasRegion", InfoType_AliasRegionAddress}, {"AliasSize", InfoType_AliasRegionSize},
        {"HeapRegion", InfoType_HeapRegionAddress}, {"HeapSize", InfoType_HeapRegionSize},
        {"AslrRegion", InfoType_AslrRegionAddress}, {"AslrSize", InfoType_AslrRegionSize},
        {"StackRegion", InfoType_StackRegionAddress}, {"StackSize", InfoType_StackRegionSize},
        {"TotalMemory", InfoType_TotalMemorySize}, {"UsedMemory", InfoType_UsedMemorySize}};
    LOG("== process info\n");
    for (unsigned i = 0; i < sizeof k / sizeof k[0]; ++i) {
        u64 v = 0; Result rc = svcGetInfo(&v, k[i].id, CUR_PROCESS_HANDLE, 0);
        LOG("  %-12s rc=0x%X value=0x%lX (%lu MB)\n", k[i].n, rc, (unsigned long)v, (unsigned long)(v >> 20));
    }
    static const struct { const char* n; unsigned id; } s[] = {
        {"MapMemory", 0x04}, {"MapSharedMemory", 0x13}, {"CreateTransferMemory", 0x15},
        {"MapPhysicalMemory", 0x2C}, {"CreateCodeMemory", 0x4B}, {"ControlCodeMemory", 0x4C},
        {"CreateSharedMemory", 0x50}, {"MapTransferMemory", 0x51}, {"MapProcessCodeMemory", 0x77}};
    LOG("== syscall hints (homebrew loader)\n");
    for (unsigned i = 0; i < sizeof s / sizeof s[0]; ++i) LOG("  0x%02X %-22s %s\n", s[i].id, s[i].n, envIsSyscallHinted(s[i].id) ? "hinted" : "NOT hinted");
}

// T1: shared memory created by us, mapped 3 times (the ideal: three RW views of one block).
static int test_shared(void) {
    LOG("== T1 shared memory x3\n");
    Handle h; Result rc = svcCreateSharedMemory(&h, SZ, Perm_Rw, Perm_R);
    LOG("  svcCreateSharedMemory rc=0x%X\n", rc);
    if (R_FAILED(rc)) return 0;
    void* v[3] = {0}; int mapped = 0;
    for (int i = 0; i < 3; ++i) {
        virtmemLock(); v[i] = find_free(SZ); rc = svcMapSharedMemory(h, v[i], SZ, Perm_Rw); virtmemUnlock();
        LOG("  map #%d at %p rc=0x%X\n", i, v[i], rc);
        if (R_FAILED(rc)) { v[i] = 0; break; }
        show("view", v[i]); ++mapped;
    }
    int ok = 0;
    if (mapped >= 2) {
        volatile u32* r[2] = {(volatile u32*)v[1], (volatile u32*)(mapped > 2 ? v[2] : v[1])};
        ok = check_alias("T1", (volatile u32*)v[0], r, mapped - 1);
    }
    for (int i = 0; i < 3; ++i) if (v[i]) svcUnmapSharedMemory(h, v[i], SZ);
    svcCloseHandle(h);
    LOG("  T1 result: %s\n", ok ? "ALIASES (3 RW views)" : "no");
    return ok;
}

// T2: code memory: owner view RW + slave view R of one heap block (two views, one read-only).
static int test_code(void) {
    LOG("== T2 code memory (owner RW + slave R)\n");
    void* src = memalign(0x1000, SZ);
    if (!src) { LOG("  memalign failed\n"); return 0; }
    memset(src, 0, SZ);
    Handle h; Result rc = svcCreateCodeMemory(&h, src, SZ);
    LOG("  svcCreateCodeMemory rc=0x%X\n", rc);
    if (R_FAILED(rc)) { free(src); return 0; }
    void *own, *slv;
    virtmemLock(); own = find_free(SZ); rc = svcControlCodeMemory(h, CodeMapOperation_MapOwner, own, SZ, Perm_Rw); virtmemUnlock();
    LOG("  MapOwner at %p rc=0x%X\n", own, rc);
    Result rc2 = -1;
    if (R_SUCCEEDED(rc)) { virtmemLock(); slv = find_free(SZ); rc2 = svcControlCodeMemory(h, CodeMapOperation_MapSlave, slv, SZ, Perm_R); virtmemUnlock();
                           LOG("  MapSlave at %p rc=0x%X\n", slv, rc2); }
    int ok = 0;
    if (R_SUCCEEDED(rc) && R_SUCCEEDED(rc2)) {
        show("source", src); show("owner", own); show("slave", slv);
        volatile u32* r[1] = {(volatile u32*)slv};
        ok = check_alias("T2", (volatile u32*)own, r, 1);
        svcControlCodeMemory(h, CodeMapOperation_UnmapSlave, slv, SZ, 0);
    }
    if (R_SUCCEEDED(rc)) svcControlCodeMemory(h, CodeMapOperation_UnmapOwner, own, SZ, 0);
    svcCloseHandle(h);
    free(src);
    LOG("  T2 result: %s\n", ok ? "aliases (RW + R only)" : "no");
    return ok;
}

// T3: transfer memory of our own heap block, mapped back into ourselves.
static int test_transfer(void) {
    LOG("== T3 transfer memory, self-mapped\n");
    void* src = memalign(0x1000, SZ);
    if (!src) { LOG("  memalign failed\n"); return 0; }
    memset(src, 0, SZ);
    Handle h; Result rc = svcCreateTransferMemory(&h, src, SZ, Perm_None);
    LOG("  svcCreateTransferMemory(perm None) rc=0x%X\n", rc);
    if (R_FAILED(rc)) { free(src); return 0; }
    void* dst; virtmemLock(); dst = find_free(SZ); rc = svcMapTransferMemory(h, dst, SZ, Perm_None); virtmemUnlock();
    LOG("  svcMapTransferMemory at %p rc=0x%X\n", dst, rc);
    int ok = 0;
    if (R_SUCCEEDED(rc)) {
        show("source", src); show("mapped", dst);
        volatile u32* r[1] = {(volatile u32*)src};
        ok = check_alias("T3", (volatile u32*)dst, r, 1);
        svcUnmapTransferMemory(h, dst, SZ);
    }
    svcCloseHandle(h);
    free(src);
    LOG("  T3 result: %s\n", ok ? "ALIASES" : "no");
    return ok;
}

// T4: svcMapMemory (heap -> alias/stack region). Expected: source loses access while mapped.
static int test_mapmemory(void) {
    LOG("== T4 svcMapMemory\n");
    void* src = memalign(0x1000, SZ);
    if (!src) { LOG("  memalign failed\n"); return 0; }
    memset(src, 0, SZ);
    void* dst; Result rc;
    virtmemLock(); dst = virtmemFindStack(SZ, 0x1000); rc = svcMapMemory(dst, src, SZ); virtmemUnlock();
    LOG("  svcMapMemory dst=%p rc=0x%X\n", dst, rc);
    int ok = 0;
    if (R_SUCCEEDED(rc)) {
        show("source", src); show("dst", dst);
        volatile u32* r[1] = {(volatile u32*)src};
        ok = check_alias("T4", (volatile u32*)dst, r, 1);
        svcUnmapMemory(dst, src, SZ);
    }
    free(src);
    LOG("  T4 result: %s\n", ok ? "ALIASES" : "no");
    return ok;
}

// Large budget checks: can we get 512 MB (the 360's physical memory) as one shared block, and
// reserve a 4 GB guest window in the address space?
static void test_budget(void) {
    LOG("== budget\n");
    Handle h; Result rc = svcCreateSharedMemory(&h, 512u * 1024u * 1024u, Perm_Rw, Perm_R);
    LOG("  512 MB shared memory: rc=0x%X\n", rc);
    if (R_SUCCEEDED(rc)) {
        void* v; virtmemLock(); v = find_free(512u << 20); rc = svcMapSharedMemory(h, v, 512u << 20, Perm_Rw); virtmemUnlock();
        LOG("  mapped at %p rc=0x%X\n", v, rc);
        if (R_SUCCEEDED(rc)) svcUnmapSharedMemory(h, v, 512u << 20);
        svcCloseHandle(h);
    }
    virtmemLock(); void* w = virtmemFindAslr(0x104000000ull, 0); virtmemUnlock();
    LOG("  free 4 GB+ window in ASLR region: %p\n", w);
}

int main(int argc, char** argv) {
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad; padInitializeDefault(&pad);
    g_log = fopen("sdmc:/lswtcs_probe.txt", "w");
    LOG("LSWTCS memalias probe\n");
    info();
    int t1 = test_shared(), t2 = test_code(), t3 = test_transfer(), t4 = test_mapmemory();
    test_budget();
    LOG("== SUMMARY shared=%d code=%d transfer=%d mapmemory=%d\n", t1, t2, t3, t4);
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
