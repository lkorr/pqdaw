// Real FFT (pq/dsp/fft.h).

#include "pq/dsp/fft.h"

#include <bit>
#include <cassert>
#include <cmath>

namespace pq::dsp {

RealFft::RealFft(uint32_t n) : n_(n), h_(n / 2) {
  assert(std::has_single_bit(n) && n >= 8 && n <= 65536);
  const int bits = std::countr_zero(h_);
  rev_.resize(h_);
  for (uint32_t i = 0; i < h_; ++i) {
    uint32_t r = 0;
    for (int b = 0; b < bits; ++b) r |= ((i >> b) & 1u) << (bits - 1 - b);
    rev_[i] = r;
  }
  const double pi = 3.14159265358979323846;
  for (uint32_t len = 2; len <= h_; len *= 2)
    for (uint32_t j = 0; j < len / 2; ++j) {
      const double a = -2.0 * pi * static_cast<double>(j) / static_cast<double>(len);
      twr_.push_back(static_cast<float>(std::cos(a)));
      twi_.push_back(static_cast<float>(std::sin(a)));
    }
  postr_.resize(n_ / 4 + 1);
  posti_.resize(n_ / 4 + 1);
  for (uint32_t k = 0; k <= n_ / 4; ++k) {
    const double a = -2.0 * pi * static_cast<double>(k) / static_cast<double>(n_);
    postr_[k] = static_cast<float>(std::cos(a));
    posti_[k] = static_cast<float>(std::sin(a));
  }
  zr_.resize(h_);
  zi_.resize(h_);
}

void RealFft::complexFft() noexcept {
  float* zr = zr_.data();
  float* zi = zi_.data();
  const float* tr = twr_.data();
  const float* ti = twi_.data();
  for (uint32_t len = 2; len <= h_; len *= 2) {
    const uint32_t half = len / 2;
    for (uint32_t s = 0; s < h_; s += len) {
      float* ar = zr + s;
      float* ai = zi + s;
      float* br = zr + s + half;
      float* bi = zi + s + half;
      // Contiguous in j: vectorises for half ≥ 8.
      for (uint32_t j = 0; j < half; ++j) {
        const float xr = br[j] * tr[j] - bi[j] * ti[j];
        const float xi = br[j] * ti[j] + bi[j] * tr[j];
        br[j] = ar[j] - xr;
        bi[j] = ai[j] - xi;
        ar[j] += xr;
        ai[j] += xi;
      }
    }
    tr += half;
    ti += half;
  }
}

void RealFft::forward(const float* x, float* re, float* im) noexcept {
  // Pack even / odd samples as one complex sequence, in bit-reversed order.
  for (uint32_t i = 0; i < h_; ++i) {
    const uint32_t r = rev_[i];
    zr_[r] = x[2 * i];
    zi_[r] = x[2 * i + 1];
  }
  complexFft();
  // X[k] = E[k] + W^k O[k], E = (Z[k] + Z*[h−k]) / 2, O = (Z[k] − Z*[h−k]) / 2i, for k = 0 … h.
  re[0] = zr_[0] + zi_[0];
  im[0] = 0.0f;
  re[h_] = zr_[0] - zi_[0];
  im[h_] = 0.0f;
  for (uint32_t k = 1; k <= h_ / 2; ++k) {
    const uint32_t m = h_ - k;
    const float er = 0.5f * (zr_[k] + zr_[m]), ei = 0.5f * (zi_[k] - zi_[m]);
    const float orr = 0.5f * (zi_[k] + zi_[m]), oi = -0.5f * (zr_[k] - zr_[m]);
    const float wr = postr_[k], wi = posti_[k];
    const float tr = wr * orr - wi * oi, ti = wr * oi + wi * orr;
    re[k] = er + tr;
    im[k] = ei + ti;
    // X[h − k] = conj(E[k]) + W^(h−k) conj(O[k]) = conj(E[k] − W^k O[k]) (W^h = −1, W^(−k) = conj W^k).
    re[m] = er - tr;
    im[m] = -(ei - ti);
  }
}

void RealFft::power(const float* x, float* out) noexcept {
  // Reuse the halves of `out` is not possible (n/2 + 1 outputs); go through the scratch directly.
  for (uint32_t i = 0; i < h_; ++i) {
    const uint32_t r = rev_[i];
    zr_[r] = x[2 * i];
    zi_[r] = x[2 * i + 1];
  }
  complexFft();
  const float dc = zr_[0] + zi_[0], ny = zr_[0] - zi_[0];
  out[0] = dc * dc;
  out[h_] = ny * ny;
  for (uint32_t k = 1; k <= h_ / 2; ++k) {
    const uint32_t m = h_ - k;
    const float er = 0.5f * (zr_[k] + zr_[m]), ei = 0.5f * (zi_[k] - zi_[m]);
    const float orr = 0.5f * (zi_[k] + zi_[m]), oi = -0.5f * (zr_[k] - zr_[m]);
    const float wr = postr_[k], wi = posti_[k];
    const float tr = wr * orr - wi * oi, ti = wr * oi + wi * orr;
    const float ar = er + tr, ai = ei + ti;
    const float br = er - tr, bi = ei - ti;
    out[k] = ar * ar + ai * ai;
    out[m] = br * br + bi * bi;
  }
}

}  // namespace pq::dsp
