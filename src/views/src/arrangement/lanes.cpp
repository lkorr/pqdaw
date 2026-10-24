// The arrangement's track content (T3.8): TrackStack (lanes + headers in the vertical scroll view) and LaneCanvas
// (grid, clips with their cached note previews, group summaries, playhead) — one virtualised widget for all rows.

#include <algorithm>
#include <bit>
#include <cmath>
#include <optional>
#include <span>

#include "internal.h"

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

// round() for pixel positions without the CRT call (positions here are ≥ −2 device px; ties round up).
inline float roundPx(float v) noexcept { return std::floor(v + 0.5f); }

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
  const auto rows = view_.rows();
  for (size_t i = 1; i < childCount(); ++i) {
    Widget& c = child(i);
    if (!c.visible()) continue;
    if (i - 1 < rows.size()) {
      const ArrangementRow& row = rows[i - 1];
      pass.place(c, {hx, r.y + row.y, hdr, row.h});
    }
  }
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
  if (buildAhead()) markDirty();
  else requestFrame();
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
  auto it = std::upper_bound(rows.begin(), rows.end(), localTop,
                             [](int32_t y, const ArrangementRow& row) { return y < row.y + row.h; });
  m.beginPaint(previewBudget_);
  for (; it != rows.end() && it->y < localBottom; ++it) {
    if (it->group || it->h - 3 - titleH < 3) continue;
    const doc::Track& t = *it->track;
    if (t.clips.empty()) continue;
    const ClipIndex& ix = m.clips(t);
    const auto [a, b] = ix.visible(from, to);
    for (size_t j = a; j < b; ++j) {
      const doc::ClipPtr& clip = t.clips[j].clip;
      if (clip->notes.empty()) continue;
      const float w = X(ix.end[j]) - X(ix.start[j]);
      if (w < minW) continue;
      if (view_.zooming() && w < am::kZoomPreviewMinPxPerNote * static_cast<float>(clip->notes.size())) continue;
      if (!m.preview(clip, clipTuning(*it, *clip, *m.shown))) return false;
    }
  }
  return true;
}

