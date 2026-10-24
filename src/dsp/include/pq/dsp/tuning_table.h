#pragma once

// The tuning node's table (T2.6, SPEC §3.3): the per-interval linear form of an EDO or MOS tuning, as the read-only
// resource of the `tuning_hz` kernel (KernelArgs::resource → const TuningTableView*).
//
// The data comes from pq::tuning::linearShape / linearTuningForm (pq/tuning/tuning_signal.h; dsp does not depend on
// tuning, so the owners are built from plain arrays). It splits in two (T2.6-fix4):
//   TuningShape  the interval table of a chain length (kind, n): boundaries, unrotated (k, m), the inverse map
//                (sorted degree of each chain element) and per boundary buildMOSScale's degenerate window and side.
//                Immutable, shared (std::shared_ptr) by every table of that shape.
//   TuningTable  one tuning: its shape plus periods per equave, the root's chain position (the mode), the region
//                hint and the definition's values; the view points into the shape.
// The math, for absolute degree D = equave·N + degree (N = n·periodsPerEquave) split as D = q·N + p·n + j, generator
// g and period P (cents):
//     MOS:  t = the interval holding g (boundaries as buildMOSScale sees them, see below), s its period shift,
//           r = inv[t][rootChain] (the mode in that interval: the explorer's transferModeRotation keeps the root's
//           chain position, T2.6-fix2), a = j + r (− n and w = 1 when ≥ n, else w = 0),
//           K = k[t][a] − k[t][r],   M = m[t][a] − m[t][r] − w + K·s − p − q·periodsPerEquave
//     EDO:  g = P / n (the generator follows the equave), K = j, M = −q      (one interval, never left)
//     pure cents = K·(g + nudge) − M·P,   stretched = pure · (1200 + stretch) / 1200,   Hz = reference · 2^(cents/1200)
// Boundaries (T2.6-fix2, M3 review H3): within window[b] (1e-6/q cents) of boundary b = p/q, buildMOSScale's
// chainSafeGenerator nudges the generator by 1e-7 cents toward the region hint's midpoint (when the hint has n notes
// per period and contains g), else to the boundary's hint-less side; tuningCoefficients makes the same choice and
// returns the nudge, so the form gives buildMOSScale's pitches at exact boundaries too (720 ¢ in 5L2s with the 5L2s
// hint: the interval below). (K, M) stay valid while lo·P + loMargin ≤ g < hi·P − hiMargin (the margins are the
// windows of the interval's ends; inside a window the range is empty, so the kernel re-derives every sample there).
//
// Unmodulated inputs: the node's param-rate inputs are float32 param slots. An input that equals float32(x) of the
// definition's double value x (generator, period, stretch, reference) reads x exactly, so the computed pitch is
// within an ulp of the document's (resolveHz's double result rounded once). When EVERY tuning param input of a lane is
// at its definition value in a block, the kernel passes the note's own `hz` input through instead (T2.6-fix1): the
// project lowering resolves NoteDesc::hz in this table's definition, so an unmodulated tuning sounds bit-identically
// to static pitches. Audio-rate inputs are used as is.
//
// Audio-rate automation (T2.6-fix2, M3 review M6): `live` (optional) points at four per-block buffer pointers the
// audio thread sets before the units run (the Timeline writes an automation lane's buffer there while its block
// moves, null while it is constant: pq/engine/automation.h AutomationLaneDesc::tap). A non-null buffer replaces the
// corresponding param input for that block, per sample: an automated generator crosses a region boundary at the
// exact sample. The buffers are global (one value per sample for every lane).

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>

#include "pq/core/aligned_buffer.h"
#include "pq/core/result.h"

