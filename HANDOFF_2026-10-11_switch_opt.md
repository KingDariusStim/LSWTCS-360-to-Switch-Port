# HANDOFF 2026-10-11: further Switch optimisation (next steps after the load-time parity work)

Audience: the next agent. Goal set by the user: the Switch build (tested in Eden, run on their console) should load and
run as fast as the **PC build**, which is the baseline. Not Xenia. **No cache may stand in for a slow pipeline** (no
Eden pipeline cache, no program-binary cache as the fix): make the work itself cheaper. Read first:
`HANDOFF_2026-10-11_loadtimes.md` (what was fixed today and how it was measured), then the standing rules in
`HANDOFF_2026-10-08_switch.md` §4–§6: ask before every SD-card copy, never delete on the SD card, one Eden at a time
(ask before closing an Eden the user has open), never set LSWTCS_AUTOSTART, patch C/C++ with the Edit tool or Python
scripts (no sed/heredoc on C).

## 0. First thing to do: read the console's `[CVTEST]` line

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
