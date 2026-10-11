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
// does not change the bit stream. Guest functions covered:
//   sub_824FCCC8 fillbuf, sub_824FCD68 getbits, sub_824FD050 decode_start, sub_824FD0A0 make_table,
//   sub_825028A8 read_pt_len, sub_82502A20 read_c_len, sub_82502BF8 decode_c, sub_824FCF98 decode_p,
//   sub_82504740 decode (r3 data, r4 dst, r5 compressed size, r6 decompressed size; returns bytes written).
// Table building is a faithful port. Decoding is restructured for speed (2026-10-11, 1.6x the faithful port on
// PC, offline benchmark on 156 MB of real blocks): a 64-bit bit buffer refilled 8 bytes at a time, kept in
// registers in the hot loops; symbol and code length in one table entry; output written straight to dst instead of
// through the 8 KB ring. The ring, the copy state (i, j) and the bit-reader globals are then rebuilt to exactly what
// the guest code leaves (checked with LSWTCS_LZ2KCHECK, 0 mismatches over boot, hub and an Episode I load).
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
#include <cstdio>
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

// PowerPC srw: shift amounts 32..63 give 0.
inline uint32_t srw(uint32_t x, uint32_t n) { return n >= 32 ? 0 : x >> n; }

struct Bad {};   // thrown where the guest would report an error or write out of bounds

// Bit reader. The guest keeps bitbuf = the next 32 bits of the stream plus the last byte read (subbitbuf) and how
// many of its bits are unread (bitcount), reading one byte at a time; bytes past the compressed size read as 0.
// Here a 64-bit buffer is refilled up to 8 bytes at a time; bitbuf is its top 32 bits, the same value. The guest's
// subbitbuf / bitcount / input pointer follow from the bit position at the end (final). A plain value type, so the
// decode loop can keep a copy in registers (stores to the output, a uint8_t*, would otherwise force reloads).
struct Bits {
    const uint8_t* in;
    uint32_t insize, loaded, nbits;   // loaded = bytes put into bb (zeros past insize); nbits = valid bits, >= 32
    uint64_t bb;

    __attribute__((always_inline)) uint32_t peek32() const { return uint32_t(bb >> 32); }
    __attribute__((noinline)) void refill_tail() {
        while (nbits <= 56) {
            const uint64_t b = loaded < insize ? in[loaded] : 0;
            bb |= b << (56 - nbits);
            ++loaded;
            nbits += 8;
        }
    }
    __attribute__((always_inline)) void refill() {   // to >= 57 valid bits
        if (__builtin_expect(loaded + 8 <= insize, 1)) {
            uint64_t v;
            memcpy(&v, in + loaded, 8);
            bb |= __builtin_bswap64(v) >> nbits;   // bits past the counted bytes are the right future bits too
            const uint32_t bytes = (63 - nbits) >> 3;
            loaded += bytes;
            nbits += bytes * 8;
        } else {
            refill_tail();
        }
    }
    __attribute__((always_inline)) void drop(uint32_t n) {   // n <= 32
        bb <<= n;
        nbits -= n;
        if (nbits < 32) refill();
    }
    __attribute__((always_inline)) void fill(uint32_t n) {   // sub_824FCCC8 (read_pt_len can ask for up to 36 bits on a bad stream)
        while (__builtin_expect(n > 32, 0)) { drop(32); n -= 32; }
        drop(n);
    }
    __attribute__((always_inline)) uint32_t get(uint32_t n) {   // sub_824FCD68
        if (!n) return 0;
        const uint32_t x = srw(peek32(), 32 - n);
        fill(n);
        return x;
    }
    void start(const uint8_t* src, uint32_t size) { in = src; insize = size; loaded = 0; nbits = 0; bb = 0; refill(); }
    // The guest's reader state here: P bits consumed, L = ceil((P + 32) / 8) bytes read.
    void final(uint32_t* subbitbuf, uint32_t* bitcount, uint32_t* consumed) const {
        const uint64_t p = uint64_t(loaded) * 8 - nbits;
        const uint64_t l = (p + 32 + 7) / 8;
        *bitcount = uint32_t(l * 8 - p - 32);
        *consumed = uint32_t(l < insize ? l : insize);
        *subbitbuf = l <= insize ? in[l - 1] : 0;
    }
};

struct Lz2k {
    Bits bits;
    uint32_t blocksize = 0;
    uint16_t left[2 * NC - 1], right[2 * NC - 1], c_table[4096], pt_table[256];
    uint8_t c_len[NC], pt_len[NPT];
    // Decode tables with the code length packed in: symbol | length << 16 (length 0 = walk the tree). Saves the
    // dependent c_len / pt_len load per symbol. Built after each block header.
    uint32_t c_fast[4096], p_fast[256];

    uint32_t bitbuf() const { return bits.peek32(); }
    void start(const uint8_t* src, uint32_t size) { bits.start(src, size); blocksize = 0; }   // sub_824FD050

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

