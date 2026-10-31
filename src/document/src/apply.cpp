// Command application (T3.4). Each command kind has one apply_ overload below. The pattern is always:
//   1. look up and validate against the input snapshot (nothing copied yet);
//   2. return the input snapshot unchanged if the command would not change anything;
//   3. copy the Project (a handful of pointers and two small vectors), path-copy down to the edited node with
//      mutableTrack()/mutableList(), edit the fresh copies, and record the ChangeSet.
// Nothing reachable from the input snapshot is ever written.

#include <algorithm>
#include <deque>
#include <array>
#include <cmath>
#include <string>
#include <unordered_set>

#include "pq/document/audio_clip.h"
#include "pq/document/commands.h"
#include "pq/document/pitch_resolve.h"
#include "checks.h"

namespace pq::doc {

namespace {

using Res = Result<Snapshot>;

using namespace detail;  // validation helpers shared with the project loader (checks.h)

// ---- Path-copying helpers ----------------------------------------------------------------------------------------

std::shared_ptr<Project> copyOf(const Snapshot& s) { return std::make_shared<Project>(*s); }

std::vector<TrackPtr>& rootList(Project& np, TrackList l) { return l == TrackList::Returns ? np.returns : np.tracks; }

// A fresh, mutable copy of the track at `path` installed in np, with all its ancestors copied too.
Track& mutableTrack(Project& np, const TrackPath& path) {
  if (path.list == TrackList::Master) {
    auto c = std::make_shared<Track>(*np.master);
    Track& r = *c;
    np.master = std::move(c);
    return r;
  }
  std::vector<TrackPtr>* list = &rootList(np, path.list);
  Track* raw = nullptr;
  for (uint32_t l = 0; l < path.depth; ++l) {
    auto c = std::make_shared<Track>(*(*list)[path.index[l]]);
    raw = c.get();
    (*list)[path.index[l]] = std::move(c);
    list = &raw->children;
  }
  return *raw;
}

// The child list that `parent` (depth 0 = the root list `l`) owns, made mutable.
std::vector<TrackPtr>& mutableList(Project& np, TrackList l, const TrackPath& parent) {
  if (parent.depth == 0) return rootList(np, l);
  return mutableTrack(np, parent).children;
}

TrackPath rootPath(TrackList l) {
  TrackPath p;
  p.list = l;
  p.depth = 0;
  return p;
}

uint32_t treeHeight(const Track& t) {
  uint32_t h = 0;
  for (const TrackPtr& c : t.children) h = std::max(h, treeHeight(*c));
  return h + 1;
}

bool isAncestorPath(const TrackPath& a, const TrackPath& b) {
  if (a.list != b.list || a.depth >= b.depth) return false;
  for (uint32_t i = 0; i < a.depth; ++i)
    if (a.index[i] != b.index[i]) return false;
  return true;
}

bool pathLess(const TrackPath& a, const TrackPath& b) {
  const uint32_t n = std::min(a.depth, b.depth);
  for (uint32_t i = 0; i < n; ++i)
    if (a.index[i] != b.index[i]) return a.index[i] < b.index[i];
  return a.depth < b.depth;
}

void collectIds(const Track& t, ChangeSet& cs) {
  cs.tracks.push_back(t.id);
  for (const ClipSlot& c : t.clips) cs.clips.push_back(c.id);
  for (const LanePtr& l : t.lanes) cs.lanes.push_back(l->id);
  forEachDeviceIn(t.devices, [&](const Device& d) { cs.devices.push_back(d.id); });  // T4.8: rack contents too
  for (const TrackPtr& c : t.children) collectIds(*c, cs);
}

// T4.8: the ids of `list`'s devices (racks: their chains, mappings and inner devices) into `ids`; none may name an
// entity of its kind that exists in `p`.
Status checkNewDeviceIds(const Project& p, const std::vector<DevicePtr>& list, std::vector<uint32_t>& ids) {
  // The project's device, chain and mapping ids once (M4 review M9: a lookup per new id walked the whole project).
  std::unordered_set<uint32_t> devices, chains, mappings;
  forEachDevice(p, [&](const Device& d, const Track&) {
    devices.insert(d.id.value);
    if (!d.rack) return;
    for (const ChainPtr& c : d.rack->chains) chains.insert(c->id.value);
    for (const MacroMapping& m : d.rack->mappings) mappings.insert(m.id.value);
  });
  Status st = ok();
  forEachDeviceIn(list, [&](const Device& d) {
    if (!st) return;
    if (devices.count(d.id.value) != 0) st = invalid("device id already in use");
    ids.push_back(d.id.value);
    if (!d.rack) return;
    for (const ChainPtr& c : d.rack->chains) {
      if (chains.count(c->id.value) != 0) st = invalid("chain id already in use");
      ids.push_back(c->id.value);
    }
    for (const MacroMapping& m : d.rack->mappings) {
      if (mappings.count(m.id.value) != 0) st = invalid("mapping id already in use");
      ids.push_back(m.id.value);
    }
  });
  return st;
}

// T4.7: drops the wires touching a removed device (defined with the wire commands below).
void dropWiresOf(Project& np, const std::vector<DeviceId>& gone, ChangeSet& cs);

// Rebuilds every track for which fn returns a modified copy (and the ancestors of those), sharing the rest.
// fn: std::optional<Track>(const Track&). Modified tracks are recorded in cs.tracks.
template <class Fn>
TrackPtr rewrite(const TrackPtr& t, Fn& fn, ChangeSet& cs) {
  std::vector<TrackPtr> kids;
  bool kidsChanged = false;
  for (size_t i = 0; i < t->children.size(); ++i) {
    if (TrackPtr r = rewrite(t->children[i], fn, cs)) {
      if (!kidsChanged) {
        kids = t->children;
        kidsChanged = true;
      }
      kids[i] = std::move(r);
    }
  }
  std::optional<Track> mod = fn(*t);
  if (!mod && !kidsChanged) return nullptr;
  Track copy = mod ? std::move(*mod) : Track(*t);
  if (kidsChanged) copy.children = std::move(kids);
  if (mod) cs.tracks.push_back(t->id);
  return std::make_shared<const Track>(std::move(copy));
}

template <class Fn>
void rewriteAll(Project& np, Fn fn, ChangeSet& cs) {
  for (std::vector<TrackPtr>* list : {&np.tracks, &np.returns})
    for (TrackPtr& t : *list)
      if (TrackPtr r = rewrite(t, fn, cs)) t = std::move(r);
  if (np.master)
    if (TrackPtr r = rewrite(np.master, fn, cs)) np.master = std::move(r);
}

// After tracks were removed: drop sends to removed returns and lanes automating them, reset outputs routed to
// removed groups. `removed` is sorted.
void cleanupAfterRemoval(Project& np, const std::vector<TrackId>& removed, ChangeSet& cs) {
  auto gone = [&](TrackId id) { return std::binary_search(removed.begin(), removed.end(), id); };
  rewriteAll(np, [&](const Track& t) -> std::optional<Track> {
    const bool sendGone = std::any_of(t.mixer.sends.begin(), t.mixer.sends.end(),
                                      [&](const Send& s) { return gone(s.target); });
    const bool outGone = t.mixer.output.kind == OutputKind::Track && gone(t.mixer.output.track);
    const bool laneGone = std::any_of(t.lanes.begin(), t.lanes.end(), [&](const LanePtr& l) {
      return l->param.kind == ParamKind::TrackSend && gone(TrackId{l->param.target});
    });
    if (!sendGone && !outGone && !laneGone) return std::nullopt;
    Track c = t;
    std::erase_if(c.mixer.sends, [&](const Send& s) { return gone(s.target); });
    if (outGone) c.mixer.output = Output{};
    std::erase_if(c.lanes, [&](const LanePtr& l) {
      const bool g = l->param.kind == ParamKind::TrackSend && gone(TrackId{l->param.target});
      if (g) cs.lanes.push_back(l->id);
      return g;
    });
    return c;
  }, cs);
}

// Validates a track subtree that is about to be inserted. `topLevelKinds` = whether the root is a return.
Status checkSubtree(const Project& p, const Track& t, std::vector<uint32_t>& ids) {
  if (!t.id.valid()) return invalid("track has no id");
  if (findTrackPtr(p, t.id)) return invalid("track id already in use");
  ids.push_back(t.id.value);
  if (t.kind == TrackKind::Master) return invalid("a project has exactly one master track");
  if (!t.children.empty() && t.kind != TrackKind::Group) return invalid("only groups have child tracks");
  if (!t.clips.empty() && t.kind != TrackKind::Midi && t.kind != TrackKind::Audio)
    return invalid("only MIDI and audio tracks hold clips");
  PQ_DOC_TRY(checkVolume(t.mixer.volumeDb));
  PQ_DOC_TRY(checkParamValue(ParamKind::TrackPan, t.mixer.pan));
  if (!t.mixer.sends.empty() && !trackHasSends(t.kind)) return invalid("this track has no sends");
  for (const Send& s : t.mixer.sends) {
    const Track* target = findTrackPtr(p, s.target);
    if (!target || target->kind != TrackKind::Return) return invalid("send target must be a return track");
    PQ_DOC_TRY(checkVolume(s.levelDb));
  }
  if (t.mixer.output.kind == OutputKind::Track) {
    const Track* g = findTrackPtr(p, t.mixer.output.track);
    if (!g || g->kind != TrackKind::Group) return invalid("output target must be a group track");
  }
  PQ_DOC_TRY(checkAssignment(p, t.tuning));
  for (size_t i = 0; i < t.clips.size(); ++i) {
    const ClipSlot& c = t.clips[i];
    if (t.kind == TrackKind::Audio) {  // T5.3
      if (c.clip || !c.audio || c.audio->id != c.id) return invalid("an audio track's clip slot must hold its audio clip");
      PQ_DOC_TRY(checkAudioClip(p, *c.audio));
    } else {
      if (c.audio || !c.clip || c.clip->id != c.id) return invalid("clip slot does not match its clip");
      PQ_DOC_TRY(checkClip(p, *c.clip));
    }
    if (i > 0 && std::pair(t.clips[i - 1].start(), t.clips[i - 1].id) >= std::pair(c.start(), c.id))
      return invalid("clips must be sorted");
  }
  for (const LanePtr& l : t.lanes) {
    if (!l || !l->id.valid()) return invalid("lane has no id");
    PQ_DOC_TRY(checkLaneParam(p, t, l->param));
    PQ_DOC_TRY(checkPoints(l->param.kind, l->points));
    if (!std::is_sorted(l->points.begin(), l->points.end(), pointLess)) return invalid("breakpoints must be sorted");
  }
  PQ_DOC_TRY(checkChain(t.kind, t.devices));
  PQ_DOC_TRY(checkNewDeviceIds(p, t.devices, ids));  // unique within the subtree too (one id counter for every entity)
  for (const TrackPtr& c : t.children) {
    if (!c) return invalid("null child track");
    if (c->kind == TrackKind::Return) return invalid("return tracks cannot be grouped");
    PQ_DOC_TRY(checkSubtree(p, *c, ids));
  }
  return ok();
}

// Any lane in the project automating `r` (or using lane id `lane`).
bool laneExists(const Project& p, const ParamRef& r, LaneId lane) {
  bool found = false;
  forEachTrack(p, [&](const Track& t, const Track*) {
    for (const LanePtr& l : t.lanes)
      if (l->param == r || l->id == lane) found = true;
  });
  return found;
}

struct Loc {
  TrackPath path;
  const Track* track;
};
Result<Loc> locate(const Project& p, TrackId id) {
  const auto path = findTrack(p, id);
  if (!path) return notFound("track not found");
  return Loc{*path, &trackAt(p, *path)};
}

bool clipLess(const ClipSlot& a, const ClipSlot& b) {
  return a.start() != b.start() ? a.start() < b.start() : a.id < b.id;
}
void insertClipSorted(std::vector<ClipSlot>& v, ClipSlot s) {
  v.insert(std::upper_bound(v.begin(), v.end(), s, clipLess), std::move(s));
}

// ---- Audio clips (T5.3) ------------------------------------------------------------------------------------------

AudioClipPtr syncedPtr(const Timeline& tl, AudioClip c) {
  return std::make_shared<const AudioClip>(syncedClip(tl, std::move(c)));
}

// Drops the crossfades of `t`'s audio clips whose left neighbour does not end exactly at their start (in the command
// that broke the adjacency; model.h AudioClip::crossfadeIn).
void dropLooseCrossfades(Track& t, ChangeSet& cs) {
  if (t.kind != TrackKind::Audio) return;
  for (size_t i = 0; i < t.clips.size(); ++i) {
    ClipSlot& sl = t.clips[i];
    if (!sl.audio || sl.audio->crossfadeIn == 0) continue;
    if (i > 0 && t.clips[i - 1].end() == sl.audio->start) continue;
    AudioClip c = *sl.audio;
    c.crossfadeIn = 0;
    sl.audio = std::make_shared<const AudioClip>(std::move(c));
    cs.clips.push_back(sl.id);
  }
}

const TuningAssignment& slotTuning(const ClipSlot& s) noexcept { return s.audio ? s.audio->tuning : s.clip->tuning; }

// ---- Tracks --------------------------------------------------------------------------------------------------------

Res apply_(const Snapshot& s, const AddTrack& c, ChangeSet& cs) {
  if (!c.track) return invalid("no track");
  const Project& p = *s;
  std::vector<uint32_t> ids;
  PQ_DOC_TRY(checkSubtree(p, *c.track, ids));
  std::sort(ids.begin(), ids.end());
  if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()) return invalid("duplicate track or device id in subtree");

