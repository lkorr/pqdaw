// EQ.2: response rules of the registry's atoms (response_rules.h, pq/graph/response_rule.h).

#include "response_rules.h"

#include <algorithm>
#include <cmath>

#include "fold.h"
#include "pq/dsp/kernel.h"
#include "pq/dsp/svf_design.h"

namespace pq::graph::rules {

namespace {

using C = std::complex<double>;

C& g(ResponseArgs& a, uint32_t out, uint32_t in) noexcept { return a.gain[out * a.numInputs + in]; }
bool usable(const ResponseArgs& a, uint32_t i) noexcept { return a.signal[i] == 0 && a.known[i] != 0; }

// z^n for n ≥ 0 by squaring (exact enough for the delay lengths a ring holds; |z| = 1).
C zPow(C z, uint32_t n) noexcept {
  C r{1.0, 0.0};
  while (n != 0) {
    if (n & 1u) r *= z;
    z *= z;
    n >>= 1;
  }
  return r;
}

// The kernel's delay in samples: clamp(d, min, max) in float, NaN → min (kernels_memory.cpp delayTap). False when the
// delay is not usable (a signal, unknown, or the max unknown).
bool delaySamples(const ResponseArgs& a, float minDelay, float& d) noexcept {
  if (a.signal[1] != 0 || a.known[1] == 0 || a.known[2] == 0) return false;
  const float maxDelay = std::ceil(std::max(a.value[2], 0.0f));
  d = std::min(std::max(a.value[1], minDelay), maxDelay);  // std::max(NaN, m) = NaN; the kernel's max gives m
  if (std::isnan(a.value[1])) d = std::min(minDelay, maxDelay);
  return true;
}

}  // namespace

ResponseKind add(ResponseArgs& a) noexcept {
  g(a, 0, 0) = 1.0;
  g(a, 0, 1) = 1.0;
  return ResponseKind::Exact;
}

ResponseKind sub(ResponseArgs& a) noexcept {
  g(a, 0, 0) = 1.0;
  g(a, 0, 1) = -1.0;
  return ResponseKind::Exact;
}

ResponseKind neg(ResponseArgs& a) noexcept {
  g(a, 0, 0) = -1.0;
  return ResponseKind::Exact;
}

ResponseKind mul(ResponseArgs& a) noexcept {
  if (a.signal[0] != 0 && a.signal[1] != 0) return ResponseKind::Unavailable;
  const uint32_t s = a.signal[0] != 0 ? 0u : 1u, k = 1u - s;
  if (!usable(a, k)) return ResponseKind::Unavailable;
  g(a, 0, s) = a.value[k];
  return ResponseKind::Exact;
}

ResponseKind div(ResponseArgs& a) noexcept {
  if (!usable(a, 1)) return ResponseKind::Unavailable;
  g(a, 0, 0) = 1.0 / static_cast<double>(a.value[1]);
  return ResponseKind::Exact;
}

ResponseKind muladd(ResponseArgs& a) noexcept {
  g(a, 0, 2) = 1.0;
  if (a.signal[0] == 0 && a.signal[1] == 0) return ResponseKind::Exact;  // only c carries the signal
  if (a.signal[0] != 0 && a.signal[1] != 0) return ResponseKind::Unavailable;
  const uint32_t s = a.signal[0] != 0 ? 0u : 1u, k = 1u - s;
  if (!usable(a, k)) return ResponseKind::Unavailable;
  g(a, 0, s) = a.value[k];
  return ResponseKind::Exact;
}

ResponseKind lerp(ResponseArgs& a) noexcept {
  if (!usable(a, 2)) return ResponseKind::Unavailable;
  const double t = a.value[2];
  g(a, 0, 0) = 1.0 - t;
  g(a, 0, 1) = t;
  return ResponseKind::Exact;
}

ResponseKind select(ResponseArgs& a) noexcept {
  if (!usable(a, 0)) return ResponseKind::Unavailable;
  g(a, 0, dsp::maskIsTrue(a.value[0]) ? 1u : 2u) = 1.0;
  return ResponseKind::Exact;
}

ResponseKind mux(ResponseArgs& a) noexcept {
  if (!usable(a, 0) || a.numInputs < 2) return ResponseKind::Unavailable;
  g(a, 0, 1u + detail::muxPick(a.value[0], a.numInputs - 1u)) = 1.0;
  return ResponseKind::Exact;
}

ResponseKind demux(ResponseArgs& a) noexcept {
  if (!usable(a, 0) || a.numOutputs == 0) return ResponseKind::Unavailable;
  g(a, detail::muxPick(a.value[0], a.numOutputs), 1) = 1.0;
  return ResponseKind::Exact;
}

ResponseKind identity(ResponseArgs& a) noexcept {
  for (uint32_t o = 0; o < a.numOutputs; ++o)
    for (uint32_t i = 0; i < a.numInputs; ++i) g(a, o, i) = 1.0;
  return ResponseKind::Exact;
}

ResponseKind z1(ResponseArgs& a) noexcept {
  g(a, 0, 0) = a.z;
  a.dependsOnOmega = true;
  return ResponseKind::Exact;
}

ResponseKind delayLinear(ResponseArgs& a) noexcept {
  float d = 0.0f;
  if (!delaySamples(a, 1.0f, d)) return ResponseKind::Unavailable;
  const float fl = std::floor(d);
  const double f = d - fl;
  // y = x[−n] + f·(x[−n−1] − x[−n])
  g(a, 0, 0) = zPow(a.z, static_cast<uint32_t>(fl)) * ((1.0 - f) + f * a.z);
  a.dependsOnOmega = true;
  return ResponseKind::Exact;
}

ResponseKind delayCubic(ResponseArgs& a) noexcept {
  float d = 0.0f;
  if (!delaySamples(a, 2.0f, d)) return ResponseKind::Unavailable;
  const float fl = std::floor(d);
  if (fl < 1.0f) return ResponseKind::Unavailable;  // a max below the minimum delay
  const double t = d - fl;
  // Catmull-Rom between x0 = x[−n] (t = 0) and x1 = x[−n−1]; xm1 = x[−n+1], x2 = x[−n−2] (kernels_memory.cpp hermite).
  const double t2 = t * t, t3 = t2 * t;
  const double wm1 = 0.5 * (-t3 + 2.0 * t2 - t), w0 = 0.5 * (3.0 * t3 - 5.0 * t2 + 2.0);
  const double w1 = 0.5 * (-3.0 * t3 + 4.0 * t2 + t), w2 = 0.5 * (t3 - t2);
  const C z = a.z;
  g(a, 0, 0) = zPow(z, static_cast<uint32_t>(fl) - 1u) * (wm1 + z * (w0 + z * (w1 + z * w2)));
  a.dependsOnOmega = true;
  return ResponseKind::Exact;
}

ResponseKind delayAllpass(ResponseArgs& a) noexcept {
  float d = 0.0f;
  if (!delaySamples(a, 1.5f, d)) return ResponseKind::Unavailable;
  const float fl = std::floor(d);
  float fr = d - fl;
  uint32_t n = static_cast<uint32_t>(fl);
  if (fr < 0.5f && n == 0) return ResponseKind::Unavailable;  // a max below the minimum delay
  if (fr < 0.5f) {  // the fraction is kept in [0.5, 1.5)
    fr += 1.0f;
    n -= 1u;
  }
  const float c = (1.0f - fr) / (1.0f + fr);  // in float, as the kernel
  // y = c·(x[−n] − y[−1]) + x[−n−1]  →  H = z^n (c + z) / (1 + c z)
  const C z = a.z;
  g(a, 0, 0) = zPow(z, n) * (static_cast<double>(c) + z) / (1.0 + static_cast<double>(c) * z);
  a.dependsOnOmega = true;
  return ResponseKind::Exact;
}

ResponseKind gain(ResponseArgs& a) noexcept {
  if (!usable(a, 1)) return ResponseKind::Unavailable;
  g(a, 0, 0) = a.value[1];
  return ResponseKind::Exact;
}

ResponseKind gainAdd(ResponseArgs& a) noexcept {
  if (!usable(a, 2)) return ResponseKind::Unavailable;
  g(a, 0, 0) = 1.0;
  g(a, 0, 1) = a.value[2];
  return ResponseKind::Exact;
}

ResponseKind slopeOne(ResponseArgs& a) noexcept {
  g(a, 0, 0) = 1.0;
  return ResponseKind::Approximate;
}

ResponseKind clip(ResponseArgs& a) noexcept {
  if (!usable(a, 1) || !usable(a, 2)) return ResponseKind::Unavailable;
  g(a, 0, 0) = a.value[1] < 0.0f && 0.0f < a.value[2] ? 1.0 : 0.0;
  return ResponseKind::Approximate;
}

// EQ.1: the svf atom, exactly as its kernel runs it: svfParamsFromInputs (the kernel's input conversion) → svfDesign →
// the float coefficients' response (svfResponse). Cutoff / q / gain must be known values (a modulated filter is not
// time-invariant: Unavailable); mode / stages / prewarp are consts. Designs per frequency (≈ 0.1 µs per section).
ResponseKind svf(ResponseArgs& a) noexcept {
  if (a.signal[0] == 0) return ResponseKind::Exact;  // x is not on the path
  for (uint32_t i = 1; i < 7; ++i)
    if (!usable(a, i)) return ResponseKind::Unavailable;
  const dsp::SvfParams p =
      dsp::svfParamsFromInputs(a.value[1], a.value[2], a.value[3], a.value[4], a.value[5], a.value[6], a.sampleRate);
  g(a, 0, 0) = dsp::svfResponse(p, a.omega);
  a.dependsOnOmega = true;
  return ResponseKind::Exact;
}

}  // namespace pq::graph::rules
