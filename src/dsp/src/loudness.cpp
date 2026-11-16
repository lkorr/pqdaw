// The loudness analysis (VZ.4): pq/dsp/loudness.h. Ported from PQ Meter (dsp/loudness.h, modules/level_meter.cpp,
// modules/lufs_meter.cpp, modules/loudness_graph.cpp).

#include "pq/dsp/loudness.h"

#include <immintrin.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace pq::dsp {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr float kSilent = 1e-7f;      // |x| below this (−140 dBFS) counts as silence
constexpr float kRestLin = 1e-5f;     // −100 dB: a falling level under this is at rest
constexpr double kRestSeconds = 3.5;  // silent this long: momentary and short-term read nothing, the windows are empty
constexpr float kTpFallSeconds = 0.6f;

float toDb(double lin) noexcept { return lin > 1e-10 ? 20.0f * static_cast<float>(std::log10(lin)) : kNoLoudness; }
float peakDb(float lin) noexcept { return lin > 1e-7f ? 20.0f * std::log10(lin) : kNoLoudness; }

// ---- true peak: the truepeak atom's arithmetic (src/kernels_dynamics.cpp, DY.5) ----------------------------------------
// ITU-R BS.1770-4 Annex 2: the 48-tap 4× interpolating filter as its four 12-tap phases. The same table and the same
// sum / difference rows as the kernel (tests/dsp/test_loudness.cpp compares this meter with the atom).
constexpr float kTpPhase[4][12] = {
    {0.0017089843750f, 0.0109863281250f, -0.0196533203125f, 0.0332031250000f, -0.0594482421875f, 0.1373291015625f,
     0.9721679687500f, -0.1022949218750f, 0.0476074218750f, -0.0266113281250f, 0.0148925781250f, -0.0083007812500f},
    {-0.0291748046875f, 0.0292968750000f, -0.0517578125000f, 0.0891113281250f, -0.1665039062500f, 0.4650878906250f,
     0.7797851562500f, -0.2003173828125f, 0.1015625000000f, -0.0582275390625f, 0.0330810546875f, -0.0189208984375f},
    {-0.0189208984375f, 0.0330810546875f, -0.0582275390625f, 0.1015625000000f, -0.2003173828125f, 0.7797851562500f,
     0.4650878906250f, -0.1665039062500f, 0.0891113281250f, -0.0517578125000f, 0.0292968750000f, -0.0291748046875f},
    {-0.0083007812500f, 0.0148925781250f, -0.0266113281250f, 0.0476074218750f, -0.1022949218750f, 0.9721679687500f,
     0.1373291015625f, -0.0594482421875f, 0.0332031250000f, -0.0196533203125f, 0.0109863281250f, 0.0017089843750f},
};
struct TpRows {
  alignas(32) float v[24][8];
};
constexpr TpRows makeTpRows() {
  TpRows r{};
  for (int ph = 0; ph < 2; ++ph)
    for (int j = 0; j < 6; ++j)
      for (int l = 0; l < 8; ++l) {
        r.v[ph * 12 + j][l] = (kTpPhase[ph][j] + kTpPhase[ph][11 - j]) * 0.5f;
        r.v[ph * 12 + 6 + j][l] = (kTpPhase[ph][j] - kTpPhase[ph][11 - j]) * 0.5f;
      }
  return r;
}
alignas(32) constexpr TpRows kTpRows = makeTpRows();

// 8 consecutive frames' estimates; p is the first frame's newest input (reads p − 11 … p + 7).
inline __m256 truePeakVec(const float* p) noexcept {  // RT-safe
  const __m256 absMask = _mm256_castsi256_ps(_mm256_set1_epi32(0x7FFFFFFF));
  auto row = [](int r) noexcept { return _mm256_load_ps(kTpRows.v[r]); };
  __m256 a = _mm256_loadu_ps(p), b = _mm256_loadu_ps(p - 11);
  __m256 s = _mm256_add_ps(a, b), d = _mm256_sub_ps(a, b);
  __m256 p0 = _mm256_mul_ps(s, row(0)), q0 = _mm256_mul_ps(d, row(6));
  __m256 p1 = _mm256_mul_ps(s, row(12)), q1 = _mm256_mul_ps(d, row(18));
  for (int j = 1; j < 6; ++j) {
    a = _mm256_loadu_ps(p - j);
    b = _mm256_loadu_ps(p - (11 - j));
    s = _mm256_add_ps(a, b);
    d = _mm256_sub_ps(a, b);
    p0 = _mm256_fmadd_ps(s, row(j), p0);
    q0 = _mm256_fmadd_ps(d, row(6 + j), q0);
    p1 = _mm256_fmadd_ps(s, row(12 + j), p1);
    q1 = _mm256_fmadd_ps(d, row(18 + j), q1);
  }
  const __m256 m0 = _mm256_add_ps(_mm256_and_ps(p0, absMask), _mm256_and_ps(q0, absMask));
  const __m256 m1 = _mm256_add_ps(_mm256_and_ps(p1, absMask), _mm256_and_ps(q1, absMask));
  return _mm256_max_ps(m0, _mm256_max_ps(m1, _mm256_and_ps(b, absMask)));
}

}  // namespace

