// The Wavetable face's display model (TMM.7). See wavetable_model.h.

#include "wavetable_model.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

#include "pq/dsp/frame_table_build.h"
#include "pq/dsp/tables.h"
#include "pq/dsp/wave_warp.h"

namespace pq::views::wt {

namespace {

constexpr double kPi = std::numbers::pi;
constexpr uint32_t kCycle = dsp::kKeyframeCycle;

uint64_t fnv(uint64_t h, const void* p, size_t n) noexcept {
  const auto* b = static_cast<const unsigned char*>(p);
  for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
  return h;
}
uint64_t mix(uint64_t h, uint64_t v) noexcept { return fnv(h, &v, sizeof v); }

// The basic cycles a keyframe may be (the chooser lights the one it is), made once.
const std::array<std::vector<float>, 6>& basics() {
  static const std::array<std::vector<float>, 6> b = [] {
    std::array<std::vector<float>, 6> out;
    for (uint32_t s = 0; s < 6; ++s) {
      const AlignedBuffer<float> c = dsp::basicCycle(static_cast<dsp::BasicCycle>(s));
      out[s].assign(c.span().begin(), c.span().end());
    }
    return out;
  }();
  return b;
}

double wrapPi(double a) noexcept {
  a = std::remainder(a, 2.0 * kPi);
  return a <= -kPi ? a + 2.0 * kPi : a;
}

}  // namespace

void smoothFrame(std::span<const double> x, uint32_t top, std::span<float> out, std::vector<double>& re,
                 std::vector<double>& im) {
  const auto n = static_cast<uint32_t>(x.size());
  const auto m = static_cast<uint32_t>(out.size() - 1);
  re.assign(x.begin(), x.end());
  im.assign(n, 0.0);
  dsp::fftInPlace(re, im, false);  // X_h = Σ x_i e^(−2πihi/n); c_h = X_h / n
  const uint32_t k = std::min({top, m / 2 - 1, n / 2 - 1});
  std::vector<double>& r2 = re;  // reuse: the m-point spectrum is built in the first m bins after saving c_0 … c_k
  std::vector<std::complex<double>> c(k + 1);
  for (uint32_t h = 0; h <= k; ++h) c[h] = {re[h] / n, im[h] / n};
  r2.assign(m, 0.0);
  im.assign(m, 0.0);
  r2[0] = c[0].real();
  for (uint32_t h = 1; h <= k; ++h) {
    const double a = kPi * h / (k + 1);
    const std::complex<double> v = c[h] * (std::sin(a) / a);  // Lanczos σ: the truncated series without its ringing
    r2[h] = v.real();
    im[h] = v.imag();
    r2[m - h] = v.real();
    im[m - h] = -v.imag();
  }
  dsp::fftInPlace(r2, im, true);  // unscaled inverse: the value at phase i / m
  for (uint32_t i = 0; i < m; ++i) out[i] = static_cast<float>(r2[i]);
  out[m] = out[0];
}

void Model::ensure() const {
  if (denseDirty_) {
    rebuildDense();
    denseDirty_ = false;
    bakeDirty_ = true;
  }
  if (bakeDirty_) {
    rebake();
    bakeDirty_ = false;
  }
}

float Model::gain() const {
  ensure();
  return gain_;
}

std::span<const float> Model::frame(uint32_t f) const {
  ensure();
  if (bakedFrames_.empty()) return {};
  f = std::min(f, frames_ - 1);
  return {bakedFrames_.data() + static_cast<size_t>(f) * (kPoints + 1), kPoints + 1};
}

const Model::Spectrum* Model::spectrumOf(const std::vector<float>& cycle, bool normalise) {
  uint64_t h = fnv(1469598103934665603ull, cycle.data(), cycle.size() * sizeof(float));
  h = mix(h, normalise ? 1u : 0u);
  for (const Spectrum& s : cache_)
    if (s.hash == h) return &s;
  Spectrum s;
  s.hash = h;
  re_.assign(kCycle, 0.0);
  im_.assign(kCycle, 0.0);
  double peak = 0.0;
  for (uint32_t i = 0; i < kCycle && i < cycle.size(); ++i) {
    re_[i] = cycle[i];
    peak = std::max(peak, std::fabs(re_[i]));
  }
  if (normalise && peak > 0.0)
    for (double& v : re_) v /= peak;
  dsp::fftInPlace(re_, im_, false);
  s.c.resize(kHarmonics);
  s.mag.resize(kHarmonics);
  s.arg.resize(kHarmonics);
  for (uint32_t k = 0; k < kHarmonics; ++k) {
    s.c[k] = {re_[k] / kCycle, im_[k] / kCycle};
    s.mag[k] = std::abs(s.c[k]);
    s.arg[k] = std::arg(s.c[k]);
  }
  cache_.push_back(std::move(s));
  return &cache_.back();
}

void Model::morph(double u, uint32_t count, std::complex<double>* out) const {
  const auto& k = data_.keyframes;
  const size_t n = k.size();
  auto copy = [&](size_t i) { std::copy_n(keys_[i]->c.begin(), count, out); };
  if (n == 1 || u <= k.front().position) return copy(0);
  if (u >= k.back().position) return copy(n - 1);
  size_t a = 0;
  while (a + 2 < n && u > k[a + 1].position) ++a;
  const double pa = k[a].position, pb = k[a + 1].position;
  const double t = pb > pa ? (u - pa) / (pb - pa) : 1.0;
  const Spectrum& A = *keys_[a];
  const Spectrum& B = *keys_[a + 1];
  const bool spectral = a >= data_.gaps.size() || data_.gaps[a] == dsp::GapMode::Spectral;
  out[0] = A.c[0] + t * (B.c[0] - A.c[0]);  // DC: linear either way
  if (!spectral) {
    for (uint32_t h = 1; h < count; ++h) out[h] = A.c[h] + t * (B.c[h] - A.c[h]);
    return;
  }
  for (uint32_t h = 1; h < count; ++h) {
    const double m = A.mag[h] + t * (B.mag[h] - A.mag[h]);
    double ph;
    if (A.mag[h] == 0.0) ph = B.arg[h];
    else if (B.mag[h] == 0.0) ph = A.arg[h];
    else {
      double d = wrapPi(B.arg[h] - A.arg[h]);
      if (std::fabs(d) >= kPi - dsp::kOppositePhaseTol) d = kPi;  // exactly opposite: counter-clockwise, as the builder
      ph = A.arg[h] + t * d;
    }
    out[h] = std::polar(m, ph);
  }
}

bool Model::update(const meta::DataPtr& data, const Bake& bake) {
  bool changed = false;
  if (data != ptr_ || !keys_.size()) {
    ptr_ = data;
    ok_ = false;
    if (data) {
      auto parsed = meta::parseWavetableData(*data);
      if (parsed) {
        data_ = std::move(*parsed);
        ok_ = !data_.keyframes.empty();
      }
    }
    if (!ok_) {  // nothing to show: one silent frame
      data_ = {};
      data_.keyframes.push_back({0.0f, std::vector<float>(kCycle, 0.0f)});
    }
    dataHash_ = data ? meta::dataHash(*data) : 0;
    // Spectra: reuse the cached ones by cycle hash, drop the rest.
    std::vector<Spectrum> old;
    old.swap(cache_);
    cache_.reserve(data_.keyframes.size());
    keys_.clear();
    for (const meta::WavetableKeyframe& k : data_.keyframes) {
      uint64_t h = fnv(1469598103934665603ull, k.cycle.data(), k.cycle.size() * sizeof(float));
      h = mix(h, data_.normalise ? 1u : 0u);
      const Spectrum* found = nullptr;
      for (const Spectrum& s : cache_)
        if (s.hash == h) found = &s;
      if (found == nullptr)
        for (Spectrum& s : old)
          if (s.hash == h && !s.c.empty()) {
            cache_.push_back(std::move(s));
            found = &cache_.back();
            break;
          }
      if (found == nullptr) found = spectrumOf(k.cycle, data_.normalise);
      keys_.push_back(found);
    }
    // cache_ never reallocated above (reserved for every keyframe): the pointers hold.
    shapes_.assign(data_.keyframes.size(), -1);
    const auto& b = basics();
    for (size_t i = 0; i < data_.keyframes.size(); ++i)
      for (uint32_t s = 0; s < b.size(); ++s)
        if (data_.keyframes[i].cycle.size() == b[s].size() &&
            std::memcmp(data_.keyframes[i].cycle.data(), b[s].data(), b[s].size() * sizeof(float)) == 0) {
          shapes_[i] = static_cast<int32_t>(s);
          break;
        }
    frames_ = data_.keyframes.size() == 1 ? 1u : std::clamp<uint32_t>(data_.frames, 1u, 256u);
    denseDirty_ = true;  // TMM.16: rendered when first drawn (a face drawing the published table never needs them)
    changed = true;
  }
  if (changed || !(bake == bake_)) {
    bake_ = bake;
    bakeDirty_ = true;
    changed = true;
    for (auto& f : sound_) f.clear();  // TMM.16: computed again when asked for
    sound_.resize(frames_);
  }
  if (changed) {
    uint64_t h = mix(1469598103934665603ull, dataHash_);
    h = mix(h, frames_);
    h = fnv(h, &bake_.fold, sizeof(float));
    h = fnv(h, &bake_.warp, sizeof(float));
    h = fnv(h, &bake_.sync, sizeof(float));
    h = mix(h, bake_.warp != 0.0f ? bake_.warpMode : 0u);
    h = mix(h, data_.removeDc ? 1u : 0u);
    key_ = h | 1u;  // never 0 (0 = a layer shared by the type)
  }
  return changed;
}

void Model::rebuildDense() const {
  dense_.assign(static_cast<size_t>(frames_) * kDisplayCycle, 0.0f);
  std::vector<std::complex<double>> c(kDisplayHarmonics);
  for (uint32_t d = 0; d < frames_; ++d) {
    const double u = frames_ > 1 ? static_cast<double>(d) / (frames_ - 1) : 0.0;
    morph(u, kDisplayHarmonics, c.data());
    re_.assign(kDisplayCycle, 0.0);
    im_.assign(kDisplayCycle, 0.0);
    re_[0] = c[0].real();
    for (uint32_t h = 1; h < kDisplayHarmonics; ++h) {
      const double x = kPi * h / kDisplayHarmonics;
      const double sigma = std::sin(x) / x;  // Lanczos σ: the truncated series without its ringing
      const std::complex<double> v = c[h] * sigma;
      re_[h] = v.real();
      im_[h] = v.imag();
      re_[kDisplayCycle - h] = v.real();
      im_[kDisplayCycle - h] = -v.imag();
    }
    dsp::fftInPlace(re_, im_, true);  // unscaled inverse: x_n = c_0 + 2 Re Σ c_h e^(2πihn/N)
    float* row = dense_.data() + static_cast<size_t>(d) * kDisplayCycle;
    for (uint32_t i = 0; i < kDisplayCycle; ++i) row[i] = static_cast<float>(re_[i]);
  }
}

void Model::rebake() const {
  bakedFrames_.assign(static_cast<size_t>(frames_) * (kPoints + 1), 0.0f);
  const auto mode = static_cast<dsp::WarpMode>(std::min<uint32_t>(bake_.warpMode, dsp::kWarpModeCount - 1));
  float peak = 0.0f;
  for (uint32_t d = 0; d < frames_; ++d) {
    const float* x = dense_.data() + static_cast<size_t>(d) * kDisplayCycle;
    auto read = [&](double t) {
      t -= std::floor(t);
      const double p = t * kDisplayCycle;
      const auto i = static_cast<uint32_t>(p) % kDisplayCycle;
      const double f = p - std::floor(p);
      return x[i] + f * (x[(i + 1) % kDisplayCycle] - x[i]);
    };
    float* out = bakedFrames_.data() + static_cast<size_t>(d) * (kPoints + 1);
    double mean = 0.0;
    for (uint32_t i = 0; i < kPoints; ++i) {
      const double phi = static_cast<double>(i) / kPoints;
      const double psi = dsp::syncPhase(phi, bake_.sync);
      const dsp::WarpTaps<double> t = dsp::warpPhase(mode, psi, bake_.warp);
      double v = read(t.a);
      if (t.mixB != 0.0) v = (1.0 - t.mixB) * v + t.mixB * read(t.b);
      v = dsp::foldValue(v, bake_.fold);
      out[i] = static_cast<float>(v);
      mean += v;
    }
    if (data_.removeDc) {
      const auto m = static_cast<float>(mean / kPoints);
      for (uint32_t i = 0; i < kPoints; ++i) out[i] -= m;
    }
    out[kPoints] = out[0];
    for (uint32_t i = 0; i < kPoints; ++i) peak = std::max(peak, std::fabs(out[i]));
  }
  gain_ = 1.0f / std::max(peak, 1.0f);
}

void Model::blended(float u, std::span<float> out) const {
  ensure();
  if (out.size() < kPoints + 1 || bakedFrames_.empty()) return;
  if (frames_ == 1) {
    std::copy_n(bakedFrames_.data(), kPoints + 1, out.data());
    return;
  }
  const float x = std::clamp(std::isfinite(u) ? u : 0.0f, 0.0f, 1.0f) * static_cast<float>(frames_ - 1);
  const uint32_t k = std::min(static_cast<uint32_t>(x), frames_ - 2);
  const float w = x - static_cast<float>(k);
  const float* a = bakedFrames_.data() + static_cast<size_t>(k) * (kPoints + 1);
  const float* b = a + (kPoints + 1);
  for (uint32_t i = 0; i <= kPoints; ++i) out[i] = a[i] + w * (b[i] - a[i]);
}

float Model::valueAt(float u, float phase) const {
  ensure();
  if (bakedFrames_.empty()) return 0.0f;
  const float ph = phase - std::floor(phase);
  const float p = ph * kPoints;
  const uint32_t i = std::min(static_cast<uint32_t>(p), kPoints - 1);
  const float f = p - static_cast<float>(i);
  auto at = [&](uint32_t frame) {
    const float* r = bakedFrames_.data() + static_cast<size_t>(frame) * (kPoints + 1);
    return r[i] + f * (r[i + 1] - r[i]);
  };
  if (frames_ == 1) return at(0);
  const float x = std::clamp(std::isfinite(u) ? u : 0.0f, 0.0f, 1.0f) * static_cast<float>(frames_ - 1);
  const uint32_t k = std::min(static_cast<uint32_t>(x), frames_ - 2);
  const float w = x - static_cast<float>(k);
  return at(k) + w * (at(k + 1) - at(k));
}

std::span<const float> Model::soundFrame(uint32_t d) {
  if (sound_.size() != frames_) sound_.assign(frames_, {});
  d = std::min(d, frames_ - 1);
  std::vector<float>& out = sound_[d];
  if (!out.empty()) return out;
  // The dense frame with every harmonic (the builder's morph; the keyframes' spectra are already normalised).
  const double u = frames_ > 1 ? static_cast<double>(d) / (frames_ - 1) : 0.0;
  soundSpec_.resize(kHarmonics);
  morph(u, kHarmonics, soundSpec_.data());
  sre_.assign(kCycle, 0.0);
  sim_.assign(kCycle, 0.0);
  sre_[0] = soundSpec_[0].real();
  for (uint32_t h = 1; h < kHarmonics; ++h) {
    sre_[h] = soundSpec_[h].real();
    sim_[h] = soundSpec_[h].imag();
    sre_[kCycle - h] = soundSpec_[h].real();
    sim_[kCycle - h] = -soundSpec_[h].imag();
  }
  dsp::fftInPlace(sre_, sim_, true);  // unscaled inverse: the cycle's 2048 samples
  // Baked through the shared formulas (the table's own read: φ → sync → warp → x → fold), then removeDc.
  const auto mode = static_cast<dsp::WarpMode>(std::min<uint32_t>(bake_.warpMode, dsp::kWarpModeCount - 1));
  const double* x = sre_.data();
  auto read = [&](double t) {
    t -= std::floor(t);
    const double p = t * kCycle;
    const auto i = static_cast<uint32_t>(p) % kCycle;
    const double f = p - std::floor(p);
    return x[i] + f * (x[(i + 1) % kCycle] - x[i]);
  };
  bakeBuf_.resize(kCycle);
  double mean = 0.0;
  for (uint32_t i = 0; i < kCycle; ++i) {
    const double phi = static_cast<double>(i) / kCycle;
    const dsp::WarpTaps<double> t = dsp::warpPhase(mode, dsp::syncPhase(phi, bake_.sync), bake_.warp);
    double v = read(t.a);
    if (t.mixB != 0.0) v = (1.0 - t.mixB) * v + t.mixB * read(t.b);
    v = dsp::foldValue(v, bake_.fold);
    bakeBuf_[i] = v;
    mean += v;
  }
  if (data_.removeDc)
    for (double& v : bakeBuf_) v -= mean / kCycle;
  out.resize(kSoundPoints + 1);
  smoothFrame(bakeBuf_, kHarmonics - 1, out, sre_, sim_);
  return out;
}

void Model::sounding(float u, std::span<float> out) {
  if (out.size() < kSoundPoints + 1) return;
  if (frames_ == 1) {
    const auto f = soundFrame(0);
    std::copy(f.begin(), f.end(), out.begin());
    return;
  }
  // The reader's frame pair and weight, from float u as it computes them (frame_table.h "Reading").
  const float x = std::clamp(std::isfinite(u) ? u : 0.0f, 0.0f, 1.0f) * static_cast<float>(frames_ - 1);
  const uint32_t k = std::min(static_cast<uint32_t>(x), frames_ - 2);
  const float w = x - static_cast<float>(k);
  const auto a = soundFrame(k);
  if (w == 0.0f) {
    std::copy(a.begin(), a.end(), out.begin());
    return;
  }
  const auto b = soundFrame(k + 1);
  for (uint32_t i = 0; i <= kSoundPoints; ++i) out[i] = a[i] + w * (b[i] - a[i]);
}

float Model::soundAt(float u, float phase) {
  const float ph = phase - std::floor(phase);
  const float p = (std::isfinite(ph) ? ph : 0.0f) * kSoundPoints;
  const uint32_t i = std::min(static_cast<uint32_t>(p), kSoundPoints - 1);
  const float f = p - static_cast<float>(i);
  auto at = [&](uint32_t frame) {
    const auto r = soundFrame(frame);
    return r[i] + f * (r[i + 1] - r[i]);
  };
  if (frames_ == 1) return at(0);
  const float x = std::clamp(std::isfinite(u) ? u : 0.0f, 0.0f, 1.0f) * static_cast<float>(frames_ - 1);
  const uint32_t k = std::min(static_cast<uint32_t>(x), frames_ - 2);
  const float w = x - static_cast<float>(k);
  const float va = at(k);
  return w == 0.0f ? va : va + w * (at(k + 1) - va);
}

void Model::harmonics(uint32_t k, std::span<float> amp, std::span<float> phase) const noexcept {
  std::fill(amp.begin(), amp.end(), 0.0f);
  std::fill(phase.begin(), phase.end(), 0.0f);
  if (k >= keys_.size()) return;
  const Spectrum& s = *keys_[k];
  double top = 0.0;
  for (uint32_t h = 1; h < kHarmonics; ++h) top = std::max(top, s.mag[h]);
  if (top <= 0.0) return;
  const size_t n = std::min({amp.size(), phase.size(), static_cast<size_t>(kHarmonics - 1)});
  for (size_t i = 0; i < n; ++i) {
    const double m = s.mag[i + 1] / top;
    amp[i] = static_cast<float>(m);
    phase[i] = m > 1e-6 ? static_cast<float>(wrapPi(s.arg[i + 1] + 0.5 * kPi)) : 0.0f;
  }
}

std::vector<float> Model::sourceCycle(float u) const {
  if (data_.keyframes.size() == 1) return data_.keyframes[0].cycle;
  const double x = std::clamp(static_cast<double>(u), 0.0, 1.0) * (frames_ - 1);
  const uint32_t k = std::min(static_cast<uint32_t>(x), frames_ - 2);
  const double w = x - k;
  std::vector<double> acc(kCycle, 0.0);
  std::vector<std::complex<double>> c(kHarmonics);
  std::vector<double> re(kCycle), im(kCycle);
  for (int side = 0; side < 2; ++side) {
    const double weight = side == 0 ? 1.0 - w : w;
    if (weight == 0.0) continue;
    morph(static_cast<double>(k + side) / (frames_ - 1), kHarmonics, c.data());
    std::fill(re.begin(), re.end(), 0.0);
    std::fill(im.begin(), im.end(), 0.0);
    re[0] = c[0].real();
    for (uint32_t h = 1; h < kHarmonics; ++h) {
      re[h] = c[h].real();
      im[h] = c[h].imag();
      re[kCycle - h] = c[h].real();
      im[kCycle - h] = -c[h].imag();
    }
    dsp::fftInPlace(re, im, true);
    for (uint32_t i = 0; i < kCycle; ++i) acc[i] += weight * re[i];
  }
  std::vector<float> out(kCycle);
  for (uint32_t i = 0; i < kCycle; ++i) out[i] = static_cast<float>(acc[i]);
  return out;
}

// ---- Heard (TMM.16) -------------------------------------------------------------------------------------------------

bool Heard::update(std::shared_ptr<const dsp::FrameTable> table, uint32_t level, const Bake& live) {
  if (table == table_ && level == level_ && live == live_) return false;
  table_ = std::move(table);
  level_ = level;
  live_ = live;
  thumbs_.clear();
  sharp_.clear();
  liveCurve_.clear();
  if (!table_) {
    key_ = 0;
    return true;
  }
  frames_ = std::max<uint32_t>(table_->frames(), 1);
  level_ = std::min(level_, table_->layout().levels - 1);
  thumbs_.assign(static_cast<size_t>(frames_) * (kPoints + 1), 0.0f);
  thumbDone_.assign(frames_, 0);
  sharp_.assign(frames_, {});
  gain_ = -1.0f;  // computed when first asked for (draw mode never asks)
  uint64_t h = mix(0x51ED270B27C2D1A5ull, reinterpret_cast<uintptr_t>(table_.get()));
  h = mix(h, level_);
  h = fnv(h, &live_, sizeof live_);
  key_ = h | 1u;
  return true;
}

// The live path reads the table (the frames blended at u) and applies its controls to that read.
double Heard::readAt(double u, double phase, uint32_t level) const noexcept {
  const dsp::FrameTableView& v = table_->view();
  if (!liveActive()) return dsp::readFrameTableReference(v, phase, u, level);
  const auto mode = static_cast<dsp::WarpMode>(std::min<uint32_t>(live_.warpMode, dsp::kWarpModeCount - 1));
  const dsp::WarpTaps<double> t = dsp::warpPhase(mode, dsp::syncPhase(phase, live_.sync), live_.warp);
  double x = dsp::readFrameTableReference(v, t.a, u, level);
  if (t.mixB != 0.0) x = (1.0 - t.mixB) * x + t.mixB * dsp::readFrameTableReference(v, t.b, u, level);
  return dsp::foldValue(x, live_.fold);
}

std::span<const float> Heard::frame(uint32_t f) const {
  if (thumbs_.empty()) return {};
  f = std::min(f, frames_ - 1);
  float* row = thumbs_.data() + static_cast<size_t>(f) * (kPoints + 1);
  if (!thumbDone_[f]) {  // read when first drawn (the stacked display; draw mode shows none)
    // 512 reads at the shown level (with a bake every level is its own rendering: the main curve's level), smoothed
    // to the thumbnail's 256 points (harmonics ≤ 127). A level's harmonics near 512 fold back by their own size (a
    // saw's: < 1e-3; a baked sync or pulse width: up to ≈ 2 % of full scale, measured): faint lines, a quarter of
    // the reads a full frame needs.
    // (Live controls on top spread the read's band (a live fold): twice the reads then.)
    const uint32_t reads = liveActive() ? 4 * kPoints : 2 * kPoints;
    buf_.resize(reads);
    const double u = frames_ > 1 ? static_cast<double>(f) / (frames_ - 1) : 0.0;
    for (uint32_t i = 0; i < reads; ++i) buf_[i] = readAt(u, static_cast<double>(i) / reads, level_);
    smoothFrame(buf_, table_->layout().top[level_], std::span<float>(row, kPoints + 1), re_, im_);
    thumbDone_[f] = 1;
  }
  return {row, kPoints + 1};
}

float Heard::gain() const {
  if (gain_ < 0.0f) {
    float peak = 0.0f;
    for (uint32_t d = 0; d < frames_; ++d)
      for (const float v : frame(d)) peak = std::max(peak, std::fabs(v));
    gain_ = 1.0f / std::max(peak, 1.0f);
  }
  return gain_;
}

void Heard::blended(float u, std::span<float> out) const {
  if (out.size() < kPoints + 1 || thumbs_.empty()) return;
  if (frames_ == 1) {
    const auto f = frame(0);
    std::copy(f.begin(), f.end(), out.begin());
    return;
  }
  const float x = std::clamp(std::isfinite(u) ? u : 0.0f, 0.0f, 1.0f) * static_cast<float>(frames_ - 1);
  const uint32_t k = std::min(static_cast<uint32_t>(x), frames_ - 2);
  const float w = x - static_cast<float>(k);
  const float* a = frame(k).data();
  const float* b = frame(k + 1).data();
  for (uint32_t i = 0; i <= kPoints; ++i) out[i] = a[i] + w * (b[i] - a[i]);
}

void Heard::curveAt(double u, std::span<float> out) const {
  buf_.resize(kCycle);  // 2048 reads hold every harmonic a level keeps (≤ 1023)
  for (uint32_t i = 0; i < kCycle; ++i) buf_[i] = readAt(u, static_cast<double>(i) / kCycle, level_);
  smoothFrame(buf_, table_->layout().top[level_], out, re_, im_);
}

std::span<const float> Heard::sharp(uint32_t frame) {
  frame = std::min(frame, frames_ - 1);
  std::vector<float>& out = sharp_[frame];
  if (out.empty()) {
    out.resize(kSoundPoints + 1);
    curveAt(frames_ > 1 ? static_cast<double>(frame) / (frames_ - 1) : 0.0, out);
  }
  return out;
}

void Heard::sounding(float u, std::span<float> out) {
  if (out.size() < kSoundPoints + 1 || !table_) return;
  if (liveActive() && frames_ > 1) {  // the controls act on the blended read: per position, not per frame
    if (u != liveU_ || liveCurve_.empty()) {
      liveU_ = u;
      liveCurve_.resize(kSoundPoints + 1);
      curveAt(std::clamp(std::isfinite(u) ? static_cast<double>(u) : 0.0, 0.0, 1.0), liveCurve_);
    }
    std::copy(liveCurve_.begin(), liveCurve_.end(), out.begin());
    return;
  }
  if (frames_ == 1) {
    const auto f = sharp(0);
    std::copy(f.begin(), f.end(), out.begin());
    return;
  }
  const float x = std::clamp(std::isfinite(u) ? u : 0.0f, 0.0f, 1.0f) * static_cast<float>(frames_ - 1);
  const uint32_t k = std::min(static_cast<uint32_t>(x), frames_ - 2);
  const float w = x - static_cast<float>(k);
  const auto a = sharp(k);
  if (w == 0.0f) {
    std::copy(a.begin(), a.end(), out.begin());
    return;
  }
  const auto b = sharp(k + 1);
  for (uint32_t i = 0; i <= kSoundPoints; ++i) out[i] = a[i] + w * (b[i] - a[i]);
}

float Heard::soundAt(float u, float phase) {
  if (!table_) return 0.0f;
  if (liveActive() && frames_ > 1) {
    const double uu = std::clamp(std::isfinite(u) ? static_cast<double>(u) : 0.0, 0.0, 1.0);
    return static_cast<float>(readAt(uu, std::isfinite(phase) ? phase - std::floor(phase) : 0.0, level_));
  }
  const float ph = phase - std::floor(phase);
  const float p = (std::isfinite(ph) ? ph : 0.0f) * kSoundPoints;
  const uint32_t i = std::min(static_cast<uint32_t>(p), kSoundPoints - 1);
  const float f = p - static_cast<float>(i);
  auto at = [&](uint32_t frame) {
    const auto r = sharp(frame);
    return r[i] + f * (r[i + 1] - r[i]);
  };
  if (frames_ == 1) return at(0);
  const float x = std::clamp(std::isfinite(u) ? u : 0.0f, 0.0f, 1.0f) * static_cast<float>(frames_ - 1);
  const uint32_t k = std::min(static_cast<uint32_t>(x), frames_ - 2);
  const float w = x - static_cast<float>(k);
  const float va = at(k);
  return w == 0.0f ? va : va + w * (at(k + 1) - va);
}

}  // namespace pq::views::wt
