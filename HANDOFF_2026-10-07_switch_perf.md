# HANDOFF 2026-10-07: Switch performance — faster first scene, faster and steadier frames

**Your task (from the user):** two goals on the Switch build (OpenGL / switch-mesa path):

- **A. Load time:** get the first frames of a new scene drawn sooner. The stall comes mostly from
  shader translation and compilation, plus whatever else the first replay of a scene does.
- **B. Frame time:** cut the cost of each ordinary frame so the game holds 60 fps (or at least a steady
  rate) without hitches.

**Optimise for real Switch hardware, not for Eden** (standing user rule). Eden is the test vehicle.

Read first, in this order:
1. `HANDOFF_2026-10-07_shader_pipeline.md` §0 and §5b: today's shader-pipeline work, the bench tool, the
   depth fix (shadows/lighting/particles).
2. `HANDOFF_2026-10-07_switch.md` §0: Switch build, memory model, all M3 (GL) findings and traps.
3. `HANDOFF_2026-10-06_perf.md`: the PC frame-pipeline work. Its `[GPUPROF]`/`[SCHEDPROF]` reading
   guide applies here unchanged. The PC render thread was cut from 6.0 ms to 3.0 ms there; most of
   those ideas are not yet ported to the GL backend.

## 0. Progress 2026-10-08 (uncommitted; base commit 5a7f02a = all 2026-10-07 work)

User end-game goal: Switch rendering in Eden nearly as fast as, or faster than, the PC build.

**Instrumentation (switch/source/gl_backend.cpp):** `g_rp[]` leaf stages of a replay (upload, shm, fwait,
program, texsrc, texkey, decode, texup, sampler, rt, bind, state, draw, resolve, present, swap, evict).
`[GLSTALL]` now prints the split of each stalled replay + programs/textures built in it; `[GLPROF]`
averages per replay every 60 replays. `[GLARENA]`/`[GLUPLOAD]` (every 300 replays): arena bytes by block
kind and the map/memcpy/unmap split.

**Findings (Eden, hub):**
- First cold split: shader programs ~19.7 s, buffer uploads ~7.6 s, textures ~0.3 s (texture decode is NOT a
  load-time problem; drop §4.2 item 5).
- Any synchronised CPU write into a buffer the GPU used recently = submit + wait for the GPU:
  switch-libdrm `nouveau_bo_wait` kicks the pushbuffer if the bo is in it, then blocks on its fence.
  The 3 per-frame `glBufferSubData` into the 512 MB shared-memory buffer cost 11.3 ms.
- Bytes written into GPU-visible memory are slow in Eden (~155 MB/s at 2 MB/frame: memcpy 13.5 ms;
  map/unmap ~0). Index data was 1 MB of the 2.1 MB arena.
- `glBufferStorage` persistent mapping: its CPU mapping is unmapped memory in Eden (20k "Unmapped Write8",
  render thread crawls). Kept as opt-in `LSWTCS_GLUPMODE=2` only, untested on hardware.

**Fixes (each has a kill switch for A/B):**
- `LSWTCS_GLSHMCOPY` (default 1): shared-memory ranges via `glCopyBufferSubData` from the arena (GPU copy).
- `LSWTCS_GLUPMODE` (default 1): arena written via unsynchronised map + own `glFenceSync` per ring slot
  (`fwait` stage). 0 = old glBufferSubData.
- `LSWTCS_IBSHM` (default 1 on Switch, compiled off on PC): guest index buffers tracked through
  shared-memory residency (`gpu_shm_track`, kernel_stubs.cpp), drawn with `g_shm` as the element buffer
  (`LswGpuDrawRec::ib_shm`). Arena 2.1 MB → 1.0 MB, upload 13 → 1.6 ms.
- `LSWTCS_GLSTATECACHE` (default 1): `GlStateCache` skips unchanged program/FBO/UBO ranges/texture/sampler/
  swizzle binds and the whole `apply_state` when its inputs are unchanged; reset by `g_gl_epoch`, which every
  state-changing helper bumps (blit, clears, texture/RT/FBO create/delete, arena realloc, each replay).