double loudnessOf(double meanSquare) noexcept {
  if (meanSquare <= 1e-15) return kNoLoudness;
  return -0.691 + 10.0 * std::log10(meanSquare);
}

void kWeighting(double fs, double shelf[5], double hp[5]) noexcept {
  {
    const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
    const double K = std::tan(kPi * f0 / fs);
    const double Vh = std::pow(10.0, G / 20.0);
    const double Vb = std::pow(Vh, 0.4996667741545416);
    const double a0 = 1.0 + K / Q + K * K;
    shelf[0] = (Vh + Vb * K / Q + K * K) / a0;
    shelf[1] = 2.0 * (K * K - Vh) / a0;
    shelf[2] = (Vh - Vb * K / Q + K * K) / a0;
    shelf[3] = 2.0 * (K * K - 1.0) / a0;
    shelf[4] = (1.0 - K / Q + K * K) / a0;
  }
  {
    const double f0 = 38.13547087602444, Q = 0.5003270373238773;
    const double K = std::tan(kPi * f0 / fs);
    const double a0 = 1.0 + K / Q + K * K;
    hp[0] = 1.0;
    hp[1] = -2.0;
    hp[2] = 1.0;
    hp[3] = 2.0 * (K * K - 1.0) / a0;
    hp[4] = (1.0 - K / Q + K * K) / a0;
  }
}

float truePeakMax(const float* x, uint32_t n) noexcept {  // RT-safe
  __m256 acc = _mm256_setzero_ps();
  uint32_t i = 0;
  for (; i + 8 <= n; i += 8) acc = _mm256_max_ps(truePeakVec(x + i), acc);  // (a NaN estimate is skipped)
  alignas(32) float t[8];
  _mm256_store_ps(t, acc);
  float m = 0.0f;
  for (const float v : t) m = std::max(m, v);
  if (i < n) {  // the tail: the frames past n read the padding; only the first n − i lanes count
    _mm256_store_ps(t, truePeakVec(x + i));
    for (uint32_t k = 0; k < n - i; ++k) m = t[k] > m ? t[k] : m;
  }
  return m;
}

// ---- LoudnessHistogram (PQ Meter dsp/loudness.h) -------------------------------------------------------------------

void LoudnessHistogram::add(double meanSquare) noexcept {
  const double lufs = loudnessOf(meanSquare);
  if (lufs <= kMinLufs) return;
  const int i = std::min(kBins - 1, static_cast<int>((lufs - kMinLufs) / kBinLu));
  bins_[static_cast<size_t>(i)].sum += meanSquare;
  ++bins_[static_cast<size_t>(i)].n;
  sum_ += meanSquare;
  ++n_;
}

void LoudnessHistogram::clear() noexcept {
  std::fill(bins_.begin(), bins_.end(), Bin{});
  sum_ = 0.0;
  n_ = 0;
}

int LoudnessHistogram::gateStartBin(double offsetLu) const noexcept {
  const double gate = loudnessOf(sum_ / static_cast<double>(n_)) - offsetLu;
  return std::clamp(static_cast<int>(std::ceil((gate - kMinLufs) / kBinLu)), 0, kBins);
}

float LoudnessHistogram::gatedLoudness(double offsetLu) const noexcept {
  if (n_ == 0) return kNoLoudness;
  double sum = 0.0;
  uint64_t n = 0;
  for (int i = gateStartBin(offsetLu); i < kBins; ++i) {
    sum += bins_[static_cast<size_t>(i)].sum;
    n += bins_[static_cast<size_t>(i)].n;
  }
  return n > 0 ? static_cast<float>(loudnessOf(sum / static_cast<double>(n))) : kNoLoudness;
}

