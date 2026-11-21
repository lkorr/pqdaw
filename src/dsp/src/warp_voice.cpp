// The Sampler's warp voice (SM.4): pq/dsp/warp_voice.h "WarpVoice".
#include "pq/dsp/warp_voice.h"

#include <immintrin.h>

#include <numbers>

#include "pq/dsp/resample.h"

namespace pq::dsp {

namespace {

constexpr double k2m32 = 1.0 / 4294967296.0;
constexpr double kMaxRate = 64.0;  // source frames per output frame at most (beyond: clamped)

// Half a period of the Hann window, w(u) = 0.5 − 0.5·cos(π u / kHannHalf), u ∈ [0, kHannHalf] (built at load time).
constexpr uint32_t kHannHalf = 4096;
struct HannTable {
  float v[kHannHalf + 1];
  HannTable() noexcept {
    for (uint32_t u = 0; u <= kHannHalf; ++u)
      v[u] = static_cast<float>(0.5 - 0.5 * std::cos(std::numbers::pi * static_cast<double>(u) / kHannHalf));
  }
};
const HannTable kHann;

// The marker segment index i (pairs i, i + 1) for a value along axis `ax` (0 frames, 1 beats): the last segment whose
// start is ≤ v, clamped to [0, m − 2] (so the end segments extend). m ≥ 2.
uint32_t segmentOf(const float* M, uint32_t m, double v, uint32_t ax) noexcept {  // RT-safe
  uint32_t lo = 0, hi = m - 1;  // pair index; find the last i with M[2i + ax] ≤ v
  while (hi - lo > 1) {
    const uint32_t mid = (lo + hi) / 2;
    if (static_cast<double>(M[2 * mid + ax]) <= v) lo = mid;
    else hi = mid;
  }
  return std::min(lo, m - 2);
}

}  // namespace

double warpFrameOfBeat(const WarpSource& src, double q, double fpb) noexcept {  // RT-safe
  const uint32_t m = src.markerCount;
  const float* M = src.markers;
  if (m == 0 || M == nullptr) return q * fpb;
  if (m == 1) return static_cast<double>(M[0]) + (q - static_cast<double>(M[1])) * fpb;
  const uint32_t i = segmentOf(M, m, q, 1);
  const double f0 = M[2 * i], b0 = M[2 * i + 1], f1 = M[2 * i + 2], b1 = M[2 * i + 3];
  return f0 + (q - b0) * (f1 - f0) / (b1 - b0);
}

double warpBeatOfFrame(const WarpSource& src, double f, double fpb) noexcept {  // RT-safe
  const uint32_t m = src.markerCount;
  const float* M = src.markers;
  if (m == 0 || M == nullptr) return f / fpb;
  if (m == 1) return static_cast<double>(M[1]) + (f - static_cast<double>(M[0])) / fpb;
  const uint32_t i = segmentOf(M, m, f, 0);
  const double f0 = M[2 * i], b0 = M[2 * i + 1], f1 = M[2 * i + 2], b1 = M[2 * i + 3];
  return b0 + (f - f0) * (b1 - b0) / (f1 - f0);
}

namespace {

// What one run needs besides the voice: the region in beats, the loop, the time law helpers.
struct Law {
  const WarpSource& src;
  const WarpVoiceRun& run;
  double fpb = 1.0;  // frames per beat at sampleBpm
  bool looping = false;
  double lsF = 0.0, leF = 0.0, lsB = 0.0, leB = 0.0;
  double startF = 0.0, endF = 0.0;

