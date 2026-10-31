// M5 review (library): in-process fuzzer for index.pqi / index.pqj / tags.json loaders, plus targeted probes.
// Modes:
//   fuzz_library fuzz <workdir> <iterations> <seed>   loader fuzzing (runs inside a 1.5 GB job memory limit)
//   fuzz_library header <workdir>                       header-count mutations: allocation / exception behaviour
//   fuzz_library indexer <datadir> <placeDir>           runs an Indexer on a data dir (crash check, child process)
//   fuzz_library spin <workdir>                         Indexer with an unwritable data dir: wakeups / CPU over 2 s
//   fuzz_library overflow <workdir> <files> [probeSleepUs]           watcher buffer overflow: index converges?
//   fuzz_library tagsmismatch                           search() with EntryTags built for another index
//   fuzz_library dp <iterations> <seed>                 fuzzyScore vs brute force on a rich alphabet
#include <windows.h>
#include <psapi.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "pq/library/index.h"
#include "pq/library/index_file.h"
#include "pq/library/indexer.h"
#include "pq/library/scan.h"
#include "pq/library/search.h"
#include "pq/library/tags.h"

using namespace pq;
using namespace pq::library;
namespace fs = std::filesystem;

namespace {

uint64_t hashWords(const uint8_t* p, std::size_t n) {
  uint64_t h = 0xcbf29ce484222325ull;
  std::size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    uint64_t w;
    std::memcpy(&w, p + i, 8);
    h = (h ^ w) * 0x100000001b3ull;
  }
  if (i < n) {
    uint64_t w = 0;
    std::memcpy(&w, p + i, n - i);
    h = (h ^ w) * 0x100000001b3ull;
  }
  return h ^ (h >> 32);
}

void limitMemory(SIZE_T bytes) {
  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
  li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY;
  li.ProcessMemoryLimit = bytes;
  SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
  AssignProcessToJobObject(job, GetCurrentProcess());
}

std::vector<uint8_t> readAll(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}
void writeAll(const fs::path& p, const std::vector<uint8_t>& b) {
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}

// A synthetic tree served from memory (probed records: no disk probes).
class FakeSource final : public ListingSource {
public:
  explicit FakeSource(uint32_t seed) : rng_(seed) {}
  bool list(uint32_t, const PlaceSpec&, std::string_view rel, Listing& out) override {
    const int depth = rel.empty() ? 0 : 1 + static_cast<int>(std::count(rel.begin(), rel.end(), '\\'));
    std::uniform_int_distribution<int> nf(0, 12), nd(0, depth < 3 ? 4 : 0), ch(0, 25);
    static const char* exts[] = {".wav", ".flac", ".pqpreset", ".pqdevice", ".scl", ".pqclip", ".mid", ".pqp", ".pqd"};
    const int files = nf(rng_);
    for (int i = 0; i < files; ++i) {
      EntryRecord r;
      std::string stem;
      for (int k = 0; k < 3 + (i % 9); ++k) stem.push_back(static_cast<char>((k % 4 == 0 ? 'A' : 'a') + ch(rng_)));
      r.name = stem + "_" + std::to_string(i) + exts[i % 9];
      r.kind = *indexedKind(r.name);
      r.category = Category(1 + i % 7);
      r.size = 1000 + i;
      r.mtime = 1234567 + i;
      r.volume = 0xABCDEF;
      for (auto& b : r.fileId) b = static_cast<uint8_t>(rng_());
      r.frames = 48000u * (1 + i);
      r.rate = 48000;
      r.channels = 2;
      r.bpm = (i % 3) ? 120.f : 0.f;
      out.files.push_back(std::move(r));
    }
    const int dirs = nd(rng_);
    for (int i = 0; i < dirs; ++i) out.subdirs.push_back("Dir" + std::to_string(i));
    out.probed = true;
    return true;
  }

private:
  std::mt19937 rng_;
};

class FailSource final : public ListingSource {
public:
  bool list(uint32_t, const PlaceSpec&, std::string_view, Listing&) override { return false; }
};

