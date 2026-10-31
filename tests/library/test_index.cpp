// T5.9: index building (initial scan, revalidation, watcher-style relists, renames), the index file and its journal,
// and the index-speed criteria on synthetic trees (2k files here, 200k in `slow`).

#include <doctest/doctest.h>

#include <cstring>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <thread>

#include "library_test_util.h"
#include "pq/library/index.h"
#include "pq/library/index_file.h"
#include "pq/library/scan.h"

using namespace pq;
using namespace pq::library;
using namespace pq::library::test;

namespace {

// Builds a full scan of the given places.
ScanResult fullScan(const Index* old, const std::vector<PlaceSpec>& places, const ProbeFn& probe = {}) {
  ScanRequest req;
  req.places = places;
  req.dirty.resize(places.size());
  for (auto& d : req.dirty) d.full = true;
  DiskSource disk;
  return rebuild(old, req, disk, probe);
}

ScanResult dirtyScan(const Index& old, const std::vector<PlaceSpec>& places, std::vector<std::string> dirs,
                     std::vector<std::string> recursive = {}) {
  ScanRequest req;
  req.places = places;
  req.dirty.resize(places.size());
  req.dirty[0].dirs = std::move(dirs);
  req.dirty[0].recursive = std::move(recursive);
  DiskSource disk;
  return rebuild(&old, req, disk, {});
}

uint32_t entryNamed(const Index& idx, std::string_view name) {
  for (uint32_t e = 0; e < idx.entryCount(); ++e)
    if (idx.name(e) == name) return e;
  return Index::kNone;
}

// A synthetic sample library: `files` files in Pack<p>\Folder<f> directories of 100 files; 85 % WAV (64 frames),
// 5 % presets, 4 % patches, 3 % Scala tunings, 3 % text files (not indexed). Written by `threads` threads.
void makeTree(const fs::path& root, uint32_t files, unsigned threads = 8) {
  static const char* kWords[] = {"Kick", "Snare", "Hat", "Clap", "Bass", "Pad", "Lead", "Pluck", "Vox", "Perc",
                                 "Tom", "Ride", "Crash", "Shaker", "Rim", "Sub", "Chord", "Stab", "FX", "Loop"};
  const std::vector<uint8_t> wav = wavBytes(64);
  const std::string preset = R"({"id":1,"patch":"Operator","kind":"instrument"})";
  std::atomic<uint32_t> next{0};
  auto work = [&] {
    for (uint32_t i = next.fetch_add(1); i < files; i = next.fetch_add(1)) {
      const fs::path dir = root / ("Pack" + std::to_string(i / 2000)) / ("Folder" + std::to_string((i / 100) % 20));
      if (i % 100 == 0) {
        std::error_code ec;
        fs::create_directories(dir, ec);
      }
      std::string stem = std::string(kWords[i % 20]) + " " + kWords[(i / 20) % 20] + " " + std::to_string(i);
      const uint32_t k = i % 100;
      if (k < 85) {
        writeBytes(dir / (stem + ".wav"), wav.data(), wav.size());
      } else if (k < 90) {
        writeText(dir / (stem + ".pqpreset"), preset);
      } else if (k < 94) {
        writeText(dir / (stem + ".pqp"), "patch P { out o = 0 }\n");
      } else if (k < 97) {
        writeText(dir / (stem + ".scl"), "! x\n1\n2/1\n");
      } else {
        writeText(dir / (stem + ".txt"), "notes");
      }
    }
  };
  // Directories first (one creator per directory would race with the file writers).
  for (uint32_t i = 0; i < files; i += 100) {
    std::error_code ec;
    fs::create_directories(root / ("Pack" + std::to_string(i / 2000)) / ("Folder" + std::to_string((i / 100) % 20)), ec);
  }
  std::vector<std::thread> pool;
  for (unsigned t = 0; t < threads; ++t) pool.emplace_back(work);
  for (auto& t : pool) t.join();
}

uint32_t indexedCount(uint32_t files) {
  uint32_t n = 0;
  for (uint32_t i = 0; i < files; ++i) n += (i % 100) < 97 ? 1 : 0;
  return n;
}

}  // namespace

