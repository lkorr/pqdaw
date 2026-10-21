// Edit operations (T3.6): tracks, clips, notes, automation. Each operation is checked on the before/after snapshots
// and as exactly one undoable history entry (edit_test::commit).

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

#include "edit_helpers.h"

using namespace pq::doc;
using namespace edit_test;
using doctest_fixture::Fixture;
using doctest_fixture::makeClip;
using doctest_fixture::makeNote;
using pq::tuning::PitchId;

namespace {

std::vector<TrackId> childIds(const Track& t) {
  std::vector<TrackId> v;
  for (const TrackPtr& c : t.children) v.push_back(c->id);
  return v;
}
std::vector<TrackId> topIds(const Project& p) {
  std::vector<TrackId> v;
  for (const TrackPtr& c : p.tracks) v.push_back(c->id);
  return v;
}

}  // namespace

// ---- Tracks --------------------------------------------------------------------------------------------------------

TEST_CASE("edit ops: create tracks after the selection, Live-style names") {
  Fixture f;
  auto e = ops::createTrack(f.doc(), f.store.ids(), TrackKind::Midi, f.lead);
  commit(f.store, e);
  REQUIRE(e->tracks.size() == 1);
  CHECK(f.doc().tracks[1]->id == e->tracks[0]);
  CHECK(f.doc().tracks[1]->name == "4 MIDI");  // Lead, Kick, Bass + 1

  auto g = ops::createTrack(f.doc(), f.store.ids(), TrackKind::Midi, f.kick, "Snare");
  commit(f.store, g);
  CHECK(childIds(f.track(f.drums)) == std::vector<TrackId>{f.kick, g->tracks[0]});

  auto r = ops::createTrack(f.doc(), f.store.ids(), TrackKind::Return, f.lead);
  commit(f.store, r);
  REQUIRE(f.doc().returns.size() == 2);
  CHECK(f.doc().returns[1]->name == "B Return");

  auto gr = ops::createTrack(f.doc(), f.store.ids(), TrackKind::Group);
  commit(f.store, gr);
  CHECK(f.doc().tracks.back()->kind == TrackKind::Group);
  CHECK(f.doc().tracks.back()->name == "2 Group");

  CHECK_FALSE(ops::createTrack(f.doc(), f.store.ids(), TrackKind::Master).ok());
  CHECK_FALSE(ops::createTrack(f.doc(), f.store.ids(), TrackKind::Midi, TrackId{9999}).ok());
}

TEST_CASE("edit ops: delete, rename, recolour, (de)activate tracks") {
  Fixture f;
  const TrackId sel[] = {f.kick, f.drums};
  auto d = ops::deleteTracks(f.doc(), sel);
  REQUIRE(d.ok());
  CHECK(d->batch().commands.size() == 1);  // the kick goes with its group
  commit(f.store, d);
  CHECK(topIds(f.doc()) == std::vector<TrackId>{f.lead, f.bass});

  const TrackId m[] = {f.master};
  CHECK_FALSE(ops::deleteTracks(f.doc(), m).ok());

  commit(f.store, ops::renameTrack(f.doc(), f.lead, "Melody"));
  CHECK(f.track(f.lead).name == "Melody");

  const TrackId two[] = {f.lead, f.bass};
  commit(f.store, ops::setTracksColour(f.doc(), two, Colour{0x123456}));
  CHECK(f.track(f.lead).colour == Colour{0x123456});
  CHECK(f.track(f.bass).colour == Colour{0x123456});

  commit(f.store, ops::toggleTracksActive(f.doc(), two));  // both active → deactivate
  CHECK_FALSE(f.track(f.lead).active);
  CHECK_FALSE(f.track(f.bass).active);
  const TrackId one[] = {f.lead};
  commit(f.store, ops::setTracksActive(f.doc(), one, true));
  commit(f.store, ops::toggleTracksActive(f.doc(), two));  // one active → deactivate all
  CHECK_FALSE(f.track(f.lead).active);
  commit(f.store, ops::toggleTracksActive(f.doc(), two));  // none active → activate all
  CHECK(f.track(f.lead).active);
  CHECK(f.track(f.bass).active);
}

