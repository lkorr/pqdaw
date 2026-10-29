#pragma once

// Arrangement view (T3.8, SPEC §9.2–9.4): Ableton Live 12's Arrangement View, display and navigation.
//
//   ┌──────────────────────────────────────────────────────────────┬──────────────────────┐
//   │ overview: the whole song, the visible region framed           │                      │  arrangement/overview
//   ├──────────────────────────────────────────────────────────────┼──────────────────────┤
//   │ ruler: bar numbers + ticks · loop brace · locators / tempo /  │                      │  arrangement/ruler
//   │        signature markers, start marker                        │                      │
//   ├──────────────────────────────────────────────────────────────┼──────────────────┬───┤
//   │ lanes: bar shading, dashed grid lines, clips (title strip +   │ track headers:   │ ▒ │  arrangement/tracks/lanes,
//   │        cached note preview), group summaries, playhead        │ name strip in    │ ▒ │  arrangement/tracks/track[i],
//   │                                                               │ the track colour │   │  arrangement/vscroll
//   │                                                               │ | mixer (T3.10)  │   │
//   ├──────────────────────────────────────────────────────────────┼──────────────────┴───┤
//   │ time ruler: 0:00 0:05 … (min:sec from the tempo map)          │                      │  arrangement/time-ruler
//   └──────────────────────────────────────────────────────────────┴──────────────────────┘
// Look: Live 12's arrangement as in the user's screenshot (theme tokens `arrangement.*`, sampled from it).
//
// Track headers sit right of the lanes, as in Live (the Session/Arrangement mixer sections follow them there).
//
// Horizontal axis: `originTick` (tick at the left edge of the lanes) and `pxPerTick` (logical px per tick; device
// px per tick = pxPerTick × UI scale). Both are view state (not in the document, not undoable: Q15). Vertical: the
// track rows scroll in a ui::ScrollView ("arrangement/tracks") with a scrollbar.
//
// Navigation: wheel scrolls vertically, Shift+wheel (or a horizontal wheel) horizontally, Ctrl+wheel zooms around
// the pointer; a drag in the ruler zooms (vertical) and scrolls (horizontal) around the pressed tick, a click in the
// ruler locates (start marker + TransportPort::locate, snapped to the grid); a click/drag in the overview moves the
// view. Actions (registered in ViewContext::actions()): arrangement.zoomToSelection (Z), arrangement.zoomBack (X),
// arrangement.zoomIn (=), arrangement.zoomOut (-), view.toggleFollow (Ctrl+Shift+F).
//
// Rendering (the T0.7-fix1 partial-redraw contract): everything paints only inside its rect and only what meets
// PaintContext::clip. Rows are a flat list built from the track tree (folded groups hide their members); each
// track keeps its clips' start/end in sorted arrays so a paint finds the visible clips by binary search, and only
// visible rows and clips emit geometry. A clip's note preview is cached and rebuilt only when the clip snapshot
// pointer (or the tuning its pitches resolve in) changes. Grid shading and lines are drawn once per column across
// all visible rows. The playhead is read from the TransportPort only while playing, on a timer due when it next
// crosses a device pixel; a move damages only its old and new columns.
//
// Editing (T3.9, Live's arrangement editing; src/arrangement/editing.cpp):
//   selection   a click on a clip selects it and shows it in the clip view (ctx.setDetailClip; a double-click also
//               shows and focuses the detail view); Shift adds, Ctrl toggles; Ctrl+A selects every clip. A click on
//               empty lane space (or in a clip's body) places the insert marker on that track; a drag there makes a
//               time selection over the tracks it crosses and selects the clips it touches (the rubber band);
//               Shift+click extends it. A click on a track header selects the track.
//   gestures    dragging a clip's title strip moves the selection (relative snap, T3.3 snapDelta, from the pressed
//               clip's start; across tracks by rows), Alt+drag copies, dragging a clip's edge resizes the selection.
//               Every gesture is previewed (the moved clips are drawn at their target, nothing is dispatched) and
//               dispatched once on release: one undo step. Esc cancels. Right-click opens the edit + grid menu.
//   edits       Ctrl+C/X/V (in-process clipboard of clips: whole selected clips, or the part of each selected
//               track's clips inside a time selection), Ctrl+D, Delete, Ctrl+E (at the insert marker or the time
//               selection's edges), Ctrl+J, Ctrl+L, Ctrl+Shift+M, Ctrl+I, Ctrl+Shift+D, Ctrl+Shift+Delete, 0
//               (deactivates / reactivates the selected clips, else toggles the selected tracks' activators; Q15).
//               Each is one pq::doc::ops edit (several chained into one Batch where needed) = one undo step.
//   Partial redraw: a selection change damages the selected rows over the changed time span only (whole rows when
//   the selected tracks change); a drag frame damages the dragged clips' old and new rects.
//
// Track headers, groups, returns and master (T3.10):
//   rows        the main track tree scrolls; the return tracks and the master are pinned at the bottom (`bottom`,
//               Live's layout) with their own lanes (grid only: automation lanes come with T3.15) and headers.
//   headers     a click selects the track (Shift adds, Ctrl toggles); the bottom edge (kResizeGrab) drags the row
//               height (ViewState, not undoable, Q26); right-click opens the track menu (rename, insert, group,
//               ungroup, colour, delete); the selected tracks' headers are tinted. The mixer section on the right is
//               T3.10's MixerStrip (pq/views/mixer/mixer.h, installed by installMixer through setHeaderDecorator).
//   commands    Ctrl+Shift+T / Ctrl+Alt+T insert a MIDI / return track after the selection; Ctrl+G groups the selected
//               tracks (any parents, nesting allowed), Ctrl+Shift+G ungroups the selected groups; Ctrl+R renames the
//               first selected track in place (Enter commits, Esc cancels); Delete with only tracks selected deletes
//               them; `0` toggles the selected tracks' activators = mute (Q15: a Param, no recompile). Each = one undo
//               step.
//   repaint     a Param-only document change (volume, pan, sends, solo: a mixer drag) rebinds the rows and headers
//               without repainting the lanes (the strips repaint only what changed); a mute change repaints them
//               (muted tracks' clips are drawn inactive, as in Live).
//
// Extension points:
//   T3.9  (editing, done): selection() / setSelection(), timeSelection()/setTimeSelection() (Z zooms to it),
//         rows()/rowAt()/tickAtX()/xOfTick() for hit testing, `onPaintOverlay` (drawn after the editing overlay),
//         the "arrangement" key scope and ActionRegistry::set for its actions.
//   T3.10 (headers, mixer): TrackHeader::mixerSection() (a Row host at the header's right, sized by
//         setHeaderMixerWidth), setHeaderDecorator() (called whenever a header is bound to a track: add or update
//         controls), setHeaderWidth(), heights from the store's ViewState (TrackView::height, logical px).
//   T3.15 (automation lanes): automation() (pq/views/arrangement/automation.h), laneRows() / laneRect() (lanes under
//         their track: ArrangementRow::lanesH), the header's paramChooser() / lanesToggle(), A = automation mode.
//
// UI thread only. Nothing here is RT-safe.

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "pq/core/grid.h"
#include "pq/core/time.h"
#include "pq/document/model.h"
#include "pq/ui/controls.h"
#include "pq/ui/overlay.h"
#include "pq/ui/widget.h"
#include "pq/views/arrangement/automation.h"
#include "pq/views/view_context.h"

