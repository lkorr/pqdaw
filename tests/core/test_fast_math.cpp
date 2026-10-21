// Sweeps every fast-math function over its documented domain against double-precision std:: references and
// checks the documented error bounds. Also checks that the AVX2 8-wide path is bit-identical to the scalar path
// on every swept input, and the documented special-value behaviour.

#include <doctest/doctest.h>

#include <immintrin.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "pq/core/fast_math.h"
#include "pq/core/units.h"

namespace {

constexpr float kInf = std::numeric_limits<float>::infinity();
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

struct ErrStats {
  double maxAbs = 0;
  double maxRel = 0;
  float worstAbsX = 0;
  float worstRelX = 0;
  std::size_t count = 0;
  std::size_t simdMismatches = 0;
};

// Runs the scalar and AVX2 versions over xs; err(x, approx) returns {abs, rel} error for one value.
template <class Scalar, class Wide, class Err>
ErrStats sweep(const std::vector<float>& xs, Scalar scalar, Wide wide, Err err) {
  ErrStats s;
  alignas(32) float in[8];
  alignas(32) float out[8];
  for (std::size_t base = 0; base < xs.size(); base += 8) {
    const std::size_t n = std::min<std::size_t>(8, xs.size() - base);
    for (std::size_t j = 0; j < 8; ++j) in[j] = xs[base + (j < n ? j : 0)];
    _mm256_store_ps(out, wide(_mm256_load_ps(in)));
    for (std::size_t j = 0; j < n; ++j) {
      const float x = in[j];
      const float y = scalar(x);
      if (std::bit_cast<uint32_t>(y) != std::bit_cast<uint32_t>(out[j])) ++s.simdMismatches;
      const auto [a, r] = err(x, y);
      if (!(a <= s.maxAbs)) { s.maxAbs = a; s.worstAbsX = x; }
      if (!(r <= s.maxRel)) { s.maxRel = r; s.worstRelX = x; }
      ++s.count;
    }
  }
  return s;
}

struct AbsRel {
  double abs;
  double rel;
};

AbsRel absRel(double approx, double ref) {
  const double a = std::abs(approx - ref);
  return {a, ref == 0 ? a : a / std::abs(ref)};
}

std::vector<float> linspace(float lo, float hi, std::size_t n) {
  std::vector<float> v;
  v.reserve(n + 1);
  for (std::size_t i = 0; i <= n; ++i)
    v.push_back(static_cast<float>(lo + (static_cast<double>(hi) - lo) * static_cast<double>(i) / n));
  return v;
}

// Every `stride`-th float bit pattern in [lo, hi] (both positive), plus hi: covers every binade evenly.
std::vector<float> bitSweep(float lo, float hi, uint32_t stride) {
  std::vector<float> v;
  const uint32_t a = std::bit_cast<uint32_t>(lo);
  const uint32_t b = std::bit_cast<uint32_t>(hi);
  for (uint64_t u = a; u <= b; u += stride) v.push_back(std::bit_cast<float>(static_cast<uint32_t>(u)));
  v.push_back(hi);
  return v;
}

void append(std::vector<float>& v, const std::vector<float>& w) { v.insert(v.end(), w.begin(), w.end()); }

std::vector<float> withNegatives(std::vector<float> v) {
  const std::size_t n = v.size();
  for (std::size_t i = 0; i < n; ++i) v.push_back(-v[i]);
  return v;
}

void report(const char* name, const ErrStats& s) {
  MESSAGE(std::string(name) << ": n=" << s.count << " maxAbs=" << s.maxAbs << " (x=" << s.worstAbsX << ") maxRel=" << s.maxRel
               << " (x=" << s.worstRelX << ")");
}

float wide1(__m256 (*f)(__m256), float x) {  // lane 0 of the AVX2 version
  return _mm256_cvtss_f32(f(_mm256_set1_ps(x)));
}

__m256 wExp2(__m256 x) { return pq::fastExp2(x); }
__m256 wLog2(__m256 x) { return pq::fastLog2(x); }
__m256 wSin(__m256 x) { return pq::fastSin(x); }
__m256 wTan(__m256 x) { return pq::fastTan(x); }
__m256 wTanh(__m256 x) { return pq::fastTanh(x); }

}  // namespace

