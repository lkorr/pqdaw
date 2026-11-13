// The project patch model (T4.13): the lowered ProjectGraph read back onto the document, and its automatic layout.
// See pq/views/project_patch/project_patch.h.

#include "pq/views/project_patch/project_patch.h"

#include <algorithm>
#include <optional>
#include <cmath>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "pq/document/instrument_groups.h"
#include "pq/document/main_path.h"
#include "pq/document/meta.h"
#include "pq/document/racks.h"
#include "pq/graph/layout.h"
#include "pq/graph/patch_file.h"
#include "pq/meta/registry.h"
#include "pq/views/meta/meta_face.h"
#include "pq/views/panel/group_panel_face.h"

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
  for (uint32_t i = 0; i < nodes.size(); ++i)  // T4.8: inside a rack; TX.groups: inside a closed group
    if (!nodes[i].inner.empty() && std::find(nodes[i].inner.begin(), nodes[i].inner.end(), d) != nodes[i].inner.end()) return i;
  return kNone;
}

namespace {

float roundUp4(float v) noexcept { return std::ceil(v / 4.0f) * 4.0f; }
float textW(std::string_view s) noexcept { return 6.0f * static_cast<float>(s.size()); }  // ≈ the 11-px UI font

// The y offset of a node's pin: the main one (index 0) at the spine, the rest below it.
float pinY(const Node& n, size_t i) noexcept { return pinOffset(n, i); }
// Where the spine (pin 0) sits from the node's top.
float spineOf(const Node& n) noexcept { return pinY(n, 0); }
// TX.mod-all: the pin rows a block needs (anchored params have none) and the height they take.
size_t pinRows(const Node& n) noexcept {
  size_t in = 0;
  for (const Port& q : n.inputs) in += q.anchored ? 0 : 1;
  return std::max<size_t>({in, n.outputs.size(), 1});
}
float rowsHeight(const Node& n) noexcept {
  return std::max(48.0f, metrics::kPinTop + metrics::kPinPitch * static_cast<float>(pinRows(n) - 1) + 16.0f);
}

std::string sendLetter(size_t returnIndex) {
  std::string s;
  size_t i = returnIndex;
  do {
    s.insert(s.begin(), static_cast<char>('A' + i % 26));
    i = i / 26;
  } while (i-- > 0);
  return s;
}

// PF.2: what a build derives from the document alone (no layout options): the wires by track and, per track, its main
// path, stages and racks. Valid while the snapshot it was made from is (ModelCache keeps it with its snapshot and
// lowering; pointers into the document).
struct DocDerived {
  bool indexed = false;
  std::unordered_map<uint32_t, uint32_t> graphIndex;
  std::unordered_map<uint32_t, size_t> returnIndex;
  std::unordered_map<uint32_t, std::vector<std::string>> modulatedParams;
  std::unordered_map<uint32_t, doc::WireStatus> wireStatus;
  std::unordered_map<uint32_t, uint32_t> modGraphWire;
  std::unordered_map<uint32_t, const doc::Track*> trackById;
  std::unordered_map<const graph::Patch*, std::unordered_set<std::string_view>> paramsOf;  // (patches of the lowering)
  // The project's wires by track (each wire under the track of its source device and of its target device, once per
  // track, in wiresOf order): a group's wires are among its track's, so the per-group passes read ~one track's wires
  // instead of the whole project's.
  std::unordered_map<uint32_t, std::vector<const doc::Wire*>> trackWires;
  std::unordered_map<uint32_t, uint32_t> groupOf;  // a grouped device's group (device ids)
  struct PerTrack {
    bool made = false;
    doc::MainPath path;                    // TX.all-free: its main path (doc::mainPath)
    std::vector<doc::ChainStage> stages;   // TMM.10-fix1: doc::trackStages (empty without a meta block or group)
    std::vector<doc::RackInfo> racks;      // GU.9: doc::findRacks
  };
  std::unordered_map<uint32_t, PerTrack> tracks;
  // A closed group's block: what is inside it at any depth, the ports its wires cross its edge by (as first added to
  // the block), the control its main input is added to (addedControl).
  struct Closed {
    bool made = false, addedMade = false;
    std::vector<doc::DeviceId> inner;
    std::vector<Port> ins, outs;
    std::string added;
  };
  std::unordered_map<uint32_t, Closed> closed;
  std::optional<doc::MainLinks> mainLinks;      // made once (one pass over the wires)
  std::optional<doc::VoiceContexts> contexts;  // GU.2: made at the first nested group (is it an instrument group?)
};

struct Builder {
  Builder(const doc::Project& project, const doc::LoweredProject& lowered, const doc::DeviceLibrary* lib,
          const LayoutOptions& options)
      : p(project), low(lowered), devices(lib), opt(options), derived(pickDerived(options)), graphIndex(derived->graphIndex),
        returnIndex(derived->returnIndex), modulatedParams(derived->modulatedParams), wireStatus(derived->wireStatus),
        modGraphWire(derived->modGraphWire), trackById(derived->trackById), paramsOf(derived->paramsOf) {}
  // PF.2: the document-derived tables of the last build when the snapshot and the lowering are the same ones; the
  // cache keeps this build's for the next.
  static std::shared_ptr<DocDerived> pickDerived(const LayoutOptions& o) {
    ModelCache* mc = o.layoutCache;
    std::shared_ptr<DocDerived> d;
    if (mc != nullptr && o.snapshot && o.lowering && mc->snapshot == o.snapshot && mc->lowering == o.lowering && mc->derived)
      d = std::static_pointer_cast<DocDerived>(mc->derived);
    else
      d = std::make_shared<DocDerived>();
    if (mc != nullptr) {
      mc->snapshot = o.snapshot;
      mc->lowering = o.lowering;
      mc->derived = o.snapshot && o.lowering ? d : nullptr;
    }
    return d;
  }

  const doc::Project& p;
  const doc::LoweredProject& low;
  const doc::DeviceLibrary* devices;
  const LayoutOptions& opt;
  ProjectPatch out;
  // PF.2: what the build derives from the document (and its lowering) alone, shared with the view's ModelCache while
  // the snapshot and the lowering stay the same (a re-layout: a resize, a nudge, a fold). The tables below are its.
  std::shared_ptr<DocDerived> derived;
  std::unordered_map<uint32_t, uint32_t>& graphIndex;   // track id → graph track
  std::unordered_map<uint32_t, size_t>& returnIndex;    // return track id → its index among the returns
  std::unordered_map<uint32_t, std::vector<std::string>>& modulatedParams;  // device id → params wires drive
  std::unordered_map<uint32_t, doc::WireStatus>& wireStatus;
  std::unordered_map<uint32_t, uint32_t>& modGraphWire;  // doc wire id → graph wire index (cross-track)
  std::unordered_map<uint32_t, const doc::Track*>& trackById;
  std::unordered_map<const graph::Patch*, std::unordered_set<std::string_view>>& paramsOf;
  // TMM.10-fix1: the band's stages (doc::trackStages): which meta device the note source reaches.
  const std::vector<doc::ChainStage>* bandStages = nullptr;
  // GU.9 (racks.h): a rack's chains off the path (node → the path chain it stacks under, in chain order) — drawn as
  // parallel group capsules under the chain on the path, the shared feed and sum their wires.
  std::unordered_map<uint32_t, uint32_t> stackUnder;
  std::vector<uint32_t> stackOrder;  // those nodes, each after the one it stacks under
  // TX.groups: each opened group's contents, placed relative to its capsule's top-left (layout() adds the capsule's
  // place, then the nudges).
  struct Capsule {
    uint32_t node = kNone;
    std::vector<std::pair<uint32_t, graph::Point2>> rel;
  };
  std::vector<Capsule> capsules;
  // PF.2: whether device `d` is `g` or inside it at any depth (its group chain, as doc::groupContains, by hash).
  bool within(doc::DeviceId d, doc::DeviceId g) const {
    uint32_t x = d.value;
    for (uint32_t depth = 0; depth <= doc::kMaxGroupDepth; ++depth) {
      if (x == g.value) return true;
      const auto it = derived->groupOf.find(x);
      if (it == derived->groupOf.end()) return false;
      x = it->second;
    }
    return false;
  }
  const std::vector<const doc::Wire*>& wiresOn(const doc::Track& t) const {
    static const std::vector<const doc::Wire*> none;
    const auto it = derived->trackWires.find(t.id.value);
    return it == derived->trackWires.end() ? none : it->second;
  }
  bool closedGroup(doc::DeviceId g) const noexcept {
    return !opt.scope.valid() && std::find(opt.closedGroups.begin(), opt.closedGroups.end(), g) != opt.closedGroups.end();
  }
  graph::Point2 nudgeOf(doc::DeviceId d) const noexcept {
    if (opt.nudges == nullptr) return {0.0f, 0.0f};
    const auto it = opt.nudges->find(d.value);
    return it == opt.nudges->end() ? graph::Point2{0.0f, 0.0f} : graph::Point2{it->second.dx, it->second.dy};
  }

  const graph::PatchLibrary* library() const noexcept { return low.graph.library; }

