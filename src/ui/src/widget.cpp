// Widget, Splitter and ScrollView. The tree-wide parts (layout pass, dispatch, focus, timers) are in
// ui_tree.cpp.

#include "pq/ui/widget.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace pq::ui {

IRect intersect(const IRect& a, const IRect& b) noexcept {
  const int32_t x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y);
  const int32_t x1 = std::min(a.right(), b.right()), y1 = std::min(a.bottom(), b.bottom());
  if (x1 <= x0 || y1 <= y0) return {x0, y0, 0, 0};
  return {x0, y0, x1 - x0, y1 - y0};
}

int32_t LayoutPass::px(float logical) const noexcept { return static_cast<int32_t>(std::lround(logical * scale_)); }

void LayoutPass::place(Widget& child, const IRect& r) { tree_.place(child, r); }

// --- Widget ---------------------------------------------------------------------------------------------

Widget::Widget(std::string name) : name_(std::move(name)) {
  assert(name_.find('/') == std::string::npos && "widget names must not contain '/'");
}

Widget::~Widget() = default;

void Widget::attach(UiTree* tree) noexcept {
  tree_ = tree;
  flags_ |= kDescendantDirty;  // a (re)attached subtree is fully revisited by the next layout
  if (tree) tree->noteOverflow(overflow_);
  for (auto& c : children_) c->attach(tree);
}

Widget& Widget::addChild(std::unique_ptr<Widget> child) {
  assert(child && !child->parent_);
#ifndef NDEBUG
  if (!child->name_.empty())
    for (const auto& s : children_) assert(s->name_ != child->name_ && "duplicate sibling widget name");
#endif
  child->parent_ = this;
  child->indexInParent_ = static_cast<uint32_t>(children_.size());
  child->flags_ &= ~(kHovered | kFocused | kCaptured | kFrameRequested);
  children_.push_back(std::move(child));
  Widget& c = *children_.back();
  c.attach(tree_);
  if (tree_) {
    tree_->layoutDirty(*this);
  } else {
    flags_ |= kArrangeDirty;
  }
  return c;
}

std::unique_ptr<Widget> Widget::removeChild(Widget& child) {
  assert(child.parent_ == this);
  if (tree_) tree_->detachSubtree(child);
  const uint32_t idx = child.indexInParent_;
  std::unique_ptr<Widget> out = std::move(children_[idx]);
  children_.erase(children_.begin() + idx);
  for (uint32_t i = idx; i < children_.size(); ++i) children_[i]->indexInParent_ = i;
  out->parent_ = nullptr;
  out->indexInParent_ = 0;
  out->attach(nullptr);
  if (tree_) tree_->layoutDirty(*this);
  return out;
}

void Widget::clearChildren() {
  while (!children_.empty()) (void)removeChild(*children_.back());
}

std::string Widget::path() const {
  // Root excluded; unnamed widgets are transparent.
  const Widget* chain[64];
  uint32_t n = 0;
  size_t len = 0;
  for (const Widget* w = this; w && w->parent_; w = w->parent_)
    if (!w->name_.empty() && n < 64) {
      chain[n++] = w;
      len += w->name_.size() + 1;
    }
  std::string p;
  p.reserve(len);
  for (uint32_t i = n; i-- > 0;) {
    if (!p.empty()) p += '/';
    p += chain[i]->name_;
  }
  return p;
}

Widget* Widget::findSegment(std::string_view segment) const noexcept {
  for (const auto& c : children_) {
    if (c->name_ == segment) return c.get();
    if (c->name_.empty())
      if (Widget* f = c->findSegment(segment)) return f;
  }
  return nullptr;
}

Widget* Widget::find(std::string_view relativePath) const noexcept {
  Widget* cur = const_cast<Widget*>(this);
  size_t pos = 0;
  while (pos < relativePath.size()) {
    size_t slash = relativePath.find('/', pos);
    if (slash == std::string_view::npos) slash = relativePath.size();
    cur = cur->findSegment(relativePath.substr(pos, slash - pos));
    if (!cur) return nullptr;
    pos = slash + 1;
  }
  return cur;
}

bool Widget::isAncestorOf(const Widget& w) const noexcept {
  for (const Widget* p = w.parent_; p; p = p->parent_)
    if (p == this) return true;
  return false;
}

Widget& Widget::setWidth(Length l) {
  if (!(width_ == l)) {
    width_ = l;
    invalidateLayout();
  }
  return *this;
}
Widget& Widget::setHeight(Length l) {
  if (!(height_ == l)) {
    height_ = l;
    invalidateLayout();
  }
  return *this;
}
Widget& Widget::setSize(Length w, Length h) {
  setWidth(w);
  return setHeight(h);
}
Widget& Widget::setLayout(LayoutKind k) {
  layout_ = k;
  invalidateArrange();
  return *this;
}
Widget& Widget::setPadding(Insets p) {
  padding_ = p;
  invalidateArrange();
  return *this;
}
Widget& Widget::setGap(int16_t g) {
  gap_ = g;
  invalidateArrange();
  return *this;
}
Widget& Widget::setAlign(Align main, Align cross) {
  mainAlign_ = main;
  crossAlign_ = cross;
  invalidateArrange();
  return *this;
}

