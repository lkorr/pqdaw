// Racks in the Device view (T4.8, SPEC §8.2; Live's rack): pq/views/device_chain/device_chain.h. GU.9 v2: a rack is a
// set of PARALLEL GROUPS derived from the wiring (pq/document/racks.h, doc::RackInfo); this frame is its other view —
// the same objects the circuit draws as group capsules. The frame stands for the rack's chain on the main path (its id;
// any chain id names the rack to the edit builders). Left to right: a strip of buttons (show macros / chains / zones,
// Map, Rand, variations), the macro panel (GU.13: a panel of the one panel system in a BlockFrame, its knobs bound to the
// macros — value blocks of the rack — through RackPanelTarget), the chain list (one row per chain group: activator, name, solo, volume, pan — its Mixer group's value
// blocks), the zone editor (its zone groups' value blocks as bars aligned with the rows), then the frames of the chain
// it shows (the chain group's series, framed like the track's) and a slot that adds to that chain. Every edit is a
// document command (edit_ops.h, racks.h's builders), one undo step each; drags are store gestures (one step).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>

#include "../common/paint.h"
#include "internal.h"
#include "pq/core/automation_curve.h"
#include "pq/document/lower.h"
#include "pq/document/modules.h"
#include "pq/document/racks.h"
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
constexpr float kAxisH = 14.0f;      // M4 review M16: the zone editor's axis row
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
constexpr std::string_view kSelectParam = "select";  // the panel's chain selector knob
std::string macroParamName(uint32_t k) { return "macro" + std::to_string(k + 1); }
std::optional<uint32_t> macroIndex(std::string_view p) {
  if (p.size() < 6 || p.substr(0, 5) != "macro") return std::nullopt;
  const int k = std::atoi(std::string(p.substr(5)).c_str());
  if (k < 1 || k > static_cast<int>(doc::kMacroCount)) return std::nullopt;
  return static_cast<uint32_t>(k - 1);
}

// M4 review UI item 2: how many macros the panel shows: up to the rack's visibleMacros, but only the columns of four
// up to one past the highest macro in use (mapped or renamed: never a value, so a macro drag does not rebuild the
// panel under the pointer), at least one column: a new rack shows four, and there is always a free macro to map next.
// The chain selector is the zone editor's (its Chain tab's marker).
uint32_t shownMacros(const doc::RackInfo& r, uint32_t visibleMacros) {
  int32_t used = -1;
  for (const doc::RackMapping& m : r.mappings) used = std::max(used, static_cast<int32_t>(m.macro));
  for (uint32_t k = 0; k < doc::kMacroCount; ++k)
    if (r.macros[k].block.valid()) used = std::max(used, static_cast<int32_t>(k));
  const auto want = static_cast<uint32_t>((used + 2 + 3) / 4 * 4);  // one past the highest used, whole columns
  return std::clamp<uint32_t>(std::min<uint32_t>(visibleMacros, std::max(4u, want)), 1, doc::kMacroCount);
}

// The macro panel: the shown macros as knobs in columns of four, bound to Control "macro<k>" of the rack's frame id
// (RackPanelTarget resolves them); no elements (the title bar only) when the macros are hidden.
doc::Panel macroPanel(const doc::RackInfo& r, doc::DeviceId id, bool showMacros, uint32_t visibleMacros) {
  doc::Panel p;
  if (!showMacros) {
    p.width = 112.0f;
    p.height = 0.0f;
    return p;
  }
  const uint32_t n = shownMacros(r, visibleMacros);
  const uint32_t rows = std::min<uint32_t>(4, n), cols = (n + 3) / 4;
  for (uint32_t k = 0; k < n; ++k) {
    doc::PanelElement e;
    e.id = k + 1;
    e.kind = "knob";
    e.x = static_cast<float>(k / 4) * kMacroCell;
    e.y = static_cast<float>(k % 4) * kMacroCell;
    e.w = kMacroCell - 4.0f;
    e.h = kMacroCell;
    e.bindings = {doc::controlBinding(id, macroParamName(k))};
    p.elements.push_back(std::move(e));
  }
  p.width = std::max(144.0f, static_cast<float>(cols) * kMacroCell);  // room for the title
  p.height = static_cast<float>(rows) * kMacroCell;
  return p;
}

// The macro panel's target: the rack's macros (0 … 127, whole steps; GU.9: each its value block), as the frame last
// showed them (refresh). A drag is one store gesture of macro writes; a macro not mapped yet has no block (no edit).
class RackPanelTarget final : public panel::PanelTarget {
public:
  RackPanelTarget(DeviceChainView::Impl& impl, doc::DeviceId id, std::function<ui::Widget*()> host)
      : impl_(impl), id_(id), host_(std::move(host)) {
    for (panel::BoundInfo& i : infos_) {
      i.valid = true;
      i.curve = dsp::curvePreset(dsp::CurvePreset::Linear, 0.0, doc::kMacroMax);
      i.steps = static_cast<int32_t>(doc::kMacroMax) + 1;
    }
  }
  void refresh(const doc::RackInfo& r) {
    for (uint32_t k = 0; k < doc::kMacroCount; ++k) {
      values_[k] = r.macros[k].value;
      blocks_[k] = r.macros[k].block;
      if (std::string name = r.macroName(k); name != infos_[k].label) {
        infos_[k].label = std::move(name);
        infos_[k].generation = ++generation_;
      }
    }
    ++rev_;
  }
  const panel::BoundInfo& info(const doc::Binding& b) const override {
    const std::optional<uint32_t> k = indexOf(b);
    return k ? infos_[*k] : none_;
  }
  double position(const doc::Binding& b) const override {
    const std::optional<uint32_t> k = indexOf(b);
    return k ? std::clamp(values_[*k] / doc::kMacroMax, 0.0, 1.0) : 0.0;
  }
  std::string format(const doc::Binding&, double position) const override {
    return std::to_string(std::lround(std::clamp(position, 0.0, 1.0) * doc::kMacroMax));
  }
  std::optional<double> parse(const doc::Binding&, std::string_view text) const override {
    const std::string t(text);
    char* end = nullptr;
    const double v = std::strtod(t.c_str(), &end);
    if (end == t.c_str()) return std::nullopt;
    return std::clamp(v, 0.0, doc::kMacroMax) / doc::kMacroMax;
  }
  void begin(const doc::Binding&, const char* label) override {
    doc::DocumentStore& st = impl_.ctx().store();
    if (!own_ && !st.inGesture()) own_ = st.beginGesture(label).ok();
  }
  void setPosition(const doc::Binding& b, double position) override {
    if (const std::optional<uint32_t> k = indexOf(b))
      impl_.live(doc::ops::setMacroValue(impl_.doc(), id_, *k, std::clamp(position, 0.0, 1.0) * doc::kMacroMax));
  }
  void setCurve(const doc::Binding&, const dsp::Curve&) override {}
  void end() override {
    if (own_ && impl_.ctx().store().inGesture()) (void)impl_.ctx().store().commitGesture();
    own_ = false;
  }
  void cancel() override {
    if (own_ && impl_.ctx().store().inGesture()) (void)impl_.ctx().store().cancelGesture();
    own_ = false;
  }
  void setOnce(const doc::Binding& b, double position, const char*) override {
    if (const std::optional<uint32_t> k = indexOf(b))
      impl_.run(doc::ops::setMacroValue(impl_.doc(), id_, *k, std::clamp(position, 0.0, 1.0) * doc::kMacroMax), false);
  }
  // TX.reset-default: the macro's value block's default (where the macro was made; ops::setDeviceParam).
  void reset(const doc::Binding& b) override {
    if (const std::optional<uint32_t> k = indexOf(b); k && blocks_[*k].valid())
      impl_.run(doc::ops::setDeviceParam(impl_.doc(), blocks_[*k], std::string(doc::kValueParam), std::nullopt), false);
  }
  uint64_t revision() const override { return rev_; }
  void notice(std::string_view message) override { impl_.ctx().setStatus(std::string(message)); }
  ui::Widget* menuHost() const noexcept override { return host_ ? host_() : nullptr; }

private:
  std::optional<uint32_t> indexOf(const doc::Binding& b) const {
    if (b.kind != doc::BindingKind::Control || b.device != id_) return std::nullopt;
    return macroIndex(b.name);
  }
  DeviceChainView::Impl& impl_;
  doc::DeviceId id_;
  std::function<ui::Widget*()> host_;
  std::array<panel::BoundInfo, doc::kMacroCount> infos_{};
  std::array<double, doc::kMacroCount> values_{};
  std::array<doc::DeviceId, doc::kMacroCount> blocks_{};  // TX.reset-default
  panel::BoundInfo none_;
  uint64_t rev_ = 1, generation_ = 0;
  bool own_ = false;
};

