#pragma once

// The recording service (T6.7; docs/plans/M6.md "### T6.7"): the non-RT side of recording. The engine's recorder
// (pq/engine/recorder.h) pushes the song runs of a session and the captured input frames (placed on what was played)
// into lock-free rings; this service drains them on its OWN writer thread (M6 review M9: not the sample pool's disk
// thread, so a slow disk never stalls clip-stream refills; every 2 ms while a session runs, asleep otherwise) into one
// 32-bit float WAV per recorded audio track (its input's 1 or 2 channels; crash-safe: the header is patched every
// second, media::WavWriter; past 4 GiB the file becomes RF64, M6 review M8), keeps the session's song-time map
// (consecutive runs merged; a loop wrap or a locate starts a new PASS; each pass's audio is the file range of the
// frames PLAYED in it, a gap inside a pass written as silence) and the live waveform (min / max per kPeakFrames file
// frames), and buffers the live notes the engine's record tap reports for the session's MIDI tracks (pollNotes, UI
// thread). A write error is reported at once (takeErrors) and that track's file stops there. finish() (UI thread)
// drains what is left, closes the files and returns the passes, the files and the notes; the playback link turns them
// into takes (pq/app/project_link.h "Recording").
//
// Threads and locks: begin / pollNotes / takeErrors / finish / live on the UI thread; pump on the writer thread (or
// the UI thread in tests: setWriterThread(false)). The file I/O runs under ioMutex_ (the writer thread; the UI thread
// only in begin / finish / bind / prepareChannels); what the UI reads (passes, frame count, peaks, notes, errors) is
// under a second mutex that is never held across a write, so live() / pollNotes() never wait for the disk (M9). Idle
// (no session): the writer thread sleeps on a condition variable.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "pq/app/autosave.h"
#include "pq/core/result.h"
#include "pq/document/model.h"
#include "pq/engine/engine.h"
#include "pq/engine/live_input.h"
#include "pq/engine/recorder.h"
#include "pq/media/wav_file.h"

namespace pq::app {

// One track a session records.
struct RecordingTrack {
  doc::TrackId track;
  bool audio = false;
  doc::AudioInput input;         // audio: the device channels
  std::filesystem::path path;    // audio: the WAV written
  int64_t frames = 0;            // audio, in the result: the frames its file holds (a write error stops it early)
  uint32_t slot = 0xFFFFFFFFu;   // MIDI: the track's event slot (its live notes)
};

// A pass: song samples [songStart, songEnd) the transport played without a jump, starting at engine time
// engineStart; the input frames PLAYED against its song samples [audio0, audio1) (empty: none) are file frames
// [file0, file0 + audio1 − audio0) (contiguous: a pass's frames are written together, a gap as silence).
struct RecordingPass {
  int64_t songStart = 0, songEnd = 0;
  int64_t engineStart = 0;
  int64_t audio0 = 0, audio1 = 0;
  int64_t file0 = 0;
};

struct RecordingResult {
  uint32_t session = 0;
  double sampleRate = 48000.0;
  std::vector<RecordingTrack> tracks;
  std::vector<RecordingPass> passes;
  int64_t fileFrames = 0;                       // frames the session wrote (each file: RecordingTrack::frames)
  std::vector<engine::LiveNoteRecord> notes;    // the session's live notes (MIDI tracks' slots), in arrival order
  uint64_t droppedFrames = 0;                   // frames written as silence (the rings were full)
  std::vector<std::string> errors;              // write errors takeErrors() has not returned yet
};

// T6.7b: deletes `target` and everything under it, but only when it is safe: `root` and `target` absolute, `target`
// strictly inside `root` (lexically, after normalisation: no "..", not `root` itself), `root` at least two levels below
// its drive / share root, and neither `target` nor anything on the way from `root` to it a symbolic link or junction (a
// link inside `target` is removed as a link, never followed). Returns the entries removed (0 when refused or absent);
// `why` gets the reason for a refusal or a failure (files in use are left).
uint64_t removeUnsavedFolder(const std::filesystem::path& target, const std::filesystem::path& root,
                             std::string* why = nullptr);

// M6 review L13: the Unsaved session folder of a run that ended without cleaning up (a crash, a killed process), and
// the takes in it.
struct StaleUnsavedSession {
  std::filesystem::path dir;                // <root>/<pid>-<unix time>
  std::vector<std::filesystem::path> wavs;  // every .wav under it (not under a `cache` folder: FZ.2's renders), sorted
  uint64_t bytes = 0;                       // their total size
  int64_t started = 0;                      // the session's start (unix seconds, from its name)
  // AS.2: its autosave (<dir>/autosave/project.pqd with a readable autosave.json) when autosaveSupersedesOrigin.
  std::optional<AutosaveMeta> autosave;
};
// Session folders directly under `root` named "<pid>-<unix time>", other than `own`, whose process is gone (`alive`
// false; default processAlive) and that hold at least one .wav or an autosave worth offering (AS.2; an autosave older
// than its origin's project.pqd is ignored, and the folder then counts only for its takes). Oldest first. Reads only
// (never deletes).
std::vector<StaleUnsavedSession> findStaleUnsaved(const std::filesystem::path& root, std::string_view own,
                                                  const std::function<bool(uint32_t pid, int64_t started)>& alive = {});
// AU.app M5: deletes (removeUnsavedFolder) the session folders under `root`, other than `own`, whose process is gone and
// that hold nothing but caches (no file outside a `cache` folder: frozen-track renders are deletable, SPEC §12), which
// findStaleUnsaved never offers. Returns how many went. ProjectLink::staleRecordings runs it before it lists.
uint32_t sweepStaleUnsaved(const std::filesystem::path& root, std::string_view own,
                           const std::function<bool(uint32_t pid, int64_t started)>& alive = {});
// True while process `pid` runs and was created within a few minutes before `started` (a reused pid is another
// process); a process this user cannot query counts as alive.
bool processAlive(uint32_t pid, int64_t started);

class RecordingService {
public:
  static constexpr uint32_t kPeakFrames = 256;  // file frames per waveform bucket
  static constexpr double kRingSeconds = 4.0;    // capture ring per input channel

