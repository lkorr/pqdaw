// Spectrum analyser (pq/dsp/spectrum.h). Ported from PQ Meter's spectrum module and dsp/spectrum_grid.h (SPEC §11.1).

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
constexpr float kNodeUnset = 1e30f;        // a dense-read node not read yet this hop
constexpr float kSoundPower = 1e-10f;      // a bin above this (−100 dBFS amplitude): the hop counts for the average
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
    node.resize(bins);
    half = static_cast<int32_t>(std::ceil(std::max(6.0f, 1.5f * noiseBins)));
  }

  // Window, transform, power, prefix sums. Clears the crest cache.
  void run(const float* x) noexcept {
    for (uint32_t i = 0; i < n; ++i) frame[i] = x[i] * w[i];
    fft.power(frame.data(), p.data());
    double acc = 0.0;
    float mx = 0.0f;
    prefix[0] = 0.0;
    for (uint32_t k = 0; k < bins; ++k) {
      p[k] *= scale;
      acc += p[k];
      mx = std::max(mx, p[k]);
      prefix[k + 1] = acc;
    }
    maxPower = mx;
    std::fill(crest.begin(), crest.end(), -1.0f);
    std::fill(node.begin(), node.end(), kNodeUnset);
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

  // The crest weight of the ≥ 13-bin window round bin kc, memoised for the hop.
  float crestAt(int32_t kc) noexcept {
    if (kc < 0 || kc >= static_cast<int32_t>(bins)) return 0.0f;
    float& c = crest[static_cast<size_t>(kc)];
    if (c < 0.0f) c = crestWeight(kc - half, kc + half);
    return c;
  }

  // The reading AT bin centre b as log2 power, memoised for the hop (PQ spectrum_grid.h nodeLn): the point reader's
  // noise / tone blend, the tone being the bin lifted by the 3-bin correction of the lobe it sits on.
  float nodeLog(int32_t b) noexcept {
    b = std::clamp(b, 0, static_cast<int32_t>(bins) - 1);
    float& memo = node[static_cast<size_t>(b)];
    if (memo != kNodeUnset) return memo;
    const float fb = static_cast<float>(b);
    const float noise = noiseBins <= 1.0f ? P(b) : noiseBins * meanPower(fb - 0.5f * noiseBins, fb + 0.5f * noiseBins);
    float r = noise;
    const float wt = crestAt(b);
    if (wt > 0.0f) {
      const int32_t last = static_cast<int32_t>(bins) - 1;
      int32_t pk = b;
      for (int k = 0; k < 3 && pk < last && P(pk + 1) > P(pk); ++k) ++pk;
      for (int k = 0; k < 3 && pk > 0 && P(pk - 1) > P(pk); ++k) --pk;
      const float pp = P(pk);
      const float corr = pp > 0.0f ? std::clamp(sum3(pk) / peak3 / pp, 1.0f, 2.0f) : 1.0f;
      const float tone = P(b) * corr;
      r = wt >= 1.0f ? tone : noise + wt * (tone - noise);
    }
    // 1e-30 (−300 dB): under any floor, and keeps the log finite through digital silence.
    memo = fastLog2(std::max(r, 1e-30f));
    return memo;
  }

  // A point between bin centres (points denser than bins): a monotone cubic (Fritsch–Carlson PCHIP) through the node
  // readings in log power — C1, no overshoot, and every bin centre reads its node exactly (PQ denseRead). It replaces
  // the linear join of the two neighbouring bins (an elbow at every bin, a step where the nearest bin changed).
  float denseRead(float bc) noexcept {
    const int32_t i0 = std::clamp(static_cast<int32_t>(std::floor(bc)), 0, static_cast<int32_t>(bins) - 2);
    const float t = std::clamp(bc - static_cast<float>(i0), 0.0f, 1.0f);
    const float lm = nodeLog(i0 - 1), l0 = nodeLog(i0), l1 = nodeLog(i0 + 1), l2 = nodeLog(i0 + 2);
    auto slope = [](float a, float b) { return a * b <= 0.0f ? 0.0f : 2.0f * a * b / (a + b); };
    const float d0 = slope(l0 - lm, l1 - l0), d1 = slope(l1 - l0, l2 - l1);
    const float t2 = t * t, t3 = t2 * t;
    const float l = (2.0f * t3 - 3.0f * t2 + 1.0f) * l0 + (t3 - 2.0f * t2 + t) * d0 + (3.0f * t2 - 2.0f * t3) * l1 +
                    (t3 - t2) * d1;
    return fastExp2(std::max(l, -126.0f));
  }

  // The power reading of point `pt` (PQ SpectrumGrid::readPoint).
  float read(uint32_t pt) noexcept {
    const float blo = geo[pt].blo, bhi = geo[pt].bhi, bc = geo[pt].bc;
    const float width = bhi - blo;
    if (width < 1.0f) return denseRead(bc);
    // Noise: the mean per reference bin over the span, or over one reference bin round the point when narrower.
    const float noise = width >= noiseBins ? noiseBins * meanPower(blo, bhi)
                                           : noiseBins * meanPower(bc - 0.5f * noiseBins, bc + 0.5f * noiseBins);
    // Tone: the strongest bin inside the span, 3-bin corrected when it is a lobe peak.
    const int32_t k0 = std::max(0, static_cast<int32_t>(std::ceil(blo)));
    const int32_t k1 = std::min(static_cast<int32_t>(bins) - 1, static_cast<int32_t>(std::floor(bhi)));
    if (k1 < k0) return denseRead(bc);  // (only the clamps at the grid's ends empty a span of a bin or more)
    int32_t k = k0;
    for (int32_t i = k0 + 1; i <= k1; ++i)
      if (P(i) > P(k)) k = i;
    const float tone = P(k - 1) <= P(k) && P(k + 1) <= P(k) ? sum3(k) / peak3 : P(k);
    // Tone or noise?
    const float wgt = width > static_cast<float>(2 * half + 1)
                          ? crestWeight(static_cast<int32_t>(std::floor(blo)), static_cast<int32_t>(std::ceil(bhi)))
                          : crestAt(static_cast<int32_t>(std::lround(bc)));
    return noise + wgt * (tone - noise);
  }

  uint32_t n;
  RealFft fft;
  float binHz;
  float noiseBins;
  float scale = 1.0f, peak3 = 1.5f, maxPower = 0.0f;
  uint32_t bins = 0;
  int32_t half = 6;
  std::vector<float> w, frame, p, crest, node, thresholds;
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
  cfg_.fMin = std::clamp(cfg_.fMin, 0.0f, 0.5f * cfg_.fMax);
  axis_ = FreqAxis(cfg_.scale, cfg_.fMin, cfg_.fMax);
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
  avgPow_.resize(P);
  hopSec_ = static_cast<float>(cfg_.hop / sr);
  applyLive();
  reset();
}

