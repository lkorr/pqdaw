#pragma once

// The 3D analysers (VZ.7; docs/plans/VZ.md "VZ.7"), ported from PQ Meter, each drawing the signal passing through it on its
// face and changing nothing (the loudness modules' body, src/analysers.cpp: out = in; signal_tap(L, R, L, R)):
//   `waterfall`           Waterfall: frequency x level x time as a lit terrain (pq/dsp/waterfall.h, kWantWaterfall)
//   `stereo_spectrogram`  Stereo Spectrogram: time x frequency x pan, the reassigned stereo point cloud or the ribbon
//                         (pq/dsp/stereo_cloud.h, kWantCloud)
// Options (analysis, camera, look) are the instance's DATA (a record of numbers).

#include "pq/meta/type.h"

namespace pq::meta {

const Type& waterfallType() noexcept;
const Type& stereoSpectrogramType() noexcept;

}  // namespace pq::meta
