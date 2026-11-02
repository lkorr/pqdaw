// The Spectrogram's analysis (pq/dsp/spectrogram.h). Ported from PQ Meter's spectrogram.cpp (analyzeFrame, analyzeChain,
// splat, the axis helpers and the window derivation of applyScrollSpeed / rebuildAnalysis), with its constants; the
// points go straight into open columns here instead of a list the UI splats.

#include "pq/dsp/spectrogram.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace pq::dsp {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr float kAxisCornerHz = 40.0f;   // the hybrid log axis: linear below, log above
constexpr float kLowCrossHz = 200.0f;    // the low chain owns everything below
constexpr float kLowWindowSec = 0.085f;  // the low chain's window duration (MiniMeters' 85 ms)
constexpr uint32_t kFifo = 1u << 16;     // ≥ 2 · kMaxFft + a chunk

inline float hzToMel(float f) noexcept { return 2595.0f * std::log10(1.0f + f / 700.0f); }
inline float melToHz(float m) noexcept { return 700.0f * (std::pow(10.0f, m / 2595.0f) - 1.0f); }
inline bool fromZero(float fMin) noexcept { return fMin < 0.5f; }

// Kaiser β = 6.2 (the window PQ Meter and MiniMeters ship; fixed: PQ Meter's clarity probe chose it) and its analytic
// derivative with respect to the sample index, periodic: x = 2i/N − 1.
struct WindowSample {
  double h, dh;
};
double besselI0(double x) {
  double sum = 1.0, term = 1.0;
  const double x2 = x * x * 0.25;
  for (int k = 1; term > sum * 1e-16; ++k) {
    term *= x2 / (static_cast<double>(k) * static_cast<double>(k));
    sum += term;
  }
  return sum;
}
double besselI1(double x) {
  if (std::fabs(x) < 1e-30) return 0.0;
  double sum = 1.0, term = 1.0;
  const double x2 = x * x * 0.25;
  for (int k = 1; term > sum * 1e-16; ++k) {
    term *= x2 / (static_cast<double>(k) * static_cast<double>(k + 1));
    sum += term;
  }
  return sum * x * 0.5;
}
WindowSample kaiser(uint32_t i, uint32_t n) {
  constexpr double kBeta = 6.2;
  const double denom = besselI0(kBeta);
  const double x = 2.0 * static_cast<double>(i) / static_cast<double>(n) - 1.0;
  const double r2 = 1.0 - x * x;
  const double sq = std::sqrt(std::max(0.0, r2));
  const double h = besselI0(kBeta * sq) / denom;
  const double dh = r2 > 1e-14 ? besselI1(kBeta * sq) * (-kBeta * x / sq) * (2.0 / static_cast<double>(n)) / denom : 0.0;
  return {h, dh};
}

}  // namespace

float gramUnit(GramScale scale, float f, float fMin, float fMax) noexcept {
  switch (scale) {
    case GramScale::Linear: return (f - fMin) / std::max(1e-6f, fMax - fMin);
    case GramScale::Mel: {
      const float m0 = hzToMel(fMin), m1 = hzToMel(fMax);
      return (hzToMel(f) - m0) / std::max(1e-6f, m1 - m0);
    }
    case GramScale::Log:
    default:
      if (fromZero(fMin)) {
        const float l = std::log2(fMax / kAxisCornerHz);
        const float a = 1.0f / (1.0f + 0.6931472f * l);
        const float fc = std::clamp(f, 0.0f, fMax);
        if (fc <= kAxisCornerHz) return a * fc / kAxisCornerHz;
        return a + (1.0f - a) * std::log2(fc / kAxisCornerHz) / l;
      }
      return std::log2(std::max(f, 1e-6f) / fMin) / std::log2(fMax / fMin);
  }
}

