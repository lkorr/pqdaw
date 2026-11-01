// TMM.6: the oversample region's rate converters (pq/dsp/oversample.h): the half-band design (stopband, passband,
// group delay, with the float32 coefficients the kernels use), the kernels against a double reference, up → down as
// an allpass, block-split invariance, 1 lane == each of 8 lanes, the phase interpolation (exact ramps through wraps),
// bool hold / any.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <numbers>
#include <vector>

#include "pq/core/aligned_buffer.h"
#include "pq/dsp/atoms.h"
#include "pq/dsp/fft.h"
#include "pq/dsp/oversample.h"

using pq::dsp::Atom;
namespace os = pq::dsp::os;

namespace {

float coef(uint32_t i) { return static_cast<float>(os::kCoefs[i]); }

// |H| of stage s (0-based) at f (cycles per sample of the stage's HIGH rate), float coefficients in double.
std::complex<double> stageResponse(uint32_t s, double f) {
  const std::complex<double> z = std::polar(1.0, 2.0 * std::numbers::pi * f);
  const std::complex<double> z2 = z * z;
  std::complex<double> a0 = 1.0, a1 = 1.0;
  for (uint32_t k = 0; k < os::kStageSize[s]; ++k) {
    const double c = coef(os::kStageFirst[s] + k);
    const std::complex<double> ap = (c + 1.0 / z2) / (1.0 + c / z2);
    (k % 2 == 0 ? a0 : a1) *= ap;
  }
  return 0.5 * (a0 + a1 / z);
}

// The double reference of one cascade (the same structure and float coefficients).
struct RefCascade {
  double y1[os::kAllpasses] = {};
  double xin[2 * os::kMaxStages] = {};
  bool upType = true;

  double path(uint32_t s, uint32_t p, double x) {
    double prevIn = xin[2 * s + p];
    xin[2 * s + p] = x;
    for (uint32_t k = p; k < os::kStageSize[s]; k += 2) {
      const uint32_t i = os::kStageFirst[s] + k;
      const double c = coef(i);
      const double y = c * (x - y1[i]) + prevIn;
      prevIn = y1[i];
      y1[i] = y;
      x = y;
    }
    return x;
  }
  void up(double x, uint32_t n, double* out) {
    std::vector<double> buf{x};
    for (uint32_t s = 0; (1u << s) < n; ++s) {
      std::vector<double> next;
      for (double v : buf) {
        next.push_back(path(s, 0, v));
        next.push_back(path(s, 1, v));
      }
      buf = std::move(next);
    }
    std::copy(buf.begin(), buf.end(), out);
  }
  double down(const double* in, uint32_t n) {
    std::vector<double> buf(in, in + n);
    for (uint32_t s = os::stagesOf(n); s-- > 0;) {
      std::vector<double> next;
      for (size_t j = 0; j + 1 < buf.size(); j += 2) next.push_back(0.5 * (path(s, 0, buf[j + 1]) + path(s, 1, buf[j])));
      buf = std::move(next);
    }
    return buf[0];
  }
};

// Runs a converter kernel on `in` in blocks of `block` base frames (lanes 1 or 8, the same signal in every lane
// shifted by the lane index · 0.01 when `lanes` = 8).
std::vector<float> runKernel(Atom a, uint32_t lanes, uint32_t n, const std::vector<float>& in, uint32_t block,
                             pq::AlignedBuffer<float>* keepState = nullptr) {
  const bool up = a == Atom::OversampleUp || a == Atom::OversamplePhase || a == Atom::OversampleHold;
  const size_t inPerFrame = up ? 1 : n, outPerFrame = up ? n : 1;
  const size_t frames = in.size() / inPerFrame / lanes;
  pq::AlignedBuffer<float> state(std::max<size_t>(pq::dsp::kernelInfo(a).stateSize / 4, 8));
  pq::dsp::StateInit init;
  init.lanes = lanes;
  pq::dsp::initState(a, state.data(), init);
  std::vector<float> out(frames * outPerFrame * lanes);
  alignas(32) float factor[8];
  std::fill(factor, factor + 8, static_cast<float>(n));
  const pq::dsp::KernelFn fn = pq::dsp::findKernel(a, lanes, 2u);
  REQUIRE(fn != nullptr);
  for (size_t f0 = 0; f0 < frames; f0 += block) {
    const size_t cnt = std::min<size_t>(block, frames - f0);
    const float* ins[2] = {in.data() + f0 * inPerFrame * lanes, factor};
    float* outs[1] = {out.data() + f0 * outPerFrame * lanes};
    pq::dsp::KernelArgs args{ins, outs, state.data(), nullptr, static_cast<uint32_t>(cnt * n), 2, 1, 2u};
    fn(args);
  }
  if (keepState) *keepState = std::move(state);
  return out;
}

}  // namespace

