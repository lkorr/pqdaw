// SpectrumFeed (pq/views/devices/analysis.h, T4.6).

#include "pq/views/devices/analysis.h"

#include <algorithm>
#include <chrono>

namespace pq::views::devices {

namespace {
constexpr uint32_t kChunk = 4096;          // frames read from the tap per pass
constexpr double kNoDataSeconds = 0.15;    // then the levels fall as for silence (PQ Meter: 150 ms)
constexpr double kFrameSeconds = 1.0 / 60.0;
constexpr float kSettledDb = -100.0f;      // below the display floor (−78 dB) with margin

double clockNow() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}
}  // namespace

SpectrumFeed::SpectrumFeed(TapRing& tap, const dsp::SpectrumConfig& config, Manual)
    : tap_(tap), cfg_(config), analyzer_(config), axis_([&] {
        dsp::SpectrumConfig c = config;
        c.sharpLows = false;  // the axis only
        return c;
      }()) {
  l_.resize(kChunk);
  r_.resize(kChunk);
  mono_.resize(std::max<uint32_t>(kChunk, cfg_.hop * 32));
  snapshot_.assign(cfg_.points, cfg_.floorDb);
}

SpectrumFeed::SpectrumFeed(TapRing& tap, const dsp::SpectrumConfig& config) : SpectrumFeed(tap, config, Manual{}) {
  thread_ = std::thread([this] { run(); });
}

SpectrumFeed::~SpectrumFeed() {
  {
    std::lock_guard lock(mutex_);
    quit_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
  for (uint32_t c = consumers_.load(); c > 0; --c) tap_.removeReader();
}

void SpectrumFeed::acquire() {
  if (consumers_.fetch_add(1, std::memory_order_relaxed) == 0) {
    tap_.addReader();
    {
      std::lock_guard lock(mutex_);
      settled_ = false;
    }
    cv_.notify_all();
  }
}

void SpectrumFeed::release() {
  if (consumers_.fetch_sub(1, std::memory_order_relaxed) == 1) {
    tap_.removeReader();
    cv_.notify_all();
  }
}

void SpectrumFeed::setSourceLive(bool live) {
  if (live_.exchange(live) == live) return;
  {
    std::lock_guard lock(mutex_);
    settled_ = false;
  }
  cv_.notify_all();
}

uint64_t SpectrumFeed::snapshot(uint64_t seq, std::vector<float>& levels, bool& settled) const {
  std::lock_guard lock(mutex_);
  settled = settled_;
  if (seq != seq_) levels.assign(snapshot_.begin(), snapshot_.end());
  return seq_;
}

void SpectrumFeed::hold() {
  std::lock_guard lock(mutex_);
  settled_ = true;
}

uint32_t SpectrumFeed::step(double now) {
  const uint32_t ran = work(now);
  return ran;
}

uint32_t SpectrumFeed::work(double now) {
  uint32_t ran = 0;
  bool data = false;
  for (int guard = 0; guard < 64; ++guard) {
    const uint64_t w = tap_.written();
    const uint64_t oldest = tap_.oldestReadable();
    if (pos_ < oldest || pos_ > w) pos_ = std::max(oldest, w > cfg_.fftSize * 4u ? w - cfg_.fftSize * 4u : 0);  // resync
    if (pos_ >= w) break;
    const uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(w - pos_, kChunk));
    const uint32_t gl = tap_.read(0, pos_, n, l_.data());
    const uint32_t gr = tap_.channels() > 1 ? tap_.read(1, pos_, n, r_.data()) : gl;
    const uint32_t k = std::min(gl, gr);
    if (k == 0) {  // overwritten while reading: start again from what is still there
      pos_ = tap_.oldestReadable();
      continue;
    }
    const float* r = tap_.channels() > 1 ? r_.data() : l_.data();
    for (uint32_t i = 0; i < k; ++i) mono_[i] = 0.5f * (l_[i] + r[i]);
    ran += analyzer_.push(mono_.data(), k);
    pos_ += k;
    data = true;
  }
  if (data) {
    lastData_ = now;
    lastSilence_ = now;
    decaying_ = false;
  } else if (now - lastData_ > kNoDataSeconds && !analyzer_.allBelow(kSettledDb)) {
    // No input: silence paced in real time, so the release falls as it would on a silent signal.
    if (!decaying_) {
      decaying_ = true;
      lastSilence_ = now;
    }
    const double due = (now - lastSilence_) * cfg_.sampleRate;
    const uint32_t frames = static_cast<uint32_t>(std::min<double>(due, static_cast<double>(mono_.size())));
    if (frames >= cfg_.hop) {
      std::fill(mono_.begin(), mono_.begin() + frames, 0.0f);
      ran += analyzer_.push(mono_.data(), frames);
      lastSilence_ += static_cast<double>(frames) / cfg_.sampleRate;
    }
  }
  const bool settledNow = !data && now - lastData_ > kNoDataSeconds && analyzer_.allBelow(kSettledDb);
  if (ran > 0 || settledNow) {
    hops_.fetch_add(ran, std::memory_order_relaxed);
    std::lock_guard lock(mutex_);
    if (ran > 0) {
      const auto lv = analyzer_.levelsDb();
      snapshot_.assign(lv.begin(), lv.end());
      ++seq_;
    }
    settled_ = settledNow;
  }
  return ran;
}

void SpectrumFeed::run() {
  std::unique_lock lock(mutex_);
  while (!quit_) {
    // Hidden, or nothing more to show: block (no periodic work at all).
    cv_.wait(lock, [&] {
      return quit_ || (consumers_.load(std::memory_order_relaxed) > 0 && (!settled_ || live_.load(std::memory_order_relaxed)));
    });
    if (quit_) break;
    wakeups_.fetch_add(1, std::memory_order_relaxed);
    lock.unlock();
    const double now = clockNow();
    if (lastData_ == 0.0) lastData_ = now;
    work(now);
    lock.lock();
    cv_.wait_for(lock, std::chrono::duration<double>(kFrameSeconds), [&] { return quit_; });
  }
}

}  // namespace pq::views::devices
