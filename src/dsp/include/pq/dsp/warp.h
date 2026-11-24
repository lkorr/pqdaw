#pragma once

// Kernels of the time-stretch warp modes (T6.3; docs/plans/M6.md T6.3; the player is pq/engine/clip_player.h). The
// clip player reads the source through the resampler (pq/dsp/resample.h: sincResample / gainCopy, the per-frame gain
// fused); these kernels make those per-frame gains and choose where grains read:
//   Tones / Texture  granular overlap-add: grains of N output frames every H = N / 2, a periodic Hann window
//                    (warpHannGain: w(j) = sin²(πj / N); Σ_k w(t − kH) = 1, so a stationary source keeps its level).
//                    Tones aligns each grain with the one before (warpBestLag: the lag that best
//                    continues the previous grain's waveform, WSOLA), so a monophonic periodic source keeps its pitch
//                    exactly (one lag per grain: a chord's partials drift; Complex is the mode for chords);
//                    Texture leaves grains where the warp law puts them and jitters them (warpJitter, Flux).
//   Beats            slices cut at transients or a grid; each slice's gain is piecewise linear (fade in, Envelope
//                    decay, the 2 ms crossfade into the next slice, a loop's declick): warpRampGain.
// All RT-safe (no allocation) except hannWindow. AVX2.
//
// SM.15: this header is also the one place of what every warp client shares (the document, the clip player's image,
// the Sampler engine and its voice): the mode and slice-loop enums, the modes' settings, the marker law (WarpLaw) and
// the rules that size grains, alignment, jitter and fades.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "pq/core/time.h"

