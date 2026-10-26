// T1.4: the optimiser. Before/after IR (printed text and node/type queries), the tan(π·cutoff/sr) hoisting
// acceptance test (unmodulated → param rate, modulated → audio rate), each pass on its own, the TypedPatch
// invariants T1.5/T1.7 rely on, and numerical equivalence (a double reference evaluator run on the patch before and
// after optimise(), over hand-written cases, the test corpus and randomly generated patches).

#include <doctest/doctest.h>

#include "doctest_string_view.h"

#include <bit>
#include <cmath>
#include <cstdio>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "pq/graph/infer.h"
#include "pq/graph/optimise.h"
#include "pq/graph/parse.h"
#include "pq/graph/schedule.h"
#include "ref_eval.h"
#include "sched_exec.h"
#include "text_util.h"

using namespace pq::graph;

namespace {

std::string diagText(const std::vector<Diagnostic>& ds, std::string_view src) {
  std::string s;
  for (const Diagnostic& d : ds) s += formatDiagnostic(src, d) + "\n";
  return s;
}

struct Run {
  ParseResult parsed;
  PatchLibrary lib;
  TypedPatch before;
  TypedPatch after;
  OptimiseStats stats;
};

OptimiseOptions sr48k() {
  OptimiseOptions o;
  o.sampleRate = 48000.0;
  return o;
}

// Parses (every patch of the file is in the library), infers the first patch and optimises it.
void optimiseText(Run& r, std::string_view src, const OptimiseOptions& o = sr48k(),
                  std::span<const RootInput> inputs = {}) {
  r.parsed = parseDetailed(src, "test.pqp");
  REQUIRE_MESSAGE(r.parsed.ok(), diagText(r.parsed.diagnostics, src));
  r.lib.add(r.parsed.file);
  InferOptions io;
  io.library = &r.lib;
  io.inputs = inputs;
  InferResult inf = inferDetailed(r.parsed.file.patches[0], io);
  REQUIRE_MESSAGE(inf.ok(), diagText(inf.diagnostics, src));
  r.before = inf.typed;
  auto res = optimise(std::move(inf.typed), o, &r.stats);
  const std::string err = res.ok() ? std::string() : res.error().message;
  REQUIRE_MESSAGE(res.ok(), err);
  r.after = std::move(res.value());
}

// The invariants every TypedPatch keeps (infer.h) plus the optimiser's: stateful nodes survive with their state
// keys and atoms, nothing pure is left unreferenced, every port type is concrete.
void checkInvariants(const TypedPatch& before, const TypedPatch& after) {
  const Patch& p = after.patch;
  REQUIRE(after.types.size() == p.portCount());
  REQUIRE(after.stateKey.size() == p.nodeCount());
  REQUIRE(after.origin.size() == p.nodeCount());
  CHECK(p.validateAll().empty());
  for (const PortType& t : after.types) {
    CHECK(t.kind != Kind::Infer);
    CHECK(t.rate != Rate::Infer);
    CHECK(t.poly != Poly::Infer);
    CHECK(t.width != kWidthInfer);
  }
  const Adjacency adj = p.adjacency();
  for (NodeIndex n = 0; n < p.nodeCount(); ++n) {
    CHECK(p.node(n).kind != NodeKind::Subcircuit);
    for (uint32_t i = 0; i < p.node(n).numInputs; ++i) {
      const WireIndex w = adj.driver[p.flatInput(n, i)];
      if (w == kNone) {
        CHECK(after.inputType(n, i).rate == Rate::Const);
        continue;
      }
      const Wire& wr = p.wire(w);
      CHECK(after.inputType(n, i) == after.outputType(wr.from.node, wr.from.port));
    }
  }
  // Stateful nodes: same multiset of (stateKey, atom) before and after, minus the dead ones (only removed, never
  // re-keyed). Their output types are unchanged.
  std::map<uint64_t, std::pair<AtomId, PortType>> statefulBefore;
  for (NodeIndex n = 0; n < before.patch.nodeCount(); ++n) {
    const Node& nd = before.patch.node(n);
    if (nd.kind == NodeKind::Atom && atomSignature(nd.atom).has(atom_flags::kStateful) && nd.numOutputs > 0)
      statefulBefore[before.stateKey[n]] = {nd.atom, before.outputType(n, 0)};
  }
  std::set<uint64_t> keys;
  for (NodeIndex n = 0; n < p.nodeCount(); ++n) {
    const Node& nd = p.node(n);
    if (nd.kind != NodeKind::Atom || !atomSignature(nd.atom).has(atom_flags::kStateful)) continue;
    CHECK(keys.insert(after.stateKey[n]).second);  // never merged, never duplicated
    const auto it = statefulBefore.find(after.stateKey[n]);
    REQUIRE_MESSAGE(it != statefulBefore.end(), "a stateful node with a new state key: " << p.describe(n));
    CHECK(it->second.first == nd.atom);
    if (nd.numOutputs > 0) CHECK(it->second.second == after.outputType(n, 0));
  }
  // Nothing dead is left (every non-root node feeds something).
  for (NodeIndex n = 0; n < p.nodeCount(); ++n) {
    const Node& nd = p.node(n);
    const bool root = nd.kind == NodeKind::Output || nd.kind == NodeKind::Export || nd.kind == NodeKind::Param ||
                      nd.kind == NodeKind::Input || nd.kind == NodeKind::Equation ||
                      (nd.kind == NodeKind::Atom && atomSignature(nd.atom).has(atom_flags::kSideEffect));
    if (!root) CHECK_MESSAGE(!adj.outgoing(n).empty(), "dead node left: " << p.describe(n));
  }
}

NodeIndex node(const TypedPatch& tp, std::string_view name) {
  const auto n = tp.patch.findByName(name);
  REQUIRE_MESSAGE(n.has_value(), "no node '" << std::string(name) << "'");
  return *n;
}

// Rate of a node's value (for an `out`/`export`: of what arrives at it).
Rate outRate(const TypedPatch& tp, std::string_view name) {
  const NodeIndex n = node(tp, name);
  return tp.patch.node(n).numOutputs > 0 ? tp.outputType(n, 0).rate : tp.inputType(n, 0).rate;
}

uint32_t countAtom(const TypedPatch& tp, std::string_view atom) {
  uint32_t c = 0;
  for (const Node& nd : tp.patch.nodes())
    if (nd.kind == NodeKind::Atom && atomSignature(nd.atom).name == atom) ++c;
  return c;
}

uint32_t countKind(const TypedPatch& tp, NodeKind k) {
  uint32_t c = 0;
  for (const Node& nd : tp.patch.nodes()) c += nd.kind == k;
  return c;
}

// The node driving input i of n (kNone if unconnected).
NodeIndex driver(const TypedPatch& tp, NodeIndex n, uint32_t i) {
  const Adjacency adj = tp.patch.adjacency();
  const WireIndex w = adj.driver[tp.patch.flatInput(n, i)];
  return w == kNone ? kNone : tp.patch.wire(w).from.node;
}

std::string atomName(const TypedPatch& tp, NodeIndex n) {
  const Node& nd = tp.patch.node(n);
  return nd.kind == NodeKind::Atom ? std::string(atomSignature(nd.atom).name) : std::string(toText(nd.kind));
}

double constValue(const TypedPatch& tp, NodeIndex n) {
  REQUIRE(tp.patch.node(n).kind == NodeKind::Const);
  return tp.patch.constant(n).value();
}

// The body of the printed patch without the `patch Name {` line and the closing brace.
std::string body(const TypedPatch& tp) {
  // Without the `patch Name {` line, the closing brace and blank lines.
  const std::string s = print(tp.patch);
  std::string r;
  size_t pos = s.find('\n') + 1;
  while (pos < s.size()) {
    size_t e = s.find('\n', pos);
    if (e == std::string::npos) e = s.size();
    const std::string line = s.substr(pos, e - pos);
    if (!line.empty() && line != "}") r += line + "\n";
    pos = e + 1;
  }
  return r;
}

void checkEquivalent(const Run& r, uint64_t samples = 64, double tol = 1e-6) {
  ref_eval::Evaluator probe(r.before);
  REQUIRE(probe.supported());
  const double d = ref_eval::maxRelDiff(r.before, r.after, samples);
  CHECK_MESSAGE(d <= tol, "max relative difference " << d << "\nbefore:\n" << print(r.before.patch) << "after:\n"
                                                     << print(r.after.patch));
}

}  // namespace

