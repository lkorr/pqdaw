// Control curves (GU.1): the model, presets, the double evaluator, the inverse and the atom's data. pq/dsp/curve.h.

#include "pq/dsp/curve.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>

#include "pq/core/epoch_reclaimer.h"

namespace pq::dsp {
namespace {

constexpr double kStraight = 1e-9;  // |bend| below this: a straight segment (compiled as Linear)

bool finite(double v) noexcept { return std::isfinite(v); }

double clamp01(double v) noexcept { return v > 0.0 ? (v < 1.0 ? v : 1.0) : 0.0; }  // NaN → 0

// The position the curve evaluates (its mode) and the sign a signed curve applies.
double prepare(const Curve& c, double t, double& sign) noexcept {
  sign = 1.0;
  switch (c.mode) {
    case CurveMode::Wrap: {
      if (!finite(t)) return 0.0;
      const double p = t - std::floor(t);
      return p < 1.0 ? p : std::nextafter(1.0, 0.0);
    }
    case CurveMode::Signed:
      sign = t > 0.0 ? 1.0 : t < 0.0 ? -1.0 : 0.0;
      return clamp01(std::fabs(t));
    case CurveMode::Clip: break;
  }
  return clamp01(t);
}

uint32_t segmentOf(const Curve& c, double p) noexcept {
  uint32_t i = 0;
  for (uint32_t k = 1; k < c.segments; ++k)
    if (p >= c.points[k].t) i = k;
  return i;
}

double segValue(const SegmentCoeffs& s, double p) noexcept {
  const double x = p - s.t0;
  switch (s.shape) {
    case CurveShape::Linear: return s.a + s.b * x;
    case CurveShape::Exp: return s.a + s.b * std::expm1(s.c * x);
    case CurveShape::Log: return s.a + s.b * std::log1p(s.c * x);
    case CurveShape::Power: return s.a + s.b * std::pow(clamp01(s.c * x), s.e);
    case CurveShape::SCurve: {
      const double u = clamp01(s.c * x);
      const double h = s.f * std::expm1(s.e * std::min(u, 1.0 - u));
      return s.a + s.b * (u < 0.5 ? h : 1.0 - h);
    }
    case CurveShape::Joined: {
      const double xf = x - s.f;
      return s.a + s.e * xf + s.b * std::expm1(s.c * xf);
    }
  }
  return s.a;
}

// The position in segment s at which its value is `u`, or NaN when it has none (the caller checked u lies between
// the segment's end values; flat: NaN).
double segInverse(const SegmentCoeffs& s, double u) noexcept {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  double x = nan;
  switch (s.shape) {
    case CurveShape::Linear: x = s.b != 0.0 ? (u - s.a) / s.b : nan; break;
    case CurveShape::Exp: x = s.b != 0.0 && s.c != 0.0 ? std::log1p((u - s.a) / s.b) / s.c : nan; break;
    case CurveShape::Log: x = s.b != 0.0 && s.c != 0.0 ? std::expm1((u - s.a) / s.b) / s.c : nan; break;
    case CurveShape::Power:
      if (s.b != 0.0 && s.c != 0.0) x = std::pow(clamp01((u - s.a) / s.b), 1.0 / s.e) / s.c;
      break;
    case CurveShape::SCurve: {
      if (s.b == 0.0 || s.c == 0.0 || s.f == 0.0 || s.e == 0.0) break;
      const double g = clamp01((u - s.a) / s.b);
      const double m = std::log1p((g <= 0.5 ? g : 1.0 - g) / s.f) / s.e;
      x = (g <= 0.5 ? m : 1.0 - m) / s.c;
      break;
    }
    case CurveShape::Joined: {
      // No closed form (a line plus an exponential): bisection over the segment, which is monotonic (h' = α + β·K·e^(Ku)
      // runs from A ≥ 0 upward). The caller checked u lies between the end values.
      const double w = s.w;
      if (!(w > 0.0)) break;
      auto f = [&](double xx) { return s.a + s.e * (xx - s.f) + s.b * std::expm1(s.c * (xx - s.f)); };
      double lo = 0.0, hi = w;
      const bool rising = f(hi) >= f(lo);
      for (int it = 0; it < 80 && hi - lo > 0.0; ++it) {
        const double mid = 0.5 * (lo + hi);
        if ((f(mid) < u) == rising) lo = mid;
        else hi = mid;
      }
      x = 0.5 * (lo + hi);
      break;
    }
  }
  return s.t0 + x;
}

double pointValue(const Curve& c, uint32_t i) noexcept { return c.min + c.points[i].y * (c.max - c.min); }

// GU.4: the VCA's law (CurvePreset::Vca; the old meta module's Log curve, pq/meta/vca.h before GU.4) over c.min …
// c.max as segments: [negative outer] [fine zone] [positive outer], each present only when it has travel.
void vcaPreset(Curve& c, double zoneArg) noexcept {
  constexpr double kFine = 1.0;
  const double zone = std::clamp(zoneArg != 0.0 ? zoneArg : kCurveVcaZone, kCurveVcaZoneMin, kCurveVcaZoneMax);
  c.presetArg = zoneArg != 0.0 ? zone : 0.0;
  const double lo = c.min, hi = std::max(c.max, c.min);
  c.max = hi;
  const double fLo = std::clamp(-kFine, lo, hi), fHi = std::clamp(kFine, lo, hi), fw = fHi - fLo;
  const double sP = std::max(kFine, lo), sN = std::max(kFine, -hi);
  const double endP = std::max(hi, sP), endN = std::max(-lo, sN);
  const double eP = std::log(endP / sP), eN = std::log(endN / sN), e = eN + eP;
  const double zf = e > 0.0 ? zone * fw * (0.5 / kFine) : 1.0;
  const double share = (1.0 - zf) / std::max(e, 1e-30);
  const double pN = share * eN, pP = pN + zf;
  const double range = hi - lo;
  auto y = [&](double v) { return range > 0.0 ? std::clamp((v - lo) / range, 0.0, 1.0) : 0.0; };
  uint8_t n = 0;
  auto point = [&](double t, double v) { c.points[n] = {t, y(v)}; };
  point(0.0, lo);
  if (eN > 0.0 && pN > 0.0) {  // the negative outer: joined to the fine zone at its end, or a pure exponential
    c.shapes[n] = hi > -kFine ? CurveSegment{CurveShape::Joined, -std::max(eN, 1.0)} : CurveSegment{CurveShape::Exp, -eN};
    ++n;
    point(pN < 1.0 ? pN : 1.0, -sN);
  }
  if (zf > 0.0 && fw > 0.0 && pP > c.points[n].t) {  // the fine zone (linear)
    c.shapes[n] = {CurveShape::Linear, 0.0};
    ++n;
    point(eP > 0.0 && pP < 1.0 ? pP : 1.0, fHi);
  }
  if (eP > 0.0 && c.points[n].t < 1.0) {  // the positive outer: joined to the fine zone at its start, or pure
    c.shapes[n] = lo < kFine ? CurveSegment{CurveShape::Joined, std::max(eP, 1.0)} : CurveSegment{CurveShape::Exp, eP};
    ++n;
    point(1.0, endP);
  }
  if (n == 0) {  // no travel anywhere (a constant)
    c.shapes[0] = {CurveShape::Linear, 0.0};
    n = 1;
    c.points[1] = {1.0, y(hi)};
  }
  c.points[n].t = 1.0;
  c.segments = n;
}

}  // namespace

bool operator==(const Curve& a, const Curve& b) noexcept {
  if (a.min != b.min || a.max != b.max || a.unit != b.unit || a.digits != b.digits || a.step != b.step ||
      a.segments != b.segments || a.mode != b.mode || a.output != b.output || a.preset != b.preset ||
      a.presetArg != b.presetArg)
    return false;
  for (uint32_t i = 0; i <= a.segments && i <= kMaxCurveSegments; ++i)
    if (!(a.points[i] == b.points[i])) return false;
  for (uint32_t i = 0; i < a.segments && i < kMaxCurveSegments; ++i)
    if (!(a.shapes[i] == b.shapes[i])) return false;
  return true;
}

bool validCurve(const Curve& c) noexcept {
  if (c.segments < 1 || c.segments > kMaxCurveSegments) return false;
  if (!finite(c.min) || !finite(c.max) || !finite(c.step) || c.step < 0.0 || !finite(c.presetArg)) return false;
  if (static_cast<uint8_t>(c.mode) > static_cast<uint8_t>(CurveMode::Signed)) return false;
  if (static_cast<uint8_t>(c.output) > static_cast<uint8_t>(CurveOutput::DbToGain)) return false;
  if (static_cast<uint8_t>(c.unit) > static_cast<uint8_t>(CurveUnit::Cents)) return false;
  if (static_cast<uint8_t>(c.preset) > static_cast<uint8_t>(CurvePreset::Vca)) return false;
  if (c.points[0].t != 0.0 || c.points[c.segments].t != 1.0) return false;
  for (uint32_t i = 0; i <= c.segments; ++i) {
    const CurvePoint& p = c.points[i];
    if (!finite(p.t) || !finite(p.y) || p.y < 0.0 || p.y > 1.0) return false;
    if (i > 0 && !(p.t > c.points[i - 1].t)) return false;
  }
  for (uint32_t i = 0; i < c.segments; ++i) {
    const CurveSegment& s = c.shapes[i];
    if (static_cast<uint8_t>(s.shape) >= kCurveShapeCount || !finite(s.bend) || std::fabs(s.bend) > 64.0) return false;
  }
  return true;
}

Curve curvePreset(CurvePreset p, double min, double max, double arg) noexcept {
  Curve c;
  c.min = min;
  c.max = max;
  c.segments = 1;
  c.points[0] = {0.0, 0.0};
  c.points[1] = {1.0, 1.0};
  c.preset = p;
  c.presetArg = 0.0;
  CurveSegment& s = c.shapes[0];
  switch (p) {
    case CurvePreset::Custom:
    case CurvePreset::Linear: c.preset = CurvePreset::Linear; break;
    case CurvePreset::Octave:
      if (min > 0.0 && max > 0.0 && max != min) {
        s = {CurveShape::Exp, std::log(max / min)};
      } else {
        c.preset = CurvePreset::Linear;
      }
      break;
    case CurvePreset::Exponential:
    case CurvePreset::Logarithmic: {
      const double k = arg != 0.0 ? std::fabs(arg) : kCurveBendK;
      c.presetArg = arg != 0.0 ? std::fabs(arg) : 0.0;
      s = {CurveShape::Exp, p == CurvePreset::Exponential ? k : -k};
      break;
    }
    case CurvePreset::Exp0:
      c.presetArg = arg;
      if (arg != 0.0) s = {CurveShape::Exp, arg * std::numbers::ln2};
      else c.preset = CurvePreset::Linear;
      break;
    case CurvePreset::Vca:
      vcaPreset(c, arg);
      // GU.5b: the VCA's markers (±10, ±100 where the range reaches past them) are points of the curve: dragging one
      // edits the curve. Splitting a segment where the curve has that value keeps the function exactly.
      for (const double v : {-100.0, -10.0, 10.0, 100.0}) (void)splitCurveAt(c, curvePosition(c, v, 0.5));
      break;
  }
  return c;
}

bool splitCurveAt(Curve& c, double t) noexcept {
  if (!(t > 0.0 && t < 1.0) || c.segments >= kMaxCurveSegments) return false;
  const uint32_t i = segmentOf(c, t);
  const double t0 = c.points[i].t, t1 = c.points[i + 1].t, w = t1 - t0;
  constexpr double kGap = 1e-6;  // never a sliver segment
  if (!(t - t0 > kGap && t1 - t > kGap)) return false;
  const CurveSegment seg = c.shapes[i];
  const bool straight = seg.shape == CurveShape::Linear || std::fabs(seg.bend) < kStraight;
  if (!straight && seg.shape != CurveShape::Exp && seg.shape != CurveShape::Joined) return false;
  // The value there in the curve's own units, as the share of the range the points hold.
  const SegmentCoeffs sc = segmentCoeffs(c, i);
  const double range = c.max - c.min;
  const double y = range != 0.0 ? std::clamp((segValue(sc, t) - c.min) / range, 0.0, 1.0) : c.points[i].y;
  for (uint32_t k = c.segments; k > i; --k) c.shapes[k] = c.shapes[k - 1];
  for (uint32_t k = c.segments + 1u; k > i + 1; --k) c.points[k] = c.points[k - 1];
  c.points[i + 1] = {t, y};
  // An exponential restricted to part of its width is the same exponential with its bend in proportion (Exp: the
  // ratio over the part; Joined: its line and exponential, the neighbour's slope at the join carried by the evaluator).
  if (!straight) {
    c.shapes[i].bend = seg.bend * (t - t0) / w;
    c.shapes[i + 1].bend = seg.bend * (t1 - t) / w;
  }
  ++c.segments;
  return true;
}

std::string_view curvePresetName(CurvePreset p) noexcept {
  switch (p) {
    case CurvePreset::Linear: return "linear";
    case CurvePreset::Octave: return "octave";
    case CurvePreset::Exponential: return "exponential";
    case CurvePreset::Logarithmic: return "logarithmic";
    case CurvePreset::Exp0: return "exp0";
    case CurvePreset::Vca: return "vca";
    case CurvePreset::Custom: break;
  }
  return {};
}

std::optional<CurvePreset> curvePresetFromName(std::string_view n) noexcept {
  if (n == "linear") return CurvePreset::Linear;
  if (n == "octave" || n == "exp") return CurvePreset::Octave;  // "exp": the historical name (@curve(exp))
  if (n == "exponential") return CurvePreset::Exponential;
  if (n == "logarithmic") return CurvePreset::Logarithmic;
  if (n == "exp0") return CurvePreset::Exp0;
  if (n == "vca") return CurvePreset::Vca;
  return std::nullopt;
}

bool curveIsLinear(const Curve& c) noexcept {
  for (uint32_t i = 0; i < c.segments && i < kMaxCurveSegments; ++i)
    if (c.shapes[i].shape != CurveShape::Linear) return false;
  return true;
}

uint32_t curveClass(const Curve& c) noexcept {
  uint32_t shapes = 0;
  uint32_t single = kCurveShapeCount;
  for (uint32_t i = 0; i < c.segments && i < kMaxCurveSegments; ++i) {
    const uint32_t s = static_cast<uint32_t>(c.shapes[i].shape);
    shapes |= 1u << s;
    single = i == 0 ? s : (single == s ? s : kCurveShapeCount);
  }
  static_assert(kCurveShapeCount <= 7, "the class packs the shape mask in 7 bits, `single` in 3");
  return static_cast<uint32_t>(c.segments) | shapes << 5 | static_cast<uint32_t>(c.mode) << 12 |
         static_cast<uint32_t>(c.output) << 14 | (c.step > 0.0 ? 1u : 0u) << 16 | single << 17;
}

namespace {

SegmentCoeffs coeffsAt(const Curve& c, uint32_t i, uint32_t depth) noexcept;

// The slope (value per unit of position) of segment i at its start or end (a Joined neighbour computed recursively:
// its own join points away from here, so the recursion ends within the segment count).
double slopeAt(const Curve& c, uint32_t i, bool atEnd, uint32_t depth) noexcept {
  const SegmentCoeffs s = coeffsAt(c, i, depth + 1);
  const double x = atEnd ? s.w : 0.0;
  switch (s.shape) {
    case CurveShape::Linear: return s.b;
    case CurveShape::Exp: return s.b * s.c * std::exp(s.c * x);
    case CurveShape::Log: return s.b * s.c / (1.0 + s.c * x);
    case CurveShape::Power: {
      if (s.e == 1.0) return s.b * s.c;
      if (!atEnd) return s.e < 1.0 ? std::copysign(std::numeric_limits<double>::infinity(), s.b) : 0.0;
      return s.b * s.e * s.c;
    }
    case CurveShape::SCurve: return s.b * s.f * s.e * s.c;
    case CurveShape::Joined: return s.e + s.b * s.c * std::exp(s.c * (x - s.f));
  }
  return 0.0;
}

SegmentCoeffs coeffsAt(const Curve& c, uint32_t i, uint32_t depth) noexcept {
  SegmentCoeffs s;
  const double t0 = c.points[i].t, t1 = c.points[i + 1].t;
  const double w = t1 - t0;
  const double y0 = pointValue(c, i), y1 = pointValue(c, i + 1);
  const double dy = y1 - y0;
  s.t0 = t0;
  const CurveSegment& seg = c.shapes[i];
  const double k = seg.bend;
  CurveShape shape = seg.shape;
  if (shape != CurveShape::Linear && std::fabs(k) < kStraight) shape = CurveShape::Linear;
  const double q = shape == CurveShape::Power ? std::exp2(k) : 1.0;
  s.shape = shape;
  s.a = y0;
  switch (shape) {
    case CurveShape::Linear: s.b = dy / w; break;
    case CurveShape::Exp:
      s.b = dy / std::expm1(k);
      s.c = k / w;
      break;
    case CurveShape::Log:
      s.b = dy / k;
      s.c = std::expm1(k) / w;
      break;
    case CurveShape::Power:
      s.b = dy;
      s.c = 1.0 / w;
      s.e = q;
      break;
    case CurveShape::SCurve:
      s.b = dy;
      s.c = 1.0 / w;
      s.e = 2.0 * k;
      s.f = 0.5 / std::expm1(k);
      break;
    case CurveShape::Joined: {
      // The neighbour at the flat end (k > 0: the previous segment's end, k < 0: the next one's start); none, or a
      // Joined one whose flat end meets this one: slope 0 there.
      const bool atStart = k > 0.0;
      const int64_t nb = atStart ? static_cast<int64_t>(i) - 1 : static_cast<int64_t>(i) + 1;
      double slope = 0.0;
      if (nb >= 0 && nb < static_cast<int64_t>(c.segments) && depth < kMaxCurveSegments) {
        const CurveSegment& ns = c.shapes[static_cast<size_t>(nb)];
        const bool facing = ns.shape == CurveShape::Joined && std::fabs(ns.bend) >= kStraight && ((ns.bend > 0.0) != atStart);
        if (!facing) slope = slopeAt(c, static_cast<uint32_t>(nb), atStart, depth);
      }
      const double K = std::fabs(k);
      double A = slope * w;  // the rise the neighbour's slope would make over this segment
      if (!std::isfinite(A)) A = dy;
      A = dy >= 0.0 ? std::clamp(A, 0.0, dy) : std::clamp(A, dy, 0.0);
      const double beta = (dy - A) / (std::expm1(K) - K);
      const double alpha = A - beta * K;
      s.e = alpha / w;
      if (atStart) {  // from the start: Y0 + α·u + β·expm1(K·u), u = x / w
        s.a = y0;
        s.b = beta;
        s.c = K / w;
        s.f = 0.0;
      } else {  // from the end: Y1 − α·u − β·expm1(K·u), u = (w − x) / w = −x' / w
        s.a = y1;
        s.b = -beta;
        s.c = -K / w;
        s.f = w;
      }
      break;
    }
  }
  s.w = w;
  return s;
}

}  // namespace

SegmentCoeffs segmentCoeffs(const Curve& c, uint32_t i) noexcept { return coeffsAt(c, i, 0); }

double curveOutput(const Curve& c, double u) noexcept {
  return c.output == CurveOutput::DbToGain ? std::pow(10.0, u / 20.0) : u;
}

double curveInput(const Curve& c, double v) noexcept {
  if (c.output != CurveOutput::DbToGain) return v;
  return v > 0.0 ? 20.0 * std::log10(v) : -std::numeric_limits<double>::infinity();
}

double evalCurveUnits(const Curve& c, double t) noexcept {
  double sign = 1.0;
  const double p = prepare(c, t, sign);
  double y = segValue(segmentCoeffs(c, segmentOf(c, p)), p);
  if (c.mode == CurveMode::Signed) y *= sign;
  if (c.step > 0.0) y = std::floor(y * (1.0 / c.step) + 0.5) * c.step;
  return y;
}

double evalCurve(const Curve& c, double t) noexcept { return curveOutput(c, evalCurveUnits(c, t)); }

double curvePosition(const Curve& c, double value, double hint) noexcept {
  double u = curveInput(c, value);
  if (std::isnan(u)) return clamp01(hint);
  u = std::clamp(u, -1e300, 1e300);
  hint = clamp01(hint);
  // The hint itself gives the value (a write of the value the knob shows): it stays — no float drift from re-inverting.
  if (evalCurve(c, hint) == value) return hint;
  double best = -1.0, bestDist = 0.0;  // the reaching position nearest the hint
  auto offer = [&](double p) {
    const double d = std::fabs(p - hint);
    if (best < 0.0 || d < bestDist) {
      best = p;
      bestDist = d;
    }
  };
  for (uint32_t i = 0; i < c.segments; ++i) {
    const double t0 = c.points[i].t, t1 = c.points[i + 1].t;
    const double y0 = pointValue(c, i), y1 = pointValue(c, i + 1);
    const double lo = std::min(y0, y1), hi = std::max(y0, y1);
    if (u < lo || u > hi) continue;
    if (y0 == y1) {  // flat: every position of it gives the value
      offer(std::clamp(hint, t0, t1));
      continue;
    }
    if (u == y0) {
      offer(t0);
      continue;
    }
    if (u == y1) {
      offer(t1);
      continue;
    }
    const double p = segInverse(segmentCoeffs(c, i), u);
    if (std::isfinite(p)) offer(std::clamp(p, t0, t1));
  }
  if (best >= 0.0) return clamp01(best);
  // Unreachable (the curve is continuous, so u lies below its lowest value or above its highest): the extreme nearest
  // it — a point (each segment is monotonic) — nearest the hint on a tie.
  double lowest = pointValue(c, 0), highest = lowest;
  for (uint32_t i = 1; i <= c.segments; ++i) {
    lowest = std::min(lowest, pointValue(c, i));
    highest = std::max(highest, pointValue(c, i));
  }
  const double target = u < lowest ? lowest : highest;
  double bestValueDist = 0.0;
  for (uint32_t i = 0; i <= c.segments; ++i) {
    const double dv = std::fabs(pointValue(c, i) - target);
    const double p = c.points[i].t;
    if (best < 0.0 || dv < bestValueDist || (dv == bestValueDist && std::fabs(p - hint) < std::fabs(best - hint))) {
      best = p;
      bestValueDist = dv;
    }
  }
  return clamp01(best);
}

CurveData compileCurve(const Curve& c) noexcept {
  CurveData d;
  const uint32_t n = std::clamp<uint32_t>(c.segments, 1, kMaxCurveSegments);
  d.segments = n;
  d.mode = static_cast<uint32_t>(c.mode);
  d.output = static_cast<uint32_t>(c.output);
  d.step = static_cast<float>(c.step);
  d.invStep = c.step > 0.0 ? static_cast<float>(1.0 / c.step) : 0.0f;
  d.shapes = 0;
  d.single = kCurveShapeCount;
  for (uint32_t i = 0; i < n; ++i) {
    const SegmentCoeffs s = segmentCoeffs(c, i);
    const uint32_t sh = static_cast<uint32_t>(s.shape);
    d.shapes |= 1u << sh;
    d.single = i == 0 ? sh : (d.single == sh ? sh : kCurveShapeCount);
    d.t[i] = static_cast<float>(s.t0);
    d.a[i] = static_cast<float>(s.a);
    d.b[i] = static_cast<float>(s.b);
    d.c[i] = static_cast<float>(s.c);
    d.e[i] = static_cast<float>(s.e);
    d.f[i] = static_cast<float>(s.f);
    d.shape[i] = static_cast<float>(sh);
  }
  for (uint32_t i = n; i < kMaxCurveSegments; ++i) d.t[i] = 2.0f;  // never reached by a search over p ≤ 1
  if (n == 1 && d.single == static_cast<uint32_t>(CurveShape::Exp)) {
    const SegmentCoeffs s = segmentCoeffs(c, 0);
    if (s.a != 0.0 && std::fabs(s.a - s.b) <= 1e-12 * std::fabs(s.a)) {  // a + a·(e^(cx) − 1) = a·e^(cx)
      d.pureExp = 1;
      d.b[0] = d.a[0];
    }
  }
  return d;
}

bool sameData(const CurveData& a, const CurveData& b) noexcept { return std::memcmp(&a, &b, sizeof(CurveData)) == 0; }

uint64_t curveHash(const Curve& c) noexcept {
  const CurveData d = compileCurve(c);
  uint64_t h = 1469598103934665603ull;
  auto mix = [&](const void* p, size_t n) {
    const auto* b = static_cast<const unsigned char*>(p);
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
  };
  mix(&d, sizeof d);
  mix(&c.min, sizeof c.min);
  mix(&c.max, sizeof c.max);
  return h | 1u;
}

CurveSlot::CurveSlot(const CurveData& initial) { data_.store(new CurveData(initial), std::memory_order_release); }

CurveSlot::~CurveSlot() { delete data_.load(std::memory_order_acquire); }

bool CurveSlot::publish(const CurveData& data, EpochReclaimer& reclaimer) {
  std::lock_guard lock(writeMutex_);
  const CurveData* cur = data_.load(std::memory_order_relaxed);
  if (cur != nullptr && sameData(*cur, data)) return false;
  data_.store(new CurveData(data), std::memory_order_release);
  if (cur != nullptr) reclaimer.retire(const_cast<CurveData*>(cur), [](void* p) noexcept { delete static_cast<CurveData*>(p); });
  return true;
}

CurveData CurveSlot::data() const {
  std::lock_guard lock(writeMutex_);
  return *data_.load(std::memory_order_relaxed);
}

}  // namespace pq::dsp
