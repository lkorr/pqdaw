// Device panel: frame, body, controls and bindings (pq/views/devices/device_panel.h, T4.6).

#include "pq/views/devices/device_panel.h"

#include <algorithm>
#include <cmath>

#include "../common/paint.h"
#include "panel_internal.h"
#include "pq/ui/lines.h"
#include "pq/ui/theme.h"

namespace pq::views::devices {

namespace {

constexpr float kJackStripH = 10.0f;
constexpr float kTitleH = 20.0f;
constexpr float kPad = 6.0f;
constexpr float kFoldedW = 132.0f;
constexpr float kLabelH = 14.0f;  // a knob's label and value rows

// ---- knob: label, arc, value in one widget (a value change damages only this rect) -----------------------------------
class PanelKnob final : public ui::ValueControl {
public:
  explicit PanelKnob(std::string name) : ValueControl(std::move(name), {}) {
    setFocusable(true);
    focusRingOnPointerFocus_ = true;
    setCursor(ui::Cursor::ResizeNS);  // T4.16c: a vertical drag sets it (Live's knobs)
  }
  void bind(const ParamInfo& p, std::string label) {
    info_ = p;
    label_ = std::move(label);
    ui::ValueSpec s;
    s.min = 0.0;
    s.max = 1.0;
    s.def = toPosition(p, p.def);
    s.dragPixels = 160.0;
    if (p.steps > 1 && !p.exp && p.max > p.min) s.step = 1.0 / (p.max - p.min);
    s.keyStep = s.step > 0.0 ? s.step : 0.01;
    formatter = [this](double pos) { return formatValue(info_, fromPosition(info_, pos)); };
    parser = [this](std::string_view t) -> std::optional<double> {
      auto v = parseValue(info_, t);
      if (!v) return std::nullopt;
      return toPosition(info_, *v);
    };
    setSpec(s);
    setInfo(label_, "Drag up or down to change the value (Shift: fine). Delete returns it to its default.");
    markDirty();
  }
  const ParamInfo& param() const noexcept { return info_; }
  void setModulation(const ui::ModulationRing& m) {
    if (m == mod_) return;
    mod_ = m;
    markDirty();
  }

  bool onPointer(const ui::PointerEvent& e) override {
    switch (e.type) {
      case ui::PointerType::Down:
        if (e.button != ui::PointerButton::Left) return false;
        if (e.clickCount >= 2) {  // T4.16-fix1: the default value, as ui::Knob
          resetToDefault();
          return true;
        }
        dragStart(e.pos);
        return true;
      case ui::PointerType::Move: {
        if (!dragging()) return false;
        const double s = tree() ? tree()->scale() : 1.0;
        dragMove(e.pos, e.mods, (spec_.max - spec_.min) / (spec_.dragPixels * s), true);
        return true;
      }
      case ui::PointerType::Up:
        if (e.button != ui::PointerButton::Left || !dragging()) return false;
        dragEnd();
        return true;
      case ui::PointerType::Wheel: return handleWheel(e);  // T4.16-fix1: one step per notch, a run = one undo step
      default: return false;
    }
  }
  // T4.17: a number typed into the focused knob: a type-in over its value row (Enter commits, Esc cancels).
  bool onKey(const ui::KeyEvent& e) override { return typeInKey(e) || ValueControl::onKey(e); }
  bool onText(const ui::TextEvent& e) override { return typeInText(e); }
  void onFocusChanged(bool focused) override { typeInFocus(focused); }
  void paintOver(ui::PaintContext& ctx) override {
    const ui::IRect r = rect();
    const int32_t lh = ctx.scale.px(kLabelH);
    paintTypeIn(ctx, {r.x, r.bottom() - lh, r.w, lh});
  }

