// T7.6 piano roll: bends between notes, pen modes, breakpoints, vibrato, ride curves (SPEC §3.1, §5; docs/plans/M7.md
// "T7.6" + amendment A3). The RollState half lives here; roll_input.cpp routes the presses, roll_state.cpp commits.
//
// What is stored is always baked breakpoints: a note's NoteCurves::pitch (cents added to the note's pitch, ticks from
// its start) or a JI object's ride (cents added to every voice, object ticks). The lowering plays exactly those, so
// what is drawn is what sounds. Nothing else is stored: a bend's lead-in / lead-out and a vibrato are recognised by
// their form (roll_state.h "bends, pen modes …"), which is why a bend can be re-dragged, reshaped or removed and a
// vibrato re-typed after a reload, a copy or an undo.
//
//   Alt + a note's left / right edge   the note glides in from the previous note / out to the next one over the
//                                      dragged length (S-curve; "Bend In / Out ▸ Straight" in the note's menu); back to
//                                      the edge removes it. Every selected note bends, each to its own neighbour.
//   Ctrl+G                             each selected note glides out into the next selected one over one grid step.
//   Draw Mode (B) on a note            drag: the stroke is the note's pitch curve from there on (thinned to 0.5 ¢, at
//                                      most kMaxPenPoints points); Alt + drag: vibrato over the dragged span (depth =
//                                      the stroke's height, a flat stroke repeats the last depth; rate from the grid);
//                                      Ctrl + click: a breakpoint (drag it on); a click still deletes the note.
//   the same on a JI object's lowest voice (not entered): its ride curve.
//   breakpoints                        shown on the selected notes and the active object's ride: drag (rows / harmonic
//                                      guides / the note's own pitch; Shift: free), double-click deletes, the diamond
//                                      between two points bends the segment.
//   Shift + a note's body              free detune: the selected notes become Cents notes that far away.
// Every gesture previews the curve its release stores (curvePreview_ / jiPreviewObject_) and is one undo step.
//
// Cost: nothing here runs per audio block. A drag frame re-thins one stroke (O(points · log)) or rebuilds one bend
// per selected note; the neighbour search of a bend runs once per gesture (two passes over the clip's notes per
// selected note). Paint adds the handles of at most 64 selected notes, each of at most kMaxCurveHandles points.

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <string>
#include <variant>

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

using Pts = std::vector<doc::Breakpoint>;
using Span = std::span<const doc::Breakpoint>;
constexpr size_t kNone = static_cast<size_t>(-1);
constexpr double kThinCents = 0.5;  // a stroke / a sampled shape stays this close to what was drawn

Error invalid(std::string message) { return Error{ErrorCode::InvalidArgument, std::move(message), {}}; }

float scaleOf(const RollState& s) noexcept { return (s.noteArea && s.noteArea->tree()) ? s.noteArea->tree()->scale() : 1.0f; }

doc::Breakpoint bp(Ticks t, double v, float curve = 0.0f) {
  return {t, static_cast<float>(std::clamp(v, -static_cast<double>(doc::kMaxCurveCents), static_cast<double>(doc::kMaxCurveCents))), curve, {}};
}
double valueAt(Span p, Ticks t) { return p.empty() ? 0.0 : doc::automationValueAt(p, t); }
double valueBefore(Span p, Ticks t) { return p.empty() ? 0.0 : doc::automationValueBefore(p, t); }

// Appends, skipping an exact repeat of the last point.
void push(Pts& out, const doc::Breakpoint& b) {
  if (!out.empty() && out.back().tick == b.tick && out.back().value == b.value) return;
  out.push_back(b);
}

// A curve that is 0 everywhere is no curve.
void normalise(Pts& p) {
  if (std::all_of(p.begin(), p.end(), [](const doc::Breakpoint& b) { return b.value == 0.0f; })) p.clear();
}

// The index of the lead-in's last point (its first point of value 0); 0: no lead-in.
size_t leadInEnd(Span p) {
  if (p.size() < 2 || p[0].tick != 0 || p[0].value == 0.0f) return 0;
  for (size_t i = 1; i < p.size(); ++i)
    if (p[i].value == 0.0f) return i;
  return 0;
}
// The index of the lead-out's first point (the curve's last point of value 0); kNone: no lead-out.
size_t leadOutBegin(Span p) {
  if (p.size() < 2 || p.back().value == 0.0f) return kNone;
  for (size_t i = p.size() - 1; i-- > 0;)
    if (p[i].value == 0.0f) return i;
  return kNone;
}

// `old` with [ta, te] replaced by `fresh` (ticks inside it, ascending). What lies before arrives where it did, and
// (keepTail) what lies after leaves from where it did: a point is added on the region's border where the curve would
// otherwise change outside it. !keepTail: the curve ends with `fresh` (its last value is held).
Pts replaceRegion(Span old, Ticks ta, Ticks te, const Pts& fresh, bool keepTail) {
  Pts out;
  size_t i = 0;
  for (; i < old.size() && old[i].tick < ta; ++i) out.push_back(old[i]);
  const bool head = !out.empty();
  const double va = valueAt(old, te);
  const double vb = ta > 0 ? valueBefore(old, ta) : (fresh.empty() ? va : static_cast<double>(fresh.front().value));
  size_t j = old.size();
  while (j > 0 && old[j - 1].tick > te) --j;
  const bool tail = keepTail && j < old.size();
  const Ticks f0 = fresh.empty() ? te : fresh.front().tick;
  const double fv0 = fresh.empty() ? va : static_cast<double>(fresh.front().value);
  if (ta > 0 && (head ? (f0 > ta || fv0 != vb) : fv0 != vb)) push(out, bp(ta, vb));
  for (const doc::Breakpoint& b : fresh) push(out, b);
  if (keepTail) {
    const Ticks f1 = fresh.empty() ? ta : fresh.back().tick;
    const double fv1 = fresh.empty() ? vb : static_cast<double>(fresh.back().value);
    if (tail ? (f1 < te || fv1 != va) : fv1 != va) push(out, bp(te, va));
    for (; j < old.size(); ++j) out.push_back(old[j]);
  }
  normalise(out);
  return out;
}

// A glide from `from` to `to` cents over [t0, t0 + length]: two points when straight, else the shape sampled and
// thinned to kThinCents. The end values are exact.
Pts glidePoints(Ticks t0, Ticks length, double from, double to, doc::JiShape::Kind kind) {
  if (length <= 0) return {};
  const int n = static_cast<int>(std::min<Ticks>(length, 48));
  if (kind == doc::JiShape::LogLinear || n < 3) return {bp(t0, from), bp(t0 + length, to)};
  doc::JiShape shape;
  shape.kind = kind;
  Pts raw;
  raw.reserve(static_cast<size_t>(n) + 1);
  for (int k = 0; k <= n; ++k)
    raw.push_back(bp(t0 + length * k / n, from + (to - from) * doc::jiShapeValue(shape, static_cast<double>(k) / n)));
  raw.front() = bp(t0, from);
  raw.back() = bp(t0 + length, to);
  return doc::ops::thinAutomation(raw, kThinCents);
}

