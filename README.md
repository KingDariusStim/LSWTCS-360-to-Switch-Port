# LSWTCS-360-to-Switch-Port
A decompiled executable of the LSWTCS iso ported by claude to run on a Mariko Switch Environment

## What's in this repo

Only original port code is tracked. Game data (`Default.xex`, `game*.DAT`, `image.bin`, `pristine.bin`),
the XenonRecomp-generated `ppc_recomp.*.cpp` files, Xenia memory dumps and build output are **not**
included — you need your own copy of the game to rebuild.

| Path | Contents |
|---|---|
| `LSWTCSRuntime/` | Host runtime: kernel/XAM stubs, D3D12 GPU backend, Xenos→DXBC bridge, XMA/XAudio2 audio, CMake build |
| `Convert 360/LSWTCS/*.toml` | XenonRecomp configs |
| `patches/recomp_output_edits.patch` | Hand edits to the recompiled output (`output_ref` → `output`). Apply after running XenonRecomp. |
| `patches/XenonRecomp.patch`, `patches/xenia-canary.patch` | Local changes to those upstream repos; `*.base` holds the commit each patch applies to |
| `ghidra_scripts/` | Ghidra scripts used as the 360 disassembly oracle |
| `HANDOFF_*.md` | Session handoff / progress notes |
| `*.sh`, `*.ps1` | Run, backup, Xenia dump and capture helpers |

### Rebuilding the recomp output

1. Extract `Default.xex` from your disc and run XenonRecomp with `Convert 360/LSWTCS/lswtcs.toml` into `Convert 360/LSWTCS/output`.
2. From `Convert 360/LSWTCS`: `patch -p1 < ../../patches/recomp_output_edits.patch`
3. Build `LSWTCSRuntime` with CMake (paths in `CMakeLists.txt` are currently absolute and need adjusting).
