cmake_minimum_required(VERSION 3.20)
project(LSWTCSRuntime CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Project root = parent of this directory; sibling checkouts are resolved from it.
get_filename_component(LSW_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
get_filename_component(XENONRECOMP_DIR "${LSW_ROOT}/../XenonRecomp" ABSOLUTE)
set(RECOMP_OUTPUT "${LSW_ROOT}/Convert 360/LSWTCS/output")

include_directories(include)
include_directories("${XENONRECOMP_DIR}/XenonUtils")
include_directories("${LSW_ROOT}/simde/simde")

file(GLOB_RECURSE RUNTIME_SOURCES "source/*.cpp")
file(GLOB_RECURSE GAME_SOURCES "${RECOMP_OUTPUT}/*.cpp")

# Game code compiled as a separate object library for isolation and speed
add_library(GameCode OBJECT ${GAME_SOURCES})

target_include_directories(GameCode PRIVATE
    include
    "${XENONRECOMP_DIR}/XenonUtils"
    "${LSW_ROOT}/simde/simde"
)

target_compile_options(GameCode PRIVATE
    -O2
    -march=native      # MOVBE for single-instruction guest byteswap loads; AVX for simde vector ops
    -g
    -w
    -fno-exceptions
    -fno-rtti
    -ffunction-sections
    -fdata-sections
)

target_precompile_headers(GameCode PRIVATE
    "${RECOMP_OUTPUT}/ppc_recomp_shared.h"
)

set_target_properties(GameCode PROPERTIES
    UNITY_BUILD ON
    UNITY_BUILD_BATCH_SIZE 20
)

# Vendored Xenia CPU shader interpreter (xenia-canary, BSD) behind a small C bridge.
# Isolated target: Xenia's headers must not mix with the recompiler's ppc_* headers.
# Shim headers (xenos/shim) replace Xenia's Memory / Shader / TraceWriter classes.
set(XENIA_ROOT "${LSW_ROOT}/xenia-canary")
add_library(XenosInterp STATIC
    "${XENIA_ROOT}/src/xenia/gpu/shader_interpreter.cc"
    "${XENIA_ROOT}/src/xenia/gpu/ucode.cc"
    xenos/xenos_glue.cpp
)
target_include_directories(XenosInterp PRIVATE
    xenos/shim
    "${XENIA_ROOT}/src"
    "${XENIA_ROOT}"
    "${XENIA_ROOT}/third_party/fmt/include"
)
# NDEBUG: Xenia asserts are debug sanity checks; the interpreter already handles each case
# (e.g. loop-stack underflow) right after. With them on, one odd shader aborts the game.
# xe=lswxi: the interpreter uses SHIM Shader/Memory classes; give its whole Xenia namespace a
# private name so it cannot ODR-clash (COMDAT-merged inline members!) with the real Xenia classes
# in XenosDxbc below. Its public interface (xenos_vs.h) is extern "C".
target_compile_definitions(XenosInterp PRIVATE NDEBUG xe=lswxi)

# Xenia's DXBC shader translator (real Xenia headers + a few stubs) for GPU-side shading.
set(XG "${XENIA_ROOT}/src/xenia/gpu")
add_library(XenosDxbc STATIC
    "${XG}/dxbc_shader_translator.cc" "${XG}/dxbc_shader_translator_alu.cc"
    "${XG}/dxbc_shader_translator_fetch.cc" "${XG}/dxbc_shader_translator_om.cc"
    "${XG}/dxbc_shader_translator_memexport.cc" "${XG}/dxbc_shader.cc" "${XG}/shader.cc"
    "${XG}/shader_translator.cc" "${XG}/shader_translator_disasm.cc" "${XG}/ucode.cc"
    "${XG}/xenos.cc" "${XG}/gpu_flags.cc" "${XG}/draw_util.cc" "${XG}/texture_info.cc"
    "${XG}/texture_info_formats.cc" "${XG}/texture_util.cc" "${XG}/texture_extent.cc"
    "${XG}/register_file.cc" "${XG}/registers.cc"
    "${XENIA_ROOT}/src/xenia/ui/graphics_util.cc"
    "${XENIA_ROOT}/src/xenia/base/string_buffer.cc" "${XENIA_ROOT}/src/xenia/base/memory.cc"
    "${XENIA_ROOT}/src/xenia/base/string.cc" "${XENIA_ROOT}/src/xenia/base/utf8.cc"
    "${XENIA_ROOT}/src/xenia/base/cvar.cc"
    "${XENIA_ROOT}/third_party/dxbc/DXBCChecksum.cpp"
    xenos_dxbc/lsw_dxbc_stubs.cc
    xenos_dxbc/lsw_gpu_bridge.cc
    xenos_dxbc/lsw_gpu_texture.cc
    xenos_dxbc/lsw_gpu_gs.cc
)
target_include_directories(XenosDxbc PRIVATE
    xenos_dxbc/shim "${XENIA_ROOT}/src" "${XENIA_ROOT}"
    "${XENIA_ROOT}/third_party/fmt/include" "${XENIA_ROOT}/third_party")
target_compile_definitions(XenosDxbc PRIVATE NDEBUG FMT_HEADER_ONLY XE_PLATFORM_WIN32=1)
target_compile_options(XenosDxbc PRIVATE -std=c++20 -O2 -march=native -g -w
    -include "${CMAKE_CURRENT_SOURCE_DIR}/xenos_dxbc/shim/lsw_dxbc_compat.h")
target_compile_options(XenosInterp PRIVATE
    -O2 -march=native -g -w
    -include "${CMAKE_CURRENT_SOURCE_DIR}/xenos/shim/lsw_xenia_compat.h"
)

# XMA audio decoding: FFmpeg subset from Xenia's fork (third_party/FFmpeg @ the xenia-canary
# submodule commit, branch xmaframes) + Xenia's XmaContextNew driven by xma/lsw_xma.cc.
enable_language(C)
set(FFMPEG_ROOT "${XENIA_ROOT}/third_party/FFmpeg")
set(FFMPEG_CFG  "${XENIA_ROOT}/third_party/ffmpeg-xenia")
include("${CMAKE_CURRENT_SOURCE_DIR}/ffmpeg_sources.cmake")
foreach(lib avutil avcodec)
  string(TOUPPER ${lib} U)
  add_library(lsw_${lib} STATIC ${FFMPEG_LIB${U}_SOURCES})
  target_include_directories(lsw_${lib} PRIVATE "${FFMPEG_CFG}" "${FFMPEG_ROOT}")
  target_compile_definitions(lsw_${lib} PRIVATE HAVE_AV_CONFIG_H BUILDING_${lib} _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS)
  target_compile_options(lsw_${lib} PRIVATE -O2 -w -std=gnu11)
endforeach()
target_include_directories(lsw_avcodec PRIVATE "${FFMPEG_ROOT}/libavcodec")
add_library(lsw_xma STATIC
    xma/lsw_xma.cc
    "${XENIA_ROOT}/src/xenia/apu/xma_context.cc" "${XENIA_ROOT}/src/xenia/apu/xma_context_new.cc"
    "${XENIA_ROOT}/src/xenia/base/bit_stream.cc" "${XENIA_ROOT}/src/xenia/base/ring_buffer.cc"
    "${XENIA_ROOT}/src/xenia/base/mutex.cc")
target_include_directories(lsw_xma PRIVATE
    xma/shim "${XENIA_ROOT}/src" "${XENIA_ROOT}" "${XENIA_ROOT}/third_party/fmt/include"
    "${XENIA_ROOT}/third_party" "${FFMPEG_CFG}" "${FFMPEG_ROOT}")
target_compile_definitions(lsw_xma PRIVATE NDEBUG FMT_HEADER_ONLY XE_PLATFORM_WIN32=1)
target_compile_options(lsw_xma PRIVATE -std=c++20 -O2 -march=native -g -w
    -include "${CMAKE_CURRENT_SOURCE_DIR}/xenos_dxbc/shim/lsw_dxbc_compat.h")

# Runtime executable — runtime stubs compile independently
add_executable(LSWTCSRuntime ${RUNTIME_SOURCES})
# Data-file root for image.bin / pristine.bin / game files (overridable at runtime via env).
# Per-source so the define doesn't touch GameCode's flags.
set_source_files_properties(source/main.cpp source/kernel_stubs.cpp PROPERTIES
    COMPILE_DEFINITIONS "LSW_ROOT_DIR=\"${LSW_ROOT}\"")


set(WINSDK_LIB "C:/Program Files (x86)/Windows Kits/10/Lib/10.0.26100.0/um/x64")

target_link_libraries(LSWTCSRuntime PRIVATE
    GameCode
    XenosInterp
    XenosDxbc
    lsw_xma lsw_avcodec lsw_avutil bcrypt
    "${WINSDK_LIB}/d3d12.lib"
    "${WINSDK_LIB}/dxgi.lib"
    "${WINSDK_LIB}/dxguid.lib"
    "${WINSDK_LIB}/d3dcompiler.lib"
    "${WINSDK_LIB}/winmm.lib"
)

target_compile_options(LSWTCSRuntime PRIVATE -O2 -march=native -fno-strict-aliasing -g -w)
# xxHash (header-only, vendored by xenia-canary): XXH3 for vertex-page residency hashing.
target_include_directories(LSWTCSRuntime PRIVATE "${XENIA_ROOT}/third_party/xxhash")

target_link_options(LSWTCSRuntime PRIVATE
    -fuse-ld=lld
    -Wl,--gc-sections
    -Wl,--allow-multiple-definition
    -Wl,-Map=LSWTCSRuntime.map   # symbol map: resolve recomp RVAs -> sub_XXXXXXXX
)