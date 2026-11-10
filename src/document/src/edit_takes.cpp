// Take lanes and comping (T6.1): pq/document/edit_ops.h "Take lanes"; model.h "Take lanes".

#include <algorithm>

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

}  // namespace

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

}  // namespace pq::doc::ops