double LoudnessHistogram::percentile(double q, int start, uint64_t n) const noexcept {
  const double rank = q * static_cast<double>(n - 1);
  uint64_t seen = 0;
  for (int i = start; i < kBins; ++i) {
    seen += bins_[static_cast<size_t>(i)].n;
    if (bins_[static_cast<size_t>(i)].n > 0 && static_cast<double>(seen - 1) >= rank)
      return kMinLufs + (static_cast<double>(i) + 0.5) * kBinLu;
  }
  return kMaxLufs;
}

float LoudnessHistogram::loudnessRange() const noexcept {
  if (n_ == 0) return 0.0f;
  const int start = gateStartBin(20.0);
  uint64_t n = 0;
  for (int i = start; i < kBins; ++i) n += bins_[static_cast<size_t>(i)].n;
  if (n < 2) return 0.0f;
  return static_cast<float>(percentile(0.95, start, n) - percentile(0.10, start, n));
}

// ---- LoudnessMeter -------------------------------------------------------------------------------------------------

LoudnessMeter::LoudnessMeter(const LoudnessConfig& c) {
  cfg_.sampleRate = 0.0;  // configure starts everything
  configure(c);
}

void LoudnessMeter::configure(const LoudnessConfig& c) {
  const bool restart = c.sampleRate != cfg_.sampleRate;
  const uint32_t added = restart ? c.parts : c.parts & ~cfg_.parts;
  cfg_ = c;
  const double sr = std::max(1000.0, c.sampleRate);
  aRms_ = 1.0f - std::exp(-1.0f / (std::max(10.0f, c.rmsWindowMs) * 0.001f * static_cast<float>(sr)));
  fall_ = std::pow(10.0f, -c.peakFallDbS / 20.0f / static_cast<float>(sr));
  clipLin_ = std::pow(10.0f, c.clipThreshDb / 20.0f);
  if (restart) {
    kWeighting(sr, shelf_, hp_);
    blockLen_ = std::max<uint32_t>(1, static_cast<uint32_t>(sr / 10.0));
    corrLen_ = std::max<uint32_t>(1, static_cast<uint32_t>(sr * 0.3));
    quietFrames_ = 0;
    points_ = histStart_ = 0;
  }
  if (added & kLoudLevels) {
    for (int ch = 0; ch < 2; ++ch) {
      rmsSq_[ch] = 0.0;
      peak_[ch] = hold_[ch] = 0.0f;
      holdAge_[ch] = clipAge_[ch] = 0.0;
      clip_[ch] = wasOver_[ch] = false;
      clipCount_[ch] = 0;
    }
  }
  if (added & kLoudLoudness) {
    if (!gated_) gated_ = std::make_unique<LoudnessHistogram>();
    if (!lra_) lra_ = std::make_unique<LoudnessHistogram>();
    std::memset(zs_, 0, sizeof zs_);
    std::memset(zh_, 0, sizeof zh_);
    blockPos_ = 0;
    blockSum_ = 0.0;
    sub_.fill(0.0);
    subCount_ = subHead_ = 0;
    corrLR_ = corrLL_ = corrRR_ = 0.0;
    corrCount_ = 0;
    correlation_ = 0.0f;
    rmsMonoDb_ = kNoLoudness;
    resetMeasurement();
  }
  if (added & kLoudTruePeak) {
    for (auto& b : tpBuf_) b.assign(11 + kTpChunk + 8, 0.0f);
    truePeak_ = truePeakNow_ = 0.0f;
  }
  if ((added & kLoudHistory) && hist_.empty()) hist_.assign(kHistory, LoudnessPoint{});
}

void LoudnessMeter::push(const float* l, const float* r, uint32_t n) noexcept {  // RT-safe
  if (n == 0) return;
  float m = 0.0f;
  for (uint32_t i = 0; i < n; ++i) m = std::max(m, std::max(std::fabs(l[i]), std::fabs(r[i])));
  quietFrames_ = m < kSilent ? quietFrames_ + n : 0;
  if (cfg_.parts & kLoudLevels) pushLevels(l, r, n);
  if (cfg_.parts & kLoudLoudness) {
    samplePeak_ = std::max(samplePeak_, m);
    pushLoudness(l, r, n);
  }
  if (cfg_.parts & kLoudTruePeak) {
    const float decay = std::exp(-static_cast<float>(n) / (kTpFallSeconds * static_cast<float>(cfg_.sampleRate)));
    truePeakNow_ *= decay;
    if (truePeakNow_ < 1e-9f) truePeakNow_ = 0.0f;
    pushTruePeak(l, n, 0);
    if (r != l) pushTruePeak(r, n, 1);
  }
}

