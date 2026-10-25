// OverlayLayer (popups, outside-click dismiss, tooltips), Menu (nested, keyboard navigable) and Popover.

#include "pq/ui/overlay.h"

#include <algorithm>
#include <cmath>

#include "controls_paint.h"
#include "pq/ui/actions.h"
#include "pq/ui/theme.h"

namespace pq::ui {

namespace {

bool inSubtree(const Widget& root, const Widget* w) noexcept { return w && (w == &root || root.isAncestorOf(*w)); }

const Widget* rootOf(const Widget& w) noexcept {
  const Widget* r = &w;
  while (r->parent()) r = r->parent();
  return r;
}

// Drop shadow + fill + border shared by menus, popovers and tooltips. The shadow reaches 1 px left/right and
// 2 logical px + 1 below the rect: popups declare it (kPopupOverflow) so partial redraw damages and repaints it.
constexpr Insets kPopupOverflow{1, 1, 1, 3};
void popupFrame(PaintContext& ctx, const IRect& r, const Theme& th, Rgba fill, Rgba border) {
  const float rad = ctx.scale.exact(th.radius.menu);
  const float d = ctx.scale.exact(2.0f);
  const Rect s1{static_cast<float>(r.x) - 1.0f, static_cast<float>(r.y) + d - 1.0f, static_cast<float>(r.right()) + 1.0f,
                static_cast<float>(r.bottom()) + d + 1.0f};
  ctx.dl.addRoundedRectFilled(s1, rad + 1.0f, mulAlpha(th.menu.shadow, 0.5f));
  const Rect s2{static_cast<float>(r.x), static_cast<float>(r.y) + d, static_cast<float>(r.right()), static_cast<float>(r.bottom()) + d};
  ctx.dl.addRoundedRectFilled(s2, rad, th.menu.shadow);
  detail::box(ctx.dl, r, rad, fill, border);
}

// The tooltip popup: one line of small text on the tooltip fill. Never hit.
class TooltipWidget : public Widget {
public:
  explicit TooltipWidget(std::string text) : Widget("tooltip"), text_(std::move(text)) {
    setHittable(false);
    setVisualOverflow(kPopupOverflow);
  }
  ISize contentExtent(ISize) const override {
    const UiScale s = tree() ? tree()->uiScale() : UiScale{};
    const PaintResources* res = tree() ? tree()->resources() : nullptr;
    int32_t w = static_cast<int32_t>(text_.size()) * s.px(7);
    if (res && res->text && res->uiFonts) w = res->text->measure(res->uiFonts->small, text_);
    return {w + 2 * s.px(6), s.px(20)};
  }
  void paint(PaintContext& ctx) override {
    const Theme* th = ctx.res.theme;
    if (!th) return;
    popupFrame(ctx, rect(), *th, th->tooltip.fill, th->tooltip.border);
    if (const ThemeFonts* f = ctx.res.uiFonts)
      detail::drawText(ctx, f->small, rect(), text_, th->tooltip.text, TextAlign::Left, ctx.scale.px(6), ctx.scale.px(4));
  }

private:
  std::string text_;
};

IRect placeRect(const IRect& vp, const IRect& a, Placement p, ISize sz) {
  int32_t x = a.x, y = a.y;
  switch (p) {
    case Placement::Below:
      y = a.bottom();
      if (y + sz.h > vp.bottom() && a.y - sz.h >= vp.y) y = a.y - sz.h;  // flip above
      break;
    case Placement::Right:
      x = a.right();
      if (x + sz.w > vp.right() && a.x - sz.w >= vp.x) x = a.x - sz.w;  // flip left
      break;
    case Placement::AtPoint:
      if (x + sz.w > vp.right() && a.x - sz.w >= vp.x) x = a.x - sz.w;
      if (y + sz.h > vp.bottom() && a.y - sz.h >= vp.y) y = a.y - sz.h;
      break;
  }
  x = std::clamp(x, vp.x, std::max(vp.x, vp.right() - sz.w));
  y = std::clamp(y, vp.y, std::max(vp.y, vp.bottom() - sz.h));
  return {x, y, std::min(sz.w, vp.w), std::min(sz.h, vp.h)};
}

}  // namespace

// --- OverlayLayer ---------------------------------------------------------------------------------------

OverlayLayer::OverlayLayer(std::string name) : Widget(std::move(name)) {
  setLayout(LayoutKind::Custom);
  setHittable(false);
}

OverlayLayer::~OverlayLayer() {
  if (observing_) observing_->removeObserver(this);
  // Owners learn that their popups are gone (e.g. a Dropdown forgets its menu); the popups die with the layer.
  std::vector<Entry> entries;
  entries.swap(entries_);
  for (Entry& e : entries)
    if (e.options.onClosed) e.options.onClosed();
}

OverlayLayer* OverlayLayer::of(const Widget& w) noexcept {
  const Widget* root = rootOf(w);
  if (root->childCount() == 0) return nullptr;
  return dynamic_cast<OverlayLayer*>(&root->child(root->childCount() - 1));
}

std::string OverlayLayer::uniqueName(std::string_view base) const {
  std::string n(base);
  for (int k = 2; find(n) != nullptr; ++k) n = std::string(base) + "-" + std::to_string(k);
  return n;
}

void OverlayLayer::observe() {
  if (tree() == observing_) return;
  if (observing_) observing_->removeObserver(this);
  observing_ = tree();
  if (observing_) observing_->addObserver(this);
}

void OverlayLayer::updateHittable() noexcept {
  bool h = false;
  for (const Entry& e : entries_) h = h || e.options.dismissOnOutsideClick;
  setHittable(h);
}

Widget& OverlayLayer::open(std::unique_ptr<Widget> popup, OverlayOptions options) {
  observe();
  hideTooltip();
  Widget* restore = tree() ? tree()->focus() : nullptr;
  Widget& w = addChild(std::move(popup));
  const bool takeFocus = options.takeFocus;
  entries_.push_back({&w, std::move(options), restore});
  updateHittable();
  if (takeFocus && tree()) {
    if (!w.focusable()) w.setFocusable(true);
    tree()->setFocus(&w);
  }
  w.markDirty();
  return w;
}

bool OverlayLayer::isOpen(const Widget& popup) const noexcept {
  return std::any_of(entries_.begin(), entries_.end(), [&](const Entry& e) { return e.popup == &popup; });
}

void OverlayLayer::close(Widget& popup) {
  auto it = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& e) { return e.popup == &popup; });
  if (it == entries_.end()) return;
  const size_t from = static_cast<size_t>(it - entries_.begin());
  std::vector<std::function<void()>> callbacks;
  UiTree* t = tree();
  while (entries_.size() > from) {
    Entry e = std::move(entries_.back());
    entries_.pop_back();
    const bool hadFocus = t && inSubtree(*e.popup, t->focus());
    graveyard_.push_back(removeChild(*e.popup));  // destroyed at the next frame: callers may be inside it
    if (t && e.options.takeFocus && (hadFocus || !t->focus())) {
      // Give focus back only to a widget that is still in the tree (it may have been a popup closed above).
      Widget* r = e.restoreFocus;
      const Widget* root = t->root();
      bool valid = false;
      if (r && root) {
        // Linear walk: closing is rare, and it never touches a destroyed widget.
        std::vector<const Widget*> stack{root};
        while (!stack.empty() && !valid) {
          const Widget* w = stack.back();
          stack.pop_back();
          if (w == r) valid = true;
          for (size_t i = 0; i < w->childCount(); ++i) stack.push_back(&w->child(i));
        }
      }
      t->setFocus(valid ? r : nullptr);
    }
    if (e.options.onClosed) callbacks.push_back(std::move(e.options.onClosed));
  }
  updateHittable();
  requestFrame();
  for (auto& cb : callbacks) cb();
}

