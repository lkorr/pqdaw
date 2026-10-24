// Table resources: FFT against a naive DFT, wavetable mipmaps are band-limited as documented, builders
// validate their input.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>
#include <random>
#include <vector>

#include "pq/dsp/tables.h"

namespace {

// Magnitude of harmonic h of a real cycle (naive DFT, double).
double harmonicMagnitude(std::span<const float> x, size_t n, size_t h) {
  std::complex<double> acc = 0;
  for (size_t i = 0; i < n; ++i) acc += static_cast<double>(x[i]) * std::polar(1.0, -2.0 * std::numbers::pi * h * i / n);
  return std::abs(acc) * 2.0 / static_cast<double>(n);
}

}  // namespace

TEST_CASE("fft matches a naive DFT and inverts") {
  constexpr size_t n = 64;
  std::mt19937 rng(1);
  std::uniform_real_distribution<double> d(-1, 1);
  std::vector<double> re(n), im(n), re0(n), im0(n);
  for (size_t i = 0; i < n; ++i) {
    re[i] = re0[i] = d(rng);
    im[i] = im0[i] = d(rng);
  }
  pq::dsp::fftInPlace(re, im, false);
  for (size_t k = 0; k < n; ++k) {
    std::complex<double> acc = 0;
    for (size_t i = 0; i < n; ++i) acc += std::complex<double>(re0[i], im0[i]) * std::polar(1.0, -2.0 * std::numbers::pi * k * i / n);
    CHECK(std::abs(acc.real() - re[k]) < 1e-12);
    CHECK(std::abs(acc.imag() - im[k]) < 1e-12);
  }
  pq::dsp::fftInPlace(re, im, true);
  for (size_t i = 0; i < n; ++i) {
    CHECK(std::abs(re[i] / n - re0[i]) < 1e-14);
    CHECK(std::abs(im[i] / n - im0[i]) < 1e-14);
  }
}

TEST_CASE("wavetable mipmaps are band-limited per level (half an octave apart) and keep the lower harmonics") {
  constexpr uint32_t N = 512;
  for (auto shape : {pq::dsp::BasicShape::Saw, pq::dsp::BasicShape::Square, pq::dsp::BasicShape::Triangle,
                     pq::dsp::BasicShape::Sine}) {
    const auto cycle = pq::dsp::Wavetable::basicCycle(shape, N);
    auto wt = pq::dsp::Wavetable::build(cycle.span());
    REQUIRE(wt.ok());
    const auto& v = wt->view();
    CHECK(v.size == N);
    CHECK(v.levels == 17);  // 2·log2(N) − 1: level j keeps harmonics up to (N/2)·2^(−j/2)
    CHECK(v.stride == N + 1);
    for (uint32_t k = 0; k < v.levels; ++k) {
      const auto lvl = wt->level(k);
      REQUIRE(lvl.size() == N + 1);
      CHECK(lvl[N] == lvl[0]);
      const size_t maxH = std::min<size_t>(static_cast<size_t>(std::floor((N / 2) * std::pow(2.0, -0.5 * k))), N / 2 - 1);
      CHECK(pq::dsp::wavetableLevelTopHarmonic(N, k) == maxH);
      if (k + 1 == v.levels) CHECK(maxH == 1);
      // Sample a few harmonics on each side of the cut.
      for (size_t h : {size_t{1}, size_t{2}, size_t{3}, maxH > 1 ? maxH - 1 : 1, maxH, maxH + 1, maxH + 2, size_t{N / 2 - 1}}) {
        if (h == 0 || h >= N / 2) continue;
        const double got = harmonicMagnitude(lvl, N, h);
        const double orig = harmonicMagnitude(cycle.span(), N, h);
        if (h <= maxH) CHECK(std::abs(got - orig) < 1e-6);
        else CHECK(got < 1e-6);
      }
    }
  }
  CHECK(!pq::dsp::Wavetable::build(std::vector<float>(100)).ok());
  CHECK(!pq::dsp::Wavetable::build(std::vector<float>(8)).ok());
  CHECK(pq::dsp::Wavetable::build(std::vector<float>(16)).ok());
}

TEST_CASE("sample data keeps zero guards around the samples") {
  const std::vector<float> s = {1, 2, 3};
  auto d = pq::dsp::SampleData::build(s);
  REQUIRE(d.ok());
  const auto& v = d->view();
  CHECK(v.length == 3);
  for (int32_t i = -pq::dsp::kSampleGuard; i < 0; ++i) CHECK(v.data[i] == 0.0f);
  for (int32_t i = 0; i < 3; ++i) CHECK(v.data[i] == s[static_cast<size_t>(i)]);
  for (int32_t i = 3; i < 3 + pq::dsp::kSampleGuard; ++i) CHECK(v.data[i] == 0.0f);
}