TEST_CASE("fast_math: fastExp2 relative error over [-126, 127]") {
  std::vector<float> xs = linspace(-126.0f, 127.0f, 3'000'000);
  append(xs, withNegatives(bitSweep(1e-30f, 1.0f, 997)));  // dense near 0 (where 2^x ≈ 1 + x·ln2)
  const auto s = sweep(xs, [](float x) { return pq::fastExp2(x); }, wExp2,
                       [](float x, float y) { return absRel(y, std::exp2(static_cast<double>(x))); });
  report("fastExp2", s);
  CHECK(s.simdMismatches == 0);
  CHECK(s.maxRel <= pq::kFastExp2MaxRelErr);
  // In cents when used as a pitch ratio.
  MESSAGE("fastExp2 max error in cents: " << 1200.0 * std::log2(1.0 + s.maxRel));

  for (int n = -126; n <= 127; ++n) CHECK(pq::fastExp2(static_cast<float>(n)) == std::ldexp(1.0f, n));
}

TEST_CASE("fast_math: fastExp2 special values") {
  CHECK(pq::fastExp2(-kInf) == 0.0f);
  CHECK(pq::fastExp2(-127.0f) == 0.0f);
  CHECK(pq::fastExp2(-1000.0f) == 0.0f);
  CHECK(pq::fastExp2(kInf) == kInf);
  CHECK(pq::fastExp2(128.0f) == kInf);
  CHECK(pq::fastExp2(1000.0f) == kInf);
  CHECK(std::isnan(pq::fastExp2(kNaN)));
  CHECK(std::isnan(wide1(wExp2, kNaN)));
  CHECK(pq::fastExp2(0.0f) == 1.0f);
  CHECK(pq::fastExp2(-0.0f) == 1.0f);
  // Just above the flush threshold the result is still accurate (it is denormal or the smallest normal).
  CHECK(pq::fastExp2(-126.0f) == std::ldexp(1.0f, -126));
}

TEST_CASE("fast_math: fastLog2 absolute error over all positive normal floats") {
  std::vector<float> xs = bitSweep(std::numeric_limits<float>::min(), std::numeric_limits<float>::max(), 401);
  const auto s = sweep(xs, [](float x) { return pq::fastLog2(x); }, wLog2,
                       [](float x, float y) { return absRel(y, std::log2(static_cast<double>(x))); });
  report("fastLog2 (all normals)", s);
  CHECK(s.simdMismatches == 0);
  CHECK(s.maxAbs <= pq::kFastLog2MaxAbsErrAll);

  const std::vector<float> mid = bitSweep(std::ldexp(1.0f, -16), std::ldexp(1.0f, 16), 53);
  const auto sm = sweep(mid, [](float x) { return pq::fastLog2(x); }, wLog2,
                        [](float x, float y) { return absRel(y, std::log2(static_cast<double>(x))); });
  report("fastLog2 [2^-16, 2^16]", sm);
  CHECK(sm.simdMismatches == 0);
  CHECK(sm.maxAbs <= pq::kFastLog2MaxAbsErr);
  MESSAGE("fastLog2 max abs error in cents on [2^-16, 2^16]: " << 1200.0 * sm.maxAbs);

  for (int n = -126; n <= 127; ++n) CHECK(pq::fastLog2(std::ldexp(1.0f, n)) == static_cast<float>(n));
}

TEST_CASE("fast_math: fastLog2 relative error near 1 ([0.5, 2], every 3rd float)") {
  std::vector<float> xs = bitSweep(0.5f, 2.0f, 3);
  const auto s = sweep(xs, [](float x) { return pq::fastLog2(x); }, wLog2,
                       [](float x, float y) { return absRel(y, std::log2(static_cast<double>(x))); });
  report("fastLog2 [0.5, 2]", s);
  CHECK(s.simdMismatches == 0);
  CHECK(s.maxRel <= pq::kFastLog2MaxRelErrNear1);
  CHECK(s.maxAbs <= pq::kFastLog2MaxAbsErr);
  CHECK(pq::fastLog2(1.0f) == 0.0f);
}

