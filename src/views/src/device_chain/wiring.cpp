// Cross-device wiring in the views (T4.7): pq/views/device_chain/wiring.h.
#include "pq/views/device_chain/wiring.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "../common/paint.h"
#include "pq/document/edit_ops.h"
#include "pq/ui/lines.h"
#include "pq/ui/overlay.h"
#include "pq/ui/theme.h"
#include "pq/views/arrangement/arrangement.h"
#include "pq/views/device_chain/device_chain.h"
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
constexpr float kCable = 2.0f;       // logical px: cable thickness
constexpr int kSamples = 40;         // points per cable
constexpr uint32_t kSpringTicks = 21;   // drag ticks (1/60 s) hovering a track row before its chain shows (0.35 s)
constexpr float kEdge = 24.0f;       // logical px: auto-scroll zone at a list's edge
constexpr float kAmountDragPx = 150.0f;  // logical px of vertical drag per unit of amount

// Cables are signals (PQ Meter's palette): param rate blue, audio green, voice amber.
ui::Rgba rateColour(uint8_t rate) noexcept {
  switch (rate) {
    case 2: return ui::signal_theme::kBands[1];
    case 1: return ui::signal_theme::kWaveform;
    default: return ui::signal_theme::kAccent;
  }
}

// A cubic bezier that leaves the jack upwards and drops into the knob from above (a sagging patch cable).
void cablePoints(ui::Vec2 a, ui::Vec2 b, float scale, ui::Vec2 (&pts)[kSamples]) {
  const float lift = (28.0f + 0.2f * std::fabs(b.x - a.x)) * scale;
  const ui::Vec2 c1{a.x, a.y - lift}, c2{b.x, b.y - lift};
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
  std::shared_ptr<const doc::ResolvedDevice> hold;  // T4.4-fix1: a user device plays its own patch
  const graph::Patch* p = lib != nullptr ? lib->patchOf(d, hold) : nullptr;
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
  const doc::DeviceInfo* info = ctx.deviceLibrary() != nullptr ? ctx.deviceLibrary()->catalog().find(l.device->patch) : nullptr;
  return info != nullptr ? info->name : l.device->patch;
}

std::string paramLabel(const ViewContext& ctx, const doc::Project& p, doc::DeviceId id, const std::string& param) {
  const doc::DeviceLoc l = doc::findDevice(p, id);
  std::shared_ptr<const doc::ResolvedDevice> hold;  // T4.4-fix1: an edited device's own labels
  const doc::DeviceInfo* info = l.device != nullptr && ctx.deviceLibrary() != nullptr
                                    ? ctx.deviceLibrary()->infoOf(*l.device, hold) : nullptr;
  const doc::DeviceParamInfo* pi = info != nullptr ? info->param(param) : nullptr;
  return pi != nullptr ? pi->label : param;
}

// ---- the badge in the middle of a cable: select, drag the amount, menu, Delete -----------------------------------------

