// AC.look: the audio Clip View's box controls in the device panels' look; see pq/views/audio_clip/clip_controls.h.

#include "pq/views/audio_clip/clip_controls.h"

#include <algorithm>
#include <cmath>

#include "../common/paint.h"
#include "pq/ui/lines.h"
#include "pq/ui/theme.h"

namespace pq::views::audio_clip {

using namespace pq::ui;

namespace {

Rgba chipFill(const Theme& th) noexcept { return lerp(th.face.body, th.face.track, 0.6f); }

Vec2 polar(Vec2 c, float r, float a) noexcept { return {c.x + r * std::cos(a), c.y + r * std::sin(a)}; }

}  // namespace

// ---- Section ------------------------------------------------------------------------------------------------------

Section::Section(std::string name, std::string title) : Widget(std::move(name)), title_(std::move(title)) {
  setPadding({8, static_cast<int16_t>(kTitleH), 8, 7});
}

void Section::setTitle(std::string title) {
  if (title == title_) return;
  title_ = std::move(title);
  markDirty();
}

void Section::paint(PaintContext& ctx) {
  const Theme* th = ctx.res.theme;
  if (!th) return;
  const IRect r = rect();
  ctx.dl.addRoundedRectFilled(r.toRect(), ctx.scale.exact(4.0f), th->face.body);
  if (const ThemeFonts* f = ctx.res.uiFonts; f && !title_.empty())
    paint::text(ctx, f->small, {r.x, r.y + ctx.scale.px(1.0f), r.w, ctx.scale.px(kTitleH)}, title_, th->face.ink, TextAlign::Left,
                ctx.scale.px(8.0f), ctx.scale.px(8.0f));
}

// ---- Caption ------------------------------------------------------------------------------------------------------

Caption::Caption(std::string name, std::string text) : Widget(std::move(name)), text_(std::move(text)) {}

void Caption::paint(PaintContext& ctx) {
  const Theme* th = ctx.res.theme;
  const ThemeFonts* f = ctx.res.uiFonts;
  if (th && f) paint::text(ctx, f->small, rect(), text_, th->face.ink, TextAlign::Left, 0, 0);
}

// ---- ClipKnob -----------------------------------------------------------------------------------------------------

ClipKnob::ClipKnob(std::string name, std::string label, ValueSpec spec) : Knob(std::move(name), std::move(spec)), label_(std::move(label)) {}

void ClipKnob::setReadout(std::string text) {
  if (text == readout_) return;
  readout_ = std::move(text);
  markDirty();
}

IRect ClipKnob::dialRect() const noexcept {
  const IRect r = rect();
  const float s = tree() ? tree()->scale() : 1.0f;
  const int32_t lh = static_cast<int32_t>(std::lround(kLabelH * s)), rh = static_cast<int32_t>(std::lround(kReadoutH * s));
  const int32_t h = std::max(0, r.h - lh - rh);
  const int32_t d = std::min(r.w, h);
  return {r.x + (r.w - d) / 2, r.y + lh + (h - d) / 2, d, d};
}

void ClipKnob::paint(PaintContext& ctx) {
  const Theme* th = ctx.res.theme;
  if (!th) return;
  const VisualState v = visualState();
  const IRect r = rect(), dial = dialRect();
  const float s = ctx.scale.factor;
  const Vec2 c{static_cast<float>(dial.x) + 0.5f * static_cast<float>(dial.w), static_cast<float>(dial.y) + 0.5f * static_cast<float>(dial.h)};
  // The panel knob's proportions (elements.cpp Knob, pointer "arc"): a 3-unit track, end ticks just outside it.
  const float track = 3.0f * s;
  const float rad = 0.5f * static_cast<float>(dial.w) - 4.0f * s - 0.5f * track;
  const bool hot = (v.hover || v.pressed) && !v.disabled;
  if (rad >= 3.0f) {
    const float a0 = kStartAngle, a1 = kStartAngle + kSweep;
    const float av = a0 + kSweep * static_cast<float>(normalized());
    float anchor = a0;  // bipolar ranges grow the arc from 0
    if (spec_.min < 0.0 && spec_.max > 0.0) anchor = a0 + kSweep * static_cast<float>((0.0 - spec_.min) / (spec_.max - spec_.min));
    const Rgba value = v.disabled ? lerp(th->face.track, th->face.value, 0.35f) : th->face.value;
    if (hot) ctx.dl.addCircleStroke(c, rad, th->face.hover, track + 2.0f * s);
    for (const float a : {a0, a1}) {
      const Vec2 seg[2] = {polar(c, rad + 0.5f * track + 1.0f * s, a), polar(c, rad + 0.5f * track + 3.5f * s, a)};
      smoothPolyline(ctx.dl, seg, 2, th->face.tick, std::max(1.0f, s));
    }
    ctx.dl.addArcStroke(c, rad, a0, a1, th->face.track, track, LineCap::Round);
    if (std::fabs(av - anchor) > 1e-3f) ctx.dl.addArcStroke(c, rad, std::min(anchor, av), std::max(anchor, av), value, track, LineCap::Round);
    ctx.dl.addCircleFilled(c, std::max(1.0f, rad - track - 1.5f * s), lerp(th->face.body, th->face.track, 0.5f));
    const Vec2 needle[2] = {polar(c, rad * 0.25f, av), polar(c, rad - 0.5f * track - 1.0f * s, av)};
    smoothPolyline(ctx.dl, needle, 2, v.disabled ? th->face.tick : th->face.handle, (hot ? 2.0f : 1.6f) * s);
  }
  const ThemeFonts* f = ctx.res.uiFonts;
  if (!f) return;
  const int32_t lh = ctx.scale.px(kLabelH), rh = ctx.scale.px(kReadoutH);
  paint::text(ctx, f->small, {r.x, r.y, r.w, lh}, label_, v.disabled ? withAlpha(th->face.ink, 150) : th->face.ink, TextAlign::Center, 1, 1);
  if (!typingIn())
    paint::text(ctx, f->numeric, {r.x, r.bottom() - rh, r.w, rh}, readout(), v.disabled ? withAlpha(th->face.ink, 170) : th->face.value,
                TextAlign::Center, 1, 1);
  if (v.focus && !v.disabled) ctx.dl.addRoundedRectStroke(r.toRect(), ctx.scale.exact(4.0f), withAlpha(th->accent.focus, 120));
}

void ClipKnob::paintOver(PaintContext& ctx) {
  const IRect r = rect();
  const int32_t rh = ctx.scale.px(kReadoutH);
  paintTypeIn(ctx, {r.x + 1, r.bottom() - rh - 2, std::max(0, r.w - 2), rh + 2});
}

// ---- Chip ---------------------------------------------------------------------------------------------------------

Chip::Chip(std::string name, std::string text) : Control(std::move(name)), text_(std::move(text)) { setCursor(Cursor::Pointer); }

void Chip::setOn(bool on) {
  if (on == on_) return;
  on_ = on;
  markDirty();
}

void Chip::paint(PaintContext& ctx) {
  const Theme* th = ctx.res.theme;
  if (!th) return;
  const VisualState v = visualState();
  Rgba fill, ink;
  if (on_) {
    fill = v.disabled ? lerp(th->face.track, th->face.value, 0.35f) : v.pressed ? paint::pressOf(th->face.value) : v.hover ? paint::hoverOf(th->face.value) : th->face.value;
    ink = th->face.body;
  } else {
    const Rgba base = chipFill(*th);
    fill = v.disabled ? lerp(th->face.body, th->face.track, 0.35f) : v.pressed ? th->face.track : v.hover ? lerp(base, th->face.hover, 0.7f) : base;
    ink = v.disabled ? withAlpha(th->face.ink, 120) : v.hover ? th->text.secondary : th->face.ink;
  }
  const IRect r = rect();
  ctx.dl.addRoundedRectFilled(r.toRect(), ctx.scale.exact(3.0f), fill);
  if (const ThemeFonts* f = ctx.res.uiFonts) paint::text(ctx, f->small, r, text_, ink, TextAlign::Center, ctx.scale.px(2.0f), ctx.scale.px(2.0f));
}

bool Chip::onPointer(const PointerEvent& e) {
  if (e.button != PointerButton::Left) return false;
  if (e.type == PointerType::Down) {
    setPressed(true);
    if (onPress) onPress();  // as Live's choosers and switches: on press
    return true;
  }
  if (e.type == PointerType::Up && captured()) {
    setPressed(false);
    return true;
  }
  return false;
}

// ---- FillBox ------------------------------------------------------------------------------------------------------

FillBox::FillBox(std::string name, std::string label, ValueSpec spec, bool fill)
    : DragBox(std::move(name), std::move(spec), TextAlign::Right), label_(std::move(label)), fill_(fill) {}

void FillBox::paint(PaintContext& ctx) {
  if (editing()) {  // the type-in: the field look of every number box while typing
    DragBox::paint(ctx);
    return;
  }
  const Theme* th = ctx.res.theme;
  if (!th) return;
  const VisualState v = visualState();
  const IRect r = rect();
  const Rect rf = r.toRect();
  const float rad = ctx.scale.exact(3.0f);
  const bool lit = (v.hover && !v.disabled) || v.pressed;
  ctx.dl.addRoundedRectFilled(rf, rad, lerp(th->face.body, th->face.track, lit ? 0.85f : 0.55f));
  if (fill_ && !v.disabled) {
    const double lo = spec_.min, hi = spec_.max;
    const double anchor = lo < 0.0 && hi > 0.0 ? (0.0 - lo) / (hi - lo) : 0.0;
    const double n = normalized();
    const float x0 = rf.x0 + static_cast<float>(std::min(anchor, n)) * (rf.x1 - rf.x0);
    const float x1 = rf.x0 + static_cast<float>(std::max(anchor, n)) * (rf.x1 - rf.x0);
    if (x1 - x0 >= 0.5f) ctx.dl.addRoundedRectFilled({x0, rf.y0, x1, rf.y1}, std::min(rad, 0.5f * (x1 - x0)), withAlpha(th->face.value, 64));
  }
  ctx.dl.addRoundedRectStroke(rf, rad, v.focus ? withAlpha(th->accent.focus, 200) : th->face.frame);
  const ThemeFonts* f = ctx.res.uiFonts;
  if (!f) return;
  const int32_t pad = ctx.scale.px(6.0f);
  int32_t used = 0;
  if (!label_.empty()) used = paint::text(ctx, f->small, r, label_, v.disabled ? withAlpha(th->face.ink, 140) : th->face.ink, TextAlign::Left, pad, pad);
  const Rgba col = v.disabled ? withAlpha(th->face.ink, 160) : v.pressed ? th->face.handle : th->face.value;
  paint::text(ctx, f->numeric, r, valueText(), col, label_.empty() ? TextAlign::Center : TextAlign::Right,
              label_.empty() ? pad : pad + used + ctx.scale.px(4.0f), pad);
}

// ---- CardFlow -----------------------------------------------------------------------------------------------------

CardFlow::CardFlow(std::string name) : Widget(std::move(name)) { setLayout(LayoutKind::Custom); }

void CardFlow::setCardWidth(const Widget& card, float width) {
  for (auto& [c, w] : widths_)
    if (c == &card) {
      if (w != width) invalidateArrange();
      w = width;
      return;
    }
  widths_.emplace_back(&card, width);
  invalidateArrange();
}

float CardFlow::naturalWidth(const Widget& card) const {
  for (const auto& [c, w] : widths_)
    if (c == &card) return w;
  return 0.0f;
}

std::vector<CardFlow::Column> CardFlow::columnsFor(float height) const {
  std::vector<Column> cols;
  float used = 0.0f;
  for (size_t i = 0; i < childCount(); ++i) {
    const Widget& c = child(i);
    if (!c.visible()) continue;
    const float h = c.height().value;
    if (cols.empty() || used + kGap + h > height) {  // a new column (a card taller than the flow gets one alone)
      cols.push_back({i, i + 1, naturalWidth(c)});
      used = h;
    } else {
      cols.back().end = i + 1;
      cols.back().width = std::max(cols.back().width, naturalWidth(c));
      used += kGap + h;
    }
  }
  return cols;
}

float CardFlow::plan(float height, float single, int* columns) const {
  const std::vector<Column> cols = columnsFor(height - kTop);
  if (columns != nullptr) *columns = static_cast<int>(cols.size());
  if (cols.size() <= 1) return single;
  float w = kGap * static_cast<float>(cols.size() - 1);
  for (const Column& c : cols) w += c.width;
  return w;
}

void CardFlow::arrange(LayoutPass& pass) {
  const IRect r = rect();
  const IRect in{r.x, r.y + pass.px(kTop), r.w, std::max(0, r.h - pass.px(kTop))};
  const std::vector<Column> cols = columnsFor(static_cast<float>(in.h) / pass.scale());
  const int32_t gap = pass.px(kGap);
  // Column widths: one column spans the flow; several keep their natural widths, scaled to fit (edges from the
  // running sum, so they add up to the content width exactly when scaled).
  float natural = 0.0f;
  for (const Column& c : cols) natural += c.width;
  const int32_t free = in.w - gap * static_cast<int32_t>(cols.empty() ? 0 : cols.size() - 1);
  const double k = cols.size() <= 1 || natural <= 0.0f
                       ? 1.0
                       : std::min(1.0, static_cast<double>(free) / (static_cast<double>(natural) * pass.scale()));
  int32_t x = in.x;
  for (const Column& c : cols) {
    const int32_t w = cols.size() == 1 ? in.w : static_cast<int32_t>(std::lround(c.width * pass.scale() * k));
    int32_t y = in.y;
    for (size_t i = c.first; i < c.end; ++i) {
      Widget& card = child(i);
      if (!card.visible()) continue;
      const int32_t h = pass.px(card.height().value);
      pass.place(card, {x, y, w, h});
      y += h + gap;
    }
    x += w + gap;
  }
}

}  // namespace pq::views::audio_clip
