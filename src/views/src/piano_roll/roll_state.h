#pragma once

// The clip view's shared state (private to pq_views, T3.11): the clip being edited and its caches, the view
// transform shared by the ruler / keys / note area / velocity lane, the selection, and the preview of a drag in
// progress. The child widgets (roll_widgets.h) paint from it and turn input into edits through it; it is the only
// place that dispatches commands.
//
// Edits are pq::doc::ops builders dispatched once per user gesture (one history entry each). A drag never edits the
// document while it runs: the widgets paint a preview (DragState) and the edit is dispatched on release, so a drag
// frame costs a repaint of the damaged area and nothing else (the 1 ms drag-frame budget).
//
// Caches, all rebuilt off the paint path when their input changes (pointer compares):
//   - tuning (built from the clip's effective TuningDef) and the RowModel (tuning, row height, fold, used degrees);
//   - per note chunk: its last note end and degree span (ChunkInfo), keyed by the chunk's pointer, so the paint
//     loop skips every chunk outside the visible time range (virtualisation) and a one-note edit recomputes one
//     chunk;
//   - the note colour table (clip colour × 128 velocities, plain / selected / disabled).
//
// UI thread only.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "pq/core/grid.h"
#include "pq/document/edit_ops.h"
#include "pq/document/model.h"
#include "pq/tuning/mos_regions.h"
#include "pq/tuning/tuning.h"
#include "pq/ui/color.h"
#include "pq/ui/controls.h"
#include "pq/ui/widget.h"
#include "pq/views/piano_roll/row_model.h"
#include "pq/views/view_context.h"

