#pragma once

// Control curves (GU.1; docs/plans/GU.md "Curves", SPEC §15 2026-11-06 "built GUIs"): how a control's KNOB POSITION
// t ∈ [0, 1] maps to its value. ONE definition, used by the UI (faces, readouts, typed values), the document's lowering
// (an undriven control's value, written as its runtime param) and the native `curve` atom (a driven control: wires,
// automation, macros), so face, readout and sound follow the same law.
//
// ---- The model ----------------------------------------------------------------------------------------------------
// A curve has a range [min, max] (in its own units: Hz, seconds, dB for a gain, a fraction for %), a display unit and
// digits (the readout), a step (> 0: the value is rounded to multiples of it, a @steps param's integers), 1 … 16
// SEGMENTS and three flags:
//   points     points[0 … n]: (t, y), t strictly increasing from exactly 0 to exactly 1, y ∈ [0, 1] the share of the
//              range (value = min + y·(max − min)); segment i runs from points[i] to points[i + 1]
//   segments   segments[0 … n − 1]: a SHAPE and a BEND (continuous; 0 = straight for every shape):
//                Linear   straight (the bend is ignored)
//                Exp      (e^(k·u) − 1)/(e^k − 1), k = bend: fine at the start for k > 0 (an octave law: k = ln(max/min)),
//                         at the end for k < 0 (the "logarithmic" preset)
//                Log      the inverse of Exp: ln(1 + (e^k − 1)·u)/k
//                Power    u^q, q = 2^bend
//                SCurve   the two halves of Exp(k) mirrored about the middle (k > 0: flat ends, steep middle)
//                Joined   (GU.4) an exponential with a straight part: its FLAT end continues the neighbour there without
//                         a kink (C1). k > 0: flat at the start, joined to the previous segment; k < 0: flat at the end,
//                         joined to the next. From the flat end (u from there, K = |k|, span the segment's rise, A the
//                         neighbour's slope there × the width, clamped to [0, span]): h(u) = α·u + β·(e^(K·u) − 1),
//                         β = (span − A)/(e^K − 1 − K), α = A − β·K, so h(0) = 0, h'(0) = A, h(1) = span. Without a
//                         neighbour there (or a Joined neighbour whose flat end meets this one) A = 0. The VCA's law:
//                         log ends joined to its fine ±1 zone (CurvePreset::Vca).
//              where u ∈ [0, 1] is the position inside the segment.
//   wrap       the knob drags around its range and a wire's sum wraps: the position is frac(t) (a phase)
//   signed     mirrored for negative positions: value(−t) = −value(t), so a wire can push the value below zero (the
//              Phase accumulator's frequency); the knob itself stays in [0, 1]
//   output     a fixed conversion after the curve (and the step): DbToGain (the curve is in dB, the DSP takes a gain)
// Curves need not be monotonic (each segment is; the curve may go up and back down).
//
// ---- Evaluation ---------------------------------------------------------------------------------------------------
// value(t) = output(step(sign · seg_i(p))), with p = clamp(t, 0, 1) (wrap: frac(t); signed: min(|t|, 1) and sign =
// sign(t)), i the last segment whose start t0 ≤ p, x = p − t0, and seg_i the shape in its coefficient form
// (segmentCoeffs below; Y0 the segment's first value, w its width, dY its rise, k its bend):
//   Linear  Y0 + (dY / w)·x                        Exp     Y0 + dY / expm1(k) · expm1(k / w · x)
//   Log     Y0 + dY / k · log1p(expm1(k) / w · x)  Power   Y0 + dY·clamp(x / w, 0, 1)^q
//   SCurve  Y0 + dY·g, u = clamp(x / w, 0, 1), h = expm1(2k·min(u, 1 − u)) / (2·expm1(k)), g = u < 0.5 ? h : 1 − h
//   Joined  a + e·x' + b·expm1(c·x'), x' = x − f (measured from the flat end: f = 0 for k > 0, f = w for k < 0, so the
//           value near the join, where the exponential term is small, keeps its precision)
// evalCurve does this in double (the UI, the lowering); the atom (pq/dsp/atoms.h `curve`) in float with the same
// coefficients rounded to float and the exact transcendental family (simd.h: SVML, 1 lane == 8 lanes), so the two agree
// to float rounding (tests/dsp/test_curve.cpp bounds it).
//
// ---- The inverse ----------------------------------------------------------------------------------------------------
// curvePosition(curve, value, hint): the knob position that gives `value` (typed values, presets, defaults, migrated
// files): each segment is inverted analytically; of the positions that reach the value the one nearest `hint` (the
// knob's current position) is chosen — the hint itself when it gives exactly `value` (so writing back the value a knob
// shows never moves it); a value the curve never reaches snaps to the reachable value nearest it (ties: nearest `hint`). The knob's own domain [0, 1] (a signed curve's negative half is the wires', never the knob's).
//
// ---- Classes ------------------------------------------------------------------------------------------------------
// curveClass: the structural part of a curve (segment count, which shapes, the flags, whether it steps): what a change
// of recompiles (doc lowering); everything else (points, bends, the range) is runtime data of the atom's slot
// (CurveSlot below), swapped atomically, so dragging a point never recompiles.
//
// Not RT code, except CurveSlot::acquire and the atom's kernel (RT-safe).

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string_view>

