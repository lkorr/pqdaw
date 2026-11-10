#pragma once

// Capture MIDI (T6.9; docs/plans/M6.md "### T6.9"): the clips Capture MIDI (Ctrl+Shift+C, the control bar's button)
// makes from what the listening tracks played without recording. The engine writes every note a listening track plays
// into the track's engine::CaptureRing (pq/engine/live_input.h "Capture MIDI"; the playback link owns the rings and
// reads them, pq/app/project_link.h "Capture MIDI"); planCapture turns those notes into doc::ops::CapturedClip's.
//
// Per track, the notes considered are those after the track's last capture and within the last 10 minutes
// (engine::kCaptureSeconds). Their last PHRASE is taken: the notes after the last silence of at least 2 bars (at the
// song tempo) between a note's end and the next note-on.
//   Playing (or stopped, when the whole phrase was played while the transport ran): only the notes played while it ran;
//   each note lands on the song tick it was played against — exactly where Record would put it: the record tap's law
//   (engine time − live latency − output latency, mapped to the song, minus the PDC total; pq/app/project_link.h
//   "Recording"), a loop wrap unwound. The clip spans the phrase's bars, not looped.
//   Stopped: the tempo is inferred from the phrase's onsets (every track's) by media::estimateTempo (the IOI core of
//   tempo detection; velocities weight the onsets); it becomes the song tempo only when the project has no clips, else
//   the song tempo stays and the notes keep their real time at it. The first note-on (of all tracks) is the clip's first
//   downbeat; the clip goes to the bar of the insert marker, looped, its length the phrase rounded to bars
//   (round((last onset − first onset + one beat) / bar), at least 1).
// Identities: what the ring kept at the note-on (doc::playedIdentity: Keys the key's Degree; Hz its frozen Cents plus
// the note-on bend, as Record does; never the tuning or the track's mode as they are at Capture). A note still held
// ends at `now`.

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "pq/core/time.h"
#include "pq/document/edit_ops.h"
#include "pq/document/input_map.h"
#include "pq/engine/live_input.h"

namespace pq::app {

struct CaptureTiming {
  double sampleRate = 48000.0;
  int64_t now = 0;           // engine time now (LiveInput::engineNow): where a held note ends
  int64_t noteBack = 0;      // live latency + output latency (frames): a note was heard against the song this earlier
  int64_t pdc = 0;           // the master's PDC total (frames)
  bool playing = false;      // the transport runs
  Ticks insertMarker = 0;    // stopped: the clip goes to this tick's bar
  bool loop = false;         // the arrangement loop is on (its wrap is unwound)
  Ticks loopStart = 0, loopEnd = 0;
};

struct CaptureTrack {
  doc::TrackId track;
  std::span<const engine::CapturedNote> notes;  // in sequence order (CaptureRing::read)
};

struct CapturePlan {
  std::vector<doc::ops::CapturedClip> clips;  // empty: nothing to capture
  std::optional<double> bpm;                  // the song tempo to set first (stopped, a project without clips)
  double inferredBpm = 0.0;                   // stopped: what the onsets said (0: too few)
};

// Not RT code (UI thread).
CapturePlan planCapture(const doc::Project& p, std::span<const CaptureTrack> tracks, const CaptureTiming& t);

}  // namespace pq::app