TEST_CASE("edit ops: reorder tracks into and out of groups") {
  Fixture f;
  const TrackId sel[] = {f.bass, f.lead};  // any order: they keep tree order
  commit(f.store, ops::moveTracks(f.doc(), sel, f.drums, f.kick));
  CHECK(childIds(f.track(f.drums)) == std::vector<TrackId>{f.lead, f.bass, f.kick});
  CHECK(topIds(f.doc()) == std::vector<TrackId>{f.drums});

  const TrackId k[] = {f.kick};
  commit(f.store, ops::moveTracks(f.doc(), k, TrackId{}, f.drums));
  CHECK(topIds(f.doc()) == std::vector<TrackId>{f.kick, f.drums});

  const TrackId l[] = {f.lead};
  commit(f.store, ops::moveTracks(f.doc(), l, TrackId{}));  // to the end of the top level
  CHECK(topIds(f.doc()) == std::vector<TrackId>{f.kick, f.drums, f.lead});

  const TrackId bad[] = {f.drums};
  auto intoItself = ops::moveTracks(f.doc(), bad, f.drums);
  REQUIRE(intoItself.ok());  // the builder defers the cycle check to MoveTrack
  CHECK_FALSE(f.store.dispatch(intoItself->command).ok());
  const TrackId ret[] = {f.reverb};
  CHECK_FALSE(ops::moveTracks(f.doc(), ret, f.drums).ok());
}

TEST_CASE("edit ops: Ctrl+G groups across parents (nested), ungroup restores") {
  Fixture f;
  const TrackId sel[] = {f.bass, f.kick, f.drums};  // kick is inside the selected drums: it stays there
  auto g = ops::groupTracks(f.doc(), f.store.ids(), sel);
  commit(f.store, g);
  const TrackId gid = g->tracks[0];
  CHECK(topIds(f.doc()) == std::vector<TrackId>{f.lead, gid});
  CHECK(childIds(f.track(gid)) == std::vector<TrackId>{f.drums, f.bass});
  CHECK(childIds(f.track(f.drums)) == std::vector<TrackId>{f.kick});

  // Nested: group the kick (depth 3) with the lead (top level).
  const TrackId sel2[] = {f.kick, f.lead};
  auto g2 = ops::groupTracks(f.doc(), f.store.ids(), sel2, "Mix");
  commit(f.store, g2);
  CHECK(f.track(g2->tracks[0]).name == "Mix");
  CHECK(childIds(f.track(g2->tracks[0])) == std::vector<TrackId>{f.lead, f.kick});
  CHECK(f.track(f.drums).children.empty());

  const TrackId ug[] = {g2->tracks[0], gid};
  auto u = ops::ungroupTracks(f.doc(), ug);
  commit(f.store, u);
  CHECK(topIds(f.doc()) == std::vector<TrackId>{f.lead, f.kick, f.drums, f.bass});
  const TrackId notGroup[] = {f.lead};
  CHECK_FALSE(ops::ungroupTracks(f.doc(), notGroup).ok());
}

TEST_CASE("edit ops: fold is view state, outside history") {
  Fixture f;
  const size_t h = f.store.history().cursor();
  ops::setFolded(f.store.view(), f.drums, true);
  CHECK(f.store.view().tracks.at(f.drums).folded);
  CHECK_FALSE(ops::toggleFolded(f.store.view(), f.drums));
  CHECK(f.store.history().cursor() == h);
}

// ---- Clips ---------------------------------------------------------------------------------------------------------

TEST_CASE("edit ops: insert empty clip (Ctrl+Shift+M) overwrites the range") {
  Fixture f;
  const auto before = played(f.track(f.lead));
  auto e = ops::insertEmptyClip(f.doc(), f.store.ids(), f.lead, kBar, 2 * kBar, "New");
  commit(f.store, e);
  const Track& t = f.track(f.lead);
  REQUIRE(t.clips.size() == 4);  // A [0,1) · New [1,2) · A' [2,4) · B
  const NoteClip& n = f.clip(f.lead, e->clips[0].clip);
  CHECK(n.start == kBar);
  CHECK(n.length == kBar);
  CHECK(n.notes.empty());
  CHECK(n.loop == ClipLoop{true, 0, kBar});
  // Everything A played outside [1, 2) bars still plays at the same time (notes cut by the new clip edge).
  std::vector<Occ> expect;
  for (Occ o : before) {
    if (o.start >= kBar && o.start < 2 * kBar) continue;
    if (o.start < kBar) o.length = std::min(o.length, kBar - o.start);
    expect.push_back(o);
  }
  CHECK(played(t) == expect);
  CHECK(idsUnique(f.doc()));
  CHECK_FALSE(ops::insertEmptyClip(f.doc(), f.store.ids(), f.drums, 0, kBar).ok());  // not a MIDI track
}

