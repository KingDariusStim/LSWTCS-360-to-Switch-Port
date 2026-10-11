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

## 4. Commit checklist

`git grep -i` for the user's name and local paths must find nothing; regenerate `patches/recomp_output_edits.patch`
only if hand edits to the generated code changed (it is the output before the two locals passes); record numbers in
this file; commit and push when the user asks.