  TrackList list = TrackList::Main;
  TrackPath parentPath = rootPath(TrackList::Main);
  if (c.track->kind == TrackKind::Return) {
    if (c.parent.valid()) return invalid("return tracks live in the returns list");
    list = TrackList::Returns;
    parentPath = rootPath(TrackList::Returns);
  } else if (c.parent.valid()) {
    Result<Loc> par = locate(p, c.parent);
    if (!par) return std::move(par).error();
    if (par->track->kind != TrackKind::Group || par->path.list != TrackList::Main)
      return invalid("parent must be a group track");
    parentPath = par->path;
  }
  if (parentPath.depth + treeHeight(*c.track) > kMaxTrackDepth) return invalid("groups nested too deeply");

  auto np = copyOf(s);
  std::vector<TrackPtr>& dst = mutableList(*np, list, parentPath);
  const size_t at = std::min<size_t>(c.index, dst.size());
  dst.insert(dst.begin() + static_cast<std::ptrdiff_t>(at), syncedTrack(*s->timeline, c.track));  // T5.3
  PQ_DOC_TRY(checkRouting(*np));
  cs.mark(kChangeStructural);
  collectIds(*c.track, cs);
  if (c.parent.valid()) cs.tracks.push_back(c.parent);
  return Snapshot(std::move(np));
}

// Removes the track at `path` from np (path-copying its parent chain) and returns it.
TrackPtr detach(Project& np, const TrackPath& path) {
  std::vector<TrackPtr>& list = mutableList(np, path.list, path.parent());
  const uint32_t i = path.index[path.depth - 1];
  TrackPtr t = list[i];
  list.erase(list.begin() + i);
  return t;
}

Res apply_(const Snapshot& s, const RemoveTrack& c, ChangeSet& cs) {
  Result<Loc> loc = locate(*s, c.track);
  if (!loc) return std::move(loc).error();
  if (loc->path.list == TrackList::Master) return invalid("the master track cannot be removed");
  auto np = copyOf(s);
  const TrackPtr gone = detach(*np, loc->path);
  ChangeSet removed;
  collectIds(*gone, removed);
  std::vector<TrackId> ids = removed.tracks;
  std::sort(ids.begin(), ids.end());
  cleanupAfterRemoval(*np, ids, cs);
  std::vector<DeviceId> devs = removed.devices;
  std::sort(devs.begin(), devs.end());
  dropWiresOf(*np, devs, cs);  // T4.7: the removed devices' wires go with them
  cs.mark(kChangeStructural);
  cs.merge(removed);
  if (const Track* parent = parentOf(*s, loc->path)) cs.tracks.push_back(parent->id);
  return Snapshot(std::move(np));
}

Res apply_(const Snapshot& s, const MoveTrack& c, ChangeSet& cs) {
  const Project& p = *s;
  Result<Loc> loc = locate(p, c.track);
  if (!loc) return std::move(loc).error();
  if (loc->path.list == TrackList::Master) return invalid("the master track cannot be moved");
  TrackPath parentPath = rootPath(loc->path.list);
  if (loc->path.list == TrackList::Returns) {
    if (c.parent.valid()) return invalid("return tracks cannot be grouped");
  } else if (c.parent.valid()) {
    Result<Loc> par = locate(p, c.parent);
    if (!par) return std::move(par).error();
    if (par->track->kind != TrackKind::Group || par->path.list != TrackList::Main)
      return invalid("parent must be a group track");
    if (c.parent == c.track || isAncestorPath(loc->path, par->path))
      return invalid("a group cannot be moved into itself");
    parentPath = par->path;
  }
  const TrackPath oldParent = loc->path.parent();
  const bool sameParent = parentOf(p, loc->path) == (parentPath.depth ? &trackAt(p, parentPath) : nullptr) &&
                          oldParent.list == parentPath.list;
  {
    const std::vector<TrackPtr>& dstList =
        parentPath.depth ? trackAt(p, parentPath).children : (parentPath.list == TrackList::Returns ? p.returns : p.tracks);
    const size_t sizeAfter = dstList.size() - (sameParent ? 1 : 0);
    const size_t at = std::min<size_t>(c.index, sizeAfter);
    if (sameParent && at == loc->path.index[loc->path.depth - 1]) return s;  // no-op
  }
  // parentPath.depth counts levels down to the new parent; the moved subtree hangs below it.
  if (parentPath.depth + treeHeight(*loc->track) > kMaxTrackDepth) return invalid("groups nested too deeply");

  auto np = copyOf(s);
  TrackPtr moving = detach(*np, loc->path);
  TrackPath newParentPath = rootPath(loc->path.list);
  if (c.parent.valid()) newParentPath = *findTrack(*np, c.parent);  // indices may have shifted
  std::vector<TrackPtr>& dst = mutableList(*np, loc->path.list, newParentPath);
  const size_t at = std::min<size_t>(c.index, dst.size());
  dst.insert(dst.begin() + static_cast<std::ptrdiff_t>(at), std::move(moving));
  PQ_DOC_TRY(checkRouting(*np));
  cs.mark(kChangeStructural);
  cs.tracks.push_back(c.track);
  if (const Track* old = parentOf(p, loc->path)) cs.tracks.push_back(old->id);
  if (c.parent.valid()) cs.tracks.push_back(c.parent);
  return Snapshot(std::move(np));
}

// Edits one track in place (copying it and its ancestors). `f(Track&)` returns Status.
template <class F>
Res editTrack(const Snapshot& s, const TrackPath& path, ChangeSet& cs, ChangeFlag flag, F&& f) {
  auto np = copyOf(s);
  Track& t = mutableTrack(*np, path);
  PQ_DOC_TRY(f(t));
  cs.mark(flag);
  cs.tracks.push_back(t.id);
  return Snapshot(std::move(np));
}

Res apply_(const Snapshot& s, const RenameTrack& c, ChangeSet& cs) {
  Result<Loc> loc = locate(*s, c.track);
  if (!loc) return std::move(loc).error();
  if (loc->track->name == c.name) return s;
  return editTrack(s, loc->path, cs, kChangeMeta, [&](Track& t) { t.name = c.name; return ok(); });
}

Res apply_(const Snapshot& s, const SetTrackColour& c, ChangeSet& cs) {
  Result<Loc> loc = locate(*s, c.track);
  if (!loc) return std::move(loc).error();
  if (loc->track->colour == c.colour) return s;
  return editTrack(s, loc->path, cs, kChangeMeta, [&](Track& t) { t.colour = c.colour; return ok(); });
}

// Mixer value setters: validate, no-op check, edit, record the ParamRef.
template <class Get, class Set>
Res setMixerParam(const Snapshot& s, TrackId id, ParamKind kind, ChangeSet& cs, Get get, Set set,
                  bool allowMaster) {
  Result<Loc> loc = locate(*s, id);
  if (!loc) return std::move(loc).error();
  if (!allowMaster && loc->track->kind == TrackKind::Master) return invalid("not available on the master track");
  if (get(*loc->track)) return s;  // unchanged
  Res r = editTrack(s, loc->path, cs, kChangeParam, [&](Track& t) { set(t); return ok(); });
  if (r) cs.params.push_back(ParamRef::track(kind, id));
  return r;
}

Res apply_(const Snapshot& s, const SetTrackVolume& c, ChangeSet& cs) {
  PQ_DOC_TRY(checkVolume(c.db));
  return setMixerParam(
      s, c.track, ParamKind::TrackVolume, cs, [&](const Track& t) { return t.mixer.volumeDb == c.db; },
      [&](Track& t) { t.mixer.volumeDb = c.db; }, true);
}

Res apply_(const Snapshot& s, const SetTrackPan& c, ChangeSet& cs) {
  PQ_DOC_TRY(checkParamValue(ParamKind::TrackPan, c.pan));
  return setMixerParam(
      s, c.track, ParamKind::TrackPan, cs, [&](const Track& t) { return t.mixer.pan == c.pan; },
      [&](Track& t) { t.mixer.pan = c.pan; }, true);
}

Res apply_(const Snapshot& s, const SetTrackMute& c, ChangeSet& cs) {
  return setMixerParam(
      s, c.track, ParamKind::TrackMute, cs, [&](const Track& t) { return t.mixer.mute == c.mute; },
      [&](Track& t) { t.mixer.mute = c.mute; }, false);
}

Res apply_(const Snapshot& s, const SetTrackSolo& c, ChangeSet& cs) {
  return setMixerParam(
      s, c.track, ParamKind::TrackSolo, cs, [&](const Track& t) { return t.mixer.solo == c.solo; },
      [&](Track& t) { t.mixer.solo = c.solo; }, false);
}

Res apply_(const Snapshot& s, const SetTrackArm& c, ChangeSet& cs) {
  Result<Loc> loc = locate(*s, c.track);
  if (!loc) return std::move(loc).error();
  if (loc->track->kind != TrackKind::Midi) return invalid("only MIDI tracks can be armed");
  if (loc->track->mixer.arm == c.arm) return s;
  return editTrack(s, loc->path, cs, kChangeMeta, [&](Track& t) { t.mixer.arm = c.arm; return ok(); });
}

// Finds the send to `target` in a sorted send list (nullptr = implicit default send).
const Send* findSend(const Mixer& m, TrackId target) {
  for (const Send& sd : m.sends)
    if (sd.target == target) return &sd;
  return nullptr;
}
// Sets the send to `target`, dropping the entry when it equals the default (so equal mixers compare equal).
void storeSend(Mixer& m, Send v) {
  std::erase_if(m.sends, [&](const Send& x) { return x.target == v.target; });
  if (v.levelDb == kMinVolumeDb && !v.preFader) return;
  m.sends.insert(std::upper_bound(m.sends.begin(), m.sends.end(), v,
                                  [](const Send& a, const Send& b) { return a.target < b.target; }),
                 v);
}

Result<Loc> locateSend(const Project& p, TrackId track, TrackId target) {
  Result<Loc> loc = locate(p, track);
  if (!loc) return loc;
  if (!trackHasSends(loc->track->kind)) return invalid("this track has no sends");
  const Track* t = findTrackPtr(p, target);
  if (!t || t->kind != TrackKind::Return || target == track) return invalid("send target must be another return track");
  return loc;
}

Res apply_(const Snapshot& s, const SetSendLevel& c, ChangeSet& cs) {
  PQ_DOC_TRY(checkVolume(c.db));
  Result<Loc> loc = locateSend(*s, c.track, c.target);
  if (!loc) return std::move(loc).error();
  const Send* cur = findSend(loc->track->mixer, c.target);
  Send v = cur ? *cur : Send{c.target};
  if (v.levelDb == c.db) return s;
  v.levelDb = c.db;
  Res r = editTrack(s, loc->path, cs, kChangeParam, [&](Track& t) { storeSend(t.mixer, v); return ok(); });
  if (r) cs.params.push_back(ParamRef::track(ParamKind::TrackSend, c.track, c.target));
  return r;
}

Res apply_(const Snapshot& s, const SetSendPreFader& c, ChangeSet& cs) {
  Result<Loc> loc = locateSend(*s, c.track, c.target);
  if (!loc) return std::move(loc).error();
  const Send* cur = findSend(loc->track->mixer, c.target);
  Send v = cur ? *cur : Send{c.target};
  if (v.preFader == c.preFader) return s;
  v.preFader = c.preFader;
  return editTrack(s, loc->path, cs, kChangeStructural, [&](Track& t) { storeSend(t.mixer, v); return ok(); });
}

Res apply_(const Snapshot& s, const SetTrackOutput& c, ChangeSet& cs) {
  Result<Loc> loc = locate(*s, c.track);
  if (!loc) return std::move(loc).error();
  if (loc->track->kind == TrackKind::Master) return invalid("the master's output is the device");
  if (c.output.kind == OutputKind::Track) {
    const Track* g = findTrackPtr(*s, c.output.track);
    if (!g || g->kind != TrackKind::Group || g->id == c.track) return invalid("output target must be a group track");
  }
  Output out = c.output;
  if (out.kind != OutputKind::Track) out.track = TrackId{};
  if (loc->track->mixer.output == out) return s;
  Res r = editTrack(s, loc->path, cs, kChangeStructural, [&](Track& t) { t.mixer.output = out; return ok(); });
  if (!r) return r;
  PQ_DOC_TRY(checkRouting(**r));
  return r;
}

Res apply_(const Snapshot& s, const SetTrackActive& c, ChangeSet& cs) {
  Result<Loc> loc = locate(*s, c.track);
  if (!loc) return std::move(loc).error();
  if (loc->track->kind == TrackKind::Master) return invalid("the master track cannot be deactivated");
  if (loc->track->active == c.active) return s;
  return editTrack(s, loc->path, cs, kChangeStructural, [&](Track& t) { t.active = c.active; return ok(); });
}

Res apply_(const Snapshot& s, const SetTrackTuning& c, ChangeSet& cs) {
  Result<Loc> loc = locate(*s, c.track);
  if (!loc) return std::move(loc).error();
  if (loc->track->kind != TrackKind::Midi && loc->track->kind != TrackKind::Group)
    return invalid("only MIDI and group tracks have a tuning");
  PQ_DOC_TRY(checkAssignment(*s, c.tuning));
  if (loc->track->tuning == c.tuning) return s;
  Res r = editTrack(s, loc->path, cs, kChangeStructural, [&](Track& t) { t.tuning = c.tuning; return ok(); });
  if (r && c.tuning.mode == TuningMode::Main) cs.tunings.push_back(c.tuning.main);
  return r;
}

Res apply_(const Snapshot& s, const GroupTracks& c, ChangeSet& cs) {
  const Project& p = *s;
  if (!c.group || c.group->kind != TrackKind::Group || !c.group->children.empty() || !c.group->clips.empty())
    return invalid("the new group must be an empty group track");
  if (c.tracks.empty()) return invalid("nothing to group");
  std::vector<uint32_t> ids;
  PQ_DOC_TRY(checkSubtree(p, *c.group, ids));
  std::vector<Loc> sel;
  for (TrackId id : c.tracks) {
    Result<Loc> loc = locate(p, id);
    if (!loc) return std::move(loc).error();
    if (loc->path.list != TrackList::Main) return invalid("only tracks in the main tree can be grouped");
    sel.push_back(*loc);
  }
  std::sort(sel.begin(), sel.end(), [](const Loc& a, const Loc& b) { return pathLess(a.path, b.path); });
  uint32_t height = 0;
  for (size_t i = 0; i < sel.size(); ++i) {
    if (i > 0 && sel[i].track == sel[i - 1].track) return invalid("a track is listed twice");
    for (size_t j = 0; j < sel.size(); ++j)
      if (isAncestorPath(sel[j].path, sel[i].path)) return invalid("a selected track is inside another selected group");
    height = std::max(height, treeHeight(*sel[i].track));
  }
  const TrackPath insertParent = sel.front().path.parent();
  const uint32_t insertIndex = sel.front().path.index[sel.front().path.depth - 1];
  if (insertParent.depth + 1 + height > kMaxTrackDepth) return invalid("groups nested too deeply");

  auto np = copyOf(s);
  auto group = std::make_shared<Track>(*c.group);
  // Detach in reverse tree order so earlier paths stay valid.
  for (size_t i = sel.size(); i-- > 0;) {
    group->children.insert(group->children.begin(), detach(*np, sel[i].path));
    if (const Track* parent = parentOf(p, sel[i].path)) cs.tracks.push_back(parent->id);
  }
  // The insertion parent comes before every selected track in tree order and is not one of them, so its path is
  // unchanged by the detaches; so is the insertion index (all later selections sit after it).
  std::vector<TrackPtr>& dst = mutableList(*np, TrackList::Main, insertParent);
  dst.insert(dst.begin() + insertIndex, TrackPtr(std::move(group)));
  PQ_DOC_TRY(checkRouting(*np));
  cs.mark(kChangeStructural);
  cs.tracks.push_back(c.group->id);
  for (const Loc& l : sel) cs.tracks.push_back(l.track->id);
  return Snapshot(std::move(np));
}

Res apply_(const Snapshot& s, const UngroupTracks& c, ChangeSet& cs) {
  Result<Loc> loc = locate(*s, c.group);
  if (!loc) return std::move(loc).error();
  if (loc->track->kind != TrackKind::Group) return invalid("not a group track");
  auto np = copyOf(s);
  const TrackPtr group = detach(*np, loc->path);
  std::vector<TrackPtr>& dst = mutableList(*np, TrackList::Main, loc->path.parent());
  const uint32_t at = loc->path.index[loc->path.depth - 1];
  dst.insert(dst.begin() + at, group->children.begin(), group->children.end());
  cleanupAfterRemoval(*np, {c.group}, cs);
  PQ_DOC_TRY(checkRouting(*np));
  cs.mark(kChangeStructural);
  cs.tracks.push_back(c.group);
  for (const TrackPtr& k : group->children) cs.tracks.push_back(k->id);
  for (const LanePtr& l : group->lanes) cs.lanes.push_back(l->id);
  if (const Track* parent = parentOf(*s, loc->path)) cs.tracks.push_back(parent->id);
  return Snapshot(std::move(np));
}

// ---- Clips ---------------------------------------------------------------------------------------------------------

struct ClipLoc {
  TrackPath path;
  const Track* track;
  int32_t index;
  const ClipPtr* clip;   // null pointer inside for an audio clip
  const ClipSlot* slot;
};
Result<ClipLoc> locateClip(const Project& p, const ClipRef& r) {
  Result<Loc> loc = locate(p, r.track);
  if (!loc) return std::move(loc).error();
  const int32_t i = findClipIndex(*loc->track, r.clip);
  if (i < 0) return notFound("clip not found on this track");
  const ClipSlot& slot = loc->track->clips[static_cast<size_t>(i)];
  return ClipLoc{loc->path, loc->track, i, &slot.clip, &slot};
}

// Replaces one clip. f(const NoteClip&) → Result<ClipPtr>, nullptr = unchanged.
template <class F>
Res editClip(const Snapshot& s, const ClipRef& r, ChangeSet& cs, ChangeFlag flag, F&& f) {
  Result<ClipLoc> loc = locateClip(*s, r);
  if (!loc) return std::move(loc).error();
  if (!*loc->clip) return invalid("not a note clip");
  Result<ClipPtr> nc = f(**loc->clip);
  if (!nc) return std::move(nc).error();
  if (!*nc) return s;
  auto np = copyOf(s);
  Track& t = mutableTrack(*np, loc->path);
  const auto i = static_cast<size_t>(loc->index);
  if ((*nc)->start != (*loc->clip)->start) {
    t.clips.erase(t.clips.begin() + static_cast<std::ptrdiff_t>(i));
    insertClipSorted(t.clips, ClipSlot{r.clip, std::move(*nc)});
  } else {
    t.clips[i].clip = std::move(*nc);
  }
  cs.mark(flag);
  cs.tracks.push_back(r.track);
  cs.clips.push_back(r.clip);
  return Snapshot(std::move(np));
}

// Clip header edits (no note changes): copy, modify, validate.
template <class F>
Res editClipHeader(const Snapshot& s, const ClipRef& r, ChangeSet& cs, ChangeFlag flag, F&& modify) {
  return editClip(s, r, cs, flag, [&](const NoteClip& old) -> Result<ClipPtr> {
    NoteClip c = withNotes(old, old.notes);
    modify(c);
    if (c == old) return ClipPtr{};
    PQ_DOC_TRY(checkClip(*s, c));
    return ClipPtr(std::make_shared<const NoteClip>(std::move(c)));
  });
}

// T5.3: edits one audio clip. modify(AudioClip&) → Status; the result is synced (Off length), validated, re-sorted, and
// the track's loose crossfades dropped. Unchanged → no-op.
template <class F>
Res editAudio(const Snapshot& s, const ClipRef& r, ChangeSet& cs, ChangeFlag flag, F&& modify) {
  Result<ClipLoc> loc = locateClip(*s, r);
  if (!loc) return std::move(loc).error();
  const ClipSlot& slot = *loc->slot;
  if (!slot.audio) return invalid("not an audio clip");
  AudioClip c = *slot.audio;
  PQ_DOC_TRY(modify(c));
  c = syncedClip(*s->timeline, std::move(c));
  if (c == *slot.audio) return s;
  if (c.id != r.clip) return invalid("a clip's id cannot change");
  PQ_DOC_TRY(checkAudioClip(*s, c));
  auto np = copyOf(s);
  Track& t = mutableTrack(*np, loc->path);
  const auto i = static_cast<size_t>(loc->index);
  auto nc = std::make_shared<const AudioClip>(std::move(c));
  if (nc->start != slot.audio->start) {
    t.clips.erase(t.clips.begin() + static_cast<std::ptrdiff_t>(i));
    insertClipSorted(t.clips, ClipSlot{r.clip, nullptr, std::move(nc)});
  } else {
    t.clips[i].audio = std::move(nc);
  }
  cs.mark(flag);
  cs.tracks.push_back(r.track);
  cs.clips.push_back(r.clip);
  return Snapshot(std::move(np));
}

// A header field both clip kinds have (name, colour, active, start): set(auto& clip) on a copy.
template <class F>
Res editEither(const Snapshot& s, const ClipRef& r, ChangeSet& cs, ChangeFlag flag, F&& set) {
  Result<ClipLoc> loc = locateClip(*s, r);
  if (!loc) return std::move(loc).error();
  if (loc->slot->audio)
    return editAudio(s, r, cs, flag, [&](AudioClip& a) {
      set(a);
      return ok();
    });
  return editClipHeader(s, r, cs, flag, [&](NoteClip& n) { set(n); });
}

Res apply_(const Snapshot& s, const AddClip& c, ChangeSet& cs) {
  if (c.audio) {  // T5.3
    if (c.clip) return invalid("a clip is either a note clip or an audio clip");
    Result<Loc> loc = locate(*s, c.track);
    if (!loc) return std::move(loc).error();
    if (loc->track->kind != TrackKind::Audio) return invalid("audio clips live on audio tracks");
    PQ_DOC_TRY(checkAudioClip(*s, *c.audio));
    if (findClipIndex(*loc->track, c.audio->id) >= 0) return invalid("clip id already on this track");
    auto np = copyOf(s);
    Track& t = mutableTrack(*np, loc->path);
    insertClipSorted(t.clips, ClipSlot{c.audio->id, nullptr, syncedPtr(*s->timeline, *c.audio)});
    cs.mark(kChangeSequence);
    cs.tracks.push_back(c.track);
    cs.clips.push_back(c.audio->id);
    return Snapshot(std::move(np));
  }
  if (!c.clip) return invalid("no clip");
  Result<Loc> loc = locate(*s, c.track);
  if (!loc) return std::move(loc).error();
  if (loc->track->kind != TrackKind::Midi) return invalid("note clips live on MIDI tracks");
  PQ_DOC_TRY(checkClip(*s, *c.clip));
  if (findClipIndex(*loc->track, c.clip->id) >= 0) return invalid("clip id already on this track");
  auto np = copyOf(s);
  Track& t = mutableTrack(*np, loc->path);
  insertClipSorted(t.clips, ClipSlot{c.clip->id, c.clip});
  cs.mark(kChangeSequence);
  cs.tracks.push_back(c.track);
  cs.clips.push_back(c.clip->id);
  return Snapshot(std::move(np));
}

Res apply_(const Snapshot& s, const RemoveClip& c, ChangeSet& cs) {
  Result<ClipLoc> loc = locateClip(*s, c.clip);
  if (!loc) return std::move(loc).error();
  auto np = copyOf(s);
  Track& t = mutableTrack(*np, loc->path);
  t.clips.erase(t.clips.begin() + loc->index);
  cs.mark(kChangeSequence);
  cs.tracks.push_back(c.clip.track);
  cs.clips.push_back(c.clip.clip);
  return Snapshot(std::move(np));
}

Res apply_(const Snapshot& s, const MoveClip& c, ChangeSet& cs) {
  PQ_DOC_TRY(checkTick(c.start, "clip start"));
  if (c.toTrack == c.clip.track || !c.toTrack.valid()) {
    return editEither(s, c.clip, cs, kChangeSequence, [&](auto& n) { n.start = c.start; });
  }
  Result<ClipLoc> loc = locateClip(*s, c.clip);
  if (!loc) return std::move(loc).error();
  Result<Loc> to = locate(*s, c.toTrack);
  if (!to) return std::move(to).error();
  ClipSlot moved = *loc->slot;
  if (moved.audio) {  // T5.3
    if (to->track->kind != TrackKind::Audio) return invalid("audio clips live on audio tracks");
    moved.audio = std::make_shared<const AudioClip>(audioMovedTo(*s->timeline, *moved.audio, c.start));
  } else {
    if (to->track->kind != TrackKind::Midi) return invalid("note clips live on MIDI tracks");
    if (moved.clip->start != c.start) {
      NoteClip n = withNotes(*moved.clip, moved.clip->notes);
      n.start = c.start;
      moved.clip = std::make_shared<const NoteClip>(std::move(n));
    }
  }
  auto np = copyOf(s);
  Track& from = mutableTrack(*np, loc->path);
  from.clips.erase(from.clips.begin() + loc->index);
  Track& dst = mutableTrack(*np, to->path);  // paths are unchanged: only clip lists were edited
  insertClipSorted(dst.clips, std::move(moved));
  cs.mark(kChangeSequence);
  cs.tracks.push_back(c.clip.track);
  cs.tracks.push_back(c.toTrack);
  cs.clips.push_back(c.clip.clip);
  return Snapshot(std::move(np));
}

Res apply_(const Snapshot& s, const SetClipBounds& c, ChangeSet& cs) {
  return editClipHeader(s, c.clip, cs, kChangeSequence, [&](NoteClip& n) {
    n.start = c.start;
    n.length = c.length;
    n.contentStart = c.contentStart;
  });
}

Res apply_(const Snapshot& s, const SetClipLoop& c, ChangeSet& cs) {
  return editClipHeader(s, c.clip, cs, kChangeSequence, [&](NoteClip& n) { n.loop = c.loop; });
}

Res apply_(const Snapshot& s, const SetClipTuning& c, ChangeSet& cs) {
  if (Result<ClipLoc> loc = locateClip(*s, c.clip); loc && loc->slot->audio) {  // T5.3: runtime data of the player
    PQ_DOC_TRY(checkAssignment(*s, c.tuning));
    Res r = editAudio(s, c.clip, cs, kChangeSequence, [&](AudioClip& a) {
      a.tuning = c.tuning;
      return ok();
    });
    if (r && *r != s && c.tuning.mode == TuningMode::Main) cs.tunings.push_back(c.tuning.main);
    return r;
  }
  Res r = editClipHeader(s, c.clip, cs, kChangeStructural, [&](NoteClip& n) { n.tuning = c.tuning; });
  if (r && *r != s && c.tuning.mode == TuningMode::Main) cs.tunings.push_back(c.tuning.main);
  return r;
}

Res apply_(const Snapshot& s, const RenameClip& c, ChangeSet& cs) {
  return editEither(s, c.clip, cs, kChangeMeta, [&](auto& n) { n.name = c.name; });
}

Res apply_(const Snapshot& s, const SetClipColour& c, ChangeSet& cs) {
  return editEither(s, c.clip, cs, kChangeMeta, [&](auto& n) { n.colour = c.colour; });
}

Res apply_(const Snapshot& s, const SetClipSignature& c, ChangeSet& cs) {
  return editClipHeader(s, c.clip, cs, kChangeMeta, [&](NoteClip& n) { n.signature = c.signature; });
}

Res apply_(const Snapshot& s, const SetClipFundamental& c, ChangeSet& cs) {
  return editClipHeader(s, c.clip, cs, kChangeMeta, [&](NoteClip& n) { n.fundamental = c.fundamental; });
}

Res apply_(const Snapshot& s, const SetClipActive& c, ChangeSet& cs) {
  return editEither(s, c.clip, cs, kChangeSequence, [&](auto& n) { n.active = c.active; });
}

Res apply_(const Snapshot& s, const SetAudioClip& c, ChangeSet& cs) {
  if (!c.audio) return invalid("no clip");
  Res r = editAudio(s, c.clip, cs, kChangeSequence, [&](AudioClip& a) {
    a = *c.audio;
    return ok();
  });
  if (r && *r != s && c.audio->sample.valid()) cs.samples.push_back(c.audio->sample);
  return r;
}

// ---- Samples (T5.3) ----------------------------------------------------------------------------------------------

bool sampleUsed(const Project& p, SampleId id) {
  bool used = false;
  forEachTrack(p, [&](const Track& t, const Track*) {
    for (const ClipSlot& sl : t.clips)
      if (sl.audio && sl.audio->sample == id) used = true;
  });
  return used;
}

Res apply_(const Snapshot& s, const AddSample& c, ChangeSet& cs) {
  PQ_DOC_TRY(checkSampleRef(c.sample));
  if (findSample(*s, c.sample.id)) return invalid("sample id already in use");
  auto np = copyOf(s);
  SampleList list = samplesOf(*s);
  list.insert(std::lower_bound(list.begin(), list.end(), c.sample.id,
                               [](const SampleRef& a, SampleId b) { return a.id < b; }),
              c.sample);
  np->samples = std::make_shared<const SampleList>(std::move(list));
  cs.mark(kChangeMeta);
  cs.samples.push_back(c.sample.id);
  return Snapshot(std::move(np));
}

Res apply_(const Snapshot& s, const RemoveSample& c, ChangeSet& cs) {
  if (!findSample(*s, c.sample)) return notFound("sample not found");
  if (sampleUsed(*s, c.sample)) return invalid("the sample is still played by an audio clip");
  auto np = copyOf(s);
  SampleList list = samplesOf(*s);
  std::erase_if(list, [&](const SampleRef& r) { return r.id == c.sample; });
  np->samples = list.empty() ? nullptr : std::make_shared<const SampleList>(std::move(list));
  cs.mark(kChangeMeta);
  cs.samples.push_back(c.sample);
  return Snapshot(std::move(np));
}

// Re-derives the Off clips of every audio track (after a tempo map or sample rate change); `only`: just those playing
// that sample. Touched tracks and clips go to `cs`; returns whether any changed.
bool resyncAudioClips(Project& np, ChangeSet& cs, SampleId only = {}) {
  bool any = false;
  const Timeline& tl = *np.timeline;
  rewriteAll(np, [&](const Track& t) -> std::optional<Track> {
    if (t.kind != TrackKind::Audio) return std::nullopt;
    std::optional<Track> copy;
    for (size_t i = 0; i < t.clips.size(); ++i) {
      const ClipSlot& sl = t.clips[i];
      if (!sl.audio || sl.audio->warp != AudioWarp::Off || (only.valid() && sl.audio->sample != only)) continue;
      AudioClip c = syncedClip(tl, *sl.audio);
      if (c == *sl.audio) continue;
      if (!copy) copy = t;
      copy->clips[i].audio = std::make_shared<const AudioClip>(std::move(c));
      cs.clips.push_back(sl.id);
    }
    if (!copy) return std::nullopt;
    any = true;
    return copy;
  }, cs);
  return any;
}

Res apply_(const Snapshot& s, const SetSample& c, ChangeSet& cs) {
  const SampleRef* old = findSample(*s, c.sample.id);
  if (!old) return notFound("sample not found");
  PQ_DOC_TRY(checkSampleRef(c.sample));
  if (*old == c.sample) return s;
  auto np = copyOf(s);
  SampleList list = samplesOf(*s);
  for (SampleRef& r : list)
    if (r.id == c.sample.id) r = c.sample;
  np->samples = std::make_shared<const SampleList>(std::move(list));
  cs.mark(kChangeMeta);
  cs.samples.push_back(c.sample.id);
  if (old->rate != c.sample.rate && resyncAudioClips(*np, cs, c.sample.id)) cs.mark(kChangeSequence);
  return Snapshot(std::move(np));
}

// ---- Notes ---------------------------------------------------------------------------------------------------------

ClipPtr clipWith(const NoteClip& old, NoteSeq seq) {
  return std::make_shared<const NoteClip>(withNotes(old, std::move(seq)));
}

Res apply_(const Snapshot& s, const AddNotes& c, ChangeSet& cs) {
  if (c.notes.empty()) return s;
  std::vector<uint32_t> ids;
  ids.reserve(c.notes.size());
  for (const Note& n : c.notes) {
    PQ_DOC_TRY(validateNote(n));
    ids.push_back(n.id.value);
  }
  std::sort(ids.begin(), ids.end());
  if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()) return invalid("duplicate note id");
  Res r = editClip(s, c.clip, cs, kChangeSequence, [&](const NoteClip& old) -> Result<ClipPtr> {
    for (const Note& n : c.notes)
      if (old.notes.find(keyOf(n))) return invalid("note already in the clip");
    return clipWith(old, old.notes.withInserted(c.notes));
  });
  if (r)
    for (const Note& n : c.notes) cs.notes.push_back(n.id);
  return r;
}

