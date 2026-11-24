#pragma once

// Signal-trace line primitives, ported from PQ Meter onto the draw list (SPEC §11: geometric AA, no MSAA).
//
// Sources (PQ Meter's src/ directory; see the reference table in CLAUDE.md):
//   ui/smooth_line.h             kSmoothFeather, smoothAlphaMul, catmullRom, splinePath, smoothPolyline,
//                                smoothPolylineCols, smoothBand, smoothCurve
//   modules/spectrum.cpp 200-325 gradientPolyline, gradientBand, splineSmooth
//   modules/spectrum.cpp 1450-1515 the curve layering (fill, one glow pass, the core; 1.0.21) → spectrumCurve
//   modules/oscilloscope.cpp 860-892 the scope's glow underlay + core / envelope band → scopeTrace, scopeEnvelope
//
// The numbers are PQ Meter's: 0.75 px feather, miter clamped at 2.5× (smoothPolyline), bevel past a 120° turn
// with a 2× miter otherwise (smoothPolylineCols), Catmull-Rom subdivision at ≤ 1.5 px chords, ≤ 24 steps
// per segment, ≤ 6000 output points. Verified against PQ Meter's own code by tests/ui/test_pqmeter_diff.cpp
// (T0.9-fix1): identical vertex/index counts, index buffers, colours and uvs; positions equal up to float
// rounding (≤ 1/256 px asserted, ≤ 2e-3 px measured), because the port multiplies by reciprocals where PQ
// Meter divides (see lines.cpp); the scope's alpha-0 glow pass is skipped (see scopeTrace).
//
// UI thread only (functions with internal scratch keep it thread_local, growing once to the largest trace).

#include <algorithm>
#include <cmath>
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

// Per-vertex-coloured antialiased stroke (spectrum.cpp gradientPolyline, 1.0.21): shared vertices on the mitred
// normal (mitre capped at 2x the half-width), a solid core thickness − 1 px wide and a 1 px fringe to alpha 0 on
// each side. 4 vertices per point, 18 indices per segment.
void gradientPolyline(DrawList& dl, const Vec2* pts, const Rgba* cols, uint32_t n, float thickness);

// Vertical fill under a curve, one quad per column: top edge in the curve's colours, bottom edge at yBottom
// with alpha scaled by bottomScale (0 = fade to nothing, 1 = flat).
void gradientBand(DrawList& dl, const Vec2* top, const Rgba* topCol, float yBottom, float bottomScale, uint32_t n);

// Smooths a per-column series through a Catmull-Rom spline over knots every knotStep columns, each knot the
// max of its half-window (biases a peak line onto or above the data). Copies through if n < 2·knotStep.
void splineSmooth(const float* src, float* dst, uint32_t n, int knotStep);

// --- WF.2: what every curve display draws with ------------------------------------------------------------------
// One antialiased look for every curve in the DAW (the ribbons above; a dense series: pq/ui/waveform.h's columns, the
// sample waveforms' renderer). A display computes its points — at least one per device px of its width
// (curveSamples), only when its input changes — and hands them to one of these (a face: FacePainter::polyline /
// series, which map world units and call them).

// A stroke of one colour that is clean at corners too: smoothPolyline, or — where the path turns by more than 60°
// somewhere (a square wave's edges, an envelope's corners, a zoomed-out wave's tips) — smoothPolylineCols in that one
// colour, whose joins are mitred on the two segments' own normals (≤ 2×) and bevel past 120°. (smoothPolyline's joins
// take their normal from the neighbours' chord: at a hard corner between a long and a short segment the short one
// pinches and the corner is notched, and a near-reversal's miter sticks out as a spike.)
void strokePolyline(DrawList& dl, std::span<const Vec2> pts, Rgba col, float thickness);

// The samples a curve `widthPx` device px wide is evaluated at: one per device px, within [lo, hi] (hi + 1 points).
inline uint32_t curveSamples(float widthPx, uint32_t lo = 16, uint32_t hi = 4096) noexcept {
  const float w = widthPx > 0.0f ? widthPx : 0.0f;
  const auto n = static_cast<uint32_t>(w < 1.0e6f ? w + 0.999f : 1.0e6f);
  return n < lo ? lo : (n > hi ? hi : n);
}

// Where a curve over [0, 1] is evaluated: n equal steps and every knot inside (0, 1) — where the curve has a corner (a
// breakpoint, a knee). A corner between two samples is cut off, and a handle drawn at the exact point then sits off
// the line. Ascending, each value once; overwrites `out`.
void curveParams(std::vector<double>& out, uint32_t n, std::span<const double> knots);

