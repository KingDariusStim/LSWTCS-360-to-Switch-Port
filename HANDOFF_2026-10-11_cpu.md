# HANDOFF 2026-10-11 (s7): CPU-side optimisation on the Switch, ranked

Audience: the next agent working on Switch performance. Goal: cut the CPU time of level frames on the console now
that the GPU no longer limits them.

**User rules (standing):** never ship an optimisation that makes gameplay worse (a measured loss stays opt-in or is
removed; picture and audio must stay correct); the PC build is the performance baseline, not Xenia; no cache may stand
in for a slow pipeline (ask before proposing one); optimise for the console, Eden is only a test vehicle; ask before
every SD-card copy and never delete on the SD card; one Eden at a time; never set `LSWTCS_AUTOSTART`; ask before
launching or killing the PC game. Patch C/C++ with the Edit tool or Python scripts, not sed on code. The user said to
skip vertex hashing work earlier (2026-10-11): ask before picking it up again.

Read first: `HANDOFF_2026-10-11_gpu.md` §1 (today's GPU work, console A/B method, the profiler tools),
`HANDOFF_2026-10-11_switch_opt.md` §0c and §2 (GPU-command thread profile, the failed off-core walk),
`HANDOFF_2026-10-11_accel.md` §2-§6 (guest-code levers not yet tried).

## 0. Where things stand (console, commit 1e483b3, on the SD card as `lswtcs.nro`)

The nouveau constant-cache fix (`switch/mesa/mesa_cbflush_edits.py`, default on) removed the GPU limit at the
Negotiations start. Clean console run, 921 MHz, standing at the start spot (~1,850-1,920 draws; logs
`switch/hwlogs/2026-10-11_console_cbflush_clean/`):

- Frame p50 / p90 / p99 **28.5 / 30.5 / 41.1 ms** (~35 fps). Before the fix: 34.1 / 38.5 / 48.1 ms.
- Render thread waits on the GPU **0.0 ms** (was ~19 ms). GPU busy ~25.7 ms per frame, so it is close behind.
- Guest scheduler `[SCHEDPROF]`: **IDLE 0.1-1.3 %** (was 25-30 %). Shares of core 0: main game thread (id 0)
  **52.8-53.5 %**, GPU-command thread (id 6, entry 0x822D2AB0) **39.2-39.5 %**, audio render thread (id 1) 4.4 %,
  XAudio client (2) 1.1 %, id 3 0.7 %. About 9,800-9,900 switches per 5 s.
- `[GPUPROF]` (GPU-command thread, per frame): walk 4.1 ms, record 10.2 ms (prepare 3.85, vertex hash + copy 2.77),
  1,885 draws (~5.4 us per draw recorded).
- `[GLPROF]` (render thread, core 1, per replay): replay **17.8 ms** = upload 1.26 (2.3 MB arena memcpy) + program
  1.05 + bind 1.23 + state 1.40 + draw 0.60 + rt 0.62 + sampler 0.56 + resolve 0.44 + texkey 0.40 + texsrc 0.34 +
  **other 9.9 ms (not attributed)**; gpuwait 1.2, swap 0.3. Changes per replay: program 167, fbo 39, ubo 2,546,
  texture 200, swizzle 184, sampler 60, state 240; 28 blits (11.8 Mpx).
- Audio: fine to the ear, but the log's idle waits went from 15-22 to 35-50 per interval: the audio thread now
  competes for a saturated core 0. Watch this with every change (§4).

**Core layout (Switch):** core 0 = every guest thread under the cooperative GIL (one runs at a time); core 1 = render
thread (GL replay, records into glthread); core 2 = glthread worker (Mesa driver work), shader-compile worker, native
reverb; core 3 = system. Audio output thread: see `switch/source/audio_switch.cpp`.

**Critical path now:** core 0 is full, so the frame is roughly the sum of what runs there: main ~15 ms + GPU-command
thread ~11-14 ms + audio ~1.3 ms. The render thread (17.8 ms on core 1) and the GPU (~25.7 ms) run in parallel with
it and are the next limits: taking work off core 0 helps until the frame reaches ~25-26 ms (GPU), after which the
GPU pixel cost (~17 ms, `HANDOFF_2026-10-11_gpu.md`) and the render thread matter again. Hub and Eden are more
CPU-bound than this spot; cutscenes are probably GPU-bound (not measured since the fix).

## 0b. s8 measurements (2026-10-11, two console runs, logs `switch/hwlogs/2026-10-11_s8_measure{,2}/`)

New diagnostics: `[GLPROF]` stages are exclusive (nested stages count once) with new stages take / plan / units /
loop / tail / fence; `[CPULOAD]` per-core busy % (opt-in, `LSWTCS_CPULOAD=1`: under-reports, see below); `[SCHEDPROF]`
`cpu=` per guest thread (host thread CPU time, Switch only: `lsw_thread_token` / `lsw_thread_cpu_ns`,
platform_switch.cpp). Play builds: `switch/env_play/env.txt` (trace, profilers, frames.log and the periodic render
diagnostics off: `LSWTCS_FRAMELOG=0`, new `LSWTCS_GLDIAG=0`); measurement runs use the env of the clean run instead.
- Frame p50 / p90 / p99 at the spot (run 2, clean env): **28.2 / 29.9 / 38.2 ms**. No crashes.
- **Core 0 is full.** `cpu=` matches turn time for the big threads: main 53.0 % turn / 51.7 % cpu, GPU-command 39.9 /
  38.6. Nobody blocks while holding the turn. The audio render thread shows cpu 13.6 % vs turn 4.4 %: the native reverb
  runs off the rotation on core 2 under that thread (expected).
- **`[CPULOAD]` under-reports core 0** (66-75 % while the guest threads alone use >90 % of it); run 1's "core 0 30 %
  idle" reading was wrong. Treat `[CPULOAD]` (InfoType_IdleTickCount from a lowest-priority sampler per core) as
  unreliable until cross-checked; core 2 reads 51-56 %, core 1 63-68 %. The core 3 sampler cannot be created (rc).
- Render thread (run 2): replay 18.2-18.7 ms = **plan 3.2-3.35** (plan_dead_work + plan_untile: real own work, a lever),
  **fence 3.2-3.3** (glFenceSync under glthread = waiting for the worker to drain the frame), loop 1.85, upload 1.4,
  state 1.2-1.4, program 1.05, bind 1.0, units 0.84, rt 0.6, resolve 0.6, sampler 0.55, texkey 0.39, texsrc 0.37,
  draw 0.4, tail 1.1-1.65 (the every-120-replays [GLDRAW] diagnostics, incl. draw_content_key over all draws and a
  glGetError), other 0.15.
- Per draw on the console (run 1, `LSWTCS_PREPPROF=1`, us): shaders 0.80, mods 0.22, glsl 0.31, viewport 0.20,
  sysconst 0.21, floats 0.33, rest 0.09; consts 1.47, indices+record 0.35 (record 1,760 B). Reused from the previous
  draw: sys 93 %, vsf 13 %, psf 88 %, bool 100 %, fetch 30 %.
- `[GPUPROF]` (run 2): walk 3.9, record 10.1 (prepare 3.7, vhash+copy 2.8) ms per frame, 1,833 draws.
- Audio idle waits: run 2 ~32-37 per interval (run 1 with PREPPROF ~70-85).

## 0c. Yoda spike fixed (2026-10-12): vertex fetches past the fetch constant's size read 0

Symptom: during gameplay (never in cutscenes) a flat triangle in Yoda's skin colour shot from Yoda across the screen
and stayed for seconds (Switch, Kashyyyk); the PC build showed large flat rectangles in tile 0's rows. User-confirmed
fixed on the console.

Cause: Yoda's skinning VS adds a morph blend: for k < c55.x it fetches a second stream (stride 3 dwords) at element
`vertex + c[56+k].x`. In gameplay the game sets c56/c57 to 8061 / 16122 (8061 dwords = 2687 vertices x 3 = one morph
target), so the fetch lands past the stream's size (16122 dwords in the fetch constant). Our translated shaders read
shared memory unchecked, i.e. whatever an earlier draw had left at that address. Real hardware evidently returns 0 there
(Xenia's own FIXME notes Adreno 200 does). Fix: bounds check in both translators (xenia-canary
`dxbc_shader_translator_fetch.cc` for the PC, `spirv_shader_translator_fetch.cc` for the Switch; in
`patches/xenia-canary.patch`): an element whose first needed word is at or past `size` reads as 0.
`LSWTCS_VFETCHCLAMP=0` turns it off. Every vertex-fetching shader's GLSL changed, so programs rebuild once.

How it was found (method worth reusing, scripts in `tools/rdoc/`): PC RenderDoc captures of glitched and clean frames
-> pixel history / pixel bisection on a glitched pixel -> the draw (Yoda, 8055 indices) -> its constants, fetch
constants and shared memory -> then `LSWTCS_CWLOG=<index count>` (new, PC): a ring of that draw's full VS constant
state, dumped to `cwdump_N.txt` on F12, showed c55.x (morph count) > 0 exactly in the glitched moments.

Ruled out on the way (each tested on the console): today's Mesa cbflush change (`LSWTCS_GLCBFLUSH=0` still glitched),
the arg-locals pass (969470b build glitched too), out-of-range float-constant reads (zero and PS padding,
`LSWTCS_CBPAD=1`), mid-frame vertex page changes (`LSWTCS_SHMLATE=1`, now opt-in). RenderDoc's post-VS readback of
Xenia's shaders reports garbage even for clean draws: do not trust it, use pixel history and the real constants.

Kept from the investigation: VS/PS float blocks with relative addressing are declared 512 entries in the SPIR-V path
too and bound in full on the Switch (the PC already did; reads past c255 now land in the runtime's padding).

## 1. Measure first (one console run, before any change)

The levers below are ranked on today's numbers; confirm them where noted.
- Per-draw GPU-command profile on hardware: `LSWTCS_PREPPROF=1` (`[PREPPROF]` stages of `lsw_gpu_prepare`,
  `[RECPROF]` recorder). Only Eden numbers exist (switch_opt handoff §0c: shader lookup 0.80 us, constant blocks
  0.93-1.35, record build 0.74, float packing 0.68, system constants 0.38, viewport 0.35, per draw).
- Is core 2 busy? The glthread worker now runs the driver work for ~1,900 draws without waiting on the GPU. If it
  is near full, moving work onto core 2 (§2.1) needs care. There is no per-core load print yet: add one (e.g.
  `svcGetInfo(InfoType_IdleTickCount, ...)` per core, printed with `[GLPROF]`) or time the worker inside
  `lsw_glthread` (switch-mesa `mesa_glthread_edits.py`).
- What is `other` (9.9 ms) in the render thread? It is replay time outside the `RpScope` stages: the per-draw loop
  itself, `texture_source` bookkeeping not in RP_TEXSRC, `gpu_frame_take`, the frame log, plan_dead_work /
  untile planning, glthread marshalling of calls that are not inside a scope. Add scopes until `other` < 2 ms.
- Same spot, same mode (charger in: 921 MHz). The user stands ~60 s at the Negotiations start (route below).

## 2. Levers, ranked by expected frame-time gain

### 2.1 Take the GPU-command thread's record work off core 0 (biggest: up to ~10 ms per frame)

`gpu_record_draw` / `lsw_gpu_prepare` are host-only work (no guest code), ~10 ms per frame at this spot, done while
the GPU-command thread holds the GIL. `pm4_extract_ib` (the walk, ~4 ms) also fires guest interrupts
(`pm4_fire_interrupt`) and writes guest memory (scratch / fence side effects), so the walk must stay a guest thread.
Idea: the walk keeps producing the register snapshot per draw, but the record (prepare, constant copies, vertex
hash + copy, record build) runs on a host worker, and the walk drops the GIL while the worker is busy, so the main
thread runs in parallel.
- Ordering: the record reads guest memory (vertex/index data, constants already captured). The game may rewrite
  that memory once the GPU has passed the fence that follows it (on the 360 the GPU reads it asynchronously too).
  So before the walk signals a fence / interrupt / memory write the game can observe, it must wait until the worker
  has finished every draw queued before it. In a level that is about one fence per frame, so the overlap is most of
  the frame.
- What failed before (switch_opt §0c): `LSWTCS_WALKOFF=1` moved the whole walk of large IBs off the rotation onto
  core 2. It lost badly in Eden (p50 23.1 -> 35-43 ms): it starved the glthread worker on core 2, ran 3-4x slower
  there, and put the walk's latency into the frame because the game waits on GPU fences every frame. Differences
  now: only the host part moves, the walk stays on core 0; the console (not Eden) decides; check core 2's load
  first (§1). Possibly use core 3 if the system allows a low-priority app thread there (check what libnx permits).
- Granularity: per-draw handoffs cost more than a record (switch_opt §2 item 3); hand over batches (e.g. per IB, or
  every N draws) and sync only at fences.
- Correctness: frame publish (`gpu_frame_take` in the render thread) must see all records of the frame; the walk's
  tiling / predication state and the per-draw register snapshot must be copied, not shared. Keep it opt-in until a
  console A/B shows a win and the picture is unchanged (Eden screenshots first).
- Must survive real multithreading later (memory `switch_multithreading_constraint.md`).

### 2.2 Make each record cheaper (~10 ms per frame -> less; helps whether or not 2.1 lands)

Per draw ~5.4 us on the console. From the Eden profile and the PC sampler (`switch/hwlogs/2026-10-11_pcprof/`):
- Constant blocks: the fetch block (768 B) is new on 66 % of draws and the VS float block on 60 %; 2.3 MB copied
  into the arena per frame (`[GLARENA]`: vsf 1,646 unique / 696 KB, fetch 1,321 / 990 KB per frame). Copy only the
  register ranges the PM4 walker marked dirty since the previous draw (compare-then-copy is what runs now).
  Fewer bytes also cut the render thread's 1.26 ms upload memcpy.
- Viewport and system constants are recomputed per draw (0.35 + 0.38 us): memoise on a generation counter of the
  context registers that feed them.
- Shader lookup 0.80 us per draw (`get_shader` hash map + clearing the 2.9 KB LswGpuDraw): cache the last (VS, PS)
  pair and the key that produced it.
- Vertex hashing + copy 2.77 ms per frame (~1,060 pages hashed per frame in the hub): would need write tracking to
  skip unchanged pages. The user said skip this earlier; ask before starting.
- Do NOT repeat: masking the fetch block to the slots the shaders use (no gain, switch_opt §0c).

### 2.3 Main game thread (~15 ms per frame on core 0)

Guest code, already on registers-as-locals (vector, live-in, argument/stack registers). Remaining levers from
`HANDOFF_2026-10-11_accel.md`:
- §2 non-volatile guest loads: `PPC_LOAD_*` dereference `volatile` pointers (751 k memory ops the compiler may not
  merge or keep in registers). Experiment: loads non-volatile, stores volatile; keep lwarx/stwcx and MMIO volatile;
  watch for hangs at boot, loading screens and the job system (guest spin-waits without calls). PC build first.
  Possibly the largest guest-code gain left. Must be revisited if real multithreading replaces the GIL.
- §6 `ctx.fpscr` toggles (40 k refs, `disableFlushMode()` / `enableFlushMode()` around FPU / VMX code each write the
  host FPCR): skip the write when the mode is unchanged.
- §6 hot VMX functions `sub_8231ED30` (3.4 % of main in a level), `sub_8231D470` (1.8 %): native NEON versions only
  if bit-exact (as `native_crt`).
- Profile the level on PC with `LSWTCS_SAMPLEPROF=1|2` + `sampleprof.py` to re-rank after each step (the main
  thread is "spread thin": top function 2.4 % in the hub).

### 2.4 Scheduler handoffs (~9,850 switches per 5 s in the level)

Each handoff costs time on core 0. Earlier experiments (`LSWTCS_QDIV=4` fewer handoffs: ~1 ms per frame but starved
audio; `LSWTCS_AUDIOPRIO` fixed audio but brought the handoffs back; `LSWTCS_SCHEDWAKE1` no change in Eden) were
before the IRQL depth fix and today's GPU fix. Re-measure on the console, watching audio idle waits.

### 2.5 Render thread (17.8 ms on core 1: the limit once core 0 drops below ~18 ms)

- Find `other` (9.9 ms) first (§1).
- glthread: every GL call is marshalled; calls that force a sync with the worker (any glGet*, glFenceSync,
  glClientWaitSync, glMapBufferRange) stall the render thread until the worker drains (glthread handoff §3b).
  Count them per replay (`glsync_report` exists) and remove the avoidable ones.
- Per-draw CPU work: 2,546 UBO binds, 240 state changes, 167 program changes per replay. Each bind is cheap on
  the CPU, but fewer binds also cut driver work on the glthread worker (core 2).
- Upload 1.26 ms: shrinks with §2.2's smaller arena.
- `LSWTCS_GLPIPE=1` (frame-end pipeline on the glthread worker) gained nothing on 2026-10-11 while the GPU was the
  limit; retry once the render thread is the limit, watching input latency (the user noticed an extra frame of lag
  on 2026-10-09).

### 2.6 Audio headroom (constraint, and a lever)

The audio thread is now squeezed on core 0 (idle waits roughly doubled in the log, inaudible so far). Every change
above that frees core-0 time helps. If a change makes audio worse, it does not ship (user rule). The native reverb
already runs on core 2 (switch_opt handoff §0b).

## 3. How to measure

- Console A/B (the method that worked on 2026-10-11): build, put the build under test on the SD card as
  `sdmc:/lswtcs/lswtcs.nro` (`switch/tools/mtp.ps1 push` renames the old one to `lswtcs_prev_<time>.nro`; the
  home-menu forwarder only launches `lswtcs.nro`), env.txt the same way, verify each push by pulling it back and
  comparing hashes. The user plays; pull `stdout.log trace.log frames.log env.txt` afterwards (each run overwrites
  them). Ask before each SD copy.
- Compare the same spot by draw count: frames.log columns are `replay serial t_s game_ft_ms swap_real_ms replay_ms
  gpuwait_ms(prev) hold_ms draws`; use swap_real (column 5) for frames with 1,800-1,950 draws. Read `[SCHEDPROF]`
  (trace.log) for core-0 shares, `[GPUPROF]` (trace.log) for the GPU-command thread, `[GLPROF]` (stdout.log) for the
  render thread, `[AUDIO] N buffers played, M idle waits` (stdout.log) for audio.
- Route: Negotiations start = from the hub start, `switch/tools/hub_to_negotiations.sh <press_now dir>` (PC or
  Eden), then the left door is Chapter 1 (doors are numbered above; the script can stop at doors 2/3: walk west),
  A (Story), START (skip the crawl), START ~25 s later (skip the cutscene). On the console the user does this.
- Eden: CPU-side A/B only (`switch/tools/eden_ab.sh <label> [ENV=VAL ...]`, hub, 45 s; `NRO=<path>` for another
  build), and screenshots for correctness (`touch .../shot_now`). Eden's GPU timers read 0 and its JIT changes the
  CPU ratios, so console numbers decide.
- PC: `LSWTCSRuntime/build/boot_to_hub.sh`, `LSWTCS_SAMPLEPROF` + `sampleprof.py` for guest-code hot spots.
- GPU timers on the console are ticks at 384/625 of real time; `gpu_ticks_to_ns` converts them now. Old `[GLGPU]`
  logs (before 2026-10-11 s7) are 1.63x too small.

## 4. Traps

- **Generated code state:** after the 9fa9965 commit, `Convert 360/LSWTCS/output` had been put back to the
  pre-arg-pass copy, so a build came out 110.2 MB instead of ~108.56 MB and lacked the optimisation. Before putting a
  build on the console, check `python tools/pass_arg_locals.py "Convert 360/LSWTCS/output" --dry-run` (when the
  pass is missing it promotes ~2.09 M refs; when applied there is nothing left to do) and the NRO size.
- Build: `switch/build.sh` (GCC -j4, a full game-code rebuild takes ~25 min; don't run the PC build at the same
  time). Mesa: `switch/tools/build_mesa.sh` re-extracts and patches everything; for one edit, apply the edits script
  to `switch/mesa/build/src/mesa-20.1.0-rc3` and run ninja + install there (as done for mesa_cbflush_edits.py), then
  relink the NRO.
- Profiler variants (`LSWTCS_GLGPUSPLIT`) and `LSWTCS_GLGPUPROF` show black / wrong frames on purpose and add a
  glFinish: never leave them in a run the user plays for feel or audio.
- The user's GPU clock: official charger in = 921 MHz. Compare runs in the same mode.

## 5. Commit checklist

`git grep -i` for the user's name and local paths must find nothing; regenerate `patches/recomp_output_edits.patch`
only if hand edits to the generated code changed (it is the output before the locals passes); record numbers in this
file; commit and push (`git push origin main`) when the user asks.
