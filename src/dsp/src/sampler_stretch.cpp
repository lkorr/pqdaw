// The Sampler's Complex / Complex Pro voices (SM.5): pq/dsp/sampler_stretch.h.
#include "pq/dsp/sampler_stretch.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

#include <immintrin.h>

#include "pq/dsp/fixed.h"
#include "pq/dsp/warp.h"
#include "simd.h"

namespace pq::dsp {

namespace {

std::atomic<uint32_t> gBankTag{0};
constexpr auto kC = static_cast<int64_t>(kComplexChunk);
constexpr auto kVoc = static_cast<int64_t>(kComplexFrames);
constexpr double kMinRate = 1e-3;   // the law's time estimate divides by the rate
constexpr uint32_t kSeed = 0x5A3D17u;  // every claim reseeds the library's generator alike: renders repeat
constexpr int64_t kReach = 4;       // vocoder frames the cubic read reaches before its first (priming)

bool claim(SamplerStretch& bank, ComplexVoice& cv, uint32_t lane, int64_t t) noexcept {  // RT-safe
  const uint32_t n = bank.lanes();
  uint32_t s = SamplerStretch::kNone;
  if (lane < n && bank.owner(lane).token == 0) s = lane;
  for (uint32_t k = 1; s == SamplerStretch::kNone && k <= n; ++k) {
    const uint32_t j = (lane + k) % n;
    if (bank.owner(j).token == 0) s = j;
  }
  if (s == SamplerStretch::kNone) {
    ++bank.misses;
    cv.phase = ComplexVoice::kWait;
    cv.stretcher = SamplerStretch::kNone;
    return false;
  }
  cv.stretcher = s;
  cv.token = bank.newToken();
  cv.bank = bank.tag();
  cv.lead = static_cast<uint32_t>(complexLead(s, bank.pool().latency()));
  cv.phase = ComplexVoice::kPending;
  cv.primeT = t + static_cast<int64_t>(cv.lead / kComplexPrimeRate);  // (staggered as the lead: M10)
  cv.fade = 0;
  bank.owner(s) = {cv.token, lane, cv.cx.vHi};
  return true;
}

void release(SamplerStretch* bank, ComplexVoice& cv) noexcept {  // RT-safe
  if (bank != nullptr && cv.phase != ComplexVoice::kWait && cv.stretcher < bank->lanes() && bank->tag() == cv.bank &&
      bank->owner(cv.stretcher).token == cv.token)
    bank->owner(cv.stretcher) = {};
  cv.stretcher = SamplerStretch::kNone;
  cv.phase = ComplexVoice::kWait;
}

// The voice as the stream's host at one sample (complex_stream.h "The host"): the read is at vocoder frame Y there,
// moving at r, so the read reaches vocoder frame y at output time t + (y − Y) / r, where the voice's law gives the
// content. The source: the resident sample, level 0.
struct StreamHost {
  const WarpVoice& v;
  const WarpSource& src;
  const WarpRunLaw& L;
  double t, Y, r;

