# HANDOFF 2026-10-11 (s5): potential accelerations, ranked

Audience: the next agent working on Switch performance. Nothing in this file is implemented yet; it is a ranked
list with the evidence behind each item. **User rules (standing):** never ship an optimisation that makes gameplay
worse (a measured loss stays opt-in or is removed); the PC build is the performance baseline, not Xenia; no cache
may stand in for a slow pipeline; ask before every SD-card copy, never delete on the SD card; one Eden at a time;
never set `LSWTCS_AUTOSTART`; don't launch or kill the PC game while the user may be playing it (ask first; the
test scripts `taskkill` it). Patch C/C++ with the Edit tool or Python scripts, not sed on code.

Read first: `HANDOFF_2026-10-11_fps_loading.md` (§2 progress block: today's native CRT, LZ2K decoder and vector
locals, with numbers and the build step for `tools/pass_vec_locals.py`).

## 0. Where things stand (console, build 969470b, on the SD card)

| Metric (console, user's run 2026-10-11 s4) | Earlier today (`charglitch` run) | Now |
|---|---|---|
| Level frame, p50 / p90 | 37.6 / 47.1 ms (~27 fps) | 29.6 / 36.8 ms (~34 fps) |
| Hub frame, p50 | – | 21.7 ms (~46 fps) |
| LZ2K decode | ~55 MB/s | ~75 MB/s (478 MB in 6.4 s) |
| Audio underruns | – | 978 idle waits in 273 s |

Logs: `switch/hwlogs/2026-10-11_console_s4/` (gitignored). Eden hub (eden_ab.sh): p50 ~18-19 ms, p90 ~21-23 ms.
Different play sessions: treat console deltas as indicative. Commits today: 46ad329 (CRT + LZ2K), b0dc960 (vector
locals), 969470b (pause-menu stale frame fix).

## 1. Argument / stack registers as locals (biggest lever, game-wide guest CPU)

**STATUS (2026-10-11 s6): APPLIED, uncommitted; Eden OK, console test pending, PC not run (user chose Eden).**
`tools/pass_arg_locals.py` promotes r1, r3-r10, f1-f13: 2.09 M of 2.10 M references in 37,993 functions,
584 k dirty-only write-backs (forward dataflow over lines + goto edges; sync points nested in `{}` don't clear the
dirty set; everything is reloaded after a call, incl. r1, dead reloads are dropped by the compiler). Skips: gain 130,
braceless 52, stmt 18 (multi-line statement at a call), loop 4, brace 3, unsafe 2. Report
`Convert 360/LSWTCS/arglocal_report.txt`; pre-pass copy `output_pre_arglocal/`. **Build step now:** apply
`patches/recomp_output_edits.patch`, then `python tools/pass_vec_locals.py "Convert 360/LSWTCS/output" --min-gain 1`,
then `python tools/pass_arg_locals.py "Convert 360/LSWTCS/output"`. First attempt broke on `ctx` inside a dbg_ram
string literal (fixed: sites are detected on string-stripped code). PC build: 0 errors. NRO 110.1 -> 108.6 MB.
Eden hub A/B, alternating (p50/p90/p99 ms, main-thread busy): arg 18.5/21.4/25.8 63.9 %, 18.5/21.4/27.3 64.0 %;
base (969470b NRO, `switch/build/lswtcs_base969470b.nro`) 18.8/21.6/26.4 66.3 %, 18.9/21.7/28.1 67.3 %: ~2 %
frame, ~3 points of main-thread time, consistent; underruns unchanged; no crash. Eden JIT-translates ARM code, so
(as with the vector pass) hardware may show more. Not yet done: lr kept in ctx (155 k stores, as planned).
Console run 1 (logs `switch/hwlogs/2026-10-11_console_s6_arglocal/`, same env.txt as s4): INCONCLUSIVE / possibly
worse. Same spot (Negotiations start, ~2,910 draws, 10 s): ft p50 32.7 (s4) -> 36.8 ms, but render replay (host
code the pass does not touch) also 18.8 -> 22.3 ms, so run conditions (dock/handheld = GPU clock, unknown) likely
differ. Main-thread busy per frame by draw band also higher (9.4 -> 10.9 ms at 1000-1700 draws), which fence
spin-waits would inflate if the GPU side is slower. Pending: controlled back-to-back console A/B (old
`lswtcs_prev_20261011_173503.nro` = 969470b vs new), same mode, ~30 s at the Negotiations start. If the new build
loses there, keep the pass out of the default build (user rule).
**Controlled console A/B DONE (same session, same mode, standing at one Negotiations spot, ~1,870 draws; logs
`switch/hwlogs/2026-10-11_console_ab_{A_old,B_new}/`): KEEP.** Frame time identical (p50 38.0 / p90 43.3 ms both;
replay 22.9 vs 23.0 ms): this spot is GPU / render-replay bound, so a guest-CPU gain cannot show in fps there. Main
guest thread busy (SCHEDPROF, steady state) 37.3 % -> 34.8 % of wall time = ~14.2 -> ~13.2 ms per frame (-7 %);
GPU-command thread unchanged (26.6 / 26.5 %); idle 30.0 -> 32.7 %. No crash in either run. Run 1's "regression"
was the different spot / conditions. SD card now: `lswtcs/lswtcs.nro` = new (arg locals), old parked as
`lswtcs_A_old_969470b.nro` (`mtp.ps1 ren` added for A/B swaps; the forwarder only launches `lswtcs.nro`).
Takeaway for what's next: heavy level spots are GPU-side bound (~23 ms replay + GPU), so §3 / §6 GPU items now pay
more than further guest-CPU passes at those spots; CPU passes still help CPU-bound scenes (hub, Eden).

Remaining `ctx.` register references in the generated code (`Convert 360/LSWTCS/output`, comments excluded):
r3-r10 ~1.62 M (r10 441 k, r3 284 k, r9 242 k, r8 166 k, r4 156 k, r5 114 k, r7 108 k, r6 100 k), r1 323 k,
lr 155 k, f1-f13 ~170 k (f13 56 k, f12 27 k), fpscr 40 k. That is ~14x what the vector pass covered (133 k). Each is
a load/store through `ctx`, and every guest memory store (plain host store through `PPC_HOST`) may alias `ctx` as far
as the compiler knows, so values are reloaded after each one. The GPR locals regen (2026-10-09, non-volatile and
r0/r2/r11/r12 only) gave +19 % Eden fps.

How: generalise `tools/pass_vec_locals.py` (same scheme, proven safe): per function, copy the used registers from
ctx to locals at entry; write them back before every line that hands ctx to other code (direct calls,
`PPC_CALL_INDIRECT_FUNC`, host helpers) and before returns / the closing brace; reload after calls. Keeps ctx-mode
semantics exactly (no ABI change, no liveness needed for correctness). Differences from the vector case:
- Many more call sites per function, and 9+ registers: spilling/reloading every used register at every call can
  cost more than it saves. Reduce it: write back only registers assigned since the last sync point (forward
  "dirty" scan over the lines between sync points, conservative at labels: treat a `loc_` label as "all dirty"),
  and reload after a call only registers the callee may change (r3-r12, f0-f13, v0-v19, lr, cr, ctr, xer are
  volatile; r1 never changes across a call: no reload needed). Keep the per-function gain test (`--min-gain`).
- `ctx.lr = 0x...;` before each `bl` (155 k stores): lr is only read by host diagnostics
  (`ppc_indirect_skip_log`, kernel_stubs.cpp has 38 `ctx.lr` / `.r1.u32` uses, e.g. caller logging and stack
  arguments of imports with >8 args). Keep it in ctx, or make it a local spilled only at calls into host code.
- Host code reads `ctx.r1` for stack arguments and the GIL scheduler switches threads inside host calls: both are
  sync points already (ctx passed), so they see written-back values.
- Function fragments and SEH funclets that read registers set by their caller: in ctx-mode semantics those come
  through ctx at entry; the pass's entry copy keeps that working (same reason the vector pass was safe).
Verify: PC build + boot/hub/level play (the user plays the PC build; ask before launching), then Eden A/B
(`switch/tools/eden_ab.sh`, `NRO=<path>` for the other build), then the console. Record numbers in the handoff.
The recomp patch stays the pre-pass output; document the pass as a build step (as for pass_vec_locals.py).

## 2. Stop treating normal guest memory as `volatile` (experiment, possibly large)

`PPC_LOAD_*` / `PPC_STORE_*` (ppc_context.h) dereference `volatile` pointers: 751 k memory ops in the generated
code, none of which the compiler may merge, reorder or keep in registers (a field loaded twice in a function is
loaded twice). Under the cooperative GIL other guest threads only run at kernel calls / quantum yields (function
calls), so within a call-free stretch plain loads are safe, except guest spin-waits on a memory flag with no call
in the loop body (the compiler could hoist the load and hang). Experiment: a non-volatile variant for loads only,
keep stores volatile, with the PC build first; look for hangs at boot, loading screens and the job system
(LSWTCS_SPINGUARD covers some spins). Also keep `lwarx/stwcx` and anything after `eieio` (MMIO) volatile. Must be
re-checked if real multithreading replaces the GIL (memory `switch_multithreading_constraint.md`).

## 3. GPU-command thread host work (~22 % of samples in a PC level; on the console it competes with core 0)

From `switch/hwlogs/2026-10-11_pcprof/sp_level.report.txt` (thread id 6, 0x822D2AB0): msvcrt memcpy/memcmp of
constant blocks 19.6 %, `pm4_extract_ib` 20.6 %, XXH3 vertex hashing 9.2 %, shader lookup 5.6 %, record/prepare
~10 %, ntdll 17 %.
- Constant blocks: the fetch block is new on 66 % of draws, the VS float block on 60 %; viewport and system
  constants are recomputed per draw. Memoise viewport / system constants on a generation counter of the context
  registers that feed them; compare-then-copy only the dirty register ranges (the PM4 walker knows which registers
  each packet wrote). `LSWTCS_PREPPROF=1` gives per-stage numbers ([PREPPROF], [RECPROF]).
- `pm4_extract_ib`: profile its inner loop (the type-0 register-block copy got a fast path on 2026-10-11); the main
  thread also walks the ring inside fence waits (1.5 % of main).
- Shader lookup (`get_shader`): hash-map lookup per draw; cache the last (vs, ps) pair per draw stream.
- Do NOT repeat: walking large IBs off core 0 (`LSWTCS_WALKOFF=1`, measured slower), masking the fetch block.
- Vertex hashing (XXH3, ~1 ms/frame Eden): **user said skip** (2026-10-11): smaller win than the above.

## 4. Scheduler handoffs (~9 % of main on PC; ~11-18 k switches per 100-280 s run)

ntdll / winpthread time on the main thread is the cooperative scheduler. Earlier attempts at fewer handoffs
(`LSWTCS_QDIV`) starved audio; since then the IRQL depth fix (`LSWTCS_IRQLFIX`) and urgent rejoin changed the
picture. Re-measure QDIV values on Eden with eden_ab.sh, watching underruns per 10 s (the console still shows 978
idle waits in 273 s, so audio is the constraint to watch, and fixing underruns is worth doing on its own).

## 5. LZ2K decoder, next step (loading and streaming spikes)

Now ~75 MB/s on the console (1.6x the faithful port on PC, 2.3-2.7x in Eden). Remaining cost: per-symbol Huffman
decode (~5 ns/symbol on PC; 42.5 M literals + 9.1 M matches, avg match 13.4, in the 156 MB benchmark set) and block
headers (~18 %, 5.5 us each, 10,984 headers). Options: two-literal table entries (decode two literals per lookup
when both codes fit in 12 bits); speed up `read_c_len` / `make_table`. Benchmark offline: dump with
`LSWTCS_LZ2KDUMP=<MB>` (lz2k_dump.bin in the run dir), build a bench that includes the decoder (the s4 bench is in
the s4 session scratchpad, not in the repo; recreate it from native_lz2k.cpp: old/new namespaces, compare outputs
and state). Verify with `LSWTCS_LZ2KCHECK=1` (0 mismatches over boot + hub + level).

## 6. Smaller / speculative

- Hot VMX functions `sub_8231ED30` (3.4 % of main in a level), `sub_8231D470` (1.8 %): now on locals; if still hot
  after §1, a hand-written NEON version is possible but must match the guest's float behaviour bit for bit (check
  mode as for native_crt).