namespace pq::views::roll {

class ClipBoxPanel;
class CornerPanel;
class RulerWidget;
class KeysWidget;
class NoteAreaWidget;
class LaneHeaderWidget;
class VelocityLaneWidget;

// What the pointer is doing in the editor.
enum class DragKind : uint8_t {
  None,
  Move,         // selected notes by (dt, steps)
  Copy,         // Alt-drag: copies of the selected notes at (dt, steps); the originals stay
  ResizeEnd,    // selected notes' ends by dt
  ResizeStart,  // selected notes' starts by dt
  Rubber,       // selection rectangle
  Draw,         // draw mode: notes in every grid cell the pointer crosses
  Velocity,     // velocity / chance lane: selected notes' value by dValue
  LoopBrace,    // ruler: the loop brace (start, end or both) by dt
  StartMarker,  // ruler: the clip's start marker
  EndMarker,    // ruler: the clip's end (unlooped clips)
  RulerZoom,    // ruler: vertical drag zooms, horizontal drag scrolls
  KeysZoom,     // keys: horizontal drag zooms the rows
};

enum class BraceEdge : uint8_t { Start, End, Both };

// How a draw gesture picks its notes' pitch identity.
enum class DrawPitch : uint8_t {
  Row,      // a Degree note on the row
  Overlay,  // Alt (T3.12): a Cents note on the background EDO line nearest the pointer
  Ratio,    // Shift (T3.19): a Ratio note on the virtual fundamental's nearest harmonic (no fundamental: the row's
            // pitch snapped to the simplest ratio, ops::simplestRatioNear)
};

struct DrawnNote {
  Ticks start = 0;
  Ticks length = 0;
  int64_t degree = 0;  // the row (a Degree note), or the nearest row of a Cents / Ratio note
  DrawPitch mode = DrawPitch::Row;
  double cents = 0.0;     // Overlay
  tuning::Rational ratio{1};  // Ratio (reference-anchored)
};

// A Ratio note's label waiting to be drawn over the notes (T3.19): its box and the colour it sits on.
struct NoteLabel {
  float x0, x1, y0, y1;
  ui::Rgba fill;
  int64_t num, den;
  uint32_t harmonic;  // its harmonic number over the virtual fundamental (0: not one)
};

struct DragState {
  DragKind kind = DragKind::None;
  bool moved = false;       // the pointer left the press point (a click otherwise)
  ui::Vec2 press{};         // device px
  ui::Vec2 last{};
  doc::NoteId pressed;      // the note under the press (Move / Copy / Resize / Velocity)
  bool pressedWasSelected = false;
  Ticks pressTick = 0;      // content tick under the press
  Ticks originTick = 0;     // the snapping origin (the pressed note's start or end, the brace edge)
  int64_t pressDegree = 0;  // row under the press (the pressed note's row for Move / Copy)
  Ticks dt = 0;             // snapped time delta
  int32_t steps = 0;        // pitch steps (degrees) for Move / Copy
  float dValue = 0.0f;      // Velocity: velocity (1–127) or chance (0–1) delta
  BraceEdge brace = BraceEdge::Both;
  bool additive = false;    // Rubber with Shift / Ctrl: add to the selection
  ui::IRect rubber{};       // device px (Rubber)
  std::vector<DrawnNote> drawn;  // Draw
  // Move / Copy preview pitch of every selected non-Degree note at `steps` (sorted by id), refreshed when steps change.
  std::vector<std::pair<uint32_t, tuning::PitchId>> previewPitch;
  int32_t previewSteps = 0;
  double zoomPxPerTick = 0.0;  // RulerZoom / KeysZoom: the zoom at the press
  double zoomTick = 0.0;       // RulerZoom: content tick kept under the pointer
  int32_t zoomRowPx = 0;
  double pressScrollTick = 0.0;
  std::vector<uint32_t> base;  // Rubber: the selection the band adds to
  std::vector<uint32_t> pressSelection;  // Rubber: the selection before the press (abortDrag restores it)
  // Damage tracking (Move / Copy / Resize / Velocity / Draw): the dragged notes as they were at the press, and the
  // screen box the preview covered last frame. Off for large selections (the whole area repaints).
  bool trackDamage = false;
  std::vector<doc::Note> tracked;
  ui::IRect damage{};
};

// Per note chunk: what the visibility tests need without touching the notes.
struct ChunkInfo {
  const doc::Note* notes = nullptr;
  uint32_t count = 0;
  Ticks firstStart = 0;
  Ticks maxEnd = 0;  // max(start + length) over the chunk
  int64_t minDegree = INT64_MAX, maxDegree = INT64_MIN;  // absolute degrees of its Degree notes
  int64_t ratioDen = 0;
  uint32_t nonDegree = 0;  // notes with another identity (their Up/Down preview needs stepPitch)  // lcm of its reference-anchored Ratio notes' denominators (0 none, −1 too large)
};

// Note colours for one clip colour (index = velocity rounded, 0 = disabled).
struct NoteColours {
  ui::Rgba fill[128];
  ui::Rgba border[128];
  ui::Rgba selFill[128];
  ui::Rgba selBorder;
  ui::Rgba disabledFill, disabledBorder;
  uint32_t forRgb = 0xFFFFFFFFu;
};

// Where a note is drawn (content y; x is computed from ticks at paint).
struct NotePlace {
  int32_t top = 0, bottom = 0;
  int8_t offGrid = 0;  // 0 on its row; +1 above / −1 below the nearest row (marked)
  bool ok = false;     // false: the pitch cannot be placed (Harmonic, JI-fundamental anchors: no JI objects yet)
  uint8_t z = 0;       // the row's z (scale layouts: notes on larger steps paint over smaller ones)
};

// One note waiting to be painted in z order (Proportional layout: rows may overlap).
struct ZNote {
  float x0, x1, y0, y1;
  ui::Rgba fill, border;
  int8_t offGrid;
  uint16_t key;  // z, + 256 when selected (selected notes always on top, as mos-roll.js drawOrder)
};

class RollState {
public:
  explicit RollState(ViewContext& ctx);
  ~RollState();
  RollState(const RollState&) = delete;
  RollState& operator=(const RollState&) = delete;

  ViewContext& ctx;

  // Child widgets (set by ClipView when it builds them).
  ClipBoxPanel* boxPanel = nullptr;
  CornerPanel* cornerPanel = nullptr;
  RulerWidget* rulerView = nullptr;
  KeysWidget* keysView = nullptr;
  NoteAreaWidget* noteArea = nullptr;
  LaneHeaderWidget* laneHeaderView = nullptr;
  VelocityLaneWidget* laneView = nullptr;

