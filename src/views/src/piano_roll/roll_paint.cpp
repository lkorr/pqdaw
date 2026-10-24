// Painting of the clip view's parts (T3.11). Every part paints only inside its rect (pushClip) and only what meets
// PaintContext::clip: rows by binary search, grid lines and notes by the visible tick range (note chunks outside it
// are skipped without touching their notes), so a partial redraw of a few rows costs a few rows.
//
// Notes are two crisp quads each (border, then the fill inset by 1 device px), colours from a per-clip table
// indexed by velocity; off-grid Ratio / Cents notes get the theme's offGrid border and a small triangle pointing to
// the side of the row their true pitch lies on.

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>

#include "../common/paint.h"
#include "pq/ui/theme.h"
#include "roll_widgets.h"

namespace pq::views::roll {

using namespace pq::ui;

namespace {

Rect frect(float x0, float y0, float x1, float y1) noexcept { return {x0, y0, x1, y1}; }

// Velocity table index.
int velIndex(float v) noexcept { return std::clamp(static_cast<int>(std::lround(v)), 1, 127); }

struct NoteDraw {
  float x0, x1;
  float y0, y1;
  Rgba fill, border;
  int8_t offGrid;
};

void drawNote(DrawList& dl, const NoteDraw& d, Rgba offGridCol) {
  const Rgba border = d.offGrid ? offGridCol : d.border;
  dl.addRectFilled(frect(d.x0, d.y0, d.x1, d.y1), border);
  if (d.x1 - d.x0 > 2.0f && d.y1 - d.y0 > 2.0f) dl.addRectFilled(frect(d.x0 + 1, d.y0 + 1, d.x1 - 1, d.y1 - 1), d.fill);
  if (d.offGrid && d.y1 - d.y0 >= 5.0f && d.x1 - d.x0 >= 8.0f) {
    // A small triangle at the note's start pointing towards the true pitch (up: above the row's pitch).
    const float h = std::min(d.y1 - d.y0 - 2.0f, 6.0f);
    const float cx = d.x0 + 2.0f + h * 0.5f;
    const float cy = (d.y0 + d.y1) * 0.5f;
    const float s = d.offGrid > 0 ? -1.0f : 1.0f;
    dl.addTriangleFilled({cx - h * 0.5f, cy - s * h * 0.35f}, {cx + h * 0.5f, cy - s * h * 0.35f}, {cx, cy + s * h * 0.45f},
                         offGridCol);
  }
}

// Visible content tick range of a device x span.
std::pair<Ticks, Ticks> tickSpan(const RollState& s, int32_t x0, int32_t x1) {
  const double a = s.tickAtX(static_cast<float>(x0)), b = s.tickAtX(static_cast<float>(x1));
  return {static_cast<Ticks>(std::floor(a)) - 1, static_cast<Ticks>(std::ceil(b)) + 1};
}

}  // namespace

// --- rows -----------------------------------------------------------------------------------------------

void paintRowsBackground(PaintContext& ctx, const RollState& s, const IRect& r) {
  const Theme& th = *ctx.res.theme;
  const IRect vis = intersect(r, ctx.clip);
  if (vis.empty()) return;
  const RowModel& rows = s.rows();
  ctx.dl.addRectFilled(vis.toRect(), th.roll.rowAccidental);
  if (rows.empty()) return;
  const auto [first, last] = rows.rowsIn(s.contentY(static_cast<float>(vis.y)), s.contentY(static_cast<float>(vis.bottom())) + 1);
  const bool lines = rows.laneHeight() >= 5;
  // Proportional (T3.12, mos-roll.js render): only the band a note can occupy is shaded, so the unshaded space
  // between bands is the interval; the pitch line runs along the pitch itself, the equave line along the tonic's.
  const bool proportional = rows.layout() == RowLayout::Proportional;
  const float x0 = static_cast<float>(vis.x), x1 = static_cast<float>(vis.right());
  for (size_t i = first; i < last; ++i) {
    const PitchRow& row = rows.row(i);
    const float y0 = static_cast<float>(s.screenY(row.top)), y1 = static_cast<float>(s.screenY(row.bottom));
    Rgba c;
    if (s.scaleHighlight()) c = (row.flags & kRowInScale) ? th.roll.rowScale : th.roll.rowAccidental;
    else c = (row.flags & kRowNatural) ? th.roll.rowNatural : th.roll.rowAccidental;
    if (c != th.roll.rowAccidental && y1 > y0) ctx.dl.addRectFilled(frect(x0, y0, x1, y1), c);
    if (proportional) {
      const float yc = static_cast<float>(s.screenY(row.centre));
      if (row.flags & kRowEquaveStart) ctx.dl.addRectFilled(frect(x0, yc, x1, yc + 1), th.roll.equave);
      else if (lines) ctx.dl.addRectFilled(frect(x0, yc, x1, yc + 1), th.roll.pitchLine);
    } else if (row.flags & kRowEquaveStart) {
      ctx.dl.addRectFilled(frect(x0, y1 - 1, x1, y1), th.roll.equave);
    } else if (lines) {
      ctx.dl.addRectFilled(frect(x0, y1 - 1, x1, y1), th.roll.rowLine);
    }
  }
}

// --- background EDO overlay (T3.12, SPEC §3.2 display grid) ------------------------------------------------------

// One line per overlay step (of the octave) at its pitch: placed through the rows like any cents pitch, so in the
// Equidistant layout the lines are spaced between the degree rows they fall between. Lines closer than 3 device px to
// the previous one are skipped (equave lines never), so a dense zoom costs at most one line per 3 px.
void paintOverlay(PaintContext& ctx, const RollState& s, const IRect& vis, float x0, float x1) {
  const tuning::Edo* e = s.overlay();
  const RowModel& rows = s.rows();
  if (!e || rows.empty() || vis.empty()) return;
  const Theme& th = *ctx.res.theme;
  const double hiCents = rows.centsAtY(s.contentY(static_cast<float>(vis.y)) - 2);
  const double loCents = rows.centsAtY(s.contentY(static_cast<float>(vis.bottom())) + 2);
  const int64_t n = e->n();
  const double step = e->equaveCents() / static_cast<double>(n);
  const int64_t kHi = static_cast<int64_t>(std::ceil(hiCents / step));
  const int64_t kLo = std::max(kHi - 8192, static_cast<int64_t>(std::floor(loCents / step)));
  float lastY = -1e30f;
  for (int64_t k = kHi; k >= kLo; --k) {
    const float y = static_cast<float>(s.screenY(rows.placeCents(e->centsOfStep(k)).centre));
    if (y < static_cast<float>(vis.y) - 1.0f || y > static_cast<float>(vis.bottom()) + 1.0f) continue;
    const bool equave = k % n == 0;
    if (!equave && y - lastY < 3.0f) continue;
    lastY = y;
    ctx.dl.addRectFilled(frect(x0, y, x1, y + 1.0f), equave ? th.roll.edoEquave : th.roll.edoLine);
  }
}

// --- virtual fundamental (SPEC §4.3, Q40) -------------------------------------------------------------------

// The clip's virtual fundamental F (reference · F; 1/D derived, or a lock) as a translucent line labelled with F
// ("1/D", "7/20") and its harmonics k·F as faint guide lines labelled k, wherever they fall in view. Lines closer than 3 device px to the previous one are skipped
// and labels need a text line of room, so a dense high range costs at most one line per 3 px of height.
void paintFundamental(PaintContext& ctx, const RollState& s, const IRect& r, const IRect& vis) {
  const std::optional<tuning::Rational> F = s.virtualFundamental();
  const RowModel& rows = s.rows();
  if (!F || rows.empty()) return;
  const double fn = static_cast<double>(F->num()), fd = static_cast<double>(F->den());
  const Theme& th = *ctx.res.theme;
  const auto [first, last] = rows.rowsIn(s.contentY(static_cast<float>(vis.y)), s.contentY(static_cast<float>(vis.bottom())) + 1);
  if (first >= last) return;
  const double hiCents = rows.row(first).cents + 100.0, loCents = rows.row(last - 1).cents - 100.0;
  const double fund = 1200.0 * std::log2(fn) - 1200.0 * std::log2(fd);  // cents of reference · F (1/D: −1200·log2 D)
  const double kLo = std::max(1.0, std::ceil(fd / fn * std::exp2(loCents / 1200.0)));
  const double kHi = std::floor(fd / fn * std::exp2(hiCents / 1200.0));
  if (kHi < kLo) return;
  TextSystem* ts = ctx.res.text;
  const ThemeFonts* fonts = ctx.res.uiFonts;
  const int32_t lineH = (ts && fonts) ? ts->metrics(fonts->small).lineHeight : 1 << 20;
  const float x0 = static_cast<float>(vis.x), x1 = static_cast<float>(vis.right());
  const int32_t pad = ctx.scale.px(3);
  float lastY = 1e30f, lastLabelY = 1e30f;
  char buf[32];
  const double kEnd = std::min(kHi, kLo + 4096.0);
  for (double k = kLo; k <= kEnd; k += 1.0) {
    const double c = fund + 1200.0 * std::log2(k);
    const float y = static_cast<float>(s.screenY(rows.placeCents(c).centre));
    if (y < static_cast<float>(vis.y) - 1.0f || y > static_cast<float>(vis.bottom()) + 1.0f) continue;
    const bool fundamentalLine = k == 1.0;
    if (!fundamentalLine && std::fabs(lastY - y) < 3.0f) continue;
    lastY = y;
    if (fundamentalLine) ctx.dl.addRectFilled({x0, y - 1.0f, x1, y + 1.0f}, th.roll.fundamental);
    else ctx.dl.addRectFilled({x0, y, x1, y + 1.0f}, th.roll.harmonic);
    if (std::fabs(lastLabelY - y) >= static_cast<float>(lineH) && fonts) {
      lastLabelY = y;
      const int n = fundamentalLine ? std::snprintf(buf, sizeof buf, "%lld/%lld", static_cast<long long>(F->num()),
                                                    static_cast<long long>(F->den()))
                                    : std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(k));
      paint::text(ctx, fonts->small, {r.x, static_cast<int32_t>(y) - lineH, ctx.scale.px(48), lineH},
                  std::string_view(buf, static_cast<size_t>(n)), th.roll.fundamental, TextAlign::Left, pad, 0);
    }
  }
}

