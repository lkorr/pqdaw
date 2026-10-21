// MOS costs (T2.3). buildMosScale runs whenever the UI or document rebuilds a scale (not on the audio thread); the
// linear table lookup + evaluation is what tuning-as-signal (T2.6) will run at block rate / per sample.

#include <cstdint>
#include <random>
#include <vector>

#include "pq/tuning/mos.h"
#include "pq/tuning/mos_regions.h"
#include "pq/tuning/tuning.h"
#include "pq_bench.h"

using namespace pq::tuning;

namespace {

constexpr size_t kCount = 1024;

std::vector<double> randomGenerators(std::mt19937_64& rng, double lo, double hi) {
  std::uniform_real_distribution<double> d(lo, hi);
  std::vector<double> g(kCount);
  for (double& x : g) x = d(rng);
  return g;
}

void benchBuild(const char* name, int32_t n, const std::vector<double>& gens) {
  const auto s = pq::bench::measure(
      [&] {
        double acc = 0.0;
        for (double g : gens) acc += buildMosScale(g, n, 1200.0).pitches.back();
        pq::bench::doNotOptimize(acc);
      },
      gens.size());
  pq::bench::report(name, s, "ns/scale");
}

void benchLookup(const char* name, int32_t n, const std::vector<double>& gens) {
  const MosLinearTable table(n);
  const auto s = pq::bench::measure(
      [&] {
        int64_t acc = 0;
        for (double g : gens) acc += table.lookup(g, 1200.0).k[n - 1];
        pq::bench::doNotOptimize(acc);
      },
      gens.size());
  pq::bench::report(name, s, "ns/lookup");
}

void benchEvaluate(const char* name, int32_t n, const std::vector<double>& gens) {
  const MosLinearTable table(n);
  std::vector<double> out(static_cast<size_t>(n));
  const auto s = pq::bench::measure(
      [&] {
        for (double g : gens) {
          const MosLinearSpan span = table.lookup(g, 1200.0);
          evaluateLinearForm(span, g, 1200.0, out.data());
        }
        pq::bench::doNotOptimize(out[static_cast<size_t>(n) / 2]);
      },
      gens.size() * static_cast<size_t>(n));
  pq::bench::report(name, s, "ns/degree");
}

}  // namespace

int main() {
  std::mt19937_64 rng(3);
  const std::vector<double> diatonic = randomGenerators(rng, 686.0, 713.0);  // inside the 5L 2s region
  const std::vector<double> anywhere = randomGenerators(rng, 0.0, 1200.0);

  benchBuild("buildMosScale/n=7", 7, diatonic);
  benchBuild("buildMosScale/n=24", 24, anywhere);
  benchLookup("MosLinearTable::lookup/n=7", 7, anywhere);
  benchLookup("MosLinearTable::lookup/n=24", 24, anywhere);
  benchEvaluate("lookup+evaluateLinearForm/n=7", 7, diatonic);
  benchEvaluate("lookup+evaluateLinearForm/n=24", 24, anywhere);

  // Region chart rebuild (the UI does this when size or period changes).
  const auto r = pq::bench::measure([&] { pq::bench::doNotOptimize(buildMosRegions(18, 18, 1200.0, 1).regions.size()); }, 1);
  pq::bench::report("buildMosRegions/maxSize=18", r, "ns/build");

  // Degree → Hz through a MOS tuning (one table read + exp2).
  MosParams p;
  p.generator = 696.578;
  p.n = 7;
  p.octaveStretch = 1.0;
  const Tuning t = *Tuning::make(*Mos::make(p), kDefaultReferenceHz);
  std::vector<PitchId> ids;
  for (size_t i = 0; i < 4096; ++i)
    ids.push_back(PitchId::fromDegree(static_cast<int32_t>(rng() % 7), static_cast<int32_t>(rng() % 5) - 2));
  std::vector<float> hz(ids.size());
  const auto d = pq::bench::measure(
      [&] {
        for (size_t i = 0; i < ids.size(); ++i) hz[i] = resolveHz(ids[i], t, {});
        pq::bench::doNotOptimize(hz[ids.size() / 2]);
      },
      ids.size());
  pq::bench::report("resolveHz/degree(mos 5L2s)", d, "ns/pitch");
  return 0;
}
