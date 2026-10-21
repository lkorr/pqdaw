#pragma once

// A tuning context and resolution of pitch identities to Hz (SPEC §3.1, §3.2).
//
// Tuning holds the tuning system plus its reference pitch (default C3 = 261.626 Hz, per project/tuning/clip).
// The system is a std::variant so the remaining kinds slot in as their tasks land: MOS (T2.3), Scale (T2.4),
// JI context (T2.5). Only EDO exists today.
//
// resolveHz does all arithmetic in double and converts to float32 once at the end, so the float32 result is within
// float rounding (≈ 0.0001 ¢) of the exact pitch.

#include <cstdint>
#include <optional>
#include <variant>

#include "pq/core/result.h"
#include "pq/tuning/edo.h"
#include "pq/tuning/pitch_id.h"

namespace pq::tuning {

inline constexpr double kDefaultReferenceHz = 261.626;  // C3 in Ableton naming (SPEC §15)

class Tuning {
public:
  using System = std::variant<Edo>;

  // 12-EDO at C3 = 261.626 Hz.
  Tuning() noexcept : system_(Edo::standard12()) {}

  // referenceHz must be finite and > 0. Errors: InvalidArgument.
  static Result<Tuning> make(const System& system, double referenceHz = kDefaultReferenceHz);

  const System& system() const noexcept { return system_; }
  double referenceHz() const noexcept { return referenceHz_; }

  // The EDO, or nullptr if this tuning is another kind. RT-safe.
  const Edo* edo() const noexcept { return std::get_if<Edo>(&system_); }

private:
  Tuning(const System& system, double referenceHz) noexcept : system_(system), referenceHz_(referenceHz) {}

  System system_;
  double referenceHz_ = kDefaultReferenceHz;
};

// The harmonic numbers of one JI object's voices (SPEC §5), as needed to resolve Harmonic identities.
// Voice i sounds at fundamentalHz · harmonics[i], or fundamentalHz / harmonics[i] for an undertone object
// (SPEC §15: undertone JI objects are a flag on the same object type). The array is owned by the caller.
struct HarmonicFrame {
  double fundamentalHz = 0.0;
  const uint32_t* harmonics = nullptr;
  uint32_t count = 0;
  bool undertone = false;
};

// Values that live outside the note and its tuning. The caller (document/engine) fills in whatever the identity
// refers to: the resolved Hz of a Ratio's anchor note or JI fundamental, and the frame of a Harmonic's JI object.
struct ResolveContext {
  double anchorHz = 0.0;  // for Ratio with AnchorKind::Note / JiFundamental
  HarmonicFrame harmonic{};
};

// Pitch in Hz as double, or nullopt if it cannot be resolved: a Ratio anchor or Harmonic frame missing from the
// context, a voice index out of range, harmonic number 0, or a result that is not a finite positive float32.
// Rules:
//   Degree   EDO: referenceHz · 2^((equave·n + degree) · log2(equave ratio) / n)
//   Ratio    anchorHz · n/d, where anchorHz = referenceHz for AnchorKind::Reference
//   Cents    referenceHz · 2^(cents/1200)
//   Harmonic fundamentalHz · h  (undertone: fundamentalHz / h), h = harmonics[voiceIndex]
// RT-safe: no allocation, no locks, bounded.
std::optional<double> resolveHzDouble(const PitchId& pitch, const Tuning& tuning, const ResolveContext& ctx) noexcept;

// float32 Hz for the engine; 0.0f if the identity cannot be resolved (see resolveHzDouble). RT-safe.
float resolveHz(const PitchId& pitch, const Tuning& tuning, const ResolveContext& ctx) noexcept;

}  // namespace pq::tuning
