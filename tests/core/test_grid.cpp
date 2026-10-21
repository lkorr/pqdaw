// T3.3: grid settings, adaptive/fixed resolution, line iteration, snapping, relative snap, quantize, ruler labels.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <ostream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "pq/core/alloc_trap.h"
#include "pq/core/grid.h"
#include "pq/core/time.h"

using namespace pq;

namespace {

constexpr Ticks W = kTicksPerWhole;  // 110880
constexpr Ticks Q = kPpq;            // 27720
constexpr Ticks S16 = W / 16;        // 6930

GridSpec fixedSpec(int exp, int tuplet = 1) {
  GridSpec s;
  s.mode = GridMode::Fixed;
  s.fixedExp = static_cast<int8_t>(exp);
  s.tuplet = static_cast<uint8_t>(tuplet);
  return s;
}
GridSpec adaptiveSpec(int level, int tuplet = 1) {
  GridSpec s;
  s.level = static_cast<int8_t>(level);
  s.tuplet = static_cast<uint8_t>(tuplet);
  return s;
}

std::vector<GridLine> collect(const Grid& g, Ticks from, Ticks to) {
  std::vector<GridLine> v;
  for (const GridLine& l : g.lines(from, to)) v.push_back(l);
  return v;
}
std::vector<Ticks> ticksOf(const std::vector<GridLine>& v) {
  std::vector<Ticks> t;
  for (const auto& l : v) t.push_back(l.tick);
  return t;
}

std::string label(const RulerPosition& p) {
  char buf[kRulerLabelMaxChars];
  return std::string(buf, formatRulerPosition(p, buf));
}
std::string divName(const GridDivision& d) {
  char buf[24];
  return std::string(buf, formatGridDivision(d, buf));
}

TimeSigMap makeSigs(std::initializer_list<TimeSigChange> changes) {
  auto r = TimeSigMap::create(std::span<const TimeSigChange>(changes.begin(), changes.size()));
  REQUIRE(r.ok());
  return *r;
}

}  // namespace

TEST_CASE("grid: keyboard commands (Ctrl+1..5) and menu names") {
  GridSpec s;  // adaptive Medium, straight, snap on
  CHECK(s.narrowed().level == -1);
  CHECK(s.widened().level == 1);
  GridSpec n = s;
  for (int i = 0; i < 10; ++i) n = n.narrowed();
  CHECK(n.level == kGridLevelMin);
  GridSpec w = s;
  for (int i = 0; i < 10; ++i) w = w.widened();
  CHECK(w.level == kGridLevelMax);
  CHECK(s.tripletToggled().tuplet == 3);
  CHECK(s.tripletToggled().tripletToggled().tuplet == 1);
  GridSpec t7 = s;
  t7.tuplet = 7;
  CHECK(t7.tripletToggled().tuplet == 3);
  CHECK(s.snapToggled().snap == false);
  CHECK(s.snapToggled().snapToggled() == s);
  const GridSpec f = s.modeToggled();
  CHECK(f.mode == GridMode::Fixed);
  CHECK(f.fixedExp == 4);  // remembered fixed value (1/16)
  CHECK(f.narrowed().fixedExp == 5);
  CHECK(f.widened().fixedExp == 3);
  GridSpec fn = f;
  for (int i = 0; i < 20; ++i) fn = fn.narrowed();
  CHECK(fn.fixedExp == kGridFixedExpMax);
  GridSpec fw = f;
  for (int i = 0; i < 20; ++i) fw = fw.widened();
  CHECK(fw.fixedExp == kGridFixedExpMin);
  CHECK(f.modeToggled().mode == GridMode::Adaptive);
  GridSpec off = s;
  off.off = true;
  CHECK(off.narrowed().off);
  CHECK_FALSE(off.modeToggled().off);

  CHECK(std::string_view(adaptiveLevelName(-3)) == "Narrowest");
  CHECK(std::string_view(adaptiveLevelName(0)) == "Medium");
  CHECK(std::string_view(adaptiveLevelName(3)) == "Widest");
  CHECK(std::string_view(fixedValueName(-3)) == "8 Bars");
  CHECK(std::string_view(fixedValueName(0)) == "1/1");
  CHECK(std::string_view(fixedValueName(6)) == "1/64");
  CHECK(adaptiveMinCellPx(-3) == 3.0);
  CHECK(adaptiveMinCellPx(0) == 24.0);
  CHECK(adaptiveMinCellPx(3) == 192.0);

  CHECK(divName({0, 16, 1}) == "1/16");
  CHECK(divName({0, 8, 3}) == "1/8T");
  CHECK(divName({0, 16, 7}) == "1/16 7:4");
  CHECK(divName({1, 0, 1}) == "1 Bar");
  CHECK(divName({4, 0, 3}) == "4 Bars 3:2");
  CHECK(divName({}) == "Off");
  char tiny[3];
  CHECK(formatGridDivision({0, 16, 1}, tiny) == 0);
}

