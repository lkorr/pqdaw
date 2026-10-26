#include "pq/engine/note_list.h"

#include <algorithm>
#include <limits>

namespace pq::engine {

NoteListSource::NoteListSource(const std::vector<TimedNote>& notes) {
  struct Key {
    int64_t time;
    int kind;  // 0 = off, 1 = on
    size_t index;
  };
  std::vector<std::pair<Key, NoteEvent>> all;
  all.reserve(notes.size() * 2);
  for (size_t i = 0; i < notes.size(); ++i) {
    const TimedNote& n = notes[i];
    const uint32_t id = static_cast<uint32_t>(i + 1);
    // A note starting before 0 is clipped to [0, its end) (M1 review L9: it kept its full length before); one that
    // ends at or before 0 is not played at all.
    const int64_t len = std::max<int64_t>(1, n.length);
    constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
    const int64_t end = n.start > kMax - len ? kMax : n.start + len;
    if (end <= 0) continue;
    const int64_t start = std::max<int64_t>(0, n.start);
    all.push_back({{start, 1, i}, noteOn(0, id, n.hz, n.velocity)});
    all.push_back({{end, 0, i}, noteOff(0, id, n.releaseVelocity)});
  }
  std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) {
    if (a.first.time != b.first.time) return a.first.time < b.first.time;
    if (a.first.kind != b.first.kind) return a.first.kind < b.first.kind;
    return a.first.index < b.first.index;
  });
  events_.reserve(all.size());
  for (const auto& [k, e] : all) events_.push_back({k.time, e});
}

void NoteListSource::produce(int64_t t, uint32_t frames, EventBuffer& out) noexcept {  // RT-safe
  if (t != next_) {  // a jump (or the first block of a render that does not start at 0): re-seek
    cursor_ = static_cast<size_t>(
        std::lower_bound(events_.begin(), events_.end(), t, [](const Timed& e, int64_t v) { return e.time < v; }) -
        events_.begin());
  }
  const int64_t end = t + frames;
  while (cursor_ < events_.size() && events_[cursor_].time < end) {
    NoteEvent e = events_[cursor_].event;
    e.frame = static_cast<uint32_t>(events_[cursor_].time - t);
    out.push(e);
    ++cursor_;
  }
  next_ = end;
}

}  // namespace pq::engine
