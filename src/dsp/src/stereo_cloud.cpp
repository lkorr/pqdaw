// The Stereo Spectrogram's analysis (VZ.7): pq/dsp/stereo_cloud.h. Ported from PQ Meter (modules/stereo_spectrogram.cpp).

#include "pq/dsp/stereo_cloud.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "pq/dsp/fft.h"

namespace pq::dsp {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kBeta = 6.2;

double besselI0(double x) noexcept {
  double sum = 1.0, term = 1.0;
  const double x2 = x * x * 0.25;
  for (int k = 1; term > sum * 1e-16; ++k) {
    term *= x2 / (static_cast<double>(k) * k);
    sum += term;
  }
  return sum;
}
double besselI1(double x) noexcept {
  if (std::abs(x) < 1e-30) return 0.0;
  double sum = 1.0, term = 1.0;
  const double x2 = x * x * 0.25;
  for (int k = 1; term > sum * 1e-16; ++k) {
    term *= x2 / (static_cast<double>(k) * (k + 1));
    sum += term;
  }
  return sum * x * 0.5;
}
// PQ dsp/fft_windows.h windowAt(kWinKaiser): h and dh/dn.
void kaiser(uint32_t i, uint32_t n, double& h, double& dh) noexcept {
  const double den = besselI0(kBeta);
  const double x = 2.0 * i / n - 1.0, r2 = 1.0 - x * x, sq = std::sqrt(std::max(0.0, r2));
  h = besselI0(kBeta * sq) / den;
  dh = r2 > 1e-14 ? besselI1(kBeta * sq) * (-kBeta * x / sq) * (2.0 / n) / den : 0.0;
}

}  // namespace

StereoCloud::StereoCloud(const StereoCloudConfig& c) {
  cfg_.sampleRate = 0.0;
  configure(c);
}
StereoCloud::~StereoCloud() = default;

void StereoCloud::configure(const StereoCloudConfig& c) {
  StereoCloudConfig n = c;
  uint32_t p = 512;
  while (p * 2 <= std::clamp<uint32_t>(c.fftSize, 512, 8192)) p *= 2;
  n.fftSize = p;
  n.loFactor = c.loFactor >= 8 ? 8 : c.loFactor >= 4 ? 4 : c.loFactor >= 2 ? 2 : 1;
  n.fMax = std::min(c.fMax, static_cast<float>(c.sampleRate * 0.5));
  n.fMin = std::clamp(c.fMin, 1.0f, n.fMax * 0.5f);
  const bool restart = n.sampleRate != cfg_.sampleRate || n.fftSize != cfg_.fftSize || n.loFactor != cfg_.loFactor ||
                       n.scale != cfg_.scale || n.fMin != cfg_.fMin || n.fMax != cfg_.fMax || !fft_;
  cfg_ = n;
  if (!restart) return;
  const uint32_t N = n.fftSize, loN = std::min<uint32_t>(N * n.loFactor, 16384);
  axis_ = FreqAxis(n.scale, n.fMin, n.fMax);
  fft_ = std::make_unique<RealFft>(N);
  loFft_ = std::make_unique<RealFft>(loN);
  winH_.resize(N);
  winTh_.resize(N);
  winDh_.resize(N);
  double s1 = 0.0, s2 = 0.0;
  for (uint32_t i = 0; i < N; ++i) {
    double h, dh;
    kaiser(i, N, h, dh);
    winH_[i] = static_cast<float>(h);
    winTh_[i] = static_cast<float>((static_cast<double>(i) - N / 2.0) * h);  // the centred-time ramp
    winDh_[i] = static_cast<float>(dh);
    s1 += h;
    s2 += h * h;
  }
  winSum_ = static_cast<float>(std::max(1e-9, s1));
  enbw_ = static_cast<float>(s2 * N / (s1 * s1));
  loWin_.resize(loN);
  s1 = s2 = 0.0;
  for (uint32_t i = 0; i < loN; ++i) {
    double h, dh;
    kaiser(i, loN, h, dh);
    loWin_[i] = static_cast<float>(h);
    s1 += h;
    s2 += h * h;
  }
  loWinSum_ = static_cast<float>(std::max(1e-9, s1));
  loEnbw_ = static_cast<float>(s2 * loN / (s1 * s1));
  uint32_t cap = 1024;
  while (cap < loN) cap *= 2;
  if (hist_[0].size() != cap) {  // (the stream index keeps counting: the display's time axis is absolute)
    for (auto& h : hist_) h.assign(cap, 0.0f);
    mask_ = cap - 1;
    at_ = 0;
  }
  in_.resize(loN);
  const uint32_t bins = N / 2 + 1, loBins = loN / 2 + 1;
  for (int ch = 0; ch < 2; ++ch) {
    for (auto* v : {&hRe_[ch], &hIm_[ch], &tRe_[ch], &tIm_[ch], &dRe_[ch], &dIm_[ch]}) v->assign(bins, 0.0f);
    loRe_[ch].assign(loBins, 0.0f);
    loIm_[ch].assign(loBins, 0.0f);
  }
  for (auto* v : {&binP_, &binDw_, &binDt_, &maskP_, &maskLvl_}) v->assign(bins, 0.0f);
  batch_.clear();
  batch_.reserve(static_cast<size_t>(bins) * 2);
  scratch_.reserve(static_cast<size_t>(bins) * 2);
  band_.reserve(static_cast<size_t>(bins) * 2);
  acc_.fill({});
  accLo_.fill({});
  for (uint32_t b = 0; b < kRibBands; ++b) {
    const float inv = 1.0f / (kRibBands - 1);
    ribHz_[b] = axis_.hz(b * inv);
    ribPrevHz_[b] = axis_.hz((b == 0 ? 0 : b - 1) * inv);
    ribNextHz_[b] = axis_.hz(std::min(kRibBands - 1, b + 1) * inv);
  }
  rib_.pos.fill(0.0f);
  rib_.width.fill(0.0f);
  rib_.db.fill(-140.0f);
  since_ = 0;
  ++gen_;
}

