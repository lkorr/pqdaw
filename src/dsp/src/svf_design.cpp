// EQ.1: the svf atom's designer and exact response (pq/dsp/svf_design.h has the conventions and the references).

#include "pq/dsp/svf_design.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace pq::dsp {

namespace {

using cd = std::complex<double>;
constexpr double kPi = std::numbers::pi;
constexpr double kSqrtHalf = 0.70710678118654752;
constexpr double kMatchedMin = 0.01;  // design pole frequency (rad/sample) below which a section is bilinear

struct Clean {
  SvfMode mode;
  bool matched;
  uint32_t n;
  double w0, q, db;
};

Clean sanitise(const SvfParams& p) noexcept {
  Clean c{};
  c.mode = static_cast<uint32_t>(p.mode) < kSvfModeCount ? p.mode : SvfMode::Lowpass;
  c.matched = p.prewarp != SvfPrewarp::Bilinear;
  c.n = svfSectionCount(c.mode, p.stages);
  const double sr = std::isfinite(p.sampleRate) && p.sampleRate > 0.0 ? p.sampleRate : 48000.0;
  const double fc = std::isnan(p.cutoffHz) ? 1000.0 : std::clamp(p.cutoffHz, 1e-3, 0.49 * sr);
  c.w0 = 2.0 * kPi * fc / sr;
  c.q = std::isnan(p.q) ? kSqrtHalf : std::clamp(p.q, 0.01, 100.0);
  c.db = std::isnan(p.gainDb) ? 0.0 : std::clamp(p.gainDb, -120.0, 120.0);
  return c;
}

// Butterworth-spread Q of section k (0-based) of n, the highest-Q one (k = n − 1) scaled by q / √½ (n = 1: q).
double spreadQ(uint32_t k, uint32_t n, double q) noexcept {
  if (n == 1) return q;
  const double qk = 1.0 / (2.0 * std::cos((2.0 * k + 1.0) * kPi / (4.0 * n)));
  return k + 1 == n ? qk * (q / kSqrtHalf) : qk;
}

// Section k of n in the cascade takes spread index 0, n − 1, 1, n − 2, … (lowest Q, highest, next lowest, …): the
// partial products of the sections then stay small, which keeps the float recursion's rounding noise, amplified by the
// sections after it, small (a 64-section Butterworth: max partial gain 2e4 instead of 7e8 either way sorted).
uint32_t interleaved(uint32_t k, uint32_t n) noexcept { return k % 2 == 0 ? k / 2 : n - 1 - k / 2; }

// Impulse-invariant poles of s² + s·wp/qp + wp² (Vicanek eq. 12) as the sums the designs need, computed without
// cancellation: P = 1 + a1 + a2, M = 1 − a1 + a2, a2, and 1 − a2.
struct Poles {
  double P, M, a2, omA2;
};
Poles matchedPoles(double wp, double qp) noexcept {
  const double z = 1.0 / (2.0 * qp);  // damping
  Poles r{};
  if (z < 1.0) {
    const double e = std::exp(-z * wp), om = -std::expm1(-z * wp);
    const double th = wp * std::sqrt(1.0 - z * z);
    const double sh = std::sin(0.5 * th), ch = std::cos(0.5 * th);
    r.P = om * om + 4.0 * e * sh * sh;
    r.M = om * om + 4.0 * e * ch * ch;
    r.a2 = e * e;
  } else {  // real poles e^(−α1), e^(−α2), α1 = (z − √(z²−1))·wp written as wp / (z + √(z²−1))
    const double s = std::sqrt(z * z - 1.0);
    const double al1 = wp / (z + s), al2 = (z + s) * wp;
    const double p1 = std::exp(-al1), p2 = std::exp(-al2);
    r.P = std::expm1(-al1) * std::expm1(-al2);
    r.M = (1.0 + p1) * (1.0 + p2);
    r.a2 = p1 * p2;
  }
  r.omA2 = -std::expm1(-2.0 * z * wp);
  return r;
}

// |analog prototype|² at ω (no inversion).
double protoMag2(const SvfProto& p, double w) noexcept {
  const double x = w / p.wp;
  const double nr = p.cLp - p.cHp * x * x, ni = p.cBp * x;
  const double dr = 1.0 - x * x, di = x / p.qp;
  return (nr * nr + ni * ni) / (dr * dr + di * di);
}

SvfSection identity(double wc, double qp) noexcept {
  SvfSection s;
  s.g = std::tan(0.5 * wc);
  s.k = 1.0 / qp;
  s.cHp = s.cBp = s.cLp = 0.0;
  s.cX = 1.0;
  return s;
}

// 1 / H in SVF form (svf_design.h): the poles become the zeros. Needs cLp, cHp > 0 (a boost bell / shelf).
SvfSection invert(const SvfSection& s) noexcept {
  const double cl = std::max(s.cLp, 1e-300), ch = std::max(s.cHp, 1e-300);
  const double r = std::sqrt(cl * ch);
  SvfSection o;
  o.g = s.g * std::sqrt(cl / ch);
  // The boost's zeros are minimum phase (Vicanek eq. 28), so k' > 0; a fit whose zeros landed on the unit circle (a
  // clamped W² + B2 at extreme Q) would give k' = 0, a marginal filter: keep Q' ≤ 1e5.
  o.k = std::max(s.cBp / r, 1e-5);
  o.cHp = 1.0 / ch;
  o.cBp = s.k / r;
  o.cLp = 1.0 / cl;
  o.cX = 0.0;
  return o;
}

SvfSection bilinear(const SvfProto& p) noexcept {
  SvfSection s;
  s.g = std::tan(0.5 * p.wc) * (p.wp / p.wc);
  s.k = 1.0 / p.qp;
  s.cHp = p.cHp;
  s.cBp = p.cBp;
  s.cLp = p.cLp;
  s.cX = 0.0;
  return s;
}

// Least-squares fit of B1, B2 (Vicanek eq. 25) to the prototype for a shelf, B0 fixed (exact DC gain).
void shelfFit(const SvfProto& p, const Poles& pl, double B0, double& B1, double& B2) noexcept {
  const double A0 = pl.P * pl.P, A1 = pl.M * pl.M, A2 = -4.0 * pl.a2;
  constexpr int kN = 16;
  const double lo = std::log(std::min(0.25 * p.wp, 0.5 * kPi)), hi = std::log(kPi);
  double s11 = 0, s12 = 0, s22 = 0, r1 = 0, r2 = 0;
  for (int i = 0; i < kN; ++i) {
    const double w = i + 1 == kN ? kPi : std::exp(lo + (hi - lo) * i / (kN - 1));
    const double sh = std::sin(0.5 * w), ch = std::cos(0.5 * w);
    const double f1 = sh * sh, f0 = ch * ch, f2 = 4.0 * f0 * f1;
    const double T = protoMag2(p, w) * (A0 * f0 + A1 * f1 + A2 * f2);
    const double u = f1 / T, v = f2 / T, b = 1.0 - B0 * f0 / T;
    s11 += u * u;
    s12 += u * v;
    s22 += v * v;
    r1 += u * b;
    r2 += v * b;
  }
  const double det = s11 * s22 - s12 * s12;
  if (det > 0.0 && std::isfinite(det)) {
    B1 = (r1 * s22 - r2 * s12) / det;
    B2 = (s11 * r2 - s12 * r1) / det;
  } else {
    B1 = protoMag2(p, kPi) * A1;
    B2 = 0.0;
  }
}

SvfSection matched(SvfMode mode, const SvfProto& p) noexcept {
  const Poles pl = matchedPoles(p.wp, p.qp);
  SvfSection s;
  s.g = std::sqrt(pl.P / pl.M);
  s.k = 2.0 * pl.omA2 / (s.g * pl.M);
  s.cX = 0.0;
  const double sh = std::sin(0.5 * p.wp), ch = std::cos(0.5 * p.wp);
  const double f1 = sh * sh, f0 = ch * ch, f2 = 4.0 * f0 * f1;  // φ1, φ0, φ2 at the pole frequency
  const double A0 = pl.P * pl.P, A1 = pl.M * pl.M, A2 = -4.0 * pl.a2;
  const double Aw = A0 * f0 + A1 * f1 + A2 * f2;  // |A(e^jωp)|²
  const double gM = s.g * pl.M;
  // From B0, B1, B2 with Vicanek's minimum-phase root (eq. 29): cLp = √B0 / P, cHp = √B1 / M, cBp = 2·√(W² + B2) / (g·M).
  auto fromB = [&](double B0, double B1, double B2) {
    B1 = std::max(B1, 0.0);
    const double r0 = std::sqrt(std::max(B0, 0.0)), r1 = std::sqrt(B1);
    const double W = 0.5 * (r0 + r1);
    s.cLp = r0 / pl.P;
    s.cHp = r1 / pl.M;
    s.cBp = 2.0 * std::sqrt(std::max(W * W + B2, 0.0)) / gM;
  };
  switch (mode) {
    case SvfMode::Lowpass: {  // §4.1: unity at DC, |H(ωp)| = Q, b2 = 0
      const double B0 = A0;
      fromB(B0, (Aw * p.qp * p.qp - B0 * f0) / f1, 0.0);
      break;
    }
    case SvfMode::Highpass:  // §4.2: double zero at DC, |H(ωp)| = Q
      s.cLp = 0.0;
      s.cBp = 0.0;
      s.cHp = std::sqrt(Aw) * p.qp / (f1 * pl.M);
      break;
    case SvfMode::Bandpass: {  // §4.3: zero at DC, unity maximum at ωp
      const double R2 = -A0 + A1 + 4.0 * (f0 - f1) * A2;
      const double B2 = (Aw - R2 * f1) / (4.0 * f1 * f1);
      const double B1 = std::max(R2 + 4.0 * (f1 - f0) * B2, 0.0);
      s.cLp = 0.0;
      s.cHp = std::sqrt(B1) / pl.M;
      s.cBp = 2.0 * std::sqrt(std::max(B2 + 0.25 * B1, 0.0)) / gM;
      break;
    }
    case SvfMode::Notch:  // zeros on the unit circle at ωp, unity at DC: b ∝ (1, −2·cos ωp, 1)
      s.cLp = 1.0;
      s.cBp = 0.0;
      s.cHp = (pl.P / pl.M) * (f0 / f1);
      break;
    case SvfMode::Allpass:  // the mirrored poles: |H| = 1 exactly
      s.cLp = 1.0;
      s.cHp = 1.0;
      s.cBp = -s.k;
      break;
    case SvfMode::Bell: {  // §4.4 (boost): unity at DC, |H(ωp)| = G, a maximum at ωp
      const double G = p.cBp * p.qp;  // = A², the prototype's peak
      const double G2 = G * G;
      const double B0 = A0;
      const double R1 = Aw * G2, R2 = (-A0 + A1 + 4.0 * (f0 - f1) * A2) * G2;
      const double B2 = (R1 - R2 * f1 - B0) / (4.0 * f1 * f1);
      fromB(B0, R2 + B0 + 4.0 * (f1 - f0) * B2, B2);
      break;
    }
    case SvfMode::LowShelf:
    case SvfMode::HighShelf: {
      const double B0 = p.cLp * p.cLp * A0;
      double B1 = 0.0, B2 = 0.0;
      shelfFit(p, pl, B0, B1, B2);
      fromB(B0, B1, B2);
      break;
    }
  }
  return s;
}

}  // namespace