std::vector<PlaceSpec> fakePlaces() {
  return {{"C:\\fuzz\\UserLib", PlaceKind::UserLibrary, "User Library"},
          {"C:\\fuzz\\Proj", PlaceKind::Project, "Proj"},
          {"D:\\fuzz\\Pack", PlaceKind::Folder, "Pack"}};
}

std::shared_ptr<Index> buildFakeIndex(uint32_t seed, std::vector<DirChange>* changes) {
  FakeSource src(seed);
  ScanRequest req;
  req.places = fakePlaces();
  ScanResult r = rebuild(nullptr, req, src, ProbeFn());
  if (changes) {
    // A second pass with a different seed: produce journal records against the first index.
    FakeSource src2(seed + 1);
    ScanRequest req2;
    req2.places = req.places;
    req2.dirty.resize(req.places.size());
    for (auto& d : req2.dirty) d.full = true;
    ScanResult r2 = rebuild(r.index.get(), req2, src2, ProbeFn());
    *changes = r2.changes;
  }
  return r.index;
}

// Touches everything a consumer of a loaded index touches.
uint64_t exercise(const std::shared_ptr<const Index>& idx) {
  uint64_t acc = 0;
  for (uint32_t e = 0; e < idx->entryCount(); ++e) {
    acc += idx->path(e).size() + idx->stem(e).size();
    acc += idx->record(e).name.size();
    acc += idx->placeOf(e);
  }
  for (uint32_t d = 0; d < idx->dirCount(); ++d) acc += idx->dirPath(d).size();
  // Rebuild from it (copy path and merge path).
  {
    FailSource fail;
    ScanRequest req;
    req.places = idx->places;
    req.dirty.resize(req.places.size());
    ScanResult r = rebuild(idx.get(), req, fail, ProbeFn());
    acc += r.index->entryCount();
    FakeSource src(7);
    for (auto& d : req.dirty) d.full = true;
    ScanResult r2 = rebuild(idx.get(), req, src, [](const std::string&, const PlaceSpec&, std::string_view, EntryRecord&) {});
    acc += r2.index->entryCount();
  }
  for (const char* q : {"a", "ab", "Ab c", "zz9", "_"}) {
    SearchQuery sq;
    sq.text = q;
    SearchResult r = search(idx, nullptr, sq);
    acc += r.total;
    sq.filter.minSeconds = 1;
    sq.filter.places = 1;
    acc += search(idx, nullptr, sq).total;
  }
  if (idx->entryCount()) acc += idx->findPath(idx->path(0));
  return acc;
}

const char* kFieldNames[] = {"version", "headerBytes", "sequence", "places", "dirs", "entries", "reserved", "strings",
                             "payloadBytes", "hash"};
const int kFieldOff[] = {8, 12, 16, 24, 28, 32, 36, 40, 48, 56};
const int kFieldLen[] = {4, 4, 8, 4, 4, 4, 4, 8, 8, 8};

int modeHeader(const fs::path& dir) {
  limitMemory(SIZE_T{1536} << 20);
  fs::create_directories(dir);
  auto idx = buildFakeIndex(1, nullptr);
  const fs::path pqi = dir / "index.pqi";
  if (!saveIndex(*idx, 3, pqi)) return 2;
  const std::vector<uint8_t> good = readAll(pqi);
  std::printf("valid index: %u dirs, %u entries, %zu bytes\n", idx->dirCount(), idx->entryCount(), good.size());
  // The header is outside the hash: change one count (a single bit flip each) and load.
  const struct {
    int field;
    uint32_t xorMask;
  } cases[] = {{3, 0x00800000}, {4, 0x00800000}, {5, 0x00800000}, {5, 0x80000000}, {5, 0x08000000}, {4, 0x04000000}};
  for (auto c : cases) {
    std::vector<uint8_t> b = good;
    uint32_t v;
    std::memcpy(&v, b.data() + kFieldOff[c.field], 4);
    v ^= c.xorMask;
    std::memcpy(b.data() + kFieldOff[c.field], &v, 4);
    writeAll(dir / "hdr.pqi", b);
    PROCESS_MEMORY_COUNTERS_EX before{}, after{};
    K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&before), sizeof before);
    const auto t0 = std::chrono::steady_clock::now();
    const char* outcome = "?";
    try {
      auto r = loadIndex(dir / "hdr.pqi");
      outcome = r ? "LOADED" : "rejected";
    } catch (const std::bad_alloc&) {
      outcome = "THREW std::bad_alloc";
    } catch (const std::length_error&) {
      outcome = "THREW std::length_error";
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&after), sizeof after);
    std::printf("header %-8s ^= 0x%08x (now %u): %s in %.1f ms, peak commit %.0f MB\n", kFieldNames[c.field], c.xorMask, v,
                outcome, ms, after.PeakPagefileUsage / 1048576.0);
  }
  return 0;
}

