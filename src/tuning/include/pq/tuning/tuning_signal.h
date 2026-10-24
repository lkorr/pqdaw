#pragma once

// Tuning as a signal (SPEC §3.3, T2.6): the per-region linear form of a tuning, as plain data for the audio-rate
// tuning node (the `tuning_hz` atom; its kernel reads this through dsp::TuningTableView, pq/dsp/tuning_table.h).
//
// Inside one constant-order interval of the generator axis (MosLinearTable, mos_regions.h) every degree of a MOS is
// linear in the generator g and the period P:
//     cents(D) = (K·g − M·P) · (1200 + stretch) / 1200          (SPEC §3.3: k·g − m·P + e·P, stretched)
// where for the absolute degree D = equave·N + degree (N = n · periodsPerEquave notes per equave), split as
// D = q·N + p·n + j (0 ≤ j < n, 0 ≤ p < periodsPerEquave), with t the interval holding g (boundaries as
// buildMOSScale sees them: MosLinearTable::lookup, the region hint deciding a degenerate generator's side), s its
// period shift and r = rotationOf(t, rootChain) the mode in that interval (T2.6-fix2: the root keeps its chain
// position, as the explorer's transferModeRotation does when the generator crosses into another interval):
//     a = j + r (− n when ≥ n, w = 1 then, else 0)
//     K = k[t][a] − k[t][r],   M = m[t][a] − m[t][r] − w + K·s − p − q·periodsPerEquave
// This reproduces buildFullScale at every generator, boundaries included (up to float rounding), with the mode the
// explorer carries there.
//
// The data splits in two (T2.6-fix4): the SHAPE (kind, n: the interval table, shared by every tuning with that chain
// length) and the DEFINITION (periods per equave, the root's chain position, the region hint, the continuous values).
//
// An EDO is the same form with one interval: n = steps, g = P / n (the generator follows the equave), K = j,
// M = −q, i.e. cents(D) = D · equave / n.
//
// Not representable (Error InvalidArgument; such tunings play the static resolved Hz, QUESTIONS Q100):
//   - MODMOS alterations and generator sequences (the JS rounds L and s to 0.01 ¢, so they are not linear in g);
//   - Scale (Scala) tunings (nothing continuous to modulate but the reference).
//
// Built off the audio thread (allocates; the MOS shape is ~0.3·n² intervals × n × 3 ints).

#include <cstdint>
#include <optional>
#include <vector>

#include "pq/core/result.h"
#include "pq/tuning/mos.h"
#include "pq/tuning/tuning.h"

namespace pq::tuning {

enum class LinearTuningKind : uint8_t { Edo, Mos };

// The interval table of one shape (MosLinearTable's data; an EDO: one interval, k[j] = j, m = 0).
struct LinearShape {
  LinearTuningKind kind = LinearTuningKind::Edo;
  int32_t n = 12;                 // notes per period (EDO: steps per equave)
  std::vector<double> bounds;     // interval boundaries as fractions of the period, 0 … 1 (EDO: {0, 1})
  std::vector<int32_t> k;         // intervals × n, unrotated (MosLinearTable::kTable)
  std::vector<int32_t> m;         // intervals × n, period shift 0
  std::vector<int32_t> inverse;   // intervals × n: sorted degree of each chain element
  std::vector<int8_t> sides;      // per boundary: buildMOSScale's hint-less nudge side (MosLinearTable)
  std::vector<double> windows;    // per boundary: the degenerate window in cents

  int32_t intervalCount() const noexcept { return static_cast<int32_t>(bounds.size()) - 1; }
};

// n ≥ 1. Errors (InvalidArgument): n < 1.
Result<LinearShape> linearShape(LinearTuningKind kind, int32_t n);

// What a tuning adds to its shape.
struct LinearTuningForm {
  LinearTuningKind kind = LinearTuningKind::Edo;
  int32_t n = 12;                 // the shape's n
  int32_t periodsPerEquave = 1;   // EDO: 1
  int32_t rootChain = 0;          // chain position of the mode's root at the definition's generator (EDO: 0)
  std::optional<RegionHint> hint; // the definition's region hint (MOS)
  // The definition's continuous parameters (the tuning node's unmodulated inputs, in double precision).
  double generator = 100.0;       // cents (EDO: equave / n)
  double period = 1200.0;         // cents (EDO: the equave)
  double stretch = 0.0;           // cents added to the 1200-cent octave (EDO: 0)
  double referenceHz = kDefaultReferenceHz;
};

// The linear form of an EDO or MOS tuning (its shape: linearShape(form.kind, form.n)). Errors (InvalidArgument): a
// Scale tuning, MODMOS alterations, a generator sequence.
Result<LinearTuningForm> linearTuningForm(const Tuning& tuning);

}  // namespace pq::tuning
