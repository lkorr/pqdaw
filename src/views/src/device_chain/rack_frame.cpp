// Racks in the Device view (T4.8, SPEC §8.2; Live's rack): pq/views/device_chain/device_chain.h. A rack's frame is, left
// to right: a strip of buttons (show macros / chains / zones, Map, Rand, variations), the macro panel (a T4.6
// DevicePanel over the rack's params: up to 16 macros and the chain selector, with the title bar's activator and fold),
// the chain list (one row per chain: activator, name, solo, volume, pan — the chain mixer), the zone editor (Key /
// Velocity / Chain zones as bars aligned with the rows), then the frames of the chain it shows (devices and nested
// racks, framed like the track's) and a slot that adds to that chain. Every edit is a document command (edit_ops.h),
// one undo step each; drags are store gestures (one step).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>

#include "../common/paint.h"
#include "internal.h"
#include "pq/core/automation_curve.h"
#include "pq/document/lower.h"
#include "pq/graph/parse.h"
#include "pq/ui/controls.h"
#include "pq/ui/overlay.h"
#include "pq/ui/theme.h"
#include "pq/views/circuit/circuit_viewer.h"

namespace pq::views {

using namespace device_chain;

namespace {

constexpr float kStripW = 30.0f;     // the button strip
constexpr float kMacroCell = 56.0f;  // a macro knob's cell (w and h)
constexpr float kListW = 220.0f;     // the chain list
constexpr float kRowH = 22.0f;       // a chain row
constexpr float kZoneW = 230.0f;     // the zone editor
constexpr float kSlotW = 112.0f;     // the add / drop slot after the shown chain
constexpr float kPad = 6.0f;         // T4.6's panel padding

std::string fmtDb(double db) {
  if (db <= doc::kMinVolumeDb) return "-inf dB";
  char b[32];
  std::snprintf(b, sizeof b, "%.1f dB", db);
  return b;
}
std::string fmtPan(double p) {
  const long v = std::lround(p * 50.0);
  if (v == 0) return "C";
  return std::to_string(std::labs(v)) + (v < 0 ? "L" : "R");
}

// The rack's params as a device signature: "macro1" … "macro16" (labels: the macro names) and "select" ("Chain").
devices::DeviceSignature rackSignature(const doc::Device& d) {
  devices::DeviceSignature s;
  s.patch = d.patch;
  for (uint32_t k = 0; k < doc::kMacroCount; ++k) {
    devices::ParamInfo p;
    p.name = doc::macroParamName(k);
    p.label = doc::macroDisplayName(*d.rack, k);
    p.min = 0.0;
    p.max = doc::kMacroMax;
    p.steps = 128;  // shown and dragged in whole steps 0 … 127 (Live's macros); the param itself is continuous
    s.params.push_back(std::move(p));
  }
  devices::ParamInfo sel;
  sel.name = std::string(doc::kSelectParam);
  sel.label = "Chain";
  sel.min = 0.0;
  sel.max = doc::kSelectMax;
  sel.steps = 128;
  s.params.push_back(std::move(sel));
  return s;
}

// The macro panel's layout: the visible macros in columns of four, then the chain selector's knob in a column of its
// own; only the title bar when the macros are hidden.
devices::PanelDesc rackPanel(const doc::Device& d, const devices::DeviceSignature& sig) {
  devices::PanelDesc p;
  p.patch = sig.patch;
  p.title = d.kind == doc::DeviceKind::Instrument ? "Instrument Rack" : "Audio Effect Rack";
  const doc::Rack& r = *d.rack;
  if (!r.showMacros) {
    p.width = 112.0f;
    p.height = 0.0f;
    return p;
  }
  const uint32_t n = std::clamp<uint32_t>(r.visibleMacros, 1, doc::kMacroCount);
  const uint32_t rows = std::min<uint32_t>(4, n), cols = (n + 3) / 4;
  for (uint32_t k = 0; k < n; ++k) {
    devices::ControlDesc c;
    c.kind = devices::ControlKind::Knob;
    c.param = c.id = doc::macroParamName(k);
    c.x = static_cast<float>(k / 4) * kMacroCell;
    c.y = static_cast<float>(k % 4) * kMacroCell;
    c.w = kMacroCell - 4.0f;
    c.h = kMacroCell;
    c.info = "A macro: drives every param mapped to it (Map, then the macro, then a knob).";
    p.controls.push_back(std::move(c));
  }
  devices::ControlDesc c;
  c.kind = devices::ControlKind::Knob;
  c.param = c.id = std::string(doc::kSelectParam);
  c.x = static_cast<float>(cols) * kMacroCell;
  c.y = 0.0f;
  c.w = kMacroCell - 4.0f;
  c.h = kMacroCell;
  c.info = "The chain selector: chains play where their Chain zone covers it (with their fades).";
  p.controls.push_back(std::move(c));
  p.width = static_cast<float>(cols + 1) * kMacroCell;
  p.height = static_cast<float>(rows) * kMacroCell;
  return p;
}

// The macro panel's target: the rack's params (macros, selector), its activator.
class RackTarget final : public devices::DeviceTarget {
public:
  RackTarget(DeviceChainView::Impl& impl, doc::DeviceId id) : impl_(impl), id_(id) {}
  void paramGestureBegin(std::string_view) override {
    if (!impl_.ctx().store().inGesture()) {
      (void)impl_.ctx().store().beginGesture("Device Parameter");
      own_ = true;
    }
  }
  void setParam(std::string_view param, double value) override {
    impl_.live(doc::ops::setDeviceParam(impl_.doc(), id_, std::string(param), std::clamp(value, 0.0, doc::kMacroMax)));
  }
  void paramGestureEnd(std::string_view) override {
    if (own_ && impl_.ctx().store().inGesture()) (void)impl_.ctx().store().commitGesture();
    own_ = false;
  }
  void setConst(std::string_view, double) override {}
  void setActive(bool on) override {
    impl_.run(doc::ops::setDevicesActive(impl_.doc(), std::vector<doc::DeviceId>{id_}, on), false);
  }

private:
  DeviceChainView::Impl& impl_;
  doc::DeviceId id_;
  bool own_ = false;
};

class RackFrame;

// One chain row: activator, name, solo, volume, pan (the chain mixer).
class ChainRow final : public ui::Widget {
public:
  ChainRow(RackFrame& rack, DeviceChainView::Impl& impl, const doc::Chain& c);
  doc::ChainId chain() const noexcept { return chain_; }
  void set(const doc::Chain& c, bool shown);
  void arrange(ui::LayoutPass& pass) override;
  void paint(ui::PaintContext& ctx) override;
  bool onPointer(const ui::PointerEvent& e) override;
  void onFrame(double now) override;
  ui::InfoText info() const override {
    return {"Chain", "Click to show the chain's devices (Up / Down: the previous / next chain); double-click the name to "
                     "rename it. The square switches the chain on or off (off: it costs nothing), S solos it, then its "
                     "volume and pan. Drop a device here to add it to the chain. Right-click for more."};
  }
  // T4.16c: rename the chain in place (double-click its name, or its menu's Rename): Enter commits, Esc cancels.
  void beginRename();
  bool renaming() const noexcept { return rename_ != nullptr && !renameDone_; }

private:
  void endRename(bool commit);
  RackFrame& rack_;
  DeviceChainView::Impl& impl_;
  doc::ChainId chain_;
  std::string name_;
  bool shown_ = false;
  ui::TextEdit* rename_ = nullptr;
  bool renameDone_ = false;
  ui::Toggle* on_ = nullptr;
  ui::Toggle* solo_ = nullptr;
  ui::DragBox* vol_ = nullptr;
  ui::DragBox* pan_ = nullptr;
};

// The chain list: a header ("Chains", +) and the rows.
class ChainList final : public ui::Widget {
public:
  ChainList(RackFrame& rack, DeviceChainView::Impl& impl) : Widget("chains"), rack_(rack), impl_(impl) {
    setLayout(ui::LayoutKind::Custom);
  }
  std::vector<ChainRow*> rows;
  void arrange(ui::LayoutPass& pass) override {
    const ui::IRect r = rect();
    int32_t y = r.y + pass.px(kTitleH);
    for (ChainRow* row : rows) {
      pass.place(*row, {r.x, y, r.w, pass.px(kRowH)});
      y += pass.px(kRowH);
    }
  }
  ui::IRect addRect() const {
    const ui::IRect r = rect();
    const int32_t th = tree() ? tree()->uiScale().px(kTitleH) : 20;
    return {r.right() - th, r.y, th, th};
  }
  // Below the rows: the area a dropped device makes a new chain from.
  ui::IRect emptyArea() const {
    const ui::IRect r = rect();
    const float s = tree() ? tree()->scale() : 1.0f;
    const int32_t top = r.y + static_cast<int32_t>(std::lround((kTitleH + kRowH * static_cast<float>(rows.size())) * s));
    return {r.x, top, r.w, std::max(0, r.bottom() - top)};
  }
  void paint(ui::PaintContext& ctx) override;
  bool onPointer(const ui::PointerEvent& e) override;
  ui::InfoText info() const override {
    return {"Chains", "The rack's parallel chains. + adds one; dropping a device below the rows makes a new chain with it. "
                      "Up / Down show the previous / next chain."};
  }
  // T4.16c: + is a button; the title band is the rack's (it moves the rack).
  ui::Cursor cursorAt(ui::Vec2 p) const override { return addRect().contains(p) ? ui::Cursor::Pointer : ui::Cursor::Inherit; }

private:
  RackFrame& rack_;
  DeviceChainView::Impl& impl_;
};

// The zone editor: per chain a bar over the zone's value range (Key: pitch, Velocity, Chain: the selector), the fades
// as ramps; drag an edge (low / high), the body (moves the zone), or in the upper part a fade edge. The Chain tab's
// header holds the selector's marker (drag it).
class ZoneEditor final : public ui::Widget {
public:
  ZoneEditor(RackFrame& rack, DeviceChainView::Impl& impl) : Widget("zones"), rack_(rack), impl_(impl) {}
  void paint(ui::PaintContext& ctx) override;
  bool onPointer(const ui::PointerEvent& e) override;
  ui::InfoText info() const override {
    return {"Zones", "Where each chain plays: drag a bar's ends, its body, or (in its upper part) its fade edges. Key "
                     "zones are pitch ranges (Hz, or degrees of the track's tuning), Velocity zones 0 … 127, Chain "
                     "zones the chain selector's range (drag the marker above them)."};
  }
  double toValue(float x) const;
  float toX(double v) const;
  // T4.16c: the ends and fade edges resize (↔), the body moves, the tabs and the key unit are buttons; the Chain tab's
  // header drags the selector's marker (↔).
  ui::Cursor cursorAt(ui::Vec2 p) const override;

private:
  enum class Grab : uint8_t { None, Lo, Hi, FadeLo, FadeHi, Move, Selector, Tab, Unit };
  // What a press at `p` grabs (the row's chain in *row); the one rule for onPointer and cursorAt.
  Grab grabAt(ui::Vec2 p, size_t* row) const;
  RackFrame& rack_;
  DeviceChainView::Impl& impl_;
  Grab grab_ = Grab::None;
  doc::ChainId chain_;
  doc::Zone start_;
  double pressValue_ = 0.0;
};

// After the shown chain: adds a device to it (the device menu), or names what to drop.
class ChainSlot final : public ui::Control {
public:
  ChainSlot(RackFrame& rack, DeviceChainView::Impl& impl) : Control("chain-slot"), rack_(rack), impl_(impl) {
    setInfo("Chain", "Opens the device menu: the device goes at the end of the chain shown (a rack without chains gets "
                     "one). Devices dragged here join that chain.");
    setCursor(ui::Cursor::Pointer);  // T4.16c: opens a menu
  }
  void paint(ui::PaintContext& ctx) override;
  bool onPointer(const ui::PointerEvent& e) override;

private:
  RackFrame& rack_;
  DeviceChainView::Impl& impl_;
};

// The button strip on the rack's left edge (Live's rack strip).
class RackStrip final : public ui::Widget {
public:
  enum Button : uint8_t { kMacros, kChains, kZones, kMap, kRand, kVariations, kButtons };
  RackStrip(RackFrame& rack, DeviceChainView::Impl& impl) : Widget("strip"), rack_(rack), impl_(impl) {}
  ui::IRect buttonRect(uint32_t b) const {
    const ui::IRect r = rect();
    const float s = tree() ? tree()->scale() : 1.0f;
    const int32_t w = static_cast<int32_t>(std::lround(26.0f * s)), h = static_cast<int32_t>(std::lround(18.0f * s));
    const int32_t step = static_cast<int32_t>(std::lround(22.0f * s));
    return {r.x + (r.w - w) / 2, r.y + static_cast<int32_t>(std::lround((kTitleH + 4.0f) * s)) + static_cast<int32_t>(b) * step, w, h};
  }
  void paint(ui::PaintContext& ctx) override;
  bool onPointer(const ui::PointerEvent& e) override;
  ui::InfoText info() const override {
    return {"Rack", "M shows the macros, C the chain list, Z the zone editor. Map: click a macro, then a knob inside the "
                    "rack to map it (again to unmap; Esc ends). Rnd randomizes the macros (not the excluded ones). Var: "
                    "store and recall macro variations."};
  }
  ui::Cursor cursorAt(ui::Vec2 p) const override {  // T4.16c: the buttons
    for (uint32_t b = 0; b < kButtons; ++b)
      if (buttonRect(b).contains(p)) return ui::Cursor::Pointer;
    return ui::Cursor::Arrow;  // the strip takes every press (no reorder drag from it)
  }

private:
  RackFrame& rack_;
  DeviceChainView::Impl& impl_;
};

class RackFrame final : public FrameBase {
public:
  RackFrame(DeviceChainView::Impl& impl, ViewContext& ctx, const doc::Device& d)
      : FrameBase("rack[" + std::to_string(d.id.value) + "]", impl, ctx, d.id), target_(impl, d.id) {
    setLayout(ui::LayoutKind::Custom);
    strip_ = &add<RackStrip>(*this, impl);
    list_ = &add<ChainList>(*this, impl);
    zones_ = &add<ZoneEditor>(*this, impl);
    slot_ = &add<ChainSlot>(*this, impl);
    update(d);
  }

