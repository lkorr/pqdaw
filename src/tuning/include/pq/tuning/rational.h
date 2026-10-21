#pragma once

// Exact rational numbers for pitch ratios (SPEC §3.1: ratio identities are stored as integers so composition never
// accumulates rounding error; they become Hz only at the final step).
//
// Invariants of every Rational value:
//   - den > 0, gcd(|num|, den) == 1 (always fully reduced; zero is 0/1),
//   - num != INT64_MIN (so negation and abs never overflow).
// Because the form is canonical, == is plain member equality.
//
// Arithmetic is checked: every operation that can overflow returns std::optional and yields nullopt instead of
// wrapping. Gcd cross-cancellation happens before multiplying, so results that fit in int64 are always found even when
// the naive cross products would not fit. All operations are allocation-free and noexcept (RT-safe).

#include <cassert>
#include <compare>
#include <cstdint>
#include <limits>
#include <numeric>
#include <optional>

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif

namespace pq::tuning {

namespace detail {

// a * b with overflow detection. RT-safe.
inline bool mulOverflows(int64_t a, int64_t b, int64_t& out) noexcept {
#if defined(_MSC_VER) && !defined(__clang__)
  int64_t hi = 0;
  const int64_t lo = _mul128(a, b, &hi);
  out = lo;
  return hi != (lo >> 63);
#else
  return __builtin_mul_overflow(a, b, &out);
#endif
}

// a + b with overflow detection. RT-safe.
inline bool addOverflows(int64_t a, int64_t b, int64_t& out) noexcept {
#if defined(_MSC_VER) && !defined(__clang__)
  const uint64_t r = static_cast<uint64_t>(a) + static_cast<uint64_t>(b);
  out = static_cast<int64_t>(r);
  // Overflow iff a and b have the same sign and the result's sign differs.
  return ((a ^ out) & (b ^ out)) < 0;
#else
  return __builtin_add_overflow(a, b, &out);
#endif
}

constexpr uint64_t absU(int64_t v) noexcept {
  return v < 0 ? uint64_t{0} - static_cast<uint64_t>(v) : static_cast<uint64_t>(v);
}

constexpr int64_t gcdI(int64_t a, int64_t b) noexcept {
  // Inputs never include INT64_MIN (class invariant), so the gcd always fits in int64. (A binary/Stein gcd was
  // measured as no faster than std::gcd here: see bench/RESULTS.md, Rational/mul.)
  return static_cast<int64_t>(std::gcd(absU(a), absU(b)));
}

}  // namespace detail

class Rational {
public:
  // 0/1.
  constexpr Rational() noexcept = default;

  // Integer value. Precondition: v != INT64_MIN (use tryMake for untrusted input).
  constexpr Rational(int64_t v) noexcept : num_(v), den_(1) {  // NOLINT: implicit by design (Rational r = 2;)
    assert(v != std::numeric_limits<int64_t>::min());
  }

  // n/d, reduced and sign-normalised. Precondition: d != 0, neither is INT64_MIN. For untrusted input use tryMake.
  constexpr Rational(int64_t n, int64_t d) noexcept {
    const bool okArgs = tryAssign(n, d);
    assert(okArgs);
    (void)okArgs;
  }

  // Checked construction: nullopt if d == 0 or the reduced value cannot satisfy the invariants. RT-safe.
  static constexpr std::optional<Rational> tryMake(int64_t n, int64_t d) noexcept {
    Rational r;
    if (!r.tryAssign(n, d)) return std::nullopt;
    return r;
  }

  constexpr int64_t num() const noexcept { return num_; }
  constexpr int64_t den() const noexcept { return den_; }

  constexpr bool isZero() const noexcept { return num_ == 0; }
  constexpr bool isPositive() const noexcept { return num_ > 0; }
  constexpr bool isInteger() const noexcept { return den_ == 1; }

  constexpr Rational operator-() const noexcept { return fromReduced(-num_, den_); }

  // nullopt for 0. RT-safe.
  constexpr std::optional<Rational> reciprocal() const noexcept {
    if (num_ == 0) return std::nullopt;
    return num_ < 0 ? fromReduced(-den_, -num_) : fromReduced(den_, num_);
  }

  // The value in double: relative error ≤ ~2 ulp. RT-safe.
  double toDouble() const noexcept { return static_cast<double>(num_) / static_cast<double>(den_); }

  // Builds from parts the caller guarantees are already reduced with den > 0 and num != INT64_MIN
  // (e.g. a stored PitchId). Asserts in dev builds.
  static constexpr Rational fromReduced(int64_t n, int64_t d) noexcept {
    Rational r;
    r.num_ = n;
    r.den_ = d;
    assert(d > 0 && n != std::numeric_limits<int64_t>::min() && detail::gcdI(n, d) == 1);
    return r;
  }

  friend constexpr bool operator==(const Rational&, const Rational&) noexcept = default;

