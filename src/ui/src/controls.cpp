// Base controls (T3.2): Control, Label, Button, Toggle, value controls (DragBox, Fader, Knob), Dropdown,
// Scrollbar, Meter. TextBuffer / TextEdit are in text_edit.cpp, menus and popovers in overlay.cpp.

#include "pq/ui/controls.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>

#include "controls_paint.h"
#include "pq/ui/overlay.h"
#include "pq/ui/theme.h"

namespace pq::ui {

using detail::box;
using detail::drawText;

FontId fontOf(const ThemeFonts& f, FontRoleId role) noexcept {
  switch (role) {
    case FontRoleId::Small: return f.small;
    case FontRoleId::Body: return f.body;
    case FontRoleId::BodyStrong: return f.bodyStrong;
    case FontRoleId::Title: return f.title;
    case FontRoleId::Numeric: return f.numeric;
  }
  return f.body;
}

// --- Control --------------------------------------------------------------------------------------------

Control::Control(std::string name) : Widget(std::move(name)) {}

Control& Control::setTooltip(std::string text) {
  tooltip_ = std::move(text);
  return *this;
}

Control& Control::setInfo(std::string title, std::string body) {
  infoTitle_ = std::move(title);
  infoBody_ = std::move(body);
  return *this;
}

void Control::setStateOverride(uint8_t bits) {
  if (bits == override_) return;
  override_ = bits;
  markDirty();
}

void Control::setPressed(bool p) noexcept {
  if (p == pressed_) return;
  pressed_ = p;
  markDirty();
}

VisualState Control::visualState() const noexcept {
  VisualState v;
  if (override_ != vis::kLive) {
    v.disabled = (override_ & vis::kDisabled) != 0 || !enabled();
    v.hover = (override_ & vis::kHover) != 0;
    v.pressed = (override_ & vis::kPressed) != 0;
    v.focus = (override_ & vis::kFocus) != 0;
    return v;
  }
  v.disabled = !enabled();
  if (v.disabled) return v;
  v.hover = hovered();
  v.pressed = pressed_;
  v.focus = focused() && (focusRingOnPointerFocus_ || (tree() && tree()->focusVisible()));
  return v;
}

// --- Label ----------------------------------------------------------------------------------------------

Label::Label(std::string name, std::string text, FontRoleId font, TextTone tone, TextAlign align)
    : Control(std::move(name)), text_(std::move(text)), font_(font), tone_(tone), align_(align) {}

void Label::setText(std::string text) {
  if (text == text_) return;
  text_ = std::move(text);
  markDirty();
}

void Label::paint(PaintContext& ctx) {
  const Theme* th = ctx.res.theme;
  const ThemeFonts* f = ctx.res.uiFonts;
  if (!th || !f) return;
  const VisualState v = visualState();
  const Rgba col = v.disabled ? th->text.disabled : (tone_ == TextTone::Primary ? th->text.primary : th->text.secondary);
  drawText(ctx, fontOf(*f, font_), rect(), text_, col, align_, 0, 0);
}

// --- Button ---------------------------------------------------------------------------------------------

Button::Button(std::string name, std::string text) : Control(std::move(name)), text_(std::move(text)) { setFocusable(true); }

void Button::setText(std::string text) {
  if (text == text_) return;
  text_ = std::move(text);
  markDirty();
}

void Button::paint(PaintContext& ctx) {
  const Theme* th = ctx.res.theme;
  if (!th) return;
  const VisualState v = visualState();
  const Rgba fill = v.disabled  ? th->control.fillDisabled
                    : v.pressed ? th->control.fillPressed
                    : v.hover   ? th->control.fillHover
                                : th->control.fill;
  box(ctx.dl, rect(), ctx.scale.exact(th->radius.control), fill, v.focus ? th->accent.focus : th->control.border);
  if (const ThemeFonts* f = ctx.res.uiFonts)
    drawText(ctx, f->small, rect(), text_, v.disabled ? th->text.disabled : th->text.primary, TextAlign::Center,
             ctx.scale.px(th->spacing.sm), ctx.scale.px(th->spacing.sm));
}

bool Button::onPointer(const PointerEvent& e) {
  switch (e.type) {
    case PointerType::Down:
      if (e.button != PointerButton::Left) return false;
      inside_ = true;
      setPressed(true);
      return true;
    case PointerType::Move:
      if (!pressedNow() && !captured()) return false;
      if (captured()) {
        const bool in = rect().contains(e.pos);
        if (in != inside_) {
          inside_ = in;
          markDirty();
        }
        setPressed(in);
      }
      return false;
    case PointerType::Up: {
      if (e.button != PointerButton::Left || !captured()) return false;
      const bool fire = rect().contains(e.pos);
      setPressed(false);
      inside_ = false;
      if (fire && onClick) onClick();
      return true;
    }
    default: return false;
  }
}

bool Button::onKey(const KeyEvent& e) {
  if (!e.down || e.repeat || (e.mods & mod::kChordMask)) return false;
  if (e.key != key::kSpace && e.key != key::kReturn) return false;
  if (onClick) onClick();
  return true;
}

void Button::onCaptureLost() {
  inside_ = false;
  setPressed(false);
}

// --- Toggle ---------------------------------------------------------------------------------------------

Toggle::Toggle(std::string name, std::string text, ToggleStyle style, bool on)
    : Control(std::move(name)), text_(std::move(text)), style_(style), on_(on) {
  setFocusable(true);
}

void Toggle::setOn(bool on) {
  if (on == on_) return;
  on_ = on;
  markDirty();
}

void Toggle::flip() {
  on_ = !on_;
  markDirty();
  if (onToggle) onToggle(on_);
}

void Toggle::paint(PaintContext& ctx) {
  const Theme* th = ctx.res.theme;
  if (!th) return;
  const VisualState v = visualState();
  Rgba onCol = th->accent.primary;
  switch (style_) {
    case ToggleStyle::Accent: onCol = th->accent.primary; break;
    case ToggleStyle::Activator: onCol = th->accent.activator; break;
    case ToggleStyle::Solo: onCol = th->accent.solo; break;
    case ToggleStyle::Arm: onCol = th->accent.arm; break;
  }
  Rgba fill, ink;
  if (on_) {
    fill = v.disabled ? lerp(onCol, th->control.fillDisabled, 0.65f) : v.pressed ? detail::pressOf(onCol) : v.hover ? detail::hoverOf(onCol) : onCol;
    ink = v.disabled ? th->control.fillDisabled : th->text.onAccent;
  } else {
    fill = v.disabled ? th->control.fillDisabled : v.pressed ? th->control.fillPressed : v.hover ? th->control.fillHover : th->control.fill;
    ink = v.disabled ? th->text.disabled : th->text.primary;
  }
  const IRect r = rect();
  box(ctx.dl, r, ctx.scale.exact(th->radius.control), fill, v.focus ? th->accent.focus : th->control.border);
  if (text_.empty()) {
    // Arm without a label: Live's record dot.
    const float rad = std::max(2.0f, std::min(ctx.scale.exact(3.5f), 0.3f * static_cast<float>(std::min(r.w, r.h))));
    ctx.dl.addCircleFilled({static_cast<float>(r.x) + 0.5f * static_cast<float>(r.w), static_cast<float>(r.y) + 0.5f * static_cast<float>(r.h)},
                           rad, on_ ? ink : (v.disabled ? th->text.disabled : th->text.secondary));
    return;
  }
  if (const ThemeFonts* f = ctx.res.uiFonts)
    drawText(ctx, text_.size() <= 2 ? f->bodyStrong : f->small, r, text_, ink, TextAlign::Center, ctx.scale.px(2), ctx.scale.px(2));
}

bool Toggle::onPointer(const PointerEvent& e) {
  if (e.button != PointerButton::Left) return false;
  if (e.type == PointerType::Down) {
    setPressed(true);
    flip();  // Live's toggles switch on press
    return true;
  }
  if (e.type == PointerType::Up && captured()) {
    setPressed(false);
    return true;
  }
  return false;
}

bool Toggle::onKey(const KeyEvent& e) {
  if (!e.down || e.repeat || (e.mods & mod::kChordMask)) return false;
  if (e.key != key::kSpace && e.key != key::kReturn) return false;
  flip();
  return true;
}

void Toggle::onCaptureLost() { setPressed(false); }

// --- ValueControl ---------------------------------------------------------------------------------------

ValueControl::ValueControl(std::string name, ValueSpec spec) : Control(std::move(name)), spec_(std::move(spec)) {
  setFocusable(true);
  focusRingOnPointerFocus_ = true;
  value_ = clampQuantise(spec_.def);
  refreshText();
}

double ValueControl::clampQuantise(double v) const noexcept {
  const double lo = std::min(spec_.min, spec_.max), hi = std::max(spec_.min, spec_.max);
  if (!(v == v)) v = spec_.def;  // NaN
  v = std::clamp(v, lo, hi);
  if (spec_.step > 0.0) v = std::clamp(lo + std::round((v - lo) / spec_.step) * spec_.step, lo, hi);
  return v;
}

void ValueControl::setValue(double v) {
  v = clampQuantise(v);
  if (v == value_) return;
  value_ = v;
  refreshText();
  markDirty();
}

double ValueControl::normalized() const noexcept {
  const double span = spec_.max - spec_.min;
  return span != 0.0 ? std::clamp((value_ - spec_.min) / span, 0.0, 1.0) : 0.0;
}

void ValueControl::setSpec(ValueSpec spec) {
  spec_ = std::move(spec);
  value_ = clampQuantise(value_);
  refreshText();
  markDirty();
}

void ValueControl::refreshText() {
  if (formatter) {
    text_ = formatter(value_);
    return;
  }
  double v = value_;
  const double half = 0.5 * std::pow(10.0, -spec_.decimals);
  if (std::fabs(v) < half) v = 0.0;  // no "-0.00"
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.*f", std::clamp(spec_.decimals, 0, 9), v);
  text_ = buf;
  if (!spec_.unit.empty()) {
    text_ += ' ';
    text_ += spec_.unit;
  }
}

std::optional<double> ValueControl::parse(std::string_view s) const {
  if (parser) return parser(s);
  auto trim = [](std::string_view x) {
    while (!x.empty() && (x.front() == ' ' || x.front() == '\t')) x.remove_prefix(1);
    while (!x.empty() && (x.back() == ' ' || x.back() == '\t')) x.remove_suffix(1);
    return x;
  };
  s = trim(s);
  if (!spec_.unit.empty() && s.size() >= spec_.unit.size()) {
    const std::string_view tail = s.substr(s.size() - spec_.unit.size());
    bool same = true;
    for (size_t i = 0; i < tail.size(); ++i) {
      const auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; };
      same = same && lower(tail[i]) == lower(spec_.unit[i]);
    }
    if (same) s = trim(s.substr(0, s.size() - spec_.unit.size()));
  }
  if (s.empty()) return std::nullopt;
  if (s.front() == '+') s.remove_prefix(1);
  double v = 0.0;
  const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
  if (r.ec != std::errc{} || r.ptr != s.data() + s.size()) return std::nullopt;
  return v;
}

