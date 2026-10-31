// T5.9: search. The dynamic-program scorer against a brute-force enumeration of every alignment; whole searches
// (filters, tags, narrowing keystroke by keystroke) against a brute-force reference search; cancellation; the
// Searcher worker; the 200k-entry keystroke timing (`perf`, ≤ 5 ms median in release).

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <random>

#include "library_test_util.h"
#include "synthetic_index.h"
#include "pq/library/search.h"

using namespace pq;
using namespace pq::library;
using namespace pq::library::test;
using namespace std::chrono_literals;

namespace {

// ---- The reference, written from the definition in search.h (independently of search.cpp) ----

bool refSep(char c) { return std::string_view(" _-.,+&()[]{}/").find(c) != std::string_view::npos; }
bool refLower(char c) { return c >= 'a' && c <= 'z'; }
bool refUpper(char c) { return c >= 'A' && c <= 'Z'; }
bool refDigit(char c) { return c >= '0' && c <= '9'; }
char refFold(char c) { return refUpper(c) ? static_cast<char>(c - 'A' + 'a') : c; }

int32_t refBonus(std::string_view s, std::size_t j) {
  if (j == 0) return kScoreNameStart;
  const char p = s[j - 1], c = s[j];
  const bool start = refSep(p) || (refLower(p) && refUpper(c)) || ((refLower(p) || refUpper(p)) && refDigit(c)) ||
                     (refDigit(p) && (refLower(c) || refUpper(c)));
  return start ? kScoreWordStart : 0;
}

void enumerate(std::string_view t, std::string_view s, std::size_t i, std::size_t prev, int32_t acc, int32_t& best) {
  for (std::size_t j = i == 0 ? 0 : prev + 1; j < s.size(); ++j) {
    if (refFold(s[j]) != t[i]) continue;
    int32_t sc = acc + kScoreMatch + refBonus(s, j);
    if (i > 0) sc += j == prev + 1 ? kScoreConsecutive : -kScoreGap * static_cast<int32_t>(j - prev - 1);
    if (i + 1 == t.size()) best = std::max(best, sc);
    else enumerate(t, s, i + 1, j, sc, best);
  }
}

int32_t bruteScore(std::string_view foldedTerm, std::string_view s) {
  int32_t best = kNoMatch;
  if (!foldedTerm.empty()) enumerate(foldedTerm, s, 0, 0, 0, best);
  return best;
}

struct RefResult {
  std::vector<SearchHit> hits;
  uint32_t total = 0;
};

RefResult referenceSearch(const Index& idx, const TagStore& store, const SearchQuery& q) {
  std::vector<std::string> terms;
  {
    std::string cur;
    for (char c : q.text + " ") {
      if (c == ' ') {
        if (!cur.empty()) terms.push_back(cur);
        cur.clear();
      } else {
        cur.push_back(refFold(c));
      }
    }
  }
  const SearchFilter& f = q.filter;
  struct S {
    uint32_t e;
    int32_t score;
    std::size_t stemLen;
  };
  std::vector<S> all;
  for (uint32_t e = 0; e < idx.entryCount(); ++e) {
    const EntryRecord r = idx.record(e);
    if (!(f.kinds & (1u << uint32_t(r.kind))) || !(f.categories & (1u << uint32_t(r.category)))) continue;
    const uint32_t place = idx.placeOf(e);
    if (place < 64 && !(f.places & (uint64_t{1} << place))) continue;
    if (f.minSeconds > 0 || f.maxSeconds != std::numeric_limits<double>::infinity()) {
      if (r.rate == 0) continue;
      const double sec = double(r.frames) / r.rate;
      if (sec < f.minSeconds || sec > f.maxSeconds) continue;
    }
    if (f.minBpm > 0 || f.maxBpm != std::numeric_limits<float>::infinity())
      if (r.bpm <= 0 || r.bpm < f.minBpm || r.bpm > f.maxBpm) continue;
    if (!f.rates.empty() && std::find(f.rates.begin(), f.rates.end(), r.rate) == f.rates.end()) continue;
    if (f.channels && r.channels != f.channels) continue;
    const TagItem* item = store.find(tagKeyOf(idx, e));
    if (f.collections && (!item || !(item->collections & f.collections))) continue;
    bool tagsOk = true;
    for (const std::string& want : f.tags) {
      bool has = false;
      std::string b = want;
      for (char& c : b) c = refFold(c);
      if (item)
        for (std::string a : item->tags) {
          for (char& c : a) c = refFold(c);
          has = has || a == b;
        }
      tagsOk = tagsOk && has;
    }
    if (!tagsOk) continue;
    int32_t total = 0;
    bool ok = true;
    const std::string_view stem = idx.stem(e);
    for (const std::string& t : terms) {
      int32_t best = bruteScore(t, stem);
      if (item)
        for (const std::string& tag : item->tags) {
          const int32_t s = bruteScore(t, tag);
          if (s != kNoMatch) best = std::max(best, s - kScoreTagPenalty);
        }
      if (best == kNoMatch) {
        ok = false;
        break;
      }
      total += best;
    }
    if (!ok) continue;
    if (!terms.empty()) total -= kScoreDepth * int32_t(std::min<uint16_t>(idx.dirs.depth[idx.entries.dir[e]], 255));
    all.push_back({e, total, stem.size()});
  }
  const bool hasText = !terms.empty();
  std::sort(all.begin(), all.end(), [&](const S& a, const S& b) {
    if (a.score != b.score) return a.score > b.score;
    if (hasText && a.stemLen != b.stemLen) return a.stemLen < b.stemLen;
    return a.e < b.e;
  });
  RefResult r;
  r.total = static_cast<uint32_t>(all.size());
  for (std::size_t i = 0; i < all.size() && i < kMaxResults; ++i) r.hits.push_back({all[i].e, all[i].score});
  return r;
}

}  // namespace

