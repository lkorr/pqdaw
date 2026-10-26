#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

#include "pq/core/time.h"

namespace {

using pq::Ticks;

// ---- Independent reference for the tempo integral -------------------------------------------------------------
// seconds(τ) = ∫ 60 / (PPQ · bpm(u)) du with bpm linear in ticks, by composite 8-point Gauss–Legendre quadrature
// (does not use the closed-form log1p/expm1 expressions the implementation uses).

constexpr double kGlX[8] = {-0.9602898564975363, -0.7966664774136267, -0.5255324099163290, -0.1834346424956498,
                            0.1834346424956498,  0.5255324099163290,  0.7966664774136267,  0.9602898564975363};
constexpr double kGlW[8] = {0.1012285362903763, 0.2223810344533745, 0.3137066458778873, 0.3626837833783620,
                            0.3626837833783620, 0.3137066458778873, 0.2223810344533745, 0.1012285362903763};

struct RefSegment {
  Ticks start;
  Ticks len;  // 0 for the last segment
  double b0;
  double b1;  // == b0 for steps
};

double refSegmentSeconds(const RefSegment& s, double tau) {
  if (tau == 0.0) return 0.0;
  if (s.b0 == s.b1) return tau * 60.0 / (static_cast<double>(pq::kPpq) * s.b0);
  constexpr int kSub = 32;
  double sum = 0.0;
  const double h = tau / kSub;
  for (int j = 0; j < kSub; ++j) {
    const double a = j * h;
    double part = 0.0;
    for (int g = 0; g < 8; ++g) {
      const double u = a + 0.5 * h * (kGlX[g] + 1.0);
      const double bpm = s.b0 + (s.b1 - s.b0) * u / static_cast<double>(s.len);
      part += kGlW[g] * 60.0 / (static_cast<double>(pq::kPpq) * bpm);
    }
    sum += 0.5 * h * part;
  }
  return sum;
}

struct Reference {
  std::vector<RefSegment> segs;
  std::vector<double> startSec;  // Kahan-summed

  double seconds(Ticks t) const {
    const auto it = std::upper_bound(segs.begin(), segs.end(), t,
                                     [](Ticks v, const RefSegment& sg) { return v < sg.start; });
    const std::size_t i = it == segs.begin() ? 0 : static_cast<std::size_t>(it - segs.begin()) - 1;
    return startSec[i] + refSegmentSeconds(segs[i], static_cast<double>(t - segs[i].start));
  }
};

Reference makeReference(const std::vector<pq::TempoPoint>& pts) {
  Reference r;
  for (std::size_t i = 0; i < pts.size(); ++i) {
    const bool last = i + 1 == pts.size();
    const bool ramp = !last && pts[i].curve == pq::TempoCurve::Linear;
    r.segs.push_back({pts[i].tick, last ? 0 : pts[i + 1].tick - pts[i].tick, pts[i].bpm,
                      ramp ? pts[i + 1].bpm : pts[i].bpm});
  }
  double sum = 0, c = 0;
  for (std::size_t i = 0; i < r.segs.size(); ++i) {
    r.startSec.push_back(sum);
    if (i + 1 < r.segs.size()) {
      const double y = refSegmentSeconds(r.segs[i], static_cast<double>(r.segs[i].len)) - c;
      const double t = sum + y;
      c = (t - sum) - y;
      sum = t;
    }
  }
  return r;
}

// A deterministic tempo map with alternating steps and ramps between 40 and 240 BPM, lasting just over
// `minSeconds` (truncated using the reference integral).
std::vector<pq::TempoPoint> makeLongSession(double minSeconds) {
  std::mt19937_64 rng(0x5eed1234);
  auto uni = [&](double lo, double hi) { return lo + (hi - lo) * (static_cast<double>(rng() >> 11) * 0x1.0p-53); };
  std::vector<pq::TempoPoint> pts;
  Ticks t = 0;
  double lowerBoundSeconds = 0;  // at the 240 BPM maximum
  while (lowerBoundSeconds < minSeconds) {
    const double bpm = uni(40.0, 240.0);
    const auto curve = (rng() & 1) ? pq::TempoCurve::Linear : pq::TempoCurve::Step;
    pts.push_back({t, bpm, curve});
    // Lengths from 1 tick (stress the boundaries) to 64 bars; odd lengths so ramps are not tick-aligned to beats.
    const Ticks len = (rng() % 8 == 0) ? static_cast<Ticks>(1 + rng() % 50)
                                       : static_cast<Ticks>(uni(1.0, 64.0 * 4.0) * pq::kPpq) + 1;
    t += len;
    lowerBoundSeconds += static_cast<double>(len) * 60.0 / (static_cast<double>(pq::kPpq) * 240.0);
  }
  pts.push_back({t, 120.0, pq::TempoCurve::Step});
  // Keep points up to the first one that starts after minSeconds; it becomes the (constant) last point.
  const Reference ref = makeReference(pts);
  std::size_t keep = 1;
  while (keep < pts.size() && ref.startSec[keep - 1] < minSeconds) ++keep;
  pts.resize(keep);
  pts.back().curve = pq::TempoCurve::Step;
  return pts;
}

}  // namespace

