// The Device view (T4.1): pq/views/device_chain/device_chain.h.
#include "pq/views/device_chain/device_chain.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "../common/drag.h"
#include "../common/paint.h"
#include "internal.h"
#include "pq/document/edit_ops.h"
#include "pq/ui/controls.h"
#include "pq/ui/theme.h"
#include "pq/views/shell/shell.h"

namespace pq::views {

using namespace device_chain;

namespace {

class DeviceFrame;

}  // namespace

struct DeviceChainView::Impl {
  DeviceChainView& view;
  ui::ScrollView* scroll = nullptr;
  ui::Widget* content = nullptr;
  std::vector<DeviceFrame*> frames;
  std::vector<doc::DevicePtr> shown;  // what each frame shows (pointer-equal while unchanged)
  doc::Snapshot shownFrom;             // keeps `shown` alive
  // Reordering by dragging a title bar.
  drag::Tracker drag;
  doc::DeviceId dragging;
  int32_t dropSlot = -1;  // insertion position among the frames (0 … n), −1: none

  explicit Impl(DeviceChainView& v) : view(v) {}
  const doc::DeviceCatalog* catalog() const noexcept {
    const doc::DeviceLibrary* lib = view.ctx_.deviceLibrary();
    return lib != nullptr ? &lib->catalog() : nullptr;
  }
  const doc::Track* track() const noexcept {
    return view.track_.valid() ? doc::findTrackPtr(*view.ctx_.doc(), view.track_) : nullptr;
  }
  // Runs an edit builder's result as one undo step; false when there was nothing to do.
  bool run(const doc::ops::EditResult& e) {
    if (!e) {
      view.ctx_.setStatus(e.error().message);
      return false;
    }
    if (e->empty()) return false;
    if (Status s = view.ctx_.store().dispatch(e->command); !s) {
      view.ctx_.setStatus(s.error().message);
      return false;
    }
    if (!e->devices.empty()) view.select(e->devices);
    return true;
  }
  int32_t frameIndex(doc::DeviceId id) const noexcept;
  void startDrag(doc::DeviceId id, const ui::PointerEvent& e) {
    drag.begin(e);
    dragging = id;
    dropSlot = -1;
  }
  void moveDrag(const ui::PointerEvent& e);
  void endDrag(bool commit);
};

namespace {

// ---- The device frame's host -----------------------------------------------------------------------------------

class FrameHost final : public DeviceHost {
public:
  FrameHost(DeviceChainView& view, ViewContext& ctx, doc::DeviceId id, std::string patch)
      : view_(view), ctx_(ctx), id_(id), patch_(std::move(patch)) {}
  ViewContext& context() noexcept override { return ctx_; }
  doc::DeviceId device() const noexcept override { return id_; }
  const doc::DeviceInfo* info() const noexcept override {
    const doc::DeviceLibrary* lib = ctx_.deviceLibrary();
    return lib != nullptr ? lib->catalog().find(patch_) : nullptr;
  }
  const doc::Device* current() const noexcept override { return doc::findDevice(*ctx_.doc(), id_).device; }
  void beginParam(std::string_view) override {
    if (!ctx_.store().inGesture()) {
      (void)ctx_.store().beginGesture("Device Parameter");
      ownGesture_ = true;
    }
  }
  void setParam(std::string_view param, std::optional<double> value) override {
    auto e = doc::ops::setDeviceParam(*ctx_.doc(), id_, std::string(param), value);
    if (e && !e->empty()) (void)ctx_.store().dispatch(e->command);
    (void)view_;
  }
  void endParam() override {
    if (ownGesture_ && ctx_.store().inGesture()) (void)ctx_.store().commitGesture();
    ownGesture_ = false;
  }
  void cancelParam() override {
    if (ownGesture_ && ctx_.store().inGesture()) (void)ctx_.store().cancelGesture();
    ownGesture_ = false;
  }
  void setSetting(std::string_view setting, std::optional<double> value) override {
    auto e = doc::ops::setDeviceSetting(*ctx_.doc(), id_, std::string(setting), value);
    if (!e) {
      ctx_.setStatus(e.error().message);
      return;
    }
    if (!e->empty()) (void)ctx_.store().dispatch(e->command);
  }

private:
  DeviceChainView& view_;
  ViewContext& ctx_;
  doc::DeviceId id_;
  std::string patch_;
  bool ownGesture_ = false;
};

// ---- A device frame: title bar (activator, name) over the device's panel ------------------------------------------

class DeviceFrame final : public ui::Widget {
public:
  DeviceFrame(DeviceChainView& view, DeviceChainView::Impl& impl, ViewContext& ctx, const doc::Device& d,
              const DevicePanelRegistry& panels)
      : Widget("device[" + std::to_string(d.id.value) + "]"), impl_(impl), id_(d.id), host_(view, ctx, d.id, d.patch) {
    setLayout(ui::LayoutKind::Custom);
    const doc::DeviceInfo* info = host_.info();
    title_ = info != nullptr ? info->name : d.patch;
    activator_ = &add<ui::Toggle>("on", "", ui::ToggleStyle::Activator, d.active);
    activator_->setInfo("Device Activator", "Turns the device on or off. Off, it is left out of the track's graph "
                                            "(no CPU); its state starts fresh when it is turned on again.");
    activator_->onToggle = [this](bool on) {
      impl_.run(doc::ops::setDevicesActive(*host_.context().doc(), std::vector<doc::DeviceId>{id_}, on));
    };
    const DevicePanelFactory* f = panels.find(d.patch);
    std::unique_ptr<DevicePanel> p = f != nullptr && *f ? (*f)(host_) : nullptr;
    if (!p) p = makeGenericPanel(host_);
    panel_ = static_cast<DevicePanel*>(&addChild(std::move(p)));
    active_ = d.active;
    info_ = "Drag the title bar to move the device in the chain. Delete removes it, 0 turns it on or off, "
            "Ctrl+C / Ctrl+V / Ctrl+D copy, paste and duplicate it. Right-click for more.";
  }

