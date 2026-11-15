// T7.16b (review M12, Q299): ChordLockGuard — a Caps Lock chord (Ctrl+CapsLock) taps the lock back once and drops the
// tap's own two events; a plain Caps Lock, an auto-repeat and a refused tap change nothing.
// bug-ji-input: the tap is sent when the chord's Caps Lock comes UP, and the event orders Windows + SDL really produce
// are fed here: every Ctrl+CapsLock press reaches the app (it used to be every other one), the tap's press is dropped
// even when it is flagged as an auto-repeat (a tap injected while the key is down), a tap whose events never come back
// is forgotten, and a quick re-press after the tap passes. The OS side (SendInput) is not driven here: it would flip
// the Caps Lock of the machine running the tests.

#include <doctest/doctest.h>

#include <cstdint>

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

constexpr uint64_t kMs = 1'000'000;

// A clock in ms and the events as the app's loop hands them to the guard (an event's time = when it happened, the
// clock = when it is handled: later while the loop is busy).
struct Feed {
  ChordLockGuard& g;
  uint64_t now = 1000 * kMs;
  bool press(bool chord, bool repeat = false, uint64_t at = 0) { return g.drop(true, chord, repeat, at ? at : now, now); }
  bool release(bool chord, uint64_t at = 0) { return g.drop(false, chord, false, at ? at : now, now); }
  void wait(uint64_t ms) { now += ms * kMs; }
};
}  // namespace

TEST_CASE("chord lock: Ctrl+CapsLock taps the lock back when its Caps Lock comes up; the tap's press and release are dropped, the user's own pass") {
  taps = 0;
  ChordLockGuard g(&tapOk);
  Feed f{g};
  CHECK_FALSE(f.press(true));  // the chord: passed on (it toggles macro mode); nothing tapped while the key is down
  CHECK(taps == 0);
  CHECK(g.owed());
  f.wait(80);
  CHECK_FALSE(f.release(true));  // the user's release: passed, and the tap goes out now
  CHECK(taps == 1);
  CHECK(g.pending());
  CHECK_FALSE(g.owed());
  f.wait(2);
  CHECK(f.press(true));    // the tap's press (Ctrl still held): dropped, no second tap
  CHECK(f.release(true));  // the tap's release: dropped
  CHECK_FALSE(g.pending());
  CHECK(taps == 1);
  CHECK(g.restores() == 1);
}

TEST_CASE("chord lock (bug-ji-input): every Ctrl+CapsLock reaches the app, ten in a row; Ctrl let go first still taps") {
  taps = 0;
  ChordLockGuard g(&tapOk);
  Feed f{g};
  int passed = 0;
  for (int i = 0; i < 10; ++i) {
    if (!f.press(true)) ++passed;  // the presses the app sees: each one toggles macro mode
    f.wait(90);
    const bool ctrlFirst = i % 2 == 1;  // the user lets Ctrl go before Caps Lock every other time
    CHECK_FALSE(f.release(!ctrlFirst));
    f.wait(1);
    CHECK(f.press(!ctrlFirst));
    CHECK(f.release(!ctrlFirst));
    f.wait(300);
  }
  CHECK(passed == 10);
  CHECK(taps == 10);
  CHECK_FALSE(g.pending());
}

