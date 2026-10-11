# HANDOFF 2026-10-11: FPS and loading optimisations in the game code (investigation done, work not started)

Audience: the next agent. The user wants the Switch build (tested in Eden, played on their console) to run and load
as fast as possible. **Rules from the user:** never apply an optimisation that makes gameplay worse (a measured loss
stays opt-in or is removed); the PC build is the performance baseline, not Xenia; no cache may stand in for a slow
pipeline; ask before every SD-card copy, never delete on the SD card; one Eden at a time; never set
`LSWTCS_AUTOSTART`. Patch C/C++ with the Edit tool or Python scripts, not sed on code. Read first:
`HANDOFF_2026-10-11_switch_opt.md` (§0b–§0d: today's reverb, IRQL, tile-copy and controls work, commit 798f3de) and
its §3 tool table.

## 1. What the profiles say

Profiles: PC build, `LSWTCS_SAMPLEPROF` (1 = always, 2 = only while game files are read), 1 ms sampling of whichever
guest thread holds the turn. Raw data and symbolised reports (`*.report.txt`) are in `switch/hwlogs/2026-10-11_pcprof/`
(gitignored, local). The PC is ~5x faster than the console per thread, but where the time goes is the same code.

**In a level, steady (Episode I Ch 1, uncapped, 30,500 samples):** main thread 69.7 %, GPU-command thread (id 6,
0x822D2AB0) 21.7 %, audio 3.9 %, others small.
- Main thread: ~79 % game code, spread thin (top function 3.4 %), by region 0x823 29 %, 0x822 20 %, 0x824 20 %,
  0x821 7 %, 0x825 3 %; ~10 % our host GPU walk (XXH3 vertex hashing 4.6 %, `pm4_extract_ib` 1.5 %: the main thread
  walks the ring inside fence waits); ~9 % synchronisation (ntdll + winpthread = the cooperative scheduler).
- GPU-command thread: msvcrt (memcpy/memcmp of constant blocks) 19.6 %, `pm4_extract_ib` 20.6 %, XXH3 9.2 %, shader
  lookup 5.6 %, record/prepare ~10 %, ntdll 17 %.

**While loading (boot + hub, and the hub -> level load):** the loader thread (id 10) spends 59–67 % in our native
LZ2K `decode` (+4 % `make_table`); the PC decompresses ~260 MB/s, the console ~55 MB/s (2026-10-11: 156 MB in
2.85 s). The I/O thread (id 9) spends 19 % in the game's `memcpy`; the loader 5.6 % in a case-insensitive string
compare. The main thread mostly waits.

**Not the bottleneck:** reading. The console's SD reads 42–51 MB/s; the 360 read its disc at ~16.5 MB/s. Most of
a hub or level load is the game's own 9.0 s minimum loading screen (`sub_8220D680`, `[0x8200A108]` = 9.0): game
design, do not change without the user's say. Decoder speed still matters for streaming during play (frame spikes)
and for anything past that minimum.

## 2. Work list, ranked by expected gain per effort

**Progress (2026-10-11 s4):**
- **2.1 DONE**, `LSWTCSRuntime/source/native_crt.cpp`: memcpy, memset, _stricmp, stristr (82500958), strlen
  (824F9068), strcpy-returning-length (824F8F48). `LSWTCS_CRTCHECK=1`: 0 mismatches over 18.4 M calls (boot, hub,
  Episode I load). Load profiles: memset gone from main (was 10.5 % at boot, 5 % in the level load), memcpy 19 % ->
  ~2 % of the I/O thread. **Recomp bug found:** XenonRecomp split memset at 0x8250F130, so the recompiled one left
  the last 1-2 bytes unwritten when the tail after alignment was 2-3 bytes (hit ~once per boot); native writes all
  bytes. The ~370 MB heap clears at boot are one memset each (native now). 824FF558 is a tokenizer, not a CRT
  routine. The 1205-site stricmp's hot caller is the text-file command dispatch `sub_825034B0` (linear search of a
  {name, handler} table per line); native stricmp is single-pass, so no index was needed.
- **2.2 DONE**, `native_lz2k.cpp` restructured (see its header): offline benchmark (156 MB of real blocks, dump with
  `LSWTCS_LZ2KDUMP=<MB>` -> lz2k_dump.bin) 293 -> 469 MB/s on PC (1.6x); **Eden 3.2-3.7 s -> 1.38 s for the same
  153 MB (2.3-2.7x)**; `LSWTCS_LZ2KCHECK=1` 0 mismatches over 197 MB. Key lever: -fno-strict-aliasing means every
  output byte store could alias the decoder state, so the bit reader is a value-type copy held in locals.
  Remaining cost: per-symbol Huffman decode (~5 ns/symbol PC) and block headers (~18 %, 5.5 us each); next step
  would be multi-symbol tables.
