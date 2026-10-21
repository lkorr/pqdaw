#pragma once

// Read-only table resources for the wavetable, shaper and sampleRead atoms (T1.6).
//
// Each resource is an owning class (built OFF the audio thread: it allocates) plus a small POD view that the
// kernels read through KernelArgs::resource. Pass `&table.view()`; the view stays valid while the owner lives.
// Publishing a new table to a running Program is the engine's job (atomic pointer swap + epoch reclamation).

#include <cstdint>
#include <span>

#include "pq/core/aligned_buffer.h"
#include "pq/core/result.h"

namespace pq::dsp {

// ---- Wavetable: band-limited mipmaps ------------------------------------------------------------------------
// Level k holds the cycle with every harmonic h > (N/2) >> k removed (N = table size; level 0 keeps harmonics
// 1 … N/2 − 1, the Nyquist bin is always dropped). Levels are built with an FFT in double precision. Each level
// stores N + 1 samples (the last repeats sample 0, so linear interpolation needs no wrap).
//
// The kernel picks, per sample and per lane, the smallest level whose highest harmonic is at or below Nyquist:
//   k = clamp(ceil(log2(N · inc)), 0, levels − 1),   inc = |phase increment| in cycles per sample,
// estimated from the phase input as |Δphase| wrapped to [0, 0.5]. So every partial stays below sr/2 (for
// inc < 0.5), at the cost of losing up to one octave of top harmonics just below a level switch. Levels are
// read with LINEAR interpolation: level k ≥ 1 is at least 2× oversampled relative to its bandwidth, and at
// level 0 (inc < 1/N, i.e. f < 23 Hz for N = 2048 at 48 kHz) the interpolation error is far below the
// noise floor for typical drawn shapes. Cubic would cost 2 extra gathers per lane for no audible gain.
struct WavetableView {
  const float* data = nullptr;  // levels × stride floats
  uint32_t size = 0;            // N, power of two
  uint32_t levels = 0;          // log2(N): level levels−1 holds only the fundamental
  uint32_t stride = 0;          // N + 1
  float sizeF = 0;              // float(N)
};

enum class BasicShape : uint8_t { Sine, Saw, Square, Triangle };

class Wavetable {
public:
  Wavetable() = default;
  // `cycle` is one period, size N = power of two in [16, 65536]. Not RT-safe (allocates, FFT).
  static Result<Wavetable> build(std::span<const float> cycle);
  // A naive (not band-limited) cycle of a basic shape, size n, amplitude 1, phase 0 at the start of the cycle
  // (saw rises from −1 to 1 across the cycle; square is +1 for the first half). Not RT-safe.
  static AlignedBuffer<float> basicCycle(BasicShape shape, uint32_t n);

  const WavetableView& view() const noexcept { return view_; }  // RT-safe
  // Level k as N + 1 samples (for tests and the editor's display).
  std::span<const float> level(uint32_t k) const noexcept;  // RT-safe

private:
  AlignedBuffer<float> data_;
  WavetableView view_{};
};

// ---- Shaper: a transfer curve read by input value -----------------------------------------------------------
// `points` = P ≥ 2 samples of the curve at x = lo + i·(hi − lo)/(P − 1). Input outside [lo, hi] is clamped
// (NaN reads the lo end). Linear interpolation between points: curves are drawn as dense tables (≥ 1024
// points recommended), where linear interpolation error is below float noise for smooth curves.
struct ShaperView {
  const float* data = nullptr;  // P floats followed by one guard copy of the last point
  float lo = 0;                 // input range start
  float scale = 0;              // (P − 1) / (hi − lo)
  float maxPos = 0;             // float(P − 1)
  uint32_t segments = 0;        // P − 1
};

class ShaperTable {
public:
  ShaperTable() = default;
  static Result<ShaperTable> build(std::span<const float> points, float lo, float hi);  // not RT-safe
  const ShaperView& view() const noexcept { return view_; }  // RT-safe

private:
  AlignedBuffer<float> data_;
  ShaperView view_{};
};

// ---- Sample data for sampleRead -----------------------------------------------------------------------------
// One mono channel held in memory (disk streaming is a later task). `kSampleGuard` zero samples are stored
// before and after the data so the 4-tap interpolator never reads out of bounds.
inline constexpr int32_t kSampleGuard = 4;

struct SampleView {
  const float* data = nullptr;  // points at sample 0; data[-kSampleGuard .. length + kSampleGuard) is readable
  int32_t length = 0;           // samples
};

class SampleData {
public:
  SampleData() = default;
  // At most 2^31 − 2·kSampleGuard − 1 samples (int32 playhead; ~12 h at 48 kHz). Not RT-safe.
  static Result<SampleData> build(std::span<const float> samples);
  const SampleView& view() const noexcept { return view_; }  // RT-safe

private:
  AlignedBuffer<float> data_;
  SampleView view_{};
};

// ---- FFT (off-thread helper for table building) --------------------------------------------------------------
// In-place iterative radix-2 complex FFT in double. n = re.size() = im.size() must be a power of two.
// inverse = true computes the unscaled inverse (caller divides by n). Not RT-safe only because callers pass
// heap buffers; the function itself does not allocate.
void fftInPlace(std::span<double> re, std::span<double> im, bool inverse) noexcept;

}  // namespace pq::dsp
