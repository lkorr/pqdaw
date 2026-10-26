// Mute / solo gates (T3.10; routing-aware solo, inactive tracks T3.17-fix3): see pq/document/audibility.h.

#include "pq/document/audibility.h"

#include <algorithm>

namespace pq::doc {

namespace {

struct Walker {
  const AudibilityOptions& opt;
  std::vector<TrackAudibility>& out;
  std::vector<const Track*>& tracks;   // parallel to out (main tree)
  std::vector<uint32_t>& parent;       // parallel: index of the parent group in out, or UINT32_MAX
  std::vector<uint8_t>& played;        // parallel: the track and every group above it are active

  // Appends each track (its `output` temporarily = "solo-related": soloed, inside a soloed group, or containing a
  // soloed track). A track that is deactivated, or inside a deactivated group, is not played at all (lowering leaves
  // it out), so its solo flag is ignored (M3 review L3). Returns whether the list's subtrees contain a soloed track.
  bool walk(const std::vector<TrackPtr>& list, bool soloAbove, bool activeAbove, uint32_t parentIndex) {
    bool any = false;
    for (const TrackPtr& t : list) {
      const auto index = static_cast<uint32_t>(out.size());
      out.push_back({t->id, false, false});
      tracks.push_back(t.get());
      parent.push_back(parentIndex);
      const bool active = activeAbove && t->active;
      played.push_back(active ? 1 : 0);
      const bool self = t->mixer.solo && active;
      const bool below = walk(t->children, soloAbove || self, active, index);
      out[index].output = self || soloAbove || below;
      any = any || self || below;
    }
    return any;
  }
};

}  // namespace

const TrackAudibility* AudibilityMap::find(TrackId id) const noexcept {
  const auto it = std::lower_bound(byId_.begin(), byId_.end(), id,
                                   [this](uint32_t i, TrackId v) { return tracks_[i].id < v; });
  if (it == byId_.end() || tracks_[*it].id != id) return nullptr;
  return &tracks_[*it];
}

AudibilityMap computeAudibility(const Project& p, const AudibilityOptions& options) {
  AudibilityMap m;
  m.tracks_.reserve(p.tracks.size() + p.returns.size() + 1);
  std::vector<const Track*> mainTracks;
  std::vector<uint32_t> parent;
  std::vector<uint8_t> played;
  Walker w{options, m.tracks_, mainTracks, parent, played};
  m.anyMainSolo_ = w.walk(p.tracks, false, true, UINT32_MAX);
  const size_t mainCount = m.tracks_.size();
  for (const TrackPtr& r : p.returns) m.anyReturnSolo_ = m.anyReturnSolo_ || (r->mixer.solo && r->active);

  auto silenced = [&](const Track& t) { return (t.mixer.mute && !options.ignoreMute) || !t.active; };
  auto indexOfMain = [&](TrackId id) -> uint32_t {
    for (uint32_t i = 0; i < mainCount; ++i)
      if (mainTracks[i]->id == id) return i;
    return UINT32_MAX;
  };
  // A main-tree track's destination group (an index into the main tree), or UINT32_MAX: the master or no output.
  auto destination = [&](const Track& t, uint32_t self) -> uint32_t {
    switch (t.mixer.output.kind) {
      case OutputKind::Default: return self == UINT32_MAX ? UINT32_MAX : parent[self];
      case OutputKind::Track: {
        const uint32_t g = indexOfMain(t.mixer.output.track);
        return g != UINT32_MAX && mainTracks[g]->kind == TrackKind::Group ? g : UINT32_MAX;
      }
      default: return UINT32_MAX;
    }
  };
  if (m.anyMainSolo_) {
    // M3 review M1: a solo-related track's signal must reach the master through every group on its output chain,
    // also one it is routed to explicitly (Live keeps the destination audible): those groups are solo-related too
    // (their own members are not). Audible returns routed into a group open that group's chain the same way.
    std::vector<uint32_t> work;
    for (uint32_t i = 0; i < mainCount; ++i)
      if (m.tracks_[i].output) work.push_back(i);
    for (const TrackPtr& r : p.returns)
      if (!silenced(*r) && (!m.anyReturnSolo_ || r->mixer.solo))
        if (const uint32_t d = destination(*r, UINT32_MAX); d != UINT32_MAX && !m.tracks_[d].output) {
          m.tracks_[d].output = true;
          work.push_back(d);
        }
    while (!work.empty()) {
      const uint32_t i = work.back();
      work.pop_back();
      const uint32_t d = destination(*mainTracks[i], i);
      if (d != UINT32_MAX && !m.tracks_[d].output) {
        m.tracks_[d].output = true;
        work.push_back(d);
      }
    }
  }

  // Main tree: the walk stored "solo-related" in `output`.
  for (uint32_t i = 0; i < mainCount; ++i) {
    TrackAudibility& a = m.tracks_[i];
    const bool related = a.output;
    // A member of a deactivated group is not played (lowering leaves it out): closed like the group.
    const bool open = !silenced(*mainTracks[i]) && played[i] != 0;
    a.output = open && (m.anyMainSolo_ ? related : !m.anyReturnSolo_);
    a.sends = open && (m.anyMainSolo_ ? related : true);
  }
  for (const TrackPtr& r : p.returns) {
    const bool open = !silenced(*r);
    m.tracks_.push_back({r->id, open && (m.anyReturnSolo_ ? r->mixer.solo : true), open});
  }
  if (p.master) m.tracks_.push_back({p.master->id, true, false});  // the master has no activator, solo or sends

  m.byId_.resize(m.tracks_.size());
  for (uint32_t k = 0; k < m.byId_.size(); ++k) m.byId_[k] = k;
  std::sort(m.byId_.begin(), m.byId_.end(), [&](uint32_t a, uint32_t b) { return m.tracks_[a].id < m.tracks_[b].id; });
  return m;
}

bool effectiveAudible(const Project& p, TrackId track) {
  const AudibilityMap m = computeAudibility(p);
  const TrackAudibility* a = m.find(track);
  return a && a->output;
}

}  // namespace pq::doc
