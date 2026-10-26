// T3.17-fix2: the smoothed gain kernels (gain, gain_add; pq/dsp/atoms.h) against a double one-pole reference, at
// every Spl mask and lane count, across block sizes; the settled fast paths; in-place gain_add.
#include <doctest/doctest.h>

#include <cmath>
#include <vector>

#include "pq/core/aligned_buffer.h"
#include "pq/core/alloc_trap.h"
#include "pq/dsp/atoms.h"

using pq::dsp::Atom;

namespace {

constexpr float kTau = 96.0f;  // 2 ms at 48 kHz, in samples

struct Rig {
  Atom atom;
  uint32_t lanes;
  pq::AlignedBuffer<float> state{1024};
  Rig(Atom a, uint32_t l) : atom(a), lanes(l) {
    pq::dsp::StateInit init;
    init.lanes = l;
    pq::dsp::initState(a, state.data(), init);
  }
  // One block: acc (gain_add only), x, g (Spl pattern or Blk), out. Spl operands are 8-float patterns.
  void run(const float* acc, bool accSpl, const float* x, bool xSpl, const float* g, bool gSpl, float* out,
           uint32_t frames) {
    const float tau[8] = {kTau, kTau, kTau, kTau, kTau, kTau, kTau, kTau};
    const bool add = atom == Atom::GainAdd;
    const float* in[4];
    uint32_t mask = 0, k = 0;
    if (add) {
      in[k] = acc;
      mask |= accSpl ? 1u << k : 0u;
      ++k;
    }
    in[k] = x;
    mask |= xSpl ? 1u << k : 0u;
    ++k;
    in[k] = g;
    mask |= gSpl ? 1u << k : 0u;
    ++k;
    in[k] = tau;
    mask |= 1u << k;
    ++k;
    const pq::dsp::KernelFn fn = pq::dsp::findKernel(atom, lanes, mask);
    REQUIRE(fn != nullptr);
    float* outs[1] = {out};
    const pq::dsp::KernelArgs a{in, outs, state.data(), nullptr, frames, static_cast<uint16_t>(k), 1, mask};
    CHECK(pq::alloc_trap::countRtViolations([&] { fn(a); }) == 0);
  }
};

}  // namespace

TEST_CASE("gain: a one-pole in closed form, any block size; settles exactly onto its target") {
  for (uint32_t block : {1u, 7u, 8u, 37u, 64u, 480u}) {
    CAPTURE(block);
    Rig r(Atom::Gain, 1);
    double y = 0.0;
    const double rr = std::exp(-1.0 / kTau);
    double worst = 0.0;
    std::vector<float> x(block), out(block);
    uint64_t t = 0;
    for (uint32_t b = 0; b < 48000 / block; ++b) {
      const float target = (t / 4800) % 2 == 0 ? 0.8f : 0.1f;  // a step every 100 ms
      float g[8];
      for (float& v : g) v = target;
      for (uint32_t i = 0; i < block; ++i) x[i] = static_cast<float>(std::sin(0.01 * static_cast<double>(t + i)));
      r.run(nullptr, false, x.data(), false, g, true, out.data(), block);
      for (uint32_t i = 0; i < block; ++i, ++t) {
        worst = std::max(worst, std::fabs(out[i] - static_cast<double>(x[i]) * y));
        y = target + (y - target) * rr;
      }
    }
    CHECK_MESSAGE(worst < 1e-5, "worst " << worst);  // float error of d·r^k accumulated over a 100 ms step
    // Settled: exactly the target (one multiply per sample from here on).
    float g[8];
    for (float& v : g) v = 0.1f;
    for (uint32_t i = 0; i < 48000 / block; ++i) r.run(nullptr, false, x.data(), false, g, true, out.data(), block);
    for (uint32_t i = 0; i < block; ++i) CHECK_MESSAGE(out[i] == x[i] * 0.1f, out[i] << " vs " << x[i] * 0.1f);
  }
}

TEST_CASE("gain: Spl x, audio-rate g (unsmoothed), and 8 lanes against the per-lane one-pole") {
  {  // Spl x · a settled g
    Rig r(Atom::Gain, 1);
    const float x[8] = {0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f};
    std::vector<float> g(64, 0.25f), out(64);
    r.run(nullptr, false, x, true, g.data(), false, out.data(), 64);
    for (float v : out) CHECK(v == 0.125f);
  }
  {  // 8 lanes: each lane its own target
    Rig r(Atom::Gain, 8);
    std::vector<float> x(8 * 32, 1.0f), out(8 * 32);
    float g[8];
    for (int l = 0; l < 8; ++l) g[l] = 0.1f * static_cast<float>(l);
    double y[8] = {};
    const double rr = std::exp(-1.0 / kTau);
    double worst = 0.0;
    for (int b = 0; b < 100; ++b) {
      r.run(nullptr, false, x.data(), false, g, true, out.data(), 32);
      for (int f = 0; f < 32; ++f)
        for (int l = 0; l < 8; ++l) {
          worst = std::max(worst, std::fabs(out[8 * f + l] - y[l]));
          y[l] = g[l] + (y[l] - g[l]) * rr;
        }
    }
    CHECK_MESSAGE(worst < 1e-6, "worst " << worst);
  }
}

TEST_CASE("gain_add: closed at 0 it is the accumulator, untouched in place; open it adds x · g") {
  Rig r(Atom::GainAdd, 1);
  std::vector<float> acc(64), x(64, 1.0f);
  for (int i = 0; i < 64; ++i) acc[i] = static_cast<float>(i);
  const float zero[8] = {};
  // In place (out == acc) at a settled 0: nothing is written (acc stays as it was).
  r.run(acc.data(), false, x.data(), false, zero, true, acc.data(), 64);
  for (int i = 0; i < 64; ++i) CHECK(acc[i] == static_cast<float>(i));
  // Out of place: a copy.
  std::vector<float> out(64);
  r.run(acc.data(), false, x.data(), false, zero, true, out.data(), 64);
  CHECK(out == acc);
  // A Spl accumulator (the chain's first link, a constant 0) at a settled 0: a fill.
  r.run(zero, true, x.data(), false, zero, true, out.data(), 64);
  for (float v : out) CHECK(v == 0.0f);
  // Opened: fades in from 0 towards acc + x · 1.
  float one[8];
  for (float& v : one) v = 1.0f;
  for (int b = 0; b < 100; ++b) r.run(acc.data(), false, x.data(), false, one, true, out.data(), 64);
  for (int i = 0; i < 64; ++i) CHECK_MESSAGE(out[i] == static_cast<float>(i) + 1.0f, out[i]);
}
