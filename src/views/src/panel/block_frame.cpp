// GU.13: a leaf block's (and a rack's macros') panel in a Live-style frame: pq/views/panel/block_frame.h.

#include "pq/views/panel/block_frame.h"

#include <algorithm>
#include <cmath>

#include "../common/paint.h"
#include "pq/document/meta.h"
#include "pq/document/modules.h"
#include "pq/meta/registry.h"
#include "pq/ui/controls.h"
#include "pq/ui/theme.h"

namespace pq::views::panel {

namespace {

// ---- the export jacks ---------------------------------------------------------------------------------------------
class JackStrip final : public ui::Widget {
public:
  explicit JackStrip(BlockFrame& frame) : Widget("jacks"), frame_(frame) {}
  void paint(ui::PaintContext& ctx) override {
    const ui::Theme* th = ctx.res.theme;
    if (!th) return;
    ctx.dl.addRectFilled(rect().toRect(), th->device.jackStrip);
    const float rad = ctx.scale.exact(3.0f);
    const std::span<const BlockFrame::Jack> js = frame_.jacks();
    for (size_t i = 0; i < js.size(); ++i) {
      const ui::IRect& j = js[i].rect;
      const ui::Vec2 c{static_cast<float>(j.x) + 0.5f * static_cast<float>(j.w), static_cast<float>(j.y) + 0.5f * static_cast<float>(j.h)};
      ctx.dl.addCircleFilled(c, rad + ctx.scale.exact(1.0f), static_cast<int32_t>(i) == hover_ ? th->device.jackRingHover : th->device.jackRing);
      ctx.dl.addCircleFilled(c, rad, th->device.jack);
    }
  }
  bool onPointer(const ui::PointerEvent& e) override {
    const int32_t at = jackAt(e.pos);
    if (pressed_ && (e.type == ui::PointerType::Move || e.type == ui::PointerType::Up)) {  // T4.7: a cable drag
      if (e.type == ui::PointerType::Up) pressed_ = false;
      if (frame_.onJackDrag) frame_.onJackDrag(e);
      return true;
    }
    if (e.type == ui::PointerType::Move) {
      if (at != hover_) {
        hover_ = at;
        tip_ = at >= 0 ? frame_.jacks()[static_cast<size_t>(at)].name : std::string();
        markDirty();
      }
      return at >= 0;
    }
    if (e.type == ui::PointerType::Down && e.button == ui::PointerButton::Left && at >= 0) {
      pressed_ = true;
      if (frame_.onJackPressed) frame_.onJackPressed(frame_.jacks()[static_cast<size_t>(at)].name, e.pos);
      return true;
    }
    return false;
  }
  void onCaptureLost() override { pressed_ = false; }
  ui::Cursor cursorAt(ui::Vec2 p) const override { return pressed_ || jackAt(p) >= 0 ? ui::Cursor::Crosshair : ui::Cursor::Inherit; }
  void onHoverChanged(bool h) override {
    if (!h && hover_ >= 0) {
      hover_ = -1;
      markDirty();
    }
  }
  std::string_view tooltip() const override { return tip_; }
  ui::InfoText info() const override {
    return {tip_.empty() ? std::string_view("Output jacks") : std::string_view(tip_),
            "A signal the device exports: drag it onto any knob of any device to modulate it (on another track: hover its "
            "row in the arrangement first)."};
  }

private:
  int32_t jackAt(ui::Vec2 p) const {
    const std::span<const BlockFrame::Jack> js = frame_.jacks();
    for (size_t i = 0; i < js.size(); ++i)
      if (js[i].rect.contains(p)) return static_cast<int32_t>(i);
    return -1;
  }
  BlockFrame& frame_;
  int32_t hover_ = -1;
  bool pressed_ = false;
  std::string tip_;
};

// ---- the title bar ------------------------------------------------------------------------------------------------
class TitleBar final : public ui::Widget {
public:
  TitleBar(BlockFrame& frame, std::string title) : Widget("title"), frame_(frame), title_(std::move(title)) {
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
    paint::box(ctx.dl, a, ctx.scale.exact(1.0f), frame_.active() ? th->device.activatorOn : th->device.activatorOff, th->device.border);
    const ui::IRect fr = foldRect();
    const float cx = static_cast<float>(fr.x) + 0.5f * static_cast<float>(fr.w), cy = static_cast<float>(fr.y) + 0.5f * static_cast<float>(fr.h);
    const float s = ctx.scale.exact(3.5f);
    const ui::Rgba tc = hoverFold_ ? th->device.titleText : th->device.label;
    if (!frame_.foldable()) {
    } else if (frame_.folded()) {
      ctx.dl.addTriangleFilled({cx - 0.6f * s, cy - s}, {cx + s, cy}, {cx - 0.6f * s, cy + s}, tc);
    } else {
      ctx.dl.addTriangleFilled({cx - s, cy - 0.6f * s}, {cx + s, cy - 0.6f * s}, {cx, cy + s}, tc);
    }
    const ui::IRect p = presetRect();
    if (f) {
      const ui::IRect t{fr.right() + ctx.scale.px(4), r.y, std::max(0, (p.w > 0 ? p.x : r.right()) - fr.right() - ctx.scale.px(8)), r.h};
      if (rename_ == nullptr)
        paint::text(ctx, f->bodyStrong, t, title_, frame_.active() ? th->device.titleText : th->device.titleTextOff, ui::TextAlign::Left);
      if (p.w > 0) {  // T5.11: the preset name; a click hot-swaps (the host's onPresetClick)
        const bool live = static_cast<bool>(frame_.onPresetClick);
        paint::box(ctx.dl, p, 0.0f, hoverPreset_ && live ? th->surface.raised : th->field.fill, th->device.border);
        const std::string& name = frame_.presetName();
        paint::text(ctx, f->small, p, name.empty() ? std::string("Default") : name, live ? th->device.titleText : th->text.disabled,
                    ui::TextAlign::Left, ctx.scale.px(4), ctx.scale.px(4));
      }
    }
  }
  bool onPointer(const ui::PointerEvent& e) override {
    if (e.type == ui::PointerType::Move) {
      const bool h = foldRect().contains(e.pos), hp = presetRect().contains(e.pos);
      if (h != hoverFold_ || hp != hoverPreset_) {
        hoverFold_ = h;
        hoverPreset_ = hp;
        markDirty();
      }
      return false;
    }
    if (e.type != ui::PointerType::Down || e.button != ui::PointerButton::Left) return false;
    if (frame_.onPresetClick && presetRect().contains(e.pos)) {
      frame_.onPresetClick();
      return true;
    }
    if (activatorRect().contains(e.pos)) {
      const bool on = !frame_.active();
      frame_.setActive(on);
      if (frame_.onActivate) frame_.onActivate(on);
      return true;
    }
    if (frame_.foldable() && (foldRect().contains(e.pos) || e.clickCount == 2)) {  // a double-click folds, as the triangle
      frame_.setFolded(!frame_.folded());
      return true;
    }
    return false;
  }
  void onHoverChanged(bool h) override {
    if (!h && (hoverFold_ || hoverPreset_)) {
      hoverFold_ = hoverPreset_ = false;
      markDirty();
    }
  }
  // T4.16c: the activator, the fold triangle and a live preset slot are buttons; the rest of the bar is the host's (a
  // drag there moves the device).
  ui::Cursor cursorAt(ui::Vec2 p) const override {
    return activatorRect().contains(p) || (frame_.foldable() && foldRect().contains(p)) || (frame_.onPresetClick && presetRect().contains(p))
               ? ui::Cursor::Pointer
               : ui::Cursor::Inherit;
  }
  ui::IRect presetSlot() const { return presetRect(); }
  ui::InfoText info() const override {
    return {title_, "Device title bar: the square switches the device on or off; the triangle (or a double-click) folds it."};
  }

private:
  float us() const { return tree() ? tree()->scale() : 1.0f; }
  ui::IRect activatorRect() const {
    const ui::IRect r = rect();
    const auto sz = static_cast<int32_t>(std::lround(10.0f * us()));
    return {r.x + static_cast<int32_t>(std::lround(5.0f * us())), r.y + (r.h - sz) / 2, sz, sz};
  }
  ui::IRect foldRect() const {
    const ui::IRect a = activatorRect();
    const auto sz = static_cast<int32_t>(std::lround(12.0f * us()));
    return {a.right() + static_cast<int32_t>(std::lround(4.0f * us())), rect().y + (rect().h - sz) / 2, sz, sz};
  }
  ui::IRect presetRect() const {
    const ui::IRect r = rect();
    const auto w = static_cast<int32_t>(std::lround(64.0f * us()));
    if (frame_.folded() || r.w < w * 3) return {};
    const auto h = static_cast<int32_t>(std::lround(14.0f * us()));
    return {r.right() - w - static_cast<int32_t>(std::lround(5.0f * us())), r.y + (r.h - h) / 2, w, h};
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

  BlockFrame& frame_;
  std::string title_;
  bool hoverFold_ = false, hoverPreset_ = false;
  ui::TextEdit* rename_ = nullptr;
  bool renameDone_ = false;
  std::function<void(const std::string&)> renameCommit_;
};

// M4 review L12: a generated label in title case ("attack" -> "Attack", "lfo_rate" -> "Lfo Rate").
std::string titleCase(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  bool start = true;
  for (const char ch : s) {
    const char c = ch == '_' ? ' ' : ch;
    out += start && c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
    start = c == ' ';
  }
  return out;
}

}  // namespace

// ---- BlockFrame -----------------------------------------------------------------------------------------------------

BlockFrame::BlockFrame(std::string name, std::unique_ptr<GroupPanelFace> face, Options o) : Widget(std::move(name)), opt_(std::move(o)) {
  setLayout(ui::LayoutKind::Custom);
  setClipChildren(true);
  for (const std::string& e : opt_.exports) jacks_.push_back({e, {}});
  if (!jacks_.empty()) strip_ = &add<JackStrip>(*this);
  title_ = &add<TitleBar>(*this, opt_.title);
  host_ = &add<PanelHost>("panel", std::move(face));
}

BlockFrame::~BlockFrame() = default;

GroupPanelFace& BlockFrame::face() const noexcept { return *host_->panelFace(); }

void BlockFrame::setActive(bool on) {
  if (active_ == on) return;
  active_ = on;
  markDirty();
}

void BlockFrame::setFolded(bool f) {
  if (folded_ == f) return;
  folded_ = f;
  host_->setVisible(!f);
  invalidateLayout();
  markDirty();
  if (onSizeChanged) onSizeChanged();
}

void BlockFrame::setFoldable(bool f) {
  if (foldable_ == f) return;
  foldable_ = f;
  if (!f) setFolded(false);
  title_->markDirty();
}

float BlockFrame::panelHeight() const noexcept {
  (void)host_->panelFace()->sync();
  return host_->face().worldSize().y;
}

float BlockFrame::panelScaleFor(float frameHeight) const noexcept {
  const float ph = panelHeight();
  if (!(ph > 0.0f)) return 1.0f;
  const float body = frameHeight - (jacks_.empty() ? 0.0f : kJackStripH) - kTitleH - 2.0f * kPad;
  return std::clamp(body / ph, opt_.minScale, opt_.maxScale);
}

float BlockFrame::logicalWidth() const noexcept {
  if (folded_) return kFoldedW;
  const float z = std::clamp(1.0f, opt_.minScale, opt_.maxScale);
  return std::max(opt_.minWidth, std::ceil(host_->face().worldSize().x * z) + 2.0f * kPad);
}

float BlockFrame::logicalHeight() const noexcept {
  const float z = std::clamp(1.0f, opt_.minScale, opt_.maxScale);
  return (jacks_.empty() ? 0.0f : kJackStripH) + kTitleH + (folded_ ? 0.0f : panelHeight() * z + 2.0f * kPad);
}

float BlockFrame::logicalWidthFor(float frameHeight) const noexcept {
  if (folded_) return kFoldedW;
  return std::max(opt_.minWidth, std::ceil(host_->face().worldSize().x * panelScaleFor(frameHeight)) + 2.0f * kPad);
}

void BlockFrame::setPresetName(std::string name) {
  if (presetName_ == name) return;
  presetName_ = std::move(name);
  title_->markDirty();
}

ui::IRect BlockFrame::presetSlotRect() const { return static_cast<const TitleBar*>(title_)->presetSlot(); }
void BlockFrame::setTitle(std::string title) { static_cast<TitleBar*>(title_)->setTitle(std::move(title)); }
const std::string& BlockFrame::title() const noexcept { return static_cast<const TitleBar*>(title_)->title(); }
void BlockFrame::beginRename(std::function<void(const std::string&)> commit) { static_cast<TitleBar*>(title_)->beginRename(std::move(commit)); }
bool BlockFrame::renaming() const noexcept { return static_cast<const TitleBar*>(title_)->renaming(); }
ui::IRect BlockFrame::titleRect() const noexcept { return title_->rect(); }

std::span<const BlockFrame::Jack> BlockFrame::jacks() const noexcept {
  if (strip_ != nullptr) {  // along the top edge from the left, from the strip's rect of the last layout
    const ui::IRect s = strip_->rect();
    const float sc = tree() ? tree()->scale() : 1.0f;
    auto px = [sc](float v) { return static_cast<int32_t>(std::lround(v * sc)); };
    const int32_t step = px(14.0f), sz = px(9.0f);
    for (size_t i = 0; i < jacks_.size(); ++i) jacks_[i].rect = {s.x + px(8.0f) + static_cast<int32_t>(i) * step, s.y + (s.h - sz) / 2, sz, sz};
  }
  return jacks_;
}

void BlockFrame::arrange(ui::LayoutPass& pass) {
  const ui::IRect r = rect();
  int32_t y = r.y;
  if (strip_ != nullptr) {
    pass.place(*strip_, {r.x, y, r.w, pass.px(kJackStripH)});
    y += pass.px(kJackStripH);
  }
  pass.place(*title_, {r.x, y, r.w, pass.px(kTitleH)});
  y += pass.px(kTitleH);
  // The panel at its scale for the frame's height, from the body's top-left padding (PanelHost centres it across).
  const float z = panelScaleFor(static_cast<float>(r.h) / pass.scale());
  const float ph = panelHeight();
  const int32_t pad = pass.px(kPad);
  const bool show = !folded_ && ph > 0.0f;
  host_->setVisible(show);
  if (show) pass.place(*host_, {r.x + pad, y + pad, std::max(0, r.w - 2 * pad), std::max(0, std::min(pass.px(ph * z), r.bottom() - y - 2 * pad))});
}

void BlockFrame::paint(ui::PaintContext& ctx) {
  const ui::Theme* th = ctx.res.theme;
  if (!th) return;
  const int32_t top = title_->rect().bottom();
  if (!folded_ && top < rect().bottom())  // the body: the device background, down to the frame's bottom
    ctx.dl.addRectFilled(ui::IRect{rect().x, top, rect().w, rect().bottom() - top}.toRect(), th->device.background);
}

void BlockFrame::paintOver(ui::PaintContext& ctx) {
  const ui::Theme* th = ctx.res.theme;
  if (!th) return;
  // A switched-off device: its controls stay editable (as in Live) but look dimmed.
  if (!active_ && !folded_) {
    const int32_t top = title_->rect().bottom();
    ctx.dl.addRectFilled(ui::IRect{rect().x, top, rect().w, std::max(0, rect().bottom() - top)}.toRect(), ui::withAlpha(th->device.background, 120));
  }
  paint::box(ctx.dl, rect(), 0.0f, 0, th->device.border);
}

// ---- the leaf's panel -------------------------------------------------------------------------------------------------

doc::Panel leafPanel(const devices::DeviceSignature& sig, const doc::Device& d, bool* face) {
  if (face != nullptr) *face = false;
  // TMM.10 / GU.7: a native engine with a face: its module's panel (the face's sections and plain controls).
  if (doc::isMetaDevice(d))
    if (const doc::ModuleInfo* m = doc::moduleOfEngine(doc::metaDeviceType(d)))
      if (doc::Panel p = doc::enginePanel(m->kind, d.id); !p.empty()) {
        if (face != nullptr) *face = true;
        return p;
      }
  doc::Panel p;
  // Columns 64 wide; a knob takes a 56-px slot, a toggle or a number box a 28-px one, 6 px between rows; two knob rows
  // per column for up to eight controls (wider, Live-like), else three.
  size_t count = sig.consts.size();
  for (const devices::ParamInfo& prm : sig.params) count += prm.hidden ? 0u : 1u;
  constexpr float kCol = 64.0f, kKnobH = 56.0f, kSmallH = 28.0f, kGap = 6.0f, kW = 56.0f;
  const float colH = static_cast<float>(count <= 8 ? 2 : 3) * (kKnobH + kGap) - kGap;
  float x = 0.0f, y = 0.0f;
  uint32_t id = 1;
  auto place = [&](std::string kind, float h, doc::Binding b, const std::string& label) {
    if (y + h > colH + 0.5f) {
      x += kCol;
      y = 0.0f;
    }
    doc::PanelElement e;
    e.id = id++;
    e.kind = std::move(kind);
    e.x = x, e.y = y, e.w = kW, e.h = h;
    e.bindings = {std::move(b)};
    if (!label.empty()) e.setField(doc::styleText(e.kind == "toggle" ? "text" : "label", label));
    p.elements.push_back(std::move(e));
    y += h + kGap;
  };
  for (const devices::ParamInfo& prm : sig.params) {
    if (prm.hidden) continue;
    // A label the patch gave (@label) is kept as written; a bare name in title case. (A toggle names itself.)
    const std::string label = prm.label == prm.name ? titleCase(prm.label) : prm.isBool ? prm.label : std::string();
    if (prm.isBool) place("toggle", kSmallH - 6.0f, doc::controlBinding(d.id, prm.name), label);
    else if (prm.steps > 1) place("number", kSmallH, doc::controlBinding(d.id, prm.name), label);
    else place("knob", kKnobH, doc::controlBinding(d.id, prm.name), label);
  }
  for (const devices::ConstInfo& c : sig.consts) place("number", kSmallH, doc::settingBinding(d.id, c.name), titleCase(c.name));
  p.width = std::max(kW, x + kW);
  p.height = count == 0 ? 0.0f : colH;
  return p;
}

}  // namespace pq::views::panel
