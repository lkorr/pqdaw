// VZ.4: the loudness front end (pq/dsp/loudness.h) — EBU Tech 3341 (momentary / short-term / integrated, the gates) and
// Tech 3342 (LRA) cases generated here, true peak against the `truepeak` atom on inter-sample peaks, correlation, the
// level meter's ballistics and clip latch, the history, the quiet auto-reset, rest, RtScope and the cost per part.

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <complex>
#include <functional>
#include <string>
#include <random>
#include <vector>

#include "pq/core/aligned_buffer.h"
#include "pq/core/alloc_trap.h"
#include "pq/core/denormal_guard.h"
#include "pq/dsp/atom_state.h"
#include "pq/dsp/atoms.h"
#include "pq/dsp/loudness.h"

using namespace pq::dsp;

namespace {

constexpr double kSr = 48000.0;
constexpr double kPi = 3.14159265358979323846;

// Feeds `seconds` of gen(frame) → (l, r) in blocks of 512.
void feed(LoudnessMeter& m, double seconds, const std::function<void(uint64_t, float&, float&)>& gen, uint64_t& t) {
  std::vector<float> l(512), r(512);
  const auto total = static_cast<uint64_t>(seconds * kSr + 0.5);
  for (uint64_t done = 0; done < total;) {
    const auto n = static_cast<uint32_t>(std::min<uint64_t>(512, total - done));
    for (uint32_t i = 0; i < n; ++i) gen(t + i, l[i], r[i]);
    m.push(l.data(), r.data(), n);
    t += n;
    done += n;
  }
}
// A stereo 1 kHz sine at `dbfs` in both channels (EBU 3341 / 3342's test tone).
std::function<void(uint64_t, float&, float&)> tone(double dbfs, double hz = 1000.0) {
  const double a = std::pow(10.0, dbfs / 20.0);
  return [a, hz](uint64_t i, float& l, float& r) { l = r = static_cast<float>(a * std::sin(2.0 * kPi * hz * static_cast<double>(i) / kSr)); };
}
void silence(uint64_t, float& l, float& r) { l = r = 0.0f; }

LoudnessConfig cfg(uint32_t parts = kLoudLevels | kLoudLoudness | kLoudTruePeak | kLoudHistory) {
  LoudnessConfig c;
  c.sampleRate = kSr;
  c.parts = parts;
  c.autoReset = false;  // the EBU cases measure one programme
  return c;
}

// The integrated loudness after the segments (dBFS, seconds) of the 1 kHz tone, one after another.
float integratedOf(std::initializer_list<std::pair<double, double>> segs, float* lra = nullptr) {
  LoudnessMeter m(cfg(kLoudLoudness));
  uint64_t t = 0;
  for (const auto& [db, s] : segs) feed(m, s, tone(db), t);
  if (lra != nullptr) *lra = m.readings().lra;
  return m.readings().integrated;
}

}  // namespace

TEST_CASE("loudness: EBU Tech 3341 cases 1, 2 — a 1 kHz stereo sine at −23 / −33 dBFS reads −23 / −33 LUFS M, S and I (± 0.1)") {
  pq::DenormalGuard dg;
  for (const double db : {-23.0, -33.0}) {
    CAPTURE(db);
    LoudnessMeter m(cfg());
    uint64_t t = 0;
    feed(m, 20.0, tone(db), t);
    const LoudnessReadings r = m.readings();
    CHECK(std::fabs(r.momentary - db) <= 0.1);
    CHECK(std::fabs(r.shortTerm - db) <= 0.1);
    CHECK(std::fabs(r.integrated - db) <= 0.1);
    CHECK(std::fabs(r.maxMomentary - db) <= 0.1);
    CHECK(std::fabs(r.maxShortTerm - db) <= 0.1);
    CHECK(r.lra < 0.1f);  // a steady tone has no range
    CHECK(r.correlation == doctest::Approx(1.0).epsilon(1e-6));
  }
}