// ---- Ticks and tuplets ------------------------------------------------------------------------------------------

TEST_CASE("time: PPQ and note values") {
  static_assert(pq::kPpq == 221760);  // 2⁶·3²·5·7·11 (SPEC §3.1, docs/QUESTIONS.md Q3)
  static_assert(pq::kTicksPerWhole == 4 * pq::kPpq);
  for (int n = 1; n <= 11; ++n) CHECK(pq::kPpq % n == 0);  // lcm(1..11) divides it
  CHECK(pq::kPpq % 64 == 0);
  CHECK(pq::noteValueTicks(4) == pq::kPpq);
  CHECK(pq::noteValueTicks(16) == pq::kPpq / 4);
  CHECK(pq::noteValueTicks(32) == pq::kPpq / 8);
  CHECK(pq::noteValueIsExact(64));
  CHECK(pq::noteValueTicks(256) == 3465);  // the finest exact straight value
  CHECK_FALSE(pq::noteValueIsExact(512));  // 1/512 = 1732.5 ticks
  CHECK(pq::noteValueTicks(512) == 1733);  // rounded
  CHECK(pq::noteValueIsExact(12));
  CHECK_FALSE(pq::noteValueIsExact(13));
  CHECK_FALSE(pq::noteValueIsExact(0));
}

TEST_CASE("time: a denominator <= 0 gives 0 ticks instead of dividing by zero (M0 review L13)") {
  static_assert(pq::noteValueTicks(0) == 0);
  static_assert(pq::tupletGroupTicks(0, 3) == 0);
  CHECK(pq::noteValueTicks(-4) == 0);
  CHECK(pq::tupletGroupTicks(-16, 7) == 0);
  volatile int64_t zero = 0;  // a runtime value (a UI field), not a constant
  CHECK(pq::noteValueTicks(zero) == 0);
  CHECK(pq::tupletGroupTicks(zero, 5) == 0);
}

TEST_CASE("time: tuplet groups follow n:m with m the largest power of two <= n") {
  CHECK(pq::tupletBase(1) == 1);
  CHECK(pq::tupletBase(2) == 2);
  CHECK(pq::tupletBase(3) == 2);
  CHECK(pq::tupletBase(4) == 4);
  CHECK(pq::tupletBase(5) == 4);
  CHECK(pq::tupletBase(6) == 4);
  CHECK(pq::tupletBase(7) == 4);
  CHECK(pq::tupletBase(9) == 8);
  CHECK(pq::tupletBase(11) == 8);
  // SPEC §9.2: a 7-tuplet at 1/16 is 7 notes per quarter.
  CHECK(pq::tupletGroupTicks(16, 7) == pq::kPpq);
  CHECK(pq::tupletGroupTicks(8, 3) == pq::kPpq);  // eighth-note triplets: 3 per quarter
  CHECK(pq::tupletGroupTicks(64, 11) == pq::kTicksPerWhole / 8);
}

