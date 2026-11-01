#pragma once

// What the Wavetable face shows of its instance's table (TMM.7; private to views). The engine's table is built off the
// audio thread by the link (pq/meta/table_builder.h, ≈ 10–65 ms); the face cannot wait for it and has no business with
// mip levels, so it derives its own DISPLAY frames from the same state, with the same rules, at display resolution:
//
//   keyframes  each keyframe's spectrum c_h, h = 0 … 1023 (one 2048-point FFT in double), cached by the cycle's hash so
//              moving a keyframe re-transforms nothing; `normalise` scales a cycle to a peak of 1 first (as the builder)
//   dense      frame d at u_d = d / (F − 1) (F = the data's frames; 1 with one keyframe, as TMM.5 builds it): the gap's
//              rule per harmonic (Spectral: magnitude linear, phase along the shorter arc, a zero magnitude taking the
//              other side's phase; Crossfade: the complex values linear), the first kDisplayHarmonics harmonics with
//              Lanczos σ factors (no Gibbs ringing in the picture), one kDisplayCycle-point inverse FFT
//   bake       fold / warp / sync through the shared formulas (dsp/wave_warp.h), each displayed frame sampled at
//              kPoints phases (the table's own read: φ → sync → warp → x → fold), then `removeDc`
//   blend      the frame at a position: the two nearest dense frames linearly (the reader's frame blend)
//
// Costs (measured in tests/views/test_meta_wavetable_face.cpp): a data change (64 frames, two keyframes) ≈ 1 ms, a
// baked knob step ≈ 0.2 ms; nothing per paint. UI thread only.

#include <array>
#include <complex>
#include <cstdint>
#include <span>
#include <vector>

#include "pq/meta/wavetable.h"

namespace pq::views::wt {

inline constexpr uint32_t kHarmonics = 1024;        // c_0 … c_1023 of a 2048-sample cycle (the builder's)
inline constexpr uint32_t kDisplayHarmonics = 192;  // drawn harmonics (σ-smoothed)
inline constexpr uint32_t kDisplayCycle = 512;      // the smoothed cycle's points (read by the displayed bake)
inline constexpr uint32_t kPoints = 256;            // phases per displayed frame (the frame holds kPoints + 1: closed)
inline constexpr uint32_t kBars = 32;               // harmonic bars shown (TMM.9 scrolls and zooms them)

struct Bake {
  float fold = 0.0f, warp = 0.0f, sync = 0.0f;
  uint32_t warpMode = 0;  // dsp::WarpMode
  friend bool operator==(const Bake&, const Bake&) = default;
};

class Model {
public:
  // Follows the instance's data (compared by pointer: a target returns the same pointer while it is unchanged) and
  // baked values. True when what is displayed changed.
  bool update(const meta::DataPtr& data, const Bake& bake);

  bool ok() const noexcept { return ok_; }
  const meta::WavetableData& data() const noexcept { return data_; }
  const meta::DataPtr& dataPtr() const noexcept { return ptr_; }
  uint32_t frames() const noexcept { return frames_; }  // displayed dense frames (≥ 1)
  // Displayed frame f (after the bake): kPoints + 1 values (the last repeats the first).
  std::span<const float> frame(uint32_t f) const noexcept;
  // The display gain: 1 / max(peak, 1) (a table louder than full scale is drawn to fit, a quiet one as it is).
  float gain() const noexcept { return gain_; }
  // The displayed frame at position u (the reader's blend of the two nearest dense frames) into out (kPoints + 1).
  void blended(float u, std::span<float> out) const noexcept;
  // One value of the blended frame at position u, phase φ ∈ [0, 1).
  float valueAt(float u, float phase) const noexcept;

  // Keyframe k's harmonics 1 … kBars: amplitude relative to its largest harmonic (1 … 1023), and phase in (−π, π]
  // relative to a sine (a sine's harmonic sits at 0).
  void harmonics(uint32_t k, std::span<float> amp, std::span<float> phase) const noexcept;
  // The source cycle at position u (what the dense frames there blend to, before the bake), 2048 samples, full
  // resolution: a keyframe added "from the blended frame". One keyframe: its cycle.
  std::vector<float> sourceCycle(float u) const;
  // Which basic shape keyframe k's cycle is exactly (dsp::basicCycle, seed 1), −1: none.
  int32_t shapeOf(uint32_t k) const noexcept { return k < shapes_.size() ? shapes_[k] : -1; }
  // A hash of everything the displayed frames depend on (the data's hash and the bake): the stack layer's tile key.
  uint64_t key() const noexcept { return key_; }

private:
  struct Spectrum {
    uint64_t hash = 0;
    std::vector<std::complex<double>> c;  // kHarmonics
    std::vector<double> mag, arg;         // polar form (the spectral morph)
  };
  const Spectrum* spectrumOf(const std::vector<float>& cycle, bool normalise);
  // The morphed spectrum at u, harmonics 0 … count − 1, into out.
  void morph(double u, uint32_t count, std::complex<double>* out) const;
  void rebuildDense();
  void rebake();

  meta::DataPtr ptr_;
  meta::WavetableData data_;
  bool ok_ = false;
  uint64_t dataHash_ = 0;
  Bake bake_{};
  uint32_t frames_ = 1;
  std::vector<Spectrum> cache_;                 // by cycle hash (kept across updates)
  std::vector<const Spectrum*> keys_;           // per keyframe
  std::vector<int32_t> shapes_;                 // per keyframe
  std::vector<float> dense_;                    // frames_ × kDisplayCycle (smoothed, unbaked)
  std::vector<float> bakedFrames_;              // frames_ × (kPoints + 1)
  float gain_ = 1.0f;
  uint64_t key_ = 0;
  std::vector<double> re_, im_;                 // FFT scratch
};

}  // namespace pq::views::wt
