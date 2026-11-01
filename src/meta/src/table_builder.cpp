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
  {
    std::lock_guard lock(mutex_);
    ++stats_.submitted;
    auto [it, fresh] = queued_.try_emplace(target);
    if (!fresh) ++stats_.replaced;
    else order_.push_back(target);
    it->second = std::move(job);
  }
  wake_.notify_one();
}

bool TableBuilder::idle() const {
  std::lock_guard lock(mutex_);
  return queued_.empty() && !running_;
}

bool TableBuilder::waitIdle(double timeoutSeconds) {
  std::unique_lock lock(mutex_);
  return idleCv_.wait_for(lock, std::chrono::duration<double>(timeoutSeconds),
                          [&] { return (queued_.empty() && !running_) || quit_; });
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
    wake_.wait(lock, [&] { return quit_ || !order_.empty(); });
    if (quit_) break;
    const uint32_t target = order_.front();
    order_.pop_front();
    auto node = queued_.extract(target);
    TableJob job = std::move(node.mapped());
    running_ = true;
    std::shared_ptr<const dsp::FrameTable> table;
    if (const auto it = cache_.find(job.key); it != cache_.end()) table = it->second.lock();
    if (table) {
      ++stats_.shared;
    } else {
      lock.unlock();
      const auto t0 = std::chrono::steady_clock::now();
      job.finish();
      Result<dsp::FrameTable> built = builder_.build(job.spec);
      const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
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
      job = TableJob{};  // release the data outside the lock
      lock.lock();
    }
    running_ = false;
    if (queued_.empty()) idleCv_.notify_all();
  }
  running_ = false;
  idleCv_.notify_all();
}

}  // namespace pq::meta
