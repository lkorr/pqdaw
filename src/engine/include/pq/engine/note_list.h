#pragma once

// A fixed list of timed notes as an event source (T1.10): what `pqc render --notes` and golden renders play.
// Times are int64 samples on the engine timeline. Built off the audio thread (sorts and allocates); produce() is
// RT-safe: a cursor walks the prebuilt event list, so a block costs O(events in it) (plus a binary search when the
// timeline jumps backwards, e.g. a second render from 0).
//
// Order at equal times: note-offs before note-ons (a note ending where the next one starts never cuts the new one),
// then list order. Note ids are 1 + the note's index in the list. A note starting before 0 plays from 0 to its own
// end; one ending at or before 0 is left out.

#include <cstdint>
#include <vector>

#include "pq/engine/image_runner.h"
#include "pq/engine/note_events.h"

namespace pq::engine {

struct TimedNote {
  int64_t start = 0;          // samples
  int64_t length = 0;         // samples (≥ 1; the note-off is at start + length)
  float hz = 261.626f;        // float32 Hz, resolved upstream
  float velocity = 0.7f;
  float releaseVelocity = 0.5f;
};

class NoteListSource {
public:
  NoteListSource() = default;
  explicit NoteListSource(const std::vector<TimedNote>& notes);

  EventSource source() noexcept { return {&NoteListSource::produce, this}; }
  // Events of [t, t + frames) into `out`, frames relative to t. RT-safe.
  void produce(int64_t t, uint32_t frames, EventBuffer& out) noexcept;

  size_t numEvents() const noexcept { return events_.size(); }
  int64_t endTime() const noexcept { return events_.empty() ? 0 : events_.back().time; }  // the last note-off

private:
  static void produce(void* self, int64_t t, uint32_t frames, EventBuffer& out) noexcept {  // RT-safe
    static_cast<NoteListSource*>(self)->produce(t, frames, out);
  }
  struct Timed {
    int64_t time;
    NoteEvent event;
  };
  std::vector<Timed> events_;
  size_t cursor_ = 0;
  int64_t next_ = 0;  // the block start produce() expects next (anything else re-seeks)
};

}  // namespace pq::engine
