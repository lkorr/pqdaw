#pragma once

// VZ.6 (docs/plans/VZ.md "VZ.6"): two pass-through analysers ported from PQ Meter, each drawing the signal passing
// through it on its face (pq/views/meta band_wave_faces.cpp) and changing nothing:
//   `band_levels`  Band Levels: 3 / 4 / 5 bands (Linkwitz–Riley crossovers, or PQ's independent low / band / high) and
//                  each band's RMS over time (pq/dsp/band_levels.h)
//   `waveform`     Waveform: the scrolling peak + RMS envelope (mid / side or L / R lanes), or a loop locked to the
//                  transport's beat (pq/dsp/waveform.h)
//
//   inputs   in   (the chain input; default 0): any width (mono or stereo), any poly
//   outputs  out  = in, exactly (the expansion wires the input port to the output port: bit-identical, no op)
//   tap      signal_tap(transport.beat, 0, in[0], in[1]) into its SignalTap request 0 (the Spectrogram's layout: the
//            worker reads the beat in channel 0 for the Waveform's loop, L and R in 2 and 3). Unwatched it costs one
//            relaxed load per block; the analysis runs on the feed's worker (pq/views/devices/analysis.h, kWantBands /
//            kWantWave) only while a face shows it.
//
// No params or settings: the options are display state kept in the instance's DATA (a record of numbers; data never
// recompiles): saved with the project, undoable.

#include "pq/meta/type.h"

namespace pq::meta {

const Type& bandLevelsType() noexcept;
const Type& waveformType() noexcept;

}  // namespace pq::meta
