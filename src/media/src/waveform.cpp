// Waveform overviews (T5.5): the min / max pyramid, its progressive builder, the disk cache, sample windows and the
// background analysis worker. See pq/media/waveform.h.

#include "pq/media/waveform.h"

#include <immintrin.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <new>
#include <system_error>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "pq/media/decoder.h"
#include "pq/media/hash.h"

namespace pq::media {

namespace fs = std::filesystem;

int16_t quantiseMin(float x) noexcept {
  if (!(x == x)) return 0;
  const float v = std::floor(x * kOverviewScale);
  return static_cast<int16_t>(std::clamp(v, -32768.0f, 32767.0f));
}

int16_t quantiseMax(float x) noexcept {
  if (!(x == x)) return 0;
  const float v = std::ceil(x * kOverviewScale);
  return static_cast<int16_t>(std::clamp(v, -32768.0f, 32767.0f));
}

// ---- WaveformOverview ---------------------------------------------------------------------------------------------

uint64_t WaveformOverview::bytesFor(uint32_t channels, uint64_t frames) noexcept {
  const uint64_t ch = std::max<uint32_t>(1, channels);
  uint64_t buckets = (std::max<uint64_t>(1, frames) + kOverviewBucketFrames - 1) / kOverviewBucketFrames;
  uint64_t values = 0;
  for (;;) {
    values += buckets * 2 * ch;
    if (buckets <= 1) break;
    buckets = (buckets + kOverviewMipFactor - 1) / kOverviewMipFactor;
  }
  return values * sizeof(int16_t);
}

std::shared_ptr<WaveformOverview> WaveformOverview::create(uint32_t channels, uint64_t frames, uint32_t sampleRate,
                                                           uint64_t contentHash) {
  std::shared_ptr<WaveformOverview> o(new WaveformOverview());
  o->channels_ = std::max<uint32_t>(1, channels);
  o->frames_ = std::max<uint64_t>(1, frames);
  o->sampleRate_ = sampleRate;
  o->contentHash_ = contentHash;
  uint64_t fpb = kOverviewBucketFrames;
  uint64_t buckets = (o->frames_ + fpb - 1) / fpb;
  std::size_t offset = 0;
  for (;;) {
    o->levels_.push_back({fpb, buckets, offset});
    offset += static_cast<std::size_t>(buckets) * 2 * o->channels_;
    if (buckets <= 1) break;
    buckets = (buckets + kOverviewMipFactor - 1) / kOverviewMipFactor;
    fpb *= kOverviewMipFactor;
  }
  o->data_.assign(offset, 0);
  return o;
}

uint64_t WaveformOverview::validBuckets(std::size_t i) const noexcept {
  const uint64_t b = builtFrames();
  if (b >= frames_) return levels_[i].buckets;
  return std::min(levels_[i].buckets, b / levels_[i].framesPerBucket);
}

// ---- builder ------------------------------------------------------------------------------------------------------

namespace {

// Min and max of n floats (n a multiple of 8 for the vector part).
inline void minMaxOf(const float* p, std::size_t n, float& lo, float& hi) noexcept {
  std::size_t i = 0;
  if (n >= 32) {
    __m256 mn0 = _mm256_loadu_ps(p), mn1 = _mm256_loadu_ps(p + 8), mn2 = _mm256_loadu_ps(p + 16), mn3 = _mm256_loadu_ps(p + 24);
    __m256 mx0 = mn0, mx1 = mn1, mx2 = mn2, mx3 = mn3;
    for (i = 32; i + 32 <= n; i += 32) {
      const __m256 a = _mm256_loadu_ps(p + i), b = _mm256_loadu_ps(p + i + 8), c = _mm256_loadu_ps(p + i + 16),
                   d = _mm256_loadu_ps(p + i + 24);
      mn0 = _mm256_min_ps(mn0, a);
      mx0 = _mm256_max_ps(mx0, a);
      mn1 = _mm256_min_ps(mn1, b);
      mx1 = _mm256_max_ps(mx1, b);
      mn2 = _mm256_min_ps(mn2, c);
      mx2 = _mm256_max_ps(mx2, c);
      mn3 = _mm256_min_ps(mn3, d);
      mx3 = _mm256_max_ps(mx3, d);
    }
    __m256 mn = _mm256_min_ps(_mm256_min_ps(mn0, mn1), _mm256_min_ps(mn2, mn3));
    __m256 mx = _mm256_max_ps(_mm256_max_ps(mx0, mx1), _mm256_max_ps(mx2, mx3));
    __m128 m4 = _mm_min_ps(_mm256_castps256_ps128(mn), _mm256_extractf128_ps(mn, 1));
    m4 = _mm_min_ps(m4, _mm_movehl_ps(m4, m4));
    m4 = _mm_min_ss(m4, _mm_shuffle_ps(m4, m4, 1));
    __m128 x4 = _mm_max_ps(_mm256_castps256_ps128(mx), _mm256_extractf128_ps(mx, 1));
    x4 = _mm_max_ps(x4, _mm_movehl_ps(x4, x4));
    x4 = _mm_max_ss(x4, _mm_shuffle_ps(x4, x4, 1));
    lo = std::min(lo, _mm_cvtss_f32(m4));
    hi = std::max(hi, _mm_cvtss_f32(x4));
  }
  for (; i < n; ++i) {
    lo = std::min(lo, p[i]);
    hi = std::max(hi, p[i]);
  }
}

}  // namespace

OverviewBuilder::OverviewBuilder(WaveformOverview& target, std::function<void()> onPublish)
    : o_(target), onPublish_(std::move(onPublish)) {
  runMin_.assign(o_.channels(), 0.0f);
  runMax_.assign(o_.channels(), 0.0f);
  mipDone_.assign(o_.levelCount(), 0);
}

void OverviewBuilder::closeBucket() {
  const uint64_t b = (pos_ - 1) / kOverviewBucketFrames;
  for (uint32_t c = 0; c < o_.channels(); ++c) {
    int16_t* mm = o_.minMaxMut(0, c);
    mm[2 * b] = quantiseMin(runMin_[c]);
    mm[2 * b + 1] = quantiseMax(runMax_[c]);
  }
  bucketFill_ = 0;
}

void OverviewBuilder::add(const float* const* planar, uint64_t n) {
  n = std::min<uint64_t>(n, o_.frames() - pos_);
  uint64_t at = 0;
  const uint32_t ch = o_.channels();
  constexpr uint64_t B = kOverviewBucketFrames;
  while (at < n) {
    if (bucketFill_ == 0 && n - at >= B) {
      // Whole buckets straight from the input (the common case).
      const uint64_t whole = (n - at) / B;
      const uint64_t b0 = pos_ / B;
      for (uint32_t c = 0; c < ch; ++c) {
        const float* src = planar[c] + at;
        int16_t* mm = o_.minMaxMut(0, c) + 2 * b0;
        for (uint64_t k = 0; k < whole; ++k) {
          float lo = src[k * B], hi = lo;
          minMaxOf(src + k * B, B, lo, hi);
          mm[2 * k] = quantiseMin(lo);
          mm[2 * k + 1] = quantiseMax(hi);
        }
      }
      at += whole * B;
      pos_ += whole * B;
    } else {
      const uint64_t m = std::min(n - at, B - bucketFill_);
      for (uint32_t c = 0; c < ch; ++c) {
        float lo = bucketFill_ ? runMin_[c] : planar[c][at];
        float hi = bucketFill_ ? runMax_[c] : planar[c][at];
        minMaxOf(planar[c] + at, static_cast<std::size_t>(m), lo, hi);
        runMin_[c] = lo;
        runMax_[c] = hi;
      }
      bucketFill_ += m;
      at += m;
      pos_ += m;
      if (bucketFill_ == B || pos_ == o_.frames()) closeBucket();
    }
  }
  if (pos_ >= nextPublish_ && pos_ < o_.frames()) {
    updateMips(false);
    o_.publish(pos_);
    nextPublish_ = (pos_ / kOverviewPublishFrames + 1) * kOverviewPublishFrames;
    if (onPublish_) onPublish_();
  }
}

void OverviewBuilder::updateMips(bool final) {
  uint64_t completeBelow = final ? o_.level(0).buckets : pos_ / kOverviewBucketFrames;
  for (std::size_t L = 1; L < o_.levelCount(); ++L) {
    const WaveformOverview::Level& lv = o_.level(L);
    const WaveformOverview::Level& below = o_.level(L - 1);
    const uint64_t complete = final ? lv.buckets : std::min(lv.buckets, completeBelow / kOverviewMipFactor);
    for (uint32_t c = 0; c < o_.channels(); ++c) {
      const int16_t* src = o_.minMax(L - 1, c);
      int16_t* dst = o_.minMaxMut(L, c);
      for (uint64_t k = mipDone_[L]; k < complete; ++k) {
        const uint64_t a = k * kOverviewMipFactor, e = std::min(a + kOverviewMipFactor, below.buckets);
        int16_t lo = src[2 * a], hi = src[2 * a + 1];
        for (uint64_t j = a + 1; j < e; ++j) {
          lo = std::min(lo, src[2 * j]);
          hi = std::max(hi, src[2 * j + 1]);
        }
        dst[2 * k] = lo;
        dst[2 * k + 1] = hi;
      }
    }
    mipDone_[L] = complete;
    completeBelow = complete;
  }
}

void OverviewBuilder::finish() {
  if (bucketFill_ > 0) closeBucket();
  // A short read: the rest of the file reads as silence (the buckets are zero from create()).
  pos_ = o_.frames();
  updateMips(true);
  o_.publish(o_.frames());
  if (onPublish_) onPublish_();
}

Result<std::shared_ptr<WaveformOverview>> buildOverview(Decoder& decoder, uint64_t contentHash,
                                                        const std::function<void(const OverviewPtr&)>& onPublish,
                                                        const std::atomic<bool>* cancel) {
  const SampleInfo& info = decoder.info();
  if (info.channels == 0 || info.frames == 0)
    return Error{ErrorCode::Unsupported, "waveform overview: the file has no audio frames", {}};
  // M5 review H1: the probe caps declared lengths by the file size; this bound is the last line (an overview of more
  // than 2^40 frames, 290 days at 44.1 kHz, is a corrupt header, and its buffer would not fit in memory).
  if (info.frames > kMaxOverviewFrames || info.channels > 64)
    return Error{ErrorCode::Unsupported, "waveform overview: implausible length or channel count", {}};
  std::shared_ptr<WaveformOverview> o;
  try {
    o = WaveformOverview::create(info.channels, info.frames, info.sampleRate, contentHash);
  } catch (const std::bad_alloc&) {
    return Error{ErrorCode::Overflow, "waveform overview: out of memory", {}};
  }
  OverviewBuilder b(*o, onPublish ? std::function<void()>([&] { onPublish(o); }) : std::function<void()>{});
  if (auto s = decoder.seek(0); !s) return std::move(s).error();
  constexpr uint64_t kChunk = 1u << 16;
  std::vector<float> buf(static_cast<std::size_t>(kChunk) * info.channels);
  std::vector<float*> ptrs(info.channels);
  for (uint32_t c = 0; c < info.channels; ++c) ptrs[c] = buf.data() + static_cast<std::size_t>(c) * kChunk;
  while (b.position() < info.frames) {
    if (cancel && cancel->load(std::memory_order_relaxed)) return Error{ErrorCode::Unknown, "waveform overview: cancelled", {}};
    Result<uint64_t> got = decoder.read(ptrs.data(), kChunk);
    if (!got) return std::move(got).error();
    if (*got == 0) break;  // truncated: the rest stays silent
    b.add(ptrs.data(), *got);
  }
  b.finish();
  return o;
}

// ---- disk cache ---------------------------------------------------------------------------------------------------

namespace {

struct FileHeader {
  char magic[4];
  uint32_t version;
  uint32_t channels;
  uint32_t sampleRate;
  uint64_t frames;
  uint64_t contentHash;
  uint64_t dataBytes;
  uint32_t bucketFrames;
  uint32_t mipFactor;
  uint8_t pad[16];
};
static_assert(sizeof(FileHeader) == 64);
constexpr char kMagic[4] = {'P', 'Q', 'W', 'F'};
constexpr uint32_t kVersion = 1;

}  // namespace

fs::path overviewCacheFile(const fs::path& dir, uint64_t contentHash, uint64_t fileSize, int64_t sourceMtime) {
  char name[96];
  if (sourceMtime == 0)
    std::snprintf(name, sizeof name, "%016llx-%llx.pqwf", static_cast<unsigned long long>(contentHash),
                  static_cast<unsigned long long>(fileSize));
  else
    std::snprintf(name, sizeof name, "%016llx-%llx-%llx.pqwf", static_cast<unsigned long long>(contentHash),
                  static_cast<unsigned long long>(fileSize), static_cast<unsigned long long>(sourceMtime));
  return dir / name;
}

int64_t sourceMtimeOf(const fs::path& file) noexcept {
  std::error_code ec;
  const auto t = fs::last_write_time(file, ec);
  return ec ? 0 : static_cast<int64_t>(t.time_since_epoch().count());
}

Status saveOverview(const WaveformOverview& o, const fs::path& file) {
  if (!o.complete()) return Error{ErrorCode::InvalidArgument, "waveform overview: not complete", {}};
  std::error_code ec;
  fs::create_directories(file.parent_path(), ec);
  FileHeader h{};
  std::memcpy(h.magic, kMagic, 4);
  h.version = kVersion;
  h.channels = o.channels();
  h.sampleRate = o.sampleRate();
  h.frames = o.frames();
  h.contentHash = o.contentHash();
  h.dataBytes = o.bytes();
  h.bucketFrames = kOverviewBucketFrames;
  h.mipFactor = kOverviewMipFactor;
  // Unique per writer (M5 review H6: the browser's worker and the views' could write one `.part` at once); the
  // rename stays atomic, the last complete writer wins.
  fs::path part = file;
  part += "." + std::to_string(GetCurrentProcessId()) + "." + std::to_string(GetCurrentThreadId()) + ".part";
  {
    std::ofstream out(part, std::ios::binary | std::ios::trunc);
    if (!out) return Error{ErrorCode::Io, "waveform overview: cannot write " + part.string(), {}};
    out.write(reinterpret_cast<const char*>(&h), sizeof h);
    out.write(reinterpret_cast<const char*>(o.data().data()), static_cast<std::streamsize>(o.bytes()));
    if (!out) return Error{ErrorCode::Io, "waveform overview: write failed " + part.string(), {}};
  }
  fs::rename(part, file, ec);
  if (ec) {
    fs::remove(part, ec);
    return Error{ErrorCode::Io, "waveform overview: cannot rename to " + file.string(), {}};
  }
  return Ok{};
}

Result<std::shared_ptr<WaveformOverview>> loadOverview(const fs::path& file, uint64_t contentHash, bool touch) {
  std::ifstream in(file, std::ios::binary);
  if (!in) return Error{ErrorCode::NotFound, "waveform overview: no cache file " + file.string(), {}};
  FileHeader h{};
  in.read(reinterpret_cast<char*>(&h), sizeof h);
  if (!in || std::memcmp(h.magic, kMagic, 4) != 0 || h.version != kVersion || h.channels == 0 || h.channels > 64 ||
      h.frames == 0 || h.bucketFrames != kOverviewBucketFrames || h.mipFactor != kOverviewMipFactor ||
      (contentHash != 0 && h.contentHash != contentHash))
    return Error{ErrorCode::Parse, "waveform overview: bad cache file " + file.string(), {}};
  std::error_code ec;
  const uint64_t size = fs::file_size(file, ec);
  if (ec || size != sizeof h + h.dataBytes) return Error{ErrorCode::Parse, "waveform overview: truncated cache file " + file.string(), {}};
  // M5 review H1: the header's frames must match its data size *before* anything is allocated from them.
  if (h.frames > kMaxOverviewFrames || WaveformOverview::bytesFor(h.channels, h.frames) != h.dataBytes)
    return Error{ErrorCode::Parse, "waveform overview: inconsistent cache file " + file.string(), {}};
  std::shared_ptr<WaveformOverview> o;
  try {
    o = WaveformOverview::create(h.channels, h.frames, h.sampleRate, h.contentHash);
  } catch (const std::bad_alloc&) {
    return Error{ErrorCode::Overflow, "waveform overview: out of memory", {}};
  }
  if (o->bytes() != h.dataBytes) return Error{ErrorCode::Parse, "waveform overview: inconsistent cache file " + file.string(), {}};
  in.read(reinterpret_cast<char*>(o->dataMut().data()), static_cast<std::streamsize>(h.dataBytes));
  if (!in) return Error{ErrorCode::Io, "waveform overview: read failed " + file.string(), {}};
  o->publish(o->frames());
  if (touch) fs::last_write_time(file, fs::file_time_type::clock::now(), ec);
  return o;
}

void trimOverviewCache(const fs::path& dir, uint64_t limitBytes) {
  if (limitBytes == 0) return;
  struct Entry {
    fs::path path;
    uint64_t size;
    fs::file_time_type time;
  };
  std::vector<Entry> files;
  uint64_t total = 0;
  std::error_code ec;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    if (!it->is_regular_file(ec) || it->path().extension() != ".pqwf") continue;
    const uint64_t s = it->file_size(ec);
    if (ec) continue;
    files.push_back({it->path(), s, it->last_write_time(ec)});
    total += s;
  }
  if (total <= limitBytes) return;
  std::sort(files.begin(), files.end(), [](const Entry& a, const Entry& b) { return a.time < b.time; });
  for (const Entry& e : files) {
    if (total <= limitBytes) break;
    if (fs::remove(e.path, ec)) total -= e.size;
  }
}

