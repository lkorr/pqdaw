#pragma once

// FZ.4 (docs/plans/RB.md D5, "### FZ.4"): the edits that render first, as views::RenderEditPort over the playback link.
// start() (UI thread) takes what the job needs from the document and the link, then renders on a worker thread (below
// normal priority; one job at a time); the status line shows its progress with a Cancel button. When the render is
// done the result comes back to the UI thread (ViewContext::uiPoster; without one, poll()), the edit is built on the
// document as it is THEN and dispatched as ONE undo step — unless the document changed what was rendered meanwhile:
// then the job is dropped, its files deleted, and the status line says why.
//   Flatten      each frozen track's current render (ProjectLink::frozenRender: Ready) is copied (the cache copy stays,
//                so an undo plays it again without a render) to `samples/frozen/<track name>.wav` and scanned for its
//                regions (below); ops::flattenTrack per track. Dropped when a track's render changed (another file) or
//                the track is no longer frozen.
//   Bounce       renderTrackOwn of each track's render document (planFreeze) over the time selection (exact, no tail)
//                or the whole track (0 → its last clip's end, plus the tail) into `samples/bounced/<name> Bounce.wav`;
//                ops::bounceToNewTrack (the whole track: its regions; a selection: one clip over it). Dropped when a
//                track's render document changed (freezeDocument).
//   Consolidate  per audio track, ProjectLink::renderClips over [from, to) into `samples/consolidated/<name> <n>.wav`;
//                ops::consolidateAudio. Dropped when a track's clips, the tempo map, the tunings or the sample table
//                changed.
// A titled project's files go under its folder; an untitled one's into its unsaved folder (ViewContext::
// unsavedRecordingDir: its first save moves them into the project's samples/recorded/, as recorded takes). File names
// are made safe and never overwrite (" 2", " 3", …). Files are 32-bit float WAV at the engine rate.
// REGIONS (renderRegions) of a render whose file frame 0 is song sample samples(origin): the spans where some channel
// is above kRegionFloor, joined across quiet gaps shorter than kRegionGapSeconds, widened by kRegionPadSeconds on both
// sides (within the file) and rounded outwards to ticks — so a clip edge lies in quiet frames (render clips have no
// edge declick, FZ.x-fix: AudioClip::noEdgeFade; one moved into sound starts hard) and the clips play the render
// within kRegionFloor of it everywhere.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "pq/core/result.h"
#include "pq/core/time.h"
#include "pq/document/edit_ops.h"
#include "pq/views/render_edit_port.h"

namespace pq::views {
class ViewContext;
}

namespace pq::app {

class ProjectLink;

inline constexpr float kRegionFloor = 1.0e-6f;      // −120 dBFS
inline constexpr double kRegionGapSeconds = 1.0;    // shorter quiet gaps do not split a region
inline constexpr double kRegionPadSeconds = 0.01;   // quiet kept around each region

// The regions of the WAV `file` (above) as tick ranges, its frame 0 at tick `origin` of `tempo` (rate: the file's).
// Errors: the file cannot be read.
Result<std::vector<doc::ops::RenderRegion>> renderRegions(const std::filesystem::path& file, const TempoMap& tempo,
                                                          Ticks origin);

class RenderEditService final : public views::RenderEditPort {
public:
  // `jit` false: interpreter images for Bounce's private link (tests).
  RenderEditService(ProjectLink& link, views::ViewContext& ctx, bool jit = true);
  ~RenderEditService() override;  // cancels a running job (its files deleted) and joins its thread
  RenderEditService(const RenderEditService&) = delete;
  RenderEditService& operator=(const RenderEditService&) = delete;

  bool start(const views::RenderEditJob& job) override;
  bool busy() const override;
  void cancel() override;

  // UI thread: applies a finished job (the posted result does it; tests call it after waitIdle). True when it ran one.
  bool poll();
  // Waits until the worker has finished (true), at most `timeoutSeconds`. Tests.
  bool waitIdle(double timeoutSeconds = 120.0);

  struct Job;  // render_edits.cpp

private:
  void post();
  void showProgress();

  ProjectLink& link_;
  views::ViewContext& ctx_;
  bool jit_;
  std::shared_ptr<Job> job_;  // the running or finished job (UI thread)
  std::thread worker_;
  std::shared_ptr<bool> alive_;  // posted closures run only while this service lives (UI thread)
};

}  // namespace pq::app
