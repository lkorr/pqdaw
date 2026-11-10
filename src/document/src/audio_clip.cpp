// Audio clip time math (T5.3): pq/document/audio_clip.h.
#include "pq/document/audio_clip.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <span>

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
  for (size_t k = 0; k < t->takes.size(); ++k) {  // T6.1: take lanes (an Off clip's start stays: still sorted)
    std::shared_ptr<TakeLane> lane;
    for (size_t i = 0; i < t->takes[k]->clips.size(); ++i) {
      const ClipSlot& sl = t->takes[k]->clips[i];
      if (!sl.audio) continue;
      AudioClip c = syncedClip(tl, *sl.audio);
      if (c == *sl.audio) continue;
      if (!lane) lane = std::make_shared<TakeLane>(*t->takes[k]);
      lane->clips[i].audio = std::make_shared<const AudioClip>(std::move(c));
    }
    if (!lane) continue;
    if (!copy) copy = *t;
    copy->takes[k] = std::move(lane);
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

namespace {
// The segment of a marker list (size ≥ 2) a coordinate falls in: the index i of its left marker, i + 1 < size; before
// the first marker the first segment, after the last the last one. `key` reads a marker's coordinate.
template <class Key>
size_t warpSegment(std::span<const WarpMarker> m, double x, Key key) noexcept {
  const auto it = std::upper_bound(m.begin(), m.end(), x, [&](double v, const WarpMarker& w) { return v < key(w); });
  const auto i = static_cast<size_t>(it - m.begin());
  return std::clamp<size_t>(i == 0 ? 0 : i - 1, 0, m.size() - 2);
}
double contentOf(const WarpMarker& w) noexcept { return static_cast<double>(w.content); }
double frameOf(const WarpMarker& w) noexcept { return w.sourceFrame; }
}  // namespace

double warpFrameAt(std::span<const WarpMarker> m, double fpt, double content) noexcept {  // RT-safe
  if (m.empty()) return content * fpt;
  if (m.size() == 1) return m[0].sourceFrame + (content - contentOf(m[0])) * fpt;
  const size_t i = warpSegment(m, content, contentOf);
  const double slope = (m[i + 1].sourceFrame - m[i].sourceFrame) / (contentOf(m[i + 1]) - contentOf(m[i]));
  return m[i].sourceFrame + (content - contentOf(m[i])) * slope;
}

double warpContentAt(std::span<const WarpMarker> m, double fpt, double frame) noexcept {  // RT-safe
  if (m.empty()) return frame / fpt;
  if (m.size() == 1) return contentOf(m[0]) + (frame - m[0].sourceFrame) / fpt;
  const size_t i = warpSegment(m, frame, frameOf);
  const double slope = (contentOf(m[i + 1]) - contentOf(m[i])) / (m[i + 1].sourceFrame - m[i].sourceFrame);
  return contentOf(m[i]) + (frame - m[i].sourceFrame) * slope;
}

double warpFrameAt(const AudioClip& c, double rate, double content) noexcept {
  return warpFrameAt(c.markers, framesPerContentTick(rate, c.sourceBpm), content);
}

double warpContentAt(const AudioClip& c, double rate, double frame) noexcept {
  return warpContentAt(c.markers, framesPerContentTick(rate, c.sourceBpm), frame);
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
  return audioPitchRatio(p, path, c);
}

double audioPitchRatio(const Project& p, const TrackPath& path, const AudioClip& c) {
  if (!warpTransposes(c.warp)) return 1.0;
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
  if (isWarped(warp)) {
    if (isWarped(c.warp)) {
      if (sourceBpm != c.sourceBpm) {
        // A new source tempo: the start marker keeps its frame (the implied slope changes; markers keep theirs), the
        // length in ticks stays.
        const double f = warpFrameAt(c, rate, static_cast<double>(clipContentAt(c.contentStart, contentLoop(c), 0)));
        n.sourceBpm = sourceBpm;
        n.contentStart = static_cast<Ticks>(std::llround(warpContentAt(n, rate, f)));
        n.loop.enabled = false;
      }
    } else {
      n.sourceBpm = sourceBpm;  // T6.1: through the markers (kept from an earlier warped life), else the implied segment
      n.contentStart = static_cast<Ticks>(std::llround(warpContentAt(n, rate, c.contentStartFrame)));
      n.loop.enabled = false;
    }
    n.warp = warp;  // between warped modes only the mode changes: content ticks and placement stay
    return n;
  }
  const TempoMap& m = timeline.tempoMap;
  n.contentStartFrame = warpFrameAt(c, rate, static_cast<double>(clipContentAt(c.contentStart, contentLoop(c), 0)));
  n.seconds = m.ticksToSeconds(clipEnd(c)) - m.ticksToSeconds(c.start);
  n.warp = AudioWarp::Off;
  return syncedClip(timeline, std::move(n));
}

}  // namespace pq::doc
