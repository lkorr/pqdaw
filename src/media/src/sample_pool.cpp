#include "pq/media/sample_pool.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <system_error>

#include "file_io.h"
#include "pq/media/decoder.h"
#include "pq/media/hash.h"
#include "pq/media/pcm.h"
#include "pq/media/probe.h"

namespace pq::media {

// ---- StreamPool (non-inline parts) --------------------------------------------------------------------------------

namespace detail {
const float* zeroFrames() noexcept {
  alignas(64) static const float zeros[kMaxSpanFrames + 2 * kSampleGuardFrames] = {};
  return zeros;
}
}  // namespace detail

StreamPool::StreamPool(uint32_t slots, uint32_t ringFloats) : slotCount_(slots), ringFloats_(ringFloats) {
  slots_ = new Slot[slots];
  const std::size_t bytes = std::size_t{slots} * ringFloats * sizeof(float);
  if (bytes) {
    ringBase_ = static_cast<float*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE, PAGE_READWRITE));
    if (!ringBase_) throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "StreamPool: reserve");
  }
  for (uint32_t i = slots; i-- > 0;) pushFree(i);
}

StreamPool::~StreamPool() {
  if (ringBase_) VirtualFree(ringBase_, 0, MEM_RELEASE);
  delete[] slots_;
}

uint32_t StreamPool::freeSlots() const noexcept {
  uint32_t n = 0;
  for (uint32_t i = static_cast<uint32_t>(freeHead_.load(std::memory_order_acquire)); i != kNone && n <= slotCount_;
       i = slots_[i].nextFree.load(std::memory_order_relaxed))
    ++n;
  return n;
}

bool StreamPool::addPrefetch(uint32_t source, int64_t frame, Direction dir, uint32_t channels, int64_t frames) noexcept {
  for (Prefetch& p : prefetch_) {
    uint32_t expected = 0;
    if (!p.state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) continue;
    const StreamId id = claimSlot(source, frame, dir, channels, frames);
    if (!id.valid()) {
      p.state.store(0, std::memory_order_release);
      return false;
    }
    p.source = source;
    p.frame = frame;
    p.dir = dir;
    p.id = id;
    prefetchCount_.fetch_add(1, std::memory_order_acq_rel);
    p.state.store(2, std::memory_order_release);
    return true;
  }
  return false;
}

void StreamPool::clearPrefetches() noexcept {
  for (Prefetch& p : prefetch_) {
    uint32_t expected = 2;
    if (!p.state.compare_exchange_strong(expected, 3, std::memory_order_acq_rel)) continue;
    release(p.id);
    p.state.store(0, std::memory_order_release);
    prefetchCount_.fetch_sub(1, std::memory_order_acq_rel);
  }
}

// ---- pool internals -------------------------------------------------------------------------------------------------

// Where a streamed sample's frames are read from: the original PCM file, or its decoded .f32 cache.
struct SamplePool::Source {
  detail::File file;  // overlapped
  PcmEncoding pcm = PcmEncoding::None;
  bool bigEndian = false;
  uint64_t dataOffset = 0;
  uint32_t blockAlign = 0;
  uint32_t channels = 0;
  int64_t frames = 0;
};

