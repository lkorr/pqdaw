// T7.13: notes-first JI in the document — JI groups (an anchor note, its declared harmonic index, the Ratio notes tied
// to it), written identities and warps (model, commands, resolution, the one effective-curve evaluation, edit
// operations, the file), and note curves. Plan: docs/plans/M7.md "Rework: notes-first JI" R1–R4.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "note_fix.h"
#include "pq/tuning/edo.h"

using namespace edit_test;
using note_fix::BendFix;
using note_fix::centsOf;
using pq::kPpq;
using pq::tuning::AnchorKind;
using pq::tuning::PitchId;
using pq::tuning::PitchKind;
using pq::tuning::Rational;

namespace {

// The fixture plus lookups by group. The clip "N" on Bass is 12-EDO (the project's default tuning).
struct JiFix : BendFix {
  const JiGroup& group(JiGroupId id) const {
    const JiGroup* g = findJiGroup(clip(), id);
    REQUIRE(g != nullptr);
    return *g;
  }
  GroupHarmonics harmonics(JiGroupId id) const {
    const std::optional<GroupHarmonics> h = groupHarmonics(clip(), group(id));
    REQUIRE(h.has_value());
    return *h;
  }
  int64_t numberOf(JiGroupId id, NoteId note) const {
    const GroupHarmonics h = harmonics(id);
    for (const GroupMember& m : h.members)
      if (m.note == note) return m.harmonic;
    FAIL("not a member");
    return 0;
  }
  const Note& noteOf(NoteId id) const {
    const Note* n = clip().notes.findById(id);
    REQUIRE(n != nullptr);
    return *n;
  }
  // The 12-EDO chord `degrees` at tick `start`, `length` long each; returns the notes in the given order.
  std::vector<Note> chord(std::vector<int32_t> degrees, Ticks start = 0, Ticks length = 4 * kPpq) {
    std::vector<Note> out;
    for (const int32_t d : degrees) out.push_back(add(start, PitchId::fromDegree(d), length));
    return out;
  }
  static std::vector<NoteKey> keysOf(const std::vector<Note>& notes) {
    std::vector<NoteKey> k;
    for (const Note& n : notes) k.push_back(keyOf(n));
    return k;
  }
  double degreeHz(int32_t degree) const {
    const pq::tuning::Tuning t = buildTuning(*effectiveTuningRef(doc(), *findTrack(doc(), bass), &clip()).def).value();
    return *pq::tuning::resolveHzDouble(PitchId::fromDegree(degree), t, {});
  }
};

std::vector<int64_t> terms(std::initializer_list<int64_t> t) { return t; }

}  // namespace

// ---- Is Harmonic N: groups and their numbering -----------------------------------------------------------------------

TEST_CASE("ji groups: Is Harmonic 5 on a 12-EDO note — the anchor's Hz does not move, members are exact, the numbering is derived") {
  JiFix f;
  IdSource& ids = f.store.ids();
  const Note e = f.add(0, PitchId::fromDegree(4), 4 * kPpq);  // a 12-EDO E
  const double hzE = f.hz(e.id);
  CHECK_FALSE(ops::makeJiGroup(f.doc(), ids, f.ref, keyOf(e), 0).ok());
  CHECK_FALSE(ops::makeJiGroup(f.doc(), ids, f.ref, keyOf(e), kMaxHarmonicIndex + 1).ok());
  CHECK_FALSE(ops::makeJiGroup(f.doc(), ids, f.ref, NoteKey{0, NoteId{999999}}, 5).ok());
  auto made = ops::makeJiGroup(f.doc(), ids, f.ref, keyOf(e), 5);
  commit(f.store, made);  // one undo step
  REQUIRE(made->jiGroups.size() == 1);
  const JiGroupId g = made->jiGroups[0];
  CHECK(f.group(g).anchor == e.id);
  CHECK(f.group(g).index == 5);
  CHECK(f.noteOf(e.id).pitch == PitchId::fromDegree(4));  // its identity is kept: frozen
  CHECK(f.hz(e.id) == hzE);                               // bit for bit
  CHECK(f.harmonics(g).anchor == 5);
  CHECK(f.harmonics(g).fundamental == Rational(1, 5));  // F = E / 5: the guides are k · E / 5
  CHECK(f.harmonics(g).proportion == terms({5}));
  CHECK(ops::makeJiGroup(f.doc(), ids, f.ref, keyOf(e), 5)->empty());  // again: nothing to do

  // A member at 6/5 (harmonic 6): exact.
  auto six = ops::addGroupNote(f.doc(), ids, f.ref, g, ops::GroupNotePitch{{}, 6}, kPpq, kPpq);
  commit(f.store, six);
  const NoteId n6 = six->notes.at(0).id;
  CHECK(f.noteOf(n6).pitch == PitchId::fromRatio(Rational(6, 5), AnchorKind::Note, e.id.value));
  CHECK(f.hz(n6) == hzE * (6.0 / 5.0));
  CHECK(f.numberOf(g, n6) == 6);
  CHECK(f.harmonics(g).proportion == terms({5, 6}));

  // A member 3/2 above the anchor: the fundamental drops an octave — the anchor reads 10, the fifth 15, the 6/5 12.
  auto fifth = ops::addGroupNote(f.doc(), ids, f.ref, g, ops::GroupNotePitch{Rational(3, 2), 0}, 2 * kPpq, kPpq);
  commit(f.store, fifth);
  const NoteId n15 = fifth->notes.at(0).id;
  CHECK(f.hz(n15) == hzE * 1.5);
  CHECK(f.harmonics(g).anchor == 10);
  CHECK(f.harmonics(g).fundamental == Rational(1, 10));
  CHECK(f.numberOf(g, n15) == 15);
  CHECK(f.numberOf(g, n6) == 12);
  CHECK(f.harmonics(g).proportion == terms({10, 12, 15}));
  CHECK(f.group(g).index == 5);  // what was declared is kept
  CHECK(f.hz(e.id) == hzE);
  // "Harmonic 15" of the group as it is numbered now is that same 3/2.
  CHECK(ops::addGroupNote(f.doc(), ids, f.ref, g, ops::GroupNotePitch{{}, 15}, 3 * kPpq, kPpq)->batch().commands.size() == 1);

  // Deleted: 5 again.
  commit(f.store, ops::deleteNotes(f.doc(), f.ref, std::vector<NoteKey>{fifth->notes[0]}));
  CHECK(f.harmonics(g).anchor == 5);
  CHECK(f.numberOf(g, n6) == 6);
  // A member too fine for any numbering is left unnumbered; the others keep theirs.
  auto fine = ops::addGroupNote(f.doc(), ids, f.ref, g, ops::GroupNotePitch{Rational(3, (int64_t{1} << 19) + 3), 0}, 3 * kPpq, kPpq);
  commit(f.store, fine);
  CHECK(f.harmonics(g).anchor == 5);
  CHECK(f.numberOf(g, fine->notes[0].id) == 0);
  CHECK(f.numberOf(g, n6) == 6);
  f.roundTrips();

  // Changing the declared index of the same anchor keeps the group.
  auto again = ops::makeJiGroup(f.doc(), ids, f.ref, keyOf(e), 10);
  commit(f.store, again);
  CHECK(again->jiGroups == std::vector<JiGroupId>{g});
  CHECK(jiGroupsOf(f.clip()).size() == 1);
  CHECK(f.numberOf(g, n6) == 12);
  CHECK(f.hz(e.id) == hzE);
}

TEST_CASE("ji groups: two groups in one clip are independent") {
  JiFix f;
  IdSource& ids = f.store.ids();
  const Note a = f.add(0, PitchId::fromDegree(0), 2 * kPpq);
  const Note b = f.add(4 * kPpq, PitchId::fromCents(431.0), 2 * kPpq);
  auto ga = ops::makeJiGroup(f.doc(), ids, f.ref, keyOf(a), 4);
  commit(f.store, ga);
  auto gb = ops::makeJiGroup(f.doc(), ids, f.ref, keyOf(b), 7);
  commit(f.store, gb);
  const JiGroupId A = ga->jiGroups[0], B = gb->jiGroups[0];
  CHECK(A != B);
  auto ma = ops::addGroupNote(f.doc(), ids, f.ref, A, ops::GroupNotePitch{{}, 5}, kPpq, kPpq);
  commit(f.store, ma);
  auto mb = ops::addGroupNote(f.doc(), ids, f.ref, B, ops::GroupNotePitch{Rational(3, 2), 0}, 5 * kPpq, kPpq);
  commit(f.store, mb);
  CHECK(f.harmonics(A).proportion == terms({4, 5}));
  CHECK(f.harmonics(B).proportion == terms({14, 21}));  // 7 and a 3/2 above it
  CHECK(f.hz(ma->notes[0].id) == f.hz(a.id) * 1.25);
  CHECK(f.hz(mb->notes[0].id) == f.hz(b.id) * 1.5);
  CHECK(jiGroupOfNote(f.clip(), f.noteOf(ma->notes[0].id)) == &f.group(A));
  CHECK(jiGroupOfNote(f.clip(), f.noteOf(b.id)) == &f.group(B));
  // Walking one leaves the other alone.
  const GroupHarmonics before = f.harmonics(B);
  commit(f.store, ops::walkJiGroup(f.doc(), f.ref, A, 1));
  CHECK(f.harmonics(A).proportion == terms({5, 6}));
  CHECK(f.harmonics(B) == before);
  // A note anchors one group only (the command refuses a second group on it).
  JiGroup dup = f.group(A);
  dup.id = ids.jiGroup();
  std::vector<JiGroup> list(jiGroupsOf(f.clip()).begin(), jiGroupsOf(f.clip()).end());
  list.push_back(dup);
  CHECK_FALSE(f.store.dispatch(SetJiGroups{f.ref, list}).ok());
  // Removing one: its members keep sounding (Cents under a tempered anchor), the other group is untouched.
  const double hm = f.hz(ma->notes[0].id);
  commit(f.store, ops::removeJiGroup(f.doc(), f.ref, A));
  CHECK(findJiGroup(f.clip(), A) == nullptr);
  CHECK(f.noteOf(ma->notes[0].id).pitch.kind() == PitchKind::Cents);
  CHECK(f.hz(ma->notes[0].id) == doctest::Approx(hm).epsilon(1e-13));
  CHECK(f.harmonics(B) == before);
  f.roundTrips();
}

TEST_CASE("ji groups: removing a group on an exact anchor leaves exact reference ratios") {
  JiFix f;
  IdSource& ids = f.store.ids();
  const Note a = f.add(0, PitchId::fromRatio(Rational(9, 8)));
  auto made = ops::makeJiGroup(f.doc(), ids, f.ref, keyOf(a), 9);
  commit(f.store, made);
  auto m = ops::addGroupNote(f.doc(), ids, f.ref, made->jiGroups[0], ops::GroupNotePitch{{}, 11}, kPpq, kPpq);
  commit(f.store, m);
  commit(f.store, ops::removeJiGroup(f.doc(), f.ref, made->jiGroups[0]));
  CHECK(f.noteOf(m->notes[0].id).pitch == PitchId::fromRatio(Rational(11, 8)));  // 9/8 · 11/9
  CHECK(f.clip().groups == nullptr);
  CHECK_FALSE(ops::removeJiGroup(f.doc(), f.ref, made->jiGroups[0]).ok());
}

// ---- Tune to JI ------------------------------------------------------------------------------------------------------

