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
//   - PREVIEW BUILDS (TMM.9, Q173 9): a job whose full build is slow (a bake, ≈ 3–5 ms per dense frame with the
//     live-band bake, or more than kPreviewMaxFullFrames frames) is first built as a preview and delivered under the
//     job's key; the full build follows once no newer job has arrived for that target for kPreviewSettle seconds (a
//     drag that pauses or ends). A newer job replaces the deferred full build, so a drag hears a preview per step
//     instead of a full table every ≈ 65–300 ms. Previews are not cached (the key names the full table). waitIdle
//     runs deferred full builds at once (offline renders).
//     TMM.16 — FOCUSED previews (the picture must match the sound during a drag): a preview is the table this builder
//     last delivered for the target (FrameTable::clone) with only the frames that matter rebuilt from the new spec
//     (FrameTableBuilder::buildFramesInto, ≤ kPreviewFocusFrames): the four dense frames around the job's `focus`
//     (the position the instance is read at: the reader's pair and one each side) and the two around each keyframe
//     whose cycle changed since that table (a stroke on a keyframe away from the position). Where voices read, a
//     preview therefore equals the full table exactly; frames elsewhere keep the previous table's content until the
//     full build. Without a compatible earlier table (the first build, another frame count, rate or mode) the
//     preview is TMM.9's: the spec with kPreviewFrames dense frames spread over the same positions (a blur of the
//     keyframes between them).
// Not RT code.

#include <chrono>
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
  std::span<const uint8_t> modulated;  // TMM.8: per ParamSpec, 1 = modulated (its live path applies it: not baked)
};

// An owned build request. Move-only: `spec`'s spans point into `keys`, `gaps` and `data` (moving the vectors keeps
// their buffers).
struct TableJob {
  DataPtr data;                       // keeps the cycles alive
  std::vector<dsp::Keyframe> keys;    // cycles: spans into data
  std::vector<dsp::GapMode> gaps;
  dsp::FrameTableSpec spec;           // keyframes / gaps: set by finish()
  uint64_t key = 0;                   // identity: equal keys build identical tables
  float focus = -1.0f;                // TMM.16: the position the table is read at (0 … 1; < 0 unknown): what a
                                      // focused preview rebuilds first. Not part of the key.
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
    uint64_t replaced = 0;   // ... replaced by a newer job for the same target before they ran (or before their
                             //     full build, after a preview)
    uint64_t built = 0;      // tables built
    uint64_t shared = 0;     // jobs answered by a live table with the same key (no build)
    uint64_t errors = 0;     // builds that failed (nothing delivered)
    uint64_t previews = 0;   // preview tables built and delivered (TMM.9)
    uint64_t focused = 0;    // ... of which focused on the frames read (TMM.16)
    uint64_t urgentWaits = 0;  // waitFor calls that had something to wait for (TMM.13-fix1 H4)
    double lastBuildMs = 0, maxBuildMs = 0;
    double lastPreviewMs = 0, maxPreviewMs = 0;
  };
  // Preview policy (TMM.9; measured in tests/meta/test_wavetable.cpp, bench/RESULTS.md).
  static constexpr uint32_t kPreviewFrames = 8;
  static constexpr uint32_t kPreviewMaxFullFrames = 128;  // unbaked: a full build of up to 128 frames is ≤ ≈ 20 ms
  static constexpr double kPreviewSettle = 0.1;            // seconds without a newer job before the full build
  static constexpr uint32_t kPreviewFocusFrames = 8;       // TMM.16: frames a focused preview rebuilds at most
  // Whether `spec` is first built as a preview.
  static bool wantsPreview(const dsp::FrameTableSpec& spec) noexcept {
    return spec.denseFrames > kPreviewFrames && (spec.bake.active() || spec.denseFrames > kPreviewMaxFullFrames);
  }
  // target: the caller's id of what the table is for (the link: its slot id); key: the job's key.
  using Done = std::function<void(uint32_t target, uint64_t key, std::shared_ptr<const dsp::FrameTable> table)>;

  explicit TableBuilder(Done done, bool lowPriority = true);
  ~TableBuilder();  // stops the worker (a running build finishes first; queued jobs are dropped)
  TableBuilder(const TableBuilder&) = delete;
  TableBuilder& operator=(const TableBuilder&) = delete;

  // Any thread. Queues `job` for `target`, replacing a job still queued for it.
  void submit(uint32_t target, TableJob job);
  // Any thread but the worker's: waits until no job is queued or running (every `done` returned); deferred full
  // builds (after a preview) run at once. False on timeout.
  bool waitIdle(double timeoutSeconds = 60.0);
  // TMM.13-fix1 H4: any thread but the worker's. Waits until each of `targets` that has a job queued or running now has
  // had a table delivered for its newest job (a preview counts: what a new instance's first image needs is a table of
  // its own state, not the full one; the full build follows as usual), and builds those targets before any other
  // (urgent). Jobs of other targets (a drag on another instance) are not waited for. False on timeout.
  bool waitFor(std::span<const uint32_t> targets, double timeoutSeconds);
  bool idle() const;
  Stats stats() const;
  // TMM.16: the caller no longer uses `target` (its id may go to another instance): a later preview for it never
  // reuses this target's earlier table, and (TMM.13-fix1 L18) a job still queued for it is dropped. A job already
  // running is delivered (the caller's `done` drops what it no longer wants).
  void forget(uint32_t target);
  // The frames a focused preview of `job` rebuilds over an earlier table built from `before` (ascending; empty: no
  // focus known, or the frame counts differ). Exposed for tests.
  static void focusFrames(const TableJob& job, const TableJob* before, std::vector<uint32_t>& out);
  // The live table with this key, or null (tests, tools).
  std::shared_ptr<const dsp::FrameTable> cached(uint64_t key) const;

