# HANDOFF 2026-10-09: rebuild switch-mesa so glthread works, then continue the glthread optimisation

Audience: the next agent. Goal: move Mesa's per-call driver work off the render thread (core 1) onto core 2 with
Mesa's **glthread**, the biggest available render-thread win. Read `HANDOFF_2026-10-08_switch.md` §4–§6 (standing
rules: ask before every SD-card copy, never delete on the SD card, ask before closing an Eden the user has open,
one Eden at a time; the user may interrupt test runs, so tell them before a run and ask them not to move the
character during measurements).

## 1. Why

Hardware, max safe Mariko clocks, heavy level scene (~3,200 draws/frame), 2026-10-09 log
(`[GLPROF] per replay`): frame 54 ms = **render thread CPU 32 ms** (of which Mesa draw calls `draw=13`, binds 3,
samplers 1.9, RTs 1.8, programs 1.65, texture sources 1.6) + **19 ms waiting for the GPU** (`gpuwait`). Core usage:
core 0 = all guest threads under the GIL (~20 % idle), core 1 = render thread (the bottleneck), core 2 = shader-compile
workers + audio output (mostly idle after boot). glthread makes GL calls on the render thread only *record* commands
into a queue; a Mesa worker thread (we put it on core 2) executes them in the driver. Expected: most of the ~13 ms
driver time leaves the render thread.

## 2. What is already in the tree (switch/source)

- `gl_backend.cpp enable_glthread()` (called once from `render_thread_main` after GL setup):
  `_glapi_get_context()` → `lsw_set_new_thread_core(2)` → `_mesa_glthread_init(ctx)` → `lsw_set_new_thread_core(-1)`
  → `_glapi_set_dispatch(_mesa_get_dispatch(ctx))`; prints `[GL] glthread ON (dispatch A -> B)` on success
  (glapi dispatch switched to the marshal table) or `did not engage`. **Default OFF**: `LSWTCS_GLTHREAD=1` in
  `sdmc:/lswtcs/env.txt` turns it on. The extra re-bind via eglMakeCurrent was removed: on this EGL platform it never
  reached `_mesa_make_current`, hence the direct `_glapi_set_dispatch`.
- `platform_switch.cpp`:
  - `__wrap_pthread_create` + `lsw_set_new_thread_core(core)`: **thread_local** hint; threads created by that thread
    pin themselves to `core` and set priority **0x2B** (above the compile workers' default 0x2C: Horizon does not
    time-slice equal priorities, a busy compile worker on core 2 starved the glthread worker).
  - `__wrap_thrd_create` (CMake `-Wl,--wrap=thrd_create`): **real bug fix, keep it.** newlib defines
    `thrd_success = 4`; switch-mesa 20.1's `u_thread_create()` does `if (ret) return 0;` (assumes 0), so every Mesa
    worker thread was created and then reported as failed → `util_queue_init()` returned false → glthread could never
    start, and Mesa's other queues (e.g. the shader disk-cache writer) never ran either. The wrapper maps 4 → 0.
    Side effect to watch: Mesa's disk-cache queue now really runs (writes to `sdmc:/lswtcs/mesacache`).

## 3. Current blocker (Eden, 2026-10-09)