  Law(const WarpSource& s, const WarpVoiceRun& r) noexcept : src(s), run(r) {
    fpb = 60.0 * src.fileRate / std::max(run.sampleBpm, 1.0);
    const auto len = static_cast<double>(src.length());
    lsF = std::clamp(run.loopStart, 0.0, len);
    leF = std::clamp(run.loopEnd, 0.0, len);
    looping = leF > lsF + 1.0;
    if (looping) {
      lsB = warpBeatOfFrame(src, lsF, fpb);
      leB = warpBeatOfFrame(src, leF, fpb);
      looping = leB > lsB;
    }
    startF = std::clamp(run.start, 0.0, len);
    if (looping && startF >= leF) startF = lsF;  // a start past the loop plays the loop
    endF = std::clamp(run.end, 0.0, len);
  }
  double beatOf(double f) const noexcept { return warpBeatOfFrame(src, f, fpb); }
  double frameOf(double q) const noexcept { return warpFrameOfBeat(src, q, fpb); }
  // Content beat of u beats since the trigger (folded into the loop while looping).
  double fold(double beat0, double u) const noexcept {
    double q = beat0 + u;
    if (looping && q >= leB) q = lsB + std::fmod(q - lsB, leB - lsB);
    return q;
  }
  // Beats' next boundary after F (a slice start, the loop end, the end).
  double nextBoundary(double F) const noexcept {
    const double limit = looping ? leF : endF;
    const float* S = src.slices;
    const uint32_t n = src.sliceCount;
    // first slice > F (+ a hair: F is a boundary itself)
    const float* it = std::upper_bound(S, S + n, static_cast<float>(F + 1e-3));
    double b = it != S + n ? static_cast<double>(*it) : limit;
    return std::min(b, limit);
  }
};

}  // namespace

// ---- the clock host (warp_voice.h "The shared renderers") --------------------------------------------------------------

namespace {

struct VoiceHost {
  static constexpr bool kClock = true;
  WarpVoice& v;
  const Law& law;
  int64_t s = 0;              // the chunk's first sample (since the trigger)
  uint32_t n = 0;
  const int64_t* ph = nullptr;  // Φ(s + i), i ≤ n
  const double* rt = nullptr;   // the rate at s + i, i < n
  const int64_t* st = nullptr;  // the 32.32 step at s + i
  const WarpScratch* sc = nullptr;
  bool constStep = false;       // every step of the chunk is the same
  static constexpr int64_t kGather = kWarpGatherFrames;

