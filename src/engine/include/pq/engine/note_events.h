#pragma once

// Note events of one engine block (T1.8, SPEC §7.1 "event" rate). Producers — the timeline sequencer (T3.16),
// MIDI input, JI objects (§5), `pqdaw --play`'s test sequence — push them into an EventBuffer on the audio thread
// before the block; the VoiceEngine (voices.h) turns them into voice allocations and per-sample voice.* buffers.
// The ProgramImage itself never reads events (ProcessContext::events is informational for it).
//
// Contract of an EventList handed to VoiceEngine::process:
//   - events[i].frame is the sample offset inside the block (an event at frame k takes effect at sample k);
//     frames ≥ the block length are clamped to the last frame (counted in VoiceStats::lateEvents);
//   - sorted by frame, ties in push order (EventBuffer::push keeps this order).
//
// Pitch: every note is float32 Hz, resolved upstream from its pitch identity and tuning (SPEC §3.1). The
// identity-derived per-voice values (voice.ratio, voice.harmonic, unison count/index) travel with the note-on.
// Expression: a Pitch event moves a sounding note to a new Hz, as a step or as a glide that is linear in cents
// (exponential in Hz) over rampFrames samples; T2.6 (tuning as signal) and glides/pitch curves drive voice.hz
// per sample through it.

#include <cstdint>

#include "pq/core/aligned_buffer.h"

namespace pq::engine {

enum class NoteEventKind : uint8_t {
  NoteOn,       // noteId, hz, velocity, ratio, harmonic, unisonCount, unisonIndex
  NoteOff,      // noteId, velocity = release velocity
  Pitch,        // noteId, hz = new Hz, rampFrames (0: step at `frame`)
  AllNotesOff,  // release every held voice (transport stop / locate)
  AllSoundOff,  // fast-fade every sounding voice (panic, project close); pending starts are dropped
};

struct NoteEvent {
  uint32_t frame = 0;             // sample offset in the block
  NoteEventKind kind = NoteEventKind::NoteOn;
  uint8_t pad_ = 0;
  uint16_t unisonCount = 1;       // NoteOn: voice.unisonCount (JI object voices at one pitch; SPEC §5)
  uint32_t noteId = 0;            // the producer's handle for this note (note-off and expression find it by id)
  float hz = 0.0f;                // NoteOn: initial Hz (> 0); Pitch: target Hz (> 0)
  float velocity = 0.0f;          // NoteOn: velocity 0..1; NoteOff: release velocity 0..1
  float ratio = 1.0f;             // NoteOn: voice.ratio (pitch as a ratio of its anchor; 1 if none)
  float harmonic = 0.0f;          // NoteOn: voice.harmonic (harmonic number of a Harmonic identity; 0 if none)
  uint16_t unisonIndex = 0;       // NoteOn: voice.index (index within its unison group)
  uint16_t pad2_ = 0;
  uint32_t rampFrames = 0;        // Pitch: glide length in samples (0 = step)
};
static_assert(sizeof(NoteEvent) == 36);

inline NoteEvent noteOn(uint32_t frame, uint32_t noteId, float hz, float velocity) noexcept {  // RT-safe
  NoteEvent e;
  e.frame = frame;
  e.kind = NoteEventKind::NoteOn;
  e.noteId = noteId;
  e.hz = hz;
  e.velocity = velocity;
  return e;
}
inline NoteEvent noteOff(uint32_t frame, uint32_t noteId, float releaseVelocity = 0.5f) noexcept {  // RT-safe
  NoteEvent e;
  e.frame = frame;
  e.kind = NoteEventKind::NoteOff;
  e.noteId = noteId;
  e.velocity = releaseVelocity;
  return e;
}
inline NoteEvent notePitch(uint32_t frame, uint32_t noteId, float hz, uint32_t rampFrames = 0) noexcept {  // RT-safe
  NoteEvent e;
  e.frame = frame;
  e.kind = NoteEventKind::Pitch;
  e.noteId = noteId;
  e.hz = hz;
  e.rampFrames = rampFrames;
  return e;
}

// A view of one block's events (what ProcessContext::events points at).
struct EventList {
  const NoteEvent* events = nullptr;
  uint32_t count = 0;
};

// Preallocated event storage for one block. Construct off the audio thread; push/clear are RT-safe.
class EventBuffer {
public:
  EventBuffer() = default;
  explicit EventBuffer(uint32_t capacity) : storage_(capacity), capacity_(capacity) { list_.events = storage_.data(); }

  // Inserts keeping the list sorted by frame (after every event with frame ≤ e.frame): O(1) for events pushed in
  // time order. Returns false (and counts the drop) when full. RT-safe.
  bool push(const NoteEvent& e) noexcept {
    if (list_.count >= capacity_) {
      ++dropped_;
      return false;
    }
    NoteEvent* ev = storage_.data();
    uint32_t i = list_.count;
    while (i > 0 && ev[i - 1].frame > e.frame) {
      ev[i] = ev[i - 1];
      --i;
    }
    ev[i] = e;
    ++list_.count;
    return true;
  }
  void clear() noexcept { list_.count = 0; }  // RT-safe

  const EventList& list() const noexcept { return list_; }
  uint32_t size() const noexcept { return list_.count; }
  uint32_t capacity() const noexcept { return capacity_; }
  uint64_t dropped() const noexcept { return dropped_; }

private:
  AlignedBuffer<NoteEvent> storage_;
  uint32_t capacity_ = 0;
  EventList list_;
  uint64_t dropped_ = 0;
};

}  // namespace pq::engine
