// T7.11a: derived bends between notes (model.h NoteBend, pq/document/note_bend.h; QUESTIONS Q289 option b) and the
// batched curve command (M7 review M13).

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <variant>
#include <vector>

#include "note_fix.h"
#include "pq/document/ji_object.h"

using namespace edit_test;
using doctest_fixture::makeClip;
using note_fix::BendFix;
using note_fix::centsOf;
using pq::kPpq;
using pq::tuning::AnchorKind;
using pq::tuning::PitchId;
using pq::tuning::PitchKind;
using pq::tuning::Rational;

namespace {

double polyline(const std::vector<Breakpoint>& pts, double x) {
  REQUIRE(!pts.empty());
  auto it = std::upper_bound(pts.begin(), pts.end(), x, [](double v, const Breakpoint& p) { return v < static_cast<double>(p.tick); });
  if (it == pts.begin()) return pts.front().value;
  const Breakpoint& a = *(it - 1);
  if (it == pts.end() || static_cast<double>(a.tick) == x) return a.value;
  return a.value + (it->value - a.value) * (x - static_cast<double>(a.tick)) / static_cast<double>(it->tick - a.tick);
}

size_t countCommands(const ops::Edit& e, auto&& pred) {
  size_t n = 0;
  for (const Command& c : e.batch().commands) n += pred(c) ? 1 : 0;
  return n;
}

}  // namespace

TEST_CASE("a bend names its neighbour: the glide follows either note without another edit") {
  BendFix f;
  const Note a = f.add(0, PitchId::fromDegree(0));
  const Note b = f.add(kPpq, PitchId::fromDegree(7));
  commit(f.store, ops::setNoteBend(f.doc(), f.ref, keyOf(b), ops::NoteEdge::Start, kPpq / 2, JiShape{}, a.id));
  const NoteCurves* nc = findNoteCurves(f.clip(), b.id);
  REQUIRE(nc != nullptr);
  CHECK(nc->pitch.empty());
  CHECK(nc->bendIn == NoteBend{kPpq / 2, JiShape{}, a.id});
  CHECK_FALSE(nc->bendOut.set());

  // In from the neighbour's pitch (7 steps of 12-EDO below), arriving on the note's own after the bend's length.
  std::vector<Breakpoint> eff = f.effective(f.clip(), b);
  REQUIRE(eff.size() >= 3);
  CHECK(eff.front().tick == 0);
  CHECK(eff.front().value == doctest::Approx(-700.0).epsilon(1e-9));
  CHECK(polyline(eff, kPpq / 4) == doctest::Approx(-350.0).epsilon(1e-4));  // the S-curve's middle
  CHECK(polyline(eff, kPpq / 2) == doctest::Approx(0.0).epsilon(1e-9));
  CHECK(eff.back().tick == kPpq);
  CHECK(eff.back().value == 0.0f);
  for (const Breakpoint& p : eff) {
    CHECK(p.curve == 0.0f);
    CHECK_FALSE(p.id.valid());
  }
  // The polyline is the curve within the tolerance, everywhere.
  const NoteBendTargets targets = noteBendTargets(*nc, NotePitchIndex(f.pitches(f.clip())));
  REQUIRE(targets.in.has_value());
  double worst = 0.0;
  for (Ticks t = 0; t <= kPpq; ++t)
    worst = std::max(worst, std::abs(polyline(eff, static_cast<double>(t)) - noteCurveCents(*nc, b.length, targets, static_cast<double>(t))));
  CHECK(worst <= 0.05 + 1e-3);  // + float32 breakpoint values around 700 ¢

  // The neighbour moves up two steps: the same record now glides in from 5 steps below. Nothing was re-done.
  const NoteKey ka[] = {keyOf(a)};
  commit(f.store, ops::transposeNotes(f.doc(), f.ref, ka, 2, ops::StepUnit::Grid));
  CHECK(*findNoteCurves(f.clip(), b.id) == *nc);
  eff = f.effective(f.clip(), f.noteAt(f.clip(), kPpq));
  CHECK(eff.front().value == doctest::Approx(-500.0).epsilon(1e-9));
  // The note itself moves: the glide's far end stays on the neighbour.
  const NoteKey kb[] = {keyOf(b)};
  commit(f.store, ops::transposeNotes(f.doc(), f.ref, kb, 3, ops::StepUnit::Grid));
  eff = f.effective(f.clip(), f.noteAt(f.clip(), kPpq));
  CHECK(eff.front().value == doctest::Approx(-800.0).epsilon(1e-9));
  CHECK(polyline(eff, kPpq / 2) == doctest::Approx(0.0).epsilon(1e-9));
  f.roundTrips();
}