  void paint(ui::PaintContext& ctx) override {
    const ui::Theme* th = ctx.res.theme;
    const ui::ThemeFonts* f = ctx.res.uiFonts;
    if (!th) return;
    const ui::VisualState v = visualState();
    const ui::IRect r = rect();
    const int32_t lh = ctx.scale.px(kLabelH);
    const ui::IRect top{r.x, r.y, r.w, lh}, bottom{r.x, r.bottom() - lh, r.w, lh};
    if (f) {
      paint::text(ctx, f->small, top, label_, th->device.label, ui::TextAlign::Center, 1, 1);
      paint::text(ctx, f->numeric, bottom, valueText(), v.disabled ? th->text.disabled : th->device.value, ui::TextAlign::Center, 1, 1);
    }
    const ui::IRect mid{r.x, r.y + lh, r.w, std::max(0, r.h - 2 * lh)};
    const ui::Vec2 c{static_cast<float>(mid.x) + 0.5f * static_cast<float>(mid.w), static_cast<float>(mid.y) + 0.5f * static_cast<float>(mid.h)};
    const float thick = ctx.scale.exact(2.5f);
    const float rad = 0.5f * static_cast<float>(std::min(mid.w, mid.h)) - ctx.scale.exact(2.0f) - 0.5f * thick;
    if (rad >= 2.0f) {
      const float a0 = ui::Knob::kStartAngle, sweep = ui::Knob::kSweep;
      const float n = static_cast<float>(normalized());
      const float av = a0 + sweep * n;
      float anchor = a0;
      // Bipolar ranges grow the arc from 0 (balance, gain in dB around 0).
      const double d0 = toDisplay(info_, info_.min), d1 = toDisplay(info_, info_.max);
      if (!info_.isBool && d0 < 0.0 && d1 > 0.0 && !info_.exp) anchor = a0 + sweep * static_cast<float>(-d0 / (d1 - d0));
      const ui::Rgba track = v.hover || v.pressed ? ui::lerp(th->control.track, th->control.needle, 0.18f) : th->control.track;
      const ui::Rgba value = v.disabled ? th->control.valueDisabled : v.pressed ? paint::hoverOf(th->device.knob) : th->device.knob;
      ctx.dl.addArcStroke(c, rad, a0, a0 + sweep, track, thick);
      if (std::fabs(av - anchor) > 1e-4f) ctx.dl.addArcStroke(c, rad, std::min(anchor, av), std::max(anchor, av), value, thick);
      const ui::Vec2 d{std::cos(av), std::sin(av)}, nrm{-d.y, d.x};
      const float hw = 0.5f * ctx.scale.exact(1.75f);
      const float i0 = rad * 0.2f, i1 = rad - 0.5f * thick;
      const ui::Vec2 quad[4] = {{c.x + d.x * i0 + nrm.x * hw, c.y + d.y * i0 + nrm.y * hw},
                                {c.x + d.x * i1 + nrm.x * hw, c.y + d.y * i1 + nrm.y * hw},
                                {c.x + d.x * i1 - nrm.x * hw, c.y + d.y * i1 - nrm.y * hw},
                                {c.x + d.x * i0 - nrm.x * hw, c.y + d.y * i0 - nrm.y * hw}};
      ctx.dl.addConvexPolyFilled(quad, v.disabled ? th->text.disabled : th->control.needle);
      // T4.7's modulation ring (a signal display: PQ Meter's accent), outside the track: the range the wires can
      // sweep faint, what the modulation swept since the last poll bright.
      if (mod_.active) {
        const float mr = rad + 0.5f * thick + ctx.scale.exact(2.0f);
        const float m0 = a0 + sweep * std::clamp(std::min(mod_.lo, mod_.hi), 0.0f, 1.0f);
        const float m1 = a0 + sweep * std::clamp(std::max(mod_.lo, mod_.hi), 0.0f, 1.0f);
        if (m1 - m0 > 1e-4f)
          ctx.dl.addArcStroke(c, mr, m0, m1, ui::withAlpha(ui::signal_theme::kAccent, 90), ctx.scale.exact(1.5f), ui::LineCap::Round);
        if (mod_.liveHi >= mod_.liveLo) {
          const float l0 = a0 + sweep * std::clamp(mod_.liveLo, 0.0f, 1.0f), l1 = a0 + sweep * std::clamp(mod_.liveHi, 0.0f, 1.0f);
          if (l1 - l0 > 1e-4f)
            ctx.dl.addArcStroke(c, mr, l0, l1, ui::withAlpha(ui::signal_theme::kAccent, 235), ctx.scale.exact(2.0f), ui::LineCap::Round);
        }
        if (mod_.current >= 0.0f) {
          const float am = a0 + sweep * std::clamp(mod_.current, 0.0f, 1.0f);
          ctx.dl.addCircleFilled({c.x + std::cos(am) * mr, c.y + std::sin(am) * mr}, ctx.scale.exact(2.0f), ui::signal_theme::kAccent);
        }
      }
    }
    if (v.focus) paint::box(ctx.dl, r, 0.0f, 0, th->accent.focus);
  }

private:
  ParamInfo info_;
  std::string label_;
  ui::ModulationRing mod_;
};

// ---- number box with a label above (Live's slider box) ----------------------------------------------------------------
class PanelBox final : public ui::Widget {
public:
  explicit PanelBox(std::string name) : Widget(std::move(name)) {
    setLayout(ui::LayoutKind::Column);
    label_ = &add<ui::Label>("label", "", ui::FontRoleId::Small, ui::TextTone::Secondary, ui::TextAlign::Center);
    label_->setHeight(ui::Length::fixed(kLabelH));
    box_ = &add<ui::DragBox>("box", ui::ValueSpec{});
    box_->setHeight(ui::Length::fixed(16.0f));
  }
  ui::DragBox& box() const noexcept { return *box_; }
  ui::Label& label() const noexcept { return *label_; }
  std::string boundTo;  // the param / const the box is set up for (rebinding re-specs it only when this changes)

private:
  ui::Label* label_;
  ui::DragBox* box_;
};

// ---- EQ filter-type chooser: the item as a filter-shape glyph (the menu lists the names) ----------------------------------
class FilterChooser final : public ui::Dropdown {
public:
  FilterChooser(std::string name, std::vector<std::string> items) : Dropdown(std::move(name), std::move(items), 0) {}
  void paint(ui::PaintContext& ctx) override {
    const ui::Theme* th = ctx.res.theme;
    if (!th) return;
    const ui::VisualState v = visualState();
    const ui::IRect r = rect();
    const ui::Rgba fill = v.hover || v.pressed || menuOpen() ? th->field.fillHover : th->field.fill;
    paint::box(ctx.dl, r, 0.0f, fill, v.focus || menuOpen() ? th->field.borderFocus : th->field.border);
    // The glyph: magnitude (0 = bottom … 1 = top) over x ∈ [0, 1], a log-ish frequency axis.
    const float gx0 = static_cast<float>(r.x) + ctx.scale.exact(7.0f), gx1 = static_cast<float>(r.right()) - ctx.scale.exact(7.0f);
    const float gy0 = static_cast<float>(r.y) + ctx.scale.exact(4.0f), gy1 = static_cast<float>(r.bottom()) - ctx.scale.exact(4.0f);
    constexpr int kN = 24;
    ui::Vec2 pts[kN];
    const int type = index();
    for (int i = 0; i < kN; ++i) {
      const float u = static_cast<float>(i) / (kN - 1);
      float m = 0.5f;
      switch (type) {
        case 0: m = u < 0.35f ? 0.5f * std::pow(u / 0.35f, 4.0f) : 0.5f; break;                 // low cut 48
        case 1: m = u < 0.45f ? 0.5f * std::sqrt(u / 0.45f) : 0.5f; break;                      // low cut 12
        case 2: m = 0.5f + 0.35f / (1.0f + std::exp((u - 0.4f) * 14.0f)); break;               // low shelf
        case 3: m = 0.5f + 0.4f * std::exp(-(u - 0.5f) * (u - 0.5f) * 40.0f); break;           // bell
        case 4: m = 0.5f - 0.45f * std::exp(-(u - 0.5f) * (u - 0.5f) * 120.0f); break;         // notch
        case 5: m = 0.5f + 0.35f / (1.0f + std::exp(-(u - 0.6f) * 14.0f)); break;              // high shelf
        case 6: m = u > 0.55f ? 0.5f * std::sqrt((1.0f - u) / 0.45f) : 0.5f; break;            // high cut 12
        default: m = u > 0.65f ? 0.5f * std::pow((1.0f - u) / 0.35f, 4.0f) : 0.5f; break;      // high cut 48
      }
      pts[i] = {gx0 + u * (gx1 - gx0), gy1 - m * (gy1 - gy0)};
    }
    ui::smoothPolyline(ctx.dl, pts, kN, v.disabled ? th->text.disabled : th->text.primary, ctx.scale.exact(1.25f));
  }
};

// ---- tabs: one latching button per selector value ------------------------------------------------------------------------
class PanelTabs final : public ui::Widget {
public:
  PanelTabs(std::string name, DevicePanel::Impl& impl, const SelectorDesc& sel) : Widget(std::move(name)), impl_(impl), sel_(sel) {
    setLayout(ui::LayoutKind::Row);
    setGap(2);
    for (size_t i = 0; i < sel.values.size(); ++i) {
      auto& t = add<ui::Toggle>("tab" + std::to_string(i), sel.values[i].empty() ? std::string("A") : sel.values[i]);
      t.setWidth(ui::Length::flex(1.0f));
      const int32_t idx = static_cast<int32_t>(i);
      t.onToggle = [this, idx](bool) { impl_.userSelect(sel_.name, idx); };
    }
  }
  void show(int32_t index) {
    for (size_t i = 0; i < childCount(); ++i) static_cast<ui::Toggle&>(child(i)).setOn(static_cast<int32_t>(i) == index);
  }
  const SelectorDesc& selector() const noexcept { return sel_; }

private:
  DevicePanel::Impl& impl_;
  const SelectorDesc& sel_;
};

// ---- the frame's parts ----------------------------------------------------------------------------------------------------
class JackStrip final : public ui::Widget {
public:
  explicit JackStrip(DevicePanel& panel) : Widget("jacks"), panel_(panel) {}
  void paint(ui::PaintContext& ctx) override {
    const ui::Theme* th = ctx.res.theme;
    if (!th) return;
    const ui::IRect r = rect();
    const float rad = ctx.scale.exact(3.0f);
    for (size_t i = 0; i < jacks().size(); ++i) {
      const ui::IRect& j = jacks()[i].rect;
      const ui::Vec2 c{static_cast<float>(j.x) + 0.5f * static_cast<float>(j.w), static_cast<float>(j.y) + 0.5f * static_cast<float>(j.h)};
      ctx.dl.addCircleFilled(c, rad + ctx.scale.exact(1.0f), static_cast<int32_t>(i) == hover_ ? th->device.jackRingHover : th->device.jackRing);
      ctx.dl.addCircleFilled(c, rad, th->device.jack);
    }
    (void)r;
  }
  bool onPointer(const ui::PointerEvent& e) override {
    const int32_t at = jackAt(e.pos);
    if (pressed_ && (e.type == ui::PointerType::Move || e.type == ui::PointerType::Up)) {  // T4.7: a cable drag
      if (e.type == ui::PointerType::Up) pressed_ = false;
      if (panel_.onJackDrag) panel_.onJackDrag(e);
      return true;
    }
    if (e.type == ui::PointerType::Move) {
      if (at != hover_) {
        hover_ = at;
        tip_ = at >= 0 ? jacks()[static_cast<size_t>(at)].name : std::string();
        markDirty();
      }
      return at >= 0;
    }
    if (e.type == ui::PointerType::Down && e.button == ui::PointerButton::Left && at >= 0) {
      pressed_ = true;
      if (panel_.onJackPressed) panel_.onJackPressed(jacks()[static_cast<size_t>(at)].name, e.pos);
      return true;
    }
    return false;
  }
  void onCaptureLost() override { pressed_ = false; }
  // T4.16c: a jack starts a cable (drag it onto a knob).
  ui::Cursor cursorAt(ui::Vec2 p) const override {
    return pressed_ || jackAt(p) >= 0 ? ui::Cursor::Crosshair : ui::Cursor::Inherit;
  }
  void onHoverChanged(bool h) override {
    if (!h && hover_ >= 0) {
      hover_ = -1;
      markDirty();
    }
  }
  std::string_view tooltip() const override { return tip_; }
  ui::InfoText info() const override { return {tip_.empty() ? std::string_view("Output jacks") : std::string_view(tip_), kJackInfo}; }

private:
  static constexpr std::string_view kJackInfo =
      "A signal the device exports: drag it onto any knob of any device to modulate it (on another track: hover its "
      "row in the arrangement first).";
  std::span<const DevicePanel::Jack> jacks() const { return panel_.jacks(); }
  int32_t jackAt(ui::Vec2 p) const {
    for (size_t i = 0; i < jacks().size(); ++i)
      if (jacks()[i].rect.contains(p)) return static_cast<int32_t>(i);
    return -1;
  }
  DevicePanel& panel_;
  int32_t hover_ = -1;
  bool pressed_ = false;
  std::string tip_;
};

class TitleBar final : public ui::Widget {
public:
  TitleBar(DevicePanel& panel, DevicePanel::Impl& impl, std::string title)
      : Widget("title"), panel_(panel), impl_(impl), title_(std::move(title)) {
    setLayout(ui::LayoutKind::Custom);  // T4.17: the rename field over the title
  }
  const std::string& title() const noexcept { return title_; }
  void setTitle(std::string t) {
    if (t == title_) return;
    title_ = std::move(t);
    markDirty();
  }
  bool renaming() const noexcept { return rename_ != nullptr && !renameDone_; }
  void beginRename(std::function<void(const std::string&)> commit) {
    if (rename_ != nullptr) return;
    renameCommit_ = std::move(commit);
    rename_ = &add<ui::TextEdit>("rename", title_);
    renameDone_ = false;
    rename_->buffer().selectAll();
    rename_->onCommit = [this](const std::string&) { endRename(true); };
    rename_->onCancel = [this] { endRename(false); };
    invalidateArrange();
    markDirty();
    if (ui::UiTree* t = tree()) {
      t->layoutNow();
      t->setFocus(rename_, true);
    }
  }
  void arrange(ui::LayoutPass& pass) override {
    if (rename_ == nullptr) return;
    const ui::IRect r = rect(), fr = foldRect(), p = presetRect();
    const int32_t x = fr.right() + pass.px(2.0f);
    pass.place(*rename_, {x, r.y + 1, std::max(0, (p.w > 0 ? p.x - pass.px(4.0f) : r.right() - pass.px(4.0f)) - x), r.h - 2});
  }
  void onFrame(double) override {
    if (rename_ != nullptr && renameDone_) {
      (void)removeChild(*rename_);
      rename_ = nullptr;
      renameDone_ = false;
      markDirty();
    }
  }
  void paint(ui::PaintContext& ctx) override {
    const ui::Theme* th = ctx.res.theme;
    const ui::ThemeFonts* f = ctx.res.uiFonts;
    if (!th) return;
    const ui::IRect r = rect();
    ctx.dl.addRectFilled(r.toRect(), th->device.titleBar);
    // Activator: a filled square, yellow when on (Live's device activator).
    const ui::IRect a = activatorRect();
    paint::box(ctx.dl, a, ctx.scale.exact(1.0f), panel_.active() ? th->device.activatorOn : th->device.activatorOff, th->device.border);
    // Fold triangle: pointing down when open, right when folded.
    const ui::IRect fr = foldRect();
    const float cx = static_cast<float>(fr.x) + 0.5f * static_cast<float>(fr.w), cy = static_cast<float>(fr.y) + 0.5f * static_cast<float>(fr.h);
    const float s = ctx.scale.exact(3.5f);
    const ui::Rgba tc = hoverFold_ ? th->device.titleText : th->device.label;
    if (panel_.folded()) ctx.dl.addTriangleFilled({cx - 0.6f * s, cy - s}, {cx + s, cy}, {cx - 0.6f * s, cy + s}, tc);
    else ctx.dl.addTriangleFilled({cx - s, cy - 0.6f * s}, {cx + s, cy - 0.6f * s}, {cx, cy + s}, tc);
    // Preset slot (placeholder until the M5 browser): a sunken field on the right.
    const ui::IRect p = presetRect();
    if (f) {
      ui::IRect t{fr.right() + ctx.scale.px(4), r.y, std::max(0, (p.w > 0 ? p.x : r.right()) - fr.right() - ctx.scale.px(8)), r.h};
      // T4.4-fix1: a user device — "● edited" after the title, in the accent colour (room kept for it first).
      static constexpr std::string_view kEdited = "\xE2\x97\x8F edited";
      int32_t markW = 0;
      if (panel_.edited() && ctx.res.text) markW = ctx.res.text->measure(f->small, kEdited) + ctx.scale.px(6);
      if (markW > t.w / 2) markW = 0;  // too narrow (folded): the title alone
      t.w -= markW;
      const int32_t adv = rename_ != nullptr ? 0 :  // T4.17: the rename field draws the name
          paint::text(ctx, f->bodyStrong, t, title_, panel_.active() ? th->device.titleText : th->device.titleTextOff, ui::TextAlign::Left);
      if (markW > 0 && rename_ == nullptr)
        paint::text(ctx, f->small, {t.x + adv + ctx.scale.px(6), r.y, markW, r.h}, kEdited,
                    panel_.active() ? th->accent.primary : th->device.titleTextOff, ui::TextAlign::Left);
      if (p.w > 0) {  // T5.11: the preset name; a click hot-swaps (the host's onPresetClick)
        const bool live = static_cast<bool>(panel_.onPresetClick);
        paint::box(ctx.dl, p, 0.0f, hoverPreset_ && live ? th->surface.raised : th->field.fill, th->device.border);
        const std::string& name = panel_.presetName();
        paint::text(ctx, f->small, p, name.empty() ? std::string("Default") : name,
                    live ? th->device.titleText : th->text.disabled, ui::TextAlign::Left, ctx.scale.px(4), ctx.scale.px(4));
      }
    }
  }
  bool onPointer(const ui::PointerEvent& e) override {
    if (e.type == ui::PointerType::Move) {
      const bool h = foldRect().contains(e.pos);
      const bool hp = presetRect().contains(e.pos);
      if (h != hoverFold_ || hp != hoverPreset_) {
        hoverFold_ = h;
        hoverPreset_ = hp;
        markDirty();
      }
      return false;
    }
    if (e.type != ui::PointerType::Down || e.button != ui::PointerButton::Left) return false;
    if (panel_.onPresetClick && presetRect().contains(e.pos)) {  // T5.11: hot-swap
      panel_.onPresetClick();
      return true;
    }
    if (activatorRect().contains(e.pos)) {
      impl_.userActive(!panel_.active());
      return true;
    }
    if (foldRect().contains(e.pos)) {
      panel_.setFolded(!panel_.folded());
      return true;
    }
    if (e.clickCount == 2) {  // T4.4-fix1: the host's action (Open in Circuit View), else fold
      if (panel_.onTitleDoubleClick) panel_.onTitleDoubleClick();
      else panel_.setFolded(!panel_.folded());
      return true;
    }
    return false;
  }
  void onHoverChanged(bool h) override {
    if (!h && (hoverFold_ || hoverPreset_)) {
      hoverFold_ = false;
      hoverPreset_ = false;
      markDirty();
    }
  }
  // T4.16c: the activator and the fold triangle are buttons; the rest of the bar is the host's (the Device view: a drag
  // there moves the device). T5.11: the preset slot too, when it hot-swaps.
  ui::Cursor cursorAt(ui::Vec2 p) const override {
    return activatorRect().contains(p) || foldRect().contains(p) || (panel_.onPresetClick && presetRect().contains(p))
               ? ui::Cursor::Pointer
               : ui::Cursor::Inherit;
  }
  ui::IRect presetSlot() const { return presetRect(); }
  ui::InfoText info() const override {
    if (panel_.onTitleDoubleClick)
      return {title_ + (panel_.edited() ? " (edited)" : ""),
              "Device title bar: the square switches the device on or off; the triangle folds it; a double-click opens "
              "the device in the Circuit View (editing a stock device makes it this track's own, edited copy)."};
    return {title_, "Device title bar: the square switches the device on or off; the triangle (or a double-click) folds it."};
  }

private:
  ui::IRect activatorRect() const {
    const ui::IRect r = rect();
    const float s = tree() ? tree()->scale() : 1.0f;
    const int32_t sz = static_cast<int32_t>(std::lround(10.0f * s));
    return {r.x + static_cast<int32_t>(std::lround(5.0f * s)), r.y + (r.h - sz) / 2, sz, sz};
  }
  ui::IRect foldRect() const {
    const ui::IRect a = activatorRect();
    const float s = tree() ? tree()->scale() : 1.0f;
    const int32_t sz = static_cast<int32_t>(std::lround(12.0f * s));
    return {a.right() + static_cast<int32_t>(std::lround(4.0f * s)), rect().y + (rect().h - sz) / 2, sz, sz};
  }
  ui::IRect presetRect() const {
    const ui::IRect r = rect();
    const float s = tree() ? tree()->scale() : 1.0f;
    const int32_t w = static_cast<int32_t>(std::lround(64.0f * s));
    if (panel_.folded() || r.w < w * 3) return {};
    const int32_t h = static_cast<int32_t>(std::lround(14.0f * s));
    return {r.right() - w - static_cast<int32_t>(std::lround(5.0f * s)), r.y + (r.h - h) / 2, w, h};
  }
  void endRename(bool commit) {
    if (rename_ == nullptr || renameDone_) return;
    renameDone_ = true;
    const std::string text = rename_->text();
    requestFrame();  // removed at the next frame: this may run inside the field's own callback
    if (ui::UiTree* t = tree(); t != nullptr && t->focus() == rename_) {
      ui::Widget* back = parent();
      while (back != nullptr && !back->focusable()) back = back->parent();
      t->setFocus(back);
    }
    if (commit && !text.empty() && text != title_ && renameCommit_) renameCommit_(text);
  }