// ---- acceptance: hoisting tan(π·cutoff/sr) ------------------------------------------------------------------------

TEST_CASE("optimise: tan(π·cutoff/sr) is param rate when cutoff is unmodulated") {
  Run r;
  optimiseText(r, R"(patch Filter {
  in x : audio
  param cutoff = 1000 [20..20000]
  out y : audio
  g = tan(3.14159265 * cutoff / sr)
  y = x * g
})");
  checkInvariants(r.before, r.after);
  CHECK(outRate(r.after, "g") == Rate::Param);  // computed once per block
  CHECK(outRate(r.after, "y") == Rate::Audio);
  // The audio-rate path is the multiply only: the tan, the scaling and the division all run at param rate.
  for (NodeIndex n = 0; n < r.after.patch.nodeCount(); ++n) {
    const std::string a = atomName(r.after, n);
    if (a == "tan" || a == "div" || (a == "mul" && n != driver(r.after, node(r.after, "y"), 0)))
      CHECK_MESSAGE(r.after.outputType(n, 0).rate <= Rate::Param, a);
  }
  CHECK(countAtom(r.after, "sr") == 0);  // folded to 48000 (the program is specialised for the sample rate)
  CHECK(body(r.after) == R"(  param cutoff = 1000 [20..20000]
  in x : audio
  out y : audio
  g = tan(3.14159265 * cutoff / 48000)
  y = x * g
)");
  checkEquivalent(r);
}

TEST_CASE("optimise: tan(π·cutoff/sr) stays audio rate when cutoff is modulated") {
  Run r;
  optimiseText(r, R"(patch Filter {
  in x : audio
  param cutoff = 1000 [20..20000]
  out y : audio
  lfo = sine(phasor(3Hz))
  g = tan(3.14159265 * (cutoff + lfo * 100) / sr)
  y = x * g
})");
  checkInvariants(r.before, r.after);
  CHECK(outRate(r.after, "g") == Rate::Audio);
  CHECK(outRate(r.after, "y") == Rate::Audio);
  checkEquivalent(r);
}

TEST_CASE("optimise: modulation that simplifies away hoists the path back to param rate") {
  // lfo·0 is +0 (the phasor is finite and ≥ 0), so cutoff + lfo·0 = cutoff + 0 has no audio input left; the
  // `cutoff + 0` itself stays (cutoff could be −0, and −0 + 0 = +0) but drops to param rate with everything after it.
  Run r;
  optimiseText(r, R"(patch Filter {
  in x : audio
  param cutoff = 1000 [20..20000]
  param on = false
  out y : audio
  out y2 : audio
  lfo = phasor(3Hz)
  g = tan(3.14159265 * (cutoff + lfo * 0) / sr)
  h = tan(3.14159265 * (false ? lfo : cutoff) / sr)
  y = x * g
  y2 = x * h
})");
  checkInvariants(r.before, r.after);
  CHECK(outRate(r.before, "g") == Rate::Audio);
  CHECK(outRate(r.before, "h") == Rate::Audio);
  CHECK(outRate(r.after, "g") == Rate::Param);
  CHECK(outRate(r.after, "h") == Rate::Param);
  CHECK(countAtom(r.after, "phasor") == 0);  // dead once nothing reads it
  CHECK(r.stats.ratesLowered >= 2);
  checkEquivalent(r);
}

TEST_CASE("optimise: per call site, one filter instance hoisted and one modulated") {
  Run r;
  optimiseText(r, R"(patch Synth {
  in x : audio
  param cutoff = 1000 [20..20000]
  out a : audio
  out b : audio
  fa = Svf(x, cutoff: cutoff)
  fb = Svf(x, cutoff: cutoff + sine(phasor(3Hz)) * 100)
  a = fa
  b = fb
}
patch Svf {
  in x : audio
  param cutoff = 1000 [20..20000]
  out lp : audio
  g = tan(3.14159265 * cutoff / sr)
  a1 = 1 / (1 + g * (g + 1.4))
  v1 = a1 * (x - s) + s * 0
  s = z1(2 * v1 - s)
  lp = v1
})");
  checkInvariants(r.before, r.after);
  CHECK(outRate(r.after, "fa__g") == Rate::Param);
  CHECK(outRate(r.after, "fa__a1") == Rate::Param);
  CHECK(outRate(r.after, "fb__g") == Rate::Audio);
  CHECK(outRate(r.after, "fb__a1") == Rate::Audio);
  checkEquivalent(r, 256);
}

// ---- constant folding ---------------------------------------------------------------------------------------------

TEST_CASE("optimise: constant folding folds literals and never params") {
  Run r;
  optimiseText(r, R"(patch F {
  in x : audio
  param p = 0.5 [0..1]
  out y : audio
  out z : audio
  out w : audio
  y = x * (2 * 3 + 1)
  z = x * (p * (4 - 2))
  w = x + tanh(0.5) + db_to_gain(-6dB) * 0 + (1 < 2 ? 3 : 4)
})");
  checkInvariants(r.before, r.after);
  const NodeIndex y = node(r.after, "y");
  CHECK(constValue(r.after, driver(r.after, driver(r.after, y, 0), 1)) == 7.0);
  // p * 2 stays a multiply (a param is a runtime value), at param rate.
  const NodeIndex zmul = driver(r.after, node(r.after, "z"), 0);
  const NodeIndex pmul = driver(r.after, zmul, 1);
  CHECK(atomName(r.after, pmul) == "mul");
  CHECK(r.after.patch.node(driver(r.after, pmul, 0)).kind == NodeKind::Param);
  CHECK(constValue(r.after, driver(r.after, pmul, 1)) == 2.0);
  CHECK(r.after.outputType(pmul, 0).rate == Rate::Param);
  // w = x + c with c = tanh(0.5) + 0 + 3 folded by the kernels themselves, in float32.
  const NodeIndex wadd = driver(r.after, node(r.after, "w"), 0);
  CHECK(atomName(r.after, wadd) == "add");
  CHECK(countAtom(r.after, "tanh") == 0);
  CHECK(countAtom(r.after, "db_to_gain") == 0);
  CHECK(countAtom(r.after, "select") == 0);
  checkEquivalent(r);
}

