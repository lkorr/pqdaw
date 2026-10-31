// A track's device chain as one circuit (T5.x-circuit-chain): pq/views/circuit/chain_circuit.h.

#include "pq/views/circuit/chain_circuit.h"

#include <algorithm>
#include <cmath>

#include "pq/graph/patch_file.h"

namespace pq::views::circuit {

using graph::Point2;
namespace m = chain_metrics;

uint32_t ChainCircuit::boxOf(doc::DeviceId d) const noexcept {
  for (uint32_t i = 0; i < boxes.size(); ++i)
    if (boxes[i].kind != ChainBoxKind::Chain && boxes[i].device == d) return i;
  return kNoBox;
}

uint32_t ChainCircuit::boxAt(float x, float y) const noexcept {
  // A child's index is above its parent's and siblings do not overlap: the last box containing the point is the
  // innermost one.
  for (uint32_t i = static_cast<uint32_t>(boxes.size()); i-- > 0;)
    if (boxes[i].contains(x, y)) return i;
  return kNoBox;
}

uint32_t ChainCircuit::deviceBoxAt(float x, float y) const noexcept {
  uint32_t b = boxAt(x, y);
  while (b != kNoBox && boxes[b].kind == ChainBoxKind::Chain) b = boxes[b].parent;
  return b;
}

std::shared_ptr<const CircuitScene> ChainSceneCache::sceneOf(const std::string& key, const graph::Patch& callee) {
  for (const auto& [k, s] : entries_)
    if (k == key) return s;
  if (entries_.size() >= kMax) entries_.erase(entries_.begin());  // oldest first
  ++builds_;
  std::shared_ptr<const CircuitScene> s = buildScene(callee);
  entries_.emplace_back(key, s);
  return s;
}

namespace {

float roundUp4(float v) noexcept { return std::ceil(v / 4.0f) * 4.0f; }

uint64_t bodyKeyOf(const SymbolSpec& s) noexcept {
  return static_cast<uint64_t>(s.symbol) | (static_cast<uint64_t>(s.glyph) << 8) | (static_cast<uint64_t>(s.w / 4.0f) << 16) |
         (static_cast<uint64_t>(s.h / 4.0f) << 36);
}

// A one-node scene: an `in` or `out` terminal named `name` (its symbol and label exactly as a patch draws them).
std::shared_ptr<const CircuitScene> terminalScene(const std::string& name, bool input) {
  graph::Patch p("terminal");
  const graph::PortType t = graph::portType(graph::Kind::Real, graph::Rate::Audio);
  if (input) (void)p.addInput(name, t);
  else (void)p.addOutput(name, t);
  return buildScene(p);
}

class Builder {
public:
  Builder(const doc::Project& p, const ChainOptions& o) : p_(p), o_(o) {
    out_.scene = std::make_shared<CircuitScene>();
    out_.scene->patchName = "chain";
  }

  // ---- boxes --------------------------------------------------------------------------------------------------

  // The boxes of `list` (and, iteratively, of every rack in it) under `parent`.
  void addList(const std::vector<doc::DevicePtr>& list, uint32_t parent, uint16_t depth) {
    std::vector<std::pair<uint32_t, const doc::Device*>> racks;  // rack boxes still to open
    auto addAll = [&](const std::vector<doc::DevicePtr>& devices, uint32_t under, uint16_t level) {
      for (const doc::DevicePtr& d : devices) {
        const uint32_t b = addDevice(*d, under, level);
        if (under == kNoBox) out_.roots.push_back(b);
        else out_.boxes[under].children.push_back(b);
        if (d->rack) racks.emplace_back(b, d.get());
      }
    };
    addAll(list, parent, depth);
    while (!racks.empty()) {
      const auto [rb, rack] = racks.back();
      racks.pop_back();
      const uint16_t level = static_cast<uint16_t>(out_.boxes[rb].depth + 1);
      uint32_t k = 0;
      for (const doc::ChainPtr& c : rack->rack->chains) {
        ChainBox cb;
        cb.kind = ChainBoxKind::Chain;
        cb.chain = c->id;
        cb.parent = rb;
        cb.depth = level;
        cb.title = c->name.empty() ? "Chain " + std::to_string(k + 1) : c->name;
        cb.kindLabel = "Chain";
        cb.active = c->active;
        cb.instrument = out_.boxes[rb].instrument;
        const uint32_t ci = push(std::move(cb));
        out_.boxes[rb].children.push_back(ci);
        addAll(c->devices, ci, level);
        ++k;
      }
    }
  }

