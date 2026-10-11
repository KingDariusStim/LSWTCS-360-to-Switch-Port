// Native replacement for the game's LZ2K decompressor (guest sub_82504740 and the functions it calls).
//
// Why: on the Switch a scene or level load spends ~80 % of its time in guest code, most of it in this decoder
// (2026-10-09: level load 3.8 s, SD reads only 0.5 s; PC sampling profile of a hub load: 64 % of the loader
// thread in sub_824FCCC8 / sub_82504740 / sub_82502BF8). Frames over 100 ms lose game time, so long loads push
// cutscene video behind the audio.
//
// The format: "LZ2K" + u32 LE decompressed size + u32 LE compressed size + data (sub_824FCE60 / sub_824FCEF8).
// The data is LHA -lh5- coded (static Huffman, 8 KB sliding dictionary): NC 510, NT 19 / TBIT 5, NP 14 / PBIT 4,
// CBIT 9, 12-bit c_table, 8-bit pt_table. The guest keeps a 32-bit bit buffer instead of LHA's 16-bit one, which
// does not change the bit stream. This is a faithful port of the guest functions:
//   sub_824FCCC8 fillbuf, sub_824FCD68 getbits, sub_824FD050 decode_start, sub_824FD0A0 make_table,
//   sub_825028A8 read_pt_len, sub_82502A20 read_c_len, sub_82502BF8 decode_c, sub_824FCF98 decode_p,
//   sub_82504740 decode (r3 data, r4 dst, r5 compressed size, r6 decompressed size; returns bytes written).
// The 8 KB ring buffer and the copy state (i, j) stay in guest memory, where the guest keeps them, and the
// decoder's global state is written back on return, so the game sees exactly what its own code would leave.
// Anything the guest would treat as an error or that would make it write out of bounds (bad table, oversized
// counts) makes the native decoder stop and run the guest function on the same block instead. Valid streams never
// get there; for a corrupt one the guest then does whatever it did before (prints "Bad table" and carries on).
//
// LSWTCS_LZ2K (default 1): 0 = always run the guest decoder.
// LSWTCS_LZ2KCHECK=1 (diagnostic): decode every block both ways, compare output and final state byte for byte,
// log mismatches as [LZ2K] MISMATCH, return the guest's result. [LZ2K] lines also report timings.
#include "../../Convert 360/LSWTCS/output/ppc_context.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" void dbg_ram(const char* fmt, ...);
extern "C" PPC_FUNC(__imp__sub_82504740);

namespace {
// Guest globals of the decoder (lis/addi pairs decoded from the guest code).
constexpr uint32_t kEnable = 0x82F1F93C, kInPtr = 0x8327EAEC, kOutPtr = 0x8327EEC0, kBitbuf = 0x8327EEC4,
                   kOrigSize = 0x83284758, kCompSize = 0x832835A4, kSubbitbuf = 0x831A2E68, kBitcount = 0x831A2E70,
                   kJ = 0x831A2604, kI = 0x8320F030, kRing = 0x8320CF48, kBlocksize = 0x83206AA0;
constexpr int NC = 510, NT = 19, NP = 14, NPT = 19, CBIT = 9, TBIT = 5, PBIT = 4;
constexpr uint32_t kDic = 8192, kDicMask = kDic - 1;

// PowerPC slw/srw: shift amounts 32..63 give 0.
inline uint32_t slw(uint32_t x, uint32_t n) { return n >= 32 ? 0 : x << n; }
inline uint32_t srw(uint32_t x, uint32_t n) { return n >= 32 ? 0 : x >> n; }

struct Bad {};   // thrown where the guest would report an error or write out of bounds

struct Lz2k {
    const uint8_t* in;
    uint32_t compsize, bitbuf = 0, subbitbuf = 0, bitcount = 0, blocksize = 0;
    uint16_t left[2 * NC - 1], right[2 * NC - 1], c_table[4096], pt_table[256];
    uint8_t c_len[NC], pt_len[NPT];