    // The header readers work on a local copy of the bit reader (in registers; the table stores could alias it).
    void read_pt_len(int nn, int nbit, int i_special) { Bits b = bits; read_pt_len(b, nn, nbit, i_special); bits = b; }
    void read_c_len() { Bits b = bits; read_c_len(b); bits = b; }

    __attribute__((always_inline)) void read_pt_len(Bits& b, int nn, int nbit, int i_special) {   // sub_825028A8
        const uint32_t n = b.get(nbit);
        if (n == 0) {
            const uint32_t c = b.get(nbit);
            memset(pt_len, 0, size_t(nn));
            for (int i = 0; i < 256; ++i) pt_table[i] = uint16_t(c);
            return;
        }
        if (n > uint32_t(nn)) throw Bad{};   // the guest would write past pt_len
        int i = 0;
        while (i < int(n)) {
            uint32_t c = b.peek32() >> 29;
            if (c == 7) {
                uint32_t m = 1u << 28;
                while (m & b.peek32()) { m >>= 1; ++c; }
            }
            b.fill(c < 7 ? 3 : c - 3);
            pt_len[i++] = uint8_t(c);
            if (i == i_special) {
                const uint32_t z = b.peek32() >> 30;
                b.fill(2);
                if (i + int(z) > nn) throw Bad{};
                for (uint32_t q = 0; q < z; ++q) pt_len[i++] = 0;
            }
        }
        while (i < nn) pt_len[i++] = 0;
        make_table(uint32_t(nn), pt_len, 8, pt_table);
    }

