#pragma once

// The views' waveform overviews (T5.5): one per project sample, built or loaded by media's analysis worker and drawn
// by ui::drawWaveform (the arrangement's audio clips, the audio clip view). Owned by the ViewContext
// (ViewContext::waveforms()).
//
//   overview(id)       the overview of a project sample (possibly still building: builtFrames < frames), or null
//                      while it is missing / not loaded yet. The first call requests it.
//   window(id, a, b)   the sample frames for deep zoom (below the overview's resolution: ui::waveformLevelFor < 0)
//                      covering [a, b), or null (a read is requested; the finest level is drawn stretched meanwhile).
//                      WF.1: a sample keeps up to kWindowsPerSample windows (the arrangement's clip, the clip view and a
//                      Sampler's well and editor each look at their own part of one file: one window per sample made
//                      them read in turn, for ever). A caller that draws the band-limited curve asks for
//                      ui::kWaveformSampleMargin frames either side of what it shows.
//
// Where they are cached: the project's `cache/waveforms/` when the project has a directory and the project cache is on
// (SPEC §12; the app turns it on), else the shared directory the app sets (`%LOCALAPPDATA%\pqdaw\waveforms`, LRU
// 1 GiB; the browser's files use it too), else nowhere (built again next time). Both are off by default, so tests and
// scenes never write a cache (not into fixture folders, not into the user's %LOCALAPPDATA%) unless they ask to.
//
// Threads: with a UI poster (ViewContext::setUiPoster, the app) requests run on two background workers (overviews;
// sample windows, so a deep-zoom read never waits behind a long build); progress comes back to the UI thread through
// the poster (at most one pending post) and notifies view_change::kWaveforms. Without a poster (tests, scenes) a
// request runs synchronously in the calling thread, so a painted scene shows its waveforms at once.
//
// SX.4: RENDERED waveforms. A time-stretch clip (Beats, Tones, Texture, Complex, Complex Pro) is drawn from its audio as it
// plays (warp, transpose, detune, reverse, markers, the mode's settings) WITHOUT its gain, fades and crossfades (the
// arrangement multiplies those in at draw time, SX.3, so a fade drag never re-renders). The render is the app's (the
// clip player offline: pq/app/clip_waves.h), installed with setClipRenderer; the views only ask, cache and draw.
//
//   rendered(...)   the clip's rendered waveform for its content key (clipRenderKey + the sample's path + the render
//                   config), or, while it renders, an older render of the same clip with the same bounds (`stale`), or
//                   nothing (the caller draws the mapped source peaks). The first call for a key queues a render.
//   debounce        a render starts kRenderDebounce after the clip's key last changed (a knob drag renders once it
//                   pauses); a newer key of the same clip replaces its queued job and cancels its running one.
//   thread          ONE background thread (Windows background mode: low CPU and I/O priority), started with the first
//                   request; results (the frames and their overview, built on the thread) come back through the UI
//                   poster and notify view_change::kWaveforms. Without a poster (tests, scenes) a render runs at once on
//                   the calling thread.
//   memory          a rendered waveform is an ordinary overview (the same renderer, mips) plus its frames (deep zoom,
//                   WF.1). Past kRenderCacheBytes the least recently drawn renders' FRAMES go first (their overviews
//                   stay; a deep zoom on one renders it again), then whole renders.
//   lifetime        setClipRenderer({}), clear() and the destructor cancel the running render and join the thread.
//
// UI thread only (except what the workers do internally).

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "pq/core/result.h"
#include "pq/document/commands.h"
#include "pq/document/ids.h"
#include "pq/document/model.h"
#include "pq/media/waveform.h"