float gramHz(GramScale scale, float t, float fMin, float fMax) noexcept {
  switch (scale) {
    case GramScale::Linear: return fMin + t * (fMax - fMin);
    case GramScale::Mel: {
      const float m0 = hzToMel(fMin), m1 = hzToMel(fMax);
      return melToHz(m0 + t * (m1 - m0));
    }
    case GramScale::Log:
    default:
      if (fromZero(fMin)) {
        const float l = std::log2(fMax / kAxisCornerHz);
        const float a = 1.0f / (1.0f + 0.6931472f * l);
        const float tc = std::clamp(t, 0.0f, 1.0f);
        if (tc <= a) return kAxisCornerHz * tc / a;
        return kAxisCornerHz * std::exp2((tc - a) / (1.0f - a) * l);
      }
      return fMin * std::pow(fMax / fMin, t);
  }
}

Spectrogram::Spectrogram(const SpectrogramConfig& config) : cfg_(config) {
  fifo_.assign(kFifo, 0.0f);
  fifoMask_ = kFifo - 1;
  const uint32_t bins = kMaxFft / 2 + 1;
  binP_.resize(bins);
  binDw_.resize(bins);
  binDt_.resize(bins);
  maskP_.resize(bins);
  maskLvl_.resize(bins);
  open_.assign(static_cast<size_t>(kOpen) * kRows, 0.0f);
  build();
}

Spectrogram::~Spectrogram() = default;

void Spectrogram::buildChain(Chain& c, uint32_t n) {
  if (c.n == n && c.fft) return;
  c.n = n;
  c.fft = std::make_unique<RealFft>(n);
  c.h.resize(n);
  c.th.resize(n);
  c.dh.resize(n);
  double sum = 0.0;
  for (uint32_t i = 0; i < n; ++i) {
    const WindowSample w = kaiser(i, n);
    const double tc = static_cast<double>(i) - static_cast<double>(n) / 2.0;  // centred time, samples
    c.h[i] = static_cast<float>(w.h);
    c.th[i] = static_cast<float>(tc * w.h);
    c.dh[i] = static_cast<float>(w.dh);
    sum += w.h;
  }
  c.wSum = static_cast<float>(std::max(1e-9, sum));
  const uint32_t bins = n / 2 + 1;
  for (std::vector<float>* v : {&c.hr, &c.hi, &c.tr, &c.ti, &c.dr, &c.di}) v->assign(bins, 0.0f);
}

void Spectrogram::build() {
  const double sr = cfg_.sampleRate > 0.0 ? cfg_.sampleRate : 48000.0;
  const uint32_t base = std::bit_ceil(std::clamp<uint32_t>(cfg_.fftSize, 256u, kMaxFft));
  uint32_t hop = std::clamp<uint32_t>(cfg_.hop, 32u, kMaxFft / 2);
  // The window is a duration wearing a sample count: hold the 48 kHz window's duration at higher rates (PQ Meter's
  // rateMul), then widen it while the hop needs more than half of it (2× overlap at least: no gaps).
  uint32_t rateMul = 1;
  while (sr >= 88200.0 * rateMul && rateMul < 8) rateMul <<= 1;
  uint32_t eff = base;
  while (eff < kMaxFft && eff / base < rateMul) eff <<= 1;
  while (eff < kMaxFft && hop > eff / 2) eff <<= 1;
  hop = std::min(hop, std::max(32u, eff / 2));
  uint32_t low = 256;
  while (static_cast<double>(low) < kLowWindowSec * sr && low < kMaxFft) low <<= 1;
  fft_ = eff;
  lowFft_ = low > eff ? low : 0;
  // The open columns must hold the reach on both sides of the newest frame: ahead ≤ kOpen / 2 − 2.
  const uint32_t sp = span();
  hop = std::max(hop, (sp + (kOpen - 5)) / (kOpen - 4));
  hop_ = hop;
  ahead_ = sp / (2 * hop_) + 1;
  fMaxEff_ = std::max(1.0f, std::min(cfg_.fMax, static_cast<float>(sr * 0.5)));
  fMinEff_ = std::clamp(cfg_.fMin, 0.0f, fMaxEff_ * 0.5f);
  buildChain(main_, fft_);
  if (lowFft_ > 0) buildChain(low_, lowFft_);
  frame_.resize(sp);
  in_.resize(std::max(fft_, lowFft_));
}

