// T7.11a: what names a note survives copies and deletions (M7 review H2, H3), and a note anchored to another note
// takes the anchor's pitch where it starts itself (M5).

#include <doctest/doctest.h>

#include <cmath>
#include <optional>
#include <vector>

#include "note_fix.h"

using namespace edit_test;
using note_fix::BendFix;
using note_fix::centsOf;
using pq::kPpq;
using pq::tuning::AnchorKind;
using pq::tuning::PitchId;
using pq::tuning::PitchKind;
using pq::tuning::Rational;

// ---- M7 review H2 --------------------------------------------------------------------------------------------------

TEST_CASE("H2: a copied clip's notes are anchored to the copied notes; the declared harmonic moves with them") {
  BendFix f;
  IdSource& ids = f.store.ids();
  const Note a = f.add(0, PitchId::fromDegree(3));
  const Note b = f.add(kPpq, PitchId::fromRatio(Rational{3, 2}, AnchorKind::Note, a.id.value));
  const Note c = f.add(2 * kPpq, PitchId::fromRatio(Rational{5, 4}, AnchorKind::Note, b.id.value));  // a chain
  auto declared = ops::makeJiGroup(f.doc(), ids, f.ref, keyOf(a), 4);
  commit(f.store, declared);
  const double hzA = f.hz(a.id);
  REQUIRE(f.hz(b.id) == hzA * 1.5);
  const std::optional<GroupHarmonics> before = groupHarmonics(f.clip(), *findJiGroup(f.clip(), declared->jiGroups.at(0)));
  REQUIRE(before.has_value());

  const ClipRef sel[] = {f.ref};
  commit(f.store, ops::duplicateClips(f.doc(), ids, sel));
  const NoteClip& cp = f.clipAt(1);
  const Note& a2 = f.noteAt(cp, 0);
  const Note& b2 = f.noteAt(cp, kPpq);
  const Note& c2 = f.noteAt(cp, 2 * kPpq);
  CHECK(a2.id != a.id);
  CHECK(b2.pitch == PitchId::fromRatio(Rational{3, 2}, AnchorKind::Note, a2.id.value));
  CHECK(c2.pitch == PitchId::fromRatio(Rational{5, 4}, AnchorKind::Note, b2.id.value));
  REQUIRE(f.hzIn(cp, b2.id).has_value());  // it sounds
  CHECK(*f.hzIn(cp, b2.id) == hzA * 1.5);
  CHECK(*f.hzIn(cp, c2.id) == hzA * 1.5 * 1.25);
  const JiGroup* copied = findJiGroupByAnchor(cp, a2.id);  // T7.13: the declared harmonic is a JI group
  REQUIRE(copied != nullptr);
  CHECK(copied->id != declared->jiGroups.at(0));
  CHECK(copied->index == 4);
  const std::optional<GroupHarmonics> after = groupHarmonics(cp, *copied);
  REQUIRE(after.has_value());
  CHECK(after->fundamental == before->fundamental);
  CHECK(after->proportion == before->proportion);
  // The source is untouched.
  CHECK(f.hz(b.id) == hzA * 1.5);
  CHECK(findJiGroupByAnchor(f.clip(), a.id) != nullptr);
  f.roundTrips();
}

TEST_CASE("H2: harmonic steps of reference ratios are re-keyed in a copy") {
  BendFix f;
  IdSource& ids = f.store.ids();
  const Note a = f.add(0, PitchId::fromRatio(Rational{5, 4}));
  const NoteKey ka[] = {keyOf(a)};
  commit(f.store, ops::transposeNotes(f.doc(), f.ref, ka, 1, ops::StepUnit::Grid));  // 5/4 → 3/2 on the fundamental 1/4
  REQUIRE(f.clip().fundamental.steps.size() == 1);
  REQUIRE(f.clip().fundamental.steps[0].note == a.id);
  const ClipRef sel[] = {f.ref};
  commit(f.store, ops::duplicateClips(f.doc(), ids, sel));
  const NoteClip& cp = f.clipAt(1);
  REQUIRE(cp.fundamental.steps.size() == 1);
  CHECK(cp.fundamental.steps[0].note == f.noteAt(cp, 0).id);
  CHECK(virtualFundamental(cp) == virtualFundamental(f.clip()));
}

