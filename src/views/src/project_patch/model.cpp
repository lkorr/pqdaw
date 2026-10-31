// The project patch model (T4.13): the lowered ProjectGraph read back onto the document, and its automatic layout.
// See pq/views/project_patch/project_patch.h.

#include "pq/views/project_patch/project_patch.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "pq/graph/patch_file.h"

namespace pq::views::project_patch {

namespace m = metrics;

int32_t Node::input(std::string_view name) const noexcept {
  for (size_t i = 0; i < inputs.size(); ++i)
    if (inputs[i].name == name) return static_cast<int32_t>(i);
  return -1;
}

int32_t Node::output(std::string_view name) const noexcept {
  for (size_t i = 0; i < outputs.size(); ++i)
    if (outputs[i].name == name) return static_cast<int32_t>(i);
  return -1;
}

uint32_t ProjectPatch::bandOf(doc::TrackId t) const noexcept {
  for (uint32_t i = 0; i < bands.size(); ++i)
    if (bands[i].track == t) return i;
  return kNone;
}

uint32_t ProjectPatch::nodeOf(doc::DeviceId d) const noexcept {
  for (uint32_t i = 0; i < nodes.size(); ++i)
    if (nodes[i].kind == NodeKind::Device && nodes[i].device == d) return i;
  for (uint32_t i = 0; i < nodes.size(); ++i)  // T4.8: inside a rack
    if (nodes[i].rack && std::find(nodes[i].inner.begin(), nodes[i].inner.end(), d) != nodes[i].inner.end()) return i;
  return kNone;
}

namespace {

float roundUp4(float v) noexcept { return std::ceil(v / 4.0f) * 4.0f; }
float textW(std::string_view s) noexcept { return 6.0f * static_cast<float>(s.size()); }  // ≈ the 11-px UI font

// The y offset of a node's pin: the main one (index 0) at the spine, the rest below it.
float pinY(const Node& n, size_t i) noexcept {
  switch (n.kind) {
    case NodeKind::Sum: return 16.0f;
    case NodeKind::Output: return 8.0f;
    default: return m::kPinTop + m::kPinPitch * static_cast<float>(i);
  }
}
// Where the spine (pin 0) sits from the node's top.
float spineOf(const Node& n) noexcept { return pinY(n, 0); }

std::string sendLetter(size_t returnIndex) {
  std::string s;
  size_t i = returnIndex;
  do {
    s.insert(s.begin(), static_cast<char>('A' + i % 26));
    i = i / 26;
  } while (i-- > 0);
  return s;
}

struct Builder {
  const doc::Project& p;
  const doc::LoweredProject& low;
  const doc::DeviceCatalog* catalog;
  const LayoutOptions& opt;
  ProjectPatch out;

  std::unordered_map<uint32_t, uint32_t> graphIndex;   // track id → graph track
  std::unordered_map<uint32_t, size_t> returnIndex;    // return track id → its index among the returns
  std::unordered_map<uint32_t, std::vector<std::string>> modulatedParams;  // device id → params wires drive
  std::unordered_map<uint32_t, doc::WireStatus> wireStatus;
  std::unordered_map<uint32_t, uint32_t> modGraphWire;  // doc wire id → graph wire index (cross-track)
  std::unordered_map<uint32_t, const doc::Track*> trackById;
  std::unordered_map<const graph::Patch*, std::unordered_set<std::string_view>> paramsOf;

  const graph::PatchLibrary* library() const noexcept { return low.graph.library; }
  const doc::ResolvedDevice* userDevice(doc::DeviceId id) const noexcept {
    for (const auto& r : low.userDevices)
      if (r && r->device == id) return r.get();
    return nullptr;
  }

  void prepare() {
    doc::forEachTrack(p, [&](const doc::Track& t, const doc::Track*) { trackById.emplace(t.id.value, &t); });
    for (size_t i = 0; i < low.trackIds.size(); ++i) graphIndex[low.trackIds[i].value] = static_cast<uint32_t>(i);
    for (size_t i = 0; i < p.returns.size(); ++i) returnIndex[p.returns[i]->id.value] = i;
    for (const doc::WireState& s : low.wires) wireStatus[s.id.value] = s.status;
    for (const doc::Wire& w : doc::wiresOf(p)) {
      if (statusOf(w.id) == doc::WireStatus::Invalid) continue;
      auto& v = modulatedParams[w.to.value];
      if (std::find(v.begin(), v.end(), w.param) == v.end()) v.push_back(w.param);
    }
    for (size_t gi = 0; gi < low.graph.wires.size(); ++gi) {
      const graph::ProjectWire& gw = low.graph.wires[gi];
      if (gw.kind == graph::WireKind::Modulation && gw.from.port.size() > 1 && gw.from.port[0] == 'm')
        modGraphWire[static_cast<uint32_t>(std::stoul(gw.from.port.substr(1)))] = static_cast<uint32_t>(gi);
    }
  }

  doc::WireStatus statusOf(doc::WireId id) const {
    const auto it = wireStatus.find(id.value);
    return it == wireStatus.end() ? doc::WireStatus::Inactive : it->second;
  }

  // T4.8: the port of `node` a wire end on `device`.`port` uses: the port itself, or on a rack's block for a device
  // inside it "<port>@<device id>".
  static std::string portKey(const Node& node, doc::DeviceId device, const std::string& port) {
    return node.device == device ? port : port + "@" + std::to_string(device.value);
  }

