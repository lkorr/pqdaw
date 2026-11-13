#pragma once

// Autosave (AS.1; docs/plans/RB.md "### AS.1", D6): the edited document is written, off the UI thread, into this run's
// Unsaved session folder — never into the user's project folder:
//
//   %LOCALAPPDATA%/pqdaw/Unsaved/<pid>-<unix time>/autosave/project.pqd    doc::saveProject (atomic: tmp + rename)
//                                                     /autosave.json  AutosaveMeta (atomic: tmp + rename)
//
// A crash leaves the session folder of a dead process behind (pq/app/recording.h findStaleUnsaved); AS.2 offers its
// autosave back at the next start (views::RecoverRecordingsPopover's Recover mode, ViewContext::recoverAutosave). Relative sample paths in the autosave are relative to AutosaveMeta::originDir (the
// project's folder), not to the autosave folder; an untitled document's recordings are referenced by their absolute
// paths inside its Unsaved recordings folder (AutosaveMeta::recordingsDir). Undo history is not kept.
//
// Policy (tick(), polled by the shell after every presented frame and when its wake timer fires): the current snapshot
// is written when it differs from the last one written and from the saved one (ViewContext::savedSnapshot), no gesture
// is open, and either `quietSeconds` passed since the last change or `maxSeconds` since the document first differed
// from what was written (a long editing run is still saved once a minute). Once the document equals the saved one
// again (Save / Save As, New, Open, an undo back to the saved state) or ViewPrefs::autosave is off, a written autosave
// is deleted. A clean quit deletes it too (close()); a quit that never asked (an error exit) keeps it.
//
// Threads: tick / submit / discard / close on the UI thread. One worker thread (below-normal priority, started at the
// first job) runs the jobs: a single pending slot, latest wins (a discard replaces a pending write and a write a
// pending discard), jobs run one at a time in submission order. With `threaded` false (tests) jobs run in pump().
//
// CPU: an idle tick is a few pointer compares (no change since the last tick: two compares, return). A write costs the
// UI thread a doc::ViewState copy; serialisation (≈ 45 ms for 100k notes) and the disk run on the worker.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include "pq/core/result.h"
#include "pq/document/model.h"
#include "pq/document/store.h"

namespace pq::views {
class ViewContext;
}

namespace pq::app {

inline constexpr std::string_view kAutosaveFolderName = "autosave";       // inside the session folder
inline constexpr std::string_view kAutosaveMetaFileName = "autosave.json";  // beside its project.pqd

// autosave.json. Strings are UTF-8; times are unix seconds.
struct AutosaveMeta {
  std::string originDir;      // the project's folder; "" for an untitled document
  std::string recordingsDir;  // an untitled document's Unsaved recordings folder ("" when titled)
  std::string name;           // the display name (ViewContext::projectName: "Untitled" or the folder's name)
  int64_t savedAt = 0;        // when this autosave was written
  int64_t originMtime = 0;    // originDir/project.pqd's last write time when it was written (0: untitled / missing)
  friend bool operator==(const AutosaveMeta&, const AutosaveMeta&) = default;
};
// Reads `autosaveDir`/autosave.json (NotFound when absent, Parse when malformed).
Result<AutosaveMeta> readAutosaveMeta(const std::filesystem::path& autosaveDir);
// A file's last write time in unix seconds (0 when it does not exist).
int64_t fileUnixTime(const std::filesystem::path& file);
// AS.2: an autosave is worth offering back: always for an untitled document; for a saved project only while its
// origin's project.pqd was not written after the autosave (missing, or last written at or before `savedAt`; a tie
// offers it: a lost edit costs more than a needless offer).
bool autosaveSupersedesOrigin(const AutosaveMeta& m);

struct AutosavePolicy {
  double quietSeconds = 10.0;  // written this long after the last change
  double maxSeconds = 60.0;    // ... or at the latest this long after the document first differed from the autosave
};

class Autosaver {
public:
  // `dir`: the autosave folder (<session>/autosave). `root`: the session folder; the delete goes through
  // removeUnsavedFolder(dir, root)'s guard and close() removes `root` itself when it is left empty.
  Autosaver(std::filesystem::path dir, std::filesystem::path root, bool threaded = true, AutosavePolicy policy = {});
  ~Autosaver();  // the pending job runs, then the thread stops (files are kept: close() deletes them)
  Autosaver(const Autosaver&) = delete;
  Autosaver& operator=(const Autosaver&) = delete;

  // UI thread. Applies the policy to `ctx` at monotonic time `now` (seconds): submits a write or a discard. Returns the
  // seconds after which the policy wants to look again although nothing changed (the shell's wake timer), or a negative
  // value when only a document change can make it act (a change presents a frame, which ticks). A write error is shown
  // once in the status line.
  double tick(views::ViewContext& ctx, double now);

  // UI thread: queues a write of `p` + `view` with `meta` (savedAt and originMtime are filled in by the worker).
  void submit(doc::Snapshot p, doc::ViewState view, AutosaveMeta meta);
  // UI thread: queues the autosave's delete.
  void discard();
  // UI thread, a clean quit: deletes the autosave, then `root` when empty; waits for it.
  void close();
  // Waits until no job is pending or running (threaded), or runs the pending one (not threaded).
  void flush();
  // Not threaded (tests): runs the pending job on this thread. True when one ran.
  bool pump();

  const std::filesystem::path& dir() const noexcept { return dir_; }
  uint64_t writes() const;      // successful writes so far
  std::string lastError() const;  // the last failed job's message ("" when the last job succeeded)

private:
  enum class JobKind : uint8_t { Write, Discard, Close };
  struct Job {
    JobKind kind = JobKind::Write;
    doc::Snapshot project;
    doc::ViewState view;
    AutosaveMeta meta;
  };
  void post(Job j);
  void run(Job& j);  // worker (or pump); no lock held
  void workerMain();

  const std::filesystem::path dir_, root_;
  const bool threaded_;
  const AutosavePolicy policy_;

  // UI thread: the policy's state.
  doc::Snapshot seen_;      // the snapshot the last tick saw
  doc::Snapshot savedSeen_;  // the saved snapshot the last tick saw
  doc::Snapshot written_;   // the snapshot last submitted (null: none since the last discard)
  bool exists_ = false;     // a write was submitted since the last discard
  bool on_ = true;          // ViewPrefs::autosave at the last tick
  double lastChange_ = 0;   // when the last tick saw `seen_` change
  double dirtySince_ = -1;  // when the document first differed from `written_` (< 0: it does not)
  uint64_t errorsShown_ = 0;

  mutable std::mutex mutex_;
  std::condition_variable wake_, idle_;
  std::optional<Job> pending_;
  bool busy_ = false;  // a job runs
  bool stop_ = false;
  uint64_t writes_ = 0;
  std::atomic<uint64_t> errors_{0};  // failed jobs (tick reads it without the lock)
  std::string lastError_;
  std::thread thread_;
};

}  // namespace pq::app
