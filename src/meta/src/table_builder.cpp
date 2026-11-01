// Off-thread frame-table builds (TMM.5): pq/meta/table_builder.h.
#include "pq/meta/table_builder.h"

#include <algorithm>
#include <chrono>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace pq::meta {

TableBuilder::TableBuilder(Done done, bool lowPriority) : done_(std::move(done)), lowPriority_(lowPriority) {
  thread_ = std::thread([this] { run(); });
}

TableBuilder::~TableBuilder() {
  {
    std::lock_guard lock(mutex_);
    quit_ = true;
    queued_.clear();
    order_.clear();
  }
  wake_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void TableBuilder::submit(uint32_t target, TableJob job) {
  job.finish();
  const bool preview = wantsPreview(job.spec);
  const Clock::time_point now = Clock::now();
  {
    std::lock_guard lock(mutex_);
    ++stats_.submitted;
    auto [it, fresh] = queued_.try_emplace(target);
    if (!fresh) ++stats_.replaced;
    else order_.push_back(target);
    it->second.job = std::move(job);
    it->second.preview = preview;
    it->second.submitted = now;
    it->second.notBefore = now;
  }
  wake_.notify_one();
}

bool TableBuilder::idle() const {
  std::lock_guard lock(mutex_);
  return queued_.empty() && !running_;
}

bool TableBuilder::waitIdle(double timeoutSeconds) {
  std::unique_lock lock(mutex_);
  // Deferred full builds (waiting for a drag to pause) are wanted now.
  bool deferred = false;
  for (auto& [target, q] : queued_)
    if (q.notBefore > q.submitted) {
      q.notBefore = q.submitted;
      deferred = true;
    }
  if (deferred) wake_.notify_all();
  ++waiters_;
  const bool ok = idleCv_.wait_for(lock, std::chrono::duration<double>(timeoutSeconds),
                                   [&] { return (queued_.empty() && !running_) || quit_; });
  --waiters_;
  return ok;
}

TableBuilder::Stats TableBuilder::stats() const {
  std::lock_guard lock(mutex_);
  return stats_;
}

std::shared_ptr<const dsp::FrameTable> TableBuilder::cached(uint64_t key) const {
  std::lock_guard lock(mutex_);
  const auto it = cache_.find(key);
  return it != cache_.end() ? it->second.lock() : nullptr;
}

void TableBuilder::run() {
#ifdef _WIN32
  if (lowPriority_) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
  std::unique_lock lock(mutex_);
  while (true) {
    // The oldest target whose job may run now; otherwise wait for a submit or the earliest deferred full build.
    std::deque<uint32_t>::iterator pick = order_.end();
    Clock::time_point earliest = Clock::time_point::max();
    const Clock::time_point now = Clock::now();
    for (auto it = order_.begin(); it != order_.end(); ++it) {
      const Clock::time_point nb = queued_.at(*it).notBefore;
      if (nb <= now) {
        pick = it;
        break;
      }
      earliest = std::min(earliest, nb);
    }
    if (quit_) break;
    if (pick == order_.end()) {
      if (earliest == Clock::time_point::max()) wake_.wait(lock);
      else wake_.wait_until(lock, earliest);
      continue;
    }
    const uint32_t target = *pick;
    order_.erase(pick);
    auto node = queued_.extract(target);
    Queued q = std::move(node.mapped());
    TableJob& job = q.job;
    running_ = true;
    std::shared_ptr<const dsp::FrameTable> table;
    bool deferFull = false;
    if (const auto it = cache_.find(job.key); it != cache_.end()) table = it->second.lock();
    if (table) {
      ++stats_.shared;
    } else if (q.preview) {
      // A preview: the same spec with fewer dense frames, delivered under the job's key, never cached; the full
      // build is queued again behind it, deferred until the target's jobs pause.
      lock.unlock();
      const auto t0 = Clock::now();
      job.finish();
      dsp::FrameTableSpec spec = job.spec;
      spec.denseFrames = kPreviewFrames;
      Result<dsp::FrameTable> built = builder_.build(spec);
      const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
      lock.lock();
      if (built.ok()) {
        table = std::make_shared<const dsp::FrameTable>(std::move(built.value()));
        ++stats_.previews;
        stats_.lastPreviewMs = ms;
        stats_.maxPreviewMs = std::max(stats_.maxPreviewMs, ms);
      }
      deferFull = true;
    } else {
      lock.unlock();
      const auto t0 = Clock::now();
      job.finish();
      Result<dsp::FrameTable> built = builder_.build(job.spec);
      const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
      lock.lock();
      if (built.ok()) {
        table = std::make_shared<const dsp::FrameTable>(std::move(built.value()));
        ++stats_.built;
        stats_.lastBuildMs = ms;
        stats_.maxBuildMs = std::max(stats_.maxBuildMs, ms);
        // Drop dead entries now and then (the cache holds only what some slot still plays).
        if (cache_.size() > 64)
          std::erase_if(cache_, [](const auto& e) { return e.second.expired(); });
        cache_[job.key] = table;
      } else {
        ++stats_.errors;
      }
    }
    if (table && !quit_) {
      lock.unlock();
      done_(target, job.key, std::move(table));
      lock.lock();
    }
    if (deferFull && !quit_ && !queued_.contains(target)) {
      // No newer job arrived meanwhile: the full build, once the target has been quiet for kPreviewSettle.
      // (Not while someone waits for idle: an offline render wants the full table now.)
      q.preview = false;
      q.notBefore = waiters_ > 0 ? q.submitted
                                 : q.submitted + std::chrono::duration_cast<Clock::duration>(
                                                     std::chrono::duration<double>(kPreviewSettle));
      queued_.emplace(target, std::move(q));
      order_.push_back(target);
    } else {
      if (deferFull && !quit_) ++stats_.replaced;  // a newer job came while the preview was built: its full table never
      lock.unlock();
      q = Queued{};  // release the data outside the lock
      lock.lock();
    }
    running_ = false;
    if (queued_.empty()) idleCv_.notify_all();
  }
  running_ = false;
  idleCv_.notify_all();
}

}  // namespace pq::meta
