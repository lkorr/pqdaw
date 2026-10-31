// The audio sequence of an audio track (T5.3): pq/document/audio_sequence.h.
#include "pq/document/audio_sequence.h"

#include <algorithm>
#include <cmath>

#include "pq/document/audio_clip.h"

namespace pq::doc {

AudioTrackDesc lowerAudioTrack(const Project& p, const TrackPath& path, const AudioLowerOptions& options) {
  const Track& t = trackAt(p, path);
  AudioTrackDesc out;
  out.id = t.id;
  if (t.kind != TrackKind::Audio) return out;
  const TempoMap& tempo = p.timeline->tempoMap;
  out.clips.reserve(t.clips.size());
  for (size_t i = 0; i < t.clips.size(); ++i) {
    const ClipSlot& sl = t.clips[i];
    if (!sl.audio) continue;
    const AudioClip& c = *sl.audio;
    const SampleRef* sample = findSample(p, c.sample);
    const double rate = sample != nullptr ? sample->rate : 48000.0;
    AudioClipDesc d;
    d.id = c.id;
    d.sample = c.sample;
    d.start = c.start;
    d.length = std::max<Ticks>(c.length, 1);
    d.warp = c.warp;
    d.reverse = c.reverse;
    d.playable = c.active && sample != nullptr && (!options.available || options.available(c.sample));
    d.startSeconds = tempo.ticksToSeconds(c.start);
    if (c.warp == AudioWarp::Off) {
      d.seconds = c.seconds;
      d.contentStartFrame = c.contentStartFrame;
      d.rateRatio = audioRateRatio(p, path, c);
      d.speed = rate * d.rateRatio;
    } else {
      d.seconds = tempo.ticksToSeconds(c.start + d.length) - d.startSeconds;
      d.contentStart = c.contentStart;
      d.loop = contentLoop(c);
      d.framesPerTick = framesPerContentTick(rate, c.sourceBpm);
    }
    d.gain = c.gainDb <= kMinClipGainDb ? 0.0f : std::pow(10.0f, std::min(c.gainDb, kMaxClipGainDb) / 20.0f);
    d.fadeIn = std::clamp<Ticks>(c.fadeIn.length, 0, d.length);
    d.fadeOut = std::clamp<Ticks>(c.fadeOut.length, 0, d.length - d.fadeIn);
    d.fadeInCurve = std::clamp(c.fadeIn.curve, -1.0f, 1.0f);
    d.fadeOutCurve = std::clamp(c.fadeOut.curve, -1.0f, 1.0f);
    // The crossfade with an adjacent left neighbour (commands keep it only while adjacent), at most either clip.
    if (c.crossfadeIn > 0 && i > 0 && t.clips[i - 1].end() == c.start)
      d.crossfadeIn = std::min({c.crossfadeIn, d.length, std::max<Ticks>(t.clips[i - 1].length(), 1)});
    out.clips.push_back(d);
  }
  return out;
}

std::optional<double> sourceFrameAt(const AudioClipDesc& c, const TempoMap& tempo, double tau) {
  if (c.warp == AudioWarp::Off) {
    const double x = tau - c.startSeconds;
    if (x < 0.0 || x >= c.seconds) return std::nullopt;
    return c.contentStartFrame + (c.reverse ? c.seconds - x : x) * c.speed;
  }
  const double x = tempo.secondsToTicks(tau) - static_cast<double>(c.start);
  const auto len = static_cast<double>(c.length);
  if (x < 0.0 || x >= len) return std::nullopt;
  double pos = static_cast<double>(c.contentStart) + (c.reverse ? len - x : x);
  if (contentLooped(c.loop) && pos >= static_cast<double>(c.loop.end)) {
    const auto span = static_cast<double>(c.loop.end - c.loop.start);
    pos = static_cast<double>(c.loop.start) + std::fmod(pos - static_cast<double>(c.loop.end), span);
  }
  return pos * c.framesPerTick;
}

}  // namespace pq::doc