TEST_CASE("optimise: folded values are bit-identical to the kernels' (fast_ atoms fold to their own approximation)") {
  Run r;
  optimiseText(r, R"(patch F {
  out a : real
  out b : real
  out c : bool
  out d : real
  a = fast_tanh(0.5)
  b = fast_exp(1)
  c = !(0.1 + 0.2 == 0.3)
  d = 1e30 * 1e30
})");
  checkInvariants(r.before, r.after);
  // 1e30 · 1e30 is inf in float32: literals are finite (Patch::validate), so it stays a (const-rate) computation.
  CHECK(countKind(r.after, NodeKind::Atom) == 1);
  CHECK(atomName(r.after, driver(r.after, node(r.after, "d"), 0)) == "mul");
  // fast_tanh(0.5) is the approximation's value, not tanh(0.5) rounded (no silent fast/exact substitution).
  const double a = constValue(r.after, driver(r.after, node(r.after, "a"), 0));
  CHECK(a == static_cast<double>(static_cast<float>(a)));
  CHECK(std::fabs(a - std::tanh(0.5)) < 1e-3);
  // In float32, 0.1f + 0.2f == 0.3f (in double it would not be): folding uses the program's arithmetic.
  const NodeIndex cn = driver(r.after, node(r.after, "c"), 0);
  REQUIRE(r.after.patch.node(cn).kind == NodeKind::Const);
  CHECK(r.after.patch.constant(cn).kind == Kind::Bool);
  CHECK(r.after.patch.constant(cn).literal == 0.0);
}

namespace {

// Folded (literal arguments) vs unfolded (the same expression on params, scheduled and run the way a backend runs
// it: Control regions on the 8-lane kernels) for `count` random arguments of every exact transcendental, compared
// bit for bit (M1 review M4: folding used the 1-lane kernels, i.e. the CRT, and differed by an ulp from SVML).
struct FoldFn {
  const char* name;
  int arity;
  float (*arg)(std::mt19937&, int);
};

float uni(std::mt19937& g, float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(g); }

const FoldFn kFoldFns[] = {
    {"sin", 1, [](std::mt19937& g, int) { return uni(g, -1e4f, 1e4f) / (g() % 2 ? 1.0f : 1000.0f); }},
    {"tan", 1, [](std::mt19937& g, int) { return uni(g, -1e3f, 1e3f) / (g() % 2 ? 1.0f : 100.0f); }},
    {"tanh", 1, [](std::mt19937& g, int) { return uni(g, -20.0f, 20.0f); }},
    {"exp", 1, [](std::mt19937& g, int) { return uni(g, -87.0f, 88.0f); }},
    {"log", 1, [](std::mt19937& g, int) { return std::exp(uni(g, -80.0f, 80.0f)); }},
    {"db_to_gain", 1, [](std::mt19937& g, int) { return uni(g, -300.0f, 300.0f); }},
    {"gain_to_db", 1, [](std::mt19937& g, int) { return std::exp(uni(g, -40.0f, 40.0f)); }},
    {"pow", 2, [](std::mt19937& g, int i) { return i == 0 ? std::exp(uni(g, -5.0f, 5.0f)) : uni(g, -10.0f, 10.0f); }},
    {"cents_to_hz", 2, [](std::mt19937& g, int i) { return i == 0 ? uni(g, -20000.0f, 20000.0f) : uni(g, 1.0f, 20000.0f); }},
    {"hz_to_cents", 2, [](std::mt19937& g, int i) { return i == 0 ? std::exp(uni(g, -5.0f, 10.0f)) : uni(g, 1.0f, 2000.0f); }},
};

std::string num9(float v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.9g", static_cast<double>(v));  // round-trips the float exactly
  return buf;
}

void foldMatchesRuntime(int count) {
  constexpr int kChunk = 1000;  // arguments per patch
  std::mt19937 rng(4242);
  for (const FoldFn& fn : kFoldFns) {
    const std::string fnName = fn.name;
    CAPTURE(fnName);
    int compared = 0, nonFinite = 0;
    for (int base = 0; base < count; base += kChunk) {
      const int n = std::min(kChunk, count - base);
      std::string folded = "patch F {\n", params = "patch U {\n";
      for (int i = 0; i < n; ++i) {
        std::string lits, refs;
        for (int a = 0; a < fn.arity; ++a) {
          const std::string v = num9(fn.arg(rng, a));
          const std::string pn = "p" + std::to_string(i) + "_" + std::to_string(a);
          params += "  param " + pn + " = " + v + "\n";
          lits += (a ? ", " : "") + v;
          refs += (a ? ", " : "") + pn;
        }
        const std::string y = "y" + std::to_string(i);
        folded += "  out " + y + " : real\n  " + y + " = " + fn.name + "(" + lits + ")\n";
        params += "  out " + y + " : real\n  " + y + " = " + fn.name + "(" + refs + ")\n";
      }
      folded += "}\n";
      params += "}\n";
      Run f, u;
      optimiseText(f, folded);
      optimiseText(u, params);
      auto sched = schedule(u.after);
      REQUIRE_MESSAGE(sched.ok(), (sched.ok() ? std::string() : sched.error().message));
      const Schedule& s = sched.value();
      const TypedPatch& sp = s.rewritten ? *s.rewritten : u.after;
      sched_exec::Executor ex(s, 8);
      const auto outs = ex.block(0);
      REQUIRE(outs.size() == s.outputs.size());
      for (size_t o = 0; o < s.outputs.size(); ++o) {
        const std::string& name = sp.patch.nodeName(s.outputs[o].node);
        const NodeIndex drv = driver(f.after, node(f.after, name), 0);
        const float runtime = outs[o][0];
        if (f.after.patch.node(drv).kind != NodeKind::Const) {  // inf/NaN results stay computations (Q34.5)
          CHECK_FALSE(std::isfinite(runtime));
          ++nonFinite;
          continue;
        }
        const auto lit = static_cast<float>(f.after.patch.constant(drv).value());
        CHECK_MESSAGE(std::bit_cast<uint32_t>(lit) == std::bit_cast<uint32_t>(runtime),
                      name << ": folded " << num9(lit) << " vs runtime " << num9(runtime));
        ++compared;
      }
    }
    CHECK(compared + nonFinite == count);
    CHECK(compared >= count * 9 / 10);
  }
}

}  // namespace

TEST_CASE("optimise: folded == unfolded bit for bit for every exact transcendental (200 arguments each)") {
  foldMatchesRuntime(200);
}

TEST_CASE("optimise: folded == unfolded bit for bit for every exact transcendental (10^4 arguments each)" *
          doctest::test_suite("slow")) {
  foldMatchesRuntime(10000);
}

