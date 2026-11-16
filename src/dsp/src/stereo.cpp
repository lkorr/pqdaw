// The stereo analysis (VZ.5): pq/dsp/stereo.h. Ported from PQ Meter (modules/stereo_image.cpp, phase_scope.cpp,
// stereo_field.cpp).

#include "pq/dsp/stereo.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>

#include "pq/dsp/fft.h"

namespace pq::dsp {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr float kSilent = 1e-7f;

uint32_t pow2Floor(uint32_t n, uint32_t lo, uint32_t hi) noexcept {
  n = std::clamp(n, lo, hi);
  uint32_t p = lo;
  while (p * 2 <= n) p *= 2;
  return p;
}

double besselI0(double x) noexcept {
  double sum = 1.0, term = 1.0;
  const double x2 = x * x * 0.25;
  for (int k = 1; term > sum * 1e-16; ++k) {
    term *= x2 / (static_cast<double>(k) * k);
    sum += term;
  }
  return sum;
}

float dbOf(double power, float enbw, float norm) noexcept {
  const float level = static_cast<float>(std::sqrt(power / std::max(1.0f, enbw))) * norm;
  return level > 1e-9f ? 20.0f * std::log10(level) : -140.0f;
}

}  // namespace

// ---- CrossSpectrum ---------------------------------------------------------------------------------------------------

CrossSpectrum::CrossSpectrum(const CrossSpectrumConfig& c) {
  cfg_.sampleRate = 0.0;  // configure builds everything
  configure(c);
}

CrossSpectrum::~CrossSpectrum() = default;

void CrossSpectrum::window(uint32_t n, std::vector<float>& w, float& sum, float& enbw) const {
  w.resize(n);
  double s1 = 0.0, s2 = 0.0;
  const double i0b = besselI0(6.2);
  for (uint32_t i = 0; i < n; ++i) {
    double h;
    if (cfg_.window == SpecWindow::Kaiser) {
      const double x = 2.0 * i / n - 1.0;
      h = besselI0(6.2 * std::sqrt(std::max(0.0, 1.0 - x * x))) / i0b;
    } else {
      h = 0.5 * (1.0 - std::cos(2.0 * kPi * i / n));
    }
    w[i] = static_cast<float>(h);
    s1 += h;
    s2 += h * h;
  }
  sum = static_cast<float>(std::max(1e-9, s1));
  enbw = s1 > 0.0 ? static_cast<float>(s2 * n / (s1 * s1)) : 1.0f;
}