TEST_CASE("library: kinds, categories, folding") {
  CHECK(indexedKind("Kick.WAV") == ItemKind::Sample);
  CHECK(indexedKind("a.flac") == ItemKind::Sample);
  CHECK(indexedKind("x.pqpreset") == ItemKind::Preset);
  CHECK(indexedKind("x.scl") == ItemKind::Tuning);
  CHECK(indexedKind("song.pqd") == ItemKind::Project);
  CHECK(!indexedKind("readme.txt"));
  CHECK(!indexedKind("noext"));
  CHECK(!indexedKind(".wav"));

  CHECK(categorize(ItemKind::Sample, PlaceKind::Folder, "", "a.wav", Category::None) == Category::Samples);
  CHECK(categorize(ItemKind::Patch, PlaceKind::Folder, "", "a.pqp", Category::None) == Category::Patches);
  CHECK(categorize(ItemKind::Preset, PlaceKind::UserLibrary, "Racks", "a.pqpreset", Category::None) ==
        Category::Racks);
  CHECK(categorize(ItemKind::Preset, PlaceKind::UserLibrary, "Presets\\Operator", "a.pqpreset", Category::None) ==
        Category::Instruments);
  CHECK(categorize(ItemKind::Preset, PlaceKind::UserLibrary, "Presets\\EQ Eight\\Mix", "a.pqpreset",
                   Category::None) == Category::AudioEffects);
  CHECK(categorize(ItemKind::Preset, PlaceKind::UserLibrary, "Defaults", "Saturator.pqpreset", Category::None) ==
        Category::AudioEffects);
  CHECK(categorize(ItemKind::Preset, PlaceKind::Folder, "Presets\\Operator", "a.pqpreset", Category::None) ==
        Category::None);
  CHECK(categorize(ItemKind::Preset, PlaceKind::Folder, "", "a.pqpreset", Category::Instruments) ==
        Category::Instruments);

  CHECK(sniffDeviceCategory(R"({"id": 3, "patch": "operator", "kind" : "instrument"})") == Category::Instruments);
  CHECK(sniffDeviceCategory(R"({"patch":"eq","kind":"audioEffect"})") == Category::AudioEffects);
  CHECK(sniffDeviceCategory(R"({"patch":"rack","kind":"audioEffect","rack": {"chains": []}})") == Category::Racks);
  CHECK(sniffDeviceCategory("not json") == Category::None);

  bool sawInstrument = false, sawEffect = false;
  for (const StockItem& s : stockItems()) {
    sawInstrument = sawInstrument || s.category == Category::Instruments;
    sawEffect = sawEffect || s.category == Category::AudioEffects;
  }
  CHECK(sawInstrument);
  CHECK(sawEffect);

  CHECK(compareFolded("abc", "ABD") < 0);
  CHECK(compareFolded("ABC", "abc") < 0);  // ties broken by raw bytes ('A' < 'a')
  CHECK(compareFolded("abc", "abc") == 0);
  CHECK(compareFolded("ab", "abc") < 0);
  CHECK(stemOf("Kick 01.wav") == "Kick 01");
  CHECK(stemOf("noext") == "noext");
  CHECK((charMask("Kick") & charMask("kick")) == charMask("kick"));
  CHECK((charMask("ki") & ~charMask("Kick 808")) == 0);
  CHECK((charMask("kz") & ~charMask("Kick 808")) != 0);
}