namespace pq::views {

class Shell;
class ArrangementView;

namespace arrangement {
class Model;
class Editor;
class LaneCanvas;
class Ruler;
class TimeRuler;
class Overview;
class TrackStack;
class BottomTracks;
class LaneHeader;
class ReenableButton;
}  // namespace arrangement

// Layout metrics in logical px.
namespace arrangement_metrics {
inline constexpr float kOverviewHeight = 16.0f;
inline constexpr float kRulerHeight = 40.0f;      // numbers 17 + loop brace 11 + marker row 12 (Live: brace below)
inline constexpr float kRulerNumberRow = 17.0f;
inline constexpr float kRulerLoopRow = 11.0f;
inline constexpr float kTimeRulerHeight = 18.0f;  // min:sec ruler under the tracks (Live)
inline constexpr float kHeaderWidth = 290.0f;     // name strip + mixer section, as in Live 12
inline constexpr float kHeaderMixerWidth = 180.0f;  // T3.10's mixer section (pq/views/mixer/mixer.h)
inline constexpr float kTrackHeight = 48.0f;      // default lane height (ViewState height 0)
inline constexpr float kMinTrackHeight = 20.0f;
inline constexpr float kMaxTrackHeight = 480.0f;
inline constexpr float kReturnTrackHeight = 20.0f;  // default return row (Live: one line)
inline constexpr float kMasterTrackHeight = 40.0f;  // default master row
inline constexpr float kBottomGap = 4.0f;           // divider between the tracks and the returns / master
inline constexpr float kBottomMaxFraction = 0.5f;   // the pinned rows take at most this share of the track area
inline constexpr float kResizeGrab = 3.0f;          // logical px at a header's bottom edge that resize the row
inline constexpr float kChipWidth = 96.0f;          // the header's chip section (tuning chip), under the title
inline constexpr float kChipHeight = 14.0f;
inline constexpr float kTitleHeight = 20.0f;      // track header title bar
inline constexpr float kClipTitleHeight = 14.0f;  // clip title strip
inline constexpr float kGroupIndent = 7.0f;       // header indent per group level
inline constexpr float kDropArea = 40.0f;         // empty lane space below the last track
inline constexpr float kRulerLabelMinPx = 44.0f;  // minimum spacing of ruler labels
// Zoom limits (logical px per tick): 1 px per 16 bars of 4/4 … 2000 px per sixteenth.
inline constexpr double kMinPxPerTick = 1.0 / (16.0 * static_cast<double>(kTicksPerWhole));
inline constexpr double kMaxPxPerTick = 2000.0 / (static_cast<double>(kTicksPerWhole) / 16.0);
inline constexpr double kDefaultPxPerTick = 96.0 / static_cast<double>(kTicksPerWhole);  // a 4/4 bar = 96 px (1/4 grid)
// Progressive note previews (frame-time bounds, see LaneCanvas::paint):
// - at most this many preview notes are built per lanes paint (at least one clip); the rest of the clips draw their
//   bodies and get their previews over the next frames;
inline constexpr uint32_t kPreviewBuildNotesPerPaint = 4096;
// - while the zoom is changing (and until kZoomSettleSeconds after the last change) a clip whose notes are denser
//   than this many device px per note draws no preview; the lanes repaint once the zoom settles;
inline constexpr float kZoomPreviewMinPxPerNote = 1.5f;
inline constexpr double kZoomSettleSeconds = 0.15;
// - the preview cache holds up to this many notes (24 bytes each: 12 MiB), or 3 × the most one paint drew; beyond
//   that, previews not drawn in the last kPreviewMaxAgePaints lanes paints are dropped.
inline constexpr size_t kPreviewCacheNotes = size_t{1} << 19;
inline constexpr uint64_t kPreviewMaxAgePaints = 120;
// Preview tiles (T3.8-fix1): with a GPU (PaintResources::tiles), a clip of at least kTileMinNotes notes whose body
// fits a tile is drawn from a mask rendered once per (clip snapshot, tuning, device px per tick, body height) into
// the renderer's TileAtlas (≤ 4 pages of 2048² R8 = 16 MiB GPU) and drawn as one quad, not one rect per note run.
// Tiles are rasterised in LaneCanvas::onFrame (never in paint), at most kTileBuildTexelsPerFrame texels per frame
// (at least one tile), and only once the zoom has settled. Until a clip's tile exists, paint draws its previous
// tile stretched (another zoom or row height), else its notes directly while this paint's fallback stays within
// kTileFallbackRectsPerPaint rects, else only its body; once the tiles are built the lanes repaint (only if a clip
// was drawn approximately: the direct path draws exactly the pixels of the tile).
inline constexpr uint32_t kTileMinNotes = 4;
inline constexpr size_t kTileBuildTexelsPerFrame = size_t{1} << 20;
inline constexpr uint32_t kTileFallbackRectsPerPaint = 4096;
}  // namespace arrangement_metrics

// One visible track row (flattened track tree; members of folded groups are not rows).
struct ArrangementRow {
  doc::TrackId id;
  const doc::Track* track = nullptr;  // valid while ArrangementView::shown() is the snapshot it came from
  int32_t parent = -1;                // row index of the containing group, −1 at the top level
  uint16_t depth = 0;                 // group nesting (0 = top level)
  bool group = false;
  bool folded = false;
  const doc::TuningDef* tuning = nullptr;  // the track's effective tuning (Inherit resolved)
  int32_t y = 0;                      // device px from the top of the track content (bottom rows: of the pinned area)
  int32_t h = 0;                      // device px (the bottom pixel row is the separator)
  bool bottom = false;                // a return or the master (pinned at the bottom, ArrangementView::bottomRows)
  // T3.15: the automation lanes shown under the track (below y + h): laneRows()[firstLane, firstLane + laneCount),
  // lanesH device px in all. rowAt() and the clip editing see only the track's own lane (y … y + h).
  int32_t lanesH = 0;
  uint32_t firstLane = 0;
  uint32_t laneCount = 0;
};

// A track header (display only in T3.8: colour, name, group indent and fold). T3.10 fills mixerSection().
class TrackHeader : public ui::Control {
public:
  TrackHeader(std::string name, ArrangementView& view);
  ~TrackHeader() override;
  // Rebinds to a row (the view reuses headers by row index when rows change).
  void bind(const ArrangementRow& row, std::span<const ArrangementRow> rows);
  doc::TrackId trackId() const noexcept { return id_; }
  const doc::Track* track() const noexcept { return track_; }
  // T3.10's controls go here: a Row host placed at the header's right edge, full height, sized by
  // ArrangementView::setHeaderMixerWidth (0 = hidden).
  ui::Widget& mixerSection() const noexcept { return *mixer_; }
  // T3.10 / T3.14: a Stack host under the title (the name column's second line, kChipWidth wide, kChipHeight high),
  // for the tuning chip; hidden while the row is too low for it.
  ui::Widget& chipSection() const noexcept { return *chip_; }
  // The fold toggle (groups only; hidden otherwise).
  ui::Control& foldButton() const noexcept { return *fold_; }
  // T3.15: the automation-mode parameter chooser (under the title, shown in automation mode when the row is tall
  // enough) and the lanes toggle (at the title's right end; shown in automation mode or while the lanes are shown).
  ui::Control& paramChooser() const noexcept { return *chooser_; }
  ui::Control& lanesToggle() const noexcept { return *lanesToggle_; }
  // Device-px rect of the title bar (name + fold), for tests and T3.10, and its fill (the track colour, grey when
  // the track is deactivated).
  ui::IRect titleRect() const noexcept;
  ui::Rgba titleColour() const noexcept;

