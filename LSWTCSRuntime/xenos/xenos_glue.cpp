// Glue between the runtime and Xenia's ShaderInterpreter (vendored from xenia-canary,
// BSD license). Compiled in the isolated XenosInterp library so Xenia's headers never
// mix with the recompiler's ppc_* headers. Modeled on Xenia's DrawExtentEstimator.
#include <algorithm>
#include <cstring>
#include <vector>

#include "xenia/gpu/register_file.h"
#include "xenia/gpu/shader_interpreter.h"
#include "xenia/gpu/xenos.h"
#include "xenia/memory.h"
#include "../include/xenos_vs.h"

namespace xe {
namespace gpu {
// register_file.cc is not vendored; the interpreter only needs the value array.
RegisterFile::RegisterFile() { std::memset(values, 0, sizeof(values)); }
}  // namespace gpu
}  // namespace xe

namespace {
using namespace xe::gpu;

class CaptureSink : public ShaderInterpreter::ExportSink {
 public:
  LswXvsVertex* v = nullptr;
  void Export(ucode::ExportRegister reg, const float* value, uint32_t mask) override {
    uint32_t r = uint32_t(reg);
    float* dst = nullptr;
    if (reg == ucode::ExportRegister::kVSPosition) { dst = v->pos; v->has_pos = 1; }
    else if (r < 8) { dst = v->interp[r]; v->interp_mask |= 1u << r; }
    else if (r == 63) {   // point size / edge flag / kill vertex (kill in .z)
      if ((mask & 4) && value[2] != 0.0f) v->killed = 1;
      return;
    }
    if (!dst) return;
    for (int c = 0; c < 4; ++c) if (mask & (1u << c)) dst[c] = value[c];
  }
};

struct State {
  RegisterFile regs;
  xe::Memory mem;
  std::vector<uint32_t> ucode;
  std::vector<uint32_t> ps_ucode;   // host-order PS microcode (empty = no PS pass)
};

// Captures the pixel shader's color export 0 (oC0).
class ColorSink : public ShaderInterpreter::ExportSink {
 public:
  LswXvsVertex* v = nullptr;
  void Export(ucode::ExportRegister reg, const float* value, uint32_t mask) override {
    if (uint32_t(reg) != 0) return;
    for (int c = 0; c < 4; ++c) if (mask & (1u << c)) v->color[c] = value[c];
    v->has_color = 1;
  }
};
State& state() { static State s; return s; }

inline uint32_t bswap32(uint32_t v) { return __builtin_bswap32(v); }
}  // namespace


// Minimal persistent host thread pool for the per-vertex VS (no guest state touched, so it
// runs outside the cooperative GIL). LSWTCS_XVS_THREADS overrides the worker count (0 = serial).
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
namespace {
struct Pool {
  std::vector<std::thread> th;
  std::mutex m; std::condition_variable cv, done_cv;
  std::function<void(uint32_t, uint32_t)> fn;
  uint32_t total = 0, chunk = 64; std::atomic<uint32_t> next{0}; uint32_t gen = 0, busy = 0;
  void work() { for (;;) { uint32_t b = next.fetch_add(chunk); if (b >= total) break; fn(b, std::min(total, b + chunk)); } }
  Pool() {
    int nt = int(std::thread::hardware_concurrency()) - 2;
    if (const char* e = getenv("LSWTCS_XVS_THREADS")) nt = atoi(e);
    if (nt < 0) nt = 0; if (nt > 15) nt = 15;
    for (int t = 0; t < nt; ++t) th.emplace_back([this] {
      uint32_t seen = 0;
      for (;;) {
        { std::unique_lock<std::mutex> lk(m); cv.wait(lk, [&] { return gen != seen; }); seen = gen; }
        work();
        { std::lock_guard<std::mutex> lk(m); if (--busy == 0) done_cv.notify_one(); }
      }
    });
  }
};
Pool& pool() { static Pool* p = new Pool(); return *p; }
void lsw_parallel_for(uint32_t count, uint32_t chunk, const std::function<void(uint32_t, uint32_t)>& f) {
  Pool& p = pool();
  if (p.th.empty() || count < 2 * chunk) { f(0, count); return; }
  { std::lock_guard<std::mutex> lk(p.m); p.fn = f; p.total = count; p.chunk = chunk; p.next = 0;
    p.busy = uint32_t(p.th.size()); ++p.gen; }
  p.cv.notify_all();
  p.work();   // caller helps
  std::unique_lock<std::mutex> lk(p.m); p.done_cv.wait(lk, [&] { return p.busy == 0; });
}
}  // namespace

extern "C" void lswtcs_xvs_set_ps(const uint32_t* ps_ucode_be, uint32_t ps_dwords) {
  State& s = state();
  s.ps_ucode.clear();
  if (!ps_ucode_be || !ps_dwords || ps_dwords > 0x4000) return;
  s.ps_ucode.resize(ps_dwords);
  for (uint32_t i = 0; i < ps_dwords; ++i) s.ps_ucode[i] = bswap32(ps_ucode_be[i]);
}

