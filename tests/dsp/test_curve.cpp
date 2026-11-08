// GU.1: control curves (pq/dsp/curve.h) and the `curve` atom (pq/dsp/atoms.h, kernels_curve.cpp).
//   presets   reproduce the knob laws that came before them (TX.knob-curves' doc::travelValue: linear, octave, exp0,
//             exponential / logarithmic K = ±4.8, a dB gain linear in dB and bent in dB, @steps, @wrap, @signed),
//             written out here as the reference formulas
//   inverse   typed values → the nearest knob position (monotonic round trips, a curve that goes up and back down,
//             unreachable values, flat segments, a gain below a dB curve's floor)
//   kernel    the atom vs evalCurve at every lane count and rate, 1 lane == 8 lanes bit for bit, every shape and
//             mixed 16-segment curves; the slot's publication
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <random>
#include <string>
#include <vector>

#include "pq/core/aligned_buffer.h"
#include "pq/core/epoch_reclaimer.h"
#include "pq/dsp/atoms.h"
#include "pq/dsp/curve.h"

using namespace pq::dsp;

namespace {

// The knob laws before GU.1 (TX.knob-curves, doc::travelValue), the references.
double oldLinear(double lo, double hi, double p) { return lo + p * (hi - lo); }
double oldOctave(double lo, double hi, double p) { return lo * std::exp(p * std::log(hi / lo)); }
double oldExp0(double lo, double hi, double k, double p) {
  const double K = k * std::numbers::ln2;
  return lo + (hi - lo) * std::expm1(p * K) / std::expm1(K);
}
double oldBend(double lo, double hi, double K, double p) { return lo + (hi - lo) * std::expm1(K * p) / std::expm1(K); }
double oldDbLinear(double lo, double hi, double p) {  // lo, hi gains: linear in dB
  return lo * std::exp(p * std::log(hi / lo));
}
double oldDbBend(double lo, double hi, double K, double p) {  // lo·(hi/lo)^g(p)
  return lo * std::exp(std::expm1(K * p) / std::expm1(K) * std::log(hi / lo));
}

void close(double got, double want, double rel = 1e-12) {
  const double tol = rel * std::max({std::fabs(want), 1e-300});
  CHECK_MESSAGE(std::fabs(got - want) <= tol, "got " << got << " want " << want);
}

double db(double gain) { return 20.0 * std::log10(gain); }

Curve dbCurve(double loGain, double hiGain, CurvePreset p = CurvePreset::Linear, double arg = 0.0) {
  Curve c = curvePreset(p, db(loGain), db(hiGain), arg);
  c.output = CurveOutput::DbToGain;
  c.unit = CurveUnit::Db;
  return c;
}

// Runs the kernel over `x` (frames · lanes floats, or one 8-float pattern when spl) and returns its output.
std::vector<float> runKernel(const CurveSlot* slot, const std::vector<float>& x, uint32_t lanes, bool spl, uint32_t frames,
                             bool convert = true) {
  pq::AlignedBuffer<float> in(std::max<size_t>(x.size(), 8)), conv(8), out(static_cast<size_t>(frames) * lanes);
  std::copy(x.begin(), x.end(), in.data());
  for (uint32_t i = 0; i < 8; ++i) conv[i] = convert ? 1.0f : 0.0f;
  const float* ins[2] = {in.data(), conv.data()};
  float* outs[1] = {out.data()};
  const uint32_t mask = (spl ? 1u : 0u) | 2u;  // `convert` a pattern
  const KernelFn fn = findKernel(Atom::Curve, lanes, mask);
  REQUIRE(fn != nullptr);
  KernelArgs a{ins, outs, nullptr, slot, frames, 2, 1, mask};
  fn(a);
  return std::vector<float>(out.data(), out.data() + static_cast<size_t>(frames) * lanes);
}

// The atom agrees with evalCurve to float rounding: within a few ulp of the value or of the range (a cancellation in
// the coefficient form near a segment's start is relative to the range, not the value).
void kernelMatches(const Curve& c, const std::string& what) {
  CurveSlot slot(compileCurve(c));
  std::vector<float> x;
  for (int i = -40; i <= 1040; ++i) x.push_back(static_cast<float>(i) / 1000.0f);
  for (const float v : {0.0f, 1.0f, -1.0f, 2.5f, -0.3f, 0.49999997f, 0.5f, 1e-7f, 0.99999994f}) x.push_back(v);
  while (x.size() % 8) x.push_back(0.5f);
  const auto frames8 = static_cast<uint32_t>(x.size() / 8);
  const std::vector<float> y1 = runKernel(&slot, x, 1, false, static_cast<uint32_t>(x.size()));
  const std::vector<float> y8 = runKernel(&slot, x, 8, false, frames8);
  const double range = std::fabs(evalCurve(c, 1.0) - evalCurve(c, 0.0)) + std::fabs(c.max - c.min) + 1e-30;
  double worst = 0.0;
  for (size_t i = 0; i < x.size(); ++i) {
    CHECK_MESSAGE(std::bit_cast<uint32_t>(y1[i]) == std::bit_cast<uint32_t>(y8[i]), what << " lane x=" << x[i]);
    const double want = evalCurve(c, static_cast<double>(x[i]));
    // (A stepped curve within float rounding of a step's rounding boundary may round either way.)
    if (c.step > 0.0) {
      Curve raw = c;
      raw.step = 0.0;
      const double r = std::fabs(std::fmod(evalCurveUnits(raw, x[i]) / c.step + 0.5, 1.0));
      if (std::min(r, 1.0 - r) < 1e-5) continue;
    }
    // The segment points are floats in the kernel: on a steep segment (a large bend over a narrow width) a position
    // error of a few float ulps of 1 moves the value by the curve's slope times it. That much is allowed on top.
    const double h = 4.0 * 0x1p-24;
    const double xd = static_cast<double>(x[i]);
    const double posErr = std::max(std::fabs(evalCurve(c, xd + h) - want), std::fabs(evalCurve(c, xd - h) - want));
    const double err = std::max(0.0, std::fabs(static_cast<double>(y1[i]) - want) - posErr);
    const double scale = c.output == CurveOutput::DbToGain ? std::fabs(want) : std::max(std::fabs(want), range);
    if (err / std::max(scale, 1e-30) > worst) {
      worst = err / std::max(scale, 1e-30);
      if (worst > 4e-6) MESSAGE(what << ": x " << x[i] << " got " << y1[i] << " want " << want);
    }
  }
  CHECK_MESSAGE(worst <= 4e-6, what << ": worst relative error " << worst);
  // Spl (a param-rate position): every frame reads the pattern.
  const std::vector<float> pat = {0.3f, 0.3f, 0.3f, 0.3f, 0.3f, 0.3f, 0.3f, 0.3f};
  const std::vector<float> ys = runKernel(&slot, pat, 1, true, 5);
  for (const float v : ys) CHECK(std::bit_cast<uint32_t>(v) == std::bit_cast<uint32_t>(y1[340]));  // x[340] = 0.3
}

}  // namespace

