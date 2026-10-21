// JI utilities (T2.5): prime and odd limits, Tenney/Benedetti height, equave reduction, harmonic series, virtual
// fundamentals.

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

#include "pq/tuning/ji.h"
#include "pq/tuning/monzo.h"

using namespace pq::tuning;

namespace {

// Reference: plain trial division (slow but obviously right for the sizes used here).
uint64_t lpfNaive(uint64_t n) {
  if (n <= 1) return 1;
  uint64_t largest = 1;
  for (uint64_t p = 2; p * p <= n; ++p)
    while (n % p == 0) {
      largest = p;
      n /= p;
    }
  return n > 1 ? n : largest;
}

}  // namespace

TEST_CASE("ji: largest prime factor") {
  CHECK(largestPrimeFactor(0) == 1);
  CHECK(largestPrimeFactor(1) == 1);
  CHECK(largestPrimeFactor(2) == 2);
  CHECK(largestPrimeFactor(1024) == 2);
  CHECK(largestPrimeFactor(81) == 3);
  CHECK(largestPrimeFactor(80) == 5);
  CHECK(largestPrimeFactor(2 * 3 * 5 * 7 * 11 * 13 * 17 * 19 * 23 * 29ull) == 29);
  CHECK(largestPrimeFactor(997) == 997);
  CHECK(largestPrimeFactor(1009) == 1009);
  CHECK(largestPrimeFactor(1009ull * 1013) == 1013);
  // Hard cases for the Pollard path: big primes and semiprimes of two ~32-bit primes.
  CHECK(largestPrimeFactor(18446744073709551557ull) == 18446744073709551557ull);  // largest 64-bit prime
  CHECK(largestPrimeFactor(4294967291ull * 4294967279ull) == 4294967291ull);
  CHECK(largestPrimeFactor(9223372036854775783ull) == 9223372036854775783ull);   // largest prime < 2^63
  CHECK(largestPrimeFactor(2ull * 9223372036854775783ull) == 9223372036854775783ull);
  CHECK(largestPrimeFactor(UINT64_MAX) == 6700417);  // 3·5·17·257·641·65537·6700417
  CHECK(largestPrimeFactor(1000003ull * 1000003ull * 1000033ull) == 1000033);  // square factor
  CHECK(largestPrimeFactor(3215031751ull) == 28351);  // strong pseudoprime to bases 2, 3, 5, 7: 151·751·28351
  // Random agreement with trial division.
  std::mt19937_64 rng(5);
  for (int i = 0; i < 2000; ++i) {
    const uint64_t n = rng() % 2000000000ull + 1;
    CAPTURE(n);
    CHECK(largestPrimeFactor(n) == lpfNaive(n));
  }
}

TEST_CASE("ji: prime limit and odd limit of ratios") {
  CHECK(primeLimitOf(Rational(1)) == 1);
  CHECK(primeLimitOf(Rational(2)) == 2);
  CHECK(primeLimitOf(Rational(81, 80)) == 5);
  CHECK(primeLimitOf(Rational(7, 4)) == 7);
  CHECK(primeLimitOf(Rational(-11, 8)) == 11);
  CHECK(primeLimitOf(Rational(64, 61)) == 61);
  CHECK(primeLimitOf(Rational(67, 64)) == 67);
  CHECK(primeLimitOf(Rational(0)) == 0);
  CHECK(primeLimitOf(Rational(INT64_MAX, 2)) == 649657);  // 2^63−1 = 7²·73·127·337·92737·649657

  CHECK(isWithinPrimeLimit(Rational(81, 80), 5));
  CHECK_FALSE(isWithinPrimeLimit(Rational(81, 80), 3));
  CHECK(isWithinPrimeLimit(Rational(1), 2));
  CHECK(isWithinPrimeLimit(Rational(1024, 729), 3));
  CHECK_FALSE(isWithinPrimeLimit(Rational(7, 4), 5));
  CHECK(isWithinPrimeLimit(Rational(7, 4), 8));  // a non-prime limit works as "all primes ≤ limit"
  CHECK_FALSE(isWithinPrimeLimit(Rational(7, 4), 6));
  CHECK(isWithinPrimeLimit(Rational(61, 60), 61));
  CHECK_FALSE(isWithinPrimeLimit(Rational(67, 64), 61));
  CHECK(isWithinPrimeLimit(Rational(67, 64), 67));   // above 61: general path
  CHECK_FALSE(isWithinPrimeLimit(Rational(71, 64), 67));
  CHECK_FALSE(isWithinPrimeLimit(Rational(0), 61));
  // Agrees with toMonzo's prime-limit check.
  for (int64_t n = 1; n <= 60; ++n)
    for (int64_t d = 1; d <= 60; ++d) {
      const Rational r(n, d);
      for (uint32_t lim : {2u, 3u, 5u, 7u, 11u, 13u}) CHECK(isWithinPrimeLimit(r, lim) == toMonzo(r, lim).ok());
    }

  CHECK(oddLimitOf(Rational(3, 2)) == 3);
  CHECK(oddLimitOf(Rational(5, 3)) == 5);
  CHECK(oddLimitOf(Rational(16, 15)) == 15);
  CHECK(oddLimitOf(Rational(8)) == 1);
  CHECK(oddLimitOf(Rational(-9, 7)) == 9);
}

