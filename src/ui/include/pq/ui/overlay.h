#pragma once

// Overlays (T3.2): menus (context menus, dropdown lists, nested submenus with keyboard navigation), anchored
// popovers with click-outside dismiss, and hover tooltips. They live in an OverlayLayer: a Custom-layout widget
// that must be the *last* child of the root (so it paints and hit-tests on top) and fills it:
//
//   auto& root = tree.emplaceRoot<Widget>();           // Stack layout (the default)
//   root.add<MainLayout>(...);                          // the app
//   auto& overlays = root.add<OverlayLayer>("overlay"); // last
//
// The layer is hittable only while a dismissable popup is open: then a press outside every popup closes them
// (and is consumed), like Live's menus. Popups are owned by the layer while open; closing one also closes every
// popup opened after it (its submenus). Closed popups are destroyed at the next frame, so a menu item's
// callback may close its own menu.
//
// Keyboard: a popup that takes focus gets it on open and gives it back on close. Menus: Up/Down/Home/End move
// the highlight (skipping separators and disabled items), Right / Enter on a submenu item opens it, Left / Esc
// closes a submenu, Esc closes the root menu, Enter / Space choose. Popovers: Esc dismisses.
//
// UI thread only.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "pq/ui/controls.h"
#include "pq/ui/widget.h"

namespace pq::ui {

class Menu;

enum class Placement : uint8_t {
  Below,    // under the anchor, left-aligned (flips above when there is no room)
  Right,    // to the right of the anchor, top-aligned (submenus; flips to the left)
  AtPoint,  // top-left at anchor.x, anchor.y (context menus; shifted to stay inside)
};

struct OverlayOptions {
  IRect anchor{};  // device px
  Placement placement = Placement::Below;
  bool dismissOnOutsideClick = true;
  bool takeFocus = true;            // focus the popup on open, restore the previous focus on close
  std::function<void()> onClosed;   // called once when the popup closes, for any reason
};

class OverlayLayer : public Widget, private TreeObserver {
public:
  explicit OverlayLayer(std::string name = "overlay");
  ~OverlayLayer() override;

  // Opens `popup` (size: popup->contentExtent(viewport), i.e. its Fixed lengths × scale unless it overrides
  // that, as Menu does). Returns the popup.
  Widget& open(std::unique_ptr<Widget> popup, OverlayOptions options);
  // Closes `popup` and every popup opened after it. No-op if it is not open.
  void close(Widget& popup);
  // Re-places an open popup (a movable panel dragged by its title, T3.x-fix-look). False when it is not open here.
  bool move(Widget& popup, const IRect& anchor, Placement placement);
  void closeAll();
  bool isOpen(const Widget& popup) const noexcept;
  size_t openCount() const noexcept { return entries_.size(); }
  Widget* top() const noexcept { return entries_.empty() ? nullptr : entries_.back().popup; }

  // The layer of `w`'s tree: the last child of the root, if it is an OverlayLayer.
  static OverlayLayer* of(const Widget& w) noexcept;
  // `base`, or `base-2`, `base-3`, … if a popup of that name is open (sibling names must be unique).
  std::string uniqueName(std::string_view base) const;

  // Tooltips: shown `delay` s after the pointer comes to rest on a widget with Widget::tooltip() text,
  // hidden on hover change or any press. Default 0.6 s.
  void setTooltipDelay(double seconds) noexcept { tooltipDelay_ = seconds; }
  bool tooltipVisible() const noexcept { return tooltip_ != nullptr; }
  // Shows a tooltip now (galleries / tests): text at device position p.
  void showTooltip(std::string_view text, Vec2 p);
  void hideTooltip();

