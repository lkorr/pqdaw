// T5.9: tags and collections: the store, tags.json, and tags that survive renames (by file ID through the watcher,
// by path + size as the fallback).

#include <doctest/doctest.h>

#include <chrono>

#include "library_test_util.h"
#include "pq/library/indexer.h"
#include "pq/library/scan.h"
#include "pq/library/search.h"
#include "pq/library/tags.h"

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

TagKey keyWithId(uint8_t id, std::string path, uint64_t size) {
  TagKey k;
  k.volume = 42;
  k.fileId[0] = id;
  k.path = std::move(path);
  k.size = size;
  return k;
}

}  // namespace

TEST_CASE("library: tag store edits and tags.json round trip") {
  TempTree tree("tagstore");
  const fs::path file = tree.root() / "lib" / "tags.json";
  TagStore s(file);
  REQUIRE(s.load());  // missing file: empty
  CHECK(s.items().empty());
  CHECK(s.collectionNames()[0] == "Red");

  const TagKey a = keyWithId(1, "C:\\x\\a.wav", 10);
  const TagKey b = keyWithId(2, "C:\\x\\b.wav", 20);
  s.setCollection(a, 0, true);
  s.setCollection(a, 4, true);
  s.addTag(a, "Drums");
  s.addTag(a, "drums");  // case-insensitive duplicate
  s.addTag(a, "dark");
  s.addTag(b, "vocal");
  s.addTag(b, "  ");     // blank: ignored
  s.renameCollection(6, "Keepers");
  REQUIRE(s.find(a));
  CHECK(s.find(a)->collections == 0b10001);
  CHECK(s.find(a)->tags == std::vector<std::string>{"dark", "Drums"});
  CHECK(s.allTags() == std::vector<std::string>{"dark", "Drums", "vocal"});

  // Lookups: by ID wins over the path; without an ID the path + size finds it; a different size does not.
  CHECK(s.find(keyWithId(1, "C:\\elsewhere.wav", 99)) == s.find(a));
  TagKey byPath;
  byPath.path = "c:/X/A.WAV";
  byPath.size = 10;
  CHECK(s.find(byPath) == s.find(a));
  byPath.size = 11;
  CHECK(s.find(byPath) == nullptr);

  REQUIRE(s.save());
  TagStore t(file);
  REQUIRE(t.load());
  REQUIRE(t.items().size() == 2);
  CHECK(t.find(a)->collections == 0b10001);
  CHECK(t.find(a)->tags == s.find(a)->tags);
  CHECK(t.find(b)->tags == std::vector<std::string>{"vocal"});
  CHECK(t.find(a)->key.volume == 42);
  CHECK(t.collectionNames()[6] == "Keepers");

  // Removing the last tag / collection drops the item.
  t.removeTag(b, "VOCAL");
  CHECK(t.find(b) == nullptr);
  t.setCollection(a, 0, false);
  t.setCollection(a, 4, false);
  t.removeTag(a, "dark");
  t.removeTag(a, "drums");
  CHECK(t.items().empty());

  // A broken file is an error, not a crash.
  writeText(file, "{ not json");
  TagStore broken(file);
  CHECK_FALSE(broken.load());
  // M5 review M17: it was overwritten by the next tag edit (every stored tag lost). It is moved aside first.
  CHECK_FALSE(fs::exists(file));
  int aside = 0;
  for (const auto& e : fs::directory_iterator(file.parent_path()))
    aside += e.path().filename().string().rfind("tags.json.corrupt-", 0) == 0 ? 1 : 0;
  CHECK(aside == 1);
  CHECK_FALSE(broken.saveBlocked());
  // A file that exists but cannot be read (another process holds it without sharing): no save this session.
  REQUIRE(s.save());
  {
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(h != INVALID_HANDLE_VALUE);
    TagStore locked(file);
    const Status st = locked.load();
    CHECK_FALSE(st);
    if (!st) CHECK(st.error().code == ErrorCode::Io);
    CHECK(locked.saveBlocked());
    locked.addTag(a, "new");
    CHECK_FALSE(locked.save());
    CloseHandle(h);
  }
  TagStore again(file);
  REQUIRE(again.load());
  CHECK(again.items().size() == 2);  // intact
}