TEST_CASE("curve presets reproduce the knob laws before them") {
  for (double p = 0.0; p <= 1.0; p += 1.0 / 64.0) {
    close(evalCurve(curvePreset(CurvePreset::Linear, -20.0, 300.0), p), oldLinear(-20.0, 300.0, p), 1e-13);
    close(evalCurve(curvePreset(CurvePreset::Octave, 20.0, 20000.0), p), oldOctave(20.0, 20000.0, p));
    close(evalCurve(curvePreset(CurvePreset::Exp0, 0.0, 20000.0, 14.0), p) + 1.0, oldExp0(0.0, 20000.0, 14.0, p) + 1.0);
    close(evalCurve(curvePreset(CurvePreset::Exp0, 0.001, 60.0, 15.87), p), oldExp0(0.001, 60.0, 15.87, p));
    close(evalCurve(curvePreset(CurvePreset::Exponential, 20.0, 20000.0), p), oldBend(20.0, 20000.0, 4.8, p));
    close(evalCurve(curvePreset(CurvePreset::Logarithmic, 20.0, 20000.0), p), oldBend(20.0, 20000.0, -4.8, p));
    // A dB gain (−70 … +6 dB): linear in dB, and bent in dB.
    const double lo = std::pow(10.0, -70.0 / 20.0), hi = std::pow(10.0, 6.0 / 20.0);
    close(evalCurve(dbCurve(lo, hi), p), oldDbLinear(lo, hi, p));
    close(evalCurve(dbCurve(lo, hi, CurvePreset::Exponential), p), oldDbBend(lo, hi, 4.8, p));
    close(evalCurve(dbCurve(lo, hi, CurvePreset::Logarithmic), p), oldDbBend(lo, hi, -4.8, p));
  }
  // The ends are exact where the old laws were.
  CHECK(evalCurve(curvePreset(CurvePreset::Exp0, 0.0, 20000.0, 14.0), 0.0) == 0.0);
  CHECK(evalCurve(curvePreset(CurvePreset::Linear, 2.0, 7.0), 0.0) == 2.0);
  CHECK(evalCurve(curvePreset(CurvePreset::Octave, 20.0, 20000.0), 0.0) == 20.0);
  // @steps: rounded to integers, floor(v + 0.5).
  Curve steps = curvePreset(CurvePreset::Linear, 0.0, 10.0);
  steps.step = 1.0;
  for (double p = 0.0; p <= 1.0; p += 0.01) CHECK(evalCurve(steps, p) == std::floor(10.0 * p + 0.5));
  // @wrap: the position wraps (a phase), so a wire's sum comes back round.
  Curve wrap = curvePreset(CurvePreset::Linear, 0.0, 1.0);
  wrap.mode = CurveMode::Wrap;
  close(evalCurve(wrap, 1.25), 0.25);
  close(evalCurve(wrap, -0.25), 0.75);
  close(evalCurve(wrap, 0.4), 0.4);
  // @signed: mirrored for negative positions (sign(p)·curve(|p|)), the knob's own half as without the flag.
  Curve sgn = curvePreset(CurvePreset::Exp0, 0.0, 20000.0, 14.0);
  sgn.mode = CurveMode::Signed;
  for (const double p : {0.1, 0.5, 0.9, 1.0}) {
    close(evalCurve(sgn, p), oldExp0(0.0, 20000.0, 14.0, p));
    close(evalCurve(sgn, -p), -oldExp0(0.0, 20000.0, 14.0, p));
  }
  CHECK(evalCurve(sgn, -2.0) == -evalCurve(sgn, 1.0));
  CHECK(evalCurve(sgn, 0.0) == 0.0);
  // Clamped outside [0, 1] (no flag).
  CHECK(evalCurve(curvePreset(CurvePreset::Linear, 3.0, 9.0), 7.0) == 9.0);
  CHECK(evalCurve(curvePreset(CurvePreset::Linear, 3.0, 9.0), -1.0) == 3.0);
}