TEST_CASE("grid: adaptive levels across zooms (4/4)") {
  const TimeSigMap sigs;
  struct Row {
    double pxPer16th;  // zoom, as pixels per sixteenth note
    int level;
    int tuplet;
    GridDivision expect;
  };
  // Cells: 1/64 = px/4, 1/32 = px/2, 1/16 = px, 1/8 = 2px, …, 1 bar = 16px. Min cell = 24·2^level px.
  const Row rows[] = {
      {30.0, 0, 1, {0, 16, 1}},  {30.0, -1, 1, {0, 32, 1}}, {30.0, -2, 1, {0, 64, 1}}, {30.0, -3, 1, {0, 64, 1}},
      {30.0, 1, 1, {0, 8, 1}},   {30.0, 2, 1, {0, 4, 1}},   {30.0, 3, 1, {0, 2, 1}},
      {24.001, 0, 1, {0, 16, 1}}, {23.999, 0, 1, {0, 8, 1}},  // either side of the 24 px boundary
      {200.0, 0, 1, {0, 64, 1}}, {200.0, 3, 1, {0, 16, 1}}, // zoomed in past 1/64: finest available
      {1.0, -3, 1, {0, 4, 1}},   {1.0, -2, 1, {0, 2, 1}},   {1.0, -1, 1, {1, 0, 1}}, {1.0, 0, 1, {2, 0, 1}},
      {1.0, 1, 1, {4, 0, 1}},    {1.0, 2, 1, {8, 0, 1}},    {1.0, 3, 1, {16, 0, 1}},
      {0.01, 0, 1, {256, 0, 1}}, {1e-9, 0, 1, {static_cast<int32_t>(kGridMaxBarsPerCell), 0, 1}},
      // Tuplets: the tuplet cell (span·base/n) must be ≥ min px. Triplet 1/16 cell = 2/3 · 1/16.
      {30.0, 0, 3, {0, 8, 3}},   // 1/16T = 20 px < 24 → 1/8T = 40 px
      {36.1, 0, 3, {0, 16, 3}},  // 1/16T = 24.07 px
      {30.1, 0, 5, {0, 16, 5}},  // 1/16 5:4 = 24.08 px
      {30.0, 0, 7, {0, 8, 7}},   // 1/16 7:4 = 17.1 px → 1/8 7:4 = 34.3 px
      {30.0, 0, 11, {0, 8, 11}}, // 1/16 11:8 = 21.8 px → 1/8 11:8 = 43.6 px
  };
  for (const Row& r : rows) {
    const Grid g(adaptiveSpec(r.level, r.tuplet), sigs, r.pxPer16th / static_cast<double>(S16));
    INFO("px/16th=" << r.pxPer16th << " level=" << r.level << " tuplet=" << r.tuplet);
    CHECK(g.divisionAt(0) == r.expect);
  }
  // Bars with a triplet: cell = k · 2 bars / 3 = k · 10.67 px at 16 px/bar; k = 4 gives 42.7 ≥ 24, k = 2 gives 21.3.
  const Grid gb(adaptiveSpec(0, 3), sigs, 1.0 / static_cast<double>(S16));
  CHECK(gb.divisionAt(0) == GridDivision{4, 0, 3});
  // Non-positive / non-finite zoom → coarsest.
  CHECK(Grid(adaptiveSpec(0), sigs, 0.0).divisionAt(0).bars == kGridMaxBarsPerCell);
  CHECK(Grid(adaptiveSpec(0), sigs, -1.0).divisionAt(0).bars == kGridMaxBarsPerCell);
}