// `old` (a note `length` long) with its lead-in (edge < 0) / lead-out replaced by a glide of `bend` ticks from /
// to `cents`; bend 0 removes it.
Pts withBend(Span old, Ticks length, int edge, Ticks bend, double cents, doc::JiShape::Kind kind) {
  if (edge < 0) {
    const size_t e = leadInEnd(old);
    const Ticks was = e ? old[e].tick : 0;
    return replaceRegion(old, 0, std::max(was, bend), glidePoints(0, bend, cents, 0.0, kind), true);
  }
  const size_t b = leadOutBegin(old);
  const Ticks was = b != kNone ? old[b].tick : length;
  return replaceRegion(old, std::min(was, length - bend), length, glidePoints(length - bend, bend, 0.0, cents, kind), false);
}

// A sine of `depth` cents from `ta`, whole half cycles ending at or before `tb`, sampled so the polyline stays within
// kThinCents of it (fewer points per cycle when that would pass `maxPoints`). Starts and ends on exact zeros, its
// swings on exact ±depth. Empty: under one cycle fits, or the period is too short to sample.
Pts vibratoPoints(Ticks ta, Ticks tb, double depth, double periodTicks, size_t maxPoints) {
  if (!(depth > 0.0) || !(periodTicks >= 16.0) || tb <= ta) return {};
  const int halves = static_cast<int>(std::floor(static_cast<double>(tb - ta) / (0.5 * periodTicks) + 1e-9));
  if (halves < 2) return {};
  // 1 − cos(π / per) ≤ kThinCents / depth, per a multiple of 4 (so the zeros and the swings are sampled).
  const double need = depth <= kThinCents ? 8.0 : std::numbers::pi / std::acos(1.0 - kThinCents / depth);
  int per = std::clamp(4 * static_cast<int>(std::ceil(need / 4.0)), 8, 64);
  while (per > 8 && (static_cast<size_t>(halves) * static_cast<size_t>(per) / 2 + 1 > maxPoints || periodTicks < 2.0 * per)) per -= 4;
  const int total = halves * per / 2;
  if (static_cast<size_t>(total) + 1 > maxPoints) return {};
  Pts out;
  out.reserve(static_cast<size_t>(total) + 1);
  for (int k = 0; k <= total; ++k) {
    const int phase = k % per;
    double v = depth * std::sin(2.0 * std::numbers::pi * phase / per);
    if (phase % (per / 2) == 0) v = 0.0;
    else if (phase == per / 4) v = depth;
    else if (phase == 3 * per / 4) v = -depth;
    out.push_back(bp(ta + static_cast<Ticks>(std::llround(k * periodTicks / per)), v));
  }
  return out;
}

std::string number(double v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.1f", v);
  std::string s = buf;
  while (!s.empty() && s.back() == '0') s.pop_back();
  if (!s.empty() && s.back() == '.') s.pop_back();
  return s.empty() ? "0" : s;
}

doc::ops::EditResult batchOf(std::string label, std::vector<doc::Command> commands, doc::ClipRef clip, std::vector<doc::NoteKey> keys) {
  doc::ops::Edit e;
  e.command = doc::Command{doc::Batch{std::move(label), std::move(commands)}};
  e.noteClip = clip;
  e.notes = std::move(keys);
  return e;
}

}  // namespace

// --- the curves -------------------------------------------------------------------------------------------------------

std::span<const doc::Breakpoint> RollState::curvePoints(const CurveTarget& t) const {
  if (!clip_) return {};
  if (t.note.valid()) {
    if (!curvePreview_.empty()) {
      const auto it = std::lower_bound(curvePreview_.begin(), curvePreview_.end(), t.note,
                                       [](const doc::NoteCurves& c, doc::NoteId id) { return c.note < id; });
      if (it != curvePreview_.end() && it->note == t.note) return it->pitch;
    }
    if (clip_->curves)
      if (const doc::NoteCurves* nc = doc::findNoteCurves(*clip_, t.note)) return nc->pitch;
    return {};
  }
  if (t.ji.valid()) {
    if (jiPreviewObject_ && jiPreviewObject_->id == t.ji) return jiPreviewObject_->ride;
    if (const doc::JiObject* o = doc::findJiObject(*clip_, t.ji)) return o->ride;
  }
  return {};
}

bool RollState::curveSpan(const CurveTarget& t, Ticks& start, Ticks& length) const {
  if (!clip_) return false;
  if (t.note.valid()) {
    const doc::Note* n = findNote(t.note);
    if (!n) return false;
    start = n->start;
    length = n->length;
    return true;
  }
  const doc::JiObject* o = t.ji.valid() ? doc::findJiObject(*clip_, t.ji) : nullptr;
  if (!o) return false;
  start = o->start;
  length = o->length;
  return true;
}

std::optional<double> RollState::curveBase(const CurveTarget& t, double offset) const {
  if (!clip_) return std::nullopt;
  if (t.note.valid()) {
    const doc::Note* n = findNote(t.note);
    return n ? pathBaseCents(n->start, n->pitch, offset) : std::nullopt;
  }
  const doc::JiObject* o = t.ji.valid() ? doc::findJiObject(*clip_, t.ji) : nullptr;
  if (!o) return std::nullopt;
  const double at = std::clamp(offset, 0.0, static_cast<double>(o->length));
  const std::optional<double> c = doc::jiVoiceCents(*o, tuning_, 0, at);
  if (!c) return std::nullopt;
  return *c - (o->ride.empty() ? 0.0 : pq::evalAutomation(std::span<const doc::Breakpoint>(o->ride), at, 0.0));
}

double RollState::centsAtScreenY(float y) const noexcept {
  if (rows_.empty()) return 0.0;
  return rows_.centsAtY(static_cast<double>(y) - static_cast<double>(area().y - scrollY_));
}

double RollState::ticksPerSecond(Ticks contentTick) const {
  if (clip_ && snap_ && snap_->timeline) {
    const Ticks at = clip_->start - clip_->contentStart + contentTick;
    const TempoMap& tm = snap_->timeline->tempoMap;
    const double tps = tm.secondsToTicks(tm.ticksToSeconds(at) + 1.0) - static_cast<double>(at);
    if (tps > 0.0) return tps;
  }
  return 2.0 * static_cast<double>(kPpq);  // 120 bpm
}

void RollState::setCurvePreview(const CurveTarget& t, const std::vector<doc::Breakpoint>& pts) {
  if (t.note.valid()) {
    const auto it = std::lower_bound(curvePreview_.begin(), curvePreview_.end(), t.note,
                                     [](const doc::NoteCurves& c, doc::NoteId id) { return c.note < id; });
    if (it != curvePreview_.end() && it->note == t.note) it->pitch = pts;
    else curvePreview_.insert(it, doc::NoteCurves{t.note, pts});
  } else if (const doc::JiObject* o = t.ji.valid() && clip_ ? doc::findJiObject(*clip_, t.ji) : nullptr) {
    doc::JiObject copy = *o;
    copy.ride = pts;
    jiPreview_ = makeJiView(copy);
    jiPreviewObject_ = std::move(copy);
  }
  markNotesDirty();
}