  uint32_t addDevice(const doc::Device& d, uint32_t parent, uint16_t depth) {
    ChainBox b;
    b.device = d.id;
    b.parent = parent;
    b.depth = depth;
    b.instrument = d.kind == doc::DeviceKind::Instrument;
    b.active = d.active;
    b.edited = d.edited();
    if (d.rack) {
      b.kind = ChainBoxKind::Rack;
      b.kindLabel = b.instrument ? "Instrument Rack" : "Audio Effect Rack";
      b.title = d.name.empty() ? b.kindLabel : d.name;
      return push(std::move(b));
    }
    b.kind = ChainBoxKind::Device;
    b.kindLabel = b.instrument ? "Instrument" : "Audio Effect";
    const doc::DeviceLibrary* lib = o_.devices;
    std::shared_ptr<const doc::ResolvedDevice> hold;
    const graph::Patch* callee = lib != nullptr ? lib->patchOf(d, hold) : nullptr;
    const doc::DeviceInfo* info = hold && hold->graph ? &hold->info : (lib != nullptr ? lib->catalog().find(d.patch) : nullptr);
    b.title = !d.name.empty() ? d.name : (info != nullptr ? info->name : d.patch);
    if (b.edited) b.title += " (edited)";
    Inner in;
    if (callee != nullptr) {
      const std::string key = hold && hold->graph ? hold->graph->root : d.patch;
      in.scene = o_.cache != nullptr ? o_.cache->sceneOf(key, *callee) : buildScene(*callee);
      const graph::SubcircuitRef sig = graph::subcircuitSignature(*callee);
      if (!b.instrument)
        for (const graph::SubcircuitPort& sp : sig.inputs)
          if (sp.type.rate == graph::Rate::Audio) {
            in.audioIn = sp.name;
            break;
          }
      if (!sig.outputs.empty()) in.audioOut = sig.outputs.front().name;
    } else {
      b.missing = true;
      b.title += " (missing)";
    }
    const uint32_t i = push(std::move(b));
    inner_[i] = std::move(in);
    return i;
  }

  // ---- layout -------------------------------------------------------------------------------------------------

  // Sizes children first (a child's index is above its parent's), then places parents first from `x`, `y`.
  void layout(float x, float y) {
    std::vector<ChainBox>& bx = out_.boxes;
    w_.assign(bx.size(), 0.0f);
    h_.assign(bx.size(), 0.0f);
    for (uint32_t i = static_cast<uint32_t>(bx.size()); i-- > 0;) {
      const ChainBox& b = bx[i];
      switch (b.kind) {
        case ChainBoxKind::Device:
          if (inner_[i].scene) {
            const CircuitScene& s = *inner_[i].scene;
            w_[i] = roundUp4(std::max(s.x1 - s.x0 + 2.0f * m::kPad, m::kEmptyW));
            h_[i] = roundUp4(s.y1 - s.y0 + 2.0f * m::kPad + m::kTitle);
          } else {
            w_[i] = m::kEmptyW;
            h_[i] = m::kEmptyH;
          }
          break;
        case ChainBoxKind::Chain: {
          float w = 0.0f, h = 0.0f;
          for (const uint32_t c : b.children) {
            w += w_[c] + (w > 0.0f ? m::kGap : 0.0f);
            h = std::max(h, h_[c]);
          }
          w_[i] = std::max(w, 96.0f) + 2.0f * m::kChainPad;
          h_[i] = m::kTitle + std::max(h, 32.0f) + m::kChainPad;
          break;
        }
        case ChainBoxKind::Rack: {
          float w = 0.0f, h = 0.0f;
          for (const uint32_t c : b.children) {
            w = std::max(w, w_[c]);
            h += h_[c] + (h > 0.0f ? m::kChainGap : 0.0f);
          }
          if (b.children.empty()) {
            w_[i] = m::kEmptyW;
            h_[i] = m::kEmptyH;
          } else {
            w_[i] = m::kRackSplit + w + m::kRackSum;
            h_[i] = m::kTitle + m::kChainPad + h + m::kChainPad;
          }
          break;
        }
      }
    }
    float cx = x;
    for (const uint32_t r : out_.roots) {
      place(r, cx, y);
      cx += w_[r] + m::kGap;
    }
    for (uint32_t i = 0; i < bx.size(); ++i) {
      const ChainBox& b = bx[i];
      if (b.kind == ChainBoxKind::Chain) {
        float px = b.x0 + m::kChainPad;
        for (const uint32_t c : b.children) {
          place(c, px, b.y0 + m::kTitle);
          px += w_[c] + m::kGap;
        }
      } else if (b.kind == ChainBoxKind::Rack) {
        float py = b.y0 + m::kTitle + m::kChainPad;
        for (const uint32_t c : b.children) {
          place(c, b.x0 + m::kRackSplit, py);
          py += h_[c] + m::kChainGap;
        }
      }
    }
  }

