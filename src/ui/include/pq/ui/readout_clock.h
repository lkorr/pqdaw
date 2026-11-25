#pragma once

// OP.2: the readout grid. Meters, visualiser faces, analysers, modulation rings and cable glow only show live audio
// data; nobody needs them at the display's rate (30 Hz by default, 60 Hz as a setting: Preferences → Display). Such
// widgets ask for their next poll with Widget::requestReadoutTimer() instead of a period of their own, and every
// one of them gets the SAME next tick of one grid owned by the tree (UiTree::setReadoutRate). So N readouts cause at
// most `rate` frames per second together (independent 1/30 s timers at N phases caused up to N × 30), and a readout
// whose data did not change damages nothing: the frames in between (a playhead at the display's rate) do not rebuild
// it.
//
// The grid is aligned to compositor passes. Its period is a whole number of refresh periods, the one whose rate is
// nearest the target (readoutPasses): at 100 Hz, 30 Hz → every 3rd pass (33.3 Hz) and 60 Hz → every 2nd (50 Hz); at
// 60 Hz → every 2nd (30 Hz) / every pass; at 144 Hz → every 5th (28.8 Hz) / every 2nd (72 Hz). A period that is not a
// multiple (33.3 ms on a 10 ms display) would show readouts alternately 3 and 4 passes apart. Its ticks lie
// kReadoutLead of a refresh period BEFORE a pass, the point furthest from both ways of drawing them:
// - while something else draws every pass (the playhead), frames start just after a pass returned: the tick half a
//   period earlier is due in that frame whatever the pass's jitter, never one frame late;
// - with only readouts animating, the loop wakes at the tick and presents half a period before the pass, so each
//   frame reaches the same pass of its group (a tick AT the pass would land on either side of it).
// Without a pass time (no pacer, a display slower than the pacer, tests) the grid runs free at the same period.
//
// All times are seconds on the tree's clock (UiTree::update). Pure functions; UI thread.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pq::ui {

inline constexpr double kReadoutHzDefault = 30.0;
inline constexpr double kReadoutLead = 0.5;       // of a refresh period: how far before a pass a tick lies
inline constexpr double kReadoutTolerance = 1e-4; // of the grid's period: a tick counts as reached this early (a clock
                                                  // stepped in k / rate must not miss the tick by a rounding error)

// Compositor passes per readout tick: the whole number whose rate is nearest `hz` (>= 1; 1 when either is unknown).
inline uint32_t readoutPasses(double refreshPeriod, double hz) noexcept {
  if (!(refreshPeriod > 0.0) || !(hz > 0.0)) return 1;
  const double n = 1.0 / (refreshPeriod * hz);
  return n < 1.5 ? 1u : static_cast<uint32_t>(std::min(n + 0.5, 1e6));
}

struct ReadoutGrid {
  double period = 1.0 / kReadoutHzDefault;  // seconds between ticks
  double origin = 0.0;                      // one tick's time (the others: origin + k · period)
  bool operator==(const ReadoutGrid&) const = default;
};

// The grid for `hz` given the refresh period (<= 0: unknown, the period is 1 / hz) and the time a compositor pass
// returned (< 0: unknown, the grid keeps its phase). `prev` is the grid until now: the new one continues it, moved
// by less than a refresh period onto the passes, so re-aligning (every frame) never moves or drops a tick; its origin
// is brought near `pass`, so the period's error does not add up over the session.
inline ReadoutGrid readoutGrid(const ReadoutGrid& prev, double hz, double refreshPeriod, double pass) noexcept {
  ReadoutGrid g;
  if (!(hz > 0.0)) hz = kReadoutHzDefault;
  g.period = refreshPeriod > 0.0 ? static_cast<double>(readoutPasses(refreshPeriod, hz)) * refreshPeriod : 1.0 / hz;
  g.origin = prev.origin;
  if (!(refreshPeriod > 0.0) || pass < 0.0) return g;
  const double base = pass - kReadoutLead * refreshPeriod;  // a pass-aligned time
  const double tick = prev.period > 0.0 ? prev.origin + std::round((base - prev.origin) / prev.period) * prev.period : base;
  g.origin = base + std::round((tick - base) / refreshPeriod) * refreshPeriod;
  return g;
}

// When a readout asking at `now` is polled next: the first tick later than `now` (one reached within the tolerance
// counts as now: the timer that fired for it asks for the following one), less the tolerance.
inline double nextReadoutTick(const ReadoutGrid& g, double now) noexcept {
  if (!(g.period > 0.0)) return now;
  const double k = std::floor((now - g.origin) / g.period + kReadoutTolerance) + 1.0;
  return g.origin + (k - kReadoutTolerance) * g.period;
}

}  // namespace pq::ui
