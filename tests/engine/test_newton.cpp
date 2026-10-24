// T1b.1-fix1: Newton's shared pieces (pq/engine/newton.h). Every tangent rule against a central difference of the
// dsp kernel itself (so a rule is checked against what the backends actually compute), the zero-derivative atoms,
// and the tangent plan's direction bits / AD-or-finite-differences choice.
#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <string>
#include <vector>

#include "interp_harness.h"
#include "pq/dsp/atoms.h"
#include "pq/engine/newton.h"

namespace {

using pq::dsp::Atom;
namespace newton = pq::engine::newton;

// The kernel's value at one frame, 1 lane (every input a 1-frame block).
float kernelValue(Atom a, const std::vector<float>& x, uint32_t numOut = 1, uint32_t out = 0) {
  const pq::dsp::KernelFn fn = pq::dsp::findKernel(a, 1, 0);
  REQUIRE(fn != nullptr);
  std::vector<const float*> in;
  for (const float& v : x) in.push_back(&v);
  std::vector<float> y(numOut, 0.0f);
  std::vector<float*> outs;
  for (float& v : y) outs.push_back(&v);
  pq::dsp::KernelArgs args;
  args.in = in.data();
  args.out = outs.data();
  args.frames = 1;
  args.numIn = static_cast<uint16_t>(x.size());
  args.numOut = static_cast<uint16_t>(numOut);
  fn(args);
  return y[out];
}

struct Case {
  Atom atom;
  std::vector<float> x;
  uint32_t numOut = 1;
};

}  // namespace

TEST_CASE("newton: every tangent rule matches a central difference of its dsp kernel") {
  const float t = std::bit_cast<float>(0xFFFFFFFFu);  // true mask
  const std::vector<Case> cases = {
      {Atom::Add, {0.3f, 0.7f}}, {Atom::Sub, {0.3f, 0.7f}}, {Atom::Mul, {0.3f, -1.7f}}, {Atom::Div, {0.8f, 1.3f}},
      {Atom::Min, {0.3f, 0.7f}}, {Atom::Min, {0.9f, 0.2f}}, {Atom::Max, {0.3f, 0.7f}}, {Atom::Max, {0.9f, 0.2f}},
      {Atom::Mod, {2.3f, 0.7f}}, {Atom::Pow, {1.3f, 1.7f}}, {Atom::PowFast, {1.3f, 1.7f}},
      {Atom::CentsToHz, {350.0f, 220.0f}}, {Atom::CentsToHzFast, {350.0f, 220.0f}},
      {Atom::HzToCents, {330.0f, 220.0f}}, {Atom::HzToCentsFast, {330.0f, 220.0f}}, {Atom::Neg, {0.4f}},
      {Atom::Abs, {-0.6f}}, {Atom::Abs, {0.6f}}, {Atom::Frac, {2.3f}}, {Atom::Sqrt, {1.7f}}, {Atom::SqrtFast, {1.7f}},
      {Atom::Exp, {0.7f}}, {Atom::ExpFast, {0.7f}}, {Atom::Log, {1.9f}}, {Atom::LogFast, {1.9f}}, {Atom::Sin, {0.8f}},
      {Atom::SinFast, {0.8f}}, {Atom::Tan, {0.6f}}, {Atom::TanFast, {0.6f}}, {Atom::Tanh, {0.4f}},
      {Atom::TanhFast, {0.4f}}, {Atom::DbToGain, {-6.0f}}, {Atom::DbToGainFast, {-6.0f}}, {Atom::GainToDb, {0.6f}},
      {Atom::GainToDbFast, {0.6f}}, {Atom::Const, {0.4f}}, {Atom::SinePhase, {0.13f}}, {Atom::SinePhase, {-2.6f}},
      {Atom::Clip, {0.3f, -0.5f, 0.5f}}, {Atom::Clip, {0.9f, -0.5f, 0.5f}}, {Atom::Clip, {-0.9f, -0.5f, 0.5f}},
      {Atom::Lerp, {0.2f, 0.9f, 0.3f}}, {Atom::Select, {t, 0.3f, 0.7f}}, {Atom::Select, {0.0f, 0.3f, 0.7f}},
      {Atom::MulAdd, {0.3f, 1.2f, -0.4f}}, {Atom::Mux, {1.4f, 0.1f, 0.2f, 0.3f}}, {Atom::Mux, {7.0f, 0.1f, 0.2f, 0.3f}},
      {Atom::Demux, {1.0f, 0.6f}, 3}, {Atom::Demux, {-4.0f, 0.6f}, 3}};
  newton::ScalarOps ops;
  uint32_t checked = 0;
  for (const Case& c : cases) {
    const auto numIn = static_cast<uint32_t>(c.x.size());
    REQUIRE(newton::hasTangent(c.atom));
    for (uint32_t k = 0; k < numIn; ++k) {
      if (!newton::differentiableInput(c.atom, k)) continue;
      std::vector<newton::Tan<float>> d(numIn), dy(c.numOut);
      d[k] = {1.0f, true};
      float a[3] = {};
      for (uint32_t i = 0; i < std::min<uint32_t>(numIn, 3); ++i) a[i] = c.x[i];
      const float y = kernelValue(c.atom, c.x, c.numOut, 0);
      newton::tangentOp(ops, c.atom, numIn, c.numOut, a, &y, d.data(), dy.data());
      for (uint32_t o = 0; o < c.numOut; ++o) {
        INFO(pq::dsp::kernelInfo(c.atom).name << " input " << k << " output " << o << " at x[0] = " << c.x[0]);
        REQUIRE(dy[o].on);
        const double h = 1e-3 * std::max(1.0, std::fabs(static_cast<double>(c.x[k])));
        std::vector<float> xp = c.x, xm = c.x;
        xp[k] = static_cast<float>(c.x[k] + h);
        xm[k] = static_cast<float>(c.x[k] - h);
        const double fd = (static_cast<double>(kernelValue(c.atom, xp, c.numOut, o)) - kernelValue(c.atom, xm, c.numOut, o)) /
                          (static_cast<double>(xp[k]) - xm[k]);
        CHECK(std::fabs(dy[o].v - fd) <= 2e-3 * std::max(1.0, std::fabs(fd)));
        ++checked;
      }
    }
  }
  MESSAGE(checked << " (atom, input, output) derivatives checked");
  CHECK(checked >= 60);
  // Atoms whose derivative is zero (and inputs that carry none) produce no tangent.
  for (Atom a : {Atom::Sign, Atom::Floor, Atom::Gt, Atom::Lt, Atom::Ge, Atom::Le, Atom::Eq, Atom::Ne, Atom::And,
                 Atom::Or, Atom::Xor, Atom::Nand, Atom::Nor, Atom::Not, Atom::GateToReal}) {
    CHECK(newton::hasTangent(a));
    CHECK_FALSE(newton::differentiableInput(a, 0));
    std::vector<newton::Tan<float>> d(2, {1.0f, true}), dy(1);
    const float x[3] = {0.4f, 0.2f, 0.7f};
    const float y = 0.5f;  // non-zero: the inlined rules divide by y (sqrt)
    newton::tangentOp(ops, a, 2, 1, x, &y, d.data(), dy.data());
    CHECK_FALSE(dy[0].on);
  }
  CHECK_FALSE(newton::differentiableInput(Atom::Select, 0));
  CHECK_FALSE(newton::differentiableInput(Atom::Mux, 0));
  CHECK_FALSE(newton::differentiableInput(Atom::Demux, 0));
  for (Atom a : {Atom::Shaper, Atom::Wavetable, Atom::Z1, Atom::Phasor, Atom::DelayLinear}) CHECK_FALSE(newton::hasTangent(a));
}

