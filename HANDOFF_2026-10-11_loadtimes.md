# HANDOFF 2026-10-11 (s2): Switch/Eden load times and frame pacing vs the PC build

Audience: the user and the next agent. Task (user, 2026-10-11): make the Switch build in Eden load and run as fast as the
**PC build** (the baseline; Xenia was dropped as the oracle). Litmus test: boot → title → hub, and audio/video sync in
the non-playable states (intro, title, loading screens). User rules for this task: **no caching to make up for a slow
pipeline** (no Eden pipeline cache, no program-binary cache as the fix); optimise the pipeline itself. Standing rules from
`HANDOFF_2026-10-08_switch.md` §4–§6 still apply (ask before SD-card copies, never delete on the SD card, one Eden at a
time, never set LSWTCS_AUTOSTART, Python/Edit tool for C edits).

## 1. Results (same flow on both: launch → title → START, A → Cantina hub)

| Phase | PC build | Eden before (11 Oct, 01:58 build) | Eden now |
|---|---|---|---|
| Boot → title level (intro card) | ~55 s before, **~11 s** now | 73 s after Eden launch (~55 s intro) | **27–28 s** after Eden launch (~11 s intro + Eden start-up) |
| Game clock during the intro | x0.18 before, **x1.000** now | x0.18 | **x1.000** |
| Title → hub loading screen | 14 s before, **~9.3 s** now (game's own 9 s minimum, §2.3) | ~12.5 s | **~10.5 s** |
| Hub entry stall | none | 7.8 s + 8.2 s (diagnostic auto-capture) + emulator pipeline builds | ~4 s, all emulator pipeline builds (§3) |
| Hub steady frame time | 16.7 ms (60 fps, locked) | ~19.5 ms | p50 18.3, p90 20.9, p99 26.9 ms (~55 fps) |

The console's SD card (`sdmc:/lswtcs/lswtcs.nro`) has the 10:48 build with every fix below (user-requested push,
sha256 verified; earlier builds kept as `lswtcs_prev_*.nro`).

## 2. Fixes (all in this session, uncommitted)

### 2.1 Diagnostic camera auto-capture now opt-in (`LSWTCS_CAMCAPTURE=1`)
`switch/source/gl_backend.cpp` (frames.log camera probe, added for the Kamino camera hunt): a > 40° camera turn
captured the frame (draw log, PNG, GLSL dump). Every hub/level entry is a camera cut, so every load paid it: Eden hub
entry 7.8 s replay + 8.2 s swap, game clock lost the same. frames.log itself stays on.

### 2.2 Boot intro ran at 18 % speed: FTFLOOR overwrote the game's nominal frame length
- `[0x830AA980]` is the game's minimum frame time (1/60 in the image, 1/30 after `sub_8229D8B8`). The FTFLOOR boot patch
  (kernel_stubs.cpp VdSwap, for cutscene A/V sync at 60 fps) overwrote it with 1 ms.
- But the game also uses it as its nominal frame length: the boot intro `sub_82264AA8` clamps each frame to
  `[floor, 3 × floor]` → cap 3 ms → its timers ran at x0.18 → intro ~55 s instead of ~10 s (PC and Eden alike;
  `[FTSTAT] ft_now=0.00300`). `sub_8242F728`/`sub_8242FA28` also convert frame counts to seconds with it (were 16–33×
  too short), and `sub_8247E5E8` (level loader) received 1 ms as a parameter.
- Fix: the word keeps the game's value; only the two **minimum clamps** (main loop `sub_82215AA0` at 0x82216B90, intro
  at 0x822650A8) call `lsw_ft_min(floor)` (1 ms when FTFLOOR is on). Generated-code edits by
  `tools/fix_ftfloor_sites.py` (marker `LSW_FTFLOOR`, ppc_recomp.219/.286.cpp); runtime in kernel_stubs.cpp
  (`lsw_ftfloor_on`, `lsw_ft_min`). The K=0 frame-timer snap is unchanged. `LSWTCS_FTFLOOR=0` = game's clamps.
- Verified: `[FTSTAT] x1.000` from the first second on PC and Eden; title at ~11 s on both.

### 2.3 Loader slept ~2 ms per file block: `LSWTCS_IOWAKE` (default on)
- File streaming: I/O thread `sub_824FD9A8` reads each block (12-byte LZ2K header + data, 32 KB slots) and sets flag
  `[0x83284760]`; the loader (`sub_825048C8`; also `sub_824F6F10`) polls the flag with `Sleep(1)`. On the 360 the disc
  read was slower than the poll; here the read was done long before, yet each block cost ≥ 1 host sleep.
  PC hub load: loader asleep 4.8 s of every 5 s (`[DELAY] lr=82504960`, ~500 sleeps/s), process 90 % idle.
