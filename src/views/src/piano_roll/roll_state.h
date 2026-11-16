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
#include "pq/document/pitch_resolve.h"
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
  RulerZoom,    // ruler's numbers row: vertical drag zooms, horizontal drag scrolls
  RulerScrub,   // the ruler's scrub strip (bug-paused-playhead): plays from the press, the transport follows a drag
  KeysZoom,     // keys: horizontal drag zooms the rows
  // T7.6 (roll_pen.cpp): a pitch curve of a note (DragState::curve)
  PenDraw,     // Draw Mode on a note: the stroke is the curve
  PenVibrato,  // the same with Alt: vibrato over the dragged span (depth from the stroke's height)
  CurvePoint,  // one breakpoint by (ticks, cents)
  CurveBow,    // the curvature of the segment after a breakpoint
  // T7.14 (roll_vibrato.cpp, roll_groups.cpp)
  Vibrato,        // the ~ grip of a note: up / down = depth, left / right = rate
  VibratoCorner,  // one corner of a vibrato's envelope, sideways (DragState::corner)
  WarpNew,        // from a frame's corner grip inward: a new warp of the group at the pointer
  WarpMove,       // a warp marker, sideways
  WarpGlide,      // a warp's glide grip: the glide's length
  ProportionBox,  // the proportion tool (J): the box the typed proportion fills
};

enum class BraceEdge : uint8_t { Start, End, Both };

// How a draw gesture picks its notes' pitch identity.
enum class DrawPitch : uint8_t {
  Row,      // a Degree note on the row
  Overlay,  // Alt (T3.12): a Cents note on the background EDO line nearest the pointer
  Ratio,    // Shift (T3.19): a Ratio note on the virtual fundamental's nearest harmonic (no fundamental: the row's
            // pitch snapped to the simplest ratio, ops::simplestRatioNear). Where a note drawn JOINS a JI group
            // (T7.16a, M7 review H1 / H3: inside the group's time span, the clip box's Harmonics switch on:
            // RollState::captureGroupAt) this is the plain draw — a member on the group's nearest harmonic, with Alt
            // of the next finer series (M5) — and Shift draws on the rows.
};

struct DrawnNote {
  Ticks start = 0;
  Ticks length = 0;
  int64_t degree = 0;  // the row (a Degree note), or the nearest row of a Cents / Ratio note
  DrawPitch mode = DrawPitch::Row;
  double cents = 0.0;     // Overlay
  tuning::Rational ratio{1};  // Ratio (to the reference, or to `anchor`)
  doc::NoteId anchor;         // Ratio: the anchor of the JI group the note joins (invalid: the reference)
};

// A Ratio note's label waiting to be drawn over the notes (T3.19): its box and the colour it sits on.
struct NoteLabel {
  float x0, x1, y0, y1;  // the note's band (device px)
  float yc;              // T7.12: its pitch line at the note's start
  ui::Rgba fill;
  int64_t num, den;   // num 0: the anchor note of a JI group, labelled with its number only
  uint32_t harmonic;  // its harmonic number in its JI group / over the virtual fundamental (0: not one)
  uint8_t group = 0;  // T7.14: 1 a numbered member of a JI group, 2 its anchor: the number alone, in the group's colours
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
  Ticks scrubSong = -1;        // RulerScrub: the song tick it last played from
  std::vector<uint32_t> base;  // Rubber: the selection the band adds to
  std::vector<uint32_t> pressSelection;  // Rubber: the selection before the press (abortDrag restores it)
  // Damage tracking (Move / Copy / Resize / Velocity / Draw): the dragged notes as they were at the press, and the
  // screen box the preview covered last frame. Off for large selections (the whole area repaints).
  bool trackDamage = false;
  std::vector<doc::Note> tracked;
  ui::IRect damage{};
  // T7.6: Move with Shift: free cents instead of rows (the notes become Cents).
  double cents = 0.0;
  // T7.6 / T7.14. ResizeStart / ResizeEnd with Alt: the edge stays and the note glides in from the previous note / out
  // to the next one over |dt| (`bendPicks`: per selected note, sorted by id, the neighbour it bends from / to; found
  // once, when the gesture first bends; the neighbours are highlighted while it runs).
  // A selected note with no such neighbour (`bendFree`, sorted) bends in from / out to the pitch the pointer is dragged
  // to instead (`bendCents` from its own pitch at that edge; snapped to the rows, the harmonic guides and its own pitch,
  // Shift: free) — written into its free curve.
  bool bend = false;
  bool bendFound = false;
  std::vector<std::pair<uint32_t, doc::NoteId>> bendPicks;
  std::vector<uint32_t> bendFree;
  double bendCents = 0.0;
  float travel = 0.0f;  // the farthest the pointer got from the press (device px): a pen stroke shorter than
                        // kMinPenStroke is a click (M7 review M7)
  // T7.14 vibrato grips: the notes the drag changes with what they had at the press (sorted by id), the corner
  // (0 onset, 1 end of the fade-in, 2 start of the fade-out, 3 end), what the status line shows.
  struct VibratoTarget {
    doc::NoteId note;
    doc::NoteVibrato base;
    Ticks length = 1;
  };
  std::vector<VibratoTarget> vibrato;
  int corner = -1;
  // T7.14 warps: the group, the warp's index in `warps` (the list the release stores; group ticks), the group's start.
  doc::JiGroupId group;
  int32_t warpIndex = -1;
  std::vector<doc::JiWarp> warps;
  Ticks groupStart = 0, groupEnd = 0;  // the anchor's start (where group ticks count from), the group's end
  Ticks groupFirst = 0;                 // the group's earliest start
  bool fromLeft = false;  // WarpNew: from the left corner
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
  float grabY = 0.0f;  // Move: the pressed note's line minus the pointer's y at the press (T7.14: harmonic snapping)
  double vibDepth = 0.0, vibRate = 0.0;
  bool curveAdded = false;     // CurvePoint: the press added the point (a release without a drag still commits)
  bool deleteOnClick = false;  // PenDraw on a note: a click (no drag) deletes it, as Draw Mode always did
  bool toggleOnClick = false;  // Shift on a selected note: a click deselects it, a drag detunes freely
  ui::Mods pressMods = 0;
  // T7.16a (M7 review M14): a Move with pitch steps moves a note other notes are anchored to: those are drawn
  // following its preview (RollState::centsOf), so the whole area repaints.
  bool movesAnchors = false;
  // bug-ji-input (Move): the selected group anchors whose whole group is selected - their members follow them
  // (movesAnchors) - and those moving ALONE (ops::moveNotes re-anchors their group onto a staying member; in a tuned
  // group they snap to the group's harmonics and are previewed there). Sorted note ids.
  std::vector<uint32_t> followAnchors, aloneAnchors;
};

