# HANDOFF 2026-10-08: Switch build — hardware performance, audio, open vertex glitch

Read this first. Then `HANDOFF_2026-10-07_switch_perf.md` §0 and §0b (the detailed log of 2026-10-08),
`HANDOFF_2026-10-07_shader_pipeline.md` §0/§5b and `HANDOFF_2026-10-07_switch.md` §0 for background.

The Switch build now runs on real hardware (the user tests on their own console). The work moved from Eden
to hardware measurements; Eden stays the quick test vehicle. Everything below is **uncommitted** on top of
`5a7f02a` (Switch GL backend + shader pipeline + depth fix). Ask the user before committing (see §6).

---

## 1. Open right now (start here)

### 1a. Vertex glitch on the current build — A/B in progress
User report (Switch and Eden): "shader draws initialise in the middle of the screen, then their vertices
are assigned one by one, visibly, to their correct 3D coordinates, and disappear" (the user later called
them "glitchy texture inits"). It appeared with the build that added, after the last build the user said
"runs great": structured guest loops (#4b), shared shader stages (#3), audio output, and (current build
only) once-per-frame vertex page hashing.

- Loops already ruled out: with `LSWTCS_GLFWDLOOPS` off (now the default) the glitch remains.
- **Pending A/B:** `%APPDATA%/eden/sdmc/lswtcs/env.txt` currently ends with the test line
  `LSWTCS_GLSTAGEREUSE=0` (shared shader stages off). The user is to restart Eden and report.
  - Glitch gone → stage reuse is the cause. Prime theory: one compiled VS/PS object attached to several
    programs that are linked concurrently on the render thread + 2 workers; Mesa's linker / ralloc may not
    be safe for that. Options: make reuse default off, or serialise links that share a stage, or reuse only
    within one thread.
  - Glitch stays → next `LSWTCS_SHMONCE=0` (once-per-frame vertex hashing; not in the first glitched build,
    so less likely), then walk back: `LSWTCS_GLWORKERS=0`, `LSWTCS_GLTEXBIAS=0`, `LSWTCS_GLFWDCF=0`,
    `LSWTCS_AUDIO_OUT=0`, `LSWTCS_IBSHM=0`.
  - **Remove the test line from Eden's env.txt afterwards** (rule: keep the original lines only).
- Ask the user where it shows (title / hub / level) and on what (characters / particles / scenery).

### 1b. Audio/video drift (5–10 s in cutscenes)
Not a bug in the audio path: at 8–12 fps on the console the game's frame time is capped, so game time
runs at x0.65–0.89 of real time ([FTSTAT] in trace.log) while audio streams play in real time. Real fix =
frame rate. A shortcut is raising the game's maximum frame time: the minimum is `[0x830AA980]` (patched,
see `[FTFLOOR]` in kernel_stubs.cpp); the maximum is applied inside the main loop `sub_82215AA0`
(ppc_recomp.219.cpp) or its callee `sub_8247e5e8` — not yet located. Use the Ghidra oracle
(memory `ghidra_360_oracle.md`). Larger frame steps can affect gameplay; propose to the user first.

### 1c. Crash under heavy overclock
`sdmc:/lswtcs/stdout.log` [CRASH] data abort pc=+0x52EE0E8 in the audio build during Episode I, with
the user's GPU/CPU overclock. The ELF changed since, so it was not symbolised. Only chase it if it
reproduces at stock clocks. (Symbolise with `aarch64-none-elf-addr2line -f -C -e switch/build/lswtcs.elf`
on the *same* build; one call per address is slow on the 120 MB ELF, batch the addresses.)

---

## 2. Where performance stands (Switch hardware)

Latest hardware numbers (pre-audio build, Episode I level, partly overclocked — clocks unknown):
~19–20 fps, every stage busy:

| Stage | Per frame |
|---|---|
| GPU | 28–33 ms (ordinary draws 17–22, full-screen passes ~6, resolves ~2.5) |
| Render thread own work | 28–30 ms (glDraw* ~12.5, bind 3.1, rt 1.8, state 1.5, sampler 1.4, upload 1.3) |
| Guest (GIL, threads take turns) | ~50 ms: game code ~20, GPU-command thread ~20 (PM4 walk 5 + record 14–15: prepare 5.5, vertex hash+copy 4.8), audio ~10 |

