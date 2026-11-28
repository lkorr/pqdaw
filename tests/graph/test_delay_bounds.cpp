// FD.1: value ranges (pq/graph/value_range.h) and what the scheduler does with them — delay bounds by interval
// analysis, the choice of cuts, loop sub-blocks (schedule.h "Loop").
//   - the rules: exact single values, each operation, per channel through pack / channel, what stays unknown;
//   - soundness by execution: random expressions over ranged params, run with the real kernels at random and extreme
//     param values, never leave their range; expressions of single values ARE their range, bit for bit;
//   - the bounded sources stay inside the range the analysis gives them;
//   - schedules: a tap behind lerp / min / max, a modulated tap, per-lane times, an unknown time, bounds exactly at a
//     sub-block edge (both sides, and a sub-block one frame too long is caught by the oracle), the cut choice (long
//     main taps cut, short stage delays fused), an 8-lane FDN; each executed against the per-sample oracle
//     (sched_exec.h Naive: every delay split, one sample at a time).

#include <doctest/doctest.h>

#include "doctest_string_view.h"

#include <cmath>
#include <random>
#include <string>
#include <vector>

#include "pq/core/fast_math.h"
#include "pq/graph/infer.h"
#include "pq/graph/optimise.h"
#include "pq/graph/parse.h"
#include "pq/graph/schedule.h"
#include "pq/graph/value_range.h"
#include "sched_exec.h"

using namespace pq::graph;
namespace dsp = pq::dsp;

namespace {

constexpr float kInf = std::numeric_limits<float>::infinity();

struct Built {
  std::string text;
  ParseResult parsed;
  PatchLibrary lib;
  TypedPatch typed;
  Adjacency adj;
  ScheduleResult sched;
};

std::string diags(const std::vector<Diagnostic>& ds, std::string_view src) {
  std::string s;
  for (const Diagnostic& d : ds) s += formatDiagnostic(src, d) + "\n";
  return s;
}

// Parse → infer (the LAST patch of the text; earlier ones are its subcircuits) → optimise (48 kHz) → schedule.
void build(Built& b, std::string_view src, ScheduleOptions so = {}) {
  b.text = std::string(src);
  b.parsed = parseDetailed(b.text, "test.pqp");
  REQUIRE_MESSAGE(b.parsed.ok(), diags(b.parsed.diagnostics, b.text));
  b.lib.add(b.parsed.file);
  InferOptions io;
  io.library = &b.lib;
  InferResult inf = inferDetailed(b.parsed.file.patches.back(), io);
  REQUIRE_MESSAGE(inf.ok(), diags(inf.diagnostics, b.text));
  OptimiseOptions oo;
  oo.sampleRate = 48000.0;
  auto opt = optimise(std::move(inf.typed), oo);
  REQUIRE_MESSAGE(opt.ok(), (opt.ok() ? std::string() : opt.error().message));
  b.typed = std::move(opt.value());
  b.adj = b.typed.patch.adjacency();
  so.paramRamps = false;  // the T1.5 schedule: sched_exec runs the ramp-free program
  b.sched = scheduleDetailed(b.typed, so);
  REQUIRE_MESSAGE(b.sched.ok(), diags(b.sched.diagnostics, b.text));
}

NodeIndex outNode(const Built& b, std::string_view name) {
  const Patch& p = b.typed.patch;
  for (NodeIndex n = 0; n < p.nodeCount(); ++n)
    if (p.node(n).kind == NodeKind::Output && p.nodeName(n) == name) return n;
  FAIL("no output " << name);
  return kNone;
}

// The range of output `name`, channel ch.
ValueRange rangeOf(const Built& b, std::string_view name, uint32_t ch = 0, bool trust = true) {
  ValueRangeOptions o;
  o.trustParamRanges = trust;
  ValueRanges r(b.typed, b.adj, o);
  return r.input(outNode(b, name), 0, ch);
}

std::vector<uint32_t> loopSubs(const Schedule& s) {
  std::vector<uint32_t> v;
  for (const Region& r : s.regions)
    if (r.kind == RegionKind::Loop) v.push_back(r.subBlock);
  return v;
}
uint32_t kernels(const Schedule& s, dsp::Atom k) {
  uint32_t n = 0;
  for (const Op& op : s.ops) n += op.kind == OpKind::Kernel && op.kernel == k;
  return n;
}

// The schedule against the per-sample oracle over several block sizes (the largest relative difference).
double worstVsOracle(const Built& b, const Schedule& s, std::initializer_list<uint32_t> frames = {64u, 37u, 1u, 4096u}) {
  REQUIRE(sched_exec::supported(b.typed));
  double worst = 0.0;
  for (uint32_t f : frames) worst = std::max(worst, sched_exec::compare(b.typed, s, f, f == 1 ? 300 : f >= 1024 ? 3 : 40, 48000.0));
  return worst;
}

const char* const kParams = R"(  param p = 2.5 [2..3]
  param q = 1 [-1..4]
  param r = 0.5 [0..1]
  param n = -2.5 [-3..-2]
  param u = 7
  param b : bool = false
)";

