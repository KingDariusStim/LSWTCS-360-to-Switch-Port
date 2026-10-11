/* Horizon has no mmap. rpmalloc_switch.c gives rpmalloc its own memory_map / memory_unmap hooks, so
 * rpmalloc's OS layer (the only user of these) is compiled but never called. */
#pragma once
#include <stddef.h>
#include <sys/types.h>
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_PRIVATE 2
#define MAP_ANONYMOUS 0x20
#define MAP_FAILED ((void*)-1)
#define MADV_DONTNEED 4
static inline void* mmap(void* a, size_t n, int p, int f, int fd, off_t o) {
    (void)a; (void)n; (void)p; (void)f; (void)fd; (void)o; return MAP_FAILED;
}
static inline int munmap(void* a, size_t n) { (void)a; (void)n; return -1; }
static inline int madvise(void* a, size_t n, int adv) { (void)a; (void)n; (void)adv; return 0; }
