#include "pq/tuning/edo.h"

#include <cassert>
#include <cmath>

namespace pq::tuning {

Edo::Edo(uint32_t n, double log2Equave, std::optional<Rational> ratio, double equaveCents) noexcept
    : n_(n),
      hasEquaveRatio_(ratio.has_value()),
      equaveRatio_(ratio.value_or(Rational{})),
      log2Equave_(log2Equave),
      equaveCents_(equaveCents),
      log2Step_(log2Equave / static_cast<double>(n)) {}

Result<Edo> Edo::make(uint32_t n, const Rational& equave) {
  if (n == 0) return Error{ErrorCode::InvalidArgument, "EDO needs at least one step", {}};
  if (equave <= Rational{1}) return Error{ErrorCode::InvalidArgument, "EDO equave must be greater than 1/1", {}};
  return Edo(n, cents(equave) / 1200.0, equave, 1200.0 * (cents(equave) / 1200.0));
}

Result<Edo> Edo::makeCents(uint32_t n, double equaveCents) {
  if (n == 0) return Error{ErrorCode::InvalidArgument, "EDO needs at least one step", {}};
  if (!std::isfinite(equaveCents) || !(equaveCents > 0.0))
    return Error{ErrorCode::InvalidArgument, "EDO equave must be a finite positive number of cents", {}};
  // Keep the given cents exactly (not 1200 · (c / 1200), which can differ in the last bit), so equaveCents()
  // reproduces the input and makeCents(n, e.equaveCents()) == e: project files store it (T3.5).
  return Edo(n, equaveCents / 1200.0, std::nullopt, equaveCents);
}

Edo Edo::standard12() noexcept { return Edo(12, 1.0, Rational{2}, 1200.0); }

Val patentVal(const Edo& edo) noexcept {  // RT-safe
  Val v;
  const double stepsPerLog2 = static_cast<double>(edo.n()) / edo.log2Equave();
  for (int i = 0; i < kValPrimeCount; ++i)
    v.steps[i] = static_cast<int32_t>(std::llround(stepsPerLog2 * std::log2(static_cast<double>(kMonzoPrimes[i]))));
  return v;
}

std::optional<int64_t> mapToSteps(const Val& val, const Monzo& m) noexcept {  // RT-safe
  for (int i = kValPrimeCount; i < kMonzoPrimeCount; ++i)
    if (m.exps[i] != 0) return std::nullopt;
  int64_t steps = 0;  // each term < 2^62 in magnitude and there are 11: fits in int64
  for (int i = 0; i < kValPrimeCount; ++i) steps += static_cast<int64_t>(val.steps[i]) * m.exps[i];
  return steps;
}

EdoRow edoRow(int64_t step, uint32_t n) noexcept {  // RT-safe
  assert(n > 0);
  const int64_t nn = n;
  int64_t q = step / nn;
  int64_t r = step % nn;
  if (r < 0) {
    r += nn;
    --q;
  }
  return {q, static_cast<uint32_t>(r)};
}

std::string stepLabel(int64_t k, uint32_t n) { return std::to_string(k) + "\\" + std::to_string(n); }

}  // namespace pq::tuning
