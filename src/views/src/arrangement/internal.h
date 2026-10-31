#pragma once

// The arrangement view's private parts (T3.8): the row/clip/preview model and the widgets the view is built from.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "pq/core/clip_time.h"
#include "pq/document/audio_clip.h"
#include "pq/document/clip_time.h"
#include "pq/document/edit_ops.h"
#include "pq/document/model.h"
#include "pq/tuning/tuning.h"
#include "pq/ui/text.h"
#include "pq/ui/theme.h"
#include "pq/ui/draw_list.h"
#include "pq/ui/tile_atlas.h"
#include "pq/ui/widget.h"
#include "pq/views/arrangement/arrangement.h"
#include "pq/views/sample_drop.h"

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

// T5.6: an audio clip's waveform tile (as PreviewTile, keyed also by how much of the overview was built).
struct AudioTile {
  ui::TileAtlas::Tile tile;
  double k = 0.0;
  int32_t width = 0;
  int32_t bodyH = 0;
  uint64_t built = 0;  // the overview's builtFrames when rendered
  PreviewTile::State state = PreviewTile::State::None;
  bool matches(double k2, int32_t bodyH2, uint64_t built2) const noexcept { return k == k2 && bodyH == bodyH2 && built == built2; }
};

// T5.6: how an audio clip's arrangement span maps to source frames, resolved once per clip snapshot and tempo map
// (the Steps transpose resolves through the clip's tuning, which is not per-paint work), and its tile.
struct AudioLayout {
  doc::AudioClipPtr clip;           // key (its address) and keeps it alive
  const TempoMap* tempo = nullptr;  // key
  double rate = 48000.0;            // the sample's rate
  int64_t frames = 1;
  uint32_t channels = 1;
  double ratio = 1.0;               // Off: transpose x detune speed ratio
  double fpt = 0.0;                 // RePitch: source frames per content tick
  double startSec = 0.0;            // Off: tau(start)
  bool constantTempo = true;        // Off: one tempo over the whole song (the clip is one linear piece)
  uint64_t lastUsed = 0;
  AudioTile tile;
};
// The layout of `clip` on `track` (no cache: drag previews, the clip view).
AudioLayout makeAudioLayout(const doc::Project& p, doc::TrackId track, const doc::AudioClipPtr& clip);

// Linear gain of a clip gain in dB (the floor is silence, as the clip player plays it).
inline float clipGainLinear(float db) noexcept {
  return db <= doc::kMinClipGainDb ? 0.0f : static_cast<float>(std::pow(10.0, static_cast<double>(db) / 20.0));
}

// One linear piece of an audio clip's waveform: clip-local device px [xa, xb) show source frames [fa, fb) (mirrored,
// fb at xa, when reverse).
struct AudioPiece {
  double xa = 0, xb = 0;
  double fa = 0, fb = 0;
  bool reverse = false;
};

