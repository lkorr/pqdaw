// Table resources (wavetable mipmaps, shaper curves, sample data) and the off-thread FFT used to build them.

#include "pq/dsp/tables.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <numbers>
#include <string>
#include <utility>
#include <vector>

namespace pq::dsp {

void fftInPlace(std::span<double> re, std::span<double> im, bool inverse) noexcept {
  const std::size_t n = re.size();
  if (n < 2 || im.size() != n || !std::has_single_bit(n)) return;
  // Bit-reversal permutation.
  for (std::size_t i = 1, j = 0; i < n; ++i) {
    std::size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      std::swap(re[i], re[j]);
      std::swap(im[i], im[j]);
    }
  }
  const double sign = inverse ? 1.0 : -1.0;
  for (std::size_t len = 2; len <= n; len <<= 1) {
    const double ang = sign * 2.0 * std::numbers::pi / static_cast<double>(len);
    const std::size_t half = len / 2;
    for (std::size_t k = 0; k < half; ++k) {
      // Twiddles computed directly (not by recurrence) so the error does not accumulate.
      const double wr = std::cos(ang * static_cast<double>(k));
      const double wi = std::sin(ang * static_cast<double>(k));
      for (std::size_t i = k; i < n; i += len) {
        const std::size_t j = i + half;
        const double tr = re[j] * wr - im[j] * wi;
        const double ti = re[j] * wi + im[j] * wr;
        re[j] = re[i] - tr;
        im[j] = im[i] - ti;
        re[i] += tr;
        im[i] += ti;
      }
    }
  }
}

// ---- Wavetable --------------------------------------------------------------------------------------------------

uint32_t wavetableLevelTopHarmonic(uint32_t n, uint32_t level) noexcept {
  const uint32_t half = n / 2;
  // Even levels are exact octaves; odd ones are floor of an irrational number (never within 1e-9 of an integer).
  const double h = level % 2 == 0 ? static_cast<double>(half >> (level / 2))
                                  : std::floor(static_cast<double>(half) * std::exp2(-0.5 * level));
  const uint32_t hi = half > 0 ? half - 1 : 0;
  return std::min(static_cast<uint32_t>(h), hi);
}

Result<Wavetable> Wavetable::build(std::span<const float> cycle) {
  const std::size_t n = cycle.size();
  if (n < 16 || n > 65536 || !std::has_single_bit(n))
    return Error{ErrorCode::InvalidArgument, "wavetable size must be a power of two in [16, 65536], got " +
                                                 std::to_string(n), {}};
  Wavetable t;
  const uint32_t size = static_cast<uint32_t>(n);
  const uint32_t levels = 2 * static_cast<uint32_t>(std::countr_zero(size)) - 1;  // half-octave levels
  const uint32_t stride = size + 1;
  t.data_ = AlignedBuffer<float>(static_cast<std::size_t>(levels) * stride);
  t.view_ = {t.data_.data(), size, levels, stride, static_cast<float>(size)};

  std::vector<double> specRe(n), specIm(n, 0.0);
  for (std::size_t i = 0; i < n; ++i) specRe[i] = cycle[i];
  fftInPlace(specRe, specIm, false);

  std::vector<double> re(n), im(n);
  for (uint32_t k = 0; k < levels; ++k) {
    const std::size_t maxH = wavetableLevelTopHarmonic(size, k);  // < N/2: the Nyquist bin is never kept
    for (std::size_t b = 0; b < n; ++b) {
      const std::size_t h = b <= n / 2 ? b : n - b;  // harmonic number of bin b
      const bool keep = h <= maxH;
      re[b] = keep ? specRe[b] : 0.0;
      im[b] = keep ? specIm[b] : 0.0;
    }
    fftInPlace(re, im, true);
    float* lvl = t.data_.data() + static_cast<std::size_t>(k) * stride;
    for (std::size_t i = 0; i < n; ++i) lvl[i] = static_cast<float>(re[i] / static_cast<double>(n));
    lvl[n] = lvl[0];
  }
  return t;
}

AlignedBuffer<float> Wavetable::basicCycle(BasicShape shape, uint32_t n) {
  AlignedBuffer<float> c(n);
  for (uint32_t i = 0; i < n; ++i) {
    const double ph = static_cast<double>(i) / static_cast<double>(n);
    double v = 0;
    switch (shape) {
      case BasicShape::Sine: v = std::sin(2.0 * std::numbers::pi * ph); break;
      case BasicShape::Saw: v = 2.0 * ph - 1.0; break;
      case BasicShape::Square: v = ph < 0.5 ? 1.0 : -1.0; break;
      case BasicShape::Triangle: v = ph < 0.25 ? 4.0 * ph : ph < 0.75 ? 2.0 - 4.0 * ph : 4.0 * ph - 4.0; break;
    }
    c[i] = static_cast<float>(v);
  }
  return c;
}

std::span<const float> Wavetable::level(uint32_t k) const noexcept {
  if (k >= view_.levels) return {};
  return {view_.data + static_cast<std::size_t>(k) * view_.stride, view_.stride};
}

// ---- Shaper -----------------------------------------------------------------------------------------------------

Result<ShaperTable> ShaperTable::build(std::span<const float> points, float lo, float hi) {
  if (points.size() < 2 || points.size() > (1u << 24))
    return Error{ErrorCode::InvalidArgument, "shaper needs 2 to 2^24 points", {}};
  if (!(hi > lo) || !std::isfinite(lo) || !std::isfinite(hi))
    return Error{ErrorCode::InvalidArgument, "shaper range must be finite with hi > lo", {}};
  ShaperTable t;
  const std::size_t p = points.size();
  t.data_ = AlignedBuffer<float>(p + 1);
  for (std::size_t i = 0; i < p; ++i) t.data_[i] = points[i];
  t.data_[p] = points[p - 1];
  const float segments = static_cast<float>(p - 1);
  t.view_ = {t.data_.data(), lo, segments / (hi - lo), segments, static_cast<uint32_t>(p - 1)};
  return t;
}

// ---- Sample data ------------------------------------------------------------------------------------------------

Result<SampleData> SampleData::build(std::span<const float> samples) {
  constexpr std::size_t kMax = 2147483647u - 2u * kSampleGuard - 1u;
  if (samples.empty() || samples.size() > kMax)
    return Error{ErrorCode::InvalidArgument, "sample length must be in [1, 2^31 - 9]", {}};
  SampleData s;
  s.data_ = AlignedBuffer<float>(samples.size() + 2 * kSampleGuard);  // zero-initialised guards
  for (std::size_t i = 0; i < samples.size(); ++i) s.data_[kSampleGuard + i] = samples[i];
  s.view_ = {s.data_.data() + kSampleGuard, static_cast<int32_t>(samples.size())};
  return s;
}

}  // namespace pq::dsp
