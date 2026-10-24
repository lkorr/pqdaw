// Memory, generator and table atoms against naive double references.

#include <doctest/doctest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <numbers>
#include <vector>

#include "atom_harness.h"
#include "pq/dsp/atom_state.h"
#include "pq/dsp/tables.h"

using namespace pqtest;
using pq::dsp::Atom;

namespace {

using V = std::vector<std::vector<double>>;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

AtomSpec spec(Atom a, std::vector<Gen> gens, RefFn ref, Tol tol = {}) {
  AtomSpec s;
  s.atom = a;
  s.gens = std::move(gens);
  s.ref = std::move(ref);
  s.tol = {tol};
  return s;
}

double hermiteRef(double xm1, double x0, double x1, double x2, double t) {
  const double c1 = 0.5 * (x1 - xm1);
  const double c2 = xm1 - 2.5 * x0 + 2.0 * x1 - 0.5 * x2;
  const double c3 = 0.5 * (x2 - xm1) + 1.5 * (x0 - x1);
  return ((c3 * t + c2) * t + c1) * t + x0;
}

// Delay-line reference: full history, x written before the read.
RefFn delayRef(int kind, double maxDelay) {
  return [kind, maxDelay](const V& in, V& out) {
    std::vector<double> h;
    auto at = [&](int64_t n) { return n >= 0 ? h[static_cast<size_t>(n)] : 0.0; };
    double y1 = 0;
    const double dMin = kind == 0 ? 1.0 : kind == 1 ? 2.0 : 1.5;
    for (size_t t = 0; t < in[0].size(); ++t) {
      h.push_back(in[0][t]);
      const int64_t n = static_cast<int64_t>(t);
      double d = in[1][t];
      d = std::isnan(d) ? dMin : std::clamp(d, dMin, maxDelay);
      int64_t i = static_cast<int64_t>(std::floor(d));
      double f = d - std::floor(d);
      if (kind == 0) {
        out[0][t] = at(n - i) + f * (at(n - i - 1) - at(n - i));
      } else if (kind == 1) {
        out[0][t] = hermiteRef(at(n - i + 1), at(n - i), at(n - i - 1), at(n - i - 2), f);
      } else {
        if (f < 0.5) {
          f += 1.0;
          i -= 1;
        }
        const double a = (1.0 - f) / (1.0 + f);
        const double y = a * (at(n - i) - y1) + at(n - i - 1);
        y1 = y;
        out[0][t] = y;
      }
    }
  };
}

}  // namespace

TEST_CASE("dsp atoms: z1, sample&hold, latch") {
  checkAtom(spec(Atom::Z1, {uniform(-1, 1)}, [](const V& in, V& out) {
    for (size_t t = 0; t < in[0].size(); ++t) out[0][t] = t == 0 ? 0.0 : in[0][t - 1];
  }));
  checkAtom(spec(Atom::SampleHold, {uniform(-1, 1), boolRuns(0.1)}, [](const V& in, V& out) {
    double held = 0;
    bool prev = false;
    for (size_t t = 0; t < in[0].size(); ++t) {
      const bool b = in[1][t] != 0;
      if (b && !prev) held = in[0][t];
      prev = b;
      out[0][t] = held;
    }
  }));
  checkAtom(spec(Atom::Latch, {uniform(-1, 1), boolRuns(0.1)}, [](const V& in, V& out) {
    double held = 0;
    for (size_t t = 0; t < in[0].size(); ++t) {
      if (in[1][t] != 0) held = in[0][t];
      out[0][t] = held;
    }
  }));
}

