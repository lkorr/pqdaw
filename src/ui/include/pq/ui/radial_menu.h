#pragma once

// MX.1: a nested radial ("bubble") menu. Any widget opens it with a tree of items centred on a point (the symbol that
// was clicked); it lives in the tree's OverlayLayer like the other popups (pq/ui/overlay.h).
//
//   ring 0      the top-level items (categories) in a full ring around the anchor; a disc at the centre shows the
//               current item's glyph, the opener's centre text, or the text typed so far
//   ring k > 0  the children of the item open on ring k − 1, as an arc outward from it (rotated to stay on screen;
//               bubbles that still do not fit are clamped inside the window)
//
// Pointer: hovering (or clicking) an item with children opens it (deeper rings close); the open items stay
// highlighted. Releasing the button over the leaf it was pressed on picks it: the menu closes, then onPick(key)
// runs (it may rebuild the opener, open another menu, …: nothing of the menu is touched after it). A press that
// misses every bubble, or any non-left press, closes the menu (and is consumed).
// Keys: Up / Down move around the focused ring (Down = clockwise), Right / Enter go in (open an item, focus its first
// child or the current item), Enter on a leaf picks it, Left / Esc go out one level (Esc on ring 0 closes; Left does
// nothing there). Typing filters: the typed text is matched against every leaf's glyph, label, key and keywords
// (ASCII case-insensitive; exact < prefix < word prefix < substring, then tree order) and the matches replace the
// sub-rings with one ring of results, best first and highlighted; the categories holding a match are lit, the rest
// dimmed. Up / Down move through the results, Enter picks, Backspace edits, Esc / Left clear the text.
// The current item (RadialMenuOptions::current) is opened to and marked when the menu opens.
//
// SX.1 controls: an item may be a CONTROL instead of a pick leaf. It edits a value through its RadialBinding and the
// menu stays open:
//   knob    a rotary disc (2× a bubble): label, value arc, value text. Vertical drag sweeps spec.dragPixels (Shift:
//           fine); a double-click (or Enter while highlighted) types a value: Enter commits, Esc cancels, a press
//           elsewhere commits, an unparsable entry reverts
//   slider  a capsule with a short bar: a press jumps to the pointer and drags along the bar (Shift: fine, relative);
//           a double-click resets; Enter types a value
//   chips   2–7 exclusive choices laid out together, the chosen one lit; a click chooses. A chip may have children: it
//           then opens a ring like a category (hover / click), so a mode's parameters sit on its sub-ring
//   toggle  a bubble lit while on; a click flips it
// Knob / slider: the wheel nudges by spec.keyStep (Shift: fine; a run of notches is one gesture that ends 0.4 s after
// the last notch, when the pointer leaves the control, on a press or on close); Alt-click resets to spec.def unless
// the item is not `resettable` — or, when the binding has an `alt` value (SX.2), an Alt-drag edits that. Every edit runs begin() before its first set() and end() once after its last: one
// gesture = one undo step for the opener (a click that changes nothing runs neither). While a knob / slider is dragged
// nothing else reacts to the pointer (its ring stays open even when the pointer leaves it). Closing the menu (Esc, a
// press outside, a pick, the layer closing it) ends a running gesture first; an open typed entry commits.
// Values are read with get() at open, after each of the menu's own set()s and on refresh(), never per frame; the
// opener calls refresh() when its document changes underneath (it learns that the menu is gone from onClosed).
// A control whose binding's enabled() is false is drawn dimmed and ignores input.
// Placement: a ring-0 item may be pinned to a side or an angle (RadialPlace); the unpinned ones fill the largest gaps
// left, in order (with no pins: spread from the top, clockwise, as before). Sub-rings are arcs outward from their
// parent and ignore pins. Typing filters pick leaves only.
//
// SX.5 bands (RadialMenuOptions::layout = Bands): the same items, controls, bindings, gestures and painting, laid out as
// compact rows hugging a rectangle (`around`: what the menu must not cover, e.g. a clip) instead of rings: the items
// placed RadialPlace::above(row) form rows above it (row 0 nearest), every other item rows below it (below(row); Auto /
// Left / Right: below, row 0). Each row is centred on options.bandX, its items side by side in order (a chip group's
// chips close together, a wider gap between slots); a band (all rows of one side) sits on a panel. A chip that is lit and
// has children shows them as a row directly outward from its row (the chosen mode's settings beside the modes: no hover
// sub-rings); a category's children show the same way while it is open (hover / click). A band that does not fit its
// side goes to the other side (outward beyond that side's band) when both fit there; otherwise it is clamped inside the
// window. Chips, toggles and leaves are pills sized to their label, knobs a smaller disc. Keys: arrows move through
// the shown items in layout order, Enter acts (types a knob / slider value, chooses a chip, flips a toggle, picks a
// leaf, opens a category), Esc closes; typing does not filter. A press on a band's panel between items is consumed.
//
// Look: theme tokens only (menu, surface, selection, accent, control, text); glyphs in SceneFonts::symbol (18 SemiBold)
// when they fit the bubble, else the theme's title font; a leaf's name shows in a tag outside its bubble while it is
// highlighted. A 120 ms open animation (rings grow from 70 %, fade in); there is no reduced-motion setting yet
// (RadialMenuOptions::animate = false skips it).
// CPU: items are flattened and measured once per open (and per UI-scale change); the bubble layout is rebuilt only
// when a ring opens / closes or the filter changes, into storage reserved at open. Paint allocates nothing, damages
// only the menu's bounding box and does nothing for damage elsewhere. Nothing runs while it is idle; closed, it is gone.
//
// UI thread only.

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pq/ui/controls.h"
#include "pq/ui/text.h"
#include "pq/ui/widget.h"