TEST_CASE("tune to JI: a 12-EDO major triad becomes 4:5:6 — the lowest note's Hz unchanged, the members exact; Back to Tuning restores the written identities") {
  JiFix f;
  IdSource& ids = f.store.ids();
  const std::vector<Note> triad = f.chord({7, 0, 4});  // given out of order: the lowest pitch is the anchor
  const Note& g = triad[0];
  const Note& c = triad[1];
  const Note& e = triad[2];
  const double hzC = f.hz(c.id), hzE = f.hz(e.id), hzG = f.hz(g.id);
  const Snapshot written = f.store.current();
  auto tuned = ops::tuneToJi(f.doc(), ids, f.ref, JiFix::keysOf(triad));
  commit(f.store, tuned);  // one undo step
  REQUIRE(tuned->jiGroups.size() == 1);
  const JiGroupId id = tuned->jiGroups[0];
  CHECK(f.group(id).anchor == c.id);
  CHECK(f.group(id).index == 4);
  CHECK(f.group(id).warps.empty());
  CHECK(f.harmonics(id).proportion == terms({4, 5, 6}));
  CHECK(f.noteOf(c.id).pitch == PitchId::fromDegree(0));  // the anchor keeps its identity
  CHECK(f.hz(c.id) == hzC);                               // and its Hz, bit for bit
  CHECK(f.noteOf(e.id).pitch == PitchId::fromRatio(Rational(5, 4), AnchorKind::Note, c.id.value));
  CHECK(f.noteOf(g.id).pitch == PitchId::fromRatio(Rational(3, 2), AnchorKind::Note, c.id.value));
  CHECK(f.hz(e.id) == hzC * 1.25);  // exact
  CHECK(f.hz(g.id) == hzC * 1.5);
  CHECK(centsOf(hzE, f.hz(e.id)) == doctest::Approx(-13.686).epsilon(1e-4));
  // Each tuned member remembers what it was written as.
  REQUIRE(notesWrittenOf(f.clip()).size() == 2);
  CHECK(findNoteWritten(f.clip(), e.id)->written == PitchId::fromDegree(4));
  CHECK(findNoteWritten(f.clip(), g.id)->written == PitchId::fromDegree(7));
  CHECK(findNoteWritten(f.clip(), c.id) == nullptr);
  f.roundTrips();
  // Tuning again changes nothing (the notes are read as they were written).
  CHECK(ops::tuneToJi(f.doc(), ids, f.ref, JiFix::keysOf(triad))->empty());

  // Back to Tuning (T7.16a, M7 review M1: a state the group keeps): every member sounds as written, bit for bit; the
  // reading (ratios, index) and the written identities stay; one undo step.
  const Snapshot wasTuned = commit(f.store, ops::untuneJiGroup(f.doc(), f.ref, id));
  CHECK_FALSE(f.group(id).tuned);
  CHECK(f.group(id).index == 4);
  CHECK(f.noteOf(e.id).pitch == PitchId::fromRatio(Rational(5, 4), AnchorKind::Note, c.id.value));
  CHECK(f.hz(c.id) == hzC);
  CHECK(f.hz(e.id) == hzE);
  CHECK(f.hz(g.id) == hzG);
  CHECK(notesWrittenOf(f.clip()).size() == 2);
  f.roundTrips();  // "tuned": false is saved
  // The same toggle again: the same reading, exactly; and Remove JI Group on a group Back to Tuning keeps what sounds.
  const Snapshot untuned = commit(f.store, ops::setJiGroupsTuned(f.doc(), f.ref, std::vector<JiGroupId>{id}, true));
  CHECK(f.group(id).tuned);
  CHECK(f.hz(e.id) == hzC * 1.25);
  CHECK(f.hz(g.id) == hzC * 1.5);
  REQUIRE(f.store.undo());
  CHECK(f.store.current() == untuned);
  commit(f.store, ops::removeJiGroup(f.doc(), f.ref, id));
  CHECK(f.noteOf(e.id).pitch == PitchId::fromDegree(4));
  CHECK(f.noteOf(g.id).pitch == PitchId::fromDegree(7));
  CHECK(f.clip().groups == nullptr);
  CHECK(f.clip().written == nullptr);
  CHECK(f.clip() == *written->tracks.back()->clips[0].clip);  // the clip as it was written
  REQUIRE(f.store.undo());
  REQUIRE(f.store.undo());
  CHECK(f.store.current() == wasTuned);
}

TEST_CASE("tune to JI: the candidates — a dominant seventh reads 4:5:6:7 before 20:25:30:36; a minor triad 10:12:15, then 16:19:24 and 6:7:9") {
  JiFix f;
  IdSource& ids = f.store.ids();
  auto index = [](const std::vector<ops::JiCandidate>& list, std::vector<int64_t> t) {
    for (size_t i = 0; i < list.size(); ++i)
      if (list[i].terms == t) return static_cast<int>(i);
    return -1;
  };
  SUBCASE("C E G B-flat") {
    const std::vector<Note> dom = f.chord({0, 4, 7, 10});
    auto cands = ops::tuneToJiCandidates(f.doc(), f.ref, JiFix::keysOf(dom));
    REQUIRE(cands.ok());
    REQUIRE(!cands->empty());
    CHECK(cands->size() <= ops::kMaxJiCandidates);
    for (const ops::JiCandidate& cd : *cands) {
      std::string line;
      for (const int64_t t : cd.terms) line += (line.empty() ? "" : ":") + std::to_string(t);
      MESSAGE(line << "  error " << cd.error << "  score " << cd.score);
    }
    CHECK(cands->front().terms == terms({4, 5, 6, 7}));
    CHECK(cands->front().worst == doctest::Approx(31.174).epsilon(1e-4));  // the 7/4 against 1000 cents
    const int septimal = index(*cands, {4, 5, 6, 7}), classical = index(*cands, {20, 25, 30, 36});
    REQUIRE(classical > 0);
    CHECK(septimal < classical);
    CHECK((*cands)[static_cast<size_t>(classical)].error < cands->front().error);  // nearer, but not simpler
    for (size_t i = 1; i < cands->size(); ++i) CHECK((*cands)[i - 1].score <= (*cands)[i].score);
    for (const ops::JiCandidate& cd : *cands) CHECK(cd.worst <= ops::kTuneToJiToleranceCents);
    // The default is the best candidate; the chooser picks another.
    auto best = ops::tuneToJi(f.doc(), ids, f.ref, JiFix::keysOf(dom));
    commit(f.store, best);
    CHECK(groupHarmonics(f.clip(), f.group(best->jiGroups[0]))->proportion == terms({4, 5, 6, 7}));
    CHECK(f.hz(dom[3].id) == f.hz(dom[0].id) * 1.75);
    // The list is the same once tuned (members are read as written), and choosing from it re-tunes the SAME group.
    auto after = ops::tuneToJiCandidates(f.doc(), f.ref, JiFix::keysOf(dom));
    REQUIRE(after.ok());
    CHECK(*after == *cands);
    ops::TuneToJiOptions pick;
    pick.proportion = {20, 25, 30, 36};
    auto other = ops::tuneToJi(f.doc(), ids, f.ref, JiFix::keysOf(dom), pick);
    commit(f.store, other);
    CHECK(other->jiGroups == best->jiGroups);
    CHECK(jiGroupsOf(f.clip()).size() == 1);
    CHECK(f.group(other->jiGroups[0]).index == 20);
    CHECK(f.noteOf(dom[3].id).pitch == PitchId::fromRatio(Rational(9, 5), AnchorKind::Note, dom[0].id.value));
    CHECK(findNoteWritten(f.clip(), dom[3].id)->written == PitchId::fromDegree(10));  // still what was written
    // A proportion of another size, or one that does not ascend, is refused.
    pick.proportion = {4, 5, 6};
    CHECK_FALSE(ops::tuneToJi(f.doc(), ids, f.ref, JiFix::keysOf(dom), pick).ok());
    pick.proportion = {4, 5, 5, 7};
    CHECK_FALSE(ops::tuneToJi(f.doc(), ids, f.ref, JiFix::keysOf(dom), pick).ok());
  }
  SUBCASE("a major triad") {
    auto cands = ops::tuneToJiCandidates(f.doc(), f.ref, JiFix::keysOf(f.chord({0, 4, 7})));
    REQUIRE(cands.ok());
    REQUIRE(!cands->empty());
    CHECK(cands->front().terms == terms({4, 5, 6}));
  }
  SUBCASE("a minor triad") {
    auto cands = ops::tuneToJiCandidates(f.doc(), f.ref, JiFix::keysOf(f.chord({0, 3, 7})));
    REQUIRE(cands.ok());
    REQUIRE(cands->size() >= 3);
    CHECK(cands->front().terms == terms({10, 12, 15}));
    const int septimal = index(*cands, {6, 7, 9});
    CHECK(septimal > 0);
    CHECK(index(*cands, {16, 19, 24}) > 0);
  }
  SUBCASE("unisons share a term; a single note is a group of its own") {
    std::vector<Note> notes = f.chord({0, 7});
    notes.push_back(f.add(4 * kPpq, PitchId::fromDegree(7)));  // the fifth again, later
    notes.push_back(f.add(4 * kPpq, PitchId::fromDegree(0)));  // and the root
    auto tuned = ops::tuneToJi(f.doc(), ids, f.ref, JiFix::keysOf(notes));
    commit(f.store, tuned);
    const JiGroupId id = tuned->jiGroups[0];
    CHECK(f.group(id).anchor == notes[0].id);  // the lowest, then the earliest
    CHECK(f.harmonics(id).proportion == terms({2, 3}));
    CHECK(f.noteOf(notes[2].id).pitch == f.noteOf(notes[1].id).pitch);
    CHECK(f.noteOf(notes[3].id).pitch == PitchId::fromRatio(Rational(1), AnchorKind::Note, notes[0].id.value));
    const Note lone = f.add(8 * kPpq, PitchId::fromDegree(5));
    auto one = ops::tuneToJi(f.doc(), ids, f.ref, std::vector<NoteKey>{keyOf(lone)});
    commit(f.store, one);
    CHECK(f.group(one->jiGroups[0]).index == 1);
    CHECK_FALSE(ops::tuneToJi(f.doc(), ids, f.ref, std::vector<NoteKey>{}).ok());
  }
  SUBCASE("a note that already is an exact ratio to the lowest keeps it") {
    const Note root = f.add(0, PitchId::fromRatio(Rational(1)));
    const Note seventh = f.add(0, PitchId::fromRatio(Rational(7, 4)));   // exact: 7/4 in every reading
    const Note third = f.add(0, PitchId::fromDegree(4));                 // tempered
    const std::vector<NoteKey> keys = {keyOf(root), keyOf(seventh), keyOf(third)};
    const double thirdHz = f.hz(third.id);
    auto cands = ops::tuneToJiCandidates(f.doc(), f.ref, keys);
    REQUIRE(cands.ok());
    REQUIRE(!cands->empty());
    for (const ops::JiCandidate& cd : *cands) CHECK(cd.terms[2] * 4 == cd.terms[0] * 7);
    CHECK(cands->front().terms == terms({4, 5, 7}));
    auto tuned = ops::tuneToJi(f.doc(), ids, f.ref, keys);
    commit(f.store, tuned);
    CHECK(f.noteOf(seventh.id).pitch == PitchId::fromRatio(Rational(7, 4), AnchorKind::Note, root.id.value));
    // T7.16a: the exact one remembers it too (Back to Tuning is a state: it plays what is written).
    CHECK(findNoteWritten(f.clip(), seventh.id)->written == PitchId::fromRatio(Rational(7, 4)));
    CHECK(findNoteWritten(f.clip(), third.id)->written == PitchId::fromDegree(4));
    const double seventhHz = f.hz(seventh.id);
    // Back to Tuning: the tempered note sounds as its degree again, the exact one at its exact reference ratio.
    commit(f.store, ops::untuneJiGroup(f.doc(), f.ref, tuned->jiGroups[0]));
    CHECK(f.hz(third.id) == thirdHz);
    CHECK(f.hz(seventh.id) == seventhHz);
    commit(f.store, ops::removeJiGroup(f.doc(), f.ref, tuned->jiGroups[0]));
    CHECK(f.noteOf(third.id).pitch == PitchId::fromDegree(4));
    CHECK(f.noteOf(seventh.id).pitch == PitchId::fromRatio(Rational(7, 4)));
  }
}

TEST_CASE("tune to JI: a subset whose lowest note was a member — it goes back to what it was written as and anchors the new group") {
  JiFix f;
  IdSource& ids = f.store.ids();
  const std::vector<Note> triad = f.chord({0, 4, 7});
  auto first = ops::tuneToJi(f.doc(), ids, f.ref, JiFix::keysOf(triad));
  commit(f.store, first);
  // E and G only: E (written degree 4) is the anchor of a second group, G a 6/5 above it.
  auto second = ops::tuneToJi(f.doc(), ids, f.ref, std::vector<NoteKey>{keyOf(triad[1]), keyOf(triad[2])});
  commit(f.store, second);
  CHECK(second->jiGroups[0] != first->jiGroups[0]);
  CHECK(f.noteOf(triad[1].id).pitch == PitchId::fromDegree(4));
  CHECK(findNoteWritten(f.clip(), triad[1].id) == nullptr);  // an anchor does not warp
  CHECK(f.noteOf(triad[2].id).pitch == PitchId::fromRatio(Rational(6, 5), AnchorKind::Note, triad[1].id.value));
  CHECK(findNoteWritten(f.clip(), triad[2].id)->written == PitchId::fromDegree(7));
  CHECK(f.harmonics(first->jiGroups[0]).members.empty());  // the first group is a bare declared harmonic now
  f.roundTrips();
}

