// Time-selection edits (T3.6): insert / delete / duplicate time shift clips, notes, automation, locators, the loop,
// tempo and time signatures consistently, as one history entry each.

#include <doctest/doctest.h>

#include <algorithm>

#include "edit_helpers.h"

using namespace pq::doc;
using namespace edit_test;
using doctest_fixture::Fixture;

namespace {

// Locators at 1 bar ("Verse", from the fixture), 3 bars, 6 bars; loop [1, 5) bars; a 3/4 change at bar 6; a tempo
// ramp 120 → 90 over bars 1..3 then 150 from bar 5.
void decorate(Fixture& f) {
  Batch b{"setup", {}};
  b.commands.push_back(AddLocator{Locator{f.store.ids().locator(), 3 * kBar, "Chorus"}});
  b.commands.push_back(AddLocator{Locator{f.store.ids().locator(), 6 * kBar, "Bridge"}});
  b.commands.push_back(SetLoop{LoopRegion{kBar, 5 * kBar, true}});
  b.commands.push_back(SetTimeSignatures{{{0, {4, 4}}, {6, {3, 4}}}});
  b.commands.push_back(SetTempoMap{{{0, 120.0, pq::TempoCurve::Step},
                                    {kBar, 120.0, pq::TempoCurve::Linear},
                                    {3 * kBar, 90.0, pq::TempoCurve::Step},
                                    {5 * kBar, 150.0, pq::TempoCurve::Step}}});
  REQUIRE(f.store.dispatch(b).ok());
}

std::vector<Ticks> locatorTicks(const Project& p) {
  std::vector<Ticks> v;
  for (const Locator& l : *p.locators) v.push_back(l.tick);
  return v;
}

// Time map of an edit for checking: where a position before the edit plays afterwards (nullopt = deleted).
struct Map {
  enum Kind { Insert, Delete, Duplicate } kind;
  Ticks a, b, len;
  std::optional<Ticks> operator()(Ticks t) const {
    switch (kind) {
      case Insert: return t >= a ? t + len : t;
      case Delete: return t < a ? std::optional<Ticks>(t) : (t < b ? std::nullopt : std::optional<Ticks>(t - len));
      case Duplicate: return t >= b ? t + len : t;
    }
    return t;
  }
};

// The played notes of a track after a time edit, from those before: shifted, cut where the edit cut a clip
// (`cuts`, in old time), dropped if deleted; plus extra (duplicated) occurrences.
std::vector<Occ> mapPlayed(const std::vector<Occ>& before, const Map& m, std::span<const Ticks> cuts) {
  std::vector<Occ> out;
  for (Occ o : before) {
    for (Ticks c : cuts)
      if (o.start < c && o.start + o.length > c) o.length = c - o.start;
    const auto s = m(o.start);
    if (!s) continue;
    o.start = *s;
    out.push_back(o);
  }
  return sorted(out);
}

void checkEnvelope(const AutomationLane& before, const AutomationLane& after, const Map& m, Ticks gapFrom,
                   Ticks gapTo, double gapValue) {
  for (Ticks t = -kBar; t < 8 * kBar; t += 1009) {
    CAPTURE(t);
    if (t >= gapFrom && t < gapTo) {
      CHECK(automationValueAt(after.points, t) == doctest::Approx(gapValue).epsilon(1e-5));
      continue;
    }
    const auto nt = m(t);
    if (!nt) continue;
    CHECK(automationValueAt(after.points, *nt) == doctest::Approx(automationValueAt(before.points, t)).epsilon(1e-5));
  }
}

}  // namespace

TEST_CASE("time edits: insert time (Ctrl+I) shifts everything after the point, splitting clips across it") {
  Fixture f;
  decorate(f);
  const Ticks at = 2 * kBar + kBar / 4, len = kBar;
  const auto leadBefore = played(f.track(f.lead));
  const auto kickBefore = played(f.track(f.kick));
  const AutomationLane volBefore = laneOf(f.doc(), f.lead, f.leadVolLane);
  const pq::TempoMap tempoBefore = f.doc().timeline->tempoMap;

  auto e = ops::insertTime(f.doc(), f.store.ids(), at, len);
  commit(f.store, e);
  const Map m{Map::Insert, at, at, len};
  const Ticks cuts[] = {at};
  CHECK(played(f.track(f.lead)) == mapPlayed(leadBefore, m, cuts));
  CHECK(played(f.track(f.kick)) == kickBefore);  // ends before the point
  CHECK(f.track(f.lead).clips.size() == 3);       // A split in two, B moved
  CHECK(f.clip(f.lead, f.clipB).start == 9 * kBar);
  // Automation: unchanged before, holds v(at) through the gap, shifted after (exact through the curved segment).
  const AutomationLane& vol = laneOf(f.doc(), f.lead, f.leadVolLane);
  checkEnvelope(volBefore, vol, m, at, at + len, automationValueBefore(volBefore.points, at));
  CHECK(locatorTicks(f.doc()) == std::vector<Ticks>{kBar, 4 * kBar, 7 * kBar});
  CHECK(f.doc().loop == LoopRegion{kBar, 6 * kBar, true});
  // Tempo: the ramp is cut at `at` and resumes after the gap, which holds the tempo at `at`.
  const pq::TempoMap& tm = f.doc().timeline->tempoMap;
  for (Ticks t = 0; t < 8 * kBar; t += 997) {
    CAPTURE(t);
    if (t < at)
      CHECK(tm.bpmAt(t) == doctest::Approx(tempoBefore.bpmAt(t)).epsilon(1e-6));
    else if (t < at + len)
      CHECK(tm.bpmAt(t) == doctest::Approx(tempoBefore.bpmAt(at)).epsilon(1e-6));
    else
      CHECK(tm.bpmAt(t) == doctest::Approx(tempoBefore.bpmAt(t - len)).epsilon(1e-6));
  }
  // The 3/4 change moves from bar 6 to bar 7.
  const auto& sigs = f.doc().timeline->signatures;
  REQUIRE(sigs.size() == 2);
  CHECK(sigs[1].bar == 7);
  CHECK(idsUnique(f.doc()));
}

