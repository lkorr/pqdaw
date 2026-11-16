// VZ.5: the stereo analysis (pq/dsp/stereo.h) — the cross-spectrum (a sine panned to p reads p ± 0.02; identical channels
// width 0; L = −R loses everything summed to mono at every frequency; uncorrelated noise coherence ~0; identical channels
// +3 dB mono), the heat-map tracker on two panned tones, glow and afterglow, rest, RtScope and the cost per face; the
// goniometer state (correlation +1 / −1 / ~0, LR4 bands split a tone into its band).

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "pq/core/alloc_trap.h"
#include "pq/core/denormal_guard.h"
#include "pq/dsp/stereo.h"

using namespace pq::dsp;

namespace {

constexpr double kSr = 48000.0;
constexpr double kPi = 3.14159265358979323846;

void feed(CrossSpectrum& s, double seconds, const std::function<void(uint64_t, float&, float&)>& gen, uint64_t& t) {
  std::vector<float> l(512), r(512);
  const auto total = static_cast<uint64_t>(seconds * kSr);
  for (uint64_t done = 0; done < total; done += 512) {
    for (uint32_t i = 0; i < 512; ++i) gen(t + i, l[i], r[i]);
    s.push(l.data(), r.data(), 512);
    t += 512;
  }
}
// The point nearest hz.
const StereoPoint& at(const CrossSpectrum& s, float hz) {
  const StereoPoint* best = &s.points()[0];
  for (const StereoPoint& p : s.points())
    if (std::fabs(std::log(p.hz / hz)) < std::fabs(std::log(best->hz / hz))) best = &p;
  return *best;
}

}  // namespace

TEST_CASE("stereo: a sine panned to p reads p ± 0.02 (power balance), identical channels width 0") {
  pq::DenormalGuard dg;
  for (const double p : {-1.0, -0.6, -0.25, 0.0, 0.4, 0.8, 1.0}) {
    CAPTURE(p);
    CrossSpectrum s;
    const double gl = std::sqrt((1.0 - p) / 2.0), gr = std::sqrt((1.0 + p) / 2.0);
    uint64_t t = 0;
    feed(s, 2.0, [&](uint64_t i, float& l, float& r) {
      const double v = 0.5 * std::sin(2.0 * kPi * 1000.0 * static_cast<double>(i) / kSr);
      l = static_cast<float>(gl * v);
      r = static_cast<float>(gr * v);
    }, t);
    const StereoPoint& pt = at(s, 1000.0f);
    CHECK(std::fabs(pt.pos - p) <= 0.02);
    CHECK(pt.width < 0.02f);  // one source in both channels: coherent
    CHECK(pt.db > -20.0f);
  }
}

TEST_CASE("stereo: L = −R loses everything summed to mono at every frequency; L = R gains +3 dB; noise coherence ~0") {
  pq::DenormalGuard dg;
  CrossSpectrumConfig c;
  c.fftSize = 8192;  // the Phase Scope's analysis
  c.hop = 2048;
  c.window = SpecWindow::Hann;
  c.coherenceMs = c.attackMs = c.releaseMs = 500.0f;
  c.points = 512;
  std::mt19937 a(5), b(6);
  std::normal_distribution<float> n(0.0f, 0.2f);
  {
    CrossSpectrum s(c);
    uint64_t t = 0;
    feed(s, 4.0, [&](uint64_t, float& l, float& r) { l = n(a), r = -l; }, t);
    for (const StereoPoint& p : s.points()) {
      CAPTURE(p.hz);
      CHECK(p.monoDb < -60.0f);
      CHECK(std::fabs(std::fabs(p.phaseDeg) - 180.0f) < 1.0f);
      CHECK(p.coherence > 0.99f);
    }
  }
  {
    CrossSpectrum s(c);
    uint64_t t = 0;
    feed(s, 4.0, [&](uint64_t, float& l, float& r) { l = r = n(a); }, t);
    for (const StereoPoint& p : s.points()) {
      CHECK(p.monoDb == doctest::Approx(3.0103).epsilon(0.001));
      CHECK(p.width < 0.01f);
    }
  }
  {
    c.coherenceMs = 3000.0f;  // a long average: the estimate's bias falls with the frames averaged
    CrossSpectrum s(c);
    uint64_t t = 0;
    feed(s, 12.0, [&](uint64_t, float& l, float& r) { l = n(a), r = n(b); }, t);
    double mean = 0.0, mono = 0.0;
    uint32_t k = 0;
    for (const StereoPoint& p : s.points())
      if (p.hz > 200.0f) {
        mean += p.coherence;
        mono += p.monoDb;
        ++k;
      }
    mean /= k;
    mono /= k;
    MESSAGE("uncorrelated noise: mean coherence " << mean << ", mean mono " << mono << " dB");
    CHECK(mean < 0.12);
    CHECK(std::fabs(mono) < 0.5);  // uncorrelated: summing neither gains nor loses
  }
}