int modeFuzz(const fs::path& dir, uint32_t iters, uint32_t seed) {
  limitMemory(SIZE_T{1536} << 20);
  fs::create_directories(dir);
  std::vector<DirChange> changes;
  auto idx = buildFakeIndex(seed, &changes);
  const fs::path pqi = dir / "f.pqi", pqj = dir / "f.pqj", tj = dir / "tags.json";
  if (!saveIndex(*idx, 5, pqi)) return 2;
  const std::vector<uint8_t> goodI = readAll(pqi);
  {
    Journal j;
    if (!j.reset(pqj, 5) || !j.append(changes)) return 3;
  }
  const std::vector<uint8_t> goodJ = readAll(pqj);
  // tags.json with a few items.
  {
    TagStore ts(tj);
    for (uint32_t e = 0; e < std::min<uint32_t>(idx->entryCount(), 40); ++e) {
      ts.setCollection(tagKeyOf(*idx, e), e % 7, true);
      ts.addTag(tagKeyOf(*idx, e), "tag" + std::to_string(e % 5));
    }
    if (!ts.save()) return 4;
  }
  const std::vector<uint8_t> goodT = readAll(tj);
  std::printf("corpus: pqi %zu B (%u entries), pqj %zu B (%zu records), tags.json %zu B\n", goodI.size(),
              idx->entryCount(), goodJ.size(), changes.size(), goodT.size());

  std::mt19937_64 rng(seed);
  struct Stats {
    uint64_t runs = 0, loaded = 0, rejected = 0, threw = 0;
  } si, sih, sj, st;
  uint64_t acc = 0;
  auto mutate = [&](std::vector<uint8_t>& b, bool structured) {
    const int kind = static_cast<int>(rng() % (structured ? 6 : 4));
    switch (kind) {
      case 0: {  // bit flips
        const int n = 1 + static_cast<int>(rng() % 4);
        for (int i = 0; i < n && !b.empty(); ++i) b[rng() % b.size()] ^= static_cast<uint8_t>(1u << (rng() % 8));
        break;
      }
      case 1:  // truncate
        b.resize(b.empty() ? 0 : rng() % b.size());
        break;
      case 2: {  // random bytes
        const std::size_t at = b.empty() ? 0 : rng() % b.size();
        for (std::size_t i = at; i < std::min(b.size(), at + 1 + rng() % 16); ++i) b[i] = static_cast<uint8_t>(rng());
        break;
      }
      case 3: {  // a 32-bit word set to an extreme
        if (b.size() < 4) break;
        const std::size_t at = (rng() % (b.size() - 3)) & ~std::size_t{3};
        static const uint32_t ext[] = {0, 1, 0xFFFFFFFFu, 0x7FFFFFFFu, 0xFFFFu, 0x10000u, 0x80000000u};
        const uint32_t v = ext[rng() % 7];
        std::memcpy(b.data() + at, &v, 4);
        break;
      }
      case 4: {  // duplicate a chunk (JSON / records)
        if (b.size() < 8) break;
        const std::size_t a = rng() % b.size(), n = 1 + rng() % std::min<std::size_t>(64, b.size() - a);
        std::vector<uint8_t> chunk(b.begin() + a, b.begin() + a + n);
        b.insert(b.begin() + rng() % b.size(), chunk.begin(), chunk.end());
        break;
      }
      default: {  // delete a chunk
        if (b.size() < 8) break;
        const std::size_t a = rng() % b.size(), n = 1 + rng() % std::min<std::size_t>(64, b.size() - a);
        b.erase(b.begin() + a, b.begin() + a + n);
        break;
      }
    }
  };
  auto fixHash = [](std::vector<uint8_t>& b) {
    if (b.size() < 64) return;
    const uint64_t payload = b.size() - 64;
    std::memcpy(b.data() + 48, &payload, 8);
    const uint64_t h = hashWords(b.data() + 64, payload);
    std::memcpy(b.data() + 56, &h, 8);
  };
  for (uint32_t it = 0; it < iters; ++it) {
    // 1. index.pqi, raw mutation (header counts left as mutated: outside the hash).
    {
      std::vector<uint8_t> b = goodI;
      mutate(b, false);
      if (rng() % 3 == 0 && b.size() >= 64) {  // keep counts sane 2/3 of the time: header mutations are in `header`
        for (int f : {3, 4, 5}) std::memcpy(b.data() + kFieldOff[f], goodI.data() + kFieldOff[f], 4);
      }
      writeAll(dir / "m.pqi", b);
      ++si.runs;
      try {
        auto r = loadIndex(dir / "m.pqi");
        if (r) {
          ++si.loaded;
          acc += exercise(r->index);
        } else {
          ++si.rejected;
        }
      } catch (...) {
        ++si.threw;
      }
    }
    // 2. index.pqi, payload mutated and the hash recomputed (a corruption the checksum cannot see: tests the
    //    structural checks). Header counts restored.
    {
      std::vector<uint8_t> b = goodI;
      std::vector<uint8_t> payload(b.begin() + 64, b.end());
      mutate(payload, false);
      b.resize(64);
      b.insert(b.end(), payload.begin(), payload.end());
      fixHash(b);
      writeAll(dir / "h.pqi", b);
      ++sih.runs;
      try {
        auto r = loadIndex(dir / "h.pqi");
        if (r) {
          ++sih.loaded;
          acc += exercise(r->index);
          // Journal replay onto a corrupt-but-accepted base.
          acc += applyChanges(*r->index, changes)->entryCount();
        } else {
          ++sih.rejected;
        }
      } catch (...) {
        ++sih.threw;
      }
    }
    // 3. journal: raw mutation, and record-level mutation with the record hash fixed up.
    {
      std::vector<uint8_t> b = goodJ;
      const bool fix = rng() % 2 == 0;
      if (fix && b.size() > 48) {
        // Mutate inside one record's payload and recompute that record's hash.
        std::size_t at = 32;
        std::vector<std::pair<std::size_t, uint32_t>> recs;
        while (at + 16 <= b.size()) {
          uint32_t sz;
          std::memcpy(&sz, b.data() + at + 4, 4);
          recs.push_back({at, sz});
          at += 16 + sz;
        }
        auto [ra, rs] = recs[rng() % recs.size()];
        std::vector<uint8_t> payload(b.begin() + ra + 16, b.begin() + ra + 16 + rs);
        mutate(payload, true);
        const uint32_t ns = static_cast<uint32_t>(payload.size());
        const uint64_t h = hashWords(payload.data(), payload.size());
        std::vector<uint8_t> nb(b.begin(), b.begin() + ra + 4);
        nb.insert(nb.end(), reinterpret_cast<const uint8_t*>(&ns), reinterpret_cast<const uint8_t*>(&ns) + 4);
        nb.insert(nb.end(), reinterpret_cast<const uint8_t*>(&h), reinterpret_cast<const uint8_t*>(&h) + 8);
        nb.insert(nb.end(), payload.begin(), payload.end());
        nb.insert(nb.end(), b.begin() + ra + 16 + rs, b.end());
        b = std::move(nb);
      } else {
        mutate(b, false);
      }
      writeAll(dir / "m.pqj", b);
      ++sj.runs;
      try {
        Journal::Contents c = Journal::read(dir / "m.pqj", 5);
        if (!c.changes.empty()) {
          ++sj.loaded;
          auto r = applyChanges(*idx, c.changes);
          acc += exercise(r);
        } else {
          ++sj.rejected;
        }
      } catch (...) {
        ++sj.threw;
      }
    }
    // 4. tags.json.
    {
      std::vector<uint8_t> b = goodT;
      mutate(b, true);
      if (rng() % 4 == 0) mutate(b, true);
      writeAll(dir / "m.json", b);
      ++st.runs;
      try {
        TagStore ts(dir / "m.json");
        if (ts.load()) {
          ++st.loaded;
          ts.reconcile(*idx);
          auto et = buildEntryTags(*idx, ts);
          SearchQuery q;
          q.text = "ta";
          acc += search(idx, et, q).total;
          q.filter.collections = 3;
          acc += search(idx, et, q).total;
          TagStore copy(dir / "m2.json");
          (void)ts.allTags();
          // Saving what loaded must work (an exception here would abort the UI thread).
          TagStore re(dir / "m.json");
          (void)re.load();
          (void)re.save();
        } else {
          ++st.rejected;
        }
      } catch (const std::exception& e) {
        ++st.threw;
        if (st.threw <= 3) std::printf("tags.json threw: %s\n", e.what());
      }
    }
  }
  std::printf("pqi raw:          %llu runs, %llu loaded, %llu rejected, %llu threw\n", si.runs, si.loaded, si.rejected, si.threw);
  std::printf("pqi hash-fixed:   %llu runs, %llu loaded, %llu rejected, %llu threw\n", sih.runs, sih.loaded, sih.rejected, sih.threw);
  std::printf("pqj:              %llu runs, %llu with records, %llu none, %llu threw\n", sj.runs, sj.loaded, sj.rejected, sj.threw);
  std::printf("tags.json:        %llu runs, %llu loaded, %llu rejected, %llu threw\n", st.runs, st.loaded, st.rejected, st.threw);
  std::printf("(acc %llu)\n", acc);
  return 0;
}

