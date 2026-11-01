// Off-thread frame-table builds (TMM.5): pq/meta/table_builder.h.
#include "pq/meta/table_builder.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

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

void TableBuilder::forget(uint32_t target) {
  std::lock_guard lock(mutex_);
  last_.erase(target);
  ++epoch_[target];
}

void TableBuilder::copyJob(const TableJob& from, TableJob& to) {
  to.data = from.data;
  to.keys = from.keys;
  to.gaps = from.gaps;
  to.spec = from.spec;
  to.key = from.key;
  to.focus = from.focus;
  to.finish();
}

void TableBuilder::focusFrames(const TableJob& job, const TableJob* before, std::vector<uint32_t>& out) {
  out.clear();
  const uint32_t frames = job.spec.denseFrames;
  if (frames < 2) return;
  auto add = [&](int64_t d) {
    if (d < 0 || d >= static_cast<int64_t>(frames) || out.size() >= kPreviewFocusFrames) return;
    const auto f = static_cast<uint32_t>(d);
    if (std::find(out.begin(), out.end(), f) == out.end()) out.push_back(f);
  };
  // The reader's pair at u: k = ⌊u·(F − 1)⌋ (≤ F − 2) and k + 1 (frame_table.h "Reading").
  auto pairAt = [&](float u) -> int64_t {
    const double x = std::clamp(static_cast<double>(u), 0.0, 1.0) * (frames - 1);
    return std::min<int64_t>(static_cast<int64_t>(std::floor(x)), frames - 2);
  };
  const bool focused = job.focus >= 0.0f && std::isfinite(job.focus);
  const int64_t k = focused ? pairAt(job.focus) : 0;
  if (focused) add(k), add(k + 1);
  // Keyframes whose cycle changed since `before` (the same keyframes at the same positions): the pair at each.
  if (before != nullptr && before->spec.denseFrames == frames && before->keys.size() == job.keys.size()) {
    bool same = true;
    for (size_t i = 0; i < job.keys.size() && same; ++i) same = before->keys[i].position == job.keys[i].position;
    if (same)
      for (size_t i = 0; i < job.keys.size(); ++i) {
        const auto a = before->keys[i].cycle, b = job.keys[i].cycle;
        if (a.data() == b.data() ||
            (a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0))
          continue;
        const int64_t kk = pairAt(job.keys[i].position);
        add(kk), add(kk + 1);
      }
  }
  if (focused) add(k - 1), add(k + 2);  // one each side: a position moving a little stays exact
  std::sort(out.begin(), out.end());
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
    const uint64_t epoch = epoch_[target];
    if (const auto it = cache_.find(job.key); it != cache_.end()) table = it->second.lock();
    if (table) {
      ++stats_.shared;
    } else if (q.preview) {
      // A preview, delivered under the job's key, never cached; the full build is queued again behind it, deferred
      // until the target's jobs pause. Focused (TMM.16) over the last table delivered for the target when it has
      // the same frame count, rate and mode; else the spec with fewer dense frames (TMM.9).
      std::shared_ptr<const dsp::FrameTable> base;
      TableJob before;
      bool haveBefore = false;
      if (const auto it = last_.find(target); it != last_.end()) {
        base = it->second.table.lock();  // gone (nobody plays it): TMM.9's preview
        copyJob(it->second.job, before);
        haveBefore = true;
      }
      lock.unlock();
      const auto t0 = Clock::now();
      job.finish();
      bool focused = false;
      Result<dsp::FrameTable> built = Error{ErrorCode::Unknown, "", {}};
      if (base && base->frames() == job.spec.denseFrames && base->layout().sampleRate == job.spec.sampleRate &&
          base->hasAntiderivative() == job.spec.antiderivative) {
        focusFrames(job, haveBefore ? &before : nullptr, focus_);
        if (!focus_.empty()) {
          built = base->clone();
          if (built.ok()) {
            if (Status st = builder_.buildFramesInto(job.spec, built.value(), focus_); !st) built = std::move(st).error();
            else focused = true;
          }
        }
      }
      if (!focused) {
        dsp::FrameTableSpec spec = job.spec;
        spec.denseFrames = kPreviewFrames;
        built = builder_.build(spec);
      }
      const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
      before = TableJob{};  // release the old data outside the lock
      base.reset();
      lock.lock();
      if (built.ok()) {
        table = std::make_shared<const dsp::FrameTable>(std::move(built.value()));
        ++stats_.previews;
        stats_.focused += focused ? 1u : 0u;
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
      // TMM.16: what the next focused preview for this target builds over (unless forgotten meanwhile).
      if (epoch_[target] == epoch) {
        Last& l = last_[target];
        l.table = table;
        copyJob(job, l.job);
      }
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
