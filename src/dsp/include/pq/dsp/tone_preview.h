#pragma once

// The tone preview (VZ.3; ported from PQ Meter's audio/tone_preview.h, b721058): one tone app-wide, played while a
// display's play surface is held (the Spectrum's plot, the Spectrogram's keys or Ctrl+click), straight into the
// audio output's monitor pair — never into the project (no track, no recording, no render sees it).
//
//   hold(owner, hz, dB, wave)  UI thread: (re)starts or retunes the tone; the owner token makes it one tone app-wide
//                              (another display's hold takes it over; a stale release from the old owner is ignored)
//   release(owner)             UI thread: the gate closes; the tone fades out over 30 ms
//   render(out, …)             audio thread, RT-safe: ADDS the tone into `channels` buffers
//
// The oscillator: uint32 phase (2^32 = one cycle) reading 2048-point tables with linear interpolation — a sine, and
// for Semisine (a half-wave rectified sine without its DC: the pitch with its even harmonics, audible on small
// speakers) and Triangle (odd harmonics at −12 dB/oct) ten band-limited tables, one per octave of fundamental (40 Hz
// … 20.48 kHz tops), every harmonic faded out between 18 and 20 kHz (PQ Meter's 2 kHz fade under its cut). The
// frequency glides in pitch (12 ms time constant, log2 domain); the gain ramps linearly (5 ms attack, 30 ms release,
// a retune at the held level does not ramp), so no press, drag or release clicks. Tables are built once (the
// constructor, off the audio thread).

#include <atomic>
#include <cstdint>

namespace pq::dsp {

enum class ToneWave : uint8_t { Sine, SemiSine, Triangle };
inline constexpr int kToneWaveCount = 3;
inline constexpr const char* kToneWaveNames[] = {"Sine", "Semisine", "Triangle"};

class TonePreview {
public:
  static constexpr double kAttackSeconds = 0.005, kReleaseSeconds = 0.030, kGlideSeconds = 0.012;
  static constexpr uint32_t kTableBits = 11, kTableSize = 1u << kTableBits;
  static constexpr int kBands = 10;  // fundamentals up to 40 · 2^b Hz

  TonePreview();  // builds the tables (allocates: never on the audio thread)

  // ---- UI thread ----
  void hold(const void* owner, float hz, float dbfs, ToneWave wave) noexcept;
  void release(const void* owner) noexcept;
  bool heldBy(const void* owner) const noexcept { return owner_ != nullptr && owner_ == owner; }
  float targetHz() const noexcept { return targetHz_.load(std::memory_order_relaxed); }
  // Held, or still fading out (the audio thread clears it once the gain reached 0). Any thread.
  bool sounding() const noexcept {
    return gate_.load(std::memory_order_relaxed) || active_.load(std::memory_order_relaxed);
  }

  // ---- audio thread ----
  // Adds `frames` of the tone at `sampleRate` into out[0 … channels − 1] (the same signal in each).
  // RT-safe
  void render(float* const* out, uint32_t channels, uint32_t frames, double sampleRate) noexcept;
  // The oscillator's state (tests: the audio thread's own reads).
  float currentHz() const noexcept { return hz_; }
  float currentGain() const noexcept { return gain_; }

private:
  const float* table(ToneWave w, float hz) const noexcept;

  // UI → audio.
  std::atomic<float> targetHz_{0.0f}, targetGain_{0.0f};
  std::atomic<uint8_t> wave_{0};
  std::atomic<bool> gate_{false};
  // Audio → UI.
  std::atomic<bool> active_{false};
  const void* owner_ = nullptr;  // UI thread
  // Audio thread.
  uint32_t phase_ = 0;
  float hz_ = 0.0f, log2Hz_ = 0.0f, gain_ = 0.0f;
  // The shared tables (built once, process-wide): [0] the sine; then kBands semisine; then kBands triangle
  // (kTableSize + 1 each: a guard point).
  const float* tables_ = nullptr;
};

}  // namespace pq::dsp
