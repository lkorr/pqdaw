#pragma once

// Monzos: a positive rational as a vector of prime exponents, 3/2 = [-1 1⟩, 81/80 = [-4 4 -1⟩.
// SPEC §3.1 keeps ratio identities "also as a monzo up to a prime limit". The fixed basis is the primes up to 61
// (18 primes): it covers the macro keymap (primes to 29, SPEC §6.2), the patent vals (primes to 31) and leaves room for
// higher-limit JI work (T2.5). A Monzo is a flat 72-byte array: no allocation, trivially copyable.

#include <array>
#include <cstdint>

#include "pq/core/result.h"
#include "pq/tuning/rational.h"

namespace pq::tuning {

inline constexpr int kMonzoPrimeCount = 18;
inline constexpr std::array<uint32_t, kMonzoPrimeCount> kMonzoPrimes = {2,  3,  5,  7,  11, 13, 17, 19, 23,
                                                                         29, 31, 37, 41, 43, 47, 53, 59, 61};
inline constexpr uint32_t kMaxMonzoPrimeLimit = 61;

struct Monzo {
  std::array<int32_t, kMonzoPrimeCount> exps{};  // exps[i] is the exponent of kMonzoPrimes[i]

  friend constexpr bool operator==(const Monzo&, const Monzo&) noexcept = default;
};

// Factorises a positive ratio over the primes ≤ primeLimit (2 ≤ primeLimit ≤ 61).
// Errors: InvalidArgument for r ≤ 0 or when r has a prime factor above primeLimit; Unsupported for a prime limit
// outside [2, 61]. (Exponents of an int64 ratio are < 64, so they cannot overflow.)
Result<Monzo> toMonzo(const Rational& r, uint32_t primeLimit = kMaxMonzoPrimeLimit);

// Rebuilds the ratio. Error Overflow if numerator or denominator do not fit in int64.
Result<Rational> toRational(const Monzo& m);

// Largest prime with a non-zero exponent; 1 for the unison [0…⟩.
uint32_t primeLimitOf(const Monzo& m) noexcept;

// Size in cents: Σ eᵢ · 1200·log2(pᵢ). RT-safe.
double cents(const Monzo& m) noexcept;

}  // namespace pq::tuning
