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
  int type = 0;           // 0 Analog Clip (the default, M4 review L11) … 8 Shaper
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

}  // namespace pq::views::devices::math
