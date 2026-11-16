// VZ.7: the 3D analysers' analysis — the waterfall (slices of the spectrum analysis: a tone's level at its frequency, the
// slice pitch from the span, the span clamped by the window, channels, rest) and the stereo cloud (a panned tone's points
// at its frequency and pan, reassigned in time, ≤ 64 a hop spent across the bands, the ribbon's pan, silence), RtScope
// and the worker cost per face.

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <random>
#include <string>
#include <vector>

#include "pq/core/alloc_trap.h"
#include "pq/core/denormal_guard.h"
#include "pq/dsp/stereo_cloud.h"
#include "pq/dsp/waterfall.h"

using namespace pq::dsp;

namespace {

constexpr double kSr = 48000.0, kPi = 3.14159265358979323846;

template <class Push, class Gen>
void run(Push&& push, double seconds, Gen&& gen, uint64_t& t) {
  std::vector<float> l(512), r(512);
  for (uint64_t done = 0; done < static_cast<uint64_t>(seconds * kSr); done += 512) {
    for (uint32_t i = 0; i < 512; ++i) gen(t + i, l[i], r[i]);
    push(l.data(), r.data(), 512u);
    t += 512;
  }
}

}  // namespace

TEST_CASE("waterfall: a tone reads its level at its frequency; slices at the span's pitch; the span bounded by the window") {
  pq::DenormalGuard dg;
  WaterfallConfig c;
  WaterfallAnalyzer w(c);
  CHECK(w.slicePitch() == static_cast<uint32_t>(std::lround(6.0 * kSr / WaterfallAnalyzer::kTargetSlices)));
  CHECK(w.analysisHop() == 533);
  uint64_t t = 0;
  const double a = std::pow(10.0, -12.0 / 20.0);
  run([&](const float* l, const float* r, uint32_t n) { w.push(l, r, n); }, 2.0,
      [&](uint64_t i, float& l, float& r) { l = r = static_cast<float>(a * std::sin(2.0 * kPi * 1000.0 * static_cast<double>(i) / kSr)); }, t);
  CHECK(w.slices() == static_cast<uint64_t>(t / w.slicePitch()));
  const auto live = w.live();
  uint32_t peak = 0;
  for (uint32_t p = 0; p < WaterfallAnalyzer::kPoints; ++p)
    if (live[p] > live[peak]) peak = p;
  const FreqAxis axis(FreqScale::Log, WaterfallAnalyzer::kFMin, WaterfallAnalyzer::kFMax);
  CHECK(std::fabs(axis.hz(peak / 511.0f) / 1000.0f - 1.0f) < 0.02f);
  CHECK(std::fabs(live[peak] + 12.0f) < 1.5f);
  const float* newest = w.slice(w.slices() - 1);
  REQUIRE(newest != nullptr);
  CHECK(std::fabs(newest[peak] + 12.0f) < 1.5f);
  CHECK(w.slice(0) != nullptr);
  // The side of L = R is silence; the left channel alone reads the tone.
  c.channel = WaterfallChannel::Side;
  w.configure(c);
  CHECK(w.slices() == 0);  // a channel change restarts
  run([&](const float* l, const float* r, uint32_t n) { w.push(l, r, n); }, 1.0,
      [&](uint64_t i, float& l, float& r) { l = r = static_cast<float>(a * std::sin(2.0 * kPi * 1000.0 * static_cast<double>(i) / kSr)); }, t);
  float mx = -200.0f;
  for (const float v : w.live()) mx = std::max(mx, v);
  CHECK(mx < -100.0f);
  // A 512 window holds 300 slices of ≤ 512 samples: 3.2 s.
  c.fftSize = 512;
  c.spanSec = 12.0f;
  w.configure(c);
  CHECK(w.config().spanSec == doctest::Approx(3.2f));
  // Silence for the span: at rest.
  CHECK_FALSE(w.atRest());
  run([&](const float* l, const float* r, uint32_t n) { w.push(l, r, n); }, 4.0, [](uint64_t, float& l, float& r) { l = r = 0.0f; }, t);
  CHECK(w.atRest());
}

