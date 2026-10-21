#pragma once

// The clip view (T3.11, SPEC §4, §9.4): the detail view's MIDI note editor in Ableton Live 12's layout.
//
//   clip-box | corner (Fold, Scale) | ruler (loop brace, start / end markers, bar numbers)
//            | keys (piano gutter)  | notes (rows, grid, notes)
//            | lane-header (Chance) | velocity (stems, or chance)
//
// It edits the clip ViewContext::detailClip() names (hidden while there is none: the slot shows "No clip
// selected"). EDO mode: one row per step of the clip's tuning (a MOS / Scala tuning gets one row per degree in the
// same uniform layout until T3.12 adds its proportional layouts; pq/views/piano_roll/row_model.h), equave markers,
// Q2 labels, scale highlighting, fold to used notes / scale. Ratio and Cents notes sit at their true pitch between
// rows and are marked off-grid (the theme's roll.offGrid border and a triangle pointing to the side of the nearest row
// the pitch lies on). The grid is the context's shared GridSpec at this editor's zoom.
//
// Every gesture (draw, move, copy, resize, velocity / chance drag, brace drag) is previewed while it runs and
// dispatched as one edit when it ends: one undo step each. Keys (scope "pianoroll", docs/HOTKEYS.md): Up / Down
// and Shift+Up / Down (identity-aware steps, pq::doc::ops::transposeNotes), Delete, Ctrl+D, Ctrl+U, Ctrl+A,
// Ctrl+C / X / V, 0; B (draw mode) and Ctrl+1–5 (grid) are global. Clicking or drawing a note auditions it through
// TransportPort::auditionHz.
//
// Painting is virtualised by the visible time range (note chunks outside it are skipped) and by the visible rows.
// UI thread only.

#include <span>

#include "pq/core/grid.h"
#include "pq/document/edit_ops.h"
#include "pq/ui/widget.h"
#include "pq/views/piano_roll/row_model.h"
#include "pq/views/shell/shell.h"
#include "pq/views/view_context.h"

namespace pq::views {

namespace roll {
class RollState;
}

class ClipView : public ui::Widget {
public:
  explicit ClipView(ViewContext& ctx);
  ~ClipView() override;

  void paint(ui::PaintContext& ctx) override;
  void arrange(ui::LayoutPass& pass) override;

  // --- state (tests, scenes) ---
  bool hasClip() const noexcept;
  const doc::ClipRef& clip() const noexcept;
  const roll::RowModel& rows() const noexcept;
  // Selected note ids (raw values, sorted).
  std::span<const uint32_t> selection() const noexcept;
  void select(std::span<const doc::NoteId> ids);
  // The editing grid at the current zoom.
  Grid grid() const noexcept;
  // The clip's virtual fundamental as a denominator D (fundamental = reference / D; 0 = no Ratio notes). Ratio
  // notes step on its harmonics (Q40); drawn per SPEC §4.3. The rule is at RollState::virtualFundamental.
  int64_t virtualFundamental() const;

  // --- view ---
  double pxPerTick() const noexcept;  // device px
  double scrollTick() const noexcept;
  // Horizontal zoom keeping `anchorTick` where it is on screen.
  void setZoom(double devicePxPerTick, double anchorTick);
  void scrollToTick(double contentTick);
  int32_t rowHeight() const noexcept;  // logical px
  void setRowHeight(int32_t logicalPx);
  void centreOnDegree(int64_t absoluteDegree);
  void fitToClip();
  bool fold() const noexcept;
  void setFold(bool on);
  bool scaleHighlight() const noexcept;
  void setScaleHighlight(bool on);
  bool showChance() const noexcept;
  void setShowChance(bool on);

  // --- geometry (device px; tests script pointer input with these) ---
  float xOfTick(double contentTick) const noexcept;
  // Centre y of the row of an absolute degree (−1 when it is not shown).
  float yOfDegree(int64_t absoluteDegree) const noexcept;
  // The note's rect as drawn (empty when it is not visible).
  ui::IRect noteRect(doc::NoteId id) const;
  // The head of the note's velocity (or chance) stem.
  ui::Vec2 stemHead(doc::NoteId id) const;

  // --- commands (also the pianoroll actions) ---
  void stepSelection(int32_t steps, doc::ops::StepUnit unit);
  void deleteSelection();
  void duplicateSelection();
  void quantizeSelection();
  void selectAll();
  void toggleSelectionActive();
  void copySelection();
  void cutSelection();
  void paste();

  roll::RollState& state() noexcept { return *state_; }

private:
  void onDocument();
  void onView(uint32_t bits);

  ViewContext& ctx_;
  std::unique_ptr<roll::RollState> state_;
  ViewContext::Subscription docSub_, viewSub_;
};

// Adds the clip view to the shell's ClipDetail slot and installs its pianoroll actions (one line in
// installDefaultContent).
ClipView& installClipView(Shell& shell);

}  // namespace pq::views
