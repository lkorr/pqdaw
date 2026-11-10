// T6.7b: pq/engine/capture_fifo.h describes it.
#include "pq/engine/capture_fifo.h"

#include <algorithm>
#include <cstring>

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
  reset();
}

void CaptureFifo::reset() noexcept {  // RT-safe
  w_ = r_ = 0;
  primed_ = false;
  qpc_ = qpcFrame_ = 0;
}

void CaptureFifo::push(const void* data, SampleFormat fmt, uint32_t deviceChannels, uint32_t frames,
                       uint64_t qpc100ns) noexcept {  // RT-safe
  if (channels_ == 0 || frames == 0) return;
  const uint32_t cap = mask_ + 1;
  if (frames > cap) {  // more than the whole ring: only its last `cap` frames can stay
    const uint32_t skip = frames - cap;
    dropped_ += skip;
    if (data != nullptr)
      data = static_cast<const uint8_t*>(data) + static_cast<size_t>(skip) * deviceChannels * sampleFormatBytes(fmt);
    if (qpc100ns != 0) qpc100ns += static_cast<uint64_t>(skip * hnsPerFrame_);
    frames = cap;
  }
  if (fill() + frames > cap) drop(fill() + frames - cap);
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

uint64_t CaptureFifo::pull(float* const* out, uint32_t frames) noexcept {  // RT-safe
  auto silence = [&] {
    for (uint32_t c = 0; c < channels_; ++c) std::memset(out[c], 0, sizeof(float) * frames);
  };
  if (!primed_) {
    if (fill() < std::max(target_, frames)) {
      silence();
      return 0;
    }
    primed_ = true;
  }
  if (fill() < frames) {  // the capture clock fell behind: an underflow, then priming again
    ++underflows_;
    drop(fill());
    primed_ = false;
    silence();
    return 0;
  }
  const uint32_t cap = mask_ + 1;
  const uint32_t at = static_cast<uint32_t>(r_ & mask_);
  const uint32_t first = std::min(frames, cap - at);
  for (uint32_t c = 0; c < channels_; ++c) {
    std::memcpy(out[c], ch(c) + at, sizeof(float) * first);
    std::memcpy(out[c] + first, ch(c), sizeof(float) * (frames - first));
  }
  uint64_t qpc = 0;
  if (qpc_ != 0) {
    const double d = (static_cast<double>(r_) - static_cast<double>(qpcFrame_)) * hnsPerFrame_;
    const double t = static_cast<double>(qpc_) + d;
    qpc = t > 0.0 ? static_cast<uint64_t>(t + 0.5) : 0;
  }
  r_ += frames;
  if (fill() > high_) drop(fill() - target_);  // the capture clock ran ahead (or a burst): back to the target delay
  return qpc;
}

}  // namespace pq::engine
