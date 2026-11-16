#pragma once

// The Wavetable face's editing math (TMM.9; private to views): the harmonic editor and the draw tools, both working on
// one keyframe's 2048-sample cycle — the canonical form the document stores (docs/plans/MM.md Decisions: the harmonic
// editor is a view of the samples).
//
//   Harmonics   the cycle's spectrum c_h (h = 0 … 1023, x_n = c_0 + 2 Re Σ c_h e^(2πihn/N); the Nyquist bin kept
//               aside, so nothing is lost). A harmonic's AMPLITUDE is its sinusoid's peak, 2|c_h| (DC: |c_0|); its
//               PHASE is relative to a sine (arg c_h + π/2: a sine's harmonic sits at 0; DC: 0 positive, π negative).
//               An edit writes the cycle back as the old samples plus a double-precision inverse FFT of the edited
//               harmonics' change, so the harmonics not edited change only by the float rounding of the samples
//               (≤ 1e-7 relative; tested ≤ 1e-6), and viewing never writes anything.
//   Strokes     pencil (linear between pointer samples), line (anchor → pointer), smooth curve (Catmull-Rom through
//               the stroke's points, non-uniform in x), each optionally on a snap grid (n columns: the pencil fills
//               whole columns, line and curve points land on column edges; values in steps of 2 / n). Line and curve
//               re-render from the cycle the stroke began with at every step; the pencil overwrites as it goes.
//               bug-wavetable: the pencil (off the grid) can be SMOOTHED — the drawn samples (the raw stroke) blurred
//               by smoothCycle at one of kSmoothWidths, blended in by the blurred mask of the samples the stroke
//               touched (min(1, 2·blur)), so the stroke's ends join what was there without a step. Smoothing 0 is
//               the raw pencil, sample for sample.
//   Smoothing   smoothCycle: three passes of a circular box w samples wide (≈ a Gaussian, σ ≈ w / 2), in double:
//               periodic (harmonic h is scaled by ≈ e^(−2π²σ²h² / 2048²)).
//   Symmetry    applySymmetry: Mirror (v(1 − x) = v(x)) or HalfWave (v(x + ½) = −v(x): odd harmonics only), the half
//               the stroke began in driving the other.
//   Actions     invert (−v), reverse (x → 1 − x, sample 0 kept), rotate (by whole samples, wrapping).
//   Nodes       the node tool's cycle (bug-wavetable): a dsp::Curve over [−1, 1] — t the phase, a point's y the value's
//               share (v + 1)/2, the knob-curve editor's shapes and bends per segment (dsp/curve.h), its first and last
//               points TIED (equal y at t = 0 and 1: the cycle is periodic). renderNodes samples it at the 2048
//               phases; fitNodes fits ≤ kFitSegments segments to a drawn cycle (greedy: the worst sample becomes a
//               point until every sample is within kFitTolerance, each segment's shape the best of a grid of the
//               knob curves' shapes with its bend refined; then points go again while their merged segment stays
//               within the error reached). A sine takes ≈ 7 nodes, a triangle or a square their corners exactly;
//               fine texture hits the cap. ≈ 5 … 40 ms, once per entry or outside edit. The stored form is
//               meta::wavetableNodeData (a keyframe's `nodes`).
//
// UI thread only; allocation-free after the first use of each object.

#include <complex>
#include <cstdint>
#include <span>
#include <vector>

#include "pq/dsp/curve.h"

