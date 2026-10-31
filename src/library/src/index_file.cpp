#include "pq/library/index_file.h"

#include <windows.h>
#include <shlobj.h>

#include <cstring>
#include <new>
#include <type_traits>

#include "win_util.h"

namespace pq::library {

using detail::toUtf8;

namespace {

constexpr char kSnapshotMagic[8] = {'P', 'Q', 'L', 'I', 'B', 'I', 'D', 'X'};
constexpr char kJournalMagic[8] = {'P', 'Q', 'L', 'I', 'B', 'J', 'N', 'L'};
constexpr uint32_t kRecordMagic = 0x4C434552u;  // "RECL"
constexpr std::size_t kHeaderBytes = 64;
constexpr std::size_t kJournalHeaderBytes = 32;
constexpr uint32_t kMaxRecordBytes = 256u << 20;

// FNV-1a over 64-bit little-endian words (the tail zero-padded): ~8× faster than bytewise, enough to catch a torn or
// corrupt file (the files are not adversarial: they live in the user's own cache directory).
uint64_t hashWords(const uint8_t* p, std::size_t n) noexcept {
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

class Writer {
public:
  std::vector<uint8_t> bytes;
  void raw(const void* p, std::size_t n) {
    const auto* b = static_cast<const uint8_t*>(p);
    bytes.insert(bytes.end(), b, b + n);
  }
  template <class T> void pod(const T& v) {
    static_assert(std::is_trivially_copyable_v<T>);
    raw(&v, sizeof v);
  }
  void str(std::string_view s) {
    pod(static_cast<uint32_t>(s.size()));
    raw(s.data(), s.size());
  }
  void align8() { bytes.resize((bytes.size() + 7) & ~std::size_t{7}, 0); }
};

class Reader {
public:
  Reader(const uint8_t* p, std::size_t n) : p_(p), n_(n) {}
  bool ok() const noexcept { return ok_; }
  std::size_t pos() const noexcept { return at_; }
  std::size_t remaining() const noexcept { return n_ - at_; }
  void fail() noexcept { ok_ = false; }
  bool raw(void* dst, std::size_t n) {
    if (!ok_ || n > n_ - at_) return ok_ = false;
    if (n) std::memcpy(dst, p_ + at_, n);
    at_ += n;
    return true;
  }
  template <class T> bool pod(T& v) { return raw(&v, sizeof v); }
  bool str(std::string& s) {
    uint32_t len = 0;
    if (!pod(len) || len > n_ - at_) return ok_ = false;
    s.assign(reinterpret_cast<const char*>(p_ + at_), len);
    at_ += len;
    return true;
  }
  void align8() {
    const std::size_t a = (at_ + 7) & ~std::size_t{7};
    if (a > n_) ok_ = false;
    else at_ = a;
  }

private:
  const uint8_t* p_;
  std::size_t n_;
  std::size_t at_ = 0;
  bool ok_ = true;
};

// The snapshot checksum: header bytes 8–55 (version, sizes, sequence, every count) and the payload (M5 review H8: the
// counts were outside it, so one flipped bit sized the columns in gigabytes).
uint64_t snapshotHash(const uint8_t* file, std::size_t payloadBytes) noexcept {
  return hashWords(file + 8, 48) * 0x9E3779B97F4A7C15ull ^ hashWords(file + kHeaderBytes, payloadBytes);
}

Error parseError(const std::filesystem::path& file, const char* what) {
  return Error{ErrorCode::Parse, std::string("library index ") + toUtf8(file.native()) + ": " + what, {}};
}

void encodeRecord(Writer& w, const EntryRecord& r) {
  w.str(r.name);
  w.pod(static_cast<uint8_t>(r.kind));
  w.pod(static_cast<uint8_t>(r.category));
  w.pod(r.flags);
  w.pod(r.size);
  w.pod(r.mtime);
  w.pod(r.volume);
  w.raw(r.fileId.data(), 16);
  w.pod(r.frames);
  w.pod(r.rate);
  w.pod(r.channels);
  w.pod(r.bits);
  w.pod(r.container);
  w.pod(r.rootKey);
  w.pod(r.bpm);
}

bool decodeRecord(Reader& rd, EntryRecord& r) {
  uint8_t kind = 0, cat = 0;
  rd.str(r.name);
  rd.pod(kind);
  rd.pod(cat);
  rd.pod(r.flags);
  rd.pod(r.size);
  rd.pod(r.mtime);
  rd.pod(r.volume);
  rd.raw(r.fileId.data(), 16);
  rd.pod(r.frames);
  rd.pod(r.rate);
  rd.pod(r.channels);
  rd.pod(r.bits);
  rd.pod(r.container);
  rd.pod(r.rootKey);
  rd.pod(r.bpm);
  if (!rd.ok() || kind >= uint8_t(ItemKind::Count) || cat >= uint8_t(Category::Count)) return false;
  r.kind = static_cast<ItemKind>(kind);
  r.category = static_cast<Category>(cat);
  return true;
}

bool readWholeFile(const std::filesystem::path& file, std::vector<uint8_t>& out, DWORD* err) {
  HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                         FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    *err = GetLastError();
    return false;
  }
  LARGE_INTEGER size{};
  bool ok = GetFileSizeEx(h, &size) && size.QuadPart < (int64_t{4} << 30);
  if (ok) {
    out.resize(static_cast<std::size_t>(size.QuadPart));
    std::size_t at = 0;
    while (ok && at < out.size()) {
      const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(out.size() - at, 1u << 30));
      DWORD got = 0;
      ok = ReadFile(h, out.data() + at, chunk, &got, nullptr) && got > 0;
      at += got;
    }
  }
  *err = ok ? 0 : GetLastError();
  CloseHandle(h);
  return ok;
}

}  // namespace

