// T7.14 piano roll: vibrato by direct manipulation (docs/plans/M7.md V1–V3). Every gesture goes through the real
// shell and pointer dispatch (piano_roll_fix.h), one undo step each; nothing is typed anywhere:
//   the ~ grip above the right end of the hovered / selected note: up / down = depth (live on the note, in the status
//   line), left / right = rate (past a dead zone), Shift = fine, double-click removes; a selection changes together;
//   the four corners of the amplitude envelope under the note: onset, fade-in, fade-out, end (grid snap, clamped
//   between their neighbours, the inner two can meet);
//   the grips do not get in the way of the resize edge, the Alt bend or the note's body.
// Golden: pianoroll-vibrato.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "piano_roll_fix.h"

using namespace roll_fix;

namespace {

doc::NoteVibrato vibratoOf(const Roll& r, doc::NoteId id) {
  const doc::NoteCurves* rec = doc::findNoteCurves(r.clip(), id);
  return rec ? rec->vibrato : doc::NoteVibrato{};
}

// The largest swing of the drawn note around `base` over [f0, f1] of its length.
double swing(const Roll& r, doc::NoteId id, double base, double f0, double f1) {
  double peak = 0.0;
  for (int i = 0; i <= 600; ++i) peak = std::max(peak, std::fabs(r.cents(id, f0 + (f1 - f0) * i / 600.0) - base));
  return peak;
}

}  // namespace

