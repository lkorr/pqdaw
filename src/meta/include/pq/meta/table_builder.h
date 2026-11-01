#pragma once

// Off-thread frame-table builds for meta modules (TMM.5; docs/plans/MM.md "App MetaResources", Decisions → DSP).
//
// A type that reads a frame table (meta::ResourceKind::FrameTable) turns an instance's state — its data (keyframes),
// the values of its baked params and its settings — into a TableJob through Type::frameTable: an owned
// dsp::FrameTableSpec plus an identity `key` (equal keys give identical tables). The playback link (pq/app) submits a
// job per instance whenever that key changes (a MetaData edit, a baked knob, a setting, the sample rate) and
// publishes each finished table into the instance's slot: never a compile.
//
// TableBuilder: ONE worker thread at below-normal priority.
//   - newest wins per target: a job submitted for a target replaces the one still queued for it (a knob drag builds
//     only the latest value once the worker is free);
//   - identical specs share one table: a cache by key holds every table still alive (weak references), so two
//     instances with the same keyframes, or a value dragged back, cost no build;
//   - the dsp::FrameTableBuilder's arenas are kept between builds (sized once for the largest spec seen); each build
//     allocates only the new table's storage (tables are immutable and shared by the slots that play them);
//   - `done` runs on the worker thread, in submission order per target.
// Not RT code.

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <unordered_map>
#include <vector>

#include "pq/core/result.h"
#include "pq/dsp/frame_table.h"
#include "pq/dsp/frame_table_build.h"
#include "pq/meta/data.h"

namespace pq::meta {

// What Type::frameTable reads.
struct TableSource {
  DataPtr data;                    // the instance's (or the type's default) data; may be null
  uint64_t dataHash = 0;           // dataHash(*data) (0 for null): computed once per data by the caller
  std::span<const double> params;  // per ParamSpec: the values the instance plays (static: what is baked)
  std::span<const double> settings;  // per SettingSpec, resolved
  double sampleRate = 48000.0;
};

// An owned build request. Move-only: `spec`'s spans point into `keys`, `gaps` and `data` (moving the vectors keeps
// their buffers).
struct TableJob {
  DataPtr data;                       // keeps the cycles alive
  std::vector<dsp::Keyframe> keys;    // cycles: spans into data
  std::vector<dsp::GapMode> gaps;
  dsp::FrameTableSpec spec;           // keyframes / gaps: set by finish()
  uint64_t key = 0;                   // identity: equal keys build identical tables
  TableJob() = default;
  TableJob(TableJob&&) noexcept = default;
  TableJob& operator=(TableJob&&) noexcept = default;
  TableJob(const TableJob&) = delete;
  TableJob& operator=(const TableJob&) = delete;
  void finish() noexcept {
    spec.keyframes = keys;
    spec.gaps = gaps;
  }
};

class TableBuilder {
public:
  struct Stats {
    uint64_t submitted = 0;  // jobs submitted
    uint64_t replaced = 0;   // ... replaced by a newer job for the same target before they ran
    uint64_t built = 0;      // tables built
    uint64_t shared = 0;     // jobs answered by a live table with the same key (no build)
    uint64_t errors = 0;     // builds that failed (nothing delivered)
    double lastBuildMs = 0, maxBuildMs = 0;
  };
  // target: the caller's id of what the table is for (the link: its slot id); key: the job's key.
  using Done = std::function<void(uint32_t target, uint64_t key, std::shared_ptr<const dsp::FrameTable> table)>;

  explicit TableBuilder(Done done, bool lowPriority = true);
  ~TableBuilder();  // stops the worker (a running build finishes first; queued jobs are dropped)
  TableBuilder(const TableBuilder&) = delete;
  TableBuilder& operator=(const TableBuilder&) = delete;

  // Any thread. Queues `job` for `target`, replacing a job still queued for it.
  void submit(uint32_t target, TableJob job);
  // Any thread but the worker's: waits until no job is queued or running (every `done` returned). False on timeout.
  bool waitIdle(double timeoutSeconds = 60.0);
  bool idle() const;
  Stats stats() const;
  // The live table with this key, or null (tests, tools).
  std::shared_ptr<const dsp::FrameTable> cached(uint64_t key) const;

private:
  void run();
  Done done_;
  bool lowPriority_;
  mutable std::mutex mutex_;
  std::condition_variable wake_, idleCv_;
  std::deque<uint32_t> order_;                       // targets with a queued job, oldest first
  std::unordered_map<uint32_t, TableJob> queued_;    // target → its newest job
  std::unordered_map<uint64_t, std::weak_ptr<const dsp::FrameTable>> cache_;
  bool running_ = false, quit_ = false;
  Stats stats_;
  dsp::FrameTableBuilder builder_;  // worker only: its arenas stay sized between builds
  std::thread thread_;
};

}  // namespace pq::meta