TEST_CASE("grid: adaptive skips note values that do not tile the bar (3/4, 6/8, 7/8, 5/12)") {
  const double px = 20.0 / static_cast<double>(Q);  // 20 px per quarter
  // 3/4: 1/4 = 20 px < 24; 1/2 does not tile 3/4 → 1 bar (60 px).
  CHECK(Grid(adaptiveSpec(0), makeSigs({{0, {3, 4}}}), px).divisionAt(0) == GridDivision{1, 0, 1});
  // 6/8: 1/4 tiles (3 per bar) → 1/4 qualifies at 20 px? no (20 < 24) → 1/2 does not tile → 1 bar.
  CHECK(Grid(adaptiveSpec(0), makeSigs({{0, {6, 8}}}), px).divisionAt(0) == GridDivision{1, 0, 1});
  CHECK(Grid(adaptiveSpec(-1), makeSigs({{0, {6, 8}}}), px).divisionAt(0) == GridDivision{0, 4, 1});
  // 7/8: 1/8 = 10 px; 1/4 is longer than the beat and does not tile → bar.
  CHECK(Grid(adaptiveSpec(-1), makeSigs({{0, {7, 8}}}), px).divisionAt(0) == GridDivision{1, 0, 1});
  CHECK(Grid(adaptiveSpec(-2), makeSigs({{0, {7, 8}}}), px).divisionAt(0) == GridDivision{0, 8, 1});
  // 5/12 (beat = 1/12): sub-beat cells are allowed even though they do not tile the bar.
  CHECK(Grid(adaptiveSpec(-3), makeSigs({{0, {5, 12}}}), px).divisionAt(0) == GridDivision{0, 16, 1});
}

TEST_CASE("grid: fixed values in 4/4 and 3/4") {
  const TimeSigMap sigs;
  const TimeSigMap sigs34 = makeSigs({{0, {3, 4}}});
  for (int e = kGridFixedExpMin; e <= kGridFixedExpMax; ++e) {
    const Grid g(fixedSpec(e), sigs, 1.0);  // zoom is irrelevant for fixed grids
    const Grid gz(fixedSpec(e), sigs, 1e-6);
    CHECK(g.divisionAt(0) == gz.divisionAt(0));
    const GridDivision d = g.divisionAt(0);
    INFO("exp=" << e << " -> " << divName(d));
    if (e < 0) {
      CHECK(d == GridDivision{1 << -e, 0, 1});
    } else if (e == 0) {
      CHECK(d == GridDivision{1, 0, 1});  // 1/1 in 4/4 = 1 bar
    } else {
      CHECK(d == GridDivision{0, 1 << e, 1});
    }
    // Lines over 16 bars: cells are exactly the note value (1/64 rounds per Q3).
    const auto lines = ticksOf(collect(g, 0, 16 * W));
    const int64_t perCell = e < 0 ? (int64_t{1} << -e) : 0;
    const std::size_t expectCount = e <= 0 ? static_cast<std::size_t>(16 / (e < 0 ? perCell : 1))
                                           : static_cast<std::size_t>(16 << e);
    REQUIRE(lines.size() == expectCount);
    for (std::size_t k = 0; k < lines.size(); ++k) {
      const Ticks expect = e <= 0 ? static_cast<Ticks>(k) * (e < 0 ? perCell : 1) * W
                                  : (static_cast<Ticks>(2 * k) * W + (int64_t{1} << e)) / (int64_t{2} << e);
      CHECK(lines[k] == expect);
    }
  }
  // Straight 1/64: alternating 1732 / 1733, exact again every 1/32.
  const auto l64 = ticksOf(collect(Grid(fixedSpec(6), sigs, 1.0), 0, Q));
  REQUIRE(l64.size() == 16);
  for (std::size_t k = 1; k < l64.size(); ++k) CHECK(l64[k] - l64[k - 1] == (k % 2 == 1 ? 1733 : 1732));
  // 1/1 and 1/2 in 3/4: 1/1 ≥ bar → 1 bar; 1/2 stays a note value, cut off at the bar line.
  CHECK(Grid(fixedSpec(0), sigs34, 1.0).divisionAt(0) == GridDivision{1, 0, 1});
  CHECK(ticksOf(collect(Grid(fixedSpec(1), sigs34, 1.0), 0, 2 * 3 * Q)) ==
        std::vector<Ticks>{0, 2 * Q, 3 * Q, 5 * Q});
  // 1/1 in 2/4: two bars per whole note.
  CHECK(Grid(fixedSpec(0), makeSigs({{0, {2, 4}}}), 1.0).divisionAt(0) == GridDivision{2, 0, 1});
}

