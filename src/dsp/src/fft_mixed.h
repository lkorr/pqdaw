#pragma once

// Private to pq_dsp: a mixed-radix (2, 3, 4, 5, 7) complex FFT in double for the frame-table builder (TMM.1), whose
// level lengths are 8·(7-smooth) and not powers of two. Recursive decimation in time with precomputed twiddles
// (the structure of KISS FFT, reimplemented). Off the audio thread; transforms never allocate once constructed.
//
// Two element types: one transform of std::complex<double>, or Cd4 = four independent transforms side by side in the
// lanes of AVX2 registers (struct of arrays: element k holds bin k of all four), which is what the builder uses for
// its batches of frames (4 lanes × 2 real signals per complex transform = 8 frames per call).

#include <immintrin.h>

#include <complex>
#include <cstdint>
#include <vector>

namespace pq::dsp::detail {

using Cd = std::complex<double>;

struct Cd4 {
  __m256d re, im;
};

class MixedFft {
public:
  MixedFft() = default;
  // n = 2^a·3^b·5^c·7^d ≥ 2. Allocates the twiddles. Returns false for other n.
  bool init(uint32_t n);
  uint32_t size() const noexcept { return n_; }
  // out[k] = Σ_j in[j]·e^(∓2πijk/n): forward (−) or inverse (+, unscaled). in and out must not overlap.
  void forward(const Cd* in, Cd* out) const noexcept;
  void inverse(const Cd* in, Cd* out) const noexcept;
  void forward(const Cd4* in, Cd4* out) const noexcept;
  void inverse(const Cd4* in, Cd4* out) const noexcept;

private:
  template <class V, bool kInv>
  void work(V* out, const V* in, std::size_t stride, const uint32_t* f) const noexcept;
  uint32_t n_ = 0;
  std::vector<Cd> tw_;            // e^(−2πik/n)
  std::vector<uint32_t> factors_;  // (p, m) pairs: radix, then the remaining length
};

}  // namespace pq::dsp::detail