void Widget::invalidateLayout() noexcept {
  Widget* target = parent_ ? parent_ : this;
  if (tree_) {
    tree_->layoutDirty(*target);
  } else {
    target->flags_ |= kArrangeDirty;
  }
}

void Widget::invalidateArrange() noexcept {
  if (tree_) {
    tree_->layoutDirty(*this);
  } else {
    flags_ |= kArrangeDirty;
  }
}

void Widget::setVisible(bool v) {
  if (v == visible()) return;
  if (!v) {
    if (tree_) {
      tree_->damageWidget(*this);
      tree_->clearInteraction(*this);
    }
    rect_ = {};  // showing again is then a rect change: damaged and fully re-arranged
  }
  set(kVisible, v);
  invalidateLayout();
}

void Widget::setEnabled(bool e) {
  if (e == enabled()) return;
  set(kEnabled, e);
  if (!e && tree_) tree_->clearInteraction(*this);
  markDirty();
}

void Widget::setVisualOverflow(Insets logicalPx) noexcept {
  overflow_ = logicalPx;
  if (tree_) tree_->noteOverflow(logicalPx);
}

void Widget::markDirty() noexcept {
  if (tree_) tree_->damageWidget(*this);
}

void Widget::requestFrame() {
  if (tree_) tree_->requestFrame(*this);
}

uint32_t Widget::requestTimer(double delaySeconds) {
  if (!tree_) return 0;
  const uint32_t id = tree_->nextTimerId_++;
  tree_->timers_.push_back({tree_->now_ + std::max(0.0, delaySeconds), this, id});
  return id;
}

void Widget::cancelTimer(uint32_t id) noexcept {
  if (!tree_) return;
  auto& t = tree_->timers_;
  t.erase(std::remove_if(t.begin(), t.end(), [&](const UiTree::Timer& x) { return x.id == id && x.widget == this; }), t.end());
}

ISize Widget::contentExtent(ISize viewport) const {
  const float s = tree_ ? tree_->scale() : 1.0f;
  auto axis = [s](const Length& l, int32_t view) {
    const int32_t lo = static_cast<int32_t>(std::lround(static_cast<float>(l.min) * s));
    const int32_t hi = l.max >= kUnbounded ? kUnbounded : static_cast<int32_t>(std::lround(static_cast<float>(l.max) * s));
    const int32_t v = l.kind == SizeKind::Fixed ? static_cast<int32_t>(std::lround(l.value * s)) : view;
    return std::clamp(v, lo, std::max(lo, hi));
  };
  return {axis(width_, viewport.w), axis(height_, viewport.h)};
}

InfoText infoFor(const Widget* w) {
  for (; w; w = w->parent()) {
    const InfoText t = w->info();
    if (!t.empty()) return t;
  }
  return {};
}

// --- Splitter -------------------------------------------------------------------------------------------

Splitter::Splitter(std::string name, Axis axis, float position, Anchor anchor)
    : Widget(std::move(name)), axis_(axis), anchor_(anchor), position_(position) {
  setLayout(LayoutKind::Custom);
}

void Splitter::setLimits(int32_t minAnchored, int32_t maxAnchored, int32_t minOther) {
  minAnchored_ = std::max(0, minAnchored);
  maxAnchored_ = std::max(minAnchored_, maxAnchored);
  minOther_ = std::max(0, minOther);
  setPosition(position_);
  invalidateArrange();
}

void Splitter::setHandleThickness(int32_t logicalPx) {
  thickness_ = std::max(0, logicalPx);
  invalidateArrange();
}

void Splitter::setPosition(float logicalPx) {
  const float p = std::clamp(logicalPx, static_cast<float>(minAnchored_), static_cast<float>(maxAnchored_));
  if (p == position_) return;
  position_ = p;
  invalidateArrange();
}

void Splitter::arrange(LayoutPass& pass) {
  const IRect r = rect();
  const bool hz = axis_ == Axis::Horizontal;
  const int32_t main = hz ? r.w : r.h;
  Widget* a = childCount() > 0 ? &child(0) : nullptr;
  Widget* b = childCount() > 1 ? &child(1) : nullptr;
  const bool showA = a && a->visible(), showB = b && b->visible();
  if (!showA || !showB) {  // one pane (or none): it takes everything, no handle
    handle_ = {};
    if (showA) pass.place(*a, r);
    if (showB) pass.place(*b, r);
    return;
  }
  auto bound = [&](int32_t v) { return v >= kUnbounded ? kUnbounded : pass.px(static_cast<float>(v)); };
  const int32_t t = std::min(pass.px(static_cast<float>(thickness_)), std::max(0, main));
  const int32_t avail = std::max(0, main - t);
  const int32_t lo = bound(minAnchored_);
  int32_t anchored = std::clamp(pass.px(position_), lo, std::max(lo, bound(maxAnchored_)));
  anchored = std::min(anchored, avail - bound(minOther_));  // the other pane keeps its minimum...
  anchored = std::max(anchored, lo);                        // ...unless that would break the anchored minimum
  anchored = std::clamp(anchored, 0, avail);
  anchoredPx_ = anchored;
  const int32_t first = anchor_ == Anchor::First ? anchored : avail - anchored;
  const int32_t second = avail - first;
  if (hz) {
    handle_ = {r.x + first, r.y, t, r.h};
    pass.place(*a, {r.x, r.y, first, r.h});
    pass.place(*b, {r.x + first + t, r.y, second, r.h});
  } else {
    handle_ = {r.x, r.y + first, r.w, t};
    pass.place(*a, {r.x, r.y, r.w, first});
    pass.place(*b, {r.x, r.y + first + t, r.w, second});
  }
}

