#pragma once

// The Oscilloscope meta module, `oscilloscope` (TX.scope; SPEC §15 2026-11-01 "analysis modules"): a pass-through
// scope. Placed in a chain (a device) or in a circuit, it draws the signal passing through it on its face (PQ Meter's
// oscilloscope in pqdaw's style: pq/views/meta, the analysis pq/dsp/scope.h) and changes nothing.
//
//   inputs   in   (the chain input; default 0): any width (mono or stereo), any poly
//   outputs  out  = in, exactly (the expansion wires the input port to the output port: bit-identical, no op)
//   tap      signal_tap(transport.beat, note, in[0], in[1]) into its SignalTap request 0 (pq/dsp/atoms.h), the
//            history the face reads: the transport's beat position (the Beat timebase: project time modulo a musical
//            span), the pitch of the lowest sounding voice when the module runs per voice (voice_lowest(voice.hz): the
//            note-locked timebase; 0 otherwise), and the signal's L and R (a mono signal fills both; a per-voice
//            signal is summed over the voices, as any voice signal into a global input). Unwatched it costs one
//            relaxed load per block; nothing else runs.
//
// No params or settings: what the face shows (timebase, channels, trigger, gain, persistence …) is display state, kept
// in the instance's DATA (a record; data never recompiles, pq/meta/data.h) so it is saved with the project and undoable.

#include "pq/meta/type.h"

namespace pq::meta {

const Type& oscilloscopeType() noexcept;

}  // namespace pq::meta
