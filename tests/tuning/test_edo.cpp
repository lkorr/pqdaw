#include <doctest/doctest.h>

#include <array>
#include <cmath>

#include "pq/tuning/edo.h"

using pq::tuning::Edo;
using pq::tuning::Rational;
using pq::tuning::Val;

namespace {

Edo edoOf(uint32_t n, Rational equave = Rational(2)) {
  auto e = Edo::make(n, equave);
  REQUIRE(e.ok());
  return *e;
}

struct KnownVal {
  uint32_t n;
  std::array<int32_t, pq::tuning::kValPrimeCount> val;
};

// Patent vals over 2.3.5.7.11.13.17.19.23.29.31. The 2.3.5.7 parts are the published values (e.g. Xenharmonic wiki):
// 12 ⟨12 19 28 34], 19 ⟨19 30 44 53], 22 ⟨22 35 51 62], 31 ⟨31 49 72 87], 41 ⟨41 65 95 115], 53 ⟨53 84 123 149],
// 72 ⟨72 114 167 202]. The full vectors were computed with exact integer arithmetic (Python big ints: k is the patent
// mapping of p iff 2^(2k−1) < p^(2n) < 2^(2k+1)), independent of floating point.
constexpr KnownVal kKnownVals[] = {
    {12, {12, 19, 28, 34, 42, 44, 49, 51, 54, 58, 59}},
    {19, {19, 30, 44, 53, 66, 70, 78, 81, 86, 92, 94}},
    {22, {22, 35, 51, 62, 76, 81, 90, 93, 100, 107, 109}},
    {31, {31, 49, 72, 87, 107, 115, 127, 132, 140, 151, 154}},
    {41, {41, 65, 95, 115, 142, 152, 168, 174, 185, 199, 203}},
    {53, {53, 84, 123, 149, 183, 196, 217, 225, 240, 257, 263}},
    {72, {72, 114, 167, 202, 249, 266, 294, 306, 326, 350, 357}},
};

// First-principles patent val: nearest step to each prime, via log(p)/log(E) (a different formula path from the
// implementation's log2 product).
Val referenceVal(uint32_t n, double equave) {
  Val v;
  for (int i = 0; i < pq::tuning::kValPrimeCount; ++i) {
    const double exact = n * std::log(static_cast<double>(pq::tuning::kMonzoPrimes[i])) / std::log(equave);
    v.steps[i] = static_cast<int32_t>(std::floor(exact + 0.5));
  }
  return v;
}

}  // namespace

TEST_CASE("edo: patent vals match the published tables") {
  for (const KnownVal& k : kKnownVals) {
    CAPTURE(k.n);
    const Val v = pq::tuning::patentVal(edoOf(k.n));
    CHECK(v.steps == k.val);
    CHECK(v == referenceVal(k.n, 2.0));
  }
}

TEST_CASE("edo: patent vals from first principles, n = 1..400") {
  for (uint32_t n = 1; n <= 400; ++n) {
    CAPTURE(n);
    CHECK(pq::tuning::patentVal(edoOf(n)) == referenceVal(n, 2.0));
  }
}

TEST_CASE("edo: non-octave equaves") {
  // Bohlen–Pierce: 13 equal divisions of 3/1. The 3.5.7 val ⟨13 19 23] is the published one.
  const Val bp = pq::tuning::patentVal(edoOf(13, Rational(3)));
  CHECK(bp.steps[1] == 13);
  CHECK(bp.steps[2] == 19);
  CHECK(bp.steps[3] == 23);
  CHECK(bp.steps[0] == 8);
  CHECK(bp == referenceVal(13, 3.0));

  // Equave given in cents: 88-cET as "1 step of 88 cents".
  auto cet = Edo::makeCents(1, 88.0);
  REQUIRE(cet.ok());
  CHECK(cet->stepCents() == doctest::Approx(88.0));
  CHECK_FALSE(cet->equaveRatio().has_value());
  CHECK(pq::tuning::patentVal(*cet).steps[1] == 22);  // 1901.955 / 88 = 21.61

  // Stretched octave in cents vs the same octave as a ratio.
  auto stretched = Edo::makeCents(12, 1201.0);
  REQUIRE(stretched.ok());
  CHECK(stretched->centsOfStep(12) == doctest::Approx(1201.0));
}

