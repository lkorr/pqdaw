// Polyphase windowed-sinc resampling (T5.4): pq/dsp/resample.h.
#include "pq/dsp/resample.h"

#include <immintrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <numbers>
#include <vector>

#include "pq/core/aligned_buffer.h"

namespace pq::dsp {

namespace {

constexpr uint32_t kQualities = 2;

struct Built {
  SincTable table;
  AlignedBuffer<float> rows;
};

std::mutex gMutex;
// Constant-initialised (atomics of null), so the RT readers never run a magic-static guard.
std::array<std::atomic<const SincTable*>, 257> gTables[kQualities]{};

// Owners (never freed before exit).
std::vector<std::unique_ptr<Built>>& owners() {
  static std::vector<std::unique_ptr<Built>> v;
  return v;
}

double besselI0(double x) {
  double sum = 1.0, term = 1.0;
  const double q = x * x / 4.0;
  for (int k = 1; k < 200; ++k) {
    term *= q / (static_cast<double>(k) * k);
    sum += term;
    if (term < sum * 1e-17) break;
  }
  return sum;
}

std::vector<float> buildPrototype(const SincPrototype& pr) {
  const uint32_t n = 512 * pr.halfWidth;
  std::vector<float> f(n + 1);
  const double w = pr.halfWidth;
  const double i0b = besselI0(pr.beta);
  for (uint32_t u = 0; u <= n; ++u) {
    const double d = w - static_cast<double>(u) / 256.0;
    const double x = d / w;
    const double win = std::abs(x) >= 1.0 ? 0.0 : besselI0(pr.beta * std::sqrt(1.0 - x * x)) / i0b;
    const double a = 2.0 * pr.cutoff * d;
    const double sinc = a == 0.0 ? 1.0 : std::sin(std::numbers::pi * a) / (std::numbers::pi * a);
    f[u] = static_cast<float>(2.0 * pr.cutoff * sinc * win);
  }
  return f;
}

uint32_t roundUp(uint32_t v, uint32_t m) { return (v + m - 1) / m * m; }

const SincTable& build(ResampleQuality quality, uint32_t q) {  // gMutex held
  const auto qi = static_cast<uint32_t>(quality);
  if (const SincTable* t = gTables[qi][q].load(std::memory_order_acquire)) return *t;
  static std::array<std::vector<float>, kQualities> protos;
  const SincPrototype pr = sincPrototype(quality);
  if (protos[qi].empty()) protos[qi] = buildPrototype(pr);
  const std::vector<float>& f = protos[qi];
  const uint32_t span = 256 * pr.halfWidth;
  const uint32_t n = 2 * span;  // F has n + 1 entries; F[n] (d = −W) is treated as 0
  auto b = std::make_unique<Built>();
  SincTable& t = b->table;
  t.q = q;
  t.span = span;
  t.center = span / q;
  t.taps = roundUp((n + q - 1) / q, 8);
  const uint32_t K = t.taps;
  b->rows = AlignedBuffer<float>(static_cast<size_t>(q + 1) * 2 * K);
  float* rows = b->rows.data();
  std::vector<double> c(K);
  for (uint32_t phi = 0; phi <= q; ++phi) {
    double sum = 0.0;
    for (uint32_t k = 0; k < K; ++k) {
      const uint64_t u = phi + static_cast<uint64_t>(k) * q;
      c[k] = u < n ? f[u] : 0.0;
      sum += c[k];
    }
    float* row = rows + static_cast<size_t>(phi) * 2 * K;
    for (uint32_t k = 0; k < K; ++k) row[k] = static_cast<float>(c[k] / sum);
  }
  for (uint32_t phi = 0; phi <= q; ++phi) {  // deltas to the next row (the last row's are zero: t < 1 there)
    float* row = rows + static_cast<size_t>(phi) * 2 * K;
    const float* next = phi < q ? row + 2 * K : row;
    for (uint32_t k = 0; k < K; ++k) row[K + k] = next[k] - row[k];
  }
  t.rows = rows;
  const SincTable* out = &b->table;
  owners().push_back(std::move(b));
  gTables[qi][q].store(out, std::memory_order_release);
  return *out;
}

inline __m256 load8(const float* p) noexcept { return _mm256_load_ps(p); }

// The table row and interpolation fraction of position P (32.32), and the first frame read.
struct Phase {
  const float* row;
  float t;
  int64_t first;
};

inline Phase phaseOf(const SincTable& tb, int64_t vBase, int64_t qq, int64_t P) noexcept {  // RT-safe
  const auto fr = static_cast<uint64_t>(static_cast<uint32_t>(P));
  int64_t v = vBase - static_cast<int64_t>(fr * tb.q);
  const int64_t neg = v >> 63;  // −1 when v < 0: m = M0 − 1
  v += neg & qq;
  Phase ph;
  ph.first = (P >> 32) - static_cast<int64_t>(tb.center) - neg;
  ph.row = tb.rows + static_cast<size_t>(v >> 32) * 2 * tb.taps;
  ph.t = static_cast<float>(static_cast<uint32_t>(v) >> 8) * (1.0f / 16777216.0f);
  return ph;
}

template <uint32_t CH>
void resampleImpl(const SincTable& tb, const float* const* in, int64_t pos, int64_t step, uint32_t frames,
                  const float* env, float gain, float* const* out) noexcept {  // RT-safe
  const uint32_t K = tb.taps;
  const int64_t qq = static_cast<int64_t>(tb.q) << 32;
  const int64_t vBase = (static_cast<int64_t>(tb.span) - static_cast<int64_t>(tb.center) * tb.q) << 32;
  const float* L = in[0];
  const float* R = CH == 2 ? in[1] : in[0];
  float* oL = out[0];
  float* oR = out[1];
  int64_t P = pos;
  for (uint32_t i = 0; i < frames; ++i, P += step) {
    const Phase ph = phaseOf(tb, vBase, qq, P);
    const __m256 tv = _mm256_set1_ps(ph.t);
    const float* row = ph.row;
    const float* xl = L + ph.first;
    __m256 l0 = _mm256_setzero_ps(), l1 = _mm256_setzero_ps();
    __m256 r0 = _mm256_setzero_ps(), r1 = _mm256_setzero_ps();
    uint32_t k = 0;
    if constexpr (CH == 2) {
      const float* xr = R + ph.first;
      for (; k + 16 <= K; k += 16) {
        const __m256 h0 = _mm256_fmadd_ps(tv, load8(row + K + k), load8(row + k));
        const __m256 h1 = _mm256_fmadd_ps(tv, load8(row + K + k + 8), load8(row + k + 8));
        l0 = _mm256_fmadd_ps(h0, _mm256_loadu_ps(xl + k), l0);
        r0 = _mm256_fmadd_ps(h0, _mm256_loadu_ps(xr + k), r0);
        l1 = _mm256_fmadd_ps(h1, _mm256_loadu_ps(xl + k + 8), l1);
        r1 = _mm256_fmadd_ps(h1, _mm256_loadu_ps(xr + k + 8), r1);
      }
      if (k < K) {
        const __m256 h0 = _mm256_fmadd_ps(tv, load8(row + K + k), load8(row + k));
        l0 = _mm256_fmadd_ps(h0, _mm256_loadu_ps(xl + k), l0);
        r0 = _mm256_fmadd_ps(h0, _mm256_loadu_ps(xr + k), r0);
      }
      const __m256 s = _mm256_hadd_ps(_mm256_add_ps(l0, l1), _mm256_add_ps(r0, r1));  // [l01 l23 r01 r23 | l45 l67 r45 r67]
      __m128 x = _mm_add_ps(_mm256_castps256_ps128(s), _mm256_extractf128_ps(s, 1));  // [la lb ra rb]
      x = _mm_hadd_ps(x, x);                                                           // [l r l r]
      const float g = env != nullptr ? env[i] : gain;
      oL[i] += _mm_cvtss_f32(x) * g;
      oR[i] += _mm_cvtss_f32(_mm_shuffle_ps(x, x, 1)) * g;
    } else {
      for (; k + 16 <= K; k += 16) {
        const __m256 h0 = _mm256_fmadd_ps(tv, load8(row + K + k), load8(row + k));
        const __m256 h1 = _mm256_fmadd_ps(tv, load8(row + K + k + 8), load8(row + k + 8));
        l0 = _mm256_fmadd_ps(h0, _mm256_loadu_ps(xl + k), l0);
        l1 = _mm256_fmadd_ps(h1, _mm256_loadu_ps(xl + k + 8), l1);
      }
      if (k < K) {
        const __m256 h0 = _mm256_fmadd_ps(tv, load8(row + K + k), load8(row + k));
        l0 = _mm256_fmadd_ps(h0, _mm256_loadu_ps(xl + k), l0);
      }
      const __m256 s = _mm256_add_ps(l0, l1);
      __m128 x = _mm_add_ps(_mm256_castps256_ps128(s), _mm256_extractf128_ps(s, 1));
      x = _mm_hadd_ps(x, x);
      x = _mm_hadd_ps(x, x);
      const float y = _mm_cvtss_f32(x) * (env != nullptr ? env[i] : gain);
      oL[i] += y;
      oR[i] += y;
    }
  }
}

}  // namespace

SincPrototype sincPrototype(ResampleQuality quality) noexcept {  // RT-safe
  if (quality == ResampleQuality::Draft) return {8, 0.42, 6.0};
  return {32, 0.4565, 8.6};
}

uint32_t sincStretchQ(double rate) noexcept {  // RT-safe
  const double r = std::abs(rate);
  if (!(r > 1.0)) return 256;
  const double q = std::floor(256.0 / r);
  return static_cast<uint32_t>(std::clamp(q, static_cast<double>(kMinStretchQ), 256.0));
}

const SincTable& prepareSincTable(ResampleQuality quality, uint32_t q) {
  q = std::clamp<uint32_t>(q, kMinStretchQ, 256);
  if (const SincTable* t = sincTable(quality, q)) return *t;
  std::lock_guard lock(gMutex);
  (void)build(quality, 256);
  return build(quality, q);
}

const SincTable* sincTable(ResampleQuality quality, uint32_t q) noexcept {  // RT-safe
  if (q < kMinStretchQ || q > 256) return nullptr;
  return gTables[static_cast<uint32_t>(quality)][q].load(std::memory_order_acquire);
}

const SincTable& sincTableAtLeast(ResampleQuality quality, uint32_t q) noexcept {  // RT-safe
  for (uint32_t k = std::clamp<uint32_t>(q, kMinStretchQ, 256); k < 256; ++k)
    if (const SincTable* t = sincTable(quality, k)) return *t;
  return *gTables[static_cast<uint32_t>(quality)][256].load(std::memory_order_acquire);
}

SincReach sincReach(const SincTable& t, int64_t pos, int64_t step, uint32_t frames) noexcept {  // RT-safe
  const int64_t end = frames > 0 ? pos + static_cast<int64_t>(frames - 1) * step : pos;
  const int64_t lo = std::min(pos, end) >> 32, hi = std::max(pos, end) >> 32;
  return {lo - static_cast<int64_t>(t.center), hi - static_cast<int64_t>(t.center) + 1 + static_cast<int64_t>(t.taps)};
}

void sincResample(const SincTable& t, const float* const* in, uint32_t channels, int64_t pos, int64_t step,
                  uint32_t frames, const float* env, float gain, float* const* out) noexcept {  // RT-safe
  if (channels >= 2) resampleImpl<2>(t, in, pos, step, frames, env, gain, out);
  else resampleImpl<1>(t, in, pos, step, frames, env, gain, out);
}

void gainCopy(const float* const* in, uint32_t channels, int64_t first, int32_t dir, uint32_t frames,
              const float* env, float gain, float* const* out) noexcept {  // RT-safe
  const uint32_t ch = channels >= 2 ? 2u : 1u;
  const __m256 g8 = _mm256_set1_ps(gain);
  const __m256i rev = _mm256_setr_epi32(7, 6, 5, 4, 3, 2, 1, 0);
  for (uint32_t c = 0; c < 2; ++c) {
    const float* x = in[ch == 2 ? c : 0] + first;
    float* o = out[c];
    uint32_t i = 0;
    if (dir > 0) {
      for (; i + 8 <= frames; i += 8) {
        const __m256 g = env != nullptr ? _mm256_loadu_ps(env + i) : g8;
        _mm256_storeu_ps(o + i, _mm256_fmadd_ps(_mm256_loadu_ps(x + i), g, _mm256_loadu_ps(o + i)));
      }
      for (; i < frames; ++i) o[i] += x[i] * (env != nullptr ? env[i] : gain);
    } else {
      for (; i + 8 <= frames; i += 8) {  // x[−i − 7 … −i], reversed
        const __m256 v = _mm256_permutevar8x32_ps(_mm256_loadu_ps(x - static_cast<int64_t>(i) - 7), rev);
        const __m256 g = env != nullptr ? _mm256_loadu_ps(env + i) : g8;
        _mm256_storeu_ps(o + i, _mm256_fmadd_ps(v, g, _mm256_loadu_ps(o + i)));
      }
      for (; i < frames; ++i) o[i] += x[-static_cast<int64_t>(i)] * (env != nullptr ? env[i] : gain);
    }
  }
}

namespace {

// The decimator (resample.h "Decimated copies"): 2J + 1 taps, padded with zeros to a multiple of 4.
constexpr uint32_t kDecTaps = 2 * kDecimatorHalfTaps + 1;
constexpr uint32_t kDecPadded = (kDecTaps + 3) / 4 * 4;

struct Decimator {
  alignas(32) double h[kDecPadded] = {};
  Decimator() {
    constexpr double fc = 0.23, beta = 11.7;
    const double J = kDecimatorHalfTaps;
    const double i0b = besselI0(beta);
    double sum = 0.0;
    for (uint32_t k = 0; k < kDecTaps; ++k) {
      const double j = static_cast<double>(k) - J;
      const double x = j / (J + 1.0);
      const double win = besselI0(beta * std::sqrt(1.0 - x * x)) / i0b;
      const double a = 2.0 * fc * j;
      const double sinc = a == 0.0 ? 1.0 : std::sin(std::numbers::pi * a) / (std::numbers::pi * a);
      h[k] = 2.0 * fc * sinc * win;
      sum += h[k];
    }
    for (uint32_t k = 0; k < kDecTaps; ++k) h[k] /= sum;
  }
};

const Decimator& decimator() {
  static const Decimator d;
  return d;
}

}  // namespace

const double* decimatorTaps() noexcept { return decimator().h; }

void decimate2(const float* x, int64_t xFirst, int64_t xLast, float* y, int64_t mFirst, int64_t count) noexcept {
  const double* h = decimator().h;
  const auto J = static_cast<int64_t>(kDecimatorHalfTaps);
  for (int64_t i = 0; i < count; ++i) {
    const int64_t lo = 2 * (mFirst + i) - J;  // y = Σ_k h_k · x[lo + k] (h is symmetric)
    double acc = 0.0;
    if (lo >= xFirst && lo + static_cast<int64_t>(kDecPadded) <= xLast) {
      const float* p = x + lo;
      __m256d a0 = _mm256_setzero_pd(), a1 = _mm256_setzero_pd();
      uint32_t k = 0;
      for (; k + 8 <= kDecPadded; k += 8) {
        a0 = _mm256_fmadd_pd(_mm256_cvtps_pd(_mm_loadu_ps(p + k)), _mm256_load_pd(h + k), a0);
        a1 = _mm256_fmadd_pd(_mm256_cvtps_pd(_mm_loadu_ps(p + k + 4)), _mm256_load_pd(h + k + 4), a1);
      }
      for (; k < kDecPadded; k += 4) a0 = _mm256_fmadd_pd(_mm256_cvtps_pd(_mm_loadu_ps(p + k)), _mm256_load_pd(h + k), a0);
      const __m256d s = _mm256_add_pd(a0, a1);
      const __m128d t = _mm_add_pd(_mm256_castpd256_pd128(s), _mm256_extractf128_pd(s, 1));
      acc = _mm_cvtsd_f64(_mm_add_sd(t, _mm_unpackhi_pd(t, t)));
    } else {  // near the ends: only the frames that exist
      const int64_t k0 = std::max<int64_t>(0, xFirst - lo), k1 = std::min<int64_t>(kDecTaps, xLast - lo);
      for (int64_t k = k0; k < k1; ++k) acc += h[k] * static_cast<double>(x[lo + k]);
    }
    y[i] = static_cast<float>(acc);
  }
}

const float* sincPrototypeTable(ResampleQuality quality) noexcept {
  (void)prepareSincTable(quality, 256);
  // The prototype is held by build()'s static; rebuild a copy for tests through a static of our own.
  static std::array<std::vector<float>, kQualities> copies;
  const auto qi = static_cast<uint32_t>(quality);
  std::lock_guard lock(gMutex);
  if (copies[qi].empty()) copies[qi] = buildPrototype(sincPrototype(quality));
  return copies[qi].data();
}

}  // namespace pq::dsp
