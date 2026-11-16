// Band Levels (pq/dsp/band_levels.h; VZ.6). Ported from PQ Meter's modules/multiband_rms.cpp (its biquad designs,
// band split, ballistics), the crossover mode as a full Linkwitz–Riley tree.

#include "pq/dsp/band_levels.h"

#include <algorithm>
#include <cmath>

namespace pq::dsp {

namespace {

constexpr float kPi = 3.14159265358979f;

struct Rbj {
  float cs, alpha;
};
Rbj rbj(float fc, float sr, float q) noexcept {
  const float w0 = 2.0f * kPi * std::clamp(fc, 1.0f, sr * 0.49f) / sr;
  return {std::cos(w0), std::sin(w0) / (2.0f * std::max(0.05f, q))};
}

float ampToDb(double meanSquare) noexcept {
  return meanSquare > 1e-14 ? static_cast<float>(10.0 * std::log10(meanSquare)) : BandLevels::kFloorDb;
}

}  // namespace

void Biquad::lowpass(float fc, float sr, float q) noexcept {
  const Rbj r = rbj(fc, sr, q);
  const float a0 = 1.0f + r.alpha;
  b0 = (1.0f - r.cs) * 0.5f / a0;
  b1 = (1.0f - r.cs) / a0;
  b2 = b0;
  a1 = -2.0f * r.cs / a0;
  a2 = (1.0f - r.alpha) / a0;
}
void Biquad::highpass(float fc, float sr, float q) noexcept {
  const Rbj r = rbj(fc, sr, q);
  const float a0 = 1.0f + r.alpha;
  b0 = (1.0f + r.cs) * 0.5f / a0;
  b1 = -(1.0f + r.cs) / a0;
  b2 = b0;
  a1 = -2.0f * r.cs / a0;
  a2 = (1.0f - r.alpha) / a0;
}
void Biquad::bandpass(float fc, float sr, float q) noexcept {
  const Rbj r = rbj(fc, sr, q);
  const float a0 = 1.0f + r.alpha;
  b0 = r.alpha / a0;
  b1 = 0.0f;
  b2 = -r.alpha / a0;
  a1 = -2.0f * r.cs / a0;
  a2 = (1.0f - r.alpha) / a0;
}
void Biquad::allpass(float fc, float sr, float q) noexcept {
  const Rbj r = rbj(fc, sr, q);
  const float a0 = 1.0f + r.alpha;
  b0 = (1.0f - r.alpha) / a0;
  b1 = -2.0f * r.cs / a0;
  b2 = 1.0f;  // (1 + α) / a0
  a1 = b1;
  a2 = b0;
}

// ---- BandSplit ----------------------------------------------------------------------------------------------------

void BandSplit::configure(const BandLevelsConfig& c, bool compensate) noexcept {
  const auto sr = static_cast<float>(c.sampleRate > 1000.0 ? c.sampleRate : 48000.0);
  n_ = std::clamp(c.bands, 3, kBands);
  indep_ = n_ == 3 && c.independent3;
  compensate_ = compensate && !indep_;
  if (indep_) {
    // The mid's width in octaves (the band-pass's −3 dB points); the low-pass and high-pass stay Butterworth (a Q from
    // the width would sag their passbands: PQ Meter's note).
    const float oct = std::clamp(c.indepOctaves, 0.1f, 6.0f);
    const float p = std::exp2(oct);
    ind_[0].lowpass(std::clamp(c.indepHz[0], 20.0f, sr * 0.48f), sr);
    ind_[1].bandpass(std::clamp(c.indepHz[1], 20.0f, sr * 0.48f), sr, std::sqrt(p) / (p - 1.0f));
    ind_[2].highpass(std::clamp(c.indepHz[2], 20.0f, sr * 0.48f), sr);
  } else {
    // The crossovers ascending and inside the usable range (an inverted pair would silence a band).
    float prev = 20.0f;
    const int nx = n_ - 1;
    for (int i = 0; i < nx; ++i) {
      const float ceiling = sr * 0.48f * std::pow(0.95f, static_cast<float>(nx - 1 - i));
      const float x = std::clamp(c.crossovers[i], prev * 1.05f, std::max(prev * 1.05f, ceiling));
      prev = x;
      lp_[i].a.lowpass(x, sr);
      lp_[i].b.lowpass(x, sr);
      hp_[i].a.highpass(x, sr);
      hp_[i].b.highpass(x, sr);
      for (int k = 0; k < i; ++k) ap_[k][i].allpass(x, sr);
    }
  }
  reset();
}

void BandSplit::reset() noexcept {
  for (int i = 0; i < kBands - 1; ++i) {
    lp_[i].reset();
    hp_[i].reset();
    for (int j = 0; j < kBands - 1; ++j) ap_[i][j].reset();
  }
  for (Biquad& b : ind_) b.reset();
}

void BandSplit::process(float x, float* out) noexcept {  // RT-safe
  if (indep_) {
    for (int b = 0; b < 3; ++b) out[b] = ind_[b].process(x);
    return;
  }
  float s = x;
  for (int k = 0; k < n_ - 1; ++k) {
    float v = lp_[k].process(s);
    if (compensate_)
      for (int j = k + 1; j < n_ - 1; ++j) v = ap_[k][j].process(v);
    out[k] = v;
    s = hp_[k].process(s);
  }
  out[n_ - 1] = s;
}

// ---- BandLevels -------------------------------------------------------------------------------------------------------

BandLevels::BandLevels(const BandLevelsConfig& c) : cfg_(c), hist_(static_cast<size_t>(kHistory) * kBands, kFloorDb) { rebuild(); }

void BandLevels::configure(const BandLevelsConfig& c) {
  std::lock_guard lock(mutex_);
  if (c == cfg_) return;
  cfg_ = c;
  rebuild();
}

BandLevelsConfig BandLevels::config() const {
  std::lock_guard lock(mutex_);
  return cfg_;
}

void BandLevels::rebuild() noexcept {
  split_.configure(cfg_);
  colSamples_ = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(cfg_.sampleRate / std::max(1.0f, cfg_.columnRate))));
  std::fill(std::begin(acc_), std::end(acc_), 0.0);
  accN_ = 0;
}

