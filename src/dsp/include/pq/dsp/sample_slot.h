#pragma once

// Live sample slots (SM.2; docs/plans/SM.md "Where it sits"): the resource the `sample_read_*`, `sample_pos_*` and
// `sample_slice` atoms read (Resource::Sample). A new sample (or a new slice table) reaches the audio thread without
// a recompile: the owner (the playback link, a test) publishes it, and every voice takes it at its NEXT trigger — a
// voice that is playing keeps the entry it started on, so a swap never changes a sounding note (click-free).
//
// ---- Publication --------------------------------------------------------------------------------------------------
// The slot holds one atomic pointer to the newest immutable Entry {the sample's channel views per mip level, its
// channels, the file-rate / engine-rate scale, the slice table, a generation, the entry it replaced}: a chain, newest
// first (the FrameTableSlot pattern, frame_table_slot.h). A kernel does one acquire load per call; each lane keeps in
// its state the generation it plays (taken at its trigger) and finds that entry on the chain (find). A kernel never
// keeps an entry pointer past its call.
//   An entry superseded by a newer one stays reachable while any kernel still READS it: a kernel marks every
// non-newest entry it read in a call (Entry::touched, one relaxed store). publish() and sweep() (non-RT) retire —
// unlink, then hand to the EpochReclaimer, freed after the audio thread's next quiescent state — the entries older
// than the oldest one still in use: in use = the newest, or touched during the last kSampleKeepSeconds (wall clock),
// or superseded less than kSampleKeepSeconds ago; at most kSampleSlotMaxEntries are kept (beyond that the oldest go
// even if read: a lane whose entry is gone stops, silent, until its next trigger). Retiring an entry drops its owner
// (the link's pin of the pooled sample: pq/media/sample_pool.h SamplePool::pin), so a replaced sample is unpinned
// once no note plays it.
//
// ---- Data ---------------------------------------------------------------------------------------------------------
// Planar float32, one or two channels (a mono sample's right channel IS its left: the kernels copy it to the right
// output), each readable on [−kSampleSlotGuard, length + kSampleSlotGuard) (zero guards: the interpolators' taps).
// Level 0 is the sample; levels 1 … `levels` its 2^L× decimated copies (pq/dsp/resample.h decimate2, media's
// SampleMips): a read faster than rate 2 reads one of them (sample_read: the level per call from |rate|).
//
// ---- Threads ------------------------------------------------------------------------------------------------------
// publish() / sweep(): any non-RT thread (serialised internally); the reclaimer must have every thread that runs
// images reading this slot as a reader. acquire() / find(): RT-safe. The slot must outlive every image that reads it
// (the link's images hold it through UnitImage::keepAlive).

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

#include "pq/core/epoch_reclaimer.h"
#include "pq/core/result.h"
#include "pq/dsp/resample.h"

namespace pq::dsp {

inline constexpr uint32_t kSampleSlotLevels = kMaxMipLevels + 1;  // level 0 + the mips
inline constexpr int32_t kSampleSlotGuard = 4;                    // readable frames each side (at least)
inline constexpr uint32_t kSampleSlotMaxEntries = 16;
inline constexpr double kSampleKeepSeconds = 2.0;
// SM.2: the longest sample a slot takes (a pinned sampler sample: pq/media/sample_pool.h SamplePool::pin): 10 min.
inline constexpr double kSampleSlotMaxSeconds = 600.0;

struct SampleLevelView {
  const float* ch[2]{};  // frame 0 of the left / right channel (right == left for mono)
  int32_t length = 0;    // frames
};

// What publish() takes: views into memory `owner` keeps alive (until the entry is retired and freed).
struct SampleSource {
  std::shared_ptr<const void> owner;
  uint32_t channels = 1;  // 1 or 2 (more: the first two are read)
  uint32_t levels = 0;    // decimated copies 1 … levels (≤ kMaxMipLevels) in level[1 …]
  SampleLevelView level[kSampleSlotLevels]{};
  double fileRate = 48000.0;  // the sample's frames per second
  std::vector<float> slices;  // slice start frames (sample_slice); sanitised by publish (sorted, in [0, length))
};

class SampleSlot {
public:
  struct Entry {
    SampleLevelView level[kSampleSlotLevels]{};
    uint32_t levels = 0;
    uint32_t channels = 1;
    float rateScale = 1.0f;          // file rate / engine rate: sample_read's rate input is in file-natural speed
    double fileRate = 48000.0;       // the sample's frames per second (units 1: fades in seconds of the file)
    const float* slices = nullptr;   // sliceCount start frames, ascending, in [0, length)
    uint32_t sliceCount = 0;
    uint64_t generation = 0;         // 1 for the initial entry, +1 per publish
    std::atomic<const Entry*> prev{nullptr};
    mutable std::atomic<uint32_t> touched{0};  // kernels: 1 when read in a call (non-newest entries only)
    // publish()/sweep() only (writeMutex_):
    std::shared_ptr<const void> owner;
    std::vector<float> sliceStore;
    std::chrono::steady_clock::time_point superseded{}, lastRead{};
  };

