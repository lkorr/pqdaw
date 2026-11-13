// Render edits (FZ.4): flatten, bounce to a new track, consolidate audio — pq/document/edit_ops.h "Render edits";
// docs/plans/RB.md D5, "### FZ.4". The app renders the WAV; these builders turn it into one undo step.

#include <string>
#include <unordered_set>

#include "edit_common.h"
#include "edit_paths.h"
#include "pq/document/audio_clip.h"
#include "pq/document/lower.h"

namespace pq::doc::ops {

using namespace detail;

namespace {

Status checkRange(Ticks a, Ticks b) {
  PQ_DOC_TRY(checkTick(a, "start"));
  PQ_DOC_TRY(checkTick(b, "end"));
  if (b <= a) return invalid("the range is empty");
  return ok();
}

bool clipTrack(const Track& t) noexcept { return t.kind == TrackKind::Midi || t.kind == TrackKind::Audio; }

// `sample` is a valid entry; true when the table needs it added (an entry with its id that differs: refused).
Result<bool> needsSample(const Project& p, const SampleRef& sample) {
  PQ_DOC_TRY(checkSampleRef(sample));
  if (const SampleRef* have = findSample(p, sample.id)) {
    if (!(*have == sample)) return invalid("sample id already in use");
    return false;
  }
  return true;
}

// The warp-Off clip playing region `r` of the render whose file frame 0 is tick `origin` (edit_ops.h "Render edits").
Result<AudioClip> regionClip(const Project& p, IdSource& ids, const Track& t, const SampleRef& s, Ticks origin,
                             const RenderRegion& r) {
  PQ_DOC_TRY(checkRange(r.start, r.end));
  if (r.start < origin) return invalid("a region starts before the render");
  const TempoMap& tm = p.timeline->tempoMap;
  const SampleTime o = tm.ticksToSamples(origin, s.rate);
  const SampleTime a = tm.ticksToSamples(r.start, s.rate);
  const SampleTime b = tm.ticksToSamples(r.end, s.rate);
  if (b <= a) return invalid("a region shorter than a sample");
  AudioClip c;
  c.id = ids.clip();
  c.name = s.displayName;
  c.colour = t.colour;
  c.start = r.start;
  c.sample = s.id;
  c.warp = AudioWarp::Off;
  c.noEdgeFade = true;  // FZ.x-fix: the render is what plays there (a clip into sound must not ramp in)
  c.contentStartFrame = static_cast<double>(a - o);
  c.seconds = tm.ticksToSeconds(r.end) - tm.ticksToSeconds(r.start);  // its length syncs to exactly `end`
  c = syncedClip(*p.timeline, std::move(c));
  PQ_DOC_TRY(checkTick(clipEnd(c), "clip end"));
  return c;
}

// Appends AddClip per region (sorted, disjoint) to `e`; the clips join e.clips.
Status addRegions(const Project& p, IdSource& ids, Edit& e, const Track& t, TrackId into, const SampleRef& s,
                  Ticks origin, std::span<const RenderRegion> regions) {
  Ticks last = origin;
  for (const RenderRegion& r : regions) {
    if (r.start < last) return invalid("regions must be sorted and disjoint");
    Result<AudioClip> c = regionClip(p, ids, t, s, origin, r);
    if (!c) return std::move(c).error();
    last = r.end;
    e.clips.push_back({into, c->id});
    batchOf(e).commands.push_back(AddClip{into, nullptr, std::make_shared<const AudioClip>(std::move(*c)), LaneId{}});
  }
  return ok();
}

// FZ.x-fix: the blocks of `t` that keep running after Flatten (frozenKeep: their output reaches another track), as
// ids; an error when they, or another track, need its own notes / audio (gone once it plays its render).
Result<std::unordered_set<uint32_t>> flattenKeep(const Project& p, const Track& t) {
  const FrozenKeep k = frozenKeep(p, t);
  std::unordered_set<uint32_t> kept;
  for (const DeviceId d : k.blocks) kept.insert(d.value);
  bool source = k.source;
  for (const Wire& w : wiresOf(p))  // a kept instrument group played by its notes (kept for a member's sake)
    source = source || (w.noteSource == t.id && !w.toMixer() && kept.count(w.to.value) != 0);
  if (source)
    return invalid(std::string("its own ") + (t.kind == TrackKind::Midi ? "notes" : "audio") +
                   " still play another track (a cable from its source, or through its blocks): disconnect it to flatten");
  return kept;
}

}  // namespace

Status flattenable(const Project& p, TrackId track) {
  const Track* t = findTrackPtr(p, track);
  if (t == nullptr) return notFound("track not found");
  if (!clipTrack(*t)) return invalid("only MIDI and audio tracks can be flattened");
  Result<std::unordered_set<uint32_t>> kept = flattenKeep(p, *t);
  if (!kept) return std::move(kept).error();
  return ok();
}

EditResult flattenTrack(const Project& p, IdSource& ids, TrackId track, const SampleRef& sample,
                        std::span<const RenderRegion> regions) {
  const Track* t = findTrackPtr(p, track);
  if (t == nullptr) return notFound("track not found");
  if (!clipTrack(*t)) return invalid("only MIDI and audio tracks can be flattened");
  Result<std::unordered_set<uint32_t>> kept = flattenKeep(p, *t);
  if (!kept) return std::move(kept).error();
  Result<bool> add = needsSample(p, sample);
  if (!add) return std::move(add).error();
  Edit e = makeEdit("Flatten");
  Batch& b = batchOf(e);
  if (t->frozen) b.commands.push_back(SetTrackFrozen{track, false});
  for (const Wire& w : wiresOf(p))  // its source's cables (into its mixer or its blocks to go); a fresh main link below
    if (w.noteSource == track) b.commands.push_back(RemoveWire{w.id});
  for (const Wire& w : wiresOf(p))  // a kept block's cables into its own mixer: that is in the render
    if (w.mixer == track && !w.fromNoteSource() && kept->count(w.from.value) != 0) b.commands.push_back(RemoveWire{w.id});
  // The blocks that do not keep running: each one whose group stays (or none) — a removed group takes its members, and
  // every removed block its cables.
  for (const DevicePtr& d : t->devices)
    if (kept->count(d->id.value) == 0 && (!d->group.valid() || kept->count(d->group.value) != 0))
      b.commands.push_back(RemoveDevice{d->id});
  for (const TakeLanePtr& l : t->takes) b.commands.push_back(RemoveTakeLane{track, l->id});
  for (const ClipSlot& c : t->clips) b.commands.push_back(RemoveClip{{track, c.id}});
  if (t->kind != TrackKind::Audio) b.commands.push_back(SetTrackKind{track, TrackKind::Audio});
  b.commands.push_back(AddWire{mainLink(ids, End{{}, track}, End{{}, track})});
  if (*add) b.commands.push_back(AddSample{sample});
  PQ_DOC_TRY(addRegions(p, ids, e, *t, track, sample, 0, regions));
  e.tracks.push_back(track);
  return e;
}

EditResult bounceToNewTrack(const Project& p, IdSource& ids, TrackId track, const SampleRef& sample, Ticks origin,
                            std::span<const RenderRegion> regions) {
  const std::optional<TrackPath> path = findTrack(p, track);
  if (!path) return notFound("track not found");
  const Track& t = trackAt(p, *path);
  if (!clipTrack(t)) return invalid("only MIDI and audio tracks can be bounced");
  PQ_DOC_TRY(checkTick(origin, "bounce start"));
  Result<bool> add = needsSample(p, sample);
  if (!add) return std::move(add).error();
  TrackId parent;
  if (const Track* par = parentOf(p, *path)) parent = par->id;
  const uint32_t index = path->index[path->depth - 1] + 1;
  Track n = makeTrack(ids.track(), TrackKind::Audio, t.name + " Bounce");
  n.colour = t.colour;
  n.mixer.volumeDb = t.mixer.volumeDb;
  n.mixer.pan = t.mixer.pan;
  n.mixer.panMode = t.mixer.panMode;
  n.mixer.mute = t.mixer.mute;
  n.mixer.trackDelayMs = t.mixer.trackDelayMs;
  n.mixer.sends = t.mixer.sends;
  n.mixer.output = t.mixer.output;
  const TrackId nid = n.id;
  Edit e = makeEdit("Bounce to New Track");
  Batch& b = batchOf(e);
  b.commands.push_back(AddTrack{std::make_shared<const Track>(n), parent, index});
  b.commands.push_back(AddWire{mainLink(ids, End{{}, nid}, End{{}, nid})});
  if (*add) b.commands.push_back(AddSample{sample});
  PQ_DOC_TRY(addRegions(p, ids, e, n, nid, sample, origin, regions));
  b.commands.push_back(SetTrackActive{track, false});
  e.tracks.push_back(nid);
  return e;
}

EditResult consolidateAudio(const Project& p, IdSource& ids, std::span<const ConsolidatedAudio> tracks, Ticks start,
                            Ticks end) {
  PQ_DOC_TRY(checkRange(start, end));
  Edit e = makeEdit("Consolidate");
  ClipEdits ed(p);
  for (const ConsolidatedAudio& a : tracks) {
    Result<ClipEditor*> te = ed.editor(a.track);
    if (!te) return std::move(te).error();
    if ((*te)->track().kind != TrackKind::Audio) return invalid("consolidating renders audio tracks only");
    Result<bool> add = needsSample(p, a.sample);
    if (!add) return std::move(add).error();
    if (*add) batchOf(e).commands.push_back(AddSample{a.sample});
    Result<AudioClip> c = regionClip(p, ids, (*te)->track(), a.sample, start, RenderRegion{start, end});
    if (!c) return std::move(c).error();
    (*te)->clearRange(start, end, ids);
    e.clips.push_back({a.track, c->id});
    (*te)->add(ClipSlot{c->id, nullptr, std::make_shared<const AudioClip>(std::move(*c)), LaneId{}});
  }
  ed.emit(batchOf(e));
  return e;
}

}  // namespace pq::doc::ops