- **2.3 DONE (applied, console test pending)**, `tools/pass_vec_locals.py`: v0-v13 promoted to locals per function,
  written back to ctx before every call / indirect call / host helper taking ctx / return, reloaded after calls
  (ctx-mode semantics, no ABI change, no liveness needed). Applied with `--min-gain 1`: 133k of 140k `ctx.v`
  references in 2479 functions (report `Convert 360/LSWTCS/veclocal_report.txt`; pre-pass copy
  `output_pre_veclocal/`). **Build step:** `patches/recomp_output_edits.patch` = output BEFORE this pass (the pass
  output would make it 27.7 MB); to reproduce `output/`, apply the patch, then run
  `python tools/pass_vec_locals.py "Convert 360/LSWTCS/output" --min-gain 1`. PC: builds and plays (user: >250 fps
  uncapped, timing fine). Eden hub, back to back: vec 17.8/20.6/27.2 and 18.7/22.9/30.2 vs items 1-2 build
  18.2/22.6/28.7 (and a noisy 22.1/27.7/41.0): no loss, gain within Eden's noise (Eden JIT-translates the ARM code,
  so register-allocation gains may only show on hardware). `eden_ab.sh` takes `NRO=<path>` for the other build.
- **2.4 SKIPPED** (user, 2026-10-11): smaller win than other work.
- Eden hub A/B (eden_ab.sh): items 1-2 both on p50 19.4 / p90 22.6 ms vs 22.4-23.2 / 25.8-26.5 in today's earlier runs;
  `LSWTCS_NATIVECRT=0` gives 19.7 / 23.4. Fewer underruns in the loading windows. Console test pending.

### 2.1 Native C-library routines (main thread FPS, loading) — do first
The game's CRT is recompiled byte loops; every byte goes through the address fold and a store.

| Guest function | What it is | Call sites | Seen in profiles |
|---|---|---|---|
| `sub_8250F2B0` | `memcpy` (290 instrs) | 2285 | 1.8 % of main in a level; 19 % of the I/O thread while loading |
| `sub_8250F0A0` | `memset` (byte head, word body) | 1745 | 10.5 % of main during boot, 5 % in the level load |
| `sub_82500A58` | case-insensitive compare (`_stricmp`, ASCII + Latin-1 fold: `cmplwi 97/122/224`) | 1205 | 5.6 % of the loader |
| `sub_82500958`, `sub_824F9068`, `sub_824F8F48`, `sub_824FF558` | byte loops, likely `strcmp` / `strlen` / `strcpy` / string search | 613 / 362 / 446 / 367 | not yet ranked |

How: a strong `PPC_FUNC(sub_XXXXXXXX)` definition replaces the recompiled weak alias (pattern:
`LSWTCSRuntime/source/native_reverb.cpp`, `native_lz2k.cpp`). Read the guest instructions (kept as comments in
`Convert 360/LSWTCS/output/ppc_recomp.*.cpp`) to confirm the exact semantics first: argument registers, the return
value (`memcpy` returns r3 = dst; the `std r3,-8(r1)` at entry saves it), overlap behaviour (is 8250F2B0 a memmove?),
and stricmp's exact case folding (it maps 224+ too). Byte copies and fills are endian-neutral, so host `memmove` /
`memset` on host pointers are exact. Guards: translate with `PPC_HOST` per contiguous region (a range may cross a
512 MB view boundary: split, see `host_span` in native_lz2k.cpp); fall back to the guest function for anything odd
(MMIO 0x7FC8xxxx, null, huge lengths). Keep `PPC_FUNC_PROLOGUE()` (GIL quantum counter). Verify: a check mode as
in native_lz2k (`LSWTCS_LZ2KCHECK`) that runs both and compares memory, then A/B in Eden (`switch/tools/eden_ab.sh`)
and a PC run. Also look at who calls the string compare 1205 times during loads: if it is a linear search of the
GAME.DAT directory per file open, a native index is a further win.

### 2.2 Faster native LZ2K decoder (loading, streaming spikes)
`LSWTCSRuntime/source/native_lz2k.cpp` is a faithful port: `fillbuf` refills one byte at a time on every symbol,
every block goes through the 8 KB ring and is then copied out, and the tree walk has a throw check per step.
Standard fixes: a 64-bit bit buffer refilled 4–7 bytes at a time; decode straight into the output buffer (copies
from the output for distances inside the block, from the ring only across the block start); keep make_table's
12-bit / 8-bit tables (or widen them). The guest-visible end state (`write_state`: in/out pointers, bitbuf, subbitbuf,
bitcount, blocksize, compsize, i, j, and the ring contents the next call continues from) must stay exactly what the
guest decoder leaves: recompute it at the end from the bit position. Verify with `LSWTCS_LZ2KCHECK=1` (0 mismatches
over a boot + hub + level load), then time `[LZ2K] native ... ms` on PC, Eden and console. Target: 2–4x on the
console.