void OverlayLayer::closeAll() {
  if (!entries_.empty()) close(*entries_.front().popup);
}

bool OverlayLayer::move(Widget& popup, const IRect& anchor, Placement placement) {
  for (Entry& e : entries_)
    if (e.popup == &popup) {
      if (e.options.anchor == anchor && e.options.placement == placement) return true;
      e.options.anchor = anchor;
      e.options.placement = placement;
      invalidateArrange();
      return true;
    }
  return false;
}

void OverlayLayer::arrange(LayoutPass& pass) {
  observe();
  const IRect vp = rect();
  for (const Entry& e : entries_) {
    const ISize sz = e.popup->contentExtent({vp.w, vp.h});
    pass.place(*e.popup, placeRect(vp, e.options.anchor, e.options.placement, sz));
  }
  if (tooltip_) {
    const ISize sz = tooltip_->contentExtent({vp.w, vp.h});
    const IRect a{static_cast<int32_t>(std::lround(tooltipPos_.x)), static_cast<int32_t>(std::lround(tooltipPos_.y)), 0, 0};
    pass.place(*tooltip_, placeRect(vp, a, Placement::AtPoint, sz));
  }
}

bool OverlayLayer::onPointer(const PointerEvent& e) {
  // Reached only when the event missed every popup (popups consume their own presses).
  if (e.type == PointerType::Down || e.type == PointerType::Move) {
    for (size_t i = entries_.size(); i-- > 0;) {
      if (!entries_[i].options.onOutsidePointer) continue;
      const std::function<bool(const PointerEvent&)> fn = entries_[i].options.onOutsidePointer;  // may close it
      if (fn(e) && e.type == PointerType::Down) return true;
      break;
    }
  }
  if (e.type == PointerType::Down) {
    for (const Entry& en : entries_)
      if (en.options.dismissOnOutsideClick) {
        close(*en.popup);
        break;
      }
    return true;
  }
  return e.type == PointerType::Up && captured();
}

