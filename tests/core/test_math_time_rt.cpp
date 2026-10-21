// RtScope allocation-trap tests for the RT entry points of fast_math.h, units.h and time.h.

#include <doctest/doctest.h>

#include <immintrin.h>

#include <cstdint>

#include "pq/core/alloc_trap.h"
#include "pq/core/fast_math.h"
#include "pq/core/time.h"
#include "pq/core/units.h"

using pq::alloc_trap::countRtViolations;

TEST_CASE("rt: fast math and unit conversions (scalar + AVX2) do not allocate") {
  REQUIRE(pq::alloc_trap::installed());
  float acc = 0.0f;
  __m256 acc8 = _mm256_setzero_ps();
  CHECK(countRtViolations([&] {
          for (float x = -10.0f; x < 10.0f; x += 0.01f) {
            acc += pq::fastExp2(x) + pq::fastLog2(x + 11.0f) + pq::fastSin(x) + pq::fastTan(x * 0.15f) +
                   pq::fastTanh(x);
            acc += pq::fastCentsToHz(x * 100.0f, 261.626f) + pq::fastHzToCents(x + 11.0f, 440.0f) +
                   pq::fastDbToGain(x) + pq::fastGainToDb(x + 11.0f);
            const __m256 v = _mm256_set1_ps(x);
            const __m256 p = _mm256_set1_ps(x + 11.0f);
            acc8 = _mm256_add_ps(acc8, pq::fastExp2(v));
            acc8 = _mm256_add_ps(acc8, pq::fastLog2(p));
            acc8 = _mm256_add_ps(acc8, pq::fastSin(v));
            acc8 = _mm256_add_ps(acc8, pq::fastTan(_mm256_mul_ps(v, _mm256_set1_ps(0.15f))));
            acc8 = _mm256_add_ps(acc8, pq::fastTanh(v));
            acc8 = _mm256_add_ps(acc8, pq::fastCentsToHz(v, _mm256_set1_ps(261.626f)));
            acc8 = _mm256_add_ps(acc8, pq::fastHzToCents(p, _mm256_set1_ps(440.0f)));
            acc8 = _mm256_add_ps(acc8, pq::fastDbToGain(v));
            acc8 = _mm256_add_ps(acc8, pq::fastGainToDb(p));
            acc += static_cast<float>(pq::centsToHz(x) + pq::hzToCents(x + 11.0) + pq::dbToGain(x) +
                                      pq::gainToDb(x + 11.0));
          }
        }) == 0);
  CHECK(acc == acc);  // keep the work observable
  CHECK(_mm256_cvtss_f32(acc8) == _mm256_cvtss_f32(acc8));
}

TEST_CASE("rt: tempo map and time signature queries do not allocate") {
  // Built outside the RtScope (create allocates by design).
  const pq::TempoPoint pts[] = {{0, 120.0, pq::TempoCurve::Linear},
                                {16 * pq::kPpq, 90.0, pq::TempoCurve::Step},
                                {32 * pq::kPpq, 140.0}};
  const auto mapR = pq::TempoMap::create(pts);
  REQUIRE(mapR.ok());
  const pq::TempoMap& map = *mapR;
  const pq::TimeSigChange changes[] = {{0, {4, 4}}, {4, {7, 8}}, {8, {5, 4}}};
  const auto sigR = pq::TimeSigMap::create(changes);
  REQUIRE(sigR.ok());
  const pq::TimeSigMap& sig = *sigR;

  int64_t acc = 0;
  double accD = 0;
  CHECK(countRtViolations([&] {
          for (pq::Ticks t = -pq::kPpq; t < 48 * pq::kPpq; t += 997) {
            accD += map.bpmAt(t) + map.ticksToSeconds(t) + map.ticksToSamplesF(t, 48000.0);
            const pq::SampleTime s = map.ticksToSamples(t, 48000.0);
            acc += s + map.firstTickAtOrAfterSample(s, 48000.0);
            accD += map.samplesToTicks(s, 48000.0) + map.secondsToTicks(static_cast<double>(t) * 1e-5);
            const pq::BarBeatTick b = sig.ticksToBarBeat(t);
            acc += sig.barBeatToTicks(b) + sig.barToTicks(b.bar) + sig.signatureAt(t).numerator +
                   sig.signatureAtBar(b.bar).denominator;
            acc += pq::tupletPosition(t, pq::tupletGroupTicks(16, 7), 7, 3) + pq::noteValueTicks(8);
          }
        }) == 0);
  CHECK(acc != 0);
  CHECK(accD == accD);
}
