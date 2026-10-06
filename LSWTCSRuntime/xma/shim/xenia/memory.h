// LSWTCS shim: shadows xenia/memory.h for the vendored XMA context code. Only the guest-address
// translation it uses. Guest virtual = g_base + va; physical p = the 0x80000000 aliased view.
#pragma once
#include <cstdint>
#include "xenia/base/memory.h"
namespace xe {
class Memory {
 public:
  explicit Memory(uint8_t* base) : base_(base) {}
  template <typename T = uint8_t*>
  T TranslateVirtual(uint32_t guest_address) const {
    return reinterpret_cast<T>(base_ + guest_address);
  }
  // In this runtime MmGetPhysicalAddress is the identity ("virt == phys"), so the buffer pointers
  // the game stores in XMA contexts are full guest VIRTUAL addresses — including heap buffers at
  // 0x4xxxxxxx. Xenia's (p & 0x1FFFFFFF) + 0x80000000 mapping turned those into addresses inside the
  // XEX image (0x82xxxxxx) and decoded PCM overwrote game constants (1.0f -> NaN -> main-loop hang).
  // Use the address as-is; only a bare physical value (< 0x20000000) goes through the 0x80000000 view.
  template <typename T = uint8_t*>
  T TranslatePhysical(uint32_t guest_address) const {
    return reinterpret_cast<T>(base_ + (guest_address < 0x20000000u ? 0x80000000u + guest_address
                                                                   : guest_address));
  }
 private:
  uint8_t* base_;
};
}  // namespace xe
