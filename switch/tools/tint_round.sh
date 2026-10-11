#!/bin/bash
# One bisection round for the 2026-10-10 blue tint: switch the ERROR-return repairs of functions [A, B) (line numbers
# in gated_funcs.txt, 0-based) off via LSWTCS_FIXOFF, boot Eden to the hub, screenshot, print B-R (tools/png_tint.py).
# Usage: switch/tools/tint_round.sh <A> <B>     (A=B: nothing off)
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
D=$ROOT/switch/hwlogs/2026-10-10_bluetint
SD="$APPDATA/eden/sdmc/lswtcs"
A=$1; B=$2
OFF=$(sed -n "$((A+1)),${B}p" $D/gated_funcs.txt | tr '\n' ',' | sed 's/,$//')
# FIXOFF takes up to 64 ranges: one range covering the slice (the list is sorted; only gated functions matter)
LO=$(sed -n "$((A+1))p" $D/gated_funcs.txt); HI=$(sed -n "${B}p" $D/gated_funcs.txt)
cp "$SD/env.txt" "$D/env_orig.txt"
[ "$A" != "$B" ] && echo "LSWTCS_FIXOFF=$LO-$HI" >> "$SD/env.txt"
rm -f "$SD"/shot_*.png
( bash "$ROOT/switch/tools/eden_hub.sh" 40 > "$D/hub_$A-$B.txt" 2>&1 ) &
t=0; while [ $t -lt 400 ] && ! grep -q "HUB loaded\|not reached" "$D/hub_$A-$B.txt"; do sleep 2; t=$((t+2)); done
sleep 20; touch "$SD/shot_now"; wait
cp "$D/env_orig.txt" "$SD/env.txt"
S=$(ls -t "$SD"/shot_*.png 2>/dev/null | head -1)
[ -n "$S" ] && cp "$S" "$D/round_$A-$B.png"
echo "round $A-$B (off $LO..$HI): $(grep -o 'HUB loaded.*\|not reached' $D/hub_$A-$B.txt) $(python -X utf8 $ROOT/tools/png_tint.py $D/round_$A-$B.png 2>/dev/null | grep -o 'B-R.*')"
