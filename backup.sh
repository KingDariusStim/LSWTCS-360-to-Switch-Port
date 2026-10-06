#!/bin/bash
# usage: backup.sh <label>  -> backups/<timestamp>_<label>/ (runtime sources + full recomp output)
cd "$(dirname "$0")"
d="backups/$(date +%Y%m%d_%H%M%S)_$1"; mkdir -p "$d"
cp -r LSWTCSRuntime/source LSWTCSRuntime/include LSWTCSRuntime/xenos LSWTCSRuntime/CMakeLists.txt "$d/"
mkdir -p "$d/recomp_output" && cp "Convert 360/LSWTCS/output/"*.cpp "Convert 360/LSWTCS/output/"*.h "$d/recomp_output/"
cp run.sh "$d/" 2>/dev/null
echo "$d $(du -sh "$d" | cut -f1)"
