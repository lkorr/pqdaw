// NZ.1: the noise_step atom (pq/dsp/atoms.h "Generators", pq/dsp/noise_step.h, kernels_noise_step.cpp).
//   timing     steps sample-exact at a rate (fractional periods included: the uint32 phase's carries), on the sync grid
//              at 120 / 90 bpm (none while the beat stands), at trig edges (a held trig: one step), a reset restarts the
//              stream (the same values again)
//   draw       the white stream is the documented hash; uniform / gaussian moments and histograms
//   colour     the design's response within ±0.5 dB of the slope over 20 Hz … 20 kHz at 48 kHz for every colour, unit
//              power gain; the kernel equals a double reference of draw → cascade → fold (colours ≤ 0) or scale
//              (colours > 0: never outside [−1, 1], Q244); RMS across colours; brown at a slow rate is a
//              continuous walk inside [−1, 1]
//   shaping    density's keep rate, Steps' levels, Smooth 0 exact holds, Smooth > 0 continuous and monotone, reaching
//              the value exactly
//   lanes      seeded apart (and voice groups apart) or one stream; a lane reset draws at once and continues the stream
//   splits     any split of the frames into calls gives the same bits (every path: white fast path, runs, scan, Blk
//              rate, colour, gaussian, density, steps; 1 and 8 lanes); RtScope on each path
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>
#include <random>
#include <string>
#include <vector>

#include "pq/core/aligned_buffer.h"
#include "pq/core/alloc_trap.h"
#include "pq/core/denormal_guard.h"
#include "pq/dsp/atom_state.h"
#include "pq/dsp/atoms.h"
#include "pq/dsp/noise_step.h"

using namespace pq::dsp;

