// JI objects and pitch paths in the piano roll (T7.4; docs/plans/M7.md "T7.4", amendment A1; the look follows
// reference/inspiration "zhea daw*.png").
//
// A JI object is drawn as
//   - a translucent box with a dashed border over its time span and pitch range,
//   - one line per voice through its stops and morphs (the lowest voice thicker, a diamond at its end),
//   - the harmonic number of each voice at the line's left end,
//   - a header above the box ("8:9:10:12:15 · morphs through 5 proportions · rides a curve"),
//   - a dotted marker at every later stop with the stop's proportion under the box (a tuning stop: the tuning's name),
//   - the ride curve as a line under the lowest voice.
// Every note with a pitch curve, and every note that follows an object, is drawn along its pitch path instead of on
// a row (paintCurveNotes).
//
// CPU: nothing is evaluated per frame that can be evaluated per document change. RollState::rebuildJiViews (refresh(),
// off the paint path) flattens every object once with doc::jiVoicePlan — the evaluation the playback lowering uses — to
// breakpoints linear in cents, and formats every label. Paint maps breakpoints to the screen: holds cost two points,
// only the segments inside the visible tick range are touched, a moving segment is subdivided every 2 px (the rows'
// cents → y map is piecewise linear, so a long glide is not a straight line in the scale layouts), a voice that
// repeats the one before it is not drawn twice, and objects outside the clip rect are skipped by their box. Measured:
// tests/views/test_piano_roll_ji.cpp "perf".
//
// Editing (all through doc::ops, one dispatch = one undo step): the JI tool (J) drags a box, then a proportion field
// creates the object; the header and the border select (drag: move in time and by rows; the left / right border:
// resize); Delete and Ctrl+D act on the selected object.
//
// T7.5 (the inner roll and morph editing: roll_ji_edit.cpp) draws on this file: jiLayout places every text and handle
// for both paint and jiAt; the active object (entered, else selected) shows its glide handles, a selected stop / voice
// label as a chip, and — entered — its voice lanes and the bezier handles of its transitions; a stop between the
// tuning and a proportion shows how far each voice moves ("−14¢").
//
// Seams for T7.6 (not implemented here): RollState::jiAt is the one hit test, jiDragEdit shows how a gesture previews
// through the edit it will commit, pathCents is the pitch of any note at any tick (hit-testing and drawing along
// curves: noteAt uses it; the pen records against it), pathBox / pathPoint give a path note's screen extent.

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <string>
#include <variant>

#include "../common/paint.h"
#include "pq/document/automation_curve.h"
#include "pq/document/commands.h"
#include "pq/document/ji_object.h"
#include "pq/ui/lines.h"
#include "pq/ui/overlay.h"
#include "pq/ui/theme.h"
#include "roll_widgets.h"

