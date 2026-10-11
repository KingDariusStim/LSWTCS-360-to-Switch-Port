// Native replacements for the game's C-library routines (2026-10-11).
//
// Why: the game's CRT is recompiled as byte / word loops where every access goes through the address fold and a
// volatile store. PC sampling profiles (HANDOFF_2026-10-11_fps_loading.md §1): memset 5-10 % of the main thread
// during loads, memcpy 19 % of the I/O thread, the case-insensitive compare 5.6 % of the loader thread.
//
// Guest functions (semantics read from the instructions kept as comments in the recompiled source):
//   sub_8250F2B0 memcpy(r3 dst, r4 src, r5 len) -> r3 = dst. A FORWARD copy in 8-byte, 4-byte or byte steps
//                depending on alignment: not a memmove. For dst <= src it equals memmove; for an overlap with
//                dst > src its result depends on the step size, so that case runs the guest function.
//   sub_8250F0A0 memset(r3 dst, r4 byte, r5 len) -> r3 unchanged. XenonRecomp split it at 0x8250F130, so the
//                recompiled version stops after one tail byte when (len after alignment) & 3 is 2 or 3: the last
//                one or two bytes were never written. The native version writes all len bytes, like the console.
//   sub_82500A58 _stricmp(r3 a, r4 b) -> -1 / 0 / 1. a == 0 -> -1, b == 0 -> 1. Folds 'a'..'z' and bytes >= 224
//                down by 32 and compares the folded bytes as SIGNED chars.
//   sub_82500958 stristr(r3 hay, r4 needle) -> pointer into hay or 0. Same fold, compared unsigned. Empty hay -> 0;
//                empty needle -> hay.
//   sub_824F9068 strlen(r3) -> r3.
//   sub_824F8F48 strcpy(r3 dst, r4 src) -> r3 = length copied. src == 0 writes only the terminator.
// Pointers go through PPC_HOST per 512 MB view region; anything that would cross a region (or a huge length) runs
// the guest function instead (the boot clears ~370 MB of heap with one memset; that is native too).
//
// LSWTCS_NATIVECRT (default 1): 0 = always run the guest functions.
// LSWTCS_CRTCHECK=1 (diagnostic): run the guest function for real and check its result against the native one
// ([CRT] MISMATCH lines; memset tail bytes the guest leaves unwritten are counted as [CRT] memset-guest-short).
#include "../../Convert 360/LSWTCS/output/ppc_context.h"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" void dbg_ram(const char* fmt, ...);
extern "C" PPC_FUNC(__imp__sub_8250F2B0);
extern "C" PPC_FUNC(__imp__sub_8250F0A0);
extern "C" PPC_FUNC(__imp__sub_82500A58);
extern "C" PPC_FUNC(__imp__sub_82500958);
extern "C" PPC_FUNC(__imp__sub_824F9068);
extern "C" PPC_FUNC(__imp__sub_824F8F48);

