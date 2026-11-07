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
//   Scope      TX.scope: the tap's history, kept as it arrives (no analysis: an oscilloscope face reads windows of it
//              with scopeRead and runs its own trigger, pq/dsp/scope.h). Channels as the tap's: a 4-channel tap's four
//              (the Oscilloscope module taps the transport's beat, the lowest voice's pitch, its L and R; pq/meta/
//              oscilloscope.h), a 2-channel tap's two. kScopeFrames frames (5.5 s at 48 kHz). A consumer that wants
//              only the scope costs no spectrum analysis.
//   Spectrogram TX.spectrogram: finished columns of PQ Meter's reassigned spectrogram (pq/dsp/spectrogram.h: three
//              FFTs per hop, the low chain, the leakage mask, consensus, the tent splat) on the channel the consumer asks
//              for, at the hop its span and width ask for (setSpectrogram), kept in a ring of kGramRing columns with
//              each column's centre and the tapped beat there (a 4-channel tap's channel 0: the Spectrogram module taps
//              the transport's beat, for its Loop playhead). The UI reads only the columns it has not seen (gramRead),
//              colours and uploads them. A consumer that wants only the spectrogram costs no spectrum analysis; it
//              settles once as many silent columns as its display shows have passed.
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

#include <algorithm>
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
#include "pq/dsp/spectrogram.h"
#include "pq/dsp/spectrum.h"
#include "pq/dsp/transfer.h"

namespace pq::views::devices {

// What a consumer of a feed draws (acquire's flags).
inline constexpr uint32_t kWantOutput = 1u, kWantInput = 2u, kWantResponse = 4u, kWantScope = 8u, kWantSpectrogram = 16u;

// TX.spectrogram: what a spectrogram consumer asks the feed to compute (setSpectrogram).
struct SpectrogramRequest {
  dsp::SpectrogramConfig config;  // its sampleRate is replaced by the feed's
  uint32_t channel = 0;           // 0 mid (L + R) / 2, 1 L, 2 R, 3 side (L − R) / 2
  uint32_t columns = 512;         // the columns the display shows (silent this long: settled)
  bool operator==(const SpectrogramRequest&) const = default;
};
// One column of the ring: its centre (the analysis stream's sample), the tapped beat there (NaN: none) and its peak.
struct GramColumn {
  double centre = 0.0;
  float beat = 0.0f;
  float peak = 0.0f;
};
// What the ring holds now (gramState).
struct GramState {
  uint64_t end = 0, oldest = 0;  // columns [oldest, end) are held
  double position = 0.0;         // the scroll position in columns (dsp::Spectrogram::position)
  uint32_t mapping = 0;          // bumps when the rows map other frequencies (the display clears)
  uint32_t hop = 0, fft = 0, lowFft = 0;
  float fMin = 0.0f, fMax = 0.0f;
  bool settled = true, running = false;  // running: a spectrogram is computed
};

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

  // ---- TX.scope: the history (kWantScope) ----
  static constexpr uint32_t kScopeFrames = 1u << 18;
  uint32_t scopeChannels() const noexcept { return std::min<uint32_t>(4, tap_.channels()); }
  // UI thread: frames appended so far (free-running; the history holds the last kScopeFrames of them) and whether the
  // worker has nothing new to show (settled: the last 2.2 s appended were silent, or nothing is wanted).
  uint64_t scopeEnd(bool& settled) const;
  // Copies frames [from, from + n) of the history into out[c] (c < scopeChannels(); a null out[c] is skipped); frames
  // no longer (or not yet) held read as 0 (NaN in channels 0 and 1 of a 4-channel tap: "no clock, no note"). Returns
  // the frames that were held.
  uint32_t scopeRead(uint64_t from, uint32_t n, float* const* out) const;