int modeIndexer(const fs::path& data, const fs::path& place) {
  limitMemory(SIZE_T{1536} << 20);
  IndexerConfig cfg;
  cfg.dataDir = data;
  cfg.places = {{std::string(reinterpret_cast<const char*>(place.u8string().c_str())), PlaceKind::Folder, "P"}};
  Indexer ix(cfg);
  ix.start();
  const bool idle = ix.waitIdle(std::chrono::seconds(10));
  std::printf("indexer survived: idle=%d entries=%u\n", idle, ix.index()->entryCount());
  ix.stop();
  return 0;
}

int modeSpin(const fs::path& dir) {
  fs::create_directories(dir / "place");
  std::ofstream(dir / "place" / "a.pqp") << "patch A { out o = 0 }";
  std::ofstream(dir / "blocker") << "x";  // a file where the data directory should go
  IndexerConfig cfg;
  cfg.dataDir = dir / "blocker" / "library";
  cfg.places = {{std::string(reinterpret_cast<const char*>((dir / "place").u8string().c_str())), PlaceKind::Folder, "P"}};
  Indexer ix(cfg);
  FILETIME c0, e0, k0, u0, c1, e1, k1, u1;
  GetProcessTimes(GetCurrentProcess(), &c0, &e0, &k0, &u0);
  ix.start();
  ix.waitIdle(std::chrono::seconds(5));
  const uint64_t w0 = ix.stats().wakeups;
  std::this_thread::sleep_for(std::chrono::seconds(2));
  const IndexerStats s = ix.stats();
  GetProcessTimes(GetCurrentProcess(), &c1, &e1, &k1, &u1);
  auto t = [](FILETIME f) { return (uint64_t(f.dwHighDateTime) << 32 | f.dwLowDateTime) / 10000.0; };
  std::printf("unwritable data dir: wakeups in 2 s idle = %llu, snapshotsWritten = %llu, CPU = %.0f ms (user+kernel) over ~2 s\n",
              s.wakeups - w0, s.snapshotsWritten, (t(u1) - t(u0)) + (t(k1) - t(k0)));
  ix.stop();
  return 0;
}

