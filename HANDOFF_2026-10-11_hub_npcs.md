# HANDOFF 2026-10-11: why do NPCs not appear in the main hub (Mos Eisley Cantina)?

Audience: the next agent. Task: find out why the Cantina hub has no wandering NPC characters in the port (PC build,
Switch build in Eden and on the console), fix it, and verify against the original game in Xenia. Read the standing
rules in `HANDOFF_2026-10-08_switch.md` §4–§6 (ask before every SD-card copy, never delete on the SD card, ask
before closing an Eden the user has open, one Eden at a time). The user tests on hardware and in Eden; they will
play to a spot and tell you when to capture.

State at hand-off: commit `eddbd09` on `main` (pushed). The console has that build
(`sdmc:/lswtcs/lswtcs.nro`, md5 fd3fffb8dcaf6fcea1fdbe310d56d524).

## 0. Confirm the bug before hunting it (do this first)

In LEGO Star Wars: The Complete Saga the Cantina is populated by the characters the player has **unlocked**
(as far as known). The user's save (`saves/00000001/savegame_21_4c`, 2.6 % progress, also installed in Xenia and
Eden, backup in `switch/hwlogs/saves_2026-10-09`) has few unlocks, so the hub may be nearly empty in the original
game too. Before anything else:

1. Xenia (`xenia/xenia_canary.exe "Convert 360/LSWTCS/Default.xex"`, keyboard mapped like Eden: arrows move,
   A/S/Z/X face buttons, M = Start; config backups `xenia-canary.config.toml.before_*`) with the same save: stand
   in the Cantina, count the NPCs, screenshot.
2. Same save in Eden (`Eden/eden-cli.exe switch/build/lswtcs.nro`) or the PC build: same spot, same count.
3. If both are empty, ask the user which save / progress shows NPCs on their console's original game (or start a
   Free Play-heavy save) before going further.

**Caution:** the PC blue-screened twice on 2026-10-10 (bug check 0x3B in `dxgkrnl.sys`, DWM present-history,
`STATUS_SEMAPHORE_LIMIT_EXCEEDED`) while Xenia ran (once during a Xenia memory dump). Windows is 24H2
26100.4652; a manual cumulative update from the Update Catalog was suggested, the user prefers not to update.
Keep Xenia sessions short; tell the user before memory dumps.

## 1. What is known that is likely relevant

- **The last big bug class was the recompiler.** XenonRecomp emitted `// ERROR` + `return;` for switch cases and
  branches outside a function as it sized it (53 functions). `sub_821CAE98` (object state query of the level
  trigger network) returned a pointer = "true" for every state, which fired level events by themselves
  (Episode 2 disco ball, Kamino camera). All 1,686 sites are repaired (tools/fix_switch_error_returns.py,
  tools/fix_switch_midjumps.py, memory `switch_error_returns.md`). **Check whether the NPC problem existed before
  and after that fix**: every repair can be switched off at run time, per function address range:
  `LSWTCS_FIXOFF=<lo>-<hi>[,...]` in `env.txt` (list of repaired functions:
  `switch/hwlogs/2026-10-10_bluetint/gated_funcs.txt`). `LSWTCS_FIXOFF=821CAE98-82A9FC84` = all off.
  If NPCs appear with all repairs off (unlikely but cheap to test), bisect like the blue tint
  (`switch/tools/tint_round.sh` is the template: Eden hub run + screenshot + measure).
- Other XenonRecomp gaps still open: `vcmpbfp` (18 sites) is `__builtin_debugtrap()` (would crash, not silently
  misbehave); `blrl` ×1 stubbed (memory `compiler_handoff.md`). `sub_822DE7F8` was hand-translated 2026-10-06 for
  the same ERROR class.
- **Object system map** (2026-10-10): level objects belong to 28 classes; the class table is at `[0x8309BA70]`
  (count at +0, records of 0xB8 bytes at `[+8]`): +0x48 state query, +0x54 activate, +0x5C. Function addresses
  per class are in `LSWTCSRuntime/source/main.cpp` (`kObjQuery` / `kObjAct`). The main.cpp hooks
  (`LSWTCS_OBJLOG`, default on) log `[OBJQ]` (query: object handle, name, wanted state, answer, first time and on
  change) and `[OBJACT]` (activations) in stdout.log. An object handle's first word points at its record; prop
  records carry their name inline (e.g. `DiscoBall_spin`).
