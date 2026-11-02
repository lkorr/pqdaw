// Transfer-function estimate (pq/dsp/transfer.h).

#include "pq/dsp/transfer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "pq/dsp/fft.h"

namespace pq::dsp {

namespace {
constexpr double kPi = 3.14159265358979323846;
// Hops of a resolution before its points may show (a single window is perfectly "coherent" with anything: the averages
// must hold several before γ² means something).
constexpr uint64_t kWarmupHops = 6;

float smoothstep(float e0, float e1, float v) noexcept {
  const float t = std::clamp((v - e0) / (e1 - e0), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}
}  // namespace

// One FFT size: its window, transforms, averaged spectra per bin (struct of arrays) and per point the bins it sums and
// the estimate it gives.
struct TransferAnalyzer::Res {
  Res(uint32_t size, uint32_t hopEvery, double sr, double hop, float tau, std::span<const float> pointHz)
      : n(size), bins(size / 2 + 1), every(hopEvery), fft(size) {
    win.resize(n);
    double sum = 0.0;
    for (uint32_t i = 0; i < n; ++i) {
      const double v = 0.5 * (1.0 - std::cos(2.0 * kPi * i / n));  // periodic Hann
      win[i] = static_cast<float>(v);
      sum += v;
    }
    scale = static_cast<float>(4.0 / (sum * sum));  // |X|²·scale = amplitude² of a bin-centred sine
    frame.resize(n);
    for (std::vector<float>* v : {&xr, &xi, &yr, &yi, &sxx, &syy, &sxyR, &sxyI}) v->assign(bins, 0.0f);
    alpha = static_cast<float>(1.0 - std::exp(-hop * every / std::max(1e-3, static_cast<double>(tau))));
    const size_t P = pointHz.size();
    lo.resize(P);
    hi.resize(P);
    const double binHz = sr / n;
    for (size_t p = 0; p < P; ++p) {  // half way (in log frequency) to each neighbour; at least the nearest bin
      const double f = pointHz[p];
      const double a = p > 0 ? std::sqrt(f * pointHz[p - 1]) : f;
      const double b = p + 1 < P ? std::sqrt(f * pointHz[p + 1]) : f;
      auto clampBin = [&](double k) { return static_cast<uint32_t>(std::clamp(k, 1.0, static_cast<double>(bins - 2))); };
      uint32_t ka = clampBin(std::ceil(a / binHz)), kb = clampBin(std::floor(b / binHz));
      if (kb < ka) ka = kb = clampBin(std::round(f / binHz));
      lo[p] = ka;
      hi[p] = kb;
    }
    mag.assign(P, -200.0f);
    coh.assign(P, 0.0f);
    level.assign(P, -200.0f);
  }
  void clear() noexcept {
    for (std::vector<float>* v : {&sxx, &syy, &sxyR, &sxyI}) std::fill(v->begin(), v->end(), 0.0f);
    std::fill(mag.begin(), mag.end(), -200.0f);
    std::fill(coh.begin(), coh.end(), 0.0f);
    std::fill(level.begin(), level.end(), -200.0f);
    hops = 0;
  }
  // The window ending on the newest sample of x and y (n samples each), for the points in [p0, p1).
  void analyse(const float* x, const float* y, size_t p0, size_t p1) noexcept {
    for (uint32_t i = 0; i < n; ++i) frame[i] = x[i] * win[i];
    fft.forward(frame.data(), xr.data(), xi.data());
    for (uint32_t i = 0; i < n; ++i) frame[i] = y[i] * win[i];
    fft.forward(frame.data(), yr.data(), yi.data());
    ++hops;
    // The first hops average cumulatively (1/k), then exponentially: a usable estimate soon after the start.
    const float a = std::max(alpha, 1.0f / static_cast<float>(std::min<uint64_t>(hops, 1u << 20)));
    const float b = 1.0f - a;
    for (uint32_t k = 0; k < bins; ++k) {
      const float xre = xr[k], xim = xi[k], yre = yr[k], yim = yi[k];
      sxx[k] = b * sxx[k] + a * (xre * xre + xim * xim);
      syy[k] = b * syy[k] + a * (yre * yre + yim * yim);
      sxyR[k] = b * sxyR[k] + a * (xre * yre + xim * yim);  // conj(X)·Y
      sxyI[k] = b * sxyI[k] + a * (xre * yim - xim * yre);
    }
    for (size_t p = p0; p < p1; ++p) {
      double xx = 0.0, yy = 0.0, cr = 0.0, ci = 0.0;
      for (uint32_t k = lo[p]; k <= hi[p]; ++k) {
        xx += sxx[k];
        yy += syy[k];
        cr += sxyR[k];
        ci += sxyI[k];
      }
      const double c2 = cr * cr + ci * ci;
      const double lin = xx > 0.0 ? std::sqrt(c2) / xx : 0.0;
      mag[p] = lin > 0.0 ? static_cast<float>(20.0 * std::log10(lin)) : -200.0f;
      coh[p] = xx > 0.0 && yy > 0.0 ? static_cast<float>(std::min(1.0, c2 / (xx * yy))) : 0.0f;
      level[p] = xx > 0.0 ? static_cast<float>(10.0 * std::log10(xx * scale)) : -200.0f;
    }
  }