TEST_CASE("grid: every tuplet x note value lands on exact integer ticks (PPQ 27720)") {
  const TimeSigMap sigs;
  for (int n : {3, 5, 7, 9, 11, 15, 21}) {
    for (int e = 0; e <= 6; ++e) {
      const int64_t d = int64_t{1} << e;
      const Grid g(fixedSpec(e, n), sigs, 1.0);
      const GridDivision div = g.divisionAt(0);
      INFO("n=" << n << " 1/" << d << " -> " << divName(div));
      CHECK(div.tuplet == n);
      const Ticks span = tupletGroupTicks(d, n);  // T0.5: base straight cells
      REQUIRE(tupletIsExact(span, n));
      REQUIRE(span * d == tupletBase(n) * W);  // the group itself is exact
      const Ticks cell = span / n;
      // 8 groups' worth of lines (whole bars).
      const Ticks range = ((8 * span + W - 1) / W) * W;
      const auto lines = collect(g, 0, range);
      // Expected: per bar (note cells restart at every bar) or per multi-bar group, k·cell from the group start.
      std::vector<Ticks> expect;
      const Ticks groupLen = e == 0 ? span : W;  // 1/1 → bar cells, a tuplet group spans base bars
      for (Ticks gs = 0; gs < range; gs += groupLen)
        for (Ticks t = gs; t < gs + groupLen && t < range; t += cell) expect.push_back(t);
      CHECK(ticksOf(lines) == expect);
      // Same positions as the T0.5 helper for every group that fits.
      for (std::size_t k = 0; k < lines.size(); ++k) {
        const Ticks gs = (lines[k].tick / groupLen) * groupLen;
        const int64_t idx = (lines[k].tick - gs) / cell;
        CHECK(lines[k].tick == tupletPosition(gs, span, n, idx));
        CHECK((lines[k].tick - gs) % cell == 0);
      }
      // Strength: tuplet-interior lines are Tuplet, group starts are Subdivision/Beat/Bar.
      for (const auto& l : lines) {
        const bool groupStart = l.tickInBar % span == 0 || (e == 0 && (l.tick % span) == 0);
        if (l.tickInBar == 0) {
          CHECK(l.strength == GridStrength::Bar);
        } else if (l.tickInBar % Q == 0) {
          CHECK(l.strength == GridStrength::Beat);
        } else if (groupStart) {
          CHECK(l.strength == GridStrength::Subdivision);
        } else {
          CHECK(l.strength == GridStrength::Tuplet);
        }
      }
    }
  }
}

TEST_CASE("grid: tuplet primes >= 13 round per line without drift") {
  const TimeSigMap sigs;
  const Grid g(fixedSpec(4, 13), sigs, 1.0);  // 13:8 of 1/16 = 13 lines per half note
  const auto lines = ticksOf(collect(g, 0, 4 * W));
  REQUIRE(lines.size() == 4 * 2 * 13);
  for (std::size_t k = 0; k < lines.size(); ++k) {
    const Ticks gs = static_cast<Ticks>(k / 13) * (W / 2);
    CHECK(lines[k] == tupletPosition(gs, W / 2, 13, static_cast<int64_t>(k % 13)));
  }
}