TEST_CASE("edit ops: move and copy clips across tracks") {
  Fixture f;
  const auto leadBefore = played(f.track(f.lead));
  const ops::ClipMove mv[] = {{{f.lead, f.clipB}, f.bass, 2 * kBar}};
  commit(f.store, ops::moveClips(f.doc(), f.store.ids(), mv));
  CHECK(findClipIndex(f.track(f.lead), f.clipB) < 0);
  const NoteClip& b = f.clip(f.bass, f.clipB);
  CHECK(b.start == 2 * kBar);
  CHECK(b.notes.size() == 3);

  // Alt-drag copy back onto the lead over clip A's second half: A is trimmed, the copy has fresh ids.
  const ops::ClipMove cp[] = {{{f.bass, f.clipB}, f.lead, 3 * kBar}};
  auto c = ops::moveClips(f.doc(), f.store.ids(), cp, {.copy = true});
  commit(f.store, c);
  CHECK(f.clip(f.bass, f.clipB).start == 2 * kBar);  // original stays
  const NoteClip& copy = f.clip(f.lead, c->clips[0].clip);
  CHECK(copy.id != f.clipB);
  CHECK(copy.start == 3 * kBar);
  CHECK(f.clip(f.lead, f.clipA).length == 3 * kBar);
  CHECK(idsUnique(f.doc()));
  std::vector<Occ> expect;
  for (Occ o : leadBefore) {
    if (o.start >= 3 * kBar) continue;  // A's last bar is overwritten, B moved away
    o.length = std::min(o.length, 3 * kBar - o.start);
    expect.push_back(o);
  }
  for (Occ o : played(f.track(f.bass))) {
    o.start += kBar;
    expect.push_back(o);
  }
  CHECK(played(f.track(f.lead)) == sorted(expect));
}

TEST_CASE("edit ops: moving clips with automation following (lock envelopes off) is exact") {
  Fixture f;
  const AutomationLane before = laneOf(f.doc(), f.lead, f.leadVolLane);
  const ClipRef a[] = {{f.lead, f.clipA}};
  commit(f.store, ops::moveClipsBy(f.doc(), f.store.ids(), a, kBar / 2, {.moveAutomation = true}));
  CHECK(f.clip(f.lead, f.clipA).start == kBar / 2);
  const AutomationLane& after = laneOf(f.doc(), f.lead, f.leadVolLane);
  // Clip A spans [0, 4 bars): the envelope over it moves by half a bar; before it holds the old value at 0.
  for (Ticks t = -kBar; t < 6 * kBar; t += 997) {
    const double want = t < kBar / 2 ? automationValueAt(before.points, 0)
                                     : automationValueAt(before.points, t - kBar / 2);
    CHECK(automationValueAt(after.points, t) == doctest::Approx(want).epsilon(1e-5));
  }
  CHECK(idsUnique(f.doc()));
}

TEST_CASE("edit ops: resize clip edges keeps notes at their absolute time") {
  Fixture f;
  const auto before = played(f.track(f.lead));
  const ClipRef a[] = {{f.lead, f.clipA}};
  commit(f.store, ops::resizeClips(f.doc(), f.store.ids(), a, ops::ClipEdge::Start, kBar));
  const NoteClip& c = f.clip(f.lead, f.clipA);
  CHECK(c.start == kBar);
  CHECK(c.length == 3 * kBar);
  CHECK(c.contentStart == kBar);
  std::vector<Occ> expect;
  for (const Occ& o : before)
    if (o.start >= kBar) expect.push_back(o);
  CHECK(played(f.track(f.lead)) == expect);

  // Growing it back restores the original playback (notes were kept, hidden).
  commit(f.store, ops::resizeClips(f.doc(), f.store.ids(), a, ops::ClipEdge::Start, -kBar));
  CHECK(played(f.track(f.lead)) == before);

  // Growing the end over clip B trims B; shrinking past the start clamps to one tick.
  commit(f.store, ops::resizeClips(f.doc(), f.store.ids(), a, ops::ClipEdge::End, 4 * kBar + kBar / 2));
  CHECK(clipEnd(f.clip(f.lead, f.clipA)) == 8 * kBar + kBar / 2);
  CHECK(f.clip(f.lead, f.clipB).start == 8 * kBar + kBar / 2);
  commit(f.store, ops::resizeClips(f.doc(), f.store.ids(), a, ops::ClipEdge::End, -100 * kBar));
  CHECK(f.clip(f.lead, f.clipA).length == 1);
}