// ---- Warps -----------------------------------------------------------------------------------------------------------

TEST_CASE("warps: a triad written in 12-EDO sounds tempered before a toJi warp, glides, holds exact 4:5:6 after it, and goes back with a second warp") {
  JiFix f;
  IdSource& ids = f.store.ids();
  const Ticks L = 8 * kPpq;
  const std::vector<Note> triad = f.chord({0, 4, 7}, 0, L);
  const double hzC = f.hz(triad[0].id), hzE = f.hz(triad[1].id), hzG = f.hz(triad[2].id);
  auto tuned = ops::tuneToJi(f.doc(), ids, f.ref, JiFix::keysOf(triad));
  commit(f.store, tuned);
  const JiGroupId id = tuned->jiGroups[0];
  CHECK(f.pitches(f.clip()).warped.empty());  // no warps: JI throughout
  // To JI at beat 2 over one beat (straight in cents), back at beat 5 (a jump).
  const std::vector<JiWarp> warps = {JiWarp{2 * kPpq, kPpq, GlideShape{GlideShape::LogLinear}, true}, JiWarp{5 * kPpq, 0, GlideShape{}, false}};
  commit(f.store, ops::setJiWarps(f.doc(), f.ref, id, warps));  // one undo step
  CHECK(f.group(id).warps == warps);
  f.roundTrips();

  // The long notes start before the warp: they resolve to their WRITTEN pitch, bit for bit.
  CHECK(f.hz(triad[0].id) == hzC);
  CHECK(f.hz(triad[1].id) == hzE);
  CHECK(f.hz(triad[2].id) == hzG);
  const ClipPitches cp = f.pitches(f.clip());
  REQUIRE(cp.warped.size() == 2);  // E and G; the anchor does not move
  const NotePitchIndex index(cp);
  const NoteWarpState* we = index.warped(triad[1].id);
  REQUIRE(we != nullptr);
  CHECK(we->group == id);
  CHECK(we->offset == 0);
  CHECK(we->writtenHz == hzE);
  CHECK(we->jiHz == hzC * 1.25);
  CHECK(we->cents == doctest::Approx(13.686).epsilon(1e-4));
  CHECK(index.warped(triad[0].id) == nullptr);

  // The ONE curve: 0 until the warp, a straight glide to −13.7 cents, held, and back up at the jump.
  const NoteWarp w = index.warp(f.clip(), triad[1].id);
  REQUIRE(w.any());
  auto at = [&](double tick) { return noteCurveCents(nullptr, L, {}, w, tick); };
  CHECK(at(0.0) == 0.0);
  CHECK(at(2.0 * kPpq) == 0.0);
  CHECK(at(2.5 * kPpq) == doctest::Approx(-0.5 * we->cents).epsilon(1e-12));
  CHECK(at(3.0 * kPpq) == -we->cents);
  CHECK(at(4.9 * kPpq) == -we->cents);
  CHECK(at(5.0 * kPpq) == 0.0);  // the jump: the value from that tick on
  CHECK(at(8.0 * kPpq) == 0.0);
  const std::vector<Breakpoint> curve = f.effective(f.clip(), f.noteOf(triad[1].id));
  REQUIRE(curve.size() == 6);
  const float d = static_cast<float>(-we->cents);
  CHECK(curve[0] == Breakpoint{0, 0.0f, 0.0f, {}});
  CHECK(curve[1] == Breakpoint{2 * kPpq, 0.0f, 0.0f, {}});
  CHECK(curve[2] == Breakpoint{3 * kPpq, d, 0.0f, {}});
  CHECK(curve[3] == Breakpoint{5 * kPpq, d, 0.0f, {}});  // the jump's two points
  CHECK(curve[4] == Breakpoint{5 * kPpq, 0.0f, 0.0f, {}});
  CHECK(curve.back() == Breakpoint{L, 0.0f, 0.0f, {}});
  CHECK(f.effective(f.clip(), f.noteOf(triad[0].id)).empty());  // the anchor: no curve
  // What sounds while the JI state holds: written · 2^(curve / 1200) is 5/4 over the root to float precision.
  CHECK(std::abs(centsOf(hzC * 1.25, hzE * std::exp2(static_cast<double>(d) / 1200.0))) < 1e-5);

  // Notes that START in another state start on that state's pitch, exactly, and curve from there.
  auto member = [&](Ticks start, Ticks length) {
    auto e = ops::addGroupNote(f.doc(), ids, f.ref, id, ops::GroupNotePitch{{}, 5}, start, length);
    commit(f.store, e);
    const NoteId n = e->notes.at(0).id;
    // It remembers the same written degree as the E (as a tuned note drawn there would).
    std::vector<NoteWritten> list(notesWrittenOf(f.clip()).begin(), notesWrittenOf(f.clip()).end());
    list.push_back(NoteWritten{n, PitchId::fromDegree(4)});
    REQUIRE(f.store.dispatch(SetNotesWritten{f.ref, list}).ok());
    return n;
  };
  const NoteId inJi = member(3 * kPpq + 7, kPpq);          // after the glide: JI
  const NoteId midGlide = member(2 * kPpq + kPpq / 4, 4 * kPpq);  // a quarter into the glide
  const NoteId afterBack = member(6 * kPpq, kPpq);         // after the second warp: written again
  CHECK(f.hz(inJi) == hzC * 1.25);  // exact
  CHECK(f.hz(afterBack) == hzE);    // the written pitch, bit for bit
  CHECK(centsOf(hzC * 1.25, f.hz(midGlide)) == doctest::Approx(0.75 * we->cents).epsilon(1e-9));
  CHECK(f.effective(f.clip(), f.noteOf(inJi)).empty());  // it holds JI for its whole length
  const std::vector<Breakpoint> mid = f.effective(f.clip(), f.noteOf(midGlide));
  REQUIRE(mid.size() == 5);
  CHECK(mid[0] == Breakpoint{0, 0.0f, 0.0f, {}});
  CHECK(mid[1].tick == kPpq - kPpq / 4);  // the rest of the glide
  CHECK(mid[1].value == doctest::Approx(-0.75 * we->cents).epsilon(1e-5));
  CHECK(mid[2].tick == 3 * kPpq - kPpq / 4);  // … held until the jump back to the written pitch
  CHECK(mid[3].value == doctest::Approx(0.25 * we->cents).epsilon(1e-5));
  // A member without a written identity is JI throughout; so is the group once the warps are gone.
  auto plain = ops::addGroupNote(f.doc(), ids, f.ref, id, ops::GroupNotePitch{{}, 7}, 0, L);
  commit(f.store, plain);
  CHECK(f.hz(plain->notes[0].id) == hzC * 1.75);
  CHECK(f.effective(f.clip(), f.noteOf(plain->notes[0].id)).empty());
  f.roundTrips();
  commit(f.store, ops::setJiWarps(f.doc(), f.ref, id, {}));
  CHECK(f.hz(triad[1].id) == hzC * 1.25);
  CHECK(f.pitches(f.clip()).warped.empty());
}

TEST_CASE("warps: group ticks count from the anchor's start (T7.16a, M7 review M7); shapes; a note anchored to a warped member; validation") {
  JiFix f;
  IdSource& ids = f.store.ids();
  const std::vector<Note> triad = f.chord({0, 4, 7}, 3 * kPpq, 4 * kPpq);
  auto tuned = ops::tuneToJi(f.doc(), ids, f.ref, JiFix::keysOf(triad));
  commit(f.store, tuned);
  const JiGroupId id = tuned->jiGroups[0];
  const std::optional<GroupSpan> span = groupSpan(f.clip(), f.group(id));
  REQUIRE(span.has_value());
  CHECK(span->start == 3 * kPpq);
  CHECK(span->end == 7 * kPpq);
  // An S-curve glide over [1, 3) beats of the group = [4, 6) beats of the clip.
  commit(f.store, ops::setJiWarps(f.doc(), f.ref, id, {JiWarp{kPpq, 2 * kPpq, GlideShape{}, true}}));
  const ClipPitches cp = f.pitches(f.clip());
  const NotePitchIndex index(cp);
  const NoteWarp w = index.warp(f.clip(), triad[1].id);
  const double D = index.warped(triad[1].id)->cents;
  CHECK(noteCurveCents(nullptr, 4 * kPpq, {}, w, 1.0 * kPpq) == 0.0);
  CHECK(noteCurveCents(nullptr, 4 * kPpq, {}, w, 2.0 * kPpq) == doctest::Approx(-0.5 * D).epsilon(1e-12));   // s(1/2) = 1/2
  CHECK(noteCurveCents(nullptr, 4 * kPpq, {}, w, 1.5 * kPpq) == doctest::Approx(-0.15625 * D).epsilon(1e-12));  // s(1/4)
  CHECK(noteCurveCents(nullptr, 4 * kPpq, {}, w, 3.0 * kPpq) == -D);
  // The flattened curve stays within its tolerance of the exact one.
  const std::vector<Breakpoint> flat = f.effective(f.clip(), f.noteOf(triad[1].id), 0.05);
  REQUIRE(flat.size() > 4);
  for (Ticks t = 0; t <= 4 * kPpq; t += 37)
    CHECK(std::abs(pq::evalAutomation(std::span<const Breakpoint>(flat), static_cast<double>(t), 0.0) - noteCurveCents(nullptr, 4 * kPpq, {}, w, static_cast<double>(t))) < 0.06);
  // A note anchored to the warped E takes E's pitch where it starts itself (Q293): tempered before, pure after.
  const Note early = f.add(3 * kPpq + 10, PitchId::fromRatio(Rational(3, 2), AnchorKind::Note, triad[1].id.value));
  const Note late = f.add(6 * kPpq + 10, PitchId::fromRatio(Rational(3, 2), AnchorKind::Note, triad[1].id.value));
  CHECK(f.hz(early.id) == f.degreeHz(4) * 1.5);
  CHECK(std::abs(centsOf(f.hz(triad[0].id) * 1.25 * 1.5, f.hz(late.id))) < 1e-9);
  // A bend and a free curve add to the warp (one evaluation).
  commit(f.store, ops::setNoteCurve(f.doc(), f.ref, keyOf(f.noteOf(triad[1].id)), {{0, 25.0f, 0.0f, {}}}));
  const NoteCurves* nc = findNoteCurves(f.clip(), triad[1].id);
  REQUIRE(nc != nullptr);
  CHECK(noteCurveCents(nc, 4 * kPpq, {}, w, 3.0 * kPpq) == doctest::Approx(25.0 - D).epsilon(1e-12));
  // T7.16a (M7 review M7): group ticks count from the ANCHOR's start. The whole chord moved in time takes its warps
  // along (nothing is stored differently) …
  commit(f.store, ops::moveNotes(f.doc(), f.ref, JiFix::keysOf({f.noteOf(triad[0].id), f.noteOf(triad[1].id), f.noteOf(triad[2].id)}), kPpq));
  CHECK(f.group(id).warps[0].at == kPpq);
  CHECK(groupSpan(f.clip(), f.group(id))->origin == 4 * kPpq);
  REQUIRE(f.store.undo());
  // … bug-ji-input (replacing Q310's last rule): the anchor moved ALONE in time (its members stay) moves only that
  // note: the warps stay where they are in the clip, shifted to count from its new start.
  commit(f.store, ops::moveNotes(f.doc(), f.ref, std::vector<NoteKey>{keyOf(f.noteOf(triad[0].id))}, -kPpq));
  CHECK(groupSpan(f.clip(), f.group(id))->start == 2 * kPpq);
  CHECK(groupSpan(f.clip(), f.group(id))->origin == 2 * kPpq);
  CHECK(f.group(id).anchor == triad[0].id);
  CHECK(f.group(id).warps[0].at == 2 * kPpq);  // still beat 4 of the clip
  CHECK(NotePitchIndex(f.pitches(f.clip())).warped(triad[1].id)->offset == kPpq);
  // … any other note leaving or moving earlier does not: a member moved before the anchor starts at a negative tick.
  commit(f.store, ops::moveNotes(f.doc(), f.ref, std::vector<NoteKey>{keyOf(f.noteOf(triad[2].id))}, -3 * kPpq));
  CHECK(groupSpan(f.clip(), f.group(id))->start == 0);
  CHECK(groupSpan(f.clip(), f.group(id))->origin == 2 * kPpq);
  CHECK(NotePitchIndex(f.pitches(f.clip())).warped(triad[1].id)->offset == kPpq);
  CHECK(NotePitchIndex(f.pitches(f.clip())).warped(triad[2].id)->offset == -2 * kPpq);
  commit(f.store, ops::deleteNotes(f.doc(), f.ref, std::vector<NoteKey>{keyOf(f.noteOf(triad[2].id))}));
  CHECK(f.group(id).warps[0].at == 2 * kPpq);
  CHECK(NotePitchIndex(f.pitches(f.clip())).warped(triad[1].id)->offset == kPpq);
  REQUIRE(f.store.undo());
  REQUIRE(f.store.undo());

  // Validation: ascending, alternating, no glide into the next warp, ranges, shapes.
  auto refused = [&](std::vector<JiWarp> warps) { CHECK_FALSE(ops::setJiWarps(f.doc(), f.ref, id, std::move(warps)).ok()); };
  refused({JiWarp{0, 0, GlideShape{}, true}, JiWarp{0, 0, GlideShape{}, false}});        // not ascending
  refused({JiWarp{0, 0, GlideShape{}, true}, JiWarp{kPpq, 0, GlideShape{}, true}});      // not alternating
  refused({JiWarp{0, kPpq, GlideShape{}, true}, JiWarp{kPpq - 1, 0, GlideShape{}, false}});  // inside the glide before it
  refused({JiWarp{-kMaxTicks - 1, 0, GlideShape{}, true}});  // (negative group ticks are fine: before the anchor)
  refused({JiWarp{0, -1, GlideShape{}, true}});
  CHECK(ops::setJiWarps(f.doc(), f.ref, id, {JiWarp{-kPpq, 0, GlideShape{}, true}}).ok());
  refused({JiWarp{0, 0, GlideShape{GlideShape::Bezier, 1.5f, 0.0f, 0.5f, 1.0f}, true}});
  CHECK(ops::setJiWarps(f.doc(), f.ref, id, {JiWarp{0, kPpq, GlideShape{}, false}, JiWarp{kPpq, 0, GlideShape{}, true}}).ok());
  CHECK_FALSE(ops::setJiWarps(f.doc(), f.ref, JiGroupId{999999}, {}).ok());
}