TEST_CASE("library: initial scan of a small tree") {
  TempTree tree("scan");
  const fs::path r = tree.root();
  writeWav(r / "Drums" / "Kick 120bpm.wav", 4800, 44100, 1);
  writeWav(r / "Drums" / "snare.wav", 2400);
  writeWav(r / "Drums" / "Sub" / "hat.wav", 1200);
  writeText(r / "Drums" / "notes.txt", "x");
  writeText(r / "Drums" / ".dotfile.wav", "x");
  writeText(r / "patches" / "Filter.pqp", "patch F { out o = 0 }");
  writeText(r / "broken.wav", "not a wav file at all");
  writeText(r / "Presets" / "Operator" / "Bright.pqpreset", "{}");
  writeText(r / "Racks" / "Big.pqpreset", R"({"patch":"x","kind":"audioEffect","rack":{}})");
  fs::create_directories(r / "Empty");
  {
    const fs::path hidden = r / "Drums" / "hidden.wav";
    writeWav(hidden, 10);
    SetFileAttributesW(hidden.c_str(), FILE_ATTRIBUTE_HIDDEN);
  }

  const std::vector<PlaceSpec> places = {{tree.rootUtf8(), PlaceKind::UserLibrary, "User Library"}};
  ScanResult res = fullScan(nullptr, places);
  const Index& idx = *res.index;
  CHECK(res.stats.changed);
  CHECK(res.stats.filesProbed == 7);
  REQUIRE(idx.entryCount() == 7);
  // Directories depth first in case-folded order; entries grouped by directory in case-folded order.
  std::vector<std::string> rel;
  for (uint32_t d = 0; d < idx.dirCount(); ++d) rel.push_back(idx.dirRelPath(d));
  CHECK(rel == std::vector<std::string>{"", "Drums", "Drums\\Sub", "Empty", "patches", "Presets",
                                        "Presets\\Operator", "Racks"});
  std::vector<std::string> names;
  for (uint32_t e = 0; e < idx.entryCount(); ++e) names.emplace_back(idx.name(e));
  CHECK(names == std::vector<std::string>{"broken.wav", "Kick 120bpm.wav", "snare.wav", "hat.wav", "Filter.pqp",
                                          "Bright.pqpreset", "Big.pqpreset"});
  for (uint32_t d = 0; d < idx.dirCount(); ++d)
    for (uint32_t e = idx.dirs.firstEntry[d]; e < idx.dirs.firstEntry[d] + idx.dirs.entryCount[d]; ++e)
      CHECK(idx.entries.dir[e] == d);

  const uint32_t kick = entryNamed(idx, "Kick 120bpm.wav");
  REQUIRE(kick != Index::kNone);
  CHECK(idx.kind(kick) == ItemKind::Sample);
  CHECK(idx.category(kick) == Category::Samples);
  CHECK(idx.entries.rate[kick] == 44100);
  CHECK(idx.entries.channels[kick] == 1);
  CHECK(idx.entries.frames[kick] == 4800);
  CHECK(idx.entries.bits[kick] == 16);
  CHECK(idx.entries.bpm[kick] == doctest::Approx(120));
  CHECK((idx.entries.flags[kick] & kFlagBpmFromName) != 0);
  CHECK(idx.entries.depth[kick] == 1);
  CHECK(idx.seconds(kick) == doctest::Approx(4800.0 / 44100));
  CHECK(idx.stem(kick) == "Kick 120bpm");
  CHECK(idx.path(kick) == utf8(r / "Drums" / "Kick 120bpm.wav"));
  CHECK(idx.findPath(utf8(r / "DRUMS" / "kick 120BPM.wav")) == kick);
  CHECK(idx.entries.size[kick] == 44 + 4800 * 2);
  CHECK(std::any_of(idx.entries.fileId[kick].begin(), idx.entries.fileId[kick].end(), [](uint8_t b) { return b; }));

  const uint32_t broken = entryNamed(idx, "broken.wav");
  CHECK((idx.entries.flags[broken] & kFlagProbeFailed) != 0);
  CHECK(idx.entries.rate[broken] == 0);
  CHECK(idx.category(entryNamed(idx, "Filter.pqp")) == Category::Patches);
  CHECK(idx.category(entryNamed(idx, "Bright.pqpreset")) == Category::Instruments);  // Presets\Operator
  CHECK(idx.category(entryNamed(idx, "Big.pqpreset")) == Category::Racks);           // sniffed "rack"
  CHECK(idx.placeOnline == std::vector<uint8_t>{1});

  // A place whose root does not exist: offline, no entries, and the other places are unaffected.
  std::vector<PlaceSpec> two = places;
  two.push_back({utf8(r / "missing"), PlaceKind::Folder, "Gone"});
  ScanResult res2 = fullScan(res.index.get(), two);
  CHECK(res2.index->placeOnline == std::vector<uint8_t>{1, 0});
  CHECK(res2.index->entryCount() == 7);
  CHECK(res2.stats.filesProbed == 0);
}

