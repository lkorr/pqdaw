// Spectrum analyser (pq/dsp/spectrum.h). Behaviour after PQ Meter's spectrum module (SPEC §11.1); our own code.

#include "pq/dsp/spectrum.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <span>

#include "pq/core/fast_math.h"
#include "pq/dsp/fft.h"

namespace pq::dsp {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr float kCrossLoHz = 353.55339f;   // 500 / √2
constexpr float kCrossHiHz = 707.10678f;   // 500 · √2
constexpr float kLinearCornerHz = 40.0f;   // hybrid axis (fMin = 0)
}  // namespace

// One FFT size. Powers are scaled so that amplitude = √power is |X|·2/Σw.
struct SpectrumAnalyzer::Window {
  explicit Window(uint32_t n, double sr, float noiseBins) : n(n), fft(n), binHz(static_cast<float>(sr / n)), noiseBins(noiseBins) {
    w.resize(n);
    double sum = 0.0;
    std::complex<double> w0 = 0.0, w1 = 0.0;
    for (uint32_t i = 0; i < n; ++i) {
      const double v = 0.5 * (1.0 - std::cos(2.0 * kPi * i / n));  // periodic Hann
      w[i] = static_cast<float>(v);
      sum += v;
      w0 += v;
      w1 += v * std::polar(1.0, -2.0 * kPi * i / n);
    }
    scale = static_cast<float>(4.0 / (sum * sum));
    peak3 = static_cast<float>((std::norm(w0) + 2.0 * std::norm(w1)) / std::norm(w0));  // 1.5 for Hann
    bins = n / 2 + 1;
    frame.resize(n);
    p.resize(bins + 2);
    prefix.resize(bins + 1);
    crest.resize(bins);
    half = static_cast<int32_t>(std::ceil(std::max(6.0f, 1.5f * noiseBins)));
  }

  // Window, transform, power, prefix sums. Clears the crest cache.
  void run(const float* x) noexcept {
    for (uint32_t i = 0; i < n; ++i) frame[i] = x[i] * w[i];
    fft.power(frame.data(), p.data());
    double acc = 0.0;
    prefix[0] = 0.0;
    for (uint32_t k = 0; k < bins; ++k) {
      p[k] *= scale;
      acc += p[k];
      prefix[k + 1] = acc;
    }
    std::fill(crest.begin(), crest.end(), -1.0f);
  }

  float P(int32_t k) const noexcept { return k < 0 || k >= static_cast<int32_t>(bins) ? 0.0f : p[static_cast<size_t>(k)]; }

  // Mean power over [a, b] in bin units, bins as unit cells [k − ½, k + ½].
  float meanPower(float a, float b) const noexcept {
    a = std::max(a, -0.5f);
    b = std::min(b, static_cast<float>(bins) - 0.5f);
    if (b <= a) return P(static_cast<int32_t>(std::lround(a)));
    auto integral = [&](float x) {  // ∫ from −½ to x
      const float c = x + 0.5f;
      const int32_t k = std::clamp(static_cast<int32_t>(std::floor(c)), 0, static_cast<int32_t>(bins) - 1);
      return prefix[static_cast<size_t>(k)] + static_cast<double>(c - static_cast<float>(k)) * p[static_cast<size_t>(k)];
    };
    return static_cast<float>((integral(b) - integral(a)) / static_cast<double>(b - a));
  }

  float sum3(int32_t k) const noexcept { return P(k - 1) + P(k) + P(k + 1); }

  // The expected maximum of K noise bins + 7 (the crest test's threshold), for the usual window sizes.
  float threshold(int32_t K) const noexcept {
    if (K < static_cast<int32_t>(thresholds.size())) return thresholds[static_cast<size_t>(K)];
    const float k = static_cast<float>(K);
    return std::log(k) + 0.5772157f + 0.5f / k + 7.0f;
  }