bool LaneCanvas::onPointer(const PointerEvent& e) {
  if (e.type != PointerType::Wheel) return view_.editor().pointer(e);  // selection and drags (T3.9)
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

void LaneCanvas::onCaptureLost() { view_.editor().cancelDrag(); }

namespace {

// Everything one lanes paint needs, computed once.
struct LanePaint {
  PaintContext& ctx;
  const Theme& th;
  Model& m;
  ArrangementView& view;
  const doc::Project& p;
  IRect r;      // canvas rect
  IRect c;      // visible part being painted
  double k;     // device px per tick
  double origin;
  Ticks from, to;  // ticks meeting c (a pixel of slack each side)
  int32_t titleH;
  FontId font = kNoFont;
  int32_t baselineOff = 0;  // title strip top → name baseline

  float X(double tick) const noexcept { return static_cast<float>(static_cast<double>(r.x) + (tick - origin) * k); }
};

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

// A clip: border, title strip with the name, body with the note preview (looped content repeated). `g` is where it
// is drawn (its own bounds, or a drag preview's); a selected clip gets a light outline (T3.9).
void paintClip(LanePaint& lp, const ArrangementRow& row, const doc::ClipPtr& clipPtr, float top, float bottom, bool trackActive,
               const ClipGeom& g, bool selected) {
  const doc::NoteClip& clip = *clipPtr;
  DrawList& dl = lp.ctx.dl;
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
  const float cx0 = std::round(lp.X(static_cast<double>(g.start)));
  float cx1 = std::round(lp.X(static_cast<double>(g.start + g.length)));
  if (cx1 <= cx0) cx1 = cx0 + 1.0f;
  // Geometry is limited to the painted region (plus a pixel): huge clips at deep zoom stay in float range.
  // Never past the canvas (the divider to the headers is the next column: a clip drawn over it would change with
  // state the canvas's damage does not cover, e.g. the selection outline).
  const float rx0 = static_cast<float>(lp.r.x), rx1 = static_cast<float>(lp.r.right());
  const float vx0 = std::max(cx0, std::max(static_cast<float>(lp.c.x) - 1.0f, rx0));
  const float vx1 = std::min(cx1, std::min(static_cast<float>(lp.c.right()) + 1.0f, rx1));
  if (vx1 <= vx0) return;
  ++lp.m.stats.clipsPainted;

  const bool active = clip.active && trackActive;
  const Rgba colour = active ? rgbOf(clip.colour) : inactiveColour();
  const Rgba body = clipBodyColour(colour);
  const float titleB = std::min(bottom, top + static_cast<float>(lp.titleH));
  dl.addRectFilled({vx0, top, vx1, titleB}, colour);
  if (bottom > titleB) dl.addRectFilled({vx0, titleB, vx1, bottom}, body);

  // Name: sticky at the lanes' left edge when the clip starts off-screen, cut at the clip's right edge.
  if (lp.font != kNoFont && lp.ctx.res.text && cx1 - cx0 >= 10.0f) {
    const float pad = static_cast<float>(lp.ctx.scale.px(3.0f));
    const float nx = std::max(cx0, static_cast<float>(lp.r.x)) + pad;
    if (nx < static_cast<float>(lp.c.right()) && cx1 - pad > nx)
      drawTextUpTo(dl, *lp.ctx.res.text, lp.font, clip.name, nx, top + static_cast<float>(lp.baselineOff), cx1 - pad,
                   lp.m.clipTextColour(lp.th, colour));
  }

  // Notes.
  const float by0 = titleB, by1 = bottom - 1.0f;
  const int32_t bodyH = static_cast<int32_t>(by1 - by0);
  // Level of detail: a clip narrower than 12 logical px shows only its colours (a preview there is a few unreadable
  // pixels, and at whole-song zoom it would cost tens of thousands of rects per frame).
  const bool detailed = cx1 - cx0 >= static_cast<float>(lp.ctx.scale.px(12.0f));
  // While the zoom changes, clips denser than kZoomPreviewMinPxPerNote draw only their bodies (their notes are a few
  // pixels each there; the frame stays cheap however far out the zoom goes); the lanes repaint once it settles.
  const bool dense = lp.view.zooming() &&
                     cx1 - cx0 < am::kZoomPreviewMinPxPerNote * static_cast<float>(clip.notes.size());
  if (detailed && dense && bodyH >= 3) {
    lp.m.skippedForZoom = true;
    ++lp.m.stats.previewsDeferred;
  }
  const Preview* pvp = nullptr;
  if (detailed && !dense && bodyH >= 3 && !clip.notes.empty()) {
    pvp = lp.m.preview(clipPtr, clipTuning(row, clip, lp.p));  // nullptr: over this paint's build budget, built in a later frame
    if (!pvp) ++lp.m.stats.previewsDeferred;
  }
  if (pvp) {
    const Preview& pv = *pvp;
    const int32_t noteH = std::clamp(static_cast<int32_t>(std::lround(static_cast<float>(bodyH) * pv.rowFraction)), 1, bodyH);
    const uint32_t span = static_cast<uint32_t>(bodyH - noteH);
    const Rgba noteCol = clipNoteColour(colour);
    const Rgba offCol = withAlpha(noteCol, 90);
    const Rgba seamCol = withAlpha(noteCol, 70);
    auto& pending = lp.m.pending;
    auto& used = lp.m.usedRows;
    auto& out = lp.m.rects;  // written to the draw list in one reservation at the end (same order as drawn)
    if (pending.size() < static_cast<size_t>(bodyH) + 1) pending.resize(static_cast<size_t>(bodyH) + 1);
    used.clear();
    out.clear();
    uint32_t emitted = 0;
    auto flush = [&](uint16_t px) {
      Model::Pending& pr = pending[px];
      if (!pr.open) return;
      pr.open = false;
      const float y = by0 + static_cast<float>(px);
      out.push_back({pr.x0, y, pr.x1, y + static_cast<float>(noteH), noteCol});
      ++emitted;
    };
    const Ticks clipEnd = g.start + g.length;
    const double k = lp.k;
    bool firstSeg = true;
    // Passes over the content from the clip start (not from the window: a note that starts left of the window
    // still shows); passes left of the window are skipped before touching notes.
    doc::forEachClipSegment(timing, g.start, std::min(clipEnd, lp.to), [&](const doc::ClipSegment& seg) {
      const Ticks segEnd = seg.at + (seg.contentTo - seg.contentFrom);
      const bool seam = !firstSeg;
      firstSeg = false;
      if (segEnd < lp.from) return;
      const float segX0 = lp.X(static_cast<double>(seg.at));
      if (seam && segX0 >= vx0 && segX0 < vx1) out.push_back({std::floor(segX0), by0, std::floor(segX0) + 1.0f, by1, seamCol});
      const Ticks visFrom = seg.contentFrom + std::max<Ticks>(0, lp.from - seg.at);
      const Ticks lookFrom = std::max(seg.contentFrom, visFrom - pv.maxLength);
      const Ticks visTo = std::min(seg.contentTo, seg.contentFrom + (lp.to - seg.at));
      auto it = std::lower_bound(pv.notes.begin(), pv.notes.end(), lookFrom,
                                 [](const PreviewNote& n, Ticks t) { return n.start < t; });
      for (; it != pv.notes.end() && it->start < visTo; ++it) {
        const Ticks nEnd = std::min(it->end, seg.contentTo);
        float x0 = roundPx(segX0 + static_cast<float>(static_cast<double>(it->start - seg.contentFrom) * k));
        float x1 = roundPx(segX0 + static_cast<float>(static_cast<double>(nEnd - seg.contentFrom) * k));
        if (x1 - x0 >= 3.0f) x1 -= 1.0f;  // a pixel gap between notes that are wide enough
        x0 = std::max(x0, std::max(cx0, rx0));
        x1 = std::min(std::max(x1, x0 + 1.0f), std::min(cx1, rx1));
        if (x1 < vx0 || x0 > vx1 || x1 <= x0) continue;
        const auto rowPx = static_cast<uint16_t>((static_cast<uint32_t>(it->y) * span + 32767u) / 65535u);
        if (it->flags & kPreviewDisabled) {
          const float y = by0 + static_cast<float>(rowPx);
          out.push_back({x0, y, x1, y + static_cast<float>(noteH), offCol});
          ++emitted;
          continue;
        }
        Model::Pending& pr = pending[rowPx];
        if (pr.open && x0 <= pr.x1) {
          pr.x1 = std::max(pr.x1, x1);  // overlaps the pending run at this pixel row: merge (zoomed-out clips)
        } else {
          if (pr.open) flush(rowPx);
          else used.push_back(rowPx);
          pr = {x0, x1, true};
        }
      }
    });
    for (uint16_t pr : used) flush(pr);
    emitRects(dl, out);
    lp.m.stats.noteRectsEmitted += emitted;
  }
  // Border: dark edges separate adjacent clips (Live draws clips with a thin dark outline); a selected clip is
  // outlined all round in the light selection colour.
  if (selected) {
    const Rgba sel = lp.th.clip.selectedBorder;
    const float w = static_cast<float>(std::max(1, lp.ctx.scale.px(1.0f)));
    dl.addRectFilled({vx0, top, vx1, top + w}, sel);
    dl.addRectFilled({vx0, bottom - w, vx1, bottom}, sel);
    if (cx0 >= vx0) dl.addRectFilled({cx0, top, cx0 + w, bottom}, sel);
    if (cx1 <= vx1) dl.addRectFilled({cx1 - w, top, cx1, bottom}, sel);
  } else if (cx1 - cx0 >= 3.0f) {
    const Rgba border = lp.th.clip.border;
    if (cx0 >= vx0) dl.addRectFilled({cx0, top, cx0 + 1.0f, bottom}, border);
    if (cx1 <= vx1) dl.addRectFilled({cx1 - 1.0f, top, cx1, bottom}, border);
  }
}

// The track's own activator and its groups' (a deactivated group silences its members).
bool rowActive(std::span<const ArrangementRow> rows, const ArrangementRow& row) noexcept {
  bool active = row.track->active;
  for (int32_t p = row.parent; p >= 0 && active; p = rows[static_cast<size_t>(p)].parent) active = rows[static_cast<size_t>(p)].track->active;
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
      const doc::NoteClip& c = *t.clips[j].clip;
      const float x0 = std::max(std::round(lp.X(static_cast<double>(ix.start[j]))),
                                std::max(static_cast<float>(lp.c.x) - 1.0f, static_cast<float>(lp.r.x)));
      const float x1 = std::min(std::max(std::round(lp.X(static_cast<double>(ix.end[j]))), x0 + 1.0f),
                                std::min(static_cast<float>(lp.c.right()) + 1.0f, static_cast<float>(lp.r.right())));
      if (x1 <= x0) continue;
      const Rgba col = (c.active && t.active && group.active) ? rgbOf(c.colour) : inactiveColour();
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
  m.stats.rowsPainted = 0;
  m.stats.clipsPainted = 0;
  m.stats.noteRectsEmitted = 0;
  m.stats.previewsDeferred = 0;
  if (c.empty()) return;
  m.beginPaint(previewBudget_);
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
    for (size_t j = a; j < b; ++j) {
      const doc::ClipPtr& clip = t.clips[j].clip;
      if (ed.hiddenWhileDragging(clip->id)) continue;  // drawn at its drag target below
      paintClip(lp, *it, clip, y0 + 1.0f, y1 - 1.0f, active, {clip->start, clip->length, clip->contentStart}, ed.selected(clip->id));
    }
  }

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
    paintClip(lp, row, dc.clip, y0 + 1.0f, y1 - 1.0f, rowActive(rows, row), g, true);
  }

  // Previews left for later frames: paint again next frame (onFrame damages the lanes) until all are built.
  m.endPaint();
  if (m.buildsDeferred) requestFrame();

  // 4. The editing overlay (T3.9), then the playhead on top.
  if (view_.onPaintOverlay) view_.onPaintOverlay(ctx);
  if (const auto& ph = view_.playhead()) {
    const float x = std::floor(lp.X(static_cast<double>(*ph)));
    const float w = static_cast<float>(std::max(1, ctx.scale.px(1.0f)));
    if (x + w > cx0 && x < cx1) dl.addRectFilled({x, static_cast<float>(c.y), x + w, static_cast<float>(c.bottom())}, th.arrangement.playhead);
  }
}

}  // namespace pq::views::arrangement