    __attribute__((always_inline)) void read_c_len(Bits& b) {   // sub_82502A20
        const uint32_t n = b.get(CBIT);
        if (n == 0) {
            const uint32_t c = b.get(CBIT);
            memset(c_len, 0, NC);
            for (int i = 0; i < 4096; ++i) c_table[i] = uint16_t(c);
            return;
        }
        if (n > uint32_t(NC)) throw Bad{};
        uint32_t i = 0;
        while (i < n) {
            uint32_t c = pt_table[b.peek32() >> 24];
            if (c >= uint32_t(NT)) {
                uint32_t m = 1u << 23;
                do {
                    if (c >= 2 * NC - 1) throw Bad{};
                    c = (b.peek32() & m) ? right[c] : left[c];
                    m >>= 1;
                } while (c >= uint32_t(NT));
            }
            b.fill(pt_len[c]);
            if (c <= 2) {
                uint32_t z;
                if (c == 0) z = 1;
                else if (c == 1) z = b.get(4) + 3;
                else z = b.get(CBIT) + 20;
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

    __attribute__((noinline)) void read_block_header() {   // start of sub_82502BF8 when blocksize == 0
        blocksize = bits.get(16);
        read_pt_len(NT, TBIT, 3);
        read_c_len();
        read_pt_len(NP, PBIT, -1);
        for (int k = 0; k < 4096; ++k) { const uint32_t c = c_table[k]; c_fast[k] = c < uint32_t(NC) ? c | uint32_t(c_len[c]) << 16 : c; }
        for (int k = 0; k < 256; ++k) { const uint32_t c = pt_table[k]; p_fast[k] = c < uint32_t(NP) ? c | uint32_t(pt_len[c]) << 16 : c; }
    }
};

// Final decoder state the guest code would leave in its globals.
struct State { uint32_t in_ptr, out_ptr, bitbuf, subbitbuf, bitcount, blocksize, compsize, j, i, total; };

// sub_82504740 body. ring = the 8 KB dictionary (guest memory, or a copy in check mode); out = host pointer for dst.
// The guest decodes 8 KB chunks into ring[0..] and copies each chunk out; a match reads ring[(r - p - 1) & 8191],
// which is always output byte (pos - p - 1): in this call's output, or for the first 8 KB before it, the ring as
// the previous call left it (ring[pos - p - 1 + 8192], a slot the guest has not overwritten yet). So this decodes
// straight into out, reading the ring only for those, and rebuilds the ring and the copy state (i, j) at the end.
// out must not overlap the input or the ring (the caller checks). Throws Bad before writing the ring or the state.
State decode(Lz2k& d, uint32_t src, uint8_t* in_host, uint32_t dst, uint8_t* out, uint8_t* ring, uint32_t compsize,
             uint32_t origsize, uint32_t i0) {
    d.start(in_host, compsize);
    // Guest copy state at the end: j = bytes of the last match still to copy (0 if it ended on the last byte),
    // -1 if the last byte was a literal, 0 if nothing was decoded; i = ring index of the next match source byte.
    int32_t j = origsize ? -1 : 0;
    uint32_t i = i0, pos = 0, bs = d.blocksize;
    Bits b = d.bits;   // local copy: stays in registers across the output stores
    while (pos < origsize) {
        // decode_c (sub_82502BF8)
        if (__builtin_expect(bs == 0, 0)) {
            d.bits = b;
            d.read_block_header();
            b = d.bits;
            bs = d.blocksize;
        }
        --bs;
        uint32_t c = d.c_fast[b.peek32() >> 20];
        if (__builtin_expect(c < 0x10000u, 0)) {   // no length packed: c is a tree node (or a 0-length symbol)
            c &= 0xFFFF;
            const uint32_t bitbuf = b.peek32();
            uint32_t m = 1u << 19;
            while (c >= uint32_t(NC)) {
                if (c >= 2 * NC - 1) throw Bad{};
                c = (bitbuf & m) ? d.right[c] : d.left[c];
                m >>= 1;
            }
            c |= uint32_t(d.c_len[c]) << 16;
        }
        b.drop(c >> 16);
        c &= 0xFFFF;
        if (c <= 255) {
            out[pos++] = uint8_t(c);
            j = -1;
            continue;
        }
        // decode_p (sub_824FCF98)
        uint32_t p = d.p_fast[b.peek32() >> 24];
        if (__builtin_expect(p < 0x10000u, 0)) {
            p &= 0xFFFF;
            const uint32_t bitbuf = b.peek32();
            uint32_t m = 1u << 23;
            while (p >= uint32_t(NP)) {
                if (p >= 2 * NC - 1) throw Bad{};
                p = (bitbuf & m) ? d.right[p] : d.left[p];
                m >>= 1;
            }
            p |= uint32_t(d.pt_len[p]) << 16;
        }
        b.drop(p >> 16);
        p &= 0xFFFF;
        if (p > 1) {   // getbits(p - 1); p <= 13
            const uint32_t extra = b.peek32() >> (33 - p);
            b.drop(p - 1);
            p = (1u << (p - 1)) + extra;
        }
        const uint32_t dist = p + 1;   // <= 8192
        const uint32_t len = c - 253, k = len < origsize - pos ? len : origsize - pos;
        uint8_t* dp = out + pos;
        if (pos >= dist) {
            const uint8_t* sp = dp - dist;
            if (dist >= 8 && pos + len + 8 <= origsize) {
                // 8 bytes at a time; may write up to 7 bytes past the match, which later output overwrites
                for (uint32_t t = 0; t < len; t += 8) { uint64_t v; memcpy(&v, sp + t, 8); memcpy(dp + t, &v, 8); }
            } else {
                for (uint32_t t = 0; t < k; ++t) dp[t] = sp[t];
            }
        } else {
            for (uint32_t t = 0; t < k; ++t) {
                const uint32_t q = pos + t;   // source = output byte q - dist, before this call's output if q < dist
                dp[t] = q >= dist ? out[q - dist] : ring[q + kDic - dist];
            }
        }
        pos += k;
        j = int32_t(len - k);
        i = (pos - dist) & kDicMask;
    }
    d.bits = b;
    d.blocksize = bs;
    // Ring as the guest leaves it: the last (partial) chunk at its start, the chunk before it after that; bytes
    // from before this call stay where they are.
    if (origsize) {
        const uint32_t n_last = origsize - ((origsize - 1) & ~kDicMask), cs_last = origsize - n_last;
        memcpy(ring, out + cs_last, n_last);
        if (cs_last >= kDic) memcpy(ring + n_last, out + cs_last - kDic + n_last, kDic - n_last);
    }
    uint32_t subbitbuf, bitcount, consumed;
    d.bits.final(&subbitbuf, &bitcount, &consumed);
    return State{src + consumed, dst + origsize, d.bitbuf(), subbitbuf, bitcount, d.blocksize, compsize - consumed,
                 uint32_t(j), i, origsize};
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
    // decode() writes out as it goes, so out must not overlap the input or the ring (the guest only writes out
    // after each 8 KB chunk).
    auto overlaps = [](const uint8_t* a, uint32_t na, const uint8_t* b, uint32_t nb) { return a < b + nb && b < a + na; };
    if (!in || !out || !ring || overlaps(out, origsize, in, compsize) || overlaps(out, origsize, ring, kDic)) {
        ++g_fallbacks;
        __imp__sub_82504740(ctx, base);
        return;
    }
    const uint32_t i0 = PPC_LOAD_U32(kI);
    static thread_local Lz2k d;
    // LSWTCS_LZ2KDUMP=<MB> (diagnostic): append the first <MB> of blocks to lz2k_dump.bin for an offline benchmark
    // (records: u32 compsize, origsize, i0, 8 KB ring, compressed data).
    static const uint32_t dump_limit = [] { const char* e = getenv("LSWTCS_LZ2KDUMP"); return e ? uint32_t(atoi(e)) << 20 : 0u; }();
    static uint32_t dumped = 0;
    if (dumped < dump_limit) {
        if (FILE* f = fopen("lz2k_dump.bin", dumped ? "ab" : "wb")) {
            const uint32_t hdr[3] = {compsize, origsize, i0};
            fwrite(hdr, 4, 3, f); fwrite(ring, 1, kDic, f); fwrite(in, 1, compsize, f);
            fclose(f);
        }
        dumped += compsize;
    }
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