TEST_CASE("dsp atoms: delay lines (linear, cubic, allpass)") {
  for (int kind = 0; kind < 3; ++kind) {
    const Atom a = kind == 0 ? Atom::DelayLinear : kind == 1 ? Atom::DelayCubic : Atom::DelayAllpass;
    AtomSpec s = spec(a, {uniform(-1, 1), uniform(-2, 110)}, delayRef(kind, 100.0), Tol{2e-6, 0});
    s.init.maxDelayFrames = 100;
    checkAtom(s, 10 + static_cast<uint32_t>(kind));
    // Slowly modulated delay (the musical case) and NaN delays (clamped to the minimum).
    s.gens[1] = [](std::mt19937& rng, uint32_t lane, uint32_t T) {
      std::vector<float> v(T);
      const double rate = 0.01 + 0.003 * lane + 0.001 * std::uniform_real_distribution<double>(0, 1)(rng);
      for (uint32_t t = 0; t < T; ++t) v[t] = static_cast<float>(50.0 + 40.0 * std::sin(rate * t));
      if (T > 10) v[T / 2] = std::numeric_limits<float>::quiet_NaN();
      return v;
    };
    checkAtom(s, 20 + static_cast<uint32_t>(kind));
  }
}

TEST_CASE("dsp atoms: delay line with the wrong lane layout outputs silence instead of touching memory") {
  pq::dsp::StateInit init;
  init.lanes = 1;
  init.maxDelayFrames = 16;
  pq::AlignedBuffer<float> state(pq::dsp::stateBytes(Atom::DelayLinear, init) / 4);
  pq::dsp::initState(Atom::DelayLinear, state.data(), init);
  float x[8 * 4], d[8], out[8 * 4];
  std::fill(std::begin(x), std::end(x), 1.0f);
  std::fill(std::begin(d), std::end(d), 2.0f);
  std::fill(std::begin(out), std::end(out), 7.0f);
  const float* ins[] = {x, d};
  float* outs[] = {out};
  pq::dsp::KernelArgs args{ins, outs, state.data(), nullptr, 4, 2, 1, 2};
  pq::dsp::findKernel(Atom::DelayLinear, 8, 2)(args);
  for (float v : out) CHECK(v == 0.0f);
}

TEST_CASE("dsp atoms: phasor and sine_hz against a double phase accumulator") {
  const double sr = 48000.0;
  const auto phasorRef = [sr](bool sine) {
    return [sr, sine](const V& in, V& out) {
      double p = 0;
      for (size_t t = 0; t < in[0].size(); ++t) {
        if (in[1][t] != 0) p = 0;
        out[0][t] = sine ? std::sin(2.0 * std::numbers::pi * p) : p;
        double hz = in[0][t];
        hz = std::isnan(hz) ? -sr / 2 : std::clamp(hz, -sr / 2, sr / 2);
        p += hz / sr;
        p -= std::floor(p);
      }
    };
  };
  const Gen hz = pickFrom({20.0f, 261.626f, 440.0f, 1000.5f, -330.0f, 12345.6f, 23999.0f, 30000.0f, -30000.0f});
  for (bool sine : {false, true}) {
    AtomSpec s = spec(sine ? Atom::SineHz : Atom::Phasor, {hz, boolRuns(0.01)}, phasorRef(sine),
                      sine ? Tol{2e-6, 0} : Tol{1e-6, 0, 1.0});
    s.init.sampleRate = sr;
    checkAtom(s, sine ? 31u : 30u);
    // Audio-rate frequency (FM-like) without resets.
    s.gens = {uniform(-5000, 5000), constant(0.0f)};
    checkAtom(s, sine ? 33u : 32u);
  }
}

