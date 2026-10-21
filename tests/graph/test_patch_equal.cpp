#include <doctest/doctest.h>

#include "doctest_string_view.h"

#include <algorithm>
#include <functional>
#include <numeric>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "fm_voice.h"
#include "pq/graph/patch.h"

using namespace pq::graph;

namespace {
// Replaces every wire matching `match` by `f(wire)`.
void rewire(Patch& p, const std::function<bool(const Wire&)>& match, const std::function<Wire(Wire)>& f) {
  std::vector<Wire> added;
  for (WireIndex w = p.wireCount(); w-- > 0;) {  // descending: disconnect moves an already-visited wire into w
    if (!match(p.wire(w))) continue;
    added.push_back(f(p.wire(w)));
    p.disconnect(w);
  }
  for (const Wire& w : added) p.connect(w.from, w.to);
}
}  // namespace

TEST_CASE("equality: reflexive, and copies are equal") {
  const Patch p = fm_voice::build();
  CHECK(p == p);
  const Patch copy = p;
  CHECK(copy == p);
  CHECK(Patch() == Patch());
  CHECK(Patch("A") != Patch("B"));
}

TEST_CASE("equality ignores NodeIds, node order and wire order") {
  const Patch a = fm_voice::build(false);
  const Patch b = fm_voice::build(true);  // different insertion order, wire order, erased junk → different ids
  REQUIRE(b.validate().ok());
  bool idsDiffer = false;
  for (NodeIndex i = 0; i < a.nodeCount(); ++i) idsDiffer |= a.node(i).id != b.node(i).id;
  CHECK(idsDiffer);
  CHECK(a.node(0).kind != b.node(0).kind);  // dense order differs too
  CHECK(a == b);
  CHECK(b == a);
}