// ---- M7 review H3 --------------------------------------------------------------------------------------------------

TEST_CASE("H3: deleting the anchor note keeps the notes anchored to it sounding at their pitch") {
  BendFix f;
  // T7.13: the leader is the LOWEST dependant (was: the first) — "promote the lowest remaining member" (plan R2);
  // here c (5/4) instead of b (3/2), so b is exactly 6/5 of it and the group's index becomes c's number, 5.
  SUBCASE("a tempered anchor declared harmonic 4: the lowest dependant takes over, the others keep exact ratios to it") {
    const Note a = f.add(0, PitchId::fromDegree(3));
    const Note b = f.add(kPpq, PitchId::fromRatio(Rational{3, 2}, AnchorKind::Note, a.id.value));
    const Note c = f.add(2 * kPpq, PitchId::fromRatio(Rational{5, 4}, AnchorKind::Note, a.id.value));
    const Note d = f.add(3 * kPpq, PitchId::fromRatio(Rational{9, 8}, AnchorKind::Note, c.id.value));  // through c
    auto declared = ops::makeJiGroup(f.doc(), f.store.ids(), f.ref, keyOf(a), 4);
    commit(f.store, declared);
    const JiGroupId group = declared->jiGroups.at(0);
    const double hb = f.hz(b.id), hc = f.hz(c.id), hd = f.hz(d.id);
    const std::optional<GroupHarmonics> before = groupHarmonics(f.clip(), *findJiGroup(f.clip(), group));
    REQUIRE(before.has_value());
    REQUIRE(before->proportion == std::vector<int64_t>{4, 5, 6});
    const double fundBefore = f.hz(a.id) * before->fundamental.toDouble();

    const NoteKey ka[] = {keyOf(a)};
    const Snapshot was = commit(f.store, ops::deleteNotes(f.doc(), f.ref, ka));  // one undo step
    REQUIRE(f.clip().notes.size() == 3);
    CHECK(f.hz(b.id) == doctest::Approx(hb).epsilon(1e-12));
    CHECK(f.hz(c.id) == doctest::Approx(hc).epsilon(1e-12));
    CHECK(f.hz(d.id) == doctest::Approx(hd).epsilon(1e-12));
    // c carries the pitch (Cents: a 12-EDO degree times 5/4 is no ratio to the reference); b is exactly 6/5 of it.
    const Note& nb = f.noteAt(f.clip(), kPpq);
    const Note& nc = f.noteAt(f.clip(), 2 * kPpq);
    CHECK(nc.pitch.kind() == PitchKind::Cents);
    CHECK(nb.pitch == PitchId::fromRatio(Rational{6, 5}, AnchorKind::Note, c.id.value));
    CHECK(f.noteAt(f.clip(), 3 * kPpq).pitch == d.pitch);  // its anchor is still there
    // The group moved to c with the number c had (5 of 4:5:6): every number and the fundamental stay.
    const JiGroup* g = findJiGroup(f.clip(), group);
    REQUIRE(g != nullptr);
    CHECK(g->anchor == c.id);
    CHECK(g->index == 5);
    const std::optional<GroupHarmonics> after = groupHarmonics(f.clip(), *g);
    REQUIRE(after.has_value());
    // d (9/8 above c) hangs on the new anchor, so it is a member now and refines the numbering: 5 : 45/8 : 6.
    CHECK(after->proportion == std::vector<int64_t>{40, 45, 48});
    CHECK(f.hz(c.id) * after->fundamental.toDouble() * 8.0 == doctest::Approx(fundBefore).epsilon(1e-12));
    REQUIRE(f.store.undo());
    CHECK(f.store.current() == was);
  }
  SUBCASE("an anchor that is a ratio to the reference: the dependants become exact ratios to the reference") {
    const Note a = f.add(0, PitchId::fromRatio(Rational{5, 4}));
    const Note b = f.add(kPpq, PitchId::fromRatio(Rational{3, 2}, AnchorKind::Note, a.id.value));
    const Note c = f.add(2 * kPpq, PitchId::fromRatio(Rational{7, 6}, AnchorKind::Note, b.id.value));
    commit(f.store, ops::makeJiGroup(f.doc(), f.store.ids(), f.ref, keyOf(a), 5));
    const double hb = f.hz(b.id), hc = f.hz(c.id);
    const NoteKey gone[] = {keyOf(a), keyOf(b)};  // the chain's two links at once
    commit(f.store, ops::deleteNotes(f.doc(), f.ref, gone));
    CHECK(f.noteAt(f.clip(), 2 * kPpq).pitch == PitchId::fromRatio(Rational{35, 16}));  // 5/4 · 3/2 · 7/6, exact
    CHECK(f.hz(c.id) == doctest::Approx(hc).epsilon(1e-15));
    CHECK(f.clip().groups == nullptr);  // the group went with its anchor and its only member
    (void)hb;
  }
  SUBCASE("a single dependant with a curve of its own becomes Cents and keeps its curve") {
    const Note a = f.add(0, PitchId::fromDegree(3));
    const Note b = f.add(kPpq, PitchId::fromRatio(Rational{7, 4}, AnchorKind::Note, a.id.value));
    const std::vector<Breakpoint> free = {{0, 0.0f, 0.0f, {}}, {100, 30.0f, 0.0f, {}}};
    commit(f.store, ops::setNoteCurve(f.doc(), f.ref, keyOf(b), free));
    const double hb = f.hz(b.id);
    const NoteKey ka[] = {keyOf(a)};
    commit(f.store, ops::deleteNotes(f.doc(), f.ref, ka));
    CHECK(f.noteAt(f.clip(), kPpq).pitch.kind() == PitchKind::Cents);
    CHECK(f.hz(b.id) == doctest::Approx(hb).epsilon(1e-12));
    CHECK(findNoteCurves(f.clip(), b.id)->pitch == free);
  }
  SUBCASE("deleting notes nobody is anchored to changes nothing else") {
    const Note a = f.add(0, PitchId::fromDegree(3));
    const Note b = f.add(kPpq, PitchId::fromRatio(Rational{3, 2}, AnchorKind::Note, a.id.value));
    const NoteKey kb[] = {keyOf(b)};
    auto e = ops::deleteNotes(f.doc(), f.ref, kb);
    REQUIRE(e.ok());
    CHECK(e->batch().commands.size() == 1);
  }
}