TEST_CASE("roll vibrato: the ~ grip — hover a note, drag the grip up for depth and sideways for rate; the wave grows on the note and the status line reads it; double-click removes") {
  Roll r;
  r.focusNotes();
  const doc::NoteId n = r.addNotes({Roll::plain(0, 8 * kQ, 4)})[0];
  const double base = r.cents(n, 0.0);
  // Nothing is shown until the pointer is over the note.
  r.d.move(r.at(10.0 * kQ, 12));
  r.d.step();
  CHECK(r.view->noteHover().note == 0);
  r.d.move(r.view->notePoint(n, 0.5));
  r.d.step();
  REQUIRE(r.view->noteHover().note == n.value);
  const std::optional<Vec2> grip = r.view->vibratoGrip(n);
  REQUIRE(grip.has_value());
  // Above the line, inside the note's end, clear of the resize zone (the last 4 px of the note).
  const Vec2 end = r.view->notePoint(n, 1.0);
  CHECK(grip->y <= end.y - 10.0f);
  CHECK(grip->x <= end.x - 10.0f);
  CHECK(grip->x >= end.x - 20.0f);

  // Up by 5 px (14 px = 100 ¢): 36 ¢. The preview is live and the status line says it.
  size_t h = r.history();
  r.d.move(*grip);
  CHECK(r.view->noteHover().note == n.value);  // over its grip the note stays hovered
  REQUIRE(r.d.down(*grip));
  r.d.move({grip->x, grip->y - 3.0f});
  r.d.move({grip->x, grip->y - 5.0f});
  r.d.step();
  CHECK(r.history() == h);
  CHECK(r.ctx.status() == "Vibrato 36 \xC2\xA2 \xC2\xB7 8 Hz");
  CHECK(swing(r, n, base, 0.0, 1.0) == doctest::Approx(36.0).epsilon(0.03));  // the wave is on the note already
  r.d.up({grip->x, grip->y - 5.0f});
  r.d.step();
  CHECK(r.history() == h + 1);
  doc::NoteVibrato v = vibratoOf(r, n);
  CHECK(v.depthCents == 36.0f);
  CHECK(v.rate == 8.0f);  // one cycle per quarter-note cell at 120 bpm is 2 Hz: folded by octaves into 4.2 … 8.5
  CHECK(v.unit == doc::NoteVibrato::RateUnit::Hz);
  CHECK(v.onset == 0);
  CHECK(v.end == 0);
  CHECK(v.fadeIn == 0);
  CHECK(v.fadeOut == 0);
  CHECK(doc::findNoteCurves(r.clip(), n)->pitch.empty());  // a property: nothing baked
  CHECK(swing(r, n, base, 0.0, 1.0) == doctest::Approx(36.0).epsilon(0.03));
  // 8 Hz at 120 bpm: four cycles per beat, 32 over the note: 64 zero crossings.
  int crossings = 0;
  double last = r.cents(n, 0.0005) - base;
  for (int i = 1; i <= 4000; ++i) {
    const double c = r.cents(n, 0.0005 + 0.999 * i / 4000.0) - base;
    if ((c > 0.0) != (last > 0.0)) ++crossings;
    last = c;
  }
  CHECK(crossings >= 62);
  CHECK(crossings <= 64);

  // Sideways: the rate. 8 px of dead zone, then 120 px to the right doubles the wavelength: 4 Hz. The depth stays.
  std::optional<Vec2> g2 = r.view->vibratoGrip(n);
  REQUIRE(g2.has_value());
  CHECK(g2->y < grip->y);  // the grip rose with the crest
  h = r.history();
  REQUIRE(r.d.down(*g2));
  r.d.move({g2->x + 6.0f, g2->y});
  CHECK(r.ctx.status() == "Vibrato 36 \xC2\xA2 \xC2\xB7 8 Hz");  // inside the dead zone
  r.d.move({g2->x + 128.0f, g2->y});
  CHECK(r.ctx.status() == "Vibrato 36 \xC2\xA2 \xC2\xB7 4 Hz");
  r.d.up({g2->x + 128.0f, g2->y});
  r.d.step();
  CHECK(r.history() == h + 1);
  v = vibratoOf(r, n);
  CHECK(v.rate == 4.0f);
  CHECK(v.depthCents == 36.0f);
  // To the left: faster. Shift = fine (a tenth of the travel, 0.1 ¢ and 0.01 Hz steps). (The grip is the hovered
  // note's: the pointer comes back to the note first.)
  r.d.move(r.view->notePoint(n, 0.5));
  g2 = r.view->vibratoGrip(n);
  r.d.drag(*g2, {g2->x - 68.0f, g2->y}, 5);
  r.d.step();
  CHECK(vibratoOf(r, n).rate == doctest::Approx(4.0 * std::sqrt(2.0)).epsilon(0.02));
  r.d.move(r.view->notePoint(n, 0.5));
  g2 = r.view->vibratoGrip(n);
  h = r.history();
  r.d.drag(*g2, {g2->x, g2->y - 14.0f}, 5, mod::kShift);
  r.d.step();
  CHECK(r.history() == h + 1);
  CHECK(vibratoOf(r, n).depthCents == doctest::Approx(46.0).epsilon(0.001));

  // Down past zero: off (the record is gone). Undo brings it back.
  r.d.move(r.view->notePoint(n, 0.5));
  g2 = r.view->vibratoGrip(n);
  r.d.drag(*g2, {g2->x, g2->y + 40.0f}, 5);
  r.d.step();
  CHECK(doc::findNoteCurves(r.clip(), n) == nullptr);
  CHECK(r.ctx.status() == "Vibrato off");
  REQUIRE(r.ctx.undo());
  r.d.step();
  CHECK(vibratoOf(r, n).set());
  // A double-click on the grip removes it.
  r.d.move(r.view->notePoint(n, 0.5));
  g2 = r.view->vibratoGrip(n);
  h = r.history();
  r.d.doubleClick(*g2);
  r.d.step();
  CHECK(r.history() == h + 1);
  CHECK(doc::findNoteCurves(r.clip(), n) == nullptr);
  // A click that does not move changes nothing.
  r.d.move(r.view->notePoint(n, 0.5));
  g2 = r.view->vibratoGrip(n);
  h = r.history();
  r.d.click(*g2);
  r.d.step();
  CHECK(r.history() == h);
}

