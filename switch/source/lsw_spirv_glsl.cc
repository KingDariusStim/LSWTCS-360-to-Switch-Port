// SPIR-V (from Xenia's SpirvShaderTranslator) -> desktop GLSL for switch-mesa, via SPIRV-Cross.
// Kept in its own translation unit: SPIRV-Cross's spirv.hpp and glslang's spirv.hpp11 (used by
// Xenia's translator) both define namespace spv and cannot be included together.
//
// Binding layout in the GLSL: uniform blocks keep Xenia's per-set binding numbers (system 0, VS
// floats 1, PS floats 2, bool/loop 3, fetch 4, clip planes 5), which are already distinct; shared
// memory is xe_shared_memory[4] at SSBO bindings 0-3; combined samplers get units from unit_base.
#include "lsw_spirv_glsl.h"

#include <cstring>
#include <exception>
#include "spirv_glsl.hpp"

// Eden drops every draw whose shader reads a storage buffer (mesa's SSBO addressing is not what its
// recompiler tracks; GLTEST ladder, 2026-10-07), while buffer textures work. So the translator's
// read-only shared memory `xe_shared_memory[4]` (4 x 128 MB SSBO bindings) becomes one R32UI
// usamplerBuffer over the whole 512 MB on unit kLswShmTextureUnit, and every
// `xe_shared_memory[A].shared_memory[B]` becomes `texelFetch(tbo, int((A) * 33554432u + (B))).x`
// (`xe_shared_memory.shared_memory[B]` -> `texelFetch(tbo, int(B)).x` with a single binding).
static size_t match_bracket(const std::string& s, size_t open) {   // s[open] == '[' -> index of its ']'
  int depth = 0;
  for (size_t i = open; i < s.size(); ++i) {
    if (s[i] == '[') ++depth;
    else if (s[i] == ']' && --depth == 0) return i;
  }
  return std::string::npos;
}

static bool shared_memory_to_texel_buffer(std::string& glsl, std::string& err) {
  const size_t block = glsl.find("buffer XeSharedMemory");
  if (block == std::string::npos) return true;
  const size_t decl_start = glsl.rfind("layout(", block);
  // One binding over the whole 512 MB (Features::max_storage_buffer_range = 512 MB, gl_bridge.cc), or
  // the translator's array of 128 MB bindings.
  const bool array = glsl.find("} xe_shared_memory[4];\n", block) != std::string::npos;
  const std::string decl_tail = array ? "} xe_shared_memory[4];\n" : "} xe_shared_memory;\n";
  const size_t decl_end = glsl.find(decl_tail, block);
  if (decl_start == std::string::npos || decl_end == std::string::npos) { err = "XeSharedMemory declaration not recognised"; return false; }
  if (glsl.substr(decl_start, block - decl_start).find("readonly") == std::string::npos) {
    err = "XeSharedMemory is written (memexport): no texel-buffer form";
    return false;
  }
  glsl.replace(decl_start, decl_end + decl_tail.size() - decl_start,
               "layout(binding = " + std::to_string(kLswShmTextureUnit) + ") uniform usamplerBuffer xe_shared_memory_tbo;\n");
  const std::string head = array ? "xe_shared_memory[" : "xe_shared_memory.shared_memory[";
  for (size_t p = 0; (p = glsl.find(head, p)) != std::string::npos;) {
    std::string index;
    size_t end;
    if (array) {
      const size_t a_open = p + head.size() - 1, a_close = match_bracket(glsl, a_open);
      const std::string mid = ".shared_memory[";
      if (a_close == std::string::npos || glsl.compare(a_close + 1, mid.size(), mid) != 0) { err = "unexpected xe_shared_memory use"; return false; }
      const size_t b_open = a_close + mid.size(), b_close = match_bracket(glsl, b_open);
      if (b_close == std::string::npos) { err = "unbalanced xe_shared_memory index"; return false; }
      index = "(" + glsl.substr(a_open + 1, a_close - a_open - 1) + ") * 33554432u + (" +
              glsl.substr(b_open + 1, b_close - b_open - 1) + ")";
      end = b_close + 1;
    } else {
      const size_t b_open = p + head.size() - 1, b_close = match_bracket(glsl, b_open);
      if (b_close == std::string::npos) { err = "unbalanced xe_shared_memory index"; return false; }
      index = glsl.substr(b_open + 1, b_close - b_open - 1);
      end = b_close + 1;
    }
    const std::string repl = "texelFetch(xe_shared_memory_tbo, int(" + index + ")).x";
    glsl.replace(p, end - p, repl);
    p += repl.size();
  }
  return true;
}

