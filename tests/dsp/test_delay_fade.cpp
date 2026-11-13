// DT.2 (docs/plans/DT.md): delay_cubic's 1-sample floor and delay_fade's read heads (pq/dsp/atoms.h "delay_fade"):
//   - delay_cubic: an impulse at 1, 1.5 and 2 samples (linear in [1, 2), the Hermite read from 2, continuous at 2),
//     the split read at 1 sample (a feedback loop through it is legal)
//   - delay_fade at rest is delay_cubic bit for bit (Spl time, and an audio-rate time inside the octave bound)
//   - a step it can glide within an octave glides linearly over the fade's frames (Spl), from where the head is
//   - a larger step crossfades the old and the new time (equal power, the fade's frames); one during a fade waits
//   - an audio-rate step beyond the bound crossfades; the split read + write match the atom bit for bit; 8 lanes
#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <numbers>
#include <random>
#include <vector>

#include "pq/core/aligned_buffer.h"
#include "pq/dsp/atom_state.h"
#include "pq/dsp/atoms.h"
#include "pq/dsp/kernel.h"

using pq::dsp::Atom;

namespace {

// One delay line's state.
struct Line {
  pq::AlignedBuffer<float> st;
  uint32_t lanes;
  Line(Atom a, uint32_t lanes_, uint32_t maxFrames) : st(1), lanes(lanes_) {
    pq::dsp::StateInit init;
    init.lanes = lanes;
    init.maxDelayFrames = maxFrames;
    st = pq::AlignedBuffer<float>(pq::dsp::stateBytes(a, init) / 4);
    pq::dsp::initState(a, st.data(), init);
  }
  // One call: inputs as given (Spl ones point at `lanes` floats), `mask` their Spl bits.
  void run(Atom a, std::vector<const float*> ins, float* out, uint32_t frames, uint32_t mask) {
    float* outs[1] = {out};
    const pq::dsp::KernelArgs args{ins.data(), out != nullptr ? outs : nullptr, st.data(), nullptr, frames,
                                   static_cast<uint16_t>(ins.size()), 1, mask};
    const pq::dsp::KernelFn k = pq::dsp::findKernel(a, lanes, mask);
    REQUIRE(k != nullptr);
    k(args);
  }
};

constexpr uint32_t kSplT = 0b110;  // delay_fade: time and fade Spl
constexpr uint32_t kSplD = 0b10;   // delay_cubic: time Spl

// delay_fade (time Spl) and delay_cubic references over a piecewise-constant time: `times[b]` for block b of `block`
// frames, input x (1 lane).
std::vector<float> renderFade(const std::vector<float>& x, const std::vector<float>& times, uint32_t block, float fade,
                              uint32_t maxFrames) {
  Line l(Atom::DelayFade, 1, maxFrames);
  std::vector<float> y(x.size());
  for (size_t b = 0; b < times.size(); ++b)
    l.run(Atom::DelayFade, {x.data() + b * block, &times[b], &fade}, y.data() + b * block, block, kSplT);
  return y;
}
std::vector<float> renderCubic(const std::vector<float>& x, const std::vector<float>& d, uint32_t maxFrames, bool spl) {
  Line l(Atom::DelayCubic, 1, maxFrames);
  std::vector<float> y(x.size());
  l.run(Atom::DelayCubic, {x.data(), d.data()}, y.data(), static_cast<uint32_t>(x.size()), spl ? kSplD : 0u);
  return y;
}

std::vector<float> noise(size_t n, uint32_t seed) {
  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> u(-1.0f, 1.0f);
  std::vector<float> x(n);
  for (float& v : x) v = u(rng);
  return x;
}

double gainNew(double p) { return std::sin(std::numbers::pi / 2.0 * p); }

}  // namespace

