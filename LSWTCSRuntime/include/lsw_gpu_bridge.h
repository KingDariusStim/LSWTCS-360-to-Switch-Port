// Bridge between the runtime's PM4 executor and Xenia's DXBC shader translator (XenosDxbc).
// Given the guest register file and the bound Xenos shaders for one draw, produces translated
// DXBC (cached), the packed constant-buffer contents and the binding info that a D3D12 back end
// needs to issue the draw with Xenia's bindful root-signature layout:
//   b0 system constants (all), b1 float constants (VS / PS separately), b2 bool+loop (all),
//   b3 fetch constants (all), table{t0 shared memory SRV, u0 shared memory UAV} (all),
//   PS table{t1.. textures}, PS table{s0.. samplers}, VS table{t1..}, VS table{s0..}.
#pragma once
#include <cstddef>
#include <cstdint>

extern "C" {

enum { LSW_GPU_MAX_TEX = 32, LSW_GPU_MAX_VFETCH = 96 };

struct LswGpuTexBinding {
  uint32_t fetch_constant;  // 0..31
  uint32_t dimension;       // xenos::FetchOpDimension: 0=1D, 1=2D, 2=3D/stacked, 3=cube
  uint32_t is_signed;
};
struct LswGpuSamplerBinding {
  uint32_t fetch_constant;
  uint32_t mag_filter, min_filter, mip_filter, aniso_filter;  // xenos enums
};

struct LswGpuDraw {
  // Translated shaders (owned by the bridge cache; stable pointers).
  const void* vs_dxbc; size_t vs_dxbc_size; uint64_t vs_key;
  const void* ps_dxbc; size_t ps_dxbc_size; uint64_t ps_key;   // ps_dxbc null = no PS
  // Constant buffer contents.
  const void* system_constants; uint32_t system_constants_size;
  const float* vs_float; uint32_t vs_float_count;    // vec4 count
  const float* ps_float; uint32_t ps_float_count;
  const uint32_t* bool_loop;                          // 40 dwords
  const uint32_t* fetch;                              // 192 dwords
  // Guest physical byte ranges the VS fetches vertices from (must be in shared memory).
  uint32_t vfetch_count;
  uint32_t vfetch_addr[LSW_GPU_MAX_VFETCH], vfetch_size[LSW_GPU_MAX_VFETCH];
  // Bindings, in descriptor-table order.
  uint32_t vs_tex_count, ps_tex_count, vs_smp_count, ps_smp_count;
  LswGpuTexBinding vs_tex[LSW_GPU_MAX_TEX], ps_tex[LSW_GPU_MAX_TEX];
  LswGpuSamplerBinding vs_smp[LSW_GPU_MAX_TEX], ps_smp[LSW_GPU_MAX_TEX];
  // Fixed-function state.
  float viewport[6];       // x, y, w, h, min_depth, max_depth (host pixels)
  uint32_t scissor[4];     // x, y, w, h
  uint32_t primitive_polygonal;
  uint32_t ps_writes_depth;
  uint32_t ps_color_targets;   // bit i: the pixel shader writes oC<i>
  // Geometry shader key bits (interpolator count, clip planes, kill, point size/coords) for
  // primitive types D3D12 lacks; combine with the type in lsw_gpu_geometry_shader.
  uint32_t gs_key_bits;
};

// regs: full register file (host order, >= 0x5003 dwords). ucode pointers are guest memory
// (big-endian dwords). index_endian: the draw's index endianness (xenos::Endian) for the VS.
// Returns 1 on success.
int lsw_gpu_prepare(const uint32_t* regs, const uint32_t* vs_ucode_be, uint32_t vs_dwords,
                    const uint32_t* ps_ucode_be, uint32_t ps_dwords, uint32_t index_endian,
                    uint32_t rt_width, uint32_t rt_height, LswGpuDraw* out);

// ── Textures ──
struct LswGpuTexDesc {
  uint32_t ok;
  uint32_t dxgi;                 // DXGI_FORMAT
  uint32_t width, height, layers;
  uint32_t dim;                  // 0 = 2D / 2D array (stacked), 2 = cube (layers = 6)
  uint32_t block, bpb;           // block size in texels (1 or 4), bytes per block
  uint32_t row_bytes, rows;      // mip 0, per layer, tightly packed
  // Mip chain: data is [layer][mip] — layer l, mip m at l * layer_bytes + mip_offset[m].
  uint32_t mips;
  uint32_t mip_row_bytes[16], mip_rows[16], mip_offset[16];
  uint32_t layer_bytes;
  const uint8_t* data; uint32_t data_size;   // bridge-owned scratch, valid until next decode
  uint32_t component_mapping;    // D3D12 Shader4ComponentMapping (fetch swizzle applied)
};
struct LswGpuSamplerDesc {
  uint32_t mag_linear, min_linear, mip_linear, max_aniso;
  uint32_t address_u, address_v, address_w;   // D3D12_TEXTURE_ADDRESS_MODE
  float border[4];                            // RGBA border colour (xenos::BorderColor)
};
// Fetch constant fields for render-to-texture lookups: physical base (bytes), TextureFormat,
// swizzle (3 bits per output), size, dimension (xenos::DataDimension). Returns 0 if not a texture.
int lsw_gpu_texture_info(const uint32_t* fetch6, uint32_t* base, uint32_t* format, uint32_t* swizzle,
                         uint32_t* width, uint32_t* height, uint32_t* dimension);
// Cheap identity key for caching (fetch constant + a content probe).
uint64_t lsw_gpu_texture_key(const uint32_t* fetch6, const uint8_t* phys_membase);
// Decodes the base level (untile + endian swap). Returns 1 on success.
int lsw_gpu_decode_texture(const uint32_t* fetch6, const uint8_t* phys_membase, LswGpuTexDesc* out);
void lsw_gpu_sampler(const uint32_t* fetch6, const LswGpuSamplerBinding* binding, LswGpuSamplerDesc* out);

// Xenia-generated DXBC geometry shader: gs_type 1 = point sprites, 2 = rectangle list
// (input: triangle list), 3 = quad list (input: line list with adjacency). Cached, stable pointer.
const void* lsw_gpu_geometry_shader(uint32_t gs_type, uint32_t key_bits, size_t* size_out);

// Size of DxbcShaderTranslator::SystemConstants (for CBV allocation).
uint32_t lsw_gpu_system_constants_size(void);
}