TEST_CASE("warps: the state function — exactly 0 / 1 outside the glides, before / at a jump") {
  const std::vector<JiWarp> w = {JiWarp{100, 50, GlideShape{GlideShape::LogLinear}, true}, JiWarp{300, 0, GlideShape{}, false},
                                 JiWarp{400, 100, GlideShape{GlideShape::Bezier, 0.3f, -0.4f, 0.6f, 1.6f}, true}};
  CHECK(jiWarpWritten({}, 5.0) == 0.0);  // no warps: JI
  CHECK(jiWarpWritten(w, -10.0) == 1.0);  // before a toJi warp: written
  CHECK(jiWarpWritten(w, 100.0) == 1.0);
  CHECK(jiWarpWritten(w, 125.0) == 0.5);
  CHECK(jiWarpWritten(w, 150.0) == 0.0);
  CHECK(jiWarpWritten(w, 299.0) == 0.0);
  CHECK(jiWarpWritten(w, 300.0) == 1.0);        // the jump: from that tick on
  CHECK(jiWarpWritten(w, 300.0, true) == 0.0);  // what it arrives with
  CHECK(jiWarpWritten(w, 400.0) == 1.0);
  CHECK(jiWarpWritten(w, 500.0) == 0.0);  // a bezier's end is exact too
  CHECK(jiWarpWritten(w, 1e9) == 0.0);
  const std::vector<JiWarp> back = {JiWarp{10, 0, GlideShape{}, false}};
  CHECK(jiWarpWritten(back, 0.0) == 0.0);  // before a !toJi warp: JI
  CHECK(jiWarpWritten(back, 10.0) == 1.0);
}

// ---- Walk, proportions, steps -----------------------------------------------------------------------------------------

TEST_CASE("walk: 22:26:30:33 one down is 21:25:29:32 with the anchor's Hz exactly where it was; refused at the first harmonic") {
  JiFix f;
  IdSource& ids = f.store.ids();
  const int64_t t[] = {22, 26, 30, 33};
  auto made = ops::proportionNotes(f.doc(), ids, f.ref, 0, 2 * kPpq, PitchId::fromDegree(3), t);
  commit(f.store, made);
  const JiGroupId id = made->jiGroups.at(0);
  const NoteId anchor = made->notes.at(0).id;
  const double hz = f.hz(anchor);
  CHECK(f.harmonics(id).proportion == terms({22, 26, 30, 33}));
  commit(f.store, ops::walkJiGroup(f.doc(), f.ref, id, -1));  // one undo step
  CHECK(f.harmonics(id).proportion == terms({21, 25, 29, 32}));
  CHECK(f.group(id).index == 21);
  CHECK(f.hz(anchor) == hz);  // bit for bit
  CHECK(f.noteOf(anchor).pitch == PitchId::fromDegree(3));
  CHECK(f.noteOf(made->notes[1].id).pitch == PitchId::fromRatio(Rational(25, 21), AnchorKind::Note, anchor.value));
  CHECK(f.hz(made->notes[3].id) == hz * (32.0 / 21.0));
  commit(f.store, ops::walkJiGroup(f.doc(), f.ref, id, 3));
  CHECK(f.harmonics(id).proportion == terms({24, 28, 32, 35}));
  CHECK(f.hz(anchor) == hz);
  CHECK(ops::walkJiGroup(f.doc(), f.ref, id, 0)->empty());
  CHECK_FALSE(ops::walkJiGroup(f.doc(), f.ref, id, -24).ok());  // the anchor would be harmonic 0
  CHECK_FALSE(ops::walkJiGroup(f.doc(), f.ref, id, static_cast<int32_t>(kMaxHarmonicIndex)).ok());
  commit(f.store, ops::walkJiGroup(f.doc(), f.ref, id, -23));  // 1:5:9:12
  CHECK(f.harmonics(id).proportion == terms({1, 5, 9, 12}));
  CHECK_FALSE(ops::walkJiGroup(f.doc(), f.ref, id, -1).ok());  // refused at 1
  CHECK(f.hz(anchor) == hz);
  // A member below the anchor stops the walk first.
  auto low = ops::addGroupNote(f.doc(), ids, f.ref, id, ops::GroupNotePitch{Rational(1, 2), 0}, 0, kPpq);
  commit(f.store, low);
  CHECK(f.harmonics(id).proportion == terms({1, 2, 10, 18, 24}));
  CHECK_FALSE(ops::walkJiGroup(f.doc(), f.ref, id, -1).ok());
  f.roundTrips();
}

TEST_CASE("proportionNotes: 8:9:10:12:15 is five regular notes in one group, the lowest the anchor on the bottom pitch") {
  JiFix f;
  IdSource& ids = f.store.ids();
  const int64_t t[] = {8, 9, 10, 12, 15};
  const uint32_t before = ids.peek();
  CHECK_FALSE(ops::proportionNotes(f.doc(), ids, f.ref, 0, kPpq, PitchId::fromDegree(0), std::vector<int64_t>{}).ok());
  CHECK_FALSE(ops::proportionNotes(f.doc(), ids, f.ref, 0, kPpq, PitchId::fromDegree(0), std::vector<int64_t>{4, 4}).ok());
  CHECK_FALSE(ops::proportionNotes(f.doc(), ids, f.ref, 0, kPpq, PitchId::fromDegree(0), std::vector<int64_t>{0, 4}).ok());
  CHECK_FALSE(ops::proportionNotes(f.doc(), ids, f.ref, 0, 0, PitchId::fromDegree(0), t).ok());  // no length
  CHECK_FALSE(ops::proportionNotes(f.doc(), ids, f.ref, 0, kPpq, PitchId::fromRatio(Rational(3, 2), AnchorKind::Note, 999999), t).ok());
  CHECK(ids.peek() == before);  // a refused proportion costs no ids
  auto made = ops::proportionNotes(f.doc(), ids, f.ref, kPpq, 3 * kPpq, PitchId::fromCents(-250.0), t, 90.0f);
  commit(f.store, made);  // one undo step
  REQUIRE(made->notes.size() == 5);
  REQUIRE(made->jiGroups.size() == 1);
  const JiGroupId id = made->jiGroups[0];
  CHECK(f.clip().notes.size() == 5);
  CHECK(f.group(id).anchor == made->notes[0].id);
  CHECK(f.group(id).index == 8);
  CHECK(f.harmonics(id).proportion == terms({8, 9, 10, 12, 15}));
  const double root = f.hz(made->notes[0].id);
  const double want[] = {1.0, 9.0 / 8.0, 10.0 / 8.0, 12.0 / 8.0, 15.0 / 8.0};
  for (size_t i = 0; i < 5; ++i) {
    const Note& n = f.noteOf(made->notes[i].id);
    CHECK(n.start == kPpq);
    CHECK(n.length == 3 * kPpq);
    CHECK(n.velocity == 90.0f);
    CHECK(f.hz(n.id) == root * want[i]);
  }
  CHECK(f.noteOf(made->notes[0].id).pitch == PitchId::fromCents(-250.0));
  CHECK(f.clip().written == nullptr);  // typed as JI: nothing was written in the tuning
  f.roundTrips();
}

TEST_CASE("steps: Up / Down on a member is the next / previous harmonic of its group; a stepped member is re-written as the nearest step (M4)") {
  JiFix f;
  IdSource& ids = f.store.ids();
  const std::vector<Note> triad = f.chord({0, 4, 7});
  auto tuned = ops::tuneToJi(f.doc(), ids, f.ref, JiFix::keysOf(triad));
  commit(f.store, tuned);
  const JiGroupId id = tuned->jiGroups[0];
  const NoteKey g[] = {keyOf(f.noteOf(triad[2].id))};
  commit(f.store, ops::transposeNotes(f.doc(), f.ref, g, 1, ops::StepUnit::Grid));  // 6 → 7; one undo step
  CHECK(f.noteOf(triad[2].id).pitch == PitchId::fromRatio(Rational(7, 4), AnchorKind::Note, triad[0].id.value));
  CHECK(f.harmonics(id).proportion == terms({4, 5, 7}));
  // T7.16a (M7 review M4): re-written as the tuning's nearest step to its new pitch (7/4 = 968.8 ¢: the minor seventh),
  // so Back to Tuning puts it on a row.
  REQUIRE(findNoteWritten(f.clip(), triad[2].id) != nullptr);
  CHECK(findNoteWritten(f.clip(), triad[2].id)->written == PitchId::fromDegree(10));
  CHECK(findNoteWritten(f.clip(), triad[1].id)->written == PitchId::fromDegree(4));
  commit(f.store, ops::moveNotes(f.doc(), f.ref, g, 0, -3));  // a vertical drag: 7 → 4
  CHECK(f.noteOf(triad[2].id).pitch == PitchId::fromRatio(Rational(1), AnchorKind::Note, triad[0].id.value));
  CHECK(findNoteWritten(f.clip(), triad[2].id)->written == PitchId::fromDegree(0));
  // bug-ji-input: the anchor stepped ALONE moves as a member does (4 → 6); the others keep their pitch, number and
  // group, the fundamental stays: the group is frozen on its lowest staying note now (the 4 the 7 was dragged down to).
  // (The whole chord stepped moves as a chord: the next test case.)
  const double F = f.hz(triad[0].id) / 4.0, e1 = f.hz(triad[1].id), e2 = f.hz(triad[2].id);
  const NoteKey c[] = {keyOf(f.noteOf(triad[0].id))};
  commit(f.store, ops::transposeNotes(f.doc(), f.ref, c, 2, ops::StepUnit::Grid));
  CHECK(f.hz(triad[0].id) == doctest::Approx(F * 6.0).epsilon(1e-12));
  CHECK(f.hz(triad[1].id) == doctest::Approx(e1).epsilon(1e-12));
  CHECK(f.hz(triad[2].id) == doctest::Approx(e2).epsilon(1e-12));
  CHECK(f.group(id).anchor == triad[2].id);
  CHECK(f.group(id).index == 4);
  CHECK(f.harmonics(id).proportion == terms({4, 5, 6}));
  CHECK(f.noteOf(triad[0].id).pitch == PitchId::fromRatio(Rational(3, 2), AnchorKind::Note, triad[2].id.value));
  // Re-written as the nearest step to its new pitch (a just fifth above C: G); the others keep what they were written as.
  CHECK(findNoteWritten(f.clip(), triad[0].id)->written == PitchId::fromDegree(7));
  CHECK(findNoteWritten(f.clip(), triad[1].id)->written == PitchId::fromDegree(4));
  CHECK(findNoteWritten(f.clip(), triad[2].id)->written == PitchId::fromDegree(0));
  // Moving a member in time keeps its written identity; a typed pitch drops it.
  const NoteKey e[] = {keyOf(f.noteOf(triad[1].id))};
  auto moved = ops::moveNotes(f.doc(), f.ref, e, kPpq);
  commit(f.store, moved);
  CHECK(findNoteWritten(f.clip(), triad[1].id) != nullptr);
  commit(f.store, ops::setNotesPitch(f.doc(), f.ref, moved->notes, PitchId::fromCents(400.0)));
  CHECK(findNoteWritten(f.clip(), triad[1].id) == nullptr);
  f.roundTrips();
}