**Result (back-to-back A/B, laptop ON BATTERY so absolute numbers are low):** replay 39.9 → 15.2 ms,
displayed ~25 → ~37.5 fps; cold hub first frame 8.2 → ~5 s. Screenshots match. Now guest-bound (~26 ms
guest frame: main 57%, GPU submit 22%, XAudio 16-17% — XAudio was 3.8% in the 10-07 numbers, check).
Remaining render thread: draw 4.4, bind 2.0, upload 1.6-2.0, sampler 1.5 (get_sampler hashes a std::string
key per unit per draw: cache per fetch constant), resolve 1.2-1.4, other 1.9.

### 0b. Later 2026-10-08 (hardware-driven; all uncommitted)

Switch hardware findings and fixes, in order (each kill switch in env.txt):
- **Title text / menu / HUD missing + red wedge (HW only):** `fileio_open` opened game files read-write
  first; Horizon locks a file open for write, so the game's 2nd open of each `D:\SHADERS\0X*` UI shader
  failed. Fixed: plain opens read-only first (kernel_stubs.cpp). Verified on HW.
- **switch-mesa EGL ignored share_context** → worker GL contexts never shared objects (black). Fixed in
  `switch/mesa/mesa_egl_share_edits.py` (hooked into build_mesa.sh).
- **newlib's single malloc lock** serialised parallel shader builds (20x slower). rpmalloc is now the
  process allocator (`switch/third_party/rpmalloc/rpmalloc_switch.c`, CMake `LSW_RPMALLOC`, wraps
  `_malloc_r` etc.; spans from the real `_malloc_r`, NOT `_memalign_r`, which recursed). Builds also 2x
  faster single-threaded. `LSWTCS_GLWORKERS` default 2 (render thread + core 2 + low-prio core 0).
- **One frame in flight** (`LSWTCS_RTSYNC`): guest waits for the renderer as on PC; restores the Cantina
  wipe transition. **GPU queue cap** (`LSWTCS_GLMAXQUEUE`): fence per frame, fixes HW input lag.
- **1x MSAA** (`LSWTCS_MSAA1X`, default on; `=0` = 2x sample space): only 2x surfaces drop the vertical
  sample factor; 4x surfaces stay full (the game clears 2x world and 1x screens through 4x 640-pitch
  surfaces — any other mapping breaks the clears: black world / streaky title).
- **Dead work** (`LSWTCS_GLDEADSKIP`): resolves whose destination is unsampled for 120 replays skip their
  copy; draws whose only output feeds such a resolve are skipped (hub: 9 draws + 9 resolves/frame, an
  unused blur chain).
- **Trace flush** wrote the whole trace.log under the logger mutex every second → 80-115 ms freezes every
  1-2 s on the SD card; now appends incrementally outside the mutex.
- **#4:** `LSWTCS_GLTEXBIAS` (pixel-shader computed-LOD fetches = implicit LOD + bias instead of
  textureGrad; Maxwell's gradient path is slow) and `LSWTCS_GLFWDCF` (forward-jump-only shaders without
  the switch dispatcher: `skip` variable + `if (skip == 0)` per exec; DOF shader 4477E1B1 193→49 KB).
  Both in xenia-canary spirv_shader_translator*.cc (regenerate patches/xenia-canary.patch).
- HW numbers (partly overclocked mid-run): hub GPU 48 → ~20.6 ms/frame, 12 → ~27-31 fps.
- Diagnostics: `[GLGPU]` (LSWTCS_GLGPUPROF=N, GPU timestamps), `[GSPIKE]` (LSWTCS_SPIKEMS), `[GLSTALL]`
  threshold (LSWTCS_GLSTALLMS), LSWTCS_SHOTAT / RESDUMPAT / STALLSHOTS(+OFFS), capture logs shader ids and
  dumps GLSL to sdmc:/lswtcs/capglsl.
- Later: forward-jump mode also handles properly nested guest loops (`loop`/`endloop` as
  `if (count) loop {}`) and do-while loops made of a predicated backward jump (label = loop header;
  hub corpus + captured hub shaders: 0 dispatchers left). #3 stage reuse (`LSWTCS_GLSTAGEREUSE`):
  compiled VS/PS/GS objects kept per session by GLSL hash (hub: 32 of 119 stage compiles saved).
  spirv_glsl_test now writes `<out>.spv.dis` (ucode disassembly).
- Open: **no audio output on Switch** (null backend, audren not implemented; XMA still decoded: 12-15 ms
  per frame of audio-thread time on HW). 4 hub vertex shaders with real Xenos loops still use the
  dispatcher. Game thread ~19 ms + GPU-command thread ~10 ms per frame on HW (pre-overclock).

