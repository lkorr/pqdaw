// The Sampler's warp voice (SM.4, SM.15): pq/dsp/warp_voice.h "WarpVoice".
#include "pq/dsp/warp_voice.h"

#include <immintrin.h>

#include "pq/dsp/resample.h"
#include "simd.h"

namespace pq::dsp {

WarpRunLaw warpRunLaw(const WarpSource& src, const WarpVoiceRun& run) noexcept {  // RT-safe
  WarpRunLaw L;
  const double fpb = 60.0 * src.fileRate / std::max(run.sampleBpm, 1.0);  // frames per beat at sampleBpm
  L.law = warpBeatLaw(src.law, src.lawCount, src.markers, src.markerCount, fpb);
  const auto len = static_cast<double>(src.length());
  L.lsF = std::clamp(run.loopStart, 0.0, len);
  L.leF = std::clamp(run.loopEnd, 0.0, len);
  if (L.leF > L.lsF + 1.0) {
    L.law.setLoop(L.law.contentAt(L.lsF), L.law.contentAt(L.leF));
    L.law.loopStartF = std::llround(L.lsF);
    L.law.loopEndF = std::llround(L.leF);
  }
  L.startF = std::clamp(run.start, 0.0, len);
  if (L.law.looping && L.startF >= L.leF) L.startF = L.lsF;  // a start past the loop plays the loop
  L.endF = std::clamp(run.end, 0.0, len);
  return L;
}

double WarpRunLaw::nextBoundary(const WarpSource& src, double F) const noexcept {  // RT-safe
  const double limit = law.looping ? leF : endF;
  const float* S = src.slices;
  const uint32_t n = src.sliceCount;
  // first slice > F (+ a hair: F is a boundary itself)
  const float* it = std::upper_bound(S, S + n, static_cast<float>(F + 1e-3));
  const double b = it != S + n ? static_cast<double>(*it) : limit;
  return std::min(b, limit);
}

// ---- the clock host (warp_voice.h "The shared renderers") --------------------------------------------------------------

namespace {

struct VoiceHost {
  static constexpr bool kClock = true;
  static constexpr uint32_t kMaxRun = kWarpVoiceChunk;
  WarpVoice& v;
  const WarpSource& src;
  const WarpVoiceRun& run;
  const WarpRunLaw& law;
  int64_t s = 0;              // the chunk's first sample (since the trigger)
  uint32_t n = 0;
  const int64_t* ph = nullptr;  // Φ(s + i), i ≤ n
  const double* rt = nullptr;   // the rate at s + i, i < n
  const int64_t* st = nullptr;  // the 32.32 step at s + i
  const WarpScratch* sc = nullptr;
  bool constant = false;        // every step of the chunk is the same
  static constexpr int64_t kGather = kWarpGatherFrames;

