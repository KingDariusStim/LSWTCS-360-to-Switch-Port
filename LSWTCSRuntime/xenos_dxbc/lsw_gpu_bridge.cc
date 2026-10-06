// Runtime <-> Xenia DXBC shader translator bridge. See include/lsw_gpu_bridge.h.
// Ports the parts of Xenia's D3D12CommandProcessor::IssueDraw / UpdateSystemConstantValues /
// UpdateBindings and PipelineCache::GetCurrent*ShaderModification that apply to the
// host-render-target path (no ROV, no tessellation, no resolution scaling).
#include <algorithm>
#include <cstring>
#include <memory>
#include <set>
#include <type_traits>
#include <cstdio>
#include <direct.h>
extern "C" void dbg_ram(const char* fmt, ...);
#include <unordered_map>
#include <vector>

#include "xenia/base/math.h"
#include "xenia/base/string_buffer.h"
#include "xenia/gpu/draw_util.h"
#include "xenia/gpu/dxbc_shader.h"
#include "xenia/gpu/dxbc_shader_translator.h"
#include "xenia/gpu/register_file.h"
#include "xenia/gpu/registers.h"
#include "xenia/gpu/texture_util.h"
#include "xenia/gpu/xenos.h"

#include "../include/lsw_gpu_bridge.h"

using namespace xe;
using namespace xe::gpu;