// Xenia wraps main's body in a loop even without jumps ("so exece can break"); SPIRV-Cross prints it as
// `for (;;) { ...; break; }`. With the whole shader inside a loop, Mesa's optimisations and register
// allocation treat every value as loop-carried (link time up to 3x on the hub's shaders). When the only
// break of that loop is its last statement and nothing continues it, the loop is a plain block.
static void remove_single_pass_main_loop(std::string& glsl) {
  const std::string head = "\n    SPIRV_CROSS_LOOP\n    for (;;)\n    {\n";
  const size_t main_pos = glsl.find("void main()\n{\n");
  if (main_pos == std::string::npos) return;
  const size_t h = glsl.find(head, main_pos);
  if (h == std::string::npos) return;
  const size_t body = h + head.size();
  // The loop's closing brace: the first line that is exactly "    }" after the head.
  const size_t close = glsl.find("\n    }\n", body - 1);
  if (close == std::string::npos) return;
  const std::string last_break = "        break;\n";
  if (close + 1 < last_break.size() || glsl.compare(close + 1 - last_break.size(), last_break.size(), last_break) != 0) return;
  // Every break / continue that belongs to this loop (not to a nested loop or switch) must be the last one.
  std::vector<char> scopes;   // per open brace: 'l' loop, 's' switch, 'b' other block
  char pending = 'b';
  size_t owned = 0;
  for (size_t p = body; p < close + 1;) {
    size_t e = glsl.find('\n', p);
    std::string line = glsl.substr(p, e - p);
    p = e + 1;
    const size_t t = line.find_first_not_of(' ');
    if (t == std::string::npos) continue;
    line.erase(0, t);
    if (line.compare(0, 5, "for (") == 0 || line.compare(0, 7, "while (") == 0 || line == "do") pending = 'l';
    else if (line.compare(0, 8, "switch (") == 0) pending = 's';
    else if (line == "{") { scopes.push_back(pending); pending = 'b'; }
    else if (line.compare(0, 1, "}") == 0) { if (scopes.empty()) return; scopes.pop_back(); }
    else if (line == "break;" || line == "continue;") {
      bool nested = false;
      for (char s : scopes) nested |= s == 'l' || (s == 's' && line == "break;");
      if (!nested) ++owned;
    } else if (line.find('{') != std::string::npos || line.find('}') != std::string::npos) {
      return;   // braces inside a statement line: not the layout this scan understands
    }
  }
  if (owned != 1 || !scopes.empty()) return;
  glsl.erase(close + 1 - last_break.size(), last_break.size());
  glsl.replace(h, head.size(), "\n    {\n");
}

bool lsw_glsl_gs_remove_returns(std::string& glsl) {
  if (glsl.find("return;") == std::string::npos) return true;
  const size_t main_pos = glsl.find("void main()");
  if (main_pos == std::string::npos) return false;
  const size_t open = glsl.find('{', main_pos);
  const size_t close = glsl.rfind('}');
  if (open == std::string::npos || close == std::string::npos || close < open) return false;
  std::string body = glsl.substr(open + 1, close - open - 1);
  if (body.find("for (") != std::string::npos || body.find("while (") != std::string::npos ||
      body.find("do\n") != std::string::npos || body.find("switch (") != std::string::npos)
    return false;
  for (size_t p = 0; (p = body.find("return;", p)) != std::string::npos; p += 6) body.replace(p, 7, "break;");
  glsl = glsl.substr(0, open + 1) + "\n    do {" + body + "    } while (false);\n" + glsl.substr(close);
  return true;
}

