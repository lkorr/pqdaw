#pragma once

// The recording service (T6.7; docs/plans/M6.md "### T6.7"): the non-RT side of recording. The engine's recorder
// (pq/engine/recorder.h) pushes the song runs of a session and the captured input frames into lock-free rings; this
// service drains them on the sample pool's disk thread (SamplePool::setDiskClient, every 2 ms while a session runs)
// into one 32-bit float WAV per recorded audio track (its input's 1 or 2 channels; crash-safe: the header is patched
// every second, media::WavWriter), keeps the session's song-time map (consecutive runs merged; a loop wrap or a locate
// starts a new PASS) and the live waveform (min / max per kPeakFrames file frames), and buffers the live notes the
// engine's record tap reports for the session's MIDI tracks (pollNotes, UI thread). finish() (UI thread) drains what is
// left, closes the files and returns the passes, the files and the notes; the playback link turns them into takes
// (pq/app/project_link.h "Recording").
//
// Threads: begin / pollNotes / finish / overlay copies on the UI thread; pump on the disk thread (or the UI thread:
// both under one mutex, never held by the audio thread). Idle (no session): the disk client returns at once.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "pq/core/result.h"
#include "pq/document/model.h"
#include "pq/engine/engine.h"
#include "pq/engine/live_input.h"
#include "pq/engine/recorder.h"
#include "pq/media/sample_pool.h"
#include "pq/media/wav_file.h"

namespace pq::app {

// One track a session records.
struct RecordingTrack {
  doc::TrackId track;
  bool audio = false;
  doc::AudioInput input;         // audio: the device channels
  std::filesystem::path path;    // audio: the WAV written
  uint32_t slot = 0xFFFFFFFFu;   // MIDI: the track's event slot (its live notes)
};

// A pass: song samples [songStart, songEnd) the transport played without a jump, starting at engine time
// engineStart; its captured part [audio0, audio1) (song samples; empty: nothing captured) is file frames [file0, …).
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
  int64_t fileFrames = 0;                       // frames in every audio file
  std::vector<engine::LiveNoteRecord> notes;    // the session's live notes (MIDI tracks' slots), in arrival order
  uint64_t droppedFrames = 0;                   // frames written as silence (the rings were full)
  std::vector<std::string> errors;              // files that could not be written
};

class RecordingService {
public:
  static constexpr uint32_t kPeakFrames = 256;  // file frames per waveform bucket
  static constexpr double kRingSeconds = 4.0;    // capture ring per input channel

  // `pool`: its disk thread runs pump() (null: pump() only from the UI thread, e.g. tests without a disk thread).
  explicit RecordingService(media::SamplePool* pool);
  ~RecordingService();
  RecordingService(const RecordingService&) = delete;
  RecordingService& operator=(const RecordingService&) = delete;

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
  // UI thread: drains what is left, closes the files, ends the session.
  RecordingResult finish();
  // Disk thread (or UI): drains the segments and their frames. True: a session runs (poll again).
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
    std::vector<float> peaks;        // the live waveform
    float peakMin = 0.0f, peakMax = 0.0f;
  };
  static bool diskClient(void* self) { return static_cast<RecordingService*>(self)->pump(); }
  bool pumpLocked();  // mutex_ held
  void addRun(const engine::CaptureSegment& s, int64_t fileFrame);

  media::SamplePool* pool_;
  engine::Engine* engine_ = nullptr;
  std::vector<uint16_t> channels_;
  std::vector<uint16_t> wantChannels_;
  std::shared_ptr<engine::CaptureSet> set_;
  uint32_t nextSerial_ = 1;
  bool active_ = false;  // UI thread's view
  uint32_t liveSession_ = 0;

  mutable std::mutex mutex_;  // everything below
  bool running_ = false;
  bool sessionKnown_ = false;
  uint32_t session_ = 0;
  double rate_ = 48000.0;
  std::vector<std::unique_ptr<Writer>> writers_;
  std::vector<RecordingTrack> tracks_;
  std::vector<RecordingPass> passes_;
  int64_t fileFrames_ = 0;
  uint64_t dropped_ = 0;
  std::vector<std::string> errors_;
  std::vector<float> scratch_;    // planar: capture-set channels × frames
  std::vector<float> zeros_;
  uint32_t peakFill_ = 0;
  int64_t lastEngineEnd_ = INT64_MIN, lastSongEnd_ = INT64_MIN;
  std::vector<engine::LiveNoteRecord> notes_;
  std::vector<uint32_t> noteSlots_;
};

}  // namespace pq::app