Res apply_(const Snapshot& s, const RemoveNotes& c, ChangeSet& cs) {
  if (c.notes.empty()) return s;
  std::vector<NoteKey> keys;
  Res r = editClip(s, c.clip, cs, kChangeSequence, [&](const NoteClip& old) -> Result<ClipPtr> {
    keys.reserve(c.notes.size());
    for (const NoteKey& k : c.notes) {
      const Note* n = old.notes.locate(k);
      if (!n) return notFound("note not found");
      keys.push_back(keyOf(*n));
    }
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    return clipWith(old, old.notes.withRemoved(keys));
  });
  if (r)
    for (const NoteKey& k : keys) cs.notes.push_back(k.id);
  return r;
}

// Shared by the note-changing commands: f(const Note& old) → Result<Note> (the new value) for each addressed
// note; unchanged notes are skipped; nothing changed → no-op.
template <class F>
Res editNotes(const Snapshot& s, const ClipRef& clip, std::span<const NoteKey> notes, ChangeSet& cs, F&& f) {
  std::vector<NoteKey> rem;
  std::vector<Note> ins;
  Res r = editClip(s, clip, cs, kChangeSequence, [&](const NoteClip& old) -> Result<ClipPtr> {
    for (const NoteKey& k : notes) {
      const Note* n = old.notes.locate(k);
      if (!n) return notFound("note not found");
      Result<Note> m = f(*n);
      if (!m) return std::move(m).error();
      if (*m == *n) continue;
      if (m->id != n->id) return invalid("a note's id cannot change");
      PQ_DOC_TRY(validateNote(*m));
      rem.push_back(keyOf(*n));
      ins.push_back(*m);
    }
    if (rem.empty()) return ClipPtr{};
    std::vector<NoteKey> sorted = rem;
    std::sort(sorted.begin(), sorted.end());
    if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) return invalid("a note is listed twice");
    return clipWith(old, old.notes.edited(rem, ins));
  });
  if (r && *r != s)
    for (const Note& n : ins) cs.notes.push_back(n.id);
  return r;
}

