// Forced-include (-include) for every Switch (devkitA64 GCC) translation unit.
// Supplies the clang-only builtins the recompiled code and runtime use, so the shared
// headers stay identical to the PC build.
#pragma once

#if defined(__GNUC__) && !defined(__clang__)
#ifndef __builtin_assume
#define __builtin_assume(cond) do { if (!(cond)) __builtin_unreachable(); } while (0)
#endif
// Rotates: GCC recognises this form and emits a single ROR.
static inline __attribute__((always_inline)) unsigned int __builtin_rotateleft32(unsigned int x, unsigned int n)
{ n &= 31; return (x << n) | (x >> ((32 - n) & 31)); }
static inline __attribute__((always_inline)) unsigned long long __builtin_rotateleft64(unsigned long long x, unsigned long long n)
{ n &= 63; return (x << n) | (x >> ((64 - n) & 63)); }
#define __builtin_debugtrap() __asm__ volatile("brk #0")
#endif

// x86 TSC (LSWTCS_MFTB_RAW diagnostics): the ARM generic timer counter stands in.
#if defined(__aarch64__)
static inline unsigned long long __builtin_ia32_rdtsc(void)
{ unsigned long long v; __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v)); return v; }
#endif