  // ---- the scene ----------------------------------------------------------------------------------------------

  // Appends a scene translated by (ox, oy); its nodes belong to `box`. Returns the first new node.
  uint32_t append(const CircuitScene& in, float ox, float oy, uint32_t box) {
    CircuitScene& sc = *out_.scene;
    const auto nodeBase = static_cast<uint32_t>(sc.nodes.size()), pinBase = static_cast<uint32_t>(sc.pins.size());
    const auto specBase = static_cast<uint32_t>(sc.specs.size()), stringBase = static_cast<uint32_t>(sc.strings.size());
    sc.specs.insert(sc.specs.end(), in.specs.begin(), in.specs.end());
    sc.strings.insert(sc.strings.end(), in.strings.begin(), in.strings.end());
    sc.nodes.reserve(sc.nodes.size() + in.nodes.size());
    for (const SceneNode& s : in.nodes) {
      SceneNode d = s;
      d.x += ox;
      d.y += oy;
      d.spec += specBase;
      d.firstPin += pinBase;
      d.node = static_cast<graph::NodeIndex>(nodeBase + s.node);
      sc.nodes.push_back(d);
      out_.boxOfNode.push_back(box);
    }
    sc.pins.reserve(sc.pins.size() + in.pins.size());
    for (const ScenePin& s : in.pins) {
      ScenePin d = s;
      d.x += ox;
      d.y += oy;
      d.node = static_cast<graph::NodeIndex>(nodeBase + s.node);
      if (d.label != UINT32_MAX) d.label += stringBase;
      sc.pins.push_back(d);
    }
    for (const SceneLabel& s : in.labels) {
      SceneLabel d = s;
      d.x0 += ox;
      d.x1 += ox;
      d.y += oy;
      d.text += stringBase;
      if (d.node != UINT32_MAX) d.node += nodeBase;
      sc.labels.push_back(d);
    }
    const auto pointBase = static_cast<uint32_t>(sc.points.size());
    sc.points.reserve(sc.points.size() + in.points.size());
    for (const Point2& q : in.points) sc.points.push_back({q.x + ox, q.y + oy});
    uint32_t maxNet = 0;
    const auto wireBase = static_cast<uint32_t>(sc.wires.size());
    for (const SceneWire& s : in.wires) {
      SceneWire d = s;
      d.firstPoint += pointBase;
      d.net += nextNet_;
      d.fromPin += pinBase;
      d.toPin += pinBase;
      d.x0 += ox, d.x1 += ox, d.y0 += oy, d.y1 += oy;
      d.wire = static_cast<graph::WireIndex>(sc.wires.size());
      maxNet = std::max(maxNet, s.net);
      sc.wires.push_back(d);
    }
    for (const SceneSegment& s : in.segments) sc.segments.push_back({s.wire + wireBase, s.point + pointBase});
    // The part's junctions as its own scene found them (its nets stay its own here), translated.
    for (size_t j = 0; j < in.junctions.size(); ++j) {
      sc.junctions.push_back({in.junctions[j].x + ox, in.junctions[j].y + oy});
      sc.junctionWire.push_back(in.junctionWire[j] + wireBase);
    }
    if (!in.wires.empty()) nextNet_ += maxNet + 1;
    return nodeBase;
  }

  uint32_t newNet() noexcept {
    if (firstLinkNet_ == UINT32_MAX) firstLinkNet_ = nextNet_;
    return nextNet_++;
  }