void LoudnessMeter::pushLevels(const float* l, const float* r, uint32_t n) noexcept {
  const double dt = static_cast<double>(n) / cfg_.sampleRate;
  const float* x[2] = {l, r};
  for (int c = 0; c < 2; ++c) {
    const float* p = x[c];
    double rms = rmsSq_[c];
    float peak = peak_[c], hold = hold_[c];
    bool over = wasOver_[c];
    const double a = aRms_;
    for (uint32_t i = 0; i < n; ++i) {
      const float v = p[i], av = std::fabs(v);
      rms += a * (static_cast<double>(v) * v - rms);
      peak = av > peak ? av : peak * fall_;
      if (av >= hold) {
        hold = av;
        holdAge_[c] = 0.0;
      }
      // The clip latches on the EVENT edge: a run of over-range samples is one clip.
      const bool o = av >= clipLin_;
      if (o) {
        if (!over) ++clipCount_[c];
        clip_[c] = true;
        clipAge_[c] = 0.0;
      }
      over = o;
    }
    // Far below the floor: true zero (multiplying them down forever parks them in denormal range).
    if (peak < 1e-9f) peak = 0.0f;
    if (hold > 0.0f && hold < 1e-9f) hold = 0.0f;
    if (rms < 1e-20) rms = 0.0;
    holdAge_[c] += dt;
    clipAge_[c] += dt;
    // An expired hold falls at the peak fall (the tick slides down to meet the peak).
    if (!cfg_.holdInfinite && holdAge_[c] > static_cast<double>(cfg_.holdSeconds)) hold *= std::pow(fall_, static_cast<float>(n));
    if (cfg_.clipResetS > 0.0f && clip_[c] && clipAge_[c] > static_cast<double>(cfg_.clipResetS)) {
      clip_[c] = false;
      clipCount_[c] = 0;
    }
    rmsSq_[c] = rms;
    peak_[c] = peak;
    hold_[c] = hold;
    wasOver_[c] = over;
  }
}

void LoudnessMeter::pushLoudness(const float* l, const float* r, uint32_t n) noexcept {
  // The K-weighting of L and R in one pair of lanes (transposed direct form II, double).
  const __m128d sb0 = _mm_set1_pd(shelf_[0]), sb1 = _mm_set1_pd(shelf_[1]), sb2 = _mm_set1_pd(shelf_[2]);
  const __m128d sa1 = _mm_set1_pd(shelf_[3]), sa2 = _mm_set1_pd(shelf_[4]);
  const __m128d ha1 = _mm_set1_pd(hp_[3]), ha2 = _mm_set1_pd(hp_[4]), mtwo = _mm_set1_pd(-2.0);
  __m128d s1 = _mm_set_pd(zs_[1][0], zs_[0][0]), s2 = _mm_set_pd(zs_[1][1], zs_[0][1]);
  __m128d h1 = _mm_set_pd(zh_[1][0], zh_[0][0]), h2 = _mm_set_pd(zh_[1][1], zh_[0][1]);
  uint32_t i = 0;
  while (i < n) {
    // Up to the next sub-block or correlation boundary.
    const uint32_t k = std::min({n - i, blockLen_ - blockPos_, corrLen_ - corrCount_});
    __m128d acc = _mm_setzero_pd();
    double lr = 0.0, ll = 0.0, rr = 0.0;
    for (uint32_t j = i; j < i + k; ++j) {
      const double dl = l[j], dr = r[j];
      const __m128d x = _mm_set_pd(dr, dl);
      const __m128d y = _mm_add_pd(_mm_mul_pd(sb0, x), s1);
      s1 = _mm_add_pd(_mm_sub_pd(_mm_mul_pd(sb1, x), _mm_mul_pd(sa1, y)), s2);
      s2 = _mm_sub_pd(_mm_mul_pd(sb2, x), _mm_mul_pd(sa2, y));
      // The high-pass: b = 1, −2, 1.
      const __m128d w = _mm_add_pd(y, h1);
      h1 = _mm_add_pd(_mm_sub_pd(_mm_mul_pd(mtwo, y), _mm_mul_pd(ha1, w)), h2);
      h2 = _mm_sub_pd(y, _mm_mul_pd(ha2, w));
      acc = _mm_add_pd(acc, _mm_mul_pd(w, w));
      lr += dl * dr;
      ll += dl * dl;
      rr += dr * dr;
    }
    alignas(16) double a[2];
    _mm_store_pd(a, acc);
    blockSum_ += a[0] + a[1];
    corrLR_ += lr;
    corrLL_ += ll;
    corrRR_ += rr;
    blockPos_ += k;
    corrCount_ += k;
    i += k;
    if (corrCount_ >= corrLen_) {  // unweighted RMS and correlation over 300 ms
      const double den = std::sqrt(corrLL_ * corrRR_);
      correlation_ = den > 1e-12 ? static_cast<float>(corrLR_ / den) : 0.0f;
      const double ms = 0.5 * (corrLL_ + corrRR_) / static_cast<double>(corrCount_);
      rmsMonoDb_ = ms > 1e-15 ? static_cast<float>(10.0 * std::log10(ms)) : kNoLoudness;
      corrLR_ = corrLL_ = corrRR_ = 0.0;
      corrCount_ = 0;
    }
    if (blockPos_ >= blockLen_) subBlock();
  }
  // Flush the filter state's denormal tail (a long silence).
  alignas(16) double t[2];
  auto store = [&](__m128d v, double (&z)[2][2], int k) {
    _mm_store_pd(t, v);
    z[0][k] = std::fabs(t[0]) < 1e-30 ? 0.0 : t[0];
    z[1][k] = std::fabs(t[1]) < 1e-30 ? 0.0 : t[1];
  };
  store(s1, zs_, 0);
  store(s2, zs_, 1);
  store(h1, zh_, 0);
  store(h2, zh_, 1);
}

