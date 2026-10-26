#pragma once

// Analysis tap (SPEC §11.1, T4.6): the audio thread copies a signal into a ring; one analysis thread reads windows of
// it. Visualisers (spectrum, scope, meters) never touch the audio thread otherwise.
//
// - Lossy by design: the writer never waits and never fails. It overwrites the oldest frames; the reader checks after
//   copying whether any frame it read may have been overwritten meanwhile (a seqlock on the free-running frame count)
//   and drops those. The ring holds `capacity` frames per channel, planar.
// - Interest: readers announce themselves (addReader / removeReader). With no reader the writer returns after one
//   relaxed load, so a hidden visualiser costs nothing on the audio side (T4.6 acceptance).
// - Exactly one writer thread and one reader thread at a time.
// - T4.5 generalises probing (taps on any wire); this is the minimal form the device panels need.

#include <algorithm>
#include <atomic>
#include <bit>
#include <cassert>
#include <cstdint>
#include <cstring>

#include "pq/core/aligned_buffer.h"
#include "pq/core/cache_line.h"

namespace pq {

class TapRing {
public:
  // Allocates: construct off the audio thread. capacityPow2 ≥ 2·maxWriteFrames, a power of two.
  TapRing(uint32_t channels, uint32_t capacityPow2, uint32_t maxWriteFrames = 1024)
      : channels_(channels), capacity_(capacityPow2), mask_(capacityPow2 - 1), maxWrite_(maxWriteFrames),
        buf_(static_cast<size_t>(channels) * capacityPow2) {
    assert(channels >= 1 && std::has_single_bit(capacityPow2) && capacityPow2 >= 2 * maxWriteFrames && maxWriteFrames > 0);
    std::memset(buf_.data(), 0, buf_.size() * sizeof(float));
  }
  TapRing(const TapRing&) = delete;
  TapRing& operator=(const TapRing&) = delete;

  uint32_t channels() const noexcept { return channels_; }
  uint32_t capacity() const noexcept { return capacity_; }
  uint32_t maxWriteFrames() const noexcept { return maxWrite_; }

  // ---- interest (any thread) ----
  void addReader() noexcept { readers_.fetch_add(1, std::memory_order_relaxed); }
  void removeReader() noexcept { readers_.fetch_sub(1, std::memory_order_relaxed); }
  bool wanted() const noexcept { return readers_.load(std::memory_order_relaxed) > 0; }

  // ---- writer (audio thread) ----
  // Copies `frames` frames of `numCh` planar channels (tap channels beyond numCh repeat the last one). Nothing when no
  // reader is interested. RT-safe: no allocation, no locks, bounded (frames · channels copies).
  void write(const float* const* ch, uint32_t numCh, uint32_t frames) noexcept {
    if (!wanted() || numCh == 0) return;
    uint32_t done = 0;
    uint64_t w = w_.written.load(std::memory_order_relaxed);
    while (done < frames) {
      const uint32_t n = std::min(frames - done, maxWrite_);
      const uint32_t pos = static_cast<uint32_t>(w) & mask_;
      const uint32_t first = std::min(n, capacity_ - pos);
      for (uint32_t c = 0; c < channels_; ++c) {
        const float* src = ch[std::min(c, numCh - 1)] + done;
        float* dst = buf_.data() + static_cast<size_t>(c) * capacity_;
        std::memcpy(dst + pos, src, first * sizeof(float));
        if (first < n) std::memcpy(dst, src + first, (n - first) * sizeof(float));
      }
      w += n;
      w_.written.store(w, std::memory_order_seq_cst);  // publish after the data (the reader's seqlock check)
      done += n;
    }
  }

  // ---- reader (one analysis thread) ----
  // Frames written so far (free-running count).
  uint64_t written() const noexcept { return w_.written.load(std::memory_order_seq_cst); }
  // The oldest frame that is still safe to read now.
  uint64_t oldestReadable() const noexcept {
    const uint64_t w = written();
    const uint64_t keep = capacity_ - maxWrite_;  // a write in progress may be overwriting the frames before that
    return w > keep ? w - keep : 0;
  }
  // Copies frames [from, from + n) of channel c into out. Returns how many leading frames are valid: the copy is
  // checked against the count after it: 0 when a concurrent write may have overwritten any of them (the reader fell
  // behind by more than capacity − maxWriteFrames), else the frames written so far (≤ n). Allocation-free.
  uint32_t read(uint32_t c, uint64_t from, uint32_t n, float* out) const noexcept {
    const uint64_t w0 = written();
    if (from >= w0) return 0;
    n = static_cast<uint32_t>(std::min<uint64_t>(n, w0 - from));
    const float* src = buf_.data() + static_cast<size_t>(std::min(c, channels_ - 1)) * capacity_;
    const uint32_t pos = static_cast<uint32_t>(from) & mask_;
    const uint32_t first = std::min(n, capacity_ - pos);
    std::memcpy(out, src + pos, first * sizeof(float));
    if (first < n) std::memcpy(out + first, src, (n - first) * sizeof(float));
    std::atomic_thread_fence(std::memory_order_seq_cst);
    const uint64_t w1 = written();
    // A write after w1 (in progress) covers frames [w1, w1 + maxWrite) = ring slots of frames [w1 + maxWrite − cap, …).
    const uint64_t safeFrom = w1 + maxWrite_ > capacity_ ? w1 + maxWrite_ - capacity_ : 0;
    // Anything older may be torn: the caller resynchronises from oldestReadable().
    return from >= safeFrom ? n : 0;
  }

private:
  const uint32_t channels_, capacity_, mask_, maxWrite_;
  AlignedBuffer<float> buf_;
  struct alignas(kHotLineSeparation) Writer {
    std::atomic<uint64_t> written{0};
  } w_;
  alignas(kHotLineSeparation) std::atomic<int32_t> readers_{0};
};

}  // namespace pq
