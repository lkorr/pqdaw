#pragma once

// Frozen-track renders (FZ.2; docs/plans/RB.md D2, D3, "### FZ.2"; pq/document/lower.h "Frozen tracks").
//
// A frozen track plays an app-owned render of its OWN contribution — what its blocks and its own cables give its mixer
// input; the mixer (volume, pan, sends, mute, routing) stays live — through one clip player instead of its circuit. The
// playback link owns a FreezeService (LinkConfig::freeze) and publishes each finished render as a one-clip audio sequence
// in a clip slot of its own; LowerOptions::frozenSlotOf names that slot only while the render in it is current, so the
// track plays live meanwhile (render missing, stale, or failed).
//
//   planFreeze      the render document of frozen track F and its key. The document is the project pruned to F and the
//                   tracks whose blocks feed F's blocks through cables (transitively; a cable from another track's notes
//                   included), with their ancestors (tuning inheritance) as empty containers; returns and the master only
//                   when they feed F. A cable from a group's, a return's or the master's block or source (a sum of other
//                   tracks, F's own output possibly among them) keeps the whole project as it is (every edit then moves
//                   the key). Otherwise normalised: what F's own contribution cannot depend on is cleared —
//                   names, colours, mixers (volume, pan, mute, solo, sends, output, Track Delay; arm, monitor and inputs
//                   stay), mixer lanes, frozen flags, cables into a mixer from another track, the loop, locators, punch
//                   and record settings, samples no kept clip plays — so editing those never re-renders. The KEY is
//                   (engine rate, a hash of the tempo map, of the tunings, of the whole render document as saved
//                   (doc::serializeProject; FNV-1a 64)): a tempo or tuning edit, or an edit of what feeds F (a cable
//                   from another track into F's blocks, an upstream block's knob), makes it stale (QUESTIONS Q252). The
//                   whole tuning list is in it: editing a tuning F does not play re-renders too (Q?-FZ.2-a).
//                   Range: song tick 0 → the last kept clip's end (at least a beat), plus the tail.
//   renderTrackOwn  the render core (FZ.4's bounce reuses it): a private link of the render document (makeOfflineLink,
//                   LinkConfig::captureOwn = F: F lowers live and exports its own sum as bus "<id>:own"), one renderPass
//                   of that bus over the range with a tail until −90 dBFS (kFreezeTailSeconds max), PDC-trimmed (file
//                   frame k = song sample k), 32-bit float WAV written to `<file>.part` and renamed when complete. A
//                   cancel or an error leaves no file.
//   FreezeService   per frozen track: the planned key, the cache lookup, one background render thread (below normal
//                   priority; jobs one at a time, a newer job for a track cancels the older one; renders after an edit
//                   start kFreezeDebounceSeconds after the last one), status and progress. Not RT-safe; the render thread
//                   never touches the playing engine.
//
// Cache (D3, SPEC §12: deletable): `<project>/cache/freeze/<track id>-<content key hex>.wav` and `<track id>.json` (the
// key, the file, frames, channels, rate); an untitled project's live in its Unsaved folder's `cache/freeze`, and Save As
// copies the folder (ViewContext::saveProjectAs). On load (or any sync) a frozen track whose `.json` matches its key and
// whose WAV exists plays it without rendering; deleting cache/ re-renders. A superseded render's WAV is deleted when
// replaced (best effort: a file the pool still streams stays until the next replacement). Files are content-named, so a
// render never overwrites a file a playing image may read.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "pq/core/result.h"
#include "pq/core/time.h"
#include "pq/document/model.h"

namespace pq::app {

inline constexpr double kFreezeTailSeconds = 30.0;     // the tail after the last clip (until −90 dBFS for a second)
inline constexpr double kFreezeDebounceSeconds = 0.25; // a re-render after an edit waits this long for the next edit

struct FreezeKey {
  uint32_t rate = 0;      // the engine rate (Hz) the render plays at
  uint64_t tempo = 0;     // FNV-1a 64 of the tempo map (as saved)
  uint64_t tunings = 0;   // ... of the tuning list
  uint64_t content = 0;   // ... of the whole render document (tempo and tunings included)
  friend bool operator==(const FreezeKey&, const FreezeKey&) = default;
};
std::string hex64(uint64_t v);  // 16 lower-case hex digits

struct FreezePlan {
  doc::Snapshot project;  // the render document (planFreeze above)
  Ticks to = 0;           // the range's end (from 0)
  FreezeKey key;
};
// UI thread or any (pure). Errors: `track` is not an active MIDI / audio track of `p`, a serialization error.
Result<FreezePlan> planFreeze(const doc::Project& p, doc::TrackId track, double rate);
// The render document only (no key): what planFreeze renders and hashes. nullopt: no such frozen-able track.
std::optional<doc::Project> freezeDocument(const doc::Project& p, doc::TrackId track);

struct OwnRenderRequest {
  doc::Snapshot project;  // a document with `track` in it (planFreeze's, or the whole project)
  doc::TrackId track;
  Ticks from = 0, to = 0;
  double tailMaxSeconds = kFreezeTailSeconds;  // 0: exactly [from, to)
  double rate = 48000.0;
  std::filesystem::path file;  // the WAV (32-bit float); written as `<file>.part` first
  std::string projectDir;      // resolves project-relative sample paths
  uint64_t chanceSeed = 0;
  bool jit = true;             // makeOfflineLink's
  const std::atomic<bool>* cancel = nullptr;
  std::atomic<float>* progress = nullptr;  // 0 … 1 (the range and the tail's maximum)
};
struct OwnRender {
  uint64_t frames = 0;   // in the file
  uint32_t channels = 0;
  float peak = 0.0f;
  bool cancelled = false;  // nothing written
};
// The render core (see the file comment). Any thread but the audio thread; blocks for the compile and the render.
Result<OwnRender> renderTrackOwn(const OwnRenderRequest& rq);

// What the track header shows (FZ.3) and the link reports.
enum class FreezeState : uint8_t {
  None,       // not frozen (or not an active MIDI / audio track)
  Rendering,  // a render for the current key is queued or running (the track plays live, or an older render in place)
  Ready,      // the current key's render plays
  Failed,     // the render failed (`error`); the track plays live until an edit re-plans it
};
struct FreezeStatus {
  FreezeState state = FreezeState::None;
  float progress = 0.0f;  // Rendering: 0 … 1
  std::string error;      // Failed
};

class FreezeService {
public:
  struct Options {
    double rate = 48000.0;
    bool jit = true;
    uint64_t chanceSeed = 0;
    // Called on the render thread when a result waits for takeResults() (the link posts it to the UI thread). May be null.
    std::function<void()> notify;
  };
  // A render the link publishes: the file of the track's newest finished render (current or stale).
  struct Render {
    doc::TrackId track;
    std::string path;      // absolute, UTF-8
    uint64_t frames = 0;
    uint32_t channels = 0;
    uint32_t rate = 0;     // the file's
    uint32_t clipId = 0;   // unique per render (the audio image keeps a playing clip by id)
    bool current = false;  // made for the track's current key: the track may play it
  };

