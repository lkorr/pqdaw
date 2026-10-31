// T5.9: the indexer thread: load + revalidate at start, the change watcher (create / rename / delete / modify, moved-in
// directories), no wakeups while idle, persistence (snapshot + journal, crash state), latency (`perf`).

#include <doctest/doctest.h>

#include <chrono>
#include <fstream>
#include <thread>

#include "library_test_util.h"
#include "pq/library/index_file.h"
#include "pq/library/indexer.h"

using namespace pq;
using namespace pq::library;
using namespace pq::library::test;
using namespace std::chrono_literals;

namespace {

uint32_t entryNamed(const Index& idx, std::string_view name) {
  for (uint32_t e = 0; e < idx.entryCount(); ++e)
    if (idx.name(e) == name) return e;
  return Index::kNone;
}

// Waits until `pred(index)` holds (or the timeout passes); returns the elapsed milliseconds (≥ timeout: failed).
template <class Pred>
double waitUntil(const Indexer& ix, Pred pred, std::chrono::milliseconds timeout,
                 std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now()) {
  for (;;) {
    const uint64_t gen = ix.generation();
    if (pred(*ix.index())) return msSince(t0);
    if (msSince(t0) >= static_cast<double>(timeout.count())) return static_cast<double>(timeout.count());
    ix.waitForGeneration(gen, 50ms);
  }
}

IndexerConfig configFor(const TempTree& tree, std::vector<PlaceSpec> places) {
  IndexerConfig c;
  c.dataDir = tree.root() / "data";
  c.places = std::move(places);
  return c;
}

}  // namespace

TEST_CASE("library: indexer start, watcher, idle, persistence") {
  TempTree tree("indexer");
  const fs::path lib = tree.root() / "lib";
  for (int i = 0; i < 12; ++i) writeWav(lib / "A" / ("a" + std::to_string(i) + ".wav"), 100 + i);
  writeText(lib / "B" / "p.pqp", "patch P { out o = 0 }");
  const std::vector<PlaceSpec> places = {{utf8(lib), PlaceKind::Folder, "Lib"}};

  IndexerConfig cfg = configFor(tree, places);
  cfg.snapshotIntervalMs = 60000;  // the journal path: the first snapshot is immediate, later changes are journaled
  Indexer ix(cfg);
  std::atomic<uint64_t> callbacks{0};
  ix.setOnChange([&](uint64_t) { ++callbacks; });
  ix.start();
  REQUIRE(ix.waitIdle(10s));
  CHECK(ix.index()->entryCount() == 13);
  CHECK(ix.generation() == 1);  // nothing on disk: one publish after the scan
  CHECK(callbacks == 1);
  IndexerStats st = ix.stats();
  CHECK(st.watchedPlaces == 1);
  CHECK(st.snapshotsWritten == 1);
  CHECK(fs::exists(tree.root() / "data" / "index.pqi"));
  MESSAGE("start: load " << st.loadMs << " ms, revalidate " << st.revalidateMs << " ms");

  // Idle: no wakeups at all while nothing changes.
  {
    const uint64_t before = ix.stats().wakeups;
    std::this_thread::sleep_for(1s);
    CHECK(ix.stats().wakeups == before);
    CHECK(ix.stats().idle);
  }

  // Create.
  writeWav(lib / "A" / "new.wav", 77);
  double ms = waitUntil(ix, [](const Index& i) { return entryNamed(i, "new.wav") != Index::kNone; }, 5000ms);
  CHECK(ms < 5000);
  MESSAGE("create reflected after " << ms << " ms");
  {
    auto idx = ix.index();
    CHECK(idx->entries.frames[entryNamed(*idx, "new.wav")] == 77);
  }
  // Rename (file ID kept, nothing probed).
  const FileIdBytes id = [&] {
    auto idx = ix.index();
    return idx->entries.fileId[entryNamed(*idx, "a5.wav")];
  }();
  fs::rename(lib / "A" / "a5.wav", lib / "A" / "renamed.wav");
  ms = waitUntil(ix, [](const Index& i) {
    return entryNamed(i, "renamed.wav") != Index::kNone && entryNamed(i, "a5.wav") == Index::kNone;
  }, 5000ms);
  CHECK(ms < 5000);
  MESSAGE("rename reflected after " << ms << " ms");
  {
    auto idx = ix.index();
    CHECK(idx->entries.fileId[entryNamed(*idx, "renamed.wav")] == id);
    CHECK(ix.stats().lastScan.filesProbed == 0);
  }
  // Modify.
  writeWav(lib / "A" / "a1.wav", 5000);
  ms = waitUntil(ix, [](const Index& i) {
    const uint32_t e = entryNamed(i, "a1.wav");
    return e != Index::kNone && i.entries.frames[e] == 5000;
  }, 5000ms);
  CHECK(ms < 5000);
  // Delete.
  fs::remove(lib / "A" / "a2.wav");
  ms = waitUntil(ix, [](const Index& i) { return entryNamed(i, "a2.wav") == Index::kNone; }, 5000ms);
  CHECK(ms < 5000);
  // A directory moved in from outside the place: its contents appear (the move reports only the directory).
  writeWav(tree.root() / "outside" / "Sub" / "moved.wav", 10);
  fs::rename(tree.root() / "outside", lib / "B" / "Moved");
  ms = waitUntil(ix, [](const Index& i) { return entryNamed(i, "moved.wav") != Index::kNone; }, 5000ms);
  CHECK(ms < 5000);
  // A directory deleted with its contents.
  fs::remove_all(lib / "B" / "Moved");
  ms = waitUntil(ix, [](const Index& i) { return entryNamed(i, "moved.wav") == Index::kNone; }, 5000ms);
  CHECK(ms < 5000);

  // Back to idle: the journal holds the changes (the snapshot interval has not passed). NTFS may deliver the last
  // notifications of the directory delete a little later (a real change, handled like any other), so first wait
  // until the counter has been still for 300 ms; from then on an idle indexer must not wake at all.
  REQUIRE(ix.waitIdle(5s));
  for (int i = 0; i < 20; ++i) {
    const uint64_t w = ix.stats().wakeups;
    std::this_thread::sleep_for(300ms);
    if (ix.waitIdle(5s) && ix.stats().wakeups == w) break;
  }
  st = ix.stats();
  CHECK(st.idle);
  CHECK(st.journalAppends >= 1);
  CHECK(st.snapshotsWritten == 1);
  {
    const uint64_t before = st.wakeups;
    std::this_thread::sleep_for(1s);
    CHECK(ix.stats().wakeups == before);
  }

  // The crash state (snapshot + journal as they are on disk now) loads to the live index.
  {
    const fs::path crash = tree.root() / "crash";
    fs::create_directories(crash);
    fs::copy_file(tree.root() / "data" / "index.pqi", crash / "index.pqi");
    fs::copy_file(tree.root() / "data" / "index.pqj", crash / "index.pqj");
    auto loaded = loadIndex(crash / "index.pqi");
    REQUIRE(loaded);
    const Journal::Contents j = Journal::read(crash / "index.pqj", loaded->sequence);
    CHECK_FALSE(j.changes.empty());
    auto replayed = applyChanges(*loaded->index, j.changes);
    CHECK(rows(*replayed) == rows(*ix.index()));
  }

  // stop() writes the snapshot; a restart loads it and publishes it at once, and the revalidation finds nothing.
  const auto finalRows = rows(*ix.index());
  ix.stop();
  CHECK(ix.stats().snapshotsWritten == 2);
  {
    Indexer again(configFor(tree, places));
    again.start();
    REQUIRE(again.waitIdle(10s));
    CHECK(again.generation() == 1);
    CHECK(rows(*again.index()) == finalRows);
    CHECK_FALSE(again.stats().lastScan.changed);
    CHECK(again.stats().lastScan.filesProbed == 0);
    MESSAGE("restart: load " << again.stats().loadMs << " ms, revalidate " << again.stats().revalidateMs << " ms");
  }
}