TEST_CASE("dsp atoms: noise is the documented counter hash, independent of block size, and uniform") {
  const auto lowbias32 = [](uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
  };
  for (uint32_t lanes : {1u, 8u}) {
    pq::dsp::StateInit init;
    init.lanes = lanes;
    init.seed = 1234;
    AtomSpec s;
    s.atom = Atom::Noise;
    s.init = init;
    s.T = 1000;
    s.maxBlock = 100;
    std::mt19937 rng(5);
    const auto blocksA = randomBlocks(rng, s.T, 67);
    const auto blocksB = std::vector<uint32_t>(10, 100);
    const auto ra = runKernel(s, lanes, 0, {}, blocksA, 1);
    const auto rb = runKernel(s, lanes, 0, {}, blocksB, 1);
    CHECK(ra.out[0] == rb.out[0]);
    pq::AlignedBuffer<float> st(pq::dsp::stateBytes(Atom::Noise, init) / 4);
    pq::dsp::initState(Atom::Noise, st.data(), init);
    const auto& ns = *reinterpret_cast<const pq::dsp::NoiseState*>(st.data());
    for (uint32_t l = 0; l < 8; ++l)
      for (uint32_t k = l + 1; k < 8; ++k) CHECK(ns.seed[l] != ns.seed[k]);
    double sum = 0, sumSq = 0;
    float lo = 1, hi = -1;
    for (uint32_t t = 0; t < s.T; ++t)
      for (uint32_t l = 0; l < lanes; ++l) {
        const uint32_t h = lowbias32(t + ns.seed[l]);
        const double ref = static_cast<double>(static_cast<int32_t>(h) >> 8) * 1.1920928955078125e-07;
        const float got = ra.out[0][static_cast<size_t>(t) * lanes + l];
        REQUIRE(static_cast<double>(got) == ref);
        sum += got;
        sumSq += static_cast<double>(got) * got;
        lo = std::min(lo, got);
        hi = std::max(hi, got);
      }
    const double n = static_cast<double>(s.T) * lanes;
    CHECK(std::abs(sum / n) < 0.05);
    CHECK(std::abs(sumSq / n - 1.0 / 3.0) < 0.03);
    CHECK(lo >= -1.0f);
    CHECK(hi < 1.0f);
  }
}

TEST_CASE("dsp atoms: wavetable read with mip selection against a double reference") {
  constexpr uint32_t N = 256;
  const auto cycle = pq::dsp::Wavetable::basicCycle(pq::dsp::BasicShape::Saw, N);
  auto wt = pq::dsp::Wavetable::build(cycle.span());
  REQUIRE(wt.ok());
  const uint32_t levels = wt->view().levels;
  REQUIRE(levels == 8);
  // Reference levels by naive DFT in double.
  std::vector<std::vector<double>> refLevels(levels, std::vector<double>(N + 1));
  std::vector<double> cRe(N / 2 + 1), cIm(N / 2 + 1);
  for (uint32_t h = 0; h <= N / 2; ++h)
    for (uint32_t i = 0; i < N; ++i) {
      const double ang = 2.0 * std::numbers::pi * h * i / N;
      cRe[h] += cycle[i] * std::cos(ang);
      cIm[h] -= cycle[i] * std::sin(ang);
    }
  for (uint32_t k = 0; k < levels; ++k) {
    const uint32_t maxH = (N / 2) >> k;
    for (uint32_t i = 0; i < N; ++i) {
      double v = cRe[0] / N;
      for (uint32_t h = 1; h <= maxH && h < N / 2; ++h) {
        const double ang = 2.0 * std::numbers::pi * h * i / N;
        v += 2.0 / N * (cRe[h] * std::cos(ang) - cIm[h] * std::sin(ang));
      }
      refLevels[k][i] = v;
    }
    refLevels[k][N] = refLevels[k][0];
  }
  // Phase ramps whose increment sits well inside one mip level's range: N·inc in 2^k·[0.55, 0.95].
  const Gen ramps = [](std::mt19937& rng, uint32_t, uint32_t T) {
    std::uniform_int_distribution<int> kd(0, 8);
    std::uniform_real_distribution<double> ud(0.55, 0.95);
    const double inc = std::ldexp(ud(rng), kd(rng)) / N * (std::bernoulli_distribution(0.3)(rng) ? -1.0 : 1.0);
    const double p0 = std::uniform_real_distribution<double>(0, 1)(rng);
    std::vector<float> v(T);
    for (uint32_t t = 0; t < T; ++t) {
      const double p = p0 + inc * t;
      v[t] = static_cast<float>(p - std::floor(p));
    }
    return v;
  };
  AtomSpec s = spec(Atom::Wavetable, {ramps}, [&](const V& in, V& out) {
    double prev = 0;
    for (size_t t = 0; t < in[0].size(); ++t) {
      const double p = in[0][t];
      double d = p - prev;
      d = std::abs(d - std::nearbyint(d));
      prev = p;
      const double x = static_cast<double>(static_cast<float>(d * N));
      int lvl = x > 0 ? static_cast<int>(std::ceil(std::log2(x))) : 0;
      lvl = std::clamp(lvl, 0, static_cast<int>(levels) - 1);
      const double pos = (p - std::floor(p)) * N;
      const double fl = std::floor(pos);
      const size_t i = static_cast<size_t>(fl) & (N - 1);
      const auto& tb = refLevels[static_cast<size_t>(lvl)];
      out[0][t] = tb[i] + (pos - fl) * (tb[i + 1] - tb[i]);
    }
  }, Tol{3e-6, 0});
  s.resource = &wt->view();
  checkAtom(s, 40);
}

