// The compile cache's key, executable identity, disk entries and eviction (cache.h, pq/jit/cache.h).

#include "cache.h"

#include <llvm/Config/llvm-config.h>
#include <llvm/Support/xxhash.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <system_error>
#include <type_traits>

#include "pq/jit/cache.h"

namespace pq::jit {

std::filesystem::path defaultCacheDir() {
#ifdef _WIN32
  wchar_t buf[MAX_PATH];
  const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return {};
  std::filesystem::path dir = std::filesystem::path(buf) / L"pqdaw" / L"jit-cache";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec && !std::filesystem::is_directory(dir, ec)) return {};
  return dir;
#else
  return {};
#endif
}

}  // namespace pq::jit

namespace pq::jit::detail {

namespace g = pq::graph;

// Entry format: bump kFormat whenever the payload layout or the meaning of a key field changes.
constexpr char kMagic[8] = {'P', 'Q', 'J', 'I', 'T', 'C', 'C', 'H'};
constexpr uint32_t kFormat = 2;  // 2: pair functions (T1b.2)
constexpr uint32_t kHeaderBytes = 64;
constexpr uint64_t kMaxPayload = uint64_t{256} << 20;
constexpr const wchar_t* kExt = L".pqjit";
constexpr const wchar_t* kTmpExt = L".tmp";

std::string Key::hex() const {
  static constexpr char d[] = "0123456789abcdef";
  std::string s(32, '0');
  for (int i = 0; i < 16; ++i) {
    s[static_cast<size_t>(15 - i)] = d[(hi >> (4 * i)) & 15];
    s[static_cast<size_t>(31 - i)] = d[(lo >> (4 * i)) & 15];
  }
  return s;
}

Key hashBytes(const void* data, size_t n) noexcept {
  const llvm::XXH128_hash_t h = llvm::xxh3_128bits(static_cast<const uint8_t*>(data), n);
  return {h.low64, h.high64};
}

namespace {

// A flat little-endian byte stream of the fields that are hashed (struct bytes would include padding).
class Stream {
public:
  template <class T>
  void put(T v) {
    static_assert(std::is_trivially_copyable_v<T>);
    if constexpr (std::is_enum_v<T>) {
      put(static_cast<std::underlying_type_t<T>>(v));
    } else {
      const size_t at = b_.size();
      b_.resize(at + sizeof(T));
      std::memcpy(b_.data() + at, &v, sizeof(T));
    }
  }
  void put(bool v) { put(static_cast<uint8_t>(v)); }
  void putBytes(const void* p, size_t n) {
    put(static_cast<uint64_t>(n));
    const size_t at = b_.size();
    b_.resize(at + n);
    if (n) std::memcpy(b_.data() + at, p, n);
  }
  void putStr(std::string_view s) { putBytes(s.data(), s.size()); }
  template <class T>
  void putU32s(const std::vector<T>& v) {
    put(static_cast<uint64_t>(v.size()));
    for (const T& x : v) put(static_cast<uint32_t>(x));
  }
  void reserve(size_t n) { b_.reserve(n); }
  const std::vector<uint8_t>& bytes() const noexcept { return b_; }

private:
  std::vector<uint8_t> b_;
};

void putOperand(Stream& h, const g::Operand& o) {
  h.put(o.kind);
  h.put(o.index);
}

}  // namespace

// A field added to any of these structs must be added to codegenDigest below (or the comment must say why code
// generation cannot read it): a key that misses a field codegen reads gives wrong cache hits. The sizes make the
// compiler stop here when a struct changes (MSVC x64 layout).
static_assert(sizeof(g::Stage) == 24, "graph::Stage changed: update codegenDigest");
static_assert(sizeof(g::Region) == 28, "graph::Region changed: update codegenDigest");
static_assert(sizeof(g::Op) == 36, "graph::Op changed: update codegenDigest");
static_assert(sizeof(g::Operand) == 8, "graph::Operand changed: update codegenDigest");
static_assert(sizeof(g::SolveBlock) == 36, "graph::SolveBlock changed: update codegenDigest");
static_assert(sizeof(g::SolveUnknown) == 28, "graph::SolveUnknown changed: update codegenDigest");
static_assert(sizeof(g::SolveEquation) == 24, "graph::SolveEquation changed: update codegenDigest");
static_assert(sizeof(g::Buffer) == 3, "graph::Buffer changed: update codegenDigest");
static_assert(sizeof(g::External) == 20, "graph::External changed: update codegenDigest");
static_assert(sizeof(g::ConstValue) == 8, "graph::ConstValue changed: update codegenDigest");
static_assert(sizeof(g::ParamSlot) == 40, "graph::ParamSlot changed: update codegenDigest");
static_assert(sizeof(g::OutputRef) == 24, "graph::OutputRef changed: update codegenDigest");
static_assert(sizeof(g::StateTag) == 8, "graph::StateTag changed: update codegenDigest");
static_assert(sizeof(dsp::StateInit) == 24, "dsp::StateInit changed: update codegenDigest");
static_assert(sizeof(g::StateEntry) == 64, "graph::StateEntry changed: update codegenDigest");
static_assert(sizeof(g::StateLayout) == 40, "graph::StateLayout changed: update codegenDigest");
static_assert(sizeof(g::Schedule) == 424 + sizeof(g::SleepPlan), "graph::Schedule changed: update codegenDigest");
static_assert(sizeof(g::ProbeRef) == 28, "graph::ProbeRef changed: update codegenDigest");
static_assert(sizeof(g::SleepPlan) == 152, "graph::SleepPlan changed: update codegenDigest");
static_assert(sizeof(g::RegionSleep) == 12, "graph::RegionSleep changed: update codegenDigest");
static_assert(sizeof(HostLayout) == 192, "HostLayout changed: update codegenDigest");

Key codegenDigest(const g::Schedule& s, const HostLayout& L, uint32_t hostSlots) {
  Stream h;
  h.reserve(64 + s.ops.size() * 40 + s.operands.size() * 5 + s.state.entries.size() * 40 + L.opInfo.size() * 4);
  h.put(s.stages.size());
  for (const g::Stage& x : s.stages) {
    h.put(x.poly), h.put(x.index), h.put(x.firstRegion), h.put(x.numRegions), h.put(x.firstOp), h.put(x.numOps);
  }
  h.put(s.regions.size());
  for (const g::Region& x : s.regions) {
    h.put(x.kind), h.put(x.poly), h.put(x.stage), h.put(x.firstOp), h.put(x.numOps), h.put(x.subBlock);
    h.put(x.firstSolve), h.put(x.numSolves);
  }
  h.put(s.ops.size());
  for (const g::Op& x : s.ops) {  // not `node` (an identity)
    h.put(x.kind), h.put(x.lanes), h.put(x.kernel), h.put(x.atom), h.put(x.numIn), h.put(x.numOut), h.put(x.channel);
    h.put(x.splMask), h.put(x.firstOperand), h.put(x.state), h.put(x.resource), h.put(x.valueKind);
  }
  h.put(s.operands.size());
  for (const g::Operand& x : s.operands) putOperand(h, x);
  h.put(s.solves.size());
  for (const g::SolveBlock& x : s.solves) {  // not `solve` (an identity)
    h.put(x.iterations), h.put(x.region), h.put(x.firstOp), h.put(x.numOps), h.put(x.firstUnknown);
    h.put(x.numUnknowns), h.put(x.firstEquation), h.put(x.numEquations);
  }
  h.put(s.unknowns.size());
  for (const g::SolveUnknown& x : s.unknowns) {
    h.put(x.channel);
    putOperand(h, x.value);
    putOperand(h, x.init);
    h.put(x.state);
  }
  h.put(s.equations.size());
  for (const g::SolveEquation& x : s.equations) {
    h.put(x.channel);
    putOperand(h, x.lhs);
    putOperand(h, x.rhs);
  }
  h.put(s.buffers.size());
  for (const g::Buffer& x : s.buffers) h.put(x.cls), h.put(x.lanes), h.put(x.perGroup);
  h.put(s.externals.size());
  for (const g::External& x : s.externals) {
    h.put(x.source), h.put(x.index), h.put(x.atom), h.put(x.channel), h.put(x.rate), h.put(x.poly);
  }
  h.put(s.constants.size());
  for (const g::ConstValue& x : s.constants) h.put(x.value), h.put(x.kind);  // the bits (−0, NaN payloads)
  h.put(s.params.size());
  for (const g::ParamSlot& x : s.params) {  // not `key` / `node`
    h.put(x.kind), h.put(x.defaultValue), h.put(x.hasRange), h.put(x.rangeMin), h.put(x.rangeMax);
  }
  h.put(s.outputs.size());
  for (const g::OutputRef& x : s.outputs) {
    h.put(x.isExport), h.put(x.channel);
    putOperand(h, x.value);
    h.put(x.poly), h.put(x.stage);
  }
  // T4.5: probed values are stored by the generated code (store elimination treats them like outputs).
  h.put(s.probes.size());
  for (const g::ProbeRef& x : s.probes) {
    putOperand(h, x.value);
    h.put(x.poly), h.put(x.stage);
  }
  // State: the layout codegen addresses, and what the host initialises it from, except the identities (the state key
  // and the noise seed, which is mixed with it): the host builds those from the image's own schedule.
  h.put(s.state.entries.size());
  for (const g::StateEntry& x : s.state.entries) {
    h.put(x.channel), h.put(x.arena), h.put(x.tag.kernel), h.put(x.tag.lanes), h.put(x.tag.extra), h.put(x.offset);
    h.put(x.size), h.put(x.init.sampleRate), h.put(x.init.lanes), h.put(x.init.maxDelayFrames), h.put(x.init.z1Init);
  }
  h.put(s.state.globalBytes), h.put(s.state.voiceBytes), h.put(s.sampleRate);
  // The sleep plan (T1.x-silence): which regions are guarded by which segment's flag, and what they fill. Sources,
  // states and params are the host's (not code).
  h.put(s.sleep.regions.size());
  for (const g::RegionSleep& x : s.sleep.regions) h.put(x.segment), h.put(x.firstFill), h.put(x.numFills);
  h.putU32s(s.sleep.fills);
  h.put(L.sleepOff);
  h.put(L.meterOff), h.put(L.meterFlagOff);  // T4.5
  // Schedule::warnings, insertedDelays and rewritten are diagnostics and names: not read by code generation.
  h.put(L.maxBlock), h.put(L.groups);
  h.putU32s(L.bufOff);
  h.putU32s(L.bufSize);
  h.put(L.solveOff), h.put(L.maxSolveN), h.put(L.callOff), h.put(L.callPtrs), h.put(L.paramOff), h.put(L.constOff);
  h.putU32s(L.opInfo);
  h.putU32s(L.pairable);
  h.putU32s(L.altOff);
  h.putU32s(L.opInfoB);
  h.put(hostSlots);
  return hashBytes(h.bytes().data(), h.bytes().size());
}

Key cacheKey(const Key& digest, const KeyOptions& o, const Key& binary) {
  Stream h;
  h.putBytes(kMagic, sizeof kMagic);
  h.put(kFormat);
  h.putStr(LLVM_VERSION_STRING);
  h.put(binary.lo), h.put(binary.hi);
  h.putStr(o.cpu);
  h.putStr(o.features);
  h.put(o.codeGenLevel), h.put(o.optLevel), h.put(o.contract), h.put(o.voiceCode);
  h.put(digest.lo), h.put(digest.hi);
  return hashBytes(h.bytes().data(), h.bytes().size());
}

// ---- The executable's identity ---------------------------------------------------------------------------------

#ifdef _WIN32

namespace {

std::wstring moduleFileName(HMODULE m) {
  std::wstring name(MAX_PATH, L'\0');
  for (;;) {
    const DWORD n = GetModuleFileNameW(m, name.data(), static_cast<DWORD>(name.size()));
    if (n == 0) return {};
    if (n < name.size()) {
      name.resize(n);
      return name;
    }
    name.resize(name.size() * 2);
  }
}

BinaryIdentity computeIdentity() {
  const auto t0 = std::chrono::steady_clock::now();
  BinaryIdentity id;
  HMODULE m = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&binaryIdentity), &m))
    return id;
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(m);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) return id;
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(m) + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return id;
  const std::wstring path = moduleFileName(m);
  if (path.empty()) return id;
  // The file as linked (the mapped image has relocations applied, which differ per run under ASLR).
  HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (f == INVALID_HANDLE_VALUE) return id;
  LARGE_INTEGER size{};
  bool ok = GetFileSizeEx(f, &size) && size.QuadPart > 0;
  if (ok) {
    HANDLE map = CreateFileMappingW(f, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (map != nullptr) {
      const void* view = MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0);
      if (view != nullptr) {
        id.hash = hashBytes(view, static_cast<size_t>(size.QuadPart));
        UnmapViewOfFile(view);
      } else {
        ok = false;
      }
      CloseHandle(map);
    } else {
      ok = false;
    }
  }
  CloseHandle(f);
  if (!ok) return id;
  id.base = reinterpret_cast<uintptr_t>(m);
  id.size = nt->OptionalHeader.SizeOfImage;
  id.ok = true;
  id.hashMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  return id;
}

}  // namespace