class RackFrame;

// One chain rowclass RackFrame;

// One chain row: activator, name, solo, volume, pan (the chain mixer).
class ChainRow final : public ui::Widget {
public:
  ChainRow(RackFrame& rack, DeviceChainView::Impl& impl, const doc::RackChain& c);
  doc::ChainId chain() const noexcept { return chain_; }
  void set(const doc::RackChain& c, bool shown);
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
  void paintOver(ui::PaintContext& ctx) override;
  // T4.18: 0 none, 1 automated, 2 overridden by hand (the volume / pan boxes' LEDs; tests).
  uint8_t volumeAutomation() const noexcept { return volAuto_; }
  uint8_t panAutomation() const noexcept { return panAuto_; }

private:
  void endRename(bool commit);
  uint8_t volAuto_ = 0, panAuto_ = 0;
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
    if (!hoverTitle_.empty()) return {hoverTitle_, hoverBody_};  // M4 review M16: the hovered bar's range
    return {"Zones", "Where each chain plays: drag a bar's ends, its body, or (in its upper part) its fade edges. Key "
                     "zones are pitch ranges (Hz, or degrees of each note's own tuning), Velocity zones 0 … 127, Chain "
                     "zones the chain selector's range (drag the marker above them)."};
  }
  void onHoverChanged(bool hovered) override {
    if (!hovered) setHoverRow(-1);
  }
  // M4 review M16: the value text of an axis (Hz: 220 / 1.5k; degrees, velocity, selector: whole numbers).
  static std::string valueText(double v, bool hz);
  double toValue(float x) const;
  float toX(double v) const;
  // T4.16c: the ends and fade edges resize (↔), the body moves, the tabs and the key unit are buttons; the Chain tab's
  // header drags the selector's marker (↔).
  ui::Cursor cursorAt(ui::Vec2 p) const override;
  // M4 review M6: Esc or a lost focus (alt-tab) cancel a zone / selector drag (the zone goes back, no undo step); a
  // capture lost otherwise keeps what the drag did (one step), and nothing follows an unpressed pointer.
  bool onKey(const ui::KeyEvent& e) override {
    if (!e.down || e.key != ui::key::kEscape || grab_ == Grab::None) return Widget::onKey(e);
    cancel();
    return true;
  }
  void onInteractionCancelled() override { cancel(); }
  void onCaptureLost() override {
    if (grab_ != Grab::None && impl_.ctx().store().inGesture()) (void)impl_.ctx().store().commitGesture();
    grab_ = Grab::None;
  }

