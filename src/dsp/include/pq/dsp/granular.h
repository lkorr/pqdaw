#pragma once

// The Sampler's granulator, the `granular` atom (SM.6; docs/plans/SM.md "Semantics" Granular, "SM.6"): up to
// kGrainsMax grains per voice lane over the lane's SampleSlot entry, rendered 8 grains per AVX2 op.
//
// ---- One voice (a lane) ----------------------------------------------------------------------------------------------
// From a trigger (a high reset) on, the voice's position P moves through [start, end) by `scan` × the file's own speed
// per output sample (wrapping; Freeze holds it), and grains spawn at its sample times t_k. GV.1: P is Position's point
// (read every run: moving Position while a note plays scrubs) plus the distance scanned since the trigger:
//   Classic   t_{k+1} = t_k + sr / hz(t_k) (pitch-synchronous), read at the transpose ratio (the source's own pitch ×
//             ratio: the grain size is the formant)
//   Loop      t_{k+1} = t_k + sr / density, read at hz / root × ratio
//   Cloud     as Loop with Poisson onsets: t_{k+1} = t_k − ln(1 − u) · sr / density
// Grain k starts reading at P(t_k) + u·sprayTime (± seconds of the source), at its rate × 2^(u·sprayPitch / 12) (±
// semitones) × (Harmonic grains) the next ratio n / d of the entry's list (cyclic; default 1, 2, 3/2, 3, 4, 5: n and
// d stay integers until the rate's one double product), panned u·sprayPan (equal power, centre gain 1), its window
// the shape's 1024-point table (linear between points) over size seconds. Every u is a counter hash of (the node's
// seed, the lane's voice slot, the grain number since the trigger, the draw): renders repeat exactly.
//   A grain's rate is fixed at its spawn (a glide moves the grains that start after it); its mip level from that rate.
// The lane's grain slots are taken on a grid of kGrainGrid samples of the voice's own clock: a spawn in the grid cell
// starting at c takes a slot whose grain ended at or before c (none free: the spawn is skipped, counted), so which
// grain plays where never depends on the block size.
//   Output: the sum of the live grains (stereo; a mono sample on both); playing: 1 from the first trigger; pos: P in
// [0, 1] of the sample. A retrigger keeps the live grains (they play out) unless the entry changed (they stop).
// ---- Rendering -------------------------------------------------------------------------------------------------------
// Per lane and grid cell: scalar spawning, then the live grains of each mip level in batches of 8 (a compacted list),
// each frame one cubic read per grain and channel (AVX2 gathers), the window by two gathers, positions 32.32 in two
// 32-bit words (carry by an unsigned compare); the batch's sum per frame added to the lane's output. Cost ∝ live
// grains; a lane with no voice and no grain costs a compare.
//   GrainTap (pq/core/grain_tap.h): with a reader, each call writes one record per voice group of its live grains.
// RT-safe: everything noexcept, no allocation.

#include <cstdint>

namespace pq::dsp {

inline constexpr uint32_t kGrainsMax = 32;          // grain slots per lane
inline constexpr uint32_t kGrainShapePoints = 1024;  // a shape table's points (+ 1: the end, 0)
inline constexpr uint32_t kGrainGrid = 64;           // the slot grid (samples of the voice's clock)
inline constexpr uint32_t kGrainHarmonicsMax = 64;   // ratios read from the entry's list at most

enum class GrainShape : uint8_t { Hann = 0, Tukey = 1, Gauss = 2, Triangle = 3, Exp = 4 };
enum class GranularMode : uint8_t { Classic = 0, Loop = 1, Cloud = 2 };

// The shape's table: kGrainShapePoints + 1 floats, w(i / kGrainShapePoints), 0 at both ends. RT-safe (built at load).
const float* grainShapeTable(GrainShape s) noexcept;
// A counter hash → [0, 1) (24 bits). RT-safe.
inline float grainUniform(uint32_t seed, uint32_t voice, uint32_t grain, uint32_t draw) noexcept {
  uint32_t h = seed * 0x9E3779B1u ^ (voice + 0x7F4A7C15u) * 0x85EBCA77u;
  h ^= grain * 0xC2B2AE3Du + draw * 0x27D4EB2Fu;
  h ^= h >> 16;
  h *= 0x7FEB352Du;
  h ^= h >> 15;
  h *= 0x846CA68Bu;
  h ^= h >> 16;
  return static_cast<float>(h >> 8) * 0x1p-24f;
}

// A lane's voice (the words before the grain arrays: atom_state.h GranularState).
struct GranVoice {
  int64_t t = 0;        // samples since the trigger (of the next frame)
  int64_t pos = 0;      // GV.1: the distance scanned since the trigger (32.32 level-0 frames, within one region length)
  double next = 0.0;    // the next spawn time (samples since the trigger)
  uint32_t count = 0;   // grains spawned since the trigger
  uint32_t harm = 0;    // the next harmonic ratio
  uint32_t active = 0;  // 1 from the first trigger
  uint32_t gen = 0;     // the entry's generation (low 32 bits)
  uint32_t skipped = 0; // spawns that found no free slot (stats)
  uint32_t pad = 0;
};

}  // namespace pq::dsp
