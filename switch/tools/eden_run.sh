#!/bin/bash
# Run an NRO in Eden for N seconds, then close Eden and show the runtime's logs from the emulated SD.
# Usage: switch/tools/eden_run.sh [seconds=180] [nro=switch/build/lswtcs.nro]
# Eden prints nothing itself; the runtime writes sdmc:/lswtcs/{stdout,stderr,trace}.log.
# Prepare the SD once with switch/tools/link_sd.ps1.
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SECS=${1:-180}
NRO=${2:-$ROOT/switch/build/lswtcs.nro}
SD="$APPDATA/eden/sdmc/lswtcs"
rm -f "$SD/stdout.log" "$SD/stderr.log" "$SD/trace.log"
"$ROOT/Eden/eden-cli.exe" "$(cygpath -w "$NRO")" >/dev/null 2>&1 &
PID=$!
for ((t = 0; t < SECS; t += 5)); do
    sleep 5
    kill -0 $PID 2>/dev/null || { echo "[eden_run] Eden exited after ~${t}s"; break; }
done
kill -0 $PID 2>/dev/null && { taskkill //F //IM eden-cli.exe >/dev/null 2>&1; echo "[eden_run] stopped Eden after ${SECS}s"; }
for f in stdout.log stderr.log; do
    [ -s "$SD/$f" ] && { echo "== $f ($(wc -l < "$SD/$f") lines, tail)"; tail -40 "$SD/$f"; }
done
[ -f "$SD/trace.log" ] && echo "== trace.log: $(wc -c < "$SD/trace.log") bytes"