  // Tone weight from the crest test over bins [a, b].
  float crestWeight(int32_t a, int32_t b) const noexcept {
    a = std::max(a, 0);
    b = std::min(b, static_cast<int32_t>(bins) - 1);
    if (b - a < 4) return 0.0f;
    float floorSum = 0.0f;
    int32_t strongest = a;
    const float* q = p.data();
    for (int32_t i = a; i <= b - 2; ++i) floorSum += std::min(q[i], q[i + 2]);
    for (int32_t i = a + 1; i <= b; ++i)
      if (q[i] > q[strongest]) strongest = i;
    const float F = 2.0f * floorSum / static_cast<float>(b - a - 1);
    if (F <= 1e-30f) return sum3(strongest) > 1e-30f ? 1.0f : 0.0f;
    const float r = sum3(strongest) / F;
    return std::clamp((r - threshold(b - a + 1)) * 0.2f, 0.0f, 1.0f);
  }

  // Per point, the span in this window's bins (constant: computed once).
  struct Geo {
    float blo, bhi, bc;
  };
  std::vector<Geo> geo;
  void prepare(std::span<const float> lo, std::span<const float> centre, std::span<const float> hi) {
    geo.resize(lo.size());
    for (size_t i = 0; i < lo.size(); ++i) geo[i] = {lo[i] / binHz, hi[i] / binHz, centre[i] / binHz};
    thresholds.assign(64, 0.0f);
    for (int32_t K = 1; K < 64; ++K) {
      const double k = K;
      thresholds[static_cast<size_t>(K)] = static_cast<float>(std::log(k) + 0.5772157 + 0.5 / k + 7.0);
    }
  }

  // The power reading of point `pt`.
  float read(uint32_t pt) noexcept {
    const float blo = geo[pt].blo, bhi = geo[pt].bhi, bc = geo[pt].bc;
    const float width = bhi - blo;
    // Noise level.
    float noise;
    if (width >= noiseBins) noise = noiseBins * meanPower(blo, bhi);
    else if (noiseBins <= 1.0f) {
      const int32_t k = static_cast<int32_t>(std::floor(bc));
      const float f = bc - static_cast<float>(k);
      noise = P(k) + (P(k + 1) - P(k)) * f;
    } else noise = noiseBins * meanPower(bc - 0.5f * noiseBins, bc + 0.5f * noiseBins);
    // Tone level.
    float tone;
    const int32_t k0 = static_cast<int32_t>(std::ceil(blo)), k1 = static_cast<int32_t>(std::floor(bhi));
    if (k1 >= k0) {
      int32_t k = k0;
      for (int32_t i = k0 + 1; i <= k1; ++i)
        if (P(i) > P(k)) k = i;
      tone = P(k - 1) <= P(k) && P(k + 1) <= P(k) ? sum3(k) / peak3 : P(k);
    } else {
      const int32_t k = static_cast<int32_t>(std::floor(bc));
      const float f = bc - static_cast<float>(k);
      const float m = std::sqrt(P(k)) + (std::sqrt(P(k + 1)) - std::sqrt(P(k))) * f;
      tone = m * m;
      int32_t pk = static_cast<int32_t>(std::lround(bc));
      for (int s = 0; s < 3; ++s) {
        if (P(pk + 1) > P(pk)) ++pk;
        else if (P(pk - 1) > P(pk)) --pk;
        else break;
      }
      const float pp = P(pk);
      if (pp > 1e-30f) tone *= std::clamp(sum3(pk) / peak3 / pp, 1.0f, 2.0f);
    }
    // Tone or noise?
    float wgt;
    const int32_t kc = static_cast<int32_t>(std::lround(bc));
    if (width > static_cast<float>(2 * half + 1)) {
      wgt = crestWeight(static_cast<int32_t>(std::floor(blo)), static_cast<int32_t>(std::ceil(bhi)));
    } else if (kc >= 0 && kc < static_cast<int32_t>(bins)) {
      float& c = crest[static_cast<size_t>(kc)];
      if (c < 0.0f) c = crestWeight(kc - half, kc + half);
      wgt = c;
    } else wgt = 0.0f;
    return noise + wgt * (tone - noise);
  }

  uint32_t n;
  RealFft fft;
  float binHz;
  float noiseBins;
  float scale = 1.0f, peak3 = 1.5f;
  uint32_t bins = 0;
  int32_t half = 6;
  std::vector<float> w, frame, p, crest, thresholds;
  std::vector<double> prefix;
};

