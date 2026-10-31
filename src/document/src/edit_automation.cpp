// Automation edit operations (T3.6).

#include <algorithm>
#include <cmath>
#include <numbers>

#include "edit_common.h"

namespace pq::doc::ops {

using namespace detail;

namespace {

struct LaneFound {
  const Track* track;
  const AutomationLane* lane;
};

Result<LaneFound> lookupLane(const Project& p, TrackId track, LaneId lane) {
  const Track* t = findTrackPtr(p, track);
  if (!t) return notFound("track not found");
  const AutomationLane* l = findLane(*t, lane);
  if (!l) return notFound("lane not found on this track");
  return LaneFound{t, l};
}

float clampValue(const ParamRef& k, double v) {
  const ParamRange r = paramRange(k);
  return static_cast<float>(std::clamp(v, r.min, r.max));
}

const Breakpoint* findPoint(const AutomationLane& l, BreakpointId id) {
  for (const Breakpoint& b : l.points)
    if (b.id == id) return &b;
  return nullptr;
}

std::vector<BreakpointId> distinct(std::span<const BreakpointId> ids) {
  std::vector<BreakpointId> v(ids.begin(), ids.end());
  std::sort(v.begin(), v.end());
  v.erase(std::unique(v.begin(), v.end()), v.end());
  return v;
}

// The shape's breakpoints over [start, end] (fresh ids, increasing, so points on one tick keep their order).
Points shapePoints(const ShapeParams& s, const ParamRef& k, Ticks start, Ticks end, IdSource& ids) {
  const float lo = clampValue(k, s.low);
  const float hi = clampValue(k, s.high);
  const double len = static_cast<double>(end - start);
  const int32_t cycles = std::max(s.cycles, 1);
  Points out;
  auto at = [&](double x, double v) {
    const Ticks t = start + static_cast<Ticks>(std::llround(x * len));
    out.push_back(Breakpoint{t, static_cast<float>(v), 0.0f, ids.breakpoint()});
  };
  switch (s.kind) {
    case ShapeKind::RampUp:
      at(0.0, lo);
      at(1.0, hi);
      break;
    case ShapeKind::RampDown:
      at(0.0, hi);
      at(1.0, lo);
      break;
    case ShapeKind::Sine: {
      constexpr int kSegs = 16;
      const double mid = 0.5 * (static_cast<double>(lo) + hi);
      const double amp = 0.5 * (static_cast<double>(hi) - lo);
      const int total = kSegs * cycles;
      for (int i = 0; i <= total; ++i) {
        const double ph = 2.0 * std::numbers::pi * static_cast<double>(i % kSegs) / kSegs;
        at(static_cast<double>(i) / total, std::clamp(mid + amp * std::sin(ph), std::min<double>(lo, hi),
                                                       std::max<double>(lo, hi)));
      }
      break;
    }
    case ShapeKind::Triangle:
      for (int c = 0; c < cycles; ++c) {
        at(static_cast<double>(c) / cycles, lo);
        at((c + 0.5) / cycles, hi);
      }
      at(1.0, lo);
      break;
    case ShapeKind::Square:
      for (int c = 0; c < cycles; ++c) {
        at(static_cast<double>(c) / cycles, hi);
        at((c + 0.5) / cycles, hi);
        at((c + 0.5) / cycles, lo);
        at((c + 1.0) / cycles, lo);
      }
      break;
    case ShapeKind::Saw:
      for (int c = 0; c < cycles; ++c) {
        at(static_cast<double>(c) / cycles, lo);
        at((c + 1.0) / cycles, hi);
      }
      break;
  }
  return out;
}

}  // namespace

EditResult addBreakpoint(const Project& p, IdSource& ids, TrackId track, LaneId lane, Ticks tick, double value,
                         float curve) {
  Result<LaneFound> f = lookupLane(p, track, lane);
  if (!f) return std::move(f).error();
  PQ_DOC_TRY(checkTick(tick, "breakpoint"));
  if (!std::isfinite(value) || !std::isfinite(curve)) return invalid("value must be finite");
  const Breakpoint b{tick, clampValue(f->lane->param, value), std::clamp(curve, -1.0f, 1.0f), ids.breakpoint()};
  Edit e = makeEdit("Add Breakpoint");
  e.points.push_back(b.id);
  batchOf(e).commands.push_back(AddBreakpoints{track, lane, {b}});
  return e;
}

EditResult moveBreakpoints(const Project& p, TrackId track, LaneId lane, std::span<const BreakpointId> points,
                           Ticks dt, double dv) {
  Result<LaneFound> f = lookupLane(p, track, lane);
  if (!f) return std::move(f).error();
  if (!std::isfinite(dv)) return invalid("value must be finite");
  Edit e = makeEdit(points.size() == 1 ? "Move Breakpoint" : "Move Breakpoints");
  UpdateBreakpoints u{track, lane, {}};
  for (BreakpointId id : distinct(points)) {
    const Breakpoint* b = findPoint(*f->lane, id);
    if (!b) return notFound("breakpoint not found");
    Breakpoint m = *b;
    m.tick += dt;
    PQ_DOC_TRY(checkTick(m.tick, "breakpoint"));
    m.value = clampValue(f->lane->param, static_cast<double>(m.value) + dv);
    if (!(m == *b)) u.points.push_back(m);
    e.points.push_back(id);
  }
  if (!u.points.empty()) batchOf(e).commands.push_back(std::move(u));
  return e;
}

EditResult deleteBreakpoints(const Project& p, TrackId track, LaneId lane, std::span<const BreakpointId> points) {
  Result<LaneFound> f = lookupLane(p, track, lane);
  if (!f) return std::move(f).error();
  Edit e = makeEdit(points.size() == 1 ? "Delete Breakpoint" : "Delete Breakpoints");
  RemoveBreakpoints r{track, lane, distinct(points)};
  for (BreakpointId id : r.points)
    if (!findPoint(*f->lane, id)) return notFound("breakpoint not found");
  if (!r.points.empty()) batchOf(e).commands.push_back(std::move(r));
  return e;
}

EditResult setSegmentCurve(const Project& p, TrackId track, LaneId lane, BreakpointId point, float curve) {
  Result<LaneFound> f = lookupLane(p, track, lane);
  if (!f) return std::move(f).error();
  if (!std::isfinite(curve)) return invalid("curve must be finite");
  const Breakpoint* b = findPoint(*f->lane, point);
  if (!b) return notFound("breakpoint not found");
  Edit e = makeEdit("Curve Segment");
  e.points.push_back(point);
  Breakpoint m = *b;
  m.curve = std::clamp(curve, -1.0f, 1.0f);
  if (!(m == *b)) batchOf(e).commands.push_back(UpdateBreakpoints{track, lane, {m}});
  return e;
}

EditResult insertShape(const Project& p, IdSource& ids, TrackId track, LaneId lane, Ticks start, Ticks end,
                       const ShapeParams& shape) {
  Result<LaneFound> f = lookupLane(p, track, lane);
  if (!f) return std::move(f).error();
  PQ_DOC_TRY(checkTick(start, "start"));
  PQ_DOC_TRY(checkTick(end, "end"));
  if (end <= start) return invalid("the range is empty");
  if (!std::isfinite(shape.low) || !std::isfinite(shape.high)) return invalid("shape values must be finite");
  // Points are drawn in envelope order (left edge, shape, right edge), so same-tick jumps keep their order. The
  // shape keeps every point, even one that repeats a neighbour's value: simplify() treats its ids as existing.
  const uint32_t firstNew = ids.peek();
  const Points& pts = f->lane->points;
  Points r = envBefore(pts, start, ids, true);
  const uint32_t shapeLo = ids.peek();
  append(r, shapePoints(shape, f->lane->param, start, end, ids));
  const uint32_t shapeHi = ids.peek();
  append(r, envFrom(pts, end, ids));
  simplify(r, firstNew, ids, shapeLo, shapeHi);
  Edit e = makeEdit("Insert Shape");
  for (const Breakpoint& b : r)
    if (b.tick >= start && b.tick <= end && !findPoint(*f->lane, b.id)) e.points.push_back(b.id);
  emitLaneDiff(batchOf(e), track, *f->lane, std::move(r));
  return e;
}

}  // namespace pq::doc::ops