TEST_CASE("steps (bug-ji-input): an anchor moved without all its members moves alone — in pitch its group is re-anchored onto the lowest staying member (one undo step, every staying Hz and the fundamental kept); with a member; in time alone the warps stay; Back to Tuning: rows, out of the group") {
  JiFix f;
  IdSource& ids = f.store.ids();
  const std::vector<Note> triad = f.chord({0, 4, 7});
  auto tuned = ops::tuneToJi(f.doc(), ids, f.ref, JiFix::keysOf(triad));
  commit(f.store, tuned);
  const JiGroupId id = tuned->jiGroups[0];
  const double F = f.hz(triad[0].id) / 4.0, hzE = f.hz(triad[1].id), hzG = f.hz(triad[2].id);
  auto key = [&](size_t i) { return keyOf(f.noteOf(triad[i].id)); };
  // A drag of the anchor alone two harmonics down (4 → 2) and a beat later: the 5 (the lowest staying) carries the group.
  const size_t h = f.store.history().cursor();
  commit(f.store, ops::moveNotes(f.doc(), f.ref, std::vector<NoteKey>{key(0)}, kPpq, -2));
  CHECK(f.store.history().cursor() == h + 1);
  CHECK(f.hz(triad[0].id) == doctest::Approx(F * 2.0).epsilon(1e-12));
  CHECK(f.hz(triad[1].id) == doctest::Approx(hzE).epsilon(1e-12));
  CHECK(f.hz(triad[2].id) == doctest::Approx(hzG).epsilon(1e-12));
  CHECK(f.noteOf(triad[0].id).start == kPpq);
  CHECK(f.noteOf(triad[1].id).start == 0);
  CHECK(f.group(id).anchor == triad[1].id);
  CHECK(f.group(id).index == 5);
  CHECK(f.harmonics(id).proportion == terms({2, 5, 6}));
  CHECK(f.noteOf(triad[0].id).pitch == PitchId::fromRatio(Rational(2, 5), AnchorKind::Note, triad[1].id.value));
  CHECK(f.noteOf(triad[2].id).pitch == PitchId::fromRatio(Rational(6, 5), AnchorKind::Note, triad[1].id.value));
  f.roundTrips();
  REQUIRE(f.store.undo());  // one step back: the triad as it was
  CHECK(f.group(id).anchor == triad[0].id);
  CHECK(f.noteOf(triad[0].id).pitch == PitchId::fromDegree(0));
  CHECK(f.harmonics(id).proportion == terms({4, 5, 6}));
  // The anchor with one member (the 6 stays): both step on the harmonics, the 6 carries the group.
  commit(f.store, ops::transposeNotes(f.doc(), f.ref, std::vector<NoteKey>{key(0), key(1)}, 1, ops::StepUnit::Grid));
  CHECK(f.group(id).anchor == triad[2].id);
  CHECK(f.hz(triad[0].id) == doctest::Approx(F * 5.0).epsilon(1e-12));
  CHECK(f.hz(triad[1].id) == doctest::Approx(F * 6.0).epsilon(1e-12));
  CHECK(f.hz(triad[2].id) == doctest::Approx(hzG).epsilon(1e-12));
  REQUIRE(f.store.undo());
  // The whole chord: a chord (T7.16a H2), the anchor stays the anchor.
  commit(f.store, ops::transposeNotes(f.doc(), f.ref, std::vector<NoteKey>{key(0), key(1), key(2)}, 1, ops::StepUnit::Grid));
  CHECK(f.group(id).anchor == triad[0].id);
  CHECK(f.noteOf(triad[0].id).pitch == PitchId::fromDegree(1));
  REQUIRE(f.store.undo());
  // In time alone: nothing is re-anchored and the warps stay where they are in the clip.
  commit(f.store, ops::setJiWarps(f.doc(), f.ref, id, {JiWarp{kPpq, kPpq, GlideShape{}, true}}));
  commit(f.store, ops::moveNotes(f.doc(), f.ref, std::vector<NoteKey>{key(0)}, kPpq));
  CHECK(f.group(id).anchor == triad[0].id);
  CHECK(f.noteOf(triad[0].id).pitch == PitchId::fromDegree(0));
  CHECK(f.noteOf(triad[0].id).start == kPpq);
  CHECK(f.group(id).warps[0].at == 0);  // beat 2 of the clip, counted from the anchor's new start
  CHECK(NotePitchIndex(f.pitches(f.clip())).warped(triad[1].id)->offset == -kPpq);
  REQUIRE(f.store.undo());
  REQUIRE(f.store.undo());
  // Back to Tuning: the anchor stepped alone is the tempered note it sounds as: a row up, out of the group; the
  // others keep sounding as written.
  commit(f.store, ops::untuneJiGroup(f.doc(), f.ref, id));
  commit(f.store, ops::transposeNotes(f.doc(), f.ref, std::vector<NoteKey>{key(0)}, 1, ops::StepUnit::Grid));
  CHECK(f.noteOf(triad[0].id).pitch == PitchId::fromDegree(1));
  CHECK(jiGroupOfNote(f.clip(), f.noteOf(triad[0].id)) == nullptr);
  CHECK(f.group(id).anchor == triad[1].id);
  CHECK(f.hz(triad[1].id) == doctest::Approx(f.degreeHz(4)).epsilon(1e-12));
  CHECK(f.hz(triad[2].id) == doctest::Approx(f.degreeHz(7)).epsilon(1e-12));
  f.roundTrips();
}

TEST_CASE("steps (T7.16a, M7 review H2): Up / Shift+Up / a vertical drag on a whole tuned chord transpose the chord — the anchor steps, the members follow with their ratios, the written identities step with them") {
  JiFix f;
  IdSource& ids = f.store.ids();
  const std::vector<Note> triad = f.chord({0, 4, 7});
  const double hz[3] = {f.hz(triad[0].id), f.hz(triad[1].id), f.hz(triad[2].id)};
  auto tuned = ops::tuneToJi(f.doc(), ids, f.ref, JiFix::keysOf(triad));
  commit(f.store, tuned);
  const JiGroupId id = tuned->jiGroups[0];
  auto keys = [&] {
    std::vector<NoteKey> k;
    for (const Note& n : triad) k.push_back(keyOf(f.noteOf(n.id)));
    return k;
  };
  auto ratiosKept = [&] {
    CHECK(f.harmonics(id).proportion == terms({4, 5, 6}));
    CHECK(f.hz(triad[1].id) == f.hz(triad[0].id) * 1.25);
    CHECK(f.hz(triad[2].id) == f.hz(triad[0].id) * 1.5);
  };
  // Up: one step of the tuning.
  commit(f.store, ops::transposeNotes(f.doc(), f.ref, keys(), 1, ops::StepUnit::Grid));
  CHECK(f.noteOf(triad[0].id).pitch == PitchId::fromDegree(1));
  ratiosKept();
  CHECK(findNoteWritten(f.clip(), triad[1].id)->written == PitchId::fromDegree(5));
  CHECK(findNoteWritten(f.clip(), triad[2].id)->written == PitchId::fromDegree(8));
  // Shift+Up: an equave.
  commit(f.store, ops::transposeNotes(f.doc(), f.ref, keys(), 1, ops::StepUnit::Equave));
  CHECK(f.noteOf(triad[0].id).pitch == PitchId::fromDegree(1, 1));
  ratiosKept();
  CHECK(findNoteWritten(f.clip(), triad[2].id)->written == PitchId::fromDegree(8, 1));
  // A vertical drag of the chord, two rows down (and a beat later).
  commit(f.store, ops::moveNotes(f.doc(), f.ref, keys(), kPpq, -2));
  CHECK(f.noteOf(triad[0].id).pitch == PitchId::fromDegree(11, 0));
  ratiosKept();
  CHECK(f.noteOf(triad[1].id).start == kPpq);
  // Back to Tuning restores the TRANSPOSED written chord, exactly: 11 + 12 = 23 → degrees 11, 15, 18 of the equave.
  commit(f.store, ops::untuneJiGroup(f.doc(), f.ref, id));
  CHECK(f.hz(triad[0].id) == doctest::Approx(hz[0] * std::exp2(11.0 / 12.0)).epsilon(1e-12));
  CHECK(f.hz(triad[1].id) == doctest::Approx(hz[1] * std::exp2(11.0 / 12.0)).epsilon(1e-12));
  CHECK(f.hz(triad[2].id) == doctest::Approx(hz[2] * std::exp2(11.0 / 12.0)).epsilon(1e-12));
  // In a group Back to Tuning a member stepped alone is the tempered note it sounds as: it steps on the rows and
  // leaves the group.
  commit(f.store, ops::transposeNotes(f.doc(), f.ref, std::vector<NoteKey>{keyOf(f.noteOf(triad[2].id))}, 1, ops::StepUnit::Grid));
  CHECK(f.noteOf(triad[2].id).pitch == PitchId::fromDegree(7, 1));
  CHECK(findNoteWritten(f.clip(), triad[2].id) == nullptr);
  CHECK(f.harmonics(id).proportion == terms({4, 5}));
  f.roundTrips();
}

// ---- Anchor loss ------------------------------------------------------------------------------------------------------

TEST_CASE("Back to Tuning (T7.16a, M7 review M3): a chord built in JI goes to the tuning's nearest steps and back to its exact ratios") {
  JiFix f;
  IdSource& ids = f.store.ids();
  const int64_t t[] = {4, 5, 6, 7};
  auto made = ops::proportionNotes(f.doc(), ids, f.ref, 0, kPpq, PitchId::fromDegree(0), t);
  commit(f.store, made);
  const JiGroupId id = made->jiGroups[0];
  const double root = f.hz(made->notes[0].id);
  CHECK(f.clip().written == nullptr);
  commit(f.store, ops::untuneJiGroup(f.doc(), f.ref, id));
  CHECK_FALSE(f.group(id).tuned);
  CHECK(findNoteWritten(f.clip(), made->notes[1].id)->written == PitchId::fromDegree(4));
  CHECK(findNoteWritten(f.clip(), made->notes[2].id)->written == PitchId::fromDegree(7));
  CHECK(findNoteWritten(f.clip(), made->notes[3].id)->written == PitchId::fromDegree(10));
  CHECK(f.hz(made->notes[1].id) == f.degreeHz(4));
  CHECK(f.hz(made->notes[3].id) == f.degreeHz(10));
  CHECK(f.hz(made->notes[0].id) == root);
  f.roundTrips();
  commit(f.store, ops::setJiGroupsTuned(f.doc(), f.ref, std::vector<JiGroupId>{id}, true));
  CHECK(f.hz(made->notes[1].id) == root * 1.25);
  CHECK(f.hz(made->notes[3].id) == root * 1.75);
  CHECK(f.harmonics(id).proportion == terms({4, 5, 6, 7}));
  // A member drawn on the guides of a tuned chord later (no written identity) goes to its nearest step too.
  auto drawn = ops::addGroupNote(f.doc(), ids, f.ref, id, ops::GroupNotePitch{{}, 9}, 0, kPpq);
  commit(f.store, drawn);
  commit(f.store, ops::untuneJiGroup(f.doc(), f.ref, id));
  CHECK(f.hz(drawn->notes[0].id) == f.degreeHz(14));  // 9/4 = 1403.9 ¢: the 14th step
  CHECK_FALSE(ops::setJiGroupsTuned(f.doc(), f.ref, std::vector<JiGroupId>{JiGroupId{999999}}, true).ok());
}

