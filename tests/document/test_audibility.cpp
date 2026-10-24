// effectiveAudible / computeAudibility (T3.10): Ableton's mute and solo rules as one pure function of the project,
// checked as a table over a tree with nested groups and two returns, plus the mixer edit operations.

#include <doctest/doctest.h>

#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "pq/document/audibility.h"
#include "pq/document/edit_ops.h"
#include "pq/document/store.h"

using namespace pq::doc;

namespace {

//   A
//   G1 = [B, G2 = [C]]
//   D
//   returns R1, R2; the master M
struct Tree {
  DocumentStore store;
  std::map<std::string, TrackId> id;
  Tree() {
    auto run = [&](const ops::EditResult& e) {
      REQUIRE(e.ok());
      REQUIRE(store.dispatch(e->command).ok());
      return e->tracks.at(0);
    };
    auto mk = [&](TrackKind k, TrackId after, const char* name) {
      return run(ops::createTrack(*store.current(), store.ids(), k, after, name));
    };
    id["A"] = mk(TrackKind::Midi, {}, "A");
    id["B"] = mk(TrackKind::Midi, id["A"], "B");
    id["C"] = mk(TrackKind::Midi, id["B"], "C");
    id["D"] = mk(TrackKind::Midi, id["C"], "D");
    id["G2"] = run(ops::groupTracks(*store.current(), store.ids(), std::vector{id["C"]}, "G2"));
    id["G1"] = run(ops::groupTracks(*store.current(), store.ids(), std::vector{id["B"], id["G2"]}, "G1"));
    id["R1"] = mk(TrackKind::Return, {}, "R1");
    id["R2"] = mk(TrackKind::Return, id["R1"], "R2");
    id["M"] = store.current()->master->id;
  }
  // "A:s G2:m D:off" — s = solo, m = mute (activator off), off = deactivated.
  void apply(const std::string& flags) {
    std::istringstream in(flags);
    std::string tok;
    while (in >> tok) {
      const auto colon = tok.find(':');
      REQUIRE(colon != std::string::npos);
      const TrackId t = id.at(tok.substr(0, colon));
      const std::string f = tok.substr(colon + 1);
      if (f == "s") REQUIRE(store.dispatch(SetTrackSolo{t, true}).ok());
      else if (f == "m") REQUIRE(store.dispatch(SetTrackMute{t, true}).ok());
      else if (f == "off") REQUIRE(store.dispatch(SetTrackActive{t, false}).ok());
      else FAIL("unknown flag " << f);
    }
  }
  // Names whose gate is open, in the tree's order.
  std::string open(bool sends) const {
    const AudibilityMap m = computeAudibility(*store.current());
    std::string out;
    forEachTrack(*store.current(), [&](const Track& t, const Track*) {
      const TrackAudibility* a = m.find(t.id);
      REQUIRE(a != nullptr);
      if (sends ? a->sends : a->output) out += (out.empty() ? "" : " ") + (t.kind == TrackKind::Master ? std::string("M") : t.name);
    });
    return out;
  }
};

struct Row {
  const char* flags;
  const char* output;  // tracks whose output stage is open
  const char* sends;   // tracks whose sends are open
};

}  // namespace