namespace {

// GU.4: the VCA meta module's Log law before GU.4 (pq/meta/vca.h at 04c59ffe, markers at their places), written out
// from its definition as the reference for CurvePreset::Vca.
double oldVcaLog(double lo, double hi, double zone, double p) {
  hi = std::max(hi, lo);
  zone = std::clamp(zone, 0.05, 0.9);
  p = std::clamp(p, 0.0, 1.0);
  const double fLo = std::clamp(-1.0, lo, hi), fw = std::clamp(1.0, lo, hi) - fLo;
  const double sP = std::max(1.0, lo), sN = std::max(1.0, -hi);
  const double endP = std::max(hi, sP), endN = std::max(-lo, sN);
  const double eP = std::log(endP / sP), eN = std::log(endN / sN), e = eN + eP;
  const double zf = e > 0.0 ? zone * fw / 2.0 : 1.0;
  const double share = (1.0 - zf) / std::max(e, 1e-30);
  const double lenN = share * eN, lenP = share * eP, pN = lenN, pP = pN + zf;
  const double slope = fw / std::max(zf, 1e-30);
  struct Side {
    double s0, alpha, b, k, len;
  };
  auto side = [&](double s0, double end, double extent, double len, bool join) {
    if (!join) return Side{s0, 0.0, s0, extent, len};  // s0 + s0·(e^(K·u) − 1) = s0·(end / s0)^u
    const double k = std::max(extent, 1.0);
    const double a = std::min(slope * len, end - s0);
    const double b = (end - s0 - a) / (std::exp(k) - 1.0 - k);
    return Side{s0, a - b * k, b, k, len};
  };
  const Side pos = side(sP, endP, eP, lenP, lo < 1.0), neg = side(sN, endN, eN, lenN, hi > -1.0);
  auto outer = [](const Side& s, double u) { return s.s0 + s.alpha * u + s.b * std::expm1(s.k * u); };
  if (eN > 0.0 && p < pN) return -outer(neg, (pN - p) / neg.len);
  if (eP > 0.0 && p > pP) return outer(pos, (p - pP) / pos.len);
  return fLo + (p - pN) * slope;
}

}  // namespace

