// Scan a guest-memory dump for BE u32 values pointing into the six script
// native-command veneer ladders (16-aligned to an entry). Reports each hit's
// guest address, ladder, command index; plus a per-1MB cluster histogram.
//   ladscan <dump.bin> <dump_guest_base_hex> [target_lo_hex target_hi_hex]
// With the optional pair, scans instead for BE pointers into [lo, hi) — used to
// find anchor globals pointing at discovered structures.
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define NLAD 12
static const uint32_t BASES[NLAD] = {
    0x82AEFBB4, 0x82D345DC, 0x82D4458C, 0x82DB20C8, 0x82E10EAC, 0x82E581DC,
    0x82D83204, 0x82DC3ED4, 0x82DD890C, 0x82E263E0, 0x82E338E4, 0x82E43740 };
#define NCMD 1669u

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s dump.bin base_hex [lo hi]\n", argv[0]); return 1; }
    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror("open"); return 1; }
    uint64_t base = strtoull(argv[2], 0, 16);
    uint64_t lo = 0, hi = 0;
    int anchor = 0;
    if (argc >= 5) { lo = strtoull(argv[3], 0, 16); hi = strtoull(argv[4], 0, 16); anchor = 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* buf = malloc(sz);
    fread(buf, 1, sz, f); fclose(f);
    static uint32_t hist[4096]; // per-1MB buckets of hits
    static uint32_t cmdhist[NCMD];
    uint64_t hits = 0; int shown = 0;
    for (long i = 0; i + 4 <= sz; i += 4) {
        uint32_t v = (buf[i] << 24) | (buf[i+1] << 16) | (buf[i+2] << 8) | buf[i+3];
        uint64_t ga = base + i;
        if (anchor) {
            if (v >= lo && v < hi) {
                hits++;
                if (shown < 60) { printf("ANCHOR ptr at %08llX -> %08X\n", (unsigned long long)ga, v); shown++; }
            }
            continue;
        }
        for (int L = 0; L < NLAD; L++) {
            uint32_t b = BASES[L];
            if (v >= b && v < b + NCMD * 16 && ((v - b) & 15) == 0) {
                // skip self-hits inside any ladder's own code bytes
                int self = 0;
                for (int M = 0; M < NLAD; M++)
                    if (ga >= BASES[M] && ga < BASES[M] + NCMD * 16) { self = 1; break; }
                if (self) break;
                hits++;
                uint32_t idx = (v - b) / 16;
                cmdhist[idx]++;
                uint32_t bucket = (uint32_t)(i >> 20);
                if (bucket < 4096) hist[bucket]++;
                if (shown < 40) { printf("HIT at %08llX -> L%d cmd=%u\n", (unsigned long long)ga, L+1, idx); shown++; }
                break;
            }
        }
    }
    printf("total hits: %llu\n", (unsigned long long)hits);
    if (!anchor) {
        printf("cluster histogram (per-1MB buckets with >=8 hits):\n");
        for (int bkt = 0; bkt < 4096; bkt++)
            if (hist[bkt] >= 8)
                printf("  guest %08llX..: %u hits\n", (unsigned long long)(base + ((uint64_t)bkt << 20)), hist[bkt]);
        printf("top commands by refs:\n");
        for (int pass = 0; pass < 15; pass++) {
            uint32_t best = 0, bi = 0;
            for (uint32_t c = 0; c < NCMD; c++) if (cmdhist[c] > best) { best = cmdhist[c]; bi = c; }
            if (!best) break;
            printf("  cmd %u: %u refs\n", bi, best);
            cmdhist[bi] = 0;
        }
        printf("cmd1213 refs: (see above if listed)\n");
    }
    return 0;
}