void BandLevels::addFrame(float s) noexcept {
  float v[kBands];
  split_.process(s, v);
  for (int b = 0; b < split_.bands(); ++b) acc_[b] += static_cast<double>(v[b]) * v[b];
  if (++accN_ >= colSamples_) pushColumn();
}

void BandLevels::push(const float* l, const float* r, uint32_t n) noexcept {
  std::lock_guard lock(mutex_);
  using Ch = BandLevelsConfig::Channel;
  for (uint32_t i = 0; i < n; ++i) {
    float s;
    switch (cfg_.channel) {
      case Ch::Left: s = l[i]; break;
      case Ch::Right: s = r[i]; break;
      case Ch::Side: s = 0.5f * (l[i] - r[i]); break;
      case Ch::Mid:
      case Ch::Mono:
      default: s = 0.5f * (l[i] + r[i]); break;
    }
    addFrame(s);
  }
}

void BandLevels::pushSilence(uint32_t n) noexcept {
  std::lock_guard lock(mutex_);
  for (uint32_t i = 0; i < n; ++i) addFrame(0.0f);
}

void BandLevels::pushColumn() noexcept {
  // Mean square over the column → dB, then the ballistics (VU 300 / 300 ms, Peak 5 / 650 ms, Custom).
  using B = BandLevelsConfig::Ballistics;
  const float atkMs = cfg_.ballistics == B::Vu ? 300.0f : cfg_.ballistics == B::Peak ? 5.0f : cfg_.attackMs;
  const float relMs = cfg_.ballistics == B::Vu ? 300.0f : cfg_.ballistics == B::Peak ? 650.0f : cfg_.releaseMs;
  const float dt = static_cast<float>(accN_ / cfg_.sampleRate);
  const float aAtk = 1.0f - std::exp(-dt / std::max(1e-4f, atkMs * 0.001f));
  const float aRel = 1.0f - std::exp(-dt / std::max(1e-4f, relMs * 0.001f));
  float* col = hist_.data() + static_cast<size_t>(written_ % kHistory) * kBands;
  for (int b = 0; b < kBands; ++b) {
    if (b >= split_.bands()) {  // an inactive band reads as nothing (no ghost when the count goes up again)
      level_[b] = kFloorDb;
      col[b] = kFloorDb;
      acc_[b] = 0.0;
      continue;
    }
    const float db = ampToDb(acc_[b] / std::max<uint32_t>(1, accN_));
    float& cur = level_[b];
    if (cur <= kFloorDb + 1.0f) cur = db;  // a first reading does not crawl up from the floor
    else cur += (db - cur) * (db > cur ? aAtk : aRel);
    cur = std::max(cur, kFloorDb);
    col[b] = cur;
    acc_[b] = 0.0;
  }
  ++written_;
  accN_ = 0;
}

uint64_t BandLevels::columns() const {
  std::lock_guard lock(mutex_);
  return written_;
}

uint32_t BandLevels::read(uint64_t from, uint32_t n, float* out) const {
  std::lock_guard lock(mutex_);
  const uint64_t oldest = written_ > kHistory ? written_ - kHistory : 0;
  if (from < oldest) from = oldest;
  if (from >= written_) return 0;
  const auto k = static_cast<uint32_t>(std::min<uint64_t>(n, written_ - from));
  for (uint32_t i = 0; i < k; ++i) {
    const float* c = hist_.data() + static_cast<size_t>((from + i) % kHistory) * kBands;
    std::copy(c, c + kBands, out + static_cast<size_t>(i) * kBands);
  }
  return k;
}

float BandLevels::level(int band) const {
  std::lock_guard lock(mutex_);
  return band >= 0 && band < kBands ? level_[band] : kFloorDb;
}

bool BandLevels::atRest() const {
  std::lock_guard lock(mutex_);
  for (int b = 0; b < split_.bands(); ++b)
    if (level_[b] > -120.0f) return false;
  return true;
}

}  // namespace pq::dsp
