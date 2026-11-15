#pragma once

// T7.16b (docs/reviews/M7-rework.md M12, QUESTIONS Q299): Caps Lock pressed as a CHORD (with Ctrl / Alt / Super held —
// macro mode's Ctrl+CapsLock; ui::isChordKey) is a shortcut, not the lock. Windows flips the OS lock on the press before
// the window hears it, so a text field typed into afterwards would come out in capitals. The guard undoes that: on a
// chord press it taps Caps Lock once through the OS input stream (Windows SendInput: the lock flips back), and drops the
// two events of that tap when they come back, so the app never sees them. The chord itself is passed on as usual.
//
// Order: SendInput inserts the tap's press and release together, so they arrive in order; the user's own release of the
// chord's Caps Lock may come before them (already queued) or after them. The guard drops the first Caps Lock press after
// its tap and the first release after that press, so either order works. A tap the OS refused (SendInput inserted
// nothing) arms nothing. Auto-repeats of the chord change nothing (the OS flips the lock on the first press only).
//
// UI thread only (the app's event loop). Elsewhere than Windows restoreCapsLock() does nothing and returns false.

#include <cstdint>

namespace pq::ui {

// Taps Caps Lock once (press + release) through the OS input stream. False: not possible here / refused.
bool restoreCapsLock() noexcept;

class ChordLockGuard {
public:
  using Restore = bool (*)() noexcept;
  explicit ChordLockGuard(Restore restore = &restoreCapsLock) noexcept : restore_(restore) {}

  // Feed every Caps Lock key event (`down`: press, `chord`: Ctrl / Alt / Super held, `repeat`: an auto-repeat).
  // Returns true when the event is the guard's own tap: the caller drops it. A chord press is passed (false) and taps.
  bool drop(bool down, bool chord, bool repeat) noexcept {
    if (down && !repeat && dropDown_ != 0) {
      --dropDown_;
      return true;
    }
    if (!down && dropDown_ == 0 && dropUp_ != 0) {
      --dropUp_;
      return true;
    }
    if (down && chord && !repeat && dropDown_ == 0 && dropUp_ == 0 && restore_ != nullptr && restore_()) {
      dropDown_ = 1;
      dropUp_ = 1;
      ++restores_;
    }
    return false;
  }
  uint32_t restores() const noexcept { return restores_; }
  bool pending() const noexcept { return (dropDown_ | dropUp_) != 0; }

private:
  Restore restore_;
  uint8_t dropDown_ = 0, dropUp_ = 0;
  uint32_t restores_ = 0;
};

}  // namespace pq::ui
