#pragma once

// What the device displays compute (T4.6 panels for T4.9 / T4.10; private to pq_views): the Saturator's static transfer
// curve and EQ Eight's magnitude response, from the same formulas as stdlib/devices/saturator.pqp and eq_eight.pqp
// (checked against the DSP references in tests/views/test_device_panels.cpp). In double: a display, not the signal.

#include <algorithm>
#include <cmath>
#include <complex>

namespace pq::views::devices::math {

inline constexpr double kPi = 3.14159265358979323846;

// ---- Saturator (saturator.pqp: sat_shape, then dry/wet) --------------------------------------------------------------
struct SaturatorState {
  int type = 3;           // 0 Analog Clip … 8 Shaper
  bool softClip = false;
  double drive = 1.0, output = 1.0, dryWet = 1.0;  // linear
  double wsDrive = 0.5, wsLin = 0.5, wsCurve = 0.5, wsDamp = 0.0, wsDepth = 0.0, wsPeriod = 0.0;  // fractions
};

// The curve of `type` at v (sat_shape without the output gain and soft clip).
inline double saturatorCurve(const SaturatorState& s, double v) {
  switch (s.type) {
    case 0: {
      const double c = std::clamp(v, -1.5, 1.5);
      return c - 4.0 / 27.0 * c * c * c;
    }
    case 1: return std::sin(std::clamp(v, -kPi / 2, kPi / 2));
    case 2: {
      const double a = std::fabs(v);
      return v * (a + 2.0) / (v * v + a + 1.0);
    }
    case 3: return std::tanh(v);
    case 4: return v / std::sqrt(std::sqrt(1.0 + v * v * v * v));
    case 5: return std::sin(v);
    case 6: return std::clamp(v, -1.0, 1.0);
    case 7: {
      const double t = std::clamp(v, -1.0, 1.0);
      const double wf = 0.5 + 7.5 * s.wsPeriod;
      const double wg = 0.01 * s.wsDamp * s.wsDamp + 1e-30;
      const double shape = s.wsLin * v + s.wsCurve * (1.5 * t - 0.5 * t * t * t) + s.wsDepth * std::sin(2.0 * kPi * wf * t);
      const double gate = (v * v + 1e-30) / (v * v + wg);
      return v + s.wsDrive * (shape * gate - v);
    }
    default: return std::clamp(v, -1.0, 1.0);  // the built-in identity shaper on [−1, 1]
  }
}

// Output for an input sample x (the colour EQ is frequency dependent and not part of the static curve).
inline double saturatorTransfer(const SaturatorState& s, double x) {
  const double g = saturatorCurve(s, x * s.drive) * s.output;
  const double w = s.softClip ? std::tanh(g) : g;
  return x + s.dryWet * (w - x);
}

// ---- EQ Eight (eq_eight.pqp: eq8_coef, eq8_band) ---------------------------------------------------------------------
enum EqType : int { kLowCut48 = 0, kLowCut12, kLowShelf, kBell, kNotch, kHighShelf, kHighCut12, kHighCut48 };

struct EqBand {
  bool on = false;
  int type = kBell;
  double freq = 1000.0, gain = 1.0, q = 0.7071;  // gain linear
};

struct EqCoef {
  double g, k, m0, m1, m2, k2, k3, k4;
};

inline EqCoef eqCoef(const EqBand& b, bool adaptive, double scale, double sr, double os) {
  const double ge = std::pow(b.gain, scale);
  const double A = std::sqrt(ge);
  const double qe = adaptive ? b.q * std::sqrt(std::max(ge, 1.0 / ge)) : b.q;
  const double w = std::tan(kPi * std::clamp(b.freq, 0.0, 0.49 * sr) / (os * sr));
  const double kc = 1.0 / std::max(b.q, 0.01), kb = 1.0 / std::max(qe, 0.01), sA = std::sqrt(A);
  constexpr double kB1 = 1.3870398453221475;
  EqCoef c{};
  switch (b.type) {
    case kLowCut48: c = {w, kB1 * kc, 1, -kB1 * kc, -1, 0, 0, 0}; break;
    case kLowCut12: c = {w, kc, 1, -kc, -1, 0, 0, 0}; break;
    case kLowShelf: c = {w / sA, kb, 1, kb * (A - 1), ge - 1, 0, 0, 0}; break;
    case kBell: c = {w, kb / A, 1, kb / A * (ge - 1), 0, 0, 0, 0}; break;
    case kNotch: c = {w, kc, 1, -kc, 0, 0, 0, 0}; break;
    case kHighShelf: c = {w * sA, kb, ge, kb * (1 - A) * A, 1 - ge, 0, 0, 0}; break;
    case kHighCut12: c = {w, kc, 0, 0, 1, 0, 0, 0}; break;
    default: c = {w, kB1 * kc, 0, 0, 1, 0, 0, 0}; break;
  }
  c.k2 = 1.1758756024193588 * kc;
  c.k3 = 0.7856949583871023 * kc;
  c.k4 = 0.2758993792829431 * kc;
  return c;
}

// The digital response of one band at f Hz: the TPT SVF is the bilinear transform of the analog prototype, so
// H(e^jω) = H_analog(s) with s = j·tan(π f / (os·sr)) / g.
inline std::complex<double> eqBandResponse(const EqBand& b, const EqCoef& c, double f, double sr, double os) {
  if (!b.on) return 1.0;
  const double wf = std::tan(kPi * std::min(f, 0.4999 * os * sr) / (os * sr));
  const std::complex<double> s(0.0, wf / c.g);
  auto section = [&](double k, double m0, double m1, double m2) {
    const std::complex<double> den = s * s + k * s + 1.0;
    return m0 + (m1 * s + m2) / den;
  };
  std::complex<double> h = section(c.k, c.m0, c.m1, c.m2);
  if (b.type == kLowCut48 || b.type == kHighCut48) {
    const bool hp = b.type == kLowCut48;
    for (double k : {c.k2, c.k3, c.k4}) h *= hp ? section(k, 1, -k, -1) : section(k, 0, 0, 1);
  }
  return h;
}

}  // namespace pq::views::devices::math