TEST_CASE("loudness: EBU Tech 3341 cases 3 – 5 — the relative (−10 LU) and absolute (−70 LUFS) gates: I = −23 ± 0.1") {
  pq::DenormalGuard dg;
  // 3: 10 s −36, 60 s −23, 10 s −36 (the quiet parts fall under the relative gate).
  CHECK(std::fabs(integratedOf({{-36.0, 10.0}, {-23.0, 60.0}, {-36.0, 10.0}}) + 23.0f) <= 0.1f);
  // 4: 10 s −72, 10 s −36, 60 s −23, 10 s −36, 10 s −72 (−72: under the absolute gate).
  CHECK(std::fabs(integratedOf({{-72.0, 10.0}, {-36.0, 10.0}, {-23.0, 60.0}, {-36.0, 10.0}, {-72.0, 10.0}}) + 23.0f) <= 0.1f);
  // 5: 20 s −26, 20.1 s −20, 20 s −26 (both survive the gate: the power mean).
  CHECK(std::fabs(integratedOf({{-26.0, 20.0}, {-20.0, 20.1}, {-26.0, 20.0}}) + 23.0f) <= 0.1f);
  // Without the gates case 3 would read about −24.6: the gate matters.
  CHECK(integratedOf({{-36.0, 10.0}, {-23.0, 60.0}, {-36.0, 10.0}}) > -23.1f);
  // Only under the absolute gate (or silence): nothing to report.
  CHECK(integratedOf({{-75.0, 10.0}}) == kNoLoudness);
  LoudnessMeter m(cfg(kLoudLoudness));
  uint64_t t = 0;
  feed(m, 5.0, silence, t);
  CHECK(m.readings().integrated == kNoLoudness);
  CHECK(m.readings().momentary == kNoLoudness);
}

TEST_CASE("loudness: EBU Tech 3342 cases 1 – 4 — LRA of stepped tones (± 1 LU)") {
  pq::DenormalGuard dg;
  float lra = 0.0f;
  (void)integratedOf({{-20.0, 20.0}, {-30.0, 20.0}}, &lra);
  CHECK(std::fabs(lra - 10.0f) <= 1.0f);
  (void)integratedOf({{-20.0, 20.0}, {-15.0, 20.0}}, &lra);
  CHECK(std::fabs(lra - 5.0f) <= 1.0f);
  (void)integratedOf({{-40.0, 20.0}, {-20.0, 20.0}}, &lra);
  CHECK(std::fabs(lra - 20.0f) <= 1.0f);
  (void)integratedOf({{-50.0, 20.0}, {-35.0, 20.0}, {-20.0, 20.0}, {-35.0, 20.0}, {-50.0, 20.0}}, &lra);
  CHECK(std::fabs(lra - 15.0f) <= 1.0f);
}

namespace {

// The truepeak atom over x (1 lane) in calls of 256: its largest output.
float atomTruePeak(const std::vector<float>& x) {
  StateInit init;
  init.lanes = 1;
  pq::AlignedBuffer<float> state(stateBytes(Atom::TruePeak, init) / 4 + 8);
  initState(Atom::TruePeak, state.data(), init);
  const KernelFn fn = findKernel(Atom::TruePeak, 1, 0);
  REQUIRE(fn != nullptr);
  std::vector<float> out(257);
  float m = 0.0f;
  for (size_t at = 0; at < x.size(); at += 256) {
    const auto n = static_cast<uint32_t>(std::min<size_t>(256, x.size() - at));
    const float* in[1] = {x.data() + at};
    float* o = out.data();
    const KernelArgs args{in, &o, state.data(), nullptr, n, 1, 1, 0};
    fn(args);
    for (uint32_t i = 0; i < n; ++i) m = std::max(m, out[i]);
  }
  // The atom's estimates trail by 6 frames: the last 11 frames' estimates come with 11 zeros after (the meter's
  // block-wise max sees them at the next push; here the tail is flushed for both).
  return m;
}

}  // namespace

