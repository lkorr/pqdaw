#include "pq/document/pitch_resolve.h"

#include <algorithm>
#include <cfloat>
#include <climits>
#include <cmath>
#include <unordered_map>
#include <utility>

#include "pq/document/ji_object.h"
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
    case TuningMode::Inherit:
      break;
  }
  return {};
}

// How an identity follows a tuning signal on its own (T2.6-fix3; a Ratio anchored to a note follows that note instead:
// ClipResolver): Degree → Degree; Cents and a reference-anchored Ratio → Reference; anything else Static.
PitchFollow ownFollow(const tuning::PitchId& p) noexcept {
  switch (p.kind()) {
    case tuning::PitchKind::Degree: return PitchFollow::Degree;
    case tuning::PitchKind::Cents: return PitchFollow::Reference;
    case tuning::PitchKind::Ratio:
      return p.anchorKind() == tuning::AnchorKind::Reference ? PitchFollow::Reference : PitchFollow::Static;
    case tuning::PitchKind::Harmonic: break;
  }
  return PitchFollow::Static;
}

// resolveHzDouble, and nullopt for a pitch that does not survive float32 (the engine's Hz: not a playable note).
std::optional<double> playableHz(const tuning::PitchId& p, const tuning::Tuning& t, const tuning::ResolveContext& rc) {
  std::optional<double> hz = tuning::resolveHzDouble(p, t, rc);
  if (hz && !(*hz >= static_cast<double>(FLT_MIN) && *hz <= static_cast<double>(FLT_MAX))) hz.reset();
  return hz;
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

  ClipResolver(const NoteClip& clip, const tuning::Tuning& t, bool signal) : clip_(clip), t_(t), signal_(signal) {
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
    r.follow = ownFollow(p);
    bool resolvable = true;
    switch (p.kind()) {
      case tuning::PitchKind::Degree:
        r.degree = p.degree();
        r.equave = p.equave();
        break;
      case tuning::PitchKind::Cents:
        break;
      case tuning::PitchKind::Ratio:
        if (p.anchorKind() == tuning::AnchorKind::Note) {
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
        if (p.anchorKind() == tuning::AnchorKind::JiFundamental) {  // T7.2: the object's fundamental at the note's start
          const JiObject* o = findJiObject(clip_, JiObjectId{p.anchorId()});
          const std::optional<double> f =
              o ? jiFundamentalHz(*o, t_, static_cast<double>(notes_[i]->start - o->start)) : std::nullopt;
          if (f) {
            rc.anchorHz = *f;
            followFundamental(r, *o);
          } else {
            resolvable = false;
          }
        }
        break;
      case tuning::PitchKind::Harmonic: {  // T7.2: the object's voice at the note's start (exact at a hold)
        const JiObject* o = findJiObject(clip_, JiObjectId{p.jiObject()});
        std::optional<double> hz =
            o ? jiVoiceHz(*o, t_, p.voiceIndex(), static_cast<double>(notes_[i]->start - o->start)) : std::nullopt;
        if (hz && !(*hz >= static_cast<double>(FLT_MIN) && *hz <= static_cast<double>(FLT_MAX))) hz.reset();
        if (hz) followFundamental(r, *o);
        r.hz = hz;
        resolvable = false;  // resolved here, not by the tuning
        break;
      }
    }
    if (resolvable) r.hz = playableHz(p, t_, rc);
    if (!r.hz || !signal_) r.follow = PitchFollow::Static;
    out_[i] = r;
    state_[i] = 2;
    return r;
  }

private:
  // A note of a JI object follows the tuning signal as the object's fundamental does (a Degree fundamental: its
  // degree, the note keeping its ratio to it).
  static void followFundamental(Resolved& r, const JiObject& o) noexcept {
    r.follow = ownFollow(o.fundamental);
    if (o.fundamental.kind() == tuning::PitchKind::Degree) {
      r.degree = o.fundamental.degree();
      r.equave = o.fundamental.equave();
    }
  }

  const NoteClip& clip_;
  const tuning::Tuning& t_;
  bool signal_;
  std::vector<const Note*> notes_;
  std::vector<std::pair<uint32_t, uint32_t>> byId_;  // (note id, index), sorted
  std::vector<uint8_t> state_;                       // 0 new, 1 in progress, 2 done
  std::vector<Resolved> out_;
};

}  // namespace