// --- note area ------------------------------------------------------------------------------------------

void NoteAreaWidget::paint(PaintContext& ctx) {
  const Theme* th = ctx.res.theme;
  if (!th) return;
  const IRect r = rect();
  const IRect vis = intersect(r, ctx.clip);
  if (vis.empty()) return;
  DrawList& dl = ctx.dl;
  dl.pushClip(r.toRect());
  if (!s_.hasClip()) {
    dl.addRectFilled(vis.toRect(), th->surface.panel);
    dl.popClip();
    return;
  }
  paintRowsBackground(ctx, s_, r);
  const float vy0 = static_cast<float>(vis.y), vy1 = static_cast<float>(vis.bottom());
  const auto [from, to] = tickSpan(s_, vis.x, vis.right());

  // Grid lines (the editing grid at this zoom).
  const Grid g = s_.grid();
  if (g.enabled()) {
    for (const GridLine& line : g.lines(from, to)) {
      const float x = std::round(s_.xOfTick(static_cast<double>(line.tick)));
      const Rgba c = line.strength == GridStrength::Bar    ? th->grid.bar
                     : line.strength == GridStrength::Beat ? th->grid.beat
                                                           : th->grid.sub;
      dl.addRectFilled(frect(x, vy0, x + 1.0f, vy1), c);
    }
  }

  paintOverlay(ctx, s_, vis, static_cast<float>(vis.x), static_cast<float>(vis.right()));
  paintFundamental(ctx, s_, r, vis);

  // Notes. A Move / Copy drag shows the selected notes displaced: look as far back / ahead as the drag reaches.
  const DragState& dr = s_.drag;
  const bool shifting = dr.kind == DragKind::Move || dr.kind == DragKind::Copy;
  const Ticks lookFrom = shifting ? std::min(from, from - dr.dt) : from;
  const Ticks lookTo = shifting ? std::max(to, to - dr.dt) : to;
  const NoteColours& col = s_.colours();
  const bool anySelected = !s_.selection().empty();
  const bool velPreview = dr.kind == DragKind::Velocity && !s_.showChance();
  const float vx0 = static_cast<float>(vis.x) - 4.0f, vx1 = static_cast<float>(vis.right()) + 4.0f;
  // Proportional rows may overlap (a collapsing MOS): notes are painted by their row's z, small steps under large
  // ones, selected notes on top (mos-roll.js drawOrder), each z in time order. A counting sort over the visible notes
  // into reused buffers; the other layouts cannot overlap and paint directly.
  const bool zOrder = s_.rows().layout() == RowLayout::Proportional;
  std::vector<ZNote>& zNotes = s_.zNotes;
  zNotes.clear();
  auto emit = [&](const doc::Note& n, Ticks start, Ticks length, const tuning::PitchId& pitch, bool sel) {
    float x0 = std::round(s_.xOfTick(static_cast<double>(start)));
    float x1 = std::max(x0 + 3.0f, std::round(s_.xOfTick(static_cast<double>(start + length))));
    if (x1 <= vx0 || x0 >= vx1) return;
    const NotePlace pl = s_.place(pitch);
    if (!pl.ok) return;
    const float y0 = static_cast<float>(s_.screenY(pl.top)), y1 = static_cast<float>(s_.screenY(pl.bottom));
    if (y1 <= vy0 || y0 >= vy1) return;
    x0 = std::max(x0, vx0);
    x1 = std::min(x1, vx1);
    NoteDraw d{x0, x1, y0, y1, 0, 0, pl.offGrid};
    if (!n.enabled()) {
      d.fill = col.disabledFill;
      d.border = col.disabledBorder;
    } else {
      const int v = velIndex(velPreview && sel ? s_.previewValue(n) : n.velocity);
      d.fill = sel ? col.selFill[v] : col.fill[v];
      d.border = sel ? col.selBorder : col.border[v];
    }
    if (zOrder) zNotes.push_back({d.x0, d.x1, d.y0, d.y1, d.fill, d.border, d.offGrid, static_cast<uint16_t>(pl.z + (sel ? 256 : 0))});
    else drawNote(dl, d, th->roll.offGrid);
  };
  s_.forNotesIn(lookFrom, lookTo, [&](const doc::Note& n) {
    const bool sel = anySelected && s_.selected(n.id);
    if (sel && dr.kind != DragKind::None) {
      if (const std::optional<RollState::Preview> pv = s_.preview(n)) {
        if (dr.kind == DragKind::Copy) emit(n, n.start, n.length, n.pitch, false);  // the original stays
        emit(n, pv->start, pv->length, pv->pitch, true);
        return;
      }
    }
    if (n.start + n.length > from && n.start < to) emit(n, n.start, n.length, n.pitch, sel);
  });
  if (zOrder && !zNotes.empty()) {
    uint32_t count[513] = {};
    for (const ZNote& z : zNotes) ++count[z.key + 1];
    for (size_t k = 1; k < 513; ++k) count[k] += count[k - 1];
    std::vector<ZNote>& sorted = s_.zSorted;
    sorted.resize(zNotes.size());
    for (const ZNote& z : zNotes) sorted[count[z.key]++] = z;  // stable: time order within one z
    for (const ZNote& z : sorted) drawNote(dl, {z.x0, z.x1, z.y0, z.y1, z.fill, z.border, z.offGrid}, th->roll.offGrid);
  }

  // Draw-mode preview: the notes the drag will add.
  if (dr.kind == DragKind::Draw)
    for (const DrawnNote& dn : dr.drawn) {
      const NotePlace pl = s_.place(s_.drawnPitch(dn));
      if (!pl.ok) continue;
      const float x0 = std::round(s_.xOfTick(static_cast<double>(dn.start)));
      const float x1 = std::max(x0 + 3.0f, std::round(s_.xOfTick(static_cast<double>(dn.start + dn.length))));
      drawNote(dl, {x0, x1, static_cast<float>(s_.screenY(pl.top)), static_cast<float>(s_.screenY(pl.bottom)),
                    col.selFill[100], col.selBorder, pl.offGrid},
               th->roll.offGrid);
    }

  // Outside the played region (before the start / loop start, after the end / loop end): dimmed, notes included.
  const float rx0 = std::round(s_.xOfTick(static_cast<double>(s_.regionStart())));
  const float rx1 = std::round(s_.xOfTick(static_cast<double>(s_.regionEnd())));
  if (rx0 > static_cast<float>(vis.x))
    dl.addRectFilled(frect(static_cast<float>(vis.x), vy0, std::min(rx0, static_cast<float>(vis.right())), vy1), th->roll.outside);
  if (rx1 < static_cast<float>(vis.right()))
    dl.addRectFilled(frect(std::max(rx1, static_cast<float>(vis.x)), vy0, static_cast<float>(vis.right()), vy1), th->roll.outside);

  if (dr.kind == DragKind::Rubber && dr.moved) {
    const Rect band = dr.rubber.toRect();
    dl.addRectFilled(band, withAlpha(th->selection.fill, 70));
    dl.addRectStroke(band, th->selection.fill);
  }
  dl.popClip();
}

