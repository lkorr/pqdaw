#pragma once

// Musical time: ticks (int64, PPQ 27720), tuplet arithmetic, a tempo map with linear ramps, and time signatures.
//
// Positions are int64 ticks or int64 samples (SPEC §3.1). Seconds appear only as a double intermediate that is
// never accumulated across conversions: each tempo segment stores its start time (computed once, with
// compensated summation, when the map is built), and a lookup evaluates one closed-form expression relative to
// that start, so error does not grow with position. Over a 10-hour session at 192 kHz the total numeric error
// is ~1e-5 samples, so ticksToSamples() returns the nearest sample (within ½ sample of the exact time, tested).
//
// Maps are immutable once built. Building (create) allocates and is NOT RT-safe; build off the audio thread and
// hand the map over. All const queries are RT-safe: no allocation, O(log segments) binary search, bounded work.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "pq/core/result.h"

namespace pq {

using Ticks = int64_t;       // musical position; kPpq per quarter note
using SampleTime = int64_t;  // position in samples at some sample rate

// 27720 = lcm(1..11) = 2³·3²·5·7·11: every n-tuplet with n ≤ 11 (and products of 3, 5, 7, 11) of a quarter
// lands on an integer tick.
inline constexpr Ticks kPpq = 27720;
inline constexpr Ticks kTicksPerWhole = 4 * kPpq;  // 110880

// Note value 1/denominator of a whole note (4 → quarter). Exact iff kTicksPerWhole % denominator == 0:
// true for 1, 2, 4, 8, 16, 32 and for 3, 5, 6, 7, 9, 10, 11, 12, …; false for 64 (1/64 = 1732.5 ticks).
constexpr bool noteValueIsExact(int64_t denominator) noexcept {  // RT-safe
  return denominator > 0 && kTicksPerWhole % denominator == 0;
}
// Rounded to the nearest tick when not exact.
constexpr Ticks noteValueTicks(int64_t denominator) noexcept {  // RT-safe
  return (2 * kTicksPerWhole + denominator) / (2 * denominator);
}

// An n-tuplet puts n notes in the time of m straight ones, m = the largest power of two ≤ n
// (3:2, 5:4, 6:4, 7:4, 9:8, 11:8; powers of two are straight). Matches SPEC §9.2: a 7-tuplet at 1/16 is 7 notes
// per quarter.
constexpr int64_t tupletBase(int64_t n) noexcept {  // RT-safe
  return n <= 1 ? 1 : static_cast<int64_t>(std::bit_floor(static_cast<uint64_t>(n)));
}
// Span of one n-tuplet group of 1/denominator notes: tupletBase(n) straight notes, computed from the whole note
// so it stays exact even where the straight note is not (11:8 of 1/64 = 1/8 = 13860 although 1/64 = 1732.5).
// Rounded to the nearest tick when not exact.
constexpr Ticks tupletGroupTicks(int64_t denominator, int64_t n) noexcept {  // RT-safe
  return (2 * kTicksPerWhole * tupletBase(n) + denominator) / (2 * denominator);
}
// Whether all n notes of a group spanning `span` ticks land on integer ticks.
constexpr bool tupletIsExact(Ticks span, int64_t n) noexcept { return n > 0 && span % n == 0; }  // RT-safe
// Position of note k (0-based, any sign) of an n-tuplet group of length `span` starting at `start`:
// start + k·span/n, exact when tupletIsExact(span, n), otherwise rounded to the nearest tick (ties up) and
// computed from k directly, so rounding never accumulates (SPEC §9.2: primes ≥ 13 work with sub-tick rounding).
constexpr Ticks tupletPosition(Ticks start, Ticks span, int64_t n, int64_t k) noexcept {  // RT-safe
  const int64_t num = 2 * k * span + n;
  const int64_t den = 2 * n;
  const int64_t q = num / den;
  return start + ((num % den != 0 && num < 0) ? q - 1 : q);  // floor division
}

// ---- Tempo map --------------------------------------------------------------------------------------------------

inline constexpr double kMinBpm = 1.0;
inline constexpr double kMaxBpm = 1000.0;

// How tempo moves from a point to the next one.
// Linear ramps are linear in BPM over *ticks* (musical time), like tempo automation drawn on a beat-based
// arrangement (Ableton-style): bpm(τ) = b0 + (b1 − b0)·τ/L. Seconds are then the closed-form integral
// s(τ) = 60/(PPQ·b0) · τ · log1p(z)/z with z = (b1 − b0)·τ/(L·b0), inverted with expm1; both are accurate
// to a few ulps including z → 0. (Linear-in-seconds ramps would need a quadratic and would look curved on the
// beat grid; see docs/QUESTIONS.md.)
enum class TempoCurve : uint8_t {
  Step,    // constant until the next point
  Linear,  // ramp linearly (in ticks) to the next point's BPM
};

struct TempoPoint {
  Ticks tick = 0;
  double bpm = 120.0;
  TempoCurve curve = TempoCurve::Step;  // ignored on the last point (tempo stays constant after it)
};

class TempoMap {
public:
  TempoMap();  // constant 120 BPM

