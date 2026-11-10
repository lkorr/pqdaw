// T6.7b: pq/engine/capture_fifo.h describes it.
#include "pq/engine/capture_fifo.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "pq/dsp/resample.h"

namespace pq::engine {

namespace {

// One interleaved sample → float (the inverse of planarToInterleaved's scaling: full scale = 2^(bits−1)).
inline float sampleAt(const uint8_t* p, SampleFormat fmt) noexcept {
  switch (fmt) {
    case SampleFormat::Float32: {
      float f;
      std::memcpy(&f, p, 4);
      return f;
    }
    case SampleFormat::Int32:
    case SampleFormat::Int24In32: {
      int32_t v;
      std::memcpy(&v, p, 4);
      return static_cast<float>(v) * (1.0f / 2147483648.0f);
    }
    case SampleFormat::Int24: {
      const int32_t v = static_cast<int32_t>(static_cast<uint32_t>(p[0]) << 8 | static_cast<uint32_t>(p[1]) << 16 |
                                             static_cast<uint32_t>(p[2]) << 24);
      return static_cast<float>(v) * (1.0f / 2147483648.0f);
    }
    case SampleFormat::Int16: {
      int16_t v;
      std::memcpy(&v, p, 2);
      return static_cast<float>(v) * (1.0f / 32768.0f);
    }
  }
  return 0.0f;
}

}  // namespace

void CaptureFifo::prepare(uint32_t channels, uint32_t capacityFrames, uint32_t target, uint32_t high, double rate) {
  channels_ = std::min(channels, kMaxChannels);
  uint32_t cap = 16;
  while (cap < capacityFrames) cap <<= 1;
  mask_ = cap - 1;
  data_ = AlignedBuffer<float>(static_cast<size_t>(cap) * std::max<uint32_t>(channels_, 1));
  high_ = std::min(high, cap - 1);
  target_ = std::min(target, high_);
  hnsPerFrame_ = rate > 0.0 ? 1e7 / rate : 0.0;
  underflows_ = dropped_ = 0;
  // T6.x-fix-ui-b: the drift loop's constants in frames at `rate`, the sinc table, the chunk window.
  rate_ = rate > 0.0 ? rate : 48000.0;
  const double loop = kLoopSeconds * rate_;
  kp_ = 2.0 / loop;  // critically damped: ω = 1 / loop
  ki_ = 1.0 / (loop * loop);
  tauFilter_ = kFilterSeconds * rate_;
  engage_ = kEngageSeconds * rate_;
  setpointFrames_ = kSetpointSeconds * rate_;
  sinc_ = &dsp::prepareSincTable(dsp::ResampleQuality::High, 256);
  window_ = AlignedBuffer<float>(static_cast<size_t>(kWindow) * std::max<uint32_t>(channels_, 1));
  spare_ = AlignedBuffer<float>(kChunk);
  reset();
}

void CaptureFifo::reset() noexcept {  // RT-safe
  w_ = r_ = 0;
  primed_ = false;
  qpc_ = qpcFrame_ = 0;
  haveSetpoint_ = resampling_ = false;
  setSum_ = setFrames_ = setpoint_ = sinceSetpoint_ = 0.0;
  err_ = integ_ = 0.0;
  ratio_ = 1.0;
  step_ = dsp::kFixedOne;
  frac_ = 0;
}

void CaptureFifo::push(const void* data, SampleFormat fmt, uint32_t deviceChannels, uint32_t frames,
                       uint64_t qpc100ns) noexcept {  // RT-safe
  if (channels_ == 0 || frames == 0) return;
  const uint32_t cap = mask_ + 1;
  const uint32_t room = resampling_ ? cap - kHistory : cap;  // resampling: the filter's left half stays readable
  if (frames > room) {  // more than the whole ring: only its last `room` frames can stay
    const uint32_t skip = frames - room;
    dropped_ += skip;
    if (data != nullptr)
      data = static_cast<const uint8_t*>(data) + static_cast<size_t>(skip) * deviceChannels * sampleFormatBytes(fmt);
    if (qpc100ns != 0) qpc100ns += static_cast<uint64_t>(skip * hnsPerFrame_);
    frames = room;
  }
  if (fill() + frames > room) drop(fill() + frames - room);
  if (qpc100ns != 0) {
    qpc_ = qpc100ns;
    qpcFrame_ = w_;
  }
  const uint32_t n = std::min(channels_, deviceChannels);
  const uint32_t at = static_cast<uint32_t>(w_ & mask_);
  const uint32_t first = std::min(frames, cap - at);  // up to the wrap, then from 0
  if (data == nullptr) {
    for (uint32_t c = 0; c < channels_; ++c) {
      std::memset(ch(c) + at, 0, sizeof(float) * first);
      std::memset(ch(c), 0, sizeof(float) * (frames - first));
    }
  } else {
    const uint32_t bytes = sampleFormatBytes(fmt);
    const size_t stride = static_cast<size_t>(deviceChannels) * bytes;
    const uint8_t* src = static_cast<const uint8_t*>(data);
    for (uint32_t c = 0; c < n; ++c) {
      float* dst = ch(c);
      const uint8_t* p = src + static_cast<size_t>(c) * bytes;
      if (fmt == SampleFormat::Float32 && deviceChannels == 1) {  // contiguous: two copies
        std::memcpy(dst + at, p, sizeof(float) * first);
        std::memcpy(dst, p + sizeof(float) * first, sizeof(float) * (frames - first));
        continue;
      }
      for (uint32_t f = 0; f < first; ++f, p += stride) dst[at + f] = sampleAt(p, fmt);
      for (uint32_t f = first; f < frames; ++f, p += stride) dst[f - first] = sampleAt(p, fmt);
    }
    for (uint32_t c = n; c < channels_; ++c) {  // the device has fewer channels: silence
      std::memset(ch(c) + at, 0, sizeof(float) * first);
      std::memset(ch(c), 0, sizeof(float) * (frames - first));
    }
  }
  w_ += frames;
}

uint64_t CaptureFifo::pull(float* const* out, uint32_t frames, uint64_t playQpc100ns) noexcept {  // RT-safe
  auto silence = [&] {
    for (uint32_t c = 0; c < channels_; ++c) std::memset(out[c], 0, sizeof(float) * frames);
  };
  if (!primed_) {
    if (fill() < std::max(target_, frames)) {
      silence();
      return 0;
    }
    primed_ = true;
    haveSetpoint_ = false;  // a new delay
    setSum_ = setFrames_ = 0.0;
  }
  // What this pull reads ahead of r_: `frames` for a copy; for the filter, up to its right half past the last position.
  uint64_t need = frames;
  if (resampling_ && frames > 0) {
    const dsp::SincReach reach = dsp::sincReach(*sinc_, static_cast<int64_t>(frac_), step_, frames);
    need = static_cast<uint64_t>(std::max<int64_t>(reach.last, 0));
  }
  if (fill() < need) {  // the capture clock fell behind: an underflow, then priming again
    ++underflows_;
    drop(fill());
    frac_ = 0;
    primed_ = false;
    silence();
    return 0;
  }
  uint64_t qpc = 0;
  if (qpc_ != 0) {
    const double at = static_cast<double>(r_) + static_cast<double>(frac_) * (1.0 / 4294967296.0);
    const double t = static_cast<double>(qpc_) + (at - static_cast<double>(qpcFrame_)) * hnsPerFrame_;
    qpc = t > 0.0 ? static_cast<uint64_t>(t + 0.5) : 0;
  }
  if (resampling_) {
    resampleInto(out, frames);
  } else {
    const uint32_t cap = mask_ + 1;
    const uint32_t at = static_cast<uint32_t>(r_ & mask_);
    const uint32_t first = std::min(frames, cap - at);
    for (uint32_t c = 0; c < channels_; ++c) {
      std::memcpy(out[c], ch(c) + at, sizeof(float) * first);
      std::memcpy(out[c] + first, ch(c), sizeof(float) * (frames - first));
    }
    r_ += frames;
  }
  if (playQpc100ns != 0 && qpc != 0 && frames > 0) track(playQpc100ns, qpc, frames);
  if (fill() > high_) drop(fill() - target_);  // the capture clock ran ahead (or a burst): back to the target delay
  return qpc;
}

// T6.x-fix-ui-b: `frames` outputs at r_ + frac_ + i·ratio through the sinc, in chunks whose input window is copied out
// of the ring first (the ring wraps; the filter wants contiguous frames). RT-safe.
void CaptureFifo::resampleInto(float* const* out, uint32_t frames) noexcept {
  const dsp::SincTable& t = *sinc_;
  const uint32_t cap = mask_ + 1;
  for (uint32_t done = 0; done < frames;) {
    const uint32_t n = std::min(kChunk, frames - done);
    const int64_t pos = static_cast<int64_t>(frac_);
    const dsp::SincReach reach = dsp::sincReach(t, pos, step_, n);  // relative to r_; first < 0 (the history)
    const uint32_t len = static_cast<uint32_t>(reach.last - reach.first);
    const uint32_t at = static_cast<uint32_t>((r_ + static_cast<uint64_t>(reach.first)) & mask_);
    const uint32_t first = std::min(len, cap - at);
    for (uint32_t c = 0; c < channels_; ++c) {
      float* w = window_.data() + static_cast<size_t>(c) * kWindow;
      std::memcpy(w, ch(c) + at, sizeof(float) * first);
      std::memcpy(w + first, ch(c), sizeof(float) * (len - first));
    }
    const int64_t wpos = pos - reach.first * dsp::kFixedOne;
    for (uint32_t c = 0; c < channels_; c += 2) {
      const uint32_t pair = std::min(2u, channels_ - c);
      const float* in[2] = {window_.data() + static_cast<size_t>(c) * kWindow,
                            window_.data() + static_cast<size_t>(c + pair - 1) * kWindow};
      float* o[2] = {out[c] + done, pair == 2 ? out[c + 1] + done : spare_.data()};
      std::memset(o[0], 0, sizeof(float) * n);
      std::memset(o[1], 0, sizeof(float) * n);
      dsp::sincResample(t, in, pair, wpos, step_, n, nullptr, 1.0f, o);
    }
    const int64_t total = static_cast<int64_t>(frac_) + static_cast<int64_t>(n) * step_;
    r_ += static_cast<uint64_t>(total >> 32);
    frac_ = static_cast<uint32_t>(total & 0xffffffff);
    done += n;
  }
}

// T6.x-fix-ui-b: one delay measurement (the period just taken: its first frame's play and capture times) into the
// setpoint, the filtered error, the engage decision and the PI loop (see the header). RT-safe.
void CaptureFifo::track(uint64_t playQpc, uint64_t capQpc, uint32_t frames) noexcept {
  const double d = (static_cast<double>(playQpc) - static_cast<double>(capQpc)) / hnsPerFrame_;
  const double n = frames;
  if (!haveSetpoint_) {
    setSum_ += d * n;
    setFrames_ += n;
    if (setFrames_ >= setpointFrames_) {
      setpoint_ = setSum_ / setFrames_;
      haveSetpoint_ = true;
      err_ = 0.0;
      sinceSetpoint_ = 0.0;
    }
    return;
  }
  sinceSetpoint_ += n;
  err_ += std::min(1.0, n / tauFilter_) * ((d - setpoint_) - err_);
  if (!resampling_) {
    if (std::fabs(err_) <= engage_) return;
    // Drift: start at the slope e has had since the setpoint (the filter lags it by about its time constant).
    resampling_ = true;
    frac_ = 0;
    integ_ = std::clamp(err_ / std::max(sinceSetpoint_ - tauFilter_, tauFilter_), -kMaxDeviation, kMaxDeviation);
  } else {
    integ_ = std::clamp(integ_ + ki_ * err_ * n, -kMaxDeviation, kMaxDeviation);
  }
  ratio_ = std::clamp(1.0 + kp_ * err_ + integ_, 1.0 - kMaxDeviation, 1.0 + kMaxDeviation);
  step_ = std::llround(ratio_ * 4294967296.0);
}

}  // namespace pq::engine