  // Ctrl+R: the title becomes a text field (all selected); Enter / focus loss commits (one undo step), Esc cancels.
  void beginRename();
  bool renaming() const noexcept { return rename_ != nullptr; }
  // Repaints when the track's selected state changed (the header is tinted while selected).
  void syncSelected();
  ui::TextEdit* renameField() const noexcept { return rename_; }

  void arrange(ui::LayoutPass& pass) override;
  void paint(ui::PaintContext& ctx) override;
  bool onPointer(const ui::PointerEvent& e) override;
  void onCaptureLost() override { resizing_ = false; }
  void onFrame(double now) override;
  // T4.16a: ResizeNS over the bottom edge (the height drag) and while resizing; else the arrow (a click selects).
  ui::Cursor cursorAt(ui::Vec2 pos) const override;
  // Whether a press at pos (device px) grabs the bottom edge that resizes the row (kResizeGrab).
  bool onResizeEdge(ui::Vec2 pos) const noexcept;

private:
  void endRename(bool commit);
  ArrangementView& view_;
  ui::TextEdit* rename_ = nullptr;
  bool renameDone_ = false;  // the field is removed at the next frame (not from inside its own callback)
  bool resizing_ = false;
  float resizeStartY_ = 0.0f;
  float resizeStartH_ = 0.0f;  // logical px
  doc::TrackId id_;
  const doc::Track* track_ = nullptr;
  std::string name_;
  uint32_t colour_ = 0;
  bool active_ = true;
  bool selected_ = false;
  bool group_ = false;
  bool folded_ = false;
  uint16_t depth_ = 0;
  std::vector<uint32_t> ancestorColours_;  // outermost first (group stripes)
  ui::Control* fold_ = nullptr;
  ui::Widget* mixer_ = nullptr;
  ui::Widget* chip_ = nullptr;
  ui::Control* chooser_ = nullptr;      // T3.15
  ui::Control* lanesToggle_ = nullptr;  // T3.15
  bool bottomRow_ = false;
  bool chooserWanted_ = false;          // automation mode: the chooser is laid out when the row has room
};

class ArrangementView : public ui::Widget {
public:
  explicit ArrangementView(ViewContext& ctx);
  ~ArrangementView() override;
  ViewContext& context() const noexcept { return ctx_; }
  // The snapshot rows() and the headers refer to (the document's current one after every change).
  const doc::Snapshot& shown() const noexcept;

