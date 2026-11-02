#pragma once

// The Spectrogram meta module, `spectrogram` (TX.spectrogram; SPEC §15 2026-11-01 "analysis modules", spectrogram added
// by the user): a pass-through analyser. Placed in a chain (a device) or in a circuit, it draws the signal passing
// through it as a scrolling spectrogram on its face (PQ Meter's reassigned spectrogram in pqdaw's style: the analysis
// pq/dsp/spectrogram.h on the feed's worker, the face pq/views/meta) and changes nothing.
//
//   inputs   in   (the chain input; default 0): any width (mono or stereo), any poly
//   outputs  out  = in, exactly (the expansion wires the input port to the output port: bit-identical, no op)
//   tap      signal_tap(transport.beat, 0, in[0], in[1]) into its SignalTap request 0 (pq/dsp/atoms.h): the transport's
//            beat position (the Loop playhead places columns by it), nothing, and the signal's L and R (a mono signal
//            fills both; a per-voice signal is summed over the voices, as any voice signal into a global input).
//            Unwatched it costs one relaxed load per block; nothing else runs.
//
// No params or settings: what the face shows (window, span, channel, scale, range, colours, gate …) is display state,
// kept in the instance's DATA (a record of numbers; data never recompiles, pq/meta/data.h) so it is saved with the
// project and undoable.

#include "pq/meta/type.h"

namespace pq::meta {

const Type& spectrogramType() noexcept;

}  // namespace pq::meta