- **Trigger network**: nodes of 0x20 bytes, walked every frame by `sub_824686C0` → `sub_82467318` (recursive);
  node types 0/1/2 with handlers at `0x82EF5048` (action / logic gate / output flag). Decompiled C in
  `ghidra_out/decomp_*.c`; regenerate with `ghidra_scripts/LSW360Decomp.java <tag> <hex addrs...>` (headless
  command in memory `ghidra_360_oracle.md`; the script fixes the save/restore no-return truncation itself).
- **Image strings** that may lead to the NPC code: `AIEDITOR_CREATURES`, `AI_rancor`, `AlertCreatures`; the hub
  object-name string table around `0xE2FF8C00` in a Xenia heap snapshot (`fountain_BASE_*`, `episodeTwoDisco`,
  `light_19a`, `shop_*`). Search the image with `strings -n 5 "Convert 360/LSWTCS/image.bin"`.

## 2. Tools that worked last session

| Need | Tool |
|---|---|
| Original-game ground truth (memory) | `tools/xenia_snapshot.ps1 -Out <prefix>`: read-only, heap 0xE0000000–0xFFFFFFFF + globals 0x82E70000–0x83460000 (~1.5 s). `tools/xenia_dumpbin.ps1` for one range. Xenia host = guest + 0x110000000. |
| Port memory | PC build under cdb: `.writemem <file> poi(LSWTCSRuntime+<g_base offset>)+0x80000000 L?0x20000000` (attach `-pd`, then `qd`). Heap layout differs from Xenia; match objects by content / names, not addresses. |
| Who writes a value | `tools/cdb_watch_changes_example.txt` (hardware write breakpoint, logs only value changes with the host call stack; never detach from inside the handler). Resolve host frames to `sub_X` with the build map (`.text$__imp__sub_X` ranges) or `addr2line`. |
| Which code reads strings / globals | Ghidra: `ghidra_scripts/LSW360Cam.java` (references to address ranges and strings), `LSW360Decomp.java`. String tables are reached through pointer tables (e.g. script command table name/handler/id triples at `0x82E7B248`); search raw big-endian pointers in `image.bin` when Ghidra shows no references. |
| Draw-level differences | Shader sets: Xenia `dump_shaders` (config, with `store_shaders = false`) vs `[SHADERNEW]` lines in the port's stdout.log (same XXH3 hash = Xenia's file names). That is how the disco ball effect was identified (2 shaders the port drew without the player's action). |
| Per-frame state | Switch build `frames.log` (game frame time, replay timings, camera eye / direction from the view matrix); auto-capture on a sudden camera turn. |
| Eden runs | `switch/tools/eden_hub.sh <secs>` (boots to the hub unattended); Eden keys: hold Q+W then G = capture combo. |

## 3. Suggested plan

1. §0: confirm the difference with the same save in Xenia vs port (screenshots + count).
2. If confirmed: find the spawning code. Candidates: the creature/AI system (`AIEDITOR_CREATURES`), a hub
   population routine that reads unlocked characters from the save (the in-memory save is identical between
   Xenia and the port: block at `0x82FA05C0`, 32,360 bytes, only a play-time counter differs), or a trigger
   network condition (log `[OBJQ]` answers in the hub and compare with expectations).
3. Diff Xenia vs port for the NPC objects: if NPC records exist in Xenia's heap (character names in a snapshot;
   look for the unlocked characters' names), search the port's memory dump for the same names; missing records =
   spawn never ran → breakpoint / log the spawner; present but inactive = compare their state words.
4. Fix, then check the Episode 2 disco ball and the Kamino level start still behave (regression), and that
   colours are right (blue-tint check: `tools/png_tint.py` on a hub screenshot, B−R ≈ −25 is right).

## 4. Housekeeping notes

- Generated code edits live in `Convert 360/LSWTCS/output` (not tracked); the tracked record is
  `patches/recomp_output_edits.patch` = `diff -ruN --strip-trailing-cr output_locals_raw_folded output` (regenerate
  before each commit). A recompiler regen needs the passes re-run in order (HANDOFF_2026-10-09_regen.md §6, then
  `tools/fix_switch_error_returns.py`, `tools/fix_switch_midjumps.py`, `tools/fix_switch_gate.py`, and the two hand
  patches in ppc_recomp.154.cpp at 0x821CAF08 / 0x821CAF5C).
- Before committing: `git grep --cached -i julian` must be empty (no local paths in tracked files).
- The PC build (`LSWTCSRuntime/build/LSWTCSRuntime.exe`) is the 10-08 archive build: it lacks the switch repairs
  and the resolve-swap fix. Rebuild it (native mingw64 cmake) before using it to judge game logic.