// A note drawn along a pitch path instead of on a row (T7.4): it has a pitch curve, a bend, a vibrato or a warp.
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
  ui::Rgba fill;
  uint16_t key = 0;  // z, + 256 when selected (selected notes always on top, as mos-roll.js drawOrder)
};

// T7.14: what the roll knows of one JI group per clip snapshot (RollState::groupInfos): its numbering and its notes.
struct GroupInfo {
  const doc::JiGroup* group = nullptr;
  const doc::Note* anchor = nullptr;
  int64_t anchorNumber = 1;
  std::vector<int64_t> proportion;        // distinct numbers, ascending
  std::vector<const doc::Note*> notes;    // the anchor and the members, in clip order
  Ticks start = 0, end = 0;               // doc::groupSpan
  Ticks origin = 0;                       // the anchor's start: where the warps count from (T7.16a, M7 review M7)
  bool tuned = false;                     // a member remembers what it was written as: warps and other readings apply
  bool inJi = true;                       // T7.16a: the group's state (doc::JiGroup::tuned; false: Back to Tuning)
  std::string proportionText;             // "4:5:6:7" (long ones elided)
  std::string header;                     // that, with warps "12-EDO \u2192 4:5:6", Back to Tuning "12-EDO (4:5:6)"
};

// T7.14: where a group's frame and its parts are on screen (device px). Corner grips, markers and glide grips are
// centred on the frame's bottom border; the header sits above the frame, the warp labels under it.
struct GroupFrame {
  ui::Rect box{};
  ui::Rect header{};      // the whole header text
  ui::Rect proportion{};  // the proportion inside the header (a click opens the chooser of a tuned group)
};
enum class GroupPart : uint8_t { None, Header, Proportion, LeftGrip, RightGrip, Marker, GlideGrip };
struct GroupHit {
  GroupPart part = GroupPart::None;
  doc::JiGroupId group;
  int32_t warp = -1;
  bool valid() const noexcept { return part != GroupPart::None; }
};

class RollState {
public:
  explicit RollState(ViewContext& ctx);
  ~RollState();
  RollState(const RollState&) = delete;
  RollState& operator=(const RollState&) = delete;

  ViewContext& ctx;

