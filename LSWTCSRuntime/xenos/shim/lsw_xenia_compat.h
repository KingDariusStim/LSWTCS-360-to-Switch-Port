// Force-included into the vendored Xenia TUs. Xenia's base/memory.h only uses
// _aligned_malloc when XE_COMPILER_MSVC; clang targeting MinGW takes the POSIX branch,
// but MinGW has no posix_memalign. Map it onto _aligned_malloc (never called on our path).
#pragma once
#if defined(__MINGW32__)
#include <malloc.h>
#include <cstddef>
static inline int posix_memalign(void** p, size_t alignment, size_t size) {
  *p = _aligned_malloc(size, alignment);
  return *p ? 0 : 12;
}
#endif
