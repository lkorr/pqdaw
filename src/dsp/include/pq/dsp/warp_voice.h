#pragma once

// The per-voice time-stretch renderer (SM.4, SM.15; docs/plans/SM.md "Where it sits", "Rework 2026-11-23"): Beats /
// Tones / Texture, one implementation for the clip player (pq/engine/clip_player.h, T6.3) and the Sampler's
// `sampler_warp` atom. Two parts:
//
// ---- The shared renderers (templates over a HOST) -------------------------------------------------------------------
// warpRenderGrains (Tones / Texture: grain k covers [origin + k·H, origin + k·H + N), centred at origin + (k + 1)·H,
// reads around P_k = the host's law at its centre, Texture jittered, Tones aligned with grain k − 1 (WSOLA:
// warpAlignGrain)) and warpRenderPass (Beats: one slice pass under a run). The host supplies what is client-specific:
//   kClock                  false: a constant pitch rate (the clip player: positions anchor + (t − t0)·step, read as
//                           constant-step pieces); true: a per-sample rate (WarpVoice below: positions anchor +
//                           dir·(Φ(t) − Φ(t0)), Φ the integrated 32.32 rate, read at per-frame positions)
//   lawFrame(t)             the source frame the voice's time law puts at output sample t (a grain's centre)
//   grainRate(gk)           a grain's rate, its Tones alignment length / lags and Texture jitter
//   source / read / keep    the source access (the clip: resident or streamed sinc readers; WarpVoice: cubic reads
//                           of a resident sample)
//   Beats: clock(p, t) = Φ(t) − Φ(p.T), constStep(&st), passReader(p), and the pass enumeration (not shared: the
//   clip's slices live in song time, the voice's follow the live tempo)
// Every position is anchored at a sample that depends on the voice alone (a grain's start, a pass's entry), so renders
// are bit-identical whatever the block.
//
// ---- Beats (one renderer) ---------------------------------------------------------------------------------------------
// Everything a pass plays at output sample t is a pure function of t and the integer clock d = Φ(t) − Φ(T) (32.32
// source frames read since the entry T), with the pass's constants F (the frame at T), L (its natural length), X (the
// 2 ms fade in output frames), step0 (the step at the entry) and next (the next pass's entry):
//   natural part   d < L: reads F + dir·d. Its natural end is the first sample with d ≥ L (natEnd).
//   cut            the next pass enters before (or within X of) the natural end — judged before the end from the
//                  clock left at step0 (t + ⌈(L − d) / step0⌉ ≥ next − X), after it from natEnd: the slice reads on
//                  naturally through its crossfade (at the source's tempo the two read the same frames: seamless).
//   not cut        Off, or a second half shorter than 2X at step0: fades out into the end, then silent. Forward:
//                  the second half [L − Lh, L) repeats from the end on (iteration i from the first sample with
//                  d ≥ L + i·Lh), each jump declicked on both sides; Back-Forward: even iterations run back from L,
//                  odd ones forward from L − Lh (continuous: no declick).
//   gains          time ramps (rampValue of a sample index: the fade in from T, the crossfade out from next, the
//                  Envelope decay into next) × clock fades (functions of the clock left / done in output frames at
//                  step0: into the natural end, around a Forward jump).
// A run whose step is constant and equal to step0 takes the fast path: the events (the natural end, each iteration)
// are solved by integer division, the pass is emitted as constant-step pieces with vector ramps (the clock fades,
// a few frames per event, through the same scalar functions) — equal to evaluating every sample (the test
// "SM.15: the Beats constant-step pieces equal the per-sample evaluation"), so a render does not depend on the block
// size or on where a glide starts and stops. The clip player always takes it (its rate is constant); Envelope < 100
// is a time ramp, so it stays on it.
//
// ---- WarpVoice (the Sampler's voice) ---------------------------------------------------------------------------------
// One voice over a RESIDENT sample (a SampleSlot entry: its mip levels, slices = Beats' transients, markers), from a
// trigger on. Time law, anchored at the note-on: content beats since the trigger b(t) = b_a + (t − t_a)·bpm / (60·sr)
// (re-anchored only when the tempo changes, so a constant tempo never drifts) → content beat q = beat(start) + b(t)
// (folded into the loop's beats while looping) → source frame through the markers' law (warp.h WarpLaw over the
// entry's segments; no markers: sampleBpm beats per minute of the file; one marker: that tempo through it; beyond
// the ends the end segments continue). Pitch: the per-sample rate r(t) (file frames per output frame) integrated
// into Φ(t); a grain reads anchor + Φ(t) − Φ(its start), a slice by its clock: a glide or vibrato moves every reader
// continuously. Reads are cubic (sample_read's interpolator) at a mip level chosen per grain / slice from its rate
// at its start. The next slice's entry is predicted from the current tempo each run and fixed once reached.
// End: without a loop the content stops at `end` (a 2 ms fade from the sample it is reached; playing() falls after it).
// RT-safe: everything noexcept, no allocation; the state is POD (an atom's lane state).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

