#pragma once

// The views' waveform overviews (T5.5): one per project sample, built or loaded by media's analysis worker and drawn
// by ui::drawWaveform (the arrangement's audio clips, the audio clip view). Owned by the ViewContext
// (ViewContext::waveforms()).
//
//   overview(id)       the overview of a project sample (possibly still building: builtFrames < frames), or null
//                      while it is missing / not loaded yet. The first call requests it.
//   window(id, a, b)   the sample frames for deep zoom (< 256 frames per px) covering [a, b), or null (a read is
//                      requested; the finest level is drawn stretched meanwhile).
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
// UI thread only (except what the workers do internally).

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "pq/document/ids.h"
#include "pq/media/waveform.h"

namespace pq::views {

class ViewContext;

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

private:
  struct Entry {
    std::string path;            // the resolved file it was requested for
    media::OverviewPtr overview;
    bool requested = false;
    bool failed = false;
    bool injected = false;
    media::SampleWindowPtr window;
    int64_t pendingFrom = 0, pendingTo = 0;  // a window read in flight
  };
  struct Arrival;
  struct Inbox;
  static void deliver(const std::weak_ptr<Inbox>& weak, Arrival a);
  Entry& entry(doc::SampleId id);
  void request(doc::SampleId id, Entry& e);
  void drain();

  ViewContext& ctx_;
  std::filesystem::path sharedDir_;
  uint64_t sharedLimit_ = kSharedCacheLimit;
  bool projectCache_ = false;
  std::unordered_map<uint32_t, Entry> entries_;
  uint64_t revision_ = 1;
  uint64_t requests_ = 0;
  uint64_t generation_ = 0;  // bumps on clear(): results of older requests are dropped
  std::shared_ptr<Inbox> inbox_;
  std::unique_ptr<media::AnalysisWorker> builds_, reads_;
};

}  // namespace pq::views