void RollState::clearCurvePreview() { curvePreview_.clear(); }

bool RollState::commitCurve(const CurveTarget& t, std::vector<doc::Breakpoint> pts, std::string_view what) {
  if (!clip_) return false;
  normalise(pts);
  if (t.note.valid()) {
    const doc::Note* n = findNote(t.note);
    if (!n) return false;
    const doc::NoteCurves* have = doc::findNoteCurves(*clip_, t.note);
    if (have ? have->pitch == pts : pts.empty()) return false;  // nothing changed: no undo step
    std::vector<uint32_t> keep = sel_;
    const bool ok = apply(doc::ops::setNoteCurve(*ctx.doc(), ref_, doc::keyOf(*n), std::move(pts)), what);
    if (ok) select(std::move(keep));
    return ok;
  }
  const doc::JiObject* o = t.ji.valid() ? doc::findJiObject(*clip_, t.ji) : nullptr;
  if (!o || o->ride == pts) return false;
  doc::JiObject copy = *o;
  copy.ride = std::move(pts);
  const int32_t stop = jiStopSel_, term = jiTermSel_;
  const bool ok = applyJi(doc::ops::setJiObject(*ctx.doc(), ctx.store().ids(), ref_, std::move(copy)), what, jiSel_.valid() ? jiSel_ : t.ji);
  if (ok) {
    jiStopSel_ = stop;
    jiTermSel_ = term;
  }
  return ok;
}

// --- handles ----------------------------------------------------------------------------------------------------------

void RollState::shownCurves(std::vector<CurveTarget>& out) const {
  out.clear();
  if (!clip_) return;
  auto add = [&](const CurveTarget& t) {
    if (std::find(out.begin(), out.end(), t) == out.end() && !curvePoints(t).empty()) out.push_back(t);
  };
  if ((clip_->curves || !curvePreview_.empty()) && sel_.size() <= 64)
    for (const uint32_t id : sel_) add(CurveTarget{doc::NoteId{id}, {}});
  if (ctx.drawMode() && hoverPart_.note != 0 && drag.kind == DragKind::None) add(CurveTarget{doc::NoteId{hoverPart_.note}, {}});
  if (drag.curve.valid()) add(drag.curve);
  if (const doc::JiObjectId a = jiActive(); a.valid()) add(CurveTarget{{}, a});
}

void RollState::curveHandles(const CurveTarget& t, std::vector<CurveHandle>& out) const {
  out.clear();
  const std::span<const doc::Breakpoint> pts = curvePoints(t);
  Ticks start = 0, length = 0;
  if (pts.empty() || pts.size() > kMaxCurveHandles || !curveSpan(t, start, length)) return;
  const float sc = scaleOf(*this);
  for (size_t i = 0; i < pts.size(); ++i) {
    const std::optional<double> base = curveBase(t, static_cast<double>(pts[i].tick));
    if (!base) {
      out.clear();
      return;
    }
    const int8_t end = !t.note.valid() ? 0 : pts[i].tick <= 0 ? -1 : pts[i].tick >= length ? 1 : 0;
    out.push_back({static_cast<int32_t>(i), false, {xOfTick(static_cast<double>(start + pts[i].tick)), yOfCents(*base + pts[i].value)}, end});
  }
  const size_t n = pts.size();
  for (size_t i = 0; i + 1 < n; ++i) {
    const doc::Breakpoint& a = pts[i];
    const doc::Breakpoint& b = pts[i + 1];
    if (b.tick <= a.tick || a.value == b.value) continue;
    if (out[i + 1].pos.x - out[i].pos.x < 28.0f * sc) continue;  // too narrow to grab between its points
    const double tm = 0.5 * static_cast<double>(a.tick + b.tick);
    const std::optional<double> base = curveBase(t, tm);
    if (!base) continue;
    out.push_back({static_cast<int32_t>(i), true, {xOfTick(static_cast<double>(start) + tm), yOfCents(*base + pq::automationSegmentValue(a, b, tm))}, 0});
  }
}

RollState::CurveHit RollState::curveHandleAt(ui::Vec2 p) const {
  CurveHit hit;
  if (!clip_ || (!clip_->curves && !clip_->ji)) return hit;
  const float sc = scaleOf(*this);
  shownCurves(targetScratch_);
  float best = 1e30f;
  for (const CurveTarget& t : targetScratch_) {
    curveHandles(t, handleScratch_);
    for (const CurveHandle& h : handleScratch_) {
      const float r = (h.mid ? 5.0f : 6.0f) * sc;
      const float dx = p.x - h.pos.x, dy = p.y - h.pos.y;
      const float left = h.end > 0 ? 2.0f * r : r, right = h.end < 0 ? 2.0f * r : r;  // further into the note at its ends
      if (dx < -left || dx > right || std::fabs(dy) > r) continue;
      const float d = dx * dx + dy * dy + (h.mid ? 4.0f * sc * sc : 0.0f);  // a point wins over a diamond next to it
      if (d < best) {
        best = d;
        hit = {t, h.index, h.mid};
      }
    }
  }
  return hit;
}

doc::JiObjectId RollState::rideLaneAt(ui::Vec2 p) const {
  if (!clip_ || !clip_->ji || jiEntered_.valid()) return {};
  const float half = pathHalfThickness() + 1.0f;
  for (size_t i = jiViews_.size(); i-- > 0;) {
    const JiView& v = jiViews_[i];
    if (!v.ok) continue;
    const double t = tickAtX(p.x) - static_cast<double>(v.start);
    if (t < 0.0 || t > static_cast<double>(v.length)) continue;
    const doc::JiObject* o = doc::findJiObject(*clip_, v.id);
    if (!o) continue;
    const std::optional<double> c = doc::jiVoiceCents(*o, tuning_, 0, t);
    if (c && std::fabs(yOfCents(*c) - p.y) <= half) return v.id;
  }
  return {};
}

double RollState::snapCurveCents(double cents, double own) const {
  double best = own;
  auto offer = [&](double c) {
    if (std::fabs(c - cents) < std::fabs(best - cents)) best = c;
  };
  if (!rows_.empty())
    if (const PitchRow* row = rows_.rowOfDegree(rows_.placeCents(cents).nearestDegree)) offer(row->cents);
  if (harmonics_)
    if (const std::optional<Harmonics> h = harmonics()) {
      const double f = h->baseCents + tuning::cents(h->ratio);
      const double k = std::max(1.0, std::round(std::exp2((cents - f) / 1200.0)));
      offer(f + 1200.0 * std::log2(k));
    }
  return best;
}

// --- bends between notes ----------------------------------------------------------------------------------------------