void OverlayLayer::onFrame(double) { graveyard_.clear(); }

void OverlayLayer::onHoverChanged(Widget* hovered) {
  if (tooltipTimer_) {
    cancelTimer(tooltipTimer_);
    tooltipTimer_ = 0;
  }
  if (hovered != tooltipFor_) hideTooltip();
  tooltipFor_ = nullptr;
  if (!hovered || hovered == this || !entries_.empty()) return;
  if (hovered->tooltip().empty()) return;
  tooltipFor_ = hovered;
  tooltipTimer_ = requestTimer(tooltipDelay_);
}

void OverlayLayer::onPointerDown(Vec2) {
  if (tooltipTimer_) {
    cancelTimer(tooltipTimer_);
    tooltipTimer_ = 0;
  }
  tooltipFor_ = nullptr;
  hideTooltip();
}

void OverlayLayer::onTimer(uint32_t id) {
  if (id != tooltipTimer_) return;
  tooltipTimer_ = 0;
  UiTree* t = tree();
  if (!t || !tooltipFor_ || t->hovered() != tooltipFor_) return;  // compared, never dereferenced unless current
  const Vec2 p = t->pointerPos();
  const float s = t->scale();
  showTooltip(tooltipFor_->tooltip(), {p.x + 10.0f * s, p.y + 18.0f * s});
}

void OverlayLayer::showTooltip(std::string_view text, Vec2 p) {
  hideTooltip();
  tooltipPos_ = p;
  tooltip_ = &addChild(std::make_unique<TooltipWidget>(std::string(text)));
  tooltip_->markDirty();
}

void OverlayLayer::hideTooltip() {
  if (!tooltip_) return;
  Widget* t = tooltip_;
  tooltip_ = nullptr;
  graveyard_.push_back(removeChild(*t));
  requestFrame();
}

// --- MenuItem -------------------------------------------------------------------------------------------

MenuItem MenuItem::item(std::string label, std::function<void()> onChoose, std::string shortcut) {
  MenuItem m;
  m.label = std::move(label);
  m.onChoose = std::move(onChoose);
  m.shortcut = std::move(shortcut);
  return m;
}

MenuItem MenuItem::sub(std::string label, std::vector<MenuItem> items) {
  MenuItem m;
  m.label = std::move(label);
  m.submenu = std::move(items);
  return m;
}

MenuItem MenuItem::sep() {
  MenuItem m;
  m.separator = true;
  return m;
}

// --- Menu -----------------------------------------------------------------------------------------------

namespace {
struct MenuMetrics {
  int32_t pad, item, sep, gutter, gap, right, arrow;
};
MenuMetrics menuMetrics(const UiScale& s, const Theme* th) {
  const float item = th ? th->size.menuItem : 20.0f;
  return {s.px(4), s.px(item), s.px(9), s.px(24), s.px(28), s.px(12), s.px(14)};
}
}  // namespace

Menu::Menu(std::vector<MenuItem> items, std::string name) : Control(std::move(name)), items_(std::move(items)) {
  setFocusable(true);
  setVisualOverflow(kPopupOverflow);
}

