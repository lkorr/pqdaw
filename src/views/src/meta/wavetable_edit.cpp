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

// ---- strokes ----

void Stroke::snap(Tool tool, uint32_t grid, float& x, float& v) noexcept {
  x = std::clamp(x, 0.0f, 1.0f);
  v = std::clamp(v, -1.0f, 1.0f);
  if (grid == 0) return;
  const float steps = static_cast<float>(grid) * 0.5f;
  v = std::clamp(std::round(v * steps) / steps, -1.0f, 1.0f);
  if (tool != Tool::Pencil) x = std::round(x * static_cast<float>(grid)) / static_cast<float>(grid);
}

void Stroke::begin(std::span<const float> base, Tool tool, uint32_t grid, float x, float v) {
  tool_ = tool == Tool::None ? Tool::Pencil : tool;
  grid_ = grid;
  base_.assign(base.begin(), base.end());
  base_.resize(kCycleLen, 0.0f);
  cycle_ = base_;
  snap(tool_, grid_, x, v);
  last_ = anchor_ = {x, v};
  pts_.clear();
  pts_.push_back({x, v});
  if (tool_ == Tool::Pencil) {
    if (grid_ > 0) fillColumns(x, v, x, v);
    else cycle_[std::min(kCycleLen - 1, static_cast<uint32_t>(std::lround(x * kCycleLen)) % kCycleLen)] = v;
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
    return;
  }
  const double w = static_cast<double>(xb) - xa;
  for (int64_t n = std::max<int64_t>(n0, 0); n <= std::min<int64_t>(n1, kCycleLen - 1); ++n) {
    const double t = w > 0.0 ? (static_cast<double>(n) / kCycleLen - xa) / w : 1.0;
    cycle_[static_cast<size_t>(n)] = static_cast<float>(va + std::clamp(t, 0.0, 1.0) * (static_cast<double>(vb) - va));
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
    case Tool::Pencil:
      if (grid_ > 0) fillColumns(last_.x, last_.v, x, v);
      else segment(last_.x, last_.v, x, v);
      last_ = {x, v};
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