  // --- the clip ---
  // Re-resolves the detail clip in the current snapshot and refreshes every cache whose input changed. Returns
  // whether anything the widgets show changed.
  bool refresh();
  bool hasClip() const noexcept { return clip_ != nullptr; }
  const doc::NoteClip* clip() const noexcept { return clip_.get(); }
  const doc::ClipRef& clipRef() const noexcept { return ref_; }
  const tuning::Tuning& tuning() const noexcept { return tuning_; }
  const RowModel& rows() const noexcept { return rows_; }
  const TimeSigMap& sigs() const noexcept { return sigs_; }
  const std::string& tuningName() const noexcept { return tuningName_; }
  ui::Rgba clipColour() const noexcept { return ui::hex(clip_ ? clip_->colour.rgb : 0x808080); }
  const std::vector<ChunkInfo>& chunks() const noexcept { return chunks_; }
  const NoteColours& colours() const noexcept { return colours_; }
  // The loop brace, start marker and length as shown (with a ruler drag's preview applied).
  doc::ClipLoop shownLoop() const noexcept;
  Ticks shownContentStart() const noexcept;
  Ticks shownLength() const noexcept;
  // [start, end) of the clip content that plays: the loop when looped, else start marker … start marker + length.
  Ticks regionStart() const noexcept;
  Ticks regionEnd() const noexcept;

  // --- view transform (device px) ---
  // The note area's rect: the ruler, the velocity lane and the keys share its x or y mapping.
  ui::IRect area() const noexcept;
  double pxPerTick() const noexcept { return pxPerTick_; }
  double scrollTick() const noexcept { return scrollTick_; }
  int32_t scrollY() const noexcept { return scrollY_; }
  int32_t rowPx() const noexcept { return rowPxLogical_; }  // logical px
  float xOfTick(double t) const noexcept;
  double tickAtX(float x) const noexcept;
  int32_t screenY(int32_t contentY) const noexcept;  // content → device y
  int32_t contentY(float screenY) const noexcept;
  void setZoom(double pxPerTick, double anchorTick, float anchorX);  // keeps anchorTick under anchorX
  void scrollToTick(double t);
  void scrollByPx(float dx, float dy);
  void setRowHeight(int32_t logicalPx);   // rebuilds the rows, keeps the centre pitch
  void centreOnDegree(int64_t degree);
  // The degree a fresh view centres on: the middle of the densest view-tall band of notes (M3 review M14).
  int64_t densestDegree(int32_t viewHeightPx) const;
  // Fit the played region horizontally and centre the notes vertically (a new clip in the view).
  void fitToClip();
  // After the parts were placed: rows follow a UI-scale change, the scroll stays in range, a new clip is fitted.
  void afterLayout();
  Grid grid() const noexcept;
  // The ctx's resolved grid follows this editor's zoom while it has focus.
  void reportResolvedGrid();

  // --- display options ---
  bool fold() const noexcept { return fold_; }
  void setFold(bool on);
  bool scaleHighlight() const noexcept { return scale_; }
  void setScaleHighlight(bool on);
  // The virtual fundamental's guide (its line, the harmonics and their labels; the clip box's JI switch; view state).
  bool showHarmonics() const noexcept { return harmonics_; }
  void setShowHarmonics(bool on);
  bool showChance() const noexcept { return chance_; }
  void setShowChance(bool on);
  const std::vector<uint8_t>& highlightScale() const noexcept { return scaleMask_; }