  // ---- horizontal axis -------------------------------------------------------------------------------------
  double pxPerTick() const noexcept { return pxPerTick_; }    // logical px per tick
  double originTick() const noexcept { return originTick_; }  // tick at the left edge of the lanes
  // Sets both (zoom clamped to the limits, origin ≥ 0) and repaints what depends on them.
  void setTimeView(double originTick, double pxPerTick);
  // Zooms by `factor` (> 1 = in) keeping the tick under device x `anchorX` where it is.
  void zoomAt(float anchorX, double factor);
  // Scrolls by device px (positive = later).
  void scrollByPx(double devicePx);
  // Device x of a tick / tick at a device x (lanes and ruler share the axis).
  float xOfTick(double tick) const noexcept;
  double tickAtX(float deviceX) const noexcept;
  // Device px per tick at the current UI scale.
  double devicePxPerTick() const noexcept;
  // The visible lanes area (device px): left of the headers, below the ruler.
  ui::IRect lanesViewport() const noexcept;
  // [first, last) tick visible in the lanes.
  TickRange visibleTicks() const noexcept;
  // The end of the arrangement's content (last clip end, loop end, last locator), at least 8 bars.
  Ticks songEnd() const noexcept;

  // ---- vertical ----------------------------------------------------------------------------------------------
  int32_t scrollY() const noexcept;
  void scrollToY(int32_t devicePx);
  // Keep a tick / a row in view (arrow-key navigation, T3.x-fix-look): scrolls only when it is outside the view.
  void revealTick(Ticks t);
  void revealRow(int32_t row);

