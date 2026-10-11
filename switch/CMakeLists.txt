# Nintendo Switch build (devkitA64 + libnx). Configure from the devkitPro MSYS2 shell:
#   /c/devkitPro/msys2/usr/bin/bash.exe -lc 'cd <repo>/switch && \
#       aarch64-none-elf-cmake -G Ninja -S . -B build -DCMAKE_BUILD_TYPE=Release && ninja -C build'
# (or switch/build.sh). Output: build/lswtcs.nro.
#
# Recompiled game code + the PC runtime (unchanged sources, Win32 subset from include/win32 +
# source/win32_switch.cpp) + Switch platform layer + OpenGL backend (M3 stage 1, gl_backend.cpp:
# clear + shader compile check) + null audio backend.
# Game data lives in sdmc:/lswtcs (image.bin, pristine.bin, GAME.DAT, ..., env.txt).
cmake_minimum_required(VERSION 3.20)
project(LSWTCSSwitch C CXX ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(LSW_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(RECOMP_OUTPUT "${LSW_ROOT}/Convert 360/LSWTCS/output")

set(LSW_ARCH_FLAGS -march=armv8-a+crc+crypto -mtune=cortex-a57)
# SHELL: keeps each "-include <file>" pair together (CMake de-duplicates repeated options).
set(LSW_COMPAT "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/include/lsw_switch_compat.h")
set(LSW_WIN32 "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/include/win32/windows.h")

# ---- recompiled game code ------------------------------------------------------------------
# Hand-made unity batches (20 files each). CMake's own UNITY_BUILD / precompiled headers write
# ABSOLUTE paths into generated sources; under devkitPro's MSYS2 cmake those are /home/... paths the
# native compiler cannot open. Relative #includes resolve identically in both path worlds.
file(GLOB GAME_SOURCES "${RECOMP_OUTPUT}/*.cpp")
list(SORT GAME_SOURCES)
set(UNITY_DIR "${CMAKE_CURRENT_BINARY_DIR}/unity")
set(UNITY_SOURCES "")
set(_batch "")
set(_n 0)
set(_u 0)
foreach(src IN LISTS GAME_SOURCES)
    file(RELATIVE_PATH rel "${UNITY_DIR}" "${src}")
    string(APPEND _batch "#include \"${rel}\"\n")
    math(EXPR _n "${_n} + 1")
    if(_n EQUAL 20)
        file(CONFIGURE OUTPUT "${UNITY_DIR}/game_${_u}.cpp" CONTENT "${_batch}" @ONLY)
        list(APPEND UNITY_SOURCES "${UNITY_DIR}/game_${_u}.cpp")
        set(_batch "")
        set(_n 0)
        math(EXPR _u "${_u} + 1")
    endif()
endforeach()
if(_n GREATER 0)
    file(CONFIGURE OUTPUT "${UNITY_DIR}/game_${_u}.cpp" CONTENT "${_batch}" @ONLY)
    list(APPEND UNITY_SOURCES "${UNITY_DIR}/game_${_u}.cpp")
endif()
add_library(GameCode OBJECT ${UNITY_SOURCES})
target_include_directories(GameCode PRIVATE
    "${RECOMP_OUTPUT}" "${LSW_ROOT}/LSWTCSRuntime/include" "${LSW_ROOT}/simde/simde")
target_compile_options(GameCode PRIVATE
    # -g1 (line tables only): full -g on 20-file unity units costs GCC several GB each.
    ${LSW_ARCH_FLAGS} ${LSW_COMPAT} -O2 -g1 -w -fno-exceptions -fno-rtti
    -ffunction-sections -fdata-sections)

# ---- XMA audio decoding: FFmpeg subset (aarch64 files instead of x86) + Xenia XmaContextNew ----
set(XENIA_ROOT "${LSW_ROOT}/xenia-canary")
set(FFMPEG_ROOT "${XENIA_ROOT}/third_party/FFmpeg")
set(FFMPEG_CFG  "${XENIA_ROOT}/third_party/ffmpeg-xenia")
include("${LSW_ROOT}/LSWTCSRuntime/ffmpeg_sources.cmake")
list(FILTER FFMPEG_LIBAVUTIL_SOURCES EXCLUDE REGEX "/x86/")
list(FILTER FFMPEG_LIBAVCODEC_SOURCES EXCLUDE REGEX "/x86/")
# From xenia-canary/third_party/ffmpeg-xenia/lib*/premake5.lua (Android-ARM64 lists); files
# missing from this FFmpeg revision are skipped.
foreach(f libavutil/aarch64/cpu.c libavutil/aarch64/float_dsp_init.c libavutil/aarch64/tx_float_init.c
          libavutil/aarch64/float_dsp_neon.S libavutil/aarch64/tx_float_neon.S)
    if(EXISTS "${FFMPEG_ROOT}/${f}")
        list(APPEND FFMPEG_LIBAVUTIL_SOURCES "${FFMPEG_ROOT}/${f}")
    endif()
endforeach()
foreach(f libavcodec/aarch64/idctdsp_init_aarch64.c libavcodec/aarch64/mpegaudiodsp_init.c
          libavcodec/aarch64/fft_neon.S libavcodec/aarch64/simple_idct_neon.S
          libavcodec/aarch64/mdct_neon.S libavcodec/aarch64/mpegaudiodsp_neon.S)
    if(EXISTS "${FFMPEG_ROOT}/${f}")
        list(APPEND FFMPEG_LIBAVCODEC_SOURCES "${FFMPEG_ROOT}/${f}")
    endif()
endforeach()
foreach(lib avutil avcodec)
    string(TOUPPER ${lib} U)
    add_library(lsw_${lib} STATIC ${FFMPEG_LIB${U}_SOURCES})
    target_include_directories(lsw_${lib} PRIVATE include/ffmpeg "${FFMPEG_CFG}" "${FFMPEG_ROOT}")
    target_compile_definitions(lsw_${lib} PRIVATE HAVE_AV_CONFIG_H BUILDING_${lib} _USE_MATH_DEFINES)
    target_compile_options(lsw_${lib} PRIVATE ${LSW_ARCH_FLAGS} -O2 -w $<$<COMPILE_LANGUAGE:C>:-std=gnu11>
        $<$<COMPILE_LANGUAGE:C>:-Wno-incompatible-pointer-types>)   # GCC 15 makes these errors; clang warns
endforeach()
target_include_directories(lsw_avcodec PRIVATE "${FFMPEG_ROOT}/libavcodec")

# Xenia sources see the Switch as Linux (XE_PLATFORM_LINUX); the two Linux syscalls they use
# (futex, gettid) come from source/linux_shim.cpp, Win32 events from the win32 shim.
add_library(lsw_xma STATIC
    "${LSW_ROOT}/LSWTCSRuntime/xma/lsw_xma.cc"
    "${XENIA_ROOT}/src/xenia/apu/xma_context.cc" "${XENIA_ROOT}/src/xenia/apu/xma_context_new.cc"
    "${XENIA_ROOT}/src/xenia/base/bit_stream.cc" "${XENIA_ROOT}/src/xenia/base/ring_buffer.cc"
    "${XENIA_ROOT}/src/xenia/base/mutex.cc" "${XENIA_ROOT}/src/xenia/base/memory.cc"
    "${XENIA_ROOT}/src/xenia/base/cvar.cc" source/xe_stubs.cc)
target_include_directories(lsw_xma PRIVATE
    include/linuxshim include/win32 "${LSW_ROOT}/LSWTCSRuntime/xma/shim"
    "${XENIA_ROOT}/src" "${XENIA_ROOT}" "${XENIA_ROOT}/third_party/fmt/include" "${XENIA_ROOT}/third_party"
    "${FFMPEG_CFG}" "${FFMPEG_ROOT}"
    "${RECOMP_OUTPUT}" "${LSW_ROOT}/LSWTCSRuntime/include" "${LSW_ROOT}/simde/simde")
target_compile_definitions(lsw_xma PRIVATE NDEBUG FMT_HEADER_ONLY __gnu_linux__)
target_compile_options(lsw_xma PRIVATE ${LSW_ARCH_FLAGS} ${LSW_COMPAT} ${LSW_WIN32} -O2 -g -w)

# ---- OpenGL shader path (M3): Xenos ucode -> Xenia SpirvShaderTranslator -> SPIR-V -> SPIRV-Cross
# -> GLSL 4.30, compiled at run time by switch-mesa. SPIRV-Cross: checkout at ../SPIRV-Cross pinned by
# patches/SPIRV-Cross.base; glslang: xenia-canary's third_party/glslang submodule (SpvBuilder only).
# xenia/ui/vulkan/vulkan_device.h comes from include/spirvshim (no Vulkan device on this path).
# PC check of the same chain: tools/spirv_glsl_test.sh.
set(XG "${XENIA_ROOT}/src/xenia/gpu")
set(GLSLANG "${XENIA_ROOT}/third_party/glslang")
set(SPIRV_CROSS "${LSW_ROOT}/SPIRV-Cross")
add_library(lsw_spirv STATIC
    "${XG}/spirv_shader_translator.cc" "${XG}/spirv_shader_translator_alu.cc"
    "${XG}/spirv_shader_translator_fetch.cc" "${XG}/spirv_shader_translator_memexport.cc"
    "${XG}/spirv_shader_translator_rb.cc" "${XG}/spirv_shader.cc" "${XG}/spirv_builder.cc"
    "${XG}/shader.cc" "${XG}/shader_translator.cc" "${XG}/shader_translator_disasm.cc" "${XG}/ucode.cc"
    "${XG}/xenos.cc" "${XG}/gpu_flags.cc" "${XG}/draw_util.cc" "${XG}/texture_info.cc"
    "${XG}/texture_info_formats.cc" "${XG}/texture_util.cc" "${XG}/texture_extent.cc"
    "${XG}/register_file.cc" "${XG}/registers.cc" "${XENIA_ROOT}/src/xenia/ui/graphics_util.cc"
    "${XENIA_ROOT}/src/xenia/base/string_buffer.cc" "${XENIA_ROOT}/src/xenia/base/string.cc"
    "${XENIA_ROOT}/src/xenia/base/utf8.cc"
    "${GLSLANG}/SPIRV/SpvBuilder.cpp" "${GLSLANG}/SPIRV/InReadableOrder.cpp"
    "${GLSLANG}/SPIRV/SpvPostProcess.cpp" "${GLSLANG}/SPIRV/Logger.cpp"
    "${SPIRV_CROSS}/spirv_cross.cpp" "${SPIRV_CROSS}/spirv_parser.cpp"
    "${SPIRV_CROSS}/spirv_cross_parsed_ir.cpp" "${SPIRV_CROSS}/spirv_cfg.cpp" "${SPIRV_CROSS}/spirv_glsl.cpp"
    "${LSW_ROOT}/LSWTCSRuntime/xenos_dxbc/lsw_gpu_texture.cc"
    source/lsw_spirv_glsl.cc source/xe_gpu_stubs.cc source/gl_bridge.cc source/gl_geometry.cc)
# gl_geometry.cc: Xenia's Vulkan geometry-shader generator body, extracted at configure time.
set(LSW_GEN "${CMAKE_CURRENT_BINARY_DIR}/gen")
set(XE_GS_SRC "${XG}/vulkan/vulkan_pipeline_cache.cc")
set(XE_GS_OUT "${LSW_GEN}/xe_geometry_shader_body.inc")
include(tools/extract_gs_body.cmake)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${XE_GS_SRC}")
target_include_directories(lsw_spirv PRIVATE
    include include/spirvshim include/linuxshim include/win32
    "${XENIA_ROOT}/src" "${XENIA_ROOT}" "${XENIA_ROOT}/third_party/fmt/include" "${XENIA_ROOT}/third_party"
    "${GLSLANG}" "${SPIRV_CROSS}" "${LSW_ROOT}/LSWTCSRuntime/include" "${LSW_GEN}")
target_compile_definitions(lsw_spirv PRIVATE NDEBUG FMT_HEADER_ONLY __gnu_linux__)
target_compile_options(lsw_spirv PRIVATE ${LSW_ARCH_FLAGS} ${LSW_COMPAT} -O2 -g -w)

# ---- runtime ------------------------------------------------------------------------------
set(RT "${LSW_ROOT}/LSWTCSRuntime/source")
add_executable(lswtcs
    "${RT}/main.cpp" "${RT}/kernel_stubs.cpp" "${RT}/gpu_ringbuf.cpp" "${RT}/native_lz2k.cpp" "${RT}/native_reverb.cpp"
    source/platform_switch.cpp source/win32_switch.cpp source/null_backends.cpp source/linux_shim.cpp
    source/gl_backend.cpp source/mesa_shim.cpp source/audio_switch.cpp)
# The runtime's main() runs on a 16 MB thread started by platform_switch.cpp.
set_source_files_properties("${RT}/main.cpp" PROPERTIES COMPILE_DEFINITIONS "main=lsw_runtime_main")
target_include_directories(lswtcs PRIVATE
    include/win32 "${RT}" "${RECOMP_OUTPUT}" "${LSW_ROOT}/LSWTCSRuntime/include" "${LSW_ROOT}/simde/simde"
    "${XENIA_ROOT}/third_party/xxhash" include)
target_compile_definitions(lswtcs PRIVATE LSW_ROOT_DIR="sdmc:/lswtcs" LSW_DATA_DIR="sdmc:/lswtcs")
target_compile_options(lswtcs PRIVATE ${LSW_ARCH_FLAGS} ${LSW_COMPAT} ${LSW_WIN32} -O2 -g -w -fno-strict-aliasing)
# switch-mesa: glad (GL loader), EGL, glapi, nouveau DRM (devkitPro portlibs).
# switch-mesa rebuilt with Mesa's shader disk cache (tools/build_mesa.sh) is preferred when present:
# its libEGL/libglapi come first on the link path; it needs zlib. Else the stock devkitPro package.
set(LSW_MESA_CACHE_LIB "${CMAKE_CURRENT_SOURCE_DIR}/mesa/build/install/opt/devkitpro/portlibs/switch/lib")
# Absolute paths: the devkitPro toolchain file puts the stock portlibs lib dir first on the -L path.
if(EXISTS "${LSW_MESA_CACHE_LIB}/libEGL.a")
    message(STATUS "LSWTCS: linking switch-mesa with shader disk cache from ${LSW_MESA_CACHE_LIB}")
    set(LSW_MESA_LIBS "${LSW_MESA_CACHE_LIB}/libEGL.a" "${LSW_MESA_CACHE_LIB}/libglapi.a" z)
else()
    message(STATUS "LSWTCS: stock switch-mesa (no shader disk cache; run tools/build_mesa.sh)")
    set(LSW_MESA_LIBS EGL glapi)
endif()
target_include_directories(lswtcs PRIVATE "$ENV{DEVKITPRO}/portlibs/switch/include")
target_link_directories(lswtcs PRIVATE "$ENV{DEVKITPRO}/portlibs/switch/lib")
target_link_libraries(lswtcs PRIVATE GameCode lsw_spirv lsw_xma lsw_avcodec lsw_avutil glad ${LSW_MESA_LIBS} drm_nouveau)
target_link_options(lswtcs PRIVATE
    -Wl,--gc-sections
    -Wl,--wrap=pthread_create              # minimum thread stack (platform_switch.cpp)
    -Wl,--wrap=pthread_detach              # libnx cannot detach; std::thread::detach would throw
    -Wl,--wrap=exit -Wl,--wrap=abort       # log the caller (platform_switch.cpp)
    -Wl,--wrap=__cxa_throw                 # log libstdc++ throws (no EH in the runtime)
    -Wl,--wrap=__malloc_lock               # DIAG: malloc lock contention (platform_switch.cpp)
    -Wl,--wrap=thrd_create                 # newlib thrd_success = 4, Mesa expects 0 (platform_switch.cpp)
    -Wl,-Map=lswtcs.map)
# rpmalloc as the process allocator (third_party/rpmalloc/rpmalloc_switch.c): per-thread caches instead
# of newlib's single malloc lock, which serialised parallel shader builds. -DLSW_RPMALLOC=OFF = newlib.
option(LSW_RPMALLOC "Use rpmalloc for malloc/free" ON)
if(LSW_RPMALLOC)
    # Own object library: the runtime target's Win32 shim headers and flags must not reach rpmalloc.
    add_library(lsw_rpmalloc OBJECT third_party/rpmalloc/rpmalloc_switch.c)
    target_include_directories(lsw_rpmalloc PRIVATE third_party/rpmalloc/shim third_party/rpmalloc)
    target_compile_options(lsw_rpmalloc PRIVATE ${LSW_ARCH_FLAGS} -O2 -w)
    target_sources(lswtcs PRIVATE $<TARGET_OBJECTS:lsw_rpmalloc>)
    target_link_options(lswtcs PRIVATE
        -Wl,--wrap=_malloc_r -Wl,--wrap=_free_r -Wl,--wrap=_realloc_r -Wl,--wrap=_calloc_r
        -Wl,--wrap=_memalign_r -Wl,--wrap=_malloc_usable_size_r)
endif()

nx_generate_nacp(lswtcs.nacp NAME "LSWTCS" AUTHOR "LSWTCS port" VERSION "0.0.1")
nx_create_nro(lswtcs NACP lswtcs.nacp)

# M0 smoke test (no runtime): proves an NRO holding all game code loads. Not built by default.
add_executable(lswtcs_m0 EXCLUDE_FROM_ALL source/m0_main.cpp)
target_include_directories(lswtcs_m0 PRIVATE "${RECOMP_OUTPUT}" "${LSW_ROOT}/LSWTCSRuntime/include" "${LSW_ROOT}/simde/simde")
target_compile_options(lswtcs_m0 PRIVATE ${LSW_ARCH_FLAGS} ${LSW_COMPAT} -O2 -g -w)
target_link_libraries(lswtcs_m0 PRIVATE GameCode)
target_link_options(lswtcs_m0 PRIVATE -Wl,--gc-sections -Wl,--unresolved-symbols=ignore-all)
nx_create_nro(lswtcs_m0 NACP lswtcs.nacp)
