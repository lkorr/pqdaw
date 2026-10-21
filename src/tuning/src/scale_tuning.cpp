#include "pq/tuning/scale_tuning.h"

#include <cmath>

namespace pq::tuning {

Result<Scale> Scale::make(ScalaScale scl) {
  auto bad = [](const char* why) { return Error{ErrorCode::InvalidArgument, why, {}}; };
  if (scl.pitches.empty()) return bad("a scale needs at least one pitch (its equave)");
  for (const ScalaPitch& p : scl.pitches)
    if (!std::isfinite(p.cents)) return bad("scale pitches must be finite");

  Scale s;
  s.cents_.reserve(scl.pitches.size());
  s.cents_.push_back(0.0);
  for (size_t i = 0; i + 1 < scl.pitches.size(); ++i) s.cents_.push_back(scl.pitches[i].cents);
  s.equaveCents_ = scl.pitches.back().cents;
  s.source_ = std::move(scl);
  return s;
}

double Scale::centsOfDegree(int64_t degree) const noexcept {  // RT-safe
  const int64_t n = static_cast<int64_t>(cents_.size());
  int64_t q = degree / n;
  int64_t r = degree % n;
  if (r < 0) {
    r += n;
    --q;
  }
  return cents_[static_cast<size_t>(r)] + static_cast<double>(q) * equaveCents_;
}

}  // namespace pq::tuning
