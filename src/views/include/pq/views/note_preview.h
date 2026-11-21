#pragma once

// AP.7 (docs/plans/AP.md "Roll preview + Flatten"; pq/document/note_flatten.h): the ROLL PREVIEW of a track's note
// devices — what the Arpeggiator generates from the clip's notes, drawn as ghost notes in the piano roll — and FLATTEN
// TO CLIP, both computed OFF THE UI THREAD. The computation is the app's (pq/app/note_stage_render.h
// renderTrackNoteStages: playback's own lowering and stages, offline), installed as the source; the views only ask,
// draw and dispatch.
//
//   request   (UI thread) a document snapshot, a track and how far (arrangement ticks): returns at once. Requests are
//             DEBOUNCED: the worker waits until none came for kDebounce, then computes the newest one per track (a
//             drag's stream of edits computes once it pauses). Nothing runs on the UI thread.
//   result    the worker hands the result to the UI thread through the context's UI poster (ViewContext::setUiPoster),
//             which stores it and notifies view_change::kNotePreview; without a poster (tests, scenes) the UI thread
//             takes finished results with pump().
//   flatten   AP.8b (review M7): flatten() queues the whole track's render on the same worker (ahead of previews, not
//             debounced: a command the user gave); when it is done the UI thread (pump) builds ops::flattenNoteStages
//             on the document it was computed from and dispatches it — ONE undo step. If the document changed in the
//             meantime it renders again from the current one (a few times, then gives up with a status).
//   config    the render's sample rate and chance seed come from the source's config function, read on the UI thread
//             with every request (the engine's rate can change: a new audio device), and are part of what a result
//             was computed for.
// Not RT code. The destructor cancels a running render (the source sees NotePreviewRequest::cancel) and joins.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "pq/core/result.h"
#include "pq/core/time.h"
#include "pq/document/model.h"
#include "pq/document/note_flatten.h"

namespace pq::views {

class ViewContext;

struct NotePreview {
  bool any = false;                       // the track has an active note-device chain
  std::vector<doc::ClipGenerated> clips;  // per rendered note clip: its generated notes
  // AP.8b (review H3): "<device> <param>" of a cable into a device param, which the render leaves out (its value is
  // only known while the project plays): Flatten refuses while one exists. "": none.
  std::string cable;
};
// What the app renders with (the engine's rate, the link's chance seed).
struct NoteRenderConfig {
  double sampleRate = 48000.0;
  uint64_t seed = 0;
  uint32_t blockFrames = 64;  // the engine's block (automation is read once per block)
  friend bool operator==(const NoteRenderConfig&, const NoteRenderConfig&) = default;
};
struct NotePreviewRequest {
  Ticks until = 0;       // render up to this arrangement tick (Flatten: doc::kMaxTicks)
  bool flatten = false;  // Flatten: every clip of the main lane, deactivated ones as if active (note_flatten.h)
  NoteRenderConfig config;
  const std::atomic<bool>* cancel = nullptr;  // set: stop (the result is discarded)
};
// The computation (the app installs renderTrackNoteStages). Called on the worker only.
using NotePreviewFn = std::function<Result<NotePreview>(const doc::Project&, doc::TrackId, const NotePreviewRequest&)>;

class NotePreviews {
public:
  static constexpr std::chrono::milliseconds kDebounce{40};
  static constexpr int kFlattenRetries = 3;  // renders again when the document changed meanwhile, at most this often

  explicit NotePreviews(ViewContext& ctx);
  ~NotePreviews();  // cancels a running render and joins the worker
  NotePreviews(const NotePreviews&) = delete;
  NotePreviews& operator=(const NotePreviews&) = delete;

  // UI thread, while no request runs (startup). `config` (optional) is read on the UI thread with every request.
  void setSource(NotePreviewFn fn, std::function<NoteRenderConfig()> config = {});
  bool hasSource() const noexcept { return static_cast<bool>(fn_); }
  NoteRenderConfig config() const { return config_ ? config_() : NoteRenderConfig{}; }  // UI thread

  // UI thread: a preview of `track` in `doc` up to arrangement tick `until` (see above).
  void request(doc::Snapshot doc, doc::TrackId track, Ticks until);
  // UI thread: Flatten to Clip of `track` from `doc` (see above). False: no source.
  bool flatten(doc::Snapshot doc, doc::TrackId track);
  bool flattening() const;  // a Flatten is queued or rendering
  // UI thread: the newest result for `track` (null: none yet) and the snapshot it was computed from.
  std::shared_ptr<const NotePreview> latest(doc::TrackId track) const;
  doc::Snapshot latestDoc(doc::TrackId track) const;
  uint64_t serial() const noexcept { return serial_; }  // moves with every result taken
  // UI thread: takes the finished results (what the poster's callback does) and applies finished Flattens; true when
  // any came.
  bool pump();

  // Tests: blocks until nothing is pending or computing (≤ timeout); the worker's thread id; how many renders it ran.
  bool waitIdle(std::chrono::milliseconds timeout = std::chrono::milliseconds(10000));
  std::thread::id workerThread() const noexcept { return workerId_; }
  uint64_t computes() const noexcept;

private:
  struct Job {
    doc::Snapshot doc;
    doc::TrackId track;
    NotePreviewRequest req;
    int retries = 0;  // flatten
  };
  struct Done {
    doc::Snapshot doc;
    doc::TrackId track;
    std::shared_ptr<const NotePreview> result;
    // flatten: its job and the render's error (if any)
    bool flatten = false;
    int retries = 0;
    std::optional<std::string> error;
  };
  void run();
  void post(Job j);
  void finishFlatten(Done& d);  // UI thread

  ViewContext& ctx_;
  NotePreviewFn fn_;
  std::function<NoteRenderConfig()> config_;
  mutable std::mutex m_;
  std::condition_variable cv_, idle_;
  std::vector<Job> pending_;   // previews: one per track (the newest)
  std::vector<Job> flattens_;  // Flattens, in order (before any preview)
  std::chrono::steady_clock::time_point last_{};
  std::vector<Done> done_;
  bool busy_ = false, quit_ = false, flatRunning_ = false;
  std::atomic<bool> cancel_{false};
  uint64_t computes_ = 0;
  std::unordered_map<uint32_t, Done> latest_;  // UI thread
  uint64_t serial_ = 0;
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
  std::thread::id workerId_;
  std::thread worker_;
};

// Flatten to Clip (the roll's command and Arp row, the device menu): queues the render of what track `track`'s note
// devices generate over its whole main lane (NotePreviews::flatten); when it finishes, doc::ops::flattenNoteStages is
// dispatched (one undo step: the notes written, the devices switched off). False — with a status saying why — when it
// cannot start: no active note device, a refusal (ops::flattenRefusal), no renderer, a Flatten already running.
bool flattenTrackNoteDevices(ViewContext& ctx, doc::TrackId track);

}  // namespace pq::views
