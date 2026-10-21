#pragma once

// The automation curve law (docs/QUESTIONS.md Q39), shared by the document's edit operations (T3.6), the engine's
// automation renderer (T3.16, pq/engine/automation.h) and the views that draw envelopes (T3.15). Moved here from
// pq/document so the engine and the views can use it without depending on each other or on `document`.
//
// A lane's breakpoints are sorted by tick (points on one tick in their stored order). Before the first point the
// envelope holds the first value, after the last point the last value. Between consecutive points a = (ta, va,
// curve c) and b = (tb, vb):
//     v(t) = va + (vb − va) · shape(x, c),   x = (t − ta) / (tb − ta)
//     shape(x, c) = (e^{k·x} − 1) / (e^k − 1),   k = K·c,  K = kAutomationCurveK   (k = 0: x, a straight line)
// c > 0 bends the segment so it starts slowly and finishes fast (convex for a rise); c < 0 the opposite.
// Several points on one tick form a jump: the segment arriving at that tick ends at the first of them, the value
// from that tick on is the last of them.
//
// Why this family: it is closed under cutting. The part [0, x0] of a segment with curvature c is again a segment of
// this family with curvature c·x0, and the part [x0, 1] one with curvature c·(1 − x0), so time edits can cut curved
// segments exactly.
//
// Point types: anything with `tick` (integer ticks), `value` and `curve` members (pq::doc::Breakpoint,
// pq::engine::AutomationPoint). All functions are header-only, allocation-free and noexcept (RT-safe).

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>

namespace pq {

inline constexpr double kAutomationCurveK = 6.0;
// |K·curve| below this is a straight line (shape(x) = x).
inline constexpr double kAutomationLinearK = 1e-9;

// shape(x, K·curve) for x in [0, 1]. RT-safe.
inline double automationShape(double x, double curve) noexcept {
  const double k = kAutomationCurveK * curve;
  if (std::abs(k) < kAutomationLinearK) return x;
  return std::expm1(k * x) / std::expm1(k);
}

// Value of the segment a → b at tick t (ta ≤ t ≤ tb; a zero-length segment gives b's value). RT-safe.
template <class P>
inline double automationSegmentValue(const P& a, const P& b, double t) noexcept {
  if (b.tick <= a.tick) return b.value;
  const double x = std::clamp((t - static_cast<double>(a.tick)) / static_cast<double>(b.tick - a.tick), 0.0, 1.0);
  return a.value + (static_cast<double>(b.value) - a.value) * automationShape(x, a.curve);
}

// The envelope at a (fractional) tick t: v(t) as above, at a jump the last point on that tick. Empty lanes return
// `fallback`. For integer t this is exactly pq::doc::automationValueAt. O(log points). RT-safe.
template <class P>
inline double evalAutomation(std::span<const P> pts, double t, double fallback = 0.0) noexcept {
  if (pts.empty()) return fallback;
  // First point with tick > t.
  const auto it = std::upper_bound(pts.begin(), pts.end(), t,
                                   [](double v, const P& b) noexcept { return v < static_cast<double>(b.tick); });
  if (it == pts.begin()) return pts.front().value;  // before the first point
  const P& a = *(it - 1);                           // last point with tick ≤ t
  if (static_cast<double>(a.tick) == t || it == pts.end()) return a.value;
  return automationSegmentValue(a, *it, t);
}

}  // namespace pq
