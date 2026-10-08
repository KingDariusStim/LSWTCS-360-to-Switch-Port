# HANDOFF 2026-10-07: optimise the shader translation pipeline (Switch / OpenGL)

**Your task:** find and implement optimisations that cut the time it takes to get the game's shaders
running on the Nintendo Switch, measured on the first and on later visits to an area. Loading the
Cantina hub currently stalls for ~2.5 min on a cold cache and ~1 min on a warm one (Eden numbers;
the user reports similar behaviour on real hardware). **Optimise for real Switch hardware, not for
Eden** (user instruction): Eden is the test vehicle only.

Read first: `HANDOFF_2026-10-07_switch.md` §0 (Switch build state, all M3 findings and traps). This
file is self-contained for the shader-pipeline task.

## 0. UPDATE 2026-10-07 session 2: pipeline optimisations (user: "pipeline optimizations, instead of
## relying on cached shaders" -> no further cache work unless asked)

**Cold hub stall (sum of `[GLSTALL]`, Eden): ~157 s -> 29.2 s; warm (Mesa cache hit) 62 s -> 18.6 s.**
Guest-side translation (Xenia + SPIRV-Cross, `[GLXLATE]`): 8.5 s -> 3.6 s. Hub GLSL: 92 shaders /
5.26 MB -> 77 / 1.88 MB. Title and hub render identically (screenshots compared). The scripted Mesa
build (`build_mesa.sh`, now also running mesa_nir_edits.py + mesa_fmz_edits.py) was re-run from the
tarball and the NRO linked against it. Nothing committed.

Bench (`switch/tools/glbench.sh`, new: compile + link + first draw of a GLSL corpus on switch-mesa in
Eden, Mesa cache off, no game boot; `LSWTCS_GLBENCH=1`, `gl_bench()` in gl_backend.cpp):

| step (cumulative) | shaders | MB | total s |
|---|---|---|---|
| baseline | 92 | 5.26 | 149.7 |
| 1 NMin/NMax -> FMin/FMax (SPIR-V word rewrite, lsw_spirv_glsl.cc) + one 512 MB shared-memory binding | 92 | 4.03 | 78.7 |
| 2 no early-z hint modification (gl_bridge.cc) + branch-free endian swap (translator) | 77 | 3.13 | 62.5 |
| 3 all-unsigned texture specialisation (modification bit 63, translator + bridge) | 77 | 2.60 | 42.0 |
| 4 FMZ: plain multiplies, nouveau sets Maxwell FMZ on every float mul (`NV50_MUL_ZERO_WINS`) | 77 | 2.02 | 25.9 |
| 5 drop the one-pass `for(;;){...break;}` main loop (lsw_spirv_glsl.cc) | 77 | 2.01 | 22.1 |
| 6 texture swizzle as GL texture state (`image_view_format_swizzle`, gl_backend apply_swizzle) | 77 | 1.88 | ~22 (link+compile 14.7 -> 13.7; draw column is Eden noise +-2 s) |

Details / why each is safe:
- NMin/NMax: Maxwell FMNMX already returns the non-NaN operand; nvc0 lowers min/max to it.
- Early-z hint (pixel modification bit 46) only differed by `layout(early_fragment_tests)`, set exactly
  when Maxwell does early Z anyway; it doubled most pixel shaders.
- Unsigned: hub data (`[GLTEXSTATE]`, LSWTCS_UCODEDUMP) = every fetch sign 00. Bridge sets
  `Modification::kLswTexturesUnsigned` when all of a shader's `texture_bindings()` have SwizzleSigns 0;
  other draws get the generic translation (extra program only there).
- FMZ: `switch/mesa/mesa_fmz_edits.py` (nouveau from_tgsi: io.mul_zero_wins when NV50_MUL_ZERO_WINS=1,
  the Gallium Nine path; GM107 emitter sets FMZ). Translator `Features::lsw_host_mul_zero_wins` makes
  ZeroIfAnyOperandIsZero / the kMul/kMad check a no-op. `LSWTCS_GLFMZ=0` turns both off together.
- Eden logs ~1000+ `shader_recompiler ... Invalid insn 0x0` asserts with or without FMZ: Eden quirk.
- `NV50_PROG_USE_NIR` (now selectable on Switch, `switch/mesa/mesa_nir_edits.py`, default off): only
  -3% and Eden asserts more -> not used.
- Swizzle: `LSWTCS_GLVIEWSWZ=0` restores in-shader swizzles (bridge + backend read the same env).
- Diagnostics added: `LSWTCS_UCODEDUMP=1` (ucode corpus to sdmc:/lswtcs/ucode + `[GLXLATE]` ms/sizes +
  `[GLTEXSTATE]`), PC harness takes a corpus dir (`spirv_glsl_test.sh <out> switch/scratch/ucode_hub2`),
  reads each dump's modification, mirrors the bridge defaults, prints `[time]` per shader.