double ValueControl::keyStep() const noexcept {
  if (spec_.keyStep > 0.0) return spec_.keyStep;
  return std::max(spec_.step, (spec_.max - spec_.min) / 100.0);
}

void ValueControl::userSet(double v) {
  v = clampQuantise(v);
  if (v == value_) return;
  if (!gesture_) {
    gesture_ = true;
    if (onGestureBegin) onGestureBegin();
  }
  value_ = v;
  refreshText();
  markDirty();
  if (onChange) onChange(value_);
}

void ValueControl::endGesture() {
  if (!gesture_) return;
  gesture_ = false;
  if (onGestureEnd) onGestureEnd();
}

void ValueControl::dragStart(Vec2 pos) {
  dragging_ = true;
  last_ = pos;
  raw_ = value_;
  setPressed(true);
}

void ValueControl::dragMove(Vec2 pos, Mods m, double unitsPerPx, bool vertical) {
  if (!dragging_) return;
  const double d = vertical ? static_cast<double>(last_.y - pos.y) : static_cast<double>(pos.x - last_.x);
  last_ = pos;
  const double k = (m & mod::kShift) ? spec_.fineFactor : 1.0;
  const double lo = std::min(spec_.min, spec_.max), hi = std::max(spec_.min, spec_.max);
  // Accumulate unquantised and clamped, so a drag past an end responds as soon as it turns back (Live).
  raw_ = std::clamp(raw_ + d * unitsPerPx * k, lo, hi);
  userSet(raw_);
}

