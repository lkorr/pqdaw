// bug-roll-playhead: the transport's playhead in the clip view (clip time: the clip's start, its start marker, the
// loop's wrap), the ruler as Live's scrub area (click plays / jumps, sideways drag scrubs, snapped, Alt unsnapped),
// Follow paging, through the real window dispatch with FakeTransport; the golden.

#include "piano_roll_fix.h"

#include <cmath>
#include <optional>

using namespace roll_fix;

namespace {

constexpr const char* kRuler = "main/body/bottom/detail/clip/clip-view/ruler";
constexpr const char* kNotes = "main/body/bottom/detail/clip/clip-view/notes";
constexpr Ticks kBar = kTicksPerWhole;
using Call = FakeTransport::Call;

// A point in the ruler's scrub strip (bug-paused-playhead: 7 px under the 13 px brace strip, scale 1) at content tick t.
Vec2 scrubPoint(Roll& r, double t) {
  const IRect ru = r.d.rect(kRuler);
  return {r.view->xOfTick(t) + 0.25f, static_cast<float>(ru.y) + 16.5f};
}

// A point in the numbers row under it.
Vec2 numbersPoint(Roll& r, double t) {
  const IRect ru = r.d.rect(kRuler);
  return {r.view->xOfTick(t) + 0.25f, static_cast<float>(ru.bottom()) - 6.0f};
}

void moveClip(Roll& r, Ticks start) {
  REQUIRE(r.ctx.store().dispatch(doc::MoveClip{r.ref, r.ref.track, start}).ok());
  r.d.step();
}

void setLoop(Roll& r, Ticks start, Ticks end) {
  doc::ClipLoop l;
  l.enabled = true;
  l.start = start;
  l.end = end;
  REQUIRE(r.ctx.store().dispatch(doc::SetClipLoop{r.ref, l}).ok());
  r.d.step();
}

}  // namespace

TEST_CASE("pianoroll playhead: drawn in clip time while the song position is inside the clip, hidden outside; loop wrap") {
  Roll r;
  moveClip(r, 2 * kBar);  // the clip plays song bars 3 … 6
  r.d.advance(0.2);
  CHECK_FALSE(r.view->playheadTick().has_value());
  const uint64_t reads0 = r.view->playheadReads();
  r.d.advance(5.0);  // stopped: no reads
  CHECK(r.view->playheadReads() == reads0);

  CHECK(r.d.press("Space") == KeyResult::Action);  // plays from the start marker (0): before the clip
  r.d.advance(0.1);
  CHECK_FALSE(r.view->playheadTick().has_value());
  r.port.setPlayheadTick(2 * kBar + kQ);
  r.d.advance(0.2);
  REQUIRE(r.view->playheadTick().has_value());
  CHECK(*r.view->playheadTick() == kQ);
  CHECK(r.view->playheadReads() > reads0);

  // A move: only the line's old and new columns repaint in the roll (and the arrangement's, the control bar's readout).
  r.port.setPlayheadTick(2 * kBar + 2 * kQ);
  r.d.tree().update(r.d.now() + 0.15);
  const IRect bar = r.d.rect("main/control-bar");
  const IRect notes = r.d.rect(kNotes);
  bool rollColumn = false;
  for (const ClipRect& c : r.d.tree().damage().rects()) {
    const bool inBar = c.y1 <= bar.bottom();
    const bool narrow = c.x1 - c.x0 <= 3;
    CHECK_MESSAGE((inBar || narrow), c.x0 << "," << c.y0 << " to " << c.x1 << "," << c.y1);
    rollColumn = rollColumn || (narrow && c.y1 >= notes.bottom() && c.x0 >= notes.x);
  }
  CHECK(rollColumn);
  r.d.step();
  CHECK(*r.view->playheadTick() == 2 * kQ);

  // After the clip's end: hidden.
  r.port.setPlayheadTick(6 * kBar + 10);
  r.d.advance(0.2);
  CHECK_FALSE(r.view->playheadTick().has_value());

  // Looped over bar 2 of the content: the first pass plays content 0 … 2 bars (song 3 … 5), then bar 2 repeats.
  setLoop(r, kBar, 2 * kBar);
  r.port.setPlayheadTick(2 * kBar + kBar + kQ);  // the first pass
  r.d.advance(0.2);
  CHECK(r.view->playheadTick() == std::optional<Ticks>(kBar + kQ));
  r.port.setPlayheadTick(4 * kBar + kQ);  // the first repeat
  r.d.advance(0.2);
  CHECK(r.view->playheadTick() == std::optional<Ticks>(kBar + kQ));
  r.port.setPlayheadTick(5 * kBar + 3 * kQ);  // the second
  r.d.advance(0.2);
  CHECK(r.view->playheadTick() == std::optional<Ticks>(kBar + 3 * kQ));

  // Detail view hidden: nothing drawn, no reads; shown again: it comes back.
  r.ctx.setDetailVisible(false);
  r.d.advance(0.2);
  CHECK_FALSE(r.view->playheadTick().has_value());
  const uint64_t reads1 = r.view->playheadReads();
  r.d.advance(2.0);
  CHECK(r.view->playheadReads() == reads1);
  r.ctx.setDetailVisible(true);
  r.d.advance(0.2);
  CHECK(r.view->playheadTick() == std::optional<Ticks>(kBar + 3 * kQ));

  // bug-paused-playhead: stop: the line stays where playback stopped, and the roll is idle again (no reads).
  CHECK(r.d.press("Space") == KeyResult::Action);
  r.d.advance(0.2);
  CHECK(r.view->playheadTick() == std::optional<Ticks>(kBar + 3 * kQ));
  const uint64_t reads2 = r.view->playheadReads();
  r.d.advance(5.0);
  CHECK(r.view->playheadReads() == reads2);
  CHECK(r.view->playheadTick() == std::optional<Ticks>(kBar + 3 * kQ));
}

