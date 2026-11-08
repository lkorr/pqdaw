#pragma once

// EQ.1: the `svf` atom's coefficient designer and its exact response (docs/plans/EQ.md "The svf atom").
//
// One section is the TPT state-variable filter (Simper / Zavalishin; stdlib/filters/svf.pqp's svf_core, run in the
// shallow form of filters/svf_fast.pqp) with an OUTPUT MIX
//   y = cHp·hp + cBp·bp + cLp·lp + cX·x        (bp = the band output v1, peak gain 1/k; lp = v2; hp = x − k·bp − lp)
// so it realises any 2nd-order numerator over the SVF's poles. As an analog prototype in S = s / ωp (ωp = the pole
// frequency) a section is H(S) = (cHp·S² + cBp·S + cLp) / (S² + S/Qp + 1) (+ cX), and the SVF is its bilinear
// transform with g = tan(ω/2)-prewarped integrator gain and k = 1/Qp.
//
// Modes (gain in dB, A = 10^(dB/40); Q the user's):
//   Lowpass  1 / (S² + S/Q + 1)                  Highpass  S² / (…)           Bandpass (S/Q) / (…) (unity at fc)
//   Notch    (S² + 1) / (…)                      Allpass   (S² − S/Q + 1) / (…)
//   Bell     (S² + S·A/Q + 1) / (S² + S/(A·Q) + 1)  (peak A² at fc, unity at DC and ∞; RBJ / Vicanek's prototype)
//   LowShelf (S² + S·A/Q + A²) / (S² + S/Q + 1) with ωp = ωc/√A   (DC gain A², ∞: 1, A at fc)
//   HighShelf (A²·S² + S·A/Q + 1) / (S² + S/Q + 1) with ωp = ωc·√A (DC 1, ∞: A²)
// Stages (N, 1 … 64; Bell and Notch always 1): N cascaded sections.
//   Lowpass / Highpass: Butterworth-spread section Qs, Q_k = 1 / (2·cos((2k−1)·π / 4N)) (k = 1 … N): −3 dB at fc,
//   12·N dB/oct; the user's Q scales the highest-Q section by Q / √½ (N = 1: plain Q).
//   LowShelf / HighShelf: gain / N per section, the same spread Qs (total gain stays, N sets the steepness).
//   Bandpass / Allpass: N identical sections at Q.
// Prewarp:
//   Bilinear: the plain Simper SVF of each prototype, prewarped at fc (g = tan(π·fc/sr)·ωp/ωc). Cramps near Nyquist.
//   Matched (default): each section's poles are the analog prototype's mapped by impulse invariance (z = e^s) and its
//   numerator is magnitude-matched (M. Vicanek, "Matched Second Order Digital Filters", 2016): lowpass, highpass,
//   bandpass and bell use the paper's §4.1–4.4 conditions; notch puts its zeros exactly on the unit circle at fc (unity
//   at DC); allpass mirrors its poles (exact unity magnitude); shelves (not in the paper) fit |B|² of eq. (25) by
//   weighted least squares to the prototype on 16 log-spaced points up to Nyquist with B0 exact at DC (QUESTIONS
//   Q?-EQ.1-a). A cut bell / shelf whose poles would sit above its zeros is designed as the boost with its poles low
//   and inverted (poles ↔ zeros), which keeps every designed pole below fc and fits better. The digital biquad is then
//   written back as SVF g, k and the output mix (g² = (1+a1+a2)/(1−a1+a2), k = 2(1−a2)/(g·(1−a1+a2)); cLp = H(1),
//   cHp = H(−1), cBp from b0 − b2). Below a design pole frequency of 0.01 rad/sample (76 Hz at 48 kHz) a section is
//   bilinear: the two agree there to < 1e−5 in frequency and the matched formulas lose precision as ω → 0.
// Inputs are sanitised: cutoff clamped to [1e−3 Hz, 0.49·sr] (NaN: 1 kHz), Q to [0.01, 100] (NaN: √½), gain to
// [−120, 120] dB (NaN: 0). A Bell / shelf section at exactly 0 dB is the identity (cX = 1, no recursion).
//
// Every function is noexcept and allocation-free (the atom designs on the audio thread when its inputs change): RT-safe.

#include <complex>
#include <cstdint>
#include <span>