TEST_CASE("edit ops: resizing a looped clip's left edge unrolls the loop backwards") {
  Fixture f;
  const ClipRef b{f.lead, f.clipB};
  // B: 8 bars, length 4 bars, loop over its first beat... start marker inside the loop.
  Batch setup{"setup", {SetClipBounds{b, 8 * kBar, 4 * kBar, pq::kPpq / 2},
                        SetClipLoop{b, ClipLoop{true, 0, pq::kPpq * 2}}}};
  REQUIRE(f.store.dispatch(setup).ok());
  const auto before = played(f.track(f.lead));
  const ClipRef sel[] = {b};
  commit(f.store, ops::resizeClips(f.doc(), f.store.ids(), sel, ops::ClipEdge::Start, -3 * pq::kPpq));
  // The part that played before plays the same; the new part plays the loop's content leading into it.
  const auto after = played(f.track(f.lead));
  std::vector<Occ> tail;
  for (const Occ& o : after)
    if (o.start >= 8 * kBar) tail.push_back(o);
  std::vector<Occ> beforeB;
  for (const Occ& o : before)
    if (o.start >= 8 * kBar) beforeB.push_back(o);
  CHECK(tail == beforeB);
  const NoteClip& c = f.clip(f.lead, f.clipB);
  CHECK(c.contentStart >= 0);
  CHECK(c.contentStart < pq::kPpq * 2);
}

TEST_CASE("edit ops: split (Ctrl+E) keeps absolute time and identity, plain and looped") {
  Fixture f;
  // Make B looped (loop = its first two beats, played four times over 2 bars).
  const ClipRef b{f.lead, f.clipB};
  Batch setup{"setup", {SetClipBounds{b, 8 * kBar, 2 * kBar, 0}, SetClipLoop{b, ClipLoop{true, 0, 2 * pq::kPpq}}}};
  REQUIRE(f.store.dispatch(setup).ok());
  const auto before = played(f.track(f.lead));
  const std::vector<uint32_t> idsBefore = allNoteIds(f.doc());

  const ClipRef sel[] = {{f.lead, f.clipA}, b};
  const Ticks cuts[] = {kBar + kBar / 3, 3 * kBar, 8 * kBar + 5 * pq::kPpq / 2};
  auto e = ops::splitClips(f.doc(), f.store.ids(), sel, cuts);
  commit(f.store, e);
  CHECK(f.track(f.lead).clips.size() == 5);
  CHECK(e->clips.size() == 5);
  // The plain clip's pieces split its notes (same ids, cut where the piece ends); the looped clip's right piece
  // plays the same occurrences from a copy.
  std::vector<Occ> expect;
  for (Occ o : before) {
    for (Ticks c : cuts)
      if (o.start < c && o.start + o.length > c) o.length = c - o.start;
    expect.push_back(o);
  }
  CHECK(played(f.track(f.lead)) == sorted(expect));
  const std::vector<uint32_t> idsAfter = allNoteIds(f.doc());
  CHECK(std::includes(idsAfter.begin(), idsAfter.end(), idsBefore.begin(), idsBefore.end()));
  CHECK(idsAfter.size() == idsBefore.size() + 3);  // B's three notes copied once
  CHECK(idsUnique(f.doc()));
  // Cuts outside every clip change nothing.
  const Ticks none[] = {100 * kBar};
  auto n = ops::splitClips(f.doc(), f.store.ids(), sel, none);
  REQUIRE(n.ok());
  CHECK(n->empty());
}

TEST_CASE("edit ops: duplicate clips (Ctrl+D) after the selection span") {
  Fixture f;
  const ClipRef sel[] = {{f.lead, f.clipB}, {f.kick, f.clipK}};  // span [0, 9 bars)
  auto e = ops::duplicateClips(f.doc(), f.store.ids(), sel);
  commit(f.store, e);
  REQUIRE(e->clips.size() == 2);
  const NoteClip& b2 = f.clip(f.lead, e->clips[0].clip);
  const NoteClip& k2 = f.clip(f.kick, e->clips[1].clip);
  CHECK(b2.start == 17 * kBar);
  CHECK(k2.start == 9 * kBar);
  CHECK(b2.notes.size() == 3);
  CHECK(k2.notes.size() == 4);
  CHECK(idsUnique(f.doc()));
}