bool Splitter::onPointer(const PointerEvent& e) {
  const bool hz = axis_ == Axis::Horizontal;
  const float coord = hz ? e.pos.x : e.pos.y;
  switch (e.type) {
    case PointerType::Down:
      if (e.button != PointerButton::Left || !handle_.contains(e.pos)) return false;
      dragging_ = true;
      dragStart_ = coord;
      dragStartPx_ = anchoredPx_;
      markDirty();
      return true;
    case PointerType::Move: {
      if (!dragging_) return false;
      const int32_t d = static_cast<int32_t>(std::lround(coord - dragStart_));
      const int32_t px = anchor_ == Anchor::First ? dragStartPx_ + d : dragStartPx_ - d;
      const float s = tree() ? tree()->scale() : 1.0f;
      setPosition(static_cast<float>(px) / s);
      return true;
    }
    case PointerType::Up:
      if (!dragging_ || e.button != PointerButton::Left) return false;
      dragging_ = false;
      markDirty();
      return true;
    default: return false;
  }
}

void Splitter::onCaptureLost() { dragging_ = false; }

void Splitter::paint(PaintContext& ctx) {
  if (alphaOf(handleColor_) != 0 && !handle_.empty()) ctx.dl.addRectFilled(handle_.toRect(), handleColor_);
}

// --- ScrollView -----------------------------------------------------------------------------------------

ScrollView::ScrollView(std::string name, bool horizontal, bool vertical)
    : Widget(std::move(name)), horizontal_(horizontal), vertical_(vertical) {
  setLayout(LayoutKind::Custom);
  setClipChildren(true);
}

ScrollView::~ScrollView() {
  for (Dependent*& d : dependents_)
    if (d) {
      Dependent* x = d;
      d = nullptr;
      x->onScrollViewGone();
    }
}

bool ScrollView::linkDependent(Dependent* d) noexcept {
  for (Dependent*& s : dependents_)
    if (s == d) return true;
  for (Dependent*& s : dependents_)
    if (!s) {
      s = d;
      return true;
    }
  return false;
}

void ScrollView::unlinkDependent(Dependent* d) noexcept {
  for (Dependent*& s : dependents_)
    if (s == d) s = nullptr;
}

void ScrollView::notifyDependents() noexcept {
  for (Dependent* d : dependents_)
    if (d) d->onScrollChanged();
}

void ScrollView::scrollTo(int32_t x, int32_t y) {
  const IRect v = rect();
  x = std::clamp(x, 0, std::max(0, content_.w - v.w));
  y = std::clamp(y, 0, std::max(0, content_.h - v.h));
  if (!horizontal_) x = 0;
  if (!vertical_) y = 0;
  if (x == scrollX_ && y == scrollY_) return;
  scrollX_ = x;
  scrollY_ = y;
  invalidateArrange();
  markDirty();
  notifyDependents();
}

void ScrollView::arrange(LayoutPass& pass) {
  const IRect v = rect();
  Widget* c = content();
  if (!c || !c->visible()) return;
  ISize ext = c->contentExtent({v.w, v.h});
  if (!horizontal_) ext.w = v.w;
  if (!vertical_) ext.h = v.h;
  content_ = ext;
  scrollX_ = std::clamp(scrollX_, 0, std::max(0, ext.w - v.w));
  scrollY_ = std::clamp(scrollY_, 0, std::max(0, ext.h - v.h));
  pass.place(*c, {v.x - scrollX_, v.y - scrollY_, ext.w, ext.h});
  // Arrange runs only when the viewport moved/resized or the content/scroll changed: scrollbars follow.
  notifyDependents();
}

bool ScrollView::onPointer(const PointerEvent& e) {
  if (e.type != PointerType::Wheel) return false;
  float dx = e.wheel.x, dy = e.wheel.y;
  if ((e.mods & mod::kShift) && dx == 0.0f) {  // Shift+wheel scrolls horizontally (wheel up = left)
    dx = -dy;
    dy = 0.0f;
  }
  const float step = wheelStep_ * (tree() ? tree()->scale() : 1.0f);
  const int32_t x = scrollX_ + static_cast<int32_t>(std::lround(dx * step));
  const int32_t y = scrollY_ - static_cast<int32_t>(std::lround(dy * step));
  const int32_t ox = scrollX_, oy = scrollY_;
  scrollTo(x, y);
  return scrollX_ != ox || scrollY_ != oy;  // at a limit: let an outer scroll view have it
}

}  // namespace pq::ui
