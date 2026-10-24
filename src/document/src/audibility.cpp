// Mute / solo gates (T3.10): see pq/document/audibility.h for the rules.

#include "pq/document/audibility.h"

#include <algorithm>

namespace pq::doc {

namespace {

bool silenced(const Track& t) noexcept { return t.mixer.mute || !t.active; }

// Main tree, depth-first: appends each track (its `output` temporarily = "solo-related": soloed, inside a soloed
// group, or containing a soloed track). Returns whether the list's subtrees contain a soloed track.
bool walkMain(const std::vector<TrackPtr>& list, bool soloAbove, std::vector<TrackAudibility>& out) {
  bool any = false;
  for (const TrackPtr& t : list) {
    const size_t index = out.size();
    out.push_back({t->id, false, false});
    const bool self = t->mixer.solo;
    const bool below = walkMain(t->children, soloAbove || self, out);
    out[index].output = self || soloAbove || below;
    any = any || self || below;
  }
  return any;
}

}  // namespace

const TrackAudibility* AudibilityMap::find(TrackId id) const noexcept {
  const auto it = std::lower_bound(byId_.begin(), byId_.end(), id,
                                   [this](uint32_t i, TrackId v) { return tracks_[i].id < v; });
  if (it == byId_.end() || tracks_[*it].id != id) return nullptr;
  return &tracks_[*it];
}

AudibilityMap computeAudibility(const Project& p) {
  AudibilityMap m;
  m.tracks_.reserve(p.tracks.size() + p.returns.size() + 1);
  m.anyMainSolo_ = walkMain(p.tracks, false, m.tracks_);
  const size_t mainCount = m.tracks_.size();
  for (const TrackPtr& r : p.returns) m.anyReturnSolo_ = m.anyReturnSolo_ || r->mixer.solo;

  // Main tree: the walk stored "solo-related" in `output`; tracks are visited in the same order to read their flags.
  size_t i = 0;
  forEachTrack(p, [&](const Track& t, const Track*) {
    if (i >= mainCount) return;  // returns and master follow below
    TrackAudibility& a = m.tracks_[i++];
    const bool related = a.output;
    const bool open = !silenced(t);
    a.output = open && (m.anyMainSolo_ ? related : !m.anyReturnSolo_);
    a.sends = open && (m.anyMainSolo_ ? related : true);
  });
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
