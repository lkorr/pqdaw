#include "pq/tuning/rational.h"

#include <cmath>

namespace pq::tuning {

double cents(const Rational& r) noexcept {  // RT-safe
  assert(r.isPositive());
  // log2 of each part separately: converting n/d to double first would lose precision once n or d exceed 2^53.
  return 1200.0 * (std::log2(static_cast<double>(r.num())) - std::log2(static_cast<double>(r.den())));
}

}  // namespace pq::tuning