// Calls f(const AudioPiece&) for the pieces of clip `c` (layout L, device px per tick k) meeting the clip-local window
// [xFrom, xTo). Off: one piece under a single tempo, else pieces of 32 px whose ends are exact (the tempo map); RePitch:
// one per pass over the content (the loop), mirrored for a reversed clip.
template <class F>
void forEachAudioPiece(const doc::AudioClip& c, const AudioLayout& L, const TempoMap& tm, double k, double xFrom, double xTo, F&& f) {
  const double W = static_cast<double>(c.length) * k;
  xFrom = std::max(0.0, xFrom);
  xTo = std::min(W, xTo);
  if (!(xTo > xFrom) || !(k > 0.0)) return;
  if (c.warp == doc::AudioWarp::Off) {
    const double speed = L.rate * L.ratio;
    const double total = c.seconds * speed;
    auto offAt = [&](double x) {  // source frames after the content start at local x (forward)
      const double tick = static_cast<double>(c.start) + x / k;
      const double t0 = std::floor(tick);
      const double s0 = tm.ticksToSeconds(static_cast<Ticks>(t0)), s1 = tm.ticksToSeconds(static_cast<Ticks>(t0) + 1);
      return (s0 + (s1 - s0) * (tick - t0) - L.startSec) * speed;
    };
    auto emit = [&](double xa, double xb) {
      const double oa = offAt(xa), ob = offAt(xb);
      if (!c.reverse) f(AudioPiece{xa, xb, c.contentStartFrame + oa, c.contentStartFrame + ob, false});
      else f(AudioPiece{xa, xb, c.contentStartFrame + total - ob, c.contentStartFrame + total - oa, true});
    };
    if (L.constantTempo) {
      emit(xFrom, xTo);
      return;
    }
    constexpr double kStep = 32.0;
    for (double xa = std::floor(xFrom / kStep) * kStep; xa < xTo; xa += kStep) emit(std::max(xa, xFrom), std::min(xa + kStep, xTo));
    return;
  }
  // RePitch: content ticks to frames; the window mirrored for a reversed clip.
  const double a = c.reverse ? W - xTo : xFrom, b = c.reverse ? W - xFrom : xTo;
  const Ticks from = c.start + static_cast<Ticks>(std::floor(a / k)), to = c.start + static_cast<Ticks>(std::ceil(b / k)) + 1;
  forEachClipSegment(c.start, c.length, c.contentStart, doc::contentLoop(c), from, to, [&](const ClipSegment& seg) {
    const double xa = static_cast<double>(seg.at - c.start) * k;
    const double xb = xa + static_cast<double>(seg.contentTo - seg.contentFrom) * k;
    const double fa = static_cast<double>(seg.contentFrom) * L.fpt, fb = static_cast<double>(seg.contentTo) * L.fpt;
    if (!c.reverse) f(AudioPiece{xa, xb, fa, fb, false});
    else f(AudioPiece{W - xb, W - xa, fa, fb, true});
  });
}

// Fade lengths in device px as the clip player plays them (clamped: the fade-in first, then the fade-out).
struct AudioFadesPx {
  float in = 0.0f, out = 0.0f;
};
inline AudioFadesPx audioFadesPx(const doc::AudioClip& c, double k) noexcept {
  const Ticks fi = std::clamp<Ticks>(c.fadeIn.length, 0, c.length);
  const Ticks fo = std::clamp<Ticks>(c.fadeOut.length, 0, c.length - fi);
  return {static_cast<float>(static_cast<double>(fi) * k), static_cast<float>(static_cast<double>(fo) * k)};
}

// The audio handles (hit zones beyond Editor::Hit's -1 / 0 / +1 / 2): the fade-in and fade-out handles at the top of
// the body where each fade ends (the crossfade's end for a crossfaded start), the gain handle at the top centre.
inline constexpr int kZoneFadeIn = 3;
inline constexpr int kZoneFadeOut = 4;
inline constexpr int kZoneGain = 5;
struct AudioHandleRects {
  ui::Rect fadeIn{}, fadeOut{}, gain{};
  bool show = false;  // the clip is wide and tall enough for handles
};
inline AudioHandleRects audioHandles(const doc::AudioClip& c, float cx0, float cx1, float by0, float by1, double k, float scale) noexcept {
  AudioHandleRects h;
  const float hs = std::max(4.0f, std::round(6.0f * scale));
  if (cx1 - cx0 < 5.0f * hs || by1 - by0 < hs + 2.0f) return h;
  h.show = true;
  const AudioFadesPx f = audioFadesPx(c, k);
  float xin = c.crossfadeIn > 0 ? cx0 + static_cast<float>(static_cast<double>(c.crossfadeIn - c.crossfadeIn / 2) * k) : cx0 + f.in;
  xin = std::clamp(xin, cx0 + 0.5f * hs, cx1 - 1.5f * hs);
  const float xout = std::clamp(cx1 - f.out, xin + hs, cx1 - 0.5f * hs);
  const float x0 = std::round(xin - 0.5f * hs), x1 = std::round(xout - 0.5f * hs);
  h.fadeIn = {x0, by0, x0 + hs, by0 + hs};
  h.fadeOut = {x1, by0, x1 + hs, by0 + hs};
  const float mid = std::round(0.5f * (cx0 + cx1)), gw = std::round(2.5f * hs);
  h.gain = {mid - std::round(0.5f * gw), by0, mid - std::round(0.5f * gw) + gw, by0 + std::max(3.0f, std::round(0.6f * hs))};
  return h;
}

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

  // T5.6: the audio layout of a clip on a row (cached by clip snapshot and tempo map).
  AudioLayout& audioLayout(const ArrangementRow& row, const doc::AudioClipPtr& clip);
  AudioLayout* cachedAudioLayout(const doc::AudioClip* clip) noexcept;
  size_t audioLayoutCount() const noexcept { return audio_.size(); }
  // Overlay geometry (fade curves, crossfades, handles) of clips whose waveform is a queued tile: appended to the
  // draw list by flushTiles after the quads. Untextured geometry only.
  ui::DrawList overlay;

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
  std::unordered_map<const doc::AudioClip*, AudioLayout> audio_;  // T5.6
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
  // T5.6, audio clips (one store gesture each, edited live: the document follows the pointer, one undo step):
  Fade,    // a fade handle: its length (Alt at the press: its curve); past the edge onto an adjacent clip: a crossfade
  Gain,    // the gain handle: vertical, Shift fine
};

