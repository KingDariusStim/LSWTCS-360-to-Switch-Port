// Guest texture decoding for the translated-shader D3D12 path (see include/lsw_gpu_bridge.h).
// CPU untiling + endian swap of the base level into a host-layout buffer, using Xenia's layout
// math (texture_util / texture_address). Block-compressed formats stay compressed (BCn).
#include <algorithm>
#include <cstring>
#include <vector>

#include "xenia/base/math.h"
#include "xenia/gpu/texture_address.h"
#include "xenia/gpu/texture_info.h"
#include "xenia/gpu/texture_util.h"
#include "xenia/gpu/xenos.h"

#include "../include/lsw_gpu_bridge.h"

extern "C" void dbg_ram(const char* fmt, ...);
using namespace xe;
using namespace xe::gpu;

namespace {
// DXGI values (avoid pulling dxgiformat.h into the Xenia TU).
enum : uint32_t {
  DXGI_R8G8B8A8_UNORM = 28, DXGI_R8G8_UNORM = 49, DXGI_R8_UNORM = 61,
  DXGI_BC1_UNORM = 71, DXGI_BC2_UNORM = 74, DXGI_BC3_UNORM = 77, DXGI_BC5_UNORM = 83,
  DXGI_B5G6R5_UNORM = 85, DXGI_B5G5R5A1_UNORM = 86, DXGI_B4G4R4A4_UNORM = 115,
};

struct HostFormat { uint32_t dxgi; uint32_t block; uint32_t bpb; uint8_t comp_map[4]; };

bool host_format(xenos::TextureFormat f, HostFormat& h) {
  using F = xenos::TextureFormat;
  switch (f) {
    case F::k_8_8_8_8: case F::k_8_8_8_8_A: case F::k_8_8_8_8_AS_16_16_16_16:
      h = {DXGI_R8G8B8A8_UNORM, 1, 4, {0, 1, 2, 3}}; return true;
    case F::k_DXT1: case F::k_DXT1_AS_16_16_16_16:
      h = {DXGI_BC1_UNORM, 4, 8, {0, 1, 2, 3}}; return true;
    case F::k_DXT2_3: case F::k_DXT2_3_AS_16_16_16_16:
      h = {DXGI_BC2_UNORM, 4, 16, {0, 1, 2, 3}}; return true;
    case F::k_DXT4_5: case F::k_DXT4_5_AS_16_16_16_16:
      h = {DXGI_BC3_UNORM, 4, 16, {0, 1, 2, 3}}; return true;
    case F::k_DXN:
      h = {DXGI_BC5_UNORM, 4, 16, {0, 1, 2, 3}}; return true;
    case F::k_8: h = {DXGI_R8_UNORM, 1, 1, {0, 0, 0, 0}}; return true;
    case F::k_8_8: h = {DXGI_R8G8_UNORM, 1, 2, {0, 1, 1, 1}}; return true;
    // Xenos component 0 is in the low bits; DXGI B-first formats keep blue in the low bits.
    case F::k_5_6_5: h = {DXGI_B5G6R5_UNORM, 1, 2, {2, 1, 0, 3}}; return true;
    case F::k_1_5_5_5: h = {DXGI_B5G5R5A1_UNORM, 1, 2, {2, 1, 0, 3}}; return true;
    case F::k_4_4_4_4: h = {DXGI_B4G4R4A4_UNORM, 1, 2, {2, 1, 0, 3}}; return true;
    default: return false;
  }
}

inline void endian_copy(uint8_t* dst, const uint8_t* src, uint32_t n, xenos::Endian e) {
  switch (e) {
    case xenos::Endian::k8in16:
      for (uint32_t i = 0; i + 1 < n; i += 2) { dst[i] = src[i + 1]; dst[i + 1] = src[i]; }
      break;
    case xenos::Endian::k8in32:
      for (uint32_t i = 0; i + 3 < n; i += 4) {
        dst[i] = src[i + 3]; dst[i + 1] = src[i + 2]; dst[i + 2] = src[i + 1]; dst[i + 3] = src[i];
      }
      break;
    case xenos::Endian::k16in32:
      for (uint32_t i = 0; i + 3 < n; i += 4) {
        dst[i] = src[i + 2]; dst[i + 1] = src[i + 3]; dst[i + 2] = src[i]; dst[i + 3] = src[i + 1];
      }
      break;
    default: std::memcpy(dst, src, n); break;
  }
}

std::vector<uint8_t>& scratch() { static std::vector<uint8_t> v; return v; }
}  // namespace