ValueRange exprRange(const std::string& expr, bool trust = true) {
  Built b;
  build(b, "patch R {\n  in x : audio\n" + std::string(kParams) + "  out y : audio\n  y = " + expr + "\n}\n");
  return rangeOf(b, "y", 0, trust);
}

}  // namespace

TEST_CASE("value range: a single value is the runtime's float32 value, bit for bit (M10)") {
  // float(0.02075) · 48000 = 995.99994 at runtime, 996 in double.
  Built b;
  build(b, R"(patch R {
  param t = 20.75ms [20.75ms..20.75ms]
  out y : audio
  y = t * sr
}
)");
  const ValueRange r = rangeOf(b, "y");
  CHECK(r.lo == static_cast<float>(0.02075) * 48000.0f);
  CHECK(r.hi == r.lo);
  CHECK(r.lo < 996.0f);
}

TEST_CASE("value range: the rules — arithmetic, min / max / clip, muladd, lerp, select, the bounded sources") {
  auto is = [](const std::string& expr, float lo, float hi) {
    INFO(expr);
    const ValueRange r = exprRange(expr);
    CHECK(r.lo == lo);
    CHECK(r.hi == hi);
  };
  is("p + q", 1.0f, 7.0f);
  is("p - q", -2.0f, 4.0f);
  is("p * q", -3.0f, 12.0f);
  is("n * q", -12.0f, 3.0f);
  is("p / n", -1.5f, 2.0f / -3.0f);
  is("-p", -3.0f, -2.0f);
  is("abs(q)", 0.0f, 4.0f);
  is("abs(n)", 2.0f, 3.0f);
  is("min(p, q)", -1.0f, 3.0f);
  is("max(p, q)", 2.0f, 4.0f);
  is("max(x, 3)", 3.0f, kInf);  // a floor under an unknown signal
  is("min(x, 3)", -kInf, 3.0f);
  is("clip(q, 0, 2)", 0.0f, 2.0f);
  is("clip(x, 0.5, 2)", 0.5f, 2.0f);
  is("clip(q, p, 10)", 2.0f, 4.0f);
  is("q * p + n", -6.0f, 10.0f);  // fused: muladd
  is("b ? p : q", -1.0f, 4.0f);
  is("lerp(p, q, 2)", -6.0f, 7.0f);  // t outside [0, 1]: by intervals, 2 · (q − p) + p
  is("phasor(100)", 0.0f, 1.0f);
  is("frac(x)", 0.0f, 1.0f);
  is("p * 0", 0.0f, 0.0f);
  // lerp with t in [0, 1]: the hull of a and b, widened by 2^-20 of the largest magnitude (not exactly the hull: the
  // kernel's fma(t, b − a, a) can pass an end by a rounding).
  {
    const ValueRange r = exprRange("lerp(p, q, r)");
    CHECK(r.lo < -1.0f);
    CHECK(r.lo > -1.00001f);
    CHECK(r.hi > 4.0f);
    CHECK(r.hi < 4.00001f);
  }
  // The sources of nominal range [−1, 1] are widened by 1e-6 (fast_sin's error, contraction).
  for (const char* e : {"sine(phasor(100))", "sine_hz(100)", "tanh(x)", "fast_tanh(x)", "sin(x)", "fast_sin(x)", "noise()",
                        "sine(x)"}) {
    INFO(e);
    const ValueRange r = exprRange(e);
    CHECK(r.lo == -1.000001f);
    CHECK(r.hi == 1.000001f);
  }
  // A modulated time factor: 1 + 0.01 · sine.
  {
    const ValueRange r = exprRange("1 + 0.01 * sine_hz(3)");
    CHECK(r.lo == std::fma(0.01f, -1.000001f, 1.0f));
    CHECK(r.hi == std::fma(0.01f, 1.000001f, 1.0f));
  }
}

