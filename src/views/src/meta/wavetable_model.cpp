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

std::span<const float> Model::frame(uint32_t f) const noexcept {
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
      if (d == -kPi) d = kPi;  // exactly opposite: counter-clockwise, as the builder
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
    rebuildDense();
    changed = true;
  }
  if (changed || !(bake == bake_) || bakedFrames_.empty()) {
    bake_ = bake;
    rebake();
    changed = true;
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

void Model::rebuildDense() {
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

void Model::rebake() {
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

void Model::blended(float u, std::span<float> out) const noexcept {
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

float Model::valueAt(float u, float phase) const noexcept {
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

}  // namespace pq::views::wt