    void fillbuf(uint32_t n) {   // sub_824FCCC8
        uint32_t r6 = slw(bitbuf, n);
        if (n > bitcount) {
            do {
                n -= bitcount;
                r6 |= slw(subbitbuf, n);
                if (compsize) { --compsize; subbitbuf = *in++; } else subbitbuf = 0;
                bitcount = 8;
            } while (n > 8);
        }
        bitcount -= n;
        bitbuf = r6 | srw(subbitbuf, bitcount);
    }
    uint32_t getbits(uint32_t n) {   // sub_824FCD68
        if (!n) return 0;
        const uint32_t x = srw(bitbuf, 32 - n);
        fillbuf(n);
        return x;
    }
    void start() { bitbuf = 0; subbitbuf = 0; bitcount = 0; fillbuf(32); blocksize = 0; }   // sub_824FD050

    void make_table(uint32_t nchar, const uint8_t* bitlen, uint32_t tablebits, uint16_t* table) {   // sub_824FD0A0
        uint16_t count[17] = {}, weight[17], start[18];
        for (uint32_t i = 0; i < nchar; ++i) {
            if (bitlen[i] > 16) throw Bad{};   // the guest would index past its stack array
            ++count[bitlen[i]];
        }
        start[1] = 0;
        for (uint32_t i = 1; i <= 16; ++i) start[i + 1] = uint16_t(start[i] + (count[i] << (16 - i)));
        if (start[17] != 0) throw Bad{};   // guest: error("Bad table")
        const uint32_t jutbits = 16 - tablebits;
        for (uint32_t i = 1; i <= tablebits; ++i) { start[i] = uint16_t(start[i] >> jutbits); weight[i] = uint16_t(1u << (tablebits - i)); }
        for (uint32_t i = tablebits + 1; i <= 16; ++i) weight[i] = uint16_t(1u << (16 - i));
        uint32_t i = uint32_t(start[tablebits + 1]) >> jutbits;
        const uint32_t k_end = 1u << tablebits;
        if (i != 0 && i != k_end) {   // the guest skips the fill when this is 0 (the 16-bit start wrapped)
            if (i > k_end) throw Bad{};
            while (i != k_end) table[i++] = 0;
        }
        uint32_t avail = nchar;
        const uint32_t mask = 1u << (15 - tablebits);
        for (uint32_t ch = 0; ch < nchar; ++ch) {
            const uint32_t len = bitlen[ch];
            if (!len) continue;
            const uint32_t k = start[len], nextcode = k + weight[len];
            if (len <= tablebits) {
                if (k < nextcode) {
                    if (nextcode > k_end) throw Bad{};
                    for (uint32_t m = k; m < nextcode; ++m) table[m] = uint16_t(ch);
                }
            } else {
                uint32_t kk = k;
                uint16_t* p = &table[kk >> jutbits];   // k < 0x10000, so this stays in the table
                for (uint32_t n = len - tablebits; n; --n) {
                    if (*p == 0) {
                        if (avail >= 2 * NC - 1) throw Bad{};
                        right[avail] = left[avail] = 0;
                        *p = uint16_t(avail++);
                    }
                    if (*p >= 2 * NC - 1) throw Bad{};
                    p = (kk & mask) ? &right[*p] : &left[*p];
                    kk <<= 1;
                }
                *p = uint16_t(ch);
            }
            start[len] = uint16_t(nextcode);
        }
    }

