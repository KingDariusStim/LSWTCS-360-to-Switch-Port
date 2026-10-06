#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# LSWTCS handoff / run helper.  Consolidates the render-config launch, the cdb
# stack-trace workflow, and screenshot capture into one script so runs are
# reproducible and the long env list never has to be retyped.
#
#   ./run.sh build              # rebuild only (kernel_stubs/main → fast; recomp → relink)
#   ./run.sh run   [SECONDS]    # launch render config, wait, print progress   (default 45s)
#   ./run.sh trace [SECONDS]    # run then dump the key trace.log signals
#   ./run.sh stack              # attach cdb to the live process, symbolize all thread stacks
#   ./run.sh shot               # view the latest in-process PNG capture path
#   ./run.sh kill               # taskkill the runtime
#   ./run.sh sym  0xRVA ...     # symbolize one or more LSWTCSRuntime+0xRVA offsets
#
# Env knobs (override inline): EXTRA="LSWTCS_SHADERID=1 LSWTCS_DRAWLOG=1" ./run.sh run 60
# ─────────────────────────────────────────────────────────────────────────────
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && (pwd -W 2>/dev/null || pwd))"
BUILD="$ROOT/LSWTCSRuntime/build"
EXE="$BUILD/LSWTCSRuntime.exe"
CDB="C:/Program Files (x86)/Windows Kits/10/Debuggers/x64/cdb.exe"
ADDR2LINE="/c/msys64/mingw64/bin/llvm-addr2line.exe"
IMAGE_BASE=0x140000000            # PE preferred base; static VA = IMAGE_BASE + RVA

# The proven render config (frontier 1 fixed: GilMutexGuard + KPRCB-tick both host-side).
# VCLOCK + KTICK + BINDCLAMP + HEAPBYPASS default-on in code; listed here for clarity/A-B.
RENDER_ENV="LSWTCS_COMPILE=1 LSWTCS_HEAPBYPASS=1 LSWTCS_SEMFIX=1 LSWTCS_STUBSRC=1 LSWTCS_RUNSHC=1 \
LSWTCS_GIL=1 LSWTCS_AE698EXIT=1 LSWTCS_SYNCREAD=1 LSWTCS_SPINGUARD=1 LSWTCS_WORKERGUARD=1 \
LSWTCS_BINDCLAMP=1 LSWTCS_GEOM=1 LSWTCS_TRACE=1"

pid_of() { tasklist //FI "IMAGENAME eq LSWTCSRuntime.exe" //FO CSV //NH 2>/dev/null | sed 's/"//g' | cut -d, -f2 | head -1; }

cmd_build() { ( cd "$BUILD" && cmake --build . --target LSWTCSRuntime 2>&1 | tail -6 ); }

cmd_kill() { taskkill //F //IM LSWTCSRuntime.exe 2>&1 | head -1; }

cmd_run() {
    local secs="${1:-45}"
    cmd_kill >/dev/null 2>&1; sleep 1
    ( cd "$BUILD" && rm -f trace.log && rm -rf shots && \
      env $RENDER_ENV ${EXTRA:-} "$EXE" > run_out.txt 2>&1 & )
    echo "launched (render config); waiting ${secs}s ..."
    sleep "$secs"
    cmd_progress
}

cmd_progress() {
    ( cd "$BUILD"
      echo "=== VdSwap frame / DRAWSTAT ==="
      grep -aoE "VdSwap\] #[0-9]+" trace.log 2>/dev/null | tail -1
      grep -a "DRAWSTAT" trace.log 2>/dev/null | tail -1
      echo "=== max draws seen ==="
      grep -aoE "draws=[1-9][0-9]*" trace.log 2>/dev/null | sort -t= -k2 -n | tail -1
      echo "=== GEOM ==="
      grep -a "GEOM\] drew" trace.log 2>/dev/null | tail -1
      echo -n "=== alive? ==="; tasklist //FI "IMAGENAME eq LSWTCSRuntime.exe" 2>/dev/null | grep -ic lswtcs )
}

cmd_trace() {
    cmd_run "${1:-45}"
    ( cd "$BUILD"
      echo "=== SHADERID (distinct shaders) ==="; grep -a "SHADERID\|TEXCONST\|GEOMCOL" trace.log | head -20
      echo "=== file IO ==="; grep -a "NtCreateFile\|NtReadFile" trace.log | tail -20 )
}

cmd_stack() {
    local pid; pid="$(pid_of)"
    [ -z "$pid" ] && { echo "no live LSWTCSRuntime process"; return 1; }
    echo "attaching cdb to PID $pid ..."
    ( cd "$BUILD" && "$CDB" -p "$pid" -c "~* k 20; qd" > cdb_stacks.txt 2>&1 )
    echo "raw stacks -> $BUILD/cdb_stacks.txt"
    echo "=== symbolized thread leaders (top LSWTCSRuntime frame per thread) ==="
    grep -E "Id:|LSWTCSRuntime\+0x" "$BUILD/cdb_stacks.txt" | \
    while read -r line; do
        if echo "$line" | grep -q "Id:"; then echo "$line" | sed 's/ Suspend.*//'; continue; fi
        rva=$(echo "$line" | grep -oE "LSWTCSRuntime\+0x[0-9a-f]+" | head -1 | sed 's/.*+0x//')
        [ -z "$rva" ] && continue
        va=$(printf "%x" $((IMAGE_BASE + 0x$rva)))
        sym=$("$ADDR2LINE" -e "$EXE" -f -C "0x$va" 2>/dev/null | head -1)
        printf "    +0x%-9s %s\n" "$rva" "$sym"
    done
}

cmd_sym() {
    for rva in "$@"; do
        rva="${rva#0x}"; rva="${rva#+0x}"
        va=$(printf "%x" $((IMAGE_BASE + 0x$rva)))
        printf "+0x%-9s -> %s\n" "$rva" "$("$ADDR2LINE" -e "$EXE" -f -C "0x$va" 2>/dev/null | head -1)"
    done
}

cmd_shot() { echo "latest capture: $BUILD/shots/latest.png  (enable with LSWTCS_SHOTS=N)"; ls -la "$BUILD/shots/" 2>/dev/null | tail -5; }

case "${1:-run}" in
    build)  cmd_build ;;
    run)    cmd_run   "${2:-}" ;;
    trace)  cmd_trace "${2:-}" ;;
    stack)  cmd_stack ;;
    sym)    shift; cmd_sym "$@" ;;
    shot)   cmd_shot ;;
    kill)   cmd_kill ;;
    prog|progress) cmd_progress ;;
    *)      echo "usage: $0 {build|run [s]|trace [s]|stack|sym 0xRVA...|shot|kill|prog}"; exit 1 ;;
esac
