// The Wavetable face's editing math (TMM.9). See wavetable_edit.h.

#include "wavetable_edit.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "pq/dsp/tables.h"

namespace pq::views::wt {

namespace {

constexpr double kPi = std::numbers::pi;

double wrapPi(double a) noexcept {
  a = std::remainder(a, 2.0 * kPi);
  return a <= -kPi ? a + 2.0 * kPi : a;
}

// A coefficient this small is an absent harmonic (−300 dB: only exact zeros and FFT dust).
constexpr double kAbsent = 1e-15;

}  // namespace

void cycleSpectrum(std::span<const float> cycle, Spectrum1& out) {
  std::vector<double> re(kCycleLen, 0.0), im(kCycleLen, 0.0);
  for (uint32_t i = 0; i < kCycleLen && i < cycle.size(); ++i) re[i] = cycle[i];
  dsp::fftInPlace(re, im, false);
  out.c.resize(kHarmonicCount);
  for (uint32_t h = 0; h < kHarmonicCount; ++h) out.c[h] = {re[h] / kCycleLen, im[h] / kCycleLen};
  out.nyquist = re[kCycleLen / 2] / kCycleLen;
}

float harmonicAmp(const Spectrum1& s, uint32_t h) noexcept {
  if (h >= s.c.size()) return 0.0f;
  return static_cast<float>(h == 0 ? std::fabs(s.c[0].real()) : 2.0 * std::abs(s.c[h]));
}

float harmonicPhase(const Spectrum1& s, uint32_t h) noexcept {
  if (h >= s.c.size()) return 0.0f;
  if (h == 0) return s.c[0].real() < 0.0 ? static_cast<float>(kPi) : 0.0f;
  if (std::abs(s.c[h]) <= kAbsent) return 0.0f;
  return static_cast<float>(wrapPi(std::arg(s.c[h]) + 0.5 * kPi));
}

std::complex<double> withAmp(std::complex<double> c, uint32_t h, double amp) noexcept {
  amp = std::max(0.0, amp);
  if (h == 0) return {c.real() < 0.0 ? -amp : amp, 0.0};
  const double arg = std::abs(c) > kAbsent ? std::arg(c) : -0.5 * kPi;  // a new harmonic starts as a sine
  return std::polar(0.5 * amp, arg);
}

std::complex<double> withPhase(std::complex<double> c, uint32_t h, double phase) noexcept {
  if (h == 0) return {std::fabs(phase) < 0.5 * kPi ? std::fabs(c.real()) : -std::fabs(c.real()), 0.0};
  const double m = std::abs(c);
  if (m <= kAbsent) return c;
  return std::polar(m, phase - 0.5 * kPi);
}

void HarmonicWriter::write(std::span<const float> base, const Spectrum1& baseSpec, std::span<const std::complex<double>> want,
                           std::span<const uint8_t> edited, std::span<float> out) {
  re_.assign(kCycleLen, 0.0);
  im_.assign(kCycleLen, 0.0);
  bool any = false;
  const uint32_t n = static_cast<uint32_t>(std::min({want.size(), edited.size(), baseSpec.c.size()}));
  for (uint32_t h = 0; h < n; ++h) {
    if (!edited[h]) continue;
    const std::complex<double> d = want[h] - baseSpec.c[h];
    if (d == std::complex<double>{}) continue;
    any = true;
    if (h == 0) {
      re_[0] = d.real();
      continue;
    }
    re_[h] = d.real();
    im_[h] = d.imag();
    re_[kCycleLen - h] = d.real();
    im_[kCycleLen - h] = -d.imag();
  }
  const uint32_t len = static_cast<uint32_t>(std::min<size_t>({base.size(), out.size(), kCycleLen}));
  if (!any) {
    std::copy_n(base.begin(), len, out.begin());
    return;
  }
  dsp::fftInPlace(re_, im_, true);  // unscaled inverse: Σ_k X_k e^(2πikn/N) = the change of the samples
  for (uint32_t i = 0; i < len; ++i) out[i] = static_cast<float>(static_cast<double>(base[i]) + re_[i]);
}

// ---- smoothing, symmetry, actions ----

void smoothCycle(std::span<float> cycle, uint32_t width, std::vector<double>& scratch) {
  const auto n = static_cast<int64_t>(cycle.size());
  if (n < 3 || width <= 1) return;
  int64_t w = static_cast<int64_t>(width | 1u);
  if (w >= n) w = (n - 1) | 1;
  if (w >= n) w -= 2;
  if (w <= 1) return;
  const int64_t h = w / 2;
  // Prefix sums over the cycle extended by h on each side (circular): b[i] = (P[i + w] − P[i]) / w. (Prefix sums, not
  // a running sum: over a stretch of zeros both ends hold the same sum, so the result is exactly 0 there.)
  scratch.resize(static_cast<size_t>(3 * n + w + 1));
  double* a = scratch.data();
  double* b = a + n;
  double* p = b + n;
  for (int64_t i = 0; i < n; ++i) a[i] = cycle[static_cast<size_t>(i)];
  const double inv = 1.0 / static_cast<double>(w);
  for (int pass = 0; pass < 3; ++pass) {
    p[0] = 0.0;
    for (int64_t j = 0; j < n + w; ++j) p[j + 1] = p[j] + a[((j - h) % n + n) % n];
    for (int64_t i = 0; i < n; ++i) b[i] = (p[i + w] - p[i]) * inv;
    std::swap(a, b);
  }
  for (int64_t i = 0; i < n; ++i) cycle[static_cast<size_t>(i)] = static_cast<float>(a[i]);
}

void applySymmetry(std::span<float> cycle, Symmetry s, float from) noexcept {
  const size_t n = cycle.size(), half = n / 2;
  if (n < 4 || s == Symmetry::Off) return;
  const bool first = from < 0.5f;
  if (s == Symmetry::Mirror) {
    for (size_t i = 1; i < half; ++i) {
      if (first) cycle[n - i] = cycle[i];
      else cycle[i] = cycle[n - i];
    }
    return;
  }
  for (size_t i = 0; i < half; ++i) {
    if (first) cycle[i + half] = -cycle[i];
    else cycle[i] = -cycle[i + half];
  }
}

void invertCycle(std::span<float> cycle) noexcept {
  for (float& v : cycle) v = -v;
}

void reverseCycle(std::span<float> cycle) noexcept {
  const size_t n = cycle.size();
  for (size_t i = 1; i < n - i; ++i) std::swap(cycle[i], cycle[n - i]);
}

void rotateCycle(std::span<float> cycle, int32_t samples) noexcept {
  const auto n = static_cast<int64_t>(cycle.size());
  if (n == 0) return;
  const int64_t k = ((static_cast<int64_t>(samples) % n) + n) % n;
  if (k == 0) return;
  std::rotate(cycle.begin(), cycle.begin() + static_cast<std::ptrdiff_t>(n - k), cycle.end());
}

// ---- nodes ----

dsp::Curve flatNodes(double v) noexcept {
  dsp::Curve c;
  c.min = -1.0;
  c.max = 1.0;
  c.preset = dsp::CurvePreset::Custom;
  c.segments = 1;
  const double y = std::clamp((v + 1.0) * 0.5, 0.0, 1.0);
  c.points[0] = {0.0, y};
  c.points[1] = {1.0, y};
  c.shapes[0] = {dsp::CurveShape::Linear, 0.0};
  return c;
}

double nodeValue(const dsp::Curve& c, double x) noexcept { return dsp::evalCurveUnits(c, x); }

void renderNodes(const dsp::Curve& c, std::span<float> cycle) noexcept {
  const auto n = static_cast<uint32_t>(std::min<size_t>(cycle.size(), kCycleLen));
  for (uint32_t i = 0; i < n; ++i) cycle[i] = static_cast<float>(dsp::evalCurveUnits(c, static_cast<double>(i) / kCycleLen));
}

bool validNodes(const dsp::Curve& c) noexcept {
  return dsp::validCurve(c) && c.min == -1.0 && c.max == 1.0 && c.segments >= 1 && c.step == 0.0 &&
         c.mode == dsp::CurveMode::Clip && c.output == dsp::CurveOutput::None &&
         c.points[0].y == c.points[c.segments].y;
}

namespace {

// fitNodes' candidate segment shapes (a coarse grid per family; the best is then refined along its bend).
struct FitShape {
  dsp::CurveShape shape;
  double bend;
};
constexpr FitShape kFitGrid[] = {
    {dsp::CurveShape::Linear, 0.0},
    {dsp::CurveShape::Power, 0.5}, {dsp::CurveShape::Power, -0.5}, {dsp::CurveShape::Power, 1.0},
    {dsp::CurveShape::Power, -1.0}, {dsp::CurveShape::Power, 1.5}, {dsp::CurveShape::Power, -1.5},
    {dsp::CurveShape::Power, 2.5}, {dsp::CurveShape::Power, -2.5},
    {dsp::CurveShape::SCurve, 1.5}, {dsp::CurveShape::SCurve, -1.5}, {dsp::CurveShape::SCurve, 3.0},
    {dsp::CurveShape::SCurve, -3.0}, {dsp::CurveShape::SCurve, 5.0}, {dsp::CurveShape::SCurve, -5.0},
    {dsp::CurveShape::SCurve, 8.0}, {dsp::CurveShape::SCurve, -8.0},
    {dsp::CurveShape::Exp, 1.5}, {dsp::CurveShape::Exp, -1.5}, {dsp::CurveShape::Exp, 3.0},
    {dsp::CurveShape::Exp, -3.0}, {dsp::CurveShape::Exp, 6.0}, {dsp::CurveShape::Exp, -6.0},
};

struct FitSeg {
  dsp::CurveSegment shape{};
  double err = 0.0;    // the largest |error| over the segment's samples (< 0: cannot be split further)
  int64_t worst = -1;  // where
};

// The segment from (ta, ya) to (tb, yb) (shares) fitted to `want`: the grid's best shape by its largest error over
// every 2nd sample, its bend refined by a golden-section search (same sign, ±1.5; Power ±0.6), then measured exactly.
FitSeg fitSegment(std::span<const double> want, double ta, double ya, double tb, double yb) {
  constexpr int64_t kN = kCycleLen;
  const auto n0 = static_cast<int64_t>(std::ceil(ta * kN));
  const int64_t n1 = tb >= 1.0 ? kN - 1 : std::min<int64_t>(kN - 1, static_cast<int64_t>(std::floor(tb * kN)));
  const double w = tb - ta;
  dsp::Curve k;  // a one-segment curve over [0, 1]: its value is the segment's share g(u)
  k.preset = dsp::CurvePreset::Custom;
  auto errOf = [&](dsp::CurveSegment sg, int64_t stride, int64_t* worst) {
    k.shapes[0] = sg;
    double e = 0.0;
    for (int64_t n = n0; n <= n1; n += stride) {
      const double u = (static_cast<double>(n) / kN - ta) / w;
      const double v = -1.0 + 2.0 * (ya + (yb - ya) * dsp::evalCurveUnits(k, u));
      const double d = std::fabs(v - want[static_cast<size_t>(n)]);
      if (d > e) {
        e = d;
        if (worst) *worst = n;
      }
    }
    return e;
  };
  FitSeg best;
  double bestErr = 1e300;
  for (const FitShape& f : kFitGrid) {
    const double e = errOf({f.shape, f.bend}, 2, nullptr);
    if (e < bestErr - 1e-12) bestErr = e, best.shape = {f.shape, f.bend};
  }
  if (best.shape.shape != dsp::CurveShape::Linear) {
    const double r = best.shape.shape == dsp::CurveShape::Power ? 0.6 : 1.5, k0 = best.shape.bend;
    double lo = k0 - r, hi = k0 + r;
    if (k0 > 0.0) lo = std::max(lo, 0.05);
    else hi = std::min(hi, -0.05);
    const double g = 0.5 * (std::sqrt(5.0) - 1.0);
    double c = hi - g * (hi - lo), d = lo + g * (hi - lo);
    double fc = errOf({best.shape.shape, c}, 2, nullptr), fd = errOf({best.shape.shape, d}, 2, nullptr);
    for (int it = 0; it < 24; ++it) {
      if (fc < fd) {
        hi = d, d = c, fd = fc;
        c = hi - g * (hi - lo);
        fc = errOf({best.shape.shape, c}, 2, nullptr);
      } else {
        lo = c, c = d, fc = fd;
        d = lo + g * (hi - lo);
        fd = errOf({best.shape.shape, d}, 2, nullptr);
      }
    }
    const double kb = fc < fd ? c : d, eb = std::min(fc, fd);
    if (eb < bestErr) best.shape.bend = static_cast<float>(kb);  // (as the data stores it)
  }
  best.err = errOf(best.shape, 1, &best.worst);
  return best;
}

}  // namespace

dsp::Curve fitNodes(std::span<const float> cycle) {
  constexpr int64_t kN = kCycleLen;
  std::vector<double> want(kN, 0.0);
  for (int64_t i = 0; i < kN && i < static_cast<int64_t>(cycle.size()); ++i)
    want[static_cast<size_t>(i)] = std::clamp(static_cast<double>(cycle[static_cast<size_t>(i)]), -1.0, 1.0);
  dsp::Curve c = flatNodes(want[0]);
  std::vector<FitSeg> segs(1);
  auto fit = [&](uint32_t i) {
    segs[i] = fitSegment(want, c.points[i].t, c.points[i].y, c.points[i + 1].t, c.points[i + 1].y);
    c.shapes[i] = segs[i].shape;
  };
  fit(0);
  // Greedy: the worst sample of the worst segment becomes a point, until every sample is within the tolerance.
  while (c.segments < kFitSegments) {
    uint32_t i = 0;
    for (uint32_t k = 1; k < c.segments; ++k)
      if (segs[k].err > segs[i].err) i = k;
    if (!(segs[i].err > kFitTolerance) || segs[i].worst < 0) break;
    const double t = static_cast<double>(segs[i].worst) / kN;
    if (t - c.points[i].t < kNodeGap * 0.999 || c.points[i + 1].t - t < kNodeGap * 0.999) {
      segs[i].err = -1.0;  // the worst sample hugs a point: leave this segment as it is
      continue;
    }
    for (uint32_t k = c.segments; k > i; --k) c.points[k + 1] = c.points[k];
    for (uint32_t k = c.segments; k > i + 1; --k) c.shapes[k] = c.shapes[k - 1];
    c.points[i + 1] = {t, (want[static_cast<size_t>(segs[i].worst)] + 1.0) * 0.5};
    c.segments = static_cast<uint8_t>(c.segments + 1);
    segs.insert(segs.begin() + i + 1, FitSeg{});
    fit(i);
    fit(i + 1);
  }
  // Then fewer: a point goes while the merged segment stays within the error reached (greedy splits cluster).
  double reached = kFitTolerance;
  for (const FitSeg& sg : segs) reached = std::max(reached, sg.err);
  while (c.segments > 1) {
    uint32_t drop = 0;
    FitSeg merged;
    merged.err = 1e300;
    for (uint32_t i = 1; i < c.segments; ++i) {
      const FitSeg m = fitSegment(want, c.points[i - 1].t, c.points[i - 1].y, c.points[i + 1].t, c.points[i + 1].y);
      if (m.err <= reached && m.err < merged.err) drop = i, merged = m;
    }
    if (drop == 0) break;
    for (uint32_t k = drop; k < c.segments; ++k) c.points[k] = c.points[k + 1];
    for (uint32_t k = drop; k + 1 < c.segments; ++k) c.shapes[k] = c.shapes[k + 1];
    c.points[c.segments] = {};
    c.shapes[c.segments - 1] = {};
    c.segments = static_cast<uint8_t>(c.segments - 1);
    segs.erase(segs.begin() + drop);
    segs[drop - 1] = merged;
    c.shapes[drop - 1] = merged.shape;
  }
  return c;
}

// ---- strokes ----

void Stroke::snap(Tool tool, uint32_t grid, float& x, float& v) noexcept {
  x = std::clamp(x, 0.0f, 1.0f);
  v = std::clamp(v, -1.0f, 1.0f);
  if (grid == 0) return;
  const float steps = static_cast<float>(grid) * 0.5f;
  v = std::clamp(std::round(v * steps) / steps, -1.0f, 1.0f);
  if (tool != Tool::Pencil) x = std::round(x * static_cast<float>(grid)) / static_cast<float>(grid);
}

void Stroke::begin(std::span<const float> base, Tool tool, uint32_t grid, float x, float v, uint32_t smooth) {
  tool_ = tool == Tool::None || tool == Tool::Nodes ? Tool::Pencil : tool;
  grid_ = grid;
  smooth_ = tool_ == Tool::Pencil && grid_ == 0 ? smooth : 0u;
  base_.assign(base.begin(), base.end());
  base_.resize(kCycleLen, 0.0f);
  cycle_ = base_;
  if (smooth_ > 1) mask_.assign(kCycleLen, 0.0f);
  snap(tool_, grid_, x, v);
  last_ = anchor_ = {x, v};
  pts_.clear();
  pts_.push_back({x, v});
  if (tool_ == Tool::Pencil) {
    if (grid_ > 0) {
      fillColumns(x, v, x, v);
    } else {
      const uint32_t n = std::min(kCycleLen - 1, static_cast<uint32_t>(std::lround(x * kCycleLen)) % kCycleLen);
      cycle_[n] = v;
      touch(n, n);
      smoothOut();
    }
  } else if (tool_ == Tool::Line) {
    segment(x, v, x, v);
  } else {
    renderCurve(pts_);
  }
}

void Stroke::segment(float xa, float va, float xb, float vb) {
  if (xb < xa) {
    std::swap(xa, xb);
    std::swap(va, vb);
  }
  const auto n0 = static_cast<int64_t>(std::ceil(static_cast<double>(xa) * kCycleLen));
  const auto n1 = static_cast<int64_t>(std::floor(static_cast<double>(xb) * kCycleLen));
  if (n1 < n0) {  // within one sample interval: the nearer sample takes the newer value
    const auto n = std::clamp<int64_t>(std::lround(static_cast<double>(xb) * kCycleLen), 0, kCycleLen - 1);
    cycle_[static_cast<size_t>(n)] = vb;
    touch(n, n);
    return;
  }
  const double w = static_cast<double>(xb) - xa;
  for (int64_t n = std::max<int64_t>(n0, 0); n <= std::min<int64_t>(n1, kCycleLen - 1); ++n) {
    const double t = w > 0.0 ? (static_cast<double>(n) / kCycleLen - xa) / w : 1.0;
    cycle_[static_cast<size_t>(n)] = static_cast<float>(va + std::clamp(t, 0.0, 1.0) * (static_cast<double>(vb) - va));
  }
  touch(n0, n1);
}

void Stroke::touch(int64_t n0, int64_t n1) noexcept {
  if (smooth_ <= 1) return;
  for (int64_t n = std::max<int64_t>(n0, 0); n <= std::min<int64_t>(n1, kCycleLen - 1); ++n) mask_[static_cast<size_t>(n)] = 1.0f;
}

void Stroke::smoothOut() {
  if (smooth_ <= 1) return;
  // The raw stroke blurred, blended in where the stroke drew (the mask blurred alike, doubled: full inside, fading
  // to nothing within about half the box outside), so its ends join the cycle that was there.
  out_ = cycle_;
  smoothCycle(out_, smooth_, scratch_);
  blur_ = mask_;
  smoothCycle(blur_, smooth_, scratch_);
  for (uint32_t i = 0; i < kCycleLen; ++i) {
    const float m = std::min(1.0f, 2.0f * blur_[i]);
    out_[i] = cycle_[i] + m * (out_[i] - cycle_[i]);
  }
}

void Stroke::fillColumns(float xa, float va, float xb, float vb) {
  const auto g = static_cast<int32_t>(grid_);
  const int32_t ca = std::clamp(static_cast<int32_t>(std::floor(xa * static_cast<float>(g))), 0, g - 1);
  const int32_t cb = std::clamp(static_cast<int32_t>(std::floor(xb * static_cast<float>(g))), 0, g - 1);
  const float steps = static_cast<float>(g) * 0.5f;
  for (int32_t c = std::min(ca, cb); c <= std::max(ca, cb); ++c) {
    const float xc = (static_cast<float>(c) + 0.5f) / static_cast<float>(g);
    float v = vb;
    if (xb != xa && c != cb) v = va + std::clamp((xc - xa) / (xb - xa), 0.0f, 1.0f) * (vb - va);
    v = std::clamp(std::round(v * steps) / steps, -1.0f, 1.0f);
    const uint32_t s0 = static_cast<uint32_t>(c) * kCycleLen / grid_, s1 = static_cast<uint32_t>(c + 1) * kCycleLen / grid_;
    std::fill(cycle_.begin() + s0, cycle_.begin() + s1, v);
  }
}

void Stroke::to(float x, float v) {
  snap(tool_, grid_, x, v);
  switch (tool_) {
    case Tool::None:
    case Tool::Nodes:
    case Tool::Pencil:
      if (grid_ > 0) fillColumns(last_.x, last_.v, x, v);
      else segment(last_.x, last_.v, x, v);
      last_ = {x, v};
      smoothOut();
      return;
    case Tool::Line:
      std::copy(base_.begin(), base_.end(), cycle_.begin());
      segment(anchor_.x, anchor_.v, x, v);
      last_ = {x, v};
      return;
    case Tool::Curve: {
      // The stroke overwrites: committed points strictly between the last committed one and the pointer go; the
      // pointer is a point of the curve, committed once it is far enough from the last committed one (so the curve
      // stays smooth however finely the pointer moves).
      const float lo = std::min(last_.x, x), hi = std::max(last_.x, x);
      std::erase_if(pts_, [&](const Pt& p) { return p.x > lo && p.x < hi; });
      work_ = pts_;
      std::erase_if(work_, [&](const Pt& p) { return p.x == x; });
      work_.insert(std::lower_bound(work_.begin(), work_.end(), x, [](const Pt& p, float k) { return p.x < k; }), Pt{x, v});
      renderCurve(work_);
      const float gap = grid_ > 0 ? 1.0f / static_cast<float>(grid_) : 1.0f / 48.0f;
      if (std::fabs(x - last_.x) >= gap * 0.999f) {
        pts_.swap(work_);
        last_ = {x, v};
      }
      return;
    }
  }
}

void Stroke::renderCurve(std::span<const Pt> pts) {
  std::copy(base_.begin(), base_.end(), cycle_.begin());
  const size_t n = pts.size();
  if (n == 0) return;
  if (n == 1) {
    segment(pts[0].x, pts[0].v, pts[0].x, pts[0].v);
    return;
  }
  // Catmull-Rom for non-uniform x: tangents by central differences (one-sided at the ends), cubic Hermite per span.
  auto slope = [&](size_t k) -> double {
    const size_t a = k == 0 ? 0 : k - 1, b = k + 1 < n ? k + 1 : n - 1;
    const double dx = static_cast<double>(pts[b].x) - pts[a].x;
    return dx > 0.0 ? (static_cast<double>(pts[b].v) - pts[a].v) / dx : 0.0;
  };
  const auto s0 = std::max<int64_t>(0, static_cast<int64_t>(std::ceil(static_cast<double>(pts.front().x) * kCycleLen)));
  const auto s1 = std::min<int64_t>(kCycleLen - 1, static_cast<int64_t>(std::floor(static_cast<double>(pts.back().x) * kCycleLen)));
  size_t k = 0;
  for (int64_t s = s0; s <= s1; ++s) {
    const double t = static_cast<double>(s) / kCycleLen;
    while (k + 2 < n && t > pts[k + 1].x) ++k;
    const double x0 = pts[k].x, x1 = pts[k + 1].x, h = x1 - x0;
    double v;
    if (h <= 0.0) {
      v = pts[k + 1].v;
    } else {
      const double u = std::clamp((t - x0) / h, 0.0, 1.0), u2 = u * u, u3 = u2 * u;
      v = (2 * u3 - 3 * u2 + 1) * pts[k].v + (u3 - 2 * u2 + u) * h * slope(k) + (-2 * u3 + 3 * u2) * pts[k + 1].v +
          (u3 - u2) * h * slope(k + 1);
    }
    cycle_[static_cast<size_t>(s)] = static_cast<float>(std::clamp(v, -1.0, 1.0));
  }
}

}  // namespace pq::views::wt