TEST_CASE("library: fuzzy scorer equals the brute-force enumeration") {
  std::mt19937 rng(7);
  const char alphabet[] = "abAB_1 c-";
  std::vector<int32_t> scratch;
  int matched = 0;
  for (int iter = 0; iter < 20000; ++iter) {
    std::string s, t;
    const int n = 1 + static_cast<int>(rng() % 14);
    for (int i = 0; i < n; ++i) s.push_back(alphabet[rng() % (sizeof alphabet - 1)]);
    const int m = 1 + static_cast<int>(rng() % 4);
    for (int i = 0; i < m; ++i) {
      char c = alphabet[rng() % (sizeof alphabet - 1)];
      if (c == ' ') c = 'a';
      t.push_back(refFold(c));
    }
    const int32_t fast = fuzzyScore(t, s, scratch);
    const int32_t ref = bruteScore(t, s);
    REQUIRE_MESSAGE(fast == ref, "term '" << t << "' in '" << s << "'");
    matched += fast != kNoMatch;
  }
  CHECK(matched > 2000);  // the corpus exercises real alignments, not only rejections

  // Ranking basics.
  CHECK(fuzzyScore("kick", "Kick 01", scratch) > fuzzyScore("kick", "Big Kick", scratch));      // name start
  CHECK(fuzzyScore("bl", "BassLine", scratch) > fuzzyScore("bl", "Bubble", scratch));         // word starts
  CHECK(fuzzyScore("sn", "Snare", scratch) > fuzzyScore("sn", "Sound", scratch));             // consecutive
  CHECK(fuzzyScore("ab", "a....b", scratch) > fuzzyScore("ab", "a.........b", scratch));      // gaps cost
  CHECK(fuzzyScore("xyz", "abc", scratch) == kNoMatch);
  CHECK(fuzzyScore("abcd", "abc", scratch) == kNoMatch);
}

TEST_CASE("library: search equals the brute-force reference (filters, tags, narrowing)") {
  auto idx = syntheticIndex(4000, 11);
  TagStore store;
  tagSome(store, *idx, 5, 12);
  auto tags = buildEntryTags(*idx, store);
  REQUIRE(tags->combos.size() > 5);

  std::vector<SearchFilter> filters(9);
  filters[1].kinds = 1u << uint32_t(ItemKind::Sample);
  filters[2].minSeconds = 0.5;
  filters[2].maxSeconds = 3;
  filters[3].minBpm = 100;
  filters[3].maxBpm = 140;
  filters[4].rates = {48000};
  filters[4].channels = 2;
  filters[5].collections = 0b0000101;
  filters[6].tags = {"DRUMS"};
  filters[7].places = 1;
  filters[7].categories = (1u << uint32_t(Category::Instruments)) | (1u << uint32_t(Category::Samples));
  filters[8].tags = {"nonexistent"};

  const std::vector<std::string> words = {"kick 808", "snrl", "bass line", "dark", "hat_o", "zz", "loop 9", "drums",
                                          "x"};
  int narrowedRuns = 0, compared = 0;
  for (std::size_t fi = 0; fi < filters.size(); ++fi) {
    // Empty text: the filter alone, in index order.
    {
      SearchQuery q{"", filters[fi]};
      const SearchResult res = search(idx, tags, q);
      const RefResult ref = referenceSearch(*idx, store, q);
      CHECK(res.total == ref.total);
      CHECK(res.hits == ref.hits);
    }
    for (const std::string& w : words) {
      SearchResult prev;
      bool havePrev = false;
      for (const std::string& text : prefixes(w)) {
        SearchQuery q{text, filters[fi]};
        SearchResult res = search(idx, tags, q, havePrev ? &prev : nullptr);
        const RefResult ref = referenceSearch(*idx, store, q);
        REQUIRE_MESSAGE(res.total == ref.total, "filter " << fi << " text '" << text << "'");
        REQUIRE_MESSAGE(res.hits == ref.hits, "filter " << fi << " text '" << text << "'");
        if (havePrev) {
          CHECK(res.narrowed);
          narrowedRuns += 1;
        }
        ++compared;
        prev = std::move(res);
        havePrev = true;
      }
    }
  }
  CHECK(narrowedRuns > 100);
  MESSAGE(compared << " queries compared with the reference");

  // A different filter (or a shorter text) does not narrow.
  SearchQuery a{"ki", {}};
  const SearchResult ra = search(idx, tags, a);
  SearchQuery b{"kic", filters[1]};
  CHECK_FALSE(search(idx, tags, b, &ra).narrowed);
  SearchQuery c{"k", {}};
  CHECK_FALSE(search(idx, tags, c, &ra).narrowed);
  // More than kMaxResults matches: the top kMaxResults are materialised, the total counts all.
  SearchQuery all{"", {}};
  const SearchResult rall = search(idx, tags, all);
  CHECK(rall.total == idx->entryCount());
  CHECK(rall.hits.size() == kMaxResults);
  // No tags at all: tag-only filters match nothing, text still works.
  SearchQuery tq{"kick", filters[6]};
  CHECK(search(idx, nullptr, tq).total == 0);
  SearchQuery plain{"kick", {}};
  CHECK(search(idx, nullptr, plain).total == referenceSearch(*idx, TagStore(), plain).total);
}

