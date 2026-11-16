// The Waveform's capture (pq/dsp/waveform.h; VZ.6). Ported from PQ Meter's modules/waveform.cpp (ingestSpan, its
// loop slots); the clocked loop follows dsp::ScopeBeat (pq/dsp/scope.h).

#include "pq/dsp/waveform.h"

#include <algorithm>
#include <cmath>

namespace pq::dsp {

namespace {

constexpr float kSplitHz = 700.0f;      // the brightness split (PQ Meter's)
constexpr float kQuiet = 1e-6f;         // −120 dBFS

void resetBin(WaveBin& b) noexcept {
  for (int c = 0; c < WaveBin::kChannels; ++c) {
    b.mn[c] = 1e9f;
    b.mx[c] = -1e9f;
    b.ss[c] = 0.0f;
    b.hs[c] = 0.0f;
  }
  b.n = 0;
}

}  // namespace

WaveCapture::WaveCapture(const WaveConfig& c) : ring_(kRingBins), loop_(kLoopSlots), loopStamp_(kLoopSlots, 0) {
  for (WaveBin& b : ring_) resetBin(b);
  resetBin(cur_);
  cfg_ = c;
  lpA_ = std::clamp(1.0f - std::exp(-2.0f * 3.14159265f * kSplitHz / static_cast<float>(cfg_.sampleRate > 0 ? cfg_.sampleRate : 48000.0)),
                    0.0f, 1.0f);
  clearLoop();
}

void WaveCapture::clearLoop() noexcept {
  for (WaveBin& b : loop_) resetBin(b);
  std::fill(loopStamp_.begin(), loopStamp_.end(), uint64_t{0});
  slotCur_ = -1;
  state_.phase = -1.0;
  ++stamp_;  // a reader recopies everything (every slot now differs from what it holds)
  std::fill(loopStamp_.begin(), loopStamp_.end(), stamp_);
}

void WaveCapture::configure(const WaveConfig& c) {
  std::lock_guard lock(mutex_);
  if (c == cfg_) return;
  const bool spanMoved = c.loop != cfg_.loop || c.loopBeats != cfg_.loopBeats || c.loopSeconds != cfg_.loopSeconds;
  if (c.sampleRate != cfg_.sampleRate && c.sampleRate > 0.0)
    lpA_ = std::clamp(1.0f - std::exp(-2.0f * 3.14159265f * kSplitHz / static_cast<float>(c.sampleRate)), 0.0f, 1.0f);
  cfg_ = c;
  if (spanMoved) clearLoop();
}

void WaveCapture::addFrame(float l, float r, double phase) noexcept {
  const float v[WaveBin::kChannels] = {l, r, 0.5f * (l + r), 0.5f * (l - r)};
  float hi[WaveBin::kChannels];
  for (int c = 0; c < WaveBin::kChannels; ++c) {
    lp_[c] += lpA_ * (v[c] - lp_[c]);
    hi[c] = v[c] - lp_[c];
    cur_.mn[c] = std::min(cur_.mn[c], v[c]);
    cur_.mx[c] = std::max(cur_.mx[c], v[c]);
    cur_.ss[c] += v[c] * v[c];
    cur_.hs[c] += hi[c] * hi[c];
  }
  if (++cur_.n >= kBinSize) {
    ring_[written_ % kRingBins] = cur_;
    ++written_;
    resetBin(cur_);
  }
  quiet_ = std::fabs(l) > kQuiet || std::fabs(r) > kQuiet ? 0 : quiet_ + 1;
  if (phase < 0.0) return;
  state_.phase = phase;
  const int64_t slot = std::clamp<int64_t>(static_cast<int64_t>(phase * kLoopSlots), 0, kLoopSlots - 1);
  WaveBin& b = loop_[static_cast<size_t>(slot)];
  if (slot != slotCur_) {  // the playhead entered a new slot: this pass owns it now
    resetBin(b);
    slotCur_ = slot;
  }
  loopStamp_[static_cast<size_t>(slot)] = stamp_;
  for (int c = 0; c < WaveBin::kChannels; ++c) {
    b.mn[c] = std::min(b.mn[c], v[c]);
    b.mx[c] = std::max(b.mx[c], v[c]);
    b.ss[c] += v[c] * v[c];
    b.hs[c] += hi[c] * hi[c];
  }
  ++b.n;
}

void WaveCapture::push(const float* beat, const float* l, const float* r, uint32_t n) noexcept {
  std::lock_guard lock(mutex_);
  const double sr = cfg_.sampleRate > 0.0 ? cfg_.sampleRate : 48000.0;
  const double beats = std::max(cfg_.loopBeats, 1.0 / 16.0);
  const double freeSpan = std::max(0.05, static_cast<double>(cfg_.loopSeconds)) * sr;
  bool clocked = false;
  size_t first = n, last = n;
  if (cfg_.loop) ++stamp_;  // the slots this chunk writes carry it (a reader copies what changed since its stamp)
  // A clock that stands still (the transport stopped on a position) moves no playhead: the picture holds.
  bool still = false;
  if (cfg_.loop && beat != nullptr) {
    float b0 = NAN, b1 = NAN;
    for (uint32_t i = 0; i < n; ++i)
      if (std::isfinite(beat[i])) {
        if (!std::isfinite(b0)) b0 = beat[i];
        b1 = beat[i];
      }
    still = std::isfinite(b0) && b0 == b1 && (!haveLast_ || lastBeat_ == b1);
  }
  for (uint32_t i = 0; i < n; ++i) {
    double phase = -1.0;
    if (cfg_.loop && !still) {
      const float b = beat != nullptr ? beat[i] : NAN;
      if (std::isfinite(b)) {
        clocked = true;
        if (first == n) first = i;
        last = i;
        double ph = std::fmod(static_cast<double>(b), beats);
        if (ph < 0.0) ph += beats;
        phase = ph / beats;
      } else {
        double ph = std::fmod(stream_ + static_cast<double>(i), freeSpan);
        if (ph < 0.0) ph += freeSpan;
        phase = ph / freeSpan;
      }
    }
    addFrame(l[i], r[i], phase);
  }
  stream_ += n;
  if (!cfg_.loop) return;
  if (still) {
    state_.clocked = true;
    state_.moving = false;
    return;
  }
  state_.clocked = clocked;
  if (!clocked) {
    state_.bpm = 0.0;
    state_.spanSamples = freeSpan;
    state_.moving = n > 0;
    haveLast_ = false;
    return;
  }
  // The tempo the clock moved at (ScopeBeat's estimate: over the chunk, or since the previous chunk when short).
  const float b0 = last - first >= 64 ? beat[first] : (haveLast_ ? lastBeat_ : beat[first]);
  const size_t frames = last - first >= 64 ? last - first : (haveLast_ ? last + 1 : last - first);
  const float d = beat[last] - b0;
  state_.moving = d != 0.0f;
  if (frames >= 64 && d > 0.0f && d < 4.0f) state_.bpm = static_cast<double>(d) / static_cast<double>(frames) * sr * 60.0;
  state_.spanSamples = state_.bpm > 0.0 ? beats * 60.0 / state_.bpm * sr : 0.0;
  lastBeat_ = beat[last];
  haveLast_ = true;
}

void WaveCapture::pushSilence(uint32_t n) noexcept {
  std::lock_guard lock(mutex_);
  for (uint32_t i = 0; i < n; ++i) addFrame(0.0f, 0.0f, -1.0);
  state_.moving = false;
}

uint64_t WaveCapture::quietFrames() const {
  std::lock_guard lock(mutex_);
  return quiet_;
}

uint64_t WaveCapture::bins() const {
  std::lock_guard lock(mutex_);
  return written_;
}

uint32_t WaveCapture::readBins(uint64_t from, uint32_t n, WaveBin* out, WaveBin* partial) const {
  std::lock_guard lock(mutex_);
  const uint64_t oldest = written_ > kRingBins ? written_ - kRingBins : 0;
  if (partial != nullptr) *partial = cur_;
  if (from < oldest) from = oldest;
  if (from >= written_) return 0;
  const auto k = static_cast<uint32_t>(std::min<uint64_t>(n, written_ - from));
  for (uint32_t i = 0; i < k; ++i) out[i] = ring_[(from + i) % kRingBins];
  return k;
}

uint64_t WaveCapture::readLoop(uint64_t since, WaveBin* slots, uint8_t* filled, WaveLoopState& state) const {
  std::lock_guard lock(mutex_);
  for (uint32_t s = 0; s < kLoopSlots; ++s) {
    if (loopStamp_[s] <= since) continue;
    slots[s] = loop_[s];
    filled[s] = loop_[s].n > 0 ? 1 : 0;
  }
  state = state_;
  state.stamp = stamp_;
  return stamp_;
}

}  // namespace pq::dsp
