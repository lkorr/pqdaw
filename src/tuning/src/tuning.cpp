#include "pq/tuning/tuning.h"

#include <cfloat>
#include <cmath>

namespace pq::tuning {

Result<Tuning> Tuning::make(const System& system, double referenceHz) {
  if (!std::isfinite(referenceHz) || !(referenceHz > 0.0))
    return Error{ErrorCode::InvalidArgument, "reference pitch must be a finite positive frequency", {}};
  return Tuning(system, referenceHz);
}

Result<Tuning> makeScalaTuning(const Scale& scale, const KeyboardMapping& kbm) {
  auto bad = [](const char* why) { return Error{ErrorCode::InvalidArgument, why, {}}; };
  const std::optional<int64_t> refDegree = mapKeyToDegree(kbm, kbm.referenceKey);
  if (!refDegree) return bad("the keyboard mapping's reference key is unmapped or outside the retuned range");
  if (!std::isfinite(kbm.referenceHz) || !(kbm.referenceHz > 0.0))
    return bad("the keyboard mapping's reference frequency must be a finite positive number");
  // Hz(reference key) = referenceHz, and the reference key plays absolute degree refDegree, so degree 0 sits
  // centsOfDegree(refDegree) below it.
  const double degree0Hz = kbm.referenceHz * std::exp2(-scale.centsOfDegree(*refDegree) * (1.0 / 1200.0));
  return Tuning::make(scale, degree0Hz);
}

std::optional<double> resolveHzDouble(const PitchId& pitch, const Tuning& tuning,
                                      const ResolveContext& ctx) noexcept {  // RT-safe
  double hz = 0.0;
  switch (pitch.kind()) {
    case PitchKind::Degree: {
      if (const Edo* edo = tuning.edo()) {
        const int64_t step = static_cast<int64_t>(pitch.equave()) * static_cast<int64_t>(edo->n()) + pitch.degree();
        hz = tuning.referenceHz() * std::exp2(edo->log2OfStep(step));
      } else if (const Mos* mos = tuning.mos()) {
        hz = tuning.referenceHz() * std::exp2(mos->centsOf(pitch.degree(), pitch.equave()) * (1.0 / 1200.0));
      } else if (const Scale* scale = tuning.scale()) {
        hz = tuning.referenceHz() * std::exp2(scale->centsOf(pitch.degree(), pitch.equave()) * (1.0 / 1200.0));
      } else {
        return std::nullopt;
      }
      break;
    }
    case PitchKind::Ratio: {
      const double anchor = pitch.anchorKind() == AnchorKind::Reference ? tuning.referenceHz() : ctx.anchorHz;
      if (!(anchor > 0.0)) return std::nullopt;
      // n and d are exact integers < 2^63; each converts with ≤ 0.5 ulp error, so the ratio is good to ~2e-16.
      hz = anchor * (static_cast<double>(pitch.rawNum()) / static_cast<double>(pitch.rawDen()));
      break;
    }
    case PitchKind::Cents:
      hz = tuning.referenceHz() * std::exp2(pitch.cents() * (1.0 / 1200.0));
      break;
    case PitchKind::Harmonic: {
      const HarmonicFrame& f = ctx.harmonic;
      const uint32_t v = pitch.voiceIndex();
      if (f.harmonics == nullptr || v >= f.count || f.harmonics[v] == 0 || !(f.fundamentalHz > 0.0))
        return std::nullopt;
      const double h = static_cast<double>(f.harmonics[v]);
      hz = f.undertone ? f.fundamentalHz / h : f.fundamentalHz * h;
      break;
    }
  }
  // Must survive the float32 conversion as a finite, positive, normal frequency (also rejects NaN).
  if (!(hz >= static_cast<double>(FLT_MIN) && hz <= static_cast<double>(FLT_MAX))) return std::nullopt;
  return hz;
}

float resolveHz(const PitchId& pitch, const Tuning& tuning, const ResolveContext& ctx) noexcept {  // RT-safe
  const std::optional<double> hz = resolveHzDouble(pitch, tuning, ctx);
  return hz ? static_cast<float>(*hz) : 0.0f;
}

}  // namespace pq::tuning
