#!/bin/bash
# Shader-pipeline benchmark on switch-mesa in Eden (gl_backend.cpp gl_bench, LSWTCS_GLBENCH=1):
# compile + link + first draw of every GLSL file of each variant, Mesa disk cache off.
# Usage: switch/tools/glbench.sh <name>=<dir with *.vert.glsl / *.frag.glsl> ... [+ENV=VALUE ...] [timeout s=900]
# (+ENV=VALUE lines go into env.txt for this run, e.g. +NV50_PROG_USE_NIR=1)
# e.g.  switch/tools/glbench.sh base=switch/scratch/spv_base new=switch/scratch/spv_e2
# Variants run in name order inside one Eden session; full per-shader lines stay in stdout.log.
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SD="$APPDATA/eden/sdmc/lswtcs"
NRO=$ROOT/switch/build/lswtcs.nro
TIMEOUT=900
EXTRA_ENV=()
rm -rf "$SD/bench"; mkdir -p "$SD/bench"
for a in "$@"; do
    case $a in
        --) ;;
        +*) EXTRA_ENV+=("${a#+}") ;;
        *=*) name=${a%%=*}; dir=${a#*=}; mkdir -p "$SD/bench/$name"
             cp "$dir"/*.vert.glsl "$dir"/*.frag.glsl "$SD/bench/$name/" 2>/dev/null
             echo "[glbench] $name: $(ls "$SD/bench/$name" | wc -l) shaders from $dir" ;;
        *) TIMEOUT=$a ;;
    esac
done
cp "$SD/env.txt" "$SD/env.txt.glbench_bak"
echo "LSWTCS_GLBENCH=1" >> "$SD/env.txt"
for e in "${EXTRA_ENV[@]}"; do echo "$e" >> "$SD/env.txt"; done
rm -f "$SD/stdout.log" "$SD/stderr.log"
"$ROOT/Eden/eden-cli.exe" "$(cygpath -w "$NRO")" >/dev/null 2>&1 &
PID=$!; T0=$SECONDS; RC=1
while [ $((SECONDS - T0)) -lt "$TIMEOUT" ]; do
    grep -aq "\[GLBENCH\] done\|\[GLBENCH\] no sdmc" "$SD/stdout.log" 2>/dev/null && { RC=0; break; }
    kill -0 $PID 2>/dev/null || { echo "[glbench] Eden exited"; break; }
    sleep 2
done
[ $RC = 0 ] || echo "[glbench] not finished after $((SECONDS - T0)) s"
taskkill //F //IM eden-cli.exe >/dev/null 2>&1
mv -f "$SD/env.txt.glbench_bak" "$SD/env.txt"
grep -a "FAILED" "$SD/stdout.log" | head -10
grep -a "\[GLBENCH\] TOTAL" "$SD/stdout.log"
exit $RC
