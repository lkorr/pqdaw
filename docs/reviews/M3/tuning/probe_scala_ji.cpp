// M3 review probe (tuning): Scala parser edge cases, EDO / JI spot checks, float32 Hz precision. Not product code.
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <string>

#include "pq/tuning/edo.h"
#include "pq/tuning/interval_names.h"
#include "pq/tuning/ji.h"
#include "pq/tuning/scala.h"
#include "pq/tuning/tuning.h"

using namespace pq;
using namespace pq::tuning;

static void scl(const char* label, const std::string& text) {
  auto r = parseScl(text, "probe.scl");
  if (!r.ok()) {
    std::printf("[%s] ERROR: %s (line %u)\n", label, r.error().message.c_str(),
                r.error().location.line);
    return;
  }
  std::printf("[%s] %zu pitches:", label, r.value().pitches.size());
  for (const ScalaPitch& p : r.value().pitches) {
    if (p.kind == ScalaPitch::Kind::Ratio)
      std::printf(" R%lld/%lld", static_cast<long long>(p.ratio.num()), static_cast<long long>(p.ratio.den()));
    else if (p.kind == ScalaPitch::Kind::Cents)
      std::printf(" C%.6f", p.cents);
    else
      std::printf(" L%s/%s(%.6f)", p.largeNum.c_str(), p.largeDen.c_str(), p.cents);
  }
  std::printf("\n");
}

static void kbm(const char* label, const std::string& text) {
  auto r = parseKbm(text, "probe.kbm");
  if (!r.ok()) {
    std::printf("[%s] ERROR: %s\n", label, r.error().message.c_str());
    return;
  }
  const KeyboardMapping& k = r.value();
  std::printf("[%s] size=%d first=%d last=%d mid=%d ref=%d hz=%g oct=%d entries=%zu; keys 58..74 ->", label, k.mapSize,
              k.firstKey, k.lastKey, k.middleKey, k.referenceKey, k.referenceHz, k.formalOctaveDegree, k.mapping.size());
  for (int key = 58; key <= 74; ++key) {
    auto d = mapKeyToDegree(k, key);
    if (d) std::printf(" %lld", static_cast<long long>(*d));
    else std::printf(" x");
  }
  std::printf("\n");
}

