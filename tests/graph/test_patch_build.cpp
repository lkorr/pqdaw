#include <doctest/doctest.h>

#include "doctest_string_view.h"

#include <algorithm>
#include <string>
#include <vector>

#include "fm_voice.h"
#include "pq/graph/patch.h"

using namespace pq::graph;

namespace {
std::string errorsOf(const Patch& p) {
  std::string s;
  for (const pq::Error& e : p.validateAll()) s += e.message + "\n";
  return s;
}
}  // namespace

TEST_CASE("patch: the FmVoice example builds and validates") {
  fm_voice::Nodes n{};
  const Patch p = fm_voice::build(false, &n);
  CHECK_MESSAGE(p.validateAll().empty(), errorsOf(p));
  CHECK(p.validate().ok());
  CHECK(p.nodeCount() == 23);
  CHECK(p.wireCount() == 22);

  // Declarations and names.
  CHECK(p.node(n.ratio).kind == NodeKind::Param);
  CHECK(p.nodeName(n.ratio) == "ratio");
  CHECK(p.paramDecl(n.attack).defaultValue.value() == doctest::Approx(0.005));
  CHECK(p.paramDecl(n.attack).rangeMax.value() == doctest::Approx(10.0));
  CHECK(p.paramDecl(n.ratio).attrs[0].name == "label");
  CHECK(p.outputType(n.ratio, 0) == portType(Kind::Real, Rate::Param, Poly::Global, kWidthMono));
  CHECK(p.inputType(n.out, 0) == portType(Kind::Real, Rate::Audio, Poly::Voice, kWidthMono));
  CHECK(p.findByName("car") == n.car);
  CHECK(p.findByName("modulator") == n.expMod);
  CHECK_FALSE(p.findByName("nope").has_value());
  CHECK(p.layout(n.env).placed);
  CHECK(p.layout(n.env).x == 200.0f);
  CHECK_FALSE(p.layout(n.m1).placed);

  // Port naming and lookup come from the atom registry / subcircuit signature.
  CHECK(p.inputName(n.ph1, 0) == "hz");
  CHECK(p.inputName(n.ph1, 1) == "reset");
  CHECK(p.inputOptional(n.ph1, 1));
  CHECK_FALSE(p.inputOptional(n.ph1, 0));
  CHECK(p.findInput(n.env, "r") == 4u);
  CHECK(p.findOutput(n.env, "out") == 0u);
  CHECK_FALSE(p.findInput(n.env, "q").has_value());
  CHECK(p.inputName(n.out, 0) == "in");
  CHECK(p.outputName(n.ratio, 0) == "out");
  CHECK(p.inputType(n.env, 0).kind == Kind::Bool);
  CHECK(p.subcircuit(n.env).patch == "adsr");

  // Flat port indices: contiguous per node, inputs then outputs.
  uint32_t expect = 0;
  for (NodeIndex i = 0; i < p.nodeCount(); ++i) {
    CHECK(p.node(i).firstPort == expect);
    if (p.node(i).numOutputs) CHECK(p.flatOutput(i, 0) == expect + p.node(i).numInputs);
    expect += p.node(i).numInputs + p.node(i).numOutputs;
  }
  CHECK(expect == p.portCount());

  CHECK(p.describe(n.mod) == "'mod' (atom sine, #" + std::to_string(n.mod) + ")");
  CHECK(p.describe(n.env).find("subcircuit adsr") != std::string::npos);
}

TEST_CASE("patch: NodeIds are stable across erase while dense indices compact") {
  Patch p("P");
  const NodeIndex a = p.addConst(Constant::real(1));
  const NodeIndex b = p.addConst(Constant::real(2));
  const NodeIndex c = p.addAtom(atomId("add"));
  const NodeIndex o = p.addOutput("o", portType(Kind::Real));
  p.connect(a, c, 0);
  p.connect(b, c, 1);
  p.connect(c, o, 0);
  const NodeId idB = p.node(b).id, idC = p.node(c).id, idO = p.node(o).id, idA = p.node(a).id;
  CHECK(idA.valid());
  CHECK(idA < idB);

  const NodeIndex eraseList[] = {a, a, 999};  // duplicates and out-of-range indices are ignored
  p.eraseNodes(eraseList);
  CHECK(p.nodeCount() == 3);
  CHECK_FALSE(p.indexOf(idA).has_value());
  REQUIRE(p.indexOf(idB).has_value());
  CHECK(*p.indexOf(idB) == 0);
  CHECK(*p.indexOf(idC) == 1);
  CHECK(*p.indexOf(idO) == 2);
  CHECK(p.constant(0).literal == 2.0);  // side table compacted with the node
  CHECK(p.wireCount() == 2);            // the wire from the erased node is gone
  CHECK(p.wire(0).from.node == 0);
  CHECK(p.wire(0).to.node == 1);
  CHECK(p.wire(0).to.port == 1);
  CHECK(p.portCount() == 1 + 3 + 1);

  // Now c's input a is unconnected: a dangling port.
  CHECK_FALSE(p.validate().ok());
  // New nodes never reuse an erased id.
  const NodeIndex d = p.addConst(Constant::real(3));
  CHECK(p.node(d).id > idO);
  p.connect(d, *p.indexOf(idC), 0);
  CHECK_MESSAGE(p.validate().ok(), errorsOf(p));
}

