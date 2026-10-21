#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

#include "pq/tuning/monzo.h"
#include "pq/tuning/rational.h"

using pq::tuning::Rational;

namespace {
constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
constexpr int64_t kMin = std::numeric_limits<int64_t>::min();
}  // namespace

TEST_CASE("rational: canonical form") {
  CHECK(Rational(6, 4) == Rational(3, 2));
  CHECK(Rational(6, 4).num() == 3);
  CHECK(Rational(6, 4).den() == 2);
  CHECK(Rational(3, -2).num() == -3);
  CHECK(Rational(3, -2).den() == 2);
  CHECK(Rational(-3, -2) == Rational(3, 2));
  CHECK(Rational(0, -7) == Rational{});
  CHECK(Rational(0, -7).den() == 1);
  CHECK(Rational(5) == Rational(10, 2));
  CHECK(Rational(kMax, kMax) == Rational(1));

  static_assert(Rational(6, 4).num() == 3, "reduction is constexpr");
}

TEST_CASE("rational: checked construction") {
  CHECK_FALSE(Rational::tryMake(1, 0).has_value());
  CHECK_FALSE(Rational::tryMake(kMin, 1).has_value());
  CHECK_FALSE(Rational::tryMake(1, kMin).has_value());
  REQUIRE(Rational::tryMake(kMax, 1).has_value());
  REQUIRE(Rational::tryMake(-kMax, 3).has_value());
  CHECK(Rational::tryMake(-kMax, 3)->num() == -kMax);  // 2^63−1 = 7·7·73·127·337·92737·649657: not divisible by 3
  CHECK(Rational::tryMake(-12, -8) == Rational(3, 2));
}

TEST_CASE("rational: reciprocal and negation") {
  CHECK(*Rational(3, 2).reciprocal() == Rational(2, 3));
  CHECK(*Rational(-3, 2).reciprocal() == Rational(-2, 3));
  CHECK((*Rational(-3, 2).reciprocal()).den() == 3);
  CHECK_FALSE(Rational{}.reciprocal().has_value());
  CHECK(-Rational(3, 2) == Rational(-3, 2));
  CHECK(-Rational(kMax) == Rational(-kMax));
}

TEST_CASE("rational: multiply and divide") {
  using pq::tuning::div;
  using pq::tuning::mul;
  CHECK(*mul(Rational(3, 2), Rational(4, 3)) == Rational(2));
  CHECK(*mul(Rational(5, 4), Rational(6, 5)) == Rational(3, 2));
  CHECK(*mul(Rational(-3, 2), Rational(2, 3)) == Rational(-1));
  CHECK(*mul(Rational{}, Rational(kMax)) == Rational{});
  CHECK(*div(Rational(3, 2), Rational(5, 4)) == Rational(6, 5));
  CHECK_FALSE(div(Rational(3, 2), Rational{}).has_value());

  // Cross-cancellation: the naive products (2^62·3 and 3·2^62) overflow, the result 1 does not.
  const int64_t p62 = int64_t{1} << 62;
  CHECK(*mul(Rational(p62, 3), Rational(3, p62)) == Rational(1));
  CHECK(*mul(Rational(p62, 3), Rational(9, p62 / 2)) == Rational(6));

  // Genuine overflow is reported, never wrapped.
  CHECK_FALSE(mul(Rational(p62), Rational(2)).has_value());        // 2^63 does not fit
  CHECK_FALSE(mul(Rational(-p62), Rational(2)).has_value());       // −2^63 is INT64_MIN: excluded by the invariant
  CHECK_FALSE(mul(Rational(1, p62), Rational(1, 3)).has_value());  // denominator overflow
  CHECK(*mul(Rational(p62 - 1), Rational(1)) == Rational(p62 - 1));
  CHECK(*mul(Rational(kMax), Rational(-1)) == Rational(-kMax));
}

TEST_CASE("rational: add and subtract") {
  using pq::tuning::add;
  using pq::tuning::sub;
  CHECK(*add(Rational(1, 2), Rational(1, 3)) == Rational(5, 6));
  CHECK(*add(Rational(1, 6), Rational(1, 3)) == Rational(1, 2));
  CHECK(*add(Rational(3, 4), Rational(1, 4)) == Rational(1));
  CHECK(*sub(Rational(1, 2), Rational(1, 2)) == Rational{});
  CHECK(*sub(Rational(1, 3), Rational(1, 2)) == Rational(-1, 6));
  CHECK(*add(Rational(kMax - 1), Rational(1)) == Rational(kMax));
  CHECK_FALSE(add(Rational(kMax), Rational(1)).has_value());
  CHECK_FALSE(sub(Rational(-kMax), Rational(1)).has_value());  // would be INT64_MIN
  CHECK_FALSE(add(Rational(1, kMax), Rational(1, kMax - 1)).has_value());  // coprime denominators: product overflows
  // Knuth reduction: large common denominators do not overflow.
  CHECK(*add(Rational(1, kMax), Rational(1, kMax)) == Rational(2, kMax));
}

TEST_CASE("rational: power") {
  using pq::tuning::pow;
  CHECK(*pow(Rational(3, 2), 0) == Rational(1));
  CHECK(*pow(Rational(3, 2), 4) == Rational(81, 16));
  CHECK(*pow(Rational(3, 2), -3) == Rational(8, 27));
  CHECK(*pow(Rational(-2), 3) == Rational(-8));
  CHECK(*pow(Rational(2), 62) == Rational(int64_t{1} << 62));
  CHECK_FALSE(pow(Rational(2), 63).has_value());
  CHECK(*pow(Rational(3), 39) == Rational(4052555153018976267LL));
  CHECK_FALSE(pow(Rational(3), 40).has_value());
  CHECK_FALSE(pow(Rational{}, -1).has_value());
  CHECK(*pow(Rational(1), std::numeric_limits<int32_t>::min()) == Rational(1));  // bounded loop, no overflow on negate
}

