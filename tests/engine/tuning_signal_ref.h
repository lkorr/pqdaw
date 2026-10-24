#pragma once

// Tests only (T2.6): the reference for tuning as a signal — the pitch buildMOSScale gives a degree at a generator
// (Mos::make → buildFullScale, Mos::centsOf), and the tolerance check against a float32 Hz output.

#include <cmath>
#include <cstdint>
#include <optional>

#include "pq/tuning/mos.h"
#include "pq/tuning/mos_tuning.h"
#include "pq/tuning/tuning.h"

namespace tuning_ref {

// Stretched cents of degree `degree` of equave `equave` for the MOS `p` at generator g (buildMOSScale per call), or
// nullopt where buildMOSScale nudges a degenerate generator (a chain note within 1e-6 ¢ of the period: exactly at
// an interval boundary, where the table takes the interval above and the JS a side of its choosing).
inline std::optional<pq::tuning::Mos> mosAt(pq::tuning::MosParams p, double g) {
  if (pq::tuning::chainSafeGenerator(g, p.n, p.period) != g) return std::nullopt;
  p.generator = g;
  auto m = pq::tuning::Mos::make(std::move(p));
  if (!m.ok()) return std::nullopt;
  return std::move(m.value());
}
inline std::optional<double> mosCents(const pq::tuning::MosParams& p, double g, int32_t degree, int32_t equave) {
  const auto m = mosAt(p, g);
  if (!m) return std::nullopt;
  return m->centsOf(degree, equave);
}

// The criterion: the float32 output is the exact pitch within 1e-4 ¢ plus the float32 rounding of the Hz value
// (half an ulp; the Hz signal is float32 by SPEC §3.1, whose own resolution is ≈ 1e-4 ¢).
inline bool withinTolerance(float out, double refHz, double cents = 1e-4) {
  const double halfUlp = 0.5 * std::ldexp(1.0, std::ilogb(refHz) - 23);
  const double tol = halfUlp + refHz * (std::exp2(cents / 1200.0) - 1.0);
  return std::fabs(static_cast<double>(out) - refHz) <= tol;
}

// Cents between a float32 output and the exact Hz (for reporting).
inline double centsError(float out, double refHz) { return std::fabs(1200.0 * std::log2(static_cast<double>(out) / refHz)); }

}  // namespace tuning_ref