// ---- Default locations ------------------------------------------------------------------------------------------

std::filesystem::path defaultLibraryDataDir() {
  wchar_t buf[MAX_PATH];
  const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return {};
  return std::filesystem::path(std::wstring(buf, n)) / L"pqdaw" / L"library";
}

std::filesystem::path defaultUserLibraryDir() {
  PWSTR docs = nullptr;
  std::filesystem::path out;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs)) && docs)
    out = std::filesystem::path(docs) / L"pqdaw" / L"User Library";
  CoTaskMemFree(docs);
  return out;
}

// ---- Snapshot -----------------------------------------------------------------------------------------------------

Status saveIndex(const Index& index, uint64_t sequence, const std::filesystem::path& file) {
  Writer w;
  w.bytes.reserve(kHeaderBytes + index.strings.size() + std::size_t{index.entryCount()} * 112 +
                  std::size_t{index.dirCount()} * 24 + 4096);
  w.bytes.resize(kHeaderBytes, 0);
  for (uint32_t p = 0; p < index.places.size(); ++p) {
    w.pod(static_cast<uint8_t>(index.places[p].kind));
    w.pod(static_cast<uint8_t>(p < index.placeOnline.size() ? index.placeOnline[p] : 1));
    w.str(index.places[p].root);
    w.str(index.places[p].name);
  }
  w.align8();
  auto col = [&](const auto& v) {
    w.raw(v.data(), v.size() * sizeof(v[0]));
    w.align8();
  };
  index.dirs.forEachColumn(col);
  index.entries.forEachColumn(col);
  w.raw(index.strings.data(), index.strings.size());
  w.align8();

  const uint64_t payloadBytes = w.bytes.size() - kHeaderBytes;
  uint8_t* h = w.bytes.data();
  std::memcpy(h, kSnapshotMagic, 8);
  const uint32_t version = kIndexFormatVersion, headerBytes = kHeaderBytes;
  const auto places = static_cast<uint32_t>(index.places.size());
  const uint32_t dirs = index.dirCount(), entries = index.entryCount(), reserved = 0;
  const uint64_t strings = index.strings.size();
  std::memcpy(h + 8, &version, 4);
  std::memcpy(h + 12, &headerBytes, 4);
  std::memcpy(h + 16, &sequence, 8);
  std::memcpy(h + 24, &places, 4);
  std::memcpy(h + 28, &dirs, 4);
  std::memcpy(h + 32, &entries, 4);
  std::memcpy(h + 36, &reserved, 4);
  std::memcpy(h + 40, &strings, 8);
  std::memcpy(h + 48, &payloadBytes, 8);
  const uint64_t hash = snapshotHash(h, payloadBytes);
  std::memcpy(h + 56, &hash, 8);

  std::error_code ec;
  if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
  if (!detail::writeFileAtomic(file.native(), w.bytes.data(), w.bytes.size()))
    return Error{ErrorCode::Io, "cannot write library index " + toUtf8(file.native()), {}};
  return Ok{};
}

namespace {
Result<LoadedIndex> loadIndexImpl(const std::filesystem::path& file);
}  // namespace

Result<LoadedIndex> loadIndex(const std::filesystem::path& file) {
  // The indexer's thread loads it: out of memory (a file far larger than any index) is a parse error, not a crash.
  try {
    return loadIndexImpl(file);
  } catch (const std::bad_alloc&) {
    return parseError(file, "too large");
  }
}

