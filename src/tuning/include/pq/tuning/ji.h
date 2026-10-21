#pragma once

// Just-intonation utilities (T2.5): prime limits, Tenney height, equave reduction, harmonic/subharmonic series and
// virtual fundamentals. All work on exact Rationals; anything that could overflow returns nullopt.
//
// RT-safety: everything here is allocation-free and noexcept except the functions returning std::vector.
// Functions marked RT-safe are also bounded; largestPrimeFactor on an arbitrary 64-bit number (Pollard rho) is not
// marked RT-safe because its running time depends on the input (it is still allocation-free).

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "pq/tuning/monzo.h"
#include "pq/tuning/rational.h"

namespace pq::tuning {

// ---- Prime limits ----------------------------------------------------------------------------------------------

// Largest prime factor of n; 1 for n ≤ 1. Exact for every uint64: trial division by the primes below 1000 (multiply
// by the inverse instead of dividing), then deterministic Miller–Rabin and Pollard–Brent rho in Montgomery form for
// what is left. Not RT-safe: the time depends on the input (tens of ns for typical JI ratios, see bench/RESULTS.md
// for random 64-bit numbers).
uint64_t largestPrimeFactor(uint64_t n) noexcept;

// Prime limit of a non-zero ratio: the largest prime dividing its numerator or denominator; 1 for ±1/1.
// (For zero, 0.) Not RT-safe (see largestPrimeFactor).
uint64_t primeLimitOf(const Rational& r) noexcept;

// True if every prime factor of r is ≤ limit (r ≠ 0). For limit < 1000 it divides out only the primes ≤ limit
// (≤ 168 multiply-compares plus ≤ 126 exact divisions by multiplication), so it is bounded and RT-safe; for larger
// limits it falls back to primeLimitOf.
bool isWithinPrimeLimit(const Rational& r, uint64_t limit) noexcept;

// Odd limit: the larger of the numerator and denominator with all factors of 2 removed (r ≠ 0). RT-safe.
uint64_t oddLimitOf(const Rational& r) noexcept;

// ---- Complexity ------------------------------------------------------------------------------------------------

// Tenney height log2(|n|·d) of a non-zero ratio, computed as log2|n| + log2 d (no overflow). RT-safe.
double tenneyHeight(const Rational& r) noexcept;
// Tenney height of a monzo: Σ |eᵢ|·log2 pᵢ (equals tenneyHeight of the ratio). RT-safe.
double tenneyHeight(const Monzo& m) noexcept;
// Benedetti height |n|·d, nullopt on overflow. RT-safe.
std::optional<uint64_t> benedettiHeight(const Rational& r) noexcept;

// ---- Equave reduction ------------------------------------------------------------------------------------------

// r·equave^k for the unique integer k that puts the result in [1, equave): 3/1 → 3/2, 1/3 → 4/3, 2/1 → 1/1.
// Requirements: r > 0, equave > 1; nullopt otherwise or on overflow. Bounded: k is estimated from the cents sizes and
// corrected by at most one step either way. RT-safe.
std::optional<Rational> reduceToEquave(const Rational& r, const Rational& equave = Rational{2}) noexcept;

// Octave reduction (equave 2/1) with shifts instead of multiplications. RT-safe.
std::optional<Rational> octaveReduce(const Rational& r) noexcept;

// The number of equaves k by which reduceToEquave shifted (r = reduced · equave^k), i.e. floor(log_equave r).
// nullopt under the same conditions as reduceToEquave. RT-safe.
std::optional<int32_t> equaveIndex(const Rational& r, const Rational& equave = Rational{2}) noexcept;

// ---- Harmonic and subharmonic series ---------------------------------------------------------------------------

// Harmonic k: k/1 (k ≥ 1). Subharmonic k: 1/k. nullopt for k = 0. RT-safe.
std::optional<Rational> harmonic(uint32_t k) noexcept;
std::optional<Rational> subharmonic(uint32_t k) noexcept;

// An otonal segment lo:lo+1:…:hi as ratios over its lowest note: 4..7 → 1/1 5/4 3/2 7/4. Empty if lo == 0 or lo > hi.
std::vector<Rational> harmonicSegment(uint32_t lo, uint32_t hi);
// A utonal segment 1/hi … 1/lo (ascending pitch) as ratios over its lowest note 1/hi: 4..6 → 1/1 6/5 3/2
// (i.e. hi/k for k = hi down to lo). Empty if lo == 0 or lo > hi.
std::vector<Rational> subharmonicSegment(uint32_t lo, uint32_t hi);

// The harmonic number of `r` over `fundamental` if r/fundamental is a positive integer (r = fundamental·k), else
// nullopt. RT-safe.
std::optional<uint64_t> harmonicNumber(const Rational& r, const Rational& fundamental) noexcept;

// Virtual fundamental of a chord: the largest f such that every ratio is an integer multiple of f, i.e.
// gcd(numerators) / lcm(denominators). 1/1 5/4 3/2 → 1/4 (the chord is 4:5:6 over it). nullopt for an empty set, a
// non-positive ratio, or overflow. RT-safe (one pass, no allocation).
std::optional<Rational> virtualFundamental(std::span<const Rational> ratios) noexcept;

// The chord as integer harmonics of its virtual fundamental (1/1 5/4 3/2 → 4 5 6). Empty on the same conditions as
// virtualFundamental returning nullopt.
std::vector<uint64_t> harmonicNumbers(std::span<const Rational> ratios);

}  // namespace pq::tuning