// Where a clip is drawn: its own bounds, or a drag preview's.
struct ClipGeom {
  Ticks start = 0;
  Ticks length = 0;
  Ticks contentStart = 0;
};

// A clip of a Move / Resize drag as it was at the press, and the row it was on (T5.6: a note or an audio clip).
struct DragClip {
  doc::ClipRef ref;
  doc::ClipSlot clip;
  int32_t row = -1;
};

// In-process clipboard: clips (copies, starts relative to the copied range) and the rows they came from, as offsets
// among the clip rows (MIDI and audio tracks; T5.6: a clip pastes only onto a track of its kind).
struct ClipboardEntry {
  uint32_t rowOffset = 0;
  doc::ClipSlot clip;  // start = offset from the clipboard origin
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
  // T5.12: the clips being moved leave the arrangement: a Clips drag session (pq/ui/drag.h) takes over. False when none
  // could start (the move goes on).
  bool dragClipsOut();
  // The user abandoned the drag (Esc, capture or window focus lost: T3.x-fix-edit, M3 review L19): as cancelDrag, and
  // a time-selection drag puts back the selection from before its press (not the partial band).
  void abortDrag();
  void selectTrack(doc::TrackId id, ui::Mods mods);
  // Arrow keys (T3.x-fix-look): the insert marker one grid cell left / right (from the time selection's start, else
  // the start marker; the selected tracks stay), the track selection one row up / down.
  void nudgeMarker(int32_t cells);
  void stepTrack(int32_t rows);
  // T4.16b keys (Live): whether Left / Right move the selected clips (clips selected, not a range drag) instead of
  // the marker; the clips by grid cells / by drag::kFineNudge (Alt) off the grid (one undo step each); Shift+Left /
  // Right move the time selection's end by a grid cell (the clips it then touches are selected, as a range drag);
  // Esc drops the clips and the time selection (the tracks stay selected).
  bool clipsNudgeable() const noexcept { return !sel_.clips.empty() && !sel_.range; }
  bool nudgeClips(int32_t cells);
  bool nudgeClipsFine(int32_t dir);
  void extendTime(int32_t cells);
  bool deselect();