double LoudnessMeter::meanOfLast(uint32_t n) const noexcept {
  if (subCount_ < n) return -1.0;
  double s = 0.0;
  for (uint32_t k = 1; k <= n; ++k) s += sub_[(subHead_ + 30 - k) % 30];
  return s / static_cast<double>(n);
}

void LoudnessMeter::subBlock() noexcept {
  sub_[subHead_] = blockSum_ / static_cast<double>(blockLen_);
  subHead_ = (subHead_ + 1) % 30;
  subCount_ = std::min<uint32_t>(30, subCount_ + 1);
  blockSum_ = 0.0;
  blockPos_ = 0;
  const double m4 = meanOfLast(4), m30 = meanOfLast(30);
  const float mom = m4 >= 0.0 ? static_cast<float>(loudnessOf(m4)) : kNoLoudness;
  // Auto-reset (PQ lufs_meter): the momentary held under target − belowLu for autoResetSec clears the integrated
  // measurement, judged before this block joins it (pre-roll's −120 counts as quiet).
  if (cfg_.autoReset) {
    if (mom <= cfg_.targetLufs - cfg_.autoResetBelowLu) {
      quietFor_ += 0.1f;
      if (armed_ && quietFor_ >= cfg_.autoResetSec) {
        gated_->clear();
        armed_ = false;
        quietFor_ = 0.0f;
      }
    } else {
      quietFor_ = 0.0f;
      armed_ = !gated_->empty();
    }
  }
  if (m4 >= 0.0) gated_->add(m4);  // a 400 ms block every 100 ms (the histogram applies the −70 LUFS gate)
  ++stCounter_;
  if (m30 >= 0.0 && stCounter_ % 10 == 0) {  // LRA: the 3 s blocks on a 1 s grid
    lra_->add(m30);
    lraLu_ = lra_->count() >= 3 ? lra_->loudnessRange() : 0.0f;
  }
  if (mom > kNoLoudness) maxM_ = std::max(maxM_, mom);
  float st = kNoLoudness;
  if (m30 >= 0.0) {
    st = static_cast<float>(loudnessOf(m30));
    if (st > kNoLoudness) {
      maxS_ = std::max(maxS_, st);
      minS_ = minS_ <= kNoLoudness ? st : std::min(minS_, st);
    }
  }
  integrated_ = gated_->gatedLoudness(10.0);
  if (cfg_.parts & kLoudHistory) {
    hist_[static_cast<size_t>(points_ % kHistory)] = {mom, st, integrated_};
    ++points_;
  }
}