TEST_CASE("value range: unknown stays unknown") {
  for (const char* e : {"x", "x * 2", "u", "u + p", "p / q", "p / x", "exp(p)", "x - x", "z1(p)", "transport.bpm",
                        "60 / transport.bpm", "gate_to_real(b) * x"}) {
    INFO(e);
    CHECK(exprRange(e).unknown());
  }
  // Half-known: a sum with an unknown has no bound on either side; |x| is at least 0 and a product keeps the sign.
  CHECK(exprRange("x + p").unknown());
  CHECK(exprRange("abs(x) + p") == ValueRange{2.0f, kInf});
  CHECK(exprRange("max(x, 0) * p") == ValueRange{0.0f, kInf});
  // Without trusting param ranges, params are unknown.
  CHECK(exprRange("p + 1", false).unknown());
  CHECK(exprRange("p + 1") == ValueRange{3.0f, 4.0f});
  // A value that depends on itself.
  Built b;
  build(b, R"(patch F {
  in x : audio
  out y : audio
  acc = z1(acc) * 0.5 + x
  y = max(acc, 1)
}
)");
  CHECK(rangeOf(b, "y") == ValueRange{1.0f, kInf});
  Built c;
  build(c, R"(patch F {
  in x : audio
  out y : audio
  acc = z1(acc) * 0.5 + 1
  y = acc
}
)");
  CHECK(rangeOf(c, "y").unknown());
}

TEST_CASE("value range: per channel through pack and channel; a subcircuit's input is its argument") {
  Built b;
  build(b, R"(patch H {
  in a : real
  out o : real
  o = a * 2
}
patch R {
  param p = 2.5 [2..3]
  param q = 1 [-1..4]
  param n = -2.5 [-3..-2]
  out y : audio ch(3)
  out one : audio
  out wide : audio ch(3)
  out sub : audio
  v = (p, q, n)
  y = v
  one = v[2]
  wide = v * (1, 2, p) + p
  sub = H(q)
}
)");
  CHECK(rangeOf(b, "y", 0) == ValueRange{2.0f, 3.0f});
  CHECK(rangeOf(b, "y", 1) == ValueRange{-1.0f, 4.0f});
  CHECK(rangeOf(b, "y", 2) == ValueRange{-3.0f, -2.0f});
  CHECK(rangeOf(b, "one") == ValueRange{-3.0f, -2.0f});
  // Each lane its own arithmetic; the mono p broadcasts.
  CHECK(rangeOf(b, "wide", 0) == ValueRange{4.0f, 6.0f});
  CHECK(rangeOf(b, "wide", 1) == ValueRange{0.0f, 11.0f});
  CHECK(rangeOf(b, "wide", 2) == ValueRange{-7.0f, -1.0f});
  CHECK(rangeOf(b, "sub") == ValueRange{-2.0f, 8.0f});
}

TEST_CASE("value range: the bounded sources stay inside the range the analysis gives them") {
  // sine(phase) = fast_sin(2π · (phase − round(phase))): every float within 1e-3 of the peaks, a grid elsewhere.
  float worst = 0.0f;
  const float halfPi = 1.57079637f;
  for (float c : {halfPi, -halfPi})
    for (float x = c - 1e-3f; x <= c + 1e-3f; x = std::nextafter(x, kInf)) worst = std::max(worst, std::fabs(pq::fastSin(x)));
  for (int i = -1000000; i <= 1000000; ++i) worst = std::max(worst, std::fabs(pq::fastSin(static_cast<float>(i) * 3.14159274e-6f)));
  CHECK(worst <= 1.000001f);
  for (float x : {-1e30f, -40.0f, -9.0f, -1.0f, 0.0f, 1.0f, 9.0f, 40.0f, 1e30f}) {
    CHECK(std::fabs(pq::fastTanh(x)) <= 1.0f);
    CHECK(std::fabs(std::tanh(x)) <= 1.0f);
  }
}