  // T4.8: a rack as one block (Node::rack).
  Node rackNode(uint32_t band, const doc::Device& d, const graph::Patch* tp) {
    Node n;
    n.kind = NodeKind::Device;
    n.band = band;
    n.device = d.id;
    n.active = d.active;
    n.instrument = d.kind == doc::DeviceKind::Instrument;
    n.rack = true;
    n.title = n.instrument ? "Instrument Rack" : "Audio Effect Rack";
    bool plays = false;
    doc::forEachDeviceIn(d, [&](const doc::Device& x) {
      if (x.id == d.id) return;
      n.inner.push_back(x.id);
      if (!x.rack && callOf(tp, x.id) != graph::kNone) plays = true;
    });
    // An Audio Effect Rack with no device that plays passes its input (or is silent): it is in the signal path.
    n.lowered = tp != nullptr && d.active && (plays || !n.instrument);
    if (n.instrument) n.inputs.push_back({"notes", ""});
    else n.inputs.push_back({"in", ""});
    auto mods = [&](doc::DeviceId dev) {
      const auto it = modulatedParams.find(dev.value);
      if (it == modulatedParams.end()) return;
      for (const std::string& prm : it->second) n.inputs.push_back({portKey(n, dev, prm), prm});
    };
    mods(d.id);  // its macros
    for (const doc::DeviceId x : n.inner) mods(x);
    n.outputs.push_back({"out", ""});
    for (const doc::Wire& w : doc::wiresOf(p)) {  // exports of devices inside it that wires use
      if (statusOf(w.id) == doc::WireStatus::Invalid) continue;
      if (std::find(n.inner.begin(), n.inner.end(), w.from) == n.inner.end()) continue;
      const std::string key = portKey(n, w.from, w.port);
      if (n.output(key) < 0) n.outputs.push_back({key, w.port});
    }
    sizeBlock(n, 120.0f);
    return n;
  }

  // ---- nodes ----------------------------------------------------------------------------------------------------

  uint32_t addNode(Node n) {
    out.nodes.push_back(std::move(n));
    return static_cast<uint32_t>(out.nodes.size() - 1);
  }

  void sizeBlock(Node& n, float minW) {
    float labelW = 0.0f;
    for (const Port& q : n.inputs) labelW = std::max(labelW, textW(q.label));
    float outW = 0.0f;
    for (const Port& q : n.outputs) outW = std::max(outW, textW(q.label));
    const size_t rows = std::max<size_t>({n.inputs.size(), n.outputs.size(), 1});
    n.w = roundUp4(std::max({minW, 32.0f + textW(n.title), 28.0f + labelW + outW}));
    n.h = roundUp4(std::max(48.0f, m::kPinTop + m::kPinPitch * static_cast<float>(rows - 1) + 16.0f));
  }

  // The device's call node in the track patch (kNone: not lowered).
  graph::NodeIndex callOf(const graph::Patch* tp, doc::DeviceId id) const {
    if (tp == nullptr) return graph::kNone;
    const auto idx = tp->findByName("d" + std::to_string(id.value));
    if (!idx || tp->node(*idx).kind != graph::NodeKind::Subcircuit) return graph::kNone;
    if (tp->node(*idx).id.value != doc::nodeIdFor(doc::NodeRole::Device, id.value, 0)) return graph::kNone;
    return *idx;
  }

  Node deviceNode(uint32_t band, const doc::Device& d, const graph::Patch* tp) {
    Node n;
    n.kind = NodeKind::Device;
    n.band = band;
    n.device = d.id;
    n.active = d.active;
    n.instrument = d.kind == doc::DeviceKind::Instrument;
    // A user device (T4.4-fix1: the instance's own patch text) plays its resolution's renamed root: its callee and its
    // description come from the resolution the lowering made; a stock device's from the library and the catalog.
    const doc::ResolvedDevice* user = d.edited() ? userDevice(d.id) : nullptr;
    const doc::DeviceInfo* info = user != nullptr && user->graph ? &user->info : catalog ? catalog->find(d.patch) : nullptr;
    n.edited = d.edited();
    n.title = info ? info->name : d.patch;
    if (n.edited) n.title += " (edited)";
    const graph::NodeIndex call = callOf(tp, d.id);
    n.lowered = call != graph::kNone;
    if (n.lowered) n.call = tp->node(call).id;
    const graph::Patch* callee = nullptr;
    if (n.lowered && library() != nullptr) callee = library()->find(tp->subcircuit(call).patch);  // what the graph calls
    else if (user != nullptr) callee = user->root();
    else if (!d.edited() && library() != nullptr) callee = library()->find(d.patch);
    n.missing = callee == nullptr;
    for (const ExpandedSize& e : opt.expanded)
      if (e.device == d.id && callee != nullptr) n.expanded = true;
    graph::SubcircuitRef sig;
    if (n.lowered) sig = tp->subcircuit(call);
    else if (callee != nullptr) sig = graph::subcircuitSignature(*callee);
    // Inputs: an instrument's notes, the audio inputs (`in`, a sidechain), the params a wire drives.
    if (n.instrument) n.inputs.push_back({"notes", ""});
    const auto modIt = modulatedParams.find(d.id.value);
    // The callee's param names, once per patch (a big device has hundreds: no name search per port).
    const std::unordered_set<std::string_view>* params = nullptr;
    if (callee != nullptr) {
      auto [it, fresh] = paramsOf.try_emplace(callee);
      if (fresh)
        for (graph::NodeIndex k = 0; k < callee->nodeCount(); ++k)
          if (callee->node(k).kind == graph::NodeKind::Param) it->second.insert(callee->nodeName(k));
      params = &it->second;
    }
    bool firstAudio = true;
    for (const graph::SubcircuitPort& sp : sig.inputs) {
      if (n.instrument) break;
      if (sp.type.rate == graph::Rate::Const) continue;  // a structural setting (a literal at the call)
      if (params != nullptr && params->count(sp.name) != 0) continue;
      n.inputs.push_back({sp.name, firstAudio ? std::string() : sp.name});
      firstAudio = false;
    }
    if (sig.inputs.empty() && !n.instrument) n.inputs.push_back({"in", ""});
    if (modIt != modulatedParams.end())
      for (const std::string& prm : modIt->second) {
        const doc::DeviceParamInfo* pi = info ? info->param(prm) : nullptr;
        n.inputs.push_back({prm, pi && !pi->label.empty() ? pi->label : prm});
      }
    bool firstOut = true;
    for (const graph::SubcircuitPort& sp : sig.outputs) {
      n.outputs.push_back({sp.name, firstOut ? std::string() : sp.name});
      firstOut = false;
    }
    if (n.outputs.empty()) n.outputs.push_back({"out", ""});
    sizeBlock(n, 96.0f);
    if (n.expanded)
      for (const ExpandedSize& e : opt.expanded)
        if (e.device == d.id) {
          n.w = roundUp4(std::max(n.w, e.w + 2.0f * m::kExpandedPad));
          n.h = roundUp4(std::max(n.h, e.h + 2.0f * m::kExpandedPad + 16.0f));
        }
    return n;
  }

