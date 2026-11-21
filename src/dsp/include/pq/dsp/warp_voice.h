#pragma once

// The per-voice time-stretch renderer (SM.4; docs/plans/SM.md "Where it sits", "Semantics"): Beats / Tones / Texture
// moved out of the clip player (pq/engine/clip_player.h, T6.3) so the Sampler's `sampler_warp` atom plays a sample
// with the same grains and slices. Two parts:
//
// ---- The shared renderers (templates over a HOST) -------------------------------------------------------------------
// warpRenderGrains (Tones / Texture: grain k covers [origin + k·H, origin + k·H + N), centred at origin + (k + 1)·H,
// reads around P_k = the host's law at its centre, Texture jittered, Tones aligned with grain k − 1 (WSOLA:
// warpAlignGrain)) and warpRenderSlices / warpRenderPass (Beats: the slice passes under a run, each a linear read at
// the pitch rate with its gains — fade in, Envelope, the 2 ms crossfade, a loop's declick — fused into the read).
// The host supplies what is client-specific:
//   kClock                  false: a constant pitch rate (the clip player: positions anchor + (t − t0)·step, read as
//                           constant-step pieces); true: a per-sample rate (WarpVoice below: positions anchor +
//                           dir·(Φ(t) − Φ(t0)), Φ the integrated 32.32 rate, read at per-frame positions)
//   lawFrame(t)             the source frame the voice's time law puts at output sample t (a grain's centre)
//   grainRate(gk)           a grain's rate, its Tones alignment length / lags and Texture jitter (the clip: constants)
//   source / read / keep    the source access (the clip: resident or streamed readers; WarpVoice: a resident sample)
//   passAt / prevPass / nextPass / passReader   Beats' pass enumeration (constant-rate hosts only)
// Every position is anchored at a sample that depends on the voice alone (a grain's start, a pass's entry), so renders
// are bit-identical whatever the block. The clip player's renders are bit-identical to before the move
// (tests/engine/test_warp.cpp "SM.4").
//
// ---- WarpVoice (the Sampler's voice) ---------------------------------------------------------------------------------
// One voice over a RESIDENT sample (a SampleSlot entry: its mip levels, slices = Beats' transients, markers), from a
// trigger on. Time law, anchored at the note-on: content beats since the trigger b(t) = b_a + (t − t_a)·bpm / (60·sr)
// (re-anchored only when the tempo changes, so a constant tempo never drifts) → content beat q = beat(start) + b(t)
// (folded into the loop's beats while looping) → source frame through the markers' piecewise-linear law (no markers:
// sampleBpm beats per minute of the file; one marker: that tempo through it; beyond the ends the end segments
// continue). Pitch: the per-sample rate r(t) (file frames per output frame) integrated into Φ(t); a grain reads
// anchor + Φ(t) − Φ(its start), a slice F + Φ(t) − Φ(its entry): a glide or vibrato moves every reader continuously.
// Reads are cubic (sample_read's interpolator) at a mip level chosen per grain / slice from its rate at its start.
// Beats here evaluates the pass geometry per sample in the clock domain (source frames since the entry): the natural
// read, the crossfade into the next slice, Off's fade into the natural end, Forward / Back-Forward loops of the second
// half and their declicks — at a constant rate the same law as the clip's closed form. The next slice's entry is
// predicted from the current tempo each run and fixed once reached.
// End: without a loop the content stops at `end` (a 2 ms fade from the sample it is reached; playing() falls after it).
// RT-safe: everything noexcept, no allocation; the state is POD (an atom's lane state).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

#include "pq/dsp/sample_slot.h"
#include "pq/dsp/warp.h"