std::optional<double> RollState::bendNeighbour(const doc::Note& n, int edge) const {
  if (!clip_) return std::nullopt;
  const std::optional<double> own = pathBaseCents(n.start, n.pitch, edge < 0 ? 0.0 : static_cast<double>(n.length));
  if (!own) return std::nullopt;
  const Ticks at = edge < 0 ? n.start : n.start + n.length;
  auto distance = [&](const doc::Note& m) -> Ticks {
    if (m.id == n.id) return -1;
    if (edge < 0) return m.start < n.start ? std::abs(m.start + m.length - at) : -1;
    return m.start > n.start ? std::abs(m.start - at) : -1;
  };
  Ticks nearest = INT64_MAX;
  for (const doc::Note& m : clip_->notes)
    if (const Ticks d = distance(m); d >= 0) nearest = std::min(nearest, d);
  if (nearest == INT64_MAX) return std::nullopt;
  std::optional<double> best;
  for (const doc::Note& m : clip_->notes) {
    if (distance(m) != nearest) continue;
    // Where the neighbour sounds at the edge that faces this note (its own curve included).
    const std::optional<double> c = pathCents(m, m.start, m.pitch, edge < 0 ? static_cast<double>(m.length) : 0.0);
    if (c && (!best || std::fabs(*c - *own) < std::fabs(*best))) best = *c - *own;
  }
  return best;
}

Ticks RollState::bendLength(doc::NoteId note, int edge, float* cents) const {
  const doc::NoteCurves* nc = clip_ && clip_->curves ? doc::findNoteCurves(*clip_, note) : nullptr;
  if (!nc) return 0;
  const Span p = nc->pitch;
  if (edge < 0) {
    const size_t e = leadInEnd(p);
    if (!e) return 0;
    if (cents) *cents = p[0].value;
    return p[e].tick;
  }
  const size_t b = leadOutBegin(p);
  if (b == kNone) return 0;
  if (cents) *cents = p.back().value;
  return p.back().tick - p[b].tick;
}

bool RollState::bendIsStraight(doc::NoteId note, int edge) const {
  const doc::NoteCurves* nc = clip_ && clip_->curves ? doc::findNoteCurves(*clip_, note) : nullptr;
  if (!nc) return false;
  const Span p = nc->pitch;
  if (edge < 0) return leadInEnd(p) == 1;
  const size_t b = leadOutBegin(p);
  return b != kNone && b + 2 == p.size();
}

doc::ops::EditResult RollState::bendEdit(DragState& d) const {
  if (!clip_) return invalid("no clip");
  const int edge = d.kind == DragKind::ResizeStart ? -1 : 1;
  if (!d.bendFound) {
    d.bendFound = true;
    d.bendDelta.clear();
    for (const doc::Note& n : clip_->notes)
      if (selected(n.id))
        if (const std::optional<double> delta = bendNeighbour(n, edge)) d.bendDelta.emplace_back(n.id.value, *delta);
    std::sort(d.bendDelta.begin(), d.bendDelta.end());
  }
  std::vector<doc::Command> commands;
  for (const auto& [id, delta] : d.bendDelta) {
    const doc::Note* n = findNote(doc::NoteId{id});
    if (!n) continue;
    const doc::NoteCurves* nc = clip_->curves ? doc::findNoteCurves(*clip_, n->id) : nullptr;
    const Span old = nc ? Span(nc->pitch) : Span();
    // Inward from the edge, as far as the bend on the other edge leaves room.
    const Ticks room = n->length - bendLength(n->id, -edge);
    const Ticks bend = std::clamp<Ticks>(edge < 0 ? d.dt : -d.dt, 0, std::max<Ticks>(room, 0));
    const doc::JiShape::Kind kind = bendIsStraight(n->id, edge) ? doc::JiShape::LogLinear : doc::JiShape::SCurve;
    Pts pts = withBend(old, n->length, edge, bend, delta, kind);
    if (!std::equal(pts.begin(), pts.end(), old.begin(), old.end())) commands.push_back(doc::SetNoteCurve{ref_, doc::keyOf(*n), std::move(pts)});
  }
  return batchOf(edge < 0 ? "Bend In" : "Bend Out", std::move(commands), ref_, selectionKeys());
}

void RollState::updateBendPreview() {
  curvePreview_.clear();
  if (!drag.bend || (drag.kind != DragKind::ResizeStart && drag.kind != DragKind::ResizeEnd)) return;
  const doc::ops::EditResult e = bendEdit(drag);
  if (!e) return;
  for (const doc::Command& c : e->batch().commands)
    if (const doc::SetNoteCurve* set = std::get_if<doc::SetNoteCurve>(&c.v)) curvePreview_.push_back(doc::NoteCurves{set->note.id, set->pitch});
  std::sort(curvePreview_.begin(), curvePreview_.end(), [](const doc::NoteCurves& a, const doc::NoteCurves& b) { return a.note < b.note; });
}

bool RollState::glideSelection() {
  if (!clip_ || sel_.size() < 2) {
    ctx.setStatus("Glide: select two or more notes");
    return false;
  }
  std::vector<const doc::Note*> notes;
  for (const doc::Note& n : clip_->notes)
    if (selected(n.id)) notes.push_back(&n);
  std::stable_sort(notes.begin(), notes.end(), [](const doc::Note* a, const doc::Note* b) { return a->start < b->start; });
  const Grid g = grid();
  std::vector<doc::Command> commands;
  for (size_t i = 0; i < notes.size(); ++i) {
    const doc::Note& a = *notes[i];
    // The next selected note: the earliest start after this one's, nearest in pitch when several start together.
    size_t j = i + 1;
    while (j < notes.size() && notes[j]->start <= a.start) ++j;
    if (j == notes.size()) continue;
    const std::optional<double> own = pathBaseCents(a.start, a.pitch, static_cast<double>(a.length));
    if (!own) continue;
    std::optional<double> delta;
    for (size_t k = j; k < notes.size() && notes[k]->start == notes[j]->start; ++k) {
      const std::optional<double> c = pathCents(*notes[k], notes[k]->start, notes[k]->pitch, 0.0);
      if (c && (!delta || std::fabs(*c - *own) < std::fabs(*delta))) delta = *c - *own;
    }
    if (!delta) continue;
    const Ticks end = a.start + a.length;
    Ticks step = kPpq / 4;
    if (g.enabled()) {
      const TickRange cell = g.cellAt(end - 1);
      step = std::max<Ticks>(1, cell.end - cell.start);
    }
    const Ticks bend = std::clamp<Ticks>(step, 0, std::max<Ticks>(a.length - bendLength(a.id, -1), 0));
    const doc::NoteCurves* nc = clip_->curves ? doc::findNoteCurves(*clip_, a.id) : nullptr;
    const Span old = nc ? Span(nc->pitch) : Span();
    Pts pts = withBend(old, a.length, 1, bend, *delta, doc::JiShape::SCurve);
    if (!std::equal(pts.begin(), pts.end(), old.begin(), old.end())) commands.push_back(doc::SetNoteCurve{ref_, doc::keyOf(a), std::move(pts)});
  }
  if (commands.empty()) {
    ctx.setStatus("Glide: the selected notes already glide into each other (or start together at one pitch)");
    return false;
  }
  return apply(batchOf("Glide Notes", std::move(commands), ref_, selectionKeys()), "Glide Notes");
}

