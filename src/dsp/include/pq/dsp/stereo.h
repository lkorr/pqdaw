#pragma once

// The stereo analysis (VZ.5; docs/plans/VZ.md "VZ.5"), ported from PQ Meter (pq meter/src/modules/stereo_image.cpp,
// phase_scope.cpp, stereo_field.cpp: their numbers):
//
// CrossSpectrum — one dual-channel analyser behind the Stereo Image and the Phase Scope. Every `hop` samples the newest
// `fftSize` of L and R are windowed (Hann or Kaiser β 6.2) and transformed; per point of a frequency axis (pq/dsp/
// freq_axis.h; `points` of them) the auto- and cross-spectra are summed over the point's bins (at least 2: coherence of
// one bin is 1 by construction) and give
//   level     dB of the pair (ENBW-normalised), eased with attack / release
//   pos       (R − L) / (R + L) in power, eased with release (−1 hard left … +1 hard right); held below `holdFloorDb`
//   coherence |avg L·conj(R)| / sqrt(avg|L|² · avg|R|²) — the spectra averaged first (Welch) on `coherenceMs`; a
//             single frame's ratio is ~1 whatever the input
//   width     1 − coherence, eased on coherenceMs, damped while the position moves (PQ: moving content keeps its shape)
//   monoDb    10 log10((avg|L|² + avg|R|² + 2 Re avg L·conj(R)) / (avg|L|² + avg|R|²)): what the band gains (+3 dB:
//             identical channels) or loses (→ −∞: R = −L) summed to mono; not gated by coherence
//   phaseDeg  the angle of the averaged cross-spectrum (R relative to L)
// and optionally (parts):
//   Glow       a points × kHeatCols grid where the tracked position ± width deposits its level (heat attack / decay)
//   Particles  PQ's heat-map tracker on its own SHORT window (`peakFft`): prominent spectral peaks (9 dB over their
//              neighbourhood, merged within a semitone), matched to a pool of kMaxParticles by a gated cost, trails,
//              spawn limits; plus the diffuse underlay of every bin splatted into the grid
//   Afterglow  per point a mark at the position / width of anything over `afterglowDb`, fading over afterglowS
// Not for the audio thread (the analysis worker). push() never allocates; configure() does.
//
// StereoFieldState — the goniometer's per-sample state (PQ stereo_field): the exponentially weighted correlation over
// `corrWindowMs`, the decaying trace radius for auto gain, and in band mode three Linkwitz-Riley (LR4) bands with their
// own correlation, radius and sample rings. The Stereo Field face runs it over the new frames of its feed's raw history.

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "pq/dsp/freq_axis.h"

namespace pq::dsp {

class RealFft;

enum class SpecWindow : uint8_t { Hann, Kaiser };
inline constexpr uint32_t kStereoGlow = 1u, kStereoParticles = 2u, kStereoAfterglow = 4u;

struct CrossSpectrumConfig {
  double sampleRate = 48000.0;
  uint32_t fftSize = 4096, hop = 256;
  SpecWindow window = SpecWindow::Kaiser;
  FreqScale scale = FreqScale::Log;
  float fMin = 20.0f, fMax = 20000.0f;
  uint32_t points = 256;
  float attackMs = 60.0f, releaseMs = 400.0f, coherenceMs = 400.0f;
  bool holdPos = true;
  float floorDb = -72.0f, ceilDb = -12.0f;  // the display's fade range (hold, rest, the grid's and particles' energy)
  uint32_t parts = 0;
  float widthScale = 1.0f;                       // Glow: the deposit's half width
  float heatAttackS = 0.05f, heatDecayS = 0.6f;  // Glow / Particles
  uint32_t peakFft = 2048;                       // Particles
  float afterglowDb = -24.0f, afterglowS = 3.0f;  // Afterglow
  bool operator==(const CrossSpectrumConfig&) const = default;
};

struct StereoPoint {
  float hz = 0.0f, db = -140.0f, pos = 0.0f, width = 0.0f, coherence = 0.0f, monoDb = 0.0f, phaseDeg = 0.0f;
  float glowPos = 0.0f, glowWidth = 0.0f, glowLevel = 0.0f;  // Afterglow
};

struct StereoParticle {
  static constexpr uint32_t kTrail = 24;
  struct Trail {
    float logF = 0.0f, pan = 0.0f, energy = 0.0f;
  };
  float logF = 0.0f, pan = 0.0f, energy = 0.0f;  // log2(Hz), −1 … +1, 0 … 1
  bool alive = false;
  std::array<Trail, kTrail> trail{};
  uint32_t trailHead = 0, trailCount = 0;
};

class CrossSpectrum {
public:
  static constexpr uint32_t kHeatCols = 128, kMaxParticles = 192;
  explicit CrossSpectrum(const CrossSpectrumConfig& c = {});
  ~CrossSpectrum();
  // A new size, window, axis, point count or rate restarts the analysis; the rest applies from the next frame.
  void configure(const CrossSpectrumConfig& c);
  const CrossSpectrumConfig& config() const noexcept { return cfg_; }

