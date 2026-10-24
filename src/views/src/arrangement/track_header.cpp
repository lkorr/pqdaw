// Track headers (T3.8 display, T3.10 interaction): group stripes and indent, a title bar in the track colour with the
// name and, for groups, the fold triangle; the mixer section (T3.10's MixerStrip, added by installMixer) on the
// right. A click selects the track, the bottom edge resizes the row, right-click opens the track menu, Ctrl+R renames
// the title in place.

#include <algorithm>
#include <cmath>

#include "../common/paint.h"
#include "internal.h"
#include "pq/document/edit_ops.h"

namespace pq::views {

using namespace pq::ui;
namespace am = arrangement_metrics;

TrackHeader::TrackHeader(std::string name, ArrangementView& view) : Control(std::move(name)), view_(view) {
  setLayout(LayoutKind::Custom);
  fold_ = &add<arrangement::FoldButton>(view);
  mixer_ = &add<Widget>("mixer");
  mixer_->setLayout(LayoutKind::Row);
  mixer_->setVisible(false);
  chip_ = &add<Widget>("chip");
  chip_->setLayout(LayoutKind::Stack);
  // T3.15: automation-mode parameter chooser and the lanes toggle (hidden until needed).
  chooser_ = &add<arrangement::ParamChooser>(view);
  chooser_->setVisible(false);
  lanesToggle_ = &add<arrangement::LanesToggle>(view);
  lanesToggle_->setVisible(false);
  setInfo("Track Title Bar",
          "Click to select the track (Shift adds, Ctrl toggles); drag its bottom edge to resize it; right-click for "
          "rename, insert, group and colour. A group's triangle folds or unfolds its tracks.");
}

TrackHeader::~TrackHeader() = default;

void TrackHeader::bind(const ArrangementRow& row, std::span<const ArrangementRow> rows) {
  const doc::Track& t = *row.track;
  bool changed = track_ != row.track || id_ != row.id || folded_ != row.folded || depth_ != row.depth;
  if (id_ != row.id && rename_) endRename(false);  // the row now shows another track
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
  // Group stripes: outermost first. Compared, so a group's colour change repaints its members' headers.
  size_t n = 0;
  for (int32_t p = row.parent; p >= 0; p = rows[static_cast<size_t>(p)].parent) ++n;
  if (ancestorColours_.size() != n) {
    ancestorColours_.assign(n, 0);
    changed = true;
  }
  size_t i = n;
  for (int32_t p = row.parent; p >= 0; p = rows[static_cast<size_t>(p)].parent) {
    const uint32_t c = rows[static_cast<size_t>(p)].track->colour.rgb;
    if (ancestorColours_[--i] != c) {
      ancestorColours_[i] = c;
      changed = true;
    }
  }
  const bool sel = view_.editor().trackSelected(id_);
  if (sel != selected_) {
    selected_ = sel;
    changed = true;
  }
  fold_->setVisible(group_);
  static_cast<arrangement::FoldButton*>(fold_)->setTrack(id_, folded_);
  // T3.15: the parameter chooser (automation mode) and the lanes toggle (automation mode, or lanes shown; not for
  // the pinned returns / master, whose envelopes show only over their rows).
  const ArrangementAutomation& au = view_.automation();
  const bool lanes = !row.bottom && au.lanesShown(id_);
  const bool toggle = !row.bottom && (au.mode() || lanes);
  if (bottomRow_ != row.bottom || lanesToggle_->visible() != toggle || (au.mode() != chooserWanted_)) changed = true;
  bottomRow_ = row.bottom;
  chooserWanted_ = au.mode();
  lanesToggle_->setVisible(toggle);
  static_cast<arrangement::LanesToggle*>(lanesToggle_)->set(id_, lanes);
  if (au.mode()) {
    const doc::ParamRef r = au.chosen(id_);
    const doc::Project& p = *view_.shown();
    const bool automated = view_.model().lane(r).lane != nullptr;
    static_cast<arrangement::ParamChooser*>(chooser_)->set(id_, paramShortName(p, r), automated, view_.context().automationOverridden(r));
  }
  if (changed) {
    invalidateArrange();
    markDirty();
  }
}

void TrackHeader::syncSelected() {
  const bool sel = id_.valid() && view_.editor().trackSelected(id_);
  if (sel == selected_) return;
  selected_ = sel;
  markDirty();
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
  if (mixer_->visible() != (mixW > 0.0f)) mixer_->setVisible(mixW > 0.0f);
  const IRect t = titleRect();
  if (fold_->visible()) pass.place(*fold_, {t.x, t.y, std::min(t.w, s.px(18.0f)), t.h});
  if (lanesToggle_->visible()) pass.place(*lanesToggle_, {t.right() - std::min(t.w, s.px(16.0f)), t.y, std::min(t.w, s.px(16.0f)), t.h});
  if (mixer_->visible()) {
    const int32_t mw = std::min(r.w, s.px(mixW));
    pass.place(*mixer_, {r.right() - mw, r.y, mw, std::max(0, r.h - 1)});
  }
  // The chip section: the name column's second line (Live leaves it empty), when the row is tall enough.
  const int32_t cy = t.bottom() + s.px(3.0f), ch = s.px(am::kChipHeight);
  const int32_t cx = t.x + (group_ ? s.px(18.0f) : s.px(6.0f));
  const int32_t cw = std::max(0, std::min(s.px(am::kChipWidth), t.right() - s.px(4.0f) - cx));
  // T3.15: in automation mode the parameter chooser goes under the chip, or takes the chip's line when the row has
  // room for only one of them (Live's header shows its choosers there).
  const int32_t qh = s.px(automation_metrics::kChooserHeight);
  const bool lineFits = cy + ch <= r.bottom() - 1 && t.w > s.px(12.0f);
  const bool chooser = chooserWanted_ && lineFits;
  const bool both = chooser && cy + ch + s.px(3.0f) + qh <= r.bottom() - 1;
  const bool chipFits = lineFits && (!chooser || both);
  if (chip_->visible() != chipFits) chip_->setVisible(chipFits);
  if (chipFits) pass.place(*chip_, {cx, cy, cw, ch});
  if (chooser_->visible() != chooser) chooser_->setVisible(chooser);
  if (chooser) pass.place(*chooser_, {cx, both ? cy + ch + s.px(3.0f) : cy, std::max(cw, std::min(s.px(am::kChipWidth), t.right() - s.px(4.0f) - cx)), qh});
  if (rename_) {
    const int32_t padL = group_ ? s.px(18.0f) : s.px(2.0f);
    pass.place(*rename_, {t.x + padL, t.y + 1, std::max(0, t.w - padL - s.px(2.0f)), std::max(0, t.h - 2)});
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
  // Selected tracks' headers are lighter (Live).
  dl.addRectFilled({x0, y0, x1, y1 - 1.0f}, selected_ ? th.mixer.selected : th.arrangement.header);
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
  if (rename_) return;  // the text field draws the name
  const Rgba text = view_.model().clipTextColour(th, fill);
  if (const ThemeFonts* f = ctx.res.uiFonts) {
    const int32_t padL = group_ ? ctx.scale.px(18.0f) : ctx.scale.px(6.0f);
    const int32_t padR = lanesToggle_->visible() ? ctx.scale.px(18.0f) : ctx.scale.px(4.0f);  // T3.15: the lanes toggle
    paint::text(ctx, f->bodyStrong, t, name_, text, TextAlign::Left, padL, padR);
  }
}

bool TrackHeader::onPointer(const PointerEvent& e) {
  const IRect r = rect();
  const float scale = tree() ? tree()->scale() : 1.0f;
  // Height resize: the bottom edge (Live drags the track's lower border).
  if (resizing_) {
    if (e.type == PointerType::Move) {
      view_.setTrackHeight(id_, resizeStartH_ + (e.pos.y - resizeStartY_) / scale);
      return true;
    }
    if (e.type == PointerType::Up && e.button == PointerButton::Left) {
      resizing_ = false;
      return true;
    }
  }
  if (e.type == PointerType::Down && e.button == PointerButton::Left &&
      e.pos.y >= static_cast<float>(r.bottom()) - std::max(2.0f, am::kResizeGrab * scale) - 1.0f) {
    resizing_ = true;
    resizeStartY_ = e.pos.y;
    resizeStartH_ = view_.trackHeight(id_);
    return true;
  }
  // Right-click: the track menu (the track is selected first unless it already is).
  if (e.type == PointerType::Down && e.button == PointerButton::Right) {
    if (!view_.editor().trackSelected(id_)) view_.selectTrack(id_, mod::kNone);
    (void)view_.openTrackMenu(id_, e.pos);
    return true;
  }
  // Double-click on a group's title folds it (as the triangle does).
  if (e.type == PointerType::Down && e.button == PointerButton::Left && e.clickCount == 2 && group_ &&
      titleRect().contains(e.pos)) {
    view_.toggleFold(id_);
    return true;
  }
  // A click selects the track (Shift adds, Ctrl toggles): `0` then toggles its activator (T3.9, Q15).
  if (e.type == PointerType::Down && e.button == PointerButton::Left) {
    view_.selectTrack(id_, e.mods);
    return true;
  }
  return e.type == PointerType::Up && (e.button == PointerButton::Left || e.button == PointerButton::Right);
}

// --- rename (Ctrl+R) ------------------------------------------------------------------------------------------------------

void TrackHeader::beginRename() {
  if (rename_ || !track_) return;
  rename_ = &add<TextEdit>("rename", name_);
  renameDone_ = false;
  rename_->buffer().selectAll();
  rename_->onCommit = [this](const std::string&) { endRename(true); };
  rename_->onCancel = [this] { endRename(false); };
  invalidateArrange();
  markDirty();
  if (UiTree* t = tree()) {
    t->layoutNow();
    t->setFocus(rename_, true);
  }
}

void TrackHeader::endRename(bool commit) {
  if (!rename_ || renameDone_) return;
  renameDone_ = true;
  const std::string text = rename_->text();
  const doc::TrackId id = id_;
  // Removed at the next frame: this may run inside the field's own callback.
  requestFrame();
  UiTree* t = tree();
  if (t && t->focus() == rename_) t->setFocus(&view_);
  if (commit && !text.empty() && text != name_) {
    ViewContext& ctx = view_.context();
    const auto e = doc::ops::renameTrack(*ctx.doc(), id, text);
    if (!e) ctx.setStatus("Rename: " + e.error().message);
    else if (Status s = ctx.store().dispatch(e->command); !s) ctx.setStatus("Rename: " + s.error().message);
  }
}

void TrackHeader::onFrame(double) {
  if (rename_ && renameDone_) {
    (void)removeChild(*rename_);
    rename_ = nullptr;
    renameDone_ = false;
    markDirty();
  }
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
