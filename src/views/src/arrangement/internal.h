#pragma once

// The arrangement view's private parts (T3.8): the row/clip/preview model and the widgets the view is built from.

#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "pq/document/clip_time.h"
#include "pq/document/model.h"
#include "pq/tuning/tuning.h"
#include "pq/ui/text.h"
#include "pq/ui/theme.h"
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

struct Preview {
  doc::ClipPtr clip;                       // keeps the clip alive: the cache key is its address
  const doc::TuningDef* tuning = nullptr;  // the tuning the pitches were resolved in
  std::vector<PreviewNote> notes;          // sorted by start
  Ticks maxLength = 0;                     // longest note (range lookups back off by it)
  float rowFraction = 0.25f;               // note height as a fraction of the body height
};

class Model {
public:
  doc::Snapshot shown;
  std::vector<ArrangementRow> rows;
  int32_t contentHeight = 0;  // device px of all rows
  Ticks songEnd = 0;          // last clip end / loop end / last locator, at least 8 bars (rebuilt with the rows)
  ArrangementView::Stats stats;

  // The clip index of a track (by id; rebuilt when the track pointer changed).
  const ClipIndex& clips(const doc::Track& t);
  // The preview of a clip resolved in `tuning` (cached by clip pointer + tuning).
  const Preview& preview(const doc::ClipPtr& clip, const doc::TuningDef* tuning);
  // After a document change: drops tuning objects and, when the caches outgrew the project, entries for clips and
  // tracks that are gone.
  void afterDocumentChange(size_t clipsInProject);
  void clear();
  size_t previewCount() const noexcept { return previews_.size(); }

  // Clip-name colour per clip colour (clipTextFor is not for per-frame use).
  ui::Rgba clipTextColour(const ui::Theme& th, ui::Rgba fill);

  // Paint scratch (kept across frames: painting does not allocate once warm).
  struct Pending {
    float x0 = 0, x1 = 0;
    bool open = false;
  };
  std::vector<Pending> pending;     // per pixel row of a clip body
  std::vector<uint16_t> usedRows;
  std::vector<const doc::Track*> members;  // a group's member tracks (group summaries)

private:
  const tuning::Tuning* tuningFor(const doc::TuningDef* def);

  std::unordered_map<uint32_t, ClipIndex> clipIndex_;            // by raw track id
  std::unordered_map<const doc::NoteClip*, Preview> previews_;
  std::unordered_map<const doc::TuningDef*, std::optional<tuning::Tuning>> tunings_;
  std::vector<double> centsScratch_;
  struct TextColour {
    ui::Rgba fill = 0, text = 0;
    const ui::Theme* theme = nullptr;
  };
  TextColour textColours_[16];
  uint32_t textNext_ = 0;
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

private:
  ArrangementView& view_;
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

}  // namespace pq::views::arrangement
