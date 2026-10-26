// The symbol sheet (T4.2 golden `circuit-symbols`): one node of every symbol and glyph, captioned, and a row of wire
// styles (bool, param, audio, poly, modulation, a branch with junction dots, a crossing without one).

#include <string>

#include "pq/graph/atoms.h"
#include "pq/views/circuit/circuit_viewer.h"

namespace pq::views::circuit {

namespace {

using graph::Kind;
using graph::NodeIndex;
using graph::Patch;
using graph::PortType;
using graph::Rate;

graph::SubcircuitPort port(std::string name, PortType t, bool optional = false) { return {std::move(name), t, optional}; }

PortType audio() { return graph::portType(Kind::Real, Rate::Audio); }
PortType param() { return graph::portType(Kind::Real, Rate::Param, graph::Poly::Global, graph::kWidthMono); }

graph::SubcircuitRef filterRef(std::string name, std::vector<std::string> outs, bool gain = false) {
  graph::SubcircuitRef r;
  r.patch = std::move(name);
  r.inputs = {port("x", audio()), port("cutoff", param(), true), port("q", param(), true)};
  if (gain) r.inputs.push_back(port("gain", param(), true));
  for (std::string& o : outs) r.outputs.push_back(port(std::move(o), audio()));
  return r;
}

class Sheet {
public:
  Patch p{"symbols"};
  void place(NodeIndex n, float x, float y) {
    graph::NodeLayout l;
    l.placed = true;
    l.x = x;
    l.y = y;
    p.setLayout(n, l);
  }
  void caption(std::string text, float x, float y) { p.addComment({std::move(text), {x, y}}); }
  NodeIndex atom(const char* name, float x, float y, const char* cap = nullptr, float capDy = 46.0f) {
    const NodeIndex n = p.addAtom(graph::atomId(name));
    place(n, x, y);
    caption(cap ? cap : name, x, y + capDy);
    return n;
  }
};

}  // namespace

std::shared_ptr<const PatchDocument> symbolSheet() {
  Sheet s;
  Patch& p = s.p;
  const float dx = 100.0f;
  // Row 1: logic gates and comparators.
  float x = 20.0f, y = 20.0f;
  for (const char* a : {"and", "or", "xor", "nand", "nor", "not", "lt", "ge", "eq", "ne", "schmitt"}) {
    s.atom(a, x, y);
    x += dx;
  }
  // Row 2: summing amps, multiplier, divider, a function box, delays, oscillators.
  x = 20.0f;
  y = 110.0f;
  for (const char* a : {"add", "sub", "mul", "div", "tanh", "z1", "delay", "phasor", "sine", "wavetable", "noise"}) {
    const bool delay = std::string_view(a) == "delay";
    const NodeIndex n = s.atom(a, x, y, delay ? "delay line" : nullptr, 60.0f);
    if (delay) {  // labelled with its delay: a literal at `time`
      const NodeIndex t = p.addConst(graph::Constant::real(10, graph::Unit::ms));
      s.place(t, x - 56, y + 16);
      p.connect(t, n, 1);
    }
    x += dx;
  }
  // Row 3: muxes and IC blocks.
  x = 20.0f;
  y = 214.0f;
  for (const char* a : {"select", "mux", "srff", "dff", "tff", "counter", "clockdiv", "samplehold", "latch", "rise", "pulse"}) {
    s.atom(a, x, y, nullptr, 66.0f);
    x += dx;
  }
  // Row 4: terminals, ports and a subcircuit block.
  x = 20.0f;
  y = 344.0f;
  {
    const NodeIndex c = p.addConst(graph::Constant::real(0.5));
    s.place(c, x, y);
    s.caption("const", x, y + 30);
    x += dx;
    const NodeIndex hz = p.addConst(graph::Constant::real(440, graph::Unit::Hz));
    s.place(hz, x, y);
    s.caption("const 440Hz", x, y + 30);
    x += dx;
    graph::ParamDecl d;
    const NodeIndex pr = p.addParam("cutoff", d);
    s.place(pr, x, y);
    s.caption("param", x, y + 30);
    x += dx;
    const NodeIndex vh = p.addAtom(graph::atomId("voice.hz"));
    s.place(vh, x, y);
    s.caption("voice source", x, y + 30);
    x += dx;
    const NodeIndex in = p.addInput("x", audio());
    s.place(in, x, y);
    s.caption("in", x, y + 30);
    x += dx;
    const NodeIndex out = p.addOutput("y", audio());
    s.place(out, x, y);
    s.caption("out", x, y + 30);
    x += dx;
    const NodeIndex ex = p.addExport("lfo");
    s.place(ex, x, y);
    s.caption("export", x, y + 30);
    x += dx;
    graph::SubcircuitRef adsr;
    adsr.patch = "adsr";
    adsr.inputs = {port("gate", graph::portType(Kind::Bool)), port("a", param(), true), port("d", param(), true),
                   port("s", param(), true), port("r", param(), true)};
    adsr.outputs = {port("out", audio()), port("done", graph::portType(Kind::Bool))};
    const NodeIndex env = p.addSubcircuit(adsr);
    s.place(env, x, y);
    s.caption("subcircuit", x, y + 62);
  }
  // Row 5: stock filters with their response glyphs.
  x = 20.0f;
  y = 464.0f;
  {  // (their callee is the caption above the block)
    const std::pair<const char*, bool> filters[] = {{"biquad_lowpass", false}, {"biquad_highpass", false}, {"biquad_bandpass", false},
                                                    {"biquad_notch", false},   {"biquad_allpass", false},  {"biquad_lowshelf", true},
                                                    {"biquad_peak", true},     {"comb", false}};
    for (const auto& [name, gain] : filters) {
      const NodeIndex f = p.addSubcircuit(filterRef(name, {"out"}, gain));
      s.place(f, x, y);
      x += dx + 12.0f;
    }
  }
  // Row 6: wire styles.
  y = 590.0f;
  x = 20.0f;
  {
    // bool: a comparator into a gate
    const NodeIndex a = p.addAtom(graph::atomId("gt"));
    const NodeIndex b = p.addAtom(graph::atomId("not"));
    s.place(a, x, y);
    s.place(b, x + 80, y + 4);
    p.connect(a, b, 0);
    s.caption("bool", x + 30, y + 52);
    x += 150.0f;
    // param: a param into a multiplier
    graph::ParamDecl d;
    const NodeIndex k = p.addParam("k", d);
    const NodeIndex m = p.addAtom(graph::atomId("mul"));
    s.place(k, x, y + 8);
    s.place(m, x + 80, y);
    p.connect(k, m, 0);
    s.caption("param", x + 30, y + 52);
    x += 150.0f;
    // audio: an input into a function
    const NodeIndex in = p.addInput("sig", audio());
    const NodeIndex t = p.addAtom(graph::atomId("tanh"));
    s.place(in, x, y + 4);
    s.place(t, x + 90, y);
    p.connect(in, t, 0);
    s.caption("audio", x + 30, y + 52);
    x += 170.0f;
    // poly: a voice source into an oscillator
    const NodeIndex vh = p.addAtom(graph::atomId("voice.hz"));
    const NodeIndex ph = p.addAtom(graph::atomId("phasor"));
    s.place(vh, x, y + 8);
    s.place(ph, x + 100, y);
    p.connect(vh, ph, 0);
    s.caption("poly (per voice)", x + 20, y + 52);
    x += 170.0f;
    // modulation: an audio-rate oscillator into a filter's cutoff param
    const NodeIndex lfo = p.addAtom(graph::atomId("sine_hz"));
    const NodeIndex f = p.addSubcircuit(filterRef("onepole", {"lp", "hp"}));
    s.place(lfo, x, y + 8);
    s.place(f, x + 90, y - 16);
    p.connect(lfo, f, 1);
    s.caption("modulation (dashed)", x + 10, y + 52);
    x += 200.0f;
    // a branch: one output to three inputs (junction dots), and two wires crossing (no dot)
    const NodeIndex src = p.addInput("b", audio());
    s.place(src, x, y + 20);
    const NodeIndex t1 = p.addAtom(graph::atomId("abs")), t2 = p.addAtom(graph::atomId("neg")), t3 = p.addAtom(graph::atomId("tanh"));
    s.place(t1, x + 100, y - 20);
    s.place(t2, x + 130, y + 20);
    s.place(t3, x + 100, y + 60);
    p.connect(src, t1, 0);
    p.connect(src, t2, 0);
    p.connect(src, t3, 0);
    s.caption("branch: junction dots", x, y + 98);
    x += 220.0f;
    const NodeIndex c1 = p.addInput("c", audio()), c2 = p.addInput("d", audio());
    const NodeIndex o1 = p.addOutput("e", audio()), o2 = p.addOutput("f", audio());
    s.place(c1, x, y - 16);
    s.place(c2, x, y + 40);
    s.place(o1, x + 120, y + 40);
    s.place(o2, x + 120, y - 16);
    p.connect(c1, o1, 0);
    p.connect(c2, o2, 0);
    s.caption("crossing: no dot", x + 20, y + 72);
  }
  auto doc = std::make_shared<PatchDocument>();
  doc->path = "symbols.pqp";
  doc->parsed.file.patches.push_back(std::move(p));
  return doc;
}

}  // namespace pq::views::circuit