// --- keys -----------------------------------------------------------------------------------------------

void KeysWidget::paint(PaintContext& ctx) {
  const Theme* th = ctx.res.theme;
  if (!th) return;
  const IRect r = rect();
  const IRect vis = intersect(r, ctx.clip);
  if (vis.empty()) return;
  DrawList& dl = ctx.dl;
  dl.pushClip(r.toRect());
  dl.addRectFilled(vis.toRect(), th->surface.panel);
  const RowModel& rows = s_.rows();
  if (!s_.hasClip() || rows.empty()) {
    dl.popClip();
    return;
  }
  if (rows.scaleLayout()) {
    paintScaleKeys(ctx, r, vis);
    dl.popClip();
    return;
  }
  const ThemeFonts* fonts = ctx.res.uiFonts;
  TextSystem* ts = ctx.res.text;
  const FontId font = fonts ? fonts->small : kNoFont;
  const int32_t fontPx = (ts && font != kNoFont) ? static_cast<int32_t>(ts->metrics(font).pixelSize) : 12;
  const float x0 = static_cast<float>(r.x), x1 = static_cast<float>(r.right());
  const float blackX1 = x0 + std::round(static_cast<float>(r.w) * 0.5f);
  const int32_t pad = ctx.scale.px(4);
  const auto [first, last] = rows.rowsIn(s_.contentY(static_cast<float>(vis.y)), s_.contentY(static_cast<float>(vis.bottom())) + 1);
  const int32_t rowH = rows.laneHeight();
  // Labels (Q2): every row when rows are tall enough for the text; else the white keys when they leave room
  // around them (a label may overhang into the unlabelled rows next to it); equave rows always. On a
  // 12edo-compatible EDO only the C rows are named until the rows are tall (Live labels the Cs).
  const bool twelve = twelveCompatibleEdo(s_.tuning()) != 0;
  const bool allLabels = rowH >= fontPx + (twelve ? 6 : 0);
  const bool naturalLabels = !twelve && rowH >= 6;
  const Rgba accent = th->accent.primary;
  for (size_t i = first; i < last; ++i) {
    const PitchRow& row = rows.row(i);
    const float y0 = static_cast<float>(s_.screenY(row.top)), y1 = static_cast<float>(s_.screenY(row.bottom));
    const bool natural = (row.flags & kRowNatural) != 0;
    dl.addRectFilled(frect(x0, y0, x1, y1), th->roll.keyWhite);
    if (!natural) dl.addRectFilled(frect(x0, y0, blackX1, y1), th->roll.keyBlack);
    // Separators: between two white keys (E|F, B|C), and the equave marker.
    const bool nextNatural = i + 1 < rows.size() && (rows.row(i + 1).flags & kRowNatural);
    if (row.flags & kRowEquaveStart) dl.addRectFilled(frect(x0, y1 - 1, x1, y1), th->roll.equave);
    else if (natural && nextNatural && rowH >= 4) dl.addRectFilled(frect(x0, y1 - 1, x1, y1), th->control.border);
    if (s_.scaleHighlight() && (row.flags & kRowInScale))
      dl.addRectFilled(frect(x1 - static_cast<float>(ctx.scale.px(3)), y0, x1, y1 - 1), accent);
  }
  if (font != kNoFont)
    for (size_t i = first; i < last; ++i) {  // labels after every key, so an overhanging label stays on top
      const PitchRow& row = rows.row(i);
      const bool equave = (row.flags & kRowEquaveStart) != 0;
      const bool natural = (row.flags & kRowNatural) != 0;
      const bool show = allLabels || equave || (naturalLabels && natural);
      const std::string& label = rows.label(i);
      if (!show || label.empty()) continue;
      const int32_t y0 = s_.screenY(row.top);
      const int32_t h = std::max(row.height(), fontPx);
      const IRect box{r.x, y0 + (row.height() - h) / 2, r.w, h};
      const int32_t right = pad + (s_.scaleHighlight() ? ctx.scale.px(3) : 0);
      paint::text(ctx, font, box, label, th->roll.keyText, TextAlign::Right, pad, right);
      if (const std::string& eq = rows.equaveLabel(i); !eq.empty() && ts) {
        // At the left on a white tab as tall as the text (rows may be shorter than it), clear of the step label.
        const int32_t tw = ts->measure(fonts->bodyStrong, eq) + 2 * pad;
        const int32_t th2 = std::min(h, ts->metrics(fonts->bodyStrong).lineHeight);
        const int32_t ty = s_.screenY(row.centre) - th2 / 2;
        dl.addRectFilled(frect(x0, static_cast<float>(ty), x0 + static_cast<float>(tw), static_cast<float>(ty + th2)),
                         th->roll.keyWhite);
        paint::text(ctx, fonts->bodyStrong, {r.x, ty, tw, th2}, eq, th->roll.keyText, TextAlign::Left, pad, 0);
      }
    }
  dl.popClip();
}