TEST_CASE("library: revalidation and relists reuse records; renames keep probe data") {
  TempTree tree("rescan");
  const fs::path r = tree.root();
  for (int i = 0; i < 20; ++i) writeWav(r / "A" / ("a" + std::to_string(i) + ".wav"), 100 + i);
  for (int i = 0; i < 10; ++i) writeWav(r / "B" / "Deep" / ("b" + std::to_string(i) + ".wav"), 200 + i);
  const std::vector<PlaceSpec> places = {{tree.rootUtf8(), PlaceKind::Folder, "Pack"}};

  std::atomic<int> probes{0};
  const ProbeFn counting = [&](const std::string& path, const PlaceSpec& place, std::string_view relDir,
                               EntryRecord& rec) {
    ++probes;
    probeFile(path, place, relDir, rec);
  };
  ScanResult first = fullScan(nullptr, places, counting);
  REQUIRE(first.index->entryCount() == 30);
  CHECK(probes == 30);

  SUBCASE("a cancelled rebuild says so") {
    std::atomic<bool> cancel{true};
    ScanRequest req;
    req.places = places;
    req.cancel = &cancel;
    DiskSource disk;
    CHECK(rebuild(first.index.get(), req, disk, {}).stats.cancelled);
  }

  SUBCASE("unchanged revalidation probes nothing and changes nothing") {
    probes = 0;
    ScanResult again = fullScan(first.index.get(), places, counting);
    CHECK(probes == 0);
    CHECK_FALSE(again.stats.changed);
    CHECK(again.changes.empty());
    CHECK(again.stats.filesReused == 30);
    CHECK(rows(*again.index) == rows(*first.index));
    CHECK(dirRows(*again.index) == dirRows(*first.index));
  }

  SUBCASE("a dirty relist lists only that directory") {
    writeWav(r / "A" / "new.wav", 50);
    fs::remove(r / "A" / "a3.wav");
    writeWav(r / "A" / "a4.wav", 999);  // modified: new size
    ScanResult d = dirtyScan(*first.index, places, {"a"});  // case-insensitive
    CHECK(d.stats.dirsListed == 1);
    CHECK(d.stats.changed);
    REQUIRE(d.changes.size() == 1);
    CHECK(d.changes[0].relPath == "A");
    const Index& idx = *d.index;
    CHECK(idx.entryCount() == 30);
    CHECK(entryNamed(idx, "a3.wav") == Index::kNone);
    REQUIRE(entryNamed(idx, "new.wav") != Index::kNone);
    CHECK(idx.entries.frames[entryNamed(idx, "new.wav")] == 50);
    CHECK(idx.entries.frames[entryNamed(idx, "a4.wav")] == 999);
    CHECK(d.stats.filesProbed == 2);
    // The untouched directories are identical rows.
    for (uint32_t e = 0; e < idx.entryCount(); ++e)
      if (idx.name(e).starts_with("b")) {
        const uint32_t o = entryNamed(*first.index, idx.name(e));
        CHECK(idx.record(e) == first.index->record(o));
      }
  }

  SUBCASE("a file rename keeps the record (matched by file ID), no probe") {
    const uint32_t before = entryNamed(*first.index, "a7.wav");
    fs::rename(r / "A" / "a7.wav", r / "A" / "renamed.wav");
    ScanResult d = dirtyScan(*first.index, places, {"A"});
    CHECK(d.stats.filesProbed == 0);
    CHECK(d.stats.filesMoved == 1);
    const uint32_t after = entryNamed(*d.index, "renamed.wav");
    REQUIRE(after != Index::kNone);
    CHECK(d.index->entries.fileId[after] == first.index->entries.fileId[before]);
    CHECK(d.index->entries.frames[after] == 107);
  }

  SUBCASE("a directory moved in is listed recursively; its files keep their records") {
    fs::rename(r / "B", r / "A" / "B2");
    ScanResult d = dirtyScan(*first.index, places, {"", "A"}, {"A\\B2"});
    CHECK(d.stats.filesProbed == 0);
    CHECK(d.stats.filesMoved == 10);
    CHECK(d.index->entryCount() == 30);
    CHECK(d.index->findDir(0, "A\\B2\\Deep") != Index::kNone);
    CHECK(d.index->findDir(0, "B") == Index::kNone);
    const uint32_t b5 = entryNamed(*d.index, "b5.wav");
    CHECK(d.index->path(b5) == utf8(r / "A" / "B2" / "Deep" / "b5.wav"));
    CHECK(d.index->entries.depth[b5] == 3);
  }

  SUBCASE("a directory replaced by one with the same name is relisted when reported as added") {
    fs::remove_all(r / "B");
    writeWav(r / "B" / "fresh.wav", 77);
    ScanResult d = dirtyScan(*first.index, places, {""}, {"B"});
    CHECK(entryNamed(*d.index, "fresh.wav") != Index::kNone);
    CHECK(entryNamed(*d.index, "b0.wav") == Index::kNone);
    CHECK(d.index->findDir(0, "B\\Deep") == Index::kNone);
  }
}