// Random expressions: `leaves` are the atoms of the grammar; every binary / ternary form the analysis has a rule for.
// `pure`: no product ever feeds a sum (sums and lerp ends are leaves), so no fma contraction is possible and every
// operation has exactly one rounding.
namespace {
std::string randomExpr(std::mt19937& rng, const std::vector<std::string>& leaves, int depth, bool pure = false) {
  auto pick = [&](uint32_t n) { return std::uniform_int_distribution<uint32_t>(0, n - 1)(rng); };
  auto leaf = [&] { return leaves[pick(static_cast<uint32_t>(leaves.size()))]; };
  if (depth == 0 || pick(6) == 0) return leaf();
  auto e = [&] { return randomExpr(rng, leaves, depth - 1, pure); };
  auto end = [&] { return pure ? leaf() : e(); };
  switch (pick(14)) {
    case 0: return "(" + end() + " + " + end() + ")";
    case 1: return "(" + end() + " - " + end() + ")";
    case 2: return "(" + e() + " * " + e() + ")";
    case 3: return pure ? "(" + e() + " * " + e() + ")" : "(" + e() + " * " + e() + " + " + e() + ")";  // muladd
    case 4: return pure ? "(" + e() + " / p)" : "(" + e() + " - " + e() + " * " + e() + ")";            // a − x·y
    case 5: return "min(" + e() + ", " + e() + ")";
    case 6: return "max(" + e() + ", " + e() + ")";
    case 7: return "clip(" + e() + ", " + e() + ", " + e() + ")";
    case 8: return "lerp(" + end() + ", " + end() + ", r)";
    case 9: return "lerp(" + end() + ", " + end() + ", " + e() + ")";
    case 10: return "abs(" + e() + ")";
    case 11: return "(-(" + e() + "))";
    case 12: return "(" + e() + " / " + (pick(2) ? "p" : "n") + ")";
    default: return "(b ? " + e() + " : " + e() + ")";
  }
}
}  // namespace

TEST_CASE("value range: random expressions never leave their range at random and extreme param values") {
  std::mt19937 rng(0xFD1u);
  const std::vector<std::string> leaves = {"p", "q", "r", "n", "0.3", "2", "(-1.5)", "100", "ph", "s", "1e-3"};
  const float lo[4] = {2.0f, -1.0f, 0.0f, -3.0f}, hi[4] = {3.0f, 4.0f, 1.0f, -2.0f};
  uint32_t bounded = 0, total = 0;
  for (int patch = 0; patch < 60; ++patch) {
    constexpr int kOuts = 6;
    std::string text = "patch R {\n" + std::string(kParams) + "  ph = phasor(997)\n  s = sine_hz(1234)\n";
    for (int o = 0; o < kOuts; ++o) text += "  out y" + std::to_string(o) + " : audio\n";
    for (int o = 0; o < kOuts; ++o) text += "  y" + std::to_string(o) + " = " + randomExpr(rng, leaves, 4) + "\n";
    text += "}\n";
    Built b;
    build(b, text);
    INFO(text);
    const Schedule& s = b.sched.schedule;
    std::vector<ValueRange> ranges;
    for (const OutputRef& o : s.outputs) {
      ValueRanges vr(b.typed, b.adj);
      ranges.push_back(vr.input(o.node, 0, 0));
      ++total;
      bounded += !ranges.back().unknown();
    }
    sched_exec::Executor ex(s, 64);
    for (int set = 0; set < 24; ++set) {
      for (uint32_t i = 0; i < s.params.size(); ++i) {
        const ParamSlot& ps = s.params[i];
        const std::string& name = b.typed.patch.nodeName(ps.node);
        if (ps.kind == Kind::Bool) {
          ex.setParam(i, dsp::boolMask((set & 1) != 0));
          continue;
        }
        const int k = name == "p" ? 0 : name == "q" ? 1 : name == "r" ? 2 : name == "n" ? 3 : -1;
        if (k < 0) continue;
        // The first 16 sets: every corner of the box; then random points.
        const float v = set < 16 ? ((set >> k) & 1 ? hi[k] : lo[k])
                                 : std::uniform_real_distribution<float>(lo[k], hi[k])(rng);
        ex.setParam(i, v);
      }
      const auto out = ex.block(static_cast<uint64_t>(set) * 64);
      for (size_t o = 0; o < out.size(); ++o)
        for (float v : out[o]) {
          if (std::isnan(v) || !(v >= ranges[o].lo && v <= ranges[o].hi)) {
            FAIL_CHECK("output " << o << " = " << v << " outside [" << ranges[o].lo << ", " << ranges[o].hi << "]");
            break;
          }
        }
    }
  }
  CHECK(bounded * 10 >= total * 9);  // the grammar has a rule for everything; only a few divide by a range with 0 … none do
}

