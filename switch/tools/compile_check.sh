#!/bin/bash
# M0 compile check: build every recompiled .cpp for aarch64 (devkitA64) and log failures.
# Usage: switch/tools/compile_check.sh [jobs]   (run from the repo root)
# Objects land in switch/scratch/gen/, per-file errors in switch/scratch/gen/*.err.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT="$ROOT/Convert 360/LSWTCS/output"
OBJ="$ROOT/switch/scratch/gen"
JOBS=${1:-$(nproc)}
mkdir -p "$OBJ"
export ROOT OUT OBJ
compile_one() {
  f="$1"; b=$(basename "$f" .cpp)
  [ "$OBJ/$b.o" -nt "$f" ] && return 0
  /c/devkitPro/devkitA64/bin/aarch64-none-elf-g++.exe -std=c++20 -O2 \
    -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE -D__SWITCH__ -w \
    -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections \
    -include "$ROOT/switch/include/lsw_switch_compat.h" \
    -I"$OUT" -I"$ROOT/LSWTCSRuntime/include" -I"$ROOT/simde/simde" -I/c/devkitPro/libnx/include \
    -c "$f" -o "$OBJ/$b.o" 2> "$OBJ/$b.err" && rm -f "$OBJ/$b.err"
}
export -f compile_one
ls "$OUT"/*.cpp | tr '\n' '\0' | xargs -0 -n1 -P"$JOBS" bash -c 'compile_one "$0"'
echo "objects: $(ls "$OBJ"/*.o 2>/dev/null | wc -l)  failures: $(ls "$OBJ"/*.err 2>/dev/null | wc -l)"
