# HANDOFF 2026-10-06 (single-view memory): finish the Switch memory model on PC

**Your task:** make the game run cleanly in **single-view mode** (`LSWTCS_SINGLEVIEW=1`), then decide
with the user whether to commit the address fold. Everything below is **uncommitted** working-tree state
on top of `4670bb5`. Read §1–§3 before touching anything.

Earlier context: `HANDOFF_2026-10-06_perf.md` (rules, build, measurement; §5.5 = last session's
saves/texanim/allocator work), `Eden/EDEN_MEMORY.md` (Switch emulator + probe results).

---

## 1. Why this exists

The 360's 512 MB of physical memory is visible at three guest ranges: `0x80000000`, `0xA0000000` and
`0xC0000000` (same bytes). On PC the runtime maps one section three times (`main.cpp`,
`MapViewOfFileEx`). **Switch homebrew cannot do that.** Eden probe #1 (`switch/probes/memalias`,
results in `Eden/EDEN_MEMORY.md`) tried shared memory (not available), code memory (second view is
read-only), transfer memory and `svcMapMemory` (source loses access).

So the Switch runtime must keep **one** copy and **fold** guest addresses `0xA0000000–0xDFFFFFFF` onto
`0x80000000 + (addr & 0x1FFFFFFF)` in software. This work does that on PC first, where we can test.

## 2. What is done (uncommitted)

| Piece | Where | State |
|---|---|---|
| `ppc_fold()` + `PPC_HOST(x)`; all `PPC_LOAD/STORE` macros go through `PPC_HOST` | `Convert 360/LSWTCS/output/ppc_context.h` | done |
| `PPC_HOST` = **table form**: `ppc_view_base[addr >> 29] + addr` (`LSW_ADDR_FOLD_CMOV=1` = compare/cmov form) | same | done |
| Table definition + fill right after guest memory is mapped (`[5]`/`[6]` point back at the `0x80` copy) | `LSWTCSRuntime/source/main.cpp` | done |
| 30,109 raw `base + EXPR` accesses in generated code (vector `lvx/stvx`, `lwarx/stwcx`, `dcbz`) rewritten to `PPC_HOST(EXPR)` | `Convert 360/LSWTCS/output/*.cpp` (226 files) via `switch/tools/fold_recomp_output.py` | done |
| `lsw_host(a)` helper (`g_base + ppc_fold(a)`) for host code; 22 runtime sites converted (file read/write buffers, ring/IB reads, physical allocator memset, PCR memsets, audio submit, save/enumerate buffers, misc guest-struct memsets) | `LSWTCSRuntime/source/kernel_stubs.cpp` | done |
| Guest pool memset folded | `main.cpp` (`memset(g_base + ppc_fold(addr) ...)`) | done |
| GPU ring-buffer helpers folded | `LSWTCSRuntime/source/gpu_ringbuf.cpp` | done |
| XMA shim `TranslateVirtual/TranslatePhysical` use `ppc_view_base` | `LSWTCSRuntime/xma/shim/xenia/memory.h` | done |
| `LSWTCS_SINGLEVIEW=1`: only `[0, 0xA0000000)` and `[0xE0000000, end)` committed; `0xA0000000–0xDFFFFFFF` reserved **no-access** so any unfolded access faults | `main.cpp` | done |

**Verified:** with the fold build in the normal 3-view mapping, the full play test passes (title, hub,
pause menu open/navigate/resume, movement, 60 s idle), rendering identical, no errors.

**Measured cost** (back-to-back hub A/B on AC power, `SCHEDPROF` work under the lock per 16.7 ms frame):

| Build | Idle | Main thread | Work under lock |
|---|---|---|---|
| No fold (`build/LSWTCSRuntime_nofold.exe`) | 76.8 / 76.9 % | 13.8 % | ~3.86 ms |
| Fold, compare + cmov | 72.2 % | 17.4 % | ~4.64 ms (+20 %) |
| **Fold, table (current)** | 74.1 / 73.8 % | 15.6–15.9 % | **~4.36 ms (+13 %)** |

The extra cost is all in the main thread (generated game code).

## 3a. UPDATE 2026-10-07: single-view is CLEAN through the play test

- `strnlen` fault = hand-added `dbg_ram("%s", (char*)(base+ctx.r30.u32))` in `sub_82503A10`
  (ppc_recomp.417.cpp). 12 such `(base+EXPR)` debug sites (no space, so the rewrite script missed them)
  in 8 generated files → `PPC_HOST(EXPR)` (scratchpad script sv_sweep2; regex `\(base\+([^()]+)\)`).
- Runtime sweep (backup `backups/20261007_112447_sv_sweep`): main.cpp semfix/stubsrc/log helpers +
  HEAPTEST/watch readers; kernel_stubs `lswtcs_d070_advance` (only takes 0xA..0xF nodes → would always
  fault), IM_LOAD_IMM shader ptrs via `lsw_host`, watch thread; lsw_xma.cc init memset.
- `sv_catch.sh` fixed: dropped `-g` (with it, cdb only read the `sxe` command at the fault itself, so no
  stack was printed); marker match is now `^===AV===` (cdb echoes the command line containing it).
- Result: `sv_catch.sh 30` NO FAULT (title → hub → idle); `EXTRA="LSWTCS_SINGLEVIEW=1" ./playtest.sh sv`
  passes all 8 stages, screenshots correct. Not yet covered: a real level (needs the user), long sessions.
- Still uncommitted; the user decides on committing (§3 step 4).

## 3. Where it stood before 2026-10-07 (historical)