TEST_CASE("value range: an expression of single values is its range, bit for bit") {
  // Every param is one value, so every range must collapse to the float the program computes (the rounding of each
  // operation as the kernels do it: mul, add, sub, div, lerp's fma(t, b − a, a)). Where a product feeds a sum the
  // range holds both roundings (the JIT may contract them into an fma), so the grammar here keeps products out of
  // sums; the value is inside its range either way.
  std::mt19937 rng(0x51D1u);
  const std::vector<std::string> leaves = {"p", "q", "r", "n", "0.3", "2", "(-1.5)", "100", "0.02075", "48000", "1e-3"};
  uint32_t exact = 0;
  for (int patch = 0; patch < 40; ++patch) {
    constexpr int kOuts = 6;
    std::string text = "patch R {\n  param p = 2.7 [2.7..2.7]\n  param q = -0.31 [-0.31..-0.31]\n  param r = 0.37 [0.37..0.37]\n"
                       "  param n = -2.2 [-2.2..-2.2]\n  param b : bool = true\n";
    for (int o = 0; o < kOuts; ++o) text += "  out y" + std::to_string(o) + " : audio\n";
    for (int o = 0; o < kOuts; ++o) text += "  y" + std::to_string(o) + " = " + randomExpr(rng, leaves, 4, true) + "\n";
    text += "}\n";
    Built b;
    build(b, text);
    INFO(text);
    const Schedule& s = b.sched.schedule;
    sched_exec::Executor ex(s, 8);
    const auto out = ex.block(0);
    for (size_t o = 0; o < out.size(); ++o) {
      ValueRanges vr(b.typed, b.adj);
      const ValueRange r = vr.input(s.outputs[o].node, 0, 0);
      INFO("output " << o);
      CHECK(out[o][0] >= r.lo);
      CHECK(out[o][0] <= r.hi);
      // A select keeps both sides (the hull); everything else is one value.
      if (text.find("? ") == std::string::npos) {
        CHECK(r.lo == r.hi);
        CHECK(r.lo == out[o][0]);
        ++exact;
      }
    }
  }
  CHECK(exact >= 60);
  // The fused product: one rounding.
  Built m;
  build(m, "patch R {\n  param p = 2.7 [2.7..2.7]\n  param q = -0.31 [-0.31..-0.31]\n  param n = -2.2 [-2.2..-2.2]\n"
           "  out y : audio\n  y = p * q + n\n}\n");
  CHECK(rangeOf(m, "y") == ValueRange{std::fma(2.7f, -0.31f, -2.2f), std::fma(2.7f, -0.31f, -2.2f)});
}

// ---- schedules ---------------------------------------------------------------------------------------------------

TEST_CASE("delay bounds: a tap whose time is lerp(time, min(sync, 5), mode) sub-blocks by its smallest time") {
  // The Delay Tap's time expression with a floor: ms / sync through lerp (Mode 0 … 1), the sync time clamped.
  Built b;
  build(b, R"(patch T {
  in x : audio
  param time = 250ms [10ms..5s]
  param sync = 0.5 [0..1]
  param mode = 0 [0..1]
  param fb = 0.5 [0..0.95]
  out y : audio
  synced = min(max(sync * 2, 20ms), 5)
  d = delay_cubic(x + fb * d, lerp(time, synced, mode), 5s)
  y = d
}
)");
  const Schedule& s = b.sched.schedule;
  // The hull's low end is 10 ms less lerp's rounding allowance (5 · 2^-20 s = 0.23 samples): 479.77 samples, a cubic
  // read's newest tap ⌊d⌋ − 1.
  CHECK(loopSubs(s) == std::vector<uint32_t>{478});
  CHECK(worstVsOracle(b, s) == 0.0);
  // The per-sample schedule of the same patch (the reference): the same samples.
  ScheduleOptions per;
  per.maxLoopSubBlock = 1;
  Built p;
  build(p, b.text, per);
  CHECK(loopSubs(p.sched.schedule) == std::vector<uint32_t>{1});
  CHECK(worstVsOracle(p, p.sched.schedule) == 0.0);
  // Without the floor (the Delay Tap as it is: Time 0 … 5 s, a sync time that depends on the tempo) there is no bound.
  Built tap;
  build(tap, R"(patch T {
  in x : audio
  param time = 250ms [0s..5s]
  param beats = 0.5 [0.0104..6]
  param mode = 0 [0..1]
  out y : audio
  synced = min(beats * 60 / transport.bpm, 5)
  d = delay_fade(x + 0.5 * d, lerp(time, synced, mode), 5s)
  y = d
}
)");
  CHECK(loopSubs(tap.sched.schedule) == std::vector<uint32_t>{1});
}

TEST_CASE("delay bounds: a modulated tap (time · (1 + 0.01 · sine)) keeps a sub-block") {
  Built b;
  build(b, R"(patch M {
  in x : audio
  param time = 10ms [10ms..1s]
  out y : audio
  d = delay_cubic(x + 0.6 * d, time * (1 + 0.01 * sine_hz(3)), 2s)
  y = d
}
)");
  const Schedule& s = b.sched.schedule;
  // 480.00003 · (1 − 0.01 · 1.000001) = 475.2: cubic 475 − 1.
  CHECK(loopSubs(s) == std::vector<uint32_t>{474});
  CHECK(kernels(s, dsp::Atom::DelayReadCubic) == 1);
  CHECK(worstVsOracle(b, s) == 0.0);
}