void CrossSpectrum::configure(const CrossSpectrumConfig& c) {
  CrossSpectrumConfig n = c;
  n.fftSize = pow2Floor(c.fftSize, 512, 16384);
  n.peakFft = pow2Floor(c.peakFft, 512, 8192);
  n.hop = std::clamp<uint32_t>(c.hop, 32, n.fftSize);
  n.points = std::clamp<uint32_t>(c.points, 16, 2048);
  n.fMax = std::min(c.fMax, static_cast<float>(c.sampleRate * 0.5));
  const bool restart = n.sampleRate != cfg_.sampleRate || n.fftSize != cfg_.fftSize || n.window != cfg_.window ||
                       n.scale != cfg_.scale || n.fMin != cfg_.fMin || n.fMax != cfg_.fMax || n.points != cfg_.points;
  const bool peakRestart = restart || n.peakFft != cfg_.peakFft;
  const uint32_t added = n.parts & ~cfg_.parts;
  cfg_ = n;
  if (restart) {
    axis_ = FreqAxis(cfg_.scale, cfg_.fMin, cfg_.fMax);
    fft_ = std::make_unique<RealFft>(cfg_.fftSize);
    window(cfg_.fftSize, win_, winSum_, enbw_);
    const uint32_t bins = cfg_.fftSize / 2 + 1;
    for (int ch = 0; ch < 2; ++ch) {
      re_[ch].assign(bins, 0.0f);
      im_[ch].assign(bins, 0.0f);
    }
    const double binHz = cfg_.sampleRate / cfg_.fftSize;
    pts_.assign(cfg_.points, StereoPoint{});
    acc_.assign(cfg_.points, Accum{});
    b0_.resize(cfg_.points);
    b1_.resize(cfg_.points);
    const float last = static_cast<float>(cfg_.points - 1);
    for (uint32_t p = 0; p < cfg_.points; ++p) {
      const float f = axis_.hz(static_cast<float>(p) / last);
      const float fPrev = axis_.hz(static_cast<float>(p == 0 ? 0 : p - 1) / last);
      const float fNext = axis_.hz(static_cast<float>(std::min(cfg_.points - 1, p + 1)) / last);
      pts_[p].hz = f;
      // Integrate a bin range, at least 2 bins wide (one bin is perfectly coherent with itself).
      double lo = 0.5 * (fPrev + f) / binHz, hi = 0.5 * (f + fNext) / binHz;
      if (hi - lo < 2.0) {
        lo = f / binHz - 1.0;
        hi = f / binHz + 1.0;
      }
      b0_[p] = static_cast<uint32_t>(std::clamp(std::floor(lo), 0.0, static_cast<double>(bins - 1)));
      b1_[p] = static_cast<uint32_t>(std::clamp(std::ceil(hi), 0.0, static_cast<double>(bins - 1)));
    }
    frames_ = quiet_ = 0;
    std::fill(heat_.begin(), heat_.end(), 0.0f);
    for (StereoParticle& p : parts_) p = StereoParticle{};
  }
  // The tracker's window is built while Particles is on and whenever its size no longer matches: a size changed while
  // Particles was off (C1: Particles → Glow → window 4096 → Particles ran 4096 over 2048-sized buffers).
  if ((cfg_.parts & kStereoParticles) && (peakRestart || !peakFft_ || peakWin_.size() != cfg_.peakFft)) {
    peakFft_ = std::make_unique<RealFft>(cfg_.peakFft);
    window(cfg_.peakFft, peakWin_, peakWinSum_, peakEnbw_);
    const uint32_t bins = cfg_.peakFft / 2 + 1;
    for (int ch = 0; ch < 2; ++ch) {
      pre_[ch].assign(bins, 0.0f);
      pim_[ch].assign(bins, 0.0f);
    }
    power_.assign(bins, 0.0f);
  }
  if ((cfg_.parts & (kStereoGlow | kStereoParticles)) && heat_.size() != static_cast<size_t>(cfg_.points) * kHeatCols)
    heat_.assign(static_cast<size_t>(cfg_.points) * kHeatCols, 0.0f);
  if ((cfg_.parts & kStereoParticles) && parts_.empty()) {
    parts_.assign(kMaxParticles, StereoParticle{});
    peaks_.reserve(128);
    matches_.reserve(static_cast<size_t>(kMaxParticles) * 128);
  }
  if (added & (kStereoGlow | kStereoParticles)) std::fill(heat_.begin(), heat_.end(), 0.0f);
  if (added & kStereoParticles)
    for (StereoParticle& p : parts_) p = StereoParticle{};
  // The history ring holds the longer window (with room: a power of two over it).
  const uint32_t need = std::max(cfg_.fftSize, (cfg_.parts & kStereoParticles) ? cfg_.peakFft : 0u);
  uint32_t cap = 1024;
  while (cap < need) cap *= 2;
  if (hist_[0].size() != cap) {
    for (auto& h : hist_) h.assign(cap, 0.0f);
    mask_ = cap - 1;
    at_ = sinceHop_ = 0;
  }
  in_.resize(std::max(cfg_.fftSize, cfg_.peakFft));
}

uint32_t CrossSpectrum::push(const float* l, const float* r, uint32_t n) noexcept {  // RT-safe
  float m = 0.0f;
  for (uint32_t i = 0; i < n; ++i) m = std::max(m, std::max(std::fabs(l[i]), std::fabs(r[i])));
  quiet_ = m < kSilent ? quiet_ + n : 0;
  uint32_t ran = 0;
  for (uint32_t i = 0; i < n;) {
    const uint32_t k = std::min(n - i, cfg_.hop - sinceHop_);
    for (uint32_t j = 0; j < k; ++j) {
      hist_[0][(at_ + j) & mask_] = l[i + j];
      hist_[1][(at_ + j) & mask_] = r[i + j];
    }
    at_ = (at_ + k) & mask_;
    sinceHop_ += k;
    i += k;
    if (sinceHop_ >= cfg_.hop) {
      sinceHop_ = 0;
      frame();
      ++ran;
    }
  }
  return ran;
}