#include "pq/dsp/fixed.h"
#include "pq/dsp/sample_slot.h"
#include "pq/dsp/warp.h"

namespace pq::dsp {

// A 32.32 source position; at pitch rate 1 on a whole frame (a slice / grain then reads as a straight copy). RT-safe.
inline int64_t warpFixed(double x, int64_t pitchStep) noexcept {
  return pitchStep == kFixedOne ? static_cast<int64_t>(std::llround(x)) * kFixedOne : toFixed(x);
}
// A gain factor clamp(base + (t − t0)·dg, 0, 1) (warp.h: anchored on a sample, so block independent).
struct WarpRamp {
  int64_t t0;
  float base, dg;
};
inline int32_t warpRampIndex(int64_t t, int64_t t0) noexcept {  // RT-safe
  return static_cast<int32_t>(std::clamp<int64_t>(t - t0, -(int64_t{1} << 30), int64_t{1} << 30));
}

// One contiguous constant-step piece of a run: output frames [at, at + frames), 32.32 source position and step.
struct WarpPiece {
  uint32_t at, frames;
  int64_t pos, step;
};

// One pass of one slice (Beats). The host enumerates them (the clip: its image's slices in song time; WarpVoice:
// the sample's slice table under the live tempo); warpRenderPass renders one.
struct WarpSlicePass {
  int64_t T = 0;              // entry (output sample)
  int64_t next = 0;           // the next pass's entry (INT64_MAX: none)
  double F = 0.0;             // source frame at T
  double L = 0.0;             // natural length in source frames (+inf: to the clip's end)
  double entryU = 0.0, exitU = 0.0;  // the host's bounds of the slice (the clip: unfolded content; WarpVoice: beats)
  int dir = 1;
  bool fadeIn = true;         // false: the first pass (none before it)
  bool terminal = false;      // the content's end: renders nothing
  uint32_t level = 0;         // the reader's mip level (WarpVoice: from the rate at the entry)
  int64_t step0 = 0;          // the 32.32 step at the entry (> 0): the scale of the pass's fades and loop rule
  int64_t phiT = 0;           // clock hosts: Φ at T
  int64_t natEnd = INT64_MAX; // clock hosts: the first sample whose clock reached L (INT64_MAX: not yet)
};

// Tones / Texture: the last grain positioned and its centre (and the one before, by parity); clock hosts also keep
// each one's anchor, Φ at its start and its mip level.
struct WarpGrains {
  int64_t idx = INT64_MIN;
  double pos[2] = {0.0, 0.0};
  int64_t anchor[2] = {0, 0};
  int64_t phi[2] = {0, 0};
  uint32_t level[2] = {0, 0};
};

struct WarpGrainRate {
  int64_t step = kFixedOne;  // 32.32 source frames per output frame (> 0)
  double rate = 1.0;
  uint32_t alignLen = 0, alignLags = 0;  // Tones (0 lags: no alignment; warp.h warpAlignFor)
  double jitter = 0.0;                   // Texture: ± source frames (warp.h warpJitterFrames)
};

struct WarpGrainParams {
  int64_t origin = 0;           // grain k covers [origin + k·hop, origin + k·hop + grain)
  uint32_t hop = 0, grain = 0;  // H = N / 2, N
  int64_t dir = 1;              // ±1 (reverse)
  const float* window = nullptr;  // hannWindow(N), cached (the clip's image); null: warpHannGain per piece
  float gain = 1.0f;
  uint32_t seed = 0;            // Texture's jitter
  uint32_t sourceRate = 48000;  // the source's frames per second (Tones' coarse stride)
  uint32_t channels = 1;
};

struct WarpSliceParams {
  int64_t fade = 96;     // the slice crossfade X (output frames; warp.h warpFadeFrames)
  WarpSliceLoop loop = WarpSliceLoop::Off;
  float envelope = 100.0f;
  float gain = 1.0f;
  bool perSample = false;  // tests: clock hosts never take the constant-step pieces (the two must be equal)
};

// Per-call buffers: gain ≥ the run's frames; corrA ≥ kWarpMaxAlignLen + 64, corrB ≥ kWarpMaxAlignLen +
// 2·kWarpMaxAlignLags + 64 (Tones' alignment); gather (WarpVoice only): 2 × kWarpGatherFrames (alignment reads past
// a resident sample's guards, zero-padded).
inline constexpr uint32_t kWarpGatherFrames = 4096;
struct WarpScratch {
  float* gain = nullptr;
  float* corrA = nullptr;
  float* corrB = nullptr;
  float* gather = nullptr;
  uint32_t* grains = nullptr;  // tests (WarpVoice): counts the grains positioned
};

// ---- Tones / Texture --------------------------------------------------------------------------------------------------

// Tones (WSOLA): the centre near p whose overlap with grain k − 1 (centred at prev) continues its waveform best: the
// mono sum of the source over the overlap, compared at a coarse stride (≈ 12 kHz) over ±alignLags, then refined frame
// by frame around the best coarse lag.
template <class H>
double warpAlignGrain(H& h, const WarpGrainParams& P, const WarpGrainRate& gr, double prev, int64_t p,
                      const WarpScratch& sc) noexcept {  // RT-safe
  const int64_t dir = P.dir;
  const auto hr = static_cast<int64_t>(std::llround(static_cast<double>(P.hop) * gr.rate));
  const auto Lc = static_cast<int64_t>(gr.alignLen), D = static_cast<int64_t>(gr.alignLags);
  const int64_t prevI = std::llround(prev);
  // The match is between whole frames: the exact continuation is prev + dir·H·r, so the fractions are added back.
  const double fix = (prev - static_cast<double>(prevI)) +
                     static_cast<double>(dir) * (static_cast<double>(P.hop) * gr.rate - static_cast<double>(hr));
  // Grain k − 1 reads [prev, prev + hr) over the overlap (reverse: below prev), grain k [p − hr, p) (reverse: above p).
  const int64_t refStart = (dir > 0 ? prevI : prevI - hr) + (hr - Lc) / 2;
  const int64_t candStart = (dir > 0 ? p - hr : p) + (hr - Lc) / 2;
  const auto stride = static_cast<int64_t>(std::clamp<uint32_t>(P.sourceRate / 12000u, 1u, 8u));
  const int64_t Dc = D / stride * stride;
  const auto refN = static_cast<uint32_t>(Lc / stride);
  const auto lags = static_cast<uint32_t>(2 * Dc / stride + 1);
  const uint32_t ch = std::min<uint32_t>(P.channels, 2);
  const int64_t keep = h.keep();
  const float* base[2] = {nullptr, nullptr};
  int64_t bf = 0;
  const auto pd = static_cast<double>(p);
  if (refN < 4) return pd;
  if (!h.source(refStart, refStart + (refN - 1) * stride + 1, dir > 0, keep, base, &bf)) return pd;
  warpMonoDecimate(base, ch, refStart - bf, static_cast<uint32_t>(stride), refN, sc.corrA);
  const int64_t cFirst = candStart - Dc;
  const uint32_t candN = refN + lags - 1;
  if (!h.source(cFirst, cFirst + (static_cast<int64_t>(candN) - 1) * stride + 1, dir > 0, keep, base, &bf)) return pd;
  warpMonoDecimate(base, ch, cFirst - bf, static_cast<uint32_t>(stride), candN, sc.corrB);
  int64_t d = static_cast<int64_t>(warpBestLag(sc.corrA, sc.corrB, refN, lags, static_cast<uint32_t>(Dc / stride))) *
                  stride - Dc;
  // Frame by frame around it (a frame further each way, so the parabola has both neighbours), then the fraction.
  const int64_t lo = std::max(-D, d - stride), hi = std::min(D, d + stride);
  const auto len = static_cast<uint32_t>(Lc);
  const double coarse = pd + static_cast<double>(d) + fix;
  if (!h.source(refStart, refStart + Lc, dir > 0, keep, base, &bf)) return coarse;
  warpMonoDecimate(base, ch, refStart - bf, 1, len, sc.corrA);
  const auto fineN = static_cast<uint32_t>(Lc + (hi - lo));
  if (!h.source(candStart + lo, candStart + lo + fineN, dir > 0, keep, base, &bf)) return coarse;
  warpMonoDecimate(base, ch, candStart + lo - bf, 1, fineN, sc.corrB);
  double frac = 0.0;
  d = lo + static_cast<int64_t>(warpBestLag(sc.corrA, sc.corrB, len, static_cast<uint32_t>(hi - lo + 1),
                                            static_cast<uint32_t>(d - lo), &frac));
  return pd + static_cast<double>(d) + frac + fix;
}

// The source frame grain k is centred on: the law at its centre (Texture: jittered), Tones: moved by the lag that
// continues grain k − 1. Grains are positioned once, in order (the state keeps the last two). A clock host also gets
// each grain's 32.32 anchor (at its first sample), Φ there and its mip level (h.grainAnchor).
// A host that skipped grains (H::kCatchUp: the clip 64; the Sampler's voice 1 — it stops positioning grains while
// its vocoder sounds, and a voice that lost its stretcher must not align every grain it skipped in one call, SM.16)
// catches up through at most that many, aligning each; further behind, grain k starts afresh at the law's position.
template <class H>
double warpGrainCentre(H& h, WarpGrains& g, const WarpGrainParams& P, int64_t k, const WarpScratch& sc) noexcept {  // RT-safe
  if (g.idx != INT64_MIN && k <= g.idx && k >= g.idx - 1) return g.pos[k & 1];
  const int64_t from = g.idx != INT64_MIN && k > g.idx && k - g.idx <= H::kCatchUp ? g.idx + 1 : k;
  const auto Hh = static_cast<int64_t>(P.hop);
  for (int64_t j = from; j <= k; ++j) {
    const int64_t gj = P.origin + j * Hh;
    const int64_t centre = gj + Hh;
    const WarpGrainRate gr = h.grainRate(gj);
    double Pc = h.lawFrame(centre);
    if (gr.jitter > 0.0) Pc += warpJitter(P.seed, j) * gr.jitter;
    const int64_t q = std::llround(Pc);
    // Tones: fractional (an aligned grain continues the one before to a fraction of a frame); Texture: whole frames.
    Pc = gr.alignLags != 0 && g.idx == j - 1 ? warpAlignGrain(h, P, gr, g.pos[(j - 1) & 1], q, sc) : static_cast<double>(q);
    g.pos[j & 1] = Pc;
    if constexpr (H::kClock) {
      const double a0 = Pc - static_cast<double>(P.dir) * static_cast<double>(P.hop) * gr.rate;
      g.anchor[j & 1] = gr.alignLags != 0 ? toFixed(a0) : warpFixed(a0, gr.step);
      g.phi[j & 1] = h.phiAt(gj);
      g.level[j & 1] = h.levelFor(gr.rate);
    }
    g.idx = j;
    h.countGrain();
  }
  return g.pos[k & 1];
}

// The grains under the run [s, s + n) added into out (env: per-frame gains, null: P.gain).
template <class H>
void warpRenderGrains(H& h, WarpGrains& g, const WarpGrainParams& P, int64_t s, uint32_t n, float* const* out,
                      const float* env, const WarpScratch& sc) noexcept {  // RT-safe
  const auto Hh = static_cast<int64_t>(P.hop), N = static_cast<int64_t>(P.grain);
  const int64_t g0 = P.origin;  // grain k covers [g0 + k·H, g0 + k·H + N)
  const int64_t kLo = floorDiv(s - g0 - N, Hh) + 1;
  const int64_t kHi = floorDiv(s + n - 1 - g0, Hh);
  const int64_t dir = P.dir;
  const int64_t keep = h.keep();
  for (int64_t k = kLo; k <= kHi; ++k) {
    const double Pk = warpGrainCentre(h, g, P, k, sc);
    const int64_t gk = g0 + k * Hh;
    const int64_t a = std::max(s, gk), b = std::min(s + n, gk + N);
    if (a >= b) continue;
    const auto at = static_cast<uint32_t>(a - s);
    const auto frames = static_cast<uint32_t>(b - a);
    float* gb = sc.gain + at;
    if (P.window != nullptr) warpWindowGain(P.window + (a - gk), env != nullptr ? env + at : nullptr, P.gain, gb, frames);
    else warpHannGain(P.grain, a - gk, env != nullptr ? env + at : nullptr, P.gain, gb, frames);
    float* o[2] = {out[0] + at, out[1] + at};
    if constexpr (!H::kClock) {
      // The grain reads P + dir·(t − C_k)·r: anchored at its first sample (it depends on the grain alone).
      const WarpGrainRate gr = h.grainRate(gk);
      const int64_t step = dir * gr.step;
      const double hr = static_cast<double>(Hh) * gr.rate;
      const double a0 = Pk - static_cast<double>(dir) * hr;
      const int64_t anchor = gr.alignLags != 0 ? toFixed(a0) : warpFixed(a0, gr.step);
      WarpPiece p;
      p.at = at;
      p.frames = frames;
      p.pos = static_cast<int64_t>(static_cast<uint64_t>(anchor) + static_cast<uint64_t>(a - gk) * static_cast<uint64_t>(step));
      p.step = step;
      h.read(h.grainReader(), p, o, gb, P.gain, keep);
    } else {
      (void)Pk;
      h.readClock(g.anchor[k & 1], dir, g.phi[k & 1], g.level[k & 1], a, frames, o, gb);
    }
  }
}

// ---- Beats (see the top: one renderer, a constant-step fast path equal to the per-sample evaluation) -----------------

// A pass's constants.
struct WarpBeats {
  int64_t T = 0, next = 0, X = 1;
  int64_t dir = 1;
  int64_t step0 = 1;     // > 0
  int64_t F32 = 0;       // the frame at T
  int64_t L32 = 0;       // the natural length (INT64_MAX: none)
  int64_t Lh32 = 0;      // the looped second half's length; it starts at loopAt = L32 − Lh32
  int64_t loopAt = 0;
  int64_t zoneOut = 0, zoneIn = 0;  // the clock left / done inside which a clock fade can be below 1
  bool hasNext = false, fadeIn = false;
  bool loopsOk = false;  // the slice loops past its natural end
  bool fwd = false;      // … forward (declicked jumps); else back and forth
  bool outFade = false;  // the natural part fades into its end (Off, and Forward's first jump)
  float envGd = 0.0f;    // > 0: Envelope's decay length (output frames before next)
  float Xf = 1.0f;
  double invStep = 1.0, iX = 1.0;
};

inline WarpBeats warpBeats(const WarpSliceParams& S, const WarpSlicePass& p) noexcept {  // RT-safe
  WarpBeats B;
  B.T = p.T;
  B.next = p.next;
  B.X = S.fade;
  B.dir = p.dir;
  B.step0 = std::max<int64_t>(p.step0, 1);
  B.hasNext = p.next != INT64_MAX;
  B.fadeIn = p.fadeIn;
  const bool finite = std::isfinite(p.L);
  B.F32 = warpFixed(p.F, B.step0);
  B.L32 = finite ? std::max<int64_t>(0, warpFixed(p.L, B.step0)) : INT64_MAX;
  // (At pitch rate 1 the half is whole frames too: every loop pass is a straight copy.)
  B.Lh32 = !finite ? 0 : B.step0 == kFixedOne ? (B.L32 >> 33) << 32 : B.L32 >> 1;
  B.loopAt = finite ? B.L32 - B.Lh32 : 0;
  B.loopsOk = S.loop != WarpSliceLoop::Off && B.Lh32 > 0 && B.Lh32 >= 2 * B.X * B.step0;
  B.fwd = S.loop == WarpSliceLoop::Forward;
  B.outFade = !B.loopsOk || B.fwd;
  B.zoneOut = (B.X + 1) * B.step0;
  B.zoneIn = B.X * B.step0;
  B.envGd = S.envelope < 100.0f && B.hasNext ? std::max(1.0f, static_cast<float>(p.next - p.T) * (1.0f - S.envelope / 100.0f))
                                             : 0.0f;
  B.Xf = static_cast<float>(B.X);
  B.invStep = 1.0 / static_cast<double>(B.step0);
  B.iX = 1.0 / static_cast<double>(B.X);
  return B;
}

// The clock fades: 1 until c (the clock left before an event) is within X + ½ output frames at step0, 0 from ½ on;
// and its mirror from an event on (w: the clock done since). Exactly 1 outside zoneOut / zoneIn. RT-safe.
inline float warpClockOut(const WarpBeats& B, int64_t c) noexcept {
  return static_cast<float>(std::clamp((static_cast<double>(c) * B.invStep - 0.5) * B.iX, 0.0, 1.0));
}
inline float warpClockIn(const WarpBeats& B, int64_t w) noexcept {
  return static_cast<float>(std::clamp((static_cast<double>(w) * B.invStep + 0.5) * B.iX, 0.0, 1.0));
}

// The pass's frames [a0, b0) of the run [s, …) evaluated sample by sample (clock hosts: a rate that moves).
template <class H>
void warpBeatsPerSample(H& h, const WarpBeats& B, WarpSlicePass& p, int64_t s, int64_t a0, int64_t b0, float* const* out,
                        const float* env, float gain, const WarpScratch& sc) noexcept {  // RT-safe
  alignas(32) int64_t pos[H::kMaxRun];
  float* g = sc.gain + (a0 - s);
  const float inBase = 0.5f / B.Xf, inDg = 1.0f / B.Xf, outBase = (B.Xf - 0.5f) / B.Xf, outDg = -1.0f / B.Xf;
  const float envDg = B.envGd > 0.0f ? -1.0f / B.envGd : 0.0f;
  for (int64_t t = a0; t < b0; ++t) {
    const auto i = static_cast<size_t>(t - a0);
    const int64_t d = h.clock(p, t);
    float gi = env != nullptr ? env[t - s] : gain;
    if (B.fadeIn) gi *= rampValue(inBase, inDg, warpRampIndex(t, B.T));
    if (B.hasNext) gi *= rampValue(outBase, outDg, warpRampIndex(t, B.next));
    if (B.envGd > 0.0f) gi *= rampValue(0.0f, envDg, warpRampIndex(t, B.next));
    int64_t at = B.F32 + B.dir * d;
    if (d < B.L32) {
      const int64_t c = B.L32 - d;
      if (B.outFade && c <= B.zoneOut) {
        const bool cut = B.hasNext && t + ceilDiv(c, B.step0) >= B.next - B.X;
        if (!cut) gi *= warpClockOut(B, c);
      }
    } else {
      if (p.natEnd == INT64_MAX) p.natEnd = t;
      const bool cut = B.hasNext && p.natEnd >= B.next - B.X;
      if (!cut) {
        if (!B.loopsOk) {
          gi = 0.0f;  // silent after its fade
        } else {
          const int64_t w = d - B.L32;
          const int64_t it = w / B.Lh32;
          const int64_t wi = w - it * B.Lh32;
          if (B.fwd) {
            at = B.F32 + B.dir * (B.loopAt + wi);
            if (wi < B.zoneIn) gi *= warpClockIn(B, wi);
            if (B.Lh32 - wi <= B.zoneOut) gi *= warpClockOut(B, B.Lh32 - wi);
          } else {
            at = (it & 1) == 0 ? B.F32 + B.dir * (B.L32 - wi) : B.F32 + B.dir * (B.loopAt + wi);
          }
        }
      }  // cut: the natural read continues through the crossfade
    }
    g[i] = gi;
    pos[i] = at;
  }
  float* o[2] = {out[0] + (a0 - s), out[1] + (a0 - s)};
  h.readAt(p, pos, g, static_cast<uint32_t>(b0 - a0), o);
}

// The same frames at a constant step st = step0: constant-step pieces between the events.
template <class H>
void warpBeatsPieces(H& h, const WarpSliceParams& S, const WarpBeats& B, WarpSlicePass& p, int64_t st, int64_t s, int64_t a0,
                     int64_t b0, float* const* out, const float* env, const WarpScratch& sc) noexcept {  // RT-safe
  auto&& rd = h.passReader(p);
  const int64_t keep = h.keep();
  // Renders [a, b) reading from 32.32 `pos` by `pst`; cOut ≥ 0: the clock left before a fade-out's event at a (it
  // falls by st per frame); wIn ≥ 0: the clock done since a fade-in's event at a.
  auto emit = [&](int64_t a, int64_t b, int64_t pos, int64_t pst, int64_t cOut, int64_t wIn) noexcept {
    const auto len = static_cast<uint32_t>(b - a);
    WarpRamp rs[3];
    int nr = 0;
    auto add = [&](WarpRamp q) noexcept {
      const float first = rampValue(q.base, q.dg, warpRampIndex(a, q.t0));
      const float last = rampValue(q.base, q.dg, warpRampIndex(b - 1, q.t0));
      if (std::min(first, last) >= 1.0f) return true;   // 1 over the piece: no factor
      if (std::max(first, last) <= 0.0f) return false;  // 0 over the piece: silent
      rs[nr++] = q;
      return true;
    };
    bool audible = true;
    if (B.fadeIn) audible = audible && add({B.T, 0.5f / B.Xf, 1.0f / B.Xf});
    if (B.hasNext) audible = audible && add({B.next, (B.Xf - 0.5f) / B.Xf, -1.0f / B.Xf});
    if (B.envGd > 0.0f) audible = audible && add({B.next, 0.0f, -1.0f / B.envGd});
    if (!audible) return;
    const auto at = static_cast<uint32_t>(a - s);
    const float* g = env != nullptr ? env + at : nullptr;
    float* gb = sc.gain + at;
    bool own = false;
    if (nr > 0) {
      warpRampGain(rs[0].base, rs[0].dg, warpRampIndex(a, rs[0].t0), g, S.gain, gb, len);
      for (int i = 1; i < nr; ++i) warpRampMul(rs[i].base, rs[i].dg, warpRampIndex(a, rs[i].t0), gb, len);
      own = true;
    }
    auto ensure = [&]() noexcept {
      if (own) return;
      if (g != nullptr) std::copy_n(g, len, gb);
      else std::fill_n(gb, len, S.gain);
      own = true;
    };
    if (wIn >= 0 && wIn < B.zoneIn) {
      const auto n1 = static_cast<uint32_t>(std::min<int64_t>(len, ceilDiv(B.zoneIn - wIn, st)));
      ensure();
      for (uint32_t i = 0; i < n1; ++i) gb[i] *= warpClockIn(B, wIn + static_cast<int64_t>(i) * st);
    }
    if (cOut >= 0) {
      const int64_t i0 = cOut > B.zoneOut ? ceilDiv(cOut - B.zoneOut, st) : 0;
      if (i0 < static_cast<int64_t>(len)) {
        ensure();
        for (auto i = static_cast<uint32_t>(i0); i < len; ++i) gb[i] *= warpClockOut(B, cOut - static_cast<int64_t>(i) * st);
      }
    }
    WarpPiece pc;
    pc.at = at;
    pc.frames = len;
    pc.pos = pos;
    pc.step = pst;
    float* o[2] = {out[0] + at, out[1] + at};
    h.read(rd, pc, o, own ? gb : g, S.gain, keep);
  };
  const int64_t dA = h.clock(p, a0);
  int64_t tA = a0;  // where the part past the natural end starts
  if (dA < B.L32) {
    const int64_t tN = B.L32 == INT64_MAX ? INT64_MAX : a0 + ceilDiv(B.L32 - dA, st);  // the first sample with d ≥ L
    const bool cut = B.L32 == INT64_MAX || (B.hasNext && tN >= B.next - B.X);
    if constexpr (H::kClock)
      if (tN < b0) p.natEnd = tN;
    const int64_t bN = cut ? b0 : std::min(b0, tN);
    emit(a0, bN, B.F32 + B.dir * dA, B.dir * st, !cut && B.outFade ? B.L32 - dA : -1, -1);
    tA = bN;
  } else {
    int64_t natEnd = B.T + ceilDiv(B.L32, st);  // (a constant-rate host: the step never moved)
    if constexpr (H::kClock) {
      if (p.natEnd == INT64_MAX) p.natEnd = a0;
      natEnd = p.natEnd;
    }
    if (B.hasNext && natEnd >= B.next - B.X) {  // cut: the natural read goes on
      emit(a0, b0, B.F32 + B.dir * dA, B.dir * st, -1, -1);
      return;
    }
  }
  if (tA >= b0 || !B.loopsOk) return;
  int64_t w = h.clock(p, tA) - B.L32;
  for (int64_t t = tA; t < b0;) {
    const int64_t it = w / B.Lh32;
    const int64_t wi = w - it * B.Lh32;
    const int64_t b = std::min(b0, t + ceilDiv(B.Lh32 - wi, st));
    const bool forward = B.fwd || (it & 1) != 0;
    emit(t, b, forward ? B.F32 + B.dir * (B.loopAt + wi) : B.F32 + B.dir * (B.L32 - wi), forward ? B.dir * st : -B.dir * st,
         B.fwd ? B.Lh32 - wi : -1, B.fwd ? wi : -1);
    w += (b - t) * st;
    t = b;
  }
}

// One pass's frames under the run [s, s + n), added into out (env: per-frame gains, null: S.gain). p.step0 is set.
template <class H>
void warpRenderPass(H& h, const WarpSliceParams& S, WarpSlicePass& p, int64_t s, uint32_t n, float* const* out,
                    const float* env, const WarpScratch& sc) noexcept {  // RT-safe
  if (p.terminal) return;
  const int64_t audEnd = p.next == INT64_MAX ? INT64_MAX : p.next + S.fade;
  const int64_t a0 = std::max(s, p.T), b0 = std::min(s + n, audEnd);
  if (a0 >= b0) return;
  h.countPass();
  const WarpBeats B = warpBeats(S, p);
  int64_t st = 0;
  const bool pieces = h.constStep(&st) && st == B.step0;
  if constexpr (H::kClock) {
    if (pieces && !S.perSample) warpBeatsPieces(h, S, B, p, st, s, a0, b0, out, env, sc);
    else warpBeatsPerSample(h, B, p, s, a0, b0, out, env, S.gain, sc);
  } else {
    (void)pieces;  // (its step is the pass's, always)
    warpBeatsPieces(h, S, B, p, B.step0, s, a0, b0, out, env, sc);
  }
}

// A constant-rate host's slice passes under the run [s, s + n) (the host enumerates them: passAt / prevPass /
// nextPass): the one sounding, the one fading out behind it, the ones starting. `step`: the host's 32.32 pitch step.
// Returns the last pass rendered (the clip player claims the next one's stream from it).
template <class H>
WarpSlicePass warpRenderSlices(H& h, const WarpSliceParams& S, int64_t step, int64_t s, uint32_t n, float* const* out,
                               const float* env, const WarpScratch& sc) noexcept {  // RT-safe
  const int64_t X = S.fade;
  WarpSlicePass p = h.passAt(s);
  p.step0 = step;
  WarpSlicePass q;
  if (s < p.T + X && h.prevPass(p, &q)) {  // still fading out
    q.step0 = step;
    warpRenderPass(h, S, q, s, n, out, env, sc);
  }
  for (uint32_t guard = 0; guard < 4096; ++guard) {
    warpRenderPass(h, S, p, s, n, out, env, sc);
    if (p.next == INT64_MAX || p.next >= s + n) break;
    p = h.nextPass(p);
    p.step0 = step;
  }
  return p;
}

// ---- WarpVoice --------------------------------------------------------------------------------------------------------

// A resident sample (a SampleSlot entry's views: readable on [−kSampleSlotGuard, length + kSampleSlotGuard)).
struct WarpSource {
  const SampleLevelView* level = nullptr;  // levels + 1 views
  uint32_t levels = 0;
  uint32_t channels = 1;
  double fileRate = 48000.0;     // frames per second
  double rateScale = 1.0;        // file rate / engine rate (rate 1 = the file's own speed)
  const float* slices = nullptr; // Beats' boundaries (start frames, ascending, in [0, length))
  uint32_t sliceCount = 0;
  const float* markers = nullptr;  // (frame, beat) pairs, both strictly ascending
  uint32_t markerCount = 0;        // pairs
  const WarpSeg* law = nullptr;    // the markers as segments over beats (the entry's; none with fewer than two)
  uint32_t lawCount = 0;
  int32_t length() const noexcept { return level[0].length; }
};
// The entry's views (RT-safe).
inline WarpSource warpSourceOf(const SampleSlot::Entry& e) noexcept {
  WarpSource s;
  s.level = e.level;
  s.levels = e.levels;
  s.channels = e.channels;
  s.fileRate = e.fileRate;
  s.rateScale = e.rateScale;
  s.slices = e.slices;
  s.sliceCount = e.sliceCount;
  s.markers = e.markers;
  s.markerCount = e.markerCount;
  s.law = e.law;
  s.lawCount = e.lawCount;
  return s;
}

// What a run plays (the atom's param-rate inputs, already in frames).
struct WarpVoiceRun {
  AudioWarp mode = AudioWarp::Tones;        // Beats / Tones / Texture (Complex / Complex Pro: Tones under the vocoder)
  double start = 0.0, end = 0.0;          // frames; the content stops at end (without a loop)
  double loopStart = 0.0, loopEnd = 0.0;  // frames; loopEnd > loopStart: the content loops there
  double bpm = 120.0, sampleBpm = 120.0;
  double sampleRate = 48000.0;            // the engine's
  WarpSettings set;                       // grainSeconds (taken at the trigger), flux, sliceLoop, envelope, formants
  bool perSample = false;                 // tests: Beats never takes the constant-step pieces
};

// A run's law, built once per run record (the kernel: per lane and call, and at a reset), never per render: the
// markers' law over beats with the loop, and the region in frames.
struct WarpRunLaw {
  WarpLaw law;                    // beats → frames; looping: the loop in beats and whole frames
  double startF = 0.0, endF = 0.0;
  double lsF = 0.0, leF = 0.0;    // the loop in frames
  // Beats' next boundary after frame F (a slice start, the loop's end, the end). RT-safe.
  double nextBoundary(const WarpSource& src, double F) const noexcept;
};
WarpRunLaw warpRunLaw(const WarpSource& src, const WarpVoiceRun& run) noexcept;  // RT-safe

inline constexpr uint32_t kWarpVoiceChunk = 128;  // frames per inner run (the Φ prefix, positions and gains)

// The read clock over one inner run of m ≤ kWarpVoiceChunk frames: Φ at each frame (ph[i], i ≤ m), each frame's rate
// (file frames per output frame) and 32.32 step. Integrated ONCE per run and shared by everything that reads on it
// (the voice's grains / slices, the Complex voice's vocoder read: SM.16).
struct WarpClock {
  alignas(32) int64_t ph[kWarpVoiceChunk + 1];
  alignas(32) int64_t st[kWarpVoiceChunk];  // (flat: only [0] is written; read through step() / rate())
  alignas(32) double rt[kWarpVoiceChunk];
  bool constant;  // every step of the run is the same
  bool flat;      // every rate of the run is the same value (no glide: the common case)
  // rate: per frame, file-natural (× scale = file frames per output frame); phi: Φ at the first frame. A flat run
  // converts its rate once; Φ is the same sums either way. RT-safe.
  void integrate(int64_t phi, const float* rate, uint32_t m, double scale) noexcept;
  int64_t step(uint32_t i) const noexcept { return st[flat ? 0u : i]; }  // RT-safe
  double rate(uint32_t i) const noexcept { return rt[flat ? 0u : i]; }   // RT-safe
};

// The Sampler's per-voice warp renderer (see the top of the file). Trivially copyable: it is a lane's state.
struct WarpVoice {
  int64_t t = 0;        // output samples since the trigger (of the next frame)
  int64_t phi = 0;      // Φ(t): 32.32 source frames read since the trigger
  int64_t ta = 0;       // the law's anchor: b(t) = ba + (t − ta)·inc content beats since the trigger
  double ba = 0.0, inc = 0.0;
  double beat0 = 0.0;   // the content beat of the start frame
  int64_t endT = INT64_MAX;  // the first sample the content reached its end (predicted until reached)
  uint32_t active = 0;  // 1: sounding (until endT + the fade)
  uint32_t hop = 0;     // Tones / Texture: H (grain 2H), taken at the trigger
  uint32_t hasPrev = 0;
  uint32_t mode = 0;    // the AudioWarp it was triggered in
  WarpGrains grains;
  WarpSlicePass cur, prev;

