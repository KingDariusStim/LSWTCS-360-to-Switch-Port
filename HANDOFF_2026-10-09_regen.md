# HANDOFF 2026-10-09: EXPERIMENTAL_REGEN — game code regenerated with registers as locals

> **PROMOTED 2026-10-09 (user request):** this tree is now the MAIN tree. Every EXPERIMENTAL_REGEN file was moved
> to the same path under LSWTCSPortMod/; main's replaced files (and its whole switch/build, LSWTCSRuntime/build,
> Convert 360/LSWTCS/output) are in `archive_main_2026-10-09/` (moves only, nothing deleted). Paths below that say
> EXPERIMENTAL_REGEN/... now live at the same relative path in LSWTCSPortMod/. The one-way rule (§0) is retired.
> switch/build was re-configured fresh (stale regen CMake state in archive_main_2026-10-09/stale_regen_cmake/).
> LSWTCSRuntime/build now holds only scripts: a PC build needs a fresh configure.

## 0. Rules for this folder (user, standing; read first)

- **One-way folder.** Copying from the parent `LSWTCSPortMod` *into* `EXPERIMENTAL_REGEN` is fine.
  **Nothing ever goes from `EXPERIMENTAL_REGEN` to the parent** (no sources, patches, toml, tools, builds)
  unless the user explicitly authorizes that specific transfer. Report results; the user decides on merging.
  (Memory: `feedback_experimental_regen_isolation.md`.)
- The experimental `.nro` going to the SD card (`E:/switch/lswtcs.nro`) replaces the main build the user tests
  on hardware: **ask the user first.** Never delete anything on the SD card (`E:`); replacing is fine, verify
  copies with `cmp`.
- Eden testing of the experimental NRO is allowed at will (user, 2026-10-09); SD copy still needs confirmation.
- Ask before closing an Eden the user has open. One Eden instance at a time.
- Also read the parent's `HANDOFF_2026-10-08_switch.md` §4–§6 (hardware/Eden workflow, standing rules).

## 1. Goal

Every recompiled guest function currently keeps the PowerPC registers in the shared `PPCContext` struct
(`ctx.rN`), so the compiler must store/reload them around each guest memory access. XenonRecomp's
`*_as_local` options turn them into C++ locals (as Unleashed Recompiled does). Expected: all guest code gets
faster at once (main game thread ~20 ms/frame on Switch hardware, audio thread, everything). Not yet
measured.

## 2. What is done

