#pragma once

// The arrangement view's private parts (T3.8): the row/clip/preview model and the widgets the view is built from.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "pq/document/clip_time.h"
#include "pq/document/edit_ops.h"
#include "pq/document/model.h"
#include "pq/tuning/tuning.h"
#include "pq/ui/text.h"
#include "pq/ui/theme.h"
#include "pq/ui/tile_atlas.h"
#include "pq/ui/widget.h"
#include "pq/views/arrangement/arrangement.h"

namespace pq::views::arrangement {

// A track's clips as sorted arrays (struct of arrays), rebuilt when the track's snapshot pointer changes. Clips are
// sorted by start (the document keeps them so); maxEnd is the running maximum of the ends, so the first clip that
// reaches past a tick is a binary search even if clips overlapped.
struct ClipIndex {
  const doc::Track* track = nullptr;
  std::vector<Ticks> start, end, maxEnd;
  // Index range of the clips meeting [from, to).
  std::pair<size_t, size_t> visible(Ticks from, Ticks to) const noexcept;
};

// One note of a clip's cached preview: content ticks and a vertical position (0 = top = highest pitch).
struct PreviewNote {
  Ticks start = 0;
  Ticks end = 0;
  uint16_t y = 0;      // 0 … 65535 over the clip's pitch range (top → bottom)
  uint16_t flags = 0;  // kPreviewDisabled
  uint32_t pad = 0;
};
inline constexpr uint16_t kPreviewDisabled = 1;

// A clip's preview rendered into the renderer's TileAtlas (T3.8-fix1): a coverage mask in clip-local device px
// (column 0 = the clip's left edge, row 0 = the top of its body) at one zoom and body height. The key is the device
// px per tick and the body height (the clip and tuning are the Preview's own key); `width` follows from them.
struct PreviewTile {
  enum class State : uint8_t { None, Ready, Failed };  // Failed: no room in the atlas at this key (drawn directly)
  ui::TileAtlas::Tile tile;
  double k = 0.0;
  int32_t width = 0;
  int32_t bodyH = 0;
  State state = State::None;
  bool matches(double k2, int32_t bodyH2) const noexcept { return k == k2 && bodyH == bodyH2; }
};

struct Preview {
  doc::ClipPtr clip;                       // keeps the clip alive: the cache key is its address
  const doc::TuningDef* tuning = nullptr;  // the tuning the pitches were resolved in
  std::vector<PreviewNote> notes;          // sorted by start
  Ticks maxLength = 0;                     // longest note (range lookups back off by it)
  float rowFraction = 0.25f;               // note height as a fraction of the body height
  uint64_t lastUsed = 0;                   // Model::paintSerial of the last paint that drew it
  PreviewTile tile;                        // the last tile rendered for it (T3.8-fix1)
};

class Model {
public:
  doc::Snapshot shown;
  std::vector<ArrangementRow> rows;
  // T3.15: the automation lanes shown under the rows (in row order), and every lane of the project by parameter
  // (sorted by ParamRef; rebuilt with the rows).
  std::vector<AutomationLaneRow> laneRows;
  struct LaneEntry {
    doc::ParamRef param;
    doc::TrackId owner;
    const doc::AutomationLane* lane = nullptr;
  };
  std::vector<LaneEntry> laneIndex;
  LaneLookup lane(const doc::ParamRef& r) const noexcept {
    const auto it = std::lower_bound(laneIndex.begin(), laneIndex.end(), r,
                                     [](const LaneEntry& e, const doc::ParamRef& x) { return e.param < x; });
    return (it != laneIndex.end() && it->param == r) ? LaneLookup{it->owner, it->lane} : LaneLookup{};
  }
  int32_t contentHeight = 0;  // device px of all rows
  std::vector<ArrangementRow> bottomRows;  // returns + master (T3.10), y from the top of the pinned area
  int32_t bottomHeight = 0;                // device px of the bottom rows
  Ticks songEnd = 0;          // last clip end / loop end / last locator, at least 8 bars (rebuilt with the rows)
  ArrangementView::Stats stats;