extern "C" int lsw_gpu_texture_info(const uint32_t* fetch6, uint32_t* base, uint32_t* format,
                                    uint32_t* swizzle, uint32_t* width, uint32_t* height,
                                    uint32_t* dimension) {
  xenos::xe_gpu_texture_fetch_t fetch;
  std::memcpy(&fetch, fetch6, sizeof(fetch));
  if (fetch.type != xenos::FetchConstantType::kTexture) return 0;
  uint32_t w1, h1, d1, base_page, mip_page, mip_min, mip_max;
  texture_util::GetSubresourcesFromFetchConstant(fetch, &w1, &h1, &d1, &base_page, &mip_page,
                                                 &mip_min, &mip_max);
  *base = (base_page << 12) & 0x1FFFFFFFu;
  *format = uint32_t(fetch.format);
  *swizzle = fetch.swizzle;
  *width = w1 + 1; *height = h1 + 1;
  *dimension = uint32_t(fetch.dimension);
  return 1;
}

extern "C" uint64_t lsw_gpu_texture_key(const uint32_t* fetch6, const uint8_t* phys_membase) {
  uint64_t h = 1469598103934665603ull;
  for (int i = 0; i < 6; ++i) h = (h ^ fetch6[i]) * 1099511628211ull;
  // Cheap content probe: first 512 bytes of the base level (catches reuse of an address).
  xenos::xe_gpu_texture_fetch_t fetch;
  std::memcpy(&fetch, fetch6, sizeof(fetch));
  uint32_t base = fetch.base_address << 12;
  if (base && phys_membase) {
    const uint8_t* p = phys_membase + (base & 0x1FFFFFFFu);
    for (uint32_t i = 0; i < 512; i += 8) { uint64_t v; std::memcpy(&v, p + i, 8); h = (h ^ v) * 1099511628211ull; }
    // Sparse probe over the whole base level (LSWTCS_TEXPROBE=0 disables): a texture streamed in
    // progressively can have its first 512 bytes final while the rest is still loading; with only the
    // head probed, that half-loaded decode stayed cached forever (title "Press START" font, band texture).
    static int sparse = -1; if (sparse < 0) { const char* e = getenv("LSWTCS_TEXPROBE"); sparse = (e && e[0] == '0') ? 0 : 1; }
    const auto* fi = FormatInfo::Get(uint32_t(fetch.format));
    if (sparse && fi && fi->bytes_per_block()) {
      uint32_t w = fetch.size_2d.width + 1, hgt = fetch.size_2d.height + 1;
      uint32_t bw = std::max(1u, fi->block_width), bh = std::max(1u, fi->block_height);
      // Tiled textures pad to 32x32 blocks; untiled rows to the fetch pitch. Use the padded extent.
      uint32_t bx = (xe::align(w, 32u * bw) / bw), by = (xe::align(hgt, 32u * bh) / bh);
      uint64_t bytes = uint64_t(bx) * by * fi->bytes_per_block();
      if (bytes > 0x1000000ull) bytes = 0x1000000ull;
      if ((base & 0x1FFFFFFFu) + bytes > 0x20000000ull) bytes = 0x20000000ull - (base & 0x1FFFFFFFu);
      if (bytes >= 1024) {
        for (uint32_t k = 1; k <= 64; ++k) {
          uint64_t off = (bytes - 8) * k / 64 & ~uint64_t(7);
          uint64_t v; std::memcpy(&v, p + off, 8); h = (h ^ v) * 1099511628211ull;
        }
      }
    }
  }
  return h;
}