TEST_CASE("stereo cloud: a panned tone's points sit at its frequency and pan; reassigned near the frame; the ribbon's pan") {
  pq::DenormalGuard dg;
  StereoCloud s;
  uint64_t t = 0;
  std::vector<CloudPoint> pts;
  uint32_t maxHop = 0, hops = 0;
  StereoCloud::RibSlice last;
  auto push = [&](const float* l, const float* r, uint32_t n) {
    s.push(l, r, n,
           [&](const CloudPoint* p, uint32_t k) {
             pts.insert(pts.end(), p, p + k);
             maxHop = std::max(maxHop, k);
             ++hops;
           },
           [&](const StereoCloud::RibSlice& sl) { last = sl; });
  };
  const double gl = std::sqrt(0.25), gr = std::sqrt(0.75);  // power balance 0.5
  run(push, 3.0, [&](uint64_t i, float& l, float& r) {
    const double v = 0.5 * std::sin(2.0 * kPi * 1000.0 * static_cast<double>(i) / kSr);
    l = static_cast<float>(gl * v), r = static_cast<float>(gr * v);
  }, t);
  CHECK(hops == static_cast<uint32_t>(t / s.hop()));
  REQUIRE(pts.size() > 100);
  // Power-weighted the tone's points are all of it (> 99.99 %). The strays are sidelobe bins the port keeps as PQ does: far
  // ones sitting on the leakage mask (36 dB + 20 log10(distance) under the tone, PQ's envelope; this FFT's Kaiser
  // leakage measures right on it at this bin offset) and near ones whose neighbours agree in time (a steady tone's
  // bins all reassign to the frame's centre) — every one at least 50 dB under the tone (measured: ≤ −57 dB).
  // (The switch-on — a step from silence, genuinely broadband — is left out: steady state from the third window on.)
  const double settle = 2.0 * s.config().fftSize;
  double near = 0.0, onPitch = 0.0, total = 0.0, strongest = 0.0;
  for (const CloudPoint& p : pts) {
    if (p.tSamples < settle) continue;
    total += p.power;
    strongest = std::max(strongest, static_cast<double>(p.power));
    if (std::fabs(p.freqHz - 1000.0f) < 1.0f) onPitch += p.power;
    if (std::fabs(p.freqHz - 1000.0f) < 10.0f) {
      near += p.power;
      CHECK(std::fabs(p.pan - 0.5f) < 0.02f);
    }
  }
  CHECK(near / total > 0.9999);
  // Reassigned onto the tone's frequency (bins are 23 Hz apart): over 99 % of the power within 1 Hz (measured 99.3 %:
  // the mainlobe's outer bins land a few Hz off; the sampled analytic window derivative, as PQ's).
  CHECK(onPitch / total > 0.99);
  for (const CloudPoint& p : pts)
    if (p.tSamples >= settle && std::fabs(p.freqHz - 1000.0f) >= 10.0f) CHECK(10.0 * std::log10(p.power / strongest) < -50.0);
  CHECK(std::fabs(pts.back().tSamples - s.head()) < s.config().fftSize * 0.5);
  uint32_t b1k = 0;
  const FreqAxis axis(FreqScale::Log, 10.0f, 24000.0f);
  for (uint32_t b = 0; b < StereoCloud::kRibBands; ++b)
    if (std::fabs(std::log(axis.hz(b / 159.0f) / 1000.0f)) < std::fabs(std::log(axis.hz(b1k / 159.0f) / 1000.0f))) b1k = b;
  CHECK(std::fabs(last.pos[b1k] - 0.5f) < 0.02f);
  CHECK(last.width[b1k] < 0.02f);
  // Noise: the hop's points are capped, spread over the bands.
  std::mt19937 ra(1), rb(2);
  std::normal_distribution<float> nz(0.0f, 0.2f);
  pts.clear();
  maxHop = 0;
  run(push, 0.5, [&](uint64_t, float& l, float& r) { l = nz(ra), r = nz(rb); }, t);
  CHECK(maxHop <= StereoCloud::kMaxPointsPerHop);
  int bands[StereoCloud::kPointBands] = {};
  for (const CloudPoint& p : pts) ++bands[std::clamp(static_cast<int>(axis.unit(p.freqHz) * 8.0f), 0, 7)];
  int lit = 0;
  for (const int b : bands) lit += b > 0 ? 1 : 0;
  CHECK(lit >= 5);
  // Silence: no points; at rest after the span.
  pts.clear();
  run(push, 7.0, [](uint64_t, float& l, float& r) { l = r = 0.0f; }, t);
  size_t late = 0;
  for (const CloudPoint& p : pts) late += p.tSamples > static_cast<double>(t) - 5.0 * kSr ? 1 : 0;
  CHECK(late == 0);
  CHECK(s.atRest());
}

