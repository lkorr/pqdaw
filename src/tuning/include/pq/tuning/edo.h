#pragma once

// Equal divisions of an equave (SPEC §3.2: EDO { n, equave = 2/1, any ratio or cents }).
// Any n ≥ 1 and any equave > 1/1. Step k sits at k · equave/n above the reference.

#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include "pq/core/result.h"
#include "pq/tuning/monzo.h"
#include "pq/tuning/rational.h"

namespace pq::tuning {

class Edo {
public:
  // n ≥ 1, equave > 1. Errors: InvalidArgument.
  static Result<Edo> make(uint32_t n, const Rational& equave = Rational{2});
  // Equave given in cents (> 0, finite), e.g. a stretched octave. Errors: InvalidArgument.
  static Result<Edo> makeCents(uint32_t n, double equaveCents);
  // 12-EDO of 2/1, the default tuning.
  static Edo standard12() noexcept;

  uint32_t n() const noexcept { return n_; }
  double equaveCents() const noexcept { return equaveCents_; }
  double stepCents() const noexcept { return equaveCents_ / static_cast<double>(n_); }
  double log2Equave() const noexcept { return log2Equave_; }
  // The exact equave, if it was given as a ratio.
  std::optional<Rational> equaveRatio() const noexcept {
    return hasEquaveRatio_ ? std::optional<Rational>{equaveRatio_} : std::nullopt;
  }

  // log2 of the frequency ratio of absolute step `step` over the reference: one multiply. RT-safe.
  double log2OfStep(int64_t step) const noexcept { return static_cast<double>(step) * log2Step_; }
  // Cents of absolute step `step`. RT-safe.
  double centsOfStep(int64_t step) const noexcept { return 1200.0 * log2OfStep(step); }

  friend bool operator==(const Edo&, const Edo&) noexcept = default;

private:
  Edo(uint32_t n, double log2Equave, std::optional<Rational> ratio) noexcept;

  uint32_t n_ = 12;
  bool hasEquaveRatio_ = true;
  Rational equaveRatio_{2};
  double log2Equave_ = 1.0;
  double equaveCents_ = 1200.0;
  double log2Step_ = 1.0 / 12.0;  // log2Equave_ / n_, precomputed for the resolver
};

// Vals over the primes up to 31 (the first 11 monzo primes).
inline constexpr int kValPrimeCount = 11;

struct Val {
  std::array<int32_t, kValPrimeCount> steps{};  // steps[i] = steps that kMonzoPrimes[i] maps to

  friend constexpr bool operator==(const Val&, const Val&) noexcept = default;
};

// Patent val: each prime p maps to round(n · log2(p) / log2(equave)), i.e. its nearest step counted in this EDO's
// own equave (so 13-ED3 gives ⟨8 13 19 23 …]). RT-safe.
Val patentVal(const Edo& edo) noexcept;

// Steps a ratio maps to under a val (val · monzo). nullopt if the monzo uses a prime above 31. RT-safe.
std::optional<int64_t> mapToSteps(const Val& val, const Monzo& m) noexcept;

// An absolute step split into equave index and degree within the equave (floor division: step −1 of 12-EDO is
// degree 11 of equave −1). RT-safe.
struct EdoRow {
  int64_t equave;
  uint32_t degree;
};
EdoRow edoRow(int64_t step, uint32_t n) noexcept;

// "k\n" step label, e.g. stepLabel(7, 12) == "7\12", stepLabel(-3, 31) == "-3\31". Piano-roll rows use
// stepLabel(edoRow(s, n).degree, n) plus the equave index (SPEC §4.1).
std::string stepLabel(int64_t k, uint32_t n);

}  // namespace pq::tuning