extern "C" int lsw_gpu_decode_texture(const uint32_t* fetch6, const uint8_t* phys_membase,
                                      LswGpuTexDesc* out) {
  std::memset(out, 0, sizeof(*out));
  xenos::xe_gpu_texture_fetch_t fetch;
  std::memcpy(&fetch, fetch6, sizeof(fetch));
  if (fetch.type != xenos::FetchConstantType::kTexture) return 0;
  HostFormat hf;
  if (!host_format(fetch.format, hf)) {
    static uint64_t seen = 0; uint32_t fmt = uint32_t(fetch.format);
    if (fmt < 64 && !(seen & (1ull << fmt))) { seen |= 1ull << fmt; dbg_ram("[TEXFMT] unsupported texture format %u (dim %u)\n", fmt, uint32_t(fetch.dimension)); }
    return 0;
  }
  xenos::DataDimension dim = fetch.dimension;
  if (dim == xenos::DataDimension::k3D || dim == xenos::DataDimension::k1D) {
    static int n = 0; if (n++ < 4) dbg_ram("[TEXFMT] unsupported dimension %u (fmt %u)\n", uint32_t(dim), uint32_t(fetch.format));
    return 0;
  }

  uint32_t w1, h1, d1, base_page, mip_page, mip_min, mip_max;
  texture_util::GetSubresourcesFromFetchConstant(fetch, &w1, &h1, &d1, &base_page, &mip_page,
                                                 &mip_min, &mip_max);
  if (!base_page) return 0;
  uint32_t width = w1 + 1, height = h1 + 1, layers = d1 + 1;
  if (width > 8192 || height > 8192 || layers > 64) return 0;
  const auto* fi = FormatInfo::Get(uint32_t(fetch.format));
  uint32_t bw = fi->block_width, bh = fi->block_height, bpb = fi->bytes_per_block();
  if (bpb != hf.bpb || bw != hf.block || bh != hf.block) return 0;
  uint32_t bpb_log2 = xe::log2_floor(bpb);
  // Mip chain: levels 0..mip_max (base at base_page, the rest at mip_page). LSWTCS_GPUDRAW_NOMIPS=1
  // keeps only the base level.
  static int nomips = -1;
  if (nomips < 0) { const char* e = getenv("LSWTCS_GPUDRAW_NOMIPS"); nomips = (e && e[0] == '1') ? 1 : 0; }
  uint32_t max_level = (mip_page && !nomips) ? std::min<uint32_t>(mip_max, 15) : 0;
  auto layout = texture_util::GetGuestTextureLayout(
      dim, fetch.pitch, width, height, layers, fetch.tiled, fetch.format,
      fetch.packed_mips, true, max_level);
  uint32_t mips = max_level + 1;
  // Host layout: per layer, levels back to back.
  uint32_t layer_bytes = 0;
  for (uint32_t m = 0; m < mips; ++m) {
    uint32_t wm = std::max(width >> m, 1u), hm = std::max(height >> m, 1u);
    out->mip_row_bytes[m] = ((wm + bw - 1) / bw) * bpb;
    out->mip_rows[m] = (hm + bh - 1) / bh;
    out->mip_offset[m] = layer_bytes;
    layer_bytes += out->mip_row_bytes[m] * out->mip_rows[m];
  }
  std::vector<uint8_t>& dst = scratch();
  dst.assign(size_t(layer_bytes) * layers, 0);
  for (uint32_t m = 0; m < mips; ++m) {
    const auto& lv = (m == 0) ? layout.base : layout.mips[m];
    uint32_t level_base = (m == 0) ? (base_page << 12)
                                   : (mip_page << 12) + layout.mip_offsets_bytes[std::min(m, layout.packed_level)];
    uint32_t wm = std::max(width >> m, 1u), hm = std::max(height >> m, 1u);
    uint32_t wb = (wm + bw - 1) / bw, hb = (hm + bh - 1) / bh;
    // Levels in the packed tail sit at an offset inside the packed level's storage.
    uint32_t off_x = 0, off_y = 0, off_z = 0;
    if (fetch.packed_mips && m >= layout.packed_level)
      texture_util::GetPackedMipOffset(width, height, 1, fetch.format, m, off_x, off_y, off_z);
    const auto& storage = (fetch.packed_mips && m > layout.packed_level && layout.packed_level != 0)
                              ? layout.mips[layout.packed_level] : lv;
    uint32_t row_pitch = (m >= layout.packed_level && layout.packed_level == 0) ? layout.base.row_pitch_bytes : storage.row_pitch_bytes;
    uint32_t pitch_aligned = row_pitch / bpb;
    if (fetch.tiled) pitch_aligned = xe::align(pitch_aligned, xenos::kTextureTileWidthHeight);
    uint32_t slice_bytes = (m >= layout.packed_level && layout.packed_level == 0) ? layout.base.array_slice_stride_bytes
                                                                                : storage.array_slice_stride_bytes;
    const uint8_t* src_base = phys_membase + (level_base & 0x1FFFFFFFu);
    uint64_t limit = 0x20000000ull - (level_base & 0x1FFFFFFFu);
    for (uint32_t z = 0; z < layers; ++z) {
      for (uint32_t by = 0; by < hb; ++by) {
        uint8_t* drow = dst.data() + size_t(z) * layer_bytes + out->mip_offset[m] + size_t(by) * out->mip_row_bytes[m];
        for (uint32_t bx = 0; bx < wb; ++bx) {
          int64_t so;
          if (fetch.tiled)
            so = texture_address::Tiled2D(int32_t(bx + off_x), int32_t(by + off_y), pitch_aligned, bpb_log2);
          else
            so = int64_t(by + off_y) * row_pitch + int64_t(bx + off_x) * bpb;
          so += int64_t(z) * slice_bytes;
          if (so < 0 || uint64_t(so) + bpb > limit) continue;
          endian_copy(drow + bx * bpb, src_base + so, bpb, fetch.endianness);
        }
      }
    }
  }
  out->ok = 1;
  out->dxgi = hf.dxgi;
  out->width = width; out->height = height; out->layers = layers;
  out->dim = (dim == xenos::DataDimension::kCube) ? 2 : 0;
  out->block = hf.block; out->bpb = bpb;
  out->row_bytes = out->mip_row_bytes[0]; out->rows = out->mip_rows[0];
  out->mips = mips; out->layer_bytes = layer_bytes;
  out->data = dst.data(); out->data_size = uint32_t(dst.size());
  // SRV component mapping: fetch swizzle (3 bits per output: 0-3 = component, 4 = 0, 5 = 1),
  // remapped through the host format's component order; D3D12 encoding matches 0-5 directly.
  uint32_t map = 0;
  for (int c = 0; c < 4; ++c) {
    uint32_t s = (fetch.swizzle >> (3 * c)) & 7;
    if (s < 4) s = hf.comp_map[s]; else if (s > 5) s = 4;
    map |= s << (3 * c);
  }
  out->component_mapping = map | (1u << 12);   // D3D12_SHADER_COMPONENT_MAPPING_ALWAYS_SET_BIT_AVOIDING_ZEROMEM_MISTAKES
  return 1;
}