With the thrd_create fix and the priority fix, `LSWTCS_GLTHREAD=1` gives a **black screen**: the render thread never
returns from `_mesa_glthread_init`. Evidence (`stdout.log`): exactly one `[NX] new thread pinned to core 2, priority
0x2B` (the glthread worker started), then no `[GL] glthread ...` line, no replays, audio keeps running.
`_mesa_glthread_init` (Mesa 20.1 `src/mesa/main/glthread.c`) ends with
`util_queue_add_job(..., glthread_thread_initialization, ...)` + `util_queue_fence_wait(&fence)`: the start-up job never
completes. `glthread_thread_initialization` is small: `ctx->Driver.SetBackgroundContext(ctx, &stats)` (→
`st_set_background_context` → `smapi->set_background_context` only if non-NULL) and `_glapi_set_context(ctx)`. **Where it
hangs is NOT proven.** Candidates, in order of likelihood:
1. The job never runs: `util_queue_thread_func` blocks before taking it (C11 `mtx_*`/`cnd_*` from newlib on libnx;
   `u_thread_setname` → `pthread_setname_np`; check every C11 call in `src/util/u_queue.c` / `u_thread.h` for code that
   assumes Mesa's own C11 return values, like the thrd_create bug).
2. The fence never signals: non-futex `util_queue_fence` (mtx + cnd) on Horizon; `util_queue_fence_signal` vs
   `_util_queue_fence_wait` with newlib's C11 return values.
3. `SetBackgroundContext` reaches switch-specific code in `src/egl/drivers/switch/egl_switch.c` / its `st_manager`
   that blocks (e.g. takes a lock the render thread holds).

## 3a. RESOLVED (2026-10-09 s2): glthread runs in Eden

- **Cause = candidate 3, found by reading the code (no markers needed):** `st_set_background_context` calls
  `smapi->set_background_context` unconditionally (the NULL check is an `assert`, compiled out by `-Db_ndebug=true`);
  switch-mesa's `egl_switch.c` allocates its `st_manager` with `CALLOC_STRUCT` and sets only `get_param`. The worker
  jumped to address 0 and died (Eden doesn't report unmapped accesses), the start-up fence never signalled.
- **Second bug, same root (driver written without glthread in mind):** DRI calls `st->thread_finish()` before its
  swap flush and before unbinding a context; `egl_switch.c` didn't, so `eglSwapBuffers` would flush the pipe context
  while the worker still draws on it, and read `cur_slot` before the worker's framebuffer validate set it.
- **Fix:** `switch/mesa/mesa_glthread_edits.py` (no-op `switch_st_set_background_context`; `thread_finish` at the
  start of `switch_swap_buffers` and on the current context in `switch_make_current`), hooked into
  `build_mesa.sh`. Mesa rebuilt + installed, NRO rebuilt (`switch/build/lswtcs.nro`, 12:33; not on the SD card).
  Note: this rebuild's tree ended up in `switch/mesa/build/dl/mesa-20.1.0-rc3` (a shell's cwd was inside `src/`, `rm`
  failed and the `&&` chain hid it); `build_mesa.sh` now fails at that point instead.
- **Eden verify:** `[GL] glthread ON`, hub reached at 52 s, screenshot correct, no crash.
- **Eden hub A/B (same build, idle, `[GLPROF] per replay`, ms):**

  | | replay | draw | bind | resolve | present | state | sampler | other | fps |
  |---|---|---|---|---|---|---|---|---|---|
  | off | 11.6 | 3.1 | 1.47 | 1.0 | 0.21 | 0.32 | 0.40 | 1.56 | 52–54 |
  | on  | ~10.1 (9.2–11.1) | 0.35 | 0.7 | 0.07 | 0.00 | 0.45 | 0.43 | **4.8** | 51–52 |

  Driver work left the render thread as intended, but "other" rose by ~3.2 ms = the render thread blocked on the worker
  at sync points (most likely the swap's `thread_finish`, plus whatever GL calls glthread can't marshal). Net replay
  −1.5 ms; fps unchanged because Eden's hub is guest-bound (wall ~19 ms). **Next = §5.1:** find the sync points
  (uncomment the `printf` in Mesa's `_mesa_glthread_finish_before`, glthread.c, or count calls into it), remove them,
  then measure on hardware.

## 3b. Hardware result, default ON, frame-end pipeline (2026-10-09 s2)

- **Console A/B** (user's runs, max clocks; logs in `switch/hwlogs/2026-10-09_glthread_{off,on}/`; in-game replays
  only, different play sessions): render thread median 22.8 → 15.1 ms, p90 43.9 → 22.0 ms; Mesa draw 6.3 → 0.3 ms;
  frame wall median 48.7 → 39.8 ms; worst 5-s fps p10 13.8 → 16.1, min 8.3 → 13.9. User: "much smoother, noticeably
  fewer stutters, shorter loads". GPU wait (~25 ms median) is now the largest single part of a heavy frame.
  → **`LSWTCS_GLTHREAD` default ON** (`=0` off). The SD card's env.txt also has `LSWTCS_GLTHREAD=1` (harmless).
- **Sync profiler** `LSWTCS_GLSYNCLOG=1` → `[GLSYNC]` (hook in Mesa's `_mesa_glthread_finish_before`): in Eden all of
  the worker wait was the two `glFenceSync` per frame (3.2 ms); map/unmap/GetError ~0.01 ms.
- **Frame-end pipeline** `LSWTCS_GLPIPE` (default on with glthread): Mesa export `lsw_glthread_call(fn, arg)` queues a
  callback on the worker in order with the GL commands (`_mesa_glthread_finish` also waits for it), and
  `lsw_egl_swap_buffers_worker` swaps from the worker. The arena fence wait + map/copy (`arena_job_run`), the arena
  fence (`arena_fence_job`), GPU-queue cap + swap + its fence (`swap_job`) run on the worker; the render thread reports
  the frame done after recording and before the next replay waits (`pipewait`) until the worker finished the previous
  upload (f.arena returns to the guest in `gpu_frame_take`) and has ≤ 1 swap outstanding. **+1 frame of latency** by
  design. In `[GLPROF]`, `swap`/`gpuwait`/`fwait` are then worker times.
  Eden hub: render thread replay ~10.1 → **~6.2 ms**, `[GLSYNC]` only a rare GetError, 51–52 → **55–56 fps**,
  screenshot correct. (state/bind rose ~0.5 ms each in Eden; watch on hardware.) **Not yet run on hardware.**
- **Hardware run with GLPIPE (2026-10-09, `switch/hwlogs/2026-10-09_glpipe/`): GLPIPE now default OFF.** fps the same,
  slight input latency, **extremely choppy audio**. Frame median 39.7 → 35.8 ms, p90 59 → 58 ms; render-thread
  replay *rose* 15 → 33 ms, all in bind/state/draw = blocked on a full glthread queue: the worker does the driver work
  plus the GPU wait serially, and the GPU (~22–25 ms) is the limit. Audio: nouveau_fence_wait is a **pure spin** on
  the Switch (it only yields on PIPE_OS_UNIX); with GLPIPE the GPU wait spun on core 2 at 0x2B, same priority as the
  audio output thread (Horizon: no time-slicing of equal priorities). Fixes kept regardless of GLPIPE: fence wait
  sleeps 50 µs per poll after 16 polls (`mesa_glthread_edits.py`), glthread worker 0x2C (below audio 0x2B), core-2
  compile worker 0x2D. Eden after the fixes: hub 55–56 fps, worker 0x2C. Retry GLPIPE once the GPU cost is down.
- Mesa was rebuilt clean from the tarball with all edit scripts (`switch/mesa/build/src`); the stale
  `switch/mesa/build/dl/mesa-20.1.0-rc3` tree from the misplaced build can be deleted (PC disk, not the SD card).

## 4. Plan (historical; steps 1–4 done, see §3a)

1. **Find the hang (no guessing).** Add a Mesa edit script `switch/mesa/mesa_glthread_edits.py` (same pattern as
   `mesa_egl_share_edits.py`: exact-string edits, idempotent, assert one match) that inserts `printf`+`fflush` markers
   in `util_queue_thread_func` (start, after setname, before/after taking a job), `glthread_thread_initialization`
   (before/after SetBackgroundContext, after _glapi_set_context), `util_queue_fence_signal`, and `_util_queue_fence_wait`.
   Hook it into `switch/tools/build_mesa.sh` after `mesa_egl_share_edits.py`. Alternative/extra: Eden's GDB stub to see
   the worker's stack while it hangs.
2. **Rebuild switch-mesa:** `switch/tools/build_mesa.sh [jobs]` (downloads Mesa 20.1.0-rc3, sha256-checked, + devkitPro's
   switch/mesa patches at the pinned commit, applies our edit scripts, meson/ninja, installs to
   `switch/mesa/build/install/opt/devkitpro/portlibs/switch/lib`; needs the devkitPro pacman packages listed in the
   script). `switch/CMakeLists.txt` links that install automatically (`LSW_MESA_CACHE_LIB`). Then `switch/build.sh`.
   Memory: don't run the Mesa build and the game build at the same time (24 GB machine, GCC -j4 already tight).
3. **Fix the cause** in the same edit script (e.g. correct C11 return-value checks on newlib, or a switch
   `set_background_context` that binds the context on the worker the way `egl_switch.c` binds it on make-current).
   Keep the markers behind an env check or remove them once it works.
4. **Verify in Eden** with `LSWTCS_GLTHREAD=1` (temporary line in Eden's env.txt, remove afterwards):
   `[GL] glthread ON (dispatch A -> B)`, hub reached, screenshot correct (`touch .../shot_now`), no crash.
   Compare `[GLPROF] per replay` with glthread off/on, same scene, user not moving: the render thread's `draw=`,
   `bind=`, `state=`, `sampler=` should drop.

## 5. Then continue the glthread optimisation

1. **Remove sync points.** Every call that makes the render thread wait for the glthread worker throws the gain
   away: `glMapBuffer*`/`glUnmapBuffer` (the per-replay arena upload `[GLUPLOAD] map/memcpy/unmap`), `glGet*`,
   `glGetError` (the periodic `err 0x%X` prints in `[GLDRAW]`), `glClientWaitSync`/fences (`gpuwait`, `RTSYNC`),
   `glReadPixels` (captures only). Mesa 20.1 glthread syncs on most of these. Options: persistent-mapped arena
   (`glBufferStorage` + `GL_MAP_PERSISTENT_BIT|GL_MAP_COHERENT_BIT`, mapped once) — check what Mesa 20.1 glthread does
   with it; move `glGetError` behind a diag env; keep fence waits but check them less often.
2. **Pin and prioritise:** worker on core 2 at 0x2B (done). Check it doesn't starve the audio output thread (core 2,
   0x2B): if audio crackles, give the glthread worker 0x2C and the compile workers 0x2D.
3. **Measure on hardware** (user runs at max safe Mariko clocks; the console's `stdout.log` `[GLPROF]` lines):
   target is render-thread CPU well below the current 32 ms/frame in the heavy scene. Make `LSWTCS_GLTHREAD` default
   on only after a clean hardware run (copy to `E:/lswtcs/lswtcs.nro` only with the user's OK; the forwarder NSP
   starts it automatically).
4. Record results here and in memory.

## 6. Other state from 2026-10-09 (not glthread)

- Pixel shaders are now built **without `precise`** (`gl_bridge.cc`, `LSWTCS_GLPSPRECISE=1` restores it) to let Mesa
  fuse mul+add (GPU-side cost). Saved program list moved to `sdmc:/lswtcs/glprogs2` (old `glprogs` left alone).
  **Not yet measured on hardware**: check `gpuwait=` in `[GLPROF]` against the 19 ms above.
- The build with glthread default OFF renders normally in Eden (hub 54–55 fps, render thread 12–13 ms/frame in Eden).
  As of this handoff it is NOT on the SD card (`E:/lswtcs/lswtcs.nro` is the band-fix build).
- Other CPU offload ideas (separate from glthread): move the GPU-command thread's host work (`[GPUPROF] walk 5.5 +
  record 13 ms/frame`, inside guest thread `0x822D2AB0` under the GIL on core 0) to a host worker; native replacement
  for the guest reverb `sub_827E2208`; partial GIL removal. See the co-op/perf discussion in
  `HANDOFF_2026-10-09_regen.md` §12 and `HANDOFF_2026-10-09_netcoop.md` §9.