TEST_CASE("oversample: the half-band stages meet their design (float32 coefficients)") {
  for (double sr : {44100.0, 48000.0, 96000.0}) {
    for (uint32_t s = 0; s < os::kMaxStages; ++s) {
      CAPTURE(sr);
      CAPTURE(s);
      const double high = sr * static_cast<double>(2u << s);  // the stage's high rate
      const double low = high / 2;
      // Stopband: everything that would fold below 20 kHz at the stage's low rate.
      double worst = 0;
      for (double f = low - 20000.0; f <= low; f += 5.0) worst = std::max(worst, std::abs(stageResponse(s, f / high)));
      CHECK(20.0 * std::log10(worst) <= -116.0);
      // Passband: flat to 20 kHz.
      double lo = 1, hi = 1;
      for (double f = 0; f <= 20000.0; f += 5.0) {
        const double m = std::abs(stageResponse(s, f / high));
        lo = std::min(lo, m);
        hi = std::max(hi, m);
      }
      CHECK(20.0 * std::log10(hi / lo) < 1e-9);
    }
  }
}

TEST_CASE("oversample: up / down kernels match the double reference; up → down is an allpass") {
  for (uint32_t n : {2u, 4u, 8u, 16u}) {
    CAPTURE(n);
    const uint32_t frames = 3000;
    std::vector<float> x(frames);
    for (uint32_t t = 0; t < frames; ++t)
      x[t] = static_cast<float>(0.7 * std::sin(0.11 * t) + 0.25 * std::sin(1.7 * t + 0.3) + (t == 100 ? 0.5 : 0.0));
    const std::vector<float> u = runKernel(Atom::OversampleUp, 1, n, x, 64);
    const std::vector<float> y = runKernel(Atom::OversampleDown, 1, n, u, 37);
    RefCascade ru, rd;
    std::vector<double> ub(n);
    double errUp = 0, errDown = 0;
    for (uint32_t t = 0; t < frames; ++t) {
      ru.up(x[t], n, ub.data());
      for (uint32_t j = 0; j < n; ++j) errUp = std::max(errUp, std::abs(ub[j] - u[t * n + j]));
      std::vector<double> uf(u.begin() + t * n, u.begin() + (t + 1) * n);
      errDown = std::max(errDown, std::abs(rd.down(uf.data(), n) - y[t]));
    }
    CHECK(errUp < 2e-6);
    CHECK(errDown < 2e-6);
    // ×2: up then down is A0·A1, an allpass: a unit impulse keeps its energy. More stages: the inner stages' allpass
    // sits between stage 1's halves, which only the transition band (above 20 kHz) notices: |T| ≤ 1 everywhere and
    // |T| = 1 within 1e-5 up to 20 kHz (at 48 kHz).
    std::vector<float> imp(8192, 0.0f);
    imp[0] = 1.0f;
    const std::vector<float> yi = runKernel(Atom::OversampleDown, 1, n, runKernel(Atom::OversampleUp, 1, n, imp, 1024), 1024);
    double e = 0;
    for (float v : yi) e += static_cast<double>(v) * v;
    if (n == 2) CHECK(std::abs(e - 1.0) < 1e-5);
    else CHECK(e <= 1.0 + 1e-6);
    pq::dsp::RealFft fft(8192);
    std::vector<float> re(4097), im(4097);
    fft.forward(yi.data(), re.data(), im.data());
    double passErr = 0, peak = 0;
    for (uint32_t b = 0; b <= 4096; ++b) {
      const double m = std::hypot(static_cast<double>(re[b]), static_cast<double>(im[b]));
      peak = std::max(peak, m);
      if (b * 48000.0 / 8192 <= 20000.0) passErr = std::max(passErr, std::abs(m - 1.0));
    }
    CHECK(passErr < 1e-5);
    CHECK(peak < 1.0 + 1e-5);
    // Group delay at DC = Σ over stages of Σ (1 − c)/(1 + c) / 2^stage: the step response's centre of mass.
    double gd = 0;
    for (uint32_t s = 0; s < os::stagesOf(n); ++s)
      for (uint32_t k = 0; k < os::kStageSize[s]; ++k) {
        const double c = coef(os::kStageFirst[s] + k);
        gd += (1 - c) / (1 + c) / static_cast<double>(1u << s);
      }
    double m0 = 0, m1 = 0;
    for (size_t t = 0; t < yi.size(); ++t) {
      m0 += yi[t];
      m1 += yi[t] * static_cast<double>(t);
    }
    CHECK(std::abs(m1 / m0 - gd) < 1e-3);
    const double expected = n == 2 ? 3.615 : n == 4 ? 4.736 : n == 8 ? 5.167 : 5.306;  // pq/dsp/oversample.h
    CHECK(std::abs(gd - expected) < 1e-3);
  }
}

