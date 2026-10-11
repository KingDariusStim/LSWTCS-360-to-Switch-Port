/* rpmalloc as the process allocator on Horizon (switch/CMakeLists.txt LSW_RPMALLOC, default ON).
 *
 * newlib's malloc has one global lock. Mesa's shader compiler allocates millions of small blocks, so
 * shader builds on two or three threads at once spent most of their time waiting on it: 50-70% of
 * acquisitions contended, 9-12 s lost per stalled frame, a 12-program title build took 20.6 s on 3
 * threads vs 0.93 s on one (Eden, 2026-10-08, [GLSTALL] "malloc lock"). rpmalloc keeps per-thread
 * caches, so small allocations take no shared lock.
 *
 * Every newlib entry point (-Wl,--wrap=_malloc_r etc.; malloc / free / new / strdup / stdio all end
 * there) is redirected to rpmalloc. rpmalloc gets its memory in 64 KB-aligned spans from the real
 * newlib allocator: span-sized requests are rare, so the old lock stays cold. Spans come from the real
 * _malloc_r, aligned here: newlib's _memalign_r calls _malloc_r itself, which is wrapped, so taking spans
 * from it recursed into rpmalloc before the first thread heap existed (hang at boot, 2026-10-08). */
#include <unistd.h>
#include <reent.h>
#include <stddef.h>
#include <stdint.h>

#define sysconf(name) 4096L   /* page size: rpmalloc's only sysconf use (_SC_PAGESIZE) */
#define LSW_RPMALLOC_TID_TPIDRRO 1   /* thread id = tpidrro_el0 (tpidr_el0 is the same on every thread) */
#include "rpmalloc.c"
#undef sysconf

extern void* __real__malloc_r(struct _reent*, size_t);
extern void __real__free_r(struct _reent*, void*);

#define LSW_SPAN_ALIGN ((size_t)64 * 1024)   /* rpmalloc's span size */
static void* lsw_span_map(size_t size, size_t* offset) {
    char* p = (char*)__real__malloc_r(_REENT, size + LSW_SPAN_ALIGN);
    if (!p) { *offset = 0; return 0; }
    char* a = (char*)(((uintptr_t)p + LSW_SPAN_ALIGN - 1) & ~(uintptr_t)(LSW_SPAN_ALIGN - 1));
    *offset = (size_t)(a - p);   /* handed back to lsw_span_unmap */
    return a;
}
static void lsw_span_unmap(void* address, size_t size, size_t offset, size_t release) {
    (void)size;
    if (release) __real__free_r(_REENT, (char*)address - offset);   /* partial decommit: nothing to do */
}

static void lsw_rp_ensure(void) {
    if (rpmalloc_is_thread_initialized()) return;
    if (!_rpmalloc_initialized) {   /* first allocation of the process (crt0, single-threaded) */
        rpmalloc_config_t cfg;
        memset(&cfg, 0, sizeof cfg);
        cfg.memory_map = lsw_span_map;
        cfg.memory_unmap = lsw_span_unmap;
        cfg.page_size = 4096;
        rpmalloc_initialize_config(&cfg);
        return;
    }
    rpmalloc_thread_initialize();
}

void* __wrap__malloc_r(struct _reent* r, size_t n) { (void)r; lsw_rp_ensure(); return rpmalloc(n); }
void __wrap__free_r(struct _reent* r, void* p) { (void)r; if (p) { lsw_rp_ensure(); rpfree(p); } }
void* __wrap__realloc_r(struct _reent* r, void* p, size_t n) { (void)r; lsw_rp_ensure(); return rprealloc(p, n); }
void* __wrap__calloc_r(struct _reent* r, size_t a, size_t b) { (void)r; lsw_rp_ensure(); return rpcalloc(a, b); }
void* __wrap__memalign_r(struct _reent* r, size_t align, size_t n) { (void)r; lsw_rp_ensure(); return rpaligned_alloc(align, n); }
size_t __wrap__malloc_usable_size_r(struct _reent* r, void* p) { (void)r; return rpmalloc_usable_size(p); }