TEST_CASE("bend out, both bends and the free curve add up; shapes; jumps of the free curve survive") {
  BendFix f;
  const Note a = f.add(0, PitchId::fromDegree(0));
  const Note b = f.add(kPpq, PitchId::fromDegree(4), 2 * kPpq);
  const Note c = f.add(3 * kPpq, PitchId::fromDegree(9));
  JiShape linear;
  linear.kind = JiShape::LogLinear;
  JiShape bezier;
  bezier.kind = JiShape::Bezier;
  bezier.x1 = 0.2f;
  bezier.y1 = 0.9f;
  bezier.x2 = 0.5f;
  bezier.y2 = 1.2f;
  commit(f.store, ops::setNoteBend(f.doc(), f.ref, keyOf(b), ops::NoteEdge::Start, kPpq / 4, linear, a.id));
  commit(f.store, ops::setNoteBend(f.doc(), f.ref, keyOf(b), ops::NoteEdge::End, kPpq / 2, bezier, c.id));
  // A free curve on top: a vibrato-like wiggle with a jump in the middle of the note.
  const std::vector<Breakpoint> free = {{0, 0.0f, 0.5f, {}}, {kPpq, 20.0f, 0.0f, {}}, {kPpq, -20.0f, 0.0f, {}}, {2 * kPpq, 0.0f, 0.0f, {}}};
  commit(f.store, ops::setNoteCurve(f.doc(), f.ref, keyOf(b), free));
  const NoteCurves* nc = findNoteCurves(f.clip(), b.id);
  REQUIRE(nc != nullptr);
  CHECK(nc->pitch == free);
  CHECK(nc->bendIn.other == a.id);   // SetNoteCurve keeps the bends
  CHECK(nc->bendOut.other == c.id);
  CHECK(nc->bendOut.shape == bezier);

  const std::vector<Breakpoint> eff = f.effective(f.clip(), b);
  const NoteBendTargets targets = noteBendTargets(*nc, NotePitchIndex(f.pitches(f.clip())));
  CHECK(*targets.in == doctest::Approx(-400.0).epsilon(1e-9));
  CHECK(*targets.out == doctest::Approx(500.0).epsilon(1e-9));
  CHECK(eff.front().value == doctest::Approx(-400.0).epsilon(1e-6));
  CHECK(polyline(eff, kPpq / 8) == doctest::Approx(-200.0 + automationShape(0.125, 0.5) * 20.0).epsilon(1e-3));  // linear in + free
  CHECK(eff.back().tick == 2 * kPpq);
  CHECK(eff.back().value == doctest::Approx(500.0).epsilon(1e-6));  // arrives on the next note's pitch
  // The jump at the middle is two points on one tick.
  const auto jump = std::adjacent_find(eff.begin(), eff.end(), [](const Breakpoint& x, const Breakpoint& y) { return x.tick == y.tick; });
  REQUIRE(jump != eff.end());
  CHECK(jump->tick == kPpq);
  CHECK(jump->value == doctest::Approx(20.0));
  CHECK((jump + 1)->value == doctest::Approx(-20.0));
  double worst = 0.0;
  for (Ticks t = 0; t <= 2 * kPpq; ++t) {
    if (t == kPpq) continue;  // the jump: both values are in the polyline
    worst = std::max(worst, std::abs(polyline(eff, static_cast<double>(t)) - noteCurveCents(*nc, b.length, targets, static_cast<double>(t))));
  }
  CHECK(worst <= 0.05 + 1e-3);
  // A coarser tolerance costs fewer points.
  CHECK(f.effective(f.clip(), b, 1.0).size() < eff.size());
  // A bend longer than the note is clamped to it by the builder; an unresolvable neighbour does nothing.
  commit(f.store, ops::setNoteBend(f.doc(), f.ref, keyOf(c), ops::NoteEdge::Start, 100 * kPpq, JiShape{}, b.id));
  CHECK(findNoteCurves(f.clip(), c.id)->bendIn.length == kPpq);
  NoteCurves alone = *nc;
  CHECK(effectiveNoteCurve(alone, b.length, NoteBendTargets{}) == free);  // no targets: the free curve as stored
  f.roundTrips();

  // Clear Bends keeps the free curve; a second call is a no-op.
  const NoteKey keys[] = {keyOf(b), keyOf(c), keyOf(a)};
  commit(f.store, ops::clearNoteBends(f.doc(), f.ref, keys));
  REQUIRE(findNoteCurves(f.clip(), b.id) != nullptr);
  CHECK(*findNoteCurves(f.clip(), b.id) == NoteCurves{b.id, free, {}, {}});
  CHECK(findNoteCurves(f.clip(), c.id) == nullptr);  // it had nothing else
  CHECK(ops::clearNoteBends(f.doc(), f.ref, keys)->empty());
}