TEST_CASE("edit ops: consolidate (Ctrl+J) unrolls loops, keeps absolute time and identity") {
  Fixture f;
  const ClipRef b{f.lead, f.clipB};
  // B looped over its first beat for 1 bar: its first note plays 4 times.
  REQUIRE(f.store.dispatch(SetClipLoop{b, ClipLoop{true, 0, pq::kPpq}}).ok());
  const auto before = played(f.track(f.lead));
  const ClipRef sel[] = {{f.lead, f.clipA}, b};
  auto e = ops::consolidateClips(f.doc(), f.store.ids(), sel);
  commit(f.store, e);
  const Track& t = f.track(f.lead);
  REQUIRE(t.clips.size() == 1);
  const NoteClip& c = *t.clips[0].clip;
  CHECK(c.start == 0);
  CHECK(c.length == 9 * kBar);
  CHECK(played(t) == before);
  // A's playing notes keep their ids; B's first note keeps its id once and has 3 fresh copies.
  CHECK(c.notes.findById(f.notesA[0].id));
  CHECK(c.notes.size() == 64 + 4);
  CHECK(idsUnique(f.doc()));
}

TEST_CASE("edit ops: consolidate deactivated clips and refuse mixed tunings") {
  Fixture f;
  const ClipRef b{f.lead, f.clipB};
  REQUIRE(f.store.dispatch(SetClipActive{b, false}).ok());
  const auto before = played(f.track(f.lead));
  const ClipRef sel[] = {{f.lead, f.clipA}, b};
  commit(f.store, ops::consolidateClips(f.doc(), f.store.ids(), sel));
  CHECK(played(f.track(f.lead)) == before);  // B's notes come in disabled
  CHECK(f.track(f.lead).clips[0].clip->active);

  REQUIRE(f.store.undo());
  REQUIRE(f.store.dispatch(SetClipTuning{b, TuningAssignment::mainTuning(f.tMos)}).ok());
  CHECK_FALSE(ops::consolidateClips(f.doc(), f.store.ids(), sel).ok());
}

TEST_CASE("edit ops: delete clips, loop selection (Ctrl+L)") {
  Fixture f;
  const ClipRef sel[] = {{f.lead, f.clipA}, {f.lead, f.clipA}};
  commit(f.store, ops::deleteClips(f.doc(), sel));
  CHECK(f.track(f.lead).clips.size() == 1);

  commit(f.store, ops::loopClipToSelection(f.doc(), {f.lead, f.clipB}, pq::kPpq, 3 * pq::kPpq));
  CHECK(f.clip(f.lead, f.clipB).loop == ClipLoop{true, pq::kPpq, 3 * pq::kPpq});
  commit(f.store, ops::loopArrangementToSelection(f.doc(), 2 * kBar, 6 * kBar));
  CHECK(f.doc().loop == LoopRegion{2 * kBar, 6 * kBar, true});
  CHECK_FALSE(ops::loopArrangementToSelection(f.doc(), 6 * kBar, 2 * kBar).ok());
}

// ---- Notes ---------------------------------------------------------------------------------------------------------