TEST_CASE("grid: time-signature changes mid-range") {
  // 4/4 bars 0–1, 7/8 bars 2–3, 3/4 from bar 4.
  const TimeSigMap sigs = makeSigs({{0, {4, 4}}, {2, {7, 8}}, {4, {3, 4}}});
  const Ticks b2 = 2 * W, b3 = b2 + 7 * W / 8, b4 = b3 + 7 * W / 8, b5 = b4 + 3 * Q;
  const Grid g(fixedSpec(2), sigs, 1.0);  // 1/4
  const auto lines = collect(g, W + 2 * Q, b5 + Q + 1);
  const std::vector<Ticks> expect = {W + 2 * Q, W + 3 * Q,                          // bar 1 (4/4)
                                     b2,        b2 + Q,     b2 + 2 * Q, b2 + 3 * Q,  // bar 2 (7/8), cut at 7/8
                                     b3,        b3 + Q,     b3 + 2 * Q, b3 + 3 * Q,  // bar 3
                                     b4,        b4 + Q,     b4 + 2 * Q,              // bar 4 (3/4)
                                     b5,        b5 + Q};
  CHECK(ticksOf(lines) == expect);
  REQUIRE(lines.size() == expect.size());
  CHECK(lines[0].strength == GridStrength::Beat);
  CHECK(lines[2].strength == GridStrength::Bar);
  CHECK(lines[2].bar == 2);
  CHECK(lines[2].sig == TimeSignature{7, 8});
  CHECK(lines[3].strength == GridStrength::Beat);  // 2/8 is a beat in 7/8
  CHECK(lines[3].tickInBar == Q);
  CHECK(lines[10].bar == 4);
  CHECK(lines[10].sig == TimeSignature{3, 4});
  CHECK(lines[13].bar == 5);
  CHECK(lines[13].strength == GridStrength::Bar);
  CHECK(lines[14].strength == GridStrength::Beat);

  // 1/16 over the 7/8 bars: sub-beat lines are Subdivision, eighths are Beat.
  const auto l16 = collect(Grid(fixedSpec(4), sigs, 1.0), b2, b3);
  REQUIRE(l16.size() == 14);
  for (std::size_t k = 0; k < l16.size(); ++k)
    CHECK(l16[k].strength == (k == 0 ? GridStrength::Bar : k % 2 == 0 ? GridStrength::Beat : GridStrength::Subdivision));

  // Adaptive bars: groups of k bars are counted from each segment's first bar.
  const TimeSigMap sigs2 = makeSigs({{0, {4, 4}}, {6, {3, 4}}});
  const double px = 6.0 / static_cast<double>(W);  // 6 px per 4/4 bar, 4.5 px per 3/4 bar
  const Grid ga(adaptiveSpec(0), sigs2, px);
  CHECK(ga.divisionAt(0) == GridDivision{4, 0, 1});      // 24 px
  CHECK(ga.divisionAt(7 * W) == GridDivision{8, 0, 1});  // 36 px (4 bars = 18 px < 24)
  std::vector<int64_t> bars;
  for (const GridLine& l : ga.lines(0, 6 * W + 40 * 3 * Q)) {
    CHECK(l.strength == GridStrength::Bar);
    CHECK(l.tickInBar == 0);
    bars.push_back(l.bar);
  }
  CHECK(bars == std::vector<int64_t>{0, 4, 6, 14, 22, 30, 38});
  CHECK(ga.lineAtOrBefore(6 * W - 1) == 4 * W);
  CHECK(ga.lineAfter(6 * W - 1) == 6 * W);
  CHECK(ga.lineAfter(6 * W) == 6 * W + 8 * 3 * Q);
}

TEST_CASE("grid: line queries agree with iteration (random, tuplets, signature changes, negative ticks)") {
  const TimeSigMap sigs = makeSigs({{0, {4, 4}}, {3, {7, 8}}, {5, {5, 4}}, {9, {6, 8}}, {12, {11, 16}}});
  std::mt19937_64 rng(42);
  const Ticks lo = -6 * W, hi = 30 * W;
  std::vector<Grid> grids;
  for (int e = kGridFixedExpMin; e <= kGridFixedExpMax; ++e)
    for (int n : {1, 3, 5, 7, 11}) grids.emplace_back(fixedSpec(e, n), sigs, 1.0);
  for (int lv = kGridLevelMin; lv <= kGridLevelMax; ++lv)
    for (double px : {0.05, 0.005, 0.0005, 0.00005}) grids.emplace_back(adaptiveSpec(lv, lv == 0 ? 7 : 1), sigs, px);
  for (const Grid& g : grids) {
    const auto lines = ticksOf(collect(g, lo, hi));
    REQUIRE(lines.size() >= 2);
    for (std::size_t k = 1; k < lines.size(); ++k) REQUIRE(lines[k] > lines[k - 1]);
    // Bar lines are always lines for note cells; spot-check a few positions exactly on lines.
    for (std::size_t k = 0; k + 1 < lines.size(); k += 1 + lines.size() / 50) {
      CHECK(g.lineAtOrBefore(lines[k]) == lines[k]);
      CHECK(g.lineAtOrAfter(lines[k]) == lines[k]);
      CHECK(g.lineAfter(lines[k]) == lines[k + 1]);
      CHECK(g.nearestLine(lines[k]) == lines[k]);
    }
    for (int i = 0; i < 300; ++i) {
      const Ticks t = lines.front() + static_cast<Ticks>(rng() % static_cast<uint64_t>(lines.back() - lines.front()));
      const auto it = std::upper_bound(lines.begin(), lines.end(), t);
      const Ticks before = *(it - 1);
      const Ticks after = before == t ? t : *it;
      const Ticks next = *it;
      INFO("t=" << t);
      CHECK(g.lineAtOrBefore(t) == before);
      CHECK(g.lineAtOrAfter(t) == after);
      CHECK(g.lineAfter(t) == next);
      CHECK(g.cellAt(t) == TickRange{before, next});
      CHECK(g.nearestLine(t) == ((t - before < after - t) ? before : after));
      // A cursor started anywhere yields the same lines.
      auto c = g.lines(t, t + 1);
      GridLine l;
      CHECK(c.next(l) == (after == t));
    }
    // Cursor started mid-range resumes exactly.
    const Ticks mid = lines[lines.size() / 2] + 1;
    const auto tail = ticksOf(collect(g, mid, hi));
    CHECK(tail == std::vector<Ticks>(std::lower_bound(lines.begin(), lines.end(), mid), lines.end()));
  }
}

