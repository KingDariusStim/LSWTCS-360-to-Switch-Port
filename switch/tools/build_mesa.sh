#!/bin/bash
# Rebuild switch-mesa 20.1.0-rc3 (devkitPro's package, recipe pinned below) WITH Mesa's shader disk
# cache, so the game's GL programs are compiled once per console instead of once per launch.
# Result: switch/mesa/build/install/opt/devkitpro/portlibs/switch/{lib,include} — switch/CMakeLists.txt
# links against it when present (else the stock switch-mesa). The system devkitPro install is untouched.
#
# Steps: download the Mesa tarball (sha256-checked) + devkitPro's switch/mesa patches at the pinned
# commit, apply them, apply switch/mesa/mesa_cache_edits.py (cache on horizon: zlib, no mmap index,
# fixed driver id, no flock; Python 3.12 build fix), configure with -Dshader-cache=true, build, install.
# Needs (devkitPro pacman): dkp-meson-scripts switch-pkg-config dkp-toolchain-vars meson python
# python-mako bison flex switch-zlib. Usage: switch/tools/build_mesa.sh [ninja jobs=6]
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
B=$ROOT/switch/mesa/build
JOBS=${1:-6}
MESA_URL=https://archive.mesa3d.org/older-versions/20.x/mesa-20.1.0-rc3.tar.xz
MESA_SHA=c90b75ea34302ebde9b81b87c5642fa864c40fe9c4ad34ce0793170c1413168d
PKG_REPO=https://github.com/devkitPro/pacman-packages.git
PKG_COMMIT=f103fe88e37180ecd0b7a9173b52b7580a54f71a   # switch/mesa: switch-mesa 20.1.0-5
mkdir -p "$B/dl"
cd "$B/dl"
[ -f mesa-20.1.0-rc3.tar.xz ] || curl -sSLf -o mesa-20.1.0-rc3.tar.xz "$MESA_URL"
echo "$MESA_SHA *mesa-20.1.0-rc3.tar.xz" | sha256sum -c -
if [ ! -d pkgs ]; then
    git clone -q --filter=blob:none --sparse "$PKG_REPO" pkgs
    git -C pkgs sparse-checkout set switch/mesa
fi
git -C pkgs checkout -q "$PKG_COMMIT"
P=$B/dl/pkgs/switch/mesa
rm -rf "$B/src" && mkdir -p "$B/src" && cd "$B/src"
tar --force-local -xf "$B/dl/mesa-20.1.0-rc3.tar.xz"
cd mesa-20.1.0-rc3
patch -p1 -s -i "$P/switch-mesa-20.1.0-5.patch"
patch -p1 -s -i "$P/gl_XML.py.patch"
patch -p1 -s -i "$P/glX_XML.py.patch"
python "$ROOT/switch/mesa/mesa_cache_edits.py" "$PWD"
python "$ROOT/switch/mesa/mesa_nir_edits.py" "$PWD"
python "$ROOT/switch/mesa/mesa_fmz_edits.py" "$PWD"
W=$(pwd -W)
# MSYSTEM=MSYS: meson rejects the MSYS python inside devkitPro's default (MinGW-flavoured) shell.
MSYSTEM=MSYS /c/devkitPro/msys2/usr/bin/bash.exe -lc "cd \"\$(cygpath -u '$W')\" && \
  /opt/devkitpro/meson-cross.sh switch ../crossfile.txt build -Db_ndebug=true -Dshader-cache=true && \
  ninja -C build -j$JOBS && DESTDIR=\"\$(cygpath -u '$(cd "$B" && pwd -W)')/install\" ninja -C build install"
echo "[build_mesa] installed to $B/install"