bool RollState::setBendShape(int edge, doc::JiShape::Kind kind) {
  if (!clip_ || !clip_->curves) return false;
  std::vector<doc::Command> commands;
  for (const doc::Note& n : clip_->notes) {
    if (!selected(n.id)) continue;
    float cents = 0.0f;
    const Ticks bend = bendLength(n.id, edge, &cents);
    if (bend <= 0) continue;
    const doc::NoteCurves* nc = doc::findNoteCurves(*clip_, n.id);
    Pts pts = withBend(nc->pitch, n.length, edge, bend, cents, kind);
    if (pts != nc->pitch) commands.push_back(doc::SetNoteCurve{ref_, doc::keyOf(n), std::move(pts)});
  }
  if (commands.empty()) return false;
  return apply(batchOf("Bend Shape", std::move(commands), ref_, selectionKeys()), "Bend Shape");
}

bool RollState::clearBends() {
  if (!clip_ || !clip_->curves) return false;
  std::vector<doc::Command> commands;
  for (const doc::Note& n : clip_->notes)
    if (selected(n.id) && doc::findNoteCurves(*clip_, n.id)) commands.push_back(doc::SetNoteCurve{ref_, doc::keyOf(n), {}});
  if (commands.empty()) return false;
  return apply(batchOf("Clear Bends", std::move(commands), ref_, selectionKeys()), "Clear Bends");
}

// --- resize, detune ---------------------------------------------------------------------------------------------------

std::vector<doc::Breakpoint> RollState::retimeCurve(std::span<const doc::Breakpoint> p, Ticks oldLength, Ticks newLength, Ticks startDelta) {
  (void)oldLength;
  Pts out;
  if (p.empty() || newLength < 1) return out;
  const size_t n = p.size();
  const size_t inEnd = leadInEnd(p), outBegin = leadOutBegin(p);
  const Ticks lin = inEnd ? p[inEnd].tick : 0;
  const Ticks lout = outBegin != kNone ? p.back().tick - p[outBegin].tick : 0;
  const Ticks nin = std::min(lin, newLength), nout = std::min(lout, newLength - nin);
  // The lead-in stays on the start (squeezed when the note is now shorter than it).
  if (lin > 0)
    for (size_t i = 0; i <= inEnd; ++i) push(out, bp(lin == nin ? p[i].tick : p[i].tick * nin / lin, p[i].value, p[i].curve));
  // What lies between keeps its place in time and is cut where the note no longer reaches.
  const Ticks lo = nin, hi = newLength - nout;
  const size_t b0 = lin > 0 ? inEnd + 1 : 0, b1 = outBegin != kNone ? outBegin : n;
  if (b0 < b1 && hi > lo) {
    Pts mid;
    bool cutLo = false, cutHi = false;
    for (size_t i = b0; i < b1; ++i) {
      const Ticks t = p[i].tick - startDelta;
      if (t < lo) cutLo = true;
      else if (t > hi) cutHi = true;
      else mid.push_back(bp(t, p[i].value, p[i].curve));
    }
    if (cutLo) push(out, bp(lo, valueAt(p, lo + startDelta)));
    for (const doc::Breakpoint& b : mid) push(out, b);
    if (cutHi) push(out, bp(hi, valueBefore(p, hi + startDelta)));
  }
  // The lead-out stays on the end.
  if (lout > 0 && nout > 0)
    for (size_t i = outBegin; i < n; ++i) push(out, bp(newLength - (p.back().tick - p[i].tick) * nout / lout, p[i].value, p[i].curve));
  normalise(out);
  return out;
}

doc::ops::EditResult RollState::resizeEdit(doc::ops::NoteEdge edge, Ticks delta) const {
  if (!clip_) return invalid("no clip");
  doc::ops::EditResult e = doc::ops::resizeNotes(*ctx.doc(), ref_, selectionKeys(), edge, delta);
  if (!e || e->empty() || !clip_->curves) return e;
  doc::Batch* batch = std::get_if<doc::Batch>(&e->command.v);
  if (!batch) return e;
  for (const doc::Note& n : clip_->notes) {
    if (!selected(n.id)) continue;
    const doc::NoteCurves* nc = doc::findNoteCurves(*clip_, n.id);
    if (!nc) continue;
    // As ops::resizeNotes: the other edge stays, the note keeps at least one tick.
    const Ticks end = n.start + n.length;
    Ticks start = n.start, length = std::max<Ticks>(1, n.length + delta);
    if (edge == doc::ops::NoteEdge::Start) {
      start = std::min(n.start + delta, end - 1);
      for (const doc::NoteKey& k : e->notes)
        if (k.id == n.id) start = k.start;  // where the op put it
      length = end - start;
    }
    Pts pts = retimeCurve(nc->pitch, n.length, length, start - n.start);
    if (pts != nc->pitch) batch->commands.push_back(doc::SetNoteCurve{ref_, doc::NoteKey{start, n.id}, std::move(pts)});
  }
  return e;
}

doc::ops::EditResult RollState::detuneEdit(Ticks dt, double cents) const {
  if (!clip_) return invalid("no clip");
  Ticks first = INT64_MAX;
  for (const doc::Note& n : clip_->notes)
    if (selected(n.id)) first = std::min(first, n.start);
  if (first == INT64_MAX) return invalid("no notes selected");
  dt = std::max(dt, -first);
  doc::UpdateNotes update{ref_, {}};
  std::vector<doc::NoteKey> keys;
  for (const doc::Note& n : clip_->notes) {
    if (!selected(n.id)) continue;
    doc::Note m = n;
    m.start = n.start + dt;
    // A note of a JI object stays on its voice (it has no pitch of its own to detune).
    if (cents != 0.0 && !doc::jiObjectOf(n.pitch).valid())
      if (const std::optional<double> c = centsOf(n.pitch)) m.pitch = tuning::PitchId::fromCents(*c + cents);
    if (m.start == n.start && m.pitch == n.pitch) {
      keys.push_back(doc::keyOf(n));
      continue;
    }
    update.updates.push_back(doc::NoteUpdate{doc::keyOf(n), m});
    keys.push_back(doc::keyOf(m));
  }
  std::vector<doc::Command> commands;
  if (!update.updates.empty()) commands.push_back(std::move(update));
  return batchOf("Detune Notes", std::move(commands), ref_, std::move(keys));
}

// --- vibrato ----------------------------------------------------------------------------------------------------------

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
  return hz;
}

std::string RollState::vibratoText(double depthCents, double rateHz) {
  return "Vibrato " + number(depthCents) + " \xC2\xA2 \xC2\xB7 " + number(rateHz) + " Hz";
}

bool RollState::parseVibrato(std::string_view text, double& depthCents, double& rateHz, std::string& error) {
  double found[2] = {0.0, 0.0};
  int count = 0;
  for (size_t i = 0; i < text.size() && count < 2;) {
    const char ch = text[i];
    if ((ch >= '0' && ch <= '9') || ch == '.') {
      double v = 0.0;
      const auto r = std::from_chars(text.data() + i, text.data() + text.size(), v);
      if (r.ec != std::errc()) {
        error = "type the depth in cents and the rate in Hz, such as 30 6";
        return false;
      }
      found[count++] = v;
      i = static_cast<size_t>(r.ptr - text.data());
    } else {
      ++i;
    }
  }
  if (count == 0) {
    error = "type the depth in cents and the rate in Hz, such as 30 6";
    return false;
  }
  if (!(found[0] >= 0.0 && found[0] <= 1200.0)) {
    error = "the depth is 0 to 1200 cents";
    return false;
  }
  if (count == 2 && !(found[1] >= 0.1 && found[1] <= 30.0)) {
    error = "the rate is 0.1 to 30 Hz";
    return false;
  }
  depthCents = found[0];
  if (count == 2) rateHz = found[1];
  return true;
}

