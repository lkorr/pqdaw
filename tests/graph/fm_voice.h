#pragma once

// The FmVoice example of PATCH_LANGUAGE §4, built programmatically the way the parser (T1.2) will build it:
//
//   patch FmVoice {
//     param ratio = 2.0 [0.25..16]        @label("Ratio")
//     param index = 1.0 [0..20]           @label("Index")
//     param attack  = 5ms   [0ms..10s]    @curve(exp)
//     param release = 300ms [1ms..30s]    @curve(exp)
//     out   out : audio voice mono
//     mod = sine(phasor(voice.hz * ratio))
//     car = sine(phasor(voice.hz + mod * index * voice.hz))
//     env = adsr(voice.gate, a: attack, d: 200ms, s: 0.7, r: release)
//     out = car * env
//     export modulator = mod
//     export envelope  = env
//     layout { node mod at (40, 60)  node car at (200, 60)  node env at (200, 160) }
//   }
//
// `scrambled` builds the same patch with a different node insertion order and wire order (and junk nodes that are
// erased again), to test that equality is structural.

#include <string>
#include <vector>

#include "pq/graph/patch.h"

namespace fm_voice {

using namespace pq::graph;

inline SubcircuitRef adsrRef() {
  const PortType real = portType(Kind::Real, Rate::Param, Poly::Global, kWidthMono);
  SubcircuitRef r;
  r.patch = "adsr";
  r.inputs = {{"gate", portType(Kind::Bool, Rate::Audio, Poly::Voice, kWidthMono), false},
              {"a", real, true},
              {"d", real, true},
              {"s", real, true},
              {"r", real, true}};
  r.outputs = {{"out", portType(Kind::Real, Rate::Audio, Poly::Voice, kWidthMono), false}};
  return r;
}

inline ParamDecl param(double def, Unit u, double lo, Unit lu, double hi, Unit hu, Attr attr) {
  ParamDecl d;
  d.kind = Kind::Real;
  d.defaultValue = Constant::real(def, u);
  d.hasRange = true;
  d.rangeMin = Constant::real(lo, lu);
  d.rangeMax = Constant::real(hi, hu);
  d.attrs.push_back(std::move(attr));
  return d;
}

inline Attr label(const char* s) { return Attr{"label", {AttrArg{"", AttrValue::string(s)}}}; }
inline Attr curveExp() { return Attr{"curve", {AttrArg{"", AttrValue::ident("exp")}}}; }

inline NodeLayout at(float x, float y) {
  NodeLayout l;
  l.placed = true;
  l.x = x;
  l.y = y;
  return l;
}

struct Nodes {
  NodeIndex ratio, index, attack, release, out;
  NodeIndex vh1, m1, ph1, mod;
  NodeIndex vh2, vh3, m2, m3, a1, ph2, car;
  NodeIndex gate, d, s, env;
  NodeIndex outMul, expMod, expEnv;
};

inline Patch build(bool scrambled = false, Nodes* nodesOut = nullptr) {
  Patch p("FmVoice");
  Nodes n{};
  const AtomId hz = atomId("voice.hz"), mul = atomId("mul"), add = atomId("add"), phasor = atomId("phasor"),
               sine = atomId("sine");
  auto addParams = [&] {
    n.ratio = p.addParam("ratio", param(2.0, Unit::None, 0.25, Unit::None, 16, Unit::None, label("Ratio")));
    n.index = p.addParam("index", param(1.0, Unit::None, 0, Unit::None, 20, Unit::None, label("Index")));
    n.attack = p.addParam("attack", param(5, Unit::ms, 0, Unit::ms, 10, Unit::s, curveExp()));
    n.release = p.addParam("release", param(300, Unit::ms, 1, Unit::ms, 30, Unit::s, curveExp()));
  };
  auto addOut = [&] { n.out = p.addOutput("out", portType(Kind::Real, Rate::Audio, Poly::Voice, kWidthMono)); };
  auto addMod = [&] {
    n.vh1 = p.addAtom(hz);
    n.m1 = p.addAtom(mul);
    n.ph1 = p.addAtom(phasor);
    n.mod = p.addAtom(sine);
    p.setNodeName(n.mod, "mod");
    p.setLayout(n.mod, at(40, 60));
  };
  auto addCar = [&] {
    n.vh2 = p.addAtom(hz);
    n.vh3 = p.addAtom(hz);
    n.m2 = p.addAtom(mul);
    n.m3 = p.addAtom(mul);
    n.a1 = p.addAtom(add);
    n.ph2 = p.addAtom(phasor);
    n.car = p.addAtom(sine);
    p.setNodeName(n.car, "car");
    p.setLayout(n.car, at(200, 60));
  };
  auto addEnv = [&] {
    n.gate = p.addAtom(atomId("voice.gate"));
    n.d = p.addConst(Constant::real(200, Unit::ms));
    n.s = p.addConst(Constant::real(0.7));
    n.env = p.addSubcircuit(adsrRef());
    p.setNodeName(n.env, "env");
    p.setLayout(n.env, at(200, 160));
  };
  auto addSinks = [&] {
    n.outMul = p.addAtom(mul);
    n.expMod = p.addExport("modulator");
    n.expEnv = p.addExport("envelope");
  };

  std::vector<NodeIndex> junk;
  if (!scrambled) {
    addParams();
    addOut();
    addMod();
    addCar();
    addEnv();
    addSinks();
  } else {
    addSinks();
    junk.push_back(p.addConst(Constant::real(99)));
    addEnv();
    addCar();
    junk.push_back(p.addAtom(atomId("noise")));
    addMod();
    addParams();
    addOut();
  }

  struct W {
    NodeIndex from;
    uint16_t out;
    NodeIndex to;
    uint16_t in;
  };
  std::vector<W> wires = {
      {n.vh1, 0, n.m1, 0},   {n.ratio, 0, n.m1, 1},  {n.m1, 0, n.ph1, 0},   {n.ph1, 0, n.mod, 0},
      {n.mod, 0, n.m2, 0},   {n.index, 0, n.m2, 1},  {n.m2, 0, n.m3, 0},    {n.vh3, 0, n.m3, 1},
      {n.vh2, 0, n.a1, 0},   {n.m3, 0, n.a1, 1},     {n.a1, 0, n.ph2, 0},   {n.ph2, 0, n.car, 0},
      {n.gate, 0, n.env, 0}, {n.attack, 0, n.env, 1}, {n.d, 0, n.env, 2},   {n.s, 0, n.env, 3},
      {n.release, 0, n.env, 4}, {n.car, 0, n.outMul, 0}, {n.env, 0, n.outMul, 1}, {n.outMul, 0, n.out, 0},
      {n.mod, 0, n.expMod, 0}, {n.env, 0, n.expEnv, 0},
  };
  if (scrambled) {
    for (size_t i = 0; i < wires.size(); i += 2) std::swap(wires[i], wires[wires.size() - 1 - i]);
    p.connect(junk[0], n.m1, 0);  // a junk wire into a real node: erased with the junk below
  }
  for (const W& w : wires) p.connect(PortRef{w.from, w.out}, PortRef{w.to, w.in});

  if (scrambled) {
    // Erasing compacts indices; re-find every node by its NodeId afterwards.
    NodeIndex* fields[] = {&n.ratio, &n.index, &n.attack, &n.release, &n.out, &n.vh1, &n.m1, &n.ph1,
                           &n.mod,   &n.vh2,   &n.vh3,    &n.m2,      &n.m3,  &n.a1,  &n.ph2, &n.car,
                           &n.gate,  &n.d,     &n.s,      &n.env,     &n.outMul, &n.expMod, &n.expEnv};
    static_assert(sizeof(fields) / sizeof(fields[0]) == sizeof(Nodes) / sizeof(NodeIndex));
    std::vector<NodeId> ids;
    for (NodeIndex* f : fields) ids.push_back(p.node(*f).id);
    p.eraseNodes(junk);
    for (size_t i = 0; i < ids.size(); ++i) *fields[i] = *p.indexOf(ids[i]);
  }
  if (nodesOut) *nodesOut = n;
  return p;
}

}  // namespace fm_voice
