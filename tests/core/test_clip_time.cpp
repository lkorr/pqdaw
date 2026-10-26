// M3 review L16: the clip-time math shared by the document and the engine (pq/core/clip_time.h), against a
// tick-by-tick model of the documented rule.
#include <doctest/doctest.h>

#include <algorithm>
#include <random>
#include <vector>

#include "pq/core/clip_time.h"

using pq::Ticks;

namespace {

// The rule, one tick at a time: content runs from contentStart; with a loop, reaching loop.end jumps to loop.start
// (a start marker at or after loop.end first folds into the loop).
std::vector<Ticks> contentModel(Ticks contentStart, const pq::ContentLoop& l, Ticks length) {
  std::vector<Ticks> c;
  Ticks p = contentStart;
  if (pq::contentLooped(l))
    while (p >= l.end) p -= l.end - l.start;
  for (Ticks o = 0; o < length; ++o) {
    c.push_back(p);
    ++p;
    if (pq::contentLooped(l) && p == l.end) p = l.start;
  }
  return c;
}

}  // namespace

TEST_CASE("clip time: content positions and passes match the tick-by-tick rule (looped, unlooped, folded markers)") {
  std::mt19937 rng(16);
  std::uniform_int_distribution<int> u(0, 40);
  for (int trial = 0; trial < 3000; ++trial) {
    pq::ContentLoop l{rng() % 4 != 0, u(rng), u(rng)};  // end ≤ start (loop off) included
    const Ticks contentStart = u(rng), start = u(rng) - 10, length = 1 + u(rng) * 2;
    INFO("trial " << trial << ": loop " << l.enabled << " [" << l.start << ", " << l.end << "), contentStart "
                  << contentStart << ", start " << start << ", length " << length);
    const std::vector<Ticks> model = contentModel(contentStart, l, length);
    for (Ticks o = 0; o < length; ++o) REQUIRE(pq::clipContentAt(contentStart, l, o) == model[o]);
    // The first pass ends at the first wrap.
    Ticks firstWrap = length;
    for (Ticks o = 1; o < length; ++o)
      if (model[o] != model[o - 1] + 1) {
        firstWrap = o;
        break;
      }
    if (pq::contentLooped(l)) {
      CHECK(pq::clipFirstPassLength(contentStart, l) >= 1);
      CHECK(std::min(pq::clipFirstPassLength(contentStart, l), length) == firstWrap);
    } else {
      CHECK(firstWrap == length);
    }
    // Segments over a random window cover it exactly, contiguous in content within each one, in time order.
    const Ticks from = start + u(rng) - 5, to = from + u(rng) * 2;
    Ticks expectAt = std::max(from, start);
    pq::forEachClipSegment(start, length, contentStart, l, from, to, [&](const pq::ClipSegment& s) {
      REQUIRE(s.at == expectAt);
      REQUIRE(s.contentTo > s.contentFrom);
      for (Ticks k = 0; k < s.contentTo - s.contentFrom; ++k) REQUIRE(model[s.at - start + k] == s.contentFrom + k);
      expectAt = s.at + (s.contentTo - s.contentFrom);
    });
    CHECK(expectAt == std::max(std::max(from, start), std::min(to, start + length)));
  }
}
