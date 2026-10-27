// The circuit canvas's scene model (T4.2/T4.3): placed symbols, orthogonal wires with styles, junctions, labels, the
// spatial grid. See pq/views/circuit/circuit_scene.h.

#include "pq/views/circuit/circuit_scene.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "pq/graph/atoms.h"

namespace pq::views::circuit {

using graph::NodeIndex;
using graph::NodeKind;
using graph::Point2;
using graph::WireIndex;

namespace {

struct PortStyle {
  graph::Kind kind = graph::Kind::Real;
  WireRate rate = WireRate::Const;
  bool poly = false;
};

WireRate fromRate(graph::Rate r, WireRate fallback) noexcept {
  switch (r) {
    case graph::Rate::Const: return WireRate::Const;
    case graph::Rate::Param: return WireRate::Param;
    case graph::Rate::Audio:
    case graph::Rate::Event: return WireRate::Audio;
    case graph::Rate::Infer: break;
  }
  return fallback;
}

std::string constantText(const graph::Constant& c) {
  if (c.kind == graph::Kind::Bool) return c.literal != 0.0 ? "true" : "false";
  char buf[48];
  const double v = c.literal;
  if (v == std::floor(v) && std::fabs(v) < 1e9) std::snprintf(buf, sizeof buf, "%.0f", v);
  else std::snprintf(buf, sizeof buf, "%.4g", v);
  return std::string(buf) + std::string(graph::unitSuffix(c.unit));
}

// Orthogonal polyline from `pts` (source pin first, target pin last): a corner wherever two consecutive points differ
// in both coordinates — leaving the source and entering the target horizontally, horizontal-first in between.
void orthogonalise(std::vector<Point2>& pts) {
  std::vector<Point2> out;
  out.reserve(pts.size() + 4);
  for (size_t i = 0; i < pts.size(); ++i) {
    const Point2 b = pts[i];
    if (!out.empty()) {
      const Point2 a = out.back();
      if (a.x != b.x && a.y != b.y) {
        const bool first = out.size() == 1, last = i + 1 == pts.size();
        if (first && last) {  // pin to pin: a Z through the middle
          const float mid = std::round((a.x + b.x) * 0.5f);
          out.push_back({mid, a.y});
          out.push_back({mid, b.y});
        } else if (last) {
          out.push_back({a.x, b.y});  // enter the target horizontally
        } else {
          out.push_back({b.x, a.y});  // leave horizontally
        }
      }
    }
    if (out.empty() || !(out.back() == b)) out.push_back(b);
  }
  // Drop points in the middle of straight runs.
  std::vector<Point2> s;
  s.reserve(out.size());
  for (const Point2& q : out) {
    if (s.size() >= 2) {
      const Point2& a = s[s.size() - 2];
      const Point2& b = s.back();
      if ((a.x == b.x && b.x == q.x) || (a.y == b.y && b.y == q.y)) {
        s.back() = q;
        continue;
      }
    }
    s.push_back(q);
  }
  pts = std::move(s);
}

uint8_t dirMask(Point2 from, Point2 to) noexcept {  // 1 right, 2 left, 4 down, 8 up
  if (to.x > from.x) return 1;
  if (to.x < from.x) return 2;
  if (to.y > from.y) return 4;
  if (to.y < from.y) return 8;
  return 0;
}

}  // namespace

void SpatialGrid::cellRange(float wx0, float wy0, float wx1, float wy1, uint32_t& c0, uint32_t& r0, uint32_t& c1,
                            uint32_t& r1) const noexcept {
  auto clampCol = [&](float x) {
    const float c = std::floor((x - x0) / cell);
    return static_cast<uint32_t>(std::clamp(c, 0.0f, static_cast<float>(cols ? cols - 1 : 0)));
  };
  auto clampRow = [&](float y) {
    const float r = std::floor((y - y0) / cell);
    return static_cast<uint32_t>(std::clamp(r, 0.0f, static_cast<float>(rows ? rows - 1 : 0)));
  };
  c0 = clampCol(wx0);
  c1 = clampCol(wx1);
  r0 = clampRow(wy0);
  r1 = clampRow(wy1);
}

namespace {

std::vector<PortStyle> portStyles(const graph::Patch& p) {
  const uint32_t n = p.nodeCount();
  const graph::Adjacency adj = p.adjacency();
  std::vector<PortStyle> out(p.portCount());  // per flat output port
  auto driverStyle = [&](NodeIndex v, uint32_t i, PortStyle& s) {
    const WireIndex w = adj.driver[p.flatInput(v, i)];
    if (w == graph::kNone) return false;
    const graph::Wire& wr = p.wire(w);
    s = out[p.flatOutput(wr.from.node, wr.from.port)];
    return true;
  };
  // Monotone (rates and poly only rise), so a few passes in node order reach the fixpoint through feedback loops.
  for (int pass = 0; pass < 16; ++pass) {
    bool changed = false;
    for (NodeIndex v = 0; v < n; ++v) {
      const graph::Node& nd = p.node(v);
      WireRate maxRate = WireRate::Const;
      bool anyPoly = false;
      graph::Kind dataKind = graph::Kind::Infer;
      for (uint32_t i = 0; i < nd.numInputs; ++i) {
        PortStyle s;
        if (!driverStyle(v, i, s)) continue;
        maxRate = std::max(maxRate, s.rate);
        anyPoly |= s.poly;
        if (dataKind == graph::Kind::Infer && p.inputType(v, i).kind == graph::Kind::Infer) dataKind = s.kind;
      }
      for (uint32_t k = 0; k < nd.numOutputs; ++k) {
        const graph::PortType t = p.outputType(v, k);
        PortStyle s;
        switch (nd.kind) {
          case NodeKind::Const:
            s.kind = p.constant(v).kind;
            s.rate = WireRate::Const;
            break;
          case NodeKind::Param:
            s.kind = t.kind == graph::Kind::Bool ? graph::Kind::Bool : graph::Kind::Real;
            s.rate = WireRate::Param;
            break;
          case NodeKind::Input:
            s.kind = t.kind == graph::Kind::Bool ? graph::Kind::Bool : graph::Kind::Real;
            s.rate = fromRate(t.rate, WireRate::Audio);
            s.poly = t.poly == graph::Poly::Voice;
            break;
          case NodeKind::Unknown:
            s.rate = WireRate::Audio;
            s.poly = anyPoly;
            break;
          case NodeKind::Atom: {
            const graph::AtomSignature& sig = graph::atomSignature(nd.atom);
            s.kind = t.kind != graph::Kind::Infer ? t.kind : (dataKind == graph::Kind::Bool ? graph::Kind::Bool : graph::Kind::Real);
            s.rate = fromRate(t.rate, sig.has(graph::atom_flags::kStateful) ? WireRate::Audio : maxRate);
            s.poly = t.poly == graph::Poly::Voice || (t.poly != graph::Poly::Global && anyPoly);
            break;
          }
          case NodeKind::Subcircuit:
            s.kind = t.kind == graph::Kind::Bool ? graph::Kind::Bool : graph::Kind::Real;
            s.rate = fromRate(t.rate, maxRate);
            s.poly = t.poly == graph::Poly::Voice || (t.poly != graph::Poly::Global && anyPoly);
            break;
          default: break;
        }
        PortStyle& cur = out[p.flatOutput(v, k)];
        const PortStyle merged{s.kind, std::max(cur.rate, s.rate), cur.poly || s.poly};
        if (merged.kind != cur.kind || merged.rate != cur.rate || merged.poly != cur.poly) {
          cur = merged;
          changed = true;
        }
      }
    }
    if (!changed) break;
  }
  return out;
}

}  // namespace

std::vector<WireStyle> outputStyles(const graph::Patch& p) {
  const std::vector<PortStyle> ps = portStyles(p);
  std::vector<WireStyle> styles(ps.size());
  for (graph::NodeIndex v = 0; v < p.nodeCount(); ++v)
    for (uint32_t k = 0; k < p.node(v).numOutputs; ++k) {
      const uint32_t o = p.flatOutput(v, k);
      styles[o].boolean = ps[o].kind == graph::Kind::Bool;
      styles[o].rate = ps[o].rate;
      styles[o].poly = ps[o].poly;
    }
  return styles;
}

std::vector<WireStyle> wireStyles(const graph::Patch& p) {
  const std::vector<PortStyle> out = portStyles(p);
  std::vector<WireStyle> styles(p.wireCount());
  for (WireIndex w = 0; w < p.wireCount(); ++w) {
    const graph::Wire& wr = p.wire(w);
    const PortStyle& s = out[p.flatOutput(wr.from.node, wr.from.port)];
    WireStyle& ws = styles[w];
    ws.boolean = s.kind == graph::Kind::Bool;
    ws.rate = s.rate;
    ws.poly = s.poly;
    if (p.node(wr.to.node).kind == NodeKind::Subcircuit) {
      const graph::PortType in = p.inputType(wr.to.node, wr.to.port);
      ws.modulation = in.rate == graph::Rate::Param && p.inputOptional(wr.to.node, wr.to.port) && s.rate == WireRate::Audio;
    }
  }
  return styles;
}

graph::AutoLayoutStats tidy(graph::Patch& patch, const graph::AutoLayoutOptions& options) {
  const std::vector<SymbolSpec> specs = describeAll(patch);
  return graph::autoLayout(patch, geometryOf(patch, specs), options);
}

void indexScene(CircuitScene& sc, const float* include) {
  sc.junctions.clear();
  sc.junctionWire.clear();
  uint32_t nets = 0;
  for (const SceneWire& sw : sc.wires) nets = std::max(nets, sw.net + 1);
  // Junctions: per net, every segment end where three or more directions meet.
  {
    std::vector<std::vector<uint32_t>> netSegs(nets);
    for (uint32_t s = 0; s < sc.segments.size(); ++s) netSegs[sc.wires[sc.segments[s].wire].net].push_back(s);
    std::vector<Point2> cand;
    for (const auto& segs : netSegs) {
      if (segs.size() < 3) continue;
      cand.clear();
      for (const uint32_t s : segs) {
        const uint32_t i = sc.segments[s].point;
        cand.push_back(sc.points[i]);
        cand.push_back(sc.points[i + 1]);
      }
      std::sort(cand.begin(), cand.end(), [](const Point2& a, const Point2& b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
      cand.erase(std::unique(cand.begin(), cand.end()), cand.end());
      for (const Point2& c : cand) {
        uint8_t mask = 0;
        for (const uint32_t s : segs) {
          const Point2 a = sc.points[sc.segments[s].point], b = sc.points[sc.segments[s].point + 1];
          if (a == c) mask |= dirMask(c, b);
          else if (b == c) mask |= dirMask(c, a);
          else if (a.x == b.x && a.x == c.x && c.y > std::min(a.y, b.y) && c.y < std::max(a.y, b.y)) mask |= 4 | 8;
          else if (a.y == b.y && a.y == c.y && c.x > std::min(a.x, b.x) && c.x < std::max(a.x, b.x)) mask |= 1 | 2;
        }
        const int dirs = ((mask & 1) != 0) + ((mask & 2) != 0) + ((mask & 4) != 0) + ((mask & 8) != 0);
        if (dirs >= 3) {
          sc.junctions.push_back(c);
          sc.junctionWire.push_back(sc.segments[segs[0]].wire);
        }
      }
    }
  }

  // Bounds (labels above nodes included through a margin).
  bool any = false;
  if (include != nullptr) {
    sc.x0 = include[0], sc.y0 = include[1], sc.x1 = include[2], sc.y1 = include[3];
    any = true;
  }
  auto grow = [&](float ax0, float ay0, float ax1, float ay1) {
    if (!any) {
      sc.x0 = ax0, sc.y0 = ay0, sc.x1 = ax1, sc.y1 = ay1;
      any = true;
      return;
    }
    sc.x0 = std::min(sc.x0, ax0), sc.y0 = std::min(sc.y0, ay0), sc.x1 = std::max(sc.x1, ax1), sc.y1 = std::max(sc.y1, ay1);
  };
  for (const SceneNode& sn : sc.nodes)
    if (!sn.hidden) grow(sn.x, sn.y - 12, sn.x + sn.w, sn.y + sn.h);
  for (const SceneWire& sw : sc.wires)
    if (!sw.hidden) grow(sw.x0, sw.y0, sw.x1, sw.y1);
  for (const SceneLabel& lb : sc.labels)
    if (lb.role == SceneLabel::Role::Comment) grow(lb.x0, lb.y - 8, lb.x0 + 6.0f * static_cast<float>(sc.strings[lb.text].size()), lb.y + 8);

  // Spatial grid.
  SpatialGrid& g = sc.grid;
  g.x0 = sc.x0;
  g.y0 = sc.y0;
  g.cols = std::max<uint32_t>(1, static_cast<uint32_t>(std::ceil((sc.x1 - sc.x0 + 1) / g.cell)));
  g.rows = std::max<uint32_t>(1, static_cast<uint32_t>(std::ceil((sc.y1 - sc.y0 + 1) / g.cell)));
  const uint32_t cells = g.cols * g.rows;
  auto fill = [&](std::vector<uint32_t>& begin, std::vector<uint32_t>& items, uint32_t count, auto&& boxOf) {
    begin.assign(cells + 1, 0);
    for (int pass = 0; pass < 2; ++pass) {
      std::vector<uint32_t> cursor;
      if (pass == 1) {
        for (uint32_t c = 0; c < cells; ++c) begin[c + 1] += begin[c];
        items.resize(begin[cells]);
        cursor.assign(begin.begin(), begin.end() - 1);
      }
      for (uint32_t i = 0; i < count; ++i) {
        float bx0, by0, bx1, by1;
        if (!boxOf(i, bx0, by0, bx1, by1)) continue;  // hidden
        uint32_t c0, r0, c1, r1;
        g.cellRange(bx0, by0, bx1, by1, c0, r0, c1, r1);
        for (uint32_t r = r0; r <= r1; ++r)
          for (uint32_t c = c0; c <= c1; ++c) {
            const uint32_t cell = r * g.cols + c;
            if (pass == 0) ++begin[cell + 1];
            else items[cursor[cell]++] = i;
          }
      }
    }
  };
  fill(g.nodeBegin, g.nodeItems, static_cast<uint32_t>(sc.nodes.size()), [&](uint32_t i, float& bx0, float& by0, float& bx1, float& by1) {
    const SceneNode& sn = sc.nodes[i];
    bx0 = sn.x, by0 = sn.y - 12, bx1 = sn.x + sn.w, by1 = sn.y + sn.h;
    return !sn.hidden;
  });
  fill(g.segBegin, g.segItems, static_cast<uint32_t>(sc.segments.size()),
       [&](uint32_t i, float& bx0, float& by0, float& bx1, float& by1) {
         const Point2 a = sc.points[sc.segments[i].point], b = sc.points[sc.segments[i].point + 1];
         bx0 = std::min(a.x, b.x), by0 = std::min(a.y, b.y), bx1 = std::max(a.x, b.x), by1 = std::max(a.y, b.y);
         return true;
       });
}

graph::AutoLayoutStats layoutSubset(graph::Patch& p, std::span<const uint8_t> keep, const graph::AutoLayoutOptions& options) {
  const uint32_t n = p.nodeCount();
  if (keep.size() != n) return {};
  // The symbols as the whole patch draws them (an arm whose driver is hidden keeps its pin).
  const std::vector<SymbolSpec> specs = describeAll(p);
  const graph::LayoutGeometry full = geometryOf(p, specs);
  graph::Patch sub = p;
  std::vector<NodeIndex> drop, oldOf;
  for (NodeIndex v = 0; v < n; ++v) {
    if (keep[v]) oldOf.push_back(v);
    else drop.push_back(v);
  }
  if (oldOf.empty()) return {};
  sub.eraseNodes(drop);  // keeps the relative order: kept node k is oldOf[k]
  graph::LayoutGeometry g;
  g.shape.resize(sub.nodeCount());
  g.inPinY.assign(sub.portCount(), 0.0f);
  g.outPinY.assign(sub.portCount(), 0.0f);
  for (NodeIndex k = 0; k < sub.nodeCount(); ++k) {
    const NodeIndex v = oldOf[k];
    g.shape[k] = full.shape[v];
    for (uint32_t i = 0; i < sub.node(k).numInputs; ++i) g.inPinY[sub.flatInput(k, i)] = full.inPin(p, v, i);
    for (uint32_t j = 0; j < sub.node(k).numOutputs; ++j) g.outPinY[sub.flatOutput(k, j)] = full.outPin(p, v, j);
  }
  const graph::AutoLayoutStats st = graph::autoLayout(sub, g, options);
  for (NodeIndex k = 0; k < sub.nodeCount(); ++k) p.setLayout(oldOf[k], sub.layout(k));
  // Wires by their target port (an input has one driver).
  std::unordered_map<uint64_t, WireIndex> byTarget;
  for (WireIndex w = 0; w < p.wireCount(); ++w) {
    const graph::Wire& wr = p.wire(w);
    byTarget.emplace((static_cast<uint64_t>(wr.to.node) << 16) | wr.to.port, w);
  }
  for (WireIndex w = 0; w < sub.wireCount(); ++w) {
    const graph::Wire& wr = sub.wire(w);
    const auto it = byTarget.find((static_cast<uint64_t>(oldOf[wr.to.node]) << 16) | wr.to.port);
    if (it == byTarget.end()) continue;
    const std::span<const Point2> via = sub.wireVia(w);
    p.setWireVia(it->second, std::vector<Point2>(via.begin(), via.end()));
  }
  return st;
}

std::shared_ptr<CircuitScene> buildScene(const graph::Patch& source, const SceneOptions& options, graph::Patch* laidOut) {
  auto scene = std::make_shared<CircuitScene>();
  CircuitScene& sc = *scene;
  graph::Patch p = source;
  sc.patchName = p.name();
  sc.specs = describeAll(p);
  const graph::LayoutGeometry geo = geometryOf(p, sc.specs);
  sc.autoPlaced = graph::completeLayout(p, geo, options.layout);
  const graph::Adjacency adj = p.adjacency();
  const uint32_t n = p.nodeCount();
  // T4.15: inactive and hidden parts.
  const bool anyInactive = options.inactiveNodes.size() == n && options.inactiveWires.size() == p.wireCount() &&
                           std::find(options.inactiveNodes.begin(), options.inactiveNodes.end(), uint8_t{1}) != options.inactiveNodes.end();
  const bool hide = anyInactive && options.hideInactive;
  auto nodeInactive = [&](NodeIndex v) { return anyInactive && options.inactiveNodes[v] != 0; };
  auto wireInactive = [&](WireIndex w) { return anyInactive && options.inactiveWires[w] != 0; };
  if (hide && options.relayoutVisible) {
    std::vector<uint8_t> keep(n);
    for (NodeIndex v = 0; v < n; ++v) keep[v] = nodeInactive(v) ? 0 : 1;
    (void)layoutSubset(p, keep, options.layout);
  }

  auto addString = [&](std::string s) {
    sc.strings.push_back(std::move(s));
    return static_cast<uint32_t>(sc.strings.size() - 1);
  };

  // Nodes and pins.
  sc.nodes.resize(n);
  for (NodeIndex v = 0; v < n; ++v) {
    const SymbolSpec& spec = sc.specs[v];
    const graph::NodeLayout& l = p.layout(v);
    SceneNode& sn = sc.nodes[v];
    sn.x = l.x;
    sn.y = l.y;
    sn.w = spec.w;
    sn.h = spec.h;
    sn.spec = v;
    sn.node = v;
    sn.inactive = nodeInactive(v);
    sn.hidden = hide && sn.inactive;
    sc.hiddenNodes += sn.hidden ? 1u : 0u;
    sn.firstPin = static_cast<uint32_t>(sc.pins.size());
    for (uint32_t i = 0; i < spec.inputs.size(); ++i) {
      ScenePin pin;
      pin.x = l.x;
      pin.y = l.y + spec.inputs[i].y;
      pin.stub = spec.inputs[i].stub;
      pin.node = v;
      pin.port = static_cast<uint16_t>(i);
      pin.input = true;
      pin.visible = spec.inputs[i].visible;
      pin.connected = adj.driver[p.flatInput(v, i)] != graph::kNone;
      if (!spec.inputs[i].label.empty() && pin.visible) pin.label = addString(spec.inputs[i].label);
      sc.pins.push_back(pin);
    }
    for (uint32_t i = 0; i < spec.outputs.size(); ++i) {
      ScenePin pin;
      pin.x = l.x + spec.w;
      pin.y = l.y + spec.outputs[i].y;
      pin.stub = spec.outputs[i].stub;
      pin.node = v;
      pin.port = static_cast<uint16_t>(i);
      pin.input = false;
      pin.visible = spec.outputs[i].visible;
      pin.connected = !adj.outgoing(v).empty();
      if (!spec.outputs[i].label.empty() && pin.visible) pin.label = addString(spec.outputs[i].label);
      sc.pins.push_back(pin);
    }
    sn.pinCount = static_cast<uint32_t>(sc.pins.size()) - sn.firstPin;
  }
  // Outputs are connected per port.
  {
    std::vector<uint8_t> outUsed(p.portCount(), 0);
    for (const graph::Wire& wr : p.wires()) outUsed[p.flatOutput(wr.from.node, wr.from.port)] = 1;
    for (NodeIndex v = 0; v < n; ++v) {
      const SceneNode& sn = sc.nodes[v];
      for (uint32_t k = 0; k < sn.pinCount; ++k) {
        ScenePin& pin = sc.pins[sn.firstPin + k];
        if (!pin.input) pin.connected = outUsed[p.flatOutput(v, pin.port)] != 0;
      }
    }
  }

  // Labels.
  using Align = SceneLabel::Align;
  using Role = SceneLabel::Role;
  for (NodeIndex v = 0; v < n; ++v) {
    const SymbolSpec& spec = sc.specs[v];
    const SceneNode& sn = sc.nodes[v];
    if (sn.hidden) continue;
    std::string title = spec.title;
    float tx0 = sn.x + 8, tx1 = sn.x + sn.w - 8, ty = sn.y + sn.h * 0.5f;
    switch (spec.symbol) {
      case Symbol::Box:
      case Symbol::Unknown: break;
      case Symbol::Ic: ty = sn.y + 10; break;  // below the pin-1 notch
      case Symbol::Subcircuit: ty = sn.y + 8; break;
      case Symbol::Filter: title.clear(); break;  // the glyph says it; the callee name goes in the caption line
      case Symbol::Const: tx0 = sn.x + 1, tx1 = sn.x + sn.w - 5; break;
      case Symbol::Source: tx0 = sn.x + 4, tx1 = sn.x + sn.w - 6; break;
      case Symbol::Param: tx0 = sn.x + 14, tx1 = sn.x + sn.w - 6; break;
      case Symbol::Input: tx0 = sn.x + 3, tx1 = sn.x + sn.w - 10; break;
      case Symbol::Output: tx0 = sn.x + 7, tx1 = sn.x + sn.w - 6; break;
      case Symbol::Export: tx0 = sn.x + 7, tx1 = sn.x + sn.w - 13; break;
      case Symbol::Delay: {
        title.clear();
        if (spec.glyph == Glyph::DelayLine) {  // labelled with the delay: the literal at `time`, if any
          const graph::Node& nd = p.node(v);
          if (nd.numInputs > 1) {
            const WireIndex w = adj.driver[p.flatInput(v, 1)];
            if (w != graph::kNone && p.node(p.wire(w).from.node).kind == NodeKind::Const)
              title = constantText(p.constant(p.wire(w).from.node));
            else if (w != graph::kNone && !p.nodeName(p.wire(w).from.node).empty())
              title = p.nodeName(p.wire(w).from.node);
          }
          ty = sn.y + 22;
          tx0 = sn.x + 9, tx1 = sn.x + sn.w - 9;
        }
        break;
      }
      default: title.clear(); break;  // gates, op-amps, circles, mux: the symbol is the label
    }
    if (!title.empty()) {
      SceneLabel lb;
      lb.x0 = tx0;
      lb.x1 = tx1;
      lb.y = ty;
      lb.text = addString(std::move(title));
      lb.align = (spec.symbol == Symbol::Input || spec.symbol == Symbol::Source || spec.symbol == Symbol::Param) ? Align::Left
                                                                                                                 : Align::Center;
      lb.role = Role::Title;
      lb.node = v;
      sc.labels.push_back(lb);
    }
    // Caption above: the binding name, or a filter's callee.
    std::string caption = spec.caption;
    if (spec.symbol == Symbol::Filter) caption = caption.empty() ? spec.title : caption + ": " + spec.title;
    if (!caption.empty()) {
      SceneLabel lb;
      lb.x0 = sn.x - 24;  // a caption may be a little wider than its symbol (layers are ≥ 48 apart)
      lb.x1 = sn.x + sn.w + 24;
      lb.y = sn.y - 6;
      lb.text = addString(std::move(caption));
      lb.role = Role::Caption;
      lb.node = v;
      sc.labels.push_back(lb);
    }
    // Pin labels inside labelled symbols.
    for (uint32_t k = 0; k < sn.pinCount; ++k) {
      const ScenePin& pin = sc.pins[sn.firstPin + k];
      if (pin.label == UINT32_MAX) continue;
      SceneLabel lb;
      const float half = (sn.w - 16) * 0.5f;
      lb.x0 = pin.input ? sn.x + 11 : sn.x + sn.w - 8 - half;
      lb.x1 = pin.input ? sn.x + 8 + half : sn.x + sn.w - 11;
      lb.y = pin.y;
      lb.text = pin.label;
      lb.align = pin.input ? Align::Left : Align::Right;
      lb.role = Role::Pin;
      lb.node = v;
      sc.labels.push_back(lb);
    }
  }
  for (const graph::LayoutComment& c : p.comments()) {
    SceneLabel lb;
    lb.x0 = c.at.x;
    lb.x1 = c.at.x + 6.0f * static_cast<float>(c.text.size()) + 400.0f;
    lb.y = c.at.y;
    lb.text = addString(c.text);
    lb.align = Align::Left;
    lb.role = Role::Comment;
    sc.labels.push_back(lb);
  }

  // Wires.
  const std::vector<WireStyle> styles = wireStyles(p);
  std::unordered_map<uint32_t, uint32_t> netOf;  // flat output port → net
  std::vector<Point2> pts;
  sc.wires.resize(p.wireCount());
  for (WireIndex w = 0; w < p.wireCount(); ++w) {
    const graph::Wire& wr = p.wire(w);
    const SceneNode& a = sc.nodes[wr.from.node];
    const SceneNode& b = sc.nodes[wr.to.node];
    const uint32_t fromPin = a.firstPin + static_cast<uint32_t>(sc.specs[wr.from.node].inputs.size()) + wr.from.port;
    const uint32_t toPin = b.firstPin + wr.to.port;
    SceneWire& sw = sc.wires[w];
    sw.style = styles[w];
    sw.wire = w;
    sw.fromPin = fromPin;
    sw.toPin = toPin;
    sw.inactive = wireInactive(w);
    sw.hidden = hide && sw.inactive;
    sw.net = netOf.emplace(p.flatOutput(wr.from.node, wr.from.port), static_cast<uint32_t>(netOf.size())).first->second;
    if (sw.hidden) {  // no points: nothing draws or hits it
      ++sc.hiddenWires;
      sw.firstPoint = static_cast<uint32_t>(sc.points.size());
      sw.x0 = sw.x1 = sc.pins[fromPin].x;
      sw.y0 = sw.y1 = sc.pins[fromPin].y;
      continue;
    }
    pts.clear();
    pts.push_back({sc.pins[fromPin].x, sc.pins[fromPin].y});
    for (const Point2& q : p.wireVia(w)) pts.push_back(q);
    pts.push_back({sc.pins[toPin].x, sc.pins[toPin].y});
    orthogonalise(pts);
    sw.firstPoint = static_cast<uint32_t>(sc.points.size());
    sw.pointCount = static_cast<uint32_t>(pts.size());
    sw.x0 = sw.x1 = pts[0].x;
    sw.y0 = sw.y1 = pts[0].y;
    for (const Point2& q : pts) {
      sw.x0 = std::min(sw.x0, q.x), sw.x1 = std::max(sw.x1, q.x);
      sw.y0 = std::min(sw.y0, q.y), sw.y1 = std::max(sw.y1, q.y);
    }
    for (uint32_t i = 0; i + 1 < pts.size(); ++i) sc.segments.push_back({w, sw.firstPoint + i});
    sc.points.insert(sc.points.end(), pts.begin(), pts.end());
  }

  indexScene(sc);
  if (laidOut) *laidOut = std::move(p);
  return scene;
}

}  // namespace pq::views::circuit