TEST_CASE("optimise: sr folds only with a known sample rate") {
  Run r;
  OptimiseOptions o;  // sampleRate 0: unknown
  optimiseText(r, "patch S { in x : audio\n out y : audio\n y = x * (sr * 0.5) }", o);
  checkInvariants(r.before, r.after);
  CHECK(countAtom(r.after, "sr") == 1);
  CHECK(outRate(r.after, "y") == Rate::Audio);
  CHECK(r.after.outputType(driver(r.after, driver(r.after, node(r.after, "y"), 0), 1), 0).rate == Rate::Const);
  Run k;
  optimiseText(k, "patch S { in x : audio\n out y : audio\n y = x * (sr * 0.5) }");
  CHECK(countAtom(k.after, "sr") == 0);
  CHECK(body(k.after).find("x * 24000") != std::string::npos);
}

TEST_CASE("optimise: a channel index folded from an expression is checked") {
  Run ok;
  optimiseText(ok, "patch C { in st : audio stereo\n out y : audio\n y = st[3 - 2] }");
  checkInvariants(ok.before, ok.after);
  CHECK(countAtom(ok.after, "channel") == 1);

  ParseResult parsed = parseDetailed("patch C { in st : audio stereo\n out y : audio\n y = st[3 - 1] }", "c.pqp");
  REQUIRE(parsed.ok());
  auto inf = infer(parsed.file.patches[0]);
  REQUIRE(inf.ok());
  auto res = optimise(std::move(inf.value()), sr48k());
  REQUIRE_FALSE(res.ok());
  CHECK(res.error().code == pq::ErrorCode::Type);
  CHECK(res.error().message.find("channel index 2 is not a channel of a 2-channel signal") != std::string::npos);
}

// ---- algebraic simplification ---------------------------------------------------------------------------------------

TEST_CASE("optimise: exact algebraic identities") {
  Run r;
  optimiseText(r, R"(patch A {
  in x : audio
  in g : bool
  param p = 0.5 [0..1]
  out o1 : audio
  out o2 : audio
  out o3 : audio
  out o4 : audio
  out o5 : audio
  out o6 : audio
  out o7 : audio
  out o8 : audio
  out b1 : bool
  out b2 : bool
  out b3 : bool
  o1 = (x * 1) / 1 + -0 - 0
  o2 = x / 4
  o3 = x / 3
  o4 = -(-x) * -1
  o5 = pow(x, 2) + pow(p, 1)
  o6 = (true ? x : p) + mux(1, p, x, p)
  o7 = abs(-x) + abs(abs(p)) + min(x, x)
  o8 = (x > 0 ? x : x) * 1
  b1 = g && true || false
  b2 = g ^^ true
  b3 = !(!g) && (g || true)
})",
               [] {
                 OptimiseOptions o = sr48k();
                 o.fuseMulAdd = false;  // look at the rewrites themselves
                 return o;
               }());
  checkInvariants(r.before, r.after);
  CHECK(body(r.after) == R"(  param p = 0.5 [0..1]
  in x : audio
  in g : bool
  out o1 : audio
  out o2 : audio
  out o3 : audio
  out o4 : audio
  out o5 : audio
  out o6 : audio
  out o7 : audio
  out o8 : audio
  out b1 : bool
  out b2 : bool
  out b3 : bool
  o1 = x
  o2 = x * 0.25
  o3 = x / 3
  o4 = -x
  o5 = x * x + p
  o6 = x + x
  o7 = abs(x) + abs(p) + x
  o8 = x
  b1 = g
  b2 = !g
  b3 = g
)");
  checkEquivalent(r);
}

TEST_CASE("optimise: identities that are not exact in float32 are left alone") {
  Run r;
  optimiseText(r, R"(patch N {
  in x : audio
  param p = 0.5 [0..1]
  out o1 : audio
  out o2 : audio
  out o3 : audio
  out o4 : audio
  out o5 : audio
  o1 = x * 0
  o2 = p + 0
  o3 = x - -0
  o4 = min(x, p) + min(p, x)
  o5 = (x * p) * 2
})");
  checkInvariants(r.before, r.after);
  // x·0 is NaN for an infinite x and −0 for a negative one; p + 0 is +0 for p = −0; min is order-dependent on NaN;
  // (x·p)·2 = x·(p·2) only up to rounding. All five stay as written (the two literal zeros are one node now: CSE).
  CHECK(body(r.after) == R"(  param p = 0.5 [0..1]
  in x : audio
  out o1 : audio
  out o2 : audio
  out o3 : audio
  out o4 : audio
  out o5 : audio
  _n16 = 0
  o1 = x * _n16
  o2 = p + _n16
  o3 = x - -0
  o4 = min(x, p) + min(p, x)
  o5 = x * p * 2
)");
}

TEST_CASE("optimise: x·0 and x+0 when the analysis proves them exact") {
  Run r;
  optimiseText(r, R"(patch Z {
  in x : audio
  out o1 : audio
  out o2 : audio
  out o3 : audio
  ph = phasor(110Hz)
  o1 = x + ph * 0
  o2 = x * (clip(x, -1, 1) * -0 + 1)
  o3 = gate_to_real(x > 0) + 0
})");
  checkInvariants(r.before, r.after);
  // ph ≥ +0 and finite: ph·0 = +0 and x + (+0)... stays (x may be −0). clip(x, −1, 1) is finite but may be negative,
  // so its product with −0 has an unknown sign: kept. gate_to_real is 0 or 1: + 0 goes.
  CHECK(countAtom(r.after, "phasor") == 0);
  CHECK(atomName(r.after, driver(r.after, node(r.after, "o1"), 0)) == "add");
  CHECK(countAtom(r.after, "clip") == 1);
  CHECK(atomName(r.after, driver(r.after, node(r.after, "o3"), 0)) == "gate_to_real");
  checkEquivalent(r);
}

TEST_CASE("optimise: replacements never change a value's poly or width") {
  Run r;
  optimiseText(r, R"(patch P {
  in st : audio stereo
  param p = 0.5 [0..1]
  out o : audio voice
  out w : audio stereo
  out ch : audio
  o = mux(0, p, voice.hz)
  w = (st, st)[0] + pack(st, p)[2] * 1
  ch = (p, voice.velocity)[1]
})");
  checkInvariants(r.before, r.after);
  // mux(0, p, voice.hz) is p, but p is global and the mux is per voice: replacing it would change what a voice
  // reducer downstream computes (sum of N voices vs once). Kept.
  CHECK(atomName(r.after, driver(r.after, node(r.after, "o"), 0)) == "mux");
  // channel 0 of (st, st) is a stereo pack input → not mono → channel stays; channel 2 of pack(st, p) is p.
  CHECK(countAtom(r.after, "channel") == 1);
  // (p, voice.velocity)[1]: the mono pack input voice.velocity, same poly → replaced.
  CHECK(atomName(r.after, driver(r.after, node(r.after, "ch"), 0)) == "voice.velocity");
}

