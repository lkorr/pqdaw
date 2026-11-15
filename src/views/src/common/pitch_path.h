#pragma once

// bug-curve-render: a moving note's pitch path drawn cleanly at any zoom — the piano roll's notes and the arrangement /
// lane-window clip previews share these. A cached path (points over ticks, flattened once per clip snapshot) is mapped
// to device px every paint and simplified THERE, so what is drawn depends on the zoom only through the pixels: a
// zoomed-out path keeps its shape with a handful of vertices instead of thousands of sub-pixel ones (whose sharp
// reversals the stroke's miters turned into spikes and combs), and a vibrato whose cycles are shorter than a few px is
// drawn as its envelope (a band around the rest of the curve) instead of aliasing into a zig-zag. The strokes are the
// ui module's anti-aliased primitives (pq/ui/lines.h). UI thread only.

#include <algorithm>
#include <cmath>
#include <span>
#include <vector>

#include "pq/core/automation_curve.h"
#include "pq/document/model.h"
#include "pq/document/note_bend.h"
#include "pq/ui/draw_list.h"
#include "pq/ui/lines.h"

namespace pq::views {

// How far (vertically, device px) a simplified path may stray from the points it was given. Under a quarter pixel:
// invisible next to the stroke's 0.75 px feather.
inline constexpr float kPathPxTolerance = 0.2f;

// A vibrato drawn as its band when one cycle is shorter than this many logical px (or than two strokes and their
// gaps): below it the up and down strokes of the wave run into each other.
inline constexpr float kVibratoBandPx = 8.0f;
// The band's fill: this fraction of the note's alpha (about the share of the band a fast wave's stroke covers), so the
// note's weight does not jump where the wave turns into the band.
inline constexpr float kVibratoBandAlpha = 0.45f;

// Streaming polyline simplification in device px ("swinging door"): points arrive left to right (x never decreasing)
// and only those the line from the last kept point cannot skip are kept — every skipped point lies within `tol` px
// (vertically) of the output, so flat runs and straight stretches collapse to their ends and sub-pixel detail to its
// silhouette. A jump (two points on one x) stays a vertical step. Output overwrites `out` (capacity reused).
class PxPolyline {
public:
  explicit PxPolyline(std::vector<ui::Vec2>& out, float tol = kPathPxTolerance) noexcept : out_(out), tol_(tol) { out_.clear(); }
  void add(ui::Vec2 p) {
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
  void restart(ui::Vec2 p) {
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
  void keep(ui::Vec2 p) {
    out_.push_back(p);
    a_ = p;
    pending_ = false;
  }
  // The first point after the anchor: the slopes from the anchor that pass within tol of it.
  void start(ui::Vec2 p) {
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
  std::vector<ui::Vec2>& out_;
  float tol_;
  ui::Vec2 a_{}, last_{};
  float lo_ = 0.0f, hi_ = 0.0f;
  bool pending_ = false;
};

// A pitch path's stroke: ui::smoothPolylineCols in one colour — its joins bevel past a 120° turn (and miter at most
// 2×) where smoothPolyline's 2.5× miters stick out of a zoomed-out wave's or a glide's sharp corners as spikes.
inline void strokePath(ui::DrawList& dl, std::span<const ui::Vec2> pts, ui::Rgba col, float thickness) {
  if (pts.size() < 2) return;
  static thread_local std::vector<ui::Rgba> cols;
  cols.assign(pts.size(), col);
  ui::smoothPolylineCols(dl, pts.data(), cols.data(), static_cast<uint32_t>(pts.size()), thickness);
}

// A paint walks a note's path in whole chunks of kPathChunkPx device px counted from the note's start, restarting the
// simplification at each chunk's start (PxPolyline::restart): the vertices in a chunk depend only on the chunk, so a
// partial repaint draws exactly what a full one does, and a long note zoomed in is walked only near the view.
inline constexpr float kPathChunkPx = 256.0f;
// The chunks' ticks: a whole number of ticks ≥ 1 per chunk, chunk k ≥ 1 starting half a tick past k · chunk, so a chunk
// never starts on a breakpoint (on a jump's tick). 0: one chunk (the whole note).
inline double pathChunkTicks(double pxPerTick, float scale) noexcept {
  return pxPerTick > 0.0 ? std::max(1.0, std::round(static_cast<double>(kPathChunkPx * scale) / pxPerTick)) : 0.0;
}
// The chunks covering [t0, t1] (ticks from the note's start) of a note `len` ticks long, as [ta, tb].
inline void chunkSpan(double t0, double t1, double len, double chunk, double& ta, double& tb) noexcept {
  if (!(chunk > 0.0)) {
    ta = 0.0;
    tb = len;
    return;
  }
  const double k0 = std::floor((t0 - 0.5) / chunk), k1 = std::max(1.0, std::ceil((t1 - 0.5) / chunk));
  ta = std::clamp(k0 >= 1.0 ? k0 * chunk + 0.5 : 0.0, 0.0, len);
  tb = std::clamp(k1 * chunk + 0.5, ta, len);
}

// Where to sample a band's edges over [t0, t1] (ticks): at the multiples of `step` inside the span, at each of
// `knots` inside it (where the band bends: the vibrato envelope's corners) and at both ends, ascending. Overwrites
// `out`.
inline void bandTicks(double t0, double t1, double step, std::span<const double> knots, std::vector<double>& out) {
  out.clear();
  if (!(t1 > t0)) return;
  step = std::max(step, (t1 - t0) / 16384.0);
  out.push_back(t0);
  size_t k = 0;
  while (k < knots.size() && knots[k] <= t0) ++k;
  for (double g = (std::floor(t0 / step) + 1.0) * step;; g += step) {
    const double t = std::min(g, t1);
    for (; k < knots.size() && knots[k] < t; ++k)
      if (knots[k] > out.back()) out.push_back(knots[k]);
    if (t > out.back()) out.push_back(t);
    if (g >= t1) break;
  }
}

// Adds the stretch [ta, tb] (ticks from the note's start; chunkSpan's) of a cached path to `line`, mapped to device px
// by x(tick) and y(value): its value at both ends and its points between (straight segments), or — `curved`, a stored
// free curve with bowed segments — samples at the multiples of `stepTicks` along the curve law; restarted at every
// multiple of `chunkTicks` (≤ 0: none). An empty path is straight at y(0).
template <class X, class Y>
void addPathSpan(PxPolyline& line, std::span<const doc::Breakpoint> curve, bool curved, double ta, double tb, double stepTicks,
                 double chunkTicks, X&& x, Y&& y) {
  auto at = [&](double t) { return ui::Vec2{x(t), y(curve.empty() ? 0.0 : pq::evalAutomation(curve, t))}; };
  double next = chunkTicks > 0.0 ? std::max(1.0, std::floor((ta - 0.5) / chunkTicks) + 1.0) * chunkTicks + 0.5 : tb;
  // The chunk starts before tick t (exclusive), each restarting the walk.
  auto boundaries = [&](double t) {
    for (; next < t && next < tb; next += chunkTicks) line.restart(at(next));
  };
  line.add(at(ta));
  if (curve.empty()) {
  } else if (curved) {
    stepTicks = std::max(stepTicks, (tb - ta) / 8192.0);
    for (double g = (std::floor(ta / stepTicks) + 1.0) * stepTicks; g < tb; g += stepTicks) {
      boundaries(g);
      line.add(at(g));
    }
  } else {
    auto it = std::upper_bound(curve.begin(), curve.end(), ta, [](double t, const doc::Breakpoint& b) { return t < static_cast<double>(b.tick); });
    for (; it != curve.end() && static_cast<double>(it->tick) < tb; ++it) {
      const double t = static_cast<double>(it->tick);
      boundaries(t);
      line.add({x(t), y(static_cast<double>(it->value))});
    }
  }
  boundaries(tb);
  line.restart(at(tb));
}

// The edges of a vibrato's band over [ta, tb]: the rest of the curve (`rest`, values; empty: 0) ± the envelope `vib`,
// widened by `halfThick` px each way, sampled at the multiples of `stepTicks` and the envelope's corners. Overwrites
// up / dn (same x per index, for ui::smoothBand); `ticks` is scratch.
template <class X, class Y>
void bandSpan(std::vector<ui::Vec2>& up, std::vector<ui::Vec2>& dn, std::vector<double>& ticks, std::span<const doc::Breakpoint> rest,
              const doc::VibratoEnvelope& vib, double ta, double tb, double stepTicks, float halfThick, X&& x, Y&& y) {
  double corners[5];
  const int nc = vib.corners(corners);
  bandTicks(ta, tb, stepTicks, std::span<const double>(corners, static_cast<size_t>(nc)), ticks);
  up.clear();
  dn.clear();
  for (const double t : ticks) {
    const double r = rest.empty() ? 0.0 : pq::evalAutomation(rest, t), a = vib.at(t);
    const float px = x(t), ya = y(r + a), yb = y(r - a);
    up.push_back({px, std::min(ya, yb) - halfThick});
    dn.push_back({px, std::max(ya, yb) + halfThick});
  }
}

}  // namespace pq::views
