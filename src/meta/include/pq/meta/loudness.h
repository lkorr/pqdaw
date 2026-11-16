#pragma once

// The loudness meta modules (VZ.4; docs/plans/VZ.md "VZ.4"): three pass-through analysers over one front end
// (pq/dsp/loudness.h, ported from PQ Meter), each drawing the signal passing through it on its face (pq/views/meta
// loudness_faces.cpp) and changing nothing:
//   `level_meter`       Level Meter: L / R RMS bars, the peak cap, the hold tick, the latched clip lamps (sample peak)
//   `lufs_meter`        LUFS Meter: momentary / short-term / integrated LUFS, LRA, true peak, correlation, maxima, target
//   `loudness_history`  Loudness History: scrolling M / S / I traces against the target and its tolerance band
//
//   inputs   in   (the chain input; default 0): any width (mono or stereo), any poly
//   outputs  out  = in, exactly (the expansion wires the input port to the output port: bit-identical, no op)
//   tap      signal_tap(in[0], in[1], in[0], in[1]) into its SignalTap request 0 (pq/dsp/atoms.h), the Spectrum's layout:
//            the analysis worker reads channels 2 and 3 (L, R; a mono signal fills both, a per-voice signal is summed
//            over the voices). Unwatched it costs one relaxed load per block; the loudness is computed off the audio
//            thread by the feed's worker (pq/views/devices/analysis.h, kWantLoudness) only while a face shows it.
//
// No params or settings: the options (windows, ballistics, target, auto-reset, scale, …) are display state kept in the
// instance's DATA (a record of numbers; data never recompiles, pq/meta/data.h): saved with the project, undoable.

#include "pq/meta/type.h"

namespace pq::meta {

const Type& levelMeterType() noexcept;
const Type& lufsMeterType() noexcept;
const Type& loudnessHistoryType() noexcept;

}  // namespace pq::meta
