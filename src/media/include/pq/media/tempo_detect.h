#pragma once

// Tempo detection for auto-warp (T6.8; docs/plans/M6.md T6.8): a sample's tempo in 60 … 200 BPM with a confidence,
// from the onsets the transient detector finds (pq/media/transients.h), each weighted by how loud its attack is.
//
// Estimator (estimateTempo): the weighted inter-onset-interval histogram of every onset pair up to kMaxLagSeconds apart
// (1 ms bins, the weight w_i · w_j, smoothed by a Gaussian of σ = 2 ms) — the autocorrelation of the weighted onset
// train — normalised by the overlap (duration − lag) so long lags are not penalised for having fewer pairs. Each
// candidate tempo (0.02 BPM steps) scores the mean of that histogram at every multiple of its beat period up to
// kMaxLagSeconds (a comb). The comb alone cannot tell a tempo from half of it (a back-beat repeats every two beats), so
// a log-normal prior (σ = 0.8 octave) picks among its peaks, centred on the spacing of the LOUD hits: the median
// interval between consecutive onsets of at least 0.2 × the largest weight (kicks and snares, not hats), folded by
// octaves into the range (120 BPM with fewer than 4 such onsets). M6 review H3: when the file is a whole number of 4/4
// bars long at a candidate the comb supports, it is taken as a loop and every candidate is weighted by its bar count
// (a power of two 1, another whole number 0.8, a fraction 0.35): a one-bar loop at 90 is not read as 60. The best
// candidate is refined by a parabola through its neighbours. Confidence = log(best / median) / log(30) of the
// prior-free comb scores (the median floored at best / 1000), in [0, 1] (a steady loop ≈ 1, scattered onsets
// ≈ 0.2 – 0.4), then lowered by up to 60 % for a near-tie at ×2 / ÷2 (another octave explains the same onsets: a
// back-beat below 100 BPM with 8ths or 16ths is a half-time groove at twice the tempo) or ×2/3 / ×3/2, so auto-warp
// (≥ 0.6) leaves those to the user. Tested on synthesised loops (tests/media/test_tempo_detect.cpp: the 20 labelled
// ones, one-bar broken-beat / two-step loops, half-time grooves, 16th-hat loops), not on recordings: real material may
// need the clip view's ×2 / ÷2. Known: a loop of 16th hats alone below ~100 BPM still reads confidently doubled.
//
// Weights (onsetStrengths): the RMS rise over the onset (40 ms after minus 40 ms before, from an energy envelope of
// kEnvelopeBlock-frame blocks), divided by the largest, floored at 0.05.
//
// Cache: per content hash a small file `<dir>/<hash>-<frames>.pqtp` (header + bpm + confidence), written to `.part` and
// renamed, and a process-wide memory cache (tempoCache()). loadOrDetectTempo reads the memory cache, then the file, else
// analyses: one pass over the audio (in place when resident, else decoded) that builds the energy envelope and, unless
// the transients are cached already, runs the transient detector too (its result goes into the transient caches). Not
// RT code: it runs on a worker thread (the views' SampleAnalysis), once per file.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

#include "pq/core/result.h"
#include "pq/media/transients.h"

namespace pq::media {

struct TempoOptions {
  double minBpm = 60.0;
  double maxBpm = 200.0;
};

struct TempoEstimate {
  double bpm = 0.0;        // 0: none (fewer than 4 onsets)
  float confidence = 0.0f;  // [0, 1]
};

inline constexpr uint32_t kEnvelopeBlock = 128;     // frames per energy-envelope block
inline constexpr double kMaxLagSeconds = 8.0;       // the longest onset interval the histogram counts

// Mean square per kEnvelopeBlock frames of the channels' mean (the last block may be partial).
class EnergyEnvelope {
public:
  void push(const float* const* ch, uint32_t channels, uint32_t n);
  std::vector<float> finish();

private:
  std::vector<float> blocks_;
  double acc_ = 0.0;
  uint32_t fill_ = 0;
};

// The attack weight of each onset (see the file comment). `envelope` from EnergyEnvelope.
std::vector<float> onsetStrengths(std::span<const double> onsets, std::span<const float> envelope, double sampleRate);
// The tempo of onsets (source frames, ascending) weighted by `strengths` (empty: all 1) in a file of `frames` frames.
TempoEstimate estimateTempo(std::span<const double> onsets, std::span<const float> strengths, double sampleRate,
                            int64_t frames, const TempoOptions& options = {});
// Transients + envelope + estimate on planar audio in memory.
TempoEstimate detectTempo(const float* const* ch, uint32_t channels, int64_t frames, uint32_t sampleRate,
                          const TempoOptions& options = {});

struct Tempo {
  uint64_t contentHash = 0;
  int64_t frames = 0;
  TempoEstimate estimate;
};
using TempoPtr = std::shared_ptr<const Tempo>;

std::filesystem::path tempoCacheFile(const std::filesystem::path& dir, uint64_t contentHash, int64_t frames);
Status saveTempo(const Tempo& t, const std::filesystem::path& file);
Result<TempoPtr> loadTempo(const std::filesystem::path& file);

// The process-wide memory cache (thread-safe).
class TempoCache {
public:
  TempoPtr find(uint64_t contentHash, int64_t frames) const;
  void put(TempoPtr t);
  void clear();

private:
  mutable std::mutex m_;
  std::unordered_map<uint64_t, TempoPtr> map_;
};
TempoCache& tempoCache();

// The memory cache, else the disk cache, else an analysis (stored in both; the transients it detects on the way go
// into the transient caches too). Same request as the transients'. Not RT-safe.
Result<TempoPtr> loadOrDetectTempo(const TransientRequest& r, const std::atomic<bool>* cancel = nullptr,
                                   const TempoOptions& options = {});

}  // namespace pq::media
