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
#include <functional>
#include <memory>
#include <optional>
#include <span>
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
#include "pq/views/mos_wheel/mos_wheel.h"
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
  // T7.6 (roll_pen.cpp): a pitch curve of a note (DragState::curve)
  PenDraw,     // Draw Mode on a note: the stroke is the curve
  PenVibrato,  // the same with Alt: vibrato over the dragged span (depth from the stroke's height)
  CurvePoint,  // one breakpoint by (ticks, cents)
  CurveBow,    // the curvature of the segment after a breakpoint
};

enum class BraceEdge : uint8_t { Start, End, Both };

// How a draw gesture picks its notes' pitch identity.
enum class DrawPitch : uint8_t {
  Row,      // a Degree note on the row
  Overlay,  // Alt (T3.12): a Cents note on the background EDO line nearest the pointer
  Ratio,    // Shift (T3.19): a Ratio note on the virtual fundamental's nearest harmonic (no fundamental: the row's
            // pitch snapped to the simplest ratio, ops::simplestRatioNear). With an active JI group (T7.13: "Is
            // Harmonic N") this is the plain draw — a member on the group's nearest harmonic — and Shift draws on
            // the rows.
};

struct DrawnNote {
  Ticks start = 0;
  Ticks length = 0;
  int64_t degree = 0;  // the row (a Degree note), or the nearest row of a Cents / Ratio note
  DrawPitch mode = DrawPitch::Row;
  double cents = 0.0;     // Overlay
  tuning::Rational ratio{1};  // Ratio (to the reference, or to the active JI group's anchor)
};

// A Ratio note's label waiting to be drawn over the notes (T3.19): its box and the colour it sits on.
struct NoteLabel {
  float x0, x1, y0, y1;  // the note's band (device px)
  float yc;              // T7.12: its pitch line at the note's start
  ui::Rgba fill;
  int64_t num, den;   // num 0: the anchor note of a JI group, labelled with its number only
  uint32_t harmonic;  // its harmonic number in its JI group / over the virtual fundamental (0: not one)
};

// T7.6: the curve a pen / breakpoint gesture edits: a note's pitch curve.
struct CurveTarget {
  doc::NoteId note;
  bool valid() const noexcept { return note.valid(); }
  friend bool operator==(const CurveTarget&, const CurveTarget&) noexcept = default;
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
  // T7.6: Move with Shift: free cents instead of rows (the notes become Cents).
  double cents = 0.0;
  // T7.6. ResizeStart / ResizeEnd with Alt: the edge stays and the note glides in from the previous note / out to the
  // next one over |dt| (`bendDelta`: per selected note, sorted by id, how far away that neighbour is in cents; found
  // once, when the gesture first bends).
  bool bend = false;
  bool bendFound = false;
  std::vector<std::pair<uint32_t, double>> bendDelta;
  // Pen and breakpoint gestures: the curve, the point (CurvePoint / CurveBow), the curve as the release will store it,
  // the stroke so far (PenDraw: ticks from the target's start, curve value in cents; sorted by tick), what the pointer
  // holds (`penHold`: pointer cents + penHold − base = the value), the vibrato being painted.
  CurveTarget curve;
  int32_t curveIndex = -1;
  std::vector<doc::Breakpoint> curvePts;
  std::vector<std::pair<Ticks, float>> stroke;
  Ticks strokeLast = 0;
  double penHold = 0.0;
  float bow0 = 0.0f;
  float bx = 0.0f;  // CurvePoint: the handle's x offset from the pointer (it keeps it through the drag)
  double vibDepth = 0.0, vibRate = 0.0;
  bool curveAdded = false;     // CurvePoint: the press added the point (a release without a drag still commits)
  bool deleteOnClick = false;  // PenDraw on a note: a click (no drag) deletes it, as Draw Mode always did
  bool toggleOnClick = false;  // Shift on a selected note: a click deselects it, a drag detunes freely
  ui::Mods pressMods = 0;
};