    void read_pt_len(int nn, int nbit, int i_special) {   // sub_825028A8
        const uint32_t n = getbits(nbit);
        if (n == 0) {
            const uint32_t c = getbits(nbit);
            memset(pt_len, 0, size_t(nn));
            for (int i = 0; i < 256; ++i) pt_table[i] = uint16_t(c);
            return;
        }
        if (n > uint32_t(nn)) throw Bad{};   // the guest would write past pt_len
        int i = 0;
        while (i < int(n)) {
            uint32_t c = bitbuf >> 29;
            if (c == 7) {
                uint32_t m = 1u << 28;
                while (m & bitbuf) { m >>= 1; ++c; }
            }
            fillbuf(c < 7 ? 3 : c - 3);
            pt_len[i++] = uint8_t(c);
            if (i == i_special) {
                const uint32_t z = bitbuf >> 30;
                fillbuf(2);
                if (i + int(z) > nn) throw Bad{};
                for (uint32_t q = 0; q < z; ++q) pt_len[i++] = 0;
            }
        }
        while (i < nn) pt_len[i++] = 0;
        make_table(uint32_t(nn), pt_len, 8, pt_table);
    }

    void read_c_len() {   // sub_82502A20
        const uint32_t n = getbits(CBIT);
        if (n == 0) {
            const uint32_t c = getbits(CBIT);
            memset(c_len, 0, NC);
            for (int i = 0; i < 4096; ++i) c_table[i] = uint16_t(c);
            return;
        }
        if (n > uint32_t(NC)) throw Bad{};
        uint32_t i = 0;
        while (i < n) {
            uint32_t c = pt_table[bitbuf >> 24];
            if (c >= uint32_t(NT)) {
                uint32_t m = 1u << 23;
                do {
                    if (c >= 2 * NC - 1) throw Bad{};
                    c = (bitbuf & m) ? right[c] : left[c];
                    m >>= 1;
                } while (c >= uint32_t(NT));
            }
            fillbuf(pt_len[c]);
            if (c <= 2) {
                uint32_t z;
                if (c == 0) z = 1;
                else if (c == 1) z = getbits(4) + 3;
                else z = getbits(CBIT) + 20;
                if (i + z > uint32_t(NC)) throw Bad{};
                memset(c_len + i, 0, z);
                i += z;
            } else {
                c_len[i++] = uint8_t(c - 2);
            }
        }
        while (i < uint32_t(NC)) c_len[i++] = 0;
        make_table(NC, c_len, 12, c_table);
    }

    uint32_t decode_c() {   // sub_82502BF8
        if (blocksize == 0) {
            blocksize = getbits(16);
            read_pt_len(NT, TBIT, 3);
            read_c_len();
            read_pt_len(NP, PBIT, -1);
        }
        --blocksize;
        uint32_t j = c_table[bitbuf >> 20];
        if (j >= uint32_t(NC)) {
            uint32_t m = 1u << 19;
            do {
                if (j >= 2 * NC - 1) throw Bad{};
                j = (bitbuf & m) ? right[j] : left[j];
                m >>= 1;
            } while (j >= uint32_t(NC));
        }
        fillbuf(c_len[j]);
        return j;
    }

