// Math and logic atoms against naive double references (both lane counts, every Blk/Spl combination,
// random block partitions including blocks that are not a multiple of 8 frames).

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <set>
#include <string>

#include "atom_harness.h"

using namespace pqtest;
using pq::dsp::Atom;

namespace {

using V = std::vector<std::vector<double>>;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kF = 1.1920928955078125e-07;  // float epsilon (2^-23)

// Element-wise reference from a scalar double function.
template <class Fn>
RefFn ew1(Fn fn) {
  return [fn](const V& in, V& out) {
    for (size_t t = 0; t < in[0].size(); ++t) out[0][t] = fn(in[0][t]);
  };
}
template <class Fn>
RefFn ew2(Fn fn) {
  return [fn](const V& in, V& out) {
    for (size_t t = 0; t < in[0].size(); ++t) out[0][t] = fn(in[0][t], in[1][t]);
  };
}
template <class Fn>
RefFn ew3(Fn fn) {
  return [fn](const V& in, V& out) {
    for (size_t t = 0; t < in[0].size(); ++t) out[0][t] = fn(in[0][t], in[1][t], in[2][t]);
  };
}

AtomSpec spec(Atom a, std::vector<Gen> gens, RefFn ref, Tol tol = {}) {
  AtomSpec s;
  s.atom = a;
  s.gens = std::move(gens);
  s.ref = std::move(ref);
  s.tol = {tol};
  return s;
}

double minRef(double a, double b) { return a < b ? a : b; }  // NaN in either → b (matches _mm_min_ps)
double maxRef(double a, double b) { return a > b ? a : b; }
double b01(bool b) { return b ? 1.0 : 0.0; }

}  // namespace

TEST_CASE("dsp atoms: kernel table is complete and consistent") {
  std::set<std::string> names;
  for (uint32_t i = 0; i < pq::dsp::kAtomCount; ++i) {
    const auto a = static_cast<Atom>(i);
    const auto& info = pq::dsp::kernelInfo(a);
    REQUIRE(info.name != nullptr);
    CHECK(names.insert(info.name).second);
    // The specialised inputs are a prefix, and every legal mask has a kernel for both lane counts.
    CHECK((info.splSpecMask & (info.splSpecMask + 1)) == 0);
    for (uint32_t lanes : {1u, 8u})
      for (uint32_t m = 0; m <= info.splSpecMask; ++m) CHECK(pq::dsp::findKernel(a, lanes, m | info.splRequired) != nullptr);
    CHECK(pq::dsp::findKernel(a, 4, info.splRequired) == nullptr);
    if (info.splRequired) CHECK(pq::dsp::findKernel(a, 8, 0) == nullptr);
  }
  CHECK(names.size() == pq::dsp::kAtomCount);
  CHECK(pq::dsp::findKernel(Atom::Add, 8, 4) == nullptr);  // bit for a nonexistent input
}