Single-view runs fault where runtime code still builds raw guest pointers. Fixed so far: `ExCreateThread`
PCR `memset` (3 sites), XMA translation. **Current fault (open):**

- **`strnlen` on guest `0xA11154C0`** during boot (loading screen, ~24 draws). Some host code measures a
  guest string through a raw pointer (likely `std::string(const char*)` / `strlen` / `printf("%s")` on
  `g_base + x` or `base + x`). The caller is not identified yet.

Next steps, in order:
1. Run `LSWTCSRuntime/build/sv_catch.sh` (see §4). Its cdb command now uses `sxe -c "..." av`, so it
   prints the stack at the fault. If the stack walk is broken by frame-pointer-less code, use
   `dq @rsp L20` and map return addresses with `llvm-addr2line -e LSWTCSRuntime.exe 0x<0x140000000+off>`.
2. Fix with `lsw_host(addr)` (host code in `kernel_stubs.cpp`) or `g_base + ppc_fold(addr)` elsewhere.
   **Grep, don't just whack-a-mole:** search `source/*.cpp`, `source/*.inc`, `xma/`, `xenos_dxbc/` for
   `g_base +`, `base +`, `(const char*)` built from guest addresses. Sites using `+ 0x80000000 + phys` or
   fixed `0x82…` image addresses are already fine.
3. Repeat until `sv_catch.sh` prints **NO FAULT** through boot, title, New Game, hub and 60 s idle. Then
   run `playtest.sh` with `EXTRA="LSWTCS_SINGLEVIEW=1"` and a level if the user can play one.
4. Ask the user before committing. If committed: regenerate `patches/recomp_output_edits.patch` (it will
   grow by the ~30k rewritten lines; consider instead teaching the XenonRecomp generator to emit
   `PPC_HOST(...)` directly, see §6), keep `switch/tools/fold_recomp_output.py` with it.

## 4. How to build, run, measure

- **Build:** `export PATH=/c/msys64/mingw64/bin:$PATH; cd LSWTCSRuntime/build && ninja.exe LSWTCSRuntime`.
  A change to `ppc_context.h` rebuilds all generated code: **~5 minutes** (163 unity units).
- **Single-view fault hunt:** `LSWTCSRuntime/build/sv_catch.sh [idle seconds]`: runs the game under cdb
  with `LSWTCS_SINGLEVIEW=1`, drives title → Start → A → hub, prints the faulting guest address and a
  symbolized call chain, or `NO FAULT`. The game window **freezes at the fault** (debugger holds it);
  the script kills it afterwards. Tell the user before running: they have mistaken it for a crash.
- **A/B:** `EXE=LSWTCSRuntime_nofold.exe ./hubprof.sh <label>` vs `./hubprof.sh <label>`, alternating,
  back to back. Read `SCHEDPROF` idle: work under lock = 16.7 ms × (1 − idle). `boot_to_hub.sh`,
  `hubprof.sh` and `playtest.sh` kill both exe names.
- **Play test:** `./playtest.sh <label>` (title, hub, pause, movement, idle; screenshots `pt_<label>_*.png`).

## 5. Rules (from the user, mandatory; full list in HANDOFF_2026-10-06_perf.md §2)

- One game instance at a time; say so before closing a game the user might be using.
- Never `LSWTCS_AUTOSTART=1`; use `press_now` (stick tokens `n s e w ne nw se sw`, `name:N` = hold N polls).
- Back up before risky changes (`./backup.sh <label>`; copy `LSWTCSRuntime/xenos_dxbc/` by hand).
- Patch C/C++ with Python scripts written via the Write tool, or the Edit tool. No heredoc/`sed` on C code.
- Commit/push only when asked; privacy check before any push (no personal names/paths/emails).
- Eden: keys and game dumps are the user's own; never read, copy or commit them (see `Eden/EDEN_MEMORY.md`).

## 6. Notes and ideas

- **Cheaper fold:** most accesses are stack (`r1`-relative) and image/data (`0x82…`). If thread stacks
  were allocated in the `0x80` view (they currently come from `g_phys_alloc`, which also returns `0xA…`
  addresses), the generator/rewrite script could emit an unfolded access for pure `ctx.r1.u32 + K`
  addresses and recover much of the +13 %. Needs a stack-allocation change first; measure.
- **Generator instead of rewrite:** XenonRecomp (`patches/XenonRecomp.patch`) could emit `PPC_HOST()`
  for vector/atomic/dcbz accesses, so the output needs no post-processing. Re-running the recompiler
  means re-applying `patches/recomp_output_edits.patch` (hand edits such as `sub_822DE7F8`).
- The table is a global, so byte stores through `uint8_t*` may force reloads (aliasing). On Switch,
  consider passing the table in a register (e.g. via `base`) if profiles show it.
- `LSWTCS_PHYSALIAS=0` (old flat fallback) is unrelated to this and still exists.

## 7. Pitfalls

- **First launch after a relink often produces an empty `run_out.txt`** (likely Windows Defender scanning
  the new ~460 MB exe past the scripts' 10 s grace). Retry once; scripts treat it as "TITLE NOT REACHED".
- The laptop throttles hard on battery (numbers ~30 % worse); measure on AC.
- `backups/20261006_225406_pre_fold_output` = generated output before the rewrite;
  `backups/20261006_225410_pre_fold` = runtime before; `build/LSWTCSRuntime_nofold.exe` = baseline exe.
  To undo the fold entirely: restore `output/` from that backup and `git checkout` the four runtime files.
