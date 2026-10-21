#pragma once

// Event-driven file change notification for the window loop (T3.7 review: idle must do no periodic work, also in dev
// builds). A helper thread blocks on the directory's change-notification handle (FindFirstChangeNotificationW) and,
// when something in the directory is written, pushes one SDL user event (at most one queued at a time). The loop
// then re-checks the file (pq::ui::ThemeFileWatcher compares its write time). Nothing runs while nothing changes.
//
// UI thread owns the object; the helper thread only waits and calls SDL_PushEvent (thread-safe).

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace pq::app {

class FileChangeNotifier {
public:
  // Watches the directory containing `fileUtf8`. `eventType`: an SDL user event type (SDL_RegisterEvents). Returns
  // null where change notifications are unavailable (the caller then has no hot reload).
  static std::unique_ptr<FileChangeNotifier> create(const std::string& fileUtf8, uint32_t eventType);
  ~FileChangeNotifier();
  FileChangeNotifier(const FileChangeNotifier&) = delete;
  FileChangeNotifier& operator=(const FileChangeNotifier&) = delete;

  uint32_t eventType() const noexcept { return eventType_; }
  // The UI thread took the event: the next change may queue another one.
  void acknowledge() noexcept { queued_.store(false, std::memory_order_release); }

private:
  FileChangeNotifier() = default;
  void run();
  void* change_ = nullptr;  // HANDLE from FindFirstChangeNotificationW
  void* stop_ = nullptr;    // HANDLE: manual-reset event
  uint32_t eventType_ = 0;
  std::atomic<bool> queued_{false};
  std::thread thread_;
};

}  // namespace pq::app
