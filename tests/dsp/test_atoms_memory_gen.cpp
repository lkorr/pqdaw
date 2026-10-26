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

TEST_CASE("dsp atoms: delay state sizes are size_t and cover the ring the kernels address (M1 review H3)") {
  // A mono 2^30-frame ring used to be 2^33 bytes computed in uint32: stateBytes() wrapped to the bare header while
  // initState() set capacity 2^31, and the kernel wrote gigabytes past the block.
  for (uint32_t lanes : {1u, 8u}) {
    const uint32_t limit = pq::dsp::maxDelayFramesLimit(lanes);
    CHECK(limit == (lanes == 1 ? (1u << 28) : (1u << 25)) - 10u);
    for (uint32_t maxDelay : {0u, 1u, 2u, 54u, 1000u, (1u << 20) - 10u, (1u << 20) - 9u, limit - 1u, limit, limit + 1u,
                              1u << 29, 1u << 30, 0xFFFFFFFFu}) {
      INFO("lanes " << lanes << " maxDelay " << maxDelay);
      pq::dsp::StateInit init;
      init.lanes = lanes;
      init.maxDelayFrames = maxDelay;
      const size_t bytes = pq::dsp::stateBytes(Atom::DelayLinear, init);
      // Whatever is asked, a ring of at most 1 GiB (larger maxima are a compile error; the dsp clamps).
      CHECK(bytes <= sizeof(pq::dsp::DelayState) + pq::dsp::kMaxDelayRingBytes);
      // The ring holds every frame a read at the (clamped) maximum reaches: max + 2 back for the 4-point read, and
      // the frame being written.
      const uint64_t effective = std::min<uint64_t>(std::max<uint32_t>(maxDelay, 2u), limit);
      CHECK(bytes >= sizeof(pq::dsp::DelayState) + (effective + 3) * lanes * sizeof(float));
      for (Atom a : {Atom::DelayCubic, Atom::DelayAllpass, Atom::DelayReadLinear, Atom::DelayWrite})
        CHECK(pq::dsp::stateBytes(a, init) == bytes);
      // initState's header describes a ring inside the block.
      if (bytes <= (size_t{1} << 22)) {
        pq::AlignedBuffer<float> st(bytes / 4);
        pq::dsp::initState(Atom::DelayLinear, st.data(), init);
        const auto& h = *reinterpret_cast<const pq::dsp::DelayState*>(st.data());
        CHECK(sizeof(pq::dsp::DelayState) + uint64_t{h.capacity} * h.lanes * sizeof(float) <= bytes);
        CHECK(h.mask == h.capacity - 1);
        CHECK(static_cast<uint64_t>(h.maxDelay) == effective);
        CHECK(uint64_t{h.capacity} >= effective + 3);
      }
    }
  }
}

TEST_CASE("dsp atoms: a delay ring with no slack (max = 2^k - 10) stays inside its state at the maximum delay") {
  // The harness checks canaries after the state block on every call; delays reach and pass the maximum.
  for (int kind = 0; kind < 3; ++kind) {
    const Atom a = kind == 0 ? Atom::DelayLinear : kind == 1 ? Atom::DelayCubic : Atom::DelayAllpass;
    AtomSpec s = spec(a, {uniform(-1, 1), uniform(100, 140)}, delayRef(kind, 118.0), Tol{2e-6, 0});
    s.init.maxDelayFrames = 118;  // + 10 frames of headroom = 128 = the ring, exactly
    checkAtom(s, 40 + static_cast<uint32_t>(kind));
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
        hz = std::isnan(hz) ? 0.0 : std::clamp(hz, -sr / 2, sr / 2);  // NaN hz is 0 Hz (M1 review L3)
        p += hz / sr;
        p -= std::floor(p);
      }
    };
  };
  const Gen hz = pickFrom({20.0f, 261.626f, 440.0f, 1000.5f, -330.0f, 12345.6f, 23999.0f, 30000.0f, -30000.0f,
                           std::numeric_limits<float>::quiet_NaN()});
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