TEST_CASE("library: index file round trip, corruption, journal replay") {
  TempTree tree("file");
  const fs::path r = tree.root() / "tree";
  for (int i = 0; i < 30; ++i) writeWav(r / ("D" + std::to_string(i % 3)) / ("s" + std::to_string(i) + ".wav"), 64 + i);
  writeText(r / "x.pqp", "patch X { out o = 0 }");
  const std::vector<PlaceSpec> places = {{utf8(r), PlaceKind::Folder, "Pack"},
                                         {utf8(tree.root() / "none"), PlaceKind::Project, "Proj"}};
  ScanResult first = fullScan(nullptr, places);
  const fs::path pqi = tree.root() / "data" / "index.pqi";
  const fs::path pqj = tree.root() / "data" / "index.pqj";
  REQUIRE(saveIndex(*first.index, 7, pqi));

  auto loaded = loadIndex(pqi);
  REQUIRE(loaded);
  CHECK(loaded->sequence == 7);
  CHECK(loaded->index->places == first.index->places);
  CHECK(loaded->index->placeOnline == first.index->placeOnline);
  CHECK(rows(*loaded->index) == rows(*first.index));
  CHECK(dirRows(*loaded->index) == dirRows(*first.index));
  CHECK(loaded->index->entries.mask == first.index->entries.mask);

  CHECK(loadIndex(tree.root() / "data" / "nope.pqi").error().code == ErrorCode::NotFound);
  {
    // A flipped byte anywhere in the payload: the hash catches it.
    std::vector<char> bytes;
    {
      std::ifstream in(pqi, std::ios::binary);
      bytes.assign(std::istreambuf_iterator<char>(in), {});
    }
    for (std::size_t at : {std::size_t{70}, bytes.size() / 2, bytes.size() - 1}) {
      std::vector<char> bad = bytes;
      bad[at] ^= 0x10;
      writeBytes(tree.root() / "bad.pqi", bad.data(), bad.size());
      auto b = loadIndex(tree.root() / "bad.pqi");
      CHECK_FALSE(b);
      if (!b) CHECK(b.error().code == ErrorCode::Parse);
    }
    writeBytes(tree.root() / "short.pqi", bytes.data(), bytes.size() / 3);
    CHECK_FALSE(loadIndex(tree.root() / "short.pqi"));
    // M5 review H8: one flipped bit in the header's counts (bytes 24–47) is caught by the checksum, before anything is
    // sized from them (it used to commit gigabytes and throw bad_alloc on the indexer thread). Every bit of bytes
    // 8–63, and the same flips with the checksum recomputed the old way (payload only), which the count bounds catch.
    for (std::size_t byte = 8; byte < 64; ++byte)
      for (int bit = 0; bit < 8; ++bit) {
        CAPTURE(byte);
        CAPTURE(bit);
        std::vector<char> bad = bytes;
        bad[byte] ^= static_cast<char>(1 << bit);
        writeBytes(tree.root() / "hdr.pqi", bad.data(), bad.size());
        auto b = loadIndex(tree.root() / "hdr.pqi");
        CHECK_FALSE(b);
      }
    for (const std::size_t at : {std::size_t{28}, std::size_t{32}, std::size_t{40}})
      for (const uint32_t v : {1u << 23, 1u << 27, 1u << 31, 0xFFFFFFFFu}) {
        std::vector<char> bad = bytes;
        uint32_t x = 0;
        std::memcpy(&x, bad.data() + at, 4);
        x ^= v;
        std::memcpy(bad.data() + at, &x, 4);
        writeBytes(tree.root() / "hdr.pqi", bad.data(), bad.size());
        CHECK_FALSE(loadIndex(tree.root() / "hdr.pqi"));
      }
  }

  // Journal: changes after the snapshot, replayed on load.
  writeWav(r / "D1" / "added.wav", 500);
  fs::remove(r / "D2" / "s2.wav");
  fs::create_directories(r / "D9" / "Inner");
  writeWav(r / "D9" / "Inner" / "deep.wav", 33);
  ScanResult second = dirtyScan(*first.index, places, {"D1", "D2", ""}, {"D9"});
  REQUIRE(second.stats.changed);
  REQUIRE(second.changes.size() == 5);  // D1, D2, the root (new subdirectory), D9, D9\Inner
  {
    Journal j;
    REQUIRE(j.reset(pqj, 7));
    // Two appends (two batches).
    std::vector<DirChange> a(second.changes.begin(), second.changes.begin() + 2);
    std::vector<DirChange> b(second.changes.begin() + 2, second.changes.end());
    REQUIRE(j.append(a));
    REQUIRE(j.append(b));
  }
  Journal::Contents c = Journal::read(pqj, 7);
  CHECK(c.changes.size() == second.changes.size());
  auto replayed = applyChanges(*loaded->index, c.changes);
  CHECK(rows(*replayed) == rows(*second.index));
  CHECK(dirRows(*replayed) == dirRows(*second.index));

  // A journal of another snapshot is ignored.
  CHECK(Journal::read(pqj, 8).changes.empty());
  // A torn tail: the last record is dropped, the rest replays.
  {
    std::vector<char> bytes;
    {
      std::ifstream in(pqj, std::ios::binary);
      bytes.assign(std::istreambuf_iterator<char>(in), {});
    }
    writeBytes(pqj, bytes.data(), bytes.size() - 5);
    Journal::Contents torn = Journal::read(pqj, 7);
    CHECK(torn.changes.size() == second.changes.size() - 1);
    CHECK(torn.validBytes < bytes.size() - 5);
    // Reopening for append cuts the torn tail; a new record then follows the intact ones.
    Journal j;
    REQUIRE(j.openAppend(pqj, torn.validBytes));
    REQUIRE(j.append({second.changes.back()}));
    j.close();
    CHECK(Journal::read(pqj, 7).changes.size() == second.changes.size());
  }

  // Record encoding round trip.
  for (const DirChange& ch : second.changes) {
    const std::vector<uint8_t> enc = encodeChange(ch);
    DirChange dec;
    REQUIRE(decodeChange(enc.data(), enc.size(), dec));
    CHECK(dec.placeRoot == ch.placeRoot);
    CHECK(dec.relPath == ch.relPath);
    CHECK(dec.listing.subdirs == ch.listing.subdirs);
    CHECK(dec.listing.files == ch.listing.files);
    DirChange cut;
    CHECK_FALSE(decodeChange(enc.data(), enc.size() - 1, cut));
  }
}

