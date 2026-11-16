// Real FFT and the spectrum analyser (pq/dsp/fft.h, spectrum.h; T4.6): reference signals.

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <random>
#include <string>
#include <vector>

#include "pq/core/alloc_trap.h"
#include "pq/dsp/fft.h"
#include "pq/dsp/spectrum.h"

using pq::dsp::RealFft;
using pq::dsp::SpectrumAnalyzer;
using pq::dsp::SpectrumConfig;

namespace {

constexpr double kPi = 3.14159265358979323846;

std::vector<float> sine(double hz, double amp, double sr, uint32_t n, double phase = 0.3) {
  std::vector<float> x(n);
  for (uint32_t i = 0; i < n; ++i) x[i] = static_cast<float>(amp * std::sin(2.0 * kPi * hz * i / sr + phase));
  return x;
}

// The point nearest to hz.
uint32_t pointAt(const SpectrumAnalyzer& a, float hz) {
  uint32_t best = 0;
  for (uint32_t p = 1; p < a.config().points; ++p)
    if (std::fabs(std::log(a.pointHz(p) / hz)) < std::fabs(std::log(a.pointHz(best) / hz))) best = p;
  return best;
}

float maxAround(const SpectrumAnalyzer& a, float hz) {
  const uint32_t p = pointAt(a, hz);
  float m = -1000.0f;
  for (uint32_t q = p > 3 ? p - 3 : 0; q <= std::min(a.config().points - 1, p + 3); ++q) m = std::max(m, a.levelsDb()[q]);
  return m;
}

SpectrumConfig flatConfig() {
  SpectrumConfig c;
  c.tiltDbPerOct = 0.0f;
  return c;
}

}  // namespace

TEST_CASE("fft: matches a direct DFT") {
  for (uint32_t n : {8u, 64u, 2048u}) {
    std::mt19937 rng(n);
    std::uniform_real_distribution<float> d(-1.0f, 1.0f);
    std::vector<float> x(n);
    for (float& v : x) v = d(rng);
    RealFft fft(n);
    std::vector<float> re(n / 2 + 1), im(n / 2 + 1), pw(n / 2 + 1);
    fft.forward(x.data(), re.data(), im.data());
    fft.power(x.data(), pw.data());
    double maxErr = 0.0;
    for (uint32_t k = 0; k <= n / 2; ++k) {
      std::complex<double> s = 0.0;
      for (uint32_t i = 0; i < n; ++i) s += static_cast<double>(x[i]) * std::polar(1.0, -2.0 * kPi * k * i / n);
      maxErr = std::max(maxErr, std::abs(s - std::complex<double>(re[k], im[k])));
      CHECK(pw[k] == doctest::Approx(std::norm(s)).epsilon(1e-3).scale(1.0));
    }
    CHECK(maxErr < 1e-4 * n);
  }
}

TEST_CASE("spectrum: a full-scale sine reads 0 dB on a bin and between bins; -20 dB reads -20") {
  const SpectrumConfig cfg = flatConfig();
  for (double hz : {1000.0, 1000.0 + 0.5 * 48000.0 / 2048.0, 5321.7, 12000.0}) {
    SpectrumAnalyzer a(cfg);
    const auto x = sine(hz, 1.0, cfg.sampleRate, 48000);
    a.push(x.data(), static_cast<uint32_t>(x.size()));
    INFO("hz = " << hz);
    CHECK(std::fabs(maxAround(a, static_cast<float>(hz))) < 0.4f);
    SpectrumAnalyzer b(cfg);
    const auto y = sine(hz, 0.1, cfg.sampleRate, 48000);
    b.push(y.data(), static_cast<uint32_t>(y.size()));
    CHECK(std::fabs(maxAround(b, static_cast<float>(hz)) + 20.0f) < 0.4f);
  }
}

TEST_CASE("spectrum: low tones use the 4x window (sharper), and read 0 dB") {
  const SpectrumConfig cfg = flatConfig();
  SpectrumAnalyzer sharp(cfg);
  SpectrumConfig plainCfg = cfg;
  plainCfg.sharpLows = false;
  SpectrumAnalyzer plain(plainCfg);
  const auto x = sine(80.0, 1.0, cfg.sampleRate, 48000);
  sharp.push(x.data(), static_cast<uint32_t>(x.size()));
  plain.push(x.data(), static_cast<uint32_t>(x.size()));
  CHECK(std::fabs(maxAround(sharp, 80.0f)) < 0.5f);
  // Half an octave away the long window has fallen much further.
  const float awaySharp = sharp.levelsDb()[pointAt(sharp, 56.0f)];
  const float awayPlain = plain.levelsDb()[pointAt(plain, 56.0f)];
  CHECK(awaySharp < awayPlain - 20.0f);
}

