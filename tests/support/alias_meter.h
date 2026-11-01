#pragma once

// Alias meter (TMM.1; reused by the Wavetable module's tests, TMM.5 / TMM.8). For a signal that is exactly periodic
// in the analysis length (a bin-exact fundamental: f0 = k0·sr / N, N a power of two), a rectangular-window DFT has no
// leakage: every bin that is not a multiple of k0 holds only aliasing (and noise), and every multiple of k0 holds one
// harmonic. The meter reports
//   - the loudest non-harmonic bin below `aliasLimitHz` (20 kHz): its level in dBFS (a full-scale sine = 0 dBFS) and
//     frequency;
//   - over the harmonics up to `presentHz` (19.8 kHz) whose expected amplitude is given: the largest level error in
//     dB for harmonics at or above −60 dBFS, and the largest absolute amplitude error for quieter ones (a 0.1 dB
//     check is meaningless near the alias floor: −100 dBFS of alias landing on a −60 dBFS harmonic is 0.09 dB).
// The DFT is pq::dsp::RealFft (float; its rounding floor is far below −150 dBFS at N = 65536).

#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

#include "pq/dsp/fft.h"

namespace pq::test {

struct AliasReport {
  double worstAliasDb = -400;   // dBFS of the loudest non-harmonic bin below the alias limit
  double worstAliasHz = 0;
  double worstHarmonicDb = 0;   // largest |level error| in dB over the checked loud harmonics
  uint32_t worstHarmonic = 0;   // its harmonic number
  double worstQuietError = 0;   // largest |amplitude error| over the checked quiet harmonics
  uint32_t harmonicsChecked = 0;
};

class AliasMeter {
public:
  explicit AliasMeter(uint32_t n) : fft_(n), re_(n / 2 + 1), im_(n / 2 + 1) {}
  uint32_t size() const noexcept { return fft_.size(); }

  // x: N samples; k0: the fundamental's bin; expected(h): the expected amplitude of harmonic h (< 0 = do not check).
  template <class Expected>
  AliasReport measure(std::span<const float> x, uint32_t k0, double sampleRate, Expected&& expected,
                      double aliasLimitHz = 20000.0, double presentHz = 19800.0) {
    const uint32_t n = fft_.size();
    fft_.forward(x.data(), re_.data(), im_.data());
    AliasReport r;
    const double binHz = sampleRate / n;
    for (uint32_t b = 1; b <= n / 2; ++b) {
      const double hz = b * binHz;
      const double amp = 2.0 * std::hypot(static_cast<double>(re_[b]), static_cast<double>(im_[b])) / n;
      if (b % k0 != 0) {
        if (hz < aliasLimitHz) {
          const double db = 20.0 * std::log10(std::max(amp, 1e-30));
          if (db > r.worstAliasDb) {
            r.worstAliasDb = db;
            r.worstAliasHz = hz;
          }
        }
        continue;
      }
      if (hz > presentHz) continue;
      const uint32_t h = b / k0;
      const double want = expected(h);
      if (want < 0) continue;
      ++r.harmonicsChecked;
      if (want >= 1e-3) {
        const double err = std::fabs(20.0 * std::log10(std::max(amp, 1e-30) / want));
        if (err > r.worstHarmonicDb) {
          r.worstHarmonicDb = err;
          r.worstHarmonic = h;
        }
      } else {
        r.worstQuietError = std::max(r.worstQuietError, std::fabs(amp - want));
      }
    }
    return r;
  }

private:
  pq::dsp::RealFft fft_;
  std::vector<float> re_, im_;
};

}  // namespace pq::test