TEST_CASE("library: search cancellation and the Searcher worker") {
  auto idx = syntheticIndex(3000, 3);
  std::atomic<uint64_t> latest{2};
  SearchQuery q{"k", {}};
  const SearchResult stale = search(idx, nullptr, q, nullptr, &latest, 1);
  CHECK(stale.cancelled);
  const SearchResult fresh = search(idx, nullptr, q, nullptr, &latest, 2);
  CHECK_FALSE(fresh.cancelled);

  Searcher s;
  std::atomic<uint64_t> results{0};
  s.setOnResult([&](uint64_t) { ++results; });
  uint64_t ticket = 0;
  for (const std::string& t : prefixes("snare")) ticket = s.submit(idx, nullptr, SearchQuery{t, {}});
  auto r = s.waitFor(ticket, 10s);
  REQUIRE(r);
  CHECK(r->ticket == ticket);
  CHECK(r->query.text == "snare");
  const RefResult ref = referenceSearch(*idx, TagStore(), SearchQuery{"snare", {}});
  CHECK(r->hits == ref.hits);
  CHECK(results >= 1);
  CHECK(s.latest() == r);
}

TEST_CASE("library: search keystrokes over 200k entries, median <= 5 ms (release)" * doctest::test_suite("perf")) {
  auto idx = syntheticIndex(200000, 99);
  TagStore store;
  tagSome(store, *idx, 1, 400);
  auto tags = buildEntryTags(*idx, store);
  std::vector<double> times;
  double worst = 0;
  for (const char* w : kTypedWords) {
    SearchResult prev;
    bool havePrev = false;
    for (const std::string& text : prefixes(w)) {
      const auto t0 = std::chrono::steady_clock::now();
      SearchResult res = search(idx, tags, SearchQuery{text, {}}, havePrev ? &prev : nullptr);
      const double ms = msSince(t0);
      times.push_back(ms);
      worst = std::max(worst, ms);
      prev = std::move(res);
      havePrev = true;
    }
  }
  std::sort(times.begin(), times.end());
  const double median = times[times.size() / 2];
  MESSAGE("200k entries: " << times.size() << " keystrokes, median " << median << " ms, p90 "
                           << times[times.size() * 9 / 10] << " ms, worst " << worst << " ms");
#ifdef PQ_OPTIMIZED
  CHECK(median <= 5.0);
#endif
}

// M5 review M15: EntryTags built for another index snapshot (BrowserPanel paired lib_.index() with lib_.entryTags(),
// which reads index() again: the indexer may publish in between). search() read tags->combo[e] past its end (0xC0000005
// with 1 entry's tags over 2M entries). Mismatched tags are now ignored: the search runs as without tags.
TEST_CASE("library: search ignores tags built for another index (no out-of-bounds read)") {
  auto tiny = syntheticIndex(1, 1);
  TagStore ts;
  ts.setCollection(tagKeyOf(*tiny, 0), 1, true);
  auto tags = buildEntryTags(*tiny, ts);
  REQUIRE(tags->combo.size() == 1);
  auto big = syntheticIndex(200000, 7);
  SearchQuery q{"s", {}};
  const SearchResult withMismatch = search(big, tags, q);
  const SearchResult without = search(big, nullptr, q);
  CHECK(withMismatch.total == without.total);
  CHECK(withMismatch.hits.size() == without.hits.size());
  q.filter.collections = 2;  // needs the tags: none apply
  CHECK(search(big, tags, q).total == 0);
}
