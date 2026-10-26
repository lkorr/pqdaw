// The Device view (T4.1): pq/views/device_chain/device_chain.h.
#include "pq/views/device_chain/device_chain.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>

#include "../common/drag.h"
#include "../common/paint.h"
#include "internal.h"
#include "pq/document/edit_ops.h"
#include "pq/ui/controls.h"
#include "pq/ui/theme.h"
#include "pq/views/device_chain/wiring.h"
#include "pq/views/devices/panel_desc.h"
#include "pq/views/circuit/device_circuit.h"
#include "pq/views/shell/shell.h"

namespace pq::views {

using namespace device_chain;

namespace {

class DeviceFrame;

}  // namespace

// The panel of a device patch: its signature and its description (the .panel.json next to the device file, else the
// generated generic panel). Cached per patch by the view.
struct PanelTemplate {
  devices::DeviceSignature sig;
  devices::PanelDesc desc;
};

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
  bool run(const doc::ops::EditResult& e, bool selectResult = true) {
    if (!e) {
      view.ctx_.setStatus(e.error().message);
      return false;
    }
    if (e->empty()) return false;
    if (Status s = view.ctx_.store().dispatch(e->command); !s) {
      view.ctx_.setStatus(s.error().message);
      return false;
    }
    if (selectResult && !e->devices.empty()) view.select(e->devices);
    return true;
  }
  int32_t frameIndex(doc::DeviceId id) const noexcept;
  // One panel template per device patch (signature + description), built on first use.
  std::map<std::string, std::unique_ptr<PanelTemplate>, std::less<>> templates;
  // T4.4-fix1: keyed by what the device plays — the stock patch's name, or a user device's own root (its name carries
  // the device id and its graph's hash: a new graph, a new template); "" for a device with nothing to play.
  static std::string templateKey(const doc::DeviceLibrary* lib, const doc::Device& d);
  const PanelTemplate* templateFor(const doc::Device& d);
  void startDrag(doc::DeviceId id, const ui::PointerEvent& e) {
    drag.begin(e);
    dragging = id;
    dropSlot = -1;
  }
  void moveDrag(const ui::PointerEvent& e);
  void endDrag(bool commit);
  // T4.7: the modulated params of the shown chain, the knob travel their wires sum to, their last sweeps.
  std::vector<ModTarget> ringTargets;
  std::vector<float> ringSpan;
  std::vector<ModSweep> ringSweeps;
  uint32_t ringTimer = 0;
  bool ringsLit = false;
};