TEST_CASE("loudness: true peak = the truepeak atom's estimate (same filter, same bits) — inter-sample peaks above the sample peak") {
  pq::DenormalGuard dg;
  std::mt19937 rng(3341);
  std::uniform_real_distribution<float> u(-1.0f, 1.0f), ph(0.0f, 6.2831853f);
  struct Case {
    double hz, phase, amp;
  };
  // sr/4 at 45°: every sample is at 0.707 of the peak (−3 dB under it); sr/2·0.45 and noise-like signals too.
  const Case cases[] = {{12000.0, kPi / 4.0, 0.5}, {12000.0, kPi / 4.0, 1.0}, {21600.0, 0.3, 0.8}, {997.0, 0.0, 0.9}};
  for (const Case& c : cases) {
    CAPTURE(c.hz);
    std::vector<float> x(48000 + 11);  // the tail: 11 zeros flush the atom's last estimates
    for (size_t i = 0; i < 48000; ++i) x[i] = static_cast<float>(c.amp * std::sin(2.0 * kPi * c.hz * static_cast<double>(i) / kSr + c.phase));
    LoudnessMeter m(cfg(kLoudLoudness | kLoudTruePeak));
    for (size_t at = 0; at < x.size(); at += 700) {
      const auto n = static_cast<uint32_t>(std::min<size_t>(700, x.size() - at));
      m.push(x.data() + at, x.data() + at, n);
    }
    const float atom = atomTruePeak(x);
    const LoudnessReadings r = m.readings();
    CHECK(r.truePeakDb == doctest::Approx(20.0 * std::log10(atom)).epsilon(1e-6));
    if (c.hz == 12000.0) {
      CHECK(r.samplePeakDb == doctest::Approx(20.0 * std::log10(c.amp * std::sqrt(0.5))).epsilon(1e-4));
      CHECK(r.truePeakDb - r.samplePeakDb > 2.7f);  // the inter-sample peak, ~3 dB over the samples
      CHECK(std::fabs(r.truePeakDb - 20.0f * std::log10(static_cast<float>(c.amp))) < 0.25f);  // Q?-DY.5-a: ≤ +0.22 dB
    }
  }
  // Random signals, L and R different: the max over both channels, still the atom's.
  std::vector<float> l(30000 + 11, 0.0f), r(30000 + 11, 0.0f);
  for (size_t i = 0; i < 30000; ++i) {
    l[i] = u(rng) * 0.6f;
    r[i] = u(rng) * (i > 15000 ? 1.2f : 0.3f);
  }
  LoudnessMeter m(cfg(kLoudTruePeak | kLoudLoudness));
  for (size_t at = 0; at < l.size(); at += 333) {
    const auto n = static_cast<uint32_t>(std::min<size_t>(333, l.size() - at));
    m.push(l.data() + at, r.data() + at, n);
  }
  const float want = std::max(atomTruePeak(l), atomTruePeak(r));
  CHECK(m.readings().truePeakDb == doctest::Approx(20.0 * std::log10(want)).epsilon(1e-6));
}

TEST_CASE("loudness: correlation +1 (L = R), −1 (L = −R), ~0 (independent noise); the unweighted RMS") {
  pq::DenormalGuard dg;
  auto corr = [](const std::function<void(uint64_t, float&, float&)>& g) {
    LoudnessMeter m(cfg(kLoudLoudness));
    uint64_t t = 0;
    feed(m, 1.0, g, t);
    return m.readings();
  };
  const auto sine = tone(-6.0, 440.0);
  CHECK(corr(sine).correlation == doctest::Approx(1.0).epsilon(1e-6));
  CHECK(corr([&](uint64_t i, float& l, float& r) {
          sine(i, l, r);
          r = -l;
        }).correlation == doctest::Approx(-1.0).epsilon(1e-6));
  std::mt19937 a(1), b(2);
  std::normal_distribution<float> n(0.0f, 0.2f);
  CHECK(std::fabs(corr([&](uint64_t, float& l, float& r) {
                    l = n(a);
                    r = n(b);
                  }).correlation) < 0.05f);
  CHECK(corr(sine).rmsDbMono == doctest::Approx(-6.0 - 3.0103).epsilon(1e-3));  // a sine's RMS: 3 dB under its peak
  CHECK(corr(silence).correlation == 0.0f);
}