TEST_CASE("grid: snap, snap toggle and Off") {
  const TimeSigMap sigs;
  GridSpec s = fixedSpec(4);  // 1/16 = 6930
  const Grid g(s, sigs, 1.0);
  CHECK(g.snapping());
  CHECK(g.snap(0) == 0);
  CHECK(g.snap(3464) == 0);
  CHECK(g.snap(3465) == S16);  // tie → later line
  CHECK(g.snap(S16 + 1) == S16);
  CHECK(g.snap(-3466) == -S16);  // before the start (pre-roll)
  CHECK(g.snap(-3465) == 0);
  // 7-tuplet 1/16: 7 lines per quarter = 3960 ticks.
  const Grid g7(fixedSpec(4, 7), sigs, 1.0);
  CHECK(g7.snap(5000) == 3960);
  CHECK(g7.snap(Q - 1) == Q);
  CHECK(g7.snap(Q + 3960 * 3 + 1980) == Q + 3960 * 4);

  const Grid noSnap(s.snapToggled(), sigs, 1.0);
  CHECK_FALSE(noSnap.snapping());
  CHECK(noSnap.enabled());
  CHECK(noSnap.snap(1234) == 1234);
  CHECK(noSnap.snapDelta(100, 5000) == 5000);
  CHECK(noSnap.nearestLine(1234) == 0);  // raw line queries still work
  s.off = true;
  const Grid off(s, sigs, 1.0);
  CHECK_FALSE(off.enabled());
  CHECK_FALSE(off.snapping());
  CHECK(off.snap(1234) == 1234);
  CHECK(off.lineAtOrBefore(1234) == 1234);
  CHECK(off.lineAfter(1234) == 1235);
  CHECK(off.divisionAt(0) == GridDivision{});
  CHECK(collect(off, 0, 100 * W).empty());
  CHECK(quantize(off, 100, 7000, 100, QuantizeMode::StartEnd) == TickRange{100, 7000});
}

TEST_CASE("grid: snapDelta (Ableton relative snap)") {
  const TimeSigMap sigs;
  const Grid g(fixedSpec(4), sigs, 1.0);  // 1/16 = 6930
  // On the grid: moves to grid lines.
  CHECK(g.snapDelta(S16, 3000) == 0);         // less than half a cell: stays
  CHECK(g.snapDelta(S16, 3465) == S16);       // half a cell: next line
  CHECK(g.snapDelta(S16, 10000) == S16);
  CHECK(g.snapDelta(S16, 11000) == 2 * S16);
  CHECK(g.snapDelta(S16, -3466) == -S16);
  CHECK(g.snapDelta(2 * S16, -100000) == -14 * S16);  // 2·S16 − 100000 = −12.43 cells → −12 cells
  // Off the grid: moves by whole cells, keeping the offset (1000) from the line before it.
  const Ticks o = S16 + 1000;
  CHECK(g.snapDelta(o, 3000) == 0);
  CHECK(g.snapDelta(o, 4000) == S16);
  CHECK(g.snapDelta(o, 3 * S16 - 100) == 3 * S16);
  CHECK(g.snapDelta(o, -4000) == -S16);
  CHECK(o + g.snapDelta(o, 5 * S16 + 17) == 6 * S16 + 1000);
  // Off-grid with a 7-tuplet grid: steps of 3960, offset kept exactly.
  const Grid g7(fixedSpec(4, 7), sigs, 1.0);
  const Ticks o7 = Q + 3960 * 2 + 77;
  for (int steps = -10; steps <= 10; ++steps) {
    const Ticks d = g7.snapDelta(o7, steps * 3960 + 500);
    const Ticks moved = o7 + d;
    CHECK((moved - 77) % 3960 == 0);  // still 77 past a 7-tuplet line (no bar cut-offs at 4/4)
  }
  // Across bars with a cut-off cell (7/8 on a 1/4 grid): the anchor is the line before, the result a line + offset.
  const TimeSigMap s78 = makeSigs({{0, {7, 8}}});
  const Grid g78(fixedSpec(2), s78, 1.0);
  const Ticks bar = 7 * W / 8;
  CHECK(bar + 500 + g78.snapDelta(bar + 500, -(Q / 2 + 600)) == 3 * Q + 500);  // last (short) cell of bar 0
}