// M5 review M14: when the snapshot cannot be written (here the data folder's path runs through a file), the indexer
// retried at once in a loop: ~2700 wakeups and a full core in 2 s. It now backs off (1 s, doubling to 60 s).
// M5 review H8: a corrupt index.pqi is deleted at start (it was met again on every launch).
TEST_CASE("library: indexer backs off when the snapshot cannot be written; a corrupt snapshot is deleted") {
  TempTree tree("indexer_spin");
  const fs::path lib = tree.root() / "lib";
  for (int i = 0; i < 20; ++i) writeWav(lib / ("s" + std::to_string(i) + ".wav"), 50 + i);
  writeText(tree.root() / "file", "a file, not a folder");
  {
    IndexerConfig cfg;
    cfg.dataDir = tree.root() / "file" / "data";  // cannot be created
    cfg.places = {{utf8(lib), PlaceKind::Folder, "Lib"}};
    cfg.snapshotIntervalMs = 100;
    Indexer ix(cfg);
    ix.start();
    REQUIRE(ix.waitIdle(10s));
    const uint64_t w0 = ix.stats().wakeups;
    std::this_thread::sleep_for(2s);
    const IndexerStats st = ix.stats();
    CHECK(st.snapshotsWritten == 0);
    CHECK(st.snapshotFailures >= 1);
    CHECK(st.snapshotFailures <= 3);       // at most the first try and two back-off retries (1 s, 2 s)
    CHECK(st.wakeups - w0 <= 4);
    ix.stop();
  }
  // A snapshot whose header was corrupted: refused, deleted, and the index rebuilt from the place.
  {
    IndexerConfig cfg;
    cfg.dataDir = tree.root() / "data";
    cfg.places = {{utf8(lib), PlaceKind::Folder, "Lib"}};
    {
      Indexer ix(cfg);
      ix.start();
      REQUIRE(ix.waitIdle(10s));
      ix.stop();
    }
    const fs::path pqi = cfg.dataDir / "index.pqi";
    REQUIRE(fs::exists(pqi));
    {
      std::fstream f(pqi, std::ios::binary | std::ios::in | std::ios::out);
      f.seekp(32);  // the entry count
      const uint32_t huge = 0x40000000u;
      f.write(reinterpret_cast<const char*>(&huge), 4);
    }
    CHECK_FALSE(loadIndex(pqi));
    Indexer ix(cfg);
    ix.start();
    REQUIRE(ix.waitIdle(10s));
    CHECK(ix.index()->entryCount() == 20);
    ix.stop();
    CHECK(loadIndex(pqi));  // written again, valid
  }
}