namespace pq {
class EpochReclaimer;
}

namespace pq::dsp {

inline constexpr uint32_t kMaxCurveSegments = 16;

enum class CurveShape : uint8_t { Linear, Exp, Log, Power, SCurve, Joined };
inline constexpr uint32_t kCurveShapeCount = 6;
enum class CurveOutput : uint8_t { None, DbToGain };
enum class CurveMode : uint8_t { Clip, Wrap, Signed };
// The readout's unit (the curve's own units; a DbToGain curve reads in dB).
enum class CurveUnit : uint8_t { None, Hz, Db, Percent, Seconds, Cents };
// Where a curve came from (the patch language's `@curve(name[, arg])`, the knob's curve menu): Custom for anything
// edited by hand. A preset is rebuilt from its name, arg and the range (curvePreset).
enum class CurvePreset : uint8_t {
  Custom,
  Linear,       // one straight segment
  Octave,       // equal ratios per travel: one Exp segment, k = ln(max / min) (min > 0)
  Exponential,  // one Exp segment, k = arg (default kCurveBendK): fine at the low end
  Logarithmic,  // one Exp segment, k = −arg (default kCurveBendK): fine at the high end
  Exp0,         // `@curve(exp0, k)`: exponential from exactly min, one Exp segment of bend k·ln 2 (the Phase accumulator's
                // frequency: k = 14; the Envelope's times: 15.87)
  Vca,          // GU.4, `@curve(vca, zone)`: the VCA's law (the old meta module's Log curve, exactly): linear through the
                // fine zone [−1, 1] (clipped to the range), which takes `zone` (0.05 … 0.9, default kCurveVcaZone) of the
                // travel when the range reaches past it on both sides; beyond it on each side a Joined segment of bend
                // ±max(ln(end / 1), 1) (C1 at ±1), the two outers sharing the rest of the travel in proportion to their
                // log extents; a range wholly beyond ±1 on one side is one pure exponential (Exp, bend ln(max / min)).
};
inline constexpr double kCurveVcaZone = 0.4;
inline constexpr double kCurveVcaZoneMin = 0.05, kCurveVcaZoneMax = 0.9;
inline constexpr double kCurveBendK = 4.8;  // the Exponential / Logarithmic presets (today's knob curves: 8 · ±0.6)

struct CurvePoint {
  double t = 0.0, y = 0.0;
  friend bool operator==(const CurvePoint&, const CurvePoint&) = default;
};
struct CurveSegment {
  CurveShape shape = CurveShape::Linear;
  double bend = 0.0;
  friend bool operator==(const CurveSegment&, const CurveSegment&) = default;
};

struct Curve {
  double min = 0.0, max = 1.0;
  CurveUnit unit = CurveUnit::None;
  int8_t digits = -1;  // readout decimals (−1: the unit's default)
  double step = 0.0;   // > 0: values rounded to multiples of it
  uint8_t segments = 1;  // 1 … kMaxCurveSegments
  std::array<CurvePoint, kMaxCurveSegments + 1> points{{{0.0, 0.0}, {1.0, 1.0}}};
  std::array<CurveSegment, kMaxCurveSegments> shapes{};
  CurveMode mode = CurveMode::Clip;
  CurveOutput output = CurveOutput::None;
  CurvePreset preset = CurvePreset::Linear;
  double presetArg = 0.0;  // the preset's argument (Exponential / Logarithmic: |k|; Exp0: k; Vca: the zone, 0: default)
  friend bool operator==(const Curve&, const Curve&) noexcept;
};

// Whether `c` is well formed (segments in range, points from 0 to 1 strictly increasing in t, every number finite,
// y ∈ [0, 1], step ≥ 0, a DbToGain range finite). Unused points / shapes (past `segments`) are ignored.
bool validCurve(const Curve& c) noexcept;
// A preset over [min, max]. Octave needs min > 0 (else Linear). Vca: max below min is a constant min.
Curve curvePreset(CurvePreset p, double min, double max, double arg = 0.0) noexcept;
// The patch language's names ("linear", "octave" — "exp" is its historical alias —, "exponential", "logarithmic",
// "exp0", "vca"); Custom has none.
std::string_view curvePresetName(CurvePreset p) noexcept;
std::optional<CurvePreset> curvePresetFromName(std::string_view name) noexcept;
// Whether every segment is straight (Linear, or a bend of 0): a wire then adds in the curve's units by default.
bool curveIsLinear(const Curve& c) noexcept;
// The structural class (see above). Equal classes: the atom's specialisation is the same.
uint32_t curveClass(const Curve& c) noexcept;

// The value at knob position `t` (see "Evaluation"). In the curve's units after its output conversion: what the DSP
// reads (a gain for a DbToGain curve).
double evalCurve(const Curve& c, double t) noexcept;
// The same before the output conversion (the curve's own units: dB for a DbToGain curve), and the conversion itself
// and its inverse (identity without one).
double evalCurveUnits(const Curve& c, double t) noexcept;
double curveOutput(const Curve& c, double units) noexcept;
double curveInput(const Curve& c, double value) noexcept;  // value → the curve's units (a gain → dB; ≤ 0: −∞ → min)
// The knob position in [0, 1] that gives `value` (after the output conversion; see "The inverse").
double curvePosition(const Curve& c, double value, double hint = 0.0) noexcept;

// ---- The atom's data -------------------------------------------------------------------------------------------------
// The coefficient form of segment i (double; the atom's data rounds it to float), on x = p − t0: a = Y0 for every
// shape; Linear b = dY / w; Exp b = dY / expm1(k), c = k / w; Log b = dY / k, c = expm1(k) / w; Power b = dY, c = 1 / w,
// e = q; SCurve b = dY, c = 1 / w, e = 2k, f = 1 / (2·expm1(k)); Joined (from its flat end) a = the flat end's value,
// e = α / w, b = ±β, c = ±K / w, f = 0 or w (see "Evaluation"). A segment with a zero bend is compiled as Linear.
// (x rather than p: the kernel's float x = p − t0 is exact near t0, where a folded c·p − c·t0 would cancel.)
struct SegmentCoeffs {
  CurveShape shape = CurveShape::Linear;
  double t0 = 0.0;
  double a = 0.0, b = 0.0, c = 0.0, e = 0.0, f = 0.0;
  double w = 0.0;  // the segment's width (in position)
};
SegmentCoeffs segmentCoeffs(const Curve& c, uint32_t i) noexcept;

// What the `curve` kernel reads (one acquire load per call). 64-byte aligned, trivially copyable.
struct alignas(64) CurveData {
  uint32_t segments = 1;
  uint32_t shapes = 1;   // bit s: some segment has shape s (after compilation)
  uint32_t mode = 0;     // CurveMode
  uint32_t output = 0;   // CurveOutput
  float step = 0.0f, invStep = 0.0f;
  uint32_t single = 0;   // the shape of every segment when they all share one (else kCurveShapeCount: mixed)
  // 1: one Exp segment through zero (a = b: the Octave preset, Y0 · (Y1/Y0)^p), evaluated as a · e^(c·x) with the plain
  // exp (≈ 4× cheaper than expm1, and as exact: relative error everywhere). compileCurve sets it.
  uint32_t pureExp = 0;
  float t[kMaxCurveSegments]{};  // segment starts (t[0] = 0)
  float a[kMaxCurveSegments]{}, b[kMaxCurveSegments]{}, c[kMaxCurveSegments]{};
  float e[kMaxCurveSegments]{}, f[kMaxCurveSegments]{};
  float shape[kMaxCurveSegments]{};  // CurveShape codes as floats (compared in the kernel)
  float reserved[24]{};              // to the 64-byte multiple: no padding bytes (sameData compares the bytes)
};
static_assert(sizeof(CurveData) == 576, "CurveData has no padding bytes");
CurveData compileCurve(const Curve& c) noexcept;
bool sameData(const CurveData& a, const CurveData& b) noexcept;  // bitwise
// A hash of everything a curve maps (its compiled data and its range): cache keys of what draws it. Never 0.
uint64_t curveHash(const Curve& c) noexcept;

// The resource a `curve` atom reads (Resource::Curve): one atomic pointer to an immutable CurveData. publish() (any
// non-RT thread, serialised internally) swaps in new data and retires the old to the reclaimer (freed after the audio
// thread's next quiescent state: a kernel that loaded it in the block running now may still read it). acquire() is
// RT-safe; a kernel never keeps the pointer past its call. The slot must outlive every image that reads it.
class CurveSlot {
public:
  explicit CurveSlot(const CurveData& initial);
  ~CurveSlot();
  CurveSlot(const CurveSlot&) = delete;
  CurveSlot& operator=(const CurveSlot&) = delete;
  // Non-RT. Equal data (bitwise) is a no-op (false).
  bool publish(const CurveData& data, EpochReclaimer& reclaimer);
  // Non-RT: a copy of the current data.
  CurveData data() const;
  const CurveData* acquire() const noexcept { return data_.load(std::memory_order_acquire); }  // RT-safe

private:
  std::atomic<const CurveData*> data_{nullptr};
  mutable std::mutex writeMutex_;
};

}  // namespace pq::dsp