TEST_CASE("equality: every structural difference is detected") {
  fm_voice::Nodes n{};
  const Patch base = fm_voice::build(false, &n);
  // Each mutation must make the patch unequal to the base, and also unequal to the scrambled build.
  const Patch scrambled = fm_voice::build(true);
  using Mutation = std::function<void(Patch&)>;
  const std::vector<std::pair<const char*, Mutation>> mutations = {
      {"patch name", [](Patch& p) { p.setName("FmVoice2"); }},
      {"param default", [&](Patch& p) { p.paramDecl(n.ratio).defaultValue = Constant::real(2.5); }},
      {"param default unit (same normalised value)",
       [&](Patch& p) { p.paramDecl(n.attack).defaultValue = Constant::real(0.005, Unit::s); }},
      {"param range", [&](Patch& p) { p.paramDecl(n.index).rangeMax = Constant::real(21); }},
      {"param attr arg", [&](Patch& p) { p.paramDecl(n.ratio).attrs[0].args[0].value.text = "Rat"; }},
      {"param attr added", [&](Patch& p) { p.paramDecl(n.ratio).attrs.push_back(Attr{"hidden", {}}); }},
      {"const literal", [&](Patch& p) { p.constant(n.s) = Constant::real(0.71); }},
      {"const unit", [&](Patch& p) { p.constant(n.d) = Constant::real(0.2, Unit::s); }},
      {"node name", [&](Patch& p) { p.setNodeName(n.car, "carrier"); }},
      {"name added to an anonymous node", [&](Patch& p) { p.setNodeName(n.m1, "m"); }},
      {"name bound to an output", [&](Patch& p) { p.setNodeName(n.env, "env", 0); }},
      {"layout position", [&](Patch& p) {
         NodeLayout l = p.layout(n.mod);
         l.x += 1;
         p.setLayout(n.mod, l);
       }},
      {"layout flip", [&](Patch& p) {
         NodeLayout l = p.layout(n.mod);
         l.flip = true;
         p.setLayout(n.mod, l);
       }},
      {"wire via points", [](Patch& p) { p.setWireVia(3, {{1, 1}}); }},
      {"comment", [](Patch& p) { p.addComment({"note", {0, 0}}); }},
      {"swapped operands of a commutative atom (structural, not algebraic)", [&](Patch& p) {
         rewire(p, [&](const Wire& w) { return w.to.node == n.m1; },
                [](Wire w) {
                  w.to.port = static_cast<uint16_t>(1 - w.to.port);
                  return w;
                });
       }},
      {"wire retargeted", [&](Patch& p) {
         // voice.hz node shared instead of a separate one
         rewire(p, [&](const Wire& w) { return w.from.node == n.vh2; },
                [&](Wire w) {
                  w.from.node = n.vh1;
                  return w;
                });
       }},
      {"extra node", [](Patch& p) { p.addConst(Constant::real(1)); }},
      {"extra wire", [&](Patch& p) { p.connect(n.ratio, n.ph1, 1); }},
      {"atom changed", [&](Patch& p) {
         // Replace `sine` of the modulator by `fast_sin` with identical wiring (same port shapes).
         const NodeIndex f = p.addAtom(atomId("fast_sin"));
         p.setNodeName(f, "mod");
         p.setLayout(f, p.layout(n.mod));
         rewire(p, [&](const Wire& w) { return w.from.node == n.mod || w.to.node == n.mod; },
                [&](Wire w) {
                  if (w.from.node == n.mod) w.from.node = f;
                  if (w.to.node == n.mod) w.to.node = f;
                  return w;
                });
         const NodeIndex old[] = {n.mod};
         p.eraseNodes(old);
       }},
      {"subcircuit callee", [&](Patch& p) {
         SubcircuitRef r = fm_voice::adsrRef();
         r.patch = "adsr2";
         const NodeIndex e = p.addSubcircuit(r);
         (void)e;
         const NodeIndex old[] = {n.env};
         p.eraseNodes(old);
       }},
      {"declaration order", [&](Patch& p) {
         // Re-add `ratio` after the other params: same set of params, different declared order.
         ParamDecl d = p.paramDecl(n.ratio);
         const NodeIndex r = p.addParam("ratio2", d);
         rewire(p, [&](const Wire& w) { return w.from.node == n.ratio; },
                [&](Wire w) {
                  w.from.node = r;
                  return w;
                });
         const NodeIndex old[] = {n.ratio};
         p.eraseNodes(old);
         p.setNodeName(*p.findByName("ratio2"), "ratio");
       }},
      {"solve block", [](Patch& p) { p.addSolve(2); }},
  };
  for (const auto& [what, mutate] : mutations) {
    CAPTURE(what);
    Patch m = base;
    mutate(m);
    CHECK_FALSE(m == base);
    CHECK_FALSE(base == m);
    CHECK_FALSE(m == scrambled);
  }
}

TEST_CASE("equality distinguishes how identical anonymous subtrees are wired") {
  // o1 = a - b ; o2 = b - a, with anonymous `sub` nodes: swapping which sub feeds which output must be unequal.
  auto build = [](bool swapped) {
    Patch p("P");
    const NodeIndex a = p.addInput("a", portType(Kind::Real));
    const NodeIndex b = p.addInput("b", portType(Kind::Real));
    const NodeIndex o1 = p.addOutput("o1", portType(Kind::Real));
    const NodeIndex o2 = p.addOutput("o2", portType(Kind::Real));
    const NodeIndex s1 = p.addAtom(atomId("sub"));
    const NodeIndex s2 = p.addAtom(atomId("sub"));
    p.connect(a, s1, 0);
    p.connect(b, s1, 1);
    p.connect(b, s2, 0);
    p.connect(a, s2, 1);
    p.connect(swapped ? s2 : s1, o1, 0);
    p.connect(swapped ? s1 : s2, o2, 0);
    return p;
  };
  CHECK(build(false) == build(false));
  CHECK_FALSE(build(false) == build(true));
}

