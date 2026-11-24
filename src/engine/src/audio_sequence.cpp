// The audio sequence image (T5.4): pq/engine/audio_sequence.h.
#include "pq/engine/audio_sequence.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <string>

#include "pq/dsp/complex_stream.h"
#include "pq/dsp/resample.h"
#include "pq/dsp/warp.h"

namespace pq::engine {

namespace {

using dsp::toFixed;

Error audioError(std::string m) { return Error{ErrorCode::InvalidArgument, "audio sequence: " + std::move(m), {}}; }

}  // namespace

// T6.3: a time-stretch clip's read rate, grains / slices (audio_sequence.h "Beats, Tones, Texture").
void AudioSequenceImage::stretchClip(AudioSequenceImage& img, Clip& c, const AudioClipDesc& d, double sr,
                                     std::set<uint32_t>& qs) {
  const uint32_t fileRate =
      d.sample < img.samples_.size() && img.samples_[d.sample].view().sampleRate != 0 ? img.samples_[d.sample].view().sampleRate
                                                                                      : static_cast<uint32_t>(sr);
  const double r = std::clamp(d.stretch.pitchRatio * static_cast<double>(fileRate) / sr, 1.0 / 64.0, 64.0);
  c.pitchRate = r;
  c.pitchStep = toFixed(r);
  c.maxRate = c.peakRate = r;
  qs.insert(dsp::sincStretchQ(r));
  for (uint32_t L = 1; L <= dsp::mipLevelFor(r, dsp::kMaxMipLevels); ++L) qs.insert(dsp::sincStretchQ(std::ldexp(r, -static_cast<int>(L))));
  if (dsp::warpComplex(d.warp)) {  // T6.5: the stretcher's settings (audio_sequence.h "Complex")
    c.seed = d.stretch.seed;
    const dsp::StretchParams fp =  // the formant rule (pq/dsp/complex_stream.h)
        dsp::complexFormants(d.warp, d.stretch.pitchRatio, d.stretch.formants, d.stretch.formantEnvelope);
    c.formantFactor = fp.formant;
    c.formantBase = fp.formantBase;
  }
  if (d.warp == dsp::AudioWarp::Tones || d.warp == dsp::AudioWarp::Texture || dsp::warpComplex(d.warp)) {
    const uint32_t n = dsp::warpGrainFrames(d.stretch.grainSeconds, sr);
    c.grain = n;
    c.hop = n / 2;
    // One window per grain size, shared.
    bool found = false;
    for (const auto& [size, at] : img.windowAt_)
      if (size == n) {
        c.winFirst = at;
        found = true;
      }
    if (!found) {
      c.winFirst = static_cast<uint32_t>(img.windows_.size());
      const std::vector<float> w = dsp::hannWindow(n);
      img.windows_.insert(img.windows_.end(), w.begin(), w.end());
      img.windowAt_.emplace_back(n, c.winFirst);
    }
    const double hr = static_cast<double>(c.hop) * r;
    if (d.warp == dsp::AudioWarp::Tones || dsp::warpComplex(d.warp))
      dsp::warpAlignFor(hr, static_cast<double>(fileRate), &c.alignLen, &c.alignLags);
    if (d.warp == dsp::AudioWarp::Texture) {
      c.jitter = dsp::warpJitterFrames(d.stretch.flux, hr);
      c.seed = d.stretch.seed;
    }
    return;
  }
  // Beats: the slice boundaries over the content the clip can play.
  c.sliceLoop = d.stretch.sliceLoop;
  c.envelope = std::clamp(d.stretch.envelope, 0.0f, 100.0f);
  double cLo = static_cast<double>(d.contentStart), cHi = static_cast<double>(d.contentStart + d.length);
  if (c.looped) {
    cLo = std::min(cLo, static_cast<double>(d.loop.start));
    cHi = static_cast<double>(d.loop.end);
  }
  std::vector<double> b;
  auto grid = [&](Ticks g) {
    g = std::max<Ticks>(g, kPpq / 32);
    for (double x = std::ceil(cLo / static_cast<double>(g)) * static_cast<double>(g); x < cHi && b.size() < kMaxSlices;
         x += static_cast<double>(g))
      b.push_back(x);
  };
  if (d.stretch.granulation > 0) {
    grid(d.stretch.granulation);
  } else if (!d.transients.empty()) {
    for (const double f : d.transients) {
      const double q = c.law.contentAt(f);
      if (q >= cLo && q < cHi && b.size() < kMaxSlices) b.push_back(q);
    }
  } else {
    grid(kPpq / 4);  // transients not analysed (yet): the 1/16 grid
  }
  if (!d.segments.empty())
    for (const WarpSegment& g : d.segments)
      if (static_cast<double>(g.content0) >= cLo && static_cast<double>(g.content0) < cHi) b.push_back(static_cast<double>(g.content0));
  if (c.looped) {
    b.push_back(static_cast<double>(d.loop.start));
    b.push_back(static_cast<double>(d.loop.end));
  }
  std::sort(b.begin(), b.end());
  std::vector<double> u;
  u.reserve(b.size());
  for (const double x : b)
    if (u.empty() || x - u.back() > 1e-6) u.push_back(x);
  c.sliceFirst = static_cast<uint32_t>(img.sliceContent_.size());
  c.sliceCount = static_cast<uint32_t>(u.size());
  for (const double x : u) {
    img.sliceContent_.push_back(x);
    img.sliceFrame_.push_back(c.law.frameAt(x));
  }
}

Result<std::unique_ptr<AudioSequenceImage>> AudioSequenceImage::build(std::span<const AudioTrackDesc> slots,
                                                                        std::vector<ClipSample> samples,
                                                                        const TempoMap& tempo,
                                                                        const AudioBuildOptions& options) {
  if (!(options.sampleRate >= 1000.0)) return audioError("bad sample rate");
  std::unique_ptr<AudioSequenceImage> img(new AudioSequenceImage());
  img->tempo_ = tempo;
  img->sr_ = options.sampleRate;
  const double sr = options.sampleRate;
  img->sliceFade_ = static_cast<uint32_t>(dsp::warpFadeFrames(sr));
  img->declick_ = options.edgeDeclick ? static_cast<uint32_t>(std::lround(std::max(options.declickSeconds, 0.0) * sr)) : 0u;
  const SampleTime declick = img->declick_;
  img->samples_ = std::move(samples);
  img->slotFirst_.reserve(slots.size() + 1);
  std::set<uint32_t> qs;  // resampler stretches the clips can need
  for (const AudioTrackDesc& tr : slots) {
    img->slotFirst_.push_back(static_cast<uint32_t>(img->clips_.size()));
    const size_t first = img->clips_.size();
    for (size_t i = 0; i < tr.clips.size(); ++i) {
      const AudioClipDesc& d = tr.clips[i];
      const SampleTime D = d.declick ? declick : 0;  // FZ.2: a clip without edge declicks
      if (d.length <= 0) return audioError("clip " + std::to_string(d.id) + ": length must be positive");
      if (d.warp == dsp::AudioWarp::Off && !(d.speed > 0.0)) return audioError("clip " + std::to_string(d.id) + ": speed must be positive");
      if (d.warp != dsp::AudioWarp::Off) {
        if (d.segments.empty() && !(d.framesPerTick > 0.0))
          return audioError("clip " + std::to_string(d.id) + ": framesPerTick must be positive");
        for (size_t k = 0; k < d.segments.size(); ++k)
          if (!(d.segments[k].framesPerTick > 0.0) || !std::isfinite(d.segments[k].frame0) ||
              (k > 0 && d.segments[k].content0 <= d.segments[k - 1].content0))
            return audioError("clip " + std::to_string(d.id) + ": warp segments must increase with positive slopes");
        if (dsp::warpStretches(d.warp) && !(d.stretch.pitchRatio > 0.0 && std::isfinite(d.stretch.pitchRatio)))
          return audioError("clip " + std::to_string(d.id) + ": pitchRatio must be positive");
      }
      Clip c;
      c.id = d.id;
      c.sample = d.playable && d.sample < img->samples_.size() && d.gain > 0.0f ? d.sample : kNoSample;
      c.warp = d.warp;
      c.reverse = d.reverse;
      if (d.cutEnd != 0 && (d.cutEnd <= d.start || d.cutEnd > d.start + d.length))
        return audioError("clip " + std::to_string(d.id) + ": cut end outside the clip");
      c.bodyStart = tempo.ticksToSamples(d.start, sr);
      // The whole clip's end: the content laws (anchors, the reverse mapping) use it, so a cut keeps every instant.
      const SampleTime fullEnd = std::max(tempo.ticksToSamples(d.start + d.length, sr), c.bodyStart + 1);
      const bool cut = d.cutEnd != 0 && d.cutEnd < d.start + d.length;
      c.bodyEnd = cut ? std::min(std::max(tempo.ticksToSamples(d.cutEnd, sr), c.bodyStart + 1), fullEnd) : fullEnd;
      c.playStart = c.bodyStart;
      c.playEnd = c.bodyEnd;
      c.gain = d.gain;
      // Crossfade with the previous clip (the document keeps one only while the two are adjacent).
      const AudioClipDesc& pd = tr.clips[i > 0 ? i - 1 : 0];
      const Ticks prevEnd = pd.cutEnd != 0 ? pd.cutEnd : pd.start + pd.length;
      const bool xin = d.crossfadeIn > 0 && i > 0 && prevEnd == d.start && img->clips_.size() > first;
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
        // A cut clip ends with the edge declick (its fade-out belongs to the end it no longer reaches).
        SampleTime f = cut ? 0 : c.bodyEnd - tempo.ticksToSamples(d.start + d.length - d.fadeOut, sr);
        c.outCurve = d.fadeOutCurve;
        if (f < D) {
          f = D;
          c.outCurve = 0.0f;
        }
        c.outStart = c.bodyEnd - std::clamp<SampleTime>(f, 0, body);  // a later crossfade out resets it
      }
      if (d.warp == dsp::AudioWarp::Off) {
        const double k = d.speed / sr;
        c.step = toFixed(k);
        const int64_t a = toFixed(d.contentStartFrame);
        if (d.reverse) {
          c.anchor = a + (fullEnd - c.bodyStart - 1) * c.step;  // f(S1 − 1) = contentStartFrame exactly (S1: whole clip)
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
        c.segFirst = static_cast<uint32_t>(img->segments_.size());
        if (d.segments.empty()) img->segments_.push_back(dsp::WarpSeg{0.0, 0.0, d.framesPerTick});
        for (const WarpSegment& g : d.segments)
          img->segments_.push_back(dsp::WarpSeg{static_cast<double>(g.content0), g.frame0, g.framesPerTick});
        c.segCount = static_cast<uint32_t>(img->segments_.size()) - c.segFirst;
        c.framesPerTick = img->segments_[c.segFirst].slope;
        // The law (its segments' address is set again once every clip is in: the vector may still grow).
        c.law.seg = img->segments_.data() + c.segFirst;
        c.law.count = c.segCount;
        if (c.looped) c.law.setLoop(static_cast<double>(d.loop.start), static_cast<double>(d.loop.end));
        double fptLo = 1e300, fptHi = 0.0;
        for (uint32_t k = c.segFirst; k < c.segFirst + c.segCount; ++k) {
          fptLo = std::min(fptLo, img->segments_[k].slope);
          fptHi = std::max(fptHi, img->segments_[k].slope);
        }
        if (d.warp == dsp::AudioWarp::RePitch) {
          // The rate range over the clip (tempo ramps, the segments' slopes): sampled, plus the ends, with a margin.
          double lo = 1e300, hi = 0.0;
          for (int k = 0; k <= 64; ++k) {
            const double bpm = tempo.bpmAt(d.start + d.length * k / 64);
            const double tps = bpm * static_cast<double>(kPpq) / (60.0 * sr);
            lo = std::min(lo, fptLo * tps);
            hi = std::max(hi, fptHi * tps);
          }
          c.maxRate = hi * 1.02;
          c.peakRate = hi;
          for (uint32_t q = dsp::sincStretchQ(hi * 1.02); q <= dsp::sincStretchQ(lo * 0.98); ++q) qs.insert(q);
          // A read at rate r > 2^L may use level L (at r / 2^L ∈ (1, 2] when the sample has the level it wants, above
          // when it has fewer levels).
          for (uint32_t L = 1; L <= dsp::mipLevelFor(hi * 1.02, dsp::kMaxMipLevels); ++L) {
            const double sc = std::ldexp(1.0, -static_cast<int>(L));
            for (uint32_t q = dsp::sincStretchQ(hi * 1.02 * sc); q <= dsp::sincStretchQ(std::max(lo * 0.98 * sc, 1.0 + 1e-9)); ++q)
              qs.insert(q);
          }
        } else {
          stretchClip(*img, c, d, sr, qs);
        }
      }
      img->clips_.push_back(c);
    }
    // M5 review M7: a crossfade between two clips that read the same sample with one continuous source law (a split
    // crossfaded back together) is equal gain; equal power would add +3 dB to identical material.
    for (size_t i = first + 1; i < img->clips_.size(); ++i) {
      Clip& c = img->clips_[i];
      Clip& p = img->clips_[i - 1];
      if (c.xinEnd <= c.xinStart || p.xoutStart != c.xinStart || c.sample != p.sample || c.sample == kNoSample ||
          c.warp != p.warp || c.reverse != p.reverse)
        continue;
      bool same = true;
      for (const SampleTime s : {c.xinStart, (c.xinStart + c.xinEnd) / 2, c.xinEnd - 1}) {
        const std::optional<double> a = img->sourceFrame(p, s), b = img->sourceFrame(c, s);
        same = same && a && b && std::abs(*a - *b) <= 1e-3;
      }
      c.xinLinear = p.xoutLinear = same;
    }
    for (size_t i = first; i < img->clips_.size(); ++i) {  // where the static envelope is exactly `gain`
      Clip& c = img->clips_[i];
      c.flatStart = std::max(c.inEnd, c.xinEnd > c.xinStart ? c.xinEnd : c.bodyStart);
      c.flatEnd = std::min(c.outStart, c.xoutEnd > c.xoutStart ? c.xoutStart : c.bodyEnd);
      if (c.flatEnd < c.flatStart) c.flatEnd = c.flatStart;
    }
  }
  img->slotFirst_.push_back(static_cast<uint32_t>(img->clips_.size()));
  for (Clip& c : img->clips_)
    if (c.segCount != 0) c.law.seg = img->segments_.data() + c.segFirst;
  (void)dsp::prepareSincTable(dsp::ResampleQuality::High, 256);
  for (uint32_t q : qs) (void)dsp::prepareSincTable(dsp::ResampleQuality::High, q);
  return img;
}

double AudioSequenceImage::repitchOffset(const Clip& c, SampleTime s) const noexcept {  // RT-safe
  return tempo_.samplesToTicks(s, sr_) - static_cast<double>(c.start);
}

double AudioSequenceImage::contentAt(const Clip& c, SampleTime s) const noexcept {  // RT-safe
  const double o = repitchOffset(c, s);
  return static_cast<double>(c.contentStart) + (c.reverse ? static_cast<double>(c.length) - o : o);
}

double AudioSequenceImage::warpFrame(const Clip& c, SampleTime s) const noexcept {  // RT-safe
  return c.law.frameAt(c.law.fold(contentAt(c, s)));
}

SampleTime AudioSequenceImage::sampleAtOffset(const Clip& c, double o) const noexcept {  // RT-safe
  const double x = static_cast<double>(c.start) + o;
  const double t0 = std::floor(x);
  const auto ti = static_cast<Ticks>(t0);
  const double f0 = tempo_.ticksToSamplesF(ti, sr_), f1 = tempo_.ticksToSamplesF(ti + 1, sr_);
  return static_cast<SampleTime>(std::ceil(f0 + (x - t0) * (f1 - f0) - 1e-7));
}

std::optional<double> AudioSequenceImage::sourceFrame(const Clip& c, SampleTime s) const {
  if (s < c.playStart || s >= c.playEnd) return std::nullopt;
  if (c.warp == dsp::AudioWarp::Off) {
    const auto p = static_cast<int64_t>(static_cast<uint64_t>(c.anchor) +
                                        static_cast<uint64_t>(s - c.bodyStart) * static_cast<uint64_t>(c.step));
    return static_cast<double>(p) * dsp::k2m32;
  }
  return warpFrame(c, s);
}

}  // namespace pq::engine