TEST_CASE("bend commands: validation, one record per note, removing a bend") {
  BendFix f;
  const Note a = f.add(0, PitchId::fromDegree(0));
  const Note b = f.add(kPpq, PitchId::fromDegree(7));
  auto refused = [&](const Command& c) {
    const Snapshot before = f.store.current();
    CHECK_FALSE(f.store.dispatch(c).ok());
    CHECK(f.store.current() == before);
  };
  const NoteBend good{kPpq / 2, JiShape{}, a.id};
  refused(SetNoteCurves{f.ref, {NoteCurves{b.id, {}, NoteBend{kPpq, JiShape{}, b.id}, {}}}});           // itself
  refused(SetNoteCurves{f.ref, {NoteCurves{b.id, {}, NoteBend{kPpq, JiShape{}, NoteId{999999}}, {}}}});  // not in the clip
  refused(SetNoteCurves{f.ref, {NoteCurves{b.id, {}, NoteBend{0, JiShape{}, a.id}, {}}}});               // no length
  refused(SetNoteCurves{f.ref, {NoteCurves{b.id, {}, NoteBend{kPpq, JiShape{}, NoteId{}}, {}}}});        // a length without a neighbour
  refused(SetNoteCurves{f.ref, {NoteCurves{NoteId{999999}, {}, good, {}}}});                             // no such note
  refused(SetNoteCurves{f.ref, {NoteCurves{b.id, {}, good, {}}, NoteCurves{b.id, {}, {}, good}}});       // two records
  refused(SetNoteCurves{f.ref, {NoteCurves{b.id, {{0, 1e9f, 0.0f, {}}}, good, {}}}});                    // an invalid curve
  JiShape bad;
  bad.x1 = 2.0f;
  refused(SetNoteCurves{f.ref, {NoteCurves{b.id, {}, NoteBend{kPpq, bad, a.id}, {}}}});
  CHECK_FALSE(ops::setNoteBend(f.doc(), f.ref, keyOf(b), ops::NoteEdge::Start, kPpq, JiShape{}, b.id).ok());
  CHECK_FALSE(ops::setNoteBend(f.doc(), f.ref, keyOf(b), ops::NoteEdge::Start, kPpq, JiShape{}, NoteId{999999}).ok());

  REQUIRE(f.store.dispatch(SetNoteCurves{f.ref, {NoteCurves{b.id, {}, good, {}}}}).ok());
  const Snapshot with = f.store.current();
  REQUIRE(f.store.dispatch(SetNoteCurves{f.ref, {NoteCurves{b.id, {}, good, {}}}}).ok());
  CHECK(f.store.current() == with);  // the same record: nothing changes, no history entry
  // The free curve is set and removed without touching the bend; removing the bend then removes the entry.
  const std::vector<Breakpoint> free = {{0, 0.0f, 0.0f, {}}, {100, 30.0f, 0.0f, {}}};
  REQUIRE(f.store.dispatch(SetNoteCurve{f.ref, keyOf(b), free}).ok());
  CHECK(*findNoteCurves(f.clip(), b.id) == NoteCurves{b.id, free, good, {}});
  REQUIRE(f.store.dispatch(SetNoteCurve{f.ref, keyOf(b), {}}).ok());
  CHECK(*findNoteCurves(f.clip(), b.id) == NoteCurves{b.id, {}, good, {}});
  commit(f.store, ops::setNoteBend(f.doc(), f.ref, keyOf(b), ops::NoteEdge::Start, 0, JiShape{}, NoteId{}));
  CHECK(f.clip().curves == nullptr);
  CHECK(ops::setNoteBend(f.doc(), f.ref, keyOf(b), ops::NoteEdge::Start, 0, JiShape{}, NoteId{})->empty());
}