namespace {

Result<OverviewPtr> loadOrBuild(const OverviewRequest& r, const std::function<void(const OverviewPtr&)>& onPublish,
                                const std::atomic<bool>* cancel) {
  uint64_t hash = r.contentHash;
  if (hash == 0) {
    Result<uint64_t> h = contentHash(r.path);
    if (!h) return std::move(h).error();
    hash = *h;
  }
  std::error_code ec;
  const uint64_t size = fs::file_size(r.path, ec);
  if (ec) return Error{ErrorCode::NotFound, "waveform overview: cannot read " + r.path.string(), {}};
  fs::path cacheFile;
  if (!r.cacheDir.empty()) {
    cacheFile = overviewCacheFile(r.cacheDir, hash, size, sourceMtimeOf(r.path));  // H2: mtime in the key
    if (fs::exists(cacheFile, ec)) {
      Result<std::shared_ptr<WaveformOverview>> o = loadOverview(cacheFile, hash, r.cacheLimitBytes > 0);
      if (o) {
        OverviewPtr p = std::move(*o);
        if (onPublish) onPublish(p);
        return p;
      }
    }
  }
  Result<std::unique_ptr<Decoder>> d = Decoder::open(r.path);
  if (!d) return std::move(d).error();
  Result<std::shared_ptr<WaveformOverview>> o = buildOverview(**d, hash, onPublish, cancel);
  if (!o) return std::move(o).error();
  if (!cacheFile.empty()) {
    if (saveOverview(**o, cacheFile)) trimOverviewCache(r.cacheDir, r.cacheLimitBytes);
  }
  return OverviewPtr(std::move(*o));
}

}  // namespace

