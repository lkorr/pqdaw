#pragma once

// The analysis side of the signal displays (SPEC §11.1; T4.6, TX.spectrum): one worker thread per analysed tap reads
// the TapRing the audio thread fills (pq/core/tap_ring.h), runs the analysers the visible displays ask for and
// publishes snapshots; the UI copies the newest one when it draws.
//
// Tap layouts: a 2-channel tap is a device's OUTPUT (L, R: the stand-alone device preview); a 4-channel tap is the
// signal_tap layout (pq/dsp/atoms.h: input L, R, output L, R: an analysed stock device in the project, the Spectrum
// meta module). What a feed can compute:
//   Output     dsp::SpectrumAnalyzer on (L + R) / 2 of the output (levels and peak hold): always
//   Input      the same on the input (4-channel taps)
//   Response   dsp::TransferAnalyzer from input mid to output mid (4-channel taps): |H| per point and its confidence
//              (no curve on silence or where the output does not follow the input)
// A consumer acquires with the analyses it draws (kWant*); the worker runs only those somebody wants.
//
// Costs nothing while hidden: with no consumer the worker blocks on a condition variable and the tap has no reader, so
// the audio thread's write is one relaxed load. Shown: the worker runs at the display's rate (60 Hz) while the levels
// move. Settled (every level below the display floor and nothing new to analyse) the UI stops asking for frames;
// the worker then
//   - for a feed whose source says when it plays (setSourceLive: the device preview) blocks too, and the display
//     releases the feed; the source's start (DevicePanel::wakeDisplays) restarts it;
//   - for a WATCHED feed (setWatch: the project's taps, whose source is any audio the engine runs) keeps the reader
//     and looks at the tap every 100 ms (an atomic load, and a scan of what arrived for a non-silent sample); new sound
//     wakes the displays through the wake post (setWakePost: posts to the UI thread, which runs the wakers the
//     displays registered). A unit the engine has put to sleep writes nothing.
// AnalysisHub (below) makes the project's feeds: one per analysed device, watched, waking through the UI poster.
//
// Threads: acquire / release / snapshot / wakers on the UI thread; the worker its own; setSourceLive any.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

#include "pq/core/tap_ring.h"
#include "pq/document/ids.h"
#include "pq/dsp/spectrum.h"
#include "pq/dsp/transfer.h"

namespace pq::views::devices {

// What a consumer of a feed draws (acquire's flags).
inline constexpr uint32_t kWantOutput = 1u, kWantInput = 2u, kWantResponse = 4u;

// One published state of a feed (per point of the analyser's axis).
struct SpectrumSnapshot {
  std::vector<float> out, peaks;       // the output's levels and peak hold (dB, after tilt and ballistics)
  std::vector<float> in;               // the input's (empty: not computed)
  std::vector<float> response, confidence;  // |H| dB and 0 … 1 (empty: not computed)
};

class SpectrumFeed {
public:
  // `tap` must outlive the feed (or be held by `owner`). Starts the worker (blocked until acquired).
  SpectrumFeed(TapRing& tap, const dsp::SpectrumConfig& config);
  SpectrumFeed(std::shared_ptr<TapRing> owner, const dsp::SpectrumConfig& config);
  ~SpectrumFeed();
  SpectrumFeed(const SpectrumFeed&) = delete;
  SpectrumFeed& operator=(const SpectrumFeed&) = delete;

  const dsp::SpectrumConfig& config() const noexcept { return cfg_; }
  float axisHz(float t) const noexcept { return axis_.axisHz(t); }
  float axisT(float hz) const noexcept { return axis_.axisT(hz); }
  float pointHz(uint32_t p) const noexcept { return axis_.pointHz(p); }
  // Whether the tap carries the device's input (4 channels): Input and Response can be computed.
  bool hasInput() const noexcept { return tap_.channels() >= 4; }
  const TapRing& tap() const noexcept { return tap_; }

  // Visible consumers (UI thread). The first acquire registers as the tap's reader and wakes the worker; the last
  // release unregisters and lets it block. `wants`: kWant* flags (what this consumer draws).
  void acquire(uint32_t wants = kWantOutput);
  void release(uint32_t wants = kWantOutput);
  uint32_t consumers() const noexcept { return consumers_.load(std::memory_order_relaxed); }
  // The source plays (true) or has stopped (false: decay to the floor, then block). Any thread.
  void setSourceLive(bool live);
  bool live() const noexcept { return live_.load(std::memory_order_relaxed); }
  // A watched feed keeps its reader while settled and wakes its displays when sound arrives (see the file comment).
  void setWatch(bool watch) noexcept { watch_.store(watch, std::memory_order_relaxed); }
  bool watching() const noexcept { return watch_.load(std::memory_order_relaxed); }
  // Called on the WORKER thread when a settled watched feed sees sound again: it must post to the UI thread, which then
  // calls runWakers(). Set before the first acquire.
  void setWakePost(std::function<void()> post) { wakePost_ = std::move(post); }
  // UI thread: wake callbacks (a display requests a frame), by key.
  void addWaker(const void* key, std::function<void()> wake);
  void removeWaker(const void* key);
  void runWakers();