  // The clip index of a track (by id; rebuilt when the track pointer changed).
  const ClipIndex& clips(const doc::Track& t);
  // The preview of a clip resolved in `tuning` (cached by clip pointer + tuning), or nullptr when it is not cached and
  // this paint's build budget is spent (buildsDeferred is then set).
  Preview* preview(const doc::ClipPtr& clip, const doc::TuningDef* tuning);
  // The cached preview, or nullptr (never builds).
  Preview* cachedPreview(const doc::ClipPtr& clip, const doc::TuningDef* tuning) noexcept;
  // Per lanes paint: resets the build budget (0 = unlimited); after it, drops old previews beyond the cache bound.
  void beginPaint(uint32_t buildBudgetNotes) noexcept;
  void endPaint();
  bool buildsDeferred = false;    // a preview was not built for lack of budget (paint again)
  bool skippedForZoom = false;    // a dense clip skipped its preview while zooming (repaint when the zoom settles)
  bool tilesAfterZoom = false;    // a paint while zooming wanted tiles at a new zoom (build them when it settles)
  // T3.8-fix1, per lanes paint: a clip wanted a tile it does not have at this key (build them once the zoom has
  // settled); a clip was drawn approximately (a stale tile stretched, or its body without notes: repaint once built);
  // note rects the fallback for missing tiles emitted.
  bool tilesMissing = false;
  bool approxDrawn = false;
  bool approxForZoom = false;  // ... of which a clip skipped only because the zoom is changing
  uint32_t fallbackRects = 0;
  // After a document change: drops tuning objects and, when the caches outgrew the project, entries for clips and
  // tracks that are gone.
  void afterDocumentChange(size_t clipsInProject);
  void clear();
  size_t previewCount() const noexcept { return previews_.size(); }
  void dropPreviews() noexcept {
    previews_.clear();
    cachedNotes_ = 0;
  }
  size_t cachedNotes() const noexcept { return cachedNotes_; }
  void setCacheLimit(size_t notes) noexcept { cacheLimitNotes_ = notes; }

  // Clip-name colour per clip colour (clipTextFor is not for per-frame use).
  ui::Rgba clipTextColour(const ui::Theme& th, ui::Rgba fill);

  // Paint scratch (kept across frames: painting does not allocate once warm).
  struct Pending {
    int32_t x0 = 0, x1 = 0;  // clip-local columns
    bool open = false;
  };
  std::vector<Pending> pending;     // per pixel row of a clip body
  struct RectOut {
    float x0, y0, x1, y1;
    ui::Rgba col;
  };
  std::vector<RectOut> rects;  // a clip's preview rects, emitted in one reservation
  // Tile quads and the borders of their clips, emitted after the rows (grouped by tile page: one draw command per
  // page instead of two texture switches per clip; T3.8-fix1).
  struct TileQuad {
    uint32_t page;
    float x0, y0, x1, y1;
    float u0, v0, u1, v1;
    ui::Rgba col;
  };
  std::vector<TileQuad> tileQuads;
  std::vector<RectOut> deferredRects;
  std::vector<uint16_t> usedRows;
  std::vector<const doc::Track*> members;  // a group's member tracks (group summaries)

private:
  const tuning::Tuning* tuningFor(const doc::TuningDef* def);

  std::unordered_map<uint32_t, ClipIndex> clipIndex_;            // by raw track id
  std::unordered_map<const doc::NoteClip*, Preview> previews_;
  uint64_t paintSerial_ = 0;
  uint32_t budgetLeft_ = 0;
  bool unlimited_ = true;
  bool builtThisPaint_ = false;
  void recountNotes() noexcept;
  size_t cachedNotes_ = 0;    // notes held by previews_
  size_t cacheLimitNotes_ = arrangement_metrics::kPreviewCacheNotes;
  size_t peakNotes_ = 0;      // most note rects one paint drew since the last trim
  uint64_t nextTrim_ = 0;
  std::unordered_map<const doc::TuningDef*, std::optional<tuning::Tuning>> tunings_;
  std::vector<double> centsScratch_;
  struct TextColour {
    ui::Rgba fill = 0, text = 0;
    const ui::Theme* theme = nullptr;
  };
  TextColour textColours_[16];
  uint32_t textNext_ = 0;
};

// ---- Editing (T3.9) -------------------------------------------------------------------------------------------

enum class DragKind : uint8_t {
  None,
  Move,    // the selected clips by (dt, rows); Alt held = Copy (the originals stay)
  Resize,  // one edge of every selected clip by dt
  Range,   // a time selection over rows (selects the clips it touches)
};

// Where a clip is drawn: its own bounds, or a drag preview's.
struct ClipGeom {
  Ticks start = 0;
  Ticks length = 0;
  Ticks contentStart = 0;
};

// A clip of a Move / Resize drag as it was at the press, and the row it was on.
struct DragClip {
  doc::ClipRef ref;
  doc::ClipPtr clip;
  int32_t row = -1;
};

// In-process clipboard: clips (copies, starts relative to the copied range) and the rows they came from, as offsets
// among the MIDI rows.
struct ClipboardEntry {
  uint32_t rowOffset = 0;
  doc::ClipPtr clip;  // start = offset from the clipboard origin
};
struct Clipboard {
  std::vector<ClipboardEntry> clips;
  Ticks length = 0;  // the copied span (a range, or the clips' extent)
};

class Editor {
public:
  explicit Editor(ArrangementView& view);

