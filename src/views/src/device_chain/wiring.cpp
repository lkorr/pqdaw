// Cross-device wiring in the views (T4.7): pq/views/device_chain/wiring.h.
#include "pq/views/device_chain/wiring.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_set>

#include "../common/paint.h"
#include "pq/document/main_path.h"
#include "pq/document/edit_ops.h"
#include "pq/ui/lines.h"
#include "pq/ui/overlay.h"
#include "pq/ui/theme.h"
#include "pq/views/arrangement/arrangement.h"
#include "pq/views/device_chain/device_chain.h"
#include "pq/views/project_patch/project_patch_view.h"
#include "pq/views/shell/shell.h"

namespace pq::views {

// ---- FixedModulationSource ------------------------------------------------------------------------------------------

void FixedModulationSource::set(doc::DeviceId device, std::string param, ModSweep s) {
  for (Entry& e : entries_)
    if (e.device == device && e.param == param) {
      e.sweep = s;
      return;
    }
  entries_.push_back({device, std::move(param), s});
}

void FixedModulationSource::readModulation(std::span<const ModTarget> targets, std::span<ModSweep> out) noexcept {
  ++reads_;
  for (size_t i = 0; i < targets.size() && i < out.size(); ++i) {
    out[i] = ModSweep{};
    for (const Entry& e : entries_)
      if (e.device == targets[i].device && e.param == targets[i].param) {
        out[i] = e.sweep;
        if (!live_) out[i].up = out[i].down = 0.0f;  // as the engine: nothing sweeps while it sleeps
      }
  }
}

namespace {

constexpr float kBadge = 15.0f;      // logical px: the badge's diameter
constexpr float kLinkBadge = 9.0f;   // TX.free-rack: a chain link's joint
constexpr float kTuckedBadge = 11.0f;  // TMM.13-fix2 (H6): a cable from the collapsed free rack, at its knob
constexpr float kCable = 2.0f;       // logical px: cable thickness
constexpr int kSamples = 40;         // points per cable
constexpr uint32_t kSpringTicks = 21;   // drag ticks (1/60 s) hovering a track row before its chain shows (0.35 s)
constexpr float kEdge = 24.0f;       // logical px: auto-scroll zone at a list's edge
constexpr float kAmountDragPx = 150.0f;  // logical px of vertical drag per unit of amount

// TMM.13-fix2 (MM review M10): cables take the circuit's palette, by what they carry — the audio path (chain links, a
// wire into an input) blue, the note source's amber, modulation into a param mint (theme circuit.wireAudio / wireNotes /
// wireMod). Without a theme (never in the app): the same colours.
ui::Rgba roleColour(const ui::Theme* th, uint8_t role) noexcept {
  switch (role) {
    case 2: return th != nullptr ? th->circuit.wireNotes : ui::rgba(240, 165, 60);
    case 1: return th != nullptr ? th->circuit.wireAudio : ui::signal_theme::kAccent;
    default: return th != nullptr ? th->circuit.wireMod : ui::rgba(102, 220, 192);
  }
}

// A cubic bezier that leaves the jack upwards and drops into the knob from above (a sagging patch cable). TX.free-rack:
// an end on a socket leaves sideways instead (an output to the right, into an input from the left), so a chain link
// between neighbouring frames is a short straight run along their bottom row.
void cablePoints(ui::Vec2 a, uint8_t da, ui::Vec2 b, uint8_t db, float scale, ui::Vec2 (&pts)[kSamples]) {
  using Dir = DeviceChainView::EndDir;
  const float dx = b.x - a.x;
  const float lift = (28.0f + 0.2f * std::fabs(dx)) * scale;
  const float side = dx > 0.0f ? std::clamp(0.45f * dx, 2.0f * scale, 90.0f * scale) : std::clamp(0.3f * -dx, 40.0f * scale, 120.0f * scale);
  // From the collapsed rack's tab the cable drops down onto the chain (not up into the arrangement).
  const float drop = std::max(20.0f * scale, 0.5f * std::fabs(b.y - a.y));
  auto ctrl = [&](ui::Vec2 p, uint8_t d) -> ui::Vec2 {
    switch (static_cast<Dir>(d)) {
      case Dir::Right: return {p.x + side, p.y};
      case Dir::Left: return {p.x - side, p.y};
      case Dir::Down: return {p.x, p.y + drop};
      case Dir::Up: break;
    }
    return {p.x, p.y - (da == static_cast<uint8_t>(Dir::Down) ? std::min(lift, drop) : lift)};
  };
  const ui::Vec2 c1 = ctrl(a, da), c2 = ctrl(b, db);
  for (int i = 0; i < kSamples; ++i) {
    const float t = static_cast<float>(i) / (kSamples - 1), u = 1.0f - t;
    const float w0 = u * u * u, w1 = 3 * u * u * t, w2 = 3 * u * t * t, w3 = t * t * t;
    pts[i] = {w0 * a.x + w1 * c1.x + w2 * c2.x + w3 * b.x, w0 * a.y + w1 * c1.y + w2 * c2.y + w3 * b.y};
  }
}

ui::IRect boundsOf(const ui::Vec2* pts, int n, float pad) {
  float x0 = pts[0].x, x1 = pts[0].x, y0 = pts[0].y, y1 = pts[0].y;
  for (int i = 1; i < n; ++i) {
    x0 = std::min(x0, pts[i].x);
    x1 = std::max(x1, pts[i].x);
    y0 = std::min(y0, pts[i].y);
    y1 = std::max(y1, pts[i].y);
  }
  const auto l = static_cast<int32_t>(std::floor(x0 - pad)), t = static_cast<int32_t>(std::floor(y0 - pad));
  return {l, t, static_cast<int32_t>(std::ceil(x1 + pad)) - l, static_cast<int32_t>(std::ceil(y1 + pad)) - t};
}

ui::IRect unite(const ui::IRect& a, const ui::IRect& b) {
  if (a.empty()) return b;
  if (b.empty()) return a;
  const int32_t l = std::min(a.x, b.x), t = std::min(a.y, b.y);
  return {l, t, std::max(a.right(), b.right()) - l, std::max(a.bottom(), b.bottom()) - t};
}

bool shownNow(const ui::Widget& w) noexcept {
  for (const ui::Widget* p = &w; p != nullptr; p = p->parent())
    if (!p->visible()) return false;
  return w.tree() != nullptr;
}

// The rate class of a device export (CableLayer::Cable::rate): voice for an instrument's, audio when what drives it
// reads the audio input or runs per sample on its own (a generator, memory, voice / transport / sample atoms, a
// subcircuit), param when it is computed from params and constants only.
uint8_t exportRate(const doc::Device& d, const doc::DeviceLibrary* lib, std::string_view port) {
  if (d.kind == doc::DeviceKind::Instrument) return 2;
  const graph::Patch* p = lib != nullptr ? lib->patchOf(d) : nullptr;
  if (p == nullptr) return 1;
  const graph::Adjacency adj = p->adjacency();
  std::vector<uint8_t> seen(p->nodeCount(), 0);
  std::vector<graph::NodeIndex> stack;
  for (graph::NodeIndex n = 0; n < p->nodeCount(); ++n)
    if (p->node(n).kind == graph::NodeKind::Export && p->nodeName(n) == port) stack.push_back(n);
  while (!stack.empty()) {
    const graph::NodeIndex n = stack.back();
    stack.pop_back();
    if (seen[n]) continue;
    seen[n] = 1;
    const graph::Node& nd = p->node(n);
    if (nd.kind == graph::NodeKind::Input && p->outputType(n, 0).rate != graph::Rate::Const) return 1;
    if (nd.kind == graph::NodeKind::Subcircuit) return 1;
    if (nd.kind == graph::NodeKind::Atom) {
      const graph::AtomCategory c = graph::atomSignature(nd.atom).category;
      if (c == graph::AtomCategory::Generator || c == graph::AtomCategory::Memory || c == graph::AtomCategory::Voice ||
          c == graph::AtomCategory::Transport || c == graph::AtomCategory::Sample || c == graph::AtomCategory::Io)
        return 1;
    }
    for (uint32_t i = 0; i < nd.numInputs; ++i)
      if (const graph::WireIndex w = adj.driver[p->flatInput(n, i)]; w != graph::kNone) stack.push_back(p->wire(w).from.node);
  }
  return 0;
}

std::string deviceName(const ViewContext& ctx, const doc::Project& p, doc::DeviceId id) {
  const doc::DeviceLoc l = doc::findDevice(p, id);
  if (l.device == nullptr) return "?";
  const doc::DeviceInfo* info = ctx.deviceLibrary() != nullptr ? ctx.deviceLibrary()->infoOf(*l.device) : nullptr;
  if (!l.device->name.empty()) return l.device->name;
  return info != nullptr ? info->name : l.device->patch;
}

std::string trackName(const doc::Project& p, doc::TrackId id) {
  const doc::Track* t = doc::findTrackPtr(p, id);
  return t != nullptr ? t->name : std::string("?");
}

std::string paramLabel(const ViewContext& ctx, const doc::Project& p, doc::DeviceId id, const std::string& param) {
  const doc::DeviceLoc l = doc::findDevice(p, id);
  const doc::DeviceInfo* info = l.device != nullptr && ctx.deviceLibrary() != nullptr
                                    ? ctx.deviceLibrary()->infoOf(*l.device) : nullptr;
  const doc::DeviceParamInfo* pi = info != nullptr ? info->param(param) : nullptr;
  return pi != nullptr ? pi->label : param;
}

// ---- the badge in the middle of a cable: select, drag the amount, menu, Delete -----------------------------------------

class Badge final : public ui::Widget {
public:
  // TX.free-rack: `wire` invalid: a chain link's badge (a small joint that reads the link; nothing to select or delete).
  Badge(CableLayer& layer, ViewContext& ctx, uint64_t key, doc::WireId wire)
      : Widget(wire.valid() ? "wire[" + std::to_string(wire.value) + "]" : "link[" + std::to_string(key) + "]"), layer_(layer),
        ctx_(ctx), wire_(wire), key_(key) {
    setFocusable(wire.valid());
  }
  doc::WireId wire() const noexcept { return wire_; }
  uint64_t key() const noexcept { return key_; }
  void setLook(uint8_t rate, doc::WireStatus status, bool selected) {
    if (rate == rate_ && status == status_ && selected == selected_) return;
    rate_ = rate;
    status_ = status;
    selected_ = selected;
    markDirty();
  }
  void amountChanged() { markDirty(); }