TEST_CASE("curve preset vca: the VCA's Log law exactly, C1 at the fine zone, every range and zone") {
  struct Range {
    double lo, hi, zone;
  };
  const Range ranges[] = {{-20000.0, 20000.0, 0.4}, {-20000.0, 20000.0, 0.05}, {-20000.0, 20000.0, 0.9}, {-2.0, 50.0, 0.6},
                          {0.0, 2.0, 0.4},         {-1.0, 1.0, 0.4},          {20.0, 20000.0, 0.4},    {-20000.0, -3.0, 0.4},
                          {-0.5, 100.0, 0.3},      {0.35, 0.35, 0.4},         {0.25, 0.8, 0.4},        {-300.0, 0.5, 0.7},
                          {5.0, 1.0, 0.4}};
  for (const Range& r : ranges) {
    CAPTURE(r.lo);
    CAPTURE(r.hi);
    CAPTURE(r.zone);
    const Curve c = curvePreset(CurvePreset::Vca, r.lo, r.hi, r.zone);
    REQUIRE(validCurve(c));
    const double scale = std::max({std::fabs(r.lo), std::fabs(r.hi), 1.0});
    for (int i = 0; i <= 4096; ++i) {
      const double p = i / 4096.0;
      const double want = oldVcaLog(r.lo, r.hi, r.zone, p);
      CHECK_MESSAGE(std::fabs(evalCurve(c, p) - want) <= 1e-12 * scale, "p " << p << " got " << evalCurve(c, p) << " want " << want);
    }
    // Typed values come back (monotonic: one position per value).
    for (int i = 1; i < 32; ++i) {
      const double p = i / 32.0, v = evalCurve(c, p);
      if (r.hi > r.lo) CHECK(std::fabs(evalCurve(c, curvePosition(c, v, 0.5)) - v) <= 1e-9 * scale);
    }
  }
  // The default: 0.7 → 1 (the knob's default), 0.3 → −1, 0.5 → 0, the ends ±20000; three segments, the outers split
  // at ±10 and ±100 (GU.5b: the markers are points), seven in all.
  const Curve d = curvePreset(CurvePreset::Vca, -20000.0, 20000.0);
  REQUIRE(d.segments == 7);
  for (const double v : {-100.0, -10.0, -1.0, 1.0, 10.0, 100.0}) {
    bool found = false;
    for (uint32_t i = 1; i < d.segments; ++i) found = found || std::fabs(d.min + d.points[i].y * (d.max - d.min) - v) < 1e-9 * 20000.0;
    CHECK_MESSAGE(found, "marker " << v);
  }
  CHECK(d.presetArg == 0.0);
  close(evalCurve(d, 0.7), 1.0, 1e-11);  // (points are shares of the range: ≈ 1e-16 of it)
  close(evalCurve(d, 0.3), -1.0, 1e-11);
  CHECK(std::fabs(evalCurve(d, 0.5)) < 1e-11);
  CHECK(static_cast<float>(evalCurve(d, 0.7)) == 1.0f);  // what an undriven knob writes
  close(evalCurve(d, 1.0), 20000.0, 1e-14);
  close(evalCurve(d, 0.0), -20000.0, 1e-14);
  // C1 at the joins (the slope on both sides of ±1 is the fine zone's: 5 per unit of travel).
  for (const double t : {0.3, 0.7}) {
    const double h = 1e-7;
    const double left = (evalCurve(d, t) - evalCurve(d, t - h)) / h, right = (evalCurve(d, t + h) - evalCurve(d, t)) / h;
    CHECK(std::fabs(left - 5.0) < 1e-4);
    CHECK(std::fabs(right - 5.0) < 1e-4);
  }
  CHECK(curvePresetFromName(curvePresetName(CurvePreset::Vca)) == CurvePreset::Vca);
  // The atom agrees with the evaluator (the Joined shape at float precision).
  kernelMatches(d, "vca default");
  kernelMatches(curvePreset(CurvePreset::Vca, -2.0, 50.0, 0.6), "vca -2 … 50");
  kernelMatches(curvePreset(CurvePreset::Vca, 20.0, 20000.0), "vca 20 … 20000");
}

