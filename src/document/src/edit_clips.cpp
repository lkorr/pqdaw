// Clip edit operations (T3.6).

#include <algorithm>
#include <unordered_set>

#include "edit_common.h"

namespace pq::doc::ops {

using namespace detail;

namespace {

Status checkRange(Ticks a, Ticks b) {
  PQ_DOC_TRY(checkTick(a, "start"));
  PQ_DOC_TRY(checkTick(b, "end"));
  if (b <= a) return invalid("the range is empty");
  return ok();
}

// Same-track clip moves of one track, for the automation follow.
struct RangeMove {
  Ticks from = 0;
  Ticks to = 0;  // new start
  Ticks length = 0;
};

// Moves (or copies) each lane envelope of `t` over the source ranges to the destinations (exact, see
// automation_curve.h).
void followAutomation(const Track& t, const std::vector<RangeMove>& moves, bool copy, IdSource& ids,
                      uint32_t firstNew, Batch& out) {
  for (const LanePtr& lp : t.lanes) {
    const AutomationLane& lane = *lp;
    if (lane.points.empty()) continue;
    std::vector<Points> slices;
    slices.reserve(moves.size());
    for (size_t i = 0; i < moves.size(); ++i) {
      const RangeMove& m = moves[i];
      // Overlapping sources (overlapping clips) would share points: later slices get fresh ids.
      bool overlaps = copy;
      for (size_t j = 0; j < i; ++j)
        overlaps = overlaps || (moves[j].from < m.from + m.length && m.from < moves[j].from + moves[j].length);
      slices.push_back(envSlice(lane.points, m.from, m.from + m.length, ids, overlaps));
    }
    Points r = lane.points;
    if (!copy) {
      // The vacated range holds the value it had on its left (then the envelope after it resumes).
      for (const RangeMove& m : moves) {
        Points next = envBefore(r, m.from, ids, true);
        Points tail = envFrom(r, m.from + m.length, ids);
        if (!next.empty() && !tail.empty())
          next.push_back(Breakpoint{m.from + m.length, next.back().value, 0.0f, ids.breakpoint()});
        append(next, tail);
        r = std::move(next);
      }
    }
    for (size_t i = 0; i < moves.size(); ++i) {
      const RangeMove& m = moves[i];
      Points next = envBefore(r, m.to, ids, true);
      Points s = slices[i];
      shiftPoints(s, m.to - m.from);
      append(next, s);
      append(next, envFrom(r, m.to + m.length, ids));
      r = std::move(next);
    }
    simplify(r, firstNew, ids);
    emitLaneDiff(out, t.id, lane, std::move(r));
  }
}

}  // namespace

EditResult insertEmptyClip(const Project& p, IdSource& ids, TrackId track, Ticks start, Ticks end, std::string name) {
  PQ_DOC_TRY(checkRange(start, end));
  ClipEdits ed(p);
  Result<ClipEditor*> te = ed.editor(track);
  if (!te) return std::move(te).error();
  if ((*te)->track().kind != TrackKind::Midi) return invalid("MIDI clips live on MIDI tracks");
  (*te)->clearRange(start, end, ids);
  NoteClip c;
  c.id = ids.clip();
  c.name = std::move(name);
  c.colour = (*te)->track().colour;
  c.start = start;
  c.length = end - start;
  c.loop = ClipLoop{true, 0, end - start};
  (*te)->add(ClipSlot{c.id, std::make_shared<const NoteClip>(c), nullptr});
  Edit e = makeEdit("Insert MIDI Clip");
  ed.emit(batchOf(e));
  e.clips.push_back({track, c.id});
  return e;
}

EditResult deleteClips(const Project& p, std::span<const ClipRef> clips) {
  Edit e = makeEdit(clips.size() == 1 ? "Delete Clip" : "Delete Clips");
  std::vector<ClipId> done;
  for (const ClipRef& r : clips) {
    Result<ClipFound> f = findAnyClip(p, r);
    if (!f) return std::move(f).error();
    if (std::find(done.begin(), done.end(), r.clip) != done.end()) continue;
    done.push_back(r.clip);
    batchOf(e).commands.push_back(RemoveClip{r});
  }
  return e;
}

EditResult moveClips(const Project& p, IdSource& ids, std::span<const ClipMove> moves, MoveClipsOptions opt) {
  const uint32_t firstNew = ids.peek();
  ClipEdits ed(p);
  struct M {
    ClipSlot src;
    ClipEditor* from;
    ClipEditor* to;
    Ticks start;
  };
  std::vector<M> ms;
  ms.reserve(moves.size());
  for (const ClipMove& mv : moves) {
    Result<ClipFound> f = findAnyClip(p, mv.clip);
    if (!f) return std::move(f).error();
    PQ_DOC_TRY(checkTick(mv.start, "clip start"));
    for (const M& m : ms)
      if (m.src.id == mv.clip.clip) return invalid("a clip is listed twice");
    Result<ClipEditor*> from = ed.editor(mv.clip.track);
    if (!from) return std::move(from).error();
    Result<ClipEditor*> to = ed.editor(mv.toTrack.valid() ? mv.toTrack : mv.clip.track);
    if (!to) return std::move(to).error();
    if ((*to)->track().kind != (*from)->track().kind)  // T5.3
      return invalid(f->audio ? "audio clips live on audio tracks" : "note clips live on MIDI tracks");
    ms.push_back({f->slot(), *from, *to, mv.start});
  }
  Edit e = makeEdit(opt.copy ? (ms.size() == 1 ? "Copy Clip" : "Copy Clips") : (ms.size() == 1 ? "Move Clip" : "Move Clips"));
  if (!opt.copy)
    for (const M& m : ms) m.from->remove(m.src.id);
  std::vector<ClipSlot> placed;
  placed.reserve(ms.size());
  for (const M& m : ms) {
    const ClipSlot c = m.to->moved(opt.copy ? cloneSlot(m.src, ids, false) : m.src, m.start);
    m.to->clearRange(c.start(), c.end(), ids);
    placed.push_back(c);
  }
  for (size_t i = 0; i < ms.size(); ++i) {
    ms[i].to->add(placed[i]);
    e.clips.push_back({ms[i].to->id(), placed[i].id});
  }
  ed.emit(batchOf(e));

  if (opt.moveAutomation) {
    // Per track, the clips that stay on it take their envelopes along.
    std::vector<const Track*> tracks;
    for (const M& m : ms)
      if (m.from == m.to && std::find(tracks.begin(), tracks.end(), &m.from->track()) == tracks.end())
        tracks.push_back(&m.from->track());
    for (const Track* t : tracks) {
      std::vector<RangeMove> rm;
      for (const M& m : ms)
        if (m.from == m.to && &m.from->track() == t && m.start != m.src.start())
          rm.push_back({m.src.start(), m.start, m.src.length()});
      if (!rm.empty()) followAutomation(*t, rm, opt.copy, ids, firstNew, batchOf(e));
    }
  }
  return e;
}

EditResult moveClipsBy(const Project& p, IdSource& ids, std::span<const ClipRef> clips, Ticks delta,
                       MoveClipsOptions opt) {
  std::vector<ClipMove> moves;
  moves.reserve(clips.size());
  for (const ClipRef& r : clips) {
    Result<ClipFound> f = findAnyClip(p, r);
    if (!f) return std::move(f).error();
    moves.push_back({r, r.track, f->slot().start() + delta});
  }
  return moveClips(p, ids, moves, opt);
}

EditResult resizeClips(const Project& p, IdSource& ids, std::span<const ClipRef> clips, ClipEdge edge, Ticks delta) {
  ClipEdits ed(p);
  struct R {
    ClipEditor* ed;
    Ticks oldStart, oldEnd, start, end;
  };
  std::vector<R> rs;
  std::vector<ClipId> keep;
  Edit e = makeEdit(clips.size() == 1 ? "Resize Clip" : "Resize Clips");
  for (const ClipRef& r : clips) {
    Result<ClipFound> f = findAnyClip(p, r);
    if (!f) return std::move(f).error();
    Result<ClipEditor*> te = ed.editor(r.track);
    if (!te) return std::move(te).error();
    const ClipSlot c = f->slot();
    Ticks s = c.start(), en = c.end();
    if (edge == ClipEdge::Start)
      s = std::min(c.start() + delta, en - 1);
    else
      en = std::max(en + delta, c.start() + 1);
    PQ_DOC_TRY(checkTick(s, "clip start"));
    PQ_DOC_TRY(checkTick(en, "clip end"));
    (*te)->replace(c.id, (*te)->bounded(c, s, en));
    rs.push_back({*te, c.start(), c.end(), s, en});
    keep.push_back(c.id);
    e.clips.push_back(r);
  }
  for (const R& r : rs) {
    if (r.start < r.oldStart) r.ed->clearRange(r.start, r.oldStart, ids, keep);
    if (r.end > r.oldEnd) r.ed->clearRange(r.oldEnd, r.end, ids, keep);
  }
  ed.emit(batchOf(e));
  return e;
}

EditResult splitClips(const Project& p, IdSource& ids, std::span<const ClipRef> clips, std::span<const Ticks> at) {
  std::vector<Ticks> cuts(at.begin(), at.end());
  std::sort(cuts.begin(), cuts.end());
  cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
  ClipEdits ed(p);
  Edit e = makeEdit("Split");
  std::vector<ClipId> done;
  for (const ClipRef& r : clips) {
    Result<ClipFound> f = findAnyClip(p, r);
    if (!f) return std::move(f).error();
    if (std::find(done.begin(), done.end(), r.clip) != done.end()) continue;
    done.push_back(r.clip);
    Result<ClipEditor*> te = ed.editor(r.track);
    if (!te) return std::move(te).error();
    ClipSlot piece = f->slot();
    bool first = true;
    for (Ticks t : cuts) {
      if (t <= piece.start() || t >= piece.end()) continue;
      auto [l, rr] = (*te)->split(piece, t, ids.clip(), ids);
      e.clips.push_back({r.track, l.id});
      if (first) {
        (*te)->replace(r.clip, std::move(l));
        first = false;
      } else {
        (*te)->add(std::move(l));
      }
      piece = std::move(rr);
    }
    if (first) {  // not cut: still part of the selection
      e.clips.push_back(r);
      continue;
    }
    e.clips.push_back({r.track, piece.id});
    (*te)->add(std::move(piece));
  }
  ed.emit(batchOf(e));
  return e;
}

EditResult duplicateClips(const Project& p, IdSource& ids, std::span<const ClipRef> clips) {
  std::vector<ClipFound> found;
  Ticks s = kMaxTicks, en = -kMaxTicks;
  for (const ClipRef& r : clips) {
    Result<ClipFound> f = findAnyClip(p, r);
    if (!f) return std::move(f).error();
    if (std::any_of(found.begin(), found.end(), [&](const ClipFound& x) { return x.slot().id == r.clip; })) continue;
    s = std::min(s, f->slot().start());
    en = std::max(en, f->slot().end());
    found.push_back(std::move(f).value());
  }
  Edit e = makeEdit(found.size() == 1 ? "Duplicate Clip" : "Duplicate Clips");
  if (found.empty()) return e;
  const Ticks len = en - s;
  PQ_DOC_TRY(checkTick(en + len, "clip end"));
  ClipEdits ed(p);
  std::vector<std::pair<ClipEditor*, ClipSlot>> copies;
  for (const ClipFound& f : found) {
    ClipEditor& te = ed.editor(*f.track);
    const ClipSlot c = te.moved(cloneSlot(f.slot(), ids, false), f.slot().start() + len);
    te.clearRange(c.start(), c.end(), ids);
    copies.emplace_back(&te, c);
  }
  for (auto& [te, c] : copies) {
    e.clips.push_back({te->id(), c.id});
    te->add(std::move(c));
  }
  ed.emit(batchOf(e));
  return e;
}

EditResult consolidateClips(const Project& p, IdSource& ids, std::span<const ClipRef> clips) {
  // Group the selection by track (in selection order of first appearance).
  struct Group {
    const Track* track;
    Ticks start = kMaxTicks;
    Ticks end = -kMaxTicks;
  };
  std::vector<Group> groups;
  for (const ClipRef& r : clips) {
    Result<ClipFound> f = findAnyClip(p, r);
    if (!f) return std::move(f).error();
    if (f->audio) return unsupported("consolidating audio clips is not supported yet (it renders them; SPEC §9.4)");
    auto it = std::find_if(groups.begin(), groups.end(), [&](const Group& g) { return g.track == f->track; });
    if (it == groups.end()) {
      groups.push_back({f->track});
      it = groups.end() - 1;
    }
    it->start = std::min(it->start, f->clip->start);
    it->end = std::max(it->end, clipEnd(*f->clip));
  }
  Edit e = makeEdit("Consolidate");
  ClipEdits ed(p);
  std::unordered_set<uint32_t> seen;
  for (const Group& g : groups) {
    std::vector<const NoteClip*> merge;
    for (const ClipSlot& s : g.track->clips)
      if (s.clip->start >= g.start && clipEnd(*s.clip) <= g.end) merge.push_back(s.clip.get());
    const NoteClip& first = *merge.front();
    for (const NoteClip* c : merge)
      if (!(c->tuning == first.tuning)) return invalid("the clips use different tunings");
    std::vector<Note> notes;
    for (const NoteClip* c : merge) {
      forEachClipSegment(*c, c->start, clipEnd(*c), [&](const ClipSegment& seg) {
        for (auto [it, end] = c->notes.range(seg.contentFrom, seg.contentTo); it != end; ++it) {
          Note m = *it;
          m.start = seg.at + (it->start - seg.contentFrom) - g.start;
          m.length = std::min(it->length, seg.contentTo - it->start);
          if (!c->active) m.flags |= kNoteDisabled;
          if (!seen.insert(it->id.value).second) m.id = ids.note();
          notes.push_back(m);
        }
      });
    }
    NoteClip c = withNotes(first, seqOf(std::move(notes)));
    c.id = ids.clip();
    c.start = g.start;
    c.length = g.end - g.start;
    c.contentStart = 0;
    c.loop = ClipLoop{true, 0, c.length};
    c.active = true;
    ClipEditor& te = ed.editor(*g.track);
    te.clearRange(g.start, g.end, ids);
    e.clips.push_back({g.track->id, c.id});
    te.add(ClipSlot{c.id, std::make_shared<const NoteClip>(std::move(c)), nullptr});
  }
  ed.emit(batchOf(e));
  return e;
}

EditResult loopClipToSelection(const Project& p, ClipRef clip, Ticks contentStart, Ticks contentEnd) {
  PQ_DOC_TRY(checkRange(contentStart, contentEnd));
  Result<ClipFound> f = findClip(p, clip);
  if (!f) return std::move(f).error();
  Edit e = makeEdit("Loop Selection");
  batchOf(e).commands.push_back(SetClipLoop{clip, ClipLoop{true, contentStart, contentEnd}});
  e.clips.push_back(clip);
  return e;
}

EditResult loopArrangementToSelection(const Project&, Ticks start, Ticks end) {
  PQ_DOC_TRY(checkRange(start, end));
  Edit e = makeEdit("Loop Selection");
  batchOf(e).commands.push_back(SetLoop{LoopRegion{start, end, true}});
  return e;
}

}  // namespace pq::doc::ops
