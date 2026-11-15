#include "pq/document/pitch_resolve.h"

#include <algorithm>
#include <cfloat>
#include <climits>
#include <cmath>
#include <unordered_map>
#include <utility>

#include "pq/document/note_bend.h"
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
    findWarped();
  }
  // The members a warp moves, resolved (sorted by note id).
  std::vector<NoteWarpState> takeWarped() {
    std::vector<NoteWarpState> out;
    out.reserve(warped_.size());
    for (const Warped& w : warped_)
      if (w.state.jiHz > 0.0 && w.state.writtenHz > 0.0) out.push_back(w.state);
    std::sort(out.begin(), out.end(), [](const NoteWarpState& a, const NoteWarpState& b) { return a.note < b.note; });
    return out;
  }
  size_t size() const noexcept { return notes_.size(); }
  const Note& note(size_t i) const noexcept { return *notes_[i]; }

  Resolved get(uint32_t i) {
    int hit = kNoHit;
    return get(i, 0, hit);
  }

private:
  // `hit`: the smallest depth of a note still being resolved that this resolution ran into (a cycle). A note on a
  // cycle of anchors does not resolve (as before). T7.11a: a cycle can also run through a bend — the bend's neighbour
  // is anchored, through other notes, to a note that takes the bending note's pitch inside that bend. Such a bend does
  // nothing for the note that closes the loop; nothing is silenced. Only results that met no cycle, and a note asked
  // for itself, are memoised, so what a note resolves to does not depend on which walk reached it first.
  Resolved get(uint32_t i, int depth, int& hit) {
    if (state_[i] == kDone) return out_[i];
    if (state_[i] >= kBusy) {
      hit = std::min(hit, static_cast<int>(state_[i] - kBusy));
      return Resolved{};
    }
    if (depth > 8) return Resolved{};  // a chain longer than the lowering allows
    state_[i] = static_cast<uint8_t>(kBusy + depth);
    int below = kNoHit;
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
          const int32_t ai = indexOf(NoteId{p.anchorId()});
          if (ai < 0) {
            resolvable = false;
          } else if (const Resolved a = get(static_cast<uint32_t>(ai), depth + 1, below); a.hz) {
            // T7.11a (M7 review M5): the anchor's pitch where this note starts — along its effective curve
            // (free curve, bends, warps), not where the anchor itself started.
            rc.anchorHz = pitchAt(static_cast<uint32_t>(ai), *a.hz, notes_[i]->start, depth, below);
            r.follow = a.follow;  // the chain follows its root
            r.degree = a.degree;
            r.equave = a.equave;
          } else {
            resolvable = false;
          }
        }
        break;
    }
    if (resolvable) r.hz = playableHz(p, t_, rc);
    if (r.hz && !warpOf_.empty() && warpOf_[i] >= 0) applyWarp(i, r);
    if (!r.hz || !signal_) r.follow = PitchFollow::Static;
    if (below != kNoHit && depth > 0) {
      // It ran into a note that is still being resolved: this result holds for this walk only. The note is resolved
      // again when it is asked for itself (depth 0), where every path that comes back to it is cut.
      state_[i] = kNew;
      if (below < depth) hit = std::min(hit, below);  // a cycle that closes on this note ends here
      return r;
    }
    out_[i] = r;
    state_[i] = kDone;
    return r;
  }

  int32_t indexOf(NoteId id) const noexcept {
    const auto it = std::lower_bound(byId_.begin(), byId_.end(), std::pair<uint32_t, uint32_t>{id.value, 0});
    return it != byId_.end() && it->first == id.value ? static_cast<int32_t>(it->second) : -1;
  }

  // T7.13 (model.h "JI groups"): the members with a written identity in a group that has warps. One pass over the
  // notes for the groups' starts, one for the members; nothing for a clip without warps.
  void findWarped() {
    if (!clip_.written) return;
    struct G {
      uint32_t anchor;
      const JiGroup* group;
      Ticks start;
      bool any;
    };
    std::vector<G> groups;
    for (const JiGroup& g : jiGroupsOf(clip_))
      if (!g.warps.empty()) groups.push_back({g.anchor.value, &g, 0, false});
    if (groups.empty()) return;
    std::sort(groups.begin(), groups.end(), [](const G& a, const G& b) { return a.anchor < b.anchor; });
    auto groupOf = [&](NoteId anchor) -> G* {
      const auto it = std::lower_bound(groups.begin(), groups.end(), anchor.value, [](const G& g, uint32_t id) { return g.anchor < id; });
      return it != groups.end() && it->anchor == anchor.value ? &*it : nullptr;
    };
    for (const Note* n : notes_) {
      G* g = groupOf(n->id);
      if (g == nullptr) g = groupOf(noteAnchorOf(n->pitch));
      if (g == nullptr) continue;
      g->start = g->any ? std::min(g->start, n->start) : n->start;
      g->any = true;
    }
    for (uint32_t i = 0; i < notes_.size(); ++i) {
      const Note& n = *notes_[i];
      const G* g = groupOf(noteAnchorOf(n.pitch));
      if (g == nullptr) continue;
      const NoteWritten* w = findNoteWritten(clip_, n.id);
      if (w == nullptr) continue;
      if (warpOf_.empty()) warpOf_.assign(notes_.size(), -1);
      warpOf_[i] = static_cast<int32_t>(warped_.size());
      warped_.push_back(Warped{g->group, w->written, NoteWarpState{n.id, g->group->id, n.start - g->start, 0.0, 0.0, 0.0}});
    }
  }

  // r.hz is the member's JI pitch: the pitch its group's warps give at its start, and how it follows there.
  void applyWarp(uint32_t i, Resolved& r) {
    Warped& w = warped_[static_cast<size_t>(warpOf_[i])];
    const std::optional<double> written = playableHz(w.written, t_, {});
    if (!written) return;  // a written identity the tuning cannot resolve: JI throughout
    w.state.jiHz = *r.hz;
    w.state.writtenHz = *written;
    w.state.cents = 1200.0 * std::log2(*written / *r.hz);
    if (!std::isfinite(w.state.cents)) {
      w.state.jiHz = w.state.writtenHz = w.state.cents = 0.0;
      return;
    }
    const double s = jiWarpWritten(w.group->warps, static_cast<double>(w.state.offset));
    if (s == 0.0) return;  // JI: anchor × ratio, exactly
    if (s == 1.0) {        // written: the Hz of the note as written, exactly; it follows the tuning as written
      r.hz = *written;
      r.follow = ownFollow(w.written);
      if (w.written.kind() == tuning::PitchKind::Degree) {
        r.degree = w.written.degree();
        r.equave = w.written.equave();
      }
      return;
    }
    const double hz = *r.hz * std::exp2(s * w.state.cents / 1200.0);  // inside a glide
    if (hz >= static_cast<double>(FLT_MIN) && hz <= static_cast<double>(FLT_MAX)) r.hz = hz;
  }

  NoteWarp warpFor(uint32_t i) const noexcept {
    if (warpOf_.empty() || warpOf_[i] < 0) return {};
    const Warped& w = warped_[static_cast<size_t>(warpOf_[i])];
    return w.state.jiHz > 0.0 ? NoteWarp{w.group->warps, w.state.offset, w.state.cents} : NoteWarp{};
  }

  // The pitch of note `ai` (resolved: `hz` at its own start) at content tick `tick`, clamped to the note's span: its
  // effective curve there (the free curve, the bends and its group's warps: note_bend.h) is added. Without a curve
  // record and a warp this is `hz` itself, exactly.
  double pitchAt(uint32_t ai, double hz, Ticks tick, int depth, int& hit) {
    const Note& a = *notes_[ai];
    const Ticks at = std::clamp<Ticks>(tick, a.start, a.start + std::max<Ticks>(a.length, 1));
    const NoteCurves* nc = findNoteCurves(clip_, a.id);
    const NoteWarp warp = warpFor(ai);
    if (nc == nullptr && !warp.any()) return hz;
    auto neighbour = [&](const NoteBend& b) -> std::optional<double> {
      if (!b.set()) return std::nullopt;
      const int32_t k = indexOf(b.other);
      return k < 0 ? std::nullopt : get(static_cast<uint32_t>(k), depth + 1, hit).hz;
    };
    const NoteBendTargets targets =
        nc != nullptr ? noteBendTargets(*nc, hz, neighbour(nc->bendIn), neighbour(nc->bendOut)) : NoteBendTargets{};
    const double cents = noteCurveCents(nc, a.length, targets, warp, static_cast<double>(at - a.start));
    return cents == 0.0 ? hz : hz * std::exp2(cents / 1200.0);
  }

  const NoteClip& clip_;
  const tuning::Tuning& t_;
  bool signal_;
  std::vector<const Note*> notes_;
  std::vector<std::pair<uint32_t, uint32_t>> byId_;  // (note id, index), sorted
  static constexpr int kNoHit = 1 << 20;
  static constexpr uint8_t kNew = 0, kDone = 1, kBusy = 2;  // kBusy + depth: being resolved at that depth
  std::vector<uint8_t> state_;
  std::vector<Resolved> out_;
  struct Warped {
    const JiGroup* group;
    tuning::PitchId written;
    NoteWarpState state;  // jiHz / writtenHz / cents: 0 until the note resolved
  };
  std::vector<Warped> warped_;
  std::vector<int32_t> warpOf_;  // by note index: its entry in warped_, −1 none; empty: the clip has no warped note
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
    cp.warped = r.takeWarped();
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

ClipPitches resolveClipPitches(const Project& p, const TrackPath& path, const NoteClip& clip,
                               const ResolveOptions& options) {
  BuiltTunings built;
  return resolveClip(p, path, clip, options, built);
}

}  // namespace pq::doc
