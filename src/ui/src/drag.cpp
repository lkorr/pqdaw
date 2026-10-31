// Drag sessions (T5.7, pq/ui/drag.h): targeting, the ghost, auto-scroll, internal and external sessions.

#include <algorithm>
#include <cmath>

#include "controls_paint.h"
#include "drag_state.h"
#include "pq/ui/text.h"
#include "pq/ui/theme.h"

namespace pq::ui {

bool UiTree::beginDrag(DragPayload payload) {
  if (!captured_) return false;
  if (drag_) endDrag(false);
  payload.external = false;
  drag_ = std::make_unique<DragState>();
  drag_->payload = std::move(payload);
  updateDrag(pointer_, mods_);
  return true;
}

void UiTree::dragEnter(DragPayload payload, Vec2 pos, Mods mods) {
  if (drag_) endDrag(false);
  payload.external = true;
  layoutNow();
  drag_ = std::make_unique<DragState>();
  drag_->payload = std::move(payload);
  updateDrag(pos, mods);
}

DropEffect UiTree::dragMove(Vec2 pos, Mods mods) {
  if (!drag_) return DropEffect::None;
  layoutNow();
  updateDrag(pos, mods);
  return drag_->feedback.effect;
}

void UiTree::dragExit() {
  if (drag_ && drag_->payload.external) endDrag(false);
}

DropEffect UiTree::dragDrop(Vec2 pos, Mods mods) {
  if (!drag_) return DropEffect::None;
  layoutNow();
  updateDrag(pos, mods);
  const DropEffect fx = drag_->target ? drag_->feedback.effect : DropEffect::None;
  Widget* t = drag_->target;
  DragState* d = drag_.get();
  DropEffect done = DropEffect::None;
  if (t && fx != DropEffect::None) {
    d->target = nullptr;  // drop() ends the session for the target: no dragLeave follows
    done = t->drop(d->payload, pos, mods);
  }
  endDrag(false);
  return done;
}

void UiTree::cancelDrag() { endDrag(false); }

const DragPayload* UiTree::dragPayload() const noexcept { return drag_ ? &drag_->payload : nullptr; }
Widget* UiTree::dropTarget() const noexcept { return drag_ ? drag_->target : nullptr; }
DropFeedback UiTree::dropFeedback() const noexcept { return drag_ ? drag_->feedback : DropFeedback{}; }
IRect UiTree::dragGhostRect() const noexcept { return drag_ ? drag_->ghost : IRect{}; }

void UiTree::updateDrag(Vec2 pos, Mods mods) {
  DragState& d = *drag_;
  d.pos = pos;
  d.mods = mods;
  Widget* target = nullptr;
  DropFeedback fb;
  for (Widget* w = hitTest(pos); w; w = w->parent_) {
    if (!w->enabled()) continue;
    const DropFeedback f = w->dragOver(d.payload, pos, mods);
    if (f.effect != DropEffect::None) {
      target = w;
      fb = f;
      break;
    }
  }
  if (target != d.target && d.target) {
    Widget* old = d.target;
    d.target = nullptr;
    old->dragLeave();
    if (!drag_) return;  // the target ended the session from dragLeave (it must not, but stay safe)
  }
  d.target = target;
  d.feedback = fb;
  // The ghost: damage where it was and where it goes.
  const IRect g = ghostRectAt(pos);
  if (!(g == d.ghost)) {
    if (!d.ghost.empty()) damage_.add(d.ghost.toRect());
    if (!g.empty()) damage_.add(g.toRect());
    d.ghost = g;
  }
  // Would anything under the pointer scroll? (The actual steps run in update().)
  bool scroll = false;
  for (Widget* w = hitTest(pos); w && !scroll; w = w->parent_) scroll = w->visible() && w->autoScrollDrag(pos, 0.0);
  if (scroll && !d.scrolling) d.lastTick = -1.0;
  d.scrolling = scroll;
}

void UiTree::autoScrollDrag(double now) {
  DragState& d = *drag_;
  if (!d.scrolling) return;
  const double dt = d.lastTick < 0 ? 0.0 : std::min(0.1, now - d.lastTick);
  d.lastTick = now;
  if (dt <= 0.0) return;
  bool moved = false;
  for (Widget* w = hitTest(d.pos); w; w = w->parent_)
    if (w->visible() && w->autoScrollDrag(d.pos, dt)) moved = true;
  if (!moved) {
    d.scrolling = false;
    return;
  }
  layoutNow();
  updateDrag(d.pos, d.mods);  // the content moved under the pointer: retarget (the arrangement's ghost clips follow)
}

void UiTree::endDrag(bool dropIt) {
  if (!drag_) return;
  std::unique_ptr<DragState> d = std::move(drag_);  // a target's callback may start no new session on this one
  if (!d->ghost.empty()) damage_.add(d->ghost.toRect());
  if (d->target) {
    Widget* t = d->target;
    d->target = nullptr;
    if (dropIt && d->feedback.effect != DropEffect::None) (void)t->drop(d->payload, d->pos, d->mods);
    else t->dragLeave();
  }
}

IRect UiTree::ghostRectAt(Vec2 pos) const {
  const DragPayload& p = drag_->payload;
  if (p.label.empty() && !p.ghost) return {};
  const float s = scale_.factor;
  int32_t w = static_cast<int32_t>(std::lround(p.ghostWidth * s));
  int32_t h = static_cast<int32_t>(std::lround(p.ghostHeight * s));
  if (w <= 0 || h <= 0) {
    int32_t textW = static_cast<int32_t>(std::lround(7.0f * s * static_cast<float>(p.label.size())));
    if (resources_ && resources_->text && resources_->uiFonts) textW = resources_->text->measure(resources_->uiFonts->small, p.label);
    w = std::min(textW + static_cast<int32_t>(std::lround(16.0f * s)), static_cast<int32_t>(std::lround(320.0f * s)));
    h = static_cast<int32_t>(std::lround(20.0f * s));
  }
  int32_t x = static_cast<int32_t>(std::lround(pos.x + 14.0f * s));
  int32_t y = static_cast<int32_t>(std::lround(pos.y + 10.0f * s));
  x = std::clamp(x, 0, std::max(0, static_cast<int32_t>(width_) - w));
  y = std::clamp(y, 0, std::max(0, static_cast<int32_t>(height_) - h));
  return {x, y, w, h};
}

void UiTree::paintGhost(PaintContext& ctx) {
  const DragState& d = *drag_;
  if (d.ghost.empty() || intersect(d.ghost, ctx.clip).empty()) return;
  ctx.widget = d.ghost;
  if (d.payload.ghost) {
    d.payload.ghost(ctx, d.ghost);
    return;
  }
  const Theme* th = ctx.res.theme;
  if (!th) return;
  const Rgba fill = d.feedback.effect == DropEffect::None ? th->surface.raised : th->menu.fill;
  detail::box(ctx.dl, d.ghost, 3.0f * ctx.scale.factor, fill, th->menu.border);
  if (ctx.res.uiFonts)
    detail::drawText(ctx, ctx.res.uiFonts->small, d.ghost, d.payload.label,
                     d.feedback.effect == DropEffect::None ? th->text.disabled : th->text.primary, TextAlign::Left,
                     static_cast<int32_t>(std::lround(8.0f * ctx.scale.factor)), static_cast<int32_t>(std::lround(8.0f * ctx.scale.factor)));
}

// --- ScrollView -------------------------------------------------------------------------------------------

bool ScrollView::autoScrollDrag(Vec2 pos, double dt) {
  const IRect r = rect();
  if (r.empty() || !r.contains(pos)) return false;
  const float s = tree() ? tree()->scale() : 1.0f;
  const float zone = kDragScrollZone * s;
  // Speed rises linearly from 0 at the zone's inner edge to kDragScrollSpeed at the viewport's edge.
  auto speed = [&](float depth) { return kDragScrollSpeed * s * std::clamp(1.0f - depth / zone, 0.0f, 1.0f); };
  float vx = 0, vy = 0;
  if (scrollsVertically()) {
    const int32_t maxY = std::max(0, content_.h - r.h);
    if (pos.y < static_cast<float>(r.y) + zone && scrollY_ > 0) vy = -speed(pos.y - static_cast<float>(r.y));
    else if (pos.y > static_cast<float>(r.bottom()) - zone && scrollY_ < maxY) vy = speed(static_cast<float>(r.bottom()) - pos.y);
  }
  if (scrollsHorizontally()) {
    const int32_t maxX = std::max(0, content_.w - r.w);
    if (pos.x < static_cast<float>(r.x) + zone && scrollX_ > 0) vx = -speed(pos.x - static_cast<float>(r.x));
    else if (pos.x > static_cast<float>(r.right()) - zone && scrollX_ < maxX) vx = speed(static_cast<float>(r.right()) - pos.x);
  }
  if (vx == 0 && vy == 0) return false;
  if (dt <= 0.0) return true;
  // Fractions accumulate (a slow edge still moves, a pixel every few frames).
  dragAccX_ += static_cast<float>(vx * dt);
  dragAccY_ += static_cast<float>(vy * dt);
  const auto dx = static_cast<int32_t>(dragAccX_), dy = static_cast<int32_t>(dragAccY_);
  dragAccX_ -= static_cast<float>(dx);
  dragAccY_ -= static_cast<float>(dy);
  if (dx != 0 || dy != 0) scrollTo(scrollX_ + dx, scrollY_ + dy);
  return true;
}

}  // namespace pq::ui
