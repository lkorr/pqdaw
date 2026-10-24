#include "pq/engine/tuning_signal.h"

#include "pq/tuning/tuning_signal.h"

namespace pq::engine {

Result<dsp::TuningTable> makeTuningTable(const tuning::Tuning& tuning) {
  Result<tuning::LinearTuningForm> form = tuning::linearTuningForm(tuning);
  if (!form) return std::move(form).error();
  const tuning::LinearTuningForm& f = form.value();
  return dsp::TuningTable::build(f.kind == tuning::LinearTuningKind::Edo ? dsp::TuningTableKind::Edo
                                                                         : dsp::TuningTableKind::Mos,
                                 f.n, f.periodsPerEquave, f.bounds, f.k, f.m, f.generator, f.period, f.stretch,
                                 f.referenceHz);
}

}  // namespace pq::engine
