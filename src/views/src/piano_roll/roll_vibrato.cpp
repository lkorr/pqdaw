// T7.14 piano roll: vibrato by direct manipulation (docs/plans/M7.md "Vibrato by direct manipulation" V1–V3; the
// user: "rightclicking and entering manual values is a no go", "same with fading in/out the vibrato amplitude").
//
// A vibrato is a property of the note (doc::NoteVibrato on its curve record: depth, rate, onset, fade-in, end,
// fade-out); doc::effectiveNoteCurve adds it wherever the note's curve is read, so the wave drawn on the note is the
// wave that plays. Nothing is typed and nothing is baked into breakpoints.
//
//   the ~ grip      above the right end of the hovered note and of every selected note. Drag up / down: the depth
//                   (1 ¢ steps, the wave grows on the note; down to 0 = off). Drag left / right (past a dead zone,
//                   so setting the depth does not change it): the rate — right stretches the waves (slower), left
//                   squeezes them (faster). Shift = fine. Double-click: off. With the note in a selection the drag
//                   applies to every selected note (each keeps its own envelope).
//   the corners     once a note vibrates: four square grips under the note at the corners of its amplitude envelope —
//                   onset, end of the fade-in, start of the fade-out, end. Drag sideways (grid snap; Shift: free).
//                   Each stays between its neighbours; the two inner ones can meet (a swell). Two corners on one
//                   tick (no fade yet): the drag's direction picks which one moves.
//   status line     "Vibrato 35 ¢ · 5.5 Hz" while the grip is dragged; the corner's position while a corner is.
// Each drag previews the record its release stores (RollState::curvePreview_) and is one undo step
// (ops::setNoteVibrato: one SetNoteCurves for every note).
//
// Cost: a drag frame re-flattens each dragged note's path once (RollState::pathOf's preview entries, T7.16a M15: not
// once per query — the grips, the envelope and the paint all read the same entry); paint adds the grips
// of at most 64 selected notes.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <string>

#include "pq/document/automation_curve.h"
#include "pq/document/commands.h"
#include "pq/document/note_bend.h"
#include "pq/ui/lines.h"
#include "pq/ui/theme.h"
#include "roll_widgets.h"

namespace pq::views::roll {

using namespace pq::ui;

namespace {

float scaleOf(const RollState& s) noexcept { return (s.noteArea && s.noteArea->tree()) ? s.noteArea->tree()->scale() : 1.0f; }

std::string number(double v, int decimals = 1) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
  std::string s = buf;
  if (s.find('.') != std::string::npos) {
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
  }
  return s.empty() ? "0" : s;
}

// The four corners of a vibrato's envelope on a note `length` long, in note ticks.
struct Corners {
  Ticks c[4];
};
Corners cornersOf(const doc::NoteVibrato& v, Ticks length) {
  const Ticks L = std::max<Ticks>(length, 1);
  const Ticks stop = doc::vibratoStop(v, L);
  const Ticks on = std::clamp<Ticks>(v.onset, 0, stop);
  const Ticks c1 = std::clamp<Ticks>(on + std::max<Ticks>(v.fadeIn, 0), on, stop);
  const Ticks c2 = std::clamp<Ticks>(stop - std::max<Ticks>(v.fadeOut, 0), c1, stop);
  return {{on, c1, c2, stop}};
}

// The envelope's amplitude (0 … 1) at a note tick.
double envelopeAt(const doc::NoteVibrato& v, Ticks length, double t) {
  const Ticks L = std::max<Ticks>(length, 1);
  const double on = static_cast<double>(v.onset), stop = static_cast<double>(doc::vibratoStop(v, L));
  if (t < on || t > stop) return 0.0;
  double a = 1.0;
  if (v.fadeIn > 0) a = std::min(a, (t - on) / static_cast<double>(v.fadeIn));
  if (v.fadeOut > 0) a = std::min(a, (stop - t) / static_cast<double>(v.fadeOut));
  return std::max(a, 0.0);
}

}  // namespace

std::string RollState::vibratoText(double depthCents, double rateHz) {
  return "Vibrato " + number(depthCents) + " \xC2\xA2 \xC2\xB7 " + number(rateHz, 2) + " Hz";
}

