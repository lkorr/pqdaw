#pragma once

// Inspection of running images (T4.5, SPEC §7.3, §7.6, §11.1): live probes and cost meters, the engine's half.
// pq/graph/inspect.h is the compiler's half (which value a probe reads; Schedule::probes).
//
// ---- Probes -------------------------------------------------------------------------------------------------------
// A schedule compiled with probes (ScheduleOptions::probes) keeps each probed value readable; its image copies the
// value after the stage that computes it — a global value after the last stage, a voice value after each voice group —
// into the probe's TapRing (the full signal: a mini-scope) and/or ProbeLevel (last / min / max of the block: a value
// readout, a bool wire's LED, a knob's live range). The owner binds those per probe id before publishing the image
// (ProgramImage::bindInspect). Cost:
//   - no probe compiled (the normal case): nothing — the image is the one a schedule without probes builds, and the
//     host's probe step is one test of an empty list per block (measured: within noise, bench/RESULTS.md T4.5);
//   - compiled, nobody reading (the ring's reader count 0, no level bound): one relaxed load per probe per block;
//   - read: a copy of the block (a strided one for a voice lane) plus, for a level, one pass over it.
// No per-sample branch is added to any op: the copy happens between stages, outside the generated code.
// Per-voice values: the newest voice (VoiceTable's voice_last lane; 0 while no voice sounds), or the sum over the
// sounding voices (bools: any voice high) with ProbePoint::voiceSum. Sleeping regions (T1.x-silence) read 0, which is
// what they output. Bools reach a TapRing as the program's lane masks (sign bit set = high) and a ProbeLevel as 0 / 1.
//
// ---- Cost meters --------------------------------------------------------------------------------------------------
// While a CostMeter bound to an image is enabled, the image reads the TSC (rdtsc) around every region it runs (both
// backends: the interpreter around its region loops, the JIT's generated code around each region or fused chain of
// regions, behind one flag word loaded per stage function) and around the whole block, and adds the cycles to the
// meter after the block. Disabled (no meter shown): one relaxed load per block and one predictable branch per region
// per block (measured within noise). A reader turns the cycles into ns per frame (CostMeter::read). Regions are the
// finest unit both backends can time without changing the code they run (a fused JIT chain times as one unit:
// CostMeter::Reading::units); pq/graph/inspect.h's opCostWeight splits a region among its ops.
//
// Threads: binding and CostMeter::attach / read off the audio thread; the copies, ProbeLevel writes and CostMeter::add
// on the audio thread (RT-safe: no allocation, no locks, bounded).

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

#include "pq/core/cache_line.h"
#include "pq/core/probe_level.h"
#include "pq/core/tap_ring.h"

namespace pq::engine {

class CostMeter {
public:
  explicit CostMeter(uint32_t maxRegions = 4096);  // allocates: off the audio thread
  CostMeter(const CostMeter&) = delete;
  CostMeter& operator=(const CostMeter&) = delete;

  // Any thread. Disabled: images bound to the meter measure nothing.
  void setEnabled(bool on) noexcept { enabled_.store(on, std::memory_order_relaxed); }
  bool enabled() const noexcept { return enabled_.load(std::memory_order_relaxed); }
  uint32_t maxRegions() const noexcept { return maxRegions_; }

  // Off the audio thread (ProgramImage::bindInspect calls it): registers an image's timing units — per schedule region,
  // the first region of the unit its time is counted in (itself, or a fused chain's first region) — and makes that
  // image the metered one (another image bound earlier stops adding). Returns the image's key.
  uint32_t attach(std::span<const uint32_t> units);
  // Audio thread: is `key` the metered image, and is the meter on? RT-safe.
  bool wants(uint32_t key) const noexcept {
    return enabled_.load(std::memory_order_relaxed) && active_.load(std::memory_order_relaxed) == key;
  }
  // Audio thread: one block's cycles per region (n ≤ maxRegions counted), the whole block's, and its frames. RT-safe.
  void add(const uint64_t* regionCycles, uint32_t n, uint64_t totalCycles, uint32_t frames) noexcept;

  struct Reading {
    uint32_t key = 0;                 // the metered image (a hot swap or the JIT's image starts a new key)
    uint64_t frames = 0;              // frames measured since the previous reading
    double totalNsPerFrame = 0.0;     // the whole image (stages, the host's own work around them)
    std::vector<double> regionNsPerFrame;  // per schedule region (0 for a region that is part of a unit, see units)
    std::vector<uint32_t> units;      // per schedule region: the first region of its timing unit
  };
  // Reader (one thread): the cost since the previous read of the same key. False until two reads of one key have seen
  // at least one block between them (the first read after a new key only takes the baseline).
  bool read(Reading& out);

private:
  const uint32_t maxRegions_;
  std::atomic<bool> enabled_{false};
  std::atomic<uint32_t> active_{0};
  uint32_t nextKey_ = 1;
  // Written by the audio thread only (load + store), read by the reader.
  std::unique_ptr<std::atomic<uint64_t>[]> cycles_;
  alignas(kCacheLineSize) std::atomic<uint64_t> total_{0};
  std::atomic<uint64_t> frames_{0};
  // attach / read
  std::mutex mutex_;
  std::vector<uint32_t> units_;        // of the active key
  uint32_t unitsKey_ = 0;
  // the reader's baseline
  uint32_t baseKey_ = 0;
  uint64_t baseTotal_ = 0, baseFrames_ = 0;
  std::vector<uint64_t> baseCycles_;
};

// One probe's destinations (by Schedule::probes id). Either may be null.
struct ProbeTap {
  uint32_t id = 0;
  TapRing* ring = nullptr;    // mono; written `frames` at a time (≤ the image's maxBlockFrames)
  ProbeLevel* level = nullptr;
};

struct InspectBinding {
  std::span<const ProbeTap> probes;  // probes of the schedule without an entry are not copied
  CostMeter* meter = nullptr;        // null: not metered
};

}  // namespace pq::engine