// --- scale keys (T3.12) ------------------------------------------------------------------------------------------

void stepClassColours(const Theme& th, std::span<const StepClass> classes, double hardness, std::vector<Rgba>& out) {
  out.clear();
  if (classes.empty()) return;
  // drawMOSCircle: softness = 1 − (L/s − 1) / 2, clamped; s = lerp(L, keySmall, 1 − softness).
  const double softness = std::clamp(1.0 - (hardness - 1.0) / 2.0, 0.0, 1.0);
  const Rgba Lc = th.roll.keyLarge;
  const Rgba sc = lerp(Lc, th.roll.keySmall, static_cast<float>(1.0 - softness));
  out.assign(classes.size(), Lc);
  if (classes.size() == 1) return;
  out.back() = sc;
  // Intermediate sizes (classes are sorted descending): between the nearest known neighbours by position, pulled
  // toward keyExtra by how far the size stands from the next smaller one (the circle's pairSoftness), largest first.
  const size_t last = classes.size() - 1;
  for (size_t i = 1; i < last; ++i) {
    const double st = classes[i].cents, above = classes[i - 1].cents, below = classes[last].cents;
    const double range = above - below;
    const double t = range > 0.01 ? (st - below) / range : 0.5;
    const Rgba blend = lerp(out[last], out[i - 1], static_cast<float>(t));
    const double pairRatio = below > 0.01 ? st / below : 20.0;
    const double pairSoftness = std::clamp(1.0 - (pairRatio - 1.0) / 2.0, 0.0, 1.0);
    const Rgba distinct = lerp(blend, th.roll.keyExtra, 0.3f);
    out[i] = lerp(distinct, blend, static_cast<float>(pairSoftness));
  }
}