TEST_CASE("dsp atoms: arithmetic") {
  const Tol exact{0, 0};
  const Tol ulp{0, kF};
  checkAtom(spec(Atom::Add, {uniform(-4, 4), uniform(-4, 4)}, ew2([](double a, double b) { return a + b; }), ulp));
  checkAtom(spec(Atom::Sub, {uniform(-4, 4), uniform(-4, 4)}, ew2([](double a, double b) { return a - b; }), ulp));
  checkAtom(spec(Atom::Mul, {uniform(-4, 4), uniform(-4, 4)}, ew2([](double a, double b) { return a * b; }), ulp));
  checkAtom(spec(Atom::Div, {uniform(-4, 4), signedMagnitude(0.05f, 4)}, ew2([](double a, double b) { return a / b; }), ulp));
  checkAtom(spec(Atom::Min, {grid(-2, 0.5f, 9), grid(-2, 0.5f, 9)}, ew2(minRef), exact));
  checkAtom(spec(Atom::Max, {grid(-2, 0.5f, 9), grid(-2, 0.5f, 9)}, ew2(maxRef), exact));
  // mod: floored, result compared modulo |b| (a/b rounding can put a result at 0 instead of b).
  checkAtom(spec(Atom::Mod, {uniform(-20, 20), signedMagnitude(0.3f, 4)},
                 ew2([](double a, double b) { return a - b * std::floor(a / b); }), Tol{1e-5, 0, 0, 1}));
  checkAtom(spec(Atom::MulAdd, {uniform(-4, 4), uniform(-4, 4), uniform(-4, 4)},
                 ew3([](double a, double b, double c) { return a * b + c; }), Tol{1e-12, kF}));
  checkAtom(spec(Atom::Lerp, {uniform(-4, 4), uniform(-4, 4), uniform(-0.5f, 1.5f)},
                 ew3([](double a, double b, double t) { return a + t * (b - a); }), Tol{1e-6, kF}));
  checkAtom(spec(Atom::Clip, {uniform(-3, 3), uniform(-2, 0), uniform(0, 2)},
                 ew3([](double x, double lo, double hi) { return minRef(maxRef(x, lo), hi); }), exact));
  checkAtom(spec(Atom::Select, {boolRuns(0.3), uniform(-4, 4), uniform(-4, 4)},
                 ew3([](double c, double a, double b) { return c != 0 ? a : b; }), exact));
  checkAtom(spec(Atom::Neg, {uniform(-4, 4)}, ew1([](double x) { return -x; }), exact));
  checkAtom(spec(Atom::Abs, {uniform(-4, 4)}, ew1([](double x) { return std::abs(x); }), exact));
  checkAtom(spec(Atom::Sign, {grid(-2, 0.5f, 9)}, ew1([](double x) { return x > 0 ? 1.0 : x < 0 ? -1.0 : 0.0; }), exact));
  checkAtom(spec(Atom::Floor, {uniform(-10, 10)}, ew1([](double x) { return std::floor(x); }), exact));
  checkAtom(spec(Atom::Frac, {uniform(-10, 10)},
                 ew1([](double x) { return std::min(x - std::floor(x), 0.99999994039535522); }), exact));
  checkAtom(spec(Atom::Sqrt, {uniform(0, 100)}, ew1([](double x) { return std::sqrt(x); }), Tol{0, kF / 2}));
  checkAtom(spec(Atom::Const, {uniform(-4, 4)}, ew1([](double x) { return x; }), exact));
}

TEST_CASE("dsp atoms: transcendental, exact and fast") {
  // exact: within a few float ulps of the double result; fast: the bounds documented in pq/core/fast_math.h.
  const Tol ex{1e-30, 4 * kF};
  checkAtom(spec(Atom::Exp, {uniform(-20, 20)}, ew1([](double x) { return std::exp(x); }), ex));
  checkAtom(spec(Atom::ExpFast, {uniform(-20, 20)}, ew1([](double x) { return std::exp(x); }), Tol{0, 2.5e-6}));
  checkAtom(spec(Atom::Log, {uniform(1e-4f, 1e4f)}, ew1([](double x) { return std::log(x); }), Tol{1e-6, 2 * kF}));
  checkAtom(spec(Atom::LogFast, {uniform(1e-4f, 1e4f)}, ew1([](double x) { return std::log(x); }), Tol{2e-6, 2 * kF}));
  checkAtom(spec(Atom::Sin, {uniform(-100, 100)}, ew1([](double x) { return std::sin(x); }), Tol{2.5e-7, 0}));
  checkAtom(spec(Atom::SinFast, {uniform(-100, 100)}, ew1([](double x) { return std::sin(x); }), Tol{2e-7, 0}));
  checkAtom(spec(Atom::Tan, {uniform(-1.5f, 1.5f)}, ew1([](double x) { return std::tan(x); }), Tol{1e-30, 4 * kF}));
  checkAtom(spec(Atom::TanFast, {uniform(-1.5f, 1.5f)}, ew1([](double x) { return std::tan(x); }), Tol{1e-30, 4e-7}));
  checkAtom(spec(Atom::Tanh, {uniform(-10, 10)}, ew1([](double x) { return std::tanh(x); }), Tol{1e-30, 4 * kF}));
  checkAtom(spec(Atom::TanhFast, {uniform(-10, 10)}, ew1([](double x) { return std::tanh(x); }), Tol{4e-7, 0}));
  checkAtom(spec(Atom::Pow, {uniform(0.01f, 10), uniform(-4, 4)}, ew2([](double a, double b) { return std::pow(a, b); }),
                 Tol{1e-30, 4 * kF}));
  checkAtom(spec(Atom::PowFast, {uniform(0.01f, 10), uniform(-4, 4)},
                 ew2([](double a, double b) { return std::pow(a, b); }), Tol{1e-30, 5e-6}));
  // Units. Exact forms are evaluated in double inside the kernel, so they are correctly rounded floats.
  const auto c2h = [](double c, double ref) { return ref * std::exp2(c / 1200.0); };
  const auto h2c = [](double hz, double ref) { return 1200.0 * std::log2(hz / ref); };
  checkAtom(spec(Atom::CentsToHz, {uniform(-6000, 6000), uniform(20, 1000)}, ew2(c2h), Tol{0, kF / 2}));
  checkAtom(spec(Atom::CentsToHzFast, {uniform(-6000, 6000), uniform(20, 1000)}, ew2(c2h), Tol{0, 1e-6}));
  checkAtom(spec(Atom::HzToCents, {uniform(10, 20000), uniform(20, 1000)}, ew2(h2c), Tol{1e-9, kF / 2}));
  checkAtom(spec(Atom::HzToCentsFast, {uniform(10, 20000), uniform(20, 1000)}, ew2(h2c), Tol{3e-3, 0}));
  const auto d2g = [](double db) { return std::pow(10.0, db / 20.0); };
  const auto g2d = [](double g) { return 20.0 * std::log10(g); };
  checkAtom(spec(Atom::DbToGain, {uniform(-120, 24)}, ew1(d2g), Tol{0, kF / 2}));
  checkAtom(spec(Atom::DbToGainFast, {uniform(-120, 24)}, ew1(d2g), Tol{0, 2e-6}));
  checkAtom(spec(Atom::GainToDb, {uniform(1e-6f, 10)}, ew1(g2d), Tol{1e-9, kF / 2}));
  checkAtom(spec(Atom::GainToDbFast, {uniform(1e-6f, 10)}, ew1(g2d), Tol{4e-5, 0}));
  // sine(phase) = sin(2π·phase) for any real phase.
  checkAtom(spec(Atom::SinePhase, {uniform(-3, 3)}, ew1([](double p) { return std::sin(2 * std::numbers::pi * p); }),
                 Tol{4e-7, 0}));
}