void CrossSpectrum::frame() noexcept {
  const uint32_t N = cfg_.fftSize;
  for (int ch = 0; ch < 2; ++ch) {
    const uint32_t start = (at_ - N) & mask_;  // the newest N samples
    for (uint32_t i = 0; i < N; ++i) in_[i] = hist_[ch][(start + i) & mask_] * win_[i];
    fft_->forward(in_.data(), re_[ch].data(), im_[ch].data());
  }
  ++frames_;
  const float norm = 2.0f / winSum_;
  const float hopSec = static_cast<float>(cfg_.hop / cfg_.sampleRate);
  auto coef = [&](float ms) { return 1.0f - std::exp(-hopSec / std::max(1e-4f, ms * 0.001f)); };
  const float aAtk = coef(cfg_.attackMs), aRel = coef(cfg_.releaseMs), aCoh = coef(cfg_.coherenceMs);
  const bool glow = (cfg_.parts & kStereoGlow) != 0, after = (cfg_.parts & kStereoAfterglow) != 0;
  const float dbSpan = std::max(1.0f, cfg_.ceilDb - cfg_.floorDb);
  const float hAtk = 1.0f - std::exp(-hopSec / std::max(0.005f, cfg_.heatAttackS));
  const float hRel = 1.0f - std::exp(-hopSec / std::max(0.01f, cfg_.heatDecayS));
  for (uint32_t p = 0; p < cfg_.points; ++p) {
    double pL = 0.0, pR = 0.0, xRe = 0.0, xIm = 0.0;
    for (uint32_t b = b0_[p]; b <= b1_[p]; ++b) {
      const double lr = re_[0][b], li = im_[0][b], rr = re_[1][b], ri = im_[1][b];
      pL += lr * lr + li * li;
      pR += rr * rr + ri * ri;
      xRe += lr * rr + li * ri;  // L · conj(R)
      xIm += li * rr - lr * ri;
    }
    const double total = pL + pR;
    const float db = dbOf(total, enbw_, norm);
    const float pos = total > 1e-20 ? static_cast<float>((pR - pL) / total) : 0.0f;
    Accum& a = acc_[p];
    a.sL += (pL - a.sL) * aCoh;
    a.sR += (pR - a.sR) * aCoh;
    a.xRe += (xRe - a.xRe) * aCoh;
    a.xIm += (xIm - a.xIm) * aCoh;
    StereoPoint& s = pts_[p];
    // Below the noise floor of either channel the ratio is unstable: silence (or one silent channel) reads narrow.
    float coh = 0.0f, widthNow = 0.0f;
    if (a.sL > 1e-24 && a.sR > 1e-24) {
      coh = static_cast<float>(std::clamp(std::sqrt(a.xRe * a.xRe + a.xIm * a.xIm) / std::sqrt(a.sL * a.sR), 0.0, 1.0));
      widthNow = 1.0f - coh;
    }
    s.coherence = coh;
    const double at = a.sL + a.sR;
    s.monoDb = at > 1e-24 ? static_cast<float>(10.0 * std::log10(std::max(at + 2.0 * a.xRe, 1e-12 * at) / at)) : 0.0f;
    s.phaseDeg = (a.xRe != 0.0 || a.xIm != 0.0) ? static_cast<float>(std::atan2(a.xIm, a.xRe) * 180.0 / kPi) : 0.0f;
    s.db += (db - s.db) * (db > s.db ? aAtk : aRel);
    if (!cfg_.holdPos || db > cfg_.floorDb) {
      const float prev = s.pos;
      s.pos += (pos - s.pos) * aRel;
      const float damp = 1.0f / (1.0f + std::fabs(s.pos - prev) * 60.0f);  // moving content keeps its shape
      s.width += (widthNow - s.width) * aCoh * damp;
    }
    if (after) {
      const float intensity = std::clamp((db - cfg_.afterglowDb) / std::max(1.0f, cfg_.ceilDb - cfg_.afterglowDb), 0.0f, 1.0f);
      if (intensity > s.glowLevel) {
        s.glowPos = s.pos;
        s.glowWidth = widthNow;
        s.glowLevel = intensity;
      }
      s.glowLevel = std::max(0.0f, s.glowLevel - (cfg_.afterglowS > 0.01f ? hopSec / cfg_.afterglowS : 1.0f));
    }
    if (glow) {  // the tracked position ± width deposits its level
      const float energy = std::clamp((s.db - cfg_.floorDb) / dbSpan, 0.0f, 1.0f);
      const float centre = (s.pos * 0.5f + 0.5f) * (kHeatCols - 1);
      const float halfW = std::max(0.5f, s.width * cfg_.widthScale * kHeatCols * 0.5f);
      const int c0 = std::clamp(static_cast<int>(std::floor(centre - halfW)), 0, static_cast<int>(kHeatCols) - 1);
      const int c1 = std::clamp(static_cast<int>(std::ceil(centre + halfW)), 0, static_cast<int>(kHeatCols) - 1);
      float* row = heat_.data() + static_cast<size_t>(p) * kHeatCols;
      for (int c = 0; c < static_cast<int>(kHeatCols); ++c) {
        const float target = c >= c0 && c <= c1 ? energy : 0.0f;
        row[c] += (target - row[c]) * (target > row[c] ? hAtk : hRel);
        if (row[c] < 1e-6f) row[c] = 0.0f;
      }
    }
  }
  if (cfg_.parts & kStereoParticles) track(hopSec);
}