  // ---- Z / X, zoom in / out ----------------------------------------------------------------------------------
  // T3.9 sets the time selection; Z zooms to it (with no selection Z fits the whole arrangement).
  void setTimeSelection(std::optional<TickRange> r);
  const std::optional<TickRange>& timeSelection() const noexcept { return timeSelection_; }
  void zoomToSelection();  // Z: pushes the current view for X
  bool zoomBack();         // X: the view before the last Z (false when there is none)
  void zoomIn();           // = : ×2 around the centre
  void zoomOut();          // - : ×½ around the centre

  // ---- follow ------------------------------------------------------------------------------------------------
  // While playing with follow on, the view pages so the playhead stays visible (Live's Follow, Ctrl+Shift+F).
  bool follow() const noexcept { return follow_; }
  void setFollow(bool on);

  // ---- rows and headers --------------------------------------------------------------------------------------
  std::span<const ArrangementRow> rows() const noexcept;
  // The return tracks and the master, pinned below the scrolling rows (their y is from the top of that area).
  std::span<const ArrangementRow> bottomRows() const noexcept;
  // Device rect of the pinned area (lanes and headers), window coordinates; empty when it has no rows.
  ui::IRect bottomRect() const noexcept;
  // Row index at a device y (in window coordinates), −1 outside the rows.
  int32_t rowAt(float deviceY) const noexcept;
  // Device rect of a row's lane (window coordinates; may lie outside the visible viewport).
  ui::IRect rowLaneRect(int32_t row) const noexcept;
  TrackHeader* header(doc::TrackId id) const noexcept;
  // Folds/unfolds a group (ViewState, not undoable; Q26).
  void toggleFold(doc::TrackId id);
  // A row's height in logical px (ViewState, not undoable; Q26), clamped to [kMinTrackHeight, kMaxTrackHeight].
  void setTrackHeight(doc::TrackId id, float logicalPx);
  float trackHeight(doc::TrackId id) const noexcept;
  // T3.10: header width and the width of each header's mixer section (logical px), and a hook run whenever a
  // header is bound to a track (create or update its controls there).
  void setHeaderWidth(float logicalPx);
  float headerWidth() const noexcept { return headerWidth_; }
  void setHeaderMixerWidth(float logicalPx);
  float headerMixerWidth() const noexcept { return mixerWidth_; }
  using HeaderDecorator = std::function<void(TrackHeader&, const ArrangementRow&)>;
  void setHeaderDecorator(HeaderDecorator d);
  // T3.9: drawn by the lanes after the clips and before the playhead (selection, drag ghosts). Device px, inside
  // the lanes' clip.
  std::function<void(ui::PaintContext&)> onPaintOverlay;