TEST_CASE("library: indexer place changes and rescans") {
  TempTree tree("places");
  const fs::path a = tree.root() / "a";
  const fs::path b = tree.root() / "b";
  writeWav(a / "one.wav", 10);
  writeWav(b / "two.wav", 10);
  IndexerConfig cfg;  // no persistence
  cfg.places = {{utf8(a), PlaceKind::Folder, "A"}};
  Indexer ix(cfg);
  ix.start();
  REQUIRE(ix.waitIdle(10s));
  CHECK(ix.index()->entryCount() == 1);

  ix.setPlaces({{utf8(a), PlaceKind::Folder, "A"}, {utf8(b), PlaceKind::Project, "B"}});
  double ms = waitUntil(ix, [](const Index& i) { return i.entryCount() == 2; }, 5000ms);
  CHECK(ms < 5000);
  REQUIRE(ix.waitIdle(5s));
  CHECK(ix.stats().watchedPlaces == 2);
  CHECK(ix.index()->places.size() == 2);

  // The second place is watched too.
  writeWav(b / "three.wav", 10);
  ms = waitUntil(ix, [](const Index& i) { return entryNamed(i, "three.wav") != Index::kNone; }, 5000ms);
  CHECK(ms < 5000);

  ix.setPlaces({{utf8(b), PlaceKind::Project, "B"}});
  ms = waitUntil(ix, [](const Index& i) { return i.places.size() == 1 && i.entryCount() == 2; }, 5000ms);
  CHECK(ms < 5000);

  // rescanAll finds a change made behind the watcher's back... which the watcher also sees; either way it lands.
  REQUIRE(ix.waitIdle(5s));
  const uint64_t gen = ix.generation();
  ix.rescanAll();
  REQUIRE(ix.waitIdle(5s));
  CHECK(ix.generation() == gen);  // nothing changed: nothing published
  ix.stop();
}

TEST_CASE("library: watcher latency <= 500 ms (create / rename / delete / modify)" * doctest::test_suite("perf")) {
  TempTree tree("latency");
  const fs::path lib = tree.root() / "lib";
  for (int i = 0; i < 2000; ++i)
    writeWav(lib / ("D" + std::to_string(i / 100)) / ("s" + std::to_string(i) + ".wav"), 64);
  IndexerConfig cfg = configFor(tree, {{utf8(lib), PlaceKind::Folder, "Lib"}});
  Indexer ix(cfg);
  ix.start();
  REQUIRE(ix.waitIdle(30s));

  for (int round = 0; round < 3; ++round) {
    const std::string n = "late" + std::to_string(round);
    auto t0 = std::chrono::steady_clock::now();
    writeWav(lib / "D3" / (n + ".wav"), 99);
    double create = waitUntil(ix, [&](const Index& i) { return entryNamed(i, n + ".wav") != Index::kNone; }, 2000ms, t0);
    t0 = std::chrono::steady_clock::now();
    fs::rename(lib / "D3" / (n + ".wav"), lib / "D4" / (n + "_moved.wav"));
    double rename = waitUntil(ix, [&](const Index& i) {
      return entryNamed(i, n + "_moved.wav") != Index::kNone && entryNamed(i, n + ".wav") == Index::kNone;
    }, 2000ms, t0);
    t0 = std::chrono::steady_clock::now();
    writeWav(lib / "D4" / (n + "_moved.wav"), 1234);
    double modify = waitUntil(ix, [&](const Index& i) {
      const uint32_t e = entryNamed(i, n + "_moved.wav");
      return e != Index::kNone && i.entries.frames[e] == 1234;
    }, 2000ms, t0);
    t0 = std::chrono::steady_clock::now();
    fs::remove(lib / "D4" / (n + "_moved.wav"));
    double del = waitUntil(ix, [&](const Index& i) { return entryNamed(i, n + "_moved.wav") == Index::kNone; },
                           2000ms, t0);
    MESSAGE("round " << round << ": create " << create << " ms, rename " << rename << " ms, modify " << modify
                     << " ms, delete " << del << " ms");
    CHECK(create <= 500);
    CHECK(rename <= 500);
    CHECK(modify <= 500);
    CHECK(del <= 500);
  }
  ix.stop();
}