- Patch C/C++ with Edit / Write-tool scripts: a Python heredoc turned `\n` escapes into real newlines.

**Next candidates (measured share of what is left):**
1. Shader-object reuse across programs: 52 hub programs = ~104 stage compiles for 77 unique shaders;
   compile is ~6 s of the bench -> ~1.5 s. Watch memory (Mesa keeps each shader's IR).
2. The 6 shaders with a real `switch(pc)` dispatcher (jumps/loops): 1C678C9D... PS ~2 s, 07A91A69... VS
   ~1.6 s; look at emitting structured loops for Xenos `loop` instead of the dispatcher.
3. SPIRV-Cross on the guest thread: 2.8 s on Switch (PC 260 ms vs translator 40 ms). Every hub
   shader takes exactly 2 SPIRV-Cross passes (force_temporary_and_recompile: a forwarded expression
   read twice). `Options::force_temporary` (1 pass) is NOT faster and makes GLSL 48% bigger: rejected.
   Next idea: find which expressions trigger it and emit them as SPIR-V temporaries in the translator.
4. Remaining per-fetch runtime decoding (half-texel offset from fetch size, LOD bias exp2, exp_adjust
   ldexp) could be specialised like the signs if a level shows many fetches.
5. Long term unchanged: deko3d + offline shader pack.

---

## 1. Rules (from the user, mandatory)

- Game data, keys, dumps: the user's own copies only. Never download, print, copy or commit them. The
  Mesa shader cache and the saved program GLSL are derived from the game: personal use, never commit.
- Ask before changing Eden's configuration or installing new devkitPro/MSYS packages (already
  installed: dkp-meson-scripts, switch-pkg-config, dkp-toolchain-vars, meson, python, python-mako,
  bison, flex, switch-zlib, libexpat).
- Patch C/C++ with the Edit tool or Python scripts written with the Write tool; no heredoc/`sed` on C.
- Commit/push only when asked. Before committing: regenerate `patches/xenia-canary.patch` and
  `patches/recomp_output_edits.patch`, run the privacy check (no user name, no `C:\Users\...` paths).
- One game instance at a time; you may close Eden to relink. The user often watches Eden runs and may
  interrupt a long command: keep runs short and say what you are doing.

## 2. The pipeline today (where every millisecond goes)

Per GL program = (vertex shader, pixel shader, optional geometry shader) of one draw:

1. **Xenos microcode → SPIR-V**: Xenia's `SpirvShaderTranslator` (xenia-canary, two local edits:
   `alpha_to_mask` member order + `lsw_padding_std140[3]` in `SystemConstants`). Driven by
   `switch/source/gl_bridge.cc` (`lsw_gpu_prepare`, Vulkan-style per-draw *modifications*: interpolator
   mask, clip planes, point size, depth mode, MIN/MAX premult). Runs on the guest thread, cached per
   (ucode hash, modification).