TEST_CASE("spectrum: white noise reads one flat level across the crossover (no step at 500 Hz)") {
  const SpectrumConfig cfg = flatConfig();
  SpectrumAnalyzer a(cfg);
  std::mt19937 rng(7);
  std::uniform_real_distribution<float> d(-1.0f, 1.0f);
  std::vector<float> x(96000);
  for (float& v : x) v = d(rng);
  a.push(x.data(), static_cast<uint32_t>(x.size()));
  // The raw reading of the last hop, averaged in power (the ballistics' fast attack / slow release on dB sits near the
  // top of a noise reading's fluctuation, by design, so the displayed level is a few dB above the mean).
  auto meanDb = [&](float lo, float hi) {
    double s = 0.0;
    int n = 0;
    for (uint32_t p = 0; p < cfg.points; ++p)
      if (a.pointHz(p) >= lo && a.pointHz(p) <= hi) {
        s += std::pow(10.0, a.lastReadingDb()[p] / 10.0);
        ++n;
      }
    return 10.0 * std::log10(s / std::max(n, 1));
  };
  // Expected per-bin noise level with the 2/Σw amplitude scale: 6σ²/N for Hann (σ² = 1/3).
  const double expected = 10.0 * std::log10(6.0 / 3.0 / 2048.0);
  const double low = meanDb(150.0f, 300.0f), mid = meanDb(400.0f, 650.0f), high = meanDb(1000.0f, 4000.0f);
  INFO("low " << low << " mid " << mid << " high " << high << " expected " << expected);
  CHECK(std::fabs(high - expected) < 1.5);
  CHECK(std::fabs(low - high) < 1.5);
  CHECK(std::fabs(mid - high) < 1.5);
}

TEST_CASE("spectrum: tilt adds dB per octave about 1 kHz") {
  SpectrumConfig cfg;
  cfg.tiltDbPerOct = 4.5f;
  SpectrumAnalyzer a(cfg);
  const auto x = sine(4000.0, 1.0, cfg.sampleRate, 48000);
  a.push(x.data(), static_cast<uint32_t>(x.size()));
  CHECK(std::fabs(maxAround(a, 4000.0f) - 9.0f) < 0.5f);
}

TEST_CASE("spectrum: ballistics (attack 25 ms, release 350 ms on dB) and peak hold (1.5 s, then 12 dB/s)") {
  SpectrumConfig cfg = flatConfig();
  cfg.peakHold = true;
  SpectrumAnalyzer a(cfg);
  const auto x = sine(2000.0, 1.0, cfg.sampleRate, 24000);
  a.push(x.data(), static_cast<uint32_t>(x.size()));
  const uint32_t p = pointAt(a, 2000.0f);
  CHECK(std::fabs(a.levelsDb()[p]) < 0.5f);
  // Silence: once the window holds no more of the sine (2048 samples), the level falls toward the floor with τ = 350 ms.
  std::vector<float> z(2048 + 512, 0.0f);
  a.push(z.data(), static_cast<uint32_t>(z.size()));
  const float atStart = a.levelsDb()[p];
  std::vector<float> s(static_cast<size_t>(0.35 * cfg.sampleRate), 0.0f);
  a.push(s.data(), static_cast<uint32_t>(s.size()));
  const float expected = -140.0f + (atStart + 140.0f) * std::exp(-1.0f);
  CHECK(std::fabs(a.levelsDb()[p] - expected) < 3.0f);
  // Peak: held for 1.5 s, then 12 dB/s.
  CHECK(a.peaksDb()[p] > -0.6f);
  std::vector<float> more(static_cast<size_t>(1.5 * cfg.sampleRate), 0.0f);
  a.push(more.data(), static_cast<uint32_t>(more.size()));
  const float held = a.peaksDb()[p];
  CHECK(held < -0.0f);
  CHECK(held > -12.0f);
  std::vector<float> sec(static_cast<size_t>(cfg.sampleRate), 0.0f);
  a.push(sec.data(), static_cast<uint32_t>(sec.size()));
  CHECK(std::fabs((held - a.peaksDb()[p]) - 12.0f) < 1.0f);
}