namespace {
constexpr uint32_t kRegion = 0x20000000u;   // one guest view region (PPC_HOST table entry)

int mode() {
    static const int m = [] {
        const char* e = getenv("LSWTCS_NATIVECRT");
        const char* c = getenv("LSWTCS_CRTCHECK");
        if (e && e[0] == '0') return 0;
        return (c && c[0] == '1') ? 2 : 1;
    }();
    return m;
}

// Bytes from addr to the end of its view region.
inline uint32_t region_left(uint32_t addr) { return kRegion - (addr & (kRegion - 1)); }

// Host pointer for [addr, addr + len) if it lies in one view region (so it is contiguous in host memory).
inline uint8_t* span(uint8_t* base, uint32_t addr, uint32_t len) {
    if (addr == 0 || len > region_left(addr)) return nullptr;
    return PPC_HOST(addr);
}

// Host pointer to a NUL-terminated guest string and its length, or nullptr if the terminator is not found
// before the end of the view region.
inline const uint8_t* str_span(uint8_t* base, uint32_t addr, uint32_t* len) {
    if (addr == 0) return nullptr;
    const uint8_t* p = PPC_HOST(addr);
    const void* z = memchr(p, 0, region_left(addr));
    if (!z) return nullptr;
    *len = uint32_t(static_cast<const uint8_t*>(z) - p);
    return p;
}

inline uint8_t fold(uint8_t c) { return ((c >= 97 && c <= 122) || c >= 224) ? uint8_t(c + 224) : c; }

std::atomic<uint64_t> g_mismatch{0}, g_memset_short{0}, g_checks{0}, g_fallback{0};
// Calls that run the guest function because a range or string is unusual (first ones logged).
void fallback(const char* what, uint32_t a, uint32_t b, uint32_t c) {
    const uint64_t n = ++g_fallback;
    if (n <= 20 || n % 10000 == 0)
        dbg_ram("[CRT] fallback %s(%08X, %08X, %08X) (#%llu)\n", what, a, b, c, (unsigned long long)n);
}
void mismatch(const char* what, uint32_t a, uint32_t b, uint32_t c, uint32_t got, uint32_t want) {
    const uint64_t n = ++g_mismatch;
    if (n <= 50 || n % 1000 == 0)
        dbg_ram("[CRT] MISMATCH %s(%08X, %08X, %u): guest %08X native %08X (#%llu)\n", what, a, b, c, got, want,
                (unsigned long long)n);
}
void check_tick() {
    const uint64_t n = ++g_checks;
    if (n % 200000 == 0)
        dbg_ram("[CRT] check: %llu calls, %llu mismatches, %llu memsets the guest left short\n", (unsigned long long)n,
                (unsigned long long)g_mismatch.load(), (unsigned long long)g_memset_short.load());
}

// Compares at most limit bytes (the room left in both strings' view regions); 2 = limit reached, run the guest.
int stricmp_native(const uint8_t* a, const uint8_t* b, uint32_t limit) {
    for (uint32_t k = 0; k < limit; ++k) {
        const int8_t x = int8_t(fold(a[k])), y = int8_t(fold(b[k]));
        if (x > y) return 1;
        if (x < y) return -1;
        if (x == 0) return 0;
    }
    return 2;
}

// Returns the offset of the match in hay, or -1.
int64_t stristr_native(const uint8_t* hay, uint32_t hay_len, const uint8_t* needle, uint32_t needle_len) {
    if (hay_len == 0) return -1;
    if (needle_len == 0) return 0;
    const uint8_t f0 = fold(needle[0]);
    for (uint32_t i = 0; i + needle_len <= hay_len; ++i) {
        if (fold(hay[i]) != f0) continue;
        uint32_t k = 1;
        while (k < needle_len && fold(hay[i + k]) == fold(needle[k])) ++k;
        if (k == needle_len) return i;
    }
    return -1;
}
}  // namespace

// Strong definitions: replace the recompiled weak aliases for every caller and the call table.

PPC_FUNC(sub_8250F2B0) {   // memcpy
    PPC_FUNC_PROLOGUE();
    const uint32_t dst = ctx.r3.u32, src = ctx.r4.u32, len = ctx.r5.u32;
    const int m = mode();
    uint8_t* d = len ? span(base, dst, len) : nullptr;
    const uint8_t* s = d ? span(base, src, len) : nullptr;
    // dst > src with overlap: the guest's result depends on its step size, run it.
    if (m == 0 || !s || (d > s && d < s + len)) {
        if (len == 0 && m != 0) return;   // the guest copies nothing and returns dst (r3 unchanged)
        if (m != 0) fallback("memcpy", dst, src, len);
        __imp__sub_8250F2B0(ctx, base);
        return;
    }
    if (m == 1) {
        memmove(d, s, len);
        ctx.r3.u64 = dst;
        return;
    }
    static thread_local std::vector<uint8_t> want;
    want.assign(s, s + len);
    __imp__sub_8250F2B0(ctx, base);
    if (ctx.r3.u32 != dst || memcmp(d, want.data(), len) != 0) mismatch("memcpy", dst, src, len, ctx.r3.u32, dst);
    check_tick();
}