Res apply_(const Snapshot& s, const UpdateNotes& c, ChangeSet& cs) {
  std::vector<NoteKey> keys;
  keys.reserve(c.updates.size());
  for (const NoteUpdate& u : c.updates) keys.push_back(u.key);
  size_t i = 0;
  return editNotes(s, c.clip, keys, cs, [&](const Note&) -> Result<Note> { return c.updates[i++].note; });
}

Res apply_(const Snapshot& s, const MoveNote& c, ChangeSet& cs) {
  return editNotes(s, c.clip, {&c.note, 1}, cs, [&](const Note& n) -> Result<Note> {
    Note m = n;
    m.start = c.start;
    m.pitch = c.pitch;
    return m;
  });
}

Res apply_(const Snapshot& s, const ResizeNote& c, ChangeSet& cs) {
  return editNotes(s, c.clip, {&c.note, 1}, cs, [&](const Note& n) -> Result<Note> {
    Note m = n;
    m.length = c.length;
    return m;
  });
}

Res apply_(const Snapshot& s, const SetNoteProperty& c, ChangeSet& cs) {
  return editNotes(s, c.clip, c.notes, cs, [&](const Note& n) -> Result<Note> {
    Note m = n;
    switch (c.property) {
      case NoteProperty::Velocity: m.velocity = c.value; break;
      case NoteProperty::ReleaseVelocity: m.releaseVelocity = c.value; break;
      case NoteProperty::Chance: m.chance = c.value; break;
      case NoteProperty::VelocityRange: m.velocityRange = c.value; break;
      case NoteProperty::Enabled:
        m.flags = c.value != 0.0f ? (m.flags & ~static_cast<uint32_t>(kNoteDisabled)) : (m.flags | kNoteDisabled);
        break;
    }
    return m;
  });
}

// ---- Automation ----------------------------------------------------------------------------------------------------

Res apply_(const Snapshot& s, const AddAutomationLane& c, ChangeSet& cs) {
  if (!c.lane || !c.lane->id.valid()) return invalid("lane has no id");
  Result<Loc> loc = locate(*s, c.track);
  if (!loc) return std::move(loc).error();
  PQ_DOC_TRY(checkLaneParam(*s, *loc->track, c.lane->param));
  // T3.17-fix3 (M3 review M3): a tuning without a signal (Scala, MODMOS, out-of-range values) has no node for a lane
  // to drive; a new lane on it would play nothing. (A file may still hold one: it loads, and playback says it is
  // inert.)
  if (!isTrackParam(c.lane->param.kind))
    if (const TuningSlot* t = findTuning(*s, TuningId{c.lane->param.owner}); t != nullptr && !hasTuningSignal(t->def))
      return invalid("this tuning has no signal (not an EDO or MOS): its parameters cannot be automated");
  PQ_DOC_TRY(checkPoints(c.lane->param.kind, c.lane->points));
  if (!std::is_sorted(c.lane->points.begin(), c.lane->points.end(), pointLess))
    return invalid("breakpoints must be sorted");
  if (laneExists(*s, c.lane->param, c.lane->id)) return invalid("this parameter already has a lane");
  Res r = editTrack(s, loc->path, cs, kChangeStructural, [&](Track& t) {
    t.lanes.push_back(c.lane);
    return ok();
  });
  if (r) {
    cs.lanes.push_back(c.lane->id);
    if (!isTrackParam(c.lane->param.kind)) cs.tunings.push_back(TuningId{c.lane->param.owner});
  }
  return r;
}

Res apply_(const Snapshot& s, const RemoveAutomationLane& c, ChangeSet& cs) {
  Result<Loc> loc = locate(*s, c.track);
  if (!loc) return std::move(loc).error();
  const auto& lanes = loc->track->lanes;
  const auto it = std::find_if(lanes.begin(), lanes.end(), [&](const LanePtr& l) { return l->id == c.lane; });
  if (it == lanes.end()) return notFound("lane not found on this track");
  const auto i = it - lanes.begin();
  Res r = editTrack(s, loc->path, cs, kChangeStructural, [&](Track& t) {
    t.lanes.erase(t.lanes.begin() + i);
    return ok();
  });
  if (r) cs.lanes.push_back(c.lane);
  return r;
}

// Edits one lane's breakpoints. f(std::vector<Breakpoint>&) → Status; unchanged → no-op.
template <class F>
Res editLane(const Snapshot& s, TrackId track, LaneId lane, ChangeSet& cs, F&& f) {
  Result<Loc> loc = locate(*s, track);
  if (!loc) return std::move(loc).error();
  const auto& lanes = loc->track->lanes;
  const auto it = std::find_if(lanes.begin(), lanes.end(), [&](const LanePtr& l) { return l->id == lane; });
  if (it == lanes.end()) return notFound("lane not found on this track");
  AutomationLane nl = **it;
  PQ_DOC_TRY(f(nl.points));
  std::sort(nl.points.begin(), nl.points.end(), pointLess);
  if (nl.points == (*it)->points) return s;
  PQ_DOC_TRY(checkPoints(nl.param.kind, nl.points));
  const auto i = static_cast<size_t>(it - lanes.begin());
  Res r = editTrack(s, loc->path, cs, kChangeAutomation, [&](Track& t) {
    t.lanes[i] = std::make_shared<const AutomationLane>(std::move(nl));
    return ok();
  });
  if (r) cs.lanes.push_back(lane);
  return r;
}

Res apply_(const Snapshot& s, const AddBreakpoints& c, ChangeSet& cs) {
  if (c.points.empty()) return s;
  return editLane(s, c.track, c.lane, cs, [&](std::vector<Breakpoint>& pts) {
    pts.insert(pts.end(), c.points.begin(), c.points.end());
    return ok();
  });
}

