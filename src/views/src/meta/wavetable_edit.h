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
//
// UI thread only; allocation-free after the first use of each object.

#include <complex>
#include <cstdint>
#include <span>
#include <vector>

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

enum class Tool : uint8_t { None, Pencil, Line, Curve };

// One stroke on a cycle.
class Stroke {
public:
  // Starts on `base` (copied) with the first point (phase x ∈ [0, 1], value v ∈ [−1, 1]); `grid` 0 or the columns.
  void begin(std::span<const float> base, Tool tool, uint32_t grid, float x, float v);
  // The pointer moved to (x, v): the stroke's cycle is updated.
  void to(float x, float v);
  std::span<const float> cycle() const noexcept { return cycle_; }
  // The point a pointer at (x, v) lands on (the grid's snap for line and curve; the pencil keeps x).
  static void snap(Tool tool, uint32_t grid, float& x, float& v) noexcept;

private:
  struct Pt {
    float x, v;
  };
  void renderCurve(std::span<const Pt> pts);
  void fillColumns(float xa, float va, float xb, float vb);
  void segment(float xa, float va, float xb, float vb);  // linear, samples between
  Tool tool_ = Tool::Pencil;
  uint32_t grid_ = 0;
  std::vector<float> base_, cycle_;
  Pt last_{0, 0}, anchor_{0, 0};
  std::vector<Pt> pts_, work_;  // the curve's committed points (sorted by x); the points rendered this step
};

}  // namespace pq::views::wt