TEST_CASE("effectiveAudible: Ableton mute / solo table (nested groups, returns, master)") {
  // Tree order: A G1 B G2 C D R1 R2 M.
  const Row rows[] = {
      // 1. Nothing set: everything open (the master has no sends).
      {"", "A G1 B G2 C D R1 R2 M", "A G1 B G2 C D R1 R2"},
      // 2. The activator mutes the track: output and sends.
      {"A:m", "G1 B G2 C D R1 R2 M", "G1 B G2 C D R1 R2"},
      // 3. A muted group closes only its own gate (its members are silent through it, their gates stay open).
      {"G1:m", "A B G2 C D R1 R2 M", "A B G2 C D R1 R2"},
      // 4. Solo one track: the other main tracks close (sends too); the returns stay open (Solo in Place).
      {"A:s", "A R1 R2 M", "A R1 R2"},
      // 5. A member solo keeps its parent group audible.
      {"B:s", "G1 B R1 R2 M", "G1 B R1 R2"},
      // 6. ... through every level of nesting.
      {"C:s", "G1 G2 C R1 R2 M", "G1 G2 C R1 R2"},
      // 7. A group solo solos its members (all levels).
      {"G1:s", "G1 B G2 C R1 R2 M", "G1 B G2 C R1 R2"},
      // 8. A nested group solo: its members and its parents, not its siblings.
      {"G2:s", "G1 G2 C R1 R2 M", "G1 G2 C R1 R2"},
      // 9. Additive solo of two top-level tracks.
      {"A:s D:s", "A D R1 R2 M", "A D R1 R2"},
      // 10. Two members of one group.
      {"B:s C:s", "G1 B G2 C R1 R2 M", "G1 B G2 C R1 R2"},
      // 11. A soloed muted track stays silent (the activator wins); solo still closes the others.
      {"A:s A:m", "R1 R2 M", "R1 R2"},
      // 12. Group solo with a muted member: the member stays silent.
      {"G1:s B:m", "G1 G2 C R1 R2 M", "G1 G2 C R1 R2"},
      // 13. Solo a return: the main tracks' outputs close, their sends stay open (the return is heard with what feeds
      //     it); the other return closes.
      {"R1:s", "R1 M", "A G1 B G2 C D R1 R2"},
      // 14. Solo a return and a track: the track (and its sends) plus the soloed return.
      {"R1:s A:s", "A R1 M", "A R1 R2"},
      // 15. Both returns soloed.
      {"R1:s R2:s", "R1 R2 M", "A G1 B G2 C D R1 R2"},
      // 16. A muted return.
      {"R1:m", "A G1 B G2 C D R2 M", "A G1 B G2 C D R2"},
      // 17. A soloed muted return: silent, and it still takes the solo from the other return.
      {"R1:s R1:m", "M", "A G1 B G2 C D R2"},
      // 18. A deactivated track is closed like a muted one.
      {"D:off", "A G1 B G2 C R1 R2 M", "A G1 B G2 C R1 R2"},
      // 19. Track solo with a muted return: the other return stays open.
      {"A:s R2:m", "A R1 M", "A R1"},
      // 20. Member solo under a muted parent: the parent's own gate stays closed (muted), the soloed chain is open.
      {"C:s G1:m", "G2 C R1 R2 M", "G2 C R1 R2"},
      // 21. A group and one of its members soloed: the whole group.
      {"G1:s C:s", "G1 B G2 C R1 R2 M", "G1 B G2 C R1 R2"},
      // 22. A top-level solo next to a group solo.
      {"D:s G2:s", "G1 G2 C D R1 R2 M", "G1 G2 C D R1 R2"},
      // 23. Everything muted: only the master.
      {"A:m G1:m B:m G2:m C:m D:m R1:m R2:m", "M", ""},
      // 24. Solo on a deactivated group: closed, but its members are still soloed through it.
      {"G2:s G2:off", "G1 C R1 R2 M", "G1 C R1 R2"},
  };
  int n = 0;
  for (const Row& r : rows) {
    ++n;
    CAPTURE(n);
    CAPTURE(r.flags);
    Tree t;
    t.apply(r.flags);
    CHECK(t.open(false) == r.output);
    CHECK(t.open(true) == r.sends);
    // effectiveAudible is the output gate.
    for (const auto& [name, id] : t.id) {
      const AudibilityMap m = computeAudibility(*t.store.current());
      CHECK(effectiveAudible(*t.store.current(), id) == m.find(id)->output);
    }
  }
  CHECK(n >= 20);
}

TEST_CASE("effectiveAudible: unknown ids, the empty project, solo flags summary") {
  DocumentStore store;
  CHECK(effectiveAudible(*store.current(), store.current()->master->id));
  CHECK_FALSE(effectiveAudible(*store.current(), TrackId{987654}));
  const AudibilityMap m = computeAudibility(*store.current());
  CHECK(m.tracks().size() == 1);
  CHECK_FALSE(m.anySolo());
  Tree t;
  t.apply("R2:s");
  const AudibilityMap m2 = computeAudibility(*t.store.current());
  CHECK(m2.anySolo());
  CHECK(m2.anyReturnSolo());
  CHECK_FALSE(m2.anyMainSolo());
}