TEST_CASE("delay_cubic: the minimum is 1 sample — an impulse at 1, 1.5 and 2 samples, continuous at 2") {
  for (uint32_t lanes : {1u, 8u}) {
    CAPTURE(lanes);
    constexpr uint32_t T = 40, at = 10;
    std::vector<float> x(T * lanes, 0.0f);
    for (uint32_t l = 0; l < lanes; ++l) x[at * lanes + l] = 1.0f;
    auto run = [&](float d) {
      Line line(Atom::DelayCubic, lanes, 64);
      std::vector<float> dv(lanes, d), y(T * lanes);
      line.run(Atom::DelayCubic, {x.data(), dv.data()}, y.data(), T, kSplD);
      return y;
    };
    const auto y1 = run(1.0f), y15 = run(1.5f), y2 = run(2.0f), y0 = run(0.25f), yNear = run(1.9999f);
    for (uint32_t l = 0; l < lanes; ++l) {
      for (uint32_t t = 0; t < T; ++t) {
        CAPTURE(t);
        CHECK(y1[t * lanes + l] == (t == at + 1 ? 1.0f : 0.0f));
        CHECK(y0[t * lanes + l] == y1[t * lanes + l]);  // below 1: clamped to 1
        CHECK(y15[t * lanes + l] == (t == at + 1 || t == at + 2 ? 0.5f : 0.0f));
        CHECK(y2[t * lanes + l] == (t == at + 2 ? 1.0f : 0.0f));
        CHECK(std::fabs(yNear[t * lanes + l] - y2[t * lanes + l]) < 2e-4f);  // continuous at 2
      }
    }
  }
}

TEST_CASE("delay_cubic: the split read at 1 sample — a per-sample feedback loop y = x + 0.5·y[n−1]") {
  constexpr uint32_t T = 64;
  Line line(Atom::DelayReadCubic, 1, 16);
  const float d = 1.0f;
  std::vector<float> y(T);
  for (uint32_t t = 0; t < T; ++t) {
    float r = 0.0f;
    line.run(Atom::DelayReadCubic, {&d}, &r, 1, 0b1);
    y[t] = (t == 3 ? 1.0f : 0.0f) + 0.5f * r;
    line.run(Atom::DelayWrite, {&y[t]}, nullptr, 1, 0);
  }
  for (uint32_t t = 0; t < T; ++t) CHECK(y[t] == (t < 3 ? 0.0f : std::ldexp(1.0f, -static_cast<int>(t - 3))));
}

TEST_CASE("delay_fade: at rest it is delay_cubic bit for bit (a param-rate time; an audio-rate one inside the bound)") {
  const auto x = noise(4096, 1);
  {
    const std::vector<float> times(64, 333.25f);
    const auto a = renderFade(x, times, 64, 480.0f, 2000);
    const auto b = renderCubic(x, std::vector<float>(1, 333.25f), 2000, true);
    for (size_t i = 0; i < x.size(); ++i) REQUIRE(std::bit_cast<uint32_t>(a[i]) == std::bit_cast<uint32_t>(b[i]));
  }
  {  // a slow sine around 300 samples: steps of ≤ 0.06 samples per sample, followed exactly
    std::vector<float> d(x.size());
    for (size_t i = 0; i < d.size(); ++i) d[i] = static_cast<float>(300.0 + 200.0 * std::sin(0.0003 * static_cast<double>(i)));
    Line l(Atom::DelayFade, 1, 2000);
    std::vector<float> a(x.size());
    const float fade = 480.0f;
    for (size_t b = 0; b < x.size(); b += 64) l.run(Atom::DelayFade, {x.data() + b, d.data() + b, &fade}, a.data() + b, 64, 0b100);
    const auto c = renderCubic(x, d, 2000, false);
    for (size_t i = 0; i < x.size(); ++i) REQUIRE(std::bit_cast<uint32_t>(a[i]) == std::bit_cast<uint32_t>(c[i]));
  }
}

TEST_CASE("delay_fade: a step within an octave glides linearly over the fade's frames, from where the head is") {
  constexpr uint32_t B = 64, F = 480;
  const auto x = noise(64 * 40, 2);
  // 100 → 300 samples at block 10 (Δ = 200 ≤ F/2: the head reads at 1 − 200/480 ≈ 0.58: a glide); at block 12 (mid
  // glide) → 50: from where it is (≈ 153.3), Δ ≈ −103 ≥ −F: a glide again.
  std::vector<float> times(40, 100.0f);
  for (size_t b = 10; b < 12; ++b) times[b] = 300.0f;
  for (size_t b = 12; b < 40; ++b) times[b] = 50.0f;
  const auto y = renderFade(x, times, B, static_cast<float>(F), 1000);
  // The head's path, as the kernel computes it (end − inc·rem, rem counting down), read by delay_cubic.
  std::vector<float> d(x.size());
  float end = 100.0f, inc = 0.0f, rem = 0.0f;
  for (size_t t = 0; t < x.size(); ++t) {
    const float v = times[t / B];
    if (v != end) {
      const float cur = std::fmaf(-inc, rem, end);
      inc = (v - cur) / static_cast<float>(F);
      rem = static_cast<float>(F);
      end = v;
    }
    rem = std::max(rem - 1.0f, 0.0f);
    d[t] = std::fmaf(-inc, rem, end);
  }
  CHECK(d[12 * B - 1] > 150.0f);
  CHECK(d[12 * B - 1] < 160.0f);
  const auto ref = renderCubic(x, d, 1000, false);
  for (size_t i = 0; i < x.size(); ++i) {
    CAPTURE(i);
    REQUIRE(std::bit_cast<uint32_t>(y[i]) == std::bit_cast<uint32_t>(ref[i]));
  }
}

