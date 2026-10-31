#include "pq/library/tags.h"

#include <algorithm>
#include <ctime>
#include <cstring>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "win_util.h"

namespace pq::library {

namespace {

std::string foldedName(std::string_view path) {
  const std::size_t sep = path.find_last_of("\\/");
  std::string n(sep == std::string_view::npos ? path : path.substr(sep + 1));
  for (char& c : n) c = foldChar(c);
  return n;
}

std::string pathKey(std::string_view path, uint64_t size) {
  return foldedPathKey(path) + '|' + std::to_string(size);
}

std::string hex(const uint8_t* p, std::size_t n) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string s;
  s.reserve(n * 2);
  for (std::size_t i = 0; i < n; ++i) {
    s.push_back(kHex[p[i] >> 4]);
    s.push_back(kHex[p[i] & 15]);
  }
  return s;
}

bool unhex(std::string_view s, uint8_t* out, std::size_t n) {
  if (s.size() != n * 2) return false;
  auto v = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (std::size_t i = 0; i < n; ++i) {
    const int hi = v(s[2 * i]), lo = v(s[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = static_cast<uint8_t>(hi * 16 + lo);
  }
  return true;
}

bool equalsFolded(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i)
    if (foldChar(a[i]) != foldChar(b[i])) return false;
  return true;
}

constexpr std::array<std::string_view, kCollectionCount> kDefaultNames = {"Red",  "Orange", "Yellow", "Green",
                                                                         "Blue", "Purple", "Grey"};

}  // namespace

bool TagKey::hasFileId() const noexcept {
  for (uint8_t b : fileId)
    if (b) return true;
  return false;
}

TagKey tagKeyOf(const Index& index, uint32_t entry) {
  TagKey k;
  k.volume = index.entries.volume[entry];
  k.fileId = index.entries.fileId[entry];
  k.path = index.path(entry);
  k.size = index.entries.size[entry];
  return k;
}

std::size_t TagStore::IdKeyHash::operator()(const IdKey& k) const noexcept {
  uint64_t a, b;
  std::memcpy(&a, k.id.data(), 8);
  std::memcpy(&b, k.id.data() + 8, 8);
  uint64_t h = k.volume * 0x9E3779B97F4A7C15ull ^ a;
  h = (h ^ (h >> 31)) * 0xBF58476D1CE4E5B9ull ^ b;
  return static_cast<std::size_t>(h ^ (h >> 29));
}

TagStore::TagStore() {
  for (uint32_t c = 0; c < kCollectionCount; ++c) names_[c] = std::string(kDefaultNames[c]);
}

TagStore::TagStore(std::filesystem::path file) : TagStore() { file_ = std::move(file); }

void TagStore::rebuildMaps() {
  byId_.clear();
  byPath_.clear();
  fileNames_.clear();
  for (uint32_t i = 0; i < items_.size(); ++i) {
    const TagKey& k = items_[i].key;
    if (k.hasFileId()) byId_[IdKey{k.volume, k.fileId}] = i;
    if (!k.path.empty()) {
      byPath_[pathKey(k.path, k.size)] = i;
      fileNames_.insert(foldedName(k.path));
    }
  }
  ++version_;
}

const TagItem* TagStore::findById(uint64_t volume, const FileIdBytes& id) const {
  const auto it = byId_.find(IdKey{volume, id});
  return it == byId_.end() ? nullptr : &items_[it->second];
}

const TagItem* TagStore::findByPath(std::string_view path, uint64_t size) const {
  const auto it = byPath_.find(pathKey(path, size));
  return it == byPath_.end() ? nullptr : &items_[it->second];
}

bool TagStore::hasFileName(std::string_view foldedName) const {
  return fileNames_.count(std::string(foldedName)) != 0;
}

const TagItem* TagStore::find(const TagKey& key) const {
  if (key.hasFileId())
    if (const TagItem* t = findById(key.volume, key.fileId)) return t;
  if (!key.path.empty()) return findByPath(key.path, key.size);
  return nullptr;
}

TagItem* TagStore::findMutable(const TagKey& key) { return const_cast<TagItem*>(find(key)); }

TagItem& TagStore::itemFor(const TagKey& key) {
  if (TagItem* t = findMutable(key)) {
    // Keep the key current (the caller has the file's present identity).
    if (key.hasFileId() && !(t->key.fileId == key.fileId && t->key.volume == key.volume)) {
      t->key.volume = key.volume;
      t->key.fileId = key.fileId;
    }
    if (!key.path.empty()) {
      t->key.path = key.path;
      t->key.size = key.size;
    }
    const auto idx = static_cast<uint32_t>(t - items_.data());
    rebuildMaps();
    return items_[idx];
  }
  items_.push_back(TagItem{key, 0, {}});
  rebuildMaps();
  return items_.back();
}

void TagStore::dropIfEmpty(TagItem* item) {
  if (item->collections == 0 && item->tags.empty()) items_.erase(items_.begin() + (item - items_.data()));
  rebuildMaps();
}

void TagStore::setCollection(const TagKey& key, uint32_t collection, bool on) {
  if (collection >= kCollectionCount) return;
  if (!on && !find(key)) return;
  TagItem& t = itemFor(key);
  if (on) t.collections = static_cast<uint8_t>(t.collections | (1u << collection));
  else t.collections = static_cast<uint8_t>(t.collections & ~(1u << collection));
  dropIfEmpty(&t);
}

void TagStore::addTag(const TagKey& key, std::string_view tag) {
  if (tag.empty() || tag.find_first_not_of(' ') == std::string_view::npos) return;
  TagItem& t = itemFor(key);
  for (const std::string& existing : t.tags)
    if (equalsFolded(existing, tag)) return;
  t.tags.emplace_back(tag);
  std::sort(t.tags.begin(), t.tags.end(), [](const std::string& a, const std::string& b) {
    return compareFolded(a, b) < 0;
  });
  ++version_;
}

void TagStore::removeTag(const TagKey& key, std::string_view tag) {
  TagItem* t = findMutable(key);
  if (!t) return;
  std::erase_if(t->tags, [&](const std::string& s) { return equalsFolded(s, tag); });
  dropIfEmpty(t);
}

void TagStore::renameCollection(uint32_t c, std::string name) {
  if (c >= kCollectionCount) return;
  names_[c] = std::move(name);
  ++version_;
}

std::vector<std::string> TagStore::allTags() const {
  std::vector<std::string> all;
  for (const TagItem& t : items_)
    for (const std::string& s : t.tags) all.push_back(s);
  std::sort(all.begin(), all.end(), [](const std::string& a, const std::string& b) { return compareFolded(a, b) < 0; });
  all.erase(std::unique(all.begin(), all.end(), [](const std::string& a, const std::string& b) { return equalsFolded(a, b); }),
            all.end());
  return all;
}

bool TagStore::reconcile(const Index& index) {
  if (items_.empty()) return false;
  bool changed = false;
  std::vector<uint8_t> resolved(items_.size(), 0);
  // By ID: follow renames and moves.
  if (!byId_.empty()) {
    for (uint32_t e = 0; e < index.entryCount(); ++e) {
      const auto it = byId_.find(IdKey{index.entries.volume[e], index.entries.fileId[e]});
      if (it == byId_.end()) continue;
      TagItem& t = items_[it->second];
      resolved[it->second] = 1;
      const std::string path = index.path(e);
      if (t.key.path != path || t.key.size != index.entries.size[e]) {
        t.key.path = path;
        t.key.size = index.entries.size[e];
        changed = true;
      }
    }
  }
  // By path + size: adopt the file's ID.
  bool unresolved = false;
  for (std::size_t i = 0; i < items_.size(); ++i) unresolved = unresolved || !resolved[i];
  if (unresolved) {
    std::string name;
    for (uint32_t e = 0; e < index.entryCount(); ++e) {
      name.assign(index.name(e));
      for (char& c : name) c = foldChar(c);
      if (!fileNames_.count(name)) continue;
      const auto it = byPath_.find(pathKey(index.path(e), index.entries.size[e]));
      if (it == byPath_.end() || resolved[it->second]) continue;
      TagItem& t = items_[it->second];
      resolved[it->second] = 1;
      if (!(t.key.fileId == index.entries.fileId[e]) || t.key.volume != index.entries.volume[e]) {
        t.key.fileId = index.entries.fileId[e];
        t.key.volume = index.entries.volume[e];
        changed = true;
      }
    }
  }
  if (changed) rebuildMaps();
  return changed;
}

// ---- tags.json ----------------------------------------------------------------------------------------------------

Status TagStore::load() {
  items_.clear();
  for (uint32_t c = 0; c < kCollectionCount; ++c) names_[c] = std::string(kDefaultNames[c]);
  saveBlocked_ = false;
  std::ifstream in(file_, std::ios::binary);
  if (!in) {
    rebuildMaps();
    std::error_code ec;
    if (!std::filesystem::exists(file_, ec) && !ec) return Ok{};  // no file yet
    saveBlocked_ = true;
    return Error{ErrorCode::Io, "cannot read the tags file " + detail::toUtf8(file_.native()) +
                                    " (tag edits are not saved this session)", {}};
  }
  std::stringstream ss;
  ss << in.rdbuf();
  in.close();
  const nlohmann::json j = nlohmann::json::parse(ss.str(), nullptr, false);
  if (j.is_discarded() || !j.is_object()) {
    rebuildMaps();
    std::filesystem::path aside = file_;
    aside += ".corrupt-" + std::to_string(static_cast<long long>(std::time(nullptr)));
    std::error_code ec;
    std::filesystem::rename(file_, aside, ec);
    if (ec) saveBlocked_ = true;
    return Error{ErrorCode::Parse, "tags file " + detail::toUtf8(file_.native()) + " is not valid JSON" +
                                       (ec ? std::string(" (tag edits are not saved this session)")
                                           : " (moved aside to " + detail::toUtf8(aside.filename().native()) + ")"), {}};
  }
  if (auto c = j.find("collections"); c != j.end() && c->is_array())
    for (uint32_t i = 0; i < kCollectionCount && i < c->size(); ++i)
      if ((*c)[i].is_string()) names_[i] = (*c)[i].get<std::string>();
  if (auto items = j.find("items"); items != j.end() && items->is_array()) {
    for (const nlohmann::json& it : *items) {
      if (!it.is_object()) continue;
      TagItem t;
      if (auto v = it.find("volume"); v != it.end() && v->is_string()) {
        uint8_t b[8] = {};
        if (unhex(v->get<std::string>(), b, 8))
          for (int i = 0; i < 8; ++i) t.key.volume = (t.key.volume << 8) | b[i];
      }
      if (auto v = it.find("fileId"); v != it.end() && v->is_string())
        if (!unhex(v->get<std::string>(), t.key.fileId.data(), 16)) t.key.fileId = {};
      if (auto v = it.find("path"); v != it.end() && v->is_string()) t.key.path = v->get<std::string>();
      if (auto v = it.find("size"); v != it.end() && v->is_number_unsigned()) t.key.size = v->get<uint64_t>();
      if (auto v = it.find("collections"); v != it.end() && v->is_array())
        for (const auto& c : *v)
          if (c.is_number_unsigned() && c.get<uint32_t>() < kCollectionCount)
            t.collections = static_cast<uint8_t>(t.collections | (1u << c.get<uint32_t>()));
      if (auto v = it.find("tags"); v != it.end() && v->is_array())
        for (const auto& s : *v)
          if (s.is_string() && !s.get<std::string>().empty()) t.tags.push_back(s.get<std::string>());
      std::sort(t.tags.begin(), t.tags.end(), [](const std::string& a, const std::string& b) {
        return compareFolded(a, b) < 0;
      });
      if (t.collections == 0 && t.tags.empty()) continue;
      if (!t.key.hasFileId() && t.key.path.empty()) continue;
      items_.push_back(std::move(t));
    }
  }
  rebuildMaps();
  return Ok{};
}

Status TagStore::save() const {
  if (saveBlocked_)
    return Error{ErrorCode::Io, "the tags file " + detail::toUtf8(file_.native()) + " could not be read: not overwritten", {}};
  nlohmann::json j;
  j["version"] = 1;
  j["collections"] = nlohmann::json::array();
  for (const std::string& n : names_) j["collections"].push_back(n);
  nlohmann::json items = nlohmann::json::array();
  for (const TagItem& t : items_) {
    nlohmann::json it;
    uint8_t vol[8];
    for (int i = 0; i < 8; ++i) vol[i] = static_cast<uint8_t>(t.key.volume >> (56 - 8 * i));
    it["volume"] = hex(vol, 8);
    it["fileId"] = hex(t.key.fileId.data(), 16);
    it["path"] = t.key.path;
    it["size"] = t.key.size;
    nlohmann::json cols = nlohmann::json::array();
    for (uint32_t c = 0; c < kCollectionCount; ++c)
      if (t.collections & (1u << c)) cols.push_back(c);
    it["collections"] = std::move(cols);
    it["tags"] = t.tags;
    items.push_back(std::move(it));
  }
  j["items"] = std::move(items);
  const std::string text = j.dump(1) + "\n";
  std::error_code ec;
  if (file_.has_parent_path()) std::filesystem::create_directories(file_.parent_path(), ec);
  if (!detail::writeFileAtomic(file_.native(), text.data(), text.size()))
    return Error{ErrorCode::Io, "cannot write " + detail::toUtf8(file_.native()), {}};
  return Ok{};
}

}  // namespace pq::library