void ValueControl::dragEnd() {
  if (!dragging_) return;
  dragging_ = false;
  setPressed(false);
  endGesture();
}

void ValueControl::onCaptureLost() { dragEnd(); }

bool ValueControl::onKey(const KeyEvent& e) {
  if (!e.down) return false;
  const Mods chord = e.mods & mod::kChordMask;
  if ((e.key == key::kDelete || e.key == key::kBackspace) && chord == mod::kNone) {
    userSet(spec_.def);
    endGesture();
    return true;
  }
  if ((e.key == key::kUp || e.key == key::kDown) && (chord & ~mod::kShift) == 0) {
    double d = keyStep() * ((chord & mod::kShift) ? spec_.fineFactor : 1.0);
    if (spec_.step > 0.0) d = std::max(d, spec_.step);
    userSet(value_ + (e.key == key::kUp ? d : -d));
    endGesture();
    return true;
  }
  return false;
}

// --- DragBox --------------------------------------------------------------------------------------------

DragBox::DragBox(std::string name, ValueSpec spec, TextAlign align) : ValueControl(std::move(name), std::move(spec)), align_(align) {}

void DragBox::beginEdit() {
  if (editing_) return;
  editing_ = true;
  // The number alone (no unit), like Live's type-in.
  char buf[64];
  double v = value_;
  if (std::fabs(v) < 0.5 * std::pow(10.0, -spec_.decimals)) v = 0.0;
  std::snprintf(buf, sizeof buf, "%.*f", std::clamp(spec_.decimals, 0, 9), v);
  edit_.setText(buf);
  edit_.selectAll();
  setAcceptsTextInput(true);
  if (tree() && !focused()) tree()->setFocus(this);
  markDirty();
}

void DragBox::endEdit() {
  editing_ = false;
  setAcceptsTextInput(false);
  markDirty();
}

void DragBox::commitEdit() {
  if (!editing_) return;
  const std::optional<double> v = parse(edit_.text());
  endEdit();
  if (v) {  // an unparsable entry reverts (Live)
    userSet(*v);
    endGesture();
  }
}

void DragBox::cancelEdit() {
  if (editing_) endEdit();
}

void DragBox::onFocusChanged(bool focused) {
  if (!focused && editing_) commitEdit();
  markDirty();
}

bool DragBox::onText(const TextEvent& e) {
  if (!editing_) return false;
  edit_.insert(e.text());
  markDirty();
  return true;
}

bool DragBox::onKey(const KeyEvent& e) {
  if (editing_) {
    if (!e.down) return true;
    if (e.key == key::kReturn) {
      commitEdit();
      return true;
    }
    if (e.key == key::kEscape) {
      cancelEdit();
      return true;
    }
    if (e.key == key::kTab) {
      commitEdit();
      return false;  // focus traversal continues
    }
    bool changed = false;
    if (tree() && edit_.handleKey(e, tree()->clipboard(), changed)) {
      markDirty();
      return true;
    }
    return false;
  }
  if (e.down && !e.repeat && e.key == key::kReturn && (e.mods & mod::kChordMask) == 0) {
    beginEdit();
    return true;
  }
  return ValueControl::onKey(e);
}