TEST_CASE("tuneChordsToJi (T7.16a, M7 review M6): a progression is tuned chord by chord in one step; chords Back to Tuning get their own reading back") {
  JiFix f;
  IdSource& ids = f.store.ids();
  const std::vector<Note> one = f.chord({0, 4, 7}, 0, 2 * kPpq);        // C major
  const std::vector<Note> two = f.chord({4, 7, 11}, 2 * kPpq, 2 * kPpq);  // E minor
  const std::vector<Note> three = f.chord({2, 5, 9}, 4 * kPpq, 2 * kPpq);  // D minor
  const std::vector<NoteKey> chords[] = {JiFix::keysOf(one), JiFix::keysOf(two), JiFix::keysOf(three)};
  const size_t h = f.store.history().cursor();
  auto tuned = ops::tuneChordsToJi(f.doc(), ids, f.ref, chords);
  commit(f.store, tuned);
  CHECK(f.store.history().cursor() == h + 1);
  REQUIRE(jiGroupsOf(f.clip()).size() == 3);
  const JiGroup* ga = findJiGroupByAnchor(f.clip(), one[0].id);
  const JiGroup* gb = findJiGroupByAnchor(f.clip(), two[0].id);
  const JiGroup* gc = findJiGroupByAnchor(f.clip(), three[0].id);
  REQUIRE((ga && gb && gc));
  CHECK(groupHarmonics(f.clip(), *ga)->proportion == terms({4, 5, 6}));
  CHECK(groupHarmonics(f.clip(), *gb)->proportion == terms({10, 12, 15}));
  CHECK(groupHarmonics(f.clip(), *gc)->proportion == terms({10, 12, 15}));
  CHECK(f.hz(three[0].id) == f.degreeHz(2));  // each chord's own root is frozen: no 680-cent fifth
  CHECK(f.hz(three[2].id) == f.hz(three[0].id) * 1.5);
  // Again: nothing (they are tuned). Back to Tuning on the second only, then the progression again: the second gets its
  // own reading back, the others stay.
  CHECK(ops::tuneChordsToJi(f.doc(), ids, f.ref, chords)->empty());
  const JiGroupId second = gb->id;
  ops::TuneToJiOptions other;
  other.proportion = {20, 24, 30};  // 10:12:15 named as 20:24:30: the index 20
  commit(f.store, ops::tuneToJi(f.doc(), ids, f.ref, chords[1], other));
  commit(f.store, ops::untuneJiGroup(f.doc(), f.ref, second));
  commit(f.store, ops::tuneChordsToJi(f.doc(), ids, f.ref, chords));
  CHECK(f.group(second).tuned);
  CHECK(f.group(second).index == 20);  // its own reading, not a new best one
  CHECK(jiGroupsOf(f.clip()).size() == 3);
}

TEST_CASE("anchor loss (T7.16a, M7 review M8): a vibrato on the anchor and on every member does not dissolve the group") {
  JiFix f;
  IdSource& ids = f.store.ids();
  const std::vector<Note> triad = f.chord({0, 4, 7});
  auto tuned = ops::tuneToJi(f.doc(), ids, f.ref, JiFix::keysOf(triad));
  commit(f.store, tuned);
  const JiGroupId id = tuned->jiGroups[0];
  NoteVibrato v;
  v.depthCents = 20.0f;
  v.rate = 6.0f;
  std::vector<ops::NoteVibratoEdit> edits;
  for (const Note& n : triad) edits.push_back({keyOf(f.noteOf(n.id)), v});
  commit(f.store, ops::setNoteVibrato(f.doc(), f.ref, edits));
  const double he = f.hz(triad[1].id), hg = f.hz(triad[2].id);
  commit(f.store, ops::deleteNotes(f.doc(), f.ref, std::vector<NoteKey>{keyOf(f.noteOf(triad[0].id))}));
  REQUIRE(jiGroupsOf(f.clip()).size() == 1);
  CHECK(f.group(id).anchor == triad[1].id);
  CHECK(f.noteOf(triad[2].id).pitch == PitchId::fromRatio(Rational(6, 5), AnchorKind::Note, triad[1].id.value));
  CHECK(f.hz(triad[1].id) == doctest::Approx(he).epsilon(1e-13));
  CHECK(f.hz(triad[2].id) == doctest::Approx(hg).epsilon(1e-13));
  CHECK(findNoteWritten(f.clip(), triad[2].id)->written == PitchId::fromDegree(7));
}

TEST_CASE("tune to JI (T7.16a, M7 review M9): two members of one anchor tuned without it keep their exact ratio") {
  JiFix f;
  IdSource& ids = f.store.ids();
  const Note d = f.add(0, PitchId::fromDegree(2), 4 * kPpq);
  commit(f.store, ops::makeJiGroup(f.doc(), ids, f.ref, keyOf(d), 8));
  const Note a = f.add(0, PitchId::fromRatio(Rational(5, 4), AnchorKind::Note, d.id.value), 4 * kPpq);
  const Note b = f.add(0, PitchId::fromRatio(Rational(45, 32), AnchorKind::Note, d.id.value), 4 * kPpq);
  auto cands = ops::tuneToJiCandidates(f.doc(), f.ref, std::vector<NoteKey>{keyOf(a), keyOf(b)});
  REQUIRE(cands.ok());
  REQUIRE(!cands->empty());
  for (const ops::JiCandidate& c : *cands) CHECK(c.terms[1] * 8 == c.terms[0] * 9);  // 9/8 in every reading
  commit(f.store, ops::tuneToJi(f.doc(), ids, f.ref, std::vector<NoteKey>{keyOf(a), keyOf(b)}));
  CHECK(f.noteOf(b.id).pitch == PitchId::fromRatio(Rational(9, 8), AnchorKind::Note, a.id.value));
  CHECK(f.hz(b.id) == doctest::Approx(f.hz(a.id) * 1.125).epsilon(1e-13));
}

TEST_CASE("consolidate (T7.16a, M7 review H6): members whose anchor lies before the clip's start marker keep sounding") {
  JiFix f;
  IdSource& ids = f.store.ids();
  const Note anchor = f.add(0, PitchId::fromDegree(0), kPpq);
  commit(f.store, ops::makeJiGroup(f.doc(), ids, f.ref, keyOf(anchor), 4));
  std::vector<Note> melody;
  for (int i = 0; i < 4; ++i) {
    const int64_t h[] = {5, 6, 7, 8};
    melody.push_back(f.add(kBar + i * kPpq, PitchId::fromRatio(Rational(h[i], 4), AnchorKind::Note, anchor.id.value), kPpq));
  }
  std::vector<double> hz;
  for (const Note& n : melody) hz.push_back(f.hz(n.id));
  // The clip's start trimmed to bar 2: the anchor is hidden, and still drives the melody.
  const NoteClip& c = f.clip();
  REQUIRE(f.store.dispatch(SetClipBounds{f.ref, c.start + kBar, c.length - kBar, c.contentStart + kBar}).ok());
  CHECK(f.hz(melody[0].id) == hz[0]);
  const ClipRef sel[] = {f.ref};
  commit(f.store, ops::consolidateClips(f.doc(), ids, sel));
  const NoteClip& m = *f.track(f.bass).clips.at(0).clip;
  REQUIRE(m.notes.size() == 4);
  size_t i = 0;
  for (const Note& n : m.notes) {
    const std::optional<double> v = f.hzIn(m, n.id);
    REQUIRE(v.has_value());
    CHECK(*v == doctest::Approx(hz[i++]).epsilon(1e-12));
  }
  REQUIRE(jiGroupsOf(m).size() == 1);  // the group lives on: its lowest member promoted
  CHECK(groupHarmonics(m, jiGroupsOf(m)[0])->proportion == terms({5, 6, 7, 8}));
  CHECK(validateClipJi(m).ok());
}

TEST_CASE("bends (T7.16a, M7 review M2): a bend from a note whose group warps starts where that note IS at the hand-over, not where it started") {
  JiFix f;
  IdSource& ids = f.store.ids();
  const std::vector<Note> triad = f.chord({0, 4, 7}, 0, 4 * kPpq);
  auto tuned = ops::tuneToJi(f.doc(), ids, f.ref, JiFix::keysOf(triad));
  commit(f.store, tuned);
  // JI at the start, back to 12-EDO at beat 2.
  commit(f.store, ops::setJiWarps(f.doc(), f.ref, tuned->jiGroups[0], {JiWarp{2 * kPpq, 0, GlideShape{}, false}}));
  const Note fNote = f.add(4 * kPpq, PitchId::fromDegree(5), 2 * kPpq);
  commit(f.store, ops::setNoteBend(f.doc(), f.ref, keyOf(fNote), ops::NoteEdge::Start, kPpq, GlideShape{}, triad[1].id));
  const NotePitchIndex index(f.pitches(f.clip()));
  const NoteBendTargets t = noteBendTargets(*findNoteCurves(f.clip(), fNote.id), index);
  REQUIRE(t.in.has_value());
  // The E ends at its written 400 ¢, so the F (500 ¢) glides in from 100 ¢ below; its start pitch (386.3 ¢) is not it.
  CHECK(*t.in == doctest::Approx(-100.0).epsilon(1e-9));
  const std::vector<Breakpoint> curve = f.effective(f.clip(), f.noteOf(fNote.id));
  REQUIRE(!curve.empty());
  CHECK(curve.front().value == doctest::Approx(-100.0f).epsilon(1e-5));
}

TEST_CASE("anchor loss: deleting the anchor promotes the lowest remaining member — every member keeps its Hz and its number") {
  JiFix f;
  IdSource& ids = f.store.ids();
  SUBCASE("a tempered anchor: the new anchor carries the pitch, the others their exact ratios to it") {
    const std::vector<Note> chord = f.chord({0, 4, 7, 10});
    ops::TuneToJiOptions o;
    o.proportion = {4, 5, 6, 7};
    auto tuned = ops::tuneToJi(f.doc(), ids, f.ref, JiFix::keysOf(chord), o);
    commit(f.store, tuned);
    const JiGroupId id = tuned->jiGroups[0];
    commit(f.store, ops::setJiWarps(f.doc(), f.ref, id, {JiWarp{8 * kPpq, kPpq, GlideShape{}, false}}));  // JI until beat 8
    const double h5 = f.hz(chord[1].id), h6 = f.hz(chord[2].id), h7 = f.hz(chord[3].id);
    const Snapshot was = commit(f.store, ops::deleteNotes(f.doc(), f.ref, std::vector<NoteKey>{keyOf(chord[0])}));  // one undo step
    REQUIRE(jiGroupsOf(f.clip()).size() == 1);
    CHECK(f.group(id).anchor == chord[1].id);  // the lowest member
    CHECK(f.group(id).index == 5);             // the number it had
    CHECK(f.group(id).warps.size() == 1);      // the warps stay
    CHECK(f.harmonics(id).proportion == terms({5, 6, 7}));
    CHECK(f.noteOf(chord[1].id).pitch.kind() == PitchKind::Cents);
    CHECK(f.noteOf(chord[2].id).pitch == PitchId::fromRatio(Rational(6, 5), AnchorKind::Note, chord[1].id.value));
    CHECK(f.noteOf(chord[3].id).pitch == PitchId::fromRatio(Rational(7, 5), AnchorKind::Note, chord[1].id.value));
    CHECK(f.hz(chord[1].id) == doctest::Approx(h5).epsilon(1e-13));
    CHECK(f.hz(chord[2].id) == doctest::Approx(h6).epsilon(1e-13));
    CHECK(f.hz(chord[3].id) == doctest::Approx(h7).epsilon(1e-13));
    // T7.16a: the new anchor keeps what it was written as (an anchor does not warp; Back to Tuning plays it).
    CHECK(findNoteWritten(f.clip(), chord[1].id)->written == PitchId::fromDegree(4));
    CHECK(findNoteWritten(f.clip(), chord[2].id)->written == PitchId::fromDegree(7));
    f.roundTrips();
    REQUIRE(f.store.undo());
    CHECK(f.store.current() == was);
    REQUIRE(f.store.redo());
    // Down to the last note: a group of its anchor alone; then nothing.
    commit(f.store, ops::deleteNotes(f.doc(), f.ref, std::vector<NoteKey>{keyOf(f.noteOf(chord[1].id)), keyOf(f.noteOf(chord[2].id))}));
    CHECK(f.group(id).anchor == chord[3].id);
    CHECK(f.group(id).index == 7);
    commit(f.store, ops::deleteNotes(f.doc(), f.ref, std::vector<NoteKey>{keyOf(f.noteOf(chord[3].id))}));
    CHECK(f.clip().groups == nullptr);
    CHECK(f.clip().written == nullptr);
  }
  SUBCASE("an anchor that is an exact ratio of the reference: the new anchor is one too") {
    const int64_t t[] = {4, 5, 6};
    auto made = ops::proportionNotes(f.doc(), ids, f.ref, 0, kPpq, PitchId::fromRatio(Rational(3, 2)), t);
    commit(f.store, made);
    commit(f.store, ops::deleteNotes(f.doc(), f.ref, std::vector<NoteKey>{made->notes[0]}));
    const JiGroupId id = made->jiGroups[0];
    CHECK(f.group(id).anchor == made->notes[1].id);
    CHECK(f.group(id).index == 5);
    CHECK(f.noteOf(made->notes[1].id).pitch == PitchId::fromRatio(Rational(15, 8)));  // 3/2 · 5/4, exact
    CHECK(f.noteOf(made->notes[2].id).pitch == PitchId::fromRatio(Rational(6, 5), AnchorKind::Note, made->notes[1].id.value));
  }
  SUBCASE("deleting a member, or the whole group, needs no promotion") {
    const int64_t t[] = {4, 5, 6};
    auto made = ops::proportionNotes(f.doc(), ids, f.ref, 0, kPpq, PitchId::fromDegree(0), t);
    commit(f.store, made);
    auto del = ops::deleteNotes(f.doc(), f.ref, std::vector<NoteKey>{made->notes[2]});
    REQUIRE(del.ok());
    CHECK(del->batch().commands.size() == 1);
    commit(f.store, del);
    CHECK(f.harmonics(made->jiGroups[0]).proportion == terms({4, 5}));
    commit(f.store, ops::deleteNotes(f.doc(), f.ref, std::vector<NoteKey>{made->notes[0], made->notes[1]}));
    CHECK(f.clip().groups == nullptr);
  }
}

