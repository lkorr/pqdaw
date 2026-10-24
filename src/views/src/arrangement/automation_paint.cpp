// Automation (T3.15): painting the envelopes (feathered ribbons, pq/ui/lines.h) and the automation widgets (lane
// headers, the header's parameter chooser and lanes toggle, "Re-enable Automation").

#include <algorithm>
#include <climits>
#include <cmath>

#include "../common/paint.h"
#include "internal.h"
#include "pq/ui/lines.h"
#include "pq/views/arrangement/automation.h"

namespace pq::views {

using namespace pq::ui;
namespace am = arrangement_metrics;
namespace au = automation_metrics;

namespace {

// A small square LED: red while automated, grey while overridden.
void led(DrawList& dl, const Theme& th, float x, float cy, float size, bool overridden) {
  const float y = std::floor(cy - size * 0.5f);
  dl.addRectFilled({x, y, x + size, y + size}, overridden ? th.automation.ledOverridden : th.automation.led);
}

// ▾ in a w×h box.
void chevron(DrawList& dl, float cx, float cy, float a, Rgba col) {
  dl.addTriangleFilled({cx - a, cy - a * 0.5f}, {cx + a, cy - a * 0.5f}, {cx, cy + a * 0.6f}, col);
}

}  // namespace

// ---- envelopes -------------------------------------------------------------------------------------------------

void ArrangementAutomation::paintLaneBackgrounds(PaintContext& ctx, const IRect& canvas) {
  const Theme* th = ctx.res.theme;
  const auto lanes = view_.laneRows();
  if (!th || lanes.empty()) return;
  const IRect c = intersect(canvas, ctx.clip);
  const int32_t top0 = canvas.y;  // the track content's top: the canvas is the track content
  const int32_t localTop = c.y - top0, localBottom = c.bottom() - top0;
  auto it = std::upper_bound(lanes.begin(), lanes.end(), localTop, [](int32_t y, const AutomationLaneRow& r) { return y < r.y + r.h; });
  for (; it != lanes.end() && it->y < localBottom; ++it) {
    const IRect r = intersect(IRect{c.x, top0 + it->y, c.w, it->h}, c);
    if (!r.empty()) ctx.dl.addRectFilled(r.toRect(), th->automation.lane);
  }
}

void ArrangementAutomation::paintLanes(PaintContext& ctx, const IRect& canvas) {
  if (ctx.frame != statsFrame_ || ctx.frame == 0) {
    const uint64_t built = stats_.envTilesBuilt, failed = stats_.envTileFailures;  // totals survive
    stats_ = {};
    stats_.envTilesBuilt = built;
    stats_.envTileFailures = failed;
  }
  statsFrame_ = ctx.frame;
  const Theme* th = ctx.res.theme;
  if (!th) return;
  const IRect c = intersect(canvas, ctx.clip);
  if (c.empty()) return;
  const int32_t top0 = canvas.y;
  const int32_t localTop = c.y - top0, localBottom = c.bottom() - top0;
  // Automation mode: the visible rows' chosen envelopes over their clips.
  if (mode_) {
    const auto rows = view_.rows();
    auto it = std::upper_bound(rows.begin(), rows.end(), localTop, [](int32_t y, const ArrangementRow& r) { return y < r.y + r.h; });
    for (; it != rows.end() && it->y < localBottom; ++it)
      if (const auto b = rowBand(static_cast<int32_t>(it - rows.begin()), false)) paintBand(ctx, *b, c);
  }
  // Lanes: separator, then the envelope.
  const auto lanes = view_.laneRows();
  auto it = std::upper_bound(lanes.begin(), lanes.end(), localTop, [](int32_t y, const AutomationLaneRow& r) { return y < r.y + r.h; });
  for (; it != lanes.end() && it->y < localBottom; ++it) {
    const float y1 = static_cast<float>(top0 + it->y + it->h);
    ctx.dl.addRectFilled({static_cast<float>(c.x), y1 - 1.0f, static_cast<float>(c.right()), y1}, th->arrangement.separator);
    paintBand(ctx, laneBand(static_cast<size_t>(it - lanes.begin())), c);
  }
}

void ArrangementAutomation::paintPinned(PaintContext& ctx, const IRect& canvas) {
  if (!mode_) return;
  const IRect c = intersect(canvas, ctx.clip);
  if (c.empty()) return;
  const auto rows = view_.bottomRows();
  for (size_t i = 0; i < rows.size(); ++i)
    if (const auto b = rowBand(static_cast<int32_t>(i), true)) paintBand(ctx, *b, c);
}

void ArrangementAutomation::paintBand(PaintContext& ctx, const AutomationBand& b, const IRect& clip) {
  const IRect vis = intersect(b.rect, clip);
  if (vis.empty() || !ctx.res.theme) return;
  const Theme& th = *ctx.res.theme;
  const auto& at = th.automation;
  DrawList& dl = ctx.dl;
  ++stats_.bandsPainted;
  const doc::Project& p = *view_.shown();
  const ParamDisplay disp = paramDisplay(p, b.param);
  const auto yOf = [&](double v) {
    return static_cast<float>(static_cast<double>(b.bottom) - displayNorm(disp, v) * static_cast<double>(b.bottom - b.top));
  };
  // Everything is clipped to the visible part of the band (ribbon feathers, handles and the readout would spill).
  dl.pushClip(vis.toRect());
  if (b.overClips) dl.addRectFilled(vis.toRect(), at.veil);

  // The time selection.
  if (selRange_ && selParam_ == b.param && selTrack_ == b.track) {
    const float x0 = std::max(static_cast<float>(vis.x), std::floor(view_.xOfTick(static_cast<double>(selRange_->start))));
    const float x1 = std::min(static_cast<float>(vis.right()),
                              selRange_->end > selRange_->start ? std::floor(view_.xOfTick(static_cast<double>(selRange_->end)))
                                                                : x0 + static_cast<float>(std::max(1, ctx.scale.px(1.0f))));
    if (x1 > x0) dl.addRectFilled({x0, static_cast<float>(b.rect.y), x1, static_cast<float>(b.rect.bottom())}, at.selection);
  }

  const bool overridden = ctx_.automationOverridden(b.param);
  const Rgba lineCol = overridden ? at.lineOverridden : at.line;
  const std::span<const doc::Breakpoint> pts = pointsOf(b);
  const double fallback = currentParamValue(p, b.param);
  if (pts.empty()) {
    // Not automated: a dashed line at the parameter's value (Live). The dash phase is fixed to the device x, so a
    // partial repaint draws exactly the dashes a full one would.
    const float y = std::floor(yOf(fallback));
    const float t = static_cast<float>(std::max(1, ctx.scale.px(1.0f)));
    const float dash = static_cast<float>(std::max(2, ctx.scale.px(4.0f))), gap = static_cast<float>(std::max(1, ctx.scale.px(3.0f)));
    const float period = dash + gap;
    for (float x = std::floor(static_cast<float>(vis.x) / period) * period; x < static_cast<float>(vis.right()); x += period)
      dl.addRectFilled({std::max(x, static_cast<float>(vis.x)), y, std::min(x + dash, static_cast<float>(vis.right())), y + t},
                       overridden ? at.lineOverridden : at.unautomated);
  } else {
    // Density from the band's whole width (not the painted part: partial and full repaints decide alike).
    const auto bandFirst = std::lower_bound(pts.begin(), pts.end(), static_cast<Ticks>(std::floor(view_.tickAtX(static_cast<float>(b.rect.x)))),
                                            [](const doc::Breakpoint& bp, Ticks t) { return bp.tick < t; });
    const auto bandLast = std::upper_bound(bandFirst, pts.end(), static_cast<Ticks>(std::ceil(view_.tickAtX(static_cast<float>(b.rect.right())))),
                                           [](Ticks t, const doc::Breakpoint& bp) { return t < bp.tick; });
    const auto count = static_cast<float>(bandLast - bandFirst);
    const float lineW = std::max(1.0f, au::kLineWidth * ctx.scale.factor);
    const bool dense = count * 2.0f > static_cast<float>(b.rect.w);
    // T3.x-fix-paint: the line from envelope tiles (rasterised once per lane snapshot and zoom, drawn as one quad per
    // chunk), else directly. Breakpoint handles then sit on the tiles' whole-pixel x mapping.
    double tileOffset = 0.0;
    const bool tiled = b.lane && pts.data() == b.lane->points.data() && paintEnvelopeTiles(ctx, b, pts, vis, dense, lineCol, lineW, tileOffset);
    if (!tiled) paintEnvelopeDirect(ctx, b, pts, fallback, vis, dense, lineCol, lineW);
    const double k = view_.devicePxPerTick();
    const auto xOf = [&](Ticks t) {
      return tiled ? static_cast<float>(tileOffset + static_cast<double>(t) * k) : view_.xOfTick(static_cast<double>(t));
    };

    // Breakpoint handles (all of them while they are at least kHandleMinSpacing px apart on average, else only the
    // selected ones).
    const float reach = (au::kPointRadius + 2.0f) * ctx.scale.factor;
    const auto first = std::lower_bound(pts.begin(), pts.end(), static_cast<Ticks>(std::floor(view_.tickAtX(static_cast<float>(vis.x) - reach))),
                                        [](const doc::Breakpoint& bp, Ticks t) { return bp.tick < t; });
    const auto last = std::upper_bound(first, pts.end(), static_cast<Ticks>(std::ceil(view_.tickAtX(static_cast<float>(vis.right()) + reach))),
                                       [](Ticks t, const doc::Breakpoint& bp) { return t < bp.tick; });
    // Level of detail from the band's whole width (count above: partial and full repaints agree).
    const bool all = count * au::kHandleMinSpacing * ctx.scale.factor <= static_cast<float>(b.rect.w);
    const bool sameLane = selParam_ == b.param && selTrack_ == b.track;
    const float r = au::kPointRadius * ctx.scale.factor;
    for (auto it = (all || (sameLane && !selIds_.empty())) ? first : last; it != last; ++it) {  // nothing to draw: no walk
      const bool sel = sameLane && std::binary_search(selIds_.begin(), selIds_.end(), it->id.value);
      if (!all && !sel) continue;
      const Vec2 c{xOf(it->tick), yOf(it->value)};
      dl.addCircleFilled(c, r + ctx.scale.factor, at.pointEdge);
      dl.addCircleFilled(c, r, sel ? at.pointSelected : lineCol);
      ++stats_.handles;
    }
  }

  // The value readout next to a dragged breakpoint (the band the drag is in).
  if (!readoutText_.empty() && drag_.kind == DragKind::Points && drag_.band.param == b.param && drag_.band.rect == b.rect &&
      ctx.res.text && ctx.res.uiFonts) {
    for (const doc::Breakpoint& bp : pts)
      if (bp.id == drag_.pressed.id) {
        const FontId font = ctx.res.uiFonts->small;
        const TextRun run = ctx.res.text->shape(font, readoutText_);
        const int32_t pad = ctx.scale.px(3.0f), h = ctx.scale.px(14.0f);
        const int32_t w = run.advance + 2 * pad;
        const float px = view_.xOfTick(static_cast<double>(bp.tick)), py = yOf(bp.value);
        int32_t x = static_cast<int32_t>(std::lround(px)) + ctx.scale.px(au::kPointRadius + 4.0f);
        int32_t y = static_cast<int32_t>(std::lround(py)) - h - ctx.scale.px(2.0f);
        if (x + w > b.rect.right()) x = static_cast<int32_t>(std::lround(px)) - ctx.scale.px(au::kPointRadius + 4.0f) - w;
        y = std::clamp(y, b.rect.y, std::max(b.rect.y, b.rect.bottom() - h));
        const IRect box{x, y, w, h};
        dl.addRectFilled(box.toRect(), at.readout);
        TextSystem::drawRun(dl, run, {static_cast<float>(x + pad), static_cast<float>(paint::capCentredBaseline(y, h, ctx.res.text->metrics(font).pixelSize))},
                            at.readoutText);
        break;
      }
  }
  dl.popClip();
}

void ArrangementAutomation::paintEnvelopeDirect(PaintContext& ctx, const AutomationBand& b, std::span<const doc::Breakpoint> pts,
                                                double fallback, const IRect& vis, bool dense, Rgba lineCol, float lineW) {
  DrawList& dl = ctx.dl;
  const ParamDisplay disp = paramDisplay(*view_.shown(), b.param);
  const auto yOf = [&](double v) {
    return static_cast<float>(static_cast<double>(b.bottom) - displayNorm(disp, v) * static_cast<double>(b.bottom - b.top));
  };
  if (dense) {
    // Dense (under 2 px per breakpoint): one span per pixel column from the lowest to the highest value the
    // envelope takes there, as a rect. The ribbon would be a zigzag of sub-pixel segments costing 8 vertices each;
    // the spans look the same and cost 4 per column.
    // Per column: the envelope at its right edge (a cursor: amortised O(1); the left edge is the previous column's)
    // and every breakpoint inside (value compares only; the display mapping runs at most twice per column). Each column
    // depends only on its own ticks, so partial and full repaints agree exactly.
    EnvelopeCursor cur(pts, fallback);
    auto it = std::lower_bound(pts.begin(), pts.end(), static_cast<Ticks>(std::floor(view_.tickAtX(static_cast<float>(vis.x)))),
                               [](const doc::Breakpoint& bp, Ticks t) { return bp.tick < t; });
    uint32_t spans = 0;
    double tl = view_.tickAtX(static_cast<float>(vis.x));
    double vl = cur.at(tl);  // the envelope at the column's left edge (the previous column's right edge)
    for (int32_t col = vis.x; col < vis.right(); ++col) {
      const double tr = view_.tickAtX(static_cast<float>(col + 1));
      double vmin = vl, vmax = vl;
      for (; it != pts.end() && static_cast<double>(it->tick) < tr; ++it) {
        if (static_cast<double>(it->tick) < tl) continue;
        vmin = std::min(vmin, static_cast<double>(it->value));
        vmax = std::max(vmax, static_cast<double>(it->value));
      }
      const double vr = cur.at(tr);
      vmin = std::min(vmin, vr);
      vmax = std::max(vmax, vr);
      vl = vr;
      const float ya = yOf(vmin), yb = vmax == vmin ? ya : yOf(vmax);
      const auto x = static_cast<float>(col);
      dl.addRectFilled({x, std::min(ya, yb) - lineW * 0.5f, x + 1.0f, std::max(ya, yb) + lineW * 0.5f}, lineCol);
      ++spans;
      tl = tr;
    }
    stats_.vertices += spans;
    return;
  }
  // The polyline over the visible columns. A partial repaint must draw exactly the pixels a full one draws, and
  // a ribbon's pixels depend on its vertices, the joins at them (each vertex's neighbours) and the column groups.
  // So the polyline starts at a breakpoint three device columns (and two breakpoints) before the painted part and
  // ends likewise after it, or at a fixed anchor just outside the band: from there on, its vertices, groups and
  // joins are the full repaint's.
  const double k = view_.devicePxPerTick();
  const float margin = 8.0f * ctx.scale.factor, anchor = 64.0f * ctx.scale.factor;
  const double lo = view_.tickAtX(static_cast<float>(b.rect.x) - anchor);
  const double hi = view_.tickAtX(static_cast<float>(b.rect.right()) + anchor);
  auto columnOf = [&](const doc::Breakpoint& bp) { return std::floor(view_.xOfTick(static_cast<double>(bp.tick))); };
  double t0 = lo;
  {
    const double from = view_.tickAtX(static_cast<float>(vis.x) - margin);
    auto it = std::upper_bound(pts.begin(), pts.end(), from, [](double t, const doc::Breakpoint& bp) { return t < static_cast<double>(bp.tick); });
    if (it != pts.begin()) {
      size_t i = static_cast<size_t>(it - pts.begin()) - 1, steps = 0;
      int cols = 0;
      float col = columnOf(pts[i]);
      while (i > 0 && (cols < 3 || steps < 2) && static_cast<double>(pts[i].tick) > lo) {
        --i;
        ++steps;
        if (const float c = columnOf(pts[i]); c != col) {
          ++cols;
          col = c;
        }
      }
      t0 = std::max(lo, static_cast<double>(pts[i].tick));
    }
  }
  double t1 = hi;
  {
    const double to = view_.tickAtX(static_cast<float>(vis.right()) + margin);
    auto it = std::lower_bound(pts.begin(), pts.end(), to, [](const doc::Breakpoint& bp, double t) { return static_cast<double>(bp.tick) < t; });
    if (it != pts.end()) {
      size_t i = static_cast<size_t>(it - pts.begin()), steps = 0;
      int cols = 0;
      float col = columnOf(pts[i]);
      while (i + 1 < pts.size() && (cols < 3 || steps < 2) && static_cast<double>(pts[i].tick) < hi) {
        ++i;
        ++steps;
        if (const float c = columnOf(pts[i]); c != col) {
          ++cols;
          col = c;
        }
      }
      t1 = std::min(hi, static_cast<double>(pts[i].tick));
    }
  }
  t1 = std::max(t0, t1);
  envelopeVertices(pts, fallback, t0, t1, static_cast<double>(au::kMaxChordPx) / k, verts_);
  // Map to device px; runs of vertices inside one pixel column collapse to first / min / max / last (a lane costs
  // O(width) however many points it has).
  poly_.clear();
  size_t group = 0;
  int32_t column = INT32_MIN;
  auto flush = [&] {
    const size_t n = poly_.size() - group;
    if (n <= 4) return;
    size_t lo = group, hi = group;
    for (size_t i = group + 1; i < poly_.size() - 1; ++i) {
      if (poly_[i].y < poly_[lo].y) lo = i;
      if (poly_[i].y > poly_[hi].y) hi = i;
    }
    const Vec2 first = poly_[group], last = poly_.back();
    const Vec2 a = poly_[std::min(lo, hi)], z = poly_[std::max(lo, hi)];
    poly_.resize(group);
    poly_.push_back(first);
    poly_.push_back(a);
    poly_.push_back(z);
    poly_.push_back(last);
  };
  for (const EnvelopeVertex& v : verts_) {
    const Vec2 q{view_.xOfTick(v.tick), yOf(v.value)};
    const auto col = static_cast<int32_t>(std::floor(q.x));
    if (col != column) {
      flush();
      group = poly_.size();
      column = col;
    }
    poly_.push_back(q);
  }
  flush();
  stats_.vertices += static_cast<uint32_t>(poly_.size());
  smoothPolyline(dl, poly_.data(), static_cast<uint32_t>(poly_.size()), lineCol, lineW);
}

// ---- lane header -------------------------------------------------------------------------------------------------

namespace arrangement {

LaneHeader::LaneHeader(std::string name, ArrangementView& view) : Control(std::move(name)), view_(view) {
  setInfo("Automation Lane",
          "Shows one parameter's automation. Choose the parameter with the chooser; + adds another lane, − hides this "
          "one. Click the line to add a breakpoint, drag breakpoints to move them, Alt-drag a segment to curve it, "
          "double-click a breakpoint to delete it; right-click for shapes. Drag the lane's bottom edge to resize it.");
}

void LaneHeader::bind(const AutomationLaneRow& lane, uint16_t depth) {
  const doc::Project& p = *view_.shown();
  std::string label = paramName(p, lane.param);
  const bool automated = lane.lane != nullptr;
  const bool overridden = view_.context().automationOverridden(lane.param);
  const bool changed = label != label_ || automated != automated_ || overridden != overridden_ || depth != depth_ ||
                       lane.param != lane_.param || lane.index != lane_.index;
  lane_ = lane;
  label_ = std::move(label);
  automated_ = automated;
  overridden_ = overridden;
  depth_ = depth;
  if (changed) markDirty();
}

namespace {
// The name column of a header (left of the mixer section).
IRect nameColumn(const IRect& r, const UiScale& s, float mixerW, uint16_t depth) {
  const int32_t indent = s.px(am::kGroupIndent * static_cast<float>(depth));
  const int32_t mix = std::min(r.w, s.px(mixerW));
  return {r.x + indent, r.y, std::max(0, r.w - indent - mix), r.h};
}
}  // namespace

// Lane headers have no mixer controls: the chooser runs across the name and mixer columns (Live's lane header shows
// the device and parameter choosers there), + and − sit at the right end.
IRect LaneHeader::chooserRect() const noexcept {
  const UiScale s = tree() ? tree()->uiScale() : UiScale{};
  const IRect n = nameColumn(rect(), s, 0.0f, depth_);
  const int32_t x0 = n.x + s.px(14.0f), x1 = n.right() - s.px(44.0f);
  return {x0, n.y + s.px(3.0f), std::max(0, x1 - x0), std::min(s.px(au::kChooserHeight), std::max(0, n.h - s.px(4.0f)))};
}

IRect LaneHeader::addRect() const noexcept {
  const UiScale s = tree() ? tree()->uiScale() : UiScale{};
  const IRect n = nameColumn(rect(), s, 0.0f, depth_);
  return {n.right() - s.px(38.0f), n.y + s.px(3.0f), s.px(14.0f), std::min(s.px(14.0f), std::max(0, n.h - s.px(4.0f)))};
}

IRect LaneHeader::hideRect() const noexcept {
  const UiScale s = tree() ? tree()->uiScale() : UiScale{};
  const IRect n = nameColumn(rect(), s, 0.0f, depth_);
  return {n.right() - s.px(21.0f), n.y + s.px(3.0f), s.px(14.0f), std::min(s.px(14.0f), std::max(0, n.h - s.px(4.0f)))};
}

void LaneHeader::paint(PaintContext& ctx) {
  const Theme* thp = ctx.res.theme;
  if (!thp) return;
  const Theme& th = *thp;
  const IRect r = rect();
  if (r.empty()) return;
  DrawList& dl = ctx.dl;
  const float x0 = static_cast<float>(r.x), y0 = static_cast<float>(r.y), x1 = static_cast<float>(r.right()), y1 = static_cast<float>(r.bottom());
  dl.addRectFilled({x0, y0, x1, y1 - 1.0f}, th.automation.laneHeader);
  dl.addRectFilled({x0, y1 - 1.0f, x1, y1}, th.arrangement.separator);
  const IRect ch = chooserRect();
  if (ch.h <= 0) return;
  // LED, chooser (field + name + ▾), + and −.
  const float ledSize = static_cast<float>(std::max(3, ctx.scale.px(6.0f)));
  if (automated_) led(dl, th, static_cast<float>(ch.x) - ledSize - static_cast<float>(ctx.scale.px(3.0f)), static_cast<float>(ch.y) + static_cast<float>(ch.h) * 0.5f, ledSize, overridden_);
  if (ch.w > 0) {
    paint::box(dl, ch, 0.0f, th.mixer.field, th.mixer.boxBorder);
    if (const ThemeFonts* f = ctx.res.uiFonts)
      paint::text(ctx, f->small, ch, label_, th.automation.laneHeaderText, TextAlign::Left, ctx.scale.px(4.0f), ctx.scale.px(14.0f));
    chevron(dl, static_cast<float>(ch.right()) - static_cast<float>(ctx.scale.px(7.0f)), static_cast<float>(ch.y) + static_cast<float>(ch.h) * 0.5f,
            static_cast<float>(ctx.scale.px(3.0f)), th.automation.laneHeaderText);
  }
  for (int i = 0; i < 2; ++i) {
    const IRect b = i == 0 ? addRect() : hideRect();
    if (b.empty()) continue;
    paint::box(dl, b, 0.0f, th.mixer.buttonOff, th.mixer.boxBorder);
    const float cx = static_cast<float>(b.x) + static_cast<float>(b.w) * 0.5f, cy = static_cast<float>(b.y) + static_cast<float>(b.h) * 0.5f;
    const float a = std::round(static_cast<float>(b.w) * 0.25f), t = static_cast<float>(std::max(1, ctx.scale.px(1.0f)));
    dl.addRectFilled({std::round(cx - a), std::round(cy - t * 0.5f), std::round(cx + a), std::round(cy - t * 0.5f) + t}, th.mixer.buttonText);
    if (i == 0) dl.addRectFilled({std::round(cx - t * 0.5f), std::round(cy - a), std::round(cx - t * 0.5f) + t, std::round(cy + a)}, th.mixer.buttonText);
  }
}

bool LaneHeader::onPointer(const PointerEvent& e) {
  const float scale = tree() ? tree()->scale() : 1.0f;
  ArrangementAutomation& a = view_.automation();
  if (resizing_) {
    if (e.type == PointerType::Move) {
      a.setLaneHeight(lane_.track, lane_.param, resizeStartH_ + (e.pos.y - resizeStartY_) / scale);
      return true;
    }
    if (e.type == PointerType::Up && e.button == PointerButton::Left) {
      resizing_ = false;
      return true;
    }
  }
  if (e.type != PointerType::Down) return e.type == PointerType::Up;
  const IRect r = rect();
  if (e.button == PointerButton::Left && e.pos.y >= static_cast<float>(r.bottom()) - std::max(2.0f, am::kResizeGrab * scale) - 1.0f) {
    resizing_ = true;
    resizeStartY_ = e.pos.y;
    resizeStartH_ = a.laneHeight(lane_.track, lane_.param);
    return true;
  }
  if (e.button == PointerButton::Right || chooserRect().contains(e.pos)) {
    (void)a.openChooser(*this, lane_.track, static_cast<int32_t>(std::find_if(view_.laneRows().begin(), view_.laneRows().end(),
                                                                              [&](const AutomationLaneRow& l) {
                                                                                return l.track == lane_.track && l.param == lane_.param;
                                                                              }) -
                                                                 view_.laneRows().begin()));
    return true;
  }
  if (e.button != PointerButton::Left) return true;
  if (addRect().contains(e.pos)) {
    (void)a.addLane(lane_.track);
    return true;
  }
  if (hideRect().contains(e.pos)) {
    a.hideLaneLater(lane_.track, lane_.index);  // removes this widget: not from inside its own handler
    return true;
  }
  view_.selectTrack(lane_.track, e.mods);
  return true;
}

// ---- header parameter chooser ------------------------------------------------------------------------------------

ParamChooser::ParamChooser(ArrangementView& view) : Control("automation"), view_(view) {
  setInfo("Automation Control Chooser",
          "The parameter whose automation is shown over this track's clips in Automation Mode (A): the track's mixer "
          "controls and the main tunings' parameters.");
}

void ParamChooser::set(doc::TrackId track, std::string label, bool automated, bool overridden) {
  if (track == track_ && label == label_ && automated == automated_ && overridden == overridden_) return;
  track_ = track;
  label_ = std::move(label);
  automated_ = automated;
  overridden_ = overridden;
  markDirty();
}

void ParamChooser::paint(PaintContext& ctx) {
  const Theme* th = ctx.res.theme;
  const IRect r = rect();
  if (!th || r.empty()) return;
  DrawList& dl = ctx.dl;
  paint::box(dl, r, 0.0f, hovered() ? paint::hoverOf(th->mixer.field) : th->mixer.field, th->mixer.boxBorder);
  const int32_t ledW = ctx.scale.px(6.0f);
  if (automated_) led(dl, *th, static_cast<float>(r.x + ctx.scale.px(3.0f)), static_cast<float>(r.y) + static_cast<float>(r.h) * 0.5f, static_cast<float>(ledW), overridden_);
  if (const ThemeFonts* f = ctx.res.uiFonts)
    paint::text(ctx, f->small, r, label_, th->mixer.fieldText, TextAlign::Left, ledW + ctx.scale.px(6.0f), ctx.scale.px(14.0f));
  chevron(dl, static_cast<float>(r.right() - ctx.scale.px(7.0f)), static_cast<float>(r.y) + static_cast<float>(r.h) * 0.5f,
          static_cast<float>(ctx.scale.px(3.0f)), th->mixer.fieldText);
}

bool ParamChooser::onPointer(const PointerEvent& e) {
  if (e.type == PointerType::Down && (e.button == PointerButton::Left || e.button == PointerButton::Right)) {
    (void)view_.automation().openChooser(*this, track_, -1);
    return true;
  }
  return e.type == PointerType::Up;
}

// ---- lanes toggle ----------------------------------------------------------------------------------------------

LanesToggle::LanesToggle(ArrangementView& view) : Control("lanes"), view_(view) {
  setInfo("Show / Hide Automation Lanes", "Shows this track's automation lanes under it, one per parameter.");
}

void LanesToggle::set(doc::TrackId track, bool shown) {
  if (track == track_ && shown == shown_) return;
  track_ = track;
  shown_ = shown;
  markDirty();
}

void LanesToggle::paint(PaintContext& ctx) {
  const IRect r = rect();
  if (r.empty() || !ctx.res.theme) return;
  const auto* header = static_cast<const TrackHeader*>(parent());
  const Rgba base = view_.model().clipTextColour(*ctx.res.theme, header->titleColour());
  const Rgba col = hovered() ? lerp(base, rgba(255, 255, 255), 0.25f) : base;
  const float cx = static_cast<float>(r.x) + static_cast<float>(r.w) * 0.5f, cy = static_cast<float>(r.y) + static_cast<float>(r.h) * 0.5f;
  const float a = std::round(static_cast<float>(ctx.scale.px(3.5f)));
  if (shown_) ctx.dl.addTriangleFilled({cx - a, cy - a * 0.6f}, {cx + a, cy - a * 0.6f}, {cx, cy + a * 0.8f}, col);  // ▼
  else ctx.dl.addTriangleFilled({cx - a * 0.6f, cy - a}, {cx + a * 0.8f, cy}, {cx - a * 0.6f, cy + a}, col);         // ▶
}

bool LanesToggle::onPointer(const PointerEvent& e) {
  if (e.type == PointerType::Down && e.button == PointerButton::Left) {
    view_.automation().showLanes(track_, !shown_);
    return true;
  }
  return e.type == PointerType::Up && e.button == PointerButton::Left;
}

// ---- Re-enable Automation --------------------------------------------------------------------------------------

ReenableButton::ReenableButton(ArrangementView& view) : Control("reenable-automation"), view_(view) {
  setInfo("Re-enable Automation",
          "Some automated parameters were changed by hand, which switched their automation off (their envelopes are "
          "grey). Click to switch it back on.");
  setVisible(false);
}

void ReenableButton::paint(PaintContext& ctx) {
  const Theme* th = ctx.res.theme;
  const IRect r = rect();
  if (!th || r.empty()) return;
  paint::box(ctx.dl, r, 0.0f, hovered() ? paint::hoverOf(th->automation.reenable) : th->automation.reenable, th->mixer.boxBorder);
  if (const ThemeFonts* f = ctx.res.uiFonts)
    paint::text(ctx, f->small, r, "Re-enable Automation", th->automation.reenableText, TextAlign::Center, ctx.scale.px(4.0f), ctx.scale.px(4.0f));
}

bool ReenableButton::onPointer(const PointerEvent& e) {
  if (e.type == PointerType::Down && e.button == PointerButton::Left) {
    view_.automation().reenable();
    return true;
  }
  return e.type == PointerType::Up && e.button == PointerButton::Left;
}

}  // namespace arrangement

}  // namespace pq::views
