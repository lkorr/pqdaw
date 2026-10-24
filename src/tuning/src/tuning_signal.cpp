#include "pq/tuning/tuning_signal.h"

#include "pq/tuning/mos_regions.h"

namespace pq::tuning {

Result<LinearShape> linearShape(LinearTuningKind kind, int32_t n) {
  if (n < 1) return Error{ErrorCode::InvalidArgument, "a tuning shape needs at least one note per period", {}};
  LinearShape s;
  s.kind = kind;
  s.n = n;
  const auto nn = static_cast<size_t>(n);
  if (kind == LinearTuningKind::Edo) {
    s.bounds = {0.0, 1.0};
    s.k.resize(nn);
    s.m.assign(nn, 0);
    s.inverse.resize(nn);
    for (int32_t j = 0; j < n; ++j) s.k[static_cast<size_t>(j)] = s.inverse[static_cast<size_t>(j)] = j;
    s.sides.assign(2, 0);
    s.windows.assign(2, 0.0);
    return s;
  }
  const MosLinearTable table(n);
  const auto b = table.boundaries();
  s.bounds.assign(b.begin(), b.end());
  s.k.assign(table.kTable().begin(), table.kTable().end());
  s.m.assign(table.mTable().begin(), table.mTable().end());
  s.inverse.assign(table.inverseTable().begin(), table.inverseTable().end());
  s.sides.assign(table.boundarySides().begin(), table.boundarySides().end());
  s.windows.assign(table.boundaryWindows().begin(), table.boundaryWindows().end());
  return s;
}

Result<LinearTuningForm> linearTuningForm(const Tuning& tuning) {
  auto bad = [](const char* why) { return Error{ErrorCode::InvalidArgument, why, {}}; };
  LinearTuningForm f;
  f.referenceHz = tuning.referenceHz();
  if (const Edo* edo = tuning.edo()) {
    f.kind = LinearTuningKind::Edo;
    f.n = static_cast<int32_t>(edo->n());
    f.periodsPerEquave = 1;
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
  f.hint = p.region;
  // The explorer's oldChain[rotation] (transferModeRotation): the root's position in the generator chain at the
  // definition's (nudged) generator.
  if (p.n > 1) {
    const std::vector<int32_t> chain = generatorChain(p.generator, p.n, p.period, p.region ? &*p.region : nullptr);
    f.rootChain = chain[static_cast<size_t>(p.rotation)];
  }
  return f;
}

}  // namespace pq::tuning
