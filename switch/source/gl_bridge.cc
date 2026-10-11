// Switch counterpart of LSWTCSRuntime/xenos_dxbc/lsw_gpu_bridge.cc: the same lsw_gpu_prepare()
// contract (include/lsw_gpu_bridge.h), but shaders go through Xenia's SpirvShaderTranslator and
// SPIRV-Cross to GLSL, and the constants follow the SPIR-V translator's layout. Ports the parts of
// Xenia's VulkanCommandProcessor::IssueDraw / UpdateSystemConstantValues and
// VulkanPipelineCache::GetCurrent*ShaderModification that apply to host render targets (no
// fragment-shader interlock, no resolution scaling, no tessellation, no memexport).
//
// The vs_dxbc / ps_dxbc / gs_dxbc pointers of LswGpuDraw carry LswGlShader objects
// (include/lsw_gl_shader.h), consumed by the OpenGL replay in gl_backend.cpp.
#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "xenia/base/math.h"
#include "xenia/base/string_buffer.h"
#include "xenia/gpu/draw_util.h"
#include "xenia/gpu/register_file.h"
#include "xenia/gpu/registers.h"
#include "xenia/gpu/spirv_shader.h"
#include "xenia/gpu/spirv_shader_translator.h"
#include "xenia/gpu/texture_util.h"
#include "xenia/gpu/xenos.h"

#include "lsw_gl_shader.h"
#include "lsw_gpu_bridge.h"
#include "lsw_spirv_glsl.h"
#define XXH_INLINE_ALL
#include "third_party/xxhash/xxhash.h"
extern uint64_t g_geom_swaps_pub;   // kernel_stubs.cpp: swaps seen ([SHADERNEW])

using namespace xe;
using namespace xe::gpu;

// Geometry shaders for point / rectangle / quad lists: SPIR-V from Xenia's Vulkan generator
// (gl_geometry.cc).
std::vector<unsigned int> lsw_build_geometry_shader_spirv(uint32_t key);

