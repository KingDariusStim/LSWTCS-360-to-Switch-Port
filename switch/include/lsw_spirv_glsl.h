// SPIR-V -> GLSL 4.30 (switch-mesa) conversion; see switch/source/lsw_spirv_glsl.cc.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Texture unit of the R32UI buffer texture over the 512 MB shared memory (guest physical memory),
// which replaces the translator's storage-buffer array (lsw_spirv_glsl.cc). Clear of the shaders' own
// units (pixel 0-15, vertex 16-31).
enum : uint32_t { kLswShmTextureUnit = 40 };

// One GL texture unit of a converted shader: Xenia's translator declares separate images and
// samplers (Vulkan style); GL needs them combined, one unit per (image, sampler) pair used.
struct LswGlslSampler {
  uint32_t unit;             // GL texture unit (layout(binding) of the combined sampler)
  uint32_t image_binding;    // Xenia texture binding index (Shader::GetTextureBindingsAfterTranslation)
  uint32_t sampler_binding;  // raw SPIR-V binding of the sampler (= texture count + sampler index)
};

// unit_base: first GL texture unit for this stage (VS and PS of one program must not overlap).
// block_suffix: appended to every uniform block name. GL links same-named blocks across stages and
// requires them identical, but Xenia's stages declare differently sized blocks under one name
// (XeFloatConstants per stage, a shorter XeSystemConstants in geometry shaders); bindings stay as-is.
// Returns false with a reason in err when SPIRV-Cross rejects the module.
bool lsw_spirv_to_glsl(const uint32_t* words, size_t count, uint32_t unit_base, const char* block_suffix,
                       std::string& out, std::vector<LswGlslSampler>& samplers, std::string& err);

// Geometry shaders: an early `return` (Xenia's generator returns when an input position is NaN or a
// point has no size) hangs Eden's shader recompiler on mesa's code (2026-10-07, switch/source
// gl_backend.cpp LSWTCS_GLTEST ladder). Rewrites main()'s body as `do { ... } while (false);` with
// each `return;` turned into `break;`. Only valid without loops in main (the generator emits none);
// returns false and leaves the source alone otherwise.
bool lsw_glsl_gs_remove_returns(std::string& glsl);