  // Not RT-safe (allocates). Points: first at tick 0, strictly increasing ticks, kMinBpm ≤ bpm ≤ kMaxBpm.
  static Result<TempoMap> create(std::span<const TempoPoint> points);

  // ---- RT-safe queries. Valid for |ticks| ≤ 2^53 and |seconds| below the corresponding time. ----
  // Before tick 0 the tempo of the first point continues unchanged (count-in, pre-roll).
  double bpmAt(Ticks t) const noexcept;                   // RT-safe
  double ticksToSeconds(Ticks t) const noexcept;          // RT-safe
  double secondsToTicks(double seconds) const noexcept;   // RT-safe; fractional ticks (inverse of the above)

  // Nearest sample to the exact time of tick t (ties round up).
  SampleTime ticksToSamples(Ticks t, double sampleRate) const noexcept;  // RT-safe
  double ticksToSamplesF(Ticks t, double sampleRate) const noexcept;     // RT-safe; unrounded
  double samplesToTicks(SampleTime s, double sampleRate) const noexcept; // RT-safe; fractional ticks
  // Smallest tick t with ticksToSamples(t) ≥ s. Exactly consistent with ticksToSamples (the analytic inverse is
  // corrected by at most a couple of forward evaluations), so the ticks that play in block [s0, s1) are exactly
  // [firstTickAtOrAfterSample(s0), firstTickAtOrAfterSample(s1)).
  Ticks firstTickAtOrAfterSample(SampleTime s, double sampleRate) const noexcept;  // RT-safe

  std::size_t segmentCount() const noexcept { return startTick_.size(); }

private:
  std::size_t segmentForTick(Ticks t) const noexcept;
  std::size_t segmentForSeconds(double s) const noexcept;
  double localSeconds(std::size_t i, double tau) const noexcept;

  // Struct-of-arrays, one entry per segment (= per point). Segment i covers [startTick_[i], startTick_[i+1]).
  std::vector<Ticks> startTick_;
  std::vector<double> startSec_;    // exact start time of the segment (compensated sum over previous segments)
  std::vector<double> secPerTick_;  // at the segment start: 60 / (PPQ · b0)
  std::vector<double> rampK_;       // (b1 − b0) / (L · b0) per tick; 0 for constant segments
  std::vector<double> bpm0_;        // BPM at segment start
};

// ---- Time signatures --------------------------------------------------------------------------------------------

struct TimeSignature {
  int32_t numerator = 4;
  int32_t denominator = 4;  // note value of one beat; must satisfy noteValueIsExact (so 64 is rejected)
  friend bool operator==(const TimeSignature&, const TimeSignature&) = default;
};

constexpr Ticks ticksPerBeat(TimeSignature sig) noexcept { return kTicksPerWhole / sig.denominator; }  // RT-safe
constexpr Ticks ticksPerBar(TimeSignature sig) noexcept { return ticksPerBeat(sig) * sig.numerator; }  // RT-safe

// Signature changes happen on bar boundaries, identified by bar index (0-based; the UI shows bar + 1).
struct TimeSigChange {
  int64_t bar = 0;
  TimeSignature sig;
};

// A position as bar / beat within the bar / tick within the beat (all 0-based, tick < ticksPerBeat).
struct BarBeatTick {
  int64_t bar = 0;
  int64_t beat = 0;
  Ticks tick = 0;
  friend bool operator==(const BarBeatTick&, const BarBeatTick&) = default;
};

class TimeSigMap {
public:
  TimeSigMap();  // 4/4 throughout

  // Not RT-safe (allocates). Changes: first at bar 0, strictly increasing bars, 1 ≤ numerator ≤ 1024,
  // noteValueIsExact(denominator) (any such denominator, so 7/12-style meters are allowed).
  static Result<TimeSigMap> create(std::span<const TimeSigChange> changes);

  // ---- RT-safe queries. Before bar 0 the first signature continues backwards. ----
  TimeSignature signatureAtBar(int64_t bar) const noexcept;  // RT-safe
  TimeSignature signatureAt(Ticks t) const noexcept;         // RT-safe
  Ticks barToTicks(int64_t bar) const noexcept;              // RT-safe; start of bar
  BarBeatTick ticksToBarBeat(Ticks t) const noexcept;        // RT-safe
  // Inverse of ticksToBarBeat; beat and tick are not normalised (beat ≥ numerator runs on linearly).
  Ticks barBeatToTicks(const BarBeatTick& p) const noexcept; // RT-safe

  std::size_t changeCount() const noexcept { return bar_.size(); }

private:
  std::size_t changeForBar(int64_t bar) const noexcept;
  std::size_t changeForTick(Ticks t) const noexcept;

  std::vector<int64_t> bar_;
  std::vector<Ticks> startTick_;
  std::vector<TimeSignature> sig_;
};

}  // namespace pq
