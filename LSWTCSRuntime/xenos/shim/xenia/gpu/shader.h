// Shim for xenia/gpu/shader.h. The vendored ShaderInterpreter only references Shader in
// two optional convenience overloads (CanInterpretShader / SetShader(const Shader&)); the
// bridge always uses SetShader(type, ucode). This minimal class keeps those compiling.
#ifndef LSWTCS_XENIA_SHADER_SHIM_H_
#define LSWTCS_XENIA_SHADER_SHIM_H_
#include <cstdint>
#include "xenia/gpu/ucode.h"   // the real shader.h provides ucode:: transitively
#include "xenia/gpu/xenos.h"

namespace xe {
namespace gpu {
class Shader {
 public:
  bool is_ucode_analyzed() const { return true; }
  bool uses_texture_fetch_instruction_results() const { return false; }
  xenos::ShaderType type() const { return type_; }
  const uint32_t* ucode_dwords() const { return ucode_; }
  xenos::ShaderType type_ = xenos::ShaderType::kVertex;
  const uint32_t* ucode_ = nullptr;
};
}  // namespace gpu
}  // namespace xe

#endif