TEST_CASE("grid: quantize at 0/50/100% and to tuplet grids") {
  const TimeSigMap sigs;
  const Grid g(fixedSpec(4), sigs, 1.0);  // 1/16 = 6930
  // start 100 after a line, end 1000 before one.
  const Ticks s = S16 + 100, e = 4 * S16 - 1000;
  CHECK(quantize(g, s, e, 0, QuantizeMode::StartEnd) == TickRange{s, e});
  CHECK(quantize(g, s, e, 0, QuantizeMode::StartOnly) == TickRange{s, e});
  CHECK(quantize(g, s, e, 100, QuantizeMode::StartOnly) == TickRange{S16, S16 + (e - s)});
  CHECK(quantize(g, s, e, 100, QuantizeMode::StartEnd) == TickRange{S16, 4 * S16});
  CHECK(quantize(g, s, e, 50, QuantizeMode::StartEnd) == TickRange{S16 + 50, 4 * S16 - 500});
  CHECK(quantize(g, s, e, 50, QuantizeMode::StartOnly) == TickRange{S16 + 50, S16 + 50 + (e - s)});
  CHECK(quantize(g, s, e, 150, QuantizeMode::StartEnd) == quantize(g, s, e, 100, QuantizeMode::StartEnd));
  CHECK(quantize(g, s, e, -5, QuantizeMode::StartEnd) == TickRange{s, e});
  // 50% of an odd distance rounds half up.
  CHECK(quantize(g, S16 + 101, S16 + 5000, 50, QuantizeMode::StartOnly).start == S16 + 51);  // −50.5 → −50
  CHECK(quantize(g, S16 - 101, S16 + 5000, 50, QuantizeMode::StartOnly).start == S16 - 50);  // +50.5 → +51
  // A short note whose end would collapse onto its start extends to the next line.
  CHECK(quantize(g, S16 + 100, S16 + 900, 100, QuantizeMode::StartEnd) == TickRange{S16, 2 * S16});
  // Even at partial amounts the note keeps a positive length.
  const TickRange tiny = quantize(g, S16 + 3000, S16 + 3001, 1, QuantizeMode::StartEnd);
  CHECK(tiny.end > tiny.start);
  // Quantize ignores the snap toggle (Ctrl+U with snap off).
  const Grid ns(fixedSpec(4).snapToggled(), sigs, 1.0);
  CHECK(quantize(ns, s, e, 100, QuantizeMode::StartEnd) == TickRange{S16, 4 * S16});

  // Tuplet grids: results are exact tuplet positions.
  for (int n : {3, 5, 7, 11}) {
    const Grid gt(fixedSpec(4, n), sigs, 1.0);
    const Ticks cell = tupletGroupTicks(16, n) / n;
    std::mt19937_64 rng(static_cast<uint64_t>(n));
    for (int i = 0; i < 200; ++i) {
      const Ticks st = static_cast<Ticks>(rng() % static_cast<uint64_t>(8 * W));
      const Ticks en = st + 1 + static_cast<Ticks>(rng() % static_cast<uint64_t>(W));
      const TickRange q = quantize(gt, st, en, 100, QuantizeMode::StartEnd);
      CHECK(q.start % cell == 0);
      CHECK(q.end % cell == 0);
      CHECK(q.end > q.start);
      CHECK(std::abs(q.start - st) <= cell / 2);
      const TickRange h = quantize(gt, st, en, 50, QuantizeMode::StartEnd);
      CHECK(std::abs(2 * (h.start - st) - (q.start - st)) <= 1);
    }
  }
  // Triplet 1/8 (9240): 10000 → 9240; 14000 → 18480.
  const Grid g3(fixedSpec(3, 3), sigs, 1.0);
  CHECK(quantize(g3, 10000, 14000, 100, QuantizeMode::StartEnd) == TickRange{9240, 18480});
}