  void prepare() {
    if (derived->indexed) return;  // PF.2: the same snapshot and lowering as the last build
    derived->indexed = true;
    doc::forEachTrack(p, [&](const doc::Track& t, const doc::Track*) { trackById.emplace(t.id.value, &t); });
    for (size_t i = 0; i < low.trackIds.size(); ++i) graphIndex[low.trackIds[i].value] = static_cast<uint32_t>(i);
    for (size_t i = 0; i < p.returns.size(); ++i) returnIndex[p.returns[i]->id.value] = i;
    for (const doc::WireState& s : low.wires) wireStatus[s.id.value] = s.status;
    {
      auto& trackWires = derived->trackWires;
      std::unordered_map<uint32_t, uint32_t> trackOf;  // device id → track id
      for (const auto& [id, t] : trackById)
        for (const doc::DevicePtr& d : t->devices) {
          trackOf.emplace(d->id.value, id);
          if (d->group.valid()) derived->groupOf.emplace(d->id.value, d->group.value);
        }
      auto find = [&](doc::DeviceId d) -> uint32_t {
        if (!d.valid()) return 0xFFFFFFFFu;
        const auto it = trackOf.find(d.value);
        return it == trackOf.end() ? 0xFFFFFFFFu : it->second;
      };
      for (const doc::Wire& w : doc::wiresOf(p)) {
        const uint32_t a = w.fromNoteSource() ? 0xFFFFFFFFu : find(w.from), b = find(w.to);
        if (a != 0xFFFFFFFFu) trackWires[a].push_back(&w);
        if (b != 0xFFFFFFFFu && b != a) trackWires[b].push_back(&w);
      }
    }
    for (const doc::Wire& w : doc::wiresOf(p)) {
      if (statusOf(w.id) == doc::WireStatus::Invalid) continue;
      if (w.toMixer() || w.param.empty()) continue;  // TX.all-free: a main input (kMainPort) or a mixer: no param pin
      auto& v = modulatedParams[w.to.value];
      if (std::find(v.begin(), v.end(), w.param) == v.end()) v.push_back(w.param);
    }
    for (size_t gi = 0; gi < low.graph.wires.size(); ++gi) {
      const graph::ProjectWire& gw = low.graph.wires[gi];
      // (DY.2: a sidechain — a cable into another track's signal input — is the same document wire's port m<wire>)
      if ((gw.kind == graph::WireKind::Modulation || gw.kind == graph::WireKind::Sidechain) && gw.from.port.size() > 1 &&
          gw.from.port[0] == 'm')
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

  // ---- nodes ----------------------------------------------------------------------------------------------------

  // TX.resize: a closed device block at its stored scale (ViewState::patchNudges' scale): its rect, its pins' rows
  // (pinOffset) and its controls' anchors all `scale` times their own (what is drawn on it follows: SceneNode::scale).
  // An opened device keeps its size (its frame holds the atoms at the canvas's zoom). TX.group-resize: an opened group's
  // stored scale scales its contents instead (scaleCapsule).
  Node scaled(Node n) const {
    if (n.expanded || (n.group && n.groupOpen)) return n;
    if (const float k = storedScale(n.device); k != 1.0f) scaleBy(n, k);
    return n;
  }
  // A device's stored scale (its nudge's; 1: none), clamped.
  float storedScale(doc::DeviceId d) const {
    if (opt.nudges == nullptr || opt.nudges->empty()) return 1.0f;
    const auto it = opt.nudges->find(d.value);
    if (it == opt.nudges->end() || it->second.scale == 1.0f || !std::isfinite(it->second.scale)) return 1.0f;
    return std::clamp(it->second.scale, doc::kMinPatchScale, doc::kMaxPatchScale);
  }
  // Node `n` `k` times its size, composed with its own scale: its rect, its pins' rows (pinOffset follows Node::scale;
  // an atom block's symbol rows and stubs here), its anchors.
  static void scaleBy(Node& n, float k) {
    n.scale *= k;
    n.w *= k;
    n.h *= k;
    for (Port& q : n.inputs)
      if (q.anchored) q.ax *= k, q.ay *= k, q.aclear *= k;
    if (n.symbol) {
      auto sp = std::make_shared<circuit::SymbolSpec>(*n.symbol);
      sp->w *= k;
      sp->h *= k;
      for (circuit::PinSpec& q : sp->inputs) q.y *= k, q.stub *= k;
      for (circuit::PinSpec& q : sp->outputs) q.y *= k, q.stub *= k;
      n.symbol = std::move(sp);
    }
  }
  // TX.group-resize (user 2026-11-09: "shrinking them will comparatively shrink everything else within it"): opened
  // capsule `node`'s contents `k` times their size and their distances from its top-left inside its title strip and
  // pad (its frame follows them; the strip and pad stay, as at any size): each member and terminal scaled (scaleBy,
  // composed with its own scale), a nested opened group's contents with it (any depth; an opened device keeps its size).
  // After circuitLayout, so the layouter (and its PF.2 cache) sees the contents at their own sizes.
  void scaleCapsule(uint32_t node, float k) {
    Capsule* cap = nullptr;
    for (Capsule& c : capsules)
      if (c.node == node) cap = &c;
    if (cap == nullptr || cap->rel.empty()) return;
    const float ox = m::kCapsulePad, oy = m::kCapsuleTitle + m::kCapsulePad;
    float x1 = 0.0f, y1 = 0.0f;
    for (auto& [kid, r] : cap->rel) {
      r = {ox + (r.x - ox) * k, oy + (r.y - oy) * k};
      Node& kn = out.nodes[kid];
      if (kn.group && kn.groupOpen) scaleCapsule(kid, k);
      else if (!kn.expanded) scaleBy(kn, k);
      x1 = std::max(x1, r.x + out.nodes[kid].w);
      y1 = std::max(y1, r.y + out.nodes[kid].h);
    }
    Node& c = out.nodes[node];
    c.w = roundUp4(std::max(x1 + m::kCapsulePad, 2.0f * m::kCapsulePad + 8.0f));
    c.h = roundUp4(std::max(y1 + m::kCapsulePad, oy + m::kCapsulePad + 8.0f));
    c.pinTop = cap->rel.front().second.y + pinOffset(out.nodes[cap->rel.front().first], 0, false);
  }

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

  // GU.5b: an atom block drawn as the circuit editor's symbol of its atom (circuit::describe on the block's patch): its
  // size, and each of the block's ports at the pin of the atom port its terminal feeds (or is fed by).
  void atomSymbol(Node& n, const doc::Device& d) const {
    const graph::Patch* ap = doc::atomBlockPatch(doc::atomDeviceName(d));
    if (ap == nullptr) return;
    graph::NodeIndex atom = graph::kNone;
    for (graph::NodeIndex k = 0; k < ap->nodeCount(); ++k)
      if (ap->node(k).kind == graph::NodeKind::Atom) atom = k;
    if (atom == graph::kNone) return;
    const std::vector<uint8_t> wired(ap->node(atom).numInputs, 1);
    const circuit::SymbolSpec sp = circuit::describe(*ap, atom, wired);
    auto spec = std::make_shared<circuit::SymbolSpec>(sp);
    spec->inputs.clear();
    spec->outputs.clear();
    spec->caption.clear();
    auto pinOf = [&](std::string_view name, bool input) {
      circuit::PinSpec pin;
      pin.y = std::round(sp.h * 0.5f);
      pin.stub = 4.0f;
      for (const graph::Wire& w : ap->wires()) {
        const graph::NodeIndex term = input ? w.from.node : w.to.node;
        const graph::NodeKind want = input ? graph::NodeKind::Input : graph::NodeKind::Output;
        if ((input ? w.to.node : w.from.node) != atom || ap->node(term).kind != want || ap->nodeName(term) != name) continue;
        const std::vector<circuit::PinSpec>& pins = input ? sp.inputs : sp.outputs;
        const uint16_t port = input ? w.to.port : w.from.port;
        if (port < pins.size()) pin = pins[port];
      }
      pin.visible = true;
      return pin;
    };
    for (const Port& q : n.inputs) spec->inputs.push_back(pinOf(q.name, true));
    for (const Port& q : n.outputs) spec->outputs.push_back(pinOf(q.name, false));
    for (Port& q : n.inputs) q.label.clear();  // the symbol's own pin labels (an IC's) say it
    for (Port& q : n.outputs) q.label.clear();
    n.w = sp.w;
    n.h = sp.h;
    n.symbol = std::move(spec);
  }

  Node deviceNode(uint32_t band, const doc::Device& d, const graph::Patch* tp) {
    Node n;
    n.kind = NodeKind::Device;
    n.band = band;
    n.device = d.id;
    n.active = d.active;
    n.instrument = d.kind == doc::DeviceKind::Instrument;
    // TMM.10-fix1: a chain meta device takes notes exactly when the note source reaches its chain input (its stage;
    // unplugged, or after another device, it does not, whatever its kind): the note wire is drawn to it, or not.
    if (doc::isMetaDevice(d))
      for (const doc::ChainStage& s : *bandStages)
        if (s.device == &d) n.instrument = s.in == doc::ChainSignal::Notes;
    // The block's description (its name) from the library; its callee from what the graph calls, else the library.
    const doc::DeviceInfo* info = devices != nullptr ? devices->infoOf(d) : nullptr;
    // TX.node-menu: the device's own name when it has one (Ctrl+R; a node-menu atom block is named after its atom).
    n.title = !d.name.empty() ? d.name : info ? info->name : d.patch;
    const graph::NodeIndex call = callOf(tp, d.id);
    n.lowered = call != graph::kNone;
    if (n.lowered) n.call = tp->node(call).id;
    const graph::Patch* callee = nullptr;
    if (n.lowered && library() != nullptr) callee = library()->find(tp->subcircuit(call).patch);  // what the graph calls
    else if (devices != nullptr) callee = devices->patchOf(d);
    n.missing = callee == nullptr;
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
      // TX.patching: `<param>_add` is where a param's added wires enter (the VCA's value): drawn as the param's pin.
      if (params != nullptr && sp.name.size() > 4 && sp.name.ends_with("_add") &&
          params->count(std::string_view(sp.name).substr(0, sp.name.size() - 4)) != 0)
        continue;
      // The main input: no label (TX.all-free: every block's, on the path or off it).
      n.inputs.push_back({sp.name, firstAudio ? std::string() : sp.name});
      firstAudio = false;
    }
    if (sig.inputs.empty() && !n.instrument) n.inputs.push_back({"in", ""});
    if (modIt != modulatedParams.end())
      for (const std::string& prm : modIt->second) {
        if (n.input(prm) >= 0) continue;  // TX.patching: a wired input already has its pin
        const doc::DeviceParamInfo* pi = info ? info->param(prm) : nullptr;
        n.inputs.push_back({prm, pi && !pi->label.empty() ? pi->label : prm, true});
      }
    // TX.all-free: where a wire into its main port ends — a meta module's chain input (Device::chainInput), else the first.
    if (doc::isMetaDevice(d) && !n.instrument)
      if (const meta::Type* t = meta::findType(doc::metaDeviceType(d))) {
        const int32_t ci = doc::metaChainInput(d, *t);
        n.mainIn = ci < 0 ? -1 : n.input(t->inputs[static_cast<size_t>(ci)].name);
      }
    bool firstOut = true;
    for (const graph::SubcircuitPort& sp : sig.outputs) {
      n.outputs.push_back({sp.name, firstOut ? std::string() : sp.name});
      firstOut = false;
    }
    if (n.outputs.empty()) n.outputs.push_back({"out", ""});
    sizeBlock(n, 96.0f);
    // GU.5b: an atom block is its circuit symbol, a value block a small knob (project_patch.h Node::symbol / valueKnob).
    if (doc::isAtomDevice(d) && !n.expanded) atomSymbol(n, d);
    if (doc::isValueDevice(d) && !n.expanded) {
      n.valueKnob = true;
      n.groupPanel = true;
      std::erase_if(n.inputs, [](const Port& q) { return !q.param; });  // (the patch has no audio input)
      n.mainIn = -1;
      n.w = m::kValueKnobW;
      n.h = m::kValueKnobH;
      for (Port& q : n.inputs)
        if (q.name == doc::kValueParam) {  // a wire into its control ends on the knob (the knob element's centre)
          q.anchored = true;
          q.ax = n.w * 0.5f;
          q.ay = n.h * 0.5f;
          q.aclear = n.w * 0.5f;
        }
    }
    // TMM.4: a chain meta device is drawn as its face (its size; the pins on the face's edges at the band's rows).
    if (std::string_view(d.patch).starts_with(doc::kMetaDevicePrefix)) {
      n.meta = true;
      const meta::Type* t = meta::findType(std::string_view(d.patch).substr(doc::kMetaDevicePrefix.size()));
      if (t != nullptr && hasFace(*t) && !n.expanded) {
        const ui::Vec2 fs = faceSize(*t);
        n.face = t;
        n.w = roundUp4(fs.x);
        n.h = roundUp4(std::max(n.h, fs.y));
      }
    }
    // TX.panel-faces: a closed stock device with a panel is drawn as it (the Device view's panel at the block's size).
    graph::Point2 panelSize{0.0f, 0.0f};
    if (!n.meta && !n.missing && !n.symbol && !n.valueKnob && opt.panelSize) panelSize = opt.panelSize(d);
    // TX.mod-all: on a face or a panel a wired param whose control is visible takes no pin: its wires end on the control.
    // (Anchored ports are the last inputs, so the pins' rows stay where they were.)
    if ((n.face != nullptr || (panelSize.x > 0.0f && panelSize.y > 0.0f)) && opt.controlAnchor) {
      for (Port& q : n.inputs)
        if (q.param)
          if (const std::optional<LayoutOptions::Anchor> a = opt.controlAnchor(d, q.name)) {
            q.anchored = true;
            q.ax = a->x;
            q.ay = a->y;
            q.aclear = a->clear;
          }
      if (n.face != nullptr) {
        const ui::Vec2 fs = faceSize(*n.face);
        n.h = roundUp4(std::max(rowsHeight(n), fs.y));
      }
    }
    {
      const graph::Point2 ps = panelSize;
      if (ps.x > 0.0f && ps.y > 0.0f) {
        const size_t rows = pinRows(n);
        n.panel = true;
        n.w = roundUp4(ps.x);
        n.h = roundUp4(std::max(ps.y, m::kPinTop + m::kPinPitch * static_cast<float>(rows - 1) + 16.0f));
      }
    }
    if (n.expanded)
      for (const ExpandedSize& e : opt.expanded)
        if (e.device == d.id) {
          n.w = roundUp4(std::max(n.w, e.w + 2.0f * m::kExpandedPad));
          n.h = roundUp4(std::max(n.h, e.h + 2.0f * m::kExpandedPad + 16.0f));
        }
    return n;
  }

  // GU.5b (docs/plans/GU.md 5a): an opened group's contents laid out as a CIRCUIT by the circuit editor's layouter
  // (graph::autoLayout: layers by signal flow left to right -- the In terminal first, the Out last --, a crossing-reducing
  // order, straightened placement, channels between the layers), on a proxy patch of its blocks (their sizes and pin
  // rows) and the wires among them. `rel`: each content's place from the capsule's top-left (TX.cable-look: the wires
  // among them are hanging cables, so the layouter's routes are not used); the capsule's size and its chain pins' row
  // (the In terminal's) follow. PF.2: the layouter's result is cached by the view (ModelCache) under the exact
  // bytes of its inputs (`key`), so a re-layout that leaves this group's contents as they were (another block resized,
  // a nudge elsewhere) does not lay it out again; never per frame.
  void circuitLayout(const doc::Device& g, const doc::Track& t, const std::vector<uint32_t>& kids, Node& c,
                     std::vector<graph::Point2>& rel) {
    const size_t n = kids.size();
    // The proxy patch's port counts (as graph::Patch makes them): the In terminal an Input node (1 in, 1 out), the Out
    // terminal an Output node (1 in), a block a subcircuit of its ports.
    auto ins = [&](size_t i) -> uint32_t {
      return i == 0 || i + 1 == n ? 1u : static_cast<uint32_t>(std::min<size_t>(out.nodes[kids[i]].inputs.size(), graph::kMaxPorts));
    };
    auto outs = [&](size_t i) -> uint32_t {
      return i + 1 == n ? 0u : i == 0 ? 1u : static_cast<uint32_t>(std::min<size_t>(out.nodes[kids[i]].outputs.size(), graph::kMaxPorts));
    };
    auto inY = [&](const Node& k, uint32_t j) { return j < k.inputs.size() && k.inputs[j].anchored ? k.inputs[j].ay : pinOffset(k, j, true); };
    // PF.2: the layouter's inputs, serialised (the cache key): sizes, port names and rows, then the wires.
    std::string key;
    auto put = [&](const void* data, size_t bytes) { key.append(static_cast<const char*>(data), bytes); };
    auto putU = [&](uint32_t u) { put(&u, sizeof u); };
    auto putF = [&](float f) { put(&f, sizeof f); };
    auto putS = [&](std::string_view sv) {
      putU(static_cast<uint32_t>(sv.size()));
      put(sv.data(), sv.size());
    };
    putU(static_cast<uint32_t>(n));
    for (size_t i = 0; i < n; ++i) {
      const Node& k = out.nodes[kids[i]];
      const uint32_t ni = ins(i), no = outs(i);
      putF(k.w), putF(k.h), putU(ni), putU(no);
      for (uint32_t j = 0; j < ni; ++j) {
        putF(inY(k, j));
        if (i != 0 && i + 1 != n) putS(k.inputs[j].name);
      }
      for (uint32_t j = 0; j < no; ++j) {
        putF(pinOffset(k, j, false));
        if (i != 0 && i + 1 != n) putS(k.outputs[j].name);
      }
    }
    // The wires among the contents (one into a block inside a nested group counts as into that group).
    auto kidOf = [&](doc::DeviceId d, std::string_view port, bool source) -> int32_t {
      if (d == g.id) {
        if (port != (source ? doc::kGroupInPort : doc::kGroupOutPort)) return -1;
        return source ? 0 : static_cast<int32_t>(n) - 1;
      }
      if (!d.valid() || !within(d, g.id)) return -1;  // PF.2: most of the track's wires, at once
      for (size_t i = 1; i + 1 < n; ++i) {
        const Node& k = out.nodes[kids[i]];
        if (k.device == d || (k.group && within(d, k.device))) return static_cast<int32_t>(i);
      }
      return -1;
    };
    struct Link {
      uint32_t a, ap, b, bp;
    };
    std::vector<Link> links;
    for (const doc::Wire* wp : wiresOn(t)) {
      const doc::Wire& w = *wp;
      if (w.fromNoteSource() || statusOf(w.id) == doc::WireStatus::Invalid) continue;
      const int32_t a = kidOf(w.from, w.port, true), b = kidOf(w.to, w.param, false);
      if (a < 0 || b < 0 || a == b) continue;
      const size_t ua = static_cast<size_t>(a), ub = static_cast<size_t>(b);
      const Node& ka = out.nodes[kids[ua]];
      const Node& kb = out.nodes[kids[ub]];
      // A closed group's block names a port of a block inside it "<port>@<id>" (portKey); an opened one: its chain pin.
      int32_t ap = ua == 0 ? 0 : ka.output(portKey(ka, w.from, w.port));
      int32_t bp = ub + 1 == n ? 0 : kb.input(portKey(kb, w.to, w.param));
      if (w.port.empty() || ap < 0) ap = 0;
      if (w.param.empty()) bp = kb.mainIn > 0 ? kb.mainIn : 0;
      if (bp < 0) bp = kb.mainIn > 0 ? kb.mainIn : 0;
      if (static_cast<uint32_t>(ap) >= outs(ua) || static_cast<uint32_t>(bp) >= ins(ub)) continue;
      links.push_back({static_cast<uint32_t>(ua), static_cast<uint32_t>(ap), static_cast<uint32_t>(ub), static_cast<uint32_t>(bp)});
      putU(links.back().a), putU(links.back().ap), putU(links.back().b), putU(links.back().bp);
    }
    ModelCache* cache = opt.layoutCache;
    ModelCache::Entry* entry = nullptr;
    bool hit = false;
    if (cache != nullptr) {
      entry = &cache->entries[g.id.value];
      entry->used = cache->epoch;
      hit = entry->key == key && entry->pos.size() == n;
      ++(hit ? cache->hits : cache->misses);
    }
    std::vector<graph::Point2> fresh;
    std::vector<graph::Point2>& pos = entry != nullptr ? entry->pos : fresh;
    if (!hit) {
      graph::Patch lp;
      std::vector<graph::NodeIndex> v(n);
      for (size_t i = 0; i < n; ++i) {
        const Node& k = out.nodes[kids[i]];
        if (i == 0) v[i] = lp.addInput("in", graph::PortType{});
        else if (i + 1 == n) v[i] = lp.addOutput("out", graph::PortType{});
        else {
          graph::SubcircuitRef ref;
          ref.patch = "block";
          for (const Port& q : k.inputs) ref.inputs.push_back({q.name, graph::PortType{}, true});
          for (const Port& q : k.outputs) ref.outputs.push_back({q.name, graph::PortType{}, false});
          v[i] = lp.addSubcircuit(std::move(ref));
        }
      }
      graph::LayoutGeometry geo;
      geo.shape.resize(lp.nodeCount());
      geo.inPinY.assign(lp.portCount(), 0.0f);
      geo.outPinY.assign(lp.portCount(), 0.0f);
      for (size_t i = 0; i < n; ++i) {
        const Node& k = out.nodes[kids[i]];
        geo.shape[v[i]] = {k.w, k.h};
        for (uint32_t j = 0; j < lp.node(v[i]).numInputs; ++j) geo.inPinY[lp.flatInput(v[i], j)] = inY(k, j);
        for (uint32_t j = 0; j < lp.node(v[i]).numOutputs; ++j) geo.outPinY[lp.flatOutput(v[i], j)] = pinOffset(k, j, false);
      }
      for (const Link& l : links)
        (void)lp.connect(graph::PortRef{v[l.a], static_cast<uint16_t>(l.ap)}, graph::PortRef{v[l.b], static_cast<uint16_t>(l.bp)});
      graph::AutoLayoutOptions lo;
      lo.layerGap = 20.0f;
      lo.nodeGap = 20.0f;
      (void)graph::autoLayout(lp, geo, lo);
      pos.clear();
      for (size_t i = 0; i < n; ++i) pos.push_back({lp.layout(v[i]).x, lp.layout(v[i]).y});
      if (entry != nullptr) entry->key = std::move(key);
    }
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (size_t i = 0; i < n; ++i) {
      const graph::Point2 l = pos[i];
      x0 = std::min(x0, l.x), y0 = std::min(y0, l.y);
      x1 = std::max(x1, l.x + out.nodes[kids[i]].w), y1 = std::max(y1, l.y + out.nodes[kids[i]].h);
    }
    const float ox = m::kCapsulePad - x0, oy = m::kCapsuleTitle + m::kCapsulePad - y0;
    rel.clear();
    for (size_t i = 0; i < n; ++i) rel.push_back({pos[i].x + ox, pos[i].y + oy});
    c.w = roundUp4(std::max(x1 - x0 + 2.0f * m::kCapsulePad, 96.0f));
    c.h = roundUp4(std::max(y1 - y0 + m::kCapsuleTitle + 2.0f * m::kCapsulePad, m::kCapsuleTitle + 48.0f));
    c.pinTop = rel[0].y + pinOffset(out.nodes[kids[0]], 0, false);
  }

  // TX.groups: a group device's capsule (project_patch.h Node::group) and, opened, its terminals and members. GU.2: a
  // member that is a group is drawn the same way inside it (recursively): opened, its capsule inside the parent's with
  // its own terminals and members; closed, a block of the parent's. The capsule node is added before its contents (it is
  // painted under them and hit after them). An instrument group (only the outermost group fed notes: doc::voiceContexts)
  // is drawn as an instrument (Node::instrument, its polyphony).
  // TX.cable-look: the control (its key on the group's block, "value@<id>") a closed group's main input is added to
  // inside: every wire from its In goes into one `add` atom whose only other feed is a value block (the Phase
  // Accumulator: In -> `hz + freq` <- Frequency), while a wire enters that main input. "" when nothing is wired into it
  // (its port stays to be plugged) or the signal does anything else inside (it keeps its pin).
  std::string addedControl(const doc::Device& g, const doc::Track& t, const Node& c) const {
    const std::vector<const doc::Wire*>& wires = wiresOn(t);  // PF.2: every wire here touches g or a device inside it
    if (std::none_of(wires.begin(), wires.end(), [&](const doc::Wire* w) {
          return !w->toMixer() && w->to == g.id && w->param.empty() && statusOf(w->id) != doc::WireStatus::Invalid;
        }))
      return {};
    const doc::Device* sum = nullptr;
    for (const doc::Wire* wp : wires) {
      const doc::Wire& w = *wp;
      if (w.fromNoteSource() || w.from != g.id || w.port != doc::kGroupInPort || statusOf(w.id) == doc::WireStatus::Invalid) continue;
      const doc::Device* d = doc::findDevice(p, w.to).device;
      if (d == nullptr || doc::atomDeviceName(*d) != "add" || (sum != nullptr && sum != d)) return {};
      sum = d;
    }
    if (sum == nullptr) return {};
    const doc::Device* value = nullptr;
    for (const doc::Wire* wp : wires) {
      const doc::Wire& w = *wp;
      if (w.to != sum->id || statusOf(w.id) == doc::WireStatus::Invalid || (w.from == g.id && w.port == doc::kGroupInPort)) continue;
      const doc::Device* d = w.fromNoteSource() ? nullptr : doc::findDevice(p, w.from).device;
      if (d == nullptr || !doc::isValueDevice(*d) || !(w.port.empty() || w.port == doc::kValueOutput) || value != nullptr) return {};
      value = d;
    }
    return value == nullptr ? std::string() : portKey(c, value->id, std::string(doc::kValueParam));
  }

  uint32_t groupNode(uint32_t band, const doc::Device& g, const doc::Track& t, const graph::Patch* tp) {
    Node c;
    c.kind = NodeKind::Device;
    c.band = band;
    c.device = g.id;
    c.active = g.active;
    c.group = true;
    c.groupOpen = !closedGroup(g.id);
    // A top-level group: its stage says it (the notes reach its In: IG.1-2's rule, no extra pass); a nested one (rare)
    // asks the voice contexts, made once per build at the first nested group.
    if (!g.group.valid()) {
      for (const doc::ChainStage& s : *bandStages)
        if (s.device == &g) c.instrument = s.in == doc::ChainSignal::Notes;
    } else {
      if (!derived->contexts) derived->contexts.emplace(doc::voiceContexts(p));
      c.instrument = derived->contexts->blockOf(g.id) != nullptr;
    }
    if (c.instrument) {
      const doc::GroupPolyphony gp = doc::groupPolyphony(g);
      c.voices = gp.mono ? 1u : gp.voices;
    }
    c.title = g.name.empty() ? std::string("Group") : g.name;
    const graph::NodeIndex call = callOf(tp, g.id);
    c.lowered = call != graph::kNone;
    if (c.lowered) c.call = tp->node(call).id;
    c.inputs.push_back({"x", ""});
    c.outputs.push_back({"y", ""});
    const std::vector<const doc::Device*> members = doc::groupMembers(t.devices, g.id);
    auto terminal = [&](bool in) {
      Node k;
      k.kind = NodeKind::Terminal;
      k.band = band;
      k.device = g.id;
      k.title = in ? "In" : "Out";
      if (in) k.outputs.push_back({std::string(doc::kGroupInPort), ""});
      else k.inputs.push_back({std::string(doc::kGroupOutPort), ""});
      k.w = m::kTerminalW;
      k.h = m::kTerminalH;
      k.lowered = c.lowered;
      k.call = c.call;
      return k;
    };
    // The inner layout (relative to the capsule): In, the members in list order, Out, on one spine; members' nudges.
    auto innerLayout = [&](const std::vector<const Node*>& kids, std::vector<graph::Point2>& rel) {
      float spine = 0.0f;
      for (const Node* k : kids) spine = std::max(spine, spineOf(*k));
      const float innerSpine = m::kCapsuleTitle + m::kCapsulePad + spine;
      float x = m::kCapsulePad, bottom = m::kCapsuleTitle + 48.0f;
      for (const Node* k : kids) {
        rel.push_back({x, innerSpine - spineOf(*k)});
        bottom = std::max(bottom, rel.back().y + k->h);
        x += k->w + m::kNodeGap * 0.75f;
      }
      c.w = roundUp4(x - m::kNodeGap * 0.75f + m::kCapsulePad);
      c.h = roundUp4(bottom + m::kCapsulePad);
      c.pinTop = innerSpine;
    };
    if (!c.groupOpen) {
      // Closed: one block holding a miniature of its own contents (each box and wire in its unit square); a nested group
      // is one box there. PF.2: drawn as its panel, it has no miniature (not made).
      if (!g.hasPanel()) {
        std::vector<Node> kids;
        kids.push_back(terminal(true));
        for (const doc::Device* d : members) {
          if (doc::isGroupDevice(*d)) {
            Node b;
            b.kind = NodeKind::Device;
            b.device = d->id;
            b.title = d->name.empty() ? std::string("Group") : d->name;
            b.w = m::kClosedGroupW;
            b.h = m::kClosedGroupH;
            kids.push_back(std::move(b));
          } else {
            kids.push_back(scaled(deviceNode(band, *d, tp)));
          }
        }
        kids.push_back(terminal(false));
        std::vector<const Node*> kp;
        for (const Node& k : kids) kp.push_back(&k);
        std::vector<graph::Point2> rel;
        innerLayout(kp, rel);
        float x0 = 0.0f, y0 = 0.0f, x1 = c.w, y1 = c.h;
        for (size_t i = 0; i < kids.size(); ++i) {
          const graph::Point2 nd = i == 0 || i + 1 == kids.size() ? graph::Point2{0.0f, 0.0f} : nudgeOf(kids[i].device);
          rel[i].x += nd.x;
          rel[i].y += nd.y;
          x0 = std::min(x0, rel[i].x - m::kCapsulePad), y0 = std::min(y0, rel[i].y - m::kCapsulePad);
          x1 = std::max(x1, rel[i].x + kids[i].w + m::kCapsulePad), y1 = std::max(y1, rel[i].y + kids[i].h + m::kCapsulePad);
        }
        const float sw = std::max(1.0f, x1 - x0), sh = std::max(1.0f, y1 - y0);
        for (size_t i = 0; i < kids.size(); ++i)
          c.mini.push_back({(rel[i].x - x0) / sw, (rel[i].y - y0) / sh, (rel[i].x + kids[i].w - x0) / sw,
                            (rel[i].y + kids[i].h - y0) / sh, kids[i].kind == NodeKind::Terminal, kids[i].title});
        // The box a device of the group stands in: its own, or the nested group's it sits in (any depth).
        auto kidOf = [&](doc::DeviceId d, std::string_view port, bool source) -> int32_t {
          if (d == g.id)
            return port == (source ? doc::kGroupInPort : doc::kGroupOutPort) ? (source ? 0 : static_cast<int32_t>(kids.size()) - 1) : -1;
          if (!d.valid() || !within(d, g.id)) return -1;  // PF.2
          for (size_t i = 1; i + 1 < kids.size(); ++i)
            if (kids[i].device == d || (doc::isGroupDevice(*members[i - 1]) && within(d, kids[i].device)))
              return static_cast<int32_t>(i);
          return -1;
        };
        for (const doc::Wire* wp : wiresOn(t)) {
          const doc::Wire& w = *wp;
          if (w.fromNoteSource() || statusOf(w.id) == doc::WireStatus::Invalid) continue;
          const int32_t a = kidOf(w.from, w.port, true), b = kidOf(w.to, w.param, false);
          if (a < 0 || b < 0 || a == b) continue;
          const Node::MiniBox& ma = c.mini[static_cast<size_t>(a)];
          const Node::MiniBox& mb = c.mini[static_cast<size_t>(b)];
          c.miniWires.push_back({ma.x1, (ma.y0 + ma.y1) * 0.5f});
          c.miniWires.push_back({mb.x0, (mb.y0 + mb.y1) * 0.5f});
        }
      }
      // Everything inside it at any depth (wires to and from them end on the block, as a rack's do) and their ports that
      // cross its edge.
      // PF.2: found once per document (DocDerived::closed): the ports it gains here depend on the wires alone.
      DocDerived::Closed& cd = derived->closed[g.id.value];
      if (!cd.made) {
        cd.made = true;
        const size_t in0 = c.inputs.size(), out0 = c.outputs.size();
        for (const doc::Device* d : doc::groupMembersDeep(t.devices, g.id)) cd.inner.push_back(d->id);
        auto inside = [&](doc::DeviceId d) { return d.valid() && within(d, g.id); };  // g or in cd.inner
        for (const doc::Wire* wp : wiresOn(t)) {
          const doc::Wire& w = *wp;
          if (statusOf(w.id) == doc::WireStatus::Invalid) continue;
          const bool from = !w.fromNoteSource() && inside(w.from), to = inside(w.to);
          if (from == to) continue;  // inside (or outside) the group altogether
          if (to && w.to != g.id) {
            const std::string key = portKey(c, w.to, w.param);
            if (c.input(key) < 0) c.inputs.push_back({key, w.param, true});
          }
          if (from && w.from != g.id) {
            const std::string key = portKey(c, w.from, w.port);
            if (c.output(key) < 0) c.outputs.push_back({key, w.port});
          }
        }
        cd.ins.assign(c.inputs.begin() + static_cast<std::ptrdiff_t>(in0), c.inputs.end());
        cd.outs.assign(c.outputs.begin() + static_cast<std::ptrdiff_t>(out0), c.outputs.end());
      } else {
        c.inputs.insert(c.inputs.end(), cd.ins.begin(), cd.ins.end());
        c.outputs.insert(c.outputs.end(), cd.outs.begin(), cd.outs.end());
      }
      c.inner = cd.inner;
      // GU.5: drawn as its panel, a wire into a control inside the group that an element shows ends on that element (no
      // pin: TX.mod-all's anchored port, its key "<name>@<id>" as the panel face answers it). Anchored ports go last, so
      // the pins' rows stay where they were.
      if (g.hasPanel() && opt.controlAnchor) {
        for (Port& q : c.inputs)
          if (q.param)
            if (const std::optional<LayoutOptions::Anchor> a = opt.controlAnchor(g, q.name)) {
              q.anchored = true;
              q.ax = a->x;
              q.ay = a->y;
              q.aclear = a->clear;
            }
        // TX.cable-look (the user: the notes "should connect to the frequency component of the phase accumulator, as
        // its supposed to be just adding to the frequency"): the main input lands on the control its signal is added to
        // inside (addedControl), as that control's own wires do. Visual only: the wire still enters the main input.
        if (!cd.addedMade) {
          cd.addedMade = true;
          cd.added = addedControl(g, t, c);
        }
        if (const std::string& key = cd.added; !key.empty())
          if (const std::optional<LayoutOptions::Anchor> a = opt.controlAnchor(g, key)) {
            Port& q = c.inputs[static_cast<size_t>(c.mainIn)];
            q.anchored = true;
            q.ax = a->x;
            q.ay = a->y;
            q.aclear = a->clear;
            q.anchorKey = key;
          }
        const std::string mainName = c.inputs[static_cast<size_t>(c.mainIn)].name;
        std::stable_partition(c.inputs.begin(), c.inputs.end(), [](const Port& q) { return !q.anchored; });
        c.mainIn = c.input(mainName);
      }
      c.pinTop = 0.0f;
      sizeBlock(c, m::kClosedGroupW);
      c.h = roundUp4(std::max(c.h, m::kClosedGroupH));
      if (g.hasPanel()) {  // GU.3: drawn as its panel (at the panel's size; its pins on the edges)
        c.groupPanel = true;
        if (const std::optional<FaceShape> ps = panel::panelShape(*g.panel, &t.devices); ps && ps->round) {  // GU.5b: a round panel
          c.panelRound = true;
          c.roundX = ps->centre.x;
          c.roundY = ps->centre.y;
          c.roundR = ps->radius;
        }
        c.mini.clear();
        c.miniWires.clear();
        const size_t rows = pinRows(c);
        c.w = roundUp4(std::max(g.panel->width, 24.0f));
        c.h = roundUp4(std::max(g.panel->height, m::kPinTop + m::kPinPitch * static_cast<float>(rows - 1) + 16.0f));
      }
      return addNode(scaled(std::move(c)));
    }
    // Opened: the capsule first, then In, the members (a nested group recursively: its capsule and contents), Out.
    const uint32_t ci = addNode(Node{});
    const size_t capIndex = capsules.size();
    capsules.emplace_back();
    capsules[capIndex].node = ci;
    std::vector<uint32_t> kids;
    kids.push_back(addNode(terminal(true)));
    for (const doc::Device* d : members)
      kids.push_back(doc::isGroupDevice(*d) ? groupNode(band, *d, t, tp) : addNode(scaled(deviceNode(band, *d, tp))));
    kids.push_back(addNode(terminal(false)));
    std::vector<graph::Point2> rel;
    circuitLayout(g, t, kids, c, rel);  // GU.5b: a circuit, not a row
    out.nodes[ci] = std::move(c);
    Node& cn = out.nodes[ci];
    for (size_t i = 0; i < kids.size(); ++i) {
      out.nodes[kids[i]].capsule = ci;
      capsules[capIndex].rel.push_back({kids[i], rel[i]});
      if (i == 0) cn.groupIn = kids[i];
      else if (i + 1 == kids.size()) cn.groupOut = kids[i];
      else cn.members.push_back(kids[i]);
    }
    if (const float k = storedScale(g.id); k != 1.0f) scaleCapsule(ci, k);  // TX.group-resize
    return ci;
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
    n.w = expanded ? m::kMixerWideW : m::kMixerW;  // TX.mixer-look: expanded, longer send faders and their levels
    n.h = sends == 0 ? m::kMixerH : roundUp4(m::kMixerSendTop + m::kPinPitch * static_cast<float>(sends - 1) + 9.0f);
    n.expanded = expanded;
    // TL.4: its scale (the default 2×, times the track's stored factor), the whole face in proportion.
    float k = m::kMixerScale;
    if (opt.nudges != nullptr)
      if (const auto it = opt.nudges->find(t.id.value); it != opt.nudges->end() && std::isfinite(it->second.scale))
        k *= it->second.scale;
    k = std::clamp(k, doc::kMinPatchScale, doc::kMaxPatchScale);
    n.scale = k;
    n.w *= k;
    n.h *= k;
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
    // TL.1: the band's lane (its height and whether its circuit is folded: view state, by track id). A scoped model (a
    // group's panel, the free rack) has no lanes.
    if (lanes()) {
      band.laneH = m::kLaneH;
      if (opt.nudges != nullptr)
        if (const auto it = opt.nudges->find(t.id.value); it != opt.nudges->end()) {
          if (it->second.laneH > 0.0f) band.laneH = roundUp4(std::clamp(it->second.laneH, std::max(doc::kMinPatchLaneH, m::kLaneMinH), doc::kMaxPatchLaneH));
          band.folded = it->second.folded;
        }
      if (opt.subLanes != nullptr)  // TL.2: its automation sub-lanes under the lane
        if (const auto it = opt.subLanes->find(t.id.value); it != opt.subLanes->end()) band.subH = std::max(0.0f, it->second);
    }
    if (t.kind == doc::TrackKind::Midi) {
      // TL.1: the track's note source is its lane (the node's rect is the strip: layout()); its notes and (TMM.10-fix2)
      // its note outputs, each a wire source (as a device's export) onto any device param.
      Node s;
      s.kind = NodeKind::Lane;
      s.band = bi;
      s.title = "Notes";
      s.outputs.push_back({"notes", ""});
      for (const doc::NoteOutput& o : doc::kNoteOutputs) s.outputs.push_back({std::string(o.port), std::string(o.pin)});
      band.source = addNode(std::move(s));
    } else if (t.kind == doc::TrackKind::Audio) {  // T5.3: the clip player (graph: clipPlayer → its chain)
      Node s;
      s.kind = NodeKind::Lane;
      s.band = bi;
      s.title = "Audio";
      s.outputs.push_back({"out", ""});
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
    DocDerived::PerTrack& td = derived->tracks[t.id.value];
    if (!td.made) {  // PF.2: once per document (DocDerived)
      td.made = true;
      if (!derived->mainLinks) derived->mainLinks.emplace(p);
      td.path = doc::mainPath(t, *derived->mainLinks);
      // (Only a meta block or a group reads its stage: a track without one skips the walk.)
      if (std::any_of(t.devices.begin(), t.devices.end(), [](const doc::DevicePtr& d) { return doc::isMetaDevice(*d) || doc::isGroupDevice(*d); }))
        td.stages = doc::trackStages(p, t, td.path);
      td.racks = doc::findRacks(p, t);
    }
    const doc::MainPath& path = td.path;
    bandStages = &td.stages;
    for (const doc::DevicePtr& d : t.devices) {
      // TX.groups: a member is drawn inside its group's capsule (or, closed, is part of its block).
      if (d->group.valid() && t.deviceIndex(d->group) >= 0) continue;
      if (doc::isGroupDevice(*d)) {
        out.bands[bi].devices.push_back(groupNode(bi, *d, t, tp));
        continue;
      }
      const uint32_t n = addNode(scaled(deviceNode(bi, *d, tp)));
      out.bands[bi].devices.push_back(n);
    }
    for (const uint32_t n : out.bands[bi].devices) out.nodes[n].offPath = !path.contains(out.nodes[n].device);  // TX.all-free
    // GU.9: the track-level racks: their chains off the path stack under the one on it; their macros and selector stay
    // on the free row (blocks wired into the chains) but belong to the rack (the Device view's free rack leaves them out).
    for (const doc::RackInfo& r : td.racks) {
      if (r.parent.valid()) continue;
      uint32_t anchor = kNone;
      std::vector<uint32_t> rest;
      for (const doc::RackChain& c : r.chains)
        for (const uint32_t n : out.bands[bi].devices)
          if (out.nodes[n].device == c.id) {
            out.nodes[n].rack = true;
            if (anchor == kNone && path.contains(c.id)) anchor = n;
            else rest.push_back(n);
          }
      for (const uint32_t n : out.bands[bi].devices) {
        const doc::DeviceId d = out.nodes[n].device;
        if (d == r.selector || std::any_of(r.macros.begin(), r.macros.end(), [&](const doc::RackMacro& m) { return m.block == d; }))
          out.nodes[n].rack = true;
      }
      if (anchor == kNone) continue;
      uint32_t under = anchor;
      for (const uint32_t n : rest) {
        out.nodes[n].offPath = false;
        stackUnder[n] = under;
        stackOrder.push_back(n);
        under = n;
      }
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

  // TX.all-free: the master's mixer into Out (the only edge that is no document wire).
  void outputEdge(uint32_t bi) {
    const Band& b = out.bands[bi];
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
      if (gw.kind != graph::WireKind::Audio) continue;  // modulation, sidechains (DY.2): from the document's wires below
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
    // TX.all-free: every document wire is a cable (the main path's links included): from a device's port (kMainPort: its
    // main output), a track's source (kMainPort: its own signal — notes, clips, its sum —, else a note output) into a
    // device's param or input (kMainPort: its main input) or a track's mixer.
    // The node of each device (ProjectPatch::nodeOf's answer, once: every wire looks up both its ends).
    std::unordered_map<uint32_t, uint32_t> nodeOfDevice;
    for (uint32_t i = 0; i < out.nodes.size(); ++i)
      if (out.nodes[i].kind == NodeKind::Device) nodeOfDevice.try_emplace(out.nodes[i].device.value, i);
    for (uint32_t i = 0; i < out.nodes.size(); ++i)
      for (const doc::DeviceId d : out.nodes[i].inner) nodeOfDevice.try_emplace(d.value, i);
    auto nodeOf = [&](doc::DeviceId d) {
      const auto it = nodeOfDevice.find(d.value);
      return it == nodeOfDevice.end() ? kNone : it->second;
    };
    std::unordered_map<uint32_t, uint32_t> bandOfTrack;
    for (uint32_t i = 0; i < out.bands.size(); ++i) bandOfTrack.try_emplace(out.bands[i].track.value, i);
    auto bandOf = [&](doc::TrackId t) {
      const auto it = bandOfTrack.find(t.value);
      return it == bandOfTrack.end() ? kNone : it->second;
    };
    for (const doc::Wire& w : doc::wiresOf(p)) {
      const doc::WireStatus st = statusOf(w.id);
      if (st == doc::WireStatus::Invalid) continue;
      uint32_t a = kNone;
      int32_t fromPort = -1;
      bool fromMidi = false;
      if (w.fromNoteSource()) {
        const uint32_t sb = bandOf(w.noteSource);
        if (sb != kNone && out.bands[sb].source != kNone) {
          a = out.bands[sb].source;
          fromMidi = out.bands[sb].kind == doc::TrackKind::Midi;
          fromPort = w.port.empty() ? 0 : out.nodes[a].output(w.port);
        }
      } else {
        a = nodeOf(w.from);
        // TX.groups: the In terminal of an opened group.
        if (a != kNone && out.nodes[a].group && out.nodes[a].device == w.from && w.port == doc::kGroupInPort && out.nodes[a].groupOpen)
          a = out.nodes[a].groupIn;
        if (a != kNone) fromPort = w.port.empty() && out.nodes[a].device == w.from ? 0 : out.nodes[a].output(portKey(out.nodes[a], w.from, w.port));
      }
      uint32_t b = kNone;
      int32_t toPort = -1;
      if (w.toMixer()) {
        const uint32_t tb = bandOf(w.mixer);
        if (tb != kNone && out.bands[tb].mixer != kNone) {
          b = out.bands[tb].mixer;
          toPort = 0;
        }
      } else {
        b = nodeOf(w.to);
        if (b != kNone && out.nodes[b].group && out.nodes[b].device == w.to && w.param == doc::kGroupOutPort && out.nodes[b].groupOpen)
          b = out.nodes[b].groupOut;  // TX.groups: the Out terminal
        if (b != kNone)
          toPort = w.param.empty() && out.nodes[b].device == w.to ? out.nodes[b].mainIn
                                                                  : out.nodes[b].input(portKey(out.nodes[b], w.to, w.param));
      }
      if (a == kNone || b == kNone) continue;
      if (a == b && out.nodes[a].group) continue;  // TX.groups: inside a closed group (its miniature draws it)
      Edge e;
      e.wire = w.id;
      e.status = st;
      // TL.1: a cable from a lane leaves its top edge at the wire's tap (band-local here; world in taps()).
      if (out.nodes[a].kind == NodeKind::Lane && w.tap) {
        e.tapPinned = true;
        e.tapX = *w.tap;
      }
      e.delayed = st == doc::WireStatus::Delayed;
      e.voice = (w.fromNoteSource() && fromMidi) || out.nodes[a].instrument;
      if (const auto it = modGraphWire.find(w.id.value); it != modGraphWire.end()) e.graphWire = it->second;
      e.input = toPort >= 0 && (out.nodes[b].kind != NodeKind::Device || !out.nodes[b].inputs[static_cast<size_t>(toPort)].param);
      // Its kind (how it is drawn): notes into a main input, an audio path inside the band, or modulation.
      const bool sameBand = out.nodes[a].band == out.nodes[b].band;
      // IG.1-2: inside one opened instrument group its series — In, a member's main output (output 0), into a member's
      // main input or Out — is its path, drawn as a chain; from In (the notes) as a note wire. (Another group's series
      // keeps TX.groups' drawing.)
      const uint32_t cap = out.nodes[a].capsule;
      auto chainInput = [&] {  // into the target meta module's chain input by name (a member taking the notes)
        const doc::Device* d = doc::findDevice(p, w.to).device;
        const meta::Type* t = d != nullptr && doc::isMetaDevice(*d) ? meta::findType(doc::metaDeviceType(*d)) : nullptr;
        const int32_t ci = t != nullptr ? doc::metaChainInput(*d, *t) : -1;
        return ci >= 0 && w.param == t->inputs[static_cast<size_t>(ci)].name;
      };
      const bool series = cap != kNone && cap == out.nodes[b].capsule && out.nodes[cap].instrument && fromPort == 0 &&
                          (out.nodes[b].kind == NodeKind::Terminal || toPort == out.nodes[b].mainIn || chainInput());
      const bool mainFrom = w.port.empty() || series;
      const bool mainTo = w.param.empty() || series;
      EdgeKind k = EdgeKind::Modulation;
      if (w.fromNoteSource() && fromMidi && mainFrom && mainTo) k = EdgeKind::Notes;
      else if (series && out.nodes[a].kind == NodeKind::Terminal) k = EdgeKind::Notes;
      else if (sameBand && e.graphWire == kNone && ((mainFrom && mainTo) || w.toMixer() || (w.fromNoteSource() && mainFrom))) k = EdgeKind::Chain;
      addEdge(k, a, fromPort, b, toPort, e);
    }
  }

  // ---- layout ---------------------------------------------------------------------------------------------------

  // TX.all-free: the blocks a band places on its spine — its main path's, in signal order (blocks nobody placed sit
  // where the series was; a nudge puts a block anywhere).
  std::vector<uint32_t> spineBlocks(const Band& b) const {
    std::vector<uint32_t> order;
    const auto it = derived->tracks.find(b.track.value);
    if (it == derived->tracks.end() || !it->second.made) return order;
    for (const doc::Device* d : it->second.path.blocks)
      for (const uint32_t n : b.devices)
        if (out.nodes[n].device == d->id) order.push_back(n);
    return order;
  }

  float placeChain(uint32_t bi, float x) {
    Band& b = out.bands[bi];
    std::vector<uint32_t> order;
    if (b.source != kNone && out.nodes[b.source].kind != NodeKind::Lane) order.push_back(b.source);  // TL.1: a Σ (a lane is no block)
    for (const uint32_t d : spineBlocks(b)) order.push_back(d);
    const float rowX = x;
    for (const uint32_t n : order) {
      out.nodes[n].x = x;
      x += out.nodes[n].w + m::kNodeGap;
    }
    // GU.9: a rack's other chains under its chain on the path, at its x (each under the one before it).
    for (const uint32_t d : b.devices)
      if (const auto it = stackUnder.find(d); it != stackUnder.end()) {
        uint32_t a = it->second;
        for (uint32_t guard = 0; stackUnder.count(a) != 0 && guard < 4096; ++guard) a = stackUnder.at(a);
        out.nodes[d].x = out.nodes[a].x;
      }
    // The blocks off the main path on a row of their own under it, from its first block's place, in list order
    // (layout(): y; a nudge moves them anywhere).
    float fx = order.empty() ? rowX : out.nodes[order.front()].x + out.nodes[order.front()].w + m::kNodeGap;
    if (order.empty() || (b.source != kNone && out.nodes[b.source].kind == NodeKind::Lane)) fx = rowX;  // TL.1: from the band's start
    for (const uint32_t d : b.devices)
      if (out.nodes[d].offPath) {
        out.nodes[d].x = fx;
        fx += out.nodes[d].w + m::kNodeGap;
      }
    // TX.mixer-look: the mixer is the track's output — right of everything else the band holds (its main path and the
    // row under it), at the top (layout(): y); the master's Out after it.
    x = std::max(x, fx);
    out.nodes[b.mixer].x = x;
    x += out.nodes[b.mixer].w + m::kNodeGap;
    if (b.output != kNone) {
      out.nodes[b.output].x = x;
      x += out.nodes[b.output].w + m::kNodeGap;
    }
    return x - m::kNodeGap;
  }

  // TX.mixer-look: after the nudges, a band's mixer stays its rightmost block — where the user placed it (PatchNudge::
  // mixerPlaced: from the band's top-left corner, so it travels with the band and nothing else moves it), else pushed
  // right of any block placed past it (and TL.7: alignMixers() then takes it to the lanes' right end). The master's Out
  // follows its mixer (level with the mixer's output).
  void placeMixer(Band& b) {
    if (b.mixer == kNone) return;
    Node& mx = out.nodes[b.mixer];
    const float x0 = mx.x, y0 = mx.y;
    const doc::PatchNudge* placed = nullptr;
    if (opt.nudges != nullptr)
      if (const auto it = opt.nudges->find(b.track.value); it != opt.nudges->end() && it->second.mixerPlaced) placed = &it->second;
    b.mixerX0 = x0, b.mixerFollows = placed == nullptr;  // TX.drag-grow
    if (placed != nullptr) {
      mx.x = b.x0 + placed->mixerX;
      mx.y = b.y + std::max(m::kTitle, placed->mixerY);  // never over the band's title strip (a wall, as for blocks)
    } else {
      float right = -1e30f;
      auto reach = [&](uint32_t n) {
        if (n != kNone && out.nodes[n].capsule == kNone) right = std::max(right, out.nodes[n].x + out.nodes[n].w);
      };
      if (b.source != kNone && out.nodes[b.source].kind != NodeKind::Lane) reach(b.source);
      for (const uint32_t d : b.devices) reach(d);
      mx.x = std::max(mx.x, right + m::kNodeGap);
    }
    if (b.output != kNone) {
      Node& o = out.nodes[b.output];
      o.x += mx.x - x0;
      o.y += mx.y - y0;
    }
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

  // TL.1: whether bands have lanes (not in a scoped model: a group's panel, the free rack).
  bool lanes() const noexcept { return !opt.scope.valid() && !opt.freeTrack.valid(); }

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
    // y: lane order; every node's main pin on the band's spine (TX.resize: low enough for the lowest main pin of a
    // scaled block, whose top then meets the title strip).
    float y = 0.0f;
    for (Band& b : out.bands) {
      b.y = y;
      float pinTop = m::kPinTop;
      for (const uint32_t d : b.devices)
        if (!out.nodes[d].offPath) pinTop = std::max(pinTop, spineOf(out.nodes[d]));
      if (b.mixer != kNone) pinTop = std::max(pinTop, spineOf(out.nodes[b.mixer]));  // TL.7: the mixer on the spine too
      const float spine = y + m::kTitle + pinTop;
      float bottom = y + m::kTitle + m::kSumH;
      auto place = [&](uint32_t n) {
        Node& nd = out.nodes[n];
        nd.y = spine - spineOf(nd);
        bottom = std::max(bottom, nd.y + nd.h);
      };
      if (b.source != kNone && out.nodes[b.source].kind != NodeKind::Lane) place(b.source);  // TL.1: a Σ (the lane: below)
      for (const uint32_t d : b.devices)
        if (!out.nodes[d].offPath && stackUnder.count(d) == 0) place(d);
      // GU.9: a rack's other chains, each under the one before it (the band grows to hold them).
      for (const uint32_t d : stackOrder) {
        if (out.nodes[d].band != out.nodes[stackUnder.at(d)].band || &out.bands[out.nodes[d].band] != &b) continue;
        const Node& above = out.nodes[stackUnder.at(d)];
        out.nodes[d].y = roundUp4(above.y + above.h + m::kFreeRowGap);
        bottom = std::max(bottom, out.nodes[d].y + out.nodes[d].h);
      }
      // TL.7: the mixer level with the band's main path (its input on the spine: the main cable runs straight into
      // it), the master's Out level with its output.
      if (b.mixer != kNone) {
        Node& mx = out.nodes[b.mixer];
        mx.y = spine - spineOf(mx);
        bottom = std::max(bottom, mx.y + mx.h);
        if (b.output != kNone) {
          Node& o = out.nodes[b.output];
          o.y = mx.y + spineOf(mx) - spineOf(o);
          bottom = std::max(bottom, o.y + o.h);
        }
      }
      // TX.all-free: the row of the blocks off the main path, under it (the band grows to hold it).
      const float row = roundUp4(bottom + m::kFreeRowGap);
      for (const uint32_t d : b.devices)
        if (out.nodes[d].offPath) {
          out.nodes[d].y = row;
          bottom = std::max(bottom, row + out.nodes[d].h);
        }
      // TL.1: the lane under the circuit (a folded band: its title strip and its lane only).
      b.h = (b.folded ? m::kTitle : roundUp4(bottom + m::kBandPad - y)) + laneBlock(b);
      y += b.h + m::kBandGap;
    }
    // TX.groups: an opened group's contents where its capsule is.
    for (const Capsule& c : capsules)
      for (const auto& [k, r] : c.rel) {
        out.nodes[k].x = out.nodes[c.node].x + r.x;
        out.nodes[k].y = out.nodes[c.node].y + r.y;
      }
    // A capsule's contents move with it (a band nudge, the group's own nudge).
    // GU.2: and a nested capsule's contents with it (any depth).
    auto moveKids = [&](uint32_t n, float dx, float dy) {
      if (n == kNone || !out.nodes[n].group || !out.nodes[n].groupOpen) return;  // (most nodes: no work, no allocation)
      std::vector<uint32_t> todo{n};
      while (!todo.empty()) {
        const uint32_t at = todo.back();
        todo.pop_back();
        if (at == kNone || !out.nodes[at].group || !out.nodes[at].groupOpen) continue;
        for (const Capsule& c : capsules)
          if (c.node == at)
            for (const auto& kr : c.rel) {
              out.nodes[kr.first].x += dx;
              out.nodes[kr.first].y += dy;
              todo.push_back(kr.first);
            }
      }
    };
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
          moveKids(n, dx, dy);  // TX.groups
        };
        move(b.source);
        for (const uint32_t d : b.devices) move(d);
        move(b.mixer);
        move(b.output);
      }
      for (uint32_t i = 0; i < out.nodes.size(); ++i) {
        Node& n = out.nodes[i];
        if (n.kind == NodeKind::Device)
          if (const auto it = opt.nudges->find(n.device.value); it != opt.nudges->end()) {
            n.x += it->second.dx;
            n.y += it->second.dy;
            moveKids(i, it->second.dx, it->second.dy);  // TX.groups: its contents with it (theirs added after)
          }
      }
    }
    // TX.groups: a capsule grows to hold its members wherever their nudges put them; its terminals stay at its edges and
    // its chain pins at the height they had (pinTop follows a top edge that moved up). GU.2: inner capsules first (they
    // come after their parent's in `capsules`), so a parent holds its nested capsules as they have grown.
    for (auto it = capsules.rbegin(); it != capsules.rend(); ++it) {
      const Capsule& c = *it;
      Node& cn = out.nodes[c.node];
      float x0 = cn.x, y0 = cn.y, x1 = cn.x + cn.w, y1 = cn.y + cn.h;
      cn.frameX0 = x0, cn.frameY0 = y0, cn.frameX1 = x1, cn.frameY1 = y1;  // TX.drag-grow
      for (const uint32_t k : cn.members) {
        const Node& mn = out.nodes[k];
        x0 = std::min(x0, mn.x - m::kCapsulePad), x1 = std::max(x1, mn.x + mn.w + m::kCapsulePad);
        y0 = std::min(y0, mn.y - m::kCapsulePad - m::kCapsuleTitle), y1 = std::max(y1, mn.y + mn.h + m::kCapsulePad);
      }
      cn.pinTop += cn.y - y0;
      cn.x = x0, cn.y = y0, cn.w = x1 - x0, cn.h = y1 - y0;
      if (cn.groupIn != kNone) out.nodes[cn.groupIn].x = cn.x + m::kCapsulePad;
      if (cn.groupOut != kNone) out.nodes[cn.groupOut].x = cn.x + cn.w - m::kCapsulePad - out.nodes[cn.groupOut].w;
    }
    // TX.node-menu (SPEC §15 2026-11-02: "the band grows to fit blocks placed at its edge"): a band grows down to hold
    // its blocks wherever their nudges put them (a block placed at or past its bottom edge, an opened capsule grown by
    // its members); the bands below it move down by as much, with their blocks.
    for (Band& b : out.bands) placeMixer(b);  // TX.mixer-look
    for (uint32_t i = 0; i < out.bands.size(); ++i) {
      Band& b = out.bands[i];
      b.baseH = b.h;  // TX.drag-grow (only its own growth below changes h)
      if (b.folded) continue;  // TL.1: its blocks are hidden
      const float oldBottom = b.y + b.h;
      const float circuitBottom = oldBottom - laneBlock(b);  // TL.1: the lane stays under the circuit
      float bottom = circuitBottom - m::kBandPad;
      for (const uint32_t d : b.devices) bottom = std::max(bottom, out.nodes[d].y + out.nodes[d].h);
      for (const uint32_t n : {b.mixer, b.output})  // TX.mixer-look: a mixer placed low grows its band too
        if (n != kNone) bottom = std::max(bottom, out.nodes[n].y + out.nodes[n].h);
      const float grow = roundUp4(bottom + m::kBandPad - circuitBottom);
      if (grow <= 0.0f) continue;
      b.h += grow;
      for (uint32_t j = 0; j < out.bands.size(); ++j)
        if (j != i && out.bands[j].y >= oldBottom) out.bands[j].y += grow;
      for (Node& n : out.nodes)
        if (n.band != i && n.band < out.bands.size() && out.bands[n.band].y >= oldBottom + grow) n.y += grow;
    }
    // Bands reach their nodes (a nudged block stays inside its band's strip horizontally).
    for (Band& b : out.bands) {
      b.baseX1 = b.x1;  // TX.drag-grow
      if (b.folded) continue;  // TL.1: its blocks are hidden
      auto reach = [&](uint32_t n) {
        if (n == kNone || out.nodes[n].kind == NodeKind::Lane) return;
        b.x1 = std::max(b.x1, out.nodes[n].x + out.nodes[n].w + 12.0f);
      };
      reach(b.source);
      for (const uint32_t d : b.devices) reach(d);
      reach(b.mixer);
      reach(b.output);
    }
    placeLanes();
    alignMixers();  // TL.7
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
    for (const Node& n : out.nodes)
      if (!n.folded) grow(n.x, n.y - 12.0f, n.x + n.w, n.y + n.h);
  }

  // TL.1: the lanes — every band's over one x range (from right of the deepest band's indent to the right end of the
  // widest band; every band reaches it), at the bottom of its band; a lane node's rect is its lane.
  void placeLanes() {
    if (!lanes() || out.bands.empty()) return;
    float left = m::kLeft, right = 0.0f;  // right of every band's indent (and nudge): every lane inside its band
    for (const Band& b : out.bands) {
      left = std::max(left, b.x0);
      right = std::max(right, b.x1);
    }
    const float x0 = left + m::kLaneInset;
    const float x1 = std::max(right - m::kLaneInset, x0 + 160.0f);
    out.laneX0 = x0;
    out.laneX1 = x1;
    for (Band& b : out.bands) {
      b.x1 = std::max(b.x1, x1 + m::kLaneInset);
      b.baseX1 = std::max(b.baseX1, x1 + m::kLaneInset);
      b.laneY = b.y + b.h - m::kLaneGapBelow - b.subH - b.laneH;  // TL.2: its sub-lanes under it
      if (b.source != kNone && out.nodes[b.source].kind == NodeKind::Lane) {
        Node& n = out.nodes[b.source];
        n.x = x0, n.y = b.laneY, n.w = x1 - x0, n.h = b.laneH;
      }
    }
  }

  // TL.7 (user 2026-11-09: "the mixer module to by default always be at the rightmost part of the audio effect lane"):
  // a mixer the user has not placed ends at the right end of the lanes' common width (with the master's Out after it),
  // so every band's main path runs from its lane's left end across to its mixer. Never left of where placeMixer put it
  // (a block placed past it widened the lanes: it is at their end already). Its drag-grow rest x (Band::mixerX0) is
  // this place, so a block dragged past it pushes it as the layout would.
  void alignMixers() {
    if (!lanes()) return;
    for (Band& b : out.bands) {
      if (b.mixer == kNone || !b.mixerFollows) continue;
      Node& mx = out.nodes[b.mixer];
      const float right = b.output != kNone ? out.nodes[b.output].x + out.nodes[b.output].w : mx.x + mx.w;
      // The band's right edge stays the lanes' (placeLanes: laneX1 + kLaneInset; reach: a block's right + 12).
      const float dx = std::max(0.0f, out.laneX1 + m::kLaneInset - 12.0f - right);
      b.mixerX0 = mx.x + dx;
      if (dx == 0.0f) continue;
      mx.x += dx;
      if (b.output != kNone) out.nodes[b.output].x += dx;
    }
  }

  // TL.1: where each cable from a lane leaves its top edge (world x): its wire's tap (band-local), else (TL.7, user
  // 2026-11-09: "the cable that comes out of the audio lane, to connect to the leftmost part, which then stretches
  // across") at the lane's LEFT end — an audio lane's kTapLeft in, a MIDI lane's just right of its port chips (a press
  // on a chip still starts a new cable of that output, not a slide); several unpinned cables side by side kTapSpread
  // apart in output order (notes, then Hz, gate, vel: the per-note outputs' cables never share a spot); within the lane.
  void taps() {
    constexpr float kTapSpread = 12.0f, kTapLeft = 12.0f;
    std::vector<uint32_t> order;  // the unpinned cables from lanes, by (lane, output, edge)
    for (uint32_t i = 0; i < out.edges.size(); ++i) {
      Edge& e = out.edges[i];
      const Node& a = out.nodes[e.from];
      if (a.kind != NodeKind::Lane || a.band >= out.bands.size()) continue;
      if (!e.tapPinned) {
        order.push_back(i);
        continue;
      }
      const float x = out.bands[a.band].x0 + e.tapX;
      e.tapX = std::clamp(x, out.laneX0 + 4.0f, std::max(out.laneX0 + 4.0f, out.laneX1 - 4.0f));
    }
    std::stable_sort(order.begin(), order.end(), [&](uint32_t x, uint32_t y) {
      const Edge &ex = out.edges[x], &ey = out.edges[y];
      return ex.from != ey.from ? ex.from < ey.from : ex.fromPort < ey.fromPort;
    });
    uint32_t lane = kNone, k = 0;
    float x0 = 0.0f;
    for (const uint32_t i : order) {
      Edge& e = out.edges[i];
      if (e.from != lane) {
        lane = e.from, k = 0;
        x0 = out.laneX0 + kTapLeft;
        const Band& b = out.bands[out.nodes[lane].band];
        for (size_t port = 0; port < std::size(kLanePortLabels); ++port)
          if (const LaneRect c = lanePortChip(out, b, port); c.x1 > c.x0) x0 = std::max(x0, c.x1 + kTapLeft);
      }
      const float x = x0 + kTapSpread * static_cast<float>(k++);
      e.tapX = std::clamp(x, out.laneX0 + 4.0f, std::max(out.laneX0 + 4.0f, out.laneX1 - 4.0f));
    }
  }

  // TL.1: a folded band's blocks (and what is inside them) are hidden; so are the wires that touch one.
  void fold() {
    if (!lanes()) return;
    for (Node& n : out.nodes)
      if (n.kind != NodeKind::Lane && n.band < out.bands.size() && out.bands[n.band].folded) n.folded = true;
    for (Edge& e : out.edges)
      if (out.nodes[e.from].folded || out.nodes[e.to].folded) e.points.clear();
  }

  // ---- routes ---------------------------------------------------------------------------------------------------

  graph::Point2 outPin(const Node& n, uint32_t i) const { return outPortPoint(n, i); }  // TX.orbit-ports
  graph::Point2 inPin(const Node& n, uint32_t i) const { return inPortPoint(n, i); }

  // ---- TX.orbit-ports: orbiting main ports (pq/views/circuit/orbit.h) ----------------------------------------------

  // A closed device block's outline: its face's shape (circle or rounded rect, at its scale), a panel's rect, or the
  // plain block's body (inside its 8-unit pin margins).
  std::vector<std::pair<const meta::Type*, FaceShape>> shapes;  // faceShape per type (a registry lookup each, else)
  circuit::Outline outlineOf(const Node& n) {
    circuit::Outline o;
    const float k = n.scale;
    if (n.face != nullptr) {
      FaceShape fs;
      bool known = false;
      for (const auto& [t, sh] : shapes)
        if (t == n.face) fs = sh, known = true;
      if (!known) shapes.emplace_back(n.face, fs = faceShape(*n.face));
      if (fs.round) {
        o.round = true;
        o.cx = n.x + fs.centre.x * k, o.cy = n.y + fs.centre.y * k, o.r = fs.radius * k;
        return o;
      }
      const ui::Vec2 sz = faceSize(*n.face);
      o.x0 = n.x, o.y0 = n.y, o.x1 = n.x + sz.x * k, o.y1 = n.y + sz.y * k, o.corner = fs.radius * k;
      return o;
    }
    if (n.groupPanel && n.panelRound) {  // GU.5b: a round panel (the VCA's dial, the Phase Accumulator's body)
      o.round = true;
      o.cx = n.x + n.roundX * k, o.cy = n.y + n.roundY * k, o.r = n.roundR * k;
      return o;
    }
    if (n.panel || n.groupPanel) {  // GU.3: a group drawn as its panel too
      o.x0 = n.x, o.y0 = n.y, o.x1 = n.x + n.w, o.y1 = n.y + n.h;
      o.corner = panel::GroupPanelFace::kCorner * k;
      return o;
    }
    const float in = std::min(8.0f, n.w * 0.25f);
    o.x0 = n.x + in, o.y0 = n.y, o.x1 = n.x + n.w - in, o.y1 = n.y + n.h, o.corner = 2.0f;
    return o;
  }

  // Which ports orbit, and the pinned ones (view state); then each port's angle toward its cables' other ends (an end
  // that orbits too: its block's centre; an anchored control: its centre; any other pin: the pin), with the spacing rule.
  void orbit() {
    std::vector<uint32_t> blocks;
    for (uint32_t i = 0; i < out.nodes.size(); ++i) {
      Node& n = out.nodes[i];
      // TX.ports-always: every closed block, wherever it sits (top level or inside an opened group), a value block's knob
      // too — its face or panel draws no jacks, so the canvas draws its main ports. Only an atom block's circuit symbol
      // keeps its pins where the symbol's leads are (the canvas draws them as stubs: visible and hit there).
      if (n.kind != NodeKind::Device || n.expanded || (n.group && n.groupOpen) || n.symbol) continue;
      n.outline = outlineOf(n);
      if (!n.outline.valid()) continue;
      n.orbitIn.on = n.mainIn >= 0 && static_cast<size_t>(n.mainIn) < n.inputs.size() && !n.inputs[static_cast<size_t>(n.mainIn)].anchored;
      n.orbitOut.on = !n.outputs.empty();
      if (opt.nudges != nullptr)
        if (const auto it = opt.nudges->find(n.device.value); it != opt.nudges->end()) {
          n.orbitIn.pinned = it->second.inPinned;
          n.orbitIn.angle = it->second.inAngle;
          n.orbitOut.pinned = it->second.outPinned;
          n.orbitOut.angle = it->second.outAngle;
        }
      if (n.orbitIn.on || n.orbitOut.on) blocks.push_back(i);
    }
    if (blocks.empty()) return;
    auto anchorOf = [&](uint32_t idx, uint32_t port, bool input, const Edge& e) -> graph::Point2 {
      const Node& m = out.nodes[idx];
      if (m.kind == NodeKind::Lane) return {e.tapX, m.y};  // TL.1: the cable's tap on the lane's top edge
      if (portOrbits(m, port, input)) return m.outline.centre();
      if (input && port < m.inputs.size() && m.inputs[port].anchored) return {m.x + m.inputs[port].ax, m.y + m.inputs[port].ay};
      return input ? inputEntry(m, port) : graph::Point2{m.x + m.w, m.y + pinOffset(m, port, false)};
    };
    // (port key = node · 2 + 0 input / 1 output, the other end), sorted by key.
    std::vector<std::pair<uint32_t, graph::Point2>> ends;
    for (const Edge& e : out.edges) {
      if (!isCable(e.kind, out.nodes[e.from].kind)) continue;
      if (portOrbits(out.nodes[e.from], e.fromPort, false)) ends.emplace_back(e.from * 2 + 1, anchorOf(e.to, e.toPort, true, e));
      if (portOrbits(out.nodes[e.to], e.toPort, true)) ends.emplace_back(e.to * 2, anchorOf(e.from, e.fromPort, false, e));
    }
    std::stable_sort(ends.begin(), ends.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<graph::Point2> pts(ends.size());
    for (size_t k = 0; k < ends.size(); ++k) pts[k] = ends[k].second;
    std::vector<graph::Point2> fixed;
    for (const uint32_t i : blocks) {
      Node& n = out.nodes[i];
      std::span<const graph::Point2> targets[2];
      for (uint32_t k = 0; k < 2; ++k) {
        const uint32_t key = i * 2 + k;
        const auto lo = std::lower_bound(ends.begin(), ends.end(), key, [](const auto& a, uint32_t v) { return a.first < v; });
        const auto hi = std::upper_bound(lo, ends.end(), key, [](uint32_t v, const auto& a) { return v < a.first; });
        targets[k] = std::span<const graph::Point2>(pts.data() + (lo - ends.begin()), static_cast<size_t>(hi - lo));
      }
      fixed.clear();
      for (size_t k = 0; k < n.inputs.size(); ++k)
        if (!portOrbits(n, k, true) && !n.inputs[k].anchored) fixed.push_back(inputEntry(n, k));
      for (size_t k = 1; k < n.outputs.size(); ++k) fixed.push_back({n.x + n.w, n.y + pinOffset(n, k, false)});
      const bool on[2] = {n.orbitIn.on, n.orbitOut.on};
      const bool pinned[2] = {n.orbitIn.pinned, n.orbitOut.pinned};
      float angles[2] = {n.orbitIn.angle, n.orbitOut.angle};
      circuit::solveOrbits(n.outline, n.scale, on, pinned, targets, fixed, angles);
      n.orbitIn.angle = angles[0];
      n.orbitOut.angle = angles[1];
      if (n.orbitIn.on) n.orbitIn.at = circuit::orbitPoint(n.outline, angles[0], n.scale);
      if (n.orbitOut.on) n.orbitOut.at = circuit::orbitPoint(n.outline, angles[1], n.scale);
    }
  }

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
      const graph::Point2 s = a.kind == NodeKind::Lane ? graph::Point2{e.tapX, a.y} : outPin(a, e.fromPort), t = inPin(b, e.toPort);
      std::vector<graph::Point2>& pts = e.points;
      pts.clear();
      pts.push_back(s);
      // TX.cables: a connection between blocks hangs straight from port to port (an anchored param's: onto its
      // control); the scene gives it its rest shape and the canvas its motion.
      // TX.cable-look (the user): inside an opened group too — only the bus structure (routes, sends, a mixer's output)
      // stays an orthogonal trace.
      e.cable = isCable(e.kind, a.kind);
      if (e.cable) {
        const Port* tp = e.toPort < b.inputs.size() ? &b.inputs[e.toPort] : nullptr;
        pts.push_back(tp != nullptr && tp->anchored ? graph::Point2{b.x + tp->ax, b.y + tp->ay} : t);
        e.net = 1u << 30 | static_cast<uint32_t>(k);
        continue;
      }
      // A bus (route / send) into a summing input: every wire into one input shares its vertical channel.
      // TMM.13-fix2 (MM review M10): parallel runs 6 apart (3 channels in a 20-unit title strip), never closer.
      const uint32_t off = (serial++ % 3) * 6;
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
      pts.push_back(t);
      simplify(pts);
    }
  }