  const doc::Rack& rack() const noexcept { return *rack_; }
  bool instrument() const noexcept { return kind_ == doc::DeviceKind::Instrument; }
  doc::ChainId shownChain() const noexcept { return shown_; }
  const doc::Chain* shownChainPtr() const noexcept { return shown_.valid() ? rack_->findChain(shown_) : nullptr; }
  std::optional<doc::ZoneKind> zoneTab() const {
    const auto it = impl_.zoneTab.find(id_.value);
    if (it == impl_.zoneTab.end() || it->second == DeviceChainView::Impl::kZonesHidden) return std::nullopt;
    return static_cast<doc::ZoneKind>(it->second);
  }
  double paramValue(std::string_view name) const {
    for (const doc::DeviceValue& v : params_)
      if (v.name == name) return v.value;
    return 0.0;
  }
  // The selector the chains hear: its knob, or where the macro mapped to it puts it.
  double selectorValue() const {
    for (const doc::MacroMapping& m : rack_->mappings)
      if (m.device == id_ && m.param == doc::kSelectParam) {
        const double pos = m.min + (m.max - m.min) * automationShape(paramValue(doc::macroParamName(m.macro)) / doc::kMacroMax, m.curve);
        return std::clamp(pos, 0.0, 1.0) * doc::kSelectMax;
      }
    return paramValue(doc::kSelectParam);
  }
  ChainList& list() noexcept { return *list_; }
  RackStrip& strip() noexcept { return *strip_; }

  devices::DevicePanel* panel() const noexcept override { return panel_; }
  devices::DeviceTarget* target() noexcept override { return &target_; }
  bool isRack() const noexcept override { return true; }

  void collect(std::map<uint32_t, FrameBase*>& all) override {
    all[id_.value] = this;
    for (FrameBase* f : inner_) f->collect(all);
  }
  void appendShown(std::vector<FrameBase*>& out) override {
    out.push_back(this);
    if (panel_ != nullptr && panel_->folded()) return;  // folded: its devices are not shown
    for (FrameBase* f : inner_) f->appendShown(out);
  }

  float logicalWidth() const override {
    if (panel_ != nullptr && panel_->folded()) return panel_->logicalWidth();
    float w = kStripW + (panel_ != nullptr ? panel_->logicalWidth() : 0.0f);
    if (rack_->showChains) w += kListW + (zoneTab() ? kZoneW : 0.0f);
    for (const FrameBase* f : inner_) w += kFrameGap + f->logicalWidth();
    return w + kFrameGap + kSlotW + kFrameGap;
  }
  float logicalHeight(float available) const override {
    if (panel_ != nullptr && panel_->folded()) return std::min(available, panel_->logicalHeight());
    return available;
  }

  void update(const doc::Device& d) override {
    const bool first = rack_ == nullptr;
    const bool optionsChanged = first || d.rack->macroNames != rack_->macroNames ||
                                d.rack->visibleMacros != rack_->visibleMacros || d.rack->showMacros != rack_->showMacros;
    rack_ = d.rack;
    kind_ = d.kind;
    active_ = d.active;
    params_ = d.params;
    // The macro panel: rebuilt when its layout changed, else its values.
    if (optionsChanged) {
      bool folded = false;
      if (panel_ != nullptr) {
        folded = panel_->folded();
        (void)removeChild(*panel_);
        panel_ = nullptr;
      }
      sig_ = rackSignature(d);
      desc_ = rackPanel(d, sig_);
      std::map<std::string, double, std::less<>> values;
      for (const doc::DeviceValue& v : d.params) values[v.name] = v.value;
      panel_ = &add<devices::DevicePanel>("panel", sig_, desc_, &target_, values);
      panel_->setFolded(folded);
      panel_->onSizeChanged = [this] {
        applyVisibility();
        relayout();
      };
    }
    panel_->setActive(d.active);
    for (const devices::ParamInfo& p : sig_.params) panel_->setParamValue(p.name, paramValue(p.name));
    // The chain rows.
    bool sameRows = list_->rows.size() == rack_->chains.size();
    for (size_t i = 0; sameRows && i < rack_->chains.size(); ++i) sameRows = list_->rows[i]->chain() == rack_->chains[i]->id;
    if (!sameRows) {
      for (ChainRow* r : list_->rows) (void)list_->removeChild(*r);
      list_->rows.clear();
      for (const doc::ChainPtr& c : rack_->chains) list_->rows.push_back(&list_->add<ChainRow>(*this, impl_, *c));
      list_->invalidateLayout();
      list_->markDirty();
    }
    // The chain shown: the one picked (view state), else the first.
    const auto it = impl_.shownChain.find(id_.value);
    shown_ = it != impl_.shownChain.end() && rack_->findChain(it->second) != nullptr ? it->second
             : rack_->chains.empty()                                                    ? doc::ChainId{}
                                                                                         : rack_->chains.front()->id;
    for (size_t i = 0; i < list_->rows.size(); ++i) list_->rows[i]->set(*rack_->chains[i], rack_->chains[i]->id == shown_);
    const bool rebuilt = syncInner();
    const bool shownBefore = list_->visible(), zonesBefore = zones_->visible();
    applyVisibility();
    zones_->markDirty();
    strip_->markDirty();
    // Only a change of what is shown re-lays the chain out; values repaint just their controls (T4.6's rule).
    if (first || optionsChanged || !sameRows || rebuilt || shownBefore != list_->visible() || zonesBefore != zones_->visible())
      relayout();
  }
  void paramsChanged(const doc::Device& d, std::span<const doc::ParamRef>) override { update(d); }

