// The circuit view at the Track level (T5.x-circuit-chain): pq/views/circuit/track_circuit.h.

#include "pq/views/circuit/track_circuit.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

#include "../common/paint.h"
#include "pq/ui/controls.h"
#include "pq/ui/theme.h"
#include "pq/views/circuit/device_circuit.h"

namespace pq::views::circuit {

using ui::Rect;
using ui::Rgba;
using ui::Vec2;

namespace {

bool overlaps(const Rect& r, const ui::IRect& clip) noexcept {
  return r.x1 >= static_cast<float>(clip.x) && r.x0 <= static_cast<float>(clip.right()) && r.y1 >= static_cast<float>(clip.y) &&
         r.y0 <= static_cast<float>(clip.bottom());
}

ui::IRect toIRect(const Rect& r) noexcept {
  const auto x0 = static_cast<int32_t>(std::floor(r.x0)), y0 = static_cast<int32_t>(std::floor(r.y0));
  return {x0, y0, static_cast<int32_t>(std::ceil(r.x1)) - x0, static_cast<int32_t>(std::ceil(r.y1)) - y0};
}

Rect worldRect(const CircuitCanvas& c, float x0, float y0, float x1, float y1) noexcept {
  const Vec2 a = c.toDevice({x0, y0}), b = c.toDevice({x1, y1});
  return {std::round(a.x), std::round(a.y), std::round(b.x), std::round(b.y)};
}

// The kind's accent: instruments the value colour, audio effects the signal colour, racks the solo blue, chains the
// symbol outline.
Rgba accentOf(const ui::Theme& th, const ChainBox& b) noexcept {
  switch (b.kind) {
    case ChainBoxKind::Device: return b.instrument ? th.accent.primary : th.circuit.wireAudio;
    case ChainBoxKind::Rack: return th.accent.solo;
    case ChainBoxKind::Chain: return th.circuit.symbolStroke;
  }
  return th.circuit.symbolStroke;
}

CircuitCanvas::View fitRect(const ui::IRect& r, float us, float x0, float y0, float x1, float y1, double maxZoom) {
  CircuitCanvas::View v;
  const double margin = 24.0;
  const double lw = r.w / static_cast<double>(us), lh = r.h / static_cast<double>(us);
  double z = std::min(lw / std::max(1.0, static_cast<double>(x1 - x0) + 2 * margin), lh / std::max(1.0, static_cast<double>(y1 - y0) + 2 * margin));
  z = std::clamp(z, CircuitCanvas::kMinZoom, maxZoom);
  v.zoom = z;
  v.x = static_cast<double>(x0 + x1) * 0.5 - lw / z * 0.5;
  v.y = static_cast<double>(y0 + y1) * 0.5 - lh / z * 0.5;
  return v;
}

}  // namespace

// ---- painting the boundaries ----------------------------------------------------------------------------------------

void paintChainBoxes(ui::PaintContext& ctx, const CircuitCanvas& canvas, const ChainCircuit& c, float dx, float dy) {
  if (ctx.res.theme == nullptr) return;
  const ui::Theme& th = *ctx.res.theme;
  ui::DrawList& dl = ctx.dl;
  const ui::IRect clip = ui::intersect(ctx.clip, canvas.rect());
  const float s = canvas.deviceScale(), us = ctx.scale.factor;
  const float line = std::max(1.0f, std::round(us));
  const float bar = std::max(2.0f, std::round(3.0f * us));
  const bool names = ctx.res.uiFonts != nullptr && ctx.res.text != nullptr;
  const ui::FontId font = names ? ctx.res.uiFonts->small : ui::kNoFont;
  const float px = names ? static_cast<float>(ctx.res.text->metrics(font).pixelSize) : 12.0f;
  const float strip = chain_metrics::kTitle * s;
  // Titles that do not fit their strip (zoomed out) go in pills at the boxes' top-left corners, outer boxes first;
  // a pill that would overlap one already placed is left out.
  std::vector<Rect> pills;
  auto pill = [&](const Rect& r, const std::string& text, Rgba col) {
    if (!names || r.x1 - r.x0 < 28.0f * us || r.y1 - r.y0 < px + 4.0f) return;
    const float pad = std::round(3.0f * us), h = std::round(px + 4.0f * us);
    const int32_t avail = static_cast<int32_t>(r.x1 - r.x0 - 2.0f * pad - 2.0f);
    const ui::TextRun run = ctx.res.text->shapeTruncated(font, text, avail);
    if (run.advance <= 0) return;
    const Rect box{r.x0 + 1.0f, r.y0 + 1.0f, r.x0 + 1.0f + static_cast<float>(run.advance) + 2.0f * pad, r.y0 + 1.0f + h};
    for (const Rect& q : pills)
      if (!(box.x1 <= q.x0 || q.x1 <= box.x0 || box.y1 <= q.y0 || q.y1 <= box.y0)) return;
    pills.push_back(box);
    dl.addRoundedRectFilled(box, 2.0f * us, ui::withAlpha(th.surface.sunken, 230));
    ui::TextSystem::drawRun(dl, run, {box.x0 + pad, static_cast<float>(paint::capCentredBaseline(static_cast<int32_t>(box.y0), static_cast<int32_t>(h), static_cast<uint32_t>(px)))}, col);
  };

  // The track's band (track mode): its colour, its name.
  if (!c.rack.valid() && c.track.valid()) {
    const Rect r = worldRect(canvas, c.x0 + dx, c.y0 + dy, c.x1 + dx, c.y1 + dy);
    if (overlaps(r, clip)) {
      const Rgba col = ui::hex(c.trackColour);
      dl.addRectFilled(r, ui::lerp(th.circuit.background, col, 0.06f));
      dl.addRectFilled({r.x0, r.y0, r.x0 + bar, r.y1}, col);
      const float h = std::max(std::min(24.0f * s, r.y1 - r.y0), 0.0f);
      if (names && h >= px + 2.0f) {
        paint::text(ctx, font, toIRect({r.x0 + bar + 6.0f, r.y0, std::min(r.x1, r.x0 + 600.0f * us), r.y0 + h}), c.trackName,
                    th.circuit.symbolText, ui::TextAlign::Left);
      } else if (names) {  // zoomed out: the name just above the band (its strip is too thin to hold it)
        const float lh = std::round(px + 4.0f * us);
        paint::text(ctx, font, toIRect({r.x0, r.y0 - lh - 2.0f * us, std::min(r.x1, r.x0 + 600.0f * us), r.y0 - 2.0f * us}), c.trackName,
                    th.circuit.symbolText, ui::TextAlign::Left);
      }
    }
  }
  for (const ChainBox& b : c.boxes) {
    const Rect r = worldRect(canvas, b.x0 + dx, b.y0 + dy, b.x1 + dx, b.y1 + dy);
    if (!overlaps(r, clip) || r.x1 - r.x0 < 2.0f || r.y1 - r.y0 < 2.0f) continue;
    const Rgba accent = accentOf(th, b);
    const float hs = std::min(strip, r.y1 - r.y0);
    switch (b.kind) {
      case ChainBoxKind::Device:
        dl.addRectFilled(r, ui::lerp(th.circuit.background, th.circuit.subcircuitFill, 0.45f));
        dl.addRectFilled({r.x0, r.y0, r.x1, r.y0 + hs}, ui::lerp(th.circuit.background, accent, 0.22f));
        dl.addRectStroke(r, ui::mulAlpha(accent, 0.9f), line);
        dl.addRectFilled({r.x0, r.y0, r.x0 + bar, r.y1}, accent);
        break;
      case ChainBoxKind::Rack:
        dl.addRectFilled(r, ui::lerp(th.circuit.background, accent, 0.05f));
        dl.addRectFilled({r.x0, r.y0, r.x1, r.y0 + hs}, ui::lerp(th.circuit.background, accent, 0.18f));
        dl.addRectStroke(r, ui::mulAlpha(accent, 0.9f), line);
        dl.addRectFilled({r.x0, r.y0, r.x0 + bar, r.y1}, accent);
        break;
      case ChainBoxKind::Chain:
        dl.addRectFilled(r, ui::mulAlpha(th.circuit.background, 0.6f));
        dl.addRectStroke(r, ui::mulAlpha(accent, 0.45f), line);
        break;
    }
    if (!names) continue;
    const Rgba textCol = b.kind == ChainBoxKind::Chain ? th.circuit.pinText : th.circuit.symbolText;
    if (hs >= px + 2.0f) {  // the title in its strip, the kind after it
      const float x0 = r.x0 + (b.kind == ChainBoxKind::Chain ? 6.0f * us : bar + 6.0f * us);
      const ui::IRect box = toIRect({x0, r.y0, r.x1 - 4.0f * us, r.y0 + hs});
      const int32_t adv = paint::text(ctx, font, box, b.title, textCol, ui::TextAlign::Left);
      if (b.kind != ChainBoxKind::Chain && adv > 0 && adv + 12 < box.w) {
        ui::IRect rest = box;
        rest.x += adv + static_cast<int32_t>(std::round(10.0f * us));
        rest.w -= adv + static_cast<int32_t>(std::round(10.0f * us));
        paint::text(ctx, font, rest, b.kindLabel, th.circuit.pinText, ui::TextAlign::Left);
      }
    } else if (b.kind != ChainBoxKind::Chain) {
      pill(r, b.title, textCol);
    }
  }
}

void paintChainVeils(ui::PaintContext& ctx, const CircuitCanvas& canvas, const ChainCircuit& c, float dx, float dy) {
  if (ctx.res.theme == nullptr) return;
  const ui::Theme& th = *ctx.res.theme;
  const ui::IRect clip = ui::intersect(ctx.clip, canvas.rect());
  const float s = canvas.deviceScale();
  const bool names = ctx.res.uiFonts != nullptr && ctx.res.text != nullptr;
  for (const ChainBox& b : c.boxes) {
    if (b.active && !b.missing) continue;
    const Rect r = worldRect(canvas, b.x0 + dx, b.y0 + dy, b.x1 + dx, b.y1 + dy);
    if (!overlaps(r, clip)) continue;
    const float hs = std::min(chain_metrics::kTitle * s, r.y1 - r.y0);
    ctx.dl.addRectFilled({r.x0, r.y0 + hs, r.x1, r.y1}, ui::mulAlpha(th.circuit.background, 0.6f));
    if (names && hs >= static_cast<float>(ctx.res.text->metrics(ctx.res.uiFonts->small).pixelSize) + 2.0f)
      paint::text(ctx, ctx.res.uiFonts->small, toIRect({r.x0, r.y0, r.x1 - 6.0f, r.y0 + hs}), b.missing ? "missing" : "off",
                  th.circuit.pinText, ui::TextAlign::Right);
  }
}

// ---- the canvas -----------------------------------------------------------------------------------------------------

ChainCanvas::ChainCanvas() : CircuitCanvas("chain-canvas") {
  setPinStubsNearOnly(true);
  setTerminalPillMinZoom(kFarZoom);  // zoomed far out: the boundaries' titles, not every terminal of every device
}

void ChainCanvas::setCircuit(std::shared_ptr<const ChainCircuit> c, bool fit, bool keepTiles) {
  circuit_ = std::move(c);
  setScene(circuit_ ? std::shared_ptr<const CircuitScene>(circuit_, circuit_->scene.get()) : nullptr, fit, keepTiles);
}

void ChainCanvas::frameBox(uint32_t box) {
  pendingFrame_ = box;
  if (rect().w > 0 && rect().h > 0 && circuit_ && box < circuit_->boxes.size()) {
    const ChainBox& b = circuit_->boxes[box];
    setView(fitRect(rect(), tree() ? tree()->scale() : 1.0f, b.x0, b.y0, b.x1, b.y1, 1.5));
    pendingFrame_ = kNoBox;
  }
  markDirty();
}

void ChainCanvas::arrange(ui::LayoutPass& pass) {
  CircuitCanvas::arrange(pass);
  if (pendingFrame_ != kNoBox) frameBox(pendingFrame_);
}

uint32_t ChainCanvas::deviceBoxAt(Vec2 device) const noexcept {
  if (!circuit_) return kNoBox;
  const graph::Point2 w = toWorld(device);
  return circuit_->deviceBoxAt(w.x, w.y);
}

bool ChainCanvas::onPointer(const ui::PointerEvent& e) {
  if (e.type == ui::PointerType::Down && e.button == ui::PointerButton::Left && e.clickCount >= 2 && (e.mods & ui::mod::kAlt) == 0) {
    const uint32_t b = deviceBoxAt(e.pos);
    if (b != kNoBox && onOpenBox) {
      onOpenBox(b);
      return true;
    }
  }
  return CircuitCanvas::onPointer(e);
}

ui::InfoText ChainCanvas::info() const {
  return {"Track Circuit", "The track's whole device chain as one circuit, read-only. Double-click a device (or Enter on a selected node) to "
                           "edit its circuit; F fits the track; Ctrl+wheel zooms, the wheel pans; Esc closes."};
}

void ChainCanvas::paintUnderlay(ui::PaintContext& ctx) {
  if (circuit_) paintChainBoxes(ctx, *this, *circuit_);
}

void ChainCanvas::paintOverlay(ui::PaintContext& ctx) {
  if (!circuit_) return;
  paintChainVeils(ctx, *this, *circuit_);
  // The track's own terminals keep their names at the Far level too (the canvas names terminals from Mid up here).
  if (lod() != Lod::Far || ctx.res.text == nullptr || ctx.res.uiFonts == nullptr || ctx.res.theme == nullptr) return;
  const ui::Theme& th = *ctx.res.theme;
  const CircuitScene& sc = *circuit_->scene;
  const ui::FontId font = ctx.res.uiFonts->small;
  const float pad = std::round(ctx.scale.exact(3.0f)), lh = std::round(ctx.scale.exact(13.0f));
  for (const uint32_t v : {circuit_->inputNode, circuit_->outputNode}) {
    if (v >= sc.nodes.size()) continue;
    const SceneNode& n = sc.nodes[v];
    const bool left = v == circuit_->inputNode;
    const Vec2 c = toDevice({left ? n.x + n.w : n.x, n.y + n.h * 0.5f});
    const ui::TextRun run = ctx.res.text->shape(font, sc.specs[n.spec].title);
    const float w = static_cast<float>(run.advance) + 2.0f * pad;
    // Kept inside the canvas (fitted far out, the margin around the band is a few pixels).
    const float lo = static_cast<float>(rect().x) + 2.0f, hi = static_cast<float>(rect().right()) - w - 2.0f;
    const float x0 = std::round(std::clamp(left ? c.x - w - 2.0f : c.x + 2.0f, lo, std::max(lo, hi))), y0 = std::round(c.y - lh * 0.5f);
    ctx.dl.addRoundedRectFilled({x0, y0, x0 + w, y0 + lh}, ctx.scale.exact(2.0f), ui::withAlpha(th.surface.sunken, 230));
    ui::TextSystem::drawRun(ctx.dl, run, {x0 + pad, std::round(y0 + lh - std::round(ctx.scale.exact(3.5f)))}, th.circuit.symbolText);
  }
}

// ---- the view -------------------------------------------------------------------------------------------------------

TrackCircuitView::TrackCircuitView(ViewContext& ctx, doc::TrackId track) : Popover("track-circuit"), ctx_(ctx), track_(track) {
  setPadding(ui::Insets::all(4));
  setGap(4);
  auto& bar = add<ui::Widget>("bar");
  bar.setLayout(ui::LayoutKind::Row);
  bar.setHeight(ui::Length::fixed(20));
  bar.setGap(4);
  // The breadcrumb's levels: Track (here, current), Device (the focused / selected device's editor).
  auto& here = bar.add<ui::Toggle>("track", "Track", ui::ToggleStyle::Accent, true);
  here.setWidth(ui::Length::fixed(48));
  here.setTooltip("The whole track's chain (this view)");
  here.onToggle = [&here](bool) { here.setOn(true); };
  edit_ = &bar.add<ui::Button>("device", "Device");
  edit_->setWidth(ui::Length::fixed(52));
  edit_->setTooltip("Edit the focused or selected device's circuit (double-click a device does the same)");
  edit_->onClick = [this] { (void)editDevice(currentDevice()); };
  title_ = &bar.add<ui::Label>("title", "", ui::FontRoleId::Small, ui::TextTone::Primary);
  title_->setWidth(ui::Length::flex());
  auto& fit = bar.add<ui::Button>("fit", "Fit");
  fit.setWidth(ui::Length::fixed(36));
  fit.onClick = [this] { canvas_->fitAll(); };
  auto& close = bar.add<ui::Button>("close", "Close");
  close.setWidth(ui::Length::fixed(48));
  close.onClick = [this] { closeSoon(); };
  canvas_ = &add<ChainCanvas>();
  canvas_->setHeight(ui::Length::flex());
  status_ = &add<ui::Label>("status", "", ui::FontRoleId::Small, ui::TextTone::Secondary);
  status_->setHeight(ui::Length::fixed(16));
  canvas_->onOpenBox = [this](uint32_t box) {
    const ChainBox& b = circuit_->boxes[box];
    (void)editDevice(b.device);
  };
  canvas_->onSelectionChanged = [this] { updateTitle(); };
  sub_ = ctx_.subscribe([this](const doc::StoreEvent& e) { onDocument(e); });
  rebuild(true);
}

TrackCircuitView::~TrackCircuitView() = default;

void TrackCircuitView::rebuild(bool fit) {
  const auto t0 = std::chrono::steady_clock::now();
  ChainOptions o;
  o.devices = ctx_.deviceLibrary();
  o.cache = &cache_;
  circuit_ = std::make_shared<const ChainCircuit>(buildTrackCircuit(*ctx_.doc(), track_, o));
  const CircuitCanvas::View keep = canvas_->view();
  const bool first = rebuilds_ == 0;
  canvas_->setCircuit(circuit_, false, !first);
  if (fit) canvas_->fitAll();  // the whole track (not the canvas's readable open on the outputs: this is the overview)
  else if (!first) canvas_->setView(keep);
  ++rebuilds_;
  lastBuildMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  if (focus_.valid() && circuit_->boxOf(focus_) == kNoBox) focus_ = {};
  uint32_t devices = 0;
  for (const ChainBox& b : circuit_->boxes) devices += b.kind == ChainBoxKind::Device ? 1u : 0u;
  setStatus("Read-only: double-click a device (or select it and press Enter) to edit its circuit. " + std::to_string(devices) +
            (devices == 1 ? " device, " : " devices, ") + std::to_string(circuit_->scene->nodes.size()) + " nodes.");
  updateTitle();
}

bool TrackCircuitView::focusDevice(doc::DeviceId device) {
  const uint32_t b = circuit_->boxOf(device);
  if (b == kNoBox) return false;
  focus_ = device;
  canvas_->frameBox(b);
  updateTitle();
  return true;
}

doc::DeviceId TrackCircuitView::currentDevice() const {
  for (const uint32_t v : canvas_->selection().nodes) {
    uint32_t b = v < circuit_->boxOfNode.size() ? circuit_->boxOfNode[v] : kNoBox;
    while (b != kNoBox && circuit_->boxes[b].kind == ChainBoxKind::Chain) b = circuit_->boxes[b].parent;
    if (b != kNoBox) return circuit_->boxes[b].device;
  }
  return focus_;
}

bool TrackCircuitView::editDevice(doc::DeviceId device) {
  const doc::DeviceLoc loc = doc::findDevice(*ctx_.doc(), device);
  if (loc.device == nullptr) {
    setStatus("Select a device (or double-click one) to edit its circuit.");
    return false;
  }
  if (loc.device->rack) {
    setStatus("A rack has no circuit of its own to edit: double-click a device inside it (its chains are edited in the Device view).");
    return false;
  }
  // This view closes first (an overlay closes everything opened above it; the widget itself is destroyed at the next
  // frame, so closing from inside its own callback is safe), then the editor opens in its place.
  ui::OverlayLayer* layer = ui::OverlayLayer::of(*this);
  if (layer == nullptr) return false;
  ViewContext& ctx = ctx_;
  layer->close(*this);
  return openDeviceInCircuitView(*layer, ctx, device) != nullptr;
}

void TrackCircuitView::closeSoon() {
  if (!closeTimer_) closeTimer_ = requestTimer(0.0);
}

void TrackCircuitView::onTimer(uint32_t id) {
  if (id == closeTimer_) {
    closeTimer_ = 0;
    if (ui::OverlayLayer* layer = ui::OverlayLayer::of(*this)) layer->close(*this);
    return;  // `this` may be gone
  }
  Popover::onTimer(id);
}

bool TrackCircuitView::onKey(const ui::KeyEvent& e) {
  if (e.down && e.key == ui::key::kReturn && (e.mods & ui::mod::kChordMask) == 0) {
    (void)editDevice(currentDevice());
    return true;
  }
  return Popover::onKey(e);
}

ui::InfoText TrackCircuitView::info() const { return canvas_->info(); }

void TrackCircuitView::onDocument(const doc::StoreEvent& e) {
  if (doc::findTrackPtr(*e.after, track_) == nullptr) {
    ctx_.setStatus("The track was removed: its circuit view closed.");
    closeSoon();
    return;
  }
  if (e.kind == doc::StoreEventKind::Reset) {
    cache_.clear();
    rebuild(true);
    return;
  }
  if (e.changes.structural() || e.changes.has(doc::kChangeMeta)) rebuild(false);
}

void TrackCircuitView::updateTitle() {
  const doc::Track* t = doc::findTrackPtr(*ctx_.doc(), track_);
  titleText_ = (t != nullptr ? t->name : std::string("(removed track)")) + " \xC2\xB7 track circuit";
  const doc::DeviceId d = currentDevice();
  const uint32_t b = d.valid() ? circuit_->boxOf(d) : kNoBox;
  if (b != kNoBox) titleText_ += " \xE2\x80\xBA " + circuit_->boxes[b].title;
  title_->setText(titleText_);
  const bool editable = b != kNoBox && circuit_->boxes[b].kind == ChainBoxKind::Device && !circuit_->boxes[b].missing;
  edit_->setEnabled(editable);
}

void TrackCircuitView::setStatus(std::string s) {
  statusText_ = std::move(s);
  status_->setText(statusText_);
}

TrackCircuitView* openTrackCircuit(ui::Widget& from, ViewContext& ctx, doc::TrackId track, doc::DeviceId focus) {
  ui::OverlayLayer* layer = ui::OverlayLayer::of(from);
  if (layer == nullptr) return nullptr;
  if (doc::findTrackPtr(*ctx.doc(), track) == nullptr) {
    ctx.setStatus("No track to show in the circuit view (select one first)");
    return nullptr;
  }
  ui::UiTree* tree = layer->tree();
  auto view = std::make_unique<TrackCircuitView>(ctx, track);
  const float sc = tree ? tree->scale() : 1.0f;
  const float w = tree ? static_cast<float>(tree->width()) / sc : 1200.0f, h = tree ? static_cast<float>(tree->height()) / sc : 800.0f;
  view->setSize(ui::Length::fixed(w), ui::Length::fixed(h));
  ui::OverlayOptions o;
  o.placement = ui::Placement::AtPoint;
  o.dismissOnOutsideClick = false;
  auto& opened = static_cast<TrackCircuitView&>(layer->open(std::move(view), std::move(o)));
  if (focus.valid()) (void)opened.focusDevice(focus);
  if (tree) tree->setFocus(&opened.canvas());
  return &opened;
}

TrackCircuitView* openDeviceChainCircuit(ui::Widget& from, ViewContext& ctx, doc::DeviceId device) {
  const doc::DeviceLoc loc = doc::findDevice(*ctx.doc(), device);
  if (loc.device == nullptr || loc.track == nullptr) {
    ctx.setStatus("No such device");
    return nullptr;
  }
  return openTrackCircuit(from, ctx, loc.track->id, device);
}

}  // namespace pq::views::circuit