### 2.3 Vector registers as locals in the recompiled code (main thread FPS)
`PPC_CONFIG_NON_VOLATILE_AS_LOCAL` / `NON_ARGUMENT_AS_LOCAL` made GPRs locals (the 2026-10-09 regen, +19 % Eden fps),
but the volatile vector registers still live in `ctx`: `sub_8231D470` (1516 instrs, 983 VMX, 1.8 % of main) has 1696
`ctx.vN` accesses vs 998 local ones; ~70,000 `ctx.v` lines game-wide. Each simde op loads and stores its operands
through memory. The same live-in analysis that made GPRs safe (`tools/pass_livein_regs.py`,
`HANDOFF_2026-10-09_regen.md`) extended to v0–v13 (and v64–v127 for VMX128) would let the compiler keep them in NEON
registers. Top VMX-heavy functions in a level: `sub_8231ED30` (3.4 %), `sub_8231D470` (1.8 %): animation /
skinning maths, likely. Measure with a re-recompile and Eden A/B; it touches generated code, so follow the regen
handoff's procedure and regenerate `patches/recomp_output_edits.patch`.

### 2.4 Vertex hashing (GPU-command and main threads)
XXH3 over ~1,060 vertex pages per frame (Eden hub; 4.6 % of main + 9.2 % of the GPU thread on PC, ~1.15 ms per frame
in Eden) decides which pages to re-upload. Pages of static level geometry never change after the load. Options:
(a) skip re-hashing pages that have not changed for N frames, re-checking them round-robin (a correctness trade-off:
needs the user's OK and a visual check); (b) write tracking with page protection plus the crash handler
(`platform_switch.cpp`) catching the write fault (may not work in Eden: test there first). Do not ship (a) silently.

### 2.5 Smaller items
- GPU-command thread constants: the fetch block is new on 66 % of draws (each mesh has its own vertex buffer), the
  VS float block on 60 %; viewport and system constants are recomputed per draw (could be memoised on a generation
  counter of the context registers). `LSWTCS_PREPPROF=1` gives per-stage numbers (`[PREPPROF]`, `[RECPROF]`).
- Scheduler sync is ~9 % of main on PC; on the console ~2,000 handoffs/s. Earlier experiments (fewer handoffs:
  `LSWTCS_QDIV`) starved audio; with today's IRQL fix and urgent rejoin, worth re-measuring.
- **Already tried, losses:** walking large IBs off core 0 (`LSWTCS_WALKOFF=1`, default off: the game waits on GPU
  fences every frame, so the walk's latency is in the frame); masking the fetch block (no effect). Don't repeat.

## 3. Identifying a guest function
`switch/hwlogs/2026-10-11_pcprof/*.report.txt` list the hot ones per thread. To see what one is: its instructions
(comments in the recompiled source), its callers (`grep "sub_XXXXXXXX(ctx, base)"`), strings it references, or the
Ghidra 360 project (memory: ghidra_360_oracle; `LSW360Query.java` answers "who calls / references X" in a minute).
Address regions: 0x822A–0x822D = the D3D / GPU runtime, 0x824F–0x8250 = CRT and file streaming, 0x827D–0x827F = audio.

## 4. Measuring
- PC: `LSWTCSRuntime/build/boot_to_hub.sh` (sourced; `boot_to_hub`), `switch/tools/hub_to_negotiations.sh .` from
  the build dir to reach Episode I Ch 1; `EXTRA="LSWTCS_SAMPLEPROF=1 LSWTCS_SAMPLEPROF_DUMPMS=60000 LSWTCS_FPS_CAP=0"`,
  `touch sampleprof_reset` once in place, `python sampleprof.py sampleprof.txt LSWTCSRuntime.exe 40`.
- Eden: `switch/tools/eden_ab.sh <label> [ENV=VAL ...]` (hub, 45 s: p50/p90/p99, underruns per 10 s, SCHEDPROF).
  The PC has been noisy (±15 % between runs after a restart): compare A/B pairs run back to back, not across hours.
- Console: the user runs it; logs over USB with `switch/tools/mtp.ps1` (DBI MTP responder). Ask before pushing.

## 5. Commit checklist
Regenerate `patches/recomp_output_edits.patch` if generated code changed (`diff -ruN --strip-trailing-cr
output_locals_raw_folded output` in `Convert 360/LSWTCS`); `git grep -i` for the user's name and local paths must find
nothing; commit and push only when the user asks.