  // Starts the voice at the next frame (t = 0).
  void trigger(const WarpSource& src, const WarpVoiceRun& run, const WarpRunLaw& law) noexcept;  // RT-safe
  void stop() noexcept { active = 0; }  // RT-safe
  // Renders n frames (adds nothing when inactive): out L / R (written, not added), the read position in [0, 1] of the
  // sample (pos, may be null), playing (1 / 0 per frame, may be null). rate: per frame, file-natural (× rateScale).
  void render(const WarpSource& src, const WarpVoiceRun& run, const WarpRunLaw& law, const float* rate, uint32_t n,
              float* outL, float* outR, float* pos, float* playing, const WarpScratch& sc) noexcept;  // RT-safe
  // One inner run of m ≤ kWarpVoiceChunk frames on a clock the caller integrated from this voice's phi (render() is a
  // loop over these). SM.5: outL null advances a voice (its clock, Φ, the tempo anchor, the end) without rendering.
  void renderClock(const WarpSource& src, const WarpVoiceRun& run, const WarpRunLaw& law, const WarpClock& ck, uint32_t m,
                   float* outL, float* outR, float* pos, float* playing, const WarpScratch& sc) noexcept;  // RT-safe
  // SM.5 (Complex): the law's source frame at output time t (fractional; before the trigger the law extrapolated),
  // unfolded through the loop (every wrap adds the loop's span in whole frames: WarpLaw::unfoldedFrameAt). RT-safe.
  double unfoldedAt(const WarpRunLaw& law, double tt) const noexcept {
    return law.law.unfoldedFrameAt(beat0 + ba + (tt - static_cast<double>(ta)) * inc);
  }
};
static_assert(std::is_trivially_copyable_v<WarpVoice>);

}  // namespace pq::dsp