double RollState::defaultVibratoRate(Ticks at) const {
  const Grid g = grid();
  if (!g.enabled()) return 6.0;
  const TickRange cell = g.cellAt(at);
  const double len = static_cast<double>(std::max<Ticks>(1, cell.end - cell.start));
  double hz = ticksPerSecond(at) / len;  // one cycle per grid cell …
  if (!(hz > 0.0) || !std::isfinite(hz)) return 6.0;
  const double lo = 3.0 * std::numbers::sqrt2;  // … folded by octaves into 4.24 … 8.49 Hz (around 6)
  while (hz < lo) hz *= 2.0;
  while (hz >= 2.0 * lo) hz *= 0.5;
  return std::round(hz * 10.0) / 10.0;
}

doc::NoteVibrato RollState::vibratoOf(doc::NoteId note) const {
  const doc::NoteCurves* rec = curveRecord(note);
  return rec ? rec->vibrato : doc::NoteVibrato{};
}

void RollState::vibratoNotes(std::vector<const doc::Note*>& out) const {
  out.clear();
  if (!clip_) return;
  auto add = [&](uint32_t id) {  // (deduplicated once at the end: a drag over many notes stays O(k log k), M15)
    if (id == 0) return;
    if (const doc::Note* n = findNote(doc::NoteId{id})) out.push_back(n);
  };
  // The selected notes, while they are few and wide enough to carry a grip each (a selected run of 16ths stays
  // clean); the note under the pointer always.
  if (sel_.size() <= kMaxVibratoGripNotes) {
    cacheSelectedNotes();
    const float sc = (noteArea && noteArea->tree()) ? noteArea->tree()->scale() : 1.0f;
    for (const uint32_t id : sel_)
      if (const doc::Note* n = findNote(doc::NoteId{id}); n && (static_cast<double>(n->length) * pxPerTick_ >= 3.0 * kVibratoGrip * sc || vibratoOf(n->id).set())) add(id);
  }
  add(hoverPart_.note);
  for (const DragState::VibratoTarget& t : drag.vibrato) add(t.note.value);
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
}

// The note's pitch `off` ticks in without its vibrato, as a device y (the centre the wave swings around).
static std::optional<float> steadyY(const RollState& s, const doc::Note& n, double off) {
  const CurveTarget t{n.id};
  std::optional<double> c = s.curveBase(t, off);
  if (!c) return std::nullopt;
  if (const std::span<const doc::Breakpoint> free = s.curvePoints(t); !free.empty()) *c += pq::evalAutomation(free, off);
  return s.yOfCents(*c);
}

// A depth in cents as device px at the note's pitch.
static float depthPx(const RollState& s, const doc::Note& n, double cents) {
  const std::optional<double> c = s.startCents(n);
  if (!c) return 0.0f;
  return std::fabs(s.yOfCents(*c + cents) - s.yOfCents(*c));
}

std::optional<ui::Vec2> RollState::vibratoGripPos(const doc::Note& n) const {
  if (!clip_ || rows_.empty()) return std::nullopt;
  const float sc = scaleOf(*this);
  const float x0 = xOfTick(static_cast<double>(n.start)), x1 = xOfTick(static_cast<double>(n.start + n.length));
  const float x = std::max(x1 - kVibratoGripInset * sc, std::min(x0 + 0.5f * kVibratoGrip * sc, x1));
  const double off = std::clamp(tickAtX(x) - static_cast<double>(n.start), 0.0, static_cast<double>(n.length));
  const std::optional<float> y = steadyY(*this, n, off);
  if (!y) return std::nullopt;
  // Above the line, and above the wave's crest there: the grip rises as the depth grows.
  const doc::NoteVibrato v = vibratoOf(n.id);
  const float crest = v.set() ? depthPx(*this, n, static_cast<double>(v.depthCents) * envelopeAt(v, n.length, off)) : 0.0f;
  return ui::Vec2{x, *y - kVibratoGripRise * sc - crest};
}