void StereoCloud::write(const float* l, const float* r, uint32_t n) noexcept {
  float m = 0.0f;
  for (uint32_t i = 0; i < n; ++i) {
    hist_[0][(at_ + i) & mask_] = l[i];
    hist_[1][(at_ + i) & mask_] = r[i];
    m = std::max(m, std::max(std::fabs(l[i]), std::fabs(r[i])));
  }
  at_ = (at_ + n) & mask_;
  written_ += n;
  quiet_ = m < 1e-7f ? quiet_ + n : 0;
}

void StereoCloud::frame() noexcept {
  const uint32_t N = cfg_.fftSize;
  head_ = static_cast<double>(written_) - N * 0.5;  // the frame's centre
  auto transform = [&](int ch, const std::vector<float>& w, std::vector<float>& re, std::vector<float>& im) {
    const uint32_t start = (at_ - N) & mask_;
    for (uint32_t i = 0; i < N; ++i) in_[i] = hist_[ch][(start + i) & mask_] * w[i];
    fft_->forward(in_.data(), re.data(), im.data());
  };
  for (int ch = 0; ch < 2; ++ch) {
    transform(ch, winH_, hRe_[ch], hIm_[ch]);
    if (cfg_.reassign) {
      transform(ch, winTh_, tRe_[ch], tIm_[ch]);
      transform(ch, winDh_, dRe_[ch], dIm_[ch]);
    }
  }
  {  // the ribbon's long window (the newest loN samples)
    const uint32_t loN = static_cast<uint32_t>(loWin_.size());
    for (int ch = 0; ch < 2; ++ch) {
      const uint32_t start = (at_ - loN) & mask_;
      for (uint32_t i = 0; i < loN; ++i) in_[i] = hist_[ch][(start + i) & mask_] * loWin_[i];
      loFft_->forward(in_.data(), loRe_[ch].data(), loIm_[ch].data());
    }
  }
  batch_.clear();
  // Per channel (anti-phase content would vanish from a mono fold; hard-panned content read 6 dB low).
  analyzeChannel(0);
  analyzeChannel(1);
  capBatch();
  ribbon();
}