TEST_CASE("delay_fade: a larger step crossfades the old and the new time (equal power), one during a fade waits") {
  constexpr uint32_t B = 64, F = 480;
  const auto x = noise(64 * 60, 3);
  // 100 → 1100 at block 10 (Δ = 1000 > F/2: a fade over frames 640 … 1119); at block 12 (mid fade) → 2000: it waits
  // for the fade's end (frame 1120), then fades 1100 → 2000 (frames 1120 … 1599); then 2000 → 40 (Δ < −F) at block 40.
  std::vector<float> times(60, 100.0f);
  for (size_t b = 10; b < 12; ++b) times[b] = 1100.0f;
  for (size_t b = 12; b < 40; ++b) times[b] = 2000.0f;
  for (size_t b = 40; b < 60; ++b) times[b] = 40.0f;
  const auto y = renderFade(x, times, B, static_cast<float>(F), 4000);
  auto at = [&](float d) { return renderCubic(x, std::vector<float>(1, d), 4000, true); };
  const auto r100 = at(100.0f), r1100 = at(1100.0f), r2000 = at(2000.0f), r40 = at(40.0f);
  auto fade = [&](size_t t, size_t start, const std::vector<float>& from, const std::vector<float>& to) {
    const double p = static_cast<double>(t - start + 1) / F;
    return gainNew(p) * to[t] + gainNew(1.0 - p) * from[t];
  };
  double worst = 0.0;
  for (size_t t = 0; t < x.size(); ++t) {
    double want;
    if (t < 640) want = r100[t];
    else if (t < 1120) want = fade(t, 640, r100, r1100);
    else if (t < 1600) want = fade(t, 1120, r1100, r2000);
    else if (t < 2560) want = r2000[t];
    else if (t < 2560 + F) want = fade(t, 2560, r2000, r40);
    else want = r40[t];
    worst = std::max(worst, std::fabs(want - static_cast<double>(y[t])));
    if (t < 640 || (t >= 1600 && t < 2560) || t >= 2560 + F) {  // one head: bit for bit
      CAPTURE(t);
      REQUIRE(std::bit_cast<uint32_t>(y[t]) == std::bit_cast<uint32_t>(static_cast<float>(want)));
    }
  }
  MESSAGE("crossfade vs the equal-power reference: max |error| " << worst);
  CHECK(worst < 1e-5);
}

TEST_CASE("delay_fade: an audio-rate step beyond the octave bound crossfades (a glide of one frame otherwise)") {
  constexpr uint32_t F = 256;
  const auto x = noise(2048, 4);
  std::vector<float> d(x.size(), 20.0f);
  for (size_t t = 1000; t < d.size(); ++t) d[t] = 500.0f;
  Line l(Atom::DelayFade, 1, 1000);
  std::vector<float> y(x.size());
  const float fade = static_cast<float>(F);
  for (size_t b = 0; b < x.size(); b += 128) l.run(Atom::DelayFade, {x.data() + b, d.data() + b, &fade}, y.data() + b, 128, 0b100);
  const auto r20 = renderCubic(x, std::vector<float>(1, 20.0f), 1000, true), r500 = renderCubic(x, std::vector<float>(1, 500.0f), 1000, true);
  for (size_t t = 0; t < x.size(); ++t) {
    CAPTURE(t);
    if (t < 1000) REQUIRE(y[t] == r20[t]);
    else if (t < 1000 + F) {
      const double p = static_cast<double>(t - 1000 + 1) / F;
      CHECK(y[t] == doctest::Approx(gainNew(p) * r500[t] + gainNew(1.0 - p) * r20[t]).epsilon(1e-5).scale(1.0));
    } else REQUIRE(y[t] == r500[t]);
  }
}

