#pragma once

// Root pitch detection for the sampler (SM.7; docs/plans/SM.md): a sample's fundamental in Hz with a confidence, so a
// dropped sample plays at its own pitch (rate = voice.hz / rootHz). Below kPitchConfidenceThreshold the sample is
// unpitched and reads as kUnpitchedHz (middle C), the sampler's default root.
//
// Where (pickPitchWindow): the steadiest loud ~0.5 s. The energy envelope (tempo_detect.h EnergyEnvelope, 128-frame
// blocks) of the first kPitchScanSeconds of the file is scored per window (steps of a quarter window) as
// mean dB − 1.5 × the dB standard deviation over its blocks, among the windows within 24 dB of the loudest; a file
// shorter than a window is analysed whole. Only that window is then analysed, so the cost does not grow with the
// file past the scan span (a 10 min file costs the decode of its first minute plus ~20 FFT frames).
//
// How (analysePitch): McLeod's normalised square difference function per frame, NSDF(τ) = 2 r(τ) / m(τ) with
// r(τ) = Σ x_j x_{j+τ} (from the power spectrum of the frame zero-padded to 2W: two forward real FFTs) and
// m(τ) = Σ x_j² + x_{j+τ}² (incrementally). W = the power of two at or above 2·sr / minHz (4096 at 48 kHz), hop W / 4,
// lags sr / maxHz … sr / minHz. Peak picking as McLeod: the key maxima (the largest NSDF value of each positive lobe
// after the first zero crossing); the first within kKeyMaximum (0.9) of the largest is the period, refined by a
// parabola through its neighbours; its height is the frame's clarity. Frames quieter than 40 dB under the loudest frame
// of the window are skipped. The frames within ±1 semitone of the frames' median count (octave slips drop
// out); the estimate is the midpoint of their 10th and 90th log-frequency percentiles (a vibrato's centre even over a
// partial cycle, where a mean is off by up to depth / (π · cycles): 4.6 ¢ for ±40 ¢ over 0.5 s; the clarity-weighted
// mean with fewer than 5 frames); the confidence is the sum of
// those frames' clarity over the number of loud frames (noise: scattered periods of low clarity ≈ 0; a drum hit: a
// few tonal frames among noisy ones). The fundamental need not be present: a tone of harmonics 2 … 5 repeats at the
// fundamental's period, and that is the key maximum the rule picks.
//
// Cache: a process-wide memory cache per content hash (pitchCache()); no disk cache (the scan is bounded, and the
// sampler stores the root it settled on in the document). Not RT code: it runs on the views' SampleAnalysis worker.

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>

#include "pq/core/result.h"
#include "pq/media/transients.h"

namespace pq::media {

inline constexpr float kUnpitchedHz = 261.63f;           // the root of an unpitched sample (middle C)
inline constexpr float kPitchConfidenceThreshold = 0.5f;  // below: unpitched
inline constexpr double kPitchScanSeconds = 60.0;        // the span searched for the analysis window

struct PitchOptions {
  float minHz = 30.0f;
  float maxHz = 4000.0f;
  float windowSeconds = 0.5f;
};

struct PitchEstimate {
  float hz = kUnpitchedHz;  // the root: the detected fundamental, or kUnpitchedHz when unpitched
  float confidence = 0.0f;  // [0, 1]
  bool pitched = false;     // confidence >= kPitchConfidenceThreshold
};

struct PitchWindow {
  int64_t start = 0;   // source frames
  int64_t frames = 0;
};

// The window to analyse, from an EnergyEnvelope of the file's first `scannedFrames` frames.
PitchWindow pickPitchWindow(std::span<const float> envelope, int64_t scannedFrames, double sampleRate,
                            const PitchOptions& options = {});
// The pitch of mono audio (the analysis window).
PitchEstimate analysePitch(std::span<const float> mono, double sampleRate, const PitchOptions& options = {});
// Window pick + analysis on planar audio in memory (more than one channel: their mean).
PitchEstimate detectPitch(const float* const* ch, uint32_t channels, int64_t frames, uint32_t sampleRate,
                          const PitchOptions& options = {});

struct Pitch {
  uint64_t contentHash = 0;
  int64_t frames = 0;
  PitchEstimate estimate;
};
using PitchPtr = std::shared_ptr<const Pitch>;

// The process-wide memory cache (thread-safe).
class PitchCache {
public:
  PitchPtr find(uint64_t contentHash, int64_t frames) const;
  void put(PitchPtr p);
  void clear();

private:
  mutable std::mutex m_;
  std::unordered_map<uint64_t, PitchPtr> map_;
};
PitchCache& pitchCache();

// The memory cache, else a detection (in place when r.resident is fully resident, else decoding the scan span, then
// seeking to the window). Same request as the transients' (cacheDir unused). Not RT-safe.
Result<PitchPtr> loadOrDetectPitch(const TransientRequest& r, const std::atomic<bool>* cancel = nullptr,
                                   const PitchOptions& options = {});

}  // namespace pq::media
