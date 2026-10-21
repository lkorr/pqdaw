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
#include "pq/tuning/tuning.h"
#include "pq/ui/color.h"
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

struct DrawnNote {
  Ticks start = 0;
  Ticks length = 0;
  int64_t degree = 0;
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
  // on the harmonics of the clip's virtual fundamental (harmonicDenominator = virtualFundamental(), Q40).
  void stepSelection(int32_t steps, doc::ops::StepUnit unit);
  void copySelection();
  void cutSelection();
  void paste();
  void setInsertTick(std::optional<Ticks> t) noexcept { insertTick_ = t; }
  // The clip's virtual fundamental as a denominator D (fundamental = reference / D; 0 = none), see the rule at its
  // definition (Q40). Ratio Up/Down and vertical drags step on its harmonics; the note area draws it (SPEC §4.3).
  int64_t virtualFundamental() const;
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
  // Preview geometry of a note while a Move / Copy / Resize drag runs (nullopt: not part of the drag).
  struct Preview {
    Ticks start, length;
    tuning::PitchId pitch;
  };
  std::optional<Preview> preview(const doc::Note& n) const { return previewOf(drag, n); }
  std::optional<Preview> previewOf(const DragState& d, const doc::Note& n) const;
  void updatePreviewPitches();
  float previewValue(const doc::Note& n) const noexcept;  // velocity / chance shown in the lane

  // --- audition ---
  void audition(const tuning::PitchId& p, float velocity, Ticks contentStart, Ticks length);

  // Repaint helpers.
  void markNotesDirty();  // note area + velocity lane
  void markAllDirty();

private:
  void rememberHarmonics(std::span<const doc::NoteKey> keys, int64_t D);
  void rebuildRows();
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
  bool chance_ = false;
  bool fitted_ = false;  // fitToClip ran for the current clip (needs a laid-out area)

  std::vector<uint32_t> sel_;
  std::vector<uint64_t> selBits_;  // bitmap of sel_ from selMin_ (empty: binary search)
  uint32_t selMin_ = 0;
  std::optional<Ticks> insertTick_;
  std::vector<doc::Note> clipboard_;  // starts relative to the earliest copied note
  // Q40: per Ratio note, the denominator its last step used and the pitch it produced.
  struct HarmonicMemory {
    int64_t denominator;
    tuning::PitchId pitch;
    doc::NoteKey key;  // where the note was after the step (a lookup hint)
  };
  std::unordered_map<uint32_t, HarmonicMemory> harmonic_;
  uint64_t harmonicVersion_ = 0;
  mutable const doc::NoteClip* vfClip_ = nullptr;  // virtualFundamental() cache
  mutable uint64_t vfVersion_ = UINT64_MAX;
  mutable int64_t vfDen_ = 0;
};

}  // namespace pq::views::roll
