// OP.2: the readout grid (pq/ui/readout_clock.h): one shared, pass-aligned tick for every widget that only shows live
// audio data. The pure functions (passes per tick at 60 / 100 / 144 Hz, the shared phase, alignment to compositor
// passes and its stability under jitter and drift), Widget::requestReadoutTimer through a real tree (N readouts at
// unrelated phases cause at most `rate` frames per second together; the rate applies live), and a model of the
// frame loop with a playhead at the display's rate (readouts poll every n-th pass exactly, never a frame late).

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "pq/ui/frame_pacer.h"
#include "pq/ui/readout_clock.h"
#include "pq/ui/test_driver.h"
#include "pq/ui/widget.h"

using namespace pq::ui;

namespace {

// A readout: polls on the tree's grid, and every poll changes what it shows (the worst case: a frame per poll).
class Readout : public Widget {
public:
  using Widget::Widget;
  void start() { timer_ = requestReadoutTimer(); }
  void onTimer(uint32_t id) override {
    if (id != timer_) return;
    polls.push_back(tree()->now());
    markDirty();
    timer_ = requestReadoutTimer();
  }
  std::vector<double> polls;

private:
  uint32_t timer_ = 0;
};

// N readouts started at unrelated times; the loop steps every `dt` for `seconds`. Returns the frames drawn.
struct Bank {
  TestDriver d{400, 100};
  std::vector<Readout*> r;
  explicit Bank(int n) {
    Widget& root = d.setRoot<Widget>();
    root.setLayout(LayoutKind::Row);
    for (int i = 0; i < n; ++i) r.push_back(&root.add<Readout>("r" + std::to_string(i)));
    d.tree().layoutNow();
    d.step();
  }
  // Starts them 3.7 ms apart (so independent timers would be at n phases), then runs.
  uint64_t run(double seconds, double dt = 0.001) {
    d.setFrameRate(1.0 / dt);
    for (Readout* x : r) {
      x->start();
      d.advance(0.0037);
    }
    d.advance(0.2);  // every one is on the grid
    for (Readout* x : r) x->polls.clear();
    const uint64_t before = d.framesDrawn();
    d.advance(seconds);
    return d.framesDrawn() - before;
  }
};

double frac(double x) { return x - std::floor(x); }

}  // namespace

TEST_CASE("readout grid: passes per tick are the whole number nearest the target rate") {
  CHECK(readoutPasses(1.0 / 60.0, 30.0) == 2);   // 30 Hz
  CHECK(readoutPasses(1.0 / 60.0, 60.0) == 1);   // 60 Hz
  CHECK(readoutPasses(1.0 / 100.0, 30.0) == 3);  // 33.3 Hz
  CHECK(readoutPasses(1.0 / 100.0, 60.0) == 2);  // 50 Hz
  CHECK(readoutPasses(1.0 / 144.0, 30.0) == 5);  // 28.8 Hz
  CHECK(readoutPasses(1.0 / 144.0, 60.0) == 2);  // 72 Hz
  CHECK(readoutPasses(1.0 / 120.0, 30.0) == 4);
  CHECK(readoutPasses(1.0 / 240.0, 60.0) == 4);
  CHECK(readoutPasses(1.0 / 30.0, 60.0) == 1);   // a display slower than the target: every pass
  CHECK(readoutPasses(0.0, 30.0) == 1);
  CHECK(readoutPasses(1.0 / 60.0, 0.0) == 1);
  // The period is that many refresh periods; without a refresh period it is 1 / rate.
  for (const double hz : {60.0, 100.0, 144.0})
    for (const double rate : {30.0, 60.0}) {
      const ReadoutGrid g = readoutGrid({}, rate, 1.0 / hz, 0.25);
      CHECK(g.period == doctest::Approx(readoutPasses(1.0 / hz, rate) / hz));
    }
  CHECK(readoutGrid({}, 30.0, 0.0, -1.0).period == doctest::Approx(1.0 / 30.0));
  CHECK(readoutGrid({}, 60.0, 0.0, -1.0).period == doctest::Approx(1.0 / 60.0));
}