TEST_CASE("time: straight 1/1 ... 1/256 and their 3/5/7/9/11-tuplets (and products) land exactly on integer ticks") {
  const int64_t denominators[] = {1, 2, 4, 8, 16, 32, 64, 128, 256};
  const int64_t tuplets[] = {3, 5, 7, 11, 9, 15, 21, 33, 35, 55, 77};
  for (const int64_t d : denominators) {
    // T0.5-fix1: every straight note value 1/1 … 1/256 is an integer tick count.
    REQUIRE(pq::noteValueIsExact(d));
    REQUIRE(pq::noteValueTicks(d) * d == pq::kTicksPerWhole);
    for (const int64_t n : tuplets) {
      const Ticks span = pq::tupletGroupTicks(d, n);
      CAPTURE(d);
      CAPTURE(n);
      REQUIRE(span * d == pq::kTicksPerWhole * pq::tupletBase(n));  // the group span itself is exact
      REQUIRE(pq::tupletIsExact(span, n));
      for (int64_t k = -2 * n; k <= 2 * n; ++k) {
        const Ticks start = 7 * pq::kTicksPerWhole;
        const Ticks pos = pq::tupletPosition(start, span, n, k);
        REQUIRE((pos - start) * n == k * span);  // exact: no rounding
      }
    }
  }
  // Any n-tuplet of a quarter for n dividing kPpq (all products of 3, 5, 7, 11, 9, 8) is exact.
  for (const int64_t n : tuplets) {
    REQUIRE(pq::tupletIsExact(pq::kPpq, n));
    for (int64_t k = 0; k <= n; ++k) REQUIRE(pq::tupletPosition(0, pq::kPpq, n, k) * n == k * pq::kPpq);
  }
}

TEST_CASE("time: unsupported primes round to the nearest tick without drift") {
  for (const int64_t n : {13, 17, 19, 23}) {
    CHECK_FALSE(pq::tupletIsExact(pq::kPpq, n));
    for (int64_t k = -1000; k <= 1000; ++k) {
      const double exact = static_cast<double>(k) * pq::kPpq / static_cast<double>(n);
      REQUIRE(std::abs(static_cast<double>(pq::tupletPosition(0, pq::kPpq, n, k)) - exact) <= 0.5);
    }
  }
}

// ---- Tempo map ----------------------------------------------------------------------------------------------------

TEST_CASE("tempo: validation") {
  using P = pq::TempoPoint;
  CHECK_FALSE(pq::TempoMap::create({}).ok());
  {
    const P p[] = {{10, 120.0}};
    CHECK_FALSE(pq::TempoMap::create(p).ok());
  }
  {
    const P p[] = {{0, 0.0}};
    CHECK_FALSE(pq::TempoMap::create(p).ok());
  }
  {
    const P p[] = {{0, std::numeric_limits<double>::quiet_NaN()}};
    CHECK_FALSE(pq::TempoMap::create(p).ok());
  }
  {
    const P p[] = {{0, 2000.0}};
    CHECK_FALSE(pq::TempoMap::create(p).ok());
  }
  {
    const P p[] = {{0, 120.0}, {100, 90.0}, {100, 80.0}};
    const auto r = pq::TempoMap::create(p);
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().code == pq::ErrorCode::InvalidArgument);
  }
  {
    const P p[] = {{0, 120.0}, {100, 90.0, pq::TempoCurve::Linear}};
    CHECK(pq::TempoMap::create(p).ok());
  }
}