- `ctx.fpscr` (40 k refs, mostly `disableFlushMode()` / `enableFlushMode()` around FPU / VMX code): each toggles
  the host FPCR. Check how often the mode actually changes at runtime; skip the write when it is unchanged.
- GPU side: heavy frames wait ~25 ms on the GPU on the console (glthread handoff §3b); per-draw state changes in the
  GL replay are the lever there (not covered by today's work).

## 7. Tools

PC: `LSWTCSRuntime/build/boot_to_hub.sh` (source; `boot_to_hub <label>`), `switch/tools/hub_to_negotiations.sh .`
(to Episode I Ch 1), `LSWTCS_SAMPLEPROF=1|2` + `sampleprof.py`. Eden: `switch/tools/eden_ab.sh <label> [ENV=VAL]`
(`NRO=<path>`). Console logs: `switch/tools/mtp.ps1 pull lswtcs <dir> stdout.log trace.log frames.log`; the
in-game capture is ZL + ZR + right-stick click (shot_<frame>.png + res_*.png render targets in sdmc:/lswtcs; the
frame number is in stdout.log `[RESDUMP] done at replay N`). `crash.log` is not cleared at start: check stdout.log
for `[CRASH]` before trusting it.

## 8. Commit checklist

Regenerate `patches/recomp_output_edits.patch` from the PRE-pass output if hand edits changed
(`diff -ruN --strip-trailing-cr output_locals_raw_folded output_pre_veclocal` in `Convert 360/LSWTCS`, or keep a
fresh pre-pass copy if another pass is added); `git grep -i` for the user's name and local paths must find nothing;
commit and push when the user asks.