TEST_CASE("dsp atoms: wavetable reads and interpolates the table (a sine table: every level is the same cycle)") {
  // Independent of the level rule: a sine's levels are all the same cycle, so any level choice or crossfade must
  // give sin(2π·phase) up to the linear-interpolation error ((2π/N)²/8 = 7.5e-5 for N = 256). Phases: steady ramps
  // of either sign, and random jumps (any |Δphase|), including phases outside [0, 1).
  constexpr uint32_t N = 256;
  const auto cycle = pq::dsp::Wavetable::basicCycle(pq::dsp::BasicShape::Sine, N);
  auto wt = pq::dsp::Wavetable::build(cycle.span());
  REQUIRE(wt.ok());
  const Gen ramps = [](std::mt19937& rng, uint32_t, uint32_t T) {
    const double inc = std::uniform_real_distribution<double>(-0.5, 0.5)(rng);
    const double p0 = std::uniform_real_distribution<double>(0, 1)(rng);
    std::vector<float> v(T);
    for (uint32_t t = 0; t < T; ++t) {
      const double p = p0 + inc * t;
      v[t] = static_cast<float>(p - std::floor(p));
    }
    return v;
  };
  const RefFn sineRef = [](const V& in, V& out) {
    for (size_t t = 0; t < in[0].size(); ++t) out[0][t] = std::sin(2.0 * std::numbers::pi * in[0][t]);
  };
  AtomSpec s = spec(Atom::Wavetable, {ramps}, sineRef, Tol{1e-4, 0});
  s.resource = &wt->view();
  checkAtom(s, 40);
  s.gens = {uniform(-3, 3)};
  checkAtom(s, 41);
}

namespace {

// Renders `T` samples of the wavetable at a constant frequency (phase computed in double, as a phasor would give
// it) through the 1-lane kernel in blocks of 64, after `warm` samples of the same signal.
std::vector<double> renderWavetable(const pq::dsp::WavetableView& v, double hz, double sr, uint32_t T, uint32_t warm) {
  pq::dsp::WavetableState st{};
  pq::dsp::initState(Atom::Wavetable, &st, {});
  const pq::dsp::KernelFn fn = pq::dsp::findKernel(Atom::Wavetable, 1, 0);
  std::vector<double> out;
  out.reserve(T);
  std::vector<float> ph(64), y(64);
  for (uint32_t t0 = 0; t0 < warm + T; t0 += 64) {
    for (uint32_t i = 0; i < 64; ++i) {
      const double p = hz * static_cast<double>(t0 + i) / sr;
      ph[i] = static_cast<float>(p - std::floor(p));
    }
    const float* ins[] = {ph.data()};
    float* outs[] = {y.data()};
    pq::dsp::KernelArgs a{ins, outs, &st, &v, 64, 1, 1, 0};
    fn(a);
    for (uint32_t i = 0; i < 64; ++i)
      if (t0 + i >= warm && out.size() < T) out.push_back(y[i]);
  }
  return out;
}

}  // namespace

