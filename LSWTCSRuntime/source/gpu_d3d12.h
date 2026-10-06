#pragma once
#include <cstdint>

// Initialize the D3D12 rendering window. Call from main() before _xstart().
void gpu_d3d12_init();

// Present a guest-memory framebuffer to the window.
//   base       : g_base (host address of the 4 GB guest memory allocation)
//   phys_addr  : guest physical address of the first pixel
//   row_pitch  : bytes per row in guest memory (must be 256-byte aligned)
//   width/height: pixel dimensions
// If the framebuffer is all zeros (pre-GPU), a live test pattern is shown instead.
void gpu_d3d12_present(uint8_t* base, uint32_t phys_addr,
                       uint32_t row_pitch, uint32_t width, uint32_t height);

// Pump window messages. Returns false when the window is closed.
bool gpu_d3d12_poll();