namespace {

struct BridgeState {
  std::unique_ptr<DxbcShaderTranslator> translator;
  std::unordered_map<uint64_t, std::unique_ptr<DxbcShader>> shaders;
  StringBuffer disasm_buffer;
  DxbcShaderTranslator::SystemConstants sys;
  std::vector<float> vs_float, ps_float;
  uint32_t bool_loop[40];
  uint32_t fetch[192];
};

BridgeState& bridge() {
  static BridgeState* s = [] {
    auto* b = new BridgeState();
    b->translator = std::make_unique<DxbcShaderTranslator>(
        ui::GraphicsProvider::GpuVendorID::kAMD, /*bindless*/ false, /*edram_rov*/ false,
        /*gamma_render_target_as_unorm8*/ true, /*msaa_2x_supported*/ true);
    b->vs_float.reserve(256 * 4);
    b->ps_float.reserve(256 * 4);
    return b;
  }();
  return *s;
}

uint64_t hash_ucode(const uint32_t* p, uint32_t n) {
  uint64_t h = 1469598103934665603ull;
  for (uint32_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
  return h ^ n;
}

// Last shader seen per (ucode pointer, size, type) with a copy of its guest bytes: a draw reusing the
// shader at the same address is verified with one memcmp instead of re-hashing the whole ucode with a
// serial FNV chain (VS + PS on every draw was most of lsw_gpu_prepare). LSWTCS_UCODEMEMO=0 disables.
struct UcodeMemo { std::vector<uint32_t> be; DxbcShader* shader; };
std::unordered_map<uint64_t, UcodeMemo>& ucode_memo() { static auto* m = new std::unordered_map<uint64_t, UcodeMemo>(); return *m; }

DxbcShader* get_shader(xenos::ShaderType type, const uint32_t* ucode_be, uint32_t dwords) {
  BridgeState& b = bridge();
  static int memo_on = -1; if (memo_on < 0) { const char* e = getenv("LSWTCS_UCODEMEMO"); memo_on = (e && e[0] == '0') ? 0 : 1; }
  const uint64_t mkey = uint64_t(reinterpret_cast<uintptr_t>(ucode_be)) ^ (uint64_t(dwords) << 40) ^
                        (type == xenos::ShaderType::kPixel ? 0x8000000000000000ull : 0);
  UcodeMemo* memo = nullptr;
  if (memo_on) {
    memo = &ucode_memo()[mkey];
    if (memo->shader && memo->be.size() == dwords && std::memcmp(memo->be.data(), ucode_be, dwords * 4u) == 0) return memo->shader;
  }
  uint64_t key = hash_ucode(ucode_be, dwords) ^ (type == xenos::ShaderType::kPixel ? 0x8000000000000000ull : 0);
  DxbcShader* raw;
  auto it = b.shaders.find(key);
  if (it != b.shaders.end()) {
    raw = it->second.get();
  } else {
    // The Shader constructor byte-swaps from guest (big-endian) order.
    auto sh = std::make_unique<DxbcShader>(type, key, ucode_be, dwords);
    sh->AnalyzeUcode(b.disasm_buffer);
    raw = sh.get();
    b.shaders.emplace(key, std::move(sh));
  }
  if (memo) { memo->be.assign(ucode_be, ucode_be + dwords); memo->shader = raw; }
  return raw;
}

DxbcShader::Translation* translate(DxbcShader& shader, uint64_t modification) {
  auto* t = shader.GetOrCreateTranslation(modification);
  if (!t->is_translated()) bridge().translator->TranslateAnalyzedShader(*t);
  return t->is_valid() ? t : nullptr;
}

void pack_floats(const RegisterFile& regs, const Shader::ConstantRegisterMap& map,
                 uint32_t base_reg, std::vector<float>& out) {
  // Sized once, then one 16-byte copy per used register (a vector insert per register showed up in
  // the record profile).
  uint32_t n = 0;
  for (uint32_t i = 0; i < 4; ++i) n += xe::bit_count(map.float_bitmap[i]);
  if (!n) { out.assign(4, 0.0f); return; }   // a valid (non-empty) binding is still required
  out.resize(size_t(n) * 4);
  float* dst = out.data();
  for (uint32_t i = 0; i < 4; ++i) {
    uint64_t bits = map.float_bitmap[i];
    uint32_t idx;
    while (xe::bit_scan_forward(bits, &idx)) {
      bits = xe::clear_lowest_bit(bits);
      std::memcpy(dst, &regs[base_reg + (i << 8) + (idx << 2)], 4 * sizeof(float));
      dst += 4;
    }
  }
}

}  // namespace

extern "C" uint32_t lsw_gpu_system_constants_size(void) {
  return uint32_t(sizeof(DxbcShaderTranslator::SystemConstants));
}

extern "C" int lsw_gpu_prepare(const uint32_t* regs_in, const uint32_t* vs_ucode_be,
                               uint32_t vs_dwords, const uint32_t* ps_ucode_be,
                               uint32_t ps_dwords, uint32_t index_endian, uint32_t rt_width,
                               uint32_t rt_height, LswGpuDraw* out) {
  if (!regs_in || !vs_ucode_be || !vs_dwords || !out) return 0;
  BridgeState& b = bridge();
  // The caller's register array (>= kRegisterCount dwords, read-only here) viewed in place: copying
  // the 80 KB file per draw was ~2 ms per frame at hub draw counts.
  static_assert(std::is_standard_layout_v<RegisterFile> && sizeof(RegisterFile) == sizeof(uint32_t) * RegisterFile::kRegisterCount,
                "RegisterFile must be a plain register array");
  const RegisterFile& regs = *reinterpret_cast<const RegisterFile*>(regs_in);
  std::memset(out, 0, sizeof(*out));

  DxbcShader* vs = get_shader(xenos::ShaderType::kVertex, vs_ucode_be, vs_dwords);
  if (!vs || !vs->is_ucode_analyzed()) return 0;

  bool primitive_polygonal = draw_util::IsPrimitivePolygonal(regs);
  DxbcShader* ps = nullptr;
  if (ps_ucode_be && ps_dwords &&
      draw_util::IsRasterizationPotentiallyDone(regs, primitive_polygonal)) {
    ps = get_shader(xenos::ShaderType::kPixel, ps_ucode_be, ps_dwords);
    if (ps && !draw_util::IsPixelShaderNeededWithRasterization(*ps, regs)) ps = nullptr;
  }

  reg::RB_DEPTHCONTROL normalized_depth_control = draw_util::GetNormalizedDepthControl(regs);

  // ── Shader modifications (PipelineCache::GetCurrent*ShaderModification, host-RT path) ──
  uint32_t ps_param_gen_pos = UINT32_MAX;
  uint32_t interpolator_mask =
      ps ? (vs->writes_interpolators() &
            ps->GetInterpolatorInputMask(regs.Get<reg::SQ_PROGRAM_CNTL>(),
                                         regs.Get<reg::SQ_CONTEXT_MISC>(), ps_param_gen_pos))
         : 0;
  DxbcShaderTranslator::Modification vs_mod(b.translator->GetDefaultVertexShaderModification(
      vs->GetDynamicAddressableRegisterCount(regs.Get<reg::SQ_PROGRAM_CNTL>().vs_num_reg),
      Shader::HostVertexShaderType::kVertex));
  vs_mod.vertex.interpolator_mask = interpolator_mask;
  auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  uint32_t user_clip_planes = pa_cl_clip_cntl.clip_disable ? 0 : pa_cl_clip_cntl.ucp_ena;
  vs_mod.vertex.user_clip_plane_count = xe::bit_count(user_clip_planes);
  vs_mod.vertex.user_clip_plane_cull =
      uint32_t(user_clip_planes && pa_cl_clip_cntl.ucp_cull_only_ena);
  vs_mod.vertex.vertex_kill_and =
      uint32_t((vs->writes_point_size_edge_flag_kill_vertex() & 0b100) &&
               !pa_cl_clip_cntl.vtx_kill_or);
  vs_mod.vertex.output_point_size =
      uint32_t((vs->writes_point_size_edge_flag_kill_vertex() & 0b001) &&
               regs.Get<reg::VGT_DRAW_INITIATOR>().prim_type == xenos::PrimitiveType::kPointList);

  DxbcShaderTranslator::Modification ps_mod(0);
  if (ps) {
    ps_mod = DxbcShaderTranslator::Modification(b.translator->GetDefaultPixelShaderModification(
        ps->GetDynamicAddressableRegisterCount(regs.Get<reg::SQ_PROGRAM_CNTL>().ps_num_reg)));
    ps_mod.pixel.interpolator_mask = interpolator_mask;
    ps_mod.pixel.interpolators_centroid =
        interpolator_mask &
        ~xenos::GetInterpolatorSamplingPattern(
            regs.Get<reg::RB_SURFACE_INFO>().msaa_samples,
            regs.Get<reg::SQ_CONTEXT_MISC>().sc_sample_cntl,
            regs.Get<reg::SQ_INTERPOLATOR_CNTL>().sampling_pattern);
    if (ps_param_gen_pos < xenos::kMaxInterpolators) {
      ps_mod.pixel.param_gen_enable = 1;
      ps_mod.pixel.param_gen_interpolator = ps_param_gen_pos;
      ps_mod.pixel.param_gen_point = uint32_t(regs.Get<reg::VGT_DRAW_INITIATOR>().prim_type ==
                                              xenos::PrimitiveType::kPointList);
    }
    using DepthStencilMode = DxbcShaderTranslator::Modification::DepthStencilMode;
    ps_mod.pixel.depth_stencil_mode =
        (ps->implicit_early_z_write_allowed() &&
         (!ps->writes_color_target(0) ||
          !draw_util::DoesCoverageDependOnAlpha(regs.Get<reg::RB_COLORCONTROL>())))
            ? DepthStencilMode::kEarlyHint
            : DepthStencilMode::kNoModifiers;
    ps_mod.pixel.rt0_blend_rgb_factor_for_premult = xenos::BlendFactor::kOne;
    ps_mod.pixel.rt0_blend_a_factor_for_premult = xenos::BlendFactor::kOne;
  }

  auto* vs_t = translate(*vs, vs_mod.value);
  if (!vs_t) return 0;
  DxbcShader::Translation* ps_t = ps ? translate(*ps, ps_mod.value) : nullptr;
  if (ps && !ps_t) ps = nullptr;

  out->vs_dxbc = vs_t->translated_binary().data();
  out->vs_dxbc_size = vs_t->translated_binary().size();
  out->vs_key = vs->ucode_data_hash() ^ (vs_mod.value * 0x9E3779B97F4A7C15ull);
  out->ps_color_targets = ps ? ps->writes_color_targets() : 0;
  // DIAG (LSWTCS_VSDUMP=1): every distinct VS (per draw key, i.e. per modification) ->
  // shdisasm/vs_<vs_key>.txt with the Xenos ucode disassembly; match against [XDLOG] vs=<key>.
  {
    static int on = -1; if (on < 0) { const char* e = getenv("LSWTCS_VSDUMP"); on = e ? atoi(e) : 0; }
    static std::set<uint64_t> done;
    if (on && done.size() < 1000 && done.insert(out->vs_key).second) {
      _mkdir("shdisasm");
      char path[128];
      snprintf(path, sizeof path, "shdisasm/vs_%016llX.txt", (unsigned long long)out->vs_key);
      if (FILE* fp = fopen(path, "w")) {
        fprintf(fp, "ucode hash %016llX, %u dwords\n%s\n", (unsigned long long)vs->ucode_data_hash(), vs_dwords, vs->ucode_disassembly().c_str());
        fclose(fp);
      }
    }
  }
  // DIAG (LSWTCS_SHDISASM=1): write the Xenos ucode disassembly of VS+PS pairs used by MRT draws
  // (world passes) to shdisasm/<vs>_<ps>.txt, once each.
  {
    static int on = -1; if (on < 0) { const char* e = getenv("LSWTCS_SHDISASM"); on = e ? atoi(e) : 0; }
    if (on && ps && (on == 2 || (ps->writes_color_targets() & ~1u))) {
      static std::set<uint64_t> done;
      uint64_t k = vs->ucode_data_hash() * 31 + ps->ucode_data_hash();
      if (done.size() < 400 && done.insert(k).second) {
        _mkdir("shdisasm");
        char path[128];
        snprintf(path, sizeof path, "shdisasm/%016llX_%016llX.txt", (unsigned long long)vs->ucode_data_hash(), (unsigned long long)ps->ucode_data_hash());
        if (FILE* fp = fopen(path, "w")) {
          fprintf(fp, "==== VS ====\n%s\n==== PS (writes RT mask 0x%X) ====\n%s\n", vs->ucode_disassembly().c_str(), ps->writes_color_targets(), ps->ucode_disassembly().c_str());
          fclose(fp);
        }
      }
    }
  }
  // DIAG (MEMEXPORT): shaders that write memory (eM exports) — log each one once with its streams.
  {
    static std::set<uint64_t> seen;
    auto note = [&](const DxbcShader& s, const char* kind) {
      if (!s.memexport_eM_written() || !seen.insert(s.ucode_data_hash()).second || seen.size() > 64) return;
      char buf[256]; int k = 0;
      for (uint32_t c : s.memexport_stream_constants()) {
        uint32_t a = regs[XE_GPU_REG_SHADER_CONSTANT_000_X + c * 4], b = regs[XE_GPU_REG_SHADER_CONSTANT_000_X + c * 4 + 1];
        k += snprintf(buf + k, sizeof buf - k, " c%u=%08X/%08X", c, a, b);
        if (k > 200) break;
      }
      dbg_ram("[MEMEXPORT] %s hash=%016llX eM=0x%02X streams:%s (prim %u)\n", kind, (unsigned long long)s.ucode_data_hash(),
              s.memexport_eM_written(), k ? buf : " none", regs[XE_GPU_REG_VGT_DRAW_INITIATOR] & 0x3F);
    };
    note(*vs, "VS");
    if (ps) note(*ps, "PS");
  }
  if (ps_t) {
    out->ps_dxbc = ps_t->translated_binary().data();
    out->ps_dxbc_size = ps_t->translated_binary().size();
    out->ps_key = ps->ucode_data_hash() ^ (ps_mod.value * 0x9E3779B97F4A7C15ull);
    // DIAG (LSWTCS_VSDUMP=1 also): every distinct PS -> shdisasm/ps_<ps_key>.txt (Xenos ucode disassembly).
    {
      static int on = -1; if (on < 0) { const char* e = getenv("LSWTCS_VSDUMP"); on = e ? atoi(e) : 0; }
      static std::set<uint64_t> done;
      if (on && done.size() < 1000 && done.insert(out->ps_key).second) {
        _mkdir("shdisasm");
        char path[128];
        snprintf(path, sizeof path, "shdisasm/ps_%016llX.txt", (unsigned long long)out->ps_key);
        if (FILE* fp = fopen(path, "w")) {
          fprintf(fp, "ucode hash %016llX\n%s\n", (unsigned long long)ps->ucode_data_hash(), ps->ucode_disassembly().c_str());
          fclose(fp);
        }
      }
    }
    out->ps_writes_depth = ps->writes_depth() ? 1 : 0;
  }
  out->primitive_polygonal = primitive_polygonal ? 1 : 0;
  {
    uint32_t k = 0;
    k |= (xe::bit_count(vs_mod.vertex.interpolator_mask) & 31u) << 2;
    k |= (uint32_t(vs_mod.vertex.user_clip_plane_count) & 7u) << 7;
    k |= (uint32_t(vs_mod.vertex.user_clip_plane_cull) & 1u) << 10;
    k |= (uint32_t(vs_mod.vertex.vertex_kill_and) & 1u) << 11;
    k |= (uint32_t(vs_mod.vertex.output_point_size) & 1u) << 12;
    k |= (uint32_t(ps ? ps_mod.pixel.param_gen_point : 0) & 1u) << 13;
    out->gs_key_bits = k;
  }

  // ── Viewport / scissor (draw_util, host render targets, scale 1) ──
  draw_util::ViewportInfo viewport_info;
  draw_util::GetViewportInfoArgs gviargs{};
  gviargs.Setup(1, 1, divisors::MagicDiv(1), divisors::MagicDiv(1), true, 16384, 16384, false,
                normalized_depth_control, false, true, ps && ps->writes_depth());
  gviargs.SetupRegisterValues(regs);
  draw_util::GetHostViewportInfo(&gviargs, viewport_info);
  out->viewport[0] = float(viewport_info.xy_offset[0]);
  out->viewport[1] = float(viewport_info.xy_offset[1]);
  out->viewport[2] = float(viewport_info.xy_extent[0]);
  out->viewport[3] = float(viewport_info.xy_extent[1]);
  out->viewport[4] = viewport_info.z_min;
  out->viewport[5] = viewport_info.z_max;
  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor);
  out->scissor[0] = scissor.offset[0]; out->scissor[1] = scissor.offset[1];
  out->scissor[2] = scissor.extent[0]; out->scissor[3] = scissor.extent[1];
  (void)rt_width; (void)rt_height;