TEST_CASE("loudness: the level meter — RMS window, peak fall, hold, clip events, auto clip reset, infinite hold") {
  pq::DenormalGuard dg;
  LoudnessConfig c = cfg(kLoudLevels);
  c.peakFallDbS = 20.0f;
  c.holdSeconds = 2.0f;
  LoudnessMeter m(c);
  uint64_t t = 0;
  feed(m, 2.0, tone(-6.0, 440.0), t);
  LoudnessReadings r = m.readings();
  CHECK(r.rmsDb[0] == doctest::Approx(-9.03).epsilon(0.002));
  CHECK(r.peakDb[1] == doctest::Approx(-6.0).epsilon(0.002));
  CHECK(r.holdDb[0] == doctest::Approx(-6.0).epsilon(0.002));
  CHECK_FALSE(r.clip[0]);
  // 1 s of silence: the peak falls 20 dB; the hold stays (2 s).
  feed(m, 1.0, silence, t);
  r = m.readings();
  CHECK(r.peakDb[0] == doctest::Approx(-26.0).epsilon(0.01));
  CHECK(r.holdDb[0] == doctest::Approx(-6.0).epsilon(0.002));
  feed(m, 1.5, silence, t);  // the hold expired at 2 s: it falls at the peak fall (0.5 s → about −10 dB)
  r = m.readings();
  CHECK(r.holdDb[0] < -14.0f);
  CHECK(r.holdDb[0] > -18.0f);
  // Three runs over full scale in L: three clips (a run is one event), R untouched.
  for (int k = 0; k < 3; ++k) {
    feed(m, 0.01, [](uint64_t, float& l, float& rr) { l = 1.2f, rr = 0.1f; }, t);
    feed(m, 0.05, silence, t);
  }
  r = m.readings();
  CHECK(r.clip[0]);
  CHECK(r.clipCount[0] == 3);
  CHECK_FALSE(r.clip[1]);
  feed(m, 60.0, silence, t);  // latched: manual reset only
  CHECK(m.readings().clip[0]);
  m.resetClips();
  CHECK_FALSE(m.readings().clip[0]);
  CHECK(m.readings().clipCount[0] == 0);
  // Auto reset 1 s after the last over; a lower threshold catches −2 dBFS.
  c.clipResetS = 1.0f;
  c.clipThreshDb = -3.0f;
  m.configure(c);
  feed(m, 0.1, tone(-2.0, 440.0), t);
  CHECK(m.readings().clip[1]);
  CHECK_FALSE(m.atRest());  // its reset is still to come
  feed(m, 1.2, silence, t);
  CHECK_FALSE(m.readings().clip[1]);
  // Infinite hold: the maximum stays.
  c.holdInfinite = true;
  m.configure(c);
  m.resetClips();
  feed(m, 0.2, tone(-12.0, 440.0), t);
  feed(m, 20.0, silence, t);
  CHECK(m.readings().holdDb[0] == doctest::Approx(-12.0).epsilon(0.002));
  CHECK(m.atRest());  // a held tick is still: more silence changes nothing
}

TEST_CASE("loudness: the history — one point per 100 ms (M, S, I at that moment); reset starts it again") {
  pq::DenormalGuard dg;
  LoudnessMeter m(cfg(kLoudLoudness | kLoudHistory));
  uint64_t t = 0;
  feed(m, 5.0, tone(-23.0), t);
  LoudnessReadings r = m.readings();
  CHECK(r.points == 50);
  std::vector<LoudnessPoint> p(64);
  REQUIRE(m.history(0, 64, p.data()) == 50);
  CHECK(p[2].momentary == kNoLoudness);  // 300 ms: no 400 ms block yet
  CHECK(std::fabs(p[3].momentary + 23.0f) < 0.1f);
  CHECK(p[28].shortTerm == kNoLoudness);
  CHECK(std::fabs(p[29].shortTerm + 23.0f) < 0.1f);
  CHECK(std::fabs(p[49].integrated + 23.0f) < 0.1f);
  m.resetMeasurement();
  r = m.readings();
  CHECK(r.historyStart == 50);
  CHECK(r.integrated == kNoLoudness);
  CHECK(r.maxMomentary == kNoLoudness);
  CHECK(std::fabs(r.momentary + 23.0f) < 0.1f);  // the windows keep running
  // The ring holds the last kHistory points.
  feed(m, 210.0, tone(-30.0), t);
  CHECK(m.history(0, 1, p.data()) == 0);
  CHECK(m.history(m.points() - LoudnessMeter::kHistory, 1, p.data()) == 1);
}