TEST_CASE("delay bounds: per-lane times through pack — the loop takes the smallest lane's bound") {
  Built b;
  build(b, R"(patch P {
  in x : audio
  param size = 1 [1..4]
  out y : audio ch(4)
  d = delay(x + 0.5 * d, (30ms, 12ms, 15ms, 21ms) * size, 1s)
  y = d
}
)");
  const Schedule& s = b.sched.schedule;
  CHECK(loopSubs(s) == std::vector<uint32_t>{static_cast<uint32_t>(std::floor(static_cast<float>(0.012) * 48000.0f))});
  CHECK(loopSubs(s).front() == 576);
  CHECK(kernels(s, dsp::Atom::DelayReadLinear) == 4);
  // One lane's time unknown: that lane is per sample, so the loop is.
  Built u;
  build(u, R"(patch P {
  in x : audio
  in t : audio
  out y : audio ch(3)
  d = delay(x + 0.5 * d, (30ms, t, 15ms), 1s)
  y = d
}
)");
  CHECK(loopSubs(u.sched.schedule) == std::vector<uint32_t>{1});
}

TEST_CASE("delay bounds: an unknown time stays per sample") {
  for (const char* time : {"t", "abs(t)", "p", "10ms * t", "max(t, 0)", "10ms + t"}) {
    INFO(time);
    Built b;
    build(b, std::string("patch U {\n  in x : audio\n  in t : audio\n  param p = 0.1\n  out y : audio\n  d = delay(x + 0.5 * d, ") +
                 time + ", 1s)\n  y = d\n}\n");
    CHECK(loopSubs(b.sched.schedule) == std::vector<uint32_t>{1});
    CHECK(worstVsOracle(b, b.sched.schedule, {64u, 1u}) == 0.0);
  }
  // A floor under the unknown is a bound.
  Built f;
  build(f, "patch U {\n  in x : audio\n  in t : audio\n  out y : audio\n  d = delay(x + 0.5 * d, max(t * 0.01, 2ms), 1s)\n  y = d\n}\n");
  CHECK(loopSubs(f.sched.schedule) == std::vector<uint32_t>{96});
  CHECK(worstVsOracle(f, f.sched.schedule) == 0.0);
}

TEST_CASE("delay bounds: a bound exactly at the sub-block edge — one frame more reads an unwritten frame") {
  // Each param sits at its range minimum, so the newest tap is exactly as old as the bound says. The sub-block must
  // be that age (not one less: no frame given away) and a schedule with one frame more must differ from the oracle
  // (the test would catch an optimistic bound; the difference is the unwritten tap's weight, 6e-5 of the signal at
  // 995.99994 samples, so any difference counts: the right schedule's is exactly 0).
  struct Case {
    std::string atom, ms;
    uint32_t sub;
  };
  const Case cases[] = {
      {"delay", "1", 48},           // float(0.001) · 48000 = 48.000002: linear ⌊d⌋
      {"delay", "0.9999", 47},      // 47.9952: one side
      {"delay", "20.75", 995},      // 995.99994 (996 in double): the other side
      {"delay", "0.5", 24},
      {"delay_cubic", "1", 47},     // cubic ⌊d⌋ − 1
      {"delay_cubic", "0.9999", 46},
      {"delay_allpass", "1", 47},   // fraction below 0.5: ⌊d⌋ − 1
      {"delay_allpass", "1.0105", 48},  // 48.504: fraction ≥ 0.5: ⌊d⌋
  };
  for (const Case& c : cases) {
    INFO(c.atom << " " << c.ms << " ms");
    Built b;
    build(b, "patch E {\n  in x : audio\n  param t = " + c.ms + "ms [" + c.ms +
                 "ms..1s]\n  out y : audio\n  d = " + c.atom + "(x + 0.5 * d, t, 1s)\n  y = d\n}\n");
    const Schedule& s = b.sched.schedule;
    REQUIRE(loopSubs(s) == std::vector<uint32_t>{c.sub});
    CHECK(worstVsOracle(b, s, {64u, 37u, 2048u}) == 0.0);
    Schedule longer = s;
    for (Region& r : longer.regions)
      if (r.kind == RegionKind::Loop) ++r.subBlock;
    CHECK(sched_exec::compare(b.typed, longer, 2048, 3, 48000.0) > 0.0);
  }
  // delay_fade reads as cubic (the oracle has no kernel variant for it; its renders are the engine tests').
  Built f;
  build(f, "patch E {\n  in x : audio\n  param t = 1ms [1ms..1s]\n  out y : audio\n  d = delay_fade(x + 0.5 * d, t, 1s)\n  y = d\n}\n");
  CHECK(loopSubs(f.sched.schedule) == std::vector<uint32_t>{47});
}

