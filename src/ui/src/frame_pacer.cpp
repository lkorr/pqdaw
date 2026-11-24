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
  timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
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
  if (timer_) CloseHandle(timer_);
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

double FramePacer::period() const noexcept {
  const uint64_t tick = GetTickCount64();
  if (periodReadAt_ != 0 && tick - periodReadAt_ < 1000) return period_;
  periodReadAt_ = tick != 0 ? tick : 1;
  DWM_TIMING_INFO info{};
  info.cbSize = sizeof info;
  // hwnd must be null since Windows 8.1: the composition's own timing.
  if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &info)) && info.rateRefresh.uiNumerator != 0 &&
      info.rateRefresh.uiDenominator != 0) {
    double p = static_cast<double>(info.rateRefresh.uiDenominator) / static_cast<double>(info.rateRefresh.uiNumerator);
    LARGE_INTEGER freq{};
    if (QueryPerformanceFrequency(&freq) && freq.QuadPart > 0 && info.qpcRefreshPeriod != 0) {
      const double measured = static_cast<double>(info.qpcRefreshPeriod) / static_cast<double>(freq.QuadPart);
      if (measured > 0.98 * p && measured < 1.02 * p) p = measured;
    }
    if (p > 1.0 / 1000.0 && p < 1.0 / 20.0) period_ = p;
  }
  return period_;
}

void FramePacer::sleep(double seconds) noexcept {
  if (!(seconds > 0.0)) return;
  seconds = seconds < 0.1 ? seconds : 0.1;
  LARGE_INTEGER due;
  due.QuadPart = -static_cast<LONGLONG>(seconds * 1e7);  // relative, 100 ns units
  if (timer_ != nullptr && due.QuadPart < 0 && SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) {
    WaitForSingleObject(timer_, 200);
    return;
  }
  Sleep(static_cast<DWORD>(seconds * 1e3 + 0.5));
}

}  // namespace pq::ui