  // Allocates. Starts with a silent entry (length 0: every read is 0, a reset never starts playback).
  SampleSlot();
  ~SampleSlot();
  SampleSlot(const SampleSlot&) = delete;
  SampleSlot& operator=(const SampleSlot&) = delete;

  // Non-RT. Makes `source` current (a null level-0 channel publishes silence), at the engine rate `engineRate`;
  // retires the entries no kernel uses any more ("Publication"). Errors: more than kSampleSlotMaxSeconds, a length
  // beyond int32, a level without data.
  Status publish(SampleSource source, double engineRate, EpochReclaimer& reclaimer);
  // Non-RT: retires what publish() would (the link calls it after its compiles and data edits).
  void sweep(EpochReclaimer& reclaimer);
  // Non-RT: the newest entry's generation; how many entries are on the chain (tests).
  uint64_t generation() const noexcept { return entry_.load(std::memory_order_acquire)->generation; }
  uint32_t entries() const;

  // RT-safe: the current entry (valid until the caller's next quiescent state).
  const Entry* acquire() const noexcept { return entry_.load(std::memory_order_acquire); }
  // RT-safe: the entry of `generation` (its low 32 bits) on the chain from `newest`, or null (gone).
  static const Entry* find(const Entry* newest, uint32_t generation) noexcept {
    const Entry* e = newest;
    for (uint32_t i = 0; e != nullptr && i <= kSampleSlotMaxEntries + 1; ++i) {
      if (static_cast<uint32_t>(e->generation) == generation) return e;
      e = e->prev.load(std::memory_order_acquire);
    }
    return nullptr;
  }

  // Tests: the keep time (default kSampleKeepSeconds).
  void setKeepSeconds(double s) noexcept { keepSeconds_ = s; }
  // Tests (use-after-free checks): how retired and final entries are freed. Default: delete.
  using EntryDeleter = void (*)(void*) noexcept;
  void setEntryDeleter(EntryDeleter d) noexcept { deleter_ = d; }

private:
  static void deleteEntry(void* p) noexcept { delete static_cast<Entry*>(p); }
  void retireOld(std::chrono::steady_clock::time_point now, EpochReclaimer& reclaimer);  // writeMutex_ held
  std::atomic<const Entry*> entry_{nullptr};
  mutable std::mutex writeMutex_;
  EntryDeleter deleter_ = &deleteEntry;
  double keepSeconds_ = kSampleKeepSeconds;
};

// Guarded planar sample memory (tests, tools, the engine's silent default): one or two channels with
// kSampleDataGuard zero frames on each side, and optionally its decimated copies (decimate2, as media's mips).
inline constexpr int32_t kSampleDataGuard = 64;
class SampleData {
public:
  // At most kSampleSlotMaxSeconds at 768 kHz and 2^31 − 2·guard − 1 frames; `right` empty (mono) or as long as
  // `left`; mipLevels ≤ kMaxMipLevels. Not RT-safe.
  static Result<std::shared_ptr<const SampleData>> build(std::span<const float> left, std::span<const float> right = {},
                                                         uint32_t mipLevels = 0);
  // A source of this data at file rate `fileRate` (the owner is `self`).
  static SampleSource source(const std::shared_ptr<const SampleData>& self, double fileRate,
                             std::vector<float> slices = {});
  uint32_t channels() const noexcept { return channels_; }
  uint32_t levels() const noexcept { return levels_; }
  const SampleLevelView& level(uint32_t l) const noexcept { return level_[l]; }

private:
  std::vector<float> store_;  // every channel of every level, guarded
  uint32_t channels_ = 1, levels_ = 0;
  SampleLevelView level_[kSampleSlotLevels]{};
};

}  // namespace pq::dsp
