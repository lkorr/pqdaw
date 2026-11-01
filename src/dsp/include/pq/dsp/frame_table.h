#pragma once

// Frame tables (TMM.1): multi-frame band-limited wavetables for the Wavetable meta module (docs/plans/MM.md,
// Decisions → DSP). Built off the audio thread by FrameTableBuilder (frame_table_build.h); read by the RT readers
// below, which TMM.2's `wavetable_pos` atom wraps.
//
// ---- Contents ---------------------------------------------------------------------------------------------------
// F dense frames (1 … 256; 64 by default), each one period of a wave with harmonics 1 … 1023 (+ DC), stored as L mip
// levels. Level j keeps harmonics 1 … H_j (H_0 = 1023, decreasing) and is stored 4× oversampled: len_j = 8·m_j
// samples with m_j the smallest 7-smooth integer ≥ H_j (so the builder's mixed-radix FFT synthesises it exactly;
// at most 10 % over 8·H_j, 4 % on average). Each harmonic is pre-divided by the interpolator's response at it, so the interpolated
// continuous wave has exactly the frame's harmonics (passband error 0; the interpolator droops ≤ 1 % at 1/8).
//
// ---- The "sr − 20 kHz" mip rule (no level crossfade) --------------------------------------------------------------
// With P = the protected band (20 kHz; scaled by sr / 44.1 kHz below 44.1 kHz) and A = (sr − P) / sr, a level may be
// used at increment inc (cycles per sample) only if H_j·inc ≤ A: every harmonic above Nyquist then folds to ≥ P.
// Levels are spaced by r = min(2, (sr − P) / P) in increment (≈ 1.205 at 44.1 kHz, 1.4 at 48 kHz, 2 at ≥ 88.2 kHz):
//   level j serves inc ∈ (b_{j−1}, b_j],  b_j = b_0·r^j,  b_0 = A / (1023·(1 + ε)),  H_j = ⌊A / (b_j·(1 + ε))⌋,
// with ε = 1e-3 covering the error of the increment estimate (float phase differences). The level is looked up, not
// computed: the float bits of inc above the low 16 (exponent + 7 mantissa bits: 128 buckets per octave, 2^−24 … 1)
// index a table of packed level infos, each bucket holding the level of its TOP increment (alias-safe for the whole
// bucket). At the bottom of its range a level then still holds every harmonic up to
// (sr − P) / (r·(1 + ε)·(1 + 1/128)) ≥ 0.991·P (19.83 kHz at P = 20 kHz; a bucket is ≤ 1/128 wide), so one level
// per lane per sample suffices;
// there is no crossfade between levels. The last level (H = 1) serves everything above; increments below 2^−24 (and 0,
// NaN) read level 0. Levels with equal H share storage. frameTableLevel() is that lookup.
//
// ---- Reading ----------------------------------------------------------------------------------------------------
// Per lane and sample: inc = the smaller of the last two |Δphase| (each wrapped to [0, ½]; NaN → 0), as the
// `wavetable` atom does, so a single phase jump (a reset) never selects a level; the first sample of a fresh state
// reads level 0. Frame position u ∈ [0, 1] (clamped; NaN → 0) → k = ⌊u·(F − 1)⌋ (≤ F − 2), weight w; the two frames'
// samples are blended linearly (skipped when no lane has w ≠ 0, and always when F = 1), then interpolated once with a
// 6-point, 5th-order piecewise polynomial designed for 4×-oversampled data (tools/fit/frame_interp_coeffs.py: every
// image ≥ 130.8 dB below the passband). The phase is frac(phase) (any float; a phasor's [0, 1) as is); the position
// in the level, frac·len, is split into index and fraction with an exact product (two-product), so no precision is
// lost beyond the float phase itself.
//
// ---- Memory -----------------------------------------------------------------------------------------------------
// Floats per frame: 28 864 at 48 kHz (64 frames: 7.05 MiB), 48 704 at 44.1 kHz (11.9 MiB: the closer level spacing
// there needs 38 levels), 16 408 at ≥ 88.2 kHz (4.0 MiB). Rows carry 4 guard samples on each side (the wrapped
// neighbours), so reads never wrap.
//
// ---- Cost (bench/RESULTS.md, ship) ------------------------------------------------------------------------------
// Per step: one 8-lane gather (the level), 16 four-float loads, 12 shuffles, the polynomial; a blend doubles the
// loads and shuffles. The frame choice is made once per block when the position is a Spl pattern.