  double unfolded(double y) const noexcept { return v.unfoldedAt(L, t + (y - Y) / r); }  // RT-safe (all)
  const WarpLaw& law() const noexcept { return L.law; }
  int64_t dir() const noexcept { return 1; }
  int64_t startU() const noexcept { return std::llround(L.startF); }
  int64_t endU() const noexcept { return L.law.loopsFrames() ? INT64_MAX : std::llround(L.endF); }
  int64_t frames() const noexcept { return src.level[0].length; }
  uint32_t channels() const noexcept { return src.channels > 1 ? 2u : 1u; }
  void read(int64_t a, int64_t b, bool /*forward*/, float* const* dst) const noexcept {
    const SampleLevelView& lv = src.level[0];
    for (uint32_t c = 0; c < channels(); ++c) std::memcpy(dst[c], lv.ch[c] + a, sizeof(float) * static_cast<size_t>(b - a));
  }
};

// l[i] (and r[i]) = the cubic of the vocoder's output at Φ_i = ph[i] (32.32 vocoder frames; the buffer's frame 0 is
// vLo), i < n ≤ kWarpVoiceChunk; frames whose taps start below vLo: 0. The taps up to (Φ_i >> 32) + 2 are held (the
// voice's chunk demand). b1 null: one channel, written to both. 8 frames at a time; a tail runs the same vector code
// on padding. RT-safe.
void readVocoder(const float* b0, const float* b1, int64_t vLo, const int64_t* ph, uint32_t n, float* l, float* r) noexcept {
  for (uint32_t i = 0; i < n; i += 8) {
    const uint32_t m = std::min<uint32_t>(8, n - i);
    alignas(32) int32_t ix[8];
    alignas(32) float fr[8], on[8];
    for (uint32_t k = 0; k < 8; ++k) {
      ix[k] = 1;
      fr[k] = on[k] = 0.0f;
    }
    for (uint32_t k = 0; k < m; ++k) {
      const int64_t at = (ph[i + k] >> 32) - vLo;
      if (at < 1) continue;
      ix[k] = static_cast<int32_t>(at);
      fr[k] = static_cast<float>(static_cast<uint32_t>(ph[i + k])) * 2.3283064365386963e-10f;  // 2^-32
      on[k] = 1.0f;
    }
    const __m256 t = _mm256_load_ps(fr), keep = _mm256_cmp_ps(_mm256_load_ps(on), _mm256_setzero_ps(), _CMP_NEQ_OQ);
    __m256 xm1, x0, x1, x2;
    simd::taps8(b0, ix, xm1, x0, x1, x2);
    const __m256 yl = _mm256_and_ps(simd::cubic4<simd::S8>(xm1, x0, x1, x2, t), keep);
    __m256 yr = yl;
    if (b1 != nullptr) {
      simd::taps8(b1, ix, xm1, x0, x1, x2);
      yr = _mm256_and_ps(simd::cubic4<simd::S8>(xm1, x0, x1, x2, t), keep);
    }
    if (m == 8) {
      _mm256_storeu_ps(l + i, yl);
      _mm256_storeu_ps(r + i, yr);
    } else {
      alignas(32) float bl[8], br[8];
      _mm256_store_ps(bl, yl);
      _mm256_store_ps(br, yr);
      for (uint32_t k = 0; k < m; ++k) {
        l[i + k] = bl[k];
        r[i + k] = br[k];
      }
    }
  }
}

// The stretcher's formant params for the read rate r (file frames per output frame): the rule at the pitch ratio
// (file-natural). Set only when the ratio, the settings or the mode moved since the last set (std::pow). RT-safe.
StretchParams formantsFor(ComplexVoice& cv, const WarpSource& src, const WarpVoiceRun& run, double r, bool* changed) noexcept {
  const double ratio = std::max(r / std::max(src.rateScale, 1e-9), kMinRate);
  const auto mode = static_cast<uint32_t>(run.mode);
  *changed = ratio != cv.fRatio || run.set.formants != cv.fFormants || run.set.formantEnvelope != cv.fOrder || mode != cv.fMode;
  if (!*changed) return {};
  cv.fRatio = ratio;
  cv.fFormants = run.set.formants;
  cv.fOrder = run.set.formantEnvelope;
  cv.fMode = mode;
  return complexFormants(run.mode, ratio, run.set.formants, run.set.formantEnvelope);
}

}  // namespace

std::unique_ptr<SamplerStretch> SamplerStretch::build(uint32_t lanes, uint32_t channels, double sampleRate) {
  return std::unique_ptr<SamplerStretch>(new SamplerStretch(std::max(lanes, 1u), channels, sampleRate));
}

SamplerStretch::SamplerStretch(uint32_t lanes, uint32_t channels, double sampleRate)
    : pool_(channels >= 2 ? lanes : 0, sampleRate, channels >= 2 ? 0 : lanes, lanes, kComplexFrames),
      owners_(std::make_unique<Owner[]>(lanes)),
      lanes_(lanes),
      channels_(channels >= 2 ? 2u : 1u),
      tag_(gBankTag.fetch_add(1, std::memory_order_relaxed) + 1),
      sampleRate_(sampleRate) {
  inCap_ = complexInFrames(pool_.latency());
  in_ = AlignedBuffer<float>(static_cast<size_t>(channels_) * inCap_);
}

void ComplexVoice::trigger(SamplerStretch* b, uint32_t lane) noexcept {  // RT-safe
  stretcher = SamplerStretch::kNone;
  phase = kWait;
  fade = 0;
  cx.vLo = cx.vHi = 0;
  if (b == nullptr) return;
  for (uint32_t s = 0; s < b->lanes(); ++s)  // the lane's previous voice is over (its words are being reset)
    if (b->owner(s).user == lane) b->owner(s) = {};
  claim(*b, *this, lane, 0);
}

void ComplexVoice::render(SamplerStretch* b, uint32_t lane, WarpVoice& v, const WarpSource& src, const WarpVoiceRun& run,
                          const WarpRunLaw& law, const float* rate, uint32_t n, float* outL, float* outR, float* pos,
                          float* playing, const WarpScratch& sc) noexcept {  // RT-safe
  const uint32_t srcCh = std::min<uint32_t>(src.channels, 2);
  const bool usable = b != nullptr && b->channels() >= srcCh && src.length() > 0;
  if (phase != kWait) {  // still ours? (a copy of these words elsewhere, or a newer bank, takes it away)
    bool ok = usable && b->tag() == bank && stretcher < b->lanes();
    if (ok) {
      const SamplerStretch::Owner& o = b->owner(stretcher);
      ok = o.token == token && o.vHi == cx.vHi;
    }
    if (!ok) {
      stretcher = SamplerStretch::kNone;
      phase = kWait;
    }
  }
  if (phase == kWait && usable && v.active != 0) claim(*b, *this, lane, v.t);
  const int64_t X = warpFadeFrames(run.sampleRate);
  const auto Xf = static_cast<float>(X);
  WarpClock ck;
  alignas(32) float cl[kWarpVoiceChunk], cr[kWarpVoiceChunk];
  for (uint32_t c0 = 0; c0 < n;) {
    const uint32_t m = std::min<uint32_t>(kWarpVoiceChunk, n - c0);
    float* oL = outL + c0;
    float* oR = outR + c0;
    float* ps = pos != nullptr ? pos + c0 : nullptr;
    float* pl = playing != nullptr ? playing + c0 : nullptr;
    c0 += m;
    const int64_t t0 = v.t;
    const bool wasActive = v.active != 0;
    const bool tones = phase != kOn || !wasActive;  // Tones sounds (priming, the cross, no stretcher); else the clock only
    if (wasActive) ck.integrate(v.phi, rate + (c0 - m), m, src.rateScale);  // Φ once, for the Tones voice and the read
    v.renderClock(src, run, law, ck, m, tones ? oL : nullptr, tones ? oR : nullptr, ps, pl, sc);
    if (phase == kWait || !usable || !wasActive) {
      if (v.active == 0) release(b, *this);
      continue;
    }
    StretchPool& P = b->pool();
    const ComplexIn in = b->in();
    const int64_t endT = v.endT;
    const int64_t* ph = ck.ph;
    const auto ld = static_cast<int64_t>(lead);
    // The chunks sample i's read needs, computed there (the law's estimate at that sample).
    auto host = [&](uint32_t i) noexcept {
      return StreamHost{v, src, law, static_cast<double>(t0 + i), static_cast<double>(ph[i]) * k2m32, std::max(ck.rt[i], kMinRate)};
    };
    auto feed = [&](uint32_t i, int64_t last, uint32_t maxChunks) noexcept {
      StreamHost h = host(i);
      bool changed = false;
      const StretchParams fp = formantsFor(*this, src, run, h.r, &changed);
      if (changed) P.setParams(stretcher, fp);
      b->chunks += cx.ensure(h, P, stretcher, in, (ph[i] >> 32) - 2, last, maxChunks);
    };
    for (uint32_t i = 0; i < m;) {
      const int64_t t = t0 + static_cast<int64_t>(i);
      const int64_t need = (ph[i] >> 32) + 3 + ld;
      if (phase == kPending) {
        if (t < primeT) {
          i = static_cast<uint32_t>(std::min<int64_t>(m, i + (primeT - t)));
          continue;
        }
        StreamHost h = host(i);
        bool changed = false;
        fRatio = -1.0;  // (the reset drops the stretcher's params: set them whatever was set before)
        const StretchParams fp = formantsFor(*this, src, run, h.r, &changed);
        cx.prime(h, P, stretcher, in, complexPrimeStart(ph[i] >> 32, kReach, P.latency()), kSeed, fp);
        primeT = t;
        primeK = complexPrimeRate(h.r);
        phase = kPriming;
      }
      if (phase == kPriming) {  // at most primeK vocoder frames per sample since the priming started
        const auto allowed = cx.ys + static_cast<int64_t>(std::floor(static_cast<double>(t - primeT + 1) * primeK));
        if (cx.vHi < need && cx.vHi + kC <= allowed)
          feed(i, need, static_cast<uint32_t>((allowed - cx.vHi) / kC));
        if (cx.vHi < need) {
          ++i;
          continue;
        }
        phase = kFading;
        fade = 0;
      }
      if (cx.vHi < need) feed(i, need, 0xFFFFFFFFu);
      // The frames from i that need nothing more: up to the first whose read does (Φ is monotonic: one search).
      const int64_t thr = (cx.vHi - 2 - ld) * kFixedOne;  // need(j) > vHi  ⇔  ph[j] ≥ thr
      const auto j = static_cast<uint32_t>(std::lower_bound(ph + i + 1, ph + m, thr) - ph);
      const uint32_t len = j - i;
      const float* b0 = P.output(stretcher);
      const float* b1 = b->channels() >= 2 ? b0 + kVoc : nullptr;
      const bool ending = endT != INT64_MAX && t + len > endT;
      if (phase == kOn && !ending) {
        readVocoder(b0, b1, cx.vLo, ph + i, len, oL + i, oR + i);
      } else {
        readVocoder(b0, b1, cx.vLo, ph + i, len, cl, cr);
        for (uint32_t k = 0; k < len; ++k) {
          const int64_t tk = t + k;
          if (endT != INT64_MAX && tk >= endT + X) {  // over (the WarpVoice zeroed the outputs)
            oL[i + k] = oR[i + k] = 0.0f;
            continue;
          }
          float a = cl[k], c = cr[k];
          if (endT != INT64_MAX && tk >= endT) {  // the end's 2 ms fade (as Tones')
            const float g = rampValue((Xf - 0.5f) / Xf, -1.0f / Xf, warpRampIndex(tk, endT));
            a *= g;
            c *= g;
          }
          if (phase == kFading) {
            const float g = (static_cast<float>(fade) + 0.5f) / Xf;
            oL[i + k] = oL[i + k] + g * (a - oL[i + k]);
            oR[i + k] = oR[i + k] + g * (c - oR[i + k]);
            if (++fade >= static_cast<uint32_t>(X)) phase = kOn;
          } else {
            oL[i + k] = a;
            oR[i + k] = c;
          }
        }
      }
      i = j;
    }
    if (v.active == 0) {
      release(b, *this);
      continue;
    }
    if (phase != kWait) b->owner(stretcher) = {token, lane, cx.vHi};
  }
}

}  // namespace pq::dsp