TEST_CASE("dsp atoms: a saw wavetable keeps every harmonic up to 16 kHz through every octave (M1 review M12)") {
  // One level per octave with a hard switch lost every harmonic above sr/4 just past 46.875·2^k Hz (N = 2048 at
  // 48 kHz): 380 Hz had nothing above 12.2 kHz. Independent check by spectrum: render, window (4-term
  // Blackman-Harris, sidelobes −92 dB), FFT, and compare each harmonic with the ideal saw's 2/(πh).
  constexpr uint32_t N = 2048;
  constexpr double sr = 48000.0;
  constexpr uint32_t T = 16384;
  const auto cycle = pq::dsp::Wavetable::basicCycle(pq::dsp::BasicShape::Saw, N);
  auto wt = pq::dsp::Wavetable::build(cycle.span());
  REQUIRE(wt.ok());
  std::vector<double> win(T);
  double winSum = 0;
  for (uint32_t i = 0; i < T; ++i) {
    const double x = 2.0 * std::numbers::pi * i / T;
    win[i] = 0.35875 - 0.48829 * std::cos(x) + 0.14128 * std::cos(2 * x) - 0.01168 * std::cos(3 * x);
    winSum += win[i];
  }
  // Frequencies: 12 per octave from 24 Hz to 8 kHz, plus both sides of every old octave boundary 46.875·2^k.
  std::vector<double> freqs;
  for (double f = 24.0; f <= 8000.0; f *= std::exp2(1.0 / 12.0)) freqs.push_back(f * 1.0137);
  for (double b = 46.875; b <= 8000.0; b *= 2) {
    freqs.push_back(b * 0.995);
    freqs.push_back(b * 1.005);
  }
  double worstTopDb = 0, worstSpurDb = -400;
  double worstTopF = 0, worstSpurF = 0;
  std::vector<double> re(T), im(T);
  for (double f0 : freqs) {
    const std::vector<double> y = renderWavetable(wt->view(), f0, sr, T, 256);
    for (uint32_t i = 0; i < T; ++i) {
      re[i] = y[i] * win[i];
      im[i] = 0;
    }
    pq::dsp::fftInPlace(re, im, false);
    std::vector<double> mag(T / 2);
    for (uint32_t k = 0; k < T / 2; ++k) mag[k] = 2.0 * std::hypot(re[k], im[k]) / winSum;
    const double binHz = sr / T;
    const auto peak = [&](double hz) {
      const auto c = static_cast<int64_t>(std::llround(hz / binHz));
      double m = 0;
      for (int64_t k = std::max<int64_t>(c - 2, 0); k <= std::min<int64_t>(c + 2, T / 2 - 1); ++k)
        m = std::max(m, mag[static_cast<size_t>(k)]);
      return m;
    };
    // Every harmonic up to 16 kHz within 6 dB of the ideal amplitude (linear interpolation and the window's
    // scalloping take at most a few dB at the top).
    for (uint32_t h = 1; h * f0 <= 16000.0; ++h) {
      const double ideal = 2.0 / (std::numbers::pi * h);
      const double db = 20.0 * std::log10(peak(h * f0) / ideal);
      if (db < worstTopDb) {
        worstTopDb = db;
        worstTopF = f0;
      }
      if (db < -6.0) {
        FAIL_CHECK("f0 " << f0 << " Hz: harmonic " << h << " (" << h * f0 << " Hz) at " << db << " dB");
        break;
      }
    }
    // Below 16 kHz, nothing but harmonics: every bin more than 6 bins from a harmonic is a spur (an alias of a
    // harmonic above Nyquist, or an interpolation image), measured relative to the fundamental.
    const double fund = peak(f0);
    for (uint32_t k = 8; k * binHz < 16000.0; ++k) {
      const double hz = k * binHz;
      const double nearest = std::max(1.0, std::round(hz / f0)) * f0;
      if (std::fabs(hz - nearest) <= 6 * binHz) continue;
      const double db = 20.0 * std::log10(std::max(mag[k], 1e-30) / fund);
      if (db > worstSpurDb) {
        worstSpurDb = db;
        worstSpurF = f0;
      }
    }
  }
  MESSAGE("saw wavetable over " << freqs.size() << " frequencies: weakest harmonic up to 16 kHz " << worstTopDb
                                << " dB (at f0 " << worstTopF << " Hz); strongest spur below 16 kHz " << worstSpurDb
                                << " dB (at f0 " << worstSpurF << " Hz)");
  CHECK(worstTopDb > -6.0);
  CHECK(worstSpurDb < -75.0);  // measured −78.9 dB (linear-interpolation images of the lowest levels, f0 ≈ 55 Hz)
}