// ---- Copies -----------------------------------------------------------------------------------------------------------

TEST_CASE("copies: clone, split, duplicate, paste and consolidate re-point groups, members and written identities") {
  JiFix f;
  IdSource& ids = f.store.ids();
  // A tuned triad in bar 1 (the anchor) and bar 2 (its members), warped, and an unrelated group in bar 3.
  const Note c = f.add(0, PitchId::fromDegree(0), kBar);
  const Note e = f.add(kBar, PitchId::fromDegree(4), kPpq);
  const Note g = f.add(kBar + kPpq, PitchId::fromDegree(7), kPpq);
  auto tuned = ops::tuneToJi(f.doc(), ids, f.ref, std::vector<NoteKey>{keyOf(c), keyOf(e), keyOf(g)});
  commit(f.store, tuned);
  const JiGroupId id = tuned->jiGroups[0];
  const std::vector<JiWarp> warps = {JiWarp{kBar + kPpq / 2, kPpq / 4, GlideShape{}, true}};
  commit(f.store, ops::setJiWarps(f.doc(), f.ref, id, warps));
  const int64_t t[] = {7, 9};
  auto other = ops::proportionNotes(f.doc(), ids, f.ref, 2 * kBar, kPpq, PitchId::fromDegree(2), t);
  commit(f.store, other);
  const double hc = f.hz(c.id), he = f.hz(e.id), hg = f.hz(g.id);
  CHECK(he == f.degreeHz(4));       // E starts before the warp: as written
  CHECK(hg == hc * 1.5);            // G after it: JI
  auto clipsOf = [&]() -> const std::vector<ClipSlot>& { return f.track(f.bass).clips; };

  SUBCASE("duplicating the clip: fresh group and note ids, the same structure and sound") {
    const ClipRef sel[] = {f.ref};
    commit(f.store, ops::duplicateClips(f.doc(), ids, sel));
    const NoteClip& src = *clipsOf()[0].clip;
    const NoteClip& cp = *clipsOf()[1].clip;
    CHECK(src.groups == f.clip().groups);
    REQUIRE(jiGroupsOf(cp).size() == 2);
    const Note& c2 = BendFix::noteAt(cp, 0);
    const Note& e2 = BendFix::noteAt(cp, kBar);
    const Note& g2 = BendFix::noteAt(cp, kBar + kPpq);
    const JiGroup* copy = findJiGroupByAnchor(cp, c2.id);
    REQUIRE(copy != nullptr);
    CHECK(copy->id != id);
    CHECK(copy->index == 4);
    CHECK(copy->warps == warps);
    CHECK(e2.pitch == PitchId::fromRatio(Rational(5, 4), AnchorKind::Note, c2.id.value));
    CHECK(findNoteWritten(cp, e2.id)->written == PitchId::fromDegree(4));
    CHECK(findNoteWritten(cp, e.id) == nullptr);
    CHECK(*f.hzIn(cp, e2.id) == he);
    CHECK(*f.hzIn(cp, g2.id) == hg);
    CHECK(validateClipJi(cp).ok());
    f.roundTrips();  // ids are unique across the project
  }
  SUBCASE("a split between the anchor and its members: the group lives on in both pieces") {
    const ClipRef sel[] = {f.ref};
    const Ticks cut[] = {kBar};
    commit(f.store, ops::splitClips(f.doc(), ids, sel, cut));  // one undo step
    const NoteClip& left = *clipsOf()[0].clip;
    const NoteClip& right = *clipsOf()[1].clip;
    REQUIRE(jiGroupsOf(left).size() == 1);
    CHECK(jiGroupsOf(left)[0].id == id);
    CHECK(jiGroupsOf(left)[0].anchor == c.id);
    CHECK(left.written == nullptr);
    REQUIRE(jiGroupsOf(right).size() == 2);
    const JiGroup* promoted = findJiGroupByAnchor(right, e.id);
    REQUIRE(promoted != nullptr);
    CHECK(promoted->id != id);       // ids name one entity
    CHECK(promoted->index == 5);
    CHECK(findJiGroup(right, other->jiGroups[0]) != nullptr);  // the unrelated group went right whole, as it was
    CHECK(right.notes.findById(g.id)->pitch == PitchId::fromRatio(Rational(6, 5), AnchorKind::Note, e.id.value));
    CHECK(*f.hzIn(left, c.id) == hc);
    CHECK(*f.hzIn(right, g.id) == doctest::Approx(hg).epsilon(1e-13));
    CHECK(findNoteWritten(right, e.id)->written == PitchId::fromDegree(4));  // kept by the new anchor (T7.16a)
    CHECK(jiGroupsOf(right)[0].warps.empty() != jiGroupsOf(right)[1].warps.empty());
    CHECK(promoted->warps[0].at == warps[0].at - kBar);  // where they were in the clip, from the new anchor (M7)
    CHECK(findNoteWritten(right, g.id) != nullptr);
    CHECK(validateClipJi(left).ok());
    CHECK(validateClipJi(right).ok());
    f.roundTrips();
    // Consolidating the pieces: every note, every group (the two halves stay two groups).
    const ClipRef both[] = {{f.bass, left.id}, {f.bass, right.id}};
    commit(f.store, ops::consolidateClips(f.doc(), ids, both));
    const NoteClip& m = *clipsOf()[0].clip;
    CHECK(m.notes.size() == 5);
    CHECK(jiGroupsOf(m).size() == 3);
    CHECK(validateClipJi(m).ok());
    f.roundTrips();
  }
  SUBCASE("consolidating a looped clip: each pass has its own copy of a group") {
    REQUIRE(f.store.dispatch(SetClipLoop{f.ref, ClipLoop{true, 0, 2 * kBar}}).ok());
    const ClipRef sel[] = {f.ref};
    commit(f.store, ops::consolidateClips(f.doc(), ids, sel));  // 4 bars = two passes of the 2-bar loop
    const NoteClip& m = *clipsOf()[0].clip;
    CHECK(m.notes.size() == 6);
    REQUIRE(jiGroupsOf(m).size() == 2);
    CHECK(jiGroupsOf(m)[0].id != jiGroupsOf(m)[1].id);
    for (const JiGroup& grp : jiGroupsOf(m)) {
      const std::optional<GroupHarmonics> h = groupHarmonics(m, grp);
      REQUIRE(h.has_value());
      CHECK(h->proportion == terms({4, 5, 6}));
      CHECK(grp.warps == warps);
    }
    CHECK(notesWrittenOf(m).size() == 4);
    CHECK(validateClipJi(m).ok());
    f.roundTrips();
  }
  SUBCASE("duplicating notes: the anchor with its members is a new group; a member alone stays in its group") {
    auto dup = ops::duplicateNotes(f.doc(), ids, f.ref, std::vector<NoteKey>{keyOf(c), keyOf(f.noteOf(e.id)), keyOf(f.noteOf(g.id))}, 4 * kBar);
    commit(f.store, dup);  // one undo step
    REQUIRE(dup->jiGroups.size() == 1);
    REQUIRE(dup->notes.size() == 3);
    const JiGroup& copy = f.group(dup->jiGroups[0]);
    CHECK(copy.id != id);
    CHECK(copy.anchor == dup->notes[0].id);
    CHECK(copy.warps == warps);
    CHECK(f.noteOf(dup->notes[1].id).pitch == PitchId::fromRatio(Rational(5, 4), AnchorKind::Note, dup->notes[0].id.value));
    CHECK(findNoteWritten(f.clip(), dup->notes[1].id)->written == PitchId::fromDegree(4));
    CHECK(f.harmonics(id).proportion == terms({4, 5, 6}));  // the source group is as it was
    auto alone = ops::duplicateNotes(f.doc(), ids, f.ref, std::vector<NoteKey>{keyOf(f.noteOf(g.id))}, kPpq);
    commit(f.store, alone);
    CHECK(alone->jiGroups.empty());
    CHECK(noteAnchorOf(f.noteOf(alone->notes[0].id).pitch) == c.id);
    CHECK(f.harmonics(id).members.size() == 3);
    CHECK(findNoteWritten(f.clip(), alone->notes[0].id)->written == PitchId::fromDegree(7));
    f.roundTrips();
  }
  SUBCASE("pasting a copied group (clipboard notes, groups, written identities) makes a group of its own") {
    const std::vector<Note> board = {c, f.noteOf(e.id), f.noteOf(g.id)};
    const std::vector<JiGroup> boardGroups = {f.group(id)};
    const std::vector<NoteWritten> boardWritten(notesWrittenOf(f.clip()).begin(), notesWrittenOf(f.clip()).end());
    std::vector<Note> pasted = board;
    for (Note& n : pasted) n.start += 5 * kBar;
    auto paste = ops::addNotes(f.doc(), ids, f.ref, pasted, {}, ops::JiSource{boardGroups, boardWritten});
    commit(f.store, paste);  // one undo step
    REQUIRE(paste->jiGroups.size() == 1);
    const JiGroup& copy = f.group(paste->jiGroups[0]);
    CHECK(copy.anchor == paste->notes[0].id);
    CHECK(copy.index == 4);
    CHECK(f.noteOf(paste->notes[2].id).pitch == PitchId::fromRatio(Rational(3, 2), AnchorKind::Note, paste->notes[0].id.value));
    CHECK(f.hz(paste->notes[1].id) == he);
    CHECK(f.hz(paste->notes[2].id) == hg);
    CHECK(jiGroupsOf(f.clip()).size() == 3);
    f.roundTrips();
  }
}

// ---- Commands, the file ----------------------------------------------------------------------------------------------