// Gutter of a scale layout (mos-roll.js drawGutter): one key per row, exactly the row's block (in the Proportional
// layout the gaps between keys are the intervals), coloured by step class with the hardness blend; the tonic in
// roll.keyTonic. Degree numbers sit at each row's pitch where they do not crowd the previous one; the equave number on
// a tab at degree 0. Overlay ticks at the right edge.
void KeysWidget::paintScaleKeys(PaintContext& ctx, const IRect& r, const IRect& vis) {
  const Theme* th = ctx.res.theme;
  DrawList& dl = ctx.dl;
  const RowModel& rows = s_.rows();
  stepClassColours(*th, rows.stepClasses(), rows.hardness(), classColours_);
  const float x0 = static_cast<float>(r.x), x1 = static_cast<float>(r.right());
  const auto [first, last] = rows.rowsIn(s_.contentY(static_cast<float>(vis.y)), s_.contentY(static_cast<float>(vis.bottom())) + 1);
  const Rgba accent = th->accent.primary;
  const float hl = static_cast<float>(ctx.scale.px(3));
  for (size_t i = first; i < last; ++i) {
    const PitchRow& row = rows.row(i);
    float y0 = static_cast<float>(s_.screenY(row.top)), y1 = static_cast<float>(s_.screenY(row.bottom));
    if (y1 <= y0) continue;
    if (y1 - y0 > 5.0f) y1 -= 1.0f;  // a hairline between keys once there is room
    const Rgba c = (row.flags & kRowEquaveStart) ? th->roll.keyTonic
                   : row.stepClass < classColours_.size() ? classColours_[row.stepClass]
                                                          : th->roll.keyLarge;
    dl.addRectFilled(frect(x0, y0, x1, y1), c);
    if (s_.scaleHighlight() && (row.flags & kRowInScale)) dl.addRectFilled(frect(x1 - hl, y0, x1, y1), accent);
  }
  // Overlay ticks: the background EDO's lines continued into the gutter's right edge.
  if (s_.overlay()) paintOverlay(ctx, s_, vis, x1 - static_cast<float>(ctx.scale.px(6)), x1);
  const ThemeFonts* fonts = ctx.res.uiFonts;
  TextSystem* ts = ctx.res.text;
  if (!fonts || !ts) return;
  const int32_t lineH = ts->metrics(fonts->small).lineHeight;
  const int32_t pad = ctx.scale.px(4);
  int32_t lastLabel = INT32_MIN / 2;
  for (size_t i = first; i < last; ++i) {
    const PitchRow& row = rows.row(i);
    const int32_t yc = s_.screenY(row.centre);
    const bool equave = (row.flags & kRowEquaveStart) != 0;
    if (!equave && yc - lastLabel < lineH) continue;  // mos-roll.js: skip a label too close to the previous one
    if (equave && yc - lastLabel < lineH / 2) continue;
    const std::string& label = rows.label(i);
    if (label.empty()) continue;
    lastLabel = yc;
    const Rgba keyCol = equave ? th->roll.keyTonic
                        : row.stepClass < classColours_.size() ? classColours_[row.stepClass]
                                                               : th->roll.keyLarge;
    // Text over the key when it is tall enough, else over the gutter background beside it: pick the readable tone.
    const bool onKey = row.height() >= lineH - 2;
    const Rgba textCol = onKey ? clipTextFor(*th, keyCol) : th->text.secondary;
    const IRect box{r.x, yc - lineH / 2, r.w, lineH};
    paint::text(ctx, fonts->small, box, label, textCol, TextAlign::Right, pad, pad + (s_.overlay() ? ctx.scale.px(6) : 0));
    if (const std::string& eq = rows.equaveLabel(i); !eq.empty()) {
      const int32_t tw = ts->measure(fonts->bodyStrong, eq) + 2 * pad;
      const int32_t th2 = ts->metrics(fonts->bodyStrong).lineHeight;
      const int32_t ty = yc - th2 / 2;
      dl.addRectFilled(frect(x0, static_cast<float>(ty), x0 + static_cast<float>(tw), static_cast<float>(ty + th2)),
                       th->roll.keyTonic);
      paint::text(ctx, fonts->bodyStrong, {r.x, ty, tw, th2}, eq, th->roll.keyText, TextAlign::Left, pad, 0);
    }
  }
}