std::optional<ui::Vec2> RollState::vibratoCornerPos(const doc::Note& n, int corner) const {
  if (!clip_ || rows_.empty() || corner < 0 || corner > 3) return std::nullopt;
  const doc::NoteVibrato v = vibratoOf(n.id);
  if (!v.set()) return std::nullopt;
  const Corners k = cornersOf(v, n.length);
  const double off = static_cast<double>(k.c[corner]);
  const std::optional<float> y = steadyY(*this, n, off);
  if (!y) return std::nullopt;
  const float sc = scaleOf(*this);
  const float a = depthPx(*this, n, static_cast<double>(v.depthCents) * envelopeAt(v, n.length, off));
  // Under the line, on the envelope's lower edge (never nearer the line than the rise, so it stays clear of it).
  return ui::Vec2{xOfTick(static_cast<double>(n.start) + off), *y + std::max(a, kVibratoGripRise * sc)};
}

RollState::VibratoHit RollState::vibratoPartAt(ui::Vec2 p) const {
  VibratoHit hit;
  if (!clip_) return hit;
  static std::vector<const doc::Note*> notes;  // UI thread only; reused
  vibratoNotes(notes);
  const float half = 0.5f * kVibratoGrip * scaleOf(*this);
  float best = 1e30f;
  for (const doc::Note* n : notes) {
    if (const std::optional<ui::Vec2> g = vibratoGripPos(*n); g && std::fabs(p.x - g->x) <= half && std::fabs(p.y - g->y) <= half) {
      const float d = (p.x - g->x) * (p.x - g->x) + (p.y - g->y) * (p.y - g->y);
      if (d < best) {
        best = d;
        hit = {VibratoPart::Grip, n->id, -1};
      }
    }
    for (int c = 0; c < 4; ++c) {
      const std::optional<ui::Vec2> k = vibratoCornerPos(*n, c);
      if (!k || std::fabs(p.x - k->x) > half || std::fabs(p.y - k->y) > half) continue;
      const float d = (p.x - k->x) * (p.x - k->x) + (p.y - k->y) * (p.y - k->y);
      if (d < best) {
        best = d;
        hit = {VibratoPart::Corner, n->id, c};
      }
    }
  }
  return hit;
}

void RollState::beginVibratoDrag(const VibratoHit& h, ui::Vec2 p) {
  drag = {};
  const doc::Note* pressed = clip_ ? findNote(h.note) : nullptr;
  if (!pressed || !h.valid()) return;
  DragState& d = drag;
  d.kind = h.part == VibratoPart::Grip ? DragKind::Vibrato : DragKind::VibratoCorner;
  d.pressed = h.note;
  d.press = d.last = p;
  d.corner = h.corner;
  // The pressed note, and with it the selection when it is part of one.
  auto add = [&](const doc::Note& n) {
    const doc::NoteVibrato v = vibratoOf(n.id);
    if (d.kind == DragKind::VibratoCorner && !v.set()) return;
    d.vibrato.push_back({n.id, v, std::max<Ticks>(n.length, 1)});
  };
  if (selected(h.note)) {
    for (const doc::Note& n : clip_->notes)
      if (selected(n.id)) add(n);
  } else {
    add(*pressed);
  }
  std::sort(d.vibrato.begin(), d.vibrato.end(), [](const DragState::VibratoTarget& a, const DragState::VibratoTarget& b) { return a.note < b.note; });
  if (d.vibrato.empty()) {
    drag = {};
    return;
  }
  if (d.kind == DragKind::VibratoCorner) {
    const Corners k = cornersOf(vibratoOf(h.note), pressed->length);
    d.originTick = pressed->start + k.c[h.corner];
    // Two corners on one tick: the first movement picks (right: the later one, left: the earlier).
    d.curveIndex = -1;
    for (int c = 0; c < 4; ++c)
      if (c != h.corner && k.c[c] == k.c[h.corner] && std::abs(c - h.corner) == 1) d.curveIndex = c;
  } else {
    const doc::NoteVibrato v = vibratoOf(h.note);
    d.vibDepth = v.set() ? static_cast<double>(v.depthCents) : 0.0;
    d.vibRate = v.set() ? static_cast<double>(v.rate) : defaultVibratoRate(pressed->start);
  }
}