TEST_CASE("oversample: block splits change nothing; every lane of 8 equals the 1-lane kernel") {
  for (Atom a : {Atom::OversampleUp, Atom::OversampleDown, Atom::OversamplePhase}) {
    for (uint32_t n : {2u, 8u}) {
      CAPTURE(pq::dsp::kernelInfo(a).name);
      CAPTURE(n);
      const bool up = a != Atom::OversampleDown;
      const uint32_t frames = 515;
      const uint32_t perFrame = up ? 1 : n;
      std::vector<float> x1(frames * perFrame), x8(frames * perFrame * 8);
      for (uint32_t i = 0; i < x1.size(); ++i) {
        for (uint32_t l = 0; l < 8; ++l) {
          const double v = a == Atom::OversamplePhase ? std::fmod(0.013 * (l + 1) * i, 1.0) : std::sin(0.05 * i + l);
          x8[i * 8 + l] = static_cast<float>(v);
        }
        x1[i] = x8[i * 8 + 3];
      }
      const std::vector<float> whole = runKernel(a, 1, n, x1, frames);
      const std::vector<float> split = runKernel(a, 1, n, x1, 7);
      CHECK(std::memcmp(whole.data(), split.data(), whole.size() * 4) == 0);
      const std::vector<float> lanes = runKernel(a, 8, n, x8, 13);
      bool same = true;
      for (size_t i = 0; i < whole.size(); ++i) same &= std::memcmp(&whole[i], &lanes[i * 8 + 3], 4) == 0;
      CHECK(same);
    }
  }
}

TEST_CASE("oversample: phase interpolation is exact for ramps and keeps their wraps") {
  for (uint32_t n : {2u, 4u, 16u}) {
    CAPTURE(n);
    // A phasor at an increment of 0.0371 (24-bit exact values, as the phasor atom's output).
    const uint32_t frames = 400;
    std::vector<float> ph(frames);
    const uint32_t inc = static_cast<uint32_t>(0.0371 * 4294967296.0);
    uint32_t acc = 0;
    for (uint32_t t = 0; t < frames; ++t, acc += inc) ph[t] = static_cast<float>(acc >> 8) * 5.9604644775390625e-08f;
    const std::vector<float> u = runKernel(Atom::OversamplePhase, 1, n, ph, 33);
    double worst = 0;
    for (uint32_t t = 1; t < frames; ++t)
      for (uint32_t j = 0; j < n; ++j) {
        // The ramp between the two base samples, through the wrap: x[t] − d·(n − 1 − j)/n, d = the increment.
        const double d = std::fmod(static_cast<double>(ph[t]) - ph[t - 1] + 1.0, 1.0);
        double want = static_cast<double>(ph[t]) - d * (n - 1 - j) / n;
        want -= std::floor(want);
        const float got = u[t * n + j];
        CHECK(got >= 0.0f);
        CHECK(got < 1.0f);
        double e = std::abs(got - want);
        e = std::min(e, 1.0 - e);  // 0.9999999 and 0 are the same phase
        worst = std::max(worst, e);
      }
    CHECK(worst < 2e-7);
    // The last sub-sample of every frame is the input exactly; the first frame (no previous value) holds it.
    for (uint32_t t = 0; t < frames; ++t) CHECK(u[t * n + n - 1] == ph[t]);
    for (uint32_t j = 0; j < n; ++j) CHECK(u[j] == ph[0]);
    // Backwards (negative increments) and unbounded phases keep their integer part.
    std::vector<float> back(frames);
    for (uint32_t t = 0; t < frames; ++t) back[t] = static_cast<float>(7.0 + std::fmod(-0.02 * t + 100.0, 1.0));
    const std::vector<float> ub = runKernel(Atom::OversamplePhase, 1, n, back, 64);
    for (uint32_t t = 1; t < frames; ++t)
      for (uint32_t j = 0; j + 1 < n; ++j) {
        double a = ub[t * n + j], b = ub[t * n + j + 1];
        double step = b - a;
        step -= std::round(step);
        CHECK(std::abs(step + 0.02 / n) < 1e-5);  // −0.02/n per sub-sample, through the wraps
        CHECK(a >= 7.0);
        CHECK(a < 8.0);
      }
  }
}

TEST_CASE("oversample: bool hold repeats, any keeps every pulse") {
  const float T = pq::dsp::boolMask(true), F = 0.0f;
  const std::vector<float> in = {T, F, F, T, T, F};
  const std::vector<float> held = runKernel(Atom::OversampleHold, 1, 4, in, 4);
  REQUIRE(held.size() == 24);
  for (uint32_t i = 0; i < 24; ++i) CHECK(std::memcmp(&held[i], &in[i / 4], 4) == 0);
  // A one-sub-sample pulse anywhere in a frame makes the frame high (canonical masks).
  std::vector<float> fine(24, F);
  fine[5] = T;   // frame 1
  fine[23] = T;  // frame 5
  const std::vector<float> any = runKernel(Atom::OversampleAny, 8 / 8, 4, fine, 2);
  REQUIRE(any.size() == 6);
  for (uint32_t k = 0; k < 6; ++k) CHECK(std::memcmp(&any[k], (k == 1 || k == 5) ? &T : &F, 4) == 0);
}