TEST_CASE("dsp atoms: a phase jump or a fresh state does not pick the wavetable level for one sample (M1 review L3)") {
  // A phasor reset upstream (here: jumps to arbitrary phases) used to select the level from the jump, |Δphase| up to
  // 0.5: a near-sine read of a saw for one sample, a click with drawn tables. The output at each sample must equal
  // the steady-state output at the same phase and frequency: the same kernel primed with three samples of the
  // steady ramp ending at that phase.
  constexpr uint32_t N = 2048;
  const auto cycle = pq::dsp::Wavetable::basicCycle(pq::dsp::BasicShape::Saw, N);
  auto wt = pq::dsp::Wavetable::build(cycle.span());
  REQUIRE(wt.ok());
  const auto& v = wt->view();
  constexpr double inc = 1000.0 / 48000.0;
  std::vector<float> ph;
  double p = 0.3;
  std::mt19937 rng(3);
  std::uniform_real_distribution<double> ud(0, 1);
  for (uint32_t t = 0; t < 2000; ++t) {
    if (t % 97 == 50) p += 0.1 + 0.8 * ud(rng);  // a jump of at least 0.1 cycle either way (5x the increment)
    ph.push_back(static_cast<float>(p - std::floor(p)));
    p += inc;
  }
  const auto steady = [&](float q) {
    pq::dsp::WavetableState st{};
    pq::dsp::initState(Atom::Wavetable, &st, {});
    float in[4], out[4];
    for (int k = 0; k < 4; ++k) {
      const double x = static_cast<double>(q) - (3 - k) * inc;
      in[k] = static_cast<float>(x - std::floor(x));
    }
    const float* ins[] = {in};
    float* outs[] = {out};
    pq::dsp::KernelArgs a{ins, outs, &st, &v, 4, 1, 1, 0};
    pq::dsp::findKernel(Atom::Wavetable, 1, 0)(a);
    return static_cast<double>(out[3]);
  };
  for (uint32_t lanes : {1u, 8u}) {
    pq::dsp::StateInit init;
    init.lanes = lanes;
    pq::dsp::WavetableState st{};
    pq::dsp::initState(Atom::Wavetable, &st, init);
    std::vector<float> in(ph.size() * lanes), out(ph.size() * lanes);
    for (size_t t = 0; t < ph.size(); ++t)
      for (uint32_t l = 0; l < lanes; ++l) in[t * lanes + l] = ph[t];
    for (uint32_t t0 = 0; t0 < ph.size(); t0 += 37) {
      const uint32_t n = std::min<uint32_t>(37, static_cast<uint32_t>(ph.size()) - t0);
      const float* ins[] = {in.data() + static_cast<size_t>(t0) * lanes};
      float* outs[] = {out.data() + static_cast<size_t>(t0) * lanes};
      pq::dsp::KernelArgs a{ins, outs, &st, &v, n, 1, 1, 0};
      pq::dsp::findKernel(Atom::Wavetable, lanes, 0)(a);
    }
    double worst = 0, first = 0;
    for (size_t t = 0; t < ph.size(); ++t)
      for (uint32_t l = 0; l < lanes; ++l) {
        const double d = std::fabs(static_cast<double>(out[t * lanes + l]) - steady(ph[t]));
        if (t == 0) first = std::max(first, d);
        else worst = std::max(worst, d);
      }
    INFO("lanes " << lanes);
    // After a jump: the steady output. The first sample of a fresh state reads the full-band level, which away
    // from the saw's edge differs from the band-limited value only by the Gibbs ripple of the missing harmonics.
    CHECK(worst < 1e-5);
    CHECK(first < 0.02);
  }
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
        // Inside the loop the taps past the seam come from the other end of the loop (M1 review L3).
        const bool inLoop = loopOn && static_cast<double>(ci) >= ls && static_cast<double>(ci) < le;
        const auto tap = [&](int64_t j) {
          if (inLoop) {
            const auto ils = static_cast<int64_t>(ls), ile = static_cast<int64_t>(le);
            while (j >= ile) j -= ile - ils;
            while (j < ils) j += ile - ils;
          }
          return at(j);
        };
        const double fr = pos - std::floor(pos);
        const double y = cubic ? hermiteRef(tap(ci - 1), tap(ci), tap(ci + 1), tap(ci + 2), fr)
                               : tap(ci) + fr * (tap(ci + 1) - tap(ci));
        out[0][t] = active ? y : 0.0;
        out[1][t] = active ? 1.0 : 0.0;
        if (active) {
          // The documented playhead: rate in 32.32 fixed point (floor(rate·2^32), exact in double here), and a
          // loop crossed in either direction is re-entered as often as it takes (so any rate stays in the loop).
          // Positions stay below 2^20 with 2^-32 steps: exact in double.
          double next = pos + std::floor(in[0][t] * 0x1p32) * 0x1p-32;
          if (loopOn && static_cast<double>(idx) < le && std::floor(next) >= le) {
            while (std::floor(next) >= le) next -= (le - ls);
          } else if (loopOn && static_cast<double>(idx) >= ls && std::floor(next) < ls) {
            while (std::floor(next) < ls) next += (le - ls);
          }
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
    // Tiny rates of either sign (M1 review M11a: for rate in [-2^-25, 0) the fraction rounded to 1.0 and the
    // playhead moved a whole sample backwards per sample), param and audio rate.
    r.gens[0] = pickFrom({-1e-9f, -2.9e-8f, -3e-8f, -1e-6f, -1e-12f, 1e-9f, -0.4999f, -0.5f, -0.0f});
    r.gens[3] = pickFrom({0.0f});
    r.gens[4] = pickFrom({0.0f});
    checkAtom(r, 72);
    // Rates beyond the loop length, both directions (M1 review M11b: one jump of the loop length left the loop for
    // good). Loops of 1 to 5 samples; start inside, before and after the loop.
    r.gens[0] = pickFrom({2.0f, 3.5f, 7.25f, -5.5f, 64.0f, -64.0f, 1.0f, -1.0f, 1000.75f});
    r.gens[2] = pickFrom({0.0f, 3.0f, 3.5f, 10.0f});
    r.gens[3] = pickFrom({3.0f});
    r.gens[4] = pickFrom({4.0f, 5.0f, 8.0f});
    checkAtom(r, 73);
    r.gens[0] = uniform(-40, 40);
    checkAtom(r, 74);
  }
  CHECK(!pq::dsp::SampleData::build(std::vector<float>{}).ok());
}