namespace pq::views {

class ViewContext;

// SX.4: what a clip's rendered waveform is rendered with (the engine's rate and block), part of its key.
struct ClipWaveConfig {
  double sampleRate = 48000.0;
  uint32_t blockFrames = 64;
  friend bool operator==(const ClipWaveConfig&, const ClipWaveConfig&) = default;
};
struct ClipWaveRequest {
  doc::ClipRef clip;
  std::string samplePath;  // the sample's absolute path (resolved on the UI thread)
  ClipWaveConfig config;
  const std::atomic<bool>* cancel = nullptr;  // set: stop (the result is discarded)
};
// The render (the app installs app::clipWaveSource): the clip's body frames at config.sampleRate without its envelope,
// one vector per channel. Called on the render thread only (or the UI thread without a poster).
using ClipWaveFn = std::function<Result<std::vector<std::vector<float>>>(const doc::Project&, const ClipWaveRequest&)>;

// A rendered waveform: frame j is song sample ticksToSamples(clip start, rate) + j.
struct RenderedWave {
  media::OverviewPtr overview;    // complete
  media::SampleWindowPtr frames;  // every frame (frame0 0); null once dropped for the memory budget
  double rate = 48000.0;
  uint64_t key = 0;
};
struct RenderedLookup {
  const RenderedWave* wave = nullptr;  // null: draw the mapped source peaks
  bool stale = false;                  // `wave` is an older render of the clip (same bounds)
  bool rendering = false;              // this key's render is queued or running
};

// SX.4: the content key of clip `c` on track `track` (every field that changes what it renders, the tempo over it, its
// resolved pitch ratio, the sample's identity): 0 for a clip drawn from its source (Off, Re-Pitch).
uint64_t clipRenderKey(const doc::Project& p, doc::TrackId track, const doc::AudioClip& c);
inline bool rendersWaveform(doc::AudioWarp w) noexcept { return doc::isWarped(w) && w != doc::AudioWarp::RePitch; }

class Waveforms {
public:
  explicit Waveforms(ViewContext& ctx);
  ~Waveforms();
  Waveforms(const Waveforms&) = delete;
  Waveforms& operator=(const Waveforms&) = delete;

  inline static constexpr uint64_t kSharedCacheLimit = uint64_t{1} << 30;
  // The shared cache directory (empty: none) and its LRU cap.
  void setSharedCacheDir(std::filesystem::path dir, uint64_t limitBytes = kSharedCacheLimit);
  const std::filesystem::path& sharedCacheDir() const noexcept { return sharedDir_; }
  // Overviews of a saved project's samples go to <project>/cache/waveforms (default off).
  void setProjectCache(bool on) noexcept { projectCache_ = on; }

  const media::WaveformOverview* overview(doc::SampleId id);
  const media::SampleWindow* window(doc::SampleId id, int64_t frame0, int64_t frame1);
  // Overviews and windows supplied directly (tests, benchmarks): no request is made for these ids.
  void put(doc::SampleId id, media::OverviewPtr o);
  void putWindow(doc::SampleId id, media::SampleWindowPtr w);

  // Bumps whenever an overview or window arrived or grew (views key their caches by it).
  uint64_t revision() const noexcept { return revision_; }
  // The samples were resolved again (the context calls this): entries whose file changed are dropped.
  void samplesResolved();
  // Drops everything (a new / opened project).
  void clear();
  // Blocks until both workers are idle and their results are applied (tests).
  void waitIdle();
  // Requests made (tests: a cached overview is not requested twice).
  uint64_t requests() const noexcept { return requests_; }
  // Sample-window reads started (tests: a settled deep-zoom paint starts none).
  uint64_t windowReads() const noexcept { return windowReads_; }
  // Bytes held by sample windows; the least recently used windows are dropped past kWindowCacheBytes (M5 review M9).
  uint64_t windowBytes() const noexcept { return windowBytes_; }
  inline static constexpr uint64_t kWindowCacheBytes = uint64_t{128} << 20;
  inline static constexpr size_t kWindowsPerSample = 4;

