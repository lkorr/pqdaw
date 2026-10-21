// Internals shared by the edit-operation builders (T3.6).

#include "edit_common.h"

#include <algorithm>
#include <cassert>

namespace pq::doc {

double automationValueBefore(std::span<const Breakpoint> pts, Ticks t, double fallback) noexcept {
  if (pts.empty()) return fallback;
  const auto it = std::lower_bound(pts.begin(), pts.end(), t, [](const Breakpoint& b, Ticks v) { return b.tick < v; });
  if (it == pts.end()) return pts.back().value;
  if (it->tick == t || it == pts.begin()) return it->value;
  return automationSegmentValue(*(it - 1), *it, static_cast<double>(t));
}

double automationValueAt(std::span<const Breakpoint> pts, Ticks t, double fallback) noexcept {
  if (pts.empty()) return fallback;
  const auto it = std::upper_bound(pts.begin(), pts.end(), t, [](Ticks v, const Breakpoint& b) { return v < b.tick; });
  if (it == pts.begin()) return pts.front().value;  // before (or on, handled by upper_bound) the first point
  const Breakpoint& a = *(it - 1);                  // last point with tick ≤ t
  if (a.tick == t || it == pts.end()) return a.value;
  return automationSegmentValue(a, *it, static_cast<double>(t));
}

namespace ops {

bool Edit::empty() const noexcept { return batch().commands.empty(); }
const Batch& Edit::batch() const noexcept { return std::get<Batch>(command.v); }

namespace detail {

Edit makeEdit(std::string label) {
  Edit e;
  e.command = Batch{std::move(label), {}};
  return e;
}

Batch& batchOf(Edit& e) { return std::get<Batch>(e.command.v); }

// ---- Lookups ---------------------------------------------------------------------------------------------------

Result<ClipFound> findClip(const Project& p, const ClipRef& r) {
  const auto path = findTrack(p, r.track);
  if (!path) return notFound("track not found");
  const Track& t = trackAt(p, *path);
  const int32_t i = findClipIndex(t, r.clip);
  if (i < 0) return notFound("clip not found on this track");
  return ClipFound{*path, &t, t.clips[static_cast<size_t>(i)].clip};
}

Result<const Track*> findMidiTrack(const Project& p, TrackId id) {
  const Track* t = findTrackPtr(p, id);
  if (!t) return notFound("track not found");
  if (t->kind != TrackKind::Midi) return invalid("note clips live on MIDI tracks");
  return t;
}

const AutomationLane* findLane(const Track& t, LaneId id) {
  for (const LanePtr& l : t.lanes)
    if (l->id == id) return l.get();
  return nullptr;
}

Result<tuning::Tuning> clipTuning(const Project& p, const ClipFound& c) {
  const TuningDef* def = effectiveTuning(p, c.path, c.clip.get());
  if (!def) return notFound("the clip's tuning was not found");
  return buildTuning(*def);
}

// ---- Clips as values ---------------------------------------------------------------------------------------------

bool sameNotes(const NoteSeq& a, const NoteSeq& b) noexcept {
  const auto ca = a.chunks();
  const auto cb = b.chunks();
  if (ca.size() != cb.size()) return false;
  for (size_t i = 0; i < ca.size(); ++i)
    if (ca[i].notes != cb[i].notes || ca[i].count != cb[i].count) return false;
  return true;
}

NoteSeq seqOf(std::vector<Note> notes) {
  Result<NoteSeq> s = NoteSeq::fromNotes(std::move(notes));
  assert(s.ok() && "seqOf: notes must be valid with unique ids");
  return s.ok() ? std::move(s).value() : NoteSeq{};
}

namespace {
Ticks floorMod(Ticks a, Ticks m) {
  const Ticks r = a % m;
  return r < 0 ? r + m : r;
}
}  // namespace

NoteClip withBounds(const NoteClip& c, Ticks start, Ticks end) {
  NoteClip n = withNotes(c, c.notes);
  if (start >= c.start) {
    n.contentStart = clipContentAt(c, start - c.start);
  } else {
    const Ticks d = c.start - start;
    if (clipLooped(c) && c.contentStart >= c.loop.start && c.contentStart < c.loop.end) {
      // Unroll the loop backwards, so the part that already played keeps its content.
      const Ticks len = c.loop.end - c.loop.start;
      n.contentStart = c.loop.start + floorMod(c.contentStart - d - c.loop.start, len);
    } else {
      n.contentStart = c.contentStart - d;
    }
  }
  n.start = start;
  n.length = end - start;
  return n;
}

std::pair<NoteClip, NoteClip> splitClipAt(const NoteClip& c, Ticks t, ClipId rightId, IdSource& ids) {
  NoteClip left = withNotes(c, c.notes);
  left.length = t - c.start;
  if (clipLooped(c)) {
    NoteClip right = cloneClip(c, ids, false);
    right.id = rightId;
    right.start = t;
    right.length = clipEnd(c) - t;
    right.contentStart = clipContentAt(c, t - c.start);
    return {std::move(left), std::move(right)};
  }
  const Ticks sc = c.contentStart + (t - c.start);
  std::vector<Note> moved;
  std::vector<NoteKey> keys;
  for (auto [it, end] = c.notes.range(sc, kMaxTicks + 1); it != end; ++it) {
    moved.push_back(*it);
    keys.push_back(keyOf(*it));
  }
  if (!keys.empty()) left.notes = c.notes.withRemoved(keys);
  NoteClip right = withNotes(c, seqOf(std::move(moved)));
  right.id = rightId;
  right.start = t;
  right.length = clipEnd(c) - t;
  right.contentStart = sc;
  return {std::move(left), std::move(right)};
}

NoteClip cloneClip(const NoteClip& c, IdSource& ids, bool onlyPlaying) {
  std::vector<Note> notes;
  auto take = [&](const Note& n) {
    Note m = n;
    m.id = ids.note();
    notes.push_back(m);
  };
  if (onlyPlaying && !clipLooped(c)) {
    for (auto [it, end] = c.notes.range(c.contentStart, c.contentStart + c.length); it != end; ++it) take(*it);
  } else {
    notes.reserve(c.notes.size());
    for (const Note& n : c.notes) take(n);
  }
  NoteClip n = withNotes(c, seqOf(std::move(notes)));
  n.id = ids.clip();
  return n;
}

// ---- ClipEditor ----------------------------------------------------------------------------------------------------

ClipEditor::ClipEditor(const Track& t) : track_(&t) {
  clips_.reserve(t.clips.size() + 4);
  for (const ClipSlot& s : t.clips) clips_.push_back(s.clip);
}

ClipPtr ClipEditor::find(ClipId id) const {
  for (const ClipPtr& c : clips_)
    if (c->id == id) return c;
  return nullptr;
}

void ClipEditor::remove(ClipId id) {
  std::erase_if(clips_, [&](const ClipPtr& c) { return c->id == id; });
}

void ClipEditor::replace(ClipId id, ClipPtr c) {
  for (ClipPtr& x : clips_)
    if (x->id == id) {
      x = std::move(c);
      return;
    }
}

void ClipEditor::clearRange(Ticks a, Ticks b, IdSource& ids, std::span<const ClipId> keep) {
  if (b <= a) return;
  std::vector<ClipPtr> out;
  out.reserve(clips_.size() + 1);
  for (ClipPtr& cp : clips_) {
    const NoteClip& c = *cp;
    const Ticks s = c.start;
    const Ticks e = clipEnd(c);
    if (e <= a || s >= b || std::find(keep.begin(), keep.end(), c.id) != keep.end()) {
      out.push_back(std::move(cp));
    } else if (a <= s && e <= b) {
      // covered: dropped
    } else if (s < a && e <= b) {
      out.push_back(std::make_shared<const NoteClip>(withBounds(c, s, a)));
    } else if (a <= s) {
      out.push_back(std::make_shared<const NoteClip>(withBounds(c, b, e)));
    } else {  // s < a, b < e: keep both ends
      auto [l, r] = splitClipAt(c, b, ids.clip(), ids);
      out.push_back(std::make_shared<const NoteClip>(withBounds(l, s, a)));
      out.push_back(std::make_shared<const NoteClip>(std::move(r)));
    }
  }
  clips_ = std::move(out);
}

void ClipEditor::emit(Batch& removes, Batch& updates, Batch& adds) const {
  struct E {
    ClipId id;
    const ClipPtr* clip;
  };
  auto byId = [](const E& x, const E& y) { return x.id < y.id; };
  std::vector<E> before;
  before.reserve(track_->clips.size());
  for (const ClipSlot& s : track_->clips) before.push_back({s.id, &s.clip});
  std::vector<E> after;
  after.reserve(clips_.size());
  for (const ClipPtr& c : clips_) after.push_back({c->id, &c});
  std::sort(before.begin(), before.end(), byId);
  std::sort(after.begin(), after.end(), byId);
  const TrackId tid = track_->id;
  size_t i = 0, j = 0;
  while (i < before.size() || j < after.size()) {
    if (j == after.size() || (i < before.size() && before[i].id < after[j].id)) {
      removes.commands.push_back(RemoveClip{{tid, before[i].id}});
      ++i;
    } else if (i == before.size() || after[j].id < before[i].id) {
      adds.commands.push_back(AddClip{tid, *after[j].clip});
      ++j;
    } else {
      const NoteClip& o = **before[i].clip;
      const NoteClip& n = **after[j].clip;
      if (before[i].clip->get() != after[j].clip->get()) {
        const bool boundsOnly = o.name == n.name && o.colour == n.colour && o.loop == n.loop &&
                                o.signature == n.signature && o.tuning == n.tuning && o.active == n.active &&
                                sameNotes(o.notes, n.notes);
        if (boundsOnly) {
          if (o.start != n.start || o.length != n.length || o.contentStart != n.contentStart)
            updates.commands.push_back(SetClipBounds{{tid, n.id}, n.start, n.length, n.contentStart});
        } else {
          removes.commands.push_back(RemoveClip{{tid, o.id}});
          adds.commands.push_back(AddClip{tid, *after[j].clip});
        }
      }
      ++i;
      ++j;
    }
  }
}

Result<ClipEditor*> ClipEdits::editor(TrackId t) {
  for (auto& e : eds_)
    if (e->id() == t) return e.get();
  Result<const Track*> tr = findMidiTrack(p_, t);
  if (!tr) return std::move(tr).error();
  eds_.push_back(std::make_unique<ClipEditor>(**tr));
  return eds_.back().get();
}

ClipEditor& ClipEdits::editor(const Track& t) {
  for (auto& e : eds_)
    if (e->id() == t.id) return *e;
  eds_.push_back(std::make_unique<ClipEditor>(t));
  return *eds_.back();
}

void ClipEdits::emit(Batch& out) const {
  Batch removes, updates, adds;
  for (const auto& e : eds_) e->emit(removes, updates, adds);
  for (Batch* b : {&removes, &updates, &adds})
    for (Command& c : b->commands) out.commands.push_back(std::move(c));
}

// ---- Automation envelopes ----------------------------------------------------------------------------------------

namespace {
size_t firstAtOrAfter(std::span<const Breakpoint> pts, Ticks t) {
  return static_cast<size_t>(
      std::lower_bound(pts.begin(), pts.end(), t, [](const Breakpoint& b, Ticks v) { return b.tick < v; }) -
      pts.begin());
}
float segX(const Breakpoint& a, const Breakpoint& b, Ticks t) {
  return static_cast<float>(static_cast<double>(t - a.tick) / static_cast<double>(b.tick - a.tick));
}
}  // namespace

Points envBefore(std::span<const Breakpoint> pts, Ticks t, IdSource& ids, bool force) {
  const size_t n = pts.size();
  const size_t i = firstAtOrAfter(pts, t);
  Points out(pts.begin(), pts.begin() + static_cast<std::ptrdiff_t>(i));
  if (n == 0 || (!force && (i == n || i == 0))) return out;
  double v = 0.0;
  if (i == n) {
    v = pts[n - 1].value;  // after the last point: the held last value
  } else if (pts[i].tick == t || i == 0) {
    v = pts[i].value;
  } else {
    const Breakpoint& a = pts[i - 1];
    v = automationSegmentValue(a, pts[i], static_cast<double>(t));
    out.back().curve = a.curve * segX(a, pts[i], t);
  }
  out.push_back(Breakpoint{t, static_cast<float>(v), 0.0f, ids.breakpoint()});
  return out;
}

Points envFrom(std::span<const Breakpoint> pts, Ticks t, IdSource& ids) {
  const size_t n = pts.size();
  const size_t i = firstAtOrAfter(pts, t);
  Points out;
  out.reserve(n - i + 1);
  if (i > 0 && (i == n || pts[i].tick > t)) {
    double v = pts[n - 1].value;
    float c = 0.0f;
    if (i < n) {
      const Breakpoint& a = pts[i - 1];
      v = automationSegmentValue(a, pts[i], static_cast<double>(t));
      c = a.curve * (1.0f - segX(a, pts[i], t));
    }
    out.push_back(Breakpoint{t, static_cast<float>(v), c, ids.breakpoint()});
  } else if (i == 0 && n > 0 && pts[0].tick > t) {
    // The constant run before the first point: start it at t too, so a slice is self-contained.
    out.push_back(Breakpoint{t, pts[0].value, 0.0f, ids.breakpoint()});
  }
  out.insert(out.end(), pts.begin() + static_cast<std::ptrdiff_t>(i), pts.end());
  return out;
}

Points envSlice(std::span<const Breakpoint> pts, Ticks a, Ticks b, IdSource& ids, bool freshIds) {
  if (pts.empty()) return {};
  const Points left = envBefore(pts, b, ids, true);
  Points out = envFrom(left, a, ids);
  if (freshIds)
    for (Breakpoint& p : out) p.id = ids.breakpoint();
  return out;
}

void shiftPoints(Points& pts, Ticks dt) {
  for (Breakpoint& p : pts) p.tick += dt;
}

void append(Points& dst, const Points& src) { dst.insert(dst.end(), src.begin(), src.end()); }

void simplify(Points& pts, uint32_t firstNew, IdSource& ids, uint32_t keepLo, uint32_t keepHi) {
  // Construction order is the envelope order; only ticks need to be ordered (stable: same-tick points keep it).
  std::stable_sort(pts.begin(), pts.end(), [](const Breakpoint& a, const Breakpoint& b) { return a.tick < b.tick; });
  auto isNew = [&](const Breakpoint& b) {
    return b.id.value >= firstNew && !(b.id.value >= keepLo && b.id.value < keepHi);
  };
  bool changed = true;
  while (changed) {
    changed = false;
    // Same tick, same value: one point is enough. It carries the later point's outgoing curve and, if either is
    // an existing point, that point's id.
    for (size_t i = 0; i + 1 < pts.size(); ++i) {
      Breakpoint& p = pts[i];
      Breakpoint& q = pts[i + 1];
      if (p.tick == q.tick && p.value == q.value && (isNew(p) || isNew(q))) {
        if (isNew(q)) {
          p.curve = q.curve;
          pts.erase(pts.begin() + static_cast<std::ptrdiff_t>(i) + 1);
        } else {
          pts.erase(pts.begin() + static_cast<std::ptrdiff_t>(i));
        }
        changed = true;
        break;
      }
    }
    if (changed) continue;
    const size_t n = pts.size();
    for (size_t i = 0; i < n && n >= 2; ++i) {
      const Breakpoint& p = pts[i];
      if (!isNew(p)) continue;
      const bool prevEq = i == 0 || pts[i - 1].value == p.value;
      const bool nextEq = i + 1 == n || pts[i + 1].value == p.value;
      // A created point inside a constant run (or at a constant end) changes nothing.
      if (prevEq && nextEq && !(i > 0 && pts[i - 1].tick == p.tick) && !(i + 1 < n && pts[i + 1].tick == p.tick)) {
        pts.erase(pts.begin() + static_cast<std::ptrdiff_t>(i));
        changed = true;
        break;
      }
    }
  }
  // Points on one tick are ordered by id in a lane: where construction order disagrees, the later points of that
  // tick get fresh (larger) ids.
  for (size_t i = 1; i < pts.size(); ++i) {
    if (pts[i].tick != pts[i - 1].tick || pts[i - 1].id < pts[i].id) continue;
    for (size_t k = i; k < pts.size() && pts[k].tick == pts[i - 1].tick; ++k) pts[k].id = ids.breakpoint();
  }
}

void emitLaneDiff(Batch& out, TrackId track, const AutomationLane& lane, Points pts) {
  std::sort(pts.begin(), pts.end(), pointLess);
  if (pts == lane.points) return;
  auto byId = [](const Breakpoint& a, const Breakpoint& b) { return a.id < b.id; };
  Points before = lane.points;
  std::sort(before.begin(), before.end(), byId);
  std::sort(pts.begin(), pts.end(), byId);
  RemoveBreakpoints rem{track, lane.id, {}};
  UpdateBreakpoints upd{track, lane.id, {}};
  AddBreakpoints add{track, lane.id, {}};
  size_t i = 0, j = 0;
  while (i < before.size() || j < pts.size()) {
    if (j == pts.size() || (i < before.size() && before[i].id < pts[j].id)) {
      rem.points.push_back(before[i++].id);
    } else if (i == before.size() || pts[j].id < before[i].id) {
      add.points.push_back(pts[j++]);
    } else {
      if (!(before[i] == pts[j])) upd.points.push_back(pts[j]);
      ++i;
      ++j;
    }
  }
  if (!rem.points.empty()) out.commands.push_back(std::move(rem));
  if (!upd.points.empty()) out.commands.push_back(std::move(upd));
  if (!add.points.empty()) out.commands.push_back(std::move(add));
}

}  // namespace detail
}  // namespace ops
}  // namespace pq::doc