  // bug-roll-playhead: the content tick the transport plays now, as the roll draws it; stopped, where it stopped /
  // was placed (bug-paused-playhead). nullopt: outside the clip, or the view hidden. ClipView keeps it (roll_playhead.cpp).
  std::optional<Ticks> playhead;

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
  // The edges are the last kEdgeGrab px of a note wide enough; `alt` (T7.16a, M7 review M13: Alt held, an edge
  // bends) widens them to max(kAltEdgeGrab px, a quarter of the note) for any width, the nearer edge winning.
  const doc::Note* noteAt(ui::Vec2 p, int* zone = nullptr, bool alt = false) const;
  inline static constexpr float kAltEdgeGrab = 6.0f;  // logical px
  // O(1) after the first lookup of an id in a clip snapshot (the first is a scan of the clip).
  const doc::Note* findNote(doc::NoteId id) const;
  // The selected notes (≤ 64) looked up in ONE pass over the clip, for the per-frame code that asks for each.
  void cacheSelectedNotes() const;
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
  // The fundamental the roll draws and snaps to: `ratio` times the pitch at `baseCents`. With a guide group (T7.13 /
  // T7.16a: guideGroup()): its anchor note where it sounds now and the group's derived fundamental 1 / (index · m)
  // (doc::groupFundamental) — with Alt held over it the next finer series, 1 / (2 · index · m) (M5) — drawn over the
  // group's time span only (`from` … `to`), where a drawn note joins it. Else the clip's reference (anchor invalid, 0
  // cents) and the virtual fundamental, across the roll. nullopt: none.
  struct Harmonics {
    doc::NoteId anchor;
    double baseCents = 0.0;
    tuning::Rational ratio{1};
    Ticks from = 0, to = 0;  // a group's: the content ticks the guides cover (from = to: everywhere)
  };
  std::optional<Harmonics> harmonics() const;
  // The same for one group (`finer`: the next finer series).
  std::optional<Harmonics> groupHarmonicsOf(const doc::JiGroup& g, bool finer = false) const;
  // The reference's virtual fundamental (Shift-draw away from any group).
  std::optional<Harmonics> referenceHarmonics() const;
  // T7.16a (M7 review H1 / H3): the JI group a note drawn at content tick `t` (pointer at device y `y`) joins: with
  // the clip box's Harmonics switch on, a group whose time span [start − kGroupCaptureTicks, end) holds t — the
  // active group first, else the one whose frame holds y, else the newest. nullptr: the note goes on the rows.
  const doc::JiGroup* captureGroupAt(Ticks t, float y) const;
  inline static constexpr Ticks kGroupCaptureTicks = kPpq / 8;
  // The group whose guides show: the one under the pointer (captureGroupAt at its last position), else the active
  // group. nullptr: none (or the switch is off).
  const doc::JiGroup* guideGroup() const;
  // Alt held over a group (the finer series shows and the plain draw lands on it, M5).
  bool finerGuides() const noexcept { return finer_; }
  void setFinerGuides(bool on);
  // T7.16a (M7 review M10): a toolbar control took the press: the keys go back to the note area.
  void focusNotes();
  // The modifiers of the last pointer event (or Alt key) over the note area: the cursor, the hover's edge zones and
  // the finer series follow Alt (M13, M5).
  ui::Mods pointerMods() const noexcept { return pointerMods_; }
  void setPointerMods(ui::Mods m);
  // The JI group the walk, "Remove JI Group" and the corner grips act on, whose guides show when no group is under the
  // pointer (T7.14): the group of the first selected note (in clip order) that is an anchor or a member; with nothing
  // selected the group that was active last (a click on empty space or a deleted note keeps it). Selecting notes of
  // no group, or Esc (forgetActiveGroup), puts it away. T7.16a: it no longer decides where drawn notes go — a note
  // joins a group only inside that group's time span (captureGroupAt). nullptr: none. Cached per clip snapshot and
  // selection.
  const doc::JiGroup* activeJiGroup() const;
  // Esc: no group stays active.
  void forgetActiveGroup() noexcept;
  // A note's harmonic number: in its JI group (its anchor, a numbered member), else — a reference-anchored Ratio —
  // over the virtual fundamental. nullopt: none. O(log members) after the first call for a clip snapshot.
  std::optional<uint64_t> harmonicOf(const doc::Note& n) const;
  // T7.14: 2 the anchor of a JI group, 1 a member of one, 0 neither. O(log groups).
  int groupRole(const doc::Note& n) const;
  // T7.16a: the note is of a group Back to Tuning (it plays and is labelled as written). O(log groups).
  bool untunedMember(const doc::Note& n) const;
  void addNotes(const std::vector<DrawnNote>& notes);
  // How a draw gesture with these modifiers at content tick `t`, device y `y` picks its pitch (where the note joins a
  // group, captureGroupAt, the plain draw is Ratio — Alt too, on the finer series — and Shift the rows).
  DrawPitch drawPitchFor(ui::Mods m, Ticks t, float y) const;
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