  // A wire through `pts` (orthogonal), its net, the pins it joins (UINT32_MAX: none).
  uint32_t addWire(const std::vector<Point2>& pts, WireStyle style, uint32_t net, uint32_t fromPin, uint32_t toPin) {
    CircuitScene& sc = *out_.scene;
    SceneWire sw;
    sw.firstPoint = static_cast<uint32_t>(sc.points.size());
    sw.pointCount = static_cast<uint32_t>(pts.size());
    sw.style = style;
    sw.net = net;
    sw.wire = static_cast<graph::WireIndex>(sc.wires.size());
    sw.fromPin = fromPin == UINT32_MAX ? 0 : fromPin;
    sw.toPin = toPin == UINT32_MAX ? 0 : toPin;
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
    if (fromPin != UINT32_MAX) sc.pins[fromPin].connected = true;
    if (toPin != UINT32_MAX) sc.pins[toPin].connected = true;
    return w;
  }

  // a → b, orthogonal, jogging at x = mid.
  static void route(std::vector<Point2>& pts, Point2 a, Point2 b, float mid) {
    pts.clear();
    pts.push_back(a);
    if (a.y != b.y) {
      pts.push_back({mid, a.y});
      pts.push_back({mid, b.y});
    }
    pts.push_back(b);
  }

  // The node of `scene` (from `base`) with this symbol and title, else kNoBox.
  uint32_t terminal(uint32_t base, const CircuitScene& in, const std::string& name, Symbol a, Symbol b = Symbol::Box) const {
    if (name.empty()) return kNoBox;
    for (uint32_t v = 0; v < in.nodes.size(); ++v) {
      const SymbolSpec& s = in.specs[in.nodes[v].spec];
      if ((s.symbol == a || s.symbol == b) && s.title == name) return base + v;
    }
    return kNoBox;
  }

  // The devices' scenes inside their boxes.
  void composeDevices() {
    for (uint32_t i = 0; i < out_.boxes.size(); ++i) {
      ChainBox& b = out_.boxes[i];
      if (b.kind != ChainBoxKind::Device || !inner_[i].scene) continue;
      const CircuitScene& in = *inner_[i].scene;
      const float ox = b.x0 + m::kPad - in.x0, oy = b.y0 + m::kTitle + m::kPad - in.y0;
      const uint32_t base = append(in, ox, oy, i);
      b.firstNode = base;
      b.nodeCount = static_cast<uint32_t>(in.nodes.size());
      b.inNode = terminal(base, in, inner_[i].audioIn, Symbol::Input);
      b.outNode = terminal(base, in, inner_[i].audioOut, Symbol::Output);
    }
  }

  uint32_t firstInputPin(uint32_t node) const {
    const SceneNode& n = out_.scene->nodes[node];
    for (uint32_t k = 0; k < n.pinCount; ++k)
      if (out_.scene->pins[n.firstPin + k].input) return n.firstPin + k;
    return UINT32_MAX;
  }
  uint32_t firstOutputPin(uint32_t node) const {
    const SceneNode& n = out_.scene->nodes[node];
    for (uint32_t k = 0; k < n.pinCount; ++k)
      if (!out_.scene->pins[n.firstPin + k].input) return n.firstPin + k;
    return UINT32_MAX;
  }

  // Where a wire into / out of each box ends (children first), and the racks' Σ.
  void endpoints() {
    std::vector<ChainBox>& bx = out_.boxes;
    in_.assign(bx.size(), {});
    outp_.assign(bx.size(), {});
    const CircuitScene& sc = *out_.scene;
    for (uint32_t i = static_cast<uint32_t>(bx.size()); i-- > 0;) {
      const ChainBox& b = bx[i];
      const float spine = b.y0 + m::kTitle + 16.0f;
      switch (b.kind) {
        case ChainBoxKind::Device: {
          if (b.outNode != kNoBox) {
            const SceneNode& n = sc.nodes[b.outNode];
            outp_[i] = {n.x + n.w, n.y + n.h * 0.5f};
          } else {
            outp_[i] = {b.x1, spine};
          }
          if (b.inNode != kNoBox) {
            const SceneNode& n = sc.nodes[b.inNode];
            in_[i] = {n.x, n.y + n.h * 0.5f};
          } else {
            in_[i] = {b.x0, b.outNode != kNoBox ? outp_[i].y : spine};  // an instrument: notes end on the boundary
          }
          break;
        }
        case ChainBoxKind::Chain:
          if (b.children.empty()) {
            in_[i] = {b.x0, spine};
            outp_[i] = {b.x1, spine};
          } else {
            in_[i] = in_[b.children.front()];
            outp_[i] = outp_[b.children.back()];
          }
          break;
        case ChainBoxKind::Rack: {
          const float y = b.children.empty() ? spine : in_[b.children.front()].y;
          in_[i] = {b.x0, y};
          outp_[i] = {b.x1, y};
          break;
        }
      }
    }
  }