namespace {

constexpr double kSr = 48000.0;
enum : uint32_t { kRate, kTrig, kBeat, kGrid, kReset, kSmooth, kColour, kDensity, kSteps, kSeed, kDist, kLanes, kNumIn };

uint32_t mix1(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

// The kernel with its state and inputs: every input a pattern unless made a block.
struct Rig {
  uint32_t lanes;
  pq::AlignedBuffer<float> state{sizeof(NoiseStepState) / sizeof(float)};
  std::vector<pq::AlignedBuffer<float>> in;
  uint32_t blk = 0;  // inputs given as blocks
  uint32_t maxFrames;
  explicit Rig(uint32_t l, uint32_t frames, uint32_t seed = 7, uint32_t group = 0) : lanes(l), maxFrames(frames) {
    for (uint32_t i = 0; i < kNumIn; ++i) in.emplace_back(static_cast<size_t>(frames) * 8 + 8);
    StateInit init;
    init.lanes = l;
    init.sampleRate = kSr;
    init.seed = seed;
    init.voiceGroup = group;
    initState(Atom::NoiseStep, state.data(), init);
    // the defaults: every frame, continuous, uniform, lanes seeded apart
    set(kRate, 1e9f), set(kDensity, kNoiseDensityContinuous), set(kLanes, 1.0f);
  }
  NoiseStepState& st() { return *reinterpret_cast<NoiseStepState*>(state.data()); }
  void set(uint32_t i, float v) {  // a pattern
    for (uint32_t k = 0; k < 8; ++k) in[i][k] = v;
    blk &= ~(1u << i);
  }
  void setLanes(uint32_t i, const std::vector<float>& v) {
    for (uint32_t k = 0; k < 8; ++k) in[i][k] = v[k % v.size()];
    blk &= ~(1u << i);
  }
  float* block(uint32_t i) {  // frame f, lane l at [f · lanes + l]
    blk |= 1u << i;
    return in[i].data();
  }
  // Frames [f0, f1) into out (frame-major, `lanes` per frame).
  void run(uint32_t f0, uint32_t f1, float* out) {
    const float* ptrs[kNumIn];
    for (uint32_t i = 0; i < kNumIn; ++i)
      ptrs[i] = (blk >> i) & 1u ? in[i].data() + static_cast<size_t>(f0) * lanes : in[i].data();
    float* o = out + static_cast<size_t>(f0) * lanes;
    float* outs[1] = {o};
    const uint32_t spl = ((1u << kNumIn) - 1u) & ~blk;
    KernelArgs a{ptrs, outs, state.data(), nullptr, f1 - f0, kNumIn, 1, spl};
    const KernelFn fn = findKernel(Atom::NoiseStep, lanes, spl);
    REQUIRE(fn != nullptr);
    fn(a);
  }
  std::vector<float> render(uint32_t frames, uint32_t chunk = 0) {
    std::vector<float> out(static_cast<size_t>(frames) * lanes + 8, -9.0f);
    if (chunk == 0) chunk = frames;
    for (uint32_t f = 0; f < frames; f += chunk) run(f, std::min(frames, f + chunk), out.data());
    out.resize(static_cast<size_t>(frames) * lanes);
    return out;
  }
};

// The key of lane l with seed param s (the kernel's formula).
uint32_t keyOf(Rig& r, uint32_t l, int32_t s = 0, bool laneSeeds = true) {
  const uint32_t lane = laneSeeds ? mix1(r.st().seed[l] ^ r.st().group) : r.st().base;
  return mix1(lane ^ (static_cast<uint32_t>(s) * 0x9E3779B9u));
}
float uniformOf(uint32_t h) { return static_cast<float>(static_cast<int32_t>(h) >> 8) * 1.1920928955078125e-07f; }

std::vector<uint32_t> stepFrames(const std::vector<float>& y) {  // where the held value changes (lane 0, 1 lane)
  std::vector<uint32_t> s{0};
  for (uint32_t f = 1; f < y.size(); ++f)
    if (y[f] != y[f - 1]) s.push_back(f);
  return s;
}

}  // namespace

TEST_CASE("noise_step: white every frame is the hash stream, uniform in [−1, 1)") {
  pq::DenormalGuard dg;
  Rig r(1, 1 << 18);
  const std::vector<float> y = r.render(1 << 18, 1000);
  const uint32_t key = keyOf(r, 0);
  for (uint32_t t = 0; t < 4096; ++t) REQUIRE(y[t] == uniformOf(mix1(key + t)));
  double m = 0, v = 0;
  std::vector<uint32_t> hist(16, 0);
  for (float x : y) {
    REQUIRE(x >= -1.0f);
    REQUIRE(x < 1.0f);
    m += x, v += x * x;
    ++hist[std::min(15, static_cast<int>((x + 1.0f) * 8.0f))];
  }
  m /= y.size(), v /= y.size();
  CHECK(std::abs(m) < 0.005);
  CHECK(std::abs(v - 1.0 / 3.0) < 0.005);
  double chi = 0, e = y.size() / 16.0;
  for (uint32_t h : hist) chi += (h - e) * (h - e) / e;
  CHECK(chi < 40.0);  // 15 degrees of freedom: p ≈ 5e-4
}

TEST_CASE("noise_step: gaussian draws have σ = 1/3 and Irwin–Hall 4's kurtosis") {
  Rig r(1, 1 << 19);
  r.set(kDist, 1.0f);
  const std::vector<float> y = r.render(1 << 19, 777);
  double m = 0, v = 0, k4 = 0, mx = 0;
  for (float x : y) m += x;
  m /= y.size();
  for (float x : y) {
    const double d = x - m;
    v += d * d, k4 += d * d * d * d, mx = std::max(mx, std::abs(static_cast<double>(x)));
  }
  v /= y.size(), k4 /= y.size();
  CHECK(std::abs(m) < 0.003);
  CHECK(std::abs(std::sqrt(v) - 1.0 / 3.0) < 0.003);
  CHECK(std::abs(k4 / (v * v) - 2.7) < 0.05);  // 3 − 6/(5·4)
  CHECK(mx <= 1.0);  // ≤ 1.155 drawn, folded inside [−1, 1]
}

TEST_CASE("noise_step: rate steps are sample-exact, fractional periods included") {
  for (const double rate : {kSr / 2.5, kSr / 7.3, 1000.3, 0.37 * kSr, 3.0, kSr / 64.0}) {
    INFO("rate " << rate);
    Rig r(1, 200000);
    r.set(kRate, static_cast<float>(rate));
    const std::vector<float> y = r.render(200000, 333);
    const double inc = static_cast<double>(std::llround(static_cast<double>(static_cast<float>(rate)) * 4294967296.0 / kSr));
    // the forced first draw at frame 0, then a step at the first frame whose phase wraps: t = ceil(k·2^32 / inc).
    std::vector<uint32_t> want{0};
    for (uint64_t k = 1;; ++k) {
      const uint64_t t = static_cast<uint64_t>(std::ceil(static_cast<double>(k) * 4294967296.0 / inc));
      if (t >= y.size()) break;
      want.push_back(static_cast<uint32_t>(t));
    }
    // a step may redraw the same value (p = 2^-24): compare the change frames with the expected steps
    const std::vector<uint32_t> got = stepFrames(y);
    REQUIRE(got.size() <= want.size());
    CHECK(want.size() - got.size() <= 1);
    size_t j = 0;
    for (uint32_t g : got) {
      while (j < want.size() && want[j] < g) ++j;
      REQUIRE(j < want.size());
      CHECK(want[j] == g);
    }
    CHECK(std::abs(static_cast<double>(want.size() - 1) - rate * y.size() / kSr) <= 1.0);
  }
}

TEST_CASE("noise_step: sync steps land on the grid (120 / 90 bpm), hold while the beat stands") {
  for (const double bpm : {120.0, 90.0}) {
    for (const double grid : {0.25, 1.0 / 3.0, 1.5}) {  // 1/16, a triplet 1/8, a dotted 1/4
      INFO("bpm " << bpm << " grid " << grid);
      const uint32_t n = 200000;
      Rig r(1, n);
      r.set(kRate, 0.0f);
      r.set(kGrid, static_cast<float>(grid));
      float* beat = r.block(kBeat);
      for (uint32_t f = 0; f < n; ++f) beat[f] = static_cast<float>(f * bpm / 60.0 / kSr);
      const std::vector<float> y = r.render(n, 512);
      const std::vector<uint32_t> got = stepFrames(y);
      const double spb = kSr * 60.0 / bpm;  // samples per beat
      REQUIRE(got.size() >= 2);
      for (size_t i = 1; i < got.size(); ++i) {
        const double line = std::round(got[i] / (grid * spb)) * grid * spb;  // the nearest grid line
        CHECK(got[i] == static_cast<uint32_t>(std::ceil(line - 1e-6)));  // the first frame at or after it
      }
      CHECK(std::abs(static_cast<double>(got.size() - 1) - n / (grid * spb)) <= 1.0);
    }
  }
  // stopped: the beat stands → one value
  Rig r(1, 4096);
  r.set(kRate, 0.0f);
  r.set(kGrid, 0.25f);
  r.set(kBeat, 3.7f);
  const std::vector<float> y = r.render(4096, 100);
  CHECK(stepFrames(y).size() == 1);
}

TEST_CASE("noise_step: trig edges step, a held trig steps once, reset restarts the stream") {
  const uint32_t n = 5000;
  Rig r(1, n);
  r.set(kRate, 0.0f);
  float* trig = r.block(kTrig);
  float* reset = r.block(kReset);
  for (uint32_t f = 0; f < n; ++f) trig[f] = reset[f] = boolMask(false);
  for (uint32_t f : {100u, 101u, 700u, 1234u}) trig[f] = boolMask(true);
  for (uint32_t f = 2000; f < 2600; ++f) trig[f] = boolMask(true);  // held
  reset[3000] = boolMask(true);
  for (uint32_t f : {3100u, 3400u}) trig[f] = boolMask(true);
  const std::vector<float> y = r.render(n, 97);
  const std::vector<uint32_t> got = stepFrames(y);
  CHECK(got == std::vector<uint32_t>{0, 100, 700, 1234, 2000, 3000, 3100, 3400});  // 101: still high, no edge
  // the reset puts the lane back at its stream's start: the frame-0 value, then the next draws are the stream at
  // frames 100 and 400 after it
  const uint32_t key = keyOf(r, 0);
  CHECK(y[3000] == uniformOf(mix1(key + 0u)));
  CHECK(y[3000] == y[0]);
  CHECK(y[3100] == uniformOf(mix1(key + 100u)));
  CHECK(y[3400] == uniformOf(mix1(key + 400u)));

  // A trig also puts the rate's phase at 0: its next step a whole period later (a voice's note-on starts its rate).
  Rig p(1, 4000);
  p.set(kRate, 100.0f);  // a step every 480 frames
  float* tp = p.block(kTrig);
  for (uint32_t f = 0; f < 4000; ++f) tp[f] = boolMask(f == 1000);
  const std::vector<float> z = p.render(4000, 64);
  CHECK(stepFrames(z) == std::vector<uint32_t>{0, 480, 960, 1000, 1480, 1960, 2440, 2920, 3400, 3880});
}

TEST_CASE("noise_step: Smooth 0 holds exactly; Smooth > 0 glides monotonically and reaches the value") {
  const uint32_t n = 48000;
  Rig r(1, n);
  r.set(kRate, 37.0f);
  const std::vector<float> held = r.render(n, 300);
  const std::vector<uint32_t> steps = stepFrames(held);
  for (size_t i = 0; i + 1 < steps.size(); ++i)
    for (uint32_t f = steps[i]; f < steps[i + 1]; ++f) REQUIRE(held[f] == held[steps[i]]);
  for (const float s : {0.25f, 1.0f}) {
    INFO("smooth " << s);
    Rig g(1, n);
    g.set(kRate, 37.0f);
    g.set(kSmooth, s);
    const std::vector<float> y = g.render(n, 211);
    const double period = kSr / 37.0;
    for (size_t i = 1; i + 1 < steps.size(); ++i) {  // the same steps and targets as the held render
      const uint32_t f0 = steps[i], f1 = steps[i + 1];
      const float a = y[f0 - 1], b = held[f0];  // from where the output was (a glide of s = 1 may not have finished)
      const uint32_t reach = f0 + static_cast<uint32_t>(std::ceil(s * period));  // u = (k+1)·du ≥ 1, + float slack
      for (uint32_t f = f0; f < f1; ++f) {
        REQUIRE(y[f] >= std::min(a, b));
        REQUIRE(y[f] <= std::max(a, b));
        if (f > f0) REQUIRE((b - a) * (y[f] - y[f - 1]) >= 0.0f);  // monotone toward b
        if (f > f0) REQUIRE(std::abs(y[f] - y[f - 1]) <= 1.5f * std::abs(b - a) / (s * period) + 1e-6f);  // continuous
        if (f >= reach + 1) REQUIRE(y[f] == b);  // reached: exactly the value
      }
    }
  }
}

TEST_CASE("noise_step: the colour design is within ±0.5 dB of its slope with unit power gain") {
  for (int i = -60; i <= 60; ++i) {
    const float c = static_cast<float>(i) * 0.1f;
    INFO("colour " << c);
    const NoiseColourDesign d = noiseColourDesign(c);
    CHECK(d.white == (i == 0));
    if (d.white) continue;
    double lo = 1e9, hi = -1e9;
    for (int k = 0; k <= 400; ++k) {
      const double f = 20.0 * std::pow(1000.0, k / 400.0) / kSr;
      const double e = noiseColourResponseDb(d, f) - c * std::log2(f);
      lo = std::min(lo, e), hi = std::max(hi, e);
    }
    CHECK((hi - lo) / 2.0 <= 0.5);
    // power gain: Σ h² of the cascade's impulse response (double)
    double w[5] = {}, p2 = 0;
    for (int t = 0; t < 2000000; ++t) {
      double x = t == 0 ? d.gain : 0.0;
      for (uint32_t s = 0; s < 4; ++s) {
        const double y = x - d.z[s] * w[s] + kNoiseColourPoles[s] * w[s + 1];
        w[s] = x;
        x = y;
      }
      w[4] = x;
      p2 += x * x;
      if (t > 1000 && std::abs(x) < 1e-12) break;
    }
    CHECK(std::abs(10.0 * std::log10(p2)) <= 0.11);
  }
}

TEST_CASE("noise_step: the kernel is draw → colour cascade → fold or scale (a double reference)") {
  for (const float c : {-6.0f, -3.0f, -0.6f, 0.3f, 2.0f, 6.0f}) {
    for (const float dist : {0.0f, 1.0f}) {
      INFO("colour " << c << " dist " << dist);
      const uint32_t n = 20000;
      Rig r(1, n);
      r.set(kColour, c);
      r.set(kDist, dist);
      const std::vector<float> y = r.render(n, 509);
      const NoiseColourDesign d = noiseColourDesign(c);
      const uint32_t key = keyOf(r, 0);
      double w[5] = {};
      double worst = 0;
      for (uint32_t t = 0; t < n; ++t) {
        const uint32_t h0 = mix1(key + t);
        double v;
        if (dist == 0.0f) {
          v = uniformOf(h0);
        } else {
          const uint32_t h1 = mix1((key ^ 0x6A09E667u) + t);
          const int32_t s = static_cast<int32_t>((h0 >> 16) + (h0 & 0xFFFF) + (h1 >> 16) + (h1 & 0xFFFF)) - 131070;
          v = s * static_cast<double>(kNoiseGaussScale);
        }
        // colours > 0: the input scaled by the bound (and a Gaussian's by its draw's bound): never outside [−1, 1]
        const double scale = c > 0.0f ? d.bound * (dist == 0.0f ? 1.0 : 1.0 / kNoiseGaussBound) : 1.0;
        double x = v * static_cast<double>(static_cast<float>(d.gain * d.bound) / d.bound) * scale;
        for (uint32_t s = 0; s < 4; ++s) {
          const double yy = x - d.z[s] * w[s] + kNoiseColourPoles[s] * w[s + 1];
          w[s] = x;
          x = yy;
        }
        w[4] = x;
        if (c > 0.0f) {
          REQUIRE(std::abs(x) <= 1.0);  // the bound holds for the exact cascade
          REQUIRE(std::abs(y[t]) <= 1.0f);
          worst = std::max(worst, std::abs(x - y[t]));
          continue;
        }
        const double want = noiseFold(static_cast<float>(x));
        // near a fold edge the float cascade may sit on the other side: compare the unfolded value there
        if (std::abs(std::abs(x) - 1.0) > 1e-3) worst = std::max(worst, std::abs(want - y[t]));
      }
      CHECK(worst < 2e-4);
    }
  }
}

TEST_CASE("noise_step: RMS across colours; brown at a slow rate is a continuous walk inside the range" *
          doctest::test_suite("slow")) {
  // Colours ≤ 0 — Gaussian: the fold almost never acts, the RMS is the draw's; uniform: the coloured sequence is
  // near-Gaussian at σ 0.577 and the fold takes up to 1.1 dB of it. Colours > 0: no fold; the RMS is the draw's times
  // the bound (and 1 / 1.1547 for Gaussian draws): exactly as designed (QUESTIONS Q244).
  for (const float dist : {1.0f, 0.0f}) {
    double rms0 = 0;
    for (const float c : {0.0f, -6.0f, -3.0f, -1.0f, 1.0f, 3.0f, 6.0f}) {
      INFO("dist " << dist << " colour " << c);
      const uint32_t n = 1 << 20;
      Rig r(8, n);  // eight independent streams: brown is correlated over ~800 frames
      r.set(kColour, c);
      r.set(kDist, dist);
      const std::vector<float> y = r.render(n, 4096);
      double s2 = 0;
      for (size_t t = static_cast<size_t>(n / 8) * 8; t < y.size(); ++t) s2 += static_cast<double>(y[t]) * y[t];
      const double rms = std::sqrt(s2 / static_cast<double>(y.size() - static_cast<size_t>(n / 8) * 8));
      if (c == 0.0f) rms0 = rms;
      const double db = 20.0 * std::log10(rms / rms0);
      MESSAGE("dist " << dist << " colour " << c << ": " << db << " dB");
      if (c > 0.0f) {
        const double want = 20.0 * std::log10(noiseColourDesign(c).bound * (dist == 1.0f ? 1.0 / kNoiseGaussBound : 1.0));
        CHECK(std::abs(db - want) < 0.15);
      } else if (dist == 1.0f) {
        CHECK(std::abs(db) < 0.3);
      } else {
        CHECK((db < 0.3 && db > -1.4));
      }
    }
  }
  Rig r(1, 1 << 20);
  r.set(kColour, -6.0f);
  r.set(kRate, 50.0f);
  r.set(kSmooth, 1.0f);
  const std::vector<float> y = r.render(1 << 20, 1024);
  float lo = 1, hi = -1, jump = 0;
  for (size_t t = 1; t < y.size(); ++t) {
    lo = std::min(lo, y[t]), hi = std::max(hi, y[t]);
    jump = std::max(jump, std::abs(y[t] - y[t - 1]));
  }
  CHECK(lo >= -1.0f);
  CHECK(hi <= 1.0f);
  CHECK(hi - lo > 0.5f);   // it wanders
  CHECK(jump < 0.002f);    // glided steps of a walk: no jumps
}

TEST_CASE("noise_step: density keeps density / step rate of the steps, the rest exactly 0") {
  {
    const uint32_t n = 1 << 20;
    Rig r(1, n);
    r.set(kDensity, 4800.0f);  // at a step every frame: 10 %
    const std::vector<float> y = r.render(n, 999);
    size_t zeros = 0;
    for (float x : y) zeros += x == 0.0f;
    CHECK(std::abs(1.0 - static_cast<double>(zeros) / n - 0.1) < 0.002);
  }
  {
    const uint32_t n = 48000 * 20;
    Rig r(1, n);
    r.set(kRate, 1000.0f);
    r.set(kDensity, 250.0f);  // a quarter of 1000 steps per second
    const std::vector<float> y = r.render(n, 4000);
    size_t kept = 0, steps = 0;
    for (uint32_t f = 0; f < n; f += 48) {  // one frame per step period
      ++steps;
      kept += y[f] != 0.0f;
    }
    CHECK(std::abs(static_cast<double>(kept) / steps - 0.25) < 0.015);
  }
}

TEST_CASE("noise_step: Steps quantises to N levels across [−1, 1]") {
  for (const float nl : {5.0f, 2.0f, 1.0f, 17.0f}) {
    INFO("steps " << nl);
    Rig r(1, 20000);
    r.set(kSteps, nl);
    const std::vector<float> y = r.render(20000, 64);
    std::vector<float> levels;
    for (float x : y) {
      if (std::find(levels.begin(), levels.end(), x) == levels.end()) levels.push_back(x);
    }
    std::sort(levels.begin(), levels.end());
    const uint32_t want = nl == 1.0f ? 1u : static_cast<uint32_t>(nl);
    CHECK(levels.size() == want);
    if (nl >= 2.0f) {
      for (uint32_t k = 0; k < levels.size(); ++k)
        CHECK(levels[k] == doctest::Approx(-1.0 + 2.0 * k / (nl - 1.0)).epsilon(1e-6));
    } else {
      CHECK(levels[0] == 0.0f);
    }
  }
}

TEST_CASE("noise_step: lanes seeded apart (and voice groups) or one stream; a lane reset continues the stream") {
  const uint32_t n = 2000;
  Rig r(8, n);
  const std::vector<float> y = r.render(n, 111);
  for (uint32_t l = 1; l < 8; ++l) CHECK(y[l] != y[0]);
  Rig g(8, n, 7, 1);  // voice group 1
  const std::vector<float> yg = g.render(n, 111);
  for (uint32_t l = 0; l < 8; ++l) CHECK(yg[l] != y[l]);
  Rig one(8, n);
  one.set(kLanes, 0.0f);
  const std::vector<float> yo = one.render(n, 111);
  for (uint32_t f = 0; f < n; ++f)
    for (uint32_t l = 1; l < 8; ++l) REQUIRE(yo[f * 8 + l] == yo[f * 8]);
  // a 1-lane instance at the same seed is lane 0's stream
  Rig mono(1, n);
  const std::vector<float> ym = mono.render(n, 50);
  for (uint32_t f = 0; f < n; ++f) REQUIRE(ym[f] == y[f * 8]);

  // A voice lane reset (the VoiceEngine's: the per-lane bytes from a fresh state) at a slow rate: the lane draws at its
  // next frame, from the shared counter (not its stream's start: Free), and its seed stays.
  Rig v(8, 4 * 1000);
  v.set(kRate, 5.0f);
  std::vector<float> out(4 * 1000 * 8);
  v.run(0, 1000, out.data());
  StateInit init;
  init.lanes = 8;
  init.sampleRate = kSr;
  init.seed = 7;
  pq::AlignedBuffer<float> fresh(sizeof(NoiseStepState) / sizeof(float));
  initState(Atom::NoiseStep, fresh.data(), init);
  const uint32_t lane = 3, perLane = laneStateInfo(Atom::NoiseStep).perLaneBytes / 4;
  for (uint32_t w = lane; w < perLane; w += 8) v.state[w] = fresh[w];
  v.run(1000, 2000, out.data());
  CHECK(out[1000 * 8 + lane] != out[999 * 8 + lane]);  // drew at once
  CHECK(out[1000 * 8 + lane] == uniformOf(mix1(keyOf(v, lane) + 1000u)));  // the stream at the counter
  CHECK(out[1000 * 8 + 2] == out[999 * 8 + 2]);  // the others hold
}

TEST_CASE("noise_step: any split of the frames into calls gives the same bits; RtScope") {
  REQUIRE(pq::alloc_trap::installed());
  struct Cfg {
    const char* name;
    void (*setup)(Rig&, uint32_t);
  };
  const Cfg cfgs[] = {
      {"white", [](Rig&, uint32_t) {}},
      {"slow glide", [](Rig& r, uint32_t) { r.set(kRate, 7.3f), r.set(kSmooth, 0.6f); }},
      {"fast steps", [](Rig& r, uint32_t) { r.set(kRate, 3000.0f), r.set(kSmooth, 1.0f), r.set(kColour, -2.5f); }},
      {"gauss coloured", [](Rig& r, uint32_t) { r.set(kDist, 1.0f), r.set(kColour, 4.0f), r.set(kDensity, 900.0f), r.set(kSteps, 9.0f); }},
      {"rate block", [](Rig& r, uint32_t n) {
         float* p = r.block(kRate);
         for (uint32_t f = 0; f < n * r.lanes; ++f) p[f] = 100.0f + 900.0f * static_cast<float>((f / r.lanes) % 700) / 700.0f;
         r.set(kSmooth, 0.5f);
       }},
      {"trig + reset + colour block", [](Rig& r, uint32_t n) {
         r.set(kRate, 0.0f);
         float* t = r.block(kTrig);
         float* s = r.block(kReset);
         float* c = r.block(kColour);
         for (uint32_t f = 0; f < n; ++f)
           for (uint32_t l = 0; l < r.lanes; ++l) {
             t[f * r.lanes + l] = boolMask((f + 13 * l) % 37 == 0);
             s[f * r.lanes + l] = boolMask((f + 101 * l) % 997 == 0);
             c[f * r.lanes + l] = -6.0f + 12.0f * static_cast<float>(f % 300) / 300.0f;
           }
         r.set(kSmooth, 0.3f);
       }},
      {"sync", [](Rig& r, uint32_t n) {
         r.set(kRate, 0.0f);
         r.set(kGrid, 0.125f);
         float* b = r.block(kBeat);
         for (uint32_t f = 0; f < n; ++f)
           for (uint32_t l = 0; l < r.lanes; ++l) b[f * r.lanes + l] = static_cast<float>(f * 2.0 / kSr);
         r.set(kSmooth, 1.0f);
       }},
      {"reset pattern", [](Rig& r, uint32_t) { r.set(kRate, 440.0f), r.set(kReset, boolMask(true)); }},
  };
  std::mt19937 rng(5);
  for (const Cfg& cfg : cfgs) {
    for (const uint32_t lanes : {1u, 8u}) {
      INFO(cfg.name << " lanes " << lanes);
      const uint32_t n = 6000;
      Rig whole(lanes, n), split(lanes, n);
      cfg.setup(whole, n);
      cfg.setup(split, n);
      const std::vector<float> a = whole.render(n);
      std::vector<float> b(static_cast<size_t>(n) * lanes + 8);
      uint64_t violations = 0;
      for (uint32_t f = 0; f < n;) {
        const uint32_t k = std::min<uint32_t>(n - f, 1 + rng() % 300);
        violations += pq::alloc_trap::countRtViolations([&] { split.run(f, f + k, b.data()); });
        f += k;
      }
      CHECK(violations == 0);
      for (size_t i = 0; i < a.size(); ++i) REQUIRE(a[i] == b[i]);
      CHECK(std::memcmp(whole.state.data(), split.state.data(), sizeof(NoiseStepState)) == 0);
    }
  }
}
