#pragma once

// The Sampler's Complex / Complex Pro voices (SM.5; docs/plans/SM.md "Semantics", "SM.5"): a phase vocoder per voice
// lane (signalsmith-stretch through dsp::StretchPool, pq/dsp/stretch_pool.h) over the resident sample, its output
// resampled at the voice's per-sample pitch rate.
//
// ---- The bank (SamplerStretch) ----------------------------------------------------------------------------------------
// One per Sampler instance whose warp setting is Complex / Pro: `lanes` stretchers of the sample's channel count,
// built off the audio thread (the link: when a lowering asks for it, before any image of it is built; again when a
// published sample has another channel count) and published into the instance's SampleSlot (SampleSlot::setStretch:
// the `sampler_warp` atom reads one resource, so the bank rides on it; retired through the slot's reclaimer). Other
// warp settings have none (a Classic project allocates no stretcher). Memory ≈ 0.75 MiB per stereo stretcher.
//   Ownership (audio thread only, no atomics: one instance's kernels run on one thread): stretcher s has an owner
// {token, user lane, vHi}; a voice holds (s, token) and its own vHi, and uses s only while all three match (so a
// voice whose lane words were copied — a hot-swap's state transfer, a Sync — or moved keeps its stretcher in one place
// only: the first to run in a block advances it, the other finds vHi moved and plays Tones while it claims another).
// A trigger in lane L first frees the stretchers whose user is L (the lane's previous voice is over: its lane words
// are being reset), then takes stretcher L if it is free, else any free one; none free: Tones, retried every call.
//
// ---- One voice (ComplexVoice, beside the lane's WarpVoice) --------------------------------------------------------
// Vocoder frames y are file frames on the voice's read clock: the output at sample t reads the vocoder output at
// Y(t) = Φ(t) (WarpVoice::phi, the integrated per-sample pitch rate), cubic, so the pitch is the rate exactly (the
// stretcher runs at transpose 1). The content the vocoder holds at frame y is the voice's time law (WarpVoice: content
// beats since the trigger → markers → source frame, unfolded through the loop) at the output time the read reaches y,
// estimated where the chunk is computed: t_y = t + (y − Y(t)) / r(t) (a glide moves the estimate; the input head is
// recomputed from the law every chunk, so the content never drifts). The stretcher is fed one continuous stream (the
// unfolded source frames, zeros before the trigger's start and, without a loop, after the end) in chunks of
// kComplexChunk output frames, each computed at the first sample whose read needs it (Y + 3 + the voice's lead), so a
// render does not depend on the block size.
//   Note-on: the voice plays Tones (WarpVoice) while its stretcher primes (reset, seek with the history before its
// first frame, then the vocoder from an output latency + a block before the read, at most kComplexPrimeRate ×
// max(1, r) vocoder frames per output sample: never a burst; ≈ 70 ms at r = 1), and crosses to Complex over 2 ms
// (warp.h warpFadeFrames) at the first sample the vocoder has what the read needs. Phase offsets: each stretcher's
// lead (a golden-ratio fraction of the analysis interval by its index) keeps its vocoder that far ahead of the read
// and delays its priming by lead / k samples, so voices started together do their spectral work in different blocks.
// Complex Pro: the formants are moved back by r^(−formants %) (the clip's rule, audio_sequence.cpp), base 1 / order.
// Lost stretcher (another voice took it): back to Tones at once (a hard switch), re-primed on a free one.
// RT-safe: everything noexcept, no allocation; ComplexVoice is POD (lane words).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>

#include "pq/core/aligned_buffer.h"
#include "pq/dsp/stretch_pool.h"
#include "pq/dsp/warp_voice.h"

namespace pq::dsp {

inline constexpr uint32_t kComplexChunk = 128;        // vocoder frames per process() call
inline constexpr uint32_t kComplexFrames = 8192;      // vocoder output kept per stretcher and channel
inline constexpr uint32_t kComplexInFrames = 16384;   // the input gather per channel at least (also ≥ the seek)
inline constexpr uint32_t kComplexPrimeRate = 4;      // vocoder frames per output sample while priming, × max(1, r)

class SamplerStretch {
public:
  static constexpr uint32_t kNone = 0xFFFFFFFFu;
  struct Owner {
    uint32_t token = 0;     // 0: free
    uint32_t user = kNone;  // the global lane (group · 8 + lane) that last ran it
    int64_t vHi = 0;        // its vocoder frames computed (the user's ComplexVoice::vHi)
  };