// T1b.x-perf (QUESTIONS Q132): a structural switch that picks a literal (`on ? x : 0` with `on` constant false) used to
// keep the select and everything feeding x when x was per voice or wider than the literal (a replacement may not
// change poly or width). The picked literal now becomes a literal typed like the select, so the off section is gone.
TEST_CASE("optimise: a constant select / mux picking a literal of another poly or width folds to a literal of its type") {
  Run r;
  optimiseText(r, R"(patch S {
  out o : audio voice
  out w : audio stereo
  out m : audio voice
  out g : audio global
  s = sine(phasor(voice.hz))
  t = sine(phasor(voice.hz * 2))
  o = 0 >= 0.5 ? s * 2 : 0
  w = 1 > 2 ? (sine(phasor(220Hz)), sine(phasor(330Hz))) : 0.25
  m = mux(1, s, 0.5)
  g = voice_sum(0 > 1 ? t : 0)
})");
  checkInvariants(r.before, r.after);
  CHECK(countAtom(r.after, "phasor") == 0);  // every oscillator was in an off section
  CHECK(countAtom(r.after, "sine") == 0);
  CHECK(countAtom(r.after, "select") == 0);
  CHECK(countAtom(r.after, "mux") == 0);
  const NodeIndex o = driver(r.after, node(r.after, "o"), 0);
  REQUIRE(o != kNone);
  CHECK(constValue(r.after, o) == 0.0);
  CHECK(r.after.outputType(o, 0).poly == Poly::Voice);
  CHECK(r.after.outputType(o, 0).rate == Rate::Const);
  const NodeIndex w = driver(r.after, node(r.after, "w"), 0);
  REQUIRE(w != kNone);
  CHECK(constValue(r.after, w) == 0.25);
  CHECK(r.after.outputType(w, 0).width == 2);
  CHECK(r.after.outputType(w, 0).poly == Poly::Global);
  const NodeIndex m = driver(r.after, node(r.after, "m"), 0);
  REQUIRE(m != kNone);
  CHECK(constValue(r.after, m) == 0.5);
  CHECK(r.after.outputType(m, 0).poly == Poly::Voice);
  // A voice reducer of the per-voice literal stays a reducer (Σ over the voices of 0), and the patch schedules.
  CHECK(countAtom(r.after, "voice_sum") == 1);
  CHECK(schedule(r.after).ok());
  // The literal arm of the same poly and width keeps folding to the shared literal, as before.
  Run q;
  optimiseText(q, "patch Q {\n  out y : audio\n  y = 0 > 1 ? sine(phasor(220Hz)) : 0\n}");
  checkInvariants(q.before, q.after);
  CHECK(countAtom(q.after, "phasor") == 0);
  checkEquivalent(q);
}

TEST_CASE("optimise: a voice reducer of a global value passes it through") {
  Run r;
  optimiseText(r, R"(patch R {
  param p = 0.5 [0..1]
  out o : audio global
  out v : audio global
  o = voice_highest(p * 2)
  v = voice.hz * p
})");
  checkInvariants(r.before, r.after);
  CHECK(countAtom(r.after, "voice_highest") == 0);
  CHECK(countAtom(r.after, "voice_sum") == 1);  // the inserted reducer of a real voice signal stays
}

// ---- CSE ------------------------------------------------------------------------------------------------------------

TEST_CASE("optimise: CSE merges pure duplicates, never stateful ones") {
  Run r;
  optimiseText(r, R"(patch C {
  in x : audio
  param p = 0.5 [0..1]
  out o1 : audio
  out o2 : audio
  out o3 : audio
  a = tanh(x * p)
  b = tanh(p * x)
  o1 = a + b
  o2 = z1(x) + z1(x) + noise() + noise()
  o3 = voice.hz + voice.hz + 2 + 2
})");
  checkInvariants(r.before, r.after);
  CHECK(countAtom(r.after, "tanh") == 1);   // commutative inputs canonicalised
  CHECK(countAtom(r.after, "z1") == 2);     // each holds its own state
  CHECK(countAtom(r.after, "noise") == 2);  // different seeds (state keys)
  CHECK(countAtom(r.after, "voice.hz") == 1);
  const NodeIndex o1 = driver(r.after, node(r.after, "o1"), 0);
  CHECK(driver(r.after, o1, 0) == driver(r.after, o1, 1));
  CHECK(r.stats.merged >= 4);
  // The stateful nodes kept their state keys.
  std::set<uint64_t> z1Before, z1After;
  for (NodeIndex n = 0; n < r.before.patch.nodeCount(); ++n)
    if (atomName(r.before, n) == "z1") z1Before.insert(r.before.stateKey[n]);
  for (NodeIndex n = 0; n < r.after.patch.nodeCount(); ++n)
    if (atomName(r.after, n) == "z1") z1After.insert(r.after.stateKey[n]);
  CHECK(z1Before == z1After);
  CHECK(z1After.size() == 2);
}

// ---- DCE ------------------------------------------------------------------------------------------------------------

TEST_CASE("optimise: dead nodes go; outputs, exports, sends, params and inputs stay") {
  Run r;
  optimiseText(r, R"(patch D {
  in x : audio
  in unused : audio
  param p = 0.5 [0..1]
  param q = 0.5 [0..1]
  out o : audio
  dead = tanh(x * q) + phasor(p * 100)
  alsoDead = z1(dead)
  s = send(x * p, bus: 1)
  o = x
  export e = x * p
  solve(iterations: 4) {
    unknown u = 0
    u == tanh(x - u)
  }
})");
  checkInvariants(r.before, r.after);
  CHECK(countAtom(r.after, "tanh") == 0);
  CHECK(countAtom(r.after, "phasor") == 0);
  CHECK(countAtom(r.after, "z1") == 0);
  CHECK(countAtom(r.after, "send") == 1);
  CHECK(countKind(r.after, NodeKind::Param) == 2);
  CHECK(countKind(r.after, NodeKind::Input) == 2);
  CHECK(countKind(r.after, NodeKind::Export) == 1);
  // The solve block's unknown feeds nothing: the whole block is dead.
  CHECK(countKind(r.after, NodeKind::Unknown) == 0);
  CHECK(countKind(r.after, NodeKind::Equation) == 0);
  CHECK(r.after.patch.solveCount() == 0);
  // x * p is shared by the send and the export (CSE), so one mul is left.
  CHECK(countAtom(r.after, "mul") == 1);
}

TEST_CASE("optimise: a live solve block is kept whole") {
  Run r;
  optimiseText(r, R"(patch L {
  in x : audio
  out o : audio
  solve(iterations: 4) {
    unknown u = 0
    unknown v
    u == tanh(x - v * 1)
    v == u * 0.5 + 0
  }
  o = u
})");
  checkInvariants(r.before, r.after);
  CHECK(countKind(r.after, NodeKind::Unknown) == 2);
  CHECK(countKind(r.after, NodeKind::Equation) == 2);
  CHECK(r.after.patch.solveCount() == 1);
  CHECK(r.after.patch.solves()[0].iterations == 4);
}

// ---- fusion -------------------------------------------------------------------------------------------------------