TEST_CASE("dsp atoms: shaper against a double reference") {
  constexpr uint32_t P = 33;
  std::vector<float> pts(P);
  for (uint32_t i = 0; i < P; ++i) pts[i] = static_cast<float>(std::tanh(1.5 * (-2.0 + 4.0 * i / (P - 1))) + 0.1 * i);
  auto sh = pq::dsp::ShaperTable::build(pts, -2.0f, 2.0f);
  REQUIRE(sh.ok());
  AtomSpec s = spec(Atom::Shaper, {uniform(-3, 3)}, [&](const V& in, V& out) {
    for (size_t t = 0; t < in[0].size(); ++t) {
      double x = (in[0][t] + 2.0) * (P - 1) / 4.0;
      x = std::clamp(x, 0.0, static_cast<double>(P - 1));
      const size_t i = std::min(static_cast<size_t>(x), static_cast<size_t>(P - 2));
      out[0][t] = pts[i] + (x - static_cast<double>(i)) * (pts[i + 1] - pts[i]);
    }
  }, Tol{3e-6, 0});
  s.resource = &sh->view();
  checkAtom(s, 50);
  CHECK(!pq::dsp::ShaperTable::build(pts, 1.0f, 1.0f).ok());
  CHECK(!pq::dsp::ShaperTable::build(std::vector<float>{1.0f}, 0.0f, 1.0f).ok());
}

TEST_CASE("dsp atoms: sample_pos and sample_read against a double playhead") {
  constexpr int32_t len = 200;
  std::vector<float> smp(len);
  std::mt19937 rng(7);
  std::uniform_real_distribution<float> ud(-1, 1);
  for (auto& x : smp) x = ud(rng);
  auto data = pq::dsp::SampleData::build(smp);
  REQUIRE(data.ok());
  auto at = [&](int64_t i) { return i >= 0 && i < len ? static_cast<double>(smp[static_cast<size_t>(i)]) : 0.0; };
  auto interp = [&](bool cubic, int64_t i, double fr) {
    return cubic ? hermiteRef(at(i - 1), at(i), at(i + 1), at(i + 2), fr) : at(i) + fr * (at(i + 1) - at(i));
  };
  for (bool cubic : {false, true}) {
    AtomSpec s = spec(cubic ? Atom::SamplePosCubic : Atom::SamplePosLinear, {uniform(-5, 210)}, [&, cubic](const V& in, V& out) {
      for (size_t t = 0; t < in[0].size(); ++t) {
        const double p = in[0][t];
        if (!(p >= 0 && p < len)) {
          out[0][t] = 0;
          continue;
        }
        out[0][t] = interp(cubic, static_cast<int64_t>(std::floor(p)), p - std::floor(p));
      }
    }, Tol{2e-6, 0});
    s.resource = &data->view();
    checkAtom(s, 60);

    const auto readRef = [&, cubic](const V& in, V& out) {
      double pos = 0;
      bool active = true;
      for (size_t t = 0; t < in[0].size(); ++t) {
        const double ls = std::floor(in[3][t]), le = std::floor(in[4][t]);
        const bool loopOn = le > ls;
        if (in[1][t] != 0) {
          pos = in[2][t];
          active = std::floor(pos) >= 0 && std::floor(pos) < len;
        }
        const int64_t idx = static_cast<int64_t>(std::floor(pos));
        const int64_t ci = std::clamp<int64_t>(idx, 0, len - 1);
        out[0][t] = active ? interp(cubic, ci, pos - std::floor(pos)) : 0.0;
        out[1][t] = active ? 1.0 : 0.0;
        if (active) {
          double next = pos + in[0][t];
          const double nidx = std::floor(next);
          if (loopOn && static_cast<double>(idx) < le && nidx >= le) next -= (le - ls);
          else if (loopOn && static_cast<double>(idx) >= ls && nidx < ls) next += (le - ls);
          pos = next;
          active = std::floor(pos) >= 0 && std::floor(pos) < len;
        }
      }
    };
    AtomSpec r = spec(cubic ? Atom::SampleReadCubic : Atom::SampleReadLinear,
                      {pickFrom({1.0f, 0.5f, -0.75f, 1.25f, 2.0f, 0.3125f, -1.5f}), boolRuns(0.02), uniform(0, 150),
                       pickFrom({20.0f, 50.5f}), pickFrom({0.0f, 80.0f, 120.7f})},
                      readRef, Tol{2e-6, 0});
    r.tol.push_back(Tol{});
    r.resource = &data->view();
    r.T = 900;
    checkAtom(r, 70);
    // Audio-rate rate input.
    r.gens[0] = uniform(-2, 2);
    checkAtom(r, 71);
  }
  CHECK(!pq::dsp::SampleData::build(std::vector<float>{}).ok());
}