  void paint(ui::PaintContext& ctx) override {
    const ui::Theme* th = ctx.res.theme;
    if (!th) return;
    if (!wire_.valid()) {  // a chain link: a joint in the cable's colour, lit while hovered
      const ui::IRect r = rect();
      const ui::Vec2 c{static_cast<float>(r.x) + 0.5f * static_cast<float>(r.w), static_cast<float>(r.y) + 0.5f * static_cast<float>(r.h)};
      const float rad = 0.5f * static_cast<float>(std::min(r.w, r.h)) - ctx.scale.exact(0.5f);
      const float alpha = layer_.mode() == CableMode::Fade ? 0.4f : 1.0f;
      const ui::Rgba col = roleColour(th, rate_);
      ctx.dl.addCircleFilled(c, rad, ui::withAlpha(ui::lerp(th->surface.raised, col, hovered() ? 0.6f : 0.3f), static_cast<uint32_t>(240 * alpha)));
      ctx.dl.addCircleStroke(c, rad, ui::withAlpha(hovered() ? th->accent.focus : col, static_cast<uint32_t>(230 * alpha)), ctx.scale.exact(1.0f));
      return;
    }
    const doc::Wire* w = doc::findWire(*ctx_.doc(), wire_);
    if (w == nullptr) return;
    const ui::IRect r = rect();
    const ui::Vec2 c{static_cast<float>(r.x) + 0.5f * static_cast<float>(r.w), static_cast<float>(r.y) + 0.5f * static_cast<float>(r.h)};
    const float rad = 0.5f * static_cast<float>(std::min(r.w, r.h)) - ctx.scale.exact(1.0f);
    const bool off = status_ == doc::WireStatus::Inactive || status_ == doc::WireStatus::Invalid || status_ == doc::WireStatus::Refused;
    const ui::Rgba col = off ? th->text.disabled : roleColour(th, rate_);
    const float alpha = layer_.mode() == CableMode::Fade ? 0.4f : 1.0f;
    ctx.dl.addCircleFilled(c, rad, ui::withAlpha(ui::lerp(th->surface.raised, col, 0.25f), static_cast<uint32_t>(240 * alpha)));
    if (w->travels()) {
      // The amount as an arc from the top (clockwise positive), like a small bipolar knob.
      const float a0 = -1.5707963f, a1 = a0 + 3.14159265f * static_cast<float>(w->amount);
      if (std::fabs(a1 - a0) > 1e-3f)
        ctx.dl.addArcStroke(c, rad - ctx.scale.exact(1.5f), std::min(a0, a1), std::max(a0, a1),
                            ui::withAlpha(col, static_cast<uint32_t>(255 * alpha)), ctx.scale.exact(2.5f));
    } else {
      // TX.patching: a wire that adds in its target's units has no amount: a plus (it adds), no arc.
      const float k = rad * 0.5f, t = ctx.scale.exact(1.5f);
      const ui::Rgba pc = ui::withAlpha(col, static_cast<uint32_t>(255 * alpha));
      ctx.dl.addRectFilled(ui::Rect{c.x - k, c.y - 0.5f * t, c.x + k, c.y + 0.5f * t}, pc);
      ctx.dl.addRectFilled(ui::Rect{c.x - 0.5f * t, c.y - k, c.x + 0.5f * t, c.y + k}, pc);
    }
    // T4.16c: hovered (or selected), the ring is lit and thicker; the layer lights the cable too.
    ctx.dl.addCircleStroke(c, rad, ui::withAlpha(selected_ || focused() || hovered() ? th->accent.focus : col, static_cast<uint32_t>(220 * alpha)),
                           ctx.scale.exact(selected_ || hovered() ? 1.6f : 1.0f));
  }
  // T4.16c: drag up / down sets the amount (TX.patching: a Travel wire's; an added wire has none).
  ui::Cursor cursorAt(ui::Vec2) const override {
    if (!wire_.valid()) return ui::Cursor::Arrow;  // TX.free-rack: a chain link only reads
    return travels() ? ui::Cursor::ResizeNS : ui::Cursor::Pointer;
  }
  void onHoverChanged(bool h) override {
    markDirty();
    layer_.hoverBadge(key_, h);
  }