namespace pq::views::roll {

using namespace pq::ui;

namespace {

constexpr float kJiPad = 7.0f;         // logical px between the outermost voices and the box's border
constexpr float kJiLabelRoom = 13.0f;  // extra room above the top voice for its harmonic label
constexpr float kJiRideGap = 9.0f;     // the ride line sits this far under the pitch it is anchored to
constexpr float kJiHeaderMin = 220.0f; // a header may run this wide past a narrow box before it is cut with "…"
constexpr float kJiTextMin = 44.0f;    // a box narrower than this shows no header and no stop labels
constexpr float kJiNarrow = 24.0f;     // a box narrower than this: plain border, no stop markers
constexpr float kJiGrab = 4.0f;        // border hit zone (each side)
constexpr float kJiSampleStep = 2.0f;  // px between samples of a moving segment

float scaleOf(const RollState& s) noexcept {
  return (s.noteArea && s.noteArea->tree()) ? s.noteArea->tree()->scale() : 1.0f;
}

std::string stopLabel(const doc::JiStop& stop, bool undertone, const std::string& tuningName) {
  if (stop.isTuning()) return tuningName.empty() ? std::string("tuning") : tuningName;
  std::string out;
  for (size_t i = 0; i < stop.terms.size(); ++i) {
    if (i) out += ':';
    if (undertone) out += "1/";
    out += std::to_string(stop.terms[i]);
  }
  return out;
}

bool jiDragKind(DragKind k) noexcept {
  return k == DragKind::JiMove || k == DragKind::JiResizeStart || k == DragKind::JiResizeEnd || k == DragKind::JiStop ||
         k == DragKind::JiGlide || k == DragKind::JiBezier;
}

// "−14¢" / "+2¢" / "+0.4¢": how far a voice moves into a stop.
std::string deltaText(double d) {
  char buf[32];
  const double a = std::fabs(d);
  if (a >= 9.95) std::snprintf(buf, sizeof buf, "%s%.0f\xC2\xA2", d < 0 ? "\xE2\x88\x92" : "+", a);
  else std::snprintf(buf, sizeof buf, "%s%.1f\xC2\xA2", d < 0 ? "\xE2\x88\x92" : "+", a);
  return buf;
}

struct JiFont {
  TextSystem* ts = nullptr;
  FontId font = kNoFont;
  float fh = 12.0f;
  float measure(const std::string& t, float sc) const {
    return (ts && font != kNoFont) ? static_cast<float>(ts->measure(font, t)) : 6.5f * sc * static_cast<float>(t.size());
  }
};
JiFont jiFont(const PaintResources* res, float sc) {
  JiFont f;
  f.fh = 12.0f * sc;
  if (!res) return f;
  f.ts = res->text;
  if (const ThemeFonts* fonts = res->uiFonts) f.font = fonts->numeric != kNoFont ? fonts->numeric : fonts->small;
  if (f.ts && f.font != kNoFont) f.fh = static_cast<float>(f.ts->metrics(f.font).pixelSize);
  return f;
}

// A filled chip behind a selected label.
void chip(DrawList& dl, const Rect& box, Rgba col, float sc) { dl.addRoundedRectFilled(box, 2.0f * sc, col); }

Error invalid(std::string message) { return Error{ErrorCode::InvalidArgument, std::move(message), {}}; }

// Horizontal / vertical 1-px lines, dashed when gap > 0, drawn only inside [lo, hi] (the visible span): a box that is
// mostly off screen costs the dashes on screen.
void hLine(DrawList& dl, float x0, float x1, float y, Rgba col, float dash, float gap, float lo, float hi) {
  const float a = std::max(x0, lo), b = std::min(x1, hi);
  if (b <= a) return;
  if (gap <= 0.0f) {
    dl.addRectFilled({a, y, b, y + 1.0f}, col);
    return;
  }
  const float period = dash + gap;
  float x = x0 + std::floor((a - x0) / period) * period;
  for (; x < b; x += period) {
    const float d0 = std::max(x, a), d1 = std::min(x + dash, b);
    if (d1 > d0) dl.addRectFilled({d0, y, d1, y + 1.0f}, col);
  }
}

void vLine(DrawList& dl, float x, float y0, float y1, Rgba col, float dash, float gap, float lo, float hi) {
  const float a = std::max(y0, lo), b = std::min(y1, hi);
  if (b <= a) return;
  if (gap <= 0.0f) {
    dl.addRectFilled({x, a, x + 1.0f, b}, col);
    return;
  }
  const float period = dash + gap;
  float y = y0 + std::floor((a - y0) / period) * period;
  for (; y < b; y += period) {
    const float d0 = std::max(y, a), d1 = std::min(y + dash, b);
    if (d1 > d0) dl.addRectFilled({x, d0, x + 1.0f, d1}, col);
  }
}

void boxBorder(DrawList& dl, const Rect& b, Rgba col, float dash, float gap, const IRect& vis) {
  const float vx0 = static_cast<float>(vis.x), vx1 = static_cast<float>(vis.right());
  const float vy0 = static_cast<float>(vis.y), vy1 = static_cast<float>(vis.bottom());
  if (b.y0 >= vy0 - 1.0f && b.y0 < vy1) hLine(dl, b.x0, b.x1, b.y0, col, dash, gap, vx0, vx1);
  if (b.y1 - 1.0f >= vy0 - 1.0f && b.y1 - 1.0f < vy1) hLine(dl, b.x0, b.x1, b.y1 - 1.0f, col, dash, gap, vx0, vx1);
  if (b.x0 >= vx0 - 1.0f && b.x0 < vx1) vLine(dl, b.x0, b.y0, b.y1, col, dash, gap, vy0, vy1);
  if (b.x1 - 1.0f >= vx0 - 1.0f && b.x1 - 1.0f < vx1) vLine(dl, b.x1 - 1.0f, b.y0, b.y1, col, dash, gap, vy0, vy1);
}

// One voice's path over the visible part of its object, into s.linePts (device px).
void voicePath(RollState& s, const JiView& v, const std::vector<doc::JiPitchPoint>& pts, float visX0, float visX1, float step) {
  std::vector<Vec2>& out = s.linePts;
  out.clear();
  if (pts.size() < 2) return;
  const double t0 = s.tickAtX(visX0) - static_cast<double>(v.start), t1 = s.tickAtX(visX1) - static_cast<double>(v.start);
  // The first segment that ends at or after the visible range's start.
  size_t i = 0;
  {
    size_t lo = 0, hi = pts.size() - 1;
    while (lo < hi) {
      const size_t mid = (lo + hi) / 2;
      if (static_cast<double>(pts[mid + 1].tick) < t0) lo = mid + 1;
      else hi = mid;
    }
    i = lo;
  }
  auto push = [&](float x, float y) {
    if (!out.empty() && out.back().x == x && out.back().y == y) return;
    out.push_back({x, y});
  };
  // The plan subdivides a curved glide to a quarter cent: zoomed out that is many breakpoints per pixel. Pieces that
  // advance less than `step` px are skipped; the last skipped end is kept (`pend`) so the path still arrives exactly.
  bool pend = false;
  Vec2 pending{};
  auto flush = [&] {
    if (pend) push(pending.x, pending.y);
    pend = false;
  };
  const double base = static_cast<double>(v.start);
  for (; i + 1 < pts.size(); ++i) {
    const doc::JiPitchPoint& a = pts[i];
    const doc::JiPitchPoint& b = pts[i + 1];
    if (static_cast<double>(a.tick) > t1) break;
    const float xa = s.xOfTick(base + static_cast<double>(a.tick)), xb = s.xOfTick(base + static_cast<double>(b.tick));
    if (a.tick == b.tick) {  // a jump
      flush();
      push(xa, s.yOfCents(a.cents));
      push(xb, s.yOfCents(b.cents));
      continue;
    }
    const float ca = std::max(xa, visX0), cb = std::min(xb, visX1);
    if (cb < ca) continue;
    const double span = static_cast<double>(xb - xa);
    auto centsAt = [&](float x) {
      const double u = span > 0.0 ? static_cast<double>(x - xa) / span : 0.0;
      return a.cents + (b.cents - a.cents) * u;
    };
    if (a.cents == b.cents) {  // a hold
      const float y = s.yOfCents(a.cents);
      flush();
      push(ca, y);
      push(cb, y);
      continue;
    }
    if (out.empty()) push(ca, s.yOfCents(centsAt(ca)));
    if (cb - out.back().x < step) {
      pend = true;
      pending = {cb, s.yOfCents(centsAt(cb))};
      continue;
    }
    pend = false;
    const int n = std::clamp(static_cast<int>(std::ceil((cb - ca) / step)), 1, 4096);
    for (int k = 1; k <= n; ++k) {
      const float x = ca + (cb - ca) * static_cast<float>(k) / static_cast<float>(n);
      push(x, s.yOfCents(centsAt(x)));
    }
  }
  flush();
}

// Returns whether the object met the clip rect (was drawn). Two passes: the box, markers, ride and voices go under
// the notes; the text (`labels`) over them, so an inner note lying on its voice does not hide the voice's number.
bool paintJiObject(PaintContext& ctx, RollState& s, const JiView& v, bool selected, const IRect& vis, bool labels) {
  if (!v.ok) return false;
  const bool entered = v.id == s.jiEntered();
  JiLayout& lay = s.jiLayoutScratch();
  const Theme& th = *ctx.res.theme;
  TextSystem* ts = ctx.res.text;
  const ThemeFonts* fonts = ctx.res.uiFonts;
  DrawList& dl = ctx.dl;
  const float sc = ctx.scale.factor;
  const Rect b = s.jiBoxRect(v);
  const FontId font = fonts ? (fonts->numeric != kNoFont ? fonts->numeric : fonts->small) : kNoFont;
  const float fh = (ts && font != kNoFont) ? static_cast<float>(ts->metrics(font).pixelSize) : 12.0f * sc;
  const float hdrH = fh + 5.0f * sc, lblH = fh + 4.0f * sc;
  const float hdrW = std::max(b.x1 - b.x0, kJiHeaderMin * sc);
  const float vx0 = static_cast<float>(vis.x), vx1 = static_cast<float>(vis.right());
  const float vy0 = static_cast<float>(vis.y), vy1 = static_cast<float>(vis.bottom());
  if (b.x0 + hdrW < vx0 || b.x0 > vx1 || b.y1 + lblH < vy0 || b.y0 - hdrH > vy1) return false;

  const float step = std::max(1.0f, kJiSampleStep * sc);
  if (!labels) {
  // The box.
  const Rect fill{std::max(b.x0, vx0), std::max(b.y0, vy0), std::min(b.x1, vx1), std::min(b.y1, vy1)};
  if (fill.x1 > fill.x0 && fill.y1 > fill.y0) dl.addRectFilled(fill, th.roll.jiFill);
  // Dashed; a box only a few px wide gets a plain faint border (its dashes would be noise, and a zoomed-out clip full
  // of objects would spend most of its paint on them).
  const bool narrow = b.x1 - b.x0 < kJiNarrow * sc;
  if (selected) boxBorder(dl, b, th.roll.jiSelected, 1.0f, 0.0f, vis);
  else if (narrow) boxBorder(dl, b, th.roll.jiStop, 1.0f, 0.0f, vis);
  else boxBorder(dl, b, th.roll.jiBorder, std::round(4.0f * sc), std::round(3.0f * sc), vis);

  // Stop markers (dotted), stop 0 has none: it is the box's left edge.
  for (size_t i = 1; i < v.stops.size() && !narrow; ++i) {
    const float x = std::round(s.xOfTick(static_cast<double>(v.start + v.stops[i].at)));
    if (x < vx0 - 1.0f || x >= vx1 || x <= b.x0 || x >= b.x1 - 1.0f) continue;
    vLine(dl, x, b.y0 + 1.0f, b.y1 - 1.0f, th.roll.jiStop, std::round(2.0f * sc), std::round(3.0f * sc), vy0, vy1);
  }

  // The ride curve: under the lowest voice, in its own colour, a dot at its start.
  if (!v.ride.empty() && b.x1 > vx0 && b.x0 < vx1) {
    std::vector<Vec2>& pts = s.linePts;
    pts.clear();
    const float gap = kJiRideGap * sc;
    const float xa = std::max(b.x0, vx0 - step), xb = std::min(b.x1, vx1 + step);
    const int n = std::clamp(static_cast<int>(std::ceil((xb - xa) / step)), 1, 8192);
    for (int k = 0; k <= n; ++k) {
      const float x = xa + (xb - xa) * static_cast<float>(k) / static_cast<float>(n);
      const Ticks t = static_cast<Ticks>(std::llround(s.tickAtX(x))) - v.start;
      pts.push_back({x, s.yOfCents(v.rideBase + doc::automationValueAt(v.ride, std::clamp<Ticks>(t, 0, v.length))) + gap});
    }
    smoothPolyline(dl, pts, th.roll.jiRide, std::max(1.5f, 1.75f * sc));
    if (b.x0 >= vx0 - 4.0f * sc)
      dl.addCircleFilled({b.x0, s.yOfCents(v.rideBase + doc::automationValueAt(v.ride, 0)) + gap}, 2.75f * sc, th.roll.jiRide);
  }

  // The voices: the lowest (voice 0, the pinned one) thicker, a diamond at its end. Entered (T7.5): each voice is a
  // row, a translucent lane along its path.
  const float thin = std::max(1.25f, 1.4f * sc), thick = std::max(2.0f, 2.6f * sc);
  const float laneW = 2.0f * s.pathHalfThickness() + 4.0f * sc;
  for (size_t i = v.plan.voices.size(); i-- > 0;) {
    if (!v.distinct[i]) continue;
    voicePath(s, v, v.plan.voices[i].pitch, std::max(b.x0, vx0 - step), std::min(b.x1, vx1 + step), step);
    if (s.linePts.size() < 2) continue;
    if (entered) smoothPolyline(dl, s.linePts, withAlpha(th.roll.jiLine, 44), laneW);
    smoothPolyline(dl, s.linePts, th.roll.jiLine, i == 0 ? thick : thin);
  }
  if (!v.plan.voices.empty() && b.x1 >= vx0 - 6.0f * sc && b.x1 <= vx1 + 6.0f * sc) {
    const float y = s.yOfCents(v.plan.voices[0].pitch.back().cents);
    const float x = b.x1 - 0.5f, r = 4.5f * sc, q = r - std::max(1.0f, 1.25f * sc);
    const Vec2 outer[4] = {{x - r, y}, {x, y - r}, {x + r, y}, {x, y + r}};
    const Vec2 inner[4] = {{x - q, y}, {x, y - q}, {x + q, y}, {x, y + q}};
    dl.addConvexPolyFilled(outer, th.roll.jiLine);
    dl.addConvexPolyFilled(inner, th.roll.rowNatural);
  }

  if (selected) {  // the edges a drag resizes
    const float my = 0.5f * (b.y0 + b.y1), hh = std::min(9.0f * sc, 0.5f * (b.y1 - b.y0) - 2.0f), hw = std::round(2.0f * sc);
    if (hh > 2.0f) {
      dl.addRoundedRectFilled({b.x0 - hw, my - hh, b.x0 + hw + 1.0f, my + hh}, hw, th.roll.jiSelected);
      dl.addRoundedRectFilled({b.x1 - hw - 1.0f, my - hh, b.x1 + hw, my + hh}, hw, th.roll.jiSelected);
    }
    // T7.5: every later stop's glide — a bar along the bottom border from where the glide begins to the stop, a grip
    // at its start (drag it to set the glide length).
    s.jiLayout(v, lay);
    for (const JiLayout::Label& g : lay.glides) {
      if (g.box.x1 < vx0 || g.box.x0 > vx1) continue;
      const float gx = 0.5f * (g.box.x0 + g.box.x1);
      float sx = gx;
      for (const JiStopMark& m : v.stops)
        if (static_cast<int32_t>(m.index) == g.index) sx = std::round(s.xOfTick(static_cast<double>(v.start + m.at)));
      const float y = b.y1 - 1.0f, r = 4.5f * sc;
      if (sx > gx) dl.addRectFilled({gx, y - std::round(1.0f * sc), sx, y + std::round(1.0f * sc)}, th.roll.jiSelected);
      const Vec2 tri[3] = {{gx - r, y}, {gx + r * 0.6f, y - r}, {gx + r * 0.6f, y + r}};
      dl.addConvexPolyFilled(tri, th.roll.jiSelected);
    }
  }
  if (entered) {  // bezier transitions: the two handles
    for (const JiView::Bezier& bz : v.beziers) {
      const Vec2 p0{s.xOfTick(static_cast<double>(v.start + bz.begin)), s.yOfCents(bz.from)};
      const Vec2 p3{s.xOfTick(static_cast<double>(v.start + bz.arrive)), s.yOfCents(bz.to)};
      if (p3.x < vx0 || p0.x > vx1) continue;
      const Vec2 h1 = s.jiBezierHandle(v, bz, 0), h2 = s.jiBezierHandle(v, bz, 1);
      const Vec2 l1[2] = {p0, h1}, l2[2] = {p3, h2};
      smoothPolyline(dl, l1, th.roll.jiSelected, std::max(1.0f, sc));
      smoothPolyline(dl, l2, th.roll.jiSelected, std::max(1.0f, sc));
      dl.addCircleFilled(h1, 3.5f * sc, th.roll.jiSelected);
      dl.addCircleFilled(h2, 3.5f * sc, th.roll.jiSelected);
    }
  }

  return true;
  }
  if (!ts || font == kNoFont) return true;
  const Rgba text = selected ? th.roll.jiSelected : th.roll.jiText;
  if (b.x1 - b.x0 < 26.0f * sc) return true;  // too narrow for any text (a zoomed-out clip full of objects pays nothing)
  s.jiLayout(v, lay);  // the one placement paint and jiAt share
  auto labelText = [&](const Rect& box, const std::string& str, bool chosen) {
    if (box.x1 < vx0 || box.x0 > vx1 || box.y1 < vy0 || box.y0 > vy1) return;
    if (chosen) chip(dl, box, th.roll.jiSelected, sc);
    const IRect ib{static_cast<int32_t>(box.x0 + 2.0f * sc), static_cast<int32_t>(box.y0), static_cast<int32_t>(box.x1 - box.x0),
                   static_cast<int32_t>(box.y1 - box.y0)};
    paint::text(ctx, font, ib, str, chosen ? th.roll.rowNatural : text, TextAlign::Left);
  };
  // Harmonic numbers at the lines' left ends (above the line; a label that would sit on the previous one is skipped).
  const int32_t termSel = selected ? s.jiTermSelection() : -1;
  for (const JiLayout::Label& l : lay.voiceLabels)
    labelText(l.box, v.voiceLabels[static_cast<size_t>(l.index)], l.index == termSel);
  // The header, above the box, and the stops' proportions under it: not for a box too narrow to read them over (a
  // zoomed-out clip full of objects would only smear them across its neighbours).
  if (b.x1 - b.x0 < kJiTextMin * sc) return true;
  if (b.y0 > vy0 && b.y0 - hdrH < vy1) {
    const IRect box{static_cast<int32_t>(b.x0), static_cast<int32_t>(b.y0 - hdrH), static_cast<int32_t>(hdrW), static_cast<int32_t>(hdrH)};
    paint::text(ctx, font, box, v.header, text, TextAlign::Left, static_cast<int32_t>(sc));
  }
  // The stops' proportions under the box, each at its marker; one that would run into the previous label is skipped.
  const int32_t stopSel = selected ? s.jiStopSelection() : -1;
  for (const JiLayout::Label& l : lay.stopLabels)
    for (const JiStopMark& m : v.stops)
      if (static_cast<int32_t>(m.index) == l.index) labelText(l.box, m.label, l.index == stopSel);
  // T7.5: between the tuning and a proportion, how far each voice moves, right of the stop's marker.
  for (const JiStopMark& m : v.stops) {
    if (m.deltas.empty()) continue;
    const float x = std::round(s.xOfTick(static_cast<double>(v.start + m.at))) + 4.0f * sc;
    if (x < b.x0 + 34.0f * sc || x > vx1 || x + 44.0f * sc > b.x1) continue;
    float lastY = 1e30f;
    for (size_t i = 0; i < m.deltas.size(); ++i) {
      if (m.deltas[i].empty()) continue;
      const float y = s.yOfCents(m.deltaCents[i]) - 2.0f * sc - s.pathHalfThickness();
      if (std::fabs(y - lastY) < fh + 1.0f) continue;
      lastY = y;
      if (y < vy0 || y - fh > vy1) continue;
      const IRect box{static_cast<int32_t>(x), static_cast<int32_t>(std::lround(y - fh)) - 1, static_cast<int32_t>(60.0f * sc), static_cast<int32_t>(fh) + 2};
      paint::text(ctx, font, box, m.deltas[i], text, TextAlign::Left);
    }
  }
  return true;
}

}  // namespace

// --- views of the objects ---------------------------------------------------------------------------------------

JiView RollState::makeJiView(const doc::JiObject& o) const {
  JiView v;
  v.id = o.id;
  v.start = o.start;
  v.length = o.length;
  v.ride = o.ride;
  bool allProportions = true;
  for (size_t i = 0; i < o.stops.size(); ++i) {
    const doc::JiStop& stop = o.stops[i];
    allProportions = allProportions && !stop.isTuning();
    if (i > 0 && stop.at >= o.length) continue;  // kept in the document, never reached
    JiStopMark m{stop.at, stopLabel(stop, o.undertone, tuningName_)};
    m.index = static_cast<uint32_t>(i);
    if (i > 0) m.glide = std::clamp<Ticks>(stop.timing.glide, 0, stop.at - o.stops[i - 1].at);
    v.stops.push_back(std::move(m));
  }
  // The header: up to three stops read as the path itself ("12-EDO > 4:5:6"), a longer morph by its length.
  if (!o.stops.empty()) v.header = stopLabel(o.stops.front(), o.undertone, tuningName_);
  if (o.stops.size() > 1 && o.stops.size() <= 3) {
    for (size_t i = 1; i < o.stops.size(); ++i) v.header += " \xE2\x86\x92 " + stopLabel(o.stops[i], o.undertone, tuningName_);
  } else if (o.stops.size() > 1) {
    v.header += " \xC2\xB7 morphs through " + std::to_string(o.stops.size());
    v.header += allProportions ? " proportions" : " stops";
  }
  if (!o.ride.empty()) v.header += " \xC2\xB7 rides a curve";
  if (!o.sounds) v.header += " \xC2\xB7 tunes its notes";

  const uint32_t V = o.voices();
  v.voiceLabels.resize(V);
  const doc::JiStop* first = nullptr;
  for (const doc::JiStop& stop : o.stops)
    if (!stop.isTuning()) {
      first = &stop;
      break;
    }
  v.voiceEntry.assign(V, 0);
  if (first) {
    v.labelStop = static_cast<int32_t>(first - o.stops.data());
    uint32_t prev = UINT32_MAX;
    for (uint32_t i = 0; i < V; ++i) {
      const uint32_t e = doc::jiVoiceEntry(o, *first, i);
      v.voiceEntry[i] = e;
      if (e == prev || e >= first->terms.size()) continue;  // doubles the voice before it
      prev = e;
      v.voiceLabels[i] = (o.undertone ? "1/" : "") + std::to_string(first->terms[e]);
    }
  }
  if (!tuningValid_) return v;
  // T7.5: per transition — bezier handles, and between the tuning and a proportion how far each voice moves.
  {
    auto rideAt = [&](Ticks t) { return o.ride.empty() ? 0.0 : static_cast<double>(doc::automationValueAt(o.ride, std::clamp<Ticks>(t, 0, o.length))); };
    for (JiStopMark& m : v.stops) {
      if (m.index == 0) continue;
      const doc::JiStop& stop = o.stops[m.index];
      const bool cross = stop.isTuning() != o.stops[m.index - 1].isTuning();
      if (cross) {
        m.deltas.assign(V, {});
        m.deltaCents.assign(V, 0.0);
      }
      uint32_t prevEntry = UINT32_MAX;
      for (uint32_t i = 0; i < V; ++i) {
        const doc::JiTransition tr = doc::jiTransition(o, i, m.index);
        if (tr.arrive > o.length) continue;
        const std::optional<double> a = doc::jiVoiceCents(o, tuning_, i, static_cast<double>(tr.begin));
        const std::optional<double> b = doc::jiVoiceCents(o, tuning_, i, static_cast<double>(tr.arrive));
        if (!a || !b) continue;
        const double from = tr.arrive > tr.begin ? *a - rideAt(tr.begin) : *b - rideAt(tr.arrive), to = *b - rideAt(tr.arrive);
        if (tr.shape.kind == doc::JiShape::Bezier && tr.arrive > tr.begin)
          v.beziers.push_back({m.index, i, tr.begin, tr.arrive, from, to, tr.shape.x1, tr.shape.y1, tr.shape.x2, tr.shape.y2});
        const uint32_t e = doc::jiVoiceEntry(o, stop, i);
        if (cross && e != prevEntry && tr.arrive > tr.begin && std::fabs(to - from) >= 0.05) {
          m.deltas[i] = deltaText(to - from);
          m.deltaCents[i] = *b;
        }
        prevEntry = e;
      }
    }
  }
  doc::JiPlanOptions po;
  po.toleranceCents = 0.25;  // a quarter cent: under half a pixel up to 2 px per cent
  std::optional<doc::JiPlan> plan = doc::jiVoicePlan(o, tuning_, po);
  if (!plan || plan->voices.empty()) return v;
  v.plan = std::move(*plan);
  v.minCents = 1e300;
  v.maxCents = -1e300;
  v.distinct.assign(v.plan.voices.size(), 1);
  for (size_t i = 0; i < v.plan.voices.size(); ++i) {
    const std::vector<doc::JiPitchPoint>& pts = v.plan.voices[i].pitch;
    if (i > 0 && pts == v.plan.voices[i - 1].pitch) v.distinct[i] = 0;
    for (const doc::JiPitchPoint& p : pts) {
      v.minCents = std::min(v.minCents, p.cents);
      v.maxCents = std::max(v.maxCents, p.cents);
    }
  }
  v.boxLow = v.minCents;
  if (!o.ride.empty()) {
    float lo = o.ride.front().value, hi = lo;
    for (const doc::Breakpoint& p : o.ride) {
      lo = std::min(lo, p.value);
      hi = std::max(hi, p.value);
    }
    v.rideBase = v.minCents - static_cast<double>(hi);  // the curve's top touches the lowest pitch (less the gap)
    v.boxLow = v.rideBase + static_cast<double>(lo);
  }
  v.ok = v.maxCents >= v.minCents;
  return v;
}

void RollState::rebuildJiViews() {
  jiViews_.clear();
  jiViewsFor_ = clip_ ? clip_->ji.get() : nullptr;
  jiViewsDirty_ = false;
  jiPreview_.reset();
  jiPreviewObject_.reset();
  if (!clip_) {
    jiSel_ = {};
    jiEntered_ = {};
    jiStopSel_ = jiTermSel_ = -1;
    return;
  }
  const std::span<const doc::JiObject> objects = doc::jiObjectsOf(*clip_);
  jiViews_.reserve(objects.size());
  for (const doc::JiObject& o : objects) jiViews_.push_back(makeJiView(o));
  if (jiSel_.valid() && !doc::findJiObject(*clip_, jiSel_)) jiSel_ = {};  // undone / deleted elsewhere
  if (jiEntered_.valid() && !doc::findJiObject(*clip_, jiEntered_)) jiEntered_ = {};
  const doc::JiObject* act = jiObject(jiActive());
  if (!act || jiStopSel_ >= static_cast<int32_t>(act->stops.size())) jiStopSel_ = -1;
  if (!act || jiTermSel_ >= static_cast<int32_t>(act->voices())) jiTermSel_ = -1;
}

JiLayout& RollState::jiLayoutScratch() const noexcept { return jiLayoutScratch_; }

float RollState::pathHalfThickness() const noexcept {
  const float sc = scaleOf(*this);
  const float lane = static_cast<float>(std::max(1, rows_.laneHeight()));
  return 0.5f * std::clamp(0.55f * lane, 3.0f * sc, 9.0f * sc);
}

ui::Vec2 RollState::jiBezierHandle(const JiView& v, const JiView::Bezier& bz, int handle) const {
  const double u = handle == 0 ? bz.x1 : bz.x2, w = handle == 0 ? bz.y1 : bz.y2;
  const double t = static_cast<double>(bz.begin) + u * static_cast<double>(bz.arrive - bz.begin);
  return {xOfTick(static_cast<double>(v.start) + t), yOfCents(bz.from + (bz.to - bz.from) * w)};
}

void RollState::jiLayout(const JiView& v, JiLayout& out) const {
  out.clear();
  if (!v.ok) return;
  const float sc = scaleOf(*this);
  const ui::Rect b = jiBoxRect(v);
  const float w = b.x1 - b.x0;
  if (w < std::min(26.0f, kJiNarrow) * sc) return;  // nothing is placed on a box this narrow
  const JiFont f = jiFont(noteArea && noteArea->tree() ? noteArea->tree()->resources() : nullptr, sc);
  // Harmonic numbers: above each line's left end.
  if (w >= 26.0f * sc) {
    float lastY = 1e30f;
    const float lx = std::floor(b.x0 + 4.0f * sc);
    for (size_t i = 0; i < v.voiceLabels.size() && i < v.plan.voices.size(); ++i) {
      if (v.voiceLabels[i].empty()) continue;
      const float y = yOfCents(v.plan.voices[i].pitch.front().cents) - 2.0f * sc;
      if (std::fabs(y - lastY) < f.fh + 1.0f) continue;
      lastY = y;
      const float top = std::round(y) - std::floor(f.fh) - 1.0f;
      const float tw = std::min(f.measure(v.voiceLabels[i], sc) + 4.0f * sc, w - 6.0f * sc);
      out.voiceLabels.push_back({static_cast<int32_t>(i), {lx - 2.0f * sc, top, lx - 2.0f * sc + tw, top + std::floor(f.fh) + 2.0f}});
    }
  }
  if (w < kJiTextMin * sc) return;
  // The stops' proportions under the box, each at its marker; one that would run into the one before is skipped.
  const float lblH = f.fh + 4.0f * sc, gap = 8.0f * sc;
  float lastRight = -1e30f;
  for (size_t i = 0; i < v.stops.size(); ++i) {
    const float x = i == 0 ? b.x0 : std::round(xOfTick(static_cast<double>(v.start + v.stops[i].at)));
    if (x < lastRight + gap) continue;
    const float tw = f.measure(v.stops[i].label, sc) + 4.0f * sc;
    lastRight = x + tw;
    out.stopLabels.push_back({static_cast<int32_t>(v.stops[i].index), {x, b.y1 + 1.0f, x + tw, b.y1 + 1.0f + lblH}});
  }
  // Glide handles on the bottom border (the active object shows and grabs them).
  if (w >= kJiNarrow * sc)
    for (size_t i = 1; i < v.stops.size(); ++i) {
      const float gx = std::round(xOfTick(static_cast<double>(v.start + v.stops[i].at - v.stops[i].glide)));
      const float r = 5.0f * sc;
      out.glides.push_back({static_cast<int32_t>(v.stops[i].index), {gx - r, b.y1 - 1.0f - r, gx + r, b.y1 + 1.0f}});
    }
}

float RollState::yOfCents(double cents) const noexcept {
  if (rows_.empty()) return static_cast<float>(area().y);
  return static_cast<float>(static_cast<double>(area().y - scrollY_) + rows_.yOfCents(cents));
}

ui::Rect RollState::jiBoxRect(const JiView& v) const {
  const float sc = scaleOf(*this);
  const float x0 = std::round(xOfTick(static_cast<double>(v.start)));
  const float x1 = std::max(x0 + 3.0f, std::round(xOfTick(static_cast<double>(v.start + v.length))));
  const float top = std::round(yOfCents(v.maxCents) - (kJiPad + kJiLabelRoom) * sc);
  const float bottom = std::round(yOfCents(v.boxLow) + (kJiPad + (v.ride.empty() ? 0.0f : kJiRideGap)) * sc);
  return {x0, top, x1, std::max(bottom, top + 3.0f)};
}

ui::Rect RollState::jiBoxRect(const JiBox& b) const {
  const float x0 = std::round(xOfTick(static_cast<double>(b.start)));
  const float x1 = std::max(x0 + 3.0f, std::round(xOfTick(static_cast<double>(b.start + b.length))));
  const PitchRow* lo = rows_.rowOfDegree(b.low);
  const PitchRow* hi = rows_.rowOfDegree(b.high);
  if (!lo || !hi) return {x0, 0.0f, x1, 0.0f};
  return {x0, static_cast<float>(screenY(hi->top)), x1, static_cast<float>(screenY(lo->bottom))};
}

JiHit RollState::jiAt(ui::Vec2 p) const {
  if (!clip_ || jiViews_.empty()) return {};
  const float sc = scaleOf(*this);
  const float grab = kJiGrab * sc;
  const float hdrH = 17.0f * sc;
  const doc::JiObjectId active = jiActive();
  auto in = [&](const ui::Rect& r) { return p.x >= r.x0 && p.x < r.x1 && p.y >= r.y0 && p.y < r.y1; };
  for (size_t i = jiViews_.size(); i-- > 0;) {  // the last drawn is on top
    const JiView& v = jiViews_[i];
    if (!v.ok) continue;
    if (jiEntered_.valid() && v.id != jiEntered_) continue;  // T7.5: inside an object only it is hit
    const ui::Rect b = jiBoxRect(v);
    if (p.x < b.x0 - grab || p.y < b.y0 - hdrH || p.y > b.y1 + 22.0f * sc) continue;
    // T7.5: the handles of the active object (bezier handles, glide grips, harmonic numbers), then any object's
    // stop labels under its box.
    const bool act = v.id == active;
    if (act || p.y >= b.y1) {
      JiLayout& lay = jiLayoutScratch_;
      jiLayout(v, lay);
      if (act) {
        if (v.id == jiEntered_)
          for (size_t k = 0; k < v.beziers.size(); ++k)
            for (int h = 0; h < 2; ++h) {
              const ui::Vec2 q = jiBezierHandle(v, v.beziers[k], h);
              if (std::fabs(p.x - q.x) <= 6.0f * sc && std::fabs(p.y - q.y) <= 6.0f * sc)
                return {v.id, JiZone::Bezier, static_cast<int32_t>(k), static_cast<uint8_t>(h)};
            }
        for (const JiLayout::Label& g : lay.glides)
          if (in(g.box)) return {v.id, JiZone::Glide, g.index};
        for (const JiLayout::Label& l : lay.voiceLabels)
          if (in(l.box)) return {v.id, JiZone::VoiceLabel, l.index};
      }
      for (const JiLayout::Label& l : lay.stopLabels)
        if (in(l.box)) return {v.id, JiZone::StopLabel, l.index};
    }
    if (p.y > b.y1 + grab) continue;
    const float w = b.x1 - b.x0;
    // The header strip: as wide as the box (at least a short grip for a narrow one).
    if (p.y < b.y0 - grab) {
      if (p.x <= b.x0 + std::max(w, 60.0f * sc)) return {v.id, JiZone::Body};
      continue;
    }
    if (p.x > b.x1 + grab) continue;
    const bool inside = p.x > b.x0 + grab && p.x < b.x1 - grab && p.y > b.y0 + grab && p.y < b.y1 - grab;
    if (inside) return {v.id, JiZone::Inside};  // the inside belongs to the notes (a double-click enters)
    if (w >= 6.0f * grab) {
      if (p.x <= b.x0 + grab) return {v.id, JiZone::Start};
      if (p.x >= b.x1 - grab) return {v.id, JiZone::End};
    } else if (p.x >= b.x1) {
      return {v.id, JiZone::End};
    }
    return {v.id, JiZone::Body};
  }
  return {};
}

// --- tool and selection -----------------------------------------------------------------------------------------

void RollState::setJiTool(bool on) {
  if (on == jiTool_) return;
  jiTool_ = on;
  if (on) ctx.setDrawMode(false);
  ctx.setStatus(on ? "JI Tool on: drag a box, then type a proportion" : "JI Tool off");
  markNotesDirty();
  if (cornerPanel) cornerPanel->markDirty();
}

void RollState::selectJiObject(doc::JiObjectId id) {
  if (id.valid()) select({});
  if (id == jiSel_) return;
  jiSel_ = id;
  jiStopSel_ = jiTermSel_ = -1;  // T7.5: they belong to the object that was selected
  markNotesDirty();
}

bool RollState::applyJi(const doc::ops::EditResult& e, std::string_view what, doc::JiObjectId keep) {
  const bool ok = apply(e, what);  // adopts the edit's notes as the selection
  if (ok) {
    select({});
    jiSel_ = (keep.valid() && clip_ && doc::findJiObject(*clip_, keep)) ? keep : doc::JiObjectId{};
    if (!jiSel_.valid() && !jiEntered_.valid()) jiStopSel_ = jiTermSel_ = -1;
    markNotesDirty();
  }
  return ok;
}

// --- creating ---------------------------------------------------------------------------------------------------

Result<JiProportion> RollState::parseProportion(std::string_view text) {
  JiProportion out;
  int over = 0, under = 0;
  size_t pos = 0;
  auto trim = [](std::string_view t) {
    while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.remove_prefix(1);
    while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) t.remove_suffix(1);
    return t;
  };
  if (trim(text).empty()) return invalid("type a proportion such as 4:5:6 (or 1/4:1/5:1/6)");
  while (pos <= text.size()) {
    const size_t colon = text.find(':', pos);
    std::string_view part = trim(text.substr(pos, colon == std::string_view::npos ? std::string_view::npos : colon - pos));
    pos = colon == std::string_view::npos ? text.size() + 1 : colon + 1;
    bool isUnder = false;
    if (part.size() > 2 && part.substr(0, 2) == "1/") {
      isUnder = true;
      part = trim(part.substr(2));
    }
    int64_t term = 0;
    const auto [end, ec] = std::from_chars(part.data(), part.data() + part.size(), term);
    if (part.empty() || ec != std::errc{} || end != part.data() + part.size())
      return invalid("\"" + std::string(part) + "\" is not a whole number: type 4:5:6 or 1/4:1/5:1/6");
    if (term < 1 || term > doc::kMaxJiTerm) return invalid("terms go from 1 to " + std::to_string(doc::kMaxJiTerm));
    (isUnder ? under : over)++;
    out.terms.push_back(term);
  }
  if (over && under) return invalid("mix of n and 1/n: type 4:5:6 (overtone) or 1/4:1/5:1/6 (undertone)");
  if (out.terms.size() > doc::kMaxJiVoices) return invalid("at most " + std::to_string(doc::kMaxJiVoices) + " terms");
  out.undertone = under > 0;
  if (out.undertone) std::sort(out.terms.begin(), out.terms.end());  // 1/6:1/5:1/4 is the same chord
  for (size_t i = 1; i < out.terms.size(); ++i)
    if (out.terms[i] <= out.terms[i - 1])
      return invalid(out.undertone ? "every 1/n once" : "terms must ascend, such as 4:5:6");
  return out;
}

