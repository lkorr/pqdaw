#pragma once

// The note sequence the timeline plays (T3.16): plain-data descriptions (filled by T3.17's lowering from the
// document, without the engine depending on `document`) and the immutable SequenceImage built from them off the
// audio thread. The Timeline (timeline.h) publishes images by atomic swap and walks them with per-track cursors.
//
// ---- Clip semantics (the same as pq/document/clip_time.h) ------------------------------------------------------
// A clip plays content from `contentStart` at arrangement tick `start`, for `length` ticks. With the loop off,
// content tick contentStart + o plays at start + o. With the loop on (loopEnabled and loopEnd > loopStart), playback
// runs from contentStart up to loopEnd and then repeats [loopStart, loopEnd) until the clip ends; a start marker at
// or after loopEnd is folded into the loop. Each run over the content is a PASS (pass 0 = the first, then one per
// loop repetition). A note sounds in a pass if its start lies in the pass's content range; it is cut at the end of
// the pass (the loop wrap or the clip end), as in Ableton. Notes that can never play (before the start marker,
// outside the loop) are dropped at build time. Disabled notes and inactive clips (`0`, deactivated: SPEC §9.3, Q15)
// emit nothing; muting a track is a gain param elsewhere (Q15), the sequencer keeps running.
//
// Clips on one track must not overlap (the document guarantees it, Q41); if they do, the later-starting clip wins
// and the earlier one is cut where it begins (equal starts: the later one in the description wins).
//
// ---- Chance and velocity range ----------------------------------------------------------------------------------
// Deterministic per note and pass (noteRandom): u1, u2 = uniform [0, 1) from (seed, note id, pass). The note plays
// iff u1 < chance; it plays with velocity clamp(velocity + u2 · velocityRange, 1, 127). The same note in the same
// pass always decides the same way (offline renders are reproducible); a new seed rerolls.
//
// ---- Pitch -------------------------------------------------------------------------------------------------------
// Notes carry resolved float32 Hz (SPEC §3.1) plus voice.ratio / voice.harmonic. PitchSource is tuning as a signal
// (T2.6): a nonzero `tuning` marks a Degree note of a tuning signal; its note-on carries degree/equave as
// voice.degree / voice.equave (note_events.h), from which the track's tuning node (`tuning_hz` + `tune_voices`)
// derives the voice's Hz per sample. `hz` stays the static pitch (and the fallback where no tuning node is bound).

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "pq/core/result.h"
#include "pq/core/time.h"

namespace pq::engine {

// T2.6 (see above). All zero: the note plays its static hz.
struct PitchSource {
  uint32_t tuning = 0;   // 0 = static Hz; otherwise the id of the tuning signal the degree refers to (T2.6)
  int32_t degree = 0;    // the note's degree and equave in that tuning (Degree identities)
  int32_t equave = 0;
};

struct NoteDesc {
  Ticks start = 0;                // content ticks (relative to the clip's content origin, as doc::Note)
  Ticks length = kPpq;            // ≥ 1
  uint32_t id = 0;                // stable note id (doc::NoteId): the RNG key and the identity across image swaps
  float hz = 261.626f;            // resolved pitch, finite and > 0
  float velocity = 100.0f;        // (0, 127]
  float releaseVelocity = 64.0f;  // [0, 127]
  float chance = 1.0f;            // [0, 1] probability of playing (per pass)
  float velocityRange = 0.0f;     // [−127, 127] random velocity deviation (see above)
  float ratio = 1.0f;             // voice.ratio
  float harmonic = 0.0f;          // voice.harmonic
  PitchSource pitch;
  bool enabled = true;            // disabled notes are kept in the document but never play
};

struct ClipDesc {
  uint32_t id = 0;                // stable clip id (diagnostics only)
  Ticks start = 0;                // arrangement ticks
  Ticks length = kTicksPerWhole;  // ≥ 1
  Ticks contentStart = 0;         // content tick that plays at `start` (start marker)
  bool loopEnabled = false;
  Ticks loopStart = 0;            // content ticks; the loop is on iff loopEnabled && loopEnd > loopStart
  Ticks loopEnd = kTicksPerWhole;
  bool active = true;             // deactivated clips emit nothing
  std::vector<NoteDesc> notes;    // any order (sorted by (start, id) at build time)
};

struct TrackSequenceDesc {
  uint32_t id = 0;                // stable track id: sounding notes follow it across image swaps
  std::vector<ClipDesc> clips;    // any order
};

struct SequenceBuildOptions {
  uint64_t seed = 0;              // chance / velocity-range seed (noteRandom)
};

// Two independent uniforms in [0, 1) for (seed, note id, pass). RT-safe.
struct NoteRandom {
  float chance;
  float velocity;
};
NoteRandom noteRandom(uint64_t seed, uint32_t noteId, int64_t pass) noexcept;

class SequenceImage {
public:
  // A note as stored: 64 bytes, one cache line.
  struct Note {
    Ticks start;            // content ticks
    Ticks length;
    uint32_t id;
    float hz;
    float velocity, releaseVelocity, chance, velocityRange;
    float ratio, harmonic;
    PitchSource pitch;
    uint32_t pad_;
  };
  static_assert(sizeof(Note) == 64);

  // A clip as stored (after trimming overlaps). Arrangement ticks unless noted.
  struct Clip {
    Ticks start, end;       // [start, end), end > start
    Ticks content0;         // content tick playing at `start` (start marker, folded into the loop)
    Ticks firstPassEnd;     // where pass 0 ends: the first loop wrap, or `end` (min of both)
    Ticks loopStart;        // content ticks
    Ticks loopLen;          // 0: not looped
    uint32_t pass0Begin, pass0End;  // note index range (into notes()) of pass 0: start ∈ [content0, pass-0 end)
    uint32_t loopBegin, loopEnd;    // note index range of the later passes: start ∈ [loopStart, loopStart + loopLen)
    uint32_t noteBegin, noteEnd;    // every stored note of the clip, sorted by (start, id)
    uint32_t id;
    uint32_t pad_;
  };

  struct Track {
    uint32_t id;
    uint32_t clipBegin, clipEnd;    // into clips(), sorted by start (so also by end)
  };

  // Off the audio thread (allocates). Errors: a clip with length < 1, a note with length < 1 or a non-finite /
  // non-positive Hz, a non-finite velocity / chance, more than 2^32 − 1 notes.
  static Result<std::unique_ptr<SequenceImage>> build(std::span<const TrackSequenceDesc> tracks,
                                                      const SequenceBuildOptions& options = {});

  // RT-safe accessors.
  std::span<const Track> tracks() const noexcept { return tracks_; }
  std::span<const Clip> clips() const noexcept { return clips_; }
  std::span<const Note> notes() const noexcept { return notes_; }
  uint32_t numTracks() const noexcept { return static_cast<uint32_t>(tracks_.size()); }
  uint64_t seed() const noexcept { return seed_; }

  // The occurrence of note `noteId` that starts at arrangement tick `on` on track `track`: its stored note and the
  // end of the pass it plays in (the note-off tick is min(on + length, passEnd)). Null when there is none (the note
  // was removed, moved, or its clip deactivated). O(log clips + log notes). RT-safe.
  const Note* findOccurrence(uint32_t track, uint32_t noteId, Ticks on, Ticks* passEnd) const noexcept;

private:
  std::vector<Track> tracks_;
  std::vector<Clip> clips_;
  std::vector<Note> notes_;
  uint64_t seed_ = 0;
};

}  // namespace pq::engine
