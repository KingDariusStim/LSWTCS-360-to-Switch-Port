// Shader objects passed from the Switch bridge (gl_bridge.cc, guest threads) to the OpenGL replay
// (gl_backend.cpp, render thread) through LswGpuDrawRec's vs_dxbc / ps_dxbc / gs_dxbc pointers.
// Created once by the bridge and never modified afterwards, except `gl`, which only the render
// thread touches.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "lsw_gpu_bridge.h"

enum LswGlStage : uint32_t { kLswGlVertex, kLswGlPixel, kLswGlGeometry };

// One texture unit: which fetch constant / dimension to bind and with which sampler state.
struct LswGlShaderUnit {
  uint32_t unit;
  uint32_t fetch_constant;     // 0..31
  uint32_t dimension;          // xenos::FetchOpDimension: 0 = 1D, 1 = 2D, 2 = 3D/stacked, 3 = cube
  uint32_t is_signed;
  LswGpuSamplerBinding sampler;
};

struct LswGlShader {
  LswGlStage stage;
  uint64_t key;
  std::string glsl;
  std::vector<LswGlShaderUnit> units;
  uint32_t gl = 0;   // GL shader object (render thread); kLswGlFailed if it did not compile
};
enum : uint32_t { kLswGlFailed = 0xFFFFFFFFu };

// Layout of the system-constants block the bridge hands to the recorder (one arena block):
// SpirvShaderTranslator::SystemConstants at 0 (UBO binding 0), ClipPlaneConstants at
// kLswGlClipPlaneOffset (UBO binding 5). 256 = GL uniform-buffer offset alignment on NVIDIA.
enum : uint32_t { kLswGlClipPlaneOffset = 512, kLswGlClipPlaneSize = 6 * 16 };