namespace {

// Runs a kernel for one block with Blk inputs (layout [t*lanes + lane]).
void runBlock(Atom a, uint32_t lanes, void* state, std::vector<const float*> ins, std::vector<float*> outs,
              uint32_t frames) {
  pq::dsp::KernelArgs args{ins.data(), outs.data(), state, nullptr, frames, static_cast<uint16_t>(ins.size()),
                           static_cast<uint16_t>(outs.size()), 0};
  pq::dsp::findKernel(a, lanes, 0)(args);
}

}  // namespace

TEST_CASE("dsp atoms: delay and z1 split kernels (per-sample feedback) match the atoms bit for bit") {
  constexpr uint32_t T = 600;
  const Atom atoms[] = {Atom::DelayLinear, Atom::DelayCubic, Atom::DelayAllpass};
  const Atom reads[] = {Atom::DelayReadLinear, Atom::DelayReadCubic, Atom::DelayReadAllpass};
  for (uint32_t lanes : {1u, 8u}) {
    std::mt19937 rng(90 + lanes);
    std::uniform_real_distribution<float> ux(-1, 1), ud(-1, 70);
    std::vector<float> x(T * lanes), d(T * lanes);
    for (auto& v : x) v = ux(rng);
    for (auto& v : d) v = ud(rng);
    pq::dsp::StateInit init;
    init.lanes = lanes;
    init.maxDelayFrames = 64;
    const uint32_t bytes = pq::dsp::stateBytes(Atom::DelayLinear, init);
    CHECK(pq::dsp::stateBytes(Atom::DelayWrite, init) == bytes);
    for (int k = 0; k < 3; ++k) {
      pq::AlignedBuffer<float> s1(bytes / 4), s2(bytes / 4);
      pq::dsp::initState(atoms[k], s1.data(), init);
      pq::dsp::initState(reads[k], s2.data(), init);
      std::vector<float> fused(T * lanes), split(T * lanes);
      const auto blocks = randomBlocks(rng, T, 67);
      uint32_t t0 = 0;
      for (uint32_t b : blocks) {
        runBlock(atoms[k], lanes, s1.data(), {x.data() + t0 * lanes, d.data() + t0 * lanes}, {fused.data() + t0 * lanes}, b);
        t0 += b;
      }
      for (uint32_t t = 0; t < T; ++t) {
        runBlock(reads[k], lanes, s2.data(), {d.data() + t * lanes}, {split.data() + t * lanes}, 1);
        runBlock(Atom::DelayWrite, lanes, s2.data(), {x.data() + t * lanes}, {}, 1);
      }
      for (uint32_t i = 0; i < T * lanes; ++i) REQUIRE(std::bit_cast<uint32_t>(fused[i]) == std::bit_cast<uint32_t>(split[i]));
    }
    // z1
    pq::dsp::Z1State z1a{}, z1b{};
    std::vector<float> fused(T * lanes), split(T * lanes);
    runBlock(Atom::Z1, lanes, &z1a, {x.data()}, {fused.data()}, T);
    for (uint32_t t = 0; t < T; ++t) {
      runBlock(Atom::Z1Read, lanes, &z1b, {}, {split.data() + t * lanes}, 1);
      runBlock(Atom::Z1Write, lanes, &z1b, {x.data() + t * lanes}, {}, 1);
    }
    CHECK(fused == split);
  }
}

