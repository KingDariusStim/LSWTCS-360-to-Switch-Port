# HANDOFF 2026-10-06 (perf): frame pipeline toward Switch performance

**Your task: keep cutting host-side frame cost so the port can run well on a Mariko Switch (Switch Lite).**
The next target is the render thread's own CPU cost (§5.1). Read §1–§4 before touching anything.

Earlier context: `HANDOFF_2026-10-06.md` (menus; the UI-text part is since fixed, see memory
`ui_text_shader_cache_fix.md`) and memory `performance_and_framelock.md` (top section = this work).

---

## 1. Where things stand

Two commits on `main` (pushed):

| Commit | What |
|---|---|
| `d0cc167` | Frame-pipeline fixes: hub 35–44 ms → ~18 ms/frame |
| `c80cc07` | D3D12 replay + present on a dedicated render thread: hub locked 60 fps |

Hub (Cantina, ~790–890 draws), 60 fps cap, same scene A/B:

| | Frame | Scheduler idle | Work under the GIL | Main thread |
|---|---|---|---|---|
| Synchronous (`LSWTCS_RENDERTHREAD=0`) | 17.26 ms | 43.9% | 9.7 ms | 7.4 ms |
| Render thread (default) | 16.66 ms, locked | 68.1% | 5.3 ms | 2.9 ms |

Verified in game with screenshots: title (incl. "Press START"), hub, pause menu (open, navigate, Resume),
a few minutes of movement. No deadlocks.

## 2. Working rules (from the user, mandatory)

- **One game instance only.** Check `tasklist | grep -i lswtcs` before launching.
- You may close the game at any time to relink (`taskkill //F //IM LSWTCSRuntime.exe`), but **say so in your
  message first**: the user is often playing. If a run you started misbehaves (e.g. stuck boot, looping audio),
  stop it and explain before trying again.
- **Never use `LSWTCS_AUTOSTART=1`** while the user plays. Use the `press_now` trigger file instead, and never
  spam it: press only after confirming the screen state from the logs.
- Back up before risky changes: `./backup.sh <label>`. It does **not** copy `LSWTCSRuntime/xenos_dxbc/`; copy
  that by hand (`backups/<label>_xdxbc/`).
- **Patch with Python scripts written via the Write tool** (template: `patch(path, [(old, new, count)])`,
  normalises CRLF), or the Edit tool. **Never bash heredocs/`sed` for C code**: a heredoc turned `\n` inside a C
  string into a real line break again this session.
- Never fake CPU speed; timing follows real time.
- Commits/pushes only when the user asks. Before every push, follow memory `github_repo.md`: regenerate
  `patches/` (they must match), `git grep` the tree and scan the diff for the user's name, personal paths and
  emails (the exact patterns are in that memory file), commit as
  the GitHub noreply identity. The user pushes straight to `main`.

## 3. Build, run, measure