  // The racks' Σ nodes (after every device scene, so device node ranges stay contiguous).
  void addSums() {
    CircuitScene& sc = *out_.scene;
    for (uint32_t i = 0; i < out_.boxes.size(); ++i) {
      ChainBox& b = out_.boxes[i];
      if (b.kind != ChainBoxKind::Rack || b.children.empty()) continue;
      SymbolSpec s;
      s.symbol = Symbol::Summer;
      s.glyph = Glyph::Plus;
      s.w = m::kSumW;
      s.h = m::kSumH;
      PinSpec pi;
      pi.y = 16.0f;
      pi.stub = 8.0f;
      s.inputs.push_back(pi);
      PinSpec po;
      po.y = 16.0f;
      po.stub = 6.0f;
      s.outputs.push_back(po);
      s.bodyKey = bodyKeyOf(s);
      const auto v = static_cast<uint32_t>(sc.nodes.size());
      sc.specs.push_back(std::move(s));
      SceneNode n;
      n.x = b.x1 - m::kRackSum + 24.0f;
      n.y = in_[i].y - 16.0f;
      n.w = m::kSumW;
      n.h = m::kSumH;
      n.spec = static_cast<uint32_t>(sc.specs.size() - 1);
      n.node = v;
      n.firstPin = static_cast<uint32_t>(sc.pins.size());
      n.pinCount = 2;
      ScenePin a;
      a.x = n.x;
      a.y = n.y + 16.0f;
      a.stub = 8.0f;
      a.node = v;
      a.input = true;
      sc.pins.push_back(a);
      ScenePin z;
      z.x = n.x + n.w;
      z.y = n.y + 16.0f;
      z.stub = 6.0f;
      z.node = v;
      z.input = false;
      sc.pins.push_back(z);
      sc.nodes.push_back(n);
      out_.boxOfNode.push_back(i);
      b.firstNode = v;
      b.nodeCount = 1;
    }
  }

  // A terminal (an `in` / `out` flag) whose wire end is at height y, left edge at x (input: right edge at x).
  uint32_t addTerminal(const std::string& name, bool input, float x, float y) {
    const std::shared_ptr<const CircuitScene> t = terminalScene(name, input);
    if (t->nodes.empty()) return kNoBox;
    const SceneNode& n = t->nodes.front();
    const float ox = input ? x - (n.x + n.w) : x - n.x;
    const float oy = y - (n.y + n.h * 0.5f);
    return append(*t, ox, oy, kNoBox);
  }

