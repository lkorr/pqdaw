// T7.6 / T7.14 piano roll: bends between notes, pen modes, breakpoints (SPEC §3.1; docs/plans/M7.md "T7.6",
// amendment A3, "Rework: notes-first JI"). The RollState half lives here; roll_input.cpp routes the presses,
// roll_state.cpp commits. Vibrato is roll_vibrato.cpp.
//
// What is stored (T7.14: the derived API of T7.11a, QUESTIONS Q289 b):
//   bends        doc::NoteBend on the note's curve record: the NEIGHBOUR'S ID, a length and a shape. The glide's far
//                end is computed wherever the curve is read (doc::effectiveNoteCurve: the roll, the lowering), so a
//                bend follows its neighbour when either note is moved or retuned. Nothing is recognised by shape.
//   free curve   NoteCurves::pitch: pen strokes and breakpoints, cents over ticks from the note's start. Never read as
//                a bend (M7 review H4): a resize leaves it where it is in time.
//
//   Alt + a note's left / right edge   the note glides in from the previous note / out to the next one over the
//                                      dragged length (S-curve; "Bend In / Out ▸ Straight" in the note's menu); back to
//                                      the edge removes it. Every selected note bends, each to its own neighbour
//                                      (bendNeighbour); the neighbours are outlined while the drag runs. A note with
//                                      no neighbour there bends from / to the pitch dragged to (up / down; snapped
//                                      as a breakpoint, Shift free; a plain vertical drag glides over a grid cell),
//                                      written into its free curve.
//   Ctrl+G                             each selected note glides out into the next selected one over one grid step.
//   Draw Mode (B) on a note            drag: the stroke is the note's pitch curve from there on (smoothed over
//                                      kPenSmoothPx each way, then thinned to 0.5 ¢, at
//                                      most kMaxPenPoints points; a drag under kMinPenStroke px is a click);
//                                      Alt + drag: vibrato over the dragged span (the note's vibrato property: depth
//                                      = the stroke's height); Ctrl + click: a breakpoint (drag it on); a click
//                                      deletes the note.
//   breakpoints                        shown on the selected notes: drag (rows / harmonic
//                                      guides / the note's own pitch; Shift: free), double-click deletes, the diamond
//                                      between two points bends the segment.
//   Shift + a note's body              free detune (after a dead zone): the selected notes become Cents notes.
// Every gesture previews the record its release stores (curvePreview_) and is one undo step.
//
// Cost: nothing here runs per audio block. A drag frame re-thins one stroke (O(points · log)) or rebuilds one record
// per selected note (and the roll flattens each of those notes' paths once per frame: RollState::pathOf's preview
// entries, T7.16a M15); the
// neighbour search of a bend runs once per gesture (two passes over the clip's notes per selected note). Paint adds
// the handles of at most 64 selected notes, each of at most kMaxCurveHandles points.

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <string>
#include <variant>

#include "pq/document/automation_curve.h"
#include "pq/document/commands.h"
#include "pq/document/note_bend.h"
#include "pq/ui/lines.h"
#include "pq/ui/overlay.h"
#include "pq/ui/theme.h"
#include "roll_widgets.h"

