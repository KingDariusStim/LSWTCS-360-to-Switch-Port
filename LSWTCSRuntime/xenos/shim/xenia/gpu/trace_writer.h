// Shim for xenia/gpu/trace_writer.h: the interpreter only calls WriteMemoryRead when a
// trace writer is attached, which the bridge never does.
#ifndef LSWTCS_XENIA_TRACE_WRITER_SHIM_H_
#define LSWTCS_XENIA_TRACE_WRITER_SHIM_H_
#include <cstddef>
#include <cstdint>

namespace xe {
namespace gpu {
class TraceWriter {
 public:
  void WriteMemoryRead(uint32_t, size_t) {}
};
}  // namespace gpu
}  // namespace xe

#endif
