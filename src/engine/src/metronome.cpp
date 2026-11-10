// The metronome (T6.6): pq/engine/metronome.h describes it.
#include "pq/engine/metronome.h"

#include <algorithm>
#include <cmath>

namespace pq::engine {

namespace {

int64_t floorDiv(int64_t a, int64_t b) noexcept {  // b > 0
  const int64_t q = a / b;
  return (a % b != 0 && a < 0) ? q - 1 : q;
}

}  // namespace

Metronome::Metronome(double sampleRate) {
  length_ = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(kClickSeconds * sampleRate)));
  accentTable_.resize(length_);
  beatTable_.resize(length_);
  // A decaying sine (τ = 3 ms) starting at phase 0, faded to 0 over its last 2 ms so the truncation does not click.
  const double tau = 0.003, fade = 0.002;
  const auto fadeFrames = std::max<uint32_t>(1, static_cast<uint32_t>(fade * sampleRate));
  constexpr double kTwoPi = 6.283185307179586;
  for (uint32_t i = 0; i < length_; ++i) {
    const double t = static_cast<double>(i) / sampleRate;
    double env = std::exp(-t / tau);
    if (i + fadeFrames > length_) env *= static_cast<double>(length_ - i) / static_cast<double>(fadeFrames);
    accentTable_[i] = static_cast<float>(env * std::sin(kTwoPi * kAccentHz * t));
    beatTable_[i] = static_cast<float>(env * 0.7 * std::sin(kTwoPi * kBeatHz * t));
  }
}

void Metronome::setLevelDb(float db) noexcept {
  gain_.store(db <= -150.0f ? 0.0f : std::pow(10.0f, db / 20.0f), std::memory_order_relaxed);
}

void Metronome::clear() noexcept {  // RT-safe
  head_ = tail_ = 0;
  tempo_ = nullptr;
  expect_ = INT64_MIN;
}

void Metronome::push(int64_t time, bool accent) noexcept {  // RT-safe
  if (tail_ - head_ == kMaxClicks) {
    ++stats_.dropped;
    return;
  }
  // Sorted by time (M6 review L9: after the latency drops, a new click can come before queued ones; render() stops at
  // the first one past the block). In order, as nearly always: one compare.
  uint32_t i = tail_;
  for (; i != head_ && queue_[(i - 1) % kMaxClicks].time > time; --i) queue_[i % kMaxClicks] = queue_[(i - 1) % kMaxClicks];
  queue_[i % kMaxClicks] = Click{time, accent};
  ++tail_;
  ++stats_.clicks;
  if (accent) ++stats_.accents;
}

void Metronome::seek(Ticks t, const TempoImage& tempo, double sampleRate) noexcept {  // RT-safe
  const TimeSigMap& sm = tempo.signatures();
  seg_ = sm.changeIndexAt(t);
  const Ticks start = sm.changeStartTick(seg_);
  const TimeSignature sig = sm.changeSignature(seg_);
  beatTicks_ = ticksPerBeat(sig);
  beatsPerBar_ = std::max<int64_t>(1, sig.numerator);
  segEnd_ = seg_ + 1 < sm.changeCount() ? sm.changeStartTick(seg_ + 1) : INT64_MAX;
  int64_t k = -floorDiv(-(t - start), beatTicks_);  // ceil((t − start) / beat)
  nextTick_ = start + k * beatTicks_;
  if (nextTick_ >= segEnd_) {  // the next segment's first beat (segments start on bars)
    ++seg_;
    const TimeSignature s2 = sm.changeSignature(seg_);
    nextTick_ = sm.changeStartTick(seg_);
    beatTicks_ = ticksPerBeat(s2);
    beatsPerBar_ = std::max<int64_t>(1, s2.numerator);
    segEnd_ = seg_ + 1 < sm.changeCount() ? sm.changeStartTick(seg_ + 1) : INT64_MAX;
    k = 0;
  }
  const int64_t m = k % beatsPerBar_;
  beatInBar_ = m < 0 ? m + beatsPerBar_ : m;
  nextAccent_ = beatInBar_ == 0;
  nextSample_ = tempo.tempo().ticksToSamples(nextTick_, sampleRate);
}