namespace pq::ui {

// What an item is (SX.1). Item: a pick leaf, or a category when it has children.
enum class RadialKind : uint8_t { Item, Knob, Slider, Chip, Toggle };

// Where a ring-0 item sits (sub-rings ignore it).
struct RadialPlace {
  enum class Side : uint8_t { Auto, Top, Right, Bottom, Left, Angle };
  Side side = Side::Auto;
  float degrees = 0.0f;  // Side::Angle: clockwise from the top
  int8_t row = 0;        // SX.5 bands: the row within its band (0 nearest the `around` rect); rings ignore it
  static RadialPlace top() noexcept { return {Side::Top, 0.0f}; }
  static RadialPlace right() noexcept { return {Side::Right, 0.0f}; }
  static RadialPlace bottom() noexcept { return {Side::Bottom, 0.0f}; }
  static RadialPlace left() noexcept { return {Side::Left, 0.0f}; }
  static RadialPlace angle(float degreesClockwiseFromTop) noexcept { return {Side::Angle, degreesClockwiseFromTop}; }
  // SX.5: top / bottom with a band row (rings: pinned top / bottom).
  static RadialPlace above(int8_t row = 0) noexcept { return {Side::Top, 0.0f, row}; }
  static RadialPlace below(int8_t row = 0) noexcept { return {Side::Bottom, 0.0f, row}; }
  bool pinned() const noexcept { return side != Side::Auto; }
  float degreesFromTop() const noexcept;  // Top 0, Right 90, Bottom 180, Left 270, Angle: degrees
};

// A control's value, owned by the opener: a knob / slider its value, chips the chosen RadialChoice::value, a toggle
// 0 / 1. The callbacks must not hold pointers that can dangle while the menu is open (look the document up).
struct RadialBinding {
  std::function<double()> get;
  std::function<void(double)> set;   // always between begin() and end()
  std::function<void()> begin, end;  // one gesture: one undo step
  std::function<std::string(double)> format;                      // knob / slider text (default: ui::formatValue)
  std::function<std::optional<double>(std::string_view)> parse;  // typed entry (default: ui::parseValue)
  std::function<bool()> enabled;                                  // unset: always enabled
  // SX.2: a knob / slider's second value, edited by an Alt-drag (vertical, altSpec.dragPixels sweep its range; Shift:
  // fine) instead of the Alt-click reset — a fade's curve beside its length. Its own begin / end: one gesture.
  std::shared_ptr<const RadialBinding> alt;
  ValueSpec altSpec;
};

struct RadialItem;
struct RadialChoice {  // one chip of a chip group
  std::string label;
  int32_t value = 0;
  std::vector<RadialItem> children;  // non-empty: the chip opens a ring (its mode's parameters)
};

struct RadialItem {
  std::string key;       // a leaf's identity: what onPick reports (unique among the leaves); controls: for find()
  std::string label;     // its name ("greater than", "Compare")
  std::string glyph;     // optional symbol drawn in the bubble (">"); empty: the label is drawn
  std::string keywords;  // optional extra search terms, space separated (">=", "arctangent")
  std::vector<RadialItem> children;  // non-empty: an item that opens a ring (a category; a chip that opens one)

