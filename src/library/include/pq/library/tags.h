#pragma once

// Tags and collections (T5.9; SPEC §9.1): 7 colour collections (Ableton-style) and free tags per file, stored in
// `tags.json` (by default next to the index: %LOCALAPPDATA%\pqdaw\library\tags.json; unlike the index it is user data,
// not a cache).
//
// Items are keyed by the NTFS identity (volume serial + 128-bit file ID), which survives renames and moves within a
// volume; the path and size are the fallback for files without an ID (FAT, some network shares) and for editors that
// save by replacing the file. reconcile() follows renames (an item found by ID gets the entry's new path) and re-keys
// fallback hits to the file's ID.
//
// Not RT code; owned by one thread (the UI). The search reads an immutable EntryTags built from it (search.h).
//
// tags.json: {"version": 1, "collections": ["Red", …7 names], "items": [{"volume": "<16 hex>", "fileId": "<32 hex>",
// "path": "C:\\…", "size": 123, "collections": [0, 3], "tags": ["drums", "dark"]}, …]}

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "pq/core/result.h"
#include "pq/library/index.h"

namespace pq::library {

inline constexpr uint32_t kCollectionCount = 7;

struct TagKey {
  uint64_t volume = 0;
  FileIdBytes fileId{};
  std::string path;  // absolute UTF-8
  uint64_t size = 0;
  bool hasFileId() const noexcept;
};
// The key of an index entry.
TagKey tagKeyOf(const Index& index, uint32_t entry);

struct TagItem {
  TagKey key;
  uint8_t collections = 0;        // bit c = in collection c
  std::vector<std::string> tags;  // sorted, unique, as typed
};

class TagStore {
public:
  TagStore();
  explicit TagStore(std::filesystem::path file);

  const std::filesystem::path& file() const noexcept { return file_; }
  // A missing file = empty store. M5 review M17: a file that exists but cannot be read (a sharing violation from a
  // sync tool or AV) is an Io error and blocks save() for the session (it would replace every stored tag); one that
  // does not parse is moved aside (tags.json.corrupt-<unix time>) first, then the store starts empty.
  Status load();
  Status save() const;  // atomic (tmp + rename); refused while saveBlocked()
  bool saveBlocked() const noexcept { return saveBlocked_; }

  const std::array<std::string, kCollectionCount>& collectionNames() const noexcept { return names_; }
  void renameCollection(uint32_t c, std::string name);

  // The item of a file (by ID, else by path + size), or null.
  const TagItem* find(const TagKey& key) const;
  // The parts of find() (buildEntryTags uses them without building a TagKey per entry).
  const TagItem* findById(uint64_t volume, const FileIdBytes& id) const;
  const TagItem* findByPath(std::string_view path, uint64_t size) const;
  // Whether any item's file name (case-folded) is `foldedName`: the path fallback is tried only for those.
  bool hasFileName(std::string_view foldedName) const;
  void setCollection(const TagKey& key, uint32_t collection, bool on);
  void addTag(const TagKey& key, std::string_view tag);
  void removeTag(const TagKey& key, std::string_view tag);

  // Every distinct free tag (sorted, case-insensitively unique).
  std::vector<std::string> allTags() const;
  std::span<const TagItem> items() const noexcept { return items_; }

  // Follows the index: items found by ID whose path changed take the new path; items found by path + size get the
  // file's ID. Returns true when something changed (the caller saves).
  bool reconcile(const Index& index);

  uint64_t version() const noexcept { return version_; }  // bumps on every change

private:
  TagItem* findMutable(const TagKey& key);
  TagItem& itemFor(const TagKey& key);
  void dropIfEmpty(TagItem* item);
  void rebuildMaps();

  std::filesystem::path file_;
  bool saveBlocked_ = false;
  std::array<std::string, kCollectionCount> names_;
  std::vector<TagItem> items_;
  struct IdKey {
    uint64_t volume = 0;
    FileIdBytes id{};
    bool operator==(const IdKey&) const = default;
  };
  struct IdKeyHash {
    std::size_t operator()(const IdKey& k) const noexcept;
  };
  std::unordered_map<IdKey, uint32_t, IdKeyHash> byId_;  // file ID → item
  std::unordered_map<std::string, uint32_t> byPath_;      // folded path + '|' + size → item
  std::unordered_set<std::string> fileNames_;             // folded file names of all items
  uint64_t version_ = 0;
};

}  // namespace pq::library
