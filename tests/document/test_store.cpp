// DocumentStore, History and structural sharing (T3.4 acceptance: identical-pointer undo/redo, gesture
// coalescing, cancelled gestures leave no history, unchanged subtrees pointer-equal across commits).

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "doc_fixture.h"

using namespace pq::doc;
using doctest_fixture::Fixture;
using doctest_fixture::makeNote;

TEST_CASE("store: apply/undo/redo walk through identical snapshot pointers") {
  Fixture f;
  std::vector<Snapshot> seen{f.store.current()};
  for (int i = 0; i < 20; ++i) {
    REQUIRE(f.store.dispatch(SetTrackVolume{f.lead, -static_cast<float>(i + 1)}).ok());
    seen.push_back(f.store.current());
  }
  for (int i = 20; i > 0; --i) {
    REQUIRE(f.store.undo());
    CHECK(f.store.current() == seen[static_cast<size_t>(i - 1)]);
  }
  CHECK(f.store.history().cursor() == 1);  // the fixture's setup batch remains
  for (int i = 1; i <= 20; ++i) {
    REQUIRE(f.store.redo());
    CHECK(f.store.current() == seen[static_cast<size_t>(i)]);
  }
  CHECK_FALSE(f.store.redo());
  // A new edit after undo drops the redo branch.
  REQUIRE(f.store.undo());
  REQUIRE(f.store.undo());
  REQUIRE(f.store.dispatch(SetTrackPan{f.lead, 0.3f}).ok());
  CHECK_FALSE(f.store.history().canRedo());
  CHECK(f.store.history().entries().size() == 20);  // setup + 18 volumes + pan
  CHECK(f.store.history().undoLabel() == "Set Pan");
}

TEST_CASE("store: a gesture coalesces many dispatches into one undo step") {
  Fixture f;
  const Snapshot base = f.store.current();
  const size_t h = f.store.history().entries().size();
  std::vector<StoreEventKind> events;
  std::vector<bool> paramOnly;
  f.store.subscribe([&](const StoreEvent& e) {
    events.push_back(e.kind);
    paramOnly.push_back(e.changes.paramOnly());
  });

  REQUIRE(f.store.beginGesture().ok());
  CHECK_FALSE(f.store.beginGesture().ok());  // no nesting
  for (int i = 0; i < 60; ++i) REQUIRE(f.store.dispatch(SetTrackVolume{f.lead, -0.1f * static_cast<float>(i + 1)}).ok());
  CHECK(f.store.history().entries().size() == h);  // nothing recorded yet
  CHECK_FALSE(f.store.undo());                     // not during a gesture
  CHECK(f.track(f.lead).mixer.volumeDb == doctest::Approx(-6.0f));
  REQUIRE(f.store.commitGesture().ok());
  const Snapshot end = f.store.current();
  REQUIRE(f.store.history().entries().size() == h + 1);
  CHECK(f.store.history().undoLabel() == "Set Volume");  // the first command's label
  const ChangeSet& cs = f.store.history().entries().back().changes;
  CHECK(cs.paramOnly());
  CHECK(cs.params == std::vector<ParamRef>{ParamRef::track(ParamKind::TrackVolume, f.lead)});
  // 60 live notifications (each param-only, for live engine writes) + 1 commit.
  REQUIRE(events.size() == 61);
  CHECK(events.back() == StoreEventKind::GestureCommit);
  for (size_t i = 0; i < 60; ++i) CHECK(paramOnly[i]);

  REQUIRE(f.store.undo());
  CHECK(f.store.current() == base);
  REQUIRE(f.store.redo());
  CHECK(f.store.current() == end);
}

TEST_CASE("store: a note drag gesture is one sequence-only step") {
  Fixture f;
  const Snapshot base = f.store.current();
  REQUIRE(f.store.beginGesture("Drag Note").ok());
  NoteKey key = f.keyA(42);
  for (int i = 1; i <= 30; ++i) {
    const pq::Ticks start = key.start + 100;
    REQUIRE(f.store.dispatch(MoveNote{{f.lead, f.clipA}, key, start, pq::tuning::PitchId::fromDegree(i % 7)}).ok());
    key.start = start;
  }
  REQUIRE(f.store.commitGesture().ok());
  CHECK(f.store.history().undoLabel() == "Drag Note");
  const ChangeSet& cs = f.store.lastChanges();
  CHECK(cs.sequenceOnly());
  CHECK(cs.notes == std::vector<NoteId>{f.notesA[42].id});
  REQUIRE(f.store.undo());
  CHECK(f.store.current() == base);
}

TEST_CASE("store: a cancelled gesture restores the base snapshot and leaves no history") {
  Fixture f;
  const Snapshot base = f.store.current();
  const size_t h = f.store.history().entries().size();
  bool sawCancel = false;
  f.store.subscribe([&](const StoreEvent& e) {
    if (e.kind == StoreEventKind::GestureCancel) {
      sawCancel = true;
      CHECK(e.after == base);
      CHECK(e.changes.has(kChangeParam));
    }
  });
  REQUIRE(f.store.beginGesture("Knob").ok());
  for (int i = 0; i < 10; ++i) REQUIRE(f.store.dispatch(SetTrackPan{f.lead, 0.05f * static_cast<float>(i + 1)}).ok());
  CHECK(f.store.current() != base);
  REQUIRE(f.store.cancelGesture().ok());
  CHECK(sawCancel);
  CHECK(f.store.current() == base);
  CHECK(f.store.history().entries().size() == h);
  CHECK_FALSE(f.store.history().canRedo());
  CHECK_FALSE(f.store.cancelGesture().ok());
  CHECK_FALSE(f.store.commitGesture().ok());
  // An empty gesture commits nothing.
  REQUIRE(f.store.beginGesture().ok());
  REQUIRE(f.store.commitGesture().ok());
  CHECK(f.store.history().entries().size() == h);
}