TEST_CASE("time edits: insert time at 0 moves everything, tempo stays anchored at tick 0") {
  Fixture f;
  decorate(f);
  const auto kickBefore = played(f.track(f.kick));
  commit(f.store, ops::insertTime(f.doc(), f.store.ids(), 0, 2 * kBar));
  const Map m{Map::Insert, 0, 0, 2 * kBar};
  CHECK(played(f.track(f.kick)) == mapPlayed(kickBefore, m, {}));
  const auto& tp = f.doc().timeline->tempo;
  REQUIRE_FALSE(tp.empty());
  CHECK(tp.front().tick == 0);
  CHECK(f.doc().timeline->tempoMap.bpmAt(4 * kBar) == doctest::Approx(105.0));  // the ramp's middle, moved
  CHECK(f.doc().timeline->signatures[1].bar == 8);
}

TEST_CASE("time edits: delete time closes the gap; locators inside are removed") {
  Fixture f;
  decorate(f);
  const Ticks a = kBar + kBar / 2, b = 3 * kBar;  // through clip A and the curved segment
  const auto leadBefore = played(f.track(f.lead));
  const AutomationLane volBefore = laneOf(f.doc(), f.lead, f.leadVolLane);
  auto e = ops::deleteTime(f.doc(), f.store.ids(), a, b);
  commit(f.store, e);
  const Map m{Map::Delete, a, b, b - a};
  const Ticks cuts[] = {a, b};
  CHECK(played(f.track(f.lead)) == mapPlayed(leadBefore, m, cuts));
  const NoteClip& ab = f.clip(f.lead, f.clipB);
  CHECK(ab.start == 8 * kBar - (b - a));
  const AutomationLane& vol = laneOf(f.doc(), f.lead, f.leadVolLane);
  checkEnvelope(volBefore, vol, m, 0, 0, 0.0);
  // At the seam the envelope jumps from v(a−) to v(b).
  CHECK(automationValueBefore(vol.points, a) == doctest::Approx(automationValueBefore(volBefore.points, a)));
  CHECK(automationValueAt(vol.points, a) == doctest::Approx(automationValueAt(volBefore.points, b)));
  CHECK(locatorTicks(f.doc()) == std::vector<Ticks>{kBar, a, 6 * kBar - (b - a)});  // Chorus sat on b
  CHECK(f.doc().loop == LoopRegion{kBar, 5 * kBar - (b - a), true});
  CHECK(idsUnique(f.doc()));
}

TEST_CASE("time edits: delete time over whole clips and the loop") {
  Fixture f;
  decorate(f);
  commit(f.store, ops::deleteTime(f.doc(), f.store.ids(), 0, 5 * kBar));
  CHECK(f.track(f.kick).clips.empty());
  REQUIRE(f.track(f.lead).clips.size() == 1);
  CHECK(f.clip(f.lead, f.clipB).start == 3 * kBar);
  CHECK(f.doc().loop == LoopRegion{0, 4 * kBar, true});  // collapsed: keeps its length at the seam
  CHECK(locatorTicks(f.doc()) == std::vector<Ticks>{kBar});
  CHECK(f.doc().timeline->tempo.front().tick == 0);
  CHECK(f.doc().timeline->tempoMap.bpmAt(0) == doctest::Approx(150.0));
  CHECK(f.doc().timeline->signatures[1].bar == 1);
}