// PQ Meter's heat-map tracker (stereo_image.cpp frame(), drawMode 2), with its numbers.
void CrossSpectrum::track(float hopSec) noexcept {
  const uint32_t M = cfg_.peakFft, bins = M / 2 + 1;
  assert(peakWin_.size() == M && power_.size() == bins && in_.size() >= M && hist_[0].size() >= M);
  for (int ch = 0; ch < 2; ++ch) {
    const uint32_t start = (at_ - M) & mask_;
    for (uint32_t i = 0; i < M; ++i) in_[i] = hist_[ch][(start + i) & mask_] * peakWin_[i];
    peakFft_->forward(in_.data(), pre_[ch].data(), pim_[ch].data());
  }
  for (uint32_t b = 0; b < bins; ++b)
    power_[b] = pre_[0][b] * pre_[0][b] + pim_[0][b] * pim_[0][b] + pre_[1][b] * pre_[1][b] + pim_[1][b] * pim_[1][b];
  const float binHz = static_cast<float>(cfg_.sampleRate / M), norm = 2.0f / peakWinSum_;
  const float dbSpan = std::max(1.0f, cfg_.ceilDb - cfg_.floorDb);
  const float hAtk = 1.0f - std::exp(-hopSec / std::max(0.005f, cfg_.heatAttackS));
  // "Heat decay" is how long a dot takes to GO: ≈ 3.4 time constants fall in it.
  const float hRel = 1.0f - std::exp(-hopSec * std::log(0.6f / 0.02f) / std::max(0.01f, cfg_.heatDecayS));
  const float smooth = 1.0f - std::exp(-hopSec / 0.03f);
  const int bMin = std::max(1, static_cast<int>(cfg_.fMin / binHz));
  const int bMax = std::min(static_cast<int>(bins) - 2, static_cast<int>(cfg_.fMax / binHz));
  auto bandPow = [&](int k, double& sl, double& sr) {
    sl += static_cast<double>(pre_[0][k]) * pre_[0][k] + static_cast<double>(pim_[0][k]) * pim_[0][k];
    sr += static_cast<double>(pre_[1][k]) * pre_[1][k] + static_cast<double>(pim_[1][k]) * pim_[1][k];
  };
  peaks_.clear();
  for (int b = bMin; b <= bMax; ++b) {
    if (power_[b] <= power_[b - 1] || power_[b] <= power_[b + 1]) continue;
    const float db = dbOf(power_[b], peakEnbw_, norm);
    if (db < cfg_.floorDb) continue;
    {  // prominence: 9 dB over the shallower side's valley within ±12 % (≥ 4 bins)
      const int half = std::max(4, static_cast<int>(b * 0.12f));
      const int n0 = std::max(1, b - half), n1 = std::min(static_cast<int>(bins) - 2, b + half);
      float lo = power_[b], hi = power_[b];
      for (int k = b - 1; k >= n0; --k) lo = std::min(lo, power_[k]);
      for (int k = b + 1; k <= n1; ++k) hi = std::min(hi, power_[k]);
      const float bg = std::max(lo, hi);
      const float ratio = bg > 1e-30f ? power_[b] / bg : 1e30f;
      if (10.0f * std::log10(std::max(ratio, 1e-30f)) < 9.0f) continue;
    }
    const float mL = power_[b - 1] > 1e-30f ? std::log(power_[b - 1]) : -70.0f;
    const float mC = power_[b] > 1e-30f ? std::log(power_[b]) : -70.0f;
    const float mR = power_[b + 1] > 1e-30f ? std::log(power_[b + 1]) : -70.0f;
    const float den = mL - 2.0f * mC + mR;
    const float delta = den < -1e-6f ? 0.5f * (mL - mR) / den : 0.0f;
    const float freq = std::clamp(static_cast<float>(b) + delta, static_cast<float>(bMin), static_cast<float>(bMax)) * binHz;
    if (freq < cfg_.fMin || freq > cfg_.fMax) continue;
    const float thresh = power_[b] * 0.0625f;  // the mainlobe: down to −12 dB or a valley
    double sl = 0.0, sr = 0.0;
    for (int k = b; k >= 1 && k >= b - 6; --k) {
      if (k != b && (power_[k] < thresh || power_[k] > power_[k + 1])) break;
      bandPow(k, sl, sr);
    }
    for (int k = b + 1; k < static_cast<int>(bins) && k <= b + 6; ++k) {
      if (power_[k] < thresh || power_[k] > power_[k - 1]) break;
      bandPow(k, sl, sr);
    }
    const double tot = sl + sr;
    if (peaks_.size() < 128)
      peaks_.push_back({std::log2(freq), tot > 1e-20 ? static_cast<float>((sr - sl) / tot) : 0.0f,
                        std::clamp((db - cfg_.floorDb) / dbSpan, 0.0f, 1.0f)});
  }
  std::sort(peaks_.begin(), peaks_.end(), [](const Peak& a, const Peak& b) { return a.energy > b.energy; });
  {  // one partial seen twice (a sweep's smear): keep the strongest within a semitone
    size_t kept = 0;
    for (size_t i = 0; i < peaks_.size(); ++i) {
      bool dup = false;
      for (size_t j = 0; j < kept && !dup; ++j) dup = std::fabs(peaks_[i].logF - peaks_[j].logF) < 1.0f / 12.0f;
      if (!dup) peaks_[kept++] = peaks_[i];
    }
    peaks_.resize(kept);
  }
  const float logRadius = std::log2(cfg_.fMax / std::max(1.0f, cfg_.fMin)) / static_cast<float>(cfg_.points) * 3.0f;
  matches_.clear();
  for (uint32_t pi = 0; pi < kMaxParticles; ++pi) {
    const StereoParticle& part = parts_[pi];
    if (!part.alive) continue;
    for (uint32_t ki = 0; ki < peaks_.size(); ++ki) {
      const float df = std::fabs(peaks_[ki].logF - part.logF) / logRadius;
      if (df > 0.75f) continue;
      const float dp = std::fabs(peaks_[ki].pan - part.pan);
      if (dp > 0.5f) continue;  // across the stereo axis is not the same source moving
      matches_.push_back({pi, ki, df * df + dp * dp * 4.0f});
    }
  }
  std::sort(matches_.begin(), matches_.end(), [](const Match& a, const Match& b) { return a.cost < b.cost; });
  std::array<bool, kMaxParticles> partUsed{};
  std::array<bool, 128> peakUsed{};
  constexpr float kDeath = 0.02f;
  auto pushTrail = [](StereoParticle& part) {
    if (part.energy <= 0.01f) return;
    part.trail[part.trailHead] = {part.logF, part.pan, part.energy};
    part.trailHead = (part.trailHead + 1) % StereoParticle::kTrail;
    part.trailCount = std::min(part.trailCount + 1, StereoParticle::kTrail);
  };
  for (const Match& m : matches_) {
    if (partUsed[m.part] || peakUsed[m.peak]) continue;
    partUsed[m.part] = peakUsed[m.peak] = true;
    StereoParticle& part = parts_[m.part];
    pushTrail(part);
    part.logF += (peaks_[m.peak].logF - part.logF) * smooth;
    part.pan += (peaks_[m.peak].pan - part.pan) * smooth;
    const float target = peaks_[m.peak].energy;
    part.energy += (target - part.energy) * (target > part.energy ? hAtk : hRel);
    if (part.energy < kDeath) part.alive = false, part.trailCount = 0;
  }
  for (uint32_t pi = 0; pi < kMaxParticles; ++pi) {
    StereoParticle& part = parts_[pi];
    if (!part.alive || partUsed[pi]) continue;
    pushTrail(part);
    part.energy -= part.energy * hRel;
    if (part.energy < kDeath) part.alive = false, part.trailCount = 0;
  }
  const float trailDecay = 1.0f - std::exp(-hopSec / std::max(0.01f, cfg_.heatDecayS * 0.5f));
  uint32_t live = 0;
  for (StereoParticle& part : parts_) {
    if (!part.alive) continue;
    ++live;
    for (uint32_t t = 0; t < part.trailCount; ++t) part.trail[t].energy -= part.trail[t].energy * trailDecay;
  }
  constexpr uint32_t kSpawnPerFrame = 12, kLiveCeiling = kMaxParticles - 24;
  uint32_t spawned = 0;
  for (uint32_t i = 0; i < peaks_.size() && spawned < kSpawnPerFrame && live < kLiveCeiling; ++i) {
    if (peakUsed[i]) continue;
    for (StereoParticle& part : parts_) {
      if (part.alive) continue;
      part = StereoParticle{};
      part.logF = peaks_[i].logF;
      part.pan = peaks_[i].pan;
      part.energy = peaks_[i].energy * 0.3f;
      part.alive = true;
      ++spawned;
      ++live;
      break;
    }
  }
  // The diffuse underlay: every bin splatted into the grid (broadband / decorrelated content has no peaks).
  const float retain = 1.0f - hRel;
  for (float& h : heat_) h = h * retain < 1e-6f ? 0.0f : h * retain;
  const int rows = static_cast<int>(cfg_.points), cols = static_cast<int>(kHeatCols);
  for (int b = bMin; b <= bMax; ++b) {
    const float db = dbOf(power_[b], peakEnbw_, norm);
    if (db < cfg_.floorDb) continue;
    const float energy = std::clamp((db - cfg_.floorDb) / dbSpan, 0.0f, 1.0f);
    double sl = 0.0, sr = 0.0;
    bandPow(b, sl, sr);
    const double tot = sl + sr;
    const float pan = tot > 1e-20 ? static_cast<float>((sr - sl) / tot) : 0.0f;
    const float cpan = pan >= 0.0f ? std::pow(pan, 0.7f) : -std::pow(-pan, 0.7f);
    const float rowF = axis_.unit(static_cast<float>(b) * binHz) * (rows - 1);
    const float colF = (cpan * 0.5f + 0.5f) * (cols - 1);
    const int rc = static_cast<int>(rowF + 0.5f), cc = static_cast<int>(colF + 0.5f);
    for (int dr = -2; dr <= 2; ++dr) {
      const int rr = rc + dr;
      if (rr < 0 || rr >= rows) continue;
      const float dy = (static_cast<float>(rr) - rowF) / 3.0f, wy = 1.0f - dy * dy;
      if (wy <= 0.0f) continue;
      for (int dc = -3; dc <= 3; ++dc) {
        const int c = cc + dc;
        if (c < 0 || c >= cols) continue;
        const float dx = (static_cast<float>(c) - colF) / 4.0f, wc = 1.0f - dx * dx;
        if (wc <= 0.0f) continue;
        float& h = heat_[static_cast<size_t>(rr) * kHeatCols + static_cast<size_t>(c)];
        h = std::max(h, wy * wc * energy);
      }
    }
  }
}

