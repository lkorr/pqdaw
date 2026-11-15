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
#include <string>
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
class MacroOverlay;    // T8.1 (src/macro/macro_overlay.h)
class MacroKeyLabels;

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
  // KB.help: the help window (the `?` button left of Fold / Scale, at the right end of the clip box's title bar: the
  // keys' gutter has no room for a third button; roll_help.h): every control of the note editor, its
  // text a table (roll_help_content.cpp), its keys read from the loaded keymap. KB.help2: a floating window over the
  // main window that the roll keeps working under. toggleHelp() is the button's click: opens it (where the session
  // last had it), or closes it; returns whether it is open now.
  bool toggleHelp();
  bool helpOpen() const;
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
  //   declareHarmonic    "Is Harmonic N" (T7.13, ops::makeJiGroup): the note becomes the frozen anchor of a JI group
  //                      with that index; the active group's series is drawn as guides, the plain draw puts members
  //                      on it (Shift: the rows), members and the anchor show their harmonic numbers.
  //                      removeActiveJiGroup: ops::removeJiGroup of the active group (activeJiGroup: the group of
  //                      the first selected note in one, else the clip's newest).
  // Drawing with Shift (draw mode, or Shift+double-click) adds Ratio notes on the harmonics of the virtual fundamental
  // (no fundamental yet: the row's pitch snapped to the simplest ratio). Ratio notes show "n/d · k" when they fit.
  bool convertSelection(tuning::PitchKind to);
  bool setSelectionPitch(const tuning::PitchId& p);
  bool enterPitch(std::string_view text);
  std::optional<tuning::PitchId> parsePitch(std::string_view text) const;
  // The pitch type-in popover for the selection (the menu's Set Pitch…): returns its text field.
  ui::TextEdit* openPitchEntry();
  bool declareHarmonic(doc::NoteId note, uint32_t n);
  bool removeActiveJiGroup();
  doc::JiGroupId activeJiGroup() const;  // invalid: none
  // What the keys print on each row (the clip box's Row Labels, the keys' right-click menu). View state.
  roll::RowLabels rowLabels() const noexcept;
  void setRowLabels(roll::RowLabels mode);
  // The info view's text for the pointer at device `p` over the note area (keys = false) or the keys: the hovered
  // note's identity and pitch, or the row's (cents, Hz, nearest ratio, overlay EDO step). Tests read it directly.
  ui::InfoText readoutAt(ui::Vec2 p, bool keys);

  // --- JI (T7.13; roll_ji.cpp) ---
  // "8:9:10:12:15": strictly ascending whole terms. The error says what is wrong.
  static Result<std::vector<int64_t>> parseProportion(std::string_view text);
  // --- JI groups, warps, vibrato (T7.14; roll_groups.cpp, roll_vibrato.cpp) ---
  // The commands behind the keys / menus / fields (each one undo step): Ctrl+Shift+J (ops::tuneToJi on the selection,
  // or ops::untuneJiGroup when it already is one tuned group), Alt+Up / Down (ops::walkJiGroup on the active group),
  // the proportion tool's field (ops::proportionNotes) and "Add Note at Ratio…" (ops::addGroupNote; "3/2", "7:4" to
  // that note, or a harmonic number).
  bool toggleTuneToJi();
  bool walkActiveGroup(int32_t steps);
  bool insertProportion(Ticks start, Ticks length, int64_t bottomDegree, const std::vector<int64_t>& terms);
  bool addNoteAtRatio(doc::NoteId note, std::string_view text);
  bool proportionArmed() const noexcept;  // J was pressed: the next drag in the note area is the proportion's box
  // Where a group's frame and its parts are (device px; tests script the pointer with these). The corner grips, the
  // warp markers and their glide grips are centred on the frame's bottom border.
  struct GroupGeometry {
    bool valid = false;
    ui::Rect box{}, header{};
    ui::Vec2 proportion{};  // the middle of the proportion inside the header (a click opens the chooser)
    ui::Vec2 leftGrip{}, rightGrip{};
    std::vector<ui::Vec2> markers, glideGrips;  // one per warp
    std::string headerText, proportionText;
    bool tuned = false;       // a member remembers what it was written as
    bool inJi = true;         // T7.16a: the group's state (false: Back to Tuning)
    bool gripsShown = false;  // the corner grips (a tuned group that is active or under the pointer)
  };
  GroupGeometry groupGeometry(doc::JiGroupId id) const;
  int32_t selectedWarp() const noexcept;  // the selected warp marker's index (−1: none)
  // A note's ~ vibrato grip and the corners of its vibrato's envelope (0 onset, 1 end of the fade-in, 2 start of the
  // fade-out, 3 end); nullopt: not placed / no vibrato.
  std::optional<ui::Vec2> vibratoGrip(doc::NoteId id) const;
  std::optional<ui::Vec2> vibratoCorner(doc::NoteId id, int corner) const;
  // The pitch the roll draws a note at, `frac` of the way through it, in cents from the reference (its effective
  // curve: bends, warps, vibrato, drawn curve); the device y of a pitch.
  std::optional<double> noteCentsAt(doc::NoteId id, double frac) const;
  // The start pitch a running drag previews the note at (its own pitch when the drag does not move it). Tests.
  std::optional<double> notePreviewCents(doc::NoteId id) const;
  float yOfCents(double cents) const noexcept;
  // How many note paths were flattened so far (a repaint of an unchanged clip adds none: M7 review M14).
  uint64_t pathBuilds() const noexcept;
  // The notes outlined as bend targets while an Alt + edge drag runs.
  std::vector<doc::NoteId> bendHighlights() const;
  // Wall-clock time of the note area's last paint (tests).
  struct PaintStats {
    double notesMs = 0.0;
  };
  PaintStats paintStats() const noexcept;
  // The harmonic number the roll shows on a note (the anchor of a JI group, a numbered member, a reference Ratio over
  // the virtual fundamental); the screen y of harmonic k's guide line (the active group's series, else the virtual
  // fundamental's) at content tick `tick` (a group's guides follow its anchor's pitch; nullopt: at the anchor's
  // start). nullopt: none.
  std::optional<uint64_t> harmonicOf(doc::NoteId note) const;
  std::optional<float> harmonicGuideY(uint64_t k, std::optional<double> tick = std::nullopt) const;
  // A point `frac` of the way through a note where it is drawn (on its row, or along its path). {−1, −1}: none.
  ui::Vec2 notePoint(doc::NoteId id, double frac) const;

  // --- the playhead and the scrub area (bug-roll-playhead; roll_playhead.cpp) ---
  // While playing, the transport's position is drawn through the ruler, the notes and the velocity lane in clip time
  // (the clip's start, its start marker, the loop's wrap) whenever the song position is inside the clip; hidden
  // outside it. Read from the port only while playing and shown, on a timer due when the line next moves a device
  // pixel; each move damages the line's old and new columns only. With Follow on (ViewContext::follow) the roll pages
  // (the playhead to the left edge) when the playhead leaves the view or enters the clip outside it — not while you
  // drag, nor once you scrolled away from it yourself.
  // The ruler off the brace and the flags is the scrub area: a click plays from there (playing: jumps there), a drag
  // that starts sideways scrubs; snapped to the roll's grid, Alt unsnapped.
  // The content tick drawn (nullopt: none) and how many times the port was read (tests).
  std::optional<Ticks> playheadTick() const noexcept;
  uint64_t playheadReads() const noexcept { return playheadReads_; }
  void onTimer(uint32_t id) override;

  // --- macro mode (T8.1; pq/views/macro/macro_mode.h, ViewContext::macro()) ---
  // While macro mode is on, the roll shows its cursor, anchor and composition (macro_overlay.h) and the keys show the
  // fewest keys to each row. The cursor's box as drawn (device px; empty: off, or out of view).
  ui::IRect macroCursorRect() const;

  roll::RollState& state() noexcept { return *state_; }

private:
  void onDocument();
  void refreshMacro();  // T8.1: the overlays follow the mode (shown only while it is active)
  void onView(uint32_t bits);
  // bug-roll-playhead (roll_playhead.cpp).
  bool shownNow() const noexcept;
  void refreshPlayhead();
  void readPlayhead();
  void schedulePlayhead(Ticks song);
  void showPlayhead(std::optional<Ticks> content);
  void damagePlayhead(Ticks content);

  ViewContext& ctx_;
  std::unique_ptr<roll::RollState> state_;
  ViewContext::Subscription docSub_, viewSub_;
  ui::Toggle* help_ = nullptr;  // KB.help: `?`; lit while the panel is open
  MacroOverlay* macroOverlay_ = nullptr;  // T8.1: over the note area
  MacroKeyLabels* macroKeys_ = nullptr;   // T8.1: over the keys
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);  // the panel's close callback may outlive the view
  uint32_t playheadTimer_ = 0;
  uint64_t playheadReads_ = 0;
};

// Adds the clip view to the shell's ClipDetail slot and installs its pianoroll actions (one line in
// installDefaultContent).
ClipView& installClipView(Shell& shell);

}  // namespace pq::views