bool RollState::addVibratoToSelection(double depthCents, double rateHz) {
  if (!clip_ || sel_.empty() || !(rateHz > 0.0)) return false;
  std::vector<doc::Command> commands;
  bool tooShort = false;
  for (const doc::Note& n : clip_->notes) {
    if (!selected(n.id)) continue;
    const doc::NoteCurves* nc = clip_->curves ? doc::findNoteCurves(*clip_, n.id) : nullptr;
    const Span old = nc ? Span(nc->pitch) : Span();
    const Ticks out = bendLength(n.id, 1);
    const Ticks from = bendLength(n.id, -1), to = n.length - out;
    if (to <= from) continue;
    const Pts fresh = vibratoPoints(from, to, depthCents, ticksPerSecond(n.start) / rateHz, kMaxVibratoPoints);
    if (fresh.empty() && depthCents > 0.0) {
      tooShort = true;
      continue;
    }
    Pts pts = replaceRegion(old, from, to, fresh, out > 0);
    if (!std::equal(pts.begin(), pts.end(), old.begin(), old.end())) commands.push_back(doc::SetNoteCurve{ref_, doc::keyOf(n), std::move(pts)});
  }
  if (commands.empty()) {
    if (tooShort) ctx.setStatus("Vibrato: the note is shorter than one cycle at " + number(rateHz) + " Hz");
    return false;
  }
  if (depthCents > 0.0) vibratoDepth_ = depthCents;
  const bool ok = apply(batchOf("Add Vibrato", std::move(commands), ref_, selectionKeys()), "Add Vibrato");
  if (ok) ctx.setStatus(vibratoText(depthCents, rateHz));
  return ok;
}

std::optional<RollState::Vibrato> RollState::vibratoAt(const CurveTarget& t, Ticks offset) const {
  const std::span<const doc::Breakpoint> p = curvePoints(t);
  Ticks start = 0, length = 0;
  if (p.size() < 5 || !curveSpan(t, start, length)) return std::nullopt;
  // The swings: points where the curve turns.
  std::vector<size_t> turns;
  for (size_t i = 1; i + 1 < p.size(); ++i)
    if ((p[i].value - p[i - 1].value) * (p[i + 1].value - p[i].value) < 0.0f && p[i].value != 0.0f) turns.push_back(i);
  if (turns.size() < 2) return std::nullopt;
  // The swing nearest the tick, then its neighbours while they alternate at about the same size and spacing.
  size_t seed = 0;
  for (size_t k = 1; k < turns.size(); ++k)
    if (std::abs(p[turns[k]].tick - offset) < std::abs(p[turns[seed]].tick - offset)) seed = k;
  auto alike = [&](size_t a, size_t b, Ticks spacing) {
    const double va = p[turns[a]].value, vb = p[turns[b]].value;
    if (va * vb >= 0.0) return false;
    const double r = std::fabs(va) / std::fabs(vb);
    if (r < 0.6 || r > 1.0 / 0.6) return false;
    if (spacing <= 0) return true;
    const double s = static_cast<double>(std::abs(p[turns[a]].tick - p[turns[b]].tick)) / static_cast<double>(spacing);
    return s > 0.6 && s < 1.0 / 0.6;
  };
  size_t lo = seed, hi = seed;
  Ticks spacing = 0;
  if (seed + 1 < turns.size() && alike(seed, seed + 1, 0)) spacing = p[turns[seed + 1]].tick - p[turns[seed]].tick;
  else if (seed > 0 && alike(seed - 1, seed, 0)) spacing = p[turns[seed]].tick - p[turns[seed - 1]].tick;
  if (spacing <= 0) return std::nullopt;
  while (hi + 1 < turns.size() && alike(hi, hi + 1, spacing)) ++hi;
  while (lo > 0 && alike(lo - 1, lo, spacing)) --lo;
  if (hi == lo) return std::nullopt;
  // It starts on the last zero before its first swing and ends on the first zero after its last.
  size_t a = turns[lo], b = turns[hi];
  while (a > 0 && p[a].value != 0.0f) --a;
  while (b + 1 < p.size() && p[b].value != 0.0f) ++b;
  if (p[a].value != 0.0f || p[b].value != 0.0f) return std::nullopt;
  Vibrato v;
  v.from = p[a].tick;
  v.to = p[b].tick;
  if (offset < v.from || offset > v.to || v.to <= v.from) return std::nullopt;
  const size_t swings = hi - lo + 1;
  for (size_t k = lo; k <= hi; ++k) v.depth += std::fabs(p[turns[k]].value) / static_cast<double>(swings);
  const double period = 2.0 * static_cast<double>(v.to - v.from) / static_cast<double>(swings);
  v.rateHz = ticksPerSecond(start + v.from) / period;
  return v;
}

bool RollState::setVibrato(const CurveTarget& t, const Vibrato& was, double depthCents, double rateHz) {
  Ticks start = 0, length = 0;
  if (!clip_ || !curveSpan(t, start, length) || !(rateHz > 0.0)) return false;
  const std::span<const doc::Breakpoint> old = curvePoints(t);
  const Pts fresh = vibratoPoints(was.from, was.to, depthCents, ticksPerSecond(start + was.from) / rateHz, kMaxVibratoPoints);
  if (fresh.empty() && depthCents > 0.0) {
    ctx.setStatus("Vibrato: under one cycle fits there at " + number(rateHz) + " Hz");
    return false;
  }
  if (depthCents > 0.0) vibratoDepth_ = depthCents;
  const bool ok = commitCurve(t, replaceRegion(old, was.from, was.to, fresh, true), "Edit Vibrato");
  if (ok) ctx.setStatus(vibratoText(depthCents, rateHz));
  return ok;
}

ui::TextEdit* RollState::openVibratoEntry() {
  if (!clip_ || sel_.empty()) return nullptr;
  const doc::Note* first = nullptr;
  for (const doc::Note& n : clip_->notes)
    if (selected(n.id)) {
      first = &n;
      break;
    }
  if (!first) return nullptr;
  const double rate = defaultVibratoRate(first->start);
  const std::string initial = number(vibratoDepth_) + " \xC2\xA2 \xC2\xB7 " + number(rate) + " Hz";
  const ui::IRect anchor = onPath(*first, first->pitch) ? pathBox(*first, first->start, first->length, first->pitch)
                                                        : screenRectOf(first->start, first->length, first->pitch);
  return openJiField("vibrato", "Add Vibrato: depth in cents, rate in Hz",
                     "The selected notes get a vibrato between their bends: its depth (cents, the swing each way) and its rate (Hz).", initial,
                     anchor, [rate](RollState& s, const std::string& text, std::string& error) {
                       double depth = s.vibratoDepth(), hz = rate;
                       if (!parseVibrato(text, depth, hz, error)) return false;
                       if (!s.addVibratoToSelection(depth, hz)) {
                         error = depth > 0.0 ? "the notes are shorter than one cycle" : "there is no vibrato to remove";
                         return false;
                       }
                       return true;
                     });
}