namespace {

using SysConst = SpirvShaderTranslator::SystemConstants;
using ClipConst = SpirvShaderTranslator::ClipPlaneConstants;
static_assert(sizeof(SysConst) <= kLswGlClipPlaneOffset, "system constants overlap the clip planes");
static_assert(sizeof(ClipConst) == kLswGlClipPlaneSize, "clip plane block size");

struct Bridge {
  std::unique_ptr<SpirvShaderTranslator> translator;
  std::unordered_map<uint64_t, std::unique_ptr<SpirvShader>> shaders;
  std::unordered_map<uint64_t, std::unique_ptr<LswGlShader>> gl;   // per translation; null = failed
  std::unordered_map<uint32_t, std::unique_ptr<LswGlShader>> gs;   // per geometry shader key
  StringBuffer disasm_buffer;
  alignas(16) uint8_t sys_block[kLswGlClipPlaneOffset + kLswGlClipPlaneSize];
  std::vector<float> vs_float, ps_float;
  uint32_t bool_loop[40];
  uint32_t fetch[192];
};

Bridge& bridge() {
  static Bridge* s = [] {
    auto* b = new Bridge();
    // No Vulkan device: the GL 4.3 core feature set. SPIR-V 1.0 (SPIRV-Cross input), 32-bit indices
    // native, clip distances available.
    SpirvShaderTranslator::Features f(false);
    f.spirv_version = spv::Spv_1_0;
    f.max_storage_buffer_range = 512u << 20;   // one binding: the texel buffer covers all 512 MB
    f.full_draw_index_uint32 = true;
    f.clip_distance = true;
    f.cull_distance = false;
    // Texture swizzles as GL texture state (gl_backend.cpp apply_swizzle), not in the shader;
    // LSWTCS_GLVIEWSWZ=0 turns both off.
    {
      const char* e = getenv("LSWTCS_GLVIEWSWZ");
      f.image_view_format_swizzle = !e || e[0] == '1';
    }
    // Plain multiplications: switch-mesa applies 0 * anything = 0 itself (FMZ, gl_backend.cpp egl_init;
    // LSWTCS_GLFMZ=0 turns both off).
    {
      const char* e = getenv("LSWTCS_GLFMZ");
      f.lsw_host_mul_zero_wins = !e || e[0] == '1';
    }
    // Pixel-shader computed-LOD fetches as implicit LOD + bias instead of explicit gradients
    // (SpirvShaderTranslator::lsw_implicit_lod_bias). LSWTCS_GLTEXBIAS=0 turns it off.
    {
      const char* e = getenv("LSWTCS_GLTEXBIAS");
      SpirvShaderTranslator::lsw_implicit_lod_bias = !e || e[0] == '1';
    }
    // Forward-jump shaders without the switch dispatcher (LSWTCS_GLFWDCF, default on) and, opt-in,
    // guest loops structured too (LSWTCS_GLFWDLOOPS=1).
    {
      const char* e = getenv("LSWTCS_GLFWDCF");
      SpirvShaderTranslator::lsw_forward_jumps = !e || e[0] == '1';
      const char* l = getenv("LSWTCS_GLFWDLOOPS");
      SpirvShaderTranslator::lsw_structured_loops = l && l[0] == '1';
    }
    b->translator = std::make_unique<SpirvShaderTranslator>(f, false, false, false);
    b->vs_float.reserve(512 * 4);
    b->ps_float.reserve(512 * 4);
    return b;
  }();
  return *s;
}

uint64_t hash_ucode(const uint32_t* p, uint32_t n) {
  uint64_t h = 1469598103934665603ull;
  for (uint32_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
  return h ^ n;
}

// Last shader per (ucode pointer, size, type) with a copy of its guest bytes: a draw reusing the shader
// at the same address is checked with one memcmp instead of re-hashing (as the PC bridge).
struct UcodeMemo { std::vector<uint32_t> be; SpirvShader* shader = nullptr; };

SpirvShader* get_shader(xenos::ShaderType type, const uint32_t* ucode_be, uint32_t dwords) {
  Bridge& b = bridge();
  static std::unordered_map<uint64_t, UcodeMemo> memo_map;
  const uint64_t mkey = uint64_t(reinterpret_cast<uintptr_t>(ucode_be)) ^ (uint64_t(dwords) << 40) ^
                        (type == xenos::ShaderType::kPixel ? 0x8000000000000000ull : 0);
  UcodeMemo& memo = memo_map[mkey];
  if (memo.shader && memo.be.size() == dwords && std::memcmp(memo.be.data(), ucode_be, dwords * 4u) == 0)
    return memo.shader;
  const uint64_t key = hash_ucode(ucode_be, dwords) ^ (type == xenos::ShaderType::kPixel ? 0x8000000000000000ull : 0);
  SpirvShader* raw;
  auto it = b.shaders.find(key);
  if (it != b.shaders.end()) {
    raw = it->second.get();
  } else {
    // The Shader constructor byte-swaps from guest (big-endian) order.
    auto sh = std::make_unique<SpirvShader>(type, key, ucode_be, dwords);
    sh->AnalyzeUcode(b.disasm_buffer);
    {   // DIAG: every new guest shader under Xenia's name for it (pipeline_cache.cc: XXH3_64bits of the guest bytes,
        // dump files shader_<hash>.ucode.*), with the swap it first appeared at: the shaders a room draws in the
        // port and not in Xenia (dump_shaders) belong to effects the port switches on by itself (Episode 2 hub
        // jukebox already lit, 2026-10-10).
        printf("[SHADERNEW] swap %llu %s %016llX (%u dwords)\n", (unsigned long long)g_geom_swaps_pub,
               type == xenos::ShaderType::kPixel ? "PS" : "VS",
               (unsigned long long)XXH3_64bits(ucode_be, size_t(dwords) * 4u), dwords);
    }
    raw = sh.get();
    b.shaders.emplace(key, std::move(sh));
  }
  memo.be.assign(ucode_be, ucode_be + dwords);
  memo.shader = raw;
  return raw;
}

void log_once(uint64_t key, const char* what, const char* detail) {
  static unsigned n = 0;
  if (n++ < 40) {
    printf("[GLBRIDGE] %016llX: %s%s%s\n", (unsigned long long)key, what, detail ? ": " : "", detail ? detail : "");
    fflush(stdout);
  }
}

// DIAG (LSWTCS_UCODEDUMP=1): every translated (shader, modification) -> sdmc:/lswtcs/ucode/
// <ucode hash>_<modification>.ucode.bin.<vert|frag> (host order, as xenia shdump), the corpus for
// switch/tools/spirv_glsl_test.sh; plus a [GLXLATE] line with the translate / SPIRV-Cross ms and sizes.
bool ucode_dump_on() {
  static int on = -1;
  if (on < 0) { const char* e = getenv("LSWTCS_UCODEDUMP"); on = e ? atoi(e) : 0; if (on) mkdir("sdmc:/lswtcs/ucode", 0777); }
  return on != 0;
}

void ucode_dump(const SpirvShader& shader, uint64_t modification, LswGlStage stage) {
  char path[128];
  snprintf(path, sizeof(path), "sdmc:/lswtcs/ucode/%016llX_%016llX.ucode.bin.%s",
           (unsigned long long)shader.ucode_data_hash(), (unsigned long long)modification,
           stage == kLswGlPixel ? "frag" : "vert");
  if (FILE* f = fopen(path, "wb")) {
    fwrite(shader.ucode_data().data(), 4, shader.ucode_dword_count(), f);
    fclose(f);
  }
}

double ms_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// Translation of `shader` with `modification` as GLSL plus its texture units (cached).
const LswGlShader* get_glsl(SpirvShader& shader, uint64_t modification, LswGlStage stage) {
  Bridge& b = bridge();
  const uint64_t key = shader.ucode_data_hash() ^ (modification * 0x9E3779B97F4A7C15ull);
  auto it = b.gl.find(key);
  if (it != b.gl.end()) return it->second.get();
  std::unique_ptr<LswGlShader> out;
  const bool dump = ucode_dump_on();
  if (dump) ucode_dump(shader, modification, stage);
  auto t0 = std::chrono::steady_clock::now();
  double xlate_ms = 0;
  auto* t = shader.GetOrCreateTranslation(modification);
  if (!t->is_translated()) b.translator->TranslateAnalyzedShader(*t);
  if (dump) { xlate_ms = ms_since(t0); t0 = std::chrono::steady_clock::now(); }
  if (!t->is_valid()) {
    log_once(key, stage == kLswGlPixel ? "PS SPIR-V translation failed" : "VS SPIR-V translation failed", nullptr);
  } else {
    const auto& bin = t->translated_binary();
    std::string glsl, err;
    std::vector<LswGlslSampler> samplers;
    // Pixel shader units from 0, vertex shader units from 16 (one program binds both).
    if (!lsw_spirv_to_glsl(reinterpret_cast<const uint32_t*>(bin.data()), bin.size() / 4,
                           stage == kLswGlPixel ? 0 : 16, stage == kLswGlPixel ? "_ps" : "_vs", glsl,
                           samplers, err)) {
      log_once(key, "SPIRV-Cross failed", err.c_str());
    } else {
      if (dump) {
        printf("[GLXLATE] %016llX_%016llX %s: %u ucode dw, translate %.1f ms -> %zu B SPIR-V, cross %.1f ms -> %zu B GLSL\n",
               (unsigned long long)shader.ucode_data_hash(), (unsigned long long)modification,
               stage == kLswGlPixel ? "ps" : "vs", unsigned(shader.ucode_dword_count()), xlate_ms, bin.size(),
               ms_since(t0), glsl.size());
        fflush(stdout);
      }
      // Pixel shaders without `precise` (LSWTCS_GLPSPRECISE=1 keeps it): Xenia marks every ALU result NoContraction
      // to reproduce the 360's rounding exactly, which stops Mesa fusing mul+add into ffma and reordering, roughly
      // doubling the ALU instruction count of the heavy full-screen passes on the Switch GPU (hardware [GLGPU]: top
      // pixel shaders 1.5-2.9 ms per frame each). Vertex shaders keep it: position invariance across passes.
      if (stage == kLswGlPixel) {
        static const bool keep = [] { const char* e = getenv("LSWTCS_GLPSPRECISE"); return e && e[0] == '1'; }();
        if (!keep) {
          std::string o;
          o.reserve(glsl.size());
          for (size_t i = 0; i < glsl.size();) {
            if (glsl.compare(i, 8, "precise ") == 0 && (i == 0 || !(isalnum(uint8_t(glsl[i - 1])) || glsl[i - 1] == '_'))) {
              i += 8;
              continue;
            }
            o += glsl[i++];
          }
          glsl.swap(o);
        }
      }
      out = std::make_unique<LswGlShader>();
      out->stage = stage;
      out->key = key;
      out->glsl = std::move(glsl);
      const auto& tex = shader.GetTextureBindingsAfterTranslation();
      const auto& smp = shader.GetSamplerBindingsAfterTranslation();
      for (const LswGlslSampler& s : samplers) {
        const uint32_t si = s.sampler_binding - uint32_t(tex.size());
        if (s.image_binding >= tex.size() || si >= smp.size()) {
          log_once(key, "texture unit with an unknown binding", nullptr);
          continue;
        }
        const auto& tb = tex[s.image_binding];
        const auto& sb = smp[si];
        LswGlShaderUnit u;
        u.unit = s.unit;
        u.fetch_constant = tb.fetch_constant;
        u.dimension = uint32_t(tb.dimension);
        u.is_signed = tb.is_signed ? 1u : 0u;
        u.sampler = {sb.fetch_constant, uint32_t(sb.mag_filter), uint32_t(sb.min_filter),
                     uint32_t(sb.mip_filter), uint32_t(sb.aniso_filter)};
        out->units.push_back(u);
      }
    }
  }
  const LswGlShader* raw = out.get();
  b.gl.emplace(key, std::move(out));
  return raw;
}

void pack_floats(const RegisterFile& regs, const Shader::ConstantRegisterMap& map, uint32_t base_reg,
                 std::vector<float>& out) {
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
  return kLswGlClipPlaneOffset + kLswGlClipPlaneSize;
}

// Viewport and scissor exactly as lsw_gpu_prepare computes them (kernel_stubs.cpp walk-time tile skipping: a
// repeated tile's draw reuses tile 0's record and only needs these for its own window offset).
extern "C" void lsw_gpu_viewport_scissor(const uint32_t* regs_in, uint32_t ps_writes_depth, float viewport[6],
                                         uint32_t scissor_out[4]) {
  const RegisterFile& regs = *reinterpret_cast<const RegisterFile*>(regs_in);
  draw_util::ViewportInfo viewport_info;
  draw_util::GetViewportInfoArgs gviargs{};
  gviargs.Setup(1, 1, divisors::MagicDiv(1), divisors::MagicDiv(1), true, 16384, 16384, false,
                draw_util::GetNormalizedDepthControl(regs), false, true, ps_writes_depth != 0);
  gviargs.SetupRegisterValues(regs);
  draw_util::GetHostViewportInfo(&gviargs, viewport_info);
  viewport[0] = float(viewport_info.xy_offset[0]);
  viewport[1] = float(viewport_info.xy_offset[1]);
  viewport[2] = float(viewport_info.xy_extent[0]);
  viewport[3] = float(viewport_info.xy_extent[1]);
  viewport[4] = viewport_info.z_min;
  viewport[5] = viewport_info.z_max;
  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor);
  scissor_out[0] = scissor.offset[0]; scissor_out[1] = scissor.offset[1];
  scissor_out[2] = scissor.extent[0]; scissor_out[3] = scissor.extent[1];
}

extern "C" int lsw_gpu_prepare(const uint32_t* regs_in, const uint32_t* vs_ucode_be, uint32_t vs_dwords,
                               const uint32_t* ps_ucode_be, uint32_t ps_dwords, uint32_t index_endian,
                               uint32_t rt_width, uint32_t rt_height, LswGpuDraw* out) {
  (void)rt_width; (void)rt_height;
  if (!regs_in || !vs_ucode_be || !vs_dwords || !out) return 0;
  Bridge& b = bridge();
  static_assert(std::is_standard_layout_v<RegisterFile> &&
                    sizeof(RegisterFile) == sizeof(uint32_t) * RegisterFile::kRegisterCount,
                "RegisterFile must be a plain register array");
  const RegisterFile& regs = *reinterpret_cast<const RegisterFile*>(regs_in);
  std::memset(out, 0, sizeof(*out));

  SpirvShader* vs = get_shader(xenos::ShaderType::kVertex, vs_ucode_be, vs_dwords);
  if (!vs || !vs->is_ucode_analyzed()) return 0;
  const bool primitive_polygonal = draw_util::IsPrimitivePolygonal(regs);
  SpirvShader* ps = nullptr;
  if (ps_ucode_be && ps_dwords && draw_util::IsRasterizationPotentiallyDone(regs, primitive_polygonal)) {
    ps = get_shader(xenos::ShaderType::kPixel, ps_ucode_be, ps_dwords);
    if (ps && !draw_util::IsPixelShaderNeededWithRasterization(*ps, regs)) ps = nullptr;
  }
  const reg::RB_DEPTHCONTROL normalized_depth_control = draw_util::GetNormalizedDepthControl(regs);
  const auto prim_type = regs.Get<reg::VGT_DRAW_INITIATOR>().prim_type;

  // ── Shader modifications (VulkanPipelineCache::GetCurrent*ShaderModification) ──
  uint32_t ps_param_gen_pos = UINT32_MAX;
  const uint32_t interpolator_mask =
      ps ? (vs->writes_interpolators() &
            ps->GetInterpolatorInputMask(regs.Get<reg::SQ_PROGRAM_CNTL>(), regs.Get<reg::SQ_CONTEXT_MISC>(),
                                         ps_param_gen_pos))
         : 0;
  SpirvShaderTranslator::Modification vs_mod(b.translator->GetDefaultVertexShaderModification(
      vs->GetDynamicAddressableRegisterCount(regs.Get<reg::SQ_PROGRAM_CNTL>().vs_num_reg),
      Shader::HostVertexShaderType::kVertex));
  vs_mod.vertex.interpolator_mask = interpolator_mask;
  const auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  const uint32_t user_clip_planes = pa_cl_clip_cntl.clip_disable ? 0 : pa_cl_clip_cntl.ucp_ena;
  vs_mod.vertex.user_clip_plane_count = xe::bit_count(user_clip_planes);
  vs_mod.vertex.user_clip_plane_cull = uint32_t(user_clip_planes && pa_cl_clip_cntl.ucp_cull_only_ena);
  vs_mod.vertex.output_point_parameters =
      uint32_t((vs->writes_point_size_edge_flag_kill_vertex() & 0b001) && prim_type == xenos::PrimitiveType::kPointList);

  SpirvShaderTranslator::Modification ps_mod(0);
  if (ps) {
    ps_mod = SpirvShaderTranslator::Modification(b.translator->GetDefaultPixelShaderModification(
        ps->GetDynamicAddressableRegisterCount(regs.Get<reg::SQ_PROGRAM_CNTL>().ps_num_reg)));
    ps_mod.pixel.interpolator_mask = interpolator_mask;
    ps_mod.pixel.interpolators_centroid =
        interpolator_mask & ~xenos::GetInterpolatorSamplingPattern(
                                regs.Get<reg::RB_SURFACE_INFO>().msaa_samples,
                                regs.Get<reg::SQ_CONTEXT_MISC>().sc_sample_cntl,
                                regs.Get<reg::SQ_INTERPOLATOR_CNTL>().sampling_pattern);
    if (ps_param_gen_pos < xenos::kMaxInterpolators) {
      ps_mod.pixel.param_gen_enable = 1;
      ps_mod.pixel.param_gen_interpolator = ps_param_gen_pos;
      ps_mod.pixel.param_gen_point = uint32_t(prim_type == xenos::PrimitiveType::kPointList);
    }
    // No early-fragment-tests hint (Vulkan sets it when the shader neither kills nor writes depth and
    // coverage does not depend on alpha): exactly when Maxwell already runs depth tests early on its
    // own, and with the hint every such pixel shader was a second program (twice the compile).
    ps_mod.pixel.depth_stencil_mode = SpirvShaderTranslator::Modification::DepthStencilMode::kNoModifiers;
    ps_mod.pixel.rt0_blend_rgb_factor_for_premult = xenos::BlendFactor::kOne;
    ps_mod.pixel.rt0_blend_a_factor_for_premult = xenos::BlendFactor::kOne;
    if (ps->writes_color_target(0)) {
      auto bc = regs.Get<reg::RB_BLENDCONTROL>(reg::RB_BLENDCONTROL::rt_register_indices[0]);
      if ((bc.color_comb_fcn == xenos::BlendOp::kMin || bc.color_comb_fcn == xenos::BlendOp::kMax) &&
          bc.color_srcblend == xenos::BlendFactor::kSrcAlpha && bc.color_destblend == xenos::BlendFactor::kOne)
        ps_mod.pixel.rt0_blend_rgb_factor_for_premult = xenos::BlendFactor::kSrcAlpha;
      if ((bc.alpha_comb_fcn == xenos::BlendOp::kMin || bc.alpha_comb_fcn == xenos::BlendOp::kMax) &&
          bc.alpha_srcblend == xenos::BlendFactor::kSrcAlpha && bc.alpha_destblend == xenos::BlendFactor::kOne)
        ps_mod.pixel.rt0_blend_a_factor_for_premult = xenos::BlendFactor::kSrcAlpha;
    }
  }
  // Specialisation: all textures of the shader unsigned in this draw (every texture in the hub) ->
  // fetches without the signed / biased / gamma paths (SpirvShaderTranslator::LswTexturesUnsigned).
  auto textures_unsigned = [&](const Shader& s) {
    for (const auto& tb : s.texture_bindings())
      if (texture_util::SwizzleSigns(regs.GetTextureFetch(tb.fetch_constant))) return false;
    return true;
  };
  if (textures_unsigned(*vs)) vs_mod.value |= SpirvShaderTranslator::Modification::kLswTexturesUnsigned;
  if (ps && textures_unsigned(*ps)) ps_mod.value |= SpirvShaderTranslator::Modification::kLswTexturesUnsigned;

  const LswGlShader* vs_gl = get_glsl(*vs, vs_mod.value, kLswGlVertex);
  if (!vs_gl) return 0;
  const LswGlShader* ps_gl = ps ? get_glsl(*ps, ps_mod.value, kLswGlPixel) : nullptr;
  if (ps && !ps_gl) ps = nullptr;

  out->vs_dxbc = vs_gl; out->vs_dxbc_size = 1; out->vs_key = vs_gl->key;
  if (ps_gl) {
    out->ps_dxbc = ps_gl; out->ps_dxbc_size = 1; out->ps_key = ps_gl->key;
    out->ps_writes_depth = ps->writes_depth() ? 1 : 0;
    out->ps_color_targets = ps->writes_color_targets();
  }
  out->primitive_polygonal = primitive_polygonal ? 1 : 0;
  // Geometry shader key bits (VulkanPipelineCache::GetGeometryShaderKey without the type, which the
  // recorder passes separately): interpolator count 2-6, user clip planes 7, cull 8, vertex kill 9,
  // point size 10, point coordinates 11.
  out->gs_key_bits = (uint32_t(xe::bit_count(interpolator_mask)) & 31u) << 2 |
                     uint32_t(vs_mod.vertex.user_clip_plane_count > 0) << 7 |
                     uint32_t(vs_mod.vertex.user_clip_plane_cull) << 8 |
                     uint32_t(vs_mod.vertex.output_point_parameters) << 10 |
                     uint32_t(ps ? ps_mod.pixel.param_gen_point : 0) << 11;

  // ── Viewport / scissor (same arguments as the PC bridge: GL with upper-left clip control behaves
  //    like D3D) ──
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

  // ── System constants (VulkanCommandProcessor::UpdateSystemConstantValues, host RT subset) ──
  std::memset(b.sys_block, 0, sizeof(b.sys_block));
  SysConst& sc = *reinterpret_cast<SysConst*>(b.sys_block);
  ClipConst& cc = *reinterpret_cast<ClipConst*>(b.sys_block + kLswGlClipPlaneOffset);
  const auto pa_cl_vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();
  const auto rb_colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
  const auto rb_depth_info = regs.Get<reg::RB_DEPTH_INFO>();
  const auto rb_surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  uint32_t flags = 0;
  if (pa_cl_vte_cntl.vtx_xy_fmt) flags |= SpirvShaderTranslator::kSysFlag_XYDividedByW;
  if (pa_cl_vte_cntl.vtx_z_fmt) flags |= SpirvShaderTranslator::kSysFlag_ZDividedByW;
  if (pa_cl_vte_cntl.vtx_w0_fmt) flags |= SpirvShaderTranslator::kSysFlag_WNotReciprocal;
  if (primitive_polygonal) flags |= SpirvShaderTranslator::kSysFlag_PrimitivePolygonal;
  if (draw_util::IsPrimitiveLine(regs)) flags |= SpirvShaderTranslator::kSysFlag_PrimitiveLine;
  flags |= uint32_t(rb_surface_info.msaa_samples) << SpirvShaderTranslator::kSysFlag_MsaaSamples_Shift;
  if (rb_depth_info.depth_format == xenos::DepthRenderTargetFormat::kD24FS8)
    flags |= SpirvShaderTranslator::kSysFlag_DepthFloat24;
  const xenos::CompareFunction alpha_test_function =
      rb_colorcontrol.alpha_test_enable ? rb_colorcontrol.alpha_func : xenos::CompareFunction::kAlways;
  flags |= uint32_t(alpha_test_function) << SpirvShaderTranslator::kSysFlag_AlphaPassIfLess_Shift;
  for (uint32_t i = 0; i < 4; ++i) {
    auto ci = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
    if (ci.color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA)
      flags |= SpirvShaderTranslator::kSysFlag_ConvertColor0ToGamma << i;
    sc.color_exp_bias[i] = xe::memory::Reinterpret<float>(int32_t(0x3F800000 + (int32_t(ci.color_exp_bias) << 23)));
  }
  sc.flags = flags;
  sc.vertex_index_endian = xenos::Endian(index_endian & 3);
  sc.vertex_base_index = regs.Get<int32_t>(XE_GPU_REG_VGT_INDX_OFFSET);
  for (uint32_t i = 0; i < 3; ++i) {
    sc.ndc_scale[i] = viewport_info.ndc_scale[i];
    sc.ndc_offset[i] = viewport_info.ndc_offset[i];
  }
  if (user_clip_planes) {
    float* wp = cc.user_clip_planes[0];
    uint32_t rem = user_clip_planes, k;
    while (xe::bit_scan_forward(rem, &k)) {
      rem = xe::clear_lowest_bit(rem);
      if (k >= 6) continue;
      std::memcpy(wp, &regs[XE_GPU_REG_PA_CL_UCP_0_X + k * 4], 4 * sizeof(float));
      wp += 4;
    }
  }
  if (prim_type == xenos::PrimitiveType::kPointList) {
    const auto minmax = regs.Get<reg::PA_SU_POINT_MINMAX>();
    const auto size = regs.Get<reg::PA_SU_POINT_SIZE>();
    sc.point_vertex_diameter_min = float(minmax.min_size) * (2.0f / 16.0f);
    sc.point_vertex_diameter_max = float(minmax.max_size) * (2.0f / 16.0f);
    sc.point_constant_diameter[0] = float(size.width) * (2.0f / 16.0f);
    sc.point_constant_diameter[1] = float(size.height) * (2.0f / 16.0f);
    sc.point_screen_diameter_to_ndc_radius[0] = 1.0f / std::max(viewport_info.xy_extent[0], uint32_t(1));
    sc.point_screen_diameter_to_ndc_radius[1] = 1.0f / std::max(viewport_info.xy_extent[1], uint32_t(1));
  }
  // Texture signs and host swizzles per used fetch constant (no image-view swizzle in GL here).
  uint32_t used_tex = vs->GetUsedTextureMaskAfterTranslation() | (ps ? ps->GetUsedTextureMaskAfterTranslation() : 0);
  uint32_t ti;
  while (xe::bit_scan_forward(used_tex, &ti)) {
    used_tex = xe::clear_lowest_bit(used_tex);
    const auto fetch = regs.GetTextureFetch(ti);
    const uint32_t sign_shift = 8 * (ti & 3);
    sc.texture_swizzled_signs[ti >> 2] |= uint32_t(texture_util::SwizzleSigns(fetch)) << sign_shift;
    const uint32_t swz_shift = 12 * (ti & 1);
    sc.texture_swizzles[ti >> 1] |=
        (lsw_gpu_texture_mapping(&regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + ti * 6]) & 0xFFFu) << swz_shift;
  }
  if (ucode_dump_on()) {   // DIAG: distinct texture states (signs, host swizzle) per shader pair
    static std::unordered_map<uint64_t, bool> seen;
    char line[512];
    int n = snprintf(line, sizeof line, "[GLTEXSTATE] %016llX %016llX:", (unsigned long long)vs->ucode_data_hash(),
                     (unsigned long long)(ps ? ps->ucode_data_hash() : 0));
    uint64_t h = vs->ucode_data_hash() * 31 + (ps ? ps->ucode_data_hash() : 0);
    uint32_t rem = vs->GetUsedTextureMaskAfterTranslation() | (ps ? ps->GetUsedTextureMaskAfterTranslation() : 0);
    while (xe::bit_scan_forward(rem, &ti)) {
      rem = xe::clear_lowest_bit(rem);
      const uint32_t signs = (sc.texture_swizzled_signs[ti >> 2] >> (8 * (ti & 3))) & 0xFF;
      const uint32_t swz = (sc.texture_swizzles[ti >> 1] >> (12 * (ti & 1))) & 0xFFF;
      h = (h * 1099511628211ull) ^ (uint64_t(ti) << 32 | signs << 12 | swz);
      if (n < int(sizeof line) - 24) n += snprintf(line + n, sizeof line - n, " t%u s%02X w%03X", ti, signs, swz);
    }
    if (!seen[h]) { seen[h] = true; printf("%s\n", line); fflush(stdout); }
  }
  sc.textures_resolved = 0;
  sc.alpha_test_reference = regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF);
  sc.alpha_to_mask = rb_colorcontrol.alpha_to_mask_enable ? (rb_colorcontrol.value >> 24) | (1 << 8) : 0;
  out->system_constants = b.sys_block;
  out->system_constants_size = uint32_t(sizeof(b.sys_block));

  // ── Float / bool+loop / fetch constants ──
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

  // ── Texture / sampler bindings (the recorder copies these; the GL replay uses the shader's units) ──
  auto fill = [&](const SpirvShader& s, LswGpuTexBinding* tex, uint32_t& nt, LswGpuSamplerBinding* smp, uint32_t& ns) {
    nt = 0;
    for (const auto& tb : s.GetTextureBindingsAfterTranslation())
      if (nt < LSW_GPU_MAX_TEX) tex[nt++] = {tb.fetch_constant, uint32_t(tb.dimension), tb.is_signed ? 1u : 0u};
    ns = 0;
    for (const auto& sb : s.GetSamplerBindingsAfterTranslation())
      if (ns < LSW_GPU_MAX_TEX)
        smp[ns++] = {sb.fetch_constant, uint32_t(sb.mag_filter), uint32_t(sb.min_filter), uint32_t(sb.mip_filter),
                     uint32_t(sb.aniso_filter)};
  };
  fill(*vs, out->vs_tex, out->vs_tex_count, out->vs_smp, out->vs_smp_count);
  if (ps) fill(*ps, out->ps_tex, out->ps_tex_count, out->ps_smp, out->ps_smp_count);
  return 1;
}