- Fix: the poll sites call `lsw_io_wait(base)` (off the GIL rotation, host condition variable, 2 ms cap) and the two
  places that set the flag call `lsw_io_flag_set()`. `tools/fix_io_wake.py` (marker `LSW_IOWAKE`,
  ppc_recomp.415/.417.cpp); runtime in kernel_stubs.cpp next to `GilYield`. `LSWTCS_IOWAKE=0` = guest Sleep(1).
- PC title→hub loading screen 14 → ~9.3 s; Eden 12.5 → ~10.5 s. Applies to every streamed load (levels too).
- **The remaining ~9 s is the game's own design**, not port overhead: the loading-screen loop in `sub_8220D680`
  (head 0x8220DDC0) runs a state machine (`[r1+112]`): state 0 → 1 when `[0x82FCC290] == 2` (or both
  `sub_823C1808` checks), state 1 ramps `f23 += frame time` up to **9.0 s** (`f16` = `[0x8200A108]`), state 2 counts
  the 0.1 s exit timer `[0x82FCC2F0]` down; exit needs gate `[0x82E7E980] != 0`. The load itself finishes in ~1 s on PC
  (all hub files read in 0.5 s). Shortening it would change game behaviour: not done; ask the user if wanted.

### 2.4 Walker: register fast path (GPU-command thread)
`pm4_extract_ib` type-0 packets (thousands of constant registers per frame) now block-copy with byte swap and call
`regset_note`/`pm4_reg_side_effect` only for the ranges where they act (0x578–0x57F, 0x2000–0x23FF, 0x4800–0x4805).
Identical results. Eden hub walk (excluding record) 1.95 → 1.72 ms/frame.

