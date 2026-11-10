#pragma once

// Kernels of the time-stretch warp modes (T6.3; docs/plans/M6.md T6.3; the player is pq/engine/clip_player.h). The
// clip player reads the source through the resampler (pq/dsp/resample.h: sincResample / gainCopy, the per-frame gain
// fused); these kernels make those per-frame gains and choose where grains read:
//   Tones / Texture  granular overlap-add: grains of N output frames every H = N / 2, a periodic Hann window
//                    (hannWindow: w(j) = 0.5 − 0.5·cos(2πj / N); Σ_k w(t − kH) = 1 exactly, so a stationary source
//                    keeps its level). Tones aligns each grain with the one before (warpBestLag: the lag that best
//                    continues the previous grain's waveform, WSOLA), so a monophonic periodic source keeps its pitch
//                    exactly (one lag per grain: a chord's partials drift; Complex is the mode for chords);
//                    Texture leaves grains where the warp law puts them and jitters them (warpJitter, Flux).
//   Beats            slices cut at transients or a grid; each slice's gain is piecewise linear (fade in, Envelope
//                    decay, the 2 ms crossfade into the next slice, a loop's declick): warpRampGain.
// All RT-safe (no allocation) except hannWindow. AVX2.

#include <cstdint>
#include <vector>

namespace pq::dsp {

// out[i] = win[i] · (env ? env[i] : gain), i < n. RT-safe, AVX2.
void warpWindowGain(const float* win, const float* env, float gain, float* out, uint32_t n) noexcept;
// A ramp is anchored on a sample t0: its value at t0 + j is clamp(fma(j, dg, base), 0, 1) (j exact as a float below
// 2^24), so a sample's gain does not depend on where a block starts (renders are bit-identical whatever the block).
// rampValue(base, dg, j); warpRampGain: out[i] = rampValue(base, dg, k + i) · (env ? env[i] : gain), i < n;
// warpRampMul: out[i] *= rampValue(base, dg, k + i). RT-safe, AVX2.
float rampValue(float base, float dg, int32_t j) noexcept;
void warpRampGain(float base, float dg, int32_t k, const float* env, float gain, float* out, uint32_t n) noexcept;
void warpRampMul(float base, float dg, int32_t k, float* out, uint32_t n) noexcept;

// The periodic Hann window of n frames (n even). Not RT-safe (allocates).
std::vector<float> hannWindow(uint32_t n);

// A deterministic value in [−1, 1) for grain k of a clip with `seed` (Texture's Flux; renders repeat exactly). RT-safe.
double warpJitter(uint32_t seed, int64_t k) noexcept;

// out[i] = Σ_c in[c][first + i·stride] (the channels' sum, every `stride`-th frame), i < count. RT-safe.
void warpMonoDecimate(const float* const* in, uint32_t channels, int64_t first, uint32_t stride, uint32_t count,
                      float* out) noexcept;

// The lag l in [0, lags) that maximises the normalised correlation Σ_i ref[i]·cand[l + i] / sqrt(Σ_i cand[l + i]²)
// over i < len (cand holds len + lags − 1 values); ties take the lag closest to `prefer`. A silent candidate scores 0.
// `frac` (optional): the sub-frame offset of the true peak in [−0.5, 0.5] (a parabola through the best lag's score and
// its neighbours'; 0 at the ends). RT-safe, AVX2 (dot products 8 wide, running energy).
uint32_t warpBestLag(const float* ref, const float* cand, uint32_t len, uint32_t lags, uint32_t prefer,
                     double* frac = nullptr) noexcept;

}  // namespace pq::dsp
