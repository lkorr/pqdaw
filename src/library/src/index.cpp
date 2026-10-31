#include "pq/library/index.h"

#include <algorithm>
#include <cstring>

#include "pq/document/devices.h"

namespace pq::library {

namespace {

bool equalsFolded(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i)
    if (foldChar(a[i]) != foldChar(b[i])) return false;
  return true;
}

// The extension (without the dot) of a file name, or "".
std::string_view extensionOf(std::string_view name) noexcept {
  const std::size_t dot = name.rfind('.');
  if (dot == std::string_view::npos || dot == 0) return {};
  return name.substr(dot + 1);
}

std::string_view firstComponent(std::string_view rel, std::string_view* rest) noexcept {
  const std::size_t sep = rel.find('\\');
  if (sep == std::string_view::npos) {
    if (rest) *rest = {};
    return rel;
  }
  if (rest) *rest = rel.substr(sep + 1);
  return rel.substr(0, sep);
}

Category stockDeviceCategory(std::string_view nameOrPatch) noexcept {
  for (const doc::StockDevice& d : doc::stockDevices())
    if (equalsFolded(d.name, nameOrPatch) || equalsFolded(d.patch, nameOrPatch))
      return d.kind == doc::DeviceKind::Instrument ? Category::Instruments : Category::AudioEffects;
  return Category::None;
}

}  // namespace

std::string_view itemKindName(ItemKind k) noexcept {
  switch (k) {
    case ItemKind::Sample: return "sample";
    case ItemKind::Preset: return "preset";
    case ItemKind::Device: return "device";
    case ItemKind::Tuning: return "tuning";
    case ItemKind::Clip: return "clip";
    case ItemKind::Patch: return "patch";
    case ItemKind::Project: return "project";
    case ItemKind::Count: break;
  }
  return "?";
}

std::optional<ItemKind> indexedKind(std::string_view fileName) noexcept {
  const std::string_view ext = extensionOf(fileName);
  if (ext.empty() || ext.size() > 8) return std::nullopt;
  struct Map {
    std::string_view ext;
    ItemKind kind;
  };
  static constexpr Map kMap[] = {
      {"wav", ItemKind::Sample},      {"wave", ItemKind::Sample},    {"w64", ItemKind::Sample},
      {"rf64", ItemKind::Sample},     {"bw64", ItemKind::Sample},    {"aif", ItemKind::Sample},
      {"aiff", ItemKind::Sample},     {"aifc", ItemKind::Sample},    {"flac", ItemKind::Sample},
      {"mp3", ItemKind::Sample},      {"ogg", ItemKind::Sample},     {"pqpreset", ItemKind::Preset},
      {"pqdevice", ItemKind::Device}, {"pqtuning", ItemKind::Tuning}, {"scl", ItemKind::Tuning},
      {"pqclip", ItemKind::Clip},     {"mid", ItemKind::Clip},       {"midi", ItemKind::Clip},
      {"pqp", ItemKind::Patch},       {"pqd", ItemKind::Project},
  };
  for (const Map& m : kMap)
    if (equalsFolded(m.ext, ext)) return m.kind;
  return std::nullopt;
}

std::string_view categoryName(Category c) noexcept {
  switch (c) {
    case Category::None: return "";
    case Category::Instruments: return "Instruments";
    case Category::AudioEffects: return "Audio Effects";
    case Category::Racks: return "Racks";
    case Category::Samples: return "Samples";
    case Category::Clips: return "Clips";
    case Category::Tunings: return "Tunings";
    case Category::Patches: return "Patches/Subcircuits";
    case Category::Grooves: return "Grooves";
    case Category::Count: break;
  }
  return "?";
}

uint64_t charMask(std::string_view s) noexcept {
  uint64_t m = 0;
  for (char ch : s) {
    const auto c = static_cast<uint8_t>(foldChar(ch));
    unsigned bit;
    if (c >= 'a' && c <= 'z')
      bit = c - 'a';
    else if (c >= '0' && c <= '9')
      bit = 26u + (c - '0');
    else
      bit = 36u + (c % 28u);
    m |= uint64_t{1} << bit;
  }
  return m;
}

int compareFolded(std::string_view a, std::string_view b) noexcept {
  const std::size_t n = std::min(a.size(), b.size());
  for (std::size_t i = 0; i < n; ++i) {
    const auto x = static_cast<uint8_t>(foldChar(a[i]));
    const auto y = static_cast<uint8_t>(foldChar(b[i]));
    if (x != y) return x < y ? -1 : 1;
  }
  if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
  const int raw = a.compare(b);
  return raw < 0 ? -1 : (raw > 0 ? 1 : 0);
}

