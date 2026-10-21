#pragma once

// A tuning context and resolution of pitch identities to Hz (SPEC §3.1, §3.2).
//
// Tuning holds the tuning system plus its reference pitch (default C3 = 261.626 Hz, per project/tuning/clip).
// The system is a std::variant: EDO, MOS (T2.3) and Scale (T2.4, Scala .scl). A Mos or Scale holds its degree table
// on the heap: copy Tunings off the audio thread (resolving only reads). SPEC §3.2's JI { prime limit, reference }
// context is not a variant yet: Ratio identities resolve without it, and its lattice/labelling helpers live in ji.h,
// fewest_keypress.h and interval_names.h (T2.5); it becomes a variant when something needs Degree resolution in JI.
//
// resolveHz does all arithmetic in double and converts to float32 once at the end, so the float32 result is within
// float rounding (≈ 0.0001 ¢) of the exact pitch.

#include <cstdint>
#include <optional>
#include <variant>

#include "pq/core/result.h"
#include "pq/tuning/edo.h"
#include "pq/tuning/mos_tuning.h"
#include "pq/tuning/pitch_id.h"
#include "pq/tuning/scala.h"
#include "pq/tuning/scale_tuning.h"

namespace pq::tuning {

inline constexpr double kDefaultReferenceHz = 261.626;  // C3 in Ableton naming (SPEC §15)

class Tuning {
public:
  using System = std::variant<Edo, Mos, Scale>;

  // 12-EDO at C3 = 261.626 Hz.
  Tuning() noexcept : system_(Edo::standard12()) {}

  // referenceHz must be finite and > 0. Errors: InvalidArgument.
  static Result<Tuning> make(const System& system, double referenceHz = kDefaultReferenceHz);

  const System& system() const noexcept { return system_; }
  double referenceHz() const noexcept { return referenceHz_; }

  // The EDO, or nullptr if this tuning is another kind. RT-safe.
  const Edo* edo() const noexcept { return std::get_if<Edo>(&system_); }
  // The MOS, or nullptr if this tuning is another kind. RT-safe.
  const Mos* mos() const noexcept { return std::get_if<Mos>(&system_); }
  // The Scale, or nullptr if this tuning is another kind. RT-safe.
  const Scale* scale() const noexcept { return std::get_if<Scale>(&system_); }

private:
  Tuning(const System& system, double referenceHz) : system_(system), referenceHz_(referenceHz) {}

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
//            MOS: referenceHz · 2^(Mos::centsOfDegree(equave·size + degree) / 1200)  (stretch included)
//            Scale: referenceHz · 2^(Scale::centsOfDegree(equave·size + degree) / 1200)
//   Ratio    anchorHz · n/d, where anchorHz = referenceHz for AnchorKind::Reference
//   Cents    referenceHz · 2^(cents/1200)
//   Harmonic fundamentalHz · h  (undertone: fundamentalHz / h), h = harmonics[voiceIndex]
// RT-safe: no allocation, no locks, bounded.
std::optional<double> resolveHzDouble(const PitchId& pitch, const Tuning& tuning, const ResolveContext& ctx) noexcept;

// A Scale tuning from a .scl scale and a .kbm mapping: the reference pitch (the Hz of degree 0, which the mapping puts
// on its middle key) is derived from the mapping's reference key and frequency, so that
//   resolveHz(*mapKey(kbm, key, scale.size()), tuning, {}) == the frequency Scala gives `key`,
// e.g. the standard 12-key mapping (middle 60, reference 69 = 440 Hz) on 12-EDO gives key 60 = 261.6256 Hz.
// Errors: InvalidArgument if the reference key is unmapped or outside [firstKey, lastKey] (Scala treats an unmapped
// reference key as an error), or the derived reference pitch is not a finite positive frequency.
Result<Tuning> makeScalaTuning(const Scale& scale, const KeyboardMapping& kbm);

// float32 Hz for the engine; 0.0f if the identity cannot be resolved (see resolveHzDouble). RT-safe.
float resolveHz(const PitchId& pitch, const Tuning& tuning, const ResolveContext& ctx) noexcept;

}  // namespace pq::tuning