TEST_CASE("curve shapes: every shape from its first point to its last, bend 0 straight") {
  for (uint32_t s = 0; s < kCurveShapeCount; ++s)
    for (const double bend : {-6.0, -1.0, 0.0, 0.7, 3.0}) {
      Curve c = curvePreset(CurvePreset::Linear, 10.0, 50.0);
      c.preset = CurvePreset::Custom;
      c.segments = 2;
      c.points[0] = {0.0, 0.2};
      c.points[1] = {0.3, 0.9};
      c.points[2] = {1.0, 0.1};
      c.shapes[0] = {static_cast<CurveShape>(s), bend};
      c.shapes[1] = {static_cast<CurveShape>(s), -bend};
      REQUIRE(validCurve(c));
      close(evalCurve(c, 0.0), 18.0, 1e-12);
      close(evalCurve(c, 0.3), 46.0, 1e-12);
      close(evalCurve(c, 1.0), 14.0, 1e-12);
      if (bend == 0.0) close(evalCurve(c, 0.15), 32.0, 1e-12);
      // Monotonic inside each segment.
      double prev = evalCurve(c, 0.0);
      for (double p = 0.005; p <= 0.3; p += 0.005) {
        const double v = evalCurve(c, p);
        CHECK(v >= prev - 1e-9);
        prev = v;
      }
    }
}

