#pragma once

// The sample pool (T5.2): one entry per distinct audio file, shared by every image that plays it, plus the disk thread
// that fills stream rings. Non-RT except where marked (the audio thread reads PooledSample::view() and streams through
// pq/media/stream.h).
//
// Residency:
//   decoded size ≤ residentLimitBytes (32 MiB, ≈ 2.9 min of 48 kHz stereo) → fully resident: planar float32 with
//     kSampleGuardFrames zero frames on both sides, decoded once at load;
//   larger → a resident head (headFrames = 2^17 ≈ 2.7 s) plus streaming. Uncompressed files (WAV/AIFF/RF64/W64 PCM)
//     stream from the file itself; compressed ones (FLAC/MP3/OGG/ADPCM) are decoded once into
//     <decodedCacheDir>/<hash>.f32 (keyed by content hash + size, LRU-capped), so streaming always does plain reads.
//   The pool budget (budgetBytes, default 2 GiB: resident data + heads) demotes the least recently used resident
//   files to streamed when a load would exceed it. A demotion publishes a new residency and retires the old one
//   through the EpochReclaimer (the audio thread may still be reading it); without a reclaimer, nothing is demoted.
// Memory: stream rings = streamSlots · ringFloats floats, reserved at construction, committed per slot on first use;
// heads = streamed files · headFrames · channels floats; both are in Stats.
//
// Disk thread (startDiskThread): THREAD_PRIORITY_ABOVE_NORMAL; 256 KiB overlapped reads (FILE_FLAG_SEQUENTIAL_SCAN)
// into a staging buffer, converted into the rings (pq/media/pcm.h). It refills a ring as soon as one whole read fits
// (256 KiB, or a quarter of a small ring), most-starved first, up to 32 reads in flight (QUESTIONS Q?-T5.2-a: the
// plan's "below half full" stalls a reader whose block exceeds the gap; rings stay nearly full instead). It waits on an event that only non-RT code sets (engage(), wake(), prefetch(), waitFilled()):
// while engaged or while any stream is claimed it polls every 2 ms (a high-resolution waitable timer); otherwise it
// blocks without a timeout (0 wakeups when idle).

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "pq/core/aligned_buffer.h"
#include "pq/core/epoch_reclaimer.h"
#include "pq/core/result.h"
#include "pq/media/sample_info.h"
#include "pq/media/stream.h"

namespace pq::media {

class Decoder;

struct PoolConfig {
  uint64_t residentLimitBytes = 32ull << 20;
  uint64_t budgetBytes = 2ull << 30;
  uint32_t headFrames = 1u << 17;
  uint32_t streamSlots = 256;
  // Per slot: 196608 stereo frames = 1.5 MiB, about 1 s at 4× speed of a 48 kHz file (the plan's sizing). 256 slots
  // reserve 384 MiB of address space; only slots that have streamed commit theirs.
  uint32_t ringFloats = 2u * 196608u;
  std::filesystem::path decodedCacheDir;  // empty: %LOCALAPPDATA%\pqdaw\decoded
  uint64_t decodedCacheLimitBytes = 4ull << 30;  // LRU cap of that directory (0 = none)
  EpochReclaimer* reclaimer = nullptr;            // needed for demotion
};

// One file's frames as the audio thread sees them; replaced (never mutated) by a demotion.
struct Residency {
  SampleView view;
  AlignedBuffer<float> storage;  // channels · (residentFrames + 2 · guard)
};

class SamplePool;

class PooledSample {
public:
  ~PooledSample();
  PooledSample(const PooledSample&) = delete;
  PooledSample& operator=(const PooledSample&) = delete;

  // The current residency's view. RT-safe: one acquire load; valid until the caller's next quiescent state.
  const SampleView& view() const noexcept { return residency_.load(std::memory_order_acquire)->view; }
  const SampleInfo& info() const noexcept { return info_; }
  const std::filesystem::path& path() const noexcept { return path_; }
  uint64_t contentHash() const noexcept { return contentHash_; }

private:
  friend class SamplePool;
  PooledSample() = default;

  SamplePool* pool_ = nullptr;
  std::filesystem::path path_;
  std::string key_;
  SampleInfo info_;
  uint64_t contentHash_ = 0;
  // Pool-managed (under the pool mutex), hence mutable behind the shared const references.
  mutable std::atomic<const Residency*> residency_{nullptr};
  mutable std::unique_ptr<Residency> owned_;  // the current residency (retired, not freed, when replaced)
  mutable uint32_t streamSource_ = 0;
  mutable uint64_t lastUse_ = 0;              // pool tick of the last load()/touch()
  mutable uint64_t bytes_ = 0;                // what this entry counts toward the budget
};

using SampleRef = std::shared_ptr<const PooledSample>;

class SamplePool {
public:
  explicit SamplePool(PoolConfig config = {});
  ~SamplePool();  // stops the disk thread; every SampleRef must be gone
  SamplePool(const SamplePool&) = delete;
  SamplePool& operator=(const SamplePool&) = delete;