  int64_t keep() const noexcept { return 0; }
  double rateAt(int64_t t) const noexcept { return rt[std::clamp<int64_t>(t - s, 0, static_cast<int64_t>(n) - 1)]; }
  int64_t stepAt(int64_t t) const noexcept { return st[std::clamp<int64_t>(t - s, 0, static_cast<int64_t>(n) - 1)]; }
  double lawFrame(int64_t t) const noexcept {
    const double u = v.ba + static_cast<double>(t - v.ta) * v.inc;
    return law.frameOf(law.fold(v.beat0, u));
  }
  WarpGrainRate grainRate(int64_t gk) const noexcept {
    WarpGrainRate r;
    r.rate = rateAt(gk);
    r.step = stepAt(gk);
    const double hr = static_cast<double>(v.hop) * r.rate;
    if (static_cast<WarpVoiceMode>(v.mode) == WarpVoiceMode::Tones) warpAlignFor(hr, law.src.fileRate, &r.alignLen, &r.alignLags);
    else r.jitter = std::clamp(law.run.flux, 0.0, 100.0) / 100.0 * hr;
    return r;
  }
  int64_t phiAt(int64_t t) const noexcept {
    if (t >= s) return ph[std::min<int64_t>(t - s, n)];
    return ph[0] - (s - t) * st[0];  // a grain that started before the trigger: extrapolated at the first rate
  }
  uint32_t levelFor(double rate) const noexcept { return mipLevelFor(rate, law.src.levels); }
  void countGrain() noexcept {}
  // Level-0 frames [first, last): in place inside the guards, else gathered with zeros (alignment reads only).
  bool source(int64_t first, int64_t last, bool /*forward*/, int64_t /*keep*/, const float** base, int64_t* bf) noexcept {
    const SampleLevelView& lv = law.src.level[0];
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
  // gb[i] = gain · w(j0 + i), the periodic Hann of N frames from kHann (linear between its points: within 4e-8 of
  // 0.5 − 0.5·cos(2πj / N), so the grains still sum to 1 within 1e-7). No dependency between frames.
  void hannGain(int64_t N, int64_t j0, float gain, float* gb, uint32_t frames) const noexcept {
    const double scale = 2.0 * kHannHalf / static_cast<double>(N);
    for (uint32_t i = 0; i < frames; ++i) {
      double x = static_cast<double>(j0 + static_cast<int64_t>(i)) * scale;
      if (x > kHannHalf) x = 2.0 * kHannHalf - x;
      x = std::clamp(x, 0.0, static_cast<double>(kHannHalf));
      const auto k = std::min<int32_t>(static_cast<int32_t>(x), static_cast<int32_t>(kHannHalf) - 1);
      const auto f = static_cast<float>(x - static_cast<double>(k));
      gb[i] = gain * (kHann.v[k] + f * (kHann.v[k + 1] - kHann.v[k]));
    }
  }
  // out[c][i] += cubic(source at pos[i] on `level`) · g[i] (pos: level-0 32.32; one rounding per add, as fmadd), 8
  // frames at a time (AVX2 gathers; a short tail runs the same vector code on a padded copy, so a frame's value does
  // not depend on where a call splits). Positions whose taps all lie in the zeros beyond the data add nothing. A run
  // of consecutive whole frames on level 0 (rate 1) is a straight copy: the cubic there is exactly the frame anyway.
  void readAt(const int64_t* pos, uint32_t level, const float* g, uint32_t frames, float* const* out) const noexcept {
    if (frames == 0) return;
    const SampleLevelView& lv = law.src.level[level];
    const int64_t len = lv.length;
    const float* ch[2] = {lv.ch[0], law.src.channels > 1 ? lv.ch[1] : lv.ch[0]};
    if (level == 0 && (pos[0] & 0xFFFFFFFF) == 0) {
      bool lin = true;
      for (uint32_t i = 1; i < frames && lin; ++i) lin = pos[i] == pos[0] + static_cast<int64_t>(i) * kWarpOne;
      const int64_t i0 = pos[0] >> 32;
      if (lin && i0 >= -kSampleSlotGuard && i0 + frames <= len + kSampleSlotGuard) {
        for (uint32_t c = 0; c < 2; ++c) {
          const float* x = ch[c] + i0;
          float* o = out[c];
          uint32_t i = 0;
          for (; i + 8 <= frames; i += 8)
            _mm256_storeu_ps(o + i, _mm256_fmadd_ps(_mm256_loadu_ps(x + i), _mm256_loadu_ps(g + i), _mm256_loadu_ps(o + i)));
          for (; i < frames; ++i) o[i] = _mm_cvtss_f32(_mm_fmadd_ss(_mm_set_ss(x[i]), _mm_set_ss(g[i]), _mm_set_ss(o[i])));
        }
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
      const __m256 half = _mm256_set1_ps(0.5f);
      for (uint32_t c = 0; c < 2; ++c) {
        const float* d = ch[c];
        const __m256 xm1 = _mm256_i32gather_ps(d - 1, vi, 4), x0 = _mm256_i32gather_ps(d, vi, 4);
        const __m256 x1 = _mm256_i32gather_ps(d + 1, vi, 4), x2 = _mm256_i32gather_ps(d + 2, vi, 4);
        // sample_read's cubic (kernels_sample.cpp interp4)
        const __m256 c1 = _mm256_mul_ps(half, _mm256_sub_ps(x1, xm1));
        const __m256 c2 = _mm256_add_ps(_mm256_fnmadd_ps(_mm256_set1_ps(2.5f), x0, xm1),
                                        _mm256_fnmadd_ps(half, x2, _mm256_add_ps(x1, x1)));
        const __m256 c3 = _mm256_fmadd_ps(half, _mm256_sub_ps(x2, xm1), _mm256_mul_ps(_mm256_set1_ps(1.5f), _mm256_sub_ps(x0, x1)));
        const __m256 y = _mm256_fmadd_ps(_mm256_fmadd_ps(_mm256_fmadd_ps(c3, t, c2), t, c1), t, x0);
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
  void readClock(int64_t anchor, int64_t dir, int64_t phiK, uint32_t level, int64_t a, uint32_t frames, float* const* o,
                 const float* gb) const noexcept {
    alignas(32) int64_t pos[kWarpVoiceChunk];
    for (uint32_t i = 0; i < frames; ++i)
      pos[i] = static_cast<int64_t>(static_cast<uint64_t>(anchor) +
                                    static_cast<uint64_t>(dir) * static_cast<uint64_t>(ph[a - s + i] - phiK));
    readAt(pos, level, gb, frames, o);
  }
};

inline double floorD(double x) noexcept {  // RT-safe (roundsd: no CRT call)
  return _mm_cvtsd_f64(_mm_floor_sd(_mm_setzero_pd(), _mm_set_sd(x)));
}
// v as 32.32 (round half up; WarpVoice's own positions, not the clip's warpToFixed).
inline int64_t fixedOf(double v) noexcept { return static_cast<int64_t>(floorD(v * 4294967296.0 + 0.5)); }  // RT-safe

// warp.h rampValue, inline: clamp(fma(j, dg, base), 0, 1).
inline float ramp(float base, float dg, int32_t j) noexcept {  // RT-safe
  const float v = _mm_cvtss_f32(_mm_fmadd_ss(_mm_set_ss(static_cast<float>(j)), _mm_set_ss(dg), _mm_set_ss(base)));
  return std::clamp(v, 0.0f, 1.0f);
}

// The first sample (since the trigger) at which the content has advanced U beats (b(t) ≥ U), under the current law.
int64_t timeOfBeats(const WarpVoice& v, double U) noexcept {  // RT-safe
  if (!(v.inc > 0.0)) return INT64_MAX;
  const double x = static_cast<double>(v.ta) + (U - v.ba) / v.inc;
  if (!(x < 9.0e18)) return INT64_MAX;
  return static_cast<int64_t>(std::ceil(x - 1e-6));
}

// A Beats pass entering at content frame F (a boundary, or the start) at sample T.
WarpSlicePass makePass(const Law& law, int64_t T, int64_t phiT, double F, double entryU, bool fadeIn, uint32_t level) noexcept {
  WarpSlicePass p;
  p.T = T;
  p.phiT = phiT;
  p.F = F;
  const double bF = law.nextBoundary(F);
  p.L = std::max(0.0, bF - F);
  p.entryU = entryU;
  p.exitU = entryU + (law.beatOf(bF) - law.beatOf(F));
  p.fadeIn = fadeIn;
  p.level = level;
  p.dir = 1;
  return p;
}

// The pass after p (entering at p.next): the boundary's frame, folded to the loop start at the loop's end; at the end
// (no loop) a terminal pass.
WarpSlicePass passAfter(const Law& law, const WarpSlicePass& p, int64_t phiT, uint32_t level) noexcept {
  const double bF = p.F + p.L;
  if (!law.looping && bF >= law.endF - 1e-6) {
    WarpSlicePass t;
    t.T = p.next;
    t.next = INT64_MAX;
    t.terminal = true;
    t.phiT = phiT;
    t.F = bF;
    t.entryU = t.exitU = p.exitU;
    return t;
  }
  const double F = law.looping && bF >= law.leF - 1e-6 ? law.lsF : bF;
  return makePass(law, p.next, phiT, F, p.exitU, true, level);
}

// One pass's frames in the chunk (WarpVoice's Beats: the pass geometry per sample in source frames since the entry).
void renderPassClock(const VoiceHost& h, WarpSlicePass& p, const WarpVoiceRun& run, int64_t X, float* const* out,
                     const WarpScratch& sc) noexcept {  // RT-safe
  if (p.terminal) return;
  const int64_t s = h.s;
  const auto n = static_cast<int64_t>(h.n);
  const int64_t audEnd = p.next == INT64_MAX ? INT64_MAX : p.next + X;
  const int64_t a0 = std::max(s, p.T), b0 = std::min(s + n, audEnd);
  if (a0 >= b0) return;
  const auto Xf = static_cast<float>(X);
  const auto Xd = static_cast<double>(X);
  const double iX = 1.0 / Xd;
  const double L = p.L, Lh = 0.5 * p.L;
  const int64_t anchor = warpToFixed(p.F);
  const WarpSliceLoop mode = run.sliceLoop;
  const float envGd = run.envelope < 100.0f && p.next != INT64_MAX
                          ? std::max(1.0f, static_cast<float>(p.next - p.T) * (1.0f - run.envelope / 100.0f))
                          : 0.0f;
  alignas(32) int64_t pos[kWarpVoiceChunk];
  float* g = sc.gain;
  float* o[2] = {out[0] + (a0 - s), out[1] + (a0 - s)};
  // The steady middle of a slice (a constant rate, no fade in / crossfade / envelope, natural, far from its natural end:
  // the rate is constant, so its remaining time only falls): gains 1, positions linear — what the loop below gives.
  if (h.constStep && !(p.fadeIn && a0 - p.T < X) && (p.next == INT64_MAX || b0 <= p.next) && envGd == 0.0f) {
    const int64_t dl = h.ph[b0 - 1 - s] - p.phiT;
    const double ul = static_cast<double>(dl) * k2m32;
    const double ir = 1.0 / std::max(h.rt[b0 - 1 - s], 1e-9);
    if (ul < L && std::clamp((L - ul) * ir - 0.5, 0.0, Xd + 1.0) * iX >= 1.0) {
      const auto m = static_cast<uint32_t>(b0 - a0);
      for (uint32_t i = 0; i < m; ++i) {
        pos[i] = anchor + (h.ph[a0 - s + i] - p.phiT);
        g[i] = 1.0f;
      }
      h.readAt(pos, p.level, g, m, o);
      return;
    }
  }
  for (int64_t t = a0; t < b0; ++t) {
    const auto i = static_cast<size_t>(t - a0);
    const int64_t d = h.ph[t - s] - p.phiT;
    const double u = static_cast<double>(d) * k2m32;
    const double r = std::max(h.rt[t - s], 1e-9);
    const double ir = 1.0 / r;
    float gi = 1.0f;
    if (p.fadeIn && t - p.T < X) gi *= ramp(0.5f / Xf, 1.0f / Xf, warpRampIndex(t, p.T));
    if (p.next != INT64_MAX && t >= p.next) gi *= ramp((Xf - 0.5f) / Xf, -1.0f / Xf, warpRampIndex(t, p.next));
    if (envGd > 0.0f) gi *= ramp(0.0f, -1.0f / envGd, warpRampIndex(t, p.next));
    const bool loopsOk = mode != WarpSliceLoop::Off && Lh * ir >= 2.0 * Xd;
    int64_t at = anchor + d;
    if (u < L) {
      // The natural part. Not cut by the next slice (its natural end, at the current rate, comes before the crossfade):
      // Off fades out into the natural end, Forward declicks into the first jump.
      const double rem = (L - u) * ir;
      const bool cut = p.next != INT64_MAX && static_cast<double>(t) + rem >= static_cast<double>(p.next - X);
      if (!cut && (!loopsOk || mode == WarpSliceLoop::Forward))
        gi *= static_cast<float>(std::clamp((rem - 0.5) * iX, 0.0, 1.0));
    } else {
      if (p.natEnd == INT64_MAX) p.natEnd = t;
      const bool cut = p.next != INT64_MAX && p.natEnd >= p.next - X;
      if (!cut) {
        if (!loopsOk) {
          gi = 0.0f;  // Off: silent after its fade
        } else {  // loop iterations over the second half
          const double w = u - L;
          const double it = floorD(w / Lh);
          const double wi = w - it * Lh;
          double off = Lh + wi;
          if (mode == WarpSliceLoop::Forward) {
            gi *= static_cast<float>(std::clamp((wi * ir + 0.5) * iX, 0.0, 1.0) * std::clamp(((Lh - wi) * ir - 0.5) * iX, 0.0, 1.0));
          } else if ((static_cast<int64_t>(it) & 1) == 0) {
            off = L - wi;  // Back-Forward: even iterations run back from the end
          }
          at = fixedOf(p.F + off);
        }
      }  // cut: the natural read continues through the crossfade
    }
    g[i] = gi;
    pos[i] = at;
  }
  h.readAt(pos, p.level, g, static_cast<uint32_t>(b0 - a0), o);
}

}  // namespace

void WarpVoice::trigger(const WarpSource& src, const WarpVoiceRun& run) noexcept {  // RT-safe
  const Law law(src, run);
  t = 0;
  phi = 0;
  ta = 0;
  ba = 0.0;
  inc = std::max(run.bpm, 1.0) / (60.0 * run.sampleRate);
  beat0 = law.beatOf(law.startF);
  mode = static_cast<uint32_t>(run.mode);
  const double secs = std::clamp(run.grainSeconds, 0.001, 0.5);
  hop = static_cast<uint32_t>(std::max<int64_t>(2, 2 * std::llround(secs * run.sampleRate / 2.0)) / 2);
  grains = {};
  hasPrev = 0;
  prev = {};
  active = src.length() > 0 && (law.looping || law.endF > law.startF) ? 1u : 0u;
  endT = law.looping ? INT64_MAX : timeOfBeats(*this, law.beatOf(law.endF) - beat0);
  if (run.mode == WarpVoiceMode::Beats) cur = makePass(law, 0, 0, law.startF, 0.0, false, 0);
}

WarpUnfold warpUnfold(const WarpSource& src, const WarpVoiceRun& run) noexcept {  // RT-safe
  const Law law(src, run);
  WarpUnfold u;
  u.looping = law.looping;
  u.loopStart = std::llround(law.lsF);
  u.loopEnd = std::llround(law.leF);
  if (u.loopEnd <= u.loopStart) u.looping = false;
  u.startF = law.startF;
  u.endF = law.endF;
  return u;
}

double WarpVoice::unfoldedAt(const WarpSource& src, const WarpVoiceRun& run, double tt) const noexcept {  // RT-safe
  const Law law(src, run);
  const double q = beat0 + ba + (tt - static_cast<double>(ta)) * inc;
  if (!law.looping || q < law.leB) return law.frameOf(q);
  const double span = law.leB - law.lsB;
  const double wraps = std::floor((q - law.lsB) / span);
  return law.frameOf(q - wraps * span) +
         wraps * static_cast<double>(std::llround(law.leF) - std::llround(law.lsF));
}

void WarpVoice::render(const WarpSource& src, const WarpVoiceRun& run, const float* rate, uint32_t n, float* outL,
                       float* outR, float* pos, float* playing, const WarpScratch& sc) noexcept {  // RT-safe
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
  const Law law(src, run);
  const int64_t X = fadeFrames(run.sampleRate);
  // The tempo: re-anchored only when it changes (a constant tempo never drifts).
  const double newInc = std::max(run.bpm, 1.0) / (60.0 * run.sampleRate);
  if (newInc != inc) {
    ba += static_cast<double>(t - ta) * inc;
    ta = t;
    inc = newInc;
  }
  if (law.looping) endT = INT64_MAX;
  else if (endT >= t) endT = std::max(t, timeOfBeats(*this, law.beatOf(law.endF) - beat0));  // not reached: predicted
  const auto vm = static_cast<WarpVoiceMode>(mode);
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
  const double scale = src.rateScale;
  for (uint32_t c0 = 0; c0 < n;) {
    const uint32_t m = std::min<uint32_t>(kWarpVoiceChunk, n - c0);
    ph[0] = phi;
    for (uint32_t i = 0; i < m; ++i) {
      double r = static_cast<double>(rate[c0 + i]) * scale;
      r = r > 0.0 ? std::min(r, kMaxRate) : 0.0;  // (NaN: 0)
      rt[i] = r;
      stp[i] = static_cast<int64_t>(r * 4294967296.0 + 0.5);  // (r ≥ 0: = llround, without the CRT call)
      ph[i + 1] = ph[i] + stp[i];
    }
    VoiceHost h{*this, law};
    h.s = t;
    h.n = m;
    h.ph = ph;
    h.rt = rt;
    h.st = stp;
    h.constStep = true;
    for (uint32_t i = 1; i < m; ++i) h.constStep = h.constStep && stp[i] == stp[0];
    h.sc = &sc;
    float* o[2] = {audible ? outL + c0 : nullptr, audible ? outR + c0 : nullptr};
    if (!audible) {
      // (SM.5: the clock only)
    } else if (vm == WarpVoiceMode::Beats) {
      if (cur.terminal) cur.next = INT64_MAX;
      else cur.next = std::max({cur.T + 1, t, timeOfBeats(*this, cur.exitU)});
      if (hasPrev != 0 && t < cur.T + X) renderPassClock(h, prev, run, X, o, sc);
      for (uint32_t guard = 0; guard < 64; ++guard) {
        renderPassClock(h, cur, run, X, o, sc);
        if (cur.terminal || cur.next >= t + m) break;
        const auto k = static_cast<uint32_t>(cur.next - t);
        WarpSlicePass np = passAfter(law, cur, ph[k], mipLevelFor(rt[k], src.levels));
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
          const float gi = ramp((Xf - 0.5f) / Xf, -1.0f / Xf, warpRampIndex(t + static_cast<int64_t>(i), endT));
          o[0][i] *= gi;
          o[1][i] *= gi;
        }
      }
    }
    // The read position (where the law is), and whether the voice still sounds.
    const float at = static_cast<float>(std::clamp(law.frameOf(law.fold(beat0, ba + static_cast<double>(t - ta) * inc)) / len, 0.0, 1.0));
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