class Badge final : public ui::Widget {
public:
  Badge(CableLayer& layer, ViewContext& ctx, doc::WireId wire)
      : Widget("wire[" + std::to_string(wire.value) + "]"), layer_(layer), ctx_(ctx), wire_(wire) {
    setFocusable(true);
  }
  doc::WireId wire() const noexcept { return wire_; }
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
    const doc::Wire* w = doc::findWire(*ctx_.doc(), wire_);
    if (w == nullptr) return;
    const ui::IRect r = rect();
    const ui::Vec2 c{static_cast<float>(r.x) + 0.5f * static_cast<float>(r.w), static_cast<float>(r.y) + 0.5f * static_cast<float>(r.h)};
    const float rad = 0.5f * static_cast<float>(std::min(r.w, r.h)) - ctx.scale.exact(1.0f);
    const bool off = status_ == doc::WireStatus::Inactive || status_ == doc::WireStatus::Invalid || status_ == doc::WireStatus::Refused;
    const ui::Rgba col = off ? th->text.disabled : rateColour(rate_);
    const float alpha = layer_.mode() == CableMode::Fade ? 0.4f : 1.0f;
    ctx.dl.addCircleFilled(c, rad, ui::withAlpha(ui::lerp(th->surface.raised, col, 0.25f), static_cast<uint32_t>(240 * alpha)));
    // The amount as an arc from the top (clockwise positive), like a small bipolar knob.
    const float a0 = -1.5707963f, a1 = a0 + 3.14159265f * static_cast<float>(w->amount);
    if (std::fabs(a1 - a0) > 1e-3f)
      ctx.dl.addArcStroke(c, rad - ctx.scale.exact(1.5f), std::min(a0, a1), std::max(a0, a1),
                          ui::withAlpha(col, static_cast<uint32_t>(255 * alpha)), ctx.scale.exact(2.5f));
    // T4.16c: hovered (or selected), the ring is lit and thicker; the layer lights the cable too.
    ctx.dl.addCircleStroke(c, rad, ui::withAlpha(selected_ || focused() || hovered() ? th->accent.focus : col, static_cast<uint32_t>(220 * alpha)),
                           ctx.scale.exact(selected_ || hovered() ? 1.6f : 1.0f));
  }
  // T4.16c: drag up / down sets the amount.
  ui::Cursor cursorAt(ui::Vec2) const override { return ui::Cursor::ResizeNS; }
  void onHoverChanged(bool h) override {
    markDirty();
    layer_.hoverBadge(wire_, h);
  }

  bool onPointer(const ui::PointerEvent& e) override {
    switch (e.type) {
      case ui::PointerType::Down:
        if (tree()) tree()->setFocus(this);
        layer_.select(wire_);
        if (e.button == ui::PointerButton::Right) {
          openMenu(e.pos);
          return true;
        }
        if (e.button != ui::PointerButton::Left) return false;
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
    const doc::Project& p = *ctx_.doc();
    if (const doc::Wire* w = doc::findWire(p, wire_)) {
      char amount[32];
      std::snprintf(amount, sizeof amount, "%+.0f %%", 100.0 * w->amount);
      tip_ = deviceName(ctx_, p, w->from) + " " + w->port + " \xE2\x86\x92 " + deviceName(ctx_, p, w->to) + " " +
             paramLabel(ctx_, p, w->to, w->param) + "  " + amount;
      switch (status_) {
        case doc::WireStatus::Delayed: tip_ += "  (closes a loop: delayed by 1 sample)"; break;
        case doc::WireStatus::Inactive: tip_ += "  (an end is switched off)"; break;
        case doc::WireStatus::Invalid: tip_ += "  (the device has no such port)"; break;
        case doc::WireStatus::Refused: tip_ += "  (not played: a loop through two instrument tracks)"; break;
        default: break;
      }
    }
    return tip_;
  }
  ui::InfoText info() const override {
    return {"Modulation Wire", "Drag up or down to set how far it moves the knob (Shift: fine); double-click: 50 %. "
                               "Delete removes it; right-click for more. z-1: the wire closes a loop "
                               "and is delayed by one sample."};
  }

private:
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
    items.push_back(ui::MenuItem::item("Invert Amount", [ctx, id] {
      if (const doc::Wire* x = doc::findWire(*ctx->doc(), id))
        if (auto e = doc::ops::setWireAmount(*ctx->doc(), id, -x->amount); e && !e->empty()) (void)ctx->store().dispatch(e->command);
    }));
    items.push_back(ui::MenuItem::item("Reset Amount (50 %)", [ctx, id] {
      if (auto e = doc::ops::setWireAmount(*ctx->doc(), id, doc::kDefaultWireAmount); e && !e->empty())
        (void)ctx->store().dispatch(e->command);
    }));
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
  if (s == impl_->statesFrom) return;
  impl_->statesFrom = s;
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
  if (ctx_.doc() != impl_->statesFrom && ctx_.deviceLibrary() != nullptr) rebuildStatus();
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
  for (const doc::Wire& w : doc::wiresOf(p)) {
    const doc::DeviceLoc from = doc::findDevice(p, w.from), to = doc::findDevice(p, w.to);
    if (from.device == nullptr || to.device == nullptr) continue;
    Cable c;
    c.wire = w.id;
    std::optional<ui::Vec2> a = chain != nullptr ? chain->jackPoint(w.from, w.port, &c.aDocked) : std::nullopt;
    std::optional<ui::Vec2> b = chain != nullptr ? chain->paramPoint(w.to, w.param, &c.bDocked) : std::nullopt;
    if (!a && !b) continue;  // neither end in the shown chain: not drawn (the project patch view shows every wire, T4.13)
    if (!a) a = trackAnchor(from.track->id);
    if (!b) b = trackAnchor(to.track->id);
    if (!a || !b) continue;
    c.a = *a;
    c.b = *b;
    c.status = statusOf(w.id);
    c.rate = exportRate(*from.device, ctx_.deviceLibrary(), w.port);
    ui::Vec2 pts[kSamples];
    cablePoints(c.a, c.b, s, pts);
    c.bounds = boundsOf(pts, kSamples, 3.0f * s + 2.0f);
    const ui::Vec2 mid = pts[kSamples / 2];
    const int32_t bd = static_cast<int32_t>(std::lround(kBadge * s));
    c.badge = {static_cast<int32_t>(std::lround(mid.x)) - bd / 2, static_cast<int32_t>(std::lround(mid.y)) - bd / 2, bd, bd};
    c.bounds = unite(c.bounds, ui::IRect{c.badge.x, c.badge.y, c.badge.w + static_cast<int32_t>(std::lround(32.0f * s)), c.badge.h});
    next.push_back(c);
  }
  // Damage what moved: per wire the old and the new cable (a cable that did not move costs nothing).
  bool moved = false;
  ui::Damage& dmg = tree()->damage();
  auto same = [](const Cable& x, const Cable& y) {
    return x.a.x == y.a.x && x.a.y == y.a.y && x.b.x == y.b.x && x.b.y == y.b.y && x.status == y.status && x.rate == y.rate &&
           x.aDocked == y.aDocked && x.bDocked == y.bDocked;
  };
  for (const Cable& n : next) {
    const auto o = std::find_if(cables_.begin(), cables_.end(), [&](const Cable& c) { return c.wire == n.wire; });
    if (o != cables_.end() && same(*o, n)) continue;
    moved = true;
    if (o != cables_.end()) dmg.add(o->bounds.toRect());
    dmg.add(n.bounds.toRect());
  }
  for (const Cable& o : cables_)
    if (std::none_of(next.begin(), next.end(), [&](const Cable& c) { return c.wire == o.wire; })) {
      moved = true;
      dmg.add(o.bounds.toRect());
    }
  cables_ = std::move(next);
  // Badges: one per drawn cable.
  bool badgesChanged = false;
  for (size_t i = 0; i < impl_->badges.size();) {
    Badge* b = impl_->badges[i];
    if (std::none_of(cables_.begin(), cables_.end(), [&](const Cable& c) { return c.wire == b->wire(); })) {
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
      if (x->wire() == c.wire) b = x;
    if (b == nullptr) {
      b = &add<Badge>(*this, ctx_, c.wire);
      impl_->badges.push_back(b);
      badgesChanged = true;
    }
    b->setLook(c.rate, c.status, c.wire == selected_);
    if (!(b->rect() == c.badge)) badgesChanged = true;
  }
  if (badgesChanged) invalidateArrange();
  return moved || badgesChanged;
}

void CableLayer::arrange(ui::LayoutPass& pass) {
  for (Badge* b : impl_->badges)
    for (const Cable& c : cables_)
      if (c.wire == b->wire()) pass.place(*b, c.badge);
}

void CableLayer::paint(ui::PaintContext& ctx) {
  const ui::Theme* th = ctx.res.theme;
  if (!th) return;
  const float alpha = mode_ == CableMode::Fade ? 0.35f : 1.0f;
  const float s = ctx.scale.factor;
  ui::Vec2 pts[kSamples];
  for (const Cable& c : cables_) {
    if (ui::intersect(c.bounds, ctx.clip).empty()) continue;
    cablePoints(c.a, c.b, s, pts);
    const bool off = c.status == doc::WireStatus::Inactive || c.status == doc::WireStatus::Invalid ||
                     c.status == doc::WireStatus::Refused;
    ui::Rgba col = off ? th->text.disabled : rateColour(c.rate);
    const bool hot = c.wire == hoverWire_;  // T4.16c: its badge is hovered
    if (c.wire == selected_ || hot) col = ui::lerp(col, ui::rgba(255, 255, 255), c.wire == selected_ ? 0.35f : 0.25f);
    col = ui::withAlpha(col, static_cast<uint32_t>((off ? 150.0f : 230.0f) * (hot ? 1.0f : alpha)));
    const float w = ctx.scale.exact(c.wire == selected_ || hot ? kCable + 1.0f : kCable);
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
      paint::box(ctx.dl, tag, ctx.scale.exact(2.0f), ui::withAlpha(th->surface.raised, static_cast<uint32_t>(240 * alpha)), col);
      paint::text(ctx, ctx.res.uiFonts->small, tag, "z-1", ui::withAlpha(th->text.primary, static_cast<uint32_t>(255 * alpha)),
                  ui::TextAlign::Center);
    }
  }
  if (!drag_.active) return;
  // The cable being dragged, and the knobs it can be dropped on.
  if (DeviceChainView* chain = impl_->chain()) {
    const Target hit = targetAt(drag_.pos);
    for (const DeviceChainView::ParamControl& pc : chain->paramControls()) {
      const bool on = pc.device == hit.device && pc.param == hit.param;
      paint::box(ctx.dl, pc.rect, ctx.scale.exact(3.0f), on ? ui::withAlpha(ui::signal_theme::kAccent, 60) : 0,
                 ui::withAlpha(ui::signal_theme::kAccent, on ? 255 : 120));
    }
    if (const auto a = chain->jackPoint(drag_.from, drag_.port)) {
      cablePoints(*a, drag_.pos, s, pts);
      ui::smoothPolyline(ctx.dl, pts, kSamples, ui::withAlpha(rateColour(1), 230), ctx.scale.exact(kCable));
      ctx.dl.addCircleFilled(drag_.pos, ctx.scale.exact(3.0f), rateColour(1));
    }
  }
}

CableLayer::Target CableLayer::targetAt(ui::Vec2 pos) const {
  if (DeviceChainView* chain = impl_->chain())
    for (const DeviceChainView::ParamControl& pc : chain->paramControls())
      if (pc.rect.contains(pos)) return {pc.device, pc.param};
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

void CableLayer::hoverBadge(doc::WireId w, bool on) {
  const doc::WireId next = on ? w : (hoverWire_ == w ? doc::WireId{} : hoverWire_);
  if (next == hoverWire_ || !tree()) return;
  // Only the two cables repaint.
  for (const Cable& c : cables_)
    if (c.wire == hoverWire_ || c.wire == next) tree()->damage().add(c.bounds.toRect());
  hoverWire_ = next;
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
