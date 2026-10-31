#pragma once

// Waveform overviews (T5.5): what the arrangement and the clip view draw an audio file's waveform from, built once
// per file (content hash) and kept on disk.
//
// Format: per channel, int16 min / max pairs at kOverviewBucketFrames (256) frames per bucket (level 0), plus mip
// levels kOverviewMipFactor (4) times coarser each, up to one bucket for the whole file (1 h of 48 kHz stereo ≈
// 7 MB). A bucket's min is floor(min · 32767) and its max ceil(max · 32767), clamped to int16, so a mip bucket (the
// min of mins, the max of maxes) equals the brute-force min / max of its frames quantised the same way.
//
// Progressive builds: an overview is allocated at its full size before the first frame is read; the builder writes
// buckets in file order and publishes how many frames are done (builtFrames, a release store) every
// kOverviewPublishFrames (2^20) frames and at the end, so a long file fills in left to right while it builds. Readers
// (any thread) read only the buckets validBuckets() covers (an acquire load): those are complete and never written
// again, so reading them while the builder runs is race-free.
//
// Disk cache: `<dir>/<contentHash>-<size>.pqwf` (hex), a small header + the levels' data, written to `.part` and
// renamed once the overview is complete. trimOverviewCache keeps a shared directory under a byte limit (least
// recently written first; a load re-stamps the file so recently used files stay).
//
// Not RT code (overviews are UI data). Building runs on a worker (WaveformWorker below), never on the audio thread.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

#include "pq/core/result.h"

namespace pq::media {

class Decoder;

inline constexpr uint32_t kOverviewBucketFrames = 256;
inline constexpr uint32_t kOverviewMipFactor = 4;
inline constexpr uint64_t kOverviewPublishFrames = uint64_t{1} << 20;
// No overview is built or loaded for more frames than this (a corrupt header; M5 review H1).
inline constexpr uint64_t kMaxOverviewFrames = uint64_t{1} << 40;
inline constexpr float kOverviewScale = 32767.0f;  // int16 full scale (sample 1.0)

// The int16 value of a bucket's min / max (floor / ceil of x · 32767, clamped; NaN reads as 0).
int16_t quantiseMin(float x) noexcept;
int16_t quantiseMax(float x) noexcept;

class WaveformOverview {
public:
  struct Level {
    uint64_t framesPerBucket = 0;
    uint64_t buckets = 0;  // for the whole file
    std::size_t offset = 0;  // of channel 0's pairs in the data; channel c at offset + c · 2 · buckets
  };

  // Storage for a file of `frames` frames (≥ 1) and `channels` channels (≥ 1), nothing built yet.
  static std::shared_ptr<WaveformOverview> create(uint32_t channels, uint64_t frames, uint32_t sampleRate,
                                                  uint64_t contentHash);
  // The byte size create() would allocate for these dimensions, without allocating (validates cache headers).
  static uint64_t bytesFor(uint32_t channels, uint64_t frames) noexcept;