void SpectrumAnalyzer::applyLive() noexcept {
  attackA_ = 1.0f - std::exp(-hopSec_ / std::max(1e-6f, cfg_.attackMs * 1e-3f));
  releaseA_ = 1.0f - std::exp(-hopSec_ / std::max(1e-6f, cfg_.releaseMs * 1e-3f));
  avgA_ = cfg_.averageSeconds > 1e-4f ? 1.0f - std::exp(-hopSec_ / cfg_.averageSeconds) : 1.0f;
}

void SpectrumAnalyzer::setLive(const SpectrumLive& l) noexcept {
  const bool avgStarts = l.average && !cfg_.average;
  if (l.peakHold && !cfg_.peakHold) {  // the hold starts from what is shown now
    std::copy(levels_.begin(), levels_.end(), peaks_.begin());
    std::fill(peakAge_.begin(), peakAge_.end(), 0.0f);
  }
  cfg_.attackMs = l.attackMs;
  cfg_.releaseMs = l.releaseMs;
  cfg_.peakHold = l.peakHold;
  cfg_.holdSeconds = l.holdSeconds;
  cfg_.fallDbPerSecond = l.fallDbPerSecond;
  cfg_.average = l.average;
  cfg_.averageSeconds = l.averageSeconds;
  applyLive();
  if (avgStarts) {
    std::fill(avgPow_.begin(), avgPow_.end(), 0.0f);
    avgWeight_ = 0.0f;
  }
}

void SpectrumAnalyzer::averageDb(float* out) const noexcept {
  const uint32_t P = cfg_.points;
  if (avgWeight_ <= 1e-6f) {
    std::fill_n(out, P, cfg_.floorDb);
    return;
  }
  const float inv = 1.0f / avgWeight_;
  for (uint32_t p = 0; p < P; ++p) {
    const float pw = avgPow_[p] * inv;
    out[p] = pw > 1e-14f ? std::max(3.0102999566f * fastLog2(pw), cfg_.floorDb) : cfg_.floorDb;
  }
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
  std::fill(avgPow_.begin(), avgPow_.end(), 0.0f);
  avgWeight_ = 0.0f;
  hops_ = 0;
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
  // The average counts hops with something in them (a pause between songs must not sink it).
  const bool avgOn = cfg_.average && main_->maxPower > kSoundPower;
  if (avgOn) avgWeight_ += (1.0f - avgWeight_) * avgA_;
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
    // The average in power, not dB: a dB mean reads a band with sparse hits low. 10^(db / 10) = 2^(db · 0.3322).
    if (avgOn) avgPow_[p] += ((db > cfg_.floorDb ? fastExp2(db * 0.33219281f) : 0.0f) - avgPow_[p]) * avgA_;
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