ui::TextEdit* RollState::openVibratoEdit(const CurveTarget& t, const Vibrato& v) {
  Ticks start = 0, length = 0;
  if (!clip_ || !curveSpan(t, start, length)) return nullptr;
  const std::string initial = number(v.depth) + " \xC2\xA2 \xC2\xB7 " + number(v.rateHz) + " Hz";
  const float x = xOfTick(static_cast<double>(start + v.from));
  const std::optional<double> base = curveBase(t, static_cast<double>(v.from));
  const ui::IRect anchor{static_cast<int32_t>(x), static_cast<int32_t>(base ? yOfCents(*base) : static_cast<float>(area().y)) - 8,
                         std::max(8, static_cast<int32_t>(xOfTick(static_cast<double>(start + v.to)) - x)), 16};
  return openJiField("vibrato", "Vibrato: depth in cents, rate in Hz", "This vibrato's depth (cents, the swing each way) and rate (Hz); depth 0 removes it.",
                     initial, anchor, [t, v](RollState& s, const std::string& text, std::string& error) {
                       double depth = v.depth, hz = v.rateHz;
                       if (!parseVibrato(text, depth, hz, error)) return false;
                       if (!s.setVibrato(t, v, depth, hz)) {
                         error = "nothing changed (under one cycle fits there at that rate)";
                         return false;
                       }
                       return true;
                     });
}

// --- pen --------------------------------------------------------------------------------------------------------------

void RollState::beginPen(const CurveTarget& t, ui::Vec2 p, ui::Mods mods, bool vibrato) {
  Ticks start = 0, length = 0;
  drag = {};
  if (!curveSpan(t, start, length)) return;
  const Ticks off = std::clamp<Ticks>(static_cast<Ticks>(std::llround(tickAtX(p.x))) - start, 0, length);
  const std::optional<double> base = curveBase(t, static_cast<double>(off));
  if (!base) return;
  DragState& d = drag;
  d.kind = vibrato ? DragKind::PenVibrato : DragKind::PenDraw;
  d.curve = t;
  d.press = d.last = p;
  d.pressMods = mods;
  d.pressTick = off;
  const std::span<const doc::Breakpoint> old = curvePoints(t);
  const double v0 = valueAt(old, off);
  d.penHold = *base + v0 - centsAtScreenY(p.y);  // the pen holds the line where it was pressed
  d.stroke.emplace_back(off, static_cast<float>(v0));
  d.strokeLast = off;
  d.curvePts.assign(old.begin(), old.end());
  if (vibrato) {
    d.vibRate = defaultVibratoRate(start + off);
    d.vibDepth = vibratoDepth_;
  }
}

void RollState::penTo(ui::Vec2 p, ui::Mods) {
  DragState& d = drag;
  Ticks start = 0, length = 0;
  if (!clip_ || !curveSpan(d.curve, start, length)) return;
  const Ticks off = std::clamp<Ticks>(static_cast<Ticks>(std::llround(tickAtX(p.x))) - start, 0, length);
  // The document's curve (the preview shows this gesture's result).
  Span old;
  if (d.curve.note.valid()) {
    if (const doc::NoteCurves* nc = clip_->curves ? doc::findNoteCurves(*clip_, d.curve.note) : nullptr) old = nc->pitch;
  } else if (const doc::JiObject* o = doc::findJiObject(*clip_, d.curve.ji)) {
    old = o->ride;
  }
  const double px2 = std::fabs(centsAtScreenY(p.y) - centsAtScreenY(p.y + 2.0f));  // two pixels, in cents
  if (d.kind == DragKind::PenVibrato) {
    const Ticks ta = std::min(d.pressTick, off), tb = std::max(d.pressTick, off);
    d.cents = std::max(d.cents, std::fabs(centsAtScreenY(p.y) - centsAtScreenY(d.press.y)));
    d.vibDepth = d.cents > 1.5 * px2 ? std::min(d.cents, 1200.0) : vibratoDepth_;  // a flat stroke: the last depth
    const Pts fresh = vibratoPoints(ta, tb, d.vibDepth, ticksPerSecond(start + ta) / d.vibRate, kMaxVibratoPoints);
    if (fresh.empty()) d.curvePts.assign(old.begin(), old.end());
    else d.curvePts = replaceRegion(old, ta, fresh.back().tick, fresh, true);
    setCurvePreview(d.curve, d.curvePts);
    return;
  }
  const std::optional<double> base = curveBase(d.curve, static_cast<double>(off));
  if (!base) return;
  const float v = bp(off, centsAtScreenY(p.y) + d.penHold - *base).value;
  // The stroke is a function of time: what the pointer passes over again is redrawn.
  const Ticks lo = std::min(d.strokeLast, off), hi = std::max(d.strokeLast, off);
  std::erase_if(d.stroke, [&](const std::pair<Ticks, float>& s) { return (s.first > lo && s.first < hi) || s.first == off; });
  d.stroke.insert(std::lower_bound(d.stroke.begin(), d.stroke.end(), off, [](const std::pair<Ticks, float>& s, Ticks t) { return s.first < t; }),
                  {off, v});
  d.strokeLast = off;
  if (d.stroke.size() < 2) {
    d.curvePts.assign(old.begin(), old.end());
  } else {
    Pts raw;
    raw.reserve(d.stroke.size());
    for (const auto& [t, value] : d.stroke) raw.push_back(bp(t, value));
    const Ticks ta = raw.front().tick, tb = raw.back().tick;
    // Where the stroke begins and ends on the curve that was there (within two pixels), it joins it exactly.
    if (ta > 0 && std::fabs(raw.front().value - valueBefore(old, ta)) <= px2) raw.front().value = static_cast<float>(valueBefore(old, ta));
    const bool joins = std::fabs(raw.back().value - valueAt(old, tb)) <= px2;
    if (joins) raw.back().value = static_cast<float>(valueAt(old, tb));
    double tolerance = kThinCents;
    Pts fresh = doc::ops::thinAutomation(raw, tolerance);
    while (fresh.size() > kMaxPenPoints) fresh = doc::ops::thinAutomation(raw, tolerance *= 2.0);
    // A stroke that comes back to the curve keeps what follows; one that ends elsewhere holds its last pitch.
    d.curvePts = replaceRegion(old, ta, tb, fresh, joins);
  }
  setCurvePreview(d.curve, d.curvePts);
}

