// Frame tables (TMM.1): layout, storage, the interpolator's response, the scalar level rule and the double
// reference reader. The AVX2 readers are in frame_table_read.cpp.

#include "pq/dsp/frame_table.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <complex>
#include <numbers>
#include <string>
#include <vector>

#include "frame_table_ops.h"

namespace pq::dsp {

namespace {

uint32_t smooth7AtLeast(uint32_t n) noexcept {
  for (;; ++n) {
    uint32_t m = n;
    for (const uint32_t p : {2u, 3u, 5u, 7u})
      while (m % p == 0) m /= p;
    if (m == 1) return n;
  }
}

constexpr uint32_t kOffsetBits = 17;
constexpr uint32_t kOffsetMask = (1u << kOffsetBits) - 1;

}  // namespace

Result<FrameTableLayout> frameTableLayout(double sampleRate) {
  if (!(sampleRate >= 8000.0 && sampleRate <= 768000.0))
    return Error{ErrorCode::InvalidArgument, "frame table sample rate must be in [8000, 768000], got " +
                                                 std::to_string(sampleRate), {}};
  FrameTableLayout l;
  l.sampleRate = sampleRate;
  l.protectedHz = sampleRate >= 44100.0 ? 20000.0 : 20000.0 * sampleRate / 44100.0;
  const double a = (sampleRate - l.protectedHz) / sampleRate;
  l.ratio = std::min(2.0, (sampleRate - l.protectedHz) / l.protectedHz);
  l.b0 = a / (kFrameTableMaxHarmonic * (1.0 + kFrameTableLevelEps));
  // H_j = ⌊A / (b_j·(1 + ε))⌋ = ⌊1023·r^−j⌋ (1e-9 absorbs the rounding of r^j at exact values).
  for (uint32_t j = 0;; ++j) {
    const double h = std::floor(kFrameTableMaxHarmonic * std::pow(l.ratio, -static_cast<double>(j)) + 1e-9);
    if (h < 1.0) break;
    l.top.push_back(static_cast<uint32_t>(h));
  }
  l.levels = static_cast<uint32_t>(l.top.size());
  uint32_t cursor = 0;
  for (uint32_t j = 0; j < l.levels; ++j) {
    if (j > 0 && l.top[j] == l.top[j - 1]) {  // same band: share the row
      l.length.push_back(l.length.back());
      l.offset.push_back(l.offset.back());
      continue;
    }
    const uint32_t len = 8 * smooth7AtLeast(l.top[j]);
    l.length.push_back(len);
    l.offset.push_back(cursor + kFrameTableGuard);
    cursor += len + 2 * kFrameTableGuard;
  }
  l.frameStride = cursor;
  // Bucket b covers the floats with bits >> 16 == base + b; its top is the next bucket's first value. Its level: the
  // first j whose range reaches that top (b_j ≥ top), so every increment in the bucket is alias-safe.
  l.bucketLevel.resize(kFrameLevelBuckets);
  for (uint32_t b = 0; b < kFrameLevelBuckets; ++b) {
    const uint32_t topBits = static_cast<uint32_t>(kFrameLevelBucketBase + static_cast<int32_t>(b) + 1) << 16;
    const double top = std::bit_cast<float>(topBits);
    uint32_t j = 0;
    while (j + 1 < l.levels && l.b0 * std::pow(l.ratio, j) < top) ++j;
    l.bucketLevel[b] = static_cast<uint8_t>(j);
  }
  if (l.frameStride > kOffsetMask || l.length.front() >= (1u << (32 - kOffsetBits)))
    return Error{ErrorCode::Overflow, "frame table layout exceeds its packing", {}};
  return l;
}

Result<FrameTable> FrameTable::allocate(const FrameTableLayout& layout, uint32_t frames, bool antiderivative) {
  if (frames < 1 || frames > kFrameTableMaxFrames)
    return Error{ErrorCode::InvalidArgument, "frame table needs 1 to 256 frames, got " + std::to_string(frames), {}};
  if (layout.levels == 0 || layout.frameStride == 0)
    return Error{ErrorCode::InvalidArgument, "frame table layout is empty", {}};
  FrameTable t;
  t.layout_ = layout;
  // + 8: a reader loads 8 floats from 2 before its index, which may reach past the last row's back guard.
  t.data_ = AlignedBuffer<float>(static_cast<std::size_t>(frames) * layout.frameStride + 8);
  t.info_ = AlignedBuffer<uint32_t>(layout.levels + 3 * kFrameLevelBuckets);
  for (uint32_t j = 0; j < layout.levels; ++j) t.info_[j] = layout.offset[j] | (layout.length[j] << kOffsetBits);
  for (uint32_t b = 0; b < kFrameLevelBuckets; ++b) t.info_[layout.levels + b] = t.info_[layout.bucketLevel[b]];
  // TMM.8: the live path's level crossfade. e_j = the top increment of the last bucket holding level j (the float
  // just above the bucket's last value), so the weight (e_j − inc)·invSpan of the richer level j − 1 reaches 0 where
  // the buckets switch to level j + 1 and 1 where they switched to j: continuous in the increment.
  std::vector<double> top(layout.levels, 0.0);
  for (uint32_t b = 0; b < kFrameLevelBuckets; ++b)
    top[layout.bucketLevel[b]] =
        std::bit_cast<float>(static_cast<uint32_t>(kFrameLevelBucketBase + static_cast<int32_t>(b) + 1) << 16);
  for (uint32_t j = 1; j < layout.levels; ++j)
    if (top[j] == 0.0) top[j] = top[j - 1];  // a level no bucket holds (not expected): no span
  const std::size_t extra = 2 * static_cast<std::size_t>(kFrameLevelBuckets) +
                            (antiderivative ? frames + static_cast<std::size_t>(frames) * layout.levels : 0);
  t.extra_ = AlignedBuffer<float>(extra);
  for (uint32_t b = 0; b < kFrameLevelBuckets; ++b) {
    const uint32_t j = layout.bucketLevel[b];
    t.info_[layout.levels + kFrameLevelBuckets + b] = t.info_[j > 0 ? j - 1 : 0];
    t.info_[layout.levels + 2 * kFrameLevelBuckets + b] = j;
    t.extra_[b] = static_cast<float>(top[j]);
    const double span = j > 0 ? top[j] - top[j - 1] : 0.0;
    t.extra_[kFrameLevelBuckets + b] = span > 0.0 ? static_cast<float>(1.0 / span) : 0.0f;
  }
  if (antiderivative) {
    t.ad_ = AlignedBuffer<float>(static_cast<std::size_t>(frames) * layout.frameStride + 8);
    t.dcOffset_ = 2 * static_cast<std::size_t>(kFrameLevelBuckets);
    t.edgeOffset_ = t.dcOffset_ + frames;
  }
  t.sine_ = AlignedBuffer<float>(4 * static_cast<std::size_t>(frames));  // TMM.12-fix1: unused until setSine
  t.view_.data = t.data_.data();
  t.view_.levelInfo = t.info_.data();
  t.view_.bucketInfo = t.info_.data() + layout.levels;
  t.view_.bucketLevel = t.layout_.bucketLevel.data();
  t.view_.frames = frames;
  t.view_.levels = layout.levels;
  t.view_.frameStride = layout.frameStride;
  t.view_.framesMinus1 = static_cast<float>(frames - 1);
  t.view_.sampleRate = static_cast<float>(layout.sampleRate);
  t.view_.bucketRich = t.info_.data() + layout.levels + kFrameLevelBuckets;
  t.view_.bucketLevelIndex = t.info_.data() + layout.levels + 2 * kFrameLevelBuckets;
  t.view_.bucketTop = t.extra_.data();
  t.view_.bucketInvSpan = t.extra_.data() + kFrameLevelBuckets;
  if (antiderivative) {
    t.view_.adData = t.ad_.data();
    t.view_.frameDc = t.extra_.data() + t.dcOffset_;
    t.view_.levelEdge = t.extra_.data() + t.edgeOffset_;
  }
  return t;
}

std::span<const float> FrameTable::row(uint32_t frame, uint32_t level) const noexcept {
  if (frame >= view_.frames || level >= view_.levels) return {};
  return {data_.data() + static_cast<std::size_t>(frame) * view_.frameStride + layout_.offset[level],
          layout_.length[level]};
}

float* FrameTable::mutableRow(uint32_t frame, uint32_t level) noexcept {
  if (frame >= view_.frames || level >= view_.levels) return nullptr;
  return data_.data() + static_cast<std::size_t>(frame) * view_.frameStride + layout_.offset[level];
}

std::span<const float> FrameTable::adRow(uint32_t frame, uint32_t level) const noexcept {
  if (!view_.adData || frame >= view_.frames || level >= view_.levels) return {};
  return {ad_.data() + static_cast<std::size_t>(frame) * view_.frameStride + layout_.offset[level],
          layout_.length[level]};
}

float* FrameTable::mutableAdRow(uint32_t frame, uint32_t level) noexcept {
  if (!view_.adData || frame >= view_.frames || level >= view_.levels) return nullptr;
  return ad_.data() + static_cast<std::size_t>(frame) * view_.frameStride + layout_.offset[level];
}

double frameInterpResponse(double f) noexcept {
  // H(f) = 2 Σ_d ∫_{−½}^{½} P_d(z) cos(2πf(z + d + ½)) dz, with ∫ z^n e^{iωz} dz = Σ_k (iω)^k / k! · M_{n+k} and
  // M_q = ∫_{−½}^{½} z^q dz (0 for odd q). 80 terms: the series converges for any |ω| up to ~2π·4.
  const double w = 2.0 * std::numbers::pi * f;
  constexpr int kTerms = 80;
  double moment[kTerms + 6];
  for (int q = 0; q < kTerms + 6; ++q) moment[q] = q % 2 ? 0.0 : 2.0 * std::pow(0.5, q + 1) / (q + 1);
  std::complex<double> integral[6];
  for (int n = 0; n < 6; ++n) {
    std::complex<double> s = 0, term = 1;  // term = (iω)^k / k!
    for (int k = 0; k < kTerms; ++k) {
      s += term * moment[n + k];
      term *= std::complex<double>(0, w) / static_cast<double>(k + 1);
    }
    integral[n] = s;
  }
  double h = 0;
  for (int d = 0; d < 3; ++d) {
    const std::complex<double> shift = std::polar(1.0, w * (d + 0.5));
    for (int n = 0; n < 6; ++n) h += 2.0 * detail::ft::kInterp[d][n] * (shift * integral[n]).real();
  }
  return h;
}

uint32_t frameTableLevel(const FrameTableView& t, float inc) noexcept {
  return t.bucketLevel[_mm_cvtsi128_si32(detail::ft::levelBucket<simd::S1>(_mm_set_ss(inc)))];
}

double readFrameTableReference(const FrameTableView& t, double phase, double position, uint32_t level) noexcept {
  if (t.frames == 0 || t.levels == 0) return 0.0;
  level = std::min(level, t.levels - 1);
  const uint32_t info = t.levelInfo[level];
  const std::size_t off = info & kOffsetMask;
  const double len = static_cast<double>(info >> kOffsetBits);
  // The frame pair and weight as the readers choose them (a discrete choice, like the level), from the float
  // position; the blend itself is in double.
  std::size_t k = 0;
  double w = 0.0;
  if (t.frames > 1) {
    __m128i ki;
    __m128 wf;
    detail::ft::framePosition<simd::S1>(t, _mm_set_ss(static_cast<float>(position)), ki, wf);
    k = static_cast<std::size_t>(_mm_cvtsi128_si32(ki));
    w = _mm_cvtss_f32(wf);
  }
  double ph = phase - std::floor(phase);
  if (!(ph >= 0.0)) ph = 0.0;  // ±inf, NaN (as the readers)
  const double pos = ph * len;
  const double fi = std::floor(pos);
  const double x = pos - fi;
  const float* a = t.data + k * t.frameStride + off + static_cast<std::ptrdiff_t>(fi);
  double y[6];
  for (int q = 0; q < 6; ++q) {
    const double va = a[q - 2];
    y[q] = w != 0.0 ? va + w * (static_cast<double>(a[q - 2 + t.frameStride]) - va) : va;
  }
  // y[0..5] = y_{−2} … y_3.
  const double e[3] = {y[2] + y[3], y[1] + y[4], y[0] + y[5]};
  const double o[3] = {y[2] - y[3], y[1] - y[4], y[0] - y[5]};
  const double z = x - 0.5;
  double out = 0;
  for (int n = 5; n >= 0; --n) {
    double c = 0;
    for (int d = 0; d < 3; ++d) c += static_cast<double>(detail::ft::kInterp[d][n]) * (n % 2 ? o[d] : e[d]);
    out = out * z + c;
  }
  return out;
}

void readFrameTableReferenceBlock(const FrameTableView& t, FrameReadState& st, const float* phase,
                                  const float* position, bool positionPerSample, uint32_t lanes, uint32_t frames,
                                  double* out) noexcept {
  using S = simd::S1;
  lanes = lanes == 8 ? 8 : 1;
  for (uint32_t n = 0; n < frames; ++n)
    for (uint32_t l = 0; l < lanes; ++l) {
      const std::size_t i = static_cast<std::size_t>(n) * lanes + l;
      const auto p = _mm_set_ss(phase[i]);
      const auto d = detail::ft::phaseDelta<S>(p, _mm_set_ss(st.prevPhase[l]));
      const float inc = _mm_cvtss_f32(S::min(d, _mm_set_ss(st.prevDelta[l])));
      st.prevDelta[l] = _mm_cvtss_f32(d);
      st.prevPhase[l] = phase[i];
      const float u = positionPerSample ? position[i] : position[lanes == 8 ? l : 0];
      out[i] = readFrameTableReference(t, phase[i], u, frameTableLevel(t, inc));
    }
}

}  // namespace pq::dsp