bool RollState::createJiObject(const JiBox& box, std::string_view text, std::string* error) {
  auto fail = [&](std::string m) {
    if (error) *error = std::move(m);
    return false;
  };
  if (!clip_) return fail("no clip");
  Result<JiProportion> pr = parseProportion(text);
  if (!pr) return fail(pr.error().message);
  if (box.length < 1) return fail("the box has no length");
  doc::ops::EditResult e = doc::ops::createJiObject(*ctx.doc(), ctx.store().ids(), ref_, box.start, box.length,
                                                    degreePitch(box.low), pr->terms, pr->undertone);
  if (!e) return fail(e.error().message);
  doc::ops::Edit edit = std::move(*e);
  // The lowest voice sits on the box's bottom row (see the header): its ratio to the fundamental is exactly 1.
  const tuning::Rational scale = pr->undertone ? tuning::Rational(pr->terms.back()) : tuning::Rational(1, pr->terms.front());
  doc::JiObjectId id = edit.jiObjects.empty() ? doc::JiObjectId{} : edit.jiObjects.front();
  if (doc::Batch* b = std::get_if<doc::Batch>(&edit.command.v))
    for (doc::Command& c : b->commands)
      if (doc::AddJiObject* a = std::get_if<doc::AddJiObject>(&c.v)) {
        a->object.stops.front().scale = scale;
        if (Status st = doc::validateJiObject(a->object); !st) return fail(st.error().message);
      }
  if (!applyJi(edit, "Create JI Object", id)) return fail("the document refused the object");
  return true;
}