  // --- JI groups (T7.13 / T7.14; roll_ji.cpp, roll_groups.cpp) ---
  // "8:9:10:12:15" (spaces allowed): strictly ascending whole terms. An error says what is wrong.
  static Result<std::vector<int64_t>> parseProportion(std::string_view text);
  // "Is Harmonic N": `note` becomes the frozen anchor of a JI group with index n (ops::makeJiGroup; a note that
  // already anchors one: its index). Its pitch does not change; it sits exactly on guide n. A note that already is a
  // MEMBER of a group becomes that group's anchor with index n (ops::reanchorJiGroup, Q297 b): every note keeps its
  // pitch, the numbers re-derive. The selection is kept.
  bool declareHarmonic(doc::NoteId note, uint32_t n, std::string* error = nullptr);
  // Removes the active JI group (ops::removeJiGroup: its members keep sounding where they are). False: none.
  bool removeActiveJiGroup();
  // An inline field under `anchor`: a title line, a text field (initial text selected) and an error line. Enter calls
  // `commit(text, error)`: true closes the field, false shows `error` and keeps it open. Esc closes.
  ui::TextEdit* openJiField(std::string_view name, std::string_view title, std::string_view info, std::string_view initial,
                            ui::IRect anchor, std::function<bool(RollState&, const std::string&, std::string&)> commit);
  ui::TextEdit* openHarmonicEntry(doc::NoteId note);  // "Is Harmonic…"
  // The clip's groups whose anchor is there, by group id (per clip snapshot; O(groups · notes) once).
  const std::vector<GroupInfo>& groupInfos() const;
  const GroupInfo* groupInfo(doc::JiGroupId id) const;
  // The tuning's short name in a header / under a warp marker: "12-EDO", else the named tuning's name.
  std::string tuningLabel() const;
  // Where the group's frame is now (a warp drag's preview included). Empty box: its notes cannot be placed.
  GroupFrame groupFrame(const GroupInfo& g) const;
  // The group's warps as shown (a gesture's preview, else the document's).
  std::span<const doc::JiWarp> shownWarps(const GroupInfo& g) const;
  // The device x of a warp's marker and of its glide grip (the glide's end, kept kWarpGripGap from the marker so
  // both can be grabbed), the y of the frame's bottom border they are centred on.
  float warpX(const GroupInfo& g, const doc::JiWarp& w) const;
  float warpGlideX(const GroupInfo& g, const doc::JiWarp& w) const;
  // The group part under a device point: the header, a corner grip (tuned groups), a warp marker or glide grip.
  GroupHit groupPartAt(ui::Vec2 p) const;
  // Whether the frame's corner grips are shown: the group is tuned and active (or hovered).
  bool gripsShown(const GroupInfo& g) const;
  // Selects the group's notes (a click on its header).
  void selectGroup(doc::JiGroupId id);
  // "Tune to JI" (Ctrl+Shift+J), T7.16a: the selection split into chords (notes overlapping in time); when every chord
  // is (in) a group in JI, Back to Tuning for those groups (ops::setJiGroupsTuned: they play as written and keep their
  // reading and warps); else one chord becomes one group read as the best candidate (ops::tuneToJi), several chords
  // or a group Back to Tuning go through ops::tuneChordsToJi. A selection with no group never untunes another group.
  // The same notes before and after: the selection is kept. One undo step. `chooser`: open the candidate list
  // afterwards when one chord was tuned and it has several readings.
  bool toggleTuneToJi(bool chooser = true);
  // The readings of a tuned group as written (ops::tuneToJiCandidates of its notes).
  std::vector<doc::ops::JiCandidate> groupCandidates(doc::JiGroupId id) const;
  // Re-tunes the group to `terms` (one of its candidates). One undo step.
  bool retuneGroup(doc::JiGroupId id, const std::vector<int64_t>& terms);
  // The chooser: a list of the group's candidates (proportion, total error) under its header; the current one is
  // ticked. Click or Up / Down + Enter picks; Esc keeps. nullptr: nothing to choose from.
  ui::Menu* openJiChooser(doc::JiGroupId id);
  // Walk (Alt+Up / Alt+Down): every number of the active group ± 1 with the anchor's pitch fixed (ops::walkJiGroup).
  bool walkActiveGroup(int32_t steps);
  // Warps (group ticks). insertWarpAt: the menu's "Back to Tuning Here" / "Into JI Here" at a content tick (the next
  // transition moves there, or a new one is added). setWarpShape / removeWarp: a marker's menu, Delete.
  bool insertWarpAt(doc::JiGroupId id, Ticks contentTick);
  bool setWarpShape(doc::JiGroupId id, int32_t index, doc::GlideShape::Kind kind);
  bool removeWarp(doc::JiGroupId id, int32_t index);
  // How far a member with a written identity is from JI at a content tick of the group: 0 JI, 1 written.
  double warpStateAt(const GroupInfo& g, Ticks contentTick) const;
  // The selected warp marker (a click on it; Delete removes it). Cleared by any note selection.
  struct WarpSel {
    doc::JiGroupId group;
    int32_t index = -1;
    bool valid() const noexcept { return group.valid() && index >= 0; }
  };
  const WarpSel& selectedWarp() const noexcept { return warpSel_; }
  void selectWarp(WarpSel w);
  bool removeSelectedWarp();
  // Warp gestures (the note area calls them).
  void beginWarpDrag(const GroupHit& h, ui::Vec2 p);
  void warpDragTo(ui::Vec2 p, ui::Mods mods);
  bool commitWarpDrag(DragState& d);
  // The proportion tool: J arms it for one box drag on empty space (Esc disarms); the release opens the field.
  bool proportionArmed() const noexcept { return proportionArmed_; }
  void armProportionTool(bool on);
  // The field: "8:9:10:12:15" + Enter puts regular notes over [start, start + length) as one group, the lowest the
  // anchor on row `bottomDegree` (ops::proportionNotes).
  ui::TextEdit* openProportionEntry(Ticks start, Ticks length, int64_t bottomDegree, ui::IRect anchor);
  bool insertProportion(Ticks start, Ticks length, int64_t bottomDegree, const std::vector<int64_t>& terms, std::string* error = nullptr);
  // "Add Note at Ratio…" on a note of a group: "3/2" or "7:4" (a ratio to THAT note) or "5" (harmonic 5 of the
  // group) adds a member at the note's time and length (ops::addGroupNote).
  ui::TextEdit* openRatioEntry(doc::NoteId note);
  bool addNoteAtRatio(doc::NoteId note, std::string_view text, std::string* error = nullptr);
  // A member dragged vertically: the harmonic steps that bring the pressed note's number to the harmonic nearest
  // the pointer's pitch. nullopt: the pressed note is no numbered member.
  std::optional<int32_t> harmonicDragSteps(const doc::Note& pressed, float y) const;
  // bug-ji-input: every note of the group `anchor` anchors (the anchor and its members) is selected: a move moves the
  // chord; otherwise a selected anchor moves alone. A note anchoring no group: whether it is selected.
  bool groupSelectedWhole(doc::NoteId anchor) const;
  // bug-ji-input: `id` is an anchor the Move `d` drags alone, of a tuned group (it moves on the group's harmonics).
  bool aloneTuned(const DragState& d, uint32_t id) const;
  // The frame the pointer is over (its grips are shown); 0: none.
  doc::JiGroupId groupFrameAt(ui::Vec2 p) const;
  void setHoverGroup(doc::JiGroupId id);
  inline static constexpr float kWarpGrip = 11.0f;     // marker / grip size (logical px)
  inline static constexpr float kWarpGripGap = 13.0f;  // the glide grip's least distance from its marker