  // --- affordances (T4.16b) ---
  // The cursor for the pointer at `p`: a drag's own, else what a press there would do (hitTest, as press()).
  ui::Cursor cursorAt(ui::Vec2 p) const;
  // The clip part a press would grab, highlighted (Live): the title strip (move) or an edge (resize). zone as Hit
  // (−1 / +1 edges, 0 title; the body selects time and is not highlighted). A change damages the two clips' boxes.
  struct HoverPart {
    doc::ClipRef ref{};
    int8_t zone = 0;
    bool operator==(const HoverPart& o) const noexcept { return ref.clip == o.ref.clip && ref.track == o.ref.track && zone == o.zone; }
  };
  const HoverPart& hoverPart() const noexcept { return hover_; }
  void hoverAt(ui::Vec2 p);
  void clearHover();

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
    // T5.6 Fade / Gain: the clip, its state at the press, which handle, and how (Alt: the curve).
    doc::AudioClipPtr audio;
    int zone = 0;
    bool curve = false;
  };
  struct Hit {
    int32_t row = -1;
    doc::ClipRef ref{};
    const doc::ClipSlot* clip = nullptr;
    int zone = 0;  // −1 left edge, +1 right edge, 0 title strip, 2 body; T5.6 audio: kZoneFadeIn / FadeOut / Gain
  };
  Hit hitTest(ui::Vec2 p) const;
  void setHover(HoverPart h);
  // Clips of rows r0 … r1 (with selectedTracksOnly: only the selected tracks' rows) meeting [s, e), plus `base`;
  // sorted and de-duplicated by id.
  std::vector<doc::ClipRef> clipsMeeting(int32_t r0, int32_t r1, bool selectedTracksOnly, Ticks s, Ticks e,
                                         std::vector<doc::ClipRef> base) const;
  Grid grid() const noexcept;
  Ticks snapTick(Ticks t) const noexcept;
  Ticks tickAt(float x) const noexcept;
  int32_t clampRow(float y) const noexcept;
  int32_t rowOf(doc::TrackId id) const noexcept;
  void press(const ui::PointerEvent& e);
  void dragTo(const ui::PointerEvent& e);
  void release(const ui::PointerEvent& e);
  void beginClipDrag(DragKind kind, const Hit& h);
  // T5.6: the audio handles (one store gesture per drag).
  void beginAudioDrag(const Hit& h, ui::Mods mods);
  void audioDragTo(const ui::PointerEvent& e);
  // Trim limits of an audio clip (warp Off / unlooped RePitch: the file's ends), as deltas of its start / end edge.
  void audioTrimLimits(const doc::AudioClip& c, Ticks& minStartDt, Ticks& maxEndDt) const;
  void updateRange(ui::Vec2 p);
  void commitDrag();
  void damageDragPreview();
  void damageSelection(const ArrangementView::Selection& a, const ArrangementView::Selection& b);
  void damageRows(const ArrangementView::Selection& s, double t0, double t1, bool fullRows);
  bool spanOf(const ArrangementView::Selection& s, double& t0, double& t1) const;
  // Selected tracks expanded to MIDI tracks (a selected group stands for its MIDI members, folded or not).
  std::vector<doc::TrackId> midiTracks() const;
  // T5.6: the same with audio tracks too (the tracks clips live on).
  std::vector<doc::TrackId> clipTracks() const;
  // The clip rows (row indices of non-group rows: MIDI and audio tracks).
  std::vector<int32_t> midiRows() const;
  bool dispatch(doc::Batch batch, std::string_view what);
  bool apply(const doc::ops::EditResult& e, std::string_view what, bool adoptClips);
  bool needRange(std::string_view what) const;
  bool fillClipboard(Clipboard& out) const;
  bool pasteAt(const Clipboard& cb, Ticks at, int32_t firstRow, std::string_view what, bool selectRange);

  ArrangementView& view_;
  ViewContext& ctx_;
  ArrangementView::Selection sel_;
  ArrangementView::Selection pressSel_;  // the selection when the current press began (abortDrag restores it)
  std::vector<uint32_t> ids_;       // selected clip ids, sorted
  std::vector<uint32_t> trackIds_;  // selected track ids, sorted
  Drag drag_;
  HoverPart hover_;
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
  void onCaptureLost() override;
  void onHoverChanged(bool hovered) override;
  ui::Cursor cursorAt(ui::Vec2 p) const override;  // T4.16b: the automation-mode envelopes' parts

private:
  ArrangementView& view_;
  ui::Mods mods_ = ui::mod::kNone;  // of the last pointer event (the cursor's modifier variants)
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
struct LanePaint;

// T5.7: files from Explorer and browser items dropped onto the lanes (pq/views/sample_drop.h has the rules). While a
// payload hovers, its audio files are probed (FileProbes) and drawn as ghost clips at their probed lengths where the
// drop would put them (new tracks: under an insertion line); other kinds explain themselves in the status line
// (MIDI, presets) or, for folders, become browser places. The drop commits once every audio file is probed: at once
// when they are, else when the last probe arrives (the status line says so meanwhile).
class FileDrop {
public:
  explicit FileDrop(ArrangementView& view);
  ~FileDrop();
  ui::DropFeedback over(const ui::DragPayload& p, ui::Vec2 pos, ui::Mods mods);
  void leave();
  ui::DropEffect drop(const ui::DragPayload& p, ui::Vec2 pos, ui::Mods mods);
  // Ghost clips and the insertion line (after the clips, before the overlay).
  void paint(LanePaint& lp);
  bool hovering() const noexcept { return hovering_; }
  bool committing() const noexcept { return pendingCommit_; }
  const SampleDropPlan& plan() const noexcept { return plan_; }
  // Horizontal auto-scroll near the lanes' left / right edges (the track scroll view does the vertical).
  bool autoScroll(ui::Vec2 pos, double dt);

private:
  void classify(const ui::DragPayload& p);
  void retarget(ui::Vec2 pos, ui::Mods mods);
  void replan();
  void damageGhosts();
  ui::IRect ghostBounds() const;
  void commit();
  void probesArrived();
  // The track rows' local y (device px from the lanes' top) where new tracks are inserted, and the target row.
  int32_t insertY() const;