TEST_CASE("loudness: the quiet auto-reset clears only the integrated measurement; off, it never does") {
  pq::DenormalGuard dg;
  for (const bool on : {true, false}) {
    CAPTURE(on);
    LoudnessConfig c = cfg(kLoudLoudness | kLoudHistory);
    c.autoReset = on;
    c.targetLufs = -14.0f;
    c.autoResetBelowLu = 12.0f;  // quiet: momentary ≤ −26
    c.autoResetSec = 5.0f;
    LoudnessMeter m(c);
    uint64_t t = 0;
    feed(m, 10.0, tone(-14.0), t);
    feed(m, 4.0, tone(-30.0), t);  // quiet 4 s: not yet
    CHECK(m.readings().integrated > -15.0f);
    CHECK_FALSE(m.atRest());
    feed(m, 2.0, silence, t);
    const LoudnessReadings r = m.readings();
    if (on) {
      CHECK(r.integrated == kNoLoudness);
      CHECK(r.maxShortTerm > -14.5f);  // the maxima keep running
    } else {
      CHECK(r.integrated > -15.0f);
    }
    feed(m, 4.0, silence, t);
    CHECK(m.atRest());
  }
}

TEST_CASE("loudness: rest — 3.5 s of silence and every falling level under −100 dB; sound wakes it") {
  pq::DenormalGuard dg;
  LoudnessConfig c = cfg();
  c.peakFallDbS = 60.0f;
  LoudnessMeter m(c);
  uint64_t t = 0;
  CHECK_FALSE(m.atRest());
  feed(m, 1.0, tone(-10.0), t);
  feed(m, 3.0, silence, t);
  CHECK_FALSE(m.atRest());  // 3 s
  feed(m, 0.6, silence, t);
  CHECK_FALSE(m.atRest());  // the RMS (300 ms one-pole) and the falling true peak (0.6 s) are still over −100 dB
  feed(m, 3.0, silence, t);
  CHECK(m.atRest());
  feed(m, 0.01, tone(-60.0), t);
  CHECK_FALSE(m.atRest());
}

TEST_CASE("loudness: push is allocation-free (RtScope), any block size") {
  pq::DenormalGuard dg;
  LoudnessMeter m(cfg());
  std::vector<float> l(5000), r(5000);
  for (size_t i = 0; i < l.size(); ++i) l[i] = r[i] = static_cast<float>(std::sin(0.01 * static_cast<double>(i)));
  uint64_t v = 0;
  for (const uint32_t n : {1u, 7u, 512u, 1024u, 1025u, 4096u, 5000u}) v += pq::alloc_trap::countRtViolations([&] { m.push(l.data(), r.data(), n); });
  v += pq::alloc_trap::countRtViolations([&] {
    m.resetMeasurement();
    m.resetClips();
    (void)m.readings();
  });
  CHECK(v == 0);
}

TEST_CASE("loudness: worker cost per second of stereo audio, by face (Level Meter / LUFS Meter / Loudness History)") {
  pq::DenormalGuard dg;
  struct Face {
    const char* name;
    uint32_t parts;
  };
  const Face faces[] = {{"Level Meter", kLoudLevels},
                        {"LUFS Meter", kLoudLoudness | kLoudTruePeak | kLoudHistory},
                        {"Loudness History", kLoudLoudness | kLoudHistory}};
  std::vector<float> l(4096), r(4096);
  std::mt19937 rng(9);
  std::uniform_real_distribution<float> u(-0.5f, 0.5f);
  for (size_t i = 0; i < l.size(); ++i) l[i] = u(rng), r[i] = u(rng);
  for (const Face& f : faces) {
    LoudnessMeter m(cfg(f.parts));
    constexpr int kSeconds = 10;
    const auto t0 = std::chrono::steady_clock::now();
    for (int k = 0; k < kSeconds * 48000 / 4096; ++k) m.push(l.data(), r.data(), 4096);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / kSeconds;
    MESSAGE(std::string(f.name) << ": " << ms << " ms of worker time per second of audio (" << ms / 10.0 << " % of one core)");
    CHECK(ms < 50.0);  // a loose bound (unoptimised builds); the optimised figures are in the PROGRESS entry
  }
}

// ---- VZ.9 (docs/reviews/VZ.md: Tests, M10, L7) -------------------------------------------------------------------