  // The audio / notes wires: along every chain, through every rack, from and to the terminals.
  void wire(bool notesIn) {
    std::vector<ChainBox>& bx = out_.boxes;
    CircuitScene& sc = *out_.scene;
    std::vector<Point2> pts;
    WireStyle audio;
    audio.rate = WireRate::Audio;
    WireStyle notes = audio;
    notes.poly = true;
    auto link = [&](ChainLink::Kind k, uint32_t from, uint32_t to, const std::vector<Point2>& path, uint32_t net, uint32_t fp, uint32_t tp) {
      ChainLink l;
      l.kind = k;
      l.from = from;
      l.to = to;
      l.sceneWire = addWire(path, k == ChainLink::Kind::Notes ? notes : audio, net, fp, tp);
      out_.links.push_back(l);
    };
    auto pinIn = [&](uint32_t b) { return b != kNoBox && bx[b].kind == ChainBoxKind::Device && bx[b].inNode != kNoBox ? firstOutputPin(bx[b].inNode) : UINT32_MAX; };
    auto pinOut = [&](uint32_t b) { return b != kNoBox && bx[b].kind == ChainBoxKind::Device && bx[b].outNode != kNoBox ? firstInputPin(bx[b].outNode) : UINT32_MAX; };
    // A row of boxes: consecutive ones joined in the gap between them.
    auto row = [&](const std::vector<uint32_t>& list) {
      for (size_t k = 0; k + 1 < list.size(); ++k) {
        const uint32_t a = list[k], b = list[k + 1];
        const bool toInstrument = bx[b].instrument && bx[b].kind != ChainBoxKind::Chain;
        if (toInstrument) continue;  // nothing reaches an instrument from its left but notes
        route(pts, outp_[a], in_[b], bx[a].x1 + m::kGap * 0.5f);
        link(ChainLink::Kind::Audio, a, b, pts, newNet(), UINT32_MAX, UINT32_MAX);
      }
    };
    row(out_.roots);
    for (uint32_t i = 0; i < bx.size(); ++i) {
      const ChainBox& b = bx[i];
      if (b.kind == ChainBoxKind::Chain) {
        if (b.children.empty()) {
          pts.assign({in_[i], outp_[i]});
          link(b.instrument ? ChainLink::Kind::Notes : ChainLink::Kind::Audio, i, i, pts, newNet(), UINT32_MAX, UINT32_MAX);
        } else {
          row(b.children);
        }
      } else if (b.kind == ChainBoxKind::Rack) {
        if (b.children.empty()) {
          pts.assign({in_[i], outp_[i]});
          link(ChainLink::Kind::Audio, i, i, pts, newNet(), UINT32_MAX, UINT32_MAX);
          continue;
        }
        // The split: the rack's input onto a bus into every chain (an Instrument Rack's: the notes).
        const uint32_t split = newNet(), sum = newNet();
        const float bus = b.x0 + m::kRackSplit * 0.5f;
        for (const uint32_t c : b.children) {
          route(pts, in_[i], in_[c], bus);
          const uint32_t first = bx[c].children.empty() ? kNoBox : bx[c].children.front();
          link(b.instrument ? ChainLink::Kind::Notes : ChainLink::Kind::Audio, i, first != kNoBox ? first : c, pts, split, UINT32_MAX,
               pinIn(first));
        }
        // Every chain's output onto the summing bus into Σ, Σ to the rack's output.
        const SceneNode& sn = sc.nodes[b.firstNode];
        const uint32_t sumIn = sn.firstPin, sumOut = sn.firstPin + 1;
        const Point2 sinPt{sc.pins[sumIn].x, sc.pins[sumIn].y};
        for (const uint32_t c : b.children) {
          const uint32_t last = bx[c].children.empty() ? kNoBox : bx[c].children.back();
          route(pts, outp_[c], sinPt, sinPt.x - 12.0f);
          link(ChainLink::Kind::Audio, last != kNoBox ? last : c, i, pts, sum, pinOut(last), sumIn);
        }
        pts.assign({{sc.pins[sumOut].x, sc.pins[sumOut].y}, outp_[i]});
        link(ChainLink::Kind::Audio, i, i, pts, newNet(), sumOut, UINT32_MAX);
      }
    }
    // The terminals.
    const uint32_t first = out_.roots.empty() ? kNoBox : out_.roots.front();
    const uint32_t last = out_.roots.empty() ? kNoBox : out_.roots.back();
    if (out_.inputNode != kNoBox) {
      const SceneNode& t = sc.nodes[out_.inputNode];
      const Point2 a{t.x + t.w, t.y + t.h * 0.5f};
      const uint32_t fp = firstOutputPin(out_.inputNode);
      if (first == kNoBox) {
        if (out_.outputNode != kNoBox && !notesIn) {
          const SceneNode& o = sc.nodes[out_.outputNode];
          route(pts, a, {o.x, o.y + o.h * 0.5f}, (a.x + o.x) * 0.5f);
          link(ChainLink::Kind::Audio, kNoBox, kNoBox, pts, newNet(), fp, firstInputPin(out_.outputNode));
        }
      } else if (!notesIn || bx[first].instrument) {
        route(pts, a, in_[first], bx[first].x0 - m::kGap * 0.5f);
        link(notesIn ? ChainLink::Kind::Notes : ChainLink::Kind::Audio, kNoBox, first, pts, newNet(), fp, pinIn(first));
      }
    }
    if (out_.outputNode != kNoBox && last != kNoBox) {
      const SceneNode& o = sc.nodes[out_.outputNode];
      route(pts, outp_[last], {o.x, o.y + o.h * 0.5f}, bx[last].x1 + m::kGap * 0.5f);
      link(ChainLink::Kind::Audio, last, kNoBox, pts, newNet(), pinOut(last), firstInputPin(out_.outputNode));
    }
  }