// A note drawn along a pitch path instead of on a row (T7.4): it has a pitch curve.
struct CurveNote {
  const doc::Note* note;
  Ticks start, length;
  tuning::PitchId pitch;
  bool selected;
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
  int32_t centre = 0;  // T7.12: the pitch itself (the row's pitch line; between rows for an off-grid pitch)
  int8_t offGrid = 0;  // 0 on its row; +1 above / −1 below the nearest row (marked)
  bool ok = false;     // false: the pitch cannot be placed (a Ratio whose anchor note is missing)
  uint8_t z = 0;       // the row's z (scale layouts: notes on larger steps paint over smaller ones)
};

// T7.12: how far above its band a note's label may reach (logical px): the label sits above the line, and the
// note's damage rect (RollState::paintRectOf) covers it.
inline constexpr float kNoteLabelRise = 12.0f;

// T7.12: one note as it is painted: a line along its pitch (roll_paint.cpp drawNoteLine). [y0, y1) is its row band
// (the faint grab area), yc the pitch the line is centred on; a note with a pitch path gets its polyline at paint.
// Also the entry of the z-ordered pass (Proportional layout: rows may overlap).
struct NoteLine {
  float x0, x1, y0, y1, yc;
  ui::Rgba fill, head;  // head: the start cap (the theme's offGrid colour for a pitch between rows)
  uint16_t key = 0;     // z, + 256 when selected (selected notes always on top, as mos-roll.js drawOrder)
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
  // T4.16b (Live's MIDI editor keys): Alt+Left / Right move the selected notes by kFineNudge off the grid; Shift+Left /
  // Right shorten / lengthen them by a grid cell (from the earliest note's end; a note never gets shorter than one
  // cell this way). One undo step each.
  void nudgeSelectionFine(int32_t dir);
  void lengthenSelection(int32_t cells);
  void copySelection();
  void cutSelection();
  void paste();
  void setInsertTick(std::optional<Ticks> t) noexcept { insertTick_ = t; }
  // The clip's virtual fundamental as a denominator D (fundamental = reference / D; 0 = none), see the rule at its
  // definition (Q40). Ratio Up/Down and vertical drags step on its harmonics; the note area draws it (SPEC §4.3).
  std::optional<tuning::Rational> virtualFundamental() const;
  // The fundamental the roll draws and snaps to: `ratio` times the pitch at `baseCents`. With an active JI group
  // (T7.13): its anchor note where it sounds now and the group's derived fundamental 1 / (index · m)
  // (doc::groupFundamental), so the guides follow the anchor. Else the clip's reference (anchor invalid, 0 cents) and
  // the virtual fundamental. nullopt: none.
  struct Harmonics {
    doc::NoteId anchor;
    double baseCents = 0.0;
    tuning::Rational ratio{1};
  };
  std::optional<Harmonics> harmonics() const;
  // T7.13 (the seam for the group UI): the JI group the guides, the plain draw and "Remove JI Group" act on — the
  // group of the first selected note (in clip order) that is an anchor or a member, else the group that was active
  // last (so a click on empty space keeps it), else the clip's newest group; nullptr: none. Cached per clip snapshot
  // and selection.
  const doc::JiGroup* activeJiGroup() const;
  // A note's harmonic number: in its JI group (its anchor, a numbered member), else — a reference-anchored Ratio —
  // over the virtual fundamental. nullopt: none. O(log members) after the first call for a clip snapshot.
  std::optional<uint64_t> harmonicOf(const doc::Note& n) const;
  void addNotes(const std::vector<DrawnNote>& notes);
  // How a draw gesture with these modifiers picks its pitch (with an active JI group the plain draw is Ratio and
  // Shift the rows).
  DrawPitch drawPitchFor(ui::Mods m) const noexcept;
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
  // T7.12: everything the note paints: that rect and the room above its band its label may take (damage).
  ui::IRect paintRectOf(Ticks start, Ticks length, const tuning::PitchId& pitch) const;
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