Hub earlier: GPU 48 → ~20.6 ms/frame after 1x MSAA + dead-work skipping; 12 → 27–31 fps.

### Work list the user approved ("work down the list")
1. **Guest-side draw recording** — started: `LSWTCS_SHMONCE` (each vertex/index page hashed at most once
   per frame; hub 2179 → 1033 hashes/frame, 0 mid-frame changes were ever seen). Draw preparation
   (`lsw_gpu_prepare`, switch/source/gl_bridge.cc) was timed in Eden at ~2.1 µs/draw spread evenly
   (shader lookups 0.6, float packing 0.5, mods/GLSL 0.3, viewport 0.3, sysconst 0.3) — no single hotspot;
   only whole-draw memoisation would pay. Not done.
2. **Render-thread draw calls** — instrumented: `[GLPROF] changes per replay` (new). Eden hub, 788 draws:
   UBO range changes 1372, texture 247, **swizzle 180** (GL_TEXTURE_SWIZZLE changes make Mesa rebuild
   sampler views — expensive), program 160, state 136, sampler 102, fbo 37. Ideas: fewer UBO rebinds
   (merge the 5 constant blocks into one range per draw, or dedupe beyond consecutive draws), stop
   per-draw swizzle changes on shared textures (e.g. one texture view per swizzle), sort-free state cuts.
3. **Audio thread cost** — the new output paces the game's mixer by the device clock; re-measure on HW
   (`[SCHEDPROF]` thread `1(827DB318)`).

Other candidates: GPU — remaining expensive shaders are ordinary (2–4 ms over hundreds of draws); guest
loops (4 hub vertex shaders) back on the dispatcher since loops are off; 1x MSAA on (2x path kept).

---

## 3. What changed on 2026-10-08 (all with env kill switches, `env.txt`)

Detailed rationale in the perf handoff §0b and in code comments. Summary:

| Change | Switch | Where |
|---|---|---|
| Game files opened read-only first (Horizon write-lock broke the 2nd open of `D:\SHADERS\*` → no UI text on HW) | — | kernel_stubs.cpp `fileio_open` |
| EGL honours share_context (worker contexts never shared objects) | — | switch/mesa/mesa_egl_share_edits.py (hooked in build_mesa.sh) |
| rpmalloc process allocator (newlib's malloc lock serialised parallel shader builds) | CMake `LSW_RPMALLOC` | switch/third_party/rpmalloc/ |
| Parallel shader builds (render thread + 2 workers) | `LSWTCS_GLWORKERS` (2) | gl_backend.cpp |
| One frame in flight (PC parity; restores the Cantina wipe) | `LSWTCS_RTSYNC` | gl_backend.cpp `gpu_d3d12_present` |
| GPU queue cap (input lag) | `LSWTCS_GLMAXQUEUE` | render loop |
| 1x MSAA for 2x surfaces (4x stays full: the game clears through 4x surfaces) | `LSWTCS_MSAA1X` (on) | gl_rt.inc `msaa_scale` |
| Dead resolves / draws skipped (unused blur chain) | `LSWTCS_GLDEADSKIP` | gl_rt.inc, `plan_dead_work` |
| Trace log flushed incrementally outside its mutex (80–115 ms freezes) | — | kernel_stubs.cpp `dbg_ram_do_init` |
| Implicit LOD + bias instead of textureGrad | `LSWTCS_GLTEXBIAS` | spirv_shader_translator_fetch.cc |
| Forward-jump shaders without the switch dispatcher | `LSWTCS_GLFWDCF` | spirv_shader_translator.cc |
| Structured guest loops / backward-jump do-whiles | `LSWTCS_GLFWDLOOPS` (**off**) | same |
| Shared compiled shader stages | `LSWTCS_GLSTAGEREUSE` (on; **suspect**, §1a) | gl_backend.cpp `program_build` |
| Vertex pages hashed once per frame | `LSWTCS_SHMONCE` (on) | kernel_stubs.cpp `gpu_shm_track` |
| Switch audio output (libnx audout, 5.1 → stereo, device-paced) | `LSWTCS_AUDIO_OUT` | switch/source/audio_switch.cpp |
| Program binary cache opt-in only (user rule: no caching) | `LSWTCS_GLCACHE=1` | gl_backend.cpp |

Diagnostics added: `[GLPROF]` render stages + "changes per replay"; `[GLSTALL]` threshold `LSWTCS_GLSTALLMS`
with per-stage split and malloc-lock wait; `[GLGPU]` GPU timestamps (`LSWTCS_GLGPUPROF=N`; Eden reads 0);
`[GSPIKE]` guest spikes (`LSWTCS_SPIKEMS=N`, needs `LSWTCS_SCHEDPROF=1`); captures `LSWTCS_SHOTAT=a,b`,
`LSWTCS_RESDUMPAT=N`, `LSWTCS_STALLSHOTS=1` + `LSWTCS_STALLSHOTOFFS=a,b` (capture lines carry shader ids,
GLSL goes to `sdmc:/lswtcs/capglsl/`); `[AUDIO]` buffer counts. PC harness
`switch/tools/spirv_glsl_test.sh <out> switch/scratch/ucode_hub2` now also writes `<x>.spv.dis` (ucode
disassembly).

---

## 4. Hardware test workflow

- SD card is **E:/** when in the PC (often it is in the Switch: then E: does not exist). Layout:
  `E:/switch/lswtcs.nro`, `E:/lswtcs/{env.txt, game.DAT, game1.DAT, image.bin, pristine.bin, SMBPATH.TXT,
  perm*.dds, SHADERS/, audio/}`; the game writes `stdout.log`, `trace.log`, `crash.log`, `capglsl/`,
  `shot_*.png`, `res_*.png`, `mesacache/` there.
- **Never delete anything on E:** (user rule, memory `feedback_sd_card_no_delete.md`). Replacing files is
  fine. Verify copies with `cmp`.
- Current E: contents: the audio build NRO (one build behind the local `switch/build/lswtcs.nro`, which
  has loops off + new switches + SHMONCE + state-change counters) and the perf `env.txt` (normal settings +
  `LSWTCS_SPIKEMS=50`, `LSWTCS_GLSTALLMS=50`, `LSWTCS_GLGPUPROF=300`). Copy the new NRO when the user asks.
- The console clock is unset (log times read 12-31 / 01-01).
- The user launches via title override (hold R on a game), not album mode.
- The user overclocks sometimes (sys-clk). Ask which clocks a run used before comparing numbers.

## 5. Eden workflow
- Build: `switch/build.sh` (needs the NRO unlocked: Eden closed). Relink 1–2 min.
- `switch/tools/eden_hub.sh <s>` boots, presses START/A, reaches the hub, kills Eden after `<s>` s.
  Screenshot: touch `%APPDATA%/eden/sdmc/lswtcs/shot_now` while running.
- One instance at a time. **The user often has their own Eden open; ask before closing it.** They may also
  interrupt tool runs that would launch Eden.
- Eden's emulated SD: test env lines go at the end of its env.txt and must be removed afterwards
  (currently `LSWTCS_GLSTAGEREUSE=0` is there for §1a). Clean copy of the normal env:
  `%TEMP%/claude/.../scratchpad/env_clean.txt` may be gone in a new session — the normal lines are the ones
  before the first `LSWTCS_SPIKEMS` / test line; `switch/sd/lswtcs/env.txt` in the repo is the base.
- Eden numbers are relative only; its in-game lag spikes are mostly Eden compiling host pipelines.

## 6. Rules (user, standing)
- Optimise for real Switch hardware, not Eden; pipeline work, not caching.
- Never delete on the SD card; replacing is fine.
- Patch C/C++ with the Edit tool or Python scripts (never sed/heredocs on C). xenia-canary files may be CRLF:
  keep line endings. Python here needs no `-I` flag (it breaks the msys python).
- Commit only when asked; before committing regenerate `patches/xenia-canary.patch` (xenia-canary has new
  translator edits), `patches/recomp_output_edits.patch` (diff against a copy of `output_ref` folded with
  `switch/tools/fold_recomp_output.py`, then normalise the header paths) and run the privacy check (no user
  name / `C:\Users\...` paths). New tracked files: `switch/source/audio_switch.cpp`,
  `switch/third_party/rpmalloc/` (public domain), `switch/mesa/mesa_egl_share_edits.py`, this handoff.
- Never set `LSWTCS_AUTOSTART`.
