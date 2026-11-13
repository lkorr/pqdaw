#pragma once

// MIDI file import (MF.2, docs/plans/DY.md "MF.2", "Core-structure decisions" 4): resolved notes of a Standard MIDI File
// (pq/document/midi_file.h resolveNotes) → note clips, ONE undo step built on the Capture MIDI path (ops::captureClip).
//
//   Ticks   file tick t → round(t · kPpq / ppq), exact int64. SMPTE files: t / ticksPerSecond seconds → ticks through
//           the file's tempo map when it is imported, else a constant 120 BPM (the SMF default).
//   Tracks  format 0: ONE track (every channel of the file: channel-per-note microtonal files stay whole); formats 1 / 2:
//           one MIDI track per MTrk that has notes, named after its Track Name. The first lands on the drop's MIDI
//           track (`target`) when there is one, the rest (or all) on new MIDI tracks (default instrument) after
//           `newTracksAfter`, each after the previous. One clip per track from `at` (file tick 0) to its last note-off,
//           its end rounded up to a bar line (not looped).
//   Pitch   a plain key (cents == 0) stores what live input on that track gives (its InputPitchMap: Keys → a Degree,
//           Hz → a frozen Cents); a bent or MTS-retuned key (and a key the track's .kbm leaves unmapped) is its
//           absolute pitch, 12-TET(key) · 2^(cents / 1200): a Degree in Keys mode when within kMidiDegreeSnapCents of
//           the track's nearest degree, else Cents (frozen against the track's reference).
//   Tempo   the file's tempo map and time signatures replace the project's from `at` on, only when the project has no
//   & sigs  clip (main or take lane: projectHasClips, the Capture MIDI bpm rule). A signature change off a bar line
//           moves to the next bar line (counted). Signatures the model cannot hold (denominator > 256) are skipped.
//
// Cost: linear in the notes (one pitch-map lookup each, a log2 for bent ones) plus one input map per target track
// (128 resolves); a 100k-note file imports in milliseconds. Not RT code (UI thread).

#include <cstdint>
#include <string>

#include "pq/document/edit_ops.h"
#include "pq/document/midi_file.h"

namespace pq::doc::ops {

inline constexpr double kMidiDegreeSnapCents = 0.5;

struct ImportMidiOptions {
  uint16_t format = 1;          // MidiFile::format
  MidiDivision division;        // MidiFile::division
  TrackId target;               // the drop's track: a MIDI track takes the first imported track (else ignored)
  Ticks at = 0;                 // where file tick 0 lands (≥ 0)
  TrackId newTracksAfter;       // new tracks follow it (invalid: at the end of the top level)
  std::string name;             // the file's name: new tracks / clips without a Track Name take it
};

// What the import did and what it could not keep (the status line's).
struct MidiImportReport {
  uint32_t tracks = 0;          // tracks that received a clip
  uint32_t newTracks = 0;
  uint32_t notes = 0;
  uint32_t bentNotes = 0;       // notes with a bend / MTS offset (cents != 0)
  uint32_t unmappedKeys = 0;    // plain keys the track's .kbm leaves unmapped: imported at their 12-TET pitch
  bool tempoImported = false;   // the tempo map was replaced (the file has tempo events, the project had no clips)
  bool signaturesImported = false;
  uint32_t movedSignatures = 0; // signature changes moved to the next bar line
  uint32_t skippedSignatures = 0;
  bool tempoKept = false;       // the file has tempo / signatures but the project has clips: not imported
  uint32_t droppedControllers = 0, droppedPressure = 0, midNoteBends = 0;  // MidiNotes' counts
};

// The import of `notes` (resolveNotes of a file with `opt.format` / `opt.division`). Errors: no notes, a non-MIDI
// `target` that is not ignorable (never: a non-MIDI target means new tracks), a track build failure.
EditResult importMidi(const Project& p, IdSource& ids, const MidiNotes& notes, const ImportMidiOptions& opt,
                      MidiImportReport* report = nullptr);

// File tick → document ticks for a PPQ division: round(t · kPpq / ppq), exact (no overflow for any int64 file tick a
// parsed file can hold).
Ticks midiPpqTicks(int64_t fileTick, uint16_t ppq) noexcept;

}  // namespace pq::doc::ops
