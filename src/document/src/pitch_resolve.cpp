#include "pq/document/pitch_resolve.h"

#include <algorithm>
#include <cfloat>
#include <unordered_map>
#include <utility>

#include "pq/tuning/mos.h"
#include "pq/tuning/tuning.h"

namespace pq::doc {

namespace {

EffectiveTuning refOf(const Project& p, const TuningAssignment& a) noexcept {
  switch (a.mode) {
    case TuningMode::Main: {
      const TuningSlot* s = findTuning(p, a.main);
      return s ? EffectiveTuning{&s->def, a.main} : EffectiveTuning{};
    }
    case TuningMode::Private:
      return {a.privateDef.get(), TuningId{}};
    case TuningMode::Inherit:
      break;
  }
  return {};
}

// Anchored Ratio notes refer to other notes of the clip by id; anchors chain (a note anchored to an anchored note).
// Memoised depth-first resolution, bounded like the playback lowering (8 links), cycles unresolved.
class ClipResolver {
public:
  ClipResolver(const NoteClip& clip, const tuning::Tuning& t) : t_(t) {
    notes_.reserve(clip.notes.size());
    for (const Note& n : clip.notes) notes_.push_back(&n);
    byId_.reserve(notes_.size());
    for (uint32_t i = 0; i < notes_.size(); ++i) byId_.emplace_back(notes_[i]->id.value, i);
    std::sort(byId_.begin(), byId_.end());
    state_.assign(notes_.size(), 0);
    hz_.assign(notes_.size(), std::nullopt);
  }
  size_t size() const noexcept { return notes_.size(); }
  const Note& note(size_t i) const noexcept { return *notes_[i]; }
  std::optional<double> hz(uint32_t i, int depth = 0) {
    if (state_[i] == 2) return hz_[i];
    if (state_[i] == 1 || depth > 8) return std::nullopt;  // a cycle, or a chain longer than the lowering allows
    state_[i] = 1;
    const tuning::PitchId& p = notes_[i]->pitch;
    tuning::ResolveContext rc;
    std::optional<double> r;
    bool resolvable = true;
    if (p.kind() == tuning::PitchKind::Ratio && p.anchorKind() == tuning::AnchorKind::Note) {
      const auto it = std::lower_bound(byId_.begin(), byId_.end(), std::pair<uint32_t, uint32_t>{p.anchorId(), 0});
      if (it == byId_.end() || it->first != p.anchorId()) resolvable = false;
      else if (const auto a = hz(it->second, depth + 1)) rc.anchorHz = *a;
      else resolvable = false;
    }
    if (resolvable) r = tuning::resolveHzDouble(p, t_, rc);
    // A pitch that does not survive float32 (the engine's Hz) is not a playable note.
    if (r && !(*r >= static_cast<double>(FLT_MIN) && *r <= static_cast<double>(FLT_MAX))) r.reset();
    hz_[i] = r;
    state_[i] = 2;
    return r;
  }

private:
  const tuning::Tuning& t_;
  std::vector<const Note*> notes_;
  std::vector<std::pair<uint32_t, uint32_t>> byId_;  // (note id, index), sorted
  std::vector<uint8_t> state_;                       // 0 new, 1 in progress, 2 done
  std::vector<std::optional<double>> hz_;
};

}  // namespace

EffectiveTuning effectiveTuningRef(const Project& p, const TrackPath& path, const NoteClip* clip) noexcept {
  if (clip && clip->tuning.mode != TuningMode::Inherit) return refOf(p, clip->tuning);
  TrackPath at = path;
  while (at.depth > 0) {
    const Track& t = trackAt(p, at);
    if (t.tuning.mode != TuningMode::Inherit) return refOf(p, t.tuning);
    if (at.list != TrackList::Main) break;
    at = at.parent();
  }
  if (p.tunings->empty()) return {};
  const TuningSlot& first = *p.tunings->front();
  return {&first.def, first.id};
}

bool hasTuningSignal(const TuningDef& def) noexcept {
  if (def.edo()) return true;
  if (const tuning::MosParams* m = def.mos()) return !tuning::hasModmos(m->modmos) && m->genSequence.empty();
  return false;
}

Result<TrackPitches> resolveTrackPitches(const Project& p, TrackId id) {
  const std::optional<TrackPath> path = findTrack(p, id);
  if (!path) return Error{ErrorCode::NotFound, "resolveTrackPitches: no such track", {}};
  const Track& track = trackAt(p, *path);
  TrackPitches out;
  out.track = id;
  out.clips.reserve(track.clips.size());
  std::unordered_map<const TuningDef*, std::optional<tuning::Tuning>> built;  // clips share their track's tuning
  for (const ClipSlot& slot : track.clips) {
    const NoteClip& clip = *slot.clip;
    ClipPitches cp;
    cp.clip = clip.id;
    cp.tuning = effectiveTuningRef(p, *path, &clip);
    cp.notes.reserve(clip.notes.size());
    const tuning::Tuning* t = nullptr;
    if (cp.tuning.def) {
      auto it = built.find(cp.tuning.def);
      if (it == built.end()) {
        Result<tuning::Tuning> b = buildTuning(*cp.tuning.def);
        it = built.emplace(cp.tuning.def, b.ok() ? std::optional<tuning::Tuning>(std::move(b.value())) : std::nullopt)
                 .first;
      }
      if (it->second) t = &*it->second;
    }
    const bool signal = cp.tuning.main.valid() && cp.tuning.def && hasTuningSignal(*cp.tuning.def);
    if (t) {
      ClipResolver r(clip, *t);
      for (uint32_t i = 0; i < r.size(); ++i) {
        const Note& n = r.note(i);
        NotePitch np;
        np.note = n.id;
        np.hz = r.hz(i);
        if (signal && np.hz && n.pitch.kind() == tuning::PitchKind::Degree) {
          np.signal = cp.tuning.main;
          np.degree = n.pitch.degree();
          np.equave = n.pitch.equave();
        }
        cp.notes.push_back(np);
      }
    } else {
      for (const Note& n : clip.notes) cp.notes.push_back(NotePitch{n.id, std::nullopt, {}, 0, 0});
    }
    out.clips.push_back(std::move(cp));
  }
  return out;
}

}  // namespace pq::doc