  void run() {
    ModelCache* mc = opt.layoutCache;
    if (mc != nullptr) ++mc->epoch;
    prepare();
    for (const doc::TrackPtr& t : p.tracks) buildBand(*t, 0, -1, true);
    for (const doc::TrackPtr& r : p.returns) buildBand(*r, 0, -1, true);
    if (p.master) buildBand(*p.master, 0, -1, true);
    if (ModelCache* c = opt.layoutCache)  // PF.2: groups gone or closed lose their layouts
      std::erase_if(c->entries, [&](const auto& e) { return e.second.used != c->epoch; });
    for (uint32_t i = 0; i < out.bands.size(); ++i) outputEdge(i);
    graphEdges();
    layout();
    taps();   // TL.1: where the lanes' cables leave them
    orbit();  // TX.orbit-ports: the main ports' places (the routes start there)
    route();
    fold();   // TL.1
    if (opt.scope.valid()) prune();
    else if (opt.freeTrack.valid()) pruneFree();
  }

  // An opened capsule's contents — its terminals, its members and (GU.2) a nested capsule's contents, any depth.
  void contentsOf(uint32_t cap, std::vector<uint32_t>& order) const {
    const Node& c = out.nodes[cap];
    if (!c.group || !c.groupOpen) return;
    if (c.groupIn != kNone) order.push_back(c.groupIn);
    for (const uint32_t k : c.members) {
      order.push_back(k);
      contentsOf(k, order);
    }
    if (c.groupOut != kNone) order.push_back(c.groupOut);
  }

