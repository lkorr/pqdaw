#pragma once

// Widget toolkit core (T3.1, SPEC §11): a retained widget tree with top-down integer-pixel layout, damage-
// driven repaint, SDL-independent input (pq/ui/input.h) with hit testing, hover, pointer capture and focus,
// and key routing into scoped keymaps (pq/ui/actions.h).
//
// Design:
//   - One Widget object per *interactive or structural* element (panels, headers, buttons, knobs). Dense
//     content (clips, notes, grid lines, automation points) is drawn by one virtualised widget that reports a
//     content extent and paints only the visible range (ScrollView + Widget::contentExtent); it never becomes
//     thousands of widgets.
//   - Layout is a single top-down pass: a parent assigns each child its rect from the child's Length specs;
//     there is no measure pass. Widgets with an intrinsic size (a label's text width) set a Fixed length when
//     their content changes. A child whose rect did not change and that has nothing dirty below it is not
//     visited, so relayout costs O(what changed).
//   - All rects are integer device pixels. Length values (sizes, min/max, padding, gaps, splitter positions)
//     are logical pixels, multiplied by UiTree::uiScale() (the window's display scale, e.g. 1.5 at 150 %;
//     pq/ui/ui_scale.h) and rounded when the layout runs. Text uses font roles resolved at the same scale
//     (round(role px × scale), hinted, never resampled), so scaled UIs are crisp.
//   - markDirty() reports the widget's rect (clipped by its clipping ancestors) to the tree's Damage; layout
//     reports old and new rects of every widget that moved. The frame loop draws only when something is
//     damaged or a widget asked for a frame (idle = no work at all).
//   - Widget ids are paths of names: "arrangement/track[3]/header/mute". Unnamed widgets (layout wrappers)
//     are transparent in paths. Sibling names must be unique (asserted in dev builds).
//   - Event dispatch (pointer, key, text) and hit testing do not allocate (tested with the allocation trap).
//
// UI thread only. Nothing here is RT-safe or meant for the audio thread.

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pq/ui/actions.h"
#include "pq/ui/damage.h"
#include "pq/ui/draw_list.h"
#include "pq/ui/input.h"
#include "pq/ui/ui_scale.h"

namespace pq::ui {

class TextSystem;
struct SceneFonts;
class UiTree;
class Widget;

// --- geometry -------------------------------------------------------------------------------------------
struct IRect {
  int32_t x = 0, y = 0, w = 0, h = 0;
  int32_t right() const noexcept { return x + w; }
  int32_t bottom() const noexcept { return y + h; }
  bool empty() const noexcept { return w <= 0 || h <= 0; }
  bool contains(Vec2 p) const noexcept {
    return p.x >= static_cast<float>(x) && p.y >= static_cast<float>(y) && p.x < static_cast<float>(x + w) &&
           p.y < static_cast<float>(y + h);
  }
  Rect toRect() const noexcept {
    return {static_cast<float>(x), static_cast<float>(y), static_cast<float>(x + w), static_cast<float>(y + h)};
  }
  bool operator==(const IRect&) const = default;
};
IRect intersect(const IRect& a, const IRect& b) noexcept;

struct ISize {
  int32_t w = 0, h = 0;
  bool operator==(const ISize&) const = default;
};

// --- layout specs ---------------------------------------------------------------------------------------
inline constexpr int32_t kUnbounded = INT32_MAX / 4;

enum class SizeKind : uint8_t {
  Fixed,  // `value` logical px
  Flex,   // main axis: share of the free space by weight `value`; cross axis / stack: fill the parent
};

// One axis of a widget's size request, clamped to [min, max] (logical px) in every case.
struct Length {
  SizeKind kind = SizeKind::Flex;
  float value = 1.0f;
  int32_t min = 0;
  int32_t max = kUnbounded;
  static constexpr Length fixed(float px) noexcept { return {SizeKind::Fixed, px, 0, kUnbounded}; }
  static constexpr Length flex(float weight = 1.0f, int32_t min = 0, int32_t max = kUnbounded) noexcept {
    return {SizeKind::Flex, weight, min, max};
  }
  bool operator==(const Length&) const = default;
};

enum class LayoutKind : uint8_t {
  Stack,   // every child fills the content rect (Flex) or sits in it at its fixed size (overlays), aligned
           // horizontally by the `main` and vertically by the `cross` alignment
  Row,     // children left to right: Fixed widths, Flex widths share the rest; cross axis = height
  Column,  // children top to bottom
  Custom,  // the widget's arrange() override places its children (Splitter, ScrollView, views)
};
enum class Align : uint8_t { Start, Center, End };

struct Insets {
  int16_t left = 0, top = 0, right = 0, bottom = 0;
  static constexpr Insets all(int16_t v) noexcept { return {v, v, v, v}; }
};

// --- painting -------------------------------------------------------------------------------------------
// What widgets draw with. T3.2 adds the theme here.
struct PaintResources {
  TextSystem* text = nullptr;         // null: no text (draw-list-only tests); widgets skip their glyphs
  const SceneFonts* fonts = nullptr;  // font roles resolved at the tree's UiScale (pq/ui/scenes.h setSceneScale)
};

struct PaintContext {
  DrawList& dl;
  const PaintResources& res;
  double time;    // seconds (the clock passed to UiTree::update)
  UiScale scale;  // logical → device px: the tree's scale (res.fonts are resolved at it)
  IRect clip;    // the scissor in effect: the visible part of the target. Virtualised widgets draw only
                 // what intersects it (see ScrollView).
};

// Places children inside a Custom layout (Widget::arrange). place() assigns a rect and lays the child out.
class LayoutPass {
public:
  float scale() const noexcept { return scale_; }
  int32_t px(float logical) const noexcept;
  void place(Widget& child, const IRect& r);

private:
  friend class UiTree;
  explicit LayoutPass(UiTree& tree) noexcept : tree_(tree) {}
  UiTree& tree_;
  float scale_ = 1.0f;
};

// --- widget ---------------------------------------------------------------------------------------------
class Widget {
public:
  explicit Widget(std::string name = {});
  virtual ~Widget();
  Widget(const Widget&) = delete;
  Widget& operator=(const Widget&) = delete;