namespace {

// |H(f)| in dB of a biquad cascade (b0 b1 b2 a1 a2 per section) at fs.
double responseDb(const double (&s)[5], const double (&h)[5], double f, double fs) {
  const std::complex<double> z = std::polar(1.0, -2.0 * kPi * f / fs);  // z⁻¹
  auto sec = [&](const double (&c)[5]) {
    return (c[0] + c[1] * z + c[2] * z * z) / (1.0 + c[3] * z + c[4] * z * z);
  };
  return 20.0 * std::log10(std::abs(sec(s) * sec(h)));
}

// Feeds seconds of gen at sr in blocks of 512.
void feedAt(LoudnessMeter& m, double sr, double seconds, const std::function<void(uint64_t, float&, float&)>& gen,
            uint64_t& t) {
  std::vector<float> l(512), r(512);
  const auto total = static_cast<uint64_t>(seconds * sr + 0.5);
  for (uint64_t done = 0; done < total;) {
    const auto n = static_cast<uint32_t>(std::min<uint64_t>(512, total - done));
    for (uint32_t i = 0; i < n; ++i) gen(t + i, l[i], r[i]);
    m.push(l.data(), r.data(), n);
    t += n;
    done += n;
  }
}

}  // namespace

TEST_CASE("loudness: the K-weighting — BS.1770's 48 kHz coefficients to 1e-9; its response at other rates") {
  double s[5], h[5];
  kWeighting(48000.0, s, h);
  // ITU-R BS.1770-4, Table 1 (pre-filter) and Table 2 (RLB high-pass).
  CHECK(std::fabs(s[0] - 1.53512485958697) < 1e-9);
  CHECK(std::fabs(s[1] + 2.69169618940638) < 1e-9);
  CHECK(std::fabs(s[2] - 1.19839281085285) < 1e-9);
  CHECK(std::fabs(s[3] + 1.69065929318241) < 1e-9);
  CHECK(std::fabs(s[4] - 0.73248077421585) < 1e-9);
  CHECK(h[0] == 1.0);
  CHECK(h[1] == -2.0);
  CHECK(h[2] == 1.0);
  CHECK(std::fabs(h[3] + 1.99004745483398) < 1e-9);
  CHECK(std::fabs(h[4] - 0.99007225036621) < 1e-9);
  // 1 kHz: +0.691 dB (the −0.691 of the loudness formula cancels it); the curve's shape at 25 / 100 Hz (the high-pass)
  // and 10 kHz (the shelf), the same at every rate within 0.05 dB (pre-warped at f0; the bilinear map elsewhere).
  CHECK(std::fabs(responseDb(s, h, 1000.0, 48000.0) - 0.691) < 0.01);  // (+0.697: the 0.691 is BS.1770's rounding)
  for (const double fs : {44100.0, 96000.0, 192000.0}) {
    double s2[5], h2[5];
    kWeighting(fs, s2, h2);
    for (const double f : {25.0, 100.0, 1000.0, 10000.0}) {
      CAPTURE(fs);
      CAPTURE(f);
      CHECK(std::fabs(responseDb(s2, h2, f, fs) - responseDb(s, h, f, 48000.0)) < 0.05);
    }
  }
  CHECK(responseDb(s, h, 25.0, 48000.0) < -5.0);   // the RLB high-pass
  CHECK(responseDb(s, h, 10000.0, 48000.0) > 3.5);  // the +4 dB shelf
}

TEST_CASE("loudness: EBU Tech 3341 cases 1, 2 at 44.1 and 96 kHz") {
  pq::DenormalGuard dg;
  for (const double sr : {44100.0, 96000.0})
    for (const double db : {-23.0, -33.0}) {
      CAPTURE(sr);
      CAPTURE(db);
      LoudnessConfig c = cfg(kLoudLoudness);
      c.sampleRate = sr;
      LoudnessMeter m(c);
      const double a = std::pow(10.0, db / 20.0);
      uint64_t t = 0;
      feedAt(m, sr, 20.0, [&](uint64_t i, float& l, float& r) {
        l = r = static_cast<float>(a * std::sin(2.0 * kPi * 1000.0 * static_cast<double>(i) / sr));
      }, t);
      const LoudnessReadings rd = m.readings();
      CHECK(std::fabs(rd.momentary - db) <= 0.1);
      CHECK(std::fabs(rd.shortTerm - db) <= 0.1);
      CHECK(std::fabs(rd.integrated - db) <= 0.1);
    }
}

