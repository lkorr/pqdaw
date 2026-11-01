#pragma once

// Voice taps (TMM.2; docs/plans/MM.md Decisions → DSP, "Per-voice dots without probes"): what a meta module's face
// needs to draw one dot per sounding voice — no probe, no recompile. The audio thread samples a per-voice signal (the
// phase a wavetable read, an envelope's stage position) into lossy per-voice-group records; a UI-side reader keeps
// the newest record per voice slot and a short history of the sampled values.
//
// ---- Writer (audio thread) -----------------------------------------------------------------------------------
// Per voice group (8 lanes; a global signal is group 0, lane 0) a ring of `depth` records. A record holds, per lane,
// kVoiceTapSamples values of the signal taken every `stride` frames of the group's own running frame count (so it
// does not depend on how a block is split into kernel calls: a per-sample feedback loop calls with 1 frame), plus the
// position and increment the writer reported with the sample that completed the record, the group's lane mask (which
// lanes hold a voice: set by the engine's image host each block, setLaneMask) and a serial number. With stride =
// blockFrames / 8 a record is one block.
//   - Interest: with no reader registered, write() returns after one relaxed load (wanted()), so an unwatched module
//     costs nothing on the audio side. Readers count themselves in (addReader / removeReader).
//   - Lossy: the writer never waits; it overwrites the oldest record. A reader checks after copying whether the
//     record may have been overwritten meanwhile (a seqlock on the group's free-running record count) and drops it.
//   - One writer thread per group at a time (the audio thread). Two images running one module in the same block (a
//     hot-swap crossfade) interleave their samples into the same records for that time: the display is lossy, the
//     samples are still values the module read.
//
// ---- Reader (UI thread) ----------------------------------------------------------------------------------------
// VoiceTapReader registers itself, polls the records since its last poll and keeps, per voice slot (group · 8 +
// lane): whether the slot holds a voice (its group's newest record has the lane in its mask and arrived within
// `staleAfter` seconds), the newest sampled value, position and increment, and the last `history` sampled values
// (the face's glow density: ~100 per voice per 60 Hz frame at 48 kHz). Any number of readers may read one tap.
//
// Memory per tap: groups × depth × sizeof(VoiceTapRecord) (8 × 64 × 352 B ≈ 176 KiB for 64 voices).

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

#include "pq/core/aligned_buffer.h"
#include "pq/core/cache_line.h"

namespace pq {

inline constexpr uint32_t kVoiceTapSamples = 8;  // samples per lane per record
inline constexpr uint32_t kVoiceTapLanes = 8;    // lanes per voice group

struct VoiceTapRecord {
  uint64_t serial = 0;    // 1-based number of this record within its group
  uint32_t group = 0;     // voice slots group·8 … group·8 + 7
  uint32_t laneMask = 0;  // bit v: lane v held a voice (a global signal: bit 0)
  uint32_t lanes = 0;     // 1 (a global signal: lane 0 only) or 8
  uint32_t stride = 0;    // frames between two samples
  float sample[kVoiceTapSamples][kVoiceTapLanes];  // [k][lane], oldest first
  float position[kVoiceTapLanes];   // as reported with the last sample (wavetable_pos: the frame position)
  float increment[kVoiceTapLanes];  // as reported with the last sample (wavetable_pos: |Δphase|, the level estimate)
};

struct VoiceTapConfig {
  uint32_t groups = 8;   // voice groups (≥ 1): voice slots = groups · 8
  uint32_t depth = 64;   // records per group, a power of two ≥ 2
  uint32_t stride = 8;   // frames between samples (≥ 1): blockFrames / 8 makes one record per block
};

class VoiceTap {
public:
  // Allocates: construct off the audio thread.
  explicit VoiceTap(const VoiceTapConfig& config);
  VoiceTap(const VoiceTap&) = delete;
  VoiceTap& operator=(const VoiceTap&) = delete;

  uint32_t groups() const noexcept { return groups_; }
  uint32_t depth() const noexcept { return depth_; }
  uint32_t stride() const noexcept { return stride_; }

  // ---- interest (any thread) ----
  void addReader() noexcept { readers_.fetch_add(1, std::memory_order_relaxed); }
  void removeReader() noexcept { readers_.fetch_sub(1, std::memory_order_relaxed); }
  bool wanted() const noexcept { return readers_.load(std::memory_order_relaxed) > 0; }  // RT-safe