TEST_CASE("roll vibrato: the envelope's four corners — onset, fade-in, fade-out and end drag sideways; the inner corners can meet (a swell)") {
  Roll r;
  r.focusNotes();
  const doc::NoteId n = r.addNotes({Roll::plain(0, 8 * kQ, 4)})[0];
  const double base = r.cents(n, 0.0);
  r.select({n});
  CHECK_FALSE(r.view->vibratoCorner(n, 0).has_value());  // no vibrato: no corners
  REQUIRE(r.view->vibratoGrip(n).has_value());           // a selected note shows its grip without the pointer
  const Vec2 grip = *r.view->vibratoGrip(n);
  r.d.drag(grip, {grip.x, grip.y - 7.0f}, 5);  // 50 ¢
  r.d.step();
  REQUIRE(vibratoOf(r, n).depthCents == 50.0f);

  // The corners lie under the line: the outer ones at the note's ends, the inner ones under the full swing.
  std::optional<Vec2> c[4];
  for (int i = 0; i < 4; ++i) {
    c[i] = r.view->vibratoCorner(n, i);
    REQUIRE(c[i].has_value());
    CHECK(c[i]->y >= r.view->notePoint(n, 0.5).y + 6.0f);
  }
  CHECK(c[0]->x == doctest::Approx(r.view->xOfTick(0.0)));
  CHECK(c[3]->x == doctest::Approx(r.view->xOfTick(8.0 * kQ)));
  CHECK(c[1]->x == c[0]->x);  // no fades yet: the inner corners sit on the outer ones
  CHECK(c[2]->x == c[3]->x);

  // The start corner dragged right: with two corners on one tick the direction picks — right is the fade-in.
  size_t h = r.history();
  REQUIRE(r.d.down(*c[0]));
  r.d.move({c[0]->x + kBeat, c[0]->y});
  r.d.move({c[0]->x + 2.0f * kBeat + 4.0f, c[0]->y});  // snaps to the grid
  CHECK(r.ctx.status() == "Vibrato fade-in: 2 beats");
  r.d.up({c[0]->x + 2.0f * kBeat + 4.0f, c[0]->y});
  r.d.step();
  CHECK(r.history() == h + 1);
  doc::NoteVibrato v = vibratoOf(r, n);
  CHECK(v.onset == 0);
  CHECK(v.fadeIn == 2 * kQ);
  CHECK(v.end == 0);
  CHECK(v.fadeOut == 0);
  CHECK(swing(r, n, base, 0.0, 0.125) < 27.0);  // half way through the fade: half the depth
  CHECK(swing(r, n, base, 0.3, 0.9) == doctest::Approx(50.0).epsilon(0.03));

  // The onset corner (now alone at the start) a beat later; the fade-in's end stays where it is.
  c[0] = r.view->vibratoCorner(n, 0);
  h = r.history();
  r.d.drag(*c[0], {c[0]->x + kBeat, c[0]->y}, 5);
  r.d.step();
  CHECK(r.history() == h + 1);
  v = vibratoOf(r, n);
  CHECK(v.onset == kQ);
  CHECK(v.fadeIn == kQ);
  CHECK(r.cents(n, 0.1) == doctest::Approx(base).epsilon(1e-12));  // silent before the onset
  // The end corner dragged left: with the fade-out's corner on it, left is the fade-out.
  c[3] = r.view->vibratoCorner(n, 3);
  r.d.drag(*c[3], {c[3]->x - 2.0f * kBeat, c[3]->y}, 5);
  r.d.step();
  v = vibratoOf(r, n);
  CHECK(v.end == 0);
  CHECK(v.fadeOut == 2 * kQ);
  // Then the end itself, a beat earlier: the fade-out's start stays, the fade is a beat shorter.
  c[3] = r.view->vibratoCorner(n, 3);
  r.d.drag(*c[3], {c[3]->x - kBeat, c[3]->y}, 5);
  r.d.step();
  v = vibratoOf(r, n);
  CHECK(v.end == 7 * kQ);
  CHECK(v.fadeOut == kQ);
  CHECK(r.cents(n, 0.95) == doctest::Approx(base).epsilon(1e-12));  // silent after the end
  // Shift = free (no grid): the onset 20 px = a third of a beat earlier.
  c[0] = r.view->vibratoCorner(n, 0);
  r.d.drag(*c[0], {c[0]->x - 20.0f, c[0]->y}, 5, mod::kShift);
  r.d.step();
  v = vibratoOf(r, n);
  CHECK(v.onset == doctest::Approx(static_cast<double>(kQ) * 2.0 / 3.0).epsilon(0.01));
  CHECK(v.onset + v.fadeIn == 2 * kQ);  // the other corner did not move

  // A swell: the fade-in's corner dragged far right stops ON the fade-out's corner (they meet, they do not cross).
  c[1] = r.view->vibratoCorner(n, 1);
  h = r.history();
  r.d.drag(*c[1], {c[1]->x + 7.0f * kBeat, c[1]->y}, 6);
  r.d.step();
  CHECK(r.history() == h + 1);
  v = vibratoOf(r, n);
  CHECK(v.onset + v.fadeIn == 6 * kQ);
  CHECK(v.end - v.fadeOut == 6 * kQ);
  CHECK(r.view->vibratoCorner(n, 1)->x == r.view->vibratoCorner(n, 2)->x);
  CHECK(swing(r, n, base, 0.7, 0.8) == doctest::Approx(50.0).epsilon(0.05));  // the peak, at beat 6
  CHECK(swing(r, n, base, 0.2, 0.3) < 20.0);
  CHECK(r.note(n).length == 8 * kQ);  // no corner drag resized the note
}

