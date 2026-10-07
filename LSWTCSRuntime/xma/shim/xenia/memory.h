// LSWTCS shim: shadows xenia/memory.h for the vendored XMA context code. Only the guest-address
// translation it uses. Guest virtual = g_base + va; physical p = the 0x80000000 aliased view.
#pragma once
#include <cstdint>
#include "xenia/base/memory.h"
// Per-512 MB-region host base (main.cpp; PPC_HOST in ppc_context.h): folds the 0xA0000000 /
// 0xC0000000 physical views onto the 0x80000000 copy when LSW_ADDR_FOLD is on.
extern "C" uint8_t* ppc_view_base[8];
namespace xe {
class Memory {
 public:
  explicit Memory(uint8_t* base) : base_(base) {}
  template <typename T = uint8_t*>
  T TranslateVirtual(uint32_t guest_address) const {
    return reinterpret_cast<T>(ppc_view_base[guest_address >> 29] + guest_address);
  }
  // In this runtime MmGetPhysicalAddress is the identity ("virt == phys"), so the buffer pointers
  // the game stores in XMA contexts are full guest VIRTUAL addresses — including heap buffers at
  // 0x4xxxxxxx. Xenia's (p & 0x1FFFFFFF) + 0x80000000 mapping turned those into addresses inside the
  // XEX image (0x82xxxxxx) and decoded PCM overwrote game constants (1.0f -> NaN -> main-loop hang).
  // Use the address as-is; only a bare physical value (< 0x20000000) goes through the 0x80000000 view.
  template <typename T = uint8_t*>
  T TranslatePhysical(uint32_t guest_address) const {
    const uint32_t a = guest_address < 0x20000000u ? 0x80000000u + guest_address : guest_address;
    return reinterpret_cast<T>(ppc_view_base[a >> 29] + a);
  }
 private:
  uint8_t* base_;
};
}  // namespace xe
