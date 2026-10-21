#pragma once

// Signal-trace line primitives, ported from PQ Meter onto the draw list (SPEC §11: geometric AA, no MSAA).
//
// Sources (PQ Meter's src/ directory; see the reference table in CLAUDE.md):
//   ui/smooth_line.h             kSmoothFeather, smoothAlphaMul, catmullRom, splinePath, smoothPolyline,
//                                smoothPolylineCols, smoothBand, smoothCurve
//   modules/spectrum.cpp 200-325 gradientPolyline, gradientBand, splineSmooth
//   modules/spectrum.cpp 1340-1405 the curve layering (fill, 3-pass bloom, body, bright core) → spectrumCurve
//   modules/oscilloscope.cpp 860-892 the scope's glow underlay + core / envelope band → scopeTrace, scopeEnvelope
//
// The numbers are PQ Meter's: 0.75 px feather, miter clamped at 2.5× (smoothPolyline), bevel past a 120° turn
// with a 2× miter otherwise (smoothPolylineCols), Catmull-Rom subdivision at ≤ 1.5 px chords, ≤ 24 steps
// per segment, ≤ 6000 output points. The geometry each function emits is identical to the original
// (vertex for vertex); only the arithmetic is reorganised where that was cheaper (see lines.cpp).
//
// UI thread only (functions with internal scratch keep it thread_local, growing once to the largest trace).

#include <cstdint>
#include <span>
#include <vector>

#include "pq/ui/color.h"
#include "pq/ui/draw_list.h"

namespace pq::ui {

// Feather width in pixels on each side of a ribbon's core: just over half a pixel, so the edge lands across
// two pixel rows and reads as continuous tone without looking blurred.
inline constexpr float kSmoothFeather = 0.75f;
inline constexpr float kMiterClamp = 2.5f;           // smoothPolyline
inline constexpr float kSplineMaxOutPoints = 6000.0f;  // splinePath output budget
inline constexpr int kSplineMaxStepsPerSegment = 24;

// Alpha scaled by k, truncated and clamped (smooth_line.h smoothAlphaMul; unlike mulAlpha, k may exceed 1).
inline Rgba smoothAlphaMul(Rgba c, float k) noexcept {
  const float a = static_cast<float>(alphaOf(c)) * k;
  const float out = a < 0.0f ? 0.0f : (a > 255.0f ? 255.0f : a);
  return withAlpha(c, static_cast<uint32_t>(out));
}

// Uniform Catmull-Rom through p1..p2 with p0/p3 as tangent anchors, t in [0, 1].
inline Vec2 catmullRom(Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3, float t) noexcept {
  const float t2 = t * t, t3 = t2 * t;
  const float a = -0.5f * t3 + t2 - 0.5f * t;
  const float b = 1.5f * t3 - 2.5f * t2 + 1.0f;
  const float c = -1.5f * t3 + 2.0f * t2 + 0.5f * t;
  const float d = 0.5f * t3 - 0.5f * t2;
  return {p0.x * a + p1.x * b + p2.x * c + p3.x * d, p0.y * a + p1.y * b + p2.y * c + p3.y * d};
}

// Subdivides `in` with Catmull-Rom so no output chord exceeds maxSegPx (≥ 0.25), raised as needed to keep the
// output within kSplineMaxOutPoints. Segments already short enough are copied, not split. `out` is
// overwritten (capacity reused).
void splinePath(std::span<const Vec2> in, std::vector<Vec2>& out, float maxSegPx = 1.5f);

// Feathered ribbon of one colour: an opaque core of `thickness` plus a feather fading to alpha 0 on each side.
// Per-vertex normals are averaged across joins; miters extend by 1/cos, clamped at kMiterClamp.
// 8 vertices / 18 indices per segment.
void smoothPolyline(DrawList& dl, const Vec2* pts, uint32_t n, Rgba col, float thickness,
                    float feather = kSmoothFeather);
inline void smoothPolyline(DrawList& dl, std::span<const Vec2> pts, Rgba col, float thickness,
                           float feather = kSmoothFeather) {
  smoothPolyline(dl, pts.data(), static_cast<uint32_t>(pts.size()), col, thickness, feather);
}

// The same ribbon with a colour per vertex. One shared miter offset per vertex (≤ 2×); a turn sharper than
// 120° gets a bevel instead (each segment ends square, one triangle + feather fills the outside).
void smoothPolylineCols(DrawList& dl, const Vec2* pts, const Rgba* cols, uint32_t n, float thickness,
                        float feather = kSmoothFeather);

// Fill between two same-length edges running left to right (a min/max envelope), feathered above and below.
void smoothBand(DrawList& dl, const Vec2* up, const Vec2* dn, uint32_t n, Rgba col, float feather = kSmoothFeather);

// splinePath then smoothPolyline.
void smoothCurve(DrawList& dl, std::span<const Vec2> pts, Rgba col, float thickness, float maxSegPx = 1.5f,
                 float feather = kSmoothFeather);

// Per-vertex-coloured quad strip, no joins, no feather (spectrum.cpp gradientPolyline): the spectrum's
// stacked passes hide the seams. 4 vertices / 6 indices per segment.
void gradientPolyline(DrawList& dl, const Vec2* pts, const Rgba* cols, uint32_t n, float thickness);

// Vertical fill under a curve, one quad per column: top edge in the curve's colours, bottom edge at yBottom
// with alpha scaled by bottomScale (0 = fade to nothing, 1 = flat).
void gradientBand(DrawList& dl, const Vec2* top, const Rgba* topCol, float yBottom, float bottomScale, uint32_t n);

// Smooths a per-column series through a Catmull-Rom spline over knots every knotStep columns, each knot the
// max of its half-window (biases a peak line onto or above the data). Copies through if n < 2·knotStep.
void splineSmooth(const float* src, float* dst, uint32_t n, int knotStep);

// --- PQ Meter's layered looks --------------------------------------------------------------------------------

// Oscilloscope trace (oscilloscope.cpp, non-envelope path): two faint glow passes under the core (PQ Meter's
// loop runs i = 3..1 and the i = 3 pass has alpha 0, so it is skipped), then the core ribbon at
// max(0.6, lineWidth). `glow` in [0, 1] (default 0.35). The glow passes use `glowBlend`; PQ Meter
// alpha-blends them.
void scopeTrace(DrawList& dl, std::span<const Vec2> pts, Rgba color, float lineWidth, float glow,
                BlendMode glowBlend = BlendMode::Alpha);

// Oscilloscope envelope (min/max band) with the glow around both edges, the band, then both edges stroked.
void scopeEnvelope(DrawList& dl, const Vec2* up, const Vec2* dn, uint32_t n, Rgba color, float lineWidth, float glow,
                   BlendMode glowBlend = BlendMode::Alpha);

struct SpectrumCurveStyle {
  float lineWidth = 1.6f;   // spectrum.h defaults
  float fillAlpha = 0.5f;
  bool gradientFill = true;
  bool glow = true;
  float glowAmount = 1.0f;  // 0..2
  BlendMode glowBlend = BlendMode::Alpha;
};

// Spectrum curve (spectrum.cpp, single-curve path): 1. fill under the curve to yBottom, 2. three bloom passes
// (7×/4×/2.2× width at α .07/.11/.17, weighted by level³), 3. the body at 1.9× width, then a brightened core at
// 0.85×. `cols` already carry the floor fade in their alpha; `levelT` is each vertex's level in [0, 1].
void spectrumCurve(DrawList& dl, const Vec2* poly, const Rgba* cols, const float* levelT, uint32_t n, float yBottom,
                   const SpectrumCurveStyle& style = {});

}  // namespace pq::ui