  // Generator morph from the clip box (a MOS clip; TU.1): every step is applied to the document inside one store
  // gesture (TuningEdit; TU.3: SetTuningParam / SetTuningDef of the clip's named tuning), so sounding notes retune during the drag and the
  // rows and Degree notes follow at frame rate (refresh: in place within an interval); end(true) commits the gesture
  // (ONE undo step), end(false) cancels it (the document as before). Past the current MOS region's ends the generator moves into
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
  std::vector<NoteLine> zNotes, zSorted;
  std::vector<NoteLabel> noteLabels;
  std::vector<CurveNote> curveNotes;  // T7.4: the visible notes drawn along a path
  // Wall-clock time of the note area's last paint (tests).
  struct PaintStats {
    double notesMs = 0.0;
  };
  PaintStats paintStats;
  std::vector<ui::Vec2> linePts;      // T7.4: one polyline being built

  // --- declaring JI (T7.13; roll_ji.cpp). The group UI (frames, Tune to JI + chooser, warp markers, walk, the
  // proportion tool, Add Note at Ratio) is T7.14's; these are what the roll keeps meanwhile. ---
  // "8:9:10:12:15" (spaces allowed): strictly ascending whole terms. An error says what is wrong.
  static Result<std::vector<int64_t>> parseProportion(std::string_view text);
  // "Is Harmonic N": `note` becomes the frozen anchor of a JI group with index n (ops::makeJiGroup; a note that
  // already anchors one: its index). Its pitch does not change; it sits exactly on guide n. Notes drawn afterwards
  // are exact ratios to it, and the numbering is derived (doc::groupHarmonics). The selection is kept.
  bool declareHarmonic(doc::NoteId note, uint32_t n, std::string* error = nullptr);
  // Removes the active JI group (ops::removeJiGroup: its members keep sounding where they are). False: none.
  bool removeActiveJiGroup();
  // An inline field under `anchor`: a title line, a text field (initial text selected) and an error line. Enter calls
  // `commit(text, error)`: true closes the field, false shows `error` and keeps it open. Esc closes.
  ui::TextEdit* openJiField(std::string_view name, std::string_view title, std::string_view info, std::string_view initial,
                            ui::IRect anchor, std::function<bool(RollState&, const std::string&, std::string&)> commit);
  ui::TextEdit* openHarmonicEntry(doc::NoteId note);  // "Is Harmonic…"
  // A point on a note drawn along a path, `frac` of the way through it (tests, anchors); nullopt: not on a path.
  std::optional<ui::Vec2> pathPoint(const doc::Note& n, double frac) const;
  // The device box of an on-path note (its path's extent plus its thickness).
  ui::IRect pathBox(const doc::Note& n, Ticks start, Ticks length, const tuning::PitchId& pitch) const;
  // Half the drawn thickness of a path note (device px).
  float pathHalfThickness() const noexcept;