  Node mixerNode(uint32_t band, const doc::Track& t, bool lowered) {
    Node n;
    n.kind = NodeKind::Mixer;
    n.band = band;
    n.title = "Mixer";
    n.inputs.push_back({"in", ""});
    n.outputs.push_back({"out", ""});
    if (lowered) {
      const uint32_t gt = graphIndex.at(t.id.value);
      std::vector<std::pair<size_t, std::string>> sends;  // (return index, port)
      for (const graph::ProjectWire& gw : low.graph.wires) {
        if (gw.kind != graph::WireKind::Audio || gw.from.track != gt) continue;
        if (gw.from.port != "post" && gw.from.port != "pre") continue;
        const doc::TrackId ret = low.trackIds[gw.to.track];
        const auto ri = returnIndex.find(ret.value);
        sends.emplace_back(ri == returnIndex.end() ? 0 : ri->second, "s" + std::to_string(ret.value));
      }
      std::sort(sends.begin(), sends.end());
      for (const auto& [ri, port] : sends) n.outputs.push_back({port, sendLetter(ri)});
    }
    const bool expanded = std::find(opt.expandedMixers.begin(), opt.expandedMixers.end(), t.id) != opt.expandedMixers.end();
    const size_t sends = n.outputs.size() - 1;
    n.w = expanded ? 144.0f : m::kMixerW;  // expanded: each send's name and level beside its pin
    n.h = roundUp4(std::max(m::kMixerH, m::kPinTop + m::kPinPitch * static_cast<float>(sends) + 16.0f));  // ≥ 60: the values row
    n.expanded = expanded;
    return n;
  }

  void buildBand(const doc::Track& t, uint16_t depth, int32_t parent, bool parentActive) {
    Band b;
    b.track = t.id;
    b.kind = t.kind;
    b.name = t.name;
    b.colour = t.colour.rgb;
    b.depth = depth;
    b.parent = parent;
    b.active = t.active && parentActive;
    b.muted = t.mixer.mute;
    const auto gi = graphIndex.find(t.id.value);
    if (gi != graphIndex.end()) {
      b.graphTrack = gi->second;
      b.role = low.graph.tracks[gi->second].role;
    }
    const auto bi = static_cast<uint32_t>(out.bands.size());
    out.bands.push_back(b);
    const graph::Patch* tp = b.graphTrack != kNone ? low.graph.tracks[b.graphTrack].patch : nullptr;
    const bool lowered = tp != nullptr;

    Band& band = out.bands[bi];
    if (t.kind == doc::TrackKind::Midi) {
      Node s;
      s.kind = NodeKind::Source;
      s.band = bi;
      s.title = "MIDI";
      s.outputs.push_back({"notes", ""});
      s.w = m::kSourceW;
      s.h = m::kSourceH;
      band.source = addNode(std::move(s));
    } else if (t.kind == doc::TrackKind::Audio) {  // T5.3: the clip player (graph: clipPlayer → its chain)
      Node s;
      s.kind = NodeKind::Source;
      s.band = bi;
      s.title = "Clips";
      s.outputs.push_back({"out", ""});
      s.w = m::kSourceW;
      s.h = m::kSourceH;
      band.source = addNode(std::move(s));
    } else {
      Node s;
      s.kind = NodeKind::Sum;
      s.band = bi;
      s.title = "\xCE\xA3";  // Σ
      s.inputs.push_back({"in", ""});
      s.outputs.push_back({"out", ""});
      s.w = m::kSumW;
      s.h = m::kSumH;
      out.bands[bi].source = addNode(std::move(s));
    }
    for (const doc::DevicePtr& d : t.devices) {
      const uint32_t n = addNode(d->rack ? rackNode(bi, *d, tp) : deviceNode(bi, *d, tp));
      out.bands[bi].devices.push_back(n);
    }
    out.bands[bi].mixer = addNode(mixerNode(bi, t, lowered));
    if (t.kind == doc::TrackKind::Master) {
      Node o;
      o.kind = NodeKind::Output;
      o.band = bi;
      o.title = "Out";
      o.inputs.push_back({"in", ""});
      o.w = 56.0f;
      o.h = 16.0f;
      out.bands[bi].output = addNode(std::move(o));
      out.master = bi;
    }
    if (t.kind == doc::TrackKind::Group) {
      for (const doc::TrackPtr& c : t.children) buildBand(*c, static_cast<uint16_t>(depth + 1), static_cast<int32_t>(bi), b.active);
      if (out.bands.size() > bi + 1) out.bands[bi].lastMember = static_cast<uint32_t>(out.bands.size() - 1);
    }
  }