SvfParams svfParamsFromInputs(float cutoff, float q, float gain, float mode, float stages, float prewarp,
                              double sampleRate) noexcept {
  auto roundClamp = [](float v, uint32_t lo, uint32_t hi) -> uint32_t {
    if (!(v >= static_cast<float>(lo))) return lo;  // NaN too
    if (v >= static_cast<float>(hi)) return hi;
    return static_cast<uint32_t>(std::lround(v));
  };
  SvfParams p;
  p.mode = static_cast<SvfMode>(roundClamp(mode, 0, kSvfModeCount - 1));
  p.prewarp = prewarp >= 0.5f ? SvfPrewarp::Bilinear : SvfPrewarp::Matched;
  p.stages = roundClamp(stages, 1, kSvfMaxStages);
  p.cutoffHz = cutoff;
  p.q = q;
  p.gainDb = std::isnan(gain) ? 0.0 : !(gain > 0.0f) ? -120.0 : 20.0 * std::log10(static_cast<double>(gain));
  p.sampleRate = sampleRate;
  return p;
}

uint32_t svfSectionCount(SvfMode mode, uint32_t stages) noexcept {
  if (mode == SvfMode::Bell || mode == SvfMode::Notch) return 1;
  return std::clamp<uint32_t>(stages, 1u, kSvfMaxStages);
}

