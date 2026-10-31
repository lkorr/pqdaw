// Project samples (T5.8): pq/document/samples.h.
#include "pq/document/samples.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <system_error>
#include <unordered_map>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "pq/document/serialize.h"

namespace pq::doc {

namespace {

namespace fs = std::filesystem;

std::wstring widen(std::string_view utf8) {
  if (utf8.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(std::max(n, 0)), L'\0');
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), w.data(), n);
  return w;
}

std::string narrow(std::wstring_view w) {
  if (w.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<size_t>(std::max(n, 0)), '\0');
  if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

std::string slashes(std::string s) {
  std::replace(s.begin(), s.end(), '\\', '/');
  return s;
}

fs::path pathOf(std::string_view utf8) { return fs::path(widen(utf8)); }
std::string utf8Of(const fs::path& p) { return slashes(narrow(p.wstring())); }

Error ioError(std::string what, std::string_view path, DWORD code) {
  return Error{ErrorCode::Io, std::move(what) + " (Windows error " + std::to_string(code) + ")", {std::string(path), 0, 0}};
}

struct Handle {
  HANDLE h = INVALID_HANDLE_VALUE;
  ~Handle() {
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
  }
};

bool fileExists(const fs::path& p) {
  const DWORD a = GetFileAttributesW(p.c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// Case-insensitive file-name key (NTFS names compare without case).
std::wstring nameKey(const fs::path& p) {
  std::wstring n = p.filename().wstring();
  CharLowerBuffW(n.data(), static_cast<DWORD>(n.size()));
  return n;
}

// Whether `child` lies inside `dir` (lexically, case-insensitive; both absolute).
bool inside(const fs::path& child, const fs::path& dir) {
  if (dir.empty()) return false;
  std::wstring c = child.lexically_normal().wstring(), d = dir.lexically_normal().wstring();
  CharLowerBuffW(c.data(), static_cast<DWORD>(c.size()));
  CharLowerBuffW(d.data(), static_cast<DWORD>(d.size()));
  std::replace(c.begin(), c.end(), L'/', L'\\');
  std::replace(d.begin(), d.end(), L'/', L'\\');
  while (!d.empty() && d.back() == L'\\') d.pop_back();
  return c.size() > d.size() + 1 && c.compare(0, d.size(), d) == 0 && c[d.size()] == L'\\';
}

// Whether the file at `p` is the sample: same size, and the same content hash when the entry has one.
bool matches(const fs::path& p, const SampleRef& r) {
  Result<FileIdentity> id = fileIdentity(utf8Of(p));
  if (!id) return false;
  if (r.size != 0 && id->size != r.size) return false;
  return r.contentHash == 0 || id->hash == r.contentHash;
}

// Searches `roots` recursively for the entries in `want` (by file name, then verified). found[id] = the file.
void search(std::span<const fs::path> roots, const std::vector<const SampleRef*>& want,
            std::unordered_map<uint32_t, fs::path>& found) {
  std::unordered_multimap<std::wstring, const SampleRef*> byName;
  for (const SampleRef* r : want) byName.emplace(nameKey(pathOf(r->path)), r);
  for (const fs::path& root : roots) {
    if (byName.empty() || root.empty()) continue;
    std::error_code ec;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
      if (!it->is_regular_file(ec)) continue;
      const auto range = byName.equal_range(nameKey(it->path()));
      for (auto m = range.first; m != range.second;) {
        if (matches(it->path(), *m->second)) {
          found.emplace(m->second->id.value, it->path());
          m = byName.erase(m);
        } else {
          ++m;
        }
      }
      if (byName.empty()) break;
    }
  }
}

std::vector<const SampleRef*> usedSamples(const Project& p) {
  std::vector<uint32_t> used;
  forEachTrack(p, [&](const Track& t, const Track*) {
    for (const ClipSlot& s : t.clips)
      if (s.audio) used.push_back(s.audio->sample.value);
  });
  std::sort(used.begin(), used.end());
  std::vector<const SampleRef*> out;
  for (const SampleRef& r : samplesOf(p))
    if (std::binary_search(used.begin(), used.end(), r.id.value)) out.push_back(&r);
  return out;
}

}  // namespace

Result<FileIdentity> fileIdentity(std::string_view pathUtf8) {
  Handle f;
  f.h = CreateFileW(widen(pathUtf8).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f.h == INVALID_HANDLE_VALUE) {
    const DWORD code = GetLastError();
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
      return Error{ErrorCode::NotFound, "no file at " + std::string(pathUtf8), {std::string(pathUtf8), 0, 0}};
    return ioError("cannot open " + std::string(pathUtf8), pathUtf8, code);
  }
  LARGE_INTEGER sz{};
  if (!GetFileSizeEx(f.h, &sz)) return ioError("cannot read the size of " + std::string(pathUtf8), pathUtf8, GetLastError());
  FileIdentity out;
  out.size = static_cast<uint64_t>(sz.QuadPart);
  uint64_t h = 1469598103934665603ull;
  auto mix = [&h](const unsigned char* b, size_t n) {
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
  };
  mix(reinterpret_cast<const unsigned char*>(&out.size), sizeof out.size);
  constexpr uint64_t kWindow = 64 * 1024;
  std::vector<unsigned char> buf(kWindow);
  const uint64_t offsets[] = {0, out.size > kWindow ? (out.size - kWindow) / 2 : 0, out.size > kWindow ? out.size - kWindow : 0};
  for (size_t w = 0; w < 3; ++w) {
    if (w > 0 && out.size <= kWindow) break;  // one window covers the file
    LARGE_INTEGER at{};
    at.QuadPart = static_cast<LONGLONG>(offsets[w]);
    if (!SetFilePointerEx(f.h, at, nullptr, FILE_BEGIN)) return ioError("cannot seek in " + std::string(pathUtf8), pathUtf8, GetLastError());
    DWORD got = 0;
    const auto want = static_cast<DWORD>(std::min<uint64_t>(kWindow, out.size));
    if (!ReadFile(f.h, buf.data(), want, &got, nullptr)) return ioError("cannot read " + std::string(pathUtf8), pathUtf8, GetLastError());
    mix(buf.data(), got);
  }
  out.hash = h;
  return out;
}

bool isAbsoluteSamplePath(std::string_view p) noexcept {
  if (p.size() >= 3 && ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':' &&
      (p[2] == '/' || p[2] == '\\'))
    return true;
  return p.size() >= 2 && (p[0] == '/' || p[0] == '\\') && (p[1] == '/' || p[1] == '\\');
}

std::string storedSamplePath(std::string_view absPath, std::string_view projectDir) {
  const fs::path f = pathOf(absPath).lexically_normal();
  if (!projectDir.empty()) {
    const fs::path d = pathOf(projectDir).lexically_normal();
    if (inside(f, d)) return utf8Of(f.lexically_relative(d));
  }
  return utf8Of(f);
}

std::string absoluteSamplePath(std::string_view stored, std::string_view projectDir) {
  if (isAbsoluteSamplePath(stored) || projectDir.empty()) return slashes(std::string(stored));
  return utf8Of((pathOf(projectDir) / pathOf(stored)).lexically_normal());
}

Result<SampleRef> makeSampleRef(IdSource& ids, std::string_view absPath, std::string_view projectDir,
                                const SampleProbeFn& probe) {
  if (!probe) return Error{ErrorCode::InvalidArgument, "no sample probe", {}};
  Result<FileIdentity> id = fileIdentity(absPath);
  if (!id) return std::move(id).error();
  Result<SampleProbe> pr = probe(absPath);
  if (!pr) return std::move(pr).error();
  SampleRef r;
  r.path = storedSamplePath(absPath, projectDir);
  r.contentHash = id->hash;
  r.size = id->size;
  r.frames = pr->frames;
  r.rate = pr->rate;
  r.channels = pr->channels;
  r.displayName = narrow(pathOf(absPath).stem().wstring());
  if (r.frames < 1 || !(r.rate > 0.0) || r.channels < 1)
    return Error{ErrorCode::InvalidArgument, "the probe found no audio in " + std::string(absPath), {}};
  r.id = ids.sample();
  return r;
}

std::vector<SampleResolution> resolveSamples(const Project& p, const SampleSearch& s) {
  std::vector<SampleResolution> out;
  std::vector<const SampleRef*> lost;
  for (const SampleRef& r : samplesOf(p)) {
    SampleResolution res;
    res.id = r.id;
    const bool relative = !isAbsoluteSamplePath(r.path);
    if (!(relative && s.projectDir.empty())) {
      const std::string abs = absoluteSamplePath(r.path, s.projectDir);
      if (fileExists(pathOf(abs))) res.path = abs;
    }
    if (res.path.empty()) lost.push_back(&r);
    out.push_back(std::move(res));
  }
  if (lost.empty()) return out;
  std::vector<fs::path> roots;
  if (!s.projectDir.empty()) roots.push_back(pathOf(s.projectDir));
  for (const std::string& f : s.folders) roots.push_back(pathOf(f));
  std::unordered_map<uint32_t, fs::path> found;
  search(roots, lost, found);
  for (SampleResolution& res : out) {
    const auto it = found.find(res.id.value);
    if (it == found.end()) continue;
    res.path = utf8Of(it->second);
    res.relocated = storedSamplePath(res.path, s.projectDir);
  }
  return out;
}

std::vector<SampleId> missingSamples(const Project& p, std::span<const SampleResolution> resolved) {
  std::vector<SampleId> out;
  const std::vector<const SampleRef*> used = usedSamples(p);
  for (const SampleResolution& r : resolved)
    if (r.path.empty() && std::any_of(used.begin(), used.end(), [&](const SampleRef* u) { return u->id == r.id; }))
      out.push_back(r.id);
  return out;
}

std::vector<SampleRef> relinkSamples(const Project& p, std::span<const SampleId> ids, std::string_view folder,
                                     std::string_view projectDir) {
  std::vector<const SampleRef*> want;
  for (const SampleId id : ids)
    if (const SampleRef* r = findSample(p, id)) want.push_back(r);
  std::unordered_map<uint32_t, fs::path> found;
  const fs::path root = pathOf(folder);
  search(std::span<const fs::path>(&root, 1), want, found);
  std::vector<SampleRef> out;
  for (const SampleRef* r : want) {
    const auto it = found.find(r->id.value);
    if (it == found.end()) continue;
    SampleRef n = *r;
    n.path = storedSamplePath(utf8Of(it->second), projectDir);
    out.push_back(std::move(n));
  }
  return out;
}

Result<CollectPlan> planCollect(const Project& p, std::string_view projectDir, std::span<const SampleResolution> resolved) {
  if (projectDir.empty()) return Error{ErrorCode::InvalidArgument, "an untitled project has no samples folder", {}};
  const fs::path dir = pathOf(projectDir);
  const fs::path dest = dir / L"samples" / L"imported";
  CollectPlan plan;
  std::map<std::wstring, FileIdentity> taken;        // destination name key → what it holds (planned or on disk)
  std::map<std::wstring, size_t> bySource;           // source key → the item copying it
  for (const SampleRef* r : usedSamples(p)) {
    const auto res = std::find_if(resolved.begin(), resolved.end(), [&](const SampleResolution& x) { return x.id == r->id; });
    if (res == resolved.end() || res->path.empty()) continue;  // missing: nothing to copy
    const fs::path from = pathOf(res->path);
    if (inside(from, dir)) continue;  // already in the project
    std::wstring srcKey = from.lexically_normal().wstring();
    CharLowerBuffW(srcKey.data(), static_cast<DWORD>(srcKey.size()));
    CollectItem item;
    item.id = r->id;
    item.from = res->path;
    if (const auto again = bySource.find(srcKey); again != bySource.end()) {  // another entry plays the same file
      const CollectItem& first = plan.items[again->second];
      item.to = first.to;
      item.stored = first.stored;
      item.bytes = first.bytes;
      item.copy = false;
      plan.items.push_back(std::move(item));
      continue;
    }
    Result<FileIdentity> idf = fileIdentity(res->path);
    if (!idf) return std::move(idf).error();
    const fs::path stem = from.stem(), ext = from.extension();
    for (uint32_t n = 1;; ++n) {
      fs::path name = stem;
      if (n > 1) name += L" (" + std::to_wstring(n) + L")";
      name += ext;
      const fs::path to = dest / name;
      const std::wstring key = nameKey(to);
      auto t = taken.find(key);
      if (t == taken.end() && fileExists(to)) {
        Result<FileIdentity> there = fileIdentity(utf8Of(to));
        t = taken.emplace(key, there ? *there : FileIdentity{~0ull, 0}).first;
      }
      if (t != taken.end()) {
        if (!(t->second == *idf)) continue;  // a different file has this name: try the next suffix
        item.copy = std::none_of(plan.items.begin(), plan.items.end(), [&](const CollectItem& x) { return nameKey(pathOf(x.to)) == key; }) &&
                    !fileExists(to);  // identical file: reuse it (or the item already copying it)
      } else {
        taken.emplace(key, *idf);
      }
      item.to = utf8Of(to);
      item.stored = storedSamplePath(item.to, projectDir);
      break;
    }
    item.bytes = idf->size;
    if (item.copy) plan.bytes += item.bytes;
    bySource.emplace(srcKey, plan.items.size());
    plan.items.push_back(std::move(item));
  }
  return plan;
}

namespace {
struct CopyState {
  const CollectOptions* o;
  uint64_t before = 0;  // bytes of earlier files
  uint64_t total = 0;
  uint64_t failAt = UINT64_MAX;
  bool failed = false;
};
DWORD CALLBACK copyProgress(LARGE_INTEGER, LARGE_INTEGER transferred, LARGE_INTEGER, LARGE_INTEGER, DWORD, DWORD, HANDLE,
                            HANDLE, LPVOID user) {
  auto* s = static_cast<CopyState*>(user);
  const uint64_t done = s->before + static_cast<uint64_t>(transferred.QuadPart);
  if (s->o->progress) s->o->progress(done, s->total);
  if (done >= s->failAt) {
    s->failed = true;
    return PROGRESS_CANCEL;
  }
  if (s->o->cancel != nullptr && s->o->cancel->load(std::memory_order_relaxed)) return PROGRESS_CANCEL;
  return PROGRESS_CONTINUE;
}
}  // namespace

Status runCollect(const CollectPlan& plan, const CollectOptions& o) {
  CopyState st{&o, 0, plan.bytes, o.failAt == CollectFailPoint::MidCopy ? plan.bytes / 2 : UINT64_MAX, false};
  bool madeDir = false;
  for (const CollectItem& it : plan.items) {
    if (!it.copy) continue;
    const fs::path to = pathOf(it.to);
    if (!madeDir) {
      std::error_code ec;
      fs::create_directories(to.parent_path(), ec);
      if (ec) return Error{ErrorCode::Io, "cannot create " + utf8Of(to.parent_path()) + ": " + ec.message(), {}};
      madeDir = true;
    }
    fs::path part = to;
    part += L".part";
    BOOL cancelFlag = FALSE;
    if (!CopyFileExW(pathOf(it.from).c_str(), part.c_str(), copyProgress, &st, &cancelFlag, 0)) {
      const DWORD code = GetLastError();
      DeleteFileW(part.c_str());
      if (st.failed) return ioError("simulated failure while copying " + it.from, it.from, ERROR_DISK_FULL);
      if (code == ERROR_REQUEST_ABORTED) return Error{ErrorCode::Io, "Collect All and Save cancelled", {}};
      return ioError("cannot copy " + it.from, it.from, code);
    }
    if (!MoveFileExW(part.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH)) {
      const DWORD code = GetLastError();
      DeleteFileW(part.c_str());
      return ioError("cannot rename into " + it.to, it.to, code);
    }
    st.before += it.bytes;
  }
  if (o.progress) o.progress(plan.bytes, plan.bytes);
  return Ok{};
}

std::vector<SampleRef> collectedRefs(const Project& p, const CollectPlan& plan) {
  std::vector<SampleRef> out;
  for (const CollectItem& it : plan.items)
    if (const SampleRef* r = findSample(p, it.id)) {
      SampleRef n = *r;
      n.path = it.stored;
      if (!(n == *r)) out.push_back(std::move(n));
    }
  return out;
}

Status collectAllAndSave(DocumentStore& store, std::string_view dirUtf8, const SampleSearch& search,
                         const CollectOptions& options) {
  const Snapshot snap = store.current();
  SampleSearch s = search;
  if (s.projectDir.empty()) s.projectDir = std::string(dirUtf8);
  const std::vector<SampleResolution> res = resolveSamples(*snap, s);
  Result<CollectPlan> plan = planCollect(*snap, dirUtf8, res);
  if (!plan) return std::move(plan).error();
  if (Status st = runCollect(*plan, options); !st) return st;
  store.relocateSamples(collectedRefs(*snap, *plan));
  return saveProject(dirUtf8, store);
}

}  // namespace pq::doc
