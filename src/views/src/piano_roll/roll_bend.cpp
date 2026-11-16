// bug-bend-shape piano roll: the shape node of a bend (the user: "all 4 notes compress into a single note really
// quickly over an extremely short period of time and theres no way to lengthen the section where it compresses, or
// change the shape of it. basically i want the inflection point of these curves to have a lil node that when you
// click and drag down, it makes it steeper, and if you drag up itll make it curve over a larger period").
//
// A bend is doc::NoteBend on the note's curve record (the neighbour's id, a length, a shape); doc::effectiveNoteCurve
// evaluates it wherever the curve is read, so what the node shapes is what plays. On every bend of the hovered note
// and of the selected notes, a node sits on the drawn path where the glide TURNS (doc::glideShapeTurn: half way
// between the two pitches; the inflection of the S):
//   up / down     the glide's length, ×2 per kBendNodePxPerOctave px: down = steeper (shorter), up = gentler
//                 (longer). Snapped to the grid's musical lengths (snapBendLength; Shift: free), within [1 tick, the
//                 note's length less its other bend]. A bend out keeps its end on the note's end, a bend in its start
//                 on the note's start (the model's anchoring), so the node travels with the glide's middle.
//   sideways      past kBendNodeDeadZone px: where in the glide it turns (doc::glideShapeTurningAt: an S that leaves
//                 and arrives flat, monotonic, no overshoot), the node following the pointer through the glide.
//   double-click  the default S-curve (the length stays).
//   status line   "Bend Out 0.5 beats · turns at 30%" while dragging.
// With the note in a selection the drag shapes the same edge's bend of every selected note that has one: each
// length by the same factor (then snapped), one shape once the drag went sideways (else each keeps its own). The drag
// previews through curvePreview_ and, once past the click slop, dispatches ops::shapeNoteBends live every frame
// (bug-live-shape, roll_live.cpp: a sounding note follows); the release commits it as one undo step.
// Separation: without Alt a node is grabbed before the note's edge (a short glide's node sits near the edge); Alt +
// the edge is the Alt bend as before.
//
// Cost: hit-testing and paint ask each shown note (≤ kMaxBendNodeNotes selected + the hovered one) for one point of
// its cached path (RollState::pathCents) per bend; a drag frame rewrites one record per shaped note and the roll
// re-flattens those notes once (pathOf's preview entries).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "pq/document/commands.h"
#include "pq/document/note_bend.h"
#include "pq/ui/theme.h"
#include "roll_widgets.h"

namespace pq::views::roll {

using namespace pq::ui;

namespace {

float scaleOf(const RollState& s) noexcept { return (s.noteArea && s.noteArea->tree()) ? s.noteArea->tree()->scale() : 1.0f; }

const doc::NoteBend& bendOf(const doc::NoteCurves& c, int edge) { return edge < 0 ? c.bendIn : c.bendOut; }
doc::NoteBend& bendOf(doc::NoteCurves& c, int edge) { return edge < 0 ? c.bendIn : c.bendOut; }

std::string number(double v, int decimals) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
  std::string s = buf;
  if (s.find('.') != std::string::npos) {
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
  }
  return s.empty() ? "0" : s;
}

// The effective length of a note's bend (clamped to the note, as the curve evaluates it).
Ticks effectiveLength(const doc::NoteBend& b, const doc::Note& n) { return std::clamp<Ticks>(b.length, 1, std::max<Ticks>(n.length, 1)); }

}  // namespace

Ticks RollState::snapBendLength(double length, Ticks step) {
  if (!(length > 0.0) || !std::isfinite(length)) return 1;
  if (step <= 0) return std::max<Ticks>(1, static_cast<Ticks>(std::llround(length)));
  const double s = static_cast<double>(step);
  if (length >= s) return static_cast<Ticks>(std::llround(length / s)) * step;
  // Below a step: the step halved k times, the k nearest in octaves (so the length moves by halves, as the drag does).
  const double k = std::round(std::log2(s / length));
  return std::max<Ticks>(1, static_cast<Ticks>(std::llround(s / std::exp2(k))));
}