  // Loads (or finds) a file: probe, then decode resident data or the head + a stream source. The same file (volume
  // + file ID + size + mtime) returns the same entry while any reference lives. Not RT-safe (decodes; may take a
  // while for a long compressed file the first time). Thread-safe.
  Result<SampleRef> load(const std::filesystem::path& path);
  // Marks a sample as used now (LRU for demotion).
  void touch(const SampleRef& s);

  StreamPool& streams() noexcept { return streams_; }

  // ---- disk thread ------------------------------------------------------------------------------------------------
  void startDiskThread();
  void stopDiskThread();
  // While engaged (refcounted; the playback link brackets Play … Stop with engage/disengage) the disk thread polls the
  // stream slots every 2 ms, so RT claims are served. Non-RT.
  void engage();
  void disengage();
  // One pass soon (after a claim made outside engagement, or a new prefetch). Non-RT.
  void wake();
  // Prefetch for a locate while stopped: a stream for `sample` read from `frame` in `dir` (the part outside the
  // resident head), filled now and adopted by the RT claim that SampleCursor::start makes at the same position.
  // False if the sample is fully resident there (nothing to prefetch) or no slot is free.
  bool prefetch(const PooledSample& sample, int64_t frame, Direction dir);
  void clearPrefetches();
  // Offline renders: blocks until the stream has `frames` frames buffered ahead of its read position (or the file
  // ends), waking the disk thread. False on timeout. Not RT-safe (the render thread waits).
  bool waitFilled(StreamId id, int64_t frames, std::chrono::milliseconds timeout = std::chrono::seconds(10));

  struct Stats {
    uint64_t samples = 0;           // live entries
    uint64_t streamedSamples = 0;
    uint64_t residentBytes = 0;     // fully resident data (with guards)
    uint64_t headBytes = 0;         // heads of streamed files (with guards)
    uint64_t ringBytesReserved = 0;
    uint64_t ringBytesCommitted = 0;
    uint64_t demotions = 0;
    uint64_t diskWakeups = 0;       // times the disk thread returned from a wait
    uint64_t diskReads = 0;
    uint64_t diskBytesRead = 0;
    uint64_t underruns = 0;
  };
  Stats stats() const;
  const PoolConfig& config() const noexcept { return config_; }

private:
  friend class PooledSample;
  struct Source;
  struct DiskState;

  void removeEntry(PooledSample* s) noexcept;  // the last reference went
  Status makeStreamed(PooledSample& s, Decoder* d, const Residency* from);
  void enforceBudget(uint64_t incoming, const PooledSample* keep);
  void demote(PooledSample& s);
  void diskMain();
  void diskPass();
  std::shared_ptr<Source> source(uint32_t id);

  PoolConfig config_;
  StreamPool streams_;

  mutable std::mutex mutex_;  // entries_ and the budget accounting (non-RT threads only)
  std::unordered_map<std::string, std::weak_ptr<const PooledSample>> entries_;  // identity key → entry
  std::mutex budgetMutex_;    // serialises enforceBudget / demote (taken before mutex_, never under it)
  std::mutex sourcesMutex_;  // sources_ (taken by the disk thread briefly; never held across I/O)
  std::unordered_map<uint32_t, std::shared_ptr<Source>> sources_;
  uint32_t nextSource_ = 1;
  uint64_t tick_ = 0;
  uint64_t residentBytes_ = 0;
  uint64_t headBytes_ = 0;
  uint64_t demotions_ = 0;
  uint64_t streamedSamples_ = 0;

  std::unique_ptr<DiskState> disk_;
  std::thread diskThread_;
  std::atomic<bool> stop_{false};
  std::atomic<int32_t> engaged_{0};
  std::atomic<int32_t> waiters_{0};
  std::atomic<uint64_t> wakeups_{0};
  std::atomic<uint64_t> reads_{0};
  std::atomic<uint64_t> bytesRead_{0};
  std::atomic<uint64_t> committed_{0};
  void* wakeEvent_ = nullptr;    // auto-reset: engage / wake / prefetch / waiters
  void* filledEvent_ = nullptr;  // manual-reset pulses after a pass that filled something (waitFilled)
};

}  // namespace pq::media
