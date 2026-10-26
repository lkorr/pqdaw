// The project patch's scene for the circuit canvas (T4.13): model nodes as schematic symbols, model edges as styled
// orthogonal wires, expanded devices opened to their own scenes. See pq/views/project_patch/project_patch_view.h.

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "pq/views/project_patch/project_patch_view.h"

namespace pq::views::project_patch {

using circuit::CircuitScene;
using circuit::Glyph;
using circuit::PinSpec;
using circuit::SceneLabel;
using circuit::ScenePin;
using circuit::SceneWire;
using circuit::Symbol;
using circuit::SymbolSpec;
using circuit::WireRate;
using circuit::WireStyle;
using graph::Point2;

namespace {

uint64_t bodyKeyOf(const SymbolSpec& s) noexcept {
  return static_cast<uint64_t>(s.symbol) | (static_cast<uint64_t>(s.glyph) << 8) | (static_cast<uint64_t>(s.w / 4.0f) << 16) |
         (static_cast<uint64_t>(s.h / 4.0f) << 36);
}

float pinYOf(const Node& n, size_t i) noexcept {
  switch (n.kind) {
    case NodeKind::Sum: return 16.0f;
    case NodeKind::Output: return 8.0f;
    default: return metrics::kPinTop + metrics::kPinPitch * static_cast<float>(i);
  }
}

SymbolSpec specOf(const Node& n) {
  SymbolSpec s;
  s.w = n.w;
  s.h = n.h;
  s.title = n.title;
  float inStub = 8.0f, outStub = 8.0f;
  switch (n.kind) {
    case NodeKind::Source: s.symbol = Symbol::Ic; break;
    case NodeKind::Sum:
      s.symbol = Symbol::Summer;
      s.glyph = Glyph::Plus;
      outStub = 6.0f;
      break;
    case NodeKind::Device:
    case NodeKind::Mixer: s.symbol = Symbol::Subcircuit; break;
    case NodeKind::Output:
      s.symbol = Symbol::Output;
      inStub = 4.0f;
      break;
  }
  for (size_t i = 0; i < n.inputs.size(); ++i) {
    PinSpec p;
    p.y = pinYOf(n, i);
    p.stub = inStub;
    p.label = n.kind == NodeKind::Device || n.kind == NodeKind::Mixer ? n.inputs[i].label : std::string();
    s.inputs.push_back(std::move(p));
  }
  for (size_t i = 0; i < n.outputs.size(); ++i) {
    PinSpec p;
    p.y = pinYOf(n, i);
    p.stub = outStub;
    p.label = n.kind == NodeKind::Device || n.kind == NodeKind::Mixer ? n.outputs[i].label : std::string();
    s.outputs.push_back(std::move(p));
  }
  s.bodyKey = bodyKeyOf(s);
  return s;
}

WireStyle styleOf(const Edge& e) {
  WireStyle w;
  w.rate = WireRate::Audio;
  switch (e.kind) {
    case EdgeKind::Notes: w.poly = true; break;
    case EdgeKind::Chain:
    case EdgeKind::Route: break;
    case EdgeKind::Send:
      if (e.silent) w.rate = WireRate::Const;  // lowered, at −∞: drawn dim
      break;
    case EdgeKind::Modulation:
      w.modulation = true;
      if (e.status == doc::WireStatus::Inactive || e.status == doc::WireStatus::Refused) w.rate = WireRate::Const;
      break;
  }
  return w;
}

// Orthogonal route from a to b: horizontal first, through the middle.
void zRoute(std::vector<Point2>& pts, Point2 a, Point2 b) {
  pts.push_back(a);
  if (a.y != b.y) {
    const float mx = std::round((a.x + b.x) * 0.5f);
    pts.push_back({mx, a.y});
    pts.push_back({mx, b.y});
  }
  pts.push_back(b);
}

}  // namespace

ProjectScene buildProjectScene(const ProjectPatch& pp, std::span<const InnerScene> inner) {
  ProjectScene out;
  out.scene = std::make_shared<CircuitScene>();
  CircuitScene& sc = *out.scene;
  sc.patchName = "project";
  out.sceneNode.assign(pp.nodes.size(), kNone);
  auto addString = [&](std::string s) {
    sc.strings.push_back(std::move(s));
    return static_cast<uint32_t>(sc.strings.size() - 1);
  };
  auto innerOf = [&](doc::DeviceId d) -> const CircuitScene* {
    for (const InnerScene& s : inner)
      if (s.device == d && s.scene) return s.scene.get();
    return nullptr;
  };

  // Model nodes.
  for (uint32_t i = 0; i < pp.nodes.size(); ++i) {
    const Node& n = pp.nodes[i];
    if (n.kind == NodeKind::Device && n.expanded && innerOf(n.device) != nullptr) continue;
    const uint32_t v = static_cast<uint32_t>(sc.nodes.size());
    out.sceneNode[i] = v;
    out.modelNode.push_back(i);
    sc.specs.push_back(specOf(n));
    const SymbolSpec& spec = sc.specs.back();
    circuit::SceneNode sn;
    sn.x = n.x;
    sn.y = n.y;
    sn.w = n.w;
    sn.h = n.h;
    sn.spec = v;
    sn.node = v;
    sn.firstPin = static_cast<uint32_t>(sc.pins.size());
    for (int side = 0; side < 2; ++side) {
      const std::vector<PinSpec>& pins = side == 0 ? spec.inputs : spec.outputs;
      for (size_t k = 0; k < pins.size(); ++k) {
        ScenePin pin;
        pin.input = side == 0;
        pin.x = side == 0 ? n.x : n.x + n.w;
        pin.y = n.y + pins[k].y;
        pin.stub = pins[k].stub;
        pin.node = v;
        pin.port = static_cast<uint16_t>(k);
        if (!pins[k].label.empty()) pin.label = addString(pins[k].label);
        sc.pins.push_back(pin);
      }
    }
    sn.pinCount = static_cast<uint32_t>(sc.pins.size()) - sn.firstPin;
    sc.nodes.push_back(sn);
    // Labels: titles as buildScene places them; pin labels inside blocks.
    SceneLabel title;
    title.node = v;
    title.text = addString(n.title);
    title.role = SceneLabel::Role::Title;
    title.x0 = n.x + 8.0f;
    title.x1 = n.x + n.w - 8.0f;
    switch (n.kind) {
      case NodeKind::Source: title.y = n.y + 10.0f; break;
      case NodeKind::Device:
      case NodeKind::Mixer: title.y = n.y + 8.0f; break;
      case NodeKind::Output:
        title.x0 = n.x + 7.0f;
        title.x1 = n.x + n.w - 6.0f;
        title.y = n.y + n.h * 0.5f;
        break;
      case NodeKind::Sum: title.node = UINT32_MAX; break;  // the symbol says it
    }
    if (title.node != UINT32_MAX) sc.labels.push_back(title);
    for (uint32_t k = 0; k < sn.pinCount; ++k) {
      const ScenePin& pin = sc.pins[sn.firstPin + k];
      if (pin.label == UINT32_MAX) continue;
      SceneLabel lb;
      const float half = (n.w - 16.0f) * 0.5f;
      lb.x0 = pin.input ? n.x + 11.0f : n.x + n.w - 8.0f - half;
      lb.x1 = pin.input ? n.x + 8.0f + half : n.x + n.w - 11.0f;
      lb.y = pin.y;
      lb.text = pin.label;
      lb.align = pin.input ? SceneLabel::Align::Left : SceneLabel::Align::Right;
      lb.role = SceneLabel::Role::Pin;
      lb.node = v;
      sc.labels.push_back(lb);
    }
  }

  // Model edges.
  std::unordered_map<uint32_t, uint32_t> nets;  // model net → scene net
  auto netOf = [&](uint32_t modelNet) { return nets.emplace(modelNet, static_cast<uint32_t>(nets.size())).first->second; };
  auto addWire = [&](const std::vector<Point2>& pts, WireStyle style, uint32_t net, uint32_t modelEdge) {
    SceneWire sw;
    sw.firstPoint = static_cast<uint32_t>(sc.points.size());
    sw.pointCount = static_cast<uint32_t>(pts.size());
    sw.style = style;
    sw.net = net;
    sw.wire = static_cast<graph::WireIndex>(sc.wires.size());
    sw.x0 = sw.x1 = pts[0].x;
    sw.y0 = sw.y1 = pts[0].y;
    for (const Point2& q : pts) {
      sw.x0 = std::min(sw.x0, q.x), sw.x1 = std::max(sw.x1, q.x);
      sw.y0 = std::min(sw.y0, q.y), sw.y1 = std::max(sw.y1, q.y);
    }
    const auto w = static_cast<uint32_t>(sc.wires.size());
    for (uint32_t i = 0; i + 1 < pts.size(); ++i) sc.segments.push_back({w, sw.firstPoint + i});
    sc.points.insert(sc.points.end(), pts.begin(), pts.end());
    sc.wires.push_back(sw);
    out.modelEdge.push_back(modelEdge);
  };
  for (uint32_t k = 0; k < pp.edges.size(); ++k) {
    const Edge& e = pp.edges[k];
    if (e.points.size() < 2) continue;
    addWire(e.points, styleOf(e), netOf(e.net), k);
    SceneWire& sw = sc.wires.back();
    const uint32_t a = out.sceneNode[e.from], b = out.sceneNode[e.to];
    sw.fromPin = a != kNone ? sc.nodes[a].firstPin + static_cast<uint32_t>(pp.nodes[e.from].inputs.size()) + e.fromPort : 0;
    sw.toPin = b != kNone ? sc.nodes[b].firstPin + e.toPort : 0;
  }
  // Pins are connected where an edge ends.
  for (const Edge& e : pp.edges) {
    const uint32_t a = out.sceneNode[e.from], b = out.sceneNode[e.to];
    if (a != kNone) sc.pins[sc.nodes[a].firstPin + pp.nodes[e.from].inputs.size() + e.fromPort].connected = true;
    if (b != kNone) sc.pins[sc.nodes[b].firstPin + e.toPort].connected = true;
  }

  // Expanded devices: the device patch's scene inside the block's frame, its terminals joined to the frame's pins.
  for (uint32_t i = 0; i < pp.nodes.size(); ++i) {
    const Node& n = pp.nodes[i];
    if (out.sceneNode[i] != kNone || n.kind != NodeKind::Device) continue;
    const CircuitScene* in = innerOf(n.device);
    if (in == nullptr) continue;
    const float ox = n.x + metrics::kExpandedPad - in->x0, oy = n.y + 16.0f + metrics::kExpandedPad - in->y0;
    const auto nodeBase = static_cast<uint32_t>(sc.nodes.size()), pinBase = static_cast<uint32_t>(sc.pins.size());
    const auto specBase = static_cast<uint32_t>(sc.specs.size()), stringBase = static_cast<uint32_t>(sc.strings.size());
    sc.specs.insert(sc.specs.end(), in->specs.begin(), in->specs.end());
    sc.strings.insert(sc.strings.end(), in->strings.begin(), in->strings.end());
    for (const circuit::SceneNode& s : in->nodes) {
      circuit::SceneNode d = s;
      d.x += ox;
      d.y += oy;
      d.spec += specBase;
      d.firstPin += pinBase;
      d.node = static_cast<graph::NodeIndex>(nodeBase + s.node);
      sc.nodes.push_back(d);
      out.modelNode.push_back(i);
    }
    for (const ScenePin& s : in->pins) {
      ScenePin d = s;
      d.x += ox;
      d.y += oy;
      d.node = static_cast<graph::NodeIndex>(nodeBase + s.node);
      if (d.label != UINT32_MAX) d.label += stringBase;
      sc.pins.push_back(d);
    }
    for (const SceneLabel& s : in->labels) {
      SceneLabel d = s;
      d.x0 += ox;
      d.x1 += ox;
      d.y += oy;
      d.text += stringBase;
      if (d.node != UINT32_MAX) d.node += nodeBase;
      sc.labels.push_back(d);
    }
    const uint32_t netBase = static_cast<uint32_t>(nets.size()) + 1;
    uint32_t maxNet = 0;
    std::vector<Point2> pts;
    for (const SceneWire& s : in->wires) {
      pts.assign(in->points.begin() + s.firstPoint, in->points.begin() + s.firstPoint + s.pointCount);
      for (Point2& q : pts) q = {q.x + ox, q.y + oy};
      addWire(pts, s.style, netBase + s.net, kNone);
      sc.wires.back().fromPin = s.fromPin + pinBase;
      sc.wires.back().toPin = s.toPin + pinBase;
      maxNet = std::max(maxNet, s.net);
    }
    for (uint32_t q = 0; q <= maxNet; ++q) netOf((1u << 31) | (netBase + q));  // keep later nets unique
    // Connectors: frame pin → the terminal of that name (inputs: `in`, params; outputs: `out`, exports).
    auto terminal = [&](std::string_view name, bool input) -> uint32_t {
      for (uint32_t v = 0; v < in->nodes.size(); ++v) {
        const SymbolSpec& s = in->specs[in->nodes[v].spec];
        const bool isIn = s.symbol == Symbol::Input || s.symbol == Symbol::Param;
        const bool isOut = s.symbol == Symbol::Output || s.symbol == Symbol::Export;
        if (s.title == name && (input ? isIn : isOut)) return nodeBase + v;
      }
      return kNone;
    };
    WireStyle conn;
    conn.rate = WireRate::Audio;
    for (size_t k = 0; k < n.inputs.size(); ++k) {
      const uint32_t t = terminal(n.inputs[k].name, true);
      if (t == kNone) continue;
      const circuit::SceneNode& tn = sc.nodes[t];
      pts.clear();
      zRoute(pts, {n.x, n.y + pinYOf(n, k)}, {tn.x, tn.y + tn.h * 0.5f});
      WireStyle s = conn;
      if (sc.specs[tn.spec].symbol == Symbol::Param) s.rate = WireRate::Param;
      addWire(pts, s, netOf((1u << 31) | (0x40000000u + nodeBase + static_cast<uint32_t>(k))), kNone);
    }
    for (size_t k = 0; k < n.outputs.size(); ++k) {
      const uint32_t t = terminal(n.outputs[k].name, false);
      if (t == kNone) continue;
      const circuit::SceneNode& tn = sc.nodes[t];
      pts.clear();
      zRoute(pts, {tn.x + tn.w, tn.y + tn.h * 0.5f}, {n.x + n.w, n.y + pinYOf(n, k)});
      addWire(pts, conn, netOf((1u << 31) | (0x50000000u + nodeBase + static_cast<uint32_t>(k))), kNone);
    }
  }

  const float include[4] = {pp.x0, pp.y0, pp.x1, pp.y1};
  circuit::indexScene(sc, pp.bands.empty() ? nullptr : include);
  return out;
}

}  // namespace pq::views::project_patch