  // TX.free-rack (LayoutOptions::freeTrack): only the track's free blocks (not group members; a free group's capsule
  // with its terminals and members) and the wires among them; one band without source, mixer or strip, around them.
  void pruneFree() {
    ProjectPatch keep;
    keep.scoped = true;
    const uint32_t b = out.bandOf(opt.freeTrack);
    if (b == kNone) {
      out = std::move(keep);
      return;
    }
    std::vector<uint32_t> order;
    for (uint32_t i = 0; i < out.nodes.size(); ++i) {
      const Node& n = out.nodes[i];
      if (n.band != b || n.kind != NodeKind::Device || !n.offPath || n.rack || n.capsule != kNone) continue;
      order.push_back(i);
      contentsOf(i, order);
    }
    std::sort(order.begin(), order.end());  // model order (a capsule before its contents)
    order.erase(std::unique(order.begin(), order.end()), order.end());
    Band band = out.bands[b];
    band.source = band.mixer = band.output = kNone;
    band.laneY = band.laneH = band.subH = 0.0f;  // TL.1: no lane in a scoped model
    band.devices.clear();
    band.parent = -1;
    band.lastMember = kNone;
    band.depth = 0;
    std::vector<uint32_t> map(out.nodes.size(), kNone);
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    for (const uint32_t i : order) {
      map[i] = static_cast<uint32_t>(keep.nodes.size());
      const Node& n = out.nodes[i];
      if (keep.nodes.empty()) x0 = n.x, y0 = n.y, x1 = n.x + n.w, y1 = n.y + n.h;
      x0 = std::min(x0, n.x), y0 = std::min(y0, n.y), x1 = std::max(x1, n.x + n.w), y1 = std::max(y1, n.y + n.h);
      keep.nodes.push_back(n);
      keep.nodes.back().band = 0;
    }
    for (Node& n : keep.nodes) {
      if (n.capsule != kNone) n.capsule = map[n.capsule];
      if (n.groupIn != kNone) n.groupIn = map[n.groupIn];
      if (n.groupOut != kNone) n.groupOut = map[n.groupOut];
      for (uint32_t& k : n.members) k = map[k];
    }
    for (const uint32_t i : order)
      if (out.nodes[i].capsule == kNone) band.devices.push_back(map[i]);
    // The band holds its blocks with a margin (a press on empty space between them is still the rack's band).
    constexpr float kMargin = 24.0f;
    band.x0 = x0 - kMargin, band.x1 = x1 + kMargin, band.y = y0 - kMargin, band.h = (y1 - y0) + 2.0f * kMargin;
    keep.bands.push_back(std::move(band));
    for (const Edge& e : out.edges) {
      if (map[e.from] == kNone || map[e.to] == kNone) continue;
      Edge x = e;
      x.from = map[e.from];
      x.to = map[e.to];
      x.graphWire = kNone;
      keep.edges.push_back(std::move(x));
    }
    keep.x0 = x0 - 8.0f, keep.y0 = y0 - 12.0f, keep.x1 = x1 + 8.0f, keep.y1 = y1 + 8.0f;
    out = std::move(keep);
  }