Res apply_(const Snapshot& s, const RemoveBreakpoints& c, ChangeSet& cs) {
  if (c.points.empty()) return s;
  return editLane(s, c.track, c.lane, cs, [&](std::vector<Breakpoint>& pts) -> Status {
    for (BreakpointId id : c.points) {
      const auto it = std::find_if(pts.begin(), pts.end(), [&](const Breakpoint& b) { return b.id == id; });
      if (it == pts.end()) return notFound("breakpoint not found");
      pts.erase(it);
    }
    return ok();
  });
}

Res apply_(const Snapshot& s, const UpdateBreakpoints& c, ChangeSet& cs) {
  return editLane(s, c.track, c.lane, cs, [&](std::vector<Breakpoint>& pts) -> Status {
    for (const Breakpoint& u : c.points) {
      const auto it = std::find_if(pts.begin(), pts.end(), [&](const Breakpoint& b) { return b.id == u.id; });
      if (it == pts.end()) return notFound("breakpoint not found");
      *it = u;
    }
    return ok();
  });
}

// ---- Timeline ------------------------------------------------------------------------------------------------------

Res apply_(const Snapshot& s, const SetTempoMap& c, ChangeSet& cs) {
  Result<TempoMap> map = TempoMap::create(c.points);
  if (!map) return std::move(map).error();
  Timeline tl = *s->timeline;
  tl.tempo = c.points;
  if (tl == *s->timeline) return s;
  tl.tempoMap = std::move(map).value();
  auto np = copyOf(s);
  np->timeline = std::make_shared<const Timeline>(std::move(tl));
  cs.mark(kChangeTimeline);
  if (resyncAudioClips(*np, cs)) cs.mark(kChangeSequence);  // T5.3: warp-Off clips keep their seconds
  return Snapshot(std::move(np));
}

Res apply_(const Snapshot& s, const SetTimeSignatures& c, ChangeSet& cs) {
  Result<TimeSigMap> map = TimeSigMap::create(c.changes);
  if (!map) return std::move(map).error();
  Timeline tl = *s->timeline;
  tl.signatures = c.changes;
  if (tl == *s->timeline) return s;
  tl.sigMap = std::move(map).value();
  auto np = copyOf(s);
  np->timeline = std::make_shared<const Timeline>(std::move(tl));
  cs.mark(kChangeTimeline);
  return Snapshot(std::move(np));
}

Res apply_(const Snapshot& s, const SetLoop& c, ChangeSet& cs) {
  PQ_DOC_TRY(checkTick(c.loop.start, "loop start"));
  PQ_DOC_TRY(checkTick(c.loop.end, "loop end"));
  if (c.loop.end <= c.loop.start) return invalid("loop end must be after loop start");
  if (c.loop == s->loop) return s;
  auto np = copyOf(s);
  np->loop = c.loop;
  cs.mark(kChangeTimeline);
  return Snapshot(std::move(np));
}

bool locatorLess(const Locator& a, const Locator& b) { return a.tick != b.tick ? a.tick < b.tick : a.id < b.id; }

Res setLocators(const Snapshot& s, LocatorList list, LocatorId touched, ChangeSet& cs) {
  std::sort(list.begin(), list.end(), locatorLess);
  if (list == *s->locators) return s;
  auto np = copyOf(s);
  np->locators = std::make_shared<const LocatorList>(std::move(list));
  cs.mark(kChangeMeta);
  cs.locators.push_back(touched);
  return Snapshot(std::move(np));
}

Res apply_(const Snapshot& s, const AddLocator& c, ChangeSet& cs) {
  if (!c.locator.id.valid()) return invalid("locator has no id");
  PQ_DOC_TRY(checkTick(c.locator.tick, "locator"));
  for (const Locator& l : *s->locators)
    if (l.id == c.locator.id) return invalid("locator id already in use");
  LocatorList list = *s->locators;
  list.push_back(c.locator);
  return setLocators(s, std::move(list), c.locator.id, cs);
}

Res apply_(const Snapshot& s, const RemoveLocator& c, ChangeSet& cs) {
  LocatorList list = *s->locators;
  if (std::erase_if(list, [&](const Locator& l) { return l.id == c.locator; }) == 0)
    return notFound("locator not found");
  return setLocators(s, std::move(list), c.locator, cs);
}

Res apply_(const Snapshot& s, const UpdateLocator& c, ChangeSet& cs) {
  PQ_DOC_TRY(checkTick(c.locator.tick, "locator"));
  LocatorList list = *s->locators;
  const auto it = std::find_if(list.begin(), list.end(), [&](const Locator& l) { return l.id == c.locator.id; });
  if (it == list.end()) return notFound("locator not found");
  *it = c.locator;
  return setLocators(s, std::move(list), c.locator.id, cs);
}

// ---- Tunings -------------------------------------------------------------------------------------------------------

int32_t tuningIndex(const Project& p, TuningId id) {
  for (size_t i = 0; i < p.tunings->size(); ++i)
    if ((*p.tunings)[i]->id == id) return static_cast<int32_t>(i);
  return -1;
}

// Replaces slot i with `slot` in a copy of the project.
std::shared_ptr<Project> withSlot(const Snapshot& s, size_t i, TuningSlot slot) {
  auto np = copyOf(s);
  TuningList list = *s->tunings;
  list[i] = std::make_shared<const TuningSlot>(std::move(slot));
  np->tunings = std::make_shared<const TuningList>(std::move(list));
  return np;
}

// Removes lanes automating tuning `id` for which keep(kind) is false.
template <class Keep>
void dropTuningLanes(Project& np, TuningId id, Keep keep, ChangeSet& cs) {
  rewriteAll(np, [&](const Track& t) -> std::optional<Track> {
    auto doomed = [&](const LanePtr& l) {
      return !isTrackParam(l->param.kind) && l->param.owner == id.value && !keep(l->param.kind);
    };
    if (std::none_of(t.lanes.begin(), t.lanes.end(), doomed)) return std::nullopt;
    Track c = t;
    std::erase_if(c.lanes, [&](const LanePtr& l) {
      if (!doomed(l)) return false;
      cs.lanes.push_back(l->id);
      return true;
    });
    return c;
  }, cs);
}

Res apply_(const Snapshot& s, const AddTuning& c, ChangeSet& cs) {
  if (!c.slot.id.valid()) return invalid("tuning has no id");
  if (findTuning(*s, c.slot.id)) return invalid("tuning id already in use");
  PQ_DOC_TRY(checkTuningDef(c.slot.def));
  auto np = copyOf(s);
  TuningList list = *s->tunings;
  list.push_back(std::make_shared<const TuningSlot>(c.slot));
  np->tunings = std::make_shared<const TuningList>(std::move(list));
  cs.mark(kChangeStructural);
  cs.tunings.push_back(c.slot.id);
  return Snapshot(std::move(np));
}

Res apply_(const Snapshot& s, const RemoveTuning& c, ChangeSet& cs) {
  const int32_t i = tuningIndex(*s, c.tuning);
  if (i < 0) return notFound("tuning not found");
  if (s->tunings->size() == 1) return invalid("a project needs at least one main tuning");
  bool used = false;
  auto uses = [&](const TuningAssignment& a) { return a.mode == TuningMode::Main && a.main == c.tuning; };
  forEachTrack(*s, [&](const Track& t, const Track*) {
    if (uses(t.tuning)) used = true;
    for (const ClipSlot& cl : t.clips)
      if (uses(slotTuning(cl))) used = true;
  });
  if (used) return invalid("tuning is still used by a track or clip");
  auto np = copyOf(s);
  TuningList list = *s->tunings;
  list.erase(list.begin() + i);
  np->tunings = std::make_shared<const TuningList>(std::move(list));
  dropTuningLanes(*np, c.tuning, [](ParamKind) { return false; }, cs);
  cs.mark(kChangeStructural);
  cs.tunings.push_back(c.tuning);
  return Snapshot(std::move(np));
}

Res apply_(const Snapshot& s, const RenameTuning& c, ChangeSet& cs) {
  const int32_t i = tuningIndex(*s, c.tuning);
  if (i < 0) return notFound("tuning not found");
  const TuningSlot& old = *(*s->tunings)[static_cast<size_t>(i)];
  if (old.name == c.name) return s;
  TuningSlot n = old;
  n.name = c.name;
  cs.mark(kChangeMeta);
  cs.tunings.push_back(c.tuning);
  return Snapshot(withSlot(s, static_cast<size_t>(i), std::move(n)));
}

Res apply_(const Snapshot& s, const SetTuningColour& c, ChangeSet& cs) {
  const int32_t i = tuningIndex(*s, c.tuning);
  if (i < 0) return notFound("tuning not found");
  const TuningSlot& old = *(*s->tunings)[static_cast<size_t>(i)];
  if (old.colour == c.colour) return s;
  TuningSlot n = old;
  n.colour = c.colour;
  cs.mark(kChangeMeta);
  cs.tunings.push_back(c.tuning);
  return Snapshot(withSlot(s, static_cast<size_t>(i), std::move(n)));
}

// The continuous parameters that differ between two definitions of the same discrete shape, or nullopt if the
// shape differs (→ structural).
std::optional<std::vector<ParamKind>> continuousDiff(const TuningDef& a, const TuningDef& b) {
  if (a.system.index() != b.system.index()) return std::nullopt;
  std::vector<ParamKind> out;
  if (a.referenceHz != b.referenceHz) out.push_back(ParamKind::TuningReference);
  if (const tuning::Edo* ea = a.edo()) {
    const tuning::Edo& eb = *b.edo();
    if (ea->n() != eb.n()) return std::nullopt;
    if (!(*ea == eb)) out.push_back(ParamKind::TuningEquave);
    return out;
  }
  if (const ScaleDef* sa = a.scale()) {
    // The pitches and the key mapping are discrete shape (a re-import); the description alone is a name (Meta,
    // signalled by an empty list).
    const ScaleDef& sb = *b.scale();
    if (sa->scale.pitches != sb.scale.pitches || sa->mapping != sb.mapping) return std::nullopt;
    return out;
  }
  const tuning::MosParams& ma = *a.mos();
  const tuning::MosParams& mb = *b.mos();
  if (ma.n != mb.n || ma.periodsPerEquave != mb.periodsPerEquave || ma.rotation != mb.rotation ||
      ma.modmos != mb.modmos || ma.genSequence != mb.genSequence)
    return std::nullopt;
  const bool regionDiffers =
      ma.region.has_value() != mb.region.has_value() ||
      (ma.region && (ma.region->perPeriodSize != mb.region->perPeriodSize ||
                     ma.region->lowCents != mb.region->lowCents || ma.region->highCents != mb.region->highCents));
  // The region hint only disambiguates a degenerate generator, i.e. it is part of the generator's value.
  if (ma.generator != mb.generator || regionDiffers) out.push_back(ParamKind::MosGenerator);
  if (ma.period != mb.period) out.push_back(ParamKind::MosPeriod);
  if (ma.octaveStretch != mb.octaveStretch) out.push_back(ParamKind::MosStretch);
  return out;
}

Res apply_(const Snapshot& s, const SetTuningDef& c, ChangeSet& cs) {
  const int32_t i = tuningIndex(*s, c.tuning);
  if (i < 0) return notFound("tuning not found");
  const TuningSlot& old = *(*s->tunings)[static_cast<size_t>(i)];
  if (old.def == c.def) return s;
  PQ_DOC_TRY(checkTuningDef(c.def));
  TuningSlot n = old;
  n.def = c.def;
  auto np = withSlot(s, static_cast<size_t>(i), std::move(n));
  if (auto diff = continuousDiff(old.def, c.def)) {
    if (old.def.scale() && old.def.scale()->scale.description != c.def.scale()->scale.description)
      cs.mark(kChangeMeta);
    if (!diff->empty()) cs.mark(kChangeParam);
    for (ParamKind k : *diff) cs.params.push_back(ParamRef::tuning(k, c.tuning));
  } else {
    cs.mark(kChangeStructural);
    dropTuningLanes(*np, c.tuning, [&](ParamKind k) { return tuningParamApplies(c.def, k); }, cs);
  }
  cs.tunings.push_back(c.tuning);
  return Snapshot(std::move(np));
}

Res apply_(const Snapshot& s, const SetTuningParam& c, ChangeSet& cs) {
  const int32_t i = tuningIndex(*s, c.tuning);
  if (i < 0) return notFound("tuning not found");
  const TuningSlot& old = *(*s->tunings)[static_cast<size_t>(i)];
  if (isTrackParam(c.param) || !tuningParamApplies(old.def, c.param))
    return invalid("parameter does not apply to this tuning");
  PQ_DOC_TRY(checkParamValue(c.param, c.value));
  TuningDef d = old.def;
  switch (c.param) {
    case ParamKind::TuningReference: d.referenceHz = c.value; break;
    case ParamKind::TuningEquave: {
      if (d.edo()->equaveCents() == c.value) return s;
      Result<tuning::Edo> e = tuning::Edo::makeCents(d.edo()->n(), c.value);
      if (!e) return std::move(e).error();
      d.system = std::move(e).value();
      break;
    }
    case ParamKind::MosGenerator: {
      // The mode follows the generator as in the explorer (transferModeRotation: the root keeps its chain position),
      // which is also what the tuning node does when automation moves the generator (T2.6-fix2), so a live edit and
      // a reload of it play the same mode.
      tuning::MosParams& m = std::get<tuning::MosParams>(d.system);
      if (m.n > 1 && m.genSequence.empty())
        m.rotation = tuning::transferModeRotation(m.generator, m.n, m.rotation, c.value, m.n, m.period,
                                                  m.region ? &*m.region : nullptr);
      m.generator = c.value;
      break;
    }
    case ParamKind::MosPeriod: std::get<tuning::MosParams>(d.system).period = c.value; break;
    case ParamKind::MosStretch: std::get<tuning::MosParams>(d.system).octaveStretch = c.value; break;
    default: return invalid("not a tuning parameter");
  }
  if (d == old.def) return s;
  PQ_DOC_TRY(checkTuningDef(d));
  TuningSlot n = old;
  n.def = std::move(d);
  cs.mark(kChangeParam);
  cs.tunings.push_back(c.tuning);
  cs.params.push_back(ParamRef::tuning(c.param, c.tuning));
  return Snapshot(withSlot(s, static_cast<size_t>(i), std::move(n)));
}

