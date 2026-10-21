// Track headers (T3.8, display only): group stripes and indent, a title bar in the track colour with the name and,
// for groups, the fold triangle. T3.10 puts its mixer controls into mixerSection().

#include <algorithm>
#include <cmath>

#include "../common/paint.h"
#include "internal.h"

namespace pq::views {

using namespace pq::ui;
namespace am = arrangement_metrics;

TrackHeader::TrackHeader(std::string name, ArrangementView& view) : Control(std::move(name)), view_(view) {
  setLayout(LayoutKind::Custom);
  fold_ = &add<arrangement::FoldButton>(view);
  mixer_ = &add<Widget>("mixer");
  mixer_->setLayout(LayoutKind::Row);
  mixer_->setVisible(false);
  setInfo("Track Title Bar", "The track's name and colour. A group's triangle folds or unfolds its tracks.");
}

void TrackHeader::bind(const ArrangementRow& row, std::span<const ArrangementRow> rows) {
  const doc::Track& t = *row.track;
  bool changed = track_ != row.track || id_ != row.id || folded_ != row.folded || depth_ != row.depth;
  track_ = row.track;
  id_ = row.id;
  group_ = row.group;
  folded_ = row.folded;
  depth_ = row.depth;
  if (name_ != t.name) {
    name_ = t.name;
    changed = true;
  }
  if (colour_ != t.colour.rgb || active_ != t.active) changed = true;
  colour_ = t.colour.rgb;
  active_ = t.active;
  ancestorColours_.clear();
  for (int32_t p = row.parent; p >= 0; p = rows[static_cast<size_t>(p)].parent)
    ancestorColours_.insert(ancestorColours_.begin(), rows[static_cast<size_t>(p)].track->colour.rgb);
  fold_->setVisible(group_);
  static_cast<arrangement::FoldButton*>(fold_)->setTrack(id_, folded_);
  if (changed) {
    invalidateArrange();
    markDirty();
  }
}

Rgba TrackHeader::titleColour() const noexcept { return active_ ? hex(colour_) : arrangement::inactiveColour(); }

IRect TrackHeader::titleRect() const noexcept {
  const IRect r = rect();
  const UiScale s = tree() ? tree()->uiScale() : UiScale{};
  const int32_t indent = s.px(am::kGroupIndent * static_cast<float>(depth_));
  const int32_t mix = mixer_->visible() ? s.px(view_.headerMixerWidth()) : 0;
  const int32_t h = std::min(std::max(0, r.h - 1), s.px(am::kTitleHeight));
  return {r.x + indent, r.y, std::max(0, r.w - indent - mix), h};
}

void TrackHeader::arrange(LayoutPass& pass) {
  const IRect r = rect();
  const UiScale s(pass.scale());
  const float mixW = view_.headerMixerWidth();
  mixer_->setVisible(mixW > 0.0f);
  const IRect t = titleRect();
  if (fold_->visible()) pass.place(*fold_, {t.x, t.y, std::min(t.w, s.px(18.0f)), t.h});
  if (mixer_->visible()) {
    const int32_t mw = std::min(r.w, s.px(mixW));
    pass.place(*mixer_, {r.right() - mw, r.y, mw, std::max(0, r.h - 1)});
  }
}

void TrackHeader::paint(PaintContext& ctx) {
  const Theme* thp = ctx.res.theme;
  if (!thp) return;
  const Theme& th = *thp;
  const IRect r = rect();
  DrawList& dl = ctx.dl;
  const float x0 = static_cast<float>(r.x), y0 = static_cast<float>(r.y);
  const float x1 = static_cast<float>(r.right()), y1 = static_cast<float>(r.bottom());
  dl.addRectFilled({x0, y0, x1, y1 - 1.0f}, th.arrangement.header);
  dl.addRectFilled({x0, y1 - 1.0f, x1, y1}, th.arrangement.separator);
  // Group stripes: one per enclosing group, in its colour, filling the indent (Live marks grouped tracks so).
  const float indent = static_cast<float>(ctx.scale.px(am::kGroupIndent));
  for (size_t i = 0; i < ancestorColours_.size(); ++i) {
    const float sx = x0 + indent * static_cast<float>(i);
    dl.addRectFilled({sx, y0, sx + indent - 1.0f, y1 - 1.0f}, lerp(hex(ancestorColours_[i]), th.arrangement.header, 0.35f));
  }
  const IRect t = titleRect();
  if (t.empty()) return;
  const Rgba fill = titleColour();
  dl.addRectFilled(t.toRect(), fill);
  const Rgba text = view_.model().clipTextColour(th, fill);
  if (const ThemeFonts* f = ctx.res.uiFonts) {
    const int32_t padL = group_ ? ctx.scale.px(18.0f) : ctx.scale.px(6.0f);
    paint::text(ctx, f->bodyStrong, t, name_, text, TextAlign::Left, padL, ctx.scale.px(4.0f));
  }
}

bool TrackHeader::onPointer(const PointerEvent& e) {
  // Double-click on a group's title folds it (as the triangle does); selection and renaming are T3.9/T3.10's.
  if (e.type == PointerType::Down && e.button == PointerButton::Left && e.clickCount == 2 && group_ &&
      titleRect().contains(e.pos)) {
    view_.toggleFold(id_);
    return true;
  }
  return false;
}

namespace arrangement {

FoldButton::FoldButton(ArrangementView& view) : Control("fold"), view_(view) {
  setInfo("Fold / Unfold Group", "Shows or hides the tracks inside this group.");
}

void FoldButton::setTrack(doc::TrackId id, bool folded) {
  id_ = id;
  if (folded != folded_) markDirty();
  folded_ = folded;
}

void FoldButton::paint(PaintContext& ctx) {
  const IRect r = rect();
  if (r.empty()) return;
  const float cx = static_cast<float>(r.x) + static_cast<float>(r.w) * 0.5f;
  const float cy = static_cast<float>(r.y) + static_cast<float>(r.h) * 0.5f;
  const float a = std::round(static_cast<float>(ctx.scale.px(4.0f)));
  if (!ctx.res.theme) return;
  // Drawn in the title text's colour (the parent header's title bar is under it).
  const auto* header = static_cast<const TrackHeader*>(parent());
  const Rgba col = view_.model().clipTextColour(*ctx.res.theme, header->titleColour());
  const Rgba c = hovered() ? lerp(col, rgba(255, 255, 255), 0.25f) : col;
  if (folded_)  // ▶
    ctx.dl.addTriangleFilled({cx - a * 0.6f, cy - a}, {cx + a * 0.8f, cy}, {cx - a * 0.6f, cy + a}, c);
  else  // ▼
    ctx.dl.addTriangleFilled({cx - a, cy - a * 0.6f}, {cx + a, cy - a * 0.6f}, {cx, cy + a * 0.8f}, c);
}

bool FoldButton::onPointer(const PointerEvent& e) {
  if (e.type == PointerType::Down && e.button == PointerButton::Left) {
    view_.toggleFold(id_);
    return true;
  }
  return e.type == PointerType::Up && e.button == PointerButton::Left;
}

}  // namespace arrangement

}  // namespace pq::views