TEST_CASE("stereo: the heat-map tracker follows two tones panned apart; glow deposits at the position; afterglow fades") {
  pq::DenormalGuard dg;
  CrossSpectrumConfig c;
  c.parts = kStereoParticles | kStereoGlow | kStereoAfterglow;
  CrossSpectrum s(c);
  uint64_t t = 0;
  auto tones = [](uint64_t i, float& l, float& r) {
    const double x = static_cast<double>(i) / kSr;
    const double lo = 0.4 * std::sin(2.0 * kPi * 220.0 * x), hi = 0.4 * std::sin(2.0 * kPi * 3000.0 * x);
    l = static_cast<float>(lo * 0.95 + hi * 0.2);  // 220 Hz left, 3 kHz right
    r = static_cast<float>(lo * 0.2 + hi * 0.95);
  };
  feed(s, 1.5, tones, t);
  int lo = 0, hi = 0, other = 0;
  for (const StereoParticle& p : s.particles()) {
    if (!p.alive || p.energy < 0.3f) continue;
    const float hz = std::exp2(p.logF);
    if (std::fabs(hz / 220.0f - 1.0f) < 0.03f && p.pan < -0.7f) ++lo;
    else if (std::fabs(hz / 3000.0f - 1.0f) < 0.03f && p.pan > 0.7f) ++hi;
    else ++other;
  }
  CHECK(lo == 1);
  CHECK(hi == 1);
  CHECK(other == 0);
  // Glow: the 3 kHz row lights on the right.
  const auto row = [&](float hz) {
    uint32_t best = 0;
    for (uint32_t p = 0; p < s.points().size(); ++p)
      if (std::fabs(std::log(s.points()[p].hz / hz)) < std::fabs(std::log(s.points()[best].hz / hz))) best = p;
    return s.heat().subspan(static_cast<size_t>(best) * CrossSpectrum::kHeatCols, CrossSpectrum::kHeatCols);
  };
  const auto r3k = row(3000.0f);
  // pos = (0.95² − 0.2²) / (0.95² + 0.2²) ≈ 0.915: column (0.915 / 2 + 0.5) · 127 ≈ 122.
  CHECK(r3k[122] > 0.5f);
  CHECK(r3k[64] < 0.05f);
  CHECK(r3k[10] < 0.05f);
  CHECK(at(s, 3000.0f).glowLevel > 0.5f);
  // Silence: everything fades and the analyser rests.
  CHECK_FALSE(s.atRest());
  feed(s, 5.0, [](uint64_t, float& l, float& r) { l = r = 0.0f; }, t);
  CHECK(s.atRest());
  for (const StereoParticle& p : s.particles()) CHECK_FALSE(p.alive);
}

