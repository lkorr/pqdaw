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
  const ClipSlot* prev = nullptr;  // the last clip lowered (its audible part ends at or before this one's start)
  for (size_t i = 0; i < t.clips.size(); ++i) {
    const ClipSlot& sl = t.clips[i];
    if (!sl.audio) continue;
    const AudioClip& c = *sl.audio;
    // M5 review H7: clips never sound on top of each other. Commands keep an audio track free of overlaps, but a tempo
    // increase lengthens warp-Off clips (they keep their seconds) and a file may hold overlaps: a clip ends where the
    // next one starts, and one the next covers from its start does not play.
    Ticks cut = 0;
    if (i + 1 < t.clips.size() && t.clips[i + 1].start() < c.start + std::max<Ticks>(c.length, 1)) {
      if (t.clips[i + 1].start() <= c.start) continue;
      cut = t.clips[i + 1].start();
    }
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
    d.cutEnd = cut;
    // The crossfade with the left neighbour that reaches this clip's start (commands keep it only while it does), at
    // most either clip's audible length.
    if (c.crossfadeIn > 0 && prev != nullptr && prev == &t.clips[i - 1] && prev->end() >= c.start)
      d.crossfadeIn = std::min({c.crossfadeIn, cut > 0 ? cut - c.start : d.length, std::max<Ticks>(c.start - prev->start(), 1)});
    out.clips.push_back(d);
    prev = &sl;
  }
  return out;
}

std::optional<double> sourceFrameAt(const AudioClipDesc& c, const TempoMap& tempo, double tau) {
  if (c.cutEnd > 0 && tempo.secondsToTicks(tau) >= static_cast<double>(c.cutEnd)) return std::nullopt;
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
