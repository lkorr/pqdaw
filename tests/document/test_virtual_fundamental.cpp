// T3.6-fix1: the virtual fundamental in the model (user decision Q40; SPEC §4.3, §6.1, §6.2). A clip's
// reference-anchored Ratio notes share a fundamental derived from what is entered (doc::virtualFundamental: 1/lcm of
// their effective denominators); Up/Down step on its harmonics and the document remembers the fundamental a step
// used (ClipFundamental::steps), so reduced ratios stay exact and the fundamental survives undo, redo and a save; the
// explicit lock (b + index) overrides it.

#include <doctest/doctest.h>

#include <optional>
#include <string>
#include <vector>

#include "edit_helpers.h"
#include "pq/document/serialize.h"

using namespace pq::doc;
using namespace edit_test;
using doctest_fixture::makeClip;
using doctest_fixture::makeTrackPtr;
using ops::StepUnit;
using pq::tuning::AnchorKind;
using pq::tuning::PitchId;
using pq::tuning::Rational;

namespace {

struct Roll {
  DocumentStore store;
  TrackId track;
  ClipRef ref;

  Roll() {
    IdSource& ids = store.ids();
    track = ids.track();
    ClipPtr c = makeClip(ids, 0, 4 * pq::kTicksPerWhole, {}, "JI");
    ref = ClipRef{track, c->id};
    Batch b{"setup", {}};
    b.commands.push_back(AddTrack{makeTrackPtr(track, TrackKind::Midi, "JI"), {}, kAppend});
    b.commands.push_back(AddClip{track, c});
    REQUIRE(store.dispatch(b).ok());
  }
  const NoteClip& clip() const {
    const Track& t = *findTrackPtr(*store.current(), track);
    return *t.clips[static_cast<size_t>(findClipIndex(t, ref.clip))].clip;
  }
  std::optional<Rational> fundamental() const { return virtualFundamental(clip()); }
  const Note* note(NoteId id) const {
    for (const Note& n : clip().notes)
      if (n.id == id) return &n;
    return nullptr;
  }
  NoteKey key(NoteId id) const { return keyOf(*note(id)); }
  NoteId add(PitchId pitch, pq::Ticks start) {
    Note n;
    n.start = start;
    n.length = pq::kPpq;
    n.pitch = pitch;
    auto e = ops::addNotes(*store.current(), store.ids(), ref, std::vector<Note>{n});
    commit(store, e);
    return e->notes[0].id;
  }
  void step(NoteId id, int32_t steps, StepUnit unit = StepUnit::Grid) {
    const NoteKey k = key(id);
    commit(store, ops::transposeNotes(*store.current(), ref, std::span<const NoteKey>(&k, 1), steps, unit));
  }
  void remove(NoteId id) {
    const NoteKey k = key(id);
    commit(store, ops::deleteNotes(*store.current(), ref, std::span<const NoteKey>(&k, 1)));
  }
  Rational ratio(NoteId id) const { return note(id)->pitch.ratio(); }
};

PitchId ratio(int64_t n, int64_t d) { return PitchId::fromRatio(Rational(n, d)); }

}  // namespace

TEST_CASE("virtual fundamental: 5/4 → 6/4 → 7/4 by Up; 4/3 refines it to 1/12, deleting it coarsens it back") {
  Roll r;
  CHECK(r.fundamental() == std::nullopt);  // no Ratio notes
  const NoteId a = r.add(ratio(5, 4), 0);
  CHECK(r.fundamental() == Rational(1, 4));
  r.step(a, 1);
  CHECK(r.ratio(a) == Rational(3, 2));  // 6/4, stored reduced
  REQUIRE(r.clip().fundamental.steps.size() == 1);  // ... with the fundamental it is on
  CHECK(r.clip().fundamental.steps[0] == HarmonicStep{a, 4, Rational(3, 2)});
  CHECK(r.fundamental() == Rational(1, 4));
  r.step(a, 1);
  CHECK(r.ratio(a) == Rational(7, 4));  // not 2/1
  CHECK(r.clip().fundamental.steps.empty());  // 7/4 needs no memory: its own denominator is 4
  CHECK(r.fundamental() == Rational(1, 4));
  r.step(a, -2);
  CHECK(r.ratio(a) == Rational(5, 4));
  r.step(a, 1);
  CHECK(r.ratio(a) == Rational(3, 2));

  // Adding 4/3 moves the fundamental to 1/12 = 1/lcm(4, 3); deleting it moves it back to 1/4 (the 3/2 is 6/4).
  const NoteId b = r.add(ratio(4, 3), pq::kPpq);
  CHECK(r.fundamental() == Rational(1, 12));
  r.remove(b);
  CHECK(r.fundamental() == Rational(1, 4));
  // And with 7/4 instead of the remembered 3/2.
  r.step(a, 1);
  CHECK(r.ratio(a) == Rational(7, 4));
  const NoteId c = r.add(ratio(4, 3), pq::kPpq);
  CHECK(r.fundamental() == Rational(1, 12));
  r.remove(c);
  CHECK(r.fundamental() == Rational(1, 4));

  // Undo restores it, every step of the way (the memory is part of the clip snapshot).
  REQUIRE(r.store.undo());  // the delete of c
  CHECK(r.fundamental() == Rational(1, 12));
  REQUIRE(r.store.undo());  // the add of c
  CHECK(r.fundamental() == Rational(1, 4));
  REQUIRE(r.store.undo());  // 3/2 → 7/4
  CHECK(r.ratio(a) == Rational(3, 2));
  CHECK(r.clip().fundamental.steps.size() == 1);
  CHECK(r.fundamental() == Rational(1, 4));
  REQUIRE(r.store.undo());  // the delete of b
  CHECK(r.fundamental() == Rational(1, 12));
  REQUIRE(r.store.redo());
  CHECK(r.fundamental() == Rational(1, 4));
  // Up from the restored 3/2 is still 7/4.
  r.step(a, 1);
  CHECK(r.ratio(a) == Rational(7, 4));
}