// ---- Devices (T4.1; inside racks T4.8) -----------------------------------------------------------------------------

// ---- Device trees (T4.8): path-copying edits of a track's device list, racks and chains included ----------------
// Every helper works on a FRESH copy of a track's top-level list (Track::devices of a mutableTrack): it replaces the
// racks / chains on the way down by fresh copies and never writes a node reachable from the input snapshot.

// M4 review M9: every helper here is iterative (explicit stacks / paths on the heap), so racks nest as deep as memory
// allows; a path-copying edit finds its path first (one walk), then copies the nodes along it (O(depth)).

// Whether rack device `d` holds chain `chain` (at any depth).
bool holdsChain(const Device& d, ChainId chain) {
  bool found = false;
  forEachDeviceIn(d, [&](const Device& x) {
    if (!found && x.rack)
      for (const ChainPtr& c : x.rack->chains) found = found || c->id == chain;
  });
  return found;
}

// One step down a device tree: the device at `device` of the current list, then its rack's chain `chain`.
struct TreeStep {
  size_t device = 0;
  size_t chain = 0;
};

// Finds the device `id` (pre-order, chains in order) or, with `chain` valid, the rack holding chain `chain`: the steps
// from `list` to the list holding it, and its index there (`chainIndex`: the chain's index in that rack). False: not
// in `list`.
bool findInTree(const std::vector<DevicePtr>& list, DeviceId id, ChainId chain, std::vector<TreeStep>& path,
                size_t& index, size_t& chainIndex) {
  struct Open {
    const std::vector<DevicePtr>* list;
    size_t device = 0;
    size_t chain = 0;  // the device's next chain to look in
  };
  std::vector<Open> stack{{&list}};
  while (!stack.empty()) {
    Open& f = stack.back();
    if (f.device >= f.list->size()) {
      stack.pop_back();
      if (!stack.empty()) ++stack.back().chain;
      continue;
    }
    const Device& d = *(*f.list)[f.device];
    if (f.chain == 0) {  // first visit of this device
      bool hit = !chain.valid() && d.id == id;
      if (chain.valid() && d.rack)
        for (size_t c = 0; c < d.rack->chains.size() && !hit; ++c)
          if (d.rack->chains[c]->id == chain) {
            hit = true;
            chainIndex = c;
          }
      if (hit) {
        path.clear();
        for (size_t i = 0; i + 1 < stack.size(); ++i) path.push_back({stack[i].device, stack[i].chain});
        index = f.device;
        return true;
      }
    }
    if (d.rack && f.chain < d.rack->chains.size()) {
      const std::vector<DevicePtr>* inner = &d.rack->chains[f.chain]->devices;
      stack.push_back({inner});  // invalidates f
      continue;
    }
    ++f.device;
    f.chain = 0;
  }
  return false;
}

// Path-copies `list` along `path` (fresh copies of each device, rack and chain on the way) and returns the fresh list
// at its end.
std::vector<DevicePtr>* copyAlong(std::vector<DevicePtr>& list, const std::vector<TreeStep>& path) {
  std::vector<DevicePtr>* cur = &list;
  for (const TreeStep& s : path) {
    DevicePtr& slot = (*cur)[s.device];
    auto dev = std::make_shared<Device>(*slot);
    auto rack = std::make_shared<Rack>(*dev->rack);
    auto chain = std::make_shared<Chain>(*rack->chains[s.chain]);
    cur = &chain->devices;
    rack->chains[s.chain] = std::move(chain);
    dev->rack = std::move(rack);
    slot = std::move(dev);
  }
  return cur;
}

// Path-copies `list` down to chain `chain` and returns its (fresh, mutable) device list; `kind` gets the list's kind,
// `rack` the id of the rack holding it. nullptr: not in this list.
std::vector<DevicePtr>* mutableChainList(std::vector<DevicePtr>& list, ChainId chain, ListKind& kind, DeviceId& rackId) {
  std::vector<TreeStep> path;
  size_t index = 0, chainIndex = 0;
  if (!findInTree(list, {}, chain, path, index, chainIndex)) return nullptr;
  std::vector<DevicePtr>* holder = copyAlong(list, path);  // the fresh list holding the rack
  std::vector<DevicePtr>* out = copyAlong(*holder, std::vector<TreeStep>{{index, chainIndex}});
  const Device& rackDev = *(*holder)[index];
  kind = chainListKind(rackDev);
  rackId = rackDev.id;
  return out;
}

// Path-copies `list` down to device `id` and returns a fresh mutable copy of it (installed in the tree). nullptr: not in
// this list.
Device* mutableDevice(std::vector<DevicePtr>& list, DeviceId id) {
  std::vector<TreeStep> path;
  size_t index = 0, chainIndex = 0;
  if (!findInTree(list, id, {}, path, index, chainIndex)) return nullptr;
  std::vector<DevicePtr>* at = copyAlong(list, path);
  auto copy = std::make_shared<Device>(*(*at)[index]);
  Device* raw = copy.get();
  (*at)[index] = std::move(copy);
  return raw;
}

// A rack device's rack, made mutable (a fresh copy installed in the device).
Rack& mutableRack(Device& d) {
  auto r = std::make_shared<Rack>(*d.rack);
  Rack& ref = *r;
  d.rack = std::move(r);
  return ref;
}

// A chain of a mutable rack, made mutable.
Chain* mutableChainOf(Rack& r, ChainId id) {
  for (ChainPtr& c : r.chains)
    if (c->id == id) {
      auto copy = std::make_shared<Chain>(*c);
      Chain* raw = copy.get();
      c = std::move(copy);
      return raw;
    }
  return nullptr;
}

// Removes device `id` from the tree (path-copying on the way) and returns it (null: not here).
DevicePtr takeFromTree(std::vector<DevicePtr>& list, DeviceId id) {
  std::vector<TreeStep> path;
  size_t index = 0, chainIndex = 0;
  if (!findInTree(list, id, {}, path, index, chainIndex)) return nullptr;
  std::vector<DevicePtr>* at = copyAlong(list, path);
  DevicePtr d = (*at)[index];
  at->erase(at->begin() + static_cast<std::ptrdiff_t>(index));
  return d;
}

// Drops every mapping whose target is no longer inside its rack (after devices left a rack or were removed); their ids
// go to `gone`. Path-copies only racks that change. Post-order (inner racks first), iterative: each open level works on
// a copy of a chain's device list, which replaces the chain's list when something in it changed.
bool pruneMappings(std::vector<DevicePtr>& top, std::vector<MappingId>& gone) {
  struct Open {
    std::vector<DevicePtr>* list = nullptr;  // the list being pruned
    size_t device = 0;
    size_t chain = 0;                        // the device's next chain
    std::shared_ptr<Device> copy;            // the device's fresh copy, once it or something inside it changes
    Rack* rack = nullptr;                    // copy's fresh rack
    std::vector<DevicePtr> inner;            // the working copy of chain `chain − 1`'s devices
    bool changed = false;                    // a device of `list` was replaced
  };
  std::deque<Open> stack;
  stack.emplace_back().list = &top;
  bool changed = false;
  while (!stack.empty()) {
    Open& f = stack.back();
    if (f.device == f.list->size()) {
      const bool listChanged = f.changed;
      stack.pop_back();
      if (stack.empty()) {
        changed = listChanged;
        break;
      }
      Open& p = stack.back();
      if (listChanged) {  // the parent device's chain `chain − 1` takes the pruned list
        if (!p.copy) {
          p.copy = std::make_shared<Device>(*(*p.list)[p.device]);
          p.rack = &mutableRack(*p.copy);
        }
        auto c = std::make_shared<Chain>(*p.rack->chains[p.chain - 1]);
        c->devices = std::move(p.inner);
        p.rack->chains[p.chain - 1] = std::move(c);
      }
      continue;
    }
    const DevicePtr& d = (*f.list)[f.device];
    if (!d->rack) {
      ++f.device;
      continue;
    }
    const Rack& cur = f.copy ? *f.rack : *d->rack;
    if (f.chain < cur.chains.size()) {
      f.inner = cur.chains[f.chain++]->devices;
      Open& c = stack.emplace_back();  // `f` stays valid (deque), and so does f.inner while the level above runs
      c.list = &f.inner;
      continue;
    }
    const Device& now = f.copy ? *f.copy : *d;
    const auto keep = [&](const MacroMapping& m) {
      return m.device == now.id ? m.param == kSelectParam : deviceContains(now, m.device);
    };
    if (!std::all_of(now.rack->mappings.begin(), now.rack->mappings.end(), keep)) {
      if (!f.copy) {
        f.copy = std::make_shared<Device>(*d);
        f.rack = &mutableRack(*f.copy);
      }
      for (const MacroMapping& m : f.rack->mappings)
        if (!keep(m)) gone.push_back(m.id);
      std::erase_if(f.rack->mappings, [&](const MacroMapping& m) { return !keep(m); });
    }
    if (f.copy) {
      (*f.list)[f.device] = std::move(f.copy);
      f.rack = nullptr;
      f.changed = true;
    }
    ++f.device;
    f.chain = 0;
  }
  return changed;
}

// The ids of `d` and of every device inside it.
std::vector<DeviceId> deviceIdsIn(const Device& d) {
  std::vector<DeviceId> out;
  forEachDeviceIn(d, [&](const Device& x) { out.push_back(x.id); });
  std::sort(out.begin(), out.end());
  return out;
}

// T4.8: the list a device goes into: a track's own chain, or a rack chain.
struct ListAt {
  TrackPath path;
  const Track* track = nullptr;
  const std::vector<DevicePtr>* list = nullptr;
  ListKind kind = ListKind::OtherTrack;
  ChainId chain;  // invalid: the track's own list
};
Result<ListAt> locateList(const Project& p, TrackId track, ChainId chain) {
  ListAt out;
  if (chain.valid()) {
    const ChainLoc c = findChain(p, chain);
    if (!c.chain) return notFound("chain not found");
    out.track = c.track;
    out.list = &c.chain->devices;
    out.kind = chainListKind(*c.rack);
    out.chain = chain;
  } else {
    out.track = findTrackPtr(p, track);
    if (!out.track) return notFound("track not found");
    out.list = &out.track->devices;
    out.kind = listKindOf(out.track->kind);
  }
  out.path = *findTrack(p, out.track->id);
  return out;
}

// The fresh list of `at` inside the fresh track `t`.
std::vector<DevicePtr>* listIn(Track& t, const ListAt& at) {
  if (!at.chain.valid()) return &t.devices;
  ListKind kind{};
  DeviceId rack;
  return mutableChainList(t.devices, at.chain, kind, rack);
}

struct DevLoc {
  TrackPath path;
  const Track* track = nullptr;
  uint32_t index = 0;
  const Device* device = nullptr;
};
Result<DevLoc> locateDevice(const Project& p, DeviceId id) {
  const DeviceLoc d = findDevice(p, id);
  if (!d.track) return notFound("device not found");
  const auto path = findTrack(p, d.track->id);
  return DevLoc{*path, d.track, d.index, d.device};
}

// Edits device `id` (anywhere in its track) through f(Device&) on a fresh copy.
template <class F>
Res editDeviceAt(const Snapshot& s, DeviceId id, ChangeSet& cs, ChangeFlag flag, F&& f) {
  Result<DevLoc> loc = locateDevice(*s, id);
  if (!loc) return std::move(loc).error();
  Res r = editTrack(s, loc->path, cs, flag, [&](Track& t) { return f(*mutableDevice(t.devices, id)); });
  if (r) cs.devices.push_back(id);
  return r;
}

Res apply_(const Snapshot& s, const AddDevice& c, ChangeSet& cs) {
  if (!c.device) return invalid("no device");
  Result<ListAt> at = locateList(*s, c.track, c.chain);
  if (!at) return std::move(at).error();
  std::vector<uint32_t> ids;
  PQ_DOC_TRY(checkNewDeviceIds(*s, std::vector<DevicePtr>{c.device}, ids));
  std::sort(ids.begin(), ids.end());
  if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()) return invalid("an id twice inside the device");
  std::vector<DevicePtr> chain = *at->list;
  const size_t index = std::min<size_t>(c.index, chain.size());
  chain.insert(chain.begin() + static_cast<std::ptrdiff_t>(index), c.device);
  PQ_DOC_TRY(checkList(at->kind, chain));
  Res r = editTrack(s, at->path, cs, kChangeStructural, [&](Track& t) {
    *listIn(t, *at) = std::move(chain);
    return ok();
  });
  if (r) forEachDeviceIn(*c.device, [&](const Device& d) { cs.devices.push_back(d.id); });
  return r;
}

