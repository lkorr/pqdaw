#pragma once

// Automation envelope evaluation for the document (T3.6). The curve law itself (Q39) lives in
// pq/core/automation_curve.h, shared with the engine's automation renderer (T3.16) and the views (T3.15); this
// header re-exports it into pq::doc and adds the integer-tick queries the edit operations need.
//
// A lane's breakpoints are sorted by (tick, id). Between consecutive points a = (ta, va, curve c) and b = (tb, vb):
//     v(t) = va + (vb − va) · (e^{K·c·x} − 1) / (e^{K·c} − 1),   x = (t − ta) / (tb − ta),   K = kAutomationCurveK
// Several points on one tick form a jump: the segment arriving at that tick ends at the first of them, the value
// from that tick on is the last of them. See the core header for why this family (closed under cutting).
//
// All functions are allocation-free and noexcept.

#include <span>

#include "pq/core/automation_curve.h"
#include "pq/core/time.h"
#include "pq/document/model.h"

namespace pq::doc {

using pq::automationSegmentValue;
using pq::automationShape;
using pq::kAutomationCurveK;

// v(t−): the value the envelope approaches from the left at t (at a jump: the first point on that tick).
// Empty lanes return `fallback`.
double automationValueBefore(std::span<const Breakpoint> pts, Ticks t, double fallback = 0.0) noexcept;
// v(t): the value from t on (at a jump: the last point on that tick). Equal to pq::evalAutomation at integer t.
double automationValueAt(std::span<const Breakpoint> pts, Ticks t, double fallback = 0.0) noexcept;

}  // namespace pq::doc