  // --- bends, pen modes, breakpoints, vibrato (T7.6; roll_pen.cpp) ---
  // A note's pitch curve is baked breakpoints (NoteCurves::pitch; what plays). Two parts of it are recognised by
  // their form, so every gesture finds them again without extra stored state:
  //   lead-in   the curve from tick 0 (value ≠ 0) to its first point of value 0: the note glides IN to its own pitch;
  //   lead-out  from its last point of value 0 to its last point (value ≠ 0): it glides OUT.
  // Alt + a note's left / right edge writes them (bendEdit): the value is the neighbour's pitch minus the note's
  // (bendNeighbour: the note ending nearest before this one's start / starting nearest after its end, nearest in pitch
  // when several), computed when the gesture is made and not followed afterwards (Q?-T7.6-a).
  // The points of a curve as shown: a gesture's preview, else the document's. Empty: none.
  std::span<const doc::Breakpoint> curvePoints(const CurveTarget& t) const;
  // The pitch the curve is added to, `offset` ticks into its note (its identity's pitch), in cents. nullopt:
  // unresolvable.
  std::optional<double> curveBase(const CurveTarget& t, double offset) const;
  // The target's start and length in content ticks (false: no such note).
  bool curveSpan(const CurveTarget& t, Ticks& start, Ticks& length) const;
  // Breakpoint handles (device px): a dot per point, a diamond in the middle of a segment wide enough to bend. Shown
  // for the selected notes and, in Draw Mode, the note under the pointer; a curve of more
  // than kMaxCurveHandles points (a free-drawn stroke, a vibrato) shows none.
  struct CurveHandle {
    int32_t index = -1;  // the point, or the segment's first point (mid)
    bool mid = false;
    ui::Vec2 pos{};
    // −1 / +1: the point lies on its note's start / end. The note's edge (a resize, an Alt bend) is grabbed first
    // there, so this handle's hit box reaches further into the note.
    int8_t end = 0;
  };
  void curveHandles(const CurveTarget& t, std::vector<CurveHandle>& out) const;
  struct CurveHit {
    CurveTarget target;
    int32_t index = -1;
    bool mid = false;
    bool valid() const noexcept { return target.valid() && index >= 0; }
  };
  CurveHit curveHandleAt(ui::Vec2 p) const;
  // The curves whose handles are shown now (see curveHandles).
  void shownCurves(std::vector<CurveTarget>& out) const;
  // The edit an Alt + edge drag in state `d` commits (one SetNoteCurve per selected note that has such a neighbour).
  doc::ops::EditResult bendEdit(DragState& d) const;
  void updateBendPreview();
  // The neighbour's pitch minus the note's own, in cents (edge −1: the note before, +1: the note after). nullopt: none.
  std::optional<double> bendNeighbour(const doc::Note& n, int edge) const;
  // The length of a note's lead-in / lead-out (0: none) and how far it bends (cents).
  Ticks bendLength(doc::NoteId note, int edge, float* cents = nullptr) const;
  // Ctrl+G: every selected note glides out into the next selected one over one grid step. One undo step.
  bool glideSelection();
  // The lead-in (edge −1) / lead-out (+1) of the selected notes redrawn Straight or as an S-curve (same length and depth).
  bool setBendShape(int edge, doc::GlideShape::Kind kind);
  bool bendIsStraight(doc::NoteId note, int edge) const;
  // Removes the selected notes' pitch curves (one undo step).
  bool clearBends();
  // Vibrato: a sine of `depth` cents (peak) at `rateHz`, whole half cycles, baked into breakpoints. On the selected
  // notes it fills each note between its lead-in and lead-out. defaultVibratoRate: one cycle per grid cell, folded by
  // octaves into 4 … 8 Hz at the clip's tempo (6 Hz without a grid).
  bool addVibratoToSelection(double depthCents, double rateHz);
  double defaultVibratoRate(Ticks at) const;
  // Ticks per second at a content tick where the clip plays it (120 bpm without a timeline).
  double ticksPerSecond(Ticks contentTick) const;
  double vibratoDepth() const noexcept { return vibratoDepth_; }
  // The vibrato in a curve around `offset` (alternating, evenly spaced swings of one size), as painted: where it
  // starts and ends (ticks from the target's start), its depth and its rate. nullopt: none there.
  struct Vibrato {
    Ticks from = 0, to = 0;
    double depth = 0.0, rateHz = 0.0;
  };
  std::optional<Vibrato> vibratoAt(const CurveTarget& t, Ticks offset) const;
  // Rewrites that vibrato with another depth / rate over the same span (one undo step).
  bool setVibrato(const CurveTarget& t, const Vibrato& was, double depthCents, double rateHz);
  static std::string vibratoText(double depthCents, double rateHz);  // "Vibrato 30 ¢ · 6 Hz"
  // "30 6", "30 ¢ · 6 Hz": depth 0 … 1200 ¢, rate 0.1 … 30 Hz (the rate may be left out: `rateHz` is kept).
  static bool parseVibrato(std::string_view text, double& depthCents, double& rateHz, std::string& error);
  ui::TextEdit* openVibratoEntry();                                       // "Add Vibrato…" (the selection)
  ui::TextEdit* openVibratoEdit(const CurveTarget& t, const Vibrato& v);  // "Edit Vibrato…"
  // Pen gestures (the note area calls them; each previews the curve the release stores).
  void beginPen(const CurveTarget& t, ui::Vec2 p, ui::Mods mods, bool vibrato);
  void penTo(ui::Vec2 p, ui::Mods mods);
  // Ctrl+click in Draw Mode: a breakpoint on the curve at the pointer (the press then drags it).
  bool beginCurvePoint(const CurveTarget& t, ui::Vec2 p);
  void beginCurveHandle(const CurveHit& h, ui::Vec2 p);
  void curveHandleTo(ui::Vec2 p, ui::Mods mods);
  bool deleteCurvePoint(const CurveHit& h);
  // Stores a note's curve (SetNoteCurve), one undo step; the selection is kept.
  bool commitCurve(const CurveTarget& t, std::vector<doc::Breakpoint> pts, std::string_view what);
  // A pitch in cents snapped for a breakpoint drag: the nearest row, harmonic guide (when shown) or `own` (the pitch
  // the curve is added to: value 0).
  double snapCurveCents(double cents, double own) const;
  // The pitch under device y, not rounded (the inverse of yOfCents).
  double centsAtScreenY(float y) const noexcept;
  // A resized note's curve: its lead-in stays on its start and its lead-out on its end, what lies between keeps its
  // place in time (cut where the note no longer reaches). `startDelta`: how far the start moved.
  static std::vector<doc::Breakpoint> retimeCurve(std::span<const doc::Breakpoint> pts, Ticks oldLength, Ticks newLength, Ticks startDelta);
  // A resize of the selected notes (ops::resizeNotes) with their curves retimed in the same undo step.
  doc::ops::EditResult resizeEdit(doc::ops::NoteEdge edge, Ticks delta) const;
  bool hasCurvePreview() const noexcept { return !curvePreview_.empty(); }
  // A Shift + body drag: the selected notes `cents` away as Cents notes, `dt` later (one undo step).
  doc::ops::EditResult detuneEdit(Ticks dt, double cents) const;
  inline static constexpr size_t kMaxCurveHandles = 24;
  inline static constexpr size_t kMaxPenPoints = 512;  // a stroke's points after thinning
  inline static constexpr size_t kMaxVibratoPoints = 4096;
  void clearCurvePreview();

