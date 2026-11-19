#pragma once

// MX.1: a nested radial ("bubble") menu. Any widget opens it with a tree of items centred on a point (the symbol that
// was clicked); it lives in the tree's OverlayLayer like the other popups (pq/ui/overlay.h).
//
//   ring 0      the top-level items (categories) in a full ring around the anchor; a disc at the centre shows the
//               current item's glyph, or the text typed so far
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
// Look: theme tokens only (menu, surface, selection, accent, text); glyphs in SceneFonts::symbol (18 SemiBold) when
// they fit the bubble, else the theme's title font; a leaf's name shows in a tag outside its bubble while it is
// highlighted. A 120 ms open animation (rings grow from 70 %, fade in); there is no reduced-motion setting yet
// (RadialMenuOptions::animate = false skips it).
// CPU: items are flattened and measured once per open (and per UI-scale change); the bubble layout is rebuilt only
// when a ring opens / closes or the filter changes, into storage reserved at open. Paint allocates nothing, damages
// only the menu's bounding box and does nothing for damage elsewhere. Nothing runs while it is idle; closed, it is gone.
//
// UI thread only.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "pq/ui/text.h"
#include "pq/ui/widget.h"

namespace pq::ui {

struct RadialItem {
  std::string key;       // a leaf's identity: what onPick reports (unique among the leaves)
  std::string label;     // its name ("greater than", "Compare")
  std::string glyph;     // optional symbol drawn in the bubble (">"); empty: the label is drawn
  std::string keywords;  // optional extra search terms, space separated (">=", "arctangent")
  std::vector<RadialItem> children;  // non-empty: an item that opens a ring (a category)

  static RadialItem leaf(std::string key, std::string label, std::string glyph = {}, std::string keywords = {});
  static RadialItem category(std::string label, std::vector<RadialItem> children);
};

struct RadialMenuOptions {
  std::string current;                          // key of the current leaf: opened to and marked ("" none)
  std::function<void(std::string_view key)> onPick;  // after the menu closed
  std::function<void()> onClosed;               // once, whenever it closes (before onPick on a pick)
  bool animate = true;                          // the open animation
};

class RadialMenu : public Widget {
public:
  static constexpr int32_t kMaxDepth = 6;  // rings (deeper items are not shown)

  // `anchor`: device px in the overlay layer's tree.
  RadialMenu(std::vector<RadialItem> items, Vec2 anchor, RadialMenuOptions options, std::string name = "radial-menu");

  // --- queries (tests, the opener) ---
  // The item with this leaf key, else the first with this label; -1 if none. Items are numbered breadth first.
  int32_t find(std::string_view keyOrLabel) const noexcept;
  int32_t itemCount() const noexcept { return static_cast<int32_t>(nodes_.size()); }
  const std::string& keyOf(int32_t item) const noexcept { return nodes_[static_cast<size_t>(item)].key; }
  const std::string& labelOf(int32_t item) const noexcept { return nodes_[static_cast<size_t>(item)].label; }
  // Whether the item's bubble is shown, and its centre / radius (device px, from the last layout).
  bool shown(int32_t item) const noexcept;
  Vec2 centreOf(int32_t item) const noexcept;
  float radiusOf(int32_t item) const noexcept;
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
  // Opening animation progress 0..1.
  float openProgress() const noexcept { return progress_; }

  // Picks a leaf: closes the menu, then onPick. Ignored for items with children.
  void pick(int32_t item);
  void close();

  void arrange(LayoutPass& pass) override;
  void paint(PaintContext& ctx) override;
  bool onPointer(const PointerEvent& e) override;
  bool onKey(const KeyEvent& e) override;
  bool onText(const TextEvent& e) override;
  void onFrame(double now) override;
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
  };
  struct Bubble {
    Vec2 c;
    float r;
    int32_t node;
    int32_t ring;  // -1: a filter result
  };
  struct Result {
    int32_t score;
    int32_t node;
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
  void placeArc(int32_t ring, int32_t node0, int32_t n, float baseAngle, float R, float r);

  std::vector<Node> nodes_;
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
  std::vector<Bubble> bubbles_;
  std::vector<int32_t> bubbleOf_;  // node → bubble index (-1 not shown)
  IRect bbox_{};
  float haloR_ = 0.0f;
  IRect tag_{};  // the highlighted leaf's name tag
  int32_t tagNode_ = -1;

  // animation
  double openedAt_ = -1.0;
  float progress_ = 1.0f;
};

// Opens a radial menu centred on `anchor` (a rect in `from`'s tree, e.g. the clicked symbol; for a tree drawn inside
// another tree's widget it is mapped where the host shows it) in `from`'s overlay layer. nullptr when there is none.
RadialMenu* openRadialMenu(Widget& from, const IRect& anchor, std::vector<RadialItem> items, RadialMenuOptions options);

}  // namespace pq::ui
