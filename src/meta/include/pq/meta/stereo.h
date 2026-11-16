#pragma once

// The stereo meta modules (VZ.5; docs/plans/VZ.md "VZ.5"), ported from PQ Meter: three pass-through analysers of the L / R
// relationship, each drawing the signal passing through it on its face (pq/views/meta stereo_faces.cpp) and changing
// nothing:
//   `stereo_field`  Stereo Field: the goniometer (mid up, side across), the correlation bar, three LR4 bands
//   `stereo_image`  Stereo Image: per frequency the pan (R − L) / (R + L) and the width 1 − coherence (ribbon, glow,
//                   heat map)
//   `phase_scope`   Phase Scope: what each frequency gains or loses summed to mono, the coherence ribbon
//
// The same body as the loudness modules (pq/meta/loudness.h): out = in exactly; signal_tap(L, R, L, R) into its
// SignalTap request 0 (a mono signal fills both; a per-voice signal is summed over the voices). The Stereo Field reads
// the feed's raw history (kWantScope); the other two its cross-spectrum (pq/dsp/stereo.h, kWantStereo), computed off
// the audio thread only while a face shows it. Options are the instance's DATA (a record of numbers).

#include "pq/meta/type.h"

namespace pq::meta {

const Type& stereoFieldType() noexcept;
const Type& stereoImageType() noexcept;
const Type& phaseScopeType() noexcept;

}  // namespace pq::meta
