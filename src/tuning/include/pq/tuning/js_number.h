#pragma once

// JavaScript number semantics needed to reproduce the mos-explorer exactly (T2.3): Math.round, the % operator on
// doubles, Number.prototype.toFixed and Number-to-String. Public so tests can pin the edge cases.

#include <cmath>
#include <string>

namespace pq::tuning::js {

// Math.round: nearest integer, halves toward +∞ (Math.round(-2.5) == -2, Math.round(0.49999999999999994) == 0).
// x − floor(x) is exact for doubles, so this has none of floor(x + 0.5)'s double-rounding errors. RT-safe.
inline double round(double x) noexcept {
  if (!std::isfinite(x)) return x;
  const double f = std::floor(x);
  return (x - f >= 0.5) ? f + 1.0 : f;
}

// a % b on doubles: truncated remainder with the sign of a, exact (= C fmod). RT-safe.
inline double mod(double a, double b) noexcept { return std::fmod(a, b); }

// ((a % p) + p) % p: the reduction the JS uses everywhere to put a pitch into [0, p). RT-safe.
inline double wrap(double a, double p) noexcept { return std::fmod(std::fmod(a, p) + p, p); }

// Math.round(x * 100) / 100, the 0.01-cent rounding applied before comparing step sizes. RT-safe.
inline double round2(double x) noexcept { return round(x * 100.0) / 100.0; }

// Number.prototype.toFixed(digits) for |x| < 1e21: exact decimal value rounded half away from zero (the spec picks
// the larger n on a tie), "-" kept for negative x even when the digits are all zero, "Infinity"/"NaN" as in JS.
std::string toFixed(double x, int digits);

// Number::toString (shortest round-trip digits, decimal notation for 1e-7 ≤ |x| < 1e21, otherwise "1.5e+21").
std::string toString(double x);

}  // namespace pq::tuning::js