2. **SPIR-V → GLSL 4.30**: SPIRV-Cross (`SPIRV-Cross/` checkout, `switch/source/lsw_spirv_glsl.cc`)
   + three GLSL rewrites needed in Eden/mesa: uniform blocks get `_vs/_ps/_gs` suffixes; the storage
   buffer `xe_shared_memory[4]` becomes `texelFetch(xe_shared_memory_tbo, ...)` (Eden drops every
   draw whose shader reads an SSBO); geometry-shader `return` → `do{...}while(false)` + `break` (an
   early return in a GS hangs Eden's shader recompiler).
3. **Mesa (switch-mesa 20.1, TGSI path, nouveau nvc0)**: `glCompileShader` (GLSL front end +
   optimiser) and `glLinkProgram` (link + GLSL→TGSI), then nouveau's backend (TGSI → Maxwell code)
   lazily at the program's **first draw**. Done synchronously on the render thread (pinned to core 1)
   in `switch/source/gl_backend.cpp` `program()` → `program_build()`.

**Measured** (Eden, render thread pinned; `LSWTCS_GLTIMING=1` prints `[GLPROGT]` lines):
- GLSL sizes: 3–145 KB per program (a hub VS is 113 KB / ~2800 lines: Xenia's vertex fetches through
  the shared-memory buffer with endian swaps, NaN-safe multiplies (`isnan` pairs per mul), a
  `for(;;){switch(pc)}` dispatcher for shaders with control-flow jumps, texture fetches emitted for
  both signed and unsigned variants + in-shader swizzle via `texture_swizzles`).
- Cold, per program: compile 0.05–2.3 s, link 0.08–2.5 s; first draw ≤ 0.02 s (backend is cheap
  cold?! — but see warm numbers: on a cache hit the backend shows up at first draw, 125–685 ms).
- Hub: ~60 programs. Cold total stall ≈ 157 s to a fully drawn hub; warm ≈ 62 s.
- Warm (Mesa disk cache hit), per program: compile 0 ms, link 50–800 ms, first draw 125–685 ms.
  What is left on a warm load is (a) TGSI deserialisation/relink and (b) nouveau's backend codegen,
  which Mesa 20.1 does **not** cache.

## 3. What already exists (use it, don't rebuild it)

- **Mesa with shader disk cache:** `switch/tools/build_mesa.sh` (downloads mesa-20.1.0-rc3, sha256
  checked, devkitPro recipe pinned at pacman-packages `f103fe8`, applies `switch/mesa/mesa_cache_edits.py`,
  builds with meson under `MSYSTEM=MSYS`, installs to `switch/mesa/build/install/...`; ~5–15 min).
  `switch/CMakeLists.txt` links those libs by absolute path when present. Runtime sets
  `MESA_GLSL_CACHE_DIR=sdmc:/lswtcs/mesacache` (gl_backend.cpp `egl_init`). Switch-specific Mesa fixes
  and why are in the edit script's comments (no mmap, fixed driver id, no flock, no util_queue,
  close-before-rename, timespec_get, Python 3.12). POSIX gaps: `switch/source/mesa_shim.cpp`.
  To iterate on Mesa quickly: edit `switch/mesa/build/src/mesa-20.1.0-rc3/...`, then
  `MSYSTEM=MSYS /c/devkitPro/msys2/usr/bin/bash.exe -lc "cd /home/<user>/Desktop/LSWTCSPortMod/switch/mesa/build/src/mesa-20.1.0-rc3 && ninja -C build -j6 && DESTDIR=/home/<user>/Desktop/LSWTCSPortMod/switch/mesa/build/install ninja -C build install"`
  then relink (`switch/build.sh`). Port lasting changes into `mesa_cache_edits.py` and re-run
  `build_mesa.sh` to prove the scripted build. Mesa's own stderr does not reach the logs from the
  render thread; trace with `fprintf(stdout, ...)` + `fflush`.
- **Background compile machinery** (off: `LSWTCS_GLWORKERS=1`): worker GL contexts on core 2,
  program list saved to `sdmc:/lswtcs/glprogs/<key>.glsl3` (only written in worker mode today),
  boot-time queueing of saved programs, all-or-nothing frames (hold the last complete frame), dormant
  program-binary save/load (`glGetProgramBinary` — Mesa now reports 1 binary format). **Workers must
  never submit GPU work** (no glFinish/glFlush): switch-mesa's contexts share one GPU channel without
  locking → command-stream corruption. It was switched off because cold starts showed stale frames for
  minutes; with a warm cache it may now be worth re-enabling for boot preloading.
- **PC test harness:** `switch/tools/spirv_glsl_test.sh` builds `spirv_glsl_test.exe` (same
  translator + SPIRV-Cross + rewrites + geometry shaders) and validates the GLSL with glslang
  (`switch/scratch/glslang_build`). Inputs: `xenia/shdump/*.ucode.bin.*` (host-order dumps; the tool
  byte-swaps them). Use it to measure GLSL size changes in seconds without the Switch build.
- **Measurement in Eden:**
  - `switch/tools/eden_hub.sh <secs>`: boots, presses START/A via `press_now`, reports when the hub
    level loaded. A cold/warm comparison: delete `%APPDATA%/eden/sdmc/lswtcs/mesacache`, run to the hub,
    record seconds until `[GLDRAW] replay N: >700 drawn`, sum of `[GLSTALL]` ms; run again warm.
    (The exact loop used is in this conversation's last commands; recreate it as a script.)
  - `LSWTCS_GLTIMING=1` in `sdmc:/lswtcs/env.txt` (remove afterwards): per-program compile / link /
    first-draw ms. Note it adds glFinish calls (distorts game speed while compiling).
  - `shot_now` file in sdmc:/lswtcs → `shot_<replay>.png` + per-draw `[GLDRAWCAP]` log.
- Switch build: `switch/build.sh` (devkitPro GCC; game code ~1 h cold, relink ~1–2 min; keep
  `LSW_JOBS=4` or GCC exhausts RAM).

## 4. Candidate optimisations (investigate, measure, then implement the winners)

Rank them by measured payoff on a hub load. Hypotheses, not conclusions:

1. **Cache nouveau's backend output** (warm-load win, possibly the biggest remaining one): in
   `src/gallium/drivers/nouveau/nvc0/nvc0_program.c` (`nvc0_program_translate`), key the generated
   code + relocations/fixups + program info on the TGSI tokens and the shader key, and store/load it
   through the screen's `disk_shader_cache` (`disk_cache_put/get`). Removes the 125–685 ms first-draw
   cost per program on warm loads.
2. **Smaller GLSL from the translator** (cold-load win, also helps warm link): check which emulation
   paths can be dropped or specialised for this game: NaN-safe multiply (does LSWTCS need the 0×inf=0
   rule? test visually), duplicated signed/unsigned texture fetches (the game's texture signs are
   known per fetch at record time), in-shader texture swizzle (use GL texture swizzle / texture views
   instead: set `Features.image_view_format_swizzle = true`), vertex-fetch endian/format handling,
   the `switch(pc)` dispatcher. Measure GLSL size and Mesa compile time per change (PC harness first,
   then `[GLPROGT]`). Keep a reference capture of title + hub to catch visual regressions.
3. **SPIR-V optimisation before SPIRV-Cross**: xenia-canary's `third_party/SPIRV-Tools` (submodule,
   not checked out) — `spirv-opt` passes (DCE, inlining, constant folding, block merging) may cut GLSL
   size a lot. Costs a dependency and translator-thread CPU; measure both sides.
