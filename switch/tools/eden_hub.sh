#!/bin/bash
# Boot the NRO in Eden and drive it to the hub with press_now (START, then A on New Game).
# The null graphics backend reports draws=0, so progress is read from the level files the game
# opens (sdmc:/lswtcs/trace.log): "levels\titles\..." = title screen, "levels\Map\Map\Map.anm" = hub
# loaded (the same 14 levels\Map files a PC hub run opens).
# Usage: switch/tools/eden_hub.sh [seconds to keep running after the hub=20] [nro]
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
AFTER=${1:-20}
NRO=${2:-$ROOT/switch/build/lswtcs.nro}
SD="$APPDATA/eden/sdmc/lswtcs"
rm -f "$SD/stdout.log" "$SD/stderr.log" "$SD/trace.log" "$SD/press_now" "$SD/crash.log"
"$ROOT/Eden/eden-cli.exe" "$(cygpath -w "$NRO")" >/dev/null 2>&1 &
PID=$!; T0=$SECONDS
wait_log() {   # <pattern> <timeout s>
    local t=0
    while [ $t -lt $2 ]; do
        grep -aq "$1" "$SD/trace.log" 2>/dev/null && return 0
        kill -0 $PID 2>/dev/null || return 2
        sleep 1; t=$((t + 1))
    done
    return 1
}
RC=1
if wait_log 'str="levels.titles.titles' 240; then
    echo "[eden_hub] title level loading at $((SECONDS - T0)) s"
    sleep 15; echo start > "$SD/press_now"; sleep 5; echo a > "$SD/press_now"
    if wait_log 'str="levels.Map.Map.Map.anm' 120; then
        echo "[eden_hub] HUB loaded at $((SECONDS - T0)) s"; RC=0; sleep "$AFTER"
    else echo "[eden_hub] hub not reached"; fi
else echo "[eden_hub] title not reached"; fi
kill -0 $PID 2>/dev/null && taskkill //F //IM eden-cli.exe >/dev/null 2>&1
grep -a "\[PRESS\]\|\[XInput\]" "$SD/trace.log" | head -10
grep -a "FTSTAT" "$SD/trace.log" | tail -1
[ -s "$SD/crash.log" ] && { echo "== crash.log"; cat "$SD/crash.log"; }
exit $RC
