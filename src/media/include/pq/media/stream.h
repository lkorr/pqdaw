#pragma once

// RT-safe disk streaming, the audio-thread side (T5.2). Header-only: the engine calls it from the audio thread.
//
// A StreamPool is a fixed set of stream slots allocated once (SamplePool owns one). Each slot is a ring of planar
// float frames that the disk thread (sample_pool.cpp) fills from a sample's file and one consumer reads in place:
//   claim(source, frame, direction, channels, frames)  pops a free slot (lock-free, never blocks)
//   span(id, n)                                        the next n frames as pointers into the ring (two pieces at the
//                                                      wrap); frames the disk thread has not delivered yet read as
//                                                      silence and count one underrun
//   release(id)                                        returns the slot
// The consumer never signals a kernel object (a syscall): the disk thread polls the slots every 2 ms while it is
// engaged (playback, prefetch, renders) and blocks otherwise (SamplePool::engage / wake).
//
// Ordering: per span one acquire load of the slot's fill word and one release store of the read position; no
// per-sample atomics, no copies (the caller reads the ring in place). Each slot word carries the claim's generation,
// so a fill meant for an earlier claim of the slot is never read.
//
// SampleCursor (below) is what a player uses: it reads a pooled sample's resident frames (the whole file, or the
// head of a streamed one) from memory and the rest through a stream, in either direction.

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <thread>

#include "pq/core/cache_line.h"

namespace pq::media {

inline constexpr uint32_t kMaxStreamChannels = 8;
inline constexpr uint32_t kMaxSpanFrames = 8192;     // frames per span() / next() call
inline constexpr int32_t kSampleGuardFrames = 64;    // zero frames before and after resident data (interpolators)
inline constexpr uint32_t kMaxPrefetches = 64;

enum class Direction : uint8_t { Forward, Reverse };

// What the audio thread reads of a pooled sample: the resident frames (all of them, or the head of a streamed
// file) and, for a streamed file, its stream source id. Plain data inside an immutable residency (sample_pool.h).
struct SampleView {
  const float* data[kMaxStreamChannels]{};  // frame 0 per channel; [−kSampleGuardFrames, residentFrames + guard) readable
  int64_t frames = 0;                       // the file's length
  int64_t residentFrames = 0;               // == frames when fully resident
  uint32_t channels = 0;
  uint32_t sampleRate = 0;
  uint32_t streamSource = 0;                // 0 = none (fully resident)
  bool streamed() const noexcept { return residentFrames < frames; }
};

// Frames in file order as up to kMaxPieces contiguous runs; a run of silence points at shared zeros.
struct FramePiece {
  const float* ch[kMaxStreamChannels];
  uint32_t frames;
};
struct FrameSpan {
  static constexpr uint32_t kMaxPieces = 6;
  FramePiece piece[kMaxPieces];
  uint32_t pieces = 0;
  uint32_t frames = 0;   // total = the frames asked for
  uint32_t missing = 0;  // frames that read as silence because the disk thread was late (underrun)
};

struct StreamId {
  uint32_t slot = UINT32_MAX;
  uint32_t gen = 0;
  bool valid() const noexcept { return slot != UINT32_MAX; }
};

namespace detail {
// Shared zero frames for silent pieces (kMaxSpanFrames + 2 · guard floats).
const float* zeroFrames() noexcept;
}  // namespace detail

class StreamPool {
public:
  // `slots` stream slots of `ringFloats` floats each (a stereo ring holds ringFloats / 2 frames). The ring memory is
  // reserved here and committed per slot by the disk thread on the slot's first use. Not RT-safe.
  StreamPool(uint32_t slots, uint32_t ringFloats);
  ~StreamPool();
  StreamPool(const StreamPool&) = delete;
  StreamPool& operator=(const StreamPool&) = delete;

  static constexpr uint64_t kFrameBits = 40;  // frames < 2^40 (≈ 254 days at 48 kHz)
  static constexpr uint64_t kFrameMask = (uint64_t{1} << kFrameBits) - 1;
  static constexpr uint32_t kGenMask = (1u << 24) - 1;
  static uint64_t pack(uint32_t gen, int64_t frame) noexcept {
    return (uint64_t{gen & kGenMask} << kFrameBits) | (static_cast<uint64_t>(frame) & kFrameMask);
  }
  static uint32_t genOf(uint64_t w) noexcept { return static_cast<uint32_t>(w >> kFrameBits); }
  static int64_t frameOf(uint64_t w) noexcept { return static_cast<int64_t>(w & kFrameMask); }

