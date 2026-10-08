// Backends not implemented on Switch yet: audio output (audren in M4) and the diagnostic CPU
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

// ---- audio output (host_audio.cpp) ------------------------------------------------------------
// false = no host voice: kernel_stubs' audio worker then paces frames on its own timer (PC path).
bool host_audio_open(int, void (*)(int)) { return false; }
void host_audio_submit(int, const uint8_t*) {}
void host_audio_close(int) {}