  // --- tree ---
  Widget& addChild(std::unique_ptr<Widget> child);
  template <class W, class... Args>
  W& add(Args&&... args) {
    return static_cast<W&>(addChild(std::make_unique<W>(std::forward<Args>(args)...)));
  }
  // Detaches `child` (clearing hover/capture/focus/timers that pointed into it) and returns it.
  std::unique_ptr<Widget> removeChild(Widget& child);
  void clearChildren();
  Widget* parent() const noexcept { return parent_; }
  size_t childCount() const noexcept { return children_.size(); }
  Widget& child(size_t i) const noexcept { return *children_[i]; }
  uint32_t indexInParent() const noexcept { return indexInParent_; }
  UiTree* tree() const noexcept { return tree_; }

  const std::string& name() const noexcept { return name_; }
  // Path of named ancestors below the root, e.g. "arrangement/track[3]/header/mute".
  std::string path() const;
  // Finds a descendant by relative path (unnamed widgets are transparent). nullptr when absent.
  Widget* find(std::string_view relativePath) const noexcept;
  bool isAncestorOf(const Widget& w) const noexcept;

  // --- layout spec (logical px) ---
  Widget& setWidth(Length l);
  Widget& setHeight(Length l);
  Widget& setSize(Length w, Length h);
  Widget& setLayout(LayoutKind k);
  Widget& setPadding(Insets p);
  Widget& setGap(int16_t g);
  // Main axis: where Fixed-only children sit when they don't fill the row/column. Cross axis: where a Fixed
  // cross size sits (Flex stretches, up to its max). Stack: `main` = horizontal, `cross` = vertical.
  Widget& setAlign(Align main, Align cross);
  const Length& width() const noexcept { return width_; }
  const Length& height() const noexcept { return height_; }
  LayoutKind layout() const noexcept { return layout_; }

  // Device-pixel rect from the last layout.
  const IRect& rect() const noexcept { return rect_; }

  // The parent must re-arrange (this widget's spec or visibility changed).
  void invalidateLayout() noexcept;
  // This widget must re-arrange its children (its own rect is unaffected), e.g. a scroll offset changed.
  void invalidateArrange() noexcept;

  // --- state ---
  bool visible() const noexcept { return has(kVisible); }
  void setVisible(bool v);
  bool enabled() const noexcept { return has(kEnabled); }
  void setEnabled(bool e);
  bool focusable() const noexcept { return has(kFocusable); }
  void setFocusable(bool f) noexcept { set(kFocusable, f); }
  // Hittable widgets can be the target of pointer events; non-hittable ones let the pointer through to
  // whatever is below them (their children are still hit-tested). Default: true.
  bool hittable() const noexcept { return has(kHittable); }
  void setHittable(bool h) noexcept { set(kHittable, h); }
  bool clipsChildren() const noexcept { return has(kClipChildren); }
  void setClipChildren(bool c) noexcept { set(kClipChildren, c); }
  // A text-entry widget: while it has focus it receives TextEvents and swallows printable keys (they never
  // reach ancestors or keymaps).
  bool acceptsTextInput() const noexcept { return has(kTextInput); }
  void setAcceptsTextInput(bool t) noexcept { set(kTextInput, t); }

