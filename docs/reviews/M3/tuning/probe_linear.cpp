// M3 review probe (tuning): linear form (the tuning node's math, header-only dsp::tuningCents) vs the static
// resolver (Mos::centsOf / buildFullScale). Not product code. Build: see build_probes.ps1.
#include <cmath>
#include <cstdio>
#include <random>

#include "pq/dsp/tuning_table.h"
#include "pq/tuning/mos_regions.h"
#include "pq/tuning/tuning.h"
#include "pq/tuning/tuning_signal.h"

using namespace pq;
using namespace pq::tuning;

static dsp::TuningTableView viewOf(const LinearTuningForm& f) {
  dsp::TuningTableView v;
  v.kind = f.kind == LinearTuningKind::Edo ? dsp::TuningTableKind::Edo : dsp::TuningTableKind::Mos;
  v.n = f.n;
  v.periodsPerEquave = f.periodsPerEquave;
  v.intervals = f.intervalCount();
  v.bounds = f.bounds.data();
  v.k = f.k.data();
  v.m = f.m.data();
  v.generator = f.generator;
  v.period = f.period;
  v.stretch = f.stretch;
  v.referenceHz = f.referenceHz;
  return v;
}

int main() {
  std::mt19937_64 rng(12345);
  std::uniform_real_distribution<double> u(0.0, 1.0);
  const double periods[] = {1200.0, 600.0, 400.0, 1901.955, 240.0, 1800.0};
  double maxErr = 0.0;
  long cases = 0, bad = 0, degenerateSkipped = 0;
  for (int c = 0; c < 20000; ++c) {
    MosParams p;
    p.period = periods[c % 6];
    p.n = 2 + static_cast<int32_t>(u(rng) * 23);
    const double kind = u(rng);
    p.generator = kind < 0.7 ? u(rng) * p.period : kind < 0.85 ? p.period + u(rng) * p.period : -u(rng) * 2 * p.period;
    p.periodsPerEquave = 1 + static_cast<int32_t>(u(rng) * 3);
    p.rotation = static_cast<int32_t>(u(rng) * p.n);
    p.octaveStretch = u(rng) < 0.5 ? 0.0 : (u(rng) - 0.5) * 40.0;
    // skip generators within 1e-6 of a boundary (degenerate: chainSafe nudge territory)
    bool degen = false;
    for (int i = 1; i < p.n; ++i) {
      const double r = std::fmod(std::fmod(i * p.generator, p.period) + p.period, p.period);
      if (r < 1e-5 || r > p.period - 1e-5) degen = true;
    }
    if (degen) { ++degenerateSkipped; continue; }
    auto mos = Mos::make(p);
    if (!mos.ok()) { std::printf("make failed\n"); return 1; }
    auto t = Tuning::make(mos.value(), 261.626);
    auto f = linearTuningForm(t.value());
    if (!f.ok()) { std::printf("form failed\n"); return 1; }
    const dsp::TuningTableView v = viewOf(f.value());
    const int32_t N = p.n * p.periodsPerEquave;
    for (int32_t e = -3; e <= 3; ++e)
      for (int32_t d = -N - 2; d < 2 * N + 2; ++d) {
        const double ref = mos.value().centsOf(d, e);
        const double lin = dsp::tuningCents(v, static_cast<int64_t>(e) * N + d, p.generator, p.period, p.octaveStretch);
        const double err = std::fabs(ref - lin);
        ++cases;
        if (err > 1e-6) {
          if (bad < 10)
            std::printf("MISMATCH g=%.9f n=%d P=%g ppe=%d rot=%d st=%g d=%d e=%d ref=%.9f lin=%.9f\n", p.generator, p.n,
                        p.period, p.periodsPerEquave, p.rotation, p.octaveStretch, d, e, ref, lin);
          ++bad;
        }
        if (err > maxErr) maxErr = err;
      }
  }
  std::printf("random sweep: %ld degree evaluations, %ld mismatches > 1e-6 c, max err %.3g c, %ld degenerate skipped\n",
              cases, bad, maxErr, degenerateSkipped);

  // Boundary probe: 5L2s at its collapsed edge g = 720 (3/5 of 1200, an exact float32 value), with the region hint
  // the explorer would have selected (5L2s: [4/7*1200, 720]).
  {
    MosParams p;
    p.generator = 720.0;
    p.n = 7;
    p.region = RegionHint{7, 4.0 / 7.0 * 1200.0, 720.0};
    auto mos = Mos::make(p);
    auto t = Tuning::make(mos.value(), 261.626);
    auto f = linearTuningForm(t.value());
    const dsp::TuningTableView v = viewOf(f.value());
    std::printf("5L2s @720 with 5L2s hint, degree: static(buildFullScale) vs node(linear form)\n");
    for (int d = 0; d < 7; ++d)
      std::printf("  d=%d static=%.4f node=%.4f diff=%.4f\n", d, mos.value().centsOf(d, 0),
                  dsp::tuningCents(v, d, 720.0, 1200.0, 0.0), dsp::tuningCents(v, d, 720.0, 1200.0, 0.0) - mos.value().centsOf(d, 0));
    // also generator 600 for n=7 (1/2 boundary) and 800 (2/3), 685.714... not exact
    for (double g : {600.0, 800.0, 480.0, 400.0}) {
      MosParams q;
      q.generator = g;
      q.n = 7;
      auto m2 = Mos::make(q);
      auto t2 = Tuning::make(m2.value(), 261.626);
      auto f2 = linearTuningForm(t2.value());
      const dsp::TuningTableView v2 = viewOf(f2.value());
      double mx = 0;
      for (int d = 0; d < 7; ++d) mx = std::fmax(mx, std::fabs(dsp::tuningCents(v2, d, g, 1200.0, 0.0) - m2.value().centsOf(d, 0)));
      std::printf("  n=7 g=%g (no hint): max |node-static| over degrees = %.4f c\n", g, mx);
    }
  }
  return 0;
}
