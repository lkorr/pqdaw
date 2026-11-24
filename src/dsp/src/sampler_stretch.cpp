// The Sampler's Complex / Complex Pro voices (SM.5): pq/dsp/sampler_stretch.h.
#include "pq/dsp/sampler_stretch.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

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

// The stretcher's lead: a golden-ratio fraction of the analysis interval by its index. RT-safe.
uint32_t leadOf(uint32_t s, const StretchPool::Latency& lat) noexcept {
  const double g = static_cast<double>(s) * 0.6180339887498949;
  const auto lead = static_cast<int64_t>((g - std::floor(g)) * static_cast<double>(lat.interval));
  return static_cast<uint32_t>(std::clamp<int64_t>(lead, 0, std::max(lat.interval - 1, 0)));
}

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
  cv.lead = leadOf(s, bank.pool().latency());
  cv.phase = ComplexVoice::kPending;
  cv.primeT = t + static_cast<int64_t>(cv.lead / kComplexPrimeRate);  // (staggered as the lead: M10)
  cv.fade = 0;
  bank.owner(s) = {cv.token, lane, cv.vHi};
  return true;
}

void release(SamplerStretch* bank, ComplexVoice& cv) noexcept {  // RT-safe
  if (bank != nullptr && cv.phase != ComplexVoice::kWait && cv.stretcher < bank->lanes() && bank->tag() == cv.bank &&
      bank->owner(cv.stretcher).token == cv.token)
    bank->owner(cv.stretcher) = {};
  cv.stretcher = SamplerStretch::kNone;
  cv.phase = ComplexVoice::kWait;
}

// One read of x[i − 1 … i + 2] at fraction f through sample_read's cubic (simd.h cubic4). RT-safe.
float cubicAt(const float* x, float f) noexcept {
  using simd::S1;
  return _mm_cvtss_f32(simd::cubic4<S1>(_mm_set_ss(x[-1]), _mm_set_ss(x[0]), _mm_set_ss(x[1]), _mm_set_ss(x[2]), _mm_set_ss(f)));
}

// The voice's chunk work inside one render call.
struct Run {
  SamplerStretch& bank;
  ComplexVoice& cv;
  WarpVoice& v;
  const WarpSource& src;
  const WarpVoiceRun& run;
  const WarpRunLaw& L;
  const StretchPool::Latency& lat;
  uint32_t ch = 1;  // the bank's channels

  double unfolded(double t) const noexcept { return v.unfoldedAt(L, t); }
  // The loop in whole frames (an unfolded frame x ≥ loopEnd plays loopStart + (x − loopEnd) mod span).
  bool looping() const noexcept { return L.law.looping && L.law.loopEndF > L.law.loopStartF; }

  // The stream frames [k0, k1) into the bank's input (frame k0 at index 0): unfolded frame u0 + k folded into the file
  // through the loop; zeros before the start, after the end (no loop) and outside the file.
  void gather(int64_t k0, int64_t k1) noexcept {
    const SampleLevelView& lv = src.level[0];
    const int64_t len = lv.length;
    const int64_t span = L.law.loopEndF - L.law.loopStartF;
    for (int64_t k = k0; k < k1;) {
      const auto at = static_cast<size_t>(k - k0);
      int64_t r = k1 - k;
      if (k < cv.zeroBelow || k >= cv.zeroAbove) {
        if (k < cv.zeroBelow) r = std::min(r, cv.zeroBelow - k);
        for (uint32_t c = 0; c < ch; ++c) std::memset(bank.in(c) + at, 0, sizeof(float) * static_cast<size_t>(r));
        k += r;
        continue;
      }
      r = std::min(r, cv.zeroAbove - k);
      const int64_t x = cv.u0 + k;
      int64_t f = x;
      if (looping() && x >= L.law.loopEndF) {
        f = L.law.loopStartF + (x - L.law.loopEndF) % span;
        r = std::min(r, span - (f - L.law.loopStartF));
      } else if (looping()) {
        r = std::min(r, L.law.loopEndF - x);
      }
      const int64_t a = std::clamp<int64_t>(f, 0, len), b = std::clamp<int64_t>(f + r, 0, len);
      for (uint32_t c = 0; c < ch; ++c) {
        float* d = bank.in(c) + at;
        if (b <= a) {
          std::memset(d, 0, sizeof(float) * static_cast<size_t>(r));
          continue;
        }
        if (a > f) std::memset(d, 0, sizeof(float) * static_cast<size_t>(a - f));
        std::memcpy(d + (a - f), lv.ch[c] + a, sizeof(float) * static_cast<size_t>(b - a));
        if (f + r > b) std::memset(d + (b - f), 0, sizeof(float) * static_cast<size_t>(f + r - b));
      }
      k += r;
    }
  }

