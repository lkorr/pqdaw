#pragma once

// Polyphase windowed-sinc resampling (T5.4; SPEC §9.4 Re-Pitch; docs/plans/M5.md T5.4). The clip player (engine) reads
// a sample at any rate through it; the browser preview (T5.10) and Simpler (T5.14) will too.
//
// ---- The prototype ------------------------------------------------------------------------------------------------
// h(d) = 2·fc·sinc(2·fc·d) · kaiser(d / W, β) on |d| < W input frames (sinc(x) = sin(πx) / (πx)), tabulated at 1/256
// of a frame: F[u] = h(W − u / 256), u = 0 … 512·W. Two qualities:
//   High   W = 32 (64 taps), fc = 0.4565, β = 8.6: passband flat to 0.42·fs (±0.001 dB to 20 kHz at 48 kHz), stopband
//          from 0.5·fs below −85 dB, so neither images (reading slower than 1) nor aliases (faster) reach the band
//          below Nyquist above −85 dB (tests/dsp/test_resample.cpp measures the table and real reads).
//   Draft  W = 8 (16 taps), fc = 0.42, β = 6: ≈ −60 dB, for scrubbing in the browser (T5.10) only.
//
// ---- Reading at a rate ---------------------------------------------------------------------------------------------
// The read position is int64 32.32 fixed point (SPEC §3.1's fixed-point rule; frames ≥ 0 in the integer part, relative
// to whatever base the caller's pointers have), advanced by a 32.32 step per output frame (negative: reading backwards;
// the kernel is symmetric, so reverse reads are the same code). Output frame i is
//     y_i = Σ_j x[j] · (1 / r_q) · h((p_i − j) / r_q),   p_i = pos + i · step,
// with r_q = 256 / q the STRETCH of the prototype: q = 256 (no stretch) for |rate| ≤ 1, where the prototype's cutoff
// already sits at the source's Nyquist; for |rate| > 1 (pitch up, or a file above the device rate) the cutoff must
// scale by 1 / rate, and q = floor(256 / |rate|) (≥ kMinStretchQ = 16, so r_q ≥ |rate| up to 16×; above that the read
// aliases, measured). Quantising the stretch to 256 / q is what keeps the cost of a stretched read that of a plain
// polyphase filter: tap k of output i then sits at table position v_i + k·q with ONE fractional part v_i for all taps,
// so a table per q (q + 1 rows of K_q = 512·W / q taps, linear interpolation between adjacent rows) holds every
// coefficient contiguously. The cutoff then sits at most 1/q below 1 / |rate| (≤ 0.4 % at a few semitones). Cost is
// proportional to the taps: 64 at |rate| ≤ 1, ≈ 64·|rate| above (bench/RESULTS.md T5.4). Every row of every table is
// normalised to a DC gain of exactly 1, so the gain does not depend on the phase.
// Tables are built by prepareSincTable, off the audio thread (the clip player prepares q = 256 when it is created, and
// its image build every q its clips can need), and read lock-free by sincTable() afterwards; a table that was not
// prepared reads as null, and sincTableAtLeast falls back to the nearest prepared one with less stretch (q = 256 at
// worst: aliasing, never a crash or an allocation). Prepared tables live until the process ends.
//
// ---- Straight copies ------------------------------------------------------------------------------------------------
// A read at exactly ±1 frame per frame from an integer position needs no filter: gainCopy is a plain (reversed) copy
// with the envelope applied, the common case of a clip at the device rate without transpose (≤ 1 ns per stereo frame).

#include <cstdint>

namespace pq::dsp {

enum class ResampleQuality : uint8_t { High, Draft };

inline constexpr uint32_t kSincPhases = 256;    // table rows per input frame of the unstretched prototype
inline constexpr uint32_t kMinStretchQ = 16;    // the largest stretch is 256 / 16 = 16× (+48 semitones)
inline constexpr int64_t kFixedOne = int64_t{1} << 32;  // one frame in 32.32

struct SincPrototype {
  uint32_t halfWidth = 32;  // W: taps = 2·W
  double cutoff = 0.4565;   // fc, in cycles per input frame
  double beta = 8.6;        // Kaiser β
};
SincPrototype sincPrototype(ResampleQuality quality) noexcept;  // RT-safe

// One stretch of one quality's prototype.
struct SincTable {
  uint32_t q = 0;          // 256 / stretch, in [kMinStretchQ, 256]
  uint32_t taps = 0;       // K: coefficients per output frame (a multiple of 8; the tail is zero)
  uint32_t center = 0;     // M0 = floor(256·W / q): output position p reads frames [floor(p) − M0, floor(p) − M0 + K + 1)
  uint32_t span = 0;       // 256·W (the prototype's half width in table steps)
  const float* rows = nullptr;  // q + 1 rows of 2·K floats (32-byte aligned): K coefficients, then K deltas to the next row
};

// The q a read at `rate` source frames per output frame uses (256 for |rate| ≤ 1). RT-safe.
uint32_t sincStretchQ(double rate) noexcept;
// Builds the table once (thread-safe; not RT-safe: it allocates) and returns it. q is clamped to [kMinStretchQ, 256].
const SincTable& prepareSincTable(ResampleQuality quality, uint32_t q);
// The table if it was prepared, else null. RT-safe (one acquire load).
const SincTable* sincTable(ResampleQuality quality, uint32_t q) noexcept;
// The prepared table closest to q from above (less stretch). The quality's q = 256 table must have been prepared.
// RT-safe.
const SincTable& sincTableAtLeast(ResampleQuality quality, uint32_t q) noexcept;

// The input frames a read of `frames` outputs from `pos` by `step` touches, relative to the pointers' base:
// [first, last). RT-safe.
struct SincReach {
  int64_t first = 0;
  int64_t last = 0;
};
SincReach sincReach(const SincTable& t, int64_t pos, int64_t step, uint32_t frames) noexcept;

// out[c][i] += y_c(pos + i·step) · (env ? env[i] : gain) for i < frames. `in` has `channels` (1 or 2) pointers, valid
// over sincReach(...); out has two channels (a mono input is added to both). RT-safe, AVX2.
void sincResample(const SincTable& t, const float* const* in, uint32_t channels, int64_t pos, int64_t step,
                  uint32_t frames, const float* env, float gain, float* const* out) noexcept;

// out[c][i] += in[c][first + i·dir] · (env ? env[i] : gain), dir = ±1. Same channel rules. RT-safe, AVX2.
void gainCopy(const float* const* in, uint32_t channels, int64_t first, int32_t dir, uint32_t frames,
              const float* env, float gain, float* const* out) noexcept;

// The prototype itself at 1/256-frame resolution (F above, 512·W + 1 values), for tests.
const float* sincPrototypeTable(ResampleQuality quality) noexcept;

}  // namespace pq::dsp