int64_t degreesPerEquave(const tuning::Tuning& t) noexcept {
  if (const tuning::Edo* e = t.edo()) return static_cast<int64_t>(e->n());
  if (const tuning::Mos* m = t.mos()) return m->size();
  if (const tuning::Scale* s = t.scale()) return s->size();
  return 0;
}

namespace {

// NotePitch::keyDegree: a Degree identity's absolute degree; any other pitch's position between the two degrees around
// it, linear in log frequency (the tuning's degrees rise monotonically). Bounded: a pitch more than 64 degrees from the
// equave estimate, or a degenerate tuning, has none.
std::optional<double> keyDegreeOf(const tuning::PitchId& p, double hz, const tuning::Tuning& t) noexcept {
  const int64_t size = degreesPerEquave(t);
  if (size <= 0) return std::nullopt;
  if (p.kind() == tuning::PitchKind::Degree) return static_cast<double>(p.equave()) * static_cast<double>(size) + p.degree();
  auto hzOf = [&](int64_t d) {
    if (d < INT32_MIN || d > INT32_MAX) return std::optional<double>{};
    return tuning::resolveHzDouble(tuning::PitchId::fromDegree(static_cast<int32_t>(d), 0), t, {});
  };
  const std::optional<double> ref = hzOf(0), eq = hzOf(size);
  if (!ref || !eq || !(*eq > *ref) || !(hz > 0.0)) return std::nullopt;
  const double est = std::floor(std::log(hz / *ref) / std::log(*eq / *ref) * static_cast<double>(size));
  if (!std::isfinite(est) || std::fabs(est) > 1e9) return std::nullopt;
  int64_t d = static_cast<int64_t>(est);
  std::optional<double> lo = hzOf(d), hi = hzOf(d + 1);
  for (int i = 0; i < 64 && lo && hi; ++i) {  // the estimate is exact for an EDO and within a step or two otherwise
    if (*lo > hz) {
      hi = lo;
      lo = hzOf(--d);
    } else if (*hi <= hz) {
      lo = hi;
      hi = hzOf(++d + 1);
    } else {
      break;
    }
  }
  if (!lo || !hi || !(*lo <= hz && hz < *hi)) return std::nullopt;
  return static_cast<double>(d) + std::log(hz / *lo) / std::log(*hi / *lo);
}

}  // namespace

NotePitch resolvePitch(const tuning::PitchId& p, const tuning::Tuning& t, TuningId signal) {
  NotePitch np;
  if (p.kind() == tuning::PitchKind::Ratio && p.anchorKind() != tuning::AnchorKind::Reference) return np;  // needs anchors
  np.hz = playableHz(p, t, {});
  if (!np.hz) return np;
  np.follow = signal.valid() ? ownFollow(p) : PitchFollow::Static;
  if (np.follow != PitchFollow::Static) np.signal = signal;
  if (p.kind() == tuning::PitchKind::Degree) {
    np.degree = p.degree();
    np.equave = p.equave();
  }
  np.keyDegree = keyDegreeOf(p, *np.hz, t);
  return np;
}

EffectiveTuning effectiveTuningRef(const Project& p, const TrackPath& path, const NoteClip* clip) noexcept {
  if (clip && clip->tuning.mode != TuningMode::Inherit) return refOf(p, clip->tuning);
  TrackPath at = path;
  while (at.depth > 0) {
    const Track& t = trackAt(p, at);
    if (t.tuning.mode != TuningMode::Inherit) return refOf(p, t.tuning);
    if (at.list != TrackList::Main) break;
    at = at.parent();
  }
  const TuningSlot* d = defaultTuningOf(p);
  return d ? EffectiveTuning{&d->def, d->id} : EffectiveTuning{};
}