ui::TextEdit* RollState::openJiEntry(const JiBox& box) {
  if (!clip_ || !noteArea) return nullptr;
  ui::OverlayLayer* layer = ui::OverlayLayer::of(*noteArea);
  if (!layer) return nullptr;
  jiPending_ = box;
  markNotesDirty();
  const ui::Rect br = jiBoxRect(box);
  ui::IRect anchor = intersect(ui::IRect{static_cast<int32_t>(br.x0), static_cast<int32_t>(br.y0), static_cast<int32_t>(br.x1 - br.x0),
                                         static_cast<int32_t>(br.y1 - br.y0)},
                               area());
  if (anchor.empty()) anchor = {area().x + 8, area().y + 8, 1, 1};
  auto pop = std::make_unique<ui::Popover>(layer->uniqueName("ji-entry"));
  ui::Popover* popPtr = pop.get();
  pop->setSize(ui::Length::fixed(280), ui::Length::fixed(78));
  pop->setInfo("JI Object", "Type the chord's proportion and press Enter: 8:9:10:12:15 (harmonics over the fundamental) or "
                            "1/4:1/5:1/6 (an undertone chord). Its lowest voice sits on the box's bottom row. Esc drops the "
                            "box.");
  pop->add<ui::Label>("title", "Proportion: 8:9:10:12:15 \xC2\xB7 1/4:1/5:1/6", ui::FontRoleId::Small, ui::TextTone::Secondary)
      .setHeight(ui::Length::fixed(16));
  auto& edit = pop->add<ui::TextEdit>("proportion");
  edit.setHeight(ui::Length::fixed(20));
  auto& err = pop->add<ui::Label>("error", "", ui::FontRoleId::Small, ui::TextTone::Primary);
  err.setHeight(ui::Length::fixed(16));
  std::shared_ptr<RollState*> alive = self_;
  ui::TextEdit* editPtr = &edit;
  ui::Label* errPtr = &err;
  edit.onEnter = [alive, layer, popPtr, editPtr, errPtr, box] {
    RollState* s = *alive;
    if (!s) return;
    std::string why;
    const std::string text = editPtr->text();
    if (!s->createJiObject(box, text, &why)) {
      errPtr->setText(why);  // the field stays open; nothing was created
      s->ctx.setStatus("JI Object: " + why);
      return;
    }
    if (layer->isOpen(*popPtr)) layer->close(*popPtr);
  };
  edit.onCancel = [layer, popPtr] {
    if (layer->isOpen(*popPtr)) layer->close(*popPtr);
  };
  ui::OverlayOptions o;
  o.anchor = anchor;
  o.placement = ui::Placement::Below;
  o.onClosed = [alive] {
    if (RollState* s = *alive) {
      s->jiPending_.reset();
      s->markNotesDirty();
    }
  };
  layer->open(std::move(pop), std::move(o));
  if (ui::UiTree* t = layer->tree()) {
    t->layoutNow();
    t->setFocus(&edit);
  }
  return &edit;
}

