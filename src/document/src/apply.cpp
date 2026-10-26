// Command application (T3.4). Each command kind has one apply_ overload below. The pattern is always:
//   1. look up and validate against the input snapshot (nothing copied yet);
//   2. return the input snapshot unchanged if the command would not change anything;
//   3. copy the Project (a handful of pointers and two small vectors), path-copy down to the edited node with
//      mutableTrack()/mutableList(), edit the fresh copies, and record the ChangeSet.
// Nothing reachable from the input snapshot is ever written.

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

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
  for (const DevicePtr& d : t.devices) cs.devices.push_back(d->id);
  for (const TrackPtr& c : t.children) collectIds(*c, cs);
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
  if (t.kind == TrackKind::Audio) return unsupported("audio tracks arrive in M6");
  if (!t.children.empty() && t.kind != TrackKind::Group) return invalid("only groups have child tracks");
  if (!t.clips.empty() && t.kind != TrackKind::Midi) return invalid("only MIDI tracks hold note clips");
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
    if (!c.clip || c.clip->id != c.id) return invalid("clip slot does not match its clip");
    PQ_DOC_TRY(checkClip(p, *c.clip));
    if (i > 0) {
      const NoteClip& a = *t.clips[i - 1].clip;
      if (std::pair(a.start, a.id) >= std::pair(c.clip->start, c.id)) return invalid("clips must be sorted");
    }
  }
  for (const LanePtr& l : t.lanes) {
    if (!l || !l->id.valid()) return invalid("lane has no id");
    PQ_DOC_TRY(checkLaneParam(p, t, l->param));
    PQ_DOC_TRY(checkPoints(l->param.kind, l->points));
    if (!std::is_sorted(l->points.begin(), l->points.end(), pointLess)) return invalid("breakpoints must be sorted");
  }
  PQ_DOC_TRY(checkChain(t.kind, t.devices));
  for (const DevicePtr& d : t.devices) {
    if (findDevice(p, d->id).track) return invalid("device id already in use");
    ids.push_back(d->id.value);  // unique within the subtree too (one id counter for every entity)
  }
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
  return a.clip->start != b.clip->start ? a.clip->start < b.clip->start : a.id < b.id;
}
void insertClipSorted(std::vector<ClipSlot>& v, ClipSlot s) {
  v.insert(std::upper_bound(v.begin(), v.end(), s, clipLess), std::move(s));
}

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
  dst.insert(dst.begin() + static_cast<std::ptrdiff_t>(at), c.track);
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
  const ClipPtr* clip;
};
Result<ClipLoc> locateClip(const Project& p, const ClipRef& r) {
  Result<Loc> loc = locate(p, r.track);
  if (!loc) return std::move(loc).error();
  const int32_t i = findClipIndex(*loc->track, r.clip);
  if (i < 0) return notFound("clip not found on this track");
  return ClipLoc{loc->path, loc->track, i, &loc->track->clips[static_cast<size_t>(i)].clip};
}

// Replaces one clip. f(const NoteClip&) → Result<ClipPtr>, nullptr = unchanged.
template <class F>
Res editClip(const Snapshot& s, const ClipRef& r, ChangeSet& cs, ChangeFlag flag, F&& f) {
  Result<ClipLoc> loc = locateClip(*s, r);
  if (!loc) return std::move(loc).error();
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

Res apply_(const Snapshot& s, const AddClip& c, ChangeSet& cs) {
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
    return editClipHeader(s, c.clip, cs, kChangeSequence, [&](NoteClip& n) { n.start = c.start; });
  }
  Result<ClipLoc> loc = locateClip(*s, c.clip);
  if (!loc) return std::move(loc).error();
  Result<Loc> to = locate(*s, c.toTrack);
  if (!to) return std::move(to).error();
  if (to->track->kind != TrackKind::Midi) return invalid("note clips live on MIDI tracks");
  ClipPtr moved = *loc->clip;
  if (moved->start != c.start) {
    NoteClip n = withNotes(*moved, moved->notes);
    n.start = c.start;
    moved = std::make_shared<const NoteClip>(std::move(n));
  }
  auto np = copyOf(s);
  Track& from = mutableTrack(*np, loc->path);
  from.clips.erase(from.clips.begin() + loc->index);
  Track& dst = mutableTrack(*np, to->path);  // paths are unchanged: only clip lists were edited
  insertClipSorted(dst.clips, ClipSlot{c.clip.clip, std::move(moved)});
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
  Res r = editClipHeader(s, c.clip, cs, kChangeStructural, [&](NoteClip& n) { n.tuning = c.tuning; });
  if (r && *r != s && c.tuning.mode == TuningMode::Main) cs.tunings.push_back(c.tuning.main);
  return r;
}

