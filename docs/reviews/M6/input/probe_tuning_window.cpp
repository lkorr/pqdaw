// M6 review (TU.1 known limit): "a main tuning's MOS-shape change writes the new params at once, the new table arrives
// with the compile: for that window a sounding Degree note is computed from the OLD table at the NEW generator".
// Evaluates the tuning node's own formula (dsp::tuningCents over the node table view) for old table @ new params vs
// new table @ new params vs what was sounding, for the structural edits a user makes on a main tuning.
//   pwsh docs/reviews/M6/input/build_probes.ps1 probe_tuning_window
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

#include "pq/document/model.h"
#include "pq/document/pitch_resolve.h"
#include "pq/dsp/tuning_table.h"
#include "pq/tuning/tuning_signal.h"

using namespace pq;

struct Table {
  tuning::LinearShape shape;
  tuning::LinearTuningForm form;
  dsp::TuningTableView v;
};

static std::unique_ptr<Table> tableOf(const doc::TuningDef& d) {
  auto t = doc::buildTuning(d);
  if (!t) return nullptr;
  auto f = tuning::linearTuningForm(*t);
  if (!f) return nullptr;
  auto out = std::make_unique<Table>();
  out->form = *f;
  out->shape = *tuning::linearShape(f->kind, f->n);
  dsp::TuningTableView& v = out->v;
  v.kind = f->kind == tuning::LinearTuningKind::Edo ? dsp::TuningTableKind::Edo : dsp::TuningTableKind::Mos;
  v.n = f->n;
  v.periodsPerEquave = f->periodsPerEquave;
  v.intervals = out->shape.intervalCount();
  v.bounds = out->shape.bounds.data();
  v.k = out->shape.k.data();
  v.m = out->shape.m.data();
  v.inv = out->shape.inverse.data();
  v.sides = out->shape.sides.data();
  v.windows = out->shape.windows.data();
  v.rootChain = f->rootChain;
  if (f->hint) {
    v.hasHint = true;
    v.hint = dsp::TuningHint{f->hint->perPeriodSize, f->hint->lowCents, f->hint->highCents};
  }
  v.generator = f->generator;
  v.period = f->period;
  return out;
}

static doc::TuningDef mos(int n, double g, double P = 1200.0) {
  doc::TuningDef d;
  tuning::MosParams m;
  m.n = n;
  m.generator = g;
  m.period = P;
  d.system = m;
  return d;
}
static doc::TuningDef edo(uint32_t n) {
  doc::TuningDef d;
  d.system = tuning::Edo::make(n).value();
  return d;
}

static void scenario(const char* name, const doc::TuningDef& a, const doc::TuningDef& b) {
  auto A = tableOf(a), B = tableOf(b);
  if (!A || !B) {
    std::printf("%-44s (no linear form)\n", name);
    return;
  }
  // The node's live inputs after applyParams(new def): generator and period of B (an EDO has no generator param:
  // it keeps the old one).
  const double g = B->form.kind == tuning::LinearTuningKind::Mos ? B->form.generator : A->form.generator;
  const double P = B->form.period;
  double maxWrong = 0.0, maxStale = 0.0;
  int third = 0;
  for (int64_t D = 0; D <= 2 * static_cast<int64_t>(B->form.n); ++D) {
    const double before = dsp::tuningCents(A->v, D, A->form.generator, A->form.period, 0.0);
    const double window = dsp::tuningCents(A->v, D, g, P, 0.0);
    const double target = dsp::tuningCents(B->v, D, B->form.generator, B->form.period, 0.0);
    maxWrong = std::max(maxWrong, std::fabs(window - target));
    maxStale = std::max(maxStale, std::fabs(window - before));
    if (std::fabs(window - before) > 0.01 && std::fabs(window - target) > 0.01) ++third;
  }
  std::printf("%-44s window vs target: max %7.1f c | window vs before: max %7.1f c | degrees at a THIRD pitch: %d\n", name,
              maxWrong, maxStale, third);
}

int main() {
  std::printf("Degrees 0 .. 2n; 'window' = old table at the new params (the node between applyParams and the compile).\n");
  scenario("MOS size 7 -> 12, generator kept (700)", mos(7, 700), mos(12, 700));
  scenario("MOS size 7 -> 5, generator kept (700)", mos(7, 700), mos(5, 700));
  scenario("MOS 7@700 -> 5@710 (chart click, another size)", mos(7, 700), mos(5, 710));
  scenario("MOS 7@696.6 -> 12@696.6 (meantone 12)", mos(7, 696.6), mos(12, 696.6));
  scenario("MOS 7@700 -> 9@260 (temperament pick)", mos(7, 700), mos(9, 260));
  scenario("MOS 7@700 -> 7@700 period 1200->600", mos(7, 700), mos(7, 700, 600));
  scenario("EDO 12 -> MOS 7@700", edo(12), mos(7, 700));
  scenario("MOS 7@700 -> EDO 31", mos(7, 700), edo(31));
  scenario("EDO 12 -> EDO 15 (Steps drag)", edo(12), edo(15));
  return 0;
}