TEST_CASE("ji: Tenney and Benedetti height") {
  CHECK(tenneyHeight(Rational(1)) == 0.0);
  CHECK(tenneyHeight(Rational(3, 2)) == doctest::Approx(std::log2(6.0)).epsilon(1e-15));
  CHECK(tenneyHeight(Rational(81, 80)) == doctest::Approx(std::log2(6480.0)).epsilon(1e-15));
  CHECK(tenneyHeight(Rational(-5, 4)) == doctest::Approx(std::log2(20.0)).epsilon(1e-15));
  // No overflow for huge parts: log2((2^62+1)·(2^62−1)) ≈ 124.
  const Rational big((int64_t{1} << 62) + 1, (int64_t{1} << 62) - 1);
  CHECK(tenneyHeight(big) == doctest::Approx(124.0).epsilon(1e-12));
  // Monzo form agrees.
  for (const Rational& r : {Rational(81, 80), Rational(15, 14), Rational(1024, 729), Rational(64, 61)})
    CHECK(tenneyHeight(*toMonzo(r)) == doctest::Approx(tenneyHeight(r)).epsilon(1e-14));
  CHECK(benedettiHeight(Rational(81, 80)) == 6480u);
  CHECK(benedettiHeight(big) == std::nullopt);
}

TEST_CASE("ji: octave and equave reduction") {
  CHECK(octaveReduce(Rational(3)) == Rational(3, 2));
  CHECK(octaveReduce(Rational(1, 3)) == Rational(4, 3));
  CHECK(octaveReduce(Rational(2)) == Rational(1));
  CHECK(octaveReduce(Rational(1, 2)) == Rational(1));
  CHECK(octaveReduce(Rational(1)) == Rational(1));
  CHECK(octaveReduce(Rational(15, 8)) == Rational(15, 8));
  CHECK(octaveReduce(Rational(81, 1)) == Rational(81, 64));
  CHECK(octaveReduce(Rational(1, 81)) == Rational(128, 81));
  CHECK(octaveReduce(Rational(int64_t{1} << 62)) == Rational(1));
  CHECK(octaveReduce(Rational(1, int64_t{1} << 62)) == Rational(1));
  CHECK(octaveReduce(Rational(INT64_MAX)) == Rational(INT64_MAX, int64_t{1} << 62));
  CHECK(octaveReduce(Rational(1, INT64_MAX)) == std::nullopt);  // 2^63 / (2^63 − 1): numerator does not fit
  CHECK(octaveReduce(Rational(0)) == std::nullopt);
  CHECK(octaveReduce(Rational(-3, 2)) == std::nullopt);
  CHECK(equaveIndex(Rational(3)) == 1);
  CHECK(equaveIndex(Rational(1, 3)) == -2);
  CHECK(equaveIndex(Rational(1024)) == 10);
  CHECK(equaveIndex(Rational(1023, 1024)) == -1);

  // Tritave and other equaves.
  CHECK(reduceToEquave(Rational(5), Rational(3)) == Rational(5, 3));
  CHECK(reduceToEquave(Rational(1, 5), Rational(3)) == Rational(9, 5));
  CHECK(reduceToEquave(Rational(9), Rational(3)) == Rational(1));
  CHECK(reduceToEquave(Rational(7, 2), Rational(3, 2)) == Rational(28, 27));
  CHECK(equaveIndex(Rational(7, 2), Rational(3, 2)) == 3);
  CHECK(reduceToEquave(Rational(3, 2), Rational(2)) == Rational(3, 2));
  CHECK(reduceToEquave(Rational(3, 2), Rational(1)) == std::nullopt);   // equave must be > 1
  CHECK(reduceToEquave(Rational(3, 2), Rational(1, 2)) == std::nullopt);
  // Exact powers of the equave land on 1/1 even where the float estimate is borderline.
  for (int k = -30; k <= 30; ++k) {
    const auto p = pow(Rational(3), k);
    REQUIRE(p);
    CHECK(reduceToEquave(*p, Rational(3)) == Rational(1));
    CHECK(equaveIndex(*p, Rational(3)) == k);
  }

  // A result that does not fit int64 is nullopt: 99999 / (5/4)^51 needs 5^51 in the denominator.
  CHECK(reduceToEquave(Rational(99999), Rational(5, 4)) == std::nullopt);

  // Property check against the definition for random ratios (sizes chosen so the reduced ratio fits int64:
  // with equave 5/4, k ≤ 21 for parts ≤ 100, and 5^21 · 100 < 2^63).
  std::mt19937_64 rng(11);
  for (int i = 0; i < 3000; ++i) {
    const uint64_t range = (i % 3 == 2) ? 100 : 100000;
    const Rational r(static_cast<int64_t>(rng() % range + 1), static_cast<int64_t>(rng() % range + 1));
    const Rational e = (i % 3 == 0) ? Rational(2) : (i % 3 == 1 ? Rational(3) : Rational(5, 4));
    CAPTURE(r.num());
    CAPTURE(r.den());
    const auto red = reduceToEquave(r, e);
    const auto k = equaveIndex(r, e);
    REQUIRE(red);
    REQUIRE(k);
    CHECK(*red >= Rational(1));
    CHECK(*red < e);
    CHECK(mul(*red, *pow(e, *k)) == r);
    if (e == Rational(2)) CHECK(*red == *octaveReduce(r));
  }
}

