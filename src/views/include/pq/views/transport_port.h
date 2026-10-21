#pragma once

// TransportPort (T3.7): how views start/stop playback and read the playhead without depending on the engine.
// `views` never includes `engine` (ARCHITECTURE §1); `app` implements this interface over the engine (T3.17) and,
// until then, over a wall-clock simulation (pq/app/sim_transport.h). Tests use FakeTransport
// (pq/views/fake_transport.h), which records every request.
//
// Plain data only: positions are int64 ticks (PPQ 27720), the loop is three numbers. The port is called on the UI
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

#include <cstdint>

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
  friend constexpr bool operator==(const PlayheadSnapshot&, const PlayheadSnapshot&) noexcept = default;
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
};

}  // namespace pq::views
