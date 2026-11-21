#pragma once

// AP.7 (docs/plans/AP.md "Roll preview + Flatten"): what a track's note devices (the Arpeggiator) GENERATE from its
// clips' notes, as document notes — the roll draws them as preview ghosts, Flatten to clip writes them. The notes are
// computed offline by the app (pq/app/note_stage_render.h: the clip's notes through the track's note-stage chain, the
// engine's own code, so they are what playback plays); this header holds the result type and the edit.
//
//   identity   EXACT where the stage's pitch rule is (engine ArpExact): a Degree shift of a Degree source is that
//              Degree; a ratio r (Harmonics, Ratio, Harmonic Walk, Layers) of a Ratio source is a Ratio r · its ratio
//              to the SAME anchor (a JI member stays a member), of any other source a Ratio r anchored to the SOURCE
//              NOTE (AnchorKind::Note); the source's own identity when it plays unshifted. Anything else (a degree
//              shift of a walked / layered note, a ratio too large) is Cents of its Hz.
//   pitch      `curve`: what it glides through (the arp's Into / Out glides, the source's own bends carried over, a
//              tie's glide), cents relative to the identity's pitch, note ticks; shaped glides sampled to lines.
//   velocity   the step's velocity × its gain (a layer's level: a document note has no gain of its own).
//
// Flatten (ops::flattenNoteStages, one undo step): in each listed clip the source notes are REMOVED — except those a
// generated note is anchored to, which stay as DEACTIVATED notes (kept, drawn grey, not played: the anchors of the
// exact ratios) —, the generated notes and their curves are added, and the track's active note devices are switched
// off (deactivated, not deleted), so the clip plays what the devices played. Library-free; not RT code.

#include <span>
#include <vector>

#include "pq/core/time.h"
#include "pq/document/edit_ops.h"
#include "pq/document/model.h"

namespace pq::doc {

struct GeneratedNote {
  Ticks start = 0;                // clip content ticks
  Ticks length = 1;               // ≥ 1
  tuning::PitchId pitch;          // its identity (see above)
  float velocity = 100.0f;        // (0, 127]
  NoteId source;                  // the clip note that generated it
  double hz = 0.0;                // what the identity sounds at (the preview's row)
  bool exact = true;              // false: `pitch` is Cents of its Hz (no exact identity)
  std::vector<Breakpoint> curve;  // note ticks → cents relative to `hz` (ids unused); empty: none
  friend bool operator==(const GeneratedNote&, const GeneratedNote&) = default;
};

struct ClipGenerated {
  ClipId clip;
  std::vector<GeneratedNote> notes;  // by start
};

namespace ops {

// Flatten to clip (see above). Errors: the track is not a MIDI track, a clip is not one of its clips, a clip loops
// (its generated notes do not repeat with the loop: switch the loop off first). An empty list only switches the devices
// off.
EditResult flattenNoteStages(const Project& p, IdSource& ids, TrackId track, std::span<const ClipGenerated> clips);

}  // namespace ops

}  // namespace pq::doc