TEST_CASE("store: listeners get before/after and can unsubscribe") {
  Fixture f;
  int calls = 0;
  Snapshot lastBefore, lastAfter;
  const ListenerId id = f.store.subscribe([&](const StoreEvent& e) {
    ++calls;
    lastBefore = e.before;
    lastAfter = e.after;
  });
  const Snapshot s0 = f.store.current();
  REQUIRE(f.store.dispatch(RenameTrack{f.bass, "Sub"}).ok());
  CHECK(calls == 1);
  CHECK(lastBefore == s0);
  CHECK(lastAfter == f.store.current());
  REQUIRE(f.store.undo());
  CHECK(calls == 2);
  CHECK(lastAfter == s0);
  f.store.unsubscribe(id);
  REQUIRE(f.store.redo());
  CHECK(calls == 2);
}

TEST_CASE("store: reset loads a document, clears history and continues ids after it") {
  Fixture f;
  const Snapshot doc = f.store.current();
  const uint32_t maxId = maxEntityId(*doc);
  DocumentStore other(doc);
  CHECK(other.ids().peek() == maxId + 1);
  CHECK(other.history().entries().empty());
  CHECK(other.current() == doc);
  DocumentStore third;
  third.reset(doc);
  CHECK(third.ids().peek() >= maxId + 1);
  CHECK_FALSE(third.undo());
}

TEST_CASE("sharing: a one-note edit leaves every other subtree pointer-equal") {
  Fixture f;
  const Snapshot a = f.store.current();
  REQUIRE(f.store.dispatch(MoveNote{{f.lead, f.clipA}, f.keyA(200), f.notesA[200].start + 1,
                                    pq::tuning::PitchId::fromDegree(9)}).ok());
  const Snapshot b = f.store.current();
  REQUIRE(a != b);
  CHECK(a->timeline == b->timeline);
  CHECK(a->tunings == b->tunings);
  CHECK(a->locators == b->locators);
  CHECK(a->master == b->master);
  CHECK(a->returns[0] == b->returns[0]);
  // Lead changed; Drums (with Kick) and Bass did not.
  CHECK(a->tracks[0] != b->tracks[0]);
  CHECK(a->tracks[1] == b->tracks[1]);
  CHECK(a->tracks[2] == b->tracks[2]);
  // Within Lead: clip B and the lanes are shared; clip A changed.
  const Track& la = *a->tracks[0];
  const Track& lb = *b->tracks[0];
  CHECK(la.clips[1].clip == lb.clips[1].clip);
  CHECK(la.lanes[0] == lb.lanes[0]);
  CHECK(la.clips[0].clip != lb.clips[0].clip);
  // Within clip A (300 notes = chunks 128 + 128 + 44), only the chunk holding note 200 was rebuilt.
  const auto& ca = la.clips[0].clip->notes.chunks();
  const auto& cb = lb.clips[0].clip->notes.chunks();
  REQUIRE(ca.size() == cb.size());
  CHECK(ca[0].notes == cb[0].notes);
  CHECK(ca[1].notes != cb[1].notes);
  CHECK(ca[2].notes == cb[2].notes);
  // The old snapshot still holds the old note.
  CHECK(la.clips[0].clip->notes.find(f.keyA(200)) != nullptr);
  CHECK(lb.clips[0].clip->notes.find(f.keyA(200)) == nullptr);
}

TEST_CASE("sharing: a nested edit copies only the path to the root") {
  Fixture f;
  const Snapshot a = f.store.current();
  REQUIRE(f.store.dispatch(SetTrackVolume{f.kick, -4.0f}).ok());
  const Snapshot b = f.store.current();
  CHECK(a->tracks[0] == b->tracks[0]);  // Lead
  CHECK(a->tracks[2] == b->tracks[2]);  // Bass
  CHECK(a->tracks[1] != b->tracks[1]);  // Drums (path)
  const Track& kickA = *a->tracks[1]->children[0];
  const Track& kickB = *b->tracks[1]->children[0];
  CHECK(&kickA != &kickB);
  CHECK(kickA.clips[0].clip == kickB.clips[0].clip);  // the clip is shared
  CHECK(kickB.mixer.volumeDb == -4.0f);
  CHECK(kickA.mixer.volumeDb == 0.0f);
}

TEST_CASE("sharing: tempo and tuning edits touch only their own node") {
  Fixture f;
  const Snapshot a = f.store.current();
  REQUIRE(f.store.dispatch(SetTempoMap{{{0, 100.0, pq::TempoCurve::Step}}}).ok());
  REQUIRE(f.store.dispatch(SetTuningParam{f.tMos, ParamKind::MosGenerator, 701.0}).ok());
  const Snapshot b = f.store.current();
  CHECK(a->timeline != b->timeline);
  CHECK(a->tunings != b->tunings);
  CHECK((*a->tunings)[0] == (*b->tunings)[0]);
  CHECK((*a->tunings)[1] == (*b->tunings)[1]);
  CHECK((*a->tunings)[2] != (*b->tunings)[2]);
  for (size_t i = 0; i < a->tracks.size(); ++i) CHECK(a->tracks[i] == b->tracks[i]);
  CHECK(a->master == b->master);
}

TEST_CASE("view state is outside the undo history") {
  Fixture f;
  REQUIRE(f.store.dispatch(RenameTrack{f.drums, "Perc"}).ok());
  f.store.view().tracks[f.drums].folded = true;
  REQUIRE(f.store.undo());
  CHECK(f.store.view().tracks.at(f.drums).folded);  // fold survives undo (Q15)
  CHECK(f.track(f.drums).name == "Drums");
}
