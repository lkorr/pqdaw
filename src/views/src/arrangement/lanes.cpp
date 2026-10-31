// The arrangement's track content (T3.8): TrackStack (lanes + headers in the vertical scroll view) and LaneCanvas
// (grid, clips with their cached note previews, group summaries, playhead) — one virtualised widget for all rows.

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <optional>
#include <span>

#include "internal.h"
#include "lane_paint.h"

namespace pq::views::arrangement {

using namespace pq::ui;
namespace am = arrangement_metrics;

namespace {

int32_t lanesWidth(const IRect& r, const UiScale& s, float headerW) {
  return std::max(0, r.w - s.px(headerW) - std::max(1, s.px(1.0f)));
}

// Ticks of one cell of a resolved grid division in signature `sig` (for density decisions only).
double cellTicks(const GridDivision& d, TimeSignature sig) noexcept {
  double t = d.bars > 0 ? static_cast<double>(d.bars) * static_cast<double>(ticksPerBar(sig))
                        : (d.denominator > 0 ? static_cast<double>(kTicksPerWhole) / d.denominator : 0.0);
  if (d.tuplet > 1) t *= static_cast<double>(std::bit_floor(static_cast<uint32_t>(d.tuplet))) / d.tuplet;
  return t;
}

// The tuning a clip's notes resolve in: its override, else the track's.
const doc::TuningDef* clipTuning(const ArrangementRow& row, const doc::NoteClip& clip, const doc::Project& p) {
  if (clip.tuning.mode == doc::TuningMode::Private && clip.tuning.privateDef) return clip.tuning.privateDef.get();
  if (clip.tuning.mode != doc::TuningMode::Inherit)
    if (const doc::TuningSlot* s = doc::findTuning(p, clip.tuning.main)) return &s->def;
  return row.tuning;
}

}  // namespace

// Solid rects in one reservation (the note preview: thousands per frame when zoomed out).
void emitRects(DrawList& dl, std::span<const Model::RectOut> rs) {
  size_t n = 0;
  for (const Model::RectOut& r : rs) n += (r.x1 > r.x0 && r.y1 > r.y0) ? 1 : 0;
  if (n == 0) return;
  const DrawList::PrimSpan sp = dl.primReserveSpan(static_cast<uint32_t>(6 * n), static_cast<uint32_t>(4 * n));
  const Vec2 uv = dl.whiteUv();
  Vertex* v = sp.v;
  uint32_t* ix = sp.i;
  uint32_t base = sp.base;
  for (const Model::RectOut& r : rs) {
    if (!(r.x1 > r.x0 && r.y1 > r.y0)) continue;
    v[0] = {{r.x0, r.y0}, uv, r.col};
    v[1] = {{r.x1, r.y0}, uv, r.col};
    v[2] = {{r.x1, r.y1}, uv, r.col};
    v[3] = {{r.x0, r.y1}, uv, r.col};
    ix[0] = base;
    ix[1] = base + 1;
    ix[2] = base + 2;
    ix[3] = base;
    ix[4] = base + 2;
    ix[5] = base + 3;
    v += 4;
    ix += 6;
    base += 4;
  }
}

namespace {

// --- Note runs (T3.8-fix1) ---------------------------------------------------------------------------------
// A clip's preview is generated in clip-local device px: column 0 = the clip's left edge, row 0 = the top of its
// body. The direct path (one rect per run, offset by the clip's on-screen left edge) and the tile path (rasterised
// once into the TileAtlas, drawn as one quad) share this generator, so both draw exactly the same pixels, and a tile
// stays valid while the clip scrolls (positions do not depend on the view origin's fraction of a pixel).

enum class RunKind : uint8_t { Note, Disabled, Seam };

struct RunGeom {
  double k = 0.0;     // device px per tick
  int32_t width = 0;  // columns the notes may cover: [0, width)
  int32_t bodyH = 0;  // rows of the body
  int32_t noteH = 0;  // rows of a note
};

// Nearest column (ties up), clamped far outside any canvas so huge clips at deep zoom stay in range.
inline int32_t roundCol(double v) noexcept {
  return static_cast<int32_t>(std::clamp(std::floor(v + 0.5), -1073741824.0, 1073741824.0));
}

int32_t localWidth(Ticks length, double k) noexcept { return std::max<int32_t>(1, roundCol(static_cast<double>(length) * k)); }

RunGeom runGeom(const Preview& pv, Ticks length, double k, int32_t bodyH) noexcept {
  const int32_t noteH = std::clamp(static_cast<int32_t>(std::lround(static_cast<float>(bodyH) * pv.rowFraction)), 1, bodyH);
  return {k, localWidth(length, k), bodyH, noteH};
}

// Calls sink(x0, x1, row, kind) for every run of the preview of a clip drawn at `start` with `timing`'s length,
// content start and loop, for the passes meeting the tick window [from, to): notes as [x0, x1) columns noteH rows tall
// from `row` (overlapping notes on one pixel row merged), disabled notes unmerged, loop seams one column, all rows.
template <class Sink>
void forEachNoteRun(const Preview& pv, const doc::NoteClip& timing, Ticks start, Ticks length, const RunGeom& geo, Ticks from,
                    Ticks to, Model& m, Sink&& sink) {
  const auto span = static_cast<uint32_t>(geo.bodyH - geo.noteH);
  auto& pending = m.pending;
  auto& used = m.usedRows;
  if (pending.size() < static_cast<size_t>(geo.bodyH) + 1) pending.resize(static_cast<size_t>(geo.bodyH) + 1);
  used.clear();
  auto flush = [&](uint16_t row) {
    Model::Pending& pr = pending[row];
    if (!pr.open) return;
    pr.open = false;
    sink(pr.x0, pr.x1, row, RunKind::Note);
  };
  const double k = geo.k;
  bool firstSeg = true;
  // Passes over the content from the clip start (not from the window: a note that starts left of the window still
  // shows); passes left of the window are skipped before touching notes.
  doc::forEachClipSegment(timing, start, std::min(start + length, to), [&](const doc::ClipSegment& seg) {
    const Ticks segEnd = seg.at + (seg.contentTo - seg.contentFrom);
    const bool seam = !firstSeg;
    firstSeg = false;
    if (segEnd < from) return;
    const double segL = static_cast<double>(seg.at - start) * k;
    if (seam) {
      const int32_t sx = roundCol(std::floor(segL));
      if (sx >= 0 && sx < geo.width) sink(sx, sx + 1, uint16_t{0}, RunKind::Seam);
    }
    const Ticks visFrom = seg.contentFrom + std::max<Ticks>(0, from - seg.at);
    const Ticks lookFrom = std::max(seg.contentFrom, visFrom - pv.maxLength);
    const Ticks visTo = std::min(seg.contentTo, seg.contentFrom + (to - seg.at));
    auto it = std::lower_bound(pv.notes.begin(), pv.notes.end(), lookFrom, [](const PreviewNote& n, Ticks t) { return n.start < t; });
    for (; it != pv.notes.end() && it->start < visTo; ++it) {
      const Ticks nEnd = std::min(it->end, seg.contentTo);
      int32_t x0 = roundCol(segL + static_cast<double>(it->start - seg.contentFrom) * k);
      int32_t x1 = roundCol(segL + static_cast<double>(nEnd - seg.contentFrom) * k);
      if (x1 - x0 >= 3) x1 -= 1;  // a pixel gap between notes that are wide enough
      x0 = std::max(x0, 0);
      x1 = std::min(std::max(x1, x0 + 1), geo.width);
      if (x1 <= x0) continue;
      const auto row = static_cast<uint16_t>((static_cast<uint32_t>(it->y) * span + 32767u) / 65535u);
      if (it->flags & kPreviewDisabled) {
        sink(x0, x1, row, RunKind::Disabled);
        continue;
      }
      Model::Pending& pr = pending[row];
      if (pr.open && x0 <= pr.x1) {
        pr.x1 = std::max(pr.x1, x1);  // overlaps the pending run at this pixel row: merge (zoomed-out clips)
      } else {
        if (pr.open) flush(row);
        else used.push_back(row);
        pr = {x0, x1, true};
      }
    }
  });
  for (uint16_t r : used) flush(r);
}

// Coverage of the translucent runs (the direct path draws them with these alphas over the body).
constexpr uint8_t kDisabledAlpha = 90;
constexpr uint8_t kSeamAlpha = 70;

// Whether a clip's preview is drawn from a tile: enough notes to be worth a quad, and a body that fits a tile.
bool wantsTile(const TileAtlas& tiles, size_t notes, int32_t width, int32_t bodyH) noexcept {
  return notes >= am::kTileMinNotes && bodyH >= 3 && width >= 1 && static_cast<uint32_t>(width) <= tiles.maxTileSize() &&
         static_cast<uint32_t>(bodyH) <= tiles.maxTileSize();
}

// Rasterises the clip's preview at (k, bodyH) into a new tile (the clip's own geometry). False when the atlas has
// no room this frame (the tile is then marked Failed for this key and the clip is drawn directly).
bool buildTile(TileAtlas& tiles, Preview& pv, const doc::NoteClip& clip, double k, int32_t bodyH, Model& m) {
  PreviewTile& t = pv.tile;
  const RunGeom geo = runGeom(pv, clip.length, k, bodyH);
  t = PreviewTile{};
  t.k = k;
  t.width = geo.width;
  t.bodyH = bodyH;
  TileAtlas::Tile tile;
  if (!tiles.allocate(static_cast<uint32_t>(geo.width), static_cast<uint32_t>(bodyH), tile)) {
    t.state = PreviewTile::State::Failed;
    ++m.stats.tileFailures;
    return false;
  }
  uint8_t* dst = tiles.write(tile);
  const uint32_t pitch = TileAtlas::rowPitch(tile);
  uint8_t* base = dst + pitch + 1;  // content origin inside the transparent border
  auto blend = [](uint8_t* p, int32_t n, uint32_t a) {
    for (int32_t i = 0; i < n; ++i) p[i] = static_cast<uint8_t>(p[i] + ((255u - p[i]) * a + 127u) / 255u);
  };
  forEachNoteRun(pv, clip, clip.start, clip.length, geo, clip.start, clip.start + clip.length, m,
                 [&](int32_t x0, int32_t x1, uint16_t row, RunKind kind) {
                   const int32_t n = x1 - x0;
                   const int32_t r0 = kind == RunKind::Seam ? 0 : row;
                   const int32_t r1 = kind == RunKind::Seam ? geo.bodyH : row + geo.noteH;
                   for (int32_t y = r0; y < r1; ++y) {
                     uint8_t* p = base + static_cast<size_t>(y) * pitch + x0;
                     if (kind == RunKind::Note) std::memset(p, 255, static_cast<size_t>(n));
                     else blend(p, n, kind == RunKind::Disabled ? kDisabledAlpha : kSeamAlpha);
                   }
                 });
  t.tile = tile;
  t.state = PreviewTile::State::Ready;
  ++m.stats.tilesBuilt;
  return true;
}

}  // namespace

// --- TrackStack -----------------------------------------------------------------------------------------

TrackStack::TrackStack(ArrangementView& view) : view_(view) { setLayout(LayoutKind::Custom); }

ISize TrackStack::contentExtent(ISize viewport) const {
  const UiScale s = tree() ? tree()->uiScale() : UiScale{};
  return {viewport.w, std::max(viewport.h, view_.model().contentHeight + s.px(am::kDropArea))};
}

void TrackStack::arrange(LayoutPass& pass) {
  const IRect r = rect();
  const UiScale s(pass.scale());
  const int32_t hdr = s.px(view_.headerWidth());
  const int32_t lw = lanesWidth(r, s, view_.headerWidth());
  const int32_t hx = r.right() - hdr;
  if (childCount() == 0) return;
  pass.place(child(0), {r.x, r.y, lw, r.h});
  // Headers by row, then the automation lane headers (T3.15) by lane.
  const auto rows = view_.rows();
  const auto& headers = view_.headerWidgets();
  for (size_t i = 0; i < headers.size() && i < rows.size(); ++i)
    if (headers[i]->visible()) pass.place(*headers[i], {hx, r.y + rows[i].y, hdr, rows[i].h});
  const auto lanes = view_.laneRows();
  const auto& laneHeaders = view_.laneHeaderWidgets();
  for (size_t i = 0; i < laneHeaders.size() && i < lanes.size(); ++i)
    pass.place(*laneHeaders[i], {hx, r.y + lanes[i].y, hdr, lanes[i].h});
}

void TrackStack::paint(PaintContext& ctx) {
  const Theme* th = ctx.res.theme;
  if (!th) return;
  const IRect r = rect();
  const IRect c = intersect(r, ctx.clip);
  if (c.empty()) return;
  const int32_t hdr = ctx.scale.px(view_.headerWidth());
  const int32_t lw = lanesWidth(r, ctx.scale, view_.headerWidth());
  const int32_t div = std::max(1, ctx.scale.px(1.0f));
  // The divider between the lanes and the headers, and the empty header column below the last track.
  ctx.dl.addRectFilled({static_cast<float>(r.x + lw), static_cast<float>(c.y), static_cast<float>(r.x + lw + div),
                        static_cast<float>(c.bottom())},
                       th->arrangement.separator);
  const int32_t rowsBottom = r.y + view_.model().contentHeight;
  if (rowsBottom < c.bottom())
    ctx.dl.addRectFilled({static_cast<float>(r.right() - hdr), static_cast<float>(std::max(rowsBottom, c.y)),
                          static_cast<float>(r.right()), static_cast<float>(c.bottom())},
                         th->arrangement.background);
}

// --- LaneCanvas -----------------------------------------------------------------------------------------

LaneCanvas::LaneCanvas(ArrangementView& view) : Widget("lanes"), view_(view) {}

// Previews still missing (over a paint's build budget, or skipped while zooming): build them for the visible clips,
// a budget per frame, without painting; repaint once they are all there (one repaint instead of one per frame).
void LaneCanvas::onFrame(double /*now*/) {
  // T3.x-fix-paint: the automation envelopes' missing tiles too (they damage the chunks they build).
  const PaintResources* res = tree() ? tree()->resources() : nullptr;
  const bool envDone = view_.automation().buildEnvelopeTiles(res ? res->tiles : nullptr, rect(), view_.lanesViewport());
  if (!buildAhead() || !envDone) {
    requestFrame();
    return;
  }
  // Everything is built. Repaint if a paint drew a clip approximately and something was built since (otherwise the
  // repaint would draw the same, and could ask again forever).
  const Model& m = view_.model();
  if (repaintWhenBuilt_) {
    repaintWhenBuilt_ = false;
    if (m.stats.previewBuilds + m.stats.tilesBuilt != latchBuilds_) markDirty();
  }
}

bool LaneCanvas::buildAhead() {
  Model& m = view_.model();
  const IRect r = rect();
  const IRect c = intersect(r, view_.lanesViewport());
  if (c.empty() || !m.shown) return true;
  const UiScale s = tree() ? tree()->uiScale() : UiScale{};
  const double k = view_.devicePxPerTick();
  const double origin = view_.originTick();
  auto X = [&](Ticks t) { return std::round(static_cast<float>(static_cast<double>(r.x) + (static_cast<double>(t) - origin) * k)); };
  const auto from = static_cast<Ticks>(std::floor(origin + (static_cast<double>(c.x - r.x) - 1.0) / k));
  const auto to = static_cast<Ticks>(std::ceil(origin + (static_cast<double>(c.right() - r.x) + 1.0) / k));
  const int32_t titleH = s.px(am::kClipTitleHeight);
  const float minW = static_cast<float>(s.px(12.0f));
  const auto rows = view_.rows();
  const int32_t localTop = c.y - r.y, localBottom = c.bottom() - r.y;
  const auto first = std::upper_bound(rows.begin(), rows.end(), localTop,
                                      [](int32_t y, const ArrangementRow& row) { return y < row.y + row.h; });
  // T3.8-fix1: tiles are built only once the zoom has settled (a tile is keyed by the exact zoom).
  const PaintResources* res = tree() ? tree()->resources() : nullptr;
  TileAtlas* tiles = res && !view_.zooming() ? res->tiles : nullptr;
  // The clips paint would draw with notes: f(row, clip, bodyH) for each, in paint order; false from f stops.
  auto forVisible = [&](auto&& f) {
    for (auto it = first; it != rows.end() && it->y < localBottom; ++it) {
      if (it->group || it->h - 3 - titleH < 3) continue;
      const doc::Track& t = *it->track;
      if (t.clips.empty()) continue;
      const ClipIndex& ix = m.clips(t);
      const auto [a, b] = ix.visible(from, to);
      // The body as paintClip computes it: [top + titleH, bottom − 1) of the clip's [y + 1, y + h − 1).
      const int32_t bodyH = it->h - 3 - titleH;
      for (size_t j = a; j < b; ++j) {
        const doc::ClipPtr& clip = t.clips[j].clip;
        if (!clip || clip->notes.empty()) continue;  // T5.3: audio clips have no note preview
        const float w = X(ix.end[j]) - X(ix.start[j]);
        if (w < minW) continue;
        if (view_.zooming() && w < am::kZoomPreviewMinPxPerNote * static_cast<float>(clip->notes.size())) continue;
        if (!f(*it, clip, bodyH)) return false;
      }
    }
    return true;
  };
  m.beginPaint(previewBudget_);
  // Pass 1: the visible clips' current tiles are in use this frame, so the allocations below never evict them.
  if (tiles)
    (void)forVisible([&](const ArrangementRow& row, const doc::ClipPtr& clip, int32_t bodyH) {
      if (const Preview* pv = m.cachedPreview(clip, clipTuning(row, *clip, *m.shown)))
        if (pv->tile.state == PreviewTile::State::Ready && pv->tile.matches(k, bodyH) && tiles->valid(pv->tile.tile))
          tiles->touch(pv->tile.tile.page);
      return true;
    });
  // Pass 2: previews within the note budget, tiles within the texel budget (at least one of each per frame).
  size_t texels = 0;
  bool builtTile = false;
  // T5.6: the audio clips' waveform tiles share the texel budget.
  if (tiles && !buildAudioTiles(*this, view_, *tiles, view_.lanesViewport(), texels, builtTile)) return false;
  return forVisible([&](const ArrangementRow& row, const doc::ClipPtr& clip, int32_t bodyH) {
    Preview* pv = m.preview(clip, clipTuning(row, *clip, *m.shown));
    if (!pv) return false;
    if (!tiles || !wantsTile(*tiles, clip->notes.size(), localWidth(clip->length, k), bodyH)) return true;
    const PreviewTile& t = pv->tile;
    if (t.matches(k, bodyH) &&
        (t.state == PreviewTile::State::Failed || (t.state == PreviewTile::State::Ready && tiles->valid(t.tile))))
      return true;
    const size_t area = static_cast<size_t>(localWidth(clip->length, k) + 2) * static_cast<size_t>(bodyH + 2);
    if (builtTile && texels + area > am::kTileBuildTexelsPerFrame) return false;
    (void)buildTile(*tiles, *pv, *clip, k, bodyH, m);
    texels += area;
    builtTile = true;
    return true;
  });
}

bool LaneCanvas::onPointer(const PointerEvent& e) {
  if (e.type != PointerType::Wheel) {
    mods_ = static_cast<Mods>(e.mods & mod::kChordMask);
    if (e.type != PointerType::Leave) pointer_ = e.pos;
    // T3.15: automation lanes and, in automation mode, the envelopes over the rows take their presses first.
    bool handled = view_.automation().pointer(e);
    if (!handled) {
      if (e.type == PointerType::Down) view_.automation().clearSelection();
      handled = view_.editor().pointer(e);  // selection and drags (T3.9)
    }
    // T4.16b: the hover highlight follows the pointer between gestures (a press clears it; after a release it shows
    // what the next press would grab).
    if (e.type == PointerType::Move || e.type == PointerType::Up) updateHover();
    return handled;
  }
  if (e.mods & mod::kCtrl) {
    view_.zoomAt(e.pos.x, std::pow(1.25, static_cast<double>(e.wheel.y)));
    return true;
  }
  if ((e.mods & mod::kShift) || e.wheel.x != 0.0f) {
    const float notches = e.wheel.x != 0.0f ? e.wheel.x : -e.wheel.y;  // Shift+wheel up = left (ScrollView's rule)
    const float step = 48.0f * (tree() ? tree()->scale() : 1.0f);
    view_.scrollByPx(static_cast<double>(notches * step));
    return true;
  }
  return false;  // plain wheel: the track scroll view scrolls vertically
}

// Capture lost without a release (a popup, the view hidden, the window losing focus): both editors drop their drag
// (T3.x-fix-edit: the automation drag used to keep following the pointer, M3 review L19).
void LaneCanvas::onCaptureLost() {
  view_.editor().abortDrag();
  view_.automation().abortDrag();
}

void LaneCanvas::onHoverChanged(bool hovered) {
  if (hovered) return;
  pointer_.reset();
  view_.editor().clearHover();
  view_.automation().clearHover();
}

void LaneCanvas::updateHover() {
  ArrangementAutomation& au = view_.automation();
  Editor& ed = view_.editor();
  if (!pointer_ || au.dragging() || ed.dragging()) return;
  // The automation takes the press first where it would (onPointer's order): then no clip part is highlighted.
  if (au.hoverAt(*pointer_, mods_)) ed.clearHover();
  else ed.hoverAt(*pointer_);
}

void LaneCanvas::modsChanged(Mods m) {
  m = static_cast<Mods>(m & mod::kChordMask);
  if (m == mods_) return;
  mods_ = m;
  updateHover();
}

Cursor LaneCanvas::cursorAt(Vec2 p) const {
  const ArrangementAutomation& au = view_.automation();
  const Editor& ed = view_.editor();
  if (ed.dragging() && !au.dragging()) return ed.cursorAt(p);
  if (const std::optional<Cursor> c = au.cursorAt(p, mods_)) return *c;
  return ed.cursorAt(p);
}

namespace {

void paintGrid(LanePaint& lp, float y0, float y1) {
  DrawList& dl = lp.ctx.dl;
  const TimeSigMap& sigs = lp.p.timeline->sigMap;
  const Ticks from = std::max<Ticks>(0, lp.from);
  if (lp.to <= from || y1 <= y0) return;
  const double logicalPxPerTick = lp.view.pxPerTick();
  const float scale = lp.ctx.scale.factor;

  // Shading: alternate bars (or grid cells of several bars when the grid is that coarse), each cell at least
  // 6 logical px wide.
  const GridSpec& spec = lp.view.context().grid();
  const Grid grid(spec.off ? GridSpec{} : spec, sigs, logicalPxPerTick);
  const TimeSignature sig0 = sigs.signatureAt(from);
  const GridDivision div0 = grid.divisionFor(sig0);
  int64_t cellBars = std::max<int64_t>(1, div0.bars);
  const double barPx = static_cast<double>(ticksPerBar(sig0)) * logicalPxPerTick;
  while (static_cast<double>(cellBars) * barPx < 6.0 && cellBars < (int64_t{1} << 20)) cellBars *= 2;
  int64_t bar = sigs.ticksToBarBeat(from).bar;
  bar -= bar % cellBars;
  for (Ticks t0 = sigs.barToTicks(bar); t0 < lp.to; bar += cellBars) {
    const Ticks t1 = sigs.barToTicks(bar + cellBars);
    if ((bar / cellBars) & 1) {
      const float x0 = std::max(std::round(lp.X(static_cast<double>(t0))), static_cast<float>(lp.c.x));
      const float x1 = std::min(std::round(lp.X(static_cast<double>(t1))), static_cast<float>(lp.c.right()));
      if (x1 > x0) dl.addRectFilled({x0, y0, x1, y1}, lp.th.arrangement.laneShade);
    }
    t0 = t1;
  }

  // Lines: the user's grid unless it is denser than one line per 4 logical px (a fixed grid zoomed out), then an
  // adaptive one. Bars and beats are stronger.
  const double cell = cellTicks(div0, sig0) * logicalPxPerTick;
  const Grid lines = (spec.off || cell < 4.0) ? Grid::adaptive(spec.off ? 24.0 : 6.0, sigs, logicalPxPerTick) : grid;
  // Live draws the grid as dashed lines (3 px on, 3 px off at 100 %). The dash phase is fixed to the device y, so a
  // partial redraw draws exactly the dashes a full one would.
  const float dash = std::max(1.0f, std::round(3.0f * scale));
  const float period = 2.0f * dash;
  const float dy0 = std::floor(y0 / period) * period;
  const Rgba barCol = lp.th.arrangement.gridLine;
  const Rgba beatCol = withAlpha(barCol, 150);
  const Rgba subCol = withAlpha(barCol, 80);
  float lastX = -1e9f;
  for (const GridLine& g : lines.lines(from, lp.to)) {
    const float x = std::floor(lp.X(static_cast<double>(g.tick)));
    if (x < static_cast<float>(lp.c.x) || x >= static_cast<float>(lp.c.right()) || x == lastX) continue;
    lastX = x;
    const Rgba col = g.strength == GridStrength::Bar ? barCol : g.strength == GridStrength::Beat ? beatCol : subCol;
    // One reservation per line: the dashes are written straight into the draw list (no per-dash call).
    const int32_t kFirst = y0 >= dy0 + dash ? 1 : 0;
    const auto kEnd = static_cast<int32_t>(std::ceil((y1 - dy0) / period));
    if (kEnd <= kFirst) continue;
    const auto n = static_cast<uint32_t>(kEnd - kFirst);
    const DrawList::PrimSpan sp = dl.primReserveSpan(6 * n, 4 * n);
    const Vec2 uv = dl.whiteUv();
    Vertex* v = sp.v;
    uint32_t* ix = sp.i;
    uint32_t base = sp.base;
    for (int32_t k = kFirst; k < kEnd; ++k) {
      const float ya = std::max(dy0 + static_cast<float>(k) * period, y0);
      const float yb = std::min(dy0 + static_cast<float>(k) * period + dash, y1);
      v[0] = {{x, ya}, uv, col};
      v[1] = {{x + 1.0f, ya}, uv, col};
      v[2] = {{x + 1.0f, yb}, uv, col};
      v[3] = {{x, yb}, uv, col};
      ix[0] = base;
      ix[1] = base + 1;
      ix[2] = base + 2;
      ix[3] = base;
      ix[4] = base + 2;
      ix[5] = base + 3;
      v += 4;
      ix += 6;
      base += 4;
    }
  }
}

// The preview drawn directly: one rect per note run, clipped to the clip's visible part [vx0, vx1] (the painted
// region plus a pixel) and the canvas. Returns the note rects emitted.
uint32_t paintDirect(LanePaint& lp, const Preview& pv, const doc::NoteClip& timing, const ClipGeom& g, float cx0, float cx1,
                     float vx0, float vx1, float by0, int32_t bodyH, Rgba noteCol) {
  const RunGeom geo = runGeom(pv, g.length, lp.k, bodyH);
  const Rgba offCol = withAlpha(noteCol, kDisabledAlpha);
  const Rgba seamCol = withAlpha(noteCol, kSeamAlpha);
  const float lo = std::max(cx0, static_cast<float>(lp.r.x)), hi = std::min(cx1, static_cast<float>(lp.r.right()));
  auto& out = lp.m.rects;  // written to the draw list in one reservation at the end (same order as generated)
  out.clear();
  uint32_t emitted = 0;
  forEachNoteRun(pv, timing, g.start, g.length, geo, lp.from, lp.to, lp.m, [&](int32_t x0, int32_t x1, uint16_t row, RunKind kind) {
    const float ax0 = std::max(cx0 + static_cast<float>(x0), lo);
    const float ax1 = std::min(cx0 + static_cast<float>(x1), hi);
    if (ax1 < vx0 || ax0 > vx1 || ax1 <= ax0) return;
    if (kind == RunKind::Seam) {
      out.push_back({ax0, by0, ax1, by0 + static_cast<float>(bodyH), seamCol});
      return;
    }
    const float y = by0 + static_cast<float>(row);
    out.push_back({ax0, y, ax1, y + static_cast<float>(geo.noteH), kind == RunKind::Note ? noteCol : offCol});
    ++emitted;
  });
  emitRects(lp.ctx.dl, out);
  lp.m.stats.noteRectsEmitted += emitted;
  return emitted;
}

// Queues a tile drawn over the clip body from local column 0 at cx0: 1:1 when it was rendered at this zoom and
// height (texel-exact: the quad's edges and uv sit on texel boundaries), else stretched (a stale tile, linear).
void queueTile(LanePaint& lp, const TileAtlas& tiles, const PreviewTile& t, float cx0, float vx0, float vx1, float by0,
               int32_t bodyH, Rgba noteCol) {
  const double ratio = t.k / lp.k;  // tile texels per device px
  const float x1 = std::min(vx1, cx0 + static_cast<float>(static_cast<double>(t.width) / ratio));
  if (x1 <= vx0) return;
  const float inv = tiles.uvScale();
  const auto u = [&](float x) {
    return (static_cast<float>(t.tile.x) + static_cast<float>(static_cast<double>(x - cx0) * ratio)) * inv;
  };
  lp.m.tileQuads.push_back({t.tile.page, vx0, by0, x1, by0 + static_cast<float>(bodyH), u(vx0), static_cast<float>(t.tile.y) * inv, u(x1),
                            static_cast<float>(t.tile.y + static_cast<uint32_t>(t.bodyH)) * inv, noteCol});
}

}  // namespace

bool clipColumns(const LanePaint& lp, Ticks start, Ticks length, ClipCols& out) noexcept {
  out.cx0 = std::round(lp.X(static_cast<double>(start)));
  out.cx1 = std::round(lp.X(static_cast<double>(start + length)));
  if (out.cx1 <= out.cx0) out.cx1 = out.cx0 + 1.0f;
  // Geometry is limited to the painted region (plus a pixel): huge clips at deep zoom stay in float range.
  // Never past the canvas (the divider to the headers is the next column: a clip drawn over it would change with
  // state the canvas's damage does not cover, e.g. the selection outline).
  const float rx0 = static_cast<float>(lp.r.x), rx1 = static_cast<float>(lp.r.right());
  out.vx0 = std::max(out.cx0, std::max(static_cast<float>(lp.c.x) - 1.0f, rx0));
  out.vx1 = std::min(out.cx1, std::min(static_cast<float>(lp.c.right()) + 1.0f, rx1));
  return out.vx1 > out.vx0;
}

float paintClipChrome(LanePaint& lp, const ClipCols& cc, float top, float bottom, Rgba colour, std::string_view name) {
  DrawList& dl = lp.ctx.dl;
  const Rgba body = clipBodyColour(colour);
  const float titleB = std::min(bottom, top + static_cast<float>(lp.titleH));
  dl.addRectFilled({cc.vx0, top, cc.vx1, titleB}, colour);
  if (bottom > titleB) dl.addRectFilled({cc.vx0, titleB, cc.vx1, bottom}, body);
  // Name: sticky at the lanes' left edge when the clip starts off-screen, cut at the clip's right edge.
  if (lp.font != kNoFont && lp.ctx.res.text && cc.cx1 - cc.cx0 >= 10.0f) {
    const float pad = static_cast<float>(lp.ctx.scale.px(3.0f));
    const float nx = std::max(cc.cx0, static_cast<float>(lp.r.x)) + pad;
    // Cut at the canvas edge too: a name running past it would paint the divider column, which the lanes' damage
    // does not cover (partial repaints then differed from full ones while zooming; T3.8-fix1).
    const float maxRight = std::min(cc.cx1 - pad, static_cast<float>(lp.r.right()));
    if (nx < static_cast<float>(lp.c.right()) && maxRight > nx)
      drawTextUpTo(dl, *lp.ctx.res.text, lp.font, name, nx, top + static_cast<float>(lp.baselineOff), maxRight,
                   lp.m.clipTextColour(lp.th, colour));
  }
  return titleB;
}

void paintClipBorder(LanePaint& lp, const ClipCols& cc, float top, float bottom, float titleB, bool selected, int hover,
                     bool deferred) {
  DrawList& dl = lp.ctx.dl;
  auto rect = [&](const Rect& rc, Rgba col) {
    if (deferred) lp.m.deferredRects.push_back({rc.x0, rc.y0, rc.x1, rc.y1, col});
    else dl.addRectFilled(rc, col);
  };
  const float cx0 = cc.cx0, cx1 = cc.cx1, vx0 = cc.vx0, vx1 = cc.vx1;
  // Border: dark edges separate adjacent clips (Live draws clips with a thin dark outline); a selected clip is
  // outlined all round in the light selection colour.
  if (selected) {
    const Rgba sel = lp.th.clip.selectedBorder;
    const float w = static_cast<float>(std::max(1, lp.ctx.scale.px(1.0f)));
    rect({vx0, top, vx1, top + w}, sel);
    rect({vx0, bottom - w, vx1, bottom}, sel);
    if (cx0 >= vx0) rect({cx0, top, cx0 + w, bottom}, sel);
    if (cx1 <= vx1) rect({cx1 - w, top, cx1, bottom}, sel);
  } else if (cx1 - cx0 >= 3.0f) {
    const Rgba border = lp.th.clip.border;
    if (cx0 >= vx0) rect({cx0, top, cx0 + 1.0f, bottom}, border);
    if (cx1 <= vx1) rect({cx1 - 1.0f, top, cx1, bottom}, border);
  }
  // T4.16b: the title strip brightened (a drag moves the clip) or the edge a drag trims (Live).
  if (hover == 0) {
    rect({vx0, top, vx1, titleB}, withAlpha(lp.th.text.primary, 56));
  } else if (hover == -1 || hover == 1) {
    const float w = std::min(static_cast<float>(std::max(2, lp.ctx.scale.px(3.0f))), std::floor((cx1 - cx0) * 0.5f));
    const float ex = hover < 0 ? cx0 : cx1 - w;
    const float a = std::max(ex, vx0), b = std::min(ex + w, vx1);
    if (b > a) rect({a, top, b, bottom}, lp.th.text.primary);
  }
}

// Emits the queued tile quads (one draw command per page), then the borders of their clips and (T5.6) the overlays of
// audio clips (fades, crossfades, handles).
void flushTiles(LanePaint& lp) {
  Model& m = lp.m;
  if (m.tileQuads.empty() && m.deferredRects.empty() && m.overlay.empty()) return;
  DrawList& dl = lp.ctx.dl;
  const TextureId prev = dl.texture();
  for (uint32_t page = 0; page < kMaxTilePages; ++page) {
    uint32_t n = 0;
    for (const Model::TileQuad& q : m.tileQuads) n += q.page == page ? 1 : 0;
    if (n == 0) continue;
    dl.setTexture(TileAtlas::texture(page));
    const DrawList::PrimSpan sp = dl.primReserveSpan(6 * n, 4 * n);
    Vertex* v = sp.v;
    uint32_t* ix = sp.i;
    uint32_t base = sp.base;
    for (const Model::TileQuad& q : m.tileQuads) {
      if (q.page != page) continue;
      v[0] = {{q.x0, q.y0}, {q.u0, q.v0}, q.col};
      v[1] = {{q.x1, q.y0}, {q.u1, q.v0}, q.col};
      v[2] = {{q.x1, q.y1}, {q.u1, q.v1}, q.col};
      v[3] = {{q.x0, q.y1}, {q.u0, q.v1}, q.col};
      ix[0] = base;
      ix[1] = base + 1;
      ix[2] = base + 2;
      ix[3] = base;
      ix[4] = base + 2;
      ix[5] = base + 3;
      v += 4;
      ix += 6;
      base += 4;
    }
  }
  dl.setTexture(prev);
  emitRects(dl, m.deferredRects);
  if (!m.overlay.empty()) {  // untextured geometry: copied with its indices rebased
    const auto vs = m.overlay.vertices();
    const auto is = m.overlay.indices();
    const DrawList::PrimSpan sp = dl.primReserveSpan(static_cast<uint32_t>(is.size()), static_cast<uint32_t>(vs.size()));
    std::memcpy(sp.v, vs.data(), vs.size_bytes());
    for (size_t i = 0; i < is.size(); ++i) sp.i[i] = is[i] + sp.base;
    m.overlay.reset(dl.targetWidth(), dl.targetHeight());
  }
  m.tileQuads.clear();
  m.deferredRects.clear();
}

namespace {

// A clip's notes (T3.8-fix1): from its tile when it has one at this zoom and body height; else (until onFrame has
// built it) from its previous tile stretched, or directly within the paint's fallback budget, or not at all. Clips
// without a tile (no GPU, few notes, too large for a tile, a resize preview) are drawn directly as in T3.8. Returns
// true when a tile quad was queued (the clip's border must then follow it: flushTiles).
bool paintNotes(LanePaint& lp, const ArrangementRow& row, const doc::ClipPtr& clipPtr, const doc::NoteClip& timing, const ClipGeom& g,
                float cx0, float cx1, float vx0, float vx1, float by0, int32_t bodyH, bool dense, Rgba noteCol) {
  Model& m = lp.m;
  const doc::NoteClip& clip = *clipPtr;
  const doc::TuningDef* tdef = clipTuning(row, clip, lp.p);
  TileAtlas* tiles = lp.ctx.res.tiles;
  // A tile is the clip's own content at its own length; a moved clip (drag preview) draws the same tile.
  const bool ownContent = g.length == clip.length && g.contentStart == clip.contentStart;
  if (tiles && ownContent && wantsTile(*tiles, clip.notes.size(), localWidth(g.length, lp.k), bodyH)) {
    Preview* pv = m.cachedPreview(clipPtr, tdef);
    const PreviewTile* t = pv ? &pv->tile : nullptr;
    const bool ready = t && t->state == PreviewTile::State::Ready && tiles->valid(t->tile);
    if (t && t->matches(lp.k, bodyH)) {
      if (ready) {
        queueTile(lp, *tiles, *t, cx0, vx0, vx1, by0, bodyH, noteCol);
        tiles->touch(t->tile.page);
        ++m.stats.tilesDrawn;
        m.stats.tileNotes += static_cast<uint32_t>(clip.notes.size());
        return true;
      }
      if (t->state == PreviewTile::State::Failed) {  // no room in the atlas at this key: exact, just slower
        (void)paintDirect(lp, *pv, timing, g, cx0, cx1, vx0, vx1, by0, bodyH, noteCol);
        return false;
      }
    }
    ++m.stats.tilesMissing;
    m.tilesMissing = true;
    if (lp.view.zooming()) m.tilesAfterZoom = true;
    if (ready) {
      queueTile(lp, *tiles, *t, cx0, vx0, vx1, by0, bodyH, noteCol);
      tiles->touch(t->tile.page);
      ++m.stats.tilesStale;
      m.approxDrawn = true;
      return true;
    }
    if (!dense && m.fallbackRects < am::kTileFallbackRectsPerPaint) {
      if (!pv) pv = m.preview(clipPtr, tdef);  // nullptr: over this paint's build budget
      if (pv) {
        m.fallbackRects += paintDirect(lp, *pv, timing, g, cx0, cx1, vx0, vx1, by0, bodyH, noteCol);
        return false;
      }
    }
    if (dense) m.skippedForZoom = m.approxForZoom = true;
    ++m.stats.previewsDeferred;
    m.approxDrawn = true;
    return false;
  }
  // No tile.
  if (dense) {
    m.skippedForZoom = m.approxForZoom = true;
    ++m.stats.previewsDeferred;
    m.approxDrawn = true;
    return false;
  }
  const Preview* pv = m.preview(clipPtr, tdef);  // nullptr: over this paint's build budget, built in a later frame
  if (!pv) {
    ++m.stats.previewsDeferred;
    m.approxDrawn = true;
    return false;
  }
  (void)paintDirect(lp, *pv, timing, g, cx0, cx1, vx0, vx1, by0, bodyH, noteCol);
  return false;
}

// A clip: border, title strip with the name, body with the note preview (looped content repeated). `g` is where it
// is drawn (its own bounds, or a drag preview's); a selected clip gets a light outline (T3.9). `hover` (T4.16b): the
// part a press would grab, highlighted (0 the title strip, −1 / +1 an edge; kNoHover none).
void paintClip(LanePaint& lp, const ArrangementRow& row, const doc::ClipPtr& clipPtr, float top, float bottom, bool trackActive,
               const ClipGeom& g, bool selected, int hover = kNoHover) {
  const doc::NoteClip& clip = *clipPtr;
  // Playback geometry for the segments: the clip itself, or (a preview) a copy of its timing without notes.
  std::optional<doc::NoteClip> moved;
  if (g.start != clip.start || g.length != clip.length || g.contentStart != clip.contentStart) {
    moved.emplace();
    moved->start = g.start;
    moved->length = g.length;
    moved->contentStart = g.contentStart;
    moved->loop = clip.loop;
  }
  const doc::NoteClip& timing = moved ? *moved : clip;
  ClipCols cc;
  if (!clipColumns(lp, g.start, g.length, cc)) return;
  ++lp.m.stats.clipsPainted;

  const bool active = clip.active && trackActive;
  const Rgba colour = active ? rgbOf(clip.colour) : inactiveColour();
  const float titleB = paintClipChrome(lp, cc, top, bottom, colour, clip.name);

  // Notes.
  const float by0 = titleB, by1 = bottom - 1.0f;
  const int32_t bodyH = static_cast<int32_t>(by1 - by0);
  // Level of detail: a clip narrower than 12 logical px shows only its colours (a preview there is a few unreadable
  // pixels, and at whole-song zoom it would cost tens of thousands of rects per frame).
  const bool detailed = cc.cx1 - cc.cx0 >= static_cast<float>(lp.ctx.scale.px(12.0f));
  // While the zoom changes, clips denser than kZoomPreviewMinPxPerNote draw no notes directly (their notes are a few
  // pixels each there; the frame stays cheap however far out the zoom goes); the lanes repaint once it settles.
  const bool dense = lp.view.zooming() &&
                     cc.cx1 - cc.cx0 < am::kZoomPreviewMinPxPerNote * static_cast<float>(clip.notes.size());
  // A tile quad and its clip's border go after the rows (flushTiles); everything else is drawn now.
  bool deferred = false;
  if (detailed && bodyH >= 3 && !clip.notes.empty())
    deferred = paintNotes(lp, row, clipPtr, timing, g, cc.cx0, cc.cx1, cc.vx0, cc.vx1, by0, bodyH, dense, clipNoteColour(colour));
  paintClipBorder(lp, cc, top, bottom, titleB, selected, hover, deferred);
}

// The track's own activator and its groups' (a deactivated or muted group silences its members; T3.10: the
// activator is the mute).
bool rowActive(std::span<const ArrangementRow> rows, const ArrangementRow& row) noexcept {
  bool active = sounds(*row.track);
  for (int32_t p = row.parent; p >= 0 && active; p = rows[static_cast<size_t>(p)].parent) active = sounds(*rows[static_cast<size_t>(p)].track);
  return active;
}

// Descendant MIDI tracks of a group (depth-first), for the group lane's clip summary.
void collectMembers(const doc::Track& g, std::vector<const doc::Track*>& out) {
  for (const doc::TrackPtr& t : g.children) {
    if (t->kind == doc::TrackKind::Group) collectMembers(*t, out);
    else out.push_back(t.get());
  }
}

// A group lane shows its members' clips as thin bars, one sub-row per member (Live's group summary).
void paintGroupSummary(LanePaint& lp, const doc::Track& group, float top, float bottom, std::vector<const doc::Track*>& members) {
  members.clear();
  collectMembers(group, members);
  if (members.empty()) return;
  const float pad = 3.0f;
  const float avail = bottom - top - 2.0f * pad;
  if (avail < 1.0f) return;
  const float sub = avail / static_cast<float>(members.size());
  for (size_t i = 0; i < members.size(); ++i) {
    const doc::Track& t = *members[i];
    const float y0 = std::round(top + pad + sub * static_cast<float>(i));
    const float y1 = std::max(y0 + 1.0f, std::round(top + pad + sub * static_cast<float>(i + 1)) - (sub >= 3.0f ? 1.0f : 0.0f));
    const ClipIndex& ix = lp.m.clips(t);
    const auto [a, b] = ix.visible(lp.from, lp.to);
    for (size_t j = a; j < b; ++j) {
      const doc::ClipSlot& c = t.clips[j];  // T5.3: note and audio clips alike
      const float x0 = std::max(std::round(lp.X(static_cast<double>(ix.start[j]))),
                                std::max(static_cast<float>(lp.c.x) - 1.0f, static_cast<float>(lp.r.x)));
      const float x1 = std::min(std::max(std::round(lp.X(static_cast<double>(ix.end[j]))), x0 + 1.0f),
                                std::min(static_cast<float>(lp.c.right()) + 1.0f, static_cast<float>(lp.r.right())));
      if (x1 <= x0) continue;
      const Rgba col = (c.active() && sounds(t) && sounds(group)) ? rgbOf(c.colour()) : inactiveColour();
      // Translucent body with a solid top edge: reads as a summary, not as clips (Live's group lane).
      lp.ctx.dl.addRectFilled({x0, y0, x1, y1}, withAlpha(col, 95));
      if (y1 - y0 >= 3.0f) lp.ctx.dl.addRectFilled({x0, y0, x1, y0 + 1.0f}, withAlpha(col, 230));
    }
  }
}

}  // namespace

void LaneCanvas::paint(PaintContext& ctx) {
  const Theme* thp = ctx.res.theme;
  if (!thp) return;
  const Theme& th = *thp;
  Model& m = view_.model();
  const IRect r = rect();
  const IRect c = intersect(r, ctx.clip);
  // A frame may paint the canvas once per damage rect (UiTree::paint over rects): the "last paint" stats, the preview
  // build budget and the cache's paint serial count frames, not passes.
  const bool newFrame = ctx.frame != paintFrame_ || ctx.frame == 0;
  if (newFrame) {
    m.stats.rowsPainted = 0;
    m.stats.clipsPainted = 0;
    m.stats.noteRectsEmitted = 0;
    m.stats.previewsDeferred = 0;
    m.stats.tilesDrawn = 0;
    m.stats.tileNotes = 0;
    m.stats.tilesStale = 0;
    m.stats.tilesMissing = 0;
  }
  m.tilesMissing = false;
  m.approxDrawn = false;
  m.approxForZoom = false;
  m.fallbackRects = 0;
  m.tileQuads.clear();
  m.deferredRects.clear();
  m.overlay.reset(ctx.dl.targetWidth(), ctx.dl.targetHeight());  // T5.6: audio overlays of this paint
  if (c.empty()) return;
  if (newFrame) {
    m.beginPaint(previewBudget_);
    m.stats.wavesDirect = 0;
    m.stats.waveColumns = 0;
  }
  paintFrame_ = ctx.frame;
  const double k = view_.devicePxPerTick();
  const double origin = view_.originTick();
  LanePaint lp{ctx, th, m, view_, *m.shown, r, c, k, origin,
               static_cast<Ticks>(std::floor(origin + (static_cast<double>(c.x - r.x) - 1.0) / k)),
               static_cast<Ticks>(std::ceil(origin + (static_cast<double>(c.right() - r.x) + 1.0) / k)),
               ctx.scale.px(am::kClipTitleHeight)};
  if (ctx.res.uiFonts && ctx.res.text) {
    lp.font = ctx.res.uiFonts->small;
    const uint32_t ps = ctx.res.text->metrics(lp.font).pixelSize;
    const int32_t cap = static_cast<int32_t>(std::lround(0.727 * static_cast<double>(ps)));
    lp.baselineOff = (lp.titleH + cap + 1) / 2;
  }
  DrawList& dl = ctx.dl;
  const auto rows = view_.rows();

  // Visible rows: binary search on the row tops.
  const int32_t localTop = c.y - r.y, localBottom = c.bottom() - r.y;
  const auto first = std::upper_bound(rows.begin(), rows.end(), localTop,
                                      [](int32_t y, const ArrangementRow& row) { return y < row.y + row.h; });
  auto last = first;
  while (last != rows.end() && last->y < localBottom) ++last;
  const float cx0 = static_cast<float>(c.x), cx1 = static_cast<float>(c.right());

  // 1. The lane colour everywhere (Live continues lanes and grid into the empty area below the tracks), selected
  // tracks' lanes lighter (T3.9), then the alternate-bar shading and the grid lines: one column per cell / line
  // across every visible row.
  dl.addRectFilled(c.toRect(), th.arrangement.lane);
  view_.automation().paintLaneBackgrounds(ctx, r);  // T3.15: automation lanes a shade darker, under the grid
  const Editor& ed = view_.editor();
  if (!ed.selection().tracks.empty())
    for (auto it = first; it != last; ++it)
      if (ed.trackSelected(it->id))
        dl.addRectFilled({cx0, static_cast<float>(r.y + it->y), cx1, static_cast<float>(r.y + it->y + it->h - 1)},
                         th.arrangement.laneSelected);
  paintGrid(lp, static_cast<float>(c.y), static_cast<float>(c.bottom()));

  // 3. Row separators, clips, group summaries.
  for (auto it = first; it != last; ++it) {
    ++m.stats.rowsPainted;
    const float y0 = static_cast<float>(r.y + it->y), y1 = static_cast<float>(r.y + it->y + it->h);
    dl.addRectFilled({cx0, y1 - 1.0f, cx1, y1}, th.arrangement.separator);
    const doc::Track& t = *it->track;
    if (it->group) {
      paintGroupSummary(lp, t, y0, y1 - 1.0f, m.members);
      continue;
    }
    if (t.clips.empty()) continue;
    const ClipIndex& ix = m.clips(t);
    const auto [a, b] = ix.visible(lp.from, lp.to);
    const bool active = rowActive(rows, *it);
    const Editor::HoverPart& hp = ed.hoverPart();
    const bool hoverRow = !ed.dragging() && hp.ref.clip.valid() && hp.ref.track == it->id;
    for (size_t j = a; j < b; ++j) {
      const doc::ClipSlot& slot = t.clips[j];
      if (ed.hiddenWhileDragging(slot.id)) continue;  // drawn at its drag target below
      // Deferred tile quads may not cross a clip drawn over them: overlapping clips (later on top) flush first.
      if (j > 0 && ix.start[j] < ix.maxEnd[j - 1]) flushTiles(lp);
      const int hover = hoverRow && slot.id == hp.ref.clip ? hp.zone : kNoHover;
      if (slot.audio) {  // T5.6: waveform, fades, crossfade, handles
        paintAudioClip(lp, *it, slot.audio, {slot.audio->start, slot.audio->length, 0}, y0 + 1.0f, y1 - 1.0f, active,
                       ed.selected(slot.id), hover);
        continue;
      }
      const doc::ClipPtr& clip = slot.clip;
      paintClip(lp, *it, clip, y0 + 1.0f, y1 - 1.0f, active, {clip->start, clip->length, clip->contentStart}, ed.selected(clip->id),
                hover);
    }
  }
  flushTiles(lp);

  // 3b. Editing (T3.9): the time selection (lighter over the selected rows; the insert marker a line), then the
  // clips being dragged, drawn at their target.
  if (const auto& ts = ed.selection().time; ts && ed.showTimeSelection()) {
    const bool marker = ts->end <= ts->start;
    const float x0 = std::max(cx0, std::floor(lp.X(static_cast<double>(ts->start))));
    const float x1 = std::min(cx1, marker ? x0 + static_cast<float>(std::max(1, ctx.scale.px(1.0f)))
                                          : std::floor(lp.X(static_cast<double>(ts->end))));
    const Rgba col = withAlpha(th.clip.selectedBorder, marker ? 200 : 46);
    if (x1 > x0)
      for (auto it = first; it != last; ++it)
        if (ed.trackSelected(it->id))
          dl.addRectFilled({x0, static_cast<float>(r.y + it->y), x1, static_cast<float>(r.y + it->y + it->h - 1)}, col);
  }
  for (const DragClip& dc : ed.dragClips()) {
    int32_t ri = 0;
    ClipGeom g;
    if (!ed.preview(dc, ri, g)) continue;
    const ArrangementRow& row = rows[static_cast<size_t>(ri)];
    const float y0 = static_cast<float>(r.y + row.y), y1 = static_cast<float>(r.y + row.y + row.h);
    if (y1 <= static_cast<float>(c.y) || y0 >= static_cast<float>(c.bottom())) continue;
    if (dc.clip.audio) paintAudioClip(lp, row, dc.clip.audio, g, y0 + 1.0f, y1 - 1.0f, rowActive(rows, row), true, kNoHover);
    else paintClip(lp, row, dc.clip.clip, y0 + 1.0f, y1 - 1.0f, rowActive(rows, row), g, true);
    flushTiles(lp);  // drag previews overlap other clips: each one complete before the next
  }

  // T3.15: automation lanes and, in automation mode, the envelopes over the rows.
  view_.automation().paintLanes(ctx, r);
  if (view_.automation().takeEnvelopeTilesWanted()) requestFrame();  // onFrame builds the tiles

  // Previews (and, once the zoom has settled, tiles) left for later frames: onFrame builds them without painting
  // and repaints the lanes once, if this paint drew a clip approximately.
  m.endPaint();
  if (m.approxDrawn) {
    if (!repaintWhenBuilt_) latchBuilds_ = m.stats.previewBuilds + m.stats.tilesBuilt;
    if (m.approxForZoom) latchBuilds_ = UINT64_MAX;  // skipped only for the zoom: repaint even if nothing is built
    repaintWhenBuilt_ = true;
  }
  if (m.buildsDeferred || (m.tilesMissing && !view_.zooming())) requestFrame();

  // 4. The editing overlay (T3.9), then the playhead on top.
  if (view_.onPaintOverlay) view_.onPaintOverlay(ctx);
  if (const auto& ph = view_.playhead()) {
    const float x = std::floor(lp.X(static_cast<double>(*ph)));
    const float w = static_cast<float>(std::max(1, ctx.scale.px(1.0f)));
    if (x + w > cx0 && x < cx1) dl.addRectFilled({x, static_cast<float>(c.y), x + w, static_cast<float>(c.bottom())}, th.arrangement.playhead);
  }
}

// --- BottomTracks / BottomLanes (T3.10: returns + master pinned at the bottom) -------------------------------------

BottomTracks::BottomTracks(ArrangementView& view) : Widget("returns"), view_(view) {
  setLayout(LayoutKind::Custom);
  setClipChildren(true);
  add<BottomLanes>(view);
}

void BottomTracks::arrange(LayoutPass& pass) {
  const IRect r = rect();
  const UiScale s(pass.scale());
  const PaintResources* res = tree() ? tree()->resources() : nullptr;
  const int32_t sb = s.px(res && res->theme ? res->theme->size.scrollbar : 10.0f);
  const int32_t gap = std::min(r.h, s.px(am::kBottomGap));
  const IRect body{r.x, r.y + gap, std::max(0, r.w - sb), std::max(0, r.h - gap)};  // the scroll view's columns
  const int32_t hdr = s.px(view_.headerWidth());
  pass.place(child(0), {body.x, body.y, lanesWidth(body, s, view_.headerWidth()), body.h});
  const auto rows = view_.bottomRows();
  for (size_t i = 1; i < childCount(); ++i) {
    Widget& c = child(i);
    if (!c.visible() || i - 1 >= rows.size()) continue;
    const ArrangementRow& row = rows[i - 1];
    pass.place(c, {body.right() - hdr, body.y + row.y, hdr, row.h});
  }
}

void BottomTracks::paint(PaintContext& ctx) {
  const Theme* th = ctx.res.theme;
  if (!th) return;
  const IRect r = rect();
  const IRect c = intersect(r, ctx.clip);
  if (c.empty()) return;
  DrawList& dl = ctx.dl;
  dl.addRectFilled(c.toRect(), th->arrangement.background);
  const int32_t gap = std::min(r.h, ctx.scale.px(am::kBottomGap));
  dl.addRectFilled(IRect{r.x, r.y, r.w, gap}.toRect(), th->arrangement.separator);
  // The divider between the lanes and the headers (as in the scrolling rows).
  const int32_t sb = ctx.scale.px(th->size.scrollbar);
  const IRect body{r.x, r.y + gap, std::max(0, r.w - sb), std::max(0, r.h - gap)};
  const int32_t lw = lanesWidth(body, ctx.scale, view_.headerWidth());
  const int32_t div = std::max(1, ctx.scale.px(1.0f));
  dl.addRectFilled(IRect{body.x + lw, body.y, div, body.h}.toRect(), th->arrangement.separator);
}

BottomLanes::BottomLanes(ArrangementView& view) : Widget("lanes"), view_(view) {}

void BottomLanes::onCaptureLost() { view_.automation().abortDrag(); }

void BottomLanes::onHoverChanged(bool hovered) {
  if (!hovered) view_.automation().clearHover();
}

Cursor BottomLanes::cursorAt(Vec2 p) const { return view_.automation().cursorAt(p, mods_).value_or(Cursor::Arrow); }

bool BottomLanes::onPointer(const PointerEvent& e) {
  if (e.type != PointerType::Wheel) {
    mods_ = static_cast<Mods>(e.mods & mod::kChordMask);
    const bool handled = view_.automation().pointer(e);  // T3.15: automation mode over the rows
    if ((e.type == PointerType::Move || e.type == PointerType::Up) && !view_.automation().dragging())
      (void)view_.automation().hoverAt(e.pos, mods_);
    return handled;
  }
  if (e.mods & mod::kCtrl) {
    view_.zoomAt(e.pos.x, std::pow(1.25, static_cast<double>(e.wheel.y)));
    return true;
  }
  if ((e.mods & mod::kShift) || e.wheel.x != 0.0f) {
    const float notches = e.wheel.x != 0.0f ? e.wheel.x : -e.wheel.y;
    view_.scrollByPx(static_cast<double>(notches * 48.0f * (tree() ? tree()->scale() : 1.0f)));
    return true;
  }
  return false;
}

void BottomLanes::paint(PaintContext& ctx) {
  const Theme* thp = ctx.res.theme;
  if (!thp) return;
  const Theme& th = *thp;
  Model& m = view_.model();
  const IRect r = rect();
  const IRect c = intersect(r, ctx.clip);
  if (c.empty() || !m.shown) return;
  const double k = view_.devicePxPerTick();
  const double origin = view_.originTick();
  LanePaint lp{ctx, th, m, view_, *m.shown, r, c, k, origin,
               static_cast<Ticks>(std::floor(origin + (static_cast<double>(c.x - r.x) - 1.0) / k)),
               static_cast<Ticks>(std::ceil(origin + (static_cast<double>(c.right() - r.x) + 1.0) / k)), 0};
  DrawList& dl = ctx.dl;
  const float cx0 = static_cast<float>(c.x), cx1 = static_cast<float>(c.right());
  dl.addRectFilled(c.toRect(), th.arrangement.lane);
  const Editor& ed = view_.editor();
  const auto rows = view_.bottomRows();
  for (const ArrangementRow& row : rows)
    if (ed.trackSelected(row.id))
      dl.addRectFilled({cx0, static_cast<float>(r.y + row.y), cx1, static_cast<float>(r.y + row.y + row.h - 1)}, th.arrangement.laneSelected);
  paintGrid(lp, static_cast<float>(c.y), static_cast<float>(c.bottom()));
  for (const ArrangementRow& row : rows) {
    const float y1 = static_cast<float>(r.y + row.y + row.h);
    if (y1 > static_cast<float>(c.y) && y1 - 1.0f < static_cast<float>(c.bottom())) dl.addRectFilled({cx0, y1 - 1.0f, cx1, y1}, th.arrangement.separator);
  }
  view_.automation().paintPinned(ctx, r);  // T3.15
  if (const auto& ph = view_.playhead()) {
    const float x = std::floor(lp.X(static_cast<double>(*ph)));
    const float w = static_cast<float>(std::max(1, ctx.scale.px(1.0f)));
    if (x + w > cx0 && x < cx1) dl.addRectFilled({x, static_cast<float>(c.y), x + w, static_cast<float>(c.bottom())}, th.arrangement.playhead);
  }
}

}  // namespace pq::views::arrangement
