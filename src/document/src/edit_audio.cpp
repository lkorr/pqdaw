// Audio clip edit operations (T5.3): pq/document/edit_ops.h "Audio clips".

#include <algorithm>
#include <cmath>

#include "edit_common.h"
#include "pq/tuning/rational.h"

namespace pq::doc::ops {

using namespace detail;

namespace {

// One SetAudioClip per clip that `f(const ClipFound&, AudioClip&) → Status` changes; every clip must be an audio clip.
template <class F>
EditResult editAudioClips(const Project& p, std::span<const ClipRef> clips, std::string label, F&& f) {
  Edit e = makeEdit(std::move(label));
  std::vector<ClipId> done;
  for (const ClipRef& r : clips) {
    Result<ClipFound> found = findAnyClip(p, r);
    if (!found) return std::move(found).error();
    if (!found->audio) return invalid("not an audio clip");
    if (std::find(done.begin(), done.end(), r.clip) != done.end()) continue;
    done.push_back(r.clip);
    AudioClip c = *found->audio;
    PQ_DOC_TRY(f(*found, c));
    c = syncedClip(*p.timeline, std::move(c));
    e.clips.push_back(r);
    if (c == *found->audio) continue;
    batchOf(e).commands.push_back(SetAudioClip{r, std::make_shared<const AudioClip>(std::move(c))});
  }
  return e;
}

double sampleRate(const Project& p, SampleId id) {
  const SampleRef* r = findSample(p, id);
  return r != nullptr ? r->rate : 48000.0;
}

}  // namespace

EditResult addAudioClips(const Project& p, IdSource& ids, std::span<const SampleRef> newSamples,
                         std::span<const NewAudioClip> clips) {
  Edit e = makeEdit(clips.size() == 1 ? "Add Audio Clip" : "Add Audio Clips");
  for (const SampleRef& s : newSamples) {
    PQ_DOC_TRY(checkSampleRef(s));
    if (findSample(p, s.id)) return invalid("sample id already in use");
    batchOf(e).commands.push_back(AddSample{s});
  }
  auto sampleOf = [&](SampleId id) -> const SampleRef* {
    for (const SampleRef& s : newSamples)
      if (s.id == id) return &s;
    return findSample(p, id);
  };
  ClipEdits ed(p);
  for (const NewAudioClip& nc : clips) {
    PQ_DOC_TRY(checkTick(nc.start, "clip start"));
    const SampleRef* s = sampleOf(nc.sample);
    if (s == nullptr) return notFound("unknown sample id " + std::to_string(nc.sample.value));
    Result<ClipEditor*> te = ed.editor(nc.track);
    if (!te) return std::move(te).error();
    if ((*te)->track().kind != TrackKind::Audio) return invalid("audio clips live on audio tracks");
    AudioClip c;
    c.id = ids.clip();
    c.name = nc.name.empty() ? s->displayName : nc.name;
    c.colour = (*te)->track().colour;
    c.start = nc.start;
    c.sample = s->id;
    c.warp = AudioWarp::Off;
    c.seconds = static_cast<double>(s->frames) / s->rate;
    c = syncedClip(*p.timeline, std::move(c));
    PQ_DOC_TRY(checkTick(clipEnd(c), "clip end"));
    (*te)->clearRange(c.start, clipEnd(c), ids);
    e.clips.push_back({nc.track, c.id});
    (*te)->add(ClipSlot{c.id, nullptr, std::make_shared<const AudioClip>(std::move(c))});
  }
  ed.emit(batchOf(e));
  return e;
}

EditResult setAudioClipGain(const Project& p, std::span<const ClipRef> clips, float gainDb) {
  if (!std::isfinite(gainDb)) return invalid("gain must be finite");
  const float g = std::clamp(gainDb, kMinClipGainDb, kMaxClipGainDb);
  return editAudioClips(p, clips, "Clip Gain", [&](const ClipFound&, AudioClip& c) {
    c.gainDb = g;
    return ok();
  });
}

EditResult setAudioClipTranspose(const Project& p, std::span<const ClipRef> clips, AudioTranspose t) {
  switch (t.unit) {
    case TransposeUnit::Cents:
      if (!std::isfinite(t.cents)) return invalid("transpose must be finite");
      t.cents = std::clamp(t.cents, -kMaxTransposeCents, kMaxTransposeCents);
      break;
    case TransposeUnit::Steps: t.steps = std::clamp(t.steps, -kMaxTransposeSteps, kMaxTransposeSteps); break;
    case TransposeUnit::Ratio:
      if (!t.ratio.isPositive() || std::abs(tuning::cents(t.ratio)) > kMaxTransposeCents)
        return invalid("a transpose ratio must be positive and within 48 semitones");
      break;
  }
  return editAudioClips(p, clips, "Clip Transpose", [&](const ClipFound&, AudioClip& c) {
    c.transpose = t;
    return ok();
  });
}

EditResult setAudioClipDetune(const Project& p, std::span<const ClipRef> clips, float cents) {
  if (!std::isfinite(cents)) return invalid("detune must be finite");
  const auto d = static_cast<float>(std::clamp(static_cast<double>(cents), -kMaxDetuneCents, kMaxDetuneCents));
  return editAudioClips(p, clips, "Clip Detune", [&](const ClipFound&, AudioClip& c) {
    c.detuneCents = d;
    return ok();
  });
}

EditResult setAudioClipsReverse(const Project& p, std::span<const ClipRef> clips, bool reverse) {
  return editAudioClips(p, clips, "Reverse Clip", [&](const ClipFound&, AudioClip& c) {
    c.reverse = reverse;
    return ok();
  });
}

EditResult setAudioClipWarp(const Project& p, std::span<const ClipRef> clips, AudioWarp warp, double sourceBpm) {
  if (!std::isfinite(sourceBpm)) return invalid("source BPM must be finite");
  const double bpm = std::clamp(sourceBpm, kMinBpm, kMaxBpm);
  return editAudioClips(p, clips, "Clip Warp", [&](const ClipFound&, AudioClip& c) {
    c = withWarp(*p.timeline, c, warp, bpm, sampleRate(p, c.sample));
    return ok();
  });
}

EditResult setAudioClipFades(const Project& p, ClipRef clip, AudioFade in, AudioFade out) {
  if (!std::isfinite(in.curve) || !std::isfinite(out.curve)) return invalid("fade curves must be finite");
  return editAudioClips(p, std::span<const ClipRef>(&clip, 1), "Clip Fades", [&](const ClipFound&, AudioClip& c) {
    c.fadeIn = {std::clamp<Ticks>(in.length, 0, c.length), std::clamp(in.curve, -1.0f, 1.0f)};
    c.fadeOut = {std::clamp<Ticks>(out.length, 0, c.length - c.fadeIn.length), std::clamp(out.curve, -1.0f, 1.0f)};
    return ok();
  });
}

EditResult setAudioClipCrossfade(const Project& p, ClipRef clip, Ticks length) {
  return editAudioClips(p, std::span<const ClipRef>(&clip, 1), "Crossfade", [&](const ClipFound& f, AudioClip& c) -> Status {
    if (length <= 0) {
      c.crossfadeIn = 0;
      return ok();
    }
    const Track& t = *f.track;
    const int32_t i = findClipIndex(t, c.id);
    // The left neighbour ends exactly at the start (or after it: warp-Off clips a tempo increase lengthened, M5 H7).
    const ClipSlot& l = t.clips[static_cast<size_t>(i > 0 ? i - 1 : 0)];
    if (i <= 0 || l.end() < c.start || l.start() >= c.start)
      return invalid("a crossfade needs a clip ending exactly where this one starts");
    const Ticks left = c.start - l.start();
    c.crossfadeIn = std::min({length, c.length, left});
    return ok();
  });
}

EditResult setAudioClipLoop(const Project& p, ClipRef clip, ClipLoop loop) {
  PQ_DOC_TRY(checkLoop(loop));
  return editAudioClips(p, std::span<const ClipRef>(&clip, 1), "Clip Loop", [&](const ClipFound&, AudioClip& c) -> Status {
    if (c.warp != AudioWarp::RePitch) return invalid("only Re-Pitch clips loop");
    c.loop = loop;
    return ok();
  });
}

}  // namespace pq::doc::ops