---

## 1. Rules (from the user, mandatory)

- **The user wants pipeline optimisations, not more caching.** Make translation and compilation
  cheaper. Don't build features that depend on a warm Mesa disk cache, a pre-built cache, or a saved
  program list unless the user asks. The existing disk cache stays as it is.
- Game data, keys and dumps are the user's own copies only. Never download, print, copy or commit
  them. The Mesa cache, the ucode corpus (`switch/scratch/ucode_hub2`) and saved GLSL are derived from
  the game: personal use only, never commit them.
- Ask before changing Eden's configuration or installing devkitPro/MSYS packages.
- **Patch C/C++ only with the Edit tool or with Python scripts written via the Write tool.** Never use a
  bash heredoc or `sed` on C: a heredoc turned `\n` inside C strings into real newlines again on
  2026-10-07. Watch CRLF files (xenia-canary): read with `newline=''` and write the same endings back.
- **One game instance at a time** (Eden or the PC exe). You may close it to relink, but say so in your
  message first: the user is often playing. **Never set `LSWTCS_AUTOSTART`**; drive menus with the
  `press_now` file (`switch/tools/eden_hub.sh` already does this).
- Commit or push only when asked. Before committing: regenerate `patches/xenia-canary.patch` and
  `patches/recomp_output_edits.patch`, and run the privacy check (no user name, no `C:\Users\...` paths).
- **Nothing from 2026-10-07 is committed.** That covers the Switch GL backend, the shader-pipeline work,
  the depth fix and the Mesa edit scripts. If you are about to make large or risky changes, suggest to
  the user that they commit the current state first.
- Remove any line you add to `%APPDATA%/eden/sdmc/lswtcs/env.txt` for a test once the test is done.
  Keep the file's original lines; never set `LSWTCS_AUTOSTART` there.

## 2. Where things stand (Eden, Cantina hub, 2026-10-07 evening)

**Loading (goal A):**

| Measure | Value | How |
|---|---|---|
| Shader stall before the hub is fully drawn, cold Mesa cache | **29.2 s** (was ~157 s that morning) | sum of `[GLSTALL]` ms in stdout.log |
| Same, warm Mesa cache | 18.6 s | same |
| Guest-thread translation (Xenia translator + SPIRV-Cross) | 3.6 s total (0.85 + 2.8) | `[GLXLATE]` with `LSWTCS_UCODEDUMP=1` |
| Hub shaders / programs | 77 unique (VS+PS) / 52 programs, 1.88 MB GLSL | |
| Bench: compile + link + first draw of the 77 hub shaders | ~22 s (compile 6, link 7.7, first draw 7.4–9.7) | `switch/tools/glbench.sh` |

`[GLSTALL]` covers more than the bench does. It is the whole replay of a frame over 250 ms, so it also
includes texture decode and upload (`get_texture` → `lsw_gpu_decode_texture`, on the render thread
inside the replay), render-target and resolve creation, and any first-use costs. **Nobody has split
that 29 s into shaders vs textures vs other yet. Do that first** (§4.1).

**Frame time (goal B), steady hub:** from `trace.log` while the user played; `[GPUPROF]` every 60
presents, `[SCHEDPROF]` every 5 s:

```
[GPUPROF] per frame: wall=18.1ms busy=18.1 (limiter idle=0.1) walk=1.6 record=3.5 (prepare=1.3 vhash+copy=1.2) replay=17.6 | draws=652
[SCHEDPROF] 0(main)=68.6%/1314us 1(XAudio 827DB318)=3.8% 6(GPU submit 822D2AB0)=23.3% IDLE=0.2%
[FTSTAT] 53–55 swaps/s
[GLDRAW] 652 drawn (plain 504 / points 72 / rects 21 / quads 55), 21 resolves, arena ~1.8 MB/frame
```

How to read this:
- The guest side is saturated: scheduler IDLE 0.2%, work under the GIL ≈ 18 ms per frame, of which the
  main thread (game code) is ~69% and the GPU submit thread (PM4 walk + record) ~23%. Recording costs
  3.5 ms (`prepare` = shader bridge 1.3, vertex hashing/copy 1.2).