  // SX.1 controls (set by the factories below).
  RadialKind kind = RadialKind::Item;
  RadialPlace place;       // ring 0 only
  ValueSpec spec;          // knob / slider: range, default, step, drag / wheel sensitivity, default text
  bool resettable = true;  // knob / slider: Alt-click (a slider also by double-click) returns to spec.def
  std::shared_ptr<const RadialBinding> binding;
  int32_t chipValue = 0;           // a chip
  std::vector<RadialItem> chips_;  // a chip group (chips()): its chips, laid out together where the group sits
  int32_t groupId_ = -1;           // internal: the group a chip came from

  static RadialItem leaf(std::string key, std::string label, std::string glyph = {}, std::string keywords = {});
  static RadialItem category(std::string label, std::vector<RadialItem> children);
  static RadialItem knob(std::string key, std::string label, ValueSpec spec, RadialBinding binding, RadialPlace place = {});
  static RadialItem slider(std::string key, std::string label, ValueSpec spec, RadialBinding binding, RadialPlace place = {});
  static RadialItem toggle(std::string key, std::string label, RadialBinding binding, RadialPlace place = {});
  // A chip group (2–7 choices); each chip's key is "<key>/<label>". Choices split over two sides are two groups
  // sharing one binding (pass the same shared_ptr).
  static RadialItem chips(std::string key, std::vector<RadialChoice> choices, RadialBinding binding, RadialPlace place = {});
  static RadialItem chips(std::string key, std::vector<RadialChoice> choices, std::shared_ptr<const RadialBinding> binding,
                          RadialPlace place = {});
};

// How the items are laid out (SX.5): rings around the anchor, or bands hugging a rectangle.
enum class RadialLayout : uint8_t { Rings, Bands };

struct RadialMenuOptions {
  std::string current;                          // key of the current leaf: opened to and marked ("" none)
  std::function<void(std::string_view key)> onPick;  // after the menu closed
  std::function<void()> onClosed;               // once, whenever it closes (before onPick on a pick)
  bool animate = true;                          // the open animation
  // SX.1: the centre disc's text (one or two lines, '\n' between; the disc grows to fit), read with the values.
  // Unset: the current item's glyph.
  std::function<std::string()> centreText;
  // SX.1: read with the values (refresh()); false closes the menu (what it edits is gone).
  std::function<bool()> valid;
  // SX.5: Bands: `around` is the rectangle the bands hug and never cover (while the window allows), `bandX` the x they
  // are centred on (NaN: the rectangle's centre). Device px in the overlay layer's tree; openRadialMenu sets `around`
  // to its anchor and maps `bandX` from the opener's tree.
  RadialLayout layout = RadialLayout::Rings;
  IRect around{};
  float bandX = std::numeric_limits<float>::quiet_NaN();
};

class RadialMenu : public Widget {
public:
  static constexpr int32_t kMaxDepth = 6;  // rings (deeper items are not shown)

