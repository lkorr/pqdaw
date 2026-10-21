// UiTree: layout pass, paint traversal, input dispatch, focus traversal, key routing, timers.

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>

#include "pq/ui/widget.h"

namespace pq::ui {

namespace {

inline int32_t scalePx(float v, float s) noexcept { return static_cast<int32_t>(std::lround(v * s)); }
inline int32_t scaleBound(int32_t v, float s) noexcept {
  return v >= kUnbounded ? kUnbounded : static_cast<int32_t>(std::lround(static_cast<float>(v) * s));
}
inline int32_t clampLen(int32_t v, const Length& l, float s) noexcept {
  const int32_t lo = scaleBound(l.min, s);
  return std::clamp(v, lo, std::max(lo, scaleBound(l.max, s)));
}
inline int32_t alignOffset(Align a, int32_t slack) noexcept {
  switch (a) {
    case Align::Start: return 0;
    case Align::Center: return slack / 2;
    case Align::End: return slack;
  }
  return 0;
}
inline uint64_t bindingKey(ScopeId scope, Chord c) noexcept {
  return (uint64_t(scope) << 48) | (uint64_t(c.mods) << 32) | uint64_t(c.key);
}
inline uint8_t buttonBit(PointerButton b) noexcept {
  return b == PointerButton::None ? 0 : static_cast<uint8_t>(1u << (static_cast<uint32_t>(b) - 1));
}

}  // namespace

UiTree::UiTree() = default;

UiTree::~UiTree() {
  hovered_ = captured_ = focus_ = nullptr;
  frameRequests_.clear();
  timers_.clear();
  root_.reset();
}

Widget& UiTree::setRoot(std::unique_ptr<Widget> root) {
  assert(root && !root->parent_);
  if (root_) {
    detachSubtree(*root_);
    root_->attach(nullptr);
  }
  root_ = std::move(root);
  root_->attach(this);
  invalidateAll();
  damage_.addAll();
  return *root_;
}

Widget* UiTree::find(std::string_view path) const noexcept { return root_ ? root_->find(path) : nullptr; }

void UiTree::setViewport(uint32_t width, uint32_t height, float scale) {
  if (width == width_ && height == height_ && scale == scale_) return;
  const bool rescaled = scale != scale_;
  width_ = width;
  height_ = height;
  scale_ = scale > 0.0f ? scale : 1.0f;
  damage_.addAll();
  if (rescaled) {
    invalidateAll();
  } else {
    layoutPending_ = true;  // the root's rect changes: everything below is re-placed anyway
  }
}

void UiTree::invalidateAll() noexcept {
  if (!root_) return;
  // Iterative pre-order walk (no recursion depth concerns, no allocation).
  Widget* w = root_.get();
  while (w) {
    w->flags_ |= Widget::kArrangeDirty | Widget::kDescendantDirty;
    if (!w->children_.empty()) {
      w = w->children_.front().get();
      continue;
    }
    while (w && w != root_.get()) {
      Widget* p = w->parent_;
      if (w->indexInParent_ + 1 < p->children_.size()) {
        w = p->children_[w->indexInParent_ + 1].get();
        break;
      }
      w = p;
    }
    if (w == root_.get()) break;
  }
  layoutPending_ = true;
}

void UiTree::layoutDirty(Widget& w) {
  w.flags_ |= Widget::kArrangeDirty;
  for (Widget* p = w.parent_; p && !(p->flags_ & Widget::kDescendantDirty); p = p->parent_)
    p->flags_ |= Widget::kDescendantDirty;
  layoutPending_ = true;
}

// --- layout ---------------------------------------------------------------------------------------------

void UiTree::layoutNow() {
  if (!root_ || !layoutPending_) return;
  layoutPending_ = false;
  ++stats_.layoutPasses;
  const IRect view{0, 0, static_cast<int32_t>(width_), static_cast<int32_t>(height_)};
  clip_ = view;
  place(*root_, view);
  // Content may have moved under a still pointer (scroll, splitter, rebuilt view): keep hover truthful.
  if (pointerInside_ && !captured_) updateHover(pointer_);
}

void UiTree::place(Widget& w, const IRect& r) {
  const bool moved = !(w.rect_ == r);
  if (moved) {
    // Old and new area, as far as they are visible through the clipping ancestors (clip_).
    if (!damage_.full() && w.visible()) {
      const IRect o = intersect(w.rect_, clip_), n = intersect(r, clip_);
      if (!o.empty()) damage_.add(o.toRect());
      if (!n.empty()) damage_.add(n.toRect());
    }
    w.rect_ = r;
  }
  if (!moved && !(w.flags_ & (Widget::kArrangeDirty | Widget::kDescendantDirty))) return;
  const IRect saved = clip_;
  if (w.clipsChildren()) clip_ = intersect(clip_, w.rect_);
  if (moved || (w.flags_ & Widget::kArrangeDirty)) {
    arrangeChildren(w);
  } else {
    w.flags_ &= ~Widget::kDescendantDirty;
    for (auto& c : w.children_)
      if (c->flags_ & (Widget::kArrangeDirty | Widget::kDescendantDirty)) place(*c, c->rect_);
  }
  clip_ = saved;
}

void UiTree::arrangeChildren(Widget& w) {
  w.flags_ &= ~(Widget::kArrangeDirty | Widget::kDescendantDirty);
  ++stats_.widgetsArranged;
  if (w.children_.empty()) return;
  switch (w.layout_) {
    case LayoutKind::Stack: arrangeStack(w); break;
    case LayoutKind::Row: arrangeLinear(w, true); break;
    case LayoutKind::Column: arrangeLinear(w, false); break;
    case LayoutKind::Custom: {
      LayoutPass pass(*this);
      pass.scale_ = scale_;
      w.arrange(pass);
      break;
    }
  }
}

namespace {
IRect contentRect(const IRect& r, const Insets& p, float s) noexcept {
  const int32_t l = scalePx(p.left, s), t = scalePx(p.top, s), rr = scalePx(p.right, s), b = scalePx(p.bottom, s);
  return {r.x + l, r.y + t, std::max(0, r.w - l - rr), std::max(0, r.h - t - b)};
}
}  // namespace

void UiTree::arrangeStack(Widget& w) {
  const float s = scale_;
  const IRect in = contentRect(w.rect_, w.padding_, s);
  for (auto& cp : w.children_) {
    Widget& c = *cp;
    if (!c.visible()) continue;
    const int32_t cw = clampLen(c.width_.kind == SizeKind::Fixed ? scalePx(c.width_.value, s) : in.w, c.width_, s);
    const int32_t ch = clampLen(c.height_.kind == SizeKind::Fixed ? scalePx(c.height_.value, s) : in.h, c.height_, s);
    place(c, {in.x + alignOffset(w.mainAlign_, in.w - cw), in.y + alignOffset(w.crossAlign_, in.h - ch), cw, ch});
  }
}

void UiTree::arrangeLinear(Widget& w, bool row) {
  const float s = scale_;
  const IRect in = contentRect(w.rect_, w.padding_, s);
  const int32_t innerMain = row ? in.w : in.h;
  const int32_t innerCross = row ? in.h : in.w;
  const int32_t gap = scalePx(w.gap_, s);

  // Pass 1: fixed sizes, flex count. mainSize_ / kFrozen are per-child scratch.
  int32_t n = 0, nFlex = 0;
  int64_t fixedSum = 0;
  for (auto& cp : w.children_) {
    Widget& c = *cp;
    if (!c.visible()) continue;
    ++n;
    const Length& L = row ? c.width_ : c.height_;
    if (L.kind == SizeKind::Fixed) {
      c.mainSize_ = clampLen(scalePx(L.value, s), L, s);
      fixedSum += c.mainSize_;
    } else {
      ++nFlex;
      c.flags_ &= ~Widget::kFrozen;
      c.mainSize_ = clampLen(0, L, s);
    }
  }
  if (n == 0) return;
  const int64_t free = int64_t(innerMain) - fixedSum - int64_t(gap) * (n - 1);

  // Pass 2: share `free` among flex children by weight; children whose share violates min/max are frozen at
  // the limit and the rest re-shared (CSS flexbox's resolution loop). Pixel edges come from rounding the
  // running sum, so the shares add up to exactly the free space and no gaps or overlaps appear.
  for (int32_t iter = 0; nFlex > 0 && iter <= nFlex; ++iter) {
    double wsum = 0;
    int64_t frozenSum = 0;
    for (auto& cp : w.children_) {
      Widget& c = *cp;
      const Length& L = row ? c.width_ : c.height_;
      if (!c.visible() || L.kind != SizeKind::Flex) continue;
      if (c.flags_ & Widget::kFrozen) {
        frozenSum += c.mainSize_;
      } else {
        wsum += std::max(0.0f, L.value);
      }
    }
    if (wsum <= 0.0) break;
    const double rem = static_cast<double>(free - frozenSum);
    double acc = 0;
    int64_t prevEdge = 0, violation = 0;
    for (auto& cp : w.children_) {
      Widget& c = *cp;
      const Length& L = row ? c.width_ : c.height_;
      if (!c.visible() || L.kind != SizeKind::Flex || (c.flags_ & Widget::kFrozen)) continue;
      acc += rem * std::max(0.0f, L.value) / wsum;
      const int64_t edge = std::llround(acc);
      const int32_t size = static_cast<int32_t>(edge - prevEdge);
      prevEdge = edge;
      c.mainSize_ = size;
      violation += clampLen(size, L, s) - size;
    }
    if (violation == 0) break;
    for (auto& cp : w.children_) {
      Widget& c = *cp;
      const Length& L = row ? c.width_ : c.height_;
      if (!c.visible() || L.kind != SizeKind::Flex || (c.flags_ & Widget::kFrozen)) continue;
      const int32_t clamped = clampLen(c.mainSize_, L, s);
      if ((violation > 0 && clamped > c.mainSize_) || (violation < 0 && clamped < c.mainSize_)) {
        c.mainSize_ = clamped;
        c.flags_ |= Widget::kFrozen;
      }
    }
  }

  int64_t used = int64_t(gap) * (n - 1);
  for (auto& cp : w.children_)
    if (cp->visible()) used += cp->mainSize_;
  const int64_t slack = int64_t(innerMain) - used;
  int32_t pos = (row ? in.x : in.y) + (slack > 0 ? alignOffset(w.mainAlign_, static_cast<int32_t>(slack)) : 0);
  const int32_t crossStart = row ? in.y : in.x;

  for (auto& cp : w.children_) {
    Widget& c = *cp;
    if (!c.visible()) continue;
    const Length& C = row ? c.height_ : c.width_;
    const int32_t cross = clampLen(C.kind == SizeKind::Fixed ? scalePx(C.value, s) : innerCross, C, s);
    const int32_t cpos = crossStart + alignOffset(w.crossAlign_, innerCross - cross);
    const int32_t size = c.mainSize_;
    place(c, row ? IRect{pos, cpos, size, cross} : IRect{cpos, pos, cross, size});
    pos += size + gap;
  }
}

// --- frame ----------------------------------------------------------------------------------------------

void UiTree::requestFrame(Widget& w) {
  if (w.flags_ & Widget::kFrameRequested) return;
  w.flags_ |= Widget::kFrameRequested;
  frameRequests_.push_back(&w);
}

void UiTree::update(double now) {
  now_ = now;
  if (!timers_.empty()) {
    timerWork_.clear();
    for (size_t i = 0; i < timers_.size();) {
      if (timers_[i].due <= now) {
        timerWork_.push_back(timers_[i]);
        timers_[i] = timers_.back();
        timers_.pop_back();
      } else {
        ++i;
      }
    }
    std::sort(timerWork_.begin(), timerWork_.end(), [](const Timer& a, const Timer& b) { return a.due < b.due || (a.due == b.due && a.id < b.id); });
    for (size_t i = 0; i < timerWork_.size(); ++i)
      if (Widget* w = timerWork_[i].widget) w->onTimer(timerWork_[i].id);  // removed widgets were nulled
    timerWork_.clear();
  }
  if (!frameRequests_.empty()) {
    frameWork_.swap(frameRequests_);
    for (Widget* w : frameWork_) w->flags_ &= ~Widget::kFrameRequested;
    for (size_t i = 0; i < frameWork_.size(); ++i)
      if (Widget* w = frameWork_[i]) w->onFrame(now);
    frameWork_.clear();
  }
  layoutNow();
}

bool UiTree::needsFrame() const noexcept { return damage_.needsFrame() || !frameRequests_.empty() || layoutPending_; }

double UiTree::nextWakeTime() const noexcept {
  double t = std::numeric_limits<double>::infinity();
  for (const Timer& x : timers_) t = std::min(t, x.due);
  return t;
}

void UiTree::paint(DrawList& dl, const PaintResources& res, double now) {
  layoutNow();
  PaintContext ctx{dl, res, now, scale_, {0, 0, static_cast<int32_t>(width_), static_cast<int32_t>(height_)}};
  stats_.widgetsPainted = 0;
  if (root_) paintWidget(*root_, ctx);
  damage_.clear();
  ++stats_.framesPainted;
}

void UiTree::paintWidget(Widget& w, PaintContext& ctx) {
  if (!w.visible() || w.rect_.empty()) return;
  const IRect vis = intersect(w.rect_, ctx.clip);
  if (vis.empty()) return;  // culled: nothing of it (or, by layout containment, its children) is visible
  ++stats_.widgetsPainted;
  w.paint(ctx);
  if (!w.children_.empty()) {
    if (w.clipsChildren()) {
      const IRect saved = ctx.clip;
      ctx.clip = vis;
      ctx.dl.pushClip(vis.toRect());
      for (auto& c : w.children_) paintWidget(*c, ctx);
      ctx.dl.popClip();
      ctx.clip = saved;
    } else {
      for (auto& c : w.children_) paintWidget(*c, ctx);
    }
  }
  w.paintOver(ctx);
}

void UiTree::damageWidget(const Widget& w) noexcept {
  if (!w.visible()) return;
  IRect r = w.rect_;
  for (const Widget* p = w.parent_; p; p = p->parent_) {
    if (!p->visible()) return;
    if (p->clipsChildren()) r = intersect(r, p->rect_);
  }
  r = intersect(r, {0, 0, static_cast<int32_t>(width_), static_cast<int32_t>(height_)});
  if (!r.empty()) damage_.add(r.toRect());
}

// --- detach / interaction cleanup -----------------------------------------------------------------------

void UiTree::clearInteraction(Widget& w) noexcept {
  auto inside = [&](const Widget* p) { return p && (p == &w || w.isAncestorOf(*p)); };
  if (inside(hovered_)) {
    hovered_->set(Widget::kHovered, false);
    Widget* h = hovered_;
    hovered_ = nullptr;
    h->onHoverChanged(false);
  }
  if (inside(captured_)) {
    Widget* c = captured_;
    captured_ = nullptr;
    buttonsHeld_ = 0;
    c->set(Widget::kCaptured, false);
    c->onCaptureLost();
  }
  if (inside(focus_)) setFocus(nullptr);
}

void UiTree::detachSubtree(Widget& w) noexcept {
  damageWidget(w);
  clearInteraction(w);
  auto inside = [&](const Widget* p) { return p && (p == &w || w.isAncestorOf(*p)); };
  frameRequests_.erase(std::remove_if(frameRequests_.begin(), frameRequests_.end(), inside), frameRequests_.end());
  for (Widget*& p : frameWork_)
    if (inside(p)) p = nullptr;
  timers_.erase(std::remove_if(timers_.begin(), timers_.end(), [&](const Timer& t) { return inside(t.widget); }), timers_.end());
  for (Timer& t : timerWork_)
    if (inside(t.widget)) t.widget = nullptr;
}

// --- input ----------------------------------------------------------------------------------------------

Widget* UiTree::hitRec(Widget& w, Vec2 p) const noexcept {
  if (!w.visible() || !w.rect_.contains(p)) return nullptr;
  for (size_t i = w.children_.size(); i-- > 0;)  // last child is on top
    if (Widget* h = hitRec(*w.children_[i], p)) return h;
  return w.hittable() ? &w : nullptr;
}

Widget* UiTree::hitTest(Vec2 p) const noexcept { return root_ ? hitRec(*root_, p) : nullptr; }

void UiTree::updateHover(Vec2 p) {
  if (captured_) return;  // hover stays on the captured widget until release
  Widget* h = pointerInside_ ? hitTest(p) : nullptr;
  if (h == hovered_) return;
  Widget* old = hovered_;
  hovered_ = h;
  if (old) {
    old->set(Widget::kHovered, false);
    old->onHoverChanged(false);
  }
  if (h) {
    h->set(Widget::kHovered, true);
    h->onHoverChanged(true);
  }
}

bool UiTree::bubblePointer(Widget* from, const PointerEvent& e) {
  for (Widget* w = from; w; w = w->parent_) {
    if (!w->enabled() || !w->onPointer(e)) continue;
    if (e.type == PointerType::Down && !captured_) {
      captured_ = w;
      w->set(Widget::kCaptured, true);
    }
    return true;
  }
  return false;
}

bool UiTree::dispatch(const InputEvent& e) {
  switch (e.kind) {
    case InputKind::Pointer: return dispatchPointer(e.pointer);
    case InputKind::Key: return dispatchKey(e.key) != KeyResult::Unhandled;
    case InputKind::Text: return dispatchText(e.text);
    case InputKind::None: break;
  }
  return false;
}

bool UiTree::dispatchPointer(const PointerEvent& e) {
  if (e.type == PointerType::Leave) {
    pointerInside_ = false;
    updateHover(pointer_);
    return false;
  }
  layoutNow();  // hit testing needs current rects
  pointer_ = e.pos;
  pointerInside_ = true;
  if (e.type == PointerType::Down) buttonsHeld_ |= buttonBit(e.button);
  if (e.type == PointerType::Up) buttonsHeld_ &= static_cast<uint8_t>(~buttonBit(e.button));

  if (captured_ && e.type != PointerType::Wheel) {
    Widget* c = captured_;
    if (c->enabled()) c->onPointer(e);
    if (e.type == PointerType::Up && buttonsHeld_ == 0 && captured_ == c) {
      captured_ = nullptr;
      c->set(Widget::kCaptured, false);
      updateHover(e.pos);  // leave the captured widget if the pointer is elsewhere now
    }
    return true;
  }

  updateHover(e.pos);
  Widget* target = hovered_;
  if (e.type == PointerType::Down) {
    Widget* f = target;
    while (f && !canFocus(*f)) f = f->parent_;
    setFocus(f);
  }
  return bubblePointer(target, e);
}

bool UiTree::dispatchText(const TextEvent& e) {
  if (focus_ && focus_->acceptsTextInput() && focus_->enabled()) return focus_->onText(e);
  return false;
}

bool UiTree::runBinding(ScopeId scope, const KeyEvent& e, KeyResult& result) const {
  const ScopeRules rules = scope < scopeRules_.size() ? scopeRules_[scope] : ScopeRules{};
  if (e.mods & rules.passthroughMods) return false;
  const uint64_t k = bindingKey(scope, chordOf(e));
  auto it = std::lower_bound(bindings_.begin(), bindings_.end(), k, [](const Bound& b, uint64_t v) { return b.key < v; });
  if (it != bindings_.end() && it->key == k && it->action != kNoAction && actions_) {
    if (e.repeat && !actions_->options(it->action).repeatable) {
      result = KeyResult::Action;  // the chord belongs to this scope; a held toggle must not re-fire
      return true;
    }
    if (actions_->invoke(it->action)) {
      result = KeyResult::Action;
      return true;
    }
    // Disabled: the chord falls through to the next scope (unless this scope swallows unbound keys).
  }
  if (rules.swallowUnbound) {
    result = KeyResult::ScopeSwallowed;
    return true;
  }
  return false;
}

KeyResult UiTree::dispatchKey(const KeyEvent& e) {
  // 1. The focused widget, then its ancestors. A focused text field claims printable keys outright.
  Widget* f = focus_;
  if (f) {
    if (f->enabled() && f->onKey(e)) return KeyResult::Widget;
    if (f->acceptsTextInput() && isPrintableKey(e)) return KeyResult::TextSwallowed;
    for (Widget* w = f->parent_; w; w = w->parent_)
      if (w->enabled() && w->onKey(e)) return KeyResult::Widget;
  }
  // Key releases and lone modifier presses are never chords.
  if (!e.down || isModifierKey(e.key)) return KeyResult::Unhandled;
  KeyResult r = KeyResult::Unhandled;
  // 2. Modal scopes (macro mode, modal dialogs), newest first.
  for (uint32_t i = modalCount_; i-- > 0;)
    if (runBinding(modal_[i], e, r)) return r;
  // 3. Scopes of the focused widget and its ancestors, innermost first.
  for (Widget* w = f; w; w = w->parent_)
    if (w->keyScope_ != kNoScope && w->keyScope_ != kGlobalScope && runBinding(w->keyScope_, e, r)) return r;
  // 4. Global actions.
  if (runBinding(kGlobalScope, e, r)) return r;
  // 5. Default focus traversal.
  if (e.key == key::kTab && (e.mods & (mod::kCtrl | mod::kAlt | mod::kSuper)) == 0) {
    focusNext((e.mods & mod::kShift) != 0);
    return KeyResult::Focus;
  }
  return KeyResult::Unhandled;
}

void UiTree::setKeymap(const ActionRegistry* actions, const Keymap* keymap) {
  actions_ = actions;
  bindings_.clear();
  scopeRules_.clear();
  if (!keymap) return;
  bindings_.reserve(keymap->bindings().size());
  for (const Keymap::Binding& b : keymap->bindings())
    bindings_.push_back({bindingKey(b.scope, b.chord), actions ? actions->find(b.action) : kNoAction});
  std::sort(bindings_.begin(), bindings_.end(), [](const Bound& a, const Bound& b) { return a.key < b.key; });
  scopeRules_.resize(keymap->scopeCount());
  for (size_t i = 0; i < keymap->scopeCount(); ++i) scopeRules_[i] = keymap->rules(static_cast<ScopeId>(i));
}

void UiTree::pushModalScope(ScopeId s) noexcept {
  removeModalScope(s);
  if (modalCount_ < kMaxModalScopes) modal_[modalCount_++] = s;
}

void UiTree::removeModalScope(ScopeId s) noexcept {
  uint32_t o = 0;
  for (uint32_t i = 0; i < modalCount_; ++i)
    if (modal_[i] != s) modal_[o++] = modal_[i];
  modalCount_ = o;
}

void UiTree::releaseCapture() {
  if (!captured_) return;
  Widget* c = captured_;
  captured_ = nullptr;
  buttonsHeld_ = 0;
  c->set(Widget::kCaptured, false);
  c->onCaptureLost();
  updateHover(pointer_);
}

// --- focus ----------------------------------------------------------------------------------------------

void UiTree::setFocus(Widget* w) {
  if (w == focus_) return;
  Widget* old = focus_;
  focus_ = w;
  if (old) {
    old->set(Widget::kFocused, false);
    old->onFocusChanged(false);
  }
  if (w) {
    w->set(Widget::kFocused, true);
    w->onFocusChanged(true);
  }
}

bool UiTree::canFocus(const Widget& w) const noexcept {
  if (!w.focusable() || !w.enabled()) return false;
  for (const Widget* p = &w; p; p = p->parent_)
    if (!p->visible()) return false;
  return true;
}

namespace {
// Pre-order successor that skips invisible subtrees; nullptr after the last node.
Widget* nextPreorder(Widget* w, Widget* root) noexcept {
  if (!w) return root;
  if (w->visible() && w->childCount()) return &w->child(0);
  while (w && w != root) {
    Widget* p = w->parent();
    if (w->indexInParent() + 1 < p->childCount()) return &p->child(w->indexInParent() + 1);
    w = p;
  }
  return nullptr;
}
Widget* lastDescendant(Widget* w) noexcept {
  while (w->visible() && w->childCount()) w = &w->child(w->childCount() - 1);
  return w;
}
// Pre-order predecessor; nullptr before the root, and prev(nullptr) = the last node.
Widget* prevPreorder(Widget* w, Widget* root) noexcept {
  if (!w) return lastDescendant(root);
  if (w == root) return nullptr;
  Widget* p = w->parent();
  if (w->indexInParent() > 0) return lastDescendant(&p->child(w->indexInParent() - 1));
  return p;
}
}  // namespace

bool UiTree::focusNext(bool backwards) {
  if (!root_) return false;
  Widget* const start = focus_;
  Widget* w = start;
  // The walk is cyclic over (nodes + one "outside" position), so it ends at `start` after at most n+1 steps.
  do {
    w = backwards ? prevPreorder(w, root_.get()) : nextPreorder(w, root_.get());
    if (w && w != start && canFocus(*w)) {
      setFocus(w);
      return true;
    }
  } while (w != start);
  return false;
}

}  // namespace pq::ui
