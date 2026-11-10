// Audio clip edit operations (T5.3): pq/document/edit_ops.h "Audio clips".

#include <algorithm>
#include <cmath>
#include <limits>

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
    if (!isWarped(c.warp)) return invalid("only warped clips loop");
    c.loop = loop;
    return ok();
  });
}

// ---- Warp markers and modes (T6.1; model.h WarpMarker, audio_clip.h warpFrameAt) ----------------------------------

namespace {
Status needWarped(const AudioClip& c) {
  return isWarped(c.warp) ? ok() : invalid("warp markers belong to warped clips (warp is Off)");
}
}  // namespace

EditResult setWarpMode(const Project& p, std::span<const ClipRef> clips, AudioWarp warp) {
  if (static_cast<uint8_t>(warp) > static_cast<uint8_t>(kLastAudioWarp)) return invalid("unknown warp mode");
  return editAudioClips(p, clips, "Clip Warp", [&](const ClipFound&, AudioClip& c) {
    c = withWarp(*p.timeline, c, warp, c.sourceBpm, sampleRate(p, c.sample));
    return ok();
  });
}

EditResult setAudioClipWarpParams(const Project& p, std::span<const ClipRef> clips, WarpParams params) {
  PQ_DOC_TRY(checkWarpParams(params));
  return editAudioClips(p, clips, "Warp Settings", [&](const ClipFound&, AudioClip& c) {
    c.warpParams = params;
    return ok();
  });
}

EditResult addWarpMarker(const Project& p, ClipRef clip, Ticks content) {
  PQ_DOC_TRY(checkTick(content, "warp marker"));
  return editAudioClips(p, std::span<const ClipRef>(&clip, 1), "Add Warp Marker", [&](const ClipFound&, AudioClip& c) -> Status {
    PQ_DOC_TRY(needWarped(c));
    if (c.markers.size() >= kMaxWarpMarkers) return invalid("too many warp markers");
    const auto it = std::lower_bound(c.markers.begin(), c.markers.end(), content,
                                     [](const WarpMarker& m, Ticks t) { return m.content < t; });
    if (it != c.markers.end() && it->content == content) return ok();  // already one there: nothing changes
    // Pinned where the source plays now: the law (and every other marker's song time) is unchanged.
    const WarpMarker m{warpFrameAt(c, sampleRate(p, c.sample), static_cast<double>(content)), content};
    c.markers.insert(it, m);
    return checkWarpMarkers(c.markers);  // a frame equal to a neighbour's (rounding) is refused, never stored
  });
}

EditResult moveWarpMarker(const Project& p, ClipRef clip, size_t index, Ticks content) {
  PQ_DOC_TRY(checkTick(content, "warp marker"));
  return editAudioClips(p, std::span<const ClipRef>(&clip, 1), "Move Warp Marker", [&](const ClipFound&, AudioClip& c) -> Status {
    PQ_DOC_TRY(needWarped(c));
    if (index >= c.markers.size()) return notFound("no such warp marker");
    // It keeps its source frame (the audio around it stretches); the others keep their content ticks, so their song
    // times stay. It stays strictly between its neighbours.
    Ticks lo = std::numeric_limits<Ticks>::min(), hi = std::numeric_limits<Ticks>::max();
    if (index > 0) lo = c.markers[index - 1].content + 1;
    if (index + 1 < c.markers.size()) hi = c.markers[index + 1].content - 1;
    c.markers[index].content = std::clamp(content, lo, hi);
    return ok();
  });
}

EditResult removeWarpMarker(const Project& p, ClipRef clip, size_t index) {
  return editAudioClips(p, std::span<const ClipRef>(&clip, 1), "Delete Warp Marker", [&](const ClipFound&, AudioClip& c) -> Status {
    PQ_DOC_TRY(needWarped(c));
    if (index >= c.markers.size()) return notFound("no such warp marker");
    c.markers.erase(c.markers.begin() + static_cast<std::ptrdiff_t>(index));
    return ok();
  });
}

// ---- Warp commands of the audio clip view (T6.8) -------------------------------------------------------------------

namespace {
// A new length for clip `c` on `t`, kept short of the next clip on the track (these commands never overwrite).
Ticks lengthBeforeNext(const Track& t, const AudioClip& c, Ticks want) {
  Ticks len = std::max<Ticks>(1, want);
  for (const ClipSlot& s : t.clips)
    if (s.id != c.id && s.start() > c.start) len = std::min(len, s.start() - c.start);
  return len;
}
}  // namespace

