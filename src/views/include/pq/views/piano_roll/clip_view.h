#pragma once

// The clip view (T3.11, SPEC §4, §9.4): the detail view's MIDI note editor in Ableton Live 12's layout.
//
//   clip-box | corner (Fold, Scale) | ruler (loop brace, start / end markers, bar numbers)
//            | keys (piano gutter)  | notes (rows, grid, notes)
//            | lane-header (Chance) | velocity (stems, or chance)
//
// It edits the clip ViewContext::detailClip() names (hidden while there is none: the slot shows "No clip
// selected"). EDO mode: one row per step of the clip's tuning; MOS / Scala: one row per degree, Proportional or
// Equidistant with step-class keys, an optional background EDO and a live generator morph (T3.12; row_model.h);
// equave markers, Q2 labels, scale highlighting, fold to used notes / scale. Ratio and Cents notes sit at their true pitch between
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

#include <optional>
#include <span>
#include <string_view>

#include "pq/core/grid.h"
#include "pq/document/edit_ops.h"
#include "pq/ui/controls.h"
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
  // The clip's virtual fundamental relative to its reference (1/D derived from the Ratio notes, or the lock;
  // nullopt = none). Ratio notes step on its harmonics (Q40); drawn per SPEC §4.3. The rule is the document's
  // (doc::virtualFundamental, T3.6-fix1).
  std::optional<tuning::Rational> virtualFundamental() const;

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
  // Scale / MOS layouts (T3.12): Proportional (default) or Equidistant rows for MOS and Scala tunings (EDOs are
  // always Uniform); switching keeps the centred row. View state, not in the document.
  roll::RowLayout scaleLayout() const noexcept;
  void setScaleLayout(roll::RowLayout layout);
  // Background EDO grid of the octave (0 = off): drawn behind the rows; Cents notes step / move on it.
  uint32_t overlayEdo() const noexcept;
  // The virtual fundamental's guide (line, harmonics, labels strip, F chip): the clip box's JI switch (T3.x-fix-look).
  bool showHarmonics() const noexcept;
  void setShowHarmonics(bool on);
  void setOverlayEdo(uint32_t n);
  // Live generator morph of a MOS clip (what the clip box's Generator box does): previews every step through the
  // context's tuning preview, end(true) commits one SetClipTuning (one undo step), end(false) restores.
  void beginGeneratorMorph();
  void morphGenerator(double generatorCents);
  void endGeneratorMorph(bool commit);
  // How many tuning changes were followed in place (MosLinearTable interval) vs by a rebuild (tests, bench).
  uint64_t morphLinearFrames() const noexcept;
  uint64_t morphRebuilds() const noexcept;

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
  // Left / Right (T3.x-fix-look): the selected notes move by `cells` grid cells (relative snap from the earliest
  // selected start, as a drag), one undo step.
  void nudgeSelection(int32_t cells);
  void deleteSelection();
  void duplicateSelection();
  void quantizeSelection();
  void selectAll();
  void toggleSelectionActive();
  void copySelection();
  void cutSelection();
  void paste();

  // --- identities, ratio entry, labels (T3.19, SPEC §3.1 / §4.3, M3 review H7) ---
  // The right-click menu of the note area offers these; each edit is one undo step.
  //   convertSelection   Degree ↔ Ratio ↔ Cents (pq::doc::ops::convertNotes: Ratio = the simplest ratio within
  //                      ops::ratioSnapTolerance, so Degree → Ratio → Degree is the identity)
  //   setSelectionPitch  a typed identity (enterPitch parses "7/4", "386.3c", "4\22", "4\22+1")
  //   lockFundamental    "this note is the index-th harmonic" (ops::lockFundamental) / unlockFundamental
  // Drawing with Shift (draw mode, or Shift+double-click) adds Ratio notes on the harmonics of the virtual fundamental
  // (no fundamental yet: the row's pitch snapped to the simplest ratio). Ratio notes show "n/d · k" when they fit.
  bool convertSelection(tuning::PitchKind to);
  bool setSelectionPitch(const tuning::PitchId& p);
  bool enterPitch(std::string_view text);
  std::optional<tuning::PitchId> parsePitch(std::string_view text) const;
  // The pitch type-in popover for the selection (the menu's Set Pitch…): returns its text field.
  ui::TextEdit* openPitchEntry();
  bool lockFundamental(doc::NoteId note, uint32_t harmonicIndex);
  bool unlockFundamental();
  // What the keys print on each row (the clip box's Row Labels, the keys' right-click menu). View state.
  roll::RowLabels rowLabels() const noexcept;
  void setRowLabels(roll::RowLabels mode);
  // The info view's text for the pointer at device `p` over the note area (keys = false) or the keys: the hovered
  // note's identity and pitch, or the row's (cents, Hz, nearest ratio, overlay EDO step). Tests read it directly.
  ui::InfoText readoutAt(ui::Vec2 p, bool keys);

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