std::string_view stemOf(std::string_view fileName) noexcept {
  const std::size_t dot = fileName.rfind('.');
  if (dot == std::string_view::npos || dot == 0) return fileName;
  return fileName.substr(0, dot);
}

std::string foldedPathKey(std::string_view path) {
  std::string k;
  k.reserve(path.size());
  for (char c : path) k.push_back(c == '/' ? '\\' : foldChar(c));
  while (k.size() > 3 && k.back() == '\\') k.pop_back();
  return k;
}

// ---- Index ------------------------------------------------------------------------------------------------------

std::string Index::dirRelPath(uint32_t d) const {
  std::vector<uint32_t> chain;
  for (uint32_t x = d; x != kNone && dirs.parent[x] != kNone; x = dirs.parent[x]) chain.push_back(x);
  std::string rel;
  for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
    if (!rel.empty()) rel.push_back('\\');
    rel.append(dirName(*it));
  }
  return rel;
}

std::string Index::dirPath(uint32_t d) const {
  std::string p = places[dirs.place[d]].root;
  while (!p.empty() && (p.back() == '\\' || p.back() == '/')) p.pop_back();
  const std::string rel = dirRelPath(d);
  if (!rel.empty()) {
    p.push_back('\\');
    p.append(rel);
  }
  return p;
}

std::string Index::path(uint32_t e) const {
  std::string p = dirPath(entries.dir[e]);
  p.push_back('\\');
  p.append(name(e));
  return p;
}

EntryRecord Index::record(uint32_t e) const {
  EntryRecord r;
  r.name = std::string(name(e));
  r.kind = static_cast<ItemKind>(entries.kind[e]);
  r.category = static_cast<Category>(entries.category[e]);
  r.flags = entries.flags[e];
  r.size = entries.size[e];
  r.mtime = entries.mtime[e];
  r.volume = entries.volume[e];
  r.fileId = entries.fileId[e];
  r.frames = entries.frames[e];
  r.rate = entries.rate[e];
  r.channels = entries.channels[e];
  r.bits = entries.bits[e];
  r.container = entries.container[e];
  r.rootKey = entries.rootKey[e];
  r.bpm = entries.bpm[e];
  return r;
}

uint32_t Index::addDir(uint32_t parent, std::string_view name, uint16_t place, uint16_t depth) {
  const auto d = static_cast<uint32_t>(dirs.parent.size());
  dirs.parent.push_back(parent);
  dirs.nameOff.push_back(static_cast<uint32_t>(strings.size()));
  const std::size_t len = std::min<std::size_t>(name.size(), 0xFFFF);
  dirs.nameLen.push_back(static_cast<uint16_t>(len));
  strings.insert(strings.end(), name.data(), name.data() + len);
  dirs.place.push_back(place);
  dirs.depth.push_back(depth);
  dirs.firstEntry.push_back(entryCount());
  dirs.entryCount.push_back(0);
  return d;
}

void Index::addEntry(uint32_t dir, const EntryRecord& r) {
  const std::size_t len = std::min<std::size_t>(r.name.size(), 0xFFFF);
  const std::string_view nm(r.name.data(), len);
  const std::string_view st = stemOf(nm);
  entries.dir.push_back(dir);
  entries.nameOff.push_back(static_cast<uint32_t>(strings.size()));
  entries.nameLen.push_back(static_cast<uint16_t>(len));
  entries.stemLen.push_back(static_cast<uint16_t>(st.size()));
  strings.insert(strings.end(), nm.data(), nm.data() + len);
  entries.kind.push_back(static_cast<uint8_t>(r.kind));
  entries.category.push_back(static_cast<uint8_t>(r.category));
  entries.flags.push_back(r.flags);
  entries.depth.push_back(static_cast<uint8_t>(std::min<uint16_t>(dirs.depth[dir], 255)));
  entries.size.push_back(r.size);
  entries.mtime.push_back(r.mtime);
  entries.volume.push_back(r.volume);
  entries.fileId.push_back(r.fileId);
  entries.frames.push_back(r.frames);
  entries.rate.push_back(r.rate);
  entries.channels.push_back(r.channels);
  entries.bits.push_back(r.bits);
  entries.container.push_back(r.container);
  entries.rootKey.push_back(r.rootKey);
  entries.bpm.push_back(r.bpm);
  entries.mask.push_back(charMask(st));
  ++dirs.entryCount[dir];
}