  doc::DeviceId id() const noexcept { return id_; }
  DevicePanel* panel() const noexcept { return panel_; }
  void setSelected(bool s) {
    if (selected_ == s) return;
    selected_ = s;
    markDirty();
  }
  void update(const doc::Device& d) {
    if (active_ != d.active) {
      active_ = d.active;
      activator_->setOn(d.active);
      markDirty();
    }
    panel_->refresh(d);
  }
  float preferredWidth(float heightLogical) const {
    return std::max(120.0f, panel_->preferredWidth(std::max(0.0f, heightLogical - kTitleH)));
  }

  ui::InfoText info() const override { return {title_, info_}; }

  void arrange(ui::LayoutPass& pass) override {
    const ui::IRect r = rect();
    const int32_t th = pass.px(kTitleH), a = pass.px(12.0f);
    pass.place(*activator_, {r.x + pass.px(5.0f), r.y + (th - a) / 2, a, a});
    pass.place(*panel_, {r.x, r.y + th, r.w, std::max(0, r.h - th)});
  }

  void paint(ui::PaintContext& ctx) override {
    const ui::Theme* t = ctx.res.theme;
    if (!t) return;
    const ui::IRect r = rect();
    const int32_t th = ctx.scale.px(kTitleH);
    paint::box(ctx.dl, r, 0.0f, t->surface.raised, 0);
    const ui::IRect title{r.x, r.y, r.w, th};
    paint::box(ctx.dl, title, 0.0f, selected_ ? t->selection.fill : t->control.fill, 0);
    if (ctx.res.uiFonts) {
      const int32_t left = ctx.scale.px(22.0f);
      paint::text(ctx, ctx.res.uiFonts->bodyStrong, title, title_,
                  !active_ ? t->text.disabled : selected_ ? t->selection.text : t->text.primary, ui::TextAlign::Left,
                  left, ctx.scale.px(6.0f));
    }
    paint::box(ctx.dl, r, 0.0f, 0, selected_ ? t->accent.focus : t->surface.divider);
  }