std::string RollState::bendShapeText(int edge, Ticks length, double turn) {
  std::string s = edge < 0 ? "Bend In " : "Bend Out ";
  s += number(static_cast<double>(length) / static_cast<double>(kPpq), 3) + (length == kPpq ? " beat" : " beats");
  if (turn >= 0.0 && std::fabs(turn - 0.5) > 0.005) s += " \xC2\xB7 turns at " + number(std::round(turn * 100.0), 0) + "%";
  s += " \xC2\xB7 down: steeper, up: gentler, sideways: where it turns";
  return s;
}

void RollState::bendNodeNotes(std::vector<const doc::Note*>& out) const {
  out.clear();
  if (!clip_ || (!clip_->curves && curvePreview_.empty())) return;
  auto add = [&](uint32_t id) {
    if (id == 0) return;
    const doc::Note* n = findNote(doc::NoteId{id});
    if (!n) return;
    const doc::NoteCurves* rec = curveRecord(n->id);
    if (rec && (rec->bendIn.set() || rec->bendOut.set())) out.push_back(n);
  };
  if (sel_.size() <= kMaxBendNodeNotes) {
    cacheSelectedNotes();
    for (const uint32_t id : sel_) add(id);
  }
  add(hoverPart_.note);
  for (const DragState::BendTarget& t : drag.bends) add(t.note.value);
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
}

std::optional<ui::Vec2> RollState::bendNodePos(const doc::Note& n, int edge) const {
  if (!clip_ || rows_.empty()) return std::nullopt;
  const doc::NoteCurves* rec = curveRecord(n.id);
  if (!rec) return std::nullopt;
  const doc::NoteBend& b = bendOf(*rec, edge);
  if (!b.set() || !findNote(b.other)) return std::nullopt;  // a bend whose neighbour is gone does nothing
  const Ticks L = std::max<Ticks>(n.length, 1), len = effectiveLength(b, n);
  const double t = static_cast<double>(edge < 0 ? 0 : L - len) + doc::glideShapeTurn(b.shape) * static_cast<double>(len);
  const std::optional<double> c = pathCents(n, n.start, n.pitch, t);
  if (!c) return std::nullopt;
  return ui::Vec2{xOfTick(static_cast<double>(n.start) + t), yOfCents(*c)};
}

RollState::BendNodeHit RollState::bendNodeAt(ui::Vec2 p) const {
  BendNodeHit hit;
  if (!clip_ || (!clip_->curves && curvePreview_.empty())) return hit;
  static std::vector<const doc::Note*> notes;  // UI thread only; reused
  bendNodeNotes(notes);
  const float half = 0.5f * kBendNode * scaleOf(*this);
  float best = 1e30f;
  for (const doc::Note* n : notes)
    for (const int edge : {-1, 1}) {
      const std::optional<ui::Vec2> g = bendNodePos(*n, edge);
      if (!g || std::fabs(p.x - g->x) > half || std::fabs(p.y - g->y) > half) continue;
      const float d = (p.x - g->x) * (p.x - g->x) + (p.y - g->y) * (p.y - g->y);
      if (d < best) {
        best = d;
        hit = {n->id, static_cast<int8_t>(edge)};
      }
    }
  return hit;
}

RollState::BendNodeHit RollState::bendNodeOver(ui::Vec2 p, ui::Mods mods) const {
  if (mods & mod::kAlt) return {};
  const BendNodeHit hit = bendNodeAt(p);
  if (!hit.valid()) return hit;
  const VibratoHit vh = vibratoPartAt(p);
  if (!vh.valid()) return hit;
  const doc::Note* n = findNote(hit.note);
  const doc::Note* v = findNote(vh.note);
  const std::optional<ui::Vec2> a = n ? bendNodePos(*n, hit.edge) : std::nullopt;
  const std::optional<ui::Vec2> b = !v ? std::nullopt : vh.part == VibratoPart::Grip ? vibratoGripPos(*v) : vibratoCornerPos(*v, vh.corner);
  if (!a || !b) return a ? hit : BendNodeHit{};
  auto d2 = [&](ui::Vec2 q) { return (p.x - q.x) * (p.x - q.x) + (p.y - q.y) * (p.y - q.y); };
  return d2(*a) <= d2(*b) ? hit : BendNodeHit{};
}