namespace {

struct SpeedResult {
  double initialMs = 0, revalidateMs = 0, saveMs = 0, loadMs = 0;
  uint32_t entries = 0;
};

SpeedResult indexSpeed(uint32_t files) {
  TempTree tree("speed");
  const auto t0 = std::chrono::steady_clock::now();
  makeTree(tree.root() / "lib", files);
  MESSAGE("created " << files << " files in " << msSince(t0) << " ms");
  const std::vector<PlaceSpec> places = {{utf8(tree.root() / "lib"), PlaceKind::Folder, "Pack"}};
  SpeedResult s;
  auto t1 = std::chrono::steady_clock::now();
  ScanResult first = fullScan(nullptr, places);
  s.initialMs = msSince(t1);
  s.entries = first.index->entryCount();
  CHECK(s.entries == indexedCount(files));
  CHECK(first.stats.filesProbed == s.entries);

  // The startup revalidation (the first pass is the one checked; two more show the spread).
  ScanResult again;
  std::vector<double> passes;
  for (int rep = 0; rep < 3; ++rep) {
    t1 = std::chrono::steady_clock::now();
    again = fullScan(first.index.get(), places);
    passes.push_back(msSince(t1));
  }
  s.revalidateMs = passes[0];
  MESSAGE("revalidate passes " << passes[0] << " / " << passes[1] << " / " << passes[2] << " ms");
  CHECK_FALSE(again.stats.changed);
  CHECK(again.stats.filesProbed == 0);
  CHECK(again.index->entryCount() == s.entries);

  const fs::path pqi = tree.root() / "index.pqi";
  t1 = std::chrono::steady_clock::now();
  REQUIRE(saveIndex(*again.index, 1, pqi));
  s.saveMs = msSince(t1);
  t1 = std::chrono::steady_clock::now();
  auto loaded = loadIndex(pqi);
  s.loadMs = msSince(t1);
  REQUIRE(loaded);
  CHECK(loaded->index->entryCount() == s.entries);
  CHECK(rows(*loaded->index) == rows(*again.index));
  MESSAGE(files << " files: initial index " << s.initialMs << " ms (" << s.entries << " entries), revalidate "
                << s.revalidateMs << " ms, save " << s.saveMs << " ms (" << fs::file_size(pqi) / 1024
                << " KiB), load " << s.loadMs << " ms");
  return s;
}

}  // namespace

TEST_CASE("library: index speed, 2k files (indicative)") {
  const SpeedResult s = indexSpeed(2000);
  // Generous bounds only (load-independent sanity); the criteria are checked on 200k files in `perf`.
  CHECK(s.revalidateMs < 2000);
  CHECK(s.loadMs < 150);
}

TEST_CASE("library: index speed, 200k files: revalidate <= 2 s, load <= 150 ms" * doctest::test_suite("perf")) {
  const SpeedResult s = indexSpeed(200000);
  CHECK(s.revalidateMs <= 2000);
  CHECK(s.loadMs <= 150);
}