private:
  using Clock = std::chrono::steady_clock;
  struct Queued {
    TableJob job;
    uint64_t seq = 0;            // per target, counts submits (TMM.13-fix1 H4: what waitFor waits for)
    bool preview = false;        // build a preview first
    Clock::time_point submitted; // when the job was submitted
    Clock::time_point notBefore; // a deferred full build waits until then
  };
  void run();
  // What a focused preview builds over: the last table delivered for a target and the job that described it (its
  // data kept alive by `data`; `spec`'s spans point into `keys` / `gaps`).
  struct Last {
    std::weak_ptr<const dsp::FrameTable> table;  // weak: the builder keeps no table alive (its slot does)
    TableJob job;
  };
  static void copyJob(const TableJob& from, TableJob& to);
  Done done_;
  bool lowPriority_;
  mutable std::mutex mutex_;
  std::condition_variable wake_, idleCv_;
  std::deque<uint32_t> order_;                       // targets with a queued job, oldest first
  std::unordered_map<uint32_t, Queued> queued_;      // target → its newest job
  std::unordered_map<uint64_t, std::weak_ptr<const dsp::FrameTable>> cache_;
  std::unordered_map<uint32_t, Last> last_;           // TMM.16: per target
  std::unordered_map<uint32_t, uint64_t> epoch_;      // TMM.16: per target, counts forget()
  std::unordered_map<uint32_t, uint64_t> submitted_;  // TMM.13-fix1 H4: per target, the seq of the newest job
  std::unordered_map<uint32_t, uint64_t> finished_;   // ... the seq of the newest job delivered (or failed, dropped)
  std::vector<uint32_t> urgent_;                      // targets someone waits for (waitFor): built first
  std::vector<uint32_t> focus_;                       // worker only: a focused preview's frames
  bool running_ = false, quit_ = false;
  uint32_t waiters_ = 0;  // threads in waitIdle (deferred full builds are not deferred then)
  Stats stats_;
  dsp::FrameTableBuilder builder_;  // worker only: its arenas stay sized between builds
  std::thread thread_;
};

}  // namespace pq::meta