// --- editing ----------------------------------------------------------------------------------------------------

bool RollState::deleteJiSelection() {
  if (!clip_ || !jiSel_.valid()) return false;
  return applyJi(doc::ops::deleteJiObject(*ctx.doc(), ref_, jiSel_), "Delete JI Object", {});
}

bool RollState::duplicateJiSelection() {
  if (!clip_ || !jiSel_.valid()) return false;
  const doc::JiObject* o = doc::findJiObject(*clip_, jiSel_);
  if (!o) return false;
  const doc::ops::EditResult e = doc::ops::duplicateJiObject(*ctx.doc(), ctx.store().ids(), ref_, jiSel_, o->length);
  return applyJi(e, "Duplicate JI Object", e && !e->jiObjects.empty() ? e->jiObjects.front() : doc::JiObjectId{});
}

doc::ops::EditResult RollState::jiDragEdit(const DragState& d) const {
  if (!clip_) return invalid("no clip");
  const doc::Project& p = *ctx.doc();
  switch (d.kind) {
    case DragKind::JiResizeStart:
      return doc::ops::resizeJiObject(p, ctx.store().ids(), ref_, d.ji, doc::ops::NoteEdge::Start, d.dt);
    case DragKind::JiResizeEnd:
      return doc::ops::resizeJiObject(p, ctx.store().ids(), ref_, d.ji, doc::ops::NoteEdge::End, d.dt);
    // By rows the fundamental and every tuning stop's pitches step as a note of that identity does; with Alt (cents)
    // they move freely (ops::moveJiObject, Q279).
    case DragKind::JiMove: return doc::ops::moveJiObject(p, ref_, d.ji, d.dt, d.steps, overlay(), d.cents);
    case DragKind::JiStop:
    case DragKind::JiGlide:
    case DragKind::JiBezier: {
      const doc::JiObject* o = doc::findJiObject(*clip_, d.ji);
      if (!o || d.jiStop < 1 || d.jiStop >= static_cast<int32_t>(o->stops.size())) return invalid("no such JI stop");
      std::vector<doc::JiStop> stops = o->stops;
      const size_t i = static_cast<size_t>(d.jiStop);
      doc::JiStop& stop = stops[i];
      if (d.kind == DragKind::JiStop) {
        // Between its neighbours, inside the object.
        const Ticks lo = stops[i - 1].at + 1;
        const Ticks hi = i + 1 < stops.size() ? stops[i + 1].at - 1 : std::max(stop.at, o->length - 1);
        stop.at = std::clamp(stop.at + d.dt, lo, std::max(lo, hi));
      } else if (d.kind == DragKind::JiGlide) {
        const Ticks gap = stop.at - stops[i - 1].at;
        stop.timing.glide = std::clamp<Ticks>(std::min(stop.timing.glide, gap) - d.dt, 0, gap);
      } else {
        const uint32_t V = o->voices();
        if (d.jiVoice >= V) return invalid("no such voice");
        if (stop.voiceTiming.size() != V) stop.voiceTiming.assign(V, std::nullopt);
        doc::JiTiming t = stop.voiceTiming[d.jiVoice].value_or(stop.timing);
        t.shape.kind = doc::JiShape::Bezier;
        (d.jiHandle == 0 ? t.shape.x1 : t.shape.x2) = std::clamp(d.bx, 0.0f, 1.0f);
        (d.jiHandle == 0 ? t.shape.y1 : t.shape.y2) = std::clamp(d.by, -1.0f, 2.0f);
        stop.voiceTiming[d.jiVoice] = t;
      }
      return doc::ops::setJiStops(p, ctx.store().ids(), ref_, d.ji, std::move(stops));
    }
    default: return invalid("not a JI drag");
  }
}