// GLSL.std.450 NMin / NMax / NClamp (min/max returning the non-NaN operand, which Xenia uses to match
// Xenos) have no GLSL equivalent, so SPIRV-Cross spells each as `isnan(a) ? b : (isnan(b) ? a : min(a,
// b))`: about a third of the hub's GLSL. Maxwell's FMNMX already returns the non-NaN operand, and
// mesa's nvc0 backend lowers min/max/clamp to it, so FMin / FMax / FClamp compile to the same code.
static void spirv_nan_minmax_to_plain(std::vector<uint32_t>& spv) {
  enum : uint32_t { kOpExtInstImport = 11, kOpExtInst = 12, kNMin = 79, kNMax = 80, kNClamp = 81,
                    kFMin = 37, kFMax = 40, kFClamp = 43 };
  uint32_t glsl_set = 0;
  for (size_t i = 5; i < spv.size();) {
    const uint32_t op = spv[i] & 0xFFFFu, n = spv[i] >> 16;
    if (!n || i + n > spv.size()) return;
    if (op == kOpExtInstImport && n >= 3 &&
        std::strncmp(reinterpret_cast<const char*>(&spv[i + 2]), "GLSL.std.450", (n - 2) * 4) == 0)
      glsl_set = spv[i + 1];
    else if (op == kOpExtInst && n >= 5 && glsl_set && spv[i + 3] == glsl_set) {
      uint32_t& inst = spv[i + 4];
      if (inst == kNMin) inst = kFMin;
      else if (inst == kNMax) inst = kFMax;
      else if (inst == kNClamp) inst = kFClamp;
    }
    i += n;
  }
}

bool lsw_spirv_to_glsl(const uint32_t* words_in, size_t count, uint32_t unit_base, const char* block_suffix,
                       std::string& out, std::vector<LswGlslSampler>& samplers, std::string& err) {
  samplers.clear();
  std::vector<uint32_t> spv(words_in, words_in + count);
  spirv_nan_minmax_to_plain(spv);
  const uint32_t* words = spv.data();
  try {
    spirv_cross::CompilerGLSL glsl(words, count);
    spirv_cross::CompilerGLSL::Options opt;
    opt.version = 430;
    opt.es = false;
    opt.vulkan_semantics = false;
    opt.separate_shader_objects = true;
    opt.enable_420pack_extension = true;
    glsl.set_common_options(opt);
    if (block_suffix && *block_suffix) {
      const auto res = glsl.get_shader_resources();
      for (const auto& r : res.uniform_buffers)
        glsl.set_name(r.base_type_id, glsl.get_name(r.base_type_id) + block_suffix);
    }
    glsl.build_combined_image_samplers();
    uint32_t unit = unit_base;
    for (const auto& c : glsl.get_combined_image_samplers()) {
      LswGlslSampler s;
      s.unit = unit++;
      s.image_binding = glsl.get_decoration(c.image_id, spv::DecorationBinding);
      s.sampler_binding = glsl.get_decoration(c.sampler_id, spv::DecorationBinding);
      glsl.set_decoration(c.combined_id, spv::DecorationBinding, s.unit);
      samplers.push_back(s);
    }
    out = glsl.compile();
    remove_single_pass_main_loop(out);
    return shared_memory_to_texel_buffer(out, err);
  } catch (const std::exception& e) {
    err = e.what();
    // Name the blocks and their member offsets so a layout rejection can be traced to a struct.
    try {
      spirv_cross::Compiler c(words, count);
      auto res = c.get_shader_resources();
      for (auto* list : {&res.uniform_buffers, &res.storage_buffers, &res.push_constant_buffers})
        for (auto& r : *list) {
          const auto& t = c.get_type(r.base_type_id);
          err += "\n  block " + r.name + " (" + c.get_name(r.base_type_id) + "):";
          for (uint32_t i = 0; i < t.member_types.size(); ++i) {
            const auto& mt = c.get_type(t.member_types[i]);
            err += " [" + std::to_string(i) + "]" + c.get_member_name(r.base_type_id, i) + "@" +
                   std::to_string(c.type_struct_member_offset(t, i)) + " vec" +
                   std::to_string(mt.vecsize) + (mt.array.empty() ? "" : " arr" + std::to_string(mt.array[0]) +
                   " stride" + std::to_string(c.type_struct_member_array_stride(t, i)));
          }
        }
    } catch (...) {}
    return false;
  }
}
