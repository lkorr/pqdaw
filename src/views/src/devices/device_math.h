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
  constexpr double kB1 = 1.9615705608064609;  // the 48s: sections 1–3 Butterworth-8, section 4 carries q (M4 review H3)
  EqCoef c{};
  switch (b.type) {
    case kLowCut48: c = {w, kB1, 1, -kB1, -1, 0, 0, 0}; break;
    case kLowCut12: c = {w, kc, 1, -kc, -1, 0, 0, 0}; break;
    case kLowShelf: c = {w / sA, kb, 1, kb * (A - 1), ge - 1, 0, 0, 0}; break;
    case kBell: c = {w, kb / A, 1, kb / A * (ge - 1), 0, 0, 0, 0}; break;
    case kNotch: c = {w, kc, 1, -kc, 0, 0, 0, 0}; break;
    case kHighShelf: c = {w * sA, kb, ge, kb * (1 - A) * A, 1 - ge, 0, 0, 0}; break;
    case kHighCut12: c = {w, kc, 0, 0, 1, 0, 0, 0}; break;
    default: c = {w, kB1, 0, 0, 1, 0, 0, 0}; break;
  }
  c.k2 = 1.6629392246050905;
  c.k3 = 1.1111404660392046;
  c.k4 = 0.27589937928294295 * kc;
  return c;
}

// The digital response of one band at f Hz: the TPT SVF is the bilinear transform of the analog prototype, so
// H(e^jω) = H_analog(s) with s = j·tan(π f / (os·sr)) / g.
// The prewarped frequency of f: tan(π f / (os·sr)) (one per display column, shared by the bands).
inline double eqWarp(double f, double sr, double os) { return std::tan(kPi * std::min(f, 0.4999 * os * sr) / (os * sr)); }