TEST_CASE("tempo: constant tempo is sample-exact") {
  const pq::TempoMap m;  // 120 BPM: a quarter is 0.5 s = 24000 samples at 48 kHz
  CHECK(m.bpmAt(0) == 120.0);
  CHECK(m.ticksToSeconds(pq::kPpq) == 0.5);
  CHECK(m.ticksToSamples(pq::kPpq, 48000.0) == 24000);
  const Ticks tenHours = 72000 * pq::kPpq;  // 72000 quarters = 36000 s
  CHECK(m.ticksToSamples(tenHours, 192000.0) == 36000LL * 192000LL);
  CHECK(m.ticksToSamples(tenHours + 8, 192000.0) == 36000LL * 192000LL + 3);  // 1 tick = 0.433 samples
  CHECK(m.samplesToTicks(24000, 48000.0) == static_cast<double>(pq::kPpq));
  // Before zero the first tempo continues.
  CHECK(m.ticksToSeconds(-pq::kPpq) == -0.5);
  CHECK(m.secondsToTicks(-0.5) == static_cast<double>(-pq::kPpq));
}

TEST_CASE("tempo: linear ramp matches the closed form and is linear in ticks") {
  const Ticks len = 4 * pq::kPpq;
  const pq::TempoPoint p[] = {{0, 60.0, pq::TempoCurve::Linear}, {len, 120.0}};
  const auto r = pq::TempoMap::create(p);
  REQUIRE(r.ok());
  const pq::TempoMap& m = *r;
  // ∫ 60/(PPQ·(60 + 60u/L)) du over [0, L] = (L/PPQ)·ln 2.
  CHECK(m.ticksToSeconds(len) == doctest::Approx(4.0 * std::log(2.0)).epsilon(1e-15));
  CHECK(m.bpmAt(len / 2) == doctest::Approx(90.0).epsilon(1e-15));
  CHECK(m.bpmAt(len) == 120.0);
  CHECK(m.bpmAt(len * 10) == 120.0);
  // After the ramp: constant 120 BPM from the ramp's end time.
  CHECK(m.ticksToSeconds(len + pq::kPpq) == doctest::Approx(4.0 * std::log(2.0) + 0.5).epsilon(1e-15));
  // Inverse
  for (Ticks t = 0; t <= len + pq::kPpq; t += 977) {
    REQUIRE(std::abs(m.secondsToTicks(m.ticksToSeconds(t)) - static_cast<double>(t)) < 1e-9);
  }
  // A ramp with a vanishing slope behaves like a constant (log1p(z)/z with z → 0).
  const pq::TempoPoint q[] = {{0, 120.0, pq::TempoCurve::Linear}, {1'000'000'000, 120.000001}};
  const auto r2 = pq::TempoMap::create(q);
  REQUIRE(r2.ok());
  CHECK(r2->ticksToSeconds(pq::kPpq) == doctest::Approx(0.5).epsilon(1e-9));
}

TEST_CASE("tempo: 10-hour session at 192 kHz with ramps stays within half a sample") {
  constexpr double kSr = 192000.0;
  double wsum = 0;
  for (const double w : kGlW) wsum += w;
  REQUIRE(std::abs(wsum - 2.0) < 1e-15);  // guards the reference quadrature constants
  const auto pts = makeLongSession(10.0 * 3600.0);
  const auto r = pq::TempoMap::create(pts);
  REQUIRE(r.ok());
  const pq::TempoMap& m = *r;
  const Reference ref = makeReference(pts);
  const Ticks end = pts.back().tick;
  const double endSeconds = ref.seconds(end);
  MESSAGE("session: " << pts.size() << " tempo points, " << endSeconds / 3600.0 << " h, " << end << " ticks");
  REQUIRE(endSeconds >= 10.0 * 3600.0);
  // Query ticks: every segment boundary ±1, plus random positions over the whole session.
  std::vector<Ticks> queries;
  for (const auto& p : pts)
    for (Ticks d = -1; d <= 1; ++d) queries.push_back(std::max<Ticks>(0, p.tick + d));
  std::mt19937_64 rng(42);
  for (int i = 0; i < 100'000; ++i) queries.push_back(static_cast<Ticks>(rng() % static_cast<uint64_t>(end + 1)));

  double worstF = 0;       // |unrounded samples − reference|
  double worstRound = 0;   // |rounded samples − reference|
  double worstTicks = 0;   // |secondsToTicks(ticksToSeconds(t)) − t|
  for (const Ticks t : queries) {
    const double exact = ref.seconds(t) * kSr;
    worstF = std::max(worstF, std::abs(m.ticksToSamplesF(t, kSr) - exact));
    const pq::SampleTime s = m.ticksToSamples(t, kSr);
    worstRound = std::max(worstRound, std::abs(static_cast<double>(s) - exact));
    worstTicks = std::max(worstTicks, std::abs(m.secondsToTicks(m.ticksToSeconds(t)) - static_cast<double>(t)));
    // Round trip through samples: the first tick at that sample is this tick or an earlier one that rounds to
    // the same sample (two ticks can share a sample when ticks are shorter than a sample).
    const Ticks back = m.firstTickAtOrAfterSample(s, kSr);
    REQUIRE(back <= t);
    REQUIRE(m.ticksToSamples(back, kSr) == s);
  }
  MESSAGE("max |samplesF - exact| = " << worstF << ", max |round(samples) - exact| = " << worstRound
                                      << ", max tick round-trip error = " << worstTicks);
  CHECK(worstF < 1e-4);                // numeric error is ~1e-5 samples or better
  CHECK(worstRound <= 0.5 + 1e-4);     // nearest sample (the 1e-4 only matters at exact .5 ties)
  CHECK(worstTicks < 1e-5);

  // Sample → tick: firstTickAtOrAfterSample is exactly consistent with ticksToSamples, and samplesToTicks
  // brackets the same position.
  const pq::SampleTime endSample = m.ticksToSamples(end, kSr);
  for (int i = 0; i < 100'000; ++i) {
    const pq::SampleTime s = static_cast<pq::SampleTime>(rng() % static_cast<uint64_t>(endSample));
    const Ticks t = m.firstTickAtOrAfterSample(s, kSr);
    REQUIRE(m.ticksToSamples(t, kSr) >= s);
    REQUIRE(m.ticksToSamples(t - 1, kSr) < s);
    const double tf = m.samplesToTicks(s, kSr);
    const Ticks t0 = static_cast<Ticks>(std::floor(tf));
    REQUIRE(m.ticksToSamplesF(t0, kSr) <= static_cast<double>(s) + 1e-4);
    REQUIRE(m.ticksToSamplesF(t0 + 1, kSr) >= static_cast<double>(s) - 1e-4);
  }
}

TEST_CASE("tempo: block partition by firstTickAtOrAfterSample covers every tick exactly once") {
  const pq::TempoPoint p[] = {{0, 999.0, pq::TempoCurve::Linear}, {50 * pq::kPpq, 20.0, pq::TempoCurve::Step},
                              {60 * pq::kPpq, 300.0}};
  const auto r = pq::TempoMap::create(p);
  REQUIRE(r.ok());
  const double sr = 44100.0;  // at 999 BPM a tick is ~0.012 sample, so many ticks share a sample
  Ticks expected = r->firstTickAtOrAfterSample(0, sr);  // negative: ticks just before 0 round to sample 0
  CHECK(expected < 0);
  for (pq::SampleTime s0 = 0; s0 < 3 * 44100; s0 += 64) {
    const Ticks a = r->firstTickAtOrAfterSample(s0, sr);
    const Ticks b = r->firstTickAtOrAfterSample(s0 + 64, sr);
    REQUIRE(a == expected);
    for (Ticks t = a; t < b; t += std::max<Ticks>(1, (b - a) / 7)) {
      const pq::SampleTime s = r->ticksToSamples(t, sr);
      REQUIRE(s >= s0);
      REQUIRE(s < s0 + 64);
    }
    expected = b;
  }
}

// ---- Time signatures ----------------------------------------------------------------------------------------------

TEST_CASE("timesig: validation") {
  using C = pq::TimeSigChange;
  CHECK_FALSE(pq::TimeSigMap::create({}).ok());
  {
    const C c[] = {{1, {4, 4}}};
    CHECK_FALSE(pq::TimeSigMap::create(c).ok());
  }
  {
    const C c[] = {{0, {4, 512}}};  // 1/512 is not a whole number of ticks
    CHECK_FALSE(pq::TimeSigMap::create(c).ok());
  }
  {
    const C c[] = {{0, {4, 4}}, {1, {7, 64}}, {2, {3, 256}}};  // x/64 (and finer) are legal at PPQ 221760
    const auto r = pq::TimeSigMap::create(c);
    REQUIRE(r.ok());
    CHECK(r->barToTicks(2) - r->barToTicks(1) == 7 * (pq::kTicksPerWhole / 64));
    CHECK(r->ticksToBarBeat(r->barToTicks(2) + 2 * (pq::kTicksPerWhole / 256) + 1) == pq::BarBeatTick{2, 2, 1});
  }
  {
    const C c[] = {{0, {0, 4}}};
    CHECK_FALSE(pq::TimeSigMap::create(c).ok());
  }
  {
    const C c[] = {{0, {4, 4}}, {4, {3, 4}}, {4, {5, 4}}};
    CHECK_FALSE(pq::TimeSigMap::create(c).ok());
  }
  {
    const C c[] = {{0, {7, 12}}};
    CHECK(pq::TimeSigMap::create(c).ok());
  }
}

TEST_CASE("timesig: bars, beats and changes on bar boundaries") {
  const pq::TimeSigMap def;
  CHECK(def.barToTicks(1) == 4 * pq::kPpq);
  CHECK(def.ticksToBarBeat(4 * pq::kPpq + pq::kPpq + 5) == pq::BarBeatTick{1, 1, 5});
  CHECK(def.ticksToBarBeat(-1) == pq::BarBeatTick{-1, 3, pq::kPpq - 1});

  const pq::TimeSigChange c[] = {{0, {4, 4}}, {4, {7, 8}}, {6, {5, 4}}, {10, {7, 12}}};
  const auto r = pq::TimeSigMap::create(c);
  REQUIRE(r.ok());
  const pq::TimeSigMap& m = *r;
  const Ticks q = pq::kPpq;
  CHECK(m.barToTicks(4) == 16 * q);
  CHECK(m.barToTicks(6) == 16 * q + 2 * 7 * (q / 2));
  CHECK(m.barToTicks(10) == m.barToTicks(6) + 4 * 5 * q);
  CHECK(m.barToTicks(11) == m.barToTicks(10) + 7 * (pq::kTicksPerWhole / 12));
  CHECK(m.signatureAtBar(5) == pq::TimeSignature{7, 8});
  CHECK(m.signatureAt(m.barToTicks(6) - 1) == pq::TimeSignature{7, 8});
  CHECK(m.signatureAt(m.barToTicks(6)) == pq::TimeSignature{5, 4});
  CHECK(m.ticksToBarBeat(m.barToTicks(5) + 3 * (q / 2) + 7) == pq::BarBeatTick{5, 3, 7});
  CHECK(m.ticksToBarBeat(m.barToTicks(12) + (pq::kTicksPerWhole / 12) * 6 + 1) == pq::BarBeatTick{12, 6, 1});

  // Round trip over a range that crosses every change, including negative positions.
  for (Ticks t = -3 * pq::kTicksPerWhole; t < m.barToTicks(14); t += 61) {
    const pq::BarBeatTick b = m.ticksToBarBeat(t);
    const pq::TimeSignature sig = m.signatureAtBar(b.bar);
    REQUIRE(b.beat >= 0);
    REQUIRE(b.beat < sig.numerator);
    REQUIRE(b.tick >= 0);
    REQUIRE(b.tick < pq::ticksPerBeat(sig));
    REQUIRE(m.barBeatToTicks(b) == t);
  }
}