TEST_CASE("fast_math: fastLog2 special values") {
  const float ninf = -kInf;
  CHECK(pq::fastLog2(0.0f) == ninf);
  CHECK(pq::fastLog2(-0.0f) == ninf);
  CHECK(pq::fastLog2(std::numeric_limits<float>::denorm_min()) == ninf);
  CHECK(pq::fastLog2(1e-40f) == ninf);
  CHECK(std::isnan(pq::fastLog2(-1.0f)));
  CHECK(std::isnan(pq::fastLog2(-1e-40f)));
  CHECK(std::isnan(pq::fastLog2(-kInf)));
  CHECK(std::isnan(pq::fastLog2(kNaN)));
  CHECK(std::isnan(wide1(wLog2, kNaN)));
  CHECK(pq::fastLog2(kInf) == kInf);
  CHECK(wide1(wLog2, kInf) == kInf);
}

TEST_CASE("fast_math: fastSin absolute error over |x| <= kFastSinMaxArg") {
  std::vector<float> xs = linspace(-pq::kFastSinMaxArg, pq::kFastSinMaxArg, 3'000'000);
  append(xs, linspace(-7.0f, 7.0f, 1'000'000));
  append(xs, withNegatives(bitSweep(1e-30f, 7.0f, 1009)));
  const auto s = sweep(xs, [](float x) { return pq::fastSin(x); }, wSin,
                       [](float x, float y) { return absRel(y, std::sin(static_cast<double>(x))); });
  report("fastSin", s);
  CHECK(s.simdMismatches == 0);
  CHECK(s.maxAbs <= pq::kFastSinMaxAbsErr);
}

TEST_CASE("fast_math: fastSin special values and small-argument exactness") {
  CHECK(pq::fastSin(0.0f) == 0.0f);
  CHECK(pq::fastSin(-0.0f) == 0.0f);
  CHECK(pq::fastSin(1e-20f) == 1e-20f);
  CHECK(pq::fastSin(-1e-20f) == -1e-20f);
  CHECK(std::isnan(pq::fastSin(kInf)));
  CHECK(std::isnan(pq::fastSin(-kInf)));
  CHECK(std::isnan(pq::fastSin(kNaN)));
  CHECK(std::isnan(wide1(wSin, kInf)));
}