TEST_CASE("virtual fundamental: Up/Down step on the shared fundamental; a step keeps it while the pitch is its own") {
  Roll r;
  const NoteId a = r.add(ratio(5, 4), 0);
  const NoteId b = r.add(ratio(4, 3), pq::kPpq);
  REQUIRE(r.fundamental() == Rational(1, 12));
  r.step(b, 1);
  CHECK(r.ratio(b) == Rational(17, 12));  // 16/12 → 17/12
  r.step(a, 1);
  CHECK(r.ratio(a) == Rational(4, 3));  // 15/12 → 16/12
  CHECK(r.fundamental() == Rational(1, 12));
  r.remove(b);
  CHECK(r.fundamental() == Rational(1, 12));  // the 4/3 was stepped on 1/12 and still has that pitch
  r.step(a, -1);
  CHECK(r.ratio(a) == Rational(5, 4));  // 16/12 → 15/12
  CHECK(r.fundamental() == Rational(1, 12));
  // A pitch set by other means (a move to another ratio) drops the memory: the fundamental follows the entry.
  {
    const Note n = *r.note(a);
    REQUIRE(r.store.dispatch(MoveNote{r.ref, keyOf(n), n.start + 1, ratio(5, 4)}).ok());  // same pitch: kept
  }
  CHECK(r.fundamental() == Rational(1, 12));
  {
    const Note n = *r.note(a);
    REQUIRE(r.store.dispatch(MoveNote{r.ref, keyOf(n), n.start, ratio(9, 8)}).ok());
  }
  CHECK(r.fundamental() == Rational(1, 8));
  // Shift+Up (× 2/1) also stays on the fundamental; a drag with steps (moveNotes) records like Up/Down.
  const NoteId c = r.add(ratio(1, 3), 2 * pq::kPpq);
  REQUIRE(r.fundamental() == Rational(1, 24));
  r.step(c, -1);
  CHECK(r.ratio(c) == Rational(7, 24));  // 8/24 → 7/24
  const NoteKey kc = r.key(c);
  commit(r.store, ops::moveNotes(*r.store.current(), r.ref, std::span<const NoteKey>(&kc, 1), pq::kPpq, 1));
  CHECK(r.ratio(c) == Rational(1, 3));  // back up on 1/24, moved in time
  CHECK(r.note(c)->start == 3 * pq::kPpq);
  CHECK(r.fundamental() == Rational(1, 24));  // 1/3 remembers 1/24
  // Ratios anchored to another note step on their own fundamental, not the clip's.
  const NoteId d = r.add(PitchId::fromRatio(Rational(5, 4), AnchorKind::Note, a.value), 4 * pq::kPpq);
  CHECK(r.fundamental() == Rational(1, 24));  // anchored ratios do not contribute
  r.step(d, 1);
  CHECK(r.note(d)->pitch.ratio() == Rational(3, 2));
  r.step(d, 1);
  CHECK(r.note(d)->pitch.ratio() == Rational(2));
}

