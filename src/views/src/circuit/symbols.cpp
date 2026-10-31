// Schematic symbols (T4.2): classification, geometry, body painting. See pq/views/circuit/symbols.h.

#include "pq/views/circuit/symbols.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

#include "pq/graph/atoms.h"
#include "pq/ui/lines.h"

namespace pq::views::circuit {

using graph::NodeIndex;
using graph::NodeKind;

namespace {

constexpr float kPitch = 16.0f;     // pin spacing
constexpr float kCharW = 6.0f;      // world units per label character (an 11-px UI font at zoom 1)
constexpr float kPi = 3.14159265358979f;

float roundUp4(float v) noexcept { return std::ceil(v / 4.0f) * 4.0f; }
float chars(std::string_view s) noexcept {
  // UTF-8 code points (labels are mostly ASCII; ⁻¹ etc. count once)
  float n = 0.0f;
  for (unsigned char c : s) n += (c & 0xC0) != 0x80 ? 1.0f : 0.0f;
  return n;
}

struct Classified {
  Symbol symbol;
  Glyph glyph = Glyph::None;
  std::string_view title = {};
};

// Atoms by name.
Classified classifyAtom(std::string_view name, uint32_t requiredInputs) {
  struct Row {
    std::string_view name;
    Symbol symbol;
    Glyph glyph;
    std::string_view title;
  };
  static constexpr Row kRows[] = {
      {"and", Symbol::And, Glyph::None, {}},          {"or", Symbol::Or, Glyph::None, {}},
      {"xor", Symbol::Xor, Glyph::None, {}},          {"nand", Symbol::Nand, Glyph::None, {}},
      {"nor", Symbol::Nor, Glyph::None, {}},          {"not", Symbol::Not, Glyph::None, {}},
      {"lt", Symbol::Comparator, Glyph::Lt, {}},      {"le", Symbol::Comparator, Glyph::Le, {}},
      {"gt", Symbol::Comparator, Glyph::Gt, {}},      {"ge", Symbol::Comparator, Glyph::Ge, {}},
      {"eq", Symbol::Comparator, Glyph::Eq, {}},      {"ne", Symbol::Comparator, Glyph::Ne, {}},
      {"schmitt", Symbol::Comparator, Glyph::Hysteresis, {}},
      {"add", Symbol::Summer, Glyph::Plus, {}},       {"sub", Symbol::Summer, Glyph::Minus, {}},
      {"mul", Symbol::Multiplier, Glyph::None, {}},   {"div", Symbol::Divider, Glyph::None, {}},
      {"z1", Symbol::Delay, Glyph::UnitDelay, {}},    {"delay", Symbol::Delay, Glyph::DelayLine, {}},
      {"delay_allpass", Symbol::Delay, Glyph::DelayLine, "allpass"},
      {"delay_cubic", Symbol::Delay, Glyph::DelayLine, "cubic"},
      {"phasor", Symbol::Oscillator, Glyph::Saw, {}}, {"sine", Symbol::Oscillator, Glyph::Sine, {}},
      {"sine_hz", Symbol::Oscillator, Glyph::Sine, {}},
      {"wavetable", Symbol::Oscillator, Glyph::Table, {}},
      {"noise", Symbol::Oscillator, Glyph::Noise, {}},
      {"select", Symbol::Mux, Glyph::None, {}},       {"mux", Symbol::Mux, Glyph::None, {}},
      {"demux", Symbol::Mux, Glyph::None, {}},
      {"srff", Symbol::Ic, Glyph::None, "SR"},        {"dff", Symbol::Ic, Glyph::None, "D"},
      {"tff", Symbol::Ic, Glyph::None, "T"},          {"counter", Symbol::Ic, Glyph::None, "CTR"},
      {"clockdiv", Symbol::Ic, Glyph::None, "DIV"},   {"timer", Symbol::Ic, Glyph::None, "TMR"},
      {"pulse", Symbol::Ic, Glyph::None, "PULSE"},    {"rise", Symbol::Ic, Glyph::None, "RISE"},
      {"fall", Symbol::Ic, Glyph::None, "FALL"},      {"change", Symbol::Ic, Glyph::None, "CHG"},
      {"samplehold", Symbol::Ic, Glyph::None, "S/H"}, {"latch", Symbol::Ic, Glyph::None, "LATCH"},
      {"neg", Symbol::Box, Glyph::None, "-x"},        {"channel", Symbol::Box, Glyph::None, "[ ]"},
      {"pack", Symbol::Box, Glyph::None, "( , )"},
  };
  for (const Row& r : kRows)
    if (r.name == name) return {r.symbol, r.glyph, r.title.empty() ? name : r.title};
  if (requiredInputs == 0) return {Symbol::Source, Glyph::None, name};  // voice.hz, transport.bpm, sr, track.in …
  return {Symbol::Box, Glyph::None, name};
}

// Stock filter subcircuits (stdlib/filters) by callee name.
Glyph filterGlyph(std::string_view callee) noexcept {
  struct Row {
    std::string_view name;
    Glyph glyph;
  };
  static constexpr Row kRows[] = {
      {"onepole", Glyph::Lowpass},          {"svf", Glyph::Lowpass},            {"svf_core", Glyph::Bandpass},
      {"biquad", Glyph::Lowpass},           {"biquad_lowpass", Glyph::Lowpass}, {"biquad_highpass", Glyph::Highpass},
      {"biquad_bandpass", Glyph::Bandpass}, {"biquad_notch", Glyph::Notch},     {"biquad_allpass", Glyph::Allpass},
      {"biquad_peak", Glyph::Peak},         {"biquad_lowshelf", Glyph::Shelf},  {"biquad_highshelf", Glyph::Shelf},
      {"allpass", Glyph::Allpass},          {"allpass_delay", Glyph::Allpass},  {"dcblock", Glyph::Highpass},
      {"comb", Glyph::Comb},                {"comb_lp", Glyph::Comb},
  };
  for (const Row& r : kRows)
    if (r.name == callee) return r.glyph;
  return Glyph::None;
}

// n pin offsets centred on the band [top, top + span), kPitch apart, on multiples of 4.
void distribute(std::vector<PinSpec>& pins, float top, float span) {
  uint32_t n = 0;
  for (const PinSpec& p : pins) n += p.visible ? 1u : 0u;
  const float mid = top + span * 0.5f;
  uint32_t k = 0;
  float firstY = mid;
  for (PinSpec& p : pins) {
    if (!p.visible) continue;
    p.y = std::round((mid + (static_cast<float>(k) - static_cast<float>(n - 1) * 0.5f) * kPitch) / 4.0f) * 4.0f;
    if (k == 0) firstY = p.y;
    ++k;
  }
  for (PinSpec& p : pins)
    if (!p.visible) p.y = firstY;
}

uint32_t visibleCount(const std::vector<PinSpec>& pins) noexcept {
  uint32_t n = 0;
  for (const PinSpec& p : pins) n += p.visible ? 1u : 0u;
  return n;
}

float maxLabel(const std::vector<PinSpec>& pins) noexcept {
  float m = 0.0f;
  for (const PinSpec& p : pins)
    if (p.visible) m = std::max(m, chars(p.label));
  return m;
}

void setStubs(std::vector<PinSpec>& pins, float stub) {
  for (PinSpec& p : pins) p.stub = stub;
}

// Text of a constant as written (value and unit).
std::string constantText(const graph::Constant& c) {
  if (c.kind == graph::Kind::Bool) return c.literal != 0.0 ? "true" : "false";
  char buf[48];
  const double v = c.literal;
  if (v == std::floor(v) && std::fabs(v) < 1e9) std::snprintf(buf, sizeof buf, "%.0f", v);
  else std::snprintf(buf, sizeof buf, "%.4g", v);
  return std::string(buf) + std::string(graph::unitSuffix(c.unit));
}

}  // namespace

SymbolSpec describe(const graph::Patch& p, NodeIndex n, const std::vector<uint8_t>& wired) {
  const graph::Node& nd = p.node(n);
  SymbolSpec s;
  s.inputs.resize(nd.numInputs);
  s.outputs.resize(nd.numOutputs);
  for (uint32_t i = 0; i < nd.numInputs; ++i) {
    const bool on = i < wired.size() && wired[i];
    s.inputs[i].visible = on || !p.inputOptional(n, i);
  }
  uint32_t required = 0;
  for (uint32_t i = 0; i < nd.numInputs; ++i) required += p.inputOptional(n, i) ? 0u : 1u;
  if (!p.nodeName(n).empty() && (nd.kind == NodeKind::Atom || nd.kind == NodeKind::Subcircuit || nd.kind == NodeKind::Const))
    s.caption = p.nodeName(n);
  switch (nd.kind) {
    case NodeKind::Atom: {
      const graph::AtomSignature& sig = graph::atomSignature(nd.atom);
      const Classified c = classifyAtom(sig.name, required);
      s.symbol = c.symbol;
      s.glyph = c.glyph;
      s.title = std::string(c.title);
      break;
    }
    case NodeKind::Const:
      s.symbol = Symbol::Const;
      s.title = constantText(p.constant(n));
      s.caption.clear();
      break;
    case NodeKind::Param: s.symbol = Symbol::Param; s.title = p.nodeName(n); break;
    case NodeKind::Input: s.symbol = Symbol::Input; s.title = p.nodeName(n); break;
    case NodeKind::Output: s.symbol = Symbol::Output; s.title = p.nodeName(n); break;
    case NodeKind::Export: s.symbol = Symbol::Export; s.title = p.nodeName(n); break;
    case NodeKind::Subcircuit: {
      const graph::SubcircuitRef& r = p.subcircuit(n);
      const Glyph fg = filterGlyph(r.patch);
      s.symbol = fg != Glyph::None ? Symbol::Filter : Symbol::Subcircuit;
      s.glyph = fg;
      s.title = r.patch;
      break;
    }
    case NodeKind::Unknown: s.symbol = Symbol::Unknown; s.title = p.nodeName(n); break;
    case NodeKind::Equation: s.symbol = Symbol::Equation; break;
  }
  // Pin labels of the labelled symbols.
  const bool labelled = s.symbol == Symbol::Ic || s.symbol == Symbol::Subcircuit || s.symbol == Symbol::Filter;
  if (labelled) {
    for (uint32_t i = 0; i < nd.numInputs; ++i) s.inputs[i].label = std::string(p.inputName(n, i));
    for (uint32_t i = 0; i < nd.numOutputs; ++i) s.outputs[i].label = std::string(p.outputName(n, i));
    // A single output named like the symbol says nothing.
    if (nd.numOutputs == 1 && (s.outputs[0].label == "out" || s.outputs[0].label == "q")) s.outputs[0].label.clear();
    if (visibleCount(s.inputs) == 1)
      for (PinSpec& pin : s.inputs)
        if (pin.label == "x" || pin.label == "in") pin.label.clear();
  }
  // M4 review L15: a mux numbers its data inputs (and a demux its outputs) next to the pins: "#" the index, then 0, 1, …;
  // a select's condition "?", then the input taken when true ("1") and when false ("0").
  if (s.symbol == Symbol::Mux && nd.kind == NodeKind::Atom) {
    const std::string_view atom = graph::atomSignature(nd.atom).name;
    if (atom == "select" && nd.numInputs == 3) {
      s.inputs[0].label = "?";
      s.inputs[1].label = "1";
      s.inputs[2].label = "0";
    } else if (atom == "mux") {
      for (uint32_t i = 0; i < nd.numInputs; ++i) s.inputs[i].label = i == 0 ? std::string("#") : std::to_string(i - 1);
    } else if (atom == "demux" && nd.numInputs >= 1) {
      s.inputs[0].label = "#";
      for (uint32_t i = 0; i < nd.numOutputs; ++i) s.outputs[i].label = std::to_string(i);
    }
  }
  const uint32_t vi = visibleCount(s.inputs), vo = visibleCount(s.outputs);
  const uint32_t rows = std::max<uint32_t>({vi, vo, 1});
  switch (s.symbol) {
    case Symbol::And:
    case Symbol::Or:
    case Symbol::Xor:
      s.w = 40;
      s.h = 32;
      distribute(s.inputs, 0, 32);
      distribute(s.outputs, 0, 32);
      setStubs(s.inputs, 8);
      setStubs(s.outputs, 8);
      break;
    case Symbol::Nand:
    case Symbol::Nor:
      s.w = 40;
      s.h = 32;
      distribute(s.inputs, 0, 32);
      distribute(s.outputs, 0, 32);
      setStubs(s.inputs, 8);
      setStubs(s.outputs, 6);
      break;
    case Symbol::Not:
      s.w = 32;
      s.h = 24;
      distribute(s.inputs, 0, 24);
      distribute(s.outputs, 0, 24);
      setStubs(s.inputs, 6);
      setStubs(s.outputs, 4);
      break;
    case Symbol::Comparator:
    case Symbol::Summer:
      s.w = 40;
      s.h = 32;
      distribute(s.inputs, 0, 32);
      distribute(s.outputs, 0, 32);
      setStubs(s.inputs, 8);
      setStubs(s.outputs, 6);
      break;
    case Symbol::Multiplier:
    case Symbol::Divider:
    case Symbol::Oscillator:
    case Symbol::Equation:
      s.w = 32;
      s.h = 32;
      distribute(s.inputs, 0, 32);
      distribute(s.outputs, 0, 32);
      for (PinSpec& pin : s.inputs) pin.stub = 16.0f - std::sqrt(std::max(0.0f, 121.0f - (pin.y - 16.0f) * (pin.y - 16.0f)));
      setStubs(s.outputs, 5);
      break;
    case Symbol::Delay:
      s.w = s.glyph == Glyph::DelayLine ? 52.0f : 40.0f;  // a delay line has room for its time
      s.h = vi <= 1 ? 24.0f : kPitch * static_cast<float>(vi);
      distribute(s.inputs, 0, s.h);
      distribute(s.outputs, 0, s.h);
      setStubs(s.inputs, 8);
      setStubs(s.outputs, 8);
      break;
    case Symbol::Mux:
      s.w = 40;  // L15: room for the input numbers inside the body
      s.h = std::max(32.0f, kPitch * static_cast<float>(rows) + 8.0f);
      distribute(s.inputs, 0, s.h);
      distribute(s.outputs, 0, s.h);
      setStubs(s.inputs, 8);
      setStubs(s.outputs, 8);
      break;
    case Symbol::Filter:
    case Symbol::Ic:
    case Symbol::Subcircuit: {
      const float top = s.symbol == Symbol::Filter ? 24.0f : 16.0f;
      s.h = std::max(s.symbol == Symbol::Filter ? 40.0f : 32.0f, kPitch * static_cast<float>(rows) + top + 4.0f);
      const float pins = 16.0f + kCharW * (maxLabel(s.inputs) + maxLabel(s.outputs)) + 12.0f;
      // A filter's name is its caption (above), not in the body.
      const float title = s.symbol == Symbol::Filter ? 0.0f : 16.0f + kCharW * chars(s.title) + 8.0f;
      s.w = roundUp4(std::max({s.symbol == Symbol::Filter ? 56.0f : (s.symbol == Symbol::Ic ? 48.0f : 64.0f), pins, title}));
      distribute(s.inputs, top, s.h - top);
      distribute(s.outputs, top, s.h - top);
      setStubs(s.inputs, 8);
      setStubs(s.outputs, 8);
      break;
    }
    case Symbol::Box:
    case Symbol::Unknown:
      s.w = roundUp4(std::max(40.0f, 16.0f + kCharW * chars(s.title) + 8.0f));
      s.h = rows <= 1 ? 24.0f : kPitch * static_cast<float>(rows);
      distribute(s.inputs, 0, s.h);
      distribute(s.outputs, 0, s.h);
      setStubs(s.inputs, 8);
      setStubs(s.outputs, 8);
      break;
    case Symbol::Const:
    case Symbol::Param:
    case Symbol::Source:
      s.w = roundUp4(std::max(20.0f, (s.symbol == Symbol::Param ? 18.0f : 12.0f) + kCharW * chars(s.title) + 6.0f));
      s.h = 16;
      distribute(s.inputs, 0, 16);
      distribute(s.outputs, 0, 16);
      setStubs(s.outputs, 4);
      setStubs(s.inputs, 4);
      break;
    case Symbol::Input:
    case Symbol::Output:
    case Symbol::Export:
      s.w = roundUp4(std::max(40.0f, 16.0f + kCharW * chars(s.title) + 10.0f));
      s.h = 16;
      distribute(s.inputs, 0, 16);
      distribute(s.outputs, 0, 16);
      setStubs(s.inputs, 4);
      setStubs(s.outputs, 4);
      break;
  }
  s.bodyKey = static_cast<uint64_t>(s.symbol) | (static_cast<uint64_t>(s.glyph) << 8) |
              (static_cast<uint64_t>(s.w / 4.0f) << 16) | (static_cast<uint64_t>(s.h / 4.0f) << 36);
  return s;
}

std::vector<SymbolSpec> describeAll(const graph::Patch& p) {
  const graph::Adjacency adj = p.adjacency();
  std::vector<SymbolSpec> out;
  out.reserve(p.nodeCount());
  std::vector<uint8_t> wired;
  for (NodeIndex n = 0; n < p.nodeCount(); ++n) {
    wired.assign(p.node(n).numInputs, 0);
    for (uint32_t i = 0; i < p.node(n).numInputs; ++i) wired[i] = adj.driver[p.flatInput(n, i)] != graph::kNone ? 1 : 0;
    out.push_back(describe(p, n, wired));
  }
  return out;
}

graph::LayoutGeometry geometryOf(const graph::Patch& p, const std::vector<SymbolSpec>& specs) {
  graph::LayoutGeometry g;
  g.shape.resize(p.nodeCount());
  g.inPinY.assign(p.portCount(), 0.0f);
  g.outPinY.assign(p.portCount(), 0.0f);
  for (NodeIndex n = 0; n < p.nodeCount(); ++n) {
    const SymbolSpec& s = specs[n];
    g.shape[n] = {s.w, s.h};
    for (uint32_t i = 0; i < s.inputs.size(); ++i) g.inPinY[p.flatInput(n, i)] = s.inputs[i].y;
    for (uint32_t i = 0; i < s.outputs.size(); ++i) g.outPinY[p.flatOutput(n, i)] = s.outputs[i].y;
  }
  return g;
}

ui::Rgba fillColour(const ui::Theme& t, Symbol s) noexcept {
  switch (s) {
    case Symbol::Const:
    case Symbol::Source: return t.circuit.terminalFill;
    case Symbol::Param: return t.circuit.paramFill;
    case Symbol::Input:
    case Symbol::Output:
    case Symbol::Export: return t.circuit.ioFill;
    case Symbol::Subcircuit: return t.circuit.subcircuitFill;
    default: return t.circuit.symbolFill;
  }
}

float strokeWidth(float scale) noexcept { return std::clamp(1.2f * scale, 0.75f, 3.0f); }

namespace {

// Local (world units, relative to the symbol's top-left) → device px.
struct Pen {
  ui::DrawList& dl;
  ui::Vec2 o;
  float s;
  float t;  // stroke width, device px
  ui::Rgba col;
  std::vector<ui::Vec2>& pts;