TEST_CASE("grid: ruler labels (bars.beats.sixteenths) at any zoom") {
  const TimeSigMap sigs = makeSigs({{0, {4, 4}}, {2, {7, 8}}, {4, {3, 4}}});
  CHECK(label(rulerPosition(sigs, 0)) == "1");
  CHECK(label(rulerPosition(sigs, Q)) == "1.2");
  CHECK(label(rulerPosition(sigs, Q + S16)) == "1.2.2");
  CHECK(label(rulerPosition(sigs, W + 3 * Q + 3 * S16)) == "2.4.4");
  CHECK(rulerPosition(sigs, Q + S16 + 5) == RulerPosition{1, 2, 2, 5});
  // 7/8: beats are eighths, two sixteenths each.
  const Ticks b2 = 2 * W;
  CHECK(label(rulerPosition(sigs, b2 + 3 * Q / 2)) == "3.4");
  CHECK(label(rulerPosition(sigs, b2 + 3 * Q / 2 + S16)) == "3.4.2");
  CHECK(label(rulerPosition(sigs, b2 + 7 * W / 8)) == "4");
  // Before the start: bar −1, no bar 0.
  CHECK(label(rulerPosition(sigs, -W)) == "-1");
  CHECK(label(rulerPosition(sigs, -Q)) == "-1.4");
  char tiny[2];
  CHECK(formatRulerPosition({123, 1, 1, 0}, tiny) == 0);

  // From grid lines (no map lookup) == from the map, at several zooms.
  for (double pxPerQuarter : {2.0, 20.0, 200.0, 2000.0}) {
    const Grid ruler = Grid::adaptive(60.0, sigs, pxPerQuarter / static_cast<double>(Q));
    int count = 0;
    for (const GridLine& l : ruler.lines(-2 * W, 40 * W)) {
      CHECK(rulerPosition(l) == rulerPosition(sigs, l.tick));
      ++count;
    }
    CHECK(count > 0);
  }
  // Zoomed out the ruler shows bar numbers only: 2 px/quarter → 8 px per 4/4 bar → 8-bar labels, but a new
  // signature starts a new count (7/8 at bar 3, 3/4 at bar 5: 6 px/bar → 16-bar labels).
  const Grid farOut = Grid::adaptive(60.0, sigs, 2.0 / static_cast<double>(Q));
  std::vector<std::string> labels;
  for (const GridLine& l : farOut.lines(0, 30 * W)) labels.push_back(label(rulerPosition(l)));
  CHECK(labels == std::vector<std::string>{"1", "3", "5", "21", "37"});
  // Zoomed in: sixteenths.
  const Grid farIn = Grid::adaptive(60.0, sigs, 400.0 / static_cast<double>(Q));
  labels.clear();
  for (const GridLine& l : farIn.lines(0, Q)) labels.push_back(label(rulerPosition(l)));
  CHECK(labels == std::vector<std::string>{"1", "1.1.2", "1.1.3", "1.1.4"});
}

TEST_CASE("rt: grid iteration over 10 hours and snap/quantize allocate nothing") {
  REQUIRE(alloc_trap::installed());
  const TimeSigMap sigs = makeSigs({{0, {4, 4}}, {100, {7, 8}}, {300, {3, 4}}, {5000, {4, 4}}});
  const Ticks tenHours = 10 * 3600 * 2 * Q;  // 120 BPM
  const Grid fine(fixedSpec(6, 1), sigs, 1.0);
  const Grid trip(fixedSpec(5, 3), sigs, 1.0);
  const Grid adapt(adaptiveSpec(0, 7), sigs, 0.001);
  int64_t count = 0;
  Ticks acc = 0;
  CHECK(alloc_trap::countRtViolations([&] {
          for (const Grid* g : {&fine, &trip, &adapt}) {
            GridLine l;
            auto c = g->lines(-W, tenHours);
            while (c.next(l)) {
              ++count;
              acc += l.tick + rulerPosition(l).sixteenth;
            }
            for (const GridLine& r : g->lines(0, 4 * W)) acc += r.tick;
            for (Ticks t = -W; t < tenHours; t += 9876543) {
              acc += g->snap(t) + g->snapDelta(t, 12345) + g->cellAt(t).end;
              const TickRange q = quantize(*g, t, t + 5000, 50, QuantizeMode::StartEnd);
              acc += q.start + q.end + rulerPosition(sigs, t).bar;
              char buf[kRulerLabelMaxChars];
              acc += static_cast<Ticks>(formatRulerPosition(rulerPosition(sigs, t), buf));
              acc += static_cast<Ticks>(formatGridDivision(g->divisionAt(t), buf));
            }
          }
        }) == 0);
  CHECK(count > 1'000'000);  // 1/64 over 10 h alone is ~1.15M lines
  CHECK(acc != 0);
}