TEST_CASE("newton: the tangent plan follows each direction through the body; table reads mean finite differences") {
  using namespace interp_test;
  const Compiled zdf = compileText(R"(patch Z {
  in x : audio
  out y : audio
  out o : audio
  s = z1(s2)
  solve(iterations: 4) {
    unknown v = s
    unknown w = 0
    v == s + 0.2 * (tanh(x) - tanh(v))
    w * 2 == x
  }
  s2 = 2 * v - s
  y = v
  o = w
})");
  REQUIRE(zdf.sched.solves.size() == 1);
  const newton::TangentPlan p = newton::planTangents(zdf.sched, zdf.sched.solves[0]);
  CHECK(p.ad);
  CHECK(p.n == 2);
  REQUIRE(p.lhsNz.size() == 2);
  // Equation 0 depends on v only (direction 0), equation 1 on w only (direction 1); the order of equations and
  // unknowns is the schedule's, so compare as a set.
  const uint32_t e0 = p.lhsNz[0] | p.rhsNz[0], e1 = p.lhsNz[1] | p.rhsNz[1];
  CHECK(((e0 == 1u && e1 == 2u) || (e0 == 2u && e1 == 1u)));
  // The body holds tanh(v) (tanh(x) does not depend on an unknown, so it is outside): its tangent has exactly v's
  // direction bit.
  uint32_t tanhOps = 0;
  for (size_t k = 0; k < p.firstIn.size(); ++k) {
    const pq::graph::Op& op = zdf.sched.ops[zdf.sched.solves[0].firstOp + k];
    if (static_cast<Atom>(op.kernel) != Atom::Tanh) continue;
    ++tanhOps;
    const uint32_t bits = p.outNz[p.firstOut[k]];
    CHECK(std::popcount(bits) == 1);
    CHECK((bits == e0 || bits == e1));
  }
  CHECK(tanhOps == 1);
  const Compiled fd = compileText(R"(patch F {
  in x : audio
  out y : audio
  solve(iterations: 4) {
    unknown v = x
    v + shaper(v) * 0.1 == x
  }
  y = v
})");
  REQUIRE(fd.sched.solves.size() == 1);
  CHECK_FALSE(newton::planTangents(fd.sched, fd.sched.solves[0]).ad);
}
