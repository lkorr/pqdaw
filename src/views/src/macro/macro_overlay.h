#pragma once

// What macro mode draws (T8.1; private to pq_views). The clip view owns the two overlays and places them over the note
// area and the keys; they let the pointer through and are hidden while macro mode is off (nothing is painted or
// measured then).
//
//   MacroOverlay    over the note area: a frame and a "MACRO · 12-EDO" tab (the mode is on), the cursor (a box one note
//                   long on the pitch the next note gets, with a line along that pitch and one down its start), the
//                   anchor (a ring on the anchor's pitch, the anchor note outlined, a connector to the cursor) and a
//                   label beside the cursor: the composition as it is built ("3/2 · 5/4 = 15/8") and what it becomes
//                   ("→ 11 steps, −11.7 ¢", "just · harmonic 6").
//   MacroKeyLabels  over the keys: the cursor's row marked, and on every row the fewest keys that reach it from the
//                   anchor (the trainer's breadth-first search; EDOs).
//   MacroLamp       the control bar's switch: "MACRO", lit with the layer while on ("MACRO · JI", "MACRO · 12-EDO").

#include <string>

#include "pq/ui/controls.h"
#include "pq/ui/widget.h"
#include "pq/views/view_context.h"

namespace pq::views {

namespace roll {
class RollState;
}

class MacroOverlay : public ui::Widget {
public:
  explicit MacroOverlay(roll::RollState& s);
  void paint(ui::PaintContext& ctx) override;
  void onFrame(double now) override;
  // The cursor's box as drawn (device px; empty: off or out of view). Tests.
  ui::IRect cursorRect() const;

private:
  roll::RollState& s_;
};

class MacroKeyLabels : public ui::Widget {
public:
  explicit MacroKeyLabels(roll::RollState& s);
  void paint(ui::PaintContext& ctx) override;

private:
  roll::RollState& s_;
};

class MacroLamp : public ui::Control {
public:
  explicit MacroLamp(ViewContext& ctx);
  // Re-reads the mode (text, light, tooltip); repaints when something it shows changed.
  void refresh();
  const std::string& text() const noexcept { return text_; }
  bool on() const noexcept { return on_; }
  void paint(ui::PaintContext& ctx) override;
  bool onPointer(const ui::PointerEvent& e) override;
  void onCaptureLost() override { setPressed(false); }

private:
  ViewContext& ctx_;
  std::string text_;
  bool on_ = false;
};

}  // namespace pq::views