  void paintOver(ui::PaintContext& ctx) override {
    if (active_ || !ctx.res.theme) return;
    // Off: the panel is dimmed (Live greys a deactivated device).
    const ui::IRect r = rect();
    const int32_t th = ctx.scale.px(kTitleH);
    const ui::Rgba dim = ui::rgba(0, 0, 0, 96);
    ctx.dl.addRectFilled(ui::IRect{r.x + 1, r.y + th, std::max(0, r.w - 2), std::max(0, r.h - th - 1)}.toRect(), dim);
  }

  bool onPointer(const ui::PointerEvent& e) override;

private:
  bool inTitle(ui::Vec2 p) const noexcept {
    const ui::IRect r = rect();
    const int32_t th = tree() ? tree()->uiScale().px(kTitleH) : 20;
    return p.x >= static_cast<float>(r.x) && p.x < static_cast<float>(r.right()) && p.y >= static_cast<float>(r.y) &&
           p.y < static_cast<float>(r.y + th);
  }

  DeviceChainView::Impl& impl_;
  doc::DeviceId id_;
  FrameHost host_;
  std::string title_, info_;
  ui::Toggle* activator_ = nullptr;
  DevicePanel* panel_ = nullptr;
  bool active_ = true;
  bool selected_ = false;
};

// ---- The chain: frames left → right, then the add slot ---------------------------------------------------------

class AddSlot final : public ui::Control {
public:
  explicit AddSlot(DeviceChainView& view) : Control("add"), view_(view) {
    setInfo("Add Device", "Opens the device menu: instruments and audio effects (until the browser arrives). A new "
                          "device goes after the selected one, else at the end of the chain.");
  }
  void setHint(std::string h) {
    if (h == hint_) return;
    hint_ = std::move(h);
    markDirty();
  }
  void paint(ui::PaintContext& ctx) override {
    const ui::Theme* t = ctx.res.theme;
    if (!t) return;
    const ui::IRect r = rect();
    const ui::VisualState vs = visualState();
    paint::box(ctx.dl, r, ctx.scale.exact(3.0f), vs.hover ? t->surface.raised : t->surface.sunken, t->surface.divider);
    if (!ctx.res.uiFonts) return;
    const int32_t plus = ctx.scale.px(24.0f);
    const ui::IRect top{r.x, r.y + r.h / 2 - plus, r.w, plus};
    paint::text(ctx, ctx.res.uiFonts->title, top, "+", t->text.secondary, ui::TextAlign::Center);
    const ui::IRect below{r.x, r.y + r.h / 2, r.w, ctx.scale.px(18.0f)};
    paint::text(ctx, ctx.res.uiFonts->small, below, hint_, t->text.secondary, ui::TextAlign::Center,
                ctx.scale.px(4.0f), ctx.scale.px(4.0f));
  }
  bool onPointer(const ui::PointerEvent& e) override {
    if (e.type == ui::PointerType::Down && e.button == ui::PointerButton::Left) {
      setPressed(true);
      return true;
    }
    if (e.type == ui::PointerType::Up && e.button == ui::PointerButton::Left) {
      setPressed(false);
      if (rect().contains(e.pos)) ui::openContextMenu(*this, {static_cast<float>(rect().x), static_cast<float>(rect().bottom())},
                                                      view_.deviceMenu());
      return true;
    }
    return false;
  }

private:
  DeviceChainView& view_;
  std::string hint_ = "Add Device";
};

class ChainContent final : public ui::Widget {
public:
  explicit ChainContent(DeviceChainView::Impl& impl) : Widget("chain"), impl_(impl) { setLayout(ui::LayoutKind::Custom); }