// --- ruler ----------------------------------------------------------------------------------------------

void RulerWidget::paint(PaintContext& ctx) {
  const Theme* th = ctx.res.theme;
  if (!th) return;
  const IRect r = rect();
  const IRect vis = intersect(r, ctx.clip);
  if (vis.empty()) return;
  DrawList& dl = ctx.dl;
  dl.pushClip(r.toRect());
  dl.addRectFilled(vis.toRect(), th->surface.panel);
  if (!s_.hasClip()) {
    dl.popClip();
    return;
  }
  const int32_t braceH = ctx.scale.px(kBraceHeight);
  const float top = static_cast<float>(r.y), braceBottom = static_cast<float>(r.y + braceH);
  const float bottom = static_cast<float>(r.bottom());
  // Marker strip.
  dl.addRectFilled(frect(static_cast<float>(vis.x), top, static_cast<float>(vis.right()), braceBottom), th->surface.sunken);
  const doc::ClipLoop loop = s_.shownLoop();
  {
    const float lx0 = std::round(s_.xOfTick(static_cast<double>(loop.start)));
    const float lx1 = std::round(s_.xOfTick(static_cast<double>(loop.end)));
    const float inset = static_cast<float>(ctx.scale.px(2));
    const Rgba c = loop.enabled ? th->control.thumb : th->control.fill;
    dl.addRectFilled(frect(lx0, top + inset, std::max(lx0 + 2.0f, lx1), braceBottom - inset), c);
    // Brace ends: small notches.
    const Rgba edge = loop.enabled ? th->control.needle : th->control.track;
    dl.addRectFilled(frect(lx0, top + inset, lx0 + 2.0f, braceBottom), edge);
    dl.addRectFilled(frect(std::max(lx0 + 2.0f, lx1) - 2.0f, top + inset, std::max(lx0 + 2.0f, lx1), braceBottom), edge);
  }
  // Start marker (a flag pointing right) and, for an unlooped clip, the end marker (pointing left).
  const float fh = static_cast<float>(braceH) - 2.0f;
  const float sx = std::round(s_.xOfTick(static_cast<double>(s_.shownContentStart())));
  dl.addTriangleFilled({sx, top + 1.0f}, {sx + fh * 0.8f, top + 1.0f + fh * 0.5f}, {sx, top + 1.0f + fh}, th->text.primary);
  if (!loop.enabled) {
    const float ex = std::round(s_.xOfTick(static_cast<double>(s_.shownContentStart() + s_.shownLength())));
    dl.addTriangleFilled({ex, top + 1.0f}, {ex, top + 1.0f + fh}, {ex - fh * 0.8f, top + 1.0f + fh * 0.5f}, th->text.primary);
  }

  // Time ruler: ticks from the editing grid, numbers on a coarser adaptive grid so labels never collide.
  const auto [from, to] = tickSpan(s_, vis.x, vis.right());
  const Grid g = s_.grid();
  const float scale = ctx.scale.factor;
  if (g.enabled())
    for (const GridLine& line : g.lines(from, to)) {
      const float x = std::round(s_.xOfTick(static_cast<double>(line.tick)));
      const float h = line.strength == GridStrength::Bar    ? bottom - braceBottom
                      : line.strength == GridStrength::Beat ? std::round(6.0f * scale)
                                                            : std::round(3.0f * scale);
      dl.addRectFilled(frect(x, bottom - h, x + 1.0f, bottom), th->text.disabled);
    }
  const ThemeFonts* fonts = ctx.res.uiFonts;
  if (fonts && ctx.res.text) {
    const Grid labels = Grid::adaptive(56.0, s_.sigs(), s_.pxPerTick() / static_cast<double>(scale));
    char buf[kRulerLabelMaxChars];
    const int32_t labelH = r.bottom() - (r.y + braceH);
    for (const GridLine& line : labels.lines(from - kTicksPerWhole, to)) {
      const float x = std::round(s_.xOfTick(static_cast<double>(line.tick)));
      dl.addRectFilled(frect(x, braceBottom, x + 1.0f, bottom), th->text.disabled);
      const size_t n = formatRulerPosition(rulerPosition(line), buf);
      paint::text(ctx, fonts->small, {static_cast<int32_t>(x) + ctx.scale.px(3), r.y + braceH, ctx.scale.px(60), labelH},
                  std::string_view(buf, n), th->text.secondary, TextAlign::Left);
    }
  }
  dl.addRectFilled(frect(static_cast<float>(vis.x), bottom - 1.0f, static_cast<float>(vis.right()), bottom), th->surface.divider);
  dl.popClip();
}

