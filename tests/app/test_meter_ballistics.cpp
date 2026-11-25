// OP.2: a track meter's ballistics do not depend on how often it is read (pq/app/meter_ballistics.h): the same signal
// polled at 30 Hz and at 60 Hz (and with a second reader in between) shows the same peak bar, hold tick and clip lamp at
// the same times, and a peak between two polls is not lost at the lower rate. And the rate itself in the settings file
// (ViewPrefs::readoutHz, the "views" section): round trip, default, leniency.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "pq/app/meter_ballistics.h"
#include "pq/app/user_settings.h"

using pq::app::MeterBallistics;

namespace {

// The engine's side: block peaks (64 frames at 48 kHz), accumulated as the maximum since the previous read, as
// ProjectProgram::readMeters hands them out.
struct Signal {
  static constexpr double kBlock = 64.0 / 48000.0;
  // A burst at 0.1 s (−6 dB, 5 ms: shorter than any poll interval), a clip at 1.0 s (one block), a −20 dB tone from
  // 2.0 s to 2.5 s; silence otherwise.
  static float blockPeak(double t) {
    if (t >= 0.1 && t < 0.105) return 0.5f;
    if (t >= 1.0 && t < 1.0 + kBlock) return 1.0f;
    if (t >= 2.0 && t < 2.5) return 0.1f;
    return 0.0f;
  }
  double at = 0.0;
  // The maximum block peak in (at, until].
  float take(double until) {
    float pk = 0.0f;
    for (; at + kBlock <= until + 1e-12; at += kBlock) pk = std::max(pk, blockPeak(at));
    return pk;
  }
};

float db(float linear) { return linear > 0.0f ? 20.0f * std::log10(linear) : MeterBallistics::kSilenceDb; }

struct Reading {
  double t;
  float peak, hold;
  bool clip;
};

// Polls the signal every 1 / hz for `seconds`; returns one reading per poll.
std::vector<Reading> poll(double hz, double seconds) {
  Signal s;
  MeterBallistics m;
  std::vector<Reading> out;
  const int n = static_cast<int>(std::llround(seconds * hz));
  for (int k = 1; k <= n; ++k) {
    const double now = static_cast<double>(k) / hz;
    const float pk = s.take(now);
    const double dt = m.advance(now);
    m.step(0, db(pk), pk >= 1.0f, now, dt);
    out.push_back({now, m.shownPeakDb(0), m.holdDb[0], m.clip[0]});
  }
  return out;
}

const Reading& at(const std::vector<Reading>& r, double t) {
  for (const Reading& x : r)
    if (std::abs(x.t - t) < 1e-9) return x;
  FAIL("no reading at that time");
  return r.front();
}

}  // namespace

TEST_CASE("meter ballistics: a peak shorter than the poll interval is shown at 30 Hz as at 60 Hz") {
  const auto r30 = poll(30.0, 0.5), r60 = poll(60.0, 0.5);
  // The 5 ms burst at 0.1 s: the first poll after it shows −6 dB at both rates (the engine held the maximum).
  float top30 = MeterBallistics::kSilenceDb, top60 = MeterBallistics::kSilenceDb;
  for (const Reading& x : r30) top30 = std::max(top30, x.peak);
  for (const Reading& x : r60) top60 = std::max(top60, x.peak);
  CHECK(top30 == doctest::Approx(-6.0206f).epsilon(1e-4));
  CHECK(top60 == doctest::Approx(-6.0206f).epsilon(1e-4));
  CHECK(at(r30, 4.0 / 30.0).hold == doctest::Approx(-6.0206f).epsilon(1e-4));
  CHECK(at(r60, 4.0 / 30.0).hold == doctest::Approx(-6.0206f).epsilon(1e-4));
}

