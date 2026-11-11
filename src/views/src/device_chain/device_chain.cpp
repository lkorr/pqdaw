// The Device view (T4.1; racks T4.8): pq/views/device_chain/device_chain.h.
#include "pq/views/device_chain/device_chain.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>

#include "../common/paint.h"
#include "internal.h"
#include "pq/views/arrangement/automation.h"
#include "pq/views/browser/library.h"
#include "pq/views/browser/library_load.h"
#include "pq/document/edit_ops.h"
#include "pq/ui/controls.h"
#include "pq/ui/theme.h"
#include "pq/views/device_chain/wiring.h"
#include "pq/views/devices/analysis.h"
#include "pq/views/devices/knob_curve_menu.h"
#include "pq/document/modules.h"
#include "pq/views/meta/face_targets.h"
#include "pq/views/shell/shell.h"
#include "pq/document/meta.h"
#include "pq/graph/patch_file.h"
#include "pq/meta/registry.h"
#include "pq/views/project_patch/project_patch_view.h"

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
  (void)lib;
  // TMM.10: a native engine's form follows its position (first: the note source reaches it).
  if (doc::isMetaDevice(d)) return d.patch + (d.kind == doc::DeviceKind::Instrument ? "#instrument" : "#effect");
  return d.patch;
}

const PanelTemplate* DeviceChainView::Impl::templateFor(const doc::Device& d) {
  const doc::DeviceLibrary* lib = view.ctx_.deviceLibrary();
  const std::string key = templateKey(lib, d);
  if (key.empty()) return nullptr;
  if (const auto it = templates.find(key); it != templates.end()) return it->second.get();
  std::unique_ptr<PanelTemplate>& t = templates[key];
  const graph::Patch* callee = lib != nullptr ? lib->patchOf(d) : nullptr;
  if (callee == nullptr) return nullptr;
  t = std::make_unique<PanelTemplate>();
  t->sig = devices::signatureOf(*callee);
  if (const meta::Type* type = meta::findType(doc::metaDeviceType(d)); type != nullptr) t->title = std::string(type->name);
  else if (const doc::DeviceInfo* info = lib->infoOf(d)) t->title = info->name;
  else t->title = d.patch;
  return t.get();
}

const devices::ParamInfo* DeviceChainView::Impl::paramInfo(doc::DeviceId device, std::string_view param) {
  const doc::DeviceLibrary* lib = view.ctx_.deviceLibrary();
  const doc::Device* d = doc::findDevice(doc(), device).device;
  const graph::Patch* patch = lib != nullptr && d != nullptr ? lib->patchOf(*d) : nullptr;
  if (patch == nullptr) return nullptr;
  std::unique_ptr<devices::DeviceSignature>& sig = sigs[patch];
  if (!sig) sig = std::make_unique<devices::DeviceSignature>(devices::signatureOf(*patch));
  const devices::ParamInfo* info = sig->findParam(param);
  if (info == nullptr) return nullptr;
  paramScratch = *info;
  paramScratch.curve = devices::storedCurve(*info, *d);  // GU.1: the block's own knob curve
  return &paramScratch;
}

FrameBase* DeviceChainView::Impl::frameFor(doc::DeviceId device) const {
  if (FrameBase* f = frameOf(device)) return f;
  // A member: the nearest group around it that has a frame of its own (not a rack: its chains' blocks have theirs).
  const doc::Project& p = doc();
  const doc::Device* d = doc::findDevice(p, device).device;
  for (uint32_t depth = 0; d != nullptr && d->group.valid() && depth < 64; ++depth) {
    if (FrameBase* f = frameOf(d->group)) return f->isRack() ? nullptr : f;
    d = doc::findDevice(p, d->group).device;
  }
  return nullptr;
}

panel::PanelHost* DeviceChainView::Impl::hostOf(doc::DeviceId device, std::string_view param, doc::Binding* binding) const {
  const FrameBase* f = frameFor(device);
  if (f == nullptr) return nullptr;
  panel::PanelHost* out = nullptr;
  forEachPanelControl(*f, [&](const PanelControl& c) {
    if (c.device != device || c.param != param) return false;
    out = c.host;
    if (binding != nullptr) *binding = *c.binding;
    return true;
  });
  return out;
}