| Step | Result |
|---|---|
| Self-contained copy of XenonRecomp, LSWTCSRuntime (no build dir; its `*.sh`/`*.py` copied), switch (no build/scratch; `mesa/build/install` + mesa `*.py` copied), xenia-canary, simde, SPIRV-Cross, patches, `Convert 360/LSWTCS/{lswtcs.toml, switches.toml, Default.xex, output_ref}` | done |
| Baseline regen with the original toml (`lswtcs_baseline.toml`) | **identical to `output_ref`** (3166 files): pipeline reproduces the original |
| Locals regen: `lswtcs_locals.toml` = original + `ctr_as_local xer_as_local reserved_as_local cr_as_local non_argument_as_local non_volatile_as_local = true` | `output_locals_raw/` |
| Address fold (`switch/tools/fold_recomp_output.py`) on a copy | `output_locals_raw_folded/` (unpatched reference) |
| Current hand edits as a fresh patch: `diff -ruN output_ref_folded output_current_edited` | `current_edits.patch` (1003 hunks / 156 files; `output_current_edited` = copy of the parent's live `output`) |
| Ported to locals: `tools/port_edits.py` (renames `ctx.r0/r2/r11/r12/r14-31, f0/f14-31, v14-127, cr*, xer, ctr, reserved` → locals in every hunk line; drops `__restgprlr/__restfpr/__restvmx` calls from added early-return guards — locals mode saves nothing in prologues) | `ported_edits.patch`; `patch -F3`: 1 hunk rejected, 95 fuzzed (`patch_apply.log`) |
| Placement check `tools/check_edit_anchors.py` (anchor = function + preceding asm comment text + occurrence #; asm comments are identical in both modes) | first 86 → 62 mismatches |
| `tools/fix_entry_edits.py`: 58 function-entry edits that patch put into the *previous* function moved to their own function's entry | done |
| By hand: `lswtcs_vsflush` hook in `ppc_recomp.342.cpp` (`sub_822B6610`, the rejected hunk); two `RETURN` diag lines (`sub_82323960` in .360, `sub_82506A38` in .418) moved to just before the final `return` | done |
| **Final check: 1862 / 1862 edit blocks at the same place** | `output/` = ported + folded output the build uses |
| Hazard review: host code touching registers that became locals | only `r2` (TOC passed to new threads in kernel_stubs.cpp/main.cpp); guest code uses r2 as a scratch register (loads before use, `std/ld r2,8(r1)` around calls), so a local is safe. 3 added hunk lines pass `ctx` to helpers (`lswtcs_heaptest`, `KeWait*`): argument registers only, safe |
| rpmalloc thread-id fix (copied from parent, see §4) | in `switch/third_party/rpmalloc/` |

## 3. Current state: Switch build in progress

- Started with `switch/build.sh > switch/build_regen.log 2>&1` (from `EXPERIMENTAL_REGEN/switch`), GCC `-j4`.
  At handoff: GameCode unity units ~30/175 of the remaining steps, **0 errors**. If the session that started it
  is gone, just rerun the same command: ninja resumes.
- **Memory:** 4 GCC jobs leave ~2.6 GB free of 24 GB. Do not link or build anything else (e.g. the parent's
  build) at the same time; pause by killing `ninja.exe` + `cc1plus.exe`, rebuild later resumes.
- Output: `EXPERIMENTAL_REGEN/switch/build/lswtcs.nro` (+ `lswtcs.elf` for addr2line).
- Expect compile errors to be possible in hand-edit code (e.g. an edit that declared/used a ctx register in a
  way the rename missed). Fix them **in EXPERIMENTAL_REGEN only**; record each fix here.

## 4. Next steps

1. Finish the build. On errors: fix in `Convert 360/LSWTCS/output/` (and, if systematic, in `tools/port_edits.py`
   so a re-port reproduces it).
2. **Eden test.** `EXPERIMENTAL_REGEN/switch/tools/eden_hub.sh` expects Eden at `$ROOT/Eden`, which is not in this
   folder. Use the parent's Eden program directly, without copying anything back:
   `../switch/tools/eden_hub.sh 90 "$(pwd)/switch/build/lswtcs.nro"` (the 2nd argument is the NRO). Eden's SD is
   shared: `%APPDATA%/eden/sdmc/lswtcs/` (env.txt must keep only its normal lines; remove any test line you add).
   Check: title → hub reached, `crash.log` absent, `stdout.log` has no `[CRASH]`. Move the character with
   `echo "n:40 e:40 s:40 w:40" > "$APPDATA/eden/sdmc/lswtcs/press_now"`, screenshot with `touch .../shot_now`
   (PNG lands as `shot_<n>.png`; a capture stalls ~6–7 s in Eden, ignore that `[GLSTALL]`).
3. **Compare with the main build** (same scene, back to back): `[SCHEDPROF]` thread shares, `[FTSTAT]` swaps/s,
   `[GPUPROF]` per-frame `walk/record` in `trace.log`. The main build is the parent's `switch/build/lswtcs.nro`
   (run it from the parent's path with `../switch/tools/eden_hub.sh 90`).
4. If it runs: report numbers to the user, ask whether to put it on the SD card for a hardware test.
5. Optional: PC build of this folder for the sampling profiler (`LSWTCS_SAMPLEPROF=1`, `LSWTCSRuntime/build/
   sampleprof.py`). Not set up here; configure fresh (the parent's PC build dir is 19 GB; build with native
   mingw64 `cmake.exe`). Only if the user wants laptop A/B numbers.

## 5. Context: what else happened 2026-10-08/09 (main build, parent folder)

- Vertex glitch (stretched default-texture shards while running) was `LSWTCS_SHMONCE`; fixed with a
  stable-page rule (pages unchanged 8 frames skip re-hash). PM4 walker: one host pointer per IB.
- **Hardware crash at hub load** = rpmalloc's `get_thread_id` read `tpidr_el0`, which libnx never sets → all
  heaps looked owned by every thread → unsynchronised cross-thread frees. Fixed with `LSW_RPMALLOC_TID_TPIDRRO`
  (`tpidrro_el0`). Verified in Eden; the fixed main NRO is on the SD card, **user is testing it on hardware**.
- Hog list after this (main build): guest XAudio2 reverb `sub_827E2208` (39% of the audio thread), GPU-command
  thread memcpy/hash, render-thread state changes. Details: memory `native_hogs_2026_10_08.md`.
- Ghidra (LSW360) cannot decompile this code (32-bit PPC language lacks ld/std and VMX128); read the
  recompiled C++ instead.

## 6. Files here

- `Convert 360/LSWTCS/`: `output/` (build input), `output_locals_raw[_folded]/`, `output_ref[_folded]/`,
  `output_current_edited/`, `current_edits.patch`, `ported_edits.patch`, `patch_apply.log`, `anchors*.txt`,
  `lswtcs_{baseline,locals}.toml`, `regen_*.log`.
- `tools/`: `port_edits.py`, `fix_entry_edits.py`, `check_edit_anchors.py` (usage in each docstring).
- Re-run the whole port from scratch: regen (`XenonRecomp.exe lswtcs_locals.toml
  ../../XenonRecomp/XenonUtils/ppc_context.h`), copy + fold, `patch -p1 -F3` with `ported_edits.patch` inside the
  copy, `fix_entry_edits.py anchors.txt ...`, the 3 hand fixes above, then `check_edit_anchors.py` must print
  0 mismatches.

## 7. Build fixes (2026-10-09 s2)

- First full build: all 175 GameCode units compiled with **0 errors**; only host code failed: 7 uses of
  `ctx.r2` (no `r2` member when `PPC_CONFIG_NON_ARGUMENT_AS_LOCAL`): `LSWTCSRuntime/source/kernel_stubs.cpp`
  (thread start `tctx.r2`, `gt->toc = ctx.r2`, interrupt `icb.r2`, XAudio worker `t.r2` + its spawn) and
  `main.cpp` (`ctx.r2 = 0`). Each wrapped in `#ifndef PPC_CONFIG_NON_ARGUMENT_AS_LOCAL` (TOC = 0 / unused in
  locals mode; every generated function declares its own `PPCRegister r2{}`). Same source still builds in
  ctx mode. Log of the failed run: `switch/build_regen_1.log`.

## 8. First Eden run: stuck before the title screen — live-in registers (2026-10-09 s2)

- Symptom: boots, main loop runs at 60 fps on the loading screen (`sub_8220D680`), title never appears.
  `titles_us_360.gsc` gets queued, then the job worker (sid 10, `0x82508C48`) blocks forever:
  `[CS] contended CS 0x84000610 ... held by sid=0 lr=0x822A2F3C depth=55` (the heap lock; main thread
  entered it 55 more times than it left). Leaking Enter sites: `0x822A1680` (sub_822A15D0) and `0x822A2928`.
- Root cause (class, not one edit): functions that read register locals before writing them, i.e. expect
  the caller's registers. In ctx mode they come through PPCContext; in locals mode they are zero.
  - MSVC SEH `__finally` funclets: `sub_822A1E6C` is called with r12 = parent frame, r22 = "lock held",
    r27 = heap; it saw r22 = 0 and skipped `RtlLeaveCriticalSection`. ~75 such funclets.
  - Split function fragments: `sub_822A2E30` tail-branches into `sub_822A34F0` with r19-r31 live.
  - Guest CRT setjmp `sub_829E9B20` (saves every non-volatile register).
  - `lwz r16,-11648(r19)` pairs at ~97 function starts are data decoded as code (`0x8213D280`); in
    locals mode they read near 0xFFFFD280 (Eden ignores it; hardware may fault). Mostly never called
    (callers use the real entry 8 bytes later).
- Fix: `tools/pass_livein_regs.py` (run on `output/` after the hand-edit port; backup in
  `output_pre_livein/`, report `livein_report.txt`). CFG liveness per function + interprocedural fixpoint
  over direct calls; live-in locals are loaded from `inline thread_local LswLiveRegs g_lsw_live`
  (ppc_context.h) at entry, and each direct call site stores the caller's copies just before the call.
  521 functions, 478 call sites. Not covered: indirect calls into such functions (SEH dispatcher), and
  callees that set non-volatile registers for their caller (live-outs); none seen so far.
- Diagnostics added in `LSWTCSRuntime/source/kernel_stubs.cpp` (experimental only): each CS records owner
  sid / outer lr / depth; `[CS] contended` line names the holder; `[CSDEPTH]` logs depth growth past 2.

## 9. Eden A/B after the live-in pass (2026-10-09 s2, 01:40)

Both: `../switch/tools/eden_hub.sh 60 [nro]`, Cantina hub, standing still, same env.txt, back to back.
Logs: session scratchpad `ab_main/`, `ab_exp/`.

| | main (ctx) | experimental (locals) |
|---|---|---|
| title / hub reached | 20 s / 54 s | 21 s / 53 s |
| hub fps ([FTSTAT], steady) | ~46.8 (233-238 swaps/5 s) | ~55.8 (268-290 swaps/5 s) |
| frame wall ([GPUPROF]) | 21.4-21.7 ms | 17.6-19.0 ms |
| main guest thread (sid 0) | 61.4% of wall ≈ 13.1 ms/frame | 56.2% ≈ 10.0 ms/frame |
| audio thread (827DB318) | 15.0% of wall | 14.1% of wall |
| GPU-command thread (822D2AB0, host-heavy) | 19.9% ≈ 4.3 ms/frame | 25.8% ≈ 4.6 ms/frame |
| render replay ([GPUPROF] replay) | 8.1-8.4 ms | 10.7-11.1 ms (host GL code, unchanged; more frames/s competing for Eden's host cores) |

No `[CS] contended`, no crash. Hardware not tested. Risk before hardware: the ~97 `lwz r16,-11648(r19)`
data-as-code entries (§8) read an unmapped address if one is ever called with r19 = 0.

## 10. On the SD card (2026-10-09, user-authorized)

- `E:/switch/lswtcs.nro` = `EXPERIMENTAL_REGEN/switch/build/lswtcs.nro` (109037765 bytes, cmp OK). It replaced the
  main build (120150213 bytes, = parent `switch/build/lswtcs.nro`; restore from there to go back).
- Previous hardware logs (main build, Switch clock wrong, order only): last run reached the hub and entered
  Episode I Negotiations, ~17 fps in level, no crash. An older `crash.log` = rpmalloc
  `_rpmalloc_span_double_link_list_pop_head` (far=0x48) from `host_audio_submit`'s per-frame `std::vector<float>`
  on the audio thread; symbols match the current main elf exactly, and that build has the tpidrro_el0 fix
  (201 `mrs tpidrro_el0`), so rpmalloc may still be unsafe somewhere. Same code is in the experimental build.
- Hardware run still exposed to the §8 r19 data-as-code entries.

## 11. Hardware feedback + fixes (2026-10-09 s2; made in the PARENT, copied parent -> experimental)

User on hardware: "ran beautifully", quick start, few initial shader lags. Fixed/added (both folders, both build):
- Save times: `switch/source/win32_switch.cpp` GetTimeZoneInformation now returns the console's offset
  (libnx timeToCalendarTimeWithMyRule; Bias = current offset incl. DST, no transition dates). Was zeros = UTC.
  Note the console's own clock reads 2025-12-31 (Album + file times), which no port change can fix.
- A/V hold (LSWTCS_AVHOLD, default 1): `audio_switch.cpp host_audio_hold`, called around the waiting part of
  `gl_backend.cpp wait_frame_programs`: no new device buffers while the picture is held for program builds,
  so the game's XAudio (and cutscene streams) pause with it. [AUDIO] line reports hold count/ms.
- Program list default ON (LSWTCS_GLPROGLIST, user decision; was off by the "not cached" rule): saved GLSL in
  sdmc:/lswtcs/glprogs queued at boot. Eden: run 1 = 11 holds / 1192 ms; run 2 = 61 boot-queued, 1 hold / 37 ms.
- Open: cutscene horizontal band (Album capture 2025123120073500, Negotiations hangar MTT): rows ~325-395 brighter,
  below partly see-through. Theory: overlapping tile rows get blended effects twice. Not investigated yet.
- Open: rpmalloc crash in host_audio_submit's per-frame std::vector (crash.log), see §10.

## 12. Follow-ups (2026-10-09, after promotion)

- **Data-as-code entries fixed:** `tools/fix_data_prefix_loads.py` (run on `Convert 360/LSWTCS/output` after the live-in
  pass) neutralises the 93 `lwz r16,-11648(r19)` / `lwz r16,N(r16)` pairs (pointer-table words before real entries; r16
  dead in all 93). No `r19 - 11648` load remains. Marker: `LSW_DATA_PREFIX`.
- **Allocator crash:** the crash.log on the SD card (rpmalloc span list, audio thread) is the crash the tpidrro_el0
  fix was made for (same-size instruction swap: identical symbol offsets; console clock puts it before the clean
  hardware runs). Hardening: `switch/source/audio_switch.cpp` recycles played frame vectors (`g_spare`) instead of
  allocating 6 KB per 5.33 ms frame.
- Build 10:55 (108980421 bytes): Eden title 19 s / hub 51 s, ~56 fps, no crash, HUD text renders.
- **Promotion side effect, fixed:** Eden's sdmc/lswtcs/SHADERS is a junction to LSWTCSRuntime/build/SHADERS; archiving
  that dir broke UI text in Eden. SHADERS + run-dir data moved back (memory `eden_sd_junctions`).
- **Cutscene band:** hub uses 3-tile resolves (rows 0-320, then 32..320 and 32..128 in the next tiles); the band's top
  edge (~row 325) sits at the first seam. Needs a live capture at an affected cutscene (`xdlog_now`, `resdump_now`,
  `shot_now` in Eden's sdmc/lswtcs) to compare tile rects/blend state with the hub.
- **Band FIXED (2026-10-09):** user observation on hardware: shadows applied above the first tile seam only. Cause in
  `switch/source/gl_rt.inc texture_source()`: the stitched texture blitted the base resolve first, then every sub-range
  resolve, regardless of age. The shadow composite (#2480) is resolved full-screen into 0x0A909000 (#2505), but the
  older per-tile colour resolves of tiles 2/3 (0x0A963000 / 0x0A9BD000, unshadowed) were pasted over rows 320..672
  when later passes sampled 0x0A909000. Now all sources are blitted in resolve-sequence order (newest last). Eden hub:
  shadows under characters below the seam now present (missing before). The PC D3D12 path
  (LSWTCSRuntime/source/gpu_xedraw.inc xd_resolved_compose) has the same base-then-subs order: same fix pending there.
- Diagnostics added: capture combo ZL+ZR+R-stick (LSWTCS_CAPCOMBO), [GLDRAWCAP] logs scissor + resolve dest offset,
  resdump also writes the stitched composites (cmp_*.png), depth on a fixed scale (LSWTCS_RESDUMPZ, default .95..1)
  and prints raw depth of the centre column at rows 308..332.
