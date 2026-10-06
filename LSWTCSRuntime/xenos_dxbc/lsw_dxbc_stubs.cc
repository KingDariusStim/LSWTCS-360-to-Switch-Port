// Minimal definitions for Xenia base/GPU facilities the vendored DXBC shader translator links
// against but our runtime does not otherwise use (logging, filesystem, trace writer, one cvar).
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/gpu/trace_writer.h"
#include "xenia/base/console.h"
#include "xenia/base/system.h"

DEFINE_bool(draw_resolution_scaled_texture_offsets, true,
            "Apply resolution-scale-aware texture fetch offsets.", "GPU");

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
namespace filesystem {
FILE* OpenFile(const std::filesystem::path& path, const std::string_view mode) {
  return _wfopen(path.c_str(), std::wstring(mode.begin(), mode.end()).c_str());
}
}  // namespace filesystem
std::string path_to_utf8(const std::filesystem::path& path) { return path.string(); }

namespace amd64 {
uint64_t GetFeatureFlags() { return 0; }  // conservative: no optional ISA paths
}  // namespace amd64
bool has_console_attached() { return true; }
void AttachConsole() {}
void ShowSimpleMessageBox(SimpleMessageBoxType, std::string_view message) {
  std::fwrite(message.data(), 1, message.size(), stderr);
}
namespace memory {
bool IsWritableExecutableMemorySupported() { return false; }
}  // namespace memory
#if XE_ENABLE_TRACE_WRITER_INSTRUMENTATION == 1
namespace gpu {
void TraceWriter::WriteMemoryRead(uint32_t, size_t, const void*) {}
}  // namespace gpu
#endif
}  // namespace xe
