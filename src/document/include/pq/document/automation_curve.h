#pragma once

// Automation envelope evaluation (T3.6; provisional curve law, docs/QUESTIONS.md "Q?-T3.6-a").
//
// A lane's breakpoints are sorted by (tick, id). Before the first point the envelope holds the first value, after the
// last point the last value. Between consecutive points a = (ta, va, curve c) and b = (tb, vb):
//     v(t) = va + (vb − va) · shape(x, K·c),   x = (t − ta) / (tb − ta),   K = kAutomationCurveK
//     shape(x, k) = (e^{k·x} − 1) / (e^k − 1)   (k = 0: x, a straight line)
// c > 0 bends the segment so it starts slowly and finishes fast (convex for a rise); c < 0 the opposite.
// Several points on one tick form a jump: the segment arriving at that tick ends at the first of them, the value
// from that tick on is the last of them.
//
// Why this family: it is closed under cutting. The part [0, x0] of a segment with curvature c is again a segment of
// this family with curvature c·x0, and the part [x0, 1] one with curvature c·(1 − x0). So inserting, deleting or
// duplicating time inside a curved segment can add breakpoints that reproduce the envelope exactly (T3.6 time edits,
// shapes, lock-envelope moves). T3.16's evalAutomation must use the same law (move this header to core then).
//
// All functions are allocation-free and noexcept.

#include <algorithm>
#include <cmath>
#include <span>

#include "pq/core/time.h"
#include "pq/document/model.h"

namespace pq::doc {

inline constexpr double kAutomationCurveK = 6.0;

// shape(x, K·curve) for x in [0, 1].
inline double automationShape(double x, double curve) noexcept {
  const double k = kAutomationCurveK * curve;
  if (std::abs(k) < 1e-9) return x;
  return std::expm1(k * x) / std::expm1(k);
}

// Value of the segment a → b at tick t (ta ≤ t ≤ tb; a zero-length segment gives b's value).
inline double automationSegmentValue(const Breakpoint& a, const Breakpoint& b, double t) noexcept {
  if (b.tick <= a.tick) return b.value;
  const double x = std::clamp((t - static_cast<double>(a.tick)) / static_cast<double>(b.tick - a.tick), 0.0, 1.0);
  return a.value + (static_cast<double>(b.value) - a.value) * automationShape(x, a.curve);
}

// v(t−): the value the envelope approaches from the left at t (at a jump: the first point on that tick).
// Empty lanes return `fallback`.
double automationValueBefore(std::span<const Breakpoint> pts, Ticks t, double fallback = 0.0) noexcept;
// v(t): the value from t on (at a jump: the last point on that tick).
double automationValueAt(std::span<const Breakpoint> pts, Ticks t, double fallback = 0.0) noexcept;

}  // namespace pq::doc