const BinaryIdentity& binaryIdentity() {
  static const BinaryIdentity id = computeIdentity();
  return id;
}

// ---- Disk entries ----------------------------------------------------------------------------------------------

namespace {

void putHeader(uint8_t* h, const Key& key, uint64_t payloadBytes, const Key& payloadHash) {
  std::memset(h, 0, kHeaderBytes);
  std::memcpy(h, kMagic, 8);
  std::memcpy(h + 8, &kFormat, 4);
  std::memcpy(h + 12, &kHeaderBytes, 4);
  std::memcpy(h + 16, &key.lo, 8);
  std::memcpy(h + 24, &key.hi, 8);
  std::memcpy(h + 32, &payloadBytes, 8);
  std::memcpy(h + 40, &payloadHash.lo, 8);
  std::memcpy(h + 48, &payloadHash.hi, 8);
}

// Bounds-checked reader of a payload.
class Reader {
public:
  Reader(const uint8_t* p, size_t n) : p_(p), n_(n) {}
  template <class T>
  bool get(T& v) {
    static_assert(std::is_trivially_copyable_v<T>);
    if (n_ - at_ < sizeof(T)) return false;
    std::memcpy(&v, p_ + at_, sizeof(T));
    at_ += sizeof(T);
    return true;
  }
  bool bytes(size_t n, const uint8_t*& out) {
    if (n_ - at_ < n) return false;
    out = p_ + at_;
    at_ += n;
    return true;
  }
  bool done() const noexcept { return at_ == n_; }

private:
  const uint8_t* p_;
  size_t n_, at_ = 0;
};

bool parsePayload(const uint8_t* p, size_t n, Entry& e) {
  Reader r(p, n);
  uint32_t stages = 0;
  if (!r.get(stages) || stages > (1u << 20)) return false;
  e.stageFunctions.assign(stages, {});
  for (std::string& name : e.stageFunctions) {
    uint32_t len = 0;
    const uint8_t* b = nullptr;
    if (!r.get(len) || len == 0 || len > 4096 || !r.bytes(len, b)) return false;
    name.assign(reinterpret_cast<const char*>(b), len);
  }
  e.pairFunctions.assign(stages, {});
  for (std::string& name : e.pairFunctions) {  // empty: no pair function for that stage
    uint32_t len = 0;
    const uint8_t* b = nullptr;
    if (!r.get(len) || len > 4096 || !r.bytes(len, b)) return false;
    name.assign(reinterpret_cast<const char*>(b), len);
  }
  uint32_t fns = 0;
  if (!r.get(e.hostSlots) || !r.get(fns) || fns > (1u << 20)) return false;
  e.fnOffsets.assign(fns, 0);
  for (uint64_t& off : e.fnOffsets)
    if (!r.get(off)) return false;
  uint8_t probes = 0;
  if (!r.get(e.irHash.lo) || !r.get(e.irHash.hi) || !r.get(e.objectBytes) || !r.get(e.irInstructions) ||
      !r.get(e.inlinedOps) || !r.get(e.calledOps) || !r.get(probes) || probes > 1)
    return false;
  e.stackProbes = probes != 0;
  uint64_t objLen = 0;
  const uint8_t* obj = nullptr;
  // A schedule without stages has no code (and no object).
  if (!r.get(objLen) || (objLen == 0) != (stages == 0) || objLen > kMaxPayload || !r.bytes(static_cast<size_t>(objLen), obj))
    return false;
  e.object.assign(obj, obj + objLen);
  return r.done();
}

std::vector<uint8_t> payloadOf(const Entry& e) {
  Stream s;
  s.reserve(e.object.size() + 256 + e.fnOffsets.size() * 8);
  s.put(static_cast<uint32_t>(e.stageFunctions.size()));
  for (const std::string& name : e.stageFunctions) {
    s.put(static_cast<uint32_t>(name.size()));
    for (char c : name) s.put(c);
  }
  for (size_t k = 0; k < e.stageFunctions.size(); ++k) {
    const std::string none;
    const std::string& name = k < e.pairFunctions.size() ? e.pairFunctions[k] : none;
    s.put(static_cast<uint32_t>(name.size()));
    for (char c : name) s.put(c);
  }
  s.put(e.hostSlots);
  s.put(static_cast<uint32_t>(e.fnOffsets.size()));
  for (uint64_t off : e.fnOffsets) s.put(off);
  s.put(e.irHash.lo), s.put(e.irHash.hi), s.put(e.objectBytes), s.put(e.irInstructions), s.put(e.inlinedOps);
  s.put(e.calledOps), s.put(static_cast<uint8_t>(e.stackProbes));
  s.put(static_cast<uint64_t>(e.object.size()));
  std::vector<uint8_t> out = s.bytes();
  out.insert(out.end(), e.object.begin(), e.object.end());
  return out;
}

bool readAll(HANDLE f, uint8_t* p, size_t n) {
  while (n > 0) {
    DWORD got = 0;
    const DWORD want = static_cast<DWORD>(std::min<size_t>(n, size_t{1} << 30));
    if (!ReadFile(f, p, want, &got, nullptr) || got == 0) return false;
    p += got;
    n -= got;
  }
  return true;
}

bool writeAll(HANDLE f, const uint8_t* p, size_t n) {
  while (n > 0) {
    DWORD put = 0;
    const DWORD want = static_cast<DWORD>(std::min<size_t>(n, size_t{1} << 30));
    if (!WriteFile(f, p, want, &put, nullptr) || put == 0) return false;
    p += put;
    n -= put;
  }
  return true;
}

uint64_t fileTimeOf(const FILETIME& t) noexcept { return (uint64_t{t.dwHighDateTime} << 32) | t.dwLowDateTime; }

}  // namespace