  void formants(double r) noexcept {
    if (run.mode != AudioWarp::ComplexPro) return;
    const double keep = std::clamp(static_cast<double>(run.set.formants), 0.0, 100.0) / 100.0;
    if (keep <= 0.0) return;  // (as claimed: no formant processing)
    const double ratio = std::max(r / std::max(src.rateScale, 1e-9), kMinRate);  // the pitch ratio (file-natural)
    bank.pool().setParams(cv.stretcher,
                          StretchParams{1.0, std::pow(ratio, -keep), false, 1.0 / std::clamp(static_cast<double>(run.set.formantEnvelope), 8.0, 256.0)});
  }

  // Primes the stretcher for a read at sample t (read position Y, rate r).
  void prime(int64_t t, int64_t phi, double r) noexcept {
    StretchPool& P = bank.pool();
    const double Y = static_cast<double>(phi) * k2m32;
    r = std::max(r, kMinRate);
    auto tOf = [&](double y) noexcept { return static_cast<double>(t) + (y - Y) / r; };
    cv.ys = floorDiv((phi >> 32) - lat.output - lat.block - 4, kC) * kC;
    const auto y0 = static_cast<double>(cv.ys + lat.output);
    cv.u0 = std::llround(unfolded(tOf(y0)));
    cv.zeroBelow = std::llround(L.startF) - cv.u0;
    cv.zeroAbove = looping() ? INT64_MAX : std::llround(L.endF) - cv.u0;
    P.reset(cv.stretcher, kSeed);
    formants(r);
    gather(lat.input - lat.seek, lat.input);
    const double rate = std::abs(unfolded(tOf(y0 + static_cast<double>(kC))) - unfolded(tOf(y0))) / static_cast<double>(kC);
    const float* in[2] = {bank.in(0), bank.in(ch - 1)};
    P.seek(cv.stretcher, in, lat.seek, rate);
    cv.head = lat.input;
    cv.vLo = cv.vHi = cv.ys;
    cv.primeT = t;
    cv.primeK = static_cast<double>(kComplexPrimeRate) * std::max(1.0, r);
    cv.phase = ComplexVoice::kPriming;
  }

  // The vocoder frames [vHi, vHi + kComplexChunk), at sample t (read position Y, rate r): the input up to the law's
  // frame where the read will reach vHi + kComplexChunk + the output latency, + the input latency.
  void chunk(int64_t t, int64_t phi, double r) noexcept {
    StretchPool& P = bank.pool();
    float* b0 = P.output(cv.stretcher);
    if (cv.vHi + kC - cv.vLo > kVoc) {  // make room: drop what the read no longer reaches
      const int64_t from = std::clamp<int64_t>((phi >> 32) - 2, cv.vLo, cv.vHi);
      const auto keep = static_cast<size_t>(cv.vHi - from);
      for (uint32_t c = 0; c < ch; ++c) std::memmove(b0 + c * kVoc, b0 + c * kVoc + (from - cv.vLo), sizeof(float) * keep);
      cv.vLo = from;
      if (cv.vHi + kC - cv.vLo > kVoc) cv.vLo = cv.vHi;
    }
    r = std::max(r, kMinRate);
    const double Y = static_cast<double>(phi) * k2m32;
    const double tEnd = static_cast<double>(t) + (static_cast<double>(cv.vHi + kC + lat.output) - Y) / r;
    const int64_t target = std::llround(unfolded(tEnd)) - cv.u0 + lat.input;
    const int64_t inN = std::clamp<int64_t>(target - cv.head, 0, int64_t{1} << 24);
    const int64_t lead = std::max<int64_t>(0, inN - static_cast<int64_t>(bank.inFrames()));
    gather(cv.head + lead, cv.head + inN);
    formants(r);
    const float* in[2] = {bank.in(0), bank.in(ch - 1)};
    float* out[2] = {b0 + (cv.vHi - cv.vLo), b0 + kVoc + (cv.vHi - cv.vLo)};
    P.process(cv.stretcher, in, static_cast<int>(inN), static_cast<int>(lead), out, static_cast<int>(kC));
    cv.head += inN;
    cv.vHi += kC;
    ++bank.chunks;
  }
};

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
  inCap_ = std::max<uint32_t>(kComplexInFrames, static_cast<uint32_t>(pool_.latency().seek) + kComplexChunk);
  in_ = AlignedBuffer<float>(static_cast<size_t>(channels_) * inCap_);
}

