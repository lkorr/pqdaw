// Tempo map and time signature lookups (ns per call). These run per block / per event on the audio thread, not
// per sample, so the budget is "small next to a block", but they should still be O(log n) and cheap.

#include <cstdio>
#include <random>
#include <vector>

#include "pq/core/cpu.h"
#include "pq/core/time.h"
#include "pq_bench.h"

int main() {
  std::printf("CPU: %s\n", pq::cpuFeatures().brand.c_str());

  // ~1000 segments, half of them ramps (a heavily automated multi-hour session).
  std::mt19937_64 rng(7);
  std::vector<pq::TempoPoint> pts;
  pq::Ticks t = 0;
  for (int i = 0; i < 1000; ++i) {
    pts.push_back({t, 40.0 + static_cast<double>(rng() % 200),
                   (i & 1) ? pq::TempoCurve::Linear : pq::TempoCurve::Step});
    t += static_cast<pq::Ticks>(1 + rng() % (64 * 4 * pq::kPpq));
  }
  const auto mapR = pq::TempoMap::create(pts);
  if (!mapR.ok()) return 1;
  const pq::TempoMap& map = *mapR;
  const pq::Ticks end = t;
  const double sr = 48000.0;

  constexpr std::size_t kQ = 4096;
  std::vector<pq::Ticks> ticks(kQ);
  std::vector<pq::SampleTime> samples(kQ);
  for (auto& q : ticks) q = static_cast<pq::Ticks>(rng() % static_cast<uint64_t>(end));
  const pq::SampleTime endS = map.ticksToSamples(end, sr);
  for (auto& s : samples) s = static_cast<pq::SampleTime>(rng() % static_cast<uint64_t>(endS));

  int64_t acc = 0;
  auto s1 = pq::bench::measure([&] {
    for (const auto q : ticks) acc += map.ticksToSamples(q, sr);
    pq::bench::doNotOptimize(acc);
  }, kQ);
  pq::bench::report("tempo/ticksToSamples (1000 segs)", s1, "ns/call");

  double accD = 0;
  auto s2 = pq::bench::measure([&] {
    for (const auto s : samples) accD += map.samplesToTicks(s, sr);
    pq::bench::doNotOptimize(accD);
  }, kQ);
  pq::bench::report("tempo/samplesToTicks (1000 segs)", s2, "ns/call");

  auto s3 = pq::bench::measure([&] {
    for (const auto s : samples) acc += map.firstTickAtOrAfterSample(s, sr);
    pq::bench::doNotOptimize(acc);
  }, kQ);
  pq::bench::report("tempo/firstTickAtOrAfterSample (1000 segs)", s3, "ns/call");

  const pq::TempoMap constant;
  auto s4 = pq::bench::measure([&] {
    for (const auto q : ticks) acc += constant.ticksToSamples(q, sr);
    pq::bench::doNotOptimize(acc);
  }, kQ);
  pq::bench::report("tempo/ticksToSamples (constant)", s4, "ns/call");

  const pq::TimeSigChange changes[] = {{0, {4, 4}}, {16, {7, 8}}, {32, {5, 4}}, {64, {3, 4}}, {128, {4, 4}}};
  const auto sigR = pq::TimeSigMap::create(changes);
  if (!sigR.ok()) return 1;
  auto s5 = pq::bench::measure([&] {
    for (const auto q : ticks) {
      const pq::BarBeatTick b = sigR->ticksToBarBeat(q % (200 * pq::kTicksPerWhole));
      acc += b.bar + b.beat + b.tick;
    }
    pq::bench::doNotOptimize(acc);
  }, kQ);
  pq::bench::report("timesig/ticksToBarBeat (5 changes)", s5, "ns/call");
  return 0;
}