TEST_CASE("curve inverse: typed values give the nearest knob position") {
  // Monotonic presets: exact round trips.
  const Curve presets[] = {curvePreset(CurvePreset::Linear, -20.0, 300.0), curvePreset(CurvePreset::Octave, 20.0, 20000.0),
                           curvePreset(CurvePreset::Exp0, 0.0, 20000.0, 14.0), curvePreset(CurvePreset::Logarithmic, 1.0, 5.0),
                           dbCurve(std::pow(10.0, -70.0 / 20.0), 2.0)};
  for (const Curve& c : presets)
    for (double p = 0.0; p <= 1.0; p += 1.0 / 32.0) {
      const double v = evalCurve(c, p);
      const double back = curvePosition(c, v, 0.5);
      CHECK_MESSAGE(std::fabs(back - p) < 1e-10, "p " << p << " back " << back);
      close(evalCurve(c, back), v, 1e-12);
    }
  // A curve that goes up and back down: 0 → 100 at t = 0.5 → 0 at t = 1. The value 50 is reached twice (t = 0.25 and
  // t = 0.75): the one nearest the knob's current position wins.
  Curve tent = curvePreset(CurvePreset::Linear, 0.0, 100.0);
  tent.preset = CurvePreset::Custom;
  tent.segments = 2;
  tent.points[1] = {0.5, 1.0};
  tent.points[2] = {1.0, 0.0};
  REQUIRE(validCurve(tent));
  close(curvePosition(tent, 50.0, 0.1), 0.25);
  close(curvePosition(tent, 50.0, 0.4), 0.25);
  close(curvePosition(tent, 50.0, 0.6), 0.75);
  close(curvePosition(tent, 50.0, 0.99), 0.75);
  // Unreachable values snap to the nearest reachable one: above the peak → the peak; below → the nearer of the two
  // zero ends.
  close(curvePosition(tent, 140.0, 0.9), 0.5);
  CHECK(curvePosition(tent, -5.0, 0.2) == 0.0);
  CHECK(curvePosition(tent, -5.0, 0.8) == 1.0);
  // A flat segment: any position on it gives the value: the hint, clamped to it.
  Curve flat = tent;
  flat.points[1] = {0.5, 0.5};
  flat.points[2] = {1.0, 0.5};
  close(curvePosition(flat, 50.0, 0.8), 0.8);
  close(curvePosition(flat, 50.0, 0.1), 0.5);
  // A gain below a dB curve's floor (or 0): its minimum.
  const Curve g = dbCurve(std::pow(10.0, -70.0 / 20.0), 2.0);
  CHECK(curvePosition(g, 0.0, 0.7) == 0.0);
  CHECK(curvePosition(g, 1e-9, 0.7) == 0.0);
  CHECK(curvePosition(g, 10.0, 0.2) == 1.0);
  // Steps: a typed step value lands on a position that gives it exactly.
  Curve steps = curvePreset(CurvePreset::Linear, 0.0, 10.0);
  steps.step = 1.0;
  for (int k = 0; k <= 10; ++k) CHECK(evalCurve(steps, curvePosition(steps, k, 0.0)) == k);
  // A bool (0 / 1).
  Curve b = curvePreset(CurvePreset::Linear, 0.0, 1.0);
  b.step = 1.0;
  CHECK(curvePosition(b, 1.0, 0.0) == 1.0);
  CHECK(curvePosition(b, 0.0, 1.0) == 0.0);
}

TEST_CASE("curve classes: points, bends and the range are data; segments, shapes and flags are structural") {
  const Curve a = curvePreset(CurvePreset::Octave, 20.0, 20000.0);
  Curve b = curvePreset(CurvePreset::Octave, 50.0, 5000.0);
  b.shapes[0].bend = 2.0;
  CHECK(curveClass(a) == curveClass(b));
  CHECK(curveClass(a) != curveClass(curvePreset(CurvePreset::Linear, 20.0, 20000.0)));
  Curve w = a;
  w.mode = CurveMode::Wrap;
  CHECK(curveClass(a) != curveClass(w));
  Curve two = a;
  two.segments = 2;
  two.points[1] = {0.5, 0.5};
  two.points[2] = {1.0, 1.0};
  two.shapes[1] = two.shapes[0];
  CHECK(curveClass(a) != curveClass(two));
  CHECK(curveIsLinear(curvePreset(CurvePreset::Linear, 0.0, 1.0)));
  CHECK_FALSE(curveIsLinear(a));
  CHECK(curvePresetFromName("exp") == CurvePreset::Octave);
  CHECK(curvePresetFromName(curvePresetName(CurvePreset::Exp0)) == CurvePreset::Exp0);
  Curve bad = a;
  bad.points[1].t = 0.9;
  CHECK_FALSE(validCurve(bad));
}

