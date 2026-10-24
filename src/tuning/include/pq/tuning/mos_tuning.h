#pragma once

// A MOS tuning context (SPEC §3.2: MOS { nL, nS, period, generator, mode rotation, modmos[], octave stretch }).
// The degree pitches are computed once, by the mos-explorer's buildFullScale, when the Mos is made; resolving a
// degree is then one table read, one multiply-add and the stretch (RT-safe).

#include <cstdint>
#include <span>
#include <vector>

#include "pq/core/result.h"
#include "pq/tuning/mos.h"

namespace pq::tuning {

class Mos {
public:
  // Requirements: generator, period finite; period > 0; n ≥ 1; periodsPerEquave ≥ 1; 0 ≤ rotation < n;
  // octaveStretch finite and > −1200; every modmos / genSequence value finite. Errors: InvalidArgument.
  static Result<Mos> make(MosParams params);

  const MosParams& params() const noexcept { return params_; }
  // Degrees per equave (n · periodsPerEquave).
  int32_t size() const noexcept { return static_cast<int32_t>(pitches_.size()); }
  // Unstretched equave (period · periodsPerEquave) in cents.
  double equaveCents() const noexcept { return equaveCents_; }
  // Unstretched degree pitches within one equave, ascending from 0.
  std::span<const double> pitches() const noexcept { return pitches_; }

  // Stretched cents of absolute degree `degree` (degree d of equave e is e·size + d), relative to the reference.
  // Matches the mos-explorer's stretchCents(pitches[d] + e·octave). RT-safe.
  double centsOfDegree(int64_t degree) const noexcept;
  // Same for degree `degree` of equave `equave` (a Degree pitch identity). Skips the division when
  // 0 ≤ degree < size(), the normal case. RT-safe.
  double centsOf(int32_t degree, int32_t equave) const noexcept {
    if (degree >= 0 && degree < size()) {
      const double pure = pitches_[static_cast<size_t>(degree)] + static_cast<double>(equave) * equaveCents_;
      return pure * stretchedOctave(params_.octaveStretch) / 1200.0;
    }
    return centsOfDegree(static_cast<int64_t>(equave) * size() + degree);
  }

  // Live morph (T3.12): this MOS at another generator inside one MosLinearTable interval, from the interval's rotated
  // linear form (rotateLinearForm for params().rotation: degree j of the period at k[j]·g − m[j]·period). Rewrites
  // the pitches in place (every period of the equave) and sets params().generator / region; nothing else changes.
  // Requires: k, m of size params().n, no MODMOS and no generator sequence, and the interval to contain `generator`
  // away from its ends (where buildMOSScale's degenerate-chain nudge applies). Equal to Mos::make of the same
  // params up to float rounding (~1e-12 cents). Allocation-free; RT-safe.
  void retuneLinear(double generator, std::span<const int32_t> k, std::span<const int32_t> m,
                    const std::optional<RegionHint>& region) noexcept;

private:
  Mos() = default;

  MosParams params_;
  std::vector<double> pitches_;
  double equaveCents_ = 1200.0;
};

}  // namespace pq::tuning