void Spectrogram::configure(const SpectrogramConfig& config, Sink& sink) {
  if (config == cfg_) return;
  const bool remap = config.scale != cfg_.scale || config.fMin != cfg_.fMin || config.fMax != cfg_.fMax ||
                     config.sampleRate != cfg_.sampleRate;
  if (head_ >= 0) {
    if (remap) {
      // Rows now stand for other frequencies: the open columns' content means nothing under the new mapping. They are
      // emitted empty (the sequence continues; the display clears on the mapping change anyway).
      for (int64_t c = static_cast<int64_t>(emitted_); c <= head_; ++c)
        std::fill_n(open_.begin() + static_cast<std::ptrdiff_t>((static_cast<uint64_t>(c) % kOpen) * kRows), kRows, 0.0f);
    }
    emitThrough(head_, sink);
  }
  if (remap) ++mapping_;
  cfg_ = config;
  build();
  anchored_ = false;
  head_ = -1;
  cleared_ = static_cast<int64_t>(emitted_) - 1;
}

void Spectrogram::emitThrough(int64_t last, Sink& sink) noexcept {
  while (static_cast<int64_t>(emitted_) <= last) {
    const uint64_t c = emitted_;
    const float* col = open_.data() + (c % kOpen) * kRows;
    const double centre = (static_cast<double>(static_cast<int64_t>(c) - offset_) + 0.5) * static_cast<double>(hop_);
    sink.column(col, c, centre);
    ++emitted_;
  }
}

double Spectrogram::position() const noexcept {
  const double e = static_cast<double>(emitted_);
  if (!anchored_) return e;
  const double live = (static_cast<double>(written_) - 0.5 * static_cast<double>(span())) / static_cast<double>(hop_) +
                      static_cast<double>(offset_) - static_cast<double>(ahead_);
  return std::clamp(live, e, e + 1.0);
}

void Spectrogram::push(const float* x, uint32_t n, Sink& sink) noexcept {
  const uint32_t sp = span();
  while (n > 0) {
    const auto room = static_cast<uint32_t>(kFifo - (written_ - read_));
    const uint32_t k = std::min(n, room);
    for (uint32_t i = 0; i < k; ++i) fifo_[(written_ + i) & fifoMask_] = x[i];
    written_ += k;
    x += k;
    n -= k;
    while (written_ - read_ >= sp) {
      for (uint32_t i = 0; i < sp; ++i) frame_[i] = fifo_[(read_ + i) & fifoMask_];
      analyseFrame(frame_.data(), static_cast<double>(read_) + 0.5 * static_cast<double>(sp), sink);
      read_ += hop_;
    }
  }
}

void Spectrogram::analyseFrame(const float* frame, double centre, Sink& sink) noexcept {
  ++frames_;
  lastCentre_ = centre;
  const auto base = static_cast<int64_t>(std::floor(centre / static_cast<double>(hop_)));
  if (!anchored_) {
    // The first frame of this geometry continues the sequence: its reach back starts at the next column to emit.
    offset_ = static_cast<int64_t>(emitted_) + static_cast<int64_t>(ahead_) - base;
    anchored_ = true;
  }
  const int64_t h = base + offset_;
  // Columns no frame from here on can reach are final.
  emitThrough(h - static_cast<int64_t>(ahead_) - 1, sink);
  // Blank the columns this frame's points may reach for the first time.
  const int64_t want = h + static_cast<int64_t>(ahead_);
  for (int64_t c = std::max(cleared_ + 1, static_cast<int64_t>(emitted_)); c <= want; ++c)
    std::fill_n(open_.begin() + static_cast<std::ptrdiff_t>((static_cast<uint64_t>(c) % kOpen) * kRows), kRows, 0.0f);
  cleared_ = std::max(cleared_, want);
  head_ = h;
  const float nyquist = static_cast<float>(cfg_.sampleRate * 0.5);
  const uint32_t sp = span();
  // Main chain: the middle fft_ samples; it owns everything from the crossover up (all of it when the low chain is off).
  analyseChain(main_, frame + (sp - fft_) / 2, centre, lowFft_ > 0 ? kLowCrossHz : 1.0f, nyquist + 1.0f);
  if (lowFft_ > 0) analyseChain(low_, frame + (sp - lowFft_) / 2, centre, 1.0f, kLowCrossHz);
}

