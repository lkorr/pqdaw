// Patch IR costs on a 1000-node graph: build, validate, adjacency, structural equality, erase, NodeId lookup.
// These bound the IR's share of an edit → interpreter rebuild (target: microseconds) and of a full compile
// (target: 50–200 ms, SPEC §7.4).

#include <cstdio>
#include <string>
#include <vector>

#include "pq/graph/patch.h"
#include "pq_bench.h"

using namespace pq::graph;

namespace {

// in x; 8 params; out y; 250 steps of  a = t + z1(t), t = fast_tanh(prev · p[k])  → 1010 nodes, 1501 wires.
// Every 10th step is named and placed, like a patch with layout.
Patch buildChain(uint32_t steps) {
  Patch p("Bench");
  const AtomId mul = atomId("mul"), tanh = atomId("fast_tanh"), z1 = atomId("z1"), add = atomId("add");
  const NodeIndex x = p.addInput("x", portType(Kind::Real, Rate::Audio, Poly::Voice, kWidthMono));
  NodeIndex params[8];
  for (int k = 0; k < 8; ++k) {
    ParamDecl d;
    d.defaultValue = Constant::real(0.5);
    d.hasRange = true;
    d.rangeMin = Constant::real(0);
    d.rangeMax = Constant::real(1);
    d.attrs.push_back(Attr{"label", {AttrArg{"", AttrValue::string("Gain")}}});
    params[k] = p.addParam("p" + std::to_string(k), std::move(d));
  }
  const NodeIndex y = p.addOutput("y", portType(Kind::Real, Rate::Audio, Poly::Voice, kWidthMono));
  NodeIndex prev = x;
  for (uint32_t i = 0; i < steps; ++i) {
    const NodeIndex m = p.addAtom(mul), t = p.addAtom(tanh), z = p.addAtom(z1), a = p.addAtom(add);
    p.connect(prev, m, 0);
    p.connect(params[i % 8], m, 1);
    p.connect(m, t, 0);
    p.connect(t, z, 0);
    p.connect(t, a, 0);
    p.connect(z, a, 1);
    if (i % 10 == 0) {
      p.setNodeName(a, "s" + std::to_string(i));
      NodeLayout l;
      l.placed = true;
      l.x = static_cast<float>(i);
      l.y = 10.0f;
      p.setLayout(a, l);
    }
    prev = a;
  }
  p.connect(prev, y, 0);
  return p;
}

}  // namespace

int main() {
  constexpr uint32_t kSteps = 250;
  const Patch ref = buildChain(kSteps);
  if (!ref.validate().ok()) {
    std::printf("benchmark graph is invalid: %s\n", ref.validate().error().message.c_str());
    return 1;
  }
  const uint32_t nodes = ref.nodeCount();
  std::printf("graph: %u nodes, %u wires, %u ports\n", nodes, ref.wireCount(), ref.portCount());

  auto run = [&](const char* name, auto&& f) {
    const auto s = pq::bench::measure(f, 1, 15, 3, 2e7);
    char label[96];
    std::snprintf(label, sizeof label, "patch1000/%s", name);
    std::printf("BENCH %-40s median %10.2f us  min %10.2f us  (%.1f ns/node)\n", label, s.medianNs / 1000.0,
                s.minNs / 1000.0, s.medianNs / nodes);
  };

  run("build", [&] {
    Patch p = buildChain(kSteps);
    pq::bench::doNotOptimize(p.nodeCount());
  });
  run("validate (valid graph)", [&] {
    const auto e = ref.validateAll();
    pq::bench::doNotOptimize(e.size());
  });
  run("adjacency (CSR)", [&] {
    const Adjacency a = ref.adjacency();
    pq::bench::doNotOptimize(a.outWires.size());
  });
  const Patch copy = ref;
  run("operator== (structural)", [&] {
    const bool eq = ref == copy;
    pq::bench::doNotOptimize(eq);
  });
  run("copy", [&] {
    Patch p = ref;
    pq::bench::doNotOptimize(p.nodeCount());
  });
  std::vector<NodeIndex> erase;
  for (NodeIndex i = 20; i < nodes; i += 100) erase.push_back(i);
  run("copy + eraseNodes(10)", [&] {
    Patch p = ref;
    p.eraseNodes(erase);
    pq::bench::doNotOptimize(p.nodeCount());
  });
  std::vector<NodeId> ids;
  for (NodeIndex i = 0; i < nodes; ++i) ids.push_back(ref.node(i).id);
  {
    const auto s = pq::bench::measure(
        [&] {
          uint32_t sum = 0;
          for (NodeId id : ids) sum += *ref.indexOf(id);
          pq::bench::doNotOptimize(sum);
        },
        ids.size());
    pq::bench::report("patch1000/indexOf(NodeId), no erases", s, "ns/lookup");
  }
  {
    // After an erase near the front, most lookups miss the one-probe guess and binary-search.
    Patch erased = ref;
    const NodeIndex first[] = {20};
    erased.eraseNodes(first);
    std::vector<NodeId> live;
    for (NodeIndex i = 0; i < erased.nodeCount(); ++i) live.push_back(erased.node(i).id);
    const auto s = pq::bench::measure(
        [&] {
          uint32_t sum = 0;
          for (NodeId id : live) sum += *erased.indexOf(id);
          pq::bench::doNotOptimize(sum);
        },
        live.size());
    pq::bench::report("patch1000/indexOf(NodeId), after erase", s, "ns/lookup");
  }
  return 0;
}