  bool hovered() const noexcept { return has(kHovered); }
  bool focused() const noexcept { return has(kFocused); }
  bool captured() const noexcept { return has(kCaptured); }

  // Keymap scope that is active while focus is inside this widget's subtree (kNoScope = none).
  void setKeyScope(ScopeId s) noexcept { keyScope_ = s; }
  ScopeId keyScope() const noexcept { return keyScope_; }

  // --- redraw / time ---
  // Damages this widget's rect (clipped by clipping ancestors). Call when its appearance changed.
  void markDirty() noexcept;
  // onFrame(now) is called once at the next frame. Re-request from onFrame to animate; stop requesting and
  // the UI is idle again.
  void requestFrame();
  // onTimer(id) is called once at or after now + delaySeconds (the frame loop sleeps until then).
  uint32_t requestTimer(double delaySeconds);
  void cancelTimer(uint32_t id) noexcept;

  // --- overridables ---
  virtual void paint(PaintContext&) {}
  virtual void paintOver(PaintContext&) {}  // after the children (focus rings, overlays)
  // Events arrive in device px. Return true when handled: pointer events stop bubbling to ancestors, and a
  // handled Down captures the pointer to this widget until every button is released.
  virtual bool onPointer(const PointerEvent&) { return false; }
  virtual bool onKey(const KeyEvent&) { return false; }
  virtual bool onText(const TextEvent&) { return false; }
  virtual void onHoverChanged(bool /*hovered*/) {}
  virtual void onFocusChanged(bool /*focused*/) {}
  virtual void onCaptureLost() {}  // capture ended without a button release (widget removed, window lost focus)
  virtual void onFrame(double /*now*/) {}
  virtual void onTimer(uint32_t /*id*/) {}
  // Virtualisation hook: the size of this widget's content when it is the content of a ScrollView with the
  // given viewport. Default: Fixed lengths, else the viewport. Views with huge content return their extent
  // and paint only PaintContext::clip.
  virtual ISize contentExtent(ISize viewport) const;
  // LayoutKind::Custom: place every visible child with pass.place().
  virtual void arrange(LayoutPass& /*pass*/) {}

protected:
  enum Flag : uint32_t {
    kVisible = 1u << 0,
    kEnabled = 1u << 1,
    kFocusable = 1u << 2,
    kHittable = 1u << 3,
    kClipChildren = 1u << 4,
    kTextInput = 1u << 5,
    kHovered = 1u << 6,
    kFocused = 1u << 7,
    kCaptured = 1u << 8,
    kArrangeDirty = 1u << 9,     // my children must be re-arranged
    kDescendantDirty = 1u << 10, // some descendant has kArrangeDirty
    kFrameRequested = 1u << 11,
    kFrozen = 1u << 12,          // layout scratch (flex clamping)
  };
  bool has(uint32_t f) const noexcept { return (flags_ & f) != 0; }
  void set(uint32_t f, bool on) noexcept { flags_ = on ? (flags_ | f) : (flags_ & ~f); }

private:
  friend class UiTree;
  friend class LayoutPass;
  void attach(UiTree* tree) noexcept;
  Widget* findSegment(std::string_view segment) const noexcept;

  UiTree* tree_ = nullptr;
  Widget* parent_ = nullptr;
  std::vector<std::unique_ptr<Widget>> children_;
  std::string name_;
  IRect rect_{};
  Length width_{};
  Length height_{};
  Insets padding_{};
  int16_t gap_ = 0;
  LayoutKind layout_ = LayoutKind::Stack;
  Align mainAlign_ = Align::Start;
  Align crossAlign_ = Align::Start;
  ScopeId keyScope_ = kNoScope;
  uint32_t indexInParent_ = 0;
  uint32_t flags_ = kVisible | kEnabled | kHittable | kArrangeDirty;
  int32_t mainSize_ = 0;  // layout scratch
};

// --- splitter -------------------------------------------------------------------------------------------
// Two panes separated by a draggable handle. `position` is the size of the anchored pane in logical px,
// clamped to [minAnchored, maxAnchored]; at layout the anchored pane also yields so the other pane keeps
// minOther (unless that would push the anchored pane below its own minimum). Anchor::Second keeps e.g. a
// bottom detail view at its height while the window resizes.
class Splitter : public Widget {
public:
  enum class Axis : uint8_t { Horizontal, Vertical };  // Horizontal: panes side by side
  enum class Anchor : uint8_t { First, Second };

