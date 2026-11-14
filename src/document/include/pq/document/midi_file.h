#pragma once

// Standard MIDI Files (MF.1, docs/plans/DY.md "MF.1"): bytes in, bytes out; no file I/O (the caller reads / writes).
//
// parseMidiFile  .mid / .midi / RIFF RMID bytes → MidiFile: the header (format, division) and every MTrk as absolute-
//                tick events (running status expanded, sysex and meta payloads kept raw). Bounds-checked everywhere:
//                truncated or malformed input is an Error, never a crash; unknown chunks are skipped. Linear time.
// writeMidiFile  MidiFile → SMF bytes (running status on, one End of Track per track): what MF.3's export builds.
// resolveNotes   MidiFile → MidiNotes: paired notes with their microtonal offset (pitch bend at note-on through RPN 0 /
//                MPE ranges, MTS tunings), tempo and time-signature lists: what MF.2's import maps to clips.
//
// Ticks here are FILE ticks (MidiDivision), never pq::Ticks; MF.2 converts.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pq/core/result.h"

namespace pq::doc {

// MThd's division: ticks per quarter (PPQ) or SMPTE frames per second × ticks per frame.
struct MidiDivision {
  uint16_t ppq = 480;         // ticks per quarter note (when smpteFps == 0); 1 … 32767
  uint8_t smpteFps = 0;       // 0: PPQ; else 24, 25, 29 (30 drop-frame = 29.97) or 30
  uint8_t ticksPerFrame = 0;  // SMPTE: ticks per frame (1 … 255)

  bool isSmpte() const noexcept { return smpteFps != 0; }
  // SMPTE: file ticks per second (29 → 29.97 fps). 0 for PPQ.
  double ticksPerSecond() const noexcept {
    if (!isSmpte()) return 0.0;
    return (smpteFps == 29 ? 30000.0 / 1001.0 : double(smpteFps)) * double(ticksPerFrame);
  }
  bool operator==(const MidiDivision&) const = default;
};

// Meta event types the reader and resolveNotes understand (any other type is kept as a raw event).
inline constexpr uint8_t kMetaTrackName = 0x03;
inline constexpr uint8_t kMetaEndOfTrack = 0x2F;  // never stored as an event: MidiTrack::endTick
inline constexpr uint8_t kMetaTempo = 0x51;
inline constexpr uint8_t kMetaTimeSignature = 0x58;

// One event at an absolute file tick.
//   status 0x80 … 0xEF  a channel message (channel = status & 15); data1 / data2 its data bytes (data2 = 0 for the
//                       one-byte messages 0xC0 / 0xD0)
//   status 0xF0 / 0xF7  a sysex packet (F0: start; F7: continuation or escape); payload = the bytes after the length
//                       (an F0 packet's payload normally ends with F7)
//   status 0xFF         a meta event of type `type`; payload = its data
struct MidiEvent {
  int64_t tick = 0;
  uint8_t status = 0;
  uint8_t type = 0;
  uint8_t data1 = 0, data2 = 0;
  uint32_t offset = 0, size = 0;  // the payload: MidiTrack::bytes[offset, offset + size)

  bool isChannel() const noexcept { return status >= 0x80 && status < 0xF0; }
  bool isSysex() const noexcept { return status == 0xF0 || status == 0xF7; }
  bool isMeta() const noexcept { return status == 0xFF; }
  uint8_t channel() const noexcept { return status & 15u; }
  uint8_t kind() const noexcept { return status & 0xF0u; }  // 0x80 note-off … 0xE0 pitch bend
  bool operator==(const MidiEvent&) const = default;
};

struct MidiTrack {
  std::vector<MidiEvent> events;  // by tick (the reader's order; writeMidiFile stable-sorts by tick)
  std::vector<uint8_t> bytes;     // sysex / meta payloads (MidiEvent::offset / size)
  int64_t endTick = 0;            // End of Track's tick (≥ the last event's tick)

  std::span<const uint8_t> payload(const MidiEvent& e) const noexcept {
    return std::span<const uint8_t>(bytes).subspan(e.offset, e.size);
  }
  // Builders (append; ticks need not be in order: the writer sorts, stable).
  void channel(int64_t tick, uint8_t status, uint8_t data1, uint8_t data2 = 0);
  void noteOn(int64_t tick, uint8_t ch, uint8_t key, uint8_t velocity) {
    channel(tick, uint8_t(0x90 | (ch & 15)), key, velocity);
  }
  void noteOff(int64_t tick, uint8_t ch, uint8_t key, uint8_t velocity = 64) {
    channel(tick, uint8_t(0x80 | (ch & 15)), key, velocity);
  }
  void controller(int64_t tick, uint8_t ch, uint8_t cc, uint8_t value) {
    channel(tick, uint8_t(0xB0 | (ch & 15)), cc, value);
  }
  void pitchBend(int64_t tick, uint8_t ch, uint16_t value14) {  // 8192 = centre
    channel(tick, uint8_t(0xE0 | (ch & 15)), uint8_t(value14 & 127), uint8_t((value14 >> 7) & 127));
  }
  // RPN `number` (MSB << 7 | LSB) set to data MSB / LSB (CC 101, 100, 6, 38), then RPN null (127 / 127).
  void rpn(int64_t tick, uint8_t ch, uint16_t number, uint8_t msb, uint8_t lsb = 0);
  void meta(int64_t tick, uint8_t type, std::span<const uint8_t> data);
  void sysex(int64_t tick, uint8_t status, std::span<const uint8_t> data);  // status F0 / F7; data as stored
  void tempo(int64_t tick, uint32_t usPerQuarter);
  // Numerator, denominator (a power of two), MIDI clocks per metronome click, 32nds per quarter.
  void timeSignature(int64_t tick, uint8_t numerator, uint8_t denominator, uint8_t clocks = 24, uint8_t n32 = 8);
  void name(std::string_view text, int64_t tick = 0);