// A sampled series left to right (device px, x never decreasing: a history, a response over frequency, a scope's
// window), whatever its density:
//   under a point per 1.5 px         a Catmull-Rom spline through the points (splinePath, chords ≤ 1 px), stroked
//   denser, and smooth               the points a line within 0.2 px of them needs (PxPolyline), stroked: a response
//                                    curve of 768 points is a ribbon of a few dozen vertices
//   denser, and not (more than 3     one antialiased column per device px covering the series' range inside it
//   vertices per 4 px are left)      (drawWaveformColumns: a stroke where the series passes through, a body where it
//                                    is denser than the pixels) — nothing overlaps itself (a translucent series keeps
//                                    one alpha) and the cost follows the width, not the point count
// `thickness` ≥ 1 device px (thinner: lower the alpha, as FacePainter::series does).
void strokeSeries(DrawList& dl, std::span<const Vec2> pts, Rgba col, float thickness);

// Streaming polyline simplification in device px ("swinging door"): points arrive left to right (x never decreasing)
// and only those the line from the last kept point cannot skip are kept — every skipped point lies within `tol` px
// (vertically) of the output, so flat runs and straight stretches collapse to their ends and sub-pixel detail to its
// silhouette. A jump (two points on one x) stays a vertical step. Output overwrites `out` (capacity reused).
// (From views' pitch paths, bug-curve-render; WF.2: here, for every curve that is mostly straight — an EQ's response.)
inline constexpr float kPathPxTolerance = 0.2f;  // under a quarter pixel: invisible next to the 0.75 px feather
class PxPolyline {
public:
  explicit PxPolyline(std::vector<Vec2>& out, float tol = kPathPxTolerance) noexcept : out_(out), tol_(tol) { out_.clear(); }
  void add(Vec2 p) {
    if (out_.empty()) {
      keep(p);
      return;
    }
    if (!pending_) {
      start(p);
      return;
    }
    const float dx = p.x - a_.x;
    if (dx > kSameX) {
      const float s = (p.y - a_.y) / dx;
      if (s >= lo_ && s <= hi_) {  // the line from the anchor to p passes every skipped point: p may be skipped later
        lo_ = std::max(lo_, (p.y - tol_ - a_.y) / dx);
        hi_ = std::min(hi_, (p.y + tol_ - a_.y) / dx);
        last_ = p;
        return;
      }
    }
    keep(last_);  // p cannot be reached past the last point: that one is kept and becomes the anchor
    start(p);
  }
  // Keeps the last point (the path's end).
  void finish() {
    if (pending_) keep(last_);
  }
  // Adds p and keeps it as a vertex: the walk restarts there, so what follows does not depend on what came before (a
  // paint that starts its walk at p draws the same vertices after it as one that passed through it). (p dropped as a
  // repeat on the anchor's x: the anchor moves onto p.)
  void restart(Vec2 p) {
    add(p);
    if (pending_) {
      keep(last_);  // (= p)
    } else if (out_.back().x != p.x || out_.back().y != p.y) {
      out_.back() = p;
      a_ = p;
    }
  }

private:
  static constexpr float kSameX = 1e-3f;
  void keep(Vec2 p) {
    out_.push_back(p);
    a_ = p;
    pending_ = false;
  }
  // The first point after the anchor: the slopes from the anchor that pass within tol of it.
  void start(Vec2 p) {
    const float dx = p.x - a_.x;
    if (dx <= kSameX) {  // on the anchor's x: a jump is kept as a vertical step, a repeat is dropped
      if (std::fabs(p.y - a_.y) > tol_) keep(p);
      return;
    }
    lo_ = (p.y - tol_ - a_.y) / dx;
    hi_ = (p.y + tol_ - a_.y) / dx;
    last_ = p;
    pending_ = true;
  }
  std::vector<Vec2>& out_;
  float tol_;
  Vec2 a_{}, last_{};
  float lo_ = 0.0f, hi_ = 0.0f;
  bool pending_ = false;
};

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
  bool glow = false;        // PQ Meter 1.0.17: off by default
  float glowAmount = 1.0f;  // 0..2
  BlendMode glowBlend = BlendMode::Alpha;
};

// Spectrum curve (spectrum.cpp 1.0.21, single-curve path): 1. fill under the curve to yBottom, 2. one glow pass
// (2.2× width + 1 px, α 0.2·glowAmount weighted by level³), 3. the brightened core at lineWidth. `cols` already carry the floor fade in their alpha; `levelT` is each vertex's level in [0, 1].
void spectrumCurve(DrawList& dl, const Vec2* poly, const Rgba* cols, const float* levelT, uint32_t n, float yBottom,
                   const SpectrumCurveStyle& style = {});

}  // namespace pq::ui