SpectrumAnalyzer::SpectrumAnalyzer(const SpectrumConfig& config) : cfg_(config) {
  cfg_.points = std::max<uint32_t>(cfg_.points, 2);
  cfg_.hop = std::clamp<uint32_t>(cfg_.hop, 1, cfg_.fftSize);
  const double sr = cfg_.sampleRate;
  cfg_.fMax = std::min(cfg_.fMax, static_cast<float>(0.5 * sr));
  main_ = std::make_unique<Window>(cfg_.fftSize, sr, 1.0f);
  if (cfg_.sharpLows) {
    lowN_ = std::min<uint32_t>(4 * cfg_.fftSize, 16384);
    if (lowN_ <= cfg_.fftSize) lowN_ = 0;
  }
  if (lowN_) {
    low_ = std::make_unique<Window>(lowN_, sr, static_cast<float>(lowN_) / static_cast<float>(cfg_.fftSize));
    lowEveryOther_ = lowN_ >= 8 * cfg_.hop;
  }
  histN_ = lowN_ ? lowN_ : cfg_.fftSize;
  hist_.assign(2 * static_cast<size_t>(histN_), 0.0f);
  if (cfg_.fMin <= 0.0f) {
    axisL_ = std::log2(cfg_.fMax / kLinearCornerHz);
    axisA_ = 1.0f / (1.0f + 0.69314718f * axisL_);
  }
  const uint32_t P = cfg_.points;
  pointHz_.resize(P);
  spanLo_.resize(P);
  spanHi_.resize(P);
  tilt_.resize(P);
  lowWeight_.resize(P);
  const float nyq = static_cast<float>(0.5 * sr);
  for (uint32_t p = 0; p < P; ++p) {
    const float t = static_cast<float>(p) / static_cast<float>(P - 1);
    const float dt = 0.5f / static_cast<float>(P - 1);
    pointHz_[p] = axisHz(t);
    spanLo_[p] = std::clamp(axisHz(std::max(0.0f, t - dt)), 0.0f, nyq);
    spanHi_[p] = std::clamp(axisHz(std::min(1.0f, t + dt)), 0.0f, nyq);
    if (p == 0) spanLo_[p] = std::max(0.0f, pointHz_[p] - (spanHi_[p] - pointHz_[p]));
    if (p == P - 1) spanHi_[p] = std::min(nyq, pointHz_[p] + (pointHz_[p] - spanLo_[p]));
    tilt_[p] = cfg_.tiltDbPerOct * std::log2(std::max(pointHz_[p], 1.0f) / cfg_.tiltPivotHz);
    const float u = std::clamp(std::log2(std::max(pointHz_[p], 1e-3f) / kCrossLoHz), 0.0f, 1.0f);
    lowWeight_[p] = lowN_ ? 1.0f - u * u * (3.0f - 2.0f * u) : 0.0f;
  }
  main_->prepare(spanLo_, pointHz_, spanHi_);
  if (low_) low_->prepare(spanLo_, pointHz_, spanHi_);
  levels_.resize(P);
  peaks_.resize(P);
  peakAge_.resize(P);
  reading_.resize(P);
  hopSec_ = static_cast<float>(cfg_.hop / sr);
  attackA_ = 1.0f - std::exp(-hopSec_ / std::max(1e-6f, cfg_.attackMs * 1e-3f));
  releaseA_ = 1.0f - std::exp(-hopSec_ / std::max(1e-6f, cfg_.releaseMs * 1e-3f));
  reset();
}

SpectrumAnalyzer::~SpectrumAnalyzer() = default;

void SpectrumAnalyzer::reset() noexcept {
  std::fill(hist_.begin(), hist_.end(), 0.0f);
  histPos_ = 0;
  total_ = 0;
  sinceHop_ = 0;
  lowValid_ = false;
  std::fill(levels_.begin(), levels_.end(), cfg_.floorDb);
  std::fill(peaks_.begin(), peaks_.end(), cfg_.floorDb);
  std::fill(peakAge_.begin(), peakAge_.end(), 0.0f);
  std::fill(reading_.begin(), reading_.end(), cfg_.floorDb);
  hops_ = 0;
}