  // --- notes ---
  // Content-y placement of a pitch (rows / interpolation), cached anchor lookups for note-anchored ratios.
  NotePlace place(const tuning::PitchId& p) const;
  std::optional<double> centsOf(const tuning::PitchId& p) const;
  int64_t nearestDegree(double cents) const noexcept;
  // The row a note is edited on (its own row, or the nearest row of an off-grid pitch).
  std::optional<int64_t> rowDegreeOf(const tuning::PitchId& p) const;
  // The topmost note under a device point (the last drawn), or nullptr. `zone`: −1 left edge, +1 right edge, 0 body.
  const doc::Note* noteAt(ui::Vec2 p, int* zone = nullptr) const;
  const doc::Note* findNote(doc::NoteId id) const noexcept;
  // Calls f(const Note&) for every note that may intersect content ticks [from, to) (by chunk; f culls exactly).
  template <class F>
  void forNotesIn(Ticks from, Ticks to, F&& f) const {
    for (const ChunkInfo& c : chunks_) {
      if (c.maxEnd <= from || c.firstStart >= to) continue;
      for (uint32_t i = 0; i < c.count; ++i) {
        const doc::Note& n = c.notes[i];
        if (n.start >= to) break;
        if (n.start + n.length > from) f(n);
      }
    }
  }

  // --- selection (note ids, sorted) ---
  const std::vector<uint32_t>& selection() const noexcept { return sel_; }
  bool selected(doc::NoteId id) const noexcept;
  void select(std::vector<uint32_t> ids);  // sorts, dedups, repaints
  void selectOnly(doc::NoteId id);
  void toggleSelected(doc::NoteId id);
  void clearSelection();
  void selectAll();
  void selectRow(int64_t degree, bool add);
  // Keys of the selected notes as they are now (scans the clip once).
  std::vector<doc::NoteKey> selectionKeys() const;

  // --- edits (each = one dispatch = one undo step) ---
  bool apply(const doc::ops::EditResult& e, std::string_view what);  // dispatch + adopt the selection
  void deleteSelection();
  void duplicateSelection();
  void quantizeSelection();
  void toggleSelectionActive();
  // Up / Down (StepUnit::Grid) and Shift+Up / Down (StepUnit::Equave) through ops::transposeNotes; Ratio notes step
  // on the harmonics of the clip's virtual fundamental (doc::virtualFundamental, kept by the document: T3.6-fix1).
  void stepSelection(int32_t steps, doc::ops::StepUnit unit);
  void nudgeSelection(int32_t cells);
  void copySelection();
  void cutSelection();
  void paste();
  void setInsertTick(std::optional<Ticks> t) noexcept { insertTick_ = t; }
  // The clip's virtual fundamental as a denominator D (fundamental = reference / D; 0 = none), see the rule at its
  // definition (Q40). Ratio Up/Down and vertical drags step on its harmonics; the note area draws it (SPEC §4.3).
  std::optional<tuning::Rational> virtualFundamental() const;
  void addNotes(const std::vector<DrawnNote>& notes);
  // The pitch of a note in a cell of row `degree` (a Degree identity).
  tuning::PitchId degreePitch(int64_t degree) const noexcept;

  // --- drag (preview; committed on release) ---
  DragState drag;
  void commitDrag();
  // At a drag's start (after the selection is final): remember the dragged notes for damageDrag().
  void beginDragTracking();
  // Damages exactly what the preview changed since the last call (or everything for an untracked drag).
  void damageDrag();
  ui::IRect screenRectOf(Ticks start, Ticks length, const tuning::PitchId& pitch) const;
  void cancelDrag();
  // The user abandoned the drag (Esc, capture or window focus lost): cancelDrag, and a rubber band puts back the
  // selection from before its press (T3.x-fix-edit, M3 review L19).
  void abortDrag();
  // Preview geometry of a note while a Move / Copy / Resize drag runs (nullopt: not part of the drag).
  struct Preview {
    Ticks start, length;
    tuning::PitchId pitch;
  };
  std::optional<Preview> preview(const doc::Note& n) const { return previewOf(drag, n); }
  std::optional<Preview> previewOf(const DragState& d, const doc::Note& n) const;
  void updatePreviewPitches();
  float previewValue(const doc::Note& n) const noexcept;  // velocity / chance shown in the lane

