# HANDOFF 2026-10-07 (Switch): get the port running as Switch homebrew in Eden

**Your task:** turn the working PC runtime into a Nintendo Switch homebrew build (`.nro`) that boots
in the Eden emulator, then renders, plays audio and takes input. Work in milestones (§5); each one
ends with a test you can run in Eden. Keep the PC build working the whole time: it is the reference
you compare against.

Read first: §1 (rules), §2 (state), §3 (what the Switch changes). Earlier context:
`HANDOFF_2026-10-06_perf.md` (build, measurement, user rules), `HANDOFF_2026-10-06_singleview.md`
(memory model), `Eden/EDEN_MEMORY.md` (Eden usage, probe results; not in git).

## 0. PROGRESS (2026-10-07, later the same day) — read this first

**Decision (user): M3 graphics = OpenGL via switch-mesa first, deko3d later.** switch-mesa 20.1,
switch-glad and switch-libdrm_nouveau are installed.

Done:
- **Switch build exists:** `switch/CMakeLists.txt`, `switch/build.sh` (→ `switch/build/lswtcs.nro`).
  Game code + the UNCHANGED PC runtime (`main.cpp`, `kernel_stubs.cpp`, `gpu_ringbuf.cpp`) + XMA +
  FFmpeg (aarch64 NEON files) + Switch platform files. All compile; first full link pending.
- **Build gotchas:** devkitPro's toolchain file only accepts its MSYS2 cmake, which writes `/home/...`
  paths into generated unity/PCH files that the native GCC cannot open → hand-made unity files with
  relative includes, no PCH. CMake de-duplicates repeated `-include` → use `SHELL:-include x`.
  **GCC needs ~2-2.5 GB per game-code unity unit: `LSW_JOBS=4` (default). 24 jobs ran the 24 GB PC
  out of memory.** Full game-code build ≈ 1 h with GCC (clang on PC: 5 min).
- **Compat:** `switch/include/lsw_switch_compat.h` (forced include) supplies clang-only builtins
  (`__builtin_assume`, `__builtin_rotateleft32/64`, `__builtin_debugtrap`, `__builtin_ia32_rdtsc`).
- **Platform layer = Win32-subset shim, not an interface refactor (deviation from M1 below):**
  `switch/include/win32/windows.h` + `switch/source/win32_switch.cpp` implement the ~100 Win32 calls
  the runtime uses: files (POSIX, 64-bit offsets, dispositions, FindFirstFile wildcards), a faithful
  wait dispatcher (events incl. PulseEvent, semaphores, recursive mutexes, waitable timers,
  WaitForMultipleObjects any/all), time (armGetSystemTick), diagnostics APIs fail cleanly. PC code
  is untouched except `LSW_DATA_DIR` (data dir macro) and three `#ifdef __SWITCH__` spots in
  main.cpp (init call, guest window, x86 rsp print). → M1's PC test is trivially satisfied.
- **Guest window (Eden probe #2, see EDEN_MEMORY.md):** `svcMapPhysicalMemory` unusable (no system
  resource); heap → `svcCreateCodeMemory` → `MapOwner` RW at exact offsets works. Backed ranges
  (`switch/source/platform_switch.cpp`): 0x7F000000 16 MB (GPU writeback/MMIO), 0x80000000 512 MB,
  0xE0000000 256 MB (pool), 0x100000000 64 MB (table) ≈ 850 MB.
- **EDEN DOES NOT FAULT ON UNMAPPED ACCESS** (reads 0, drops writes). A guest access outside the
  backed ranges is silent in Eden and a crash on hardware.
- Threads: `--wrap=pthread_create` gives every thread ≥ 8 MB stack; runtime main runs on a 16 MB thread.
- Xenia base/XMA sources build as "Linux" (`-D__gnu_linux__`); futex/gettid map to
  svcWaitForAddress/svcSignalToAddress (`switch/source/linux_shim.cpp`). FFmpeg config overrides in
  `switch/include/ffmpeg/config.h` (no mmap etc.).