TEST_CASE("chord lock (bug-ji-input): the tap's press flagged as an auto-repeat is still the tap's; held chords tap once") {
  SUBCASE("a tap press SDL reports as a repeat (the key looked down) is dropped, and the next real press passes") {
    taps = 0;
    ChordLockGuard g(&tapOk);
    Feed f{g};
    CHECK_FALSE(f.press(true));
    f.wait(60);
    CHECK_FALSE(f.release(true));
    CHECK(f.press(true, /*repeat=*/true));
    CHECK(f.release(true));
    f.wait(400);
    CHECK_FALSE(f.press(true));  // the T7.16b guard swallowed this one
    CHECK(g.owed());
  }
  SUBCASE("held: the auto-repeats pass and tap nothing; the release taps once") {
    taps = 0;
    ChordLockGuard g(&tapOk);
    Feed f{g};
    CHECK_FALSE(f.press(true));
    for (int i = 0; i < 5; ++i) {
      f.wait(33);
      CHECK_FALSE(f.press(true, true));
    }
    CHECK(taps == 0);
    CHECK_FALSE(f.release(true));
    CHECK(taps == 1);
    CHECK(f.press(true));
    CHECK(f.release(true));
    CHECK_FALSE(g.pending());
  }
  SUBCASE("a repeat with no press seen (the window got the focus while the chord was held) owes nothing") {
    taps = 0;
    ChordLockGuard g(&tapOk);
    Feed f{g};
    CHECK_FALSE(f.press(true, true));
    CHECK_FALSE(g.owed());
    CHECK_FALSE(f.release(true));
    CHECK(taps == 0);
  }
}

TEST_CASE("chord lock (bug-ji-input): a tap whose events never come back is forgotten; a quick re-press after the tap passes") {
  SUBCASE("the focus left between the release and the tap: the next press, a second later, is the user's") {
    taps = 0;
    ChordLockGuard g(&tapOk);
    Feed f{g};
    CHECK_FALSE(f.press(true));
    f.wait(70);
    CHECK_FALSE(f.release(true));
    CHECK(g.pending());
    f.wait(ChordLockGuard::kTapWindowNs / kMs + 1);
    CHECK_FALSE(f.press(true));  // not mistaken for the lost tap
    CHECK(g.owed());
    CHECK_FALSE(f.release(true));
    CHECK(taps == 2);
    CHECK(f.press(true));
    CHECK(f.release(true));
  }
  SUBCASE("the events of a tap handled late (a busy frame) still count as the tap's: their times are the tap's") {
    taps = 0;
    ChordLockGuard g(&tapOk);
    Feed f{g};
    CHECK_FALSE(f.press(true));
    f.wait(70);
    CHECK_FALSE(f.release(true));
    const uint64_t sent = f.now;
    f.wait(900);  // the loop gets to them almost a second later
    CHECK(f.press(true, false, sent + 1 * kMs));
    CHECK(f.release(true, sent + 1 * kMs));
  }
  SUBCASE("a fast double Ctrl+CapsLock: the second press comes after the first one's tap and passes") {
    taps = 0;
    ChordLockGuard g(&tapOk);
    Feed f{g};
    CHECK_FALSE(f.press(true));
    f.wait(50);
    CHECK_FALSE(f.release(true));
    f.wait(1);
    CHECK(f.press(true));
    CHECK(f.release(true));
    f.wait(40);
    CHECK_FALSE(f.press(true));
    f.wait(50);
    CHECK_FALSE(f.release(true));
    CHECK(taps == 2);
    CHECK(f.press(true));
    CHECK(f.release(true));
    CHECK_FALSE(g.pending());
  }
}

TEST_CASE("chord lock: a plain Caps Lock is the lock (nothing tapped, nothing dropped); a refused tap arms nothing") {
  taps = 0;
  ChordLockGuard g(&tapOk);
  Feed f{g};
  for (int i = 0; i < 3; ++i) {
    CHECK_FALSE(f.press(false));
    CHECK_FALSE(f.release(false));
    f.wait(100);
  }
  CHECK(taps == 0);
  CHECK(g.restores() == 0);
  ChordLockGuard refused(&tapRefused);
  Feed r{refused};
  CHECK_FALSE(r.press(true));
  CHECK_FALSE(r.release(true));
  CHECK(taps == 1);
  CHECK_FALSE(refused.pending());
  CHECK_FALSE(r.press(false));  // the next real press is not mistaken for a tap
  CHECK_FALSE(r.release(false));
  ChordLockGuard none(nullptr);
  Feed n{none};
  CHECK_FALSE(n.press(true));
  CHECK_FALSE(n.release(true));
  CHECK_FALSE(none.pending());
}