bool CrossSpectrum::atRest() const noexcept {
  if (static_cast<double>(quiet_) < std::max<double>(cfg_.fftSize, cfg_.peakFft) + cfg_.hop) return false;
  for (const StereoPoint& p : pts_)
    if (p.db > cfg_.floorDb - 1.0f || p.glowLevel > 0.0f) return false;
  for (const float h : heat_)
    if (h > 0.0f) return false;
  for (const StereoParticle& p : parts_)
    if (p.alive) return false;
  return true;
}

// ---- StereoFieldState (PQ stereo_field) ------------------------------------------------------------------------------

namespace {
// Designed in double (1 − cos w0 = 2 sin²(w0 / 2)) and run in double: float sections lose a low crossover at high rates.
void butterworth(double fc, double sr, bool high, double& b0, double& b1, double& b2, double& a1, double& a2) noexcept {
  const double w0 = 2.0 * kPi * std::clamp(fc, 1.0, sr * 0.49) / sr;
  const double h = std::sin(0.5 * w0), omc = 2.0 * h * h, cs = std::cos(w0);
  const double alpha = std::sin(w0) / (2.0 * 0.70710678118654752), a0 = 1.0 + alpha;
  const double num = high ? 2.0 - omc : omc;  // 1 + cos w0, or 1 − cos w0
  b0 = num * 0.5 / a0;
  b1 = (high ? -num : num) / a0;
  b2 = b0;
  a1 = -2.0 * cs / a0;
  a2 = (1.0 - alpha) / a0;
}
}  // namespace