void RollState::updateJiPreview() {
  jiPreview_.reset();
  jiPreviewObject_.reset();
  if (!jiDragKind(drag.kind)) return;
  const doc::ops::EditResult e = jiDragEdit(drag);
  if (!e) return;  // refused (e.g. a start dragged past the end): the object is shown as it is
  if (const doc::Batch* b = std::get_if<doc::Batch>(&e->command.v))
    for (const doc::Command& c : b->commands)
      if (const doc::SetJiObject* set = std::get_if<doc::SetJiObject>(&c.v); set && set->object.id == drag.ji) {
        jiPreview_ = makeJiView(set->object);
        jiPreviewObject_ = set->object;
      }
}

// --- pitch paths ------------------------------------------------------------------------------------------------

bool RollState::onPath(const doc::Note& n, const tuning::PitchId& pitch) const noexcept {
  if (!clip_) return false;
  if (!clip_->curves && !clip_->ji) return false;
  if (clip_->ji && doc::jiObjectOf(pitch).valid()) return true;
  return clip_->curves && doc::findNoteCurves(*clip_, n.id) != nullptr;
}

std::optional<double> RollState::pathCents(const doc::Note& n, Ticks start, const tuning::PitchId& pitch, double offset) const {
  if (!clip_) return std::nullopt;
  double base = 0.0;
  if (const doc::JiObjectId oid = doc::jiObjectOf(pitch); oid.valid()) {
    // T7.5: while an object is dragged its inner notes are drawn on the object as the release will leave it.
    const doc::JiObject* o = (jiPreviewObject_ && jiPreviewObject_->id == oid) ? &*jiPreviewObject_ : doc::findJiObject(*clip_, oid);
    if (!o) return std::nullopt;
    const double t = static_cast<double>(start - o->start) + offset;
    std::optional<double> c;
    if (pitch.kind() == tuning::PitchKind::Harmonic) {
      c = doc::jiVoiceCents(*o, tuning_, pitch.voiceIndex(), t);
    } else {
      c = doc::jiFundamentalCents(*o, tuning_, t);
      if (c) *c += tuning::cents(pitch.ratio());
    }
    if (!c) return std::nullopt;
    base = *c;
  } else {
    const std::optional<double> c = centsOf(pitch);
    if (!c) return std::nullopt;
    base = *c;
  }
  if (clip_->curves)
    if (const doc::NoteCurves* nc = doc::findNoteCurves(*clip_, n.id))
      base += doc::automationValueAt(nc->pitch, static_cast<Ticks>(std::llround(offset)));
  return base;
}

