// The golden-audio comparison helper itself (T1.10): what passes, what fails, and that each check does its part.
#include <doctest/doctest.h>

#include <cmath>

#include "golden_audio.h"

namespace {

constexpr double kPi = 3.14159265358979323846;

golden::Channels sine(double hz, double amp, double phase = 0.0, size_t frames = 24000, uint32_t sr = 48000) {
  golden::Channels c(1, std::vector<float>(frames));
  for (size_t i = 0; i < frames; ++i)
    c[0][i] = static_cast<float>(amp * std::sin(2.0 * kPi * hz * static_cast<double>(i) / sr + phase));
  return c;
}

}  // namespace

TEST_CASE("golden compare: identical and nearly identical renders pass") {
  const auto a = sine(440, 0.5);
  auto cmp = golden::compare(a, 48000, a, 48000);
  CHECK(cmp.ok);
  CHECK(cmp.maxAbs == 0.0);
  CHECK(cmp.maxSpectralDb == 0.0);
  auto b = a;
  for (size_t i = 0; i < b[0].size(); ++i) b[0][i] += static_cast<float>(1e-6 * std::sin(0.37 * static_cast<double>(i)));
  cmp = golden::compare(a, 48000, b, 48000);
  INFO(cmp.message);
  CHECK(cmp.ok);
  CHECK(cmp.maxAbs < 2e-6);
  CHECK(cmp.maxSpectralDb < 0.01);
}

TEST_CASE("golden compare: format mismatches fail before any measurement") {
  const auto a = sine(440, 0.5);
  auto shorter = a;
  shorter[0].pop_back();
  CHECK_FALSE(golden::compare(a, 48000, shorter, 48000).ok);
  CHECK_FALSE(golden::compare(a, 48000, a, 44100).ok);
  golden::Channels stereo{a[0], a[0]};
  CHECK_FALSE(golden::compare(a, 48000, stereo, 48000).ok);
  auto nan = a;
  nan[0][100] = std::nanf("");
  const auto cmp = golden::compare(a, 48000, nan, 48000);
  CHECK_FALSE(cmp.ok);
  CHECK_FALSE(cmp.formatOk);
}

TEST_CASE("golden compare: the absolute check catches waveform changes the spectrum does not show") {
  // Same magnitude spectrum, different phase: spectral passes, absolute fails.
  const auto a = sine(440, 0.5);
  const auto b = sine(440, 0.5, kPi / 2);
  const auto cmp = golden::compare(a, 48000, b, 48000);
  CHECK_FALSE(cmp.ok);
  CHECK(cmp.maxAbs > 0.5);
  CHECK(cmp.maxSpectralDb < 0.1);  // measured 0.06 dB (window-edge leakage differs with phase); tolerance 0.2
  golden::Tolerance looseAbs;
  looseAbs.absTol = 1e30;
  CHECK(golden::compare(a, 48000, b, 48000, looseAbs).ok);
}

TEST_CASE("golden compare: the spectral check catches pitch, level and new content") {
  golden::Tolerance specOnly;
  specOnly.absTol = 1e30;
  const auto a = sine(440, 0.5);
  // 25 cents sharp.
  auto cmp = golden::compare(a, 48000, sine(440.0 * std::exp2(25.0 / 1200.0), 0.5), 48000, specOnly);
  INFO(cmp.message);
  CHECK_FALSE(cmp.ok);
  // −0.3 dB.
  CHECK_FALSE(golden::compare(a, 48000, sine(440, 0.5 * std::pow(10.0, -0.3 / 20.0)), 48000, specOnly).ok);
  // A −60 dB tone added in an empty band (absolute error 1e-3, also caught by the absolute check).
  auto c = a;
  const auto hiss = sine(9000, 1e-3);
  for (size_t i = 0; i < c[0].size(); ++i) c[0][i] += hiss[0][i];
  CHECK_FALSE(golden::compare(a, 48000, c, 48000, specOnly).ok);
  CHECK_FALSE(golden::compare(a, 48000, c, 48000).ok);
  // Content below the floor (−100 dB) is ignored by the spectral check.
  auto quiet = a;
  const auto tiny = sine(9000, 1e-5);
  for (size_t i = 0; i < quiet[0].size(); ++i) quiet[0][i] += tiny[0][i];
  CHECK(golden::compare(a, 48000, quiet, 48000, specOnly).ok);
}

TEST_CASE("golden compare: band levels are calibrated to a full-scale sine") {
  golden::Tolerance tol;
  std::vector<double> centres;
  const auto levels = golden::bandLevels(sine(1000, 1.0)[0], 48000, tol, &centres);
  REQUIRE(levels.size() >= 3);
  // A middle frame: the band holding 1 kHz (plus its Hann leakage into neighbours) is near 0 dB.
  const auto& mid = levels[levels.size() / 2];
  double peak = -1e9;
  size_t at = 0;
  for (size_t b = 0; b < mid.size(); ++b)
    if (mid[b] > peak) {
      peak = mid[b];
      at = b;
    }
  CHECK(peak < 0.1);
  CHECK(peak > -7.0);  // the main lobe may straddle two semitone bands
  CHECK(std::abs(centres[at] - 1000.0) < 60.0);
  CHECK(mid.front() == doctest::Approx(tol.floorDb));
}