void StereoFieldState::configure(double sampleRate, float corrWindowMs, bool bands, float xoverLow, float xoverHigh) {
  const bool rateChanged = sampleRate != sr_;
  sr_ = sampleRate;
  aCorr_ = 1.0f - std::exp(-1.0f / (std::max(10.0f, corrWindowMs) * 0.001f * static_cast<float>(sampleRate)));
  aPeak_ = 1.0f - std::exp(-1.0f / (2.0f * static_cast<float>(sampleRate)));
  const bool turnedOn = bands && !bands_;
  bands_ = bands;
  if (!bands) return;
  const double sr = sampleRate;
  const double x0 = std::min<double>(xoverLow, xoverHigh * 0.95), x1 = std::max<double>(xoverHigh, x0 * 1.05);
  for (uint32_t b = 0; b < kBands; ++b) {
    Band& bs = band_[b];
    bs.useHp = b > 0;
    bs.useLp = b < kBands - 1;
    for (int c = 0; c < 2; ++c)
      for (int s = 0; s < 2; ++s) {
        Biquad& hp = bs.hp[c][s];
        Biquad& lp = bs.lp[c][s];
        butterworth(b == 1 ? x0 : x1, sr, true, hp.b0, hp.b1, hp.b2, hp.a1, hp.a2);
        butterworth(b == 0 ? x0 : x1, sr, false, lp.b0, lp.b1, lp.b2, lp.a1, lp.a2);
      }
    if (bs.ring[0].empty())
      for (auto& r : bs.ring) r.assign(kRing, 0.0f);
  }
  if (turnedOn || rateChanged) {
    for (Band& bs : band_) {
      for (auto& ch : bs.hp)
        for (Biquad& q : ch) q.z1 = q.z2 = 0.0f;
      for (auto& ch : bs.lp)
        for (Biquad& q : ch) q.z1 = q.z2 = 0.0f;
      bs.sLL = bs.sRR = bs.sLR = 0.0;
      bs.corr = bs.peak = 0.0f;
      for (auto& r : bs.ring) std::fill(r.begin(), r.end(), 0.0f);
    }
    written_ = 0;
  }
}