TEST_CASE("ji: harmonic and subharmonic series") {
  CHECK(harmonic(0) == std::nullopt);
  CHECK(harmonic(7) == Rational(7));
  CHECK(subharmonic(7) == Rational(1, 7));
  CHECK(subharmonic(0) == std::nullopt);
  CHECK(harmonicSegment(4, 7) == std::vector<Rational>{Rational(1), Rational(5, 4), Rational(3, 2), Rational(7, 4)});
  CHECK(subharmonicSegment(4, 6) == std::vector<Rational>{Rational(1), Rational(6, 5), Rational(3, 2)});
  CHECK(harmonicSegment(1, 1) == std::vector<Rational>{Rational(1)});
  CHECK(harmonicSegment(0, 3).empty());
  CHECK(harmonicSegment(5, 4).empty());
  CHECK(subharmonicSegment(8, 16).size() == 9);
  CHECK(subharmonicSegment(8, 16).back() == Rational(2));
  // Segments are ascending.
  const auto u = subharmonicSegment(3, 11);
  for (size_t i = 1; i < u.size(); ++i) CHECK(u[i - 1] < u[i]);

  CHECK(harmonicNumber(Rational(5, 4), Rational(1, 4)) == 5u);
  CHECK(harmonicNumber(Rational(330), Rational(55)) == 6u);
  CHECK(harmonicNumber(Rational(5, 4), Rational(1, 3)) == std::nullopt);
  CHECK(harmonicNumber(Rational(1), Rational(0)) == std::nullopt);
}

TEST_CASE("ji: virtual fundamental and harmonic numbers of a chord") {
  const std::vector<Rational> major = {Rational(1), Rational(5, 4), Rational(3, 2)};
  CHECK(virtualFundamental(major) == Rational(1, 4));
  CHECK(harmonicNumbers(major) == std::vector<uint64_t>{4, 5, 6});
  const std::vector<Rational> minor = {Rational(1), Rational(6, 5), Rational(3, 2)};
  CHECK(virtualFundamental(minor) == Rational(1, 10));
  CHECK(harmonicNumbers(minor) == std::vector<uint64_t>{10, 12, 15});
  const std::vector<Rational> dom7 = {Rational(3, 2), Rational(15, 8), Rational(9, 4), Rational(21, 8)};
  CHECK(virtualFundamental(dom7) == Rational(3, 8));
  CHECK(harmonicNumbers(dom7) == std::vector<uint64_t>{4, 5, 6, 7});
  const std::vector<Rational> high = {Rational(6), Rational(9)};
  CHECK(virtualFundamental(high) == Rational(3));
  CHECK(virtualFundamental(std::vector<Rational>{}) == std::nullopt);
  CHECK(virtualFundamental(std::vector<Rational>{Rational(1), Rational(-3, 2)}) == std::nullopt);
  CHECK(harmonicNumbers(std::vector<Rational>{}).empty());
  // lcm overflow.
  const std::vector<Rational> coprime = {Rational(1, 4294967291), Rational(1, 4294967279), Rational(1, 4294967231)};
  CHECK(virtualFundamental(coprime) == std::nullopt);
}
