// Scala scale tuning (T2.4) and JI utilities (T2.5).
//   - resolveHz for Scale degrees and the .kbm key → degree → Hz path: per note-on / per re-resolve on the engine side.
//   - .scl parsing: file import (UI thread).
//   - Fewest-keypress BFS: rebuilt when the EDO, prime limit or keymap changes (UI thread).
//   - Prime-limit / equave-reduction / gallery lookups: per label or per macro keypress.

#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "pq/tuning/fewest_keypress.h"
#include "pq/tuning/interval_names.h"
#include "pq/tuning/ji.h"
#include "pq/tuning/scala.h"
#include "pq/tuning/scale_tuning.h"
#include "pq/tuning/tuning.h"
#include "pq_bench.h"

using namespace pq::tuning;

namespace {

constexpr size_t kCount = 4096;

// A 43-note just scale (Partch), as .scl text.
std::string partch43() {
  const char* ratios[] = {"81/80",   "33/32",  "21/20", "16/15", "12/11", "11/10", "10/9",  "9/8",   "8/7",
                          "7/6",     "32/27",  "6/5",   "11/9",  "5/4",   "14/11", "9/7",   "21/16", "4/3",
                          "27/20",   "11/8",   "7/5",   "10/7",  "16/11", "40/27", "3/2",   "32/21", "14/9",
                          "11/7",    "8/5",    "18/11", "5/3",   "27/16", "12/7",  "7/4",   "16/9",  "9/5",
                          "20/11",   "11/6",   "15/8",  "40/21", "64/33", "160/81", "2/1"};
  std::string s = "! partch43.scl\r\n!\r\nPartch's 43-tone monophonic scale (ratios)\r\n 43\r\n!\r\n";
  for (const char* r : ratios) {
    s += ' ';
    s += r;
    s += "\r\n";
  }
  return s;
}

}  // namespace

