#pragma once

// T7.16b (docs/reviews/M7-rework.md M12, QUESTIONS Q299): Caps Lock pressed as a CHORD (with Ctrl / Alt / Super held —
// macro mode's Ctrl+CapsLock; ui::isChordKey) is a shortcut, not the lock. Windows flips the OS lock on the press before
// the window hears it, so a text field typed into afterwards would come out in capitals. The guard undoes that: once
// the chord's Caps Lock is RELEASED it taps Caps Lock once through the OS input stream (Windows SendInput: the lock
// flips back), and drops the two events of that tap when they come back, so the app never sees them. The chord itself
// (its press, auto-repeats and release) is passed on as usual.
//
// Why on the release (bug-ji-input; the T7.16b guard tapped on the press): a tap injected while the physical key is
// still down is no key transition — Windows does not flip the lock for it and SDL reports its press as an auto-repeat
// (the key is already down) and drops the user's real release that follows (the key is already up). The guard then
// let the tap's press through and stayed armed, swallowing the user's NEXT Ctrl+CapsLock: macro mode toggled only
// every other time, on the presses where the OS lock turned on. On the release the key is up, the tap's press is a real
// transition, and the tap's two events arrive in order (SendInput inserts them together, after anything queued).
//
// The tap's events are dropped whatever their repeat flag says. Should they never come back (the focus moved to
// another window between the release and the tap, a keyboard hook ate them), the guard forgets them after
// kTapWindowNs, so the user's next press is never mistaken for them. A tap the OS refused (SendInput inserted nothing)
// arms nothing. Auto-repeats of the held chord change nothing (the OS flips the lock on the first press only).
// Not handled: a new press of the chord already queued before the release was processed (a re-press within one event
// loop iteration) is taken for the tap's press.
//
// UI thread only (the app's event loop). Elsewhere than Windows restoreCapsLock() does nothing and returns false.

#include <cstdint>

namespace pq::ui {

// Taps Caps Lock once (press + release) through the OS input stream. False: not possible here / refused.
bool restoreCapsLock() noexcept;

class ChordLockGuard {
public:
  using Restore = bool (*)() noexcept;
  // How long after the tap its two events may still come back (event timestamps; they normally take milliseconds).
  static constexpr uint64_t kTapWindowNs = 500'000'000;
  explicit ChordLockGuard(Restore restore = &restoreCapsLock) noexcept : restore_(restore) {}

  // Feed every Caps Lock key event: `down` press / release, `chord` Ctrl / Alt / Super held, `repeat` the platform's
  // auto-repeat flag, `timeNs` the event's timestamp, `nowNs` the clock now in the same time base (when a tap is sent).
  // Returns true when the event is the guard's own tap: the caller drops it.
  bool drop(bool down, bool chord, bool repeat, uint64_t timeNs, uint64_t nowNs) noexcept {
    if ((dropDown_ | dropUp_) != 0 && timeNs > tapNs_ + kTapWindowNs) dropDown_ = dropUp_ = 0;  // never came back
    if (down && dropDown_ != 0) {
      --dropDown_;
      return true;
    }
    if (!down && dropDown_ == 0 && dropUp_ != 0) {
      --dropUp_;
      return true;
    }
    if (down) {
      // The OS flipped the lock on this press: it is tapped back when the key comes up.
      if (chord && !repeat) owed_ = true;
      return false;
    }
    if (owed_) {
      owed_ = false;
      if (restore_ != nullptr && restore_()) {
        dropDown_ = 1;
        dropUp_ = 1;
        tapNs_ = nowNs;
        ++restores_;
      }
    }
    return false;
  }
  uint32_t restores() const noexcept { return restores_; }
  // A tap was sent and its events have not all come back yet.
  bool pending() const noexcept { return (dropDown_ | dropUp_) != 0; }
  // A chord press was seen; its release will tap.
  bool owed() const noexcept { return owed_; }

private:
  Restore restore_;
  bool owed_ = false;
  uint8_t dropDown_ = 0, dropUp_ = 0;
  uint64_t tapNs_ = 0;
  uint32_t restores_ = 0;
};

}  // namespace pq::ui