private:
  void cancel() {
    if (grab_ != Grab::None && impl_.ctx().store().inGesture()) (void)impl_.ctx().store().cancelGesture();
    grab_ = Grab::None;
  }
  enum class Grab : uint8_t { None, Lo, Hi, FadeLo, FadeHi, Move, Selector, Tab, Unit };
  // What a press at `p` grabs (the row's chain in *row); the one rule for onPointer and cursorAt.
  Grab grabAt(ui::Vec2 p, size_t* row) const;
  void setHoverRow(int32_t row);
  RackFrame& rack_;
  DeviceChainView::Impl& impl_;
  int32_t hoverRow_ = -1;  // M16: the row under the pointer (its range in the info view)
  std::string hoverTitle_, hoverBody_;
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
      : FrameBase("rack[" + std::to_string(d.id.value) + "]", impl, ctx, d.id), nesting_(impl.nestingBuilt) {
    track_ = doc::findDevice(impl.doc(), d.id).track != nullptr ? doc::findDevice(impl.doc(), d.id).track->id : doc::TrackId{};
    setLayout(ui::LayoutKind::Custom);
    strip_ = &add<RackStrip>(*this, impl);
    list_ = &add<ChainList>(*this, impl);
    zones_ = &add<ZoneEditor>(*this, impl);
    slot_ = &add<ChainSlot>(*this, impl);
    update(d);
  }

  const doc::RackInfo& rack() const noexcept { return rack_; }
  bool instrument() const noexcept { return rack_.instrument; }
  doc::ChainId shownChain() const noexcept { return shown_; }
  const doc::RackChain* shownChainPtr() const noexcept { return shown_.valid() ? rack_.findChain(shown_) : nullptr; }
  bool showChains() const { return impl_.rackShowChains.count(id_.value) == 0 || impl_.rackShowChains.at(id_.value); }
  bool showMacros() const { return impl_.rackShowMacros.count(id_.value) == 0 || impl_.rackShowMacros.at(id_.value); }
  uint32_t visibleMacros() const {
    const auto it = impl_.rackVisibleMacros.find(id_.value);
    return it == impl_.rackVisibleMacros.end() ? doc::kDefaultMacros : it->second;
  }
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
    for (const doc::RackMapping& m : rack_.mappings)
      if (m.device == rack_.selector && rack_.selector.valid()) {
        const double pos = m.min + (m.max - m.min) * (paramValue(macroParamName(m.macro)) / doc::kMacroMax);
        return std::round(std::clamp(pos, 0.0, 1.0) * doc::kSelectMax);
      }
    return paramValue(kSelectParam);
  }
  ChainList& list() noexcept { return *list_; }
  RackStrip& strip() noexcept { return *strip_; }

  panel::BlockFrame* panel() const noexcept override { return panel_; }
  bool isRack() const noexcept override { return true; }
  // GU.13: a macro's control is its value block's (Show Automation, Knob Curve); a macro not mapped yet has none.
  std::optional<std::pair<doc::DeviceId, std::string>> controlAt(ui::Vec2 p) const override {
    std::optional<std::pair<doc::DeviceId, std::string>> c = FrameBase::controlAt(p);
    if (!c || c->first != id_) return c;
    const std::optional<uint32_t> k = macroIndex(c->second);
    if (!k || !rack_.macros[*k].block.valid()) return std::nullopt;
    return std::make_pair(rack_.macros[*k].block, std::string(doc::kValueParam));
  }

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
    if (showChains()) w += kListW + (zoneTab() ? kZoneW : 0.0f);
    for (const FrameBase* f : inner_) w += kFrameGap + f->logicalWidth();
    return w + kFrameGap + kSlotW + kFrameGap;
  }
  float logicalHeight(float available) const override {
    if (panel_ != nullptr && panel_->folded()) return std::min(available, panel_->logicalHeight());
    return available;
  }

  // GU.9: what the frame shows is derived from the document (doc::RackInfo); the device given is its chain on the path.
  bool keeps(const doc::Device& d) const override {
    const doc::DeviceLoc l = doc::findDevice(impl_.doc(), d.id);
    return l.track != nullptr && doc::rackOfChain(impl_.doc(), *l.track, d.id).has_value();
  }
  bool derived() const noexcept override { return true; }
  void update(const doc::Device& d) override {
    const doc::DeviceLoc loc = doc::findDevice(impl_.doc(), d.id);
    std::optional<doc::RackInfo> info = loc.track != nullptr ? doc::rackOfChain(impl_.doc(), *loc.track, d.id) : std::nullopt;
    if (!info) return;  // (sync rebuilds the frame: keeps() is false now)
    const bool first = !built_;
    built_ = true;
    std::array<std::string, doc::kMacroCount> names{};
    for (uint32_t k = 0; k < doc::kMacroCount; ++k) names[k] = info->macroName(k);
    const uint32_t shown = shownMacros(*info, visibleMacros());
    const bool optionsChanged = first || names != macroNames_ || showMacros() != showMacrosBuilt_ || shown != shownMacros_ ||
                                info->instrument != rack_.instrument;
    macroNames_ = names;
    showMacrosBuilt_ = showMacros();
    shownMacros_ = shown;
    rack_ = std::move(*info);
    active_ = std::any_of(rack_.chains.begin(), rack_.chains.end(), [](const doc::RackChain& c) { return c.active; });
    params_.clear();
    for (uint32_t k = 0; k < doc::kMacroCount; ++k) params_.push_back(doc::DeviceValue{macroParamName(k), rack_.macros[k].value});
    params_.push_back(doc::DeviceValue{std::string(kSelectParam), rack_.selectorValue});
    // The macro panel: rebuilt when its layout changed, else its values.
    if (optionsChanged) {
      bool folded = false;
      if (panel_ != nullptr) {
        folded = panel_->folded();
        (void)removeChild(*panel_);
        panel_ = nullptr;
      }
      auto mp = std::make_shared<const doc::Panel>(macroPanel(rack_, id_, showMacros(), visibleMacros()));
      auto target = std::make_unique<RackPanelTarget>(impl_, id_, [this]() -> ui::Widget* { return panel_ != nullptr ? &panel_->host() : nullptr; });
      macros_ = target.get();
      auto face = std::make_unique<panel::GroupPanelFace>([mp]() -> doc::PanelPtr { return mp; }, std::move(target));
      face->setOwner(id_);
      panel::BlockFrame::Options o;
      o.title = rack_.instrument ? "Instrument Rack" : "Audio Effect Rack";
      title_ = o.title;
      panel_ = &add<panel::BlockFrame>("panel", std::move(face), std::move(o));
      panel_->setFolded(folded);
      panel_->onActivate = [this](bool on) {  // GU.9: every chain's activator (a rack has no switch of its own)
        const std::optional<doc::RackInfo> r = doc::rackOf(impl_.doc(), id_);
        if (!r) return;
        doc::DocumentStore& st = impl_.ctx().store();
        const bool own = !st.inGesture() && st.beginGesture(on ? "Activate Rack" : "Deactivate Rack").ok();
        for (const doc::RackChain& c : r->chains) impl_.live(doc::ops::setChainActive(impl_.doc(), c.id, on));
        if (own) (void)st.commitGesture();
      };
      panel_->onSizeChanged = [this] {
        applyVisibility();
        relayout();
      };
    }
    panel_->setActive(active_);
    panel_->setTitle(title_);
    macros_->refresh(rack_);
    panel_->host().notifyChanged();
    // The chain rows.
    bool sameRows = list_->rows.size() == rack_.chains.size();
    for (size_t i = 0; sameRows && i < rack_.chains.size(); ++i) sameRows = list_->rows[i]->chain() == rack_.chains[i].id;
    if (!sameRows) {
      for (ChainRow* r : list_->rows) (void)list_->removeChild(*r);
      list_->rows.clear();
      for (const doc::RackChain& c : rack_.chains) list_->rows.push_back(&list_->add<ChainRow>(*this, impl_, c));
      list_->invalidateLayout();
      list_->markDirty();
    }
    // The chain shown: the one picked (view state), else the first.
    const auto it = impl_.shownChain.find(id_.value);
    shown_ = it != impl_.shownChain.end() && rack_.findChain(it->second) != nullptr ? it->second
             : rack_.chains.empty()                                                   ? doc::ChainId{}
                                                                                       : rack_.chains.front().id;
    for (size_t i = 0; i < list_->rows.size(); ++i) list_->rows[i]->set(rack_.chains[i], rack_.chains[i].id == shown_);
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
  // GU.9: the rack's blocks changed somewhere (any edit of its track): shows them again.
  void refreshDerived() {
    if (const doc::Device* d = doc::findDevice(impl_.doc(), id_).device) update(*d);
  }

  // Shows another chain (view state: no document change).
  void showChain(doc::ChainId c) {
    impl_.shownChain[id_.value] = c;
    shown_ = c;
    for (size_t i = 0; i < list_->rows.size(); ++i) list_->rows[i]->set(rack_.chains[i], rack_.chains[i].id == shown_);
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
    list_->setVisible(showChains());
    zones_->setVisible(showChains() && zoneTab().has_value());
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
    if (showChains()) {
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
      if (const std::optional<ui::IRect> k = macroKnob(renamingMacro_)) pass.place(*macroRename_, {k->x, k->y, k->w, pass.px(16.0f)});
  }

  void paint(ui::PaintContext& ctx) override {
    const ui::Theme* th = ctx.res.theme;
    if (!th) return;
    const ui::IRect r = rect();
    paint::box(ctx.dl, r, 0.0f, th->device.background, 0);
    // The title band over the chain list and the shown chain (the macro panel draws its own title bar).
    const int32_t tb = ctx.scale.px(kTitleH);
    ctx.dl.addRectFilled(ui::IRect{r.x, r.y, r.w, tb}.toRect(), th->device.titleBar);
    if (const doc::RackChain* c = shownChainPtr(); c != nullptr && ctx.res.uiFonts && !(panel_ != nullptr && panel_->folded())) {
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
  void automationChanged() override {  // T4.18: the chain rows' volume / pan
    if (!built_ || list_ == nullptr || list_->rows.size() != rack_.chains.size()) return;
    for (size_t i = 0; i < list_->rows.size(); ++i) list_->rows[i]->set(rack_.chains[i], rack_.chains[i].id == shown_);
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
    if (showChains() && list_->rect().contains(p)) {
      for (ChainRow* row : list_->rows)
        if (row->rect().contains(p)) {
          const doc::RackChain* c = rack_.findChain(row->chain());
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
    const doc::RackChain* c = shownChainPtr();
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
    if (instrument() && !c->devices.empty() && (c->devices.front()->kind == doc::DeviceKind::Instrument || doc::isGroupDevice(*c->devices.front())) &&
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

  // The rack's own macro knobs (device px; map mode picks them).
  std::optional<ui::IRect> macroKnob(uint32_t k) const {
    if (panel_ == nullptr || panel_->folded()) return std::nullopt;
    return panel_->host().controlRect(doc::controlBinding(id_, macroParamName(k)));
  }

  // T4.16c: a right-click on a macro knob offers Rename Macro: a field over the knob's label, Enter commits (one undo
  // step), Esc cancels.
  void contextItems(ui::Vec2 p, std::vector<ui::MenuItem>& items) override {
    for (uint32_t k = 0; k < doc::kMacroCount; ++k) {
      const std::optional<ui::IRect> w = macroKnob(k);
      if (!w || !w->contains(p)) continue;
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
    if (!macroKnob(k) || macroRename_ != nullptr) return;
    renamingMacro_ = k;
    macroRename_ = &add<ui::TextEdit>("macro-rename", rack_.macroName(k));
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
    if (commit && !text.empty() && text != rack_.macroName(renamingMacro_)) {
      const auto r = doc::ops::renameMacro(impl_.doc(), id_, renamingMacro_, text);
      if (!r) impl_.ctx().setStatus(r.error().message);  // an unmapped macro has no block to name
      else impl_.run(r, false);
    }
  }

  // The frames of the chain shown: rebuilt when its devices changed, else updated.
  bool syncInner() {
    const doc::RackChain* c = shownChainPtr();
    // M4 review M9: below kMaxNestedRackFrames nested racks, the chain's devices are not framed.
    const bool deep = nesting_ + 1 >= DeviceChainView::kMaxNestedRackFrames;
    std::vector<doc::DevicePtr> devs;  // GU.9: the chain group's series (its devices, the snapshot's pointers)
    if (c != nullptr && !deep)
      if (const doc::Track* t = doc::findTrackPtr(impl_.doc(), track_))
        for (const doc::Device* x : c->devices)
          for (const doc::DevicePtr& p : t->devices)
            if (p.get() == x) devs.push_back(p);
    bool same = devs.size() == innerShown_.size();
    for (size_t i = 0; same && i < devs.size(); ++i) same = devs[i]->id == innerShown_[i]->id && inner_[i]->keeps(*devs[i]);
    if (same) {
      for (size_t i = 0; i < devs.size(); ++i)
        if (devs[i] != innerShown_[i] || inner_[i]->derived()) inner_[i]->update(*devs[i]);
    } else {
      for (FrameBase* f : inner_) (void)removeChild(*f);
      inner_.clear();
      const uint32_t outer = impl_.nestingBuilt;
      impl_.nestingBuilt = nesting_ + 1;  // the racks framed here are one level deeper
      for (const doc::DevicePtr& d : devs) {
        auto f = makeFrame(impl_, ctx_, *d);
        inner_.push_back(f.get());
        addChild(std::move(f));
      }
      impl_.nestingBuilt = outer;
    }
    innerShown_ = devs;
    return !same;
  }

  RackPanelTarget* macros_ = nullptr;  // the macro panel's target (owned by its face)
  std::string title_;
  uint32_t nesting_ = 0;  // racks around this one in the Device view (0: on the track's chain)
  doc::TrackId track_;
  doc::RackInfo rack_;
  bool built_ = false;
  std::array<std::string, doc::kMacroCount> macroNames_{};
  bool showMacrosBuilt_ = true;
  bool active_ = true;
  std::vector<doc::DeviceValue> params_;
  panel::BlockFrame* panel_ = nullptr;
  RackStrip* strip_ = nullptr;
  ChainList* list_ = nullptr;
  ZoneEditor* zones_ = nullptr;
  uint32_t shownMacros_ = 0;  // UI item 2: the macros the panel shows (rebuilt when it changes)
  ChainSlot* slot_ = nullptr;
  doc::ChainId shown_;
  std::vector<FrameBase*> inner_;
  std::vector<doc::DevicePtr> innerShown_;
  ui::TextEdit* macroRename_ = nullptr;  // T4.16c
  uint32_t renamingMacro_ = 0;
  bool macroRenameDone_ = false;
};

// ---- ChainRow ----------------------------------------------------------------------------------------------------

ChainRow::ChainRow(RackFrame& rack, DeviceChainView::Impl& impl, const doc::RackChain& c)
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

void ChainRow::set(const doc::RackChain& c, bool shown) {
  if (name_ != c.name || shown_ != shown) markDirty();
  name_ = c.name.empty() ? std::string("Chain") : c.name;
  // GU.9: a hand-wired parallel group has no chain mixer: its row shows no volume / pan.
  vol_->setVisible(c.volumeBlock.valid());
  pan_->setVisible(c.panBlock.valid());
  shown_ = shown;
  on_->setOn(c.active);
  solo_->setOn(c.solo);
  // T4.18: an automated volume / pan shows its envelope at the playhead (its own value once overridden by hand).
  double vol = c.volumeDb, pan = c.pan;
  uint8_t va = 0, pa = 0;
  // GU.9: their lanes are the Volume / Pan value blocks' (knob positions, through the blocks' curves).
  if (const doc::Track* t = impl_.track()) {
    const Ticks tick = impl_.ctx().transport().playhead().tick;
    for (const doc::LanePtr& l : t->lanes) {
      if (l->param.kind != doc::ParamKind::DeviceParam || l->points.empty()) continue;
      const bool isVol = c.volumeBlock.valid() && l->param.owner == c.volumeBlock.value;
      const bool isPan = c.panBlock.valid() && l->param.owner == c.panBlock.value;
      if (!isVol && !isPan) continue;
      const bool over = impl_.ctx().automationOverridden(l->param);
      (isVol ? va : pa) = over ? 2 : 1;
      if (!over) {
        const double pos = evalAutomation(std::span<const doc::Breakpoint>(l->points), static_cast<double>(tick), 0.0);
        if (isVol) vol = dsp::evalCurveUnits(doc::chainVolumeCurve(), pos);
        else pan = dsp::evalCurve(doc::chainPanCurve(), pos);
      }
    }
  }
  if (va != volAuto_ || pa != panAuto_) markDirty();
  volAuto_ = va;
  panAuto_ = pa;
  if (!vol_->inGesture()) vol_->setValue(vol);
  if (!pan_->inGesture()) pan_->setValue(pan);
}

void ChainRow::paintOver(ui::PaintContext& ctx) {
  if ((volAuto_ == 0 && panAuto_ == 0) || !ctx.res.theme) return;
  const auto& at = ctx.res.theme->automation;
  const float size = static_cast<float>(std::max(2, ctx.scale.px(3.0f)));
  auto led = [&](const ui::Widget* w, uint8_t state) {
    if (state == 0 || w == nullptr) return;
    const float x = static_cast<float>(w->rect().x + 1), y = static_cast<float>(w->rect().y + 1);
    ctx.dl.addRectFilled({x, y, x + size, y + size}, state == 2 ? at.ledOverridden : at.led);
  };
  led(vol_, volAuto_);
  led(pan_, panAuto_);
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
    const doc::RackChain* lc = rack_.rack().findChain(chain_);
    if (lc == nullptr) return true;
    DeviceChainView::Impl* im = &impl_;
    const doc::ChainId c = chain_;
    const doc::DeviceId rack = rack_.id();
    const int32_t index = rack_.rack().chainIndex(chain_);
    const size_t count = rack_.rack().chains.size();
    std::vector<ui::MenuItem> items;
    if (lc->volumeBlock.valid()) {  // T4.18: the chain's volume / pan lanes (GU.9: its Mixer's value blocks)
      ViewContext* vc = &impl_.ctx();
      const doc::TrackId t = rack_.rack().track;
      const doc::DeviceId vb = lc->volumeBlock, pb = lc->panBlock;
      items.push_back(ui::MenuItem::item("Show Volume Automation", [vc, t, vb] {
        vc->showAutomation(t, doc::ParamRef::device(vb, std::string(doc::kValueParam)));
      }));
      items.push_back(ui::MenuItem::item("Show Pan Automation", [vc, t, pb] {
        vc->showAutomation(t, doc::ParamRef::device(pb, std::string(doc::kValueParam)));
      }));
      items.push_back(ui::MenuItem::sep());
    }
    items.push_back(ui::MenuItem::item(lc->active ? "Deactivate Chain" : "Activate Chain",
                                       [im, c, on = lc->active] { im->run(doc::ops::setChainActive(im->doc(), c, !on), false); }));
    items.push_back(ui::MenuItem::item(lc->solo ? "Unsolo" : "Solo",
                                       [im, c, s = lc->solo] { im->run(doc::ops::setChainSolo(im->doc(), c, !s), false); }));
    items.push_back(ui::MenuItem::item("Rename", [im, rack, c] {  // T4.16c (the row is looked up: it may be rebuilt)
      if (auto* f = dynamic_cast<RackFrame*>(im->frameOf(rack)))
        for (ChainRow* row : f->list().rows)
          if (row->chain() == c) row->beginRename();
    }));
    items.push_back(ui::MenuItem::sep());
    items.push_back(ui::MenuItem::item("Duplicate Chain",
                                       [im, c] { im->run(doc::ops::duplicateChain(im->doc(), im->ctx().store().ids(), c), false); }));
    items.push_back(ui::MenuItem::item("Delete Chain", [im, c] {
      im->run(doc::ops::deleteChains(im->doc(), im->ctx().store().ids(), std::vector<doc::ChainId>{c}), false);
    }));
    items.push_back(ui::MenuItem::item("Add Chain",
                                       [im, rack] { im->run(doc::ops::addChain(im->doc(), im->ctx().store().ids(), rack), false); }));
    items.push_back(ui::MenuItem::sep());
    const bool first = index == 0, last = static_cast<size_t>(index) + 1 == count;
    ui::MenuItem up = ui::MenuItem::item("Move Up", [im, c, i = static_cast<uint32_t>(index)] {
      im->run(doc::ops::moveChain(im->doc(), c, i - 1), false);
    });
    up.enabled = !first;
    items.push_back(std::move(up));
    ui::MenuItem down = ui::MenuItem::item("Move Down", [im, c, i = static_cast<uint32_t>(index)] {
      im->run(doc::ops::moveChain(im->doc(), c, i + 1), false);
    });
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
    if (tab == doc::ZoneKind::Select)  // UI item 2: the chain selector lives here (its marker), its value beside the tabs
      paint::text(ctx, ctx.res.uiFonts->small, {x, r.y, r.right() - x - ctx.scale.px(4.0f), tb},
                  "Selector " + valueText(rack_.selectorValue(), false), th->text.secondary, ui::TextAlign::Right);
  }
  const ui::Rgba zoneFill = ui::withAlpha(th->accent.primary, 110), solidFill = ui::withAlpha(th->accent.primary, 200);
  const doc::RackInfo& rack = rack_.rack();
  // M4 review M16: an axis along the bottom (ticks and values in the tab's unit) with faint lines up through the rows.
  const int32_t axisH = ctx.scale.px(kAxisH);
  const int32_t axisY = r.bottom() - axisH;
  const Axis ax = axisOf(tab, rack.keyUnit);
  {
    static constexpr double kHzTicks[] = {20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000};
    std::vector<double> ticks;
    if (ax.log) ticks.assign(std::begin(kHzTicks), std::end(kHzTicks));
    else if (tab == doc::ZoneKind::Key) for (double d = ax.lo; d <= ax.hi + 1e-9; d += 12.0) ticks.push_back(d);
    else ticks = {0, 32, 64, 96, 127};
    ctx.dl.addRectFilled(ui::IRect{r.x + 1, axisY, r.w - 1, 1}.toRect(), th->surface.divider);
    int32_t lastRight = r.x - r.w;
    for (const double v : ticks) {
      const float x = std::round(toX(v));
      ctx.dl.addRectFilled({x, static_cast<float>(r.y + tb), x + 1.0f, static_cast<float>(axisY)}, ui::withAlpha(th->surface.divider, 120));
      ctx.dl.addRectFilled({x, static_cast<float>(axisY), x + 1.0f, static_cast<float>(axisY + ctx.scale.px(3.0f))}, th->text.secondary);
      if (!ctx.res.uiFonts || !ctx.res.text) continue;
      const std::string label = valueText(v, ax.log);
      const int32_t w = ctx.res.text->measure(ctx.res.uiFonts->small, label);
      int32_t lx = static_cast<int32_t>(x) - w / 2;
      lx = std::clamp(lx, r.x + ctx.scale.px(2.0f), r.right() - w - ctx.scale.px(2.0f));
      if (lx < lastRight + ctx.scale.px(4.0f)) continue;  // no overlapping labels
      paint::text(ctx, ctx.res.uiFonts->small, {lx, axisY + ctx.scale.px(2.0f), w, axisH - ctx.scale.px(2.0f)}, label,
                  th->text.secondary, ui::TextAlign::Left);
      lastRight = lx + w;
    }
  }
  for (size_t i = 0; i < rack.chains.size(); ++i) {
    const doc::RackChain& c = rack.chains[i];
    const int32_t y = r.y + tb + static_cast<int32_t>(i) * ctx.scale.px(kRowH);
    const int32_t h = ctx.scale.px(kRowH);
    if (y + h > axisY) break;
    if (static_cast<int32_t>(i) == hoverRow_)
      ctx.dl.addRectFilled(ui::IRect{r.x + 1, y, r.w - 1, h - 1}.toRect(), ui::withAlpha(th->text.primary, 14));
    ctx.dl.addRectFilled(ui::IRect{r.x, y + h - 1, r.w, 1}.toRect(), th->surface.divider);
    const doc::Zone& z = c.zone(tab);
    const float xl = toX(z.lo), xfl = toX(z.fadeLo), xfh = toX(z.fadeHi), xh = toX(z.hi);
    const float y0 = static_cast<float>(y) + 0.3f * static_cast<float>(h), y1 = static_cast<float>(y + h) - 3.0f * ctx.scale.exact(1.0f);
    // The fades as ramps, the full-gain part solid.
    if (xfl > xl) ctx.dl.addTriangleFilled({xl, y1}, {xfl, y0}, {xfl, y1}, zoneFill);
    if (xh > xfh) ctx.dl.addTriangleFilled({xfh, y0}, {xh, y1}, {xfh, y1}, zoneFill);
    if (xfh > xfl) ctx.dl.addRectFilled({xfl, y0, xfh, y1}, c.active ? solidFill : zoneFill);
    else ctx.dl.addRectFilled({xfl - 1.0f, y0, xfl + 1.0f, y1}, solidFill);
    // M16: the bar's range on it when it fits (its ends' values).
    if (ctx.res.uiFonts && ctx.res.text) {
      const std::string label = valueText(z.lo, ax.log) + " \xE2\x80\x93 " + valueText(z.hi, ax.log);
      const int32_t w = ctx.res.text->measure(ctx.res.uiFonts->small, label);
      if (static_cast<float>(w + ctx.scale.px(6.0f)) < xh - xl)
        paint::text(ctx, ctx.res.uiFonts->small,
                    {static_cast<int32_t>(xl), static_cast<int32_t>(y0), static_cast<int32_t>(xh - xl), static_cast<int32_t>(y1 - y0)},
                    label, th->text.onAccent, ui::TextAlign::Center);
    }
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
  if (p.y >= static_cast<float>(r.bottom()) - kAxisH * s) return Grab::None;  // M16: the axis row
  const auto row = static_cast<size_t>((p.y - static_cast<float>(r.y + tb)) / static_cast<float>(rh));
  if (row >= rack_.rack().chains.size()) return Grab::None;
  if (rowOut != nullptr) *rowOut = row;
  const doc::Zone& z = rack_.rack().chains[row].zone(tab);
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

std::string ZoneEditor::valueText(double v, bool hz) {
  char buf[32];
  if (!hz) std::snprintf(buf, sizeof buf, "%d", static_cast<int>(std::lround(v)));
  else if (v >= 1000.0) std::snprintf(buf, sizeof buf, std::fmod(v, 1000.0) < 1.0 ? "%.0fk" : "%.1fk", v / 1000.0);
  else std::snprintf(buf, sizeof buf, v < 100.0 && std::fabs(v - std::round(v)) >= 0.05 ? "%.1f" : "%.0f", v);
  return buf;
}

void ZoneEditor::setHoverRow(int32_t row) {
  const doc::RackInfo& rack = rack_.rack();
  if (row >= static_cast<int32_t>(rack.chains.size())) row = -1;
  if (row == hoverRow_) return;
  hoverRow_ = row;
  hoverTitle_.clear();
  hoverBody_.clear();
  if (row >= 0) {
    const doc::ZoneKind tab = rack_.zoneTab().value_or(doc::ZoneKind::Select);
    const bool hz = axisOf(tab, rack.keyUnit).log;
    const char* unit = tab == doc::ZoneKind::Key ? (hz ? " Hz" : " degrees") : "";
    const doc::RackChain& c = rack.chains[static_cast<size_t>(row)];
    const doc::Zone& z = c.zone(tab);
    hoverTitle_ = (c.name.empty() ? std::string("Chain") : c.name) + ": " + valueText(z.lo, hz) + " \xE2\x80\x93 " +
                  valueText(z.hi, hz) + unit;
    hoverBody_ = "Full level " + valueText(z.fadeLo, hz) + " \xE2\x80\x93 " + valueText(z.fadeHi, hz) + unit +
                 "; fades in from " + valueText(z.lo, hz) + " and out to " + valueText(z.hi, hz) +
                 ". Drag the bar's ends, its body, or (in its upper part) its fade edges.";
  }
  markDirty();
  impl_.ctx().infoChanged();
}

bool ZoneEditor::onPointer(const ui::PointerEvent& e) {
  const ui::IRect r = rect();
  const float s = tree() ? tree()->scale() : 1.0f;
  const doc::ZoneKind tab = rack_.zoneTab().value_or(doc::ZoneKind::Select);
  doc::DocumentStore& store = impl_.ctx().store();
  if (e.type == ui::PointerType::Move && grab_ == Grab::None) {  // M16: the hovered row
    const int32_t tb = static_cast<int32_t>(std::lround(kTitleH * s)), rh = static_cast<int32_t>(std::lround(kRowH * s));
    const float axisY = static_cast<float>(r.bottom()) - kAxisH * s;
    int32_t row = -1;
    if (r.contains(e.pos) && e.pos.y >= static_cast<float>(r.y + tb) && e.pos.y < axisY)
      row = static_cast<int32_t>((e.pos.y - static_cast<float>(r.y + tb)) / static_cast<float>(rh));
    setHoverRow(row);
  }
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
      impl_.run(doc::ops::setKeyZoneUnit(impl_.doc(), impl_.ctx().store().ids(), rack_.id(), rack_.rack().keyUnit == doc::KeyZoneUnit::Hz
                                                                        ? doc::KeyZoneUnit::Degree
                                                                        : doc::KeyZoneUnit::Hz),
                false);
      return true;
    }
    if (g == Grab::Selector) {
      grab_ = Grab::Selector;
      (void)store.beginGesture("Device Parameter");
      impl_.live(doc::ops::setSelectorValue(impl_.doc(), rack_.id(), std::round(toValue(e.pos.x))));
      if (!rack_.rack().selector.valid()) impl_.ctx().setStatus("Chain selector: give a chain a chain zone first (drag its bar)");
      return true;
    }
    grab_ = g;
    if (grab_ == Grab::None) return true;
    const doc::RackChain& c = rack_.rack().chains[row];
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
      impl_.live(doc::ops::setSelectorValue(impl_.doc(), rack_.id(), std::round(v)));
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
    if (doc::zoneValid(z, tab, rack_.rack().keyUnit))
      impl_.live(doc::ops::setChainZone(impl_.doc(), impl_.ctx().store().ids(), chain_, tab, z));
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
  if (const doc::RackChain* c = rack_.shownChainPtr()) {
    ui::openContextMenu(*this, at, impl_.deviceMenu(rack_.rack().track, c->id, doc::kAppend, rack_.instrument()));
    return true;
  }
  // No chain yet: a device picked makes the first chain with it.
  std::vector<ui::MenuItem> items;
  DeviceChainView::Impl* im = &impl_;
  const doc::DeviceId rack = rack_.id();
  for (const doc::ModuleInfo& m : doc::moduleInfos()) {  // GU.11: the built-in modules (group presets)
    const doc::DeviceKind kind = m.instrument ? doc::DeviceKind::Instrument : doc::DeviceKind::AudioEffect;
    ui::MenuItem it = ui::MenuItem::item(std::string(m.name), [im, rack, patch = std::string(doc::kModuleItemPrefix) + std::string(m.key), kind] {
      im->run(doc::ops::addChainWithDevice(im->doc(), im->ctx().store().ids(), rack, patch, kind));
    });
    it.enabled = !m.instrument || rack_.instrument();
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
  const doc::RackInfo& rk = rack_.rack();
  const bool on[kButtons] = {rack_.showMacros(), rack_.showChains(), rack_.zoneTab().has_value(), impl_.mapRack == rack_.id(),
                             false, rk.variations > 0};
  const char* text[kButtons] = {"M", "C", "Z", "Map", "Rnd", "Var"};
  for (uint32_t b = 0; b < kButtons; ++b) {
    const ui::IRect br = buttonRect(b);
    paint::box(ctx.dl, br, ctx.scale.exact(2.0f), on[b] ? (b == kMap ? th->circuit.wireBool : th->accent.primary) : th->control.track, 0);
    paint::text(ctx, ctx.res.uiFonts->small, br, text[b], on[b] ? th->text.onAccent : th->text.secondary, ui::TextAlign::Center);
  }
}

bool RackStrip::onPointer(const ui::PointerEvent& e) {
  if (e.type != ui::PointerType::Down || e.button != ui::PointerButton::Left) return e.type == ui::PointerType::Up;
  const doc::RackInfo& rk = rack_.rack();
  const doc::DeviceId id = rack_.id();
  for (uint32_t b = 0; b < kButtons; ++b) {
    if (!buttonRect(b).contains(e.pos)) continue;
    switch (b) {  // GU.9: what the frame shows is view state (a rack has no options of its own)
      case kMacros:
        impl_.rackShowMacros[id.value] = !rack_.showMacros();
        rack_.refreshDerived();
        rack_.relayout();
        break;
      case kChains:
        impl_.rackShowChains[id.value] = !rack_.showChains();
        rack_.refreshDerived();
        rack_.relayout();
        break;
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
        if (rk.variations > 0) items.push_back(ui::MenuItem::sep());
        for (uint32_t i = 0; i < rk.variations; ++i) {
          const std::string n = "Variation " + std::to_string(i + 1);
          items.push_back(ui::MenuItem::item("Recall " + n, [im, id, i] { im->run(doc::ops::recallVariation(im->doc(), id, i), false); }));
          items.push_back(ui::MenuItem::sub("   " + n, {ui::MenuItem::item("Overwrite", [im, id, i] {
                                                          im->run(doc::ops::storeVariation(im->doc(), id, i), false);
                                                        }),
                                                        ui::MenuItem::item("Delete", [im, id, i] {
                                                          im->run(doc::ops::deleteVariation(im->doc(), id, i), false);
                                                        })}));
        }
        items.push_back(ui::MenuItem::sep());
        std::vector<ui::MenuItem> excl;
        for (uint32_t k = 0; k < rack_.visibleMacros(); ++k) {
          const bool x = rk.macros[k].excluded;
          ui::MenuItem it = ui::MenuItem::item(rk.macroName(k), [im, id, k, x] {
            im->run(doc::ops::setMacroExcluded(im->doc(), id, k, !x), false);
          });
          it.checked = x;
          it.enabled = rk.macros[k].block.valid();
          excl.push_back(std::move(it));
        }
        items.push_back(ui::MenuItem::sub("Exclude from Rand", std::move(excl)));
        std::vector<ui::MenuItem> count;
        for (uint32_t n : {1u, 2u, 4u, 8u, 12u, 16u}) {
          RackFrame* rf = &rack_;
          ui::MenuItem it = ui::MenuItem::item(std::to_string(n), [im, id, n, rf] {
            im->rackVisibleMacros[id.value] = n;
            rf->refreshDerived();
            rf->relayout();
          });
          it.checked = rack_.visibleMacros() == n;
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
    const std::optional<doc::RackInfo>& rk = mapInfo();
    if (!rk) return;
    for (const auto& [id, fr] : impl_.all) {
      const doc::DeviceId dev{id};
      const bool self = dev == impl_.mapRack;
      // GU.11 / GU.13: the controls of the frame's panels (a leaf's, a module's), bound to blocks inside the rack; the
      // rack's own frame: its macros (picks); a nested rack's macros: their value blocks. Switches are not mapped.
      bool stop = false;
      device_chain::forEachPanelControl(*fr, [&](const device_chain::PanelControl& c) {
        if (self) {
          if (c.device != dev) return false;
          const std::optional<uint32_t> m = macroIndex(c.param);
          return stop = m.has_value() && f(Target{dev, c.param, c.rect, static_cast<int32_t>(*m)});
        }
        Target t{c.device, c.param, c.rect, -1};
        if (fr->isRack() && c.device == dev) {  // a nested rack's macro
          const std::optional<uint32_t> m = macroIndex(c.param);
          const std::optional<doc::RackInfo> inner = m ? doc::rackOf(impl_.doc(), dev) : std::nullopt;
          if (!inner || !inner->macros[*m].block.valid()) return false;
          t.device = inner->macros[*m].block;
          t.param = doc::kValueParam;
        }
        if (!rk->holds(impl_.doc(), t.device) || c.host->panelFace()->target().info(*c.binding).isSwitch) return false;
        return stop = f(t);
      });
      if (stop) return;
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
    const doc::RackMapping* m = nullptr;
    if (const std::optional<doc::RackInfo>& rk = mapInfo())
      for (const doc::RackMapping& x : rk->mappings)
        if (x.device == t.device && x.param == t.param) m = &x;
    const bool ours = m != nullptr;
    const bool byPicked = ours && static_cast<int32_t>(m->macro) == impl_.mapMacro;
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
  // The rack in map mode, derived once per document snapshot (a pointer move asks every target).
  const std::optional<doc::RackInfo>& mapInfo() const {
    if (infoFrom_ != impl_.ctx().doc() || infoRack_ != impl_.mapRack) {
      infoFrom_ = impl_.ctx().doc();
      infoRack_ = impl_.mapRack;
      info_ = doc::rackOf(impl_.doc(), impl_.mapRack);
    }
    return info_;
  }
  DeviceChainView::Impl& impl_;
  ui::IRect hover_{};  // the target under the pointer (device px)
  mutable doc::Snapshot infoFrom_;
  mutable doc::DeviceId infoRack_;
  mutable std::optional<doc::RackInfo> info_;
};

}  // namespace

std::unique_ptr<FrameBase> device_chain::makeRackFrame(DeviceChainView::Impl& impl, ViewContext& ctx, const doc::Device& d) {
  return std::make_unique<RackFrame>(impl, ctx, d);
}

std::unique_ptr<ui::Widget> device_chain::makeMapOverlay(DeviceChainView::Impl& impl) {
  return std::make_unique<MapOverlay>(impl);
}

device_chain::ChainPlace device_chain::chainPlaceOf(const doc::Project& p, doc::DeviceId device) {
  ChainPlace out;
  const doc::DeviceLoc l = doc::findDevice(p, device);
  if (l.device == nullptr || l.track == nullptr || !l.device->group.valid()) return out;
  out.rack = doc::rackOfChain(p, *l.track, l.device->group);
  if (!out.rack) return out;
  const doc::RackChain* c = out.rack->findChain(l.device->group);
  out.chain = c->id;
  for (const doc::Device* x : c->devices)
    for (const doc::DevicePtr& d : l.track->devices)
      if (d.get() == x) {
        if (x->id == device) out.index = static_cast<int32_t>(out.list.size());
        out.list.push_back(d);
      }
  return out;
}

double device_chain::playedPosition(const doc::Project& p, doc::DeviceId block, uint32_t depth) {
  const doc::Device* d = doc::findDevice(p, block).device;
  if (d == nullptr) return 0.0;
  const doc::DeviceValue* v = d->param(doc::kValueParam);
  double pos = v != nullptr ? v->value : 0.0;
  if (depth < 16)
    for (const doc::Wire& w : doc::wiresOf(p))
      if (!w.toMixer() && w.to == block && w.param == doc::kValueParam && w.travels() && !w.fromNoteSource())
        pos += w.amount * playedPosition(p, w.from, depth + 1);
  return std::clamp(pos, 0.0, 1.0);
}

std::optional<std::pair<doc::RackInfo, doc::RackMapping>> device_chain::macroMappingOf(const doc::Project& p,
                                                                                         doc::DeviceId device,
                                                                                         std::string_view param) {
  for (const doc::Wire& w : doc::wiresOf(p)) {
    if (w.toMixer() || w.to != device || w.param != param || !w.travels() || w.fromNoteSource()) continue;
    const doc::Device* m = doc::findDevice(p, w.from).device;
    if (m == nullptr || !doc::isValueDevice(*m) || m->setting(doc::kMacroSetting) == nullptr) continue;
    std::optional<doc::RackInfo> r = doc::rackOf(p, w.from);
    if (!r) continue;
    for (const doc::RackMapping& x : r->mappings)
      if (x.wire == w.id) return std::make_pair(std::move(*r), x);
  }
  return std::nullopt;
}

void DeviceChainView::Impl::setMapMode(doc::DeviceId rack) {
  // M4 review UI item 6: Map mode says what to do next.
  if (rack.valid() && !mapRack.valid())
    view.ctx_.setStatus("Map: click a macro, then each parameter it should drive (again: unmaps it); Esc or Map ends.");
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
  const std::optional<doc::RackInfo> rk = doc::rackOf(p, m.mapRack);
  if (!rk) return false;
  const doc::RackMapping* cur = nullptr;
  for (const doc::RackMapping& x : rk->mappings)
    if (x.device == device && x.param == param) cur = &x;
  bool r = false;
  if (cur != nullptr && static_cast<int32_t>(cur->macro) == m.mapMacro) {
    r = m.run(doc::ops::unmapMacro(p, cur->wire), false);  // a second click unmaps
  } else {
    // A macro's first mapping takes over the param's position (Live).
    std::optional<double> macroValue;
    const bool hasMappings = rk->macros[static_cast<size_t>(m.mapMacro)].block.valid();
    if (!hasMappings) {  // the control's position as shown (GU.13: on whichever panel shows it)
      doc::Binding b;
      if (panel::PanelHost* h = m.hostOf(device, param, &b)) macroValue = h->panelFace()->target().position(b) * doc::kMacroMax;
    }
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

}  // namespace pq::views
