#pragma once

// The bug report's "steps to reproduce" (T5.x-bugreport): the recent input, kept as compact fixed-size records in a
// ring and formatted only when a report is made (F12).
//
//   record()  after the tree dispatched an event: key downs (with the focused widget), pointer downs / ups (button,
//             position, the widget under it), wheel steps, typed text. Pointer moves are not recorded (a move only
//             updates the last known position). Each record copies the widget's path tail into a fixed buffer and
//             keeps its type_info (static storage: valid after the widget is gone). No allocation, a few hundred ns
//             per click or key; nothing per frame.
//   format()  oldest first, times relative to F12: a down + up in place is one "click" line, a down + up elsewhere
//             one "drag start → end" line, consecutive wheel steps over one widget one line, consecutive typed text
//             one line, key repeats one "×N" line.
//
// UI thread only.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <typeinfo>

#include "pq/ui/input.h"
#include "pq/ui/widget.h"

namespace pq::views::bugreport {

class InputLog {
public:
  static constexpr size_t kCapacity = 256;
  static constexpr size_t kPathChars = 88;  // the widget path's tail
  static constexpr size_t kTextChars = 16;

  // After `tree` dispatched `e`; `now` in seconds (any monotonic clock, the one format() is called with).
  void record(const ui::InputEvent& e, const ui::UiTree& tree, double now) noexcept;
  // The last `maxLines` lines (oldest first), recorded after the `since`-th event (see total()), times relative to `now`.
  std::string format(double now, size_t maxLines = 40, uint64_t since = 0) const;
  // Events recorded so far (also those that fell out of the ring).
  uint64_t total() const noexcept { return total_; }
  void clear() noexcept {
    total_ = 0;
  }

  enum class Kind : uint8_t { Down, Up, Wheel, Key, Text };
  struct Entry {
    double t = 0;
    float x = 0, y = 0;    // pointer position, device px
    float wx = 0, wy = 0;  // wheel notches
    uint32_t key = 0;      // Key: the layout key
    ui::Mods mods = 0;
    Kind kind = Kind::Down;
    uint8_t button = 0;
    uint8_t clicks = 0;
    bool repeat = false;
    const std::type_info* type = nullptr;  // the widget's (null: none)
    char path[kPathChars] = {};             // "…/arrangement/lanes"; "" = none
    char text[kTextChars] = {};             // Text: the UTF-8 typed
  };
  const Entry& entry(uint64_t i) const noexcept { return ring_[i % kCapacity]; }

private:
  std::array<Entry, kCapacity> ring_{};
  uint64_t total_ = 0;
};

// The tail of `w`'s path of names ("…/track[3]/header/mute") written into `out` (NUL-terminated, at most n - 1 bytes),
// without allocating. Unnamed widgets are transparent (an unnamed leaf writes its nearest named ancestors' path).
void writePathTail(const ui::Widget* w, char* out, size_t n) noexcept;

}  // namespace pq::views::bugreport
