// Take lanes and comping (T6.1): pq/document/edit_ops.h "Take lanes"; model.h "Take lanes".

#include <algorithm>
#include <cmath>
#include <limits>

#include "edit_common.h"

namespace pq::doc::ops {

using namespace detail;

namespace {

Result<const Track*> takeTrack(const Project& p, TrackId id) {
  const Track* t = findTrackPtr(p, id);
  if (t == nullptr) return notFound("track not found");
  if (t->kind != TrackKind::Midi && t->kind != TrackKind::Audio) return invalid("only MIDI and audio tracks have take lanes");
  return t;
}

Result<const TakeLane*> takeOf(const Track& t, LaneId lane) {
  const TakeLane* l = t.takeLane(lane);
  if (l == nullptr) return notFound("take lane not found on this track");
  return l;
}


// AR.1: `a`'s touches written over `base` (the lane's points; `fresh`: nothing was written to it yet, so an empty lane
// takes `before` up to the first touch). See RecordedTouch.
Points writeTouches(Points base, const RecordedAutomation& a, IdSource& ids) {
  bool wrote = false;
  for (const RecordedTouch& t : a.touches) {
    std::vector<Breakpoint> s;
    s.reserve(t.samples.size());
    for (Breakpoint b : t.samples) {
      if (!std::isfinite(b.value)) continue;
      b.tick = std::max<Ticks>(b.tick, s.empty() ? Ticks{0} : s.back().tick);
      if (!s.empty() && b.tick == s.back().tick) s.back().value = b.value;  // one tick: the last value counts
      else s.push_back(b);
    }
    if (s.empty()) continue;
    s = thinAutomation(s, a.tolerance);
    const uint32_t firstNew = ids.peek();
    const Ticks t0 = s.front().tick;
    const float last = s.back().value;
    Points r;
    if (!base.empty()) r = envBefore(base, t0, ids, true);
    else if (!wrote) r.push_back(Breakpoint{t0, static_cast<float>(a.before), 0.0f, ids.breakpoint()});
    for (Breakpoint b : s) {
      b.curve = 0.0f;
      b.id = ids.breakpoint();
      r.push_back(b);
    }
    Ticks t1 = std::max(t.end, s.back().tick);
    bool resume = !base.empty();
    if (t.latch) {  // held up to the lane's next point after it (none: for good)
      const auto next = std::upper_bound(base.begin(), base.end(), t0, [](Ticks v, const Breakpoint& b) { return v < b.tick; });
      resume = next != base.end();
      t1 = resume ? next->tick : s.back().tick;
    }
    if (t1 > s.back().tick) r.push_back(Breakpoint{t1, last, 0.0f, ids.breakpoint()});
    if (resume) append(r, envFrom(base, t1, ids));
    simplify(r, firstNew, ids);
    base = std::move(r);
    wrote = true;
  }
  std::sort(base.begin(), base.end(), pointLess);
  return base;
}

}  // namespace

std::vector<Breakpoint> thinAutomation(std::span<const Breakpoint> s, double tolerance) {
  const size_t n = s.size();
  if (n <= 2) return {s.begin(), s.end()};
  std::vector<uint8_t> keep(n, 0);
  keep[0] = keep[n - 1] = 1;
  std::vector<std::pair<size_t, size_t>> todo{{0, n - 1}};
  while (!todo.empty()) {
    const auto [a, b] = todo.back();
    todo.pop_back();
    if (b <= a + 1) continue;
    const double ta = static_cast<double>(s[a].tick), span = static_cast<double>(s[b].tick) - ta;
    const double va = s[a].value, dv = static_cast<double>(s[b].value) - va;
    double worst = -1.0;
    size_t at = a;
    for (size_t i = a + 1; i < b; ++i) {
      const double x = span > 0.0 ? (static_cast<double>(s[i].tick) - ta) / span : 0.0;
      const double d = std::fabs(static_cast<double>(s[i].value) - (va + dv * x));
      if (d > worst) {
        worst = d;
        at = i;
      }
    }
    if (worst > tolerance) {
      keep[at] = 1;
      todo.push_back({a, at});
      todo.push_back({at, b});
    }
  }
  std::vector<Breakpoint> out;
  for (size_t i = 0; i < n; ++i)
    if (keep[i] != 0) out.push_back(s[i]);
  return out;
}

EditResult addTakeLane(const Project& p, IdSource& ids, TrackId track, std::span<const ClipSlot> clips, std::string name) {
  Result<const Track*> t = takeTrack(p, track);
  if (!t) return std::move(t).error();
  auto lane = std::make_shared<TakeLane>();
  lane->id = ids.lane();
  lane->name = name.empty() ? "Take " + std::to_string((*t)->takes.size() + 1) : std::move(name);
  Edit e = makeEdit("Add Take Lane");
  lane->clips.reserve(clips.size());
  for (const ClipSlot& c : clips) {
    if ((c.audio != nullptr) == (c.clip != nullptr)) return invalid("a clip slot holds exactly one clip");
    ClipSlot fresh = cloneSlot(c, ids, false);  // fresh clip (and note) ids: the lane never shares an id
    e.clips.push_back(ClipRef{track, fresh.id});
    lane->clips.push_back(std::move(fresh));
  }
  std::sort(lane->clips.begin(), lane->clips.end(), [](const ClipSlot& a, const ClipSlot& b) {
    return a.start() != b.start() ? a.start() < b.start() : a.id < b.id;
  });
  e.tracks.push_back(track);
  batchOf(e).commands.push_back(AddTakeLane{track, std::move(lane)});
  return e;
}

EditResult removeTakeLane(const Project& p, TrackId track, LaneId lane) {
  Result<const Track*> t = takeTrack(p, track);
  if (!t) return std::move(t).error();
  if (Result<const TakeLane*> l = takeOf(**t, lane); !l) return std::move(l).error();
  Edit e = makeEdit("Delete Take Lane");
  e.tracks.push_back(track);
  batchOf(e).commands.push_back(RemoveTakeLane{track, lane});
  return e;
}

EditResult renameTakeLane(const Project& p, TrackId track, LaneId lane, std::string name) {
  Result<const Track*> t = takeTrack(p, track);
  if (!t) return std::move(t).error();
  Result<const TakeLane*> l = takeOf(**t, lane);
  if (!l) return std::move(l).error();
  Edit e = makeEdit("Rename Take Lane");
  e.tracks.push_back(track);
  if ((*l)->name != name) batchOf(e).commands.push_back(RenameTakeLane{track, lane, std::move(name)});
  return e;
}

EditResult setAuditionTake(const Project& p, TrackId track, LaneId lane) {
  Result<const Track*> t = takeTrack(p, track);
  if (!t) return std::move(t).error();
  if (lane.valid())
    if (Result<const TakeLane*> l = takeOf(**t, lane); !l) return std::move(l).error();
  Edit e = makeEdit("Audition Take");
  e.tracks.push_back(track);
  if ((*t)->auditionTake != lane) batchOf(e).commands.push_back(SetAuditionTake{track, lane});
  return e;
}

EditResult promoteTake(const Project& p, TrackId track, LaneId lane) {
  Result<const Track*> t = takeTrack(p, track);
  if (!t) return std::move(t).error();
  if (Result<const TakeLane*> l = takeOf(**t, lane); !l) return std::move(l).error();
  Edit e = makeEdit("Promote Take");
  e.tracks.push_back(track);
  batchOf(e).commands.push_back(PromoteTake{track, lane});
  return e;
}

EditResult compRegion(const Project& p, IdSource& ids, TrackId track, LaneId lane, Ticks t0, Ticks t1, Ticks crossfade) {
  PQ_DOC_TRY(checkTick(t0, "comp start"));
  PQ_DOC_TRY(checkTick(t1, "comp end"));
  if (t1 <= t0) return invalid("a comp region needs start < end");
  if (crossfade < 0) return invalid("crossfade must not be negative");
  Result<const Track*> tr = takeTrack(p, track);
  if (!tr) return std::move(tr).error();
  const Track& t = **tr;
  Result<const TakeLane*> take = takeOf(t, lane);
  if (!take) return std::move(take).error();

  Edit e = makeEdit("Comp Take");
  e.tracks.push_back(track);
  ClipEditor ed(p, t);
  ed.clearRange(t0, t1, ids);  // what the main lane had there goes (cut, the rest trimmed; ids for right pieces)
  std::vector<ClipId> pieces;
  for (const ClipSlot& c : (*take)->clips) {
    if (c.end() <= t0 || c.start() >= t1) continue;
    const Ticks s = std::max(c.start(), t0), en = std::min(c.end(), t1);
    ClipSlot copy = cloneSlot(c, ids, false);  // fresh ids; every note / source frame kept (bounded below)
    copy.take = lane;
    ClipSlot piece = ed.bounded(copy, s, en);
    if (piece.audio) {  // a piece cut inside the take clip loses that edge's fade
      AudioClip a = *piece.audio;
      if (s > c.start()) a.fadeIn = {};
      if (en < c.end()) a.fadeOut = {};
      a.crossfadeIn = s > c.start() ? 0 : a.crossfadeIn;  // a take-internal crossfade stays where its neighbour does
      piece.audio = std::make_shared<const AudioClip>(std::move(a));
    }
    pieces.push_back(piece.id);
    ed.add(std::move(piece));
  }
  // Audio: crossfade the region's two edges where a clip meets a clip exactly there (centred on the edge; clamped to
  // both clips as setAudioClipCrossfade does).
  if (t.kind == TrackKind::Audio && crossfade > 0) {
    std::vector<ClipSlot>& v = ed.clips();
    std::sort(v.begin(), v.end(), [](const ClipSlot& a, const ClipSlot& b) {
      return a.start() != b.start() ? a.start() < b.start() : a.id < b.id;
    });
    for (size_t i = 1; i < v.size(); ++i) {
      const Ticks at = v[i].start();
      if ((at != t0 && at != t1) || v[i - 1].end() != at || !v[i].audio) continue;
      AudioClip a = *v[i].audio;
      a.crossfadeIn = std::min({crossfade, a.length, at - v[i - 1].start()});
      v[i].audio = std::make_shared<const AudioClip>(std::move(a));
    }
  }
  Batch removes, updates, adds;
  ed.emit(removes, updates, adds);
  Batch& b = batchOf(e);
  for (Batch* part : {&removes, &updates, &adds})
    for (Command& c : part->commands) b.commands.push_back(std::move(c));
  for (const ClipId id : pieces) e.clips.push_back(ClipRef{track, id});
  return e;
}

namespace {

// Overdub (MIDI): the notes (absolute ticks) into the main lane (see commitRecording).
void overdubNotes(ClipEditor& ed, IdSource& ids, std::vector<Note> notes, const Track& t, Ticks recStart, Ticks recEnd) {
  std::vector<ClipSlot>& v = ed.clips();
  std::sort(v.begin(), v.end(), [](const ClipSlot& a, const ClipSlot& b) {
    return a.start() != b.start() ? a.start() < b.start() : a.id < b.id;
  });
  std::sort(notes.begin(), notes.end(), [](const Note& a, const Note& b) { return a.start < b.start; });
  std::vector<std::vector<Note>> into(v.size());
  std::vector<std::pair<size_t, Note>> orphans;  // (gap index = clips starting at or before it, note)
  for (const Note& n : notes) {
    size_t host = v.size(), gap = 0;
    for (size_t i = 0; i < v.size(); ++i) {
      if (v[i].start() <= n.start) gap = i + 1;
      if (v[i].clip && v[i].start() <= n.start && n.start < v[i].end()) host = i;
    }
    if (host < v.size()) into[host].push_back(n);
    else orphans.emplace_back(gap, n);
  }
  for (size_t i = 0; i < v.size(); ++i) {
    if (into[i].empty()) continue;
    const NoteClip& c = *v[i].clip;
    std::vector<Note> all(c.notes.begin(), c.notes.end());
    for (Note n : into[i]) {
      n.start = n.start - c.start + c.contentStart;
      n.length = std::max<Ticks>(1, std::min(n.length, c.start + c.length - (n.start - c.contentStart + c.start)));
      n.id = ids.note();
      all.push_back(n);
    }
    v[i].clip = std::make_shared<const NoteClip>(withNotes(c, seqOf(std::move(all))));
  }
  for (size_t k = 0; k < orphans.size();) {
    const size_t gap = orphans[k].first;
    size_t e = k;
    while (e < orphans.size() && orphans[e].first == gap) ++e;
    // The new clip spans the recorded range inside this gap (between the main lane's clips around it).
    const Ticks limit = gap < v.size() ? v[gap].start() : std::numeric_limits<Ticks>::max();
    const Ticks floor = gap > 0 ? v[gap - 1].end() : std::numeric_limits<Ticks>::min();
    const Ticks s = std::min(std::max(recStart, floor), orphans[k].second.start);
    Ticks end = std::max(recEnd, s + 1);
    for (size_t j = k; j < e; ++j) end = std::max(end, orphans[j].second.start + orphans[j].second.length);
    end = std::min(end, limit);
    NoteClip c;
    c.id = ids.clip();
    c.name = t.name;
    c.colour = t.colour;
    c.start = s;
    c.length = std::max<Ticks>(1, end - s);
    std::vector<Note> ns;
    for (size_t j = k; j < e; ++j) {
      Note n = orphans[j].second;
      n.start -= s;
      n.length = std::max<Ticks>(1, std::min(n.length, c.length - n.start));
      n.id = ids.note();
      ns.push_back(n);
    }
    c.notes = seqOf(std::move(ns));
    v.push_back(ClipSlot{c.id, std::make_shared<const NoteClip>(std::move(c)), nullptr});
    k = e;
  }
}

}  // namespace

EditResult commitRecording(const Project& p, IdSource& ids, const RecordingCommit& commit) {
  Edit e = makeEdit("Record");
  Batch& b = batchOf(e);
  for (const SampleRef& s : commit.samples) {
    PQ_DOC_TRY(checkSampleRef(s));
    if (findSample(p, s.id)) return invalid("sample id already in use");
    b.commands.push_back(AddSample{s});
  }
  auto sampleKnown = [&](SampleId id) {
    for (const SampleRef& s : commit.samples)
      if (s.id == id) return true;
    return findSample(p, id) != nullptr;
  };
  ClipEdits main(p);
  for (const RecordedTrack& rt : commit.tracks) {
    Result<const Track*> tr = takeTrack(p, rt.track);
    if (!tr) return std::move(tr).error();
    const Track& t = **tr;
    if (rt.passes.empty()) continue;
    e.tracks.push_back(rt.track);
    std::vector<ClipSlot> passes;
    for (const ClipSlot& c : rt.passes) {
      if ((c.audio != nullptr) == (c.clip != nullptr)) return invalid("a clip slot holds exactly one clip");
      if ((c.audio != nullptr) != (t.kind == TrackKind::Audio)) return invalid("a recorded clip of the wrong kind");
      ClipSlot s = c;
      if (s.audio) {
        if (!sampleKnown(s.audio->sample)) return notFound("unknown sample id " + std::to_string(s.audio->sample.value));
        AudioClip a = *s.audio;
        a.warp = AudioWarp::Off;
        a = syncedClip(*p.timeline, std::move(a));
        PQ_DOC_TRY(checkTick(a.start, "clip start"));
        PQ_DOC_TRY(checkTick(clipEnd(a), "clip end"));
        s.audio = std::make_shared<const AudioClip>(std::move(a));
      } else {
        PQ_DOC_TRY(checkTick(s.clip->start, "clip start"));
        for (const Note& n : s.clip->notes) PQ_DOC_TRY(validateNote(n));
      }
      passes.push_back(std::move(s));
    }
    ClipEditor& ed = *main.editor(rt.track).value();
    if (rt.overdub && t.kind == TrackKind::Midi) {
      std::vector<Note> notes;
      Ticks recStart = std::numeric_limits<Ticks>::max(), recEnd = std::numeric_limits<Ticks>::min();
      for (const ClipSlot& c : passes) {
        recStart = std::min(recStart, c.start());
        recEnd = std::max(recEnd, c.end());
        for (Note n : c.clip->notes) {
          n.start += c.clip->start - c.clip->contentStart;
          notes.push_back(n);
        }
      }
      overdubNotes(ed, ids, std::move(notes), t, recStart, recEnd);
      continue;
    }
    LaneId last;
    for (size_t i = 0; i < passes.size(); ++i) {
      auto lane = std::make_shared<TakeLane>();
      lane->id = ids.lane();
      lane->name = "Take " + std::to_string(t.takes.size() + i + 1);
      ClipSlot fresh = cloneSlot(passes[i], ids, false);
      e.clips.push_back(ClipRef{rt.track, fresh.id});
      lane->clips.push_back(std::move(fresh));
      last = lane->id;
      b.commands.push_back(AddTakeLane{rt.track, std::move(lane)});
    }
    ClipSlot inMain = cloneSlot(passes.back(), ids, false);
    inMain.take = last;
    ed.clearRange(inMain.start(), inMain.end(), ids);
    e.clips.push_back(ClipRef{rt.track, inMain.id});
    ed.add(std::move(inMain));
  }
  main.emit(b);
  // AR.1: the automation written while recording, a parameter at a time, each checked on a scratch document: a
  // parameter that cannot have a lane (its device or tuning went meanwhile) is left out, the take still commits.
  Snapshot scratch(Snapshot{}, &p);  // non-owning: p outlives this call
  for (const RecordedAutomation& a : commit.automation) {
    if (a.touches.empty() || !isAutomatable(a.param)) continue;
    const Track* home = nullptr;
    const AutomationLane* lane = nullptr;
    forEachTrack(*scratch, [&](const Track& t, const Track*) {
      for (const LanePtr& l : t.lanes)
        if (lane == nullptr && l->param == a.param) {
          home = &t;
          lane = l.get();
        }
    });
    Batch mine;
    if (lane != nullptr) {
      emitLaneDiff(mine, home->id, *lane, writeTouches(lane->points, a, ids));
    } else {
      TrackId at = a.home;
      if (isTrackParam(a.param.kind)) at = TrackId{a.param.owner};
      else if (isDeviceParamKind(a.param.kind)) at = laneHome(*scratch, a.param) ? laneHome(*scratch, a.param)->id : TrackId{};
      if (!at.valid() || findTrackPtr(*scratch, at) == nullptr) continue;
      auto l = std::make_shared<AutomationLane>();
      l->id = ids.lane();
      l->param = a.param;
      l->paramName = a.paramName;
      l->points = writeTouches({}, a, ids);
      mine.commands.push_back(AddAutomationLane{at, std::move(l)});
    }
    if (mine.commands.empty()) continue;
    ChangeSet cs;
    Result<Snapshot> next = apply(scratch, mine, cs);
    if (!next) continue;
    scratch = std::move(next).value();
    for (Command& c : mine.commands) b.commands.push_back(std::move(c));
  }
  return e;
}

}  // namespace pq::doc::ops
