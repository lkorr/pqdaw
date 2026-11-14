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
// Ctrl+C / X / V, 0, J (the JI tool, T7.4); B (draw mode) and Ctrl+1–5 (grid) are global. Clicking or drawing a note auditions it through
// TransportPort::auditionHz.
//
// Painting is virtualised by the visible time range (note chunks outside it are skipped) and by the visible rows.
// UI thread only.

#include <optional>
#include <span>
#include <string_view>
#include <vector>

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
  // Live generator morph of a MOS clip (what the clip box's Generator box does): applies every step to the
  // document inside one store gesture (TU.3: edits of the clip's NAMED tuning, SetTuningParam / SetTuningDef, so every
  // clip and track on it follows), end(true) commits it (one undo step), end(false) restores.
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
  // T4.16b: the note part highlighted under the pointer (the one a press would grab): note id 0 = none; zone −1 the
  // start, +1 the end, 0 the body.
  struct NoteHover {
    uint32_t note = 0;
    int zone = 0;
  };
  NoteHover noteHover() const noexcept;

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

  // --- JI objects (T7.4, SPEC §5; roll_ji.cpp) ---
  // Drawn as a box over the object's span and pitch range with its voices as lines through the morphs, harmonic
  // numbers, a header, stop markers and the ride curve. The JI tool (J, "pianoroll.toggleJiTool"): drag a box, then
  // type a proportion ("8:9:10:12:15"; "1/4:1/5:1/6" = undertone) in the field that opens; the lowest voice sits on
  // the box's bottom row. An object's header or border selects it (drag: move in time and by rows; left / right
  // border: resize); Delete and Ctrl+D act on the selected object. One undo step each.
  bool jiTool() const noexcept;
  void setJiTool(bool on);
  doc::JiObjectId jiSelection() const noexcept;
  void selectJiObject(doc::JiObjectId id);
  // The object's box as drawn, device px, not clipped (empty: no such object). Its header is the strip above it.
  ui::IRect jiObjectRect(doc::JiObjectId id) const;
  // What the proportion field accepts: "8:9:10:12:15" (overtone; terms strictly ascending), "1/4:1/5:1/6" (undertone:
  // terms 4, 5, 6, in any order). The error says what is wrong.
  struct Proportion {
    std::vector<int64_t> terms;
    bool undertone = false;
  };
  static Result<Proportion> parseProportion(std::string_view text);
  // Wall-clock time of the note area's last paint and of its JI-object part; the objects it drew (tests).
  struct PaintStats {
    double notesMs = 0.0;
    double jiMs = 0.0;
    uint32_t jiPainted = 0;
  };
  PaintStats paintStats() const noexcept;

  // --- inner roll and morph editing (T7.5; roll_ji_edit.cpp) ---
  // A double-click on an object enters it (Esc / a double-click outside leaves): its voices are the rows inside the
  // box, the rest of the roll is dimmed, drawing and double-clicks put Harmonic notes on the voice under the pointer,
  // typing a proportion adds a stop at the pointer, T a tuning stop. A stop's label under the box selects it (drag:
  // move, double-click: retype, Delete: remove), the grip on the bottom border sets its glide; a harmonic number
  // selects that term (Alt+Up / Down: ops::walkJiTerm). "Tune to JI" (Ctrl+Shift+J, "pianoroll.tuneToJi") turns the
  // selected notes into the inner notes of an object that glides from the tuning to the nearest proportion; "Is
  // Harmonic…" in a note's menu locks the virtual fundamental at note / N.
  doc::JiObjectId jiEntered() const noexcept;
  void enterJiObject(doc::JiObjectId id);
  void leaveJiObject();
  int32_t jiStopSelection() const noexcept;  // index into the object's stops; −1 none
  int32_t jiTermSelection() const noexcept;  // the voice whose harmonic number is selected; −1 none
  void selectJiStop(doc::JiObjectId id, int32_t stop);
  void selectJiTerm(doc::JiObjectId id, int32_t voice);
  bool tuneSelectionToJi();
  bool lockFundamentalAt(doc::NoteId note, uint32_t n, bool subharmonic);
  // Where the object's parts are on screen (device px; empty: not shown): a stop's label under the box, a voice's
  // harmonic number, a stop's glide grip on the bottom border. `index`: the stop (StopLabel, Glide) or the voice.
  enum class JiPart : uint8_t { StopLabel, VoiceLabel, Glide };
  ui::IRect jiPartRect(doc::JiObjectId id, JiPart part, int32_t index) const;
  // A point on voice `voice`'s line at object tick `objectTick`; a bezier transition's handle (0 / 1); a point `frac`
  // of the way through a note where it is drawn (on its row, or along its path). {−1, −1}: none.
  ui::Vec2 jiVoicePoint(doc::JiObjectId id, uint32_t voice, Ticks objectTick) const;
  ui::Vec2 jiBezierHandle(doc::JiObjectId id, uint32_t stop, uint32_t voice, int handle) const;
  ui::Vec2 notePoint(doc::NoteId id, double frac) const;

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