TEST_CASE("deleting a bend's neighbour drops the bend in the same edit") {
  BendFix f;
  const Note a = f.add(0, PitchId::fromDegree(0));
  const Note b = f.add(kPpq, PitchId::fromDegree(7));
  const Note c = f.add(2 * kPpq, PitchId::fromDegree(9));
  commit(f.store, ops::setNoteBend(f.doc(), f.ref, keyOf(b), ops::NoteEdge::Start, kPpq / 2, JiShape{}, a.id));
  commit(f.store, ops::setNoteBend(f.doc(), f.ref, keyOf(b), ops::NoteEdge::End, kPpq / 2, JiShape{}, c.id));
  commit(f.store, ops::setNoteBend(f.doc(), f.ref, keyOf(c), ops::NoteEdge::Start, kPpq / 2, JiShape{}, b.id));
  const NoteKey ka[] = {keyOf(a)};
  commit(f.store, ops::deleteNotes(f.doc(), f.ref, ka));  // one undo step (commit checks it)
  const NoteCurves* nb = findNoteCurves(f.clip(), b.id);
  REQUIRE(nb != nullptr);
  CHECK_FALSE(nb->bendIn.set());
  CHECK(nb->bendOut.other == c.id);
  CHECK(validateClipJi(f.clip()).ok());
  // The bent note goes: its record goes, and so does the bend that aimed at it (c's record was only that).
  const NoteKey kb[] = {keyOf(b)};
  commit(f.store, ops::deleteNotes(f.doc(), f.ref, kb));
  CHECK(f.clip().curves == nullptr);
  REQUIRE(f.store.undo());
  CHECK(findNoteCurves(f.clip(), c.id)->bendIn.other == b.id);
}