  ArrangementView& view_;
  std::vector<std::string> paths_;   // the hovering payload's paths (a new session when they change)
  std::vector<std::string> audio_;   // its audio files, in order
  std::vector<std::string> folders_;
  uint32_t midi_ = 0, presets_ = 0, others_ = 0;
  std::vector<ProbedFile> probed_;   // of audio_, refreshed on every move / probe arrival
  SampleDropTarget target_;
  int32_t row_ = -1;                 // the target row (−1: below the last track)
  Ticks at_ = 0;
  bool oneTrack_ = false;
  bool hovering_ = false;
  bool pendingCommit_ = false;       // dropped before every probe finished: commit when they have
  SampleDropPlan plan_;
  ui::IRect painted_{};              // the ghosts' bounds as last damaged
  float autoScrollAcc_ = 0;
};

class LaneCanvas : public ui::Widget {
public:
  explicit LaneCanvas(ArrangementView& view);
  // T5.7: drops (FileDrop).
  ui::DropFeedback dragOver(const ui::DragPayload& p, ui::Vec2 pos, ui::Mods mods) override;
  void dragLeave() override;
  ui::DropEffect drop(const ui::DragPayload& p, ui::Vec2 pos, ui::Mods mods) override;
  bool autoScrollDrag(ui::Vec2 pos, double dt) override;
  void paint(ui::PaintContext& ctx) override;
  bool onPointer(const ui::PointerEvent& e) override;
  void onCaptureLost() override;
  void onFrame(double now) override;
  void onHoverChanged(bool hovered) override;
  // T4.16b: the cursor of what a press would do (the automation's parts first, as onPointer asks them first), and
  // the hover highlight of the clip part / breakpoint a press would grab.
  ui::Cursor cursorAt(ui::Vec2 p) const override;
  // The modifiers changed with the pointer still (a key event in the arrangement): Alt turns a breakpoint press into
  // a curve drag, Ctrl / Shift a line press into a selection, so the hover follows.
  void modsChanged(ui::Mods m);
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
  ui::Mods mods_ = ui::mod::kNone;     // held at the last pointer / key event
  std::optional<ui::Vec2> pointer_;    // the pointer over the canvas (none: outside)
  void updateHover();
};

// Loop brace, bar numbers, ticks, locators, tempo / signature markers, start marker, playhead. Click locates,
// drag zooms (vertical) and scrolls (horizontal). T4.16b: the loop brace's ends and the brace itself, and the
// locators, drag (snapped relative to the grid, previewed, one undo step on release; Esc cancels).
class Ruler : public ui::Control {
public:
  explicit Ruler(ArrangementView& view);
  void paint(ui::PaintContext& ctx) override;
  bool onPointer(const ui::PointerEvent& e) override;
  void onCaptureLost() override { cancelDrag(); }
  ui::Cursor cursorAt(ui::Vec2 p) const override;
  // Esc: drops a brace / locator drag (true when one ran).
  bool cancelDrag();
  // What a press at `p` grabs.
  enum class Grab : uint8_t { Zoom, LoopStart, LoopEnd, Loop, Locator };
  struct Hit {
    Grab grab = Grab::Zoom;
    uint32_t locator = 0;  // index into the project's locators (Grab::Locator)
  };
  Hit hitAt(ui::Vec2 p) const;
  // The loop and a locator's tick as drawn now (a drag's preview applied).
  doc::LoopRegion shownLoop() const noexcept;
  Ticks shownLocator(uint32_t index) const noexcept;

private:
  ArrangementView& view_;
  Hit grab_{};
  Ticks dt_ = 0;  // the dragged part's snapped delta
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
  ui::Cursor cursorAt(ui::Vec2 p) const override;  // T4.16b: ↕ at the bottom edge, Pointer over its buttons
  bool onResizeEdge(ui::Vec2 p) const noexcept;

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