extern "C" uint32_t lswtcs_xvs_run_draw(const uint32_t* regs, uint8_t* phys_membase,
                                        const uint32_t* vs_ucode_be, uint32_t vs_dwords,
                                        uint32_t draw_initiator, uint32_t dma_base,
                                        uint32_t dma_size, LswXvsVertex* out, uint32_t max_out) {
  if (!regs || !phys_membase || !vs_ucode_be || !vs_dwords || !out || !max_out) return 0;
  State& s = state();
  std::memcpy(s.regs.values, regs, sizeof(s.regs.values));
  s.mem.physical_membase_ = phys_membase;
  // Xenia keeps shader microcode in host byte order (it byte-swaps on IM_LOAD).
  s.ucode.resize(vs_dwords);
  for (uint32_t i = 0; i < vs_dwords; ++i) s.ucode[i] = bswap32(vs_ucode_be[i]);

  // VGT_DRAW_INITIATOR: prim[5:0], source_select[7:6] (0=DMA, 2=auto), index_size[11],
  // num_indices[31:16]. VGT_DMA_SIZE: num_words[23:0], swap_mode[31:30].
  uint32_t num_indices = draw_initiator >> 16;
  uint32_t source_select = (draw_initiator >> 6) & 3;
  bool index32 = (draw_initiator >> 11) & 1;
  uint32_t num_words = dma_size & 0xFFFFFF;
  auto index_endian = xenos::Endian(dma_size >> 30);
  if (!index32) {   // same normalisation as Xenia's PrimitiveProcessor
    if (index_endian == xenos::Endian::k8in32) index_endian = xenos::Endian::k8in16;
    else if (index_endian == xenos::Endian::k16in32) index_endian = xenos::Endian::kNone;
  }
  const uint8_t* ib = (source_select == 0)
      ? phys_membase + ((dma_base & 0x1FFFFFFF) & ~uint32_t(index32 ? 3 : 1)) : nullptr;

  uint32_t index_offset = regs[XE_GPU_REG_VGT_INDX_OFFSET] & 0xFFFFFF;
  uint32_t min_index = regs[XE_GPU_REG_VGT_MIN_VTX_INDX] & 0xFFFFFF;
  uint32_t max_index = regs[XE_GPU_REG_VGT_MAX_VTX_INDX] & 0xFFFFFF;
  if (max_index == 0 || max_index < min_index) { min_index = 0; max_index = 0xFFFFFF; }

  // Pass 1: resolve every index; the VS output depends only on the vertex index (constants
  // and fetch state are fixed for the draw), so collect UNIQUE indices (post-transform cache).
  static std::vector<uint32_t> cache_slot;   // vi -> job + 1 (0 = not seen this draw)
  static std::vector<uint32_t> touched, job_vi, job_slot, src;
  uint32_t n = std::min(num_indices, max_out);
  job_vi.clear(); job_slot.clear(); src.assign(n, 0xFFFFFFFFu);
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t vi;
    if (ib) {
      if (i < num_words) {
        uint32_t raw = index32 ? reinterpret_cast<const uint32_t*>(ib)[i]
                               : reinterpret_cast<const uint16_t*>(ib)[i];
        vi = index32 ? xenos::GpuSwap(raw, index_endian)
                     : xenos::GpuSwap(uint16_t(raw), index_endian);
        vi &= 0xFFFFFF;
      } else {
        vi = 0;
      }
    } else {
      vi = i;
    }
    vi = std::min(max_index, std::max(min_index, (vi + index_offset) & 0xFFFFFF));
    if (vi < (1u << 20)) {
      if (cache_slot.size() <= vi) cache_slot.resize(std::max<size_t>(vi + 1, cache_slot.size() * 2), 0);
      if (uint32_t j1 = cache_slot[vi]) { src[i] = job_slot[j1 - 1]; continue; }
      cache_slot[vi] = uint32_t(job_vi.size()) + 1; touched.push_back(vi);
    }
    job_vi.push_back(vi); job_slot.push_back(i);
  }
  for (uint32_t t : touched) cache_slot[t] = 0;
  touched.clear();

  // Pass 2: run the VS once per unique vertex, spread over host worker threads (each with its
  // own interpreter; registers/memory/ucode are read-only during the draw).
  const uint32_t* ucode = s.ucode.data();
  auto run_range = [&](uint32_t b, uint32_t e) {
    ShaderInterpreter interp(s.regs, s.mem);
    interp.SetShader(xenos::ShaderType::kVertex, ucode);
    CaptureSink sink;
    interp.SetExportSink(&sink);
    for (uint32_t k = b; k < e; ++k) {
      LswXvsVertex& v = out[job_slot[k]];
      std::memset(&v, 0, sizeof(v));
      sink.v = &v;
      interp.temp_registers()[0] = float(job_vi[k]);
      interp.Execute();
    }
    if (s.ps_ucode.empty()) return;
    // Per-vertex pixel shader: interpolators o0..o7 arrive in PS registers r0..r7.
    ShaderInterpreter ps(s.regs, s.mem);
    ps.SetShader(xenos::ShaderType::kPixel, s.ps_ucode.data());
    ColorSink csink;
    ps.SetExportSink(&csink);
    for (uint32_t k = b; k < e; ++k) {
      LswXvsVertex& v = out[job_slot[k]];
      if (!v.has_pos) continue;
      float* r = ps.temp_registers();
      std::memset(r, 0, sizeof(float) * 4 * 8);
      for (int i = 0; i < 8; ++i)
        if (v.interp_mask & (1u << i)) std::memcpy(r + 4 * i, v.interp[i], sizeof(float) * 4);
      csink.v = &v;
      ps.Execute();
    }
  };
  lsw_parallel_for(uint32_t(job_vi.size()), 64, run_range);

  // Pass 3: duplicates copy their first occurrence.
  for (uint32_t i = 0; i < n; ++i) if (src[i] != 0xFFFFFFFFu) out[i] = out[src[i]];
  return n;
}