  // --- selection ---
  const ArrangementView::Selection& selection() const noexcept { return sel_; }
  void setSelection(ArrangementView::Selection s, bool showInClipView);
  bool selected(doc::ClipId id) const noexcept {
    return !ids_.empty() && std::binary_search(ids_.begin(), ids_.end(), id.value);
  }
  bool trackSelected(doc::TrackId id) const noexcept {
    return !trackIds_.empty() && std::binary_search(trackIds_.begin(), trackIds_.end(), id.value);
  }
  ArrangementView::Selection selectionOfClips(std::vector<doc::ClipRef> clips) const;
  void afterDocumentChange(const doc::StoreEvent& e);

  // --- input (LaneCanvas forwards everything but the wheel) ---
  bool pointer(const ui::PointerEvent& e);
  void cancelDrag();
  void selectTrack(doc::TrackId id, ui::Mods mods);

  // --- paint queries ---
  DragKind dragKind() const noexcept { return drag_.moved ? drag_.kind : DragKind::None; }
  bool dragging() const noexcept { return drag_.kind != DragKind::None; }
  // A clip whose original is not drawn (it is being moved or resized: its preview is drawn instead).
  bool hiddenWhileDragging(doc::ClipId id) const noexcept {
    return drag_.moved && (drag_.kind == DragKind::Resize || (drag_.kind == DragKind::Move && !drag_.copy)) && selected(id);
  }
  // The time selection overlay is hidden while the selected clips are moved or resized (it follows them on
  // release); a copy leaves it where the originals are.
  bool showTimeSelection() const noexcept {
    return !(drag_.moved && (drag_.kind == DragKind::Resize || (drag_.kind == DragKind::Move && !drag_.copy)));
  }
  const std::vector<DragClip>& dragClips() const noexcept { return drag_.clips; }
  // Preview of a dragged clip: target row and geometry (false: not drawn).
  bool preview(const DragClip& c, int32_t& row, ClipGeom& g) const noexcept;

  // --- edits ---
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
  bool hasClipboard() const noexcept { return !clipboard_.clips.empty(); }
  ui::Menu* openContextMenu(ui::Vec2 at);

  // --- tracks (T3.10, tracks.cpp) ---
  bool newTrack(doc::TrackKind kind);
  bool groupSelection();
  bool ungroupSelection();
  bool deleteTracks();
  bool renameSelection();
  bool setTracksColour(doc::Colour c);
  ui::Menu* openTrackMenu(doc::TrackId id, ui::Vec2 at);
  ui::Widget* openColourPicker(ui::Vec2 at);
  // Selects exactly these tracks (after a track edit).
  void selectTracks(std::vector<doc::TrackId> tracks);