void Menu::setMinWidthPx(int32_t w) {
  minWidth_ = std::max(0, w);
  invalidateLayout();
}

int32_t Menu::itemHeight(int32_t i) const noexcept {
  const UiScale s = tree() ? tree()->uiScale() : UiScale{};
  const PaintResources* res = tree() ? tree()->resources() : nullptr;
  const MenuMetrics m = menuMetrics(s, res ? res->theme : nullptr);
  return items_[static_cast<size_t>(i)].separator ? m.sep : m.item;
}

ISize Menu::contentExtent(ISize) const {
  const UiScale s = tree() ? tree()->uiScale() : UiScale{};
  const PaintResources* res = tree() ? tree()->resources() : nullptr;
  const MenuMetrics m = menuMetrics(s, res ? res->theme : nullptr);
  const bool measure = res && res->text && res->uiFonts;
  int32_t label = 0, shortcut = 0, h = 2 * m.pad;
  bool arrows = false;
  for (const MenuItem& it : items_) {
    h += it.separator ? m.sep : m.item;
    if (it.separator) continue;
    label = std::max(label, measure ? res->text->measure(res->uiFonts->body, it.label) : static_cast<int32_t>(it.label.size()) * s.px(7));
    if (!it.shortcut.empty())
      shortcut = std::max(shortcut, measure ? res->text->measure(res->uiFonts->small, it.shortcut) : static_cast<int32_t>(it.shortcut.size()) * s.px(6));
    arrows = arrows || !it.submenu.empty();
  }
  const int32_t w = m.gutter + label + (shortcut ? m.gap + shortcut : 0) + (arrows ? m.arrow : 0) + m.right;
  return {std::max(w, minWidth_), h};
}

IRect Menu::itemRect(int32_t i) const noexcept {
  if (i < 0 || i >= static_cast<int32_t>(items_.size())) return {};
  const UiScale s = tree() ? tree()->uiScale() : UiScale{};
  const PaintResources* res = tree() ? tree()->resources() : nullptr;
  const MenuMetrics m = menuMetrics(s, res ? res->theme : nullptr);
  const IRect r = rect();
  int32_t y = r.y + m.pad;
  for (int32_t k = 0; k < i; ++k) y += items_[static_cast<size_t>(k)].separator ? m.sep : m.item;
  return {r.x + 1, y, std::max(0, r.w - 2), itemHeight(i)};
}

int32_t Menu::itemAt(Vec2 p) const noexcept {
  for (int32_t i = 0; i < static_cast<int32_t>(items_.size()); ++i)
    if (itemRect(i).contains(p)) return i;
  return -1;
}

bool Menu::selectable(int32_t i) const noexcept {
  if (i < 0 || i >= static_cast<int32_t>(items_.size())) return false;
  const MenuItem& it = items_[static_cast<size_t>(i)];
  if (it.separator || !it.enabled) return false;
  if (!it.action.empty() && tree() && tree()->actions()) {
    const ActionIndex a = tree()->actions()->find(it.action);
    if (a != kNoAction && !tree()->actions()->enabled(a)) return false;
  }
  return true;
}

int32_t Menu::step(int32_t from, int32_t dir) const noexcept {
  const auto n = static_cast<int32_t>(items_.size());
  if (n == 0) return -1;
  int32_t i = from;
  for (int32_t k = 0; k < n; ++k) {
    i = (i < 0) ? (dir > 0 ? 0 : n - 1) : (i + dir + n) % n;
    if (selectable(i)) return i;
  }
  return -1;
}

void Menu::setHighlighted(int32_t i) {
  if (i == hi_) return;
  hi_ = i;
  if (child_) closeSubmenu();
  markDirty();
}

Menu* Menu::root() noexcept {
  Menu* m = this;
  while (m->parent_) m = m->parent_;
  return m;
}