  DevicePanel& panel_;
  DevicePanel::Impl& impl_;
  std::string title_;
  bool hoverFold_ = false;
  bool hoverPreset_ = false;
  ui::TextEdit* rename_ = nullptr;  // T4.17
  bool renameDone_ = false;
  std::function<void(const std::string&)> renameCommit_;
};

// The body: controls at their description rects (logical px × UI scale).
class PanelBody final : public ui::Widget {
public:
  explicit PanelBody(DevicePanel& panel) : Widget("body"), panel_(panel) {
    setLayout(ui::LayoutKind::Custom);
    setClipChildren(true);
  }
  void place(ui::Widget& w, const ControlDesc& d) { placed_.push_back({&w, &d}); }
  void arrange(ui::LayoutPass& pass) override {
    const ui::IRect r = rect();
    const int32_t pad = pass.px(kPad);
    for (const Placed& p : placed_) {
      if (!p.widget->visible()) continue;
      const int32_t x0 = pass.px(p.desc->x), y0 = pass.px(p.desc->y);
      const int32_t x1 = pass.px(p.desc->x + p.desc->w), y1 = pass.px(p.desc->y + p.desc->h);
      pass.place(*p.widget, {r.x + pad + x0, r.y + pad + y0, x1 - x0, y1 - y0});
    }
  }
  void paint(ui::PaintContext& ctx) override {
    if (const ui::Theme* th = ctx.res.theme) ctx.dl.addRectFilled(rect().toRect(), th->device.background);
  }
  void paintOver(ui::PaintContext& ctx) override {
    // A switched-off device: its controls stay editable (as in Live) but look dimmed.
    if (!panel_.active())
      if (const ui::Theme* th = ctx.res.theme) ctx.dl.addRectFilled(rect().toRect(), ui::withAlpha(th->device.background, 120));
  }

private:
  struct Placed {
    ui::Widget* widget;
    const ControlDesc* desc;
  };
  DevicePanel& panel_;
  std::vector<Placed> placed_;
};

}  // namespace

bool shownInTree(const ui::Widget& w) noexcept {
  if (!w.tree()) return false;
  for (const ui::Widget* p = &w; p; p = p->parent())
    if (!p->visible()) return false;
  return true;
}

// ---- Impl ------------------------------------------------------------------------------------------------------------

std::string DevicePanel::Impl::resolve(std::string_view templ) const { return resolveTemplate(templ, selValues); }

double DevicePanel::Impl::value(std::string_view name) const {
  if (!name.empty() && name[0] == '#') {
    const std::string n = resolve(name.substr(1));
    const auto it = consts.find(n);
    return it != consts.end() ? it->second : 0.0;
  }
  const std::string n = resolve(name);
  const auto it = params.find(n);
  if (it != params.end()) return it->second;
  const auto c = consts.find(n);
  return c != consts.end() ? c->second : 0.0;
}

void DevicePanel::Impl::userParamBegin(const std::string& name) {
  if (panel.target_) panel.target_->paramGestureBegin(name);
}

void DevicePanel::Impl::userParamEnd(const std::string& name) {
  if (panel.target_) panel.target_->paramGestureEnd(name);
}

void DevicePanel::Impl::userParam(const std::string& name, double v, ui::Widget* source) {
  if (suppress) return;
  params[name] = v;
  if (panel.target_) panel.target_->setParam(name, v);
  changed(name, false, source);
}

void DevicePanel::Impl::userConst(const std::string& name, double v, ui::Widget* source) {
  if (suppress) return;
  const auto it = consts.find(name);
  if (it != consts.end() && it->second == v) return;
  consts[name] = v;
  if (panel.target_) panel.target_->setConst(name, v);
  changed(name, true, source);
}

void DevicePanel::Impl::userActive(bool on) {
  panel.setActive(on);
  if (panel.target_) panel.target_->setActive(on);
}

void DevicePanel::Impl::userSelect(const std::string& selector, int32_t index) {
  if (suppress) return;
  panel.select(selector, index);
}

void DevicePanel::Impl::changed(std::string_view name, bool isConst, ui::Widget* source) {
  suppress = true;
  bool visibility = false;
  const std::string key = isConst ? "#" + std::string(name) : std::string(name);
  for (Bound& b : bound) {
    if (b.display) {
      if (b.display->dependsOn(key)) b.display->valuesChanged();
    } else if (b.widget != source && ((!isConst && b.param == name) || (isConst && b.constName == name))) {
      bindOne(b);
    }
    for (const Condition& c : b.desc->when)
      if (c.source != Condition::Source::Selector && resolve(c.name) == name &&
          (c.source == Condition::Source::Const) == isConst)
        visibility = true;
  }
  suppress = false;
  if (visibility) refreshVisibility();
}

bool DevicePanel::Impl::conditionsHold(const ControlDesc& d) const {
  for (const Condition& c : d.when) {
    if (c.source == Condition::Source::Selector) {
      const auto it = selValues.find(c.name);
      const std::string cur = it != selValues.end() ? it->second : std::string();
      if (std::find(c.keys.begin(), c.keys.end(), cur) == c.keys.end()) return false;
      continue;
    }
    const std::string n = resolve(c.name);
    double v = 0.0;
    if (c.source == Condition::Source::Const) {
      const auto it = consts.find(n);
      v = it != consts.end() ? it->second : 0.0;
    } else {
      const auto it = params.find(n);
      v = it != params.end() ? it->second : 0.0;
    }
    bool any = false;
    for (double x : c.values) any = any || std::fabs(x - v) < 1e-9 * std::max(1.0, std::fabs(x));
    if (!any) return false;
  }
  return true;
}

void DevicePanel::Impl::refreshVisibility() {
  bool any = false;
  for (Bound& b : bound) {
    const bool show = conditionsHold(*b.desc);
    if (b.widget->visible() != show) {
      b.widget->setVisible(show);
      any = true;
    }
  }
  if (any) panel.body_->invalidateArrange();
}

void DevicePanel::Impl::bindOne(Bound& b) {
  const ControlDesc& d = *b.desc;
  b.param = d.param.empty() ? std::string() : resolve(d.param);
  b.constName = d.constName.empty() ? std::string() : resolve(d.constName);
  const ParamInfo* p = b.param.empty() ? nullptr : paramInfo(b.param);
  const double v = p ? params[b.param] : (!b.constName.empty() ? consts[b.constName] : 0.0);
  switch (d.kind) {
    case ControlKind::Knob: {
      auto& k = static_cast<PanelKnob&>(*b.widget);
      if (p) {
        if (k.param().name != p->name) k.bind(*p, d.label.empty() ? p->label : d.label);
        k.setValue(toPosition(*p, v));
      }
      break;
    }
    case ControlKind::Box: {
      auto& pb = static_cast<PanelBox&>(*b.widget);
      ui::DragBox& box = pb.box();
      if (p && pb.boundTo == p->name) {
        box.setValue(toPosition(*p, v));
      } else if (p) {
        pb.boundTo = p->name;
        const ParamInfo info = *p;
        ui::ValueSpec s;
        s.min = 0.0;
        s.max = 1.0;
        s.def = toPosition(info, info.def);
        s.dragPixels = 160.0;
        if (info.steps > 1 && !info.exp && info.max > info.min) s.step = 1.0 / (info.max - info.min);
        s.keyStep = s.step > 0.0 ? s.step : 0.01;
        box.formatter = [info](double pos) { return formatValue(info, fromPosition(info, pos)); };
        box.parser = [info](std::string_view t) -> std::optional<double> {
          auto x = parseValue(info, t);
          if (!x) return std::nullopt;
          return toPosition(info, *x);
        };
        box.setSpec(s);
        box.setValue(toPosition(info, v));
        pb.label().setText(d.label.empty() ? info.label : d.label);
        box.setInfo(info.label, "Drag up or down, or double-click to type a value. Delete returns it to its default.");
      } else if (pb.boundTo == "#" + b.constName) {
        box.setValue(v);
      } else {
        pb.boundTo = "#" + b.constName;
        ui::ValueSpec s;
        const ConstInfo* c = panel.sig_.findConst(b.constName);
        s.min = 0.0;
        s.max = std::max(16.0, (c ? c->def : 0.0) * 4.0);
        s.def = c ? c->def : 0.0;
        s.step = 1.0;
        s.decimals = 0;
        box.formatter = nullptr;
        box.parser = nullptr;
        box.setSpec(s);
        box.setValue(v);
        pb.label().setText(d.label.empty() ? b.constName : d.label);
        box.setInfo(b.constName, "A structural setting: changing it recompiles the device (crossfaded while it plays).");
      }
      break;
    }
    case ControlKind::Toggle: {
      auto& t = static_cast<ui::Toggle&>(*b.widget);
      t.setOn(std::fabs(v - d.values[1]) < 1e-9);
      break;
    }
    case ControlKind::Chooser: {
      auto& c = static_cast<ui::Dropdown&>(*b.widget);
      int32_t idx = 0;
      double best = 1e300;
      for (size_t i = 0; i < d.values.size(); ++i)
        if (std::fabs(d.values[i] - v) < best) {
          best = std::fabs(d.values[i] - v);
          idx = static_cast<int32_t>(i);
        }
      c.setIndex(idx);
      break;
    }
    case ControlKind::Tabs: {
      auto& t = static_cast<PanelTabs&>(*b.widget);
      const auto it = selection.find(d.selector);
      t.show(it != selection.end() ? it->second : 0);
      break;
    }
    case ControlKind::Visual:
      if (b.display) {
        b.display->rebind();
        b.display->valuesChanged();
      }
      break;
    case ControlKind::Label: break;
  }
}

void DevicePanel::Impl::bindAll() {
  suppress = true;
  for (Bound& b : bound) bindOne(b);
  suppress = false;
  refreshVisibility();
}

// ---- DevicePanel ------------------------------------------------------------------------------------------------------

DevicePanel::DevicePanel(std::string name, DeviceSignature sig, PanelDesc desc, DeviceTarget* target,
                         const std::map<std::string, double, std::less<>>& values)
    : Widget(std::move(name)), sig_(std::move(sig)), desc_(std::move(desc)), target_(target), impl_(std::make_unique<Impl>(*this)) {
  Impl& im = *impl_;
  for (const ParamInfo& p : sig_.params) {
    const auto it = values.find(p.name);
    im.params[p.name] = it != values.end() ? it->second : p.def;
  }
  for (const ConstInfo& c : sig_.consts) {
    const auto it = values.find(c.name);
    im.consts[c.name] = it != values.end() ? it->second : c.def;
  }
  for (const SelectorDesc& s : desc_.selectors) {
    im.selection[s.name] = s.def;
    im.selValues[s.name] = s.values[static_cast<size_t>(s.def)];
  }

  setLayout(ui::LayoutKind::Column);
  setClipChildren(true);
  const float w = desc_.width + 2 * kPad;
  setWidth(ui::Length::fixed(w));
  if (!sig_.exports.empty()) {
    auto& j = add<JackStrip>(*this);
    j.setHeight(ui::Length::fixed(kJackStripH));
    for (const std::string& e : sig_.exports) jacks_.push_back({e, {}});
  }
  title_ = &add<TitleBar>(*this, im, desc_.title);
  title_->setHeight(ui::Length::fixed(kTitleH));
  auto& body = add<PanelBody>(*this);
  body.setHeight(ui::Length::fixed(desc_.height + 2 * kPad));
  body_ = &body;

  for (const ControlDesc& d : desc_.controls) {
    Impl::Bound b;
    b.desc = &d;
    switch (d.kind) {
      case ControlKind::Knob: {
        auto& k = body.add<PanelKnob>(d.id);
        Impl* ip = &im;
        k.onGestureBegin = [ip, &k] {
          for (Impl::Bound& x : ip->bound)
            if (x.widget == &k) ip->userParamBegin(x.param);
        };
        k.onChange = [ip, &k](double pos) {
          for (Impl::Bound& x : ip->bound)
            if (x.widget == &k) ip->userParam(x.param, fromPosition(k.param(), pos), &k);
        };
        k.onGestureEnd = [ip, &k] {
          for (Impl::Bound& x : ip->bound)
            if (x.widget == &k) ip->userParamEnd(x.param);
        };
        b.widget = &k;
        break;
      }
      case ControlKind::Box: {
        auto& pb = body.add<PanelBox>(d.id);
        Impl* ip = &im;
        ui::DragBox* box = &pb.box();
        PanelBox* pbp = &pb;
        box->onGestureBegin = [ip, pbp] {
          for (Impl::Bound& x : ip->bound)
            if (x.widget == pbp && !x.param.empty()) ip->userParamBegin(x.param);
        };
        box->onChange = [ip, pbp](double pos) {
          for (Impl::Bound& x : ip->bound) {
            if (x.widget != pbp) continue;
            if (!x.param.empty()) {
              if (const ParamInfo* p = ip->paramInfo(x.param)) ip->userParam(x.param, fromPosition(*p, pos), pbp);
            } else if (!x.constName.empty()) {
              ip->userConst(x.constName, pos, pbp);
            }
          }
        };
        box->onGestureEnd = [ip, pbp] {
          for (Impl::Bound& x : ip->bound)
            if (x.widget == pbp && !x.param.empty()) ip->userParamEnd(x.param);
        };
        b.widget = &pb;
        break;
      }
      case ControlKind::Toggle: {
        auto& t = body.add<ui::Toggle>(d.id, d.text.empty() ? (d.label.empty() ? d.param + d.constName : d.label) : d.text);
        Impl* ip = &im;
        ui::Toggle* tp = &t;
        const ControlDesc* dp = &d;
        t.onToggle = [ip, tp, dp](bool on) {
          for (Impl::Bound& x : ip->bound) {
            if (x.widget != tp) continue;
            const double v = on ? dp->values[1] : dp->values[0];
            if (!x.param.empty()) {
              ip->userParamBegin(x.param);
              ip->userParam(x.param, v, tp);
              ip->userParamEnd(x.param);
            } else {
              ip->userConst(x.constName, v, tp);
            }
          }
        };
        if (!d.info.empty()) t.setInfo(d.text.empty() ? d.id : d.text, d.info);
        else if (!d.constName.empty())
          t.setInfo(d.text.empty() ? d.constName : d.text, "A structural setting: switching it recompiles the device (crossfaded while it plays).");
        b.widget = &t;
        break;
      }
      case ControlKind::Chooser: {
        ui::Dropdown& c = d.style == "filter" ? static_cast<ui::Dropdown&>(body.add<FilterChooser>(d.id, d.items))
                                              : body.add<ui::Dropdown>(d.id, d.items, 0);
        Impl* ip = &im;
        ui::Dropdown* cp = &c;
        const ControlDesc* dp = &d;
        c.onChange = [ip, cp, dp](int32_t i) {
          const double v = dp->values[static_cast<size_t>(std::clamp<int32_t>(i, 0, static_cast<int32_t>(dp->values.size()) - 1))];
          for (Impl::Bound& x : ip->bound) {
            if (x.widget != cp) continue;
            if (!x.param.empty()) {
              ip->userParamBegin(x.param);
              ip->userParam(x.param, v, cp);
              ip->userParamEnd(x.param);
            } else {
              ip->userConst(x.constName, v, cp);
            }
          }
        };
        if (!d.info.empty()) c.setInfo(d.label.empty() ? d.id : d.label, d.info);
        else if (!d.constName.empty())
          c.setInfo(d.label.empty() ? d.constName : d.label, "A structural setting: changing it recompiles the device (crossfaded while it plays).");
        b.widget = &c;
        break;
      }
      case ControlKind::Label: {
        auto& l = body.add<ui::Label>(d.id, d.text, ui::FontRoleId::Small, ui::TextTone::Secondary, ui::TextAlign::Left);
        b.widget = &l;
        break;
      }
      case ControlKind::Tabs: {
        const SelectorDesc* s = desc_.findSelector(d.selector);
        auto& t = body.add<PanelTabs>(d.id, im, *s);
        b.widget = &t;
        break;
      }
      case ControlKind::Visual: {
        std::unique_ptr<PanelDisplay> disp = makeDisplay(im, d);
        b.display = disp.get();
        b.widget = &body.addChild(std::move(disp));
        break;
      }
    }
    body.place(*b.widget, d);
    im.bound.push_back(std::move(b));
  }
  im.bindAll();
}

DevicePanel::~DevicePanel() = default;

void DevicePanel::setParamValue(std::string_view param, double value) {
  const auto it = impl_->params.find(param);
  if (it == impl_->params.end() || it->second == value) return;
  it->second = value;
  impl_->changed(param, false, nullptr);
}

void DevicePanel::setConstValue(std::string_view name, double value) {
  const auto it = impl_->consts.find(name);
  if (it == impl_->consts.end() || it->second == value) return;
  it->second = value;
  impl_->changed(name, true, nullptr);
}

double DevicePanel::paramValue(std::string_view param) const {
  const auto it = impl_->params.find(param);
  return it != impl_->params.end() ? it->second : 0.0;
}

double DevicePanel::constValue(std::string_view name) const {
  const auto it = impl_->consts.find(name);
  return it != impl_->consts.end() ? it->second : 0.0;
}

void DevicePanel::select(std::string_view selector, int32_t index) {
  const SelectorDesc* s = desc_.findSelector(selector);
  if (!s) return;
  index = std::clamp(index, 0, static_cast<int32_t>(s->values.size()) - 1);
  int32_t& cur = impl_->selection[s->name];
  if (cur == index) {
    // Tabs flip on press: show the (unchanged) selection on this selector's tabs again.
    impl_->suppress = true;
    for (Impl::Bound& b : impl_->bound)
      if (b.desc->kind == ControlKind::Tabs && b.desc->selector == s->name) impl_->bindOne(b);
    impl_->suppress = false;
    return;
  }
  cur = index;
  impl_->selValues[s->name] = s->values[static_cast<size_t>(index)];
  // Rebind only what uses this selector (controls whose bindings or conditions name it, tabs, displays).
  const std::string key = "{" + s->name + "}";
  impl_->suppress = true;
  for (Impl::Bound& b : impl_->bound) {
    const ControlDesc& d = *b.desc;
    bool uses = d.param.find(key) != std::string::npos || d.constName.find(key) != std::string::npos ||
                (d.kind == ControlKind::Tabs && d.selector == s->name) || d.kind == ControlKind::Visual;
    for (const Condition& c : d.when) uses = uses || c.name == s->name || c.name.find(key) != std::string::npos;
    if (uses) impl_->bindOne(b);
  }
  impl_->suppress = false;
  impl_->refreshVisibility();
}

int32_t DevicePanel::selected(std::string_view selector) const {
  const auto it = impl_->selection.find(selector);
  return it != impl_->selection.end() ? it->second : 0;
}

void DevicePanel::setActive(bool on) {
  if (active_ == on) return;
  active_ = on;
  markDirty();
}

void DevicePanel::setTitle(std::string title) { static_cast<TitleBar*>(title_)->setTitle(std::move(title)); }
const std::string& DevicePanel::title() const noexcept { return static_cast<const TitleBar*>(title_)->title(); }
void DevicePanel::beginRename(std::function<void(const std::string&)> commit) {
  static_cast<TitleBar*>(title_)->beginRename(std::move(commit));
}
bool DevicePanel::renaming() const noexcept { return static_cast<const TitleBar*>(title_)->renaming(); }

void DevicePanel::setEdited(bool e) {
  if (edited_ == e) return;
  edited_ = e;
  markDirty();
}

void DevicePanel::setPresetName(std::string name) {
  if (presetName_ == name) return;
  presetName_ = std::move(name);
  if (title_) title_->markDirty();
}

ui::IRect DevicePanel::presetSlotRect() const {
  const auto* t = dynamic_cast<const TitleBar*>(title_);
  return t ? t->presetSlot() : ui::IRect{};
}

void DevicePanel::setFolded(bool f) {
  if (folded_ == f) return;
  folded_ = f;
  body_->setVisible(!f);
  setWidth(ui::Length::fixed(logicalWidth()));
  invalidateLayout();
  markDirty();
  if (onSizeChanged) onSizeChanged();
}

float DevicePanel::logicalWidth() const noexcept { return folded_ ? kFoldedW : desc_.width + 2 * kPad; }

float DevicePanel::logicalHeight() const noexcept {
  return (sig_.exports.empty() ? 0.0f : kJackStripH) + kTitleH + desc_.height + 2 * kPad;
}

void DevicePanel::wakeDisplays() {
  for (Impl::Bound& b : impl_->bound)
    if (b.display) b.display->requestFrame();
}

ui::Widget* DevicePanel::controlFor(std::string_view param) const noexcept {
  for (const Impl::Bound& b : impl_->bound)
    if (b.param == param && !b.display && b.widget->visible()) return b.widget;
  return nullptr;
}

void DevicePanel::setModulation(std::string_view param, const ui::ModulationRing& ring) {
  for (Impl::Bound& b : impl_->bound)
    if (b.param == param && b.desc->kind == ControlKind::Knob) static_cast<PanelKnob&>(*b.widget).setModulation(ring);
}

ui::Widget* DevicePanel::control(std::string_view id) const noexcept {
  for (const Impl::Bound& b : impl_->bound)
    if (b.desc->id == id) return b.widget;
  return nullptr;
}

std::span<const DevicePanel::Jack> DevicePanel::jacks() const noexcept {
  // Along the top edge from the left, from the strip's rect of the last layout.
  if (!jacks_.empty() && childCount() > 0) {
    const ui::IRect s = child(0).rect();
    const float sc = tree() ? tree()->scale() : 1.0f;
    auto px = [sc](float v) { return static_cast<int32_t>(std::lround(v * sc)); };
    const int32_t step = px(14.0f), sz = px(9.0f);
    for (size_t i = 0; i < jacks_.size(); ++i) jacks_[i].rect = {s.x + px(8.0f) + static_cast<int32_t>(i) * step, s.y + (s.h - sz) / 2, sz, sz};
  }
  return jacks_;
}

void DevicePanel::paint(ui::PaintContext& ctx) {
  const ui::Theme* th = ctx.res.theme;
  if (!th) return;
  // UI item 3: a panel taller than its controls (the Device view's height) continues its background below them.
  if (body_ != nullptr && body_->visible() && body_->rect().bottom() < rect().bottom())
    ctx.dl.addRectFilled(ui::IRect{rect().x, body_->rect().bottom(), rect().w, rect().bottom() - body_->rect().bottom()}.toRect(),
                         th->device.background);
  if (!jacks_.empty() && childCount() > 0) ctx.dl.addRectFilled(child(0).rect().toRect(), th->device.jackStrip);
}

void DevicePanel::paintOver(ui::PaintContext& ctx) {
  if (const ui::Theme* th = ctx.res.theme) paint::box(ctx.dl, rect(), 0.0f, 0, th->device.border);
}

}  // namespace pq::views::devices
