// T5.9: the browser index and search. Runner benchmarks (pq_bench.h), on a synthetic 200k-entry index in memory
// (tests/library/synthetic_index.h; no files):
//   search/keystroke 200k     every keystroke of the typed words (kTypedWords), each narrowing the previous result as
//                             the Searcher does: ms per keystroke (median over all keystrokes of one round)
//   search/first char 200k    a one-character query over all entries (no narrowing: the worst keystroke)
//   search/filter only 200k   empty text, Samples 0.5–3 s at 48 kHz (filters only, index order)
//   fuzzy/score               fuzzyScore of a 4-character term in a 24-character name: ns per call
//   tags/build 200k           buildEntryTags with ~500 tagged files: ms
//   index/load 200k           loadIndex of the saved synthetic index from %TEMP% (warm OS cache): ms
//   index/save 200k           saveIndex (write, flush, rename): ms
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include "../../tests/library/synthetic_index.h"
#include "pq/library/index_file.h"
#include "pq/library/search.h"
#include "pq/library/tags.h"
#include "pq_bench.h"

using namespace pq;
using namespace pq::library;
using namespace pq::library::test;
namespace fs = std::filesystem;

namespace {

double msSince(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

struct Fixture {
  std::shared_ptr<Index> index = syntheticIndex(200000, 99);
  TagStore store;
  std::shared_ptr<const EntryTags> tags;
  Fixture() {
    tagSome(store, *index, 1, 400);
    tags = buildEntryTags(*index, store);
  }
};

Fixture& fixture() {
  static Fixture f;
  return f;
}

}  // namespace

int main(int argc, char** argv) {
  bench::Runner r("pq_bench_library");

  r.add("search/keystroke 200k", [](bench::Context& c) {
    Fixture& f = fixture();
    std::vector<double> perKey;
    for (int round = 0; round < std::max(3, c.options().repeats); ++round) {
      for (const char* w : kTypedWords) {
        SearchResult prev;
        bool have = false;
        for (const std::string& text : prefixes(w)) {
          const auto t0 = std::chrono::steady_clock::now();
          SearchResult res = search(f.index, f.tags, SearchQuery{text, {}}, have ? &prev : nullptr);
          perKey.push_back(msSince(t0) * 1e6);  // ns: summarize's fields are ns-named; reported in ms below
          prev = std::move(res);
          have = true;
        }
      }
    }
    bench::Stats s = bench::summarize(perKey);
    s.medianNs /= 1e6;
    s.minNs /= 1e6;
    s.p99Ns /= 1e6;
    s.maxNs /= 1e6;
    c.record(s, "ms/keystroke");
  });

  r.add("search/first char 200k", [](bench::Context& c) {
    Fixture& f = fixture();
    c.measure([&] { (void)search(f.index, f.tags, SearchQuery{"k", {}}); }, 1, "ns/query");
  });

  r.add("search/filter only 200k", [](bench::Context& c) {
    Fixture& f = fixture();
    SearchFilter filter;
    filter.categories = 1u << uint32_t(Category::Samples);
    filter.minSeconds = 0.5;
    filter.maxSeconds = 3;
    filter.rates = {48000};
    c.measure([&] { (void)search(f.index, f.tags, SearchQuery{"", filter}); }, 1, "ns/query");
  });

  r.add("fuzzy/score", [](bench::Context& c) {
    std::vector<int32_t> scratch;
    const std::string_view name = "Kick_808 Hard SnareRoll 7";
    int32_t sink = 0;
    c.measure([&] { sink += fuzzyScore("ksr7", name, scratch); }, 1, "ns/call");
    if (sink == 1) std::printf(" ");
  });

  r.add("tags/build 200k", [](bench::Context& c) {
    Fixture& f = fixture();
    c.measure([&] { (void)buildEntryTags(*f.index, f.store); }, 1, "ns/build");
  });

  const fs::path dir = fs::temp_directory_path() / ("pq_bench_library_" + std::to_string(GetCurrentProcessId()));
  r.add("index/save 200k", [&](bench::Context& c) {
    Fixture& f = fixture();
    fs::create_directories(dir);
    c.measure([&] { (void)saveIndex(*f.index, 1, dir / "index.pqi"); }, 1, "ns/save");
  });
  r.add("index/load 200k", [&](bench::Context& c) {
    Fixture& f = fixture();
    fs::create_directories(dir);
    (void)saveIndex(*f.index, 1, dir / "index.pqi");
    c.measure([&] { (void)loadIndex(dir / "index.pqi"); }, 1, "ns/load");
  });

  const int rc = r.main(argc, argv);
  std::error_code ec;
  fs::remove_all(dir, ec);
  return rc;
}
