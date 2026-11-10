// The background image compiler (T1b.5): pq/engine/background_compile.h describes it.
#include "pq/engine/background_compile.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <utility>

namespace pq::engine {

uint32_t defaultCompileThreads() noexcept {
  if (const char* env = std::getenv("PQ_JIT_THREADS"); env != nullptr && *env != '\0') {
    const long n = std::strtol(env, nullptr, 10);
    if (n >= 1 && n <= 64) return static_cast<uint32_t>(n);
  }
  const unsigned hw = std::thread::hardware_concurrency();
  return std::clamp<uint32_t>(hw > 2 ? hw - 2 : 1, 1, 6);
}

BackgroundCompiler::BackgroundCompiler(const BackgroundCompilerConfig& config) : cfg_(config) {
  const uint32_t n = cfg_.threads != 0 ? cfg_.threads : defaultCompileThreads();
  workers_.reserve(n);
  for (uint32_t k = 0; k < n; ++k) workers_.emplace_back([this] { run(); });
}

BackgroundCompiler::~BackgroundCompiler() {
  {
    std::lock_guard lock(mutex_);
    quit_ = true;
    queue_.clear();
  }
  cv_.notify_all();
  for (std::thread& w : workers_)
    if (w.joinable()) w.join();
}

bool BackgroundCompiler::isRunning(const graph::Schedule* s) const noexcept {
  for (const Running& r : running_)
    if (r.schedule == s) return true;
  return false;
}

void BackgroundCompiler::want(std::vector<BackgroundJob> jobs) {
  {
    std::lock_guard lock(mutex_);
    auto in = [](const std::deque<BackgroundJob>& q, const graph::Schedule* s) {
      for (const BackgroundJob& j : q)
        if (j.schedule.get() == s) return true;
      return false;
    };
    for (Running& r : running_) r.wanted = false;
    // PF.4: new schedules (an edit) first, then those already queued, then refinements — each in the caller's order.
    std::deque<BackgroundJob> fresh, queued, refine;
    for (BackgroundJob& j : jobs) {
      if (j.schedule == nullptr) continue;
      bool running = false;
      for (Running& r : running_)
        if (r.schedule == j.schedule.get()) running = r.wanted = true;
      if (running || in(fresh, j.schedule.get()) || in(queued, j.schedule.get()) || in(refine, j.schedule.get())) continue;
      (j.refine ? refine : in(queue_, j.schedule.get()) ? queued : fresh).push_back(std::move(j));
    }
    std::deque<BackgroundJob> next = std::move(fresh);
    for (BackgroundJob& j : queued) next.push_back(std::move(j));
    for (BackgroundJob& j : refine) next.push_back(std::move(j));
    for (const BackgroundJob& j : queue_) stats_.dropped += in(next, j.schedule.get()) ? 0u : 1u;
    for (const BackgroundJob& j : next) stats_.wanted += in(queue_, j.schedule.get()) ? 0u : 1u;
    queue_ = std::move(next);
  }
  cv_.notify_all();
}

bool BackgroundCompiler::waitIdle(double timeoutSeconds) {
  std::unique_lock lock(mutex_);
  return cv_.wait_for(lock, std::chrono::duration<double>(timeoutSeconds),
                      [&] { return quit_ || (queue_.empty() && running_.empty()); });
}

bool BackgroundCompiler::stopping() const noexcept {
  std::lock_guard lock(mutex_);
  return quit_;
}

BackgroundStats BackgroundCompiler::stats() const {
  std::lock_guard lock(mutex_);
  return stats_;
}

void BackgroundCompiler::run() {
  if (cfg_.lowPriority) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
  std::unique_lock lock(mutex_);
  for (;;) {
    cv_.wait(lock, [&] { return quit_ || !queue_.empty(); });
    if (quit_) return;
    BackgroundJob job = std::move(queue_.front());
    queue_.pop_front();
    running_.push_back(Running{job.schedule.get(), true});
    lock.unlock();
    if (cfg_.beforeBuild != nullptr) cfg_.beforeBuild(cfg_.user, job);
    const auto t0 = std::chrono::steady_clock::now();
    bool refine = false;
    Result<std::unique_ptr<ProgramImage>> img = [&]() -> Result<std::unique_ptr<ProgramImage>> {
      if (cfg_.backend.quick != nullptr && !job.refine)
        return cfg_.backend.quick(cfg_.backend.user, *job.schedule, job.maxBlockFrames, job.maxVoiceGroups, job.resources,
                                  &refine);
      if (cfg_.backend.fn != nullptr)
        return cfg_.backend.fn(cfg_.backend.user, *job.schedule, job.maxBlockFrames, job.maxVoiceGroups, job.resources);
      InterpretOptions io;
      io.maxBlockFrames = job.maxBlockFrames;
      io.maxVoiceGroups = job.maxVoiceGroups;
      io.resources = job.resources;
      return buildInterpreted(*job.schedule, io);
    }();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    const bool ok = img.ok();
    job.first = ok && refine;
    if (cfg_.deliver != nullptr) cfg_.deliver(cfg_.user, job, std::move(img));
    lock.lock();
    auto self = std::find_if(running_.begin(), running_.end(), [&](const Running& r) { return r.schedule == job.schedule.get(); });
    const bool wanted = self != running_.end() && self->wanted;
    if (self != running_.end()) running_.erase(self);
    if (job.first && wanted && !quit_) {  // Q111 C: the final image after every queued first one
      bool queued = false;
      for (const BackgroundJob& j : queue_) queued = queued || j.schedule == job.schedule;
      if (!queued) {
        BackgroundJob r = job;
        r.first = false;
        r.refine = true;
        queue_.push_back(std::move(r));
        ++stats_.wanted;
        ++stats_.refinements;
      }
    }
    if (ok) {
      ++stats_.built;
      stats_.lastBuildMs = ms;
      stats_.totalBuildMs += ms;
    } else {
      ++stats_.failed;
    }
    cv_.notify_all();
  }
}

}  // namespace pq::engine
