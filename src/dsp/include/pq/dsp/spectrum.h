#pragma once

// Spectrum analyser (SPEC §11.1, T4.6): the behaviour of PQ Meter's spectrum module, reimplemented from its
// documented behaviour (PQ Meter is proprietary: no code is copied). Runs on the analysis thread; the UI draws from
// snapshots of levelsDb().
//
//   FFT 2048, hop 512, periodic Hann; amplitude = |X|·2/Σw (a full-scale sine reads 0 dB).
//   "Sharp lows": a 4× longer window (≤ 16384) ending on the same sample, transformed every other hop when it is
//   ≥ 8 hops long, crossfaded in below 500 Hz (one octave around it, 353.6 … 707.1 Hz: long weight
//   1 − smoothstep(log2(f / 353.6)), mixed in power).
//   `points` output points on the axis (fMin > 0: log from fMin to fMax; fMin = 0: linear to 40 Hz, log above, the
//   halves joined with equal slope). Each point reads its span (half way to its neighbours) with the tone-peak /
//   noise-level reader: noise = mean power over the span (× noiseBins), tone = the strongest bin's 3-bin lobe sum /
//   the window's 3-bin peak ratio (scalloping-free), blended by a crest test (strongest 3-bin sum over a robust
//   noise floor of a ≥ 13-bin window, against the expected maximum of that many noise bins + 7).
//   Optional 1/N-octave smoothing. Tilt (dB/oct about 1 kHz). Ballistics on dB per hop: attack 25 ms, release
//   350 ms. Peak hold (optional): 1.5 s, then 12 dB/s.
//
// Mono input (the caller mixes: PQ Meter's default is (L + R) / 2). push() never allocates; the constructor does.

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace pq::dsp {

class RealFft;

struct SpectrumConfig {
  double sampleRate = 48000.0;
  uint32_t fftSize = 2048;
  uint32_t hop = 512;
  bool sharpLows = true;      // the 4× window below 500 Hz
  uint32_t points = 1024;
  float fMin = 10.0f;         // 0: PQ Meter's hybrid linear/log axis
  float fMax = 22000.0f;
  float tiltDbPerOct = 4.5f;  // SPEC §11.1 (PQ Meter's own default is 0)
  float tiltPivotHz = 1000.0f;
  float smoothingOct = 0.0f;  // 1/N-octave smoothing width in octaves (0 = off; 1/24 … 1/3)
  float attackMs = 25.0f;
  float releaseMs = 350.0f;
  bool peakHold = false;
  float holdSeconds = 1.5f;
  float fallDbPerSecond = 12.0f;
  float floorDb = -140.0f;    // the level of silence (and the start value)
};

class SpectrumAnalyzer {
public:
  explicit SpectrumAnalyzer(const SpectrumConfig& config = {});
  ~SpectrumAnalyzer();
  SpectrumAnalyzer(const SpectrumAnalyzer&) = delete;
  SpectrumAnalyzer& operator=(const SpectrumAnalyzer&) = delete;

  const SpectrumConfig& config() const noexcept { return cfg_; }
  // Back to silence (levels at the floor, buffers empty).
  void reset() noexcept;

  // Feeds n mono samples; runs one analysis per completed hop. Returns the hops analysed. Allocation-free.
  uint32_t push(const float* x, uint32_t n) noexcept;

  // Per point, after tilt and ballistics (dB). Valid until the next push().
  std::span<const float> levelsDb() const noexcept { return levels_; }
  std::span<const float> peaksDb() const noexcept { return peaks_; }
  // The raw reading of the last hop (before ballistics; tests).
  std::span<const float> lastReadingDb() const noexcept { return reading_; }
  uint64_t hops() const noexcept { return hops_; }
  // True when every level is at or below `db` (an idle consumer may stop drawing).
  bool allBelow(float db) const noexcept;

  // The axis: t ∈ [0, 1] ↔ Hz. Point p sits at axisHz(p / (points − 1)).
  float axisHz(float t) const noexcept;
  float axisT(float hz) const noexcept;
  float pointHz(uint32_t p) const noexcept { return pointHz_[p]; }

private:
  struct Window;  // one FFT size: window, transform, power, prefix sums
  void analyse() noexcept;

  SpectrumConfig cfg_;
  std::unique_ptr<Window> main_, low_;
  uint32_t lowN_ = 0;
  bool lowEveryOther_ = false;
  bool lowValid_ = false;
  // Input history: the last lowN_ (or fftSize) samples, as a ring written twice (contiguous reads).
  std::vector<float> hist_;
  uint32_t histN_ = 0, histPos_ = 0;
  uint64_t total_ = 0;      // samples pushed since reset
  uint32_t sinceHop_ = 0;   // samples since the last analysis
  std::vector<float> pointHz_, spanLo_, spanHi_;  // per point: Hz and the span in Hz
  std::vector<float> tilt_, lowWeight_;           // per point: tilt dB, long-window weight
  std::vector<float> levels_, peaks_, peakAge_, reading_;
  std::vector<float> scratch_;
  float attackA_ = 1.0f, releaseA_ = 1.0f, hopSec_ = 0.0f;
  float axisA_ = 0.0f, axisL_ = 0.0f;  // hybrid axis: linear part share, log2(fMax / 40)
  uint64_t hops_ = 0;
};

}  // namespace pq::dsp