uint32_t Index::findDir(uint32_t place, std::string_view relPath) const {
  uint32_t cur = kNone;
  for (uint32_t d = 0; d < dirCount(); ++d)
    if (dirs.parent[d] == kNone && dirs.place[d] == place) {
      cur = d;
      break;
    }
  if (cur == kNone) return kNone;
  std::string_view rest = relPath;
  while (!rest.empty() && rest.front() == '\\') rest.remove_prefix(1);
  while (!rest.empty()) {
    std::string_view next;
    const std::string_view comp = firstComponent(rest, &next);
    uint32_t found = kNone;
    for (uint32_t d = cur + 1; d < dirCount(); ++d)
      if (dirs.parent[d] == cur && equalsFolded(dirName(d), comp)) {
        found = d;
        break;
      }
    if (found == kNone) return kNone;
    cur = found;
    rest = next;
  }
  return cur;
}

uint32_t Index::findPath(std::string_view absPath) const {
  const std::string key = foldedPathKey(absPath);
  const std::size_t sep = key.rfind('\\');
  if (sep == std::string::npos) return kNone;
  const std::string_view dirKey = std::string_view(key).substr(0, sep);
  const std::string_view fileName = std::string_view(key).substr(sep + 1);
  for (uint32_t p = 0; p < places.size(); ++p) {
    const std::string root = foldedPathKey(places[p].root);
    if (dirKey.size() < root.size() || dirKey.substr(0, root.size()) != root) continue;
    std::string_view rel = dirKey.substr(root.size());
    if (!rel.empty() && rel.front() != '\\') continue;
    const uint32_t d = findDir(p, rel);
    if (d == kNone) continue;
    for (uint32_t e = dirs.firstEntry[d]; e < dirs.firstEntry[d] + dirs.entryCount[d]; ++e)
      if (equalsFolded(name(e), fileName)) return e;
  }
  return kNone;
}

// ---- Categories ---------------------------------------------------------------------------------------------------

Category sniffDeviceCategory(std::string_view head) noexcept {
  auto valueAfterKey = [&](std::string_view key, std::size_t from, std::size_t* at) -> bool {
    for (std::size_t p = head.find(key, from); p != std::string_view::npos; p = head.find(key, p + 1)) {
      std::size_t q = p + key.size();
      while (q < head.size() && (head[q] == ' ' || head[q] == '\t' || head[q] == '\r' || head[q] == '\n')) ++q;
      if (q < head.size() && head[q] == ':') {
        *at = q + 1;
        return true;
      }
    }
    return false;
  };
  std::size_t at = 0;
  if (valueAfterKey("\"rack\"", 0, &at)) return Category::Racks;
  if (valueAfterKey("\"kind\"", 0, &at)) {
    while (at < head.size() && (head[at] == ' ' || head[at] == '\t' || head[at] == '\r' || head[at] == '\n')) ++at;
    if (at < head.size() && head[at] == '"') {
      const std::size_t end = head.find('"', at + 1);
      if (end != std::string_view::npos) {
        const std::string_view v = head.substr(at + 1, end - at - 1);
        if (v == "instrument") return Category::Instruments;
        if (v == "audioEffect") return Category::AudioEffects;
      }
    }
  }
  return Category::None;
}

Category categorize(ItemKind kind, PlaceKind place, std::string_view relDir, std::string_view fileName,
                    Category sniffed) noexcept {
  switch (kind) {
    case ItemKind::Sample: return Category::Samples;
    case ItemKind::Clip: return Category::Clips;
    case ItemKind::Tuning: return Category::Tunings;
    case ItemKind::Patch: return Category::Patches;
    case ItemKind::Project: return Category::None;
    case ItemKind::Preset:
    case ItemKind::Device: break;
    case ItemKind::Count: return Category::None;
  }
  if (sniffed != Category::None) return sniffed;
  if (place != PlaceKind::UserLibrary) return Category::None;
  std::string_view rest;
  const std::string_view top = firstComponent(relDir, &rest);
  if (equalsFolded(top, "Racks")) return Category::Racks;
  if (equalsFolded(top, "Presets") && !rest.empty()) return stockDeviceCategory(firstComponent(rest, nullptr));
  if (equalsFolded(top, "Defaults")) return stockDeviceCategory(stemOf(fileName));
  return Category::None;
}

std::vector<StockItem> stockItems() {
  std::vector<StockItem> out;
  for (const doc::StockDevice& d : doc::stockDevices())
    out.push_back({d.name, d.patch,
                   d.kind == doc::DeviceKind::Instrument ? Category::Instruments : Category::AudioEffects});
  return out;
}

}  // namespace pq::library