namespace pq::views::wt {

inline constexpr uint32_t kCycleLen = 2048;
inline constexpr uint32_t kHarmonicCount = 1024;  // c_0 … c_1023

struct Spectrum1 {
  std::vector<std::complex<double>> c;  // kHarmonicCount
  double nyquist = 0.0;                 // c_1024 (real)
};

// The spectrum of a cycle (kCycleLen samples).
void cycleSpectrum(std::span<const float> cycle, Spectrum1& out);

// Display values of harmonic h from a spectrum.
float harmonicAmp(const Spectrum1& s, uint32_t h) noexcept;
float harmonicPhase(const Spectrum1& s, uint32_t h) noexcept;  // (−π, π]

// The coefficient harmonic h takes when its amplitude (peak, ≥ 0) or displayed phase is set. A phase on a harmonic
// that is absent changes nothing (it has none); an amplitude on an absent harmonic starts at a sine's phase.
std::complex<double> withAmp(std::complex<double> c, uint32_t h, double amp) noexcept;
std::complex<double> withPhase(std::complex<double> c, uint32_t h, double phase) noexcept;

// Writes `base` (the cycle the spectrum `baseSpec` was taken of) plus the change of every harmonic flagged in
// `edited` (to `want`) into `out`. Scratch is kept between calls.
class HarmonicWriter {
public:
  void write(std::span<const float> base, const Spectrum1& baseSpec, std::span<const std::complex<double>> want,
             std::span<const uint8_t> edited, std::span<float> out);

private:
  std::vector<double> re_, im_;
};

enum class Tool : uint8_t { None, Pencil, Line, Curve, Nodes };

// The pencil's smoothing amounts (the box width of smoothCycle; 0: the raw pencil) and the Smooth action's step.
inline constexpr uint32_t kSmoothLevels = 4;
inline constexpr uint32_t kSmoothWidths[kSmoothLevels] = {0, 9, 25, 65};
inline constexpr uint32_t kSmoothStep = 15;

// A periodic blur of `cycle` in place: three passes of a circular box `width` samples wide (made odd; ≤ 1: nothing).
// `scratch` is kept between calls.
void smoothCycle(std::span<float> cycle, uint32_t width, std::vector<double>& scratch);

enum class Symmetry : uint8_t { Off, Mirror, HalfWave };
inline constexpr uint32_t kSymmetryCount = 3;
// Makes `cycle` (kCycleLen samples) symmetric: the half holding phase `from` drives the other.
void applySymmetry(std::span<float> cycle, Symmetry s, float from) noexcept;

void invertCycle(std::span<float> cycle) noexcept;
void reverseCycle(std::span<float> cycle) noexcept;                 // x → 1 − x (sample 0 stays)
void rotateCycle(std::span<float> cycle, int32_t samples) noexcept;  // later by `samples` (wrapping)

// ---- nodes ----
inline constexpr uint32_t kFitSegments = 12;    // fitNodes' most segments (the curve's 16 leave room to add)
inline constexpr double kFitTolerance = 0.03;   // its target: every sample within this (values, ±1 full scale)
inline constexpr double kNodeGap = 1.0 / 2048;  // the least distance between points (one sample)
// A flat node cycle at value v (one segment, its ends tied).
dsp::Curve flatNodes(double v = 0.0) noexcept;
// The node cycle's value at phase x ∈ [0, 1].
double nodeValue(const dsp::Curve& c, double x) noexcept;
// Samples the node cycle at the kCycleLen phases n / kCycleLen.
void renderNodes(const dsp::Curve& c, std::span<float> cycle) noexcept;
// Nodes fitted to a drawn cycle (kCycleLen samples).
dsp::Curve fitNodes(std::span<const float> cycle);
// Whether `c` is a node cycle (a valid curve over [−1, 1], its ends tied, ≥ 1 segment).
bool validNodes(const dsp::Curve& c) noexcept;

// One stroke on a cycle.
class Stroke {
public:
  // Starts on `base` (copied) with the first point (phase x ∈ [0, 1], value v ∈ [−1, 1]); `grid` 0 or the columns;
  // `smooth` the pencil's smoothing box width (kSmoothWidths; 0 or a grid: none).
  void begin(std::span<const float> base, Tool tool, uint32_t grid, float x, float v, uint32_t smooth = 0);
  // The pointer moved to (x, v): the stroke's cycle is updated.
  void to(float x, float v);
  std::span<const float> cycle() const noexcept { return smooth_ > 1 ? out_ : cycle_; }
  // The raw stroke (before smoothing).
  std::span<const float> raw() const noexcept { return cycle_; }
  // The point a pointer at (x, v) lands on (the grid's snap for line and curve; the pencil keeps x).
  static void snap(Tool tool, uint32_t grid, float& x, float& v) noexcept;

private:
  struct Pt {
    float x, v;
  };
  void renderCurve(std::span<const Pt> pts);
  void fillColumns(float xa, float va, float xb, float vb);
  void segment(float xa, float va, float xb, float vb);  // linear, samples between
  void touch(int64_t n0, int64_t n1) noexcept;           // samples n0 … n1 were drawn (smoothing's mask)
  void smoothOut();                                      // out_ from cycle_ and the touched mask
  Tool tool_ = Tool::Pencil;
  uint32_t grid_ = 0, smooth_ = 0;
  std::vector<float> base_, cycle_, out_, blur_, mask_;
  std::vector<double> scratch_;
  Pt last_{0, 0}, anchor_{0, 0};
  std::vector<Pt> pts_, work_;  // the curve's committed points (sorted by x); the points rendered this step
};

}  // namespace pq::views::wt