float SpectrumAnalyzer::axisHz(float t) const noexcept {
  t = std::clamp(t, 0.0f, 1.0f);
  if (cfg_.fMin > 0.0f) return cfg_.fMin * std::pow(cfg_.fMax / cfg_.fMin, t);
  if (t <= axisA_) return kLinearCornerHz * t / axisA_;
  return kLinearCornerHz * std::exp2((t - axisA_) / (1.0f - axisA_) * axisL_);
}

float SpectrumAnalyzer::axisT(float hz) const noexcept {
  if (cfg_.fMin > 0.0f) return std::log(std::max(hz, 1e-6f) / cfg_.fMin) / std::log(cfg_.fMax / cfg_.fMin);
  if (hz <= kLinearCornerHz) return hz / kLinearCornerHz * axisA_;
  return axisA_ + (1.0f - axisA_) * std::log2(hz / kLinearCornerHz) / axisL_;
}

bool SpectrumAnalyzer::allBelow(float db) const noexcept {
  for (float v : levels_)
    if (v > db) return false;
  return true;
}

uint32_t SpectrumAnalyzer::push(const float* x, uint32_t n) noexcept {
  uint32_t ran = 0;
  for (uint32_t i = 0; i < n; ++i) {
    float v = x[i];
    if (!std::isfinite(v)) v = 0.0f;
    hist_[histPos_] = v;
    hist_[histPos_ + histN_] = v;
    if (++histPos_ == histN_) histPos_ = 0;
    ++total_;
    if (++sinceHop_ >= cfg_.hop && total_ >= cfg_.fftSize) {
      sinceHop_ = 0;
      analyse();
      ++ran;
    }
  }
  return ran;
}

void SpectrumAnalyzer::analyse() noexcept {
  // The window of the newest histN_ samples is hist_[histPos_ … histPos_ + histN_).
  const float* newest = hist_.data() + histPos_ + histN_;  // one past the newest sample
  main_->run(newest - cfg_.fftSize);
  bool useLow = false;
  if (low_ && total_ >= lowN_) {
    if (!lowEveryOther_ || !lowValid_ || (hops_ & 1u) == 0) low_->run(newest - lowN_);
    lowValid_ = true;
    useLow = true;
  }
  const uint32_t P = cfg_.points;
  const float smoothHalf = 0.5f * cfg_.smoothingOct;
  for (uint32_t p = 0; p < P; ++p) {
    const float f = pointHz_[p];
    const float lw = useLow ? lowWeight_[p] : 0.0f;
    float power = 0.0f;
    if (lw < 1.0f) {  // below the crossover the long window alone is read (the main reading would get weight 0)
      power = main_->read(p);
      if (smoothHalf > 0.0f) {  // 1/N-octave smoothing where the band is wider than the window's resolution
        const float slo = f * std::exp2(-smoothHalf), shi = f * std::exp2(smoothHalf);
        if ((shi - slo) / main_->binHz >= main_->peak3) power = main_->meanPower(slo / main_->binHz, shi / main_->binHz);
      }
    }
    if (lw > 0.0f) {
      float lp = low_->read(p);
      if (smoothHalf > 0.0f) {
        const float slo = f * std::exp2(-smoothHalf), shi = f * std::exp2(smoothHalf);
        if ((shi - slo) / low_->binHz >= low_->noiseBins * low_->peak3)
          lp = low_->noiseBins * low_->meanPower(slo / low_->binHz, shi / low_->binHz);
      }
      power = lw * lp + (1.0f - lw) * power;
    }
    // Tilt only on signal: silence stays at the floor at every frequency.
    // 10·log10 through the fast log2 (4.5e-6 absolute in log2: 1.4e-5 dB, far below what a display shows).
    const float db = power > 1e-14f ? std::max(3.0102999566f * fastLog2(power) + tilt_[p], cfg_.floorDb) : cfg_.floorDb;
    reading_[p] = db;
    float& cur = levels_[p];
    cur += (db - cur) * (db > cur ? attackA_ : releaseA_);
    if (cfg_.peakHold) {
      float& pk = peaks_[p];
      if (cur >= pk) {
        pk = cur;
        peakAge_[p] = 0.0f;
      } else {
        peakAge_[p] += hopSec_;
        if (peakAge_[p] > cfg_.holdSeconds) pk = std::max(cur, pk - cfg_.fallDbPerSecond * hopSec_);
      }
    }
  }
  ++hops_;
}

}  // namespace pq::dsp
