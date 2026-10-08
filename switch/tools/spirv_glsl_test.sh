#!/bin/bash
# Build spirv_glsl_test.exe on PC (MinGW clang) and run it over every shader in xenia/shdump, plus the
# point / rectangle / quad geometry shaders (switch/source/gl_geometry.cc).
# Usage: switch/tools/spirv_glsl_test.sh [outdir=switch/scratch/spirv] [ucode dir=xenia/shdump]
# (a Switch LSWTCS_UCODEDUMP corpus, e.g. switch/scratch/ucode_hub, carries each draw's modification).
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${1:-$ROOT/switch/scratch/spirv}
IN=${2:-$ROOT/xenia/shdump}
X=$ROOT/xenia-canary; XG=$X/src/xenia/gpu; GL=$X/third_party/glslang/SPIRV; SC=$ROOT/SPIRV-Cross
CXX=/c/msys64/mingw64/bin/clang++.exe
mkdir -p "$OUT/obj" "$OUT/gen"
/c/msys64/mingw64/bin/cmake.exe -DXE_GS_SRC="$XG/vulkan/vulkan_pipeline_cache.cc" \
  -DXE_GS_OUT="$OUT/gen/xe_geometry_shader_body.inc" -P "$ROOT/switch/tools/extract_gs_body.cmake" || exit 1
SRC=(
  "$ROOT/switch/tools/spirv_glsl_test.cc" "$ROOT/switch/source/lsw_spirv_glsl.cc" "$ROOT/switch/source/gl_geometry.cc"
  "$XG/spirv_shader_translator.cc" "$XG/spirv_shader_translator_alu.cc"
  "$XG/spirv_shader_translator_fetch.cc" "$XG/spirv_shader_translator_memexport.cc"
  "$XG/spirv_shader_translator_rb.cc" "$XG/spirv_shader.cc" "$XG/spirv_builder.cc"
  "$XG/shader.cc" "$XG/shader_translator.cc" "$XG/shader_translator_disasm.cc" "$XG/ucode.cc"
  "$XG/xenos.cc" "$XG/gpu_flags.cc" "$XG/draw_util.cc" "$XG/texture_info.cc"
  "$XG/texture_info_formats.cc" "$XG/texture_util.cc" "$XG/texture_extent.cc"
  "$XG/register_file.cc" "$XG/registers.cc" "$X/src/xenia/ui/graphics_util.cc"
  "$X/src/xenia/base/string_buffer.cc" "$X/src/xenia/base/memory.cc" "$X/src/xenia/base/string.cc"
  "$X/src/xenia/base/utf8.cc" "$X/src/xenia/base/cvar.cc"
  "$ROOT/LSWTCSRuntime/xenos_dxbc/lsw_dxbc_stubs.cc"
  "$GL/SpvBuilder.cpp" "$GL/InReadableOrder.cpp" "$GL/SpvPostProcess.cpp" "$GL/Logger.cpp"
  "$SC/spirv_cross.cpp" "$SC/spirv_parser.cpp" "$SC/spirv_cross_parsed_ir.cpp" "$SC/spirv_cfg.cpp"
  "$SC/spirv_glsl.cpp")
FLAGS=(-std=c++20 -O1 -g -march=native -w -DNDEBUG -DFMT_HEADER_ONLY -DXE_PLATFORM_WIN32=1
  -include "$ROOT/LSWTCSRuntime/xenos_dxbc/shim/lsw_dxbc_compat.h"
  -I"$ROOT/switch/include/spirvshim" -I"$ROOT/switch/include" -I"$ROOT/LSWTCSRuntime/xenos_dxbc/shim" -I"$X/src" -I"$X"
  -I"$X/third_party/fmt/include" -I"$X/third_party" -I"$X/third_party/glslang" -I"$SC" -I"$OUT/gen")
# Objects only track their own source file: rebuild everything when any relevant header is newer.
EXE=$OUT/spirv_glsl_test.exe
[ -f "$EXE" ] && [ -n "$(find "$XG" "$X/src/xenia/base" "$GL" "$SC" "$ROOT/switch/include" "$OUT/gen" \
  "$ROOT/LSWTCSRuntime/xenos_dxbc/shim" \( -name '*.h*' -o -name '*.inc' \) -newer "$EXE" -print -quit)" ] && rm -rf "$OUT/obj"/*.o
OBJS=(); FAIL=0
for s in "${SRC[@]}"; do
  o="$OUT/obj/$(basename "${s%.*}").o"; OBJS+=("$o")
  [ "$o" -nt "$s" ] && continue
  "$CXX" "${FLAGS[@]}" -c "$s" -o "$o" 2> "$o.err" || { echo "COMPILE FAIL $(basename "$s")"; head -20 "$o.err"; FAIL=1; } &
  while [ "$(jobs -r | wc -l)" -ge 8 ]; do sleep 0.2; done
done
wait
for o in "${OBJS[@]}"; do [ -f "$o" ] || FAIL=1; done
[ $FAIL = 0 ] || exit 1
"$CXX" "${OBJS[@]}" -o "$EXE" -static 2> "$OUT/link.err" || { head -40 "$OUT/link.err"; exit 1; }
echo "[build] ok"
N=0; OK=0
for f in "$IN"/*.ucode.bin.vert "$IN"/*.ucode.bin.frag; do
  b=$(basename "$f"); k=vs; [[ $f == *.frag ]] && k=ps
  N=$((N + 1))
  "$EXE" $k "$f" "$OUT/$b.spv" > "$OUT/$b.glsl" 2> "$OUT/$b.err" && OK=$((OK + 1)) || echo "FAIL $b: $(tail -1 "$OUT/$b.err")"
done
# Geometry shaders: type (1 point, 2 rect, 3 quad) | interpolators << 2 | clip planes << 7 | point size << 10.
for key in 0x11 0x12 0x13 0x92 0x413 0x81; do
  N=$((N + 1))
  "$EXE" gs $key > "$OUT/gs_$key.geom.glsl" 2> "$OUT/gs_$key.err" && OK=$((OK + 1)) || echo "FAIL gs $key: $(tail -1 "$OUT/gs_$key.err")"
done
echo "[run] $OK / $N shaders -> GLSL in $OUT"
# Validate the GLSL against the OpenGL spec with glslang's standalone compiler, if built
# (cmake -S xenia-canary/third_party/glslang -B switch/scratch/glslang_build -DENABLE_OPT=OFF ...).
V=$ROOT/switch/scratch/glslang_build/StandAlone/glslang.exe
if [ -x "$V" ]; then
  VOK=0; VN=0
  for g in "$OUT"/*.glsl; do
    st=vert; [[ $g == *.frag.glsl ]] && st=frag; [[ $g == *.geom.glsl ]] && st=geom; VN=$((VN + 1))
    PATH=/c/msys64/mingw64/bin:$PATH "$V" -S $st "$g" > "$g.val" 2>&1 && VOK=$((VOK + 1)) || echo "INVALID $(basename "$g")"
  done
  echo "[validate] $VOK / $VN compile as GLSL 4.30 (glslang)"
fi