TEST_CASE("patch: adjacency (CSR) and disconnect") {
  fm_voice::Nodes n{};
  Patch p = fm_voice::build(false, &n);
  const Adjacency adj = p.adjacency();
  CHECK(adj.outBegin.size() == p.nodeCount() + 1);
  CHECK(adj.outWires.size() == p.wireCount());
  // mod feeds m2 and the modulator export; env feeds outMul and the envelope export.
  CHECK(adj.outgoing(n.mod).size() == 2);
  CHECK(adj.outgoing(n.env).size() == 2);
  CHECK(adj.outgoing(n.out).empty());
  for (NodeIndex i = 0; i < p.nodeCount(); ++i)
    for (WireIndex w : adj.outgoing(i)) CHECK(p.wire(w).from.node == i);
  // Every input of every node is driven by exactly the wire that targets it.
  for (WireIndex w = 0; w < p.wireCount(); ++w) CHECK(adj.driver[p.flatInput(p.wire(w).to.node, p.wire(w).to.port)] == w);
  CHECK(adj.driver[p.flatInput(n.ph1, 1)] == kNone);  // phasor reset left unconnected

  // Disconnect swaps the last wire into the hole.
  const Wire last = p.wire(p.wireCount() - 1);
  p.setWireVia(p.wireCount() - 1, {{1, 2}, {3, 4}});
  p.disconnect(0);
  CHECK(p.wireCount() == 21);
  CHECK(p.wire(0) == last);
  CHECK(p.wireVia(0).size() == 2);
  CHECK_FALSE(p.validate().ok());  // m1.a (the first wire's target) is now dangling
}

TEST_CASE("patch: variadic atoms, subcircuits, solve blocks") {
  Patch p("Variadic");
  const NodeIndex mux = p.addAtom(atomId("mux"), 4);
  CHECK(p.node(mux).numInputs == 5);
  CHECK(p.node(mux).numOutputs == 1);
  CHECK(p.inputName(mux, 0) == "index");
  CHECK(p.inputName(mux, 4) == "in");
  const NodeIndex demux = p.addAtom(atomId("demux"));  // minimum repeat
  CHECK(p.node(demux).numInputs == 2);
  CHECK(p.node(demux).numOutputs == 2);
  const NodeIndex pack = p.addAtom(atomId("pack"), 3);
  CHECK(p.node(pack).numInputs == 3);

  // A nonlinear one-pole solved per sample: y == x - g·tanh(y) style, with 2 unknowns and 2 equations.
  const SolveIndex s = p.addSolve(4);
  const NodeIndex u1 = p.addUnknown("y1", s);
  const NodeIndex u2 = p.addUnknown("y2", s);
  const NodeIndex e1 = p.addEquation(s);
  const NodeIndex e2 = p.addEquation(s);
  CHECK(p.node(u1).payload == s);
  CHECK(p.inputName(u1, 0) == "init");
  CHECK(p.inputOptional(u1, 0));
  CHECK(p.inputName(e1, 1) == "rhs");
  CHECK(p.node(e2).numOutputs == 0);
  CHECK(p.solves()[0].iterations == 4);
  (void)u2;
}

TEST_CASE("patch: a solve block for a tanh-feedback one-pole validates") {
  // patch TanhPole { in x : audio; param g = 0.5; out y : audio
  //   solve(iterations: 3) { unknown v ; v == x - g * tanh(v) }
  //   y = v }
  Patch p("TanhPole");
  const NodeIndex x = p.addInput("x", portType(Kind::Real, Rate::Audio));
  ParamDecl gd;
  gd.defaultValue = Constant::real(0.5);
  const NodeIndex g = p.addParam("g", gd);
  const NodeIndex y = p.addOutput("y", portType(Kind::Real, Rate::Audio));
  const SolveIndex s = p.addSolve(3);
  const NodeIndex v = p.addUnknown("v", s);
  const NodeIndex eq = p.addEquation(s);
  const NodeIndex th = p.addAtom(atomId("tanh"));
  const NodeIndex m = p.addAtom(atomId("mul"));
  const NodeIndex sub = p.addAtom(atomId("sub"));
  p.connect(v, th, 0);
  p.connect(g, m, 0);
  p.connect(th, m, 1);
  p.connect(x, sub, 0);
  p.connect(m, sub, 1);
  p.connect(v, eq, 0);
  p.connect(sub, eq, 1);
  p.connect(v, y, 0);
  CHECK_MESSAGE(p.validate().ok(), errorsOf(p));
}

TEST_CASE("patch: indexOf finds every surviving node after repeated erases, and no erased one") {
  Patch p("P");
  std::vector<NodeId> ids;
  for (int i = 0; i < 200; ++i) ids.push_back(p.node(p.addConst(Constant::real(i))).id);
  std::vector<bool> alive(ids.size(), true);
  for (int round = 0; round < 5; ++round) {
    std::vector<NodeIndex> erase;
    for (size_t k = 0; k < ids.size(); ++k)
      if (alive[k] && (k * 7 + static_cast<size_t>(round) * 3) % 11 == 0) {
        erase.push_back(*p.indexOf(ids[k]));
        alive[k] = false;
      }
    p.eraseNodes(erase);
    for (size_t k = 0; k < ids.size(); ++k) {
      CAPTURE(k);
      const auto idx = p.indexOf(ids[k]);
      CHECK(idx.has_value() == alive[k]);
      if (idx) CHECK(p.constant(*idx).literal == static_cast<double>(k));  // the side table followed its node
    }
  }
  CHECK_FALSE(p.indexOf(NodeId{}).has_value());
  CHECK_FALSE(p.indexOf(NodeId{100000}).has_value());
}
