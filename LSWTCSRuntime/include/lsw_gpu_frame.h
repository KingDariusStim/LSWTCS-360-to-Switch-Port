// Per-frame list of guest draws recorded by the PM4 executor (kernel_stubs.cpp) for the D3D12
// translated-shader path (gpu_d3d12.cpp). All data a draw needs is snapshotted into a byte arena
// at record time, because guest memory (vertex/index buffers, constants) changes before present.
#pragma once
#include <cstdint>
#include <vector>
#include "lsw_gpu_bridge.h"

struct LswGpuRange { uint32_t phys; uint32_t size; uint32_t arena_off; };

struct LswGpuDrawRec {
  // 0 = draw, 1 = EDRAM resolve (only the rt / rs_* fields are meaningful).
  uint32_t kind;
  // EDRAM render target state: RB_SURFACE_INFO, RB_COLOR_INFO (of RT 0, or of the resolve
  // source), RB_DEPTH_INFO, RB_MODECONTROL edram mode (4 color+depth, 5 depth only, 6 copy).
  uint32_t surf_info, color_info, depth_info, edram_mode;
  // Multiple render targets: RB_COLOR_INFO / COLOR1..3_INFO, RB_BLENDCONTROL0..3, PS output mask.
  uint32_t color_info_mrt[4], blend_mrt[4], ps_rt_mask;
  uint32_t stencil_ref, stencil_ref_bf;   // RB_STENCILREFMASK (0x210D), RB_STENCILREFMASK_BF (0x210C)
  // Resolve: source (0-3 color RT, 4 depth), sample select, rectangle in surface pixels
  // [x0,x1)x[y0,y1), destination (physical base, pitch/height in texels, TextureFormat, texel
  // offset of the rectangle inside it), clears applied to the source after the copy.
  uint32_t rs_src, rs_sample, rs_copy_cmd;
  int32_t rs_rect[4];
  uint32_t rs_dest, rs_dest_pitch, rs_dest_height, rs_dest_fmt;
  int32_t rs_dest_x, rs_dest_y;
  uint32_t rs_clear_color, rs_clear_depth, rs_color_clear, rs_color_clear_lo, rs_depth_clear;
  // Shaders (bridge-owned DXBC) and cache keys.
  const void* vs_dxbc; uint32_t vs_size; uint64_t vs_key;
  const void* ps_dxbc; uint32_t ps_size; uint64_t ps_key;
  // Constant buffers: arena offsets (256-aligned) and sizes in bytes.
  uint32_t cb_sys_off, cb_sys_size;
  uint32_t cb_vsf_off, cb_vsf_size;
  uint32_t cb_psf_off, cb_psf_size;
  uint32_t cb_bool_off, cb_fetch_off;
  // Vertex data the VS reads from shared memory: [first_range, first_range + range_count).
  uint32_t first_range, range_count;
  // Index buffer (raw guest bytes in the arena; the VS applies the endian swap itself).
  uint32_t ib_off, ib_count, ib_32bit;   // ib_count == 0: non-indexed (vertex_count used)
  uint32_t vertex_count;
  uint32_t topology;                     // D3D_PRIMITIVE_TOPOLOGY
  const void* gs_dxbc; uint32_t gs_size; uint32_t gs_type;   // optional geometry shader
  // Fixed-function state.
  uint32_t blendcontrol, color_mask, depthcontrol, sc_mode_cntl;
  float viewport[6]; uint32_t scissor[4];
  uint32_t ps_tex_count, ps_smp_count, vs_tex_count, vs_smp_count;
  // Texture bindings: fetch constant index + dimension per slot (resolved at replay).
  uint8_t ps_tex_fetch[32], ps_tex_dim[32], vs_tex_fetch[32], vs_tex_dim[32];
  LswGpuSamplerBinding ps_smp[32], vs_smp[32];
};

struct LswGpuFrame {
  std::vector<uint8_t> arena;
  std::vector<LswGpuRange> ranges;
  std::vector<LswGpuDrawRec> draws;
  uint32_t swap_fb = 0;   // frontbuffer address in the PM4_XE_SWAP packet that ended this frame
  void clear() { arena.clear(); ranges.clear(); draws.clear(); }
  uint32_t alloc(uint32_t size, uint32_t align = 256) {
    uint32_t off = uint32_t((arena.size() + (align - 1)) & ~size_t(align - 1));
    arena.resize(off + size);
    return off;
  }
};

// Building (PM4 thread) and published (present) frames; swapped at the guest swap marker.
extern LswGpuFrame g_gpu_frame_build, g_gpu_frame_present;
extern bool g_gpu_frame_ready;
