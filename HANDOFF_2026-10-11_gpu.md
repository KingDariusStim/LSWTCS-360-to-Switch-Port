# HANDOFF 2026-10-11 (s6): GPU-side optimisation, ranked

Audience: the next agent working on Switch performance. Goal: cut the GPU time of heavy level frames on the console.
**User rules (standing):** never ship an optimisation that makes gameplay worse (a measured loss stays opt-in or is
removed; picture and audio must stay correct); the PC build is the performance baseline, not Xenia; no cache may
stand in for a slow pipeline (ask the user before proposing one); ask before every SD-card copy, never delete on the
SD card; one Eden at a time; never set `LSWTCS_AUTOSTART`; ask before launching or killing the PC game. Patch C/C++
with the Edit tool or Python scripts, not sed on code.

Read first: `HANDOFF_2026-10-11_accel.md` §1 (today's guest-CPU work and the console A/B method), then
`HANDOFF_2026-10-09_glthread.md` §3b (glthread / GLPIPE / fence-spin history) and memory `untiling_plan.md`.

## 0. Why GPU now

Controlled console A/B 2026-10-11 (same session and mode, standing at one spot at the start of Negotiations, ~1,870
draws; logs `switch/hwlogs/2026-10-11_console_ab_{A_old,B_new}/`): the arg-locals build cut main guest thread time
~7 % (37.3 -> 34.8 % busy) and the frame time did **not** move (p50 38.0 / p90 43.3 ms in both). `[GLPROF]` there:

```
wall=38.7 ms  replay=15.2 (render thread CPU)  gpuwait=22.5  | upload 1.3, bind 0.9, state 0.9, program 0.8,
resolve 0.6, rt 0.5, sampler 0.5, draw 0.3, other 8.7
changes per replay: program 164, fbo 39, ubo 2422, texture 195, swizzle 180, sampler 59, state 235
[GLDRAW] 1268 drawn (plain 1141 / points 72 / rects 15 / quads 40), 34 resolves (18 dead), 554 tile repeats skipped
```

The render thread finishes in ~15 ms and then waits ~22.5 ms for the GPU: **the GPU takes ~38 ms per frame** at that
spot, so 26 fps is a GPU limit. CPU-side work (guest or render thread) cannot raise fps there until the GPU frame
drops below ~30 ms. Hub and Eden are more CPU-bound; heavy level spots and cutscenes are GPU-bound.

## 1. Measure first (one console run, before any change)

The tools exist; they have not been run since untiling (2026-10-09) and today's changes.
- `LSWTCS_GLGPUPROF=300` (GPU timestamps after every draw / resolve in every 300th replay -> `[GLGPU]`: totals by draw
  kind, vertices submitted, the 10 programs costing the most GPU time). Hardware only (Eden reads 0).
- `LSWTCS_GLGPUSPLIT=1` with it: sampled replays cycle normal / PIXELS OFF (1x1 scissor) / PIXELS+VERTS OFF (<= 6
  vertices per draw). normal - pixels-off = pixel cost; pixels-off - both-off = vertex cost; rest = fixed per-draw cost.
  Those sampled frames look wrong for one frame (expected).
- Both are lines in `sdmc:/lswtcs/env.txt` (currently `=0`). Editing env.txt is an SD-card copy: ask the user; keep
  the old file (mtp.ps1 push renames it to `env_prev_<time>.txt`).
- Run it standing at the same Negotiations spot (~1,870 draws) for 60 s+ so all three variants land on that scene;
  the user reports docked / handheld and clocks.

**⚠️ CORRECTION 2026-10-11 s7 (read first): GPU timestamps on the Switch are in GPU-timer ticks running at 384/625
of real time, not ns. Multiply every earlier `[GLGPU]` number (including 2026-10-09 and the table below) by 625/384 =
1.628.** Proof (`LSWTCS_GLGPUSPAN=1` run, logs `switch/hwlogs/2026-10-11_console_gpuspan/`, 921 MHz): paused (27-30
draws, vsync-locked 16.67 ms) the raw swap-to-swap stamps read 10.2 ms; in the level (frame mean 35.4 / p50 34.1 ms)
21.0 ms. Both ratios ~1.63. Fixed in `gl_backend.cpp` (`gpu_ticks_to_ns`, applied to [GLGPU] and [GLSPAN]).
Corrected picture at the Negotiations start: **the GPU is busy the whole ~34 ms frame** (swap-to-swap = draws..present,
idle gap 0.0 ms, pre-draw and swap ~0): a true GPU limit, which is why GLPIPE gained nothing. Corrected split
(sampled frames x1.628): total ~33.2 ms = **pixels ~15.0 ms + vertices ~1.3 ms + fixed ~16.9 ms**. Fixed: 1,138 plain
draws 8.5 ms (~7.5 us each), 15 rects 3.9 ms with a 1x1 scissor (~260 us each; likely the texture-source blits
charged to the next draw), resolves 2.6 ms, quads 0.85, points 0.65. Top program 46 (322 draws) 2.2-3.7 ms. Re-ranked
levers: (1) fixed per-draw cost (2.2: UBO binds, state; and find what the 15 rect draws / texsrc blits do), (2) pixel
cost (2.4 and 2.3 shader quality), (3) vertex fetch last (~1.3 ms). The "GPU idle / feeding-limited" reading below is
WRONG (it came from the unscaled stamps). The pause idea (user): paused frames draw only the menu over a copied frame
(969470b), so they show the per-frame floor, not what costs GPU time in the level.

**⭐ s7 RUN 3: CB FLUSH FIX = WIN, NOW DEFAULT ON (logs `switch/hwlogs/2026-10-11_console_cbflush/`):** same spot,
same profiler settings as run 1, `NVC0_CB_FLUSH_ON_WRITE=1`: frame p50 34.2 -> **27.9 ms**, p90 38.2 -> **30.1 ms**;
GPU per normal frame 33.4 -> ~25.7 ms; render-thread gpuwait 19.2 -> 0.4 ms. User: picture fine (black frames = the
profiler's PIXELS OFF samples, to confirm in a run without the profiler), audio fine (log idle waits ~37 per interval vs
12-24 before: the guest is now saturated, scheduler IDLE 28 % -> 0.8 %). The spot is now CPU-bound: render-thread
replay 17.6 ms (`other` ~10 ms), guest walk+record ~14 ms, main thread 52 %. Default on via `egl_init` (setenv, no
overwrite; `LSWTCS_GLCBFLUSH=0` to opt out). **Clean console run (default-on build, original env.txt, no profiler; logs
`switch/hwlogs/2026-10-11_console_cbflush_clean/`): p50/p90/p99 28.5 / 30.5 / 41.1 ms** at 1,800-1,950 draws (was
34.1 / 38.5 / 48.1 at 921 MHz), gpuwait 0.0, scheduler IDLE 1.3 %. User: no black frames, audio fine (log idle waits
35-50 per interval vs 15-22 before; not audible). Next: CPU side again (render thread `other` ~10 ms, guest
walk/record/vhash ~14 ms), then pixel cost (17 ms GPU, the limit again in heavier scenes / cutscenes).

**s7 console runs 1-2 (GLGPUPROF=120, GLGPUSPLIT=2, Negotiations start, 921 MHz; logs
`switch/hwlogs/2026-10-11_console_split2_copy{0,1}/`), GPU ms, real (tick-scaled):**

| variant | GPU | step cost |
|---|---|---|
| normal | 33.4 | |
| PIXELS OFF | 16.3 | pixel shading ~17.1 |
| +VERTS OFF | 15.4 | vertices ~0.9 |
| +UBO FROZEN | ~9.6 | **UBO binds ~5.8 (2,400 per frame, ~2.4 us each)** |
| +TEX FROZEN | ~9.2 | textures ~0.4 |
| +STATE FROZEN | ~8.9 | state ~0.3 |
| floor | 8.9 | resolves 2.6 (34), texsrc blits 1.8 (12), rects 1.5, plain 1.3 (~1.1 us/draw), points+quads+present 1.8 |

Level: 28 blits per frame, 11.8 Mpx. Run 2 (`GLCOPYIMG=1`): LOSS (texsrc 1.8 -> 6.5, resolves 2.6 -> 6.3, frame p50
34.2 -> 41.7 ms): nouveau's glCopyImageSubData is slow; code removed. **UBO-bind cause found in nouveau:**
`nvc0_constbufs_validate` sets `cb_dirty` ("Force cache flush for UBO") on every buffer UBO bind, and `nvc0_draw_vbo`
then emits `MEM_BARRIER 0x1011` (waits for prior work) -> a barrier on almost every draw. Fix:
`switch/mesa/mesa_cbflush_edits.py` (in build_mesa.sh), opt-in `NVC0_CB_FLUSH_ON_WRITE=1` in env.txt: flush on a bind
only if a buffer may have been written since the last flush (transfer_map WRITE, buffer copy, user upload,
invalidate, clear_buffer) or the buffer is persistent. Eden hub OK (Eden does not model the constant cache: console
must check the picture). Run 3 = same profiler settings + `NVC0_CB_FLUSH_ON_WRITE=1`.

**s7 later: profiler flaw + texture-source copies (uncommitted build, Eden-checked):**
- The PIXELS OFF variant bumped `g_gl_epoch` after every draw, so every following draw re-bound all UBOs, textures,
  samplers and state: the "fixed ~17 ms" above is inflated by that, and pixel cost understated. Fixed (only the state
  block is re-applied; the 1x1 scissor is set again after it). `LSWTCS_GLGPUSPLIT=2` adds +UBO FROZEN / +TEX FROZEN /
  +STATE FROZEN variants to price each kind of per-draw change. Texture-source blits are now their own `[GLGPU]`
  item ("texsrc blits", with "pre" = GPU time just before them), no longer charged to the next draw.
- Blits are shader draws and each bumps `g_gl_epoch` (next draw re-binds everything). Eden hub: 21 blits per frame,
  7.8 Mpx (~8.5 full screens). `LSWTCS_GLCOPYIMG=1` (default OFF until a console A/B): plain copies (mode 0, same
  format, no swap, in bounds) use `glCopyImageSubData` instead, no epoch bump. Eden hub: 17 of 21 blits become copies,
  cache resets 7 -> 5 per frame, ubo 864 -> 834, program 112 -> 102; hub and pause-menu screenshots unchanged
  (`switch/hwlogs/2026-10-11_eden_copyimg/`). `[GLPROF]` changes line now prints blits / copies / Mpx per replay.
- Planned console runs (Negotiations start, ~60 s each): (1) `GLGPUPROF=120 GLGPUSPLIT=2 GLCOPYIMG=0`; (2) same with
  `GLCOPYIMG=1`; (3) optional `NV50_PROG_USE_NIR=1` (shader compiler A/B for pixel cost, env-only).

**CONSOLE RESULT 2026-10-11 (s7; official charger, GPU 921 MHz; build 9fa9965; logs
`switch/hwlogs/2026-10-11_console_gpuprof/`), standing at the Negotiations start (1,138-1,140 plain / 72 points /
15 rects / 35 quads / 34 resolves, ~616 k vertices):**

| variant (replays) | GPU ms | plain | rects | resolves | points | quads |
|---|---|---|---|---|---|---|
| normal (5700, 6600) | 20.5, 20.4 | 11.6 | 4.8 | 2.4 | 0.8 | 0.6 |
| PIXELS OFF (5100, 6000, 6900) | 11.0, 11.6, 11.0 | 5.7-6.7 | 2.1-2.5 | 1.6 | 0.4 | 0.5 |
| PIXELS+VERTS OFF (4500, 6300; 5400 = 12.1 outlier) | 10.4, 10.4 | 5.2 | 2.4 | 1.6 | 0.4 | 0.5 |

- Pixel cost ~9.2 ms, **vertex cost only ~0.8 ms** (so 2.1 vertex fetch drops to the bottom), fixed cost ~10.4 ms:
  plain 4.6 us per draw (5.2 ms), the 15 rects 2.4 ms *with a 1x1 scissor* (~160 us each: probably the texture-source
  blits that are charged to the next draw), resolves 1.6 ms with pixels clipped.
- Top programs (normal): 46 (322 draws) 1.4-2.3 ms, 44 (208) 0.9-1.3, 167 (90) 0.4-1.4, 240 / 102 / 193 (1 draw
  each, full-screen) 0.5-1.4 / 0.6-1.3 / 0.5-0.7, 113 (96) 0.45, 6 (points, 72) 0.4-0.8. No single dominant program.
- **The GPU is busy only ~20.4 ms of the 34.7 ms frame** (`[GLPROF]` wall 34.7 = replay 15.1 + gpuwait 18.6; frame
  p50 34.1 / p90 38.5 ms). "gpuwait" is the wait on the previous frame's fence, and with glthread that also includes
  the glthread worker executing the queued GL calls (driver work), plus any time before nouveau kicks the push buffer.
  So this spot is limited by **feeding the GPU** (render thread + glthread worker + kick timing), not by GPU
  throughput. Next tests: `LSWTCS_GLPIPE=1` again (it was judged when the GPU took 38 ms), a glFlush every N draws
  during replay so the GPU starts sooner, and cutting the driver work per draw (2,440 UBO binds, 2.2 below).

**`LSWTCS_GLPIPE=1` console A/B 2026-10-11 (s7; same spot, same 921 MHz, profiler off; logs
`switch/hwlogs/2026-10-11_console_glpipe/`): NO GAIN, keep it off.** Frames with 1,800-1,900 draws: p50/p90/p99
34.6 / 38.7 / 50.7 ms vs 34.1 / 38.5 / 48.1 without it (profiler run, 1 sampled frame in 300). Audio idle waits per
interval and controls unchanged (user: audio and controls fine). With the pipeline the render thread's replay grew
to 32.9 ms, almost all `bind` 16.4 + `draw` 5.9 ms = blocked on a full glthread queue, and the worker's fence wait
was 22.0 ms. Frame times are spread 32-42 ms, not vsync-quantised. So the serial chain is worker -> GPU, and
overlapping the render thread does not help. That contradicts "GPU busy only 20.4 ms" unless the sampled stamps miss
GPU work (work before `GK_START`: arena upload, texture uploads / texsrc blits; or the swap). **Next: measure GPU
busy vs. idle in normal frames** (a timestamp at replay start and after present in every frame, read back a frame
later, no glFinish; print the GPU span per frame and the gap between frames), before choosing between the
flush-every-N-draws test and cutting driver work per draw.

**Eden run 2026-10-11 (user asked for Eden instead of the console; logs `switch/hwlogs/2026-10-11_eden_gpuprof/`):**
same spot (1,866-1,874 draws: 1,138-1,143 plain / 72 points / 15 rects / 35-38 quads / 34 resolves, matching the
console `[GLDRAW]`), 90 s standing, 2,849 frames. As expected, Eden's GPU timestamps read ~0 (0.07 ms per frame), so
**no pixel/vertex/fixed split; the console run above is still needed.** Usable numbers: **~616 k vertices per frame**
(~540 per plain draw), the same in all three variants (the count is taken before the PIXELS+VERTS OFF clamp). Eden
frame: game_ft p50 31.1 / p90 33.4 ms, replay p50 15.7 ms, `[GLPROF]` upload 5.7-6.3 ms (console 1.3), ubo 2,374 per
replay. Wall time of the sampled frames (with glFinish) went *up* with work removed (normal 12-20, pixels-off 25-30,
both-off 32-35 ms): Eden emulates the GPU on the host, so those times say nothing about Maxwell. Route in Eden:
`hub_to_negotiations.sh` stopped at doors 2/3; door 1 is further west (`s:60`, `w:70`, `nw:50 n:60` opened it), then
A (Story), START (crawl), START (skip the cutscene) about 25 s later.

Last hardware numbers (2026-10-09, before untiling, `switch/hwlogs/2026-10-09_gpusplit3b/`, sampled scenes differ):
fixed cost ~7 us per plain draw for every program; one heavy frame with PIXELS OFF still took **39.9 ms with 3,977 draws
and 1.28 M vertices** (program 53: 12.5 ms over 2,523 draws; program 362: 7.5 ms over only 30 draws; program 259:
8.0 ms over 342). So in heavy frames, vertex and per-draw work outweighed pixel shading. Untiling has since cut the
draws (that frame shape would now be ~1,300-2,000 draws), so re-measure before choosing.

## 2. Levers, ranked by expected GPU gain (re-rank after §1)

### 2.1 Vertex fetch (likely biggest if §1 confirms vertex cost)
Vertex data is read in the translated shaders with `texelFetch` on a `usamplerBuffer` spanning all 512 MB of guest
memory (`lsw_spirv_glsl.cc`: `xe_shared_memory` -> `texelFetch(xe_shared_memory_tbo, ...)`, chosen because Eden drops
SSBO-read shaders), one 32-bit word per fetch, plus the per-component endian swap and format unpack in the shader. On
Maxwell that is the texture path per word instead of the vertex attribute fetch unit.
- Cheaper fetch inside the same model: fetch `uvec4` (RGBA32UI view) when a vertex's words are contiguous and
  16-byte aligned (one fetch instead of up to four); hoist shared address math. Shader-side only, no data copies.
- Native vertex attributes: bind guest memory ranges as GL vertex buffers (`glBindVertexBuffer` on the shared-memory
  buffer at the stream's offset/stride, `glVertexAttribIFormat` as raw uints) and keep the swap + unpack in the
  shader. No converted copy, so not a cache. Needs the vertex-fetch instructions mapped to attributes at
  translation time (Xenia's SPIR-V translator emits them as memory loads: look at `SpirvShaderTranslator` vfetch).
  Watch Eden: it dropped SSBO-read shaders; plain vertex attributes are the most portable path.
- Check the vertex count per program in `[GLGPU]` (program 362: 7.5 ms over 30 draws looks vertex- or pixel-heavy;
  find what it is with `resdump` / GLDRAW diagnostics).

### 2.2 Fixed per-draw cost (~7 us x ~1,270 draws ~ 9 ms GPU at the spot)
Per frame there are 2,422 UBO binds (~1.9 per draw), 195 texture binds, 180 swizzle changes, 164 program changes,
235 state changes. On nouveau each UBO bind is a constbuf rebind in the push buffer (and a GPU-side constbuf update).
- One UBO per frame for all constant blocks with per-draw offsets, bound once; pass the per-draw base as a small
  uniform / push the offset via `glBindBufferRange` only when it changes; or merge the VS float block, PS float block,
  bool/loop and fetch blocks into one range per draw (1 bind instead of ~2).
- Skip redundant binds (compare with the currently bound range) and order draws within a pass by program only where
  the guest order allows it (it rarely does; don't reorder blending draws).
- Swizzle changes (180): `LSWTCS_GLVIEWSWZ` texture-view swizzles; check whether swizzle state can live in the
  sampler/view cache so a texture bind carries it.
- Same work also lowers render-thread `bind`/`state` time (CPU), which matters once the GPU frame drops.

### 2.3 Shader code quality (pixel + vertex)
Shaders go Xenos ucode -> Xenia SPIR-V -> GLSL (spirv-cross, `lsw_spirv_glsl.cc`) -> nouveau (TGSI front end). No
SPIR-V optimisation pass runs. Options:
- Run `spirv-opt` performance passes (SPIRV-Tools; Xenia's tree has it) before spirv-cross: dead code, constant
  folding, local loads/stores to registers. Translation time rises; measure it against the shader-load budget
  (feedback: make the pipeline cheaper, not cached).
- Runtime A/B of nouveau's NIR front end: `NV50_PROG_USE_NIR=1` (selectable since `mesa_nir_edits.py`; was measured
  only for compile time on 2026-10-07: -3 %, and Eden asserts more -> console only). Never measured for GPU time.
- Look at the top programs from §1 by hand (`LSWTCS_UCODEDUMP=1`, `[GLXLATE]`): branchy predication, the per-word
  endian swaps, FMZ (`NV50_MUL_ZERO_WINS` is on; keep it, the picture needs it).

### 2.4 Pixel cost
- Full-screen passes at 1280x720: post effects and the 30-draw program in §1. Look for passes the game renders at a
  lower resolution on the 360 that we render at full size (RT size from EDRAM surface info vs. resolve size).
- Resolves (34 per frame, 18 already skipped as dead): GPU cost 0.6-1.7 ms in 2026-10-09 samples; check blit vs.
  copy path and format conversions.
- Do not lower the output resolution or quality by default (user rule); an opt-in setting would be the user's call.

### 2.5 After the GPU frame drops: overlap again
- `LSWTCS_GLPIPE` (frame-end pipeline, default off since the GPU was the limit and the fence wait spun on the audio
  core; spin fix is in): retry once GPU time < render-thread time, watching audio underruns.
- Render-thread CPU (15 ms: `other` 8.7 ms unattributed, upload 1.3) and the GPU-command thread (accel handoff §3)
  become the limit next.

## 3. How to measure (method that worked on 2026-10-11)

- Console A/B in one session, same mode: park builds on the SD card under descriptive names
  (`switch/tools/mtp.ps1 ren lswtcs <old> <new>`; the home-menu forwarder only launches `sdmc:/lswtcs/lswtcs.nro`, so
  rename the build under test to that), the user stands ~30-60 s at one spot per build, pull `stdout.log trace.log
  frames.log env.txt` between runs (each run overwrites them). Current SD state: `lswtcs.nro` = arg-locals build (HEAD),
  `lswtcs_A_old_969470b.nro` = previous.
- Compare the same spot (draw count in frames.log column 9 + `[GLPROF]` gpuwait), not different play routes: run 1 of
  today looked 4 ms slower purely from the route.
- frames.log columns: replay serial t_s game_ft_ms swap_real_ms replay_ms gpuwait_ms(prev) hold_ms draws.
- Eden is useless for GPU timing (timestamps read 0, different GPU); use it only for correctness (screenshots:
  `touch .../shot_now`) and CPU-side A/B (`switch/tools/eden_ab.sh`).
- Build: `switch/build.sh` (GCC -j4; don't run the PC build at the same time). Generated code build steps: recomp
  patch, then `tools/pass_vec_locals.py`, then `tools/pass_arg_locals.py` (accel handoff §1).
  **Trap (2026-10-11 s7):** after the 9fa9965 commit, `Convert 360/LSWTCS/output` had been put back to the pre-arg-pass
  state (vector pass applied, arg pass not), so a build came out 110.2 MB instead of 108.6 MB. Check with
  `python tools/pass_arg_locals.py "Convert 360/LSWTCS/output" --dry-run` (it promotes ~2.09 M refs when not applied)
  and the NRO size (~108.56 MB) before putting a build on the console.
- Measurement build (s7, on the SD card as `lswtcs.nro`, 108,558,533 bytes, includes the tick-scale fix):
  `LSWTCS_GLGPUSPAN=1` ([GLSPAN] per-frame GPU span / idle gap, no glFinish) and `LSWTCS_GLFLUSHN=N` (glFlush every
  N draws; untested, moot now that the GPU is busy all frame), both off by default. SD env.txt has GLGPUSPAN=1.
  Parked on SD: `lswtcs_A_9fa9965.nro` (previous playable), `lswtcs_prev_20261011_205034.nro` (GLSPAN build
  before the tick-scale fix).

## 4. Commit checklist

`git grep -i` for the user's name and local paths must find nothing; regenerate `patches/recomp_output_edits.patch`
only if hand edits to the generated code changed (it is the output before the two locals passes); record numbers in
this file; commit and push when the user asks.