PPC_FUNC(sub_8250F0A0) {   // memset
    PPC_FUNC_PROLOGUE();
    const uint32_t dst = ctx.r3.u32, len = ctx.r5.u32;
    const uint8_t v = ctx.r4.u8;
    const int m = mode();
    uint8_t* d = len ? span(base, dst, len) : nullptr;
    if (m == 0 || !d) {
        if (len == 0 && m != 0) return;
        if (m != 0) fallback("memset", dst, v, len);
        __imp__sub_8250F0A0(ctx, base);
        return;
    }
    if (m == 1) {
        memset(d, v, len);
        return;
    }
    __imp__sub_8250F0A0(ctx, base);
    // The recompiled tail can stop one or two bytes short (see the top of this file): count that separately,
    // anything else is a mismatch. Then leave what the console would have written.
    uint32_t first_bad = len;
    for (uint32_t i = 0; i < len; ++i) if (d[i] != v) { first_bad = i; break; }
    if (first_bad != len) {
        if (first_bad + 2 >= len) ++g_memset_short; else mismatch("memset", dst, v, len, first_bad, len);
        memset(d, v, len);
    }
    check_tick();
}

PPC_FUNC(sub_82500A58) {   // _stricmp
    PPC_FUNC_PROLOGUE();
    const int m = mode();
    const uint32_t a = ctx.r3.u32, b = ctx.r4.u32;
    // One pass, no pre-scan: most compares (the text parsers' command-table searches) differ at the first byte.
    const int r = (m && a && b) ? stricmp_native(PPC_HOST(a), PPC_HOST(b), std::min(region_left(a), region_left(b))) : 2;
    if (r == 2) { __imp__sub_82500A58(ctx, base); return; }   // null pointers and odd strings: the guest's own code
    if (m == 1) { ctx.r3.s64 = r; return; }
    __imp__sub_82500A58(ctx, base);
    if (ctx.r3.s32 != r) mismatch("stricmp", a, b, 0, ctx.r3.u32, uint32_t(r));
    check_tick();
}

PPC_FUNC(sub_82500958) {   // stristr
    PPC_FUNC_PROLOGUE();
    const int m = mode();
    const uint32_t hay = ctx.r3.u32, needle = ctx.r4.u32;
    uint32_t lh = 0, ln = 0;
    const uint8_t* ph = m ? str_span(base, hay, &lh) : nullptr;
    const uint8_t* pn = ph ? str_span(base, needle, &ln) : nullptr;
    if (!pn) { __imp__sub_82500958(ctx, base); return; }
    const int64_t off = stristr_native(ph, lh, pn, ln);
    const uint32_t r = off < 0 ? 0 : hay + uint32_t(off);
    if (m == 1) { ctx.r3.u64 = r; return; }
    __imp__sub_82500958(ctx, base);
    if (ctx.r3.u32 != r) mismatch("stristr", hay, needle, 0, ctx.r3.u32, r);
    check_tick();
}

PPC_FUNC(sub_824F9068) {   // strlen
    PPC_FUNC_PROLOGUE();
    const int m = mode();
    const uint32_t s = ctx.r3.u32;
    uint32_t len = 0;
    if (!m || !str_span(base, s, &len)) { __imp__sub_824F9068(ctx, base); return; }
    if (m == 1) { ctx.r3.u64 = len; return; }
    __imp__sub_824F9068(ctx, base);
    if (ctx.r3.u32 != len) mismatch("strlen", s, 0, 0, ctx.r3.u32, len);
    check_tick();
}

PPC_FUNC(sub_824F8F48) {   // strcpy returning the length
    PPC_FUNC_PROLOGUE();
    const int m = mode();
    const uint32_t dst = ctx.r3.u32, src = ctx.r4.u32;
    uint32_t len = 0;
    const uint8_t* s = (m && src) ? str_span(base, src, &len) : nullptr;
    uint8_t* d = s ? span(base, dst, len + 1) : nullptr;
    // Overlap: the guest's byte-by-byte forward copy can run on into its own output; leave that to the guest.
    if (!d || (d > s && d <= s + len) || (s > d && s <= d + len)) { __imp__sub_824F8F48(ctx, base); return; }
    if (m == 1) {
        memcpy(d, s, len + 1);
        ctx.r3.u64 = len;
        return;
    }
    static thread_local std::vector<uint8_t> want;
    want.assign(s, s + len + 1);
    __imp__sub_824F8F48(ctx, base);
    if (ctx.r3.u32 != len || memcmp(d, want.data(), len + 1) != 0) mismatch("strcpy", dst, src, len, ctx.r3.u32, len);
    check_tick();
}
