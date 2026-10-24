// The background image compiler (T1b.5): pq/engine/background_compile.h describes it.
#include "pq/engine/background_compile.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <chrono>
#include <utility>

namespace pq::engine {

BackgroundCompiler::BackgroundCompiler(const BackgroundCompilerConfig& config) : cfg_(config) {
  worker_ = std::thread([this] { run(); });
}

BackgroundCompiler::~BackgroundCompiler() {
  {
    std::lock_guard lock(mutex_);
    quit_ = true;
    queue_.clear();
  }
  cv_.notify_all();
  if (worker_.joinable()) worker_.join();
}

void BackgroundCompiler::want(std::vector<BackgroundJob> jobs) {
  {
    std::lock_guard lock(mutex_);
    auto in = [](const std::deque<BackgroundJob>& q, const graph::Schedule* s) {
      for (const BackgroundJob& j : q)
        if (j.schedule.get() == s) return true;
      return false;
    };
    std::deque<BackgroundJob> next;
    for (BackgroundJob& j : jobs)
      if (j.schedule != nullptr && j.schedule.get() != running_ && !in(next, j.schedule.get())) next.push_back(std::move(j));
    for (const BackgroundJob& j : queue_) stats_.dropped += in(next, j.schedule.get()) ? 0u : 1u;
    for (const BackgroundJob& j : next) stats_.wanted += in(queue_, j.schedule.get()) ? 0u : 1u;
    queue_ = std::move(next);
  }
  cv_.notify_all();
}

bool BackgroundCompiler::waitIdle(double timeoutSeconds) {
  std::unique_lock lock(mutex_);
  return cv_.wait_for(lock, std::chrono::duration<double>(timeoutSeconds),
                      [&] { return quit_ || (queue_.empty() && running_ == nullptr); });
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
    running_ = job.schedule.get();
    lock.unlock();
    if (cfg_.beforeBuild != nullptr) cfg_.beforeBuild(cfg_.user, job);
    const auto t0 = std::chrono::steady_clock::now();
    Result<std::unique_ptr<ProgramImage>> img = [&]() -> Result<std::unique_ptr<ProgramImage>> {
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
    if (cfg_.deliver != nullptr) cfg_.deliver(cfg_.user, job, std::move(img));
    lock.lock();
    running_ = nullptr;
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