// --- paint ------------------------------------------------------------------------------------------------------

void paintJiObjects(PaintContext& ctx, RollState& s, const IRect& vis, bool labels, JiPass pass) {
  if (!ctx.res.theme || !s.hasClip()) return;
  const Theme& th = *ctx.res.theme;
  const DragState& dr = s.drag;
  const JiView* pv = (jiDragKind(dr.kind) && dr.moved) ? s.jiPreview() : nullptr;
  const doc::JiObjectId entered = s.jiEntered(), active = s.jiActive();
  uint32_t painted = 0;
  for (const JiView& v : s.jiViews()) {
    if (pass == JiPass::NotEntered && v.id == entered) continue;
    if (pass == JiPass::EnteredOnly && v.id != entered) continue;
    const bool dragged = pv && pv->id == v.id;
    painted += paintJiObject(ctx, s, dragged ? *pv : v, v.id == active, vis, labels) ? 1u : 0u;
  }
  if (labels) return;
  s.paintStats.jiPainted = painted;
  // The box being drawn, or waiting for its proportion.
  std::optional<JiBox> box = s.jiPending();
  if (dr.kind == DragKind::JiBox && dr.moved && dr.jiLength > 0) box = JiBox{dr.jiStart, dr.jiLength, dr.jiLow, dr.jiHigh};
  if (box) {
    const Rect b = s.jiBoxRect(*box);
    if (b.y1 > b.y0) {
      const float sc = ctx.scale.factor;
      const Rect fill{std::max(b.x0, static_cast<float>(vis.x)), std::max(b.y0, static_cast<float>(vis.y)),
                      std::min(b.x1, static_cast<float>(vis.right())), std::min(b.y1, static_cast<float>(vis.bottom()))};
      if (fill.x1 > fill.x0 && fill.y1 > fill.y0) ctx.dl.addRectFilled(fill, th.roll.jiFill);
      boxBorder(ctx.dl, b, th.roll.jiSelected, std::round(4.0f * sc), std::round(3.0f * sc), vis);
    }
  }
}