  // ---- edges ----------------------------------------------------------------------------------------------------

  void addEdge(EdgeKind k, uint32_t from, int32_t fromPort, uint32_t to, int32_t toPort, Edge proto = {}) {
    if (from == kNone || to == kNone || fromPort < 0 || toPort < 0) return;
    proto.kind = k;
    proto.from = from;
    proto.fromPort = static_cast<uint32_t>(fromPort);
    proto.to = to;
    proto.toPort = static_cast<uint32_t>(toPort);
    out.edges.push_back(std::move(proto));
  }

  int32_t audioIn(const Node& n) const noexcept {
    if (n.kind == NodeKind::Device) {
      if (n.instrument) return -1;
      return n.inputs.empty() ? -1 : 0;
    }
    return n.input("in");
  }

  void chainEdges(uint32_t bi) {
    const Band& b = out.bands[bi];
    if (b.graphTrack == kNone) return;
    std::vector<uint32_t> chain;
    if (b.kind == doc::TrackKind::Midi) {
      const uint32_t inst = b.devices.empty() ? kNone : b.devices[0];
      if (inst != kNone && out.nodes[inst].instrument && out.nodes[inst].lowered && b.role == graph::TrackRole::Instrument) {
        Edge e;
        e.voice = true;
        addEdge(EdgeKind::Notes, b.source, 0, inst, 0, e);
        chain.push_back(inst);
      }
    } else {
      chain.push_back(b.source);
    }
    for (const uint32_t d : b.devices)
      if (out.nodes[d].lowered && !out.nodes[d].instrument) chain.push_back(d);
    chain.push_back(b.mixer);
    for (size_t i = 0; i + 1 < chain.size(); ++i) {
      Edge e;
      e.voice = false;
      addEdge(EdgeKind::Chain, chain[i], 0, chain[i + 1], audioIn(out.nodes[chain[i + 1]]), e);
    }
    if (b.output != kNone) addEdge(EdgeKind::Chain, b.mixer, 0, b.output, 0);
  }

  void graphEdges() {
    std::unordered_map<uint32_t, uint32_t> bandOfGraph;  // graph track → band
    for (uint32_t i = 0; i < out.bands.size(); ++i)
      if (out.bands[i].graphTrack != kNone) bandOfGraph[out.bands[i].graphTrack] = i;
    std::unordered_set<uint64_t> delayed;
    for (const doc::DelayedSend& d : low.delayedSends) delayed.insert((uint64_t{d.from.value} << 32) | d.to.value);
    for (uint32_t gi = 0; gi < low.graph.wires.size(); ++gi) {
      const graph::ProjectWire& gw = low.graph.wires[gi];
      if (gw.kind != graph::WireKind::Audio) continue;  // modulation: from the document's wires below
      const auto fb = bandOfGraph.find(gw.from.track), tb = bandOfGraph.find(gw.to.track);
      if (fb == bandOfGraph.end() || tb == bandOfGraph.end()) continue;
      const Band& from = out.bands[fb->second];
      const Band& to = out.bands[tb->second];
      Edge e;
      e.graphWire = gi;
      if (gw.from.port == "out" && gw.to.port == "in") {
        addEdge(EdgeKind::Route, from.mixer, 0, to.source, out.nodes[to.source].input("in"), e);
      } else if ((gw.from.port == "post" || gw.from.port == "pre") && gw.to.port.size() > 1 && gw.to.port[0] == 's') {
        e.pre = gw.from.port == "pre";
        float level = doc::kMinVolumeDb;
        if (const auto ti = trackById.find(from.track.value); ti != trackById.end())
          for (const doc::Send& s : ti->second->mixer.sends)
            if (s.target == to.track) level = s.levelDb;
        e.silent = !(level > doc::kMinVolumeDb);
        e.delayed = delayed.count((uint64_t{from.track.value} << 32) | to.track.value) != 0;
        addEdge(EdgeKind::Send, from.mixer, out.nodes[from.mixer].output("s" + std::to_string(to.track.value)), to.source,
                out.nodes[to.source].input("in"), e);
      }
    }
    for (const doc::Wire& w : doc::wiresOf(p)) {
      const doc::WireStatus st = statusOf(w.id);
      if (st == doc::WireStatus::Invalid) continue;
      const uint32_t a = out.nodeOf(w.from), b = out.nodeOf(w.to);
      if (a == kNone || b == kNone) continue;
      Edge e;
      e.wire = w.id;
      e.status = st;
      e.delayed = st == doc::WireStatus::Delayed;
      e.voice = out.nodes[a].instrument;
      if (const auto it = modGraphWire.find(w.id.value); it != modGraphWire.end()) e.graphWire = it->second;
      addEdge(EdgeKind::Modulation, a, out.nodes[a].output(portKey(out.nodes[a], w.from, w.port)), b,
              out.nodes[b].input(portKey(out.nodes[b], w.to, w.param)), e);
    }
  }

