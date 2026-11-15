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

// The tooltip popup: small text on the tooltip fill, one line per '\n' (T4.15: the circuit editor's node tooltips).
// Never hit.
class TooltipWidget : public Widget {
public:
  explicit TooltipWidget(std::string text) : Widget("tooltip"), text_(std::move(text)) {
    setHittable(false);
    setVisualOverflow(kPopupOverflow);
    for (size_t b = 0;;) {
      const size_t e = text_.find('\n', b);
      lines_.emplace_back(text_.data() + b, (e == std::string::npos ? text_.size() : e) - b);
      if (e == std::string::npos) break;
      b = e + 1;
    }
  }
  ISize contentExtent(ISize) const override {
    const UiScale s = tree() ? tree()->uiScale() : UiScale{};
    const PaintResources* res = tree() ? tree()->resources() : nullptr;
    int32_t w = 0;
    for (const std::string_view l : lines_) {
      int32_t lw = static_cast<int32_t>(l.size()) * s.px(7);
      if (res && res->text && res->uiFonts) lw = res->text->measure(res->uiFonts->small, l);
      w = std::max(w, lw);
    }
    return {w + 2 * s.px(6), s.px(4) + static_cast<int32_t>(lines_.size()) * s.px(16)};
  }
  void paint(PaintContext& ctx) override {
    const Theme* th = ctx.res.theme;
    if (!th) return;
    const IRect r = rect();
    popupFrame(ctx, r, *th, th->tooltip.fill, th->tooltip.border);
    const ThemeFonts* f = ctx.res.uiFonts;
    if (!f) return;
    const int32_t lh = ctx.scale.px(16);
    for (size_t i = 0; i < lines_.size(); ++i) {
      const IRect box{r.x, r.y + ctx.scale.px(2) + static_cast<int32_t>(i) * lh, r.w, lh};
      detail::drawText(ctx, f->small, box, lines_[i], th->tooltip.text, TextAlign::Left, ctx.scale.px(6), ctx.scale.px(4));
    }
  }

private:
  std::string text_;
  std::vector<std::string_view> lines_;  // into text_
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
    case Placement::Free: break;  // clamped below, never flipped
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
  // TX.panel-faces: a tree drawn inside another tree's widget opens its popups in the outer tree's layer.
  if (const UiTree* t = w.tree(); t != nullptr && t->host() != nullptr)
    if (const Widget* h = t->host()->hostWidget()) return of(*h);
  const Widget* root = rootOf(w);
  if (root->childCount() == 0) return nullptr;
  return dynamic_cast<OverlayLayer*>(&root->child(root->childCount() - 1));
}

