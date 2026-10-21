// Grid lines for one viewport, snapping and quantize (T3.3). Target: lines for one 4K-wide viewport < 20 µs.

#include <cstdio>
#include <random>
#include <vector>

#include "pq/core/cpu.h"
#include "pq/core/grid.h"
#include "pq/core/time.h"
#include "pq_bench.h"

namespace {

constexpr double kViewportPx = 3840.0;

// Iterates every line of a viewport starting at `from`, consuming tick + strength + ruler numbers.
int64_t viewport(const pq::Grid& g, pq::Ticks from, pq::Ticks span, int64_t& lines) {
  int64_t acc = 0;
  pq::GridLine l;
  auto c = g.lines(from, from + span);
  while (c.next(l)) {
    acc += l.tick + static_cast<int64_t>(l.strength);
    ++lines;
  }
  return acc;
}

}  // namespace

int main() {
  std::printf("CPU: %s\n", pq::cpuFeatures().brand.c_str());
  const pq::TimeSigChange changes[] = {{0, {4, 4}}, {16, {7, 8}}, {32, {5, 4}}, {64, {3, 4}}, {128, {4, 4}}};
  const auto sigR = pq::TimeSigMap::create(changes);
  if (!sigR.ok()) return 1;
  const pq::TimeSigMap& sigs = *sigR;

  struct Case {
    const char* name;
    pq::GridSpec spec;
    double pxPerQuarter;
  };
  pq::GridSpec narrowest;
  narrowest.level = pq::kGridLevelMin;  // 3 px cells: the densest adaptive grid
  pq::GridSpec narrowest7 = narrowest;
  narrowest7.tuplet = 7;
  pq::GridSpec medium;
  pq::GridSpec fixed64;
  fixed64.mode = pq::GridMode::Fixed;
  fixed64.fixedExp = 6;
  const Case cases[] = {
      {"grid/viewport 4K narrowest (1/64 @ 12 px/q)", narrowest, 48.0},   // 1/64 = 3 px → 1280 lines
      {"grid/viewport 4K narrowest 7-tuplet", narrowest7, 48.0},
      {"grid/viewport 4K medium", medium, 48.0},
      {"grid/viewport 4K fixed 1/64 zoomed out", fixed64, 4.0},            // 1/64 = 0.25 px → 15360 lines
      {"grid/viewport 4K narrowest zoomed out (bars)", narrowest, 0.05},
  };

  std::mt19937_64 rng(3);
  constexpr int kViews = 64;
  std::vector<pq::Ticks> starts(kViews);
  for (auto& s : starts) s = static_cast<pq::Ticks>(rng() % (200 * pq::kTicksPerWhole));

  for (const Case& c : cases) {
    const double pxPerTick = c.pxPerQuarter / static_cast<double>(pq::kPpq);
    const pq::Grid g(c.spec, sigs, pxPerTick);
    const auto span = static_cast<pq::Ticks>(kViewportPx / pxPerTick);
    int64_t lines = 0;
    int64_t acc = 0;
    for (const auto s : starts) acc += viewport(g, s, span, lines);
    auto st = pq::bench::measure([&] {
      int64_t n = 0;
      for (const auto s : starts) acc += viewport(g, s, span, n);
      pq::bench::doNotOptimize(acc);
    }, kViews);
    pq::bench::report(c.name, st, "ns/viewport");
    std::printf("      %.0f lines/viewport, %.2f ns/line\n", static_cast<double>(lines) / kViews,
                st.medianNs / (static_cast<double>(lines) / kViews));
  }

  // Snap / relative snap / quantize per call (a drag frame does a handful of these).
  constexpr std::size_t kQ = 4096;
  std::vector<pq::Ticks> ticks(kQ);
  for (auto& t : ticks) t = static_cast<pq::Ticks>(rng() % (200 * pq::kTicksPerWhole));
  pq::GridSpec g7spec;
  g7spec.mode = pq::GridMode::Fixed;
  g7spec.tuplet = 7;
  const pq::Grid g16(g7spec, sigs, 1.0);
  const pq::Grid gad(medium, sigs, 48.0 / pq::kPpq);
  int64_t acc = 0;
  auto s1 = pq::bench::measure([&] {
    for (const auto t : ticks) acc += g16.snap(t);
    pq::bench::doNotOptimize(acc);
  }, kQ);
  pq::bench::report("grid/snap fixed 1/16 7-tuplet", s1, "ns/call");
  auto s2 = pq::bench::measure([&] {
    for (const auto t : ticks) acc += gad.snap(t);
    pq::bench::doNotOptimize(acc);
  }, kQ);
  pq::bench::report("grid/snap adaptive", s2, "ns/call");
  auto s3 = pq::bench::measure([&] {
    for (const auto t : ticks) acc += g16.snapDelta(t, 12345);
    pq::bench::doNotOptimize(acc);
  }, kQ);
  pq::bench::report("grid/snapDelta", s3, "ns/call");
  auto s4 = pq::bench::measure([&] {
    for (const auto t : ticks) {
      const pq::TickRange q = pq::quantize(g16, t, t + 5000, 75, pq::QuantizeMode::StartEnd);
      acc += q.start + q.end;
    }
    pq::bench::doNotOptimize(acc);
  }, kQ);
  pq::bench::report("grid/quantize startEnd", s4, "ns/note");
  auto s5 = pq::bench::measure([&] {
    for (const auto t : ticks) {
      char buf[pq::kRulerLabelMaxChars];
      acc += static_cast<int64_t>(pq::formatRulerPosition(pq::rulerPosition(sigs, t), buf));
    }
    pq::bench::doNotOptimize(acc);
  }, kQ);
  pq::bench::report("grid/rulerPosition + format", s5, "ns/label");
  return 0;
}