TEST_CASE("time edits: duplicate time (Ctrl+Shift+D) inserts a copy of the range after it") {
  Fixture f;
  decorate(f);
  const Ticks a = kBar / 2, b = kBar + kBar / 4, len = b - a;
  const auto leadBefore = played(f.track(f.lead));
  const auto kickBefore = played(f.track(f.kick));
  const AutomationLane volBefore = laneOf(f.doc(), f.lead, f.leadVolLane);
  auto e = ops::duplicateTime(f.doc(), f.store.ids(), a, b);
  commit(f.store, e);
  const Map m{Map::Duplicate, a, b, len};
  // Originals: cut at b (the clip across b is split), shifted after b; plus the copy of [a, b) at [b, 2b − a).
  auto expect = [&](const std::vector<Occ>& before) {
    const Ticks cuts[] = {b};
    std::vector<Occ> out = mapPlayed(before, m, cuts);
    for (Occ o : before) {
      if (o.start < a || o.start >= b) continue;
      o.length = std::min(o.length, b - o.start);
      o.start += len;
      out.push_back(o);
    }
    return sorted(out);
  };
  CHECK(played(f.track(f.lead)) == expect(leadBefore));
  CHECK(played(f.track(f.kick)) == expect(kickBefore));
  // Automation: v'(t) = v(t) before b, v(t − len) after b (the copied range continues seamlessly).
  const AutomationLane& vol = laneOf(f.doc(), f.lead, f.leadVolLane);
  for (Ticks t = -kBar; t < 8 * kBar; t += 1009) {
    CAPTURE(t);
    const double want = automationValueAt(volBefore.points, t < b ? t : t - len);
    CHECK(automationValueAt(vol.points, t) == doctest::Approx(want).epsilon(1e-5));
  }
  CHECK(locatorTicks(f.doc()) == std::vector<Ticks>{kBar, 3 * kBar + len, 6 * kBar + len});
  CHECK(f.doc().loop == LoopRegion{kBar, 5 * kBar + len, true});
  CHECK(idsUnique(f.doc()));
}

TEST_CASE("time edits: nothing after the edit point changes nothing but the loop/locators it reaches") {
  Fixture f;
  auto e = ops::insertTime(f.doc(), f.store.ids(), 100 * kBar, kBar);
  REQUIRE(e.ok());
  CHECK(e->empty());
  CHECK_FALSE(ops::insertTime(f.doc(), f.store.ids(), 0, 0).ok());
  CHECK_FALSE(ops::deleteTime(f.doc(), f.store.ids(), kBar, kBar).ok());
}

TEST_CASE("time edits: looped clips keep their playback through insert / delete / duplicate time") {
  for (int op = 0; op < 3; ++op) {
    CAPTURE(op);
    Fixture f;
    // B: 3 bars at bar 8, start marker inside a 3-beat loop.
    const ClipRef b{f.lead, f.clipB};
    Batch setup{"setup", {SetClipBounds{b, 8 * kBar, 3 * kBar, pq::kPpq},
                          SetClipLoop{b, ClipLoop{true, 0, 3 * pq::kPpq}}}};
    REQUIRE(f.store.dispatch(setup).ok());
    const auto before = played(f.track(f.lead));
    const Ticks a = 9 * kBar + pq::kPpq / 3, e = 10 * kBar + 5 * pq::kPpq / 7, len = e - a;
    std::vector<Occ> expect;
    if (op == 0) {
      commit(f.store, ops::insertTime(f.doc(), f.store.ids(), a, len));
      const Ticks cuts[] = {a};
      expect = mapPlayed(before, Map{Map::Insert, a, a, len}, cuts);
    } else if (op == 1) {
      commit(f.store, ops::deleteTime(f.doc(), f.store.ids(), a, e));
      const Ticks cuts[] = {a, e};
      expect = mapPlayed(before, Map{Map::Delete, a, e, len}, cuts);
    } else {
      commit(f.store, ops::duplicateTime(f.doc(), f.store.ids(), a, e));
      const Ticks cuts[] = {e};
      expect = mapPlayed(before, Map{Map::Duplicate, a, e, len}, cuts);
      for (Occ o : before)
        if (o.start >= a && o.start < e) {
          o.length = std::min(o.length, e - o.start);
          o.start += len;
          expect.push_back(o);
        }
      expect = sorted(expect);
    }
    CHECK(played(f.track(f.lead)) == expect);
    CHECK(idsUnique(f.doc()));
  }
}

TEST_CASE("time edits: a signature change that leaves the bar grid snaps to the next bar line") {
  Fixture f;
  REQUIRE(f.store.dispatch(SetTimeSignatures{{{0, {4, 4}}, {4, {7, 8}}}}).ok());
  // Insert half a bar at bar 2: the change at 4 bars would land at 4.5 bars, so it snaps to bar 5.
  commit(f.store, ops::insertTime(f.doc(), f.store.ids(), 2 * kBar, kBar / 2));
  const auto& s = f.doc().timeline->signatures;
  REQUIRE(s.size() == 2);
  CHECK(s[1].bar == 5);
  CHECK(s[1].sig == pq::TimeSignature{7, 8});
  // Delete bars 3..5 (holding the change): the 7/8 continues from the seam.
  REQUIRE(f.store.undo());
  commit(f.store, ops::deleteTime(f.doc(), f.store.ids(), 3 * kBar, 5 * kBar));
  const auto& d = f.doc().timeline->signatures;
  REQUIRE(d.size() == 2);
  CHECK(d[1].bar == 3);
  CHECK(d[1].sig == pq::TimeSignature{7, 8});
}