  // ---- writer (audio thread) ----
  // The occupied lanes of voice group `group` for the coming block (the image host, before its stages). RT-safe.
  void setLaneMask(uint32_t group, uint32_t mask) noexcept {
    if (group < groups_) w_.laneMask[group].store(mask & 0xFFu, std::memory_order_relaxed);
  }
  // Samples `frames` frames of x for voice group `group`: lanes 1 (x[f·xStride], lane 0) or 8 (x[f·xStride + lane];
  // xStride 0 = an 8-float pattern constant over the call). wrap: store frac(x) (a phase). position / increment: 8
  // floats each (lane order; null = 0), the values a record completed in this call reports. Nothing when no reader
  // is registered or group ≥ groups(). RT-safe: no allocation, no locks, bounded (frames / stride samples).
  void write(uint32_t group, uint32_t lanes, const float* x, uint32_t xStride, uint32_t frames, bool wrap,
             const float* position, const float* increment) noexcept;

  // ---- reader (any thread; allocation-free) ----
  // Records written so far to `group` (free-running).
  uint64_t written(uint32_t group) const noexcept { return groupHead(group).written.load(std::memory_order_seq_cst); }
  // Copies record `index` (0-based, < written) of `group`. False when it is not written yet or may have been
  // overwritten (the reader fell more than depth − 1 records behind).
  bool read(uint32_t group, uint64_t index, VoiceTapRecord& out) const noexcept;

private:
  struct alignas(kCacheLineSize) GroupHead {
    std::atomic<uint64_t> written{0};
  };
  // Writer-side accumulation of the record in progress (audio thread only).
  struct Pending {
    uint32_t count = 0;   // samples taken into `sample`
    uint32_t cursor = 0;  // frames until the next sample
    float sample[kVoiceTapSamples][kVoiceTapLanes];
  };
  const GroupHead& groupHead(uint32_t g) const noexcept { return heads_[g]; }
  void publish(uint32_t group, uint32_t lanes, const float* position, const float* increment) noexcept;

  const uint32_t groups_, depth_, mask_, stride_;
  std::unique_ptr<GroupHead[]> heads_;
  AlignedBuffer<VoiceTapRecord> ring_;  // [group · depth + slot]
  struct Writer {
    std::unique_ptr<std::atomic<uint32_t>[]> laneMask;
    std::unique_ptr<Pending[]> pending;
  } w_;
  alignas(kHotLineSeparation) std::atomic<int32_t> readers_{0};
};

// The UI side: registers on construction, unregisters on destruction. Not RT (one UI thread polls it).
class VoiceTapReader {
public:
  struct Voice {
    bool active = false;     // holds a voice now (see the file comment)
    float value = 0;         // newest sampled value (wavetable_pos: the phase read, in [0, 1))
    float position = 0;
    float increment = 0;
    uint64_t serial = 0;     // of the record it came from (0: never seen)
  };
  // history: sampled values kept per voice slot (≥ 8). staleAfter: seconds after which a group without new
  // records counts as silent (its voices inactive).
  explicit VoiceTapReader(std::shared_ptr<VoiceTap> tap, uint32_t history = 128, double staleAfter = 0.1);
  ~VoiceTapReader();
  VoiceTapReader(const VoiceTapReader&) = delete;
  VoiceTapReader& operator=(const VoiceTapReader&) = delete;

  // Reads every record written since the last poll (now: a monotonic time in seconds). Returns the records read.
  uint32_t poll(double now);

  uint32_t voiceSlots() const noexcept { return static_cast<uint32_t>(voices_.size()); }
  const Voice& voice(uint32_t slot) const noexcept { return voices_[slot]; }
  // The last sampled values of a voice slot, oldest first, at most out.size(); returns how many were written.
  uint32_t history(uint32_t slot, std::span<float> out) const noexcept;
  // Records read / records the reader missed (overwritten before it polled).
  uint64_t received() const noexcept { return received_; }
  uint64_t lost() const noexcept { return lost_; }
  const VoiceTap& tap() const noexcept { return *tap_; }

private:
  void take(const VoiceTapRecord& r, double now);
  std::shared_ptr<VoiceTap> tap_;
  uint32_t history_;
  double staleAfter_;
  std::vector<Voice> voices_;
  std::vector<float> hist_;         // [slot · history_ + i], a ring per slot
  std::vector<uint32_t> histHead_;  // per slot: next write position
  std::vector<uint32_t> histCount_; // per slot: values held (≤ history_)
  std::vector<uint64_t> next_;      // per group: next record index to read
  std::vector<double> seen_;        // per group: time of the newest record (or −inf)
  std::vector<uint32_t> mask_;      // per group: the newest record's lane mask
  uint64_t received_ = 0, lost_ = 0;
};

}  // namespace pq