std::vector<ui::MenuItem> DeviceChainView::Impl::deviceMenu(doc::TrackId trackId, doc::ChainId chain, uint32_t index,
                                                            bool instruments) {
  std::vector<ui::MenuItem> items;
  const doc::DeviceLibrary* lib = view.ctx_.deviceLibrary();
  if (lib == nullptr || (!trackId.valid() && !chain.valid())) {
    ui::MenuItem none = ui::MenuItem::item(lib == nullptr ? "No device library" : "No track selected");
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
    // GU.11: the built-in modules of that kind (group presets: Utility, Saturator, LFO, the instruments).
    for (const doc::ModuleInfo& m : doc::moduleInfos()) {
      const bool stockEffect = m.kind == doc::ModuleKind::Utility || m.kind == doc::ModuleKind::Saturator || m.kind == doc::ModuleKind::Lfo;
      if (kind == doc::DeviceKind::Instrument ? !m.instrument : (m.instrument || !stockEffect)) continue;
      const std::string patch = std::string(doc::kModuleItemPrefix) + std::string(m.key);
      ui::MenuItem it = ui::MenuItem::item("   " + std::string(m.name), [add, patch, kind] { add(patch, kind); });
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
  // TMM.10: the meta modules — a chain meta device (its panel is its face). TMM.10-fix1: every one fits every slot of
  // every track; what it plays follows from what its chain input receives there (the note source first on a MIDI track,
  // else the previous device's signal); it replaces nothing (ops::addMetaDevice).
  {
    items.push_back(ui::MenuItem::sep());
    ui::MenuItem head = ui::MenuItem::item("Meta Modules");
    head.enabled = false;
    items.push_back(std::move(head));
    for (const meta::Type* t : metaModuleTypes()) {
      const std::string patch = std::string(doc::kMetaDevicePrefix) + std::string(t->id);
      auto kind = doc::ops::metaDeviceKindAt(doc(), *t, trackId, index);
      ui::MenuItem it = ui::MenuItem::item("   " + std::string(t->name), [add, patch, k = kind ? *kind : doc::DeviceKind::Instrument] {
        add(patch, k);
      });
      it.enabled = kind.ok();
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
  // TX.free-rack: over the free-device rack (its tab or its canvas): out of the chain (a device of the track's own chain).
  const bool overFree = freeRack != nullptr && freeRack->visible() && freeRack->rect().contains(e.pos) && frameIndex(dragging) >= 0;
  if (overFree) {
    d.kind = DropTarget::Kind::Free;
    inRack = true;
  } else if (best != nullptr && best->id() != dragging) {
    inRack = best->dropAt(e.pos, d);
  }
  if (freeRack != nullptr) freeRack->setDropHighlight(overFree);
  if (!inRack) d = trackSlot(e.pos, dragging);
  // T4.16c: a drop the document would refuse shows NotAllowed instead of a marker.
  if (d.kind == DropTarget::Kind::Chain || d.kind == DropTarget::Kind::NewChain) {
    const doc::Project& p = doc();
    const doc::DeviceLoc moving = doc::findDevice(p, dragging);
    // GU.9: the target rack (derived: a set of parallel groups); a rack being dragged is all of its chains.
    const std::optional<doc::RackInfo> into = doc::rackOf(p, d.kind == DropTarget::Kind::NewChain ? d.rack : d.chain);
    if (moving.device != nullptr && into) {
      const std::optional<doc::RackInfo> self = moving.track != nullptr ? doc::rackOfChain(p, *moving.track, dragging) : std::nullopt;
      bool inside = false;
      if (self)
        for (const doc::RackChain& c : self->chains)
          inside = inside || into->findChain(c.id) != nullptr || into->holds(p, c.id) ||
                   (moving.track != nullptr && groupContains(moving.track->devices, c.id, into->chains.front().id));
      else
        inside = moving.track != nullptr && groupContains(moving.track->devices, dragging, into->chains.front().id);
      if (inside) d.refused = "A rack cannot go inside itself";
      else if (moving.device->kind == doc::DeviceKind::Instrument && !doc::isMetaDevice(*moving.device) && !into->instrument)
        d.refused = "An instrument goes into an Instrument Rack, not an Audio Effect Rack";
    }
  }
  if (d.kind != drop.kind || d.index != drop.index || d.chain != drop.chain || d.rack != drop.rack || d.refused != drop.refused) {
    drop = d;
    content->markDirty();
  }
}

DropTarget DeviceChainView::Impl::trackSlot(ui::Vec2 pos, doc::DeviceId dragged) const {
  DropTarget d;
  const float s = view.tree() ? view.tree()->scale() : 1.0f;
  // The track's chain: the gap nearest the pointer (before the first frame whose centre is right of it).
  d.kind = DropTarget::Kind::Track;
  auto slot = static_cast<uint32_t>(frames.size());
  for (size_t i = 0; i < frames.size(); ++i) {
    const ui::IRect r = frames[i]->rect();
    if (pos.x < static_cast<float>(r.x) + static_cast<float>(r.w) * 0.5f) {
      slot = static_cast<uint32_t>(i);
      break;
    }
  }
  // TX.all-free: the list is the main path and has no order rule (an instrument may move from the head like any block:
  // the cables decide what the notes reach). A rack chain keeps its own (rack_frame.cpp).
  (void)dragged;
  d.index = slot;
  int32_t x = 0;
  const auto n = static_cast<uint32_t>(frames.size());
  if (slot < n) x = frames[slot]->rect().x - static_cast<int32_t>(std::lround(kFrameGap * s)) / 2;
  else if (n > 0) x = frames.back()->rect().right() + static_cast<int32_t>(std::lround(kFrameGap * s)) / 2;
  else x = content->rect().x + static_cast<int32_t>(std::lround(kChainPad * s));
  const ui::IRect r = content->rect();
  const int32_t w = std::max(2, static_cast<int32_t>(std::lround(2.0f * s)));
  const int32_t pad = static_cast<int32_t>(std::lround(kChainPad * s));
  d.marker = {x - w / 2, r.y + pad, w, r.h - 2 * pad};
  return d;
}

void DeviceChainView::Impl::endDrag(bool commit) {
  const DropTarget d = drop;
  const bool moved = drag.moved;
  const doc::DeviceId id = dragging;
  drag.end();
  dragging = {};
  if (freeRack != nullptr) freeRack->setDropHighlight(false);
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
      // TX.all-free: a reorder rewires — the device leaves the path where it was (dissolved there: the path stays
      // connected) and is spliced in at the slot (counted with it in its old place, as the frames show it). One step.
      const int32_t from = frameIndex(id);
      if (from >= 0 && (d.index == static_cast<uint32_t>(from) || d.index == static_cast<uint32_t>(from) + 1)) return;
      run(doc::ops::moveOnPath(doc(), view.ctx_.store().ids(), view.ctx_.deviceLibrary(), id, view.track_, d.index));
      break;
    }
    case DropTarget::Kind::Chain: {
      uint32_t to = d.index;
      const ChainPlace at = chainPlaceOf(doc(), id);
      if (at.chain == d.chain && at.index >= 0 && d.index > static_cast<uint32_t>(at.index)) --to;
      const std::optional<doc::RackInfo> rk = doc::rackOf(doc(), d.chain);
      run(doc::ops::moveIntoChain(doc(), view.ctx_.store().ids(), id, to, d.chain));  // TX.all-free: off the path first
      if (rk)  // the rack's frame (its chain on the path) shows the chain it went into
        for (const doc::RackChain& c : rk->chains) shownChain[c.id.value] = d.chain;
      break;
    }
    case DropTarget::Kind::NewChain:
      run(doc::ops::moveDeviceToNewChain(doc(), view.ctx_.store().ids(), id, d.rack));
      break;
    case DropTarget::Kind::Free: (void)view.takeIntoRack(id); break;  // TX.free-rack
    case DropTarget::Kind::None: break;
  }
}

namespace {

// ---- A leaf's frame: its panel (GU.13: a synthetic group-system panel in a Live-style frame), plus selection, the reorder
// drag and the context menu ------------------------------------------------------------------------------------------

class DeviceFrame final : public FrameBase {
public:
  DeviceFrame(DeviceChainView::Impl& impl, ViewContext& ctx, const doc::Device& d, const PanelTemplate* tpl, std::string key)
      : FrameBase("device[" + std::to_string(d.id.value) + "]", impl, ctx, d.id), key_(std::move(key)) {
    setLayout(ui::LayoutKind::Custom);
    if (tpl != nullptr) {
      bool face = false;
      auto p = std::make_shared<const doc::Panel>(panel::leafPanel(tpl->sig, d, &face));
      auto gf = std::make_unique<panel::GroupPanelFace>([p]() -> doc::PanelPtr { return p; },
                                                         panel::makeDocPanelTarget(ctx, d.id, [this]() -> ui::Widget* {
                                                           return panel_ != nullptr ? &panel_->host() : nullptr;
                                                         }));
      gf->setOwner(d.id);
      panel::BlockFrame::Options o;
      o.title = tpl->title;
      o.exports = tpl->sig.exports;
      if (face) {  // TMM.10: a native engine's face scales with the Device view's height
        o.minScale = 0.5f;
        o.maxScale = 2.0f;
        o.minWidth = 212.0f;  // the title bar keeps room for a module's name
      }
      panel_ = &add<panel::BlockFrame>("panel", std::move(gf), std::move(o));
      panel_->setActive(d.active);
      panel_->onActivate = [this](bool on) {
        impl_.run(doc::ops::setDevicesActive(*ctx_.doc(), std::vector<doc::DeviceId>{id_}, on), false);
      };
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
      title_ = tpl->title;
      if (!d.name.empty()) panel_->setTitle(d.name);  // T4.17
      // T5.11: the preset slot names the last preset loaded here; a click hot-swaps the device's presets in the browser.
      panel_->setPresetName(ctx.library().presetName(d.id));
      panel_->onPresetClick = [this] { impl_.view.hotSwap(id_); };
    }
    if (tpl == nullptr) missing_ = "'" + d.patch + "' is not available: bypassed";
  }

  bool keeps(const doc::Device& d) const override {
    // Its template key (TMM.10-fix1: a native engine's key carries its position, so its face's socket follows it).
    return DeviceChainView::Impl::templateKey(ctx_.deviceLibrary(), d) == key_;
  }

  panel::BlockFrame* panel() const noexcept override { return panel_; }
  // The device changed in the document (values, settings, activation): the panel shows it.
  void update(const doc::Device& d) override {
    if (!panel_) return;
    panel_->setTitle(d.name.empty() ? title_ : d.name);  // T4.17
    panel_->setActive(d.active);
    panel_->setPresetName(ctx_.library().presetName(d.id));
    panel_->host().notifyChanged();
  }
  void paramsChanged(const doc::Device&, std::span<const doc::ParamRef>) override {
    if (panel_) panel_->host().notifyChanged();
  }
  float logicalWidth() const override { return panel_ ? panel_->logicalWidth() : 160.0f; }
  // TMM.10: a face panel scales with the Device view's height.
  float logicalWidthFor(float available) const override {
    return panel_ ? panel_->logicalWidthFor(logicalHeight(available)) : 160.0f;
  }
  // M4 review UI item 3: an open panel fills the Device view's height (its background continues below its controls, as
  // Live's devices do); folded, the title bar only.
  float logicalHeight(float available) const override {
    return panel_ && panel_->folded() ? std::min(available, panel_->logicalHeight()) : available;
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

  bool inTitle(ui::Vec2 p) const override { return panel_ ? panel_->titleRect().contains(p) : FrameBase::inTitle(p); }

private:
  std::string key_;  // the template it was built from (Impl::templateKey)
  std::string title_;
  panel::BlockFrame* panel_ = nullptr;
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
  int32_t totalWidthPx(float scale, float hLogical) const {
    float x = kChainPad;
    for (const FrameBase* f : impl_.frames) x += f->logicalWidthFor(hLogical - 2.0f * kChainPad) + kFrameGap;
    x += kAddW + kChainPad;
    return static_cast<int32_t>(std::lround(x * scale));
  }
  ui::ISize contentExtent(ui::ISize viewport) const override {
    const float s = tree() ? tree()->scale() : 1.0f;
    return {std::max(viewport.w, totalWidthPx(s, static_cast<float>(viewport.h) / s)), viewport.h};
  }
  void arrange(ui::LayoutPass& pass) override {
    const ui::IRect r = rect();
    const float hLogical = static_cast<float>(r.h) / pass.scale();
    const int32_t pad = pass.px(kChainPad), gap = pass.px(kFrameGap);
    int32_t x = r.x + pad;
    const int32_t h = std::max(0, r.h - 2 * pad);
    for (FrameBase* f : impl_.frames) {
      const int32_t w = pass.px(f->logicalWidthFor(hLogical - 2.0f * kChainPad));
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
  if (!ctx.res.theme) return;
  // T4.18: an automated param's control carries Live's LED in its corner (grey while overridden by hand). GU.13: on any of
  // the frame's panels (a leaf's, a group's: a member's control).
  if (!automated_.empty()) {
    std::vector<panel::PanelHost*> hs;
    hosts(hs);
    const auto& at = ctx.res.theme->automation;
    const float size = static_cast<float>(std::max(2, ctx.scale.px(4.0f)));
    for (const Mark& mk : automated_) {
      const doc::Binding b = doc::controlBinding(mk.device, mk.param);
      std::optional<ui::IRect> cr;
      for (const panel::PanelHost* h : hs)
        if (!cr) cr = h->controlRect(b);
      if (!cr) continue;
      const ui::IRect r = ui::intersect(*cr, rect());
      if (r.w < 2 * static_cast<int32_t>(size) || r.h < 2 * static_cast<int32_t>(size)) continue;
      // Left of the control (clear of its centred label) where the frame has room, else inside its corner.
      const int32_t outside = r.x - static_cast<int32_t>(size) - 1;
      const float x = static_cast<float>(outside >= rect().x + 1 ? outside : r.x + 1), y = static_cast<float>(r.y + 1);
      ctx.dl.addRectFilled({x, y, x + size, y + size}, mk.overridden ? at.ledOverridden : at.led);
    }
  }
  if (!selected_) return;
  // The selected device: an outline in the focus colour (Live highlights the selected device).
  paint::box(ctx.dl, rect(), 0.0f, 0, ctx.res.theme->accent.focus);
}

void FrameBase::hosts(std::vector<panel::PanelHost*>& out) const {
  if (panel::BlockFrame* b = panel(); b != nullptr && !b->folded()) out.push_back(&b->host());
}

std::optional<std::pair<doc::DeviceId, std::string>> FrameBase::controlAt(ui::Vec2 p) const {
  std::vector<panel::PanelHost*> hs;
  hosts(hs);
  for (const panel::PanelHost* h : hs)
    if (const doc::Binding* b = h->controlAt(p); b != nullptr && b->meta.empty()) return std::make_pair(b->device, b->name);
  return std::nullopt;
}

void FrameBase::setAutomated(std::vector<Mark> marks) {
  if (marks == automated_) return;
  automated_ = std::move(marks);
  markDirty();
}

void device_chain::forEachPanelControl(const FrameBase& f, const std::function<bool(const PanelControl&)>& fn) {
  std::vector<panel::PanelHost*> hs;
  f.hosts(hs);
  for (panel::PanelHost* h : hs) {
    bool stop = false;
    h->visitControls([&](const doc::Binding& b, const ui::IRect& r) {
      if (stop || !b.meta.empty()) return;
      stop = fn(PanelControl{b.device, b.name, r, h, &b});
    });
    if (stop) return;
  }
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
      contextItems(e.pos, items);  // T4.16c (a macro's Rename Macro first)
      // T4.18: over a control (a knob, a box, a toggle, a macro; GU.13: on any of the frame's panels — a leaf's, a
      // group's member, a rack's macro block): its automation lane in the arrangement. A macro-mapped param plays its
      // macro: no lane of its own unless it already has one (the arrangement's chooser).
      if (const std::optional<std::pair<doc::DeviceId, std::string>> ctl = controlAt(e.pos)) {
        const doc::DeviceId cd = ctl->first;
        const std::string param = ctl->second;
        const doc::DeviceLoc cl = doc::findDevice(*ctx_.doc(), cd);
        bool any = false;
        if (cl.track != nullptr && (!macroMappingOf(*ctx_.doc(), cd, param) ||
                                    findAutomationLane(*ctx_.doc(), doc::ParamRef::device(cd, param)).lane != nullptr)) {
          ViewContext* c = &ctx_;
          const doc::TrackId t = cl.track->id;
          items.push_back(ui::MenuItem::item("Show Automation", [c, t, cd, param] {
            c->showAutomation(t, doc::ParamRef::device(cd, param));
          }));
          any = true;
        }
        if (std::optional<ui::MenuItem> curve = knobCurveMenu(ctx_, cd, param)) {  // TX.knob-curves (a mapped knob too)
          items.push_back(std::move(*curve));
          any = true;
        }
        if (any) items.push_back(ui::MenuItem::sep());
      }
      // T5.x-circuit-chain: Open in Circuit View shows the whole track's chain as one circuit focused on this device.
      items.push_back(ui::MenuItem::item("Open in Circuit View", [v, id] { v->openInCircuitView(id); }));
      // TX.all-free: off the main path (its neighbours joined; it keeps its other cables: the free rack shows it).
      if (loc.device != nullptr && !isRack() && !chainPlaceOf(*ctx_.doc(), id).inChain())
        items.push_back(ui::MenuItem::item("Take Off Path", [v, id] { (void)v->takeIntoRack(id); }));
      // T5.11: the user library.
      items.push_back(ui::MenuItem::sep());
      items.push_back(ui::MenuItem::item("Save Preset", [v, id] { v->savePreset(id, false); }));
      if (loc.device != nullptr && !isRack()) {
        items.push_back(ui::MenuItem::item("Save as Default Preset", [v, id] { v->savePreset(id, true); }));
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
      items.push_back(ui::MenuItem::item("Rename", [v] { v->renameSelected(); }, "Ctrl+R"));  // T4.17
      items.push_back(ui::MenuItem::sep());
      items.push_back(ui::MenuItem::item("Group", [v] { v->groupSelected(); }, "Ctrl+G"));  // T4.8
      if (isRack())
        items.push_back(ui::MenuItem::item("Ungroup", [v] { v->ungroupSelected(); }, "Ctrl+Shift+G"));  // T4.17
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

bool FrameBase::onKey(const ui::KeyEvent& e) {
  if (e.down && e.key == ui::key::kEscape && impl_.dragging == id_ && impl_.drag.active) {
    impl_.endDrag(false);
    return true;
  }
  return Widget::onKey(e);
}

void FrameBase::onCaptureLost() {
  if (impl_.dragging == id_ && impl_.drag.active) impl_.endDrag(false);
}

void FrameBase::onInteractionCancelled() {
  if (impl_.dragging == id_ && impl_.drag.active) impl_.endDrag(false);
}

std::vector<doc::DevicePtr> device_chain::shownDevices(const std::vector<doc::DevicePtr>& list) { return list; }

std::vector<doc::DevicePtr> device_chain::pathDevices(const doc::Project& p, const doc::Track& t, const doc::MainPath& path) {
  std::vector<doc::DevicePtr> out = doc::flowOrder(t, path);
  out.resize(path.blocks.size());
  // TX.all-free: a meta block's kind means nothing on a track (model.h); its form — and so its face's socket — is what
  // reaches its main input along the path (meta.h trackStages: the note source makes it play the notes). The frame is
  // made from a copy of the block with that kind (the template key carries it), the document keeps the block as it is.
  bool meta = false;
  for (const doc::DevicePtr& d : out) meta = meta || doc::isMetaDevice(*d);
  if (!meta) return out;
  for (const doc::ChainStage& st : doc::trackStages(p, t)) {
    if (st.offPath || !doc::isMetaDevice(*st.device)) continue;
    const doc::DeviceKind k = st.in == doc::ChainSignal::Notes ? doc::DeviceKind::Instrument : doc::DeviceKind::AudioEffect;
    if (st.device->kind == k) continue;
    for (doc::DevicePtr& d : out)
      if (d.get() == st.device) {
        auto copy = std::make_shared<doc::Device>(*d);
        copy->kind = k;
        d = std::move(copy);
      }
  }
  return out;
}

std::unique_ptr<FrameBase> device_chain::makeFrame(DeviceChainView::Impl& impl, ViewContext& ctx, const doc::Device& d) {
  // GU.9: a chain of a rack (a set of parallel groups) stands for the rack: its frame.
  if (doc::isGroupDevice(d))
    if (const doc::DeviceLoc l = doc::findDevice(impl.doc(), d.id); l.track != nullptr && doc::rackOfChain(impl.doc(), *l.track, d.id))
      return makeRackFrame(impl, ctx, d);
  if (doc::isGroupDevice(d)) return makeGroupFrame(impl, ctx, d);  // TX.groups
  return std::make_unique<DeviceFrame>(impl, ctx, d, impl.templateFor(d),
                                       DeviceChainView::Impl::templateKey(ctx.deviceLibrary(), d));
}

// ---- DeviceChainView -------------------------------------------------------------------------------------------

DeviceChainView::DeviceChainView(ViewContext& ctx) : Widget("chain-view"), ctx_(ctx), impl_(std::make_unique<Impl>(*this)) {
  setLayout(ui::LayoutKind::Column);
  setFocusable(true);
  setKeyScope(ctx.scope("devices"));
  impl_->freeRack = &add<FreeRackPanel>(*impl_, ctx);  // TX.free-rack: over the chain
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
      impl_->sigs.clear();
      sync(true);
    }
    else if (bits & (view_change::kSelection | view_change::kDetailClip)) follow();
    if (bits & view_change::kLibrary)  // T5.11: preset names in the title bars
      for (auto& [id, f] : impl_->all)
        if (panel::BlockFrame* panel = f->panel(); panel != nullptr && !f->isRack())
          panel->setPresetName(ctx_.library().presetName(doc::DeviceId{id}));
    if (bits & view_change::kModulation) refreshRings();
    if (bits & view_change::kAnalysis)  // TX.spectrum: a device's analysis tap appeared: its displays and face look again
      wakeFaces();
    if (bits & view_change::kSignal) {  // KB.3: the engine is active again: the panels and the modulation rings poll
      wakeFaces();
      if (!ringsPolling()) pollRings();
    }
    if (bits & view_change::kResponses) {  // EQ.4b: the compile of a new structure arrived: the response displays draw it
      std::vector<panel::PanelHost*> hs;
      for (const auto& [id, f] : impl_->all) f->hosts(hs);
      for (panel::PanelHost* h : hs) h->notifyChanged();
    }
    if (bits & view_change::kAutomation) refreshAutomation();  // T4.18: overridden / re-enabled
    else if ((bits & view_change::kTransport) && (!impl_->autoShown.empty() || impl_->chainAuto)) applyAutomation();
  });
  sync(true);
}

DeviceChainView::~DeviceChainView() {
  impl_->retiredFrames.clear();  // before the rest of Impl (templates, maps) a frame may still reach
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

void DeviceChainView::rebuild() {
  if (impl_->freeRack != nullptr) {  // TX.free-rack: the rack's faces read the same voice taps as the chain's
    impl_->freeRack->view().faceVoices = faceVoices;
    impl_->freeRack->view().rebindFaces();
  }
  sync(true);
}

void DeviceChainView::sync(bool force) {
  Impl& m = *impl_;
  const doc::TrackId want = wantedTrack();
  const doc::Track* t = want.valid() ? doc::findTrackPtr(*ctx_.doc(), want) : nullptr;
  // TX.all-free: the list is the track's main path (a group's members are inside its panel; blocks off the path are in
  // the free rack).
  doc::MainPath path = t != nullptr ? doc::mainPath(*ctx_.doc(), *t) : doc::MainPath{};
  const std::vector<doc::DevicePtr> chain = t != nullptr ? pathDevices(*ctx_.doc(), *t, path) : std::vector<doc::DevicePtr>{};
  bool sameIds = !force && want == track_ && chain.size() == m.shown.size();
  for (size_t i = 0; sameIds && i < chain.size(); ++i)
    sameIds = chain[i]->id == m.shown[i]->id && m.frames[i]->keeps(*chain[i]);
  if (sameIds) {
    for (size_t i = 0; i < chain.size(); ++i)
      if (chain[i] != m.shown[i] || m.frames[i]->derived()) m.frames[i]->update(*chain[i]);
  } else {
    if (m.drag.active) m.endDrag(false);
    for (FrameBase* f : m.frames) m.retiredFrames.push_back(m.content->removeChild(*f));  // H1: not destroyed here
    m.frames.clear();
    if (!m.retiredFrames.empty()) requestFrame();
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
  m.mainPath = std::move(path);
  m.rackHeld.clear();  // GU.9: the track-level racks' blocks (inFreeRack)
  if (t != nullptr)
    for (const doc::RackInfo& r : doc::findRacks(*ctx_.doc(), *t)) {
      if (r.parent.valid()) continue;
      for (const doc::RackChain& c : r.chains) m.rackHeld.insert(c.id.value);
      for (const doc::RackMacro& mc : r.macros)
        if (mc.block.valid()) m.rackHeld.insert(mc.block.value);
      if (r.selector.valid()) m.rackHeld.insert(r.selector.value);
    }
  // TX.free-rack: the rack shows the track's free devices (hidden with no track); the frames' ports for the cables.
  if (m.freeRack != nullptr) {
    m.freeRack->setVisible(t != nullptr);
    m.freeRack->follow(want);
  }
  // The selection keeps only devices shown (any level).
  m.reindex();
  m.refreshPorts();
  std::erase_if(selection_, [&](doc::DeviceId id) { return m.frameOf(id) == nullptr; });
  for (auto& [id, f] : m.all)
    f->setSelected(std::find(selection_.begin(), selection_.end(), doc::DeviceId{id}) != selection_.end());
  if (m.mapRack.valid() && m.frameOf(m.mapRack) == nullptr) m.setMapMode({});
  if (auto* slot = static_cast<AddSlot*>(addSlot_)) {
    slot->setVisible(t != nullptr);
    slot->setHint(t == nullptr ? std::string() : chain.empty() ? "Add Device" : "Add");
  }
  refreshRings();  // T4.7
  refreshAutomation();  // T4.18
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
    doc::MainPath path = t != nullptr ? doc::mainPath(*e.after, *t) : doc::MainPath{};
    const std::vector<doc::DevicePtr> now = t != nullptr ? pathDevices(*e.after, *t, path) : std::vector<doc::DevicePtr>{};
    if (t == nullptr || now.size() != m.shown.size()) return sync(false);
    for (size_t i = 0; i < now.size(); ++i) {
      if (now[i] == m.shown[i] && !m.frames[i]->derived()) continue;  // GU.9: a rack's blocks are not its device
      m.frames[i]->paramsChanged(*now[i], c.params);
      m.shown[i] = now[i];
    }
    m.shownFrom = e.after;
    m.mainPath = std::move(path);
    // A modulated knob's ring follows it; T4.8: a macro moves its mapped knobs' rings.
    // (TX.all-free: a main link drives no knob.)
    const doc::WireList& ws = doc::wiresOf(*e.after);
    if (std::any_of(ws.begin(), ws.end(), [](const doc::Wire& w) { return !w.mainLink(); }) ||
        std::any_of(m.all.begin(), m.all.end(), [](const auto& f) { return f.second->isRack(); }))
      refreshRings();
    if (!m.autoShown.empty()) refreshAutomation();  // T4.18: an automated knob shows its envelope, not the new value
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
      oneList = oneList && l.device != nullptr && l.track == first.track && l.device->group == first.device->group;
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
  const std::vector<doc::DevicePtr>* list = &impl_->shown;  // TX.all-free: the main path
  if (!selection_.empty())
    if (const ChainPlace at = chainPlaceOf(*ctx_.doc(), selection_.back()); at.inChain()) {
      if (at.list.empty()) return false;
      const doc::DeviceId one[1] = {last ? at.list.back()->id : at.list.front()->id};
      select(one);
      revealDevice(one[0]);
      return true;
    }
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
  // A rack chain is a series; the track's list is its main path (TX.all-free: the move rewires either).
  const ChainPlace at0 = chainPlaceOf(*ctx_.doc(), id);
  const std::vector<doc::DevicePtr>& list = at0.inChain() ? at0.list : impl_->shown;
  int64_t at = -1;
  for (size_t i = 0; i < list.size(); ++i)
    if (list[i]->id == id) at = static_cast<int64_t>(i);
  if (at < 0) return false;
  const int64_t to = at + dir;
  if (to < 0 || to >= static_cast<int64_t>(list.size())) return false;
  // An Instrument Rack's chain keeps its instrument first: neither it nor an effect passes it (TMM.10-fix1: a chain meta
  // device first is a stage like any other). TX.all-free: the track's main path has no such rule.
  if (at0.inChain() && at0.rack->instrument && list.front()->kind == doc::DeviceKind::Instrument && !doc::isMetaDevice(*list.front()) &&
      (at == 0 || to == 0)) {
    ctx_.setStatus("The instrument stays first in the chain");
    return false;
  }
  const bool r = at0.inChain()
                     ? impl_->run(doc::ops::moveIntoChain(*ctx_.doc(), ctx_.store().ids(), id, static_cast<uint32_t>(to), at0.chain))
                     : impl_->run(doc::ops::moveOnPath(*ctx_.doc(), ctx_.store().ids(), ctx_.deviceLibrary(), id, track_,
                                                       static_cast<uint32_t>(dir > 0 ? to + 1 : to)));
  if (r) revealDevice(id);
  return r;
}

bool DeviceChainView::showNeighbourChain(int32_t dir) {
  // The selected rack, or the rack whose shown chain holds the selected device: Up / Down step through its chains (Live's
  // chain list); a device inside the old chain hands the selection to the first device of the new one.
  if (selection_.size() != 1) return false;
  const doc::Project& p = *ctx_.doc();
  const doc::DeviceId sel = selection_.front();
  // The rack: the selected rack frame's, else the one whose chain holds the selected device.
  doc::DeviceId frame;
  if (FrameBase* f = impl_->frameOf(sel); f != nullptr && f->isRack()) frame = sel;
  const ChainPlace place = chainPlaceOf(p, sel);
  std::optional<doc::RackInfo> rk = frame.valid() ? doc::rackOf(p, frame) : place.rack;
  if (!rk || rk->chains.empty()) return false;
  if (!frame.valid())
    for (const doc::RackChain& c : rk->chains)
      if (impl_->frameOf(c.id) != nullptr && impl_->frameOf(c.id)->isRack()) frame = c.id;
  if (!frame.valid()) return false;
  const auto& chains = rk->chains;
  const doc::ChainId cur = shownChain(frame);
  int64_t at = 0;
  for (size_t i = 0; i < chains.size(); ++i)
    if (chains[i].id == cur) at = static_cast<int64_t>(i);
  const int64_t to = std::clamp<int64_t>(at + dir, 0, static_cast<int64_t>(chains.size()) - 1);
  if (to == at) return true;
  const doc::RackChain& next = chains[static_cast<size_t>(to)];
  showChain(frame, next.id);
  if (frame != sel) {
    const doc::DeviceId one[1] = {next.devices.empty() ? frame : next.devices.front()->id};
    select(one);
  }
  revealDevice(selection_.front());
  return true;
}

std::vector<ui::MenuItem> DeviceChainView::deviceMenu() {
  const doc::Track* t = impl_->track();
  if (t == nullptr) return impl_->deviceMenu({}, {}, doc::kAppend, false);
  uint32_t index = doc::kAppend;
  index = slotAfterSelection(index);  // TX.all-free: on the main path
  return impl_->deviceMenu(track_, {}, index, t->kind == doc::TrackKind::Midi);
}

void DeviceChainView::wakeFaces() {
  std::vector<panel::PanelHost*> hs;
  for (const auto& [id, f] : impl_->all) f->hosts(hs);
  for (panel::PanelHost* h : hs) h->wake();
}

bool DeviceChainView::addDevice(const doc::DeviceInfo& info) {
  const doc::Track* t = impl_->track();
  if (t == nullptr) return false;
  uint32_t index = doc::kAppend;
  index = slotAfterSelection(index);  // TX.all-free: on the main path
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
    if (k != LibraryItem::CatalogDevice && k != LibraryItem::Preset) return {};
  }
  return {ui::DropEffect::Copy, ui::Cursor::Arrow};
}

ui::DropEffect DeviceChainView::drop(const ui::DragPayload& p, ui::Vec2, ui::Mods) {
  if (dragOver(p, {}, ui::mod::kNone).effect == ui::DropEffect::None) return ui::DropEffect::None;
  // After the selected device of the shown chain, else at the end (one undo step per item).
  uint32_t index = doc::kAppend;
  if (const doc::Track* t = impl_->track())
    index = slotAfterSelection(index);  // TX.all-free: on the main path
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

uint32_t DeviceChainView::slotAfterSelection(uint32_t otherwise) const {
  if (selection_.empty()) return otherwise;
  for (size_t i = 0; i < impl_->shown.size(); ++i)
    if (impl_->shown[i]->id == selection_.back()) return static_cast<uint32_t>(i + 1);
  return otherwise;
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
                      [&] {  // inside a deleted rack (GU.9: inside one of its chain groups)
                        for (const doc::DeviceId s : sel)
                          if (const std::optional<doc::RackInfo> rk = doc::rackOf(*ctx_.doc(), s); rk && rk->holds(*ctx_.doc(), id)) return true;
                        return false;
                      }();
    if (gone) seen = true;
    else if (!seen) prev = id;
    else if (!next.valid()) next = id;
  }
  // TX.all-free: Delete DISSOLVES (the main path through them stays connected; their other cables go with them).
  // GU.9: a rack's frame deletes the rack — every chain group (the last one joins its feed to its sum), its macros.
  std::vector<doc::DeviceId> plain, chains;
  for (const doc::DeviceId id : sel) {
    const FrameBase* f = impl_->frameOf(id);
    const std::optional<doc::RackInfo> rk = f != nullptr && f->isRack() ? doc::rackOf(*ctx_.doc(), id) : std::nullopt;
    if (rk)
      for (const doc::RackChain& c : rk->chains) chains.push_back(c.id);
    else
      plain.push_back(id);
  }
  if (!chains.empty() && plain.empty()) {
    if (!impl_->run(doc::ops::deleteChains(*ctx_.doc(), ctx_.store().ids(), chains))) return false;
  } else if (!impl_->run(doc::ops::dissolveDevices(*ctx_.doc(), ctx_.store().ids(), ctx_.deviceLibrary(), plain))) {
    return false;
  }
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
  // In display order (whatever order they were selected in).
  for (const doc::DeviceId id : shownOrder()) {
    if (std::find(selection_.begin(), selection_.end(), id) == selection_.end()) continue;
    const doc::DeviceLoc loc = doc::findDevice(*ctx_.doc(), id);
    if (loc.device != nullptr) clipboard_.push_back(loc.list()[loc.index]);
  }
  return !clipboard_.empty();
}

bool DeviceChainView::cutSelected() { return copySelected() && deleteSelected(); }

bool DeviceChainView::paste() {
  const doc::Track* t = impl_->track();
  if (t == nullptr || clipboard_.empty()) return false;
  // Into the list of the last selected device (a rack chain or the track's), right after it; else at the track's end.
  if (!selection_.empty()) {
    if (const ChainPlace at = chainPlaceOf(*ctx_.doc(), selection_.back()); at.inChain()) {
      // GU.9: pasted onto the track off the path, then each into the chain after the selection (one step each).
      bool any = false;
      uint32_t index = static_cast<uint32_t>(at.index + 1);
      for (const doc::DevicePtr& d : clipboard_) {
        const doc::ops::EditResult pasted = doc::ops::pasteDevices(*ctx_.doc(), ctx_.store().ids(), track_,
                                                                   std::span<const doc::DevicePtr>(&d, 1), doc::ops::kOffPath);
        if (!pasted || pasted->devices.empty() || !impl_->run(pasted)) continue;
        any = impl_->run(doc::ops::moveIntoChain(*ctx_.doc(), ctx_.store().ids(), pasted->devices.front(), index++, at.chain)) || any;
      }
      return any;
    }
  }
  uint32_t index = doc::kAppend;
  index = slotAfterSelection(index);  // TX.all-free: on the main path
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
  // TX.all-free: `index` is the slot on the main path, counted with the device in its old place (the frames' order).
  return impl_->run(doc::ops::moveOnPath(*ctx_.doc(), ctx_.store().ids(), ctx_.deviceLibrary(), device, track_, index));
}

bool DeviceChainView::openInCircuitView(doc::DeviceId device) {
  return ctx_.openProjectCircuit({}, device);  // T5.x-one-circuit: the project patch view, framed on it
}

bool DeviceChainView::groupSelected() {
  if (selection_.empty()) return false;
  const std::vector<doc::DeviceId> sel = selection_;
  return impl_->run(doc::ops::groupDevices(*ctx_.doc(), ctx_.store().ids(), sel));
}

bool DeviceChainView::addRack(bool instrument) {
  const doc::Track* t = track_.valid() ? doc::findTrackPtr(*ctx_.doc(), track_) : nullptr;
  if (t == nullptr) return false;
  uint32_t index = doc::kAppend;
  if (selection_.size() == 1) index = slotAfterSelection(index);  // TX.all-free: on the main path
  return impl_->run(doc::ops::addDevice(*ctx_.doc(), ctx_.store().ids(), track_,
                                        std::string(instrument ? doc::kInstrumentRack : doc::kAudioEffectRack),
                                        instrument ? doc::DeviceKind::Instrument : doc::DeviceKind::AudioEffect, index));
}

bool DeviceChainView::ungroupSelected() {
  if (selection_.size() != 1) {
    if (!selection_.empty()) ctx_.setStatus("Ungroup: select one rack");
    return false;
  }
  const std::optional<doc::RackInfo> rk = doc::rackOf(*ctx_.doc(), selection_.front());
  if (!rk) {
    ctx_.setStatus("Ungroup: the selected device is not a rack");
    return false;
  }
  if (rk->chains.size() != 1) {
    ctx_.setStatus("Ungroup: a rack with one chain (this one has " + std::to_string(rk->chains.size()) + ")");
    return false;
  }
  const auto r = doc::ops::ungroupDevices(*ctx_.doc(), ctx_.store().ids(), rk->id);
  if (!r || !impl_->run(r)) return false;
  select(r->devices);  // the rack's devices take its place; they become the selection (Live)
  return true;
}

bool DeviceChainView::renameSelected() {
  if (selection_.size() != 1) return false;
  const doc::DeviceId id = selection_.front();
  FrameBase* f = impl_->frameOf(id);
  panel::BlockFrame* p = f != nullptr ? f->panel() : nullptr;
  if (p == nullptr) return false;
  if (p->folded()) p->setFolded(false);
  ViewContext* c = &ctx_;
  Impl* im = impl_.get();
  p->beginRename([c, im, id](const std::string& name) { im->run(doc::ops::renameDevice(*c->doc(), id, name)); });
  return true;
}

ui::Widget* DeviceChainView::frameOf(doc::DeviceId device) const noexcept { return impl_->frameOf(device); }

panel::BlockFrame* DeviceChainView::panelOf(doc::DeviceId device) const noexcept {
  FrameBase* f = impl_->frameOf(device);
  return f != nullptr ? f->panel() : nullptr;
}

std::optional<ui::IRect> DeviceChainView::controlRect(doc::DeviceId device, std::string_view param) const {
  if (FrameBase* f = impl_->frameOf(device); f != nullptr && f->isRack()) {  // a rack's macro ("macro<k>")
    std::optional<ui::IRect> out;
    forEachPanelControl(*f, [&](const PanelControl& c) {
      if (c.device == device && c.param == param) out = c.rect;
      return out.has_value();
    });
    return out;
  }
  doc::Binding b;
  const panel::PanelHost* h = impl_->hostOf(device, param, &b);
  return h != nullptr ? h->controlRect(b) : std::nullopt;
}

panel::PanelHost* DeviceChainView::controlHost(doc::DeviceId device, std::string_view param) const {
  if (FrameBase* f = impl_->frameOf(device); f != nullptr && f->isRack()) {
    panel::PanelHost* out = nullptr;
    forEachPanelControl(*f, [&](const PanelControl& c) {
      if (c.device == device && c.param == param) out = c.host;
      return out != nullptr;
    });
    return out;
  }
  return impl_->hostOf(device, param);
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
  ctx.setViewAction("device.ungroup", [p] { p->ungroupSelected(); }, has);  // T4.17
  ctx.setViewAction("device.rename", [p] { p->renameSelected(); }, has);    // T4.17
  // M4 review UI item 6: Create > Instrument Rack / Audio Effect Rack (after the selected device, else at the end).
  ctx.setViewAction("device.addInstrumentRack", [p] { p->addRack(true); }, [p] { return p->track().valid(); });
  ctx.setViewAction("device.addAudioEffectRack", [p] { p->addRack(false); }, [p] { return p->track().valid(); });
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
  // TX.free-rack: the expanded rack grows the detail area (the body splitter's lower pane): the arrangement moves up,
  // the chain keeps its height.
  if (auto* body = dynamic_cast<ui::Splitter*>(shell.tree().find("main/body"))) {
    p->onRackHeight = [body](float dy) { body->setPosition(body->position() + dy); };
    if (p->freeRackOpen()) body->setPosition(body->position() + p->freeRackHeight() + DeviceChainView::kRackHandleH);
  }
  return v;
}

// ---- TX.free-rack: the rack, and the ends of the cables of the signal path --------------------------------------------

bool DeviceChainView::freeRackOpen() const noexcept { return impl_->freeRack != nullptr && impl_->freeRack->open(); }
void DeviceChainView::setFreeRackOpen(bool open) {
  if (impl_->freeRack != nullptr) impl_->freeRack->setOpen(open);
}
float DeviceChainView::freeRackHeight() const noexcept { return impl_->freeRack != nullptr ? impl_->freeRack->bodyHeight() : 0.0f; }
void DeviceChainView::setFreeRackHeight(float logicalPx) {
  if (impl_->freeRack != nullptr) impl_->freeRack->setBodyHeight(logicalPx);
}
project_patch::ProjectPatchView* DeviceChainView::freeRackView() const noexcept {
  return impl_->freeRack != nullptr && impl_->freeRack->visible() ? &impl_->freeRack->view() : nullptr;
}
ui::IRect DeviceChainView::freeRackRect() const noexcept {
  return impl_->freeRack != nullptr && impl_->freeRack->visible() ? impl_->freeRack->rect() : ui::IRect{};
}

bool DeviceChainView::addFreeDevice(const std::string& patch) {
  if (!track_.valid() || impl_->freeRack == nullptr) return false;
  setFreeRackOpen(true);  // the new device is shown where it went
  return impl_->freeRack->view().addFreeDevice(track_, patch);
}

bool DeviceChainView::takeIntoRack(doc::DeviceId device) {
  // TX.all-free: off the main path (its neighbours joined), so into the free rack. One step.
  const doc::DeviceId one[1] = {device};
  if (!impl_->run(doc::ops::takeOffPath(*ctx_.doc(), ctx_.store().ids(), ctx_.deviceLibrary(), one), false)) return false;
  selection_.clear();
  return true;
}

bool DeviceChainView::putIntoChainAt(doc::DeviceId device, uint32_t slot) {
  if (impl_->track() == nullptr) return false;
  // TX.all-free: spliced into the main path at the slot (the frames are the path). One step.
  if (!impl_->run(doc::ops::insertOnPath(*ctx_.doc(), ctx_.store().ids(), device, slot), false)) return false;
  const doc::DeviceId one[1] = {device};
  select(one);
  return true;
}

std::optional<uint32_t> DeviceChainView::chainSlotAt(ui::Vec2 pos) const {
  if (!track_.valid() || !chainViewport().contains(pos)) return std::nullopt;
  return impl_->trackSlot(pos, {}).index;
}

void DeviceChainView::Impl::refreshPorts() {
  ports.clear();
  const doc::DeviceLibrary* lib = view.ctx_.deviceLibrary();
  const doc::Project& p = doc();
  for (const auto& [id, f] : all) {
    const doc::Device* d = doc::findDevice(p, doc::DeviceId{id}).device;
    if (d == nullptr) continue;
    Ports& out = ports[id];
    const graph::Patch* patch = lib != nullptr && !f->isRack() ? lib->patchOf(*d) : nullptr;
    if (patch == nullptr) {  // a rack, a missing patch: its chain's ports
      if (d->kind != doc::DeviceKind::Instrument) out.inputs.push_back("in");
      out.outputs.push_back("out");
      continue;
    }
    std::vector<std::string_view> params;
    for (graph::NodeIndex k = 0; k < patch->nodeCount(); ++k)
      if (patch->node(k).kind == graph::NodeKind::Param) params.push_back(patch->nodeName(k));
    auto isParam = [&](std::string_view n) { return std::find(params.begin(), params.end(), n) != params.end(); };
    const graph::SubcircuitRef sig = graph::subcircuitSignature(*patch);
    const bool group = doc::isGroupDevice(*d);
    for (const graph::SubcircuitPort& sp : sig.inputs) {
      if (sp.type.rate == graph::Rate::Const || isParam(sp.name)) continue;
      if (sp.name.size() > 4 && sp.name.ends_with("_add") && isParam(std::string_view(sp.name).substr(0, sp.name.size() - 4))) continue;
      if (group && sp.name == doc::kGroupOutPort) continue;  // TX.groups: the Out terminal is inside the panel
      out.inputs.push_back(sp.name);
    }
    for (const graph::SubcircuitPort& sp : sig.outputs) {
      if (group && sp.name == doc::kGroupInPort) continue;
      out.outputs.push_back(sp.name);
    }
    if (out.outputs.empty()) out.outputs.push_back("out");
  }
}

namespace {
constexpr float kSocketRow = 12.0f, kSocketPitch = 14.0f, kSocketR = 5.0f;  // logical px
bool shownNow(const ui::Widget& w) noexcept;                    // below (the T4.7 hooks)
ui::Vec2 clampInto(ui::Vec2 p, const ui::IRect& r, bool& inside);
}  // namespace

bool DeviceChainView::isInput(doc::DeviceId device, std::string_view port) const {
  const Impl::Ports* ps = impl_->portsOf(device);
  return ps != nullptr && std::find(ps->inputs.begin(), ps->inputs.end(), port) != ps->inputs.end();
}

namespace {
// A socket on a frame's edge (left: inputs, right: outputs), `row` rows up from its bottom (never above its title bar).
ui::Vec2 socketAt(const ui::IRect& r, bool right, size_t row, float us) {
  const float x = right ? static_cast<float>(r.right()) : static_cast<float>(r.x);
  const float y = std::max(static_cast<float>(r.y) + 10.0f * us,
                           static_cast<float>(r.bottom()) - (kSocketRow + kSocketPitch * static_cast<float>(row)) * us);
  return {x, std::round(y)};
}
}  // namespace

bool DeviceChainView::heldByRack(doc::DeviceId device) const { return impl_->rackHeld.count(device.value) != 0; }

bool DeviceChainView::inFreeRack(doc::DeviceId device) const {
  const doc::Track* t = impl_->track();
  if (t == nullptr || impl_->freeRack == nullptr || !impl_->freeRack->visible()) return false;
  const int32_t i = t->deviceIndex(device);
  if (i < 0) return false;
  const doc::Device& d = *t->devices[static_cast<size_t>(i)];
  // TX.all-free: off the main path; a member is in the rack with its group when the group is off the path.
  // (GU.2: a member at any depth is shown with its outermost group.)
  const doc::DeviceId outer = doc::outermostGroup(t->devices, d.id);
  const doc::DeviceId shownAs = outer.valid() ? outer : d.id;
  // GU.9: a rack's other chains, its macros and its selector are the rack frame's (not the free rack's).
  return !impl_->mainPath.contains(shownAs) && impl_->rackHeld.count(shownAs.value) == 0;
}

ui::Vec2 DeviceChainView::tabPoint(doc::DeviceId device) const {
  const float us = tree() ? tree()->scale() : 1.0f;
  const ui::IRect r = impl_->freeRack != nullptr ? impl_->freeRack->rect() : ui::IRect{};
  uint32_t k = 0;
  if (const doc::Track* t = impl_->track())
    for (const doc::DevicePtr& d : t->devices) {
      if (d->id == device) break;
      k += impl_->mainPath.contains(d->id) ? 0u : 1u;  // TX.all-free: the blocks off the main path, in list order
    }
  const float x = std::min(static_cast<float>(r.x) + (36.0f + 10.0f * static_cast<float>(k % 12)) * us, static_cast<float>(r.right()) - 4.0f);
  return {std::round(x), static_cast<float>(r.y) + kRackTabH * us};
}

std::optional<DeviceChainView::CableEnd> DeviceChainView::sourceEnd(doc::DeviceId device, std::string_view port) const {
  const doc::DeviceLoc l = doc::findDevice(*ctx_.doc(), device);
  if (l.device == nullptr) return std::nullopt;
  const float us = tree() ? tree()->scale() : 1.0f;
  // A free device of the shown track: its pin in the rack (collapsed: the rack's tab).
  if (inFreeRack(device)) {
    if (impl_->freeRack->open())
      if (bool docked = false; auto q = impl_->freeRack->view().outputPoint(device, port, &docked))
        return CableEnd{*q, docked, EndDir::Right};
    return CableEnd{tabPoint(device), false, EndDir::Down};
  }
  const panel::BlockFrame* panel = panelOf(device);
  if (panel != nullptr)
    for (const panel::BlockFrame::Jack& j : panel->jacks())
      if (j.name == port) {  // an export's jack on the panel's top strip
        bool docked = false;
        const auto q = jackPoint(device, port, &docked);
        return CableEnd{*q, docked, EndDir::Up};
      }
  if (const FrameBase* f = impl_->frameOf(device); f != nullptr && shownNow(*f)) {
    const Impl::Ports* ps = impl_->portsOf(device);
    size_t row = 0;
    if (ps != nullptr)
      for (size_t i = 0; i < ps->outputs.size(); ++i)
        if (ps->outputs[i] == port) row = i;
    bool inside = false;
    const ui::Vec2 q = clampInto(socketAt(f->rect(), true, row, us), chainViewport(), inside);
    return CableEnd{q, inside, EndDir::Right};
  }
  bool docked = false;
  if (auto q = groupPoint(device, true, &docked)) return CableEnd{*q, docked, EndDir::Up};
  return std::nullopt;
}

std::optional<DeviceChainView::CableEnd> DeviceChainView::targetEnd(doc::DeviceId device, std::string_view port) const {
  const doc::DeviceLoc l = doc::findDevice(*ctx_.doc(), device);
  if (l.device == nullptr) return std::nullopt;
  const float us = tree() ? tree()->scale() : 1.0f;
  if (inFreeRack(device)) {
    if (impl_->freeRack->open()) {
      bool docked = false;
      if (auto q = impl_->freeRack->view().inputPoint(device, port, &docked)) return CableEnd{*q, docked, EndDir::Left};
    }
    return CableEnd{tabPoint(device), false, EndDir::Down};
  }
  if (isInput(device, port))
    if (const FrameBase* f = impl_->frameOf(device); f != nullptr && shownNow(*f)) {
      const Impl::Ports* ps = impl_->portsOf(device);
      size_t row = 0;
      for (size_t i = 0; i < ps->inputs.size(); ++i)
        if (ps->inputs[i] == port) row = i;
      bool inside = false;
      const ui::Vec2 q = clampInto(socketAt(f->rect(), false, row, us), chainViewport(), inside);
      return CableEnd{q, inside, EndDir::Left};
    }
  bool docked = false;
  if (auto q = paramPoint(device, port, &docked)) return CableEnd{*q, docked, EndDir::Up};
  return std::nullopt;
}

std::optional<DeviceChainView::CableEnd> DeviceChainView::notesEnd() const {
  const doc::Track* t = impl_->track();
  if (t == nullptr || t->kind == doc::TrackKind::Master || t->kind == doc::TrackKind::Return) return std::nullopt;
  const float us = tree() ? tree()->scale() : 1.0f;
  const ui::IRect vp = chainViewport();
  const float y = static_cast<float>(vp.bottom()) - (kChainPad + kSocketRow) * us;
  return CableEnd{{static_cast<float>(vp.x) + 1.0f, std::round(y)}, true, EndDir::Right};
}

std::vector<DeviceChainView::ChainLink> DeviceChainView::chainLinks() const {
  std::vector<ChainLink> out;
  const doc::Track* t = impl_->track();
  if (t == nullptr) return out;
  const std::vector<doc::ChainStage> stages = doc::trackStages(*ctx_.doc(), *t);  // TX.all-free: along the main path
  const doc::Device* prev = nullptr;
  bool fromSource = t->kind == doc::TrackKind::Midi || t->kind == doc::TrackKind::Audio;  // the clips reach the first one
  for (const doc::ChainStage& s : stages) {
    if (s.offPath || s.device == nullptr || s.device->group.valid()) continue;  // (IG.1-2: a group's series: its own)
    if (impl_->frameOf(s.device->id) == nullptr) continue;
    if (s.in == doc::ChainSignal::None) {  // unplugged: nothing goes in; the next stage hears it
      prev = s.device;
      fromSource = false;
      continue;
    }
    ChainLink k;
    k.to = s.device->id;
    k.rate = s.in == doc::ChainSignal::Audio ? 1 : 2;
    if (doc::isMetaDevice(*s.device) && s.input >= 0)
      if (const meta::Type* type = meta::findType(doc::metaDeviceType(*s.device)); type != nullptr && static_cast<size_t>(s.input) < type->inputs.size())
        k.input = std::string(type->inputs[static_cast<size_t>(s.input)].name);
    const Impl::Ports* ps = impl_->portsOf(k.to);
    if (k.input.empty()) k.input = ps != nullptr && !ps->inputs.empty() ? ps->inputs.front() : std::string("notes");
    std::optional<CableEnd> a, b;
    if (prev != nullptr) {
      k.from = prev->id;
      const Impl::Ports* pp = impl_->portsOf(prev->id);
      a = sourceEnd(prev->id, pp != nullptr && !pp->outputs.empty() ? pp->outputs.front() : std::string("out"));
    } else if (fromSource) {
      a = notesEnd();
    }
    if (const FrameBase* f = impl_->frameOf(k.to); f != nullptr && shownNow(*f)) {
      size_t row = 0;
      if (ps != nullptr)
        for (size_t i = 0; i < ps->inputs.size(); ++i)
          if (ps->inputs[i] == k.input) row = i;
      bool inside = false;
      const float us = tree() ? tree()->scale() : 1.0f;
      b = CableEnd{clampInto(socketAt(f->rect(), false, row, us), chainViewport(), inside), inside, EndDir::Left};
      b->docked = inside;
    }
    prev = s.device;
    fromSource = false;
    if (!a || !b) continue;
    k.a = *a;
    k.b = *b;
    out.push_back(std::move(k));
  }
  return out;
}

std::vector<DeviceChainView::InputSocket> DeviceChainView::inputSockets() const {
  std::vector<InputSocket> out;
  const ui::IRect vp = chainViewport();
  const float us = tree() ? tree()->scale() : 1.0f;
  const auto half = static_cast<int32_t>(std::lround(kSocketR * us)) + 1;
  for (const auto& [id, f] : impl_->all) {
    if (!shownNow(*f)) continue;
    const Impl::Ports* ps = impl_->portsOf(doc::DeviceId{id});
    if (ps == nullptr) continue;
    for (size_t i = 0; i < ps->inputs.size(); ++i) {
      const ui::Vec2 c = socketAt(f->rect(), false, i, us);
      const ui::IRect r{static_cast<int32_t>(std::lround(c.x)) - half, static_cast<int32_t>(std::lround(c.y)) - half, 2 * half, 2 * half};
      if (ui::intersect(r, vp).empty()) continue;
      out.push_back({doc::DeviceId{id}, ps->inputs[i], r});
    }
  }
  return out;
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
  // GU.11 / GU.13: the controls of every frame's panels — a leaf's (its knobs, its face's controls), a group's (each bound
  // to a block of the group: a value block's `value`, a native engine's param) — but not a switch, nor a rack's macros.
  for (const auto& [id, f] : impl_->all)
    device_chain::forEachPanelControl(*f, [&](const device_chain::PanelControl& c) {
      if (f->isRack() && c.device == f->id()) return false;
      if (c.host->panelFace()->target().info(*c.binding).isSwitch) return false;
      const ui::IRect r = ui::intersect(c.rect, vp);
      if (!r.empty()) out.push_back({c.device, std::string(c.param), r});
      return false;
    });
  return out;
}

// TX.groups: a group member has no frame of its own: its cables end on its group's panel (the title bar's left end for
// an output, its right end for a param), not docked.
std::optional<ui::Vec2> DeviceChainView::groupPoint(doc::DeviceId device, bool output, bool* docked) const {
  const doc::DeviceLoc l = doc::findDevice(*ctx_.doc(), device);
  if (l.device == nullptr || !l.device->group.valid() || l.track == nullptr) return std::nullopt;
  // GU.2: a member at any depth: its outermost group's frame (nested groups have no frame of their own).
  const device_chain::FrameBase* f = impl_->frameOf(doc::outermostGroup(l.track->devices, device));
  if (f == nullptr) return std::nullopt;
  const ui::IRect r = f->rect();
  const ui::Vec2 p{output ? static_cast<float>(r.x) + 8.0f : static_cast<float>(r.right()) - 6.0f, static_cast<float>(r.y) + 10.0f};
  bool inside = false;
  const ui::Vec2 q = clampInto(p, chainViewport(), inside);
  if (docked != nullptr) *docked = false;
  return q;
}

std::optional<ui::Vec2> DeviceChainView::jackPoint(doc::DeviceId device, std::string_view port, bool* docked) const {
  const panel::BlockFrame* panel = panelOf(device);
  if (panel == nullptr) return groupPoint(device, true, docked);
  ui::Vec2 p{static_cast<float>(panel->rect().x) + 8.0f, static_cast<float>(panel->rect().y)};
  for (const panel::BlockFrame::Jack& j : panel->jacks())
    if (j.name == port) p = {static_cast<float>(j.rect.x) + 0.5f * static_cast<float>(j.rect.w),
                             static_cast<float>(j.rect.y) + 0.5f * static_cast<float>(j.rect.h)};
  bool inside = false;
  const ui::Vec2 q = clampInto(p, chainViewport(), inside);
  if (docked != nullptr) *docked = inside;
  return q;
}

std::optional<ui::Vec2> DeviceChainView::paramPoint(doc::DeviceId device, std::string_view param, bool* docked) const {
  // GU.13: the control on whichever panel shows it (a leaf's, its group's): docked on it while it is in view.
  doc::Binding b;
  if (const panel::PanelHost* h = impl_->hostOf(device, param, &b))
    if (const std::optional<ui::Vec2> p = h->controlPoint(b)) {
      bool inside = false;
      const ui::Vec2 q = clampInto(*p, chainViewport(), inside);
      if (docked != nullptr) *docked = inside;
      return q;
    }
  const panel::BlockFrame* panel = panelOf(device);
  if (panel == nullptr) return groupPoint(device, false, docked);
  const ui::IRect r = panel->rect();  // not shown now (folded, another tab): the title bar's right end
  bool inside = false;
  const ui::Vec2 q = clampInto({static_cast<float>(r.right()) - 6.0f, static_cast<float>(r.y) + 14.0f}, chainViewport(), inside);
  if (docked != nullptr) *docked = false;
  return q;
}

bool DeviceChainView::ringsPolling() const noexcept { return impl_->ringTimer != 0; }

namespace {
// What a site's tap is in (lower.h "Modulation wires": the last stage's): 0 knob position (Travel wires), 1 an Add wire's
// (GU.1: position on a curved control, the curve's units on a linear one), 2 the DSP units (migrated Units wires).
uint8_t ringLaw(const doc::Wire& w) noexcept { return w.travels() ? 0 : w.law == doc::WireLaw::Units ? 2 : 1; }
doc::ModulationTap::Domain ringDomain(uint8_t law, const devices::ParamInfo& info) noexcept {
  using D = doc::ModulationTap::Domain;
  return law == 2 ? D::Dsp : law == 1 && doc::addsInUnits(info.curve) ? D::Curve : D::Position;
}
}  // namespace

void DeviceChainView::refreshRings() {
  Impl& m = *impl_;
  const doc::Project& p = *ctx_.doc();
  m.ringTargets.clear();
  m.ringSpan.clear();
  m.ringUnits.clear();
  for (const doc::Wire& w : doc::wiresOf(p)) {
    if (m.frameFor(w.to) == nullptr) continue;  // GU.13: a group member's control is on its group's panel
    // GU.9: a macro mapping (a Travel wire from a macro's value block) is drawn as the mapping's range, not a ring.
    if (w.travels() && !w.fromNoteSource())
      if (const doc::Device* src = doc::findDevice(p, w.from).device;
          src != nullptr && doc::isValueDevice(*src) && src->setting(doc::kMacroSetting) != nullptr)
        continue;
    // TX.patching: an added wire has no amount: no static span (its reach is its signal's), only the live sweep.
    const float span = w.travels() ? std::fabs(static_cast<float>(w.amount)) : 0.0f;
    bool found = false;
    for (size_t i = 0; i < m.ringTargets.size(); ++i)
      if (m.ringTargets[i].device == w.to && m.ringTargets[i].param == w.param) {
        m.ringSpan[i] += span;
        m.ringUnits[i] = std::max(m.ringUnits[i], ringLaw(w));
        found = true;
      }
    if (!found) {
      m.ringTargets.push_back({w.to, w.param});
      m.ringSpan.push_back(span);
      m.ringUnits.push_back(ringLaw(w));
    }
  }
  m.ringSweeps.resize(m.ringTargets.size());
  // The controls a macro drives (a Travel wire from a macro's value block): found once here, resolved below.
  std::vector<std::pair<doc::DeviceId, std::string>> mappedCtl;
  for (const doc::Wire& w : doc::wiresOf(p))
    if (!w.toMixer() && !w.param.empty() && w.travels() && !w.fromNoteSource())
      if (const doc::Device* src = doc::findDevice(p, w.from).device;
          src != nullptr && doc::isValueDevice(*src) && src->setting(doc::kMacroSetting) != nullptr)
        mappedCtl.emplace_back(w.to, w.param);
  // GU.13: every control of every frame's panels (a leaf's, a group's members') — one ring law for all.
  std::vector<panel::PanelHost*> moved;  // the panels whose rings changed (each repaints those controls only)
  for (auto& [id, f] : m.all) {
    device_chain::forEachPanelControl(*f, [&](const device_chain::PanelControl& c) {
      if (f->isRack() && c.device == f->id()) return false;  // a rack's macros: no ring
      panel::PanelTarget& t = c.host->panelFace()->target();
      ui::ModulationRing ring;
      bool modulated = false, isMapped = false;
      for (const ModTarget& rt : m.ringTargets) modulated = modulated || (rt.device == c.device && rt.param == c.param);
      for (const auto& [d, prm] : mappedCtl) isMapped = isMapped || (d == c.device && prm == c.param);
      if (modulated || isMapped) {
        const devices::ParamInfo* infoP = m.paramInfo(c.device, c.param);
        if (infoP == nullptr) return false;
        const devices::ParamInfo info = *infoP;
        // The position shown (T4.8: a macro-mapped control where its macro puts it: the centre the wires add to).
        const float pk = static_cast<float>(std::clamp(t.position(*c.binding), 0.0, 1.0));
        bool mapped = false;
        if (const auto ml = isMapped ? macroMappingOf(p, c.device, c.param) : std::nullopt) {
          const doc::RackMapping& mm = ml->second;
          mapped = true;
          ring.active = true;
          ring.lo = static_cast<float>(std::min(mm.min, mm.max));
          ring.hi = static_cast<float>(std::max(mm.min, mm.max));
          ring.current = pk;
        }
        for (size_t i = 0; i < m.ringTargets.size(); ++i) {
          if (m.ringTargets[i].device != c.device || m.ringTargets[i].param != c.param) continue;
          if (mapped && m.ringSpan[i] <= 0.0f && m.ringUnits[i] == 0) continue;  // (only its mapping: shown above)
          ring.active = true;
          ring.lo = std::clamp(pk - m.ringSpan[i], 0.0f, 1.0f);
          ring.hi = std::clamp(pk + m.ringSpan[i], 0.0f, 1.0f);
          ring.current = -1.0f;
          const ModSweep& sw = m.ringSweeps[i];
          if (sw.known && (sw.up > 0.0f || sw.down > 0.0f)) {
            if (m.ringUnits[i] != 0) {  // TX.patching / GU.1: an added wire's sweep in its tap's domain (sweptPosition)
              const doc::ModulationTap::Domain dom = ringDomain(m.ringUnits[i], info);
              ring.liveLo = std::clamp(static_cast<float>(devices::sweptPosition(info, pk, -sw.down, dom)), 0.0f, 1.0f);
              ring.liveHi = std::clamp(static_cast<float>(devices::sweptPosition(info, pk, sw.up, dom)), 0.0f, 1.0f);
            } else {
              ring.liveLo = std::clamp(pk - sw.down, 0.0f, 1.0f);
              ring.liveHi = std::clamp(pk + sw.up, 0.0f, 1.0f);
            }
            ring.current = 0.5f * (ring.liveLo + ring.liveHi);
          }
        }
      }
      if (t.setModulation(*c.binding, ring) && std::find(moved.begin(), moved.end(), c.host) == moved.end()) moved.push_back(c.host);
      return false;
    });
  }
  for (panel::PanelHost* h : moved) h->notifyChanged();
  // Poll while a modulated knob is shown and the source runs (and once after it stops: the rings fall still).
  ModulationSource* src = ctx_.modulationSource();
  const bool want = src != nullptr && !m.ringTargets.empty() && (src->modulationLive() || m.ringsLit);
  if (want && m.ringTimer == 0) m.ringTimer = requestTimer(1.0 / 30.0);
  if (!want && m.ringTimer != 0) {
    cancelTimer(m.ringTimer);
    m.ringTimer = 0;
  }
}

void DeviceChainView::onFrame(double) { impl_->retiredFrames.clear(); }  // no handler of a retired frame runs now

void DeviceChainView::onTimer(uint32_t id) {
  if (impl_->follow.fired(id)) {  // T4.18
    applyAutomation();
    return;
  }
  if (id != impl_->ringTimer) return;
  impl_->ringTimer = 0;
  pollRings();
}

// ---- T4.18: automated knobs ----------------------------------------------------------------------------------------

bool DeviceChainView::automationPolling() const noexcept { return impl_->follow.polling(); }

std::vector<std::pair<std::string, bool>> DeviceChainView::automationMarks(doc::DeviceId device) const {
  std::vector<std::pair<std::string, bool>> out;
  const std::vector<AutomationFollow::Target>& ts = impl_->follow.targets();
  for (const uint32_t i : impl_->autoShown)
    if (ts[i].param.owner == device.value) out.emplace_back(ts[i].name, !ts[i].follows());
  return out;
}

void DeviceChainView::refreshAutomation() {
  Impl& m = *impl_;
  // The positions shown for the old targets go (the controls show their own until applyAutomation shows the envelope).
  for (const uint32_t i : m.autoShown) {
    const AutomationFollow::Target& a = m.follow.targets()[i];
    doc::Binding b;
    if (panel::PanelHost* h = m.hostOf(doc::DeviceId{a.param.owner}, a.name, &b); h != nullptr && h->panelFace()->target().showPosition(b, std::nullopt))
      h->notifyChanged();
  }
  m.follow.clear();
  m.autoShown.clear();
  m.chainAuto = false;
  std::map<FrameBase*, std::vector<FrameBase::Mark>> marks;
  if (const doc::Track* t = m.track()) m.follow.collect(ctx_, *t, false);  // a device's lanes live on its track (the shown one)
  const std::vector<AutomationFollow::Target>& ts = m.follow.targets();
  for (uint32_t i = 0; i < ts.size(); ++i) {
    const AutomationFollow::Target& a = ts[i];
    const doc::DeviceId owner{a.param.owner};
    // GU.9: a chain's volume / pan lane is its Mixer value block's (no frame of its own: the rack's rows show it).
    if (m.frameOf(owner) == nullptr)
      if (const doc::Device* vb = doc::findDevice(*ctx_.doc(), owner).device; vb != nullptr && doc::isValueDevice(*vb))
        m.chainAuto = true;
    FrameBase* f = m.frameFor(owner);  // GU.13: a group member's control is on its group's panel
    if (f == nullptr) continue;
    // A macro-mapped param plays its macro, not its lane (lower.h): its knob is left to the macro, its LED grey.
    marks[f].push_back({owner, a.name, !a.follows()});
    m.autoShown.push_back(i);
  }
  for (auto& [id, f] : m.all) {
    auto it = marks.find(f);
    f->setAutomated(it != marks.end() ? std::move(it->second) : std::vector<FrameBase::Mark>{});
    if (f->isRack()) f->automationChanged();
  }
  applyAutomation();
}

void DeviceChainView::applyAutomation() {
  Impl& m = *impl_;
  const PlayheadSnapshot ph = ctx_.transport().playhead();
  bool live = false;
  for (const uint32_t i : m.autoShown) {
    const AutomationFollow::Target& a = m.follow.targets()[i];
    if (a.mapped) continue;  // left to the macro
    const doc::DeviceId device{a.param.owner};
    doc::Binding b;
    panel::PanelHost* h = m.hostOf(device, a.name, &b);
    if (h == nullptr) continue;
    panel::PanelTarget& t = h->panelFace()->target();
    if (a.overridden) {  // overridden by hand: the knob shows (and plays) its own value until re-enabled
      if (t.showPosition(b, std::nullopt)) h->notifyChanged();
      continue;
    }
    live = true;
    // The envelope at the playhead (Live: an automated knob follows its automation, moving while it plays). GU.1: a
    // device param's lane holds knob positions.
    const doc::Device* d = doc::findDevice(*ctx_.doc(), device).device;
    const devices::ParamInfo* info = m.paramInfo(device, a.name);
    const double own = d != nullptr && info != nullptr ? devices::storedPosition(*info, *d) : t.position(b);
    const double pos = AutomationFollow::valueAt(a, static_cast<double>(ph.tick), own);
    if (t.showPosition(b, pos)) h->notifyChanged();  // repaints that control only when its shown position moved
  }
  if (m.chainAuto || live)  // racks: their chains' volume / pan (refreshAutomation also clears them)
    for (auto& [id, f] : m.all)
      if (f->isRack()) f->automationChanged();
  m.follow.poll(*this, (live || m.chainAuto) && ph.playing);
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