void RollState::vibratoDragTo(ui::Vec2 p, ui::Mods mods) {
  DragState& d = drag;
  if (!clip_ || d.vibrato.empty()) return;
  const float sc = scaleOf(*this);
  const bool fine = (mods & mod::kShift) != 0;
  const doc::Note* pressed = findNote(d.pressed);
  if (!pressed) return;
  const auto base = std::lower_bound(d.vibrato.begin(), d.vibrato.end(), d.pressed,
                                     [](const DragState::VibratoTarget& t, doc::NoteId id) { return t.note < id; });
  if (base == d.vibrato.end() || base->note != d.pressed) return;
  if (d.kind == DragKind::Vibrato) {
    // Up / down: the depth, in the roll's own pitch scale (the crest follows the pointer). Left / right: the rate.
    double dc = centsAtScreenY(p.y) - centsAtScreenY(d.press.y);
    if (fine) dc *= 0.1;
    const double depth0 = base->base.set() ? static_cast<double>(base->base.depthCents) : 0.0;
    const double step = fine ? 0.1 : 1.0;
    const double depth = std::clamp(std::round((depth0 + dc) / step) * step, 0.0, static_cast<double>(doc::kMaxVibratoCents));
    const float dead = 8.0f * sc;
    float dx = p.x - d.press.x;
    dx = std::fabs(dx) <= dead ? 0.0f : dx - std::copysign(dead, dx);
    const bool beats = base->base.set() && base->base.unit == doc::NoteVibrato::RateUnit::Beats;
    const double rate0 = base->base.set() ? static_cast<double>(base->base.rate) : defaultVibratoRate(pressed->start);
    const double lo = beats ? doc::kMinVibratoBeats : doc::kMinVibratoHz, hi = beats ? doc::kMaxVibratoBeats : doc::kMaxVibratoHz;
    const double rstep = fine ? 0.01 : 0.1;
    double rate = rate0 * std::exp2(-static_cast<double>(dx) / (static_cast<double>(fine ? 1200.0f : 120.0f) * sc));
    rate = dx == 0.0f ? rate0 : std::clamp(std::round(rate / rstep) * rstep, lo, hi);
    d.vibDepth = depth;
    d.vibRate = rate;
    for (const DragState::VibratoTarget& t : d.vibrato) {
      doc::NoteVibrato v = t.base;
      if (depth <= 0.0) {
        v = {};
      } else {
        v.depthCents = static_cast<float>(depth);
        v.rate = static_cast<float>(rate);
        v.unit = beats ? doc::NoteVibrato::RateUnit::Beats : doc::NoteVibrato::RateUnit::Hz;
      }
      previewRecord(t.note).vibrato = v;
    }
    if (depth <= 0.0) ctx.setStatus("Vibrato off");
    else if (beats) ctx.setStatus("Vibrato " + number(depth) + " \xC2\xA2 \xC2\xB7 " + number(rate, 2) + " per beat");
    else ctx.setStatus(vibratoText(depth, rate));
    markNotesDirty();
    return;
  }
  // A corner, sideways.
  const float dxPx = p.x - d.press.x;
  if (d.curveIndex >= 0 && dxPx != 0.0f) {  // two corners shared the tick: the direction picks
    d.corner = dxPx > 0.0f ? std::max(d.corner, d.curveIndex) : std::min(d.corner, d.curveIndex);
    d.curveIndex = -1;
  }
  const Grid g = grid();
  const Ticks raw = static_cast<Ticks>(std::llround(tickAtX(p.x) - tickAtX(d.press.x)));
  const Ticks dt = !fine && g.snapping() ? g.snapDelta(d.originTick, raw) : raw;
  d.dt = dt;
  for (const DragState::VibratoTarget& t : d.vibrato) {
    Corners k = cornersOf(t.base, t.length);
    const int c = d.corner;
    Ticks lo = c > 0 ? k.c[c - 1] : 0, hi = c < 3 ? k.c[c + 1] : t.length;
    if (c == 0) hi = std::min(hi, std::max<Ticks>(k.c[3] - 1, 0));  // the onset stays before the end
    if (c == 3) lo = std::max(lo, k.c[0] + 1);
    k.c[c] = std::clamp(k.c[c] + dt, lo, std::max(lo, hi));
    doc::NoteVibrato v = t.base;
    v.onset = k.c[0];
    v.fadeIn = k.c[1] - k.c[0];
    v.end = k.c[3] >= t.length ? 0 : k.c[3];
    v.fadeOut = k.c[3] - k.c[2];
    previewRecord(t.note).vibrato = v;
  }
  {
    const doc::NoteVibrato v = vibratoOf(d.pressed);
    const Corners k = cornersOf(v, pressed->length);
    const double beat = static_cast<double>(kPpq);
    static const char* const kWhat[4] = {"Vibrato starts", "Vibrato fade-in", "Vibrato fade-out", "Vibrato ends"};
    std::string s = kWhat[d.corner];
    if (d.corner == 0) s += " " + number(static_cast<double>(k.c[0]) / beat, 2) + " beats into the note";
    else if (d.corner == 1) s += ": " + number(static_cast<double>(k.c[1] - k.c[0]) / beat, 2) + " beats";
    else if (d.corner == 2) s += ": " + number(static_cast<double>(k.c[3] - k.c[2]) / beat, 2) + " beats";
    else s += k.c[3] >= pressed->length ? std::string(" with the note") : " " + number(static_cast<double>(k.c[3]) / beat, 2) + " beats into the note";
    ctx.setStatus(s);
  }
  markNotesDirty();
}