- The render thread (core 1) spends ~17.6 ms per frame in `replay()` (`GP_REPLAY`). Both sides hover
  around 16.7 ms, so either one alone caps the rate at about 55 fps.
- **Eden's numbers are not hardware numbers.** Eden JIT-compiles the ARM code on a fast PC CPU, and
  its GL calls go through its own GPU emulation. Eden also *displays* only ~2 fps of what the game
  presents (known Eden issue, ignore it). Use Eden for correctness and for relative A/B (same
  scene, back-to-back runs). For absolute numbers, ask the user to run the NRO on their console and
  send back `sdmc:/lswtcs/trace.log` + `stdout.log`. The same `[GPUPROF]`/`[SCHEDPROF]`/`[GLSTALL]`
  lines are written there. Agree with the user on a fixed test (boot → hub, stand still 60 s, walk a set
  route) so runs are comparable.

## 3. Build, run, measure (Switch)

- Build: `switch/build.sh` (devkitPro GCC). A relink takes 1–2 min, a cold game-code build ~1 h. Keep the
  default `LSW_JOBS=4`: more parallel GCC jobs run out of RAM. Output: `switch/build/lswtcs.nro`.
- Mesa (switch-mesa 20.1 rebuilt by us): `switch/tools/build_mesa.sh` (from the tarball; runs
  `switch/mesa/mesa_cache_edits.py`, `mesa_nir_edits.py`, `mesa_fmz_edits.py`; 5–15 min). For quick
  experiments, edit `switch/mesa/build/src/mesa-20.1.0-rc3/...`, then run ninja + install (exact
  command in the shader-pipeline handoff §3) and relink. Move lasting changes into an edit script and
  re-run `build_mesa.sh` to prove it.