// PQ analyzeChannel: the reassignment pass with the leakage mask and the consensus test.
void StereoCloud::analyzeChannel(int ch) noexcept {
  const uint32_t N = cfg_.fftSize, nb = N / 2 + 1, hop = this->hop();
  const float binHz = static_cast<float>(cfg_.sampleRate / N), ampNorm = 2.0f / winSum_;
  const float minAmp = std::pow(10.0f, -140.0f / 20.0f), minP = minAmp * minAmp * 0.25f;
  const float radToHz = static_cast<float>(cfg_.sampleRate / (2.0 * kPi)), nyq = static_cast<float>(cfg_.sampleRate * 0.5);
  const auto &re = hRe_[ch], &im = hIm_[ch];
  for (uint32_t k = 0; k < nb; ++k) binP_[k] = re[k] * re[k] + im[k] * im[k];
  if (cfg_.reassign)
    for (uint32_t k = 0; k < nb; ++k) {
      const float p = binP_[k];
      binDw_[k] = p > 0.0f ? (dIm_[ch][k] * re[k] - dRe_[ch][k] * im[k]) / p : 0.0f;
      binDt_[k] = p > 0.0f ? (tRe_[ch][k] * re[k] + tIm_[ch][k] * im[k]) / p : 0.0f;
    }
  {  // the sidelobe leakage envelope of the frame's own strong bins (a max pyramid, octave by octave)
    constexpr float kNearDb = 58.0f, kBaseDb = 36.0f;
    std::fill(maskP_.begin(), maskP_.end(), 0.0f);
    std::copy(binP_.begin(), binP_.end(), maskLvl_.begin());
    uint32_t n = nb;
    for (int l = 0;; ++l) {
      const float dist = static_cast<float>(1 << l);
      const float attn = std::pow(10.0f, -std::max(kNearDb, kBaseDb + 20.0f * std::log10(dist)) / 10.0f);
      for (uint32_t k = 0; k < nb; ++k) {
        const uint32_t j = k >> l;
        float m = maskLvl_[j];
        if (j > 0) m = std::max(m, maskLvl_[j - 1]);
        if (j + 1 < n) m = std::max(m, maskLvl_[j + 1]);
        maskP_[k] = std::max(maskP_[k], m * attn);
      }
      if (n == 1) break;
      const uint32_t n2 = (n + 1) / 2;
      for (uint32_t j = 0; j < n2; ++j) maskLvl_[j] = std::max(maskLvl_[2 * j], 2 * j + 1 < n ? maskLvl_[2 * j + 1] : 0.0f);
      n = n2;
    }
  }
  const double center = head_;
  const float consensus = cfg_.consensus;
  for (uint32_t k = 1; k + 1 < nb; ++k) {
    const float p = binP_[k];
    if (p * ampNorm * ampNorm < minP || p < maskP_[k]) continue;
    float freq = static_cast<float>(k) * binHz;
    double t = center;
    if (cfg_.reassign) {
      const float dw = binDw_[k], dt = binDt_[k];
      const float fDev = -dw * radToHz;
      if (std::abs(fDev) > 3.5f * binHz || std::abs(dt) > N * 0.5f) continue;
      if (consensus > 0.0f) {  // a genuine component's neighbours land where it does, in frequency or in time
        bool agree = false;
        for (int s = -1; s <= 1 && !agree; s += 2) {
          const uint32_t j = k + s;
          if (j < 1 || j >= nb - 1 || binP_[j] <= 0.0f) continue;
          const float jf = static_cast<float>(j) * binHz - binDw_[j] * radToHz;
          agree = std::abs(jf - (freq + fDev)) < consensus * binHz || std::abs(binDt_[j] - dt) < consensus * hop;
        }
        if (!agree) continue;
      }
      freq += fDev;
      t += dt;
    }
    if (freq < 1.0f || freq > nyq) continue;
    const double pl = static_cast<double>(hRe_[0][k]) * hRe_[0][k] + static_cast<double>(hIm_[0][k]) * hIm_[0][k];
    const double pr = static_cast<double>(hRe_[1][k]) * hRe_[1][k] + static_cast<double>(hIm_[1][k]) * hIm_[1][k];
    const double total = pl + pr;
    if (!(total > 1e-20)) continue;
    if (batch_.size() < batch_.capacity())
      batch_.push_back({t, freq, static_cast<float>((pr - pl) / total), static_cast<float>(total) * ampNorm * ampNorm});
  }
}