bool DragBox::onPointer(const PointerEvent& e) {
  const float s = tree() ? tree()->scale() : 1.0f;
  if (editing_) {
    // Clicks inside the type-in place the caret; the run is the one paint() draws.
    const PaintResources* res = tree() ? tree()->resources() : nullptr;
    auto byteAt = [&](float x) -> uint32_t {
      if (!res || !res->text || !res->uiFonts) return static_cast<uint32_t>(edit_.text().size());
      const TextRun run = res->text->shape(res->uiFonts->numeric, edit_.text());
      const int32_t pad = static_cast<int32_t>(std::lround(4.0f * s));
      int32_t x0 = rect().x + pad;
      if (align_ == TextAlign::Center) x0 = rect().x + (rect().w - run.advance) / 2;
      else if (align_ == TextAlign::Right) x0 = rect().right() - pad - run.advance;
      return run.hitTest(static_cast<int32_t>(std::lround(x)) - x0);
    };
    if (e.type == PointerType::Down && e.button == PointerButton::Left) {
      if (e.clickCount >= 2) edit_.selectAll();
      else edit_.setCaret(byteAt(e.pos.x), (e.mods & mod::kShift) != 0);
      markDirty();
      return true;
    }
    if (e.type == PointerType::Move && captured()) {
      edit_.setCaret(byteAt(e.pos.x), true);
      markDirty();
      return true;
    }
    return e.type == PointerType::Up && captured();
  }
  switch (e.type) {
    case PointerType::Down:
      if (e.button != PointerButton::Left) return false;
      if (e.clickCount >= 2) {
        dragEnd();
        beginEdit();
        return true;
      }
      dragStart(e.pos);
      return true;
    case PointerType::Move:
      if (!dragging()) return false;
      dragMove(e.pos, e.mods, (spec_.max - spec_.min) / (spec_.dragPixels * static_cast<double>(s)), true);
      return true;
    case PointerType::Up:
      if (e.button != PointerButton::Left || !dragging()) return false;
      dragEnd();
      return true;
    default: return false;
  }
}

void DragBox::paint(PaintContext& ctx) {
  const Theme* th = ctx.res.theme;
  if (!th) return;
  const VisualState v = visualState();
  const IRect r = rect();
  const Rgba fill = editing_ ? th->surface.sunken : (v.hover && !v.disabled) || v.pressed ? th->field.fillHover : th->field.fill;
  box(ctx.dl, r, ctx.scale.exact(th->radius.control), fill, (v.focus || editing_) ? th->field.borderFocus : th->field.border);
  const ThemeFonts* f = ctx.res.uiFonts;
  if (!f || !ctx.res.text) return;
  const int32_t pad = ctx.scale.px(4);
  if (!editing_) {
    const Rgba col = v.disabled ? th->text.disabled : v.pressed ? th->accent.primary : th->text.primary;
    drawText(ctx, f->numeric, r, text_, col, align_, pad, pad);
    return;
  }
  // Type-in: selection, text and caret, clipped to the box.
  TextSystem& ts = *ctx.res.text;
  const TextRun run = ts.shape(f->numeric, edit_.text());
  int32_t x0 = r.x + pad;
  if (align_ == TextAlign::Center) x0 = r.x + (r.w - run.advance) / 2;
  else if (align_ == TextAlign::Right) x0 = r.right() - pad - run.advance;
  x0 = std::max(x0, r.x + pad - std::max(0, run.advance - (r.w - 2 * pad)));
  const IRect in = detail::inset(r, 1);
  ctx.dl.pushClip(in.toRect());
  const float y0 = static_cast<float>(r.y + ctx.scale.px(3)), y1 = static_cast<float>(r.bottom() - ctx.scale.px(3));
  if (edit_.hasSelection()) {
    const float a = static_cast<float>(x0 + run.caretX(edit_.selBegin())), b = static_cast<float>(x0 + run.caretX(edit_.selEnd()));
    ctx.dl.addRectFilled({a, y0, b, y1}, th->text.selection);
  }
  const int32_t base = detail::capCentredBaseline(r.y, r.h, ts.metrics(f->numeric).pixelSize);
  TextSystem::drawRun(ctx.dl, run, {static_cast<float>(x0), static_cast<float>(base)}, th->text.primary);
  const float cx = static_cast<float>(x0 + run.caretX(edit_.caret()));
  ctx.dl.addRectFilled({cx, y0, cx + 1.0f, y1}, th->text.caret);
  ctx.dl.popClip();
}

// --- Fader ----------------------------------------------------------------------------------------------

namespace {
struct FaderGeom {
  IRect thumb;
  int32_t travel;  // device px
  int32_t start;   // device coordinate of the thumb at the maximum (vertical: top) / minimum (horizontal: left)
};
FaderGeom faderGeom(const IRect& r, Fader::Orientation o, double norm, float scale) {
  FaderGeom g{};
  const int32_t thick = std::max(4, static_cast<int32_t>(std::lround(8.0f * scale)));
  if (o == Fader::Orientation::Vertical) {
    const int32_t w = std::min(r.w, static_cast<int32_t>(std::lround(18.0f * scale)));
    g.travel = std::max(0, r.h - thick);
    g.start = r.y;
    const int32_t y = r.y + static_cast<int32_t>(std::lround((1.0 - norm) * g.travel));
    g.thumb = {r.x + (r.w - w) / 2, y, w, thick};
  } else {
    const int32_t h = std::min(r.h, static_cast<int32_t>(std::lround(14.0f * scale)));
    g.travel = std::max(0, r.w - thick);
    g.start = r.x;
    const int32_t x = r.x + static_cast<int32_t>(std::lround(norm * g.travel));
    g.thumb = {x, r.y + (r.h - h) / 2, thick, h};
  }
  return g;
}
}  // namespace

Fader::Fader(std::string name, ValueSpec spec, Orientation o) : ValueControl(std::move(name), std::move(spec)), orient_(o) {}

IRect Fader::thumbRect() const noexcept { return faderGeom(rect(), orient_, normalized(), tree() ? tree()->scale() : 1.0f).thumb; }

