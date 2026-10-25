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
// (exponential in Hz) over rampFrames samples (glides, pitch curves).
// Tuning as a signal (T2.6): a note-on may carry its Degree identity (`degree`, `equave`; NaN = none) as
// voice.degree / voice.equave; a patch's tuning node (`tuning_hz` bound with `tune_voices`) then derives the
// voice's Hz per sample from the modulated tuning, and `hz` is only the fallback for lanes without a degree.
// T2.6-fix3: `tuning` (voice.tuning) names which of the track patch's tuning nodes the note follows (a reference-
// relative note has a tuning but no degree), and a Pitch event with kNoteEventRetune moves a held note to a new
// static Hz AND identity (a re-resolved note: the timeline's sequence swap retunes instead of cutting it).

#include <cstdint>
#include <limits>

#include "pq/core/aligned_buffer.h"

namespace pq::engine {

enum class NoteEventKind : uint8_t {
  NoteOn,       // noteId, hz, velocity, ratio, harmonic, unisonCount, unisonIndex
  NoteOff,      // noteId, velocity = release velocity
  Pitch,        // noteId, hz = new Hz, rampFrames (0: step at `frame`)
  AllNotesOff,  // release every held voice; deferred starts are dropped (transport stop / locate)
  AllSoundOff,  // fast-fade every sounding voice (panic, project close); pending starts are dropped
};

// NoteEvent::flags.
inline constexpr uint8_t kNoteEventRetune = 1;  // Pitch: also replace the note's identity (ratio, harmonic, degree,
                                                // equave, tuning): a held note re-resolved by an edit (T2.6-fix3)

struct NoteEvent {
  uint32_t frame = 0;             // sample offset in the block
  NoteEventKind kind = NoteEventKind::NoteOn;
  uint8_t flags = 0;              // kNoteEventRetune
  uint16_t unisonCount = 1;       // NoteOn: voice.unisonCount (JI object voices at one pitch; SPEC §5)
  uint32_t noteId = 0;            // the producer's handle for this note (note-off and expression find it by id)
  float hz = 0.0f;                // NoteOn: initial Hz (> 0); Pitch: target Hz (> 0)
  float velocity = 0.0f;          // NoteOn: velocity 0..1; NoteOff: release velocity 0..1
  float ratio = 1.0f;             // NoteOn: voice.ratio (pitch as a ratio of its anchor; 1 if none)
  float harmonic = 0.0f;          // NoteOn: voice.harmonic (harmonic number of a Harmonic identity; 0 if none)
  uint16_t unisonIndex = 0;       // NoteOn: voice.index (index within its unison group)
  uint16_t pad2_ = 0;
  uint32_t rampFrames = 0;        // Pitch: glide length in samples (0 = step)
  float degree = std::numeric_limits<float>::quiet_NaN();  // NoteOn: voice.degree (T2.6; NaN = no Degree identity)
  float equave = std::numeric_limits<float>::quiet_NaN();  // NoteOn: voice.equave
  float tuning = 0.0f;            // NoteOn: voice.tuning (T2.6-fix3: the tuning node the note follows; 0 = none)
};
static_assert(sizeof(NoteEvent) == 48);

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
//
// Overflow (a full buffer) never loses a note-off (M1 review M7: a lost NoteOff is a stuck note):
//   1. an incoming NoteOn is dropped;
//   2. otherwise the newest NoteOn in the buffer is dropped to make room (its note never starts);
//   3. an incoming Pitch is dropped; otherwise the newest Pitch in the buffer is dropped;
//   4. the buffer holds only offs (NoteOff / AllNotesOff / AllSoundOff) and another off arrives: if an All* event at
//      or before its frame already covers it, it is redundant (no NoteOn is in the block, so every held note is
//      released by it); otherwise the newest event and the incoming one are merged into one AllNotesOff (AllSoundOff
//      if either is one) at the earlier of their frames: notes may be released early, never left hanging.
// dropped() counts dropped NoteOn / Pitch events; mergedOffs() counts offs absorbed by rule 4.
class EventBuffer {
public:
  EventBuffer() = default;
  explicit EventBuffer(uint32_t capacity) : storage_(capacity), capacity_(capacity) { list_.events = storage_.data(); }

  // Inserts keeping the list sorted by frame (after every event with frame ≤ e.frame): O(1) for events pushed in
  // time order. Returns false when the event itself was dropped or merged (see above). RT-safe.
  bool push(const NoteEvent& e) noexcept {
    if (capacity_ == 0) {
      ++dropped_;
      return false;
    }
    if (list_.count >= capacity_) {
      if (e.kind == NoteEventKind::NoteOn) {
        ++dropped_;
        return false;
      }
      if (removeNewest(NoteEventKind::NoteOn)) {
        ++dropped_;
      } else if (e.kind == NoteEventKind::Pitch) {
        ++dropped_;
        return false;
      } else if (removeNewest(NoteEventKind::Pitch)) {
        ++dropped_;
      } else {
        mergeOff(e);
        return false;
      }
    }
    insert(e);
    return true;
  }
  void clear() noexcept { list_.count = 0; }  // RT-safe

  const EventList& list() const noexcept { return list_; }
  uint32_t size() const noexcept { return list_.count; }
  uint32_t capacity() const noexcept { return capacity_; }
  uint64_t dropped() const noexcept { return dropped_; }
  uint64_t mergedOffs() const noexcept { return merged_; }

private:
  void insert(const NoteEvent& e) noexcept {  // RT-safe; count < capacity
    NoteEvent* ev = storage_.data();
    uint32_t i = list_.count;
    while (i > 0 && ev[i - 1].frame > e.frame) {
      ev[i] = ev[i - 1];
      --i;
    }
    ev[i] = e;
    ++list_.count;
  }
  bool removeNewest(NoteEventKind k) noexcept {  // RT-safe: the last event of kind k (latest frame, latest push)
    NoteEvent* ev = storage_.data();
    for (uint32_t i = list_.count; i > 0; --i)
      if (ev[i - 1].kind == k) {
        for (uint32_t j = i; j < list_.count; ++j) ev[j - 1] = ev[j];
        --list_.count;
        return true;
      }
    return false;
  }
  void mergeOff(const NoteEvent& e) noexcept {  // RT-safe; the buffer is full of offs, e is an off
    ++merged_;
    NoteEvent* ev = storage_.data();
    const bool sound = e.kind == NoteEventKind::AllSoundOff;
    for (uint32_t i = 0; i < list_.count && ev[i].frame <= e.frame; ++i)  // already covered?
      if (ev[i].kind == NoteEventKind::AllSoundOff || (!sound && ev[i].kind == NoteEventKind::AllNotesOff)) return;
    NoteEvent last = ev[--list_.count];
    NoteEvent all;
    all.kind = sound || last.kind == NoteEventKind::AllSoundOff ? NoteEventKind::AllSoundOff : NoteEventKind::AllNotesOff;
    all.frame = last.frame < e.frame ? last.frame : e.frame;
    insert(all);
  }

  AlignedBuffer<NoteEvent> storage_;
  uint32_t capacity_ = 0;
  EventList list_;
  uint64_t dropped_ = 0;
  uint64_t merged_ = 0;
};

}  // namespace pq::engine
