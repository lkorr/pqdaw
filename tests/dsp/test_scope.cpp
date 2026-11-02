// The oscilloscope's analysis (pq/dsp/scope.h, TX.scope): the trace stands still. Each case plays a signal through the
// tracker as a scope face does (a frame every 800 samples, 60 fps at 48 kHz, over the newest `need` samples) and
// measures the absolute phase where each frame starts: a still trace starts at the same phase of the waveform in every
// frame. Sine, a bright wave (several rising zero crossings per period), the bright wave in noise, a note change, very
// low and very high pitches, the edge trigger (rising / falling, level), the note-locked period, Stable's pinned span,
// the detector's accuracy and the beat slots.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "pq/dsp/scope.h"

using pq::dsp::ScopeBeat;
using pq::dsp::ScopeFrame;
using pq::dsp::ScopeSettings;
using pq::dsp::ScopeTracker;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kSr = 48000.0;
constexpr size_t kHop = 800;

struct Noise {
  uint64_t s;
  explicit Noise(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 1) {}
  float next(float a) {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return a * (static_cast<float>(s >> 40) / static_cast<float>(1u << 23) - 1.0f);
  }
};

// A bright wave: eight harmonics at 1/k with spread phases (three rising zero crossings per period).
double bright(double ph) {
  double v = 0.0;
  for (int k = 1; k <= 8; ++k) v += std::sin(2.0 * kPi * k * ph + 0.7 * k * k) / k;
  return 0.45 * v;
}

struct Run {
  std::vector<double> phase;   // per frame: the start's phase in cycles of the frequency then (0 … 1)
  std::vector<ScopeFrame> frames;
};

// Plays `frames` frames of x(i) (absolute sample i) through `t`; hz(i): the true frequency at i (phase measure).
// phaseOf(i): the true phase in cycles at sample i.
Run play(ScopeTracker& t, const std::function<float(size_t)>& x, const std::function<double(size_t)>& phaseOf,
         int frames, float noteHz = 0.0f, size_t startAt = 96000) {
  Run r;
  std::vector<float> win;
  for (int k = 0; k < frames; ++k) {
    const size_t end = startAt + static_cast<size_t>(k) * kHop;
    const size_t n = std::min(t.need(kSr), end);
    win.resize(n);
    for (size_t i = 0; i < n; ++i) win[i] = x(end - n + i);
    const ScopeFrame f = t.frame(win, kSr, noteHz, false);
    const size_t abs = end - n + f.start;
    const double ph = phaseOf(abs);
    r.phase.push_back(ph - std::floor(ph));
    r.frames.push_back(f);
  }
  return r;
}

// The largest distance (in cycles) of the frames' start phases from their circular mean, over frames [from, to).
double phaseSpread(const Run& r, size_t from, size_t to) {
  double c = 0.0, s = 0.0;
  for (size_t i = from; i < to; ++i) {
    c += std::cos(2.0 * kPi * r.phase[i]);
    s += std::sin(2.0 * kPi * r.phase[i]);
  }
  const double mean = std::atan2(s, c) / (2.0 * kPi);
  double worst = 0.0;
  for (size_t i = from; i < to; ++i) {
    double d = r.phase[i] - mean;
    d -= std::round(d);
    worst = std::max(worst, std::fabs(d));
  }
  return worst;
}

ScopeSettings pitchSettings(ScopeSettings::Lock lock = ScopeSettings::Lock::Constant) {
  ScopeSettings s;
  s.timebase = ScopeSettings::Timebase::Pitch;
  s.lock = lock;
  return s;
}

}  // namespace