uint32_t svfPrototype(const SvfParams& in, SvfProto* out) noexcept {
  const Clean c = sanitise(in);
  for (uint32_t k = 0; k < c.n; ++k) {
    SvfProto& p = out[k];
    p = SvfProto{};
    p.wp = p.wc = c.w0;
    switch (c.mode) {
      case SvfMode::Lowpass:
      case SvfMode::Highpass:
        p.qp = spreadQ(interleaved(k, c.n), c.n, c.q);
        p.cHp = c.mode == SvfMode::Highpass ? 1.0 : 0.0;
        p.cLp = c.mode == SvfMode::Lowpass ? 1.0 : 0.0;
        break;
      case SvfMode::Bandpass:
        p.qp = c.q;
        p.cHp = 0.0;
        p.cBp = 1.0 / c.q;
        p.cLp = 0.0;
        break;
      case SvfMode::Notch:
        p.qp = c.q;
        p.cHp = 1.0;
        p.cLp = 1.0;
        break;
      case SvfMode::Allpass:
        p.qp = c.q;
        p.cHp = 1.0;
        p.cBp = -1.0 / c.q;
        p.cLp = 1.0;
        break;
      case SvfMode::Bell: {  // the boost of |dB| (poles at ωc, Q·A), inverted for a cut
        const double A = std::pow(10.0, std::fabs(c.db) / 40.0);
        p.qp = c.q * A;
        p.cHp = 1.0;
        p.cBp = A / c.q;
        p.cLp = 1.0;
        p.invert = c.db < 0.0;
        break;
      }
      case SvfMode::LowShelf:
      case SvfMode::HighShelf: {  // the variant with its poles at ωc / √A: a low-shelf boost or a high-shelf cut
        const double db = c.db / c.n;
        const double A = std::pow(10.0, std::fabs(db) / 40.0);
        p.qp = spreadQ(interleaved(k, c.n), c.n, c.q);
        p.wp = c.w0 / std::sqrt(A);
        if (c.mode == SvfMode::LowShelf) {
          p.cHp = 1.0;
          p.cBp = A / p.qp;
          p.cLp = A * A;
          p.invert = db < 0.0;
        } else {
          p.cHp = 1.0 / (A * A);
          p.cBp = 1.0 / (A * p.qp);
          p.cLp = 1.0;
          p.invert = db > 0.0;
        }
        break;
      }
    }
  }
  return c.n;
}