TEST_CASE("edit ops: add, delete, move, resize, duplicate notes") {
  Fixture f;
  const ClipRef b{f.lead, f.clipB};
  Note n;
  n.start = 3 * pq::kPpq;
  n.length = pq::kPpq;
  n.pitch = PitchId::fromDegree(7);
  auto a = ops::addNotes(f.doc(), f.store.ids(), b, std::span<const Note>(&n, 1));
  commit(f.store, a);
  REQUIRE(a->notes.size() == 1);
  CHECK(f.clip(f.lead, f.clipB).notes.size() == 4);

  auto m = ops::moveNotes(f.doc(), b, a->notes, -pq::kPpq / 2, 1);
  commit(f.store, m);
  const Note* moved = f.clip(f.lead, f.clipB).notes.find(m->notes[0]);
  REQUIRE(moved);
  CHECK(moved->start == 3 * pq::kPpq - pq::kPpq / 2);
  CHECK(moved->pitch == PitchId::fromDegree(8));  // one 31-EDO step

  auto r = ops::resizeNotes(f.doc(), b, m->notes, ops::NoteEdge::Start, pq::kPpq / 4);
  commit(f.store, r);
  const Note* rs = f.clip(f.lead, f.clipB).notes.find(r->notes[0]);
  REQUIRE(rs);
  CHECK(rs->start == 3 * pq::kPpq - pq::kPpq / 4);
  CHECK(rs->length == pq::kPpq * 3 / 4);
  auto r2 = ops::resizeNotes(f.doc(), b, r->notes, ops::NoteEdge::End, -10 * pq::kPpq);
  commit(f.store, r2);
  CHECK(f.clip(f.lead, f.clipB).notes.find(r2->notes[0])->length == 1);

  auto d = ops::duplicateNotes(f.doc(), f.store.ids(), b, r2->notes);
  commit(f.store, d);
  CHECK(f.clip(f.lead, f.clipB).notes.size() == 5);
  CHECK(d->notes[0].start == r2->notes[0].start + 1);  // offset = the selection's span (1 tick)

  commit(f.store, ops::deleteNotes(f.doc(), b, d->notes));
  CHECK(f.clip(f.lead, f.clipB).notes.size() == 4);
  const NoteKey missing{0, NoteId{99999}};
  CHECK_FALSE(ops::deleteNotes(f.doc(), b, std::span<const NoteKey>(&missing, 1)).ok());
}

TEST_CASE("edit ops: velocity and chance edits are clamped") {
  Fixture f;
  const ClipRef a{f.lead, f.clipA};
  const NoteKey keys[] = {f.keyA(0), f.keyA(1)};
  commit(f.store, ops::setNotesProperty(f.doc(), a, keys, NoteProperty::Velocity, 200.0f));
  CHECK(f.clip(f.lead, f.clipA).notes.find(keys[0])->velocity == 127.0f);
  commit(f.store, ops::offsetNotesProperty(f.doc(), a, keys, NoteProperty::Velocity, -500.0f));
  CHECK(f.clip(f.lead, f.clipA).notes.find(keys[1])->velocity == 1.0f);
  commit(f.store, ops::setNotesProperty(f.doc(), a, keys, NoteProperty::Chance, 0.25f));
  CHECK(f.clip(f.lead, f.clipA).notes.find(keys[0])->chance == 0.25f);
  commit(f.store, ops::offsetNotesProperty(f.doc(), a, keys, NoteProperty::Chance, 2.0f));
  CHECK(f.clip(f.lead, f.clipA).notes.find(keys[0])->chance == 1.0f);
  commit(f.store, ops::setNotesProperty(f.doc(), a, keys, NoteProperty::Enabled, 0.0f));
  CHECK_FALSE(f.clip(f.lead, f.clipA).notes.find(keys[0])->enabled());
  auto same = ops::setNotesProperty(f.doc(), a, keys, NoteProperty::Enabled, 0.0f);
  REQUIRE(same.ok());
  CHECK(same->empty());
}

