#include "pq/engine/tuning_signal.h"

namespace pq::engine {

namespace {

dsp::TuningTableKind kindOf(tuning::LinearTuningKind k) {
  return k == tuning::LinearTuningKind::Edo ? dsp::TuningTableKind::Edo : dsp::TuningTableKind::Mos;
}

}  // namespace

Result<TuningShapeKey> tuningShapeKey(const tuning::Tuning& tuning) {
  Result<tuning::LinearTuningForm> form = tuning::linearTuningForm(tuning);
  if (!form) return std::move(form).error();
  return TuningShapeKey{kindOf(form.value().kind), form.value().n};
}

Result<std::shared_ptr<const dsp::TuningShape>> makeTuningShape(const TuningShapeKey& key) {
  Result<tuning::LinearShape> s = tuning::linearShape(
      key.kind == dsp::TuningTableKind::Edo ? tuning::LinearTuningKind::Edo : tuning::LinearTuningKind::Mos, key.n);
  if (!s) return std::move(s).error();
  const tuning::LinearShape& l = s.value();
  return dsp::TuningShape::build(key.kind, l.n, l.bounds, l.k, l.m, l.inverse, l.sides, l.windows);
}

Result<dsp::TuningTable> makeTuningTable(const tuning::Tuning& tuning, std::shared_ptr<const dsp::TuningShape> shape,
                                         const dsp::TuningLiveInputs* live) {
  Result<tuning::LinearTuningForm> form = tuning::linearTuningForm(tuning);
  if (!form) return std::move(form).error();
  const tuning::LinearTuningForm& f = form.value();
  const TuningShapeKey key{kindOf(f.kind), f.n};
  if (!shape) {
    auto made = makeTuningShape(key);
    if (!made) return std::move(made).error();
    shape = std::move(made.value());
  } else if (shape->kind() != key.kind || shape->n() != key.n) {
    return Error{ErrorCode::InvalidArgument, "tuning table: the shape does not match the tuning", {}};
  }
  dsp::TuningDefinition d;
  d.periodsPerEquave = f.periodsPerEquave;
  d.rootChain = f.rootChain;
  if (f.hint) {
    d.hasHint = true;
    d.hint = dsp::TuningHint{f.hint->perPeriodSize, f.hint->lowCents, f.hint->highCents};
  }
  d.generator = f.generator;
  d.period = f.period;
  d.stretch = f.stretch;
  d.referenceHz = f.referenceHz;
  return dsp::TuningTable::build(std::move(shape), d, live);
}

}  // namespace pq::engine
