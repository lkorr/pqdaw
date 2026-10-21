#pragma once

// A note's pitch identity (SPEC §3.1): the rule that decides its Hz and how it reacts to tuning changes.
//
//   Degree   (degree, equave) in the clip's tuning        moves with the tuning
//   Ratio    exact n/d relative to an anchor               stays at true pitch
//   Cents    absolute cents from the tuning's reference    never moves
//   Harmonic (jiObjectId, voiceIndex)                      follows the JI object (SPEC §5)
//
// Layout: 24 bytes, 8-byte aligned, trivially copyable, so arrays of notes stay dense and copying is a memcpy.
//   [ 0, 8)  head: ratio numerator | cents (double) | {degree, equave} | {jiObject, voiceIndex}
//   [ 8,16)  ratio denominator (0 for other kinds)
//   [16,20)  anchor id (Ratio only)
//   [20]     kind, [21] anchor kind, [22,24) zero padding
// A Ratio is the largest payload (two int64 plus an anchor); putting the tag bytes in what would otherwise be padding
// keeps the struct at 24 instead of 32 bytes. The ratio is stored reduced with a positive numerator and denominator
// (see Rational), so == compares canonical forms.
//
// Ids (anchor note, JI object) are uint32 for now; they become the core stable-ID type when that exists (ARCHITECTURE
// §1, core). Their meaning belongs to the document model; resolution only sees the Hz the caller looked up for them.

#include <cstdint>
#include <cassert>
#include <type_traits>
#include <optional>

#include "pq/tuning/rational.h"

namespace pq::tuning {

enum class PitchKind : uint8_t { Degree, Ratio, Cents, Harmonic };

// What a Ratio identity is relative to (SPEC §3.1).
enum class AnchorKind : uint8_t {
  Reference,      // the tuning's reference pitch (clip reference); anchorId unused
  Note,           // another note; anchorId = its note id, Hz supplied in ResolveContext::anchorHz
  JiFundamental,  // a JI object's fundamental; anchorId = JI object id, Hz supplied in ResolveContext::anchorHz
};

class PitchId {
public:
  // Default: degree 0 of equave 0 (the reference pitch in an EDO).
  constexpr PitchId() noexcept = default;

  static constexpr PitchId fromDegree(int32_t degree, int32_t equave = 0) noexcept {
    PitchId p;
    p.kind_ = PitchKind::Degree;
    p.head_.degree = {degree, equave};
    return p;
  }

  // Precondition: r > 0. Use tryFromRatio for unchecked input.
  static PitchId fromRatio(const Rational& r, AnchorKind anchor = AnchorKind::Reference,
                           uint32_t anchorId = 0) noexcept {
    assert(r.isPositive());
    PitchId p;
    p.kind_ = PitchKind::Ratio;
    p.head_.num = r.num();
    p.den_ = r.den();
    p.anchorKind_ = anchor;
    p.anchorId_ = anchorId;
    return p;
  }

  // nullopt unless r > 0. RT-safe.
  static std::optional<PitchId> tryFromRatio(const Rational& r, AnchorKind anchor = AnchorKind::Reference,
                                             uint32_t anchorId = 0) noexcept {
    if (!r.isPositive()) return std::nullopt;
    return fromRatio(r, anchor, anchorId);
  }

  static constexpr PitchId fromCents(double centsFromReference) noexcept {
    PitchId p;
    p.kind_ = PitchKind::Cents;
    p.head_.cents = centsFromReference;
    return p;
  }

  static constexpr PitchId fromHarmonic(uint32_t jiObject, uint32_t voiceIndex) noexcept {
    PitchId p;
    p.kind_ = PitchKind::Harmonic;
    p.head_.harmonic = {jiObject, voiceIndex};
    return p;
  }

  constexpr PitchKind kind() const noexcept { return kind_; }

  // Accessors: valid only for the matching kind (asserted in dev builds).
  constexpr int32_t degree() const noexcept { assert(kind_ == PitchKind::Degree); return head_.degree.degree; }
  constexpr int32_t equave() const noexcept { assert(kind_ == PitchKind::Degree); return head_.degree.equave; }
  constexpr Rational ratio() const noexcept {
    assert(kind_ == PitchKind::Ratio);
    return Rational::fromReduced(head_.num, den_);
  }
  constexpr AnchorKind anchorKind() const noexcept { assert(kind_ == PitchKind::Ratio); return anchorKind_; }
  constexpr uint32_t anchorId() const noexcept { assert(kind_ == PitchKind::Ratio); return anchorId_; }
  constexpr double cents() const noexcept { assert(kind_ == PitchKind::Cents); return head_.cents; }
  constexpr uint32_t jiObject() const noexcept { assert(kind_ == PitchKind::Harmonic); return head_.harmonic.jiObject; }
  constexpr uint32_t voiceIndex() const noexcept {
    assert(kind_ == PitchKind::Harmonic);
    return head_.harmonic.voiceIndex;
  }

  // Raw parts for the resolver's hot path (no asserts). RT-safe.
  constexpr int64_t rawNum() const noexcept { return head_.num; }
  constexpr int64_t rawDen() const noexcept { return den_; }

  friend constexpr bool operator==(const PitchId& a, const PitchId& b) noexcept {
    if (a.kind_ != b.kind_) return false;
    switch (a.kind_) {
      case PitchKind::Degree:
        return a.head_.degree.degree == b.head_.degree.degree && a.head_.degree.equave == b.head_.degree.equave;
      case PitchKind::Ratio:
        return a.head_.num == b.head_.num && a.den_ == b.den_ && a.anchorKind_ == b.anchorKind_ &&
               a.anchorId_ == b.anchorId_;
      case PitchKind::Cents:
        return a.head_.cents == b.head_.cents;
      case PitchKind::Harmonic:
        return a.head_.harmonic.jiObject == b.head_.harmonic.jiObject &&
               a.head_.harmonic.voiceIndex == b.head_.harmonic.voiceIndex;
    }
    return false;
  }

private:
  struct DegreeParts {
    int32_t degree;
    int32_t equave;
  };
  struct HarmonicParts {
    uint32_t jiObject;
    uint32_t voiceIndex;
  };
  union Head {
    int64_t num;
    double cents;
    DegreeParts degree;
    HarmonicParts harmonic;
  };

  Head head_{.degree = {0, 0}};
  int64_t den_ = 0;
  uint32_t anchorId_ = 0;
  PitchKind kind_ = PitchKind::Degree;
  AnchorKind anchorKind_ = AnchorKind::Reference;
  uint16_t reserved_ = 0;
};

static_assert(sizeof(PitchId) == 24, "PitchId layout: keep notes dense");
static_assert(alignof(PitchId) == 8);
static_assert(std::is_trivially_copyable_v<PitchId>);

}  // namespace pq::tuning