TEST_CASE("dsp atoms: sample_read interpolates across the loop seam from the loop's other end (M1 review L3)") {
  // Sample k = k (a ramp), loop [10, 14), start 12, rate 0.5: frames read 12, 12.5, 13, 13.5, 10, 10.5 …; at 13.5
  // the linear taps are 13 and 10 (the sample after loopEnd − 1 inside the loop is loopStart), not 13 and 14.
  std::vector<float> ramp(32);
  for (size_t k = 0; k < ramp.size(); ++k) ramp[k] = static_cast<float>(k);
  auto data = pq::dsp::SampleData::build(ramp);
  REQUIRE(data.ok());
  for (const bool cubic : {false, true}) {
    for (const uint32_t lanes : {1u, 8u}) {
      INFO("cubic " << cubic << ", lanes " << lanes);
      const Atom atom = cubic ? Atom::SampleReadCubic : Atom::SampleReadLinear;
      constexpr uint32_t T = 8;
      std::vector<float> rate(8, 0.5f), start(8, 12.0f), ls(8, 10.0f), le(8, 14.0f);
      std::vector<float> reset(T * lanes, 0.0f);
      for (uint32_t l = 0; l < lanes; ++l) reset[l] = pq::dsp::boolMask(true);  // reset at frame 0
      std::vector<float> out(T * lanes), playing(T * lanes);
      pq::dsp::SampleReadState st{};
      pq::dsp::StateInit init;
      init.lanes = lanes;
      pq::dsp::initState(atom, &st, init);
      const float* ins[] = {rate.data(), reset.data(), start.data(), ls.data(), le.data()};
      float* outs[] = {out.data(), playing.data()};
      const uint32_t spl = 0b11101;  // rate, start, loopStart, loopEnd at param rate; reset per sample
      pq::dsp::KernelArgs args{ins, outs, &st, &data->view(), T, 5, 2, spl};
      pq::dsp::findKernel(atom, lanes, spl)(args);
      // Positions 12, 12.5, 13, 13.5, 10, 10.5, 11, 11.5; loop-wrapped taps: s(j) = 10 + (j − 10) mod 4.
      const auto s = [](int j) { return static_cast<double>(10 + ((j - 10) % 4 + 4) % 4); };
      for (uint32_t t = 0; t < T; ++t) {
        const double pos = 12.0 + 0.5 * t >= 14.0 ? 12.0 + 0.5 * t - 4.0 : 12.0 + 0.5 * t;
        const int i = static_cast<int>(std::floor(pos));
        const double fr = pos - i;
        const double want = cubic ? hermiteRef(s(i - 1), s(i), s(i + 1), s(i + 2), fr) : s(i) + fr * (s(i + 1) - s(i));
        for (uint32_t l = 0; l < lanes; ++l) CHECK(out[t * lanes + l] == doctest::Approx(want).epsilon(1e-6));
      }
      // The case that read past the seam before: 13.5 → (13 + 10) / 2 = 11.5, not 13.5.
      if (!cubic) CHECK(out[3 * lanes] == 11.5f);
    }
  }
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
    const size_t bytes = pq::dsp::stateBytes(Atom::DelayLinear, init);
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
    const size_t bytes = pq::dsp::stateBytes(a, init);
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