// PQ capBatchByBand: the hop's points capped per band of the display axis (a floor on each band's share).
void StereoCloud::capBatch() noexcept {
  const size_t B = batch_.size();
  if (B <= kMaxPointsPerHop) return;
  band_.resize(B);
  scratch_.resize(B);
  long count[kPointBands] = {}, begin[kPointBands], end[kPointBands];
  for (size_t i = 0; i < B; ++i) {
    const float u = axis_.unit(std::clamp(batch_[i].freqHz, cfg_.fMin, cfg_.fMax));
    band_[i] = static_cast<uint8_t>(std::clamp(static_cast<int>(u * kPointBands), 0, static_cast<int>(kPointBands) - 1));
    ++count[band_[i]];
  }
  long acc = 0;
  for (uint32_t b = 0; b < kPointBands; ++b) begin[b] = acc, acc += count[b], end[b] = acc;
  {
    long cur[kPointBands];
    std::copy(begin, begin + kPointBands, cur);
    for (size_t i = 0; i < B; ++i) scratch_[static_cast<size_t>(cur[band_[i]]++)] = batch_[i];
  }
  int keep[kPointBands], spare = 0;
  for (uint32_t b = 0; b < kPointBands; ++b) {
    keep[b] = static_cast<int>(std::min<long>(count[b], kMaxPointsPerBand));
    spare += static_cast<int>(kMaxPointsPerBand) - keep[b];
  }
  while (spare > 0) {
    int hungry = 0;
    for (uint32_t b = 0; b < kPointBands; ++b) hungry += count[b] > keep[b] ? 1 : 0;
    if (hungry == 0) break;
    const int share = std::max(1, spare / hungry);
    for (uint32_t b = 0; b < kPointBands && spare > 0; ++b) {
      if (count[b] <= keep[b]) continue;
      const int add = std::min({share, spare, static_cast<int>(count[b] - keep[b])});
      keep[b] += add;
      spare -= add;
    }
  }
  size_t out = 0;
  for (uint32_t b = 0; b < kPointBands; ++b) {
    const auto first = scratch_.begin() + begin[b];
    if (keep[b] < count[b])
      std::nth_element(first, first + keep[b], scratch_.begin() + end[b], [](const CloudPoint& a, const CloudPoint& c) { return a.power > c.power; });
    for (int i = 0; i < keep[b]; ++i) batch_[out++] = scratch_[static_cast<size_t>(begin[b] + i)];
  }
  batch_.resize(out);
}