  ui::Vec2 at(float x, float y) const noexcept { return {o.x + s * x, o.y + s * y}; }
  void begin() { pts.clear(); }
  void add(float x, float y) { pts.push_back(at(x, y)); }
  void stroke() {
    if (pts.size() >= 2) ui::smoothPolyline(dl, pts.data(), static_cast<uint32_t>(pts.size()), col, t);
  }
  void strokeClosed() {
    if (pts.size() < 3) return;
    pts.push_back(pts[0]);
    pts.push_back(pts[1]);  // so the first corner joins like the others
    stroke();
  }
  // Non-antialiased fan fill (star-shaped outlines; the stroke drawn over it covers the edge).
  void fill() {
    const uint32_t n = static_cast<uint32_t>(pts.size());
    if (n < 3) return;
    ui::Vec2 c{0, 0};
    for (const ui::Vec2& q : pts) c.x += q.x, c.y += q.y;
    c.x /= static_cast<float>(n);
    c.y /= static_cast<float>(n);
    const ui::Vec2 uv = dl.whiteUv();
    dl.primReserve(n * 3, n + 1);
    const uint32_t base = dl.vtxCurrentIdx();
    dl.primWriteVtx(c, uv, col);
    for (const ui::Vec2& q : pts) dl.primWriteVtx(q, uv, col);
    for (uint32_t i = 0; i < n; ++i) {
      dl.primWriteIdx(base);
      dl.primWriteIdx(base + 1 + i);
      dl.primWriteIdx(base + 1 + (i + 1) % n);
    }
  }
  void line(float x0, float y0, float x1, float y1) {
    begin();
    add(x0, y0);
    add(x1, y1);
    stroke();
  }
  // Arc points around (cx, cy) from a0 to a1 (radians, screen space: y down), appended.
  void arc(float cx, float cy, float r, float a0, float a1, int steps) {
    for (int i = 0; i <= steps; ++i) {
      const float a = a0 + (a1 - a0) * static_cast<float>(i) / static_cast<float>(steps);
      add(cx + r * std::cos(a), cy + r * std::sin(a));
    }
  }
  // Quadratic Bézier appended (without its start point).
  void quad(float x0, float y0, float cx, float cy, float x1, float y1, int steps) {
    for (int i = 1; i <= steps; ++i) {
      const float u = static_cast<float>(i) / static_cast<float>(steps), v = 1.0f - u;
      add(v * v * x0 + 2.0f * u * v * cx + u * u * x1, v * v * y0 + 2.0f * u * v * cy + u * u * y1);
    }
  }
  void rect(float x0, float y0, float x1, float y1, float radius, bool filled) {
    const ui::Rect r{o.x + s * x0, o.y + s * y0, o.x + s * x1, o.y + s * y1};
    if (filled) dl.addRoundedRectFilled(r, radius * s, col);
    else dl.addRoundedRectStroke(r, std::max(radius * s, 0.0f), col, std::max(1.0f, std::round(t)));
  }
  void circle(float cx, float cy, float r, bool filled) {
    if (filled) dl.addCircleFilled(at(cx, cy), r * s, col);
    else dl.addCircleStroke(at(cx, cy), r * s, col, t);
  }
  void dot(float cx, float cy, float r) { dl.addCircleFilled(at(cx, cy), std::max(r * s, 0.6f), col); }
};

int arcSteps(float r, float s) noexcept { return std::clamp(static_cast<int>(r * s * 0.6f), 6, 40); }

// ---- outlines (shared by both layers: the fill fills it, the stroke draws it) --------------------------------
void andOutline(Pen& p, float right) {  // flat back at x = 8, semicircular front ending at x = right
  const float r = 14.0f, cx = right - r;
  p.begin();
  p.add(8, 2);
  p.add(cx, 2);
  p.arc(cx, 16, r, -kPi / 2, kPi / 2, arcSteps(r, p.s));
  p.add(8, 30);
}

void orOutline(Pen& p, float tip) {  // concave back, curved sides meeting at the tip (x = tip, y = 16)
  const int st = arcSteps(14.0f, p.s);
  p.begin();
  p.add(6, 2);
  p.quad(6, 2, tip - 10, 2, tip, 16, st);
  p.quad(tip, 16, tip - 10, 30, 6, 30, st);
  p.quad(6, 30, 12, 16, 6, 2, st);
  p.pts.pop_back();  // the start point again
}

void triangle(Pen& p, float x0, float y0, float y1, float tip) {
  p.begin();
  p.add(x0, y0);
  p.add(tip, (y0 + y1) * 0.5f);
  p.add(x0, y1);
}

void glyphWave(Pen& p, Glyph g, float cx, float cy, float rx, float ry) {
  p.begin();
  switch (g) {
    case Glyph::Saw:
      p.add(cx - rx, cy + ry);
      p.add(cx, cy - ry);
      p.add(cx, cy + ry);
      p.add(cx + rx, cy - ry);
      break;
    case Glyph::Sine:
      for (int i = 0; i <= 24; ++i) {
        const float u = static_cast<float>(i) / 24.0f;
        p.add(cx - rx + 2.0f * rx * u, cy - ry * std::sin(2.0f * kPi * u));
      }
      break;
    case Glyph::Table: {
      const float steps[] = {0.2f, 0.9f, 0.5f, -0.6f, -0.2f, -0.9f};
      for (int i = 0; i < 6; ++i) {
        const float x0 = cx - rx + 2.0f * rx * static_cast<float>(i) / 6.0f, x1 = cx - rx + 2.0f * rx * static_cast<float>(i + 1) / 6.0f;
        p.add(x0, cy - ry * steps[i]);
        p.add(x1, cy - ry * steps[i]);
      }
      break;
    }
    case Glyph::Noise: {
      const float v[] = {0.1f, 0.8f, -0.5f, 0.4f, -0.9f, 0.6f, -0.2f, 0.9f, -0.7f, 0.3f, -0.4f};
      for (int i = 0; i < 11; ++i) p.add(cx - rx + 2.0f * rx * static_cast<float>(i) / 10.0f, cy - ry * v[i]);
      break;
    }
    default: break;
  }
  p.stroke();
}

// Filter response glyph in the box [x0, x1] × [y0, y1] (y0 = top, the passband level a third down).
void glyphResponse(Pen& p, Glyph g, float x0, float y0, float x1, float y1) {
  const float pass = y0 + (y1 - y0) * 0.3f, stop = y1, w = x1 - x0;
  auto X = [&](float u) { return x0 + w * u; };
  p.begin();
  switch (g) {
    case Glyph::Lowpass:
      p.add(X(0), pass);
      p.add(X(0.5f), pass);
      p.quad(X(0.5f), pass, X(0.68f), pass, X(0.92f), stop, 8);
      break;
    case Glyph::Highpass:
      p.add(X(0.08f), stop);
      p.quad(X(0.08f), stop, X(0.32f), pass, X(0.5f), pass, 8);
      p.add(X(1), pass);
      break;
    case Glyph::Bandpass:
      p.add(X(0.1f), stop);
      p.quad(X(0.1f), stop, X(0.5f), y0 - (y1 - y0) * 0.35f, X(0.9f), stop, 12);
      break;
    case Glyph::Notch:
      p.add(X(0), pass);
      p.add(X(0.35f), pass);
      p.add(X(0.5f), stop);
      p.add(X(0.65f), pass);
      p.add(X(1), pass);
      break;
    case Glyph::Allpass: {
      p.add(X(0), pass);
      p.add(X(1), pass);
      p.stroke();
      p.begin();  // phase: an S below
      const float m = (pass + stop) * 0.5f, a = (stop - pass) * 0.35f;
      for (int i = 0; i <= 12; ++i) {
        const float u = static_cast<float>(i) / 12.0f;
        p.add(X(0.2f + 0.6f * u), m + a * std::tanh(6.0f * (u - 0.5f)));
      }
      break;
    }
    case Glyph::Shelf:
      p.add(X(0), (pass + stop) * 0.5f);
      p.add(X(0.35f), (pass + stop) * 0.5f);
      p.quad(X(0.35f), (pass + stop) * 0.5f, X(0.5f), (pass + stop) * 0.5f, X(0.55f), (pass * 3.0f + stop) * 0.25f, 4);
      p.quad(X(0.55f), (pass * 3.0f + stop) * 0.25f, X(0.6f), pass, X(0.75f), pass, 4);
      p.add(X(1), pass);
      break;
    case Glyph::Peak:
      p.add(X(0), (pass + stop) * 0.5f);
      p.add(X(0.3f), (pass + stop) * 0.5f);
      p.quad(X(0.3f), (pass + stop) * 0.5f, X(0.5f), y0 - (y1 - y0) * 0.25f, X(0.7f), (pass + stop) * 0.5f, 10);
      p.add(X(1), (pass + stop) * 0.5f);
      break;
    case Glyph::Comb:
      for (int i = 0; i < 4; ++i) {
        const float a = X(0.05f + 0.25f * static_cast<float>(i)), b = X(0.175f + 0.25f * static_cast<float>(i));
        p.add(a, stop);
        p.add(b, pass);
      }
      p.add(X(1), stop);
      break;
    default: break;
  }
  p.stroke();
}

// Relation sign of a comparator, centred at (cx, cy), half-size r.
void glyphRelation(Pen& p, Glyph g, float cx, float cy, float r) {
  auto chevron = [&](bool less) {
    p.begin();
    const float d = less ? 1.0f : -1.0f;
    p.add(cx + d * r * 0.7f, cy - r);
    p.add(cx - d * r * 0.7f, cy);
    p.add(cx + d * r * 0.7f, cy + r);
    p.stroke();
  };
  switch (g) {
    case Glyph::Lt: chevron(true); break;
    case Glyph::Gt: chevron(false); break;
    case Glyph::Le:
    case Glyph::Ge: {
      const bool less = g == Glyph::Le;
      const float d = less ? 1.0f : -1.0f;
      p.begin();
      p.add(cx + d * r * 0.7f, cy - r);
      p.add(cx - d * r * 0.7f, cy - r * 0.25f);
      p.add(cx + d * r * 0.7f, cy + r * 0.5f);
      p.stroke();
      p.line(cx - r * 0.7f, cy + r, cx + r * 0.7f, cy + r);
      break;
    }
    case Glyph::Eq:
    case Glyph::Ne:
      p.line(cx - r * 0.8f, cy - r * 0.35f, cx + r * 0.8f, cy - r * 0.35f);
      p.line(cx - r * 0.8f, cy + r * 0.35f, cx + r * 0.8f, cy + r * 0.35f);
      if (g == Glyph::Ne) p.line(cx + r * 0.4f, cy - r, cx - r * 0.4f, cy + r);
      break;
    case Glyph::Hysteresis:
      p.begin();
      p.add(cx - r, cy + r * 0.6f);
      p.add(cx + r * 0.3f, cy + r * 0.6f);
      p.add(cx + r * 0.3f, cy - r * 0.6f);
      p.add(cx + r, cy - r * 0.6f);
      p.stroke();
      p.begin();
      p.add(cx - r, cy - r * 0.6f);
      p.add(cx - r * 0.3f, cy - r * 0.6f);
      p.add(cx - r * 0.3f, cy + r * 0.6f);
      p.stroke();
      break;
    default: break;
  }
}

}  // namespace

void paintBody(ui::DrawList& dl, const SymbolSpec& spec, ui::Vec2 origin, float scale, Layer layer, ui::Rgba col) {
  thread_local std::vector<ui::Vec2> scratch;
  Pen p{dl, origin, scale, strokeWidth(scale), col, scratch};
  const bool fill = layer == Layer::Fill;
  const float w = spec.w, h = spec.h;
  auto shape = [&] {  // the current outline, filled or stroked
    if (fill) p.fill();
    else p.strokeClosed();
  };
  switch (spec.symbol) {
    case Symbol::And:
      andOutline(p, 32);
      shape();
      break;
    case Symbol::Nand:
      andOutline(p, 28);
      shape();
      p.circle(31, 16, 3, fill);
      break;
    case Symbol::Or:
      orOutline(p, 32);
      shape();
      break;
    case Symbol::Nor:
      orOutline(p, 28);
      shape();
      p.circle(31, 16, 3, fill);
      break;
    case Symbol::Xor:
      orOutline(p, 32);
      shape();
      if (!fill) {
        p.begin();
        p.add(2, 2);
        p.quad(2, 2, 8, 16, 2, 30, arcSteps(14.0f, scale));
        p.stroke();
      }
      break;
    case Symbol::Not:
      triangle(p, 6, 3, 21, 22);
      shape();
      p.circle(25, 12, 3, fill);
      break;
    case Symbol::Comparator:
    case Symbol::Summer:
      triangle(p, 8, 1, 31, 34);
      shape();
      if (!fill) {
        if (spec.symbol == Symbol::Comparator) {
          glyphRelation(p, spec.glyph, 17, 16, 4.5f);
        } else {
          // Σ, then the input signs: + + (add) or + − (sub)
          p.begin();
          p.add(21, 11);
          p.add(14, 11);
          p.add(18, 16);
          p.add(14, 21);
          p.add(21, 21);
          p.stroke();
          // L15: the signs a little larger, so add (+ +) and sub (+ −) read apart at a glance.
          p.line(9.5f, 8, 14, 8);
          p.line(11.75f, 5.75f, 11.75f, 10.25f);
          p.line(9.5f, 24, 14, 24);
          if (spec.glyph == Glyph::Plus) p.line(11.75f, 21.75f, 11.75f, 26.25f);
        }
      }
      break;
    case Symbol::Multiplier:
    case Symbol::Divider:
    case Symbol::Equation:
      p.circle(16, 16, 11, fill);
      if (!fill) {
        if (spec.symbol == Symbol::Multiplier) {
          p.line(11.5f, 11.5f, 20.5f, 20.5f);
          p.line(20.5f, 11.5f, 11.5f, 20.5f);
        } else if (spec.symbol == Symbol::Divider) {
          p.line(10.5f, 16, 21.5f, 16);
          p.dot(16, 11.5f, 1.4f);
          p.dot(16, 20.5f, 1.4f);
        } else {
          p.line(11, 13.5f, 21, 13.5f);
          p.line(11, 18.5f, 21, 18.5f);
        }
      }
      break;
    case Symbol::Oscillator:
      p.circle(16, 16, 11, fill);
      if (!fill) glyphWave(p, spec.glyph, 16, 16, 6.5f, 5.0f);
      break;
    case Symbol::Delay:
      p.rect(8, 2, w - 8, h - 2, 0, fill);
      if (!fill) {
        if (spec.glyph == Glyph::UnitDelay) {
          // z⁻¹: a "z" and a small "-1"
          const float cy = h * 0.5f;
          p.begin();
          p.add(13, cy - 4);
          p.add(19, cy - 4);
          p.add(13, cy + 4);
          p.add(19, cy + 4);
          p.stroke();
          p.line(21, cy - 6, 24, cy - 6);
          p.begin();
          p.add(25.5f, cy - 7.5f);
          p.add(27, cy - 8.5f);
          p.add(27, cy - 3);
          p.stroke();
        } else {
          // Δ near the top (the time is written under it)
          const float cx = w * 0.5f;
          p.begin();
          p.add(cx, 6);
          p.add(cx + 5, 14);
          p.add(cx - 5, 14);
          p.add(cx, 6);
          p.stroke();
        }
      }
      break;
    case Symbol::Filter:
      p.rect(8, 0, w - 8, h, 2, fill);
      if (!fill) glyphResponse(p, spec.glyph, 14, 5, w - 14, 19);
      break;
    case Symbol::Mux:
      p.begin();
      p.add(8, 0);
      p.add(w - 8, 6);
      p.add(w - 8, h - 6);
      p.add(8, h);
      shape();
      break;
    case Symbol::Ic:
      p.rect(8, 0, w - 8, h, 0, fill);
      if (!fill) {
        // the pin-1 notch
        p.begin();
        p.arc(w * 0.5f, 0, 3, 0, kPi, 8);
        p.stroke();
      }
      break;
    case Symbol::Subcircuit:
      p.rect(8, 0, w - 8, h, 2, fill);
      if (!fill) p.line(8, 16, w - 8, 16);
      break;
    case Symbol::Box:
    case Symbol::Unknown:
      p.rect(8, 2, w - 8, h - 2, 3, fill);
      break;
    case Symbol::Const:
    case Symbol::Source:
      p.rect(0, 1, w - 4, 15, 7, fill);
      break;
    case Symbol::Param:
      p.rect(0, 1, w - 4, 15, 7, fill);
      if (!fill) {
        p.circle(8, 8, 3.5f, false);
        p.line(8, 8, 10.5f, 5.5f);
      }
      break;
    case Symbol::Input:
      p.begin();
      p.add(0, 1);
      p.add(w - 10, 1);
      p.add(w - 4, 8);
      p.add(w - 10, 15);
      p.add(0, 15);
      shape();
      break;
    case Symbol::Output:
      p.begin();
      p.add(4, 1);
      p.add(w - 6, 1);
      p.add(w, 8);
      p.add(w - 6, 15);
      p.add(4, 15);
      shape();
      break;
    case Symbol::Export:
      p.rect(4, 1, w - 2, 15, 3, fill);
      if (!fill) p.circle(w - 9, 8, 3, false);
      break;
  }
}

}  // namespace pq::views::circuit