  // Exact ordering via 128-bit cross products (never overflows). RT-safe.
  friend std::strong_ordering operator<=>(const Rational& a, const Rational& b) noexcept;

private:
  constexpr bool tryAssign(int64_t n, int64_t d) noexcept {
    constexpr int64_t kMin = std::numeric_limits<int64_t>::min();
    if (d == 0 || n == kMin || d == kMin) return false;
    if (n == 0) {
      num_ = 0;
      den_ = 1;
      return true;
    }
    const int64_t g = detail::gcdI(n, d);
    n /= g;
    d /= g;
    if (d < 0) {
      n = -n;
      d = -d;
    }
    num_ = n;
    den_ = d;
    return true;
  }

  int64_t num_ = 0;
  int64_t den_ = 1;
};

inline std::strong_ordering operator<=>(const Rational& a, const Rational& b) noexcept {
  // Compare a.num * b.den with b.num * a.den (dens are positive, so the direction is preserved).
#if defined(_MSC_VER) && !defined(__clang__)
  int64_t hiA = 0, hiB = 0;
  const uint64_t loA = static_cast<uint64_t>(_mul128(a.num_, b.den_, &hiA));
  const uint64_t loB = static_cast<uint64_t>(_mul128(b.num_, a.den_, &hiB));
  if (hiA != hiB) return hiA <=> hiB;
  return loA <=> loB;
#else
  const __int128 l = static_cast<__int128>(a.num_) * b.den_;
  const __int128 r = static_cast<__int128>(b.num_) * a.den_;
  return l < r ? std::strong_ordering::less : (l > r ? std::strong_ordering::greater : std::strong_ordering::equal);
#endif
}

// a * b, nullopt on overflow. Cross-cancels first, so (2^62/3)·(3/2^62) succeeds. RT-safe.
inline std::optional<Rational> mul(const Rational& a, const Rational& b) noexcept {
  if (a.isZero() || b.isZero()) return Rational{};
  const int64_t g1 = detail::gcdI(a.num(), b.den());
  const int64_t g2 = detail::gcdI(b.num(), a.den());
  int64_t n = 0, d = 0;
  if (detail::mulOverflows(a.num() / g1, b.num() / g2, n)) return std::nullopt;
  if (detail::mulOverflows(a.den() / g2, b.den() / g1, d)) return std::nullopt;
  if (n == std::numeric_limits<int64_t>::min()) return std::nullopt;
  // Already reduced: a and b were reduced and the cross gcds have been removed.
  return Rational::fromReduced(n, d);
}

// a / b, nullopt on overflow or division by zero. RT-safe.
inline std::optional<Rational> div(const Rational& a, const Rational& b) noexcept {
  const auto rb = b.reciprocal();
  if (!rb) return std::nullopt;
  return mul(a, *rb);
}

// a + b, nullopt on overflow. RT-safe.
inline std::optional<Rational> add(const Rational& a, const Rational& b) noexcept {
  // a/b + c/d = (a·(d/g) + c·(b/g)) / (b·(d/g)), g = gcd(b, d); then reduce by gcd(numerator, g) (Knuth 4.5.1).
  const int64_t g = detail::gcdI(a.den(), b.den());
  const int64_t bd = b.den() / g;
  const int64_t ad = a.den() / g;
  int64_t t1 = 0, t2 = 0, n = 0;
  if (detail::mulOverflows(a.num(), bd, t1)) return std::nullopt;
  if (detail::mulOverflows(b.num(), ad, t2)) return std::nullopt;
  if (detail::addOverflows(t1, t2, n)) return std::nullopt;
  if (n == 0) return Rational{};
  if (n == std::numeric_limits<int64_t>::min()) return std::nullopt;
  const int64_t g2 = detail::gcdI(n, g);
  int64_t d = 0;
  if (detail::mulOverflows(a.den() / g2, bd, d)) return std::nullopt;
  // Knuth: gcd(n/g2, d) == 1 here, so no second reduction is needed.
  return Rational::fromReduced(n / g2, d);
}

// a - b, nullopt on overflow. RT-safe.
inline std::optional<Rational> sub(const Rational& a, const Rational& b) noexcept { return add(a, -b); }

// base^exponent (negative exponents allowed), nullopt on overflow or 0^negative. Square-and-multiply: at most
// 2·32 multiplications, so bounded. RT-safe.
inline std::optional<Rational> pow(const Rational& base, int32_t exponent) noexcept {
  Rational b = base;
  if (exponent < 0) {
    const auto r = base.reciprocal();
    if (!r) return std::nullopt;
    b = *r;
  }
  uint32_t e = exponent < 0 ? uint32_t{0} - static_cast<uint32_t>(exponent) : static_cast<uint32_t>(exponent);
  Rational result{1};
  while (e != 0) {
    if (e & 1u) {
      const auto m = mul(result, b);
      if (!m) return std::nullopt;
      result = *m;
    }
    e >>= 1;
    if (e != 0) {
      const auto sq = mul(b, b);
      if (!sq) return std::nullopt;
      b = *sq;
    }
  }
  return result;
}

// Size of a positive ratio in cents (1200·log2(n/d)), computed as log2(n) − log2(d) so huge n and d keep full
// precision. Precondition: r > 0. RT-safe.
double cents(const Rational& r) noexcept;

}  // namespace pq::tuning
