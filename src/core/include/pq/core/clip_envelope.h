#pragma once

// The static envelope law of an audio clip (pq/engine/audio_sequence.h "Envelope"), shared by the clip player (which
// evaluates it in song samples) and the views (SX.3: the arrangement draws each clip's waveform multiplied by it, in
// ticks), so what is drawn is what plays:
//   fade in   over [bodyStart, inEnd)      automationShape(x, inCurve)        (x = 0 … 1 over the window)
//   fade out  over [outStart, bodyEnd)     1 − automationShape(x, outCurve)
//   crossfade in  over [xinStart, xinEnd)  sin(π/2 · u), or u when xinLinear (equal gain)
//   crossfade out over [xoutStart, xoutEnd) cos(π/2 · u), or 1 − u when xoutLinear
// The product of the windows that contain t; the clip gain multiplies it (not included here). `centre` is where a
// crossfade samples its position inside a time unit: 0.5 for whole song samples (the sample's centre), 0 for a
// continuous position (ticks). Header-only, allocation-free, RT-safe.

#include <cmath>
#include <numbers>

#include "pq/core/automation_curve.h"

namespace pq {

// The clip player's edge declick (AudioBuildOptions::declickSeconds): an edge without a crossfade whose fade is shorter
// gets a linear fade of this length instead.
inline constexpr double kClipEdgeDeclickSeconds = 0.002;

inline double clipFadeInShape(double x, double curve) noexcept { return automationShape(x, curve); }          // RT-safe
inline double clipFadeOutShape(double x, double curve) noexcept { return 1.0 - automationShape(x, curve); }   // RT-safe
inline double clipCrossfadeInShape(double u, bool linear) noexcept {  // RT-safe
  return linear ? u : std::sin(0.5 * std::numbers::pi * u);
}
inline double clipCrossfadeOutShape(double u, bool linear) noexcept {  // RT-safe
  return linear ? 1.0 - u : std::cos(0.5 * std::numbers::pi * u);
}

// The shape at t of an envelope E (any struct with the members bodyStart, bodyEnd, inEnd, inCurve, outStart, outCurve,
// xinStart, xinEnd, xinLinear, xoutStart, xoutEnd, xoutLinear, all positions in one unit T). RT-safe.
template <class E, class T>
inline double clipEnvelopeShape(const E& e, T t, double centre) noexcept {
  double g = 1.0;
  if (t >= e.bodyStart && t < e.inEnd)
    g *= clipFadeInShape(static_cast<double>(t - e.bodyStart) / static_cast<double>(e.inEnd - e.bodyStart), e.inCurve);
  if (t >= e.outStart && t < e.bodyEnd)
    g *= clipFadeOutShape(static_cast<double>(t - e.outStart) / static_cast<double>(e.bodyEnd - e.outStart), e.outCurve);
  if (t >= e.xinStart && t < e.xinEnd)
    g *= clipCrossfadeInShape((static_cast<double>(t - e.xinStart) + centre) / static_cast<double>(e.xinEnd - e.xinStart), e.xinLinear);
  if (t >= e.xoutStart && t < e.xoutEnd)
    g *= clipCrossfadeOutShape((static_cast<double>(t - e.xoutStart) + centre) / static_cast<double>(e.xoutEnd - e.xoutStart),
                               e.xoutLinear);
  return g;
}

}  // namespace pq
