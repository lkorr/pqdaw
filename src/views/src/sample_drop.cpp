// T5.7: audio files dropped into the arrangement (pq/views/sample_drop.h).

#include "pq/views/sample_drop.h"

#include <algorithm>

#include "pq/document/audio_clip.h"
#include "pq/document/commands.h"
#include "pq/views/sample_analysis.h"
#include "pq/views/view_context.h"

namespace pq::views {

SampleDropTarget sampleDropTargetFor(const doc::Project& p, doc::TrackId row) {
  SampleDropTarget t;
  if (!row.valid()) {
    t.kind = SampleDropTarget::Kind::AtEnd;
    return t;
  }
  const doc::Track* tr = doc::findTrackPtr(p, row);
  if (!tr) {
    t.kind = SampleDropTarget::Kind::AtEnd;
    return t;
  }
  t.track = row;
  switch (tr->kind) {
    case doc::TrackKind::Audio: t.kind = SampleDropTarget::Kind::OnTrack; break;
    case doc::TrackKind::Midi:
    case doc::TrackKind::Group: t.kind = SampleDropTarget::Kind::AfterTrack; break;
    case doc::TrackKind::Return:
    case doc::TrackKind::Master: t.kind = SampleDropTarget::Kind::Refused; break;
  }
  return t;
}

SampleDropPlan planSampleDrop(const doc::Project& p, const SampleDropTarget& target, std::span<const ProbedFile> audio,
                              Ticks at, bool oneTrack) {
  SampleDropPlan plan;
  plan.target = target;
  if (target.kind == SampleDropTarget::Kind::Refused || audio.empty()) return plan;
  const TempoMap& tempo = p.timeline->tempoMap;
  const bool onTrack = target.kind == SampleDropTarget::Kind::OnTrack;
  Ticks seq = std::max<Ticks>(0, at);
  for (uint32_t i = 0; i < audio.size(); ++i) {
    SampleDropPlan::Item it;
    it.file = i;
    it.pending = audio[i].state != ProbedFile::State::Ready;
    if (oneTrack) {
      it.newTrack = onTrack ? -1 : 0;
      it.start = seq;
    } else {
      it.newTrack = onTrack ? static_cast<int32_t>(i) - 1 : static_cast<int32_t>(i);
      it.start = std::max<Ticks>(0, at);
    }
    it.length = it.pending ? kTicksPerWhole : std::max<Ticks>(1, doc::offClipLength(tempo, it.start, audio[i].seconds()));
    if (oneTrack) seq = it.start + it.length;
    plan.newTracks = std::max<uint32_t>(plan.newTracks, static_cast<uint32_t>(it.newTrack + 1));
    plan.items.push_back(it);
  }
  return plan;
}

Result<doc::ops::Edit> sampleDropEdit(const doc::Project& p, doc::IdSource& ids, const SampleDropPlan& plan,
                                      std::span<const ProbedFile> audio, std::span<const doc::SampleResolution> resolved,
                                      std::string_view projectDir) {
  using namespace pq::doc;
  if (plan.target.kind == SampleDropTarget::Kind::Refused) return Error{ErrorCode::InvalidArgument, "this track takes no clips", {}};
  if (plan.items.empty()) return Error{ErrorCode::InvalidArgument, "no audio files", {}};
  for (const SampleDropPlan::Item& it : plan.items)
    if (audio[it.file].state != ProbedFile::State::Ready)
      return Error{ErrorCode::InvalidArgument, audio[it.file].displayName + ": " +
                                                   (audio[it.file].error.empty() ? "not probed yet" : audio[it.file].error), {}};
  Batch batch;
  batch.label = plan.items.size() == 1 ? "Drop Sample" : "Drop Samples";
  Snapshot snap = std::make_shared<const Project>(p);
  ChangeSet changes;
  // 1. The new tracks, each after the previous one (the first after the target, or at the end).
  std::vector<TrackId> newIds;
  TrackId after = plan.target.kind == SampleDropTarget::Kind::AtEnd ? TrackId{} : plan.target.track;
  for (uint32_t k = 0; k < plan.newTracks; ++k) {
    std::string name;
    for (const SampleDropPlan::Item& it : plan.items)
      if (it.newTrack == static_cast<int32_t>(k)) {
        name = audio[it.file].displayName;
        break;
      }
    auto e = ops::createTrack(*snap, ids, TrackKind::Audio, after, std::move(name));
    if (!e) return std::move(e).error();
    Result<Snapshot> next = apply(snap, e->command, changes);
    if (!next) return std::move(next).error();
    snap = std::move(*next);
    for (Command& c : std::get<Batch>(e->command.v).commands) batch.commands.push_back(std::move(c));
    newIds.push_back(e->tracks.at(0));
    after = newIds.back();
  }
  // 2. The table entries (one per distinct file; files already in the table are reused) and the clips.
  std::vector<SampleRef> added;
  std::vector<SampleId> fileSample(audio.size());
  for (const SampleDropPlan::Item& it : plan.items) {
    const ProbedFile& f = audio[it.file];
    if (fileSample[it.file].valid()) continue;
    for (const SampleResolution& r : resolved)
      if (!r.path.empty() && r.path == f.absPath) fileSample[it.file] = r.id;
    for (uint32_t j = 0; j < it.file && !fileSample[it.file].valid(); ++j)
      if (audio[j].absPath == f.absPath && fileSample[j].valid()) fileSample[it.file] = fileSample[j];
    if (fileSample[it.file].valid()) continue;
    SampleRef s;
    s.id = ids.sample();
    s.path = storedSamplePath(f.absPath, projectDir);
    s.contentHash = f.contentHash;
    s.size = f.size;
    s.frames = f.frames;
    s.rate = f.rate;
    s.channels = f.channels;
    s.displayName = f.displayName;
    fileSample[it.file] = s.id;
    added.push_back(std::move(s));
  }
  std::vector<ops::NewAudioClip> clips;
  clips.reserve(plan.items.size());
  for (const SampleDropPlan::Item& it : plan.items) {
    ops::NewAudioClip nc;
    nc.track = it.newTrack < 0 ? plan.target.track : newIds.at(static_cast<size_t>(it.newTrack));
    nc.start = it.start;
    nc.sample = fileSample[it.file];
    clips.push_back(std::move(nc));
  }
  auto e = ops::addAudioClips(*snap, ids, added, clips);
  if (!e) return std::move(e).error();
  ops::Edit out = std::move(*e);
  for (Command& c : std::get<Batch>(out.command.v).commands) batch.commands.push_back(std::move(c));
  out.command = std::move(batch);
  out.tracks = newIds;
  return out;
}

namespace {

// T6.8: auto-warp of the dropped clips (the "Auto-Warp long samples" preference): a sample longer than
// kAutoWarpSeconds whose tempo is known — its tempo tag or name, else a detection of confidence >= kAutoWarpConfidence,
// which may arrive later from the analysis worker — is switched to the default warp mode at that tempo. Dispatched
// with the drop's coalescing key: when nothing happened in between it joins the drop's undo step.
constexpr double kAutoWarpSeconds = 8.0;
constexpr float kAutoWarpConfidence = 0.6f;

void autoWarpOne(ViewContext& ctx, doc::ClipRef ref, const doc::AudioClipPtr& dropped, double bpm, uint64_t key) {
  const doc::Project& p = *ctx.doc();
  const doc::Track* t = doc::findTrackPtr(p, ref.track);
  if (!t) return;
  const int32_t i = doc::findClipIndex(*t, ref.clip);
  if (i < 0 || t->clips[static_cast<size_t>(i)].audio != dropped) return;  // edited (or gone) since the drop: left alone
  auto e = doc::ops::autoWarpClips(p, std::span<const doc::ClipRef>(&ref, 1), ctx.viewPrefs().defaultWarpMode, bpm);
  if (!e || e->empty()) return;
  if (Status s = ctx.store().dispatchCoalescing(e->command, key); !s) ctx.setStatus("Auto-Warp: " + s.error().message);
}

}  // namespace

Result<std::vector<doc::ClipRef>> dropSamples(ViewContext& ctx, const SampleDropTarget& target,
                                              std::span<const ProbedFile> audio, Ticks at, bool oneTrack) {
  const SampleDropPlan plan = planSampleDrop(*ctx.doc(), target, audio, at, oneTrack);
  auto e = sampleDropEdit(*ctx.doc(), ctx.store().ids(), plan, audio, ctx.sampleResolutions(), ctx.projectDir());
  if (!e) return std::move(e).error();
  static uint64_t dropCount = 0;
  const uint64_t key = 0x5444524F50000000ull + ++dropCount;  // "TDROP" + n: this drop's coalescing run
  if (Status s = ctx.store().dispatchCoalescing(e->command, key); !s) return std::move(s).error();
  if (ctx.viewPrefs().autoWarpLongSamples && doc::isWarped(ctx.viewPrefs().defaultWarpMode)) {
    for (const doc::ClipRef& ref : e->clips) {
      const doc::Project& p = *ctx.doc();
      const doc::Track* t = doc::findTrackPtr(p, ref.track);
      const int32_t i = t ? doc::findClipIndex(*t, ref.clip) : -1;
      if (i < 0 || !t->clips[static_cast<size_t>(i)].audio) continue;
      const doc::AudioClipPtr clip = t->clips[static_cast<size_t>(i)].audio;
      const doc::SampleRef* s = doc::findSample(p, clip->sample);
      if (!s || !(s->rate > 0.0) || static_cast<double>(s->frames) / s->rate <= kAutoWarpSeconds) continue;
      double fileBpm = 0.0;  // the tempo tag or name of the file it came from
      for (const ProbedFile& f : audio)
        if (f.contentHash == s->contentHash && f.frames == s->frames) fileBpm = f.bpm;
      if (fileBpm >= kMinBpm && fileBpm <= kMaxBpm) {
        autoWarpOne(ctx, ref, clip, fileBpm, key);
        continue;
      }
      ctx.sampleAnalysis().whenAnalysed(clip->sample, [&ctx, ref, clip, key](const SampleAnalysisResult* a) {
        if (a && a->tempo && a->tempo->estimate.bpm > 0.0 && a->tempo->estimate.confidence >= kAutoWarpConfidence)
          autoWarpOne(ctx, ref, clip, a->tempo->estimate.bpm, key);
      });
    }
  }
  return std::move(e->clips);
}

}  // namespace pq::views