int main() {
  std::printf("==== Scala .scl ====\n");
  scl("ratio+comment", "desc\n 3\n 3/2 !comment\n 5/4 E\\\n 2/1\n");
  scl("bare integer is a ratio", "desc\n2\n 700\n 2\n");
  scl("negative cents", "desc\n2\n -50.0\n 1200.\n");
  scl("leading period", "desc\n1\n .5\n");
  scl("spaced slash 3 / 2", "desc\n2\n 3 / 2\n 2/1\n");
  scl("exponent 1e3", "desc\n2\n 1e3\n 2/1\n");
  scl("indented comment between pitches", "desc\n2\n 3/2\n   ! indented comment\n 2/1\n");
  scl("blank lines", "desc\n\n2\n\n 3/2\n\n 2/1\n");
  scl("count with trailing text", "desc\n2 notes\n 3/2\n 2/1\n");
  scl("description that looks numeric", "12\n2\n 3/2\n 2/1\n");
  scl("empty description", "!c\n\n2\n 3/2\n 2/1\n");
  scl("zero count", "desc\n0\n");
  scl("comma decimal", "desc\n1\n 100,5\n");
  scl("plus cents", "desc\n1\n +100.0\n");
  scl("huge ratio", "desc\n1\n 123456789012345678901234567/98765432109876543210\n");
  scl("ratio then period 3/2.", "desc\n1\n 3/2.\n");
  scl("CR only", "desc\r2\r 3/2\r 2/1\r");
  {
    auto s = parseScl("desc\n0\n");
    if (s.ok()) {
      auto sc = Scale::make(s.value());
      std::printf("[zero count -> Scale::make] %s\n", sc.ok() ? "ok" : sc.error().message.c_str());
    }
  }

  std::printf("==== Scala .kbm ====\n");
  kbm("12-key std", "12\n0\n127\n60\n69\n440.0\n12\n0\n1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n");
  kbm("white keys only (x)", "12\n0\n127\n60\n69\n440.0\n7\n0\nx\n1\nx\n2\n3\nx\n4\nx\n5\nx\n6\n");
  kbm("reference key unmapped", "12\n0\n127\n60\n61\n440.0\n7\n0\nx\n1\nx\n2\n3\nx\n4\nx\n5\nx\n6\n");
  kbm("ref key outside first..last", "0\n60\n72\n60\n80\n440.0\n0\n");
  kbm("map size 0 (linear)", "0\n0\n127\n60\n69\n440.0\n0\n");
  kbm("short mapping (tail unmapped)", "12\n0\n127\n60\n60\n261.6\n12\n0\n1\n2\n");
  kbm("formal octave 0, size 12", "12\n0\n127\n60\n60\n261.6\n0\n0\n1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n");
  {
    // makeScalaTuning with a reference key outside [first,last]
    auto s = parseScl("d\n12\n100.\n200.\n300.\n400.\n500.\n600.\n700.\n800.\n900.\n1000.\n1100.\n2/1\n");
    auto k = parseKbm("0\n60\n72\n60\n80\n440.0\n0\n");
    auto sc = Scale::make(s.value());
    if (k.ok()) {
      auto t = makeScalaTuning(sc.value(), k.value());
      std::printf("[makeScalaTuning ref outside range] %s\n", t.ok() ? "ok" : t.error().message.c_str());
    }
    auto k2 = parseKbm("12\n0\n127\n60\n69\n440.0\n12\n0\n1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n");
    auto t2 = makeScalaTuning(sc.value(), k2.value());
    auto pid = mapKey(k2.value(), 60, sc.value().size());
    std::printf("[12edo std kbm] key60 = %.9f Hz (expect 261.625565), key 69 = %.9f\n",
                resolveHzDouble(*pid, t2.value(), {}).value_or(-1),
                resolveHzDouble(*mapKey(k2.value(), 69, 12), t2.value(), {}).value_or(-1));
  }

  std::printf("==== EDO ====\n");
  {
    auto e = Edo::make(13, Rational{3});
    Val v = patentVal(e.value());
    std::printf("13-ED3 patent val:");
    for (int i = 0; i < 6; ++i) std::printf(" %d", v.steps[i]);
    std::printf("  (expect 8 13 19 23 28 30 -> computed independently: round(13*log3 p))\n");
    auto t = Tuning::make(e.value(), 100.0);
    for (int deg : {-14, -1, 0, 13, 27})
      std::printf("  13-ED3 degree %d equave 0: %.9f Hz  (exact 100*3^(%d/13) = %.9f)\n", deg,
                  resolveHzDouble(PitchId::fromDegree(deg, 0), t.value(), {}).value_or(-1), deg,
                  100.0 * std::pow(3.0, deg / 13.0));
    std::printf("  13-ED3 degree 2 equave -2: %.9f (exact %.9f)\n",
                resolveHzDouble(PitchId::fromDegree(2, -2), t.value(), {}).value_or(-1), 100.0 * std::pow(3.0, (2 - 26) / 13.0));
    auto ec = Edo::makeCents(0 + 5, -100.0);
    std::printf("  makeCents negative equave: %s\n", ec.ok() ? "accepted" : "rejected");
    auto big = Edo::make(1u << 31);
    std::printf("  Edo::make(2^31) %s; degree INT32_MAX equave INT32_MAX resolves to %s\n", big.ok() ? "ok" : "rejected",
                big.ok() ? (resolveHzDouble(PitchId::fromDegree(INT32_MAX, INT32_MAX), Tuning::make(big.value()).value(), {}) ? "a value" : "nullopt") : "-");
  }

  std::printf("==== float32 Hz precision (SPEC 3.1: ~0.0001 c) ====\n");
  {
    double worst = 0;
    Tuning t;
    for (int s = -120; s <= 120; ++s) {
      const double exact = 261.626 * std::exp2(s / 12.0);
      const float f = resolveHz(PitchId::fromDegree(s, 0), t, {});
      worst = std::fmax(worst, std::fabs(1200.0 * std::log2(static_cast<double>(f) / exact)));
    }
    std::printf("12edo degrees -120..120: worst float32 Hz error %.3g c (bound 0.5ulp = 1200*log2(1+2^-24) = %.3g c)\n",
                worst, 1200.0 * std::log2(1.0 + std::ldexp(1.0, -24)));
  }

  std::printf("==== JI ====\n");
  {
    const Rational a[] = {Rational{1}, Rational{5, 4}, Rational{3, 2}};
    auto vf = virtualFundamental(a);
    std::printf("vf(1 5/4 3/2) = %lld/%lld (expect 1/4)\n", (long long)vf->num(), (long long)vf->den());
    const Rational b[] = {Rational{3, 2}, Rational{4, 3}};
    vf = virtualFundamental(b);
    std::printf("vf(3/2 4/3) = %lld/%lld (expect gcd(3,4)/lcm(2,3) = 1/6)\n", (long long)vf->num(), (long long)vf->den());
    const Rational c2[] = {Rational{9, 4}, Rational{3, 2}};
    vf = virtualFundamental(c2);
    std::printf("vf(9/4 3/2) = %lld/%lld (expect gcd(9,3)/lcm(4,2) = 3/4)\n", (long long)vf->num(), (long long)vf->den());
    std::printf("tenney(3/2)=%.12f expect %.12f; tenney(81/80)=%.12f expect %.12f\n", tenneyHeight(Rational{3, 2}),
                std::log2(6.0), tenneyHeight(Rational{81, 80}), std::log2(6480.0));
    auto r1 = octaveReduce(Rational{1, 3});
    auto r2 = reduceToEquave(Rational{1, 3}, Rational{3});
    auto r3 = octaveReduce(Rational{2});
    auto r4 = reduceToEquave(Rational{7, 1}, Rational{3, 2});
    std::printf("octaveReduce(1/3)=%lld/%lld (4/3); reduce(1/3, 3)=%lld/%lld (1/1); octaveReduce(2)=%lld/%lld (1/1); "
                "reduce(7, 3/2)=%lld/%lld (expect 7*(2/3)^4 = 112/81)\n",
                (long long)r1->num(), (long long)r1->den(), (long long)r2->num(), (long long)r2->den(),
                (long long)r3->num(), (long long)r3->den(), (long long)r4->num(), (long long)r4->den());
    std::printf("equaveIndex(1/3)=%d (expect -2); equaveIndex(2)=%d (1); equaveIndex(1023/512)=%d (0)\n",
                *equaveIndex(Rational{1, 3}), *equaveIndex(Rational{2}), *equaveIndex(Rational{1023, 512}));
    std::printf("primeLimit(1000000007*998244353 / 2) = %llu (expect 1000000007)\n",
                (unsigned long long)primeLimitOf(Rational{static_cast<int64_t>(1000000007LL * 998244353LL), 2}));
    std::printf("largestPrimeFactor(2^61-1) = %llu (prime itself)\n",
                (unsigned long long)largestPrimeFactor((1ull << 61) - 1));
    std::printf("largestPrimeFactor(18446744073709551557) = %llu (largest 64-bit prime)\n",
                (unsigned long long)largestPrimeFactor(18446744073709551557ull));
    std::printf("largestPrimeFactor(4294967291^2) = %llu (expect 4294967291)\n",
                (unsigned long long)largestPrimeFactor(4294967291ull * 4294967291ull));
    std::printf("oddLimit(15/8)=%llu (15), oddLimit(9/7)=%llu (9)\n", (unsigned long long)oddLimitOf(Rational{15, 8}),
                (unsigned long long)oddLimitOf(Rational{9, 7}));
    std::printf("isWithinPrimeLimit(7/4, 5)=%d (0), (25/24,5)=%d (1)\n", isWithinPrimeLimit(Rational{7, 4}, 5),
                isWithinPrimeLimit(Rational{25, 24}, 5));
    auto seg = subharmonicSegment(4, 6);
    std::printf("subharmonicSegment(4,6):");
    for (auto& r : seg) std::printf(" %lld/%lld", (long long)r.num(), (long long)r.den());
    std::printf(" (expect 1/1 6/5 3/2)\n");
    const JiIntervalInfo* i = findJiInterval(Rational{7, 4});
    std::printf("gallery 7/4: %s | %s | %s\n", i ? std::string(i->colourAbbrev).c_str() : "-",
                i ? std::string(i->colourName).c_str() : "-", i ? std::string(i->fjs).c_str() : "-");
    std::printf("trainer name 3/2: '%s', 2/3: '%s', 6/4: '%s'\n", trainerIntervalName(3, 2).c_str(),
                trainerIntervalName(2, 3).c_str(), trainerIntervalName(6, 4).c_str());
  }
  return 0;
}
