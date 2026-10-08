#!/bin/bash
# Build the Switch NRO with devkitA64 + libnx.   Usage: switch/build.sh [ninja args...]
# Output: switch/build/lswtcs.nro. Runs in devkitPro's MSYS2 shell (its cmake is the only one the
# devkitPro toolchain file accepts). That shell mounts C:\Users at /home, so the directory is
# passed as a Windows path and converted there; see CMakeLists.txt for the unity-file workaround.
# Parallelism defaults to LSW_JOBS=4: each game-code unity unit needs ~2-2.5 GB in GCC; 24 jobs
# exhausted a 24 GB machine and 6 left only ~2 GB free next to the editor and browser.
WIN=$(cd "$(dirname "$0")" && pwd -W)
JOBS=${LSW_JOBS:-4}
exec /c/devkitPro/msys2/usr/bin/bash.exe -lc "cd \"\$(cygpath -u '$WIN')\" && \
  { [ -f build/build.ninja ] || \$DEVKITPRO/portlibs/switch/bin/aarch64-none-elf-cmake -G Ninja \
      -S . -B build -DCMAKE_BUILD_TYPE=Release; } && ninja -C build -j$JOBS $*"