TEST_CASE("library: tags survive a rename (watcher, file ID) and follow the path") {
  TempTree tree("tagrename");
  const fs::path lib = tree.root() / "lib";
  writeWav(lib / "Drums" / "kick.wav", 100);
  writeWav(lib / "Drums" / "snare.wav", 100);
  IndexerConfig cfg;
  cfg.places = {{utf8(lib), PlaceKind::Folder, "Lib"}};
  Indexer ix(cfg);
  ix.start();
  REQUIRE(ix.waitIdle(10s));

  TagStore store(tree.root() / "tags.json");
  {
    auto idx = ix.index();
    const uint32_t kick = entryNamed(*idx, "kick.wav");
    REQUIRE(kick != Index::kNone);
    store.setCollection(tagKeyOf(*idx, kick), 2, true);
    store.addTag(tagKeyOf(*idx, kick), "favourite");
  }
  REQUIRE(store.save());

  // Rename and move into another folder.
  fs::create_directories(lib / "Keep");
  fs::rename(lib / "Drums" / "kick.wav", lib / "Keep" / "Big Kick.wav");
  const auto t0 = std::chrono::steady_clock::now();
  std::shared_ptr<const Index> idx;
  while (msSince(t0) < 5000) {
    idx = ix.index();
    if (entryNamed(*idx, "Big Kick.wav") != Index::kNone && entryNamed(*idx, "kick.wav") == Index::kNone) break;
    ix.waitForGeneration(ix.generation(), 50ms);
  }
  const uint32_t moved = entryNamed(*idx, "Big Kick.wav");
  REQUIRE(moved != Index::kNone);

  // Found by file ID at the new path; the search sees the tag and the collection.
  const TagItem* item = store.find(tagKeyOf(*idx, moved));
  REQUIRE(item);
  CHECK(item->tags == std::vector<std::string>{"favourite"});
  auto tags = buildEntryTags(*idx, store);
  CHECK(tags->combo[moved] != 0);
  CHECK(tags->combo[entryNamed(*idx, "snare.wav")] == 0);
  SearchFilter inCollection;
  inCollection.collections = 1u << 2;
  const SearchResult r1 = search(idx, tags, SearchQuery{"", inCollection});
  REQUIRE(r1.hits.size() == 1);
  CHECK(r1.hits[0].entry == moved);
  const SearchResult r2 = search(idx, tags, SearchQuery{"favo", {}});  // a tag term
  REQUIRE(r2.hits.size() == 1);
  CHECK(r2.hits[0].entry == moved);

  // reconcile() records the new path, so the path fallback stays current too; it survives a save + load.
  CHECK(store.reconcile(*idx));
  CHECK(store.find(tagKeyOf(*idx, moved))->key.path == utf8(lib / "Keep" / "Big Kick.wav"));
  CHECK_FALSE(store.reconcile(*idx));
  REQUIRE(store.save());
  TagStore reloaded(tree.root() / "tags.json");
  REQUIRE(reloaded.load());
  CHECK(reloaded.find(tagKeyOf(*idx, moved)) != nullptr);
  ix.stop();
}

TEST_CASE("library: path + size fallback adopts the file ID") {
  TempTree tree("tagfallback");
  const fs::path lib = tree.root() / "lib";
  writeWav(lib / "pad.wav", 300);
  const std::vector<PlaceSpec> places = {{utf8(lib), PlaceKind::Folder, "Lib"}};
  ScanRequest req;
  req.places = places;
  DiskSource disk;
  ScanResult res = rebuild(nullptr, req, disk, {});
  const Index& idx = *res.index;
  const uint32_t pad = entryNamed(idx, "pad.wav");
  REQUIRE(pad != Index::kNone);

  // An item keyed by path + size only (a file system without IDs, or written by an editor that replaced the file).
  TagStore store;
  TagKey k;
  k.path = idx.path(pad);
  k.size = idx.entries.size[pad];
  store.addTag(k, "warm");
  auto tags = buildEntryTags(idx, store);
  CHECK(tags->combo[pad] != 0);
  CHECK(store.reconcile(idx));
  const TagItem* item = store.find(tagKeyOf(idx, pad));
  REQUIRE(item);
  CHECK(item->key.hasFileId());
  CHECK(item->key.fileId == idx.entries.fileId[pad]);
}