namespace {
Result<LoadedIndex> loadIndexImpl(const std::filesystem::path& file) {
  std::vector<uint8_t> bytes;
  DWORD err = 0;
  if (!readWholeFile(file, bytes, &err)) {
    if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
      return Error{ErrorCode::NotFound, "no library index " + toUtf8(file.native()), {}};
    return Error{ErrorCode::Io, "cannot read library index " + toUtf8(file.native()), {}};
  }
  if (bytes.size() < kHeaderBytes || std::memcmp(bytes.data(), kSnapshotMagic, 8) != 0)
    return parseError(file, "not an index file");
  uint32_t version = 0, headerBytes = 0, places = 0, dirs = 0, entries = 0;
  uint64_t sequence = 0, strings = 0, payloadBytes = 0, hash = 0;
  std::memcpy(&version, bytes.data() + 8, 4);
  std::memcpy(&headerBytes, bytes.data() + 12, 4);
  std::memcpy(&sequence, bytes.data() + 16, 8);
  std::memcpy(&places, bytes.data() + 24, 4);
  std::memcpy(&dirs, bytes.data() + 28, 4);
  std::memcpy(&entries, bytes.data() + 32, 4);
  std::memcpy(&strings, bytes.data() + 40, 8);
  std::memcpy(&payloadBytes, bytes.data() + 48, 8);
  std::memcpy(&hash, bytes.data() + 56, 8);
  if (version != kIndexFormatVersion) return parseError(file, "unsupported version");
  if (headerBytes != kHeaderBytes || payloadBytes != bytes.size() - kHeaderBytes)
    return parseError(file, "size mismatch");
  if (snapshotHash(bytes.data(), payloadBytes) != hash) return parseError(file, "checksum mismatch");
  // Counts are bounded by the payload before anything is sized from them (a hash collision must not allocate either).
  if (uint64_t{places} * 10 > payloadBytes || uint64_t{dirs} > payloadBytes || uint64_t{entries} > payloadBytes ||
      strings > payloadBytes)
    return parseError(file, "counts exceed the file");

  auto idx = std::make_shared<Index>();
  Reader rd(bytes.data() + kHeaderBytes, payloadBytes);
  for (uint32_t p = 0; p < places && rd.ok(); ++p) {
    uint8_t kind = 0, online = 0;
    PlaceSpec s;
    rd.pod(kind);
    rd.pod(online);
    rd.str(s.root);
    rd.str(s.name);
    if (kind > uint8_t(PlaceKind::Folder)) return parseError(file, "bad place");
    s.kind = static_cast<PlaceKind>(kind);
    idx->places.push_back(std::move(s));
    idx->placeOnline.push_back(online);
  }
  rd.align8();
  auto col = [&](auto& v, std::size_t n) {
    if (!rd.ok() || n > rd.remaining() / sizeof(v[0])) return rd.fail();  // never sized past the bytes that remain
    v.resize(n);
    rd.raw(v.data(), n * sizeof(v[0]));
    rd.align8();
  };
  idx->dirs.forEachColumn([&](auto& v) { col(v, dirs); });
  idx->entries.forEachColumn([&](auto& v) { col(v, entries); });
  if (!rd.ok() || strings > payloadBytes) return parseError(file, "truncated");
  idx->strings.resize(static_cast<std::size_t>(strings));
  rd.raw(idx->strings.data(), idx->strings.size());
  if (!rd.ok()) return parseError(file, "truncated");

  // Structural checks: every reference in range (a valid hash makes failures unlikely, but a bad index must never
  // make the browser read out of bounds).
  for (uint32_t d = 0; d < dirs; ++d) {
    const uint32_t parent = idx->dirs.parent[d];
    if ((parent != Index::kNone && parent >= d) || idx->dirs.place[d] >= places ||
        uint64_t{idx->dirs.nameOff[d]} + idx->dirs.nameLen[d] > strings ||
        uint64_t{idx->dirs.firstEntry[d]} + idx->dirs.entryCount[d] > entries)
      return parseError(file, "bad directory");
  }
  for (uint32_t e = 0; e < entries; ++e) {
    if (idx->entries.dir[e] >= dirs || uint64_t{idx->entries.nameOff[e]} + idx->entries.nameLen[e] > strings ||
        idx->entries.stemLen[e] > idx->entries.nameLen[e] || idx->entries.kind[e] >= uint8_t(ItemKind::Count) ||
        idx->entries.category[e] >= uint8_t(Category::Count))
      return parseError(file, "bad entry");
  }
  return LoadedIndex{std::move(idx), sequence};
}
}  // namespace

// ---- Journal ------------------------------------------------------------------------------------------------------

std::vector<uint8_t> encodeChange(const DirChange& c) {
  Writer w;
  w.str(c.placeRoot);
  w.str(c.relPath);
  w.pod(static_cast<uint32_t>(c.listing.subdirs.size()));
  for (const std::string& s : c.listing.subdirs) w.str(s);
  w.pod(static_cast<uint32_t>(c.listing.files.size()));
  for (const EntryRecord& r : c.listing.files) encodeRecord(w, r);
  return std::move(w.bytes);
}