// --- velocity lane --------------------------------------------------------------------------------------

float VelocityLaneWidget::valueRange() const noexcept { return s_.showChance() ? 1.0f : 127.0f; }

float VelocityLaneWidget::yOfValue(float value) const noexcept {
  const IRect r = rect();
  const float pad = 4.0f * (tree() ? tree()->scale() : 1.0f);
  const float h = static_cast<float>(r.h) - 2.0f * pad;
  return std::round(static_cast<float>(r.bottom()) - pad - h * (value / valueRange()));
}

void VelocityLaneWidget::paint(PaintContext& ctx) {
  const Theme* th = ctx.res.theme;
  if (!th) return;
  const IRect r = rect();
  const IRect vis = intersect(r, ctx.clip);
  if (vis.empty()) return;
  DrawList& dl = ctx.dl;
  dl.pushClip(r.toRect());
  dl.addRectFilled(vis.toRect(), th->roll.rowAccidental);
  if (!s_.hasClip()) {
    dl.popClip();
    return;
  }
  const float x0 = static_cast<float>(vis.x), x1 = static_cast<float>(vis.right());
  const float range = valueRange();
  for (int q = 1; q <= 4; ++q) {
    const float y = yOfValue(range * static_cast<float>(q) / 4.0f);
    dl.addRectFilled(frect(x0, y, x1, y + 1.0f), q == 4 ? th->grid.beat : th->grid.sub);
  }
  // Grid lines continue from the note area (bars and beats only).
  const auto [from, to] = tickSpan(s_, vis.x, vis.right());
  const Grid g = s_.grid();
  if (g.enabled())
    for (const GridLine& line : g.lines(from, to)) {
      if (line.strength != GridStrength::Bar && line.strength != GridStrength::Beat) continue;
      const float x = std::round(s_.xOfTick(static_cast<double>(line.tick)));
      dl.addRectFilled(frect(x, static_cast<float>(vis.y), x + 1.0f, static_cast<float>(vis.bottom())),
                       line.strength == GridStrength::Bar ? th->grid.bar : th->grid.sub);
    }
  const NoteColours& col = s_.colours();
  const bool anySelected = !s_.selection().empty();
  const bool chance = s_.showChance();
  const float scale = ctx.scale.factor;
  const float head = std::max(3.0f, std::round(5.0f * scale));
  const float baseY = yOfValue(0.0f);
  const DragState& dr = s_.drag;
  const bool shifting = dr.kind == DragKind::Move || dr.kind == DragKind::Copy;
  auto stem = [&](const doc::Note& n, Ticks start, bool sel) {
    const float x = std::round(s_.xOfTick(static_cast<double>(start)));
    if (x < x0 - head || x > x1 + head) return;
    const float v = s_.previewValue(n);
    const float y = yOfValue(v);
    Rgba c;
    if (!n.enabled()) c = col.disabledFill;
    else if (chance) c = sel ? th->text.primary : th->accent.solo;
    else c = sel ? col.selFill[velIndex(v)] : col.fill[velIndex(v)];
    dl.addRectFilled(frect(x, y, x + 1.0f, baseY), c);
    dl.addRectFilled(frect(x - std::floor(head * 0.5f), y - std::floor(head * 0.5f), x - std::floor(head * 0.5f) + head,
                           y - std::floor(head * 0.5f) + head),
                     sel ? th->text.primary : col.border[velIndex(v)]);
    dl.addRectFilled(frect(x - std::floor(head * 0.5f) + 1.0f, y - std::floor(head * 0.5f) + 1.0f,
                           x - std::floor(head * 0.5f) + head - 1.0f, y - std::floor(head * 0.5f) + head - 1.0f),
                     c);
  };
  const Ticks lookFrom = shifting ? std::min(from, from - dr.dt) : from;
  const Ticks lookTo = shifting ? std::max(to, to - dr.dt) : to;
  // A stem sits at its note's start: only notes starting in view (plus a head's width) are drawn.
  const Ticks pad = static_cast<Ticks>(std::ceil(head / std::max(s_.pxPerTick(), 1e-9)));
  s_.forNotesIn(lookFrom - pad, lookTo + pad, [&](const doc::Note& n) {
    const bool sel = anySelected && s_.selected(n.id);
    Ticks start = n.start;
    if (sel && shifting) {
      if (dr.kind == DragKind::Copy) stem(n, n.start, false);
      start += dr.dt;
    }
    stem(n, start, sel);
  });
  dl.addRectFilled(frect(x0, static_cast<float>(r.y), x1, static_cast<float>(r.y) + 1.0f), th->surface.divider);
  dl.popClip();
}