  Splitter(std::string name, Axis axis, float position, Anchor anchor = Anchor::First);
  void setLimits(int32_t minAnchored, int32_t maxAnchored, int32_t minOther);
  void setHandleThickness(int32_t logicalPx);
  void setHandleColor(Rgba c) noexcept { handleColor_ = c; }
  float position() const noexcept { return position_; }
  void setPosition(float logicalPx);
  IRect handleRect() const noexcept { return handle_; }
  bool dragging() const noexcept { return dragging_; }

  void arrange(LayoutPass& pass) override;
  bool onPointer(const PointerEvent& e) override;
  void onCaptureLost() override;
  void paint(PaintContext& ctx) override;

private:
  Axis axis_;
  Anchor anchor_;
  float position_;
  int32_t minAnchored_ = 0, maxAnchored_ = kUnbounded, minOther_ = 0;
  int32_t thickness_ = 4;
  Rgba handleColor_ = 0;
  IRect handle_{};
  bool dragging_ = false;
  float dragStart_ = 0;      // pointer main coordinate at Down
  int32_t dragStartPx_ = 0;  // anchored pane size at Down
  int32_t anchoredPx_ = 0;   // from the last layout
};

// --- scroll view ----------------------------------------------------------------------------------------
// Viewport onto its first child (the content). The content's size comes from content->contentExtent(); its
// rect is offset by the scroll position and painted with the viewport as clip, so a virtualised content
// widget draws only PaintContext::clip. Wheel scrolls vertically (Shift+wheel horizontally).
class ScrollView : public Widget {
public:
  ScrollView(std::string name, bool horizontal = true, bool vertical = true);
  Widget* content() const noexcept { return childCount() ? &child(0) : nullptr; }
  int32_t scrollX() const noexcept { return scrollX_; }
  int32_t scrollY() const noexcept { return scrollY_; }
  // Device px; clamped to the content at the next layout.
  void scrollTo(int32_t x, int32_t y);
  ISize contentSize() const noexcept { return content_; }
  void setWheelStep(float logicalPx) noexcept { wheelStep_ = logicalPx; }

  void arrange(LayoutPass& pass) override;
  bool onPointer(const PointerEvent& e) override;

private:
  bool horizontal_, vertical_;
  int32_t scrollX_ = 0, scrollY_ = 0;
  ISize content_{};
  float wheelStep_ = 48.0f;
};

// --- tree -----------------------------------------------------------------------------------------------
enum class KeyResult : uint8_t {
  Unhandled,
  Widget,         // the focused widget or an ancestor handled it
  TextSwallowed,  // a focused text-entry widget claimed a printable key
  Action,         // a keymap binding ran an action
  ScopeSwallowed, // a scope with swallowUnbound stopped an unbound key
  Focus,          // Tab / Shift+Tab moved focus
};

class UiTree {
public:
  UiTree();
  ~UiTree();
  UiTree(const UiTree&) = delete;
  UiTree& operator=(const UiTree&) = delete;

  Widget& setRoot(std::unique_ptr<Widget> root);
  template <class W, class... Args>
  W& emplaceRoot(Args&&... args) {
    return static_cast<W&>(setRoot(std::make_unique<W>(std::forward<Args>(args)...)));
  }
  Widget* root() const noexcept { return root_.get(); }
  // Path from the root ("" = root). nullptr when absent.
  Widget* find(std::string_view path) const noexcept;

  // Target size in device px and the logical → device scale. Damages everything; a scale change also
  // invalidates every widget's layout (every logical length maps to a new device size).
  void setViewport(uint32_t width, uint32_t height, UiScale scale = {});
  uint32_t width() const noexcept { return width_; }
  uint32_t height() const noexcept { return height_; }
  const UiScale& uiScale() const noexcept { return scale_; }
  float scale() const noexcept { return scale_.factor; }