TEST_CASE("edo: construction and step geometry") {
  const Edo e12 = Edo::standard12();
  CHECK(e12 == edoOf(12));
  CHECK(e12.n() == 12);
  CHECK(e12.equaveCents() == 1200.0);
  CHECK(e12.stepCents() == doctest::Approx(100.0));
  CHECK(*e12.equaveRatio() == Rational(2));
  CHECK(e12.centsOfStep(7) == doctest::Approx(700.0));
  CHECK(e12.centsOfStep(-12) == doctest::Approx(-1200.0));

  const Edo e31 = edoOf(31);
  CHECK(e31.centsOfStep(18) == doctest::Approx(1200.0 * 18 / 31));

  const Edo tritave = edoOf(13, Rational(3));
  CHECK(tritave.equaveCents() == doctest::Approx(1901.955000865387));
  CHECK(tritave.centsOfStep(13) == doctest::Approx(1901.955000865387));

  CHECK(edoOf(1).stepCents() == doctest::Approx(1200.0));  // 1-EDO is legal

  CHECK(Edo::make(0).error().code == pq::ErrorCode::InvalidArgument);
  CHECK(Edo::make(12, Rational(1)).error().code == pq::ErrorCode::InvalidArgument);
  CHECK(Edo::make(12, Rational(1, 2)).error().code == pq::ErrorCode::InvalidArgument);
  CHECK_FALSE(Edo::makeCents(12, 0.0).ok());
  CHECK_FALSE(Edo::makeCents(12, -5.0).ok());
  CHECK_FALSE(Edo::makeCents(12, std::nan("")).ok());
  CHECK_FALSE(Edo::makeCents(12, INFINITY).ok());
  CHECK_FALSE(Edo::makeCents(0, 1200.0).ok());
}

TEST_CASE("edo: mapping ratios through a val") {
  const auto m = [](Rational r) { return *pq::tuning::toMonzo(r); };
  const Val v12 = pq::tuning::patentVal(Edo::standard12());
  const Val v53 = pq::tuning::patentVal(edoOf(53));
  const Val v22 = pq::tuning::patentVal(edoOf(22));
  CHECK(*pq::tuning::mapToSteps(v12, m(Rational(3, 2))) == 7);
  CHECK(*pq::tuning::mapToSteps(v12, m(Rational(5, 4))) == 4);
  CHECK(*pq::tuning::mapToSteps(v12, m(Rational(81, 80))) == 0);  // 12-EDO tempers out the syntonic comma
  CHECK(*pq::tuning::mapToSteps(v53, m(Rational(81, 80))) == 1);
  CHECK(*pq::tuning::mapToSteps(v22, m(Rational(50, 49))) == 0);  // 22 is a pajara tuning: tempers out 50/49
  CHECK(*pq::tuning::mapToSteps(v12, m(Rational(2, 3))) == -7);
  CHECK_FALSE(pq::tuning::mapToSteps(v12, m(Rational(37, 32))).has_value());  // prime 37 is outside the val
}

TEST_CASE("edo: step labels and rows") {
  CHECK(pq::tuning::stepLabel(7, 12) == "7\\12");
  CHECK(pq::tuning::stepLabel(0, 31) == "0\\31");
  CHECK(pq::tuning::stepLabel(-3, 31) == "-3\\31");
  CHECK(pq::tuning::stepLabel(53, 53) == "53\\53");

  auto row = pq::tuning::edoRow(-1, 12);
  CHECK(row.equave == -1);
  CHECK(row.degree == 11);
  row = pq::tuning::edoRow(31, 31);
  CHECK(row.equave == 1);
  CHECK(row.degree == 0);
  row = pq::tuning::edoRow(-31, 31);
  CHECK(row.equave == -1);
  CHECK(row.degree == 0);
  row = pq::tuning::edoRow(45, 31);
  CHECK(row.equave == 1);
  CHECK(row.degree == 14);
  CHECK(pq::tuning::stepLabel(pq::tuning::edoRow(45, 31).degree, 31) == "14\\31");
}
