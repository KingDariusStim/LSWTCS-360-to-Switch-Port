# HANDOFF 2026-10-11: further Switch optimisation (next steps after the load-time parity work)

Audience: the next agent. Goal set by the user: the Switch build (tested in Eden, run on their console) should load and
run as fast as the **PC build**, which is the baseline. Not Xenia. **No cache may stand in for a slow pipeline** (no
Eden pipeline cache, no program-binary cache as the fix): make the work itself cheaper. Read first:
`HANDOFF_2026-10-11_loadtimes.md` (what was fixed today and how it was measured), then the standing rules in
`HANDOFF_2026-10-08_switch.md` §4–§6: ask before every SD-card copy, never delete on the SD card, one Eden at a time
(ask before closing an Eden the user has open), never set LSWTCS_AUTOSTART, patch C/C++ with the Edit tool or Python
scripts (no sed/heredoc on C).

## 0. First thing to do: read the console's `[CVTEST]` line

**RESULT (2026-10-11, console, `switch/hwlogs/2026-10-11_cvtest/`): timed waits are ACCURATE on hardware**:
`cv.wait_for(2ms) took us: 2012 2002 2003 2005 2002 | sleep_for(1ms) took 1003 us`. The instant-return theory below
is ruled out; no libnx-wait rewrite is needed. With the current build (IOWAKE with its own deadline) the level-load
peak was **21,493 switches / 5 s** (was 73,890), in line with Eden's ~28k, so the storm is gone either way. In-level
steady state on the console: IDLE ≈ 0 %, core 0 split main 54–57 % / GPU-command thread (6) 24–28 % / audio (1)
15–16 %, so work-list items 2–3 (reverb, GPU-command copying) are next. (The 61 s frame at t=214.9 s is the console
being suspended to the HOME menu, not a hang.)

The last build on the console (`sdmc:/lswtcs/lswtcs.nro`, sha256 C8036B69…, commit of this handoff) runs a one-time
self-test at the first frame and prints:

`[CVTEST] cv.wait_for(2ms) took us: a b c d e | sleep_for(1ms) took N us`

Eden prints ~2050–2180 us and ~1080 us. The user boots the console to the title, reconnects USB (DBI), and you pull
the log: `switch/tools/mtp.ps1 pull lswtcs switch/hwlogs/<dir> stdout.log trace.log` (`stdout.log` has the line).

Why it matters: during a level load on the console (2026-10-11, `switch/hwlogs/2026-10-11_levelcrash/`) the guest
scheduler did **73,890 switches in 5 s**. Earlier console builds peaked at 17–18k; Eden peaks at ~28k with the whole
load in one window. The loader (sched id 10) and the I/O thread (id 9) took turns of 9 µs and 3 µs. Theory:
libstdc++'s `condition_variable::wait_for` on newlib/libnx returns at once (the timed wait may be converted to a
wall-clock deadline, and this console's clock reads 2025-12-31), so the new `lsw_io_wait` (`LSWTCS_IOWAKE`) handed the
turn back immediately in a loop. `lsw_io_wait` now keeps its own steady-clock deadline and sleeps 100 µs when a wait
returns early, which fixes that path either way.

**If `[CVTEST]` shows near-zero waits**, the same flaw is elsewhere, and it is likely a general hardware CPU leak:
- `kw_block` (kernel_stubs.cpp, every guest `KeWaitForSingleObject`/`KeWaitForMultipleObjects` on a guest dispatcher
  object) loops `wait_for(1 ms steps)` until its steady deadline. With instant returns, a blocked guest thread
  busy-spins up to 16 ms **on core 0**, where all guest threads are pinned, stealing time from the thread that runs.
- Any other `wait_for`/`wait_until` in kernel_stubs.cpp, switch/source (audio_switch.cpp `g_cv.wait_for(20 ms)`,
  gl_backend.cpp) and the Win32 shims in `switch/source/win32_switch.cpp` (`WaitForSingleObject` timeouts).
Fix at the root: use libnx primitives with relative timeouts (`condvarWaitTimeout`, `svcWaitSynchronization`, or
`svcSleepThread`) for these waits on Switch, or a steady-clock deadline loop with a real sleep as in `lsw_io_wait`.
Then compare `[SCHEDPROF]` switches and `IDLE` on the console in the hub and during a level load, before/after.