int modeOverflow(const fs::path& dir, int files, int probeSleepUs) {
  fs::remove_all(dir);
  fs::create_directories(dir / "place" / "A");
  IndexerConfig cfg;  // no persistence
  cfg.places = {{std::string(reinterpret_cast<const char*>((dir / "place").u8string().c_str())), PlaceKind::Folder, "P"}};
  cfg.probe = [probeSleepUs](const std::string&, const PlaceSpec&, std::string_view, EntryRecord&) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(probeSleepUs);
    while (std::chrono::steady_clock::now() < until) {}  // busy: Sleep(1) is ~15 ms here
  };
  Indexer ix(cfg);
  ix.start();
  ix.waitIdle(std::chrono::seconds(10));
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < files; ++i) {
    const std::wstring n = (dir / "place" / "A" / ("a_rather_long_file_name_to_fill_the_change_buffer_quickly_" + std::to_string(i) + ".pqp")).wstring();
    HANDLE h = CreateFileW(n.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    CloseHandle(h);
  }
  uint32_t n = 0;
  for (int k = 0; k < 1200; ++k) {
    n = ix.index()->entryCount();
    if (n == static_cast<uint32_t>(files)) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  const IndexerStats s = ix.stats();
  std::printf("overflow: %d files created; index has %u after %.0f ms; overflows=%llu rebuilds=%llu\n", files, n, ms,
              s.overflows, s.rebuilds);
  ix.stop();
  fs::remove_all(dir);
  return n == static_cast<uint32_t>(files) ? 0 : 1;
}

int modeTagsMismatch() {
  // EntryTags built for a 1-entry index, used with a 2M-entry index (what BrowserPanel::onLibrary can pair up when
  // the indexer publishes between lib_.index() and lib_.entryTags()).
  Index tiny;
  const uint32_t d0 = tiny.addDir(Index::kNone, "", 0, 0);
  EntryRecord r;
  r.name = "x.wav";
  tiny.addEntry(d0, r);
  tiny.places = {{"C:\\x", PlaceKind::Folder, "x"}};
  TagStore ts;
  ts.setCollection(tagKeyOf(tiny, 0), 1, true);
  auto tags = buildEntryTags(tiny, ts);
  auto big = std::make_shared<Index>();
  big->places = tiny.places;
  const uint32_t d = big->addDir(Index::kNone, "", 0, 0);
  for (uint32_t i = 0; i < 2000000; ++i) {
    r.name = "s" + std::to_string(i) + ".wav";
    big->addEntry(d, r);
  }
  std::printf("tags->combo.size() = %zu, index entries = %u\n", tags->combo.size(), big->entryCount());
  std::fflush(stdout);
  SearchQuery q;
  q.text = "s1";
  q.filter.collections = 2;
  SearchResult res = search(big, tags, q);
  std::printf("search returned (no crash): total %u\n", res.total);
  return 0;
}

// Brute force from search.h's definition.
bool refSep(char c) { return std::string_view(" _-.,+&()[]{}/").find(c) != std::string_view::npos; }
int32_t refBonus(std::string_view s, std::size_t j) {
  if (j == 0) return kScoreNameStart;
  auto lo = [](char c) { return c >= 'a' && c <= 'z'; };
  auto up = [](char c) { return c >= 'A' && c <= 'Z'; };
  auto dg = [](char c) { return c >= '0' && c <= '9'; };
  const char p = s[j - 1], c = s[j];
  const bool st = refSep(p) || (lo(p) && up(c)) || ((lo(p) || up(p)) && dg(c)) || (dg(p) && (lo(c) || up(c)));
  return st ? kScoreWordStart : 0;
}
void enumerate(std::string_view t, std::string_view s, std::size_t i, std::size_t prev, int32_t acc, int32_t& best) {
  for (std::size_t j = i == 0 ? 0 : prev + 1; j < s.size(); ++j) {
    if (foldChar(s[j]) != t[i]) continue;
    int32_t sc = acc + kScoreMatch + refBonus(s, j);
    if (i > 0) sc += j == prev + 1 ? kScoreConsecutive : -kScoreGap * static_cast<int32_t>(j - prev - 1);
    if (i + 1 == t.size()) best = std::max(best, sc);
    else enumerate(t, s, i + 1, j, sc, best);
  }
}

int modeDp(uint32_t iters, uint32_t seed) {
  std::mt19937 rng(seed);
  const std::string alpha = "aAbB1 _-.\xC3\xA9xX9(";
  std::vector<int32_t> scratch;
  uint32_t mismatches = 0, matched = 0;
  for (uint32_t i = 0; i < iters; ++i) {
    std::string s, t;
    const int n = 1 + rng() % 14, m = 1 + rng() % 5;
    for (int k = 0; k < n; ++k) s.push_back(alpha[rng() % alpha.size()]);
    for (int k = 0; k < m; ++k) t.push_back(foldChar(alpha[rng() % alpha.size()]));
    int32_t b = kNoMatch;
    enumerate(t, s, 0, 0, 0, b);
    const int32_t d = fuzzyScore(t, s, scratch);
    if (b != kNoMatch) ++matched;
    if (b != d && ++mismatches <= 5) std::printf("DP mismatch: term '%s' in '%s': dp %d brute %d\n", t.c_str(), s.c_str(), d, b);
  }
  std::printf("dp: %u cases, %u matched, %u mismatches\n", iters, matched, mismatches);
  return mismatches ? 1 : 0;
}

int modeFocus(const fs::path& dir) {
  // Every place is watched; rescanUnwatched() (the app calls it on each window focus) should have nothing to do.
  fs::remove_all(dir);
  fs::create_directories(dir / "place");
  for (int d = 0; d < 50; ++d) {
    fs::create_directories(dir / "place" / ("D" + std::to_string(d)));
    for (int i = 0; i < 400; ++i) std::ofstream(dir / "place" / ("D" + std::to_string(d)) / ("f" + std::to_string(i) + ".pqp")) << "x";
  }
  IndexerConfig cfg;
  cfg.places = {{std::string(reinterpret_cast<const char*>((dir / "place").u8string().c_str())), PlaceKind::Folder, "P"}};
  Indexer ix(cfg);
  ix.start();
  ix.waitIdle(std::chrono::seconds(30));
  const IndexerStats a = ix.stats();
  for (int k = 0; k < 10; ++k) {
    ix.rescanUnwatched();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ix.waitIdle(std::chrono::seconds(10));
  }
  const IndexerStats b = ix.stats();
  std::printf("watched places %u, entries %u; 10 x rescanUnwatched: rebuilds +%llu, last scan dirsCopied %u dirsListed %u, wakeups +%llu, generation %llu\n",
              b.watchedPlaces, ix.index()->entryCount(), b.rebuilds - a.rebuilds, b.lastScan.dirsCopied, b.lastScan.dirsListed,
              b.wakeups - a.wakeups, ix.generation());
  ix.stop();
  fs::remove_all(dir);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) return 64;
  const std::string mode = argv[1];
  if (mode == "fuzz" && argc >= 5) return modeFuzz(argv[2], std::stoul(argv[3]), std::stoul(argv[4]));
  if (mode == "header" && argc >= 3) return modeHeader(argv[2]);
  if (mode == "indexer" && argc >= 4) return modeIndexer(argv[2], argv[3]);
  if (mode == "spin" && argc >= 3) return modeSpin(argv[2]);
  if (mode == "overflow" && argc >= 4) return modeOverflow(argv[2], std::stoi(argv[3]), argc >= 5 ? std::stoi(argv[4]) : 0);
  if (mode == "tagsmismatch") return modeTagsMismatch();
  if (mode == "focus" && argc >= 3) return modeFocus(argv[2]);
  if (mode == "dp" && argc >= 4) return modeDp(std::stoul(argv[2]), std::stoul(argv[3]));
  return 64;
}