  // --- pitch paths (T7.4) ---
  // Device y of a pitch in cents, not rounded.
  float yOfCents(double cents) const noexcept;
  // Whether a note is drawn along a path: it has a pitch curve. O(1) for a clip without curves.
  bool onPath(const doc::Note& n, const tuning::PitchId& pitch) const noexcept;
  // The pitch in cents of a note `offset` ticks after its start, when it starts at `start` with identity `pitch` (a
  // drag preview may differ from the note): the identity's pitch plus the note's own pitch curve. nullopt:
  // unresolvable. The seam hit-testing and curve editing go along.
  std::optional<double> pathCents(const doc::Note& n, Ticks start, const tuning::PitchId& pitch, double offset) const;
  // The same without the note's own curve (T7.6: what a curve's values are added to).
  std::optional<double> pathBaseCents(Ticks start, const tuning::PitchId& pitch, double offset) const;

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

  // --- hover affordance (T4.16b) ---
  // The note part a press at the pointer would grab (Live brightens the note and shows the edge it would trim):
  // note id (0: none) and zone (−1 start, +1 end, 0 body). The note area draws it over the notes; a change damages
  // only the two notes' rects. Not shown while a gesture runs.
  struct HoverPart {
    uint32_t note = 0;
    int8_t zone = 0;
    bool operator==(const HoverPart&) const = default;
  };
  const HoverPart& hoverPart() const noexcept { return hoverPart_; }
  void setHoverPart(HoverPart h);
  // From the pointer at device `p` over the note area (Draw Mode: the body only, a press deletes the note).
  void hoverPartAt(ui::Vec2 p);
  // The pointer left the note area.
  void clearHoverPart();
  // After a document change moved the notes: recompute at the last pointer position (no damage: everything repaints).
  void refreshHoverPart();