Menu* Menu::openSubmenu() {
  if (hi_ < 0 || items_[static_cast<size_t>(hi_)].submenu.empty() || !selectable(hi_)) return nullptr;
  if (child_) return child_;
  OverlayLayer* layer = OverlayLayer::of(*this);
  if (!layer) return nullptr;
  const UiScale s = tree() ? tree()->uiScale() : UiScale{};
  const MenuMetrics m = menuMetrics(s, tree() && tree()->resources() ? tree()->resources()->theme : nullptr);
  auto sub = std::make_unique<Menu>(items_[static_cast<size_t>(hi_)].submenu, layer->uniqueName("submenu"));
  sub->parent_ = this;
  OverlayOptions o;
  const IRect ir = itemRect(hi_);
  o.anchor = {rect().x, ir.y - m.pad, rect().w, ir.h};
  o.placement = Placement::Right;
  o.onClosed = [this] { child_ = nullptr; };  // `this` outlives its submenu (closed first; graveyard)
  child_ = &static_cast<Menu&>(layer->open(std::move(sub), std::move(o)));
  child_->setHighlighted(child_->step(-1, +1));
  markDirty();
  return child_;
}

void Menu::closeSubmenu() {
  if (!child_) return;
  if (OverlayLayer* layer = OverlayLayer::of(*this)) layer->close(*child_);
  child_ = nullptr;
}

void Menu::choose(int32_t i) {
  if (!selectable(i)) return;
  setHighlighted(i);
  const MenuItem& it = items_[static_cast<size_t>(i)];
  if (!it.submenu.empty()) {
    openSubmenu();
    return;
  }
  // Copy what runs: closing moves the whole chain to the graveyard (alive until the next frame), and the
  // callback may open another menu.
  const std::string action = it.action;
  const std::function<void()> cb = it.onChoose;
  UiTree* t = tree();
  if (OverlayLayer* layer = OverlayLayer::of(*this)) layer->close(*root());
  if (t && !action.empty() && t->actions()) {
    const ActionIndex a = t->actions()->find(action);
    if (a != kNoAction) t->actions()->invoke(a);
  }
  if (cb) cb();
}

bool Menu::onPointer(const PointerEvent& e) {
  switch (e.type) {
    case PointerType::Move: {
      const int32_t i = itemAt(e.pos);
      if (i < 0 || !selectable(i) || i == hi_) return true;
      setHighlighted(i);
      if (openTimer_) cancelTimer(openTimer_);
      openTimer_ = 0;
      if (!items_[static_cast<size_t>(i)].submenu.empty()) {
        pendingOpen_ = i;
        openTimer_ = requestTimer(0.25);  // a short rest opens the submenu (passing over it does not)
      }
      return true;
    }
    case PointerType::Down: return true;
    case PointerType::Up: {
      const int32_t i = itemAt(e.pos);
      if (i >= 0 && selectable(i)) choose(i);
      return true;
    }
    default: return false;
  }
}

void Menu::onTimer(uint32_t id) {
  if (id != openTimer_) return;
  openTimer_ = 0;
  if (pendingOpen_ == hi_) openSubmenu();
}

bool Menu::onKey(const KeyEvent& e) {
  if (!e.down) return true;
  OverlayLayer* layer = OverlayLayer::of(*this);
  switch (e.key) {
    case key::kDown: setHighlighted(step(hi_, +1)); return true;
    case key::kUp: setHighlighted(step(hi_, -1)); return true;
    case key::kHome: setHighlighted(step(-1, +1)); return true;
    case key::kEnd: setHighlighted(step(-1, -1)); return true;
    case key::kRight:
      if (Menu* sub = openSubmenu(); sub && tree()) tree()->setFocus(sub, true);
      return true;
    case key::kLeft:
      if (parent_ && layer) layer->close(*this);  // back to the parent (it gets focus back)
      return true;
    case key::kEscape:
      if (layer) layer->close(*this);
      return true;
    case key::kReturn:
    case key::kSpace:
      if (!e.repeat && hi_ >= 0) {
        if (!items_[static_cast<size_t>(hi_)].submenu.empty()) {
          if (Menu* sub = openSubmenu(); sub && tree()) tree()->setFocus(sub, true);
        } else {
          choose(hi_);
        }
      }
      return true;
    default: return true;  // modal while open: nothing reaches the keymaps underneath
  }
}

