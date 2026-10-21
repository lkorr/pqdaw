#include "pq/tuning/monzo.h"

#include <cmath>
#include <string>

namespace pq::tuning {

namespace {

// Divides out every prime ≤ primeLimit from v, adding (sign · count) to exps. Returns the unfactored remainder.
uint64_t factorInto(uint64_t v, uint32_t primeLimit, int32_t sign, Monzo& m) noexcept {
  for (int i = 0; i < kMonzoPrimeCount && kMonzoPrimes[i] <= primeLimit && v > 1; ++i) {
    const uint64_t p = kMonzoPrimes[i];
    while (v % p == 0) {  // at most 63 iterations in total: each one divides v by ≥ 2
      v /= p;
      m.exps[i] += sign;
    }
  }
  return v;
}

// Product of p^e over the positive exponents (sign = +1) or the negated negative ones (sign = −1), overflow-checked.
bool productOf(const Monzo& m, int32_t sign, int64_t& out) noexcept {
  int64_t acc = 1;
  for (int i = 0; i < kMonzoPrimeCount; ++i) {
    const int64_t e = static_cast<int64_t>(m.exps[i]) * sign;
    for (int64_t k = 0; k < e; ++k) {  // bounded: overflow ends the loop after < 64 multiplications
      if (detail::mulOverflows(acc, static_cast<int64_t>(kMonzoPrimes[i]), acc)) return false;
    }
  }
  out = acc;
  return true;
}

}  // namespace

Result<Monzo> toMonzo(const Rational& r, uint32_t primeLimit) {
  if (primeLimit < 2 || primeLimit > kMaxMonzoPrimeLimit)
    return Error{ErrorCode::Unsupported, "monzo prime limit must be in [2, 61], got " + std::to_string(primeLimit), {}};
  if (!r.isPositive()) return Error{ErrorCode::InvalidArgument, "monzo of a non-positive ratio", {}};
  Monzo m;
  const uint64_t restN = factorInto(static_cast<uint64_t>(r.num()), primeLimit, +1, m);
  const uint64_t restD = factorInto(static_cast<uint64_t>(r.den()), primeLimit, -1, m);
  if (restN != 1 || restD != 1) {
    return Error{ErrorCode::InvalidArgument,
                 std::to_string(r.num()) + "/" + std::to_string(r.den()) + " has a prime factor above the " +
                     std::to_string(primeLimit) + "-limit",
                 {}};
  }
  return m;
}

Result<Rational> toRational(const Monzo& m) {
  int64_t n = 0, d = 0;
  if (!productOf(m, +1, n) || !productOf(m, -1, d))
    return Error{ErrorCode::Overflow, "monzo does not fit in an int64 ratio", {}};
  // Distinct primes on each side, so n/d is already reduced.
  return Rational::fromReduced(n, d);
}

uint32_t primeLimitOf(const Monzo& m) noexcept {  // RT-safe
  for (int i = kMonzoPrimeCount - 1; i >= 0; --i)
    if (m.exps[i] != 0) return kMonzoPrimes[i];
  return 1;
}

double cents(const Monzo& m) noexcept {  // RT-safe
  double c = 0.0;
  for (int i = 0; i < kMonzoPrimeCount; ++i)
    if (m.exps[i] != 0) c += static_cast<double>(m.exps[i]) * std::log2(static_cast<double>(kMonzoPrimes[i]));
  return 1200.0 * c;
}

}  // namespace pq::tuning