TEST_CASE("scope: the detector reads sines and bright waves within 0.2 %, nothing on noise or silence") {
  ScopeTracker t;
  t.setSettings(pitchSettings());
  for (const double hz : {31.0, 55.0, 110.0, 220.0, 440.0, 1000.0, 2500.0, 4000.0}) {
    for (const bool br : {false, true}) {
      CAPTURE(hz);
      CAPTURE(br);
      std::vector<float> x(12000);
      for (size_t i = 0; i < x.size(); ++i) {
        const double ph = hz * static_cast<double>(i) / kSr;
        x[i] = static_cast<float>(br ? bright(ph) : 0.5 * std::sin(2.0 * kPi * ph));
      }
      const auto p = t.detectPitch(x, x.size(), kSr);
      REQUIRE(p.period > 0.0);
      CHECK(kSr / p.period == doctest::Approx(hz).epsilon(0.002));
      CHECK(p.clarity > 0.9f);
    }
  }
  Noise n(3);
  std::vector<float> x(12000);
  for (float& v : x) v = n.next(0.5f);
  CHECK(t.detectPitch(x, x.size(), kSr).period == 0.0);
  std::fill(x.begin(), x.end(), 0.0f);
  CHECK(t.detectPitch(x, x.size(), kSr).period == 0.0);
}

TEST_CASE("scope: pitch timebase holds sine, bright, noisy, low and high tones still") {
  struct Case {
    const char* name;
    double hz;
    bool bright;
    float noise;
    double maxSamples;  // the start's spread allowed, in samples (or 0.5 % of a period, whichever is larger)
  };
  const Case cases[] = {
      {"sine 220", 220.0, false, 0.0f, 1.0},      {"bright 220", 220.0, true, 0.0f, 1.0},
      {"bright 220 + noise -20 dB", 220.0, true, 0.05f, 3.0},
      {"bright 31 (low)", 31.0, true, 0.0f, 1.0}, {"sine 4000 (high)", 4000.0, false, 0.0f, 1.0},
      {"bright 1500", 1500.0, true, 0.0f, 1.0},
  };
  for (const auto lock : {ScopeSettings::Lock::Constant, ScopeSettings::Lock::Stable, ScopeSettings::Lock::Tracking}) {
    for (const Case& cs : cases) {
      CAPTURE(std::string(cs.name));
      CAPTURE(static_cast<int>(lock));
      ScopeTracker t;
      t.setSettings(pitchSettings(lock));
      Noise nz(7);
      std::vector<float> sig(96000 + 200 * kHop + 10);
      for (size_t i = 0; i < sig.size(); ++i) {
        const double ph = cs.hz * static_cast<double>(i) / kSr;
        sig[i] = static_cast<float>(cs.bright ? bright(ph) : 0.5 * std::sin(2.0 * kPi * ph)) + nz.next(cs.noise);
      }
      const Run r = play(t, [&](size_t i) { return sig[i]; }, [&](size_t i) { return cs.hz * static_cast<double>(i) / kSr; },
                         200);
      const double spread = phaseSpread(r, 20, r.phase.size()) * kSr / cs.hz;
      MESSAGE(std::string(cs.name) << " lock " << static_cast<int>(lock) << ": start spread " << spread << " samples, pitch "
                      << r.frames.back().pitchHz);
      CHECK(spread <= std::max(cs.maxSamples, 0.005 * kSr / cs.hz));
      CHECK(r.frames.back().pitchHz == doctest::Approx(cs.hz).epsilon(0.002));
      if (lock == ScopeSettings::Lock::Tracking)
        CHECK(static_cast<double>(r.frames.back().span) == doctest::Approx(std::max(16.0, kSr / cs.hz)).epsilon(0.01));
      if (lock == ScopeSettings::Lock::Constant) CHECK(r.frames.back().span == static_cast<size_t>(52.2 * 0.001 * kSr));
    }
  }
}

