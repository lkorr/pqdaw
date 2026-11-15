// T7.16b (review M12, Q299): ChordLockGuard — a Caps Lock chord (Ctrl+CapsLock) taps the lock back once and drops the
// tap's own two events, in either order of the user's release; a plain Caps Lock, an auto-repeat and a refused tap
// change nothing. The OS side (SendInput) is not driven here: it would flip the Caps Lock of the machine running the
// tests.

#include <doctest/doctest.h>

#include "pq/ui/chord_lock.h"

using pq::ui::ChordLockGuard;

namespace {
int taps = 0;
bool tapOk() noexcept {
  ++taps;
  return true;
}
bool tapRefused() noexcept {
  ++taps;
  return false;
}
}  // namespace

TEST_CASE("chord lock: Ctrl+CapsLock taps the lock back once; the tap's press and release are dropped, the user's own pass") {
  SUBCASE("the user's release after the tap") {
    taps = 0;
    ChordLockGuard g(&tapOk);
    CHECK_FALSE(g.drop(true, true, false));  // the chord: passed on (it toggles macro mode)
    CHECK(taps == 1);
    CHECK(g.pending());
    CHECK(g.drop(true, true, false));  // the tap's press (Ctrl still held): dropped, no second tap
    CHECK(g.drop(false, true, false));  // the tap's release: dropped
    CHECK_FALSE(g.pending());
    CHECK_FALSE(g.drop(false, true, false));  // the user's release: passed
    CHECK(taps == 1);
    CHECK(g.restores() == 1);
  }
  SUBCASE("the user's release already queued before the tap") {
    taps = 0;
    ChordLockGuard g(&tapOk);
    CHECK_FALSE(g.drop(true, true, false));
    CHECK_FALSE(g.drop(false, true, false));  // the user's release: passed (no tap press seen yet)
    CHECK(g.drop(true, true, false));
    CHECK(g.drop(false, false, false));  // Ctrl released meanwhile: still the tap's release
    CHECK_FALSE(g.pending());
    CHECK(taps == 1);
  }
  SUBCASE("held: auto-repeats tap nothing, the next chord taps again") {
    taps = 0;
    ChordLockGuard g(&tapOk);
    CHECK_FALSE(g.drop(true, true, false));
    CHECK_FALSE(g.drop(true, true, true));  // auto-repeat of the held chord
    CHECK(g.drop(true, true, false));
    CHECK(g.drop(false, true, false));
    CHECK_FALSE(g.drop(true, true, true));
    CHECK_FALSE(g.drop(false, true, false));
    CHECK(taps == 1);
    CHECK_FALSE(g.drop(true, true, false));  // a second Ctrl+CapsLock
    CHECK(taps == 2);
    CHECK(g.drop(true, true, false));
    CHECK(g.drop(false, true, false));
    CHECK_FALSE(g.drop(false, true, false));
  }
}

TEST_CASE("chord lock: a plain Caps Lock is the lock (nothing tapped, nothing dropped); a refused tap arms nothing") {
  taps = 0;
  ChordLockGuard g(&tapOk);
  for (int i = 0; i < 3; ++i) {
    CHECK_FALSE(g.drop(true, false, false));
    CHECK_FALSE(g.drop(false, false, false));
  }
  CHECK(taps == 0);
  CHECK(g.restores() == 0);
  ChordLockGuard refused(&tapRefused);
  CHECK_FALSE(refused.drop(true, true, false));
  CHECK(taps == 1);
  CHECK_FALSE(refused.pending());
  CHECK_FALSE(refused.drop(true, false, false));  // the next real press is not mistaken for a tap
  CHECK_FALSE(refused.drop(false, false, false));
  ChordLockGuard none(nullptr);
  CHECK_FALSE(none.drop(true, true, false));
  CHECK_FALSE(none.pending());
}