  // Device px the chain needs at a height of `heightLogical` (frames, gaps, the add slot).
  int32_t totalWidthPx(float heightLogical, float scale) const {
    float x = kChainPad;
    for (const DeviceFrame* f : impl_.frames) x += f->preferredWidth(heightLogical - 2.0f * kChainPad) + kFrameGap;
    x += kAddW + kChainPad;
    return static_cast<int32_t>(std::lround(x * scale));
  }
  ui::ISize contentExtent(ui::ISize viewport) const override {
    const float s = tree() ? tree()->scale() : 1.0f;
    return {std::max(viewport.w, totalWidthPx(static_cast<float>(viewport.h) / s, s)), viewport.h};
  }
  void arrange(ui::LayoutPass& pass) override {
    const ui::IRect r = rect();
    const float hLogical = static_cast<float>(r.h) / pass.scale();
    const int32_t pad = pass.px(kChainPad), gap = pass.px(kFrameGap);
    int32_t x = r.x + pad;
    const int32_t h = std::max(0, r.h - 2 * pad);
    for (DeviceFrame* f : impl_.frames) {
      const int32_t w = pass.px(f->preferredWidth(hLogical - 2.0f * kChainPad));
      pass.place(*f, {x, r.y + pad, w, h});
      x += w + gap;
    }
    if (impl_.view.addSlot()) pass.place(*impl_.view.addSlot(), {x, r.y + pad, pass.px(kAddW), h});
  }
  void paintOver(ui::PaintContext& ctx) override {
    if (impl_.dropSlot < 0 || !ctx.res.theme) return;
    // The drop marker: a 2-px accent line in the gap where the dragged device lands.
    int32_t x = 0;
    const auto n = static_cast<int32_t>(impl_.frames.size());
    if (impl_.dropSlot < n) x = impl_.frames[static_cast<size_t>(impl_.dropSlot)]->rect().x - ctx.scale.px(kFrameGap) / 2;
    else if (n > 0) x = impl_.frames.back()->rect().right() + ctx.scale.px(kFrameGap) / 2;
    const ui::IRect r = rect();
    const int32_t w = std::max(2, ctx.scale.px(2.0f));
    ctx.dl.addRectFilled(ui::IRect{x - w / 2, r.y + ctx.scale.px(kChainPad), w, r.h - 2 * ctx.scale.px(kChainPad)}.toRect(),
                         ctx.res.theme->accent.primary);
  }

private:
  DeviceChainView::Impl& impl_;
};

bool DeviceFrame::onPointer(const ui::PointerEvent& e) {
  DeviceChainView& view = impl_.view;
  if (e.type == ui::PointerType::Down) {
    if (tree()) tree()->setFocus(&view);
    if (e.button == ui::PointerButton::Right) {
      if (std::find(view.selection().begin(), view.selection().end(), id_) == view.selection().end()) {
        const doc::DeviceId one[1] = {id_};
        view.select(one);
      }
      DeviceChainView* v = &view;
      const doc::Device* d = host_.current();
      const bool on = d != nullptr && d->active;
      std::vector<ui::MenuItem> items;
      items.push_back(ui::MenuItem::item(on ? "Deactivate" : "Activate", [v] { v->toggleSelectedActive(); }, "0"));
      items.push_back(ui::MenuItem::sep());
      items.push_back(ui::MenuItem::item("Cut", [v] { v->cutSelected(); }, "Ctrl+X"));
      items.push_back(ui::MenuItem::item("Copy", [v] { v->copySelected(); }, "Ctrl+C"));
      items.push_back(ui::MenuItem::item("Paste", [v] { v->paste(); }, "Ctrl+V"));
      items.push_back(ui::MenuItem::item("Duplicate", [v] { v->duplicateSelected(); }, "Ctrl+D"));
      items.push_back(ui::MenuItem::item("Delete", [v] { v->deleteSelected(); }, "Delete"));
      items.push_back(ui::MenuItem::sep());
      items.push_back(ui::MenuItem::sub("Add Device", view.deviceMenu()));
      ui::openContextMenu(*this, e.pos, std::move(items));
      return true;
    }
    if (e.button != ui::PointerButton::Left) return false;
    // A click selects (Ctrl: toggles in the selection); a press on the title bar may start a reorder drag.
    std::vector<doc::DeviceId> sel(view.selection().begin(), view.selection().end());
    const auto it = std::find(sel.begin(), sel.end(), id_);
    if ((e.mods & ui::mod::kCtrl) != 0) {
      if (it != sel.end()) sel.erase(it);
      else sel.push_back(id_);
    } else if (it == sel.end()) {
      sel.assign(1, id_);
    }
    view.select(sel);
    if (inTitle(e.pos)) impl_.startDrag(id_, e);
    return true;
  }
  if (impl_.dragging == id_) {
    if (e.type == ui::PointerType::Move && impl_.drag.active) {
      impl_.moveDrag(e);
      return true;
    }
    if (e.type == ui::PointerType::Up) {
      impl_.endDrag(true);
      return true;
    }
  }
  return e.type == ui::PointerType::Up;
}

}  // namespace

// ---- Impl: drags -----------------------------------------------------------------------------------------------

int32_t DeviceChainView::Impl::frameIndex(doc::DeviceId id) const noexcept {
  for (size_t i = 0; i < frames.size(); ++i)
    if (frames[i]->id() == id) return static_cast<int32_t>(i);
  return -1;
}

void DeviceChainView::Impl::moveDrag(const ui::PointerEvent& e) {
  const float s = view.tree() ? view.tree()->scale() : 1.0f;
  drag.move(e.pos, s);
  if (!drag.moved) return;
  // The slot whose gap is nearest: before the first frame whose centre is right of the pointer.
  auto slot = static_cast<int32_t>(frames.size());
  for (size_t i = 0; i < frames.size(); ++i) {
    const ui::IRect r = frames[i]->rect();
    if (e.pos.x < static_cast<float>(r.x) + static_cast<float>(r.w) * 0.5f) {
      slot = static_cast<int32_t>(i);
      break;
    }
  }
  // An effect never goes before the instrument; the instrument stays first.
  const doc::Track* t = track();
  const bool hasInstrument = t && !t->devices.empty() && t->devices.front()->kind == doc::DeviceKind::Instrument;
  const int32_t from = frameIndex(dragging);
  if (hasInstrument) slot = from == 0 ? 0 : std::max(slot, 1);
  if (slot != dropSlot) {
    dropSlot = slot;
    content->markDirty();
  }
}

void DeviceChainView::Impl::endDrag(bool commit) {
  const int32_t slot = dropSlot;
  const bool moved = drag.moved;
  const doc::DeviceId id = dragging;
  drag.end();
  dragging = {};
  if (dropSlot >= 0) {
    dropSlot = -1;
    content->markDirty();
  }
  if (!commit || !moved || slot < 0) return;
  const int32_t from = frameIndex(id);
  if (from < 0) return;
  // Slots count the dragged frame; MoveDevice's index is in the chain without it.
  const int32_t to = slot > from ? slot - 1 : slot;
  if (to != from) view.moveDevice(id, static_cast<uint32_t>(to));
}

// ---- DeviceChainView -------------------------------------------------------------------------------------------

DeviceChainView::DeviceChainView(ViewContext& ctx) : Widget("devices"), ctx_(ctx), impl_(std::make_unique<Impl>(*this)) {
  setLayout(ui::LayoutKind::Column);
  setFocusable(true);
  setKeyScope(ctx.scope("devices"));
  auto& scroll = add<ui::ScrollView>("scroll", true, false);
  impl_->scroll = &scroll;
  auto& content = scroll.add<ChainContent>(*impl_);
  impl_->content = &content;
  addSlot_ = &content.add<AddSlot>(*this);
  auto& bar = add<ui::Scrollbar>("hscroll", scroll, ui::Scrollbar::Axis::Horizontal);
  bar.setHeight(ui::Length::fixed(10.0f));
  sub_ = ctx.subscribe([this](const doc::StoreEvent& e) { onStore(e); });
  observer_ = ctx.observe([this](uint32_t bits) {
    if (bits & view_change::kDevices) sync(true);
    else if (bits & (view_change::kSelection | view_change::kDetailClip)) follow();
  });
  sync(true);
}

DeviceChainView::~DeviceChainView() {
  for (std::string_view id : ViewContext::kDeviceChainActions) ctx_.clearViewAction(id);
}

doc::TrackId DeviceChainView::wantedTrack() const {
  const doc::Project& p = *ctx_.doc();
  if (ctx_.selectedTrack().valid() && doc::findTrackPtr(p, ctx_.selectedTrack())) return ctx_.selectedTrack();
  if (ctx_.detailClip().track.valid() && doc::findTrackPtr(p, ctx_.detailClip().track)) return ctx_.detailClip().track;
  return {};
}

void DeviceChainView::follow() {
  if (wantedTrack() != track_) sync(false);
}

void DeviceChainView::rebuild() { sync(true); }

void DeviceChainView::sync(bool force) {
  Impl& m = *impl_;
  const doc::TrackId want = wantedTrack();
  const doc::Track* t = want.valid() ? doc::findTrackPtr(*ctx_.doc(), want) : nullptr;
  static const std::vector<doc::DevicePtr> kNone;
  const std::vector<doc::DevicePtr>& chain = t != nullptr ? t->devices : kNone;
  bool sameIds = !force && want == track_ && chain.size() == m.shown.size();
  for (size_t i = 0; sameIds && i < chain.size(); ++i) sameIds = chain[i]->id == m.shown[i]->id;
  if (sameIds) {
    for (size_t i = 0; i < chain.size(); ++i)
      if (chain[i] != m.shown[i]) m.frames[i]->update(*chain[i]);
  } else {
    if (m.drag.active) m.endDrag(false);
    for (DeviceFrame* f : m.frames) (void)m.content->removeChild(*f);
    m.frames.clear();
    for (const doc::DevicePtr& d : chain) {
      auto f = std::make_unique<DeviceFrame>(*this, m, ctx_, *d, panels_);
      DeviceFrame* raw = f.get();
      m.content->addChild(std::move(f));  // placed by ChainContent::arrange, left of the add slot
      m.frames.push_back(raw);
    }
    if (want != track_) m.scroll->scrollTo(0, 0);
    m.content->invalidateLayout();
    m.content->invalidateArrange();
    markDirty();
  }
  track_ = want;
  m.shown = chain;
  m.shownFrom = ctx_.doc();
  // The selection keeps only devices of this chain.
  std::erase_if(selection_, [&](doc::DeviceId id) { return m.frameIndex(id) < 0; });
  for (DeviceFrame* f : m.frames)
    f->setSelected(std::find(selection_.begin(), selection_.end(), f->id()) != selection_.end());
  if (auto* slot = static_cast<AddSlot*>(addSlot_)) {
    slot->setVisible(t != nullptr);
    slot->setHint(t == nullptr ? std::string() : chain.empty() ? "Add Device" : "Add");
  }
}

void DeviceChainView::onStore(const doc::StoreEvent& e) {
  Impl& m = *impl_;
  if (e.kind == doc::StoreEventKind::GestureCommit) return;  // already shown live
  const doc::ChangeSet& c = e.changes;
  const bool deviceParamsOnly =
      e.kind != doc::StoreEventKind::Reset && c.paramOnly() && !c.params.empty() &&
      std::all_of(c.params.begin(), c.params.end(), [](const doc::ParamRef& r) { return r.kind == doc::ParamKind::DeviceParam; });
  if (deviceParamsOnly && wantedTrack() == track_) {
    // Only those controls repaint (T4.6's acceptance: a param change damages only its control).
    const doc::Track* t = m.track();
    if (t == nullptr || t->devices.size() != m.shown.size()) return sync(false);
    for (size_t i = 0; i < t->devices.size(); ++i) {
      if (t->devices[i] == m.shown[i]) continue;
      m.frames[i]->panel()->paramsChanged(*t->devices[i], c.params);
      m.shown[i] = t->devices[i];
    }
    m.shownFrom = e.after;
    return;
  }
  sync(e.kind == doc::StoreEventKind::Reset);
}

void DeviceChainView::select(std::span<const doc::DeviceId> devices) {
  selection_.assign(devices.begin(), devices.end());
  for (DeviceFrame* f : impl_->frames)
    f->setSelected(std::find(selection_.begin(), selection_.end(), f->id()) != selection_.end());
}

std::vector<ui::MenuItem> DeviceChainView::deviceMenu() {
  std::vector<ui::MenuItem> items;
  const doc::DeviceCatalog* cat = impl_->catalog();
  const doc::Track* t = impl_->track();
  if (cat == nullptr || t == nullptr) {
    ui::MenuItem none = ui::MenuItem::item(cat == nullptr ? "No device library" : "No track selected");
    none.enabled = false;
    items.push_back(std::move(none));
    return items;
  }
  for (const doc::DeviceKind kind : {doc::DeviceKind::Instrument, doc::DeviceKind::AudioEffect}) {
    ui::MenuItem head = ui::MenuItem::item(kind == doc::DeviceKind::Instrument ? "Instruments" : "Audio Effects");
    head.enabled = false;
    if (!items.empty()) items.push_back(ui::MenuItem::sep());
    items.push_back(std::move(head));
    for (const doc::DeviceInfo& d : cat->devices()) {
      if (d.kind != kind) continue;
      const doc::DeviceInfo* info = &d;
      ui::MenuItem it = ui::MenuItem::item("   " + d.name, [this, info] { addDevice(*info); });
      it.enabled = kind == doc::DeviceKind::AudioEffect || t->kind == doc::TrackKind::Midi;
      items.push_back(std::move(it));
    }
  }
  return items;
}

bool DeviceChainView::addDevice(const doc::DeviceInfo& info) {
  const doc::Track* t = impl_->track();
  if (t == nullptr) return false;
  uint32_t index = doc::kAppend;
  for (size_t i = 0; i < t->devices.size(); ++i)
    if (!selection_.empty() && t->devices[i]->id == selection_.back()) index = static_cast<uint32_t>(i + 1);
  return impl_->run(doc::ops::addDevice(*ctx_.doc(), ctx_.store().ids(), track_, info.patch, info.kind, index));
}

bool DeviceChainView::deleteSelected() {
  if (selection_.empty()) return false;
  const std::vector<doc::DeviceId> sel = selection_;
  if (!impl_->run(doc::ops::deleteDevices(*ctx_.doc(), sel))) return false;
  selection_.clear();
  return true;
}

bool DeviceChainView::copySelected() {
  const doc::Track* t = impl_->track();
  if (t == nullptr || selection_.empty()) return false;
  clipboard_.clear();
  for (const doc::DevicePtr& d : t->devices)  // chain order, whatever order they were selected in
    if (std::find(selection_.begin(), selection_.end(), d->id) != selection_.end()) clipboard_.push_back(d);
  return !clipboard_.empty();
}

bool DeviceChainView::cutSelected() { return copySelected() && deleteSelected(); }

bool DeviceChainView::paste() {
  const doc::Track* t = impl_->track();
  if (t == nullptr || clipboard_.empty()) return false;
  uint32_t index = doc::kAppend;
  for (size_t i = 0; i < t->devices.size(); ++i)
    if (!selection_.empty() && t->devices[i]->id == selection_.back()) index = static_cast<uint32_t>(i + 1);
  return impl_->run(doc::ops::pasteDevices(*ctx_.doc(), ctx_.store().ids(), track_, clipboard_, index));
}

bool DeviceChainView::duplicateSelected() {
  if (selection_.empty()) return false;
  return impl_->run(doc::ops::duplicateDevices(*ctx_.doc(), ctx_.store().ids(), selection_));
}

bool DeviceChainView::toggleSelectedActive() {
  if (selection_.empty()) return false;
  const std::vector<doc::DeviceId> keep = selection_;
  const bool r = impl_->run(doc::ops::toggleDevicesActive(*ctx_.doc(), keep));
  select(keep);
  return r;
}

bool DeviceChainView::moveDevice(doc::DeviceId device, uint32_t index) {
  return impl_->run(doc::ops::moveDevice(*ctx_.doc(), device, {}, index));
}

ui::Widget* DeviceChainView::frameOf(doc::DeviceId device) const noexcept {
  const int32_t i = impl_->frameIndex(device);
  return i < 0 ? nullptr : impl_->frames[static_cast<size_t>(i)];
}

DevicePanel* DeviceChainView::panelOf(doc::DeviceId device) const noexcept {
  const int32_t i = impl_->frameIndex(device);
  return i < 0 ? nullptr : impl_->frames[static_cast<size_t>(i)]->panel();
}

void DeviceChainView::paint(ui::PaintContext& ctx) {
  const ui::Theme* t = ctx.res.theme;
  if (!t) return;
  paint::box(ctx.dl, rect(), 0.0f, t->surface.panel, 0);
  if (!track_.valid() && ctx.res.uiFonts)
    paint::text(ctx, ctx.res.uiFonts->small, rect(), "No track selected", t->text.secondary, ui::TextAlign::Center);
}

bool DeviceChainView::onPointer(const ui::PointerEvent& e) {
  // Empty space: focus the view (its keys), clear the selection; a right-click offers the device menu and paste.
  if (e.type != ui::PointerType::Down) return false;
  if (tree()) tree()->setFocus(this);
  select({});
  if (e.button == ui::PointerButton::Right && track_.valid()) {
    std::vector<ui::MenuItem> items = deviceMenu();
    items.push_back(ui::MenuItem::sep());
    ui::MenuItem p = ui::MenuItem::item("Paste", [this] { paste(); }, "Ctrl+V");
    p.enabled = !clipboard_.empty();
    items.push_back(std::move(p));
    ui::openContextMenu(*this, e.pos, std::move(items));
  }
  return true;
}

DeviceChainView& installDeviceChain(Shell& shell) {
  ViewContext& ctx = shell.context();
  auto& v = shell.slot(ShellSlot::DeviceDetail).add<DeviceChainView>(ctx);
  DeviceChainView* p = &v;
  auto has = [p] { return !p->selection().empty(); };
  ctx.setViewAction("device.delete", [p] { p->deleteSelected(); }, has);
  ctx.setViewAction("device.copy", [p] { p->copySelected(); }, has);
  ctx.setViewAction("device.cut", [p] { p->cutSelected(); }, has);
  ctx.setViewAction("device.paste", [p] { p->paste(); }, [p] { return p->track().valid(); });
  ctx.setViewAction("device.duplicate", [p] { p->duplicateSelected(); }, has);
  ctx.setViewAction("device.toggleActive", [p] { p->toggleSelectedActive(); }, has);
  return v;
}

DeviceChainView* findDeviceChain(Shell& shell) noexcept {
  return dynamic_cast<DeviceChainView*>(shell.slot(ShellSlot::DeviceDetail).find("devices"));
}

}  // namespace pq::views