TEST_CASE("optimise: a·b + c fuses into muladd only at one rate") {
  Run r;
  optimiseText(r, R"(patch M {
  in x : audio
  in y : audio
  param p = 0.5 [0..1]
  param q = 0.25 [0..1]
  out o1 : audio
  out o2 : audio
  out o3 : audio
  o1 = x * p + y
  o2 = p * q + x
  shared = x * y
  o3 = shared + p + shared
})");
  checkInvariants(r.before, r.after);
  CHECK(atomName(r.after, driver(r.after, node(r.after, "o1"), 0)) == "muladd");
  // p·q is param rate; fusing it into the audio add would compute it per sample.
  const NodeIndex o2 = driver(r.after, node(r.after, "o2"), 0);
  CHECK(atomName(r.after, o2) == "add");
  CHECK(r.after.outputType(driver(r.after, o2, 0), 0).rate == Rate::Param);
  // A product with two consumers is not fused (it would be computed twice).
  CHECK(countAtom(r.after, "muladd") == 1);
  checkEquivalent(r, 64, 1e-9);

  Run off;
  OptimiseOptions o = sr48k();
  o.fuseMulAdd = false;
  optimiseText(off, "patch M { in x : audio\n param p = 0.5\n out o : audio\n o = x * p + x }", o);
  CHECK(countAtom(off.after, "muladd") == 0);
}

// ---- durations ------------------------------------------------------------------------------------------------------

TEST_CASE("optimise: durations become samples as const/param math (QUESTIONS Q30.6)") {
  Run r;
  optimiseText(r, R"(patch T {
  in x : audio
  param t = 10ms [1ms..1s]
  out o1 : audio
  out o2 : audio
  out o3 : audio
  out o4 : bool
  o1 = delay(x, 5ms, 1s)
  o2 = delay(x, 5smp, 4800smp)
  o3 = delay(x, t, 1s)
  o4 = timer(x > 0, t * 2) || pulse()
})");
  checkInvariants(r.before, r.after);
  CHECK(r.after.durationsInSamples);
  CHECK_FALSE(r.before.durationsInSamples);
  const NodeIndex d1 = driver(r.after, node(r.after, "o1"), 0);
  CHECK(constValue(r.after, driver(r.after, d1, 1)) == static_cast<double>(0.005f * 48000.0f));
  CHECK(constValue(r.after, driver(r.after, d1, 2)) == 1.0);  // max stays seconds (the builder sizes the ring)
  const NodeIndex d2 = driver(r.after, node(r.after, "o2"), 0);
  CHECK(constValue(r.after, driver(r.after, d2, 1)) == 5.0);  // an smp literal is already samples
  CHECK(constValue(r.after, driver(r.after, d2, 2)) == 0.1);  // 4800 samples as seconds at 48 kHz
  CHECK(r.after.patch.constant(driver(r.after, d2, 2)).unit == Unit::s);
  // A param duration: t · 48000, once per block.
  const NodeIndex d3 = driver(r.after, node(r.after, "o3"), 0);
  const NodeIndex conv = driver(r.after, d3, 1);
  CHECK(atomName(r.after, conv) == "mul");
  CHECK(r.after.outputType(conv, 0).rate == Rate::Param);
  CHECK(r.after.inputType(d3, 1).rate == Rate::Param);
  // timer(…, t·2): (t·2)·48000 at param rate; pulse() keeps its unconnected 0 (a single pulse).
  for (NodeIndex n = 0; n < r.after.patch.nodeCount(); ++n) {
    if (atomName(r.after, n) == "timer") CHECK(r.after.inputType(n, 1).rate == Rate::Param);
    if (atomName(r.after, n) == "pulse") CHECK(driver(r.after, n, 0) == kNone);
  }
  CHECK(r.stats.durations == 5);

  // Idempotent: a second run inserts nothing.
  OptimiseStats again;
  auto twice = optimise(r.after, sr48k(), &again);
  REQUIRE(twice.ok());
  CHECK(again.durations == 0);
  CHECK(twice.value().patch == r.after.patch);
}

TEST_CASE("optimise: durations without a known sample rate multiply by the sr atom") {
  Run r;
  OptimiseOptions o;
  optimiseText(r, "patch T { in x : audio\n out o : audio\n o = delay(x, 5ms, 1s) }", o);
  checkInvariants(r.before, r.after);
  const NodeIndex conv = driver(r.after, driver(r.after, node(r.after, "o"), 0), 1);
  CHECK(atomName(r.after, conv) == "mul");
  CHECK(atomName(r.after, driver(r.after, conv, 1)) == "sr");
  CHECK(r.after.outputType(conv, 0).rate == Rate::Const);
}

TEST_CASE("optimise: an smp literal through arithmetic into a duration input is an error (M1 review L1)") {
  auto errorOf = [](std::string_view src) {
    ParseResult parsed = parseDetailed(src, "test.pqp");
    REQUIRE(parsed.ok());
    PatchLibrary lib;
    lib.add(parsed.file);
    InferOptions io;
    io.library = &lib;
    InferResult inf = inferDetailed(parsed.file.patches[0], io);
    REQUIRE(inf.ok());
    auto res = optimise(std::move(inf.typed), sr48k());
    return res.ok() ? std::string() : res.error().message;
  };
  const std::string e1 = errorOf("patch T { in x : audio\n out o : audio\n o = delay(x, 5smp * 2, 1s) }");
  CHECK(e1.find("a value computed from an `smp` literal reaches the duration input 'time'") != std::string::npos);
  CHECK(e1.find("5smp * 2 would mean 10 s") != std::string::npos);
  // Through a select, into timer `dur`, into a delay `max`, and through a subcircuit's param.
  CHECK_FALSE(errorOf("patch T { in x : audio\n out o : audio\n o = delay(x, x > 0 ? 5smp : 9smp, 1s) }").empty());
  CHECK_FALSE(errorOf("patch T { in x : audio\n out o : bool\n o = timer(x > 0, 100smp + 1) }").empty());
  CHECK(errorOf("patch T { in x : audio\n out o : audio\n o = delay(x, 1ms, 4800smp / 2) }").find("'max'") !=
        std::string::npos);
  CHECK_FALSE(errorOf("patch T { in x : audio\n out o : audio\n o = D(x, t: 5smp)\n}\n"
                      "patch D { in x : audio\n param t = 1ms\n out o : audio\n o = delay(x, t * 2, 1s) }")
                  .empty());
  // Still fine: smp wired directly (also through a subcircuit param), smp arithmetic that is not a duration, and a
  // comparison with an smp literal (a bool carries no duration).
  CHECK(errorOf("patch T { in x : audio\n out o : audio\n o = delay(x, 5smp, 64smp) }").empty());
  CHECK(errorOf("patch T { in x : audio\n out o : audio\n o = D(x, t: 5smp)\n}\n"
                "patch D { in x : audio\n param t = 1ms\n out o : audio\n o = delay(x, t, 1s) }")
            .empty());
  CHECK(errorOf("patch T { in x : audio\n out o : audio\n o = delay(x * (48000smp / sr), 1ms, 1s) }").empty());
  CHECK(errorOf("patch T { in x : audio\n out o : audio\n o = delay(x, x * 4 > 3smp ? 1ms : 2ms, 1s) }").empty());
}

