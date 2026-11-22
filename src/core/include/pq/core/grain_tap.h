#pragma once

// Grain taps (SM.6; docs/plans/SM.md "SM.6", "Face" grain dots): what the Sampler's face needs to draw a dot per live
// grain of its granulator — no probe, no recompile. The VoiceTap discipline (pq/core/voice_tap.h):
//   - Writer (audio thread): the `granular` kernel, once per call and voice group, writes one record of the group's
//     live grains (where each reads, 0 … 1 of the sample; its window's amplitude now; its pan; its lane) into the
//     group's ring of `depth` records. With no reader registered write() returns after one relaxed load (wanted()),
//     so an unwatched granulator costs nothing more.
//   - Lossy: the writer never waits; it overwrites the oldest record. A reader checks after copying whether the record
//     may have been overwritten meanwhile (a seqlock on the group's free-running record count) and drops it.
//   - One writer thread per group at a time (the audio thread); two images running one instance in a hot-swap
//     crossfade interleave their records for that time (the display is lossy).
// Reader (any non-RT thread): latest() copies the newest record of a group; GrainTapReader keeps the newest record
// per group and drops groups that went quiet (staleAfter).
// Memory per tap: groups × depth × sizeof(GrainTapRecord) (9 × 8 × 4.1 KiB ≈ 295 KiB for 64 voices).

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "pq/core/aligned_buffer.h"
#include "pq/core/cache_line.h"

namespace pq {

inline constexpr uint32_t kGrainTapGrains = 256;  // per record: 8 lanes × 32 grains

struct GrainTapGrain {
  float position = 0;   // where it reads now, 0 … 1 of the sample
  float amplitude = 0;  // its window's value now (0 … 1)
  float pan = 0;        // −1 … 1
  uint32_t lane = 0;    // 0 … 7 within the group
};

struct GrainTapRecord {
  uint64_t serial = 0;  // 1-based number of this record within its group
  uint32_t group = 0;
  uint32_t count = 0;   // grains in `grain`
  GrainTapGrain grain[kGrainTapGrains];
};

struct GrainTapConfig {
  uint32_t groups = 8;  // voice groups (≥ 1)
  uint32_t depth = 8;   // records per group, a power of two ≥ 2
};

class GrainTap {
public:
  // Allocates: construct off the audio thread.
  explicit GrainTap(const GrainTapConfig& config);
  GrainTap(const GrainTap&) = delete;
  GrainTap& operator=(const GrainTap&) = delete;

  uint32_t groups() const noexcept { return groups_; }
  uint32_t depth() const noexcept { return depth_; }

  // ---- interest (any thread) ----
  void addReader() noexcept { readers_.fetch_add(1, std::memory_order_relaxed); }
  void removeReader() noexcept { readers_.fetch_sub(1, std::memory_order_relaxed); }
  bool wanted() const noexcept { return readers_.load(std::memory_order_relaxed) > 0; }  // RT-safe

  // ---- writer (audio thread) ----
  // The record to fill for `group` (null when no reader is registered or group ≥ groups()); commit() publishes it with
  // `count` grains. RT-safe: no allocation, no locks.
  GrainTapRecord* begin(uint32_t group) noexcept;
  void commit(uint32_t group, uint32_t count) noexcept;

  // ---- reader (any thread; allocation-free) ----
  uint64_t written(uint32_t group) const noexcept { return heads_[group].written.load(std::memory_order_seq_cst); }
  // Copies record `index` (0-based, < written) of `group`; false when not written yet or possibly overwritten.
  bool read(uint32_t group, uint64_t index, GrainTapRecord& out) const noexcept;
  // The newest complete record of `group` (false: none, or overwritten while copied).
  bool latest(uint32_t group, GrainTapRecord& out) const noexcept;

private:
  struct alignas(kCacheLineSize) GroupHead {
    std::atomic<uint64_t> written{0};
  };
  const uint32_t groups_, depth_, mask_;
  std::unique_ptr<GroupHead[]> heads_;
  AlignedBuffer<GrainTapRecord> ring_;  // [group · depth + slot]
  alignas(kHotLineSeparation) std::atomic<int32_t> readers_{0};
};

// The UI side: registers on construction, unregisters on destruction. Not RT (one UI thread polls it).
class GrainTapReader {
public:
  explicit GrainTapReader(std::shared_ptr<GrainTap> tap, double staleAfter = 0.1);
  ~GrainTapReader();
  GrainTapReader(const GrainTapReader&) = delete;
  GrainTapReader& operator=(const GrainTapReader&) = delete;
  // Takes the newest record of every group (now: a monotonic time in seconds); returns the groups that had one.
  uint32_t poll(double now);
  // The live grains of group g as of the last poll (empty when the group went quiet).
  const GrainTapRecord* group(uint32_t g) const noexcept;
  const GrainTap& tap() const noexcept { return *tap_; }

private:
  std::shared_ptr<GrainTap> tap_;
  double staleAfter_;
  std::vector<GrainTapRecord> last_;
  std::vector<uint64_t> serial_;
  std::vector<double> seen_;
};

}  // namespace pq