  // T4.7's wires between two devices of this circuit, in the gutter above the boxes.
  void modulation(float top) {
    const CircuitScene& sc = *out_.scene;
    std::vector<Point2> pts;
    WireStyle mod;
    mod.rate = WireRate::Audio;
    mod.modulation = true;
    uint32_t lane = 0;
    for (const doc::Wire& w : doc::wiresOf(p_)) {
      const uint32_t fb = out_.boxOf(w.from), tb = out_.boxOf(w.to);
      if (fb == kNoBox || tb == kNoBox) continue;
      const ChainBox& f = out_.boxes[fb];
      const ChainBox& t = out_.boxes[tb];
      auto find = [&](const ChainBox& b, const std::string& name, Symbol sym) -> uint32_t {
        if (b.kind != ChainBoxKind::Device) return kNoBox;
        for (uint32_t v = b.firstNode; v < b.firstNode + b.nodeCount; ++v)
          if (sc.specs[sc.nodes[v].spec].symbol == sym && sc.specs[sc.nodes[v].spec].title == name) return v;
        return kNoBox;
      };
      const uint32_t src = find(f, w.port, Symbol::Export), dst = find(t, w.param, Symbol::Param);
      Point2 a{f.x1, f.y0 + m::kTitle * 0.5f}, b{t.x0, t.y0 + m::kTitle * 0.5f};
      if (src != kNoBox) a = {sc.nodes[src].x + sc.nodes[src].w, sc.nodes[src].y + sc.nodes[src].h * 0.5f};
      if (dst != kNoBox) b = {sc.nodes[dst].x, sc.nodes[dst].y + sc.nodes[dst].h * 0.5f};
      const float gy = top - 16.0f - m::kModLane * static_cast<float>(lane++);
      pts.assign({a, {a.x + 8.0f, a.y}, {a.x + 8.0f, gy}, {b.x - 8.0f, gy}, {b.x - 8.0f, b.y}, b});
      ChainLink l;
      l.kind = ChainLink::Kind::Modulation;
      l.from = fb;
      l.to = tb;
      l.wire = w.id;
      l.sceneWire = addWire(pts, mod, newNet(), src != kNoBox ? firstInputPin(src) : UINT32_MAX,
                            dst != kNoBox ? firstOutputPin(dst) : UINT32_MAX);
      out_.links.push_back(l);
    }
  }

  ChainCircuit finish(float margin, float titleStrip) {
    ChainCircuit& c = out_;
    const CircuitScene& sc = *c.scene;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool any = false;
    auto grow = [&](float a, float b, float cc, float d) {
      if (!any) x0 = a, y0 = b, x1 = cc, y1 = d, any = true;
      else x0 = std::min(x0, a), y0 = std::min(y0, b), x1 = std::max(x1, cc), y1 = std::max(y1, d);
    };
    for (const ChainBox& b : c.boxes) grow(b.x0, b.y0, b.x1, b.y1);
    for (const SceneNode& n : sc.nodes) grow(n.x, n.y, n.x + n.w, n.y + n.h);
    for (const SceneWire& w : sc.wires) grow(w.x0, w.y0, w.x1, w.y1);
    c.x0 = x0 - margin;
    c.y0 = y0 - margin - titleStrip;
    c.x1 = x1 + margin;
    c.y1 = y1 + margin;
    const float include[4] = {c.x0, c.y0, c.x1, c.y1};
    // Junctions: the devices' scenes' are copied (append); only the links' nets (allocated after every part's) are searched.
    indexScene(*c.scene, include, firstLinkNet_ == UINT32_MAX ? nextNet_ : firstLinkNet_);
    return std::move(out_);
  }

  ChainCircuit& out() noexcept { return out_; }
  const Point2& inPoint(uint32_t b) const { return in_[b]; }
  const Point2& outPoint(uint32_t b) const { return outp_[b]; }
  float rootsTop() const noexcept {
    float t = 0.0f;
    for (const ChainBox& b : out_.boxes) t = std::min(t, b.y0);
    return t;
  }

private:
  uint32_t push(ChainBox b) {
    out_.boxes.push_back(std::move(b));
    inner_.emplace_back();  // one per box (only devices fill theirs)
    return static_cast<uint32_t>(out_.boxes.size() - 1);
  }
  void place(uint32_t i, float x, float y) {
    ChainBox& b = out_.boxes[i];
    b.x0 = x;
    b.y0 = y;
    b.x1 = x + w_[i];
    b.y1 = y + h_[i];
  }