  // Not RT-safe: `lanes` stretchers (≥ 1) of `channels` (1 or 2) at `sampleRate`.
  static std::unique_ptr<SamplerStretch> build(uint32_t lanes, uint32_t channels, double sampleRate);
  SamplerStretch(const SamplerStretch&) = delete;
  SamplerStretch& operator=(const SamplerStretch&) = delete;

  uint32_t lanes() const noexcept { return lanes_; }
  uint32_t channels() const noexcept { return channels_; }
  double sampleRate() const noexcept { return sampleRate_; }
  uint32_t tag() const noexcept { return tag_; }  // distinct per bank (a voice's claim names its bank)
  size_t memoryBytes() const noexcept { return pool_.memoryBytes() + sizeof(float) * in_.size(); }
  uint32_t inFrames() const noexcept { return inCap_; }  // the input gather per channel (beyond: skipped as zeros)

  // ---- audio thread (RT-safe) ----
  StretchPool& pool() noexcept { return pool_; }
  Owner& owner(uint32_t s) noexcept { return owners_[s]; }
  float* in(uint32_t c) noexcept { return in_.data() + static_cast<size_t>(c) * inCap_; }
  uint32_t newToken() noexcept {
    if (++nextToken_ == 0) nextToken_ = 1;
    return nextToken_;
  }
  // Tests / stats: claims that found no free stretcher, chunks computed.
  uint64_t misses = 0, chunks = 0;

private:
  SamplerStretch(uint32_t lanes, uint32_t channels, double sampleRate);
  StretchPool pool_;
  std::unique_ptr<Owner[]> owners_;
  AlignedBuffer<float> in_;  // channels × inCap_
  uint32_t lanes_, channels_, tag_, inCap_;
  double sampleRate_;
  uint32_t nextToken_ = 0;
};

struct ComplexVoice {
  enum Phase : uint32_t { kWait = 0, kPending, kPriming, kFading, kOn };
  int64_t vLo = 0, vHi = 0;       // vocoder frames held in the stretcher's output buffer: [vLo, vHi)
  int64_t ys = 0;                 // the first vocoder frame (the priming schedule's base)
  int64_t head = 0;               // stream frames fed
  int64_t u0 = 0;                 // unfolded source frame of stream frame 0
  int64_t zeroBelow = 0;          // stream frames below: zeros (before the start)
  int64_t zeroAbove = INT64_MAX;  // stream frames from here on: zeros (after the end, no loop)
  int64_t primeT = 0;             // Pending: the sample the priming starts; Priming: the one it started
  double primeK = 0.0;            // vocoder frames per output sample while priming
  uint32_t stretcher = SamplerStretch::kNone, token = 0, bank = 0;
  uint32_t phase = kWait;
  uint32_t fade = 0;              // Fading: samples done
  uint32_t lead = 0;

  // At the lane's trigger (after WarpVoice::trigger): frees the stretchers lane `lane` ran, claims one. RT-safe.
  void trigger(SamplerStretch* bank, uint32_t lane) noexcept;
  // Renders n (≤ 256) frames of voice v (which it advances), as WarpVoice::render: Tones while the stretcher primes,
  // the 2 ms cross, then the vocoder read at Φ. bank null (or of fewer channels than the sample): Tones. run.mode
  // ComplexPro: formant preservation (run.set.formants %, envelope order run.set.formantEnvelope). RT-safe.
  void render(SamplerStretch* bank, uint32_t lane, WarpVoice& v, const WarpSource& src, const WarpVoiceRun& run,
              const WarpRunLaw& law, const float* rate, uint32_t n, float* outL, float* outR, float* pos,
              float* playing, const WarpScratch& sc) noexcept;
};
static_assert(std::is_trivially_copyable_v<ComplexVoice>);

}  // namespace pq::dsp
