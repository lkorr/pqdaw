// T5.x-index-progressive: the first scan of a large folder publishes its tree before any probe finishes, the probe pass
// publishes its results in increments and reports progress, unprobed entries survive revalidations, renames and a
// restart, the indexing probe never reads past an MP3's headers, and odd names (a trailing space, Greek) index.

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include "library_test_util.h"
#include "pq/library/index_file.h"
#include "pq/library/indexer.h"
#include "pq/library/scan.h"
#include "pq/library/search.h"

using namespace pq;
using namespace pq::library;
using namespace pq::library::test;
using namespace std::chrono_literals;

namespace {

uint32_t unprobedCount(const Index& idx) {
  return static_cast<uint32_t>(std::count_if(idx.entries.flags.begin(), idx.entries.flags.end(),
                                             [](uint8_t f) { return (f & kFlagUnprobed) != 0; }));
}

uint32_t entryNamed(const Index& idx, std::string_view name) {
  for (uint32_t e = 0; e < idx.entryCount(); ++e)
    if (idx.name(e) == name) return e;
  return Index::kNone;
}

// A probe that takes `ms` per file and records every call (thread-safe: the probe pass runs it on several threads).
struct SlowProbe {
  std::chrono::milliseconds delay;
  std::atomic<uint32_t> calls{0};
  std::atomic<uint32_t> inFlight{0}, maxInFlight{0};
  std::mutex mutex;
  std::vector<std::string> paths;

  ProbeFn fn() {
    return [this](const std::string& path, const PlaceSpec& place, std::string_view rel, EntryRecord& r) {
      const uint32_t now = ++inFlight;
      for (uint32_t m = maxInFlight.load(); now > m && !maxInFlight.compare_exchange_weak(m, now);) {
      }
      std::this_thread::sleep_for(delay);
      r.frames = 4800;
      r.rate = 48000;
      r.channels = 2;
      r.category = categorize(r.kind, place.kind, rel, r.name, Category::None);
      {
        std::lock_guard lock(mutex);
        paths.push_back(path);
      }
      ++calls;
      --inFlight;
    };
  }
};

void makeTree(const fs::path& lib, int files) {
  // Nested folders: A, A\B, A\B\C (each with files) and an empty D\E.
  for (int i = 0; i < files; ++i) {
    const char* dir = i % 3 == 0 ? "A" : i % 3 == 1 ? "A\\B" : "A\\B\\C";
    writeWav(lib / dir / ("s" + std::to_string(i) + ".wav"), 16);
  }
  fs::create_directories(lib / "D" / "E");
}

}  // namespace

