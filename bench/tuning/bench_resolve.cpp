// resolveHz cost per pitch kind, plus checked Rational multiplication.
// resolveHz runs per note-on and whenever a tuning change re-resolves held notes (block rate), so its per-call cost
// bounds how many voices can be retuned per block.

#include <cstdint>
#include <random>
#include <vector>

#include "pq/tuning/tuning.h"
#include "pq_bench.h"

using namespace pq::tuning;

namespace {

constexpr size_t kCount = 4096;

void benchKind(const char* name, const std::vector<PitchId>& ids, const Tuning& t, const ResolveContext& ctx) {
  std::vector<float> out(ids.size());
  const auto s = pq::bench::measure(
      [&] {
        for (size_t i = 0; i < ids.size(); ++i) out[i] = resolveHz(ids[i], t, ctx);
        pq::bench::doNotOptimize(out[ids.size() / 2]);
      },
      ids.size());
  pq::bench::report(name, s, "ns/pitch");
}

}  // namespace

int main() {
  std::mt19937_64 rng(1);
  const Tuning t31 = *Tuning::make(*Edo::make(31), kDefaultReferenceHz);
  const uint32_t chord[] = {4, 5, 6, 7, 9, 11, 13, 15};
  ResolveContext ctx;
  ctx.anchorHz = 220.0;
  ctx.harmonic = {55.0, chord, 8, false};

  std::vector<PitchId> degree, ratio, cents, harmonic, mixed;
  for (size_t i = 0; i < kCount; ++i) {
    degree.push_back(PitchId::fromDegree(static_cast<int32_t>(rng() % 31), static_cast<int32_t>(rng() % 5) - 2));
    ratio.push_back(PitchId::fromRatio(Rational(static_cast<int64_t>(rng() % 64 + 1), static_cast<int64_t>(rng() % 64 + 1)),
                                       (i & 1) ? AnchorKind::Note : AnchorKind::Reference, 0));
    cents.push_back(PitchId::fromCents(static_cast<double>(rng() % 480000) / 100.0 - 2400.0));
    harmonic.push_back(PitchId::fromHarmonic(0, static_cast<uint32_t>(rng() % 8)));
  }
  for (size_t i = 0; i < kCount; ++i) {
    const std::vector<PitchId>* src[] = {&degree, &ratio, &cents, &harmonic};
    mixed.push_back((*src[rng() % 4])[i]);
  }

  benchKind("resolveHz/degree(31edo)", degree, t31, ctx);
  benchKind("resolveHz/ratio", ratio, t31, ctx);
  benchKind("resolveHz/cents", cents, t31, ctx);
  benchKind("resolveHz/harmonic", harmonic, t31, ctx);
  benchKind("resolveHz/mixed(random kinds)", mixed, t31, ctx);

  // Checked Rational multiply (gcd cross-cancellation + overflow-checked 64×64): the cost of one macro-mode interval
  // step on the composition (SPEC §6.2).
  std::vector<Rational> steps;
  const Rational pool[] = {Rational(3, 2), Rational(2, 3), Rational(5, 4), Rational(4, 5), Rational(7, 4), Rational(4, 7)};
  for (size_t i = 0; i < kCount; ++i) steps.push_back(pool[rng() % 6]);
  const auto s = pq::bench::measure(
      [&] {
        Rational acc{1};
        for (const Rational& r : steps) {
          const auto m = mul(acc, r);
          acc = m ? *m : Rational{1};
        }
        pq::bench::doNotOptimize(acc);
      },
      steps.size());
  pq::bench::report("Rational/mul(checked)", s, "ns/op");
  return 0;
}