  // Claims a slot for `source` (a pool stream source id), reading from `frame` on: forward, the first frame read is
  // `frame`; reverse, the first read ends at `frame` (exclusive) and reads go down. `frames` = the file's length
  // (reads past it are silence, not underruns). An adoptable prefetch of the same (source, frame, direction) is taken
  // over instead of a new slot. Invalid id if every slot is taken. RT-safe.
  StreamId claim(uint32_t source, int64_t frame, Direction dir, uint32_t channels, int64_t frames) noexcept;

  // The next n ≤ kMaxSpanFrames frames of the stream, advancing it. Forward: [pos, pos + n); reverse: [pos − n, pos),
  // pieces in file order either way. RT-safe.
  FrameSpan span(StreamId id, uint32_t n) noexcept;

  // How many frames a span would deliver right now without silence (filled ahead of the read position). RT-safe.
  int64_t buffered(StreamId id) const noexcept;
  // Frames left before the stream's end (forward: the file's end; reverse: frame 0). Consumer thread only. RT-safe.
  int64_t remaining(StreamId id) const noexcept {
    const Slot& s = slots_[id.slot];
    return s.dir == Direction::Forward ? std::max<int64_t>(0, s.limit - s.readPos) : std::max<int64_t>(0, s.readPos);
  }

  void release(StreamId id) noexcept;  // RT-safe

  // Total spans that had to read silence because the disk thread was late (the status bar's disk indicator).
  uint64_t underruns() const noexcept { return underruns_.load(std::memory_order_relaxed); }

  uint32_t slotCount() const noexcept { return slotCount_; }
  uint32_t ringFloats() const noexcept { return ringFloats_; }
  uint32_t freeSlots() const noexcept;  // diagnostics (walks the free list; not RT-relevant)

  // ---- prefetch (non-RT): a stream claimed ahead of playback that a later RT claim of the same position adopts.
  bool addPrefetch(uint32_t source, int64_t frame, Direction dir, uint32_t channels, int64_t frames) noexcept;
  // Releases every prefetch not yet adopted.
  void clearPrefetches() noexcept;

  // ---- disk-thread side (sample_pool.cpp) --------------------------------------------------------------------------
  struct Request {
    uint32_t gen = 0;
    bool active = false;
    uint32_t source = 0;
    int64_t start = 0;
    Direction dir = Direction::Forward;
    uint32_t channels = 0;
    int64_t frames = 0;
  };
  // A consistent snapshot of slot i's claim (seqlock read). Any thread.
  Request request(uint32_t i) const noexcept;
  // The consumer's read position for generation `gen` (−1 if the slot's consumer is on another generation).
  int64_t consumedPosition(uint32_t i, uint32_t gen) const noexcept;
  // Publishes fill progress for generation `gen` (forward: frames below `frontier` are filled; reverse: frames at
  // and above it).
  void publishFill(uint32_t i, uint32_t gen, int64_t frontier) noexcept {
    slots_[i].filled.store(pack(gen, frontier), std::memory_order_release);
  }
  float* ring(uint32_t i) noexcept { return ringBase_ + static_cast<std::size_t>(i) * ringFloats_; }
  uint32_t ringFrames(uint32_t channels) const noexcept { return channels ? ringFloats_ / channels : 0; }

private:
  enum : uint32_t { kFree = 0, kActive = 1 };
  static constexpr uint32_t kNone = UINT32_MAX;

  struct alignas(kHotLineSeparation) Slot {
    // Claim description, written by the claimer under the seqlock `seq` (odd while being written).
    std::atomic<uint32_t> seq{0};
    std::atomic<uint32_t> state{kFree};
    std::atomic<uint32_t> source{0};
    std::atomic<uint32_t> dirChannels{0};  // channels | dir << 16
    std::atomic<int64_t> start{0};
    std::atomic<int64_t> frames{0};
    std::atomic<uint32_t> nextFree{kNone};
    // Written by the disk thread: fill frontier (gen | frame).
    alignas(kHotLineSeparation) std::atomic<uint64_t> filled{0};
    // Written by the consumer: read position (gen | frame).
    alignas(kHotLineSeparation) std::atomic<uint64_t> consumed{0};
    // Consumer-private (the thread that claimed, or adopted, the stream).
    alignas(kHotLineSeparation) int64_t readPos = 0;
    int64_t limit = 0;
    uint32_t gen = 0;
    uint32_t channels = 0;
    uint32_t capFrames = 0;
    Direction dir = Direction::Forward;
  };

  struct alignas(kCacheLineSize) Prefetch {
    std::atomic<uint32_t> state{0};  // 0 empty, 1 being written, 2 ready (adoptable), 3 being taken
    uint32_t source = 0;
    int64_t frame = 0;
    Direction dir = Direction::Forward;
    StreamId id;
  };

