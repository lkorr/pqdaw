#include "pq/tuning/tuning_signal.h"

#include "pq/tuning/mos_regions.h"

namespace pq::tuning {

Result<LinearTuningForm> linearTuningForm(const Tuning& tuning) {
  auto bad = [](const char* why) { return Error{ErrorCode::InvalidArgument, why, {}}; };
  LinearTuningForm f;
  f.referenceHz = tuning.referenceHz();
  if (const Edo* edo = tuning.edo()) {
    const auto n = static_cast<int32_t>(edo->n());
    f.kind = LinearTuningKind::Edo;
    f.n = n;
    f.periodsPerEquave = 1;
    f.bounds = {0.0, 1.0};
    f.k.resize(static_cast<size_t>(n));
    f.m.assign(static_cast<size_t>(n), 0);
    for (int32_t j = 0; j < n; ++j) f.k[static_cast<size_t>(j)] = j;
    f.period = edo->equaveCents();
    f.generator = edo->stepCents();
    return f;
  }
  const Mos* mos = tuning.mos();
  if (!mos) return bad("only EDO and MOS tunings have an audio-rate linear form (a Scale plays static pitches)");
  const MosParams& p = mos->params();
  if (hasModmos(p.modmos)) return bad("MODMOS alterations are not linear in the generator (static pitches only)");
  if (!p.genSequence.empty()) return bad("a generator sequence is not linear in one generator (static pitches only)");
  f.kind = LinearTuningKind::Mos;
  f.n = p.n;
  f.periodsPerEquave = p.periodsPerEquave;
  f.generator = p.generator;
  f.period = p.period;
  f.stretch = p.octaveStretch;
  const MosLinearTable table(p.n);
  const std::span<const double> b = table.boundaries();
  f.bounds.assign(b.begin(), b.end());
  const auto intervals = static_cast<size_t>(table.intervalCount());
  const auto n = static_cast<size_t>(p.n);
  f.k.resize(intervals * n);
  f.m.resize(intervals * n);
  for (size_t t = 0; t < intervals; ++t) {
    // The interval's midpoint in [0, P): periodShift 0, so the rotated form carries no shift.
    const double mid = (f.bounds[t] + f.bounds[t + 1]) / 2.0 * p.period;
    const MosLinearSpan span = table.lookup(mid, p.period);
    rotateLinearForm(span, p.rotation, f.k.data() + t * n, f.m.data() + t * n);
  }
  return f;
}

}  // namespace pq::tuning