// --- panels ---------------------------------------------------------------------------------------------

void ClipBoxPanel::paint(PaintContext& ctx) {
  const Theme* th = ctx.res.theme;
  if (!th) return;
  const IRect r = rect();
  DrawList& dl = ctx.dl;
  dl.pushClip(r.toRect());
  dl.addRectFilled(r.toRect(), th->surface.panel);
  const ThemeFonts* f = ctx.res.uiFonts;
  const doc::NoteClip* c = s_.clip();
  if (!c || !f) {
    dl.popClip();
    return;
  }
  const int32_t pad = ctx.scale.px(6);
  const int32_t lh = ctx.scale.px(18);
  // Title bar in the clip colour (Live's clip box header).
  const Rgba clipCol = s_.clipColour();
  const IRect title{r.x, r.y, r.w, ctx.scale.px(20)};
  dl.addRectFilled(title.toRect(), clipCol);
  paint::text(ctx, f->bodyStrong, title, c->name.empty() ? std::string_view("MIDI Clip") : std::string_view(c->name),
              clipTextFor(*th, clipCol), TextAlign::Left, pad, pad);
  // The first rows are widgets: the tuning chip (T3.14), then for scale tunings layout / EDO grid / generator (T3.12).
  int32_t y = title.bottom() + ctx.scale.px(4) + lh;
  for (size_t i = 0; i < childCount(); ++i) {
    const Widget& w = child(i);
    if (w.visible() && w.height().kind == SizeKind::Fixed && &w != loop_ && &w != tuning_) y = std::max(y, w.rect().bottom() + ctx.scale.px(2));
  }
  auto line = [&](std::string_view key, std::string_view value) {
    const IRect row{r.x, y, r.w, lh};
    paint::text(ctx, f->small, row, key, th->text.secondary, TextAlign::Left, pad, pad);
    paint::text(ctx, f->small, row, value, th->text.primary, TextAlign::Right, pad, pad);
    y += lh;
  };
  char buf[64];
  std::snprintf(buf, sizeof buf, "%d / %d", c->signature.numerator, c->signature.denominator);
  line("Signature", buf);
  auto pos = [&](Ticks t) {
    char b[kRulerLabelMaxChars];
    const RulerPosition p = rulerPosition(s_.sigs(), t);
    std::snprintf(b, sizeof b, "%lld.%d.%d", static_cast<long long>(p.bar), p.beat, p.sixteenth);
    return std::string(b);
  };
  line("Start", pos(s_.shownContentStart()));
  if (s_.shownLoop().enabled) {
    line("Loop", pos(s_.shownLoop().start));
    line("Loop End", pos(s_.shownLoop().end));
  } else {
    line("End", pos(s_.shownContentStart() + s_.shownLength()));
  }
  std::snprintf(buf, sizeof buf, "%zu", c->notes.size());
  line("Notes", buf);
  std::snprintf(buf, sizeof buf, "%zu", s_.selection().size());
  if (!s_.selection().empty()) line("Selected", buf);
  dl.popClip();
}

void CornerPanel::paint(PaintContext& ctx) {
  if (const Theme* th = ctx.res.theme) {
    ctx.dl.addRectFilled(rect().toRect(), th->surface.panel);
    if (s_.ctx.drawMode()) {
      // Draw mode indicator: a pencil-coloured bar along the bottom (Live highlights its draw-mode switch).
      const IRect r = rect();
      ctx.dl.addRectFilled(Rect{static_cast<float>(r.x), static_cast<float>(r.bottom() - ctx.scale.px(2)),
                                static_cast<float>(r.right()), static_cast<float>(r.bottom())},
                           th->accent.primary);
    }
  }
}

void LaneHeaderWidget::paint(PaintContext& ctx) {
  if (const Theme* th = ctx.res.theme) ctx.dl.addRectFilled(rect().toRect(), th->surface.panel);
}

}  // namespace pq::views::roll
