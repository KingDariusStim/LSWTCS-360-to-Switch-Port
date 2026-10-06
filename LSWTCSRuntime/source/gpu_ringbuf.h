#pragma once
#include <cstdint>

// GPU ring-buffer GET (read pointer) advancement.
//
// On Xbox 360 hardware the Command Processor (CP) reads PM4 packets out of a
// ring buffer in EDRAM-adjacent memory and advances the GET register as it
// goes.  The CPU spins in sub_822AE698 waiting for GET >= target before
// submitting more work.
//
// In this port a real CP thread is not implemented; rendering is handled by
// the platform-specific backend (D3D12 on Windows, future NVN/OpenGL on
// Switch).  We therefore advance GET = PUT immediately whenever the game
// would otherwise spin, so initialization and frame submission proceed
// without blocking.
//
// These functions are pure guest-memory operations and contain no
// platform-specific code, keeping the Switch port path open.

// Called at the entry of sub_822AE698 before any spin check.
// Reads PUT from the GPU object and stores it to the GET mirror so the
// spin condition is satisfied on the first check.
//   base         – host pointer to the 4 GB guest address space
//   gpu_obj_addr – guest address of the GPU ring-buffer control object (r31)
void ppc_ringbuf_advance_get(uint8_t* base, uint32_t gpu_obj_addr);