  // ---- editing (T3.9) ----------------------------------------------------------------------------------------
  struct Selection {
    std::vector<doc::ClipRef> clips;   // selected clips; the first is the one shown in the clip view
    std::vector<doc::TrackId> tracks;  // selected tracks (row order): the clips' tracks or the rows a range covers
    std::optional<TickRange> time;     // the time selection; start == end is the insert marker
    bool range = false;                // made by a drag over time (or Shift-extended): edits act on the time range
                                       // of `tracks`, not on whole clips
    bool empty() const noexcept { return clips.empty() && tracks.empty() && !time; }
  };
  const Selection& selection() const noexcept;
  // Replaces the selection (repaints what changed; with showInClipView the first clip goes to the clip view).
  void setSelection(Selection s, bool showInClipView = false);
  // Whole clips: time = their span, tracks = theirs.
  void selectClips(std::span<const doc::ClipRef> clips, bool showInClipView = true);
  void clearSelection();
  bool clipSelected(doc::ClipId id) const noexcept;
  bool dragging() const noexcept;  // a clip move / copy / resize or a range drag is in progress
  // The edits behind the actions (each = one undo step; false when there was nothing to do or it failed, the
  // reason then in the status line).
  bool deleteSelection();
  bool copySelection();
  bool cutSelection();
  bool paste();
  bool duplicateSelection();
  bool splitSelection();
  bool consolidateSelection();
  bool insertMidiClip();
  bool insertTime();
  bool duplicateTime();
  bool deleteTime();
  bool loopSelection();
  bool toggleActive();
  void selectAll();
  // ---- tracks (T3.10; each = one undo step, false when nothing happened, the reason in the status line) --------
  // Selects a track (a header click: Shift adds, Ctrl toggles).
  void selectTrack(doc::TrackId id, ui::Mods mods = 0);
  // Ctrl+Shift+T (Midi) / Ctrl+Alt+T (Return): after the last selected track (returns: after the last selected return),
  // else at the end; the new track is selected and gets the next palette colour.
  bool newTrack(doc::TrackKind kind);
  bool groupSelection();    // Ctrl+G
  bool ungroupSelection();  // Ctrl+Shift+G
  bool deleteTracks();      // the selected tracks (Delete after a header click, the track menu)
  bool renameSelection();   // Ctrl+R: the first selected track's title becomes a text field
  bool setTracksColour(doc::Colour c);  // the selected tracks
  // Right-click on a header: Rename, Insert MIDI / Return Track, Group, Ungroup, Colour…, Delete.
  ui::Menu* openTrackMenu(doc::TrackId id, ui::Vec2 at);
  // The track colour palette (Live's clip / track colours, theme clipPalette) for the selected tracks.
  ui::Widget* openColourPicker(ui::Vec2 at);
  bool hasClipboard() const noexcept;
  // The right-click menu (edit commands, then the grid: Adaptive / Fixed / tuplets / Snap / Off) at device `at`.
  ui::Menu* openContextMenu(ui::Vec2 at);
  // Device rect of a clip's lane box in window coordinates (empty when its track is not a row).
  ui::IRect clipRect(const doc::ClipRef& clip) const noexcept;
  // Double-click on a clip: installArrangement sets this to show the detail view and focus the clip view.
  std::function<void()> onOpenClip;