TEST_CASE("H3: a split that parts a note from its anchor keeps both halves sounding") {
  BendFix f;
  IdSource& ids = f.store.ids();
  const Note a = f.add(0, PitchId::fromDegree(3), kPpq);                                            // bar 1
  const Note b = f.add(kBar, PitchId::fromRatio(Rational{3, 2}, AnchorKind::Note, a.id.value));         // bar 2
  const Note c = f.add(kBar + kPpq, PitchId::fromRatio(Rational{5, 4}, AnchorKind::Note, a.id.value));  // bar 2
  auto declared = ops::makeJiGroup(f.doc(), ids, f.ref, keyOf(a), 4);
  commit(f.store, declared);
  const double ha = f.hz(a.id), hb = f.hz(b.id), hc = f.hz(c.id);
  const ClipRef sel[] = {f.ref};
  const Ticks cut[] = {kBar};
  commit(f.store, ops::splitClips(f.doc(), ids, sel, cut));
  const NoteClip& left = f.clipAt(0);
  const NoteClip& right = f.clipAt(1);
  REQUIRE(left.notes.size() == 1);
  REQUIRE(right.notes.size() == 2);
  CHECK(*f.hzIn(left, a.id) == ha);
  REQUIRE(f.hzIn(right, b.id).has_value());  // the right clip is not silent
  REQUIRE(f.hzIn(right, c.id).has_value());
  CHECK(*f.hzIn(right, b.id) == doctest::Approx(hb).epsilon(1e-12));
  CHECK(*f.hzIn(right, c.id) == doctest::Approx(hc).epsilon(1e-12));
  // T7.13: the lowest parted member leads (c, 5/4; was: the first, b): b is exactly 6/5 of it.
  CHECK(f.noteAt(right, kBar).pitch == PitchId::fromRatio(Rational{6, 5}, AnchorKind::Note, c.id.value));
  // The left piece keeps the group; in the right piece a new group is anchored on c with the number it had (5).
  const JiGroup* gl = findJiGroupByAnchor(left, a.id);
  REQUIRE(gl != nullptr);
  CHECK(gl->id == declared->jiGroups.at(0));
  CHECK(gl->index == 4);
  const JiGroup* gr = findJiGroupByAnchor(right, c.id);
  REQUIRE(gr != nullptr);
  CHECK(gr->id != gl->id);
  CHECK(gr->index == 5);
  f.roundTrips();
}