TEST_CASE("optimise: equal literals of different units are not merged (M1 review L11)") {
  // Without a sample rate an smp `max` stays in samples for the scheduler; the plain 5 comes first in the order, so
  // a unit-blind CSE would make the delay read 5 s.
  Run r;
  OptimiseOptions o;  // sampleRate 0
  optimiseText(r, "patch C { in x : audio\n out y : audio\n out z : audio\n y = x * 5\n z = delay(x, 1ms, 5smp) }", o);
  checkInvariants(r.before, r.after);
  const NodeIndex d = driver(r.after, node(r.after, "z"), 0);
  REQUIRE(atomName(r.after, d) == "delay");
  const NodeIndex mx = driver(r.after, d, 2);
  REQUIRE(r.after.patch.node(mx).kind == NodeKind::Const);
  CHECK(r.after.patch.constant(mx).unit == Unit::smp);
  CHECK(r.after.patch.constant(mx).value() == 5.0);
  const NodeIndex m = driver(r.after, node(r.after, "y"), 0);
  REQUIRE(atomName(r.after, m) == "mul");
  const NodeIndex five = driver(r.after, m, 1);
  CHECK(five != mx);
  CHECK(r.after.patch.constant(five).unit == Unit::None);
  auto s = schedule(r.after);
  REQUIRE(s.ok());
  CHECK(s.value().state.globalBytes < 4096);  // a 5-frame ring, not 5 s
}

// ---- select sinking (M1 review M16) ---------------------------------------------------------------------------------

namespace {

std::vector<Rate> ratesOf(const TypedPatch& tp, std::string_view atom) {
  std::vector<Rate> v;
  for (NodeIndex n = 0; n < tp.patch.nodeCount(); ++n)
    if (atomName(tp, n) == atom) v.push_back(tp.outputType(n, 0).rate);
  return v;
}

uint32_t countAt(const TypedPatch& tp, std::string_view atom, Rate r) {
  uint32_t c = 0;
  for (Rate x : ratesOf(tp, atom)) c += x == r;
  return c;
}

}  // namespace

TEST_CASE("optimise: FmVoice's adsr has no audio-rate exp after optimise (select sinking)") {
  const std::string src = text_util::readFile(std::filesystem::path(PQ_TEST_DATA_DIR) / "golden" / "patches" / "fm_voice.pqp");
  Run off;
  OptimiseOptions o = sr48k();
  o.sinkSelects = false;
  optimiseText(off, src, o);
  CHECK(countAt(off.after, "exp", Rate::Audio) == 1);  // the per voice-sample exp the review measured

  Run r;
  optimiseText(r, src);
  checkInvariants(r.before, r.after);
  CHECK(r.stats.converged);
  // coef = 1 − exp(−1/max(time·sr, 1)), time = attacking ? a : gate ? d : r: five ops sunk below the two selects.
  CHECK(r.stats.sunk == 5);
  CHECK(countAt(r.after, "exp", Rate::Audio) == 0);
  CHECK(countAt(r.after, "div", Rate::Audio) == 0);
  CHECK(countAt(r.after, "max", Rate::Audio) == 0);
  // attack and release: one param-rate exp each; the decay (200 ms) folded to a literal coefficient.
  CHECK(countAt(r.after, "exp", Rate::Param) == 2);
  CHECK(ratesOf(r.after, "exp").size() == 2);
  // The per-sample work is the two selects (time's two plus target's two before; target's stay).
  CHECK(countAt(r.after, "select", Rate::Audio) == countAt(off.after, "select", Rate::Audio));
  CHECK(r.stats.nodesAfter < off.stats.nodesAfter + 12);
}

TEST_CASE("optimise: select sinking preserves values, NaN and infinities included") {
  Run r;
  optimiseText(r, R"(patch S {
  in x : audio
  param a = 5ms [0ms..10s]
  param q = 300ms [1ms..30s]
  out y : audio
  out z : audio
  out w : audio
  out v : audio
  t = x > 0.3 ? a : x < -0.3 ? 200ms : q
  y = 1 - exp(-1 / max(t * sr, 1))
  z = log(mux(x * 3 + 1.5, a, -1, q, 0)) * 2 - 1
  w = tanh(t * x)
  v = (x > 0 ? q : a) > 0.1 ? 1 : 0
})");
  checkInvariants(r.before, r.after);
  CHECK(r.stats.sunk >= 8);
  CHECK(countAt(r.after, "exp", Rate::Audio) == 0);
  CHECK(countAt(r.after, "log", Rate::Audio) == 0);
  CHECK(countAt(r.after, "tanh", Rate::Audio) == 1);  // its other input (x) is audio: nothing to gain
  checkEquivalent(r, 512);
}

TEST_CASE("optimise: select sinking runs bit-identically on the schedule") {
  // Arithmetic and fast_ kernels only: an exact transcendental of a global value is a 1-lane CRT call at audio rate
  // and an 8-lane SVML call at param rate (QUESTIONS Q94), as with any rate refinement.
  const char* src = R"(patch S {
  in x : audio
  param a = 0.25 [0..1]
  param q = 0.75 [0..1]
  out y : audio
  out z : audio
  t = x > 0.3 ? a : x < -0.3 ? 0.002 : q
  y = 1 - fast_exp(-1 / max(t * 480, 1))
  z = mux(x * 3 + 1.5, a, -1, q, 0) * 7 - fast_tanh(q)
})";
  Run on, off;
  OptimiseOptions o = sr48k();
  optimiseText(on, src, o);
  o.sinkSelects = false;
  optimiseText(off, src, o);
  CHECK(on.stats.sunk > 0);
  auto s1 = schedule(on.after), s0 = schedule(off.after);
  REQUIRE(s1.ok());
  REQUIRE(s0.ok());
  REQUIRE(sched_exec::supported(on.after));
  sched_exec::Executor e1(s1.value(), 64), e0(s0.value(), 64);
  for (uint64_t b = 0; b < 16; ++b) {
    const auto r1 = e1.block(b * 64), r0 = e0.block(b * 64);
    REQUIRE(r1.size() == r0.size());
    for (size_t k = 0; k < r1.size(); ++k)
      for (size_t f = 0; f < 64; ++f) CHECK(std::bit_cast<uint32_t>(r1[k][f]) == std::bit_cast<uint32_t>(r0[k][f]));
  }
}

TEST_CASE("optimise: select sinking leaves state, wide muxes and audio arms alone") {
  Run r;
  optimiseText(r, R"(patch S {
  in x : audio
  param a = 0.5 [0..1]
  param q = 0.25 [0..1]
  out y1 : audio
  out y2 : audio
  out y3 : audio
  out y4 : audio
  y1 = phasor(x > 0 ? a : q)
  y2 = exp(mux(x * 20, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16))
  y3 = exp(x > 0 ? a : x)
  y4 = exp(mux(x * 16, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15))
})");
  checkInvariants(r.before, r.after);
  // phasor is stateful: one instance, still reading the select (a phase per arm would change its state).
  CHECK(countAtom(r.after, "phasor") == 1);
  CHECK(atomName(r.after, driver(r.after, driver(r.after, node(r.after, "y1"), 0), 0)) == "select");
  // 17 arms > 16 copies: left as is. An audio arm: left as is. 16 literal arms: sunk and folded to 16 literals.
  CHECK(countAt(r.after, "exp", Rate::Audio) == 2);
  CHECK(atomName(r.after, driver(r.after, node(r.after, "y4"), 0)) == "mux");
  CHECK(r.stats.sunk == 1);
  checkEquivalent(r, 256);
}

// ---- driver -------------------------------------------------------------------------------------------------------