- M2 backends (`switch/source/null_backends.cpp`): no draw translation (`lsw_gpu_prepare` → 0), no
  present, audio paced by kernel_stubs' own timer (`host_audio_open` → false). XMA decoding is real.
- **SD data:** `switch/tools/link_sd.ps1` (user-approved): hard links / junctions in
  `%APPDATA%\eden\sdmc\lswtcs` to the user's existing files, no copies; `env.txt` from
  `switch/sd/lswtcs/env.txt` (PC launch flags). Run: `switch/tools/eden_run.sh [secs]`.

**M2 PASSED in Eden (2026-10-07 14:50):** `lswtcs.nro` (113 MB) boots headless to the title screen:
first 53 file opens identical to a PC boot (`switch/scratch/pc_ref_*.log`, gitignored), then the
title idle loop (attract movie `DEMOINTRO.WMV` absent by config → title music restarts every ~40 s);
XMA decoder up (320 contexts); 60 fps locked, `[FTSTAT] ft_sum x1.000`; no faults in 3 min.
Fixes needed on the way: `std::thread::detach()` throws on libnx (pthread_detach unsupported) →
`--wrap=pthread_detach`; one generated file included `<windows.h>` and two call `GetCurrentThreadId`
(guarded/declared in ppc_recomp.337/340/342 — **regenerate patches/recomp_output_edits.patch before
committing**). Diagnostics kept: libnx exception handler → `sdmc:/lswtcs/crash.log`, `exit`/`abort`/
`__cxa_throw` wrappers log callers; offsets are relative to `_start` → `aarch64-none-elf-addr2line
-fCpe switch/build/lswtcs.elf <off>`. Memory map measured on PC with `memscan_now` (see
platform_switch.cpp); three stray guest pages at 0x0/0x01000000/0x79600000 are backed, not explained.

**M5-lite input DONE (2026-10-07 14:55):** `win32_switch.cpp` resolves `LoadLibraryA("xinput1_4.dll")`
/ `GetProcAddress("XInputGetState")` to a libnx pad reader (player 1 + handheld; face buttons by
label: Switch A = Xbox A; ZL/ZR = full triggers; +/- = Start/Back). kernel_stubs unchanged; `press_now`
works from `sdmc:/lswtcs/press_now` (cwd). **Eden: START then A from the title loads the hub** — the
same 14 `levels\Map\...` files as a PC hub run, no crash, 60 fps, `ft_sum x0.999`. Driver script:
`switch/tools/eden_hub.sh` (detects progress by level files, since null graphics reports draws=0;
not yet run end to end in this form — the test above was by hand). Real-pad input untested.

