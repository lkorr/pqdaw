// The arrangement overview strip (T3.8): the whole song in miniature with the visible region framed (Live's
// Arrangement Overview). Its clip blocks are cached and rebuilt only when the document, the rows, the strip's size
// or the range it shows change; scrolling repaints the cached blocks and moves the frame.

#include <algorithm>
#include <cmath>

#include "internal.h"

namespace pq::views::arrangement {

using namespace pq::ui;

Overview::Overview(ArrangementView& view) : Control("overview"), view_(view) {
  setInfo("Arrangement Overview",
          "The whole arrangement; the frame is the part shown below. Click or drag to move the view.");
}

void Overview::attachScroll(ScrollView& sv) {
  if (scroll_) scroll_->unlinkDependent(this);
  scroll_ = sv.linkDependent(this) ? &sv : nullptr;
}

Overview::~Overview() {
  if (scroll_) scroll_->unlinkDependent(this);
}

void Overview::onScrollChanged() noexcept { markDirty(); }
void Overview::onScrollViewGone() noexcept { scroll_ = nullptr; }

TickRange Overview::range() const noexcept {
  const Ticks end = std::max(view_.songEnd() + 4 * kTicksPerWhole, view_.visibleTicks().end);
  return {0, std::max<Ticks>(end, 1)};
}

void Overview::rebuildCache(const IRect& r) {
  blocks_.clear();
  const Model& m = view_.model();
  const TickRange rg = range();
  cacheDoc_ = m.shown.get();
  cacheRows_ = m.stats.rowRebuilds;
  cacheRect_ = r;
  cacheRange_ = rg;
  cacheValid_ = true;
  const int32_t contentH = m.contentHeight;
  if (contentH <= 0 || r.w <= 0 || r.h <= 2) return;
  const int32_t oh = r.h - 1;  // the bottom row is the divider
  const double xScale = static_cast<double>(r.w) / static_cast<double>(rg.end - rg.start);
  const auto rows = view_.rows();
  auto addTrack = [&](const doc::Track& t, int32_t y0, int32_t y1, bool parentActive) {
    const size_t first = blocks_.size();
    for (const doc::ClipSlot& s : t.clips) {
      const doc::NoteClip& c = *s.clip;
      const auto x0 = static_cast<int32_t>(std::floor(static_cast<double>(c.start - rg.start) * xScale));
      const auto x1 = std::max(x0 + 1, static_cast<int32_t>(std::ceil(static_cast<double>(c.start + c.length - rg.start) * xScale)));
      const Rgba col = (c.active && sounds(t) && parentActive) ? rgbOf(c.colour) : inactiveColour();
      if (blocks_.size() > first) {
        Block& last = blocks_.back();
        if (last.col == col && x0 <= last.x1) {
          last.x1 = std::max(last.x1, x1);
          continue;
        }
      }
      blocks_.push_back({x0, x1, y0, y1, col});
    }
  };
  struct Members {
    decltype(addTrack)& add;
    void operator()(const doc::Track& g, int32_t y0, int32_t y1, bool active) {
      for (const doc::TrackPtr& t : g.children) {
        if (t->kind == doc::TrackKind::Group) (*this)(*t, y0, y1, active && sounds(*t));
        else add(*t, y0, y1, active);
      }
    }
  } members{addTrack};
  for (const ArrangementRow& row : rows) {
    auto y0 = static_cast<int32_t>(static_cast<int64_t>(row.y) * oh / contentH);
    auto y1 = static_cast<int32_t>(static_cast<int64_t>(row.y + row.h) * oh / contentH);
    if (y1 - y0 >= 3) y1 -= 1;  // a gap between tracks when there is room
    if (y1 <= y0) y1 = y0 + 1;
    if (row.group) {
      if (row.folded) members(*row.track, y0, y1, sounds(*row.track));
      continue;
    }
    addTrack(*row.track, y0, y1, sounds(*row.track));
  }
  // Rasterise at the strip's pixel resolution and keep one run per row and colour: many tracks share a pixel row
  // of a 16 px strip, so this bounds the strip's geometry by its pixels instead of the project's clip count
  // (64 × 200 clips → a few thousand runs instead of 12,800 overlapping blocks per paint).
  const auto w = static_cast<size_t>(r.w);
  cells_.assign(w * static_cast<size_t>(oh), 0);
  for (const Block& b : blocks_) {
    const int32_t bx0 = std::clamp(b.x0, 0, r.w), bx1 = std::clamp(b.x1, 0, r.w);
    const int32_t by0 = std::clamp(b.y0, 0, oh), by1 = std::clamp(b.y1, 0, oh);
    for (int32_t y = by0; y < by1; ++y)
      std::fill(cells_.begin() + static_cast<ptrdiff_t>(static_cast<size_t>(y) * w + static_cast<size_t>(bx0)),
                cells_.begin() + static_cast<ptrdiff_t>(static_cast<size_t>(y) * w + static_cast<size_t>(bx1)), b.col);
  }
  blocks_.clear();
  for (int32_t y = 0; y < oh; ++y) {
    const Rgba* row = cells_.data() + static_cast<size_t>(y) * w;
    for (int32_t x = 0; x < r.w;) {
      const Rgba col = row[x];
      int32_t e = x + 1;
      while (e < r.w && row[e] == col) ++e;
      if (col != 0) {
        // Extend the run above when it is identical (tall bands become one block).
        Block* above = nullptr;
        for (size_t i = blocks_.size(); i-- > 0 && blocks_[i].y1 >= y;)
          if (blocks_[i].y1 == y && blocks_[i].x0 == x && blocks_[i].x1 == e && blocks_[i].col == col) {
            above = &blocks_[i];
            break;
          }
        if (above) above->y1 = y + 1;
        else blocks_.push_back({x, e, y, y + 1, col});
      }
      x = e;
    }
  }
}

void Overview::paint(PaintContext& ctx) {
  const Theme* th = ctx.res.theme;
  if (!th) return;
  const IRect r = rect();
  const IRect c = intersect(r, ctx.clip);
  if (c.empty()) return;
  DrawList& dl = ctx.dl;
  const Model& m = view_.model();
  if (!cacheValid_ || cacheDoc_ != m.shown.get() || cacheRows_ != m.stats.rowRebuilds || !(cacheRect_ == r) ||
      !(cacheRange_ == range()))
    rebuildCache(r);
  dl.addRectFilled(r.toRect(), th->arrangement.lane);
  const float ox = static_cast<float>(r.x), oy = static_cast<float>(r.y);
  for (const Block& b : blocks_)
    dl.addRectFilled({ox + static_cast<float>(b.x0), oy + static_cast<float>(b.y0), ox + static_cast<float>(b.x1),
                      oy + static_cast<float>(b.y1)},
                     withAlpha(b.col, 210));
  dl.addRectFilled({static_cast<float>(r.x), static_cast<float>(r.bottom() - 1), static_cast<float>(r.right()),
                    static_cast<float>(r.bottom())},
                   th->arrangement.separator);

  // The visible region: horizontally the lanes' tick range, vertically the scrolled part of the rows.
  const TickRange rg = range();
  const TickRange vis = view_.visibleTicks();
  const double xScale = static_cast<double>(r.w) / static_cast<double>(rg.end - rg.start);
  const float fx0 = std::floor(ox + static_cast<float>(static_cast<double>(vis.start - rg.start) * xScale));
  const float fx1 = std::max(fx0 + 3.0f, std::ceil(ox + static_cast<float>(static_cast<double>(vis.end - rg.start) * xScale)));
  const int32_t oh = r.h - 1;
  float fy0 = oy, fy1 = oy + static_cast<float>(oh);
  if (m.contentHeight > 0) {
    const double sy = static_cast<double>(view_.scrollY());
    const double vh = static_cast<double>(view_.trackScroll().rect().h);
    const double ch = static_cast<double>(m.contentHeight);
    fy0 = oy + static_cast<float>(std::floor(std::clamp(sy / ch, 0.0, 1.0) * oh));
    fy1 = oy + static_cast<float>(std::ceil(std::clamp((sy + vh) / ch, 0.0, 1.0) * oh));
    if (fy1 - fy0 < 3.0f) fy1 = std::min(oy + static_cast<float>(oh), fy0 + 3.0f);
  }
  // Live frames the visible part with light lines above and below (2 px at 100 %) and thin sides.
  const Rect frame{std::max(fx0, ox), fy0, std::min(fx1, static_cast<float>(r.right())), fy1};
  dl.addRectStroke(frame, th->arrangement.overviewFrame);
  const float t2 = static_cast<float>(std::max(1, ctx.scale.px(1.0f)));
  dl.addRectFilled({frame.x0, frame.y0, frame.x1, frame.y0 + 2.0f * t2}, th->arrangement.overviewFrame);
  dl.addRectFilled({frame.x0, frame.y1 - 2.0f * t2, frame.x1, frame.y1}, th->arrangement.overviewFrame);
}

bool Overview::onPointer(const PointerEvent& e) {
  const IRect r = rect();
  auto tickAt = [&](float x) {
    const TickRange rg = range();
    return static_cast<double>(rg.start) +
           (static_cast<double>(x) - r.x) * static_cast<double>(rg.end - rg.start) / std::max(1, r.w);
  };
  switch (e.type) {
    case PointerType::Down: {
      if (e.button != PointerButton::Left) return false;
      const TickRange vis = view_.visibleTicks();
      const double t = tickAt(e.pos.x);
      if (t >= static_cast<double>(vis.start) && t < static_cast<double>(vis.end)) {
        grabOffset_ = t - view_.originTick();
      } else {  // outside the frame: centre the view there, then drag from the centre
        grabOffset_ = static_cast<double>(vis.end - vis.start) * 0.5;
        view_.setTimeView(t - grabOffset_, view_.pxPerTick());
      }
      dragging_ = true;
      return true;
    }
    case PointerType::Move:
      if (!dragging_) return false;
      view_.setTimeView(tickAt(e.pos.x) - grabOffset_, view_.pxPerTick());
      return true;
    case PointerType::Up:
      if (!dragging_) return false;
      dragging_ = false;
      return true;
    default: return false;
  }
}

// --- Corner ---------------------------------------------------------------------------------------------

void Corner::paint(PaintContext& ctx) {
  const Theme* th = ctx.res.theme;
  if (!th) return;
  const IRect r = rect();
  ctx.dl.addRectFilled(r.toRect(), th->arrangement.ruler);
  ctx.dl.addRectFilled({static_cast<float>(r.x), static_cast<float>(r.bottom() - 1), static_cast<float>(r.right()),
                        static_cast<float>(r.bottom())},
                       th->arrangement.separator);
}

}  // namespace pq::views::arrangement
