// T6.3: the warp kernels (pq/dsp/warp.h): Hann windows overlap-add to exactly 1 at half overlap; ramps are anchored
// (a sample's value does not depend on where a call starts) and clamp; the best lag of a shifted sine is the shift (with
// the sub-frame fraction); the jitter is deterministic and within [−1, 1).
#include <doctest/doctest.h>

#include <cmath>
#include <numbers>
#include <vector>

#include "pq/dsp/warp.h"

TEST_CASE("warp kernels: Hann windows sum to 1 at half overlap") {
  for (uint32_t n : {48u, 1440u, 2880u}) {
    const std::vector<float> w = pq::dsp::hannWindow(n);
    for (uint32_t j = 0; j < n / 2; ++j) CHECK(w[j] + w[j + n / 2] == doctest::Approx(1.0).epsilon(1e-6));
    CHECK(w[0] == 0.0f);
  }
}

TEST_CASE("warp kernels: ramps are anchored on a sample (block independent), clamped, and gain fused") {
  std::vector<float> env(300), whole(300), parts(300);
  for (size_t i = 0; i < env.size(); ++i) env[i] = 0.5f + 0.001f * static_cast<float>(i);
  const float base = 0.37f / 96.0f, dg = 1.0f / 96.0f;
  pq::dsp::warpRampGain(base, dg, -40, env.data(), 0.0f, whole.data(), 300);
  // The same samples in odd pieces.
  uint32_t at = 0;
  for (uint32_t len : {3u, 17u, 8u, 101u, 171u}) {
    pq::dsp::warpRampGain(base, dg, -40 + static_cast<int32_t>(at), env.data() + at, 0.0f, parts.data() + at, len);
    at += len;
  }
  CHECK(whole == parts);
  for (size_t i = 0; i < 300; ++i) {
    const float r = pq::dsp::rampValue(base, dg, -40 + static_cast<int32_t>(i));
    CHECK(r >= 0.0f);
    CHECK(r <= 1.0f);
    CHECK(whole[i] == r * env[i]);
  }
  CHECK(whole[0] == 0.0f);       // clamped below
  CHECK(whole[299] == env[299]);  // clamped above
  std::vector<float> m(whole);
  pq::dsp::warpRampMul(1.0f, -1.0f / 50.0f, 0, m.data(), 300);
  CHECK(m[0] == whole[0]);
  CHECK(m[100] == 0.0f);
}

TEST_CASE("warp kernels: the best lag of a shifted sine is its shift, with the fraction") {
  const uint32_t len = 512, lags = 50;  // one period of lags past the shift: a single peak
  std::vector<float> ref(len), cand(len + lags - 1);
  const double period = 37.3, shift = 23.4;  // cand[l + i] = ref[i] at l = 23.4 (and every period beyond)
  for (uint32_t i = 0; i < len; ++i) ref[i] = static_cast<float>(std::sin(2.0 * std::numbers::pi * i / period));
  for (uint32_t i = 0; i < cand.size(); ++i)
    cand[i] = static_cast<float>(std::sin(2.0 * std::numbers::pi * (static_cast<double>(i) - shift) / period));
  double frac = 0.0;
  const uint32_t l = pq::dsp::warpBestLag(ref.data(), cand.data(), len, lags, 24, &frac);
  CHECK(l == 23);
  CHECK(static_cast<double>(l) + frac == doctest::Approx(shift).epsilon(2e-3));
  // Ties (silence) take the preferred lag.
  std::vector<float> zero(cand.size(), 0.0f);
  CHECK(pq::dsp::warpBestLag(ref.data(), zero.data(), len, lags, 30) == 30);
}

TEST_CASE("warp kernels: the jitter is deterministic per (seed, k) and in [−1, 1)") {
  double sum = 0.0;
  for (int64_t k = -500; k < 500; ++k) {
    const double j = pq::dsp::warpJitter(7, k);
    CHECK(j >= -1.0);
    CHECK(j < 1.0);
    CHECK(j == pq::dsp::warpJitter(7, k));
    sum += j;
  }
  CHECK(std::abs(sum / 1000.0) < 0.1);
  CHECK(pq::dsp::warpJitter(7, 3) != pq::dsp::warpJitter(8, 3));
}