// Modelled on Tech 3341's short-term / momentary cases: alternations whose every window means −23 LUFS (constant
// short-term / momentary), and bursts of exactly one window at −23 LUFS placed off the 100 ms grid (the maxima, M10).
TEST_CASE("loudness: Tech 3341-style S / M cases — constant S and M of alternations; max S and M of off-grid bursts") {
  pq::DenormalGuard dg;
  auto alternating = [](double aSec, double aDb, double bSec, double bDb) {
    const double ga = std::pow(10.0, aDb / 20.0), gb = std::pow(10.0, bDb / 20.0), period = aSec + bSec;
    return [=](uint64_t i, float& l, float& r) {
      const double ts = static_cast<double>(i) / kSr;
      const double g = std::fmod(ts, period) < aSec ? ga : gb;
      l = r = static_cast<float>(g * std::sin(2.0 * kPi * 1000.0 * ts));
    };
  };
  {  // 1.34 s at −20, 1.66 s at −30 dBFS: every 3 s window means −23.0 LUFS.
    LoudnessMeter m(cfg(kLoudLoudness | kLoudHistory));
    uint64_t t = 0;
    feed(m, 30.0, alternating(1.34, -20.0, 1.66, -30.0), t);
    std::vector<LoudnessPoint> p(300);
    REQUIRE(m.history(0, 300, p.data()) == 300);
    for (size_t k = 40; k < 300; ++k) CHECK(std::fabs(p[k].shortTerm + 23.0f) <= 0.1f);
  }
  {  // (the same for the momentary) 0.18 s at −20, 0.22 s at −30: every 400 ms window means −23.0.
    LoudnessMeter m(cfg(kLoudLoudness | kLoudHistory));
    uint64_t t = 0;
    feed(m, 10.0, alternating(0.18, -20.0, 0.22, -30.0), t);
    std::vector<LoudnessPoint> p(100);
    REQUIRE(m.history(0, 100, p.data()) == 100);
    for (size_t k = 5; k < 100; ++k) CHECK(std::fabs(p[k].momentary + 23.0f) <= 0.1f);
  }
  // One burst of exactly the window (3 s / 0.4 s) at −23 LUFS in silence, starting anywhere against the grid:
  // the max reads −23 ± 0.1 (on a 100 ms grid a 0.4 s burst 50 ms off read −23.58).
  for (const double win : {3.0, 0.4})
    for (const double off : {0.0, 0.013, 0.031, 0.05, 0.067, 0.089}) {
      CAPTURE(win);
      CAPTURE(off);
      LoudnessMeter m(cfg(kLoudLoudness));
      uint64_t t = 0;
      feed(m, 1.0 + off, silence, t);
      const uint64_t start = t;
      const double a = std::pow(10.0, -23.0 / 20.0);
      feed(m, win, [&](uint64_t i, float& l, float& r) {
        l = r = static_cast<float>(a * std::sin(2.0 * kPi * 1000.0 * static_cast<double>(i - start) / kSr));
      }, t);
      feed(m, 4.0, silence, t);
      const LoudnessReadings r = m.readings();
      CHECK(std::fabs((win > 1.0 ? r.maxShortTerm : r.maxMomentary) + 23.0f) <= 0.1f);
    }
}

TEST_CASE("loudness: after a reset the gate and LRA take only audio from after it (L7)") {
  pq::DenormalGuard dg;
  // 10 s loud, reset, then −40: the first 400 ms block after the reset must not carry the loud audio into the gate.
  LoudnessMeter m(cfg(kLoudLoudness));
  uint64_t t = 0;
  feed(m, 10.0, tone(-10.0), t);
  m.resetMeasurement();
  feed(m, 0.3, tone(-40.0), t);
  CHECK(m.readings().integrated == kNoLoudness);  // 3 sub-blocks since the reset: no 400 ms block of its own yet
  CHECK(std::fabs(m.readings().momentary + 16.0f) < 0.3f);  // the live momentary runs across it: (0.1 + 3e-4) / 4
  feed(m, 6.1, tone(-40.0), t);
  const LoudnessReadings r2 = m.readings();
  CHECK(std::fabs(r2.integrated + 40.0f) <= 0.1f);
  // The K-filter's own memory of the loud tone (a few ms of its tail) is in the first window after the reset: within
  // 0.5 LU. (Before L7 the first windows held up to 300 ms / 2.9 s of the -10 audio: maxima near -16.)
  CHECK(std::fabs(r2.maxMomentary + 40.0f) <= 0.5f);
  CHECK(std::fabs(r2.maxShortTerm + 40.0f) <= 0.5f);
}
