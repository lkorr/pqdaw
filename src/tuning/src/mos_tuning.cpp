#include "pq/tuning/mos_tuning.h"

#include <algorithm>
#include <cmath>

namespace pq::tuning {

Result<Mos> Mos::make(MosParams p) {
  auto bad = [](const char* why) { return Error{ErrorCode::InvalidArgument, why, {}}; };
  auto allFinite = [](const std::vector<double>& v) {
    return std::all_of(v.begin(), v.end(), [](double x) { return std::isfinite(x); });
  };
  if (!std::isfinite(p.generator)) return bad("MOS generator must be finite");
  if (!std::isfinite(p.period) || !(p.period > 0.0)) return bad("MOS period must be a finite positive number of cents");
  if (p.n < 1) return bad("MOS needs at least one note per period");
  if (p.periodsPerEquave < 1) return bad("MOS needs at least one period per equave");
  if (p.rotation < 0 || p.rotation >= p.n) return bad("MOS mode rotation must be in [0, n)");
  if (!std::isfinite(p.octaveStretch) || !(p.octaveStretch > -1200.0))
    return bad("octave stretch must be finite and greater than -1200 cents");
  if (!allFinite(p.modmos) || !allFinite(p.genSequence)) return bad("MODMOS and generator sequence must be finite");

  Mos m;
  FullScale full = buildFullScale(p);
  m.pitches_ = std::move(full.pitches);
  m.equaveCents_ = full.equaveCents;
  m.params_ = std::move(p);
  return m;
}

double Mos::centsOfDegree(int64_t degree) const noexcept {  // RT-safe
  const int64_t n = static_cast<int64_t>(pitches_.size());
  int64_t q = degree / n;
  int64_t r = degree % n;
  if (r < 0) {
    r += n;
    --q;
  }
  const double pure = pitches_[static_cast<size_t>(r)] + static_cast<double>(q) * equaveCents_;
  // stretchCents: pure · (1200 + stretch) / 1200, in the JS operation order.
  return pure * stretchedOctave(params_.octaveStretch) / 1200.0;
}

}  // namespace pq::tuning