    uint32_t decode_p() {   // sub_824FCF98
        uint32_t j = pt_table[bitbuf >> 24];
        if (j >= uint32_t(NP)) {
            uint32_t m = 1u << 23;
            do {
                if (j >= 2 * NC - 1) throw Bad{};
                j = (bitbuf & m) ? right[j] : left[j];
                m >>= 1;
            } while (j >= uint32_t(NP));
        }
        fillbuf(pt_len[j]);
        if (j != 0) j = (1u << (j - 1)) + getbits(j - 1);
        return j;
    }
};

// Final decoder state the guest code would leave in its globals.
struct State { uint32_t in_ptr, out_ptr, bitbuf, subbitbuf, bitcount, blocksize, compsize, j, i, total; };

// sub_82504740 body. ring = the 8 KB dictionary (guest memory, or a copy in check mode); out = host pointer for dst.
// Throws Bad before writing anything that could differ from the guest (all checks precede the writes they guard).
State decode(Lz2k& d, uint32_t src, uint8_t* in_host, uint32_t dst, uint8_t* out, uint8_t* ring, uint32_t compsize,
             uint32_t origsize, uint32_t i0) {
    d.in = in_host;
    d.compsize = compsize;
    d.start();
    int32_t j = 0;
    uint32_t i = i0, total = 0;
    while (origsize != 0) {
        const uint32_t n = origsize > kDic ? kDic : origsize;
        uint32_t r = 0;
        bool full = false;
        while (--j >= 0) {
            ring[r] = ring[i];
            i = (i + 1) & kDicMask;
            if (++r == n) { full = true; break; }
        }
        while (!full) {
            const uint32_t c = d.decode_c();
            if (c <= 255) {
                ring[r] = uint8_t(c);
                if (++r == n) break;
                continue;
            }
            i = (r - d.decode_p() - 1) & kDicMask;
            j = int32_t(c) - 254;
            for (;;) {
                ring[r] = ring[i];
                i = (i + 1) & kDicMask;
                if (++r == n) { full = true; break; }
                if (--j < 0) break;
            }
        }
        memcpy(out + total, ring, n);
        total += n;
        origsize -= n;
    }
    const uint32_t consumed = compsize - d.compsize;
    return State{src + consumed, dst + total, d.bitbuf, d.subbitbuf, d.bitcount, d.blocksize, d.compsize,
                 uint32_t(j), i, total};
}

// Host pointer for [addr, addr + len) if it is contiguous in host memory (one guest view region).
uint8_t* host_span(uint8_t* base, uint32_t addr, uint32_t len) {
    if (!len) return PPC_HOST(addr);
    uint8_t* p = PPC_HOST(addr);
    return PPC_HOST(addr + len - 1) == p + (len - 1) ? p : nullptr;
}

void write_state(uint8_t* base, const State& s) {
    PPC_STORE_U32(kInPtr, s.in_ptr);
    PPC_STORE_U32(kOutPtr, s.out_ptr);
    PPC_STORE_U32(kBitbuf, s.bitbuf);
    PPC_STORE_U32(kSubbitbuf, s.subbitbuf);
    PPC_STORE_U32(kBitcount, s.bitcount);
    PPC_STORE_U32(kBlocksize, s.blocksize);
    PPC_STORE_U32(kCompSize, s.compsize);
    PPC_STORE_U32(kOrigSize, 0);
    PPC_STORE_U32(kJ, s.j);
    PPC_STORE_U32(kI, s.i);
}

int mode() {
    static const int m = [] {
        const char* e = getenv("LSWTCS_LZ2K");
        const char* c = getenv("LSWTCS_LZ2KCHECK");
        if (e && e[0] == '0') return 0;
        return (c && c[0] == '1') ? 2 : 1;
    }();
    return m;
}

std::atomic<uint64_t> g_calls{0}, g_bytes{0}, g_ns{0}, g_guest_ns{0}, g_fallbacks{0}, g_mismatches{0};
uint64_t now_ns() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
void report() {
    const uint64_t calls = g_calls.load();
    if (calls == 1 || calls % 200 == 0)
        dbg_ram("[LZ2K] %s: %llu blocks, %.1f MB out, native %.1f ms, %llu fallbacks, %llu mismatches\n",
                mode() == 2 ? "check" : "native", (unsigned long long)calls, g_bytes.load() / 1048576.0, g_ns.load() / 1e6,
                (unsigned long long)g_fallbacks.load(), (unsigned long long)g_mismatches.load());
    if (mode() == 2 && (calls == 1 || calls % 200 == 0))
        dbg_ram("[LZ2K]   guest decoder %.1f ms for the same blocks (x%.1f)\n", g_guest_ns.load() / 1e6,
                g_ns.load() ? double(g_guest_ns.load()) / double(g_ns.load()) : 0.0);
}
}  // namespace

