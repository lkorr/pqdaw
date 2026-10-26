// Real FFT and the spectrum analyser (pq/dsp/fft.h, spectrum.h; T4.6): reference signals.

#include <doctest/doctest.h>

#include <cmath>
#include <complex>
#include <random>
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