LiveClip liveClip(const Track& t, ClipId edited, Ticks at) noexcept {
  LiveClip out;
  if (edited.valid())
    for (const ClipSlot& s : t.clips)
      if (s.id == edited && s.clip) {
        out.clip = s.clip.get();
        out.id = s.id;
        return out;  // case (1): wherever the playhead is
      }
  // Case (2): the answer only changes where a clip starts or ends, so its span runs between the nearest boundaries.
  for (const ClipSlot& s : playedClips(t)) {
    if (!s.clip || !s.clip->active) continue;
    const Ticks a = s.start(), b = s.end();
    if (a <= at && at < b) {  // sorted by (start, id): a later one starts at or after this one
      out.clip = s.clip.get();
      out.id = s.id;
    }
    for (const Ticks edge : {a, b}) {
      if (edge <= at) out.from = std::max(out.from, edge);
      else out.to = std::min(out.to, edge);
    }
  }
  return out;
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

namespace {

using BuiltTunings = std::unordered_map<const TuningDef*, std::optional<tuning::Tuning>>;  // clips share their tuning

// One note clip of the track at `path`, resolved in its effective tuning (resolveTrackPitches' per-clip work).
ClipPitches resolveClip(const Project& p, const TrackPath& path, const NoteClip& clip, const ResolveOptions& options,
                        BuiltTunings& built) {
  ClipPitches cp;
  cp.clip = clip.id;
  cp.tuning = effectiveTuningRef(p, path, &clip);
  cp.notes.reserve(clip.notes.size());
  const bool signal = cp.tuning.main.valid() && cp.tuning.def && hasTuningSignal(*cp.tuning.def);
  const tuning::Tuning* t = nullptr;
  if (signal && options.signalTuning) t = options.signalTuning(cp.tuning.main, *cp.tuning.def);
  if (!t && cp.tuning.def) {
    auto it = built.find(cp.tuning.def);
    if (it == built.end()) {
      Result<tuning::Tuning> b = buildTuning(*cp.tuning.def);
      it = built.emplace(cp.tuning.def, b.ok() ? std::optional<tuning::Tuning>(std::move(b.value())) : std::nullopt).first;
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
      if (v.hz) np.keyDegree = keyDegreeOf(r.note(i).pitch, *v.hz, *t);
      cp.notes.push_back(np);
    }
  } else {
    for (const Note& n : clip.notes) cp.notes.push_back(NotePitch{n.id, std::nullopt, {}, PitchFollow::Static, 0, 0});
  }
  return cp;
}

}  // namespace

Result<TrackPitches> resolveTrackPitches(const Project& p, TrackId id, const ResolveOptions& options) {
  const std::optional<TrackPath> path = findTrack(p, id);
  if (!path) return Error{ErrorCode::NotFound, "resolveTrackPitches: no such track", {}};
  const Track& track = trackAt(p, *path);
  TrackPitches out;
  out.track = id;
  const std::vector<ClipSlot>& clips = playedClips(track);  // T6.1: the audition take's, else the main lane's
  out.clips.reserve(clips.size());
  BuiltTunings built;
  for (const ClipSlot& slot : clips) {
    if (!slot.clip) {  // T5.3: an audio clip has no notes (keeps out.clips aligned with the played clips)
      out.clips.push_back(ClipPitches{});
      out.clips.back().clip = slot.id;
      continue;
    }
    out.clips.push_back(resolveClip(p, *path, *slot.clip, options, built));
  }
  return out;
}

Result<ClipPitches> resolveClipPitches(const Project& p, TrackId track, ClipId clip, const ResolveOptions& options) {
  const std::optional<TrackPath> path = findTrack(p, track);
  if (!path) return Error{ErrorCode::NotFound, "resolveClipPitches: no such track", {}};
  const Track& t = trackAt(p, *path);
  const int32_t i = findClipIndex(t, clip);
  if (i < 0) return Error{ErrorCode::NotFound, "resolveClipPitches: no such clip on the track", {}};
  const ClipSlot& slot = t.clips[static_cast<size_t>(i)];
  if (!slot.clip) return Error{ErrorCode::InvalidArgument, "resolveClipPitches: not a note clip", {}};
  BuiltTunings built;
  return resolveClip(p, *path, *slot.clip, options, built);
}

}  // namespace pq::doc
