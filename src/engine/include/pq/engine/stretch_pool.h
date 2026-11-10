#pragma once

// The stretch pool (T6.5; docs/plans/M6.md T6.5): the phase-vocoder time-stretchers behind the Complex / Complex Pro
// warp modes (pq/engine/audio_sequence.h "Complex"), signalsmith-stretch 1.4.0 (docs/DEPENDENCIES.md). The clip player
// owns one: stereo stretchers and mono ones (T6.5b: a mono source runs a mono stretcher, half the work), every one
// configured off the audio thread (signalsmith's default preset at the engine rate, split computation: each spectrum's
// work is spread over the output frames of its interval instead of landing on one block). Split computation spreads
// one voice's work only: voices primed on the same sample would compute their spectra in the same blocks, so the clip
// player offsets each voice's chunk grid (M6 review M10; clip_player.h "Complex").
//
// T6.x-fix-warp (M6 review H2, L15): the pool GROWS. It is created empty (or with a fixed set, for tests and one-clip
// renders) with room for `capacity` stretchers; build() makes more off the audio thread (the timeline sizes them from
// the Complex clips of each audio image it publishes: ClipPlayer::stretchDemand) and adopt() hands them over on the
// audio thread without allocating (the pool's vectors are reserved to the capacity). A project without Complex clips
// has no stretcher. The pool never shrinks while its clip player lives; a rate change makes a new clip player and so a
// new pool.
//
// The audio thread claims a stretcher for a voice (a free-list pop per channel count: the pool belongs to the clip
// player, which only the audio thread drives, so the claim needs no atomics) and releases it when the voice ends; a
// mono claim takes a stereo stretcher when no mono one is free (fed the one channel twice); none free: the clip player
// plays the voice as Tones meanwhile (counted) and retries the claim every block. Everything in the "audio thread"
// section is allocation-free (the T6.5 spike, tests/engine/test_stretch_pool.cpp: RtScope over 10k blocks of varying
// ratio, pitch and formants; no local patch of the library was needed). The library's own randomness (phase
// randomisation beyond a 2× stretch) draws from a generator the pool reseeds at every claim, so a render is
// deterministic. Each stretcher has an output buffer of `outFrames` frames per channel (two channels) that the clip
// player keeps its recent output in (out()).
//
// Latency: a stretcher's output at time t is the analysis made `latency().output` output frames earlier, centred
// `latency().input` input frames behind the newest input then. The clip player primes a voice (reset, seek with the
// input before its first frame, then process from an output latency plus a block before the voice's start), so a
// Complex clip is sample-aligned and reports no latency. The pool is used without transpose: the clip player resamples
// the output for the pitch (exact; signalsmith's own transpose estimates each peak's frequency, a few cents off).

#include <atomic>
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
  struct Slot;  // one configured stretcher and its output buffer (opaque; build() returns a linked list of them)
  // Input frames (`input`: the analysis centre's lag behind the newest input; `seek`: the history seek() wants),
  // output frames (`output`), the analysis block and its interval (the hop between spectra).
  struct Latency {
    int input = 0, output = 0, seek = 0, block = 0, interval = 0;
    friend bool operator==(const Latency&, const Latency&) noexcept = default;
  };
  // The latencies a pool at `sampleRate` has, from the preset's formula (no stretcher needed: prefetch positions).
  // RT-safe. Tests check it against latency().
  static Latency latencyFor(double sampleRate) noexcept;

  // Not RT-safe: room for `capacity` stretchers (at least voices + monoVoices); allocates and configures `voices`
  // stereo and `monoVoices` mono ones at `sampleRate`, each with an output buffer of `outFrames` frames per channel.
  StretchPool(uint32_t voices, double sampleRate, uint32_t monoVoices = 0, uint32_t capacity = 0, uint32_t outFrames = 0);
  ~StretchPool();
  StretchPool(const StretchPool&) = delete;
  StretchPool& operator=(const StretchPool&) = delete;

  // Not RT-safe (any thread): `stereo` + `mono` more stretchers at `sampleRate` (stereo ones first in the list), for
  // adopt() by a pool of that rate and output size; null when both are 0. A list nobody adopted: destroy().
  static Slot* build(uint32_t stereo, uint32_t mono, double sampleRate, uint32_t outFrames);
  static void destroy(Slot* list) noexcept;
  static Slot* append(Slot* list, Slot* more) noexcept;  // `more` linked after `list` (either may be null)
  // Lock-free push of a build() list onto a shared head (another thread takes the whole list with exchange(nullptr)).
  static void push(std::atomic<Slot*>& head, Slot* list) noexcept;

  uint32_t size() const noexcept { return static_cast<uint32_t>(slots_.size()); }
  uint32_t capacity() const noexcept { return capacity_; }
  uint32_t available() const noexcept { return static_cast<uint32_t>(free_.size() + freeMono_.size()); }  // RT-safe
  uint32_t availableMono() const noexcept { return static_cast<uint32_t>(freeMono_.size()); }             // RT-safe
  uint32_t channels(uint32_t i) const noexcept;                                                         // RT-safe
  // Whether claim(·, channels) would find a stretcher. RT-safe.
  bool canClaim(uint32_t channels) const noexcept { return !free_.empty() || (channels == 1 && !freeMono_.empty()); }
  size_t memoryBytes() const noexcept { return memory_; }  // estimated bytes of the stretchers it holds
  const Latency& latency() const noexcept { return latency_; }  // measured from a configured stretcher

  // ---- audio thread (RT-safe) ----
  // Takes over a build() list (made at this pool's rate and output size): its stretchers become free ones. Those that
  // do not fit the capacity are not taken: returns them (null when all fit); the caller frees them off the audio thread.
  Slot* adopt(Slot* list) noexcept;
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
  // Stretcher i's output buffer (`outFrames` floats per channel; channel 1 follows channel 0).
  float* output(uint32_t i) noexcept;

  // Tests: the generator state of stretcher i (determinism checks).
  uint64_t generatorState(uint32_t i) const noexcept;

private:
  std::vector<std::unique_ptr<Slot>> slots_;
  std::vector<uint32_t> free_, freeMono_;
  uint32_t capacity_ = 0;
  size_t memory_ = 0;
  Latency latency_;
};

}  // namespace pq::engine
