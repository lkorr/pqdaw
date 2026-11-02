#pragma once

// The Spectrum meta module, `spectrum` (TX.spectrum; SPEC §15 2026-11-01 "analysis modules"): a pass-through analyser.
// Placed in a chain (a device) or in a circuit, it shows the spectrum of the signal passing through it on its face
// (the reusable spectrum display, pq/views/devices/spectrum_plot.h: PQ Meter's look) and changes nothing.
//
//   inputs   in   (the chain input; default 0): any width (mono or stereo), any poly
//   outputs  out  = in, exactly (the expansion wires the input port to the output port: bit-identical, no op)
//   tap      signal_tap(in[0], in[1], in[0], in[1]) into its SignalTap request 0 (pq/dsp/atoms.h): the analysis tap
//            the face reads (a mono signal fills both channels; a per-voice signal is summed over the voices, as any
//            voice signal into a global input). Unwatched it costs one relaxed load per block; nothing else runs.
//
// No params or settings: what the display shows (peak hold, the input overlay) is the face's view state.

#include "pq/meta/type.h"

namespace pq::meta {

const Type& spectrumType() noexcept;

}  // namespace pq::meta