namespace {

// A random valid DAG: consts and real math atoms, each input fed by an earlier node, three outputs, some names.
// Every node is structurally distinct (unique const values, no two atoms with the same op and inputs), so any
// rewire produces a non-isomorphic patch.
struct RandomSpec {
  struct N {
    int kind;  // 0 const, else index into kRandomAtoms
    double value;
    std::vector<uint32_t> inputs;  // source spec indices
    std::string name;
  };
  std::vector<N> nodes;
  uint32_t outputs[3];
};

constexpr const char* kRandomAtoms[] = {"", "add", "mul", "sub", "tanh", "abs", "min"};

RandomSpec randomSpec(std::mt19937& rng, uint32_t count) {
  RandomSpec s;
  std::set<std::pair<int, std::vector<uint32_t>>> seen;
  for (uint32_t i = 0; i < count; ++i) {
    RandomSpec::N n;
    n.value = i + 0.5;
    for (;;) {
      n.kind = i < 3 ? 0 : static_cast<int>(rng() % 7);
      n.inputs.clear();
      if (n.kind == 0) break;
      const auto arity = static_cast<uint32_t>(atomSignature(atomId(kRandomAtoms[n.kind])).inputs.size());
      for (uint32_t k = 0; k < arity; ++k) n.inputs.push_back(static_cast<uint32_t>(rng() % i));
      if (seen.insert({n.kind, n.inputs}).second) break;
    }
    if (rng() % 4 == 0) n.name = "n" + std::to_string(i);
    s.nodes.push_back(std::move(n));
  }
  for (uint32_t& o : s.outputs) o = count - 1 - static_cast<uint32_t>(rng() % 5);
  return s;
}

Patch buildSpec(const RandomSpec& s, std::mt19937& rng, bool permute) {
  const auto count = static_cast<uint32_t>(s.nodes.size());
  std::vector<uint32_t> order(count);
  std::iota(order.begin(), order.end(), 0u);
  if (permute) std::shuffle(order.begin(), order.end(), rng);
  Patch p("Random");
  NodeIndex outs[3];
  for (int k = 0; k < 3; ++k) outs[k] = p.addOutput("o" + std::to_string(k), portType(Kind::Real));
  std::vector<NodeIndex> index(count);
  for (uint32_t i : order) {
    const RandomSpec::N& n = s.nodes[i];
    index[i] = n.kind == 0 ? p.addConst(Constant::real(n.value)) : p.addAtom(atomId(kRandomAtoms[n.kind]));
    if (!n.name.empty()) p.setNodeName(index[i], n.name);
  }
  std::vector<Wire> wires;
  for (uint32_t i = 0; i < count; ++i)
    for (uint32_t k = 0; k < s.nodes[i].inputs.size(); ++k)
      wires.push_back(Wire{PortRef{index[s.nodes[i].inputs[k]], 0}, PortRef{index[i], static_cast<uint16_t>(k)}});
  for (int k = 0; k < 3; ++k) wires.push_back(Wire{PortRef{index[s.outputs[k]], 0}, PortRef{outs[k], 0}});
  if (permute) std::shuffle(wires.begin(), wires.end(), rng);
  for (const Wire& w : wires) p.connect(w.from, w.to);
  return p;
}

}  // namespace

TEST_CASE("equality: random graphs equal their permuted rebuilds and differ after any single rewire") {
  std::mt19937 rng(1234);
  int rewires = 0;
  for (int trial = 0; trial < 300; ++trial) {
    CAPTURE(trial);
    const RandomSpec spec = randomSpec(rng, 8 + static_cast<uint32_t>(rng() % 40));
    const Patch a = buildSpec(spec, rng, false);
    const Patch b = buildSpec(spec, rng, true);
    REQUIRE(a.validate().ok());
    REQUIRE(b.validate().ok());
    CHECK(a == b);
    CHECK(b == a);

    // Move one atom input of a node i ≥ 4 to a different earlier source (the DAG stays valid).
    std::vector<uint32_t> atoms;
    for (uint32_t i = 4; i < spec.nodes.size(); ++i)
      if (!spec.nodes[i].inputs.empty()) atoms.push_back(i);
    if (atoms.empty()) continue;
    RandomSpec changed = spec;
    const uint32_t i = atoms[rng() % atoms.size()];
    uint32_t& src = changed.nodes[i].inputs[rng() % changed.nodes[i].inputs.size()];
    src = (src + 1 + static_cast<uint32_t>(rng() % (i - 1))) % i;  // any earlier node except the old one
    const Patch c = buildSpec(changed, rng, true);
    REQUIRE(c.validate().ok());
    CHECK_FALSE(a == c);
    ++rewires;
  }
  CHECK(rewires > 250);
}
