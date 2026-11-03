#pragma once

// The node menu (TX.node-menu, SPEC §15 2026-11-02 "cables and the node menu"): Tab in the project patch view and in the
// circuit editor, a cable dropped on empty space, Tab over a cable. A dark panel at the pointer, keyboard first:
//
//   search box   at the top, focused: typing goes straight in (fuzzy over names, aliases and descriptions; every
//                space-separated term must match). Text that names no catalog entry but parses as a patch-language node
//                spec (`tanh`, `0.5`, `440Hz`, `* 2`, `param cutoff = 1kHz`) is offered first as "Place …" (Enter).
//   tabs         down the left: All, Atoms, Meta modules, Instruments, Audio effects, MIDI effects, Visualisers, User
//                library, Recent (Tab / Shift+Tab cycle them; a click picks one)
//   list         the matches grouped by category (headers), compatible entries first (a cable's menu: nodes with an
//                input / an output), the rest dimmed after them; Up / Down / PageUp / PageDown / Home / End move the
//                highlight, the wheel scrolls, hovering highlights, a click or Enter places, Esc closes
//   preview      the highlighted entry: name, kind chip, "N params, M in", description, its ports, PARAMETERS in two
//                columns (name, default)
//
// The menu only chooses: what a choice places is the opener's (onPick / onSpec). Painted in the theme's menu and text
// tokens, no widget per row. CPU: the catalog is shared (built once per library and index snapshot); a keystroke
// re-runs the search (one pass, ≈ 300 entries) and repaints the menu only; no timers, no frames while open and idle.
// UI thread only.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "pq/ui/controls.h"
#include "pq/ui/overlay.h"
#include "pq/views/circuit/node_catalog.h"

namespace pq::views::circuit {

struct NodeMenuOptions {
  std::shared_ptr<const NodeCatalog> catalog;
  NodeFilter filter = NodeFilter::None;
  std::string title;  // shown left of the search box ("Add", "Connect Out →", "Insert on cable"); "" = "Add node"
  NodeTab tab = NodeTab::All;
  std::vector<graph::SubcircuitRef> callees;  // the circuit editor: its file's patches (typed specs may call them)
  std::function<void(const NodeEntry& e)> onPick;       // a catalog entry chosen (the menu then closes)
  std::function<void(const std::string& spec)> onSpec;  // a typed spec chosen
  std::function<void()> onClosed;                       // closed for any reason (after onPick / onSpec)
};

class NodeMenu : public ui::Control {
public:
  explicit NodeMenu(NodeMenuOptions options);

  // ---- state (tests) ----
  const std::string& query() const noexcept { return buf_.text(); }
  void setQuery(std::string q);
  NodeTab tab() const noexcept { return tab_; }
  void setTab(NodeTab t);
  struct Row {
    enum class Kind : uint8_t { Header, Entry, Spec };
    Kind kind = Kind::Entry;
    uint32_t entry = 0;     // Entry: index into the catalog
    NodeCategory category = NodeCategory::Atoms;  // Header
    bool compatible = true;
  };
  const std::vector<Row>& rows() const noexcept { return rows_; }
  int32_t highlighted() const noexcept { return hi_; }  // a row index (never a header), −1 none
  void setHighlighted(int32_t row);
  const NodeEntry* highlightedEntry() const noexcept;
  bool specOffered() const noexcept { return specValid_; }
  const NodeCatalog& catalog() const noexcept { return *opt_.catalog; }
  // Places the highlighted row (as Enter does). False: nothing highlighted.
  bool choose();
  // Device-px rects from the last layout: a tab, a list row (scrolled; empty when not visible), the preview pane.
  ui::IRect tabRect(NodeTab t) const noexcept;
  ui::IRect rowRect(int32_t row) const noexcept;
  ui::IRect previewRect() const noexcept;

  static constexpr float kWidth = 740.0f, kHeight = 452.0f;  // logical px (at most the window less a margin)
  // TX.node-menu-fix1: it opens with a fade and a slight growth from the pointer's corner over kOpenSeconds (frames
  // only then). finishOpening(): shown at once (scenes, goldens).
  static constexpr double kOpenSeconds = 0.12;
  static constexpr float kPointerGap = 12.0f;  // logical px between the pointer and the menu's corner
  void finishOpening() noexcept;
  float openProgress() const noexcept;  // 0 … 1

  ui::ISize contentExtent(ui::ISize viewport) const override;
  void paint(ui::PaintContext& ctx) override;
  void onFrame(double now) override;
  bool onPointer(const ui::PointerEvent& e) override;
  bool onKey(const ui::KeyEvent& e) override;
  bool onText(const ui::TextEvent& e) override;
  ui::InfoText info() const override;

private:
  struct Metrics {
    int32_t pad, header, tabsW, tabH, rowH, headH, listW;
  };
  Metrics metrics() const noexcept;
  ui::IRect listRect() const noexcept;
  int32_t rowTop(int32_t row) const noexcept;  // offset of a row from the list's content top (device px)
  int32_t contentHeight() const noexcept;
  void refresh();     // search again (query or tab changed)
  void ensureVisible(int32_t row);
  int32_t step(int32_t from, int32_t dir) const noexcept;
  int32_t rowAt(ui::Vec2 p) const noexcept;
  void close();
  void paintMenu(ui::PaintContext& ctx);
  void paintPreview(ui::PaintContext& ctx, const ui::IRect& r);

  NodeMenuOptions opt_;
  ui::TextBuffer buf_;
  NodeTab tab_ = NodeTab::All;
  std::vector<Row> rows_;
  int32_t hi_ = -1;
  int32_t scroll_ = 0;  // device px
  bool specValid_ = false;
  bool closing_ = false;
  double openAt_ = -1.0;  // the open animation's start (−1: not yet, −2: done)
};

// Opens the menu in `from`'s tree's overlay layer, focused: its top-left kPointerGap right of and below device point
// `at` (its top-right left of it when there is no room on the right; always inside the window). Null when there is no
// overlay layer.
NodeMenu* openNodeMenu(ui::Widget& from, ui::Vec2 at, NodeMenuOptions options);

}  // namespace pq::views::circuit