  // Shows another chain (view state: no document change).
  void showChain(doc::ChainId c) {
    impl_.shownChain[id_.value] = c;
    shown_ = c;
    for (size_t i = 0; i < list_->rows.size(); ++i) list_->rows[i]->set(*rack_->chains[i], rack_->chains[i]->id == shown_);
    syncInner();
    impl_.reindex();
    impl_.view.refreshRings();  // new panels: their rings and mapped values
    list_->markDirty();
    relayout();
  }
  // What a fold, the chain list and the zone editor show.
  void applyVisibility() {
    const bool folded = panel_ != nullptr && panel_->folded();
    for (size_t i = 0; i < childCount(); ++i) child(i).setVisible(!folded || &child(i) == panel_);
    if (folded) return;
    list_->setVisible(rack_->showChains);
    zones_->setVisible(rack_->showChains && zoneTab().has_value());
  }
  void relayout() {
    invalidateLayout();
    if (impl_.content != nullptr) impl_.content->invalidateLayout();
    markDirty();
  }

  void arrange(ui::LayoutPass& pass) override {
    const ui::IRect r = rect();
    int32_t x = r.x;
    if (panel_ != nullptr && panel_->folded()) {
      pass.place(*panel_, {r.x, r.y, pass.px(panel_->logicalWidth()), pass.px(panel_->logicalHeight())});
      return;
    }
    pass.place(*strip_, {x, r.y, pass.px(kStripW), r.h});
    x += pass.px(kStripW);
    if (panel_ != nullptr) {
      const int32_t w = pass.px(panel_->logicalWidth());
      pass.place(*panel_, {x, r.y, w, std::min(r.h, pass.px(panel_->logicalHeight()))});
      x += w;
    }
    if (rack_->showChains) {
      pass.place(*list_, {x, r.y, pass.px(kListW), r.h});
      x += pass.px(kListW);
      if (zoneTab()) {
        pass.place(*zones_, {x, r.y, pass.px(kZoneW), r.h});
        x += pass.px(kZoneW);
      }
    }
    for (FrameBase* f : inner_) {
      x += pass.px(kFrameGap);
      const int32_t w = pass.px(f->logicalWidth());
      const float avail = static_cast<float>(r.h) / pass.scale() - kTitleH - kPad;
      pass.place(*f, {x, r.y + pass.px(kTitleH), w, std::min(r.h - pass.px(kTitleH), pass.px(f->logicalHeight(avail)))});
      x += w;
    }
    x += pass.px(kFrameGap);
    pass.place(*slot_, {x, r.y + pass.px(kTitleH), pass.px(kSlotW), std::max(0, r.h - pass.px(kTitleH) - pass.px(kPad))});
    if (macroRename_ != nullptr)  // over the macro's label
      if (const ui::Widget* k = macroKnob(renamingMacro_)) pass.place(*macroRename_, {k->rect().x, k->rect().y, k->rect().w, pass.px(16.0f)});
  }

  void paint(ui::PaintContext& ctx) override {
    const ui::Theme* th = ctx.res.theme;
    if (!th) return;
    const ui::IRect r = rect();
    paint::box(ctx.dl, r, 0.0f, th->device.background, 0);
    // The title band over the chain list and the shown chain (the macro panel draws its own title bar).
    const int32_t tb = ctx.scale.px(kTitleH);
    ctx.dl.addRectFilled(ui::IRect{r.x, r.y, r.w, tb}.toRect(), th->device.titleBar);
    if (const doc::Chain* c = shownChainPtr(); c != nullptr && ctx.res.uiFonts && !(panel_ != nullptr && panel_->folded())) {
      // The chain shown, named above its devices.
      const int32_t x0 = slot_->rect().x - (inner_.empty() ? 0 : (slot_->rect().x - inner_.front()->rect().x));
      const ui::IRect t{x0, r.y, std::max(0, slot_->rect().right() - x0), tb};
      paint::text(ctx, ctx.res.uiFonts->small, t, c->name, active_ ? th->device.titleText : th->device.titleTextOff,
                  ui::TextAlign::Left, ctx.scale.px(6.0f));
    }
  }
  void paintOver(ui::PaintContext& ctx) override {
    if (const ui::Theme* th = ctx.res.theme) paint::box(ctx.dl, rect(), 0.0f, 0, th->device.border);
    FrameBase::paintOver(ctx);
  }

  bool inTitle(ui::Vec2 p) const override {
    // The title band right of the macro panel's activator (the panel's own title bar handles its activator and fold).
    if (!FrameBase::inTitle(p)) return false;
    if (panel_ != nullptr && panel_->rect().contains(p)) {
      const float s = tree() ? tree()->scale() : 1.0f;
      return p.x > static_cast<float>(panel_->rect().x) + 36.0f * s;
    }
    return true;
  }

  bool dropAt(ui::Vec2 p, DropTarget& out) override {
    if (panel_ != nullptr && panel_->folded()) return false;
    const float s = tree() ? tree()->scale() : 1.0f;
    if (rack_->showChains && list_->rect().contains(p)) {
      for (ChainRow* row : list_->rows)
        if (row->rect().contains(p)) {
          const doc::Chain* c = rack_->findChain(row->chain());
          out.kind = DropTarget::Kind::Chain;
          out.chain = row->chain();
          out.index = c != nullptr ? static_cast<uint32_t>(c->devices.size()) : doc::kAppend;
          out.marker = row->rect();
          out.row = true;
          return true;
        }
      const ui::IRect empty = list_->emptyArea();
      if (empty.contains(p)) {
        out.kind = DropTarget::Kind::NewChain;
        out.rack = id_;
        out.marker = {empty.x, empty.y, empty.w, static_cast<int32_t>(std::lround(kRowH * s))};
        out.row = true;
        return true;
      }
      return false;
    }
    // The chain shown: the gap nearest the pointer.
    const doc::Chain* c = shownChainPtr();
    const int32_t from = inner_.empty() ? slot_->rect().x : inner_.front()->rect().x;
    if (c == nullptr || p.x < static_cast<float>(from) - 4.0f * s || p.x > static_cast<float>(slot_->rect().right())) {
      if (c == nullptr && slot_->rect().contains(p)) {  // no chain yet: the slot makes one
        out.kind = DropTarget::Kind::NewChain;
        out.rack = id_;
        out.marker = slot_->rect();
        out.row = true;
        return true;
      }
      return false;
    }
    auto slot = static_cast<uint32_t>(inner_.size());
    for (size_t i = 0; i < inner_.size(); ++i) {
      const ui::IRect fr = inner_[i]->rect();
      if (p.x < static_cast<float>(fr.x) + static_cast<float>(fr.w) * 0.5f) {
        slot = static_cast<uint32_t>(i);
        break;
      }
    }
    // An Instrument Rack's chain keeps its instrument first.
    if (instrument() && !c->devices.empty() && c->devices.front()->kind == doc::DeviceKind::Instrument &&
        c->devices.front()->id != impl_.dragging)
      slot = std::max(slot, 1u);
    out.kind = DropTarget::Kind::Chain;
    out.chain = c->id;
    out.index = slot;
    const int32_t gap = static_cast<int32_t>(std::lround(kFrameGap * s));
    const int32_t x = slot < inner_.size() ? inner_[slot]->rect().x - gap / 2 : slot_->rect().x - gap / 2;
    const int32_t w = std::max(2, static_cast<int32_t>(std::lround(2.0f * s)));
    out.marker = {x - w / 2, slot_->rect().y, w, slot_->rect().h};
    out.row = false;
    return true;
  }

  // The rack's own macro knobs (map mode picks them).
  ui::Widget* macroKnob(uint32_t k) const {
    return panel_ != nullptr && !panel_->folded() ? panel_->controlFor(doc::macroParamName(k)) : nullptr;
  }

  // T4.16c: a right-click on a macro knob offers Rename Macro: a field over the knob's label, Enter commits (one undo
  // step), Esc cancels.
  void contextItems(ui::Vec2 p, std::vector<ui::MenuItem>& items) override {
    for (uint32_t k = 0; k < doc::kMacroCount; ++k) {
      const ui::Widget* w = macroKnob(k);
      if (w == nullptr || !w->visible() || !w->rect().contains(p)) continue;
      DeviceChainView::Impl* im = &impl_;
      const doc::DeviceId rack = id_;
      items.push_back(ui::MenuItem::item("Rename Macro", [im, rack, k] {
        if (auto* f = dynamic_cast<RackFrame*>(im->frameOf(rack))) f->beginMacroRename(k);
      }));
      items.push_back(ui::MenuItem::sep());
      return;
    }
  }
  void beginMacroRename(uint32_t k) {
    const ui::Widget* knob = macroKnob(k);
    if (knob == nullptr || macroRename_ != nullptr) return;
    renamingMacro_ = k;
    macroRename_ = &add<ui::TextEdit>("macro-rename", doc::macroDisplayName(*rack_, k));
    macroRenameDone_ = false;
    macroRename_->buffer().selectAll();
    macroRename_->onCommit = [this](const std::string&) { endMacroRename(true); };
    macroRename_->onCancel = [this] { endMacroRename(false); };
    invalidateArrange();
    markDirty();
    if (ui::UiTree* t = tree()) {
      t->layoutNow();
      t->setFocus(macroRename_, true);
    }
  }
  bool renamingMacro() const noexcept { return macroRename_ != nullptr && !macroRenameDone_; }
  void onFrame(double) override {
    if (macroRename_ != nullptr && macroRenameDone_) {
      (void)removeChild(*macroRename_);
      macroRename_ = nullptr;
      macroRenameDone_ = false;
      markDirty();
    }
  }

private:
  void endMacroRename(bool commit) {
    if (macroRename_ == nullptr || macroRenameDone_) return;
    macroRenameDone_ = true;
    const std::string text = macroRename_->text();
    requestFrame();  // removed at the next frame: this may run inside the field's own callback
    if (ui::UiTree* t = tree(); t != nullptr && t->focus() == macroRename_) t->setFocus(&impl_.view);
    if (commit && !text.empty() && text != doc::macroDisplayName(*rack_, renamingMacro_))
      impl_.run(doc::ops::renameMacro(impl_.doc(), id_, renamingMacro_, text), false);
  }