  // A point on a note drawn along a path, `frac` of the way through it (tests, anchors); nullopt: not on a path.
  std::optional<ui::Vec2> pathPoint(const doc::Note& n, double frac) const;
  // The device box of an on-path note (its path's extent plus its thickness).
  ui::IRect pathBox(const doc::Note& n, Ticks start, Ticks length, const tuning::PitchId& pitch) const;
  // Half the drawn thickness of a path note (device px).
  float pathHalfThickness() const noexcept;

  // --- bends, pen modes, breakpoints (T7.6, on the derived API since T7.14; roll_pen.cpp) ---
  // A bend is a property of the note (doc::NoteBend: the neighbour's id, a length, a shape): Alt + a note's left /
  // right edge and Ctrl+G write it, and its far end follows the neighbour from then on. The free curve
  // (NoteCurves::pitch: pen strokes, breakpoints) is never read as a bend (M7 review H4).
  // The note's curve record as shown: a gesture's preview, else the document's. nullptr: none.
  const doc::NoteCurves* curveRecord(doc::NoteId note) const;
  // The points of the FREE curve as shown. Empty: none.
  std::span<const doc::Breakpoint> curvePoints(const CurveTarget& t) const;
  // The pitch the free curve is added to, `offset` ticks into its note, in cents: the note's pitch where it starts
  // plus its bends and its group's warp there (not its vibrato). nullopt: unresolvable.
  std::optional<double> curveBase(const CurveTarget& t, double offset) const;
  // The target's start and length in content ticks (false: no such note).
  bool curveSpan(const CurveTarget& t, Ticks& start, Ticks& length) const;
  // Breakpoint handles (device px): a dot per point, a diamond in the middle of a segment wide enough to bend. Shown
  // for the selected notes and, in Draw Mode, the note under the pointer; a curve of more
  // than kMaxCurveHandles points (a free-drawn stroke) shows none.
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
  // The neighbour a bend of `n`'s start (edge −1) / end (+1) glides from / to (M7 review M9): of the notes that end
  // nearest before its start (start nearest after its end) — those within one grid step of the nearest — the one
  // nearest in pitch that is not at the note's own pitch. nullptr: none (`samePitch`: there is a neighbour, at the
  // note's own pitch).
  const doc::Note* bendNeighbour(const doc::Note& n, int edge, bool* samePitch = nullptr) const;
  // The edit an Alt + edge drag in state `d` commits (one SetNoteCurves for every selected note it bends: to its
  // neighbour, or — none there — from / to d.bendCents in its free curve).
  doc::ops::EditResult bendEdit(DragState& d) const;
  void updateBendPreview();
  // An Alt + edge drag's pitch for a note with no neighbour there: the pointer's travel up / down in cents from the
  // pressed note's pitch at that edge, snapped to the rows, the harmonic guides and that pitch (Shift: whole cents).
  double freeBendCents(ui::Vec2 p, ui::Mods mods) const;
  // The length of a note's bend in (edge −1) / out (+1); 0: none.
  Ticks bendLength(doc::NoteId note, int edge) const;
  // Ctrl+G: every selected note glides out into the next selected one over one grid step. One undo step.
  bool glideSelection();
  // The bend in (edge −1) / out (+1) of the selected notes Straight or as an S-curve.
  bool setBendShape(int edge, doc::GlideShape::Kind kind);
  bool bendIsStraight(doc::NoteId note, int edge) const;
  // Removes the selected notes' bends, drawn curves and vibratos (one undo step).
  bool clearBends();
  // Ticks per second at a content tick where the clip plays it (120 bpm without a timeline).
  double ticksPerSecond(Ticks contentTick) const;
  // Pen gestures (the note area calls them; each previews the curve the release stores).
  void beginPen(const CurveTarget& t, ui::Vec2 p, ui::Mods mods, bool vibrato);
  void penTo(ui::Vec2 p, ui::Mods mods);
  // Ctrl+click in Draw Mode: a breakpoint on the curve at the pointer (the press then drags it).
  bool beginCurvePoint(const CurveTarget& t, ui::Vec2 p);
  void beginCurveHandle(const CurveHit& h, ui::Vec2 p);
  void curveHandleTo(ui::Vec2 p, ui::Mods mods);
  bool deleteCurvePoint(const CurveHit& h);
  // Stores a note's free curve (SetNoteCurve), one undo step; the selection is kept.
  bool commitCurve(const CurveTarget& t, std::vector<doc::Breakpoint> pts, std::string_view what);
  // A pitch in cents snapped for a breakpoint drag at content tick `tick`: the nearest row, harmonic guide (when
  // shown; where it is at that tick) or `own` (the pitch the curve is added to: value 0).
  double snapCurveCents(double cents, double own, double tick) const;
  // The pitch under device y, not rounded (the inverse of yOfCents).
  double centsAtScreenY(float y) const noexcept;
  // A resized note's free curve: it keeps its place in time and is cut where the note no longer reaches.
  // `startDelta`: how far the start moved.
  static std::vector<doc::Breakpoint> retimeCurve(std::span<const doc::Breakpoint> pts, Ticks newLength, Ticks startDelta);
  // A resize of the selected notes (ops::resizeNotes) with their free curves retimed in the same undo step (bends
  // and vibratos are properties: they need nothing).
  doc::ops::EditResult resizeEdit(doc::ops::NoteEdge edge, Ticks delta) const;
  bool hasCurvePreview() const noexcept { return !curvePreview_.empty() || warpPreview_.valid(); }
  // A Shift + body drag: the selected notes `cents` away as Cents notes, `dt` later (one undo step).
  doc::ops::EditResult detuneEdit(Ticks dt, double cents) const;
  inline static constexpr size_t kMaxCurveHandles = 24;
  inline static constexpr size_t kMaxPenPoints = 512;  // a stroke's points after thinning
  inline static constexpr float kMinPenStroke = 8.0f;  // logical px: a shorter pen drag is a click (it erases the note)
  inline static constexpr float kDetuneDeadZone = 6.0f;  // logical px before a Shift drag detunes (M7 review H5)
  void clearCurvePreview();