TEST_CASE("optimise: the fixpoint converges, honours the pass cap and is idempotent") {
  const char* src = R"(patch F {
  in x : audio
  param p = 0.5 [0..1]
  out o : audio
  a = x * (1 + 0 * 5)
  b = -(-(a / 1)) + -0
  c = pow(b, 1) * p + tanh(x * 1) - tanh(x)
  o = c * (2 - 1)
})";
  Run r;
  optimiseText(r, src);
  checkInvariants(r.before, r.after);
  CHECK(r.stats.converged);
  CHECK(r.stats.passes >= 1);
  CHECK(r.stats.passes <= 3);
  CHECK(r.stats.nodesAfter < r.stats.nodesBefore);
  checkEquivalent(r);

  Run capped;
  OptimiseOptions o = sr48k();
  o.maxPasses = 1;
  optimiseText(capped, src, o);
  checkInvariants(capped.before, capped.after);
  CHECK(capped.stats.passes == 1);
  checkEquivalent(capped);

  OptimiseStats again;
  auto twice = optimise(r.after, sr48k(), &again);
  REQUIRE(twice.ok());
  CHECK(twice.value().patch == r.after.patch);
  CHECK(again.folded + again.simplified + again.merged + again.removed + again.fused == 0);
}

TEST_CASE("optimise: feedback through z1 and a delay-free cycle are left for the scheduler") {
  Run r;
  optimiseText(r, R"(patch Fb {
  in x : audio
  out o : audio
  acc = z1(acc * 1 + x * 0.5)
  o = acc + -0
})");
  checkInvariants(r.before, r.after);
  CHECK(countAtom(r.after, "z1") == 1);
  CHECK(atomName(r.after, driver(r.after, node(r.after, "o"), 0)) == "z1");
  checkEquivalent(r, 128);
}

// ---- corpus and random patches: semantics preserved ---------------------------------------------------------------

TEST_CASE("optimise: every corpus patch optimises with its invariants and outputs intact") {
  text_util::DirLoader loader(text_util::casesDir());
  PatchLibrary lib;
  std::vector<ParseResult> files;
  for (const auto& path : text_util::pqpFiles(text_util::casesDir())) {
    ParseOptions po;
    po.imports = &loader;
    files.push_back(parseDetailed(text_util::readFile(path), path.string(), po));
    REQUIRE(files.back().ok());
  }
  for (const ParseResult& f : files) lib.add(f.file);
  int optimised = 0, compared = 0;
  for (const ParseResult& f : files)
    for (const Patch& p : f.file.patches) {
      CAPTURE(p.name());
      std::vector<RootInput> inputs;
      const Adjacency adj = p.adjacency();
      for (NodeIndex n = 0; n < p.nodeCount(); ++n)
        if (p.node(n).kind == NodeKind::Input) inputs.push_back(RootInput{adj.driver[p.flatInput(n, 0)] == kNone, {}});
      InferOptions io;
      io.library = &lib;
      io.inputs = inputs;
      InferResult inf = inferDetailed(p, io);
      REQUIRE(inf.ok());
      OptimiseStats st;
      auto res = optimise(inf.typed, sr48k(), &st);
      const std::string err = res.ok() ? std::string() : res.error().message;
      REQUIRE_MESSAGE(res.ok(), err);
      checkInvariants(inf.typed, res.value());
      CHECK(st.converged);
      // Growth only from duration conversions and select sinking (≤ 16 copies + 15 selects per sunk op; the copies
      // run per block, the per-sample op count drops).
      CHECK(st.nodesAfter <= st.nodesBefore + st.durations * 2 + st.sunk * 31);
      ++optimised;
      if (ref_eval::Evaluator(inf.typed).supported()) {
        const double d = ref_eval::maxRelDiff(inf.typed, res.value(), 256);
        CHECK_MESSAGE(d <= 1e-5, "max relative difference " << d);
        ++compared;
      }
    }
  CHECK(optimised >= 15);
  CHECK(compared >= 5);
  MESSAGE("corpus: " << optimised << " patches optimised, " << compared << " compared numerically");
}

namespace {

// A random patch of `count` bindings over x (audio), p, q (params), a phasor, and the earlier bindings. The
// templates are biased towards the optimiser's rules (identities, literal folding, duplicates); values stay bounded
// (tanh, sin, clip) so a relative tolerance is meaningful.
std::string randomPatch(std::mt19937& rng, int count) {
  static const char* templates[] = {
      "A + B", "A - B", "A * B", "tanh(A)", "sin(A)", "A * 1", "1 * A", "A + -0", "A + 0", "A - 0", "A * 0",
      "A / 2", "A / 4", "A / -1", "A * -1", "pow(A, 2)", "pow(A, 1)", "-(-A)", "abs(-A)", "abs(abs(A))",
      "min(A, A)", "max(A, B)", "clip(A, -1, 1)", "(A > B ? A : B)", "(true ? A : B)", "(false ? A : B)",
      "mux(1, A, B)", "lerp(A, B, 0.25)", "muladd(A, B, 0.5)", "A * B + A", "(2 * 3 - 5) * A", "A * (ph * 0)",
      "z1(A)", "tanh(A * B + sin(A))", "A * B + B * A", "tanh(A) + tanh(A)", "(1 < 2 ? A : B) * 0.5",
      "A * (q * 2)", "gate_to_real(A > 0) + 0", "A / 3", "A + (0.25 + 0.5)"};
  std::vector<std::string> names = {"x", "p", "q", "ph"};
  std::string s = "patch R {\n  in x : audio\n  param p = 0.37 [0..1]\n  param q = -0.6 [-1..1]\n  out y : audio\n  out z : audio\n"
                  "  ph = phasor(110Hz)\n";
  auto pick = [&] { return names[std::uniform_int_distribution<size_t>(0, names.size() - 1)(rng)]; };
  for (int i = 0; i < count; ++i) {
    std::string t = templates[std::uniform_int_distribution<size_t>(0, std::size(templates) - 1)(rng)];
    const std::string a = pick(), b = pick();
    std::string e;
    for (char c : t) {
      if (c == 'A') e += a;
      else if (c == 'B') e += b;
      else e += c;
    }
    const std::string n = "n" + std::to_string(i);
    s += "  " + n + " = tanh(" + e + ")\n";
    names.push_back(n);
  }
  s += "  y = " + names.back() + "\n  z = " + names[names.size() / 2] + " + " + names[names.size() - 2] + "\n}\n";
  return s;
}

}  // namespace

TEST_CASE("optimise: random patches evaluate the same before and after" * doctest::test_suite("slow")) {
  std::mt19937 rng(1234);
  int changed = 0;
  for (int k = 0; k < 150; ++k) {
    const std::string src = randomPatch(rng, 6 + k % 20);
    CAPTURE(src);
    Run r;
    optimiseText(r, src);
    checkInvariants(r.before, r.after);
    CHECK(r.stats.converged);
    ref_eval::Evaluator probe(r.before);
    REQUIRE(probe.supported());
    const double d = ref_eval::maxRelDiff(r.before, r.after, 96);
    CHECK_MESSAGE(d <= 1e-6, "max relative difference " << d << "\nafter:\n" << print(r.after.patch));
    changed += r.stats.nodesAfter < r.stats.nodesBefore;
  }
  CHECK(changed > 100);
}
