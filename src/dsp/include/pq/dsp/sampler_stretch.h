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
// SM.16: the stream — what the stretcher is fed, the priming, the chunks, the lead, the formant rule, the constants —
// is dsp::ComplexStream (pq/dsp/complex_stream.h), the one the clip player drives too. The voice is its host and keeps
// what is the Sampler's: the lane's ownership of a stretcher, the Tones cover, the 2 ms cross, the cubic read.
//   Vocoder frames y are file frames on the voice's read clock: the output at sample t reads the vocoder output at
// Y(t) = Φ(t) (WarpVoice::phi, the integrated per-sample pitch rate), cubic, so the pitch is the rate exactly. The
// content the vocoder holds at frame y is the voice's time law (WarpVoice: content beats since the trigger → markers
// → source frame, unfolded through the loop) at the output time the read reaches y, estimated where the chunk is
// computed: t_y = t + (y − Y(t)) / r(t) (a glide moves the estimate; the stream recomputes its input head from the law
// every chunk, so the content never drifts). Each chunk is computed at the first sample whose read needs it (Y + 3 +
// the voice's lead), so a render does not depend on the block size. Φ is integrated once per inner run (WarpClock,
// shared with the Tones voice under it), the sample a chunk is due at is found once per chunk (Φ is monotonic), and
// the frames between two chunks are read 8 at a time (simd.h taps8 + the one cubic).
//   Note-on: the voice plays Tones (WarpVoice) while its stretcher primes (at most complexPrimeRate(r) vocoder frames
// per output sample: never a burst; ≈ 70 ms at r = 1; its priming delayed by lead / kComplexPrimeRate samples, so
// voices started together do their spectral work in different blocks), and crosses to Complex over 2 ms (warp.h
// warpFadeFrames) at the first sample the vocoder has what the read needs.
// Complex Pro: complexFormants at the pitch ratio of the moment, set again only when the ratio or the settings moved.
// Lost stretcher (another voice took it): back to Tones at once (a hard switch; the Tones voice positions its grains
// afresh: it never catches up on the ones it skipped, warpGrainCentre), re-primed on a free one.
// RT-safe: everything noexcept, no allocation; ComplexVoice is POD (lane words).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>

#include "pq/core/aligned_buffer.h"
#include "pq/dsp/complex_stream.h"
#include "pq/dsp/stretch_pool.h"
#include "pq/dsp/warp_voice.h"

namespace pq::dsp {

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
  ComplexIn in() noexcept { return {{in_.data(), in_.data() + (channels_ > 1 ? inCap_ : 0u)}, inCap_}; }
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
  ComplexStream cx;               // the stream (cx.vHi also names the voice's place in its stretcher: Owner::vHi)
  int64_t primeT = 0;             // Pending: the sample the priming starts; Priming: the one it started
  double primeK = 0.0;            // vocoder frames per output sample while priming
  double fRatio = 0.0;            // the formant params last set: the pitch ratio, Formants, the order, the mode
  float fFormants = 0.0f, fOrder = 0.0f;
  uint32_t fMode = 0;
  uint32_t stretcher = SamplerStretch::kNone, token = 0, bank = 0;
  uint32_t phase = kWait;
  uint32_t fade = 0;              // Fading: samples done
  uint32_t lead = 0;

  // At the lane's trigger (after WarpVoice::trigger): frees the stretchers lane `lane` ran, claims one. RT-safe.
  void trigger(SamplerStretch* bank, uint32_t lane) noexcept;
  // Renders n frames of voice v (which it advances), as WarpVoice::render: Tones while the stretcher primes,
  // the 2 ms cross, then the vocoder read at Φ. bank null (or of fewer channels than the sample): Tones. run.mode
  // ComplexPro: formant preservation (run.set.formants %, envelope order run.set.formantEnvelope). RT-safe.
  void render(SamplerStretch* bank, uint32_t lane, WarpVoice& v, const WarpSource& src, const WarpVoiceRun& run,
              const WarpRunLaw& law, const float* rate, uint32_t n, float* outL, float* outR, float* pos,
              float* playing, const WarpScratch& sc) noexcept;
};
static_assert(std::is_trivially_copyable_v<ComplexVoice>);

}  // namespace pq::dsp
