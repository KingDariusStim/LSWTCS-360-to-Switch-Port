#!/bin/bash
# Eden hub A/B: switch/tools/eden_ab.sh <label> [ENV=VAL ...]
# Appends the given lines to Eden's sdmc:/lswtcs/env.txt for one eden_hub.sh run (45 s in the hub), restores the
# file afterwards (byte copy), and prints: hub frame times (frames.log, hub = draws > 700, first 200 frames skipped),
# audio underruns ("idle waits") per ~10 s window, and the last [SCHEDPROF] switch count. Logs kept in
# switch/hwlogs/eden_ab/<label>/.
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SD="$APPDATA/eden/sdmc/lswtcs"
LABEL=$1; shift
OUT="$ROOT/switch/hwlogs/eden_ab/$LABEL"; mkdir -p "$OUT"
cp "$SD/env.txt" "$OUT/env_orig.txt"
for kv in "$@"; do printf '%s\n' "$kv" >> "$SD/env.txt"; done
"$ROOT/switch/tools/eden_hub.sh" 45 $NRO > "$OUT/eden_hub.out" 2>&1   # NRO=<path> runs another build
cp "$OUT/env_orig.txt" "$SD/env.txt"
cp "$SD/frames.log" "$SD/stdout.log" "$SD/trace.log" "$OUT/" 2>/dev/null
grep "eden_hub" "$OUT/eden_hub.out" | tr '\n' ' '; echo
awk 'NR>1 && $1 ~ /^[0-9]+$/ && $9>700 {n++; if(n<200) next; m++; v[m]=$5} END{asort(v); printf "[%s] hub frames=%d p50=%.1f p90=%.1f p99=%.1f\n", "'"$LABEL"'", m, v[int(m*.5)], v[int(m*.9)], v[int(m*.99)]}' "$OUT/frames.log"
printf "[%s] underruns/10s: " "$LABEL"; grep -a "\[AUDIO\] [0-9]* buffers" "$OUT/stdout.log" | awk '{print $5}' | awk 'NR>1{printf "%d ", $1-p} {p=$1} END{print ""}'
grep -a "SCHEDPROF\] t=" "$OUT/trace.log" | tail -1 | cut -c1-120
grep -a "CRASH" "$OUT/stdout.log"
exit 0