std::filesystem::path entryPath(const std::filesystem::path& dir, const Key& key) {
  std::filesystem::path p = dir / key.hex();
  p += kExt;
  return p;
}

ReadResult readEntry(const std::filesystem::path& dir, const Key& key, bool writable, Entry& out) {
  const std::filesystem::path path = entryPath(dir, key);
  HANDLE f = CreateFileW(path.c_str(), GENERIC_READ | (writable ? FILE_WRITE_ATTRIBUTES : 0),
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (f == INVALID_HANDLE_VALUE) return ReadResult::Miss;
  bool valid = false;
  LARGE_INTEGER size{};
  if (GetFileSizeEx(f, &size) && size.QuadPart >= kHeaderBytes &&
      static_cast<uint64_t>(size.QuadPart) <= kHeaderBytes + kMaxPayload) {
    std::vector<uint8_t> buf(static_cast<size_t>(size.QuadPart));
    if (readAll(f, buf.data(), buf.size())) {
      const uint8_t* h = buf.data();
      uint32_t format = 0, headerBytes = 0;
      Key k, ph;
      uint64_t payload = 0;
      std::memcpy(&format, h + 8, 4);
      std::memcpy(&headerBytes, h + 12, 4);
      std::memcpy(&k.lo, h + 16, 8);
      std::memcpy(&k.hi, h + 24, 8);
      std::memcpy(&payload, h + 32, 8);
      std::memcpy(&ph.lo, h + 40, 8);
      std::memcpy(&ph.hi, h + 48, 8);
      valid = std::memcmp(h, kMagic, 8) == 0 && format == kFormat && headerBytes == kHeaderBytes && k == key &&
              payload == buf.size() - kHeaderBytes && hashBytes(h + kHeaderBytes, payload) == ph &&
              parsePayload(h + kHeaderBytes, static_cast<size_t>(payload), out);
    }
  }
  if (valid && writable) {  // least recently used: a hit counts as a use
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    SetFileTime(f, nullptr, nullptr, &now);
  }
  CloseHandle(f);
  if (valid) return ReadResult::Hit;
  out = Entry{};
  if (writable) DeleteFileW(path.c_str());  // a reader still holding it keeps reading (FILE_SHARE_DELETE)
  return ReadResult::Rejected;
}

uint64_t writeEntry(const std::filesystem::path& dir, const Key& key, const Entry& e) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  const std::vector<uint8_t> payload = payloadOf(e);
  if (payload.size() > kMaxPayload) return 0;
  uint8_t header[kHeaderBytes];
  putHeader(header, key, payload.size(), hashBytes(payload.data(), payload.size()));
  // A name no other writer uses (process, thread, counter), in the same directory so the rename is atomic.
  static std::atomic<uint64_t> counter{0};
  std::filesystem::path tmp = dir / (key.hex() + "." + std::to_string(GetCurrentProcessId()) + "." +
                                     std::to_string(GetCurrentThreadId()) + "." + std::to_string(counter++));
  tmp += kTmpExt;
  HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return 0;
  const bool wrote = writeAll(f, header, kHeaderBytes) && writeAll(f, payload.data(), payload.size());
  CloseHandle(f);
  const std::filesystem::path target = entryPath(dir, key);
  if (!wrote || !MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    DeleteFileW(tmp.c_str());
    return 0;
  }
  return kHeaderBytes + payload.size();
}