TEST_CASE("spectrum: the hybrid axis (fMin = 0) is linear to 40 Hz and continuous") {
  SpectrumConfig cfg;
  cfg.fMin = 0.0f;
  cfg.fMax = 20000.0f;
  SpectrumAnalyzer a(cfg);
  CHECK(a.axisHz(0.0f) == 0.0f);
  CHECK(a.axisHz(1.0f) == doctest::Approx(20000.0f).epsilon(1e-4));
  const float t40 = a.axisT(40.0f);
  CHECK(a.axisHz(t40) == doctest::Approx(40.0f).epsilon(1e-4));
  CHECK(a.axisHz(0.5f * t40) == doctest::Approx(20.0f).epsilon(1e-4));
  CHECK(a.axisT(a.axisHz(0.7f)) == doctest::Approx(0.7f).epsilon(1e-4));
}

TEST_CASE("spectrum: push never allocates (analysis thread, RtScope)") {
  SpectrumAnalyzer a(flatConfig());
  const auto x = sine(440.0, 0.5, 48000.0, 20000);
  CHECK(pq::alloc_trap::countRtViolations([&] { a.push(x.data(), static_cast<uint32_t>(x.size())); }) == 0);
  CHECK(a.hops() > 0);
}

TEST_CASE("spectrum: analysis cost per hop (1024 points, sharp lows, music-like input)" * doctest::test_suite("perf")) {
  SpectrumConfig cfg;  // the hub's: FFT 2048, hop 512, the 4x low window, 1024 points
  SpectrumAnalyzer a(cfg);
  std::mt19937 rng(7);
  std::normal_distribution<float> g(0.0f, 0.05f);
  std::vector<float> x(static_cast<size_t>(cfg.hop) * 400);
  for (size_t i = 0; i < x.size(); ++i) {
    const double t = static_cast<double>(i) / cfg.sampleRate;
    x[i] = g(rng) + static_cast<float>(0.3 * std::sin(2.0 * kPi * 55.0 * t) + 0.2 * std::sin(2.0 * kPi * 440.0 * t) +
                                       0.1 * std::sin(2.0 * kPi * 3150.0 * t));
  }
  a.push(x.data(), cfg.hop * 40);  // warm
  const auto t0 = std::chrono::steady_clock::now();
  const uint32_t hops = a.push(x.data() + cfg.hop * 40, static_cast<uint32_t>(x.size()) - cfg.hop * 40);
  const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / hops;
  MESSAGE("spectrum analysis: " << us << " us per hop (" << hops << " hops)");
  CHECK(hops > 300);
}

// ---- VZ.2: the shared frequency axis, the dense read, the average ----------------------------------------------------

TEST_CASE("freq axis: every scale round-trips; the hybrid log axis is continuous in fMin (8f6b6ef)") {
  using pq::dsp::FreqAxis;
  using pq::dsp::FreqScale;
  for (FreqScale sc : {FreqScale::Log, FreqScale::Mel, FreqScale::Linear})
    for (float fMin : {0.0f, 1.0f, 5.0f, 10.0f, 19.9f, 20.0f, 30.0f, 200.0f}) {
      const FreqAxis a(sc, fMin, 20000.0f);
      CAPTURE(static_cast<int>(sc));
      CAPTURE(fMin);
      CHECK(a.unit(fMin) == doctest::Approx(0.0f).scale(1.0f).epsilon(1e-4));
      CHECK(a.unit(20000.0f) == doctest::Approx(1.0f).epsilon(1e-4));
      for (float u : {0.1f, 0.37f, 0.5f, 0.9f}) CHECK(a.unit(a.hz(u)) == doctest::Approx(u).epsilon(1e-3));
    }
  // Under 20 Hz the hybrid applies with its corner at 40 − fMin; at 20 it is the plain log axis: no jump at 0 → 1 Hz
  // (the old axis switched to pure log at 1 Hz and moved 1 kHz a quarter of the panel) nor at 20 Hz.
  auto at1k = [](float fMin) { return FreqAxis(FreqScale::Log, fMin, 20000.0f).unit(1000.0f); };
  CHECK(FreqAxis(FreqScale::Log, 0.0f, 20000.0f).hybrid());
  CHECK(FreqAxis(FreqScale::Log, 19.0f, 20000.0f).hybrid());
  CHECK(!FreqAxis(FreqScale::Log, 20.0f, 20000.0f).hybrid());
  CHECK(std::fabs(at1k(1.0f) - at1k(0.0f)) < 0.01f);
  CHECK(std::fabs(at1k(19.99f) - at1k(20.0f)) < 0.001f);
  for (float f = 0.0f; f < 30.0f; f += 0.5f) CHECK(std::fabs(at1k(f + 0.5f) - at1k(f)) < 0.01f);
  // Slope-continuous at the corner (no kink between the linear foot and the log part).
  const FreqAxis h(FreqScale::Log, 0.0f, 20000.0f);
  const float fc = h.hybridShape().fc, d = 0.01f;
  const float below = (h.unit(fc) - h.unit(fc - d)) / d, above = (h.unit(fc + d) - h.unit(fc)) / d;
  CHECK(below == doctest::Approx(above).epsilon(0.01));
}