uint32_t svfDesign(const SvfParams& in, SvfSection* out) noexcept {
  const Clean c = sanitise(in);
  SvfProto protos[kSvfMaxStages];
  const uint32_t n = svfPrototype(in, protos);
  const bool gainMode = c.mode == SvfMode::Bell || c.mode == SvfMode::LowShelf || c.mode == SvfMode::HighShelf;
  for (uint32_t k = 0; k < n; ++k) {
    const SvfProto& p = protos[k];
    if (gainMode && c.db == 0.0) {
      out[k] = identity(p.wc, p.qp);
      continue;
    }
    const SvfSection s = c.matched && p.wp >= kMatchedMin ? matched(c.mode, p) : bilinear(p);
    out[k] = p.invert ? invert(s) : s;
  }
  return n;
}

SvfKernelCoefs svfKernelCoefs(const SvfSection& s) noexcept {
  SvfKernelCoefs c;
  if (s.cHp == 0.0 && s.cBp == 0.0 && s.cLp == 0.0) {  // the identity (or a plain gain): no recursion
    c.m0 = static_cast<float>(s.cX);
    return c;
  }
  const double gk = s.g * (s.g + s.k);
  const double a1 = 1.0 / (1.0 + gk);
  const double b1 = 2.0 * s.g * a1;
  c.d1 = static_cast<float>(-2.0 * gk * a1);
  c.b1 = static_cast<float>(b1);
  c.b2 = static_cast<float>(b1 * s.g);
  c.m0 = static_cast<float>(s.cHp + s.cX);
  c.h1 = static_cast<float>(0.5 * (s.cBp - s.k * s.cHp));
  c.h2 = static_cast<float>(0.5 * (s.cLp - s.cHp));
  return c;
}

std::complex<double> svfSectionResponse(const SvfKernelCoefs& c, double omega) noexcept {
  const double d1 = c.d1, b1 = c.b1, b2 = c.b2, m0 = c.m0, h1 = c.h1, h2 = c.h2;
  const double sh = std::sin(0.5 * omega);
  const cd zm1(-2.0 * sh * sh, std::sin(omega));  // z − 1 without cancellation
  const cd det = (zm1 - d1) * (zm1 + b2) + b1 * b1;
  const cd s1 = b1 * zm1 / det;
  const cd s2 = (b1 * b1 + (zm1 - d1) * b2) / det;
  const double C1 = h1 * (2.0 + d1) + h2 * b1, C2 = -h1 * b1 + h2 * (2.0 - b2), D = m0 + h1 * b1 + h2 * b2;
  return D + C1 * s1 + C2 * s2;
}

std::complex<double> svfResponse(std::span<const SvfSection> sections, double omega) noexcept {
  cd h(1.0, 0.0);
  for (const SvfSection& s : sections) h *= svfSectionResponse(svfKernelCoefs(s), omega);
  return h;
}

std::complex<double> svfResponse(const SvfParams& p, double omega) noexcept {
  SvfSection s[kSvfMaxStages];
  const uint32_t n = svfDesign(p, s);
  return svfResponse(std::span<const SvfSection>(s, n), omega);
}

std::complex<double> svfAnalogResponse(std::span<const SvfProto> sections, double omega) noexcept {
  cd h(1.0, 0.0);
  for (const SvfProto& p : sections) {
    const cd S(0.0, omega / p.wp);
    const cd v = (p.cHp * S * S + p.cBp * S + p.cLp) / (S * S + S / p.qp + 1.0);
    h *= p.invert ? 1.0 / v : v;
  }
  return h;
}

std::complex<double> svfAnalogResponse(const SvfParams& p, double omega) noexcept {
  SvfProto s[kSvfMaxStages];
  const uint32_t n = svfPrototype(p, s);
  return svfAnalogResponse(std::span<const SvfProto>(s, n), omega);
}

}  // namespace pq::dsp