  // ---- SX.4: rendered clip waveforms (see the file comment) ----
  inline static constexpr std::chrono::milliseconds kRenderDebounce{150};
  inline static constexpr uint64_t kRenderCacheBytes = uint64_t{256} << 20;
  // Installs the render (null: none; a running render is cancelled and the thread joined, the renders dropped).
  // `config` is read on the UI thread with every lookup (the engine's rate can change).
  void setClipRenderer(ClipWaveFn fn, std::function<ClipWaveConfig()> config = {});
  bool hasClipRenderer() const noexcept { return static_cast<bool>(renderFn_); }
  // The waveform to draw clip `ref` (`c`, content key `key` = clipRenderKey, non-zero) of snapshot `doc` from.
  RenderedLookup rendered(const doc::Snapshot& doc, doc::ClipRef ref, const doc::AudioClip& c, uint64_t key);
  // Deep zoom: the frames of `w`, or null when they were dropped for the budget (a render with frames is queued).
  // `doc`, `ref`, `c`: the clip as rendered() was asked (what a re-render renders). Without a poster `w` may be replaced.
  const media::SampleWindow* renderedFrames(const doc::Snapshot& doc, doc::ClipRef ref, const doc::AudioClip& c,
                                            const RenderedWave& w);
  // Tests: renders started on the render function, renders stored, bytes held by renders; blocks until the render
  // thread is idle (with a poster the caller then runs the posted callbacks; without one there is no thread).
  uint64_t rendersStarted() const noexcept;
  uint64_t rendersDone() const noexcept { return rendersDone_; }
  uint64_t renderBytes() const noexcept { return renderBytes_; }
  bool waitRendersIdle(std::chrono::milliseconds timeout = std::chrono::milliseconds(30000));
  // The UI-thread cost of the last swap (finished renders stored: cache insertion and eviction), in seconds.
  double lastSwapSeconds() const noexcept { return lastSwap_; }
  // The memory budget (tests shrink it; default kRenderCacheBytes).
  void setRenderBudget(uint64_t bytes) noexcept { renderBudget_ = bytes; }
  uint64_t renderBudget() const noexcept { return renderBudget_; }
  // Applies finished renders (what the poster's callback does). True when any came.
  bool drainRenders();

private:
  struct Entry {
    std::string path;            // the resolved file it was requested for
    media::OverviewPtr overview;
    bool requested = false;
    bool failed = false;
    bool injected = false;
    struct Win {
      media::SampleWindowPtr w;
      uint64_t use = 0;       // windowClock_ at the last use (LRU of the window cache)
      bool injected = false;  // putWindow: not counted, never evicted
    };
    std::vector<Win> windows;                // WF.1: the windows held (≤ kWindowsPerSample read ones)
    std::vector<std::pair<int64_t, int64_t>> pending;  // window reads in flight
    int64_t fileFrames = -1;                 // the file's length once known (window requests are clamped to it)
    bool windowFailed = false;               // a read failed: not retried on every paint (M5 review H3)
    uint64_t pathHash = 0;                   // SX.4: of `path` (a rendered waveform's key)
  };
  struct Renders;  // SX.4 (rendered_waves.cpp)
  struct RenderJob;
  struct RenderDone;
  void requestRender(RenderJob j);
  void storeRender(RenderDone d);
  void stopRenders();
  void clearRenders();
  struct Arrival;
  struct Inbox;
  static void deliver(const std::weak_ptr<Inbox>& weak, Arrival a);
  Entry& entry(doc::SampleId id);
  void request(doc::SampleId id, Entry& e);
  void drain();
  const media::SampleWindow* storeWindow(Entry& e, media::SampleWindowPtr w);  // keeps the windows within the caps
  void dropWindows(Entry& e);

  ViewContext& ctx_;
  std::filesystem::path sharedDir_;
  uint64_t sharedLimit_ = kSharedCacheLimit;
  bool projectCache_ = false;
  std::unordered_map<uint32_t, Entry> entries_;
  uint64_t revision_ = 1;
  uint64_t requests_ = 0;
  uint64_t windowReads_ = 0;
  uint64_t windowClock_ = 0;
  uint64_t windowBytes_ = 0;  // all entries' windows
  uint64_t generation_ = 0;  // bumps on clear(): results of older requests are dropped
  std::shared_ptr<Inbox> inbox_;
  std::unique_ptr<media::AnalysisWorker> builds_, reads_;
  // SX.4
  ClipWaveFn renderFn_;
  std::function<ClipWaveConfig()> renderConfig_;
  std::shared_ptr<Renders> renders_;  // the thread's shared state (null: no thread yet)
  uint64_t rendersDone_ = 0;
  uint64_t renderBytes_ = 0;
  uint64_t renderClock_ = 0;
  uint64_t renderBudget_ = kRenderCacheBytes;
  double lastSwap_ = 0.0;
};

}  // namespace pq::views