### 2.4b Pause menu showed a stale Cantina frame mid-level (user report, fixed)
The pause menu draws a snapshot of the current frame (a one-shot resolve, sampled only afterwards). Dead-work skipping
(`LSWTCS_GLDEADSKIP`, gl_rt.inc `dest_dead`) treated its destination as dead (first resolved > 120 replays ago, not
sampled since: last used in the hub's area transition), so the copy was skipped and the menu showed that old frame.
Now a destination can only be dead if it was also resolved within the last 2 replays (a chain repeated every frame,
like the hub's unused blur chain); `g_dest_last_resolve` is updated in `plan_dead_work` after the decisions. Eden:
pause in the hub and mid-level in Negotiations shows the current scene; the hub's 9 blur copies are still skipped.
**On the console's SD card** (10:48 build, sha256 3D5A2473…, previous kept as `lswtcs_prev_20261011_105313.nro`).

### 2.5 PC build fixes
- `near` lambda renamed (`close_to`, kernel_stubs.cpp): a Windows macro; the PC build did not compile.
- **Blue tint on PC** (user report) = the resolve `copy_dest_swap` fix existed only in the Switch GL path. Ported to
  `LSWTCSRuntime/source/gpu_xedraw.inc`: `XdResolved::swap` set per resolve, carried through `XdTexSrc`, applied in
  `xd_resolved_swizzle` (R/B source exchange in the fetch swizzle). User confirmed: tint gone.
- PC build dir `LSWTCSRuntime/build` re-configured (clang, Ninja); old exe kept as `LSWTCSRuntime_1008.exe`.

## 3. What is left between Eden and PC

1. **Hub entry hitches in Eden (~4 s) and occasional 2–3 s stalls** (`gpuwait` 2.1 s / 0.86 s / 3.0 s with tiny replay;
   one 575 ms replay in `state` = glthread queue full behind the worker): the emulated GPU stalls while **Eden builds
   host pipelines** for new state combinations. Evidence: each program's first draw costs ~90–100 ms in Eden
   (`LSWTCS_GLTIMING=1`, 60 programs ≈ 5.4 s), unchanged with `ST_DEBUG=precompile` (Mesa variant compile is not it).
   Eden's own pipeline cache never fills for this NRO: Eden logs title ID 0000000000000000 for a directly launched NRO
   (disk cache disabled; `%APPDATA%/eden/shader/054c535443530000/vulkan_pipelines.bin` = 64-byte header since 9 Oct).
   Not a hardware cost. Per user rule no cache work; Eden's `use_asynchronous_shaders` is an Eden setting (ask first).
2. **Hub ~55 fps in Eden** (PC 60): guest-bound on the GIL core, 0 % idle. Per frame (Eden): main game thread ~11.3 ms
   (spread thin, top function 2.4 %), GPU-command thread ~4.3 ms (walk 1.7 + record 3.7: prepare 1.6, vertex
   hash+copy 0.9), audio thread ~2.9 ms (**reverb `sub_827E2208` + helpers ≈ 60 % of it**). Levers, largest first:
   - Reverb: native port is bit-exact but not faster (`native_reverb.md`). Options: (a) hoist loop-invariant
     coefficient loads / remove per-access range checks in the generated loop 2, verify with `LSWTCS_REVERBCHECK=1`
     (+`LSWTCS_REVERBTEST=1`, the hub feeds it silence); (b) run it on a host worker one block behind — but its final
     stage mixes into the dry signal already in the output buffers (`ldv` from the output pointers before `stv`), so the
     wet part would have to be split out first.
   - GPU-command thread off the GIL: `gpu_record_draw` is host-only, but `pm4_extract_ib` also fires guest interrupts
     (`pm4_fire_interrupt`, line ~105 of the function) and writes guest memory (scratch side effects), so the GIL can
     only be dropped around record batches. Per-draw GIL handoffs would cost more than they save.
3. **Level loads (measured, Episode I Chapter 1 Negotiations, Story):** PC and Eden match: after START on the opening
   crawl, the loading screen holds ~10 s (same `sub_8220D680` loop, 9 s minimum), cutscene at +11 s on both. Before
   that, Story mode shows the Episode crawl card ("Press START", skippable; it times out after **45 s** =
   `[0x82E7E988]`, the r25==2 path of the same loop: exit on a button press or 45 s). All level files load in ~1 s.
   Scripted walk from the hub start: `switch/tools/hub_to_negotiations.sh <press_now dir>` (NPCs can push the player
   off the path; check a screenshot, the Chapter 1 door is the left round door in the Episode I corridor).
4. **Scheduler experiments (Eden hub, all opt-in, defaults unchanged):**

   | Config | p50 / p90 ms | underruns per 10 s (steady hub) | switches / 5 s |
   |---|---|---|---|
   | default | 18.3 / 20.9 | 10 / 3 / 9 | ~6,400 |
   | `LSWTCS_QDIV=4` | 17.0 / 19.3 | 403 in the load window, 57 / 89 / 85 | 4,680 |
   | `QDIV=4` + `LSWTCS_AUDIOPRIO=2` (audio chain chosen first) | 17.5 / 19.7 | 23 / 21 / 13 | 5,340 |
   | `QDIV=2` + `AUDIOPRIO=1` (also cuts the divisor short) | 18.6 / 21.3 | 6 / 4 / 0 | 7,080 |
   | `LSWTCS_SCHEDWAKE1=1` (wake only the next thread) | 18.2 / 21.0 | 13 / 6 / 10 | 6,600 |

   "Underruns" = `[AUDIO] idle waits` (audout had nothing to play = audible gap). Fewer GIL handoffs buy ~1 ms/frame
   but starve audio; `AUDIOPRIO` (XAudio client workers + render thread 0x827DB318 get the turn first) fixes audio
   but its early yields bring the handoffs back. `switch/tools/eden_ab.sh <label> [ENV=VAL ...]` runs one A/B
   (temporary env lines, restored afterwards; logs in `switch/hwlogs/eden_ab/<label>/`).

## 4. Tools added this session

| Tool | Use |
|---|---|
| `LSWTCS_SWAPCHAIN=1` | `[SWAPCHAIN]` in trace.log: guest call chain at each VdSwap when it changes, plus every 30 swaps the loading-loop gate/timer/elapsed words. Finds which game state is presenting during a wait. |
| `LSWTCS_DELAYLOG=1` | `[DELAY]` every 5 s: guest sleeps (KeDelayExecutionThread) per caller (through the Sleep wrapper `sub_82538950`) with requested/real ms. |
| `LSWTCS_FNLOG` | entry/return lines now go to trace.log with a ms timestamp (stdout is lost when the process is killed); list gained `82506A38` (frame timer, `[FTIMER]` with f1 + host dt), `82357998`, `82358568`, `823CF5B0`. |
| `LSWTCS_SAMPLEPROF_DUMPMS=<ms>` | sampler dump interval (default 20000), with `sampleprof_reset` for a window. |
| `LSWTCSRuntime/build/sample_main.sh`, `sample_all.sh` | cdb stack samples of the PC exe (main thread / all threads), symbolised with inline frames. |
| `tools/fix_ftfloor_sites.py`, `tools/fix_io_wake.py` | the generated-code edits of §2.2/§2.3 (idempotent; rerun after a recompiler regen, after the passes in HANDOFF_2026-10-09_regen.md §6 and the switch-return fixes). |

Measuring: PC `LSWTCSRuntime/build/boot_to_hub.sh` (`launch_game`, `draws`), `[GPUPROF] per frame` timeline
(`wall=`, `draws=`); Eden `switch/tools/eden_hub.sh <s>` then `sdmc:/lswtcs/frames.log` (`t_s`, `swap_real_ms`,
`replay_ms`, `gpuwait_ms`, `draws`). Phases by draw count: intro/loading ~26–30, title 50–90, hub ~780–860.

## 5. Before committing
Regenerate `patches/recomp_output_edits.patch` (generated-code edits: LSW_FTFLOOR, LSW_IOWAKE), run the privacy check
(`git grep --cached -i <user name>` finds nothing), commit only when the user asks.