bool decodeChange(const uint8_t* data, std::size_t size, DirChange& out) {
  Reader rd(data, size);
  rd.str(out.placeRoot);
  rd.str(out.relPath);
  uint32_t n = 0;
  rd.pod(n);
  if (!rd.ok() || n > size) return false;
  out.listing.subdirs.resize(n);
  for (std::string& s : out.listing.subdirs) rd.str(s);
  rd.pod(n);
  if (!rd.ok() || n > size) return false;
  out.listing.files.resize(n);
  for (EntryRecord& r : out.listing.files)
    if (!decodeRecord(rd, r)) return false;
  out.listing.probed = true;
  return rd.ok() && rd.pos() == size;
}

Journal::~Journal() { close(); }

void Journal::close() noexcept {
  if (handle_) CloseHandle(handle_);
  handle_ = nullptr;
}

Status Journal::reset(const std::filesystem::path& file, uint64_t sequence) {
  close();
  std::error_code ec;
  if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
  HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return Error{ErrorCode::Io, "cannot create journal " + toUtf8(file.native()), {}};
  uint8_t header[kJournalHeaderBytes] = {};
  std::memcpy(header, kJournalMagic, 8);
  const uint32_t version = kIndexFormatVersion;
  std::memcpy(header + 8, &version, 4);
  std::memcpy(header + 16, &sequence, 8);
  DWORD written = 0;
  if (!WriteFile(h, header, sizeof header, &written, nullptr) || written != sizeof header) {
    CloseHandle(h);
    return Error{ErrorCode::Io, "cannot write journal " + toUtf8(file.native()), {}};
  }
  handle_ = h;
  return Ok{};
}

Status Journal::openAppend(const std::filesystem::path& file, uint64_t validBytes) {
  close();
  HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return Error{ErrorCode::Io, "cannot open journal " + toUtf8(file.native()), {}};
  LARGE_INTEGER at{};
  at.QuadPart = static_cast<LONGLONG>(validBytes);
  if (!SetFilePointerEx(h, at, nullptr, FILE_BEGIN) || !SetEndOfFile(h)) {
    CloseHandle(h);
    return Error{ErrorCode::Io, "cannot truncate journal " + toUtf8(file.native()), {}};
  }
  handle_ = h;
  return Ok{};
}

Status Journal::append(const std::vector<DirChange>& changes) {
  if (!handle_) return Error{ErrorCode::Io, "journal not open", {}};
  Writer w;
  for (const DirChange& c : changes) {
    const std::vector<uint8_t> payload = encodeChange(c);
    if (payload.size() > kMaxRecordBytes) continue;  // a directory that large: left to the snapshot
    w.pod(kRecordMagic);
    w.pod(static_cast<uint32_t>(payload.size()));
    w.pod(hashWords(payload.data(), payload.size()));
    w.raw(payload.data(), payload.size());
  }
  DWORD written = 0;
  // One write per batch: a crash tears at most the batch's last record, which read() then drops.
  if (!WriteFile(static_cast<HANDLE>(handle_), w.bytes.data(), static_cast<DWORD>(w.bytes.size()), &written, nullptr) ||
      written != w.bytes.size())
    return Error{ErrorCode::Io, "cannot append to journal", {}};
  return Ok{};
}

Journal::Contents Journal::read(const std::filesystem::path& file, uint64_t sequence) {
  Contents c;
  std::vector<uint8_t> bytes;
  DWORD err = 0;
  if (!readWholeFile(file, bytes, &err) || bytes.size() < kJournalHeaderBytes) return c;
  uint64_t seq = 0;
  std::memcpy(&seq, bytes.data() + 16, 8);
  if (std::memcmp(bytes.data(), kJournalMagic, 8) != 0 || seq != sequence) return c;
  std::size_t at = kJournalHeaderBytes;
  c.validBytes = at;
  while (at + 16 <= bytes.size()) {
    uint32_t magic = 0, size = 0;
    uint64_t hash = 0;
    std::memcpy(&magic, bytes.data() + at, 4);
    std::memcpy(&size, bytes.data() + at + 4, 4);
    std::memcpy(&hash, bytes.data() + at + 8, 8);
    if (magic != kRecordMagic || size > bytes.size() - at - 16) break;
    const uint8_t* payload = bytes.data() + at + 16;
    if (hashWords(payload, size) != hash) break;
    DirChange ch;
    if (!decodeChange(payload, size, ch)) break;
    c.changes.push_back(std::move(ch));
    at += 16 + size;
    c.validBytes = at;
  }
  return c;
}

}  // namespace pq::library