void Menu::paint(PaintContext& ctx) {
  const Theme* th = ctx.res.theme;
  if (!th) return;
  const IRect r = rect();
  popupFrame(ctx, r, *th, th->menu.fill, th->menu.border);
  const MenuMetrics m = menuMetrics(ctx.scale, th);
  const ThemeFonts* f = ctx.res.uiFonts;
  for (int32_t i = 0; i < static_cast<int32_t>(items_.size()); ++i) {
    const MenuItem& it = items_[static_cast<size_t>(i)];
    const IRect ir = itemRect(i);
    if (it.separator) {
      ctx.dl.addHLine(static_cast<float>(ir.x + m.pad + 2), static_cast<float>(ir.right() - m.pad - 2), static_cast<float>(ir.y + ir.h / 2),
                      th->menu.separator);
      continue;
    }
    const bool ok = selectable(i);
    const bool hi = ok && i == hi_;
    if (hi) ctx.dl.addRectFilled(IRect{ir.x + 1, ir.y, std::max(0, ir.w - 2), ir.h}.toRect(), th->selection.fill);
    const Rgba ink = !ok ? th->text.disabled : hi ? th->selection.text : th->text.primary;
    if (it.checked) {
      // A tick in the gutter, as two strokes.
      const float x = static_cast<float>(ir.x) + ctx.scale.exact(7.0f), y = static_cast<float>(ir.y) + 0.5f * static_cast<float>(ir.h);
      const float u = ctx.scale.exact(1.0f), w = 0.5f * ctx.scale.exact(1.6f);
      auto seg = [&](Vec2 a, Vec2 b) {
        const float dx = b.x - a.x, dy = b.y - a.y, len = std::sqrt(dx * dx + dy * dy);
        const Vec2 n{-dy / len * w, dx / len * w};
        const Vec2 q[4] = {{a.x + n.x, a.y + n.y}, {b.x + n.x, b.y + n.y}, {b.x - n.x, b.y - n.y}, {a.x - n.x, a.y - n.y}};
        ctx.dl.addConvexPolyFilled(q, ink);
      };
      seg({x, y + 0.5f * u}, {x + 3.0f * u, y + 3.5f * u});
      seg({x + 3.0f * u, y + 3.5f * u}, {x + 9.0f * u, y - 3.5f * u});
    }
    if (f) {
      const int32_t rightPad = m.right + (it.submenu.empty() ? 0 : m.arrow);
      if (!it.shortcut.empty())
        detail::drawText(ctx, f->small, ir, it.shortcut, hi ? th->selection.text : (ok ? th->text.secondary : th->text.disabled), TextAlign::Right,
                         m.gutter, rightPad);
      detail::drawText(ctx, f->body, ir, it.label, ink, TextAlign::Left, m.gutter, rightPad);
    }
    if (!it.submenu.empty()) {
      const float ax = static_cast<float>(ir.right()) - ctx.scale.exact(10.0f), ay = static_cast<float>(ir.y) + 0.5f * static_cast<float>(ir.h);
      const float hw = ctx.scale.exact(2.5f), hh = ctx.scale.exact(4.0f);
      ctx.dl.addTriangleFilled({ax - hw, ay - hh}, {ax + hw, ay}, {ax - hw, ay + hh}, ink);
    }
  }
}

Menu* openContextMenu(Widget& from, Vec2 at, std::vector<MenuItem> items) {
  OverlayLayer* layer = OverlayLayer::of(from);
  if (!layer) return nullptr;
  OverlayOptions o;
  o.anchor = {static_cast<int32_t>(std::lround(at.x)), static_cast<int32_t>(std::lround(at.y)), 0, 0};
  o.placement = Placement::AtPoint;
  return &static_cast<Menu&>(layer->open(std::make_unique<Menu>(std::move(items), layer->uniqueName("context-menu")), std::move(o)));
}

// --- Popover --------------------------------------------------------------------------------------------

Popover::Popover(std::string name) : Control(std::move(name)) {
  setFocusable(true);
  setVisualOverflow(kPopupOverflow);
  setLayout(LayoutKind::Column);
  setPadding(Insets::all(8));
  setGap(6);
}

void Popover::paint(PaintContext& ctx) {
  const Theme* th = ctx.res.theme;
  if (!th) return;
  popupFrame(ctx, rect(), *th, th->menu.fill, th->menu.border);
}

bool Popover::onKey(const KeyEvent& e) {
  if (e.down && e.key == key::kEscape) {
    if (OverlayLayer* layer = OverlayLayer::of(*this)) layer->close(*this);
    return true;
  }
  return false;
}

bool Popover::onPointer(const PointerEvent& e) { return e.type == PointerType::Down || (e.type == PointerType::Up && captured()); }

}  // namespace pq::ui
