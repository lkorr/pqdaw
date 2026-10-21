// Track edit operations (T3.6).

#include <algorithm>
#include <string>

#include "edit_common.h"

namespace pq::doc::ops {

using namespace detail;

namespace {

bool isAncestor(const TrackPath& a, const TrackPath& b) {
  if (a.list != b.list || a.depth >= b.depth) return false;
  for (uint32_t i = 0; i < a.depth; ++i)
    if (a.index[i] != b.index[i]) return false;
  return true;
}

bool treeLess(const TrackPath& a, const TrackPath& b) {
  if (a.list != b.list) return a.list < b.list;
  const uint32_t n = std::min(a.depth, b.depth);
  for (uint32_t i = 0; i < n; ++i)
    if (a.index[i] != b.index[i]) return a.index[i] < b.index[i];
  return a.depth < b.depth;
}

struct Sel {
  TrackId id;
  TrackPath path;
};

// Looks up the selection, drops duplicates and tracks inside other selected tracks, sorts by tree order.
Result<std::vector<Sel>> normalizeSelection(const Project& p, std::span<const TrackId> tracks, bool allowMaster) {
  std::vector<Sel> sel;
  sel.reserve(tracks.size());
  for (TrackId id : tracks) {
    const auto path = findTrack(p, id);
    if (!path) return notFound("track not found");
    if (path->list == TrackList::Master && !allowMaster) return invalid("not available on the master track");
    sel.push_back({id, *path});
  }
  std::sort(sel.begin(), sel.end(), [](const Sel& a, const Sel& b) { return treeLess(a.path, b.path); });
  sel.erase(std::unique(sel.begin(), sel.end(), [](const Sel& a, const Sel& b) { return a.id == b.id; }), sel.end());
  std::vector<Sel> out;
  out.reserve(sel.size());
  for (const Sel& s : sel) {
    // Sorted in tree order, so an ancestor (if selected) is already in `out`.
    const bool inside = std::any_of(out.begin(), out.end(), [&](const Sel& o) { return isAncestor(o.path, s.path); });
    if (!inside) out.push_back(s);
  }
  return out;
}

std::string defaultName(const Project& p, TrackKind kind) {
  uint32_t n = 0;
  forEachTrack(p, [&](const Track& t, const Track*) { n += t.kind == kind ? 1u : 0u; });
  switch (kind) {
    case TrackKind::Group: return std::to_string(n + 1) + " Group";
    case TrackKind::Return: {
      std::string s;
      for (uint32_t k = n + 1; k > 0; k = (k - 1) / 26) s.insert(s.begin(), static_cast<char>('A' + (k - 1) % 26));
      return s + " Return";
    }
    default: return std::to_string(n + 1) + " MIDI";
  }
}

const std::vector<TrackPtr>& listOf(const Project& p, TrackList l) { return l == TrackList::Returns ? p.returns : p.tracks; }

}  // namespace

EditResult createTrack(const Project& p, IdSource& ids, TrackKind kind, TrackId after, std::string name, Colour colour) {
  if (kind != TrackKind::Midi && kind != TrackKind::Group && kind != TrackKind::Return)
    return invalid("only MIDI, group and return tracks can be created");
  TrackId parent;
  uint32_t index = kAppend;
  if (after.valid()) {
    const auto path = findTrack(p, after);
    if (!path) return notFound("track not found");
    const bool afterIsReturn = path->list == TrackList::Returns;
    if ((kind == TrackKind::Return) == afterIsReturn && path->list != TrackList::Master) {
      if (const Track* par = parentOf(p, *path)) parent = par->id;
      index = path->index[path->depth - 1] + 1;
    }
  }
  if (name.empty()) name = defaultName(p, kind);
  Track t = makeTrack(ids.track(), kind, std::move(name));
  t.colour = colour;
  Edit e = makeEdit(kind == TrackKind::Return ? "Insert Return Track"
                                              : (kind == TrackKind::Group ? "Insert Group Track" : "Insert MIDI Track"));
  e.tracks.push_back(t.id);
  batchOf(e).commands.push_back(AddTrack{std::make_shared<const Track>(std::move(t)), parent, index});
  return e;
}

EditResult deleteTracks(const Project& p, std::span<const TrackId> tracks) {
  Result<std::vector<Sel>> sel = normalizeSelection(p, tracks, false);
  if (!sel) return std::move(sel).error();
  Edit e = makeEdit(sel->size() == 1 ? "Delete Track" : "Delete Tracks");
  for (const Sel& s : *sel) batchOf(e).commands.push_back(RemoveTrack{s.id});
  return e;
}

EditResult renameTrack(const Project& p, TrackId track, std::string name) {
  if (!findTrackPtr(p, track)) return notFound("track not found");
  Edit e = makeEdit("Rename Track");
  e.tracks.push_back(track);
  batchOf(e).commands.push_back(RenameTrack{track, std::move(name)});
  return e;
}

EditResult setTracksColour(const Project& p, std::span<const TrackId> tracks, Colour colour) {
  Edit e = makeEdit("Track Colour");
  for (TrackId id : tracks) {
    if (!findTrackPtr(p, id)) return notFound("track not found");
    batchOf(e).commands.push_back(SetTrackColour{id, colour});
    e.tracks.push_back(id);
  }
  return e;
}

EditResult moveTracks(const Project& p, std::span<const TrackId> tracks, TrackId parent, TrackId before) {
  Result<std::vector<Sel>> sel = normalizeSelection(p, tracks, false);
  if (!sel) return std::move(sel).error();
  Edit e = makeEdit(sel->size() == 1 ? "Move Track" : "Move Tracks");
  if (sel->empty()) return e;
  const TrackList list = sel->front().path.list;
  for (const Sel& s : *sel)
    if (s.path.list != list) return invalid("return tracks move only among returns");
  if (list == TrackList::Returns && parent.valid()) return invalid("return tracks cannot be grouped");
  // The destination list as ids; simulate each MoveTrack (index = position after removing the moved track).
  std::vector<TrackId> cur;
  if (parent.valid()) {
    const Track* g = findTrackPtr(p, parent);
    if (!g || g->kind != TrackKind::Group) return invalid("parent must be a group track");
    for (const TrackPtr& c : g->children) cur.push_back(c->id);
  } else {
    for (const TrackPtr& c : listOf(p, list)) cur.push_back(c->id);
  }
  for (const Sel& s : *sel)
    if (s.id == before) return invalid("cannot move tracks before one of themselves");
  if (before.valid() && std::find(cur.begin(), cur.end(), before) == cur.end())
    return invalid("`before` is not a child of the destination");
  TrackId prev;
  for (const Sel& s : *sel) {
    std::erase(cur, s.id);
    size_t at = cur.size();
    if (prev.valid()) {
      at = static_cast<size_t>(std::find(cur.begin(), cur.end(), prev) - cur.begin()) + 1;
    } else if (before.valid()) {
      at = static_cast<size_t>(std::find(cur.begin(), cur.end(), before) - cur.begin());
    }
    cur.insert(cur.begin() + static_cast<std::ptrdiff_t>(at), s.id);
    batchOf(e).commands.push_back(MoveTrack{s.id, parent, static_cast<uint32_t>(at)});
    e.tracks.push_back(s.id);
    prev = s.id;
  }
  return e;
}

EditResult groupTracks(const Project& p, IdSource& ids, std::span<const TrackId> tracks, std::string name) {
  Result<std::vector<Sel>> sel = normalizeSelection(p, tracks, false);
  if (!sel) return std::move(sel).error();
  if (sel->empty()) return invalid("nothing to group");
  GroupTracks g;
  for (const Sel& s : *sel) {
    if (s.path.list != TrackList::Main) return invalid("only tracks in the main tree can be grouped");
    g.tracks.push_back(s.id);
  }
  if (name.empty()) name = defaultName(p, TrackKind::Group);
  const TrackId gid = ids.track();
  g.group = std::make_shared<const Track>(makeTrack(gid, TrackKind::Group, std::move(name)));
  Edit e = makeEdit("Group Tracks");
  e.tracks.push_back(gid);
  batchOf(e).commands.push_back(std::move(g));
  return e;
}

EditResult ungroupTracks(const Project& p, std::span<const TrackId> groups) {
  Edit e = makeEdit("Ungroup Tracks");
  std::vector<TrackId> done;
  for (TrackId id : groups) {
    if (std::find(done.begin(), done.end(), id) != done.end()) continue;
    const Track* g = findTrackPtr(p, id);
    if (!g) return notFound("track not found");
    if (g->kind != TrackKind::Group) return invalid("not a group track");
    batchOf(e).commands.push_back(UngroupTracks{id});
    done.push_back(id);
    for (const TrackPtr& c : g->children)
      if (std::find(groups.begin(), groups.end(), c->id) == groups.end()) e.tracks.push_back(c->id);
  }
  return e;
}

EditResult setTracksActive(const Project& p, std::span<const TrackId> tracks, bool active) {
  Edit e = makeEdit(active ? "Activate Tracks" : "Deactivate Tracks");
  for (TrackId id : tracks) {
    const Track* t = findTrackPtr(p, id);
    if (!t) return notFound("track not found");
    if (t->kind == TrackKind::Master) return invalid("the master track cannot be deactivated");
    if (t->active != active) batchOf(e).commands.push_back(SetTrackActive{id, active});
    e.tracks.push_back(id);
  }
  return e;
}

EditResult toggleTracksActive(const Project& p, std::span<const TrackId> tracks) {
  bool anyActive = false;
  for (TrackId id : tracks) {
    const Track* t = findTrackPtr(p, id);
    if (!t) return notFound("track not found");
    anyActive = anyActive || t->active;
  }
  return setTracksActive(p, tracks, !anyActive);
}

void setFolded(ViewState& view, TrackId track, bool folded) { view.tracks[track].folded = folded; }

bool toggleFolded(ViewState& view, TrackId track) {
  TrackView& v = view.tracks[track];
  v.folded = !v.folded;
  return v.folded;
}

}  // namespace pq::doc::ops