  // Device rect of a clip on a row (window coordinates).
  ui::IRect clipBox(int32_t row, Ticks start, Ticks end) const noexcept;

private:
  struct Drag {
    DragKind kind = DragKind::None;
    bool moved = false;
    bool copy = false;
    doc::ops::ClipEdge edge = doc::ops::ClipEdge::End;
    ui::Vec2 press{};
    int32_t pressRow = -1;
    Ticks origin = 0;  // snapping origin (Move: the pressed clip's start; Resize: its edge; Range: the anchor)
    Ticks dt = 0;
    int32_t drow = 0;
    Ticks minDt = 0, maxDt = 0;  // clamps (start ≥ 0, length ≥ 1)
    std::vector<DragClip> clips;
    std::vector<ui::IRect> damage;  // the preview's rects last frame
    // Range
    int32_t anchorRow = -1;
    bool additive = false;
    std::vector<doc::ClipRef> base;
    // A press on a clip of a multiple selection: on release without a move, select just it (Live).
    doc::ClipRef pressed{};
    bool collapseOnClick = false;
  };
  struct Hit {
    int32_t row = -1;
    doc::ClipRef ref{};
    const doc::NoteClip* clip = nullptr;
    int zone = 0;  // −1 left edge, +1 right edge, 0 title strip, 2 body
  };
  Hit hitTest(ui::Vec2 p) const;
  Grid grid() const noexcept;
  Ticks snapTick(Ticks t) const noexcept;
  Ticks tickAt(float x) const noexcept;
  int32_t clampRow(float y) const noexcept;
  int32_t rowOf(doc::TrackId id) const noexcept;
  void press(const ui::PointerEvent& e);
  void dragTo(const ui::PointerEvent& e);
  void release(const ui::PointerEvent& e);
  void beginClipDrag(DragKind kind, const Hit& h);
  void updateRange(ui::Vec2 p);
  void commitDrag();
  void damageDragPreview();
  void damageSelection(const ArrangementView::Selection& a, const ArrangementView::Selection& b);
  void damageRows(const ArrangementView::Selection& s, double t0, double t1, bool fullRows);
  bool spanOf(const ArrangementView::Selection& s, double& t0, double& t1) const;
  // Selected tracks expanded to MIDI tracks (a selected group stands for its MIDI members, folded or not).
  std::vector<doc::TrackId> midiTracks() const;
  // The MIDI rows (row indices of non-group rows).
  std::vector<int32_t> midiRows() const;
  bool dispatch(doc::Batch batch, std::string_view what);
  bool apply(const doc::ops::EditResult& e, std::string_view what, bool adoptClips);
  bool needRange(std::string_view what) const;
  bool fillClipboard(Clipboard& out) const;
  bool pasteAt(const Clipboard& cb, Ticks at, int32_t firstRow, std::string_view what, bool selectRange);

