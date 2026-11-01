#pragma once

// The frame-table builder (TMM.1; docs/plans/MM.md Decisions → DSP). Off the audio thread: keyframes (2048-sample
// cycles at positions in [0, 1]) → F dense frames (spectral morph or crossfade per gap) → optional baked warp /
// fold / sync (wave_warp.h, at 16× resolution with analytic corrections) → band-limited mip levels (frame_table.h).
// Everything is computed in double. The builder keeps its scratch arenas between builds: once they are sized for
// the largest spec seen (keyframes, frames, sample rate, bake resolution), build() into a preallocated FrameTable
// performs no heap allocation.
//
// ---- Steps ------------------------------------------------------------------------------------------------------
// 1. Keyframes: each cycle's spectrum c_h, h = 0 … 1023 (x(φ) = Σ_h c_h·e^(2πihφ), c_−h = conj c_h; the 2048-point
//    cycle's Nyquist bin is dropped). `normalise` first scales each keyframe's cycle to a peak of 1 (the samples as
//    given; a band-limited cycle may overshoot it slightly, as any band-limited edge does).
// 2. Dense frame d sits at u_d = d / (F − 1) (u = 0 for F = 1). Between keyframes a and b (u_a ≤ u ≤ u_b, t = the
//    position in the gap): Spectral (default) interpolates each harmonic's magnitude linearly and its phase along the
//    shorter arc (a zero magnitude takes the other side's phase; exactly opposite phases turn counter-clockwise);
//    DC is interpolated linearly. Crossfade interpolates the complex coefficients (= the samples) linearly. Before
//    the first and after the last keyframe the frames hold that keyframe.
// 3. Bake (when any of warp / fold / sync is non-zero): each dense frame's cycle is sampled through the shared
//    formulas at M = 2048·R points on a half-sample grid (x read from an M-point upsampled copy through the same
//    6-point interpolator), and its harmonics 0 … 1023 come from an M-point FFT. The composed read jumps or kinks at
//    known phases (wave_warp.h warpBreakpoints); their value, slope and curvature jumps (from 2nd-order jets through
//    the same formulas) are subtracted as periodic Bernoulli polynomials before the FFT and added back exactly after
//    it, so the sampled remainder is C2 and its aliasing is negligible. R = frameBakeResolution(spec): 16 for any
//    single control (≤ −126 dB from a 64× bake over sweeps of every control), raised for combinations.
// 4. `removeDc` zeroes every dense frame's DC (after the bake, which can create DC).
// 5. Levels: harmonic h of level j is c_h / H(h / len_j) (frameInterpResponse) for h ≤ H_j; an inverse FFT of
//    len_j points gives the row; eight frames share one transform (four AVX2 lanes × real and imaginary). With
//    `antiderivative` (TMM.8, Shape mode) the same again for P (c_h / (2πih), no DC), plus each frame's DC and each
//    level's value at phase 0.
// Build time (bench/RESULTS.md): 64 frames ≈ 10 ms, with a single baked control ≈ 65 ms (one thread).

#include <complex>
#include <cstdint>
#include <memory>
#include <span>

#include "pq/core/result.h"
#include "pq/dsp/frame_table.h"
#include "pq/dsp/wave_warp.h"

namespace pq::dsp {

inline constexpr uint32_t kKeyframeCycle = 2048;
inline constexpr uint32_t kMaxKeyframes = 256;

enum class GapMode : uint8_t { Spectral, Crossfade };

struct Keyframe {
  float position = 0;               // [0, 1], non-decreasing along the spec
  std::span<const float> cycle;     // kKeyframeCycle samples (finite)
};

// Static warp / fold / sync baked into the table; 0 = not applied (wave_warp.h for the ranges).
struct FrameBake {
  WarpMode warpMode = WarpMode::Bend;
  float warp = 0;  // [−1, 1]
  float fold = 0;  // [0, 1]
  float sync = 0;  // [0, 1]
  bool active() const noexcept { return warp != 0.0f || fold != 0.0f || sync != 0.0f; }
};

struct FrameTableSpec {
  std::span<const Keyframe> keyframes;  // 1 … 256
  std::span<const GapMode> gaps;        // keyframes − 1 entries, or empty (all Spectral)
  uint32_t denseFrames = 64;            // 1 … 256
  FrameBake bake;
  bool removeDc = false;
  bool normalise = false;
  double sampleRate = 48000;
  uint32_t bakeResolution = 16;         // minimum bake samples per keyframe-cycle sample: a power of two in [4, 128]
  // TMM.8: also build the Shape-mode antiderivative arrays (frame_table.h FrameTableView::adData; the table must be
  // allocated with them). Step 5 writes, per level, the rows of P (harmonics c_h / (2πih), pre-emphasised like the
  // value rows), the frame's DC and the level's value at phase 0.
  bool antiderivative = false;
};

// The bake resolution actually used: spec.bakeResolution, doubled (up to kMaxAutoBakeResolution, never above what
// was asked for when that is higher) while M = 2048·R is below twice the composed read's estimated bandwidth
// (1023 × the read slope × max(1, the fold's θ)). Every single control at any value needs no more than 16×;
// combinations (sync with pulse width or fold) raise it. Beyond the cap, combinations whose bandwidth exceeds M / 2
// keep an in-band error (measured in tests/dsp/test_frame_table.cpp; QUESTIONS Q?-TMM.1-a): the table is still
// band-limited, so playback never aliases; its harmonics deviate from the ideal waveform's.
inline constexpr uint32_t kMaxAutoBakeResolution = 64;
uint32_t frameBakeResolution(const FrameTableSpec& spec) noexcept;

// A 64-bit hash of everything that determines the table (identical specs → identical tables): for sharing.
uint64_t frameTableSpecHash(const FrameTableSpec& spec) noexcept;

class FrameTableBuilder {
public:
  FrameTableBuilder();
  ~FrameTableBuilder();
  FrameTableBuilder(const FrameTableBuilder&) = delete;
  FrameTableBuilder& operator=(const FrameTableBuilder&) = delete;

  // Builds into `out`, which must have been allocated for spec.sampleRate, spec.denseFrames and
  // spec.antiderivative (FrameTable::allocate with frameTableLayout(spec.sampleRate)). Sizes the arenas on first use of a larger spec (allocates then only).
  Status buildInto(const FrameTableSpec& spec, FrameTable& out);
  // Allocates the table (one allocation of its storage) and builds it.
  Result<FrameTable> build(const FrameTableSpec& spec);

  // The last successful build's dense-frame spectra (after bake and removeDc): c_h, h = 0 … 1023.
  std::span<const std::complex<double>> frameSpectrum(uint32_t frame) const noexcept;
  uint32_t frames() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Basic cycles (TMM.1, the Wavetable module's Basic Shapes): n samples, amplitude 1, phase 0 at the start (saw rises
// −1 → 1; square and pulse are +1 first). Noise is uniform white noise from `seed`, mean removed, peak 1. Naive (not
// band-limited): the builder band-limits. Not RT-safe.
enum class BasicCycle : uint8_t { Sine, Triangle, Saw, Square, Pulse25, Noise };
AlignedBuffer<float> basicCycle(BasicCycle shape, uint32_t n = kKeyframeCycle, uint64_t seed = 1);

}  // namespace pq::dsp