TEST_CASE("readout grid: everyone asking between two ticks gets the same next tick, later than now") {
  const ReadoutGrid g = readoutGrid({}, 30.0, 0.01, 5.0003);
  const double t0 = nextReadoutTick(g, 7.0);
  CHECK(t0 > 7.0);
  CHECK(t0 - 7.0 <= g.period);
  for (double now = t0 + 1e-5; now < t0 + g.period - 1e-5; now += 0.0007) {
    CHECK(nextReadoutTick(g, now) == doctest::Approx(t0 + g.period).epsilon(1e-12));
    CHECK(nextReadoutTick(g, now) > now);
  }
  // The timer that fired for a tick asks for the following one, also when the clock is a rounding error short of it.
  CHECK(nextReadoutTick(g, t0) == doctest::Approx(t0 + g.period).epsilon(1e-12));
  // (t0 is the tick less the tolerance: the tick itself is t0 + tolerance · period.)
  const double tick = t0 + kReadoutTolerance * g.period;
  CHECK(nextReadoutTick(g, std::nextafter(tick, 0.0)) == doctest::Approx(t0 + g.period).epsilon(1e-12));
  CHECK(nextReadoutTick(g, tick) == doctest::Approx(t0 + g.period).epsilon(1e-12));
  // A clock stepped in k / rate reaches every tick of a free grid at its step (no tick is a step late).
  const ReadoutGrid free{};
  double due = nextReadoutTick(free, 0.0);
  for (int k = 1; k <= 3000; ++k) {
    const double now = static_cast<double>(k) / 30.0;
    REQUIRE(due <= now);
    due = nextReadoutTick(free, now);
    REQUIRE(due > now);
  }
}

TEST_CASE("readout grid: ticks lie half a refresh period before a pass, and re-aligning never moves one by a pass") {
  for (const double hz : {60.0, 100.0, 144.0})
    for (const double rate : {30.0, 60.0}) {
      CAPTURE(hz);
      CAPTURE(rate);
      const double T = 1.0 / hz;
      const double phase = 0.0031;  // passes at phase + k·T
      ReadoutGrid g = readoutGrid({}, rate, T, phase + 12.0 * T);
      // Every tick is (k + 1/2)·T away from a pass.
      for (double now = 0.3; now < 0.5; now += 0.0123) {
        const double t = nextReadoutTick(g, now) + kReadoutTolerance * g.period;
        CHECK(frac((t - phase) / T) == doctest::Approx(1.0 - kReadoutLead).epsilon(1e-6));
      }
      // Re-aligned at every pass for a minute, with ±0.2·T jitter in when the pass was seen and a display whose real
      // period is 50 ppm off the one the grid is told: the ticks stay the same passes' (each one n passes after the
      // last), and stay half a period before them.
      const double real = T * (1.0 + 50e-6);
      const uint32_t n = readoutPasses(T, rate);
      double last = -1.0;
      int64_t lastPassIndex = -1;
      uint32_t ticks = 0;
      for (int64_t k = 13; k < static_cast<int64_t>(60.0 * hz); ++k) {
        const double pass = phase + static_cast<double>(k) * real;
        const double jitter = 0.2 * T * std::sin(static_cast<double>(k) * 1.7);
        g = readoutGrid(g, rate, T, pass + jitter);
        const double t = nextReadoutTick(g, pass) + kReadoutTolerance * g.period;  // the tick after this pass
        if (last >= 0.0 && t - last < 0.5 * T) continue;                            // still the same tick
        const double passes = (t - phase) / real;  // in real passes
        const int64_t index = static_cast<int64_t>(std::floor(passes));
        // 0.2·T of jitter around the half-period lead: between 0.3 and 0.7 of a pass, never across one.
        REQUIRE(frac(passes) > 0.25);
        REQUIRE(frac(passes) < 0.75);
        if (lastPassIndex >= 0) REQUIRE(index - lastPassIndex == static_cast<int64_t>(n));
        lastPassIndex = index;
        last = t;
        ++ticks;
      }
      CHECK(ticks >= static_cast<uint32_t>(59.0 * hz / n));
    }
}

TEST_CASE("readout grid: a new rate or refresh period takes over at once; an unknown pass keeps the phase") {
  ReadoutGrid g = readoutGrid({}, 30.0, 0.01, 2.0);
  CHECK(g.period == doctest::Approx(0.03));
  const ReadoutGrid h = readoutGrid(g, 60.0, 0.01, 2.0);
  CHECK(h.period == doctest::Approx(0.02));
  CHECK(frac((h.origin - 2.0) / 0.01) == doctest::Approx(0.5));
  const ReadoutGrid f = readoutGrid(g, 30.0, 1.0 / 60.0, -1.0);  // moved to a 60 Hz display, paced by time
  CHECK(f.period == doctest::Approx(2.0 / 60.0));
  CHECK(f.origin == g.origin);
}

