// Xenia base/GPU facilities the vendored SPIR-V shader translator links against (Switch counterpart
// of the non-logging part of LSWTCSRuntime/xenos_dxbc/lsw_dxbc_stubs.cc; logging is in xe_stubs.cc).
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include "xenia/base/cvar.h"

DEFINE_bool(draw_resolution_scaled_texture_offsets, true,
            "Apply resolution-scale-aware texture fetch offsets.", "GPU");

namespace xe {
std::string path_to_utf8(const std::filesystem::path& path) { return path.string(); }
namespace filesystem {
FILE* OpenFile(const std::filesystem::path& path, const std::string_view mode) {
  return std::fopen(path.c_str(), std::string(mode).c_str());
}
}  // namespace filesystem
}  // namespace xe