TEST_CASE("stereo cloud: time reassignment puts an impulse's points at its sample (± 2), whichever frame holds it") {
  pq::DenormalGuard dg;
  for (const uint64_t n0 : {uint64_t{30000}, uint64_t{30077}, uint64_t{30500}}) {
    CAPTURE(n0);
    StereoCloud s;
    std::vector<CloudPoint> pts;
    auto push = [&](const float* l, const float* r, uint32_t n) {
      s.push(l, r, n, [&](const CloudPoint* p, uint32_t k) { pts.insert(pts.end(), p, p + k); },
             [](const StereoCloud::RibSlice&) {});
    };
    uint64_t t = 0;
    run(push, 1.0, [&](uint64_t i, float& l, float& r) { l = r = i == n0 ? 0.8f : 0.0f; }, t);
    REQUIRE(!pts.empty());
    double strongest = 0.0;
    for (const CloudPoint& p : pts) strongest = std::max(strongest, static_cast<double>(p.power));
    size_t counted = 0;
    for (const CloudPoint& p : pts) {
      if (p.power < strongest * 1e-3) continue;  // within 30 dB of the strongest
      ++counted;
      CHECK(std::fabs(p.tSamples - static_cast<double>(n0)) < 2.0);
    }
    CHECK(counted > 8);
  }
}

TEST_CASE("3D analysers: push is allocation-free (RtScope); worker cost per face per second of stereo") {
  pq::DenormalGuard dg;
  std::vector<float> l(4096), r(4096);
  std::mt19937 rng(4);
  std::uniform_real_distribution<float> u(-0.5f, 0.5f);
  for (size_t i = 0; i < l.size(); ++i) l[i] = u(rng), r[i] = u(rng);
  WaterfallAnalyzer w;
  StereoCloud s;
  size_t got = 0;
  auto onPts = [&](const CloudPoint*, uint32_t k) { got += k; };
  auto onSlice = [&](const StereoCloud::RibSlice&) {};
  CHECK(pq::alloc_trap::countRtViolations([&] { w.push(l.data(), r.data(), 4096); }) == 0);
  CHECK(pq::alloc_trap::countRtViolations([&] { s.push(l.data(), r.data(), 4096, onPts, onSlice); }) == 0);
  constexpr int kBlocks = 5 * 48000 / 4096;
  auto time = [&](auto&& f) {
    const auto t0 = std::chrono::steady_clock::now();
    for (int k = 0; k < kBlocks; ++k) f();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 5.0;
  };
  const double wf = time([&] { w.push(l.data(), r.data(), 4096); });
  const double cl = time([&] { s.push(l.data(), r.data(), 4096, onPts, onSlice); });
  MESSAGE("Waterfall: " << wf << " ms, Stereo Spectrogram: " << cl << " ms of worker time per second of audio");
  CHECK(wf < 200.0);
  CHECK(cl < 400.0);
}