TEST_CASE("pianoroll playhead: stopped, it follows where the start marker is placed; Stop again returns it to 0") {
  Roll r;
  moveClip(r, 2 * kBar);
  r.d.advance(0.2);
  CHECK_FALSE(r.view->playheadTick().has_value());  // stopped at 0: before the clip
  // A press on empty lane space places the start marker and, stopped, the transport's position, before the views hear.
  r.ctx.placeStart(2 * kBar + kQ);
  CHECK(r.port.last(Call::Locate)->tick == 2 * kBar + kQ);
  r.d.step();
  CHECK(r.view->playheadTick() == std::optional<Ticks>(kQ));
  r.ctx.jumpTo(2 * kBar + 2 * kQ);
  r.d.step();
  CHECK(r.view->playheadTick() == std::optional<Ticks>(2 * kQ));
  r.ctx.stopButton();  // stopped: back to the beginning (before the clip): hidden
  r.d.step();
  CHECK_FALSE(r.view->playheadTick().has_value());
}

TEST_CASE("pianoroll ruler: a click plays from there when stopped, jumps there when playing; snapped, Alt unsnapped") {
  Roll r;
  moveClip(r, 2 * kBar);
  r.port.calls.clear();  // opening the project located
  // Stopped: a click a fifth of a beat past beat 2 of the clip plays from beat 2 (the roll's 1/4 grid).
  r.d.click(scrubPoint(r, static_cast<double>(kQ) + 0.2 * static_cast<double>(kQ)));
  REQUIRE(r.port.count(Call::Play) == 1);
  CHECK(r.port.last(Call::Play)->tick == 2 * kBar + kQ);
  CHECK(r.ctx.startMarker() == 2 * kBar + kQ);
  CHECK(r.port.count(Call::Locate) == 0);
  r.d.advance(0.2);
  CHECK(r.view->playheadTick() == std::optional<Ticks>(kQ));

  // Playing: a click jumps (no second Play); with Alt exactly where it was clicked.
  const double raw = 3.0 * static_cast<double>(kQ) + 0.3 * static_cast<double>(kQ);
  r.d.click(scrubPoint(r, raw), mod::kAlt);
  CHECK(r.port.count(Call::Play) == 1);
  REQUIRE(r.port.count(Call::Locate) == 1);
  const double tol = 1.0 / r.view->pxPerTick() + 1.0;
  CHECK(std::abs(static_cast<double>(r.port.last(Call::Locate)->tick) - (static_cast<double>(2 * kBar) + raw)) <= tol);
  CHECK(r.ctx.startMarker() == r.port.last(Call::Locate)->tick);
  r.d.advance(0.2);
  REQUIRE(r.view->playheadTick().has_value());
  CHECK(std::abs(static_cast<double>(*r.view->playheadTick()) - raw) <= tol);

  // Past the clip's end (an unlooped clip): clamped to its last tick, so the line stays in the clip.
  r.view->setZoom(30.0 / static_cast<double>(kQ), 0.0);  // the whole clip and more in view
  r.d.step();
  r.d.click(scrubPoint(r, static_cast<double>(4 * kBar + 2 * kQ)), mod::kAlt);
  CHECK(r.port.last(Call::Locate)->tick == 6 * kBar - 1);

  // Looped: in the pass the transport is in. Loop bar 2; the transport in the second repeat (song bar 6).
  setLoop(r, kBar, 2 * kBar);
  r.port.setPlayheadTick(5 * kBar + kQ);
  r.d.advance(0.2);
  r.d.click(scrubPoint(r, static_cast<double>(kBar + 2 * kQ)));
  CHECK(r.port.last(Call::Locate)->tick == 5 * kBar + 2 * kQ);
  // Content only the first pass plays (bar 1, before the loop): there.
  r.d.click(scrubPoint(r, static_cast<double>(kQ)));
  CHECK(r.port.last(Call::Locate)->tick == 2 * kBar + kQ);

  // Space stops; Space again plays from the start marker the last click left; Shift+Space continues.
  CHECK(r.d.press("Space") == KeyResult::Action);
  CHECK(r.d.press("Space") == KeyResult::Action);
  CHECK(r.port.last(Call::Play)->tick == 2 * kBar + kQ);
}