int main() {
  std::mt19937_64 rng(3);

  // ---- Scale resolve ---------------------------------------------------------------------------------------------
  const Scale partch = *Scale::make(*parseScl(partch43()));
  KeyboardMapping kbm = KeyboardMapping::linear(60, 69, 440.0);
  const Tuning t = *makeScalaTuning(partch, kbm);
  std::vector<PitchId> ids, wrapped;
  std::vector<int32_t> keys;
  for (size_t i = 0; i < kCount; ++i) {
    ids.push_back(PitchId::fromDegree(static_cast<int32_t>(rng() % 43), static_cast<int32_t>(rng() % 5) - 2));
    wrapped.push_back(PitchId::fromDegree(static_cast<int32_t>(rng() % 200) - 100, 0));
    keys.push_back(static_cast<int32_t>(rng() % 128));
  }
  std::vector<float> out(kCount);
  auto s = pq::bench::measure(
      [&] {
        for (size_t i = 0; i < kCount; ++i) out[i] = resolveHz(ids[i], t, {});
        pq::bench::doNotOptimize(out[kCount / 2]);
      },
      kCount);
  pq::bench::report("resolveHz/degree(Scale partch43)", s, "ns/pitch");
  s = pq::bench::measure(
      [&] {
        for (size_t i = 0; i < kCount; ++i) out[i] = resolveHz(wrapped[i], t, {});
        pq::bench::doNotOptimize(out[kCount / 2]);
      },
      kCount);
  pq::bench::report("resolveHz/degree(Scale, out-of-range deg)", s, "ns/pitch");
  // 12-key pattern mapping with formal octave, as from a .kbm.
  KeyboardMapping kbm12;
  kbm12.mapSize = 12;
  kbm12.formalOctaveDegree = 43;
  kbm12.mapping = {0, 4, 7, 9, 11, 13, 17, 20, 24, 27, 30, 33};
  const Tuning t12 = *makeScalaTuning(partch, kbm12);
  s = pq::bench::measure(
      [&] {
        for (size_t i = 0; i < kCount; ++i) {
          const auto p = mapKey(kbm12, keys[i], 43);
          out[i] = p ? resolveHz(*p, t12, {}) : 0.0f;
        }
        pq::bench::doNotOptimize(out[kCount / 2]);
      },
      kCount);
  pq::bench::report("mapKey(kbm 12/43)+resolveHz", s, "ns/key");

  // ---- Parsing ---------------------------------------------------------------------------------------------------
  const std::string text = partch43();
  s = pq::bench::measure(
      [&] {
        auto r = parseScl(text);
        pq::bench::doNotOptimize(r.ok());
      },
      1);
  pq::bench::report("parseScl(partch43, 43 ratios)", s, "ns/file");
  std::string cents = "cents\n 1000\n";
  for (int i = 1; i <= 1000; ++i) cents += " " + std::to_string(i * 1.2345678) + "\n";
  s = pq::bench::measure(
      [&] {
        auto r = parseScl(cents);
        pq::bench::doNotOptimize(r.ok());
      },
      1000);
  pq::bench::report("parseScl(1000 cents lines)", s, "ns/line");
  const ScalaScale partchScl = *parseScl(text);
  s = pq::bench::measure(
      [&] {
        auto w = writeScl(partchScl);
        pq::bench::doNotOptimize(w.ok());
      },
      1);
  pq::bench::report("writeScl(partch43)", s, "ns/file");

  // ---- Fewest-keypress BFS ---------------------------------------------------------------------------------------
  const std::vector<KeyRatio> keymap = jsObjectKeyOrder(trainerDefaultKeymap());
  for (const uint32_t edo : {12u, 31u, 53u, 72u}) {
    s = pq::bench::measure(
        [&] {
          auto m = FewestKeypressMap::build(keymap, edo, 29);
          pq::bench::doNotOptimize(m.ok());
        },
        1);
    const std::string name = "FewestKeypressMap::build(default keys, " + std::to_string(edo) + "edo, 29-lim)";
    pq::bench::report(name.c_str(), s, "ns/build");
  }
  const FewestKeypressMap m53 = *FewestKeypressMap::build(keymap, 53, 13);
  s = pq::bench::measure(
      [&] {
        int64_t acc = 0;
        for (int64_t st = -106; st <= 106; ++st) acc += m53.find(st) != nullptr;
        pq::bench::doNotOptimize(acc);
      },
      213);
  pq::bench::report("FewestKeypressMap::find", s, "ns/lookup");

  // ---- JI helpers ------------------------------------------------------------------------------------------------
  std::vector<Rational> small, big;
  std::vector<uint64_t> big64;
  for (size_t i = 0; i < kCount; ++i) {
    small.emplace_back(static_cast<int64_t>(rng() % 4096 + 1), static_cast<int64_t>(rng() % 4096 + 1));
    big.emplace_back(static_cast<int64_t>(rng() >> 2) | 1, static_cast<int64_t>(rng() >> 2) | 1);
    big64.push_back(rng() | 1);
  }
  auto benchR = [&](const char* name, const std::vector<Rational>& v, auto&& fn) {
    const auto st = pq::bench::measure(
        [&] {
          uint64_t acc = 0;
          for (const Rational& r : v) acc += fn(r);
          pq::bench::doNotOptimize(acc);
        },
        v.size());
    pq::bench::report(name, st, "ns/ratio");
  };
  benchR("isWithinPrimeLimit(r<=4096, 13)", small, [](const Rational& r) { return isWithinPrimeLimit(r, 13); });
  benchR("primeLimitOf(r<=4096)", small, [](const Rational& r) { return primeLimitOf(r); });
  benchR("primeLimitOf(random 62-bit parts)", big, [](const Rational& r) { return primeLimitOf(r); });
  benchR("octaveReduce(r<=4096)", small,
         [](const Rational& r) { return static_cast<uint64_t>(octaveReduce(r).value_or(Rational{}).num()); });
  benchR("reduceToEquave(r<=4096, 3/1)", small, [](const Rational& r) {
    return static_cast<uint64_t>(reduceToEquave(r, Rational(3)).value_or(Rational{}).num());
  });
  benchR("tenneyHeight(r<=4096)", small, [](const Rational& r) { return static_cast<uint64_t>(tenneyHeight(r)); });
  benchR("findJiInterval(r<=4096)", small, [](const Rational& r) { return findJiInterval(r) != nullptr; });
  s = pq::bench::measure(
      [&] {
        uint64_t acc = 0;
        for (uint64_t n : big64) acc += largestPrimeFactor(n);
        pq::bench::doNotOptimize(acc);
      },
      big64.size(), 5, 1);
  pq::bench::report("largestPrimeFactor(random odd 64-bit)", s, "ns/number");
  return 0;
}