  ArrangementView& view_;
  ViewContext& ctx_;
  ArrangementView::Selection sel_;
  std::vector<uint32_t> ids_;       // selected clip ids, sorted
  std::vector<uint32_t> trackIds_;  // selected track ids, sorted
  Drag drag_;
  Clipboard clipboard_;
  std::optional<TickRange> lastPaste_;  // the range the last paste selected (pasting again goes after it)
};

// The return tracks and the master, pinned under the scrolling rows (T3.10): their lanes (BottomLanes, child 0) and
// headers, placed from ArrangementView::bottomRows().
class BottomTracks : public ui::Widget {
public:
  explicit BottomTracks(ArrangementView& view);
  void arrange(ui::LayoutPass& pass) override;
  void paint(ui::PaintContext& ctx) override;

private:
  ArrangementView& view_;
};

// The pinned rows' lanes: lane colour, bar shading and grid (as the track lanes), selection tint, separators,
// playhead. Wheel as the lanes (Ctrl zooms, Shift scrolls time).
class BottomLanes : public ui::Widget {
public:
  explicit BottomLanes(ArrangementView& view);
  void paint(ui::PaintContext& ctx) override;
  bool onPointer(const ui::PointerEvent& e) override;

private:
  ArrangementView& view_;
};

// Content of the vertical ScrollView: the lane canvas and the headers, placed from the rows.
class TrackStack : public ui::Widget {
public:
  explicit TrackStack(ArrangementView& view);
  ui::ISize contentExtent(ui::ISize viewport) const override;
  void arrange(ui::LayoutPass& pass) override;
  void paint(ui::PaintContext& ctx) override;

private:
  ArrangementView& view_;
};

// Clips, grid, group summaries, playhead: one virtualised widget for all rows.
class LaneCanvas : public ui::Widget {
public:
  explicit LaneCanvas(ArrangementView& view);
  void paint(ui::PaintContext& ctx) override;
  bool onPointer(const ui::PointerEvent& e) override;
  void onCaptureLost() override;
  void onFrame(double now) override;
  // Builds missing previews (and, once the zoom has settled, preview tiles: T3.8-fix1) of the visible clips within
  // one frame's budgets; true when none is missing.
  bool buildAhead();
  // Preview notes built per paint (0 = unlimited).
  void setPreviewBudget(uint32_t notes) noexcept { previewBudget_ = notes; }

private:
  ArrangementView& view_;
  uint32_t previewBudget_ = arrangement_metrics::kPreviewBuildNotesPerPaint;
  bool repaintWhenBuilt_ = false;  // a paint drew a clip approximately: repaint once everything is built
  uint64_t latchBuilds_ = 0;       // previews + tiles built when repaintWhenBuilt_ was set
  uint64_t paintFrame_ = 0;        // PaintContext::frame of the last paint (stats and budgets are per frame)
};

// Loop brace, bar numbers, ticks, locators, tempo / signature markers, start marker, playhead. Click locates,
// drag zooms (vertical) and scrolls (horizontal).
class Ruler : public ui::Control {
public:
  explicit Ruler(ArrangementView& view);
  void paint(ui::PaintContext& ctx) override;
  bool onPointer(const ui::PointerEvent& e) override;
  void onCaptureLost() override { pressed_ = false; }

private:
  ArrangementView& view_;
  struct Marker {
    Ticks tick;
    uint8_t kind;  // 0 locator, 1 time signature, 2 tempo
    uint32_t index;
  };
  std::vector<Marker> markers_;  // paint scratch
  bool pressed_ = false;
  bool dragging_ = false;
  ui::Vec2 pressPos_{};
  double pressTick_ = 0;
  double pressZoom_ = 0;
};

// Minutes:seconds under the tracks (Live's time ruler), from the tempo map.
class TimeRuler : public ui::Control {
public:
  explicit TimeRuler(ArrangementView& view);
  void paint(ui::PaintContext& ctx) override;

private:
  ArrangementView& view_;
};

// The whole song in a strip; the visible region framed. Click / drag moves the view.
class Overview : public ui::Control, private ui::ScrollView::Dependent {
public:
  explicit Overview(ArrangementView& view);
  ~Overview() override;
  // Follows the track scroll view's vertical position (the frame), as its second dependent after the scrollbar.
  void attachScroll(ui::ScrollView& sv);
  void paint(ui::PaintContext& ctx) override;
  bool onPointer(const ui::PointerEvent& e) override;
  void onCaptureLost() override { dragging_ = false; }
  // The tick range the strip shows.
  TickRange range() const noexcept;
  void invalidateCache() noexcept { cacheValid_ = false; }

private:
  void rebuildCache(const ui::IRect& r);
  void onScrollChanged() noexcept override;
  void onScrollViewGone() noexcept override;
  ArrangementView& view_;
  ui::ScrollView* scroll_ = nullptr;
  bool dragging_ = false;
  double grabOffset_ = 0;  // ticks between the pointer and the view origin at press
  // Cached clip blocks (relative to the strip's origin): rebuilt when the document, rows, width or range change.
  struct Block {
    int32_t x0, x1, y0, y1;
    ui::Rgba col;
  };
  std::vector<Block> blocks_;
  std::vector<ui::Rgba> cells_;  // rasterisation scratch (strip pixels)
  bool cacheValid_ = false;
  const void* cacheDoc_ = nullptr;
  uint64_t cacheRows_ = 0;
  ui::IRect cacheRect_{};
  TickRange cacheRange_{};
};

// ---- Automation (T3.15) ----------------------------------------------------------------------------------------

// The header of an automation lane (ArrangementView::laneRows()[index]): automated-state LED, the parameter chooser,
// "+" (another lane) and "−" (hide this one) in the name column; the bottom edge resizes the lane.
class LaneHeader : public ui::Control {
public:
  LaneHeader(std::string name, ArrangementView& view);
  void bind(const AutomationLaneRow& lane, uint16_t depth);
  const AutomationLaneRow& lane() const noexcept { return lane_; }
  // Device rects of the parts (tests).
  ui::IRect chooserRect() const noexcept;
  ui::IRect addRect() const noexcept;
  ui::IRect hideRect() const noexcept;
  void paint(ui::PaintContext& ctx) override;
  bool onPointer(const ui::PointerEvent& e) override;
  void onCaptureLost() override { resizing_ = false; }

private:
  ArrangementView& view_;
  AutomationLaneRow lane_;
  uint16_t depth_ = 0;
  std::string label_;
  bool automated_ = false;
  bool overridden_ = false;
  bool resizing_ = false;
  float resizeStartY_ = 0.0f;
  float resizeStartH_ = 0.0f;
};

// The track header's parameter chooser in automation mode (Live's control chooser): the parameter drawn over the
// track's clips.
class ParamChooser : public ui::Control {
public:
  explicit ParamChooser(ArrangementView& view);
  void set(doc::TrackId track, std::string label, bool automated, bool overridden);
  void paint(ui::PaintContext& ctx) override;
  bool onPointer(const ui::PointerEvent& e) override;

private:
  ArrangementView& view_;
  doc::TrackId track_;
  std::string label_;
  bool automated_ = false;
  bool overridden_ = false;
};

// The header's lanes toggle (▸ hidden / ▾ shown).
class LanesToggle : public ui::Control {
public:
  explicit LanesToggle(ArrangementView& view);
  void set(doc::TrackId track, bool shown);
  void paint(ui::PaintContext& ctx) override;
  bool onPointer(const ui::PointerEvent& e) override;

private:
  ArrangementView& view_;
  doc::TrackId track_;
  bool shown_ = false;
};

// "Re-enable Automation" above the track headers: shown while an automated parameter is overridden.
class ReenableButton : public ui::Control {
public:
  explicit ReenableButton(ArrangementView& view);
  void paint(ui::PaintContext& ctx) override;
  bool onPointer(const ui::PointerEvent& e) override;

private:
  ArrangementView& view_;
};

// A filler (panel background) above the header column.
class Corner : public ui::Widget {
public:
  Corner() = default;
  void paint(ui::PaintContext& ctx) override;
};

// The group fold triangle in a header's title bar.
class FoldButton : public ui::Control {
public:
  FoldButton(ArrangementView& view);
  void setTrack(doc::TrackId id, bool folded);
  void paint(ui::PaintContext& ctx) override;
  bool onPointer(const ui::PointerEvent& e) override;

private:
  ArrangementView& view_;
  doc::TrackId id_;
  bool folded_ = false;
};

// Draws the glyphs of `s` (shaped once, cached by the text system) that end at or before `maxRight`: Live cuts clip
// names at the clip edge without an ellipsis. Unlike shapeTruncated this never reshapes when the width changes
// (zooming), and it needs no clip push (which would split draw commands per clip). Returns the advance.
inline int32_t drawTextUpTo(ui::DrawList& dl, ui::TextSystem& ts, ui::FontId font, std::string_view s, float x, float baseline,
                            float maxRight, ui::Rgba col) {
  if (s.empty() || font == ui::kNoFont || maxRight <= x) return 0;
  ui::TextRun run = ts.shape(font, s);
  const float ox = std::floor(x + 0.5f);
  size_t n = 0;
  while (n < run.glyphs.size() && ox + static_cast<float>(run.glyphs[n].x0 + run.glyphs[n].w) <= maxRight) ++n;
  if (n == 0) return 0;
  run.glyphs = run.glyphs.first(n);
  ui::TextSystem::drawRun(dl, run, {x, baseline}, col);
  return run.advance;
}

// Colours.
inline ui::Rgba rgbOf(doc::Colour c) noexcept { return ui::hex(c.rgb); }
// Clip body and note colours derived from the clip colour (Live: notes dark on a lighter body).
inline ui::Rgba clipBodyColour(ui::Rgba clip) noexcept { return ui::lerp(clip, ui::rgba(30, 30, 30), 0.30f); }
inline ui::Rgba clipNoteColour(ui::Rgba clip) noexcept { return ui::lerp(clip, ui::rgba(0, 0, 0), 0.78f); }
inline ui::Rgba inactiveColour() noexcept { return ui::rgba(112, 112, 112); }
// Whether a track's clips are drawn active: deactivated or muted (its activator off, T3.10) tracks draw grey, as in
// Live.
inline bool sounds(const doc::Track& t) noexcept { return t.active && !t.mixer.mute; }

}  // namespace pq::views::arrangement