TEST_CASE("fast_math: fastTan relative error over |x| < pi/2, including the pole") {
  std::vector<float> xs = linspace(-pq::kFastTanMaxArg, pq::kFastTanMaxArg, 3'000'000);
  append(xs, withNegatives(bitSweep(1e-30f, 1.0f, 1013)));
  // The last 200k floats below pi/2 on both sides: tan from ~1.3e7 down to ~80.
  const uint32_t top = std::bit_cast<uint32_t>(pq::kFastTanMaxArg);
  append(xs, withNegatives(bitSweep(std::bit_cast<float>(top - 200'000), pq::kFastTanMaxArg, 1)));
  const auto s = sweep(xs, [](float x) { return pq::fastTan(x); }, wTan,
                       [](float x, float y) { return absRel(y, std::tan(static_cast<double>(x))); });
  report("fastTan", s);
  CHECK(s.simdMismatches == 0);
  CHECK(s.maxRel <= pq::kFastTanMaxRelErr);
}

TEST_CASE("fast_math: fastTan prewarp use and out-of-domain clamping") {
  // g = tan(pi * fc / fs) for fc from 1 Hz to just below Nyquist at 48 kHz.
  const double fs = 48000.0;
  double worst = 0;
  for (double fc = 1.0; fc < fs / 2; fc *= 1.0001) {
    const float x = static_cast<float>(3.14159265358979323846 * fc / fs);
    worst = std::max(worst, absRel(pq::fastTan(x), std::tan(static_cast<double>(x))).rel);
  }
  CHECK(worst <= pq::kFastTanMaxRelErr);
  const float atMax = pq::fastTan(pq::kFastTanMaxArg);
  CHECK(std::isfinite(atMax));
  CHECK(atMax > 1e7f);
  CHECK(pq::fastTan(2.0f) == atMax);
  CHECK(pq::fastTan(kInf) == atMax);
  CHECK(pq::fastTan(-kInf) == -atMax);
  CHECK(std::isnan(pq::fastTan(kNaN)));
  CHECK(std::isnan(wide1(wTan, kNaN)));
  CHECK(pq::fastTan(0.0f) == 0.0f);
}

TEST_CASE("fast_math: fastTanh absolute and relative error over the reals") {
  std::vector<float> xs = linspace(-20.0f, 20.0f, 3'000'000);
  append(xs, withNegatives(bitSweep(1e-30f, 20.0f, 1019)));
  const auto s = sweep(xs, [](float x) { return pq::fastTanh(x); }, wTanh,
                       [](float x, float y) { return absRel(y, std::tanh(static_cast<double>(x))); });
  report("fastTanh", s);
  CHECK(s.simdMismatches == 0);
  CHECK(s.maxAbs <= pq::kFastTanhMaxAbsErr);
  CHECK(s.maxRel <= pq::kFastTanhMaxRelErr);
}

TEST_CASE("fast_math: fastTanh special values and bounds") {
  CHECK(pq::fastTanh(kInf) == 1.0f);
  CHECK(pq::fastTanh(-kInf) == -1.0f);
  CHECK(pq::fastTanh(1e30f) == 1.0f);
  CHECK(pq::fastTanh(0.0f) == 0.0f);
  CHECK(std::signbit(pq::fastTanh(-0.0f)));
  CHECK(std::isnan(pq::fastTanh(kNaN)));
  CHECK(std::isnan(wide1(wTanh, kNaN)));
  for (float x = 0.0f; x < 30.0f; x += 0.001f) {
    REQUIRE(pq::fastTanh(x) <= 1.0f);
    REQUIRE(pq::fastTanh(-x) == -pq::fastTanh(x));
  }
}

// ---- units ------------------------------------------------------------------------------------------------------

TEST_CASE("units: precise conversions") {
  CHECK(pq::centsToHz(0.0) == pq::kDefaultReferenceHz);
  CHECK(pq::centsToHz(1200.0, 440.0) == 880.0);
  CHECK(pq::centsToHz(-2400.0, 440.0) == 110.0);
  CHECK(pq::hzToCents(880.0, 440.0) == 1200.0);
  CHECK(pq::hzToCents(pq::kDefaultReferenceHz) == 0.0);
  for (double c = -10000.0; c <= 10000.0; c += 0.37) REQUIRE(std::abs(pq::hzToCents(pq::centsToHz(c)) - c) < 1e-9);
  // 3/2 = 701.955 cents
  CHECK(pq::hzToCents(3.0, 2.0) == doctest::Approx(701.9550008653874).epsilon(1e-14));

  CHECK(pq::dbToGain(0.0) == 1.0);
  CHECK(pq::dbToGain(20.0) == doctest::Approx(10.0).epsilon(1e-15));
  CHECK(pq::dbToGain(-std::numeric_limits<double>::infinity()) == 0.0);
  CHECK(pq::gainToDb(1.0) == 0.0);
  CHECK(pq::gainToDb(10.0) == doctest::Approx(20.0).epsilon(1e-15));
  CHECK(pq::gainToDb(0.0) == -std::numeric_limits<double>::infinity());
  CHECK(std::isnan(pq::gainToDb(-1.0)));
  for (double db = -200.0; db <= 200.0; db += 0.13) REQUIRE(std::abs(pq::gainToDb(pq::dbToGain(db)) - db) < 1e-11);
}

TEST_CASE("units: fastCentsToHz error in cents over +-10 octaves") {
  const float refs[] = {261.626f, 440.0f, 1.0f, 8.1757989f};
  double worst = 0;
  std::size_t mismatches = 0;
  for (const float ref : refs) {
    const auto xs = linspace(-12000.0f, 12000.0f, 1'000'000);
    for (std::size_t i = 0; i < xs.size(); ++i) {
      const float c = xs[i];
      const float hz = pq::fastCentsToHz(c, ref);
      const float hz8 = _mm256_cvtss_f32(pq::fastCentsToHz(_mm256_set1_ps(c), _mm256_set1_ps(ref)));
      if (std::bit_cast<uint32_t>(hz) != std::bit_cast<uint32_t>(hz8)) ++mismatches;
      const double exact = pq::centsToHz(c, ref);
      worst = std::max(worst, std::abs(1200.0 * std::log2(hz / exact)));
    }
  }
  MESSAGE("fastCentsToHz max error: " << worst << " cents");
  CHECK(mismatches == 0);
  CHECK(worst <= pq::kFastCentsToHzMaxErrCents);
  CHECK(pq::fastCentsToHz(1200.0f, 440.0f) == 880.0f);
  CHECK(pq::fastCentsToHz(0.0f, 261.626f) == 261.626f);
}

TEST_CASE("units: fastHzToCents absolute error over hz/ref in [2^-20, 2^20]") {
  const float refs[] = {261.626f, 440.0f, 1.0f};
  double worst = 0;
  std::size_t mismatches = 0;
  for (const float ref : refs) {
    const auto ratios = bitSweep(std::ldexp(1.0f, -20), std::ldexp(1.0f, 20), 211);
    for (const float r : ratios) {
      const float hz = r * ref;
      const float c = pq::fastHzToCents(hz, ref);
      const float c8 = _mm256_cvtss_f32(pq::fastHzToCents(_mm256_set1_ps(hz), _mm256_set1_ps(ref)));
      if (std::bit_cast<uint32_t>(c) != std::bit_cast<uint32_t>(c8)) ++mismatches;
      worst = std::max(worst, std::abs(c - pq::hzToCents(hz, ref)));
    }
  }
  MESSAGE("fastHzToCents max error: " << worst << " cents");
  CHECK(mismatches == 0);
  CHECK(worst <= pq::kFastHzToCentsMaxErrCents);
  CHECK(std::abs(pq::fastHzToCents(880.0f, 440.0f) - 1200.0f) <= pq::kFastHzToCentsMaxErrCents);
  CHECK(pq::fastHzToCents(0.0f, 440.0f) == -kInf);
}

TEST_CASE("units: fastDbToGain and fastGainToDb") {
  double worstRel = 0;
  for (const float db : linspace(-300.0f, 300.0f, 2'000'000)) {
    const float g = pq::fastDbToGain(db);
    REQUIRE(std::bit_cast<uint32_t>(g) ==
            std::bit_cast<uint32_t>(_mm256_cvtss_f32(pq::fastDbToGain(_mm256_set1_ps(db)))));
    worstRel = std::max(worstRel, absRel(g, pq::dbToGain(db)).rel);
  }
  MESSAGE("fastDbToGain max rel error: " << worstRel);
  CHECK(worstRel <= pq::kFastDbToGainMaxRelErr);

  double worstDb = 0;
  for (const float g : bitSweep(1e-15f, 1e15f, 173)) {
    const float db = pq::fastGainToDb(g);
    REQUIRE(std::bit_cast<uint32_t>(db) ==
            std::bit_cast<uint32_t>(_mm256_cvtss_f32(pq::fastGainToDb(_mm256_set1_ps(g)))));
    worstDb = std::max(worstDb, std::abs(db - pq::gainToDb(g)));
  }
  MESSAGE("fastGainToDb max abs error: " << worstDb << " dB");
  CHECK(worstDb <= pq::kFastGainToDbMaxAbsErrDb);

  CHECK(pq::fastDbToGain(0.0f) == 1.0f);
  CHECK(pq::fastDbToGain(-kInf) == 0.0f);
  CHECK(pq::fastDbToGain(-1000.0f) == 0.0f);
  CHECK(pq::fastGainToDb(1.0f) == 0.0f);
  CHECK(pq::fastGainToDb(0.0f) == -kInf);
  CHECK(std::isnan(pq::fastGainToDb(-1.0f)));
}