TEST_CASE("mixer ops: activator, exclusive / additive solo and arm, a return's Pre/Post") {
  Tree t;
  DocumentStore& s = t.store;
  auto commit = [&](const ops::EditResult& e) {
    REQUIRE(e.ok());
    const size_t h = s.history().cursor();
    REQUIRE(s.dispatch(e->command).ok());
    CHECK(s.history().cursor() == h + 1);  // one undo step
    return s.lastChanges();
  };
  auto trk = [&](const char* n) { return *findTrackPtr(*s.current(), t.id.at(n)); };

  // Activator = mute: a Param edit (no recompile, Q15).
  const std::vector<TrackId> ab{t.id["A"], t.id["B"]};
  ChangeSet c = commit(ops::toggleTracksMute(*s.current(), ab));
  CHECK(c.paramOnly());
  CHECK(trk("A").mixer.mute);
  CHECK(trk("B").mixer.mute);
  REQUIRE(s.dispatch(SetTrackMute{t.id["A"], false}).ok());
  commit(ops::toggleTracksMute(*s.current(), ab));  // one is on → mute all
  CHECK(trk("A").mixer.mute);
  commit(ops::toggleTracksMute(*s.current(), ab));  // all off → back on
  CHECK_FALSE(trk("A").mixer.mute);
  CHECK_FALSE(trk("B").mixer.mute);
  CHECK_FALSE(ops::setTracksMute(*s.current(), std::vector{t.id["M"]}, true).ok());

  // Exclusive solo: soloing D unsolos A; Ctrl (additive) adds; a click on a soloed track unsolos only it.
  c = commit(ops::soloTrack(*s.current(), t.id["A"], false));
  CHECK(c.paramOnly());
  CHECK(trk("A").mixer.solo);
  commit(ops::soloTrack(*s.current(), t.id["D"], false));
  CHECK_FALSE(trk("A").mixer.solo);
  CHECK(trk("D").mixer.solo);
  commit(ops::soloTrack(*s.current(), t.id["R1"], true));
  CHECK(trk("D").mixer.solo);
  CHECK(trk("R1").mixer.solo);
  commit(ops::soloTrack(*s.current(), t.id["D"], false));
  CHECK_FALSE(trk("D").mixer.solo);
  CHECK(trk("R1").mixer.solo);
  CHECK_FALSE(ops::soloTrack(*s.current(), t.id["M"], false).ok());

  // Exclusive arm over MIDI tracks; groups cannot be armed.
  commit(ops::armTrack(*s.current(), t.id["A"], false));
  commit(ops::armTrack(*s.current(), t.id["C"], false));
  CHECK_FALSE(trk("A").mixer.arm);
  CHECK(trk("C").mixer.arm);
  commit(ops::armTrack(*s.current(), t.id["D"], true));
  CHECK(trk("C").mixer.arm);
  CHECK(trk("D").mixer.arm);
  CHECK_FALSE(ops::armTrack(*s.current(), t.id["G1"], false).ok());

  // A return's Pre/Post: every other sender's send to it; structural (the tap point moves).
  CHECK_FALSE(ops::returnSendsPreFader(*s.current(), t.id["R1"]));
  c = commit(ops::setReturnSendsPreFader(*s.current(), t.id["R1"], true));
  CHECK(c.structural());
  CHECK(ops::returnSendsPreFader(*s.current(), t.id["R1"]));
  CHECK_FALSE(ops::returnSendsPreFader(*s.current(), t.id["R2"]));
  REQUIRE(trk("R2").mixer.sends.size() == 1);  // the other return sends too
  CHECK(trk("R2").mixer.sends[0].preFader);
  CHECK(trk("R1").mixer.sends.empty());  // never to itself
  commit(ops::setReturnSendsPreFader(*s.current(), t.id["R1"], false));
  CHECK_FALSE(ops::returnSendsPreFader(*s.current(), t.id["R1"]));
  CHECK(trk("A").mixer.sends.empty());  // back to the implicit default: nothing stored
  CHECK_FALSE(ops::setReturnSendsPreFader(*s.current(), t.id["A"], true).ok());
}