  // ---- automation (T3.15, pq/views/arrangement/automation.h) ---------------------------------------------------
  ArrangementAutomation& automation() const noexcept { return *automation_; }
  // The automation lanes shown under the rows (y from the top of the track content, as rows()).
  std::span<const AutomationLaneRow> laneRows() const noexcept;
  // Device rect of a lane (window coordinates; may lie outside the visible viewport), and of a pinned row's lane.
  ui::IRect laneRect(size_t laneIndex) const noexcept;
  ui::IRect pinnedRowRect(size_t bottomRow) const noexcept;
  // Rows, lanes and headers are laid out again (lanes shown / hidden, a lane resized).
  void rowsChanged() { rebuildRows(); }

  // ---- transport ---------------------------------------------------------------------------------------------
  // The playhead tick last read from the port while playing (nullopt when stopped).
  const std::optional<Ticks>& playhead() const noexcept { return playhead_; }
  // Locates (the ruler click): start marker + port.locate, `tick` snapped when the grid snaps.
  void locateAt(Ticks tick, bool snapToGrid = true);

  // ---- diagnostics (tests, benchmarks) -------------------------------------------------------------------------
  struct Stats {
    uint64_t rowRebuilds = 0;
    uint64_t previewBuilds = 0;      // note previews built (cache misses)
    uint32_t rowsPainted = 0;        // last lanes paint
    uint32_t clipsPainted = 0;
    uint32_t noteRectsEmitted = 0;
    uint64_t playheadReads = 0;      // TransportPort::playhead() calls made by the view
    uint32_t previewsDeferred = 0;   // last lanes paint: clips drawn without a preview (build budget / zooming)
    uint64_t previewsEvicted = 0;
    // T3.8-fix1, last lanes paint: clips drawn from their exact tile (and their notes), from a stale tile (stretched),
    // and clips that wanted a tile it did not have yet. Totals: tiles rasterised, and tiles that found no room.
    uint32_t tilesDrawn = 0;
    uint32_t tileNotes = 0;
    uint32_t tilesStale = 0;
    uint32_t tilesMissing = 0;
    uint64_t tilesBuilt = 0;
    uint64_t tileFailures = 0;
  };
  const Stats& stats() const noexcept;
  size_t previewCacheSize() const noexcept;
  // True from a zoom change until kZoomSettleSeconds after the last one (dense clips skip their previews).
  bool zooming() const noexcept { return zooming_; }
  void dropPreviews();  // empties the preview cache (benchmarks, tests)
  size_t previewCacheNotes() const noexcept;
  void setPreviewCacheLimit(size_t notes);  // default kPreviewCacheNotes (tests)

