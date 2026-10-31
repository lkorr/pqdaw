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
// Memoised depth-first resolution, bounded like the playback lowering (8 links), cycles unresolved. With `signal`
// (the clip follows a tuning signal) each note also gets how it follows it (PitchFollow, T2.6-fix3): a chain of note
// anchors follows its root.
class ClipResolver {
public:
  struct Resolved {
    std::optional<double> hz;
    PitchFollow follow = PitchFollow::Static;
    int32_t degree = 0, equave = 0;
  };

  ClipResolver(const NoteClip& clip, const tuning::Tuning& t, bool signal) : t_(t), signal_(signal) {
    notes_.reserve(clip.notes.size());
    for (const Note& n : clip.notes) notes_.push_back(&n);
    byId_.reserve(notes_.size());
    for (uint32_t i = 0; i < notes_.size(); ++i) byId_.emplace_back(notes_[i]->id.value, i);
    std::sort(byId_.begin(), byId_.end());
    state_.assign(notes_.size(), 0);
    out_.assign(notes_.size(), Resolved{});
  }
  size_t size() const noexcept { return notes_.size(); }
  const Note& note(size_t i) const noexcept { return *notes_[i]; }

  Resolved get(uint32_t i, int depth = 0) {
    if (state_[i] == 2) return out_[i];
    if (state_[i] == 1 || depth > 8) return Resolved{};  // a cycle, or a chain longer than the lowering allows
    state_[i] = 1;
    const tuning::PitchId& p = notes_[i]->pitch;
    tuning::ResolveContext rc;
    Resolved r;
    bool resolvable = true;
    switch (p.kind()) {
      case tuning::PitchKind::Degree:
        r.follow = PitchFollow::Degree;
        r.degree = p.degree();
        r.equave = p.equave();
        break;
      case tuning::PitchKind::Cents:
        r.follow = PitchFollow::Reference;
        break;
      case tuning::PitchKind::Ratio:
        if (p.anchorKind() == tuning::AnchorKind::Reference) {
          r.follow = PitchFollow::Reference;
        } else if (p.anchorKind() == tuning::AnchorKind::Note) {
          const auto it = std::lower_bound(byId_.begin(), byId_.end(), std::pair<uint32_t, uint32_t>{p.anchorId(), 0});
          if (it == byId_.end() || it->first != p.anchorId()) {
            resolvable = false;
          } else if (const Resolved a = get(it->second, depth + 1); a.hz) {
            rc.anchorHz = *a.hz;
            r.follow = a.follow;  // the chain follows its root
            r.degree = a.degree;
            r.equave = a.equave;
          } else {
            resolvable = false;
          }
        }
        break;  // a JI-fundamental anchor needs its JI object (M7): resolveHzDouble fails without anchorHz
      case tuning::PitchKind::Harmonic:
        break;
    }
    if (resolvable) r.hz = tuning::resolveHzDouble(p, t_, rc);
    // A pitch that does not survive float32 (the engine's Hz) is not a playable note.
    if (r.hz && !(*r.hz >= static_cast<double>(FLT_MIN) && *r.hz <= static_cast<double>(FLT_MAX))) r.hz.reset();
    if (!r.hz || !signal_) r.follow = PitchFollow::Static;
    out_[i] = r;
    state_[i] = 2;
    return r;
  }

private:
  const tuning::Tuning& t_;
  bool signal_;
  std::vector<const Note*> notes_;
  std::vector<std::pair<uint32_t, uint32_t>> byId_;  // (note id, index), sorted
  std::vector<uint8_t> state_;                       // 0 new, 1 in progress, 2 done
  std::vector<Resolved> out_;
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
  // T2.6-fix4 (M3 review L9): a definition whose continuous values the node's params cannot hold (outside
  // paramRange, which the program clamps them to) plays static pitches instead of a node that would play others.
  auto inRange = [](ParamKind k, double v) {
    const ParamRange r = paramRange(k);
    return v >= r.min && v <= r.max;
  };
  if (!inRange(ParamKind::TuningReference, def.referenceHz)) return false;
  if (const tuning::Edo* e = def.edo()) return inRange(ParamKind::TuningEquave, e->equaveCents());
  if (const tuning::MosParams* m = def.mos())
    return !tuning::hasModmos(m->modmos) && m->genSequence.empty() && inRange(ParamKind::MosGenerator, m->generator) &&
           inRange(ParamKind::MosPeriod, m->period) && inRange(ParamKind::MosStretch, m->octaveStretch);
  return false;
}

Result<TrackPitches> resolveTrackPitches(const Project& p, TrackId id, const ResolveOptions& options) {
  const std::optional<TrackPath> path = findTrack(p, id);
  if (!path) return Error{ErrorCode::NotFound, "resolveTrackPitches: no such track", {}};
  const Track& track = trackAt(p, *path);
  TrackPitches out;
  out.track = id;
  out.clips.reserve(track.clips.size());
  std::unordered_map<const TuningDef*, std::optional<tuning::Tuning>> built;  // clips share their track's tuning
  for (const ClipSlot& slot : track.clips) {
    if (!slot.clip) {  // T5.3: an audio clip has no notes (keeps out.clips aligned with track.clips)
      out.clips.push_back(ClipPitches{});
      out.clips.back().clip = slot.id;
      continue;
    }
    const NoteClip& clip = *slot.clip;
    ClipPitches cp;
    cp.clip = clip.id;
    cp.tuning = effectiveTuningRef(p, *path, &clip);
    cp.notes.reserve(clip.notes.size());
    const bool signal = cp.tuning.main.valid() && cp.tuning.def && hasTuningSignal(*cp.tuning.def);
    const tuning::Tuning* t = nullptr;
    if (signal && options.signalTuning) t = options.signalTuning(cp.tuning.main, *cp.tuning.def);
    if (!t && cp.tuning.def) {
      auto it = built.find(cp.tuning.def);
      if (it == built.end()) {
        Result<tuning::Tuning> b = buildTuning(*cp.tuning.def);
        it = built.emplace(cp.tuning.def, b.ok() ? std::optional<tuning::Tuning>(std::move(b.value())) : std::nullopt)
                 .first;
      }
      if (it->second) t = &*it->second;
    }
    if (t) {
      ClipResolver r(clip, *t, signal);
      for (uint32_t i = 0; i < r.size(); ++i) {
        const ClipResolver::Resolved v = r.get(i);
        NotePitch np;
        np.note = r.note(i).id;
        np.hz = v.hz;
        np.follow = v.follow;
        if (v.follow != PitchFollow::Static) np.signal = cp.tuning.main;
        np.degree = v.degree;
        np.equave = v.equave;
        cp.notes.push_back(np);
      }
    } else {
      for (const Note& n : clip.notes) cp.notes.push_back(NotePitch{n.id, std::nullopt, {}, PitchFollow::Static, 0, 0});
    }
    out.clips.push_back(std::move(cp));
  }
  return out;
}

}  // namespace pq::doc