**M3 stage 1 (2026-10-07 15:20): shader chain + GL context work in Eden.**
- SPIRV-Cross checked out at `SPIRV-Cross/` (tag vulkan-sdk-1.4.363.0, `patches/SPIRV-Cross.base`,
  user-approved); xenia-canary `third_party/glslang` submodule initialised (xenia's pin).
- Chain: Xenos ucode → Xenia `SpirvShaderTranslator` (no Vulkan device: `Features(false)`, stand-in
  `switch/include/spirvshim/xenia/ui/vulkan/vulkan_device.h`) → SPIR-V → `switch/source/lsw_spirv_glsl.cc`
  (SPIRV-Cross, own TU: its spirv.hpp clashes with glslang's) → GLSL 4.30.
- **xenia-canary edits (regenerate patches/xenia-canary.patch before committing):** the
  system-constants block was not GLSL-expressible: `alpha_to_mask` declared out of memory order
  (moved in the member list + `SystemConstantIndex`), and `color_exp_bias` (vec4) at offset 180 →
  `lsw_padding_std140[3]` in `SystemConstants` puts it at 192.
- PC check: `switch/tools/spirv_glsl_test.sh` → 22/22 xenia/shdump shaders convert and validate
  (glslang standalone built in switch/scratch/glslang_build). NOTE shdump files are host order;
  `Shader` byte-swaps its input, so the test swaps on load (unswapped input crashes AnalyzeUcode).
- Switch: `switch/source/gl_backend.cpp` replaces the null GPU backend: render thread owns EGL/GL
  (GL calls only there), clears to the PC backbuffer colour (0.05,0.05,0.08) and swaps per guest
  present; `lsw_gpu_prepare` translates each new VS/PS on the guest thread, queues GLSL, mesa
  compiles it on the render thread, still returns 0 (no replay). Eden: `NV120 | 4.3 Core Mesa
  20.1.0-rc3`, **19/19 title shaders compile in mesa**, 60 fps, x1.000. User saw a "purple screen"
  (expected: the dark clear colour, unconfirmed); swap counter `[GL] render thread:` added since.

**M3 stage 2 (2026-10-07 16:30): game draws replay in Eden (title, LSWTCS_GPURT=0 path).**
- `switch/source/gl_bridge.cc`: SPIR-V `lsw_gpu_prepare` (Vulkan-style modifications + system
  constants; clip planes in the same arena block at +512, UBO 5); `LswGlShader` objects
  (`switch/include/lsw_gl_shader.h`) ride in the recorder's vs/ps/gs_dxbc pointers. Texture swizzle
  via new `lsw_gpu_texture_mapping()` (LSWTCSRuntime/xenos_dxbc/lsw_gpu_texture.cc, PC unaffected).
- `switch/source/gl_geometry.cc`: point/rect/quad geometry shaders = Xenia's Vulkan generator body,
  extracted at configure time by `switch/tools/extract_gs_body.cmake` (no copy in the repo).
- `switch/source/gl_backend.cpp`: render thread replays `g_gpu_frame_render` into a 1280x720 FBO
  (glClipControl upper-left/0..1 via eglGetProcAddress — switch-glad stops at 4.3), 512 MB SSBO
  shared memory (4 x 128 MB bindings), arena = one UBO/index buffer per frame, GL texture/sampler
  caches, blit-flip to the window. Env: `LSWTCS_GPURT=0` (switch/sd/lswtcs/env.txt).
- **Fixes found in Eden:** (1) same-named uniform blocks across stages must match in GL → blocks get
  `_vs/_ps/_gs` suffixes; (2) **an early `return` in a geometry shader hangs Eden's shader
  recompiler** (found with the `LSWTCS_GLTEST=1` ladder in gl_backend.cpp) → `lsw_glsl_gs_remove_returns`
  rewrites main into do{...}while(false)+break; (3) programs without PS get an empty FS.
- Result: 54-87 draws/frame at the title (PC: 50-70), 10 programs, 54 textures, no GL errors, runs
  continuously. Eden still logs non-fatal "Invalid insn" asserts on new shaders (also for self-test
  shaders that render fine). **Picture not yet checked by eye.** `LSWTCS_GLTEST=1` is set in the SD
  env.txt only (not the repo copy) — remove it when done.

**TITLE SCREEN RENDERS IN EDEN (2026-10-07 17:30)** — logo, starfield, banner, characters, ships
(capture: `shot_now` file in sdmc:/lswtcs -> shot_<replay>.png + per-draw `[GLDRAWCAP]` log). Two more
Eden/mesa findings, both found with the pixel-checked `LSWTCS_GLTEST=1` ladder (gl_backend.cpp):
- **Eden drops every draw whose shader READS A STORAGE BUFFER** (any size, VS or PS; silently, plus
  "Invalid insn" in Eden's log). Buffer textures work: shared memory is now an R32UI `usamplerBuffer`
  over the 512 MB buffer on unit 40 (`kLswShmTextureUnit`); `lsw_spirv_glsl.cc` rewrites
  `xe_shared_memory[A].shared_memory[B]` -> `texelFetch(tbo, A*33554432+B)`. Shaders that WRITE shared
  memory (memexport) are refused with a log line — none seen yet.
- Front face: with glClipControl(UPPER_LEFT) the guest's CW/CCW maps directly (my mirrored mapping
  culled every single-sided 2D quad).
Debug switches (SD env.txt only): `LSWTCS_GLTEST=1` (ladder + captures at replays 300/1500 + control
square), `LSWTCS_GLDBG=1..5` (state off / magenta PS / show tex0 / interp0 rgb / interp0 alpha),
`LSWTCS_GLFACE=1` (swap winding).
Open: Eden shows ~2 fps of our ~50 presented/s (its GPU emulation; check what costs — 512 MB buffer
texture, per-frame arena re-upload); 3D models bind cube-dimension texture slots to 2D fetches
("DIM-MISMATCH", dummy bound — check the PC path's behaviour); no render targets/resolves yet.

**Hub (2026-10-07 18:00):** first try exhausted Eden's GPU address space ("Failed to allocate free
space in the GPU AS!" in Eden's log; render thread blocked in the driver, game kept running) → arena
is now a ring of 3 fixed buffers (no per-frame glBufferData orphaning) + texture cache budget with LRU
eviction (`LSWTCS_GLTEXMB`, default 192). Now: hub replays 750 draws/frame at ~40 replays/s, 156
textures = 38 MB, arena buffers 123 MB, no GL errors. **But the first hub frame takes ~2 min**: Eden
recompiles ~60 new mesa shaders + builds host pipelines (its log fills with FSWZADD/RAM/SAM stubs) —
an Eden-side cost (async shader building is an Eden setting: ask the user before changing it). Hub
picture: HUD (hearts, "Insert controller") only, world black = same as PC before EDRAM RTs/resolves.

**M3 stage 3 DONE (2026-10-07 19:00): THE CANTINA HUB RENDERS IN EDEN** (world, characters, HUD,
lighting) — `switch/source/gl_rt.inc` ports gpu_xedraw.inc's EDRAM RTs (2048-row targets keyed by
base/pitch/format, D32F_S8 depth), resolves (GLSL blit, modes 1:1 / 4x avg / 2x avg / single sample +
clears), resolved-texture sources (sized views, predicated-tiling composites, stale-probe drop) and
present-from-frontbuffer-resolve. All host textures are 1-layer GL_TEXTURE_2D_ARRAY (shaders sample
sampler2DArray). Swizzle overrides for resolved sources go through per-draw system-constant copies
(patch ring, `SystemConstants::texture_swizzles` at +96). `LSWTCS_GPURT=0` removed from env.txt (the
old single-FBO path still exists behind it). Hub: 790 draws + 21 resolves/frame, 13 RTs + 18 resolve
targets = 179 MB, 87 textures 24 MB, arena buffers 110 MB.
Trap hit on the way: the index buffer binding (`GL_ELEMENT_ARRAY_BUFFER` = arena, VAO state) must be
re-bound every frame after the ring switch — without it Eden logs "Unmapped Read16 @ 0x1E30xx" floods
and the render thread crawls.

**Loading work (2026-10-07 evening; user: optimise for Switch hardware, not Eden):**
- Cost = Mesa compiling our big translated GLSL (up to 145 KB/program): GLSL front end ≈ link ≈ half
  each, nouveau backend codegen at first draw. Threads were all on one core → render thread pinned to
  core 1 (`pin_to_core`), compile workers core 2: compile times dropped ~10x on workers.
- Background compile (plan B, `LSWTCS_GLWORKERS=1`, default 0): works only if workers NEVER submit
  (no glFinish: switch-mesa's contexts share one unlocked GPU channel → Eden "executing_macro" asserts
  + render-thread hang); all-or-nothing frames (hold last complete) — but cold starts show stale
  frames for minutes, so off.
- **Plan A done: switch-mesa rebuilt WITH the shader disk cache** — `switch/tools/build_mesa.sh` +
  `switch/mesa/mesa_cache_edits.py` (devkitPro recipe pinned f103fe8; installs to
  switch/mesa/build/install, CMake links it by absolute path; needs switch-zlib + host meson/python
  mako/libexpat). Switch fixes: no mmap (persistent in-memory index + pwrite write-back), fixed driver
  id, no flock, **no util_queue (devkitPro's c11 call_once leaves u_queue's list uninitialised →
  NULL-list scribbles)**, **close temp file before rename (SD fs: EIO)**, timespec_get, Py3.12 mako
  check. Runtime: `MESA_GLSL_CACHE_DIR=sdmc:/lswtcs/mesacache`; `switch/source/mesa_shim.cpp`
  (pwrite/dirfd/fstatat/getuid). Result (Eden): hub cold 157 s → warm 62 s; warm per program:
  compile 0 ms, link 50-800 ms, first draw 125-685 ms (nouveau backend: NOT cached by Mesa 20.1).
- Cache entries are valid on any Switch / Eden with the same build (fixed id + GPU NV120 + GLSL
  hash) → a pre-built cache can be copied to the SD card (personal use: derived from the game).
Next for loading: cache nouveau backend code too (nvc0_program_translate); program-list warm-up mode +
automated level tour to pre-build the cache.

Next: Eden's ~2 fps display + 2-min first-hub-frame shader stall; level test; commit (regenerate
patches first); M4 audio (audren). (EDRAM RTs → FBOs, resolves, textures via
`lsw_gpu_texture.cc` decode, system constants fill for the SPIR-V layout), title screen first. M3 shader path: Xenia's SPIR-V translator (needs glslang SPIRV headers, vendored; and a
Vulkan device-features header, stubbable) + **SPIRV-Cross (not vendored yet)** → GLSL for mesa.