  // ── System constants (UpdateSystemConstantValues_Impl, non-ROV subset) ──
  auto& sc = b.sys;
  std::memset(&sc, 0, sizeof(sc));
  auto pa_cl_vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();
  auto rb_colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
  auto rb_depth_info = regs.Get<reg::RB_DEPTH_INFO>();
  auto rb_surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  uint32_t flags = 0;
  if (pa_cl_vte_cntl.vtx_xy_fmt) flags |= DxbcShaderTranslator::kSysFlag_XYDividedByW;
  if (pa_cl_vte_cntl.vtx_z_fmt) flags |= DxbcShaderTranslator::kSysFlag_ZDividedByW;
  if (pa_cl_vte_cntl.vtx_w0_fmt) flags |= DxbcShaderTranslator::kSysFlag_WNotReciprocal;
  if (primitive_polygonal) flags |= DxbcShaderTranslator::kSysFlag_PrimitivePolygonal;
  if (draw_util::IsPrimitiveLine(regs)) flags |= DxbcShaderTranslator::kSysFlag_PrimitiveLine;
  if (rb_depth_info.depth_format == xenos::DepthRenderTargetFormat::kD24FS8)
    flags |= DxbcShaderTranslator::kSysFlag_DepthFloat24;
  xenos::CompareFunction alpha_test_function =
      rb_colorcontrol.alpha_test_enable ? rb_colorcontrol.alpha_func : xenos::CompareFunction::kAlways;
  flags |= uint32_t(alpha_test_function) << DxbcShaderTranslator::kSysFlag_AlphaPassIfLess_Shift;
  for (uint32_t i = 0; i < 4; ++i) {
    auto ci = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
    if (ci.color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA)
      flags |= DxbcShaderTranslator::kSysFlag_ConvertColor0ToGamma << i;
    int32_t bias = ci.color_exp_bias;
    sc.color_exp_bias[i] = xe::memory::Reinterpret<float>(int32_t(0x3F800000 + (bias << 23)));
  }
  sc.flags = flags;
  sc.tessellation_factor_range_min = regs.Get<float>(XE_GPU_REG_VGT_HOS_MIN_TESS_LEVEL) + 1.0f;
  sc.tessellation_factor_range_max = regs.Get<float>(XE_GPU_REG_VGT_HOS_MAX_TESS_LEVEL) + 1.0f;
  sc.line_loop_closing_index = 0;
  sc.vertex_index_endian = xenos::Endian(index_endian & 3);
  sc.vertex_index_offset = regs.Get<reg::VGT_INDX_OFFSET>().indx_offset;
  sc.vertex_index_min = regs.Get<reg::VGT_MIN_VTX_INDX>().min_indx;
  sc.vertex_index_max = regs.Get<reg::VGT_MAX_VTX_INDX>().max_indx;
  if (!pa_cl_clip_cntl.clip_disable) {
    float* wp = sc.user_clip_planes[0];
    uint32_t rem = pa_cl_clip_cntl.ucp_ena, k;
    while (xe::bit_scan_forward(rem, &k)) {
      rem = xe::clear_lowest_bit(rem);
      std::memcpy(wp, &regs[XE_GPU_REG_PA_CL_UCP_0_X + k * 4], 4 * sizeof(float));
      wp += 4;
    }
  }
  for (uint32_t i = 0; i < 3; ++i) {
    sc.ndc_scale[i] = viewport_info.ndc_scale[i];
    sc.ndc_offset[i] = viewport_info.ndc_offset[i];
  }
  sc.sample_count_log2[0] = rb_surface_info.msaa_samples >= xenos::MsaaSamples::k4X ? 1 : 0;
  sc.sample_count_log2[1] = rb_surface_info.msaa_samples >= xenos::MsaaSamples::k2X ? 1 : 0;
  sc.alpha_test_reference = regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF);
  sc.alpha_to_mask = rb_colorcontrol.alpha_to_mask_enable ? (rb_colorcontrol.value >> 24) | (1 << 8) : 0;
  // Texture signs: identity (unsigned) until the texture path provides real values.
  for (uint32_t i = 0; i < 8; ++i) sc.texture_swizzled_signs[i] = 0;
  out->system_constants = &sc;
  out->system_constants_size = uint32_t(sizeof(sc));

  // ── Float / bool+loop / fetch constants (UpdateBindings) ──
  pack_floats(regs, vs->constant_register_map(), XE_GPU_REG_SHADER_CONSTANT_000_X, b.vs_float);
  if (ps) pack_floats(regs, ps->constant_register_map(), XE_GPU_REG_SHADER_CONSTANT_256_X, b.ps_float);
  else b.ps_float.assign(4, 0.0f);
  out->vs_float = b.vs_float.data(); out->vs_float_count = uint32_t(b.vs_float.size() / 4);
  out->ps_float = b.ps_float.data(); out->ps_float_count = uint32_t(b.ps_float.size() / 4);
  std::memcpy(b.bool_loop, &regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031], sizeof(b.bool_loop));
  std::memcpy(b.fetch, &regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], sizeof(b.fetch));
  out->bool_loop = b.bool_loop;
  out->fetch = b.fetch;

  // ── Vertex buffer ranges the VS fetches from shared memory ──
  const auto& cmv = vs->constant_register_map();
  for (uint32_t i = 0; i < xe::countof(cmv.vertex_fetch_bitmap); ++i) {
    uint32_t bits = cmv.vertex_fetch_bitmap[i], j;
    while (xe::bit_scan_forward(bits, &j)) {
      bits = xe::clear_lowest_bit(bits);
      xenos::xe_gpu_vertex_fetch_t vf = regs.GetVertexFetch(i * 32 + j);
      if (vf.type != xenos::FetchConstantType::kVertex) continue;
      if (out->vfetch_count < LSW_GPU_MAX_VFETCH) {
        out->vfetch_addr[out->vfetch_count] = vf.address << 2;
        out->vfetch_size[out->vfetch_count] = vf.size << 2;
        ++out->vfetch_count;
      }
    }
  }

  // DIAG: a VS whose disassembly has vfetch but produced no ranges (first 8 distinct shaders).
  if (!out->vfetch_count) {
    static std::set<uint64_t> seen;
    const std::string& dis = vs->ucode_disassembly();
    if (seen.size() < 8 && dis.find("vfetch") != std::string::npos && seen.insert(vs->ucode_data_hash()).second) {
      dbg_ram("[VFDIAG] VS %016llX %u dw: bitmap %08X %08X %08X, vf95 regs %08X %08X\n",
              (unsigned long long)vs->ucode_data_hash(), vs_dwords, cmv.vertex_fetch_bitmap[0], cmv.vertex_fetch_bitmap[1],
              cmv.vertex_fetch_bitmap[2], regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 190], regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 191]);
      printf("[VFDIAG] VS %016llX %u dw: bitmap %08X %08X %08X\n%s\n", (unsigned long long)vs->ucode_data_hash(), vs_dwords,
             cmv.vertex_fetch_bitmap[0], cmv.vertex_fetch_bitmap[1], cmv.vertex_fetch_bitmap[2], dis.c_str());
      fflush(stdout);
    }
  }

  // ── Texture / sampler bindings ──
  auto fill_tex = [](const DxbcShader& s, LswGpuTexBinding* dst, uint32_t& n) {
    n = 0;
    for (const auto& tb : s.GetTextureBindingsAfterTranslation()) {
      if (n >= LSW_GPU_MAX_TEX) break;
      dst[n++] = {tb.fetch_constant, uint32_t(tb.dimension), tb.is_signed ? 1u : 0u};
    }
  };
  auto fill_smp = [](const DxbcShader& s, LswGpuSamplerBinding* dst, uint32_t& n) {
    n = 0;
    for (const auto& sb : s.GetSamplerBindingsAfterTranslation()) {
      if (n >= LSW_GPU_MAX_TEX) break;
      dst[n++] = {sb.fetch_constant, uint32_t(sb.mag_filter), uint32_t(sb.min_filter),
                  uint32_t(sb.mip_filter), uint32_t(sb.aniso_filter)};
    }
  };
  fill_tex(*vs, out->vs_tex, out->vs_tex_count);
  fill_smp(*vs, out->vs_smp, out->vs_smp_count);
  if (ps) {
    fill_tex(*ps, out->ps_tex, out->ps_tex_count);
    fill_smp(*ps, out->ps_smp, out->ps_smp_count);
  }
  // Texture signs (UpdateSystemConstantValues: GetActiveTextureSwizzledSigns), pre-swizzled.
  uint32_t used_tex = vs->GetUsedTextureMaskAfterTranslation() | (ps ? ps->GetUsedTextureMaskAfterTranslation() : 0);
  uint32_t ti;
  while (xe::bit_scan_forward(used_tex, &ti)) {
    used_tex = xe::clear_lowest_bit(used_tex);
    uint32_t shift = (ti & 3) * 8;
    uint8_t signs = texture_util::SwizzleSigns(regs.GetTextureFetch(ti));
    sc.texture_swizzled_signs[ti >> 2] = (sc.texture_swizzled_signs[ti >> 2] & ~(0xFFu << shift)) | (uint32_t(signs) << shift);
  }
  return 1;
}