- Run to the hub: `switch/tools/eden_hub.sh <seconds after hub>` (boots, presses START/A, reports "HUB
  loaded"). For a cold shader measurement, first move `%APPDATA%/eden/sdmc/lswtcs/mesacache` aside
  (rename it). Don't delete it unless the user agrees.
- In-game triggers (create the file in `%APPDATA%/eden/sdmc/lswtcs/`): `shot_now` → `shot_<replay>.png`
  + per-draw `[GLDRAWCAP]` log; `resdump_now` → every resolve target / RT as PNG (same names as the PC's
  `resdump_now`, for PC/Switch diffs); `press_now` (button name) → one pad press.
- Env switches (one `NAME=VALUE` per line in `sdmc:/lswtcs/env.txt`; remove them afterwards):
  `LSWTCS_GLTIMING=1` (per-program `[GLPROGT]` compile/link/first-draw ms; adds glFinish, which distorts
  game speed), `LSWTCS_UCODEDUMP=1` (ucode corpus + `[GLXLATE]` + `[GLTEXSTATE]`), `LSWTCS_GLBENCH=1`
  (via glbench.sh), `LSWTCS_GLWORKERS=N` (background compile, default 0), `LSWTCS_GLFMZ`,
  `LSWTCS_GLVIEWSWZ`, `LSWTCS_GLD32F` (kill switches for today's changes), `LSWTCS_FPS_CAP` (default 60).
- PC harness for shader output: `switch/tools/spirv_glsl_test.sh <outdir> switch/scratch/ucode_hub2`
  (translates the hub corpus with the bridge's settings, validates with glslang, prints `[time]`).
- PC reference renderer: `LSWTCSRuntime/build/boot_to_hub.sh` / `hubprof.sh` (see the perf handoff).
  Use it when a Switch change might alter the picture.
- After every Eden run that behaves oddly, read `%APPDATA%/eden/log/eden_log.txt`. Eden doesn't fault
  on unmapped memory (it reads 0 and drops writes); look for "Unmapped" lines. Thousands of
  `shader_recompiler ... Invalid insn 0x0` asserts are normal (they appear with or without our changes).

## 4. Goal A: first scene drawn faster

### 4.1 First: split the stall (no guessing)

Add timers around the parts of `replay()` (switch/source/gl_backend.cpp) that run on first use:
program build (`program()` → `program_build`), first draw of a new program (nouveau backend), texture
decode + upload (`get_texture`/`create_texture`), RT/resolve-target creation (`rt_get`,
`resolved_get`, gl_rt.inc), sampler creation. Report them per stalled replay in the `[GLSTALL]`
line. Also time the guest side: `[GLXLATE]` (3.6 s) runs inside `lsw_gpu_prepare` on the GPU submit
thread under the GIL, so it delays the game, not the render thread. Rank the work by this split.

### 4.2 Candidates (measure each with glbench.sh and a cold hub run)

1. **Parallel compilation.** The Switch gives applications three cores (0–2): guest threads on 0, the
   render thread on 1, core 2 nearly idle. `LSWTCS_GLWORKERS` (worker GL contexts on core 2) exists but
   is off: cold starts showed stale frames for minutes while programs compiled. Compiles are now ~5x
   cheaper, so re-evaluate. One option: the render thread blocks (as now) but compiles a frame's
   missing programs on render thread + worker in parallel, so the wait is shorter and nothing stale is
   shown. **Trap:** workers must never submit GPU work (no glFinish/glFlush). switch-mesa's contexts
   share one GPU channel without locking, which corrupts the command stream. nouveau's backend compile
   happens at first draw, so it stays on the render thread.
2. **Shader-object reuse across programs.** 52 programs ≈ 104 stage compiles for 77 unique shaders. Keep
   compiled VS/PS objects by GLSL hash (`program_build` compiles both from source every time). Saves
   ~1.5 s of compile. Watch memory: Mesa keeps each shader's IR.
3. **The six dispatcher shaders.** Shaders with Xenos jumps/loops become `for(;;){switch(pc)}`. The worst
   two: `1C678C9DD7795F36` PS ~2 s, `07A91A698B150792` VS ~1.6 s (bench). Emit structured loops for
   Xenos `loop`/`jmp` patterns where possible (xenia-canary `spirv_shader_translator.cc`, main loop /
   `main_switch_op_`).
4. **SPIRV-Cross on the guest thread (2.8 s).** Every hub shader takes exactly 2 SPIRV-Cross passes
   (`force_temporary_and_recompile`: a forwarded expression read twice). `Options::force_temporary`
   was tried: not faster, 48% bigger GLSL. Find which expressions force the second pass and emit them
   as explicit SPIR-V temporaries in the translator. Alternatively, move translation off the GIL onto
   core 2 (it only needs the ucode and the modification).
5. **Texture decode/upload**, if §4.1 shows it is significant: decode on core 2, upload on the render
   thread. Check `lsw_gpu_decode_texture`'s untiling/format conversion for slow paths.
6. **Remaining per-fetch runtime decoding** in the shaders (half-texel offset from the fetch size, LOD
   bias `exp2`, `exp_adjust` `ldexp`). This could be specialised like the texture signs, if a level
   shows many fetches.
7. Long term, user-approved direction: deko3d + an offline shader pack (no on-device compile). Large;
   propose it only with numbers.

Validate every shader change against a PC screenshot of the same view. Also check the title screen,
the hub (shadows, warm light, table glows), and one real level.

## 5. Goal B: cheaper, steadier frames

### 5.1 Instrument the render thread first

On the Switch, `[GPUPROF] replay:` sub-stages print 0.00 because the GL backend only times the whole
replay. Add the same split as the PC (`GP_RP_*` in `LSWTCSRuntime/include/lsw_gpu_frame.h`): arena
upload, shared-memory copies, texture/sampler collection, state application (`apply_state`), UBO
binds, draw calls, resolves/blits (`do_resolve`), present blit, `eglSwapBuffers` (it blocks for
vsync: `eglSwapInterval(1)`; measure it separately). Also try GL timer queries
(`GL_TIME_ELAPSED`) for GPU time, if switch-mesa/Eden support them.

### 5.2 Render-thread candidates (port of the PC's 6.0 → 3.0 ms work)

- **Per-draw redundant state.** Every draw re-issues 6 `glBindBufferRange` calls, plus `glActiveTexture`,
  `glBindTexture`, `glTexParameteriv(GL_TEXTURE_SWIZZLE_RGBA)` and `glBindSampler` per unit, plus the
  full `apply_state` (blend per RT, depth, stencil, cull, viewport, scissor). Mesa marks state dirty on
  many of these even when nothing changed. Cache the last value per binding/unit and skip unchanged
  ones (the PC did this: commit `8c092ce` cached texture/sampler bindings, `f7032ea` deduplicated
  constant blocks and skipped unchanged root bindings). The swizzle call was added on 2026-10-07; cache
  it per (texture, swizzle) too.
- **Uploads.** One `glBufferSubData` of the whole arena (~1.8 MB/frame) into a 3-buffer ring, plus
  shared-memory range copies. Try a persistently mapped buffer (`ARB_buffer_storage`, if switch-mesa
  exposes it on the 4.3 context) and write the arena directly into it, or upload only what changed
  (the PC deduplicates constant blocks).
- **Program switches and draw count.** 652 draws, 504 plain, the rest points/rects/quads through
  geometry shaders. Geometry shaders are slow on Maxwell: check how much the 148 GS draws cost, and
  whether rects/quads could be expanded on the CPU or by an index pattern instead.
- **Resolves.** 21 per frame, each a full-screen blit through a GLSL program plus FBO switches. Check
  for redundant ones (same source rect re-resolved, or resolve targets nobody samples).
- **`glFinish` must stay out of the steady path.** It appears only under diagnostics (GLTIMING,
  capture, bench, probes). Keep it that way.

### 5.3 Guest-side candidates (the GIL side, ~18 ms in Eden)

- Recording: `prepare` 1.3 ms (shader bridge per draw: modification build, texture-state checks;
  `textures_unsigned` loops every draw since 2026-10-07, so cache it per shader+fetch state), `vhash+copy`
  1.2 ms (vertex page hashing; the PC notes explain it), PM4 walk 1.6 ms.
- The main thread (game code, recompiled PPC) is ~69% of guest time. Compiler flags, the single-view
  memory fold (+13% guest CPU on PC) and the GIL scheduler are covered in the Switch handoff and memory
  `switch_multithreading_constraint.md`. The GIL is temporary: prefer fixes that survive real
  multithreading.
- Get hardware numbers before investing here (§2): Eden's CPU speed says nothing about the console's.

### 5.4 Stability (hitches)

- Any new shader that appears mid-play (an effect seen for the first time) stalls a frame for its
  compile + link + first draw. Find out how often this happens in a level: `[GLSTALL]` lines after the
  scene is up. If needed, compile the shaders a level's draws reference during its loading screen
  (that is pipeline work, not a cache).
- Texture creation and eviction (`evict_textures`, 192 MB budget, `LSWTCS_GLTEXMB`) and new
  RT/resolve targets also create one-off spikes. The 5.1 instrumentation will show them.
- Frame pacing: the guest limiter (`LSWTCS_FPS_CAP`, default 60) plus `eglSwapInterval(1)`. When a frame
  takes 17–19 ms, vsync turns it into a 33 ms frame. Once frames fit, check that the limiter and vsync
  don't fight. If 60 isn't reachable on hardware, a steady 30 (the game's timing follows real time) is a
  valid target to propose.

## 6. Traps already paid for

- Sampling a `GL_DEPTH32F_STENCIL8` texture returns 0 on switch-mesa. Depth RTs are D24S8 now
  (`LSWTCS_GLDEPTHPROBE=1` is the probe).
- Eden silently drops draws whose shader reads an SSBO (hence the texel buffer). An early `return` in a
  geometry shader hangs Eden's recompiler. A VS+GS program without a fragment stage hangs Eden.
- The index buffer binding is VAO state: re-bind `GL_ELEMENT_ARRAY_BUFFER` after switching arena
  buffers.
- libnx starts every thread on the default core: pin new threads (`pin_to_core`, `svcSetThreadCoreMask`).
- Mesa's own stderr doesn't reach the logs from the render thread; trace with `fprintf(stdout)` + `fflush`.
- `std::vector<T> v(size_t(n));` is the most vexing parse in some forms: use a cast or braces.
- The Eden GPU address space is limited: unbounded caches ran it out ("Failed to allocate free space in
  the GPU AS" in eden_log.txt). Hence the texture budget and the fixed arena ring.
- The PC laptop varies ±30% run to run; Eden varies too. Only compare back-to-back A/B runs of the
  same scene.

## 7. Suggested order

1. Commit check with the user (§1). Then instrument (§4.1, §5.1) and get one hardware measurement.
2. Goal B quick wins on the render thread (§5.2 state caching), A/B in Eden, then on hardware.
3. Goal A: parallel compile or shader-object reuse (§4.2 items 1–2), then the dispatcher shaders.
4. Re-measure, update this file and the memory index (`MEMORY.md`), and report numbers to the user.
