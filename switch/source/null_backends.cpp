// Backends not implemented on Switch yet: the diagnostic CPU
// vertex-shader interpreter. The GPU entry points live in gl_backend.cpp (M3).
#include <switch.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include "lsw_gpu_bridge.h"
#include "xenos_vs.h"

// ---- CPU vertex-shader interpreter (diagnostic LSWTCS_XVS path) -------------------------------
void lswtcs_xvs_set_ps(const uint32_t*, uint32_t) {}
uint32_t lswtcs_xvs_run_draw(const uint32_t*, uint8_t*, const uint32_t*, uint32_t, uint32_t, uint32_t, uint32_t,
                             LswXvsVertex*, uint32_t) { return 0; }

// Audio output: audio_switch.cpp (libnx audout).