TEST_CASE("copies re-point bends: clip duplicate, note duplicate, paste, split, consolidate") {
  BendFix f;
  IdSource& ids = f.store.ids();
  const Note a = f.add(0, PitchId::fromDegree(0));
  const Note b = f.add(kPpq, PitchId::fromDegree(7));
  const Note c = f.add(kBar + kPpq, PitchId::fromDegree(9));  // in bar 2
  commit(f.store, ops::setNoteBend(f.doc(), f.ref, keyOf(b), ops::NoteEdge::Start, kPpq / 2, JiShape{}, a.id));
  commit(f.store, ops::setNoteBend(f.doc(), f.ref, keyOf(b), ops::NoteEdge::End, kPpq / 2, JiShape{}, c.id));

  SUBCASE("a duplicated clip's bends glide between the copies") {
    const ClipRef sel[] = {f.ref};
    commit(f.store, ops::duplicateClips(f.doc(), ids, sel));
    const NoteClip& cp = f.clipAt(1);
    const Note& b2 = f.noteAt(cp, kPpq);
    const NoteCurves* nc = findNoteCurves(cp, b2.id);
    REQUIRE(nc != nullptr);
    CHECK(nc->bendIn.other == f.noteAt(cp, 0).id);
    CHECK(nc->bendOut.other == f.noteAt(cp, kBar + kPpq).id);
    CHECK(f.effective(cp, b2).front().value == doctest::Approx(-700.0).epsilon(1e-9));
    f.roundTrips();
  }
  SUBCASE("duplicated notes: a copied neighbour is followed, another one stays the original; one curve command") {
    const NoteKey both[] = {keyOf(a), keyOf(b)};
    auto e = ops::duplicateNotes(f.doc(), ids, f.ref, both, 2 * kPpq);
    REQUIRE(e.ok());
    CHECK(countCommands(*e, [](const Command& x) { return std::holds_alternative<SetNoteCurves>(x.v); }) == 1);
    CHECK(countCommands(*e, [](const Command& x) { return std::holds_alternative<SetNoteCurve>(x.v); }) == 0);
    commit(f.store, e);
    const Note& a2 = f.noteAt(f.clip(), 2 * kPpq);
    const Note& b2 = f.noteAt(f.clip(), 3 * kPpq);
    const NoteCurves* nc = findNoteCurves(f.clip(), b2.id);
    REQUIRE(nc != nullptr);
    CHECK(nc->bendIn.other == a2.id);  // copied with it
    CHECK(nc->bendOut.other == c.id);  // not copied: still a note of the clip
  }
  SUBCASE("a paste into another clip keeps the bends between pasted notes and drops the others") {
    ClipPtr other = makeClip(ids, 8 * kBar, kBar, {}, "P");
    REQUIRE(f.store.dispatch(AddClip{f.bass, other}).ok());
    const ClipRef to{f.bass, other->id};
    const Note src[] = {a, b};
    const NoteCurves curves[] = {*findNoteCurves(f.clip(), b.id)};
    commit(f.store, ops::addNotes(f.doc(), ids, to, src, curves));
    const NoteClip& pc = f.clipAt(1);
    const NoteCurves* nc = findNoteCurves(pc, f.noteAt(pc, kPpq).id);
    REQUIRE(nc != nullptr);
    CHECK(nc->bendIn.other == f.noteAt(pc, 0).id);
    CHECK_FALSE(nc->bendOut.set());  // c was not pasted and is no note of that clip
    CHECK(validateClipJi(pc).ok());
  }
  SUBCASE("a split drops the bend across the cut and keeps the one inside a piece") {
    const ClipRef sel[] = {f.ref};
    const Ticks cut[] = {kBar};
    commit(f.store, ops::splitClips(f.doc(), ids, sel, cut));
    const NoteClip& left = f.clipAt(0);
    const NoteCurves* nc = findNoteCurves(left, b.id);
    REQUIRE(nc != nullptr);
    CHECK(nc->bendIn.other == a.id);
    CHECK_FALSE(nc->bendOut.set());
    CHECK(f.clipAt(1).curves == nullptr);
    // Consolidating the pieces keeps what is there.
    const ClipRef pieces[] = {{f.bass, f.clipAt(0).id}, {f.bass, f.clipAt(1).id}};
    commit(f.store, ops::consolidateClips(f.doc(), ids, pieces));
    const NoteClip& merged = f.clipAt(0);
    CHECK(findNoteCurves(merged, b.id)->bendIn.other == a.id);
  }
  SUBCASE("consolidating a looped clip: each pass's bend glides to that pass's notes") {
    REQUIRE(f.store.dispatch(SetClipLoop{f.ref, ClipLoop{true, 0, kBar}}).ok());
    const ClipRef sel[] = {f.ref};
    commit(f.store, ops::consolidateClips(f.doc(), ids, sel));
    const NoteClip& merged = f.clipAt(0);
    for (const Ticks bar : {Ticks{0}, kBar, 2 * kBar}) {
      const NoteCurves* nc = findNoteCurves(merged, f.noteAt(merged, bar + kPpq).id);
      REQUIRE(nc != nullptr);
      CHECK(nc->bendIn.other == f.noteAt(merged, bar).id);
      CHECK_FALSE(nc->bendOut.set());  // c is outside the loop: not in the merged clip
    }
    CHECK(validateClipJi(merged).ok());
  }
}
