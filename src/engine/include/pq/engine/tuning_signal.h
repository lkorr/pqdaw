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
// Not RT-safe (allocates). The table must outlive every image built with it.

#include "pq/core/result.h"
#include "pq/dsp/tuning_table.h"
#include "pq/tuning/tuning.h"

namespace pq::engine {

// Errors (InvalidArgument): a tuning without a linear form (Scale, MODMOS, generator sequence:
// tuning::linearTuningForm), which plays static pitches instead.
Result<dsp::TuningTable> makeTuningTable(const tuning::Tuning& tuning);

}  // namespace pq::engine
