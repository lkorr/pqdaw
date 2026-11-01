#pragma once

// Test inputs for the frame-table tests (TMM.1): basic cycles and the "drawn" fixture (64 keyframes of smoothed
// random walks, normalised to 0 dBFS, then every harmonic capped to a_h·h ≤ 0.09, the plan's bound for the float
// phase's precision floor; docs/plans/MM.md Decisions → DSP "Phase precision").

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <random>
#include <vector>

#include "pq/dsp/frame_table_build.h"
#include "pq/dsp/tables.h"

namespace ftfix {

using Cycle = std::vector<float>;

inline Cycle basic(pq::dsp::BasicCycle s) {
  const auto b = pq::dsp::basicCycle(s);
  return Cycle(b.begin(), b.end());
}

// A cycle from harmonic amplitudes / phases: x(φ) = Σ a_h cos(2πhφ + p_h).
inline Cycle fromHarmonics(const std::vector<std::pair<uint32_t, std::pair<double, double>>>& parts) {
  Cycle c(pq::dsp::kKeyframeCycle);
  for (uint32_t i = 0; i < c.size(); ++i) {
    double v = 0;
    for (const auto& [h, ap] : parts)
      v += ap.first * std::cos(2.0 * std::numbers::pi * h * i / c.size() + ap.second);
    c[i] = static_cast<float>(v);
  }
  return c;
}

inline std::vector<Cycle> drawnFixture(uint32_t count = 64, uint32_t seed = 11) {
  constexpr uint32_t n = pq::dsp::kKeyframeCycle;
  std::mt19937 rng(seed);
  std::uniform_real_distribution<double> step(-1.0, 1.0);
  std::vector<Cycle> out;
  std::vector<double> re(n), im(n);
  for (uint32_t k = 0; k < count; ++k) {
    // A random walk made periodic (trend removed), smoothed by a 9-tap circular moving average.
    std::vector<double> w(n);
    double acc = 0;
    for (uint32_t i = 0; i < n; ++i) w[i] = acc += step(rng);
    const double drift = (w[n - 1] - w[0]) / (n - 1);
    for (uint32_t i = 0; i < n; ++i) w[i] -= drift * i;
    for (uint32_t i = 0; i < n; ++i) {
      double s = 0;
      for (int d = -4; d <= 4; ++d) s += w[(i + n + d) % n];
      re[i] = s / 9.0;
      im[i] = 0;
    }
    double mean = 0;
    for (const double v : re) mean += v;
    mean /= n;
    double peak = 0;
    for (double& v : re) peak = std::max(peak, std::fabs(v -= mean));
    for (double& v : re) v /= peak;
    pq::dsp::fftInPlace(re, im, false);
    for (uint32_t h = 1; h < n / 2; ++h) {
      const double a = 2.0 * std::hypot(re[h], im[h]) / n;
      const double cap = 0.09 / h;
      if (a > cap) {
        const double g = cap / a;
        re[h] *= g, im[h] *= g, re[n - h] *= g, im[n - h] *= g;
      }
    }
    re[n / 2] = im[n / 2] = 0;
    pq::dsp::fftInPlace(re, im, true);
    Cycle c(n);
    for (uint32_t i = 0; i < n; ++i) c[i] = static_cast<float>(re[i] / n);
    out.push_back(std::move(c));
  }
  return out;
}

// A spec over cycles at evenly spaced positions (one cycle: position 0).
struct SpecHolder {
  std::vector<Cycle> cycles;
  std::vector<pq::dsp::Keyframe> keys;
  std::vector<pq::dsp::GapMode> gaps;
  pq::dsp::FrameTableSpec spec;
  SpecHolder(std::vector<Cycle> c, double sampleRate, uint32_t dense = 64,
             pq::dsp::GapMode mode = pq::dsp::GapMode::Spectral)
      : cycles(std::move(c)) {
    for (std::size_t i = 0; i < cycles.size(); ++i) {
      const float pos = cycles.size() > 1 ? static_cast<float>(static_cast<double>(i) / (cycles.size() - 1)) : 0.0f;
      keys.push_back({pos, cycles[i]});
    }
    gaps.assign(cycles.size() - 1, mode);
    spec.keyframes = keys;
    spec.gaps = gaps;
    spec.denseFrames = dense;
    spec.sampleRate = sampleRate;
  }
  SpecHolder(const SpecHolder&) = delete;
};

}  // namespace ftfix