TEST_CASE("dsp atoms: stateless logic") {
  const auto b2 = [](auto fn) { return ew2([fn](double a, double b) { return b01(fn(a != 0, b != 0)); }); };
  checkAtom(spec(Atom::And, {boolRuns(0.4), boolRuns(0.4)}, b2([](bool a, bool b) { return a && b; })));
  checkAtom(spec(Atom::Or, {boolRuns(0.4), boolRuns(0.4)}, b2([](bool a, bool b) { return a || b; })));
  checkAtom(spec(Atom::Xor, {boolRuns(0.4), boolRuns(0.4)}, b2([](bool a, bool b) { return a != b; })));
  checkAtom(spec(Atom::Nand, {boolRuns(0.4), boolRuns(0.4)}, b2([](bool a, bool b) { return !(a && b); })));
  checkAtom(spec(Atom::Nor, {boolRuns(0.4), boolRuns(0.4)}, b2([](bool a, bool b) { return !(a || b); })));
  checkAtom(spec(Atom::Not, {boolRuns(0.4)}, ew1([](double a) { return b01(a == 0); })));
  checkAtom(spec(Atom::GateToReal, {boolRuns(0.4)}, ew1([](double a) { return a; })));
  const auto g = grid(-1, 0.5f, 5);  // equal values are common
  checkAtom(spec(Atom::Gt, {g, g}, ew2([](double a, double b) { return b01(a > b); })));
  checkAtom(spec(Atom::Lt, {g, g}, ew2([](double a, double b) { return b01(a < b); })));
  checkAtom(spec(Atom::Ge, {g, g}, ew2([](double a, double b) { return b01(a >= b); })));
  checkAtom(spec(Atom::Le, {g, g}, ew2([](double a, double b) { return b01(a <= b); })));
  checkAtom(spec(Atom::Eq, {g, g}, ew2([](double a, double b) { return b01(a == b); })));
  checkAtom(spec(Atom::Ne, {g, g}, ew2([](double a, double b) { return b01(a != b); })));
  // NaN: ordered compares are false, ne is true.
  const auto withNaN = pickFrom({0.0f, 1.0f, std::numeric_limits<float>::quiet_NaN()});
  checkAtom(spec(Atom::Lt, {withNaN, withNaN}, ew2([](double a, double b) { return b01(a < b); })));
  checkAtom(spec(Atom::Ne, {withNaN, withNaN}, ew2([](double a, double b) { return b01(a != b); })));
}

