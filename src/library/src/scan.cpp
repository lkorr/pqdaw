#include "pq/library/scan.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

#include "pq/media/probe.h"
#include "win_util.h"

namespace pq::library {

using detail::joinPath;
using detail::toUtf8;
using detail::toWide;
using detail::winPath;

namespace {

std::string folded(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = foldChar(c);
  return out;
}

std::string childRel(std::string_view rel, std::string_view name) {
  std::string r(rel);
  if (!r.empty()) r.push_back('\\');
  r.append(name);
  return r;
}

// Hidden / system entries, dot names and name-surrogate reparse points (junctions, symlinks, mount points) are left
// out; cloud placeholders (OneDrive) are kept but flagged offline so nothing opens (and downloads) them.
bool skipEntry(std::wstring_view name, DWORD attrs, DWORD reparseTag) {
  if (name.empty() || name[0] == L'.') return true;
  if (attrs & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) return true;
  if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) && IsReparseTagNameSurrogate(reparseTag)) return true;
  return false;
}

bool isOfflineAttr(DWORD attrs) {
  return (attrs & (FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS)) != 0;
}

void addFile(Listing& out, std::wstring_view wname, uint64_t size, int64_t mtime, uint64_t volume,
             const uint8_t* id, std::size_t idBytes, DWORD attrs) {
  std::string name = toUtf8(wname);
  const auto kind = indexedKind(name);
  if (!kind) return;
  EntryRecord r;
  r.name = std::move(name);
  r.kind = *kind;
  r.size = size;
  r.mtime = mtime;
  r.volume = volume;
  if (id) std::memcpy(r.fileId.data(), id, std::min<std::size_t>(idBytes, 16));
  if (isOfflineAttr(attrs)) r.flags |= kFlagOffline;
  out.files.push_back(std::move(r));
}

bool listFallback(const std::wstring& dir, uint64_t volume, Listing& out) {
  WIN32_FIND_DATAW fd{};
  HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                              FIND_FIRST_EX_LARGE_FETCH);
  if (h == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;  // an empty directory
  do {
    const std::wstring_view name(fd.cFileName);
    if (skipEntry(name, fd.dwFileAttributes, fd.dwReserved0)) continue;
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      out.subdirs.push_back(toUtf8(name));
      continue;
    }
    const uint64_t size = (uint64_t{fd.nFileSizeHigh} << 32) | fd.nFileSizeLow;
    const int64_t mtime = static_cast<int64_t>((uint64_t{fd.ftLastWriteTime.dwHighDateTime} << 32) |
                                               fd.ftLastWriteTime.dwLowDateTime);
    addFile(out, name, size, mtime, volume, nullptr, 0, fd.dwFileAttributes);
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return true;
}

}  // namespace

// ---- DiskSource -----------------------------------------------------------------------------------------------------

