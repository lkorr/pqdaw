#pragma once

// A scale tuning context (SPEC §3.2: Scale { cents[], equave }, from a Scala .scl file).
// Degree 0 is 1/1 at the tuning's reference pitch; degrees 1..n−1 are the file's pitches; the last pitch of the file
// is the equave (formal octave), so degree d of equave e sits at cents[d] + e·equave. Degrees outside [0, n) wrap by
// floor division, as Scala's octave extension does. The .scl pitches are kept (exact ratios stay exact) so the
// tuning can be written back out; the cents table is computed once at make() and resolving is one table read and one
// multiply-add (RT-safe).
//
// A .kbm keyboard mapping is not part of the Scale: it decides which degree a MIDI key plays (mapKey, scala.h) and,
// through its reference key/frequency, the tuning's reference pitch (makeScalaTuning, tuning.h).

#include <cstdint>
#include <span>
#include <vector>

#include "pq/core/result.h"
#include "pq/tuning/scala.h"

namespace pq::tuning {

class Scale {
public:
  // Requirements: at least one pitch (the equave) and every pitch finite. Errors: InvalidArgument.
  // As in Scala, nothing else is restricted: pitches may be below 1/1 or out of order, and the equave may be 1/1 or
  // below (real files do this: a descending scale ending on 16/29, harmonics + subharmonics ending on 1/1), which
  // makes the octave extension descend or repeat in place. A local check over all 5401 files of the Scala archive
  // (version 94; not part of the repo) built a Scale from every one.
  static Result<Scale> make(ScalaScale scl);

  const ScalaScale& source() const noexcept { return source_; }
  // Degrees per equave (the .scl note count).
  int32_t size() const noexcept { return static_cast<int32_t>(cents_.size()); }
  double equaveCents() const noexcept { return equaveCents_; }
  // Degree pitches within one equave, cents[0] == 0 (the implicit 1/1).
  std::span<const double> cents() const noexcept { return cents_; }

  // Cents of absolute degree `degree` (degree d of equave e is e·size + d). RT-safe.
  double centsOfDegree(int64_t degree) const noexcept;
  // Same for degree `degree` of equave `equave` (a Degree pitch identity); skips the division in the normal case
  // 0 ≤ degree < size(). RT-safe.
  double centsOf(int32_t degree, int32_t equave) const noexcept {
    if (degree >= 0 && degree < size())
      return cents_[static_cast<size_t>(degree)] + static_cast<double>(equave) * equaveCents_;
    return centsOfDegree(static_cast<int64_t>(equave) * size() + degree);
  }

  friend bool operator==(const Scale& a, const Scale& b) { return a.source_ == b.source_; }

private:
  Scale() = default;

  ScalaScale source_;
  std::vector<double> cents_;
  double equaveCents_ = 1200.0;
};

}  // namespace pq::tuning