  // ---- layout ---------------------------------------------------------------------------------------------------

  float placeChain(uint32_t bi, float x) {
    Band& b = out.bands[bi];
    std::vector<uint32_t> order;
    order.push_back(b.source);
    order.insert(order.end(), b.devices.begin(), b.devices.end());
    order.push_back(b.mixer);
    if (b.output != kNone) order.push_back(b.output);
    for (const uint32_t n : order) {
      out.nodes[n].x = x;
      x += out.nodes[n].w + m::kNodeGap;
    }
    return x - m::kNodeGap;
  }

  // Places a main-tree track (and its members) from the left; returns the right end of its chain.
  float placeMain(uint32_t bi) {
    Band& b = out.bands[bi];
    b.x0 = m::kLeft + m::kIndent * static_cast<float>(b.depth);
    float start = b.x0 + 8.0f;
    if (b.kind == doc::TrackKind::Group && b.lastMember != kNone) {
      float membersEnd = start;
      for (uint32_t c = bi + 1; c <= b.lastMember; ++c)
        if (out.bands[c].parent == static_cast<int32_t>(bi)) membersEnd = std::max(membersEnd, placeMain(c));
      start = std::max(start, membersEnd + m::kBusGap);
    } else if (b.kind == doc::TrackKind::Group) {
      start += m::kBusGap - 8.0f;
    }
    const float end = placeChain(bi, start);
    out.bands[bi].x1 = end + 12.0f;
    return end;
  }

  void layout() {
    // x
    float mainEnd = m::kLeft;
    for (uint32_t i = 0; i < out.bands.size(); ++i)
      if (out.bands[i].depth == 0 && (out.bands[i].kind == doc::TrackKind::Midi || out.bands[i].kind == doc::TrackKind::Audio ||
                                      out.bands[i].kind == doc::TrackKind::Group))
        mainEnd = std::max(mainEnd, placeMain(i));
    const float busRoom = 8.0f * static_cast<float>(p.returns.size());
    float retEnd = mainEnd;
    for (uint32_t i = 0; i < out.bands.size(); ++i)
      if (out.bands[i].kind == doc::TrackKind::Return) {
        out.bands[i].x0 = m::kLeft;
        const float end = placeChain(i, mainEnd + m::kBusGap + busRoom);
        out.bands[i].x1 = end + 12.0f;
        retEnd = std::max(retEnd, end);
      }
    if (out.master != kNone) {
      Band& b = out.bands[out.master];
      b.x0 = m::kLeft;
      const float end = placeChain(out.master, retEnd + m::kBusGap + 8.0f);
      b.x1 = end + 12.0f;
    }
    // y: lane order; every node's main pin on the band's spine.
    float y = 0.0f;
    for (Band& b : out.bands) {
      b.y = y;
      const float spine = y + m::kTitle + m::kPinTop;
      float bottom = y + m::kTitle + m::kSumH;
      auto place = [&](uint32_t n) {
        Node& nd = out.nodes[n];
        nd.y = spine - spineOf(nd);
        bottom = std::max(bottom, nd.y + nd.h);
      };
      place(b.source);
      for (const uint32_t d : b.devices) place(d);
      place(b.mixer);
      if (b.output != kNone) place(b.output);
      b.h = roundUp4(bottom + m::kBandPad - y);
      y += b.h + m::kBandGap;
    }
    // Nudges (view state): a band moves with its nodes, a device block alone.
    if (opt.nudges != nullptr && !opt.nudges->empty()) {
      for (Band& b : out.bands) {
        const auto it = opt.nudges->find(b.track.value);
        if (it == opt.nudges->end()) continue;
        const float dx = it->second.dx, dy = it->second.dy;
        b.y += dy;
        b.x0 += dx;
        b.x1 += dx;
        auto move = [&](uint32_t n) {
          if (n == kNone) return;
          out.nodes[n].x += dx;
          out.nodes[n].y += dy;
        };
        move(b.source);
        for (const uint32_t d : b.devices) move(d);
        move(b.mixer);
        move(b.output);
      }
      for (Node& n : out.nodes)
        if (n.kind == NodeKind::Device)
          if (const auto it = opt.nudges->find(n.device.value); it != opt.nudges->end()) {
            n.x += it->second.dx;
            n.y += it->second.dy;
          }
    }
    // Bands reach their nodes (a nudged block stays inside its band's strip horizontally).
    for (Band& b : out.bands) {
      auto reach = [&](uint32_t n) {
        if (n == kNone) return;
        b.x1 = std::max(b.x1, out.nodes[n].x + out.nodes[n].w + 12.0f);
      };
      reach(b.source);
      for (const uint32_t d : b.devices) reach(d);
      reach(b.mixer);
      reach(b.output);
    }
    // Group containers: the group's band and its members' (inner groups first: they nest).
    for (uint32_t i = static_cast<uint32_t>(out.bands.size()); i-- > 0;) {
      Band& b = out.bands[i];
      if (b.kind != doc::TrackKind::Group) continue;
      b.cx0 = b.x0 - 4.0f;
      b.cy0 = b.y - 4.0f;
      b.cx1 = b.x1 + 4.0f;
      b.cy1 = b.y + b.h + 4.0f;
      if (b.lastMember != kNone)
        for (uint32_t c = i + 1; c <= b.lastMember; ++c) {
          const Band& mb = out.bands[c];
          const float x0 = mb.kind == doc::TrackKind::Group ? mb.cx0 : mb.x0;
          const float x1 = mb.kind == doc::TrackKind::Group ? mb.cx1 : mb.x1;
          const float y1 = mb.kind == doc::TrackKind::Group ? mb.cy1 : mb.y + mb.h;
          b.cx0 = std::min(b.cx0, x0 - 4.0f);
          b.cx1 = std::max(b.cx1, x1 + 4.0f);
          b.cy1 = std::max(b.cy1, y1 + 4.0f);
        }
    }
    // Bounds.
    bool any = false;
    auto grow = [&](float x0, float y0, float x1, float y1) {
      if (!any) {
        out.x0 = x0, out.y0 = y0, out.x1 = x1, out.y1 = y1;
        any = true;
        return;
      }
      out.x0 = std::min(out.x0, x0), out.y0 = std::min(out.y0, y0), out.x1 = std::max(out.x1, x1), out.y1 = std::max(out.y1, y1);
    };
    for (const Band& b : out.bands) {
      grow(b.x0, b.y, b.x1, b.y + b.h);
      if (b.kind == doc::TrackKind::Group) grow(b.cx0, b.cy0, b.cx1, b.cy1);
    }
    for (const Node& n : out.nodes) grow(n.x, n.y - 12.0f, n.x + n.w, n.y + n.h);
  }