namespace pq::dsp {

enum class TuningTableKind : uint32_t { Edo = 0, Mos = 1 };

// Per-block audio-rate inputs of a tuning (see the file comment). Audio thread only.
struct TuningLiveInputs {
  const float* generator = nullptr;
  const float* period = nullptr;
  const float* stretch = nullptr;
  const float* reference = nullptr;
};

// A region hint (tuning::RegionHint): the selected region's notes per period and its generator range in cents.
struct TuningHint {
  int32_t perPeriodSize = 0;
  double lowCents = 0.0;
  double highCents = 0.0;
};

inline constexpr double kTuningNudge = 1e-7;  // chainSafeGenerator's nudge in cents

struct TuningTableView {
  TuningTableKind kind = TuningTableKind::Edo;
  int32_t n = 12;                 // notes per period (EDO: steps per equave)
  int32_t periodsPerEquave = 1;
  int32_t intervals = 1;
  const double* bounds = nullptr; // intervals + 1 fractions of the period, 0 … 1
  const int32_t* k = nullptr;     // intervals × n, unrotated
  const int32_t* m = nullptr;     // intervals × n, period shift 0
  const int32_t* inv = nullptr;   // intervals × n: inv[t][c] = sorted degree of chain element c
  const int8_t* sides = nullptr;  // intervals + 1: hint-less nudge side (−1, 0, +1)
  const double* windows = nullptr;// intervals + 1: degenerate window in cents (0: none)
  int32_t rootChain = 0;          // chain position of the mode's root (MOS)
  bool hasHint = false;
  TuningHint hint{};
  double generator = 100.0;       // the definition's values (see "Unmodulated inputs")
  double period = 1200.0;
  double stretch = 0.0;
  double referenceHz = 261.626;
  float generatorF = 100.0f;      // float32 of the above
  float periodF = 1200.0f;
  float stretchF = 0.0f;
  float referenceF = 261.626f;
  const TuningLiveInputs* live = nullptr;  // audio-rate automation (see the file comment), or null
};

// One lane's linear coefficients: pure cents = K·(g + nudge) − M·P while lo·P + loMargin ≤ g < hi·P − hiMargin
// (lo = hi = NaN: invalid inputs, never in range; lo = +∞: inside a boundary window, re-derive at every sample).
struct TuningCoefficients {
  double K = 0.0;
  double M = 0.0;
  double lo = -std::numeric_limits<double>::infinity();
  double hi = std::numeric_limits<double>::infinity();
  double loMargin = 0.0;
  double hiMargin = 0.0;
  double nudge = 0.0;
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

// The side buildMOSScale nudges generator g at boundary `b` to (see the file comment). RT-safe.
inline int32_t tuningNudgeSide(const TuningTableView& v, int32_t b, double g) noexcept {
  if (v.hasHint && v.hint.perPeriodSize == v.n && g >= v.hint.lowCents - 1e-6 && g <= v.hint.highCents + 1e-6)
    return (v.hint.lowCents + v.hint.highCents) / 2.0 >= g ? 1 : -1;
  return v.sides[b];
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
  double s = std::floor(x);
  double x0 = x - s;
  if (x0 >= 1.0) {
    x0 = 0.0;
    s += 1.0;
  }
  const int32_t last = v.intervals - 1;
  int32_t t = 0, hiI = v.intervals;  // last t with bounds[t] ≤ x0
  while (hiI - t > 1) {
    const int32_t mid = (t + hiI) / 2;
    if (v.bounds[mid] <= x0) t = mid;
    else hiI = mid;
  }
  // A degenerate generator: buildMOSScale's nudge decides the side.
  bool inWindow = false;
  const double cLo = (s + v.bounds[t]) * P, cHi = (s + v.bounds[t + 1]) * P;
  if (std::fabs(g - cLo) < v.windows[t]) {
    inWindow = true;
    c.nudge = tuningNudgeSide(v, t, g) * kTuningNudge;
    if (g + c.nudge < cLo) {
      if (t == 0) {
        t = last;
        s -= 1.0;
      } else {
        --t;
      }
    }
  } else if (std::fabs(cHi - g) < v.windows[t + 1]) {
    inWindow = true;
    c.nudge = tuningNudgeSide(v, t + 1, g) * kTuningNudge;
    if (g + c.nudge >= cHi) {
      if (t == last) {
        t = 0;
        s += 1.0;
      } else {
        ++t;
      }
    }
  }
  const int64_t base = static_cast<int64_t>(t) * n;
  const int64_t rot = v.inv[base + v.rootChain];
  int64_t a = j + rot;
  int64_t w = 0;
  if (a >= n) {
    a -= n;
    w = 1;
  }
  const int64_t K = static_cast<int64_t>(v.k[base + a]) - v.k[base + rot];
  const int64_t M = static_cast<int64_t>(v.m[base + a]) - v.m[base + rot] - w + K * static_cast<int64_t>(s) - p -
                    q * v.periodsPerEquave;
  c.K = static_cast<double>(K);
  c.M = static_cast<double>(M);
  if (inWindow) {
    c.lo = std::numeric_limits<double>::infinity();  // empty: re-derive while near the boundary
    c.hi = -std::numeric_limits<double>::infinity();
  } else {
    c.lo = s + v.bounds[t];
    c.hi = s + v.bounds[t + 1];
    c.loMargin = v.windows[t];
    c.hiMargin = v.windows[t + 1];
  }
  return c;
}

// Stretched cents of absolute degree `degree` (relative to the reference) at generator g, period P, stretch st, in
// double. RT-safe. (The kernel's per-sample path computes the same expression in AVX2 doubles.)
inline double tuningCents(const TuningTableView& v, int64_t degree, double g, double P, double st) noexcept {
  if (v.kind == TuningTableKind::Edo) g = P / static_cast<double>(v.n);
  const TuningCoefficients c = tuningCoefficients(v, degree, g, P);
  const double pure = c.K * (g + c.nudge) - c.M * P;
  return pure * (1200.0 + st) / 1200.0;
}

// The interval table of one shape (see the file comment). Built off the audio thread, then immutable.
class TuningShape {
public:
  // n ≥ 1; bounds = intervals + 1 ascending values from 0 to 1 (EDO: one interval); k, m, inverse = intervals × n
  // (inverse[t][k[t][j]] = j); sides, windows = intervals + 1. Errors: InvalidArgument.
  static Result<std::shared_ptr<const TuningShape>> build(TuningTableKind kind, int32_t n,
                                                          std::span<const double> bounds,
                                                          std::span<const int32_t> k, std::span<const int32_t> m,
                                                          std::span<const int32_t> inverse,
                                                          std::span<const int8_t> sides,
                                                          std::span<const double> windows);
  TuningTableKind kind() const noexcept { return kind_; }
  int32_t n() const noexcept { return n_; }
  int32_t intervals() const noexcept { return intervals_; }
  // Bytes held (the M3 review's M8: tables reach 0.5 MiB at n = 60).
  size_t bytes() const noexcept;

private:
  friend class TuningTable;
  TuningTableKind kind_ = TuningTableKind::Edo;
  int32_t n_ = 0;
  int32_t intervals_ = 0;
  AlignedBuffer<double> bounds_;
  AlignedBuffer<int32_t> k_, m_, inv_;
  AlignedBuffer<int8_t> sides_;
  AlignedBuffer<double> windows_;
};

// The definition part of a table (see the file comment).
struct TuningDefinition {
  int32_t periodsPerEquave = 1;
  int32_t rootChain = 0;
  bool hasHint = false;
  TuningHint hint{};
  double generator = 100.0, period = 1200.0, stretch = 0.0, referenceHz = 261.626;
};

// One tuning's table: a shape plus a definition (the view stays valid while the table lives; moving keeps it valid).
class TuningTable {
public:
  TuningTable() = default;
  // Errors (InvalidArgument): no shape, periods per equave < 1, a root chain position outside [0, n), invalid
  // generator / period / stretch / reference.
  static Result<TuningTable> build(std::shared_ptr<const TuningShape> shape, const TuningDefinition& definition,
                                   const TuningLiveInputs* live = nullptr);
  const TuningTableView& view() const noexcept { return view_; }  // RT-safe
  const std::shared_ptr<const TuningShape>& shape() const noexcept { return shape_; }

private:
  std::shared_ptr<const TuningShape> shape_;
  TuningTableView view_{};
};

}  // namespace pq::dsp
