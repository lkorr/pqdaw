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
//   sounding   TMM.16 (the picture must match the sound): a dense frame AT FULL RESOLUTION — all 1023 harmonics, no σ
//              (the table's level 0, what a low note plays), one 2048-point inverse FFT, baked through the same shared
//              formulas at kSoundPoints phases, `removeDc` after the bake — computed lazily per dense frame and kept
//              until the data or the bake changes (≈ 0.15 ms a frame; the highlighted frame needs two). The draw-mode
//              cycle, the highlighted frame and the voice dots use it; the stacked frames behind stay σ-smoothed
//              thumbnails.
//
// Costs (measured in tests/views/test_meta_wavetable_face.cpp): a data change (64 frames, two keyframes) ≈ 1 ms, a
// baked knob step ≈ 0.2 ms; nothing per paint. UI thread only.

#include <array>
#include <complex>
#include <cstdint>
#include <span>
#include <vector>

#include "pq/dsp/frame_table.h"
#include "pq/meta/wavetable.h"

namespace pq::views::wt {

// TMM.16: a periodic frame of x.size() samples (a power of two) as out.size() − 1 points (a power of two; the last
// value repeats the first): its harmonics 0 … K, K = min(top, points / 2 − 1), weighted by Lanczos σ factors (no
// Gibbs ringing in the picture: a band-limited edge shows as an edge, not as ripples), the rest dropped.
void smoothFrame(std::span<const double> x, uint32_t top, std::span<float> out, std::vector<double>& re,
                 std::vector<double>& im);

inline constexpr uint32_t kHarmonics = 1024;        // c_0 … c_1023 of a 2048-sample cycle (the builder's)
inline constexpr uint32_t kDisplayHarmonics = 192;  // drawn harmonics (σ-smoothed)
inline constexpr uint32_t kDisplayCycle = 512;      // the smoothed cycle's points (read by the displayed bake)
inline constexpr uint32_t kPoints = 256;            // phases per displayed frame (the frame holds kPoints + 1: closed)
inline constexpr uint32_t kBars = 32;               // harmonic bars shown (TMM.9 scrolls and zooms them)
inline constexpr uint32_t kSoundPoints = 1024;      // TMM.16: phases of a sounding frame (+ 1: closed)

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
  std::span<const float> frame(uint32_t f) const;
  // The display gain: 1 / max(peak, 1) (a table louder than full scale is drawn to fit, a quiet one as it is).
  float gain() const;
  // The displayed frame at position u (the reader's blend of the two nearest dense frames) into out (kPoints + 1).
  void blended(float u, std::span<float> out) const;
  // One value of the blended frame at position u, phase φ ∈ [0, 1).
  float valueAt(float u, float phase) const;
  // TMM.16: the frame at position u as it sounds (see the file comment) into out (kSoundPoints + 1 values: the last
  // repeats the first), and one value of it at phase φ. The reader's pair and weight (from float u, as it computes
  // them).
  void sounding(float u, std::span<float> out);
  float soundAt(float u, float phase);
  // Dense frame d as it sounds (kSoundPoints + 1 values).
  std::span<const float> soundFrame(uint32_t d);

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
  void rebuildDense() const;
  void rebake() const;
  void ensure() const;  // TMM.16: the dense frames and their bake are rendered when first needed

  meta::DataPtr ptr_;
  meta::WavetableData data_;
  bool ok_ = false;
  uint64_t dataHash_ = 0;
  Bake bake_{};
  uint32_t frames_ = 1;
  std::vector<Spectrum> cache_;                 // by cycle hash (kept across updates)
  std::vector<const Spectrum*> keys_;           // per keyframe
  std::vector<int32_t> shapes_;                 // per keyframe
  mutable std::vector<float> dense_;            // frames_ × kDisplayCycle (smoothed, unbaked)
  mutable std::vector<float> bakedFrames_;      // frames_ × (kPoints + 1)
  mutable float gain_ = 1.0f;
  mutable bool denseDirty_ = true, bakeDirty_ = true;
  uint64_t key_ = 0;
  mutable std::vector<double> re_, im_;         // FFT scratch
  std::vector<std::vector<float>> sound_;       // TMM.16: per dense frame, empty until asked for
  std::vector<std::complex<double>> soundSpec_; // scratch
  std::vector<double> sre_, sim_, bakeBuf_;
};

// TMM.16 — what the audio side plays, read from the table published to the instance's slot (views::HeardTable): the
// picture is then the sound by construction (previews while a drag runs included), not a second rendering of it. Each
// displayed frame is the table's dense frame at one mip level — the level the lowest sounding voice reads (its
// reported increment, dsp::frameTableLevel), level 0 (every harmonic: the lowest notes) when nothing sounds — read
// through the readers' own interpolator (dsp::readFrameTableReference), with the controls the live path applies on top
// (fold / warp / sync when modulated: the shared formulas at their param values). Nothing is read until drawn:
// thumbnails (kPoints) per frame when the stacked display draws it, full frames (kSoundPoints) per frame when the
// highlighted frame or draw mode needs it; a new table drops both. UI thread only.
class Heard {
public:
  // True when what is displayed changed. A null table clears it (the face falls back to its Model).
  bool update(std::shared_ptr<const dsp::FrameTable> table, uint32_t level, const Bake& live);
  bool active() const noexcept { return table_ != nullptr; }
  const dsp::FrameTable* table() const noexcept { return table_.get(); }
  uint32_t level() const noexcept { return level_; }
  uint32_t frames() const noexcept { return frames_; }
  std::span<const float> frame(uint32_t f) const;  // a thumbnail: kPoints + 1 values
  float gain() const;
  void blended(float u, std::span<float> out) const;  // thumbnails blended as the reader blends frames
  void sounding(float u, std::span<float> out);              // kSoundPoints + 1 values
  float soundAt(float u, float phase);
  uint64_t key() const noexcept { return key_; }

private:
  double readAt(double u, double phase, uint32_t level) const noexcept;  // the read at u, live controls applied
  void curveAt(double u, std::span<float> out) const;  // the curve at u: 2048 reads at the level, smoothed
  bool liveActive() const noexcept { return live_.fold != 0.0f || live_.warp != 0.0f || live_.sync != 0.0f; }
  std::span<const float> sharp(uint32_t frame);
  std::shared_ptr<const dsp::FrameTable> table_;
  uint32_t level_ = 0;
  Bake live_{};
  uint32_t frames_ = 1;
  mutable std::vector<float> thumbs_;       // frames × (kPoints + 1), each read when first asked for
  mutable std::vector<uint8_t> thumbDone_;
  std::vector<std::vector<float>> sharp_;   // per frame, empty until asked for
  mutable float gain_ = 1.0f;               // < 0: not computed yet
  uint64_t key_ = 0;
  mutable std::vector<double> buf_, re_, im_;  // scratch
  float liveU_ = -1.0f;                     // live controls: the curve at one position (the controls act on the
  std::vector<float> liveCurve_;            // blended read)
};

}  // namespace pq::views::wt