TEST_CASE("roll vibrato: a drag on one selected note's grip applies to every selected note; each keeps its own envelope") {
  Roll r;
  r.focusNotes();
  const std::vector<doc::NoteId> ids = r.addNotes({Roll::plain(0, 4 * kQ, 0), Roll::plain(0, 4 * kQ, 7), Roll::plain(4 * kQ, 4 * kQ, 4)});
  // The second note already swells in over a beat.
  doc::NoteVibrato had;
  had.depthCents = 10.0f;
  had.rate = 5.0f;
  had.fadeIn = kQ;
  REQUIRE(r.ctx.store().dispatch(doc::SetNoteCurves{r.ref, {doc::NoteCurves{ids[1], {}, {}, {}, had}}}).ok());
  r.d.step();
  r.select({ids[0], ids[1]});
  size_t h = r.history();
  const Vec2 grip = *r.view->vibratoGrip(ids[0]);
  r.d.drag(grip, {grip.x, grip.y - 4.0f}, 4);  // + 29 ¢ on the grabbed note
  r.d.step();
  CHECK(r.history() == h + 1);  // one undo step for both
  CHECK(vibratoOf(r, ids[0]).depthCents == 29.0f);
  CHECK(vibratoOf(r, ids[1]).depthCents == 29.0f);
  CHECK(vibratoOf(r, ids[1]).rate == vibratoOf(r, ids[0]).rate);
  CHECK(vibratoOf(r, ids[1]).fadeIn == kQ);  // its own envelope
  CHECK(vibratoOf(r, ids[0]).fadeIn == 0);
  CHECK_FALSE(vibratoOf(r, ids[2]).set());  // not selected
  CHECK(r.view->selection().size() == 2);
  // A corner too: both ends a beat earlier.
  const Vec2 c3 = *r.view->vibratoCorner(ids[0], 3);
  r.d.drag(c3, {c3.x - kBeat, c3.y}, 5);
  r.d.step();
  CHECK(vibratoOf(r, ids[0]).fadeOut == kQ);  // (two corners on the end: left is the fade-out)
  CHECK(vibratoOf(r, ids[1]).fadeOut == kQ);
  // Double-click the grip: both off, one step.
  h = r.history();
  r.d.doubleClick(*r.view->vibratoGrip(ids[1]));
  r.d.step();
  CHECK(r.history() == h + 1);
  CHECK(r.clip().curves == nullptr);
  REQUIRE(r.ctx.undo());
  r.d.step();
  CHECK(vibratoOf(r, ids[0]).set());
  CHECK(vibratoOf(r, ids[1]).fadeIn == kQ);
}