TEST_CASE("stereo field: correlation +1 / −1 / ~0; LR4 bands carry a tone in its own band") {
  pq::DenormalGuard dg;
  auto run = [](StereoFieldState& f, const std::function<void(uint64_t, float&, float&)>& g, double seconds) {
    std::vector<float> l(480), r(480);
    for (uint64_t at = 0; at < static_cast<uint64_t>(seconds * kSr); at += 480) {
      for (uint32_t i = 0; i < 480; ++i) g(at + i, l[i], r[i]);
      f.push(l.data(), r.data(), 480);
    }
  };
  auto sine = [](double hz) {
    return [hz](uint64_t i, float& l, float& r) { l = r = static_cast<float>(0.5 * std::sin(2.0 * kPi * hz * static_cast<double>(i) / kSr)); };
  };
  StereoFieldState f;
  f.configure(kSr, 300.0f, false, 250.0f, 4000.0f);
  run(f, sine(440.0), 1.0);
  CHECK(f.correlation() == doctest::Approx(1.0).epsilon(1e-4));
  CHECK(f.radius() == doctest::Approx(0.5 * std::sqrt(2.0)).epsilon(0.01));
  f.reset();
  run(f, [&](uint64_t i, float& l, float& r) { sine(440.0)(i, l, r), r = -l; }, 1.0);
  CHECK(f.correlation() == doctest::Approx(-1.0).epsilon(1e-4));
  std::mt19937 a(1), b(2);
  std::normal_distribution<float> n(0.0f, 0.2f);
  f.reset();
  run(f, [&](uint64_t, float& l, float& r) { l = n(a), r = n(b); }, 2.0);
  CHECK(std::fabs(f.correlation()) < 0.1f);
  // Bands: 60 Hz sits in Low, 1 kHz in Mid, 10 kHz in High.
  for (const auto& [hz, band] : {std::pair{60.0, 0u}, std::pair{1000.0, 1u}, std::pair{10000.0, 2u}}) {
    CAPTURE(hz);
    StereoFieldState g;
    g.configure(kSr, 300.0f, true, 250.0f, 4000.0f);
    run(g, sine(hz), 1.0);
    for (uint32_t k = 0; k < StereoFieldState::kBands; ++k) {
      if (k == band) CHECK(g.bandRadius(k) > 0.6f);
      else CHECK(g.bandRadius(k) < 0.1f);
    }
    CHECK(g.bandCorrelation(band) == doctest::Approx(1.0).epsilon(1e-3));
    std::vector<float> l(64), r(64);
    g.bandRead(band, g.bandWritten() - 64, 64, l.data(), r.data());
    float m = 0.0f;
    for (const float v : l) m = std::max(m, std::fabs(v));
    CHECK(m > 0.4f);
  }
}

TEST_CASE("stereo: push is allocation-free (RtScope); the cost per face per second of stereo") {
  pq::DenormalGuard dg;
  std::vector<float> l(4096), r(4096);
  std::mt19937 rng(3);
  std::uniform_real_distribution<float> u(-0.5f, 0.5f);
  for (size_t i = 0; i < l.size(); ++i) l[i] = u(rng), r[i] = u(rng);
  struct Face {
    const char* name;
    CrossSpectrumConfig c;
  };
  CrossSpectrumConfig ribbon, heat, phase;
  heat.parts = kStereoParticles;
  CrossSpectrumConfig glow;
  glow.parts = kStereoGlow;
  phase.fftSize = 8192;
  phase.hop = 2048;
  phase.window = SpecWindow::Hann;
  phase.points = 512;
  const Face faces[] = {{"Stereo Image (ribbon)", ribbon}, {"Stereo Image (glow)", glow}, {"Stereo Image (heat map)", heat},
                        {"Phase Scope", phase}};
  for (const Face& f : faces) {
    CrossSpectrum s(f.c);
    CHECK(pq::alloc_trap::countRtViolations([&] { s.push(l.data(), r.data(), 4096); }) == 0);
    constexpr int kBlocks = 5 * 48000 / 4096;
    const auto t0 = std::chrono::steady_clock::now();
    for (int k = 0; k < kBlocks; ++k) s.push(l.data(), r.data(), 4096);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 5.0;
    MESSAGE(std::string(f.name) << ": " << ms << " ms of worker time per second of audio");
    CHECK(ms < 200.0);  // loose (unoptimised builds); the optimised figures are in the PROGRESS entry
  }
  StereoFieldState g;
  g.configure(kSr, 300.0f, true, 250.0f, 4000.0f);
  CHECK(pq::alloc_trap::countRtViolations([&] { g.push(l.data(), r.data(), 4096); }) == 0);
  const auto t0 = std::chrono::steady_clock::now();
  for (int k = 0; k < 5 * 48000 / 4096; ++k) g.push(l.data(), r.data(), 4096);
  MESSAGE("Stereo Field (bands, on the UI thread): "
          << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 5.0 << " ms per second");
}