namespace pq::views::roll {

using namespace pq::ui;

namespace {

using Pts = std::vector<doc::Breakpoint>;
using Span = std::span<const doc::Breakpoint>;
constexpr double kThinCents = 0.5;  // a stroke stays this close to what was drawn
constexpr double kSamePitchCents = 0.5;  // a neighbour nearer than this is "at the note's own pitch"
constexpr float kPenSmoothPx = 6.0f;     // a stroke is smoothed over this many logical px each way

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

// The pointer arrives on whole device pixels, so a slow stroke is a staircase of one-pixel steps (7 ¢ on 14 px rows)
// with the hand's jitter on top. Each inner sample becomes the triangle-weighted mean of the samples within `radius`
// ticks of it: the steps turn into slopes, a deliberate move stays (it is wider than the window). Near the ends the
// window shrinks to stay symmetric (a straight stroke stays straight), and the two ends keep their values (they join
// the curve that was there). O(samples · samples in a window).
void smoothStroke(Pts& p, double radius, std::vector<float>& scratch) {
  if (p.size() < 3 || !(radius > 0.0)) return;
  scratch.resize(p.size());
  const double t0 = static_cast<double>(p.front().tick), t1 = static_cast<double>(p.back().tick);
  size_t lo = 0, hi = 0;  // (both window edges only move right: t − r and t + r never decrease)
  for (size_t i = 0; i < p.size(); ++i) {
    const double t = static_cast<double>(p[i].tick);
    const double r = std::min({radius, t - t0, t1 - t});
    scratch[i] = p[i].value;
    if (!(r > 0.0)) continue;
    while (static_cast<double>(p[lo].tick) < t - r) ++lo;
    while (hi + 1 < p.size() && static_cast<double>(p[hi + 1].tick) <= t + r) ++hi;
    double sw = 0.0, sv = 0.0;
    for (size_t j = lo; j <= hi; ++j) {
      const double w = 1.0 - std::fabs(static_cast<double>(p[j].tick) - t) / r;
      sw += w;
      sv += w * static_cast<double>(p[j].value);
    }
    scratch[i] = sw > 0.0 ? static_cast<float>(sv / sw) : p[i].value;
  }
  for (size_t i = 1; i + 1 < p.size(); ++i) p[i].value = scratch[i];
}

// A curve that is 0 everywhere is no curve.
void normalise(Pts& p) {
  if (std::all_of(p.begin(), p.end(), [](const doc::Breakpoint& b) { return b.value == 0.0f; })) p.clear();
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

doc::ops::EditResult batchOf(std::string label, std::vector<doc::Command> commands, doc::ClipRef clip, std::vector<doc::NoteKey> keys) {
  doc::ops::Edit e;
  e.command = doc::Command{doc::Batch{std::move(label), std::move(commands)}};
  e.noteClip = clip;
  e.notes = std::move(keys);
  return e;
}

doc::NoteBend& bendOf(doc::NoteCurves& c, int edge) { return edge < 0 ? c.bendIn : c.bendOut; }
const doc::NoteBend& bendOf(const doc::NoteCurves& c, int edge) { return edge < 0 ? c.bendIn : c.bendOut; }

}  // namespace

// --- the curves -------------------------------------------------------------------------------------------------------

const doc::NoteCurves* RollState::curveRecord(doc::NoteId note) const {
  if (!clip_ || !note.valid()) return nullptr;
  if (!curvePreview_.empty()) {
    const auto it = std::lower_bound(curvePreview_.begin(), curvePreview_.end(), note,
                                     [](const doc::NoteCurves& c, doc::NoteId id) { return c.note < id; });
    if (it != curvePreview_.end() && it->note == note) return &*it;
  }
  return clip_->curves ? doc::findNoteCurves(*clip_, note) : nullptr;
}

std::span<const doc::Breakpoint> RollState::curvePoints(const CurveTarget& t) const {
  const doc::NoteCurves* rec = curveRecord(t.note);
  return rec ? std::span<const doc::Breakpoint>(rec->pitch) : std::span<const doc::Breakpoint>();
}

bool RollState::curveSpan(const CurveTarget& t, Ticks& start, Ticks& length) const {
  const doc::Note* n = clip_ && t.note.valid() ? findNote(t.note) : nullptr;
  if (!n) return false;
  start = n->start;
  length = n->length;
  return true;
}

std::optional<double> RollState::curveBase(const CurveTarget& t, double offset) const {
  const doc::Note* n = clip_ && t.note.valid() ? findNote(t.note) : nullptr;
  if (!n) return std::nullopt;
  std::optional<double> base = startCents(*n);
  if (!base) return std::nullopt;
  // Its bends and its group's warp there (the free curve is what is edited; the vibrato rides on top of it).
  const doc::NoteCurves* rec = curveRecord(n->id);
  const doc::NotePitchIndex& index = pitchIndex();
  const doc::NoteWarp warp = index.anyWarp() ? index.warp(*clip_, n->id) : doc::NoteWarp{};
  if (!rec && !warp.any()) return base;
  doc::NoteBendTargets targets = rec ? doc::noteBendTargets(*rec, index) : doc::NoteBendTargets{};
  targets.vibratoPeriod = 0.0;
  const Ticks len = std::max<Ticks>(n->length, 1);
  const double tick = std::clamp(offset, 0.0, static_cast<double>(len));
  *base += doc::noteCurveCents(rec, len, targets, warp, tick);
  if (rec && !rec->pitch.empty()) *base -= pq::evalAutomation(std::span<const doc::Breakpoint>(rec->pitch), tick);
  return base;
}

double RollState::centsAtScreenY(float y) const noexcept {
  if (rows_.empty()) return 0.0;
  return rows_.centsAtY(static_cast<double>(y) - static_cast<double>(area().y - scrollY_));
}

double RollState::ticksPerSecond(Ticks contentTick) const {
  if (clip_ && snap_) return doc::clipTicksPerSecond(*snap_, *clip_, contentTick);  // as the playback's vibrato period
  return 2.0 * static_cast<double>(kPpq);                                           // 120 bpm
}

doc::NoteCurves& RollState::previewRecord(doc::NoteId note) {
  touchPreview();  // the caller changes the record
  const auto it = std::lower_bound(curvePreview_.begin(), curvePreview_.end(), note,
                                   [](const doc::NoteCurves& c, doc::NoteId id) { return c.note < id; });
  if (it != curvePreview_.end() && it->note == note) return *it;
  doc::NoteCurves rec;
  if (const doc::NoteCurves* have = clip_ && clip_->curves ? doc::findNoteCurves(*clip_, note) : nullptr) rec = *have;
  rec.note = note;
  return *curvePreview_.insert(it, std::move(rec));
}

void RollState::setCurvePreview(const CurveTarget& t, const std::vector<doc::Breakpoint>& pts) {
  if (t.note.valid()) previewRecord(t.note).pitch = pts;
  markNotesDirty();
}

void RollState::clearCurvePreview() {
  curvePreview_.clear();
  warpPreview_ = {};
  touchPreview();
}

bool RollState::commitCurve(const CurveTarget& t, std::vector<doc::Breakpoint> pts, std::string_view what) {
  if (!clip_) return false;
  normalise(pts);
  const doc::Note* n = t.note.valid() ? findNote(t.note) : nullptr;
  if (!n) return false;
  const doc::NoteCurves* have = clip_->curves ? doc::findNoteCurves(*clip_, t.note) : nullptr;
  if (have ? have->pitch == pts : pts.empty()) return false;  // nothing changed: no undo step
  std::vector<uint32_t> keep = sel_;
  const bool ok = apply(doc::ops::setNoteCurve(*ctx.doc(), ref_, doc::keyOf(*n), std::move(pts)), what);
  if (ok) select(std::move(keep));
  return ok;
}

// --- handles ----------------------------------------------------------------------------------------------------------

void RollState::shownCurves(std::vector<CurveTarget>& out) const {
  out.clear();
  if (!clip_) return;
  auto add = [&](const CurveTarget& t) {
    if (std::find(out.begin(), out.end(), t) == out.end() && !curvePoints(t).empty()) out.push_back(t);
  };
  if ((clip_->curves || !curvePreview_.empty()) && sel_.size() <= 64) {
    cacheSelectedNotes();  // (the handles ask for each note)
    for (const uint32_t id : sel_) add(CurveTarget{doc::NoteId{id}});
  }
  if (ctx.drawMode() && hoverPart_.note != 0 && drag.kind == DragKind::None) add(CurveTarget{doc::NoteId{hoverPart_.note}});
  if (drag.curve.valid()) add(drag.curve);
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
  if (!clip_ || !clip_->curves) return hit;
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

double RollState::snapCurveCents(double cents, double own, double tick) const {
  double best = own;
  auto offer = [&](double c) {
    if (std::fabs(c - cents) < std::fabs(best - cents)) best = c;
  };
  if (!rows_.empty())
    if (const PitchRow* row = rows_.rowOfDegree(rows_.placeCents(cents).nearestDegree)) offer(row->cents);
  if (harmonics_)
    if (const std::optional<Harmonics> h = harmonics()) {
      const double f = h->baseCents + harmonicsShift(*h, tick) + tuning::cents(h->ratio);  // the guides at that tick
      const double k = std::max(1.0, std::round(std::exp2((cents - f) / 1200.0)));
      offer(f + 1200.0 * std::log2(k));
    }
  return best;
}

// --- bends between notes ----------------------------------------------------------------------------------------------

const doc::Note* RollState::bendNeighbour(const doc::Note& n, int edge, bool* samePitch) const {
  if (samePitch) *samePitch = false;
  if (!clip_) return nullptr;
  const std::optional<double> own = startCents(n);
  if (!own) return nullptr;
  const Ticks at = edge < 0 ? n.start : n.start + n.length;
  // How far a note's facing end is from this edge: a note that starts earlier ends there (bend in), a note that
  // starts later starts there (bend out). −1: not a candidate.
  auto gap = [&](const doc::Note& m) -> Ticks {
    if (m.id == n.id) return -1;
    if (edge < 0) return m.start < n.start ? std::abs(m.start + m.length - at) : -1;
    return m.start > n.start ? std::abs(m.start - at) : -1;
  };
  Ticks nearest = INT64_MAX;
  for (const doc::Note& m : clip_->notes)
    if (const Ticks d = gap(m); d >= 0) nearest = std::min(nearest, d);
  if (nearest == INT64_MAX) return nullptr;
  // Everything within one grid step of the nearest is "the notes before / after": a bass note that ends exactly on
  // the edge does not win over the melody note that ends a few ticks early (M9).
  Ticks step = kPpq / 4;
  if (const Grid g = grid(); g.enabled()) {
    const TickRange cell = g.cellAt(std::max<Ticks>(at - (edge < 0 ? 1 : 0), 0));
    step = std::max<Ticks>(1, cell.end - cell.start);
  }
  const doc::Note* best = nullptr;
  double bestCents = 0.0;
  Ticks bestGap = 0;
  bool same = false;
  for (const doc::Note& m : clip_->notes) {
    const Ticks d = gap(m);
    if (d < 0 || d - nearest > step) continue;
    const std::optional<double> c = startCents(m);
    if (!c) continue;
    const double delta = std::fabs(*c - *own);
    if (delta <= kSamePitchCents) {
      same = true;
      continue;
    }
    if (!best || delta < bestCents || (delta == bestCents && d < bestGap)) {
      best = &m;
      bestCents = delta;
      bestGap = d;
    }
  }
  if (!best && samePitch) *samePitch = same;
  return best;
}

Ticks RollState::bendLength(doc::NoteId note, int edge) const {
  const doc::NoteCurves* rec = curveRecord(note);
  const doc::Note* n = findNote(note);
  if (!rec || !n || !bendOf(*rec, edge).set()) return 0;
  return std::min(bendOf(*rec, edge).length, std::max<Ticks>(n->length, 1));
}

bool RollState::bendIsStraight(doc::NoteId note, int edge) const {
  const doc::NoteCurves* rec = curveRecord(note);
  return rec && bendOf(*rec, edge).set() && bendOf(*rec, edge).shape.kind == doc::GlideShape::LogLinear;
}

doc::ops::EditResult RollState::bendEdit(DragState& d) const {
  if (!clip_) return invalid("no clip");
  const int edge = d.kind == DragKind::ResizeStart ? -1 : 1;
  auto docRecord = [&](doc::NoteId id) { return clip_->curves ? doc::findNoteCurves(*clip_, id) : nullptr; };
  if (!d.bendFound) {
    d.bendFound = true;
    d.bendPicks.clear();
    d.bendFree.clear();
    for (const doc::Note& n : clip_->notes) {
      if (!selected(n.id)) continue;
      // A bend that is there keeps its neighbour (the drag changes its length); a new one finds it.
      const doc::NoteCurves* have = docRecord(n.id);
      if (have && bendOf(*have, edge).set() && findNote(bendOf(*have, edge).other)) d.bendPicks.emplace_back(n.id.value, bendOf(*have, edge).other);
      else if (const doc::Note* other = bendNeighbour(n, edge)) d.bendPicks.emplace_back(n.id.value, other->id);
      else d.bendFree.push_back(n.id.value);
    }
    std::sort(d.bendPicks.begin(), d.bendPicks.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::sort(d.bendFree.begin(), d.bendFree.end());
  }
  doc::SetNoteCurves set{ref_, {}};
  // Inward from the edge, as far as the bend on the other edge leaves room. A free bend dragged only up or down glides
  // over one grid cell.
  const Grid g = grid();
  auto lengthOf = [&](const doc::Note& n, const doc::NoteCurves& rec, bool free) {
    const doc::NoteBend& opposite = bendOf(rec, -edge);
    const Ticks room = n.length - (opposite.set() ? std::min(opposite.length, n.length) : 0);
    Ticks want = edge < 0 ? d.dt : -d.dt;
    if (free && d.dt == 0) {
      want = kPpq / 4;
      if (g.enabled()) {
        const TickRange cell = g.cellAt(edge < 0 ? n.start : std::max<Ticks>(n.start + n.length - 1, 0));
        want = std::max<Ticks>(1, cell.end - cell.start);
      }
    }
    return std::clamp<Ticks>(want, 0, std::max<Ticks>(room, 0));
  };
  auto bent = [&](doc::NoteId id) -> std::optional<doc::NoteCurves> {
    const doc::Note* n = findNote(id);
    if (!n) return std::nullopt;
    const doc::NoteCurves* have = docRecord(n->id);
    doc::NoteCurves rec = have ? *have : doc::NoteCurves{n->id, {}, {}, {}, {}};
    const bool free = std::binary_search(d.bendFree.begin(), d.bendFree.end(), id.value);
    const Ticks length = lengthOf(*n, rec, free);
    if (const auto pick = std::lower_bound(d.bendPicks.begin(), d.bendPicks.end(), id.value, [](const auto& p, uint32_t v) { return p.first < v; });
        pick != d.bendPicks.end() && pick->first == id.value) {
      doc::NoteBend& b = bendOf(rec, edge);
      if (length <= 0) b = {};
      else b = doc::NoteBend{length, b.set() ? b.shape : doc::GlideShape{}, pick->second};
    } else if (length > 0 && d.bendCents != 0.0) {
      // No neighbour: a glide in its free curve from / to the dragged pitch, joining what the curve does there.
      const Span old = rec.pitch;
      const Ticks L = std::max<Ticks>(n->length, 1);
      if (edge < 0) {
        const Pts fresh{bp(0, valueAt(old, 0) + d.bendCents), bp(length, valueAt(old, length))};
        rec.pitch = replaceRegion(old, 0, length, fresh, true);
      } else {
        const Pts fresh{bp(L - length, valueAt(old, L - length)), bp(L, valueBefore(old, L) + d.bendCents)};
        rec.pitch = replaceRegion(old, L - length, L, fresh, false);
      }
    }
    if (have ? *have == rec : rec.empty()) return std::nullopt;
    return rec;
  };
  // Both lists are sorted by id: merged, the records stay in note order.
  size_t i = 0, j = 0;
  while (i < d.bendPicks.size() || j < d.bendFree.size()) {
    const bool pick = j == d.bendFree.size() || (i < d.bendPicks.size() && d.bendPicks[i].first < d.bendFree[j]);
    const doc::NoteId id{pick ? d.bendPicks[i++].first : d.bendFree[j++]};
    if (std::optional<doc::NoteCurves> rec = bent(id)) set.curves.push_back(std::move(*rec));
  }
  std::vector<doc::Command> commands;
  if (!set.curves.empty()) commands.push_back(std::move(set));
  return batchOf(edge < 0 ? "Bend In" : "Bend Out", std::move(commands), ref_, selectionKeys());
}

void RollState::updateBendPreview() {
  curvePreview_.clear();
  touchPreview();
  if (!drag.bend || (drag.kind != DragKind::ResizeStart && drag.kind != DragKind::ResizeEnd)) return;
  const doc::ops::EditResult e = bendEdit(drag);
  if (!e) return;
  for (const doc::Command& c : e->batch().commands)
    if (const doc::SetNoteCurves* set = std::get_if<doc::SetNoteCurves>(&c.v)) curvePreview_ = set->curves;
  std::sort(curvePreview_.begin(), curvePreview_.end(), [](const doc::NoteCurves& a, const doc::NoteCurves& b) { return a.note < b.note; });
  if (!drag.bendFree.empty()) {
    const bool in = drag.kind == DragKind::ResizeStart;
    char buf[96];
    if (drag.bendCents == 0.0) std::snprintf(buf, sizeof buf, "%s: drag up or down for the pitch it %s", in ? "Bend In" : "Bend Out", in ? "comes from" : "goes to");
    else std::snprintf(buf, sizeof buf, "%s %s %+.0f \xC2\xA2", in ? "Bend In" : "Bend Out", in ? "from" : "to", drag.bendCents);
    ctx.setStatus(buf);
  }
}

double RollState::freeBendCents(ui::Vec2 p, ui::Mods mods) const {
  const DragState& d = drag;
  const double dc = centsAtScreenY(p.y) - centsAtScreenY(d.press.y);
  const doc::Note* n = clip_ ? findNote(d.pressed) : nullptr;
  if (!n || (mods & mod::kShift)) return std::round(dc);
  // The pitch the pressed note has at that edge; the dragged pitch snaps as a breakpoint does.
  const double off = d.kind == DragKind::ResizeStart ? 0.0 : static_cast<double>(n->length);
  const std::optional<double> base = curveBase(CurveTarget{n->id}, off);
  if (!base) return std::round(dc);
  Span free;  // the document's (the preview holds this gesture's result)
  if (const doc::NoteCurves* nc = clip_->curves ? doc::findNoteCurves(*clip_, n->id) : nullptr) free = nc->pitch;
  const double own = *base + (free.empty() ? 0.0 : pq::evalAutomation(free, off));
  return snapCurveCents(own + dc, own, static_cast<double>(n->start) + off) - own;
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
  doc::SetNoteCurves set{ref_, {}};
  for (size_t i = 0; i < notes.size(); ++i) {
    const doc::Note& a = *notes[i];
    // The next selected note: the earliest start after this one's, nearest in pitch when several start together.
    size_t j = i + 1;
    while (j < notes.size() && notes[j]->start <= a.start) ++j;
    if (j == notes.size()) continue;
    const std::optional<double> own = startCents(a);
    if (!own) continue;
    const doc::Note* next = nullptr;
    double best = 0.0;
    for (size_t k = j; k < notes.size() && notes[k]->start == notes[j]->start; ++k) {
      const std::optional<double> c = startCents(*notes[k]);
      if (c && (!next || std::fabs(*c - *own) < best)) {
        next = notes[k];
        best = std::fabs(*c - *own);
      }
    }
    if (!next) continue;
    const Ticks end = a.start + a.length;
    Ticks step = kPpq / 4;
    if (g.enabled()) {
      const TickRange cell = g.cellAt(end - 1);
      step = std::max<Ticks>(1, cell.end - cell.start);
    }
    const doc::NoteCurves* have = clip_->curves ? doc::findNoteCurves(*clip_, a.id) : nullptr;
    doc::NoteCurves rec = have ? *have : doc::NoteCurves{a.id, {}, {}, {}, {}};
    const Ticks room = a.length - (rec.bendIn.set() ? std::min(rec.bendIn.length, a.length) : 0);
    const Ticks length = std::clamp<Ticks>(step, 0, std::max<Ticks>(room, 0));
    if (length <= 0) continue;
    rec.bendOut = doc::NoteBend{length, rec.bendOut.set() ? rec.bendOut.shape : doc::GlideShape{}, next->id};
    if (have ? !(*have == rec) : true) set.curves.push_back(std::move(rec));
  }
  if (set.curves.empty()) {
    ctx.setStatus("Glide: the selected notes already glide into each other (or start together)");
    return false;
  }
  std::vector<doc::Command> commands;
  commands.push_back(std::move(set));
  return apply(batchOf("Glide Notes", std::move(commands), ref_, selectionKeys()), "Glide Notes");
}

bool RollState::setBendShape(int edge, doc::GlideShape::Kind kind) {
  if (!clip_ || !clip_->curves) return false;
  doc::SetNoteCurves set{ref_, {}};
  for (const doc::Note& n : clip_->notes) {
    if (!selected(n.id)) continue;
    const doc::NoteCurves* have = doc::findNoteCurves(*clip_, n.id);
    if (!have || !bendOf(*have, edge).set() || bendOf(*have, edge).shape.kind == kind) continue;
    doc::NoteCurves rec = *have;
    bendOf(rec, edge).shape.kind = kind;
    set.curves.push_back(std::move(rec));
  }
  if (set.curves.empty()) return false;
  std::vector<doc::Command> commands;
  commands.push_back(std::move(set));
  return apply(batchOf("Bend Shape", std::move(commands), ref_, selectionKeys()), "Bend Shape");
}

bool RollState::clearBends() {
  if (!clip_ || !clip_->curves) return false;
  doc::SetNoteCurves set{ref_, {}};
  for (const doc::Note& n : clip_->notes)
    if (selected(n.id) && doc::findNoteCurves(*clip_, n.id)) set.curves.push_back(doc::NoteCurves{n.id, {}, {}, {}, {}});
  if (set.curves.empty()) return false;
  std::vector<doc::Command> commands;
  commands.push_back(std::move(set));
  return apply(batchOf("Clear Curves", std::move(commands), ref_, selectionKeys()), "Clear Curves");
}

// --- resize, detune ---------------------------------------------------------------------------------------------------

std::vector<doc::Breakpoint> RollState::retimeCurve(std::span<const doc::Breakpoint> p, Ticks newLength, Ticks startDelta) {
  Pts out;
  if (p.empty() || newLength < 1) return out;
  // Every point keeps its place in time; what the note no longer reaches is cut, with a point on the cut.
  bool cutLo = false, cutHi = false;
  Pts mid;
  for (const doc::Breakpoint& b : p) {
    const Ticks t = b.tick - startDelta;
    if (t < 0) cutLo = true;
    else if (t > newLength) cutHi = true;
    else mid.push_back(bp(t, b.value, b.curve));
  }
  if (cutLo) push(out, bp(0, valueAt(p, startDelta)));
  for (const doc::Breakpoint& b : mid) push(out, b);
  if (cutHi) push(out, bp(newLength, valueBefore(p, newLength + startDelta)));
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
    if (!nc || nc->pitch.empty()) continue;
    // As ops::resizeNotes: the other edge stays, the note keeps at least one tick.
    const Ticks end = n.start + n.length;
    Ticks start = n.start, length = std::max<Ticks>(1, n.length + delta);
    if (edge == doc::ops::NoteEdge::Start) {
      start = std::min(n.start + delta, end - 1);
      for (const doc::NoteKey& k : e->notes)
        if (k.id == n.id) start = k.start;  // where the op put it
      length = end - start;
    }
    Pts pts = retimeCurve(nc->pitch, length, start - n.start);
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
    if (cents != 0.0)
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
    const doc::NoteVibrato have = vibratoOf(t.note);
    d.vibrato.push_back({t.note, have, std::max<Ticks>(length, 1)});
    d.vibRate = have.set() && have.unit == doc::NoteVibrato::RateUnit::Hz ? static_cast<double>(have.rate) : kDefaultVibratoHz;
    d.vibDepth = vibratoDepth_;
  }
}

void RollState::penTo(ui::Vec2 p, ui::Mods) {
  DragState& d = drag;
  Ticks start = 0, length = 0;
  if (!clip_ || !curveSpan(d.curve, start, length)) return;
  const Ticks off = std::clamp<Ticks>(static_cast<Ticks>(std::llround(tickAtX(p.x))) - start, 0, length);
  // The document's free curve (the preview shows this gesture's result).
  Span old;
  if (const doc::NoteCurves* nc = clip_->curves ? doc::findNoteCurves(*clip_, d.curve.note) : nullptr) old = nc->pitch;
  const double px2 = std::fabs(centsAtScreenY(p.y) - centsAtScreenY(p.y + 2.0f));  // two pixels, in cents
  if (d.kind == DragKind::PenVibrato) {
    // T7.14: the note's vibrato property over the dragged span (nothing is baked).
    const Ticks ta = std::min(d.pressTick, off), tb = std::max(d.pressTick, off);
    d.cents = std::max(d.cents, std::fabs(centsAtScreenY(p.y) - centsAtScreenY(d.press.y)));
    d.vibDepth = d.cents > 1.5 * px2 ? std::min(std::round(d.cents), static_cast<double>(doc::kMaxVibratoCents)) : vibratoDepth_;  // a flat stroke: the last depth
    doc::NoteVibrato v;
    v.depthCents = static_cast<float>(d.vibDepth);
    v.rate = static_cast<float>(std::clamp(d.vibRate, static_cast<double>(doc::kMinVibratoHz), static_cast<double>(doc::kMaxVibratoHz)));
    v.unit = doc::NoteVibrato::RateUnit::Hz;
    v.onset = std::min(ta, std::max<Ticks>(length - 1, 0));
    v.end = tb >= length ? 0 : std::max(tb, v.onset + 1);
    previewRecord(d.curve.note).vibrato = tb > ta ? v : d.vibrato.front().base;
    markNotesDirty();
    ctx.setStatus(vibratoText(d.vibDepth, d.vibRate));
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
    static std::vector<float> smoothScratch;  // UI thread only; reused
    if (pxPerTick_ > 0.0) smoothStroke(raw, static_cast<double>(kPenSmoothPx * scaleOf(*this)) / pxPerTick_, smoothScratch);
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
    if (!(mods & mod::kShift)) cents = snapCurveCents(cents, *base, static_cast<double>(start + off));  // rows, harmonic guides, own pitch
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
  const Theme& th = *ctx.res.theme;
  const float sc = ctx.scale.factor;
  const DragState& d = s.drag;
  // T7.14: the notes an Alt + edge drag bends from / to, outlined while it runs.
  if (d.bend && (d.kind == DragKind::ResizeStart || d.kind == DragKind::ResizeEnd))
    for (const auto& [id, other] : d.bendPicks) {
      (void)id;
      const doc::Note* m = s.findNote(other);
      if (!m) continue;
      const IRect r = s.onPath(*m, m->pitch) ? s.pathBox(*m, m->start, m->length, m->pitch) : s.screenRectOf(m->start, m->length, m->pitch);
      if (r.empty() || intersect(r, vis).empty()) continue;
      ctx.dl.addRoundedRectStroke(r.toRect(), 3.0f * sc, th.accent.primary, std::max(1.0f, std::round(1.5f * sc)));
    }
  static std::vector<CurveTarget> targets;  // UI thread only; reused across frames
  static std::vector<RollState::CurveHandle> handles;
  s.shownCurves(targets);
  if (targets.empty()) return;
  const float x0 = static_cast<float>(vis.x) - 8.0f * sc, x1 = static_cast<float>(vis.right()) + 8.0f * sc;
  const float y0 = static_cast<float>(vis.y) - 8.0f * sc, y1 = static_cast<float>(vis.bottom()) + 8.0f * sc;
  for (const CurveTarget& t : targets) {
    s.curveHandles(t, handles);
    const Rgba ring = s.colours().selBorder;
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