TEST_CASE("roll vibrato: the grips leave the other gestures alone — the note's end still resizes, Alt + the end still bends, the body still moves; a vibrating note is hit on its wave") {
  Roll r;
  r.focusNotes();
  const std::vector<doc::NoteId> ids = r.addNotes({Roll::plain(0, 4 * kQ, 4), Roll::plain(4 * kQ, 2 * kQ, 7)});
  const doc::NoteId n = ids[0];
  r.select({n});
  const Vec2 grip = *r.view->vibratoGrip(n);
  r.d.drag(grip, {grip.x, grip.y - 7.0f}, 5);
  r.d.step();
  REQUIRE(vibratoOf(r, n).depthCents == 50.0f);
  const doc::NoteVibrato v = vibratoOf(r, n);

  // The right edge (on the line, the last pixels of the note): a resize.
  size_t h = r.history();
  const Vec2 end = r.view->notePoint(n, 0.992);  // (on its wave: the note is hit where it is drawn)
  r.d.drag(end, {end.x - kBeat, end.y}, 5);
  r.d.step();
  CHECK(r.history() == h + 1);
  CHECK(r.note(n).length == 3 * kQ);
  CHECK(vibratoOf(r, n) == v);  // "to the note's end" still: nothing rewritten
  // Alt + the right edge: a bend out to the next note; the vibrato stays.
  const Vec2 end2 = r.view->notePoint(n, 0.989);
  r.d.drag(end2, {end2.x - kBeat, end2.y}, 5, mod::kAlt);
  r.d.step();
  CHECK(r.history() == h + 2);
  CHECK(doc::findNoteCurves(r.clip(), n)->bendOut.other == ids[1]);
  CHECK(vibratoOf(r, n) == v);
  CHECK(r.note(n).length == 3 * kQ);
  // The body, on the wave: a move. (The note is drawn along its wave and hit there.)
  const Vec2 body = r.view->notePoint(n, 0.3);
  r.d.drag(body, {body.x + kBeat, body.y}, 5);
  r.d.step();
  CHECK(r.history() == h + 3);
  CHECK(r.note(n).start == kQ);
  CHECK(vibratoOf(r, n) == v);
}

TEST_CASE("roll paint: note paths are flattened once per clip snapshot — a repaint, a scroll or a selection change evaluates nothing (M14)") {
  Roll r;
  REQUIRE(r.d.enableGpu().ok());  // the note area paints with the theme
  r.focusNotes();
  std::vector<doc::Note> notes;
  for (int i = 0; i < 40; ++i) notes.push_back(Roll::plain(i * (kQ / 4), kQ, i % 12));
  const std::vector<doc::NoteId> ids = r.addNotes(notes);
  std::vector<doc::NoteCurves> curves;
  doc::NoteVibrato v;
  v.depthCents = 30.0f;
  v.rate = 6.0f;
  for (const doc::NoteId id : ids) curves.push_back(doc::NoteCurves{id, {}, {}, {}, v});
  std::sort(curves.begin(), curves.end(), [](const doc::NoteCurves& a, const doc::NoteCurves& b) { return a.note < b.note; });
  REQUIRE(r.ctx.store().dispatch(doc::SetNoteCurves{r.ref, curves}).ok());
  r.d.advance(0.1);
  const uint64_t built = r.view->pathBuilds();
  CHECK(built >= 40);
  // Repaints of the same snapshot: scrolling, zooming rows, selecting, hovering.
  r.view->scrollToTick(2.0 * kQ);
  r.d.advance(0.1);
  r.select({ids[3], ids[4]});
  r.d.advance(0.1);
  r.d.move(r.view->notePoint(ids[20], 0.5));
  r.d.advance(0.1);
  CHECK(r.view->pathBuilds() == built);
  // An edit of one note makes a new snapshot: the paths are rebuilt once, then cached again.
  r.d.click(r.view->notePoint(ids[20], 0.5));
  (void)r.d.press("Up");
  r.d.advance(0.1);
  const uint64_t rebuilt = r.view->pathBuilds();
  CHECK(rebuilt > built);
  CHECK(rebuilt <= built + 45);
  r.d.advance(0.3);
  CHECK(r.view->pathBuilds() == rebuilt);
}

TEST_CASE("render: pianoroll-vibrato (vibrato with fades: the wave, its envelope and the grips) matches the golden image") { checkGolden("pianoroll-vibrato"); }
