#pragma once

// Frame pacing on the compositor instead of driver vsync (SPEC §11, PQ Meter main.cpp / window_chrome_win.cpp).
//
// Blocking vsync (FIFO present) lets the NVIDIA driver busy-wait for the vblank; PQ Meter measured a whole
// core burned that way. Instead the swapchain presents without blocking (MAILBOX/IMMEDIATE) and the frame
// loop waits for DwmFlush(), a real kernel wait that returns after the compositor's next pass.
//
// DwmFlush has no timeout and blocks indefinitely when the compositor is idle (e.g. a surface promoted to
// direct scanout / an MPO plane), so it runs on a helper thread and wait() bounds it. Unlike PQ Meter's
// free-running pump, the helper only flushes on request, so an idle UI (no frames) wakes no threads at all.

#include <cstdint>
#include <memory>

namespace pq::ui {

class FramePacer {
public:
  FramePacer();
  ~FramePacer();
  FramePacer(const FramePacer&) = delete;
  FramePacer& operator=(const FramePacer&) = delete;

  // True when DWM composition is on (always on Windows 8+) and the helper thread is running.
  bool available() const noexcept;
  // Waits for the next composition pass, at most timeoutMs. Returns false on timeout or when unavailable
  // (the caller then simply continues: a missed pass costs one unpaced frame, never a hang).
  bool wait(uint32_t timeoutMs = 17) noexcept;
  // Number of waits that hit the timeout (diagnostics).
  uint32_t timeouts() const noexcept;

private:
  struct Shared;
  std::shared_ptr<Shared> shared_;
};

}  // namespace pq::ui
