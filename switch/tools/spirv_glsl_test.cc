// PC check of the Switch OpenGL shader path: a dumped Xenos shader (xenia shdump *.ucode.bin.*)
// -> Xenia's SpirvShaderTranslator -> SPIR-V -> SPIRV-Cross -> desktop GLSL (what switch-mesa
// compiles at run time; conversion in switch/source/lsw_spirv_glsl.cc).
// Build and run over xenia/shdump: switch/tools/spirv_glsl_test.sh. Usage:
//   spirv_glsl_test <vs|ps> <ucode.bin> [out.spv]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "lsw_spirv_glsl.h"
#include "xenia/base/string_buffer.h"
#include "xenia/gpu/spirv_shader.h"
#include "xenia/gpu/spirv_shader_translator.h"
using namespace xe::gpu;

std::vector<unsigned int> lsw_build_geometry_shader_spirv(uint32_t key);

int main(int argc, char** argv) {
  if (argc >= 3 && argv[1][0] == 'g') {   // gs <key>: geometry shader from gl_geometry.cc
    const std::vector<unsigned int> spv = lsw_build_geometry_shader_spirv(uint32_t(strtoul(argv[2], nullptr, 0)));
    std::string src, err;
    std::vector<LswGlslSampler> samplers;
    if (spv.empty() || !lsw_spirv_to_glsl(spv.data(), spv.size(), 0, "_gs", src, samplers, err) ||
        !lsw_glsl_gs_remove_returns(src)) {
      fprintf(stderr, "[gs] FAILED (%zu words): %s\n", spv.size(), err.c_str());
      return 3;
    }
    fwrite(src.data(), 1, src.size(), stdout);
    return 0;
  }
  if (argc < 3) { printf("usage: spirv_glsl_test <vs|ps> <ucode.bin> [out.spv] | gs <key>\n"); return 1; }
  const bool ps = argv[1][0] == 'p';
  FILE* f = fopen(argv[2], "rb");
  if (!f) { printf("open fail\n"); return 1; }
  std::vector<uint32_t> d; uint32_t w;
  // shdump files are host order; Shader's constructor byte-swaps (it expects big-endian guest memory).
  while (fread(&w, 4, 1, f) == 1) d.push_back(__builtin_bswap32(w));
  fclose(f);
  SpirvShader sh(ps ? xenos::ShaderType::kPixel : xenos::ShaderType::kVertex, 0x1234, d.data(),
                 uint32_t(d.size()));
  xe::StringBuffer buf;
  sh.AnalyzeUcode(buf);
  // No Vulkan device: core-feature defaults (all=false), SPIR-V 1.0, no EDRAM interlock.
  SpirvShaderTranslator::Features features(false);
  features.spirv_version = spv::Spv_1_0;
  features.max_storage_buffer_range = 512u << 20;   // one binding: the texel buffer covers all 512 MB
  features.full_draw_index_uint32 = true;
  features.clip_distance = true;
  // As gl_bridge.cc: texture swizzles as GL texture state (LSW_XT_VIEWSWIZZLE=0 -> in the shader) and
  // plain multiplications for the host FMZ multiply (LSW_XT_MULZERO=0 -> Xenia's zero selects).
  features.image_view_format_swizzle = true;
  features.lsw_host_mul_zero_wins = true;
  if (const char* e = getenv("LSW_XT_VIEWSWIZZLE")) features.image_view_format_swizzle = atoi(e) != 0;
  if (const char* e = getenv("LSW_XT_MULZERO")) features.lsw_host_mul_zero_wins = atoi(e) != 0;
  SpirvShaderTranslator tr(features, false, false, false);
  uint64_t mod = ps ? tr.GetDefaultPixelShaderModification(64)
                    : tr.GetDefaultVertexShaderModification(
                          64, Shader::HostVertexShaderType::kVertex);
  // Switch dumps (gl_bridge.cc LSWTCS_UCODEDUMP) are named <ucode hash>_<modification>.ucode.bin.*:
  // translate with the modification the game's draw used.
  {
    const char* base = argv[2];
    for (const char* p = argv[2]; *p; ++p)
      if (*p == '/' || *p == '\\') base = p + 1;
    unsigned long long h, m;
    char tail[16];
    // Bit 46 = pixel depth_stencil_mode kEarlyHint, which gl_bridge.cc no longer sets.
    if (sscanf(base, "%16llx_%16llx.ucode.%15s", &h, &m, tail) == 3) mod = ps ? m & ~(1ull << 46) : m;
  }
  // Every hub texture is unsigned, so gl_bridge.cc sets the all-unsigned specialisation
  // (LSW_XT_UNSIGNED=0 -> the generic translation).
  {
    const char* e = getenv("LSW_XT_UNSIGNED");
    if (!e || atoi(e)) mod |= SpirvShaderTranslator::Modification::kLswTexturesUnsigned;
  }
  auto* t = sh.GetOrCreateTranslation(mod);
  const auto t0 = std::chrono::steady_clock::now();
  const bool ok = tr.TranslateAnalyzedShader(*t);
  const auto t1 = std::chrono::steady_clock::now();
  const auto& bin = t->translated_binary();
  fprintf(stderr, "[spirv] %zu ucode dwords -> translate ok=%d, %zu bytes SPIR-V\n", d.size(), ok,
          bin.size());
  if (!ok || bin.size() < 20) return 2;
  if (argc > 3) { FILE* o = fopen(argv[3], "wb"); fwrite(bin.data(), 1, bin.size(), o); fclose(o); }
  std::string src, err;
  std::vector<LswGlslSampler> samplers;
  if (!lsw_spirv_to_glsl(reinterpret_cast<const uint32_t*>(bin.data()), bin.size() / 4, ps ? 0 : 16, ps ? "_ps" : "_vs", src,
                         samplers, err)) {
    fprintf(stderr, "[spirv-cross] FAILED: %s\n", err.c_str());
    return 3;
  }
  fprintf(stderr, "[time] translate %.2f ms, cross %.2f ms\n",
          std::chrono::duration<double, std::milli>(t1 - t0).count(),
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count());
  fwrite(src.data(), 1, src.size(), stdout);
  for (const auto& u : samplers)
    fprintf(stderr, "[units] unit %u <- texture binding %u, sampler binding %u\n", u.unit, u.image_binding,
            u.sampler_binding);
  return 0;
}
