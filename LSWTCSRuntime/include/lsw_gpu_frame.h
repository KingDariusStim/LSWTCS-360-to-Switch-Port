// Per-frame list of guest draws recorded by the PM4 executor (kernel_stubs.cpp) for the D3D12
// translated-shader path (gpu_d3d12.cpp). All data a draw needs is snapshotted into a byte arena
// at record time, because guest memory (vertex/index buffers, constants) changes before present.
#pragma once
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <utility>
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

// Vector allocator that leaves new elements uninitialised (resize() would zero-fill every byte of
// the multi-MB arena right before it is overwritten).
template <class T> struct LswDefaultInitAlloc : std::allocator<T> {
  template <class U> struct rebind { using other = LswDefaultInitAlloc<U>; };
  using std::allocator<T>::allocator;
  template <class U> void construct(U* p) noexcept { ::new (static_cast<void*>(p)) U; }
  template <class U, class... A> void construct(U* p, A&&... a) { ::new (static_cast<void*>(p)) U(std::forward<A>(a)...); }
};

// LswGpuRange::arena_off for a range whose content is already resident in GPU shared memory.
enum : uint32_t { kLswRangeResident = 0xFFFFFFFFu };

struct LswGpuFrame {
  std::vector<uint8_t, LswDefaultInitAlloc<uint8_t>> arena;
  std::vector<LswGpuRange> ranges;
  std::vector<LswGpuDrawRec> draws;
  // Shared-memory uploads of earlier published frames that were never replayed: the record-time
  // residency tracking assumed them done, so they are issued before this frame's draws.
  std::vector<LswGpuRange> prelude;
  uint32_t swap_fb = 0;   // frontbuffer address in the PM4_XE_SWAP packet that ended this frame
  void clear() { arena.clear(); ranges.clear(); draws.clear(); prelude.clear(); }
  // Zero-filled (constant buffers: padding and short blocks must read as 0).
  uint32_t alloc(uint32_t size, uint32_t align = 256) {
    size_t old = arena.size();
    uint32_t off = alloc_uninit(size, align);
    std::memset(arena.data() + old, 0, arena.size() - old);
    return off;
  }
  // Uninitialised: for vertex / index data that is copied in right away.
  uint32_t alloc_uninit(uint32_t size, uint32_t align = 256) {
    uint32_t off = uint32_t((arena.size() + (align - 1)) & ~size_t(align - 1));
    arena.resize(off + size);
    return off;
  }
};

// 64-bit content hash for vertex ranges: four independent lanes (xxHash64 round) so it runs near
// memory bandwidth instead of one serial multiply chain per 8 bytes.
static inline uint64_t lsw_hash_bytes(const uint8_t* p, uint32_t n) {
  const uint64_t P1 = 0x9E3779B185EBCA87ull, P2 = 0xC2B2AE3D27D4EB4Full;
  auto rotl = [](uint64_t x, int r) { return (x << r) | (x >> (64 - r)); };
  auto round = [&](uint64_t acc, uint64_t in) { return rotl(acc + in * P2, 31) * P1; };
  uint64_t a = P1 + P2, b = P2, c = 0, d = 0 - P1;
  uint32_t i = 0;
  for (; i + 32 <= n; i += 32) {
    uint64_t w[4]; std::memcpy(w, p + i, 32);
    a = round(a, w[0]); b = round(b, w[1]); c = round(c, w[2]); d = round(d, w[3]);
  }
  uint64_t h = rotl(a, 1) + rotl(b, 7) + rotl(c, 12) + rotl(d, 18);
  for (; i + 8 <= n; i += 8) { uint64_t v; std::memcpy(&v, p + i, 8); h = rotl(h ^ round(0, v), 27) * P1; }
  for (; i < n; ++i) h = (h ^ p[i]) * P1;
  h ^= h >> 33; h *= P2; h ^= h >> 29;
  return h ^ n;
}

// GPUPROF stage accumulators (nanoseconds / counts), defined in kernel_stubs.cpp.
enum { GP_WALK, GP_RECORD, GP_PREPARE, GP_VCOPY, GP_REPLAY, GP_SHMHASH, GP_PRESENT, GP_FENCEWAIT, GP_SUBMIT,
       GP_FBFILL, GP_DRAWS, GP_VBYTES, GP_IRQ,
       GP_RP_ARENA, GP_RP_PREPASS, GP_RP_PSO, GP_RP_BIND, GP_RP_DIAG, GP_LIMIT, GP_RTWAIT,
       GP_RP_TEXSRC, GP_RP_SRV, GP_RP_SMP, GP_RP_RT, GP_RP_DRAW, GP_COUNT };
extern "C" uint64_t g_gpuprof[GP_COUNT];
static inline uint64_t gp_now() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
struct GpScope {   // adds the enclosing scope's duration to g_gpuprof[k]
  int k; uint64_t t0;
  explicit GpScope(int stage) : k(stage), t0(gp_now()) {}
  ~GpScope() { g_gpuprof[k] += gp_now() - t0; }
};

// Three slots: build (recorded by the PM4 walker on a guest thread), present (published at the guest
// swap marker, waiting for the renderer) and render (taken by the render thread, replayed there).
// g_gpu_frame_mtx guards present + g_gpu_frame_ready; build is walker-only, render is renderer-only.
// A published frame replaced before the renderer took it hands its shared-memory uploads on.
#include <mutex>
extern LswGpuFrame g_gpu_frame_build, g_gpu_frame_present, g_gpu_frame_render;
extern bool g_gpu_frame_ready;   // present holds a frame the renderer has not taken yet
extern std::mutex g_gpu_frame_mtx;
// Renderer side: move the latest published frame into g_gpu_frame_render (no-op if none is new).
static inline void gpu_frame_take() {
  std::lock_guard<std::mutex> lk(g_gpu_frame_mtx);
  if (!g_gpu_frame_ready) return;
  std::swap(g_gpu_frame_present, g_gpu_frame_render);
  g_gpu_frame_ready = false;
}
