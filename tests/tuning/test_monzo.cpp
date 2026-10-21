#include <doctest/doctest.h>

#include <random>

#include "pq/tuning/monzo.h"

using pq::tuning::Monzo;
using pq::tuning::Rational;

namespace {
Monzo monzoOf(std::initializer_list<int32_t> exps) {
  Monzo m;
  int i = 0;
  for (int32_t e : exps) m.exps[i++] = e;
  return m;
}
}  // namespace

TEST_CASE("monzo: known ratios") {
  CHECK(*pq::tuning::toMonzo(Rational(1)) == Monzo{});
  CHECK(*pq::tuning::toMonzo(Rational(3, 2)) == monzoOf({-1, 1}));
  CHECK(*pq::tuning::toMonzo(Rational(81, 80)) == monzoOf({-4, 4, -1}));
  CHECK(*pq::tuning::toMonzo(Rational(225, 224)) == monzoOf({-5, 2, 2, -1}));
  CHECK(*pq::tuning::toMonzo(Rational(29, 16)) == monzoOf({-4, 0, 0, 0, 0, 0, 0, 0, 0, 1}));
  CHECK(*pq::tuning::toMonzo(Rational(61, 1)) ==
        monzoOf({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}));
  CHECK(*pq::tuning::toMonzo(Rational(int64_t{1} << 62)) == monzoOf({62}));
}

TEST_CASE("monzo: prime limit") {
  CHECK(pq::tuning::primeLimitOf(Monzo{}) == 1);
  CHECK(pq::tuning::primeLimitOf(*pq::tuning::toMonzo(Rational(3, 2))) == 3);
  CHECK(pq::tuning::primeLimitOf(*pq::tuning::toMonzo(Rational(7, 4))) == 7);
  CHECK(pq::tuning::primeLimitOf(*pq::tuning::toMonzo(Rational(2))) == 2);

  CHECK(pq::tuning::toMonzo(Rational(7, 4), 7).ok());
  const auto tooHigh = pq::tuning::toMonzo(Rational(7, 4), 5);
  REQUIRE_FALSE(tooHigh.ok());
  CHECK(tooHigh.error().code == pq::ErrorCode::InvalidArgument);
  CHECK_FALSE(pq::tuning::toMonzo(Rational(67), 61).ok());  // 67 is beyond the monzo basis
  CHECK_FALSE(pq::tuning::toMonzo(Rational(1, 37), 31).ok());
  CHECK(pq::tuning::toMonzo(Rational(1, 37), 37).ok());
  CHECK(pq::tuning::toMonzo(Rational(3, 2), 62).error().code == pq::ErrorCode::Unsupported);
  CHECK(pq::tuning::toMonzo(Rational(3, 2), 1).error().code == pq::ErrorCode::Unsupported);
  CHECK(pq::tuning::toMonzo(Rational{}).error().code == pq::ErrorCode::InvalidArgument);
  CHECK(pq::tuning::toMonzo(Rational(-3, 2)).error().code == pq::ErrorCode::InvalidArgument);
}

TEST_CASE("monzo: back to rational, overflow checked") {
  CHECK(*pq::tuning::toRational(monzoOf({-4, 4, -1})) == Rational(81, 80));
  CHECK(*pq::tuning::toRational(Monzo{}) == Rational(1));
  CHECK(*pq::tuning::toRational(monzoOf({62})) == Rational(int64_t{1} << 62));
  const auto over = pq::tuning::toRational(monzoOf({63}));
  REQUIRE_FALSE(over.ok());
  CHECK(over.error().code == pq::ErrorCode::Overflow);
  CHECK(pq::tuning::toRational(monzoOf({0, -40})).error().code == pq::ErrorCode::Overflow);
  CHECK(*pq::tuning::toRational(monzoOf({0, -39})) == Rational(1, 4052555153018976267LL));
}

TEST_CASE("monzo: random round trips and cents") {
  std::mt19937_64 rng(42);
  for (int t = 0; t < 2000; ++t) {
    // Random 61-limit ratio with numerator and denominator below 2^62.
    int64_t parts[2] = {1, 1};
    for (int64_t& part : parts) {
      for (int k = 0; k < 8; ++k) {
        const int64_t p = pq::tuning::kMonzoPrimes[rng() % pq::tuning::kMonzoPrimeCount];
        if (part > (int64_t{1} << 55)) break;
        part *= p;
      }
    }
    const Rational r(parts[0], parts[1]);
    const auto m = pq::tuning::toMonzo(r);
    REQUIRE(m.ok());
    REQUIRE(*pq::tuning::toRational(*m) == r);
    CHECK(pq::tuning::cents(*m) == doctest::Approx(pq::tuning::cents(r)).epsilon(1e-9).scale(1.0));
  }
}
