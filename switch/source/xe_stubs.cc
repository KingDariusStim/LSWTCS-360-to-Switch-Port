// Xenia base facilities the vendored XMA code links against (Switch counterpart of the logging part
// of LSWTCSRuntime/xenos_dxbc/lsw_dxbc_stubs.cc): logging is compiled in but disabled.
#include <cstddef>
#include <cstdint>
#include <utility>
#include "xenia/base/logging.h"

namespace xe {
namespace logging {
bool ShouldLog(LogLevel, uint32_t) { return false; }
namespace internal {
uint32_t GetLogLevel() { return 0; }
std::pair<char*, size_t> GetThreadBuffer() {
  static thread_local char buf[8192];
  return {buf, sizeof(buf)};
}
void AppendLogLine(LogLevel, const char, size_t) {}
}  // namespace internal
}  // namespace logging
}  // namespace xe