bool DiskSource::list(uint32_t /*placeIndex*/, const PlaceSpec& place, std::string_view relPath, Listing& out) {
  const std::wstring dir = winPath(joinPath(place.root, relPath));
  HANDLE h = CreateFileW(dir.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  uint64_t volume = 0;
  FILE_ID_INFO fid{};
  if (GetFileInformationByHandleEx(h, FileIdInfo, &fid, sizeof fid)) {
    volume = fid.VolumeSerialNumber;
  } else {
    BY_HANDLE_FILE_INFORMATION bh{};
    if (GetFileInformationByHandle(h, &bh)) volume = bh.dwVolumeSerialNumber;
  }
  if (buffer_.size() < (64u << 10)) buffer_.resize(64u << 10);
  bool first = true;
  for (;;) {
    if (!GetFileInformationByHandleEx(h, first ? FileIdExtdDirectoryRestartInfo : FileIdExtdDirectoryInfo,
                                      buffer_.data(), static_cast<DWORD>(buffer_.size()))) {
      const DWORD err = GetLastError();
      CloseHandle(h);
      if (err == ERROR_NO_MORE_FILES) return true;
      if (first && (err == ERROR_INVALID_PARAMETER || err == ERROR_NOT_SUPPORTED ||
                    err == ERROR_INVALID_FUNCTION)) {
        out.files.clear();
        out.subdirs.clear();
        return listFallback(dir, volume, out);
      }
      return false;
    }
    first = false;
    const uint8_t* p = buffer_.data();
    for (;;) {
      const auto* info = reinterpret_cast<const FILE_ID_EXTD_DIR_INFO*>(p);
      const std::wstring_view name(info->FileName, info->FileNameLength / sizeof(wchar_t));
      if (!skipEntry(name, info->FileAttributes, info->ReparsePointTag)) {
        if (info->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
          out.subdirs.push_back(toUtf8(name));
        } else {
          addFile(out, name, static_cast<uint64_t>(info->EndOfFile.QuadPart), info->LastWriteTime.QuadPart, volume,
                  info->FileId.Identifier, sizeof info->FileId.Identifier, info->FileAttributes);
        }
      }
      if (info->NextEntryOffset == 0) break;
      p += info->NextEntryOffset;
    }
  }
}

// ---- Probing --------------------------------------------------------------------------------------------------------

void probeFile(const std::string& absPath, const PlaceSpec& place, std::string_view relDir, EntryRecord& r) {
  Category sniffed = Category::None;
  if (r.kind == ItemKind::Sample && !(r.flags & kFlagOffline)) {
    auto info = media::probe(std::filesystem::path(winPath(absPath)));
    if (info) {
      r.frames = info->frames;
      r.rate = info->sampleRate;
      r.channels = static_cast<uint16_t>(std::min<uint32_t>(info->channels, 0xFFFF));
      r.bits = static_cast<uint8_t>(std::min<uint16_t>(info->bitsPerSample, 255));
      r.container = static_cast<uint8_t>(info->container);
      r.rootKey = static_cast<int8_t>(info->rootKey >= 0 && info->rootKey <= 127 ? info->rootKey : -1);
      r.bpm = static_cast<float>(info->bpm);
      if (info->framesScanned) r.flags |= kFlagFramesScanned;
      if (info->hasLoop()) r.flags |= kFlagHasLoop;
      if (info->bpmSource == media::BpmSource::FileName) r.flags |= kFlagBpmFromName;
    } else {
      r.flags |= kFlagProbeFailed;
    }
  } else if ((r.kind == ItemKind::Preset || r.kind == ItemKind::Device) && !(r.flags & kFlagOffline)) {
    HANDLE h = CreateFileW(winPath(absPath).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
      char head[4096];
      DWORD got = 0;
      if (ReadFile(h, head, sizeof head, &got, nullptr)) sniffed = sniffDeviceCategory(std::string_view(head, got));
      CloseHandle(h);
    }
  }
  r.category = categorize(r.kind, place.kind, relDir, r.name, sniffed);
}

// ---- Rebuild --------------------------------------------------------------------------------------------------------

namespace {

struct IdKey {
  uint64_t volume;
  FileIdBytes id;
  bool operator==(const IdKey&) const = default;
};
struct IdKeyHash {
  std::size_t operator()(const IdKey& k) const noexcept {
    uint64_t a, b;
    std::memcpy(&a, k.id.data(), 8);
    std::memcpy(&b, k.id.data() + 8, 8);
    uint64_t h = k.volume * 0x9E3779B97F4A7C15ull ^ a;
    h = (h ^ (h >> 31)) * 0xBF58476D1CE4E5B9ull ^ b;
    return static_cast<std::size_t>(h ^ (h >> 29));
  }
};

bool hasId(const FileIdBytes& id) {
  for (uint8_t b : id)
    if (b) return true;
  return false;
}

// Probe fields (and category / flags from probing) of `from` onto `to`, keeping to's identity and name.
void takeProbe(EntryRecord& to, const EntryRecord& from) {
  to.category = from.category;
  to.flags = static_cast<uint8_t>((from.flags & ~kFlagOffline) | (to.flags & kFlagOffline));
  to.frames = from.frames;
  to.rate = from.rate;
  to.channels = from.channels;
  to.bits = from.bits;
  to.container = from.container;
  to.rootKey = from.rootKey;
  to.bpm = from.bpm;
}

class Builder {
public:
  Builder(const Index* old, const ScanRequest& req, ListingSource& src, const ProbeFn& probe, ScanResult& res)
      : old_(old), req_(req), src_(src), probe_(probe), res_(res), out_(*res.index) {
    if (old_) {
      const uint32_t n = old_->dirCount();
      childStart_.assign(n + 1, 0);
      for (uint32_t d = 0; d < n; ++d)
        if (old_->dirs.parent[d] != Index::kNone) ++childStart_[old_->dirs.parent[d] + 1];
      for (uint32_t d = 0; d < n; ++d) childStart_[d + 1] += childStart_[d];
      children_.resize(childStart_[n]);
      std::vector<uint32_t> fill(childStart_.begin(), childStart_.end() - 1);
      for (uint32_t d = 0; d < n; ++d)
        if (old_->dirs.parent[d] != Index::kNone) children_[fill[old_->dirs.parent[d]]++] = d;
    }
  }

  void run() {
    out_.places = req_.places;
    out_.placeOnline.assign(req_.places.size(), 1);
    if (old_ && old_->places.size() != req_.places.size()) res_.stats.changed = true;
    for (uint32_t p = 0; p < req_.places.size(); ++p) {
      const PlaceSpec& place = req_.places[p];
      uint32_t oldRoot = Index::kNone;
      uint32_t oldPlace = Index::kNone;
      if (old_) {
        const std::string key = foldedPathKey(place.root);
        for (uint32_t q = 0; q < old_->places.size(); ++q)
          if (foldedPathKey(old_->places[q].root) == key) {
            oldPlace = q;
            break;
          }
        if (oldPlace != Index::kNone) {
          for (uint32_t d = 0; d < old_->dirCount(); ++d)
            if (old_->dirs.parent[d] == Index::kNone && old_->dirs.place[d] == oldPlace) {
              oldRoot = d;
              break;
            }
          if (oldPlace != p || !(old_->places[oldPlace] == place)) res_.stats.changed = true;
        } else {
          res_.stats.changed = true;
        }
      }
      dirty_.clear();
      recursive_.clear();
      // A place without dirty information (or new to the index) is relisted completely.
      full_ = oldRoot == Index::kNone || p >= req_.dirty.size();
      if (p < req_.dirty.size()) {
        const ScanRequest::PlaceDirty& pd = req_.dirty[p];
        full_ = full_ || pd.full;
        for (const std::string& d : pd.dirs) dirty_.insert(folded(d));
        for (const std::string& d : pd.recursive) recursive_.insert(folded(d));
      }
      placeIndex_ = p;
      place_ = &place;
      const bool wasOnline = oldPlace == Index::kNone || oldPlace >= old_->placeOnline.size() ||
                             old_->placeOnline[oldPlace] != 0;
      emit(std::string(), std::string_view(), oldRoot, Index::kNone, 0, full_);
      if (old_ && oldPlace != Index::kNone && wasOnline != (out_.placeOnline[p] != 0)) res_.stats.changed = true;
    }
    if (old_ && (old_->entryCount() != out_.entryCount() || old_->dirCount() != out_.dirCount()))
      res_.stats.changed = true;
    if (!old_) res_.stats.changed = true;
  }

private:
  bool isDirty(const std::string& rel, bool* recursive) const {
    if (dirty_.empty() && recursive_.empty()) return false;
    const std::string k = folded(rel);
    if (recursive_.count(k)) {
      *recursive = true;
      return true;
    }
    return dirty_.count(k) != 0;
  }

  void copyEntries(uint32_t oldDir, uint32_t newDir) {
    const uint32_t first = old_->dirs.firstEntry[oldDir];
    const uint32_t count = old_->dirs.entryCount[oldDir];
    if (count == 0) return;
    Index::Entries& e = out_.entries;
    const Index::Entries& o = old_->entries;
    const std::size_t at = e.dir.size();
    auto copyCol = [&](auto& dst, const auto& src) { dst.insert(dst.end(), src.begin() + first, src.begin() + first + count); };
    copyCol(e.dir, o.dir);
    copyCol(e.nameOff, o.nameOff);
    copyCol(e.nameLen, o.nameLen);
    copyCol(e.stemLen, o.stemLen);
    copyCol(e.kind, o.kind);
    copyCol(e.category, o.category);
    copyCol(e.flags, o.flags);
    copyCol(e.depth, o.depth);
    copyCol(e.size, o.size);
    copyCol(e.mtime, o.mtime);
    copyCol(e.volume, o.volume);
    copyCol(e.fileId, o.fileId);
    copyCol(e.frames, o.frames);
    copyCol(e.rate, o.rate);
    copyCol(e.channels, o.channels);
    copyCol(e.bits, o.bits);
    copyCol(e.container, o.container);
    copyCol(e.rootKey, o.rootKey);
    copyCol(e.bpm, o.bpm);
    copyCol(e.mask, o.mask);
    for (std::size_t i = at; i < at + count; ++i) {
      e.dir[i] = newDir;
      const uint32_t off = e.nameOff[i];
      e.nameOff[i] = static_cast<uint32_t>(out_.strings.size());
      out_.strings.insert(out_.strings.end(), old_->strings.begin() + off, old_->strings.begin() + off + e.nameLen[i]);
    }
    out_.dirs.entryCount[newDir] += count;
  }

  const EntryRecord* findMoved(const EntryRecord& f) {
    if (!old_ || !hasId(f.fileId)) return nullptr;
    if (!byIdBuilt_) {
      byIdBuilt_ = true;
      byId_.reserve(old_->entryCount());
      for (uint32_t e = 0; e < old_->entryCount(); ++e)
        if (hasId(old_->entries.fileId[e])) byId_.emplace(IdKey{old_->entries.volume[e], old_->entries.fileId[e]}, e);
    }
    const auto it = byId_.find(IdKey{f.volume, f.fileId});
    if (it == byId_.end()) return nullptr;
    const uint32_t e = it->second;
    if (old_->entries.size[e] != f.size || old_->entries.mtime[e] != f.mtime || old_->entries.kind[e] != uint8_t(f.kind))
      return nullptr;
    movedScratch_ = old_->record(e);
    return &movedScratch_;
  }

  // Emits directory `rel` (name `name`) and everything below it.
  void emit(const std::string& rel, std::string_view name, uint32_t oldDir, uint32_t parentNew, uint16_t depth,
            bool listAll) {
    if (req_.cancel && req_.cancel->load(std::memory_order_relaxed)) {
      res_.stats.cancelled = true;
      return;
    }
    const uint32_t newDir = out_.addDir(parentNew, name, static_cast<uint16_t>(placeIndex_), depth);
    bool recursive = false;
    const bool dirty = isDirty(rel, &recursive);
    const bool childListAll = listAll || recursive;
    if (!(listAll || oldDir == Index::kNone || dirty)) {
      ++res_.stats.dirsCopied;
      copyEntries(oldDir, newDir);
      for (uint32_t c = childStart_[oldDir]; c < childStart_[oldDir + 1]; ++c) {
        const uint32_t od = children_[c];
        const std::string_view cn = old_->dirName(od);
        emit(childRel(rel, cn), cn, od, newDir, static_cast<uint16_t>(depth + 1), false);
      }
      return;
    }

    Listing listing;
    if (!src_.list(placeIndex_, *place_, rel, listing)) {
      ++res_.stats.listFailures;
      if (depth == 0) out_.placeOnline[placeIndex_] = 0;
      if (oldDir != Index::kNone) {
        copyEntries(oldDir, newDir);
        for (uint32_t c = childStart_[oldDir]; c < childStart_[oldDir + 1]; ++c) {
          const uint32_t od = children_[c];
          const std::string_view cn = old_->dirName(od);
          emit(childRel(rel, cn), cn, od, newDir, static_cast<uint16_t>(depth + 1), false);
        }
      }
      return;
    }
    ++res_.stats.dirsListed;
    std::sort(listing.files.begin(), listing.files.end(),
              [](const EntryRecord& a, const EntryRecord& b) { return compareFolded(a.name, b.name) < 0; });
    std::sort(listing.subdirs.begin(), listing.subdirs.end(),
              [](const std::string& a, const std::string& b) { return compareFolded(a, b) < 0; });

    bool changed = oldDir == Index::kNone;
    uint32_t o = oldDir != Index::kNone ? old_->dirs.firstEntry[oldDir] : 0;
    const uint32_t oEnd = oldDir != Index::kNone ? o + old_->dirs.entryCount[oldDir] : 0;
    for (EntryRecord& f : listing.files) {
      while (o < oEnd && compareFolded(old_->name(o), f.name) < 0) {
        ++o;
        changed = true;  // removed
      }
      if (o < oEnd && compareFolded(old_->name(o), f.name) == 0) {
        const uint32_t oe = o++;
        if (listing.probed) {
          if (!(old_->record(oe) == f)) changed = true;
        } else if (old_->entries.size[oe] == f.size && old_->entries.mtime[oe] == f.mtime &&
                   old_->entries.kind[oe] == uint8_t(f.kind) &&
                   (old_->entries.fileId[oe] == f.fileId || !hasId(f.fileId) || !hasId(old_->entries.fileId[oe]))) {
          ++res_.stats.filesReused;
          const EntryRecord prev = old_->record(oe);
          takeProbe(f, prev);
          if (!(prev == f)) changed = true;  // identity or offline flag moved
        } else {
          changed = true;
          probeNew(rel, f);
        }
      } else {
        changed = true;
        if (!listing.probed) probeNew(rel, f);
      }
      out_.addEntry(newDir, f);
    }
    if (o < oEnd) changed = true;

    // Subdirectories: match the old children by name.
    std::vector<std::pair<std::string, uint32_t>> subs;
    subs.reserve(listing.subdirs.size());
    {
      uint32_t c = oldDir != Index::kNone ? childStart_[oldDir] : 0;
      const uint32_t cEnd = oldDir != Index::kNone ? childStart_[oldDir + 1] : 0;
      for (std::string& s : listing.subdirs) {
        while (c < cEnd && compareFolded(old_->dirName(children_[c]), s) < 0) {
          ++c;
          changed = true;
        }
        uint32_t od = Index::kNone;
        if (c < cEnd && compareFolded(old_->dirName(children_[c]), s) == 0) od = children_[c++];
        else changed = true;
        subs.emplace_back(s, od);
      }
      if (c < cEnd) changed = true;
    }

    if (changed) {
      DirChange ch;
      ch.placeRoot = place_->root;
      ch.relPath = rel;
      ch.listing.probed = true;
      ch.listing.subdirs = listing.subdirs;
      ch.listing.files = std::move(listing.files);
      res_.changes.push_back(std::move(ch));
      res_.stats.changed = true;
    }
    for (auto& [s, od] : subs) emit(childRel(rel, s), s, od, newDir, static_cast<uint16_t>(depth + 1), childListAll);
  }

  void probeNew(const std::string& rel, EntryRecord& f) {
    if (const EntryRecord* m = findMoved(f)) {
      ++res_.stats.filesMoved;
      takeProbe(f, *m);
      return;
    }
    ++res_.stats.filesProbed;
    if (probe_) probe_(joinPath(joinPath(place_->root, rel), f.name), *place_, rel, f);
    else probeFile(joinPath(joinPath(place_->root, rel), f.name), *place_, rel, f);
  }

  const Index* old_;
  const ScanRequest& req_;
  ListingSource& src_;
  const ProbeFn& probe_;
  ScanResult& res_;
  Index& out_;
  std::vector<uint32_t> childStart_{0};
  std::vector<uint32_t> children_;
  std::unordered_set<std::string> dirty_, recursive_;
  bool full_ = false;
  uint32_t placeIndex_ = 0;
  const PlaceSpec* place_ = nullptr;
  bool byIdBuilt_ = false;
  std::unordered_map<IdKey, uint32_t, IdKeyHash> byId_;
  EntryRecord movedScratch_;
};

// Replays recorded listings.
class RecordSource final : public ListingSource {
public:
  explicit RecordSource(const std::vector<DirChange>& changes) {
    for (const DirChange& c : changes) map_[key(c.placeRoot, c.relPath)] = &c;  // later records win
  }
  bool list(uint32_t, const PlaceSpec& place, std::string_view relPath, Listing& out) override {
    const auto it = map_.find(key(place.root, relPath));
    if (it == map_.end()) return false;
    out = it->second->listing;
    out.probed = true;
    return true;
  }
  static std::string key(std::string_view root, std::string_view rel) {
    return foldedPathKey(root) + '\x01' + folded(rel);
  }

private:
  std::unordered_map<std::string, const DirChange*> map_;
};

}  // namespace

ScanResult rebuild(const Index* old, const ScanRequest& req, ListingSource& source, const ProbeFn& probe) {
  ScanResult res;
  res.index = std::make_shared<Index>();
  if (old) {
    // Reserve the old sizes: an unchanged rebuild then never reallocates a column.
    res.index->dirs.forEachColumn([&](auto& c) { c.reserve(old->dirCount()); });
    res.index->entries.forEachColumn([&](auto& c) { c.reserve(old->entryCount()); });
    res.index->strings.reserve(old->strings.size());
  }
  Builder b(old, req, source, probe, res);
  b.run();
  return res;
}

std::shared_ptr<Index> applyChanges(const Index& base, const std::vector<DirChange>& changes) {
  ScanRequest req;
  req.places = base.places;
  req.dirty.resize(base.places.size());
  for (const DirChange& c : changes) {
    const std::string k = foldedPathKey(c.placeRoot);
    for (uint32_t p = 0; p < base.places.size(); ++p)
      if (foldedPathKey(base.places[p].root) == k) req.dirty[p].dirs.push_back(c.relPath);
  }
  RecordSource src(changes);
  const ProbeFn none = [](const std::string&, const PlaceSpec&, std::string_view, EntryRecord&) {};
  ScanResult r = rebuild(&base, req, src, none);
  // A recorded directory that could not be replayed keeps its old contents (RecordSource fails only for directories
  // without a record, which rebuild() copies); the online flags are the base's.
  r.index->placeOnline = base.placeOnline;
  r.index->placeOnline.resize(r.index->places.size(), 1);
  return r.index;
}

}  // namespace pq::library
