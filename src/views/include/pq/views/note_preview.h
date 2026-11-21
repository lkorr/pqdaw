#pragma once

// AP.7 (docs/plans/AP.md "Roll preview + Flatten"; pq/document/note_flatten.h): the ROLL PREVIEW of a track's note
// devices — what the Arpeggiator generates from the clip's notes, drawn as ghost notes in the piano roll — computed
// OFF THE UI THREAD. The computation is the app's (pq/app/note_stage_render.h renderTrackNoteStages: playback's own
// lowering and stages, offline), installed as the source; the views only ask and draw.
//
//   request   (UI thread) a document snapshot, a track and how far (arrangement ticks): returns at once. Requests are
//             DEBOUNCED: the worker waits until none came for kDebounce, then computes the newest one per track (a
//             drag's stream of edits computes once it pauses). Nothing runs on the UI thread.
//   result    the worker hands the result to the UI thread through the context's UI poster (ViewContext::setUiPoster),
//             which stores it and notifies view_change::kNotePreview; without a poster (tests, scenes) the UI thread
//             takes finished results with pump().
//   flatten   computeNow() runs the source on the calling thread: Flatten to clip is a command the user gave (it
//             computes the whole track once), not the preview.
// Not RT code.

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
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
  std::vector<doc::ClipGenerated> clips;  // per played note clip: its generated notes
};
// The computation (the app installs renderTrackNoteStages). Called on the worker (and by computeNow).
using NotePreviewFn = std::function<Result<NotePreview>(const doc::Project&, doc::TrackId, Ticks until)>;

class NotePreviews {
public:
  static constexpr std::chrono::milliseconds kDebounce{40};

  explicit NotePreviews(ViewContext& ctx);
  ~NotePreviews();  // joins the worker
  NotePreviews(const NotePreviews&) = delete;
  NotePreviews& operator=(const NotePreviews&) = delete;

  void setSource(NotePreviewFn fn);  // UI thread, while no request runs (startup)
  bool hasSource() const noexcept { return static_cast<bool>(fn_); }

  // UI thread: a preview of `track` in `doc` up to arrangement tick `until` (see above).
  void request(doc::Snapshot doc, doc::TrackId track, Ticks until);
  // UI thread: the newest result for `track` (null: none yet) and the snapshot it was computed from.
  std::shared_ptr<const NotePreview> latest(doc::TrackId track) const;
  doc::Snapshot latestDoc(doc::TrackId track) const;
  uint64_t serial() const noexcept { return serial_; }  // moves with every result taken
  // UI thread: takes the finished results (what the poster's callback does); true when any came.
  bool pump();

  Result<NotePreview> computeNow(const doc::Project& doc, doc::TrackId track, Ticks until) const;

  // Tests: blocks until no request is pending or computing (≤ timeout); the worker's thread id; how many previews it
  // computed.
  bool waitIdle(std::chrono::milliseconds timeout = std::chrono::milliseconds(10000));
  std::thread::id workerThread() const noexcept { return workerId_; }
  uint64_t computes() const noexcept;

private:
  struct Job {
    doc::Snapshot doc;
    doc::TrackId track;
    Ticks until = 0;
  };
  struct Done {
    doc::Snapshot doc;
    doc::TrackId track;
    std::shared_ptr<const NotePreview> result;
  };
  void run();

  ViewContext& ctx_;
  NotePreviewFn fn_;
  mutable std::mutex m_;
  std::condition_variable cv_, idle_;
  std::vector<Job> pending_;  // one per track (the newest)
  std::chrono::steady_clock::time_point last_{};
  std::vector<Done> done_;
  bool busy_ = false, quit_ = false;
  uint64_t computes_ = 0;
  std::unordered_map<uint32_t, Done> latest_;  // UI thread
  uint64_t serial_ = 0;
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
  std::thread::id workerId_;
  std::thread worker_;
};

// Flatten to Clip (the roll's command and Arp row, the device menu): computes what track `track`'s note devices
// generate over its whole clips (NotePreviews::computeNow) and dispatches doc::ops::flattenNoteStages (one undo step:
// the notes written, the devices switched off). False — with a status saying why — when nothing could be flattened.
bool flattenTrackNoteDevices(ViewContext& ctx, doc::TrackId track);

}  // namespace pq::views