TEST_CASE("dsp atoms: z1 starts at StateInit::z1Init (the language's `init`), fused and split") {
  for (uint32_t lanes : {1u, 8u}) {
    pq::dsp::StateInit init;
    init.lanes = lanes;
    init.z1Init = 0.25f;
    for (Atom a : {Atom::Z1, Atom::Z1Read, Atom::Z1Write}) {
      REQUIRE(pq::dsp::stateBytes(a, init) == sizeof(pq::dsp::Z1State));
      pq::dsp::Z1State s;
      pq::dsp::initState(a, &s, init);
      for (float v : s.last) CHECK(v == 0.25f);
    }
    pq::dsp::Z1State s;
    pq::dsp::initState(Atom::Z1, &s, init);
    std::vector<float> x(4 * lanes), y(4 * lanes);
    for (uint32_t i = 0; i < x.size(); ++i) x[i] = static_cast<float>(i + 1);
    runBlock(Atom::Z1, lanes, &s, {x.data()}, {y.data()}, 4);
    for (uint32_t l = 0; l < lanes; ++l) {
      CHECK(y[l] == 0.25f);                  // first output: the init value
      CHECK(y[lanes + l] == x[l]);           // then x delayed by one sample
    }
  }
}

TEST_CASE("dsp atoms: a feedback comb filter through the split delay kernels matches a double reference") {
  // y[n] = x[n] + g·delay(y)[n], delay modulated around 20.5 samples; per-sample read → math → write.
  constexpr uint32_t T = 3000;
  constexpr float g = 0.7f;
  for (int k = 0; k < 3; ++k) {
    const Atom read = k == 0 ? Atom::DelayReadLinear : k == 1 ? Atom::DelayReadCubic : Atom::DelayReadAllpass;
    pq::dsp::StateInit init;
    init.lanes = 1;
    init.maxDelayFrames = 32;
    pq::AlignedBuffer<float> st(pq::dsp::stateBytes(read, init) / 4);
    pq::dsp::initState(read, st.data(), init);
    std::vector<float> x(T), d(T), y(T);
    for (uint32_t t = 0; t < T; ++t) {
      x[t] = t < 5 ? 1.0f : static_cast<float>(0.1 * std::sin(0.05 * t));
      d[t] = static_cast<float>(20.5 + 3.0 * std::sin(0.002 * t));
    }
    for (uint32_t t = 0; t < T; ++t) {
      float r = 0;
      runBlock(read, 1, st.data(), {&d[t]}, {&r}, 1);
      y[t] = x[t] + g * r;
      runBlock(Atom::DelayWrite, 1, st.data(), {&y[t]}, {}, 1);
    }
    // Reference: same recursion in double.
    std::vector<double> yr(T);
    double y1 = 0;
    auto at = [&](int64_t n) { return n >= 0 ? yr[static_cast<size_t>(n)] : 0.0; };
    double worst = 0;
    for (uint32_t t = 0; t < T; ++t) {
      const int64_t n = t;
      double dd = d[t];
      int64_t i = static_cast<int64_t>(std::floor(dd));
      double f = dd - std::floor(dd);
      double r;
      if (k == 0) {
        r = at(n - i) + f * (at(n - i - 1) - at(n - i));
      } else if (k == 1) {
        r = hermiteRef(at(n - i + 1), at(n - i), at(n - i - 1), at(n - i - 2), f);
      } else {
        if (f < 0.5) {
          f += 1;
          i -= 1;
        }
        const double c = (1 - f) / (1 + f);
        r = c * (at(n - i) - y1) + at(n - i - 1);
        y1 = r;
      }
      yr[t] = static_cast<double>(x[t]) + static_cast<double>(g) * r;
      worst = std::max(worst, std::abs(yr[t] - static_cast<double>(y[t])));
    }
    CHECK(worst < 1e-5);
  }
}