  uint32_t channels() const noexcept { return channels_; }
  uint64_t frames() const noexcept { return frames_; }
  uint32_t sampleRate() const noexcept { return sampleRate_; }
  uint64_t contentHash() const noexcept { return contentHash_; }
  std::size_t levelCount() const noexcept { return levels_.size(); }
  const Level& level(std::size_t i) const noexcept { return levels_[i]; }
  // Channel c's min / max pairs of level i (pair k = bucket k: [2k] min, [2k + 1] max).
  const int16_t* minMax(std::size_t i, uint32_t c) const noexcept {
    return data_.data() + levels_[i].offset + static_cast<std::size_t>(c) * 2 * levels_[i].buckets;
  }
  int16_t* minMaxMut(std::size_t i, uint32_t c) noexcept {
    return data_.data() + levels_[i].offset + static_cast<std::size_t>(c) * 2 * levels_[i].buckets;
  }
  // Frames built so far (monotone; == frames() once complete). Acquire: the buckets it covers are readable.
  uint64_t builtFrames() const noexcept { return built_.load(std::memory_order_acquire); }
  bool complete() const noexcept { return builtFrames() >= frames_; }
  // Buckets of level i that are complete (all their frames built; the file's last, shorter bucket once complete).
  uint64_t validBuckets(std::size_t i) const noexcept;
  // The data size in bytes (all levels, all channels).
  std::size_t bytes() const noexcept { return data_.size() * sizeof(int16_t); }
  std::span<const int16_t> data() const noexcept { return data_; }
  std::span<int16_t> dataMut() noexcept { return data_; }
  // Builder / loader side: frames [0, n) are written (release).
  void publish(uint64_t n) noexcept { built_.store(n, std::memory_order_release); }

private:
  WaveformOverview() = default;
  uint32_t channels_ = 0;
  uint32_t sampleRate_ = 0;
  uint64_t frames_ = 0;
  uint64_t contentHash_ = 0;
  std::vector<Level> levels_;
  std::vector<int16_t> data_;
  std::atomic<uint64_t> built_{0};
};
using OverviewPtr = std::shared_ptr<const WaveformOverview>;

// Feeds planar float frames into an overview in file order, publishing as it goes.
class OverviewBuilder {
public:
  // `onPublish` (optional) runs on the calling thread after each publish.
  explicit OverviewBuilder(WaveformOverview& target, std::function<void()> onPublish = {});
  // The next n frames of every channel (planar[c][0 … n)). Frames beyond the overview's length are ignored.
  void add(const float* const* planar, uint64_t n);
  // Completes the overview (a short read leaves the rest of the file at 0: silence) and publishes it.
  void finish();
  uint64_t position() const noexcept { return pos_; }

private:
  void closeBucket();
  void updateMips(bool final);
  WaveformOverview& o_;
  std::function<void()> onPublish_;
  uint64_t pos_ = 0;            // frames added
  uint64_t bucketFill_ = 0;     // frames in the open level-0 bucket
  std::vector<float> runMin_, runMax_;  // the open bucket's per-channel running min / max
  std::vector<uint64_t> mipDone_;       // per level ≥ 1: buckets final (never recomputed)
  uint64_t nextPublish_ = kOverviewPublishFrames;
};

// Decodes the whole file from frame 0 into a new overview (progressive: `onPublish` after each publish, on this
// thread). `cancel` (optional) is polled between chunks: the build stops (an error) when it turns true.
Result<std::shared_ptr<WaveformOverview>> buildOverview(Decoder& decoder, uint64_t contentHash,
                                                        const std::function<void(const OverviewPtr&)>& onPublish = {},
                                                        const std::atomic<bool>* cancel = nullptr);

// ---- disk cache -------------------------------------------------------------------------------------------------
// The cache file of a source with this content hash, size and last-write time (sourceMtimeOf; 0 = not in the key).
// The mtime keeps two files whose sampled content hash and size collide apart (M5 review H2).
std::filesystem::path overviewCacheFile(const std::filesystem::path& dir, uint64_t contentHash, uint64_t fileSize,
                                        int64_t sourceMtime = 0);
// A file's last-write time as file_time_type ticks (0 when it cannot be read).
int64_t sourceMtimeOf(const std::filesystem::path& file) noexcept;
// Writes a complete overview (atomically: `.part` then rename). Creates the directory.
Status saveOverview(const WaveformOverview& o, const std::filesystem::path& file);
// Reads an overview file; fails unless its header is consistent and names `contentHash` (0: any). Re-stamps the file's
// last-write time when `touch` (the LRU order of trimOverviewCache).
Result<std::shared_ptr<WaveformOverview>> loadOverview(const std::filesystem::path& file, uint64_t contentHash,
                                                       bool touch = false);
// Deletes the least recently written `.pqwf` files of `dir` until the rest take ≤ limitBytes (0: no limit).
void trimOverviewCache(const std::filesystem::path& dir, uint64_t limitBytes);

// ---- sample windows (deep zoom) -----------------------------------------------------------------------------------
// Frames [frame0, frame0 + frames) of a file, planar float32 (what the waveform draws below 256 frames per pixel).
struct SampleWindow {
  int64_t frame0 = 0;
  int64_t frames = 0;
  uint32_t channels = 0;
  std::vector<float> data;  // channel c at data[c · frames …]
  const float* channel(uint32_t c) const noexcept { return data.data() + static_cast<std::size_t>(c) * static_cast<std::size_t>(frames); }
};
using SampleWindowPtr = std::shared_ptr<const SampleWindow>;
// Reads [frame0, frame0 + frames) clamped to the file. Not RT-safe.
Result<SampleWindowPtr> readSampleWindow(const std::filesystem::path& path, int64_t frame0, int64_t frames);

// ---- the analysis worker --------------------------------------------------------------------------------------------
// One background thread that runs posted jobs in order. With `background` it runs in Windows' background mode
// (THREAD_MODE_BACKGROUND_BEGIN: low CPU and I/O priority), so building overviews never competes with the audio, disk
// or UI threads. Jobs check stopping() between chunks. The destructor drops queued jobs and joins the thread.
class AnalysisWorker {
public:
  explicit AnalysisWorker(bool background = true);
  ~AnalysisWorker();
  AnalysisWorker(const AnalysisWorker&) = delete;
  AnalysisWorker& operator=(const AnalysisWorker&) = delete;

  void post(std::function<void()> job);
  // Blocks until the queue is empty and no job runs (tests, scenes).
  void waitIdle();
  const std::atomic<bool>& stopping() const noexcept { return stop_; }
  bool idle() const;

private:
  void run(bool background);
  mutable std::mutex m_;
  std::condition_variable cv_, idleCv_;
  std::deque<std::function<void()>> jobs_;
  bool busy_ = false;
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

// What the overview of one file comes from: the disk cache when it holds it, else a build (saved to the cache when
// complete). `contentHash` 0 = computed here; `cacheDir` empty = no disk cache; `cacheLimitBytes` > 0 trims that
// directory after a save (the shared browser cache; project caches are not trimmed).
struct OverviewRequest {
  std::filesystem::path path;
  uint64_t contentHash = 0;
  std::filesystem::path cacheDir;
  uint64_t cacheLimitBytes = 0;
};
// Synchronous (the worker's job, and the no-worker path of tests): cache hit or build. `onPublish` as buildOverview.
Result<OverviewPtr> loadOrBuildOverview(const OverviewRequest& r, const std::function<void(const OverviewPtr&)>& onPublish = {},
                                        const std::atomic<bool>* cancel = nullptr);

}  // namespace pq::media
