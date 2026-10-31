// Time-selection edit operations (T3.6): insert / delete / duplicate time on every track.

#include <algorithm>

#include "edit_common.h"

namespace pq::doc::ops {

using namespace detail;

namespace {

enum class TimeOp : uint8_t { Insert, Delete, Duplicate };

// A time edit: Insert puts `len` ticks at `a` (b = a); Delete removes [a, b); Duplicate copies [a, b) to b.
struct TimeEdit {
  TimeOp op;
  Ticks a = 0;
  Ticks b = 0;
  Ticks len = 0;
  // Where a position at or after the edit point goes (positions inside a deleted range collapse to a).
  Ticks map(Ticks t) const {
    switch (op) {
      case TimeOp::Insert: return t >= a ? t + len : t;
      case TimeOp::Duplicate: return t >= b ? t + len : t;
      case TimeOp::Delete: return t < a ? t : (t < b ? a : t - len);
    }
    return t;
  }
  Ticks cut() const { return op == TimeOp::Insert ? a : b; }  // where insert/duplicate open the gap
};

// The envelope after the time edit (exact, see automation_curve.h). `force`: keep the run before the first point
// exactly even if the first point is moved or removed.
Points timeEditEnvelope(std::span<const Breakpoint> pts, const TimeEdit& te, IdSource& ids, bool force) {
  Points r;
  switch (te.op) {
    case TimeOp::Insert: {
      r = envBefore(pts, te.a, ids, force);
      Points right = envFrom(pts, te.a, ids);
      shiftPoints(right, te.len);
      append(r, right);
      break;
    }
    case TimeOp::Delete: {
      bool inside = false;
      for (const Breakpoint& p : pts) inside = inside || (p.tick >= te.a && p.tick < te.b);
      r = envBefore(pts, te.a, ids, force || inside);
      Points right = envFrom(pts, te.b, ids);
      shiftPoints(right, -te.len);
      append(r, right);
      break;
    }
    case TimeOp::Duplicate: {
      r = envBefore(pts, te.b, ids, force);
      Points copy = envSlice(pts, te.a, te.b, ids, true);
      shiftPoints(copy, te.len);
      append(r, copy);
      Points right = envFrom(pts, te.b, ids);
      shiftPoints(right, te.len);
      append(r, right);
      break;
    }
  }
  return r;
}

// ---- Clips -------------------------------------------------------------------------------------------------------

// Opens a gap of `len` at `at` in one track's clips: clips across `at` are split, later ones move.
void openGap(ClipEditor& ed, Ticks at, Ticks len, IdSource& ids) {
  std::vector<ClipSlot> out;
  out.reserve(ed.clips().size() + 1);
  for (ClipSlot& c : ed.clips()) {
    if (c.end() <= at) {
      out.push_back(std::move(c));
    } else if (c.start() >= at) {
      out.push_back(ed.moved(c, c.start() + len));
    } else {
      auto [l, r] = ed.split(c, at, ids.clip(), ids);
      out.push_back(std::move(l));
      out.push_back(ed.moved(r, r.start() + len));
    }
  }
  ed.clips() = std::move(out);
}

void editClips(const Project& p, const TimeEdit& te, IdSource& ids, Batch& out) {
  ClipEdits ed(p);
  forEachTrack(p, [&](const Track& t, const Track*) {
    if ((t.kind != TrackKind::Midi && t.kind != TrackKind::Audio) || t.clips.empty()) return;  // T5.3: audio too
    const Ticks from = te.a;
    // Clips are sorted by start; anything ending at or before `from` is untouched. A quick check on the last clips
    // avoids creating editors for tracks that end before the edit.
    const bool touched =
        std::any_of(t.clips.rbegin(), t.clips.rend(), [&](const ClipSlot& s) { return s.end() > from; });
    if (!touched) return;
    ClipEditor& e = ed.editor(t);
    switch (te.op) {
      case TimeOp::Insert: openGap(e, te.a, te.len, ids); break;
      case TimeOp::Delete: {
        e.clearRange(te.a, te.b, ids);
        for (ClipSlot& c : e.clips())
          if (c.start() >= te.b) c = e.moved(c, c.start() - te.len);
        break;
      }
      case TimeOp::Duplicate: {
        std::vector<ClipSlot> copies;
        for (const ClipSlot& c : e.clips()) {
          if (c.end() <= te.a || c.start() >= te.b) continue;
          const ClipSlot part = e.bounded(c, std::max(c.start(), te.a), std::min(c.end(), te.b));
          const ClipSlot copy = cloneSlot(part, ids, true);
          copies.push_back(e.moved(copy, copy.start() + te.len));
        }
        openGap(e, te.b, te.len, ids);
        for (ClipSlot& c : copies) e.add(std::move(c));
        break;
      }
    }
  });
  ed.emit(out);
}

// ---- Locators, loop ----------------------------------------------------------------------------------------------

void editLocators(const Project& p, const TimeEdit& te, Batch& out) {
  for (const Locator& l : *p.locators) {
    if (te.op == TimeOp::Delete && l.tick >= te.a && l.tick < te.b) {
      out.commands.push_back(RemoveLocator{l.id});
      continue;
    }
    const Ticks t = te.map(l.tick);
    if (t != l.tick) out.commands.push_back(UpdateLocator{Locator{l.id, t, l.name}});
  }
}

void editLoop(const Project& p, const TimeEdit& te, Batch& out) {
  LoopRegion l = p.loop;
  const Ticks len = l.end - l.start;
  l.start = te.map(l.start);
  // The end belongs to the material before it: an edit point exactly at the end leaves it where it is.
  if (te.op == TimeOp::Delete)
    l.end = l.end < te.a ? l.end : (l.end <= te.b ? te.a : l.end - te.len);
  else
    l.end = l.end > te.cut() ? l.end + te.len : l.end;
  if (l.end <= l.start) l.end = l.start + len;
  if (!(l == p.loop)) out.commands.push_back(SetLoop{l});
}

// ---- Tempo -------------------------------------------------------------------------------------------------------
// The tempo map as an envelope: a Linear point ramps (curve 0) to the next one; a Step point holds, i.e. an extra
// point at the next tick with the held value. Back from an envelope: per tick, the departing value is the tempo
// point; the segment to the next tick is Step if it arrives at the same value, else Linear.

// `curveById[id]` = the original curve of the tempo point that became envelope point `id` (Step for hold points).
Points tempoEnvelope(const std::vector<TempoPoint>& tp, std::vector<TempoCurve>& curveById) {
  Points e;
  uint32_t id = 1;
  curveById.assign(2 * tp.size() + 2, TempoCurve::Step);
  for (size_t i = 0; i < tp.size(); ++i) {
    curveById[id] = tp[i].curve;
    e.push_back(Breakpoint{tp[i].tick, static_cast<float>(tp[i].bpm), 0.0f, BreakpointId{id++}});
    if (tp[i].curve == TempoCurve::Step && i + 1 < tp.size())
      e.push_back(Breakpoint{tp[i + 1].tick, static_cast<float>(tp[i].bpm), 0.0f, BreakpointId{id++}});
  }
  return e;
}

std::vector<TempoPoint> tempoFromEnvelope(const Points& e, const std::vector<TempoPoint>& orig,
                                          const std::vector<TempoCurve>& curveById) {
  auto origCurve = [&](BreakpointId id) {
    return id.value < curveById.size() ? curveById[id.value] : TempoCurve::Step;
  };
  // Bpm values travel through float in the envelope: restore the original doubles where they match.
  auto exactBpm = [&](float v) {
    for (const TempoPoint& o : orig)
      if (static_cast<float>(o.bpm) == v) return o.bpm;
    return static_cast<double>(v);
  };
  std::vector<TempoPoint> out;
  for (size_t i = 0; i < e.size();) {
    size_t j = i;
    while (j + 1 < e.size() && e[j + 1].tick == e[i].tick) ++j;
    TempoPoint t{e[i].tick, exactBpm(e[j].value), origCurve(e[j].id)};
    if (j + 1 < e.size()) {
      size_t k = j + 1;  // the next tick's last point = its departing value
      while (k + 1 < e.size() && e[k + 1].tick == e[j + 1].tick) ++k;
      if (e[j + 1].value != e[j].value)
        t.curve = TempoCurve::Linear;  // a ramp (exact unless the next tick also jumps)
      else if (e[k].value != e[j].value)
        t.curve = TempoCurve::Step;    // hold, then jump
      // else: a hold; Step and Linear (to an equal value) are the same, keep the original flag
    }
    out.push_back(t);
    i = j + 1;
  }
  if (out.empty() || out.front().tick > 0) {
    const double first = out.empty() ? orig.front().bpm : out.front().bpm;
    out.insert(out.begin(), TempoPoint{0, first, TempoCurve::Step});
  }
  return out;
}

void editTempo(const Project& p, const TimeEdit& teIn, Batch& out) {
  // The tempo map starts at tick 0: edits before 0 act from 0 (the pre-roll keeps the first tempo).
  TimeEdit te = teIn;
  const std::vector<TempoPoint>& tp = p.timeline->tempo;
  if (te.op == TimeOp::Delete) {
    te.a = std::max<Ticks>(te.a, 0);
    te.b = std::max<Ticks>(te.b, 0);
    if (te.b <= te.a) return;
    te.len = te.b - te.a;
  } else if (te.op == TimeOp::Insert) {
    te.a = std::max<Ticks>(te.a, 0);
  } else if (te.b <= 0) {
    return;
  } else {
    te.a = std::max<Ticks>(te.a, 0);
  }
  if (tp.size() == 1) return;  // constant tempo: nothing moves
  std::vector<TempoCurve> curveById;
  const Points env = tempoEnvelope(tp, curveById);
  IdSource local;
  local.reserveThrough(static_cast<uint32_t>(env.size()) + 1);
  const uint32_t firstNew = local.peek();
  Points r = timeEditEnvelope(env, te, local, true);
  r = envFrom(r, 0, local);  // drop anything moved before 0, keeping the value at 0
  std::erase_if(r, [](const Breakpoint& b) { return b.tick < 0; });
  simplify(r, firstNew, local);
  std::vector<TempoPoint> np = tempoFromEnvelope(r, tp, curveById);
  bool same = np.size() == tp.size();
  for (size_t i = 0; same && i < np.size(); ++i)
    same = np[i].tick == tp[i].tick && np[i].bpm == tp[i].bpm && np[i].curve == tp[i].curve;
  if (!same) out.commands.push_back(SetTempoMap{std::move(np)});
}

// ---- Time signatures ---------------------------------------------------------------------------------------------
// Changes sit on bar lines. Each change moves with the time edit; one that no longer lands on a bar line is snapped
// to the next bar line of the signature before it.

void editSignatures(const Project& p, const TimeEdit& teIn, Batch& out) {
  TimeEdit te = teIn;
  te.a = std::max<Ticks>(te.a, 0);
  te.b = std::max<Ticks>(te.b, 0);
  if (te.op != TimeOp::Insert) {
    if (te.b <= te.a) return;
    te.len = te.op == TimeOp::Delete ? te.b - te.a : te.len;
  }
  const TimeSigMap& map = p.timeline->sigMap;
  struct S {
    Ticks tick;
    TimeSignature sig;
    bool created;
  };
  std::vector<S> in;
  for (size_t i = 0; i < map.changeCount(); ++i) in.push_back({map.changeStartTick(i), map.changeSignature(i), false});
  if (in.size() == 1 && te.op != TimeOp::Delete) return;  // one signature throughout: nothing moves
  std::vector<S> moved;
  switch (te.op) {
    case TimeOp::Insert:
      for (const S& s : in) moved.push_back({s.tick > 0 && s.tick >= te.a ? s.tick + te.len : s.tick, s.sig, false});
      break;
    case TimeOp::Delete: {
      const S* lastInside = nullptr;
      bool atB = false;
      for (const S& s : in) {
        if (s.tick >= te.a && s.tick < te.b) {
          lastInside = &s;
        } else {
          atB = atB || s.tick == te.b;
          moved.push_back({s.tick >= te.b ? s.tick - te.len : s.tick, s.sig, false});
        }
      }
      // The signature in force at b continues after the seam.
      if (lastInside && !atB) moved.push_back({te.a, lastInside->sig, true});
      break;
    }
    case TimeOp::Duplicate: {
      bool atA = false, atB = false;
      for (const S& s : in) {
        atA = atA || s.tick == te.a;
        atB = atB || s.tick == te.b;
        moved.push_back({s.tick > 0 && s.tick >= te.b ? s.tick + te.len : s.tick, s.sig, false});
        if (s.tick >= te.a && s.tick < te.b) moved.push_back({s.tick + te.len, s.sig, true});
      }
      if (!atA) moved.push_back({te.b, map.signatureAt(te.a), true});
      if (!atB) moved.push_back({te.b + te.len, map.signatureAt(te.b), true});
      break;
    }
  }
  std::stable_sort(moved.begin(), moved.end(), [](const S& x, const S& y) { return x.tick < y.tick; });
  // Rebuild bar indices, snapping to bar lines; on the same bar the later change wins.
  std::vector<TimeSigChange> outList;
  std::vector<bool> createdFlags;
  Ticks curTick = 0;
  for (const S& s : moved) {
    if (outList.empty()) {
      outList.push_back({0, s.sig});
      createdFlags.push_back(s.created);
      continue;
    }
    const Ticks barLen = ticksPerBar(outList.back().sig);
    const Ticks d = std::max<Ticks>(s.tick - curTick, 0);
    const int64_t bars = (d + barLen - 1) / barLen;
    if (bars == 0) {
      outList.back().sig = s.sig;
      createdFlags.back() = createdFlags.back() && s.created;
      continue;
    }
    outList.push_back({outList.back().bar + bars, s.sig});
    createdFlags.push_back(s.created);
    curTick += bars * barLen;
  }
  // Drop created changes that repeat the signature before them.
  std::vector<TimeSigChange> clean;
  for (size_t i = 0; i < outList.size(); ++i) {
    if (i > 0 && createdFlags[i] && outList[i].sig == clean.back().sig) continue;
    clean.push_back(outList[i]);
  }
  const std::vector<TimeSigChange>& orig = p.timeline->signatures;
  bool same = clean.size() == orig.size();
  for (size_t i = 0; same && i < clean.size(); ++i) same = clean[i].bar == orig[i].bar && clean[i].sig == orig[i].sig;
  if (!same) out.commands.push_back(SetTimeSignatures{std::move(clean)});
}

// ---- All of it ---------------------------------------------------------------------------------------------------

EditResult timeEdit(const Project& p, IdSource& ids, const TimeEdit& te, std::string label) {
  PQ_DOC_TRY(checkTick(te.a, "start"));
  PQ_DOC_TRY(checkTick(te.b, "end"));
  if (te.len < 1) return invalid("the time range is empty");
  PQ_DOC_TRY(checkTick(te.cut() + te.len, "end"));
  const uint32_t firstNew = ids.peek();
  Edit e = makeEdit(std::move(label));
  Batch& b = batchOf(e);
  editClips(p, te, ids, b);
  forEachTrack(p, [&](const Track& t, const Track*) {
    for (const LanePtr& lp : t.lanes) {
      const Points& pts = lp->points;
      if (pts.empty() || pts.back().tick < te.a) continue;  // nothing at or after the edit: unchanged
      Points r = timeEditEnvelope(pts, te, ids, false);
      simplify(r, firstNew, ids);
      emitLaneDiff(b, t.id, *lp, std::move(r));
    }
  });
  editLocators(p, te, b);
  editLoop(p, te, b);
  editTempo(p, te, b);
  editSignatures(p, te, b);
  return e;
}

}  // namespace

EditResult insertTime(const Project& p, IdSource& ids, Ticks at, Ticks length) {
  return timeEdit(p, ids, TimeEdit{TimeOp::Insert, at, at, length}, "Insert Time");
}

EditResult deleteTime(const Project& p, IdSource& ids, Ticks start, Ticks end) {
  return timeEdit(p, ids, TimeEdit{TimeOp::Delete, start, end, end - start}, "Delete Time");
}

EditResult duplicateTime(const Project& p, IdSource& ids, Ticks start, Ticks end) {
  return timeEdit(p, ids, TimeEdit{TimeOp::Duplicate, start, end, end - start}, "Duplicate Time");
}

}  // namespace pq::doc::ops