// T1.8-fix1 (M1 review M9): an 8-lane delay line resets one lane in O(1) by setting DelayState::start[lane] to the
// write position: from then on that lane reads exactly what a ring zeroed at that moment would give, the other
// lanes are untouched, and the per-lane history stays bounded by the capacity (start is clamped) over many wraps.
TEST_CASE("dsp atoms: 8-lane delay lines reset one lane through DelayState::start, bit for bit like a zeroed ring") {
  constexpr uint32_t kB = 37;  // block length (not a multiple of 8)
  for (Atom a : {Atom::DelayLinear, Atom::DelayCubic, Atom::DelayAllpass}) {
    INFO("atom " << static_cast<int>(a));
    pq::dsp::StateInit init;
    init.lanes = 8;
    init.maxDelayFrames = 64;
    const uint32_t bytes = pq::dsp::stateBytes(a, init);
    pq::AlignedBuffer<float> sa(bytes / 4), sb(bytes / 4);
    pq::dsp::initState(a, sa.data(), init);
    pq::dsp::initState(a, sb.data(), init);
    auto& A = *reinterpret_cast<pq::dsp::DelayState*>(sa.data());
    const uint32_t cap = A.capacity;
    const pq::dsp::KernelFn k = pq::dsp::findKernel(a, 8, 0);
    REQUIRE(k != nullptr);
    std::vector<float> x(kB * 8), d(kB * 8), oa(kB * 8), ob(kB * 8);
    uint32_t rng = 5;
    auto rnd = [&] {
      rng = rng * 1664525u + 1013904223u;
      return static_cast<float>(rng >> 8) / 16777216.0f;
    };
    auto run = [&](void* st, const std::vector<float>& in, std::vector<float>& out) {
      const float* ins[] = {in.data(), d.data()};
      float* outs[] = {out.data()};
      pq::dsp::KernelArgs args{ins, outs, st, nullptr, kB, 2, 1, 0};
      k(args);
    };
    // Phase 1: A hears noise in every lane; B hears the same except lane 3, which stays silent.
    for (uint32_t blk = 0; blk < 6; ++blk) {
      std::vector<float> xb(kB * 8);
      for (uint32_t i = 0; i < kB * 8; ++i) {
        x[i] = rnd() - 0.5f;
        xb[i] = (i & 7u) == 3 ? 0.0f : x[i];
        d[i] = 2.0f + 62.0f * rnd();  // up to maxDelay: every tap distance gets exercised
      }
      run(sa.data(), x, oa);
      run(sb.data(), xb, ob);
    }
    // Reset lane 3 of A in O(1); B's lane 3 has only ever seen silence.
    A.start[3] = A.writePos;
    A.apY1[3] = 0.0f;
    // Phase 2: identical input; many ring wraps (start must be clamped, never wrap around).
    for (uint32_t blk = 0; blk < 4 * cap / kB; ++blk) {
      for (uint32_t i = 0; i < kB * 8; ++i) {
        x[i] = rnd() - 0.5f;
        d[i] = 1.0f + 63.0f * rnd();
      }
      run(sa.data(), x, oa);
      run(sb.data(), x, ob);
      for (uint32_t i = 0; i < kB * 8; ++i) REQUIRE(std::bit_cast<uint32_t>(oa[i]) == std::bit_cast<uint32_t>(ob[i]));
      for (uint32_t l = 0; l < 8; ++l) REQUIRE(A.writePos - A.start[l] <= cap);
    }
  }
}