TEST_CASE("virtual fundamental: the explicit lock (b + index) overrides the derivation until unlocked") {
  Roll r;
  const NoteId a = r.add(ratio(7, 4), 0);
  const NoteId deg = r.add(PitchId::fromDegree(3), pq::kPpq);
  REQUIRE(r.fundamental() == Rational(1, 4));
  CHECK_FALSE(ops::lockFundamental(*r.store.current(), r.ref, r.key(deg), 5).ok());  // no exact ratio
  CHECK_FALSE(ops::lockFundamental(*r.store.current(), r.ref, r.key(a), 0).ok());
  commit(r.store, ops::lockFundamental(*r.store.current(), r.ref, r.key(a), 5));  // "7/4 is the 5th harmonic"
  CHECK(r.fundamental() == Rational(7, 20));
  r.step(a, 1);
  CHECK(r.ratio(a) == Rational(42, 20));  // the 6th harmonic of 7/20 = 21/10
  CHECK(r.clip().fundamental.steps.empty());  // nothing to remember: the lock is the fundamental
  r.step(a, -2);
  CHECK(r.ratio(a) == Rational(7, 5));  // the 4th
  REQUIRE(r.store.undo());
  REQUIRE(r.store.undo());
  REQUIRE(r.store.undo());  // the lock
  CHECK(r.fundamental() == Rational(1, 4));
  REQUIRE(r.store.redo());
  CHECK(r.fundamental() == Rational(7, 20));
  commit(r.store, ops::unlockFundamental(*r.store.current(), r.ref));
  CHECK(r.fundamental() == Rational(1, 4));  // derived again from the 7/4
}

TEST_CASE("virtual fundamental: serialization round-trips (schema 2, the field written only when set)") {
  Roll r;
  const NoteId a = r.add(ratio(5, 4), 0);
  r.add(ratio(9, 8), pq::kPpq);
  const auto plain = serializeProject(*r.store.current());
  REQUIRE(plain.ok());
  CHECK(plain.value().find("\"fundamental\"") == std::string::npos);  // older files and goldens are unchanged
  r.step(a, 2);  // 10/8 → 12/8: 3/2 remembers 1/8
  REQUIRE(r.clip().fundamental.steps.size() == 1);
  commit(r.store, ops::lockFundamental(*r.store.current(), r.ref, r.key(a), 3));  // 3/2 = 3 · 1/2
  const auto text = serializeProject(*r.store.current());
  REQUIRE(text.ok());
  CHECK(text.value().find("\"fundamental\": {\"lock\": \"1/2\", \"steps\": [{\"note\": " +
                          std::to_string(a.value) + ", \"denominator\": 8, \"ratio\": \"3/2\"}]}") != std::string::npos);
  auto back = parseProject(text.value());
  REQUIRE_MESSAGE(back.ok(), (back.ok() ? std::string() : back.error().message));
  CHECK(*back->project == *r.store.current());
  const auto again = serializeProject(*back->project);
  REQUIRE(again.ok());
  CHECK(again.value() == text.value());
  // Malformed fields are refused with a path.
  std::string bad = text.value();
  bad.replace(bad.find("\"denominator\": 8"), 16, "\"denominator\": 0");
  CHECK_FALSE(parseProject(bad).ok());
}

TEST_CASE("virtual fundamental: denominators near int64 never overflow the lcm (M3 review L8)") {
  // std::lcm is undefined when the result does not fit; lcmCapped stops at the cap first.
  constexpr int64_t kCap = kMaxFundamentalDenominator;
  CHECK(lcmCapped(4, 6, kCap) == 12);
  CHECK(lcmCapped(kCap, 1, kCap) == kCap);
  CHECK(lcmCapped(kCap, 3, kCap) == -1);
  CHECK(lcmCapped((int64_t{1} << 43) + 1, (int64_t{1} << 44) + 3, kCap) == -1);
  CHECK(lcmCapped(INT64_MAX, INT64_MAX - 1, INT64_MAX) == -1);  // the product would wrap
  CHECK(lcmCapped(INT64_MAX, INT64_MAX, INT64_MAX) == INT64_MAX);
  // A clip whose reference-anchored ratios have denominators beyond 2^43: no fundamental (each note steps on its own).
  NoteClip c;
  c.notes = NoteSeq::fromNotes({}).value();
  std::vector<Note> notes;
  for (const int64_t d : {(int64_t{1} << 43) + 1, (int64_t{1} << 44) + 3, int64_t{3}}) {
    Note n;
    n.id = NoteId{static_cast<uint32_t>(notes.size() + 1)};
    n.pitch = PitchId::fromRatio(Rational{d + 1, d});
    notes.push_back(n);
  }
  c.notes = NoteSeq::fromNotes(std::move(notes)).value();
  CHECK_FALSE(virtualFundamental(c).has_value());
}
