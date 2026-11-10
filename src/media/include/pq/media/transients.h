#pragma once

// Transient (onset) detection for Beats warping (T6.3; docs/plans/M6.md T6.3): where a sample's notes and hits start,
// in source frames, so the clip player can cut a Beats clip into slices at them (pq/engine/audio_sequence.h).
//
// Detector (TransientDetector): the channels' mean, a short-time spectrum (pq/dsp/fft.h; N = 1024 frames at ≤ 48 kHz,
// the power of two at or above 21 ms; hop N / 4; Hann), 12 bands per octave from 30 Hz (each band's peak bin),
// log-compressed M = log(1 + 100·|X|·4/N), and the rectified flux against the previous frame's maximum over the
// neighbouring bands (so vibrato does not read as onsets): flux_t = Σ_b max(0, M_t[b] − max(M_{t−1}[b−1 … b+1])). An onset is a flux peak that is the maximum within ±3 hops, at least `threshold` × the
// file's median flux (and 1 % of its largest) above the local mean (10 hops before, 3 after), and at least
// `minGapSeconds` after the previous one (the larger peak wins). Its position is refined in the time domain: the knee of the cumulative energy (16-frame
// blocks) over the analysis window, where the best two-line fit breaks, at the lines' intersection; so a sharp attack
// lands within a few ms: the knee, then the first 16-frame block within ±3 ms whose energy passes 20 % of the way
// from the level before to the level after (tests/media: the fixture's sharp attacks within 0.3 ms median, 90 % within
// 3.2 ms; isolated drum hits within 0.5 ms).
//
// Cache: per content hash (pq/media/hash.h) a small file `<dir>/<hash>-<frames>.pqtr` (hex; header + float64 onsets),
// written to `.part` and renamed, and a process-wide memory cache (transientCache()). loadOrDetectTransients reads
// the memory cache, then the file, else detects (and fills both). Not RT code: detection runs on a worker thread
// (the playback link's loader); the audio thread only reads the onsets the image builder copied.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "pq/core/result.h"
#include "pq/dsp/fft.h"
#include "pq/media/stream.h"

namespace pq::media {

class Decoder;

struct TransientOptions {
  float threshold = 5.0f;        // × the file's median flux, above the local mean
  float minGapSeconds = 0.03f;   // between two onsets
};

class TransientDetector {
public:
  explicit TransientDetector(uint32_t sampleRate, const TransientOptions& options = {});
  // Appends n frames of planar audio (`channels` pointers; more than 2 are averaged too).
  void push(const float* const* ch, uint32_t channels, uint32_t n);
  // The onsets (source frames, ascending). Ends the analysis.
  std::vector<double> finish();
  uint32_t fftSize() const noexcept { return n_; }
  uint32_t hop() const noexcept { return hop_; }

private:
  void frame(const float* x);
  uint32_t sr_, n_, hop_;
  TransientOptions opt_;
  dsp::RealFft fft_;
  std::vector<float> win_, buf_, tmp_, re_, im_, mag_, prev_;
  std::vector<uint32_t> edges_;  // band b = bins [edges_[b], edges_[b + 1])
  int64_t bufStart_ = 0;   // absolute frame of buf_[0] (negative: the leading silence)
  int64_t next_ = 0;       // absolute first frame of the next analysis frame
  int64_t total_ = 0;      // frames pushed
  std::vector<float> flux_;   // per analysis frame
  std::vector<float> fine_;   // energy per kFineBlock frames
  double fineAcc_ = 0.0;
  uint32_t fineFill_ = 0;
};

// Every onset of planar audio (`frames` frames).
std::vector<double> detectTransients(const float* const* ch, uint32_t channels, int64_t frames, uint32_t sampleRate,
                                     const TransientOptions& options = {});
// The same, decoding the whole file. `cancel` (optional) stops it (an error).
Result<std::vector<double>> detectTransients(Decoder& d, const std::atomic<bool>* cancel = nullptr,
                                             const TransientOptions& options = {});

struct Transients {
  uint64_t contentHash = 0;
  int64_t frames = 0;
  std::vector<double> onsets;  // source frames, ascending
};
using TransientsPtr = std::shared_ptr<const Transients>;

std::filesystem::path transientCacheFile(const std::filesystem::path& dir, uint64_t contentHash, int64_t frames);
Status saveTransients(const Transients& t, const std::filesystem::path& file);
Result<TransientsPtr> loadTransients(const std::filesystem::path& file);

// The process-wide memory cache (thread-safe).
class TransientCache {
public:
  TransientsPtr find(uint64_t contentHash, int64_t frames) const;
  void put(TransientsPtr t);
  void clear();

private:
  mutable std::mutex m_;
  std::unordered_map<uint64_t, TransientsPtr> map_;  // key: hash ^ frames·φ
};
TransientCache& transientCache();

struct TransientRequest {
  std::filesystem::path path;      // the file (decoded when `resident` is null)
  uint64_t contentHash = 0;
  int64_t frames = 0;              // the file's length
  const SampleView* resident = nullptr;  // a fully resident view of it: analysed in place (no decode)
  std::filesystem::path cacheDir;  // empty: no disk cache
};
// The memory cache, else the disk cache, else a detection (stored in both). Not RT-safe.
Result<TransientsPtr> loadOrDetectTransients(const TransientRequest& r, const std::atomic<bool>* cancel = nullptr);

}  // namespace pq::media