TEST_CASE("ji commands: validation — groups, written identities, curves; the loader refuses what the commands refuse") {
  JiFix f;
  IdSource& ids = f.store.ids();
  const Note a = f.add(0, PitchId::fromDegree(0));
  const Note b = f.add(kPpq, PitchId::fromRatio(Rational(5, 4), AnchorKind::Note, a.id.value));
  auto refused = [&](const Command& c) {
    const Snapshot before = f.store.current();
    CHECK_FALSE(f.store.dispatch(c).ok());
    CHECK(f.store.current() == before);
  };
  JiGroup g;
  g.id = ids.jiGroup();
  g.anchor = a.id;
  g.index = 4;
  g.warps = {JiWarp{0, 10, GlideShape{GlideShape::Bezier, 0.25f, -0.5f, 0.75f, 1.5f}, true}, JiWarp{10, 0, GlideShape{}, false}};
  JiGroup bad = g;
  bad.id = {};
  refused(SetJiGroups{f.ref, {bad}});
  bad = g;
  bad.anchor = NoteId{999999};  // not a note of the clip
  refused(SetJiGroups{f.ref, {bad}});
  bad = g;
  bad.index = 0;
  refused(SetJiGroups{f.ref, {bad}});
  bad = g;
  bad.index = kMaxHarmonicIndex + 1;
  refused(SetJiGroups{f.ref, {bad}});
  bad = g;
  bad.warps[1].toJi = true;
  refused(SetJiGroups{f.ref, {bad}});
  refused(SetJiGroups{f.ref, {g, g}});  // one id twice
  REQUIRE(f.store.dispatch(SetJiGroups{f.ref, {g}}).ok());
  CHECK(f.store.lastChanges().sequenceOnly());  // never a recompile
  CHECK(f.store.lastChanges().jiGroups == std::vector<JiGroupId>{g.id});
  const Snapshot withGroup = f.store.current();
  REQUIRE(f.store.dispatch(SetJiGroups{f.ref, {g}}).ok());
  CHECK(f.store.current() == withGroup);  // a no-op
  // Written identities: a note of the clip, a plain pitch, one per note.
  refused(SetNotesWritten{f.ref, {NoteWritten{NoteId{999999}, PitchId::fromDegree(4)}}});
  refused(SetNotesWritten{f.ref, {NoteWritten{b.id, PitchId::fromRatio(Rational(5, 4), AnchorKind::Note, a.id.value)}}});
  refused(SetNotesWritten{f.ref, {NoteWritten{b.id, PitchId::fromCents(std::nan(""))}}});
  refused(SetNotesWritten{f.ref, {NoteWritten{b.id, PitchId::fromDegree(4)}, NoteWritten{b.id, PitchId::fromDegree(5)}}});
  REQUIRE(f.store.dispatch(SetNotesWritten{f.ref, {NoteWritten{b.id, PitchId::fromDegree(4)}}}).ok());
  CHECK(f.store.lastChanges().sequenceOnly());
  CHECK(f.store.lastChanges().notes == std::vector<NoteId>{b.id});
  f.roundTrips();
  // RemoveNotes drops the removed note's written identity, and a group with its anchor; undo brings both back.
  const Snapshot full = f.store.current();
  REQUIRE(f.store.dispatch(RemoveNotes{f.ref, {keyOf(b)}}).ok());
  CHECK(f.clip().written == nullptr);
  CHECK(f.clip().groups != nullptr);
  REQUIRE(f.store.dispatch(RemoveNotes{f.ref, {keyOf(a)}}).ok());
  CHECK(f.clip().groups == nullptr);
  REQUIRE(f.store.undo());
  REQUIRE(f.store.undo());
  CHECK(f.store.current() == full);
  // Curves: by note key; RemoveNotes drops the note's curve.
  const std::vector<Breakpoint> curve = {{0, 0.0f, 0.0f, {}}, {100, 50.0f, 0.5f, {}}, {100, -50.0f, 0.0f, {}}};
  refused(SetNoteCurve{f.ref, keyOf(b), {{0, 0.0f, 0.0f, BreakpointId{9}}}});
  refused(SetNoteCurve{f.ref, keyOf(b), {{-1, 0.0f, 0.0f, {}}}});
  refused(SetNoteCurve{f.ref, keyOf(b), {{0, 1e9f, 0.0f, {}}}});
  refused(SetNoteCurve{f.ref, NoteKey{0, NoteId{999999}}, curve});
  REQUIRE(f.store.dispatch(SetNoteCurve{f.ref, keyOf(b), curve}).ok());
  CHECK(f.store.lastChanges().sequenceOnly());
  CHECK(findNoteCurves(f.clip(), b.id)->pitch == curve);
  f.roundTrips();

  // The file: every field survives; what the commands refuse is refused with a path.
  pq::Result<std::string> text = serializeProject(f.doc(), f.store.view());
  REQUIRE(text.ok());
  CHECK(text->find("\"groups\": [") != std::string::npos);
  CHECK(text->find("\"written\": [") != std::string::npos);
  auto broken = [&](std::string_view from, std::string_view to, std::string_view message) {
    std::string s = *text;
    const size_t at = s.find(from);
    REQUIRE_MESSAGE(at != std::string::npos, std::string(from));
    s.replace(at, from.size(), to);
    pq::Result<LoadedProject> r = parseProject(s);
    REQUIRE_FALSE(r.ok());
    CHECK_MESSAGE(r.error().message.find(message) != std::string::npos, r.error().message);
    CHECK(r.error().message.find("$.tracks[") != std::string::npos);
  };
  const std::string anchorKey = "\"anchor\": " + std::to_string(a.id.value) + ", \"index\": 4";
  broken(anchorKey, "\"anchor\": " + std::to_string(a.id.value) + ", \"index\": 0", "harmonic number");
  broken(anchorKey, "\"anchor\": " + std::to_string(a.id.value) + ", \"index\": 4, \"bogus\": 1", "unknown key");
  broken("\"shape\": \"bezier\"", "\"shape\": \"wobble\"", "shape must be");
  broken("\"toJi\": false", "\"toJi\": true", "alternate");
  broken("\"handles\": [0.25, -0.5, 0.75, 1.5]", "\"handles\": [0.25, -0.5, 0.75]", "handles are");
  const std::string writtenKey = "{\"note\": " + std::to_string(b.id.value) + ", \"pitch\": {\"degree\": 4, \"equave\": 0}}";
  broken(writtenKey, "{\"note\": " + std::to_string(b.id.value) + ", \"pitch\": {\"ratio\": \"5/4\", \"anchor\": \"note\", \"anchorId\": 3}}",
         "written identity");
}

TEST_CASE("note curves: duplicate, paste, delete and convert carry or drop them") {
  JiFix f;
  const Note a = f.add(0, PitchId::fromDegree(0));
  const Note b = f.add(kPpq, PitchId::fromDegree(4));
  const std::vector<Breakpoint> bend = {{0, -200.0f, 0.4f, {}}, {kPpq / 4, 0.0f, 0.0f, {}}};
  auto set = ops::setNoteCurve(f.doc(), f.ref, keyOf(a), bend);
  commit(f.store, set);
  CHECK(ops::setNoteCurve(f.doc(), f.ref, keyOf(a), bend)->empty());
  CHECK_FALSE(ops::setNoteCurve(f.doc(), f.ref, NoteKey{5, NoteId{777777}}, bend).ok());
  CHECK_FALSE(ops::setNoteCurve(f.doc(), f.ref, keyOf(a), {{5, 0.0f, 0.0f, {}}, {1, 0.0f, 0.0f, {}}}).ok());
  // Duplicate: the copy has the curve (one undo step); the plain note's copy has none.
  auto dup = ops::duplicateNotes(f.doc(), f.store.ids(), f.ref, std::vector<NoteKey>{keyOf(a), keyOf(b)}, 8 * kPpq);
  commit(f.store, dup);
  REQUIRE(dup->notes.size() == 2);
  const NoteCurves* copied = findNoteCurves(f.clip(), dup->notes[0].id);
  REQUIRE(copied != nullptr);
  CHECK(copied->pitch == bend);
  CHECK(findNoteCurves(f.clip(), dup->notes[1].id) == nullptr);
  CHECK(noteCurvesOf(f.clip()).size() == 2);
  // Paste: the clipboard's notes and their curves (matched by the ids the notes still carry).
  const std::vector<Note> clipboard = {a, b};
  const std::vector<NoteCurves> clipCurves = {NoteCurves{a.id, bend}};
  std::vector<Note> pasted = clipboard;
  for (Note& n : pasted) n.start += 16 * kPpq;
  auto paste = ops::addNotes(f.doc(), f.store.ids(), f.ref, pasted, clipCurves);
  commit(f.store, paste);
  REQUIRE(findNoteCurves(f.clip(), paste->notes[0].id) != nullptr);
  CHECK(findNoteCurves(f.clip(), paste->notes[0].id)->pitch == bend);
  CHECK(noteCurvesOf(f.clip()).size() == 3);
  // Converting keeps the id, so the curve stays.
  commit(f.store, ops::convertNotes(f.doc(), f.ref, std::vector<NoteKey>{keyOf(a)}, PitchKind::Cents));
  CHECK(findNoteCurves(f.clip(), a.id) != nullptr);
  // Moving / resizing too (the key changes, the id does not).
  commit(f.store, ops::moveNotes(f.doc(), f.ref, std::vector<NoteKey>{keyOf(a)}, 5));
  CHECK(findNoteCurves(f.clip(), a.id)->pitch == bend);
  // Delete: the curve goes with its note.
  commit(f.store, ops::deleteNotes(f.doc(), f.ref, std::vector<NoteKey>{NoteKey{5, a.id}, dup->notes[0]}));
  CHECK(findNoteCurves(f.clip(), a.id) == nullptr);
  CHECK(noteCurvesOf(f.clip()).size() == 1);
  f.roundTrips();
  commit(f.store, ops::deleteNotes(f.doc(), f.ref, std::vector<NoteKey>{paste->notes[0]}));
  CHECK(f.clip().curves == nullptr);
}

TEST_CASE("clip copies and splits carry curves re-keyed to the copied notes") {
  JiFix f;
  IdSource& ids = f.store.ids();
  const Note left = f.add(kBar - kPpq, PitchId::fromDegree(0));
  const Note curved = f.add(kBar + 2 * kPpq, PitchId::fromDegree(7));
  const std::vector<Breakpoint> bend = {{0, 100.0f, 0.0f, {}}, {1000, 0.0f, 0.0f, {}}};
  REQUIRE(f.store.dispatch(SetNoteCurve{f.ref, keyOf(curved), bend}).ok());
  REQUIRE(f.store.dispatch(SetNoteCurve{f.ref, keyOf(left), bend}).ok());
  auto clipsOf = [&]() -> const std::vector<ClipSlot>& { return f.track(f.bass).clips; };
  SUBCASE("duplicate") {
    const ClipRef sel[] = {f.ref};
    commit(f.store, ops::duplicateClips(f.doc(), ids, sel));
    const NoteClip& src = *clipsOf()[0].clip;
    const NoteClip& cp = *clipsOf()[1].clip;
    REQUIRE(noteCurvesOf(cp).size() == 2);
    for (const NoteCurves& nc : noteCurvesOf(cp)) {
      CHECK(nc.pitch == bend);
      CHECK(cp.notes.findById(nc.note) != nullptr);
      CHECK(src.notes.findById(nc.note) == nullptr);
    }
    f.roundTrips();
  }
  SUBCASE("split at the bar line") {
    const ClipRef sel[] = {f.ref};
    const Ticks cuts[] = {kBar};
    commit(f.store, ops::splitClips(f.doc(), ids, sel, cuts));
    const NoteClip& l = *clipsOf()[0].clip;
    const NoteClip& r = *clipsOf()[1].clip;
    REQUIRE(noteCurvesOf(l).size() == 1);
    CHECK(noteCurvesOf(l)[0].note == left.id);
    REQUIRE(noteCurvesOf(r).size() == 1);
    CHECK(noteCurvesOf(r)[0].note == curved.id);
    f.roundTrips();
  }
}

TEST_CASE("ji file: the rich project's groups, warps and written identities survive save -> load") {
  // tests/document/fixtures/full is the byte-for-byte check (test_serialize.cpp); here: the model round trip of a
  // group with every warp field and a written identity of each kind.
  JiFix f;
  IdSource& ids = f.store.ids();
  const int64_t t[] = {4, 5, 6, 7};
  auto made = ops::proportionNotes(f.doc(), ids, f.ref, 0, kPpq, PitchId::fromCents(-350.5), t);
  commit(f.store, made);
  commit(f.store, ops::setJiWarps(f.doc(), f.ref, made->jiGroups[0],
                                  {JiWarp{0, 7, GlideShape{GlideShape::Bezier, 0.1f, -1.0f, 0.9f, 2.0f}, false},
                                   JiWarp{100, 50, GlideShape{GlideShape::LogLinear}, true}, JiWarp{kMaxTicks, 0, GlideShape{}, false}}));
  REQUIRE(f.store.dispatch(SetNotesWritten{f.ref,
                                           {NoteWritten{made->notes[1].id, PitchId::fromDegree(-3, 1)},
                                            NoteWritten{made->notes[2].id, PitchId::fromCents(12.5)},
                                            NoteWritten{made->notes[3].id, PitchId::fromRatio(Rational{7, 4})}}})
              .ok());
  f.roundTrips();
}