  RecordingService();   // starts the writer thread
  ~RecordingService();  // a running session is finished first, then the thread stops
  RecordingService(const RecordingService&) = delete;
  RecordingService& operator=(const RecordingService&) = delete;

  // Tests: false stops the writer thread (pump() is then called by hand); true starts it again. UI thread.
  void setWriterThread(bool on);
  // UI thread: the engine whose recorder is drained (a sample-rate change makes a new one). Not during a session.
  void bind(engine::Engine* e);
  // UI thread: the device input channels the armed audio tracks use (sorted, distinct). A changed set gets new rings
  // (allocated here, handed to the recorder) unless a session runs (then at its end); an empty one keeps the rings
  // (nothing is captured without a session).
  void prepareChannels(std::vector<uint16_t> channels);
  // UI thread: starts a session (before the Record command is posted): opens the audio tracks' files (`dir` is
  // created), turns the engine's note tap on. Errors: a session already runs, a file that cannot be created.
  Status begin(std::vector<RecordingTrack> tracks, double sampleRate);
  bool active() const noexcept { return active_; }
  // UI thread: pops the engine's record tap (call it often: the tap is a 4096-entry ring).
  void pollNotes();
  // UI thread: the write errors since the last call (M8: reported while the session runs, not only at its end).
  std::vector<std::string> takeErrors();
  // UI thread: drains what is left, closes the files, ends the session.
  RecordingResult finish();
  // Writer thread (or the UI thread in tests): drains the segments and their frames. True: a session runs.
  bool pump();

  // ---- The live overlay (UI thread; append-only during a session) ----
  struct Live {
    uint32_t session = 0;     // increments per begin(); 0: none yet
    std::vector<RecordingPass> passes;
    int64_t fileFrames = 0;
    std::vector<doc::TrackId> audioTracks;  // the session's audio tracks, in begin() order
    std::vector<std::vector<float>> peaks;  // per audio track: min, max per kPeakFrames file frames (its channels)
    std::vector<engine::LiveNoteRecord> notes;
  };
  // Copies what changed into `out` (peaks / notes appended from out's sizes when out.session is this session).
  void live(Live& out) const;

private:
  struct Writer {
    RecordingTrack track;
    media::WavWriter wav;
    std::vector<int32_t> ringIndex;  // per file channel: index into the capture set's channels (−1: silence)
    float peakMin = 0.0f, peakMax = 0.0f;
  };
  bool pumpLocked();                                                  // ioMutex_ held
  void addRun(const engine::CaptureSegment& s);                       // ioMutex_ held
  void addAudio(const engine::CaptureSegment& s, bool inRings);       // ioMutex_ held
  // Writes `n` frames to every writer — the capture set's channels from scratch_ (planar, `stride` frames per channel)
  // starting at frame `from`, or silence — and adds them to the live peaks. ioMutex_ held.
  void writeFrames(bool fromRings, uint32_t stride, uint32_t from, uint32_t n);
  void startThread();
  void stopThread();
  void writerMain();

  engine::Engine* engine_ = nullptr;
  std::vector<uint16_t> channels_;
  std::vector<uint16_t> wantChannels_;
  uint32_t nextSerial_ = 1;
  bool active_ = false;  // UI thread's view
  uint32_t liveSession_ = 0;
  size_t errorsTaken_ = 0;  // UI thread

  // The writer thread.
  std::thread thread_;
  std::mutex wakeMutex_;
  std::condition_variable wake_;
  bool stop_ = false;                 // wakeMutex_
  std::atomic<bool> pumping_{false};  // a session runs: the thread polls every 2 ms

  std::mutex ioMutex_;  // the writer side: everything down to mutex_
  std::shared_ptr<engine::CaptureSet> set_;
  bool running_ = false;
  bool sessionKnown_ = false;
  uint32_t session_ = 0;
  double rate_ = 48000.0;
  std::vector<std::unique_ptr<Writer>> writers_;
  std::vector<RecordingTrack> tracks_;
  std::vector<float> scratch_;  // planar: capture-set channels × frames
  std::vector<float> zeros_;
  std::vector<std::vector<float>> newPeaks_;  // per writer: peaks finished in this pump, published under mutex_
  uint32_t peakFill_ = 0;
  int64_t lastEngineEnd_ = INT64_MIN, lastSongEnd_ = INT64_MIN;
  std::vector<uint32_t> noteSlots_;

  mutable std::mutex mutex_;  // what the UI reads; never held across a write
  std::vector<RecordingPass> passes_;
  int64_t fileFrames_ = 0;
  uint64_t dropped_ = 0;
  std::vector<std::string> errors_;
  std::vector<doc::TrackId> audioTracks_;
  std::vector<std::vector<float>> peaks_;  // per writer: min, max per kPeakFrames frames
  std::vector<engine::LiveNoteRecord> notes_;
};

}  // namespace pq::app