**Build** (do not use Git-Bash `cmake`; it is devkitPro's and corrupts the cache):

```
export PATH=/c/msys64/mingw64/bin:$PATH
cd LSWTCSRuntime/build && ninja.exe LSWTCSRuntime
```

The link fails with "Permission denied" if the game is running: close it first.

**Measure: `LSWTCSRuntime/build/hubprof.sh <label>`** (local, untracked). Boots at the 60 fps cap, presses
Start then A only when the title is up (max 4 rounds, never while booting), waits until the hub (>500 draws),
settles 40 s, saves `prof_<label>.txt` (GPUPROF + SCHEDPROF) and `shot_<label>.png`, then **closes the game**.
`EXTRA="LSWTCS_RENDERTHREAD=0" ./hubprof.sh sync` for A/B. Takes ~2.5 min.

**Read the numbers this way:**
- `[GPUPROF] per frame:` (trace.log, every 60 presents): `wall`, `walk` (PM4 walk, excl. record), `record`
  (`prepare` = shader bridge, `vhash+copy` = vertex page hashing), `replay`, `present` (`fencewait`,
  `submit`), `fbfill`, `draws`, `vbytes` (vertex KB uploaded/frame, should be ~13 KB in the hub).
- `[GPUPROF] guest ISR … | replay: arena->upload … prepass … pso … bind …`: replay sub-stages.
- `[GPUPROF] guest wait for render thread`: guest blocked on the renderer (≈0 now).
- `busy=… (limiter idle=…)`: limiter time includes other guest threads running, so it is NOT pure idle.
- **The real throughput metric is SCHEDPROF** (`LSWTCS_SCHEDPROF=1`, every 5 s):
  work under the GIL per frame = `wall × (1 − IDLE%)`. Per-thread shares: id 0 = main, 6 = `sub_822D2AB0`
  (GPU submit worker: walk + record), 1 = XAudio worker.
- **This laptop (Ryzen AI 9 HX 370) varies ±30 % run to run** (big/compact cores, power state). Compare A/B in
  back-to-back runs, never against a number from another session.
- Uncapped (`CAP=0`) runs are pinned to the monitor's 60 Hz by flip-model presentation, so they can't show
  throughput above 60. Use SCHEDPROF idle instead.

**Diagnostics added this session:** `LSWTCS_NOREPLAY=1` skips replay + submit (window freezes; frame-time
upper bound for render-side savings); `LSWTCS_PRESENTYIELD=0` holds the GIL during the GPU fence wait (old);
`LSWTCS_RENDERTHREAD=0` synchronous replay (old); `LSWTCS_SHMALWAYS=1` uploads every vertex page every draw.
`NOREPLAY` with `FPS_CAP=0` starves boot (see §6), so run it capped.

## 4. What was done and where (so you don't redo it)

| Fix | Where | Effect |
|---|---|---|
| GPU fence wait released the GIL (`lswtcs_gil_blocking`, a GilYield wrapper callable from other TUs) | `gpu_d3d12.cpp` present; `kernel_stubs.cpp` | ~15 ms/frame of blocked guest time freed; uncapped boot works |
| Diagnostics env flags in the PM4 walker read once (`LSW_ENV_SET(name)` macro) | `kernel_stubs.cpp` `pm4_extract_ib` | walk 18–21 → ~1 ms. **Never call raw `getenv` in walk/record/replay paths.** |
| Shader bridge views `g_xe_regs` as `RegisterFile` in place (was an 80 KB memcpy per draw) | `xenos_dxbc/lsw_gpu_bridge.cc` | prepare 1.9 → 0.7 ms |
| Vertex residency per 4 KB page, decided at record time (`gpu_shm_track`; page hash once per walk epoch; epochs bump at walk start, after guest ISRs, after tile-drain yields; CP memory writes invalidate their page) | `kernel_stubs.cpp` | ~8 MB → ~13 KB uploaded/frame (old exact-range map ping-ponged on overlapping slices) |
| Frames replaced before the renderer took them carry their uploads forward (`LswGpuFrame::prelude`, `gpu_shm_carry_uploads`) | `kernel_stubs.cpp`, `lsw_gpu_frame.h` | required for record-time residency correctness |
| Arena allocator without zero-fill for vertex data (`alloc_uninit`) | `lsw_gpu_frame.h` | |
| Per-replay memo fetch constant → texture; RT rebind only after a real blit (`g_xd_blit_count`) | `gpu_xedraw.inc` | small |
| **Render thread**: `gpu_d3d12_present` posts a job; `gpu_d3d12_present_impl` (replay+submit+present) runs on a detached host thread, one frame in flight. Slots build / present / render (`gpu_frame_take`, `g_gpu_frame_mtx`). GEOM batches under `g_geom_present_mtx`. | `gpu_d3d12.cpp` (end of file), `lsw_gpu_frame.h`, `gpu_xedraw.inc` | GIL work 9.7 → 5.3 ms |

Invariants to keep:
- **Only the render thread touches D3D12 and all `xd_*` / `g_xd_*` state** after `gpu_d3d12_init`. The
  render thread is not in the guest scheduler (`g_sched_id == -1`): it must never run guest code.
- Record-time residency assumes **every recorded upload reaches the GPU in record order**. Replay issues a
  draw's uploads before any `continue`/skip, plus the frame's `prelude` first. Keep both if you restructure.
- Translated shader blobs (`r.vs_dxbc` etc.) are stable pointers owned by the bridge, written only by the
  recording side.

## 5. Next work, in priority order

Switch scale: an A57 core is roughly 7× slower than this laptop at the 1.785 GHz Mariko overclock and ~12×
slower at stock 1.02 GHz (public single-core benchmarks, not measured on hardware). A game gets **3 cores**
(core 3 is OS-reserved; the A53 cluster is disabled). Projection with the render thread: max(GIL ~50 ms,
render ~46 ms) ≈ 20 fps overclocked, ~12 fps stock. Both sides must shrink.

### 5.1 UPDATE (same day, session 2): bind cache done, replay 5.6 → 2.9 ms (uncommitted)
`LSWTCS_BINDCACHE` (default on; `=0` = old path for A/B in the same binary), all in `gpu_xedraw.inc`:
per-replay memo of `xd_texture_source` (invalidated by resolve seq / `g_xd_src_epoch` when a target is
dropped or replaced); SRV tables deduped by view-spec content per replay (`g_xd_srv_tables`); sampler
tables keyed by hash + stored descs (no `std::string`); "same table as the previous draw" fast path per
stage (`XdTexLast`/`XdSmpLast`, also skips the prepass); redundant RS/PSO/viewport/scissor/topology sets
skipped (trackers reset on any blit). Back-to-back A/B, hub: bind 2.4 → 0.6, prepass 0.47 → 0.18,
replay 5.5 → 2.9, render-thread present 6.0 → 3.4 ms. Screens identical; no SMPCACHE overflow/STALE.
New GPUPROF sub-timers: `texsrc` (uncached lookups only), `srv`, `smp` (slow path only), `rt`, `draw`.
**What's left on the render thread:** `draw=1.3 ms` = raw D3D12 command recording (5 root CBVs + table +
IB + draw ≈ 1.6 µs/draw) — backend-specific (NVN/Vulkan on Switch). The frame arena is **~3.1 MB/frame**
(every draw copies its full fetch/bool/float constant blocks, float blocks padded to 8 KB): record-time
dedup against the previous draw's blocks would cut guest memcpy, `arena->upload`, and let replay skip
unchanged root CBVs. That is the next cheap win and also helps §5.2.
**Play test (scripted, `build/playtest.sh <label>`, local/untracked):** title + "Press START", hub,
d-pad movement, pause menu open (grayscale world, 48 draws) / navigate / Resume, 90 s idle: all screens
correct. 160 hub samples: wall avg 16.86 ms (locked 60), max 21.96 (pause/resume transition), replay avg
3.18 ms, guest wait for render thread 0.01 ms, no OVERFLOW/STALE/PSO-fail lines. Not covered: entering a
level (press_now has no analog stick). Committed as `8c092ce`.