TEST_CASE("scope: a note change re-locks within a fifth of a second; vibrato does not rescale Stable's span") {
  ScopeTracker t;
  t.setSettings(pitchSettings(ScopeSettings::Lock::Stable));
  // 220 Hz, then 330 Hz from frame 60.
  const size_t change = 96000 + 60 * kHop;
  const auto phaseOf = [&](size_t i) {
    return i < change ? 220.0 * static_cast<double>(i) / kSr
                      : 220.0 * static_cast<double>(change) / kSr + 330.0 * static_cast<double>(i - change) / kSr;
  };
  const auto x = [&](size_t i) { return static_cast<float>(bright(phaseOf(i))); };
  const Run r = play(t, x, phaseOf, 140);
  CHECK(phaseSpread(r, 20, 60) * kSr / 220.0 <= 1.0);
  // Within 12 frames (the detector's 6, the span's 12 more would be 18; 200 ms is 12 frames at 60 fps) the pitch is
  // the new note's; the span follows within 24.
  CHECK(r.frames[60 + 12].pitchHz == doctest::Approx(330.0).epsilon(0.01));
  CHECK(static_cast<double>(r.frames[60 + 24].span) == doctest::Approx(kSr / 330.0).epsilon(0.01));
  CHECK(phaseSpread(r, 60 + 24, 140) * kSr / 330.0 <= 1.0);

  // Vibrato ±50 cents at 5.5 Hz: the span never moves.
  ScopeTracker v;
  v.setSettings(pitchSettings(ScopeSettings::Lock::Stable));
  std::vector<double> ph(96000 + 300 * kHop + 10);
  double acc = 0.0;
  for (size_t i = 0; i < ph.size(); ++i) {
    ph[i] = acc;
    acc += 220.0 * std::pow(2.0, 0.5 / 12.0 * std::sin(2.0 * kPi * 5.5 * static_cast<double>(i) / kSr)) / kSr;
  }
  const Run rv = play(v, [&](size_t i) { return static_cast<float>(bright(ph[i])); }, [&](size_t i) { return ph[i]; }, 300);
  size_t lo = rv.frames[20].span, hi = lo;
  for (size_t i = 20; i < rv.frames.size(); ++i) {
    lo = std::min(lo, rv.frames[i].span);
    hi = std::max(hi, rv.frames[i].span);
  }
  CHECK(lo == hi);
}

TEST_CASE("scope: the note's period locks exactly (no estimate), cycles across the width") {
  ScopeTracker t;
  ScopeSettings s = pitchSettings(ScopeSettings::Lock::Tracking);
  s.cycles = 3;
  t.setSettings(s);
  // A noisy, inharmonic-looking signal the detector could not read: still locked by the note.
  Noise nz(11);
  std::vector<float> sig(96000 + 100 * kHop + 10);
  for (size_t i = 0; i < sig.size(); ++i) sig[i] = static_cast<float>(bright(196.0 * static_cast<double>(i) / kSr)) + nz.next(0.3f);
  const Run r = play(t, [&](size_t i) { return sig[i]; }, [](size_t i) { return 196.0 * static_cast<double>(i) / kSr; }, 100,
                     196.0f);
  CHECK(r.frames.back().note);
  CHECK(r.frames.back().pitchHz == doctest::Approx(196.0).epsilon(1e-6));
  CHECK(r.frames.back().span == static_cast<size_t>(std::llround(3.0 * kSr / 196.0)));
  CHECK(phaseSpread(r, 5, r.phase.size()) * kSr / 196.0 <= 4.0);
  // Detect only: the same signal through the detector.
  ScopeTracker d;
  s.source = ScopeSettings::PitchSource::Detect;
  d.setSettings(s);
  const Run rd = play(d, [&](size_t i) { return sig[i]; }, [](size_t i) { return 196.0 * static_cast<double>(i) / kSr; }, 30,
                      196.0f);
  CHECK_FALSE(rd.frames.back().note);
}

