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

// OP.1: one present per refresh. The frame loop draws a frame the moment one is due after an idle wait, so input is
// answered at once; but widget timers are independent (playhead, meters, each panel face, the position readout), and
// presenting each one's frame at once gave 85 - 109 presents per second. frameWait() decides whether a frame due at
// `now` must first wait, so that the timers firing meanwhile fold into one frame. All times in seconds on one clock:
//   lastFrame      the previous frame's time (negative: none yet): when the loop started drawing it, or the slot it
//                  was held until (frameTime)
//   lastPass       when FramePacer::wait last returned true, i.e. just after a compositor pass (negative: never)
//   passPeriod     the period of the passes FramePacer::wait waits for (FramePacer::period(); <= 0: no pacer)
//   displayPeriod  the refresh period of the display the window is on (<= 0: unknown, taken as passPeriod)
// A frame at least one period after the previous present is never held back (the first frame after idling).
//
// SYNCED (the display refreshes at the pacer's rate, or faster): waitForPassBeforeFrame().
// - A frame waits for the next pass when it falls into the same pass as the previous present, judged from
//   `lastPass`. A frame due within kPassTolerance of the next pass counts as belonging to it: waiting there would, if
//   the pass has in fact just gone by, skip a whole one (a 2-period gap), whereas a second present in one pass costs
//   only what every frame cost before.
// - With no recent pass time the pacer is asked (which provides one). That is the second frame after a long idle
//   time: it may come up to two periods after the first when a pass went by in between.
// TIMED (the display is slower than the pacer: DwmFlush follows the primary monitor, e.g. 100 Hz, and the window is
// on a 60 Hz one; or there is no pacer): the frame waits until one display period after the previous frame's time.
// Not phase-locked to that display (nothing here knows its vblank), but never more than one present per refresh;
// and frameTime() makes the held frame's time its slot, not the moment the timer returned, so the timer's lateness
// and the draw time do not add up to a lower rate (the frames stay on a grid of one period).
inline constexpr double kPassTolerance = 0.1;  // of a period
inline constexpr double kPassMaxAge = 30.0;    // seconds a pass time predicts later passes (the period is DWM's measured one)
inline constexpr double kSlowerDisplay = 1.2;  // displayPeriod / passPeriod from which the display counts as slower

constexpr bool waitForPassBeforeFrame(double now, double lastFrame, double lastPass, double period) noexcept {
  if (!(period > 0.0) || lastFrame < 0.0 || !(now - lastFrame < period)) return false;
  if (lastPass < 0.0 || !(now - lastPass < kPassMaxAge)) return true;
  // Passes since lastPass (lastFrame may precede it by less than a period: a frame drawn, then a wait).
  const auto passOf = [&](double t) { return static_cast<int64_t>((t - lastPass) / period + kPassTolerance + 1.0); };
  return passOf(now) == passOf(lastFrame);
}

struct FrameWait {
  enum class Kind : uint8_t { None, Pass, Sleep };
  Kind kind = Kind::None;
  double seconds = 0.0;  // Sleep: for how long
};
constexpr FrameWait frameWait(double now, double lastFrame, double lastPass, double passPeriod,
                              double displayPeriod) noexcept {
  const bool timed = !(passPeriod > 0.0) || displayPeriod >= kSlowerDisplay * passPeriod;
  if (!timed) return {waitForPassBeforeFrame(now, lastFrame, lastPass, passPeriod) ? FrameWait::Kind::Pass : FrameWait::Kind::None, 0.0};
  if (!(displayPeriod > 0.0) || lastFrame < 0.0 || !(now - lastFrame < displayPeriod)) return {};
  return {FrameWait::Kind::Sleep, lastFrame + displayPeriod - now};
}
// The time of the frame drawn after `w` (the next call's lastFrame); `now` is the clock after the wait. A frame held
// by time takes its slot, one period after the previous one, unless the timer came back more than half a period late.
constexpr double frameTime(FrameWait w, double lastFrame, double now, double displayPeriod) noexcept {
  if (w.kind != FrameWait::Kind::Sleep) return now;
  const double slot = lastFrame + displayPeriod;
  return now - slot > 0.5 * displayPeriod ? now : slot;
}

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
  // The compositor's refresh period in seconds (1/60 when unknown): DWM's measured period (which predicts later
  // passes from one pass time for many seconds) when it agrees with the nominal rate, else the nominal one. Re-read
  // at most once a second, so a display-mode change is followed; costs a tick-count read otherwise.
  double period() const noexcept;
  // Sleeps the calling thread for `seconds` (clamped to 0 … 0.1) on a high-resolution timer: a kernel wait, no spin.
  void sleep(double seconds) noexcept;

private:
  struct Shared;
  std::shared_ptr<Shared> shared_;
  mutable double period_ = 1.0 / 60.0;
  mutable uint64_t periodReadAt_ = 0;  // GetTickCount64 of the last read (0: never)
  void* timer_ = nullptr;              // sleep(): a high-resolution waitable timer (null: Sleep's granularity)
};

}  // namespace pq::ui
