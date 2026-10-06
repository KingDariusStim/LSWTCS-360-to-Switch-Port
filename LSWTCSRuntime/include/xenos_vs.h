// Bridge to the vendored Xenia ShaderInterpreter (LSWTCSRuntime/xenos/). Runs a draw's
// Xenos vertex shader on the CPU, one invocation per index, and returns the exported
// position and interpolators. Used for first real pixels and as a permanent correctness
// oracle for a future GPU shader path (see memory: stale_dump_and_jobs.md §7).
#pragma once
#include <cstdint>

extern "C" {

struct LswXvsVertex {
  float    pos[4];          // oPos export (clip space, pre W-divide)
  float    interp[8][4];    // o0..o7 interpolator exports
  uint32_t interp_mask;     // bit i set if interpolator i was written
  int      has_pos;
  int      killed;          // vertex-kill flag exported
  float    color[4];        // pixel shader oC0 evaluated at this vertex (Gouraud), if a PS was set
  int      has_color;
};

// Optional: the draw's pixel shader. When set (non-null), the PS is run once per unique
// vertex with that vertex's interpolators as its inputs, and oC0 lands in .color.
// Texture fetches return 0 in the interpreter. Pass nullptr to disable.
void lswtcs_xvs_set_ps(const uint32_t* ps_ucode_be, uint32_t ps_dwords);

// regs          full guest register file (>= 0x5003 dwords, host byte order)
// phys_membase  host pointer to guest physical address 0 (g_base + 0x80000000)
// vs_ucode_be   host pointer to the VS microcode in guest memory (big-endian dwords)
// vs_dwords     microcode size in dwords
// draw_initiator / dma_base / dma_size: the DRAW_INDX packet words [1], [2], [3]
// Returns the number of vertices written (one per index, in draw order).
uint32_t lswtcs_xvs_run_draw(const uint32_t* regs, uint8_t* phys_membase,
                             const uint32_t* vs_ucode_be, uint32_t vs_dwords,
                             uint32_t draw_initiator, uint32_t dma_base, uint32_t dma_size,
                             LswXvsVertex* out, uint32_t max_out);
}