namespace {

// ---- A device panel's target: the document's device instance ---------------------------------------------------

class DocTarget final : public devices::DeviceTarget {
public:
  DocTarget(DeviceChainView::Impl& impl, ViewContext& ctx, doc::DeviceId id) : impl_(impl), ctx_(ctx), id_(id) {}
  void paramGestureBegin(std::string_view) override {
    if (!ctx_.store().inGesture()) {
      (void)ctx_.store().beginGesture("Device Parameter");
      ownGesture_ = true;
    }
  }
  void setParam(std::string_view param, double value) override {
    // Inside the gesture each change is applied at once (live param writes); outside it is one undo step of its own.
    auto e = doc::ops::setDeviceParam(*ctx_.doc(), id_, std::string(param), value);
    if (e && !e->empty()) (void)ctx_.store().dispatch(e->command);
  }
  void paramGestureEnd(std::string_view) override {
    if (ownGesture_ && ctx_.store().inGesture()) (void)ctx_.store().commitGesture();
    ownGesture_ = false;
  }
  void setConst(std::string_view name, double value) override {
    impl_.run(doc::ops::setDeviceSetting(*ctx_.doc(), id_, std::string(name), value), false);
  }
  void setActive(bool on) override {
    impl_.run(doc::ops::setDevicesActive(*ctx_.doc(), std::vector<doc::DeviceId>{id_}, on), false);
  }

private:
  DeviceChainView::Impl& impl_;
  ViewContext& ctx_;
  doc::DeviceId id_;
  bool ownGesture_ = false;
};

// ---- A device frame: the device's panel, plus selection, the reorder drag and the context menu ---------------------

class DeviceFrame final : public ui::Widget {
public:
  DeviceFrame(DeviceChainView::Impl& impl, ViewContext& ctx, const doc::Device& d, const PanelTemplate* tpl, std::string key)
      : Widget("device[" + std::to_string(d.id.value) + "]"), impl_(impl), ctx_(ctx), id_(d.id), target_(impl, ctx, d.id),
        key_(std::move(key)) {
    setLayout(ui::LayoutKind::Custom);
    if (tpl != nullptr) {
      std::map<std::string, double, std::less<>> values;
      for (const doc::DeviceValue& v : d.params) values[v.name] = v.value;
      for (const doc::DeviceValue& v : d.settings) values[v.name] = v.value;
      panel_ = &add<devices::DevicePanel>("panel", tpl->sig, tpl->desc, &target_, values);
      panel_->setActive(d.active);
      panel_->onSizeChanged = [this] { impl_.content->invalidateLayout(); };
      // T4.7: a jack press and the drag after it go to the cable layer (installWiring).
      panel_->onJackPressed = [this](std::string_view port, ui::Vec2 pos) {
        ui::PointerEvent e;
        e.type = ui::PointerType::Down;
        e.button = ui::PointerButton::Left;
        e.pos = pos;
        if (impl_.view.onJack) impl_.view.onJack(id_, port, e);
      };
      panel_->onJackDrag = [this](const ui::PointerEvent& e) {
        if (impl_.view.onJack) impl_.view.onJack(id_, {}, e);
      };
      // T4.4-fix1: a user device's "edited" marker; a double-click on the title opens the device in the circuit view.
      panel_->setEdited(d.edited());
      panel_->onTitleDoubleClick = [this] { impl_.view.openInCircuitView(id_); };
    }
    if (tpl == nullptr && d.edited()) {
      const doc::DeviceLibrary* lib = ctx.deviceLibrary();
      const auto r = lib ? lib->resolve(d) : nullptr;
      missing_ = "'" + d.patch + "' (edited) does not play: " +
                 (r ? r->error.substr(0, r->error.find('\n')) : std::string("no device library")) +
                 ". Right-click: Open in Circuit View, Revert to Stock.";
    } else if (tpl == nullptr) {
      missing_ = "'" + d.patch + "' is not available: bypassed";
    }
    active_ = d.active;
  }
  const std::string& key() const noexcept { return key_; }

  doc::DeviceId id() const noexcept { return id_; }
  devices::DevicePanel* panel() const noexcept { return panel_; }
  devices::DeviceTarget* target() noexcept { return &target_; }
  void setSelected(bool s) {
    if (selected_ == s) return;
    selected_ = s;
    markDirty();
  }
  // The device changed in the document (values, settings, activation): the panel repaints what moved.
  void update(const doc::Device& d) {
    active_ = d.active;
    if (!panel_) return;
    panel_->setActive(d.active);
    for (const devices::ParamInfo& p : panel_->signature().params) {
      const doc::DeviceValue* v = d.param(p.name);
      panel_->setParamValue(p.name, v != nullptr ? v->value : p.def);
    }
    for (const devices::ConstInfo& c : panel_->signature().consts) {
      const doc::DeviceValue* v = d.setting(c.name);
      panel_->setConstValue(c.name, v != nullptr ? v->value : c.def);
    }
  }
  // Only these params of the device changed (a Param ChangeSet).
  void paramsChanged(const doc::Device& d, std::span<const doc::ParamRef> params) {
    if (!panel_) return;
    for (const doc::ParamRef& r : params) {
      if (r.kind != doc::ParamKind::DeviceParam || r.owner != d.id.value) continue;
      for (const devices::ParamInfo& p : panel_->signature().params)
        if (doc::deviceParamHash(p.name) == r.target) {
          const doc::DeviceValue* v = d.param(p.name);
          panel_->setParamValue(p.name, v != nullptr ? v->value : p.def);
        }
    }
  }
  float logicalWidth() const { return panel_ ? panel_->logicalWidth() : 160.0f; }
  float logicalHeight(float available) const { return panel_ ? std::min(available, panel_->logicalHeight()) : available; }