int32_t Fader::trackLength() const noexcept { return faderGeom(rect(), orient_, 0.0, tree() ? tree()->scale() : 1.0f).travel; }

bool Fader::onPointer(const PointerEvent& e) {
  const bool vertical = orient_ == Orientation::Vertical;
  switch (e.type) {
    case PointerType::Down: {
      if (e.button != PointerButton::Left) return false;
      const FaderGeom g = faderGeom(rect(), orient_, normalized(), tree() ? tree()->scale() : 1.0f);
      if (!g.thumb.contains(e.pos) && g.travel > 0) {
        // Jump: centre the thumb on the pointer, then drag from there.
        const double half = 0.5 * (vertical ? g.thumb.h : g.thumb.w);
        const double p = vertical ? (static_cast<double>(e.pos.y) - g.start - half) : (static_cast<double>(e.pos.x) - g.start - half);
        const double n = std::clamp(p / g.travel, 0.0, 1.0);
        userSet(spec_.min + (spec_.max - spec_.min) * (vertical ? 1.0 - n : n));
      }
      dragStart(e.pos);
      return true;
    }
    case PointerType::Move: {
      if (!dragging()) return false;
      const int32_t len = std::max(1, trackLength());
      dragMove(e.pos, e.mods, (spec_.max - spec_.min) / static_cast<double>(len), vertical);
      return true;
    }
    case PointerType::Up:
      if (e.button != PointerButton::Left || !dragging()) return false;
      dragEnd();
      return true;
    default: return false;
  }
}

void Fader::paint(PaintContext& ctx) {
  const Theme* th = ctx.res.theme;
  if (!th) return;
  const VisualState v = visualState();
  const IRect r = rect();
  const bool vertical = orient_ == Orientation::Vertical;
  const FaderGeom g = faderGeom(r, orient_, normalized(), ctx.scale.factor);
  // Slot: a dark groove along the travel; the value fill runs from the minimum to the thumb centre.
  const int32_t slot = std::max(2, ctx.scale.px(4)), fillW = std::max(2, ctx.scale.px(2));
  const Rgba value = v.disabled ? th->control.valueDisabled : th->control.value;
  if (vertical) {
    const int32_t cx = r.x + r.w / 2;
    const int32_t half = g.thumb.h / 2;
    ctx.dl.addRectFilled(IRect{cx - slot / 2, r.y + half, slot, std::max(0, r.h - 2 * half)}.toRect(), th->control.border);
    const int32_t top = g.thumb.y + half;
    ctx.dl.addRectFilled(IRect{cx - fillW / 2, top, fillW, std::max(0, r.bottom() - half - top)}.toRect(), value);
  } else {
    const int32_t cy = r.y + r.h / 2;
    const int32_t half = g.thumb.w / 2;
    ctx.dl.addRectFilled(IRect{r.x + half, cy - slot / 2, std::max(0, r.w - 2 * half), slot}.toRect(), th->control.border);
    const int32_t right = g.thumb.x + half;
    ctx.dl.addRectFilled(IRect{r.x + half, cy - fillW / 2, std::max(0, right - r.x - half), fillW}.toRect(), value);
  }
  // Thumb: a flat cap with a centre line, like a console fader.
  const Rgba cap = v.disabled ? th->control.fillDisabled : (v.hover || v.pressed) ? th->control.thumbHover : th->control.thumb;
  box(ctx.dl, g.thumb, ctx.scale.exact(1.5f), cap, v.focus ? th->accent.focus : th->control.border);
  if (vertical)
    ctx.dl.addHLine(static_cast<float>(g.thumb.x + 2), static_cast<float>(g.thumb.right() - 2), static_cast<float>(g.thumb.y + g.thumb.h / 2),
                    v.pressed ? th->control.value : th->control.border);
  else
    ctx.dl.addVLine(static_cast<float>(g.thumb.x + g.thumb.w / 2), static_cast<float>(g.thumb.y + 2), static_cast<float>(g.thumb.bottom() - 2),
                    v.pressed ? th->control.value : th->control.border);
}

// --- Knob -----------------------------------------------------------------------------------------------

Knob::Knob(std::string name, ValueSpec spec) : ValueControl(std::move(name), std::move(spec)) {}

void Knob::setModulation(const ModulationRing& m) {
  if (m == mod_) return;
  mod_ = m;
  markDirty();
}

bool Knob::onPointer(const PointerEvent& e) {
  switch (e.type) {
    case PointerType::Down:
      if (e.button != PointerButton::Left) return false;
      dragStart(e.pos);
      return true;
    case PointerType::Move: {
      if (!dragging()) return false;
      const double s = tree() ? tree()->scale() : 1.0;
      dragMove(e.pos, e.mods, (spec_.max - spec_.min) / (spec_.dragPixels * s), true);
      return true;
    }
    case PointerType::Up:
      if (e.button != PointerButton::Left || !dragging()) return false;
      dragEnd();
      return true;
    default: return false;
  }
}

