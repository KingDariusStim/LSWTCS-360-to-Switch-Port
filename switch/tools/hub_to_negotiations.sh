#!/bin/bash
# Walk from the Cantina hub start position into Episode I and select Chapter 1 (Negotiations), Story.
# Usage: hub_to_negotiations.sh <dir with press_now trigger>   (PC: LSWTCSRuntime/build; Eden: %APPDATA%/eden/sdmc/lswtcs)
# Found by screenshots on the PC build 2026-10-11 (Qui-Gon P1, save 2.6 %); NPCs can bump the player, check a shot.
D=$1
step() { echo "$1" > "$D/press_now"; sleep "$2"; }
step "ne:20 n:90" 5
step "e:12 n:150" 6
step "n:120" 5
step "w:28 n:120" 6
step "n:150" 7
step "e:40 ne:50 n:60" 7
step "w:30 n:90" 6
step "a:20" 2