  ui::InfoText info() const override {
    return {"Device", "Drag the title bar to move the device in the chain. Delete removes it, 0 turns it on or off, "
                      "Ctrl+C / Ctrl+V / Ctrl+D copy, paste and duplicate it. Right-click for more."};
  }

  void arrange(ui::LayoutPass& pass) override {
    if (panel_) pass.place(*panel_, rect());
  }

  void paint(ui::PaintContext& ctx) override {
    if (panel_ || !ctx.res.theme) return;
    const ui::Theme* t = ctx.res.theme;
    paint::box(ctx.dl, rect(), 0.0f, t->surface.raised, t->surface.divider);
    if (ctx.res.uiFonts)
      paint::text(ctx, ctx.res.uiFonts->small, paint::inset(rect(), ctx.scale.px(6.0f)), missing_, t->text.secondary,
                  ui::TextAlign::Left);
  }

  void paintOver(ui::PaintContext& ctx) override {
    if (!selected_ || !ctx.res.theme) return;
    // The selected device: an outline in the focus colour (Live highlights the selected device).
    paint::box(ctx.dl, rect(), 0.0f, 0, ctx.res.theme->accent.focus);
  }

  bool onPointer(const ui::PointerEvent& e) override;

private:
  bool inTitle(ui::Vec2 p) const noexcept {
    const ui::IRect r = rect();
    // The title bar is the panel's top kTitleH (below the jack strip of a device with exports).
    const float strip = panel_ && !panel_->signature().exports.empty() ? 10.0f : 0.0f;  // T4.6 kJackStripH
    const int32_t top = tree() ? tree()->uiScale().px(strip) : 0;
    const int32_t th = tree() ? tree()->uiScale().px(kTitleH) : 20;
    return p.x >= static_cast<float>(r.x) && p.x < static_cast<float>(r.right()) &&
           p.y >= static_cast<float>(r.y + top) && p.y < static_cast<float>(r.y + top + th);
  }