  // TX.groups (LayoutOptions::scope): only the scope group's capsule, its contents and the wires among them; one band
  // (the group's track) with no source, mixer or strip of its own.
  void prune() {
    uint32_t cap = kNone;
    for (uint32_t i = 0; i < out.nodes.size(); ++i)
      if (out.nodes[i].group && out.nodes[i].groupOpen && out.nodes[i].device == opt.scope) cap = i;
    ProjectPatch keep;
    keep.scoped = true;
    if (cap == kNone) {
      out = std::move(keep);
      return;
    }
    std::vector<uint32_t> map(out.nodes.size(), kNone);
    std::vector<uint32_t> order{cap};
    contentsOf(cap, order);
    std::sort(order.begin(), order.end());  // model order (the capsule first: its contents are hit before it)
    const Node& c = out.nodes[cap];
    Band band = out.bands[c.band];
    band.source = band.mixer = band.output = kNone;
    band.laneY = band.laneH = band.subH = 0.0f;  // TL.1: no lane in a scoped model
    band.devices.clear();
    band.parent = -1;
    band.lastMember = kNone;
    band.depth = 0;
    for (const uint32_t i : order) {
      map[i] = static_cast<uint32_t>(keep.nodes.size());
      keep.nodes.push_back(out.nodes[i]);
      keep.nodes.back().band = 0;
    }
    for (Node& n : keep.nodes) {
      if (n.capsule != kNone) n.capsule = map[n.capsule];
      if (n.groupIn != kNone) n.groupIn = map[n.groupIn];
      if (n.groupOut != kNone) n.groupOut = map[n.groupOut];
      for (uint32_t& k : n.members) k = map[k];
    }
    band.devices.push_back(map[cap]);
    band.x0 = c.x, band.x1 = c.x + c.w, band.y = c.y, band.h = c.h;
    keep.bands.push_back(std::move(band));
    for (const Edge& e : out.edges) {
      if (map[e.from] == kNone || map[e.to] == kNone) continue;
      Edge x = e;
      x.from = map[e.from];
      x.to = map[e.to];
      x.graphWire = kNone;
      keep.edges.push_back(std::move(x));
    }
    keep.x0 = c.x, keep.y0 = c.y - 12.0f, keep.x1 = c.x + c.w, keep.y1 = c.y + c.h;
    out = std::move(keep);
  }
};

}  // namespace