  bool onPointer(const ui::PointerEvent& e) override {
    if (layer_.dragging()) return false;  // TX.free-rack: a cable dropped over a badge is the layer's (an input socket under it)
    if (!wire_.valid()) {  // TX.free-rack: a chain link: what it is, in the status line
      if (e.type == ui::PointerType::Down)
        ctx_.setStatus("The chain's own signal path: drag a device's title bar onto the free-device rack to take it out of "
                       "the chain, or switch it off to pass it by");
      return e.type == ui::PointerType::Down || e.type == ui::PointerType::Up;
    }
    switch (e.type) {
      case ui::PointerType::Down:
        if (tree()) tree()->setFocus(this);
        layer_.select(wire_);
        if (e.button == ui::PointerButton::Right) {
          openMenu(e.pos);
          return true;
        }
        if (e.button != ui::PointerButton::Left) return false;
        if (!travels()) return true;  // TX.patching: selected; nothing to drag
        if (e.clickCount == 2) {
          setAmount(doc::kDefaultWireAmount, true);
          return true;
        }
        if (const doc::Wire* w = doc::findWire(*ctx_.doc(), wire_)) {
          dragging_ = true;
          moved_ = false;
          startY_ = e.pos.y;
          startAmount_ = w->amount;
        }
        return true;
      case ui::PointerType::Move: {
        if (!dragging_) return false;
        const float s = tree() ? tree()->scale() : 1.0f;
        const double fine = (e.mods & ui::mod::kShift) != 0 ? 0.1 : 1.0;
        const double a = std::clamp(startAmount_ - fine * (e.pos.y - startY_) / (kAmountDragPx * s), -1.0, 1.0);
        if (!moved_) {
          moved_ = true;
          if (!ctx_.store().inGesture()) {
            (void)ctx_.store().beginGesture("Wire Amount");
            ownGesture_ = true;
          }
        }
        setAmount(std::round(a * 1000.0) / 1000.0, false);
        return true;
      }
      case ui::PointerType::Up:
        if (!dragging_) return false;
        dragging_ = false;
        if (ownGesture_ && ctx_.store().inGesture()) (void)ctx_.store().commitGesture();
        ownGesture_ = false;
        return true;
      default: return false;
    }
  }
  void onInteractionCancelled() override {
    if (ownGesture_ && ctx_.store().inGesture()) (void)ctx_.store().cancelGesture();
    ownGesture_ = false;
    dragging_ = false;
  }
  void onCaptureLost() override {
    if (ownGesture_ && ctx_.store().inGesture()) (void)ctx_.store().commitGesture();
    ownGesture_ = false;
    dragging_ = false;
  }
  bool onKey(const ui::KeyEvent& e) override {
    if (!e.down || (e.key != ui::key::kDelete && e.key != ui::key::kBackspace)) return false;
    layer_.select(wire_);
    return layer_.deleteSelected();
  }
  std::string_view tooltip() const override {
    tip_.clear();
    if (!wire_.valid()) {  // TX.free-rack: a chain link
      if (const CableLayer::Cable* c = layer_.cableOf(key_)) tip_ = c->label;
      return tip_;
    }
    const doc::Project& p = *ctx_.doc();
    if (const doc::Wire* w = doc::findWire(p, wire_)) {
      char amount[32];
      std::snprintf(amount, sizeof amount, "%+.0f %%", 100.0 * w->amount);
      // TMM.10-fix2: a note-source wire's source is its track's notes.
      const std::string src = w->fromNoteSource() ? trackName(p, w->noteSource) + " notes" : deviceName(ctx_, p, w->from);
      const CableLayer::Cable* c = layer_.cableOf(key_);
      tip_ = src + " " + w->port + " \xE2\x86\x92 " + deviceName(ctx_, p, w->to) + " " + paramLabel(ctx_, p, w->to, w->param) + "  " +
             (c != nullptr && c->input ? std::string("(into the input: a signal path)")
                                       : (w->travels() ? std::string(amount) : std::string("(adds)")));
      switch (status_) {
        case doc::WireStatus::Delayed: tip_ += "  (closes a loop: delayed by 1 sample)"; break;
        case doc::WireStatus::Inactive: tip_ += "  (an end is switched off)"; break;
        case doc::WireStatus::Invalid: tip_ += "  (the device has no such port)"; break;
        case doc::WireStatus::Refused: tip_ += "  (not played: a loop the engine cannot run, through two instrument tracks or the voices' sum)"; break;
        default: break;
      }
    }
    return tip_;
  }
  ui::InfoText info() const override {
    if (!wire_.valid())
      return {"Signal Path", "The chain's own signal: what each device passes to the next (green: audio, amber: notes or "
                             "voices). A switched-off device is passed by; free devices (the rack above) take no part."};
    if (!travels())
      return {"Modulation Wire", "Adds its signal to the parameter in the parameter's own units (Hz to a frequency, "
                                 "0 to 1 to a 0 to 1 knob), clamped to its range; scale it with a VCA. Delete removes it; "
                                 "right-click for more. z-1: the wire closes a loop and is delayed by one sample."};
    return {"Modulation Wire", "Drag up or down to set how far it moves the knob (Shift: fine); double-click: 50 %. "
                               "Delete removes it; right-click for more. z-1: the wire closes a loop "
                               "and is delayed by one sample."};
  }

private:
  bool travels() const {
    const doc::Wire* w = doc::findWire(*ctx_.doc(), wire_);
    return w != nullptr && w->travels();
  }
  void setAmount(double a, bool ownStep) {
    auto e = doc::ops::setWireAmount(*ctx_.doc(), wire_, a);
    if (!e || e->empty()) return;
    (void)ownStep;
    (void)ctx_.store().dispatch(e->command);
  }
  void openMenu(ui::Vec2 at) {
    const doc::Wire* w = doc::findWire(*ctx_.doc(), wire_);
    if (w == nullptr) return;
    std::vector<ui::MenuItem> items;
    CableLayer* layer = &layer_;
    ViewContext* ctx = &ctx_;
    const doc::WireId id = wire_;
    items.push_back(ui::MenuItem::item("Delete Wire", [layer] { layer->deleteSelected(); }, "Delete"));
    if (w->travels()) {
    items.push_back(ui::MenuItem::item("Invert Amount", [ctx, id] {
      if (const doc::Wire* x = doc::findWire(*ctx->doc(), id))
        if (auto e = doc::ops::setWireAmount(*ctx->doc(), id, -x->amount); e && !e->empty()) (void)ctx->store().dispatch(e->command);
    }));
    items.push_back(ui::MenuItem::item("Reset Amount (50 %)", [ctx, id] {
      if (auto e = doc::ops::setWireAmount(*ctx->doc(), id, doc::kDefaultWireAmount); e && !e->empty())
        (void)ctx->store().dispatch(e->command);
    }));
    }
    if (rate_ == 2) {  // a voice source: how its voices become one value (SPEC §7.4)
      static constexpr const char* kNames[] = {"Sum", "Last Voice", "Highest", "Lowest", "Loudest"};
      std::vector<ui::MenuItem> sub;
      for (int k = 0; k < 5; ++k) {
        const auto r = static_cast<doc::WireReducer>(k);
        ui::MenuItem it = ui::MenuItem::item(kNames[k], [ctx, id, r] {
          if (auto e = doc::ops::setWireReducer(*ctx->doc(), id, r); e && !e->empty()) (void)ctx->store().dispatch(e->command);
        });
        it.checked = w->reducer == r;
        sub.push_back(std::move(it));
      }
      items.push_back(ui::MenuItem::sep());
      items.push_back(ui::MenuItem::sub("Voices", std::move(sub)));
    }
    ui::openContextMenu(*this, at, std::move(items));
  }

