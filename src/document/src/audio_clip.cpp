// Audio clip time math (T5.3): pq/document/audio_clip.h.
#include "pq/document/audio_clip.h"

#include <algorithm>
#include <cmath>
#include <optional>

#include "pq/tuning/rational.h"
#include "pq/tuning/tuning.h"

namespace pq::doc {

namespace {

Ticks floorMod(Ticks a, Ticks m) noexcept {
  const Ticks r = a % m;
  return r < 0 ? r + m : r;
}

// The RePitch start marker of a FORWARD clip given new bounds [s, e) (a note clip's rule, clip_time.h).
Ticks forwardContentStart(const AudioClip& c, Ticks s) noexcept {
  const ContentLoop l = contentLoop(c);
  if (s >= c.start) return clipContentAt(c.contentStart, l, s - c.start);
  const Ticks d = c.start - s;
  if (contentLooped(l) && c.contentStart >= l.start && c.contentStart < l.end)
    return l.start + floorMod(c.contentStart - d - l.start, l.end - l.start);  // unroll the loop backwards
  return c.contentStart - d;
}

}  // namespace

Ticks offClipLength(const TempoMap& tempo, Ticks start, double seconds) noexcept {
  const double end = tempo.secondsToTicks(tempo.ticksToSeconds(start) + seconds);
  const auto e = static_cast<Ticks>(std::llround(end));
  return std::max<Ticks>(1, e - start);
}

AudioClip syncedClip(const Timeline& timeline, AudioClip c) {
  if (c.warp == AudioWarp::Off) c.length = offClipLength(timeline.tempoMap, c.start, c.seconds);
  return c;
}

TrackPtr syncedTrack(const Timeline& tl, const TrackPtr& t) {
  std::optional<Track> copy;
  for (size_t i = 0; i < t->clips.size(); ++i) {
    const ClipSlot& sl = t->clips[i];
    if (!sl.audio) continue;
    AudioClip c = syncedClip(tl, *sl.audio);
    if (c == *sl.audio) continue;
    if (!copy) copy = *t;
    copy->clips[i].audio = std::make_shared<const AudioClip>(std::move(c));
  }
  for (size_t i = 0; i < t->children.size(); ++i) {
    TrackPtr k = syncedTrack(tl, t->children[i]);
    if (k == t->children[i]) continue;
    if (!copy) copy = *t;
    copy->children[i] = std::move(k);
  }
  if (!copy) return t;
  std::sort(copy->clips.begin(), copy->clips.end(), [](const ClipSlot& a, const ClipSlot& b) {
    return a.start() != b.start() ? a.start() < b.start() : a.id < b.id;
  });
  return std::make_shared<const Track>(std::move(*copy));
}

double framesPerContentTick(double rate, double sourceBpm) noexcept {
  return rate * 60.0 / (sourceBpm * static_cast<double>(kPpq));
}

const TuningDef* audioClipTuning(const Project& p, const TrackPath& path, const AudioClip& c) noexcept {
  switch (c.tuning.mode) {
    case TuningMode::Main:
      if (const TuningSlot* s = findTuning(p, c.tuning.main)) return &s->def;
      return nullptr;
    case TuningMode::Private: return c.tuning.privateDef.get();
    case TuningMode::Inherit: break;
  }
  return effectiveTuning(p, path);
}

double transposeCents(const AudioTranspose& t, const tuning::Tuning* tun) {
  switch (t.unit) {
    case TransposeUnit::Cents: return t.cents;
    case TransposeUnit::Ratio: return t.ratio.isPositive() ? tuning::cents(t.ratio) : 0.0;
    case TransposeUnit::Steps: {
      if (tun == nullptr || t.steps == 0) return 0.0;
      const std::optional<double> a = tuning::resolveHzDouble(tuning::PitchId::fromDegree(t.steps), *tun, {});
      const std::optional<double> b = tuning::resolveHzDouble(tuning::PitchId::fromDegree(0), *tun, {});
      if (!a || !b || *a <= 0.0 || *b <= 0.0) return 0.0;
      return 1200.0 * std::log2(*a / *b);
    }
  }
  return 0.0;
}

double audioRateRatio(const Project& p, const TrackPath& path, const AudioClip& c) {
  if (c.warp != AudioWarp::Off) return 1.0;
  double cents = 0.0;
  if (c.transpose.unit == TransposeUnit::Steps) {
    if (const TuningDef* def = audioClipTuning(p, path, c))
      if (Result<tuning::Tuning> t = buildTuning(*def)) cents = transposeCents(c.transpose, &*t);
  } else {
    cents = transposeCents(c.transpose, nullptr);
  }
  cents = std::clamp(cents, -kMaxTransposeCents, kMaxTransposeCents) +
          std::clamp(static_cast<double>(c.detuneCents), -kMaxDetuneCents, kMaxDetuneCents);
  return std::exp2(cents / 1200.0);
}

AudioClip audioWithBounds(const Timeline& timeline, const AudioClip& c, Ticks s, Ticks e, double rate, double ratio) {
  AudioClip n = c;
  if (c.warp == AudioWarp::Off) {
    const TempoMap& m = timeline.tempoMap;
    const double t0 = m.ticksToSeconds(c.start);
    const double ts = m.ticksToSeconds(s), te = m.ticksToSeconds(e);
    const double speed = rate * ratio;
    n.contentStartFrame = c.reverse ? c.contentStartFrame + (t0 + c.seconds - te) * speed
                                    : c.contentStartFrame + (ts - t0) * speed;
    n.seconds = te - ts;
    n.start = s;
    n.length = offClipLength(m, s, n.seconds);
    return n;
  }
  // RePitch: the forward clip's bounds; a reversed clip's piece [s, e) is the forward piece mirrored in the clip.
  const Ticks end = clipEnd(c);
  const Ticks fs = c.reverse ? c.start + (end - e) : s;
  n.contentStart = forwardContentStart(c, fs);
  n.start = s;
  n.length = e - s;
  return n;
}

std::pair<AudioClip, AudioClip> splitAudioAt(const Timeline& timeline, const AudioClip& c, Ticks t, ClipId rightId,
                                             double rate, double ratio) {
  AudioClip left = audioWithBounds(timeline, c, c.start, t, rate, ratio);
  AudioClip right = audioWithBounds(timeline, c, t, clipEnd(c), rate, ratio);
  if (c.warp == AudioWarp::Off) {
    // Exact complements: the right piece ends where the clip ended (no second rounding of tau(end)).
    right.seconds = c.seconds - left.seconds;
    right.length = offClipLength(timeline.tempoMap, t, right.seconds);
  }
  left.fadeOut = {};
  right.id = rightId;
  right.fadeIn = {};
  right.crossfadeIn = 0;
  return {std::move(left), std::move(right)};
}

AudioClip audioMovedTo(const Timeline& timeline, AudioClip c, Ticks start) {
  c.start = start;
  return syncedClip(timeline, std::move(c));
}

AudioClip withWarp(const Timeline& timeline, const AudioClip& c, AudioWarp warp, double sourceBpm, double rate) {
  if (warp == c.warp && (warp == AudioWarp::Off || sourceBpm == c.sourceBpm)) return c;
  AudioClip n = c;
  if (warp == AudioWarp::RePitch) {
    if (c.warp == AudioWarp::RePitch) {
      // A new source tempo: the start marker keeps its frame (content ticks scale with the tempo), the length stays.
      const double f = static_cast<double>(clipContentAt(c.contentStart, contentLoop(c), 0)) *
                       framesPerContentTick(rate, c.sourceBpm);
      n.contentStart = static_cast<Ticks>(std::llround(f / framesPerContentTick(rate, sourceBpm)));
      n.loop.enabled = false;
    } else {
      n.contentStart = static_cast<Ticks>(std::llround(c.contentStartFrame / framesPerContentTick(rate, sourceBpm)));
      n.loop.enabled = false;
    }
    n.sourceBpm = sourceBpm;
    n.warp = AudioWarp::RePitch;
    return n;
  }
  const TempoMap& m = timeline.tempoMap;
  n.contentStartFrame = static_cast<double>(clipContentAt(c.contentStart, contentLoop(c), 0)) *
                        framesPerContentTick(rate, c.sourceBpm);
  n.seconds = m.ticksToSeconds(clipEnd(c)) - m.ticksToSeconds(c.start);
  n.warp = AudioWarp::Off;
  return syncedClip(timeline, std::move(n));
}

}  // namespace pq::doc
