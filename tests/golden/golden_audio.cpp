#include "golden_audio.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>

namespace golden {

namespace {

constexpr double kPi = 3.14159265358979323846;

// In-place iterative radix-2 FFT (n a power of two).
void fft(std::vector<std::complex<double>>& a) {
  const size_t n = a.size();
  for (size_t i = 1, j = 0; i < n; ++i) {
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(a[i], a[j]);
  }
  for (size_t len = 2; len <= n; len <<= 1) {
    const double ang = -2.0 * kPi / static_cast<double>(len);
    const std::complex<double> wl(std::cos(ang), std::sin(ang));
    for (size_t i = 0; i < n; i += len) {
      std::complex<double> w(1.0, 0.0);
      for (size_t k = 0; k < len / 2; ++k) {
        const std::complex<double> u = a[i + k], v = a[i + k + len / 2] * w;
        a[i + k] = u + v;
        a[i + k + len / 2] = u - v;
        w *= wl;
      }
    }
  }
}

std::string fmt(const char* f, double a, double b = 0, double c = 0, double d = 0) {
  char buf[256];
  std::snprintf(buf, sizeof buf, f, a, b, c, d);
  return buf;
}

}  // namespace

std::vector<std::vector<double>> bandLevels(const std::vector<float>& x, uint32_t sampleRate, const Tolerance& tol,
                                            std::vector<double>* centres) {
  const size_t n = tol.fftSize;
  const size_t hop = n / 2;
  std::vector<double> w(n);
  double w2 = 0.0;
  for (size_t i = 0; i < n; ++i) {
    w[i] = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(n));
    w2 += w[i] * w[i];
  }
  // Energy of a full-scale sine summed over the positive-frequency bins (Parseval): n/4 · Σw².
  const double ref = static_cast<double>(n) / 4.0 * w2;
  // Semitone band edges from 20 Hz, as bin indices; each band at least one bin wide.
  const double df = static_cast<double>(sampleRate) / static_cast<double>(n);
  std::vector<size_t> edges{0};
  for (int k = 0;; ++k) {
    const double f = 20.0 * std::exp2(k / 12.0);
    if (f >= sampleRate / 2.0) break;
    const size_t b = static_cast<size_t>(std::lround(f / df));
    if (b > edges.back()) edges.push_back(b);
  }
  edges.push_back(n / 2 + 1);
  if (centres) {
    centres->clear();
    for (size_t b = 0; b + 1 < edges.size(); ++b) centres->push_back(0.5 * (edges[b] + edges[b + 1] - 1) * df);
  }
  const double floorE = std::pow(10.0, tol.floorDb / 10.0);
  std::vector<std::vector<double>> out;
  // Whole windows only (a zero-padded partial window would splatter a phase-dependent truncation edge across every
  // band): every hop, plus one more aligned to the end so the tail is covered. Shorter signals: one padded window.
  std::vector<size_t> starts;
  for (size_t s = 0; s + n <= x.size(); s += hop) starts.push_back(s);
  if (starts.empty()) starts.push_back(0);
  else if (starts.back() + n < x.size()) starts.push_back(x.size() - n);
  std::vector<std::complex<double>> buf(n);
  for (const size_t start : starts) {
    for (size_t i = 0; i < n; ++i) {
      const size_t s = start + i;
      buf[i] = {s < x.size() ? static_cast<double>(x[s]) * w[i] : 0.0, 0.0};
    }
    fft(buf);
    std::vector<double> levels;
    for (size_t b = 0; b + 1 < edges.size(); ++b) {
      double e = 0.0;
      for (size_t k = edges[b]; k < edges[b + 1]; ++k) e += std::norm(buf[k]);
      levels.push_back(10.0 * std::log10(std::max(e / ref, floorE)));
    }
    out.push_back(std::move(levels));
  }
  return out;
}

Comparison compare(const Channels& golden, uint32_t goldenRate, const Channels& actual, uint32_t actualRate,
                   const Tolerance& tol) {
  Comparison c;
  if (golden.size() != actual.size() || goldenRate != actualRate || golden.empty()) {
    c.message = fmt("format differs: golden %.0f ch at %.0f Hz, render %.0f ch at %.0f Hz",
                    static_cast<double>(golden.size()), goldenRate, static_cast<double>(actual.size()), actualRate);
    return c;
  }
  for (size_t ch = 0; ch < golden.size(); ++ch)
    if (golden[ch].size() != actual[ch].size()) {
      c.message = fmt("length differs: golden %.0f frames, render %.0f frames (channel %.0f)",
                      static_cast<double>(golden[ch].size()), static_cast<double>(actual[ch].size()),
                      static_cast<double>(ch));
      return c;
    }
  for (size_t ch = 0; ch < actual.size(); ++ch)
    for (size_t i = 0; i < actual[ch].size(); ++i)
      if (!std::isfinite(actual[ch][i])) {
        c.message = fmt("render has a non-finite sample at frame %.0f, channel %.0f", static_cast<double>(i),
                        static_cast<double>(ch));
        return c;
      }
  c.formatOk = true;
  for (size_t ch = 0; ch < golden.size(); ++ch)
    for (size_t i = 0; i < golden[ch].size(); ++i) {
      const double d = std::abs(static_cast<double>(actual[ch][i]) - static_cast<double>(golden[ch][i]));
      if (!(d <= c.maxAbs)) {  // NaN in the golden counts as a difference
        c.maxAbs = std::isfinite(d) ? d : 1e30;
        c.maxAbsChannel = static_cast<uint32_t>(ch);
        c.maxAbsFrame = i;
      }
    }
  for (size_t ch = 0; ch < golden.size(); ++ch) {
    std::vector<double> centres;
    const auto lg = bandLevels(golden[ch], goldenRate, tol, &centres);
    const auto la = bandLevels(actual[ch], actualRate, tol);
    for (size_t f = 0; f < lg.size(); ++f)
      for (size_t b = 0; b < lg[f].size(); ++b) {
        const double d = std::abs(lg[f][b] - la[f][b]);
        if (d > c.maxSpectralDb) {
          c.maxSpectralDb = d;
          c.worstChannel = static_cast<uint32_t>(ch);
          c.worstFrame = static_cast<uint32_t>(f);
          c.worstBandHz = centres[b];
        }
      }
  }
  const bool absOk = c.maxAbs <= tol.absTol;
  const bool specOk = c.maxSpectralDb <= tol.spectralDb;
  c.ok = absOk && specOk;
  c.message = fmt("max |diff| %.3g (tol %.3g)", c.maxAbs, tol.absTol) +
              fmt(" at frame %.0f ch %.0f; ", static_cast<double>(c.maxAbsFrame), c.maxAbsChannel) +
              fmt("max band diff %.3g dB (tol %.3g dB)", c.maxSpectralDb, tol.spectralDb) +
              fmt(" at %.0f Hz, frame %.0f", c.worstBandHz, c.worstFrame) + (c.ok ? "" : absOk ? " [spectral FAIL]" : specOk ? " [absolute FAIL]" : " [absolute + spectral FAIL]");
  return c;
}

}  // namespace golden