void Spectrogram::analyseChain(Chain& c, const float* x, double centre, float fLo, float fHi) noexcept {
  const uint32_t n = c.n;
  float* in = in_.data();
  for (uint32_t i = 0; i < n; ++i) in[i] = x[i] * c.h[i];
  c.fft->forward(in, c.hr.data(), c.hi.data());
  const bool re = cfg_.reassign;
  if (re) {
    for (uint32_t i = 0; i < n; ++i) in[i] = x[i] * c.th[i];
    c.fft->forward(in, c.tr.data(), c.ti.data());
    for (uint32_t i = 0; i < n; ++i) in[i] = x[i] * c.dh[i];
    c.fft->forward(in, c.dr.data(), c.di.data());
  }
  const uint32_t bins = n / 2 + 1;
  const auto sr = static_cast<float>(cfg_.sampleRate);
  const float binHz = sr / static_cast<float>(n);
  const float ampNorm = 2.0f / c.wSum;  // |X| → the sine's amplitude
  const float minAmp = std::pow(10.0f, cfg_.floorDb / 20.0f);
  const float minP = minAmp * minAmp * 0.25f;  // points a bit below the floor still land
  const float radToHz = sr / static_cast<float>(2.0 * kPi);
  float* bp = binP_.data();
  float* bw = binDw_.data();
  float* bt = binDt_.data();
  const float* hr = c.hr.data();
  const float* hi = c.hi.data();
  for (uint32_t k = 0; k < bins; ++k) bp[k] = hr[k] * hr[k] + hi[k] * hi[k];
  if (re) {
    for (uint32_t k = 0; k < bins; ++k) {
      const float p = bp[k];
      if (p > 0.0f) {
        bw[k] = (c.di[k] * hr[k] - c.dr[k] * hi[k]) / p;  // Im(X_dh · conj X_h) / |X|²
        bt[k] = (c.tr[k] * hr[k] + c.ti[k] * hi[k]) / p;  // Re(X_th · conj X_h) / |X|²
      } else {
        bw[k] = 0.0f;
        bt[k] = 0.0f;
      }
    }
  }
  // The sidelobe leakage mask (PQ Meter: a windowed float FFT of one sine dusts every bin at −78 … −112 dB; a point
  // below the leakage envelope of the frame's own strong bins describes the window, not the signal). Level l attributes
  // the block maxima up to ~2^l bins away with the deduction max(58, 36 + 20·log10 2^l) dB (power).
  {
    static const float* const kAttn = [] {
      static float a[24];
      for (int l = 0; l < 24; ++l) {
        const float ded = std::max(58.0f, 36.0f + 20.0f * std::log10(static_cast<float>(1u << l)));
        a[l] = std::pow(10.0f, -ded / 10.0f);
      }
      return a;
    }();
    float* mp = maskP_.data();
    float* ml = maskLvl_.data();
    std::fill_n(mp, bins, 0.0f);
    std::copy_n(bp, bins, ml);
    uint32_t nb = bins;
    for (int l = 0;; ++l) {
      const float attn = kAttn[l];
      for (uint32_t k = 0; k < bins; ++k) {
        const uint32_t j = k >> l;
        float m = ml[j];
        if (j > 0) m = std::max(m, ml[j - 1]);
        if (j + 1 < nb) m = std::max(m, ml[j + 1]);
        mp[k] = std::max(mp[k], m * attn);
      }
      if (nb == 1) break;
      const uint32_t nb2 = (nb + 1) / 2;
      for (uint32_t j = 0; j < nb2; ++j) {
        const float a = ml[2 * j];
        const float b = 2 * j + 1 < nb ? ml[2 * j + 1] : 0.0f;
        ml[j] = a > b ? a : b;
      }
      nb = nb2;
    }
  }
  const float tTol = cfg_.consensus * static_cast<float>(hop_);
  const float* mp = maskP_.data();
  for (uint32_t k = 1; k + 1 < bins; ++k) {
    const float p = bp[k];
    const float ampSq = p * ampNorm * ampNorm;
    if (ampSq < minP) continue;
    if (p < mp[k]) continue;  // below the leakage envelope
    float freq = static_cast<float>(k) * binHz;
    double t = centre;
    if (re) {
      const float dw = bw[k], dt = bt[k];
      const float fDev = -dw * radToHz;  // ω̂ = ω − Im(…)
      if (std::fabs(fDev) > cfg_.maxBinDev * binHz) continue;
      if (std::fabs(dt) > static_cast<float>(n) * 0.5f) continue;
      // Consensus: a real component's neighbouring bins reassign to the same frequency (a partial) or the same time
      // (an impulse); leakage and mixed bins agree with nothing.
      if (cfg_.consensus > 0.0f) {
        bool agree = false;
        for (int s = -1; s <= 1 && !agree; s += 2) {
          const uint32_t j = k + static_cast<uint32_t>(s);
          if (j < 1 || j + 1 >= bins || bp[j] <= 0.0f) continue;
          const float jf = static_cast<float>(j) * binHz - bw[j] * radToHz;
          if (std::fabs(jf - (freq + fDev)) < cfg_.consensus * binHz) agree = true;
          else if (std::fabs(bt[j] - dt) < tTol) agree = true;
        }
        if (!agree) continue;
      }
      freq += fDev;
      t += static_cast<double>(dt);
    }
    if (freq < fLo || freq >= fHi) continue;  // the chain's band (half open: exactly one chain inks a frequency)
    splat(t, freq, ampSq);
  }
}