TEST_CASE("library: a first scan publishes the tree before the probes; increments; progress") {
  TempTree tree("progressive");
  const fs::path lib = tree.root() / "lib";
  constexpr int kFiles = 60;
  makeTree(lib, kFiles);

  SlowProbe slow{20ms};
  IndexerConfig cfg;  // no persistence
  cfg.places = {{utf8(lib), PlaceKind::Folder, "Lib"}};
  cfg.probe = slow.fn();
  cfg.probeBudgetMs = 0;  // defer every probe
  cfg.probePublishMs = 100;
  cfg.probePublishFiles = 1000;
  cfg.probeThreads = 3;
  cfg.backgroundPriority = false;
  Indexer ix(cfg);
  struct Seen {
    uint64_t gen;
    uint32_t dirs, entries, unprobed;
  };
  std::mutex seenMutex;
  std::vector<Seen> seen;
  ix.setOnChange([&](uint64_t gen) {
    const auto idx = ix.index();
    std::lock_guard lock(seenMutex);
    seen.push_back({gen, idx->dirCount(), idx->entryCount(), unprobedCount(*idx)});
  });
  CHECK(ix.progress().phase == IndexerProgress::Phase::Idle);  // not started
  ix.start();

  // The first publish: every folder (nested ones and the empty ones too) and every file, before the probes are done.
  REQUIRE(ix.waitForGeneration(0, 5s) >= 1);
  {
    const uint32_t callsAtFirst = slow.calls.load();
    std::lock_guard lock(seenMutex);
    REQUIRE_FALSE(seen.empty());
    CHECK(seen[0].gen == 1);
    CHECK(seen[0].entries == kFiles);
    CHECK(seen[0].dirs == 6);  // root, A, A\B, A\B\C, D, D\E
    CHECK(seen[0].unprobed == kFiles);
    CHECK(callsAtFirst < kFiles);
  }
  {
    const auto first = ix.index();
    CHECK(first->findDir(0, "A\\B\\C") != Index::kNone);
    CHECK(first->findDir(0, "D\\E") != Index::kNone);
    const uint32_t e = entryNamed(*first, "s0.wav");
    REQUIRE(e != Index::kNone);
    if (unprobedCount(*first) == kFiles) {
      CHECK(first->category(e) == Category::Samples);  // from the path alone
      CHECK(first->entries.rate[e] == 0);
      CHECK(first->seconds(e) == 0.0);
      // A duration filter leaves unprobed entries out (they have no duration); a name search finds them.
      SearchQuery q;
      q.filter.minSeconds = 0.01;
      CHECK(search(first, nullptr, q).total == 0);
      CHECK(search(first, nullptr, SearchQuery{"s0", {}}).total >= 1);
    }
  }

  // Progress while probing: the pass's total and a count that only grows.
  uint32_t lastProbed = 0;
  bool sawProbing = false, monotonic = true, totalRight = true;
  for (auto t0 = std::chrono::steady_clock::now(); msSince(t0) < 10000;) {
    const IndexerProgress p = ix.progress();
    if (p.phase == IndexerProgress::Phase::Probing) {
      sawProbing = true;
      totalRight = totalRight && p.filesToProbe == kFiles && p.placeName == "Lib" && p.filesListed == kFiles;
      monotonic = monotonic && p.filesProbed >= lastProbed && p.filesProbed <= kFiles;
      lastProbed = p.filesProbed;
    }
    if (ix.stats().idle) break;
    std::this_thread::sleep_for(5ms);
  }
  REQUIRE(ix.waitIdle(10s));
  CHECK(sawProbing);
  CHECK(monotonic);
  CHECK(totalRight);
  CHECK(ix.progress().phase == IndexerProgress::Phase::Idle);

  // Every file probed exactly once, on several threads; the results came in several publishes.
  CHECK(slow.calls == kFiles);
  {
    std::lock_guard lock(slow.mutex);
    std::vector<std::string> p = slow.paths;
    std::sort(p.begin(), p.end());
    CHECK(std::unique(p.begin(), p.end()) == p.end());
  }
  CHECK(slow.maxInFlight >= 2);
  const IndexerStats st = ix.stats();
  CHECK(st.filesProbedLater == kFiles);
  CHECK(st.probePublishes >= 2);
  {
    std::lock_guard lock(seenMutex);
    CHECK(seen.size() >= 3);
    for (size_t i = 1; i < seen.size(); ++i) CHECK(seen[i].unprobed <= seen[i - 1].unprobed);
    CHECK(seen.back().unprobed == 0);
  }
  const auto done = ix.index();
  CHECK(unprobedCount(*done) == 0);
  CHECK(std::all_of(done->entries.rate.begin(), done->entries.rate.end(), [](uint32_t r) { return r == 48000; }));
  SearchQuery q;
  q.filter.minSeconds = 0.01;
  CHECK(search(done, nullptr, q).total == kFiles);

  // Idle again: no wakeups (the pool's threads wait without a timeout too).
  const uint64_t w = ix.stats().wakeups;
  std::this_thread::sleep_for(500ms);
  CHECK(ix.stats().wakeups == w);
  ix.stop();
}