  // `anchor`: device px in the overlay layer's tree.
  RadialMenu(std::vector<RadialItem> items, Vec2 anchor, RadialMenuOptions options, std::string name = "radial-menu");

  // --- queries (tests, the opener) ---
  // The item with this leaf / control key, else the first with this label; -1 if none. Items are numbered breadth
  // first (a chip group's chips take its place among its siblings).
  int32_t find(std::string_view keyOrLabel) const noexcept;
  int32_t itemCount() const noexcept { return static_cast<int32_t>(nodes_.size()); }
  const std::string& keyOf(int32_t item) const noexcept { return nodes_[static_cast<size_t>(item)].key; }
  const std::string& labelOf(int32_t item) const noexcept { return nodes_[static_cast<size_t>(item)].label; }
  // Whether the item's bubble is shown, and its centre / radius (device px, from the last layout; a slider's radius
  // is its half height, halfWidthOf its half width).
  bool shown(int32_t item) const noexcept;
  Vec2 centreOf(int32_t item) const noexcept;
  float radiusOf(int32_t item) const noexcept;
  float halfWidthOf(int32_t item) const noexcept;
  Vec2 centre() const noexcept { return centre_; }  // the rings' centre (the anchor, moved to keep ring 0 on screen)
  // The items opened, ring 0 first (openItem(k) is the item whose children form ring k + 1).
  int32_t openDepth() const noexcept { return openDepth_; }
  int32_t openItem(int32_t ring) const noexcept { return path_[ring]; }
  int32_t highlighted() const noexcept;  // the keyboard / hover item (-1 none)
  int32_t focusRing() const noexcept { return focusRing_; }
  const std::string& filter() const noexcept { return filter_; }
  // Filter results shown (best first).
  int32_t resultCount() const noexcept { return shownResults_; }
  int32_t result(int32_t i) const noexcept { return results_[static_cast<size_t>(i)].node; }
  IRect bounds() const noexcept { return bbox_; }  // what the menu paints (device px)
  // SX.5 bands: whether the menu is laid out as bands, and a band's panel (0 the items placed above, 1 the others;
  // device px, empty when it has no rows) and whether it was moved to the other side.
  bool bands() const noexcept { return bands_; }
  IRect bandRect(int32_t band) const noexcept { return bandRect_[band]; }
  bool bandFlipped(int32_t band) const noexcept { return bandFlipped_[band]; }
  // Opening animation progress 0..1.
  float openProgress() const noexcept { return progress_; }

  // SX.1 controls.
  RadialKind kindOf(int32_t item) const noexcept { return nodes_[static_cast<size_t>(item)].kind; }
  double valueOf(int32_t item) const noexcept { return nodes_[static_cast<size_t>(item)].value; }  // as last read
  const std::string& valueTextOf(int32_t item) const noexcept { return nodes_[static_cast<size_t>(item)].valueText; }
  bool enabledOf(int32_t item) const noexcept { return nodes_[static_cast<size_t>(item)].enabled; }
  bool litOf(int32_t item) const noexcept;               // a chip that is chosen, a toggle that is on
  int32_t dragging() const noexcept { return drag_; }    // the knob / slider being dragged (-1 none)
  int32_t inGesture() const noexcept { return gesture_; }  // the control whose gesture runs (-1 none)
  int32_t typingIn() const noexcept { return typing_; }  // the control with a typed entry open (-1 none)
  const TextBuffer& typeInBuffer() const noexcept { return typeIn_; }
  const std::string& centreText() const noexcept { return centreText_; }
  // A slider's bar ends (device px, from the last layout): a press there sets min / max.
  Vec2 sliderStart(int32_t item) const noexcept;
  Vec2 sliderEnd(int32_t item) const noexcept;
  // Re-reads every value, enabled state and the centre text; closes the menu when options.valid() is false.
  void refresh();