TEST_CASE("pianoroll ruler: a drag in the scrub strip scrubs (the position follows the pointer); the numbers row zooms and scrolls") {
  Roll r;
  r.port.calls.clear();  // opening the project located
  const double z0 = r.view->pxPerTick(), s0 = r.view->scrollTick();
  const Vec2 a = scrubPoint(r, static_cast<double>(kQ));
  const Vec2 b = scrubPoint(r, static_cast<double>(5 * kQ));
  r.d.drag(a, b, 12);
  CHECK(r.view->pxPerTick() == doctest::Approx(z0));
  CHECK(r.view->scrollTick() == doctest::Approx(s0));
  REQUIRE(r.port.count(Call::Play) == 1);  // the first position plays, the rest jump
  CHECK(r.port.count(Call::Locate) >= 3);
  CHECK(r.port.last(Call::Locate)->tick == 5 * kQ);
  // Each new grid cell once: the scrub never repeats a position.
  for (size_t i = 1; i < r.port.calls.size(); ++i)
    if (r.port.calls[i].call == Call::Locate && r.port.calls[i - 1].call == Call::Locate) CHECK(r.port.calls[i].tick != r.port.calls[i - 1].tick);
  CHECK(r.ctx.startMarker() == 5 * kQ);

  // The numbers row: a click does nothing to the transport; up / down zooms; sideways scrolls.
  const size_t calls = r.port.calls.size();
  const Vec2 n = numbersPoint(r, static_cast<double>(kQ));
  r.d.click(n);
  r.d.drag(n, {n.x, n.y + 40.0f}, 6);
  const double z1 = r.view->pxPerTick();
  CHECK(z1 > z0 * 1.2);
  const double s1 = r.view->scrollTick();
  const Vec2 n2 = numbersPoint(r, static_cast<double>(2 * kQ));
  r.d.drag(n2, {n2.x - 60.0f, n2.y}, 6);
  CHECK(r.view->pxPerTick() == doctest::Approx(z1));
  CHECK(r.view->scrollTick() > s1);
  CHECK(r.port.calls.size() == calls);
}

TEST_CASE("pianoroll follow: the roll pages when the playhead leaves the view; Ctrl+Shift+F turns it off; scrolled away it stays") {
  Roll r;
  r.view->setZoom(200.0 / static_cast<double>(kQ), 0.0);
  r.view->scrollToTick(0.0);
  r.d.step();
  CHECK(r.ctx.follow());
  CHECK(r.d.press("Space") == KeyResult::Action);
  r.port.setPlayheadTick(kQ);
  r.d.advance(0.2);
  CHECK(r.view->scrollTick() == 0.0);
  r.port.setPlayheadTick(6 * kQ);  // out of view (≈ 5 beats shown): a page, the playhead at the left edge
  r.d.advance(0.2);
  CHECK(r.view->scrollTick() == static_cast<double>(6 * kQ));
  CHECK(r.view->playheadTick() == std::optional<Ticks>(6 * kQ));

  CHECK(r.d.press("Ctrl+Shift+F") == KeyResult::Action);  // off: the view stays
  CHECK_FALSE(r.ctx.follow());
  r.port.setPlayheadTick(12 * kQ);
  r.d.advance(0.2);
  CHECK(r.view->scrollTick() == static_cast<double>(6 * kQ));

  CHECK(r.d.press("Ctrl+Shift+F") == KeyResult::Action);  // on again, but the playhead is off where the user is
  CHECK(r.ctx.follow());
  r.port.setPlayheadTick(13 * kQ);
  r.d.advance(0.2);
  CHECK(r.view->scrollTick() == static_cast<double>(6 * kQ));
  // Scrolled back to it: it is followed again once it leaves the view.
  r.view->scrollToTick(static_cast<double>(10 * kQ));
  r.d.step();
  r.port.setPlayheadTick(14 * kQ);
  r.d.advance(0.2);
  CHECK(r.view->scrollTick() == static_cast<double>(10 * kQ));
  r.port.setPlayheadTick(15 * kQ + kQ / 2);
  r.d.advance(0.2);
  CHECK(r.view->scrollTick() == static_cast<double>(15 * kQ + kQ / 2));
  CHECK(r.d.press("Space") == KeyResult::Action);
}

TEST_CASE("render: pianoroll-playhead (the transport's line through the ruler, the notes and the velocity lane, in a looped clip) matches the golden image") {
  checkGolden("pianoroll-playhead");
}