The user also reported one crash during that load (Episode I Palace Rescue: the character loader `sub_8230C8C8`
followed a garbage pointer, `0x3CDAF63D`, from the just-loaded `captainpanaka_360.ghg`; retry loaded fine; the console
was overclocked). Handshake order of the read-ahead double buffer was checked and is unchanged by IOWAKE. If it
recurs at stock clocks, suspect a race the switch storm made more likely: diff the loaded file bytes against the
archive (`LSWTCS_LZ2KCHECK=1` checks decompression only).

## 0b. DONE 2026-10-11 (s3): reverb off core 0 + IRQL depth leak (pushed to the console SD, sha256 1CBEA489…)

- **Native reverb, fast path**: generated code now uses a per-call local copy of the address map (`Fast F`) and a
  template `S` for the diagnostic path, so byte stores no longer force reloads (`-fno-strict-aliasing`). Eden: native
  0.65 -> 0.452 ms/block (guest 0.439). `LSWTCS_REVERBCHECK=1` now checks the *shipping* instance (runs it on the real
  memory with the shadow state swapped in, then restores the guest's result): 0 mismatches / 10,000 blocks with
  `LSWTCS_REVERBTEST=1`. **Default now native** (`LSWTCS_REVERB=0` = guest).
- **Off the rotation** (`LSWTCS_REVERBOFF`, default 1): the audio thread runs the native reverb via
  `lswtcs_gil_blocking_urgent` (kernel_stubs.cpp: leaves the rotation, on return takes the next handoff and pokes
  `lswtcs_gil_ctr` so the holder yields at its next function entry); on Switch it also moves to core 2 at priority
  0x2B (`lsw_offcore_begin/end`, platform_switch.cpp; move ~8 us each way).
- **IRQL depth leak** (`LSWTCS_IRQLFIX`, default 1): `KeRaiseIrqlToDpcLevel` counted as a spinlock section but the game
  lowers IRQL mostly without `KfLowerIrql` (main: 2137 raises / 1511 lowers -> depth 626 forever), so main and the
  GPU-command thread yielded only every 9th quantum (~1.3 ms turns). Bare raises are no longer counted (spinlock
  acquire/release pairs still are; they balance).
- Eden hub, back to back (PC slower than this morning after a restart, compare within the set): default before
  p50/p90 24.9/31.2 ms, hub underruns 744-1712 per 10 s; IRQLFIX 23.9/28.1, 0-6; IRQLFIX+REVERBOFF 21.4/25.0, 3-6;
  final defaults 22.4/26.9, 1-4. Audio thread on core 0: 16 % -> 7 %. Switches/5 s 5.3k -> 11k (shorter turns).
- Diagnostics left in: `[GILURGENT]` (rejoin wait by holder; GPU-command thread still ~1.4 ms, it holds the turn in
  host work), `[SPINDBG]` (per-thread lock-call counts), `[AUDIO] ... peak` (output level; 2026-10-11 the user's
  "no audio in Eden" was Windows outputting to Steam Streaming Speakers, not the game).
- Console test needed: underruns/choppiness by ear, fps in a level, any crash (IRQLFIX removes the old atomicity of
  bare raised-IRQL sections). Item 2 (GPU-command thread) not started.

## 0c. GPU-command thread, 2026-10-11 (s3) — small win; off-core walking is a loss

- Per-draw profile (`LSWTCS_PREPPROF=1`: `[PREPPROF]` stages of the Switch `lsw_gpu_prepare`, `[RECPROF]` recorder; Eden
  hub, us per draw, ~800 draws/frame): shader lookup 0.80 (incl. clearing the 2.9 KB LswGpuDraw), constant blocks into
  the arena 0.93-1.35, record build 0.74, float packing 0.68, system constants 0.38, viewport 0.35, modifications 0.30,
  GLSL lookup 0.23. Block reuse from the previous draw: sys 85 %, VS floats 40 %, PS floats 80 %, bool 99 %, fetch 34 %.
- Done: prepare clears only the scalar fields; the draw record is built in place in `f.draws` (was a 1760 B local,
  zeroed and copied); bool/loop and fetch constants are passed straight from the register file. Record time per frame
  4.69 -> 4.49 ms, prepare 2.02 -> 1.87 ms.
- Tried, no gain: masking the fetch block to the slots the shaders use (reuse stays 34 %: the used vertex-fetch slots
  are what changes, every mesh has its own buffer). Removed.
- Tried, LOSS: `LSWTCS_WALKOFF=1` (opt-in, default off) walks large ring-level IBs (>= `LSWTCS_WALKOFF_MIN`, 4096 dwords)
  off the rotation on core 2 (`WalkNeedsTurn` around interrupts / waits; other walkers wait in `walk_wait_other`).
  Eden hub p50 23.1 ms -> 39.4 (prio 0x2B: starves the glthread worker, replay 8 -> 38 ms), 35.7 (0x2C), 43.1 (0x2D):
  the game waits on GPU fences every frame, so the walk's latency is in the frame, and on shared core 2 it ran 3-4x
  slower. Revisit only if a core has real spare time on hardware.
- Left: vertex hashing 1.15 ms/frame (~1060 pages hashed per frame; needs write tracking to skip), constants (fetch
  block new 66 % of draws), viewport/system constants recomputed per draw (could be memoised on register generations).

## 0d. 2026-10-11 (s3): characters cut off below row 320 (console, Episode I palace) — FIXED; controls swapped

- Captures `switch/hwlogs/2026-10-11_charglitch/` (shot_15031 + resdump of that frame): character colour and depth end
  at row 320 (tile 0's bottom); shadows complete. stdout: `[GLUNTILE] 388001 walk-time tile copies outside an untiled
  group dropped`. Walk-time tile skipping records later tiles' draws as copies of tile 0's; when the replay's
  plan_untile rejects the group (levels with characters: different segment structure), the copies were DROPPED (they
  carried tile 0's system constants), so nothing below tile 0 that the later tiles drew appeared.
- Fix: the copy gets its own system constants — tile 0's block with the viewport-derived fields (NDC scale/offset,
  point radius) patched by `lsw_gpu_tile_state` (gl_bridge.cc; every other register equals tile 0's by the walk's
  check) — and the replay draws such copies like any record (residency upload skipped: tile 0 uploaded the shared
  ranges). Log now says "... outside an untiled group drawn". Eden Ep I Ch 1: characters complete; the palace scene
  itself still needs a console check. Open question: why plan_untile rejects those groups (LSWTCS_GLUNTILEDBG lines).
- Controls (win32_switch.cpp, user request): Switch A/B -> 360 B/A, X/Y -> Y/X (bottom button = 360 A), right stick
  Y inverted.

## 1. Where the time goes now (Eden, Cantina hub, standing still)

Frame 19.5 ms ≈ 55 fps (PC: locked 60). p50 18.3 / p90 20.9 / p99 26.9 ms. **Guest-bound**: the GIL core (core 0)
is 100 % busy:

| Thread (sched id, entry) | Share | ms/frame | Notes |
|---|---|---|---|
| main game thread (0) | ~58 % | ~11.3 | spread thin, top function 2.4 % (PC sampler, `sampleprof.py`) |
| GPU-command thread (6, 0x822D2AB0) | ~22 % | ~4.3 | PM4 walk 1.7 + draw record 3.7 (prepare 1.6, vertex hash+copy 0.9) |
| audio render thread (1, 0x827DB318) | ~15 % | ~2.9 | **reverb `sub_827E2208` + helpers ≈ 60 % of it** |
| XAudio client worker (2), others | ~3 % | | |

Render thread (core 1) replay ~5 ms: not the limit in Eden. On hardware the picture differs (render thread and GPU
are limits in heavy scenes, see `HANDOFF_2026-10-09_glthread.md`); measure there before tuning.

Loading is at PC parity (intro ~11 s, hub ~10.5 vs 9.3 s, level ~11 vs 11 s). The rest of those loads is the game's
own **9 s minimum loading screen** (`sub_8220D680` state machine, `[0x8200A108]` = 9.0) and the skippable Episode
crawl (45 s timeout). Don't shorten them without the user's say.

## 2. Ranked work list

1. **Timed waits on hardware (§0).** Possibly the cheapest big win on the console. Measure first.
2. **Reverb off the guest core.** ~1.6 ms/frame in the Eden hub, more on hardware in levels (audio thread 10–15
   ms/frame there). Two options:
   - Hand-optimise the native port (`LSWTCSRuntime/source/native_reverb.cpp`, generated `native_reverb_gen.inc` from
     `tools/gen_native_reverb.py`): it is bit-exact but slower than the guest (0.65 vs 0.51 ms/block in Eden) because
     every load/store goes through `host()` with range checks and the per-sample "loop 2" (generated lines ~676–1348
     of `rv_827E2208`) reloads ~40 coefficients and their pointers from the stack frame every sample. Hoist the
     loop-invariant loads, keep filter states in locals across the 256-sample block, keep the arithmetic order.
     Verify with `LSWTCS_REVERBCHECK=1 LSWTCS_REVERBTEST=1` (Eden; the hub feeds the reverb silence, the test signal
     fixes that): 0 mismatches over thousands of blocks, then time `LSWTCS_REVERB=1` vs default.
   - Async: run it on a host worker (core 2) one 256-sample block behind. Its output stage builds all four outputs
     from internal buffers (wet/dry mix inside the effect), so the whole effect output would be 5.3 ms late. Needs a
     state copy strategy (parameter writes by the game, resets, instance teardown while the worker runs). It changes
     audio timing, so **ask the user before doing it**; they can't be asked to verify audio blind.
3. **GPU-command thread.** `gpu_record_draw` is host-only work done while holding the GIL. `pm4_extract_ib` also fires
   guest interrupts (`pm4_fire_interrupt`) and writes guest memory (scratch side effects), so the GIL can only be
   dropped around batches of records, not per draw (each handoff costs more than a record). Smaller wins: the
   fetch-constant block (768 B, 557 unique per frame = 417 KB) and VS constants (312 KB) are copied into the arena
   every frame; uploading only the slots a shader uses would cut copying and hashing. 2026-10-11 already did the
   type-0 register fast path (walk 1.95 → 1.72 ms).
4. **Scheduler handoff cost** (opt-in experiments, results in the load-time handoff §3.4): fewer handoffs
   (`LSWTCS_QDIV=4`) buy ~1 ms/frame but starve audio; `LSWTCS_AUDIOPRIO` fixes audio but brings the handoffs back;
   `LSWTCS_SCHEDWAKE1` (wake only the next thread) changed nothing in Eden. Revisit after §0: if waits spin on the
   console, the handoff numbers there are distorted.
5. **Eden-only stalls** (not hardware): ~90–100 ms per new GL program at first draw = Eden building host pipelines
   (Mesa precompile changes nothing). Eden's disk cache is off for a directly launched NRO (title ID 0). Per user
   rule, no cache work. Fewer distinct programs/states would help both Eden and the console's first-use stalls.

## 3. Tools (all in the tree)

| Tool | Use |
|---|---|
| `switch/tools/eden_ab.sh <label> [ENV=VAL ...]` | one Eden hub run with temporary env lines (restored after); prints hub p50/p90/p99, audio underruns per 10 s, last `[SCHEDPROF]`; logs in `switch/hwlogs/eden_ab/<label>/` |
| `switch/tools/hub_to_negotiations.sh <press_now dir>` | walks from the hub start to Episode I Chapter 1 (works on PC and Eden; NPCs can push the player off: check `shot_now`, correct with `w:`/`n:` presses) |
| `LSWTCS_SWAPCHAIN=1` | guest call chain of the presenting thread when it changes + loading-loop state words, `[SWAPCHAIN]` in trace.log |
| `LSWTCS_DELAYLOG=1` | guest sleeps per caller, `[DELAY]` every 5 s |
| `LSWTCS_FNLOG=<hex,...>` | function entry/return to trace.log with ms timestamps (list in main.cpp `FNLOG_LIST`) |
| `LSWTCS_SAMPLEPROF=1` + `LSWTCS_SAMPLEPROF_DUMPMS` + `sampleprof_reset` | PC sampler per guest thread; `LSWTCSRuntime/build/sampleprof.py` |
| `LSWTCSRuntime/build/sample_main.sh`, `sample_all.sh` | cdb stack samples of the PC exe (symbolised, inline frames) — build dir, not tracked |
| `[SCHEDPROF]`, `[GPUPROF]`, `[GLPROF]`, `frames.log` | existing per-thread / per-frame profiles (see earlier handoffs) |

PC baseline: `LSWTCSRuntime/build` (clang/Ninja, `"C:/msys64/mingw64/bin/ninja.exe" -j6`), `boot_to_hub.sh`
(`launch_game`, `draws`). Screenshots: PC `shot_now` → `build/shots/latest.png`; Eden `sdmc:/lswtcs/shot_now` →
`shot_<n>.png` (readable with the Read tool, used to steer the player).

## 4. Commit hygiene
Regenerate `patches/recomp_output_edits.patch` (`diff -ruN --strip-trailing-cr output_locals_raw_folded output` in
`Convert 360/LSWTCS`) and, if xenia-canary changed, `patches/xenia-canary.patch`; `git grep -i <user name>` must find
nothing; commit only when the user asks. Generated-code edits are applied by scripts in `tools/` (rerun after a
recompiler regen: see HANDOFF_2026-10-09_regen.md §6, then `fix_switch_error_returns.py`, `fix_switch_midjumps.py`,
`fix_switch_gate.py`, `fix_ftfloor_sites.py`, `fix_io_wake.py`).