namespace {

struct DirFile {
  std::filesystem::path path;
  uint64_t bytes = 0, written = 0;
  bool tmp = false;
};

std::vector<DirFile> listDir(const std::filesystem::path& dir) {
  std::vector<DirFile> files;
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileExW((dir / L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                              FIND_FIRST_EX_LARGE_FETCH);
  if (h == INVALID_HANDLE_VALUE) return files;
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    const std::wstring name = fd.cFileName;
    const auto ends = [&](const wchar_t* ext) {
      const size_t n = std::wcslen(ext);
      return name.size() > n && name.compare(name.size() - n, n, ext) == 0;
    };
    const bool tmp = ends(kTmpExt);
    if (!tmp && !ends(kExt)) continue;  // not ours
    files.push_back({dir / name, (uint64_t{fd.nFileSizeHigh} << 32) | fd.nFileSizeLow, fileTimeOf(fd.ftLastWriteTime), tmp});
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return files;
}

}  // namespace

uint64_t directoryBytes(const std::filesystem::path& dir) {
  uint64_t total = 0;
  for (const DirFile& f : listDir(dir))
    if (!f.tmp) total += f.bytes;
  return total;
}

EvictResult evict(const std::filesystem::path& dir, uint64_t maxBytes) {
  EvictResult r;
  std::vector<DirFile> files = listDir(dir);
  FILETIME nowFt;
  GetSystemTimeAsFileTime(&nowFt);
  const uint64_t now = fileTimeOf(nowFt);
  constexpr uint64_t kHour = uint64_t{36000000000};  // 100 ns units
  std::vector<DirFile> entries;
  for (DirFile& f : files) {
    if (f.tmp) {
      if (now > f.written + kHour) DeleteFileW(f.path.c_str());  // abandoned by a crashed writer
      continue;
    }
    r.bytes += f.bytes;
    entries.push_back(std::move(f));
  }
  if (r.bytes <= maxBytes) return r;
  std::sort(entries.begin(), entries.end(), [](const DirFile& a, const DirFile& b) { return a.written < b.written; });
  const uint64_t target = maxBytes / 4 * 3;
  for (const DirFile& f : entries) {
    if (r.bytes <= target) break;
    if (DeleteFileW(f.path.c_str())) {
      r.bytes -= f.bytes;
      ++r.evictedFiles;
      r.evictedBytes += f.bytes;
    }
  }
  return r;
}

#else  // no disk cache off Windows (pqdaw is Windows-only for now): every lookup misses, nothing is stored

const BinaryIdentity& binaryIdentity() {
  static const BinaryIdentity id;
  return id;
}
std::filesystem::path entryPath(const std::filesystem::path& dir, const Key& key) { return dir / (key.hex() + ".pqjit"); }
ReadResult readEntry(const std::filesystem::path&, const Key&, bool, Entry&) { return ReadResult::Miss; }
uint64_t writeEntry(const std::filesystem::path&, const Key&, const Entry&) { return 0; }
EvictResult evict(const std::filesystem::path&, uint64_t) { return {}; }
uint64_t directoryBytes(const std::filesystem::path&) { return 0; }

#endif

}  // namespace pq::jit::detail