  explicit FreezeService(Options o);
  ~FreezeService();  // cancels every render, joins the thread
  FreezeService(const FreezeService&) = delete;
  FreezeService& operator=(const FreezeService&) = delete;

  // UI thread. The frozen tracks of `s`: each is planned (a cheap check first: an unchanged render document keeps its
  // key), its cache looked up in `cacheDir` (`<dir>/cache/freeze`) and a render queued on a miss; tracks no longer frozen
  // are dropped (their renders cancelled). `reset`: a new document (every entry dropped first). Returns true when a
  // track's current render changed (the link republishes its audio and its frozen slots).
  bool sync(const doc::Snapshot& s, const std::string& cacheDir, const std::string& projectDir, bool reset);
  // UI thread: takes the renders that finished; true when one became a track's file.
  bool takeResults();
  // UI thread: the sample rate changed (every key is stale; the next sync re-plans).
  void setRate(double rate);

  bool empty() const;                                  // no frozen track (any thread)
  std::vector<Render> renders() const;                 // UI thread: the files to publish
  FreezeStatus status(doc::TrackId track) const;       // any thread
  uint64_t generation() const noexcept { return gen_.load(std::memory_order_acquire); }  // bumps on every change
  // Paths that stopped being a track's file since the last call (UI thread): the link forgets their loads.
  std::vector<std::string> takeSuperseded();
  // Waits until no render is queued or running (true), at most `timeoutSeconds`. Any thread but the render thread.
  bool waitIdle(double timeoutSeconds);
  struct Stats {
    uint64_t planned = 0;    // render documents hashed (the cheap check missed)
    uint64_t cacheHits = 0;  // files found for a key without rendering
    uint64_t started = 0, finished = 0, cancelled = 0, failed = 0;
  };
  Stats stats() const;

private:
  struct Job {
    doc::TrackId track;
    FreezePlan plan;
    std::filesystem::path dir;  // <cache>/freeze
    std::string projectDir;
    double due = 0.0;           // steady seconds: not before
    std::atomic<bool> cancel{false};
    std::atomic<float> progress{0.0f};
    // Result (render thread → takeResults).
    bool done = false, ok = false;
    std::string error;
    std::string path;
    OwnRender out;
  };
  struct Entry {
    doc::TrackPtr track;            // the node planned last (cheap check: sameFrozenContent and the document equal)
    std::optional<doc::Project> doc;  // its render document
    FreezePlan plan;
    std::filesystem::path dir;
    std::shared_ptr<Job> job;       // queued or running for plan.key
    std::optional<Render> file;     // the newest finished render
    FreezeKey fileKey;
    std::string error;              // the last render's
  };
  void plan(Entry& e, const doc::Project& p, const doc::Track& t, const std::string& cacheDir,
            const std::string& projectDir, bool fresh);
  void queue(Entry& e, doc::TrackId id, const std::string& projectDir, double delay);
  void dropJob(Entry& e);
  bool fileInUse(const std::string& path) const;  // a track's file (never deleted)
  void workerMain();
  void bump() noexcept { gen_.fetch_add(1, std::memory_order_acq_rel); }

  Options opt_;
  mutable std::mutex mutex_;  // entries_, jobs_, results_, superseded_, stats_
  std::condition_variable cv_;
  std::map<uint32_t, Entry> entries_;  // by track id (UI thread writes, under mutex_)
  std::deque<std::shared_ptr<Job>> jobs_;
  std::shared_ptr<Job> running_;
  std::vector<std::shared_ptr<Job>> results_;
  std::vector<std::string> superseded_;
  Stats stats_;
  uint32_t nextClip_ = 0x80000000u;  // render clip ids: above every document id
  bool quit_ = false;
  std::thread worker_;  // started with the first job
  std::atomic<uint64_t> gen_{1};
};

}  // namespace pq::app