  // Picks a leaf: closes the menu, then onPick. Ignored for items with children and for controls.
  void pick(int32_t item);
  void close();

  void arrange(LayoutPass& pass) override;
  void paint(PaintContext& ctx) override;
  bool onPointer(const PointerEvent& e) override;
  bool onKey(const KeyEvent& e) override;
  bool onText(const TextEvent& e) override;
  void onFrame(double now) override;
  void onTimer(uint32_t id) override;
  void onFocusChanged(bool focused) override;
  void onCaptureLost() override;
  ISize contentExtent(ISize viewport) const override { return viewport; }

private:
  struct Node {
    std::string key, label, glyph;
    std::string keyL, labelL, glyphL, keywordsL;  // lower-cased (ASCII) for matching
    int32_t parent = -1, first = -1, count = 0;
    int32_t depth = 0;
    FontId glyphFont = kNoFont;  // set by measure()
    FontId labelFont = kNoFont;
    int32_t labelW = 0;  // in labelFont, px
    // SX.1
    RadialKind kind = RadialKind::Item;
    RadialPlace place;
    ValueSpec spec;
    bool resettable = true;
    int32_t binding = -1;  // index into bindings_
    int32_t group = -1;    // the chips of one group share it (one layout slot)
    int32_t chipValue = 0;
    double value = 0.0;
    bool enabled = true;
    std::string valueText;
    float hw = 0.0f, hh = 0.0f;  // half extents (device px); a circle unless capsule (then hw == hh == radius)
    bool capsule = false;
  };
  struct Bubble {
    Vec2 c;
    float r;  // a circle's radius; a capsule's half height
    int32_t node;
    int32_t ring;  // -1: a filter result
    float hw;      // half width (== r for a circle)
  };
  struct Result {
    int32_t score;
    int32_t node;
  };
  struct Slot {  // ring-0 layout unit: one item, or a run of chips of one group
    int32_t first, count;
    float angle, tw, rx;  // centre angle; tangential width (px, with the gaps); radial half extent
    bool pinned;
  };
  void flatten(std::vector<RadialItem>& items);
  void measure();          // fonts and bubble radii, once per open / scale change
  void relayout();         // bubble positions from the state, damages old ∪ new bounds
  void ensureLayout();
  int32_t ringFirst(int32_t ring) const noexcept;  // first node of ring k (its parent's children)
  int32_t ringCount(int32_t ring) const noexcept;
  void openTo(int32_t ring, int32_t node);  // node (on ring `ring`) becomes the open item there
  void setHighlight(int32_t ring, int32_t node);
  void updateFilter();
  int32_t bubbleAt(Vec2 p) const noexcept;  // index into bubbles_, -1 none
  int32_t matchScore(const Node& n) const noexcept;
  bool hasMatch(int32_t node) const noexcept;
  void damage(const IRect& r) noexcept;
  // An arc of n bubbles centred on baseAngle at distance R, rotated to fit the window. ring < 0: the filter results
  // (radius r); else the nodes node0 .. node0 + n − 1 with their own sizes.
  void placeArc(int32_t ring, int32_t node0, int32_t n, float baseAngle, float R, float r);
  void layoutRing0();
  void layoutBands();                                  // SX.5
  bool expanded(int32_t node) const noexcept;          // bands: its children show as a row
  bool activate(int32_t node);                         // Enter on a control / chip / toggle (false: not a control)
  void bandStep(int32_t dir);                          // bands: the arrows
  float tangentExt(int32_t node, float a) const noexcept;
  float radialExt(int32_t node, float a) const noexcept;
  bool isControl(int32_t node) const noexcept { return nodes_[static_cast<size_t>(node)].kind != RadialKind::Item; }
  // controls
  const RadialBinding* bindingOf(int32_t node) const noexcept;
  void readValues();                        // get() / enabled() / format of every control, the centre text
  void setControl(int32_t node, double v);  // clamps, starts the gesture, set(), re-reads
  void setAlt(int32_t node, double v);      // the same for the binding's alt value (SX.2)
  void endGesture();
  void finishWheel();
  void finishAll(bool commitTyping);  // ends the drag, the wheel run and the gesture; commits / cancels a typed entry
  void beginTypeIn(int32_t node);
  void endTypeIn(bool commit);
  void resetControl(int32_t node);
  double sliderValueAt(int32_t node, float x) const noexcept;
  void sliderBar(int32_t node, float& x0, float& x1, float& y) const noexcept;
  void measureCentre();