  // --- frame ---
  // Runs due timers, frame requests and layout. Call once per loop iteration with the current time (s).
  void update(double now);
  // True when update() left something to draw (damage). Paint then.
  bool needsPaint() const noexcept { return damage_.needsFrame(); }
  // True when the loop must not block: something to draw, a pending frame request or pending layout.
  bool needsFrame() const noexcept;
  // Earliest pending timer (s), or +infinity. The loop may sleep until then when !needsFrame().
  double nextWakeTime() const noexcept;
  // Draws the whole tree into `dl` (already reset to width()×height()) and clears the damage.
  void paint(DrawList& dl, const PaintResources& res, double now);
  Damage& damage() noexcept { return damage_; }
  // Forces a full relayout at the next update() (benchmarks, theme changes).
  void invalidateAll() noexcept;
  void layoutNow();  // runs pending layout immediately (update() does this too)

  // --- input ---
  bool dispatch(const InputEvent& e);
  bool dispatchPointer(const PointerEvent& e);
  KeyResult dispatchKey(const KeyEvent& e);
  bool dispatchText(const TextEvent& e);
  // Deepest visible, hittable widget under p (children outside their parent's rect are not hit).
  Widget* hitTest(Vec2 p) const noexcept;

  Widget* hovered() const noexcept { return hovered_; }
  Widget* captured() const noexcept { return captured_; }
  Widget* focus() const noexcept { return focus_; }
  void setFocus(Widget* w);
  // Moves focus to the next (or previous) visible, enabled, focusable widget in tree order, wrapping.
  bool focusNext(bool backwards = false);
  // Ends a capture without a button release (calls onCaptureLost).
  void releaseCapture();

  // --- actions and keymaps ---
  // Uses `actions` and `keymap` for key routing (both must outlive the tree or be replaced). Resolves every
  // binding's action id now; ids with no registered action are kept unresolved and do nothing.
  void setKeymap(const ActionRegistry* actions, const Keymap* keymap);
  // Modal scopes (macro mode, a modal dialog) are consulted before the focus chain, newest first.
  void pushModalScope(ScopeId s) noexcept;
  void removeModalScope(ScopeId s) noexcept;

  struct Stats {
    uint64_t framesPainted = 0;
    uint64_t layoutPasses = 0;
    uint64_t widgetsArranged = 0;  // widgets whose children were (re)placed
    uint64_t widgetsPainted = 0;   // last paint
  };
  const Stats& stats() const noexcept { return stats_; }

private:
  friend class Widget;
  friend class LayoutPass;
  void place(Widget& w, const IRect& r);
  void arrangeChildren(Widget& w);
  void arrangeLinear(Widget& w, bool row);
  void arrangeStack(Widget& w);
  void layoutDirty(Widget& w);
  void detachSubtree(Widget& w) noexcept;
  void clearInteraction(Widget& w) noexcept;  // hover / capture / focus inside w end (hidden, disabled, removed)
  void damageWidget(const Widget& w) noexcept;
  void paintWidget(Widget& w, PaintContext& ctx);
  Widget* hitRec(Widget& w, Vec2 p) const noexcept;
  void updateHover(Vec2 p);
  bool bubblePointer(Widget* from, const PointerEvent& e);
  bool runBinding(ScopeId scope, const KeyEvent& e, KeyResult& result) const;
  bool canFocus(const Widget& w) const noexcept;
  void requestFrame(Widget& w);

  std::unique_ptr<Widget> root_;
  Damage damage_;
  uint32_t width_ = 0, height_ = 0;
  UiScale scale_{};
  bool layoutPending_ = true;
  IRect clip_{};  // during layout: the visible area through the clipping ancestors of the widget being placed

  Widget* hovered_ = nullptr;
  Widget* captured_ = nullptr;
  Widget* focus_ = nullptr;
  uint8_t buttonsHeld_ = 0;  // bit (button - 1)
  Vec2 pointer_{-1e9f, -1e9f};
  bool pointerInside_ = false;

  std::vector<Widget*> frameRequests_;
  std::vector<Widget*> frameWork_;
  struct Timer {
    double due;
    Widget* widget;
    uint32_t id;
  };
  std::vector<Timer> timers_;
  std::vector<Timer> timerWork_;
  uint32_t nextTimerId_ = 1;
  double now_ = 0;

  struct Bound {
    uint64_t key;  // scope << 48 | mods << 32 | key
    ActionIndex action;
  };
  const ActionRegistry* actions_ = nullptr;
  std::vector<Bound> bindings_;
  std::vector<ScopeRules> scopeRules_;
  static constexpr uint32_t kMaxModalScopes = 8;
  ScopeId modal_[kMaxModalScopes] = {};
  uint32_t modalCount_ = 0;

  Stats stats_;
};

}  // namespace pq::ui