void Knob::paint(PaintContext& ctx) {
  const Theme* th = ctx.res.theme;
  if (!th) return;
  const VisualState v = visualState();
  const IRect r = rect();
  const Vec2 c{static_cast<float>(r.x) + 0.5f * static_cast<float>(r.w), static_cast<float>(r.y) + 0.5f * static_cast<float>(r.h)};
  const float thick = ctx.scale.exact(2.5f);
  const float rad = 0.5f * static_cast<float>(std::min(r.w, r.h)) - ctx.scale.exact(4.0f) - 0.5f * thick;
  if (rad < 2.0f) return;
  const float a0 = kStartAngle, a1 = kStartAngle + kSweep;
  const float n = static_cast<float>(normalized());
  const float av = a0 + kSweep * n;
  // Bipolar ranges grow the value arc from 0 (pan, detune), unipolar ones from the minimum.
  float anchor = a0;
  if (spec_.min < 0.0 && spec_.max > 0.0)
    anchor = a0 + kSweep * static_cast<float>((0.0 - spec_.min) / (spec_.max - spec_.min));
  const Rgba track = v.disabled ? th->control.fillDisabled : v.hover || v.pressed ? lerp(th->control.track, th->control.needle, 0.18f) : th->control.track;
  const Rgba value = v.disabled ? th->control.valueDisabled : v.pressed ? detail::hoverOf(th->control.value) : th->control.value;
  ctx.dl.addArcStroke(c, rad, a0, a1, track, thick);
  if (std::fabs(av - anchor) > 1e-4f) ctx.dl.addArcStroke(c, rad, std::min(anchor, av), std::max(anchor, av), value, thick);
  // Needle from near the centre to the arc.
  const Vec2 d{std::cos(av), std::sin(av)}, nrm{-d.y, d.x};
  const float hw = 0.5f * ctx.scale.exact(1.75f);
  const float i0 = rad * 0.25f, i1 = rad - 0.5f * thick;
  const Vec2 quad[4] = {{c.x + d.x * i0 + nrm.x * hw, c.y + d.y * i0 + nrm.y * hw},
                        {c.x + d.x * i1 + nrm.x * hw, c.y + d.y * i1 + nrm.y * hw},
                        {c.x + d.x * i1 - nrm.x * hw, c.y + d.y * i1 - nrm.y * hw},
                        {c.x + d.x * i0 - nrm.x * hw, c.y + d.y * i0 - nrm.y * hw}};
  ctx.dl.addConvexPolyFilled(quad, v.disabled ? th->text.disabled : th->control.needle);
  // Modulation ring (signal display: PQ Meter's accent), outside the track.
  if (mod_.active && !v.disabled) {
    const float mr = rad + 0.5f * thick + ctx.scale.exact(2.25f);
    const float m0 = a0 + kSweep * std::clamp(std::min(mod_.lo, mod_.hi), 0.0f, 1.0f);
    const float m1 = a0 + kSweep * std::clamp(std::max(mod_.lo, mod_.hi), 0.0f, 1.0f);
    if (m1 - m0 > 1e-4f) ctx.dl.addArcStroke(c, mr, m0, m1, withAlpha(signal_theme::kAccent, 210), ctx.scale.exact(1.5f), LineCap::Round);
    if (mod_.current >= 0.0f) {
      const float am = a0 + kSweep * std::clamp(mod_.current, 0.0f, 1.0f);
      ctx.dl.addCircleFilled({c.x + std::cos(am) * mr, c.y + std::sin(am) * mr}, ctx.scale.exact(2.0f), signal_theme::kAccent);
    }
  }
  if (v.focus) box(ctx.dl, r, ctx.scale.exact(th->radius.control), 0, th->accent.focus);
}

// --- Dropdown -------------------------------------------------------------------------------------------

Dropdown::Dropdown(std::string name, std::vector<std::string> items, int32_t index)
    : Control(std::move(name)), items_(std::move(items)), index_(items_.empty() ? -1 : std::clamp(index, 0, static_cast<int32_t>(items_.size()) - 1)),
      alive_(std::make_shared<Dropdown*>(this)) {
  setFocusable(true);
}

Dropdown::~Dropdown() {
  *alive_ = nullptr;  // pending menu callbacks become no-ops
  if (menu_)
    if (OverlayLayer* layer = OverlayLayer::of(*menu_)) layer->close(*menu_);
}

void Dropdown::setIndex(int32_t i) {
  if (items_.empty()) return;
  i = std::clamp(i, 0, static_cast<int32_t>(items_.size()) - 1);
  if (i == index_) return;
  index_ = i;
  markDirty();
}

void Dropdown::choose(int32_t i) {
  if (items_.empty()) return;
  i = std::clamp(i, 0, static_cast<int32_t>(items_.size()) - 1);
  if (i == index_) return;
  index_ = i;
  markDirty();
  if (onChange) onChange(index_);
}

void Dropdown::openMenu() {
  if (menu_ || items_.empty()) return;
  OverlayLayer* layer = OverlayLayer::of(*this);
  if (!layer) return;
  std::vector<MenuItem> mi;
  mi.reserve(items_.size());
  const std::shared_ptr<Dropdown*> alive = alive_;
  for (int32_t i = 0; i < static_cast<int32_t>(items_.size()); ++i) {
    MenuItem it = MenuItem::item(items_[static_cast<size_t>(i)], [alive, i] {
      if (*alive) (*alive)->choose(i);
    });
    it.checked = i == index_;
    mi.push_back(std::move(it));
  }
  OverlayOptions o;
  o.anchor = rect();
  o.placement = Placement::Below;
  o.onClosed = [alive] {
    if (Dropdown* d = *alive) {
      d->menu_ = nullptr;
      d->markDirty();
    }
  };
  auto menu = std::make_unique<Menu>(std::move(mi), layer->uniqueName("dropdown-menu"));
  Menu& m = static_cast<Menu&>(layer->open(std::move(menu), std::move(o)));
  m.setMinWidthPx(rect().w);
  m.setHighlighted(index_);
  menu_ = &m;
  markDirty();
}