  // Returns the frames analysed.
  uint32_t push(const float* l, const float* r, uint32_t n) noexcept;  // RT-safe (no allocation)
  uint64_t frames() const noexcept { return frames_; }
  std::span<const StereoPoint> points() const noexcept { return pts_; }
  std::span<const float> heat() const noexcept { return heat_; }  // points × kHeatCols, row 0 = fMin (Glow / Particles)
  std::span<const StereoParticle> particles() const noexcept { return parts_; }
  // Nothing drawn moves any more: the input silent for a window, every level under the floor, the grid, particles and
  // afterglow gone.
  bool atRest() const noexcept;

private:
  void frame() noexcept;
  void track(float hopSec) noexcept;
  void window(uint32_t n, std::vector<float>& w, float& sum, float& enbw) const;

  CrossSpectrumConfig cfg_;
  FreqAxis axis_;
  std::unique_ptr<RealFft> fft_, peakFft_;
  std::vector<float> win_, peakWin_;
  float winSum_ = 1.0f, enbw_ = 1.0f, peakWinSum_ = 1.0f, peakEnbw_ = 1.0f;
  std::vector<float> hist_[2];  // the newest samples, a power-of-two ring
  uint32_t mask_ = 0, at_ = 0, sinceHop_ = 0;
  std::vector<float> in_, re_[2], im_[2], pre_[2], pim_[2], power_;  // scratch
  std::vector<uint32_t> b0_, b1_;                                   // each point's bins
  struct Accum {
    double sL = 0.0, sR = 0.0, xRe = 0.0, xIm = 0.0;
  };
  std::vector<Accum> acc_;
  std::vector<StereoPoint> pts_;
  std::vector<float> heat_;
  std::vector<StereoParticle> parts_;
  struct Peak {
    float logF, pan, energy;
  };
  std::vector<Peak> peaks_;
  struct Match {
    uint32_t part, peak;
    float cost;
  };
  std::vector<Match> matches_;
  uint64_t frames_ = 0, quiet_ = 0;
};

class StereoFieldState {
public:
  static constexpr uint32_t kBands = 3, kRing = 1u << 16;
  void configure(double sampleRate, float corrWindowMs, bool bands, float xoverLow, float xoverHigh);
  void reset() noexcept;
  void push(const float* l, const float* r, uint32_t n) noexcept;  // RT-safe
  float correlation() const noexcept { return corr_; }
  float radius() const noexcept { return peak_; }  // the recent trace radius (auto gain: 0.85 / radius)
  bool bands() const noexcept { return bands_; }
  float bandCorrelation(uint32_t b) const noexcept { return band_[b].corr; }
  float bandRadius(uint32_t b) const noexcept { return band_[b].peak; }
  uint64_t bandWritten() const noexcept { return written_; }
  // Band b's samples [from, from + n) ∩ the ring into l, r (zeros where not held).
  void bandRead(uint32_t b, uint64_t from, uint32_t n, float* l, float* r) const noexcept;

private:
  struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    float run(float x) noexcept {
      const float y = b0 * x + z1;
      z1 = b1 * x - a1 * y + z2;
      z2 = b2 * x - a2 * y;
      return y;
    }
  };
  struct Band {
    Biquad hp[2][2], lp[2][2];  // [channel][stage]: LR4 = two Butterworth stages
    bool useHp = false, useLp = false;
    double sLL = 0.0, sRR = 0.0, sLR = 0.0;
    float corr = 0.0f, peak = 0.0f;
    std::vector<float> ring[2];
  };
  static void corrOf(double sLL, double sRR, double sLR, float& corr) noexcept;
  double sr_ = 48000.0;
  float aCorr_ = 0.0f, aPeak_ = 0.0f;
  double sLL_ = 0.0, sRR_ = 0.0, sLR_ = 0.0;
  float corr_ = 0.0f, peak_ = 0.0f;
  bool bands_ = false;
  std::array<Band, kBands> band_{};
  uint64_t written_ = 0;
};

}  // namespace pq::dsp