TEST_CASE("H3: Duplicate Time drops hidden notes; a playing note anchored to one keeps its pitch") {
  BendFix f;
  IdSource& ids = f.store.ids();
  const Note a = f.add(0, PitchId::fromRatio(Rational{5, 4}), kPpq);                                    // bar 1
  const Note b = f.add(kBar, PitchId::fromRatio(Rational{3, 2}, AnchorKind::Note, a.id.value));         // bar 2
  const double hb = f.hz(b.id);
  commit(f.store, ops::duplicateTime(f.doc(), ids, kBar, 2 * kBar));  // bar 2 again at bar 3
  // The copy holds bar 2 only (the anchor in bar 1 is not played by that piece).
  const NoteClip* copy = nullptr;
  for (const ClipSlot& s : f.track(f.bass).clips)
    if (s.clip->start == 2 * kBar) copy = s.clip.get();
  REQUIRE(copy != nullptr);
  REQUIRE(copy->notes.size() == 1);
  const Note& b2 = *copy->notes.begin();
  CHECK(b2.pitch == PitchId::fromRatio(Rational{15, 8}));  // 5/4 · 3/2: exact
  REQUIRE(f.hzIn(*copy, b2.id).has_value());
  CHECK(*f.hzIn(*copy, b2.id) == doctest::Approx(hb).epsilon(1e-15));
}

// ---- M7 review M5 --------------------------------------------------------------------------------------------------

