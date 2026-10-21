#include "pq/tuning/ji.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <numeric>

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif

namespace pq::tuning {

namespace {

// ---- Trial division by the odd primes below 1000 ----------------------------------------------------------------
// Divisibility by an odd p is tested without dividing: with pinv = p⁻¹ mod 2^64, p | n ⇔ n·pinv ≤ ⌊(2^64−1)/p⌋, and
// then n/p = n·pinv exactly (Granlund–Montgomery). One multiply and compare per prime instead of a 64-bit division.

struct OddPrime {
  uint64_t p;
  uint64_t inv;  // p⁻¹ mod 2^64
  uint64_t lim;  // (2^64 − 1) / p
};

constexpr int kOddPrimesBelow1000 = 167;  // 3 … 997

constexpr std::array<OddPrime, kOddPrimesBelow1000> makeOddPrimes() {
  std::array<OddPrime, kOddPrimesBelow1000> t{};
  size_t k = 0;
  for (uint64_t c = 3; c < 1000; c += 2) {
    bool prime = true;
    for (uint64_t d = 3; d * d <= c; d += 2)
      if (c % d == 0) prime = false;
    if (!prime) continue;
    uint64_t inv = c;  // Newton iteration: each step doubles the correct low bits (3 → 6 → … → 96 ≥ 64)
    for (int i = 0; i < 5; ++i) inv *= 2 - c * inv;
    t[k++] = {c, inv, ~uint64_t{0} / c};
  }
  return t;
}

constexpr std::array<OddPrime, kOddPrimesBelow1000> kOddPrimes = makeOddPrimes();
static_assert(kOddPrimes.back().p == 997 && kOddPrimes[0].inv * 3 == 1);

// Removes every factor p from n (n ≠ 0); returns whether there was one. Bounded: ≤ 40 iterations for p ≥ 3.
inline bool divideOut(uint64_t& n, const OddPrime& op) noexcept {
  if (n * op.inv > op.lim) return false;
  do n *= op.inv;
  while (n * op.inv <= op.lim);
  return true;
}

// ---- Montgomery arithmetic modulo an odd n, for Miller–Rabin and Pollard rho -----------------------------------

inline uint64_t mul128(uint64_t a, uint64_t b, uint64_t& hi) noexcept {
#if defined(_MSC_VER) && !defined(__clang__)
  return _umul128(a, b, &hi);
#else
  const unsigned __int128 p = static_cast<unsigned __int128>(a) * b;
  hi = static_cast<uint64_t>(p >> 64);
  return static_cast<uint64_t>(p);
#endif
}

class Montgomery {
public:
  explicit Montgomery(uint64_t n) noexcept : n_(n) {
    uint64_t inv = n;
    for (int i = 0; i < 5; ++i) inv *= 2 - n * inv;
    nNegInv_ = uint64_t{0} - inv;
    const uint64_t r1 = (uint64_t{0} - n) % n;  // 2^64 mod n
    r2_ = slowMulMod(r1, r1);                   // 2^128 mod n
    one_ = r1;
  }

  uint64_t one() const noexcept { return one_; }
  uint64_t to(uint64_t a) const noexcept { return mul(a % n_, r2_); }

  // a·b·2^−64 mod n, fully reduced (< n), for a, b < n.
  uint64_t mul(uint64_t a, uint64_t b) const noexcept {
    uint64_t hi = 0;
    const uint64_t lo = mul128(a, b, hi);
    return reduce(hi, lo);
  }

  uint64_t pow(uint64_t base, uint64_t e) const noexcept {
    uint64_t r = one_;
    while (e != 0) {
      if (e & 1u) r = mul(r, base);
      base = mul(base, base);
      e >>= 1;
    }
    return r;
  }

private:
  uint64_t reduce(uint64_t hi, uint64_t lo) const noexcept {
    const uint64_t m = lo * nNegInv_;
    uint64_t mnHi = 0;
    const uint64_t mnLo = mul128(m, n_, mnHi);
    // (hi:lo + mnHi:mnLo) / 2^64: the low words sum to 0 mod 2^64, so only their carry matters. The true result is
    // < 2n, which may exceed 2^64 when n ≥ 2^63: the carries detect that.
    const uint64_t carryLo = (lo + mnLo < lo) ? 1 : 0;
    const uint64_t t = hi + mnHi;
    const bool c1 = t < hi;
    const uint64_t t2 = t + carryLo;
    const bool c2 = t2 < t;
    return (c1 || c2 || t2 >= n_) ? t2 - n_ : t2;
  }