  uint32_t popFree() noexcept {
    uint64_t head = freeHead_.load(std::memory_order_acquire);
    for (;;) {
      const auto idx = static_cast<uint32_t>(head);
      if (idx == kNone) return kNone;
      const uint32_t next = slots_[idx].nextFree.load(std::memory_order_relaxed);
      const uint64_t want = ((head >> 32) + 1) << 32 | next;
      if (freeHead_.compare_exchange_weak(head, want, std::memory_order_acq_rel, std::memory_order_acquire)) return idx;
    }
  }
  void pushFree(uint32_t idx) noexcept {
    uint64_t head = freeHead_.load(std::memory_order_relaxed);
    for (;;) {
      slots_[idx].nextFree.store(static_cast<uint32_t>(head), std::memory_order_relaxed);
      const uint64_t want = ((head >> 32) + 1) << 32 | idx;
      if (freeHead_.compare_exchange_weak(head, want, std::memory_order_release, std::memory_order_relaxed)) return;
    }
  }
  StreamId claimSlot(uint32_t source, int64_t frame, Direction dir, uint32_t channels, int64_t frames) noexcept;

  Slot* slots_ = nullptr;
  uint32_t slotCount_ = 0;
  uint32_t ringFloats_ = 0;
  float* ringBase_ = nullptr;  // VirtualAlloc reservation: slotCount_ · ringFloats_ floats
  alignas(kHotLineSeparation) std::atomic<uint64_t> freeHead_{kNone};  // tag << 32 | index
  alignas(kHotLineSeparation) std::atomic<uint64_t> underruns_{0};
  alignas(kHotLineSeparation) std::atomic<uint32_t> prefetchCount_{0};
  Prefetch prefetch_[kMaxPrefetches];
};

// ---- StreamPool inline (RT) --------------------------------------------------------------------------------------

inline StreamId StreamPool::claimSlot(uint32_t source, int64_t frame, Direction dir, uint32_t channels,
                                      int64_t frames) noexcept {  // RT-safe
  const uint32_t idx = popFree();
  if (idx == kNone) return {};
  Slot& s = slots_[idx];
  const uint32_t q = s.seq.load(std::memory_order_relaxed) + 1;  // odd: being written
  s.seq.store(q, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  s.source.store(source, std::memory_order_relaxed);
  s.dirChannels.store(channels | (dir == Direction::Reverse ? 1u << 16 : 0u), std::memory_order_relaxed);
  s.start.store(frame, std::memory_order_relaxed);
  s.frames.store(frames, std::memory_order_relaxed);
  s.state.store(kActive, std::memory_order_relaxed);
  const uint32_t gen = ((q + 1) / 2) & kGenMask;
  s.consumed.store(pack(gen, frame), std::memory_order_relaxed);
  s.seq.store(q + 1, std::memory_order_release);  // even: published
  s.readPos = frame;
  s.limit = frames;
  s.gen = gen;
  s.channels = channels;
  s.capFrames = ringFloats_ / channels;
  s.dir = dir;
  return {idx, gen};
}

inline StreamId StreamPool::claim(uint32_t source, int64_t frame, Direction dir, uint32_t channels,
                                  int64_t frames) noexcept {  // RT-safe
  assert(channels >= 1 && channels <= kMaxStreamChannels);
  if (prefetchCount_.load(std::memory_order_acquire) != 0) {
    for (Prefetch& p : prefetch_) {
      if (p.state.load(std::memory_order_acquire) != 2 || p.source != source || p.frame != frame || p.dir != dir)
        continue;
      uint32_t expected = 2;
      if (!p.state.compare_exchange_strong(expected, 3, std::memory_order_acq_rel)) continue;
      const StreamId id = p.id;
      p.state.store(0, std::memory_order_release);
      prefetchCount_.fetch_sub(1, std::memory_order_acq_rel);
      return id;  // its consumer-private state was set by the prefetching claim; the CAS hands it over
    }
  }
  return claimSlot(source, frame, dir, channels, frames);
}

inline int64_t StreamPool::buffered(StreamId id) const noexcept {  // RT-safe
  const Slot& s = slots_[id.slot];
  const uint64_t f = s.filled.load(std::memory_order_acquire);
  if (genOf(f) != s.gen) return 0;
  const int64_t frontier = frameOf(f);
  return s.dir == Direction::Forward ? std::max<int64_t>(0, frontier - s.readPos)
                                     : std::max<int64_t>(0, s.readPos - frontier);
}

inline FrameSpan StreamPool::span(StreamId id, uint32_t n) noexcept {  // RT-safe
  assert(id.valid() && n <= kMaxSpanFrames);
  Slot& s = slots_[id.slot];
  FrameSpan out;
  out.frames = n;
  const uint64_t f = s.filled.load(std::memory_order_acquire);  // the one acquire load per block
  const float* ring = ringBase_ + static_cast<std::size_t>(id.slot) * ringFloats_;
  const float* zeros = detail::zeroFrames() + kSampleGuardFrames;
  const uint32_t ch = s.channels;
  const int64_t cap = s.capFrames;
  auto ringPieces = [&](int64_t first, uint32_t count) {  // frames [first, first + count) from the ring
    while (count > 0) {
      const int64_t at = first % cap;
      const auto run = static_cast<uint32_t>(std::min<int64_t>(count, cap - at));
      FramePiece& p = out.piece[out.pieces++];
      for (uint32_t c = 0; c < ch; ++c) p.ch[c] = ring + static_cast<std::size_t>(c) * cap + at;
      p.frames = run;
      first += run;
      count -= run;
    }
  };
  auto zeroPiece = [&](uint32_t count) {
    if (count == 0) return;
    FramePiece& p = out.piece[out.pieces++];
    for (uint32_t c = 0; c < ch; ++c) p.ch[c] = zeros;
    p.frames = count;
  };
  const bool mine = genOf(f) == s.gen;
  if (s.dir == Direction::Forward) {
    const int64_t frontier = mine ? frameOf(f) : s.readPos;
    const int64_t pos = s.readPos;
    const auto have = static_cast<uint32_t>(std::clamp<int64_t>(frontier - pos, 0, n));
    ringPieces(pos, have);
    const uint32_t lack = n - have;
    zeroPiece(lack);
    // Frames past the end of the file are silence, not an underrun.
    const int64_t lackInFile = std::clamp<int64_t>(s.limit - (pos + have), 0, lack);
    out.missing = static_cast<uint32_t>(lackInFile);
    s.readPos = pos + n;
  } else {
    const int64_t frontier = mine ? frameOf(f) : s.readPos;
    const int64_t pos = s.readPos;
    const auto have = static_cast<uint32_t>(std::clamp<int64_t>(pos - frontier, 0, n));
    const uint32_t lack = n - have;
    zeroPiece(lack);  // file order: the missing frames are the lower ones
    ringPieces(pos - have, have);
    const int64_t lackInFile = std::clamp<int64_t>((pos - have), 0, lack);  // frames below 0 are silence
    out.missing = static_cast<uint32_t>(lackInFile);
    s.readPos = pos - n;
  }
  // The frames this span returns stay protected until the next span: the published read position is where this
  // span started, so the disk thread never overwrites frames the caller is still reading in place.
  const int64_t done = out.frames == 0 ? s.readPos : (s.dir == Direction::Forward ? s.readPos - n : s.readPos + n);
  s.consumed.store(pack(s.gen, done < 0 ? 0 : done), std::memory_order_release);
  if (out.missing) underruns_.fetch_add(1, std::memory_order_relaxed);
  return out;
}

inline void StreamPool::release(StreamId id) noexcept {  // RT-safe
  if (!id.valid()) return;
  Slot& s = slots_[id.slot];
  const uint32_t q = s.seq.load(std::memory_order_relaxed) + 1;
  s.seq.store(q, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  s.state.store(kFree, std::memory_order_relaxed);
  s.seq.store(q + 1, std::memory_order_release);
  pushFree(id.slot);
}

inline StreamPool::Request StreamPool::request(uint32_t i) const noexcept {
  const Slot& s = slots_[i];
  for (;;) {
    const uint32_t q1 = s.seq.load(std::memory_order_acquire);
    if (q1 & 1u) {  // a claim or release is being written (a few instructions; the writer may be preempted)
      std::this_thread::yield();
      continue;
    }
    Request r;
    r.active = s.state.load(std::memory_order_relaxed) == kActive;
    r.source = s.source.load(std::memory_order_relaxed);
    const uint32_t dc = s.dirChannels.load(std::memory_order_relaxed);
    r.start = s.start.load(std::memory_order_relaxed);
    r.frames = s.frames.load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (s.seq.load(std::memory_order_relaxed) != q1) continue;
    r.gen = (q1 / 2) & kGenMask;
    r.channels = dc & 0xFFFFu;
    r.dir = (dc >> 16) ? Direction::Reverse : Direction::Forward;
    return r;
  }
}

inline int64_t StreamPool::consumedPosition(uint32_t i, uint32_t gen) const noexcept {
  const uint64_t c = slots_[i].consumed.load(std::memory_order_acquire);
  return genOf(c) == (gen & kGenMask) ? frameOf(c) : -1;
}

// ---- SampleCursor ------------------------------------------------------------------------------------------------

// A read position over one sample (one per playing clip / voice): frames inside the resident part come from memory,
// the rest from a stream claimed on start(). The view is re-read every call (one acquire load), so a sample the pool
// demotes from resident to streamed while it plays keeps working (it claims a stream then).
// `Sample` is anything with `const SampleView& view() const noexcept` (PooledSample; tests use plain views).
template <class Sample>
class SampleCursor {
public:
  // First frame of the stream a read from `frame` in `dir` needs (forward: past the resident head).
  static int64_t streamStart(const SampleView& v, int64_t frame, Direction dir) noexcept {
    return dir == Direction::Forward ? std::max(frame, v.residentFrames) : frame;
  }

  // Forward: the next frame read is `frame`. Reverse: the next read ends at `frame` (exclusive). RT-safe.
  void start(StreamPool& pool, const Sample& sample, int64_t frame, Direction dir) noexcept {
    stop();
    pool_ = &pool;
    sample_ = &sample;
    pos_ = frame;
    dir_ = dir;
    const SampleView& v = sample.view();
    if (v.streamed() && (dir == Direction::Forward ? frame < v.frames : frame > v.residentFrames)) claimFor(v);
  }

  // The next n ≤ kMaxSpanFrames frames, in file order (reverse: [pos − n, pos)). Frames outside the file are
  // silence; frames the disk thread has not delivered yet are silence and set `missing`. RT-safe.
  FrameSpan next(uint32_t n) noexcept {
    FrameSpan out;
    out.frames = n;
    const SampleView& v = sample_->view();
    if (v.streamed() && !stream_.valid() && (dir_ == Direction::Forward ? pos_ + n > v.residentFrames : pos_ > v.residentFrames))
      claimFor(v);
    const int64_t a = dir_ == Direction::Forward ? pos_ : pos_ - n;  // [a, b) in file order
    const int64_t b = a + n;
    // Pieces in order: below 0 (silence), resident, streamed, past the end (silence).
    const int64_t r0 = std::clamp<int64_t>(a, 0, v.frames), r1 = std::clamp<int64_t>(b, 0, v.frames);
    const auto below = static_cast<uint32_t>(std::clamp<int64_t>(-a, 0, n));
    add(out, v, below, nullptr, 0);
    const int64_t resEnd = std::min(r1, v.residentFrames);
    if (resEnd > r0) add(out, v, static_cast<uint32_t>(resEnd - r0), &v, r0);
    const int64_t s0 = std::max(r0, v.residentFrames);
    if (r1 > s0) {
      const auto k = static_cast<uint32_t>(r1 - s0);
      if (stream_.valid()) {
        const FrameSpan sp = pool_->span(stream_, k);
        for (uint32_t i = 0; i < sp.pieces; ++i) out.piece[out.pieces++] = sp.piece[i];
        out.missing += sp.missing;
      } else {
        add(out, v, k, nullptr, 0);  // no slot was free: silence
        out.missing += k;
      }
    }
    add(out, v, n - below - static_cast<uint32_t>(r1 - r0), nullptr, 0);
    pos_ = dir_ == Direction::Forward ? b : a;
    return out;
  }

  void stop() noexcept {
    if (pool_ && stream_.valid()) pool_->release(stream_);
    stream_ = {};
  }
  ~SampleCursor() { stop(); }

  int64_t position() const noexcept { return pos_; }
  bool streaming() const noexcept { return stream_.valid(); }
  StreamId stream() const noexcept { return stream_; }

private:
  void claimFor(const SampleView& v) noexcept {
    // The stream covers the part of the file outside the resident head, starting where this cursor will need it.
    const int64_t from = dir_ == Direction::Forward ? std::max(pos_, v.residentFrames) : std::min(pos_, v.frames);
    stream_ = pool_->claim(v.streamSource, from, dir_, v.channels, v.frames);
  }
  static void add(FrameSpan& out, const SampleView& v, uint32_t count, const SampleView* from, int64_t first) noexcept {
    if (count == 0) return;
    FramePiece& p = out.piece[out.pieces++];
    const float* zeros = detail::zeroFrames() + kSampleGuardFrames;
    for (uint32_t c = 0; c < v.channels; ++c) p.ch[c] = from ? from->data[c] + first : zeros;
    p.frames = count;
  }

  StreamPool* pool_ = nullptr;
  const Sample* sample_ = nullptr;
  int64_t pos_ = 0;
  Direction dir_ = Direction::Forward;
  StreamId stream_;
};

}  // namespace pq::media