TEST_CASE("curve atom: equals evalCurve to float rounding at every lane count; 1 lane == 8 lanes") {
  kernelMatches(curvePreset(CurvePreset::Linear, -20.0, 300.0), "linear");
  kernelMatches(curvePreset(CurvePreset::Octave, 20.0, 20000.0), "octave");
  kernelMatches(curvePreset(CurvePreset::Exp0, 0.0, 20000.0, 14.0), "exp0");
  kernelMatches(curvePreset(CurvePreset::Logarithmic, 0.001, 60.0), "logarithmic");
  kernelMatches(dbCurve(std::pow(10.0, -70.0 / 20.0), 2.0), "dB linear");
  kernelMatches(dbCurve(std::pow(10.0, -70.0 / 20.0), 2.0, CurvePreset::Exponential), "dB bent");
  Curve sgn = curvePreset(CurvePreset::Exp0, 0.0, 20000.0, 14.0);
  sgn.mode = CurveMode::Signed;
  kernelMatches(sgn, "signed");
  Curve wrap = curvePreset(CurvePreset::Linear, 0.0, 1.0);
  wrap.mode = CurveMode::Wrap;
  kernelMatches(wrap, "wrap");
  Curve steps = curvePreset(CurvePreset::Linear, 0.0, 10.0);
  steps.step = 1.0;
  kernelMatches(steps, "steps");
  std::mt19937 rng(7);
  std::uniform_real_distribution<double> u01(0.0, 1.0), bend(-8.0, 8.0);
  for (uint32_t s = 0; s <= kCurveShapeCount; ++s)
    for (const uint32_t n : {1u, 3u, 16u}) {
      Curve c = curvePreset(CurvePreset::Linear, -5.0, 40.0);
      c.preset = CurvePreset::Custom;
      c.segments = static_cast<uint8_t>(n);
      std::vector<double> ts{0.0, 1.0};
      for (uint32_t i = 1; i < n; ++i) ts.push_back(u01(rng) * 0.98 + 0.01);
      std::sort(ts.begin(), ts.end());
      for (uint32_t i = 0; i <= n; ++i) c.points[i] = {ts[i], u01(rng)};
      for (uint32_t i = 0; i < n; ++i)
        c.shapes[i] = {static_cast<CurveShape>(s < kCurveShapeCount ? s : rng() % kCurveShapeCount), bend(rng)};
      REQUIRE(validCurve(c));
      kernelMatches(c, std::string("shape ") + std::to_string(s) + " segments " + std::to_string(n));
    }
}

TEST_CASE("curve atom: convert 0 leaves out the output conversion; no slot is the identity") {
  const Curve g = dbCurve(std::pow(10.0, -70.0 / 20.0), 2.0);
  CurveSlot slot(compileCurve(g));
  const std::vector<float> x = {0.0f, 0.25f, 0.5f, 1.0f, 0.0f, 0.25f, 0.5f, 1.0f};
  const std::vector<float> units = runKernel(&slot, x, 1, false, 8, false);
  for (size_t i = 0; i < x.size(); ++i) CHECK(std::fabs(units[i] - evalCurveUnits(g, x[i])) <= 1e-4);
  const std::vector<float> same = runKernel(nullptr, x, 8, false, 1);
  for (size_t i = 0; i < x.size(); ++i) CHECK(same[i] == x[i]);
}

TEST_CASE("curve slot: publication swaps the data, equal data is a no-op, the old data is retired") {
  pq::EpochReclaimer reclaimer;
  const Curve a = curvePreset(CurvePreset::Linear, 0.0, 10.0);
  CurveSlot slot(compileCurve(a));
  const CurveData* first = slot.acquire();
  CHECK_FALSE(slot.publish(compileCurve(a), reclaimer));
  CHECK(slot.acquire() == first);
  const Curve b = curvePreset(CurvePreset::Linear, 0.0, 20.0);
  CHECK(slot.publish(compileCurve(b), reclaimer));
  CHECK(slot.acquire() != first);
  CHECK(sameData(slot.data(), compileCurve(b)));
  CHECK(reclaimer.pending() == 1);
  CHECK(reclaimer.collect() == 1);  // no reader registered: nothing holds it
}