  void arrange(LayoutPass& pass) override;
  bool onPointer(const PointerEvent& e) override;
  void onFrame(double now) override;
  void onTimer(uint32_t id) override;

private:
  void onHoverChanged(Widget* hovered) override;
  void onPointerDown(Vec2 pos) override;
  void updateHittable() noexcept;
  void observe();
  struct Entry {
    Widget* popup;
    OverlayOptions options;
    Widget* restoreFocus;
  };
  std::vector<Entry> entries_;
  std::vector<std::unique_ptr<Widget>> graveyard_;  // closed popups, destroyed at the next frame
  UiTree* observing_ = nullptr;
  Widget* tooltip_ = nullptr;      // the tooltip popup (a child, not an entry)
  Widget* tooltipFor_ = nullptr;   // widget whose tooltip is pending/shown
  uint32_t tooltipTimer_ = 0;
  double tooltipDelay_ = 0.6;
  Vec2 tooltipPos_{0, 0};
};

// --- menus ----------------------------------------------------------------------------------------------
struct MenuItem {
  std::string label;
  std::string shortcut;              // shown right-aligned (e.g. "Ctrl+D"); display only
  std::string action;                // action id run through the tree's ActionRegistry when chosen; a
                                     // disabled action greys the item
  std::function<void()> onChoose;    // called when chosen (after `action`)
  std::vector<MenuItem> submenu;     // non-empty: a submenu item (an arrow; choosing opens it)
  bool enabled = true;
  bool checked = false;
  bool separator = false;

  static MenuItem item(std::string label, std::function<void()> onChoose = {}, std::string shortcut = {});
  static MenuItem sub(std::string label, std::vector<MenuItem> items);
  static MenuItem sep();
};

// A menu popup. Paints its items itself (no widget per item). Size from the items' text measured with the
// tree's resources at open (UiTree::setResources), else an estimate.
class Menu : public Control {
public:
  explicit Menu(std::vector<MenuItem> items, std::string name = "menu");
  const std::vector<MenuItem>& items() const noexcept { return items_; }
  int32_t highlighted() const noexcept { return hi_; }
  void setHighlighted(int32_t i);
  // Opens the highlighted item's submenu (when it has one) and moves focus there. Returns it.
  Menu* openSubmenu();
  void closeSubmenu();
  Menu* submenu() const noexcept { return child_; }
  Menu* parentMenu() const noexcept { return parent_; }
  // Chooses item i (runs it and closes the whole menu chain); a submenu item opens instead.
  void choose(int32_t i);
  // Device-px rect of item i inside the menu (from the last layout).
  IRect itemRect(int32_t i) const noexcept;
  bool selectable(int32_t i) const noexcept;
  // At least this wide (device px), e.g. a dropdown's width.
  void setMinWidthPx(int32_t w);

  ISize contentExtent(ISize viewport) const override;
  void paint(PaintContext& ctx) override;
  bool onPointer(const PointerEvent& e) override;
  bool onKey(const KeyEvent& e) override;
  void onTimer(uint32_t id) override;

private:
  int32_t itemAt(Vec2 p) const noexcept;
  int32_t step(int32_t from, int32_t dir) const noexcept;
  Menu* root() noexcept;
  int32_t itemHeight(int32_t i) const noexcept;  // device px
  std::vector<MenuItem> items_;
  int32_t hi_ = -1;
  Menu* parent_ = nullptr;
  Menu* child_ = nullptr;
  uint32_t openTimer_ = 0;
  int32_t pendingOpen_ = -1;
  int32_t minWidth_ = 0;
};

// Opens a context menu at device position `at` in `from`'s overlay layer (nullptr when there is none).
Menu* openContextMenu(Widget& from, Vec2 at, std::vector<MenuItem> items);

// --- popover --------------------------------------------------------------------------------------------
// An anchored panel holding arbitrary child widgets (Column layout, padded). Give it a Fixed width and height;
// open it with OverlayLayer::open(..., {anchor, Placement::Below}). Esc or a press outside dismisses it.
class Popover : public Control {
public:
  explicit Popover(std::string name = "popover");
  void paint(PaintContext& ctx) override;
  bool onKey(const KeyEvent& e) override;
  bool onPointer(const PointerEvent& e) override;  // presses inside are consumed (never reach the layer)
};

}  // namespace pq::ui