  // ---- TX.spectrogram: the column ring (kWantSpectrogram) ----
  static constexpr uint32_t kGramRing = 512;
  static constexpr uint32_t kGramRows = dsp::Spectrogram::kRows;
  // UI thread: a consumer's request (by key; the one asking for the most columns decides the analysis, so two displays
  // of one instance at different sizes share one), and its removal (with its release).
  void setSpectrogram(const void* key, const SpectrogramRequest& request);
  void clearSpectrogram(const void* key);
  GramState gramState() const;
  // Copies columns [from, from + n) ∩ the held ones into rows (n · kGramRows floats, row 0 = fMax; null: skipped) and
  // info (null: skipped), from its start; returns the columns copied (from < oldest copies nothing).
  uint32_t gramRead(uint64_t from, uint32_t n, float* rows, GramColumn* info) const;

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
  void scopeAppend(const float* const* ch, uint32_t n, bool quiet);  // the worker
  void gramPush(const float* mono, const float* beat, uint32_t n);  // the worker (beat: null = none)
  void pickGram();  // gramMutex_ held: the deciding request
  struct GramSink;

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
  std::atomic<uint32_t> consumers_{0}, wantIn_{0}, wantResp_{0}, wantScope_{0}, wantGram_{0}, noOutput_{0};
  std::atomic<bool> live_{false}, watch_{false};
  std::atomic<uint64_t> hops_{0}, wakeups_{0}, posts_{0};
  std::function<void()> wakePost_;
  std::map<const void*, std::function<void()>> wakers_;  // UI thread
  mutable std::mutex mutex_;  // snap_, seq_, settled_; and the worker's sleep
  std::condition_variable cv_;
  SpectrumSnapshot snap_;
  uint64_t seq_ = 0;
  bool settled_ = true;
  // TX.scope: the history (scopeMutex_; allocated by the first scope consumer).
  mutable std::mutex scopeMutex_;
  std::vector<float> scope_[4];
  uint64_t scopeEnd_ = 0;
  uint64_t scopeQuiet_ = 0;  // frames appended since the last non-silent one (worker)
  // TX.spectrogram: requests (UI, under gramMutex_), the analyser (the worker's alone) and the ring (worker writes,
  // UI reads, gramMutex_). TMM.13-fix1 M4: the worker analyses WITHOUT the lock (a 4096-frame push is 13 – 15 ms, and
  // the UI takes the lock every frame): its columns go to a worker-private stage, and the lock is held only to copy
  // the finished ones into the ring and publish gramPub_.
  mutable std::mutex gramMutex_;
  std::map<const void*, SpectrogramRequest> gramRequests_;
  SpectrogramRequest gramWant_;  // the deciding request
  bool gramDirty_ = false;
  std::vector<float> gramRows_;      // the ring (gramMutex_)
  std::vector<GramColumn> gramInfo_;
  GramState gramPub_;                // the ring's state as last published (gramMutex_; settled is mutex_'s)
  std::unique_ptr<dsp::Spectrogram> gram_;  // worker
  uint32_t gramChannel_ = 0, gramCols_ = 512;  // worker
  std::vector<float> gramIn_, gramBeat_;       // worker: the channel buffer; the beat by stream sample
  std::vector<float> gramStageRows_;           // worker: columns finished in this push (rows, info, index)
  std::vector<GramColumn> gramStageInfo_;
  std::vector<uint64_t> gramStageIndex_;
  uint64_t gramQuiet_ = 0;  // columns since the last one with anything in it (worker)
  void gramPublish();       // the worker: the stage into the ring, gramPub_ (takes gramMutex_)
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
  // GU.7: the feed of a display's signal tap (pq/document/panel.h: the tap of `device` under name path `path`), from
  // the path source (null: none set, or no such tap). Stable while its tap is.
  using PathTapOf = std::function<std::shared_ptr<TapRing>(doc::DeviceId, std::string_view)>;
  void setPathSource(PathTapOf tapOf) { pathTapOf_ = std::move(tapOf); }
  SpectrumFeed* feed(doc::DeviceId device, std::string_view path);
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
  SpectrumFeed* feedOn(Entry* found, std::shared_ptr<TapRing> tap, Entry& slot);
  TapOf tapOf_;
  double rate_;
  Poster post_;
  std::map<uint32_t, Entry> feeds_;
  PathTapOf pathTapOf_;
  std::map<std::pair<uint32_t, std::string>, Entry, std::less<>> pathFeeds_;
  std::vector<std::shared_ptr<SpectrumFeed>> retired_;
};

// TX.spectrum (scenes, tests; device_scenes.cpp): a device's analysis as a project device's tap carries it (input L/R,
// output L/R) — a fixed music-like test signal in, the same through a linear filter of magnitude `mag` (Hz → |H|) out,
// 2 s — analysed now by a manual feed (the output spectrum with peak hold, and with `response` the measured response),
// then held: deterministic.
struct AnalysedFeed {
  std::unique_ptr<TapRing> tap;
  std::unique_ptr<SpectrumFeed> feed;
};
AnalysedFeed makeAnalysedFeed(const std::function<double(double)>& mag, bool response);

}  // namespace pq::views::devices