  int64_t keep() const noexcept { return 0; }
  double rateAt(int64_t t) const noexcept { return rt[std::clamp<int64_t>(t - s, 0, static_cast<int64_t>(n) - 1)]; }
  int64_t stepAt(int64_t t) const noexcept { return st[std::clamp<int64_t>(t - s, 0, static_cast<int64_t>(n) - 1)]; }
  double lawFrame(int64_t t) const noexcept {
    return law.law.frameAt(law.law.fold(v.beat0 + v.ba + static_cast<double>(t - v.ta) * v.inc));
  }
  WarpGrainRate grainRate(int64_t gk) const noexcept {
    WarpGrainRate r;
    r.rate = rateAt(gk);
    r.step = stepAt(gk);
    const double hr = static_cast<double>(v.hop) * r.rate;
    if (static_cast<AudioWarp>(v.mode) == AudioWarp::Texture) r.jitter = warpJitterFrames(run.set.flux, hr);
    else warpAlignFor(hr, src.fileRate, &r.alignLen, &r.alignLags);
    return r;
  }
  int64_t phiAt(int64_t t) const noexcept {
    if (t >= s) return ph[std::min<int64_t>(t - s, n)];
    return ph[0] - (s - t) * st[0];  // a grain that started before the trigger: extrapolated at the first rate
  }
  uint32_t levelFor(double rate) const noexcept { return mipLevelFor(rate, src.levels); }
  void countGrain() noexcept {}
  void countPass() noexcept {}
  // Beats (warp_voice.h): the pass's clock at t in [s, s + n], the chunk's step when it is constant.
  int64_t clock(const WarpSlicePass& p, int64_t t) const noexcept { return ph[t - s] - p.phiT; }
  bool constStep(int64_t* step) const noexcept {
    *step = st[0];
    return constant;
  }
  uint32_t passReader(const WarpSlicePass& p) const noexcept { return p.level; }
  // Level-0 frames [first, last): in place inside the guards, else gathered with zeros (alignment reads only).
  bool source(int64_t first, int64_t last, bool /*forward*/, int64_t /*keep*/, const float** base, int64_t* bf) noexcept {
    const SampleLevelView& lv = src.level[0];
    base[0] = lv.ch[0];
    base[1] = lv.ch[1];
    *bf = 0;
    if (first >= -kSampleSlotGuard && last <= static_cast<int64_t>(lv.length) + kSampleSlotGuard) return true;
    if (last - first > kGather || last <= first) return false;
    const int64_t a = std::max<int64_t>(first, 0), b = std::min<int64_t>(last, lv.length);
    for (uint32_t c = 0; c < 2; ++c) {
      float* dst = sc->gather + c * kGather;
      std::memset(dst, 0, sizeof(float) * static_cast<size_t>(last - first));
      if (b > a) std::memcpy(dst + (a - first), lv.ch[c] + a, sizeof(float) * static_cast<size_t>(b - a));
      base[c] = dst;
    }
    *bf = first;
    return true;
  }
  // out[c][i] += source[i0 + i] · g[i]: whole frames at rate 1 on level 0 (the cubic there is exactly the frame).
  void copyAt(int64_t i0, const float* g, uint32_t frames, float* const* out) const noexcept {
    const SampleLevelView& lv = src.level[0];
    const float* ch[2] = {lv.ch[0], src.channels > 1 ? lv.ch[1] : lv.ch[0]};
    for (uint32_t c = 0; c < 2; ++c) {
      const float* x = ch[c] + i0;
      float* o = out[c];
      uint32_t i = 0;
      for (; i + 8 <= frames; i += 8)
        _mm256_storeu_ps(o + i, _mm256_fmadd_ps(_mm256_loadu_ps(x + i), _mm256_loadu_ps(g + i), _mm256_loadu_ps(o + i)));
      for (; i < frames; ++i) o[i] = _mm_cvtss_f32(_mm_fmadd_ss(_mm_set_ss(x[i]), _mm_set_ss(g[i]), _mm_set_ss(o[i])));
    }
  }
  bool copyable(int64_t pos0, uint32_t frames) const noexcept {
    const int64_t i0 = pos0 >> 32;
    return (pos0 & 0xFFFFFFFF) == 0 && i0 >= -kSampleSlotGuard &&
           i0 + frames <= static_cast<int64_t>(src.level[0].length) + kSampleSlotGuard;
  }
  // out[c][i] += cubic(source at pos[i] on `level`) · g[i] (pos: level-0 32.32; one rounding per add, as fmadd), 8
  // frames at a time (AVX2 gathers; a short tail runs the same vector code on a padded copy, so a frame's value does
  // not depend on where a call splits). Positions whose taps all lie in the zeros beyond the data add nothing. A run
  // of consecutive whole frames on level 0 (rate 1) is a straight copy: the cubic there is exactly the frame anyway.
  void readAt(const int64_t* pos, uint32_t level, const float* g, uint32_t frames, float* const* out) const noexcept {
    if (frames == 0) return;
    const SampleLevelView& lv = src.level[level];
    const int64_t len = lv.length;
    const float* ch[2] = {lv.ch[0], src.channels > 1 ? lv.ch[1] : lv.ch[0]};
    if (level == 0 && copyable(pos[0], frames)) {
      bool lin = true;
      for (uint32_t i = 1; i < frames && lin; ++i) lin = pos[i] == pos[0] + static_cast<int64_t>(i) * kFixedOne;
      if (lin) {
        copyAt(pos[0] >> 32, g, frames, out);
        return;
      }
    }
    for (uint32_t i = 0; i < frames; i += 8) {
      const uint32_t m = std::min<uint32_t>(8, frames - i);
      alignas(32) int32_t ix[8] = {};
      alignas(32) float fr[8] = {}, gm[8] = {};
      bool any = false;
      for (uint32_t k = 0; k < m; ++k) {
        const int64_t x = pos[i + k] >> level;  // arithmetic: positions may be negative
        const int64_t idx = x >> 32;
        if (g[i + k] == 0.0f || idx < -3 || idx > len + 1) continue;
        ix[k] = static_cast<int32_t>(idx);
        fr[k] = static_cast<float>(static_cast<uint32_t>(x)) * 2.3283064365386963e-10f;  // 2^-32
        gm[k] = g[i + k];
        any = true;
      }
      if (!any) continue;
      const __m256i vi = _mm256_load_si256(reinterpret_cast<const __m256i*>(ix));
      const __m256 t = _mm256_load_ps(fr), gv = _mm256_load_ps(gm);
      for (uint32_t c = 0; c < 2; ++c) {
        const float* d = ch[c];
        const __m256 y = simd::cubic4<simd::S8>(_mm256_i32gather_ps(d - 1, vi, 4), _mm256_i32gather_ps(d, vi, 4),
                                                _mm256_i32gather_ps(d + 1, vi, 4), _mm256_i32gather_ps(d + 2, vi, 4), t);
        float* o = out[c] + i;
        if (m == 8) {
          _mm256_storeu_ps(o, _mm256_fmadd_ps(y, gv, _mm256_loadu_ps(o)));
        } else {
          alignas(32) float buf[8] = {};
          for (uint32_t k = 0; k < m; ++k) buf[k] = o[k];
          _mm256_store_ps(buf, _mm256_fmadd_ps(y, gv, _mm256_load_ps(buf)));
          for (uint32_t k = 0; k < m; ++k) o[k] = buf[k];
        }
      }
    }
  }
  // A grain's frames: positions anchor + dir·(Φ(t) − Φ(its start)).
  void readClock(int64_t anchor, int64_t dir, int64_t phiK, uint32_t level, int64_t a, uint32_t frames, float* const* o,
                 const float* gb) const noexcept {
    alignas(32) int64_t pos[kWarpVoiceChunk];
    for (uint32_t i = 0; i < frames; ++i)
      pos[i] = static_cast<int64_t>(static_cast<uint64_t>(anchor) +
                                    static_cast<uint64_t>(dir) * static_cast<uint64_t>(ph[a - s + i] - phiK));
    readAt(pos, level, gb, frames, o);
  }
  // Beats: a pass's frames at per-frame positions, and a constant-step piece of them (g null: `gain`).
  void readAt(const WarpSlicePass& p, const int64_t* pos, const float* g, uint32_t frames, float* const* o) const noexcept {
    readAt(pos, p.level, g, frames, o);
  }
  void read(uint32_t level, const WarpPiece& pc, float* const* o, const float* g, float gain, int64_t /*keep*/) const noexcept {
    if (g == nullptr) {
      float* gb = sc->gain + pc.at;
      std::fill_n(gb, pc.frames, gain);
      g = gb;
    }
    if (level == 0 && pc.step == kFixedOne && copyable(pc.pos, pc.frames)) {
      copyAt(pc.pos >> 32, g, pc.frames, o);
      return;
    }
    alignas(32) int64_t pos[kWarpVoiceChunk];
    for (uint32_t i = 0; i < pc.frames; ++i) pos[i] = fixedAt(pc.pos, static_cast<int64_t>(i), pc.step);
    readAt(pos, level, g, pc.frames, o);
  }
};

// A Beats pass's entry step at least (rate 1/64): a slice entered while the rate is (near) 0 still gets fades and a
// loop rule of a sensible length once the rate rises.
constexpr int64_t kMinEntryStep = kFixedOne / 64;

// The first sample (since the trigger) at which the content has advanced U beats (b(t) ≥ U), under the current law.
int64_t timeOfBeats(const WarpVoice& v, double U) noexcept {  // RT-safe
  if (!(v.inc > 0.0)) return INT64_MAX;
  const double x = static_cast<double>(v.ta) + (U - v.ba) / v.inc;
  if (!(x < 9.0e18)) return INT64_MAX;
  return static_cast<int64_t>(std::ceil(x - 1e-6));
}

// A Beats pass entering at content frame F (a boundary, or the start) at sample T; step0 0: taken at its first run.
WarpSlicePass makePass(const WarpSource& src, const WarpRunLaw& law, int64_t T, int64_t phiT, double F, double entryU,
                       bool fadeIn, uint32_t level, int64_t step0) noexcept {
  WarpSlicePass p;
  p.T = T;
  p.phiT = phiT;
  p.F = F;
  const double bF = law.nextBoundary(src, F);
  p.L = std::max(0.0, bF - F);
  p.entryU = entryU;
  p.exitU = entryU + (law.law.contentAt(bF) - law.law.contentAt(F));
  p.fadeIn = fadeIn;
  p.level = level;
  p.step0 = step0;
  p.dir = 1;
  return p;
}

// The pass after p (entering at p.next): the boundary's frame, folded to the loop start at the loop's end; at the end
// (no loop) a terminal pass.
WarpSlicePass passAfter(const WarpSource& src, const WarpRunLaw& law, const WarpSlicePass& p, int64_t phiT, uint32_t level,
                        int64_t step0) noexcept {
  const double bF = p.F + p.L;
  if (!law.law.looping && bF >= law.endF - 1e-6) {
    WarpSlicePass t;
    t.T = p.next;
    t.next = INT64_MAX;
    t.terminal = true;
    t.phiT = phiT;
    t.F = bF;
    t.entryU = t.exitU = p.exitU;
    return t;
  }
  const double F = law.law.looping && bF >= law.leF - 1e-6 ? law.lsF : bF;
  return makePass(src, law, p.next, phiT, F, p.exitU, true, level, step0);
}

}  // namespace

void WarpVoice::trigger(const WarpSource& src, const WarpVoiceRun& run, const WarpRunLaw& law) noexcept {  // RT-safe
  t = 0;
  phi = 0;
  ta = 0;
  ba = 0.0;
  inc = std::max(run.bpm, 1.0) / (60.0 * run.sampleRate);
  beat0 = law.law.contentAt(law.startF);
  mode = static_cast<uint32_t>(run.mode);
  hop = warpGrainFrames(run.set.grainSeconds, run.sampleRate) / 2;
  grains = {};
  hasPrev = 0;
  prev = {};
  active = src.length() > 0 && (law.law.looping || law.endF > law.startF) ? 1u : 0u;
  endT = law.law.looping ? INT64_MAX : timeOfBeats(*this, law.law.contentAt(law.endF) - beat0);
  cur = {};
  if (run.mode == AudioWarp::Beats) cur = makePass(src, law, 0, 0, law.startF, 0.0, false, 0, 0);
}

void WarpVoice::render(const WarpSource& src, const WarpVoiceRun& run, const WarpRunLaw& law, const float* rate, uint32_t n,
                       float* outL, float* outR, float* pos, float* playing, const WarpScratch& sc) noexcept {  // RT-safe
  const bool audible = outL != nullptr;
  if (audible) {
    std::fill_n(outL, n, 0.0f);
    std::fill_n(outR, n, 0.0f);
  }
  const auto len = static_cast<double>(src.length());
  if (active == 0 || len <= 0.0) {
    active = 0;
    if (pos != nullptr) std::fill_n(pos, n, 0.0f);
    if (playing != nullptr) std::fill_n(playing, n, 0.0f);
    return;
  }
  const int64_t X = warpFadeFrames(run.sampleRate);
  // The tempo: re-anchored only when it changes (a constant tempo never drifts).
  const double newInc = std::max(run.bpm, 1.0) / (60.0 * run.sampleRate);
  if (newInc != inc) {
    ba += static_cast<double>(t - ta) * inc;
    ta = t;
    inc = newInc;
  }
  if (law.law.looping) endT = INT64_MAX;
  else if (endT >= t) endT = std::max(t, timeOfBeats(*this, law.law.contentAt(law.endF) - beat0));  // not reached: predicted
  const bool beats = static_cast<AudioWarp>(mode) == AudioWarp::Beats;
  alignas(32) int64_t ph[kWarpVoiceChunk + 1];
  alignas(32) int64_t stp[kWarpVoiceChunk];
  alignas(32) double rt[kWarpVoiceChunk];
  WarpGrainParams P;
  P.origin = -static_cast<int64_t>(hop);  // grain 0 is centred on the trigger
  P.hop = hop;
  P.grain = 2 * hop;
  P.dir = 1;
  P.gain = 1.0f;
  P.seed = 0x5A3D17u;
  P.sourceRate = static_cast<uint32_t>(std::lround(src.fileRate));
  P.channels = src.channels;
  WarpSliceParams S;
  S.fade = X;
  S.loop = run.set.sliceLoop;
  S.envelope = run.set.envelope;
  S.gain = 1.0f;
  S.perSample = run.perSample;
  const double scale = src.rateScale;
  for (uint32_t c0 = 0; c0 < n;) {
    const uint32_t m = std::min<uint32_t>(kWarpVoiceChunk, n - c0);
    // Φ over the chunk. A constant rate (no glide: the common case) converts once; Φ is the same sums either way.
    bool constant = true;
    for (uint32_t i = 1; i < m; ++i) constant = constant && rate[c0 + i] == rate[c0];
    ph[0] = phi;
    if (constant) {
      const double r = clampReadRate(static_cast<double>(rate[c0]) * scale);
      const int64_t step = rateStep(r);
      for (uint32_t i = 0; i < m; ++i) {
        rt[i] = r;
        stp[i] = step;
        ph[i + 1] = ph[i] + step;
      }
    } else {
      for (uint32_t i = 0; i < m; ++i) {
        const double r = clampReadRate(static_cast<double>(rate[c0 + i]) * scale);
        rt[i] = r;
        stp[i] = rateStep(r);
        ph[i + 1] = ph[i] + stp[i];
      }
      constant = true;  // (different rates can still round to one step)
      for (uint32_t i = 1; i < m; ++i) constant = constant && stp[i] == stp[0];
    }
    VoiceHost h{*this, src, run, law};
    h.s = t;
    h.n = m;
    h.ph = ph;
    h.rt = rt;
    h.st = stp;
    h.constant = constant;
    h.sc = &sc;
    float* o[2] = {audible ? outL + c0 : nullptr, audible ? outR + c0 : nullptr};
    if (!audible) {
      // (SM.5: the clock only)
    } else if (beats) {
      if (cur.step0 == 0 && !cur.terminal) {  // the first pass: its entry is this chunk's first sample
        cur.step0 = std::max<int64_t>(stp[0], kMinEntryStep);
        cur.level = mipLevelFor(rt[0], src.levels);
      }
      if (cur.terminal) cur.next = INT64_MAX;
      else cur.next = std::max({cur.T + 1, t, timeOfBeats(*this, cur.exitU)});
      if (hasPrev != 0 && t < cur.T + X) warpRenderPass(h, S, prev, t, m, o, nullptr, sc);
      for (uint32_t guard = 0; guard < 64; ++guard) {
        warpRenderPass(h, S, cur, t, m, o, nullptr, sc);
        if (cur.terminal || cur.next >= t + m) break;
        const auto k = static_cast<uint32_t>(cur.next - t);
        const WarpSlicePass np =
            passAfter(src, law, cur, ph[k], mipLevelFor(rt[k], src.levels), std::max<int64_t>(stp[k], kMinEntryStep));
        prev = cur;
        hasPrev = 1;
        cur = np;
        cur.next = cur.terminal ? INT64_MAX : std::max(cur.T + 1, timeOfBeats(*this, cur.exitU));
      }
    } else {
      warpRenderGrains(h, grains, P, t, m, o, nullptr, sc);
      // The end (no loop): a 2 ms fade from the sample the content reaches it.
      if (endT != INT64_MAX && t + m > endT) {
        const auto Xf = static_cast<float>(X);
        for (uint32_t i = 0; i < m; ++i) {
          const float gi = rampValue((Xf - 0.5f) / Xf, -1.0f / Xf, warpRampIndex(t + static_cast<int64_t>(i), endT));
          o[0][i] *= gi;
          o[1][i] *= gi;
        }
      }
    }
    // The read position (where the law is), and whether the voice still sounds.
    const float at = static_cast<float>(std::clamp(h.lawFrame(t) / len, 0.0, 1.0));
    for (uint32_t i = 0; i < m; ++i) {
      const bool on = endT == INT64_MAX || t + i < endT + X;
      if (pos != nullptr) pos[c0 + i] = on ? at : 0.0f;
      if (playing != nullptr) playing[c0 + i] = on ? 1.0f : 0.0f;
    }
    phi = ph[m];
    t += m;
    c0 += m;
    if (endT != INT64_MAX && t >= endT + X) {
      active = 0;
      for (uint32_t i = c0; i < n; ++i) {
        if (pos != nullptr) pos[i] = 0.0f;
        if (playing != nullptr) playing[i] = 0.0f;
      }
      return;
    }
  }
}

}  // namespace pq::dsp