namespace pq::dsp {

// ---- Modes and settings (one of each; doc::AudioWarp / doc::BeatsLoop are these) ---------------------------------------
// How a source follows the tempo: Off (its own time), RePitch (speed follows the tempo), and the time-stretch modes
// Beats / Tones / Texture (this file, pq/dsp/warp_voice.h) and Complex / Complex Pro (the phase vocoder).
enum class AudioWarp : uint8_t { Off, RePitch, Beats, Tones, Texture, Complex, ComplexPro };
inline constexpr AudioWarp kLastAudioWarp = AudioWarp::ComplexPro;
inline constexpr bool warpComplex(AudioWarp w) noexcept { return w == AudioWarp::Complex || w == AudioWarp::ComplexPro; }
inline constexpr bool warpStretches(AudioWarp w) noexcept { return w >= AudioWarp::Beats && w <= kLastAudioWarp; }
// What a Beats slice does past its natural end.
enum class WarpSliceLoop : uint8_t { Off, Forward, BackForward };
// A kernel's float input as the enum (the Sampler engine wires the enum's value; NaN / out of range: `fallback`).
inline AudioWarp audioWarpOf(float v, AudioWarp fallback = AudioWarp::Tones) noexcept {  // RT-safe
  return v >= 0.0f && v <= static_cast<float>(kLastAudioWarp) ? static_cast<AudioWarp>(static_cast<int>(v + 0.5f)) : fallback;
}
inline WarpSliceLoop warpSliceLoopOf(float v) noexcept {  // RT-safe
  return v >= 0.0f && v <= 2.0f ? static_cast<WarpSliceLoop>(static_cast<int>(v + 0.5f)) : WarpSliceLoop::Forward;
}

// The time-stretch modes' settings, resolved (doc::WarpParams is the user's form: ms, ticks; both clients lower to
// this one). The Sampler ignores pitchRatio (its rate is per sample) and granulation (its slices are the sample's).
struct WarpSettings {
  double pitchRatio = 1.0;       // the transpose × detune (a pitch shift; the duration follows the tempo)
  Ticks granulation = kPpq / 4;  // Beats: the slice grid in content ticks (0: the transients)
  WarpSliceLoop sliceLoop = WarpSliceLoop::Forward;
  float envelope = 100.0f;       // Beats: % of each slice's gap at full gain before it fades to the next slice
  double grainSeconds = 0.03;    // Tones / Texture
  float flux = 0.0f;             // Texture: grain position jitter, % of the hop
  uint32_t seed = 0;             // Texture: the jitter's seed; Complex: the stretcher's (the app uses the clip id)
  float formants = 100.0f;       // Complex Pro: formant preservation, % [0, 100]
  float formantEnvelope = 128.0f;  // Complex Pro: the spectral envelope's order [8, 256] (formant base sr / order)
};

// ---- The rules (one of each) ------------------------------------------------------------------------------------------
inline constexpr uint32_t kWarpMaxAlignLen = 2048;  // Tones: source frames compared per grain at most
inline constexpr uint32_t kWarpMaxAlignLags = 960;  // Tones: ± source frames searched at most
// The grain of `seconds` at engine rate sr: N output frames (even, ≥ 2); the hop is N / 2. RT-safe.
inline uint32_t warpGrainFrames(double seconds, double sr) noexcept {
  const double secs = std::clamp(seconds, 0.001, 0.5);
  return static_cast<uint32_t>(std::max<int64_t>(2, 2 * std::llround(secs * sr / 2.0)));
}
// Tones' alignment for a grain of hop H at rate r (hr = H·r source frames): none below 16 frames, else over
// min(hr, kWarpMaxAlignLen) frames, ± min(hr / 2, 12 ms of the source, kWarpMaxAlignLags). RT-safe.
inline void warpAlignFor(double hr, double sourceRate, uint32_t* len, uint32_t* lags) noexcept {
  *len = *lags = 0;
  if (!(hr >= 16.0)) return;
  *len = static_cast<uint32_t>(std::min<double>(std::round(hr), kWarpMaxAlignLen));
  *lags = static_cast<uint32_t>(
      std::min<double>({std::floor(hr / 2.0), std::floor(0.012 * sourceRate), static_cast<double>(kWarpMaxAlignLags)}));
}
// Texture's jitter: ± flux % of the hop's source frames. RT-safe.
inline double warpJitterFrames(double flux, double hr) noexcept { return std::clamp(flux, 0.0, 100.0) / 100.0 * hr; }
// Beats' slice crossfade and a voice's end fade: 2 ms of output. RT-safe.
inline int64_t warpFadeFrames(double sr) noexcept { return std::max<int64_t>(1, std::lround(0.002 * sr)); }

// ---- The marker law ---------------------------------------------------------------------------------------------------
// content (the clip: content ticks; the Sampler: beats) → source frame, piecewise linear: from content x0 on (the
// first segment also before it) frame = f0 + (x − x0)·slope, slope > 0. Warp markers (x_i, f_i) make one segment per
// neighbouring pair (the end segments extend); a lone marker one segment through it at the source's own tempo; none:
// one through (0, 0).
struct WarpSeg {
  double x0 = 0.0, f0 = 0.0, slope = 1.0;
  constexpr double frame(double x) const noexcept { return f0 + (x - x0) * slope; }
  constexpr double content(double f) const noexcept { return x0 + (f - f0) / slope; }
  friend constexpr bool operator==(const WarpSeg&, const WarpSeg&) noexcept = default;
};
// The piece of n ≥ 1 ascending keys that holds v: the last k ≥ 1 with key(k) ≤ v, else 0. RT-safe.
template <class Key>
constexpr uint32_t warpPieceOf(uint32_t n, double v, Key&& key) noexcept {
  uint32_t lo = 1, hi = n;
  while (lo < hi) {
    const uint32_t mid = (lo + hi) / 2;
    if (key(mid) <= v) lo = mid + 1;
    else hi = mid;
  }
  return lo - 1;
}
// How many segments `markers` markers make, and segment k of them (slope1: the source's own slope, used with fewer
// than two markers). xy(i) → {content, frame} of marker i (a pair of doubles). RT-safe.
constexpr uint32_t warpMarkerSegs(uint32_t markers) noexcept { return markers < 2 ? 1u : markers - 1; }
template <class XY>
constexpr WarpSeg warpMarkerSeg(uint32_t markers, uint32_t k, double slope1, XY&& xy) noexcept {
  if (markers == 0) return WarpSeg{0.0, 0.0, slope1};
  const auto a = xy(k);
  if (markers == 1) return WarpSeg{a.first, a.second, slope1};
  const auto b = xy(k + 1);
  return WarpSeg{a.first, a.second, (b.second - a.second) / (b.first - a.first)};
}

// The law over prepared segments (a view: the image / the slot's entry owns them; null: the single segment `one`),
// with the content loop [loopStart, loopEnd) when looping. Built once per image clip / per run, never per render.
struct WarpLaw {
  const WarpSeg* seg = nullptr;  // count segments by x0 (and f0); null: `one`
  uint32_t count = 1;
  WarpSeg one;
  bool looping = false;
  double loopStart = 0.0, loopEnd = 0.0;  // content
  int64_t loopStartF = 0, loopEndF = 0;   // the loop in whole source frames (unfolded streams: Complex)

