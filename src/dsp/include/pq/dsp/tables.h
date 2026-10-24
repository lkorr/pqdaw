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

// ---- Wavetable: band-limited mipmaps, half an octave apart ---------------------------------------------------
// Level j (0 ≤ j < levels = 2·log2(N) − 1) holds the cycle with every harmonic h > H_j = floor((N/2)·2^(−j/2))
// removed (N = table size; level 0 keeps harmonics 1 … N/2 − 1, the Nyquist bin is always dropped; the last level
// keeps only the fundamental). Levels are built with an FFT in double precision. Each level stores N + 1 samples
// (the last repeats sample 0, so linear interpolation needs no wrap).
//
// Level choice. inc = estimated |phase increment| in cycles per sample (see below), L = log2(N·inc). Level j's top
// harmonic sits at 2^(L − 1 − j/2) cycles per sample; it is "right" for the frequency when that top lies in
// [1/3, 2/3]: everything up to sr/3 (16 kHz at 48 kHz) is present, and partials above Nyquist fold back no lower
// than sr/3. That holds for L in [j/2 − 0.585, j/2 + 0.415], a one-octave window, and adjacent levels' windows
// overlap by half an octave, which is where the kernel CROSSFADES between them (M1 review M12: with one level per
// octave and a hard switch, a saw lost every harmonic above sr/4 just past each octave boundary). With
// u = 2·L estimated from the float bits of N·inc (log2(m) ≈ m − 1, at most 0.086 octave low):
//   k = floor(u),  t = min(2·(u − k), 1),  out = lerp(read(level k), read(level k + 1), t)   (levels clamped)
// so level j fades in over u ∈ [j − 1, j − 0.5), is alone up to u = j, and fades out over [j, j + 0.5): it is used
// for true L in [j/2 − 0.5, j/2 + 0.336], inside its window with ≥ 0.079 octave to spare on both sides. Every
// harmonic up to sr/3 is therefore always at full level (in both crossfaded levels), and no alias lands below sr/3.
// Cost: 4 table reads per sample instead of 2 (two levels × linear interpolation, fetched as 64-bit pairs); the
// mono kernel skips the lower level when no lane needs it (t = 1, half of every half-octave). Measured 0.81 →
// 1.72 ns/sample mono in a crossfade, 1.24 at a steady single-level pitch; 4.8 → 10.3 ns per 8-voice frame.
//
// Increment estimate: the smaller of the last two |Δphase| (each wrapped to [0, 0.5]; NaN → 0). One phase jump
// (a phasor reset upstream, the first sample after the state is initialised) then never selects the level for
// the jump: the first sample of a fresh state reads level 0, and a jump costs nothing.
//
// Levels are read with LINEAR interpolation: level j ≥ 2 is at least 2× oversampled relative to its bandwidth,
// and levels 0 and 1 are only used below about 42 Hz (N = 2048 at 48 kHz), where the interpolation error is far
// below the noise floor for typical drawn shapes. Cubic would cost 4 extra gathers per sample for no audible gain.
struct WavetableView {
  const float* data = nullptr;  // levels × stride floats
  uint32_t size = 0;            // N, power of two
  uint32_t levels = 0;          // 2·log2(N) − 1: level levels−1 holds only the fundamental
  uint32_t stride = 0;          // N + 1
  float sizeF = 0;              // float(N)
};

// Highest harmonic kept by level j of an N-point table (see above): floor((N/2)·2^(−j/2)), at most N/2 − 1.
uint32_t wavetableLevelTopHarmonic(uint32_t n, uint32_t level) noexcept;

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
  // Level j as N + 1 samples (for tests and the editor's display).
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