// Strong definition: replaces the recompiled sub_82504740 (a weak alias) for every caller and the call table.
PPC_FUNC(sub_82504740) {
    const int m = mode();
    if (m == 0 || PPC_LOAD_U32(kEnable) == 0) { __imp__sub_82504740(ctx, base); return; }
    const uint32_t src = ctx.r3.u32, dst = ctx.r4.u32, compsize = ctx.r5.u32, origsize = ctx.r6.u32;
    uint8_t* in = host_span(base, src, compsize);
    uint8_t* out = host_span(base, dst, origsize);
    uint8_t* ring = host_span(base, kRing, kDic);
    if (!in || !out || !ring) { ++g_fallbacks; __imp__sub_82504740(ctx, base); return; }
    const uint32_t i0 = PPC_LOAD_U32(kI);
    static thread_local Lz2k d;
    if (m == 1) {
        // On Bad, dst and ring may hold partial output; the guest decoder then rewrites them from the start (only
        // a corrupt stream gets here, and the guest's handling of it is unchanged apart from those bytes).
        const uint64_t t0 = now_ns();
        try {
            const State s = decode(d, src, in, dst, out, ring, compsize, origsize, i0);
            write_state(base, s);
            ctx.r3.u64 = s.total;
            g_ns += now_ns() - t0;
            g_bytes += s.total;
        } catch (const Bad&) {
            ++g_fallbacks;
            dbg_ram("[LZ2K] block at 0x%08X (%u -> %u bytes): table error, running the guest decoder\n", src, compsize, origsize);
            __imp__sub_82504740(ctx, base);
        }
        ++g_calls;
        report();
        return;
    }
    // Check mode: native into copies, then the guest for real, then compare.
    static thread_local std::vector<uint8_t> out_copy, ring_copy;
    out_copy.assign(origsize, 0);
    ring_copy.assign(ring, ring + kDic);
    State s{};
    bool bad = false;
    const uint64_t t0 = now_ns();
    try { s = decode(d, src, in, dst, out_copy.data(), ring_copy.data(), compsize, origsize, i0); }
    catch (const Bad&) { bad = true; ++g_fallbacks; }
    const uint64_t t1 = now_ns();
    __imp__sub_82504740(ctx, base);
    const uint64_t t2 = now_ns();
    g_ns += t1 - t0;
    g_guest_ns += t2 - t1;
    g_bytes += origsize;
    ++g_calls;
    if (!bad) {
        const bool out_ok = memcmp(out_copy.data(), out, origsize) == 0;
        const bool ring_ok = memcmp(ring_copy.data(), ring, kDic) == 0;
        const bool st_ok = ctx.r3.u32 == s.total && PPC_LOAD_U32(kInPtr) == s.in_ptr && PPC_LOAD_U32(kOutPtr) == s.out_ptr &&
                           PPC_LOAD_U32(kBitbuf) == s.bitbuf && PPC_LOAD_U32(kSubbitbuf) == s.subbitbuf &&
                           PPC_LOAD_U32(kBitcount) == s.bitcount && PPC_LOAD_U32(kBlocksize) == s.blocksize &&
                           PPC_LOAD_U32(kCompSize) == s.compsize && PPC_LOAD_U32(kJ) == s.j && PPC_LOAD_U32(kI) == s.i;
        if (!out_ok || !ring_ok || !st_ok) {
            ++g_mismatches;
            uint32_t first = 0;
            while (first < origsize && out_copy[first] == out[first]) ++first;
            dbg_ram("[LZ2K] MISMATCH block 0x%08X (%u -> %u): output %s (first diff at %u), ring %s, state %s "
                    "(ret %u/%u in %08X/%08X bitbuf %08X/%08X j %d/%d i %u/%u)\n", src, compsize, origsize,
                    out_ok ? "ok" : "DIFF", first, ring_ok ? "ok" : "DIFF", st_ok ? "ok" : "DIFF", ctx.r3.u32, s.total,
                    PPC_LOAD_U32(kInPtr), s.in_ptr, PPC_LOAD_U32(kBitbuf), s.bitbuf, int32_t(PPC_LOAD_U32(kJ)), int32_t(s.j),
                    PPC_LOAD_U32(kI), s.i);
        }
    }
    report();
}