  bool operator==(const MidiTrack&) const = default;
};

struct MidiFile {
  uint16_t format = 1;  // 0: one track; 1: simultaneous tracks; 2: independent sequences
  MidiDivision division;
  std::vector<MidiTrack> tracks;
  bool operator==(const MidiFile&) const = default;
};

// Errors (ErrorCode::Parse, the message names the byte offset): not an SMF / RMID, a chunk or event past the end, a
// variable-length number over 4 bytes, a data byte where a status byte is needed (no running status), a status byte
// inside a channel message's data, a system message (F1 … FE) in a track, fewer MTrk chunks than MThd declares, a bad
// division / format. Bytes after End of Track inside its chunk are ignored; chunks other than MThd / MTrk are skipped.
Result<MidiFile> parseMidiFile(std::span<const uint8_t> bytes);
// A format-`file.format` SMF: each track's events stable-sorted by tick, running status for channel messages, End of
// Track at max(endTick, last event). Any End of Track meta in `events` is dropped (endTick is the track's end).
std::vector<uint8_t> writeMidiFile(const MidiFile& file);

// ── Notes ─────────────────────────────────────────────────────────────────────────────────────────────────────────────

struct MidiNote {
  int64_t tick = 0;      // note-on (file ticks)
  int64_t length = 0;    // ≥ 0; to the note-off, the sustain release, or the track's end
  uint8_t key = 0;       // 0 … 127
  uint8_t channel = 0;   // 0 … 15
  uint8_t velocity = 0;  // 1 … 127
  uint8_t releaseVelocity = 64;  // the note-off's (64 for a velocity-0 note-on, an all-notes-off or an unpaired note)
  uint16_t track = 0;    // index into MidiFile::tracks
  double cents = 0.0;    // offset from the key's 12-TET pitch: MTS tuning + pitch bend at note-on (+ the MPE master's)
  bool operator==(const MidiNote&) const = default;
};

struct MidiTempo {
  int64_t tick = 0;
  uint32_t usPerQuarter = 500000;
  double bpm() const noexcept { return 60'000'000.0 / double(usPerQuarter); }
  bool operator==(const MidiTempo&) const = default;
};

struct MidiTimeSignature {
  int64_t tick = 0;
  uint8_t numerator = 4;
  uint16_t denominator = 4;  // the actual value (2^dd of the meta)
  bool operator==(const MidiTimeSignature&) const = default;
};

struct MidiNotes {
  std::vector<MidiNote> notes;  // by (tick, track, then file order)
  std::vector<MidiTempo> tempos;  // by tick; one per tick (the last in file order wins); empty: none in the file
  std::vector<MidiTimeSignature> signatures;  // as tempos
  std::vector<std::string> trackNames;  // per track: its first Track Name meta ("" when none)
  std::vector<uint32_t> trackNotes;     // per track: its note count
  int64_t endTick = 0;                  // the longest track's end
  // What a note list cannot hold (MF.2's status line): control changes other than sustain / RPN / NRPN / data entry /
  // all-notes-off / reset; poly and channel pressure; pitch bends that change the bend under a sounding note.
  uint32_t droppedControllers = 0;
  uint32_t droppedPressure = 0;
  uint32_t midNoteBends = 0;
  // AU.dsp (RB-DY M8, Q263): the file speaks in absolute pitch — it holds a pitch bend, an MTS message or an RPN tuning
  // (RPN 0 bend range, 3 tuning program, 4 tuning bank) anywhere. The import then reads EVERY note of it as its 12-TET
  // key + cents, not through the track's key map (pq/document/midi_import.h).
  bool absolutePitch = false;
};

// Notes of `file`. Channel state (bend, ranges, sustain, MPE zones, tuning programs) is shared by every track in
// formats 0 / 1 (the tracks are merged in time order, ties by track index, as one port plays them) and per track in
// format 2. Pairing: per (track, channel, key) first in, first out; a note-on with velocity 0 is a note-off.
// Sustain (CC 64 ≥ 64) holds released notes to the pedal's release (or a new note-on of the same key: the held one ends
// there). Unpaired notes (and notes held at the end) end at their track's end.
// Pitch: cents = the key's tuning offset + the channel's bend × its range (+ the zone master's bend × its range on an
// MPE member channel), all at note-on.
//   Bend range: RPN 0 (data entry MSB semitones, LSB cents; default ±2 semitones). MPE (RPN 6 on channel 1 / 16 = an
//   MCM, data MSB = member count): member channels ±48 semitones, the master ±2; RPN 0 on a member sets every member
//   of its zone; a zone shrinks an overlapping one; a count of 0 ends the zone (members back to ±2).
//   Tuning (MTS): single-note tuning change real-time 08 02 (program) / 08 07 (bank, program; real-time or not), bulk
//   dump 08 01 (program) / 08 04 (bank, program), scale / octave 1-byte 08 08 and 2-byte 08 09 (a channel mask).
//   A channel plays the key table of its tuning program (RPN 3 program, RPN 4 bank; default 0 / 0) plus its scale /
//   octave offsets. Frequency data 7F 7F 7F = no change. Device ids are not checked. A sysex split into F0 + F7
//   packets is joined before it is read.
MidiNotes resolveNotes(const MidiFile& file);

}  // namespace pq::doc