EditResult slideWarpMarker(const Project& p, ClipRef clip, size_t index, double sourceFrame) {
  if (!std::isfinite(sourceFrame)) return invalid("warp marker frame must be finite");
  return editAudioClips(p, std::span<const ClipRef>(&clip, 1), "Slide Warp Marker", [&](const ClipFound&, AudioClip& c) -> Status {
    PQ_DOC_TRY(needWarped(c));
    if (index >= c.markers.size()) return notFound("no such warp marker");
    // Strictly between the neighbours: the closest representable frames inside (a fraction of a frame off them).
    double lo = 0.0, hi = std::numeric_limits<double>::max();
    if (index > 0) lo = std::nextafter(c.markers[index - 1].sourceFrame, hi);
    if (index + 1 < c.markers.size()) hi = std::nextafter(c.markers[index + 1].sourceFrame, 0.0);
    if (!(lo <= hi)) return ok();  // no room: unchanged
    c.markers[index].sourceFrame = std::clamp(sourceFrame, lo, hi);
    return checkWarpMarkers(c.markers);
  });
}

EditResult warpFromHere(const Project& p, ClipRef clip, Ticks content, double bpm) {
  PQ_DOC_TRY(checkTick(content, "warp marker"));
  if (!(bpm >= kMinBpm && bpm <= kMaxBpm)) return invalid("tempo out of range");
  return editAudioClips(p, std::span<const ClipRef>(&clip, 1), "Warp From Here", [&](const ClipFound&, AudioClip& c) -> Status {
    PQ_DOC_TRY(needWarped(c));
    const double rate = sampleRate(p, c.sample);
    const double frame = warpFrameAt(c, rate, static_cast<double>(content));
    std::vector<WarpMarker> m;
    for (const WarpMarker& k : c.markers)
      if (k.content < content) m.push_back(k);
    const bool lone = m.empty();
    if (!lone && !(m.back().sourceFrame < frame)) return invalid("Warp From Here: the source runs backwards there");
    m.push_back({frame, content});
    if (lone) {
      c.sourceBpm = bpm;  // a lone marker reads at the source BPM on both sides
    } else {
      m.push_back({frame + framesPerContentTick(rate, bpm) * static_cast<double>(kPpq), content + kPpq});
    }
    c.markers = std::move(m);
    return checkWarpMarkers(c.markers);
  });
}

EditResult warpAsLoop(const Project& p, ClipRef clip, int32_t bars, AudioWarp offMode) {
  if (bars < 1 || bars > 1024) return invalid("bars out of range");
  if (!isWarped(offMode)) return invalid("Warp as Loop needs a warped mode");
  return editAudioClips(p, std::span<const ClipRef>(&clip, 1), "Warp as Loop", [&](const ClipFound& f, AudioClip& c) -> Status {
    const double rate = sampleRate(p, c.sample);
    double a = 0.0, b = 0.0;  // the source region, frames
    if (!isWarped(c.warp)) {
      a = c.contentStartFrame;
      b = a + c.seconds * rate * audioRateRatio(p, f.path, c);
      c = withWarp(*p.timeline, c, offMode, c.sourceBpm, rate);
    } else {
      const bool looped = c.loop.enabled && c.loop.end > c.loop.start;
      const Ticks c0 = looped ? c.loop.start : c.contentStart, c1 = looped ? c.loop.end : c.contentStart + c.length;
      a = warpFrameAt(c, rate, static_cast<double>(c0));
      b = warpFrameAt(c, rate, static_cast<double>(c1));
    }
    if (!(b > a)) return invalid("Warp as Loop: the clip plays no source");
    const Ticks len = static_cast<Ticks>(bars) * kTicksPerWhole;
    // sourceBpm so that `len` content ticks span b − a frames: fpt = (b − a) / len = rate · 60 / (bpm · kPpq).
    const double bpm = rate * 60.0 * static_cast<double>(len) / ((b - a) * static_cast<double>(kPpq));
    if (!(bpm >= kMinBpm && bpm <= kMaxBpm)) return invalid("Warp as Loop: the tempo would be out of range");
    c.sourceBpm = bpm;
    c.contentStart = static_cast<Ticks>(std::llround(a / framesPerContentTick(rate, bpm)));
    c.markers = {WarpMarker{a, c.contentStart}};  // lone: exact through the region's start at sourceBpm
    c.loop = {true, c.contentStart, c.contentStart + len};
    c.length = lengthBeforeNext(*f.track, c, len);
    return ok();
  });
}

EditResult autoWarpClips(const Project& p, std::span<const ClipRef> clips, AudioWarp warp, double bpm) {
  if (!isWarped(warp)) return invalid("auto-warp needs a warped mode");
  if (!(bpm >= kMinBpm && bpm <= kMaxBpm)) return invalid("tempo out of range");
  return editAudioClips(p, clips, "Auto-Warp", [&](const ClipFound& f, AudioClip& c) -> Status {
    if (isWarped(c.warp)) return ok();
    const SampleRef* s = findSample(p, c.sample);
    const double rate = s != nullptr ? s->rate : 48000.0;
    c = withWarp(*p.timeline, c, warp, bpm, rate);
    if (s != nullptr && !c.reverse) {
      const auto end = static_cast<Ticks>(std::floor(warpContentAt(c, rate, static_cast<double>(s->frames))));
      c.length = lengthBeforeNext(*f.track, c, end - c.contentStart);
    }
    return ok();
  });
}

}  // namespace pq::doc::ops