#include <cstdint>
#include <span>
#include <vector>

#include "pq/core/aligned_buffer.h"
#include "pq/core/result.h"

namespace pq::dsp {

inline constexpr uint32_t kFrameTableMaxHarmonic = 1023;
inline constexpr uint32_t kFrameTableMaxFrames = 256;
inline constexpr uint32_t kFrameTableGuard = 4;     // samples before and after each row
inline constexpr double kFrameTableLevelEps = 1e-3;

// The level structure for one sample rate (off-thread; shared by every table built for that rate).
struct FrameTableLayout {
  double sampleRate = 0;
  double protectedHz = 0;      // P
  double ratio = 0;            // r
  double b0 = 0;               // top increment of level 0
  uint32_t levels = 0;         // L
  uint32_t frameStride = 0;    // floats per frame (all levels, guards included; a multiple of 8)
  std::vector<uint32_t> top;     // H_j
  std::vector<uint32_t> length;  // len_j
  std::vector<uint32_t> offset;  // first sample of level j's row within a frame (after its front guard)
  std::vector<uint8_t> bucketLevel;  // kFrameLevelBuckets: the level of each increment bucket
};

// The increment buckets of the level lookup: 128 per octave from 2^−24 up to 1.
inline constexpr uint32_t kFrameLevelBucketsPerOctave = 128;
inline constexpr uint32_t kFrameLevelBuckets = 24 * kFrameLevelBucketsPerOctave;
inline constexpr int32_t kFrameLevelBucketBase = (127 - 24) * 128;  // (float bits of 2^−24) >> 16

// sampleRate in [8 kHz, 768 kHz]. Not RT-safe.
Result<FrameTableLayout> frameTableLayout(double sampleRate);

// What the readers see (POD). Pass `table.view()`; valid while the FrameTable lives.
struct FrameTableView {
  const float* data = nullptr;          // frames × frameStride floats (+ 8 readable floats past the end)
  const uint32_t* levelInfo = nullptr;  // per level: offset (bits 0–16) | len << 17
  const uint32_t* bucketInfo = nullptr; // per increment bucket: its level's packed info
  const uint8_t* bucketLevel = nullptr; // per increment bucket: its level
  uint32_t frames = 0;                  // F ≥ 1
  uint32_t levels = 0;                  // L ≥ 1
  uint32_t frameStride = 0;
  float framesMinus1 = 0;               // float(F − 1)
  float sampleRate = 0;                 // the layout's (TMM.8: a kernel inside an oversample region reads its factor)
  // TMM.8, the live path's level crossfade (wave_live.h): per increment bucket the packed info of the next richer
  // level (level j − 1; level 0: itself), the effective top e_j of the bucket's level (the top of the last bucket
  // holding level j) and 1 / (e_j − e_{j−1}) (0 for level 0).
  const uint32_t* bucketRich = nullptr;
  const uint32_t* bucketLevelIndex = nullptr;  // the bucket's level j (bucketLevel as 32-bit words, for gathers)
  const float* bucketTop = nullptr;
  const float* bucketInvSpan = nullptr;
  // TMM.8, Shape mode (wave_live.h "wavetable_shape"): present only in a table built with
  // FrameTableSpec::antiderivative. adData: the periodic part P of each frame's antiderivative, stored like `data`
  // (the same rows, guards and pre-emphasis; harmonics c_h / (2πih), DC 0); frameDc: per frame its DC c_0 (so
  // G(u) = c_0·u + P(u)); levelEdge: per frame and level (frame·levels + level) the level's value at phase 0, as the
  // reader interpolates it (the clamp's constant, T(0) = T(1)). Null otherwise.
  const float* adData = nullptr;
  const float* frameDc = nullptr;
  const float* levelEdge = nullptr;
};

class FrameTable {
public:
  FrameTable() = default;
  // Zeroed storage for `frames` frames at the layout's rate (frames in [1, 256]); with `antiderivative` also the
  // Shape-mode arrays (TMM.8: twice the row memory). Not RT-safe.
  static Result<FrameTable> allocate(const FrameTableLayout& layout, uint32_t frames, bool antiderivative = false);

