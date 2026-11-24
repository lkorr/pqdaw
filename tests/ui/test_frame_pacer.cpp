// OP.1: the frame loop's "one present per refresh" decision (pq/ui/frame_pacer.h, frameWait / waitForPassBeforeFrame)
// as a pure function of the clock, and a model of the loop on it: independent widget timers must not present more
// often than the display refreshes, and the first frame after idling is never held back.

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

#include "pq/ui/frame_pacer.h"

using namespace pq::ui;

namespace {

constexpr double kP = 1.0 / 60.0;

// The interactive loop over a set of periodic timers (each re-armed from the frame that drew it, like a widget
// timer), on a display whose passes are at k·period + phase. Returns the present times.
struct Model {
  double period = kP;         // the pacer's passes
  double display = 0.0;       // the window's display (0: unknown)
  double phase = 0.0;
  double drawCost = 0.0005;   // updateUi + drawFrame
  double flushLag = 0.0003;   // DwmFlush returns this long after the pass
  bool paced = true;          // false: the loop as it was before OP.1
  std::vector<double> timers; // periods

  std::vector<double> run(double seconds) const {
    std::vector<double> presents;
    std::vector<double> due(timers.size());
    for (size_t i = 0; i < timers.size(); ++i) due[i] = 0.0131 * static_cast<double>(i + 1);  // unrelated phases
    double now = 0.0, lastFrame = -1.0, lastPass = -1.0;
    auto nextPass = [&](double t) { return (std::floor((t - phase) / period) + 1.0) * period + phase; };
    while (now < seconds) {
      // The blocking wait: until the earliest timer (SDL_WaitEventTimeout rounds up to whole ms).
      const double wake = *std::min_element(due.begin(), due.end());
      if (wake > now) now += std::ceil((wake - now) * 1000.0) / 1000.0;
      if (paced) {
        const FrameWait w = frameWait(now, lastFrame, lastPass, period, display);
        if (w.kind == FrameWait::Kind::Pass) {
          now = nextPass(now) + flushLag;
          lastPass = now;
        } else if (w.kind == FrameWait::Kind::Sleep) {
          now += w.seconds + 0.0006;  // the timer's lateness
        }
        lastFrame = frameTime(w, lastFrame, now, display);
      }
      for (size_t i = 0; i < timers.size(); ++i)
        if (due[i] <= now) due[i] = now + timers[i];  // fired: drawn in this frame, re-armed
      now += drawCost;
      presents.push_back(now);
    }
    return presents;
  }
};

// The largest interval between presents from the `from`-th on. The second frame of a run is the one case that may
// come late (under two periods after the first): the first is drawn at once wherever in a pass it falls, no pass time
// is known yet, and the pacer then waits for a pass that may have just gone by. From then on the loop is locked.
double largestGap(const std::vector<double>& p, size_t from = 2) {
  double g = 0.0;
  for (size_t i = std::max<size_t>(from, 1); i < p.size(); ++i) g = std::max(g, p[i] - p[i - 1]);
  return g;
}

}  // namespace

TEST_CASE("frame pacer: the first frame, and a frame a period or more after the last, is never held back") {
  CHECK_FALSE(waitForPassBeforeFrame(5.0, -1.0, -1.0, kP));         // nothing presented yet
  CHECK_FALSE(waitForPassBeforeFrame(5.0, 5.0 - 1.001 * kP, 5.0 - 1.001 * kP, kP)); // one period
  CHECK_FALSE(waitForPassBeforeFrame(9.0, 5.0, 5.0, kP));           // after idling
  CHECK_FALSE(waitForPassBeforeFrame(9.0, 5.0, -1.0, kP));
  CHECK_FALSE(waitForPassBeforeFrame(1.0, 0.999, 0.999, 0.0));      // no period known: never waits
}

TEST_CASE("frame pacer: a second frame in the pass of the last present waits; one in the next pass does not") {
  const double pass = 2.0;  // the pacer returned here; the frame was presented 1 ms later
  const double present = pass + 0.001;
  CHECK(waitForPassBeforeFrame(pass + 0.004, present, pass, kP));
  CHECK(waitForPassBeforeFrame(pass + 0.8 * kP, present, pass, kP));
  // Within the tolerance of the next pass: counted as that pass (a wait there could skip a whole one).
  CHECK_FALSE(waitForPassBeforeFrame(pass + (1.0 - 0.5 * kPassTolerance) * kP, present, pass, kP));
  // An unpaced present late in a pass, then a frame early in the next one: not held back.
  const double late = pass + 0.7 * kP;
  CHECK_FALSE(waitForPassBeforeFrame(pass + 1.05 * kP, late, pass, kP));
  CHECK(waitForPassBeforeFrame(pass + 0.85 * kP, late, pass, kP));
  // A present, then a pacer wait (the loop's "another frame is already due" step): the next frame belongs to the new
  // pass and is drawn without a second wait.
  CHECK_FALSE(waitForPassBeforeFrame(pass + 0.0002, pass - 0.6 * kP, pass, kP));
}

TEST_CASE("frame pacer: without a recent pass time, a frame less than a period after the last asks the pacer") {
  CHECK(waitForPassBeforeFrame(10.004, 10.0, -1.0, kP));
  CHECK(waitForPassBeforeFrame(10.004 + kPassMaxAge, 10.0 + kPassMaxAge, 9.5, kP));
  CHECK_FALSE(waitForPassBeforeFrame(10.004 + kP, 10.0, -1.0, kP));
}

