// T4.15: structural constants of a patch as written (pq/graph/structure.h) — constant evaluation through const inputs,
// arguments and pure atoms, the dead arms of constant selects / muxes, what is inactive (only reachable through them)
// and what stays (shared nodes, unconnected work in progress, params), call arguments, and agreement with the
// optimiser: every node the compiled program keeps is active, every inactive atom is gone from it.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

#include "pq/graph/infer.h"
#include "pq/graph/optimise.h"
#include "pq/graph/parse.h"
#include "pq/graph/structure.h"

using namespace pq::graph;

namespace {

ParseResult parseOk(std::string_view src) {
  ParseResult r = parseDetailed(src, "test.pqp");
  REQUIRE(r.ok());
  return r;
}

NodeIndex named(const Patch& p, std::string_view n) {
  const auto v = p.findByName(n);
  REQUIRE(v.has_value());
  return *v;
}

bool inactive(const StructuralInfo& s, const Patch& p, std::string_view n) { return s.nodeInactive[named(p, n)] != 0; }

std::string satShapeText() {
  std::ifstream in(std::filesystem::path(PQ_TEST_DATA_DIR) / ".." / "stdlib" / "devices" / "saturator.pqp", std::ios::binary);
  REQUIRE(in.good());
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string all = ss.str();
  const size_t at = all.find("patch sat_shape");
  REQUIRE(at != std::string::npos);
  return all.substr(at);
}

}  // namespace

TEST_CASE("structure: a constant selector switches its other arms off; shared and unread nodes stay") {
  const ParseResult r = parseOk(
      "patch P {\n"
      "  in    x    : audio\n"
      "  in    mode : real const = 1\n"
      "  param g = 1 [0..2]\n"
      "  out   y : audio\n"
      "  a  = tanh(x * g)\n"
      "  b  = sin(x) * 0.5\n"
      "  c  = abs(x)\n"
      "  sh = x * 2\n"
      "  m  = mux(mode, a, b + sh, c)\n"
      "  y  = m + sh\n"
      "  wip = sqrt(x)\n"  // read by nothing yet: active (a sink)
      "}\n");
  const Patch& p = r.file.patches[0];
  const StructuralInfo s = analyseStructure(p);
  CHECK(inactive(s, p, "a"));       // tanh(x * g): the mul under it too
  CHECK_FALSE(inactive(s, p, "b"));  // mode 1 picks b + sh
  CHECK(inactive(s, p, "c"));
  CHECK_FALSE(inactive(s, p, "sh"));  // also read by y
  CHECK_FALSE(inactive(s, p, "wip"));
  CHECK(inactive(s, p, "g"));  // a param read only inside a switched-off arm goes with it
}

TEST_CASE("structure: arguments override defaults; a param is never a structural constant") {
  const ParseResult r = parseOk(
      "patch P {\n"
      "  in    x    : audio\n"
      "  in    on   : real const = 0\n"
      "  param k = 1 [0..1]\n"
      "  out   y : audio\n"
      "  wet = x * 3\n"
      "  y   = on >= 0.5 ? wet : x\n"
      "  z   = k > 0.5 ? x * 4 : x\n"
      "  export e = z\n"
      "}\n");
  const Patch& p = r.file.patches[0];
  const StructuralInfo off = analyseStructure(p);
  CHECK(inactive(off, p, "wet"));
  const ConstArg on[] = {{"on", 1.0}};
  const StructuralInfo onInfo = analyseStructure(p, on);
  CHECK_FALSE(inactive(onInfo, p, "wet"));
  CHECK(onInfo.inactiveNodes == 0);
  // The selector of z is a param: nothing switched off behind it.
  for (WireIndex w = 0; w < p.wireCount(); ++w)
    if (p.wire(w).to.node == named(p, "z")) CHECK(onInfo.deadWire[w] == 0);
  double v = 0;
  CHECK(off.constantAt(p, named(p, "on"), 0, v));
  CHECK(v == 0.0);
  CHECK_FALSE(off.constantAt(p, named(p, "k"), 0, v));
  // Unknown or non-const arguments are ignored.
  const ConstArg junk[] = {{"x", 5.0}, {"nope", 1.0}};
  CHECK(analyseStructure(p, junk).inactiveNodes == off.inactiveNodes);
}

TEST_CASE("structure: constants through arithmetic, comparisons, logic and nested selects") {
  const ParseResult r = parseOk(
      "patch P {\n"
      "  in    x   : audio\n"
      "  in    alg : real const = 3\n"
      "  in    on  : real const = 1\n"
      "  out   y : audio\n"
      "  i    = alg - 1\n"
      "  r    = mux(i, 1, 0, 1, 0) >= 0.5 && on >= 0.5\n"   // alg 3 → index 2 → 1 → true
      "  q    = mux(i, 0, 0, 0, 1) >= 0.5\n"                // → false
      "  p1   = x * 5\n"
      "  p2   = x * 7\n"
      "  y    = r ? (q ? p1 : p2) : x\n"
      "}\n");
  const Patch& p = r.file.patches[0];
  const StructuralInfo s = analyseStructure(p);
  double v = 0;
  REQUIRE(s.constantAt(p, named(p, "r"), 0, v));
  CHECK(v == 1.0);
  REQUIRE(s.constantAt(p, named(p, "q"), 0, v));
  CHECK(v == 0.0);
  CHECK(inactive(s, p, "p1"));
  CHECK_FALSE(inactive(s, p, "p2"));
  const ConstArg four[] = {{"alg", 4.0}};
  const StructuralInfo s4 = analyseStructure(p, four);  // index 3: r false → x, both products off
  CHECK(inactive(s4, p, "p1"));
  CHECK(inactive(s4, p, "p2"));
}