TEST_CASE("rational: ordering") {
  CHECK(Rational(1, 3) < Rational(1, 2));
  CHECK(Rational(-1, 2) < Rational(1, 3));
  CHECK(Rational(3, 2) > Rational(4, 3));
  CHECK((Rational(2, 4) <=> Rational(1, 2)) == std::strong_ordering::equal);
  // Cross products here exceed int64: exercises the 128-bit comparison.
  // (M−2)/(M−1) = 1 − 1/(M−1) < 1 − 1/M = (M−1)/M.
  CHECK(Rational(kMax - 2, kMax - 1) < Rational(kMax - 1, kMax));
  CHECK_FALSE(Rational(kMax - 1, kMax) < Rational(kMax - 2, kMax - 1));
  CHECK(Rational(-(kMax - 1), kMax) < Rational(-(kMax - 2), kMax - 1));
}

TEST_CASE("rational: cents") {
  CHECK(pq::tuning::cents(Rational(2)) == doctest::Approx(1200.0).epsilon(1e-15));
  CHECK(pq::tuning::cents(Rational(3, 2)) == doctest::Approx(701.955000865387).epsilon(1e-14));
  CHECK(pq::tuning::cents(Rational(5, 4)) == doctest::Approx(386.313713864835).epsilon(1e-14));
  CHECK(pq::tuning::cents(Rational(81, 80)) == doctest::Approx(21.5062895967).epsilon(1e-10));
  CHECK(pq::tuning::cents(Rational(2, 3)) == doctest::Approx(-701.955000865387).epsilon(1e-14));
  // Huge numerator/denominator keep precision: 3^39 / 2^61 ≈ 3^39/2^61.
  const double c = pq::tuning::cents(Rational(4052555153018976267LL, int64_t{1} << 61));
  CHECK(c == doctest::Approx(1200.0 * (39.0 * std::log2(3.0) - 61.0)).epsilon(1e-13));
}

// Acceptance (T2.1): composing 50 random intervals stays exact. Each trial multiplies 50 intervals drawn from the
// macro keymap (SPEC §6.2: 2/1 3/2 5/4 … 29/16 and reciprocals) plus some common commas and steps. The oracle tracks
// the true value as a prime-exponent vector: by unique factorisation the composed Rational is exact iff its monzo equals
// the summed exponents. Overflow must be reported exactly when the true numerator or denominator reaches 2^63.
TEST_CASE("rational: composition of 50 random intervals is exact") {
  const std::vector<Rational> pool = {
      Rational(2),      Rational(3, 2),   Rational(5, 4),   Rational(7, 4),    Rational(11, 8),  Rational(13, 8),
      Rational(17, 16), Rational(19, 16), Rational(23, 16), Rational(29, 16),  Rational(9, 8),   Rational(16, 15),
      Rational(81, 80), Rational(7, 6),   Rational(25, 24), Rational(64, 63),  Rational(10, 9),  Rational(225, 224)};
  std::mt19937_64 rng(0x5eed2024u);
  std::uniform_int_distribution<size_t> pick(0, pool.size() - 1);
  std::bernoulli_distribution down(0.5);

  int completed = 0, overflowed = 0;
  constexpr int kTrials = 2000;
  for (int t = 0; t < kTrials; ++t) {
    Rational r{1};
    pq::tuning::Monzo expect;
    double expectCents = 0.0;
    std::vector<Rational> steps;
    bool overflow = false;
    for (int i = 0; i < 50; ++i) {
      Rational step = pool[pick(rng)];
      if (down(rng)) step = *step.reciprocal();
      const auto sm = pq::tuning::toMonzo(step);
      REQUIRE(sm.ok());
      for (int k = 0; k < pq::tuning::kMonzoPrimeCount; ++k) expect.exps[k] += sm->exps[k];
      expectCents += pq::tuning::cents(step);

      const auto next = pq::tuning::mul(r, step);
      // Oracle for representability: log2 of the true numerator/denominator (error ≪ the gap to 63).
      double log2Num = 0.0, log2Den = 0.0;
      for (int k = 0; k < pq::tuning::kMonzoPrimeCount; ++k) {
        const double l = expect.exps[k] * std::log2(static_cast<double>(pq::tuning::kMonzoPrimes[k]));
        (expect.exps[k] > 0 ? log2Num : log2Den) += std::fabs(l);
      }
      const bool fits = log2Num < 63.0 - 1e-9 && log2Den < 63.0 - 1e-9;
      REQUIRE(next.has_value() == fits);
      if (!next) {
        overflow = true;
        break;
      }
      r = *next;
      steps.push_back(step);
      const auto got = pq::tuning::toMonzo(r);
      REQUIRE(got.ok());
      REQUIRE(*got == expect);
    }
    if (overflow) {
      ++overflowed;
      continue;
    }
    ++completed;
    CHECK(pq::tuning::cents(r) == doctest::Approx(expectCents).epsilon(1e-12));
    // Undo every step in reverse: must land exactly on 1/1.
    Rational back = r;
    for (auto it = steps.rbegin(); it != steps.rend(); ++it) back = *pq::tuning::div(back, *it);
    REQUIRE(back == Rational(1));
  }
  MESSAGE("50-interval walks: ", completed, " exact, ", overflowed, " overflow correctly reported");
  CHECK(completed + overflowed == kTrials);
  CHECK(completed >= kTrials / 2);  // the acceptance case (all 50 steps exact) is exercised many times over
}