Res apply_(const Snapshot& s, const RenameClip& c, ChangeSet& cs) {
  return editClipHeader(s, c.clip, cs, kChangeMeta, [&](NoteClip& n) { n.name = c.name; });
}

Res apply_(const Snapshot& s, const SetClipColour& c, ChangeSet& cs) {
  return editClipHeader(s, c.clip, cs, kChangeMeta, [&](NoteClip& n) { n.colour = c.colour; });
}

Res apply_(const Snapshot& s, const SetClipSignature& c, ChangeSet& cs) {
  return editClipHeader(s, c.clip, cs, kChangeMeta, [&](NoteClip& n) { n.signature = c.signature; });
}

Res apply_(const Snapshot& s, const SetClipFundamental& c, ChangeSet& cs) {
  return editClipHeader(s, c.clip, cs, kChangeMeta, [&](NoteClip& n) { n.fundamental = c.fundamental; });
}

Res apply_(const Snapshot& s, const SetClipActive& c, ChangeSet& cs) {
  return editClipHeader(s, c.clip, cs, kChangeSequence, [&](NoteClip& n) { n.active = c.active; });
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
      if (uses(cl.clip->tuning)) used = true;
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

// ---- Devices (T4.1) ------------------------------------------------------------------------------------------------

struct DevLoc {
  TrackPath path;
  const Track* track = nullptr;
  uint32_t index = 0;
};
Result<DevLoc> locateDevice(const Project& p, DeviceId id) {
  const DeviceLoc d = findDevice(p, id);
  if (!d.track) return notFound("device not found");
  const auto path = findTrack(p, d.track->id);
  return DevLoc{*path, d.track, d.index};
}

Res apply_(const Snapshot& s, const AddDevice& c, ChangeSet& cs) {
  if (!c.device) return invalid("no device");
  Result<Loc> loc = locate(*s, c.track);
  if (!loc) return std::move(loc).error();
  if (findDevice(*s, c.device->id).track) return invalid("device id already in use");
  std::vector<DevicePtr> chain = loc->track->devices;
  const size_t at = std::min<size_t>(c.index, chain.size());
  chain.insert(chain.begin() + static_cast<std::ptrdiff_t>(at), c.device);
  PQ_DOC_TRY(checkChain(loc->track->kind, chain));
  Res r = editTrack(s, loc->path, cs, kChangeStructural, [&](Track& t) {
    t.devices = std::move(chain);
    return ok();
  });
  if (r) cs.devices.push_back(c.device->id);
  return r;
}

Res apply_(const Snapshot& s, const RemoveDevice& c, ChangeSet& cs) {
  Result<DevLoc> loc = locateDevice(*s, c.device);
  if (!loc) return std::move(loc).error();
  Res r = editTrack(s, loc->path, cs, kChangeStructural, [&](Track& t) {
    t.devices.erase(t.devices.begin() + loc->index);
    return ok();
  });
  if (!r) return r;
  cs.devices.push_back(c.device);
  const WireList& wires = wiresOf(**r);
  if (std::none_of(wires.begin(), wires.end(), [&](const Wire& w) { return w.from == c.device || w.to == c.device; }))
    return r;
  auto np = std::make_shared<Project>(**r);  // T4.7: its wires go with it (one command, one undo step)
  dropWiresOf(*np, std::vector<DeviceId>{c.device}, cs);
  return Snapshot(std::move(np));
}

Res apply_(const Snapshot& s, const MoveDevice& c, ChangeSet& cs) {
  Result<DevLoc> from = locateDevice(*s, c.device);
  if (!from) return std::move(from).error();
  const TrackId toId = c.toTrack.valid() ? c.toTrack : from->track->id;
  const DevicePtr dev = from->track->devices[from->index];
  if (toId == from->track->id) {
    std::vector<DevicePtr> chain = from->track->devices;
    chain.erase(chain.begin() + from->index);
    const size_t at = std::min<size_t>(c.index, chain.size());
    if (at == from->index) return s;  // where it is
    chain.insert(chain.begin() + static_cast<std::ptrdiff_t>(at), dev);
    PQ_DOC_TRY(checkChain(from->track->kind, chain));
    Res r = editTrack(s, from->path, cs, kChangeStructural, [&](Track& t) {
      t.devices = std::move(chain);
      return ok();
    });
    if (r) cs.devices.push_back(c.device);
    return r;
  }
  Result<Loc> to = locate(*s, toId);
  if (!to) return std::move(to).error();
  std::vector<DevicePtr> dst = to->track->devices;
  const size_t at = std::min<size_t>(c.index, dst.size());
  dst.insert(dst.begin() + static_cast<std::ptrdiff_t>(at), dev);
  PQ_DOC_TRY(checkChain(to->track->kind, dst));
  std::vector<DevicePtr> src = from->track->devices;
  src.erase(src.begin() + from->index);
  auto np = copyOf(s);
  mutableTrack(*np, from->path).devices = std::move(src);
  mutableTrack(*np, to->path).devices = std::move(dst);
  cs.mark(kChangeStructural);
  cs.tracks.push_back(from->track->id);
  cs.tracks.push_back(toId);
  cs.devices.push_back(c.device);
  return Snapshot(std::move(np));
}

// Replaces device `index` of the track at `path` by `d` (path-copying the track).
Res editDevice(const Snapshot& s, const DevLoc& loc, Device d, ChangeSet& cs, ChangeFlag flag) {
  const DeviceId id = d.id;
  Res r = editTrack(s, loc.path, cs, flag, [&](Track& t) {
    t.devices[loc.index] = std::make_shared<const Device>(std::move(d));
    return ok();
  });
  if (r) cs.devices.push_back(id);
  return r;
}

Res apply_(const Snapshot& s, const SetDeviceActive& c, ChangeSet& cs) {
  Result<DevLoc> loc = locateDevice(*s, c.device);
  if (!loc) return std::move(loc).error();
  const Device& old = *loc->track->devices[loc->index];
  if (old.active == c.active) return s;
  Device d = old;
  d.active = c.active;
  return editDevice(s, *loc, std::move(d), cs, kChangeStructural);
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
  Result<DevLoc> loc = locateDevice(*s, c.device);
  if (!loc) return std::move(loc).error();
  PQ_DOC_TRY(checkDeviceValue(c.param, c.value));
  Device d = *loc->track->devices[loc->index];
  if (!setValue(d.params, c.param, c.value)) return s;
  cs.params.push_back(ParamRef::device(c.device, c.param));
  return editDevice(s, *loc, std::move(d), cs, kChangeParam);
}

Res apply_(const Snapshot& s, const SetDeviceSetting& c, ChangeSet& cs) {
  Result<DevLoc> loc = locateDevice(*s, c.device);
  if (!loc) return std::move(loc).error();
  PQ_DOC_TRY(checkDeviceValue(c.setting, c.value));
  Device d = *loc->track->devices[loc->index];
  if (!setValue(d.settings, c.setting, c.value)) return s;
  return editDevice(s, *loc, std::move(d), cs, kChangeStructural);
}

// T4.4-fix1: a user device's text (fork / edit) or back to the stock patch (null).
Res apply_(const Snapshot& s, const SetDevicePatch& c, ChangeSet& cs) {
  Result<DevLoc> loc = locateDevice(*s, c.device);
  if (!loc) return std::move(loc).error();
  const Device& old = *loc->track->devices[loc->index];
  if (c.source && c.source->text.empty())
    return Error{ErrorCode::InvalidArgument, "a device patch text must not be empty", {}};
  if (old.source == c.source || (old.source && c.source && *old.source == *c.source)) return s;
  Device d = old;
  d.source = c.source;
  return editDevice(s, *loc, std::move(d), cs, kChangeStructural);
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
    "Clip Colour", "Clip Signature", "Activate Clip", "Clip Fundamental",
    "Add Notes", "Delete Notes", "Edit Notes", "Move Note", "Resize Note", "Note Property",
    "Add Automation Lane", "Delete Automation Lane", "Add Breakpoints", "Delete Breakpoints", "Edit Breakpoints",
    "Tempo", "Time Signature", "Loop", "Add Locator", "Delete Locator", "Edit Locator",
    "Add Tuning", "Delete Tuning", "Rename Tuning", "Tuning Colour", "Edit Tuning", "Tuning Parameter",
    "Add Device", "Delete Device", "Move Device", "Activate Device", "Device Parameter", "Device Setting",
    "Edit Device Patch",
    "Add Wire", "Delete Wire", "Wire Amount", "Wire Reducer",
    "Batch"};

}  // namespace

std::string_view label(const Command& c) noexcept {
  if (const Batch* b = std::get_if<Batch>(&c.v)) return b->label.empty() ? kLabels.back() : std::string_view(b->label);
  return kLabels[c.v.index()];
}

Result<Snapshot> apply(const Snapshot& s, const Command& c, ChangeSet& changes) {
  Res r = applyAny(s, c, changes);
  if (r) changes.normalize();
  return r;
}

}  // namespace pq::doc
