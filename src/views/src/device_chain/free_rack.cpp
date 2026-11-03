// TX.free-rack (SPEC §15 2026-11-01 "resize, groups, and the free-device rack"; the user: "for the free floating modules
// that arent sequentially linked … theyll be on another tab that floats above it, that can be minimized/collapsed down
// … when its expanded itll shift the 'master' ableton look etc up a bit and then the free device rack will be shown,
// which can be zoomed into … the exact same controllability … as when its in circuit mode"). The selected track's free
// devices (Device::free, not group members: the circuit's free row) live in a panel over the Device view's chain: a tab
// strip (collapse / expand, how many, Add Free Device) and, expanded, the project patch canvas embedded and pruned to
// those devices (project_patch::ProjectPatchView(ctx, FreeRack)) with a resize handle under it. Collapsed, the canvas
// is hidden: it builds nothing, paints nothing and follows no document change until it shows again.
#include <algorithm>
#include <cmath>
#include <string>

#include "../common/paint.h"
#include "internal.h"
#include "pq/ui/overlay.h"
#include "pq/ui/theme.h"
#include "pq/views/project_patch/project_patch_view.h"

namespace pq::views::device_chain {

namespace {
constexpr float kPlusW = 116.0f;  // the tab's "+ Add Free Device" button (logical px)
}  // namespace

FreeRackPanel::FreeRackPanel(DeviceChainView::Impl& impl, ViewContext& ctx)
    : Widget("free-rack"), impl_(impl), ctx_(ctx) {
  setLayout(ui::LayoutKind::Custom);
  setClipChildren(true);
  open_ = ctx.viewPrefs().freeRackOpen;
  height_ = std::clamp(ctx.viewPrefs().freeRackHeight, DeviceChainView::kRackMinH, DeviceChainView::kRackMaxH);
  view_ = &add<project_patch::ProjectPatchView>(ctx, project_patch::ProjectPatchView::FreeRack{});
  view_->setVisible(open_);
  view_->faceVoices = impl.view.faceVoices;
  // A block dragged out of the rack onto the chain goes into it; a wire dragged out of it lands on the chain's knobs
  // and inputs (the cable layer takes the drag over, as from a jack).
  DeviceChainView::Impl* im = &impl_;
  view_->onBlockOutside = [im](doc::DeviceId device, ui::Vec2 pos, bool drop) {
    DeviceChainView& v = im->view;
    const std::optional<uint32_t> slot = device.valid() ? v.chainSlotAt(pos) : std::nullopt;
    if (!drop) {
      DropTarget d;
      if (slot) {
        d = im->trackSlot(pos, {});
        im->rackDragging = device;
      } else {
        im->rackDragging = {};
      }
      if (d.kind != im->drop.kind || d.index != im->drop.index) {
        im->drop = d;
        im->content->markDirty();
      }
      return slot.has_value();
    }
    im->rackDragging = {};
    if (im->drop.kind != DropTarget::Kind::None) {
      im->drop = DropTarget{};
      im->content->markDirty();
    }
    return slot && v.putIntoChainAt(device, *slot);
  };
  view_->onWireOutside = [im](doc::DeviceId from, const std::string& port, ui::Vec2 pos) {
    DeviceChainView& v = im->view;
    if (!v.onJack) return;
    ui::PointerEvent e;
    e.type = ui::PointerType::Down;
    e.button = ui::PointerButton::Left;
    e.pos = pos;
    v.onJack(from, port, e);
    e.type = ui::PointerType::Move;
    v.onJack(from, {}, e);  // releases the canvas's capture: the layer has the pointer from here
  };
  setHeight(ui::Length::fixed(logicalHeight()));
}

float FreeRackPanel::logicalHeight() const noexcept {
  return DeviceChainView::kRackTabH + (open_ ? height_ + DeviceChainView::kRackHandleH : 0.0f);
}

void FreeRackPanel::applyHeight() {
  setHeight(ui::Length::fixed(logicalHeight()));
  invalidateLayout();
  if (parent() != nullptr) parent()->invalidateLayout();
  markDirty();
}

void FreeRackPanel::savePrefs() {
  ViewPrefs p = ctx_.viewPrefs();
  p.freeRackOpen = open_;
  p.freeRackHeight = height_;
  ctx_.setViewPrefs(p);
}

void FreeRackPanel::setOpen(bool open) {
  if (open == open_) return;
  open_ = open;
  view_->setVisible(open_);
  if (open_) view_->setRackTrack(track_);
  applyHeight();
  // The detail area grows by the body (the arrangement moves up; the chain keeps its height), shrinks back when closed.
  if (impl_.view.onRackHeight) impl_.view.onRackHeight((open_ ? 1.0f : -1.0f) * (height_ + DeviceChainView::kRackHandleH));
  savePrefs();
}

void FreeRackPanel::setBodyHeight(float logicalPx) {
  const float h = std::clamp(logicalPx, DeviceChainView::kRackMinH, DeviceChainView::kRackMaxH);
  if (h == height_) return;
  const float delta = h - height_;
  height_ = h;
  if (!open_) return;
  applyHeight();
  if (impl_.view.onRackHeight) impl_.view.onRackHeight(delta);
}

void FreeRackPanel::follow(doc::TrackId track) {
  track_ = track;
  if (open_) view_->setRackTrack(track);  // collapsed: it picks the track up when it opens
  markDirty();
}

void FreeRackPanel::setDropHighlight(bool on) {
  if (on == highlight_) return;
  highlight_ = on;
  markDirty();
}

uint32_t FreeRackPanel::freeCount() const {
  const doc::Track* t = track_.valid() ? doc::findTrackPtr(*ctx_.doc(), track_) : nullptr;
  if (t == nullptr) return 0;
  uint32_t n = 0;
  const doc::MainPath path = doc::mainPath(*ctx_.doc(), *t);  // TX.all-free: the blocks off the main path
  for (const doc::DevicePtr& d : t->devices) n += !path.contains(d->id) && !d->group.valid() ? 1u : 0u;
  return n;
}

ui::IRect FreeRackPanel::plusRect() const noexcept {
  const ui::IRect r = rect();
  const float us = tree() ? tree()->scale() : 1.0f;
  const auto w = static_cast<int32_t>(std::lround(kPlusW * us)), th = static_cast<int32_t>(std::lround(DeviceChainView::kRackTabH * us));
  return {r.right() - w, r.y, w, th};
}

ui::IRect FreeRackPanel::handleRect() const noexcept {
  if (!open_) return {};
  const ui::IRect r = rect();
  const float us = tree() ? tree()->scale() : 1.0f;
  const auto hh = std::max(2, static_cast<int32_t>(std::lround(DeviceChainView::kRackHandleH * us)));
  return {r.x, r.bottom() - hh, r.w, hh};
}

void FreeRackPanel::arrange(ui::LayoutPass& pass) {
  const ui::IRect r = rect();
  const int32_t th = pass.px(DeviceChainView::kRackTabH), hh = pass.px(DeviceChainView::kRackHandleH);
  pass.place(*view_, {r.x + 1, r.y + th, std::max(0, r.w - 2), std::max(0, r.h - th - hh)});
}

void FreeRackPanel::paint(ui::PaintContext& ctx) {
  const ui::Theme* t = ctx.res.theme;
  if (t == nullptr) return;
  const ui::IRect r = rect();
  const int32_t th = ctx.scale.px(DeviceChainView::kRackTabH);
  // The tab strip: a tab on the left (the rack's name and count, the fold triangle), the add button on the right.
  ctx.dl.addRectFilled(ui::IRect{r.x, r.y, r.w, th}.toRect(), t->surface.panel);
  ctx.dl.addHLine(static_cast<float>(r.x), static_cast<float>(r.right()), static_cast<float>(r.y + th) - 0.5f, t->surface.divider, 1.0f);
  const uint32_t n = freeCount();
  const std::string label = "Free devices  \xC2\xB7  " + std::to_string(n);
  const int32_t pad = ctx.scale.px(6.0f);
  int32_t tabW = ctx.scale.px(150.0f);
  if (ctx.res.text != nullptr && ctx.res.uiFonts != nullptr)
    tabW = ctx.res.text->shape(ctx.res.uiFonts->small, label).advance + ctx.scale.px(30.0f);
  const ui::IRect tab{r.x, r.y, tabW, th};
  const ui::Rgba tabFill = open_ ? t->surface.raised : (n > 0 ? ui::lerp(t->surface.raised, t->circuit.wireAudio, 0.12f) : t->surface.sunken);
  paint::box(ctx.dl, tab, ctx.scale.exact(3.0f), tabFill, t->surface.divider);
  // ▾ open, ▸ collapsed.
  const float cx = static_cast<float>(r.x) + ctx.scale.exact(11.0f), cy = static_cast<float>(r.y) + 0.5f * static_cast<float>(th);
  const float k = ctx.scale.exact(3.5f);
  const ui::Rgba ink = n > 0 || open_ ? t->text.primary : t->text.secondary;
  if (open_) ctx.dl.addTriangleFilled({cx - k, cy - 0.5f * k}, {cx + k, cy - 0.5f * k}, {cx, cy + 0.8f * k}, ink);
  else ctx.dl.addTriangleFilled({cx - 0.5f * k, cy - k}, {cx + 0.8f * k, cy}, {cx - 0.5f * k, cy + k}, ink);
  if (ctx.res.uiFonts == nullptr) return;
  paint::text(ctx, ctx.res.uiFonts->small, {r.x + ctx.scale.px(20.0f), r.y, tabW - ctx.scale.px(22.0f), th}, label, ink, ui::TextAlign::Left);
  const ui::IRect plus = plusRect();
  paint::text(ctx, ctx.res.uiFonts->small, paint::inset(plus, pad), "+ Add Free Device", t->text.secondary, ui::TextAlign::Right);
  if (!open_) return;
  // The body's frame and the resize handle (a grip in its middle).
  const ui::IRect body{r.x, r.y + th, r.w, r.h - th};
  ctx.dl.addRectFilled(body.toRect(), t->circuit.background);
  const ui::IRect h = handleRect();
  ctx.dl.addRectFilled(h.toRect(), t->surface.panel);
  const float gx = static_cast<float>(h.x) + 0.5f * static_cast<float>(h.w), gy = static_cast<float>(h.y) + 0.5f * static_cast<float>(h.h);
  ctx.dl.addHLine(gx - ctx.scale.exact(14.0f), gx + ctx.scale.exact(14.0f), std::round(gy), t->surface.divider, ctx.scale.exact(1.0f));
}

void FreeRackPanel::paintOver(ui::PaintContext& ctx) {
  const ui::Theme* t = ctx.res.theme;
  if (t == nullptr) return;
  const ui::IRect r = rect();
  if (open_ && ctx.res.uiFonts != nullptr && view_->patch().nodes.empty()) {
    const int32_t th = ctx.scale.px(DeviceChainView::kRackTabH);
    paint::text(ctx, ctx.res.uiFonts->small, {r.x, r.y + th, r.w, r.h - th},
                track_.valid() ? "No free devices: + Add Free Device, or drag a device's title bar up here to take it out of the chain"
                               : "No track selected",
                t->text.disabled, ui::TextAlign::Center, ctx.scale.px(8.0f), ctx.scale.px(8.0f));
  }
  // A chain device dragged over the rack: where it would go (out of the chain).
  if (highlight_) paint::box(ctx.dl, r, 0.0f, ui::withAlpha(t->accent.primary, 40), t->accent.primary);
}

bool FreeRackPanel::onPointer(const ui::PointerEvent& e) {
  const ui::IRect r = rect();
  const float us = tree() ? tree()->scale() : 1.0f;
  const float th = DeviceChainView::kRackTabH * us;
  if (e.type == ui::PointerType::Down && e.button == ui::PointerButton::Left) {
    if (handleRect().contains(e.pos)) {
      resizing_ = true;
      pressY_ = e.pos.y;
      h0_ = height_;
      return true;
    }
    if (e.pos.y < static_cast<float>(r.y) + th) {
      if (plusRect().contains(e.pos) && track_.valid()) {
        DeviceChainView* v = &impl_.view;
        ui::openContextMenu(*this, {static_cast<float>(plusRect().x), static_cast<float>(plusRect().bottom())},
                            view_->addFreeDeviceItems(track_, [v](const std::string& patch) { (void)v->addFreeDevice(patch); }));
        return true;
      }
      setOpen(!open_);
      return true;
    }
    return false;
  }
  if (e.type == ui::PointerType::Down && e.button == ui::PointerButton::Right && e.pos.y < static_cast<float>(r.y) + th && track_.valid()) {
    DeviceChainView* v = &impl_.view;
    std::vector<ui::MenuItem> items;
    items.push_back(ui::MenuItem::sub("Add Free Device",
                                      view_->addFreeDeviceItems(track_, [v](const std::string& patch) { (void)v->addFreeDevice(patch); })));
    items.push_back(ui::MenuItem::item(open_ ? "Collapse" : "Expand", [this] { setOpen(!open_); }));
    ui::openContextMenu(*this, e.pos, std::move(items));
    return true;
  }
  if (resizing_) {
    if (e.type == ui::PointerType::Move) {
      // Dragging the handle down makes the rack taller (the detail area grows with it).
      setBodyHeight(h0_ + (e.pos.y - pressY_) / us);
      return true;
    }
    if (e.type == ui::PointerType::Up) {
      resizing_ = false;
      savePrefs();
      return true;
    }
  }
  return false;
}

void FreeRackPanel::onCaptureLost() {
  if (resizing_) savePrefs();
  resizing_ = false;
}

ui::Cursor FreeRackPanel::cursorAt(ui::Vec2 p) const {
  if (resizing_ || handleRect().contains(p)) return ui::Cursor::ResizeNS;
  const float us = tree() ? tree()->scale() : 1.0f;
  if (p.y < static_cast<float>(rect().y) + DeviceChainView::kRackTabH * us) return ui::Cursor::Pointer;
  return ui::Cursor::Inherit;
}

ui::InfoText FreeRackPanel::info() const {
  return {"Free Devices",
          "The track's devices outside its chain: they play only through the wires drawn to and from them (the circuit's "
          "free row). Click the tab to expand or collapse the rack, drag its bottom edge to resize it. Inside: Ctrl+wheel "
          "zooms, the wheel pans, drag an output onto a knob or an input (here or on the chain below) to wire it, S-drag "
          "resizes a block. Drag a block onto the chain to put it into the chain, a device's title bar up here to take it "
          "out."};
}

}  // namespace pq::views::device_chain
