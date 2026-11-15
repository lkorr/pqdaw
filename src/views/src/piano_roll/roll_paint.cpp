// Painting of the clip view's parts (T3.11). Every part paints only inside its rect (pushClip) and only what meets
// PaintContext::clip: rows by binary search, grid lines and notes by the visible tick range (note chunks outside it
// are skipped without touching their notes), so a partial redraw of a few rows costs a few rows.
//
// T7.12: every note is a line along its pitch, painted by one routine (drawNoteLine): a faint band over the row it
// is grabbed in, a body a few px thick centred on the exact pitch (between rows for a Ratio / Cents pitch off the
// grid, along the pitch path for a note with a curve), round ends, and a thicker head at its start
// so repeated notes on one pitch read as separate notes. Colours from a per-clip table indexed by velocity; the head
// of an off-grid note takes the theme's offGrid colour. Metrics: the theme's roll.noteLine* tokens.

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>

#include "../common/labels.h"
#include "../common/paint.h"
#include "../common/pitch_path.h"
#include "pq/document/automation_curve.h"
#include "pq/tuning/ji.h"
#include "pq/ui/lines.h"
#include "pq/ui/theme.h"
#include "roll_playhead.h"
#include "roll_widgets.h"

namespace pq::views::roll {

using namespace pq::ui;

namespace {

Rect frect(float x0, float y0, float x1, float y1) noexcept { return {x0, y0, x1, y1}; }

// Velocity table index.
int velIndex(float v) noexcept { return std::clamp(static_cast<int>(std::lround(v)), 1, 127); }

// The line metrics at this UI scale (theme tokens roll.noteLine*, logical px).
struct LineStyle {
  float thick;     // body thickness
  float headH;     // head (start cap) height
  float headW;     // head length
  float pathBand;  // the grab band of a note on a pitch path (its hit thickness)
  uint8_t bandAlpha;
};

LineStyle lineStyle(const Theme& th, const RollState& s, float scale) noexcept {
  return {std::max(1.0f, std::round(th.roll.noteLine * scale)), std::max(1.0f, std::round(th.roll.noteLineHead * scale)),
          std::max(1.0f, std::round(th.roll.noteLineHeadWidth * scale)), 2.0f * s.pathHalfThickness(),
          static_cast<uint8_t>(std::clamp(th.roll.noteLineBand, 0.0f, 255.0f))};
}

// One note: band, body, head. `path` (≥ 2 points, device px, left to right) is the pitch path of a note that has
// one; without it the note is straight at d.yc. No allocation: a handful of draw-list primitives per note, and a
// note narrower than its thickness is two crisp quads (a zoomed-out clip stays cheap).
void drawNoteLine(DrawList& dl, const LineStyle& st, const NoteLine& d, std::span<const Vec2> path = {}) {
  const float w = d.x1 - d.x0, rowH = d.y1 - d.y0;
  if (w <= 0.0f || rowH <= 0.0f) return;
  // Never thicker than the row: small rows keep a px of air on each side while there is room for it.
  const float t = std::min(st.thick, std::clamp(rowH - 2.0f, std::min(2.0f, rowH), rowH));
  const float hh = std::min(st.headH, rowH);
  const float hw = std::min(st.headW, w);
  const bool head = hh >= t + 2.0f;
  if (path.size() >= 2) {
    if (st.bandAlpha) strokePath(dl, path, withAlpha(d.fill, st.bandAlpha), st.pathBand);
    strokePath(dl, path, d.fill, t);
    const Vec2 a = path.front(), b = path.back();
    if (b.x >= d.x1 - 0.5f) dl.addCircleFilled(b, 0.5f * t, d.fill);
    if (a.x <= d.x0 + 0.5f) {  // the note's start is in view
      if (head) dl.addRoundedRectFilled(frect(a.x, a.y - 0.5f * hh, a.x + hw, a.y + 0.5f * hh), 0.5f * std::min(hw, hh), d.head);
      else dl.addCircleFilled(a, 0.5f * t, d.fill);
    }
    return;
  }
  if (st.bandAlpha) dl.addRectFilled(frect(d.x0, d.y0, d.x1, d.y1), withAlpha(d.fill, st.bandAlpha));
  const float top = std::round(d.yc - 0.5f * t);
  const bool tiny = w < t + 2.0f || t < 3.0f;  // no room for round ends
  if (!(head && hw >= w)) {
    if (tiny) dl.addRectFilled(frect(d.x0, top, d.x1, top + t), d.fill);
    else dl.addRoundedRectFilled(frect(d.x0, top, d.x1, top + t), 0.5f * t, d.fill);
  }
  if (head) {
    const float htop = std::round(d.yc - 0.5f * hh);
    if (tiny) dl.addRectFilled(frect(d.x0, htop, d.x0 + hw, htop + hh), d.head);
    else dl.addRoundedRectFilled(frect(d.x0, htop, d.x0 + hw, htop + hh), 0.5f * std::min(hw, hh), d.head);
  }
}

// Px between samples along a stored free curve with bowed segments (logical); every other path is its cached
// straight segments.
constexpr float kPathSampleStep = 2.0f;
// A path note narrower than kPathTinyPx is one quad over its wave's extent; narrower than kPathSmallPx its line
// alone (no grab band, no round ends, no head). Logical px.
constexpr float kPathTinyPx = 10.0f;
constexpr float kPathSmallPx = 20.0f;

// Visible content tick range of a device x span.
std::pair<Ticks, Ticks> tickSpan(const RollState& s, int32_t x0, int32_t x1) {
  // A pixel either side: lines are drawn at round(x), so one up to half a pixel left of x0 still lands in it
  // (bug-roll-playhead: a playhead column's partial repaint dropped the grid line at its edge).
  const double a = s.tickAtX(static_cast<float>(x0 - 1)), b = s.tickAtX(static_cast<float>(x1 + 1));
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

namespace {

// The harmonics k·F (k ≥ 1) of the clip's virtual fundamental between two pitches (cents from the reference), as
// f(k, screen y) top to bottom; lines closer than 3 device px to the previous one are skipped (a dense high range costs
// at most one per 3 px). At most 4096 per call.
template <class F>
void forEachHarmonic(const RollState& s, const RollState::Harmonics& H, int32_t contentTop, int32_t contentBottom, float yMin, float yMax, F&& f) {
  const tuning::Rational& Fr = H.ratio;
  const RowModel& rows = s.rows();
  const auto [first, last] = rows.rowsIn(contentTop, contentBottom + 1);
  if (first >= last) return;
  const double fn = static_cast<double>(Fr.num()), fd = static_cast<double>(Fr.den());
  // The fundamental stands on its base (the reference at 0 cents, or the active JI group's anchor note).
  const double hiCents = rows.row(first).cents + 100.0 - H.baseCents, loCents = rows.row(last - 1).cents - 100.0 - H.baseCents;
  const double fund = H.baseCents + 1200.0 * std::log2(fn) - 1200.0 * std::log2(fd);  // cents of base · F (1/D: −1200·log2 D)
  const double kLo = std::max(1.0, std::ceil(fd / fn * std::exp2(loCents / 1200.0)));
  const double kHi = std::floor(fd / fn * std::exp2(hiCents / 1200.0));
  if (kHi < kLo) return;
  float lastY = 1e30f;
  const double kEnd = std::min(kHi, kLo + 4096.0);
  for (double k = kLo; k <= kEnd; k += 1.0) {
    const double c = fund + 1200.0 * std::log2(k);
    // On a tempered anchor the lines fall between rows: placed by pitch (yOfCents), not on the nearest row's centre.
    const float y = H.anchor.valid() ? std::round(s.yOfCents(c)) : static_cast<float>(s.screenY(rows.placeCents(c).centre));
    if (y < yMin - 1.0f || y > yMax + 1.0f) continue;
    if (k != 1.0 && std::fabs(lastY - y) < 3.0f) continue;
    lastY = y;
    f(k, y);
  }
}

}  // namespace

// The fundamental F (the virtual fundamental reference · 1/D, or the active JI group's: its anchor's pitch / (index · m))
// as a translucent line and its harmonics k·F
// as faint guide lines, under the notes. Their labels are drawn over the notes' area in a strip at its right edge
// (paintHarmonicLabels), never under notes (T3.x-fix-look, M3 review M18). Off with the clip box's JI switch.
void paintFundamental(PaintContext& ctx, const RollState& s, const IRect& r, const IRect& vis) {
  const std::optional<RollState::Harmonics> F = s.harmonics();
  if (!F || !s.showHarmonics() || s.rows().empty()) return;
  (void)r;
  const Theme& th = *ctx.res.theme;
  float x0 = static_cast<float>(vis.x), x1 = static_cast<float>(vis.right());
  if (F->from != F->to) {  // T7.16a (H1 / H3): a group's guides cover its time span, where a drawn note joins it
    x0 = std::max(x0, std::round(s.xOfTick(static_cast<double>(F->from))));
    x1 = std::min(x1, std::round(s.xOfTick(static_cast<double>(F->to))));
    if (x1 <= x0) return;
  }
  // A group's guides follow its anchor's pitch (its drawn curve, bends, warps): a note drawn on one starts where it
  // sounds (pitch_resolve: a note takes its anchor's pitch where it starts). Sampled every 3 px; flat runs collapse.
  static std::vector<float> xs, shift;  // UI thread only; reused
  xs.clear();
  shift.clear();
  bool moves = false;
  if (F->anchor.valid()) {
    const float stepPx = 3.0f * ctx.scale.factor;
    for (float x = x0;; x = std::min(x + stepPx, x1)) {
      const double sh = s.harmonicsShift(*F, s.tickAtX(x));
      moves = moves || sh != 0.0;
      const size_t n = shift.size();
      if (n >= 2 && shift[n - 1] == static_cast<float>(sh) && shift[n - 2] == static_cast<float>(sh)) xs[n - 1] = x;  // flat: extend
      else {
        xs.push_back(x);
        shift.push_back(static_cast<float>(sh));
      }
      if (x >= x1) break;
    }
  }
  if (!moves) {
    forEachHarmonic(s, *F, s.contentY(static_cast<float>(vis.y)), s.contentY(static_cast<float>(vis.bottom())), static_cast<float>(vis.y),
                    static_cast<float>(vis.bottom()), [&](double k, float y) {
                      if (k == 1.0) ctx.dl.addRectFilled({x0, y - 1.0f, x1, y + 1.0f}, th.roll.fundamental);
                      else ctx.dl.addRectFilled({x0, y, x1, y + 1.0f}, th.roll.harmonic);
                    });
    return;
  }
  // Every harmonic in view somewhere along the span (from the series at the highest shift to the one at the lowest),
  // skipping a line closer than 3 px to the previous one (measured at the lowest shift) as forEachHarmonic does.
  const RowModel& rows = s.rows();
  const auto [first, last] = rows.rowsIn(s.contentY(static_cast<float>(vis.y)), s.contentY(static_cast<float>(vis.bottom())) + 1);
  if (first >= last) return;
  const auto [lo, hi] = std::minmax_element(shift.begin(), shift.end());
  const double fund = F->baseCents + tuning::cents(F->ratio);
  const double kLo = std::max(1.0, std::ceil(std::exp2((rows.row(last - 1).cents - 100.0 - fund - static_cast<double>(*hi)) / 1200.0)));
  const double kHi = std::min(std::floor(std::exp2((rows.row(first).cents + 100.0 - fund - static_cast<double>(*lo)) / 1200.0)), kLo + 4096.0);
  static std::vector<Vec2> line;  // UI thread only; reused
  float lastY = 1e30f;
  for (double k = kLo; k <= kHi; k += 1.0) {
    const double c = fund + 1200.0 * std::log2(k);
    const float y = s.yOfCents(c + static_cast<double>(*lo));
    if (k != 1.0 && std::fabs(lastY - y) < 3.0f) continue;
    lastY = y;
    line.clear();
    for (size_t i = 0; i < xs.size(); ++i) line.push_back({xs[i], s.yOfCents(c + static_cast<double>(shift[i]))});
    smoothPolyline(ctx.dl, std::span<const Vec2>(line), k == 1.0 ? th.roll.fundamental : th.roll.harmonic, k == 1.0 ? 2.0f : 1.0f);
  }
}

// The labels of those lines: the harmonic numbers (2, 3, … and F itself as "1/4") in a strip along the right edge of
// the note area, over a band of the panel colour so they read on any row and never sit under a note; placed with a
// LabelSpace over the whole widget (partial and full repaints place the same ones). When F's own line is out of view a
// chip pins it to the corner it lies beyond: "F = 1/4 · 65.41 Hz" (bottom-left when F is below, top-left when above).
void paintHarmonicLabels(PaintContext& ctx, const RollState& s, const IRect& r) {
  std::optional<RollState::Harmonics> H = s.harmonics();
  const std::optional<tuning::Rational> F = H ? std::optional<tuning::Rational>(H->ratio) : std::nullopt;
  const bool anchored = H && H->anchor.valid();  // T7.5b: F hangs on a note, not on the reference: it reads "F"
  // Guides that follow a moving anchor are labelled where they reach the strip (or end before it).
  if (anchored && H->from != H->to)
    H->baseCents += s.harmonicsShift(*H, std::clamp(s.tickAtX(static_cast<float>(r.right())), static_cast<double>(H->from), static_cast<double>(H->to)));
  TextSystem* ts = ctx.res.text;
  const ThemeFonts* fonts = ctx.res.uiFonts;
  if (!F || !s.showHarmonics() || s.rows().empty() || !ts || !fonts || r.empty()) return;
  const Theme& th = *ctx.res.theme;
  DrawList& dl = ctx.dl;
  const FontId font = fonts->small;
  const int32_t lineH = ts->metrics(font).lineHeight;
  const int32_t pad = ctx.scale.px(3);
  char buf[48];
  // Strip width: the widest label it may hold ("1/4", a lock "7/20", harmonic numbers up to 4096).
  const int nf = std::snprintf(buf, sizeof buf, "%lld/%lld", static_cast<long long>(F->num()), static_cast<long long>(F->den()));
  const int32_t stripW = std::min(r.w / 3, std::max(ts->measure(font, std::string_view(buf, static_cast<size_t>(std::max(0, nf)))),
                                                    ts->measure(font, "4096")) + 2 * pad);
  const IRect strip{r.right() - stripW, r.y, stripW, r.h};
  if (const IRect sv = intersect(strip, ctx.clip); !sv.empty()) {
    dl.addRectFilled(sv.toRect(), withAlpha(th.surface.panel, 215));
    if (sv.x == strip.x) dl.addRectFilled(IRect{strip.x, sv.y, 1, sv.h}.toRect(), th.roll.harmonic);
  }
  paint::LabelSpace space(ctx.scale.px(2));
  bool fundamentalShown = false;
  forEachHarmonic(s, *H, s.contentY(static_cast<float>(r.y)), s.contentY(static_cast<float>(r.bottom())), static_cast<float>(r.y),
                  static_cast<float>(r.bottom()), [&](double k, float y) {
                    const int32_t top = static_cast<int32_t>(y) - lineH / 2;
                    if (top < r.y || top + lineH > r.bottom()) return;
                    if (!space.place(top, top + lineH)) return;
                    const int n = k != 1.0   ? std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(k))
                                  : anchored ? std::snprintf(buf, sizeof buf, "F")
                                             : std::snprintf(buf, sizeof buf, "%lld/%lld", static_cast<long long>(F->num()),
                                                             static_cast<long long>(F->den()));
                    if (k == 1.0) fundamentalShown = true;
                    paint::text(ctx, font, {strip.x, top, strip.w, lineH}, std::string_view(buf, static_cast<size_t>(std::max(0, n))),
                                k == 1.0 ? th.text.primary : th.roll.fundamental, TextAlign::Center, 0, 0);
                  });
  if (fundamentalShown) return;
  // F out of view: where is it?
  const double fund = H->baseCents + 1200.0 * std::log2(static_cast<double>(F->num())) - 1200.0 * std::log2(static_cast<double>(F->den()));
  const float fy = static_cast<float>(s.screenY(s.rows().placeCents(fund).centre));
  const bool below = fy >= static_cast<float>(r.bottom());
  const double hz = s.tuning().referenceHz() * std::exp2(fund / 1200.0);
  const int n = anchored ? std::snprintf(buf, sizeof buf, "F \xC2\xB7 %.2f Hz %s", hz, below ? "\xE2\x86\x93" : "\xE2\x86\x91")
                         : std::snprintf(buf, sizeof buf, "F = %lld/%lld \xC2\xB7 %.2f Hz %s", static_cast<long long>(F->num()),
                                         static_cast<long long>(F->den()), hz, below ? "\xE2\x86\x93" : "\xE2\x86\x91");
  const std::string_view label(buf, static_cast<size_t>(std::max(0, n)));
  const int32_t w = std::min(r.w - stripW, ts->measure(font, label) + 2 * ctx.scale.px(5));
  const int32_t h = lineH + ctx.scale.px(2);
  const int32_t m = ctx.scale.px(4);
  const IRect chip{r.x + m, below ? r.bottom() - m - h : r.y + m, w, h};
  if (chip.w <= 0 || chip.h > r.h) return;
  paint::box(dl, chip, 0.0f, th.field.fill, th.roll.fundamental);
  paint::text(ctx, font, chip, label, th.text.primary, TextAlign::Left, ctx.scale.px(5), ctx.scale.px(3));
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
  using PaintClock = std::chrono::steady_clock;
  const PaintClock::time_point paintStart = PaintClock::now();
  struct PaintTimer {  // T7.4: RollState::paintStats.notesMs (tests)
    double& ms;
    PaintClock::time_point start;
    ~PaintTimer() { ms = std::chrono::duration<double, std::milli>(PaintClock::now() - start).count(); }
  } paintTimer{s_.paintStats.notesMs, paintStart};
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
  paintGroupFrames(ctx, s_, vis);  // T7.14: every JI group's frame and header, under its notes

  // Notes. A Move / Copy drag shows the selected notes displaced: look as far back / ahead as the drag reaches.
  const DragState& dr = s_.drag;
  const bool shifting = dr.kind == DragKind::Move || dr.kind == DragKind::Copy;
  const Ticks lookFrom = shifting ? std::min(from, from - dr.dt) : from;
  const Ticks lookTo = shifting ? std::max(to, to - dr.dt) : to;
  const NoteColours& col = s_.colours();
  const LineStyle style = lineStyle(*th, s_, ctx.scale.factor);
  const bool anySelected = !s_.selection().empty();
  const bool velPreview = dr.kind == DragKind::Velocity && !s_.showChance();
  const float vx0 = static_cast<float>(vis.x) - 4.0f, vx1 = static_cast<float>(vis.right()) + 4.0f;
  const float cutPad = style.headW + style.thick + 4.0f;
  const float cx0 = static_cast<float>(r.x) - cutPad, cx1 = static_cast<float>(r.right()) + cutPad;
  // Proportional rows may overlap (a collapsing MOS): notes are painted by their row's z, small steps under large
  // ones, selected notes on top (mos-roll.js drawOrder), each z in time order. A counting sort over the visible notes
  // into reused buffers; the other layouts cannot overlap and paint directly.
  const bool zOrder = s_.rows().layout() == RowLayout::Proportional;
  std::vector<NoteLine>& zNotes = s_.zNotes;
  zNotes.clear();
  // T3.19: Ratio notes carry their ratio (and harmonic number over the virtual fundamental), drawn after every note.
  std::vector<NoteLabel>& labels = s_.noteLabels;
  labels.clear();
  // T7.4: notes with a pitch curve are drawn along their path, after the others.
  std::vector<CurveNote>& curved = s_.curveNotes;
  curved.clear();
  const bool anyPath = s_.clip()->curves != nullptr || s_.clip()->groups != nullptr || s_.hasCurvePreview();  // (warps)
  const std::optional<RollState::Harmonics> H = s_.harmonics();
  const std::optional<tuning::Rational> vf = s_.virtualFundamental();
  const bool anyGroup = s_.clip()->groups != nullptr;
  auto emit = [&](const doc::Note& n, Ticks start, Ticks length, const tuning::PitchId& pitch, bool sel) {
    float x0 = std::round(s_.xOfTick(static_cast<double>(start)));
    float x1 = std::max(x0 + 3.0f, std::round(s_.xOfTick(static_cast<double>(start + length))));
    if (x1 <= vx0 || x0 >= vx1) return;
    NoteLine d{x0, x1, 0, 0, 0, 0, 0, 0};
    const bool path = anyPath && s_.onPath(n, pitch);
    if (path) {
      // Its label sits at its start: the pitch there, the lane as its band.
      const std::optional<double> c0 = s_.pathCents(n, start, pitch, 0.0);
      if (!c0) return;
      const float lane = static_cast<float>(std::max(1, s_.rows().laneHeight()));
      d.yc = s_.yOfCents(*c0);
      d.y0 = d.yc - 0.5f * lane;
      d.y1 = d.yc + 0.5f * lane;
    } else {
      const NotePlace pl = s_.place(pitch);
      if (!pl.ok) return;
      d.y0 = static_cast<float>(s_.screenY(pl.top));
      d.y1 = static_cast<float>(s_.screenY(pl.bottom));
      if (d.y1 <= vy0 || d.y0 >= vy1) return;
      d.yc = static_cast<float>(s_.screenY(pl.centre));
      // Cut far outside the widget only (float range), and by the widget, not by this paint's clip: the round ends
      // and the head stay where the note's are, and a partial repaint emits the same geometry as a full one.
      d.x0 = std::max(x0, cx0);
      d.x1 = std::min(x1, cx1);
      d.key = static_cast<uint16_t>(pl.z + (sel ? 256 : 0));
      if (pl.offGrid) d.head = th->roll.offGrid;
    }
    if (!n.enabled()) {  // deactivated: grey, with a dark head (a soft note is grey too, but its head is its own colour)
      d.fill = col.disabledFill;
      d.head = col.disabledBorder;
    } else {
      const int v = velIndex(velPreview && sel ? s_.previewValue(n) : n.velocity);
      d.fill = sel ? col.selFill[v] : col.fill[v];
    }
    if (d.head == 0) d.head = d.fill;
    if (path) curved.push_back({&n, start, length, pitch, sel});
    else if (zOrder) zNotes.push_back(d);
    else drawNoteLine(dl, style, d);
    // T7.14: a note of a JI group carries its harmonic number alone (the anchor's marked), in the group's colours.
    if (anyGroup && pitch == n.pitch)
      if (const int role = s_.groupRole(n); role != 0) {
        if (s_.untunedMember(n)) return;  // T7.16a: Back to Tuning: it reads as the tempered note it plays as
        if (const std::optional<uint64_t> h = s_.harmonicOf(n); h && *h <= 99999) {
          labels.push_back({d.x0, d.x1, d.y0, d.y1, d.yc, d.fill, 0, 1, static_cast<uint32_t>(*h), static_cast<uint8_t>(role)});
          return;
        }
      }
    if (pitch.kind() == tuning::PitchKind::Ratio) {
      const tuning::Rational r = pitch.ratio();
      uint32_t k = 0;
      // Its harmonic number: a member of a JI group in its group (T7.13; a drag's preview pitch against the active
      // group's series), a reference Ratio over the virtual fundamental.
      std::optional<uint64_t> h;
      if (pitch.anchorKind() == tuning::AnchorKind::Reference) {
        if (vf) h = tuning::harmonicNumber(r, *vf);
      } else if (H && H->anchor.valid() && pitch.anchorId() == H->anchor.value) {
        h = tuning::harmonicNumber(r, H->ratio);  // a member being dragged: its number on the active group's series
        if (h && *h <= 99999) {
          labels.push_back({d.x0, d.x1, d.y0, d.y1, d.yc, d.fill, 0, 1, static_cast<uint32_t>(*h), 1});
          return;
        }
      }
      if (h && *h <= 99999) k = static_cast<uint32_t>(*h);
      labels.push_back({d.x0, d.x1, d.y0, d.y1, d.yc, d.fill, r.num(), r.den(), k, 0});
    }
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
    for (const NoteLine& z : zNotes) ++count[z.key + 1];
    for (size_t k = 1; k < 513; ++k) count[k] += count[k - 1];
    std::vector<NoteLine>& sorted = s_.zSorted;
    sorted.resize(zNotes.size());
    for (const NoteLine& z : zNotes) sorted[count[z.key]++] = z;  // stable: time order within one z
    for (const NoteLine& z : sorted) drawNoteLine(dl, style, z);
  }
  // T4.16b: the part of the note a press would grab (Live): the note's band brightened, the edge it would trim
  // marked over the band's height.
  if (const RollState::HoverPart& hp = s_.hoverPart(); hp.note != 0 && dr.kind == DragKind::None)
    if (const doc::Note* n = s_.findNote(doc::NoteId{hp.note}); n && !s_.onPath(*n, n->pitch))
      if (const NotePlace pl = s_.place(n->pitch); pl.ok) {
        const float x0 = std::round(s_.xOfTick(static_cast<double>(n->start)));
        const float x1 = std::max(x0 + 3.0f, std::round(s_.xOfTick(static_cast<double>(n->start + n->length))));
        const float y0 = static_cast<float>(s_.screenY(pl.top)), y1 = static_cast<float>(s_.screenY(pl.bottom));
        if (x1 > vx0 && x0 < vx1 && y1 > vy0 && y0 < vy1) {
          dl.addRectFilled(frect(x0, y0, x1, y1), withAlpha(th->text.primary, 40));
          if (hp.zone != 0) {
            const float w = std::min(std::max(2.0f, std::round(2.0f * ctx.scale.factor)), (x1 - x0) * 0.5f);
            const float ex = hp.zone > 0 ? x1 - w : x0;
            dl.addRectFilled(frect(ex, y0, ex + w, y1), th->text.primary);
          }
        }
      }

  if (!curved.empty()) paintCurveNotes(ctx, s_, vis);  // the same lines, along their paths
  if (!labels.empty()) paintNoteLabels(ctx);

  paintGroupOverlays(ctx, s_, vis);  // T7.14: warp markers, grips, labels; the proportion tool's box
  paintCurveHandles(ctx, s_, vis);   // T7.6: the breakpoints of the curves being edited
  paintVibratoGrips(ctx, s_, vis);   // T7.14: the ~ grips and the envelope corners

  // Draw-mode preview: the notes the drag will add.
  if (dr.kind == DragKind::Draw)
    for (const DrawnNote& dn : dr.drawn) {
      const tuning::PitchId pitch = s_.drawnPitch(dn);
      const NotePlace pl = s_.place(pitch);
      if (!pl.ok) continue;
      const float x0 = std::round(s_.xOfTick(static_cast<double>(dn.start)));
      const float x1 = std::max(x0 + 3.0f, std::round(s_.xOfTick(static_cast<double>(dn.start + dn.length))));
      // A note joining a group whose anchor moves is shown where it will sound (on the guide it was drawn on).
      float dy = 0.0f;
      if (dn.anchor.valid())
        if (const std::optional<double> c = s_.centsOf(pitch)) {
          const double sh = s_.harmonicsShift(RollState::Harmonics{dn.anchor}, static_cast<double>(dn.start));
          if (sh != 0.0) dy = s_.yOfCents(*c + sh) - s_.yOfCents(*c);
        }
      drawNoteLine(dl, style,
                   {x0, x1, static_cast<float>(s_.screenY(pl.top)) + dy, static_cast<float>(s_.screenY(pl.bottom)) + dy,
                    static_cast<float>(s_.screenY(pl.centre)) + dy, col.selFill[100], pl.offGrid ? th->roll.offGrid : col.selFill[100], 0});
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

// Ratio labels (T3.19, SPEC §4.3 "notes with Ratio identity show their ratio"; M3 review UI §2): "7/4" once the note
// is ≥ 24 logical px wide and its row as tall as the small font, with " · 7" (its harmonic number over the clip's
// virtual fundamental) when that fits too. T7.12: at the note's left end, after its head and above its line, in the
// note's colour lifted towards the text colour (it sits on the roll, not on the note).
void NoteAreaWidget::paintNoteLabels(PaintContext& ctx) {
  const Theme& th = *ctx.res.theme;
  TextSystem* ts = ctx.res.text;
  const ThemeFonts* fonts = ctx.res.uiFonts;
  if (!ts || !fonts) return;
  const FontId font = fonts->small;
  const LineStyle style = lineStyle(th, s_, ctx.scale.factor);
  const float minW = ctx.scale.exact(24.0f);
  const int32_t px = static_cast<int32_t>(ts->metrics(font).pixelSize);
  const float minH = static_cast<float>(px) + 1.0f;
  const int32_t pad = ctx.scale.px(2);
  char buf[48];
  for (const NoteLabel& l : s_.noteLabels) {
    const float w = l.x1 - l.x0, h = l.y1 - l.y0;
    if (l.group != 0) {
      // T7.14: a JI group's note: its harmonic number above the line at its left end, as in the frame's proportion;
      // the anchor's brighter, after a small diamond (the frozen note).
      if (w < ctx.scale.exact(12.0f) || h < 0.6f * minH) continue;
      const int n = std::snprintf(buf, sizeof buf, "%u", l.harmonic);
      if (n <= 0) continue;
      const float t = std::min(style.thick, std::max(1.0f, h - 2.0f));
      const int32_t bottom = static_cast<int32_t>(std::round(l.yc - 0.5f * t)) - 1;
      const int32_t top = bottom - px - 1;
      int32_t left = static_cast<int32_t>(l.x0) + pad;
      const Rgba ink = l.group == 2 ? th.roll.jiSelected : th.roll.jiText;
      if (l.group == 2) {
        const float r = std::max(2.5f, 0.28f * static_cast<float>(px));
        const Vec2 c{static_cast<float>(left) + r, static_cast<float>(top + bottom) * 0.5f + 1.0f};
        const Vec2 diamond[4] = {{c.x - r, c.y}, {c.x, c.y - r}, {c.x + r, c.y}, {c.x, c.y + r}};
        ctx.dl.addConvexPolyFilled(diamond, ink);
        left += static_cast<int32_t>(std::ceil(2.0f * r)) + pad;
      }
      const std::string_view text(buf, static_cast<size_t>(n));
      paint::text(ctx, l.group == 2 ? fonts->bodyStrong : font, {left, top, ts->measure(fonts->bodyStrong, text) + 2 * pad, bottom - top + 1}, text, ink, TextAlign::Left);
      continue;
    }
    if (w < minW || h < minH) continue;
    int n = l.num == 0 ? std::snprintf(buf, sizeof buf, "%u", l.harmonic)  // the anchor: its harmonic number
                       : std::snprintf(buf, sizeof buf, "%lld/%lld", static_cast<long long>(l.num), static_cast<long long>(l.den));
    if (n <= 0) continue;
    const int32_t left = static_cast<int32_t>(l.x0 + std::min(style.headW, w)) + pad;
    const int32_t avail = static_cast<int32_t>(l.x1) - left - pad;
    std::string_view text(buf, static_cast<size_t>(n));
    if (ts->measure(font, text) > avail) continue;
    if (l.harmonic && l.num != 0) {
      const int m = std::snprintf(buf + n, sizeof buf - static_cast<size_t>(n), " \xC2\xB7 %u", l.harmonic);
      if (m > 0 && ts->measure(font, std::string_view(buf, static_cast<size_t>(n + m))) <= avail) text = std::string_view(buf, static_cast<size_t>(n + m));
    }
    const float t = std::min(style.thick, std::max(1.0f, h - 2.0f));
    const int32_t bottom = static_cast<int32_t>(std::round(l.yc - 0.5f * t)) - 1;
    // Not higher above the band than the note's damage rect reaches (RollState::paintRectOf).
    const int32_t top = std::max(bottom - px, static_cast<int32_t>(l.y0) - static_cast<int32_t>(kNoteLabelRise * ctx.scale.factor));
    const IRect box{left, top, avail, bottom - top};
    paint::text(ctx, font, box, text, lerp(l.fill, th.text.primary, 0.45f), TextAlign::Left);
  }
}

// The notes drawn along a pitch path (RollState::curveNotes, filled by the note area's paint): the note's cached
// effective curve (RollState::pathOf: flattened once per clip snapshot, M7 review M14) mapped to the screen into the
// shared scratch polyline, then the same line as every note. Nothing is evaluated per frame but the view's two ends:
// a paint walks the cached points in view. bug-curve-render: the points are simplified in device px as they are mapped
// (views/common/pitch_path.h PxPolyline: within kPathPxTolerance of the curve), so a zoomed-out note is its silhouette
// in a few vertices — the per-column extremes this replaced were vertical reversals the stroke's miters drew as spikes
// and combs; the cache is flattened to a fraction of a px at the current rows (fitPathTolerance), so a zoomed-in curve
// has no visible facets; and a vibrato whose cycle is shorter than kVibratoBandPx (or than two strokes and their gaps)
// is drawn as its band — the envelope around the rest of the curve, at kVibratoBandAlpha — with the rest of the curve
// as the note's line, instead of a zig-zag aliasing with the zoom.
void paintCurveNotes(PaintContext& ctx, RollState& s, const IRect& vis) {
  if (!ctx.res.theme || s.curveNotes.empty()) return;
  const Theme& th = *ctx.res.theme;
  const NoteColours& col = s.colours();
  const LineStyle style = lineStyle(th, s, ctx.scale.factor);
  s.fitPathTolerance(kPathPxTolerance);
  const float step = std::max(1.0f, kPathSampleStep * ctx.scale.factor);
  const float lane = static_cast<float>(std::max(1, s.rows().laneHeight()));
  const float thick = std::min(style.thick, std::clamp(lane - 2.0f, std::min(2.0f, lane), lane));  // as drawNoteLine
  const float vx0 = static_cast<float>(vis.x) - 2.0f * step, vx1 = static_cast<float>(vis.right()) + 2.0f * step;
  const double ppt = s.pxPerTick();
  const double chunk = pathChunkTicks(ppt, ctx.scale.factor);
  const double bandPeriodPx = std::max(static_cast<double>(kVibratoBandPx * ctx.scale.factor), 2.0 * (static_cast<double>(thick) + 2.0 * ctx.scale.factor));
  std::vector<Vec2>& pts = s.linePts;
  static thread_local std::vector<Vec2> up, dn;
  static thread_local std::vector<double> ticks, knots;
  for (const CurveNote& c : s.curveNotes) {
    const float x0 = s.xOfTick(static_cast<double>(c.start));
    const float x1 = std::max(x0 + 3.0f, s.xOfTick(static_cast<double>(c.start + c.length)));
    const float xa = std::max(x0, vx0), xb = std::min(x1, vx1);
    if (xb <= xa) continue;
    const RollState::NotePath* p = s.pathOf(*c.note);
    const std::optional<double> base = p && c.pitch == c.note->pitch ? std::optional<double>(p->base + s.anchorPreviewShift(*c.note)) : s.centsOf(c.pitch);
    if (!base) continue;
    const bool on = c.note->enabled();
    const Rgba fill = on ? (c.selected ? col.selFill : col.fill)[velIndex(c.note->velocity)] : col.disabledFill;
    // Level of detail (a zoomed-out clip stays cheap, as the straight notes' two quads): a note too narrow to show
    // its wave is one quad over the wave's extent; a small one is its line without the band and the round ends.
    if (x1 - x0 < kPathTinyPx * ctx.scale.factor) {
      const float ya = s.yOfCents(*base + (p ? static_cast<double>(p->hi) : 0.0)), yb = s.yOfCents(*base + (p ? static_cast<double>(p->lo) : 0.0));
      ctx.dl.addRectFilled(frect(std::round(x0), std::round(ya - 0.5f * thick), std::round(x1), std::round(yb + 0.5f * thick)), fill);
      continue;
    }
    // A vibrato too fast for this zoom: its band, and the rest of the curve as the line.
    const bool band = p && p->vibPeriod > 0.0 && p->vibPeriod * ppt < bandPeriodPx;
    const std::vector<doc::Breakpoint>* src = p ? (band ? &p->rest : &p->pts) : nullptr;
    const std::span<const doc::Breakpoint> curve = src ? std::span<const doc::Breakpoint>(*src) : std::span<const doc::Breakpoint>{};
    // The whole chunks of the note in view (pitch_path.h chunkSpan: a partial repaint draws the vertices a full one does).
    const double len = static_cast<double>(c.length);
    double ta = 0.0, tb = len;
    chunkSpan(s.tickAtX(xa) - static_cast<double>(c.start), s.tickAtX(xb) - static_cast<double>(c.start), len, chunk, ta, tb);
    auto xAt = [&](double t) { return s.xOfTick(static_cast<double>(c.start) + t); };
    auto yAt = [&](double v) { return s.yOfCents(*base + v); };
    {
      PxPolyline line(pts);
      addPathSpan(line, curve, band ? p->restCurved : p != nullptr && p->curved, ta, tb, static_cast<double>(step) / std::max(ppt, 1e-12), chunk, xAt, yAt);
      line.finish();
    }
    if (pts.size() < 2) pts.push_back({xAt(tb), pts.empty() ? s.yOfCents(*base) : pts.back().y});
    if (band) {
      // The envelope around the rest of the curve.
      bandSpan(up, dn, ticks, knots, curve, p->restCurved, p->vib, ta, tb, 1.5 * ctx.scale.factor / std::max(ppt, 1e-12), chunk, 0.5f * thick, xAt, yAt);
      smoothBand(ctx.dl, up.data(), dn.data(), static_cast<uint32_t>(up.size()), mulAlpha(fill, kVibratoBandAlpha));
    }
    if (x1 - x0 < kPathSmallPx * ctx.scale.factor) {
      strokePath(ctx.dl, pts, fill, thick);
      continue;
    }
    const float y = pts.front().y;
    drawNoteLine(ctx.dl, style, {x0, x1, y - 0.5f * lane, y + 0.5f * lane, y, fill, on ? fill : col.disabledBorder, 0}, pts);
  }
}

void NoteAreaWidget::paintOver(PaintContext& ctx) {
  if (!ctx.res.theme || !s_.hasClip()) return;
  const IRect r = rect();
  if (intersect(r, ctx.clip).empty()) return;
  ctx.dl.pushClip(r.toRect());
  paintHarmonicLabels(ctx, s_, r);
  paintPlayhead(ctx, s_, r);  // bug-roll-playhead: over everything
  ctx.dl.popClip();
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
  const bool twelve = rows.namedLabels();  // 12edo names: only the C rows until the rows are tall (Live)
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
    // A sliver row (an s step near collapse) gets no number: it would sit on its neighbours' keys and the equave tab
    // (T3.x-fix-look, M3 review L18).
    if (!equave && row.height() < std::max(3, lineH / 3)) continue;
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

  // bug-paused-playhead: the scrub strip (Live's scrub area) between the brace and the numbers.
  const float scrubBottom = braceBottom + static_cast<float>(ctx.scale.px(kScrubHeight));
  dl.addRectFilled(frect(static_cast<float>(vis.x), braceBottom, static_cast<float>(vis.right()), scrubBottom), th->surface.raised);
  dl.addRectFilled(frect(static_cast<float>(vis.x), scrubBottom - 1.0f, static_cast<float>(vis.right()), scrubBottom), th->surface.divider);

  // Time ruler: ticks from the editing grid, numbers on a coarser adaptive grid so labels never collide.
  const auto [from, to] = tickSpan(s_, vis.x, vis.right());
  const Grid g = s_.grid();
  const float scale = ctx.scale.factor;
  if (g.enabled())
    for (const GridLine& line : g.lines(from, to)) {
      const float x = std::round(s_.xOfTick(static_cast<double>(line.tick)));
      const float h = line.strength == GridStrength::Bar    ? bottom - scrubBottom
                      : line.strength == GridStrength::Beat ? std::round(6.0f * scale)
                                                            : std::round(3.0f * scale);
      dl.addRectFilled(frect(x, bottom - h, x + 1.0f, bottom), th->text.disabled);
    }
  const ThemeFonts* fonts = ctx.res.uiFonts;
  if (fonts && ctx.res.text) {
    const Grid labels = Grid::adaptive(56.0, s_.sigs(), s_.pxPerTick() / static_cast<double>(scale));
    char buf[kRulerLabelMaxChars];
    const int32_t labelY = static_cast<int32_t>(scrubBottom);
    const int32_t labelH = r.bottom() - labelY;
    for (const GridLine& line : labels.lines(from - kTicksPerWhole, to)) {
      const float x = std::round(s_.xOfTick(static_cast<double>(line.tick)));
      dl.addRectFilled(frect(x, scrubBottom, x + 1.0f, bottom), th->text.disabled);
      const size_t n = formatRulerPosition(rulerPosition(line), buf);
      paint::text(ctx, fonts->small, {static_cast<int32_t>(x) + ctx.scale.px(3), labelY, ctx.scale.px(60), labelH},
                  std::string_view(buf, n), th->text.secondary, TextAlign::Left);
    }
  }
  dl.addRectFilled(frect(static_cast<float>(vis.x), bottom - 1.0f, static_cast<float>(vis.right()), bottom), th->surface.divider);
  paintPlayhead(ctx, s_, r);  // bug-roll-playhead
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
    if (n.id.value == hoverStem_ && dr.kind != DragKind::Velocity) {  // T4.17: the stem a press would grab
      const float h0 = std::floor(head * 0.5f) + std::max(1.0f, std::round(scale));
      const float w = std::max(1.0f, std::round(scale));
      const Rgba hc = th->accent.primary;
      dl.addRectFilled(frect(x - h0, y - h0, x + h0 + 1.0f, y - h0 + w), hc);
      dl.addRectFilled(frect(x - h0, y + h0 + 1.0f - w, x + h0 + 1.0f, y + h0 + 1.0f), hc);
      dl.addRectFilled(frect(x - h0, y - h0, x - h0 + w, y + h0 + 1.0f), hc);
      dl.addRectFilled(frect(x + h0 + 1.0f - w, y - h0, x + h0 + 1.0f, y + h0 + 1.0f), hc);
      if (baseY > y + h0 + 1.0f) dl.addRectFilled(frect(x, y + h0 + 1.0f, x + 1.0f + w, baseY), hc);
    }
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
  paintPlayhead(ctx, s_, r);  // bug-roll-playhead
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
              clipTextFor(*th, clipCol), TextAlign::Left, pad, ctx.scale.px(kHelpButton + 4.0f));  // KB.help: the ? button
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