---

## 1. Rules (from the user, mandatory)

- **Game data, keys, dumps:** the game files come only from the user's own Xbox 360 disc; Switch
  keys only from the user's own console. Never download, request, read, print, copy or commit them.
  Point Eden at where the user keeps them, nothing more. Details in `Eden/EDEN_MEMORY.md`.
- **Ask before** changing Eden's configuration, installing firmware into it, or installing new
  devkitPro packages (see §5 M3 decision).
- One game instance at a time (PC). You may close a running PC game to relink. Never
  `LSWTCS_AUTOSTART=1`; drive the game with `press_now` (see the perf handoff).
- Back up before risky changes (`./backup.sh <label>`). Patch C/C++ with the Edit tool or Python
  scripts written with the Write tool; no heredoc/`sed` on C code.
- Commit/push only when asked. Before any push: privacy check (`git grep -i` for the user's name,
  no `C:\Users\...` paths, no key/dump locations), regenerate `patches/` (§6).

## 2. Where things stand (commit a9d27c0)

PC runtime (Windows, MinGW clang, D3D12) plays the game: title, menus, hub, levels, cutscenes,
saves, audio, at a locked 60 fps.

Ready for Switch:
- **Single-view memory model works.** The 360 sees its 512 MB physical memory at 0x80000000,
  0xA0000000 and 0xC0000000. Switch homebrew cannot map one block at several writable addresses
  (Eden probe #1), so guest addresses 0xA0000000-0xDFFFFFFF are folded onto the 0x80000000 copy in
  software: `PPC_HOST()` / `ppc_view_base[]` in `Convert 360/LSWTCS/output/ppc_context.h`,
  `lsw_host()` in runtime code. With `LSWTCS_SINGLEVIEW=1` the alias ranges are unmapped and a full
  level plays with no faults. Cost: +13% guest CPU on PC.
- **Game time follows real time at any frame rate.** The game was built for 30 fps vsync; two
  rounding errors made it run 10% fast at 60 fps (cutscenes drifted from their audio). Fixed in
  `kernel_stubs.cpp` (search `FTFLOOR`, `50000000`). Guest timer = 50 MHz. This matters on Switch:
  whatever frame rate you reach, game time stays correct, so 30 fps is a valid first target (it is
  the game's native rate).

Not Switch-ready: everything that talks to the OS (§4), and the GIL (one host thread runs all guest
threads; the Switch has 3 usable cores, see memory note `switch_multithreading_constraint`).

## 3. What the Switch changes

Facts already established (Eden probe #1, `switch/probes/memalias`):
- No multi-mapping of writable memory (shared memory not available to apps; code memory alias is
  read-only). Hence the single-view fold, already done.
- Process memory budget ~3.2 GB (Eden). 39-bit address space; reserving a 4 GB guest window is easy.
- **No overcommit.** On Windows the runtime commits ~3.6 GB of guest address space and Windows only
  backs pages the game touches (~1.2 GB working set in a level, including 82 MB code and D3D).
  On Switch every mapped page is real memory. Map only what the game uses: the image
  (0x82000000-0x8346xxxx), the physical range 0x80000000-0x9FFFFFFF, the virtual heap the game
  allocates (0x4xxxxxxx), thread stacks, the 0x7FC8xxxx / 0x7FEAxxxx MMIO pages the runtime emulates,
  and the dispatch table (`PPC_FUNC_TABLE_BASE` = 0x100000000, 64 MB, host pointers). Measure peak
  per-region use on PC first (add a log of the guest allocators' high-water marks).
- Eden marks every SVC as allowed for homebrew; real hardware enforces the title's permissions.
  Passing in Eden is necessary, not sufficient.
- Eden's CPU speed says nothing about hardware speed (it JITs on the PC). Use Eden for correctness;
  measure performance on hardware later.

Toolchain installed (`/c/devkitPro`, use its MSYS2 shell:
`/c/devkitPro/msys2/usr/bin/bash.exe -lc 'cd <dir> && make'`): devkitA64 r29.2 (GCC 15.2),
libnx 4.12, **deko3d 0.5 + uam 1.1** (deko3d's GLSL-to-shader compiler), switch-cmake, switch-tools.
**Not installed:** switch-mesa (OpenGL/EGL), switch-sdl2, switch-glad.

## 4. Platform dependency inventory (runtime, ~10.8k lines)

| Area | Where | PC today | Switch replacement |
|---|---|---|---|
| Guest memory | `main.cpp` ~640-720 | `VirtualAlloc` reserve/commit, `MapViewOfFileEx` | reserve with `virtmemFindAslr`, back only used ranges (§3) from the heap (`svcSetHeapSize` + `svcMapMemory`) or `svcMapPhysicalMemory` (check app permissions first; probe it in Eden) |
| Files | `kernel_stubs.cpp` (Nt*File, `LSWTCS_GAMEROOT`, `D:\` mapping) | Win32 file API, GAME.DAT >2 GB | `fopen`/`fseeko` on `sdmc:/lswtcs/` (64-bit offsets) or `fsFs*` |
| Timer | `lswtcs_guest_timebase`, `lsw_guest_timebase`, limiter | QPC, `timeBeginPeriod` | `armGetSystemTick()` (19.2 MHz) scaled to 50 MHz; `svcSleepThread` |
| Threads, sync | `kernel_stubs.cpp` (GIL scheduler, Ke*/Nt* objects), `xma/` (`CreateEvent`) | `std::thread`, `std::mutex`, Win32 events | `std::thread`/pthreads work on libnx; replace Win32 events with `std::condition_variable` or libnx `UEvent`. Thread priorities/cores: `svcSetThreadCoreMask` |
| GPU | `gpu_d3d12.cpp` (1.1k), `gpu_xedraw.inc` (1.5k), `xenos_dxbc/` | D3D12 replay on a render thread, Xenia DXBC shader translator, own window | §5 M3 |
| Audio | `host_audio.cpp` (138 lines) | XAudio2, 6-ch float 48 kHz voices, credits on `OnBufferEnd` | `audren` (libnx) or `audout`; downmix 5.1 to stereo |
| XMA decode | `xma/` (Xenia XmaContextNew + FFmpeg) | portable C/C++, Win32 events | builds as is after the event swap |
| Input | `kernel_stubs.cpp` (`XInputGetState` x2, `GetAsyncKeyState`, `press_now` file) | XInput | `padUpdate`/`padGetButtons` (libnx hid); map to XINPUT_STATE |
| Crash diagnostics | `main.cpp` (`AddVectoredExceptionHandler` x4) | guest-address fault reports | libnx `__libnx_exception_handler` (optional, later) |
| Config | `getenv("LSWTCS_*")` everywhere | environment variables | read `sdmc:/lswtcs/env.txt` at startup and `setenv()` each line (newlib has setenv) |
| Logging | `dbg_ram` -> `trace.log` | file in build dir | `sdmc:/lswtcs/trace.log`; Eden's SD is `%APPDATA%\eden\sdmc` |

Generated code (`Convert 360/LSWTCS/output`, 226 files, 349 MB of C++, 82 MB of x86 code):
- Portable in principle: byteswaps are builtins, vector ops go through simde (has NEON backends),
  `mftb` calls `lsw_guest_timebase()`, `__int128` exists on aarch64.
- Remove `-march=native` (x86 MOVBE/AVX); use `-march=armv8-a+crc+crypto -mtune=cortex-a57`.
- `XE_PLATFORM_WIN32=1` is defined for the Xenia translator libraries (CMakeLists.txt): needs a
  Switch/Linux-like platform define and shims.
- Expect a long compile (PC: ~5 min for 163 unity units on a fast laptop) and a large NRO. Check early
  that an NRO of this size loads in Eden (M0).

Game data on the SD card (user's own, never committed): `GAME.DAT`, `GAME1.DAT`, `Audio\`, `LEVELS\`
etc. from the disc; `image.bin` and `pristine.bin` (built from the user's own `Default.xex`); the
`SHADERS` cache the UI text needs (see memory note `ui_text_shader_cache_fix`).

## 5. Milestones (each ends with an Eden test)

**M0. Compile check (no running yet).** Add a Switch CMake toolchain build (switch-cmake provides
`Switch.cmake` / `nx_create_nro`) that compiles the generated code + runtime with stubs for every
§4 platform function. Goal: a linked `.nro`, plus a list of compile errors outside the platform
layer (x86 intrinsics, MSVC/Win32 types in shared headers). Eden test: the NRO starts and prints
"hello" + its heap/budget to `sdmc:/lswtcs/trace.log` (copy the logging style of the memalias probe).

**M1. Platform layer on PC.** Move every §4 OS call behind one interface (e.g.
`include/lsw_platform.h`, `source/platform_win32.cpp`), with no behaviour change. PC test: `playtest.sh`
passes, `sv_catch.sh` NO FAULT, A/B timing unchanged. This keeps the Switch work from forking the
runtime.

**M2. Headless boot on Switch.** `source/platform_switch.cpp`: memory (only used ranges), SD files,
timer, threads, env file, log. GPU: keep the PM4 ring walk and draw recording, but skip replay
(nothing on screen). Audio and input stubbed. Eden test: trace.log shows the boot reaching the title
screen the same way PC does: `[GPUPROF] per frame ... draws=` around 50-70 at the title, the
`[FTFLOOR]` lines, no faults. Compare against a PC log of the same boot.

**M3. Graphics.** Decision for the user before you start (it needs a package install):
- (a) **deko3d** (installed). Native, fastest. Shaders must be DKSH, compiled from GLSL by uam.
  uam is a host-side compiler: practical only with an **offline shader pack** built on PC (record every
  translated shader during PC play, compile to DKSH, ship the pack; a shader missing from the pack =
  an invisible draw).
- (b) **OpenGL via switch-mesa** (not installed; `dkp-pacman -S switch-mesa switch-glad`). Compiles GLSL
  at runtime: complete coverage, stutter on first use, slower than deko3d. Easier to debug.
- Either way the shader source changes: today Xenia's `DxbcShaderTranslator` emits DXBC for D3D12.
  xenia-canary also has a SPIR-V translator (its Vulkan backend); SPIR-V -> GLSL via SPIRV-Cross
  feeds both uam and mesa. Check its dependencies before committing to it.
- Suggested order: (b) first for a correct picture, then (a) with the shader pack for speed.
  The replay logic in `gpu_xedraw.inc` (EDRAM render targets, resolves, tile composition, MRT,
  texture decode in `lsw_gpu_texture.cc`) is API-independent in intent; port it to the chosen API
  rather than rewriting it.
Eden test: the title screen matches `reference/` screenshots from PC.

**M4. Audio** (`audren`), **M5. Input** (libnx hid; also keep a file-trigger path for scripted tests).
Eden test: title music plays; Start/A reach the hub; pause menu works.

**M6. Hardware and performance.** Real-hardware run (the user decides when). Expect the GIL design to
be the limit: PC spends ~4.4 ms per frame of guest work under the lock; an A57 at 1 GHz is several
times slower. Plan: hit 30 fps (the game's native rate) single-core first, then replace the GIL with
real guest-lock emulation to use 3 cores (memory note `switch_multithreading_constraint`).
Possible cheaper fold: stack/image accesses don't need translation (singleview handoff §6).

## 6. Build, test and commit notes

- PC build: `export PATH=/c/msys64/mingw64/bin:$PATH; cd LSWTCSRuntime/build && ninja.exe LSWTCSRuntime`
  (configure only with `/c/msys64/mingw64/bin/cmake.exe`; Git Bash's `cmake` is devkitPro's and
  corrupts the cache). A change to `ppc_context.h` rebuilds all generated code (~5 min).
- PC launch settings: `launch_game` in `LSWTCSRuntime/build/boot_to_hub.sh` lists the env flags the
  game needs. Add `LSWTCS_SINGLEVIEW=1 LSWTCS_VCLOCK=0` to match the Switch model. These become
  `sdmc:/lswtcs/env.txt` on Switch.
- Scripts in `LSWTCSRuntime/build/` (not in git): `playtest.sh`, `hubprof.sh`, `sv_catch.sh` (runs
  under cdb, prints the stack of the first access violation; the game window freezes at a fault, so
  warn the user). First launch after a relink often fails to reach the title (Defender scan):
  retry once.
- Measure game speed with the `[FTSTAT]` log line (every 5 s, in VdSwap): `ft_sum` / real must be
  x1.000. Use it on Switch too.
- **Recompiler output is not in git.** To rebuild it: run XenonRecomp (+ `patches/XenonRecomp.patch`),
  then `python switch/tools/fold_recomp_output.py "Convert 360/LSWTCS/output"`, then apply
  `patches/recomp_output_edits.patch` (hand edits only). Before committing, regenerate that patch
  against a folded copy of `output_ref`:
  copy `output_ref` to a temp dir, run the fold script on it, `diff -ruN --strip-trailing-cr
  output_ref output > recomp_output_edits.patch` from a dir holding both, and check that applying it
  to the folded copy reproduces `output` (`diff -rq` empty). A plain diff against `output_ref` is
  17 MB.
- Eden: `Eden\eden-cli.exe <path to .nro>`; it prints nothing; read `%APPDATA%\eden\sdmc\lswtcs\`.
  `--debug <port>` starts a GDB stub (devkitA64-gdb is installed).

## 7. Pitfalls

- Don't let the Switch work regress the PC build; keep `playtest.sh` green after each step.
- Don't map the full 4 GB guest window with real memory on Switch (no overcommit, §3).
- The dispatch table stores 64-bit host function pointers inside the guest window at 0x100000000;
  keep it outside the folded range.
- Laptop throttles on battery: measure on AC.
- `Eden/` is gitignored and must stay that way; never add key or dump paths to tracked files.