namespace {

constexpr uint32_t kStagingBytes = 256u << 10;
constexpr uint32_t kMaxInflight = 32;
constexpr uint32_t kCacheHeaderBytes = 4096;
constexpr char kCacheMagic[8] = {'P', 'Q', 'D', 'E', 'C', 'F', '3', '2'};

struct CacheHeader {
  char magic[8];
  uint32_t version;
  uint32_t channels;
  uint32_t sampleRate;
  uint32_t reserved;
  uint64_t frames;
  uint64_t sourceSize;
  uint64_t sourceHash;
};

std::string hex(uint64_t v) {
  static const char* d = "0123456789abcdef";
  std::string s(16, '0');
  for (int i = 15; i >= 0; --i, v >>= 4) s[static_cast<std::size_t>(i)] = d[v & 15];
  return s;
}

std::string identityKey(const std::filesystem::path& path, const FileIdentity& id) {
  std::string k;
  if (id.hasFileId()) {
    k = hex(id.volumeSerial);
    for (uint8_t b : id.fileId) k += hex(b).substr(14);
  } else {
    k = std::filesystem::absolute(path).string();
  }
  return k + ':' + hex(id.size) + ':' + hex(static_cast<uint64_t>(id.mtime));
}

std::filesystem::path defaultCacheDir() {
  wchar_t buf[MAX_PATH];
  const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
  std::filesystem::path base = (n > 0 && n < MAX_PATH) ? std::filesystem::path(buf) : std::filesystem::temp_directory_path();
  return base / "pqdaw" / "decoded";
}

// Planar storage with guard frames for `frames` frames per channel.
std::unique_ptr<Residency> allocResidency(const SampleInfo& info, int64_t residentFrames) {
  auto r = std::make_unique<Residency>();
  const std::size_t stride = static_cast<std::size_t>(residentFrames) + 2 * kSampleGuardFrames;
  r->storage = AlignedBuffer<float>(stride * info.channels);
  r->view.frames = static_cast<int64_t>(info.frames);
  r->view.residentFrames = residentFrames;
  r->view.channels = info.channels;
  r->view.sampleRate = info.sampleRate;
  for (uint32_t c = 0; c < info.channels; ++c) r->view.data[c] = r->storage.data() + c * stride + kSampleGuardFrames;
  return r;
}

uint64_t residencyBytes(const Residency& r) { return r.storage.size() * sizeof(float); }

// Decodes up to `frames` frames from the decoder's position into r's channels at `at`.
Result<int64_t> decodeInto(Decoder& d, Residency& r, int64_t at, int64_t frames) {
  float* ptrs[kMaxStreamChannels];
  int64_t done = 0;
  while (done < frames) {
    for (uint32_t c = 0; c < r.view.channels; ++c) ptrs[c] = const_cast<float*>(r.view.data[c]) + at + done;
    auto got = d.read(ptrs, static_cast<uint64_t>(std::min<int64_t>(frames - done, 1 << 16)));
    if (!got) return std::move(got).error();
    if (*got == 0) break;
    done += static_cast<int64_t>(*got);
  }
  return done;
}

Status writeAll(HANDLE h, const void* p, std::size_t n, const std::filesystem::path& path) {
  const auto* b = static_cast<const uint8_t*>(p);
  while (n > 0) {
    DWORD wrote = 0;
    const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(n, 1u << 30));
    if (!WriteFile(h, b, chunk, &wrote, nullptr) || wrote == 0) return detail::ioError(path, "cannot write");
    b += wrote;
    n -= wrote;
  }
  return Ok{};
}

// Deletes the least recently written .f32 files until the directory fits `limit` (keeping `keep`).
void trimCache(const std::filesystem::path& dir, uint64_t limit, const std::filesystem::path& keep) {
  if (limit == 0) return;
  struct F {
    std::filesystem::path p;
    uint64_t size;
    std::filesystem::file_time_type t;
  };
  std::vector<F> files;
  uint64_t total = 0;
  std::error_code ec;
  for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
    if (e.path().extension() != ".f32") continue;
    F f{e.path(), e.file_size(ec), e.last_write_time(ec)};
    total += f.size;
    files.push_back(std::move(f));
  }
  std::sort(files.begin(), files.end(), [](const F& a, const F& b) { return a.t < b.t; });
  for (const F& f : files) {
    if (total <= limit) break;
    if (f.p == keep) continue;
    if (std::filesystem::remove(f.p, ec)) total -= f.size;
  }
}

}  // namespace

struct SamplePool::DiskState {
  struct Local {
    uint32_t gen = 0;
    bool active = false;
    bool committed = false;
    int64_t frontier = 0;
    std::shared_ptr<Source> src;
  };
  std::vector<Local> local;
  std::vector<AlignedBuffer<uint8_t>> staging;
  HANDLE events[kMaxInflight] = {};
  HANDLE timer = nullptr;
};