void RollState::beginBendShape(const BendNodeHit& h, ui::Vec2 p) {
  drag = {};
  const doc::Note* pressed = clip_ && h.valid() ? findNote(h.note) : nullptr;
  if (!pressed) return;
  DragState& d = drag;
  d.kind = DragKind::BendShape;
  d.pressed = h.note;
  d.press = d.last = p;
  d.bendEdge = h.edge;
  // The pressed note, and with it the selection when it is part of one (those with a bend on this edge).
  auto add = [&](const doc::Note& n) {
    const doc::NoteCurves* rec = curveRecord(n.id);
    if (!rec || !bendOf(*rec, h.edge).set()) return;
    const doc::NoteBend& other = bendOf(*rec, -h.edge);
    const Ticks room = n.length - (other.set() ? std::min(other.length, n.length) : 0);
    d.bends.push_back({n.id, bendOf(*rec, h.edge), std::max<Ticks>(room, 1)});
  };
  if (selected(h.note)) {
    for (const doc::Note& n : clip_->notes)
      if (selected(n.id)) add(n);
  } else {
    add(*pressed);
  }
  std::sort(d.bends.begin(), d.bends.end(), [](const DragState::BendTarget& a, const DragState::BendTarget& b) { return a.note < b.note; });
  const auto it = std::find_if(d.bends.begin(), d.bends.end(), [&](const DragState::BendTarget& t) { return t.note == h.note; });
  if (it == d.bends.end()) {
    drag = {};
    return;
  }
  d.turn0 = doc::glideShapeTurn(it->base.shape);
}

void RollState::bendShapeTo(ui::Vec2 p, ui::Mods mods) {
  DragState& d = drag;
  if (!clip_ || d.bends.empty()) return;
  const float sc = scaleOf(*this);
  // Up / down: the length, by octaves (down: shorter = steeper).
  const double factor = std::exp2(-static_cast<double>(p.y - d.press.y) / static_cast<double>(kBendNodePxPerOctave * sc));
  const bool free = (mods & mod::kShift) != 0;
  const Grid g = grid();
  auto stepAt = [&](const doc::Note& n) -> Ticks {
    if (free || !g.snapping()) return 0;
    const TickRange cell = g.cellAt(d.bendEdge < 0 ? n.start : std::max<Ticks>(n.start + n.length - 1, 0));
    return std::max<Ticks>(1, cell.end - cell.start);
  };
  auto lengthOf = [&](const DragState::BendTarget& t, const doc::Note& n) {
    return std::clamp<Ticks>(snapBendLength(static_cast<double>(effectiveLength(t.base, n)) * factor, stepAt(n)), 1, t.room);
  };
  // Sideways: the turn, through the pressed note's glide at its new length (past a dead zone, so an up / down drag
  // leaves the shapes alone).
  const doc::Note* pressed = findNote(d.pressed);
  const auto pt = std::find_if(d.bends.begin(), d.bends.end(), [&](const DragState::BendTarget& t) { return t.note == d.pressed; });
  if (!pressed || pt == d.bends.end()) return;
  const Ticks pressedLength = lengthOf(*pt, *pressed);
  const float dead = kBendNodeDeadZone * sc;
  const float dx = p.x - d.press.x;
  d.turned = std::fabs(dx) > dead;
  double turn = d.turn0;
  if (d.turned) {
    const double widthPx = std::max(static_cast<double>(pressedLength) * pxPerTick_, 1.0);
    turn = std::clamp(d.turn0 + static_cast<double>(dx - std::copysign(dead, dx)) / widthPx, doc::kMinGlideTurn, 1.0 - doc::kMinGlideTurn);
  }
  const doc::GlideShape shape = doc::glideShapeTurningAt(turn);
  for (const DragState::BendTarget& t : d.bends) {
    const doc::Note* n = findNote(t.note);
    if (!n) continue;
    doc::NoteBend& b = bendOf(previewRecord(t.note), d.bendEdge);
    b = t.base;
    b.length = lengthOf(t, *n);
    if (d.turned) b.shape = shape;
  }
  ctx.setStatus(bendShapeText(d.bendEdge, pressedLength, d.turned ? turn : doc::glideShapeTurn(pt->base.shape)));
  markNotesDirty();
}

