#pragma once

// Tuning as a signal (SPEC §3.3, T2.6): the per-region linear form of a tuning, as plain data for the audio-rate
// tuning node (the `tuning_hz` atom; its kernel reads this through dsp::TuningTableView, pq/dsp/tuning_table.h).
//
// Inside one constant-order interval of the generator axis (MosLinearTable, mos_regions.h) every degree of a MOS is
// linear in the generator g and the period P:
//     cents(D) = (K·g − M·P) · (1200 + stretch) / 1200          (SPEC §3.3: k·g − m·P + e·P, stretched)
// where for the absolute degree D = equave·N + degree (N = n · periodsPerEquave notes per equave), split as
// D = q·N + p·n + j (0 ≤ j < n, 0 ≤ p < periodsPerEquave):
//     K = kRot[t][j],   M = mRot[t][j] + K·floor(g/P) − p − q·periodsPerEquave
// with t the interval holding g/P − floor(g/P) and kRot/mRot the interval's coefficients rotated to the mode
// (rotateLinearForm with periodShift 0). This reproduces Mos::centsOf / buildFullScale exactly (up to float
// rounding) everywhere except exactly at an interval boundary, where the table uses the interval above and
// buildMOSScale nudges the generator by 1e-7 ¢ to one side (mos_regions.h).
//
// An EDO is the same form with one interval: n = steps, g = P / n (the generator follows the equave), K = j,
// M = −q, i.e. cents(D) = D · equave / n.
//
// Not representable (Error InvalidArgument; such tunings play the static resolved Hz, QUESTIONS Q100):
//   - MODMOS alterations and generator sequences (the JS rounds L and s to 0.01 ¢, so they are not linear in g);
//   - Scale (Scala) tunings (nothing continuous to modulate but the reference).
//
// Built off the audio thread (allocates: ~0.3·n² intervals × n × 2 ints).

#include <cstdint>
#include <vector>

#include "pq/core/result.h"
#include "pq/tuning/tuning.h"

namespace pq::tuning {

enum class LinearTuningKind : uint8_t { Edo, Mos };

struct LinearTuningForm {
  LinearTuningKind kind = LinearTuningKind::Edo;
  int32_t n = 12;                 // notes per period (EDO: steps per equave)
  int32_t periodsPerEquave = 1;   // EDO: 1
  std::vector<double> bounds;     // interval boundaries as fractions of the period, 0 … 1 (EDO: {0, 1})
  std::vector<int32_t> k;         // intervals × n, rotated to the mode
  std::vector<int32_t> m;         // intervals × n, rotated to the mode (periodShift 0)
  // The definition's continuous parameters (the tuning node's unmodulated inputs, in double precision).
  double generator = 100.0;       // cents (EDO: equave / n)
  double period = 1200.0;         // cents (EDO: the equave)
  double stretch = 0.0;           // cents added to the 1200-cent octave (EDO: 0)
  double referenceHz = kDefaultReferenceHz;

  int32_t intervalCount() const noexcept { return static_cast<int32_t>(bounds.size()) - 1; }
};

// The linear form of an EDO or MOS tuning. Errors (InvalidArgument): a Scale tuning, MODMOS alterations, a
// generator sequence.
Result<LinearTuningForm> linearTuningForm(const Tuning& tuning);

}  // namespace pq::tuning