bool Dropdown::onPointer(const PointerEvent& e) {
  if (e.type == PointerType::Down && e.button == PointerButton::Left) {
    openMenu();
    return true;
  }
  return e.type == PointerType::Up && captured();
}

bool Dropdown::onKey(const KeyEvent& e) {
  if (!e.down || (e.mods & mod::kChordMask)) return false;
  if (e.key == key::kUp || e.key == key::kDown) {
    choose(index_ + (e.key == key::kDown ? 1 : -1));
    return true;
  }
  if (!e.repeat && (e.key == key::kReturn || e.key == key::kSpace)) {
    openMenu();
    return true;
  }
  return false;
}

void Dropdown::paint(PaintContext& ctx) {
  const Theme* th = ctx.res.theme;
  if (!th) return;
  const VisualState v = visualState();
  const IRect r = rect();
  const bool open = menu_ != nullptr;
  const Rgba fill = v.disabled ? th->control.fillDisabled : (v.hover || v.pressed || open) ? th->field.fillHover : th->field.fill;
  box(ctx.dl, r, ctx.scale.exact(th->radius.control), fill, (v.focus || open) ? th->field.borderFocus : th->field.border);
  // Chevron at the right.
  const float cw = ctx.scale.exact(7.0f), ch = ctx.scale.exact(4.0f);
  const float cx = static_cast<float>(r.right()) - ctx.scale.exact(9.0f), cy = static_cast<float>(r.y) + 0.5f * static_cast<float>(r.h);
  const Rgba ink = v.disabled ? th->text.disabled : th->text.secondary;
  ctx.dl.addTriangleFilled({cx - 0.5f * cw, cy - 0.5f * ch}, {cx + 0.5f * cw, cy - 0.5f * ch}, {cx, cy + 0.5f * ch}, ink);
  if (const ThemeFonts* f = ctx.res.uiFonts; f && index_ >= 0)
    drawText(ctx, f->small, r, items_[static_cast<size_t>(index_)], v.disabled ? th->text.disabled : th->text.primary, TextAlign::Left,
             ctx.scale.px(th->spacing.sm + 2), ctx.scale.px(18));
}

// --- Scrollbar ------------------------------------------------------------------------------------------

Scrollbar::Scrollbar(std::string name, ScrollView& view, Axis axis) : Control(std::move(name)), view_(&view), axis_(axis) {
  view.linkDependent(this);
}

Scrollbar::~Scrollbar() {
  if (view_) view_->unlinkDependent(this);
}

void Scrollbar::onScrollViewGone() noexcept {
  view_ = nullptr;
  markDirty();
}

int32_t Scrollbar::viewLen() const noexcept { return view_ ? (axis_ == Axis::Vertical ? view_->rect().h : view_->rect().w) : 0; }
int32_t Scrollbar::contentLen() const noexcept {
  return view_ ? (axis_ == Axis::Vertical ? view_->contentSize().h : view_->contentSize().w) : 0;
}
int32_t Scrollbar::scrollPos() const noexcept { return view_ ? (axis_ == Axis::Vertical ? view_->scrollY() : view_->scrollX()) : 0; }

void Scrollbar::scrollToPos(int32_t p) {
  if (!view_) return;
  if (axis_ == Axis::Vertical) view_->scrollTo(view_->scrollX(), p);
  else view_->scrollTo(p, view_->scrollY());
}

IRect Scrollbar::thumbRect() const noexcept {
  const IRect r = rect();
  const int32_t view = viewLen(), content = contentLen();
  if (!view_ || content <= view || view <= 0) return {};
  const bool v = axis_ == Axis::Vertical;
  const int32_t len = v ? r.h : r.w;
  const float s = tree() ? tree()->scale() : 1.0f;
  const int32_t minThumb = std::min(len, static_cast<int32_t>(std::lround(16.0f * s)));
  const int32_t thumb = std::max(minThumb, static_cast<int32_t>(static_cast<int64_t>(len) * view / content));
  const int32_t travel = len - thumb;
  const int32_t pos = static_cast<int32_t>(static_cast<int64_t>(travel) * scrollPos() / (content - view));
  return v ? IRect{r.x, r.y + pos, r.w, thumb} : IRect{r.x + pos, r.y, thumb, r.h};
}

bool Scrollbar::onPointer(const PointerEvent& e) {
  const bool v = axis_ == Axis::Vertical;
  const float coord = v ? e.pos.y : e.pos.x;
  switch (e.type) {
    case PointerType::Down: {
      if (e.button != PointerButton::Left || !view_) return false;
      const IRect t = thumbRect();
      if (t.empty()) return true;
      if (t.contains(e.pos)) {
        dragging_ = true;
        grab_ = coord - static_cast<float>(v ? t.y : t.x);
        setPressed(true);
      } else {
        // Page towards the press.
        const int32_t page = std::max(1, viewLen() - std::max(1, viewLen() / 10));
        scrollToPos(scrollPos() + (coord < static_cast<float>(v ? t.y : t.x) ? -page : page));
      }
      return true;
    }
    case PointerType::Move: {
      if (!dragging_ || !view_) return false;
      const IRect r = rect(), t = thumbRect();
      if (t.empty()) return true;
      const int32_t len = v ? r.h : r.w, thumb = v ? t.h : t.w, travel = std::max(1, len - thumb);
      const float start = coord - grab_ - static_cast<float>(v ? r.y : r.x);
      const double f = std::clamp(static_cast<double>(start) / travel, 0.0, 1.0);
      scrollToPos(static_cast<int32_t>(std::lround(f * (contentLen() - viewLen()))));
      return true;
    }
    case PointerType::Up:
      if (!dragging_) return captured();
      dragging_ = false;
      setPressed(false);
      return true;
    default: return false;
  }
}