  const WarpSeg* segs() const noexcept { return seg != nullptr ? seg : &one; }  // RT-safe (all below)
  uint32_t pieceAt(double x) const noexcept {
    const WarpSeg* g = segs();
    return count < 2 ? 0u : warpPieceOf(count, x, [g](uint32_t k) { return g[k].x0; });
  }
  uint32_t pieceAtFrame(double f) const noexcept {
    const WarpSeg* g = segs();
    return count < 2 ? 0u : warpPieceOf(count, f, [g](uint32_t k) { return g[k].f0; });
  }
  double frameAt(double x) const noexcept { return segs()[pieceAt(x)].frame(x); }
  double contentAt(double f) const noexcept { return segs()[pieceAtFrame(f)].content(f); }
  double slopeAt(double x) const noexcept { return segs()[pieceAt(x)].slope; }
  // The loop: content x ≥ loopEnd plays loopStart + (x − loopEnd) mod span; folds: how many spans that took off.
  void setLoop(double start, double end) noexcept {
    looping = end > start;
    loopStart = start;
    loopEnd = end;
    loopStartF = looping ? std::llround(frameAt(start)) : 0;
    loopEndF = looping ? std::llround(frameAt(end)) : 0;
  }
  double fold(double x) const noexcept {
    if (!looping || x < loopEnd) return x;
    return loopStart + std::fmod(x - loopEnd, loopEnd - loopStart);
  }
  int64_t folds(double x) const noexcept {
    if (!looping || x < loopEnd) return 0;
    return static_cast<int64_t>(std::floor((x - loopEnd) / (loopEnd - loopStart))) + 1;
  }
  // folds when a position exactly on a fold boundary belongs to the pass below it (a reverse reader's passes).
  int64_t foldsBelow(double x) const noexcept {
    if (!looping || x <= loopEnd) return 0;
    return static_cast<int64_t>(std::ceil((x - loopEnd) / (loopEnd - loopStart)));
  }
  // The source frame of content x as one continuous stream: every fold adds the loop's span in whole frames.
  double unfoldedFrameAt(double x) const noexcept {
    const double f = frameAt(fold(x));
    return loopEndF > loopStartF ? f + static_cast<double>(folds(x) * (loopEndF - loopStartF)) : f;
  }
  // ... and back (SM.16, the Complex stream): the file frame of the whole unfolded frame u (a loop of at least one
  // frame: loopsFrames()).
  bool loopsFrames() const noexcept { return looping && loopEndF > loopStartF; }
  int64_t foldFrame(int64_t u) const noexcept {
    if (!loopsFrames() || u < loopEndF) return u;
    return loopStartF + (u - loopEndF) % (loopEndF - loopStartF);
  }
};

// The Sampler's markers — (frame, beat) float pairs, both strictly ascending (SampleSlot sanitises them) — as the
// law's segments over beats: empty with fewer than two markers. Not RT-safe (allocates).
std::vector<WarpSeg> warpBeatSegments(std::span<const float> pairs);
// The law over them: `segs` (warpBeatSegments of the pairs; none with fewer than two markers: then one segment at
// fpb frames per beat through the lone marker, or through (0, 0)). RT-safe.
inline WarpLaw warpBeatLaw(const WarpSeg* segs, uint32_t segCount, const float* pairs, uint32_t markers, double fpb) noexcept {
  WarpLaw law;
  if (segs != nullptr && segCount > 0) {
    law.seg = segs;
    law.count = segCount;
  } else {
    law.one = warpMarkerSeg(markers != 0 && pairs != nullptr ? 1u : 0u, 0, fpb, [pairs](uint32_t i) {
      return std::pair<double, double>(pairs[2 * i + 1], pairs[2 * i]);
    });
  }
  return law;
}
// The same for editors and drawing (owns its segments). Not RT-safe.
struct WarpBeatLaw {
  std::vector<WarpSeg> store;
  WarpLaw law;
  WarpBeatLaw(std::span<const float> pairs, double fpb)
      : store(warpBeatSegments(pairs)),
        law(warpBeatLaw(store.data(), static_cast<uint32_t>(store.size()), pairs.data(), static_cast<uint32_t>(pairs.size() / 2), fpb)) {}
  WarpBeatLaw(const WarpBeatLaw&) = delete;
  WarpBeatLaw& operator=(const WarpBeatLaw&) = delete;
};

// ---- Gains --------------------------------------------------------------------------------------------------------------
// The periodic Hann window of N frames (N even), w(j) = sin²(π j / N) (= 0.5 − 0.5·cos(2πj / N), within 2e-7;
// Σ_k w(j − k·N/2) = 1 within one rounding): out[i] = w(j0 + i) · (env ? env[i] : gain), i < n, 0 ≤ j0, j0 + n ≤ N. A pure function of j and N (no table,
// no recurrence), so a frame's gain does not depend on where a call starts. The one window: the clip player's image
// caches it per grain size (hannWindow, read back by warpWindowGain); the Sampler's voice, whose grain size is only
// known at its trigger on the audio thread, calls it per grain piece. RT-safe, AVX2.
void warpHannGain(uint32_t N, int64_t j0, const float* env, float gain, float* out, uint32_t n) noexcept;
// out[i] = win[i] · (env ? env[i] : gain), i < n (win: hannWindow's cache). RT-safe, AVX2.
void warpWindowGain(const float* win, const float* env, float gain, float* out, uint32_t n) noexcept;
// A ramp is anchored on a sample t0: its value at t0 + j is clamp(fma(j, dg, base), 0, 1) (j exact as a float below
// 2^24), so a sample's gain does not depend on where a block starts (renders are bit-identical whatever the block).
// rampValue(base, dg, j); warpRampGain: out[i] = rampValue(base, dg, k + i) · (env ? env[i] : gain), i < n;
// warpRampMul: out[i] *= rampValue(base, dg, k + i). RT-safe, AVX2.
float rampValue(float base, float dg, int32_t j) noexcept;
void warpRampGain(float base, float dg, int32_t k, const float* env, float gain, float* out, uint32_t n) noexcept;
void warpRampMul(float base, float dg, int32_t k, float* out, uint32_t n) noexcept;

// warpHannGain's window of n frames (n even), cached. Not RT-safe (allocates).
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