void LoudnessMeter::pushTruePeak(const float* x, uint32_t n, uint32_t c) noexcept {
  float* b = tpBuf_[c].data();
  float m = 0.0f;
  for (uint32_t i = 0; i < n;) {
    const uint32_t k = std::min(kTpChunk, n - i);
    std::memcpy(b + 11, x + i, k * sizeof(float));
    m = std::max(m, truePeakMax(b + 11, k));
    std::memmove(b, b + k, 11 * sizeof(float));  // the newest 11 lead the next chunk
    i += k;
  }
  truePeak_ = std::max(truePeak_, m);
  truePeakNow_ = std::max(truePeakNow_, m);
}

LoudnessReadings LoudnessMeter::readings() const noexcept {
  LoudnessReadings r;
  if (cfg_.parts & kLoudLevels)
    for (int c = 0; c < 2; ++c) {
      r.rmsDb[c] = toDb(std::sqrt(std::max(0.0, rmsSq_[c])));
      r.peakDb[c] = toDb(peak_[c]);
      r.holdDb[c] = toDb(hold_[c]);
      r.clip[c] = clip_[c];
      r.clipCount[c] = clipCount_[c];
    }
  if (cfg_.parts & kLoudLoudness) {
    const double m4 = meanOfLast(4), m30 = meanOfLast(30);
    if (m4 >= 0.0) r.momentary = static_cast<float>(loudnessOf(m4));
    if (m30 >= 0.0) r.shortTerm = static_cast<float>(loudnessOf(m30));
    r.integrated = integrated_;
    r.lra = lraLu_;
    r.samplePeakDb = peakDb(samplePeak_);
    r.truePeakDb = r.samplePeakDb;
    r.correlation = correlation_;
    r.rmsDbMono = rmsMonoDb_;
    r.maxMomentary = maxM_;
    r.maxShortTerm = maxS_;
    r.minShortTerm = minS_;
  }
  if (cfg_.parts & kLoudTruePeak) {
    r.truePeakDb = peakDb(truePeak_);
    r.truePeakNowDb = peakDb(truePeakNow_);
  }
  r.points = points_;
  r.historyStart = histStart_;
  return r;
}

uint32_t LoudnessMeter::history(uint64_t from, uint32_t n, LoudnessPoint* out) const noexcept {
  if (hist_.empty()) return 0;
  const uint64_t oldest = points_ > kHistory ? points_ - kHistory : 0;
  if (from < oldest || from >= points_) return 0;
  const auto k = static_cast<uint32_t>(std::min<uint64_t>(n, points_ - from));
  for (uint32_t i = 0; i < k; ++i) out[i] = hist_[static_cast<size_t>((from + i) % kHistory)];
  return k;
}

void LoudnessMeter::resetMeasurement() noexcept {  // RT-safe
  if (gated_) gated_->clear();
  if (lra_) lra_->clear();
  integrated_ = kNoLoudness;
  lraLu_ = 0.0f;
  samplePeak_ = 0.0f;
  truePeak_ = 0.0f;
  maxM_ = maxS_ = minS_ = kNoLoudness;
  stCounter_ = 0;
  quietFor_ = 0.0f;
  armed_ = false;
  histStart_ = points_;  // the history starts again
}

void LoudnessMeter::resetClips() noexcept {  // RT-safe
  for (int c = 0; c < 2; ++c) {
    clip_[c] = false;
    clipCount_[c] = 0;
    hold_[c] = 0.0f;
    holdAge_[c] = 0.0;
  }
}

bool LoudnessMeter::atRest() const noexcept {
  if (static_cast<double>(quietFrames_) < kRestSeconds * cfg_.sampleRate) return false;
  if (cfg_.parts & kLoudLevels)
    for (int c = 0; c < 2; ++c) {
      if (peak_[c] >= kRestLin || rmsSq_[c] >= static_cast<double>(kRestLin) * kRestLin) return false;
      if (!cfg_.holdInfinite && hold_[c] >= kRestLin) return false;
      if (cfg_.clipResetS > 0.0f && clip_[c]) return false;  // its auto reset is still to come
    }
  if ((cfg_.parts & kLoudLoudness) && cfg_.autoReset && armed_) return false;  // the quiet reset is still to come
  if ((cfg_.parts & kLoudTruePeak) && truePeakNow_ >= kRestLin) return false;
  return true;
}

}  // namespace pq::dsp