TEST_CASE("library: unprobed entries survive revalidation, renames and a restart; nothing is probed twice") {
  TempTree tree("progressive_restart");
  const fs::path lib = tree.root() / "lib";
  constexpr int kFiles = 40;
  makeTree(lib, kFiles);
  const std::vector<PlaceSpec> places = {{utf8(lib), PlaceKind::Folder, "Lib"}};

  // Revalidation and a rename at the scan level: unprobed records are reused as they are (no probe).
  {
    ScanRequest req;
    req.places = places;
    req.probeBudgetMs = 0;
    DiskSource disk;
    SlowProbe none{0ms};
    ScanResult first = rebuild(nullptr, req, disk, none.fn());
    CHECK(first.stats.filesDeferred == kFiles);
    CHECK(first.stats.filesProbed == 0);
    CHECK(unprobedCount(*first.index) == kFiles);
    const auto oldId = first.index->entries.fileId[entryNamed(*first.index, "s1.wav")];
    fs::rename(lib / "A" / "B" / "s1.wav", lib / "A" / "renamed.wav");
    ScanRequest again;
    again.places = places;  // everything relisted, probes inline (no budget)
    ScanResult second = rebuild(first.index.get(), again, disk, none.fn());
    CHECK(second.stats.filesProbed == 0);
    CHECK(second.stats.filesMoved == 1);
    CHECK(unprobedCount(*second.index) == kFiles);
    const uint32_t r = entryNamed(*second.index, "renamed.wav");
    REQUIRE(r != Index::kNone);
    CHECK(second.index->entries.fileId[r] == oldId);
    CHECK((second.index->entries.flags[r] & kFlagUnprobed) != 0);
    fs::rename(lib / "A" / "renamed.wav", lib / "A" / "B" / "s1.wav");
  }

  // The indexer stopped in the middle of the pass: the snapshot holds what was probed; a restart probes only the rest.
  IndexerConfig cfg;
  cfg.dataDir = tree.root() / "data";
  cfg.places = places;
  cfg.probeBudgetMs = 0;
  cfg.probeThreads = 1;
  cfg.probePublishMs = 50;
  cfg.backgroundPriority = false;
  SlowProbe slow{25ms};
  cfg.probe = slow.fn();
  uint32_t leftAtStop = 0;
  {
    Indexer ix(cfg);
    ix.start();
    REQUIRE(ix.waitForGeneration(0, 5s) >= 1);
    for (auto t0 = std::chrono::steady_clock::now(); slow.calls < 8 && msSince(t0) < 5000;) std::this_thread::sleep_for(2ms);
    // A rename while the pass runs: matched by file ID, still probed once.
    fs::rename(lib / "A" / "B" / "s4.wav", lib / "A" / "B" / "s4 moved.wav");
    const double ms = [&] {
      const auto t0 = std::chrono::steady_clock::now();
      for (;;) {
        const auto idx = ix.index();
        if (entryNamed(*idx, "s4 moved.wav") != Index::kNone) return msSince(t0);
        if (msSince(t0) > 5000) return 5000.0;
        ix.waitForGeneration(ix.generation(), 50ms);
      }
    }();
    CHECK(ms < 5000);
    ix.stop();
    auto loaded = loadIndex(cfg.dataDir / "index.pqi");
    REQUIRE(loaded);
    CHECK(loaded->index->entryCount() == kFiles);
    leftAtStop = unprobedCount(*loaded->index);
    CHECK(leftAtStop > 0);
    CHECK(leftAtStop < kFiles);
    CHECK(slow.calls == kFiles - leftAtStop);  // what was probed is in the snapshot
  }
  SlowProbe fast{0ms};
  cfg.probe = fast.fn();
  {
    Indexer ix(cfg);
    ix.start();
    REQUIRE(ix.waitIdle(10s));
    CHECK(fast.calls == leftAtStop);
    CHECK(unprobedCount(*ix.index()) == 0);
    CHECK(entryNamed(*ix.index(), "s4 moved.wav") != Index::kNone);
    ix.stop();
  }
  CHECK(slow.calls + fast.calls == kFiles);
}

namespace {

// MPEG-1 layer III, 128 kb/s, 44.1 kHz, stereo frames (417 bytes, no padding).
std::vector<uint8_t> mp3Bytes(std::size_t frames, bool bogusXing) {
  std::vector<uint8_t> b(frames * 417);
  for (std::size_t f = 0; f < frames; ++f) {
    b[f * 417] = 0xFF;
    b[f * 417 + 1] = 0xFB;
    b[f * 417 + 2] = 0x90;
    b[f * 417 + 3] = 0x00;
  }
  if (bogusXing) {
    std::memcpy(b.data() + 36, "Xing", 4);
    b[43] = 1;
    std::memset(b.data() + 44, 0xFF, 4);
  }
  return b;
}

std::wstring longPath(const fs::path& p) { return L"\\\\?\\" + p.wstring(); }

}  // namespace