  uint32_t n, bins, every;
  RealFft fft;
  std::vector<float> win, frame, xr, xi, yr, yi, sxx, syy, sxyR, sxyI;
  float alpha = 0.0f, scale = 1.0f;
  uint64_t hops = 0;
  std::vector<uint32_t> lo, hi;
  std::vector<float> mag, coh, level;
};

TransferAnalyzer::TransferAnalyzer(const TransferConfig& config, std::span<const float> pointHz)
    : cfg_(config), pointHz_(pointHz.begin(), pointHz.end()) {
  cfg_.hop = std::clamp<uint32_t>(cfg_.hop, 1, cfg_.fftSize);
  const double hop = static_cast<double>(cfg_.hop) / cfg_.sampleRate;
  main_ = std::make_unique<Res>(cfg_.fftSize, 1, cfg_.sampleRate, hop, cfg_.averageSeconds, pointHz_);
  if (cfg_.lowFactor > 1) {
    const uint32_t every = std::max<uint32_t>(1, cfg_.lowFactor * cfg_.fftSize / 8 / cfg_.hop);  // 87.5 % overlap
    low_ = std::make_unique<Res>(cfg_.fftSize * cfg_.lowFactor, every, cfg_.sampleRate, hop, cfg_.lowAverageSeconds, pointHz_);
  }
  histN_ = low_ ? low_->n : main_->n;
  hx_.assign(2 * static_cast<size_t>(histN_), 0.0f);
  hy_.assign(2 * static_cast<size_t>(histN_), 0.0f);
  const size_t P = pointHz_.size();
  lowWeight_.assign(P, 0.0f);
  lowEnd_ = 0;
  if (low_) {
    const float c0 = cfg_.crossoverHz / std::sqrt(2.0f), c1 = cfg_.crossoverHz * std::sqrt(2.0f);  // one octave
    for (size_t p = 0; p < P; ++p) {
      lowWeight_[p] = 1.0f - smoothstep(0.0f, 1.0f, std::log2(std::max(pointHz_[p], 1e-3f) / c0) / std::log2(c1 / c0));
      if (lowWeight_[p] > 0.0f) lowEnd_ = p + 1;
    }
  }
  mainBegin_ = 0;
  while (mainBegin_ < P && lowWeight_[mainBegin_] >= 1.0f) ++mainBegin_;
  mag_.assign(P, cfg_.minDb);
  conf_.assign(P, 0.0f);
  coh_.assign(P, 0.0f);
  inDb_.assign(P, -200.0f);
}

TransferAnalyzer::~TransferAnalyzer() = default;

void TransferAnalyzer::reset() noexcept {
  std::fill(hx_.begin(), hx_.end(), 0.0f);
  std::fill(hy_.begin(), hy_.end(), 0.0f);
  main_->clear();
  if (low_) low_->clear();
  std::fill(mag_.begin(), mag_.end(), cfg_.minDb);
  std::fill(conf_.begin(), conf_.end(), 0.0f);
  std::fill(coh_.begin(), coh_.end(), 0.0f);
  std::fill(inDb_.begin(), inDb_.end(), -200.0f);
  histPos_ = 0;
  total_ = 0;
  sinceHop_ = 0;
  hops_ = 0;
}

bool TransferAnalyzer::anyConfident() const noexcept {
  for (const float c : conf_)
    if (c > 0.0f) return true;
  return false;
}

uint32_t TransferAnalyzer::push(const float* x, const float* y, uint32_t n) noexcept {
  uint32_t ran = 0;
  while (n > 0) {
    const uint32_t k = std::min(n, cfg_.hop - sinceHop_);
    for (uint32_t i = 0; i < k; ++i) {  // the ring, written twice: [pos] and [pos + histN_]
      hx_[histPos_] = hx_[histPos_ + histN_] = x[i];
      hy_[histPos_] = hy_[histPos_ + histN_] = y[i];
      if (++histPos_ == histN_) histPos_ = 0;
    }
    x += k;
    y += k;
    n -= k;
    total_ += k;
    sinceHop_ += k;
    if (sinceHop_ == cfg_.hop) {
      sinceHop_ = 0;
      if (total_ >= main_->n) {
        analyse();
        ++ran;
      }
    }
  }
  return ran;
}

void TransferAnalyzer::analyse() noexcept {
  ++hops_;
  // The newest n samples start at histPos_ + histN_ − n in the doubled ring.
  auto window = [&](const std::vector<float>& h, uint32_t n) { return h.data() + histPos_ + histN_ - n; };
  main_->analyse(window(hx_, main_->n), window(hy_, main_->n), mainBegin_, pointHz_.size());
  if (low_ && total_ >= low_->n && hops_ % low_->every == 0)
    low_->analyse(window(hx_, low_->n), window(hy_, low_->n), 0, lowEnd_);
  const size_t P = pointHz_.size();
  // Per point: the two resolutions mixed by the crossover weight (a point of only one uses that one).
  float loudest = -400.0f;
  for (size_t p = 0; p < P; ++p) {
    const float w = low_ && low_->hops > 0 ? lowWeight_[p] : 0.0f;
    const float m = w > 0.0f ? (w < 1.0f ? w * low_->mag[p] + (1.0f - w) * main_->mag[p] : low_->mag[p]) : main_->mag[p];
    const float c = w > 0.0f ? (w < 1.0f ? std::min(low_->coh[p], main_->coh[p]) : low_->coh[p]) : main_->coh[p];
    const float l = w > 0.0f ? (w < 1.0f ? w * low_->level[p] + (1.0f - w) * main_->level[p] : low_->level[p]) : main_->level[p];
    mag_[p] = std::clamp(m, cfg_.minDb, cfg_.maxDb);
    coh_[p] = c;
    inDb_[p] = l;
    loudest = std::max(loudest, l);
  }
  const float rel = loudest - cfg_.inputRangeDb;
  for (size_t p = 0; p < P; ++p) {
    const float w = low_ ? lowWeight_[p] : 0.0f;
    const bool warm = (w >= 1.0f || main_->hops >= kWarmupHops) && (w <= 0.0f || (low_->hops >= kWarmupHops));
    if (!warm) {
      conf_[p] = 0.0f;
      continue;
    }
    const float level = inDb_[p];
    const float c = smoothstep(cfg_.minCoherence, cfg_.fullCoherence, coh_[p]) *
                    smoothstep(cfg_.inputFloorDb, cfg_.inputFloorDb + 6.0f, level) * smoothstep(rel, rel + 6.0f, level);
    conf_[p] = c > 1e-3f ? c : 0.0f;
  }
}

}  // namespace pq::dsp