  // --- audition ---
  void audition(const tuning::PitchId& p, float velocity, Ticks contentStart, Ticks length);
  std::optional<int64_t> auditionDegree(const tuning::PitchId& p) const;  // TU.2 (roll_state.cpp)
  // AU.voices: the press that auditioned ended (pointer up, capture lost): its notes stop sounding (released now, at
  // most their own length). Only after a press that auditioned (auditionHeld).
  void releaseAudition();
  bool auditionHeld = false;

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
  std::unique_ptr<TuningEdit> morphEdit_;    // the morph's live edit (TU.1)
  tuning::MosRegionSet morphRegions_;
  double morphRegionsPeriod_ = 0.0;
  int32_t morphRegionsReps_ = 0, morphRegionsMax_ = 0;
  std::vector<doc::Note> clipboard_;  // starts relative to the earliest copied note
  std::vector<doc::NoteCurves> clipboardCurves_;  // T7.4: the copied notes' pitch curves (keyed by the source ids)
  std::vector<doc::JiGroup> clipboardGroups_;     // T7.13: the JI groups whose anchor was copied (source ids)
  std::vector<doc::NoteWritten> clipboardWritten_;  // and the copied members' written identities
  // T7.6: the curves a gesture in progress shows instead of the document's (sorted by note id; an empty curve = the
  // note shown without one), and the depth of the last vibrato painted (the default of the next).
  std::vector<doc::NoteCurves> curvePreview_;
  double vibratoDepth_ = 30.0;
  void setCurvePreview(const CurveTarget& t, const std::vector<doc::Breakpoint>& pts);
  mutable std::vector<CurveHandle> handleScratch_;
  mutable std::vector<CurveTarget> targetScratch_;
  RowLabels rowLabels_ = RowLabels::Auto;
  int64_t hoverRow_ = -1;   // the row index the readout is for (−1: none)
  uint32_t hoverNote_ = 0;  // the note id it is for (0: none)
  bool hoverKeys_ = false;
  const void* hoverSnap_ = nullptr;  // the snapshot it was formatted for
  std::string hoverTitle_, hoverBody_;
  HoverPart hoverPart_;
  std::optional<ui::Vec2> hoverPartPos_;  // the pointer's last position over the note area (none: outside)
  std::shared_ptr<RollState*> self_;
  mutable const doc::NoteClip* vfClip_ = nullptr;  // virtualFundamental() cache (per clip snapshot)
  mutable std::optional<tuning::Rational> vf_;
  // T7.13: the same snapshot's JI group numbers ((note id, its number), sorted by id; the anchors' ids, sorted), and
  // the active group for the current selection (activeJiGroup()).
  mutable std::vector<std::pair<uint32_t, int64_t>> groupNumbers_;
  mutable std::vector<uint32_t> groupAnchors_;
  mutable bool activeGroupValid_ = false;
  mutable const doc::JiGroup* activeGroup_ = nullptr;
  mutable doc::JiGroupId lastActiveGroup_;
  mutable const doc::Note* activeAnchor_ = nullptr;
  mutable tuning::Rational activeFundamental_{1};
};

}  // namespace pq::views::roll