  // ---- routes ---------------------------------------------------------------------------------------------------

  graph::Point2 outPin(const Node& n, uint32_t i) const { return {n.x + n.w, n.y + pinY(n, i)}; }
  graph::Point2 inPin(const Node& n, uint32_t i) const { return {n.x, n.y + pinY(n, i)}; }

  static void simplify(std::vector<graph::Point2>& pts) {
    std::vector<graph::Point2> s;
    for (const graph::Point2& q : pts) {
      if (!s.empty() && s.back() == q) continue;
      if (s.size() >= 2) {
        const graph::Point2& a = s[s.size() - 2];
        const graph::Point2& b = s.back();
        if ((a.x == b.x && b.x == q.x) || (a.y == b.y && b.y == q.y)) {
          s.back() = q;
          continue;
        }
      }
      s.push_back(q);
    }
    pts = std::move(s);
  }

  void route() {
    // Each summing input's bus: its x, left of the Σ (returns' buses staggered so they do not overlap).
    std::unordered_map<uint32_t, float> busX;
    for (uint32_t i = 0; i < out.bands.size(); ++i) {
      const Band& b = out.bands[i];
      if (b.kind == doc::TrackKind::Midi || b.kind == doc::TrackKind::Audio) continue;  // no summing input
      float x = out.nodes[b.source].x - 16.0f;
      if (b.kind == doc::TrackKind::Return) {
        const auto ri = returnIndex.find(b.track.value);
        x -= 8.0f * static_cast<float>(ri == returnIndex.end() ? 0 : ri->second);
      }
      busX[b.source] = x;
    }
    uint32_t serial = 0;
    for (size_t k = 0; k < out.edges.size(); ++k) {
      Edge& e = out.edges[k];
      const Node& a = out.nodes[e.from];
      const Node& b = out.nodes[e.to];
      const graph::Point2 s = outPin(a, e.fromPort), t = inPin(b, e.toPort);
      std::vector<graph::Point2>& pts = e.points;
      pts.clear();
      pts.push_back(s);
      const uint32_t off = (serial++ % 4) * 3;
      switch (e.kind) {
        case EdgeKind::Notes:
        case EdgeKind::Chain:
          if (s.y != t.y && t.x > s.x) {
            const float mx = std::round((s.x + t.x) * 0.5f);
            pts.push_back({mx, s.y});
            pts.push_back({mx, t.y});
          } else if (t.x <= s.x) {  // a nudged block left of its source: around, below the band
            const Band& band = out.bands[a.band];
            const float gy = band.y + band.h - 4.0f;
            pts.push_back({s.x + 12.0f, s.y});
            pts.push_back({s.x + 12.0f, gy});
            pts.push_back({t.x - 12.0f, gy});
            pts.push_back({t.x - 12.0f, t.y});
          }
          e.net = 1u << 30 | static_cast<uint32_t>(k);
          break;
        case EdgeKind::Route:
        case EdgeKind::Send: {
          const float bx = busX.count(e.to) ? busX[e.to] : t.x - 16.0f;
          if (bx > s.x + 4.0f) {
            pts.push_back({bx, s.y});
            pts.push_back({bx, t.y});
          } else {  // the bus is left of the source (a return routed into a group): out right, over the band, back
            const Band& tb = out.bands[b.band];
            const float gy = tb.y - m::kBandGap * 0.5f;
            pts.push_back({s.x + 12.0f + static_cast<float>(off), s.y});
            pts.push_back({s.x + 12.0f + static_cast<float>(off), gy});
            pts.push_back({bx, gy});
            pts.push_back({bx, t.y});
          }
          e.net = e.to;  // one net per summing input: the bus shows junctions where its feeds join
          break;
        }
        case EdgeKind::Modulation: {
          const Band& tb = out.bands[b.band];
          const float o = static_cast<float>(off);
          if (a.band == b.band && t.x > s.x + 24.0f) {
            const float mx = std::round((s.x + t.x) * 0.5f) + o;
            pts.push_back({mx, s.y});
            pts.push_back({mx, t.y});
          } else {
            const float gy = tb.y + 4.0f + o;  // the target band's title strip (above its nodes)
            pts.push_back({s.x + 10.0f + o, s.y});
            pts.push_back({s.x + 10.0f + o, gy});
            pts.push_back({t.x - 10.0f - o, gy});
            pts.push_back({t.x - 10.0f - o, t.y});
          }
          e.net = 1u << 30 | static_cast<uint32_t>(k);
          break;
        }
      }
      pts.push_back(t);
      simplify(pts);
    }
  }