TEST_CASE("scope: the fixed timebase's edge trigger (rising, falling, a level) holds a sine still") {
  for (const auto trig : {ScopeSettings::Trigger::Rising, ScopeSettings::Trigger::Falling}) {
    for (const float level : {0.0f, 0.25f, -0.3f}) {
      CAPTURE(static_cast<int>(trig));
      CAPTURE(level);
      ScopeTracker t;
      ScopeSettings s;
      s.timebase = ScopeSettings::Timebase::Fixed;
      s.windowMs = 20.0f;
      s.trigger = trig;
      s.level = level;
      t.setSettings(s);
      const double hz = 173.0;
      const auto x = [&](size_t i) { return static_cast<float>(0.5 * std::sin(2.0 * kPi * hz * static_cast<double>(i) / kSr)); };
      const auto phaseOf = [&](size_t i) { return hz * static_cast<double>(i) / kSr; };
      const Run r = play(t, x, phaseOf, 60);
      CHECK(phaseSpread(r, 0, r.phase.size()) * kSr / hz <= 1.0);
      CHECK(r.frames.back().span == 960);
      // The start is where the sine crosses the level in the trigger's direction.
      const double want = std::asin(level / 0.5) / (2.0 * kPi);
      double ph = r.phase.back() - (trig == ScopeSettings::Trigger::Rising ? want : 0.5 - want);
      ph -= std::round(ph);
      CHECK(std::fabs(ph) * kSr / hz <= 1.0);
    }
  }
  // Off: the newest window, free-running.
  ScopeTracker t;
  ScopeSettings s;
  s.timebase = ScopeSettings::Timebase::Fixed;
  s.trigger = ScopeSettings::Trigger::Off;
  t.setSettings(s);
  std::vector<float> x(10000, 0.1f);
  const ScopeFrame f = t.frame(x, kSr, 0.0f, false);
  CHECK(f.start + f.span == x.size());
}

TEST_CASE("scope: the first reference starts on the steepest rising zero crossing") {
  std::vector<float> shape(256);
  for (size_t i = 0; i < shape.size(); ++i) shape[i] = static_cast<float>(std::sin(2.0 * kPi * (i / 256.0 + 0.37)));
  ScopeTracker::anchorShapePolarity(shape);
  CHECK(shape[0] <= 0.0f);
  CHECK(shape[1] > 0.0f);
}

TEST_CASE("scope: beat slots — phase from the clock, tempo, a pass overwrites, frames without a clock skipped") {
  ScopeBeat b;
  b.setSpan(4.0);
  CHECK(b.phase() < 0.0);
  const double bpm = 120.0, perSample = bpm / 60.0 / kSr;
  std::vector<float> beat(4096), l(4096), r(4096);
  double pos = 0.0;
  for (int chunk = 0; chunk < 48; ++chunk) {  // 4 s: two passes of 4 beats
    for (size_t i = 0; i < beat.size(); ++i) {
      beat[i] = static_cast<float>(pos);
      const double frac = pos - std::floor(pos);
      l[i] = frac < 0.05 ? 0.8f : 0.1f;  // a click on every beat
      r[i] = -l[i];
      pos += perSample;
    }
    b.ingest(beat.data(), l.data(), r.data(), beat.size());
  }
  CHECK(b.bpm(kSr) == doctest::Approx(bpm).epsilon(0.002));
  CHECK(b.moving());
  std::vector<float> mn, mx;
  std::vector<uint8_t> ok;
  b.columns(ScopeBeat::kL, 400, mn, mx, ok);
  CHECK(ok[0]);
  CHECK(mx[0] == doctest::Approx(0.8f));   // the click at the downbeat
  CHECK(mx[50] == doctest::Approx(0.1f));  // between beats
  b.columns(ScopeBeat::kS, 400, mn, mx, ok);
  CHECK(mx[0] == doctest::Approx(0.8f));   // side = (l − r) / 2 = l
  b.columns(ScopeBeat::kM, 400, mn, mx, ok);
  CHECK(mx[0] == doctest::Approx(0.0f));
  // No clock: nothing changes, not moving.
  const double ph = b.phase();
  std::fill(beat.begin(), beat.end(), std::nanf(""));
  b.ingest(beat.data(), l.data(), r.data(), beat.size());
  CHECK(b.phase() == ph);
  CHECK_FALSE(b.moving());
  b.setSpan(2.0);
  b.columns(ScopeBeat::kL, 400, mn, mx, ok);
  CHECK(std::none_of(ok.begin(), ok.end(), [](uint8_t v) { return v != 0; }));
}