  uint64_t slowMulMod(uint64_t a, uint64_t b) const noexcept {
    uint64_t hi = 0;
    const uint64_t lo = mul128(a, b, hi);
#if defined(_MSC_VER) && !defined(__clang__)
    uint64_t rem = 0;
    (void)_udiv128(hi % n_, lo, n_, &rem);  // hi % n < n keeps the quotient in 64 bits
    return rem;
#else
    return static_cast<uint64_t>(((static_cast<unsigned __int128>(hi) << 64) | lo) % n_);
#endif
  }

  uint64_t n_ = 1;
  uint64_t nNegInv_ = 0;
  uint64_t r2_ = 0;
  uint64_t one_ = 0;
};

// Deterministic Miller–Rabin for odd n with no factor below 1000: bases {2, 7, 61} are exact below 4 759 123 141
// (Jaeschke), Sinclair's 7 bases for all n < 2^64.
bool isPrimeLarge(uint64_t n) noexcept {
  constexpr uint64_t kBases32[] = {2, 7, 61};
  constexpr uint64_t kBases64[] = {2, 325, 9375, 28178, 450775, 9780504, 1795265022};
  const Montgomery m(n);
  uint64_t d = n - 1;
  const int s = std::countr_zero(d);
  d >>= s;
  const uint64_t minusOne = m.to(n - 1);
  auto witness = [&](uint64_t a) {  // true if a proves n composite
    a %= n;
    if (a == 0) return false;
    uint64_t x = m.pow(m.to(a), d);
    if (x == m.one() || x == minusOne) return false;
    for (int i = 1; i < s; ++i) {
      x = m.mul(x, x);
      if (x == minusOne) return false;
    }
    return true;
  };
  if (n < 4759123141ull) {
    for (uint64_t a : kBases32)
      if (witness(a)) return false;
  } else {
    for (uint64_t a : kBases64)
      if (witness(a)) return false;
  }
  return true;
}

// A non-trivial factor of an odd composite n (Pollard–Brent rho with batched gcds, in Montgomery form: gcds are
// unaffected because 2^64 is invertible mod n). Tries c = 1, 2, … until one succeeds; for composite n some c does.
uint64_t rhoFactor(uint64_t n) noexcept {
  const Montgomery m(n);
  for (uint64_t c = 1;; ++c) {
    auto f = [&](uint64_t x) {
      const uint64_t y = m.mul(x, x) + c;  // (x² + c) mod n; c < n, so one conditional subtraction
      return (y < c || y >= n) ? y - n : y;
    };
    uint64_t y = m.to(2), x = y, ys = y, q = m.one(), g = 1;
    constexpr uint64_t kBatch = 128;
    for (uint64_t r = 1; g == 1; r <<= 1) {
      x = y;
      for (uint64_t i = 0; i < r; ++i) y = f(y);
      for (uint64_t k = 0; k < r && g == 1; k += kBatch) {
        ys = y;
        const uint64_t lim = std::min(kBatch, r - k);
        for (uint64_t i = 0; i < lim; ++i) {
          y = f(y);
          q = m.mul(q, x > y ? x - y : y - x);
        }
        g = std::gcd(q, n);
      }
    }
    if (g == n) {
      // The batch overshot (or q hit 0): redo it one step at a time.
      do {
        ys = f(ys);
        g = std::gcd(x > ys ? x - ys : ys - x, n);
      } while (g == 1);
    }
    if (g != n) return g;
  }
}

// Largest prime factor of an odd n with no prime factor below 1000.
uint64_t largestFactorLarge(uint64_t n) noexcept {
  if (n == 1) return 1;
  if (n < 1009ull * 1009ull || isPrimeLarge(n)) return n;  // below 1009² it cannot have two factors ≥ 1009
  const uint64_t d = rhoFactor(n);
  return std::max(largestFactorLarge(d), largestFactorLarge(n / d));
}

uint64_t absU(int64_t v) noexcept { return detail::absU(v); }

}  // namespace

// ---- Prime limits ----------------------------------------------------------------------------------------------

uint64_t largestPrimeFactor(uint64_t n) noexcept {
  if (n <= 1) return 1;
  uint64_t largest = 1;
  if ((n & 1u) == 0) {
    largest = 2;
    n >>= std::countr_zero(n);
  }
  // Trial division by the primes below 1000: all that ratios in practice need, and it leaves only large factors.
  for (const OddPrime& op : kOddPrimes) {
    if (op.p * op.p > n) return n > 1 ? std::max(largest, n) : largest;  // no factor ≤ √n left: n is 1 or prime
    if (divideOut(n, op)) largest = op.p;
  }
  return n > 1 ? std::max(largest, largestFactorLarge(n)) : largest;
}

uint64_t primeLimitOf(const Rational& r) noexcept {
  if (r.isZero()) return 0;
  return std::max(largestPrimeFactor(absU(r.num())), largestPrimeFactor(static_cast<uint64_t>(r.den())));
}

bool isWithinPrimeLimit(const Rational& r, uint64_t limit) noexcept {  // RT-safe for limit < 1000
  if (r.isZero()) return false;
  if (limit >= 1000) return primeLimitOf(r) <= limit;
  uint64_t n = absU(r.num()), d = static_cast<uint64_t>(r.den());
  if (limit < 2) return n == 1 && d == 1;
  n >>= std::countr_zero(n);
  d >>= std::countr_zero(d);
  for (const OddPrime& op : kOddPrimes) {
    if (op.p > limit || (n == 1 && d == 1)) break;
    divideOut(n, op);
    divideOut(d, op);
  }
  return n == 1 && d == 1;
}

uint64_t oddLimitOf(const Rational& r) noexcept {  // RT-safe
  if (r.isZero()) return 0;
  const uint64_t n = absU(r.num()), d = static_cast<uint64_t>(r.den());
  return std::max(n >> std::countr_zero(n), d >> std::countr_zero(d));
}

// ---- Complexity ------------------------------------------------------------------------------------------------

double tenneyHeight(const Rational& r) noexcept {  // RT-safe
  return std::log2(static_cast<double>(absU(r.num()))) + std::log2(static_cast<double>(r.den()));
}

double tenneyHeight(const Monzo& m) noexcept {  // RT-safe
  double h = 0.0;
  for (int i = 0; i < kMonzoPrimeCount; ++i)
    if (m.exps[i] != 0) h += std::fabs(static_cast<double>(m.exps[i])) * std::log2(static_cast<double>(kMonzoPrimes[i]));
  return h;
}

std::optional<uint64_t> benedettiHeight(const Rational& r) noexcept {  // RT-safe
  int64_t p = 0;
  if (detail::mulOverflows(static_cast<int64_t>(absU(r.num())), r.den(), p)) return std::nullopt;
  return static_cast<uint64_t>(p);
}

// ---- Equave reduction ------------------------------------------------------------------------------------------

namespace {

struct Reduced {
  Rational value;
  int32_t k;  // r = value · equave^k
};

std::optional<Reduced> octaveReduceImpl(const Rational& r) noexcept {
  if (!r.isPositive()) return std::nullopt;
  // r = 2^t · n/d with n, d odd. Reduce n/d to [1, 2) by a power of two on one side, which keeps it in lowest terms.
  const uint64_t num = static_cast<uint64_t>(r.num()), den = static_cast<uint64_t>(r.den());
  const int tn = std::countr_zero(num), td = std::countr_zero(den);
  const uint64_t n = num >> tn, d = den >> td;
  const int bn = std::bit_width(n), bd = std::bit_width(d);
  int shift = bn - bd;  // n/d · 2^-shift ∈ (1/2, 2)
  // The shifted side has bit width max(bn, bd) ≤ 63, so these comparisons cannot overflow.
  const bool atLeastOne = shift >= 0 ? n >= (d << shift) : (n << -shift) >= d;
  if (!atLeastOne) --shift;  // now n/d · 2^-shift ∈ [1, 2)
  // value = n/d · 2^-shift; k = t + shift where t = tn − td.
  uint64_t vn = n, vd = d;
  if (shift > 0) {
    if (shift >= 63 || (vd >> (63 - shift)) != 0) return std::nullopt;
    vd <<= shift;
  } else if (shift < 0) {
    if (-shift >= 63 || (vn >> (63 + shift)) != 0) return std::nullopt;
    vn <<= -shift;
  }
  return Reduced{Rational::fromReduced(static_cast<int64_t>(vn), static_cast<int64_t>(vd)), tn - td + shift};
}

std::optional<Reduced> reduceImpl(const Rational& r, const Rational& equave) noexcept {
  if (!r.isPositive() || !(equave > Rational{1})) return std::nullopt;
  if (equave == Rational{2}) return octaveReduceImpl(r);
  // Estimate k from the sizes, then correct: the estimate is exact except within rounding of an equave boundary.
  const double est = std::floor(cents(r) / cents(equave));
  if (!(std::fabs(est) < 2.0e9)) return std::nullopt;
  int32_t k = static_cast<int32_t>(est);
  auto shifted = [&](int32_t kk) -> std::optional<Rational> {
    const auto p = pow(equave, -kk);
    if (!p) return std::nullopt;
    return mul(r, *p);
  };
  std::optional<Rational> v = shifted(k);
  for (int fix = 0; fix < 2 && v; ++fix) {
    if (*v >= equave) {
      v = shifted(++k);
    } else if (*v < Rational{1}) {
      v = shifted(--k);
    } else {
      return Reduced{*v, k};
    }
  }
  if (v && *v >= Rational{1} && *v < equave) return Reduced{*v, k};
  return std::nullopt;
}

}  // namespace

std::optional<Rational> reduceToEquave(const Rational& r, const Rational& equave) noexcept {  // RT-safe
  const auto x = reduceImpl(r, equave);
  if (!x) return std::nullopt;
  return x->value;
}

std::optional<Rational> octaveReduce(const Rational& r) noexcept {  // RT-safe
  const auto x = octaveReduceImpl(r);
  if (!x) return std::nullopt;
  return x->value;
}

std::optional<int32_t> equaveIndex(const Rational& r, const Rational& equave) noexcept {  // RT-safe
  const auto x = reduceImpl(r, equave);
  if (!x) return std::nullopt;
  return x->k;
}

// ---- Harmonic and subharmonic series ---------------------------------------------------------------------------

std::optional<Rational> harmonic(uint32_t k) noexcept {  // RT-safe
  if (k == 0) return std::nullopt;
  return Rational(static_cast<int64_t>(k));
}

std::optional<Rational> subharmonic(uint32_t k) noexcept {  // RT-safe
  if (k == 0) return std::nullopt;
  return Rational::fromReduced(1, static_cast<int64_t>(k));
}

std::vector<Rational> harmonicSegment(uint32_t lo, uint32_t hi) {
  std::vector<Rational> out;
  if (lo == 0 || lo > hi) return out;
  out.reserve(static_cast<size_t>(hi - lo) + 1);
  for (uint64_t k = lo; k <= hi; ++k) out.emplace_back(static_cast<int64_t>(k), static_cast<int64_t>(lo));
  return out;
}

std::vector<Rational> subharmonicSegment(uint32_t lo, uint32_t hi) {
  std::vector<Rational> out;
  if (lo == 0 || lo > hi) return out;
  out.reserve(static_cast<size_t>(hi - lo) + 1);
  for (uint64_t k = hi; k >= lo; --k) out.emplace_back(static_cast<int64_t>(hi), static_cast<int64_t>(k));
  return out;
}

std::optional<uint64_t> harmonicNumber(const Rational& r, const Rational& fundamental) noexcept {  // RT-safe
  if (!r.isPositive() || !fundamental.isPositive()) return std::nullopt;
  const auto q = div(r, fundamental);
  if (!q || !q->isInteger()) return std::nullopt;
  return static_cast<uint64_t>(q->num());
}

std::optional<Rational> virtualFundamental(std::span<const Rational> ratios) noexcept {  // RT-safe
  if (ratios.empty()) return std::nullopt;
  int64_t g = 0;  // gcd of numerators
  int64_t l = 1;  // lcm of denominators
  for (const Rational& r : ratios) {
    if (!r.isPositive()) return std::nullopt;
    g = std::gcd(g, r.num());
    const int64_t step = r.den() / std::gcd(l, r.den());
    if (detail::mulOverflows(l, step, l)) return std::nullopt;
  }
  return Rational::tryMake(g, l);
}

std::vector<uint64_t> harmonicNumbers(std::span<const Rational> ratios) {
  std::vector<uint64_t> out;
  const auto f = virtualFundamental(ratios);
  if (!f) return out;
  out.reserve(ratios.size());
  for (const Rational& r : ratios) {
    const auto h = harmonicNumber(r, *f);
    if (!h) return {};
    out.push_back(*h);
  }
  return out;
}

}  // namespace pq::tuning