TEST_CASE("M5: a note anchored to another note takes the anchor's pitch at its own start") {
  BendFix f;
  IdSource& ids = f.store.ids();
  const pq::tuning::Tuning t = buildTuning(*effectiveTuningRef(f.doc(), *findTrack(f.doc(), f.bass), &f.clip()).def).value();

  SUBCASE("the anchor has a pitch curve") {
    const Note a = f.add(0, PitchId::fromDegree(4), 4 * kPpq);
    // 0 ¢ for a beat, down 14 ¢ over the second beat, held.
    const std::vector<Breakpoint> curve = {{kPpq, 0.0f, 0.0f, {}}, {2 * kPpq, -14.0f, 0.0f, {}}};
    commit(f.store, ops::setNoteCurve(f.doc(), f.ref, keyOf(a), curve));
    const Note early = f.add(kPpq / 2, PitchId::fromRatio(Rational{3, 2}, AnchorKind::Note, a.id.value));
    const Note mid = f.add(kPpq + kPpq / 2, PitchId::fromRatio(Rational{3, 2}, AnchorKind::Note, a.id.value));
    const Note late = f.add(3 * kPpq, PitchId::fromRatio(Rational{3, 2}, AnchorKind::Note, a.id.value));
    const Note after = f.add(6 * kPpq, PitchId::fromRatio(Rational{3, 2}, AnchorKind::Note, a.id.value));  // the anchor has ended
    const Note chained = f.add(3 * kPpq + 1, PitchId::fromRatio(Rational{5, 4}, AnchorKind::Note, late.id.value));
    const double ha = f.hz(a.id);
    CHECK(f.hz(early.id) == ha * 1.5);  // before the curve moves: exact
    CHECK(centsOf(ha * 1.5, f.hz(mid.id)) == doctest::Approx(-7.0).epsilon(1e-9));
    CHECK(centsOf(ha * 1.5, f.hz(late.id)) == doctest::Approx(-14.0).epsilon(1e-9));  // pure against what sounds
    CHECK(centsOf(ha * 1.5, f.hz(after.id)) == doctest::Approx(-14.0).epsilon(1e-9));  // where the anchor ended
    CHECK(f.hz(chained.id) == f.hz(late.id) * 1.25);
    // The anchor's own pitch is its pitch at its start, as before.
    CHECK(ha == *pq::tuning::resolveHzDouble(PitchId::fromDegree(4), t, {}));
  }
  SUBCASE("the anchor bends in from its neighbour") {
    const Note n0 = f.add(0, PitchId::fromDegree(0));
    const Note a = f.add(kPpq, PitchId::fromDegree(2), 2 * kPpq);
    commit(f.store, ops::setNoteBend(f.doc(), f.ref, keyOf(a), ops::NoteEdge::Start, kPpq, GlideShape{}, n0.id));
    const Note on = f.add(kPpq + 1, PitchId::fromRatio(Rational{2}, AnchorKind::Note, a.id.value));          // just in the glide
    const Note half = f.add(kPpq + kPpq / 2, PitchId::fromRatio(Rational{2}, AnchorKind::Note, a.id.value));  // its middle
    const Note there = f.add(2 * kPpq, PitchId::fromRatio(Rational{2}, AnchorKind::Note, a.id.value));
    const double ha = f.hz(a.id);
    CHECK(centsOf(ha * 2.0, f.hz(on.id)) == doctest::Approx(-200.0).epsilon(1e-4));
    CHECK(centsOf(ha * 2.0, f.hz(half.id)) == doctest::Approx(-100.0).epsilon(1e-9));
    CHECK(f.hz(there.id) == ha * 2.0);
    // A bend that would depend on itself does nothing instead of silencing anything: a bends out to y, y is
    // anchored to `there`, and `there` takes a's pitch — inside that bend.
    const Note y = f.add(3 * kPpq, PitchId::fromRatio(Rational{3, 2}, AnchorKind::Note, there.id.value));
    commit(f.store, ops::setNoteBend(f.doc(), f.ref, keyOf(a), ops::NoteEdge::End, 2 * kPpq, GlideShape{}, y.id));
    for (const NotePitch& np : f.pitches(f.clip()).notes) CHECK(np.hz.has_value());
    CHECK(f.hz(there.id) == ha * 2.0);       // the bend out is not followed by what it depends on
    CHECK(f.hz(y.id) == ha * 2.0 * 1.5);
    CHECK(centsOf(ha * 2.0, f.hz(half.id)) == doctest::Approx(-100.0 + 1200.0 * std::log2(3.0) * 0.15625).epsilon(1e-9));  // a note that does not close the loop follows both bends: s(1/4) of the way out
  }
  // T7.13: this case was "the anchor follows a JI object's voice through a Tune to JI glide" (ops::jiFromNotes); the
  // object is gone, the same sound is a tuned group with a toJi warp.
  SUBCASE("the anchor is a warped member of a JI group (written in 12-EDO, tuned to JI over the first beat)") {
    const Note c = f.add(0, PitchId::fromDegree(0), 4 * kPpq);
    const Note e = f.add(0, PitchId::fromDegree(4), 4 * kPpq);
    const Note g = f.add(0, PitchId::fromDegree(7), 4 * kPpq);
    const NoteKey triad[] = {keyOf(c), keyOf(e), keyOf(g)};
    auto made = ops::tuneToJi(f.doc(), ids, f.ref, triad);
    commit(f.store, made);
    commit(f.store, ops::setJiWarps(f.doc(), f.ref, made->jiGroups.at(0), {JiWarp{0, kPpq, GlideShape{}, true}}));
    // e is 12-EDO at its start and 5/4 over the root from the end of the glide.
    const double root = f.hz(c.id);
    CHECK(f.hz(e.id) == *pq::tuning::resolveHzDouble(PitchId::fromDegree(4), t, {}));
    const Note fifthAtStart = f.add(1, PitchId::fromRatio(Rational{3, 2}, AnchorKind::Note, e.id.value));
    const Note fifthAfter = f.add(2 * kPpq, PitchId::fromRatio(Rational{3, 2}, AnchorKind::Note, e.id.value));
    CHECK(std::abs(centsOf(root * 1.25 * 1.5, f.hz(fifthAfter.id))) < 1e-9);
    CHECK(std::abs(centsOf(f.hz(e.id) * 1.5, f.hz(fifthAtStart.id))) < 0.1);  // the tempered third it starts on
  }
}