  struct Inner {
    std::shared_ptr<const CircuitScene> scene;
    std::string audioIn, audioOut;
  };

  const doc::Project& p_;
  const ChainOptions& o_;
  ChainCircuit out_;
  std::vector<Inner> inner_;  // per box
  std::vector<float> w_, h_;
  std::vector<Point2> in_, outp_;
  uint32_t nextNet_ = 0;
  uint32_t firstLinkNet_ = UINT32_MAX;  // the first net newNet() handed out (every part's nets are below it)
};

const char* trackInputName(doc::TrackKind k) noexcept {
  switch (k) {
    case doc::TrackKind::Midi: return "MIDI";
    case doc::TrackKind::Audio: return "Audio In";
    default: return "\xCE\xA3 In";  // Σ In: every track routed into it
  }
}

}  // namespace

ChainCircuit buildTrackCircuit(const doc::Project& project, doc::TrackId track, const ChainOptions& options) {
  Builder b(project, options);
  const doc::Track* t = doc::findTrackPtr(project, track);
  if (t == nullptr) {
    ChainCircuit empty;
    empty.scene = std::make_shared<CircuitScene>();
    indexScene(*empty.scene);
    return empty;
  }
  b.out().track = track;
  b.out().trackName = t->name;
  b.out().trackColour = t->colour.rgb;
  b.addList(t->devices, kNoBox, 0);
  b.layout(0.0f, 0.0f);
  b.composeDevices();
  b.endpoints();
  b.addSums();
  // The terminals: the track's input left of its first box, its mixer right of its last one.
  ChainCircuit& c = b.out();
  const uint32_t first = c.roots.empty() ? kNoBox : c.roots.front();
  const uint32_t last = c.roots.empty() ? kNoBox : c.roots.back();
  const float yIn = first != kNoBox ? b.inPoint(first).y : m::kTitle + 16.0f;
  const float yOut = last != kNoBox ? b.outPoint(last).y : yIn;
  const float xOut = last != kNoBox ? c.boxes[last].x1 + m::kGap : 0.0f + m::kGap;
  c.inputNode = b.addTerminal(trackInputName(t->kind), true, -m::kGap, yIn);
  c.outputNode = b.addTerminal("Mixer", false, xOut, yOut);
  b.wire(t->kind == doc::TrackKind::Midi);
  b.modulation(b.rootsTop());
  return b.finish(16.0f, 24.0f);  // the track's band: a margin and its name strip
}

ChainCircuit buildRackCircuit(const doc::Project& project, doc::DeviceId rack, const ChainOptions& options) {
  Builder b(project, options);
  const doc::DeviceLoc loc = doc::findDevice(project, rack);
  if (loc.device == nullptr || !loc.device->rack) {
    ChainCircuit empty;
    empty.scene = std::make_shared<CircuitScene>();
    indexScene(*empty.scene);
    return empty;
  }
  ChainCircuit& c = b.out();
  c.rack = rack;
  if (loc.track != nullptr) {
    c.track = loc.track->id;
    c.trackName = loc.track->name;
    c.trackColour = loc.track->colour.rgb;
  }
  // The rack as the one root (a list of one device: addList opens it).
  const std::vector<doc::DevicePtr>& list = loc.list();
  for (const doc::DevicePtr& d : list)
    if (d->id == rack) {
      const std::vector<doc::DevicePtr> one{d};
      b.addList(one, kNoBox, 0);
      break;
    }
  b.layout(0.0f, 0.0f);
  b.composeDevices();
  b.endpoints();
  b.addSums();
  const uint32_t root = c.roots.front();
  const bool instrument = c.boxes[root].instrument;
  c.inputNode = b.addTerminal(instrument ? "notes" : "in", true, -m::kGap, b.inPoint(root).y);
  c.outputNode = b.addTerminal("out", false, c.boxes[root].x1 + m::kGap, b.outPoint(root).y);
  b.wire(instrument);
  b.modulation(b.rootsTop());
  return b.finish(4.0f, 0.0f);
}

}  // namespace pq::views::circuit