TEST_CASE("library: a trailing-space folder, a Greek file name and MP3s without a usable frame count index") {
  TempTree tree("odd_names");
  const fs::path lib = tree.root() / "lib";
  writeWav(lib / "plain.wav", 100);
  // "7 " (a trailing space): plain Win32 paths cannot create or open it (the space is stripped); \\?\ paths can.
  const fs::path spaced = lib / "komovari64" / L"7 ";
  REQUIRE(CreateDirectoryW(longPath(lib / "komovari64").c_str(), nullptr) != 0);
  REQUIRE(CreateDirectoryW(longPath(spaced).c_str(), nullptr) != 0);
  {
    const std::vector<uint8_t> wav = wavBytes(480);
    HANDLE h = CreateFileW(longPath(spaced / L"in space.wav").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(h != INVALID_HANDLE_VALUE);
    DWORD w = 0;
    WriteFile(h, wav.data(), static_cast<DWORD>(wav.size()), &w, nullptr);
    CloseHandle(h);
  }
  writeWav(lib / L"\u039a\u03bf\u03c5\u03c6\u03bf\u03bd\u03ae\u03c3\u03b9 \u516b.wav", 200);  // path::string() threw on it
  {
    const std::vector<uint8_t> cbr = mp3Bytes(300, false);
    writeBytes(lib / "cbr.mp3", cbr.data(), cbr.size());
    const std::vector<uint8_t> bogus = mp3Bytes(600, true);
    writeBytes(lib / "bogus.mp3", bogus.data(), bogus.size());
  }

  IndexerConfig cfg;
  cfg.places = {{utf8(lib), PlaceKind::Folder, "Lib"}};
  Indexer ix(cfg);  // the real probe
  ix.start();
  REQUIRE(ix.waitIdle(10s));
  const auto idx = ix.index();
  CHECK(idx->entryCount() == 5);
  CHECK(unprobedCount(*idx) == 0);
  CHECK(ix.stats().lastScan.listFailures == 0);
  CHECK(idx->findDir(0, "komovari64\\7 ") != Index::kNone);
  const uint32_t inSpace = entryNamed(*idx, "in space.wav");
  REQUIRE(inSpace != Index::kNone);
  CHECK(idx->entries.frames[inSpace] == 480);
  CHECK(idx->path(inSpace).find("komovari64\\7 \\in space.wav") != std::string::npos);
  const uint32_t greek = entryNamed(*idx, "\xCE\x9A\xCE\xBF\xCF\x85\xCF\x86\xCE\xBF\xCE\xBD\xCE\xAE\xCF\x83\xCE\xB9 \xE5\x85\xAB.wav");
  REQUIRE(greek != Index::kNone);
  CHECK(idx->entries.frames[greek] == 200);
  CHECK((idx->entries.flags[greek] & kFlagProbeFailed) == 0);
  // MP3s without a frame count (none / an implausible Xing count): estimated from the bitrate, never scanned.
  for (const char* n : {"cbr.mp3", "bogus.mp3"}) {
    const uint32_t e = entryNamed(*idx, n);
    REQUIRE(e != Index::kNone);
    const uint8_t f = idx->entries.flags[e];
    CHECK((f & kFlagLengthEstimated) != 0);
    CHECK((f & kFlagFramesScanned) == 0);
    CHECK(idx->entries.rate[e] == 44100);
    const double frames = std::string_view(n) == "cbr.mp3" ? 300 * 1152 : 599 * 1152;
    CHECK(static_cast<double>(idx->entries.frames[e]) == doctest::Approx(frames).epsilon(0.003));
  }
  ix.stop();
  // TempTree's remove_all cannot delete "7 " through a plain path.
  DeleteFileW(longPath(spaced / L"in space.wav").c_str());
  RemoveDirectoryW(longPath(spaced).c_str());
}