Result<OverviewPtr> loadOrBuildOverview(const OverviewRequest& r, const std::function<void(const OverviewPtr&)>& onPublish,
                                        const std::atomic<bool>* cancel) {
  // Runs on worker threads, where an escaping exception terminates the app (M5 review H1): out of memory on a huge
  // (or corrupt) file is an error for that file, nothing more.
  try {
    return loadOrBuild(r, onPublish, cancel);
  } catch (const std::bad_alloc&) {
    return Error{ErrorCode::Overflow, "waveform overview: out of memory for " + r.path.string(), {}};
  }
}

// ---- sample windows -----------------------------------------------------------------------------------------------

Result<SampleWindowPtr> readSampleWindow(const fs::path& path, int64_t frame0, int64_t frames) {
  Result<std::unique_ptr<Decoder>> d = Decoder::open(path);
  if (!d) return std::move(d).error();
  const SampleInfo& info = (*d)->info();
  const int64_t total = static_cast<int64_t>(info.frames);
  const int64_t a = std::clamp<int64_t>(frame0, 0, total);
  const int64_t e = std::clamp<int64_t>(frame0 + std::max<int64_t>(frames, 0), a, total);
  auto w = std::make_shared<SampleWindow>();
  w->frame0 = a;
  w->frames = e - a;
  w->channels = info.channels;
  w->fileFrames = total;
  w->data.assign(static_cast<std::size_t>(w->frames) * info.channels, 0.0f);
  if (w->frames > 0) {
    if (auto s = (*d)->seek(static_cast<uint64_t>(a)); !s) return std::move(s).error();
    std::vector<float*> ptrs(info.channels);
    for (uint32_t c = 0; c < info.channels; ++c) ptrs[c] = w->data.data() + static_cast<std::size_t>(c) * static_cast<std::size_t>(w->frames);
    uint64_t done = 0;
    while (done < static_cast<uint64_t>(w->frames)) {
      std::vector<float*> at(ptrs);
      for (float*& p : at) p += done;
      Result<uint64_t> got = (*d)->read(at.data(), static_cast<uint64_t>(w->frames) - done);
      if (!got) return std::move(got).error();
      if (*got == 0) break;
      done += *got;
    }
  }
  return SampleWindowPtr(std::move(w));
}