  // --- scale layouts, EDO overlay, live morph (T3.12) ---
  // The layout of MOS / Scala rows (Proportional by default; EDO rows are always Uniform). Switching keeps the row
  // at the middle of the note area there (mos-roll.js toggleLayout keeps the centred row).
  RowLayout scaleLayout() const noexcept { return scaleLayout_; }
  void setScaleLayout(RowLayout layout);
  // Background EDO grid (of the octave) over a scale view: 0 = off. Display, and snapping: Cents notes step / move
  // on its lines, and Alt in draw mode draws Cents notes on them.
  uint32_t overlayEdo() const noexcept { return overlay_ ? overlay_->n() : 0; }
  void setOverlayEdo(uint32_t n);
  const tuning::Edo* overlay() const noexcept { return overlay_ ? &*overlay_ : nullptr; }
  // The overlay line nearest `cents` (requires an overlay).
  double nearestOverlayCents(double cents) const noexcept;
  // The pitch a drawn cell gets: a Degree note on its row, or a Cents note on the overlay.
  tuning::PitchId drawnPitch(const DrawnNote& d) const noexcept;

  // Generator morph from the clip box (a MOS clip): every step previews through the context's tuning preview (the
  // rows and Degree notes follow at frame rate, nothing is dispatched); end(true) commits the result as ONE
  // SetClipTuning (one undo step), end(false) restores. Past the current MOS region's ends the generator moves into
  // the adjacent region (tuning::findAdjacentRegion) carrying the mode (transferModeRotation), as the MOS wheel's
  // chart does.
  bool canMorphGenerator() const noexcept;
  void beginGeneratorMorph();
  void morphGenerator(double generatorCents);
  void endGeneratorMorph(bool commit);
  bool morphingGenerator() const noexcept { return morphBase_.has_value(); }
  // How refresh() followed tuning changes: in place from the MosLinearTable interval (linear) or by a full rebuild
  // (buildMOSScale, at region / interval crossings and for anything but a generator change).
  struct MorphStats {
    uint64_t linear = 0;
    uint64_t rebuilt = 0;
  };
  const MorphStats& morphStats() const noexcept { return morphStats_; }
  // Whether the last refresh() that reported a change only moved pitches in place (a morph frame): then only the
  // keys, the note area and the clip box's generator need repainting (not the ruler or the velocity lane).
  bool pitchOnlyChange() const noexcept { return pitchOnlyChange_; }

  // Paint scratch (the note area's z-ordered notes, the ratio labels), reused across frames.
  std::vector<ZNote> zNotes, zSorted;
  std::vector<NoteLabel> noteLabels;

  // --- row labels (T3.19) ---
  RowLabels rowLabels() const noexcept { return rowLabels_; }
  void setRowLabels(RowLabels mode);

  // A handle menus and popovers hold: null once this state is destroyed.
  const std::shared_ptr<RollState*>& self() const noexcept { return self_; }

  // --- identities (T3.19, SPEC §3.1) ---
  // Converts the selected notes (ops::convertNotes; one undo step). False when nothing changed.
  bool convertSelection(tuning::PitchKind to);
  // Gives every selected note this pitch identity (ops::setNotesPitch; one undo step).
  bool setSelectionPitch(const tuning::PitchId& p);
  // The type-in for the selected notes' pitch (a popover with a text field, anchored at the first selected note; Enter
  // applies parsePitch's identity, Esc closes). Returns the text field (nullptr without a selection or overlay).
  ui::TextEdit* openPitchEntry();
  // A typed pitch: "7/4" or "7:4" (a ratio over the reference), "386.3c" / "386.3¢" (cents), "4\22" or "4\22+1"
  // (degree 4, equave +1, of the clip's tuning: the size must match). nullopt when unparsable.
  std::optional<tuning::PitchId> parsePitch(std::string_view text) const;
  // The fundamental lock (ops::lockFundamental: `note` is the index-th harmonic) and its release; one undo step each.
  bool lockFundamentalOn(doc::NoteId note, uint32_t harmonicIndex);
  bool unlockFundamental();
  // Shift-draw: the reference-anchored ratio a drawn note on row `degree` with the pointer at `cents` gets.
  tuning::Rational ratioFor(int64_t degree, double cents) const;

