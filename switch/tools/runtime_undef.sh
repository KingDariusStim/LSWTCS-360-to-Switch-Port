#!/bin/bash
# Compile the runtime + Switch platform sources for aarch64 and list symbols that are still
# undefined (excluding recompiled-game symbols and libc/libnx). Run from the repo root.
set -u
GXX=/c/devkitPro/devkitA64/bin/aarch64-none-elf-g++.exe
NM=/c/devkitPro/devkitA64/bin/aarch64-none-elf-nm.exe
O=switch/scratch/rt
mkdir -p "$O"
FLAGS=(-std=gnu++20 -O1 -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE -D__SWITCH__ -w
  -include switch/include/lsw_switch_compat.h -include switch/include/win32/windows.h
  -Iswitch/include/win32 -I"Convert 360/LSWTCS/output" -ILSWTCSRuntime/include -ILSWTCSRuntime/source
  -Isimde/simde -Ixenia-canary/third_party/xxhash -I/c/devkitPro/libnx/include
  '-DLSW_ROOT_DIR="sdmc:/lswtcs"' '-DLSW_DATA_DIR="sdmc:/lswtcs"')
pids=()
for src in LSWTCSRuntime/source/kernel_stubs.cpp LSWTCSRuntime/source/gpu_ringbuf.cpp \
           switch/source/win32_switch.cpp switch/source/platform_switch.cpp switch/source/null_backends.cpp; do
  [ -f "$src" ] || continue
  "$GXX" "${FLAGS[@]}" -c "$src" -o "$O/$(basename "$src" .cpp).o" 2> "$O/$(basename "$src" .cpp).err" &
  pids+=($!)
done
"$GXX" "${FLAGS[@]}" -Dmain=lsw_runtime_main -c LSWTCSRuntime/source/main.cpp -o "$O/main.o" 2> "$O/main.err" &
pids+=($!)
for p in "${pids[@]}"; do wait $p; done
for e in "$O"/*.err; do grep -m5 "error" "$e" | sed "s|^|$(basename "$e"): |"; done
"$NM" -u "$O"/*.o | grep -v ":" | awk '{print $2}' | sort -u > "$O/undef.txt"
"$NM" --defined-only "$O"/*.o | awk 'NF==3{print $3}' | sort -u > "$O/def.txt"
comm -23 "$O/undef.txt" "$O/def.txt" | grep -vE '^(sub_|__imp__|_Z.*sub_|__real_|_ZSt|_ZNSt|_ZNKSt|_ZdlPv|_Znwm|_ZdaPv|_Znam|__cxa|__gxx|_Unwind|mem|str|f[a-z]+$|__aeabi|__stack_chk)' > "$O/missing.txt"
echo "undefined (non-game, non-libc candidates): $(wc -l < "$O/missing.txt")"
cat "$O/missing.txt" | c++filt 2>/dev/null || cat "$O/missing.txt"