void StereoFieldState::reset() noexcept {
  sLL_ = sRR_ = sLR_ = 0.0;
  corr_ = peak_ = 0.0f;
  for (Band& bs : band_) {
    bs.sLL = bs.sRR = bs.sLR = 0.0;
    bs.corr = bs.peak = 0.0f;
  }
}

void StereoFieldState::corrOf(double sLL, double sRR, double sLR, float& corr) noexcept {
  const double den = std::sqrt(sLL * sRR);
  if (den > 1e-12) corr = static_cast<float>(std::clamp(sLR / den, -1.0, 1.0));
  else corr += 0.05f * (0.0f - corr);
}

void StereoFieldState::push(const float* l, const float* r, uint32_t n) noexcept {  // RT-safe
  constexpr float kRoot2Inv = 0.70710678f;
  const double a = aCorr_;
  for (uint32_t i = 0; i < n; ++i) {
    const float x = l[i], y = r[i];
    sLL_ += a * (static_cast<double>(x) * x - sLL_);
    sRR_ += a * (static_cast<double>(y) * y - sRR_);
    sLR_ += a * (static_cast<double>(x) * y - sLR_);
    const float rad = std::max(std::fabs(x + y), std::fabs(x - y)) * kRoot2Inv;
    peak_ = rad > peak_ ? rad : peak_ + aPeak_ * (rad - peak_);
  }
  corrOf(sLL_, sRR_, sLR_, corr_);
  if (sLL_ < 1e-24) sLL_ = 0.0;
  if (sRR_ < 1e-24) sRR_ = 0.0;
  if (std::fabs(sLR_) < 1e-24) sLR_ = 0.0;
  if (peak_ < 1e-9f) peak_ = 0.0f;
  if (!bands_) return;
  for (Band& bs : band_) {
    for (uint32_t i = 0; i < n; ++i) {
      float bl = l[i], br = r[i];
      if (bs.useHp) {
        bl = bs.hp[0][1].run(bs.hp[0][0].run(bl));
        br = bs.hp[1][1].run(bs.hp[1][0].run(br));
      }
      if (bs.useLp) {
        bl = bs.lp[0][1].run(bs.lp[0][0].run(bl));
        br = bs.lp[1][1].run(bs.lp[1][0].run(br));
      }
      const auto idx = static_cast<size_t>((written_ + i) & (kRing - 1));
      bs.ring[0][idx] = bl;
      bs.ring[1][idx] = br;
      bs.sLL += a * (static_cast<double>(bl) * bl - bs.sLL);
      bs.sRR += a * (static_cast<double>(br) * br - bs.sRR);
      bs.sLR += a * (static_cast<double>(bl) * br - bs.sLR);
      const float rad = std::max(std::fabs(bl + br), std::fabs(bl - br)) * kRoot2Inv;
      bs.peak = rad > bs.peak ? rad : bs.peak + aPeak_ * (rad - bs.peak);
    }
    corrOf(bs.sLL, bs.sRR, bs.sLR, bs.corr);
    if (bs.sLL < 1e-24) bs.sLL = 0.0;
    if (bs.sRR < 1e-24) bs.sRR = 0.0;
    if (std::fabs(bs.sLR) < 1e-24) bs.sLR = 0.0;
    if (bs.peak < 1e-9f) bs.peak = 0.0f;
    for (auto& ch : bs.hp)
      for (Biquad& q : ch) {
        if (std::fabs(q.z1) < 1e-20f) q.z1 = 0.0f;
        if (std::fabs(q.z2) < 1e-20f) q.z2 = 0.0f;
      }
    for (auto& ch : bs.lp)
      for (Biquad& q : ch) {
        if (std::fabs(q.z1) < 1e-20f) q.z1 = 0.0f;
        if (std::fabs(q.z2) < 1e-20f) q.z2 = 0.0f;
      }
  }
  written_ += n;
}

void StereoFieldState::bandRead(uint32_t b, uint64_t from, uint32_t n, float* l, float* r) const noexcept {
  const Band& bs = band_[b];
  const uint64_t oldest = written_ > kRing ? written_ - kRing : 0;
  for (uint32_t i = 0; i < n; ++i) {
    const uint64_t at = from + i;
    const bool held = !bs.ring[0].empty() && at >= oldest && at < written_;
    const auto idx = static_cast<size_t>(at & (kRing - 1));
    l[i] = held ? bs.ring[0][idx] : 0.0f;
    r[i] = held ? bs.ring[1][idx] : 0.0f;
  }
}

}  // namespace pq::dsp