  // The frames of the chain shown: rebuilt when its devices changed, else updated.
  bool syncInner() {
    const doc::Chain* c = shownChainPtr();
    static const std::vector<doc::DevicePtr> kNone;
    const std::vector<doc::DevicePtr>& devs = c != nullptr ? c->devices : kNone;
    bool same = devs.size() == innerShown_.size();
    for (size_t i = 0; same && i < devs.size(); ++i)
      same = devs[i]->id == innerShown_[i]->id && devs[i]->isRack() == innerShown_[i]->isRack() &&
             inner_[i]->keeps(*devs[i]);
    if (same) {
      for (size_t i = 0; i < devs.size(); ++i)
        if (devs[i] != innerShown_[i]) inner_[i]->update(*devs[i]);
    } else {
      for (FrameBase* f : inner_) (void)removeChild(*f);
      inner_.clear();
      for (const doc::DevicePtr& d : devs) {
        auto f = makeFrame(impl_, ctx_, *d);
        inner_.push_back(f.get());
        addChild(std::move(f));
      }
    }
    innerShown_ = devs;
    return !same;
  }

  RackTarget target_;
  std::shared_ptr<const doc::Rack> rack_;
  doc::DeviceKind kind_ = doc::DeviceKind::AudioEffect;
  bool active_ = true;
  std::vector<doc::DeviceValue> params_;
  devices::DeviceSignature sig_;
  devices::PanelDesc desc_;
  devices::DevicePanel* panel_ = nullptr;
  RackStrip* strip_ = nullptr;
  ChainList* list_ = nullptr;
  ZoneEditor* zones_ = nullptr;
  ChainSlot* slot_ = nullptr;
  doc::ChainId shown_;
  std::vector<FrameBase*> inner_;
  std::vector<doc::DevicePtr> innerShown_;
  ui::TextEdit* macroRename_ = nullptr;  // T4.16c
  uint32_t renamingMacro_ = 0;
  bool macroRenameDone_ = false;
};

// ---- ChainRow ----------------------------------------------------------------------------------------------------

ChainRow::ChainRow(RackFrame& rack, DeviceChainView::Impl& impl, const doc::Chain& c)
    : Widget("chain[" + std::to_string(c.id.value) + "]"), rack_(rack), impl_(impl), chain_(c.id) {
  setLayout(ui::LayoutKind::Custom);
  on_ = &add<ui::Toggle>("on", "", ui::ToggleStyle::Activator, c.active);
  on_->onToggle = [this](bool on) { impl_.run(doc::ops::setChainActive(impl_.doc(), chain_, on), false); };
  solo_ = &add<ui::Toggle>("solo", "S", ui::ToggleStyle::Solo, c.solo);
  solo_->onToggle = [this](bool on) { impl_.run(doc::ops::setChainSolo(impl_.doc(), chain_, on), false); };
  ui::ValueSpec vs;
  vs.min = doc::kMinVolumeDb;
  vs.max = doc::kMaxVolumeDb;
  vs.def = 0.0;
  vs.dragPixels = 150.0;
  vs.decimals = 1;
  vs.unit = "dB";
  vol_ = &add<ui::DragBox>("volume", vs);
  vol_->formatter = [](double v) { return fmtDb(v); };
  vol_->setSpec(vs);  // shows the value through the formatter
  ui::ValueSpec ps;
  ps.min = -1.0;
  ps.max = 1.0;
  ps.def = 0.0;
  ps.step = 0.02;
  ps.dragPixels = 100.0;
  pan_ = &add<ui::DragBox>("pan", ps);
  pan_->formatter = [](double v) { return fmtPan(v); };
  pan_->setSpec(ps);
  auto begin = [this] {
    if (!impl_.ctx().store().inGesture()) (void)impl_.ctx().store().beginGesture("Chain Mixer");
  };
  auto end = [this] {
    if (impl_.ctx().store().inGesture()) (void)impl_.ctx().store().commitGesture();
  };
  vol_->onGestureBegin = begin;
  pan_->onGestureBegin = begin;
  vol_->onGestureEnd = end;
  pan_->onGestureEnd = end;
  vol_->onChange = [this](double v) {
    impl_.live(doc::ops::setChainMixer(impl_.doc(), chain_, static_cast<float>(v), static_cast<float>(pan_->value())));
  };
  pan_->onChange = [this](double v) {
    impl_.live(doc::ops::setChainMixer(impl_.doc(), chain_, static_cast<float>(vol_->value()), static_cast<float>(v)));
  };
  setCursor(ui::Cursor::Pointer);  // T4.16c: a click shows the chain
  set(c, false);
}

void ChainRow::beginRename() {
  if (rename_ != nullptr) return;
  rename_ = &add<ui::TextEdit>("rename", name_);
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

void ChainRow::endRename(bool commit) {
  if (rename_ == nullptr || renameDone_) return;
  renameDone_ = true;
  const std::string text = rename_->text();
  requestFrame();  // removed at the next frame: this may run inside the field's own callback
  if (ui::UiTree* t = tree(); t != nullptr && t->focus() == rename_) t->setFocus(&impl_.view);
  if (commit && !text.empty() && text != name_) impl_.run(doc::ops::renameChain(impl_.doc(), chain_, text), false);
}

void ChainRow::onFrame(double) {
  if (rename_ != nullptr && renameDone_) {
    (void)removeChild(*rename_);
    rename_ = nullptr;
    renameDone_ = false;
    invalidateArrange();
    markDirty();
  }
}

void ChainRow::set(const doc::Chain& c, bool shown) {
  if (name_ != c.name || shown_ != shown) markDirty();
  name_ = c.name;
  shown_ = shown;
  on_->setOn(c.active);
  solo_->setOn(c.solo);
  if (!vol_->inGesture()) vol_->setValue(c.volumeDb);
  if (!pan_->inGesture()) pan_->setValue(c.pan);
}

void ChainRow::arrange(ui::LayoutPass& pass) {
  const ui::IRect r = rect();
  const int32_t pad = pass.px(3.0f), h = r.h - 2 * pad;
  int32_t x = r.x + pad;
  pass.place(*on_, {x, r.y + pad, pass.px(14.0f), h});
  int32_t right = r.right() - pad;
  const int32_t panW = pass.px(34.0f), volW = pass.px(50.0f), soloW = pass.px(16.0f);
  right -= panW;
  pass.place(*pan_, {right, r.y + pad, panW, h});
  right -= volW + pad;
  pass.place(*vol_, {right, r.y + pad, volW, h});
  right -= soloW + pad;
  pass.place(*solo_, {right, r.y + pad, soloW, h});
  if (rename_ != nullptr) {  // over the name
    const int32_t x0 = r.x + pad + pass.px(14.0f) + pass.px(3.0f);
    pass.place(*rename_, {x0, r.y + 1, std::max(0, right - pad - x0), std::max(0, r.h - 2)});
  }
}

void ChainRow::paint(ui::PaintContext& ctx) {
  const ui::Theme* th = ctx.res.theme;
  if (!th) return;
  const ui::IRect r = rect();
  if (shown_) ctx.dl.addRectFilled(r.toRect(), th->selection.fill);
  ctx.dl.addRectFilled(ui::IRect{r.x, r.bottom() - 1, r.w, 1}.toRect(), th->surface.divider);
  if (!ctx.res.uiFonts || rename_ != nullptr) return;  // the rename field draws the name
  const int32_t x0 = on_->rect().right() + ctx.scale.px(5.0f);
  const ui::IRect t{x0, r.y, std::max(0, solo_->rect().x - x0 - ctx.scale.px(3.0f)), r.h};
  paint::text(ctx, ctx.res.uiFonts->small, t, name_, shown_ ? th->selection.text : th->text.primary, ui::TextAlign::Left);
}

bool ChainRow::onPointer(const ui::PointerEvent& e) {
  if (e.type != ui::PointerType::Down) return e.type == ui::PointerType::Up;
  if (e.button == ui::PointerButton::Right) {
    const doc::ChainLoc loc = doc::findChain(impl_.doc(), chain_);
    if (loc.chain == nullptr) return true;
    DeviceChainView::Impl* im = &impl_;
    const doc::ChainId c = chain_;
    const doc::DeviceId rack = loc.rack->id;
    std::vector<ui::MenuItem> items;
    items.push_back(ui::MenuItem::item(loc.chain->active ? "Deactivate Chain" : "Activate Chain",
                                       [im, c, on = loc.chain->active] { im->run(doc::ops::setChainActive(im->doc(), c, !on), false); }));
    items.push_back(ui::MenuItem::item(loc.chain->solo ? "Unsolo" : "Solo",
                                       [im, c, s = loc.chain->solo] { im->run(doc::ops::setChainSolo(im->doc(), c, !s), false); }));
    items.push_back(ui::MenuItem::item("Rename", [im, rack, c] {  // T4.16c (the row is looked up: it may be rebuilt)
      if (auto* f = dynamic_cast<RackFrame*>(im->frameOf(rack)))
        for (ChainRow* row : f->list().rows)
          if (row->chain() == c) row->beginRename();
    }));
    items.push_back(ui::MenuItem::sep());
    items.push_back(ui::MenuItem::item("Duplicate Chain",
                                       [im, c] { im->run(doc::ops::duplicateChain(im->doc(), im->ctx().store().ids(), c), false); }));
    items.push_back(ui::MenuItem::item("Delete Chain",
                                       [im, c] { im->run(doc::ops::deleteChains(im->doc(), std::vector<doc::ChainId>{c}), false); }));
    items.push_back(ui::MenuItem::item("Add Chain",
                                       [im, rack] { im->run(doc::ops::addChain(im->doc(), im->ctx().store().ids(), rack), false); }));
    items.push_back(ui::MenuItem::sep());
    const bool first = loc.index == 0, last = loc.index + 1 == loc.rack->rack->chains.size();
    ui::MenuItem up = ui::MenuItem::item("Move Up", [im, c, i = loc.index] { im->run(doc::ops::moveChain(im->doc(), c, i - 1), false); });
    up.enabled = !first;
    items.push_back(std::move(up));
    ui::MenuItem down = ui::MenuItem::item("Move Down", [im, c, i = loc.index] { im->run(doc::ops::moveChain(im->doc(), c, i + 1), false); });
    down.enabled = !last;
    items.push_back(std::move(down));
    ui::openContextMenu(*this, e.pos, std::move(items));
    return true;
  }
  if (e.button != ui::PointerButton::Left) return false;
  if (e.clickCount == 2) {  // T4.16c: Live renames a chain on a double-click on its name
    beginRename();
    return true;
  }
  rack_.showChain(chain_);
  return true;
}

// ---- ChainList ---------------------------------------------------------------------------------------------------

void ChainList::paint(ui::PaintContext& ctx) {
  const ui::Theme* th = ctx.res.theme;
  if (!th) return;
  const ui::IRect r = rect();
  ctx.dl.addRectFilled(r.toRect(), th->surface.sunken);
  const int32_t tb = ctx.scale.px(kTitleH);
  ctx.dl.addRectFilled(ui::IRect{r.x, r.y, r.w, tb}.toRect(), th->device.titleBar);
  ctx.dl.addRectFilled(ui::IRect{r.x, r.y, 1, r.h}.toRect(), th->device.border);
  if (!ctx.res.uiFonts) return;
  paint::text(ctx, ctx.res.uiFonts->small, {r.x, r.y, r.w, tb}, "Chains", th->device.titleText, ui::TextAlign::Left,
              ctx.scale.px(6.0f));
  const ui::IRect a = addRect();
  paint::text(ctx, ctx.res.uiFonts->bodyStrong, a, "+", th->device.titleText, ui::TextAlign::Center);
  if (rows.empty()) {
    const ui::IRect e = emptyArea();
    paint::text(ctx, ctx.res.uiFonts->small, {e.x, e.y, e.w, ctx.scale.px(2.0f * kRowH)},
                rack_.instrument() ? "Drop an instrument here" : "Drop audio effects here", th->text.secondary,
                ui::TextAlign::Center);
  }
}

bool ChainList::onPointer(const ui::PointerEvent& e) {
  if (e.type != ui::PointerType::Down || e.button != ui::PointerButton::Left) return e.type == ui::PointerType::Up;
  if (addRect().contains(e.pos)) {
    impl_.run(doc::ops::addChain(impl_.doc(), impl_.ctx().store().ids(), rack_.id()), false);
    return true;
  }
  return false;
}

// ---- ZoneEditor --------------------------------------------------------------------------------------------------

// The zone editor's value axis for a zone kind: linear over 0 … 127 (velocity, selector), log 20 Hz … 20 kHz (key zones
// in Hz), linear −48 … 72 degrees (key zones in degrees).
struct Axis {
  bool log = false;
  double lo = 0.0, hi = 127.0;
};
Axis axisOf(doc::ZoneKind k, doc::KeyZoneUnit unit) {
  if (k != doc::ZoneKind::Key) return {};
  return unit == doc::KeyZoneUnit::Hz ? Axis{true, 20.0, 20000.0} : Axis{false, -48.0, 72.0};
}

float ZoneEditor::toX(double v) const {
  const doc::ZoneKind k = rack_.zoneTab().value_or(doc::ZoneKind::Select);
  const Axis a = axisOf(k, rack_.rack().keyUnit);
  const ui::IRect r = rect();
  const float s = tree() ? tree()->scale() : 1.0f;
  const float x0 = static_cast<float>(r.x) + 6.0f * s, x1 = static_cast<float>(r.right()) - 6.0f * s;
  double t = a.log ? (std::log(std::max(v, 1e-9)) - std::log(a.lo)) / (std::log(a.hi) - std::log(a.lo)) : (v - a.lo) / (a.hi - a.lo);
  t = std::clamp(t, 0.0, 1.0);
  return x0 + static_cast<float>(t) * (x1 - x0);
}

double ZoneEditor::toValue(float x) const {
  const doc::ZoneKind k = rack_.zoneTab().value_or(doc::ZoneKind::Select);
  const Axis a = axisOf(k, rack_.rack().keyUnit);
  const ui::IRect r = rect();
  const float s = tree() ? tree()->scale() : 1.0f;
  const float x0 = static_cast<float>(r.x) + 6.0f * s, x1 = static_cast<float>(r.right()) - 6.0f * s;
  const double t = std::clamp(static_cast<double>((x - x0) / std::max(1.0f, x1 - x0)), 0.0, 1.0);
  return a.log ? std::exp(std::log(a.lo) + t * (std::log(a.hi) - std::log(a.lo))) : a.lo + t * (a.hi - a.lo);
}

void ZoneEditor::paint(ui::PaintContext& ctx) {
  const ui::Theme* th = ctx.res.theme;
  if (!th) return;
  const ui::IRect r = rect();
  ctx.dl.addRectFilled(r.toRect(), th->surface.sunken);
  ctx.dl.addRectFilled(ui::IRect{r.x, r.y, 1, r.h}.toRect(), th->device.border);
  const int32_t tb = ctx.scale.px(kTitleH);
  ctx.dl.addRectFilled(ui::IRect{r.x, r.y, r.w, tb}.toRect(), th->device.titleBar);
  const doc::ZoneKind tab = rack_.zoneTab().value_or(doc::ZoneKind::Select);
  // The tabs: Key, Vel, Chain (an Audio Effect Rack: Chain only).
  if (ctx.res.uiFonts) {
    const char* names[3] = {"Key", "Vel", "Chain"};
    const int32_t w = ctx.scale.px(40.0f);
    int32_t x = r.x + ctx.scale.px(4.0f);
    for (uint32_t i = rack_.instrument() ? 0u : 2u; i < 3; ++i) {
      const ui::IRect t{x, r.y + ctx.scale.px(3.0f), w, tb - ctx.scale.px(6.0f)};
      const bool on = static_cast<uint32_t>(tab) == i;
      paint::box(ctx.dl, t, ctx.scale.exact(2.0f), on ? th->accent.primary : th->control.track, 0);
      paint::text(ctx, ctx.res.uiFonts->small, t, names[i], on ? th->text.onAccent : th->text.secondary, ui::TextAlign::Center);
      x += w + ctx.scale.px(3.0f);
    }
    if (tab == doc::ZoneKind::Key)
      paint::text(ctx, ctx.res.uiFonts->small, {x, r.y, r.right() - x - ctx.scale.px(4.0f), tb},
                  rack_.rack().keyUnit == doc::KeyZoneUnit::Hz ? "Hz" : "Degrees", th->text.secondary, ui::TextAlign::Right);
  }
  const ui::Rgba zoneFill = ui::withAlpha(th->accent.primary, 110), solidFill = ui::withAlpha(th->accent.primary, 200);
  const doc::Rack& rack = rack_.rack();
  for (size_t i = 0; i < rack.chains.size(); ++i) {
    const doc::Chain& c = *rack.chains[i];
    const int32_t y = r.y + tb + static_cast<int32_t>(i) * ctx.scale.px(kRowH);
    const int32_t h = ctx.scale.px(kRowH);
    if (y + h > r.bottom()) break;
    ctx.dl.addRectFilled(ui::IRect{r.x, y + h - 1, r.w, 1}.toRect(), th->surface.divider);
    const doc::Zone& z = c.zone(tab);
    const float xl = toX(z.lo), xfl = toX(z.fadeLo), xfh = toX(z.fadeHi), xh = toX(z.hi);
    const float y0 = static_cast<float>(y) + 0.3f * static_cast<float>(h), y1 = static_cast<float>(y + h) - 3.0f * ctx.scale.exact(1.0f);
    // The fades as ramps, the full-gain part solid.
    if (xfl > xl) ctx.dl.addTriangleFilled({xl, y1}, {xfl, y0}, {xfl, y1}, zoneFill);
    if (xh > xfh) ctx.dl.addTriangleFilled({xfh, y0}, {xh, y1}, {xfh, y1}, zoneFill);
    if (xfh > xfl) ctx.dl.addRectFilled({xfl, y0, xfh, y1}, c.active ? solidFill : zoneFill);
    else ctx.dl.addRectFilled({xfl - 1.0f, y0, xfl + 1.0f, y1}, solidFill);
  }
  // The selector's marker (Chain tab), above the bars.
  if (tab == doc::ZoneKind::Select) {
    const float x = toX(rack_.selectorValue());
    const float yb = static_cast<float>(r.y + tb);
    const float s = ctx.scale.exact(4.0f);
    ctx.dl.addTriangleFilled({x - s, yb - s}, {x + s, yb - s}, {x, yb}, th->accent.focus);
    const int32_t rows = static_cast<int32_t>(rack.chains.size());
    ctx.dl.addRectFilled({x - 0.5f, yb, x + 0.5f, yb + static_cast<float>(rows * ctx.scale.px(kRowH))}, ui::withAlpha(th->accent.focus, 160));
  }
}

ZoneEditor::Grab ZoneEditor::grabAt(ui::Vec2 p, size_t* rowOut) const {
  const ui::IRect r = rect();
  const float s = tree() ? tree()->scale() : 1.0f;
  const int32_t tb = static_cast<int32_t>(std::lround(kTitleH * s)), rh = static_cast<int32_t>(std::lround(kRowH * s));
  const doc::ZoneKind tab = rack_.zoneTab().value_or(doc::ZoneKind::Select);
  if (!r.contains(p)) return Grab::None;
  if (p.y < static_cast<float>(r.y + tb)) {
    // The tabs, the key unit, or the selector's marker.
    const float x = p.x - static_cast<float>(r.x) - 4.0f * s;
    const auto i = static_cast<int32_t>(std::floor(x / (43.0f * s))) + (rack_.instrument() ? 0 : 2);
    if (x >= 0.0f && i >= 0 && i < 3 && (rack_.instrument() || i == 2)) return Grab::Tab;
    if (tab == doc::ZoneKind::Key && p.x > static_cast<float>(r.right()) - 60.0f * s) return Grab::Unit;
    return tab == doc::ZoneKind::Select ? Grab::Selector : Grab::None;
  }
  const auto row = static_cast<size_t>((p.y - static_cast<float>(r.y + tb)) / static_cast<float>(rh));
  if (row >= rack_.rack().chains.size()) return Grab::None;
  if (rowOut != nullptr) *rowOut = row;
  const doc::Zone& z = rack_.rack().chains[row]->zone(tab);
  const float slop = 4.0f * s;
  const float yInRow = p.y - static_cast<float>(r.y + tb) - static_cast<float>(row) * static_cast<float>(rh);
  const bool upper = yInRow < 0.4f * static_cast<float>(rh);
  if (upper && std::fabs(p.x - toX(z.fadeLo)) < slop) return Grab::FadeLo;
  if (upper && std::fabs(p.x - toX(z.fadeHi)) < slop) return Grab::FadeHi;
  if (std::fabs(p.x - toX(z.lo)) < slop) return Grab::Lo;
  if (std::fabs(p.x - toX(z.hi)) < slop) return Grab::Hi;
  if (p.x > toX(z.lo) && p.x < toX(z.hi)) return Grab::Move;
  return Grab::None;
}

ui::Cursor ZoneEditor::cursorAt(ui::Vec2 p) const {
  switch (grab_ != Grab::None ? grab_ : grabAt(p, nullptr)) {
    case Grab::Lo:
    case Grab::Hi:
    case Grab::FadeLo:
    case Grab::FadeHi:
    case Grab::Selector: return ui::Cursor::ResizeEW;
    case Grab::Move: return ui::Cursor::Move;
    case Grab::Tab:
    case Grab::Unit: return ui::Cursor::Pointer;
    case Grab::None: break;
  }
  return ui::Cursor::Arrow;
}

bool ZoneEditor::onPointer(const ui::PointerEvent& e) {
  const ui::IRect r = rect();
  const float s = tree() ? tree()->scale() : 1.0f;
  const doc::ZoneKind tab = rack_.zoneTab().value_or(doc::ZoneKind::Select);
  doc::DocumentStore& store = impl_.ctx().store();
  if (e.type == ui::PointerType::Down && e.button == ui::PointerButton::Left) {
    size_t row = 0;
    const Grab g = grabAt(e.pos, &row);
    if (g == Grab::Tab) {
      const float x = e.pos.x - static_cast<float>(r.x) - 4.0f * s;
      const auto i = static_cast<int32_t>(std::floor(x / (43.0f * s))) + (rack_.instrument() ? 0 : 2);
      impl_.view.showZones(rack_.id(), static_cast<doc::ZoneKind>(i));
      return true;
    }
    if (g == Grab::Unit) {
      impl_.run(doc::ops::setKeyZoneUnit(impl_.doc(), rack_.id(), rack_.rack().keyUnit == doc::KeyZoneUnit::Hz
                                                                        ? doc::KeyZoneUnit::Degree
                                                                        : doc::KeyZoneUnit::Hz),
                false);
      return true;
    }
    if (g == Grab::Selector) {
      grab_ = Grab::Selector;
      (void)store.beginGesture("Device Parameter");
      impl_.live(doc::ops::setDeviceParam(impl_.doc(), rack_.id(), std::string(doc::kSelectParam), std::round(toValue(e.pos.x))));
      return true;
    }
    grab_ = g;
    if (grab_ == Grab::None) return true;
    const doc::Chain& c = *rack_.rack().chains[row];
    const doc::Zone& z = c.zone(tab);
    chain_ = c.id;
    start_ = z;
    pressValue_ = toValue(e.pos.x);
    (void)store.beginGesture("Chain Zone");
    return true;
  }
  if (e.type == ui::PointerType::Move && grab_ != Grab::None) {
    const double v = toValue(e.pos.x);
    if (grab_ == Grab::Selector) {
      impl_.live(doc::ops::setDeviceParam(impl_.doc(), rack_.id(), std::string(doc::kSelectParam), std::round(v)));
      return true;
    }
    const doc::ZoneRange range = doc::zoneRange(tab, rack_.rack().keyUnit);
    const Axis a = axisOf(tab, rack_.rack().keyUnit);
    auto clampV = [&](double x) { return std::clamp(x, range.min, range.max); };
    auto snap = [&](double x) { return a.log ? x : std::round(x); };  // velocity / selector / degrees: whole steps
    doc::Zone z = start_;
    switch (grab_) {
      case Grab::Lo:  // the end pushes its fade along (Live)
        z.lo = std::min(clampV(snap(v)), z.hi);
        z.fadeLo = std::clamp(z.fadeLo, z.lo, z.hi);
        z.fadeHi = std::max(z.fadeHi, z.fadeLo);
        break;
      case Grab::Hi:
        z.hi = std::max(clampV(snap(v)), z.lo);
        z.fadeHi = std::clamp(z.fadeHi, z.lo, z.hi);
        z.fadeLo = std::min(z.fadeLo, z.fadeHi);
        break;
      case Grab::FadeLo: z.fadeLo = std::clamp(snap(v), z.lo, z.fadeHi); break;
      case Grab::FadeHi: z.fadeHi = std::clamp(snap(v), z.fadeLo, z.hi); break;
      case Grab::Move: {
        // Moves the zone keeping its shape (ratios on the log axis).
        if (a.log) {
          double k = v / std::max(pressValue_, 1e-9);
          k = std::clamp(k, range.min / std::max(start_.lo, 1e-9), range.max / std::max(start_.hi, 1e-9));
          z = {start_.lo * k, start_.fadeLo * k, start_.fadeHi * k, start_.hi * k};
        } else {
          double d = snap(v - pressValue_);
          d = std::clamp(d, range.min - start_.lo, range.max - start_.hi);
          z = {start_.lo + d, start_.fadeLo + d, start_.fadeHi + d, start_.hi + d};
        }
        break;
      }
      default: break;
    }
    if (doc::zoneValid(z, tab, rack_.rack().keyUnit)) impl_.live(doc::ops::setChainZone(impl_.doc(), chain_, tab, z));
    return true;
  }
  if (e.type == ui::PointerType::Up && grab_ != Grab::None) {
    grab_ = Grab::None;
    if (store.inGesture()) (void)store.commitGesture();
    return true;
  }
  return false;
}

// ---- ChainSlot, RackStrip -------------------------------------------------------------------------------------------

void ChainSlot::paint(ui::PaintContext& ctx) {
  const ui::Theme* t = ctx.res.theme;
  if (!t) return;
  const ui::IRect r = rect();
  paint::box(ctx.dl, r, ctx.scale.exact(3.0f), visualState().hover ? t->surface.raised : t->surface.sunken, t->surface.divider);
  if (!ctx.res.uiFonts) return;
  const char* hint = rack_.shownChainPtr() != nullptr ? "Add to Chain"
                     : rack_.instrument()             ? "Drop an Instrument"
                                                      : "Drop Audio Effects";
  const int32_t plus = ctx.scale.px(24.0f);
  paint::text(ctx, ctx.res.uiFonts->title, {r.x, r.y + r.h / 2 - plus, r.w, plus}, "+", t->text.secondary, ui::TextAlign::Center);
  paint::text(ctx, ctx.res.uiFonts->small, {r.x, r.y + r.h / 2, r.w, ctx.scale.px(18.0f)}, hint, t->text.secondary,
              ui::TextAlign::Center, ctx.scale.px(4.0f), ctx.scale.px(4.0f));
}

bool ChainSlot::onPointer(const ui::PointerEvent& e) {
  if (e.type != ui::PointerType::Up || e.button != ui::PointerButton::Left || !rect().contains(e.pos))
    return e.type == ui::PointerType::Down && e.button == ui::PointerButton::Left;
  const ui::Vec2 at{static_cast<float>(rect().x), static_cast<float>(rect().bottom())};
  if (const doc::Chain* c = rack_.shownChainPtr()) {
    ui::openContextMenu(*this, at, impl_.deviceMenu({}, c->id, doc::kAppend, rack_.instrument()));
    return true;
  }
  // No chain yet: a device picked makes the first chain with it.
  std::vector<ui::MenuItem> items;
  const doc::DeviceCatalog* cat = impl_.catalog();
  DeviceChainView::Impl* im = &impl_;
  const doc::DeviceId rack = rack_.id();
  if (cat != nullptr)
    for (const doc::DeviceInfo& d : cat->devices()) {
      ui::MenuItem it = ui::MenuItem::item(d.name, [im, rack, patch = d.patch, kind = d.kind] {
        im->run(doc::ops::addChainWithDevice(im->doc(), im->ctx().store().ids(), rack, patch, kind));
      });
      it.enabled = d.kind == doc::DeviceKind::AudioEffect || rack_.instrument();
      items.push_back(std::move(it));
    }
  // T4.16c: the racks too (a nested rack), as the other device menus list them.
  if (!items.empty()) items.push_back(ui::MenuItem::sep());
  for (const bool instrumentRack : {true, false}) {
    ui::MenuItem it = ui::MenuItem::item(instrumentRack ? "Instrument Rack" : "Audio Effect Rack", [im, rack, instrumentRack] {
      im->run(doc::ops::addChainWithDevice(im->doc(), im->ctx().store().ids(), rack,
                                           std::string(instrumentRack ? doc::kInstrumentRack : doc::kAudioEffectRack),
                                           instrumentRack ? doc::DeviceKind::Instrument : doc::DeviceKind::AudioEffect));
    });
    it.enabled = !instrumentRack || rack_.instrument();
    items.push_back(std::move(it));
  }
  ui::openContextMenu(*this, at, std::move(items));
  return true;
}

void RackStrip::paint(ui::PaintContext& ctx) {
  const ui::Theme* th = ctx.res.theme;
  if (!th) return;
  const ui::IRect r = rect();
  ctx.dl.addRectFilled(r.toRect(), th->device.titleBar);
  if (!ctx.res.uiFonts) return;
  const doc::Rack& rk = rack_.rack();
  const bool on[kButtons] = {rk.showMacros, rk.showChains, rack_.zoneTab().has_value(), impl_.mapRack == rack_.id(), false,
                             !rk.variations.empty()};
  const char* text[kButtons] = {"M", "C", "Z", "Map", "Rnd", "Var"};
  for (uint32_t b = 0; b < kButtons; ++b) {
    const ui::IRect br = buttonRect(b);
    paint::box(ctx.dl, br, ctx.scale.exact(2.0f), on[b] ? (b == kMap ? th->circuit.wireBool : th->accent.primary) : th->control.track, 0);
    paint::text(ctx, ctx.res.uiFonts->small, br, text[b], on[b] ? th->text.onAccent : th->text.secondary, ui::TextAlign::Center);
  }
}

bool RackStrip::onPointer(const ui::PointerEvent& e) {
  if (e.type != ui::PointerType::Down || e.button != ui::PointerButton::Left) return e.type == ui::PointerType::Up;
  const doc::Rack& rk = rack_.rack();
  const doc::DeviceId id = rack_.id();
  for (uint32_t b = 0; b < kButtons; ++b) {
    if (!buttonRect(b).contains(e.pos)) continue;
    switch (b) {
      case kMacros: impl_.run(doc::ops::setRackShow(impl_.doc(), id, rk.showChains, !rk.showMacros), false); break;
      case kChains: impl_.run(doc::ops::setRackShow(impl_.doc(), id, !rk.showChains, rk.showMacros), false); break;
      case kZones:
        impl_.view.showZones(id, rack_.zoneTab() ? std::nullopt
                                                 : std::optional<doc::ZoneKind>(rack_.instrument() ? doc::ZoneKind::Key
                                                                                                   : doc::ZoneKind::Select));
        break;
      case kMap: impl_.setMapMode(impl_.mapRack == id ? doc::DeviceId{} : id); break;
      case kRand: impl_.run(doc::ops::randomizeMacros(impl_.doc(), id, static_cast<uint64_t>(e.pos.x * 7919.0f) ^ id.value), false); break;
      case kVariations: {
        DeviceChainView::Impl* im = &impl_;
        std::vector<ui::MenuItem> items;
        items.push_back(ui::MenuItem::item("Store Variation", [im, id] { im->run(doc::ops::storeVariation(im->doc(), id), false); }));
        if (!rk.variations.empty()) items.push_back(ui::MenuItem::sep());
        for (uint32_t i = 0; i < rk.variations.size(); ++i) {
          const std::string& n = rk.variations[i].name;
          items.push_back(ui::MenuItem::item("Recall " + n, [im, id, i] { im->run(doc::ops::recallVariation(im->doc(), id, i), false); }));
          items.push_back(ui::MenuItem::sub("   " + n, {ui::MenuItem::item("Overwrite", [im, id, i] {
                                                          im->run(doc::ops::storeVariation(im->doc(), id, {}, i), false);
                                                        }),
                                                        ui::MenuItem::item("Delete", [im, id, i] {
                                                          im->run(doc::ops::deleteVariation(im->doc(), id, i), false);
                                                        })}));
        }
        items.push_back(ui::MenuItem::sep());
        std::vector<ui::MenuItem> excl;
        for (uint32_t k = 0; k < rk.visibleMacros; ++k) {
          const bool x = ((rk.randomizeExclude >> k) & 1u) != 0;
          ui::MenuItem it = ui::MenuItem::item(doc::macroDisplayName(rk, k), [im, id, k, x] {
            im->run(doc::ops::setMacroExcluded(im->doc(), id, k, !x), false);
          });
          it.checked = x;
          excl.push_back(std::move(it));
        }
        items.push_back(ui::MenuItem::sub("Exclude from Rand", std::move(excl)));
        std::vector<ui::MenuItem> count;
        for (uint32_t n : {1u, 2u, 4u, 8u, 12u, 16u}) {
          ui::MenuItem it = ui::MenuItem::item(std::to_string(n), [im, id, n] { im->run(doc::ops::setVisibleMacros(im->doc(), id, n), false); });
          it.checked = rk.visibleMacros == n;
          count.push_back(std::move(it));
        }
        items.push_back(ui::MenuItem::sub("Macros Shown", std::move(count)));
        if (rack_.instrument()) {
          ui::MenuItem pv = ui::MenuItem::item("Per-Voice Chains", [im, id, v = rk.voiceChains] {
            im->run(doc::ops::setVoiceChains(im->doc(), id, !v), false);
          });
          pv.checked = rk.voiceChains;
          items.push_back(std::move(pv));
        }
        ui::openContextMenu(*this, {static_cast<float>(buttonRect(b).right()), static_cast<float>(buttonRect(b).y)}, std::move(items));
        break;
      }
      default: break;
    }
    markDirty();
    return true;
  }
  return true;
}

// ---- Map mode -----------------------------------------------------------------------------------------------------

// Over the chain while a rack's Map is on: tints the knobs a macro of that rack may drive (mapped ones stronger), rings
// the picked macro; clicks pick a macro or map / unmap a knob. Wheel events pass through (the chain still scrolls).
class MapOverlay final : public ui::Widget {
public:
  explicit MapOverlay(DeviceChainView::Impl& impl) : Widget("map-overlay"), impl_(impl) { setHittable(true); }

  struct Target {
    doc::DeviceId device;
    std::string_view param;  // the panel signature's (alive while the visit runs)
    ui::IRect rect;
    int32_t macro = -1;  // a macro knob of the rack in map mode (a pick, not a target)
  };
  // Visits the targets (no allocation: T4.16c's cursorAt asks on every pointer move); `f` returns true to stop.
  template <class F>
  void forEachTarget(F&& f) const {
    if (impl_.frameOf(impl_.mapRack) == nullptr) return;
    const doc::DeviceLoc rl = doc::findDevice(impl_.doc(), impl_.mapRack);
    if (rl.device == nullptr || !rl.device->rack) return;
    for (const auto& [id, fr] : impl_.all) {
      const doc::DeviceId dev{id};
      const bool self = dev == impl_.mapRack;
      if (!self && !doc::deviceContains(*rl.device, dev)) continue;
      const devices::DevicePanel* panel = fr->panel();
      if (panel == nullptr || panel->folded()) continue;
      for (const devices::ParamInfo& p : panel->signature().params) {
        if (p.isBool) continue;
        const ui::Widget* w = panel->controlFor(p.name);
        if (w == nullptr || !w->visible() || w->rect().empty()) continue;
        Target t{dev, p.name, w->rect(), -1};
        if (self) {
          const std::optional<uint32_t> m = doc::macroIndex(p.name);
          if (m) t.macro = static_cast<int32_t>(*m);
          else if (p.name != doc::kSelectParam) continue;
        }
        if (f(t)) return;
      }
    }
  }
  bool overMapButton(ui::Vec2 p) const {
    if (auto* rf = impl_.frameOf(impl_.mapRack); rf != nullptr && rf->isRack())
      for (size_t i = 0; i < rf->childCount(); ++i)
        if (auto* strip = dynamic_cast<const RackStrip*>(&rf->child(i)); strip != nullptr && strip->buttonRect(RackStrip::kMap).contains(p))
          return true;
    return false;
  }
  // T4.16c: a macro (pick) or a knob it can map, and the Map button: Pointer; the rest of the chain does nothing now.
  ui::Cursor cursorAt(ui::Vec2 p) const override {
    if (overMapButton(p)) return ui::Cursor::Pointer;
    bool hit = false;
    forEachTarget([&](const Target& t) { return hit = t.rect.contains(p); });
    return hit ? ui::Cursor::Pointer : ui::Cursor::Arrow;
  }

  void paint(ui::PaintContext& ctx) override {
    const ui::Theme* th = ctx.res.theme;
    if (!th) return;
    forEachTarget([&](const Target& t) {
      paintTarget(ctx, *th, t);
      return false;
    });
  }
  void paintTarget(ui::PaintContext& ctx, const ui::Theme& th, const Target& t) const {
    if (t.macro >= 0) {
      const bool picked = t.macro == impl_.mapMacro;
      paint::box(ctx.dl, t.rect, 0.0f, picked ? ui::withAlpha(th.circuit.wireBool, 90) : t.rect == hover_ ? ui::withAlpha(th.circuit.wireBool, 45) : 0,
                 picked || t.rect == hover_ ? th.accent.focus : ui::withAlpha(th.circuit.wireBool, 200));
      return;
    }
    const doc::MappingLoc m = doc::mappingOf(impl_.doc(), t.device, t.param);
    const bool ours = m.mapping != nullptr && m.rack->id == impl_.mapRack;
    const bool byPicked = ours && static_cast<int32_t>(m.mapping->macro) == impl_.mapMacro;
    const bool hot = t.rect == hover_;
    paint::box(ctx.dl, t.rect, 0.0f, ui::withAlpha(th.circuit.wireBool, byPicked ? 140 : ours ? 80 : hot ? 70 : 35),
               hot ? th.accent.focus : ui::withAlpha(th.circuit.wireBool, 200));
  }

  void setHover(ui::IRect r) {
    if (r == hover_) return;
    if (ui::UiTree* t = tree()) {  // only the two outlines repaint
      auto grown = [](const ui::IRect& x) { return ui::IRect{x.x - 2, x.y - 2, x.w + 4, x.h + 4}; };
      if (!hover_.empty()) t->damage().add(grown(hover_).toRect());
      if (!r.empty()) t->damage().add(grown(r).toRect());
    }
    hover_ = r;
  }
  void onHoverChanged(bool h) override {
    if (!h) setHover({});
  }

  bool onPointer(const ui::PointerEvent& e) override {
    if (e.type == ui::PointerType::Move) {  // T4.16c: the target under the pointer is outlined brighter
      ui::IRect r{};
      forEachTarget([&](const Target& t) {
        if (t.rect.contains(e.pos)) r = t.rect;
        return !r.empty();
      });
      setHover(r);
      return false;
    }
    if (e.type != ui::PointerType::Down) return e.type == ui::PointerType::Up;
    if (e.button != ui::PointerButton::Left) return true;
    // The rack strip's Map button stays usable (it ends map mode).
    if (overMapButton(e.pos)) {
      impl_.setMapMode({});
      return true;
    }
    doc::DeviceId device;
    std::string param;
    int32_t macro = -1;
    forEachTarget([&](const Target& t) {
      if (!t.rect.contains(e.pos)) return false;
      device = t.device;
      param = std::string(t.param);
      macro = t.macro;
      return true;
    });
    // Acted on after the visit: mapping changes the document (and the panels the visit walks).
    if (macro >= 0) impl_.view.pickMacro(macro);
    else if (device.valid()) impl_.view.mapPicked(device, param);
    return true;
  }
  ui::InfoText info() const override {
    return {"Map Mode", "Click a macro of the rack, then knobs inside it: each click maps the knob to that macro (or "
                        "unmaps it). Esc or Map ends map mode."};
  }

private:
  DeviceChainView::Impl& impl_;
  ui::IRect hover_{};  // the target under the pointer (device px)
};

}  // namespace

std::unique_ptr<FrameBase> device_chain::makeRackFrame(DeviceChainView::Impl& impl, ViewContext& ctx, const doc::Device& d) {
  return std::make_unique<RackFrame>(impl, ctx, d);
}

std::unique_ptr<ui::Widget> device_chain::makeMapOverlay(DeviceChainView::Impl& impl) {
  return std::make_unique<MapOverlay>(impl);
}

void DeviceChainView::Impl::setMapMode(doc::DeviceId rack) {
  mapRack = rack;
  mapMacro = -1;
  if (rack.valid() && mapOverlay == nullptr) mapOverlay = &content->addChild(makeMapOverlay(*this));
  if (!rack.valid() && mapOverlay != nullptr) {
    (void)content->removeChild(*mapOverlay);
    mapOverlay = nullptr;
  }
  for (auto& [id, f] : all)
    if (f->isRack()) f->markDirty();
  content->invalidateLayout();
  content->markDirty();
}

// ---- DeviceChainView: racks ------------------------------------------------------------------------------------------

doc::ChainId DeviceChainView::shownChain(doc::DeviceId rack) const {
  auto* f = dynamic_cast<RackFrame*>(impl_->frameOf(rack));
  return f != nullptr ? f->shownChain() : doc::ChainId{};
}

void DeviceChainView::showChain(doc::DeviceId rack, doc::ChainId chain) {
  if (auto* f = dynamic_cast<RackFrame*>(impl_->frameOf(rack))) f->showChain(chain);
  else impl_->shownChain[rack.value] = chain;
}

void DeviceChainView::setMapMode(doc::DeviceId rack) { impl_->setMapMode(rack); }
doc::DeviceId DeviceChainView::mapRack() const noexcept { return impl_->mapRack; }
int32_t DeviceChainView::mapMacro() const noexcept { return impl_->mapMacro; }

void DeviceChainView::pickMacro(int32_t macro) {
  impl_->mapMacro = macro;
  if (impl_->mapOverlay != nullptr) impl_->mapOverlay->markDirty();
}

bool DeviceChainView::mapPicked(doc::DeviceId device, std::string_view param) {
  Impl& m = *impl_;
  if (!m.mapRack.valid() || m.mapMacro < 0) {
    ctx_.setStatus("Map: click a macro first, then the knob to map to it");
    return false;
  }
  const doc::Project& p = *ctx_.doc();
  const doc::MappingLoc cur = doc::mappingOf(p, device, param);
  bool r = false;
  if (cur.mapping != nullptr && cur.rack->id == m.mapRack && cur.mapping->macro == m.mapMacro) {
    r = m.run(doc::ops::unmapParam(p, device, param), false);  // a second click unmaps
  } else {
    // A macro's first mapping takes over the param's position (Live).
    std::optional<double> macroValue;
    const doc::DeviceLoc rl = doc::findDevice(p, m.mapRack);
    bool hasMappings = false;
    if (rl.device != nullptr && rl.device->rack)
      for (const doc::MacroMapping& mm : rl.device->rack->mappings) hasMappings = hasMappings || mm.macro == m.mapMacro;
    if (!hasMappings)
      if (devices::DevicePanel* panel = panelOf(device))
        if (const devices::ParamInfo* info = panel->signature().findParam(param))
          macroValue = devices::toPosition(*info, panel->paramValue(param)) * doc::kMacroMax;
    r = m.run(doc::ops::mapMacro(p, ctx_.store().ids(), m.mapRack, static_cast<uint32_t>(m.mapMacro), device,
                                 std::string(param), macroValue),
              false);
  }
  if (m.mapOverlay != nullptr) m.mapOverlay->markDirty();
  refreshRings();
  return r;
}

void DeviceChainView::showZones(doc::DeviceId rack, std::optional<doc::ZoneKind> tab) {
  impl_->zoneTab[rack.value] = tab ? static_cast<uint8_t>(*tab) : Impl::kZonesHidden;
  if (FrameBase* f = impl_->frameOf(rack)) {
    if (const doc::DeviceLoc l = doc::findDevice(*ctx_.doc(), rack); l.device != nullptr) f->update(*l.device);
  }
  impl_->reindex();
  refreshRings();
  impl_->content->invalidateLayout();
}

std::optional<doc::ZoneKind> DeviceChainView::zonesShown(doc::DeviceId rack) const {
  const auto it = impl_->zoneTab.find(rack.value);
  if (it == impl_->zoneTab.end() || it->second == Impl::kZonesHidden) return std::nullopt;
  return static_cast<doc::ZoneKind>(it->second);
}

namespace {

// A read-only document of a rack's lowered subcircuit (Q?-T4.8-a): the patch (its text is the printed IR), with the
// device files it calls as imports so a double-click opens them.
Result<std::shared_ptr<circuit::PatchDocument>> rackDocument(const doc::Project& project, const doc::DeviceLibrary& lib,
                                                            doc::DeviceId rack) {
  Result<graph::Patch> sub = doc::rackSubcircuit(project, rack, lib.library(), &lib);
  if (!sub) return std::move(sub).error();
  auto d = std::make_shared<circuit::PatchDocument>();
  d->path = sub->name() + ".pqp";
  d->source = graph::print(*sub);
  std::vector<std::string> files;
  for (graph::NodeIndex n = 0; n < sub->nodeCount(); ++n) {
    if (sub->node(n).kind != graph::NodeKind::Subcircuit) continue;
    const std::string f = lib.pathOf(sub->subcircuit(n).patch);
    if (!f.empty() && std::find(files.begin(), files.end(), f) == files.end()) files.push_back(f);
  }
  d->parsed.file.patches.push_back(std::move(*sub));
  d->parsed.lowered = true;
  // T4.4-fix1: a user device inside the rack calls its own renamed patches: they come from its resolution (printed
  // as the project plays them), with the stock file's imports for what they call.
  if (const doc::Device* rd = doc::findDevice(project, rack).device) {
    doc::forEachDeviceIn(*rd, [&](const doc::Device& x) {
      if (!x.source) return;
      const auto r = lib.resolve(x);
      if (!r || !r->graph) return;
      auto u = std::make_shared<circuit::PatchDocument>();
      u->path = lib.pathOf(x.patch);
      for (const graph::Patch& pp : r->graph->file.patches) u->source += graph::print(pp);
      u->parsed.file = r->graph->file;
      u->parsed.lowered = true;
      if (!u->path.empty())
        if (auto stock = circuit::loadPatchDocument(u->path)) u->imports = (*stock)->imports;
      d->imports.push_back(std::move(u));
    });
  }
  for (const std::string& f : files)
    if (auto imp = circuit::loadPatchDocument(f)) d->imports.push_back(std::move(*imp));
  return d;
}

// Keeps an open rack viewer in step with the document (a hidden child of the viewer: it lives as long as the viewer).
// A structural or meta edit that changes the rack's subcircuit rebuilds it (rack -> circuit view in sync); param-only
// edits (macro turns, mixer and zone drags) change runtime values, not the drawn structure, and cost nothing here.
// The rack removed: the viewer closes.
class RackViewerSync final : public ui::Widget {
public:
  RackViewerSync(circuit::CircuitViewer& viewer, ViewContext& ctx, doc::DeviceId rack)
      : Widget("rack-sync"), viewer_(viewer), ctx_(ctx), rack_(rack) {
    setVisible(false);
    sub_ = ctx.subscribe([this](const doc::StoreEvent& e) { onStore(e); });
  }

private:
  void onStore(const doc::StoreEvent& e) {
    const doc::DeviceLoc now = doc::findDevice(*e.after, rack_);
    if (now.device == nullptr || !now.device->rack) {
      // Closed on the next timer tick, not here: the edit may come from an event the viewer is dispatching.
      if (!closeTimer_) {
        ctx_.setStatus("The rack was removed: its circuit view closed.");
        closeTimer_ = requestTimer(0.0);
      }
      return;
    }
    if (!e.changes.has(doc::kChangeStructural) && !e.changes.has(doc::kChangeMeta)) return;
    const doc::DeviceLoc was = doc::findDevice(*e.before, rack_);
    if (was.device == now.device) return;  // the rack and everything in it unchanged
    const doc::DeviceLibrary* lib = ctx_.deviceLibrary();
    if (lib == nullptr) return;
    auto d = rackDocument(*e.after, *lib, rack_);
    if (!d) return;
    const auto& oldPatches = viewer_.document().parsed.file.patches;
    if (!oldPatches.empty() && oldPatches.front() == (*d)->parsed.file.patches.front() &&
        viewer_.document().imports.size() == (*d)->imports.size())
      return;  // the same subcircuit (an edit that does not change what the rack plays)
    (void)viewer_.replaceDocument(std::move(*d));
  }

  void onTimer(uint32_t id) override {
    if (id != closeTimer_) return;
    closeTimer_ = 0;
    if (ui::OverlayLayer* layer = ui::OverlayLayer::of(viewer_)) layer->close(viewer_);  // destroys this
  }

  circuit::CircuitViewer& viewer_;
  ViewContext& ctx_;
  doc::DeviceId rack_;
  uint32_t closeTimer_ = 0;
  ViewContext::Subscription sub_;
};

}  // namespace

bool DeviceChainView::openRackCircuit(doc::DeviceId rack) {
  const doc::DeviceLibrary* lib = ctx_.deviceLibrary();
  if (lib == nullptr) return false;
  auto d = rackDocument(*ctx_.doc(), *lib, rack);
  if (!d) {
    ctx_.setStatus(d.error().message);
    return false;
  }
  const std::string name = (*d)->parsed.file.patches.front().name();
  circuit::CircuitViewer* v = circuit::openCircuitViewer(*this, std::move(*d), name);
  if (v == nullptr) {
    ctx_.setStatus("The rack cannot be shown");
    return false;
  }
  v->add<RackViewerSync>(*v, ctx_, rack);
  return true;
}

}  // namespace pq::views