void Metronome::advance(const TempoImage& tempo, double sampleRate) noexcept {  // RT-safe
  nextTick_ += beatTicks_;
  if (++beatInBar_ == beatsPerBar_) beatInBar_ = 0;
  if (nextTick_ >= segEnd_) {  // a signature change (on a bar line): its grid from its first beat
    const TimeSigMap& sm = tempo.signatures();
    ++seg_;
    const TimeSignature sig = sm.changeSignature(seg_);
    nextTick_ = sm.changeStartTick(seg_);
    beatTicks_ = ticksPerBeat(sig);
    beatsPerBar_ = std::max<int64_t>(1, sig.numerator);
    segEnd_ = seg_ + 1 < sm.changeCount() ? sm.changeStartTick(seg_ + 1) : INT64_MAX;
    beatInBar_ = 0;
  }
  nextAccent_ = beatInBar_ == 0;
  nextSample_ = tempo.tempo().ticksToSamples(nextTick_, sampleRate);
}

void Metronome::schedule(int64_t blockTime, std::span<const TransportRun> runs, const TempoImage& tempo,
                         const CountInState& countIn, double sampleRate, uint32_t latency,
                         bool songBeats) noexcept {  // RT-safe
  for (const TransportRun& r : runs) {
    const int64_t at = blockTime + r.frame + latency;  // engine time of the run's first frame, as heard
    const SampleTime end = r.pos + r.len;
    if (r.kind == TransportRun::CountIn) {
      if (countIn.beats == 0 || !(countIn.beatFrames > 0.0)) continue;
      auto k = static_cast<uint32_t>(std::max(0.0, std::floor(static_cast<double>(r.pos) / countIn.beatFrames)));
      for (; k < countIn.beats; ++k) {
        const SampleTime s = std::llround(static_cast<double>(k) * countIn.beatFrames);
        if (s >= end) break;
        if (s < r.pos) continue;
        push(at + (s - r.pos), k % countIn.beatsPerBar == 0);
        ++stats_.countInClicks;
      }
      continue;
    }
    if (!songBeats) {
      expect_ = INT64_MIN;
      continue;
    }
    if (&tempo != tempo_ || r.pos != expect_) {  // a locate, a loop wrap, a new tempo map: find the beat again
      tempo_ = &tempo;
      seek(tempo.tempo().firstTickAtOrAfterSample(r.pos, sampleRate), tempo, sampleRate);
    }
    // Bounded: a run of len frames holds at most len beats (each beat is at least one sample after the last).
    for (uint32_t guard = 0; nextSample_ < end && guard <= r.len; ++guard) {
      if (nextSample_ >= r.pos) push(at + (nextSample_ - r.pos), nextAccent_);
      advance(tempo, sampleRate);
    }
    expect_ = end;
  }
}

void Metronome::render(int64_t start, uint32_t frames, float* const* out, uint32_t numOut) noexcept {  // RT-safe
  const int64_t end = start + frames;
  const float g = gain_.load(std::memory_order_relaxed);
  const uint32_t ch = std::min<uint32_t>(numOut, 2);
  for (uint32_t i = head_; i != tail_; ++i) {
    const Click& c = queue_[i % kMaxClicks];
    if (c.time >= end) break;  // chronological: the rest start later
    const int64_t from = std::max(c.time, start), to = std::min<int64_t>(c.time + length_, end);
    if (from < to && g > 0.0f) {
      const float* tab = (c.accent ? accentTable_.data() : beatTable_.data()) + (from - c.time);
      const auto n = static_cast<uint32_t>(to - from);
      const auto off = static_cast<uint32_t>(from - start);
      for (uint32_t k = 0; k < ch; ++k) {
        float* dst = out[k] + off;
        for (uint32_t f = 0; f < n; ++f) dst[f] += g * tab[f];
      }
    }
    if (c.time + length_ <= end && i == head_) ++head_;  // ended (equal lengths: the ended ones are a prefix)
  }
}

}  // namespace pq::engine
