#pragma once

// The background image compiler (T1b.5, SPEC §7.5 "an edit plays immediately through the interpreter, then swaps to
// the JIT image"): ONE worker thread that builds images of unit schedules with a UnitBackend (the JIT: the engine
// never includes jit, so the backend is a function pointer, e.g. pq::compile::jitUnitBackend()) and hands each
// finished image to the caller's `deliver` on that thread. The caller (the app's ProjectLink) publishes it into its
// ProjectProgram if the unit still runs that schedule (ProjectCompiler::unitOf) — an Instant swap, sample-exact,
// because JIT and interpreter images are bit-identical by default (T1b.1-fix1) and the swap transfers all state.
//
// ---- Policy (bounded, low priority) ------------------------------------------------------------------------------
//   - one thread, so at most one compile (one core) at any time, whatever the edit rate;
//   - the thread runs at THREAD_PRIORITY_BELOW_NORMAL (lowPriority): the UI thread (normal) and the audio thread
//     (MMCSS "Pro Audio") preempt it, and the interpreted images keep playing meanwhile;
//   - want(jobs) REPLACES the queue: the compile path names the schedules that still lack a JIT image after every
//     structural compile, so a job for a schedule that was edited again (or whose unit was removed) is dropped
//     before it starts. A compile already running is not interrupted (LLVM cannot be cancelled); its image is
//     delivered and the caller drops it when the unit has moved on (stale);
//   - a schedule is compiled once per want() that names it while it is neither queued nor running;
//   - T1b.x-perf (QUESTIONS Q111 option C): with a UnitBackend::quick, a job builds a quick first image when that is
//     quicker (the JIT's voice groups; a cache hit or a unit without pairs gives the final image at once), delivers it
//     marked `first`, and queues a refinement job (`refine`: the final image through UnitBackend::fn) at the END of
//     the queue, so every unit gets its first image before any unit's final one. A refinement is queued only if no
//     want() since the job started left its schedule out (the caller has moved on). want() takes refinement jobs too
//     (the caller lists the schedules that have a first image only with `refine` set).
//
// Threads: construction, want(), waitIdle(), stats() and destruction from any non-RT thread (internally locked).
// Nothing here runs on the audio thread.

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "pq/core/result.h"
#include "pq/engine/interpreter.h"
#include "pq/engine/program_image.h"
#include "pq/engine/project_compile.h"
#include "pq/graph/schedule.h"

namespace pq::engine {

struct BackgroundJob {
  std::shared_ptr<const graph::Schedule> schedule;  // the identity of a job (one per schedule)
  uint32_t maxBlockFrames = 1024;
  uint32_t maxVoiceGroups = 1;
  const Resources* resources = nullptr;  // must outlive the image
  uint64_t tag = 0;                      // the caller's (e.g. the unit's compile-cache key)
  bool refine = false;  // build the final image (UnitBackend::fn) even if the backend has a quick one (Q111 C)
  bool first = false;   // set on delivery: a quick first image; a refinement job for the schedule is queued after it
};

struct BackgroundCompilerConfig {
  UnitBackend backend;  // required: what builds the images (fn null: the interpreter, for tests)
  // Worker thread, after each build (successful or not). It may block briefly (e.g. wait for room to publish);
  // poll stopping() while it does.
  void (*deliver)(void* user, const BackgroundJob& job, Result<std::unique_ptr<ProgramImage>> image) = nullptr;
  // Worker thread, right before each build: a test hook (a simulated slow compile: block until released).
  void (*beforeBuild)(void* user, const BackgroundJob& job) = nullptr;
  void* user = nullptr;
  bool lowPriority = true;  // THREAD_PRIORITY_BELOW_NORMAL for the worker
};

struct BackgroundStats {
  uint64_t wanted = 0;      // jobs queued by want()
  uint64_t dropped = 0;     // queued jobs removed by a later want() before they started
  uint64_t built = 0;       // images built
  uint64_t failed = 0;      // builds that returned an error
  uint64_t refinements = 0; // refinement jobs queued after a first image (Q111 C)
  double lastBuildMs = 0.0, totalBuildMs = 0.0;
};

class BackgroundCompiler {
public:
  explicit BackgroundCompiler(const BackgroundCompilerConfig& config);  // starts the worker
  // Drops the queue, waits for a running build (and its delivery) to finish, joins the worker.
  ~BackgroundCompiler();
  BackgroundCompiler(const BackgroundCompiler&) = delete;
  BackgroundCompiler& operator=(const BackgroundCompiler&) = delete;

  // Replaces the queue with `jobs` (in order), minus the one running now; see "Policy".
  void want(std::vector<BackgroundJob> jobs);
  // True once nothing is queued or running (every wanted image delivered); false on timeout.
  bool waitIdle(double timeoutSeconds = 60.0);
  // The destructor has begun: a deliver that waits should give up.
  bool stopping() const noexcept;
  BackgroundStats stats() const;

private:
  void run();

  BackgroundCompilerConfig cfg_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<BackgroundJob> queue_;
  const graph::Schedule* running_ = nullptr;
  bool runningWanted_ = false;  // no want() since the running job started left its schedule out
  bool quit_ = false;
  BackgroundStats stats_;
  std::thread worker_;
};

}  // namespace pq::engine