TEST_CASE("edit ops: quantize (Ctrl+U) to straight and tuplet grids, amount, all notes") {
  Fixture f;
  const ClipRef b{f.lead, f.clipB};
  Note n[3];
  const Ticks starts[] = {1000, pq::kPpq + 3000, 2 * pq::kPpq - 50};
  for (int i = 0; i < 3; ++i) {
    n[i].start = starts[i];
    n[i].length = 5000;
  }
  auto a = ops::addNotes(f.doc(), f.store.ids(), b, n);
  commit(f.store, a);
  const pq::TimeSigMap sigs;
  const pq::Grid g16(pq::GridSpec{pq::GridMode::Fixed, 0, 4, 1, true, false}, sigs, 0.0);
  auto q = ops::quantizeNotes(f.doc(), b, a->notes, g16);
  commit(f.store, q);
  const NoteSeq& seq = f.clip(f.lead, f.clipB).notes;
  CHECK(seq.find(q->notes[0])->start == 0);
  CHECK(seq.find(q->notes[1])->start == pq::kPpq);  // 3000 past a line, 3930 before the next
  CHECK(seq.find(q->notes[2])->start == 2 * pq::kPpq);
  for (const NoteKey& k : q->notes) CHECK(seq.find(k)->length == 5000);

  REQUIRE(f.store.undo());
  // 7-tuplet 1/16 at 50 %: each start moves half way to the nearest of 7 lines per quarter (exact ticks).
  const pq::Grid g7(pq::GridSpec{pq::GridMode::Fixed, 0, 4, 7, true, false}, sigs, 0.0);
  auto q7 = ops::quantizeNotes(f.doc(), b, a->notes, g7, 0, 50);
  commit(f.store, q7);
  const NoteSeq& s7 = f.clip(f.lead, f.clipB).notes;
  CHECK(s7.find(q7->notes[0])->start == 1000 + (0 - 1000) / 2);
  CHECK(s7.find(q7->notes[1])->start == pq::kPpq + 3000 + (pq::kPpq + 3960 - (pq::kPpq + 3000)) / 2);

  // Empty selection = every note of the clip.
  REQUIRE(f.store.undo());
  auto all = ops::quantizeNotes(f.doc(), b, {}, g16, 0, 100, pq::QuantizeMode::StartEnd);
  commit(f.store, all);
  for (const Note& x : f.clip(f.lead, f.clipB).notes) {
    CHECK(x.start % (pq::kPpq / 4) == 0);
    CHECK((x.start + x.length) % (pq::kPpq / 4) == 0);
  }
}

TEST_CASE("edit ops: identity conversions (freeze to cents, snap to degree)") {
  Fixture f;
  const ClipRef a{f.lead, f.clipA};  // 31-EDO
  const NoteKey k[] = {f.keyA(5)};    // degree 5
  commit(f.store, ops::convertNotes(f.doc(), a, k, pq::tuning::PitchKind::Cents));
  const Note* c = f.clip(f.lead, f.clipA).notes.find(k[0]);
  REQUIRE(c->pitch.kind() == pq::tuning::PitchKind::Cents);
  CHECK(c->pitch.cents() == doctest::Approx(5 * 1200.0 / 31));
  commit(f.store, ops::convertNotes(f.doc(), a, k, pq::tuning::PitchKind::Degree));
  CHECK(f.clip(f.lead, f.clipA).notes.find(k[0])->pitch == PitchId::fromDegree(5, 0));
}

// ---- Automation ----------------------------------------------------------------------------------------------------

TEST_CASE("edit ops: add, move, delete breakpoints and curve a segment") {
  Fixture f;
  auto a = ops::addBreakpoint(f.doc(), f.store.ids(), f.lead, f.leadVolLane, 3 * kBar, 100.0);
  commit(f.store, a);
  const AutomationLane& l1 = laneOf(f.doc(), f.lead, f.leadVolLane);
  REQUIRE(l1.points.size() == 4);
  CHECK(l1.points.back().value == kMaxVolumeDb);  // clamped

  const BreakpointId mv[] = {f.bp1, a->points[0]};
  commit(f.store, ops::moveBreakpoints(f.doc(), f.lead, f.leadVolLane, mv, kBar / 4, -3.0));
  const AutomationLane& l2 = laneOf(f.doc(), f.lead, f.leadVolLane);
  CHECK(l2.points[1].tick == kBar + kBar / 4);
  CHECK(l2.points[1].value == -3.0f);
  CHECK(l2.points[3].value == 3.0f);

  commit(f.store, ops::setSegmentCurve(f.doc(), f.lead, f.leadVolLane, f.bp0, -4.0f));
  CHECK(laneOf(f.doc(), f.lead, f.leadVolLane).points[0].curve == -1.0f);

  const BreakpointId del[] = {f.bp2};
  commit(f.store, ops::deleteBreakpoints(f.doc(), f.lead, f.leadVolLane, del));
  CHECK(laneOf(f.doc(), f.lead, f.leadVolLane).points.size() == 3);
  CHECK_FALSE(ops::deleteBreakpoints(f.doc(), f.lead, f.leadVolLane, del).ok());
}