namespace pq::dsp {

inline constexpr int64_t kWarpOne = int64_t{1} << 32;

inline int64_t warpToFixed(double v) noexcept { return static_cast<int64_t>(std::llround(v * 4294967296.0)); }  // RT-safe
// A 32.32 source position; at pitch rate 1 on a whole frame (a slice / grain then reads as a straight copy). RT-safe.
inline int64_t warpFixed(double x, int64_t pitchStep) noexcept {
  return pitchStep == kWarpOne ? static_cast<int64_t>(std::llround(x)) * kWarpOne : warpToFixed(x);
}
inline int64_t warpFloorDiv(int64_t a, int64_t b) noexcept {  // RT-safe; b > 0
  return a >= 0 ? a / b : -((-a + b - 1) / b);
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

enum class WarpSliceLoop : uint8_t { Off, Forward, BackForward };  // = engine::SliceLoop

// One pass of one slice (Beats).
struct WarpSlicePass {
  int64_t T = 0;              // entry (output sample)
  int64_t next = 0;           // the next pass's entry (INT64_MAX: none)
  double F = 0.0;             // source frame at T
  double L = 0.0;             // natural length in source frames (+inf: to the clip's end)
  double entryU = 0.0, exitU = 0.0;  // the host's bounds of the slice (the clip: unfolded content; WarpVoice: beats)
  int dir = 1;
  bool fadeIn = true;         // false: the first pass (none before it)
  // WarpVoice (clock) only:
  bool terminal = false;      // the content's end: renders nothing
  uint32_t level = 0;         // mip level (from the rate at the entry)
  int64_t phiT = 0;           // Φ at T
  int64_t natEnd = INT64_MAX; // the first sample whose read reached L (INT64_MAX: not yet)
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
  int64_t step = kWarpOne;  // 32.32 source frames per output frame (> 0)
  double rate = 1.0;
  uint32_t alignLen = 0, alignLags = 0;  // Tones (0 lags: no alignment)
  double jitter = 0.0;                   // Texture: ± source frames
};

struct WarpGrainParams {
  int64_t origin = 0;           // grain k covers [origin + k·hop, origin + k·hop + grain)
  uint32_t hop = 0, grain = 0;  // H = N / 2, N
  int64_t dir = 1;              // ±1 (reverse)
  const float* window = nullptr;  // the periodic Hann window of N (constant-rate hosts; clock hosts: h.hannGain)
  float gain = 1.0f;
  uint32_t seed = 0;            // Texture's jitter
  uint32_t sourceRate = 48000;  // the source's frames per second (Tones' coarse stride)
  uint32_t channels = 1;
};

struct WarpSliceParams {
  int64_t fade = 96;     // the slice crossfade X (output frames)
  double rate = 1.0;     // r (constant-rate hosts)
  int64_t step = kWarpOne;
  WarpSliceLoop loop = WarpSliceLoop::Off;
  float envelope = 100.0f;
  float gain = 1.0f;
};

// Per-call buffers: gain ≥ the run's frames; corrA ≥ kWarpMaxAlignLen + 64, corrB ≥ kWarpMaxAlignLen +
// 2·kWarpMaxAlignLags + 64 (Tones' alignment); gather (WarpVoice only): 2 × kWarpGatherFrames (alignment reads past
// a resident sample's guards, zero-padded).
inline constexpr uint32_t kWarpMaxAlignLen = 2048;  // Tones: source frames compared per grain at most
inline constexpr uint32_t kWarpMaxAlignLags = 960;  // Tones: ± source frames searched at most
inline constexpr uint32_t kWarpGatherFrames = 4096;
struct WarpScratch {
  float* gain = nullptr;
  float* corrA = nullptr;
  float* corrB = nullptr;
  float* gather = nullptr;
};

// Tones' alignment and Texture's jitter for a grain of hop H at rate r (audio_sequence.cpp's rule): Tones aligns when
// H·r ≥ 16 over min(H·r, kWarpMaxAlignLen) frames, ± min(H·r / 2, 12 ms of the source, kWarpMaxAlignLags).
inline void warpAlignFor(double hr, double sourceRate, uint32_t* len, uint32_t* lags) noexcept {  // RT-safe
  *len = *lags = 0;
  if (!(hr >= 16.0)) return;
  *len = static_cast<uint32_t>(std::min<double>(std::round(hr), kWarpMaxAlignLen));
  *lags = static_cast<uint32_t>(
      std::min<double>({std::floor(hr / 2.0), std::floor(0.012 * sourceRate), static_cast<double>(kWarpMaxAlignLags)}));
}

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
template <class H>
double warpGrainCentre(H& h, WarpGrains& g, const WarpGrainParams& P, int64_t k, const WarpScratch& sc) noexcept {  // RT-safe
  if (g.idx != INT64_MIN && k <= g.idx && k >= g.idx - 1) return g.pos[k & 1];
  const int64_t from = g.idx != INT64_MIN && k > g.idx && k - g.idx <= 64 ? g.idx + 1 : k;
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
      g.anchor[j & 1] = gr.alignLags != 0 ? warpToFixed(a0) : warpFixed(a0, gr.step);
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
  const int64_t kLo = warpFloorDiv(s - g0 - N, Hh) + 1;
  const int64_t kHi = warpFloorDiv(s + n - 1 - g0, Hh);
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
    if constexpr (!H::kClock) warpWindowGain(P.window + (a - gk), env != nullptr ? env + at : nullptr, P.gain, gb, frames);
    else h.hannGain(N, a - gk, P.gain, gb, frames);
    float* o[2] = {out[0] + at, out[1] + at};
    if constexpr (!H::kClock) {
      // The grain reads P + dir·(t − C_k)·r: anchored at its first sample (it depends on the grain alone).
      const WarpGrainRate gr = h.grainRate(gk);
      const int64_t step = dir * gr.step;
      const double hr = static_cast<double>(Hh) * gr.rate;
      const double a0 = Pk - static_cast<double>(dir) * hr;
      const int64_t anchor = gr.alignLags != 0 ? warpToFixed(a0) : warpFixed(a0, gr.step);
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

// ---- Beats (constant-rate hosts: the clip player) ------------------------------------------------------------------

template <class H>
void warpRenderPass(H& h, const WarpSliceParams& S, const WarpSlicePass& p, int64_t s, uint32_t n, float* const* out,
                    const float* env, const WarpScratch& sc) noexcept {  // RT-safe
  const int64_t Xi = S.fade;
  const auto X = static_cast<float>(Xi);
  const int64_t audEnd = p.next == INT64_MAX ? INT64_MAX : p.next + Xi;
  const int64_t a0 = std::max(s, p.T), b0 = std::min(s + n, audEnd);
  if (a0 >= b0) return;
  h.countPass();
  const double r = S.rate;
  const int64_t step = p.dir * S.step;
  const bool finite = std::isfinite(p.L);
  const double tE = finite ? p.L / r : 0.0;  // the natural end, output frames after T
  const double Lh = 0.5 * p.L;
  const double lam = finite ? Lh / r : 0.0;
  // A slice the next one cuts before (or within the crossfade of) its natural end plays on naturally through its
  // fade-out: at the source's own tempo the two read the same frames there, so the crossfade is seamless.
  const int64_t natEnd0 = finite ? p.T + static_cast<int64_t>(std::ceil(tE)) : INT64_MAX;
  const bool cut = !finite || (p.next != INT64_MAX && natEnd0 >= p.next - Xi);
  const bool loops = !cut && S.loop != WarpSliceLoop::Off && lam >= 2.0 * static_cast<double>(Xi);
  const int64_t natEnd = cut ? INT64_MAX : natEnd0;
  auto& rd = h.passReader(p);
  const int64_t keep = h.keep();
  const float envGd = S.envelope < 100.0f && p.next != INT64_MAX
                          ? std::max(1.0f, static_cast<float>(p.next - p.T) * (1.0f - S.envelope / 100.0f))
                          : 0.0f;
  // Renders [a, b) reading from 32.32 `pos` by `st`, with the pass's gains and `extra` ramps.
  auto emit = [&](int64_t a, int64_t b, int64_t pos, int64_t st, const WarpRamp* extra, int nExtra) noexcept {
    const auto len = static_cast<uint32_t>(b - a);
    WarpRamp rs[8];
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
    if (p.fadeIn) audible = audible && add({p.T, 0.5f / X, 1.0f / X});
    if (p.next != INT64_MAX) audible = audible && add({p.next, (X - 0.5f) / X, -1.0f / X});
    if (envGd > 0.0f) audible = audible && add({p.next, 0.0f, -1.0f / envGd});
    for (int i = 0; i < nExtra; ++i) audible = audible && add(extra[i]);
    if (!audible) return;
    const auto at = static_cast<uint32_t>(a - s);
    const float* g = env != nullptr ? env + at : nullptr;
    if (nr > 0) {
      float* gb = sc.gain + at;
      warpRampGain(rs[0].base, rs[0].dg, warpRampIndex(a, rs[0].t0), g, S.gain, gb, len);
      for (int i = 1; i < nr; ++i) warpRampMul(rs[i].base, rs[i].dg, warpRampIndex(a, rs[i].t0), gb, len);
      g = gb;
    }
    WarpPiece pc;
    pc.at = at;
    pc.frames = len;
    pc.pos = pos;
    pc.step = st;
    float* o[2] = {out[0] + at, out[1] + at};
    h.read(rd, pc, o, g, S.gain, keep);
  };
  // The natural part: from F at the pitch rate, anchored at the entry.
  {
    const int64_t a = a0, b = std::min(b0, natEnd);
    if (a < b) {
      const int64_t pos = static_cast<int64_t>(static_cast<uint64_t>(warpFixed(p.F, S.step)) +
                                               static_cast<uint64_t>(a - p.T) * static_cast<uint64_t>(step));
      WarpRamp ex[1];
      int ne = 0;
      // Off: fade out into the natural end; Forward: declick into the first loop jump.
      if (!cut && (!loops || S.loop == WarpSliceLoop::Forward))
        ex[ne++] = {p.T, static_cast<float>((tE - 0.5) / static_cast<double>(Xi)), -1.0f / X};
      emit(a, b, pos, step, ex, ne);
    }
  }
  if (!loops) return;
  // Loop iterations over the slice's second half: iteration i starts tE + i·λ after the entry.
  const double from = std::max(0.0, std::floor((static_cast<double>(a0 - p.T) - tE) / lam));
  for (auto i = static_cast<int64_t>(from); i < static_cast<int64_t>(from) + 64; ++i) {
    const double ti0 = tE + static_cast<double>(i) * lam, ti1 = tE + static_cast<double>(i + 1) * lam;
    const int64_t t0 = p.T + static_cast<int64_t>(std::ceil(ti0)), t1 = p.T + static_cast<int64_t>(std::ceil(ti1));
    if (t0 >= b0) break;
    const int64_t a = std::max(a0, t0), b = std::min(b0, t1);
    if (a >= b) continue;
    const double frac = std::ceil(ti0) - ti0;  // output frames between the iteration's start and its first sample
    double off = Lh + frac * r;
    int64_t st = step;
    if (S.loop == WarpSliceLoop::BackForward && (i & 1) == 0) {  // even iterations run back from the end
      off = p.L - frac * r;
      st = -step;
    }
    const int64_t anchor = warpFixed(p.F + static_cast<double>(p.dir) * off, S.step);
    const int64_t pos = static_cast<int64_t>(static_cast<uint64_t>(anchor) + static_cast<uint64_t>(a - t0) * static_cast<uint64_t>(st));
    WarpRamp ex[2];
    int ne = 0;
    if (S.loop == WarpSliceLoop::Forward) {  // declick both sides of each jump
      ex[ne++] = {t0, static_cast<float>((static_cast<double>(t0 - p.T) - ti0 + 0.5) / static_cast<double>(Xi)), 1.0f / X};
      ex[ne++] = {t0, static_cast<float>((ti1 - static_cast<double>(t0 - p.T) - 0.5) / static_cast<double>(Xi)), -1.0f / X};
    }
    emit(a, b, pos, st, ex, ne);
  }
}

// The slice passes under the run [s, s + n): the one sounding, the one fading out behind it, the ones starting.
// Returns the last pass rendered (the clip player claims the next one's stream from it).
template <class H>
WarpSlicePass warpRenderSlices(H& h, const WarpSliceParams& S, int64_t s, uint32_t n, float* const* out, const float* env,
                               const WarpScratch& sc) noexcept {  // RT-safe
  const int64_t X = S.fade;
  WarpSlicePass p = h.passAt(s);
  WarpSlicePass q;
  if (s < p.T + X && h.prevPass(p, &q)) warpRenderPass(h, S, q, s, n, out, env, sc);  // still fading out
  for (uint32_t guard = 0; guard < 4096; ++guard) {
    warpRenderPass(h, S, p, s, n, out, env, sc);
    if (p.next == INT64_MAX || p.next >= s + n) break;
    p = h.nextPass(p);
  }
  return p;
}

// ---- WarpVoice --------------------------------------------------------------------------------------------------------

enum class WarpVoiceMode : uint8_t { Beats, Tones, Texture };

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
  int32_t length() const noexcept { return level[0].length; }
};

// What a run plays (the atom's param-rate inputs, already in frames).
struct WarpVoiceRun {
  WarpVoiceMode mode = WarpVoiceMode::Tones;
  double start = 0.0, end = 0.0;          // frames; the content stops at end (without a loop)
  double loopStart = 0.0, loopEnd = 0.0;  // frames; loopEnd > loopStart: the content loops there
  double bpm = 120.0, sampleBpm = 120.0;
  double sampleRate = 48000.0;            // the engine's
  double grainSeconds = 0.03;             // Tones / Texture (taken at the trigger)
  double flux = 0.0;                      // Texture: % of the hop
  WarpSliceLoop sliceLoop = WarpSliceLoop::Forward;
  float envelope = 100.0f;                // Beats: %
};

inline constexpr uint32_t kWarpVoiceChunk = 128;  // frames per inner run (the Φ prefix, positions and gains)

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
  uint32_t mode = 0;    // the WarpVoiceMode it was triggered in
  WarpGrains grains;
  WarpSlicePass cur, prev;

  // Starts the voice at the next frame (t = 0).
  void trigger(const WarpSource& src, const WarpVoiceRun& run) noexcept;  // RT-safe
  void stop() noexcept { active = 0; }  // RT-safe
  // Renders n frames (adds nothing when inactive): out L / R (written, not added), the read position in [0, 1] of the
  // sample (pos, may be null), playing (1 / 0 per frame, may be null). rate: per frame, file-natural (× rateScale).
  // SM.5: outL null advances a Tones / Texture voice (its clock, Φ, the tempo anchor, the end) without rendering.
  void render(const WarpSource& src, const WarpVoiceRun& run, const float* rate, uint32_t n, float* outL, float* outR,
              float* pos, float* playing, const WarpScratch& sc) noexcept;  // RT-safe
  // SM.5 (Complex): the law's source frame at output time t (fractional; before the trigger the law extrapolated),
  // unfolded through the loop (every wrap adds the loop's span in whole frames: WarpUnfold). RT-safe.
  double unfoldedAt(const WarpSource& src, const WarpVoiceRun& run, double t) const noexcept;
  // The end fade and Beats' slice crossfade: 2 ms.
  static int64_t fadeFrames(double sampleRate) noexcept {
    return std::max<int64_t>(1, std::lround(0.002 * sampleRate));
  }
};
static_assert(std::is_trivially_copyable_v<WarpVoice>);

// SM.5 (Complex): the run's region and loop in whole frames: an unfolded frame x ≥ loopEnd plays the file at
// loopStart + (x − loopEnd) mod (loopEnd − loopStart) while looping. RT-safe.
struct WarpUnfold {
  bool looping = false;
  int64_t loopStart = 0, loopEnd = 0;
  double startF = 0.0, endF = 0.0;
};
WarpUnfold warpUnfold(const WarpSource& src, const WarpVoiceRun& run) noexcept;

// The source frame of content beat q / the beat of source frame f (WarpSource's markers; fpb = frames per beat at
// sampleBpm). RT-safe.
double warpFrameOfBeat(const WarpSource& src, double q, double fpb) noexcept;
double warpBeatOfFrame(const WarpSource& src, double f, double fpb) noexcept;

}  // namespace pq::dsp
