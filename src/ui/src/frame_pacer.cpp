#include "pq/ui/frame_pacer.h"

#include <windows.h>
#include <dwmapi.h>

#include <atomic>
#include <system_error>
#include <thread>

namespace pq::ui {

// Owned jointly by the pacer and its helper thread: if the helper is stuck inside DwmFlush when the pacer
// is destroyed, it is detached and keeps the events alive until it wakes and sees `quit`.
struct FramePacer::Shared {
  HANDLE request = nullptr;  // auto-reset: one flush per request
  HANDLE done = nullptr;     // auto-reset: one wake per flush
  std::atomic<bool> quit{false};
  std::atomic<uint32_t> timeouts{0};
  ~Shared() {
    if (request) CloseHandle(request);
    if (done) CloseHandle(done);
  }
};

FramePacer::FramePacer() {
  BOOL composition = FALSE;
  if (FAILED(DwmIsCompositionEnabled(&composition)) || !composition) return;
  auto s = std::make_shared<Shared>();
  s->request = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  s->done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!s->request || !s->done) return;
  try {
    std::thread([s] {
      for (;;) {
        WaitForSingleObject(s->request, INFINITE);
        if (s->quit.load(std::memory_order_acquire)) return;
        // A failed flush (composition toggled off, session locked) must not become a spin.
        if (FAILED(DwmFlush())) Sleep(15);
        SetEvent(s->done);
      }
    }).detach();
  } catch (const std::system_error&) {
    return;  // no helper thread: pacing unavailable, frames run unpaced rather than hang
  }
  shared_ = std::move(s);
}

FramePacer::~FramePacer() {
  if (!shared_) return;
  shared_->quit.store(true, std::memory_order_release);
  SetEvent(shared_->request);
  // The thread holds its own reference to Shared, so nothing to wait for here.
}

bool FramePacer::available() const noexcept { return shared_ != nullptr; }

bool FramePacer::wait(uint32_t timeoutMs) noexcept {
  if (!shared_) return false;
  SetEvent(shared_->request);
  if (WaitForSingleObject(shared_->done, timeoutMs) == WAIT_OBJECT_0) return true;
  shared_->timeouts.fetch_add(1, std::memory_order_relaxed);
  return false;
}

uint32_t FramePacer::timeouts() const noexcept { return shared_ ? shared_->timeouts.load(std::memory_order_relaxed) : 0; }

}  // namespace pq::ui