  // ---- widget ------------------------------------------------------------------------------------------------
  void arrange(ui::LayoutPass& pass) override;
  void paint(ui::PaintContext& ctx) override;
  void onTimer(uint32_t id) override;
  void onFrame(double now) override;
  void onFocusChanged(bool focused) override;
  bool onKey(const ui::KeyEvent& e) override;

  // Internal (the view's own widgets).
  arrangement::Model& model() const noexcept { return *model_; }
  arrangement::Editor& editor() const noexcept { return *editor_; }
  arrangement::LaneCanvas& lanes() const noexcept { return *lanes_; }
  ui::ScrollView& trackScroll() const noexcept { return *scroll_; }
  void markAxisDirty();  // the horizontal axis changed: ruler, overview and lanes repaint
  void syncHeaderSelection();  // the selected tracks changed: their headers' tint
  const std::vector<TrackHeader*>& headerWidgets() const noexcept { return headers_; }
  const std::vector<arrangement::LaneHeader*>& laneHeaderWidgets() const noexcept { return laneHeaders_; }
  ui::Widget* reenableButton() const noexcept;

private:
  void onDocument(const doc::StoreEvent& e);
  void onViewChange(uint32_t bits);
  void rebuildRows(bool lanesUnchanged = false);
  void syncHeaders();
  void rebindTrackNodes();  // a mixer Param change: new track nodes, same rows (T3.x-fix-paint)
  void refreshTransport();
  void schedulePlayhead();
  void readPlayhead();
  void damagePlayheadColumn(Ticks t);
  void reportResolvedGrid();
  void registerActions();
  void pushZoom();

  ViewContext& ctx_;
  std::unique_ptr<arrangement::Model> model_;
  std::unique_ptr<arrangement::Editor> editor_;
  std::unique_ptr<ArrangementAutomation> automation_;
  ViewContext::Subscription docSub_, viewSub_;
  arrangement::Overview* overview_ = nullptr;
  arrangement::Ruler* ruler_ = nullptr;
  arrangement::TimeRuler* timeRuler_ = nullptr;
  ui::Widget* timeCorner_ = nullptr;
  ui::ScrollView* scroll_ = nullptr;
  arrangement::TrackStack* stack_ = nullptr;
  arrangement::LaneCanvas* lanes_ = nullptr;
  ui::Widget* rulerCorner_ = nullptr;
  ui::Widget* overviewCorner_ = nullptr;
  std::vector<TrackHeader*> headers_;
  std::vector<TrackHeader*> bottomHeaders_;
  std::vector<arrangement::LaneHeader*> laneHeaders_;  // T3.15: one per laneRows() entry
  arrangement::ReenableButton* reenable_ = nullptr;     // T3.15: above the headers, shown while automation is overridden
  arrangement::BottomTracks* bottom_ = nullptr;
  HeaderDecorator decorator_;

  double originTick_ = 0.0;
  double pxPerTick_ = arrangement_metrics::kDefaultPxPerTick;
  float headerWidth_ = arrangement_metrics::kHeaderWidth;
  float mixerWidth_ = arrangement_metrics::kHeaderMixerWidth;
  float rowsScale_ = 0.0f;  // UI scale the rows' device px were computed at
  std::optional<TickRange> timeSelection_;
  struct SavedView {
    double origin, pxPerTick;
  };
  std::vector<SavedView> zoomStack_;
  bool follow_ = true;
  std::optional<Ticks> playhead_;
  uint32_t playheadTimer_ = 0;
  bool zooming_ = false;
  uint32_t settleTimer_ = 0;
};

// Installs the arrangement into ShellSlot::TrackArea (called from installDefaultContent), registers its actions and
// focuses it (the arrangement's key scope is active from the start, like Live's).
ArrangementView& installArrangement(Shell& shell);

}  // namespace pq::views