Res apply_(const Snapshot& s, const RemoveDevice& c, ChangeSet& cs) {
  Result<DevLoc> loc = locateDevice(*s, c.device);
  if (!loc) return std::move(loc).error();
  const std::vector<DeviceId> gone = deviceIdsIn(*loc->device);  // T4.8: a rack with everything inside it
  std::vector<MappingId> pruned;
  Res r = editTrack(s, loc->path, cs, kChangeStructural, [&](Track& t) {
    (void)takeFromTree(t.devices, c.device);
    pruneMappings(t.devices, pruned);  // outer racks' mappings onto what was removed
    return ok();
  });
  if (!r) return r;
  cs.devices.insert(cs.devices.end(), gone.begin(), gone.end());
  const WireList& wires = wiresOf(**r);
  const auto touches = [&](const Wire& w) {
    return std::binary_search(gone.begin(), gone.end(), w.from) || std::binary_search(gone.begin(), gone.end(), w.to);
  };
  if (std::none_of(wires.begin(), wires.end(), touches)) return r;
  auto np = std::make_shared<Project>(**r);  // T4.7: its wires go with it (one command, one undo step)
  dropWiresOf(*np, gone, cs);
  return Snapshot(std::move(np));
}

Res apply_(const Snapshot& s, const MoveDevice& c, ChangeSet& cs) {
  Result<DevLoc> from = locateDevice(*s, c.device);
  if (!from) return std::move(from).error();
  const DeviceLoc src = findDevice(*s, c.device);
  const DevicePtr dev = src.list()[src.index];
  const TrackId toId = c.toTrack.valid() ? c.toTrack : from->track->id;
  Result<ListAt> to = locateList(*s, toId, c.toChain);
  if (!to) return std::move(to).error();
  if (c.toChain.valid() && holdsChain(*dev, c.toChain)) return invalid("a rack cannot go into one of its own chains");
  const bool sameList = to->list == &src.list();
  if (sameList) {
    std::vector<DevicePtr> chain = *to->list;
    chain.erase(chain.begin() + src.index);
    const size_t at = std::min<size_t>(c.index, chain.size());
    if (at == src.index) return s;  // where it is
    chain.insert(chain.begin() + static_cast<std::ptrdiff_t>(at), dev);
    PQ_DOC_TRY(checkList(to->kind, chain));
    Res r = editTrack(s, to->path, cs, kChangeStructural, [&](Track& t) {
      *listIn(t, *to) = std::move(chain);
      return ok();
    });
    if (r) cs.devices.push_back(c.device);
    return r;
  }
  {  // the destination list with the device in it must hold
    std::vector<DevicePtr> dst = *to->list;
    dst.insert(dst.begin() + static_cast<std::ptrdiff_t>(std::min<size_t>(c.index, dst.size())), dev);
    PQ_DOC_TRY(checkList(to->kind, dst));
  }
  auto np = copyOf(s);
  std::vector<MappingId> pruned;
  // No Track& is held across a mutableTrack call (M4 review H1): mutableTrack re-copies every ancestor on its path, so
  // when the destination is a child of the source group, a reference to the source taken before the second call points
  // at a copy the new ancestor chain no longer owns (freed). Each step re-resolves its track; the paths stay valid
  // because only device lists change. The extra Track copies cost a few hundred bytes on the UI thread.
  // The source first (removing may path-copy the racks on the way to the destination too, when both are in one track).
  (void)takeFromTree(mutableTrack(*np, from->path).devices, c.device);
  std::vector<DevicePtr>* dst = listIn(mutableTrack(*np, to->path), *to);
  if (dst == nullptr) return invalid("the destination chain is inside the moved device");
  dst->insert(dst->begin() + static_cast<std::ptrdiff_t>(std::min<size_t>(c.index, dst->size())), dev);
  pruneMappings(mutableTrack(*np, from->path).devices, pruned);  // mappings of the racks it left
  cs.mark(kChangeStructural);
  cs.tracks.push_back(from->track->id);
  cs.tracks.push_back(to->track->id);
  forEachDeviceIn(*dev, [&](const Device& d) { cs.devices.push_back(d.id); });
  return Snapshot(std::move(np));
}

Res apply_(const Snapshot& s, const SetDeviceActive& c, ChangeSet& cs) {
  const DeviceLoc d = findDevice(*s, c.device);
  if (!d.track) return notFound("device not found");
  if (d.device->active == c.active) return s;
  return editDeviceAt(s, c.device, cs, kChangeStructural, [&](Device& x) {
    x.active = c.active;
    return ok();
  });
}

// Sets or clears (nullopt) `name` in a sorted value list; false when nothing changes.
bool setValue(std::vector<DeviceValue>& v, const std::string& name, const std::optional<double>& value) {
  const auto it = std::lower_bound(v.begin(), v.end(), name,
                                   [](const DeviceValue& a, const std::string& n) { return a.name < n; });
  const bool present = it != v.end() && it->name == name;
  if (!value) {
    if (!present) return false;
    v.erase(it);
    return true;
  }
  if (present) {
    if (it->value == *value) return false;
    it->value = *value;
    return true;
  }
  v.insert(it, DeviceValue{name, *value});
  return true;
}

Res apply_(const Snapshot& s, const SetDeviceParam& c, ChangeSet& cs) {
  const DeviceLoc loc = findDevice(*s, c.device);
  if (!loc.track) return notFound("device not found");
  PQ_DOC_TRY(checkDeviceValue(c.param, c.value));
  if (loc.device->rack) {  // T4.8: a macro or the chain selector
    if (!isRackParam(c.param)) return invalid("'" + c.param + "' is not a rack param (macro1 … macro16, select)");
    if (c.value && !finiteIn(*c.value, 0.0, kMacroMax)) return invalid("rack params are in [0, 127]");
  }
  std::vector<DeviceValue> params = loc.device->params;
  if (!setValue(params, c.param, c.value)) return s;
  cs.params.push_back(ParamRef::device(c.device, c.param));
  return editDeviceAt(s, c.device, cs, kChangeParam, [&](Device& d) {
    d.params = std::move(params);
    return ok();
  });
}

Res apply_(const Snapshot& s, const SetDeviceSetting& c, ChangeSet& cs) {
  const DeviceLoc loc = findDevice(*s, c.device);
  if (!loc.track) return notFound("device not found");
  PQ_DOC_TRY(checkDeviceValue(c.setting, c.value));
  if (loc.device->rack) return invalid("racks have no settings");
  std::vector<DeviceValue> settings = loc.device->settings;
  if (!setValue(settings, c.setting, c.value)) return s;
  return editDeviceAt(s, c.device, cs, kChangeStructural, [&](Device& d) {
    d.settings = std::move(settings);
    return ok();
  });
}

// ---- Racks (T4.8) -----------------------------------------------------------------------------------------------

struct RackLoc {
  const Device* rack = nullptr;
  const Track* track = nullptr;
};
Result<RackLoc> locateRack(const Project& p, DeviceId id) {
  const DeviceLoc d = findDevice(p, id);
  if (!d.track) return notFound("rack not found");
  if (!d.device->rack) return invalid("the device is not a rack");
  return RackLoc{d.device, d.track};
}

// Edits the chain `id` (fresh copy) of its rack (fresh copy) through f(Rack&, Chain&).
template <class F>
Res editChain(const Snapshot& s, ChainId id, ChangeSet& cs, ChangeFlag flag, F&& f) {
  const ChainLoc c = findChain(*s, id);
  if (!c.chain) return notFound("chain not found");
  return editDeviceAt(s, c.rack->id, cs, flag, [&](Device& rack) {
    Rack& r = mutableRack(rack);
    return f(r, *mutableChainOf(r, id));
  });
}

Res apply_(const Snapshot& s, const AddChain& c, ChangeSet& cs) {
  if (!c.chain) return invalid("no chain");
  Result<RackLoc> rl = locateRack(*s, c.rack);
  if (!rl) return std::move(rl).error();
  if (!c.chain->id.valid() || findChain(*s, c.chain->id).chain) return invalid("chain id missing or already in use");
  std::vector<uint32_t> ids{c.chain->id.value};
  PQ_DOC_TRY(checkNewDeviceIds(*s, c.chain->devices, ids));
  std::sort(ids.begin(), ids.end());
  if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()) return invalid("an id twice inside the chain");
  Res r = editDeviceAt(s, c.rack, cs, kChangeStructural, [&](Device& rack) {
    Rack& rr = mutableRack(rack);
    rr.chains.insert(rr.chains.begin() + static_cast<std::ptrdiff_t>(std::min<size_t>(c.index, rr.chains.size())), c.chain);
    return checkRack(rack);
  });
  if (r) forEachDeviceIn(c.chain->devices, [&](const Device& d) { cs.devices.push_back(d.id); });
  return r;
}

Res apply_(const Snapshot& s, const RemoveChain& c, ChangeSet& cs) {
  const ChainLoc loc = findChain(*s, c.chain);
  if (!loc.chain) return notFound("chain not found");
  std::vector<DeviceId> gone;
  forEachDeviceIn(loc.chain->devices, [&](const Device& d) { gone.push_back(d.id); });
  std::sort(gone.begin(), gone.end());
  const DeviceId rackId = loc.rack->id;
  std::vector<MappingId> pruned;
  Res r = editTrack(s, *findTrack(*s, loc.track->id), cs, kChangeStructural, [&](Track& t) {
    Rack& rr = mutableRack(*mutableDevice(t.devices, rackId));
    std::erase_if(rr.chains, [&](const ChainPtr& x) { return x->id == c.chain; });
    pruneMappings(t.devices, pruned);
    return ok();
  });
  if (!r) return r;
  cs.devices.push_back(rackId);
  cs.devices.insert(cs.devices.end(), gone.begin(), gone.end());
  const WireList& wires = wiresOf(**r);
  const auto touches = [&](const Wire& w) {
    return std::binary_search(gone.begin(), gone.end(), w.from) || std::binary_search(gone.begin(), gone.end(), w.to);
  };
  if (std::none_of(wires.begin(), wires.end(), touches)) return r;
  auto np = std::make_shared<Project>(**r);
  dropWiresOf(*np, gone, cs);
  return Snapshot(std::move(np));
}

Res apply_(const Snapshot& s, const MoveChain& c, ChangeSet& cs) {
  const ChainLoc loc = findChain(*s, c.chain);
  if (!loc.chain) return notFound("chain not found");
  const size_t n = loc.rack->rack->chains.size();
  const size_t at = std::min<size_t>(c.index, n - 1);
  if (at == loc.index) return s;
  return editDeviceAt(s, loc.rack->id, cs, kChangeStructural, [&](Device& rack) {
    Rack& rr = mutableRack(rack);
    ChainPtr moved = rr.chains[loc.index];
    rr.chains.erase(rr.chains.begin() + loc.index);
    rr.chains.insert(rr.chains.begin() + static_cast<std::ptrdiff_t>(at), std::move(moved));
    return ok();
  });
}

Res apply_(const Snapshot& s, const RenameChain& c, ChangeSet& cs) {
  const ChainLoc loc = findChain(*s, c.chain);
  if (!loc.chain) return notFound("chain not found");
  if (loc.chain->name == c.name) return s;
  return editChain(s, c.chain, cs, kChangeMeta, [&](Rack&, Chain& ch) {
    ch.name = c.name;
    return ok();
  });
}

Res apply_(const Snapshot& s, const SetChainMixer& c, ChangeSet& cs) {
  if (!finiteIn(c.volumeDb, kMinVolumeDb, kMaxVolumeDb)) return invalid("chain volume out of range");
  if (!finiteIn(c.pan, -1.0, 1.0)) return invalid("chain pan out of range");
  const ChainLoc loc = findChain(*s, c.chain);
  if (!loc.chain) return notFound("chain not found");
  if (loc.chain->volumeDb == c.volumeDb && loc.chain->pan == c.pan) return s;
  cs.params.push_back(ParamRef::chain(c.chain, 0));
  return editChain(s, c.chain, cs, kChangeParam, [&](Rack&, Chain& ch) {
    ch.volumeDb = c.volumeDb;
    ch.pan = c.pan;
    return ok();
  });
}

Res apply_(const Snapshot& s, const SetChainActive& c, ChangeSet& cs) {
  const ChainLoc loc = findChain(*s, c.chain);
  if (!loc.chain) return notFound("chain not found");
  if (loc.chain->active == c.active) return s;
  return editChain(s, c.chain, cs, kChangeStructural, [&](Rack&, Chain& ch) {
    ch.active = c.active;
    return ok();
  });
}

Res apply_(const Snapshot& s, const SetChainSolo& c, ChangeSet& cs) {
  const ChainLoc loc = findChain(*s, c.chain);
  if (!loc.chain) return notFound("chain not found");
  if (loc.chain->solo == c.solo) return s;
  return editChain(s, c.chain, cs, kChangeStructural, [&](Rack&, Chain& ch) {
    ch.solo = c.solo;
    return ok();
  });
}

Res apply_(const Snapshot& s, const SetChainZone& c, ChangeSet& cs) {
  const ChainLoc loc = findChain(*s, c.chain);
  if (!loc.chain) return notFound("chain not found");
  if (static_cast<uint8_t>(c.kind) > static_cast<uint8_t>(ZoneKind::Select)) return invalid("unknown zone kind");
  const KeyZoneUnit unit = loc.rack->rack->keyUnit;
  if (!zoneValid(c.zone, c.kind, unit)) return invalid("zone out of order or out of range");
  if (c.kind != ZoneKind::Select && loc.rack->kind != DeviceKind::Instrument && !zoneOff(c.zone, c.kind, unit))
    return invalid("key and velocity zones are Instrument Rack zones");
  const Zone& old = loc.chain->zone(c.kind);
  if (old == c.zone) return s;
  // A zone that stays on (or off) is a runtime value; switching it on or off adds or removes its nodes.
  const bool structural = zoneOff(old, c.kind, unit) != zoneOff(c.zone, c.kind, unit);
  if (!structural) cs.params.push_back(ParamRef::chain(c.chain, 1 + static_cast<uint32_t>(c.kind)));
  return editChain(s, c.chain, cs, structural ? kChangeStructural : kChangeParam, [&](Rack&, Chain& ch) {
    ch.zone(c.kind) = c.zone;
    return ok();
  });
}