**Constant-block dedup (`LSWTCS_CBDEDUP`, default on; `=0` for A/B):** `LswGpuFrame::put_block` reuses the
previous draw's block of the same kind (sys / VS float / PS float / bool / fetch) when byte-identical
(state reset in `clear()`); replay skips root CBVs whose offset didn't change and descriptor tables
already bound at the same root slot (`st_cbv`/`st_tbl`, reset on root-signature change or any blit).
A/B: arena 3.1 → 2.0 MB/frame, `draw` 1.35 → 0.9 ms; play test with the table-slot skip: replay avg
3.18 → 2.42 ms, render thread (present) 4.87 → 3.00 ms, wall 16.81 locked, title/hub/pause/movement
correct. Guest-side `record` change within noise. **What keeps the arena at 2 MB:** VS float blocks
that differ per object (world matrices) are full 256-register snapshots padded to 8 KB (zero tail for
dynamic indexing). Removing that needs sized CBVs (descriptor-table CBVs return 0 out of bounds) or
per-register dirty tracking — a root-signature change, not done.

### 5.1 Render-thread CPU cost (was ~5 ms here, ~35–60 ms on Switch).
Replay sub-stages last measured: bind ~3 ms, arena→upload ~0.3, prepass ~0.4, pso ~0.1, and the remainder
(RT binding, root CBVs, draw calls) ~1.5 ms. Ideas, measure each:
- **bind**: per draw per texture it creates an SRV (`CreateShaderResourceView`), runs `xd_texture_source` and
  `lsw_gpu_texture_info`; samplers build a `std::string` key and call `lsw_gpu_sampler` per draw. Cache SRV
  descriptors per (resource, format, mapping, dims) in a CPU heap and `CopyDescriptorsSimple`, or cache whole
  descriptor tables by content like `g_xd_smp_cache` already does for samplers. Add sub-timers first.
- Skip redundant state: root signature / PSO / viewport / scissor / topology are set every draw.
- The FSIG fingerprint loop (`[FSIG]`, always on) walks every draw and builds strings each frame. Cheap here
  (diag=0.04 ms) but drop or gate it for Switch.
- Longer term the D3D12 backend becomes NVN or Vulkan on Switch. Keep the replay's per-draw logic
  backend-agnostic where you touch it.

### 5.2 Guest-side GIL work (5.3 ms here)
walk ~0.6 + record ~1.4 (prepare ~0.5, vertex page hashing ~0.5) + guest code ~3. The big lever is removing
the GIL so the game's ~9 threads use all 3 cores (memory `switch_multithreading_constraint.md`). It's a large
project: guest locks must really serialize host threads. Smaller wins first: `lsw_gpu_prepare` still
`memset`s a ~3 KB `LswGpuDraw` and recomputes system constants per draw.

### 5.3 Known risk to watch
Replay reads **texture data from guest memory concurrently** with running guest threads (record only
snapshots vertex/index/constant data). A texture written mid-replay may show stale for a frame, or a
half-written texture could be cached by `lsw_gpu_texture_key`. If the user reports flicker or half-loaded
textures, snapshot texture bytes at record time (or hash/dirty-track texture pages like vertex pages).

### 5.4 Other open threads (not perf, for context)
- Audio: music stalls after ~4 s (memory `audio_pipeline.md`).
- `fbfill` in VdSwap still CPU-fills the guest frontbuffer with the clear colour every frame (~0.1 ms); likely
  obsolete with GPU render targets, but verify before removing.

## 6. Pitfalls hit this session

- **Uncapped + anything that removes the per-frame pause starves boot** (GIL never released → loaders never
  run → black 25-draw boot screen forever). Happened with the old GIL-held fence wait and again with
  `NOREPLAY` at `FPS_CAP=0`.
- The first hub frames after loading are black (fade-in). Wait before judging a screenshot.
- `press_now` timing: pressing before the title is up is ignored, and extra presses in the hub open the pause
  menu or attack. `hubprof.sh` handles this; copy its logic for new scripts.
- The `[82288 loc420] … skipping crash-prone cleanup` line is an old recomp guard (`ppc_recomp.432.cpp`), not a
  new problem.
- Backups from this session: `backups/20261006_105657_pre_frameperf` (+ `pre_frameperf_xdxbc`),
  `backups/20261006_115708_pre_renderthread` (+ `pre_renderthread_xdxbc`).
