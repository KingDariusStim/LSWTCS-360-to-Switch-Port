#!/bin/bash
# Build the LSWTCS forwarder program: switchbrew nx-hbloader (third_party/nx-hbloader, unmodified clone) built so
# that it starts sdmc:/lswtcs/lswtcs.nro (outside sdmc:/switch, so hbmenu does not list the game) and exits to the
# Home menu when the game quits (stock hbloader restarts hbmenu instead). 39-bit address space (application).
# Packed as an installable title (needs sys-patch on the console, which this user has):
#   pkg/control/{control.nacp, icon_<Language>.dat}  (NACP: StartupUserAccount=Required, LDN id = save owner = TID)
#   hbp_build/hacbrewpack.exe -k <prod.keys path> --titleid <TID> --exefsdir pkg/exefs --controldir pkg/control
#       --noromfs --nologo --nspdir pkg/nsp --ncadir pkg/nca          (never read or copy the keys' contents)
# Title id used: 054C535443530000 (fixed forever: save data + LDN id depend on it).
# Usage: switch/forwarder/build_forwarder.sh <title id, 16 hex digits> [nro path on SD]
# Output: switch/forwarder/out/atmosphere/contents/<TID>/exefs/{main,main.npdm}  (copy to pkg/exefs before packing)
set -e
TID=$(echo "${1:?donor title id (16 hex digits)}" | tr 'a-f' 'A-F')
[[ "$TID" =~ ^[0-9A-F]{16}$ ]] || { echo "title id must be 16 hex digits"; exit 1; }
NRO=${2:-sdmc:/lswtcs/lswtcs.nro}
HERE=$(cd "$(dirname "$0")" && pwd)
B="$HERE/hbl_src"   # not "build": the devkitPro Makefile treats a dir named build as its inner pass
rm -rf "$B" && mkdir -p "$B" && cp -r "$HERE/third_party/nx-hbloader/." "$B/" && rm -rf "$B/.git"
# 1. default NRO = the game; exit after the game instead of reloading the default.
sed -i "s|#define DEFAULT_NRO \"sdmc:/hbmenu.nro\"|#define DEFAULT_NRO \"$NRO\"|" "$B/source/main.c"
sed -i 's|static char g_nextNroPath\[512\];|static char g_nextNroPath[512];\nstatic bool g_lswLoadedOnce = false;   // LSWTCS forwarder: exit after the game|' "$B/source/main.c"
sed -i 's|    if (g_nextNroPath\[0\] == .\\0.)$|    if (g_nextNroPath[0] == '"'"'\\0'"'"' \&\& g_lswLoadedOnce)\n        svcExitProcess();   // LSWTCS forwarder: the game quit (and chained nothing): back to the Home menu\n    g_lswLoadedOnce = true;\n\n    if (g_nextNroPath[0] == '"'"'\\0'"'"')|' "$B/source/main.c"
grep -q "g_lswLoadedOnce = true" "$B/source/main.c" || { echo "patching main.c failed"; exit 1; }
grep -q "$NRO" "$B/source/main.c" || { echo "patching DEFAULT_NRO failed"; exit 1; }
# 2. NPDM for the donor title.
sed -i "s|\"name\": \"hbloader\"|\"name\": \"lswtcs\"|; s|0x010000000000100D|0x$TID|g" "$B/hbl.json"
# 39-bit address space (type 3), as applications get: the runtime reserves a >4 GB guest window next to a ~3 GB heap
# (hbloader's own json says 1 = 36-bit; Atmosphère's hbl override fixes that with override_address_space=39_bit).
sed -i 's|"address_space_type": 1,|"address_space_type": 3,|' "$B/hbl.json"
grep -q '"address_space_type": 3,' "$B/hbl.json" || { echo "patching address_space_type failed"; exit 1; }
# Application, not applet (hbl.json targets the Album applet: application_type 2), and no forced debug: with either
# left as is the title fails to load (Eden "Failed to load ROM (Error 32)" = unable to parse kernel metadata; the
# console showed "an error occurred"). Tested 2026-10-09: type 1 + force_debug false boots in Eden; map_region is fine.
python - "$B/hbl.json" <<'PYEOF'
import json, sys
p = sys.argv[1]; j = json.load(open(p))
for c in j['kernel_capabilities']:
    if c['type'] == 'application_type': c['value'] = 1
    if c['type'] == 'debug_flags': c['value']['force_debug'] = False
json.dump(j, open(p, 'w'), indent=4)
PYEOF
# 3. build in devkitPro's shell.
WIN=$(cd "$B" && pwd -W)
/c/devkitPro/msys2/usr/bin/bash.exe -lc "cd \"\$(cygpath -u '$WIN')\" && make -j4" > "$HERE/build.log" 2>&1 || { tail -20 "$HERE/build.log"; exit 1; }
OUT="$HERE/out/atmosphere/contents/$TID/exefs"
rm -rf "$HERE/out" && mkdir -p "$OUT"
cp "$B/build/exefs/main" "$B/build/exefs/main.npdm" "$OUT/"
echo "forwarder for $TID -> $NRO"
find "$HERE/out" -type f -exec ls -la {} \;