void paintCurveNotes(PaintContext& ctx, RollState& s, const IRect& vis, JiPass pass) {
  if (!ctx.res.theme || s.curveNotes.empty()) return;
  const doc::JiObjectId entered = s.jiEntered();
  const NoteColours& col = s.colours();
  const float sc = ctx.scale.factor;
  const float step = std::max(1.0f, kJiSampleStep * sc);
  const float lane = static_cast<float>(std::max(1, s.rows().laneHeight()));
  const float thick = std::clamp(0.55f * lane, 3.0f * sc, 9.0f * sc);
  const float vx0 = static_cast<float>(vis.x) - 2.0f * step, vx1 = static_cast<float>(vis.right()) + 2.0f * step;
  std::vector<Vec2>& pts = s.linePts;
  for (const CurveNote& c : s.curveNotes) {
    if (pass != JiPass::All && (doc::jiObjectOf(c.pitch) == entered) != (pass == JiPass::EnteredOnly)) continue;
    const float x0 = s.xOfTick(static_cast<double>(c.start));
    const float x1 = std::max(x0 + 3.0f, s.xOfTick(static_cast<double>(c.start + c.length)));
    const float xa = std::max(x0, vx0), xb = std::min(x1, vx1);
    if (xb <= xa) continue;
    pts.clear();
    const int n = std::clamp(static_cast<int>(std::ceil((xb - xa) / step)), 1, 8192);
    bool ok = true;
    for (int k = 0; k <= n && ok; ++k) {
      const float x = xa + (xb - xa) * static_cast<float>(k) / static_cast<float>(n);
      const double off = std::clamp(s.tickAtX(x) - static_cast<double>(c.start), 0.0, static_cast<double>(c.length));
      const std::optional<double> cents = s.pathCents(*c.note, c.start, c.pitch, off);
      if (!cents) ok = false;
      else pts.push_back({x, s.yOfCents(*cents)});
    }
    if (!ok || pts.size() < 2) continue;
    Rgba fill, border;
    if (!c.note->enabled()) {
      fill = col.disabledFill;
      border = col.disabledBorder;
    } else {
      const int v = std::clamp(static_cast<int>(std::lround(c.note->velocity)), 1, 127);
      fill = c.selected ? col.selFill[v] : col.fill[v];
      border = c.selected ? col.selBorder : col.border[v];
    }
    smoothPolyline(ctx.dl, pts, border, thick + 2.0f);
    smoothPolyline(ctx.dl, pts, fill, thick);
  }
}

}  // namespace pq::views::roll