Res apply_(const Snapshot& s, const AddMacroMapping& c, ChangeSet& cs) {
  PQ_DOC_TRY(checkMapping(c.mapping));
  Result<RackLoc> rl = locateRack(*s, c.rack);
  if (!rl) return std::move(rl).error();
  if (findMapping(*s, c.mapping.id).mapping) return invalid("mapping id already in use");
  if (const MappingLoc m = mappingOf(*s, c.mapping.device, c.mapping.param); m.mapping)
    return invalid("this param is already mapped to a macro");
  const DeviceLoc target = findDevice(*s, c.mapping.device);
  if (!target.device) return notFound("mapping target device not found");
  if (target.device->rack && !isRackParam(c.mapping.param)) return invalid("a rack's params are its macros and selector");
  Res r = editDeviceAt(s, c.rack, cs, kChangeStructural, [&](Device& rack) {
    Rack& rr = mutableRack(rack);
    rr.mappings.insert(std::upper_bound(rr.mappings.begin(), rr.mappings.end(), c.mapping.id,
                                        [](MappingId a, const MacroMapping& b) { return a < b.id; }),
                       c.mapping);
    return checkRack(rack);
  });
  if (r) cs.devices.push_back(c.mapping.device);
  return r;
}

Res apply_(const Snapshot& s, const RemoveMacroMapping& c, ChangeSet& cs) {
  const MappingLoc m = findMapping(*s, c.mapping);
  if (!m.mapping) return notFound("mapping not found");
  const DeviceId target = m.mapping->device;
  Res r = editDeviceAt(s, m.rack->id, cs, kChangeStructural, [&](Device& rack) {
    std::erase_if(mutableRack(rack).mappings, [&](const MacroMapping& x) { return x.id == c.mapping; });
    return ok();
  });
  if (r) cs.devices.push_back(target);
  return r;
}

Res apply_(const Snapshot& s, const SetMacroMapping& c, ChangeSet& cs) {
  PQ_DOC_TRY(checkMapping(c.mapping));
  const MappingLoc m = findMapping(*s, c.mapping.id);
  if (!m.mapping) return notFound("mapping not found");
  if (m.mapping->device != c.mapping.device || m.mapping->param != c.mapping.param)
    return invalid("a mapping's target cannot change (remove it and map again)");
  if (*m.mapping == c.mapping) return s;
  Res r = editDeviceAt(s, m.rack->id, cs, kChangeStructural, [&](Device& rack) {
    for (MacroMapping& x : mutableRack(rack).mappings)
      if (x.id == c.mapping.id) x = c.mapping;
    return ok();
  });
  if (r) cs.devices.push_back(c.mapping.device);
  return r;
}

Res apply_(const Snapshot& s, const SetRackOptions& c, ChangeSet& cs) {
  Result<RackLoc> rl = locateRack(*s, c.rack);
  if (!rl) return std::move(rl).error();
  const Rack& old = *rl->rack->rack;
  const RackOptions& o = c.options;
  if (rackOptions(old) == o) return s;
  const bool structural = o.keyUnit != old.keyUnit || o.voiceChains != old.voiceChains;
  return editDeviceAt(s, c.rack, cs, structural ? kChangeStructural : kChangeMeta, [&](Device& rack) {
    Rack& r = mutableRack(rack);
    r.macroNames = o.macroNames;
    r.visibleMacros = o.visibleMacros;
    r.variations = o.variations;
    r.randomizeExclude = o.randomizeExclude;
    r.showChains = o.showChains;
    r.showMacros = o.showMacros;
    r.voiceChains = o.voiceChains;
    if (o.keyUnit != r.keyUnit) {  // the key zones meant another unit: back to off
      r.keyUnit = o.keyUnit;
      for (ChainPtr& ch : r.chains) {
        auto copy = std::make_shared<Chain>(*ch);
        copy->key = fullZone(ZoneKind::Key, o.keyUnit);
        ch = std::move(copy);
      }
    }
    return checkRack(rack);
  });
}

// T4.4-fix1: a user device's text (fork / edit) or back to the stock patch (null).
Res apply_(const Snapshot& s, const SetDevicePatch& c, ChangeSet& cs) {
  const DeviceLoc loc = findDevice(*s, c.device);  // T4.8: at any depth (a device inside a rack)
  if (!loc.track) return notFound("device not found");
  const Device& old = *loc.device;
  if (old.rack) return invalid("a rack has no patch text");
  if (c.source && c.source->text.empty())
    return Error{ErrorCode::InvalidArgument, "a device patch text must not be empty", {}};
  if (old.source == c.source || (old.source && c.source && *old.source == *c.source)) return s;
  return editDeviceAt(s, c.device, cs, kChangeStructural, [&](Device& d) {
    d.source = c.source;
    return ok();
  });
}

// ---- Modulation wires (T4.7) --------------------------------------------------------------------------------------

// np's wire list without the wires touching a device of `gone` (sorted); their ids into cs. No-op without such wires.
void dropWiresOf(Project& np, const std::vector<DeviceId>& gone, ChangeSet& cs) {
  if (!np.wires || gone.empty()) return;
  auto touches = [&](const Wire& w) {
    return std::binary_search(gone.begin(), gone.end(), w.from) || std::binary_search(gone.begin(), gone.end(), w.to);
  };
  if (std::none_of(np.wires->begin(), np.wires->end(), touches)) return;
  WireList keep;
  for (const Wire& w : *np.wires) {
    if (touches(w)) cs.wires.push_back(w.id);
    else keep.push_back(w);
  }
  np.wires = keep.empty() ? nullptr : std::make_shared<const WireList>(std::move(keep));
}

Res withWires(const Snapshot& s, WireList list, ChangeSet& cs, ChangeFlag flag, WireId touched) {
  auto np = copyOf(s);
  np->wires = list.empty() ? nullptr : std::make_shared<const WireList>(std::move(list));
  cs.mark(flag);
  cs.wires.push_back(touched);
  return Snapshot(std::move(np));
}

Res apply_(const Snapshot& s, const AddWire& c, ChangeSet& cs) {
  PQ_DOC_TRY(checkWire(c.wire));
  if (!findDevice(*s, c.wire.from).track) return notFound("wire source device not found");
  if (!findDevice(*s, c.wire.to).track) return notFound("wire target device not found");
  const WireList& cur = wiresOf(*s);
  for (const Wire& w : cur) {
    if (w.id == c.wire.id) return invalid("wire id already in use");
    if (w.from == c.wire.from && w.port == c.wire.port && w.to == c.wire.to && w.param == c.wire.param)
      return invalid("these two ports are already wired");
  }
  WireList list = cur;
  list.insert(std::upper_bound(list.begin(), list.end(), c.wire.id, [](WireId a, const Wire& b) { return a < b.id; }),
              c.wire);
  return withWires(s, std::move(list), cs, kChangeStructural, c.wire.id);
}

Res apply_(const Snapshot& s, const RemoveWire& c, ChangeSet& cs) {
  if (!findWire(*s, c.wire)) return notFound("wire not found");
  WireList list = wiresOf(*s);
  std::erase_if(list, [&](const Wire& w) { return w.id == c.wire; });
  return withWires(s, std::move(list), cs, kChangeStructural, c.wire);
}

template <class F>
Res editWire(const Snapshot& s, WireId id, ChangeSet& cs, ChangeFlag flag, F&& f) {
  const Wire* old = findWire(*s, id);
  if (!old) return notFound("wire not found");
  Wire w = *old;
  f(w);
  if (w == *old) return s;
  PQ_DOC_TRY(checkWire(w));
  WireList list = wiresOf(*s);
  for (Wire& x : list)
    if (x.id == id) x = w;
  return withWires(s, std::move(list), cs, flag, id);
}

Res apply_(const Snapshot& s, const SetWireAmount& c, ChangeSet& cs) {
  if (!finiteIn(c.amount, -1.0, 1.0)) return invalid("wire amount must be in [-1, 1]");
  Res r = editWire(s, c.wire, cs, kChangeParam, [&](Wire& w) { w.amount = c.amount; });
  if (r && *r != s) cs.params.push_back(ParamRef::wireAmount(c.wire));
  return r;
}

Res apply_(const Snapshot& s, const SetWireReducer& c, ChangeSet& cs) {
  return editWire(s, c.wire, cs, kChangeStructural, [&](Wire& w) { w.reducer = c.reducer; });
}

// ---- Batch ---------------------------------------------------------------------------------------------------------

Res applyAny(const Snapshot& s, const Command& c, ChangeSet& cs);

Res apply_(const Snapshot& s, const Batch& c, ChangeSet& cs) {
  Snapshot cur = s;
  ChangeSet acc;
  for (size_t i = 0; i < c.commands.size(); ++i) {
    Res r = applyAny(cur, c.commands[i], acc);
    if (!r) {
      Error e = std::move(r).error();
      e.message = std::string(label(c.commands[i])) + " (step " + std::to_string(i + 1) + " of " + c.label +
                  "): " + e.message;
      return e;
    }
    cur = std::move(r).value();
  }
  if (cur != s) cs.merge(acc);
  return cur;
}

Res applyAny(const Snapshot& s, const Command& c, ChangeSet& cs) {
  return std::visit([&](const auto& cmd) { return apply_(s, cmd, cs); }, c.v);
}

// Labels in CommandVariant order.
constexpr std::array<std::string_view, std::variant_size_v<CommandVariant>> kLabels = {
    "Add Track", "Delete Track", "Move Track", "Rename Track", "Track Colour", "Set Volume", "Set Pan",
    "Mute", "Solo", "Arm", "Set Send", "Send Pre/Post", "Set Output", "Activate Track", "Track Tuning",
    "Group Tracks", "Ungroup Tracks",
    "Add Clip", "Delete Clip", "Move Clip", "Resize Clip", "Clip Loop", "Clip Tuning", "Rename Clip",
    "Clip Colour", "Clip Signature", "Activate Clip", "Clip Fundamental", "Edit Audio Clip", "Add Sample",
    "Delete Sample", "Edit Sample",
    "Add Notes", "Delete Notes", "Edit Notes", "Move Note", "Resize Note", "Note Property",
    "Add Automation Lane", "Delete Automation Lane", "Add Breakpoints", "Delete Breakpoints", "Edit Breakpoints",
    "Tempo", "Time Signature", "Loop", "Add Locator", "Delete Locator", "Edit Locator",
    "Add Tuning", "Delete Tuning", "Rename Tuning", "Tuning Colour", "Edit Tuning", "Tuning Parameter",
    "Add Device", "Delete Device", "Move Device", "Activate Device", "Device Parameter", "Device Setting",
    "Edit Device Patch",
    "Add Wire", "Delete Wire", "Wire Amount", "Wire Reducer",
    "Add Chain", "Delete Chain", "Move Chain", "Rename Chain", "Chain Mixer", "Activate Chain", "Solo Chain", "Chain Zone",
    "Map Macro", "Unmap Macro", "Edit Macro Mapping", "Rack Options",
    "Batch"};

}  // namespace

RackOptions rackOptions(const Rack& r) {
  RackOptions o;
  o.macroNames = r.macroNames;
  o.visibleMacros = r.visibleMacros;
  o.variations = r.variations;
  o.randomizeExclude = r.randomizeExclude;
  o.keyUnit = r.keyUnit;
  o.voiceChains = r.voiceChains;
  o.showChains = r.showChains;
  o.showMacros = r.showMacros;
  return o;
}

std::string_view label(const Command& c) noexcept {
  if (const Batch* b = std::get_if<Batch>(&c.v)) return b->label.empty() ? kLabels.back() : std::string_view(b->label);
  return kLabels[c.v.index()];
}

namespace {
// T5.3: after the whole command (a Batch's steps may separate two clips and join them again), the crossfades whose
// clips are no longer adjacent are dropped, on the audio tracks the command touched.
Snapshot finishCrossfades(const Snapshot& s, ChangeSet& cs) {
  std::shared_ptr<Project> np;
  for (const TrackId id : cs.tracks) {
    const auto path = findTrack(np ? *np : *s, id);
    if (!path) continue;
    const Track& t = trackAt(np ? *np : *s, *path);
    if (t.kind != TrackKind::Audio) continue;
    bool loose = false;
    for (size_t i = 0; i < t.clips.size() && !loose; ++i) {
      const ClipSlot& sl = t.clips[i];
      loose = sl.audio && sl.audio->crossfadeIn != 0 && (i == 0 || t.clips[i - 1].end() != sl.audio->start);
    }
    if (!loose) continue;
    if (!np) np = copyOf(s);
    dropLooseCrossfades(mutableTrack(*np, *path), cs);
  }
  return np ? Snapshot(std::move(np)) : s;
}
}  // namespace

Result<Snapshot> apply(const Snapshot& s, const Command& c, ChangeSet& changes) {
  Res r = applyAny(s, c, changes);
  if (!r) return r;
  changes.normalize();
  if (*r != s && changes.has(kChangeSequence)) {
    Snapshot done = finishCrossfades(*r, changes);
    changes.normalize();
    return done;
  }
  return r;
}

}  // namespace pq::doc