  DeviceChainView::Impl& impl_;
  ViewContext& ctx_;
  doc::DeviceId id_;
  DocTarget target_;
  std::string key_;  // the template it was built from (Impl::templateKey)
  devices::DevicePanel* panel_ = nullptr;
  std::string missing_;
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
    (void)heightLogical;
    for (const DeviceFrame* f : impl_.frames) x += f->logicalWidth() + kFrameGap;
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
      const int32_t w = pass.px(f->logicalWidth());
      pass.place(*f, {x, r.y + pad, w, std::min(h, pass.px(f->logicalHeight(hLogical - 2.0f * kChainPad)))});
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
      const doc::Device* d = doc::findDevice(*ctx_.doc(), id_).device;
      const bool on = d != nullptr && d->active;
      std::vector<ui::MenuItem> items;
      // T4.4-fix1 (SPEC §15 2026-10-26): the circuit editor on this device; a user device back to the stock patch.
      const doc::DeviceId id = id_;
      items.push_back(ui::MenuItem::item("Open in Circuit View", [v, id] { v->openInCircuitView(id); }));
      ui::MenuItem revert = ui::MenuItem::item("Revert to Stock", [v] { v->revertSelectedToStock(); });
      revert.enabled = d != nullptr && d->edited();
      items.push_back(std::move(revert));
      items.push_back(ui::MenuItem::sep());
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

std::string DeviceChainView::Impl::templateKey(const doc::DeviceLibrary* lib, const doc::Device& d) {
  if (!d.source) return d.patch;
  const auto r = lib != nullptr ? lib->resolve(d) : nullptr;
  return r && r->graph ? r->graph->root : std::string();
}

const PanelTemplate* DeviceChainView::Impl::templateFor(const doc::Device& d) {
  const doc::DeviceLibrary* lib = view.ctx_.deviceLibrary();
  const std::string key = templateKey(lib, d);
  if (key.empty()) return nullptr;
  if (const auto it = templates.find(key); it != templates.end()) return it->second.get();
  std::shared_ptr<const doc::ResolvedDevice> hold;
  const graph::Patch* callee = lib != nullptr ? lib->patchOf(d, hold) : nullptr;
  std::unique_ptr<PanelTemplate> t;
  if (callee != nullptr) {
    t = std::make_unique<PanelTemplate>();
    t->sig = devices::signatureOf(*callee);
    // The stock panel while its bindings hold for the (edited) signature, else the generic one.
    const std::string file = lib->pathOf(d.patch);
    auto desc = file.empty() ? Result<devices::PanelDesc>(Error{ErrorCode::NotFound, "", {}})
                             : devices::loadPanelDesc(devices::panelPathFor(file), t->sig);
    const doc::DeviceInfo* info = lib->catalog().find(d.patch);
    t->desc = desc ? std::move(*desc) : devices::autoPanel(t->sig, info != nullptr ? info->name : d.patch);
  }
  return (templates[key] = std::move(t)).get();
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

DeviceChainView::DeviceChainView(ViewContext& ctx) : Widget("chain-view"), ctx_(ctx), impl_(std::make_unique<Impl>(*this)) {
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
    if (bits & view_change::kDevices) {
      impl_->templates.clear();  // another library: other signatures
      sync(true);
    }
    else if (bits & (view_change::kSelection | view_change::kDetailClip)) follow();
    if (bits & view_change::kModulation) refreshRings();
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
  // T4.4-fix1: a device whose patch text changed (an edit in the circuit view, Revert to Stock) gets a new panel when
  // what it plays changed (a comment or layout edit keeps the template, and the panel).
  for (size_t i = 0; sameIds && i < chain.size(); ++i)
    if (chain[i]->source != m.shown[i]->source) {
      sameIds = Impl::templateKey(ctx_.deviceLibrary(), *chain[i]) == m.frames[i]->key();
      if (sameIds && m.frames[i]->panel()) m.frames[i]->panel()->setEdited(chain[i]->edited());
    }
  if (sameIds) {
    for (size_t i = 0; i < chain.size(); ++i)
      if (chain[i] != m.shown[i]) m.frames[i]->update(*chain[i]);
  } else {
    if (m.drag.active) m.endDrag(false);
    for (DeviceFrame* f : m.frames) (void)m.content->removeChild(*f);
    m.frames.clear();
    for (const doc::DevicePtr& d : chain) {
      auto f = std::make_unique<DeviceFrame>(m, ctx_, *d, m.templateFor(*d), Impl::templateKey(ctx_.deviceLibrary(), *d));
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
  refreshRings();  // T4.7
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
      m.frames[i]->paramsChanged(*t->devices[i], c.params);
      m.shown[i] = t->devices[i];
    }
    m.shownFrom = e.after;
    if (!doc::wiresOf(*e.after).empty()) refreshRings();  // a modulated knob's ring follows it
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

bool DeviceChainView::openInCircuitView(doc::DeviceId device) {
  return circuit::openDeviceInCircuitView(*this, ctx_, device) != nullptr;
}

bool DeviceChainView::revertSelectedToStock() {
  if (selection_.empty()) return false;
  const std::vector<doc::DeviceId> keep = selection_;
  const bool r = impl_->run(doc::ops::revertDevicesToStock(*ctx_.doc(), keep));
  select(keep);
  return r;
}

ui::Widget* DeviceChainView::frameOf(doc::DeviceId device) const noexcept {
  const int32_t i = impl_->frameIndex(device);
  return i < 0 ? nullptr : impl_->frames[static_cast<size_t>(i)];
}

devices::DeviceTarget* DeviceChainView::targetOf(doc::DeviceId device) const noexcept {
  const int32_t i = impl_->frameIndex(device);
  return i < 0 ? nullptr : impl_->frames[static_cast<size_t>(i)]->target();
}

devices::DevicePanel* DeviceChainView::panelOf(doc::DeviceId device) const noexcept {
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

// ---- T4.7: wiring hooks and the modulation rings ---------------------------------------------------------------------

ui::IRect DeviceChainView::chainViewport() const noexcept { return impl_->scroll->rect(); }

void DeviceChainView::scrollChainBy(int32_t dx) {
  impl_->scroll->scrollTo(impl_->scroll->scrollX() + dx, impl_->scroll->scrollY());
}

namespace {
bool shownNow(const ui::Widget& w) noexcept {
  for (const ui::Widget* p = &w; p != nullptr; p = p->parent())
    if (!p->visible()) return false;
  return w.tree() != nullptr;
}
ui::Vec2 clampInto(ui::Vec2 p, const ui::IRect& r, bool& inside) {
  const float x = std::clamp(p.x, static_cast<float>(r.x), static_cast<float>(r.right()));
  const float y = std::clamp(p.y, static_cast<float>(r.y), static_cast<float>(r.bottom()));
  inside = x == p.x && y == p.y;
  return {x, y};
}
}  // namespace

std::vector<DeviceChainView::ParamControl> DeviceChainView::paramControls() const {
  std::vector<ParamControl> out;
  const ui::IRect vp = chainViewport();
  for (const auto* f : impl_->frames) {
    const devices::DevicePanel* panel = static_cast<const DeviceFrame*>(f)->panel();
    if (panel == nullptr || panel->folded()) continue;
    for (const devices::ParamInfo& p : panel->signature().params) {
      if (p.isBool) continue;
      const ui::Widget* w = panel->controlFor(p.name);
      if (w == nullptr || !shownNow(*w)) continue;
      const ui::IRect r = ui::intersect(w->rect(), vp);
      if (r.empty()) continue;
      out.push_back({static_cast<const DeviceFrame*>(f)->id(), p.name, r});
    }
  }
  return out;
}


std::optional<ui::Vec2> DeviceChainView::jackPoint(doc::DeviceId device, std::string_view port, bool* docked) const {
  const devices::DevicePanel* panel = panelOf(device);
  if (panel == nullptr) return std::nullopt;
  ui::Vec2 p{static_cast<float>(panel->rect().x) + 8.0f, static_cast<float>(panel->rect().y)};
  for (const devices::DevicePanel::Jack& j : panel->jacks())
    if (j.name == port) p = {static_cast<float>(j.rect.x) + 0.5f * static_cast<float>(j.rect.w),
                             static_cast<float>(j.rect.y) + 0.5f * static_cast<float>(j.rect.h)};
  bool inside = false;
  const ui::Vec2 q = clampInto(p, chainViewport(), inside);
  if (docked != nullptr) *docked = inside;
  return q;
}

std::optional<ui::Vec2> DeviceChainView::paramPoint(doc::DeviceId device, std::string_view param, bool* docked) const {
  const devices::DevicePanel* panel = panelOf(device);
  if (panel == nullptr) return std::nullopt;
  const ui::Widget* w = panel->folded() ? nullptr : panel->controlFor(param);
  bool ok = w != nullptr && shownNow(*w);
  ui::Vec2 p;
  if (ok) {
    const ui::IRect r = w->rect();
    p = {static_cast<float>(r.x) + 0.5f * static_cast<float>(r.w), static_cast<float>(r.y) + 0.25f * static_cast<float>(r.h)};
  } else {  // the title bar's right end
    const ui::IRect r = panel->rect();
    p = {static_cast<float>(r.right()) - 6.0f, static_cast<float>(r.y) + 14.0f};
  }
  bool inside = false;
  const ui::Vec2 q = clampInto(p, chainViewport(), inside);
  if (docked != nullptr) *docked = ok && inside;
  return q;
}

bool DeviceChainView::ringsPolling() const noexcept { return impl_->ringTimer != 0; }

void DeviceChainView::refreshRings() {
  Impl& m = *impl_;
  const doc::Project& p = *ctx_.doc();
  m.ringTargets.clear();
  m.ringSpan.clear();
  for (const doc::Wire& w : doc::wiresOf(p)) {
    if (m.frameIndex(w.to) < 0) continue;
    bool found = false;
    for (size_t i = 0; i < m.ringTargets.size(); ++i)
      if (m.ringTargets[i].device == w.to && m.ringTargets[i].param == w.param) {
        m.ringSpan[i] += std::fabs(static_cast<float>(w.amount));
        found = true;
      }
    if (!found) {
      m.ringTargets.push_back({w.to, w.param});
      m.ringSpan.push_back(std::fabs(static_cast<float>(w.amount)));
    }
  }
  m.ringSweeps.resize(m.ringTargets.size());
  for (DeviceFrame* f : m.frames) {
    devices::DevicePanel* panel = f->panel();
    if (panel == nullptr) continue;
    for (const devices::ParamInfo& info : panel->signature().params) {
      ui::ModulationRing ring;
      for (size_t i = 0; i < m.ringTargets.size(); ++i) {
        if (m.ringTargets[i].device != f->id() || m.ringTargets[i].param != info.name) continue;
        const auto pk = static_cast<float>(devices::toPosition(info, panel->paramValue(info.name)));
        ring.active = true;
        ring.lo = std::clamp(pk - m.ringSpan[i], 0.0f, 1.0f);
        ring.hi = std::clamp(pk + m.ringSpan[i], 0.0f, 1.0f);
        const ModSweep& s = m.ringSweeps[i];
        if (s.known && (s.up > 0.0f || s.down > 0.0f)) {
          ring.liveLo = std::clamp(pk - s.down, 0.0f, 1.0f);
          ring.liveHi = std::clamp(pk + s.up, 0.0f, 1.0f);
          ring.current = 0.5f * (ring.liveLo + ring.liveHi);
        }
      }
      panel->setModulation(info.name, ring);
    }
  }
  // Poll while a modulated knob is shown and the source runs (and once after it stops: the rings fall still).
  ModulationSource* src = ctx_.modulationSource();
  const bool want = src != nullptr && !m.ringTargets.empty() && (src->modulationLive() || m.ringsLit);
  if (want && m.ringTimer == 0) m.ringTimer = requestTimer(1.0 / 30.0);
  if (!want && m.ringTimer != 0) {
    cancelTimer(m.ringTimer);
    m.ringTimer = 0;
  }
}

void DeviceChainView::onTimer(uint32_t id) {
  if (id != impl_->ringTimer) return;
  impl_->ringTimer = 0;
  pollRings();
}

void DeviceChainView::pollRings() {
  Impl& m = *impl_;
  ModulationSource* src = ctx_.modulationSource();
  if (src == nullptr || m.ringTargets.empty()) return;
  src->readModulation(m.ringTargets, m.ringSweeps);
  m.ringsLit = false;
  for (const ModSweep& s : m.ringSweeps) m.ringsLit = m.ringsLit || s.up > 0.0f || s.down > 0.0f;
  refreshRings();  // repaints only the knobs whose ring moved; re-arms the timer while live
}

DeviceChainView* findDeviceChain(Shell& shell) noexcept {
  return dynamic_cast<DeviceChainView*>(shell.slot(ShellSlot::DeviceDetail).find("chain-view"));
}

}  // namespace pq::views