IRect OverlayLayer::anchorRect(const Widget& w, const IRect& r) noexcept {
  const UiTree* t = w.tree();
  return t != nullptr && t->host() != nullptr ? t->host()->toHost(r) : r;
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
    IRect area = vp;
    if (e.options.within != nullptr) {
      const IRect in = intersect(vp, e.options.within->rect());  // per axis: only where the popup fits
      if (sz.w <= in.w) area.x = in.x, area.w = in.w;
      if (sz.h <= in.h) area.y = in.y, area.h = in.h;
    }
    pass.place(*e.popup, placeRect(area, e.options.anchor, e.options.placement, sz));
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
  if (!hovered || hovered == this) return;
  // No tooltips while a menu or a popover is open. A floating window (no focus taken, no outside dismiss: KB.help2)
  // leaves the app under it working, its tooltips included.
  for (const Entry& e : entries_)
    if (e.options.takeFocus || e.options.dismissOnOutsideClick) return;
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
    if ((it.swatch >> 24) != 0) {  // right of the tick, inside the gutter
      const float dot = ctx.scale.exact(3.5f);
      ctx.dl.addCircleFilled({static_cast<float>(ir.x + m.gutter) - ctx.scale.exact(4.5f), static_cast<float>(ir.y) + 0.5f * static_cast<float>(ir.h)},
                             dot, ok ? it.swatch : th->text.disabled);
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
  // TMM.13-fix1 L16: `at` is a point of from's tree; a tree drawn inside another's widget (a hosted PanelFace) shows it
  // where its host draws it, as Dropdown's anchor (OverlayLayer::anchorRect).
  o.anchor = OverlayLayer::anchorRect(from, {static_cast<int32_t>(std::lround(at.x)), static_cast<int32_t>(std::lround(at.y)), 0, 0});
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

// --- FloatingWindow -------------------------------------------------------------------------------------

FloatingWindow::FloatingWindow(std::string name, std::string title, float minW, float minH)
    : Popover(std::move(name)), title_(std::move(title)), minW_(minW), minH_(minH) {
  setLayout(LayoutKind::Custom);
  setPadding({});
  close_ = &add<Button>("close", "\xC3\x97");  // ×
  close_->setFocusable(false);
  close_->setTooltip("Close");
  close_->onClick = [this] {
    if (OverlayLayer* layer = OverlayLayer::of(*this)) layer->close(*this);
  };
}

void FloatingWindow::setTitle(std::string t) {
  if (t == title_) return;
  title_ = std::move(t);
  markDirty();
}

OverlayOptions FloatingWindow::floatingOptions(const Geometry& g, float scale) {
  OverlayOptions o;
  o.anchor = {static_cast<int32_t>(std::lround(g.x * scale)), static_cast<int32_t>(std::lround(g.y * scale)), 0, 0};
  o.placement = Placement::Free;
  o.dismissOnOutsideClick = false;
  o.takeFocus = false;
  return o;
}

void FloatingWindow::setGeometry(const Geometry& g) {
  geo_ = g;
  geo_.w = std::max(g.w, minW_);
  geo_.h = std::max(g.h, minH_);
  const float sc = tree() ? tree()->scale() : 1.0f;
  if (OverlayLayer* layer = OverlayLayer::of(*this))
    (void)layer->move(*this, {static_cast<int32_t>(std::lround(geo_.x * sc)), static_cast<int32_t>(std::lround(geo_.y * sc)), 0, 0},
                      Placement::Free);
  if (parent()) parent()->invalidateArrange();  // a new size: the layer re-places it
}

void FloatingWindow::place(const IRect& d) {
  const float sc = tree() ? tree()->scale() : 1.0f;
  geo_ = {static_cast<float>(d.x) / sc, static_cast<float>(d.y) / sc, static_cast<float>(d.w) / sc, static_cast<float>(d.h) / sc};
  if (OverlayLayer* layer = OverlayLayer::of(*this)) (void)layer->move(*this, {d.x, d.y, 0, 0}, Placement::Free);
  if (parent()) parent()->invalidateArrange();
}

ISize FloatingWindow::contentExtent(ISize viewport) const {
  const float sc = tree() ? tree()->scale() : 1.0f;
  auto dev = [sc](float v) { return static_cast<int32_t>(std::lround(v * sc)); };
  return {std::min(viewport.w, std::max(dev(minW_), dev(geo_.w))), std::min(viewport.h, std::max(dev(minH_), dev(geo_.h)))};
}

IRect FloatingWindow::body() const noexcept {
  const IRect r = rect();
  const int32_t th = static_cast<int32_t>(std::lround(kTitleH * (tree() ? tree()->scale() : 1.0f)));
  return {r.x, r.y + th, r.w, std::max(0, r.h - th)};
}

void FloatingWindow::arrange(LayoutPass& pass) {
  const IRect r = rect();
  const int32_t th = pass.px(kTitleH), b = pass.px(18.0f);
  pass.place(*close_, {r.right() - pass.px(5.0f) - b, r.y + (th - b) / 2, b, b});
  arrangeBody(pass, body());
}

void FloatingWindow::paint(PaintContext& ctx) {
  Popover::paint(ctx);  // shadow, fill, border
  const Theme* th = ctx.res.theme;
  if (!th) return;
  const IRect r = rect();
  const int32_t tb = ctx.scale.px(kTitleH);
  const bool active = tree() && inSubtree(*this, tree()->focus());
  const float rad = ctx.scale.exact(th->radius.menu);
  // The title bar: the raised surface while the window has the focus.
  const Rect bar{static_cast<float>(r.x) + 1.0f, static_cast<float>(r.y) + 1.0f, static_cast<float>(r.right()) - 1.0f, static_cast<float>(r.y + tb)};
  ctx.dl.addRoundedRectFilled(bar, std::max(0.0f, rad - 1.0f), active ? th->surface.raised : th->surface.panel);
  ctx.dl.addRectFilled({bar.x0, bar.y1 - std::min(rad, static_cast<float>(tb) / 2.0f), bar.x1, bar.y1}, active ? th->surface.raised : th->surface.panel);
  ctx.dl.addHLine(static_cast<float>(r.x) + 1.0f, static_cast<float>(r.right()) - 1.0f, static_cast<float>(r.y + tb), th->menu.border);
  if (const ThemeFonts* f = ctx.res.uiFonts) {
    const IRect tr{r.x, r.y, std::max(0, close_->rect().x - r.x), tb};
    detail::drawText(ctx, f->bodyStrong, tr, title_, th->text.primary, TextAlign::Left, ctx.scale.px(10), ctx.scale.px(8));
  }
}

void FloatingWindow::paintOver(PaintContext& ctx) {
  const Theme* th = ctx.res.theme;
  if (!th) return;
  // The bottom-right grip: a triangle of six dots.
  const IRect r = rect();
  const float dot = std::max(1.0f, std::round(2.0f * ctx.scale.factor)), step = std::round(4.0f * ctx.scale.factor);
  const float x1 = static_cast<float>(r.right()) - std::round(3.0f * ctx.scale.factor), y1 = static_cast<float>(r.bottom()) - std::round(3.0f * ctx.scale.factor);
  const Rgba c = mulAlpha(th->text.secondary, 0.6f);
  for (int a = 0; a < 3; ++a)
    for (int b = 0; a + b < 3; ++b) {
      const float x = x1 - static_cast<float>(a) * step, y = y1 - static_cast<float>(b) * step;
      ctx.dl.addRectFilled({x - dot, y - dot, x, y}, c);
    }
}

uint8_t FloatingWindow::zoneAt(Vec2 p) const noexcept {
  const IRect r = rect();
  if (!r.contains(p)) return 0;
  const float sc = tree() ? tree()->scale() : 1.0f;
  const float e = kEdge * sc, g = kGrip * sc;
  const float l = p.x - static_cast<float>(r.x), rt = static_cast<float>(r.right()) - p.x;
  const float t = p.y - static_cast<float>(r.y), bt = static_cast<float>(r.bottom()) - p.y;
  uint8_t z = 0;
  if (l < e) z |= kLeft;
  else if (rt <= e) z |= kRight;
  if (t < e) z |= kTop;
  else if (bt <= e) z |= kBottom;
  // Near a corner along an edge: the corner.
  if ((z & (kLeft | kRight)) && !(z & (kTop | kBottom))) z |= t < g ? kTop : bt <= g ? kBottom : 0;
  if ((z & (kTop | kBottom)) && !(z & (kLeft | kRight))) z |= l < g ? kLeft : rt <= g ? kRight : 0;
  if (rt <= g && bt <= g) z = kRight | kBottom;  // the grip
  if (z) return z;
  return t < kTitleH * sc ? kMove : 0;
}

bool FloatingWindow::onPointer(const PointerEvent& e) {
  if (e.type == PointerType::Down && e.button == PointerButton::Left) {
    drag_ = zoneAt(e.pos);
    press_ = e.pos;
    pressRect_ = rect();
    if (drag_ != 0) return true;
  }
  if (e.type == PointerType::Move && drag_ != 0) {
    const OverlayLayer* layer = OverlayLayer::of(*this);
    const IRect vp = layer ? layer->rect() : IRect{0, 0, pressRect_.right(), pressRect_.bottom()};
    const float sc = tree() ? tree()->scale() : 1.0f;
    const int32_t dx = static_cast<int32_t>(std::lround(e.pos.x - press_.x)), dy = static_cast<int32_t>(std::lround(e.pos.y - press_.y));
    const IRect p = pressRect_;
    IRect n = p;
    if (drag_ == kMove) {
      n.x = std::clamp(p.x + dx, vp.x, std::max(vp.x, vp.right() - p.w));
      n.y = std::clamp(p.y + dy, vp.y, std::max(vp.y, vp.bottom() - p.h));
    } else {
      // Each moved edge stays inside the layer and at least the minimum size from the opposite one (the minimum wins
      // when the layer is smaller than it).
      const int32_t mw = static_cast<int32_t>(std::lround(minW_ * sc)), mh = static_cast<int32_t>(std::lround(minH_ * sc));
      auto within = [](int32_t v, int32_t lo, int32_t hi) { return std::max(lo, std::min(v, hi)); };
      int32_t x0 = p.x, x1 = p.right(), y0 = p.y, y1 = p.bottom();
      if (drag_ & kLeft) x0 = std::min(within(p.x + dx, vp.x, x1 - mw), x1 - mw);
      if (drag_ & kRight) x1 = within(p.right() + dx, x0 + mw, vp.right());
      if (drag_ & kTop) y0 = std::min(within(p.y + dy, vp.y, y1 - mh), y1 - mh);
      if (drag_ & kBottom) y1 = within(p.bottom() + dy, y0 + mh, vp.bottom());
      n = {x0, y0, x1 - x0, y1 - y0};
    }
    if (n != rect()) place(n);
    return true;
  }
  if (e.type == PointerType::Up && drag_ != 0) {
    drag_ = 0;
    if (rect() != pressRect_ && onGeometry) onGeometry(*this);
    return true;
  }
  if (e.type == PointerType::Wheel) return true;  // nothing under the window scrolls
  return Popover::onPointer(e);
}

void FloatingWindow::onCaptureLost() {
  if (drag_ == 0) return;
  drag_ = 0;
  if (rect() != pressRect_ && onGeometry) onGeometry(*this);  // where the user left it
}

Cursor FloatingWindow::cursorAt(Vec2 p) const {
  const uint8_t z = drag_ != 0 ? drag_ : zoneAt(p);
  switch (z) {
    case kMove: return Cursor::Move;
    case kLeft:
    case kRight: return Cursor::ResizeEW;
    case kTop:
    case kBottom: return Cursor::ResizeNS;
    case kLeft | kTop:
    case kRight | kBottom: return Cursor::ResizeNWSE;
    case kRight | kTop:
    case kLeft | kBottom: return Cursor::ResizeNESW;
    default: return Popover::cursorAt(p);
  }
}

}  // namespace pq::ui