TEST_CASE("delay bounds: the cut choice — long taps cut, short delays and z1s fused inside the sub-block") {
  // One loop through a long tap and a short stage delay in series: cutting the long one breaks the cycle, so the
  // 5-sample delay runs fused inside sub-blocks of 480 (cutting both, as before FD.1, gave sub-blocks of 5).
  Built a;
  build(a, R"(patch S {
  in x : audio
  out y : audio
  long = delay(x + 0.5 * short, 10ms, 1s)
  short = delay(long, 5smp, 64smp)
  y = short
}
)");
  CHECK(loopSubs(a.sched.schedule) == std::vector<uint32_t>{480});
  CHECK(kernels(a.sched.schedule, dsp::Atom::DelayReadLinear) == 1);
  CHECK(kernels(a.sched.schedule, dsp::Atom::DelayLinear) == 1);  // the short one, fused
  CHECK(worstVsOracle(a, a.sched.schedule) == 0.0);
  // A second cycle through the short delay alone: it must be cut, and bounds the loop.
  Built b;
  build(b, R"(patch S {
  in x : audio
  out y : audio
  long = delay(x + 0.5 * short, 10ms, 1s)
  short = delay(long + 0.3 * short, 5smp, 64smp)
  y = short
}
)");
  CHECK(loopSubs(b.sched.schedule) == std::vector<uint32_t>{5});
  CHECK(kernels(b.sched.schedule, dsp::Atom::DelayReadLinear) == 2);
  CHECK(worstVsOracle(b, b.sched.schedule) == 0.0);
  // Three delays in one ring: only the longest is cut.
  Built c;
  build(c, R"(patch S {
  in x : audio
  out y : audio
  d1 = delay(x + 0.5 * d3, 3ms, 1s)
  d2 = delay_cubic(d1, 7ms, 1s)
  d3 = delay(d2, 1ms, 1s)
  y = d3
}
)");
  CHECK(loopSubs(c.sched.schedule) == std::vector<uint32_t>{335});  // cubic 7 ms: 336 − 1
  CHECK(kernels(c.sched.schedule, dsp::Atom::DelayReadCubic) == 1);
  CHECK(kernels(c.sched.schedule, dsp::Atom::DelayLinear) == 2);
  CHECK(worstVsOracle(c, c.sched.schedule) == 0.0);
  // Two rings sharing a node, one through the 2 ms tap only: the cuts are every tap of at least 2 ms.
  Built d;
  build(d, R"(patch S {
  in x : audio
  out y : audio
  m = x + 0.4 * a + 0.4 * c
  a = delay(m, 2ms, 1s)
  b = delay(m, 9ms, 1s)
  c = delay(b, 20smp, 64smp)
  y = m
}
)");
  CHECK(loopSubs(d.sched.schedule) == std::vector<uint32_t>{96});
  CHECK(kernels(d.sched.schedule, dsp::Atom::DelayReadLinear) == 2);
  CHECK(kernels(d.sched.schedule, dsp::Atom::DelayLinear) == 1);
  CHECK(worstVsOracle(d, d.sched.schedule) == 0.0);
  // maxLoopSubBlock = 1: the per-sample reference of the same patch.
  ScheduleOptions per;
  per.maxLoopSubBlock = 1;
  Built r;
  build(r, d.text, per);
  CHECK(loopSubs(r.sched.schedule) == std::vector<uint32_t>{1});
  CHECK(worstVsOracle(r, r.sched.schedule) == 0.0);
  ScheduleOptions cap;
  cap.maxLoopSubBlock = 32;
  Built k;
  build(k, d.text, cap);
  CHECK(loopSubs(k.sched.schedule) == std::vector<uint32_t>{32});
  CHECK(worstVsOracle(k, k.sched.schedule) == 0.0);
}