doc::ops::EditResult RollState::bendShapeEdit(const DragState& d) const {
  std::vector<doc::ops::NoteBendShapeEdit> edits;
  const doc::ops::NoteEdge edge = d.bendEdge < 0 ? doc::ops::NoteEdge::Start : doc::ops::NoteEdge::End;
  for (const DragState::BendTarget& t : d.bends) {
    const doc::Note* n = findNote(t.note);
    const doc::NoteCurves* rec = curveRecord(t.note);  // what the preview shows
    if (!n || !rec || !bendOf(*rec, d.bendEdge).set()) continue;
    const doc::NoteBend& b = bendOf(*rec, d.bendEdge);
    edits.push_back({doc::keyOf(*n), edge, b.length, b.shape});
  }
  return doc::ops::shapeNoteBends(*snap_, ref_, edits);  // (a record as it is adds nothing)
}

bool RollState::resetBendShape(const BendNodeHit& h) {
  if (!clip_ || !h.valid()) return false;
  std::vector<doc::ops::NoteBendShapeEdit> edits;
  const doc::ops::NoteEdge edge = h.edge < 0 ? doc::ops::NoteEdge::Start : doc::ops::NoteEdge::End;
  for (const doc::Note& n : clip_->notes) {
    if (!(n.id == h.note || (selected(h.note) && selected(n.id)))) continue;
    const doc::NoteCurves* rec = curveRecord(n.id);
    if (!rec || !bendOf(*rec, h.edge).set() || bendOf(*rec, h.edge).shape == doc::GlideShape{}) continue;
    edits.push_back({doc::keyOf(n), edge, bendOf(*rec, h.edge).length, doc::GlideShape{}});
  }
  if (edits.empty()) {
    ctx.setStatus(std::string(h.edge < 0 ? "Bend In" : "Bend Out") + " is the S-curve already: drag its node to shape it");
    return false;
  }
  std::vector<uint32_t> keep = sel_;
  const bool ok = apply(doc::ops::shapeNoteBends(*ctx.doc(), ref_, edits), "Bend Shape");
  select(std::move(keep));
  if (ok) ctx.setStatus(std::string(h.edge < 0 ? "Bend In" : "Bend Out") + ": S-curve");
  return ok;
}

// --- paint ------------------------------------------------------------------------------------------------------------

void paintBendNodes(PaintContext& ctx, RollState& s, const IRect& vis) {
  if (!ctx.res.theme || !s.hasClip()) return;
  const DragState& d = s.drag;
  // Another gesture's preview (once it moved: a press alone repaints nothing).
  if (d.kind != DragKind::None && d.moved && d.kind != DragKind::BendShape) return;
  static std::vector<const doc::Note*> notes;  // UI thread only; reused
  s.bendNodeNotes(notes);
  if (notes.empty()) return;
  const Theme& th = *ctx.res.theme;
  const float sc = ctx.scale.factor;
  const float m = RollState::kBendNode * sc;
  const float x0 = static_cast<float>(vis.x) - m, x1 = static_cast<float>(vis.right()) + m;
  const float y0 = static_cast<float>(vis.y) - m, y1 = static_cast<float>(vis.bottom()) + m;
  const Rgba ring = s.colours().selBorder;
  for (const doc::Note* n : notes)
    for (const int edge : {-1, 1}) {
      const std::optional<Vec2> g = s.bendNodePos(*n, edge);
      if (!g || g->x < x0 || g->x > x1 || g->y < y0 || g->y > y1) continue;
      // A ring like a breakpoint's with an accent core: it shapes the glide, it is not a point of the curve.
      const bool held = d.kind == DragKind::BendShape && d.bendEdge == edge &&
                        std::any_of(d.bends.begin(), d.bends.end(), [&](const DragState::BendTarget& t) { return t.note == n->id; });
      ctx.dl.addCircleFilled(*g, (held ? 5.5f : 4.5f) * sc, ring);
      ctx.dl.addCircleFilled(*g, (held ? 3.5f : 2.75f) * sc, held ? th.text.primary : th.accent.primary);
    }
}

}  // namespace pq::views::roll