ProjectPatch buildProjectPatch(const doc::Project& project, const doc::LoweredProject& lowered,
                               const doc::DeviceLibrary* devices, const LayoutOptions& options) {
  Builder b{project, lowered, devices, options};
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
    // TX.groups: an opened group's members are blocks of the band too (inside its capsule).
    // GU.2: and a nested capsule's members (any depth).
    std::vector<uint32_t> blocks = b.devices;
    for (size_t i = 0; i < blocks.size(); ++i)
      if (const Node& d = pp.nodes[blocks[i]]; d.group && d.groupOpen) blocks.insert(blocks.end(), d.members.begin(), d.members.end());
    uint32_t calls = 0;
    for (graph::NodeIndex n = 0; n < tp.nodeCount(); ++n) {
      const std::string& name = tp.nodeName(n);
      if (tp.node(n).kind != graph::NodeKind::Subcircuit || name.size() < 2 || name[0] != 'd' ||
          name.find_first_not_of("0123456789", 1) != std::string::npos)
        continue;
      ++calls;
      const doc::DeviceId id{static_cast<uint32_t>(std::stoul(name.substr(1)))};
      bool found = false;
      for (const uint32_t d : blocks) {
        const Node& nd = pp.nodes[d];
        // T4.8: inside a rack; TX.groups: inside a closed group
        if (!nd.inner.empty() && std::find(nd.inner.begin(), nd.inner.end(), id) != nd.inner.end()) {
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
    for (const uint32_t d : blocks) lowered += pp.nodes[d].lowered ? 1u : 0u;
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
    } else if (gw.kind == graph::WireKind::Modulation || gw.kind == graph::WireKind::Sidechain) {
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
      if (!b.inner.empty()) {  // T4.8: "<param>@<device>" is that device's call inside the rack (no "@": the rack's own macro)
        const size_t at = pin.rfind('@');
        if (at == std::string::npos) continue;  // a macro: the rack's own param, read by its mappings
        callId = graph::NodeId{doc::nodeIdFor(doc::NodeRole::Device, static_cast<uint32_t>(std::stoul(pin.substr(at + 1))), 0)};
        pin.resize(at);
      }
      const auto call = tp.indexOf(callId);
      if (!call) return "a modulation wire into a block without a call";
      auto in = tp.findInput(*call, pin);
      if (!in) return "a modulation wire into a param the call does not have";
      // TX.patching: a param whose patch adds its wires after its own law takes them at `<param>_add`.
      if (tp.adjacency().driver[tp.flatInput(*call, *in)] == graph::kNone)
        if (const auto units = tp.findInput(*call, pin + "_add")) in = units;
      if (tp.adjacency().driver[tp.flatInput(*call, *in)] == graph::kNone)
        return "a modulation wire into a param pin the track patch leaves unconnected";
    }
  }
  // TX.groups: an opened group's capsule holds its terminals and members.
  for (const Node& c : pp.nodes) {
    if (!c.group || !c.groupOpen) continue;
    std::vector<uint32_t> kids = c.members;
    kids.push_back(c.groupIn);
    kids.push_back(c.groupOut);
    for (const uint32_t k : kids) {
      if (k == kNone || k >= pp.nodes.size()) return "group " + c.title + ": a terminal or member is missing";
      const Node& n = pp.nodes[k];
      if (n.capsule == kNone || &pp.nodes[n.capsule] != &c) return "group " + c.title + ": " + n.title + " is not its member";
      if (n.x < c.x || n.y < c.y || n.x + n.w > c.x + c.w + 0.01f || n.y + n.h > c.y + c.h + 0.01f)
        return "group " + c.title + ": " + n.title + " outside its capsule";
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