namespace {
// An N-lane feedback delay network from graph atoms (N a power of two): N delay lines (times a pack of N, scaled by
// `size`), log2(N) add / sub butterfly stages (an unnormalised Hadamard), a loop gain, optionally a short per-lane
// stage delay behind the matrix (a diffuser's), and the input added to every lane. `packed`: one width-N delay line
// with a pack of times, the butterflies on its channels; else N mono delay lines.
std::string fdnText(int n, bool stages, bool packed) {
  const double ms[16] = {29.7, 37.1, 41.1, 43.7, 53.3, 59.9, 61.3, 67.1, 71.9, 73.3, 79.1, 83.9, 89.3, 97.7, 101.9, 107.3};
  std::string s = "patch Fdn {\n  in x : audio\n  param g = 0.4 [0..0.5]\n  param size = 1 [1..4]\n  out y : audio\n";
  auto id = [](const char* p, int i) { return std::string(p) + std::to_string(i); };
  if (packed) {
    std::string times, fb;
    for (int i = 0; i < n; ++i) {
      times += (i ? ", " : "") + std::to_string(ms[i]) + "ms";
      fb += (i ? ", " : "") + id("m", i);
    }
    s += "  d = delay((" + fb + "), (" + times + ") * size, 0.5s)\n";
    for (int i = 0; i < n; ++i) s += "  " + id("h0_", i) + " = d[" + std::to_string(i) + "]\n";
  } else {
    for (int i = 0; i < n; ++i)
      s += "  " + id("h0_", i) + " = delay(" + id("m", i) + ", " + std::to_string(ms[i]) + "ms * size, 0.5s)\n";
  }
  int stage = 0;
  for (int span = 1; span < n; span *= 2, ++stage) {
    const std::string in = "h" + std::to_string(stage) + "_", out = "h" + std::to_string(stage + 1) + "_";
    for (int i = 0; i < n; ++i) {
      const bool upper = (i / span) % 2 == 0;
      s += "  " + id(out.c_str(), i) + " = " +
           (upper ? id(in.c_str(), i) + " + " + id(in.c_str(), i + span) : id(in.c_str(), i - span) + " - " + id(in.c_str(), i)) + "\n";
    }
  }
  const std::string last = "h" + std::to_string(stage) + "_";
  const double norm = 1.0 / std::sqrt(static_cast<double>(n));
  for (int i = 0; i < n; ++i) {
    std::string v = id(last.c_str(), i);
    if (stages) {
      s += "  " + id("s", i) + " = delay(" + v + ", " + std::to_string(3 + 2 * i) + "smp, 64smp)\n";
      v = id("s", i);
    }
    s += "  " + id("m", i) + " = x + " + v + " * (g * " + std::to_string(norm) + ")\n";
  }
  s += "  y = h0_0";
  for (int i = 1; i < n; ++i) s += " + " + id("h0_", i);
  return s + "\n}\n";
}
}  // namespace

TEST_CASE("delay bounds: an 8-lane FDN (8 delays, add / sub butterflies, gain) runs in sub-blocks of its shortest main delay") {
  const auto shortest = static_cast<uint32_t>(std::floor(static_cast<float>(0.0297) * 48000.0f));
  CHECK(shortest == 1425);
  for (const bool stages : {false, true}) {
    INFO("stages " << stages);
    Built b;
    build(b, fdnText(8, stages, false));
    const Schedule& s = b.sched.schedule;
    CHECK(loopSubs(s) == std::vector<uint32_t>{shortest});
    CHECK(kernels(s, dsp::Atom::DelayReadLinear) == 8);            // the main taps are the cuts
    CHECK(kernels(s, dsp::Atom::DelayLinear) == (stages ? 8u : 0u));  // the stage delays (3 … 17 samples) run fused
    // Against the per-sample oracle, with blocks longer than the sub-block (several sub-blocks per block). The order
    // of the ops does not change a sample: bit-identical.
    CHECK(worstVsOracle(b, s, {64u, 37u, 4096u}) == 0.0);
    ScheduleOptions per;
    per.maxLoopSubBlock = 1;
    Built r;
    build(r, b.text, per);
    CHECK(loopSubs(r.sched.schedule) == std::vector<uint32_t>{1});
    CHECK(kernels(r.sched.schedule, dsp::Atom::DelayReadLinear) == (stages ? 16u : 8u));
    CHECK(worstVsOracle(r, r.sched.schedule, {64u, 4096u}) == 0.0);
  }
  // One width-8 delay line with a pack of times (a lane cable's Delay Tap): the same sub-block, 8 reads.
  for (const int n : {8, 16}) {
    Built p;
    build(p, fdnText(n, false, true));
    CHECK(loopSubs(p.sched.schedule) == std::vector<uint32_t>{shortest});
    CHECK(kernels(p.sched.schedule, dsp::Atom::DelayReadLinear) == static_cast<uint32_t>(n));
  }
}
