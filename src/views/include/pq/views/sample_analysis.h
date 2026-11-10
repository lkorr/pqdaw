#pragma once

// The views' sample analysis (T6.8): per project sample its transients (media/transients.h: the faint markers on the
// audio clip view's waveform) and its tempo (media/tempo_detect.h: auto-warp, "Warp as N-Bar Loop", the ×2 / ÷2
// suggestions), from one off-thread job per file (media::loadOrDetectTempo: one pass that finds both). Owned by the
// ViewContext (ViewContext::sampleAnalysis()), shaped like Waveforms (pq/views/waveforms.h).
//
//   get(id)                 the analysis of a project sample, or null while it is missing / running. The first call
//                           requests it.
//   whenAnalysed(id, fn)    runs fn(analysis) once it is there (at once when it is; fn gets null if it failed). The
//                           drop's auto-warp waits on this.
//
// Caches: the process-wide memory caches of media (the playback link's Beats job shares them: whichever analyses a
// file first, the other finds it), and on disk the project's `cache/analysis/` when the project has a directory and
// the project cache is on (the app turns it on; off by default, so tests and scenes never write one).
//
// Threads: with a UI poster (ViewContext::setUiPoster, the app) the job runs on a background worker and its result
// comes back through the poster (notifies view_change::kSampleAnalysis); without one (tests, scenes) a request runs
// synchronously in the calling thread.
//
// UI thread only (except what the worker does internally).

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "pq/document/ids.h"
#include "pq/media/tempo_detect.h"
#include "pq/media/transients.h"

namespace pq::media {
class AnalysisWorker;
}

namespace pq::views {

class ViewContext;

struct SampleAnalysisResult {
  media::TransientsPtr transients;  // onsets in source frames
  media::TempoPtr tempo;            // estimate.bpm 0: none
};

class SampleAnalysis {
public:
  explicit SampleAnalysis(ViewContext& ctx);
  ~SampleAnalysis();
  SampleAnalysis(const SampleAnalysis&) = delete;
  SampleAnalysis& operator=(const SampleAnalysis&) = delete;

  // Analyses of a saved project's samples go to <project>/cache/analysis (default off).
  void setProjectCache(bool on) noexcept { projectCache_ = on; }

  const SampleAnalysisResult* get(doc::SampleId id);
  using Callback = std::function<void(const SampleAnalysisResult*)>;
  void whenAnalysed(doc::SampleId id, Callback fn);

  // Bumps whenever an analysis arrived.
  uint64_t revision() const noexcept { return revision_; }
  // The samples were resolved again: entries whose file changed are dropped.
  void samplesResolved();
  // Drops everything (a new / opened project); pending callbacks are dropped too.
  void clear();
  // Blocks until the worker is idle and its results are applied (tests).
  void waitIdle();
  uint64_t requests() const noexcept { return requests_; }

private:
  struct Entry {
    std::string path;
    bool requested = false, done = false, failed = false;
    SampleAnalysisResult result;
    std::vector<Callback> waiting;
  };
  struct Arrival {
    uint32_t id = 0;
    uint64_t generation = 0;
    std::string path;
    SampleAnalysisResult result;
    bool failed = false;
  };
  struct Inbox;
  Entry& entry(doc::SampleId id);
  void request(doc::SampleId id, Entry& e);
  void finish(Entry& e);  // runs the waiting callbacks
  void drain();

  ViewContext& ctx_;
  bool projectCache_ = false;
  std::unordered_map<uint32_t, Entry> entries_;
  uint64_t revision_ = 1;
  uint64_t requests_ = 0;
  uint64_t generation_ = 0;
  std::shared_ptr<Inbox> inbox_;
  std::unique_ptr<media::AnalysisWorker> worker_;
};

}  // namespace pq::views
