#pragma once

// TransportPort (T3.7): how views start/stop playback and read the playhead without depending on the engine.
// `views` never includes `engine` (ARCHITECTURE §1); `app` implements this interface over the engine (T3.17) and,
// until then, over a wall-clock simulation (pq/app/sim_transport.h). Tests use FakeTransport
// (pq/views/fake_transport.h), which records every request.
//
// Plain data only: positions are int64 ticks (PPQ 221760), the loop is three numbers. The port is called on the UI
// thread; an engine implementation turns calls into EngineCommands (SPSC) and reads the playhead from the lossy
// EngineReport ring, so playhead() is a cheap copy of the last report and may lag the audio by up to a block.
//
// Semantics (Ableton Live's transport):
//   play(from)          start playback at `from` (Space plays from the start marker the views keep).
//   stop()              stop; the stopped position is remembered for continuePlayback().
//   continuePlayback()  Shift+Space: resume from where playback stopped (or the last locate while stopped).
//   locate(t)           jump: while playing, playback continues from t; while stopped, t becomes the position
//                       continuePlayback() resumes from.
//   setLoop(l)          the arrangement loop the transport wraps at while it is enabled (mirrors the document's
//                       LoopRegion; ViewContext keeps it in sync).
//   playhead()          the last known transport state. Views read it only while playing (the control bar's
//                       position display, T3.8's playhead line).
//   auditionHz(hz, velocity, durationMs)
//                       T3.11: preview one pitch on the focused clip's track (a note clicked or drawn in the piano
//                       roll, a key pressed in its gutter): `hz` resolved from the note's pitch identity, velocity
//                       in (0, 127], the sound released after durationMs. Fire and forget, independent of playback;
//                       T3.17 implements it over the engine (the app's simulated transport ignores it).
//   releaseAuditions()  AU.voices: the press that auditioned ended (the pointer let go): every sounding audition is
//                       released now, or at its own end when that comes first (≥ 50 ms after its onset), so a note
//                       clicked in the piano roll sounds while it is held, at most its own length.
//   liveKey(equave, step, velocity, on)
//                       T6.4: the computer MIDI keyboard (`M`): degree `step` (0 … 16) above equave offset `equave`
//                       (−5 … 5) went down (velocity 1 … 127) or up, into the armed tracks that listen to it (each in its
//                       own tuning and note input mode). A key-up repeats its key-down's equave and step.
//   record(from)        T6.6 / T6.7: the Record button (F9). Stopped: the project's count-in (RecordSettings::countInBars;
//                       the transport holds at `from` meanwhile and playhead() reports countingIn), then playback and
//                       recording from `from` (the armed tracks' inputs: audio to WAV, notes; punch, loop takes).
//                       Playing: recording starts where the playhead is. Recording: recording ends, playback goes on
//                       (the session is committed as one undo step). Default: play(from).
//   captureMidi(insertMarker)
//                       T6.9: Capture MIDI (Ctrl+Shift+C): what the listening tracks played without recording becomes
//                       clips (one undo step): playing, at the song ticks it was played against; stopped, at the bar
//                       of `insertMarker`, looped, the tempo inferred from it (set when the project has no clips).
//                       False: nothing to capture. Default: false.
//   outputLatencyMs()   T6.6: how late the output is behind the transport (the PDC total), ms. Default 0.
//   trackLatencyMs(id)  T6.6: how late track `id`'s output is (its compensated latency, Track Delay included), ms.
//   audioInputChannels()   T6.7: the open device's input channels (the strips' input chooser). Default 0.
//   recordingOverlay(out)  T6.7: what the running (or last) recording session made so far, for the arrangement's
//                       live overlay: false when nothing changed since `out` was filled (out.revision), else `out` is
//                       updated (its waveform appended). Default: false.

#include <cstdint>
#include <vector>

#include "pq/core/time.h"

namespace pq::views {

struct TransportLoop {
  Ticks start = 0;
  Ticks end = 0;
  bool enabled = false;
  friend constexpr bool operator==(const TransportLoop&, const TransportLoop&) noexcept = default;
};

struct PlayheadSnapshot {
  bool playing = false;
  Ticks tick = 0;          // playing: the current position; stopped: where continuePlayback() resumes
  uint64_t sequence = 0;   // increases with every new report, so a view can skip unchanged snapshots
  // T6.6: a count-in runs (playing is true, tick is where playback will start): beat countInBeat (0-based) of
  // countInBeats, countInBeatsPerBar per bar.
  bool countingIn = false;
  uint32_t countInBeat = 0, countInBeats = 0, countInBeatsPerBar = 0;
  bool recording = false;  // T6.7: the transport records (Record pressed, until Stop / Record again)
  friend constexpr bool operator==(const PlayheadSnapshot&, const PlayheadSnapshot&) noexcept = default;
};

// T6.7: a recording session as the arrangement draws it (plain data; ticks).
struct RecordingOverlay {
  uint64_t revision = 0;             // bumps whenever something below changed
  uint32_t session = 0;              // 0: none
  bool active = false;               // the session still records
  struct Region {                    // one pass on one track
    uint32_t trackId = 0;
    Ticks start = 0, end = 0;        // what it covers so far
    // Audio: the waveform: pairs (min, max) [peak0, peak0 + peakCount) of peaks[peakList] cover [audioStart, end).
    bool audio = false;
    Ticks audioStart = 0;
    uint32_t peakList = 0, peak0 = 0, peakCount = 0;
  };
  std::vector<Region> regions;
  std::vector<std::vector<float>> peaks;  // per recorded audio track: min, max per bucket of its file
  struct Note {
    uint32_t trackId = 0;
    Ticks start = 0, length = 0;     // a held note grows to the current end
    double hz = 0.0;
    float velocity = 0.0f;
  };
  std::vector<Note> notes;
};

class TransportPort {
public:
  virtual ~TransportPort() = default;
  virtual void play(Ticks from) = 0;
  virtual void stop() = 0;
  virtual void continuePlayback() = 0;
  virtual void locate(Ticks tick) = 0;
  virtual void setLoop(const TransportLoop& loop) = 0;
  virtual PlayheadSnapshot playhead() = 0;
  virtual void auditionHz(float hz, float velocity, float durationMs) = 0;
  virtual void releaseAuditions() = 0;
  virtual void liveKey(int32_t equave, int32_t step, float velocity, bool on) {
    (void)equave, (void)step, (void)velocity, (void)on;
  }
  virtual void record(Ticks from) { play(from); }
  virtual bool captureMidi(Ticks insertMarker) {
    (void)insertMarker;
    return false;
  }
  virtual double outputLatencyMs() { return 0.0; }
  virtual uint32_t audioInputChannels() { return 0; }
  virtual bool recordingOverlay(RecordingOverlay& out) {
    (void)out;
    return false;
  }
  virtual double trackLatencyMs(uint32_t trackId) {
    (void)trackId;
    return 0.0;
  }
};

}  // namespace pq::views
