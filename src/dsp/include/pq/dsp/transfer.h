#pragma once

// Transfer-function estimate (TX.spectrum; SPEC §15 2026-11-01 "analysis modules"): what a device does to the signal
// passing through it, measured from its input x and output y alone, so it works for any device (an EQ, a filter whose
// cutoff an LFO moves, a user patch), not only for those whose coefficients are known.
//
//   Welch / H1 estimator: per hop, X = FFT(w·x), Y = FFT(w·y) (periodic Hann, `fftSize`), and per bin the
//   exponentially averaged auto and cross spectra Sxx = ⟨|X|²⟩, Syy = ⟨|Y|²⟩, Sxy = ⟨X̄·Y⟩ (time constant
//   `averageSeconds`). Per display point (its span: half way to its neighbours on the caller's axis, at least the
//   nearest bin) the spectra are summed over the span's bins (frequency smoothing), then
//       H = Σ Sxy / Σ Sxx                    (|H| in dB: the response; H1 is unbiased by noise added at the output)
//       γ² = |Σ Sxy|² / (Σ Sxx · Σ Syy)      (coherence: 1 where y is a linear function of x, ≈ 0 for unrelated
//                                             signals, lower for a nonlinear or time-varying device)
//   A point's CONFIDENCE (0 … 1, the display's alpha) is smoothstep over γ² from `minCoherence` to `fullCoherence`
//   times smoothstep over the input level (the point's input power as dBFS of a sine) from `inputFloorDb` to 6 dB above
//   it, and over the level below the loudest point (`inputRangeDb`): where nothing excites the device (silence, a band
//   the input has no energy in) or the output does not follow the input (unrelated noise), there is no curve.
//
// Not for the audio thread (the analysis thread runs it); push() never allocates once constructed.

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace pq::dsp {

class RealFft;

struct TransferConfig {
  double sampleRate = 48000.0;
  uint32_t fftSize = 4096;      // 11.7 Hz bins at 48 kHz (above the crossover)
  uint32_t hop = 1024;          // 75 % overlap
  float averageSeconds = 0.35f; // the cross spectra's time constant
  // The lows (as the spectrum's "sharp lows"): a window lowFactor× longer (1.5 Hz bins at 48 kHz: a narrow bass
  // notch or a steep high-pass reads true), transformed every lowFactor·fftSize/8 samples (87.5 % overlap), averaged
  // over lowAverageSeconds, mixed in below crossoverHz (one octave around it, in dB). 1: off.
  uint32_t lowFactor = 8;
  float lowAverageSeconds = 0.7f;
  float crossoverHz = 400.0f;
  float minCoherence = 0.70f;   // γ² below this: no curve
  float fullCoherence = 0.90f;  // γ² above this: full confidence
  float inputFloorDb = -100.0f; // a point's input level below this: no curve
  float inputRangeDb = 70.0f;   // or more than this below the loudest point's
  float minDb = -120.0f, maxDb = 60.0f;  // |H| is clamped to this range
};

class TransferAnalyzer {
public:
  // `pointHz`: the display axis (ascending, > 0), copied. Allocates.
  TransferAnalyzer(const TransferConfig& config, std::span<const float> pointHz);
  ~TransferAnalyzer();
  TransferAnalyzer(const TransferAnalyzer&) = delete;
  TransferAnalyzer& operator=(const TransferAnalyzer&) = delete;

  const TransferConfig& config() const noexcept { return cfg_; }
  uint32_t points() const noexcept { return static_cast<uint32_t>(pointHz_.size()); }
  float pointHz(uint32_t p) const noexcept { return pointHz_[p]; }
  // Forgets everything (no curve until the averages fill again).
  void reset() noexcept;

  // Feeds n samples of the input x and the output y (time-aligned); runs one estimate per completed hop. Returns the
  // hops analysed. Allocation-free.
  uint32_t push(const float* x, const float* y, uint32_t n) noexcept;

  // Per point, after the last hop: |H| in dB, and its confidence (0: draw nothing).
  std::span<const float> magnitudeDb() const noexcept { return mag_; }
  std::span<const float> confidence() const noexcept { return conf_; }
  // Per point: the coherence γ² and the input level (dBFS of a sine) of the last estimate (tests).
  std::span<const float> coherence() const noexcept { return coh_; }
  std::span<const float> inputDb() const noexcept { return inDb_; }
  uint64_t hops() const noexcept { return hops_; }
  // Whether any point has confidence > 0.
  bool anyConfident() const noexcept;

private:
  struct Res;  // one FFT size: window, transforms, averaged spectra, per-point estimate
  void analyse() noexcept;

  TransferConfig cfg_;
  std::unique_ptr<Res> main_, low_;
  // The last histN_ samples of x and y, each a ring written twice (contiguous reads).
  std::vector<float> hx_, hy_;
  uint32_t histN_ = 0, histPos_ = 0;
  uint64_t total_ = 0;
  uint32_t sinceHop_ = 0;
  uint64_t hops_ = 0;
  std::vector<float> pointHz_;
  std::vector<float> lowWeight_;   // per point: the long window's share (1 below the crossover band, 0 above)
  size_t lowEnd_ = 0;              // points [0, lowEnd_) read the long window
  size_t mainBegin_ = 0;           // points [mainBegin_, P) read the main one
  std::vector<float> mag_, conf_, coh_, inDb_;
};

}  // namespace pq::dsp
