// The Device view (T4.1; racks T4.8): pq/views/device_chain/device_chain.h.
#include "pq/views/device_chain/device_chain.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>

#include "../common/paint.h"
#include "pq/core/automation_curve.h"
#include "internal.h"
#include "pq/views/browser/library.h"
#include "pq/views/browser/library_load.h"
#include "pq/document/edit_ops.h"
#include "pq/ui/controls.h"
#include "pq/ui/theme.h"
#include "pq/views/device_chain/wiring.h"
#include "pq/views/circuit/device_circuit.h"
#include "pq/views/devices/panel_desc.h"
#include "pq/views/shell/shell.h"

namespace pq::views {

using namespace device_chain;

// ---- Impl ------------------------------------------------------------------------------------------------------

bool DeviceChainView::Impl::run(const doc::ops::EditResult& e, bool selectResult) {
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

void DeviceChainView::Impl::live(const doc::ops::EditResult& e) {
  if (e && !e->empty()) (void)view.ctx_.store().dispatch(e->command);
}

int32_t DeviceChainView::Impl::frameIndex(doc::DeviceId id) const noexcept {
  for (size_t i = 0; i < frames.size(); ++i)
    if (frames[i]->id() == id) return static_cast<int32_t>(i);
  return -1;
}

FrameBase* DeviceChainView::Impl::frameOf(doc::DeviceId id) const noexcept {
  const auto it = all.find(id.value);
  return it == all.end() ? nullptr : it->second;
}

void DeviceChainView::Impl::reindex() {
  all.clear();
  for (FrameBase* f : frames) f->collect(all);
  for (auto& [id, f] : all)
    f->setSelected(std::find(view.selection_.begin(), view.selection_.end(), doc::DeviceId{id}) != view.selection_.end());
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

std::vector<ui::MenuItem> DeviceChainView::Impl::deviceMenu(doc::TrackId trackId, doc::ChainId chain, uint32_t index,
                                                            bool instruments) {
  std::vector<ui::MenuItem> items;
  const doc::DeviceCatalog* cat = catalog();
  if (cat == nullptr || (!trackId.valid() && !chain.valid())) {
    ui::MenuItem none = ui::MenuItem::item(cat == nullptr ? "No device library" : "No track selected");
    none.enabled = false;
    items.push_back(std::move(none));
    return items;
  }
  DeviceChainView* v = &view;
  // T5.11: a new device applies its default preset (the User Library's Defaults/<Device>.pqpreset) when there is one.
  auto add = [v, trackId, chain, index](std::string patch, doc::DeviceKind kind) {
    auto r = addCatalogDevice(v->ctx_, patch, kind, DeviceSlot{trackId, index, chain});
    if (!r) v->ctx_.setStatus("Add Device: " + r.error().message);
    else {
      const doc::DeviceId one[1] = {*r};
      v->select(one);
    }
  };
  for (const doc::DeviceKind kind : {doc::DeviceKind::Instrument, doc::DeviceKind::AudioEffect}) {
    ui::MenuItem head = ui::MenuItem::item(kind == doc::DeviceKind::Instrument ? "Instruments" : "Audio Effects");
    head.enabled = false;
    if (!items.empty()) items.push_back(ui::MenuItem::sep());
    items.push_back(std::move(head));
    for (const doc::DeviceInfo& d : cat->devices()) {
      if (d.kind != kind) continue;
      ui::MenuItem it = ui::MenuItem::item("   " + d.name, [add, patch = d.patch, kind] { add(patch, kind); });
      it.enabled = kind == doc::DeviceKind::AudioEffect || instruments;
      items.push_back(std::move(it));
    }
    // T4.8: the racks (Live lists them with the devices of their kind).
    const bool instrumentRack = kind == doc::DeviceKind::Instrument;
    ui::MenuItem rack = ui::MenuItem::item(instrumentRack ? "   Instrument Rack" : "   Audio Effect Rack",
                                           [add, instrumentRack, kind] {
                                             add(std::string(instrumentRack ? doc::kInstrumentRack : doc::kAudioEffectRack), kind);
                                           });
    rack.enabled = !instrumentRack || instruments;
    items.push_back(std::move(rack));
  }
  // T5.11: the user devices of the library ("Save as New Device"), for every project.
  const auto& users = view.ctx_.library().userDevices();
  if (!users.empty()) {
    items.push_back(ui::MenuItem::sep());
    ui::MenuItem head = ui::MenuItem::item("User Devices");
    head.enabled = false;
    items.push_back(std::move(head));
    for (const Library::UserDeviceEntry& u : users) {
      const std::string path = u.path;
      ui::MenuItem it = ui::MenuItem::item("   " + u.file.name, [v, path, trackId, chain, index] {
        auto r = loadDeviceItem(v->ctx_, path, DeviceSlot{trackId, index, chain});
        if (!r) v->ctx_.setStatus("Add Device: " + r.error().message);
      });
      it.enabled = u.file.kind == doc::DeviceKind::AudioEffect || instruments;
      items.push_back(std::move(it));
    }
  }
  return items;
}

void DeviceChainView::Impl::startDrag(doc::DeviceId id, const ui::PointerEvent& e) {
  drag.begin(e);
  dragging = id;
  drop = DropTarget{};
}

void DeviceChainView::Impl::moveDrag(const ui::PointerEvent& e) {
  const float s = view.tree() ? view.tree()->scale() : 1.0f;
  drag.move(e.pos, s);
  if (!drag.moved) return;
  DropTarget d;
  // T4.8: a rack's chain list or the chain it shows, deepest first (a nested rack's frame lies inside its parent's).
  FrameBase* best = nullptr;
  for (auto& [id, f] : all) {
    if (!f->isRack() || !f->rect().contains(e.pos)) continue;
    if (best == nullptr || (f->rect().w <= best->rect().w && f->rect().h <= best->rect().h)) best = f;
  }
  bool inRack = false;
  if (best != nullptr && best->id() != dragging) inRack = best->dropAt(e.pos, d);
  if (!inRack) {
    // The track's chain: the gap nearest the pointer (before the first frame whose centre is right of it).
    d.kind = DropTarget::Kind::Track;
    auto slot = static_cast<uint32_t>(frames.size());
    for (size_t i = 0; i < frames.size(); ++i) {
      const ui::IRect r = frames[i]->rect();
      if (e.pos.x < static_cast<float>(r.x) + static_cast<float>(r.w) * 0.5f) {
        slot = static_cast<uint32_t>(i);
        break;
      }
    }
    // An effect never goes before the instrument; the instrument stays first.
    const doc::Track* t = track();
    const bool hasInstrument = t && !t->devices.empty() && t->devices.front()->kind == doc::DeviceKind::Instrument;
    const int32_t from = frameIndex(dragging);
    if (hasInstrument) slot = from == 0 ? 0u : std::max(slot, 1u);
    d.index = slot;
    int32_t x = 0;
    const auto n = static_cast<uint32_t>(frames.size());
    if (slot < n) x = frames[slot]->rect().x - static_cast<int32_t>(std::lround(kFrameGap * s)) / 2;
    else if (n > 0) x = frames.back()->rect().right() + static_cast<int32_t>(std::lround(kFrameGap * s)) / 2;
    const ui::IRect r = content->rect();
    const int32_t w = std::max(2, static_cast<int32_t>(std::lround(2.0f * s)));
    const int32_t pad = static_cast<int32_t>(std::lround(kChainPad * s));
    d.marker = {x - w / 2, r.y + pad, w, r.h - 2 * pad};
  }
  // T4.16c: a drop the document would refuse shows NotAllowed instead of a marker.
  if (d.kind == DropTarget::Kind::Chain || d.kind == DropTarget::Kind::NewChain) {
    const doc::Project& p = doc();
    const doc::DeviceLoc moving = doc::findDevice(p, dragging);
    const doc::Device* into = d.kind == DropTarget::Kind::NewChain ? doc::findDevice(p, d.rack).device : doc::findChain(p, d.chain).rack;
    if (moving.device != nullptr && into != nullptr) {
      if (into == moving.device || doc::deviceContains(*moving.device, into->id)) d.refused = "A rack cannot go inside itself";
      else if (moving.device->kind == doc::DeviceKind::Instrument && into->kind != doc::DeviceKind::Instrument)
        d.refused = "An instrument goes into an Instrument Rack, not an Audio Effect Rack";
    }
  }
  if (d.kind != drop.kind || d.index != drop.index || d.chain != drop.chain || d.rack != drop.rack || d.refused != drop.refused) {
    drop = d;
    content->markDirty();
  }
}

void DeviceChainView::Impl::endDrag(bool commit) {
  const DropTarget d = drop;
  const bool moved = drag.moved;
  const doc::DeviceId id = dragging;
  drag.end();
  dragging = {};
  if (drop.kind != DropTarget::Kind::None) {
    drop = DropTarget{};
    content->markDirty();
  }
  if (!commit || !moved || d.kind == DropTarget::Kind::None) return;
  if (d.refused != nullptr) {
    view.ctx_.setStatus(d.refused);
    return;
  }
  const doc::DeviceLoc loc = doc::findDevice(doc(), id);
  if (loc.device == nullptr) return;
  switch (d.kind) {
    case DropTarget::Kind::Track: {
      // Slots count the dragged frame when it is in the track's chain; MoveDevice's index is in the chain without it.
      const int32_t from = frameIndex(id);
      const auto to = static_cast<uint32_t>(from >= 0 && d.index > static_cast<uint32_t>(from) ? d.index - 1 : d.index);
      if (from >= 0 && to == static_cast<uint32_t>(from)) return;
      run(doc::ops::moveDevice(doc(), id, view.track_, to));
      break;
    }
    case DropTarget::Kind::Chain: {
      uint32_t to = d.index;
      if (loc.chain != nullptr && loc.chain->id == d.chain && d.index > loc.index) --to;
      run(doc::ops::moveDevice(doc(), id, {}, to, d.chain));
      shownChain[doc::findChain(doc(), d.chain).rack != nullptr ? doc::findChain(doc(), d.chain).rack->id.value : 0] = d.chain;
      break;
    }
    case DropTarget::Kind::NewChain:
      run(doc::ops::moveDeviceToNewChain(doc(), view.ctx_.store().ids(), id, d.rack));
      break;
    case DropTarget::Kind::None: break;
  }
}

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
    // T4.8: a knob a macro drives turns its macro (the inverse of the mapping), as Live's mapped controls follow theirs.
    if (const doc::MappingLoc m = doc::mappingOf(*ctx_.doc(), id_, param); m.mapping != nullptr) {
      FrameBase* f = impl_.frameOf(id_);
      const devices::ParamInfo* info = f != nullptr && f->panel() != nullptr ? f->panel()->signature().findParam(param) : nullptr;
      if (info == nullptr) return;
      const doc::MacroMapping& mm = *m.mapping;
      const double span = mm.max - mm.min;
      const double s = std::abs(span) < 1e-9 ? 0.0 : std::clamp((devices::toPosition(*info, value) - mm.min) / span, 0.0, 1.0);
      const double k = kAutomationCurveK * mm.curve;
      const double x = std::abs(k) < kAutomationLinearK ? s : std::log1p(s * std::expm1(k)) / k;  // shape⁻¹
      impl_.live(doc::ops::setMacroValue(*ctx_.doc(), m.rack->id, mm.macro, std::clamp(x, 0.0, 1.0) * doc::kMacroMax));
      return;
    }
    // Inside the gesture each change is applied at once (live param writes); outside it is one undo step of its own.
    impl_.live(doc::ops::setDeviceParam(*ctx_.doc(), id_, std::string(param), value));
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

class DeviceFrame final : public FrameBase {
public:
  DeviceFrame(DeviceChainView::Impl& impl, ViewContext& ctx, const doc::Device& d, const PanelTemplate* tpl, std::string key)
      : FrameBase("device[" + std::to_string(d.id.value) + "]", impl, ctx, d.id), target_(impl, ctx, d.id),
        key_(std::move(key)), source_(d.source) {
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
      // T5.11: the preset slot names the last preset loaded here; a click hot-swaps the device's presets in the browser.
      panel_->setPresetName(ctx.library().presetName(d.id));
      panel_->onPresetClick = [this] { impl_.view.hotSwap(id_); };
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
  }

  bool keeps(const doc::Device& d) const override {
    return d.source == source_ || DeviceChainView::Impl::templateKey(ctx_.deviceLibrary(), d) == key_;
  }

  devices::DevicePanel* panel() const noexcept override { return panel_; }
  devices::DeviceTarget* target() noexcept override { return &target_; }
  // The device changed in the document (values, settings, activation): the panel repaints what moved.
  void update(const doc::Device& d) override {
    source_ = d.source;
    if (!panel_) return;
    panel_->setActive(d.active);
    panel_->setEdited(d.edited());
    panel_->setPresetName(ctx_.library().presetName(d.id));
    for (const devices::ParamInfo& p : panel_->signature().params) {
      const doc::DeviceValue* v = d.param(p.name);
      panel_->setParamValue(p.name, v != nullptr ? v->value : p.def);
    }
    for (const devices::ConstInfo& c : panel_->signature().consts) {
      const doc::DeviceValue* v = d.setting(c.name);
      panel_->setConstValue(c.name, v != nullptr ? v->value : c.def);
    }
  }
  void paramsChanged(const doc::Device& d, std::span<const doc::ParamRef> params) override {
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
  float logicalWidth() const override { return panel_ ? panel_->logicalWidth() : 160.0f; }
  float logicalHeight(float available) const override {
    return panel_ ? std::min(available, panel_->logicalHeight()) : available;
  }

  ui::InfoText info() const override {
    return {"Device", "Drag the title bar to move the device in the chain or into a rack's chain. Left / Right select the "
                      "neighbouring device (Shift: add it to the selection), Ctrl+Left / Right move it. Delete removes "
                      "it, 0 turns it on or off, Ctrl+C / Ctrl+V / Ctrl+D copy, paste and duplicate it. Ctrl+click or "
                      "Shift+Right selects several: Ctrl+G groups them into a rack. Right-click for more."};
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

  bool inTitle(ui::Vec2 p) const override {
    const ui::IRect r = rect();
    // The title bar is the panel's top kTitleH (below the jack strip of a device with exports).
    const float strip = panel_ && !panel_->signature().exports.empty() ? 10.0f : 0.0f;  // T4.6 kJackStripH
    const int32_t top = tree() ? tree()->uiScale().px(strip) : 0;
    const int32_t th = tree() ? tree()->uiScale().px(kTitleH) : 20;
    return p.x >= static_cast<float>(r.x) && p.x < static_cast<float>(r.right()) &&
           p.y >= static_cast<float>(r.y + top) && p.y < static_cast<float>(r.y + top + th);
  }

private:
  DocTarget target_;
  std::string key_;           // the template it was built from (Impl::templateKey)
  doc::UserPatchPtr source_;  // the patch text it shows (T4.4-fix1)
  devices::DevicePanel* panel_ = nullptr;
  std::string missing_;
};

// ---- The chain: frames left → right, then the add slot ---------------------------------------------------------

class AddSlot final : public ui::Control {
public:
  explicit AddSlot(DeviceChainView& view) : Control("add"), view_(view) {
    setInfo("Add Device", "Opens the device menu: instruments, audio effects and racks (until the browser arrives). A "
                          "new device goes after the selected one, else at the end of the chain.");
    setCursor(ui::Cursor::Pointer);  // T4.16c: opens a menu
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

  // Device px the chain needs (frames, gaps, the add slot).
  int32_t totalWidthPx(float scale) const {
    float x = kChainPad;
    for (const FrameBase* f : impl_.frames) x += f->logicalWidth() + kFrameGap;
    x += kAddW + kChainPad;
    return static_cast<int32_t>(std::lround(x * scale));
  }
  ui::ISize contentExtent(ui::ISize viewport) const override {
    const float s = tree() ? tree()->scale() : 1.0f;
    return {std::max(viewport.w, totalWidthPx(s)), viewport.h};
  }
  void arrange(ui::LayoutPass& pass) override {
    const ui::IRect r = rect();
    const float hLogical = static_cast<float>(r.h) / pass.scale();
    const int32_t pad = pass.px(kChainPad), gap = pass.px(kFrameGap);
    int32_t x = r.x + pad;
    const int32_t h = std::max(0, r.h - 2 * pad);
    for (FrameBase* f : impl_.frames) {
      const int32_t w = pass.px(f->logicalWidth());
      pass.place(*f, {x, r.y + pad, w, std::min(h, pass.px(f->logicalHeight(hLogical - 2.0f * kChainPad)))});
      x += w + gap;
    }
    if (impl_.view.addSlot()) pass.place(*impl_.view.addSlot(), {x, r.y + pad, pass.px(kAddW), h});
    if (impl_.mapOverlay != nullptr) pass.place(*impl_.mapOverlay, r);  // T4.8: over everything while it is shown
  }
  void paintOver(ui::PaintContext& ctx) override {
    const DropTarget& d = impl_.drop;
    if (d.kind == DropTarget::Kind::None || d.refused != nullptr || !ctx.res.theme) return;
    // The drop marker: a 2-px accent line in the gap where the dragged device lands, or an outlined chain row.
    if (d.row) paint::box(ctx.dl, d.marker, 0.0f, 0, ctx.res.theme->accent.primary);
    else ctx.dl.addRectFilled(d.marker.toRect(), ctx.res.theme->accent.primary);
  }

private:
  DeviceChainView::Impl& impl_;
};

}  // namespace

// ---- FrameBase ---------------------------------------------------------------------------------------------------

bool FrameBase::inTitle(ui::Vec2 p) const {
  const ui::IRect r = rect();
  const int32_t th = tree() ? tree()->uiScale().px(kTitleH) : 20;
  return rect().contains(p) && p.y < static_cast<float>(r.y + th);
}

void FrameBase::setSelected(bool s) {
  if (selected_ == s) return;
  selected_ = s;
  markDirty();
}

void FrameBase::paintOver(ui::PaintContext& ctx) {
  if (!selected_ || !ctx.res.theme) return;
  // The selected device: an outline in the focus colour (Live highlights the selected device).
  paint::box(ctx.dl, rect(), 0.0f, 0, ctx.res.theme->accent.focus);
}

ui::Cursor FrameBase::cursorAt(ui::Vec2 p) const {
  // A reorder drag in progress (this frame holds the pointer): Move, or NotAllowed where nothing would land.
  if (impl_.dragging == id_ && impl_.drag.active && impl_.drag.moved)
    return impl_.drop.kind == DropTarget::Kind::None || impl_.drop.refused != nullptr ? ui::Cursor::NotAllowed : ui::Cursor::Move;
  return inTitle(p) ? ui::Cursor::Move : ui::Cursor::Inherit;
}

bool FrameBase::onPointer(const ui::PointerEvent& e) {
  DeviceChainView& view = impl_.view;
  if (e.type == ui::PointerType::Down) {
    if (tree()) tree()->setFocus(&view);
    if (e.button == ui::PointerButton::Right) {
      if (std::find(view.selection().begin(), view.selection().end(), id_) == view.selection().end()) {
        const doc::DeviceId one[1] = {id_};
        view.select(one);
      }
      DeviceChainView* v = &view;
      const doc::DeviceLoc loc = doc::findDevice(*ctx_.doc(), id_);
      const bool on = loc.device != nullptr && loc.device->active;
      std::vector<ui::MenuItem> items;
      // T4.4-fix1 (SPEC §15 2026-10-26): the circuit editor on this device; a user device back to the stock patch.
      // T4.8: a rack opens its lowered subcircuit (read-only) instead.
      const doc::DeviceId id = id_;
      contextItems(e.pos, items);  // T4.16c
      if (loc.device != nullptr && loc.device->rack) {
        items.push_back(ui::MenuItem::item("Open in Circuit View", [v, id] { v->openRackCircuit(id); }));
      } else {
        items.push_back(ui::MenuItem::item("Open in Circuit View", [v, id] { v->openInCircuitView(id); }));
        ui::MenuItem revert = ui::MenuItem::item("Revert to Stock", [v] { v->revertSelectedToStock(); });
        revert.enabled = loc.device != nullptr && loc.device->edited();
        items.push_back(std::move(revert));
      }
      // T5.11: the user library.
      items.push_back(ui::MenuItem::sep());
      items.push_back(ui::MenuItem::item("Save Preset", [v, id] { v->savePreset(id, false); }));
      if (loc.device != nullptr && !loc.device->rack) {
        items.push_back(ui::MenuItem::item("Save as Default Preset", [v, id] { v->savePreset(id, true); }));
        ui::MenuItem asNew = ui::MenuItem::item("Save as New Device", [v, id] { v->saveAsNewDevice(id); });
        asNew.enabled = loc.device->edited();
        items.push_back(std::move(asNew));
      }
      items.push_back(ui::MenuItem::item("Hot-Swap Presets\xE2\x80\xA6", [v, id] { v->hotSwap(id); }));
      items.push_back(ui::MenuItem::sep());
      items.push_back(ui::MenuItem::item(on ? "Deactivate" : "Activate", [v] { v->toggleSelectedActive(); }, "0"));
      items.push_back(ui::MenuItem::sep());
      items.push_back(ui::MenuItem::item("Cut", [v] { v->cutSelected(); }, "Ctrl+X"));
      items.push_back(ui::MenuItem::item("Copy", [v] { v->copySelected(); }, "Ctrl+C"));
      items.push_back(ui::MenuItem::item("Paste", [v] { v->paste(); }, "Ctrl+V"));
      items.push_back(ui::MenuItem::item("Duplicate", [v] { v->duplicateSelected(); }, "Ctrl+D"));
      items.push_back(ui::MenuItem::item("Delete", [v] { v->deleteSelected(); }, "Delete"));
      items.push_back(ui::MenuItem::sep());
      items.push_back(ui::MenuItem::item("Group", [v] { v->groupSelected(); }, "Ctrl+G"));  // T4.8
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

std::unique_ptr<FrameBase> device_chain::makeFrame(DeviceChainView::Impl& impl, ViewContext& ctx, const doc::Device& d) {
  if (d.rack) return makeRackFrame(impl, ctx, d);
  return std::make_unique<DeviceFrame>(impl, ctx, d, impl.templateFor(d),
                                       DeviceChainView::Impl::templateKey(ctx.deviceLibrary(), d));
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
    if (bits & view_change::kLibrary)  // T5.11: preset names in the title bars
      for (auto& [id, f] : impl_->all)
        if (devices::DevicePanel* panel = f->panel(); panel != nullptr && !f->isRack())
          panel->setPresetName(ctx_.library().presetName(doc::DeviceId{id}));
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
  for (size_t i = 0; sameIds && i < chain.size(); ++i)
    sameIds = chain[i]->id == m.shown[i]->id && chain[i]->isRack() == m.shown[i]->isRack() && m.frames[i]->keeps(*chain[i]);
  if (sameIds) {
    for (size_t i = 0; i < chain.size(); ++i)
      if (chain[i] != m.shown[i]) m.frames[i]->update(*chain[i]);
  } else {
    if (m.drag.active) m.endDrag(false);
    for (FrameBase* f : m.frames) (void)m.content->removeChild(*f);
    m.frames.clear();
    for (const doc::DevicePtr& d : chain) {
      auto f = makeFrame(m, ctx_, *d);
      FrameBase* raw = f.get();
      m.content->addChild(std::move(f));  // placed by ChainContent::arrange, left of the add slot
      m.frames.push_back(raw);
    }
    if (want != track_) m.scroll->scrollTo(0, 0);
    markDirty();
  }
  m.content->invalidateLayout();
  m.content->invalidateArrange();
  if (m.mapOverlay != nullptr) {  // keep the overlay the last child (on top of every frame)
    std::unique_ptr<ui::Widget> o = m.content->removeChild(*m.mapOverlay);
    m.mapOverlay = &m.content->addChild(std::move(o));
  }
  track_ = want;
  m.shown = chain;
  m.shownFrom = ctx_.doc();
  // The selection keeps only devices shown (any level).
  m.reindex();
  std::erase_if(selection_, [&](doc::DeviceId id) { return m.frameOf(id) == nullptr; });
  for (auto& [id, f] : m.all)
    f->setSelected(std::find(selection_.begin(), selection_.end(), doc::DeviceId{id}) != selection_.end());
  if (m.mapRack.valid() && m.frameOf(m.mapRack) == nullptr) m.setMapMode({});
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
    // A modulated knob's ring follows it; T4.8: a macro moves its mapped knobs' rings.
    if (!doc::wiresOf(*e.after).empty() || std::any_of(m.all.begin(), m.all.end(), [](const auto& f) { return f.second->isRack(); }))
      refreshRings();
    return;
  }
  sync(e.kind == doc::StoreEventKind::Reset);
}

void DeviceChainView::select(std::span<const doc::DeviceId> devices) {
  const size_t before = selection_.size();
  selection_.assign(devices.begin(), devices.end());
  for (auto& [id, f] : impl_->all)
    f->setSelected(std::find(selection_.begin(), selection_.end(), doc::DeviceId{id}) != selection_.end());
  // T4.16c (discoverability): a selection of several devices of one list names what Ctrl+G does with it.
  if (selection_.size() >= 2 && selection_.size() != before) {
    const doc::Project& p = *ctx_.doc();
    const doc::DeviceLoc first = doc::findDevice(p, selection_.front());
    bool oneList = first.device != nullptr;
    for (const doc::DeviceId id : selection_) {
      const doc::DeviceLoc l = doc::findDevice(p, id);
      oneList = oneList && l.device != nullptr && l.track == first.track && l.chain == first.chain;
    }
    if (oneList)
      ctx_.setStatus(std::to_string(selection_.size()) + " devices selected: Ctrl+G (or right-click > Group) groups them into a rack");
  }
}

// ---- T4.16c: keyboard navigation (Live's Device view) ------------------------------------------------------------------

std::vector<doc::DeviceId> DeviceChainView::shownOrder() const {
  std::vector<FrameBase*> frames;
  for (FrameBase* f : impl_->frames) f->appendShown(frames);
  std::vector<doc::DeviceId> ids;
  ids.reserve(frames.size());
  for (const FrameBase* f : frames) ids.push_back(f->id());
  return ids;
}

void DeviceChainView::revealDevice(doc::DeviceId device) {
  if (tree() != nullptr) tree()->layoutNow();  // a structural edit re-laid the chain out
  const FrameBase* f = impl_->frameOf(device);
  if (f == nullptr) return;
  const ui::IRect vp = chainViewport(), r = f->rect();
  const int32_t pad = tree() != nullptr ? tree()->uiScale().px(kChainPad) : 4;
  int32_t dx = 0;
  if (r.x < vp.x + pad) dx = r.x - vp.x - pad;
  else if (r.right() > vp.right() - pad) dx = std::min(r.right() - vp.right() + pad, r.x - vp.x - pad);  // its left edge stays
  if (dx != 0) scrollChainBy(dx);
}

bool DeviceChainView::selectNeighbour(int32_t dir, bool extend) {
  const std::vector<doc::DeviceId> order = shownOrder();
  if (order.empty()) return false;
  const doc::DeviceId lead = selection_.empty() ? doc::DeviceId{} : selection_.back();
  if (extend && lead.valid()) {
    // Shift: grow (or shrink back) the selection along the lead device's own list — the devices Ctrl+G can group.
    const doc::DeviceLoc l = doc::findDevice(*ctx_.doc(), lead);
    if (l.device == nullptr) return false;
    const auto& list = l.list();
    const int64_t at = static_cast<int64_t>(l.index) + dir;
    if (at < 0 || at >= static_cast<int64_t>(list.size())) return true;  // at the list's end: nothing to add
    const doc::DeviceId next = list[static_cast<size_t>(at)]->id;
    std::vector<doc::DeviceId> sel = selection_;
    if (std::find(sel.begin(), sel.end(), next) != sel.end()) {
      sel.pop_back();  // stepping back over the selection: the lead leaves it
      std::erase(sel, next);
      sel.push_back(next);  // … and its neighbour leads
    } else {
      sel.push_back(next);
    }
    select(sel);
    revealDevice(next);
    return true;
  }
  const auto it = lead.valid() ? std::find(order.begin(), order.end(), lead) : order.end();
  size_t to = 0;
  if (it == order.end()) to = dir > 0 ? 0 : order.size() - 1;
  else {
    const auto i = static_cast<int64_t>(it - order.begin()) + dir;
    to = static_cast<size_t>(std::clamp<int64_t>(i, 0, static_cast<int64_t>(order.size()) - 1));
  }
  const doc::DeviceId one[1] = {order[to]};
  select(one);
  revealDevice(order[to]);
  return true;
}

bool DeviceChainView::selectEnd(bool last) {
  // Within the list of the selected device (a rack chain is its own list), else the track's chain.
  const doc::Track* t = impl_->track();
  if (t == nullptr) return false;
  const std::vector<doc::DevicePtr>* list = &t->devices;
  if (!selection_.empty())
    if (const doc::DeviceLoc l = doc::findDevice(*ctx_.doc(), selection_.back()); l.device != nullptr) list = &l.list();
  if (list->empty()) return false;
  const doc::DeviceId one[1] = {last ? list->back()->id : list->front()->id};
  select(one);
  revealDevice(one[0]);
  return true;
}

bool DeviceChainView::moveSelected(int32_t dir) {
  if (selection_.size() != 1) {
    if (!selection_.empty()) ctx_.setStatus("Move: select one device (Ctrl+Left / Right move it along its chain)");
    return false;
  }
  const doc::DeviceId id = selection_.front();
  const doc::DeviceLoc l = doc::findDevice(*ctx_.doc(), id);
  if (l.device == nullptr) return false;
  const auto& list = l.list();
  const int64_t to = static_cast<int64_t>(l.index) + dir;
  if (to < 0 || to >= static_cast<int64_t>(list.size())) return false;
  // The instrument stays first (the track's chain, an Instrument Rack's chain): neither it nor an effect passes it.
  if (list.front()->kind == doc::DeviceKind::Instrument && (l.index == 0 || to == 0)) {
    ctx_.setStatus("The instrument stays first in the chain");
    return false;
  }
  const bool r = l.chain != nullptr ? impl_->run(doc::ops::moveDevice(*ctx_.doc(), id, {}, static_cast<uint32_t>(to), l.chain->id))
                                    : impl_->run(doc::ops::moveDevice(*ctx_.doc(), id, track_, static_cast<uint32_t>(to)));
  if (r) revealDevice(id);
  return r;
}

bool DeviceChainView::showNeighbourChain(int32_t dir) {
  // The selected rack, or the rack whose shown chain holds the selected device: Up / Down step through its chains (Live's
  // chain list); a device inside the old chain hands the selection to the first device of the new one.
  if (selection_.size() != 1) return false;
  const doc::Project& p = *ctx_.doc();
  const doc::DeviceLoc l = doc::findDevice(p, selection_.front());
  if (l.device == nullptr) return false;
  const doc::Device* rack = l.device->rack ? l.device : l.rack;
  if (rack == nullptr || rack->rack->chains.empty()) return false;
  const auto& chains = rack->rack->chains;
  const doc::ChainId cur = shownChain(rack->id);
  int64_t at = 0;
  for (size_t i = 0; i < chains.size(); ++i)
    if (chains[i]->id == cur) at = static_cast<int64_t>(i);
  const int64_t to = std::clamp<int64_t>(at + dir, 0, static_cast<int64_t>(chains.size()) - 1);
  if (to == at) return true;
  const doc::Chain& next = *chains[static_cast<size_t>(to)];
  showChain(rack->id, next.id);
  if (rack != l.device) {
    const doc::DeviceId one[1] = {next.devices.empty() ? rack->id : next.devices.front()->id};
    select(one);
  }
  revealDevice(selection_.front());
  return true;
}

std::vector<ui::MenuItem> DeviceChainView::deviceMenu() {
  const doc::Track* t = impl_->track();
  if (t == nullptr) return impl_->deviceMenu({}, {}, doc::kAppend, false);
  uint32_t index = doc::kAppend;
  for (size_t i = 0; i < t->devices.size(); ++i)
    if (!selection_.empty() && t->devices[i]->id == selection_.back()) index = static_cast<uint32_t>(i + 1);
  return impl_->deviceMenu(track_, {}, index, t->kind == doc::TrackKind::Midi);
}

bool DeviceChainView::addDevice(const doc::DeviceInfo& info) {
  const doc::Track* t = impl_->track();
  if (t == nullptr) return false;
  uint32_t index = doc::kAppend;
  for (size_t i = 0; i < t->devices.size(); ++i)
    if (!selection_.empty() && t->devices[i]->id == selection_.back()) index = static_cast<uint32_t>(i + 1);
  // T5.11: with its default preset when the User Library has one.
  auto r = addCatalogDevice(ctx_, info.patch, info.kind, DeviceSlot{track_, index, {}});
  if (!r) {
    ctx_.setStatus("Add Device: " + r.error().message);
    return false;
  }
  const doc::DeviceId one[1] = {*r};
  select(one);
  return true;
}

// ---- T5.11: the user library ------------------------------------------------------------------------------------------

bool DeviceChainView::savePreset(doc::DeviceId device, bool asDefault) {
  auto r = saveDevicePreset(ctx_, device, asDefault);
  if (!r) {
    ctx_.setStatus((asDefault ? "Save as Default Preset: " : "Save Preset: ") + r.error().message);
    return false;
  }
  ctx_.setStatus((asDefault ? "Default preset saved: " : "Preset saved: ") + *r);
  return true;
}

bool DeviceChainView::saveAsNewDevice(doc::DeviceId device) {
  auto r = views::saveAsNewDevice(ctx_, device);
  if (!r) {
    ctx_.setStatus("Save as New Device: " + r.error().message);
    return false;
  }
  ctx_.setStatus("Saved as a new device: " + *r);
  return true;
}

bool DeviceChainView::hotSwap(doc::DeviceId device) {
  const doc::Device* d = doc::findDevice(*ctx_.doc(), device).device;
  if (d == nullptr) return false;
  HotSwapTarget t;
  t.kind = HotSwapTarget::Kind::Device;
  t.device = device;
  t.patch = d->patch;
  t.label = deviceDisplayName(ctx_, *d);
  if (!ctx_.library().requestHotSwap(t)) {
    ctx_.setStatus("Hot-Swap needs the browser");
    return false;
  }
  return true;
}

ui::DropFeedback DeviceChainView::dragOver(const ui::DragPayload& p, ui::Vec2, ui::Mods) {
  if ((p.kind != ui::DragKind::BrowserItems && p.kind != ui::DragKind::Files) || p.paths.empty() || !track_.valid())
    return {};
  for (const std::string& s : p.paths) {
    const LibraryItem k = libraryItemOf(s);
    if (k != LibraryItem::CatalogDevice && k != LibraryItem::Preset && k != LibraryItem::UserDevice) return {};
  }
  return {ui::DropEffect::Copy, ui::Cursor::Arrow};
}

ui::DropEffect DeviceChainView::drop(const ui::DragPayload& p, ui::Vec2, ui::Mods) {
  if (dragOver(p, {}, ui::mod::kNone).effect == ui::DropEffect::None) return ui::DropEffect::None;
  // After the selected device of the shown chain, else at the end (one undo step per item).
  uint32_t index = doc::kAppend;
  if (const doc::Track* t = impl_->track())
    for (size_t i = 0; i < t->devices.size(); ++i)
      if (!selection_.empty() && t->devices[i]->id == selection_.back()) index = static_cast<uint32_t>(i + 1);
  std::vector<doc::DeviceId> made;
  for (const std::string& s : p.paths) {
    auto r = loadDeviceItem(ctx_, s, DeviceSlot{track_, index, {}});
    if (!r) {
      ctx_.setStatus("Could not load " + s + ": " + r.error().message);
      continue;
    }
    made.push_back(*r);
    if (index != doc::kAppend) ++index;
  }
  if (made.empty()) return ui::DropEffect::None;
  select(made);
  return ui::DropEffect::Copy;
}

bool DeviceChainView::deleteSelected() {
  if (selection_.empty()) return false;
  const std::vector<doc::DeviceId> sel = selection_;
  // T4.16c (Live): the device after the deleted ones takes the selection (else the one before), so Delete and the
  // arrow keys go on from there.
  const std::vector<doc::DeviceId> order = shownOrder();
  doc::DeviceId next, prev;
  bool seen = false;
  for (const doc::DeviceId id : order) {
    const bool gone = std::find(sel.begin(), sel.end(), id) != sel.end() ||
                      [&] {  // inside a deleted rack
                        for (const doc::DeviceId s : sel)
                          if (const doc::Device* d = doc::findDevice(*ctx_.doc(), s).device; d != nullptr && doc::deviceContains(*d, id)) return true;
                        return false;
                      }();
    if (gone) seen = true;
    else if (!seen) prev = id;
    else if (!next.valid()) next = id;
  }
  if (!impl_->run(doc::ops::deleteDevices(*ctx_.doc(), sel))) return false;
  selection_.clear();
  if (const doc::DeviceId keep = next.valid() ? next : prev; keep.valid() && impl_->frameOf(keep) != nullptr) {
    const doc::DeviceId one[1] = {keep};
    select(one);
  }
  return true;
}

bool DeviceChainView::copySelected() {
  if (selection_.empty()) return false;
  clipboard_.clear();
  // In list order (whatever order they were selected in): the track's chain, then each rack chain depth-first.
  doc::forEachDeviceIn(impl_->shown, [&](const doc::Device& d) {
    if (std::find(selection_.begin(), selection_.end(), d.id) == selection_.end()) return;
    const doc::DeviceLoc loc = doc::findDevice(*ctx_.doc(), d.id);
    if (loc.device != nullptr) clipboard_.push_back(loc.list()[loc.index]);
  });
  return !clipboard_.empty();
}

bool DeviceChainView::cutSelected() { return copySelected() && deleteSelected(); }

bool DeviceChainView::paste() {
  const doc::Track* t = impl_->track();
  if (t == nullptr || clipboard_.empty()) return false;
  // Into the list of the last selected device (a rack chain or the track's), right after it; else at the track's end.
  if (!selection_.empty()) {
    const doc::DeviceLoc loc = doc::findDevice(*ctx_.doc(), selection_.back());
    if (loc.device != nullptr && loc.chain != nullptr)
      return impl_->run(doc::ops::pasteDevices(*ctx_.doc(), ctx_.store().ids(), track_, clipboard_, loc.index + 1, loc.chain->id));
  }
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

bool DeviceChainView::groupSelected() {
  if (selection_.empty()) return false;
  const std::vector<doc::DeviceId> sel = selection_;
  return impl_->run(doc::ops::groupDevices(*ctx_.doc(), ctx_.store().ids(), sel));
}

ui::Widget* DeviceChainView::frameOf(doc::DeviceId device) const noexcept { return impl_->frameOf(device); }

devices::DeviceTarget* DeviceChainView::targetOf(doc::DeviceId device) const noexcept {
  FrameBase* f = impl_->frameOf(device);
  return f != nullptr ? f->target() : nullptr;
}

devices::DevicePanel* DeviceChainView::panelOf(doc::DeviceId device) const noexcept {
  FrameBase* f = impl_->frameOf(device);
  return f != nullptr ? f->panel() : nullptr;
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

bool DeviceChainView::onKey(const ui::KeyEvent& e) {
  // T4.8: Esc leaves macro map mode.
  if (e.down && e.key == ui::key::kEscape && impl_->mapRack.valid()) {
    impl_->setMapMode({});
    return true;
  }
  return Widget::onKey(e);
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
  ctx.setViewAction("device.group", [p] { p->groupSelected(); }, has);  // T4.8
  // T4.16c: Live's arrow keys in the Device view.
  auto chain = [p] { return p->track().valid(); };
  ctx.setViewAction("device.selectPrev", [p] { p->selectNeighbour(-1, false); }, chain);
  ctx.setViewAction("device.selectNext", [p] { p->selectNeighbour(1, false); }, chain);
  ctx.setViewAction("device.extendPrev", [p] { p->selectNeighbour(-1, true); }, chain);
  ctx.setViewAction("device.extendNext", [p] { p->selectNeighbour(1, true); }, chain);
  ctx.setViewAction("device.selectFirst", [p] { p->selectEnd(false); }, chain);
  ctx.setViewAction("device.selectLast", [p] { p->selectEnd(true); }, chain);
  ctx.setViewAction("device.moveLeft", [p] { p->moveSelected(-1); }, has);
  ctx.setViewAction("device.moveRight", [p] { p->moveSelected(1); }, has);
  ctx.setViewAction("device.chainUp", [p] { p->showNeighbourChain(-1); }, has);
  ctx.setViewAction("device.chainDown", [p] { p->showNeighbourChain(1); }, has);
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
  for (const auto& [id, f] : impl_->all) {
    const devices::DevicePanel* panel = f->panel();
    if (panel == nullptr || panel->folded() || !shownNow(*panel)) continue;
    for (const devices::ParamInfo& p : panel->signature().params) {
      if (p.isBool) continue;
      const ui::Widget* w = panel->controlFor(p.name);
      if (w == nullptr || !shownNow(*w)) continue;
      const ui::IRect r = ui::intersect(w->rect(), vp);
      if (r.empty()) continue;
      out.push_back({f->id(), p.name, r});
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
    if (m.frameOf(w.to) == nullptr) continue;
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
  for (auto& [id, f] : m.all) {
    devices::DevicePanel* panel = f->panel();
    if (panel == nullptr) continue;
    for (const devices::ParamInfo& info : panel->signature().params) {
      ui::ModulationRing ring;
      // T4.8: a macro-mapped param shows the mapping's range and where the macro puts it now (the mapping is the centre
      // the wires add to).
      float pk = static_cast<float>(devices::toPosition(info, panel->paramValue(info.name)));
      if (const doc::MappingLoc ml = doc::mappingOf(p, f->id(), info.name); ml.mapping != nullptr) {
        const doc::MacroMapping& mm = *ml.mapping;
        const double macro = doc::rackParamValue(*ml.rack, doc::macroParamName(mm.macro)) / doc::kMacroMax;
        pk = static_cast<float>(mm.min + (mm.max - mm.min) * automationShape(macro, mm.curve));
        ring.active = true;
        ring.lo = static_cast<float>(std::min(mm.min, mm.max));
        ring.hi = static_cast<float>(std::max(mm.min, mm.max));
        ring.current = pk;
        // The knob shows what the macro sets (its own stored value is not played while mapped).
        panel->setParamValue(info.name, devices::fromPosition(info, pk));
      }
      for (size_t i = 0; i < m.ringTargets.size(); ++i) {
        if (m.ringTargets[i].device != f->id() || m.ringTargets[i].param != info.name) continue;
        ring.active = true;
        ring.lo = std::clamp(pk - m.ringSpan[i], 0.0f, 1.0f);
        ring.hi = std::clamp(pk + m.ringSpan[i], 0.0f, 1.0f);
        ring.current = -1.0f;
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
