#include "gpu_ringbuf.h"
#include "../../Convert 360/LSWTCS/output/ppc_context.h"   // ppc_fold (LSW_ADDR_FOLD)
#include <cstdio>
extern "C" void dbg_ram(const char* fmt, ...);

// GPU ring-buffer control object field offsets (discovered from sub_822AE698):
//
//   obj + 0x2A90 (10896): uint32  – guest address of the GET register mirror.
//                                   The CP writes the current read offset here
//                                   so the CPU can observe GPU progress without
//                                   an MMIO read.  (Analogous to Xenia's
//                                   RingBuffer::read_ptr_ / CP_RB_RPTR mirror.)
//
//   obj + 0x2A9C (10908): uint32  – current PUT value (write offset, in dwords).
//                                   Equivalent to CP_RB_WPTR in Xenia.
//
// sub_822AE698 spin condition (see loc_822AE730):
//   let rptr_ptr = *(obj+0x2A90)      -- pointer to GET mirror
//   let wptr     = *(obj+0x2A9C)      -- PUT value
//   let rptr     = *rptr_ptr          -- actual GET value
//   spin while (wptr - target) < (wptr - rptr)
//           i.e. while rptr < target
//
// Advancing GET = PUT means wptr - rptr == 0, so the condition is never
// entered regardless of target.

static inline uint32_t rb_load(uint8_t* base, uint32_t addr) {
    return __builtin_bswap32(*reinterpret_cast<volatile uint32_t*>(base + ppc_fold(addr)));
}

static inline void rb_store(uint8_t* base, uint32_t addr, uint32_t val) {
    *reinterpret_cast<volatile uint32_t*>(base + ppc_fold(addr)) = __builtin_bswap32(val);
}

void ppc_ringbuf_advance_get(uint8_t* base, uint32_t gpu_obj_addr) {
    uint32_t get_mirror_addr = rb_load(base, gpu_obj_addr + 10896);
    uint32_t put_val         = rb_load(base, gpu_obj_addr + 10908);

    // Validate the GET mirror pointer before writing.  A zero or host-range
    // address indicates the ring buffer hasn't been set up yet; skip.
    if (get_mirror_addr < 0x80000000u) {
        return;
    }

    rb_store(base, get_mirror_addr, put_val);
}
