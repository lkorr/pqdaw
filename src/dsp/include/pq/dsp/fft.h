#pragma once

// Real FFT for the analysers (T4.6: the spectrum of SPEC §11.1). Radix-2, decimation in time, on split real /
// imaginary arrays (struct of arrays: the butterflies of a stage are contiguous and vectorise), via an N/2-point
// complex transform and the usual real-input post-processing. Float32 throughout; twiddles computed in double.
//
// Not for the audio thread (the analysis thread runs it), but forward() / power() never allocate once constructed.

#include <cstdint>
#include <vector>

namespace pq::dsp {

class RealFft {
public:
  // n: a power of two, 8 … 65536. Allocates the tables and scratch.
  explicit RealFft(uint32_t n);
  uint32_t size() const noexcept { return n_; }

  // Spectrum of n real samples: re[k], im[k] for k = 0 … n/2 (n/2 + 1 bins each). Unnormalised (X[0] = Σ x).
  void forward(const float* x, float* re, float* im) noexcept;
  // |X[k]|² for k = 0 … n/2 (n/2 + 1 values).
  void power(const float* x, float* out) noexcept;

private:
  void complexFft() noexcept;  // in place on zr_, zi_ (bit-reversed input order is applied first)

  uint32_t n_, h_;                   // h_ = n / 2: the complex transform's size
  std::vector<uint32_t> rev_;        // bit reversal of h_
  std::vector<float> twr_, twi_;     // per stage (len 2, 4, … h_): e^(−2πij/len), j < len/2, concatenated
  std::vector<float> postr_, posti_; // e^(−2πik/n), k ≤ n/4 (real post-processing)
  std::vector<float> zr_, zi_;       // scratch
};

}  // namespace pq::dsp
