#pragma once

// The analysis side of a device panel's spectrum (SPEC §11.1, T4.6): one worker thread per analysed tap reads the
// TapRing the audio thread fills (pq/core/tap_ring.h), runs dsp::SpectrumAnalyzer on (L + R) / 2 and publishes
// snapshots; the UI copies the newest one when it draws.
//
// Costs nothing while hidden: with no visible consumer (acquire/release count 0) the worker blocks on a condition
// variable and the tap has no reader, so the audio thread's write is one relaxed load. While shown but with the source
// stopped (setSourceLive(false)), the worker lets the levels fall (silence paced in real time, as PQ Meter does) until
// every point is below the display floor, then blocks too. The UI side stops asking for frames once the snapshot
// stops changing and its ease has settled (SpectrumDisplay in device_panel.cpp).
//
// T4.5 generalises taps to any wire; a feed then wraps whichever tap a probe opens.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

#include "pq/core/tap_ring.h"
#include "pq/dsp/spectrum.h"

namespace pq::views::devices {

class SpectrumFeed {
public:
  // `tap` must outlive the feed. Starts the worker (blocked until acquired).
  SpectrumFeed(TapRing& tap, const dsp::SpectrumConfig& config);
  ~SpectrumFeed();
  SpectrumFeed(const SpectrumFeed&) = delete;
  SpectrumFeed& operator=(const SpectrumFeed&) = delete;

  const dsp::SpectrumConfig& config() const noexcept { return cfg_; }
  float axisHz(float t) const noexcept { return axis_.axisHz(t); }
  float axisT(float hz) const noexcept { return axis_.axisT(hz); }

  // Visible consumers (UI thread). The first acquire registers as the tap's reader and wakes the worker; the last
  // release unregisters and lets it block.
  void acquire();
  void release();
  uint32_t consumers() const noexcept { return consumers_.load(std::memory_order_relaxed); }
  // The source plays (true) or has stopped (false: decay to the floor, then block). Any thread.
  void setSourceLive(bool live);
  bool live() const noexcept { return live_.load(std::memory_order_relaxed); }

  // UI thread: copies the newest levels (dB, one per analyser point) when they are newer than `seq`; returns the
  // snapshot's sequence (unchanged when nothing new). `settled`: the worker has nothing more to show (blocked, or the
  // source stopped and the levels are at the floor).
  uint64_t snapshot(uint64_t seq, std::vector<float>& levels, bool& settled) const;

  // Tests: runs the worker's step synchronously instead of the thread (the thread is not started).
  struct Manual {};
  SpectrumFeed(TapRing& tap, const dsp::SpectrumConfig& config, Manual);
  // One worker iteration (reads the tap, analyses, publishes). Returns the hops analysed. Manual feeds only.
  uint32_t step(double now);
  // No more snapshots will come (a manual feed in a scene or test): the displays may stop animating.
  void hold();
  // Stats (tests, the bench): hops analysed, wake-ups of the worker.
  uint64_t hopsAnalysed() const noexcept { return hops_.load(std::memory_order_relaxed); }
  uint64_t wakeups() const noexcept { return wakeups_.load(std::memory_order_relaxed); }

private:
  void run();
  uint32_t work(double now);  // one iteration; the analyser is the worker's
  void publish();

  TapRing& tap_;
  dsp::SpectrumConfig cfg_;
  dsp::SpectrumAnalyzer analyzer_;  // worker thread
  dsp::SpectrumAnalyzer axis_;      // the same axis, for the UI's mapping (never pushed to)
  std::vector<float> l_, r_, mono_;
  uint64_t pos_ = 0;
  double lastData_ = 0.0, lastSilence_ = 0.0;
  bool decaying_ = false;
  std::atomic<uint32_t> consumers_{0};
  std::atomic<bool> live_{false};
  std::atomic<uint64_t> hops_{0}, wakeups_{0};
  mutable std::mutex mutex_;  // snapshot_, seq_, settled_; and the worker's sleep
  std::condition_variable cv_;
  std::vector<float> snapshot_;
  uint64_t seq_ = 0;
  bool settled_ = true;
  bool quit_ = false;
  std::thread thread_;
};

}  // namespace pq::views::devices