TEST_CASE("edit ops: insert a shape over a range keeps the envelope outside exact") {
  Fixture f;
  const AutomationLane before = laneOf(f.doc(), f.lead, f.leadVolLane);
  const Ticks s = kBar / 2, e = kBar + kBar / 2;  // across the curved segment
  for (ops::ShapeKind kind : {ops::ShapeKind::RampUp, ops::ShapeKind::RampDown, ops::ShapeKind::Sine,
                              ops::ShapeKind::Triangle, ops::ShapeKind::Square, ops::ShapeKind::Saw}) {
    CAPTURE(static_cast<int>(kind));
    auto sh = ops::insertShape(f.doc(), f.store.ids(), f.lead, f.leadVolLane, s, e,
                               ops::ShapeParams{kind, -20.0, -10.0, 2});
    commit(f.store, sh);
    const AutomationLane& after = laneOf(f.doc(), f.lead, f.leadVolLane);
    for (Ticks t = -kBar; t < 4 * kBar; t += 1013) {
      if (t >= s && t < e) {
        const double v = automationValueAt(after.points, t);
        CHECK(v >= -20.0 - 1e-4);
        CHECK(v <= -10.0 + 1e-4);
      } else if (t < s) {
        CHECK(automationValueAt(after.points, t) == doctest::Approx(automationValueAt(before.points, t)).epsilon(1e-5));
      } else {
        CHECK(automationValueAt(after.points, t) == doctest::Approx(automationValueAt(before.points, t)).epsilon(1e-5));
      }
    }
    CHECK(automationValueBefore(after.points, s) == doctest::Approx(automationValueBefore(before.points, s)));
    REQUIRE(f.store.undo());
  }
  // Square: high for the first half of each cycle, low after.
  commit(f.store, ops::insertShape(f.doc(), f.store.ids(), f.lead, f.leadVolLane, 0, kBar,
                                   ops::ShapeParams{ops::ShapeKind::Square, -20.0, -10.0, 1}));
  const AutomationLane& sq = laneOf(f.doc(), f.lead, f.leadVolLane);
  CHECK(automationValueAt(sq.points, kBar / 4) == -10.0);
  CHECK(automationValueAt(sq.points, 3 * kBar / 4) == -20.0);
}

TEST_CASE("automation curve: the law is closed under cutting") {
  // A segment cut at x0 is reproduced by curvature c·x0 on the left and c·(1 − x0) on the right.
  const Breakpoint a{0, 1.0f, 0.7f, BreakpointId{1}}, b{1000, 5.0f, 0.0f, BreakpointId{2}};
  const Ticks cut = 300;
  const double vc = automationSegmentValue(a, b, cut);
  const Breakpoint l{0, 1.0f, 0.7f * 0.3f, BreakpointId{1}}, m{cut, static_cast<float>(vc), 0.7f * 0.7f, BreakpointId{3}};
  for (Ticks t = 0; t <= 1000; t += 50) {
    const double whole = automationSegmentValue(a, b, static_cast<double>(t));
    const double parts = t <= cut ? automationSegmentValue(l, m, static_cast<double>(t))
                                  : automationSegmentValue(m, b, static_cast<double>(t));
    CHECK(parts == doctest::Approx(whole).epsilon(1e-6));
  }
  CHECK(automationShape(0.5, 0.0) == 0.5);
  CHECK(automationShape(0.5, 1.0) < 0.5);
  CHECK(automationShape(0.5, -1.0) > 0.5);
}

TEST_CASE("edit ops: Alt-drag copy with automation copies the envelope over the clip") {
  Fixture f;
  // A breakpoint inside B's range [8, 9) bars, so there is something to copy.
  REQUIRE(f.store.dispatch(AddBreakpoints{f.lead, f.leadVolLane, {{8 * kBar + kBar / 2, 3.0f, 0.4f, f.store.ids().breakpoint()}}}).ok());
  const AutomationLane before = laneOf(f.doc(), f.lead, f.leadVolLane);
  const ops::ClipMove cp[] = {{{f.lead, f.clipB}, f.lead, 12 * kBar}};  // copy at [12, 13) bars
  commit(f.store, ops::moveClips(f.doc(), f.store.ids(), cp, {.copy = true, .moveAutomation = true}));
  const AutomationLane& after = laneOf(f.doc(), f.lead, f.leadVolLane);
  for (Ticks t = -kBar; t < 14 * kBar; t += 1013) {
    CAPTURE(t);
    const Ticks src = (t >= 12 * kBar && t < 13 * kBar) ? t - 4 * kBar : t;
    CHECK(automationValueAt(after.points, t) == doctest::Approx(automationValueAt(before.points, src)).epsilon(1e-5));
  }
  CHECK(f.track(f.lead).clips.size() == 3);
  CHECK(idsUnique(f.doc()));
}
