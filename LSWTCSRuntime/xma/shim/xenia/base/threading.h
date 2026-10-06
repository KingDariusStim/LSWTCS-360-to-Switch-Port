// LSWTCS shim: shadows xenia/base/threading.h for the vendored XMA context code (the real header
// needs the third_party/date submodule). Only what XmaContext uses: an auto-reset Event + Wait.
#pragma once
#include <windows.h>
#include <memory>
#include "xenia/base/mutex.h"
#include <cstdio>
#include <filesystem>
#include <string_view>
namespace xe { namespace filesystem {
// Defined in xenos_dxbc/lsw_dxbc_stubs.cc (used only by XmaContext::DumpRaw).
FILE* OpenFile(const std::filesystem::path& path, const std::string_view mode);
} }
namespace xe {
namespace threading {
enum class WaitResult { kSuccess, kUserCallback, kTimeout, kAbandoned, kFailed };
class Event {
 public:
  static std::unique_ptr<Event> CreateAutoResetEvent(bool initial_state) {
    return std::unique_ptr<Event>(new Event(CreateEvent(nullptr, FALSE, initial_state, nullptr)));
  }
  static std::unique_ptr<Event> CreateManualResetEvent(bool initial_state) {
    return std::unique_ptr<Event>(new Event(CreateEvent(nullptr, TRUE, initial_state, nullptr)));
  }
  ~Event() { if (h_) CloseHandle(h_); }
  void Set() { SetEvent(h_); }
  void Reset() { ResetEvent(h_); }
  HANDLE native_handle() const { return h_; }
 private:
  explicit Event(HANDLE h) : h_(h) {}
  HANDLE h_;
};
inline WaitResult Wait(Event* e, bool /*is_alertable*/) {
  return WaitForSingleObject(e->native_handle(), INFINITE) == WAIT_OBJECT_0 ? WaitResult::kSuccess : WaitResult::kFailed;
}
}  // namespace threading
}  // namespace xe