TEST_CASE("meter ballistics: the peak bar falls, the hold tick holds and the clip lamp latches by time, not by poll") {
  const auto r30 = poll(30.0, 5.0), r60 = poll(60.0, 5.0);
  // At every time both rates have a reading (each 30 Hz poll), the readings agree. The peak bar may differ by the fall
  // over the part of a poll interval by which the two saw the peak apart (≤ 1/30 s: 0.87 dB); in steady fall it is equal.
  const float slack = MeterBallistics::kFallDbPerSecond / 30.0f + 0.01f;
  for (const Reading& a : r30) {
    const Reading& b = at(r60, a.t);
    CAPTURE(a.t);
    CHECK(std::abs(a.peak - b.peak) <= slack);
    CHECK(a.clip == b.clip);
    // The hold tick: equal except within a poll interval of the moment it lets go (1.5 s after the peak it holds).
    if (a.hold != b.hold) {
      const bool nearRelease = std::abs(a.t - (0.105 + 1.5)) < 0.05 || std::abs(a.t - (1.0 + 1.5)) < 0.05 || std::abs(a.t - (2.5 + 1.5)) < 0.05;
      CHECK(nearRelease);
    }
  }
  // The fall is 26 dB per second at both rates: half a second after the clip (0 dB at 1.0 s) the bar is at −13 dB.
  CHECK(at(r30, 1.5).peak == doctest::Approx(-13.0f).epsilon(0.07));
  CHECK(at(r60, 1.5).peak == doctest::Approx(-13.0f).epsilon(0.07));
  // Between two readings a second apart in the fall, exactly 26 dB at both.
  CHECK(at(r30, 3.0).peak - at(r30, 3.5).peak == doctest::Approx(13.0f).epsilon(1e-3));
  CHECK(at(r60, 3.0).peak - at(r60, 3.5).peak == doctest::Approx(13.0f).epsilon(1e-3));
  // The clip lamp latched at both, from the first poll after the clip on.
  CHECK_FALSE(at(r30, 0.9).clip);
  CHECK(at(r30, 1.1).clip);
  CHECK(at(r60, 1.1).clip);
  CHECK(at(r30, 5.0).clip);
  // The hold tick: 0 dB held for 1.5 s after the clip, at both rates.
  CHECK(at(r30, 2.4).hold == doctest::Approx(0.0f));
  CHECK(at(r60, 2.4).hold == doctest::Approx(0.0f));
  CHECK(at(r30, 2.7).hold < -15.0f);
  CHECK(at(r60, 2.7).hold < -15.0f);
}

TEST_CASE("meter ballistics: each meter falls on its own clock (a second meter read in the same poll falls too)") {
  // Two meters stepped one after the other at the same `now` (one read per reader, as the patch view's mixer faces
  // do): with one clock shared by all readers the second saw dt = 0 and its bar never fell.
  MeterBallistics a, b;
  for (MeterBallistics* m : {&a, &b}) {
    const double dt = m->advance(1.0);
    m->step(0, 0.0f, false, 1.0, dt);
  }
  for (int k = 1; k <= 30; ++k) {
    const double now = 1.0 + k / 30.0;
    for (MeterBallistics* m : {&a, &b}) {
      const double dt = m->advance(now);
      m->step(0, MeterBallistics::kSilenceDb, false, now, dt);
    }
  }
  CHECK(a.peakDb[0] == doctest::Approx(-26.0f).epsilon(1e-3));
  CHECK(b.peakDb[0] == doctest::Approx(-26.0f).epsilon(1e-3));
  // Not read for a long time: falls as for kMaxStepSeconds, and the first step of a new meter does not fall.
  MeterBallistics c;
  CHECK(c.advance(100.0) == 0.0);
  CHECK(c.advance(200.0) == MeterBallistics::kMaxStepSeconds);
}

TEST_CASE("user settings (OP.2): the meter & visualiser rate round-trips; 30 by default; only 30 or 60 are taken") {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "pq_op2_readout_settings";
  fs::remove_all(dir);
  fs::create_directories(dir);
  const std::string path = (dir / "settings.json").string();
  CHECK(pq::views::ViewPrefs{}.readoutHz == 30);
  CHECK(pq::app::loadViewPrefs(path).readoutHz == 30);  // no file
  pq::views::ViewPrefs p;
  p.readoutHz = 60;
  REQUIRE(pq::app::saveViewPrefs(path, p).ok());
  CHECK(pq::app::loadViewPrefs(path).readoutHz == 60);
  CHECK(pq::app::loadViewPrefs(path) == p);
  p.readoutHz = 30;
  REQUIRE(pq::app::saveViewPrefs(path, p).ok());
  CHECK(pq::app::loadViewPrefs(path).readoutHz == 30);
  { std::ofstream(path) << R"({"views": {"readoutHz": 45}})"; }
  CHECK(pq::app::loadViewPrefs(path).readoutHz == 30);  // not a rate the app offers: the default
  { std::ofstream(path) << R"({"views": {"readoutHz": "60"}})"; }
  CHECK(pq::app::loadViewPrefs(path).readoutHz == 30);  // the wrong type keeps the default
  { std::ofstream(path) << R"({"views": {"readoutHz": 60}})"; }
  CHECK(pq::app::loadViewPrefs(path).readoutHz == 60);
  fs::remove_all(dir);
}