void Spectrogram::splat(double t, float hz, float power) noexcept {
  // Out-of-range frequencies are dropped, not clamped (clamping piles them onto the edge rows).
  if (hz < fMinEff_ || hz > fMaxEff_) return;
  const double colF = t / static_cast<double>(hop_) + static_cast<double>(offset_);
  const auto cBase = static_cast<int64_t>(std::floor(colF));
  const auto cf = static_cast<float>(colF - static_cast<double>(cBase));
  const float u = std::clamp(gramUnit(cfg_.scale, std::max(hz, fMinEff_), fMinEff_, fMaxEff_), 0.0f, 1.0f);
  const float rowF = (1.0f - u) * static_cast<float>(kRows - 1);
  const auto rBase = static_cast<int32_t>(std::floor(rowF));
  const float rf = rowF - static_cast<float>(rBase);
  // A separable tent: 1.1 columns in time (joins reassigned beads along a ridge into a line), 0.6 rows in frequency (a
  // partial is a hairline; reassignment has already put it on its row).
  constexpr float kRc = 1.1f, kRr = 0.6f;
  float wc[4], wr[4];
  for (int o = -1; o <= 2; ++o) {
    wc[o + 1] = std::max(0.0f, kRc - std::fabs(static_cast<float>(o) - cf));
    wr[o + 1] = std::max(0.0f, kRr - std::fabs(static_cast<float>(o) - rf));
  }
  // Peak-normalised: a point centred on a cell paints its full power; one straddling two splits it.
  const float wcMax = std::max(std::max(wc[0], wc[1]), std::max(wc[2], wc[3]));
  const float wrMax = std::max(std::max(wr[0], wr[1]), std::max(wr[2], wr[3]));
  if (wcMax <= 0.0f || wrMax <= 0.0f) return;
  const float norm = power / (wcMax * wrMax);
  const auto lo = static_cast<int64_t>(emitted_), hi = head_ + static_cast<int64_t>(ahead_);
  ++points_;
  for (int oc = -1; oc <= 2; ++oc) {
    if (wc[oc + 1] <= 0.0f) continue;
    const int64_t c = cBase + oc;
    if (c < lo || c > hi) continue;
    float* col = open_.data() + (static_cast<uint64_t>(c) % kOpen) * kRows;
    for (int orr = -1; orr <= 2; ++orr) {
      if (wr[orr + 1] <= 0.0f) continue;
      const int32_t r = rBase + orr;
      if (r < 0 || r >= static_cast<int32_t>(kRows)) continue;
      // MAX, not sum: the overlap factor must not change the brightness (PQ Meter; MiniMeters' depth test).
      const float v = norm * wc[oc + 1] * wr[orr + 1];
      if (v > col[r]) col[r] = v;
    }
  }
}

}  // namespace pq::dsp
