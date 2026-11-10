#pragma once

// The stretch pool (T6.5; docs/plans/M6.md T6.5): the phase-vocoder time-stretchers behind the Complex / Complex Pro
// warp modes (pq/engine/audio_sequence.h "Complex"), signalsmith-stretch 1.4.0 (docs/DEPENDENCIES.md). The clip player
// owns one: N stereo stretchers and M mono ones (T6.5b: a mono source runs a mono stretcher, half the work), every one
// configured off the audio thread when the pool is made (signalsmith's default
// preset at the engine rate, split computation: each spectrum's work is spread over the output frames of its interval
// instead of landing on one block, so voices started together do not spike together). A rate change makes a new
// clip player and so a new pool.
//
// The audio thread claims a stretcher for a voice (a free-list pop per channel count: the pool belongs to the clip player, which only the
// audio thread drives, so the claim needs no atomics) and releases it when the voice ends; a mono claim takes a stereo stretcher
// when no mono one is free (fed the one channel twice); none free: the clip player falls back to Tones (counted). Everything in the "audio thread" section is allocation-free (the T6.5 spike,
// tests/engine/test_stretch_pool.cpp: RtScope over 10k blocks of varying ratio, pitch and formants; no local patch of
// the library was needed). The library's own randomness (phase randomisation beyond a 2× stretch) draws from a
// generator the pool reseeds at every claim, so a render is deterministic.
//
// Latency: a stretcher's output at time t is the analysis made `latency().output` output frames earlier, centred
// `latency().input` input frames behind the newest input then. The clip player primes a voice (reset, seek with the
// input before its first frame, then process from an output latency plus a block before the voice's start), so a
// Complex clip is sample-aligned and reports no latency. The pool is used without transpose: the clip player resamples
// the output for the pitch (exact; signalsmith's own transpose estimates each peak's frequency, a few cents off).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace pq::engine {

struct StretchParams {
  double transpose = 1.0;   // frequency factor (> 0): the clip's pitch ratio × file rate / engine rate
  double formant = 1.0;     // formant factor relative to the input (1: formants stay where the input has them)
  bool compensate = false;  // formants follow `formant` relative to the input whatever the transpose (Complex Pro)
  double formantBase = 0.0; // the formant analysis' fundamental, cycles per sample (0: detected)
  friend bool operator==(const StretchParams&, const StretchParams&) noexcept = default;
};

class StretchPool {
public:
  static constexpr uint32_t kNone = 0xFFFFFFFFu;
  // Input frames (`input`: the analysis centre's lag behind the newest input; `seek`: the history seek() wants),
  // output frames (`output`), and the analysis block.
  struct Latency {
    int input = 0, output = 0, seek = 0, block = 0;
    friend bool operator==(const Latency&, const Latency&) noexcept = default;
  };
  // The latencies a pool at `sampleRate` has, from the preset's formula (no stretcher needed: prefetch positions).
  // RT-safe. Tests check it against latency().
  static Latency latencyFor(double sampleRate) noexcept;

  // Not RT-safe: allocates and configures `voices` stereo and `monoVoices` mono stretchers at `sampleRate`. Stereo
  // stretchers are indices [0, voices), mono ones [voices, voices + monoVoices).
  StretchPool(uint32_t voices, double sampleRate, uint32_t monoVoices = 0);
  ~StretchPool();
  StretchPool(const StretchPool&) = delete;
  StretchPool& operator=(const StretchPool&) = delete;

  uint32_t size() const noexcept { return static_cast<uint32_t>(slots_.size()); }
  uint32_t available() const noexcept { return static_cast<uint32_t>(free_.size() + freeMono_.size()); }  // RT-safe
  uint32_t availableMono() const noexcept { return static_cast<uint32_t>(freeMono_.size()); }             // RT-safe
  uint32_t channels(uint32_t i) const noexcept { return i < stereo_ ? 2u : 1u; }                         // RT-safe
  size_t memoryBytes() const noexcept { return memory_; }  // what the stretchers allocated (measured at creation)
  const Latency& latency() const noexcept { return latency_; }  // measured from a configured stretcher

  // ---- audio thread (RT-safe) ----
  // A free stretcher for `channels` input channels (1: a mono one, else a stereo one; a stereo one when no mono one is
  // free), reset, its generator seeded with `seed`; kNone when none is free.
  uint32_t claim(uint32_t seed, uint32_t channels = 2) noexcept;
  void release(uint32_t i) noexcept;
  void reset(uint32_t i, uint32_t seed) noexcept;  // as claimed afresh (transpose 1, no formant processing)
  void setParams(uint32_t i, const StretchParams& p) noexcept;
  // in / out: one pointer per channel(i) (in[1] / out[1] are not read / written by a mono stretcher).
  // Loads input history without processing (in[c][0 .. frames), forward), `rate` = input frames per output frame.
  void seek(uint32_t i, const float* const* in, int frames, double rate) noexcept;
  // Consumes in[c][0 .. inFrames) and writes out[c][0 .. outFrames). `inLead` > 0: only frames [inLead, inFrames) are
  // in `in` (in[c][k] is frame inLead + k); the ones before read as zeros (a run too fast for the caller's buffer).
  void process(uint32_t i, const float* const* in, int inFrames, int inLead, float* const* out, int outFrames) noexcept;

  // Tests: the generator state of stretcher i (determinism checks).
  uint64_t generatorState(uint32_t i) const noexcept;

private:
  struct Slot;
  std::vector<std::unique_ptr<Slot>> slots_;
  std::vector<uint32_t> free_, freeMono_;
  uint32_t stereo_ = 0;
  size_t memory_ = 0;
  Latency latency_;
};

}  // namespace pq::engine