TEST_CASE("freq axis: the Linear grid is even (2 kHz on 20 kHz); Log / Mel take the decade ladder first") {
  using pq::dsp::FreqGridLine;
  using pq::dsp::FreqScale;
  FreqGridLine g[24];
  const int n = pq::dsp::freqGridLines(FreqScale::Linear, 0.0f, 20000.0f, g, 24);
  REQUIRE(n == 9);  // 2k … 18k (0 is the edge, 20k past 98 %)
  std::vector<float> hz;
  for (int i = 0; i < n; ++i) hz.push_back(g[i].hz);
  std::sort(hz.begin(), hz.end());
  for (int i = 0; i < n; ++i) CHECK(hz[static_cast<size_t>(i)] == doctest::Approx(2000.0f * static_cast<float>(i + 1)));
  const int m = pq::dsp::freqGridLines(FreqScale::Log, 10.0f, 22000.0f, g, 24);
  REQUIRE(m >= 4);
  CHECK(g[0].hz == 20.0f);
  CHECK(g[1].hz == 100.0f);
  CHECK(std::string(g[2].label) == "1k");
}

TEST_CASE("spectrum: Mel and Linear axes space the points evenly on themselves and read tones at level") {
  for (pq::dsp::FreqScale sc : {pq::dsp::FreqScale::Mel, pq::dsp::FreqScale::Linear}) {
    SpectrumConfig cfg = flatConfig();
    cfg.scale = sc;
    cfg.fMin = 0.0f;
    cfg.fMax = 20000.0f;
    SpectrumAnalyzer a(cfg);
    CHECK(a.axisT(a.pointHz(300)) == doctest::Approx(300.0f / 1023.0f).epsilon(1e-3));
    for (double hz : {440.0, 3150.0, 12000.0}) {
      SpectrumAnalyzer b(cfg);
      const auto x = sine(hz, 0.5, cfg.sampleRate, 48000);
      b.push(x.data(), static_cast<uint32_t>(x.size()));
      INFO("scale " << static_cast<int>(sc) << " hz " << hz);
      CHECK(std::fabs(maxAround(b, static_cast<float>(hz)) + 6.02f) < 0.5f);
    }
  }
}

TEST_CASE("spectrum: the dense read (points denser than bins) is a smooth single lobe through the bin readings") {
  SpectrumConfig cfg = flatConfig();
  SpectrumAnalyzer a(cfg);
  const auto x = sine(97.3, 1.0, cfg.sampleRate, 48000);  // between the long window's bins
  a.push(x.data(), static_cast<uint32_t>(x.size()));
  const auto r = a.lastReadingDb();
  // Around the tone the 8192 window's bins (5.86 Hz) span several points: a monotone cubic through their readings has
  // one maximum, at level, and no flat runs or elbows (no point is a local extremum except the peak).
  uint32_t p0 = pointAt(a, 80.0f), p1 = pointAt(a, 115.0f);
  REQUIRE(p1 > p0 + 20);
  int maxima = 0;
  float peak = -1000.0f;
  for (uint32_t p = p0 + 1; p < p1; ++p) {
    if (r[p] > r[p - 1] && r[p] >= r[p + 1]) ++maxima;
    peak = std::max(peak, r[p]);
  }
  CHECK(maxima == 1);
  CHECK(std::fabs(peak) < 0.6f);
  // C1: the slope changes gradually from point to point on the lobe's flanks (a linear join kinks at every bin).
  float worstKink = 0.0f;
  for (uint32_t p = p0 + 1; p < p1; ++p) {
    if (r[p] < -40.0f || r[p - 1] < -40.0f || r[p + 1] < -40.0f) continue;
    worstKink = std::max(worstKink, std::fabs((r[p + 1] - r[p]) - (r[p] - r[p - 1])));
  }
  CHECK(worstKink < 3.0f);
}