// gs_type: 1 point list, 2 rectangle list, 3 quad list (= Xenia's PipelineGeometryShader values).
extern "C" const void* lsw_gpu_geometry_shader(uint32_t gs_type, uint32_t key_bits, size_t* size_out) {
  if (size_out) *size_out = 0;
  if (gs_type < 1 || gs_type > 3) return nullptr;
  Bridge& b = bridge();
  const uint32_t key = (gs_type & 3u) | (key_bits & ~3u);
  auto it = b.gs.find(key);
  if (it == b.gs.end()) {
    std::unique_ptr<LswGlShader> out;
    const std::vector<unsigned int> spirv = lsw_build_geometry_shader_spirv(key);
    std::string glsl, err;
    std::vector<LswGlslSampler> samplers;
    if (spirv.empty() || !lsw_spirv_to_glsl(spirv.data(), spirv.size(), 0, "_gs", glsl, samplers, err)) {
      log_once(key, "geometry shader failed", err.c_str());
    } else if (!lsw_glsl_gs_remove_returns(glsl)) {
      log_once(key, "geometry shader has loops: early returns not rewritten (hangs Eden)", nullptr);
    } else {
      out = std::make_unique<LswGlShader>();
      out->stage = kLswGlGeometry;
      out->key = key;
      out->glsl = std::move(glsl);
    }
    it = b.gs.emplace(key, std::move(out)).first;
  }
  if (it->second && size_out) *size_out = 1;
  return it->second.get();
}