// ---- PooledSample -----------------------------------------------------------------------------------------------------

PooledSample::~PooledSample() {
  if (pool_) pool_->removeEntry(this);
}

// ---- SamplePool -------------------------------------------------------------------------------------------------------

SamplePool::SamplePool(PoolConfig config) : config_(std::move(config)), streams_(config_.streamSlots, config_.ringFloats) {
  if (config_.decodedCacheDir.empty()) config_.decodedCacheDir = defaultCacheDir();
  wakeEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  filledEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

SamplePool::~SamplePool() {
  stopDiskThread();
  if (wakeEvent_) CloseHandle(wakeEvent_);
  if (filledEvent_) CloseHandle(filledEvent_);
}

void SamplePool::removeEntry(PooledSample* s) noexcept {
  std::shared_ptr<Source> dropped;
  {
    std::lock_guard lock(mutex_);
    auto it = entries_.find(s->key_);
    if (it != entries_.end() && it->second.expired()) entries_.erase(it);
    if (s->streamSource_) {
      headBytes_ -= s->bytes_;
      --streamedSamples_;
    } else {
      residentBytes_ -= s->bytes_;
    }
  }
  if (s->streamSource_) {
    std::lock_guard lock(sourcesMutex_);
    auto it = sources_.find(s->streamSource_);
    if (it != sources_.end()) {
      dropped = std::move(it->second);  // the disk thread may still hold it for an in-flight read
      sources_.erase(it);
    }
  }
}

std::shared_ptr<SamplePool::Source> SamplePool::source(uint32_t id) {
  std::lock_guard lock(sourcesMutex_);
  auto it = sources_.find(id);
  return it == sources_.end() ? nullptr : it->second;
}

Result<SampleRef> SamplePool::load(const std::filesystem::path& path) {
  auto decoder = Decoder::open(path);
  if (!decoder) return std::move(decoder).error();
  Decoder& d = **decoder;
  const SampleInfo& info = d.info();
  if (info.channels > kMaxStreamChannels)
    return Error{ErrorCode::Unsupported, path.string() + ": more than 8 channels", {}};
  if (info.frames >= (uint64_t{1} << StreamPool::kFrameBits))
    return Error{ErrorCode::Unsupported, path.string() + ": too long", {}};
  const std::string key = identityKey(path, info.identity);
  {
    std::lock_guard lock(mutex_);
    auto it = entries_.find(key);
    if (it != entries_.end())
      if (SampleRef existing = it->second.lock()) {
        existing->lastUse_ = ++tick_;
        return existing;
      }
  }
  std::unique_ptr<PooledSample> s(new PooledSample);
  s->path_ = path;
  s->key_ = key;
  s->info_ = info;
  if (auto h = contentHash(path)) s->contentHash_ = *h;

  const uint64_t guarded = (info.frames + 2 * kSampleGuardFrames) * info.channels * sizeof(float);
  if (info.decodedBytes() <= config_.residentLimitBytes) {
    enforceBudget(guarded, nullptr);
    auto r = allocResidency(info, static_cast<int64_t>(info.frames));
    auto got = decodeInto(d, *r, 0, static_cast<int64_t>(info.frames));
    if (!got) return std::move(got).error();
    r->view.frames = r->view.residentFrames = *got;  // a truncated file plays as far as it decodes
    s->bytes_ = residencyBytes(*r);
    s->owned_ = std::move(r);
  } else {
    if (auto st = makeStreamed(*s, &d, nullptr); !st) return std::move(st).error();
  }
  s->residency_.store(s->owned_.get(), std::memory_order_release);

  std::lock_guard lock(mutex_);
  auto it = entries_.find(key);
  if (it != entries_.end())
    if (SampleRef existing = it->second.lock()) {  // another thread loaded the same file meanwhile
      existing->lastUse_ = ++tick_;
      s->pool_ = nullptr;  // never counted: drop it without touching the books
      if (s->streamSource_) {
        std::lock_guard sl(sourcesMutex_);
        sources_.erase(s->streamSource_);
      }
      return existing;
    }
  s->pool_ = this;
  s->lastUse_ = ++tick_;
  if (s->streamSource_) {
    headBytes_ += s->bytes_;
    ++streamedSamples_;
  } else {
    residentBytes_ += s->bytes_;
  }
  SampleRef ref(s.release());
  entries_[key] = ref;
  return ref;
}

void SamplePool::touch(const SampleRef& s) {
  std::lock_guard lock(mutex_);
  s->lastUse_ = ++tick_;
}

// Head + stream source for `s`. `d` decodes (at its start) when given; else `from` is the sample's current resident
// data (a demotion: no decoding, the cache file is written from memory).
Status SamplePool::makeStreamed(PooledSample& s, Decoder* d, const Residency* from) {
  const SampleInfo& info = s.info_;
  const int64_t frames = from ? from->view.frames : static_cast<int64_t>(info.frames);
  const int64_t head = std::min<int64_t>(config_.headFrames, frames);
  auto r = allocResidency(info, head);
  r->view.frames = frames;
  auto src = std::make_shared<Source>();
  src->channels = info.channels;
  src->frames = frames;
  std::filesystem::path streamPath = s.path_;
  if (info.pcm != PcmEncoding::None) {
    // Uncompressed: stream the file itself; the head comes from the decoder (or the resident copy).
    src->pcm = info.pcm;
    src->bigEndian = info.bigEndian;
    src->dataOffset = info.dataOffset;
    src->blockAlign = info.blockAlign;
    if (from) {
      for (uint32_t c = 0; c < info.channels; ++c)
        std::memcpy(const_cast<float*>(r->view.data[c]), from->view.data[c], static_cast<std::size_t>(head) * sizeof(float));
    } else {
      auto got = decodeInto(*d, *r, 0, head);
      if (!got) return std::move(got).error();
    }
  } else {
    // Compressed: decode once into the cache (or reuse it), filling the head on the way.
    std::error_code ec;
    std::filesystem::create_directories(config_.decodedCacheDir, ec);
    const std::filesystem::path cache =
        config_.decodedCacheDir / (hex(s.contentHash_) + "-" + hex(info.identity.size) + ".f32");
    bool reuse = false;
    if (auto f = detail::File::open(cache, detail::File::Access::Random)) {
      CacheHeader h{};
      if (f->readExact(0, &h, sizeof h) && std::memcmp(h.magic, kCacheMagic, 8) == 0 && h.version == 1 &&
          h.channels == info.channels && h.sampleRate == info.sampleRate && h.sourceSize == info.identity.size &&
          h.sourceHash == s.contentHash_ && f->size() >= kCacheHeaderBytes + h.frames * info.channels * 4) {
        reuse = true;
        src->frames = static_cast<int64_t>(h.frames);
        r->view.frames = src->frames;
      }
    }
    if (reuse) {
      std::filesystem::last_write_time(cache, std::filesystem::file_time_type::clock::now(), ec);  // LRU
      // The head from the cache file (plain reads).
      auto f = detail::File::open(cache, detail::File::Access::Sequential);
      if (!f) return std::move(f).error();
      std::vector<uint8_t> raw(static_cast<std::size_t>(head) * info.channels * 4);
      if (auto st = f->readExact(kCacheHeaderBytes, raw.data(), raw.size()); !st) return st;
      float* ptrs[kMaxStreamChannels];
      for (uint32_t c = 0; c < info.channels; ++c) ptrs[c] = const_cast<float*>(r->view.data[c]);
      pcmToPlanar(PcmEncoding::F32, false, raw.data(), info.channels, static_cast<std::size_t>(head), ptrs);
    } else {
      const std::filesystem::path tmp =
          cache.string() + "." + std::to_string(GetCurrentProcessId()) + "." + std::to_string(GetCurrentThreadId()) + ".tmp";
      HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
      if (h == INVALID_HANDLE_VALUE) return detail::ioError(tmp, "cannot create");
      std::vector<uint8_t> header(kCacheHeaderBytes, 0);
      Status st = writeAll(h, header.data(), header.size(), tmp);
      constexpr int64_t kChunk = 1 << 15;
      std::vector<float> planar(static_cast<std::size_t>(kChunk) * info.channels);
      std::vector<float> inter(planar.size());
      int64_t written = 0;
      while (st && written < frames) {
        const int64_t want = std::min(kChunk, frames - written);
        int64_t got = 0;
        if (from) {
          got = want;
          for (uint32_t c = 0; c < info.channels; ++c)
            std::memcpy(planar.data() + c * kChunk, from->view.data[c] + written, static_cast<std::size_t>(want) * 4);
        } else {
          float* ptrs[kMaxStreamChannels];
          for (uint32_t c = 0; c < info.channels; ++c) ptrs[c] = planar.data() + c * kChunk;
          auto r2 = d->read(ptrs, static_cast<uint64_t>(want));
          if (!r2) {
            st = std::move(r2).error();
            break;
          }
          got = static_cast<int64_t>(*r2);
          if (got == 0) break;
        }
        // Head frames go straight into the resident head.
        for (int64_t f = written; f < std::min(written + got, head); ++f)
          for (uint32_t c = 0; c < info.channels; ++c)
            const_cast<float*>(r->view.data[c])[f] = planar[c * kChunk + static_cast<std::size_t>(f - written)];
        for (int64_t f = 0; f < got; ++f)
          for (uint32_t c = 0; c < info.channels; ++c)
            inter[static_cast<std::size_t>(f) * info.channels + c] = planar[c * kChunk + static_cast<std::size_t>(f)];
        st = writeAll(h, inter.data(), static_cast<std::size_t>(got) * info.channels * 4, tmp);
        written += got;
      }
      if (st) {
        CacheHeader ch{};
        std::memcpy(ch.magic, kCacheMagic, 8);
        ch.version = 1;
        ch.channels = info.channels;
        ch.sampleRate = info.sampleRate;
        ch.frames = static_cast<uint64_t>(written);
        ch.sourceSize = info.identity.size;
        ch.sourceHash = s.contentHash_;
        LARGE_INTEGER zero{};
        if (!SetFilePointerEx(h, zero, nullptr, FILE_BEGIN)) st = detail::ioError(tmp, "cannot seek");
        if (st) st = writeAll(h, &ch, sizeof ch, tmp);
      }
      CloseHandle(h);
      if (st && !MoveFileExW(tmp.c_str(), cache.c_str(), MOVEFILE_REPLACE_EXISTING)) st = detail::ioError(cache, "cannot rename");
      if (!st) {
        std::filesystem::remove(tmp, ec);
        return st;
      }
      src->frames = written;
      r->view.frames = written;
      r->view.residentFrames = std::min<int64_t>(head, written);
      trimCache(config_.decodedCacheDir, config_.decodedCacheLimitBytes, cache);
    }
    src->pcm = PcmEncoding::F32;
    src->dataOffset = kCacheHeaderBytes;
    src->blockAlign = info.channels * 4;
    streamPath = cache;
  }
  auto file = detail::File::open(streamPath, detail::File::Access::Overlapped);
  if (!file) return std::move(file).error();
  src->file = std::move(*file);
  uint32_t id;
  {
    std::lock_guard lock(sourcesMutex_);
    id = nextSource_++;
    sources_[id] = std::move(src);
  }
  r->view.streamSource = id;
  s.streamSource_ = id;
  s.bytes_ = residencyBytes(*r);
  s.owned_ = std::move(r);
  return Ok{};
}

// Demotes least-recently-used fully resident samples (not `keep`) until `incoming` more bytes fit the budget.
void SamplePool::enforceBudget(uint64_t incoming, const PooledSample* keep) {
  if (!config_.reclaimer) return;
  std::lock_guard budget(budgetMutex_);  // one demotion at a time (concurrent loads would pick the same victim)
  for (;;) {
    SampleRef victim;
    std::vector<SampleRef> alive;  // dropped after the lock: a last reference must not destroy under it
    {
      std::lock_guard lock(mutex_);
      if (residentBytes_ + headBytes_ + incoming <= config_.budgetBytes) return;
      uint64_t oldest = UINT64_MAX;
      for (auto& [k, w] : entries_) {
        SampleRef e = w.lock();
        if (!e) continue;
        if (e.get() != keep && e->streamSource_ == 0 && e->lastUse_ < oldest) {
          oldest = e->lastUse_;
          victim = e;
        }
        alive.push_back(std::move(e));
      }
    }
    alive.clear();
    if (!victim) return;  // nothing left to demote: the load goes over budget
    demote(const_cast<PooledSample&>(*victim));
  }
}

void SamplePool::demote(PooledSample& s) {
  const Residency* old = s.owned_.get();
  const uint64_t oldBytes = s.bytes_;
  PooledSample tmp;  // build the streamed residency on the side
  tmp.path_ = s.path_;
  tmp.info_ = s.info_;
  tmp.contentHash_ = s.contentHash_;
  if (!makeStreamed(tmp, nullptr, old)) return;
  std::unique_ptr<Residency> retired = std::move(s.owned_);
  s.owned_ = std::move(tmp.owned_);
  s.streamSource_ = tmp.streamSource_;
  s.residency_.store(s.owned_.get(), std::memory_order_release);
  config_.reclaimer->retire(retired.release());  // the audio thread may still read the old frames this block
  std::lock_guard lock(mutex_);
  residentBytes_ -= oldBytes;
  headBytes_ += tmp.bytes_;
  s.bytes_ = tmp.bytes_;
  ++streamedSamples_;
  ++demotions_;
}

// ---- disk thread --------------------------------------------------------------------------------------------------

void SamplePool::startDiskThread() {
  if (diskThread_.joinable()) return;
  stop_.store(false);
  disk_ = std::make_unique<DiskState>();
  disk_->local.resize(streams_.slotCount());
  for (uint32_t i = 0; i < kMaxInflight; ++i) {
    disk_->staging.emplace_back(kStagingBytes);
    disk_->events[i] = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  }
  disk_->timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  diskThread_ = std::thread([this] { diskMain(); });
}

void SamplePool::stopDiskThread() {
  if (!diskThread_.joinable()) return;
  stop_.store(true);
  SetEvent(wakeEvent_);
  diskThread_.join();
  for (HANDLE e : disk_->events)
    if (e) CloseHandle(e);
  if (disk_->timer) CloseHandle(disk_->timer);
  disk_.reset();
}

void SamplePool::engage() {
  engaged_.fetch_add(1);
  SetEvent(wakeEvent_);
}
void SamplePool::disengage() { engaged_.fetch_sub(1); }
void SamplePool::wake() { SetEvent(wakeEvent_); }

bool SamplePool::prefetch(const PooledSample& sample, int64_t frame, Direction dir) {
  const SampleView& v = sample.view();
  if (!v.streamed()) return false;
  if (dir == Direction::Forward ? frame >= v.frames : frame <= v.residentFrames) return false;
  const int64_t start = dir == Direction::Forward ? std::max(frame, v.residentFrames) : std::min(frame, v.frames);
  if (!streams_.addPrefetch(v.streamSource, start, dir, v.channels, v.frames)) return false;
  wake();
  return true;
}

void SamplePool::clearPrefetches() { streams_.clearPrefetches(); }

bool SamplePool::waitFilled(StreamId id, int64_t frames, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  waiters_.fetch_add(1);
  bool ok = false;
  for (;;) {
    const int64_t need = std::min(frames, streams_.remaining(id));
    if (streams_.buffered(id) >= need) {
      ok = true;
      break;
    }
    if (std::chrono::steady_clock::now() >= deadline) break;
    SetEvent(wakeEvent_);
    WaitForSingleObject(filledEvent_, 5);
  }
  waiters_.fetch_sub(1);
  return ok;
}

SamplePool::Stats SamplePool::stats() const {
  Stats s;
  {
    std::lock_guard lock(mutex_);
    for (auto& [k, w] : entries_)
      if (!w.expired()) ++s.samples;
    s.streamedSamples = streamedSamples_;
    s.residentBytes = residentBytes_;
    s.headBytes = headBytes_;
    s.demotions = demotions_;
  }
  s.ringBytesReserved = uint64_t{streams_.slotCount()} * streams_.ringFloats() * sizeof(float);
  s.ringBytesCommitted = committed_.load();
  s.diskWakeups = wakeups_.load();
  s.diskReads = reads_.load();
  s.diskBytesRead = bytesRead_.load();
  s.underruns = streams_.underruns();
  return s;
}

void SamplePool::diskMain() {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
  SetThreadDescription(GetCurrentThread(), L"pqdaw disk");
  bool busy = false;
  while (!stop_.load()) {
    if (busy || engaged_.load() > 0 || waiters_.load() > 0) {
      LARGE_INTEGER due;
      due.QuadPart = -20000;  // 2 ms, relative, in 100 ns units
      SetWaitableTimer(disk_->timer, &due, 0, nullptr, nullptr, FALSE);
      HANDLE both[2] = {wakeEvent_, disk_->timer};
      WaitForMultipleObjects(2, both, FALSE, INFINITE);
    } else {
      WaitForSingleObject(wakeEvent_, INFINITE);  // idle: no timeout, no wakeups
    }
    wakeups_.fetch_add(1, std::memory_order_relaxed);
    if (stop_.load()) break;
    diskPass();
    busy = false;
    for (const auto& l : disk_->local) busy |= l.active;
  }
}

void SamplePool::diskPass() {
  struct Job {
    uint32_t slot;
    uint32_t gen;
    int64_t first;  // frames [first, first + count)
    int64_t count;
    int64_t urgency;
    OVERLAPPED ov;
  };
  std::vector<Job> jobs;  // the disk thread is not realtime: a small vector per pass is fine
  const uint32_t ringBytes = streams_.ringFloats() * sizeof(float);
  for (uint32_t i = 0; i < streams_.slotCount(); ++i) {
    const StreamPool::Request r = streams_.request(i);
    DiskState::Local& l = disk_->local[i];
    if (!r.active) {
      if (l.active) {
        l.active = false;
        l.src.reset();
      }
      continue;
    }
    if (!l.active || l.gen != r.gen) {  // a new claim of this slot
      l.active = true;
      l.gen = r.gen;
      l.frontier = r.start;
      l.src = source(r.source);
      if (!l.committed) {
        if (VirtualAlloc(streams_.ring(i), ringBytes, MEM_COMMIT, PAGE_READWRITE)) {
          l.committed = true;
          committed_.fetch_add(ringBytes, std::memory_order_relaxed);
        }
      }
      streams_.publishFill(i, r.gen, l.frontier);
    }
    if (!l.src || !l.committed || r.channels != l.src->channels) continue;
    const int64_t consumed = streams_.consumedPosition(i, r.gen);
    if (consumed < 0) continue;
    const int64_t cap = streams_.ringFrames(r.channels);
    const int64_t chunk = kStagingBytes / l.src->blockAlign;
    // Refill as soon as a whole 256 KiB read fits (or a quarter of a small ring): rings stay nearly full.
    const int64_t minRead = std::min(chunk, cap / 4);
    if (r.dir == Direction::Forward) {
      if (consumed > l.frontier) {  // the reader ran past the data (an underrun): skip ahead
        l.frontier = consumed;
        streams_.publishFill(i, r.gen, l.frontier);
      }
      const int64_t end = std::min(r.frames, l.src->frames);
      const int64_t have = l.frontier - consumed;
      if (l.frontier >= end || cap - have < minRead) continue;
      const int64_t n = std::min({chunk, cap - have, end - l.frontier});
      jobs.push_back({i, r.gen, l.frontier, n, have, {}});
    } else {
      if (consumed < l.frontier) {
        l.frontier = consumed;
        streams_.publishFill(i, r.gen, l.frontier);
      }
      const int64_t have = consumed - l.frontier;
      if (l.frontier <= 0 || cap - have < minRead) continue;
      const int64_t n = std::min({chunk, cap - have, l.frontier});
      jobs.push_back({i, r.gen, l.frontier - n, n, have, {}});
    }
  }
  if (jobs.empty()) return;
  std::sort(jobs.begin(), jobs.end(), [](const Job& a, const Job& b) { return a.urgency < b.urgency; });
  if (jobs.size() > kMaxInflight) jobs.resize(kMaxInflight);
  // Issue every read, then complete them in order.
  for (std::size_t j = 0; j < jobs.size(); ++j) {
    Job& job = jobs[j];
    const Source& src = *disk_->local[job.slot].src;
    const uint64_t offset = src.dataOffset + static_cast<uint64_t>(job.first) * src.blockAlign;
    job.ov = {};
    job.ov.Offset = static_cast<DWORD>(offset);
    job.ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
    job.ov.hEvent = disk_->events[j];
    ResetEvent(disk_->events[j]);
    const DWORD bytes = static_cast<DWORD>(job.count * src.blockAlign);
    if (!ReadFile(src.file.handle(), disk_->staging[j].data(), bytes, nullptr, &job.ov) &&
        GetLastError() != ERROR_IO_PENDING)
      job.count = 0;  // read error: the stream stays where it is (underruns show it)
  }
  bool filledAny = false;
  for (std::size_t j = 0; j < jobs.size(); ++j) {
    Job& job = jobs[j];
    DiskState::Local& l = disk_->local[job.slot];
    const Source& src = *l.src;
    DWORD got = 0;
    if (job.count == 0 || !GetOverlappedResult(src.file.handle(), &job.ov, &got, TRUE)) continue;
    reads_.fetch_add(1, std::memory_order_relaxed);
    bytesRead_.fetch_add(got, std::memory_order_relaxed);
    int64_t frames = got / src.blockAlign;
    if (frames <= 0) continue;
    if (frames < job.count && streams_.request(job.slot).dir == Direction::Reverse) {
      frames = 0;  // a short read below the frontier would leave a hole: retry next pass
      continue;
    }
    // Convert into the ring (two runs at the wrap).
    const uint32_t ch = src.channels;
    const int64_t cap = streams_.ringFrames(ch);
    float* ring = streams_.ring(job.slot);
    const uint8_t* raw = disk_->staging[j].data();
    int64_t first = job.first, left = frames;
    while (left > 0) {
      const int64_t at = first % cap;
      const int64_t run = std::min(left, cap - at);
      float* dst[kMaxStreamChannels];
      for (uint32_t c = 0; c < ch; ++c) dst[c] = ring + c * cap + at;
      pcmToPlanar(src.pcm, src.bigEndian, raw, ch, static_cast<std::size_t>(run), dst);
      raw += run * src.blockAlign;
      first += run;
      left -= run;
    }
    // Publish only if the slot still holds the claim this read was for.
    const StreamPool::Request now = streams_.request(job.slot);
    if (!now.active || now.gen != job.gen || l.gen != job.gen) continue;
    l.frontier = now.dir == Direction::Forward ? job.first + frames : job.first;
    streams_.publishFill(job.slot, job.gen, l.frontier);
    filledAny = true;
  }
  if (filledAny && waiters_.load() > 0) SetEvent(filledEvent_);
}

}  // namespace pq::media