bool RollState::beginCurvePoint(const CurveTarget& t, ui::Vec2 p) {
  Ticks start = 0, length = 0;
  drag = {};
  if (!curveSpan(t, start, length)) return false;
  const Ticks off = std::clamp<Ticks>(static_cast<Ticks>(std::llround(tickAtX(p.x))) - start, 0, length);
  const std::optional<double> base = curveBase(t, static_cast<double>(off));
  if (!base) return false;
  const std::span<const doc::Breakpoint> old = curvePoints(t);
  Pts pts(old.begin(), old.end());
  if (pts.size() >= doc::kMaxCurvePoints) return false;
  if (pts.empty()) pts = {bp(0, 0.0), bp(length, 0.0)};  // the first point of a curve bends the note between its ends
  const double v = valueAt(pts, off);
  auto it = std::find_if(pts.begin(), pts.end(), [&](const doc::Breakpoint& b) { return b.tick == off; });
  if (it == pts.end()) {
    it = std::upper_bound(pts.begin(), pts.end(), off, [](Ticks tick, const doc::Breakpoint& b) { return tick < b.tick; });
    const float curve = it != pts.begin() && it != pts.end() ? std::prev(it)->curve : 0.0f;
    it = pts.insert(it, bp(off, v, curve));
  }
  DragState& d = drag;
  d.kind = DragKind::CurvePoint;
  d.curve = t;
  d.curveIndex = static_cast<int32_t>(it - pts.begin());
  d.curvePts = std::move(pts);
  d.curveAdded = true;
  d.press = d.last = p;
  d.penHold = *base + v - centsAtScreenY(p.y);
  d.bx = 0.0f;
  setCurvePreview(t, d.curvePts);
  return true;
}

void RollState::beginCurveHandle(const CurveHit& h, ui::Vec2 p) {
  drag = {};
  const std::span<const doc::Breakpoint> old = curvePoints(h.target);
  Ticks start = 0, length = 0;
  if (h.index < 0 || static_cast<size_t>(h.index) >= old.size() || !curveSpan(h.target, start, length)) return;
  const doc::Breakpoint& b = old[static_cast<size_t>(h.index)];
  const std::optional<double> base = curveBase(h.target, static_cast<double>(b.tick));
  if (!base) return;
  DragState& d = drag;
  d.kind = h.mid ? DragKind::CurveBow : DragKind::CurvePoint;
  d.curve = h.target;
  d.curveIndex = h.index;
  d.curvePts.assign(old.begin(), old.end());
  d.press = d.last = p;
  d.bow0 = b.curve;
  d.penHold = *base + b.value - centsAtScreenY(p.y);
  d.bx = xOfTick(static_cast<double>(start + b.tick)) - p.x;  // the handle keeps its offset from the pointer
}

void RollState::curveHandleTo(ui::Vec2 p, ui::Mods mods) {
  DragState& d = drag;
  Ticks start = 0, length = 0;
  Pts& pts = d.curvePts;
  if (d.curveIndex < 0 || static_cast<size_t>(d.curveIndex) >= pts.size() || !curveSpan(d.curve, start, length)) return;
  const size_t i = static_cast<size_t>(d.curveIndex);
  if (d.kind == DragKind::CurveBow) {
    if (i + 1 >= pts.size()) return;
    // The diamond follows the pointer: down pulls the middle of the segment down.
    const float sign = pts[i + 1].value > pts[i].value ? 1.0f : -1.0f;
    pts[i].curve = std::clamp(d.bow0 + sign * (p.y - d.press.y) / (60.0f * scaleOf(*this)), -1.0f, 1.0f);
  } else {
    const Grid g = grid();
    Ticks at = static_cast<Ticks>(std::llround(tickAtX(p.x + d.bx)));
    if (g.snapping()) at = g.snap(at);
    const Ticks lo = i > 0 ? pts[i - 1].tick : 0, hi = i + 1 < pts.size() ? pts[i + 1].tick : length;
    const Ticks off = std::clamp<Ticks>(at - start, lo, std::max(lo, hi));
    const std::optional<double> base = curveBase(d.curve, static_cast<double>(off));
    if (!base) return;
    double cents = centsAtScreenY(p.y) + d.penHold;
    if (!(mods & mod::kShift)) cents = snapCurveCents(cents, *base);  // rows, harmonic guides, the note's own pitch
    pts[i] = bp(off, cents - *base, pts[i].curve);
  }
  setCurvePreview(d.curve, pts);
}

bool RollState::deleteCurvePoint(const CurveHit& h) {
  const std::span<const doc::Breakpoint> old = curvePoints(h.target);
  if (h.mid || h.index < 0 || static_cast<size_t>(h.index) >= old.size()) return false;
  Pts pts(old.begin(), old.end());
  pts.erase(pts.begin() + h.index);
  return commitCurve(h.target, std::move(pts), "Delete Bend Point");
}

// --- paint ------------------------------------------------------------------------------------------------------------

void paintCurveHandles(PaintContext& ctx, RollState& s, const IRect& vis) {
  if (!ctx.res.theme || !s.hasClip()) return;
  static std::vector<CurveTarget> targets;  // UI thread only; reused across frames
  static std::vector<RollState::CurveHandle> handles;
  s.shownCurves(targets);
  if (targets.empty()) return;
  const Theme& th = *ctx.res.theme;
  const float sc = ctx.scale.factor;
  const float x0 = static_cast<float>(vis.x) - 8.0f * sc, x1 = static_cast<float>(vis.right()) + 8.0f * sc;
  const float y0 = static_cast<float>(vis.y) - 8.0f * sc, y1 = static_cast<float>(vis.bottom()) + 8.0f * sc;
  const DragState& d = s.drag;
  for (const CurveTarget& t : targets) {
    s.curveHandles(t, handles);
    const Rgba ring = t.ji.valid() ? th.roll.jiSelected : s.colours().selBorder;
    for (const RollState::CurveHandle& h : handles) {
      if (h.pos.x < x0 || h.pos.x > x1 || h.pos.y < y0 || h.pos.y > y1) continue;
      const bool held = d.curve == t && d.curveIndex == h.index && (d.kind == DragKind::CurveBow) == h.mid &&
                        (d.kind == DragKind::CurvePoint || d.kind == DragKind::CurveBow);
      if (h.mid) {
        const float r = (held ? 4.5f : 3.5f) * sc, q = r - std::max(1.0f, 1.25f * sc);
        const Vec2 outer[4] = {{h.pos.x - r, h.pos.y}, {h.pos.x, h.pos.y - r}, {h.pos.x + r, h.pos.y}, {h.pos.x, h.pos.y + r}};
        const Vec2 inner[4] = {{h.pos.x - q, h.pos.y}, {h.pos.x, h.pos.y - q}, {h.pos.x + q, h.pos.y}, {h.pos.x, h.pos.y + q}};
        ctx.dl.addConvexPolyFilled(outer, ring);
        if (!held) ctx.dl.addConvexPolyFilled(inner, th.roll.rowNatural);
      } else {
        ctx.dl.addCircleFilled(h.pos, (held ? 5.0f : 4.0f) * sc, ring);
        if (!held) ctx.dl.addCircleFilled(h.pos, 2.25f * sc, th.roll.rowNatural);
      }
    }
  }
}

}  // namespace pq::views::roll
