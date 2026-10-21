#include "file_watch.h"

#include <SDL3/SDL.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <filesystem>

namespace pq::app {

std::unique_ptr<FileChangeNotifier> FileChangeNotifier::create(const std::string& fileUtf8, uint32_t eventType) {
#ifdef _WIN32
  if (eventType == 0) return nullptr;
  const std::filesystem::path dir =
      std::filesystem::path(std::u8string(fileUtf8.begin(), fileUtf8.end())).parent_path();
  HANDLE change = FindFirstChangeNotificationW(dir.c_str(), FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME);
  if (change == INVALID_HANDLE_VALUE) return nullptr;
  HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!stop) {
    FindCloseChangeNotification(change);
    return nullptr;
  }
  std::unique_ptr<FileChangeNotifier> n(new FileChangeNotifier());
  n->change_ = change;
  n->stop_ = stop;
  n->eventType_ = eventType;
  n->thread_ = std::thread([p = n.get()] { p->run(); });
  return n;
#else
  (void)fileUtf8;
  (void)eventType;
  return nullptr;
#endif
}

FileChangeNotifier::~FileChangeNotifier() {
#ifdef _WIN32
  if (stop_) SetEvent(static_cast<HANDLE>(stop_));
  if (thread_.joinable()) thread_.join();
  if (change_) FindCloseChangeNotification(static_cast<HANDLE>(change_));
  if (stop_) CloseHandle(static_cast<HANDLE>(stop_));
#endif
}

void FileChangeNotifier::run() {
#ifdef _WIN32
  SetThreadDescription(GetCurrentThread(), L"pqdaw file watch");
  const HANDLE handles[2] = {static_cast<HANDLE>(stop_), static_cast<HANDLE>(change_)};
  for (;;) {
    const DWORD r = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
    if (r != WAIT_OBJECT_0 + 1) return;  // stop requested (or an error)
    if (!queued_.exchange(true, std::memory_order_acq_rel)) {
      SDL_Event e{};
      e.type = eventType_;
      e.user.timestamp = SDL_GetTicksNS();
      if (!SDL_PushEvent(&e)) queued_.store(false, std::memory_order_release);
    }
    if (!FindNextChangeNotification(handles[1])) return;
  }
#endif
}

}  // namespace pq::app