// ---- analysis worker ----------------------------------------------------------------------------------------------

AnalysisWorker::AnalysisWorker(bool background) : thread_([this, background] { run(background); }) {}

AnalysisWorker::~AnalysisWorker() {
  {
    std::lock_guard<std::mutex> l(m_);
    stop_.store(true, std::memory_order_relaxed);
    jobs_.clear();
  }
  cv_.notify_all();
  thread_.join();
}

void AnalysisWorker::post(std::function<void()> job) {
  {
    std::lock_guard<std::mutex> l(m_);
    jobs_.push_back(std::move(job));
  }
  cv_.notify_one();
}

bool AnalysisWorker::idle() const {
  std::lock_guard<std::mutex> l(m_);
  return jobs_.empty() && !busy_;
}

void AnalysisWorker::waitIdle() {
  std::unique_lock<std::mutex> l(m_);
  idleCv_.wait(l, [&] { return (jobs_.empty() && !busy_) || stop_.load(std::memory_order_relaxed); });
}

void AnalysisWorker::run(bool background) {
  // Background mode lowers the thread's CPU, I/O and memory priority (Windows' THREAD_MODE_BACKGROUND_BEGIN).
  if (background) SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
  for (;;) {
    std::function<void()> job;
    {
      std::unique_lock<std::mutex> l(m_);
      busy_ = false;
      idleCv_.notify_all();
      cv_.wait(l, [&] { return !jobs_.empty() || stop_.load(std::memory_order_relaxed); });
      if (stop_.load(std::memory_order_relaxed)) break;
      job = std::move(jobs_.front());
      jobs_.pop_front();
      busy_ = true;
    }
    job();
  }
  if (background) SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
  std::lock_guard<std::mutex> l(m_);
  busy_ = false;
  idleCv_.notify_all();
}

}  // namespace pq::media