TEST_CASE("readout timers: N readouts at unrelated phases draw at most `rate` frames per second together") {
  {
    Bank b(8);  // a free grid (no display known): 30 Hz
    const uint64_t frames = b.run(2.0);
    CHECK(frames <= 61);
    CHECK(frames >= 59);
    for (const Readout* x : b.r) {
      CHECK(x->polls.size() >= 59);
      CHECK(x->polls.size() <= 61);
      CHECK(x->polls == b.r[0]->polls);  // all polled in the same frames
    }
  }
  {
    Bank b(8);
    b.d.tree().setReadoutRate(60.0);
    CHECK(b.d.tree().readoutRate() == 60.0);
    const uint64_t frames = b.run(2.0);
    CHECK(frames <= 121);
    CHECK(frames >= 119);
  }
  {
    Bank b(8);  // a 100 Hz display: every 3rd pass
    b.d.tree().setReadoutRefresh(0.01, 0.0042);
    const uint64_t frames = b.run(3.0);
    CHECK(frames <= 101);
    CHECK(frames >= 99);
    b.d.tree().setReadoutRate(60.0);  // live: every 2nd pass from the next poll on
    b.d.advance(0.1);
    const uint64_t before = b.d.framesDrawn();
    b.d.advance(2.0);
    CHECK(b.d.framesDrawn() - before <= 101);
    CHECK(b.d.framesDrawn() - before >= 99);
    b.d.tree().setReadoutRate(30.0);
    b.d.advance(0.1);
    const uint64_t again = b.d.framesDrawn();
    b.d.advance(3.0);
    CHECK(b.d.framesDrawn() - again <= 101);
    CHECK(b.d.framesDrawn() - again >= 99);
  }
  {
    Bank b(3);  // 144 Hz: every 5th pass (28.8 Hz) / every 2nd (72 Hz)
    b.d.tree().setReadoutRefresh(1.0 / 144.0, 0.001);
    const uint64_t frames = b.run(5.0);
    CHECK(frames <= 145);
    CHECK(frames >= 143);
  }
}

TEST_CASE("readout timers: an idle tree has no readout timer, and one that stops asking leaves the tree idle") {
  TestDriver d(100, 100);
  Widget& root = d.setRoot<Widget>();
  Readout& r = root.add<Readout>("r");
  d.step();
  CHECK(d.tree().nextWakeTime() == std::numeric_limits<double>::infinity());
  const uint32_t id = r.requestReadoutTimer();  // not the Readout's own: fires once, re-arms nothing
  CHECK(id != 0);
  const double due = d.tree().nextWakeTime();
  CHECK(due > d.now());
  CHECK(due <= d.now() + 1.0 / 30.0);
  d.advance(0.1);
  CHECK(d.tree().nextWakeTime() == std::numeric_limits<double>::infinity());
  CHECK_FALSE(d.tree().needsFrame());
}

// The frame loop (pq/ui/frame_pacer.h) with a playhead timer faster than the display and readouts on the grid: the
// playhead presents every pass, the readouts are polled in every n-th of those frames, never one late.
TEST_CASE("readout grid: with a playhead at the display's rate, readouts poll in every n-th frame exactly") {
  for (const double hz : {60.0, 100.0, 144.0})
    for (const double rate : {30.0, 60.0}) {
      CAPTURE(hz);
      CAPTURE(rate);
      const double T = 1.0 / hz, phase = 0.0017;
      const uint32_t n = readoutPasses(T, rate);
      auto nextPass = [&](double t) { return (std::floor((t - phase) / T) + 1.0) * T + phase; };
      ReadoutGrid g{};
      double now = 0.0, lastFrame = -1.0, lastPass = -1.0;
      double playhead = 0.004, readout = nextReadoutTick(g, 0.0);
      std::vector<int64_t> pollFrames;
      int64_t frame = 0;
      uint32_t k = 0;
      while (now < 20.0) {
        const double wake = std::min(playhead, readout);
        if (wake > now) now += std::ceil((wake - now) * 1000.0) / 1000.0;
        const FrameWait w = frameWait(now, lastFrame, lastPass, T, T);
        if (w.kind == FrameWait::Kind::Pass) {
          now = nextPass(now) + 0.0003 + 0.0004 * std::sin(static_cast<double>(++k) * 2.3);  // DwmFlush's lag, jittering
          lastPass = now;
        }
        lastFrame = frameTime(w, lastFrame, now, T);
        g = readoutGrid(g, rate, T, lastPass);  // (the loop: before the tree's update)
        if (playhead <= now) playhead = now + 1.0 / 144.0;
        if (readout <= now) {
          pollFrames.push_back(frame);
          readout = nextReadoutTick(g, now);
        }
        now += 0.0005;
        ++frame;
      }
      REQUIRE(pollFrames.size() > 100);
      // (the first polls come before the first pass is known)
      for (size_t i = 8; i < pollFrames.size(); ++i) REQUIRE(pollFrames[i] - pollFrames[i - 1] == static_cast<int64_t>(n));
      CHECK(frame >= static_cast<int64_t>(19.5 * hz));  // the playhead kept the display's rate
    }
}