  std::vector<Node> nodes_;
  std::vector<std::shared_ptr<const RadialBinding>> bindings_;
  int32_t rootCount_ = 0;
  Vec2 anchor_;
  RadialMenuOptions options_;
  int32_t current_ = -1;

  // state
  int32_t path_[kMaxDepth]{};
  int32_t openDepth_ = 0;  // rings 0..openDepth_ are shown
  int32_t focusRing_ = 0;
  int32_t hi_ = -1;        // highlighted node (normal mode)
  int32_t resHi_ = 0;      // highlighted result (filter mode)
  int32_t pressed_ = -1;   // node pressed with the left button
  std::string filter_, filterL_;
  std::vector<Result> results_;
  std::vector<uint8_t> lit_;  // node holds a match (filter mode)
  int32_t shownResults_ = 0;
  // controls
  int32_t drag_ = -1, gesture_ = -1, typing_ = -1, wheel_ = -1;
  bool dragAlt_ = false, gestureAlt_ = false;  // SX.2: the drag / gesture edits the binding's alt value
  uint32_t wheelTimer_ = 0;
  double dragRaw_ = 0.0, wheelRaw_ = 0.0;
  Vec2 dragLast_{0, 0};
  TextBuffer typeIn_;
  std::string centreText_;
  bool closed_ = false;

  // measured (device px)
  float ringR_[kMaxDepth]{};    // bubble radius per depth
  float resultR_ = 0.0f;        // bubble radius of the results ring
  float centreR_ = 0.0f, gap_ = 0.0f;
  float scaleAt_ = 0.0f;        // measure() ran at this scale (0: not yet)
  bool measuredWithText_ = false;

  // layout
  IRect laidOutFor_{};
  Vec2 centre_;
  float ringRadius_[kMaxDepth + 1]{};  // ring k's distance from the centre (index kMaxDepth: the results ring)
  float ringExt_[kMaxDepth]{};         // ring k's largest radial half extent
  std::vector<Slot> slots_;            // ring 0's layout units (reserved at open)
  std::vector<int32_t> order_;         // scratch: ring-0 slots by angle
  std::vector<Bubble> bubbles_;
  std::vector<int32_t> bubbleOf_;  // node → bubble index (-1 not shown)
  IRect bbox_{};
  float haloR_ = 0.0f;
  IRect tag_{};  // the highlighted leaf's name tag
  int32_t tagNode_ = -1;

  // bands (SX.5)
  struct Row {
    int32_t band, first, count;  // band 0 above, 1 below; rowNodes_[first .. first + count)
    float w, h;
  };
  bool bands_ = false;
  float bandX_ = 0.0f;
  std::vector<Row> rows_;            // reserved at open
  std::vector<int32_t> rowNodes_;    // reserved at open
  IRect bandRect_[2]{};
  bool bandFlipped_[2]{};

  // animation
  double openedAt_ = -1.0;
  float progress_ = 1.0f;
};

// Opens a radial menu centred on `anchor` (a rect in `from`'s tree, e.g. the clicked symbol; for a tree drawn inside
// another tree's widget it is mapped where the host shows it) in `from`'s overlay layer. nullptr when there is none.
RadialMenu* openRadialMenu(Widget& from, const IRect& anchor, std::vector<RadialItem> items, RadialMenuOptions options);

}  // namespace pq::ui