bool RollState::commitVibratoDrag(DragState& d) {
  if (!clip_ || d.vibrato.empty()) {
    curvePreview_.clear();
    return false;
  }
  std::vector<doc::ops::NoteVibratoEdit> edits;
  for (const DragState::VibratoTarget& t : d.vibrato) {
    const doc::Note* n = findNote(t.note);
    if (!n) continue;
    edits.push_back({doc::keyOf(*n), vibratoOf(t.note)});  // what the preview shows
  }
  curvePreview_.clear();
  const doc::ops::EditResult e = doc::ops::setNoteVibrato(*ctx.doc(), ref_, edits);
  std::vector<uint32_t> keep = sel_;
  const bool ok = apply(e, "Vibrato");
  select(std::move(keep));
  if (ok && d.kind == DragKind::Vibrato && d.vibDepth > 0.0) vibratoDepth_ = d.vibDepth;
  return ok;
}

bool RollState::removeVibrato(doc::NoteId note) {
  if (!clip_) return false;
  std::vector<doc::ops::NoteVibratoEdit> edits;
  for (const doc::Note& n : clip_->notes)
    if ((n.id == note || (selected(note) && selected(n.id))) && vibratoOf(n.id).set()) edits.push_back({doc::keyOf(n), doc::NoteVibrato{}});
  if (edits.empty()) return false;
  std::vector<uint32_t> keep = sel_;
  const bool ok = apply(doc::ops::setNoteVibrato(*ctx.doc(), ref_, edits), "Remove Vibrato");
  select(std::move(keep));
  if (ok) ctx.setStatus("Vibrato off");
  return ok;
}

bool RollState::setSelectionVibrato(double depthCents, double rateHz) {
  if (!clip_ || sel_.empty()) return false;
  std::vector<doc::ops::NoteVibratoEdit> edits;
  for (const doc::Note& n : clip_->notes) {
    if (!selected(n.id)) continue;
    doc::NoteVibrato v = vibratoOf(n.id);
    v.depthCents = static_cast<float>(depthCents);
    v.rate = static_cast<float>(rateHz);
    v.unit = doc::NoteVibrato::RateUnit::Hz;
    edits.push_back({doc::keyOf(n), depthCents > 0.0 ? v : doc::NoteVibrato{}});
  }
  std::vector<uint32_t> keep = sel_;
  const bool ok = apply(doc::ops::setNoteVibrato(*ctx.doc(), ref_, edits), "Vibrato");
  select(std::move(keep));
  if (ok && depthCents > 0.0) vibratoDepth_ = depthCents;
  return ok;
}

// --- paint ------------------------------------------------------------------------------------------------------------