  const FrameTableView& view() const noexcept { return view_; }  // RT-safe
  const FrameTableLayout& layout() const noexcept { return layout_; }
  uint32_t frames() const noexcept { return view_.frames; }
  bool hasAntiderivative() const noexcept { return view_.adData != nullptr; }
  std::size_t bytes() const noexcept {
    return (data_.size() + ad_.size() + extra_.size()) * sizeof(float) + info_.size() * sizeof(uint32_t) +
           layout_.bucketLevel.size();
  }
  // Level j of frame k: len_j samples (the guards sit at [−4, 0) and [len, len + 4)).
  std::span<const float> row(uint32_t frame, uint32_t level) const noexcept;
  float* mutableRow(uint32_t frame, uint32_t level) noexcept;  // for the builder
  // TMM.8 (antiderivative tables only; null / empty otherwise): the P row, the DC, the edge values (builder).
  std::span<const float> adRow(uint32_t frame, uint32_t level) const noexcept;
  float* mutableAdRow(uint32_t frame, uint32_t level) noexcept;
  float* mutableFrameDc() noexcept { return view_.adData ? extra_.data() + dcOffset_ : nullptr; }
  float* mutableLevelEdge() noexcept { return view_.adData ? extra_.data() + edgeOffset_ : nullptr; }

private:
  AlignedBuffer<float> data_;
  AlignedBuffer<float> ad_;     // TMM.8: antiderivative rows (Shape mode)
  AlignedBuffer<float> extra_;  // TMM.8: bucketTop, bucketInvSpan, then frameDc, levelEdge (Shape mode)
  std::size_t dcOffset_ = 0, edgeOffset_ = 0;
  AlignedBuffer<uint32_t> info_;
  FrameTableLayout layout_;
  FrameTableView view_{};
};

// The interpolator's frequency response at f cycles per sample (the continuous transform of its impulse response;
// H(0) = 1). The builder divides harmonic h of level j by H(h / len_j). Not RT-safe only by cost (a short series).
double frameInterpResponse(double f) noexcept;

// ---- RT readers (TMM.2's kernel wraps these) -----------------------------------------------------------------
// Kernel use: KernelArgs::resource → const FrameTableView*, the atom's state holds a FrameReadState, in[0] = phase,
// in[1] = position (positionPerSample = !splBit(mask, 1)), out[0]. A crossfade between an old and a new table runs
// both readers on copies of the same state (the state depends only on the phase), so the crossfade costs two reads.
// Per-lane state: the previous phase and the previous |Δphase| (8 lanes; L1 uses lane 0). Zero = fresh.
struct alignas(32) FrameReadState {
  float prevPhase[8];
  float prevDelta[8];
};

// The level the readers choose for increment `inc` (cycles per sample, ≥ 0 or NaN). RT-safe.
uint32_t frameTableLevel(const FrameTableView& t, float inc) noexcept;

// L1: `frames` samples of one signal. phase[n]; position[n] when positionPerSample, else position[0] for the
// whole block (a Spl pattern). Vectorised over time (8 samples per AVX2 step; any `frames`). RT-safe.
void readFrameTableL1(const FrameTableView& t, FrameReadState& st, const float* phase, const float* position,
                      bool positionPerSample, uint32_t frames, float* out) noexcept;
// L8: one voice group, buffers frame-major with the 8 voices interleaved (phase[n·8 + v]); position is frames·8
// floats when positionPerSample, else 8 per-voice floats for the block. RT-safe.
void readFrameTableL8(const FrameTableView& t, FrameReadState& st, const float* phase, const float* position,
                      bool positionPerSample, uint32_t frames, float* out) noexcept;

// ---- Double reference (tests) ----------------------------------------------------------------------------------
// One read at a given level, in double: frac(phase)·len exactly, the same 6-point polynomial (its float coefficients,
// widened), the blend in double. The frame pair and weight are the readers' (from float(position), a discrete
// choice like the level). RT-safe.
double readFrameTableReference(const FrameTableView& t, double phase, double position, uint32_t level) noexcept;
// A block as the readers do it (lanes 1 or 8, same buffer layouts), with the level chosen exactly as they choose it
// (the same float increment estimate and frameTableLevel) and every read in double. RT-safe.
void readFrameTableReferenceBlock(const FrameTableView& t, FrameReadState& st, const float* phase,
                                  const float* position, bool positionPerSample, uint32_t lanes, uint32_t frames,
                                  double* out) noexcept;

}  // namespace pq::dsp