TEST_CASE("curve detents (TX.detent): flagged points; a drag holds there and releases without a jump; a step lands") {
  Curve c = curvePreset(CurvePreset::Linear, 0.0, 1.0);
  // setCurveDetent splits a segment where no point is (the function unchanged), flags an existing point in place.
  REQUIRE(setCurveDetent(c, 0.5));
  CHECK(c.segments == 2);
  CHECK(c.points[1].t == 0.5);
  CHECK(c.points[1].detent);
  for (int i = 0; i <= 64; ++i) CHECK(evalCurve(c, i / 64.0) == doctest::Approx(i / 64.0).epsilon(1e-15));
  Curve plain = c;
  plain.points[1].detent = false;
  CHECK(curveClass(plain) == curveClass(c));          // not structural
  CHECK(sameData(compileCurve(plain), compileCurve(c)));  // and not the atom's data
  CHECK_FALSE(plain == c);                             // but a change of the curve (saved, undoable)
  CHECK_FALSE(setCurveDetent(c, 0.0));                 // the ends are never detents
  CHECK_FALSE(setCurveDetent(c, 1.0));
  REQUIRE(setCurveDetent(c, 0.25));
  std::array<double, kMaxCurveSegments + 1> d{};
  REQUIRE(curveDetents(c, d) == 2);
  CHECK(d[0] == 0.25);
  CHECK(d[1] == 0.5);
  REQUIRE(setCurveDetent(c, 0.25, false));
  CHECK(curveDetents(c, d) == 1);

  // The drag: zone 0.03. From 0.4 upward: free to 0.5, held over 0.06 of travel, then free again with that subtracted.
  const double z = 0.03;
  CHECK(detentDrag(c, 0.4, 0.05, z) == doctest::Approx(0.45));
  CHECK(detentDrag(c, 0.4, 0.1, z) == 0.5);
  CHECK(detentDrag(c, 0.4, 0.13, z) == 0.5);
  CHECK(detentDrag(c, 0.4, 0.16, z) == 0.5);
  CHECK(detentDrag(c, 0.4, 0.17, z) == doctest::Approx(0.51));
  // Continuous everywhere (no leap into or out of the zone), monotonic.
  double prev = detentDrag(c, 0.4, 0.0, z);
  for (int i = 1; i <= 4000; ++i) {
    const double p = detentDrag(c, 0.4, i * 1e-4, z);
    CHECK(p >= prev);
    CHECK(p - prev <= 1e-4 + 1e-12);
    prev = p;
  }
  // Downward the same, mirrored; a drag that starts on the detent is held for ±zone.
  CHECK(detentDrag(c, 0.6, -0.1, z) == 0.5);
  CHECK(detentDrag(c, 0.6, -0.17, z) == doctest::Approx(0.49));
  CHECK(detentDrag(c, 0.5, 0.02, z) == 0.5);
  CHECK(detentDrag(c, 0.5, -0.03, z) == 0.5);
  CHECK(detentDrag(c, 0.5, 0.05, z) == doctest::Approx(0.52));
  CHECK(detentDrag(c, 0.5, -0.05, z) == doctest::Approx(0.48));
  // A detent behind the drag does nothing; no detents: plain travel.
  CHECK(detentDrag(c, 0.6, 0.1, z) == doctest::Approx(0.7));
  CHECK(detentDrag(curvePreset(CurvePreset::Linear, 0.0, 1.0), 0.4, 0.13, z) == doctest::Approx(0.53));

  // A step across a detent lands on it; one that starts on it or stops short passes it by.
  CHECK(detentStep(c, 0.45, 0.55) == 0.5);
  CHECK(detentStep(c, 0.55, 0.45) == 0.5);
  CHECK(detentStep(c, 0.5, 0.51) == 0.51);
  CHECK(detentStep(c, 0.5, 0.49) == 0.49);
  CHECK(detentStep(c, 0.40, 0.49) == 0.49);
  CHECK(detentStep(c, 0.40, 0.50) == 0.50);

  // The VCA's law: ×1 and ×−1 are detents (its fine zone's edges).
  const Curve v = curvePreset(CurvePreset::Vca, -20000.0, 20000.0);
  REQUIRE(curveDetents(v, d) == 2);
  close(evalCurve(v, d[0]), -1.0, 1e-11);
  close(evalCurve(v, d[1]), 1.0, 1e-11);
}