TEST_CASE("delay_fade: the split read + write match the atom bit for bit (steps, fades, random blocks), 1 and 8 lanes") {
  constexpr uint32_t T = 3000;
  for (uint32_t lanes : {1u, 8u}) {
    CAPTURE(lanes);
    std::mt19937 rng(7 + lanes);
    const auto x = noise(static_cast<size_t>(T) * lanes, 5 + lanes);
    std::uniform_real_distribution<float> ud(1.0f, 300.0f);
    std::vector<float> fade(lanes, 64.0f);
    // A time per lane that jumps every 200 frames (blocks of 50: Spl per block).
    std::vector<float> times(T / 50 * lanes);
    for (uint32_t b = 0; b < T / 50; ++b)
      for (uint32_t l = 0; l < lanes; ++l) times[b * lanes + l] = b % 4 == 0 ? ud(rng) : (b > 0 ? times[(b - 1) * lanes + l] : 100.0f);
    Line fused(Atom::DelayFade, lanes, 400), split(Atom::DelayReadFade, lanes, 400);
    std::vector<float> a(static_cast<size_t>(T) * lanes), s(a.size());
    for (uint32_t b = 0; b < T / 50; ++b)
      fused.run(Atom::DelayFade, {x.data() + b * 50 * lanes, &times[b * lanes], fade.data()}, a.data() + b * 50 * lanes, 50, kSplT);
    for (uint32_t t = 0; t < T; ++t) {
      split.run(Atom::DelayReadFade, {&times[t / 50 * lanes], fade.data()}, s.data() + t * lanes, 1, 0b11);
      split.run(Atom::DelayWrite, {x.data() + t * lanes}, nullptr, 1, 0);
    }
    for (size_t i = 0; i < a.size(); ++i) REQUIRE(std::bit_cast<uint32_t>(a[i]) == std::bit_cast<uint32_t>(s[i]));
  }
}

TEST_CASE("delay_fade: 8 lanes move independently — one lane fades, one glides, the others stay delay_cubic") {
  constexpr uint32_t T = 1024, B = 64, F = 128;
  const auto x = noise(static_cast<size_t>(T) * 8, 6);
  Line l(Atom::DelayFade, 8, 1000);
  std::vector<float> y(static_cast<size_t>(T) * 8);
  const std::vector<float> fade(8, static_cast<float>(F));
  for (uint32_t b = 0; b < T / B; ++b) {
    std::vector<float> t(8, 50.0f);
    if (b >= 4) t[2] = 600.0f;  // a fade
    if (b >= 4) t[5] = 80.0f;   // a glide (30 ≤ F/2)
    l.run(Atom::DelayFade, {x.data() + b * B * 8, t.data(), fade.data()}, y.data() + b * B * 8, B, kSplT);
  }
  // Lane 0 never moves: delay_cubic at 50 on that lane.
  Line c(Atom::DelayCubic, 8, 1000);
  std::vector<float> r(y.size());
  const std::vector<float> d50(8, 50.0f);
  c.run(Atom::DelayCubic, {x.data(), d50.data()}, r.data(), T, kSplD);
  for (uint32_t t = 0; t < T; ++t) {
    for (uint32_t lane : {0u, 1u, 7u}) REQUIRE(std::bit_cast<uint32_t>(y[t * 8 + lane]) == std::bit_cast<uint32_t>(r[t * 8 + lane]));
    if (t < 4 * B) REQUIRE(std::bit_cast<uint32_t>(y[t * 8 + 2]) == std::bit_cast<uint32_t>(r[t * 8 + 2]));
  }
  // Lane 2 after the fade: x delayed by 600 exactly (whole samples: the Hermite read is the tap; 0 before the input).
  for (uint32_t t = 4 * B + F; t < T; ++t) REQUIRE(y[t * 8 + 2] == (t >= 600 ? x[(t - 600) * 8 + 2] : 0.0f));
  // Lane 5 after the glide: x delayed by 80.
  for (uint32_t t = 4 * B + F; t < T; ++t) REQUIRE(y[t * 8 + 5] == x[(t - 80) * 8 + 5]);
}