4. **SPIRV-Cross output options** (`lsw_spirv_glsl.cc`): fewer temporaries/`precise`, different
   loop/switch emission; check what Mesa's front end spends time on with large switch dispatchers.
5. **Mesa NIR path** (`NV50_PROG_USE_NIR`, forced off by devkitPro's patch for the cache layout):
   may compile faster or slower; measure.
6. **Pre-built cache**: cache entries are valid across all Switch units and Eden for the same build
   (fixed driver id + GPU NV120 + GLSL hash). Add a warm-up mode that compiles every saved program
   (glprogs list — make it always written, not only in worker mode) and an automated level tour
   (press_now through the menus, or call the engine's level-load function; the Ghidra 360 oracle can
   find it) to fill the list; then ship `mesacache/` with the SD data (personal use).
7. **Boot preload on core 2** once warm loads are cheap: re-enable workers only for preloading the
   saved list at boot (on-demand compiles still synchronous).
8. Long term (user-approved direction): **deko3d + offline shader pack** (compile everything on PC with
   uam → no on-device compile at all). Large; only propose with numbers from 1–7.

## 5. Traps already paid for (don't rediscover them)

- Eden: SSBO reads dropped silently; GS early `return` hangs the recompiler; no fault on unmapped
  memory (reads 0, drops writes) — a NULL-pointer bug shows up as random hangs + `Unmapped` lines in
  `%APPDATA%/eden/log/eden_log.txt`. Read Eden's log after every odd run.
- switch-mesa: one unlocked GPU channel for all contexts; no mmap; util_queue unusable (call_once);
  SD fs cannot rename open files; uniform blocks with the same name must match across stages.
- Thread placement: libnx starts every thread on the default core — pin new threads
  (`svcSetThreadCoreMask`); guest threads core 0, render thread core 1, workers core 2.
- Index buffer binding is VAO state: re-bind `GL_ELEMENT_ARRAY_BUFFER` after switching arena buffers.
- `std::vector<T> v(size_t(n));` is a function declaration (most vexing parse) — use a cast.
- Debug lines `[GL] eglGetDisplay...` etc. in `egl_init` are temporary instrumentation; drop them
  when touching that code.

## 5b. FIXED 2026-10-07 s2: shadows, light tone, particles (one cause)

Every depth resolve was all zeros on Switch: **sampling a GL_DEPTH32F_STENCIL8 texture returns 0 on
switch-mesa/Eden** (probe `LSWTCS_GLDEPTHPROBE=1`: D32F_S8 -> 0.0, D32F and D24_S8 -> 0.5). The hub is
deferred (HANDOFF_2026-10-05 §4: shadow map + scene depth resolves -> shadow-mask pass -> mip chain ->
composite), so zero depth killed the shadows, changed the composite's light tone and faded out the
depth-tested particles. Fix: depth RTs are GL_DEPTH24_STENCIL8 (gl_rt.inc `rt_internal`;
`LSWTCS_GLD32F=1` restores the old format). Verified: depth resolve ranges and the mask buffer now match
the PC's; shadows, warm light and table glows visible. New diag: `resdump_now` file in sdmc:/lswtcs ->
res_<dest>_<w>x<h>.png for every resolve target / colour RT, same names and normalisation as the PC's
`resdump_now`, for buffer-by-buffer PC/Switch diffs. Watch for z-fighting or shadow acne from 24-bit
unorm depth (the PC uses D32F) in other levels.

## 6. Open items outside this task (leave unless asked)

Shadows FIXED (§5b); Eden displays only ~2 fps of the ~40–60 presented; DIM-MISMATCH
(cube slots bound to 2D fetches on 3D models); audio output (M4). Nothing from 2026-10-07's Switch work
is committed yet.