// PQ ribbonFrame: the Stereo Image's per-band analysis on the plain spectra, the long window under ~700 Hz.
void StereoCloud::ribbon() noexcept {
  const uint32_t N = cfg_.fftSize, nb = N / 2 + 1, loN = static_cast<uint32_t>(loWin_.size()), loNb = loN / 2 + 1;
  const float binHz = static_cast<float>(cfg_.sampleRate / N), loBinHz = static_cast<float>(cfg_.sampleRate / loN);
  const float norm = 2.0f / winSum_, loNorm = 2.0f / loWinSum_;
  const float hopSec = static_cast<float>(hop() / cfg_.sampleRate);
  auto coef = [&](float ms) { return 1.0f - std::exp(-hopSec / std::max(1e-4f, ms * 0.001f)); };
  const float aAtk = coef(cfg_.ribAttackMs), aRel = coef(cfg_.ribReleaseMs), aCoh = coef(cfg_.ribCoherenceMs);
  const float blendLo = 500.0f * std::exp2(-0.5f), blendHi = 500.0f * std::exp2(0.5f);
  struct Sums {
    double pL = 0.0, pR = 0.0, xRe = 0.0, xIm = 0.0;
  };
  auto sums = [](const std::vector<float>* re, const std::vector<float>* im, uint32_t bins, float gHz, float f, float fp, float fn) {
    float lo = 0.5f * (fp + f) / gHz, hi = 0.5f * (f + fn) / gHz;
    if (hi - lo < 2.0f) lo = f / gHz - 1.0f, hi = f / gHz + 1.0f;
    const uint32_t b0 = static_cast<uint32_t>(std::clamp(static_cast<int>(std::floor(lo)), 1, static_cast<int>(bins) - 1));
    const uint32_t b1 = static_cast<uint32_t>(std::clamp(static_cast<int>(std::ceil(hi)), 1, static_cast<int>(bins) - 1));
    Sums s;
    for (uint32_t b = b0; b <= b1; ++b) {
      const double lr = re[0][b], li = im[0][b], rr = re[1][b], ri = im[1][b];
      s.pL += lr * lr + li * li;
      s.pR += rr * rr + ri * ri;
      s.xRe += lr * rr + li * ri;
      s.xIm += li * rr - lr * ri;
    }
    return s;
  };
  auto width = [&](Accum& a, const Sums& s) {
    a.sL += (s.pL - a.sL) * aCoh;
    a.sR += (s.pR - a.sR) * aCoh;
    a.xRe += (s.xRe - a.xRe) * aCoh;
    a.xIm += (s.xIm - a.xIm) * aCoh;
    if (a.sL > 1e-24 && a.sR > 1e-24)
      return 1.0f - static_cast<float>(std::clamp(std::sqrt(a.xRe * a.xRe + a.xIm * a.xIm) / std::sqrt(a.sL * a.sR), 0.0, 1.0));
    return 0.0f;
  };
  rib_.tSamples = head_;
  for (uint32_t b = 0; b < kRibBands; ++b) {
    const float f = ribHz_[b];
    float wLo = 0.0f;
    if (f < blendHi) {
      if (f <= blendLo) wLo = 1.0f;
      else {
        const float u = std::clamp(std::log2(f / blendLo), 0.0f, 1.0f);
        wLo = 1.0f - u * u * (3.0f - 2.0f * u);
      }
    }
    float posS = 0.0f, widthS = 0.0f, lvlS = 0.0f, posL = 0.0f, widthL = 0.0f, lvlL = 0.0f;
    if (wLo < 1.0f) {
      const Sums s = sums(hRe_, hIm_, nb, binHz, f, ribPrevHz_[b], ribNextHz_[b]);
      const double tot = s.pL + s.pR;
      lvlS = static_cast<float>(std::sqrt(tot / std::max(1.0f, enbw_))) * norm;
      posS = tot > 1e-20 ? static_cast<float>((s.pR - s.pL) / tot) : 0.0f;
      widthS = width(acc_[b], s);
    }
    if (wLo > 0.0f) {
      const Sums s = sums(loRe_, loIm_, loNb, loBinHz, f, ribPrevHz_[b], ribNextHz_[b]);
      const double tot = s.pL + s.pR;
      lvlL = static_cast<float>(std::sqrt(tot / std::max(1.0f, loEnbw_))) * loNorm;
      posL = tot > 1e-20 ? static_cast<float>((s.pR - s.pL) / tot) : 0.0f;
      widthL = width(accLo_[b], s);
    }
    const float level = std::sqrt(wLo * lvlL * lvlL + (1.0f - wLo) * lvlS * lvlS);
    const float db = level > 1e-9f ? 20.0f * std::log10(level) : -140.0f;
    float& cdb = rib_.db[b];
    cdb += (db - cdb) * (db > cdb ? aAtk : aRel);
    if (db > cfg_.floorDb) {
      const float prev = rib_.pos[b];
      rib_.pos[b] += (posS * (1.0f - wLo) + posL * wLo - rib_.pos[b]) * aRel;
      const float damp = 1.0f / (1.0f + std::fabs(rib_.pos[b] - prev) * 60.0f);
      rib_.width[b] += (widthS * (1.0f - wLo) + widthL * wLo - rib_.width[b]) * aCoh * damp;
    }
  }
}

}  // namespace pq::dsp
