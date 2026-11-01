#include "fft_mixed.h"

#include <cmath>
#include <numbers>

namespace pq::dsp::detail {

bool MixedFft::init(uint32_t n) {
  if (n < 2) return false;
  std::vector<uint32_t> f;
  uint32_t m = n;
  while (m % 4 == 0) { f.push_back(4); m /= 4; }
  while (m % 2 == 0) { f.push_back(2); m /= 2; }
  while (m % 3 == 0) { f.push_back(3); m /= 3; }
  while (m % 5 == 0) { f.push_back(5); m /= 5; }
  while (m % 7 == 0) { f.push_back(7); m /= 7; }
  if (m != 1) return false;
  n_ = n;
  factors_.clear();
  uint32_t rest = n;
  for (const uint32_t p : f) {
    rest /= p;
    factors_.push_back(p);
    factors_.push_back(rest);
  }
  tw_.resize(n);
  for (uint32_t k = 0; k < n; ++k) tw_[k] = std::polar(1.0, -2.0 * std::numbers::pi * k / n);
  return true;
}

namespace {

// The element operations, for one complex value or four in AVX2 lanes.
inline Cd add(Cd a, Cd b) noexcept { return a + b; }
inline Cd sub(Cd a, Cd b) noexcept { return a - b; }
inline Cd mul(Cd a, Cd w) noexcept {
  return {a.real() * w.real() - a.imag() * w.imag(), a.real() * w.imag() + a.imag() * w.real()};
}
inline Cd mulI(Cd a) noexcept { return {-a.imag(), a.real()}; }  // i·a

inline Cd4 add(Cd4 a, Cd4 b) noexcept { return {_mm256_add_pd(a.re, b.re), _mm256_add_pd(a.im, b.im)}; }
inline Cd4 sub(Cd4 a, Cd4 b) noexcept { return {_mm256_sub_pd(a.re, b.re), _mm256_sub_pd(a.im, b.im)}; }
inline Cd4 mul(Cd4 a, Cd w) noexcept {
  const __m256d wr = _mm256_set1_pd(w.real()), wi = _mm256_set1_pd(w.imag());
  return {_mm256_fmsub_pd(a.re, wr, _mm256_mul_pd(a.im, wi)), _mm256_fmadd_pd(a.re, wi, _mm256_mul_pd(a.im, wr))};
}
inline Cd4 mulI(Cd4 a) noexcept { return {_mm256_sub_pd(_mm256_setzero_pd(), a.im), a.re}; }

inline Cd twiddle(const Cd* tw, std::size_t k, bool inv) noexcept { return inv ? std::conj(tw[k]) : tw[k]; }

template <class V, bool kInv>
void butterflies(V* out, std::size_t stride, uint32_t p, uint32_t m, const Cd* tw, std::size_t n) noexcept {
  // out[u + q·m] for q < p are the p sub-transforms' bin u; combine with twiddles e^(∓2πi·u·q/(p·m)).
  switch (p) {
    case 2:
      for (uint32_t u = 0; u < m; ++u) {
        const V t = mul(out[u + m], twiddle(tw, u * stride, kInv));
        out[u + m] = sub(out[u], t);
        out[u] = add(out[u], t);
      }
      break;
    case 4:
      for (uint32_t u = 0; u < m; ++u) {
        const V a0 = out[u];
        const V a1 = mul(out[u + m], twiddle(tw, u * stride, kInv));
        const V a2 = mul(out[u + 2 * m], twiddle(tw, 2 * u * stride, kInv));
        const V a3 = mul(out[u + 3 * m], twiddle(tw, 3 * u * stride, kInv));
        const V s02 = add(a0, a2), d02 = sub(a0, a2), s13 = add(a1, a3);
        // forward: X1 = d02 − i·d13, X3 = d02 + i·d13 (inverse: the other way round)
        const V id13 = mulI(sub(a1, a3));
        out[u] = add(s02, s13);
        out[u + 2 * m] = sub(s02, s13);
        out[u + m] = kInv ? add(d02, id13) : sub(d02, id13);
        out[u + 3 * m] = kInv ? sub(d02, id13) : add(d02, id13);
      }
      break;
    default: {  // 3, 5, 7: the twiddled inputs, then a direct p-point DFT with the p-th roots of unity
      Cd root[7];
      for (uint32_t q = 0; q < p; ++q) root[q] = twiddle(tw, q * (n / p), kInv);  // e^(∓2πiq/p)
      V s[7];
      for (uint32_t u = 0; u < m; ++u) {
        s[0] = out[u];
        for (uint32_t q = 1; q < p; ++q) s[q] = mul(out[u + q * m], twiddle(tw, q * u * stride, kInv));
        for (uint32_t k = 0; k < p; ++k) {
          V acc = s[0];
          uint32_t r = 0;
          for (uint32_t q = 1; q < p; ++q) {
            r += k;
            if (r >= p) r -= p;
            acc = add(acc, mul(s[q], root[r]));
          }
          out[u + k * m] = acc;
        }
      }
      break;
    }
  }
}

}  // namespace

template <class V, bool kInv>
void MixedFft::work(V* out, const V* in, std::size_t stride, const uint32_t* f) const noexcept {
  const uint32_t p = f[0], m = f[1];
  if (m == 1) {
    for (uint32_t q = 0; q < p; ++q) out[q] = in[q * stride];
  } else {
    for (uint32_t q = 0; q < p; ++q)
      work<V, kInv>(out + static_cast<std::size_t>(q) * m, in + q * stride, stride * p, f + 2);
  }
  butterflies<V, kInv>(out, stride, p, m, tw_.data(), n_);
}

void MixedFft::forward(const Cd* in, Cd* out) const noexcept { work<Cd, false>(out, in, 1, factors_.data()); }
void MixedFft::inverse(const Cd* in, Cd* out) const noexcept { work<Cd, true>(out, in, 1, factors_.data()); }
void MixedFft::forward(const Cd4* in, Cd4* out) const noexcept { work<Cd4, false>(out, in, 1, factors_.data()); }
void MixedFft::inverse(const Cd4* in, Cd4* out) const noexcept { work<Cd4, true>(out, in, 1, factors_.data()); }

}  // namespace pq::dsp::detail