void Scrollbar::onCaptureLost() {
  dragging_ = false;
  setPressed(false);
}

void Scrollbar::paint(PaintContext& ctx) {
  const Theme* th = ctx.res.theme;
  if (!th) return;
  const VisualState vs = visualState();
  const IRect r = rect();
  ctx.dl.addRectFilled(r.toRect(), th->scrollbar.track);
  IRect t = thumbRect();
  if (t.empty()) return;
  const int32_t in = ctx.scale.px(2);
  t = axis_ == Axis::Vertical ? IRect{t.x + in, t.y + 1, std::max(1, t.w - 2 * in), std::max(1, t.h - 2)}
                              : IRect{t.x + 1, t.y + in, std::max(1, t.w - 2), std::max(1, t.h - 2 * in)};
  const Rgba col = vs.disabled ? th->control.fillDisabled : (vs.hover || vs.pressed) ? th->scrollbar.thumbHover : th->scrollbar.thumb;
  box(ctx.dl, t, ctx.scale.exact(th->radius.control), col, 0);
}

// --- Meter ----------------------------------------------------------------------------------------------

Meter::Meter(std::string name, float floorDb) : Control(std::move(name)), floorDb_(std::min(floorDb, -1.0f)) {}

int32_t Meter::lampHeight() const noexcept {
  const float s = tree() ? tree()->scale() : 1.0f;
  return std::max(2, static_cast<int32_t>(std::lround(3.0f * s)));
}

Meter::Px Meter::pixels(const MeterLevels& l) const noexcept {
  Px p{};
  p.channels = static_cast<uint8_t>(std::clamp<int>(l.channels, 1, 2));
  const IRect r = rect();
  const int32_t top = r.y + lampHeight() + 1;
  const int32_t h = std::max(0, r.bottom() - top);
  auto px = [&](float db) {
    const float f = std::clamp((db - floorDb_) / -floorDb_, 0.0f, 1.0f);
    return static_cast<int16_t>(std::lround(f * static_cast<float>(h)));
  };
  for (int c = 0; c < p.channels; ++c) {
    p.rms[c] = px(l.rmsDb[c]);
    p.peak[c] = px(l.peakDb[c]);
    p.hold[c] = l.holdDb[c] > floorDb_ ? px(l.holdDb[c]) : int16_t(-1);
    p.clip[c] = l.clip[c];
  }
  return p;
}

void Meter::setLevels(const MeterLevels& l) {
  levels_ = l;
  if (!(pixels(l) == drawn_)) markDirty();  // only when a drawn pixel changes
}

bool Meter::onPointer(const PointerEvent& e) {
  if (e.type != PointerType::Down || e.button != PointerButton::Left) return false;
  if (e.pos.y < static_cast<float>(rect().y + lampHeight() + 2) && onResetClip) onResetClip();
  return true;
}

void Meter::paint(PaintContext& ctx) {
  using namespace signal_theme;
  const IRect r = rect();
  const Px p = pixels(levels_);
  drawn_ = p;
  const int32_t lamp = lampHeight();
  const int32_t top = r.y + lamp + 1, bot = r.bottom();
  const int32_t n = p.channels, gap = n > 1 ? 1 : 0;
  const int32_t barW = std::max(1, (r.w - gap * (n - 1)) / n);
  const Rgba body[2] = {kWaveform, n > 1 ? kWaveformR : kWaveform};
  // PQ Meter's zone tint: the colour of a level is decided by its dB (the whole body takes the zone of its top).
  auto zone = [&](float db, Rgba base, uint32_t alpha) {
    if (db > kMeterHotDb) return withAlpha(kMeterHot, alpha);
    if (db > kMeterWarnDb) return withAlpha(kMeterWarn, alpha);
    return withAlpha(base, alpha);
  };
  for (int32_t c = 0; c < n; ++c) {
    const int32_t x0 = r.x + c * (barW + gap), x1 = x0 + barW;
    const float fx0 = static_cast<float>(x0), fx1 = static_cast<float>(x1);
    ctx.dl.addRectFilled({fx0, static_cast<float>(top), fx1, static_cast<float>(bot)}, kMeterWell);
    if (p.rms[c] > 0)
      ctx.dl.addRectFilled({fx0, static_cast<float>(bot - p.rms[c]), fx1, static_cast<float>(bot)},
                           zone(levels_.rmsDb[c], body[c], kMeterBodyAlpha));
    if (p.peak[c] > 0) {
      const int32_t capH = (bot - top) >= 48 ? 2 : 1;
      const int32_t y = std::max(top, bot - p.peak[c]);
      ctx.dl.addRectFilled({fx0, static_cast<float>(y), fx1, static_cast<float>(std::min(bot, y + capH))}, zone(levels_.peakDb[c], kMeterPeakCap, 255));
    }
    if (p.hold[c] >= 0) {
      const int32_t y = std::max(top, bot - p.hold[c]);
      ctx.dl.addRectFilled({fx0, static_cast<float>(y), fx1, static_cast<float>(y + 1)}, kMeterHold);
    }
    ctx.dl.addRectFilled({fx0, static_cast<float>(r.y), fx1, static_cast<float>(r.y + lamp)}, p.clip[c] ? kMeterLampOn : kMeterLampOff);
  }
}

}  // namespace pq::ui