  void run() {
    prepare();
    for (const doc::TrackPtr& t : p.tracks) buildBand(*t, 0, -1, true);
    for (const doc::TrackPtr& r : p.returns) buildBand(*r, 0, -1, true);
    if (p.master) buildBand(*p.master, 0, -1, true);
    for (uint32_t i = 0; i < out.bands.size(); ++i) chainEdges(i);
    graphEdges();
    layout();
    route();
  }
};

}  // namespace

ProjectPatch buildProjectPatch(const doc::Project& project, const doc::LoweredProject& lowered,
                               const doc::DeviceCatalog* catalog, const LayoutOptions& options) {
  Builder b{project, lowered, catalog, options, {}, {}, {}, {}, {}, {}, {}, {}};
  b.run();
  return std::move(b.out);
}

// ---- the check against the graph ------------------------------------------------------------------------------------

std::string checkAgainstGraph(const ProjectPatch& pp, const doc::Project& project, const doc::LoweredProject& low) {
  const graph::ProjectGraph& g = low.graph;
  // Bands ↔ graph tracks.
  std::vector<uint32_t> bandOfGraph(g.tracks.size(), kNone);
  for (uint32_t i = 0; i < pp.bands.size(); ++i) {
    const Band& b = pp.bands[i];
    if (b.graphTrack == kNone) continue;
    if (b.graphTrack >= g.tracks.size()) return "band " + b.name + ": graph track out of range";
    if (bandOfGraph[b.graphTrack] != kNone) return "graph track " + std::to_string(b.graphTrack) + " has two bands";
    bandOfGraph[b.graphTrack] = i;
    if (g.tracks[b.graphTrack].id != b.track.value) return "band " + b.name + ": not its graph track's id";
    if (g.tracks[b.graphTrack].role != b.role) return "band " + b.name + ": role differs";
  }
  for (uint32_t t = 0; t < g.tracks.size(); ++t)
    if (bandOfGraph[t] == kNone) return "graph track " + g.tracks[t].name + " has no band";
  // Devices ↔ calls.
  for (uint32_t t = 0; t < g.tracks.size(); ++t) {
    const graph::Patch& tp = *g.tracks[t].patch;
    const Band& b = pp.bands[bandOfGraph[t]];
    uint32_t calls = 0;
    for (graph::NodeIndex n = 0; n < tp.nodeCount(); ++n) {
      const std::string& name = tp.nodeName(n);
      if (tp.node(n).kind != graph::NodeKind::Subcircuit || name.size() < 2 || name[0] != 'd' ||
          name.find_first_not_of("0123456789", 1) != std::string::npos)
        continue;
      ++calls;
      const doc::DeviceId id{static_cast<uint32_t>(std::stoul(name.substr(1)))};
      bool found = false;
      for (const uint32_t d : b.devices) {
        const Node& nd = pp.nodes[d];
        if (nd.rack && std::find(nd.inner.begin(), nd.inner.end(), id) != nd.inner.end()) {  // T4.8: inside a rack
          if (!nd.lowered) return "device " + name + " of " + b.name + ": inside a rack drawn as not lowered";
          if (tp.node(n).id.value != doc::nodeIdFor(doc::NodeRole::Device, id.value, 0))
            return "device " + name + " of " + b.name + ": its call is not the device's node";
          --calls;  // counted with its rack's block, not as a block of its own
          found = true;
          continue;
        }
        if (nd.device != id) continue;
        if (!nd.lowered || nd.call != tp.node(n).id) return "device " + name + " of " + b.name + ": not drawn as its call";
        found = true;
      }
      if (!found) return "device call " + name + " of " + b.name + " has no block";
    }
    uint32_t lowered = 0;
    for (const uint32_t d : b.devices) lowered += pp.nodes[d].lowered && !pp.nodes[d].rack ? 1u : 0u;
    if (lowered != calls) return "band " + b.name + ": " + std::to_string(lowered) + " lowered blocks, " + std::to_string(calls) + " calls";
  }
  for (const Band& b : pp.bands)
    if (b.graphTrack == kNone)
      for (const uint32_t d : b.devices)
        if (pp.nodes[d].lowered) return "band " + b.name + " is not lowered but has a lowered block";
  // Graph wires ↔ edges.
  std::vector<uint32_t> edgeOfWire(g.wires.size(), kNone);
  for (uint32_t k = 0; k < pp.edges.size(); ++k) {
    const Edge& e = pp.edges[k];
    if (e.graphWire == kNone) continue;
    if (e.graphWire >= g.wires.size()) return "edge " + std::to_string(k) + ": graph wire out of range";
    if (edgeOfWire[e.graphWire] != kNone) return "graph wire " + std::to_string(e.graphWire) + " drawn twice";
    edgeOfWire[e.graphWire] = k;
  }
  for (uint32_t w = 0; w < g.wires.size(); ++w) {
    const graph::ProjectWire& gw = g.wires[w];
    const std::string what = "graph wire " + g.tracks[gw.from.track].name + "." + gw.from.port + " → " +
                             g.tracks[gw.to.track].name + "." + gw.to.port;
    if (edgeOfWire[w] == kNone) return what + " is not drawn";
    const Edge& e = pp.edges[edgeOfWire[w]];
    const Node& a = pp.nodes[e.from];
    const Node& b = pp.nodes[e.to];
    if (a.band != bandOfGraph[gw.from.track] || b.band != bandOfGraph[gw.to.track]) return what + ": drawn between other bands";
    if (gw.kind == graph::WireKind::Audio && gw.from.port == "out" && gw.to.port == "in") {
      if (e.kind != EdgeKind::Route || a.kind != NodeKind::Mixer || a.outputs[e.fromPort].name != "out" || b.kind != NodeKind::Sum)
        return what + ": not a route from the mixer into the Σ";
    } else if (gw.kind == graph::WireKind::Audio && (gw.from.port == "post" || gw.from.port == "pre")) {
      if (e.kind != EdgeKind::Send || a.kind != NodeKind::Mixer || b.kind != NodeKind::Sum ||
          a.outputs[e.fromPort].name != "s" + std::to_string(g.tracks[gw.to.track].id) || e.pre != (gw.from.port == "pre"))
        return what + ": not a send from the mixer's send pin into the return's Σ";
    } else if (gw.kind == graph::WireKind::Modulation) {
      if (e.kind != EdgeKind::Modulation || gw.from.port != "m" + std::to_string(e.wire.value) || gw.to.port != gw.from.port)
        return what + ": not the modulation edge of its wire";
    } else {
      return what + ": a kind the view does not draw";
    }
  }
  // Every other edge is internal to a band, or a same-track modulation wire into a driven param pin.
  for (const Edge& e : pp.edges) {
    if (e.graphWire != kNone) continue;
    const Node& a = pp.nodes[e.from];
    const Node& b = pp.nodes[e.to];
    if (e.kind == EdgeKind::Route || e.kind == EdgeKind::Send) return "a route / send edge without a graph wire";
    const bool lowered = e.kind != EdgeKind::Modulation || e.status == doc::WireStatus::Lowered || e.status == doc::WireStatus::Delayed;
    if (!lowered) continue;  // an inactive / refused wire: drawn dim, not in the graph
    if (a.band != b.band) return "an edge across bands without a graph wire";
    if (e.kind == EdgeKind::Modulation) {
      const Band& band = pp.bands[a.band];
      if (band.graphTrack == kNone) return "a lowered modulation wire on a band without a graph track";
      const graph::Patch& tp = *g.tracks[band.graphTrack].patch;
      std::string pin = b.inputs[e.toPort].name;
      graph::NodeId callId = b.call;
      if (b.rack) {  // T4.8: "<param>@<device>" is that device's call inside the rack (no "@": the rack's own macro)
        const size_t at = pin.rfind('@');
        if (at == std::string::npos) continue;  // a macro: the rack's own param, read by its mappings
        callId = graph::NodeId{doc::nodeIdFor(doc::NodeRole::Device, static_cast<uint32_t>(std::stoul(pin.substr(at + 1))), 0)};
        pin.resize(at);
      }
      const auto call = tp.indexOf(callId);
      if (!call) return "a modulation wire into a block without a call";
      const auto in = tp.findInput(*call, pin);
      if (!in) return "a modulation wire into a param the call does not have";
      if (tp.adjacency().driver[tp.flatInput(*call, *in)] == graph::kNone)
        return "a modulation wire into a param pin the track patch leaves unconnected";
    }
  }
  // Groups contain their members.
  for (uint32_t i = 0; i < pp.bands.size(); ++i) {
    const Band& b = pp.bands[i];
    if (b.parent < 0) continue;
    const Band& gp = pp.bands[static_cast<size_t>(b.parent)];
    if (gp.kind != doc::TrackKind::Group) return "band " + b.name + ": parent is not a group";
    const doc::Track* gt = doc::findTrackPtr(project, gp.track);
    if (gt == nullptr || std::none_of(gt->children.begin(), gt->children.end(), [&](const doc::TrackPtr& c) { return c->id == b.track; }))
      return "band " + b.name + ": not a member of " + gp.name;
    if (b.x0 < gp.cx0 || b.x1 > gp.cx1 || b.y < gp.cy0 || b.y + b.h > gp.cy1) return "band " + b.name + ": outside its group's container";
  }
  return {};
}

}  // namespace pq::views::project_patch