void paintVibratoGrips(PaintContext& ctx, RollState& s, const IRect& vis) {
  if (!ctx.res.theme || !s.hasClip()) return;
  const DragState& d = s.drag;
  // Another gesture's preview (once it moved: a press alone repaints nothing).
  if (d.kind != DragKind::None && d.moved && d.kind != DragKind::Vibrato && d.kind != DragKind::VibratoCorner) return;
  static std::vector<const doc::Note*> notes;  // UI thread only; reused
  s.vibratoNotes(notes);
  if (notes.empty()) return;
  const Theme& th = *ctx.res.theme;
  const float sc = ctx.scale.factor;
  const float half = 0.5f * RollState::kVibratoGrip * sc;
  const float px = std::max(1.0f, std::round(sc));
  const Rect view{static_cast<float>(vis.x) - 2.0f * half, static_cast<float>(vis.y) - 2.0f * half, static_cast<float>(vis.right()) + 2.0f * half,
                  static_cast<float>(vis.bottom()) + 2.0f * half};
  auto inView = [&](Vec2 p) { return p.x >= view.x0 && p.x <= view.x1 && p.y >= view.y0 && p.y <= view.y1; };
  const Rgba ink = th.text.primary, dark = th.surface.panel, lit = th.accent.primary;
  for (const doc::Note* n : notes) {
    const doc::NoteVibrato v = s.vibratoOf(n->id);
    const bool held = d.kind != DragKind::None && d.pressed == n->id;
    if (v.set()) {
      // The amplitude envelope, both sides of the note, and its four corners (under the note).
      Vec2 up[4], dn[4];
      bool ok = true;
      for (int c = 0; c < 4 && ok; ++c) {
        const Corners k = cornersOf(v, n->length);
        const double off = static_cast<double>(k.c[c]);
        const std::optional<float> y = steadyY(s, *n, off);
        if (!y) {
          ok = false;
          break;
        }
        const float a = depthPx(s, *n, static_cast<double>(v.depthCents) * envelopeAt(v, n->length, off));
        const float x = s.xOfTick(static_cast<double>(n->start) + off);
        up[c] = {x, *y - a};
        dn[c] = {x, *y + a};
      }
      if (ok && up[3].x >= view.x0 && up[0].x <= view.x1) {
        smoothPolyline(ctx.dl, std::span<const Vec2>(up, 4), withAlpha(ink, 150), px);
        smoothPolyline(ctx.dl, std::span<const Vec2>(dn, 4), withAlpha(ink, 150), px);
        for (int c = 0; c < 4; ++c) {
          const std::optional<Vec2> k = s.vibratoCornerPos(*n, c);
          if (!k || !inView(*k)) continue;
          if (std::fabs(k->y - dn[c].y) > 1.0f) ctx.dl.addRectFilled({k->x - 0.5f * px, dn[c].y, k->x + 0.5f * px, k->y}, withAlpha(ink, 150));
          const bool grabbed = held && d.kind == DragKind::VibratoCorner && d.corner == c;
          const float r = (grabbed ? 0.5f : 0.42f) * RollState::kVibratoGrip * sc;
          ctx.dl.addRectFilled({k->x - r, k->y - r, k->x + r, k->y + r}, dark);
          ctx.dl.addRectFilled({k->x - r + px, k->y - r + px, k->x + r - px, k->y + r - px}, grabbed ? ink : lit);
        }
      }
    }
    // The ~ grip: a rounded tab with a small wave; filled while the note vibrates.
    const std::optional<Vec2> g = s.vibratoGripPos(*n);
    if (!g || !inView(*g)) continue;
    const bool grabbed = held && d.kind == DragKind::Vibrato;
    const Rect tab{g->x - half, g->y - half, g->x + half, g->y + half};
    const bool on = v.set();
    ctx.dl.addRoundedRectFilled(tab, 3.0f * sc, grabbed ? ink : on ? lit : dark);
    if (!(on || grabbed)) ctx.dl.addRoundedRectStroke(tab, 3.0f * sc, withAlpha(ink, 190), px);
    Vec2 wave[9];
    for (int k = 0; k < 9; ++k) {
      const float u = static_cast<float>(k) / 8.0f;
      wave[k] = {g->x - 0.62f * half + 1.24f * half * u, g->y - 0.38f * half * std::sin(2.0f * std::numbers::pi_v<float> * u)};
    }
    smoothPolyline(ctx.dl, std::span<const Vec2>(wave, 9), on || grabbed ? dark : ink, std::max(1.0f, 1.25f * sc));
  }
}

}  // namespace pq::views::roll