  // --- vibrato by dragging (T7.14, plan M7 V1–V3; roll_vibrato.cpp) ---
  // Vibrato is a property of the note (doc::NoteVibrato); nothing is typed and nothing is baked.
  //   the ~ grip   above the right end of the hovered note and of every selected note: drag up / down = depth (the
  //                wave grows on the note), left / right = rate (right: longer waves); Shift = fine; double-click
  //                removes it. With several notes selected the drag applies to all of them.
  //   corners      once a note vibrates, the four corners of its amplitude envelope are grips under the note: onset,
  //                end of the fade-in, start of the fade-out, end. They drag sideways (grid snap; Shift: free); the
  //                two inner ones can meet (a swell).
  // Separation from the other gestures: the ~ grip lies ABOVE the line and kVibratoGripInset inside the note's end,
  // clear of the resize / Alt-bend zone (the last kEdgeGrab px of the note's band); the corner grips lie UNDER the
  // line (under the envelope), clear of the labels above and of the breakpoints on the line.
  enum class VibratoPart : uint8_t { None, Grip, Corner };
  struct VibratoHit {
    VibratoPart part = VibratoPart::None;
    doc::NoteId note;
    int corner = -1;
    bool valid() const noexcept { return part != VibratoPart::None; }
  };
  VibratoHit vibratoPartAt(ui::Vec2 p) const;
  // Where a note's ~ grip and its envelope's corner grips are (device px); nullopt: the note is not placed / has no
  // vibrato.
  std::optional<ui::Vec2> vibratoGripPos(const doc::Note& n) const;
  std::optional<ui::Vec2> vibratoCornerPos(const doc::Note& n, int corner) const;
  // The notes whose grips are shown: the hovered one, and the selected ones while they are at most
  // kMaxVibratoGripNotes and each wide enough for a grip (or vibrating already).
  void vibratoNotes(std::vector<const doc::Note*>& out) const;
  // The vibrato of a note as shown (a gesture's preview first); !set(): none.
  doc::NoteVibrato vibratoOf(doc::NoteId note) const;
  void beginVibratoDrag(const VibratoHit& h, ui::Vec2 p);
  void vibratoDragTo(ui::Vec2 p, ui::Mods mods);
  bool commitVibratoDrag(DragState& d);
  // Removes the vibrato of the note (and of the selection when the note is in it). One undo step.
  bool removeVibrato(doc::NoteId note);
  // Sets the selected notes' vibrato depth and rate (Hz), keeping each note's envelope (scenes, tests).
  bool setSelectionVibrato(double depthCents, double rateHz);
  static std::string vibratoText(double depthCents, double rateHz);  // "Vibrato 30 ¢ · 6 Hz"
  inline static constexpr double kDefaultVibratoHz = 6.0;    // the rate of a new vibrato
  inline static constexpr size_t kMaxVibratoGripNotes = 16;  // a larger selection shows the hovered note's grips only
  inline static constexpr float kVibratoGrip = 12.0f;       // grip size (logical px; hit boxes are this big)
  inline static constexpr float kVibratoGripInset = 12.0f;  // the ~ grip's centre from the note's end
  inline static constexpr float kVibratoGripRise = 12.0f;   // … and above / the corners below the line (mirrored for
                                                            // an inverted vibrato: the grip on the side it swings first)