TEST_CASE("dsp atoms: mux and demux") {
  // mux with 3 data inputs; index includes out-of-range and NaN values. All 16 Spl combinations are covered
  // because mux treats every input's rate at run time (KernelArgs::splMask).
  AtomSpec s = spec(Atom::Mux,
                    {pickFrom({-1.0f, 0.0f, 0.5f, 1.0f, 1.99f, 2.0f, 2.5f, 7.0f, std::numeric_limits<float>::quiet_NaN()}),
                     uniform(-1, 1), uniform(-1, 1), uniform(-1, 1)},
                    [](const V& in, V& out) {
                      for (size_t t = 0; t < in[0].size(); ++t) {
                        const double i = in[0][t];
                        const double k = std::isnan(i) ? 0.0 : std::clamp(std::floor(i), 0.0, 2.0);
                        out[0][t] = in[1 + static_cast<size_t>(k)][t];
                      }
                    });
  checkAtom(s);
  // Bool data through mux (masks pass through unchanged).
  AtomSpec sb = s;
  sb.gens = {grid(0, 1, 3), boolRuns(0.3), boolRuns(0.3), boolRuns(0.3)};
  sb.ref = [](const V& in, V& out) {
    for (size_t t = 0; t < in[0].size(); ++t) out[0][t] = in[1 + static_cast<size_t>(in[0][t])][t] != 0 ? 1.0 : 0.0;
  };
  {
    // Compare masks bit-exactly by checking the raw output against the selected input.
    const auto blocks = std::vector<uint32_t>{5, 8, 13, 0, 64, 10};
    std::mt19937 rng(3);
    std::vector<std::vector<float>> in(4);
    for (uint32_t i = 0; i < 4; ++i) in[i] = sb.gens[i](rng, 0, 100);
    sb.T = 100;
    const auto r = runKernel(sb, 1, 0, in, blocks, 1);
    for (uint32_t t = 0; t < 100; ++t)
      CHECK(std::bit_cast<uint32_t>(r.out[0][t]) == std::bit_cast<uint32_t>(in[1 + static_cast<size_t>(in[0][t])][t]));
  }

  AtomSpec d = spec(Atom::Demux, {pickFrom({-3.0f, 0.0f, 0.7f, 1.0f, 2.2f, 3.0f, 9.0f}), uniform(-1, 1)},
                    [](const V& in, V& out) {
                      for (size_t t = 0; t < in[0].size(); ++t) {
                        const double k = std::clamp(std::floor(in[0][t]), 0.0, 3.0);
                        for (size_t o = 0; o < 4; ++o) out[o][t] = (static_cast<double>(o) == k) ? in[1][t] : 0.0;
                      }
                    });
  d.numOut = 4;
  d.tol = {Tol{}, Tol{}, Tol{}, Tol{}};
  checkAtom(d);
}