TEST_CASE("frame pacer: independent widget timers fold into one frame per pass (was the union of their ticks)") {
  // The timers of a playing arrangement with faces: the playhead (1/144 s), meters and faces (1/60 s), the device
  // ring (1/30 s), the position readout (4 ms).
  Model m;
  m.timers = {1.0 / 144.0, 1.0 / 60.0, 1.0 / 60.0, 1.0 / 30.0, 0.004};
  m.paced = false;
  const size_t before = m.run(10.0).size();
  CHECK(before > 1000);  // the audit's 85 - 109 per second, and more with the 4 ms readout
  for (const double phase : {0.0, 0.004, 0.0111}) {
    m.paced = true;
    m.phase = phase;
    const std::vector<double> p = m.run(10.0);
    CHECK(p.size() <= 602);
    CHECK(p.size() >= 590);                // and not fewer: every pass gets its frame
    CHECK(largestGap(p) < 1.2 * kP);       // no pass skipped
    CHECK(largestGap(p, 1) < 2.0 * kP);    // the start: see largestGap
  }
}

TEST_CASE("frame pacer: one 60 Hz timer on a 60 Hz display keeps its rate (no skipped passes)") {
  // The hazard of a plain "wait if less than a period since the last present": a timer whose period is the display's
  // fires right around a pass, and a wait just after it skips to the next one (a 2-period gap, 30 fps).
  for (const double phase : {0.0, 0.003, 0.008, 0.013, 0.016}) {
    Model m;
    m.timers = {1.0 / 60.0};
    m.phase = phase;
    const std::vector<double> p = m.run(10.0);
    CHECK(p.size() <= 601);
    CHECK(p.size() >= 540);            // the timer's own rate (its period + the wait's rounding up to whole ms)
    CHECK(largestGap(p) < 1.5 * kP);
  }
}

TEST_CASE("frame pacer: at 144 Hz the same timers present at most once per pass") {
  Model m;
  m.period = 1.0 / 144.0;
  m.timers = {1.0 / 144.0, 1.0 / 60.0, 1.0 / 60.0, 0.004};
  const std::vector<double> p = m.run(5.0);
  CHECK(p.size() <= 5 * 144 + 2);
  CHECK(largestGap(p) < 1.5 * m.period + 0.001);
}

TEST_CASE("frame pacer: a display slower than the pacer's passes is paced by time, one present per refresh") {
  // DwmFlush follows the primary monitor (100 Hz here); the window is on a 60 Hz one.
  const double pass = 0.01;
  CHECK(frameWait(5.004, 5.0, 5.0, pass, kP).kind == FrameWait::Kind::Sleep);
  CHECK(frameWait(5.004, 5.0, 5.0, pass, kP).seconds == doctest::Approx(kP - 0.004));
  CHECK(frameWait(5.0 + 1.001 * kP, 5.0, 5.0, pass, kP).kind == FrameWait::Kind::None);
  CHECK(frameWait(5.004, -1.0, -1.0, pass, kP).kind == FrameWait::Kind::None);
  // The same or a faster display, or an unknown one: the pacer's passes.
  CHECK(frameWait(5.004, 5.0, 5.0, kP, kP).kind == FrameWait::Kind::Pass);
  CHECK(frameWait(5.004, 5.0, 5.0, kP, 1.0 / 59.94).kind == FrameWait::Kind::Pass);
  CHECK(frameWait(5.004, 5.0, 5.0, kP, 0.01).kind == FrameWait::Kind::Pass);
  CHECK(frameWait(5.004, 5.0, 5.0, kP, 0.0).kind == FrameWait::Kind::Pass);
  // No pacer: by time when the display is known, unpaced otherwise.
  CHECK(frameWait(5.004, 5.0, -1.0, 0.0, kP).kind == FrameWait::Kind::Sleep);
  CHECK(frameWait(5.004, 5.0, -1.0, 0.0, 0.0).kind == FrameWait::Kind::None);

  Model m;
  m.period = pass;
  m.display = kP;
  m.timers = {1.0 / 144.0, 1.0 / 60.0, 1.0 / 60.0, 1.0 / 30.0, 0.004};
  const std::vector<double> p = m.run(10.0);
  CHECK(p.size() <= 601);
  CHECK(p.size() >= 598);  // on the grid: the timer's lateness and the draw time do not lower the rate
  CHECK(largestGap(p) < 1.2 * kP);
  // frameTime: a held frame takes its slot; one whose timer came back very late, or that was not held, the clock.
  const FrameWait held{FrameWait::Kind::Sleep, 0.004};
  CHECK(frameTime(held, 5.0, 5.0 + kP + 0.001, kP) == 5.0 + kP);
  CHECK(frameTime(held, 5.0, 5.0 + 1.6 * kP, kP) == 5.0 + 1.6 * kP);
  CHECK(frameTime({}, 5.0, 7.0, kP) == 7.0);
  CHECK(frameTime({FrameWait::Kind::Pass, 0.0}, 5.0, 5.01, kP) == 5.01);
}

TEST_CASE("frame pacer: sleep is a timed kernel wait") {
  FramePacer pacer;
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < 10; ++i) pacer.sleep(0.005);
  const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  CHECK(s >= 0.045);
  CHECK(s < 1.0);  // loose: a loaded machine
  pacer.sleep(0.0);
  pacer.sleep(-1.0);
}

TEST_CASE("frame pacer: the pacer reports a plausible refresh period") {
  FramePacer pacer;
  const double p = pacer.period();
  CHECK(p > 1.0 / 1000.0);
  CHECK(p < 1.0 / 20.0);
  CHECK(pacer.period() == p);  // cached
}
