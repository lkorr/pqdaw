// Macro mode's overlay, key labels and lamp (T8.1): see macro_overlay.h.

#include "macro_overlay.h"

#include <algorithm>
#include <cmath>

#include "../common/labels.h"
#include "../common/paint.h"
#include "../piano_roll/roll_widgets.h"
#include "pq/views/macro/macro_mode.h"

namespace pq::views {

using namespace pq::ui;

namespace {

// The cursor's box: one note long from the cursor's tick, on the band of the row nearest its pitch.
IRect cursorBox(const roll::RollState& s, const MacroMode& m, const MacroMode::Target& t) {
  const roll::CentsPlacement pl = s.rows().placeCents(t.cents);
  const int32_t x0 = static_cast<int32_t>(std::round(s.xOfTick(static_cast<double>(m.tick()))));
  const int32_t x1 = std::max(x0 + 6, static_cast<int32_t>(std::round(s.xOfTick(static_cast<double>(m.tick() + m.noteLength())))));
  int32_t y0 = s.screenY(pl.top), y1 = s.screenY(pl.bottom);
  if (y1 - y0 < 8) {  // flat rows: still a box the eye finds
    const int32_t c = s.screenY(pl.centre);
    y0 = c - 4;
    y1 = c + 4;
  }
  return {x0, y0, x1 - x0, y1 - y0};
}

}  // namespace

// --- MacroOverlay -------------------------------------------------------------------------------------------

MacroOverlay::MacroOverlay(roll::RollState& s) : Widget("macro-overlay"), s_(s) { setHittable(false); }

void MacroOverlay::onFrame(double) { (void)s_.ctx.macro().sync(); }

IRect MacroOverlay::cursorRect() const {
  const MacroMode& m = s_.ctx.macro();
  if (!m.active() || s_.rows().empty()) return {};
  const MacroMode::Target t = m.target();
  return t.ok ? intersect(cursorBox(s_, m, t), rect()) : IRect{};
}

void MacroOverlay::paint(PaintContext& ctx) {
  MacroMode& m = s_.ctx.macro();
  const Theme* th = ctx.res.theme;
  if (!m.active()) return;
  if (m.syncPending()) requestFrame();  // the selection changed without an event of ours: adopt it next frame
  if (!th || s_.rows().empty()) return;
  const IRect r = rect();
  DrawList& dl = ctx.dl;
  dl.pushClip(r.toRect());
  const Rgba accent = th->accent.primary;
  const float px = std::max(1.0f, ctx.scale.exact(1.0f));
  const float fx0 = static_cast<float>(r.x), fx1 = static_cast<float>(r.right());
  const float fy0 = static_cast<float>(r.y), fy1 = static_cast<float>(r.bottom());

  const MacroMode::Target t = m.target();
  const std::optional<double> anchorCents = m.anchorCents();
  IRect box{};
  if (t.ok) {
    box = cursorBox(s_, m, t);
    const float cy = s_.yOfCents(t.cents);
    const float bx0 = static_cast<float>(box.x);
    // Where the next note lands: its pitch across the view, its start down the view.
    dl.addRectFilled({fx0, std::round(cy) - 0.5f * px, fx1, std::round(cy) + 0.5f * px}, withAlpha(accent, 110));
    dl.addRectFilled({bx0 - 0.5f * px, fy0, bx0 + 0.5f * px, fy1}, withAlpha(accent, 70));
    // The anchor: a ring on its pitch at its start, the anchor note outlined, a connector to the cursor's pitch.
    if (anchorCents) {
      const doc::Note* an = m.anchorNote().valid() ? s_.findNote(m.anchorNote()) : nullptr;
      const float ay = s_.yOfCents(*anchorCents);
      const float ax = an ? s_.xOfTick(static_cast<double>(an->start)) : bx0;
      const Rgba ink = th->text.primary;
      if (an) {
        const IRect nr = s_.screenRectOf(an->start, an->length, an->pitch);
        if (!nr.empty()) dl.addRectStroke(paint::inset(nr, -ctx.scale.px(2)).toRect(), withAlpha(ink, 200), px);
      }
      if (std::fabs(ay - cy) > 1.0f) {  // dotted: the interval being composed
        const float lo = std::min(ay, cy), hi = std::max(ay, cy), dash = 3.0f * px;
        for (float y = lo; y < hi; y += 2.0f * dash) dl.addRectFilled({bx0 - px, y, bx0 + px, std::min(hi, y + dash)}, withAlpha(ink, 170));
      }
      const float rad = ctx.scale.exact(5.0f);
      dl.addCircleFilled({ax, ay}, rad, withAlpha(th->surface.background, 230));
      dl.addCircleStroke({ax, ay}, rad, ink, 1.5f * px);
      dl.addCircleFilled({ax, ay}, rad * 0.4f, ink);
    }
    // The cursor box.
    dl.addRectFilled(box.toRect(), withAlpha(accent, 90));
    dl.addRectStroke(box.toRect(), accent, 2.0f * px);
  }

  // The frame and the tab: macro mode is on.
  dl.addRectStroke({fx0, fy0, fx1, fy1}, withAlpha(accent, 220), 2.0f * px);
  TextSystem* ts = ctx.res.text;
  const ThemeFonts* fonts = ctx.res.uiFonts;
  if (ts && fonts && fonts->small != kNoFont) {
    const FontId font = fonts->small;
    const int32_t lineH = ts->metrics(font).lineHeight;
    const int32_t pad = ctx.scale.px(6);
    std::string tab = m.lampText();
    if (m.locked()) tab += " \xC2\xB7 harmonic lock";
    const IRect chip{r.x, r.y, std::min(r.w, ts->measure(font, tab) + 2 * pad), lineH + ctx.scale.px(4)};
    dl.addRectFilled(chip.toRect(), accent);
    paint::text(ctx, font, chip, tab, th->text.onAccent, TextAlign::Left, pad, pad);

    // The label beside the cursor: the composition and what it becomes.
    if (t.ok) {
      const std::string l1 = m.composing() || m.lockPending() ? m.compositionText() : std::string();
      const std::string l2 = m.detailText();
      const int lines = (l1.empty() ? 0 : 1) + (l2.empty() ? 0 : 1);
      if (lines > 0) {
        const int32_t w = std::max(l1.empty() ? 0 : ts->measure(font, l1), l2.empty() ? 0 : ts->measure(font, l2)) + 2 * pad;
        const int32_t h = lines * lineH + ctx.scale.px(6);
        const int32_t gap = ctx.scale.px(8);
        int32_t x = box.right() + gap;
        if (x + w > r.right() - gap) x = box.x - gap - w;  // no room on the right: on the left of the cursor
        x = std::clamp(x, r.x + gap, std::max(r.x + gap, r.right() - gap - w));
        int32_t y = box.y + box.h / 2 - h / 2;
        y = std::clamp(y, r.y + chip.h + ctx.scale.px(2), std::max(r.y + chip.h + ctx.scale.px(2), r.bottom() - gap - h));
        const IRect lab{x, y, w, h};
        paint::box(dl, lab, ctx.scale.exact(th->radius.control), withAlpha(th->surface.background, 240), accent);
        int32_t ly = lab.y + ctx.scale.px(3);
        if (!l1.empty()) {
          paint::text(ctx, font, {lab.x, ly, lab.w, lineH}, l1, th->text.primary, TextAlign::Left, pad, pad);
          ly += lineH;
        }
        if (!l2.empty())
          paint::text(ctx, font, {lab.x, ly, lab.w, lineH}, l2, l1.empty() ? th->text.primary : accent, TextAlign::Left, pad, pad);
      }
    }
  }
  dl.popClip();
}

// --- MacroKeyLabels -----------------------------------------------------------------------------------------

MacroKeyLabels::MacroKeyLabels(roll::RollState& s) : Widget("macro-keys"), s_(s) { setHittable(false); }

void MacroKeyLabels::paint(PaintContext& ctx) {
  MacroMode& m = s_.ctx.macro();
  const Theme* th = ctx.res.theme;
  const roll::RowModel& rows = s_.rows();
  if (!th || !m.active() || rows.empty()) return;
  const IRect r = rect();
  const IRect vis = intersect(r, ctx.clip);
  if (vis.empty()) return;
  DrawList& dl = ctx.dl;
  dl.pushClip(r.toRect());
  const Rgba accent = th->accent.primary;
  const MacroMode::Target t = m.target();
  // The cursor's row: a bar across the keys.
  if (t.ok) {
    const roll::CentsPlacement pl = rows.placeCents(t.cents);
    const float y0 = static_cast<float>(s_.screenY(pl.top)), y1 = std::max(static_cast<float>(s_.screenY(pl.bottom)), y0 + 2.0f);
    dl.addRectFilled({static_cast<float>(r.x), y0, static_cast<float>(r.right()), y1}, withAlpha(accent, 120));
    dl.addRectFilled({static_cast<float>(r.right() - ctx.scale.px(4)), y0, static_cast<float>(r.right()), y1}, accent);
  }
  // Fewest keys from the anchor to each row.
  TextSystem* ts = ctx.res.text;
  const ThemeFonts* fonts = ctx.res.uiFonts;
  if (ts && fonts && fonts->small != kNoFont && m.keypressMap()) {
    const FontId font = fonts->small;
    const int32_t lineH = ts->metrics(font).lineHeight;
    const int32_t pad = ctx.scale.px(2);
    const int64_t anchorRow = m.anchorRow();
    paint::LabelSpace space(1);
    const auto [first, last] = rows.rowsIn(s_.contentY(static_cast<float>(vis.y)), s_.contentY(static_cast<float>(vis.bottom())) + 1);
    const auto draw = [&](size_t i) {
      const roll::PitchRow& row = rows.row(i);
      const std::string_view label = m.keypressLabel(row.degree - anchorRow);
      if (label.empty()) return;
      const int32_t top = s_.screenY(row.centre) - lineH / 2;
      if (!space.place(top, top + lineH)) return;
      const int32_t w = std::min(r.w - 2 * pad, ts->measure(font, label) + 2 * pad);
      const IRect chip{r.x + pad, top, w, lineH};
      dl.addRoundedRectFilled(chip.toRect(), ctx.scale.exact(2.0f), withAlpha(th->surface.background, 225));
      paint::text(ctx, font, chip, label, accent, TextAlign::Left, pad, pad);
    };
    // From the anchor's row outwards, so the nearest rows keep their labels where rows are too flat for all of them.
    const int64_t at = rows.indexOfDegree(anchorRow);
    const int64_t lo = static_cast<int64_t>(first), hi = static_cast<int64_t>(last);
    if (at < 0) {
      for (size_t i = first; i < last; ++i) draw(i);
    } else {
      for (int64_t d = 1; at - d >= lo || at + d < hi; ++d) {
        if (at - d >= lo && at - d < hi) draw(static_cast<size_t>(at - d));
        if (at + d >= lo && at + d < hi) draw(static_cast<size_t>(at + d));
      }
    }
  }
  dl.popClip();
}

// --- MacroLamp ----------------------------------------------------------------------------------------------

MacroLamp::MacroLamp(ViewContext& ctx) : Control("macro"), ctx_(ctx) {
  setFocusable(false);
  setCursor(ui::Cursor::Pointer);
  setInfo("Macro Mode",
          "Enters notes into the open note clip from the computer keyboard: the number row multiplies by 2/1 3/2 5/4 7/4 "
          "11/8 \xE2\x80\xA6" " and Q to P by their reciprocals, the key left of 1 places a note at the cursor, Tab places "
          "and continues from it. In the tempered layer notes go to the tuning's nearest step; from a JI note, and in the "
          "JI layer, they are exact ratios. Click to switch it on and off; the ? in the note editor lists every key.");
  refresh();
}

void MacroLamp::refresh() {
  const MacroMode& m = ctx_.macro();
  std::string text = m.lampText();
  const bool on = m.on();
  if (text == text_ && on == on_) return;
  text_ = std::move(text);
  on_ = on;
  setTooltip(on ? "Macro mode is on: click to turn it off" : "Macro mode: interval entry from the keyboard");
  markDirty();
}

bool MacroLamp::onPointer(const PointerEvent& e) {
  if (e.button != PointerButton::Left) return false;
  if (e.type == PointerType::Down) {
    setPressed(true);
    ctx_.macro().toggle();
    return true;
  }
  if (e.type == PointerType::Up) {
    setPressed(false);
    return true;
  }
  return false;
}

void MacroLamp::paint(PaintContext& ctx) {
  const Theme* th = ctx.res.theme;
  if (!th) return;
  const VisualState v = visualState();
  const IRect r = rect();
  Rgba fill = on_ ? th->accent.primary : th->surface.panel;
  if (v.pressed) fill = on_ ? paint::pressOf(fill) : th->control.fill;
  else if (v.hover) fill = on_ ? paint::hoverOf(fill) : th->surface.raised;
  paint::box(ctx.dl, r, ctx.scale.exact(th->radius.control), fill, th->control.border);
  const ThemeFonts* fonts = ctx.res.uiFonts;
  if (!fonts) return;
  paint::text(ctx, fonts->small, r, text_, on_ ? th->text.onAccent : th->text.secondary, TextAlign::Center, ctx.scale.px(4), ctx.scale.px(4));
}

}  // namespace pq::views
