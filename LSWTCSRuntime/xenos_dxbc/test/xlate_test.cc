// Standalone check: translate a dumped Xenos shader (xenia shdump *.ucode.bin.*) to DXBC with
// Xenia's DxbcShaderTranslator and disassemble it with D3DDisassemble.
#include <cstdio>
#include <vector>
#include <windows.h>
#include <d3dcompiler.h>
#include "xenia/base/string_buffer.h"
#include "xenia/gpu/dxbc_shader.h"
#include "xenia/gpu/dxbc_shader_translator.h"
using namespace xe::gpu;
int main(int argc, char** argv) {
  if (argc < 3) { printf("usage: xlate_test <vs|ps> <ucode.bin> [swap]\n"); return 1; }
  bool ps = argv[1][0] == 'p';
  FILE* f = fopen(argv[2], "rb"); if (!f) { printf("open fail\n"); return 1; }
  std::vector<uint32_t> d; uint32_t w; while (fread(&w, 4, 1, f) == 1) d.push_back(argc > 3 ? __builtin_bswap32(w) : w); fclose(f);
  printf("%zu dwords, first %08X\n", d.size(), d.empty() ? 0 : d[0]);
  DxbcShader sh(ps ? xenos::ShaderType::kPixel : xenos::ShaderType::kVertex, 0x1234, d.data(), uint32_t(d.size()));
  xe::StringBuffer buf; sh.AnalyzeUcode(buf);
  printf("analyzed\n");
  DxbcShaderTranslator tr(xe::ui::GraphicsProvider::GpuVendorID::kAMD, false, false);
  uint64_t mod = ps ? tr.GetDefaultPixelShaderModification(64)
                    : tr.GetDefaultVertexShaderModification(64, Shader::HostVertexShaderType::kVertex);
  auto* t = sh.GetOrCreateTranslation(mod);
  bool ok = tr.TranslateAnalyzedShader(*t);
  const auto& bin = t->translated_binary();
  printf("translate ok=%d size=%zu\n", ok, bin.size());
  ID3DBlob* dis = nullptr;
  HRESULT hr = D3DDisassemble(bin.data(), bin.size(), 0, nullptr, &dis);
  printf("D3DDisassemble hr=0x%08X\n", (unsigned)hr);
  if (dis) { fwrite(dis->GetBufferPointer(), 1, dis->GetBufferSize(), stdout); }
  return ok ? 0 : 2;
}
