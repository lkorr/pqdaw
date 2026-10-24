#pragma once

// Tuning as a signal (T2.6, SPEC §3.3), engine side: the tuning node's table resource from a tuning.
//
// A track patch derives its voices' pitch from a tuning node:
//     t  = tuning_hz(voice.degree, voice.equave, voice.hz, voice.cents, generator, period, stretch, reference,
//                    table: <index into Resources::tunings>)
//     tv = tune_voices(t.hz, t.cents)     // voice.hz / voice.cents of every subcircuit this patch instantiates
// with generator / period / stretch / reference wired to params (automation) or any modulator. Notes carry their
// Degree identity as voice.degree / voice.equave (NoteEvent::degree / equave, PitchSource in sequence.h); notes
// without one keep their static voice.hz. The table holds the per-interval linear form (pq/tuning/tuning_signal.h)
// and is what Resources::tunings points at (interpreter.h; the JIT reads the same).
//
// The table splits into a shape (the interval table of a chain length, shareable between tunings: T2.6-fix4) and the
// tuning's definition (pq/dsp/tuning_table.h).
//
// Not RT-safe (allocates). A table must outlive every image built with it.

#include <memory>

#include "pq/core/result.h"
#include "pq/dsp/tuning_table.h"
#include "pq/tuning/tuning.h"
#include "pq/tuning/tuning_signal.h"

namespace pq::engine {

// The shape a tuning's table needs: (kind, n). Errors (InvalidArgument): no linear form (see makeTuningTable).
struct TuningShapeKey {
  dsp::TuningTableKind kind = dsp::TuningTableKind::Edo;
  int32_t n = 0;
  friend bool operator==(const TuningShapeKey&, const TuningShapeKey&) = default;
};
Result<TuningShapeKey> tuningShapeKey(const tuning::Tuning& tuning);

// The shape of `key` (~0.3·n² intervals × n × 3 ints for a MOS; one interval for an EDO). Errors: n < 1.
Result<std::shared_ptr<const dsp::TuningShape>> makeTuningShape(const TuningShapeKey& key);

// The table of `tuning` on `shape` (its own shape when null), with optional audio-rate inputs. Errors
// (InvalidArgument): a tuning without a linear form (Scale, MODMOS, generator sequence: tuning::linearTuningForm),
// which plays static pitches instead; a shape of another kind or size.
Result<dsp::TuningTable> makeTuningTable(const tuning::Tuning& tuning,
                                         std::shared_ptr<const dsp::TuningShape> shape = {},
                                         const dsp::TuningLiveInputs* live = nullptr);

}  // namespace pq::engine