extern "C" void lsw_gpu_sampler(const uint32_t* fetch6, const LswGpuSamplerBinding* b,
                                LswGpuSamplerDesc* out) {
  xenos::xe_gpu_texture_fetch_t fetch;
  std::memcpy(&fetch, fetch6, sizeof(fetch));
  auto pick = [](uint32_t binding, uint32_t from_fetch) { return binding == 3 ? from_fetch : binding; };
  uint32_t mag = b ? pick(b->mag_filter, uint32_t(fetch.mag_filter)) : uint32_t(fetch.mag_filter);
  uint32_t min = b ? pick(b->min_filter, uint32_t(fetch.min_filter)) : uint32_t(fetch.min_filter);
  uint32_t mip = b ? pick(b->mip_filter, uint32_t(fetch.mip_filter)) : uint32_t(fetch.mip_filter);
  uint32_t aniso = b ? (b->aniso_filter == 7 ? uint32_t(fetch.aniso_filter) : b->aniso_filter)
                     : uint32_t(fetch.aniso_filter);
  out->mag_linear = mag == 1; out->min_linear = min == 1; out->mip_linear = mip == 1;
  out->max_aniso = aniso >= 1 && aniso <= 5 ? (1u << (aniso - 1)) : 0;
  auto addr = [](uint32_t c) -> uint32_t {   // D3D12_TEXTURE_ADDRESS_MODE
    switch (c) { case 0: return 1; case 1: return 2; case 2: case 4: return 3; case 6: return 4; default: return 5; }
  };
  out->address_u = addr(uint32_t(fetch.clamp_x));
  out->address_v = addr(uint32_t(fetch.clamp_y));
  out->address_w = addr(uint32_t(fetch.clamp_z));
  // Border colour (as Xenia D3D12TextureCache::WriteSampler). Shadow maps use clamp-to-border with
  // a white border (= far depth, lit) outside the light frustum.
  float bc[4] = {0, 0, 0, 0};
  switch (uint32_t(fetch.border_color)) {
    case 1: bc[0] = bc[1] = bc[2] = bc[3] = 1.0f; break;   // k_ABGR_White
    case 2: bc[0] = 0.5f; bc[2] = 0.5f; break;             // k_ACBYCR_Black
    case 3: bc[1] = 0.5f; bc[2] = 0.5f; break;             // k_ACBCRY_Black
    default: break;                                        // k_ABGR_Black
  }
  std::memcpy(out->border, bc, sizeof(bc));
}
