// Shim for xenia/memory.h (the emulator's Memory class), used only by the vendored
// xenia/gpu/shader_interpreter.cc, which needs physical_membase() for vertex fetches.
// In the recompiled runtime, guest physical address P lives at g_base + 0x80000000 + P,
// so physical_membase = g_base + 0x80000000 (set by the bridge per draw).
#ifndef LSWTCS_XENIA_MEMORY_SHIM_H_
#define LSWTCS_XENIA_MEMORY_SHIM_H_
#include <cstdint>

namespace xe {
class Memory {
 public:
  uint8_t* physical_membase_ = nullptr;
  uint8_t* physical_membase() const { return physical_membase_; }
  template <typename T = uint8_t*>
  T TranslatePhysical(uint32_t guest_address) const {
    return reinterpret_cast<T>(physical_membase_ + (guest_address & 0x1FFFFFFF));
  }
};
}  // namespace xe

#endif