  // UI thread: copies the newest levels (dB, one per analyser point) when they are newer than `seq`; returns the
  // snapshot's sequence (unchanged when nothing new). `settled`: the worker has nothing more to show (blocked, or the
  // levels are at the floor and nothing new arrives).
  uint64_t snapshot(uint64_t seq, std::vector<float>& levels, bool& settled) const;
  uint64_t snapshot(uint64_t seq, SpectrumSnapshot& out, bool& settled) const;

  // Tests: runs the worker's step synchronously instead of the thread (the thread is not started).
  struct Manual {};
  SpectrumFeed(TapRing& tap, const dsp::SpectrumConfig& config, Manual);
  // One worker iteration (reads the tap, analyses, publishes). Returns the hops analysed. Manual feeds only.
  uint32_t step(double now);
  // No more snapshots will come (a manual feed in a scene or test): the displays may stop animating.
  void hold();
  // Stats (tests, the bench): hops analysed, wake-ups of the worker, wake posts.
  uint64_t hopsAnalysed() const noexcept { return hops_.load(std::memory_order_relaxed); }
  uint64_t wakeups() const noexcept { return wakeups_.load(std::memory_order_relaxed); }
  uint64_t wakePosts() const noexcept { return posts_.load(std::memory_order_relaxed); }

private:
  void run();
  uint32_t work(double now);  // one iteration; the analysers are the worker's
  bool peekSound();           // a settled watched feed: anything non-silent arrived (advances pos_)
  uint32_t wants() const noexcept;

  std::shared_ptr<TapRing> owner_;
  TapRing& tap_;
  dsp::SpectrumConfig cfg_;
  dsp::SpectrumAnalyzer out_;   // worker thread
  dsp::SpectrumAnalyzer in_;    // worker thread (4-channel taps)
  dsp::SpectrumAnalyzer axis_;  // the same axis, for the UI's mapping (never pushed to)
  std::unique_ptr<dsp::TransferAnalyzer> transfer_;  // worker thread (4-channel taps)
  std::vector<float> ch_[4], mono_, monoIn_;
  uint64_t pos_ = 0;
  double lastData_ = 0.0, lastSilence_ = 0.0;
  bool decaying_ = false;
  uint32_t ranWants_ = 0;  // what the worker computed last (a new want restarts its analyser)
  std::atomic<uint32_t> consumers_{0}, wantIn_{0}, wantResp_{0};
  std::atomic<bool> live_{false}, watch_{false};
  std::atomic<uint64_t> hops_{0}, wakeups_{0}, posts_{0};
  std::function<void()> wakePost_;
  std::map<const void*, std::function<void()>> wakers_;  // UI thread
  mutable std::mutex mutex_;  // snap_, seq_, settled_; and the worker's sleep
  std::condition_variable cv_;
  SpectrumSnapshot snap_;
  uint64_t seq_ = 0;
  bool settled_ = true;
  bool quit_ = false;
  std::thread thread_;
};

// The project's feeds (TX.spectrum): one per device that has an analysis tap (an analysed stock device, a chain
// Spectrum meta device), created on first use, watched, waking its displays through the UI poster. A device whose
// tap object changed (it was removed and its slot reused) gets a new feed; a superseded feed is dropped once nobody
// holds it. UI thread only.
class AnalysisHub {
public:
  using TapOf = std::function<std::shared_ptr<TapRing>(doc::DeviceId)>;
  using Poster = std::function<void(std::function<void()>)>;
  AnalysisHub(TapOf tapOf, double sampleRate, Poster post);
  ~AnalysisHub();
  // A new tap source / sample rate: every feed is superseded (kept until no display holds it).
  void setSource(TapOf tapOf, double sampleRate);
  // The device's feed (null: it has no tap). Stable while its tap is.
  SpectrumFeed* feed(doc::DeviceId device);
  size_t feeds() const noexcept { return feeds_.size(); }
  double sampleRate() const noexcept { return rate_; }
  // Scenes and tests: `feed` (on `tap`) is the device's feed while the tap source gives `tap` for it.
  void adopt(doc::DeviceId device, std::shared_ptr<TapRing> tap, std::shared_ptr<SpectrumFeed> feed);
  // The project's feeds use this analyser configuration (SPEC §11.1's spectrum; peak hold computed).
  static dsp::SpectrumConfig config(double sampleRate);

private:
  struct Entry {
    std::shared_ptr<TapRing> tap;
    std::shared_ptr<SpectrumFeed> feed;
  };
  TapOf tapOf_;
  double rate_;
  Poster post_;
  std::map<uint32_t, Entry> feeds_;
  std::vector<std::shared_ptr<SpectrumFeed>> retired_;
};

}  // namespace pq::views::devices