  CableLayer& layer_;
  ViewContext& ctx_;
  doc::WireId wire_;
  uint64_t key_ = 0;
  uint8_t rate_ = 1;
  doc::WireStatus status_ = doc::WireStatus::Lowered;
  bool selected_ = false;
  bool dragging_ = false, moved_ = false, ownGesture_ = false;
  float startY_ = 0.0f;
  double startAmount_ = 0.0;
  mutable std::string tip_;
};

}  // namespace

struct CableLayer::Impl {
  std::vector<Badge*> badges;                // one per drawn wire with a badge
  std::vector<doc::WireState> states;        // doc::wireStates of `statesFrom`
  doc::Snapshot statesFrom;
  const doc::DeviceLibrary* statesLib = nullptr;  // the library they were planned with
  ui::IRect dragBounds{};                    // what the dragged cable and the target outlines painted
  uint32_t lastHoverRow = 0xFFFFFFFFu;
  // The tree and the slots the layer reads, not the Shell: a screenshot or a test may destroy the Shell object before
  // the widget tree.
  ui::UiTree* tree = nullptr;
  ui::Widget* trackArea = nullptr;
  ui::Widget* deviceSlot = nullptr;
  DeviceChainView* chain() const { return dynamic_cast<DeviceChainView*>(deviceSlot->find("chain-view")); }
  ArrangementView* arrangement() const { return dynamic_cast<ArrangementView*>(trackArea->find("arrangement")); }
};

CableLayer::CableLayer(ViewContext& ctx, Shell& shell)
    : Widget("cable-layer"), ctx_(ctx), impl_(std::make_unique<Impl>()) {
  impl_->tree = &shell.tree();
  impl_->trackArea = &shell.slot(ShellSlot::TrackArea);
  impl_->deviceSlot = &shell.slot(ShellSlot::DeviceDetail);
  setLayout(ui::LayoutKind::Custom);
  setHittable(false);  // the pointer passes through, except on badges (and while a cable is dragged)
  sub_ = ctx.subscribe([this](const doc::StoreEvent& e) { onStore(e); });
  observer_ = ctx.observe([this](uint32_t bits) {
    if (bits & (view_change::kDevices | view_change::kSelection | view_change::kDetailClip | view_change::kLayout)) refresh();
  });
  shell.tree().addObserver(this);
  rebuildStatus();
}

CableLayer::~CableLayer() {
  impl_->tree->removeObserver(this);
  ctx_.clearViewAction("view.cycleCables");
  ctx_.clearViewAction("wire.delete");
}

void CableLayer::setMode(CableMode m) {
  if (m == mode_) return;
  mode_ = m;
  setVisible(m != CableMode::Hide);  // hidden: no paint, no hit testing, no geometry work (onLayout returns at once)
  markDirty();
  refresh();
  for (Badge* b : impl_->badges) b->markDirty();
  ctx_.setStatus(m == CableMode::Show ? "Cables: shown" : m == CableMode::Fade ? "Cables: faded" : "Cables: hidden");
}

void CableLayer::cycleMode() { setMode(mode_ == CableMode::Show ? CableMode::Fade : mode_ == CableMode::Fade ? CableMode::Hide : CableMode::Show); }

void CableLayer::select(doc::WireId w) {
  if (w == selected_) return;
  selected_ = w;
  refresh();
}

bool CableLayer::deleteSelected() {
  if (!selected_.valid() || doc::findWire(*ctx_.doc(), selected_) == nullptr) return false;
  auto e = doc::ops::deleteWires(*ctx_.doc(), std::vector<doc::WireId>{selected_});
  if (!e) {
    ctx_.setStatus(e.error().message);
    return false;
  }
  selected_ = {};
  return ctx_.store().dispatch(e->command).ok();
}

doc::WireId CableLayer::badgeAt(ui::Vec2 pos) const noexcept {
  for (const Badge* b : impl_->badges)
    if (b->visible() && b->rect().contains(pos)) return b->wire();
  return {};
}

void CableLayer::rebuildStatus() {
  const doc::Snapshot s = ctx_.doc();
  if (s == impl_->statesFrom && impl_->statesLib == ctx_.deviceLibrary()) return;
  impl_->statesFrom = s;
  impl_->statesLib = ctx_.deviceLibrary();
  impl_->states = ctx_.deviceLibrary() != nullptr ? doc::wireStates(*s, *ctx_.deviceLibrary())
                                                  : std::vector<doc::WireState>{};
}

void CableLayer::onStore(const doc::StoreEvent& e) {
  if (e.kind == doc::StoreEventKind::GestureCommit) return;
  if (e.kind == doc::StoreEventKind::Reset || e.changes.structural()) {
    if (selected_.valid() && doc::findWire(*ctx_.doc(), selected_) == nullptr) selected_ = {};
    rebuildStatus();
    refresh();
    return;
  }
  bool amounts = false;
  for (const doc::ParamRef& r : e.changes.params) amounts = amounts || r.kind == doc::ParamKind::WireAmount;
  if (amounts)
    for (Badge* b : impl_->badges) b->amountChanged();
}

void CableLayer::onLayout() {
  if (!visible()) return;
  refresh();
}

bool CableLayer::refresh() {
  if (!tree()) return false;
  if (!visible()) {
    if (cables_.empty()) return false;
    cables_.clear();
    return true;
  }
  // The wire states change only with the structure (onStore rebuilds them then); a param edit keeps them — no wire
  // planning of the whole project per knob step (TX.all-free: a project has a main link per block).
  if ((impl_->statesFrom == nullptr || impl_->statesLib != ctx_.deviceLibrary()) && ctx_.deviceLibrary() != nullptr)
    rebuildStatus();
  const doc::Project& p = *ctx_.doc();
  DeviceChainView* chain = impl_->chain();
  ArrangementView* arr = impl_->arrangement();
  const float s = tree()->scale();
  // A track's anchor in the arrangement: its header's left edge (clamped to the visible rows).
  auto trackAnchor = [&](doc::TrackId id) -> std::optional<ui::Vec2> {
    if (arr == nullptr || !shownNow(*arr)) return std::nullopt;
    const ui::IRect vp = arr->lanesViewport();
    const auto rows = arr->rows();
    for (size_t r = 0; r < rows.size(); ++r)
      if (rows[r].id == id) {
        const ui::IRect lr = arr->rowLaneRect(static_cast<int32_t>(r));
        const float y = std::clamp(static_cast<float>(lr.y) + 0.5f * static_cast<float>(lr.h), static_cast<float>(vp.y),
                                   static_cast<float>(vp.bottom()));
        return ui::Vec2{static_cast<float>(vp.right()), y};
      }
    const auto bottom = arr->bottomRows();
    for (size_t r = 0; r < bottom.size(); ++r)
      if (bottom[r].id == id) {
        const ui::IRect lr = arr->pinnedRowRect(r);
        return ui::Vec2{static_cast<float>(vp.right()), static_cast<float>(lr.y) + 0.5f * static_cast<float>(lr.h)};
      }
    return std::nullopt;
  };
  auto statusOf = [&](doc::WireId id) {
    for (const doc::WireState& st : impl_->states)
      if (st.id == id) return st.status;
    return doc::WireStatus::Lowered;
  };
  std::vector<Cable> next;
  auto place = [&](Cable& c, float badge) {
    ui::Vec2 pts[kSamples];
    cablePoints(c.a, c.da, c.b, c.db, s, pts);
    c.bounds = boundsOf(pts, kSamples, 3.0f * s + 2.0f);
    const ui::Vec2 mid = pts[kSamples / 2];
    const int32_t bd = static_cast<int32_t>(std::lround(badge * s));
    c.badge = {static_cast<int32_t>(std::lround(mid.x)) - bd / 2, static_cast<int32_t>(std::lround(mid.y)) - bd / 2, bd, bd};
    c.bounds = unite(c.bounds, ui::IRect{c.badge.x, c.badge.y, c.badge.w + static_cast<int32_t>(std::lround(32.0f * s)), c.badge.h});
  };
  // TX.free-rack: the chain's own signal path first (series audio, the notes into the first device), under the wires.
  const doc::TrackId shown = chain != nullptr ? chain->track() : doc::TrackId{};
  if (chain != nullptr)
    for (const DeviceChainView::ChainLink& k : chain->chainLinks()) {
      Cable c;
      c.kind = Kind::Chain;
      c.key = (1ull << 63) | (static_cast<uint64_t>(k.from.value & 0x7FFFFFFFu) << 32) | k.to.value;
      c.a = k.a.at;
      c.b = k.b.at;
      c.aDocked = k.a.docked;
      c.bDocked = k.b.docked;
      c.da = static_cast<uint8_t>(k.a.dir);
      c.db = static_cast<uint8_t>(k.b.dir);
      c.rate = k.rate;
      c.role = k.rate == 2 && !k.from.valid() ? 2 : 1;  // M10: the notes into the chain amber, the audio path blue
      c.input = true;
      const std::string src = k.from.valid() ? deviceName(ctx_, p, k.from) : (k.rate == 2 ? std::string("Notes") : std::string("Audio clips"));
      c.label = src + " \xE2\x86\x92 " + deviceName(ctx_, p, k.to) + (k.input.empty() || k.input == "in" ? std::string() : " " + k.input) +
                (k.rate == 2 ? "  \xC2\xB7  notes / voices (the chain)" : "  \xC2\xB7  audio (the chain)");
      place(c, kLinkBadge);
      next.push_back(std::move(c));
    }
  // TX.all-free: the shown track's main path is drawn above (chainLinks): its links are not drawn again.
  const doc::Track* shownTrack = shown.valid() ? doc::findTrackPtr(p, shown) : nullptr;
  const doc::MainPath path = shownTrack != nullptr ? doc::mainPath(p, *shownTrack) : doc::MainPath{};
  // The shown track's blocks: a main link touching none of them (nor its source or mixer) is never drawn here, and is
  // skipped before any lookup (a project has one per block).
  std::unordered_set<uint32_t> shownBlocks;
  if (shownTrack != nullptr)
    for (const doc::DevicePtr& d : shownTrack->devices) shownBlocks.insert(d->id.value);
  // PF.2: a cable has an end here only on the shown chain (its blocks, its note source): every other wire is skipped
  // before any lookup — this runs on every layout pass (an S-drag step in the project patch view: 8.5k wires).
  static const doc::WireList noWires;
  for (const doc::Wire& w : chain != nullptr ? doc::wiresOf(p) : noWires) {
    if (!(w.fromNoteSource() ? w.noteSource == shown : shownBlocks.count(w.from.value) != 0) &&
        !(!w.toMixer() && shownBlocks.count(w.to.value) != 0))
      continue;
    if (path.isLink(w.id)) continue;
    // GU.9: a rack's own wiring — its chains' feeds and sums, its macros' mappings, its selector — is its frame's (the
    // rows, the macro panel, Map mode), not a cable.
    if (chain != nullptr && ((!w.fromNoteSource() && chain->heldByRack(w.from)) || (!w.toMixer() && chain->heldByRack(w.to))))
      continue;
    if (w.mainLink() && !(w.fromNoteSource() ? w.noteSource == shown : shownBlocks.count(w.from.value) != 0) &&
        !(w.toMixer() ? w.mixer == shown : shownBlocks.count(w.to.value) != 0))
      continue;
    const doc::DeviceLoc from = w.fromNoteSource() ? doc::DeviceLoc{} : doc::findDevice(p, w.from), to = doc::findDevice(p, w.to);
    if ((!w.fromNoteSource() && from.device == nullptr) || to.device == nullptr) continue;  // (into a mixer: the circuit's)
    // TX.all-free: a main port by name (kMainPort: its first output, its main input).
    const std::string port = w.port.empty() && from.device != nullptr ? doc::mainPortsOf(*from.device, ctx_.deviceLibrary()).out : w.port;
    const std::string param = w.param.empty() ? doc::mainPortsOf(*to.device, ctx_.deviceLibrary()).in : w.param;
    // TX.groups: a wire inside a group (between its members and terminals) is drawn by the group's panel.
    if (from.device != nullptr) {
      // GU.9: a group end is its group by its terminals (In / Out); by its main ports it is a member of its parent.
      const doc::DeviceId ga = doc::isGroupDevice(*from.device) && w.port == doc::kGroupInPort ? from.device->id : from.device->group;
      const doc::DeviceId gb = doc::isGroupDevice(*to.device) && w.param == doc::kGroupOutPort ? to.device->id : to.device->group;
      if (ga.valid() && ga == gb) continue;
      // TX.free-rack: between two devices of the free-device rack: drawn by the rack's canvas.
      if (chain != nullptr && chain->inFreeRack(w.from) && chain->inFreeRack(w.to)) continue;
    }
    Cable c;
    c.wire = w.id;
    c.key = w.id.value;
    std::optional<DeviceChainView::CableEnd> ea, eb;
    if (chain != nullptr) {
      if (w.fromNoteSource()) {  // TMM.10-fix2: the shown track's note source is the chain's left end
        if (w.noteSource == shown) ea = chain->notesEnd();
      } else {
        ea = chain->sourceEnd(w.from, port);
      }
      eb = chain->targetEnd(w.to, param);
      c.input = chain->isInput(w.to, param);
    }
    if (!ea && !eb) continue;  // neither end in the shown chain: not drawn (the project patch view shows every wire, T4.13)
    std::optional<ui::Vec2> a = ea ? std::optional<ui::Vec2>(ea->at) : trackAnchor(w.fromNoteSource() ? w.noteSource : from.track->id);
    std::optional<ui::Vec2> b = eb ? std::optional<ui::Vec2>(eb->at) : trackAnchor(to.track->id);
    if (!a || !b) continue;
    c.a = *a;
    c.b = *b;
    c.aDocked = ea && ea->docked;
    c.bDocked = eb && eb->docked;
    c.da = ea ? static_cast<uint8_t>(ea->dir) : 0;
    c.db = eb ? static_cast<uint8_t>(eb->dir) : 0;
    c.status = statusOf(w.id);
    c.rate = from.device != nullptr ? exportRate(*from.device, ctx_.deviceLibrary(), port) : 2;
    if (c.input && c.rate == 0) c.rate = 1;  // TX.free-rack: into an input it is a signal path (heard as audio)
    c.role = w.fromNoteSource() ? 2 : (c.input ? 1 : 0);  // M10: notes amber, a signal path blue, modulation mint
    place(c, kBadge);
    // H6: from (or into) the collapsed free rack the cables would fan out of its tab across the panels: the badge sits
    // on the chain end instead (above-right of the knob or socket) and the cable shows while it is hovered.
    if (chain != nullptr && !chain->freeRackOpen() && from.device != nullptr &&
        (chain->inFreeRack(w.from) || chain->inFreeRack(w.to))) {
      c.tucked = true;
      const bool fromRack = chain->inFreeRack(w.from);
      const ui::Vec2 end = fromRack ? c.b : c.a;
      const int32_t bd = static_cast<int32_t>(std::lround(kTuckedBadge * s));
      c.badge = {static_cast<int32_t>(std::lround(end.x + 9.0f * s)), static_cast<int32_t>(std::lround(end.y + 2.0f * s)), bd, bd};
      c.bounds = unite(c.bounds, c.badge);
    }
    next.push_back(std::move(c));
  }
  // L22: the chain's links are first, in chain order; between one's target end and the next one's source end (on one row,
  // left to right) lies the device the audio passes through.
  for (size_t i = 1; i < next.size() && next[i].kind == Kind::Chain; ++i) {
    Cable& c = next[i];
    const Cable& prev = next[i - 1];
    if (std::fabs(prev.b.y - c.a.y) > 1.0f || c.a.x <= prev.b.x) continue;
    c.through = true;
    c.t0 = prev.b;
    c.t1 = c.a;
    const int32_t pad = static_cast<int32_t>(std::ceil(2.0f * s)) + 1;
    c.bounds = unite(c.bounds, ui::IRect{static_cast<int32_t>(std::floor(c.t0.x)) - pad, static_cast<int32_t>(std::floor(c.t0.y)) - pad,
                                         static_cast<int32_t>(std::ceil(c.t1.x - c.t0.x)) + 2 * pad, 2 * pad});
  }
  // Damage what moved: per wire the old and the new cable (a cable that did not move costs nothing).
  bool moved = false;
  ui::Damage& dmg = tree()->damage();
  auto same = [](const Cable& x, const Cable& y) {
    return x.a.x == y.a.x && x.a.y == y.a.y && x.b.x == y.b.x && x.b.y == y.b.y && x.status == y.status && x.rate == y.rate && x.role == y.role &&
           x.aDocked == y.aDocked && x.bDocked == y.bDocked && x.da == y.da && x.db == y.db && x.through == y.through &&
           x.t0.x == y.t0.x && x.t0.y == y.t0.y && x.t1.x == y.t1.x && x.tucked == y.tucked;
  };
  for (const Cable& n : next) {
    const auto o = std::find_if(cables_.begin(), cables_.end(), [&](const Cable& c) { return c.key == n.key; });
    if (o != cables_.end() && same(*o, n)) continue;
    moved = true;
    if (o != cables_.end()) dmg.add(o->bounds.toRect());
    dmg.add(n.bounds.toRect());
  }
  for (const Cable& o : cables_)
    if (std::none_of(next.begin(), next.end(), [&](const Cable& c) { return c.key == o.key; })) {
      moved = true;
      dmg.add(o.bounds.toRect());
    }
  cables_ = std::move(next);
  // Badges: one per drawn cable.
  bool badgesChanged = false;
  for (size_t i = 0; i < impl_->badges.size();) {
    Badge* b = impl_->badges[i];
    if (std::none_of(cables_.begin(), cables_.end(), [&](const Cable& c) { return c.key == b->key(); })) {
      if (hoverKey_ == b->key()) hoverKey_ = 0;
      (void)removeChild(*b);
      impl_->badges.erase(impl_->badges.begin() + static_cast<std::ptrdiff_t>(i));
      badgesChanged = true;
    } else {
      ++i;
    }
  }
  for (const Cable& c : cables_) {
    Badge* b = nullptr;
    for (Badge* x : impl_->badges)
      if (x->key() == c.key) b = x;
    if (b == nullptr) {
      b = &add<Badge>(*this, ctx_, c.key, c.wire);
      impl_->badges.push_back(b);
      badgesChanged = true;
    }
    b->setLook(c.role, c.status, c.wire.valid() && c.wire == selected_);
    if (!(b->rect() == c.badge)) badgesChanged = true;
  }
  if (badgesChanged) invalidateArrange();
  return moved || badgesChanged;
}

const CableLayer::Cable* CableLayer::cableOf(uint64_t key) const noexcept {
  for (const Cable& c : cables_)
    if (c.key == key) return &c;
  return nullptr;
}

doc::WireId CableLayer::hoveredWire() const noexcept {
  const Cable* c = hoverKey_ != 0 ? cableOf(hoverKey_) : nullptr;
  return c != nullptr ? c->wire : doc::WireId{};
}

void CableLayer::arrange(ui::LayoutPass& pass) {
  for (Badge* b : impl_->badges)
    for (const Cable& c : cables_)
      if (c.key == b->key()) pass.place(*b, c.badge);
}

void CableLayer::paint(ui::PaintContext& ctx) {
  const ui::Theme* th = ctx.res.theme;
  if (!th) return;
  const float modeAlpha = mode_ == CableMode::Fade ? 0.35f : 1.0f;
  const float s = ctx.scale.factor;
  ui::Vec2 pts[kSamples];
  for (const Cable& c : cables_) {
    if (ui::intersect(c.bounds, ctx.clip).empty()) continue;
    // TMM.13-fix2 (MM review H6): only the chain's audio path is drawn in full; a wire's cable that is not hovered (its
    // badge) or selected is drawn at 35 % (the panels' labels read over it), one tucked into the collapsed rack not at all.
    const bool focusCable = c.key == hoverKey_ || (c.wire.valid() && c.wire == selected_);
    if (c.tucked && !focusCable) continue;
    const float alpha = c.kind == Kind::Chain || focusCable ? modeAlpha : 0.35f * modeAlpha;
    cablePoints(c.a, c.da, c.b, c.db, s, pts);
    const bool off = c.status == doc::WireStatus::Inactive || c.status == doc::WireStatus::Invalid ||
                     c.status == doc::WireStatus::Refused;
    ui::Rgba col = off ? th->text.disabled : roleColour(th, c.role);
    const bool hot = c.key == hoverKey_;  // T4.16c: its badge is hovered
    const bool sel = c.wire.valid() && c.wire == selected_;
    if (sel || hot) col = ui::lerp(col, ui::rgba(255, 255, 255), sel ? 0.35f : 0.25f);
    col = ui::withAlpha(col, static_cast<uint32_t>((off ? 150.0f : 230.0f) * (hot ? 1.0f : alpha)));
    // TX.free-rack: a signal path (the chain's links, a wire into an input) is drawn a little heavier than modulation.
    const float base = c.input ? kCable + 0.5f : kCable;
    const float w = ctx.scale.exact(sel || hot ? base + 1.0f : base);
    if (c.through) {  // L22: the audio path through the device before this link, along its foot (1.5 px, under the joints)
      const ui::Vec2 run[2] = {c.t0, c.t1};
      ui::smoothPolyline(ctx.dl, run, 2, ui::withAlpha(col, static_cast<uint32_t>(170.0f * alpha)), ctx.scale.exact(1.5f));
    }
    if (off) {  // dashed: every other run of 4 points
      for (int i = 0; i + 1 < kSamples; i += 8) ui::smoothPolyline(ctx.dl, pts + i, static_cast<uint32_t>(std::min(5, kSamples - i)), col, w);
    } else {
      ui::smoothPolyline(ctx.dl, pts, kSamples, col, w);
    }
    // The ends: a plug on a docked jack / knob, an open ring where the end is a track header or a scrolled-away edge.
    for (const auto& [pt, docked] : {std::pair{c.a, c.aDocked}, std::pair{c.b, c.bDocked}}) {
      if (docked) ctx.dl.addCircleFilled(pt, ctx.scale.exact(2.5f), col);
      else ctx.dl.addCircleStroke(pt, ctx.scale.exact(3.0f), col, ctx.scale.exact(1.25f));
    }
    if (c.status == doc::WireStatus::Delayed && ctx.res.uiFonts) {
      // The automatic 1-sample delay (SPEC §15 2026-10-24: shown in the UI), as a tag beside the badge ("z-1": the UI
      // font has no superscript minus).
      const ui::IRect tag{c.badge.right() + ctx.scale.px(3.0f), c.badge.y - ctx.scale.px(1.0f), ctx.scale.px(26.0f),
                          c.badge.h + ctx.scale.px(2.0f)};
      paint::box(ctx.dl, tag, ctx.scale.exact(2.0f), ui::withAlpha(th->surface.raised, static_cast<uint32_t>(240 * modeAlpha)), ui::withAlpha(col, static_cast<uint32_t>(255 * modeAlpha)));  // legible when the cable is dimmed
      paint::text(ctx, ctx.res.uiFonts->small, tag, "z-1", ui::withAlpha(th->text.primary, static_cast<uint32_t>(255 * modeAlpha)),
                  ui::TextAlign::Center);
    }
  }
  if (!drag_.active) return;
  // The cable being dragged, and the knobs and inputs it can be dropped on.
  if (DeviceChainView* chain = impl_->chain()) {
    const Target hit = targetAt(drag_.pos);
    for (const DeviceChainView::ParamControl& pc : chain->paramControls()) {
      const bool on = pc.device == hit.device && pc.param == hit.param;
      paint::box(ctx.dl, pc.rect, ctx.scale.exact(3.0f), on ? ui::withAlpha(ui::signal_theme::kAccent, 60) : 0,
                 ui::withAlpha(ui::signal_theme::kAccent, on ? 255 : 120));
    }
    // TX.free-rack: the input sockets on the frames' left edges (an audio input, a meta module's inputs).
    for (const DeviceChainView::InputSocket& is : chain->inputSockets()) {
      if (is.device == drag_.from) continue;
      const bool on = is.device == hit.device && is.input == hit.param;
      const ui::Vec2 c{static_cast<float>(is.rect.x) + 0.5f * static_cast<float>(is.rect.w),
                       static_cast<float>(is.rect.y) + 0.5f * static_cast<float>(is.rect.h)};
      const float r = 0.5f * static_cast<float>(is.rect.w) - 1.0f;
      ctx.dl.addCircleFilled(c, r, on ? ui::withAlpha(roleColour(th, 1), 200) : ui::withAlpha(th->surface.sunken, 230));
      ctx.dl.addCircleStroke(c, r, ui::withAlpha(roleColour(th, 1), on ? 255 : 160), ctx.scale.exact(on ? 1.6f : 1.0f));
    }
    if (const auto a = chain->sourceEnd(drag_.from, drag_.port)) {
      const uint8_t db = hit.device.valid() && chain->isInput(hit.device, hit.param) ? static_cast<uint8_t>(DeviceChainView::EndDir::Left)
                                                                                     : static_cast<uint8_t>(DeviceChainView::EndDir::Up);
      cablePoints(a->at, static_cast<uint8_t>(a->dir), drag_.pos, db, s, pts);
      const ui::Rgba dc = roleColour(th, db == static_cast<uint8_t>(DeviceChainView::EndDir::Left) ? 1 : 0);
      ui::smoothPolyline(ctx.dl, pts, kSamples, ui::withAlpha(dc, 230), ctx.scale.exact(kCable));
      ctx.dl.addCircleFilled(drag_.pos, ctx.scale.exact(3.0f), dc);
    }
  }
}

CableLayer::Target CableLayer::targetAt(ui::Vec2 pos) const {
  if (DeviceChainView* chain = impl_->chain()) {
    for (const DeviceChainView::ParamControl& pc : chain->paramControls())
      if (pc.rect.contains(pos)) return {pc.device, pc.param};
    // TX.free-rack: an input socket (not the dragged device's own), or a block's input / control in the rack.
    for (const DeviceChainView::InputSocket& is : chain->inputSockets())
      if (is.device != drag_.from && is.rect.contains(pos)) return {is.device, is.input};
    if (project_patch::ProjectPatchView* rack = chain->freeRackView(); rack != nullptr && chain->freeRackOpen())
      if (const project_patch::ProjectPatchView::WireTarget t = rack->wireTargetAt(pos); t.device.valid()) return {t.device, t.param};
  }
  return {};
}

void CableLayer::beginDrag(doc::DeviceId from, std::string port, ui::Vec2 pos) {
  if (mode_ == CableMode::Hide) setMode(CableMode::Show);  // a new cable is always visible
  drag_.active = true;
  drag_.from = from;
  drag_.port = std::move(port);
  drag_.pos = pos;
  drag_.hoverTrack = {};
  drag_.hoverTicks = 0;
  drag_.cursor = ui::Cursor::Crosshair;  // over the jack it starts from
  // M4 review UI item 6: what a jack drag does, in the status line.
  ctx_.setStatus("Drop the cable on a knob to modulate it, or on an input socket (a frame's left edge) to feed it (another track's: hover its row first); Esc cancels.");
  setHittable(true);  // from the first move on, the layer takes the pointer (the jack strip may be rebuilt meanwhile)
  setFocusable(true);
  if (tree()) tree()->setFocus(this);  // Esc cancels
  drag_.timer = requestTimer(1.0 / 60.0);
  markDirty();
}

void CableLayer::endDrag(bool commit, ui::Vec2 pos) {
  if (!drag_.active) return;
  drag_.active = false;
  setHittable(false);
  setFocusable(false);
  if (drag_.timer != 0) cancelTimer(drag_.timer);
  drag_.timer = 0;
  markDirty();
  if (!commit) return;
  const Target t = targetAt(pos);
  if (!t.device.valid()) return;
  auto e = doc::ops::connectWire(*ctx_.doc(), ctx_.store().ids(), drag_.from, drag_.port, t.device, t.param);
  if (!e) {
    ctx_.setStatus(e.error().message);
    return;
  }
  if (ctx_.store().dispatch(e->command).ok() && !e->wires.empty()) select(e->wires.front());
}

void CableLayer::hoverBadge(uint64_t key, bool on) {
  const uint64_t next = on ? key : (hoverKey_ == key ? 0 : hoverKey_);
  if (next == hoverKey_ || !tree()) return;
  // Only the two cables repaint.
  for (const Cable& c : cables_)
    if (c.key == hoverKey_ || c.key == next) tree()->damage().add(c.bounds.toRect());
  hoverKey_ = next;
}

ui::Cursor CableLayer::cursorAt(ui::Vec2) const {
  // T4.16c: while a cable is dragged, what a release here would do: wire a knob (Crosshair), nothing over the
  // arrangement's rows (hovering them shows that track's chain: Arrow), else nothing at all (NotAllowed).
  if (!drag_.active) return ui::Cursor::Inherit;
  return drag_.cursor;
}

void CableLayer::updateDragCursor() {
  if (targetAt(drag_.pos).device.valid()) {
    drag_.cursor = ui::Cursor::Crosshair;
    return;
  }
  ArrangementView* arr = impl_->arrangement();
  drag_.cursor = arr != nullptr && shownNow(*arr) && arr->rect().contains(drag_.pos) ? ui::Cursor::Arrow : ui::Cursor::NotAllowed;
}

bool CableLayer::onPointer(const ui::PointerEvent& e) {
  if (!drag_.active) return false;
  if (e.type == ui::PointerType::Move) {
    drag_.pos = e.pos;
    updateDragCursor();
    markDirty();
    return true;
  }
  if (e.type == ui::PointerType::Up) {
    endDrag(true, e.pos);
    return true;
  }
  return e.type == ui::PointerType::Down;
}

bool CableLayer::onKey(const ui::KeyEvent& e) {
  if (drag_.active && e.down && e.key == ui::key::kEscape) {
    endDrag(false, drag_.pos);
    return true;
  }
  return false;
}

void CableLayer::onTimer(uint32_t id) {
  if (id != drag_.timer) return;
  drag_.timer = 0;
  if (!drag_.active) return;
  const float s = tree() ? tree()->scale() : 1.0f;
  const float edge = kEdge * s;
  // Over the arrangement's rows: hovering one shows its chain (spring-loaded); near the list's ends it scrolls.
  if (ArrangementView* arr = impl_->arrangement(); arr != nullptr && shownNow(*arr)) {
    const ui::IRect vp = arr->lanesViewport();
    const ui::IRect area = arr->rect();
    if (area.contains(drag_.pos)) {
      if (drag_.pos.y < static_cast<float>(vp.y) + edge && drag_.pos.y >= static_cast<float>(vp.y))
        arr->scrollToY(std::max(0, arr->scrollY() - static_cast<int32_t>(std::lround(12.0f * s))));
      else if (drag_.pos.y > static_cast<float>(vp.bottom()) - edge && drag_.pos.y <= static_cast<float>(vp.bottom()))
        arr->scrollToY(arr->scrollY() + static_cast<int32_t>(std::lround(12.0f * s)));
      const int32_t row = arr->rowAt(drag_.pos.y);
      const doc::TrackId t = row >= 0 ? arr->rows()[static_cast<size_t>(row)].id : doc::TrackId{};
      if (t != drag_.hoverTrack) {
        drag_.hoverTrack = t;
        drag_.hoverTicks = 0;
      } else if (t.valid() && ++drag_.hoverTicks >= kSpringTicks && ctx_.selectedTrack() != t) {
        ctx_.setSelectedTrack(t);
      }
    } else {
      drag_.hoverTrack = {};
    }
  }
  // Near the left / right end of the shown chain it scrolls sideways.
  if (DeviceChainView* chain = impl_->chain()) {
    const ui::IRect vp = chain->chainViewport();
    if (vp.contains(drag_.pos)) {
      if (drag_.pos.x < static_cast<float>(vp.x) + edge) chain->scrollChainBy(-static_cast<int32_t>(std::lround(12.0f * s)));
      else if (drag_.pos.x > static_cast<float>(vp.right()) - edge) chain->scrollChainBy(static_cast<int32_t>(std::lround(12.0f * s)));
    }
  }
  updateDragCursor();  // the chain may have scrolled (or switched tracks) under a still pointer
  markDirty();
  drag_.timer = requestTimer(1.0 / 60.0);
}

void CableLayer::onCaptureLost() {}

void CableLayer::onInteractionCancelled() { endDrag(false, drag_.pos); }

CableLayer& installWiring(Shell& shell) {
  ViewContext& ctx = shell.context();
  auto& layer = shell.slot(ShellSlot::Cables).add<CableLayer>(ctx, shell);
  CableLayer* l = &layer;
  ctx.setViewAction("view.cycleCables", [l] { l->cycleMode(); });
  ctx.setViewAction("wire.delete", [l] { l->deleteSelected(); }, [l] { return l->selected().valid(); });
  if (DeviceChainView* chain = findDeviceChain(shell)) {
    ui::UiTree* tree = &shell.tree();
    chain->onJack = [l, tree](doc::DeviceId device, std::string_view port, const ui::PointerEvent& e) {
      if (e.type == ui::PointerType::Down) {
        l->beginDrag(device, std::string(port), e.pos);
        return;
      }
      if (!l->dragging()) return;
      if (e.type == ui::PointerType::Move) {
        // Hand the pointer to the layer: the jack strip may be rebuilt (another track's chain) during the drag.
        tree->releaseCapture();
        (void)l->onPointer(e);
      } else if (e.type == ui::PointerType::Up) {
        (void)l->onPointer(e);
      }
    };
  }
  return layer;
}

CableLayer* findCableLayer(Shell& shell) noexcept {
  return dynamic_cast<CableLayer*>(shell.slot(ShellSlot::Cables).find("cable-layer"));
}

}  // namespace pq::views