TEST_CASE("spectrum: the average line is an unbiased power mean that skips silence and restarts on reset") {
  SpectrumConfig cfg = flatConfig();
  cfg.average = true;
  cfg.averageSeconds = 2.0f;
  SpectrumAnalyzer a(cfg);
  std::vector<float> avg(cfg.points);
  a.averageDb(avg.data());
  CHECK(avg[pointAt(a, 1000.0f)] == cfg.floorDb);  // nothing counted yet
  // Unbiased from the first hops: a steady tone averages at its level at once (the weight corrects the EMA's start).
  const auto loud = sine(1000.0, 1.0, cfg.sampleRate, 4800);
  a.push(loud.data(), static_cast<uint32_t>(loud.size()));
  a.averageDb(avg.data());
  const uint32_t p = pointAt(a, 1000.0f);
  CHECK(std::fabs(avg[p] - a.lastReadingDb()[p]) < 0.5f);
  // Silence does not sink it (once the window has left the tone: the hops that still hold part of it count).
  std::vector<float> quiet(48000, 0.0f);
  a.push(quiet.data(), cfg.fftSize + cfg.hop);
  a.averageDb(avg.data());
  const float before = avg[p];
  a.push(quiet.data(), static_cast<uint32_t>(quiet.size()));
  a.averageDb(avg.data());
  CHECK(avg[p] == doctest::Approx(before).epsilon(1e-3));
  // A power mean: equal time at 0 dB and -20 dB reads 10 log10((1 + 0.01) / 2) = -2.97 dB, not the dB mean (-10). The
  // segments are long against the window (a frame straddling a step mixes amplitudes, not powers: with 2400-sample
  // segments almost every frame does and the mean reads -3.44); the time constant long against the run.
  SpectrumConfig c2 = cfg;
  c2.averageSeconds = 600.0f;
  SpectrumAnalyzer b(c2);
  const auto quietTone = sine(1000.0, 0.1, cfg.sampleRate, 48000);
  const auto loudTone = sine(1000.0, 1.0, cfg.sampleRate, 48000);
  for (int i = 0; i < 10; ++i) {
    b.push(loudTone.data(), static_cast<uint32_t>(loudTone.size()));
    b.push(quietTone.data(), static_cast<uint32_t>(quietTone.size()));
  }
  b.averageDb(avg.data());
  // Against the analyser's own steady readings of the two levels (a full-scale tone need not read exactly 0 dB).
  const uint32_t p1k = pointAt(b, 1000.0f);
  auto steady = [&](const std::vector<float>& x) {
    SpectrumAnalyzer s(cfg);
    s.push(x.data(), static_cast<uint32_t>(x.size()));
    return static_cast<double>(s.lastReadingDb()[p1k]);
  };
  const double lo = steady(quietTone), hi = steady(loudTone);
  const double powerMean = 10.0 * std::log10(0.5 * (std::pow(10.0, hi / 10.0) + std::pow(10.0, lo / 10.0)));
  MESSAGE("power mean: " << avg[p1k] << " (expected " << powerMean << "; readings " << hi << ", " << lo << ")");
  CHECK(std::fabs(avg[p1k] - powerMean) < 0.1);
  CHECK(std::fabs(powerMean - (hi - 3.0103 + 10.0 * std::log10(1.01))) < 0.01);
  // reset() restarts it.
  b.reset();
  b.averageDb(avg.data());
  CHECK(avg[pointAt(b, 1000.0f)] == c2.floorDb);
  // setLive keeps the analysis running (no restart of the levels) and an average switched on starts empty.
  SpectrumConfig c3 = flatConfig();
  SpectrumAnalyzer c(c3);
  c.push(loud.data(), static_cast<uint32_t>(loud.size()));
  const float lvl = c.levelsDb()[p];
  pq::dsp::SpectrumLive live;
  live.average = true;
  live.releaseMs = 1000.0f;
  c.setLive(live);
  CHECK(c.levelsDb()[p] == lvl);
  c.averageDb(avg.data());
  CHECK(avg[p] == c3.floorDb);
  c.push(loud.data(), static_cast<uint32_t>(loud.size()));
  c.averageDb(avg.data());
  CHECK(avg[p] > -3.0f);
}
