// The audio sequence image (T5.4): pq/engine/audio_sequence.h.
#include "pq/engine/audio_sequence.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <string>

#include "pq/dsp/resample.h"

namespace pq::engine {

namespace {

constexpr double kFixedOneD = 4294967296.0;

int64_t toFixed(double v) { return static_cast<int64_t>(std::llround(v * kFixedOneD)); }

Error audioError(std::string m) { return Error{ErrorCode::InvalidArgument, "audio sequence: " + std::move(m), {}}; }

// Folds a RePitch content position into the loop (clipContentAt with fractional ticks).
double foldContent(double p, const ContentLoop& l) {
  if (!contentLooped(l) || p < static_cast<double>(l.end)) return p;
  const auto span = static_cast<double>(l.end - l.start);
  return static_cast<double>(l.start) + std::fmod(p - static_cast<double>(l.end), span);
}

}  // namespace

Result<std::unique_ptr<AudioSequenceImage>> AudioSequenceImage::build(std::span<const AudioTrackDesc> slots,
                                                                        std::vector<ClipSample> samples,
                                                                        const TempoMap& tempo,
                                                                        const AudioBuildOptions& options) {
  if (!(options.sampleRate >= 1000.0)) return audioError("bad sample rate");
  std::unique_ptr<AudioSequenceImage> img(new AudioSequenceImage());
  img->tempo_ = tempo;
  img->sr_ = options.sampleRate;
  const double sr = options.sampleRate;
  img->declick_ = options.edgeDeclick ? static_cast<uint32_t>(std::lround(std::max(options.declickSeconds, 0.0) * sr)) : 0u;
  const SampleTime D = img->declick_;
  img->samples_ = std::move(samples);
  img->slotFirst_.reserve(slots.size() + 1);
  std::set<uint32_t> qs;  // resampler stretches the clips can need
  for (const AudioTrackDesc& tr : slots) {
    img->slotFirst_.push_back(static_cast<uint32_t>(img->clips_.size()));
    const size_t first = img->clips_.size();
    for (size_t i = 0; i < tr.clips.size(); ++i) {
      const AudioClipDesc& d = tr.clips[i];
      if (d.length <= 0) return audioError("clip " + std::to_string(d.id) + ": length must be positive");
      if (d.warp == ClipWarp::Off && !(d.speed > 0.0)) return audioError("clip " + std::to_string(d.id) + ": speed must be positive");
      if (d.warp == ClipWarp::RePitch && !(d.framesPerTick > 0.0))
        return audioError("clip " + std::to_string(d.id) + ": framesPerTick must be positive");
      Clip c;
      c.id = d.id;
      c.sample = d.playable && d.sample < img->samples_.size() && d.gain > 0.0f ? d.sample : kNoSample;
      c.warp = d.warp;
      c.reverse = d.reverse;
      c.bodyStart = tempo.ticksToSamples(d.start, sr);
      c.bodyEnd = std::max(tempo.ticksToSamples(d.start + d.length, sr), c.bodyStart + 1);
      c.playStart = c.bodyStart;
      c.playEnd = c.bodyEnd;
      c.gain = d.gain;
      // Crossfade with the previous clip (the document keeps one only while the two are adjacent).
      const bool xin = d.crossfadeIn > 0 && i > 0 && tr.clips[i - 1].start + tr.clips[i - 1].length == d.start &&
                       img->clips_.size() > first;
      if (xin) {
        const Ticks half = d.crossfadeIn / 2;
        c.xinStart = tempo.ticksToSamples(d.start - half, sr);
        c.xinEnd = std::max(tempo.ticksToSamples(d.start + d.crossfadeIn - half, sr), c.xinStart + 1);
        c.playStart = std::min(c.playStart, c.xinStart);
        Clip& prev = img->clips_.back();
        prev.xoutStart = c.xinStart;
        prev.xoutEnd = c.xinEnd;
        prev.playEnd = std::max(prev.playEnd, c.xinEnd);
        prev.outStart = prev.bodyEnd;  // the crossfade replaces its fade-out and declick
        prev.outCurve = 0.0f;
      }
      const SampleTime body = c.bodyEnd - c.bodyStart;
      if (xin) {
        c.inEnd = c.bodyStart;
      } else {
        SampleTime f = tempo.ticksToSamples(d.start + d.fadeIn, sr) - c.bodyStart;
        c.inCurve = d.fadeInCurve;
        if (f < D) {
          f = D;
          c.inCurve = 0.0f;
        }
        c.inEnd = c.bodyStart + std::clamp<SampleTime>(f, 0, body);
      }
      {
        SampleTime f = c.bodyEnd - tempo.ticksToSamples(d.start + d.length - d.fadeOut, sr);
        c.outCurve = d.fadeOutCurve;
        if (f < D) {
          f = D;
          c.outCurve = 0.0f;
        }
        c.outStart = c.bodyEnd - std::clamp<SampleTime>(f, 0, body);  // a later crossfade out resets it
      }
      if (d.warp == ClipWarp::Off) {
        const double k = d.speed / sr;
        c.step = toFixed(k);
        const int64_t a = toFixed(d.contentStartFrame);
        if (d.reverse) {
          c.anchor = a + (body - 1) * c.step;  // f(S1 − 1) = contentStartFrame exactly
          c.step = -c.step;
        } else {
          c.anchor = a;
        }
        c.maxRate = c.peakRate = k;
        qs.insert(dsp::sincStretchQ(k));
        for (uint32_t L = 1; L <= dsp::mipLevelFor(k, dsp::kMaxMipLevels); ++L) qs.insert(dsp::sincStretchQ(std::ldexp(k, -static_cast<int>(L))));
      } else {
        c.start = d.start;
        c.length = d.length;
        c.contentStart = d.contentStart;
        c.loop = d.loop;
        c.looped = contentLooped(d.loop);
        c.framesPerTick = d.framesPerTick;
        // The rate range over the clip (tempo ramps): sampled, plus the ends, with a margin.
        double lo = 1e300, hi = 0.0;
        for (int k = 0; k <= 64; ++k) {
          const double bpm = tempo.bpmAt(d.start + d.length * k / 64);
          const double r = d.framesPerTick * bpm * static_cast<double>(kPpq) / (60.0 * sr);
          lo = std::min(lo, r);
          hi = std::max(hi, r);
        }
        c.maxRate = hi * 1.02;
        c.peakRate = hi;
        for (uint32_t q = dsp::sincStretchQ(hi * 1.02); q <= dsp::sincStretchQ(lo * 0.98); ++q) qs.insert(q);
        // A read at rate r > 2^L may use level L (at r / 2^L ∈ (1, 2] when the sample has the level it wants, above
        // when it has fewer levels).
        for (uint32_t L = 1; L <= dsp::mipLevelFor(hi * 1.02, dsp::kMaxMipLevels); ++L) {
          const double s = std::ldexp(1.0, -static_cast<int>(L));
          for (uint32_t q = dsp::sincStretchQ(hi * 1.02 * s); q <= dsp::sincStretchQ(std::max(lo * 0.98 * s, 1.0 + 1e-9)); ++q)
            qs.insert(q);
        }
      }
      img->clips_.push_back(c);
    }
    for (size_t i = first; i < img->clips_.size(); ++i) {  // where the static envelope is exactly `gain`
      Clip& c = img->clips_[i];
      c.flatStart = std::max(c.inEnd, c.xinEnd > c.xinStart ? c.xinEnd : c.bodyStart);
      c.flatEnd = std::min(c.outStart, c.xoutEnd > c.xoutStart ? c.xoutStart : c.bodyEnd);
      if (c.flatEnd < c.flatStart) c.flatEnd = c.flatStart;
    }
  }
  img->slotFirst_.push_back(static_cast<uint32_t>(img->clips_.size()));
  (void)dsp::prepareSincTable(dsp::ResampleQuality::High, 256);
  for (uint32_t q : qs) (void)dsp::prepareSincTable(dsp::ResampleQuality::High, q);
  return img;
}

double AudioSequenceImage::repitchOffset(const Clip& c, SampleTime s) const noexcept {  // RT-safe
  return tempo_.samplesToTicks(s, sr_) - static_cast<double>(c.start);
}

std::optional<double> AudioSequenceImage::sourceFrame(const Clip& c, SampleTime s) const {
  if (s < c.playStart || s >= c.playEnd) return std::nullopt;
  if (c.warp == ClipWarp::Off) {
    const auto p = static_cast<int64_t>(static_cast<uint64_t>(c.anchor) +
                                        static_cast<uint64_t>(s - c.bodyStart) * static_cast<uint64_t>(c.step));
    return static_cast<double>(p) / kFixedOneD;
  }
  const double o = repitchOffset(c, s);
  const double u = c.reverse ? static_cast<double>(c.length) - o : o;
  return foldContent(static_cast<double>(c.contentStart) + u, c.loop) * c.framesPerTick;
}

}  // namespace pq::engine