  // --- pitch paths (T7.4; T7.14: the effective curve, cached) ---
  // Device y of a pitch in cents, not rounded.
  float yOfCents(double cents) const noexcept;
  // Whether a note is drawn along a path instead of on its row: it has a curve record (a drawn curve, a bend, a
  // vibrato), its group's warps move it, or anchorMoved. O(1) for a clip without curves and warps.
  bool onPath(const doc::Note& n, const tuning::PitchId& pitch) const;
  // A note anchored to a note whose curve (free curve, bends, warps) has moved it where this one starts: it sounds
  // there (pitch_resolve, Q293), not where its anchor chain stands still, so it is drawn on a path at that pitch.
  bool anchorMoved(const doc::Note& n) const;
  // The pitch in cents of a group's anchor at content tick `t` as a note starting there resolves against it (its
  // free curve, bends and warps, not its vibrato; clamped to its span) minus its pitch at its start: how far the
  // harmonic guides of `h` (drawn along the anchor) are moved there. 0 for the reference's guides.
  double harmonicsShift(const Harmonics& h, double t) const;
  // A path note as it is drawn and hit: `base` = the pitch it resolves to at its start in cents (a warped member:
  // its written pitch, its JI pitch or the point of the glide), `pts` = doc::effectiveNoteCurve (the free curve, the
  // bends, the warps and the vibrato; cents over ticks from the note's start; empty: straight) flattened to
  // pathTolerance() (bug-curve-render: a fraction of a device px at the vertical zoom it was built for; an entry
  // too coarse for the current zoom is flattened again, a finer one stands — the paint simplifies in device px).
  // Cached (M7 review M14): a paint walks the cached points and evaluates nothing. An entry
  // stands until what it was flattened from changes: the note (start, length, identity), its curve record (compared
  // by value when the clip's curve list is replaced), the tuning or the timeline; an entry that read the clip's
  // resolved pitches (a bend's neighbour, a warp, a note anchor) is rebuilt for every new clip snapshot. So moving
  // one note of a clip of 2,000 vibrating notes re-flattens none of the others. A note a gesture previews is
  // evaluated per call into a scratch entry (valid until the next call). nullptr: not a path note, or its pitch
  // does not resolve.
  struct NotePath {
    double base = 0.0;
    std::vector<doc::Breakpoint> pts;
    bool ok = false;
    bool curved = false;  // a point has a curvature (the stored free curve as it is): segments are not straight
    float lo = 0.0f, hi = 0.0f;  // the extent of `pts` in cents (0 … 0 when empty): what a note too narrow for its wave is drawn as
    // bug-curve-render: a note with a vibrato also keeps the curve without it (`rest`, flattened alike; empty:
    // straight) and the vibrato's envelope and period (ticks), so a paint where its cycles are too short to draw shows
    // the band the wave fills around `rest` (vibPeriod 0: no vibrato).
    std::vector<doc::Breakpoint> rest;
    bool restCurved = false;
    doc::VibratoEnvelope vib;
    double vibPeriod = 0.0;
    double tol = 0.0;  // the tolerance (cents) it was flattened to
    // What it was flattened from (the cache keeps an entry across clip snapshots while these stand).
    const doc::NoteCurves* rec = nullptr;
    Ticks start = 0, length = 0;
    tuning::PitchId pitch;
    bool usedIndex = false;  // it read the clip's resolved pitches (a bend's neighbour, a warp, a note anchor)
    uint64_t baseGen = 0;    // the tuningGen_ `base` is for (T7.16a M16: a tuning change re-derives only `base`)
    uint64_t previewGen = 0;  // a preview entry: the preview it was built for
  };
  const NotePath* pathOf(const doc::Note& n) const;
  // The flattening tolerance (cents) of the cached paths: kPathToleranceCents, halved until it is under `maxPx`
  // device px at the current rows (never under kMinPathToleranceCents). Set by the paint (fitPathTolerance), again
  // only once the rows' scale moved by 2× (a vertical zoom, not a tuning morph); an entry is flattened again when it is
  // more than 2× coarser than this.
  inline static constexpr double kPathToleranceCents = 1.0;
  inline static constexpr double kMinPathToleranceCents = 1.0 / 64.0;
  double pathTolerance() const noexcept { return pathTol_; }
  void fitPathTolerance(float maxPx) noexcept;
  // The resolved pitches of the shown clip and their index (built once per clip snapshot / tuning / timeline, only
  // when a note needs them).
  const doc::NotePitchIndex& pitchIndex() const;
  // The pitch in cents of a note `offset` ticks after its start, when it starts at `start` with identity `pitch` (a
  // drag preview may differ from the note): where it starts plus its effective curve. nullopt: unresolvable. The seam
  // hit-testing and drawing go along.
  std::optional<double> pathCents(const doc::Note& n, Ticks start, const tuning::PitchId& pitch, double offset) const;
  // Where a note sounds when it starts, in cents (a path note: NotePath::base; else its identity's pitch).
  std::optional<double> startCents(const doc::Note& n) const;
  // T7.16a (M7 review M14): how far a note anchored to a note a Move drag steps is shown moved with it (cents; 0:
  // not one). For the cached paths, whose base is the document's.
  double anchorPreviewShift(const doc::Note& n) const;
  // How many path entries were flattened since the roll was made (tests: a repaint of an unchanged clip adds none).
  uint64_t pathBuilds() const noexcept { return pathBuilds_; }

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
  // A drawn note's ratio on row `degree` with the pointer at `cents`: the harmonic of `H` nearest the pointer (to its
  // base: the reference, or a group's anchor), else (no H) the row's pitch snapped to the simplest ratio.
  tuning::Rational ratioFor(int64_t degree, double cents, const std::optional<Harmonics>& H) const;

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
  mutable std::unordered_map<uint32_t, const doc::Note*> noteCache_;  // findNote's answers for noteCacheFor_
  mutable const doc::NoteClip* noteCacheFor_ = nullptr;
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
  // T7.6 / T7.14: the curve records a gesture in progress shows instead of the document's (whole records, sorted by
  // note id; an empty record = the note shown without one), and the depth of the last vibrato made (the default of
  // the next pen vibrato).
  std::vector<doc::NoteCurves> curvePreview_;
  double vibratoDepth_ = 30.0;
  void setCurvePreview(const CurveTarget& t, const std::vector<doc::Breakpoint>& pts);
  // The preview record of a note (made from the document's when the gesture has none yet).
  doc::NoteCurves& previewRecord(doc::NoteId note);
  // T7.14: a warp gesture's preview: the group's warps as the release would store them, the clip with them and its
  // resolved pitches (the members' start pitches move with the warps).
  struct WarpPreview {
    doc::JiGroupId group;
    std::vector<doc::JiWarp> warps;
    std::shared_ptr<const doc::NoteClip> clip;
    doc::ClipPitches pitches;
    doc::NotePitchIndex index;
    bool valid() const noexcept { return group.valid(); }
  };
  WarpPreview warpPreview_;
  void setWarpPreview(doc::JiGroupId group, std::vector<doc::JiWarp> warps);
  bool applyWarps(doc::JiGroupId group, std::vector<doc::JiWarp> warps, std::string_view what);
  WarpSel warpSel_;
  bool proportionArmed_ = false;
  doc::JiGroupId hoverGroup_;
  // The path cache (pathOf).
  struct PathCache {
    const doc::NoteClip* clip = nullptr;
    const void* timeline = nullptr;
    uint64_t tuningGen = 0;
    bool resolved = false;
    bool anyWarp = false;
    std::shared_ptr<const doc::NoteCurveList> curves;  // the list the entries' records point into (kept alive)
    doc::ClipPitches pitches;
    doc::NotePitchIndex index;
    std::unordered_map<uint32_t, NotePath> notes;
  };
  mutable PathCache paths_;
  double pathTol_ = kPathToleranceCents;
  double fittedPxPerCent_ = 0.0;  // the rows' scale pathTol_ was fitted to
  // T7.16a (M7 review M15): a gesture's preview paths by note id, valid while previewGen_ stands (bumped whenever
  // curvePreview_ or warpPreview_ changes), so a drag frame flattens each previewed note once, not per query.
  mutable std::unordered_map<uint32_t, NotePath> previewPaths_;
  uint64_t previewGen_ = 1;
  bool finer_ = false;  // Alt over a group: its finer series (M5)
  ui::Mods pointerMods_ = 0;
  doc::JiGroupId hoverGuide_;  // the group under the pointer whose guides show (captureGroupAt at the pointer)
  // M14: while a Move drag steps notes others are anchored to, centsOf follows their preview pitch (`shown`).
  std::optional<double> centsOfImpl(const tuning::PitchId& p, bool shown) const;
  mutable uint64_t pathBuilds_ = 0;
  uint64_t tuningGen_ = 1;  // bumped whenever tuning_ changes (a rebuild or an in-place morph)
  void syncPaths() const;
  bool previewTouches(const doc::Note& n) const;
  void touchPreview();  // curvePreview_ / warpPreview_ changed: the preview paths go (previewGen_)
  mutable std::vector<GroupInfo> groupInfos_;
  mutable const doc::NoteClip* groupInfosFor_ = nullptr;
  mutable uint64_t groupInfosGen_ = 0;
  mutable std::string groupInfosTuning_;
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
  mutable std::vector<uint32_t> untunedAnchors_;  // the anchors of groups Back to Tuning (sorted)
  mutable bool activeGroupValid_ = false;
  mutable const doc::JiGroup* activeGroup_ = nullptr;
  mutable doc::JiGroupId lastActiveGroup_;
  mutable const doc::Note* activeAnchor_ = nullptr;
  mutable tuning::Rational activeFundamental_{1};
};

}  // namespace pq::views::roll
