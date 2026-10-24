#pragma once

// The tuning node's table (T2.6, SPEC §3.3): the per-interval linear form of an EDO or MOS tuning, as the read-only
// resource of the `tuning_hz` kernel (KernelArgs::resource → const TuningTableView*).
//
// The data comes from pq::tuning::linearTuningForm (pq/tuning/tuning_signal.h; dsp does not depend on tuning, so
// the owner is built from plain arrays). The math, for absolute degree D = equave·N + degree (N = n·periodsPerEquave)
// split as D = q·N + p·n + j, generator g and period P (cents):
//     MOS:  t = the interval holding g/P − s (s = floor(g/P)),  K = k[t][j],  M = m[t][j] + K·s − p − q·periodsPerEquave
//     EDO:  g = P / n (the generator follows the equave),       K = j,        M = −q          (one interval, never left)
//     pure cents = K·g − M·P,   stretched cents = pure · (1200 + stretch) / 1200,   Hz = reference · 2^(cents / 1200)
// (K, M) stay valid while lo·P ≤ g < hi·P (lo, hi = s + the interval's bounds), so the kernel re-derives them only
// when the generator leaves the interval (a region crossing, detected at the crossing sample) or the degree changes.
//
// Unmodulated inputs: the node's param-rate inputs are float32 param slots. An input that equals float32(x) of the
// definition's double value x (generator, period, stretch, reference) reads x exactly, so the computed pitch is
// within an ulp of the document's (resolveHz's double result rounded once). When EVERY tuning param input of a lane is
// at its definition value in a block, the kernel passes the note's own `hz` input through instead (T2.6-fix1): the
// project lowering resolves NoteDesc::hz in this table's definition, so an unmodulated tuning sounds bit-identically
// to static pitches. Audio-rate inputs are used as is.

#include <cmath>
#include <cstdint>
#include <limits>
#include <span>

#include "pq/core/aligned_buffer.h"
#include "pq/core/result.h"

namespace pq::dsp {

enum class TuningTableKind : uint32_t { Edo = 0, Mos = 1 };

struct TuningTableView {
  TuningTableKind kind = TuningTableKind::Edo;
  int32_t n = 12;                 // notes per period (EDO: steps per equave)
  int32_t periodsPerEquave = 1;
  int32_t intervals = 1;
  const double* bounds = nullptr; // intervals + 1 fractions of the period, 0 … 1
  const int32_t* k = nullptr;     // intervals × n (rotated to the mode)
  const int32_t* m = nullptr;     // intervals × n (rotated to the mode, period shift 0)
  double generator = 100.0;       // the definition's values (see "Unmodulated inputs")
  double period = 1200.0;
  double stretch = 0.0;
  double referenceHz = 261.626;
  float generatorF = 100.0f;      // float32 of the above
  float periodF = 1200.0f;
  float stretchF = 0.0f;
  float referenceF = 261.626f;
};

// One lane's linear coefficients: pure cents = K·g − M·P while lo·P ≤ g < hi·P (lo = hi = NaN: invalid inputs,
// never in range).
struct TuningCoefficients {
  double K = 0.0;
  double M = 0.0;
  double lo = -std::numeric_limits<double>::infinity();
  double hi = std::numeric_limits<double>::infinity();
};

// floor division helper (D, N > 0) → (q, r) with 0 ≤ r < N. RT-safe.
inline void tuningFloorDiv(int64_t d, int64_t n, int64_t& q, int64_t& r) noexcept {
  q = d / n;
  r = d % n;
  if (r < 0) {
    r += n;
    --q;
  }
}

// Coefficients of absolute degree `degree` for generator g and period P (EDO: g is ignored). RT-safe: a binary
// search over the interval bounds (~log2(0.3·n²) compares).
inline TuningCoefficients tuningCoefficients(const TuningTableView& v, int64_t degree, double g, double P) noexcept {
  TuningCoefficients c;
  const int64_t n = v.n;
  const int64_t N = n * v.periodsPerEquave;
  int64_t q = 0, r = 0;
  tuningFloorDiv(degree, N, q, r);
  const int64_t p = r / n;
  const int64_t j = r - p * n;
  if (v.kind == TuningTableKind::Edo) {
    c.K = static_cast<double>(j);
    c.M = static_cast<double>(-q);
    return c;  // lo = −∞, hi = +∞: an EDO has one interval
  }
  constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
  if (!(std::isfinite(g) && std::isfinite(P) && P > 0.0)) {
    c.lo = c.hi = kNaN;
    return c;
  }
  // The same reduction as MosLinearTable::lookup.
  const double x = g / P;
  const double s = std::floor(x);
  double x0 = x - s;
  if (x0 >= 1.0) x0 = 0.0;
  int32_t lo = 0, hi = v.intervals;  // last t with bounds[t] ≤ x0
  while (hi - lo > 1) {
    const int32_t mid = (lo + hi) / 2;
    if (v.bounds[mid] <= x0) lo = mid;
    else hi = mid;
  }
  const int64_t t = lo;
  const int64_t K = v.k[t * n + j];
  const int64_t M = static_cast<int64_t>(v.m[t * n + j]) + K * static_cast<int64_t>(s) - p - q * v.periodsPerEquave;
  c.K = static_cast<double>(K);
  c.M = static_cast<double>(M);
  c.lo = s + v.bounds[t];
  c.hi = s + v.bounds[t + 1];
  return c;
}

// Stretched cents of absolute degree `degree` (relative to the reference) at generator g, period P, stretch st, in
// double. RT-safe. (The kernel's per-sample path computes the same expression in AVX2 doubles.)
inline double tuningCents(const TuningTableView& v, int64_t degree, double g, double P, double st) noexcept {
  if (v.kind == TuningTableKind::Edo) g = P / static_cast<double>(v.n);
  const TuningCoefficients c = tuningCoefficients(v, degree, g, P);
  const double pure = c.K * g - c.M * P;
  return pure * (1200.0 + st) / 1200.0;
}

// Owner of a table (built off the audio thread; the view stays valid while it lives).
class TuningTable {
public:
  TuningTable() = default;
  // n ≥ 1, periodsPerEquave ≥ 1, bounds = intervals + 1 ascending values from 0 to 1, k and m = intervals × n
  // (EDO: one interval, k[j] = j, m = 0). Errors: InvalidArgument.
  static Result<TuningTable> build(TuningTableKind kind, int32_t n, int32_t periodsPerEquave,
                                   std::span<const double> bounds, std::span<const int32_t> k,
                                   std::span<const int32_t> m, double generator, double period, double stretch,
                                   double referenceHz);
  const TuningTableView& view() const noexcept { return view_; }  // RT-safe

private:
  AlignedBuffer<double> bounds_;
  AlignedBuffer<int32_t> k_;
  AlignedBuffer<int32_t> m_;
  TuningTableView view_{};
};

}  // namespace pq::dsp
