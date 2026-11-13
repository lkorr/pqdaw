#pragma once

// MIDI file export (MF.3, docs/plans/DY.md "MF.3"): note clips → Standard MIDI File bytes (format 1), built with MF.1's
// MidiTrack builders and writeMidiFile. No file I/O (the caller writes the bytes).
//
//   Tracks  track 0: the tempo map and time signatures from the earliest clip's start (file tick 0) to the last clip's
//           end; then one MTrk per clip (in the order given), named after the clip.
//   Notes   each clip as it plays over its arrangement length (pq/document/clip_time.h: the loop unrolled, notes cut at
//           the pass end); disabled notes skipped; the stored velocity / release velocity; pitch = the note's static Hz
//           in its clip's effective tuning (resolveClipPitches: what resolveTrackPitches gives). Ticks: round(t · division
//           / kPpq) from the origin (exact for kPpq's divisors; 960: every multiple of 231 ticks); a note keeps ≥ 1 tick.
//   Tempo   a Step point is one Set Tempo; a Linear ramp is stepped every sixteenth note at each step's mean tempo (the
//           step's duration is kept to the µs). Signatures whose denominator is not a power of two ≤ 128 or whose
//           numerator is over 255 cannot be written (skipped, counted).
//   Modes   Mpe        lower zone: MCM (RPN 6 = 15 on channel 1), members 2–16 at ±48 semitones (RPN 0 on each member,
//                      every MTrk at tick 0); each note on a free member channel (round-robin, shared by every clip of
//                      the file: one port) with the bend to its exact pitch before its note-on (14 bits at ±48: steps of
//                      0.586 ¢, error ≤ 0.293 ¢). More than 15 at once: the channel whose note started first is reused,
//                      that note ending there (counted: `stolen`).
//           TwelveEdo  the nearest 12-TET key (key 69 = 440 Hz), channel 1, no bends.
//           Degrees    key = 60 + the note's absolute degree in its clip's tuning (equave · size + degree), or the
//                      lowest key the tuning's .kbm maps to that degree when it carries one; channel 1. A note between
//                      degrees takes the nearest one (counted: `snapped`). Round-trips with a Keys import (MF.2) into a
//                      track of the same tuning (key map Tuning / Consecutive).
//   A note whose key falls outside 0 … 127 (or a degree no .kbm key plays) is skipped (`outOfRange`); an unresolvable
//   pitch (pitch_resolve.h: a missing anchor, a Harmonic identity) is skipped (`unresolved`).
//
// Cost: linear in the played notes plus a sort (O(n log n)); MPE allocation O(n · 15). Not RT code (UI thread).

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pq/core/result.h"
#include "pq/document/commands.h"
#include "pq/document/model.h"

namespace pq::doc {

enum class MidiExportMode : uint8_t { Mpe, TwelveEdo, Degrees };
inline constexpr std::array<std::string_view, 3> kMidiExportModeNames = {"MPE", "12edo", "Degrees"};

struct ExportMidiOptions {
  MidiExportMode mode = MidiExportMode::Mpe;
  uint16_t division = 960;  // ticks per quarter note (1 … 32767)
};

inline constexpr uint8_t kMpeMembers = 15;         // lower zone: channels 2–16 (0-based 1 … 15)
inline constexpr uint8_t kMpeBendSemitones = 48;  // the MPE default member range, sent as RPN 0

// What the export wrote and what it could not (the status line's).
struct ExportMidiReport {
  uint32_t clips = 0;
  uint32_t notes = 0;        // notes written
  uint32_t unresolved = 0;   // skipped: no pitch
  uint32_t outOfRange = 0;   // skipped: key outside 0 … 127 (Degrees: a degree no key plays)
  uint32_t snapped = 0;      // Degrees: notes between degrees, moved to the nearest
  uint32_t stolen = 0;       // MPE: notes ended early because > 15 sounded at once
  uint32_t maxVoices = 0;    // MPE: the most notes sounding at once (before stealing)
  uint32_t tempoEvents = 0;
  uint32_t signatures = 0;
  uint32_t skippedSignatures = 0;
};

// The SMF bytes of `clips` (note clips of tracks' main lanes). Errors: no clips, a missing clip (NotFound), an audio clip
// or a division outside 1 … 32767 (InvalidArgument).
Result<std::vector<uint8_t>> exportMidi(const Project& p, std::span<const ClipRef> clips,
                                        const ExportMidiOptions& options = {}, ExportMidiReport* report = nullptr);

// The status line's text for a finished export: "Exported 12 notes (MPE)" plus what was skipped / stolen.
std::string describeMidiExport(const ExportMidiReport& r, MidiExportMode mode);

}  // namespace pq::doc