void ComplexVoice::trigger(SamplerStretch* b, uint32_t lane) noexcept {  // RT-safe
  stretcher = SamplerStretch::kNone;
  phase = kWait;
  fade = 0;
  vLo = vHi = 0;
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
      ok = o.token == token && o.vHi == vHi;
    }
    if (!ok) {
      stretcher = SamplerStretch::kNone;
      phase = kWait;
    }
  }
  if (phase == kWait && usable && v.active != 0) claim(*b, *this, lane, v.t);
  // Φ per frame, as WarpVoice::render integrates it.
  alignas(32) int64_t ph[257];
  alignas(32) double rt[256];
  n = std::min<uint32_t>(n, 256);
  ph[0] = v.phi;
  for (uint32_t i = 0; i < n; ++i) {
    const double r = clampReadRate(static_cast<double>(rate[i]) * src.rateScale);
    rt[i] = r;
    ph[i + 1] = ph[i] + rateStep(r);
  }
  const int64_t t0 = v.t;
  const bool wasActive = v.active != 0;
  if (phase != kOn) {
    v.render(src, run, law, rate, n, outL, outR, pos, playing, sc);  // Tones (priming, the cross, no stretcher)
  } else {
    std::fill_n(outL, n, 0.0f);
    std::fill_n(outR, n, 0.0f);
    v.render(src, run, law, rate, n, nullptr, nullptr, pos, playing, sc);  // the clock only
  }
  if (phase == kWait || !usable || !wasActive) {
    if (v.active == 0) release(b, *this);
    return;
  }
  Run R{*b, *this, v, src, run, law, b->pool().latency()};
  R.ch = b->channels();
  const int64_t X = warpFadeFrames(run.sampleRate);
  const auto Xf = static_cast<float>(X);
  const int64_t endT = v.endT;
  const float* b0 = b->pool().output(stretcher);
  const float* b1 = R.ch >= 2 ? b0 + kVoc : b0;
  for (uint32_t i = 0; i < n; ++i) {
    const int64_t t = t0 + static_cast<int64_t>(i);
    const int64_t y = ph[i] >> 32;
    const int64_t need = y + 3 + static_cast<int64_t>(lead);
    if (phase == kPending) {
      if (t < primeT) continue;
      R.prime(t, ph[i], rt[i]);
    }
    if (phase == kPriming) {  // at most primeK vocoder frames per sample since the priming started
      const auto allowed = ys + static_cast<int64_t>(std::floor(static_cast<double>(t - primeT + 1) * primeK));
      while (vHi < need && vHi + kC <= allowed) R.chunk(t, ph[i], rt[i]);
      if (vHi < need) continue;
      phase = kFading;
      fade = 0;
    }
    while (vHi < need) R.chunk(t, ph[i], rt[i]);
    if (endT != INT64_MAX && t >= endT + X) {  // over (the WarpVoice zeroed the outputs)
      outL[i] = outR[i] = 0.0f;
      continue;
    }
    const auto at = static_cast<size_t>(y - vLo);
    const float f = static_cast<float>(static_cast<double>(ph[i] & 0xFFFFFFFF) * k2m32);
    float cl = 0.0f, cr2 = 0.0f;
    if (y - 1 >= vLo) {
      cl = cubicAt(b0 + at, f);
      cr2 = cubicAt(b1 + at, f);
    }
    if (endT != INT64_MAX && t >= endT) {  // the end's 2 ms fade (as Tones')
      const float g = rampValue((Xf - 0.5f) / Xf, -1.0f / Xf, warpRampIndex(t, endT));
      cl *= g;
      cr2 *= g;
    }
    if (phase == kFading) {
      const float g = (static_cast<float>(fade) + 0.5f) / Xf;
      outL[i] = outL[i] + g * (cl - outL[i]);
      outR[i] = outR[i] + g * (cr2 - outR[i]);
      if (++fade >= static_cast<uint32_t>(X)) phase = kOn;
    } else {
      outL[i] = cl;
      outR[i] = cr2;
    }
  }
  if (v.active == 0) {
    release(b, *this);
    return;
  }
  if (phase != kWait) b->owner(stretcher) = {token, lane, vHi};
}

}  // namespace pq::dsp