namespace pq::dsp {

enum class SvfMode : uint8_t { Lowpass, Highpass, Bandpass, Notch, Bell, LowShelf, HighShelf, Allpass };
inline constexpr uint32_t kSvfModeCount = 8;
enum class SvfPrewarp : uint8_t { Matched, Bilinear };
inline constexpr uint32_t kSvfMaxStages = 64;

struct SvfParams {
  SvfMode mode = SvfMode::Lowpass;
  SvfPrewarp prewarp = SvfPrewarp::Matched;
  uint32_t stages = 1;          // clamped to [1, 64]; Bell and Notch: 1
  double cutoffHz = 1000.0;
  double q = 0.70710678118654752;
  double gainDb = 0.0;          // Bell and shelves only
  double sampleRate = 48000.0;
};

// One section in analog-prototype form: H(S) = (cHp·S² + cBp·S + cLp) / (S² + S/qp + 1), S = jω / wp (ω, wp in
// radians per sample); `invert`: the section is 1 / H (a cut designed as its boost). Plain aggregates (no member
// initialisers), so the designer's 64-entry arrays cost nothing to declare on the audio thread.
struct SvfProto {
  double wp, qp;
  double cHp, cBp, cLp;
  double wc;  // the reference cutoff the bilinear form prewarps at (radians per sample)
  bool invert;
};

// One designed section: the SVF's g and k and its output mix (see the header comment). A plain aggregate.
struct SvfSection {
  double g, k;
  double cHp, cBp, cLp, cX;
};

// What the kernel runs per section (the shallow form; svf_fast.pqp): v = x − ic2, n1 = ic1 + d1·ic1 + b1·v,
// n2 = ic2 + b1·ic1 + b2·v, y = m0·x + h1·(ic1 + n1) + h2·(ic2 + n2), then ic1 = n1, ic2 = n2.
struct SvfKernelCoefs {
  float d1 = 0.0f, b1 = 0.0f, b2 = 0.0f;
  float m0 = 1.0f, h1 = 0.0f, h2 = 0.0f;
};

// The design parameters the atom uses for its input values (exactly the kernel's conversion, so a response evaluator
// reading the atom's inputs designs the same filter): mode rounded and clamped to 0 … 7, stages to 1 … 64, prewarp
// ≥ 0.5 bilinear (else, NaN included, matched), gain a linear amplitude → dB (≤ 0: −120 dB; NaN: 0 dB; 0 dB for the
// modes without a gain). RT-safe.
SvfParams svfParamsFromInputs(float cutoff, float q, float gain, float mode, float stages, float prewarp,
                              double sampleRate) noexcept;

// Sections of a mode at `stages` (1 … 64; Bell, Notch: 1). RT-safe.
uint32_t svfSectionCount(SvfMode mode, uint32_t stages) noexcept;

// The target analog sections (after sanitising `p`), as the matched design builds them (a cut bell / shelf: its boost,
// invert set). Writes svfSectionCount() entries to `out` (room for kSvfMaxStages) and returns the count. RT-safe.
uint32_t svfPrototype(const SvfParams& p, SvfProto* out) noexcept;

// The design: writes svfSectionCount() sections to `out` (room for kSvfMaxStages), returns the count. RT-safe.
uint32_t svfDesign(const SvfParams& p, SvfSection* out) noexcept;

// The float coefficients the kernel runs for a section (rounded once from double). RT-safe.
SvfKernelCoefs svfKernelCoefs(const SvfSection& s) noexcept;

// Exact H(e^jω) of one section as the kernel runs it (its float coefficients, evaluated in double through the state
// space of the recursion above), and of a cascade (the product). ω in radians per sample. RT-safe.
// EQ.1b: through the ω-independent form below (svfSectionPoly), so a response evaluator that keeps a design's polys
// (graph's svf rule) computes exactly these values.
std::complex<double> svfSectionResponse(const SvfKernelCoefs& c, double omega) noexcept;
std::complex<double> svfResponse(std::span<const SvfSection> sections, double omega) noexcept;

// A section's H as a ratio of quadratics in u = z − 1 (no cancellation near DC): H = (n2·u² + n1·u + n0) / (u² + e1·u
// + e0), from the state space of the recursion (H = D + C1·s1 + C2·s2 over det = (u − d1)(u + b2) + b1²); a section
// without recursion (the identity, a plain gain) is the constant n2 (e1 = e0 = 0). RT-safe.
struct SvfSectionPoly {
  double n2, n1, n0, e1, e0;
};
SvfSectionPoly svfSectionPoly(const SvfKernelCoefs& c) noexcept;
// The cascade's H at ω: the product of the sections' numerators and denominators, divided every 8 sections. RT-safe.
std::complex<double> svfCascadeResponse(const SvfSectionPoly* sections, uint32_t count, double omega) noexcept;
// The same given u = e^jω − 1 as svfEjwMinus1(ω) computes it (an evaluator shares it between its filters). RT-safe.
std::complex<double> svfEjwMinus1(double omega) noexcept;  // (−2·sin²(ω/2), sin ω): no cancellation near DC
std::complex<double> svfCascadeResponseAt(const SvfSectionPoly* sections, uint32_t count, std::complex<double> u) noexcept;
// The same for a whole design from its parameters (designs into a stack array: ~3.5 KiB). RT-safe.
std::complex<double> svfResponse(const SvfParams& p, double omega) noexcept;

// The analog prototype's response at ω (radians per sample, the prototype's s = jω): the target the matched design
// approximates. RT-safe.
std::complex<double> svfAnalogResponse(std::span<const SvfProto> sections, double omega) noexcept;
std::complex<double> svfAnalogResponse(const SvfParams& p, double omega) noexcept;

}  // namespace pq::dsp