inline std::complex<double> eqBandResponseWarped(const EqBand& b, const EqCoef& c, double wf) {
  if (!b.on) return 1.0;
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

inline std::complex<double> eqBandResponse(const EqBand& b, const EqCoef& c, double f, double sr, double os) {
  return eqBandResponseWarped(b, c, eqWarp(f, sr, os));
}

// ---- Auto Filter (auto_filter.pqp: the small-signal response of the unmodulated filter, and the LFO shapes) ----------
enum AfType : int { kAfLowpass = 0, kAfHighpass, kAfBandpass, kAfNotch, kAfMorph, kAfDj, kAfComb, kAfResampling, kAfNotchLp, kAfVowel };

struct AutoFilterState {
  int type = kAfLowpass, circuit = 0, slope = 0;  // circuit 0 Clean, 1 OSR, 2 MS2, 3 SMP, 4 PRD; slope 0: 12 dB, 1: 24 dB
  double freq = 1000.0, res = 0.3, morph = 0.0, dj = 0.0;
};

// The cutoff the filter runs at without modulation (the DJ knob's for the DJ type), clamped as the device clamps it.
inline double autoFilterCutoff(const AutoFilterState& s, double sr) {
  double base = s.freq;
  if (s.type == kAfDj) base = s.dj < 0.0 ? 20000.0 * std::exp(6.684611727667927 * s.dj) : 15.0 * std::exp(6.907755278982137 * s.dj);
  return std::clamp(base, 10.0, 0.49 * sr);
}

// The response at f Hz for small signals (the nonlinear models are linear there: tanh(v) ≈ v, below the OSR diode's
// knee). The TPT filters are the bilinear transforms of their analog prototypes: s = j·tan(πf/sr)/tan(πfc/sr).
// Resampling: the sample-and-hold's in-band (zero-order hold) magnitude at the new rate 2·fc.
inline std::complex<double> autoFilterResponse(const AutoFilterState& st, double f, double sr) {
  using C = std::complex<double>;
  const double fc = autoFilterCutoff(st, sr);
  const double fw = std::tan(kPi * std::min(f, 0.4999 * sr) / sr);
  auto sAt = [&](double cut) { return C(0.0, fw / std::tan(kPi * std::clamp(cut, 10.0, 0.49 * sr) / sr)); };
  const C s = sAt(fc);
  const int type = st.type;
  if (type <= kAfDj) {
    const double pm = 4.0 * st.morph;
    const double hLP = std::max(0.0, 1.0 - std::fabs(pm)) + std::max(0.0, pm - 3.0), hBP = std::max(0.0, 1.0 - std::fabs(pm - 1.0));
    const double hHP = std::max(0.0, 1.0 - std::fabs(pm - 2.0)), hN = std::max(0.0, 1.0 - std::fabs(pm - 3.0));
    const double djHP = st.dj > 0.0 ? 1.0 : 0.0;
    const double w[6][4] = {{1, 0, 0, 0}, {0, 0, 1, 0}, {0, 1, 0, 0}, {0, 0, 0, 1}, {hLP, hBP, hHP, hN}, {1 - djHP, 0, djHP, 0}};
    const double wLP = w[type][0], wBP = w[type][1], wHP = w[type][2], wN = w[type][3], hn = wHP + wN;
    const int model = type > kAfMorph ? 0 : st.circuit;
    C h;
    if (model == 4) {  // PRD: the ladder's pole mixing over 1 + k·G⁴
      const double k = 4.0 * st.res;
      const C G = 1.0 / (1.0 + s);
      double c[5];
      if (st.slope) {
        c[0] = hn, c[1] = -4 * hn, c[2] = 4 * wBP + 6 * wHP + 8 * wN, c[3] = -8 * wBP - 4 * wHP - 8 * wN, c[4] = wLP + 4 * wBP + wHP + 4 * wN;
      } else {
        c[0] = hn, c[1] = 2 * wBP - 2 * hn, c[2] = wLP - 2 * wBP + wHP + 2 * wN, c[3] = 0, c[4] = 0;
      }
      C num = 0.0, gn = 1.0;
      for (double ci : c) {
        num += ci * gn;
        gn *= G;
      }
      h = num / (1.0 + k * G * G * G * G);
    } else {  // the SVF models: (hn·D + (kb·wBP − k·hn)·s + wLP − wHP) / D per section, D = s² + k·s + 1
      const bool clean = model == 0;
      const double k12 = clean ? std::max(std::sqrt(2.0) * (1.0 - 0.8 * st.res), 0.02) : std::sqrt(2.0) * (1.0 - st.res);
      const double kB = st.slope ? 0.541196100146197 * k12 : k12;
      const double kbB = clean ? kB : std::max(kB, 0.25);
      auto sec = [&](double k, double kb) {
        const C D = s * s + k * s + 1.0;
        return (hn * D + (kb * wBP - k * hn) * s + (wLP - wHP)) / D;
      };
      h = sec(kB, kbB);
      if (st.slope) h *= sec(1.8477590650225735, 1.8477590650225735);
    }
    if (type == kAfDj) h = 1.0 + std::min(1.0, 20.0 * std::fabs(st.dj)) * (h - 1.0);
    return h;
  }
  if (type == kAfComb) {
    const double fb = 0.792 * st.res, T = sr / std::clamp(fc, 20.0, 10000.0);
    return std::sqrt(1.0 - fb * fb) / (1.0 - fb * std::polar(1.0, -2.0 * kPi * f / sr * T));
  }
  if (type == kAfResampling) {
    const double u = kPi * f / std::min(2.0 * fc, 0.45 * sr);
    return u < 1e-9 ? 1.0 : std::sin(u) / u;
  }
  if (type == kAfNotchLp) {
    const double k = std::max(std::sqrt(2.0) * (1.0 - 0.8 * st.res), 0.02);
    const C s2 = sAt(std::min(2.0 * fc, 0.49 * sr));
    return (s * s + 1.0) / (s * s + k * s + 1.0) / (s2 * s2 + std::sqrt(2.0) * s2 + 1.0);
  }
  // Vowel: three formant band-passes, A E I O U by morph, shifted by √(fc / 1 kHz).
  const double q4 = 4.0 * st.morph;
  const int i = static_cast<int>(std::min(std::floor(q4), 3.0));
  const double fr = q4 - i;
  static constexpr double F[3][5] = {{730, 530, 270, 570, 300}, {1090, 1840, 2290, 840, 870}, {2440, 2480, 3010, 2410, 2240}};
  const double k = 1.0 / (3.0 + 9.0 * st.res), sh = std::sqrt(fc * 0.001), gains[3] = {1.0, 0.5, 0.25};
  C h = 0.0;
  for (int j = 0; j < 3; ++j) {
    const double fj = std::clamp((F[j][i] + fr * (F[j][i + 1] - F[j][i])) * sh, 20.0, 0.45 * sr);
    const C sj = sAt(fj);
    h += gains[j] * k * sj / (sj * sj + k * sj + 1.0);
  }
  return h;
}

// The LFO shapes (af_shape) at phase ph in [0, 1); `cycle` picks the random shape's held value (a fixed pattern
// for the display: the device draws its values from noise).
inline double autoFilterLfo(int shape, double ph, int cycle = 0) {
  auto frac = [](double x) { return x - std::floor(x); };
  switch (shape) {
    case 0: return std::sin(2.0 * kPi * ph);
    case 1: return 4.0 * std::fabs(frac(ph + 0.75) - 0.5) - 1.0;
    case 2: return 2.0 * frac(ph + 0.5) - 1.0;
    case 3: return 1.0 - 2.0 * frac(ph + 0.5);
    case 4: return ph < 0.5 ? 1.0 : -1.0;
    default: {
      static constexpr double kSteps[8] = {0.35, -0.7, 0.9, -0.15, 0.55, -0.95, 0.1, -0.45};
      return kSteps[static_cast<unsigned>(cycle) % 8u];
    }
  }
}

}  // namespace pq::views::devices::math