TEST_CASE("structure: call arguments carry constants into a subcircuit; unconnected pins are left out") {
  const ParseResult r = parseOk(
      "patch Outer {\n"
      "  in    x    : audio\n"
      "  in    kind : real const = 2\n"
      "  out   y : audio\n"
      "  y = Inner(x, mode: kind + 1)\n"
      "  z = Inner(x)\n"
      "  export e = z\n"
      "}\n"
      "patch Inner {\n"
      "  in    v    : audio\n"
      "  in    mode : real const = 0\n"
      "  in    gain : real const = 1\n"
      "  out   y : audio\n"
      "  y = mux(mode, v, v * 2, v * 3, v * 4) * gain\n"
      "}\n");
  const Patch& outer = r.file.patches[0];
  const StructuralInfo s = analyseStructure(outer);
  const auto calls = [&] {
    std::vector<NodeIndex> c;
    for (NodeIndex v = 0; v < outer.nodeCount(); ++v)
      if (outer.node(v).kind == NodeKind::Subcircuit) c.push_back(v);
    return c;
  }();
  REQUIRE(calls.size() == 2);
  const NodeIndex plain = named(outer, "z");
  const NodeIndex first = calls[0] == plain ? calls[1] : calls[0];
  const std::vector<ConstArg> a = callArguments(outer, s, first);
  REQUIRE(a.size() == 1);
  CHECK(a[0].name == "mode");
  CHECK(a[0].value == 3.0);
  CHECK(callArguments(outer, s, plain).empty());
  const Patch& inner = r.file.patches[1];
  const StructuralInfo si = analyseStructure(inner, a);
  CHECK(si.inactiveNodes > 0);
  CHECK(si.inactiveWires > 0);
}

TEST_CASE("structure: Saturator's curve: only the chosen curve is active, as the optimiser compiles it") {
  const std::string text = satShapeText();
  const ParseResult r = parseOk(text);
  const Patch& p = r.file.patches[0];
  REQUIRE(p.name() == "sat_shape");
  const StructuralInfo s = analyseStructure(p);  // type 0 (Analog Clip, the default since M4 review L11), softClip 0
  CHECK_FALSE(inactive(s, p, "analog"));
  CHECK_FALSE(inactive(s, p, "ca"));
  for (const char* off : {"softsine", "bass", "medium", "hard", "fold", "digital", "ws", "drawn", "av", "v2", "gate"})
    CHECK_MESSAGE(inactive(s, p, off), off);
  // Each curve in turn.
  const char* curves[] = {"analog", "softsine", "bass", "medium", "hard", "fold", "digital", "ws", "drawn"};
  for (int t = 0; t < 9; ++t) {
    const ConstArg a[] = {{"type", static_cast<double>(t)}};
    const StructuralInfo st = analyseStructure(p, a);
    for (int k = 0; k < 9; ++k) CHECK_MESSAGE((st.nodeInactive[named(p, curves[k])] != 0) == (k != t), curves[k] << " at type " << t);
  }

  // The optimiser agrees: every atom of the compiled program comes from an active node, and no inactive atom survives.
  PatchLibrary lib;
  lib.add(r.file);
  InferOptions io;
  io.library = &lib;
  RootInput roots[3];  // v from the engine; type and softClip read their defaults (literals: folded)
  roots[1].connected = false;
  roots[2].connected = false;
  io.inputs = roots;
  InferResult inf = inferDetailed(p, io);
  REQUIRE(inf.ok());
  OptimiseOptions oo;
  oo.sampleRate = 48000.0;
  auto opt = optimise(std::move(inf.typed), oo);
  REQUIRE(opt.ok());
  std::set<NodeIndex> kept;
  for (NodeIndex v = 0; v < opt->patch.nodeCount(); ++v) {
    const NodeOrigin& o = opt->origin[v];
    if (o.instance == 0 && o.what == NodeOrigin::What::Node && o.node != kNone) kept.insert(o.node);
  }
  // Params stay in the program either way (its interface: knobs, never folded); literals are merged by value, so a
  // kept literal may carry the origin of an equal one in an inactive curve (the `1` of `gain(…, start: 1)` and
  // the digital clip's 1, T4.x-fix-dsp): the check is on the atoms.
  for (const NodeIndex v : kept)
    if (p.node(v).kind != NodeKind::Param && p.node(v).kind != NodeKind::Const) CHECK_MESSAGE(s.nodeInactive[v] == 0, p.describe(v));
  for (NodeIndex v = 0; v < p.nodeCount(); ++v)
    if (s.nodeInactive[v] && p.node(v).kind == NodeKind::Atom) CHECK_MESSAGE(kept.count(v) == 0, p.describe(v));
}

TEST_CASE("structure: bool eq / ne and stateful atoms are never folded") {
  const ParseResult r = parseOk(
      "patch P {\n"
      "  in    x : audio\n"
      "  out   y : audio\n"
      "  t = (true == true) ? x * 2 : x * 3\n"
      "  u = z1(1) > 0.5 ? x * 4 : x * 5\n"
      "  y = t + u\n"
      "}\n");
  const StructuralInfo s = analyseStructure(r.file.patches[0]);
  CHECK(s.inactiveNodes == 0);
}