  // --- hover readout (T3.19, M3 review UI §2) ---
  // The pointer at device `p` over the note area (keys = false) or the keys: re-formats the readout when the row or
  // the note under it changed and tells the info view (ctx.infoChanged). clearHover when the pointer leaves.
  void hoverAt(ui::Vec2 p, bool keys);
  void clearHover();
  bool hovering() const noexcept { return hoverRow_ >= 0; }
  const std::string& hoverTitle() const noexcept { return hoverTitle_; }
  const std::string& hoverBody() const noexcept { return hoverBody_; }
  // The readout for a pitch in cents from the reference (row label, cents, Hz, nearest simple ratio and its
  // deviation, the overlay EDO's nearest step), as hoverAt formats it. Also used for the hovered note.
  std::string pitchReadout(double cents, bool nearestRatio = true) const;

  // --- audition ---
  void audition(const tuning::PitchId& p, float velocity, Ticks contentStart, Ticks length);

  // Repaint helpers.
  void markNotesDirty();  // note area + velocity lane
  void markAllDirty();

private:
  void rebuildRows();
  bool morphInPlace(const doc::TuningDef& def);
  void rebuildChunks();
  void rebuildColours();
  void rebuildUsed();
  void buildAnchorIndex() const;

  doc::Snapshot snap_;
  doc::ClipRef ref_{};
  doc::ClipPtr clip_;
  doc::TuningDef tuningDef_{};
  bool tuningValid_ = false;
  tuning::Tuning tuning_{};
  std::string tuningName_;
  RowModel rows_;
  TimeSigMap sigs_;
  TimeSignature sigsFor_{0, 0};
  std::vector<ChunkInfo> chunks_;
  std::unordered_map<const doc::Note*, ChunkInfo> chunkCache_;
  std::vector<int64_t> used_;
  NoteColours colours_{};
  std::vector<uint8_t> scaleMask_;
  mutable std::unordered_map<uint32_t, const doc::Note*> anchorIndex_;
  mutable bool anchorIndexValid_ = false;

  double pxPerTick_ = 0.0;
  double scrollTick_ = 0.0;
  int32_t scrollY_ = 0;
  int32_t rowPxLogical_ = 14;
  float builtScale_ = 0.0f;
  bool fold_ = false;
  bool scale_ = false;
  bool harmonics_ = true;
  bool chance_ = false;
  bool fitted_ = false;  // fitToClip ran for the current clip (needs a laid-out area)

  std::vector<uint32_t> sel_;
  std::vector<uint64_t> selBits_;  // bitmap of sel_ from selMin_ (empty: binary search)
  uint32_t selMin_ = 0;
  std::optional<Ticks> insertTick_;
  // T3.12
  RowLayout scaleLayout_ = RowLayout::Proportional;
  bool keepCentredRow_ = false;
  bool pitchOnlyChange_ = false;  // the last refresh() only retuned in place  // rebuildRows: centre the row that was centred (layout switch)
  std::optional<tuning::Edo> overlay_;
  std::unique_ptr<tuning::MosLinearTable> linear_;  // for the current chain length (built on the first morph frame)
  double linearLow_ = 0.0, linearHigh_ = -1.0;      // the interval the rotated form below is for
  int32_t linearRotation_ = -1;
  std::vector<int32_t> linearK_, linearM_;
  MorphStats morphStats_;
  std::optional<doc::TuningDef> morphBase_;  // the clip's tuning when the generator morph began
  doc::TuningDef morphDef_;
  tuning::MosRegionSet morphRegions_;
  double morphRegionsPeriod_ = 0.0;
  int32_t morphRegionsReps_ = 0, morphRegionsMax_ = 0;
  std::vector<doc::Note> clipboard_;  // starts relative to the earliest copied note
  RowLabels rowLabels_ = RowLabels::Auto;
  int64_t hoverRow_ = -1;   // the row index the readout is for (−1: none)
  uint32_t hoverNote_ = 0;  // the note id it is for (0: none)
  bool hoverKeys_ = false;
  const void* hoverSnap_ = nullptr;  // the snapshot it was formatted for
  std::string hoverTitle_, hoverBody_;
  std::shared_ptr<RollState*> self_;
  mutable const doc::NoteClip* vfClip_ = nullptr;  // virtualFundamental() cache (per clip snapshot)
  mutable std::optional<tuning::Rational> vf_;
};

}  // namespace pq::views::roll