TEST_CASE("dsp atoms: stateful logic") {
  // schmitt
  checkAtom(spec(Atom::Schmitt, {uniform(-1, 1), uniform(-0.6f, -0.1f), uniform(0.1f, 0.6f)}, [](const V& in, V& out) {
    bool q = false;
    for (size_t t = 0; t < in[0].size(); ++t) {
      if (in[0][t] > in[2][t]) q = true;
      else if (in[0][t] < in[1][t]) q = false;
      out[0][t] = b01(q);
    }
  }));
  // edges
  const auto edge = [](int kind) {
    return [kind](const V& in, V& out) {
      bool prev = false;
      for (size_t t = 0; t < in[0].size(); ++t) {
        const bool b = in[0][t] != 0;
        out[0][t] = b01(kind == 0 ? (b && !prev) : kind == 1 ? (!b && prev) : (b != prev));
        prev = b;
      }
    };
  };
  checkAtom(spec(Atom::Rise, {boolRuns(0.3)}, edge(0)));
  checkAtom(spec(Atom::Fall, {boolRuns(0.3)}, edge(1)));
  checkAtom(spec(Atom::Change, {boolRuns(0.3)}, edge(2)));
  // flip-flops
  checkAtom(spec(Atom::SrFlipFlop, {boolRuns(0.2), boolRuns(0.2)}, [](const V& in, V& out) {
    bool q = false;
    for (size_t t = 0; t < in[0].size(); ++t) {
      if (in[1][t] != 0) q = false;
      else if (in[0][t] != 0) q = true;
      out[0][t] = b01(q);
    }
  }));
  checkAtom(spec(Atom::DFlipFlop, {boolRuns(0.3), boolRuns(0.3)}, [](const V& in, V& out) {
    bool q = false, prev = false;
    for (size_t t = 0; t < in[0].size(); ++t) {
      const bool c = in[1][t] != 0;
      if (c && !prev) q = in[0][t] != 0;
      prev = c;
      out[0][t] = b01(q);
    }
  }));
  checkAtom(spec(Atom::TFlipFlop, {boolRuns(0.3), boolRuns(0.3)}, [](const V& in, V& out) {
    bool q = false, prev = false;
    for (size_t t = 0; t < in[0].size(); ++t) {
      const bool c = in[1][t] != 0;
      if (c && !prev && in[0][t] != 0) q = !q;
      prev = c;
      out[0][t] = b01(q);
    }
  }));
  // counter(up, down, reset, n)
  const auto countN = [](double x) -> int64_t { return (x >= 1 && x < 2147483648.0) ? std::llround(std::nearbyint(x)) : 0; };
  AtomSpec c = spec(Atom::Counter, {boolRuns(0.3), boolRuns(0.2), boolRuns(0.02), pickFrom({0.0f, 3.0f, 5.0f, 4.6f, -2.0f})},
                    [countN](const V& in, V& out) {
                      int64_t count = 0;
                      bool pu = false, pd = false;
                      for (size_t t = 0; t < in[0].size(); ++t) {
                        const bool u = in[0][t] != 0, d = in[1][t] != 0;
                        if (u && !pu) ++count;
                        if (d && !pd) --count;
                        pu = u;
                        pd = d;
                        const int64_t n = countN(in[3][t]);
                        if (n >= 1) {
                          if (count > n - 1) count = 0;
                          if (count < 0) count = n - 1;
                        }
                        if (in[2][t] != 0) count = 0;
                        out[0][t] = static_cast<double>(count);
                      }
                    });
  checkAtom(c);
  // clockdiv(clk, n)
  checkAtom(spec(Atom::ClockDiv, {boolRuns(0.3), pickFrom({1.0f, 2.0f, 3.0f, 0.0f, 4.4f})}, [countN](const V& in, V& out) {
    int64_t k = -1;
    bool prev = false;
    for (size_t t = 0; t < in[0].size(); ++t) {
      const bool c = in[0][t] != 0;
      int64_t n = countN(in[1][t]);
      if (n < 1) n = 1;
      if (c && !prev) k = (k + 1 >= n) ? 0 : k + 1;
      prev = c;
      out[0][t] = b01(c && k == 0);
    }
  }));
  // pulse(period)
  checkAtom(spec(Atom::Pulse, {pickFrom({1.0f, 2.0f, 5.0f, 7.4f, 13.0f})}, [countN](const V& in, V& out) {
    int64_t r = 0;
    for (size_t t = 0; t < in[0].size(); ++t) {
      const bool fire = r == 0;
      out[0][t] = b01(fire);
      if (fire) r = countN(in[0][t]) - 1;
      else if (r > 0) --r;
    }
  }));
  // pulse with an invalid period fires exactly once.
  checkAtom(spec(Atom::Pulse, {pickFrom({0.0f, -3.0f, std::numeric_limits<float>::quiet_NaN()})},
                 [](const V& in, V& out) {
                   for (size_t t = 0; t < in[0].size(); ++t) out[0][t] = b01(t == 0);
                 }));
  // timer(trig, duration)
  checkAtom(spec(Atom::Timer, {boolRuns(0.05), pickFrom({0.0f, 1.0f, 3.0f, 10.0f, 2.6f, -4.0f, 40.0f})},
                 [](const V& in, V& out) {
                   int64_t r = 0;
                   bool prev = false;
                   for (size_t t = 0; t < in[0].size(); ++t) {
                     const bool b = in[0][t] != 0;
                     if (b && !prev) r = static_cast<int64_t>(std::nearbyint(std::max(in[1][t], 0.0)));
                     prev = b;
                     out[0][t] = b01(r > 0);
                     if (r > 0) --r;
                   }
                 }));
}
