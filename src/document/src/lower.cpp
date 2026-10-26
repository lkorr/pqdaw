// Project → graph lowering (T3.17; IR with stable ids T3.17-fix2): pq/document/lower.h describes the signal flow.
#include "pq/document/lower.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <numbers>
#include <unordered_map>

#include "pq/document/audibility.h"
#include "pq/document/pitch_resolve.h"
#include "pq/graph/patch_file.h"

namespace pq::doc {

namespace {

using graph::NodeIndex;
using graph::PortRef;

enum class Kind : uint8_t { Midi, Group, Return, Master };

// A MIDI track's tuning node (T2.6-fix1).
struct TuningNode {
  TuningId id;                      // invalid: no node
  const TuningDef* def = nullptr;
  uint32_t table = 0;               // index into Resources::tunings
};

struct Node {
  const Track* track = nullptr;
  Kind kind = Kind::Midi;
  const Track* parent = nullptr;    // active parent group (main tree)
  std::vector<TuningNode> tunings;  // T2.6-fix3: one node per tuning signal its clips follow (trackTuningSignals)
};

// A lowered send (see the file comment of lower.h).
struct SendLink {
  uint32_t from = 0, to = 0;  // node indices (sender, return)
  bool pre = false;           // reads the sender's pre-fader export
  bool delayed = false;       // closes a loop: an automatic 1-sample delay
  bool conditional = false;   // lowered only while stored (return → return, or a loop)
  float levelDb = kMinVolumeDb;
};

struct Plan {
  std::vector<Node> nodes;
  std::unordered_map<uint32_t, uint32_t> indexOf;  // track id → node index
  std::vector<uint32_t> dest;                      // per node: the node its output feeds, or graph::kNone
  std::vector<SendLink> sends;
  uint32_t master = graph::kNone;
};

using TableOf = std::function<uint32_t(TuningId, const TuningDef&)>;

const Send* storedSend(const Track& t, TrackId ret) {
  for (const Send& s : t.mixer.sends)
    if (s.target == ret) return &s;
  return nullptr;
}

// T4.1: whether a MIDI track lowers an instrument (its first active device, found in the library as one).
bool lowersInstrument(const Track& t, const graph::PatchLibrary* lib) noexcept;

// The lowered nodes of a project (active tracks), in forEachTrack order: main tree depth-first, returns, master;
// their routing and their sends. `tableOf` (optional): MIDI tracks following a tuning signal get a tuning node with
// that table (only those that lower an instrument with `lib`: the node feeds its voices).
Plan plan(const Project& p, const TableOf* tableOf, const graph::PatchLibrary* lib) {
  Plan pl;
  struct Walk {
    const Project& p;
    std::vector<Node>& nodes;
    const TableOf* tableOf;
    const graph::PatchLibrary* lib;
    void operator()(const std::vector<TrackPtr>& list, const Track* parent, TrackPath path) {
      for (uint32_t i = 0; i < list.size(); ++i) {
        const TrackPtr& t = list[i];
        if (!t->active) continue;  // deactivated: left out with its members (Q15)
        if (t->kind != TrackKind::Midi && t->kind != TrackKind::Group) continue;
        TrackPath here = path;
        if (here.depth < kMaxTrackDepth) here.index[here.depth++] = i;
        Node n;
        n.track = t.get();
        n.kind = t->kind == TrackKind::Midi ? Kind::Midi : Kind::Group;
        n.parent = parent;
        if (n.kind == Kind::Midi && tableOf != nullptr && *tableOf && here.depth > path.depth && lowersInstrument(*t, lib))
          for (const TuningId id : trackTuningSignals(p, here)) {
            const TuningSlot* slot = findTuning(p, id);
            n.tunings.push_back(TuningNode{id, &slot->def, (*tableOf)(id, slot->def)});
          }
        nodes.push_back(std::move(n));
        if (t->kind == TrackKind::Group) (*this)(t->children, t.get(), here);
      }
    }
  } walk{p, pl.nodes, tableOf, lib};
  walk(p.tracks, nullptr, TrackPath{});
  const auto mainCount = static_cast<uint32_t>(pl.nodes.size());
  for (const TrackPtr& r : p.returns)
    if (r->active) pl.nodes.push_back(Node{r.get(), Kind::Return, nullptr, {}});
  if (p.master) {
    pl.master = static_cast<uint32_t>(pl.nodes.size());
    pl.nodes.push_back(Node{p.master.get(), Kind::Master, nullptr, {}});
  }
  const auto n = static_cast<uint32_t>(pl.nodes.size());
  for (uint32_t i = 0; i < n; ++i) pl.indexOf[pl.nodes[i].track->id.value] = i;

  // Routing.
  auto inside = [&](const Track& group, TrackId id) {  // id is group itself or one of its members
    if (group.id == id) return true;
    struct Find {
      TrackId id;
      bool operator()(const std::vector<TrackPtr>& list) const {
        for (const TrackPtr& t : list)
          if (t->id == id || (*this)(t->children)) return true;
        return false;
      }
    };
    return Find{id}(group.children);
  };
  pl.dest.assign(n, graph::kNone);
  for (uint32_t i = 0; i < n; ++i) {
    const Node& nd = pl.nodes[i];
    if (nd.kind == Kind::Master) continue;
    const Output& op = nd.track->mixer.output;
    uint32_t d = pl.master;
    switch (op.kind) {
      case OutputKind::Default:
        if (nd.parent != nullptr) d = pl.indexOf.at(nd.parent->id.value);
        break;
      case OutputKind::Master: break;
      case OutputKind::Track:
        if (const Track* target = findTrackPtr(p, op.track); target != nullptr && target->kind == TrackKind::Group) {
          const auto it = pl.indexOf.find(op.track.value);
          if (it == pl.indexOf.end()) d = graph::kNone;  // a deactivated group plays nothing (M3 review L1)
          else if (!inside(*nd.track, op.track)) d = it->second;  // (a group cannot feed itself: the master)
        }
        break;
      case OutputKind::None: d = graph::kNone; break;
    }
    pl.dest[i] = d;
  }

  // Sends: main tracks first, then returns, each in track order; a send that closes a loop is delayed.
  std::vector<std::vector<uint32_t>> adj(n);
  for (uint32_t i = 0; i < n; ++i)
    if (pl.dest[i] != graph::kNone) adj[i].push_back(pl.dest[i]);
  std::vector<uint8_t> seen(n);
  std::vector<uint32_t> stack;
  auto reaches = [&](uint32_t from, uint32_t to) {
    std::fill(seen.begin(), seen.end(), uint8_t{0});
    stack.assign(1, from);
    seen[from] = 1;
    while (!stack.empty()) {
      const uint32_t v = stack.back();
      stack.pop_back();
      if (v == to) return true;
      for (uint32_t w : adj[v])
        if (!seen[w]) {
          seen[w] = 1;
          stack.push_back(w);
        }
    }
    return false;
  };
  auto addSends = [&](uint32_t s) {
    const Node& sn = pl.nodes[s];
    for (uint32_t r = mainCount; r < n; ++r) {
      if (pl.nodes[r].kind != Kind::Return || r == s) continue;
      const Send* stored = storedSend(*sn.track, pl.nodes[r].track->id);
      const bool feedback = reaches(r, s);
      SendLink l;
      l.from = s;
      l.to = r;
      l.conditional = sn.kind == Kind::Return || feedback;
      if (l.conditional && stored == nullptr) continue;
      l.pre = stored != nullptr && stored->preFader;
      l.delayed = feedback;
      l.levelDb = stored != nullptr ? std::clamp(stored->levelDb, kMinVolumeDb, kMaxVolumeDb) : kMinVolumeDb;
      pl.sends.push_back(l);
      adj[s].push_back(r);
    }
  };
  for (uint32_t s = 0; s < mainCount; ++s) addSends(s);
  for (uint32_t s = mainCount; s < n; ++s)
    if (pl.nodes[s].kind == Kind::Return) addSends(s);
  return pl;
}

// ---- IR construction ---------------------------------------------------------------------------------------------

// Fixed NodeIds of a role's own nodes (< kEntityNodeBase): never renumbered, so their state keys never move.
enum Fixed : uint32_t {
  kVolume = 1, kPan, kGate, kSendGate, kMute,
  kIn = 8, kOut, kPost, kPre, kDone,
  kInstrument = 16, kVoiceSum,
  kC0 = 20, kC1, kCHalf, kCFloor, kCQuarterPi, kCHalfPi, kCTime,
  kVolLe = 32, kVolDb, kVg, kUnmuted, kGateOut, kGateSends, kGOut, kGPost,
  kPanA = 40, kPanB, kTheta, kPl, kPr,
  kOutL = 48, kOutR, kOutPack, kOutGain, kPostL, kPostR, kPostPack, kPostGain, kPreL, kPreR, kPrePack, kPreGain,
  kVDegree = 64, kVEquave, kVHz, kVCents, kVTuning, kTuneVoices,
  kSendAcc = 72,
  kLiteralPool = 256,  // T4.1: device setting literals take ids kLiteralPool … kEntityNodeBase − 1 (per track)
};
// Per entity (nodeIdFor): a tuning node's nodes, a send's nodes.
enum TuningK : uint32_t { kTRef, kTPer, kTGen, kTSt, kTNode, kTTable, kTIndex };
enum SendK : uint32_t { kSIn, kSLevel, kSLe, kSDb, kSGain, kSDelay, kSAdd };

// `audio global stereo` (the ports' declared type, as the text form wrote them).
constexpr graph::PortType kStereo = graph::portType(graph::Kind::Infer, graph::Rate::Audio, graph::Poly::Global, 2);

class Ir {
public:
  explicit Ir(std::string name) : p_(std::move(name)) {}
  graph::Patch& patch() noexcept { return p_; }
  bool failed() const noexcept { return failed_; }

  PortRef atom(std::string_view name, std::initializer_list<PortRef> ins, uint32_t id, uint16_t repeat = 0) {
    const NodeIndex n = take(p_.addAtom(graph::atomId(name), repeat), id);
    uint16_t i = 0;
    for (const PortRef& in : ins) p_.connect(in, PortRef{n, i++});
    return PortRef{n, 0};
  }
  // A constant: one node per id (a fixed constant is shared by everything that reads it).
  PortRef konst(double v, uint32_t id) {
    for (const auto& [cid, port] : consts_)
      if (cid == id) return port;
    const PortRef port{take(p_.addConst(graph::Constant::real(v)), id), 0};
    consts_.emplace_back(id, port);
    return port;
  }
  // A literal of a device setting (T4.1): its own node, its id from the track's constant pool (stateless: the id
  // only has to be unique in the patch).
  PortRef literal(double v) {
    if (nextLiteral_ >= kEntityNodeBase) {
      failed_ = true;
      overflow_ = true;
      return PortRef{take(p_.addConst(graph::Constant::real(v)), 0), 0};
    }
    return PortRef{take(p_.addConst(graph::Constant::real(v)), nextLiteral_++), 0};
  }
  bool literalOverflow() const noexcept { return overflow_; }
  PortRef param(const std::string& name, double def, double lo, double hi, uint32_t id) {
    graph::ParamDecl d;
    d.defaultValue = graph::Constant::real(def);
    d.hasRange = true;
    d.rangeMin = graph::Constant::real(lo);
    d.rangeMax = graph::Constant::real(hi);
    return PortRef{take(p_.addParam(name, std::move(d)), id), 0};
  }
  PortRef input(const std::string& name, PortRef def, uint32_t id) {
    const NodeIndex n = take(p_.addInput(name, kStereo), id);
    p_.connect(def, PortRef{n, 0});
    return PortRef{n, 0};
  }
  void output(const std::string& name, graph::PortType type, PortRef value, uint32_t id) {
    const NodeIndex n = take(p_.addOutput(name, type), id);
    p_.connect(value, PortRef{n, 0});
  }
  void exportPort(const std::string& name, PortRef value, uint32_t id) {
    const NodeIndex n = take(p_.addExport(name), id);
    p_.connect(value, PortRef{n, 0});
  }
  NodeIndex call(graph::SubcircuitRef ref, uint32_t id) { return take(p_.addSubcircuit(std::move(ref)), id); }
  // Gives every node its document-derived id. False: two nodes asked for one id, or an id did not fit (a bug, or an
  // entity id beyond nodeIdFor's range).
  bool finish() {
    std::vector<graph::NodeId> ids;
    ids.reserve(ids_.size());
    for (uint32_t v : ids_) ids.push_back(graph::NodeId{v});
    return !failed_ && p_.setNodeIds(ids);
  }

private:
  NodeIndex take(NodeIndex n, uint32_t id) {
    if (id == 0) failed_ = true;
    ids_.push_back(id);
    return n;
  }
  graph::Patch p_;
  std::vector<uint32_t> ids_;
  std::vector<std::pair<uint32_t, PortRef>> consts_;
  uint32_t nextLiteral_ = kLiteralPool;
  bool failed_ = false;
  bool overflow_ = false;
};

// The gain stages (see lower.h): x → out (and for senders post / pre exports).
void gainStages(Ir& ir, Kind kind, PortRef x, float smoothingMs) {
  const PortRef volume = ir.param("volume", 0.0, kMinVolumeDb, kMaxVolumeDb, kVolume);
  const PortRef pan = ir.param("pan", 0.0, -1.0, 1.0, kPan);
  const PortRef c0 = ir.konst(0.0, kC0), c1 = ir.konst(1.0, kC1);
  const PortRef time = ir.konst(static_cast<double>(smoothingMs) * 1e-3, kCTime);
  const PortRef vg = ir.atom("select", {ir.atom("le", {volume, ir.konst(kMinVolumeDb, kCFloor)}, kVolLe), c0,
                                        ir.atom("db_to_gain", {volume}, kVolDb)},
                             kVg);
  PortRef pl, pr;
  if (kind == Kind::Midi) {  // a mono source: constant power, −3 dB per side at the centre
    const PortRef th = ir.atom("mul", {ir.atom("add", {pan, c1}, kPanA), ir.konst(std::numbers::pi / 4, kCQuarterPi)}, kTheta);
    pl = ir.atom("sin", {ir.atom("add", {th, ir.konst(std::numbers::pi / 2, kCHalfPi)}, kPanB)}, kPl);
    pr = ir.atom("sin", {th}, kPr);
  } else {  // a stereo bus: balance, 0 dB at the centre
    pl = ir.atom("min", {c1, ir.atom("sub", {c1, pan}, kPanA)}, kPl);
    pr = ir.atom("min", {c1, ir.atom("add", {c1, pan}, kPanB)}, kPr);
  }
  auto stage = [&](PortRef g, uint32_t l, uint32_t r, uint32_t pack, uint32_t gain) {
    const PortRef gl = ir.atom("mul", {pl, g}, l), gr = ir.atom("mul", {pr, g}, r);
    return ir.atom("gain", {x, ir.atom("pack", {gl, gr}, pack, 2), time}, gain);
  };
  if (kind == Kind::Master) {
    ir.output("out", kStereo, stage(vg, kOutL, kOutR, kOutPack, kOutGain), kOut);
    return;
  }
  const PortRef gate = ir.param("gate", 1.0, 0.0, 1.0, kGate);
  const PortRef sendGate = ir.param("sendGate", 1.0, 0.0, 1.0, kSendGate);
  const PortRef mute = ir.param("mute", 0.0, 0.0, 1.0, kMute);
  const PortRef unmuted = ir.atom("lt", {mute, ir.konst(0.5, kCHalf)}, kUnmuted);
  const PortRef go = ir.atom("select", {unmuted, gate, c0}, kGateOut);
  const PortRef so = ir.atom("select", {unmuted, sendGate, c0}, kGateSends);
  ir.output("out", kStereo, stage(ir.atom("mul", {vg, go}, kGOut), kOutL, kOutR, kOutPack, kOutGain), kOut);
  ir.exportPort("post", stage(ir.atom("mul", {vg, so}, kGPost), kPostL, kPostR, kPostPack, kPostGain), kPost);
  ir.exportPort("pre", stage(so, kPreL, kPreR, kPrePack, kPreGain), kPre);
}

std::string patchName(const Node& n) {
  const std::string id = std::to_string(n.track->id.value);
  switch (n.kind) {
    case Kind::Midi: return "Midi" + id;
    case Kind::Group: return "Group" + id;
    case Kind::Return: return "Return" + id;
    case Kind::Master: return "Master" + id;
  }
  return id;
}

// ---- Device chains (T4.1) ------------------------------------------------------------------------------------------

// What a device patch is, read from its declarations (devices.h describeDevice applies the same rule): an instrument
// has no non-const `in` and a per-voice first `out`.
bool calleeIsInstrument(const graph::Patch& p) noexcept {
  bool audioIn = false, voiceOut = false, haveOut = false;
  for (graph::NodeIndex v = 0; v < p.nodeCount(); ++v) {
    const graph::Node& nd = p.node(v);
    if (nd.kind == graph::NodeKind::Input && p.outputType(v, 0).rate != graph::Rate::Const) audioIn = true;
    if (nd.kind == graph::NodeKind::Output && !haveOut) {
      haveOut = true;
      voiceOut = p.inputType(v, 0).poly == graph::Poly::Voice;
    }
  }
  return !audioIn && voiceOut;
}

bool lowersInstrument(const Track& t, const graph::PatchLibrary* lib) noexcept {
  for (const DevicePtr& d : t.devices) {
    if (!d->active) continue;
    if (d->kind != DeviceKind::Instrument || lib == nullptr) return false;
    const graph::Patch* callee = lib->find(d->patch);
    return callee != nullptr && calleeIsInstrument(*callee);
  }
  return false;
}

struct LDev {
  const Device* device = nullptr;
  const graph::Patch* callee = nullptr;
};

// The devices of a track that are lowered, in chain order: active, their patch in the library and of their kind (an
// instrument only first on a MIDI track). The rest are bypassed; with `missing`, those not found / of the wrong kind
// are listed there.
std::vector<LDev> loweredDevices(const Node& n, const graph::PatchLibrary* lib, std::vector<DeviceId>* missing) {
  std::vector<LDev> out;
  for (const DevicePtr& d : n.track->devices) {
    if (!d->active) continue;
    const graph::Patch* callee = lib != nullptr ? lib->find(d->patch) : nullptr;
    const bool instrument = d->kind == DeviceKind::Instrument;
    if (callee == nullptr || calleeIsInstrument(*callee) != instrument || (instrument && (n.kind != Kind::Midi || !out.empty()))) {
      if (missing != nullptr) missing->push_back(d->id);
      continue;
    }
    out.push_back(LDev{d.get(), callee});
  }
  return out;
}

// A device's call node: named d<id>, NodeId nodeIdFor(Device, id, 0); the settings the user set become literals at
// their `real const` inputs; params stay unconnected (runtime params d<id>__<name>, linked by the app).
graph::NodeIndex callDevice(Ir& ir, const LDev& d) {
  const graph::NodeIndex c = ir.call(graph::subcircuitSignature(*d.callee), nodeIdFor(NodeRole::Device, d.device->id.value, 0));
  graph::Patch& p = ir.patch();
  p.setNodeName(c, "d" + std::to_string(d.device->id.value));
  for (const DeviceValue& v : d.device->settings) {
    const auto in = p.findInput(c, v.name);
    if (!in || p.inputType(c, *in).rate != graph::Rate::Const) continue;  // not a setting of this patch (any more)
    p.connect(ir.literal(v.value), PortRef{c, static_cast<uint16_t>(*in)});
  }
  return c;
}

// The audio effects of `devs` from `first` on, wired left → right after `x` (declared width `width`).
PortRef effectChain(Ir& ir, std::span<const LDev> devs, size_t first, PortRef x, uint16_t width) {
  for (size_t k = first; k < devs.size(); ++k) {
    const LDev& d = devs[k];
    // The first non-const `in` (its port index = its ordinal among the callee's `in`s) and the first `out`.
    uint32_t inPort = 0, ordinal = 0;
    uint16_t inWidth = 0, outWidth = 0;
    bool haveIn = false, haveOut = false;
    for (graph::NodeIndex v = 0; v < d.callee->nodeCount(); ++v) {
      const graph::Node& nd = d.callee->node(v);
      if (nd.kind == graph::NodeKind::Input) {
        if (!haveIn && d.callee->outputType(v, 0).rate != graph::Rate::Const) {
          haveIn = true;
          inPort = ordinal;
          inWidth = d.callee->outputType(v, 0).width;
        }
        ++ordinal;
      } else if (nd.kind == graph::NodeKind::Output && !haveOut) {
        haveOut = true;
        outWidth = d.callee->inputType(v, 0).width;
      }
    }
    const uint32_t id = d.device->id.value;
    if (width == graph::kWidthMono && inWidth == graph::kWidthStereo)  // a mono signal into a stereo input
      x = ir.atom("pack", {x, x}, nodeIdFor(NodeRole::Device, id, 1), 2);
    const graph::NodeIndex c = callDevice(ir, d);
    ir.patch().connect(x, PortRef{c, static_cast<uint16_t>(inPort)});
    x = PortRef{c, 0};
    if (outWidth != graph::kWidthInfer) width = outWidth;
    else if (inWidth == graph::kWidthStereo) width = graph::kWidthStereo;
  }
  return x;
}

Result<graph::Patch> buildPatch(const Plan& pl, uint32_t index, const LowerOptions& o, bool& voices,
                                std::vector<DeviceId>* missing) {
  const Node& n = pl.nodes[index];
  Ir ir(patchName(n));
  PortRef x;
  const std::vector<LDev> devs = loweredDevices(n, o.library, missing);
  const bool instrument = n.kind == Kind::Midi && !devs.empty() && devs.front().device->kind == DeviceKind::Instrument;
  voices = instrument;
  size_t firstEffect = 0;
  uint16_t width = graph::kWidthStereo;
  if (n.kind == Kind::Midi && !instrument) {
    x = ir.konst(0.0, kC0);  // no instrument: nothing plays (its effects still run)
    width = graph::kWidthMono;
  } else if (n.kind == Kind::Midi) {
    if (!n.tunings.empty()) {
      // The tuning nodes (T2.6-fix1; one per tuning signal, chained, each taking the voices that select it: T2.6-fix3):
      // param defaults are fixed (the values arrive as params), so the IR only depends on the tunings' kinds, ids
      // and table indices. Ranges are the document's (paramRange; T2.6-fix4: the program clamps a param to its
      // declared range, so a narrower one would make the node and the static pitch differ).
      PortRef hz = ir.atom("voice.hz", {}, kVHz), cents = ir.atom("voice.cents", {}, kVCents);
      const PortRef degree = ir.atom("voice.degree", {}, kVDegree), equave = ir.atom("voice.equave", {}, kVEquave);
      const PortRef select = ir.atom("voice.tuning", {}, kVTuning);
      const PortRef c0 = ir.konst(0.0, kC0);
      for (size_t k = 0; k < n.tunings.size(); ++k) {
        const TuningNode& t = n.tunings[k];
        const std::string i = std::to_string(k + 1);
        const bool edo = t.def->edo() != nullptr;
        auto id = [&](uint32_t sub) { return nodeIdFor(NodeRole::TuningNode, t.id.value, sub); };
        auto param = [&](const char* name, double def, ParamKind kind, uint32_t sub) {
          const ParamRange r = paramRange(kind);
          return ir.param(std::string(name) + i, def, r.min, r.max, id(sub));
        };
        const PortRef tref = param("tref", 261.626, ParamKind::TuningReference, kTRef);
        const PortRef tper = param("tper", 1200.0, edo ? ParamKind::TuningEquave : ParamKind::MosPeriod, kTPer);
        const PortRef tgen = edo ? c0 : param("tgen", 700.0, ParamKind::MosGenerator, kTGen);
        const PortRef tst = edo ? c0 : param("tst", 0.0, ParamKind::MosStretch, kTSt);
        const PortRef node =
            ir.atom("tuning_hz",
                    {degree, equave, hz, cents, tgen, tper, tst, tref, ir.konst(static_cast<double>(t.table), id(kTTable)),
                     select, ir.konst(static_cast<double>(k + 1), id(kTIndex))},
                    id(kTNode));
        hz = PortRef{node.node, 0};
        cents = PortRef{node.node, 1};
      }
      ir.atom("tune_voices", {hz, cents}, kTuneVoices);
    }
    // The instrument (T4.1: the chain's first device; its first `out` is the voice signal, `done` ends voices).
    const NodeIndex syn = callDevice(ir, devs.front());
    x = ir.atom("voice_sum", {PortRef{syn, 0}}, kVoiceSum);
    if (const auto done = ir.patch().findOutput(syn, "done"))
      ir.output("done", graph::portType(graph::Kind::Bool, graph::Rate::Infer, graph::Poly::Voice, 1),
                PortRef{syn, static_cast<uint16_t>(*done)}, kDone);
    firstEffect = 1;
    width = graph::kWidthMono;
    for (graph::NodeIndex v = 0; v < devs.front().callee->nodeCount(); ++v)
      if (devs.front().callee->node(v).kind == graph::NodeKind::Output) {
        if (devs.front().callee->inputType(v, 0).width == graph::kWidthStereo) width = graph::kWidthStereo;
        break;
      }
  } else if (n.kind == Kind::Return) {
    // The sum of the sends into it (T3.17-fix2): one input and one gain_add per sender, ids from the sender.
    const PortRef c0 = ir.konst(0.0, kC0);
    const PortRef time = ir.konst(static_cast<double>(o.smoothingMs) * 1e-3, kCTime);
    const PortRef floor = ir.konst(kMinVolumeDb, kCFloor);
    PortRef acc = c0;
    for (const SendLink& l : pl.sends) {
      if (l.to != index) continue;
      const uint32_t from = pl.nodes[l.from].track->id.value;
      auto id = [&](uint32_t sub) { return nodeIdFor(NodeRole::SendInput, from, sub); };
      PortRef src = ir.input("s" + std::to_string(from), c0, id(kSIn));
      if (l.delayed) src = ir.atom("z1", {src}, id(kSDelay));  // closes a loop: 1 sample (SPEC §15 2026-10-24)
      const PortRef level = ir.param("send" + std::to_string(from), kMinVolumeDb, kMinVolumeDb, kMaxVolumeDb, id(kSLevel));
      const PortRef g = ir.atom("select", {ir.atom("le", {level, floor}, id(kSLe)), c0, ir.atom("db_to_gain", {level}, id(kSDb))},
                                id(kSGain));
      acc = ir.atom("gain_add", {acc, src, g, time}, id(kSAdd));
    }
    x = acc;
  } else {
    x = ir.input("in", ir.konst(0.0, kC0), kIn);
  }
  x = effectChain(ir, devs, firstEffect, x, width);
  gainStages(ir, n.kind, x, o.smoothingMs);
  if (ir.literalOverflow())
    return Error{ErrorCode::Overflow, "lower: track " + std::to_string(n.track->id.value) + ": more than " +
                                          std::to_string(kEntityNodeBase - kLiteralPool) + " device settings", {}};
  if (!ir.finish())
    return Error{ErrorCode::Overflow, "lower: track " + std::to_string(n.track->id.value) +
                                          ": a document id too large for a stable node id (nodeIdFor)", {}};
  return std::move(ir.patch());
}

void addParams(const Plan& pl, uint32_t index, const AudibilityMap& aud, std::vector<LoweredParam>& out,
               const graph::PatchLibrary* lib) {
  const Node& n = pl.nodes[index];
  const Track& t = *n.track;
  const std::string id = std::to_string(t.id.value);
  // T4.1: every param of every lowered device (unconnected at the call: d<id>__<name> in the track patch).
  if (lib != nullptr)
    for (const LDev& d : loweredDevices(n, lib, nullptr)) {
      const std::string dev = std::to_string(d.device->id.value);
      for (graph::NodeIndex v = 0; v < d.callee->nodeCount(); ++v) {
        if (d.callee->node(v).kind != graph::NodeKind::Param) continue;
        const std::string& name = d.callee->nodeName(v);
        const DeviceValue* set = d.device->param(name);
        const double value = set != nullptr ? set->value : d.callee->paramDecl(v).defaultValue.value();
        out.push_back(LoweredParam{"dev" + dev + ":" + name, index, "d" + dev + "__" + name, static_cast<float>(value),
                                   ParamRef::device(d.device->id, name)});
      }
    }
  auto add = [&](const std::string& name, float v, std::optional<ParamRef> ref) {
    out.push_back(LoweredParam{id + ":" + name, index, name, v, ref});
  };
  add("volume", std::clamp(t.mixer.volumeDb, kMinVolumeDb, kMaxVolumeDb), ParamRef::track(ParamKind::TrackVolume, t.id));
  add("pan", std::clamp(t.mixer.pan, -1.0f, 1.0f), ParamRef::track(ParamKind::TrackPan, t.id));
  if (n.kind != Kind::Master) {
    const TrackAudibility* a = aud.find(t.id);
    add("gate", a == nullptr || a->output ? 1.0f : 0.0f, std::nullopt);
    add("sendGate", a == nullptr || a->sends ? 1.0f : 0.0f, std::nullopt);
    add("mute", t.mixer.mute ? 1.0f : 0.0f, ParamRef::track(ParamKind::TrackMute, t.id));
  }
  if (n.kind == Kind::Return)
    for (const SendLink& l : pl.sends) {
      if (l.to != index) continue;
      const TrackId from = pl.nodes[l.from].track->id;
      out.push_back(LoweredParam{std::to_string(from.value) + ":send:" + id, index, "send" + std::to_string(from.value),
                                 l.levelDb, ParamRef::track(ParamKind::TrackSend, from, t.id)});
    }
  for (size_t k = 0; k < n.tunings.size(); ++k) {  // tuning node params: one key per tuning, shared by its followers
    const TuningNode& node = n.tunings[k];
    const TuningDef& d = *node.def;
    const std::string i = std::to_string(k + 1);
    auto tuningParam = [&](const char* name, ParamKind kind, double v) {
      const ParamRange r = paramRange(kind);
      const ParamRef ref = ParamRef::tuning(kind, node.id);
      out.push_back(LoweredParam{paramKey(ref), index, name + i, static_cast<float>(std::clamp(v, r.min, r.max)), ref});
    };
    tuningParam("tref", ParamKind::TuningReference, d.referenceHz);
    if (const tuning::Edo* e = d.edo()) {
      tuningParam("tper", ParamKind::TuningEquave, e->equaveCents());
    } else {
      const tuning::MosParams& m = *d.mos();
      tuningParam("tper", ParamKind::MosPeriod, m.period);
      tuningParam("tgen", ParamKind::MosGenerator, m.generator);
      tuningParam("tst", ParamKind::MosStretch, m.octaveStretch);
    }
  }
}

graph::TrackRole roleOf(Kind k) {
  switch (k) {
    case Kind::Midi: return graph::TrackRole::Instrument;
    case Kind::Group: return graph::TrackRole::Group;
    case Kind::Return: return graph::TrackRole::Return;
    case Kind::Master: return graph::TrackRole::Master;
  }
  return graph::TrackRole::Audio;
}

// 64-bit FNV-1a over plain values.
struct Hasher {
  uint64_t h = 1469598103934665603ull;
  void bytes(const void* p, size_t n) noexcept {
    const auto* b = static_cast<const unsigned char*>(p);
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
  }
  template <class T>
  void operator()(const T& v) noexcept {
    bytes(&v, sizeof v);
  }
  void str(std::string_view s) noexcept {
    (*this)(s.size());
    bytes(s.data(), s.size());
  }
  void constant(const graph::Constant& c) noexcept {
    (*this)(c.kind);
    (*this)(c.unit);
    (*this)(std::bit_cast<uint64_t>(c.literal));
  }
};

}  // namespace

uint32_t nodeIdFor(NodeRole role, uint32_t entity, uint32_t k) noexcept {
  const uint64_t id = kEntityNodeBase + (static_cast<uint64_t>(entity) * 3 + static_cast<uint32_t>(role)) * kEntityNodeSlots + k;
  return id <= 0xFFFFFFFFull && k < kEntityNodeSlots ? static_cast<uint32_t>(id) : 0u;
}

uint64_t patchHash(const graph::Patch& p) noexcept {
  Hasher h;
  h.str(p.name());
  for (NodeIndex n = 0; n < p.nodeCount(); ++n) {
    const graph::Node& nd = p.node(n);
    h(nd.id.value);
    h(nd.kind);
    h(nd.atom);
    h(nd.numInputs);
    h(nd.numOutputs);
    h.str(p.nodeName(n));
    switch (nd.kind) {
      case graph::NodeKind::Const: h.constant(p.constant(n)); break;
      case graph::NodeKind::Param: {
        const graph::ParamDecl& d = p.paramDecl(n);
        h.constant(d.defaultValue);
        h(d.hasRange);
        h.constant(d.rangeMin);
        h.constant(d.rangeMax);
        break;
      }
      case graph::NodeKind::Subcircuit: h.str(p.subcircuit(n).patch); break;
      default: break;
    }
    for (uint32_t i = 0; i < static_cast<uint32_t>(nd.numInputs) + nd.numOutputs; ++i) {
      const graph::PortType& t = p.portTypes()[nd.firstPort + i];
      h(t.kind);
      h(t.rate);
      h(t.poly);
      h(t.width);
    }
  }
  for (const graph::Wire& w : p.wires()) {
    h(p.node(w.from.node).id.value);
    h(w.from.port);
    h(p.node(w.to.node).id.value);
    h(w.to.port);
  }
  return h.h;
}

std::string patchText(const LoweredPatch& p) { return graph::print(p.patch); }

std::string paramKey(const ParamRef& ref) {
  switch (ref.kind) {
    case ParamKind::TrackVolume: return std::to_string(ref.owner) + ":volume";
    case ParamKind::TrackPan: return std::to_string(ref.owner) + ":pan";
    case ParamKind::TrackMute: return std::to_string(ref.owner) + ":mute";  // T3.17-fix3: its lane plays
    case ParamKind::TrackSend: return std::to_string(ref.owner) + ":send:" + std::to_string(ref.target);
    // The tuning node's params (T2.6-fix1).
    case ParamKind::TuningReference: return "tuning:" + std::to_string(ref.owner) + ":reference";
    case ParamKind::TuningEquave: return "tuning:" + std::to_string(ref.owner) + ":equave";
    case ParamKind::MosGenerator: return "tuning:" + std::to_string(ref.owner) + ":generator";
    case ParamKind::MosPeriod: return "tuning:" + std::to_string(ref.owner) + ":period";
    case ParamKind::MosStretch: return "tuning:" + std::to_string(ref.owner) + ":stretch";
    default: return {};  // solo is derived into the gates
  }
}

TuningId trackTuningSignal(const Project& p, const TrackPath& path) noexcept {
  const EffectiveTuning e = effectiveTuningRef(p, path);
  return e.main.valid() && e.def != nullptr && hasTuningSignal(*e.def) ? e.main : TuningId{};
}

std::vector<TuningId> trackTuningSignals(const Project& p, const TrackPath& path) {
  std::vector<TuningId> out;
  if (const TuningId t = trackTuningSignal(p, path); t.valid()) out.push_back(t);
  std::vector<TuningId> overrides;
  for (const ClipSlot& c : trackAt(p, path).clips) {
    if (c.clip->tuning.mode != TuningMode::Main) continue;
    const EffectiveTuning e = effectiveTuningRef(p, path, c.clip.get());
    if (e.main.valid() && e.def != nullptr && hasTuningSignal(*e.def)) overrides.push_back(e.main);
  }
  std::sort(overrides.begin(), overrides.end(), [](TuningId a, TuningId b) { return a.value < b.value; });
  for (const TuningId t : overrides)
    if (std::find(out.begin(), out.end(), t) == out.end()) out.push_back(t);
  return out;
}

uint32_t tuningNodeIndex(std::span<const TuningId> signals, TuningId t) noexcept {
  for (size_t i = 0; i < signals.size(); ++i)
    if (signals[i] == t) return static_cast<uint32_t>(i + 1);
  return 0;
}

Result<LoweredProject> lower(const Project& p, const LowerOptions& o, const LoweredProject* previous) {
  if (!p.master) return Error{ErrorCode::InvalidArgument, "lower: the project has no master", {}};
  if (o.library == nullptr) return Error{ErrorCode::InvalidArgument, "lower: no device library", {}};
  const Plan pl = plan(p, &o.tuningTableOf, o.library);
  const AudibilityMap aud = computeAudibility(p, AudibilityOptions{true});
  LoweredProject out;
  out.graph.library = o.library;
  std::unordered_map<uint32_t, std::shared_ptr<const LoweredPatch>> prev;  // track id → its previous patch
  if (previous != nullptr)
    for (size_t j = 0; j < previous->trackIds.size(); ++j) prev.emplace(previous->trackIds[j].value, previous->patches[j]);
  for (uint32_t i = 0; i < pl.nodes.size(); ++i) {
    const Node& n = pl.nodes[i];
    bool voices = false;
    auto built = buildPatch(pl, i, o, voices, &out.missingDevices);
    if (!built.ok()) return built.error();
    std::shared_ptr<const LoweredPatch> lp;
    const uint64_t hash = patchHash(built.value());
    if (const auto it = prev.find(n.track->id.value);
        it != prev.end() && it->second->hash == hash && it->second->patch == built.value()) {
      lp = it->second;  // the same IR: the same patch object, its unit is not recompiled
    } else {
      auto made = std::make_shared<LoweredPatch>();
      made->patch = std::move(built.value());
      made->hash = hash;
      lp = std::move(made);
    }
    out.patches.push_back(lp);
    out.trackIds.push_back(n.track->id);
    graph::ProjectTrack pt;
    pt.id = n.track->id.value;
    pt.name = n.track->name;
    pt.patch = &out.patches.back()->patch;
    // T4.1: a MIDI track without an active instrument is a plain audio track (no voices, no notes).
    pt.role = n.kind == Kind::Midi && !voices ? graph::TrackRole::Audio : roleOf(n.kind);
    pt.eventSlot = voices && o.slotOf ? o.slotOf(n.track->id) : graph::kNone;
    if (voices && pt.eventSlot == graph::kNone) out.withoutSlot.push_back(n.track->id);
    out.graph.tracks.push_back(std::move(pt));
    addParams(pl, i, aud, out.params, o.library);
  }
  out.master = pl.master;
  // Wires: outputs, then sends (wire order = summing order).
  using W = graph::WireKind;
  for (uint32_t i = 0; i < pl.nodes.size(); ++i)
    if (pl.dest[i] != graph::kNone) out.graph.wires.push_back({{i, "out"}, {pl.dest[i], "in"}, W::Audio});
  for (const SendLink& l : pl.sends) {
    const TrackId from = pl.nodes[l.from].track->id;
    out.graph.wires.push_back({{l.from, l.pre ? "pre" : "post"}, {l.to, "s" + std::to_string(from.value)}, W::Audio});
    if (l.delayed) out.delayedSends.push_back(DelayedSend{from, pl.nodes[l.to].track->id});
  }
  out.graph.output = {pl.master, "out"};
  return out;
}

std::vector<LoweredParam> lowerParams(const Project& p, bool tuningNodes, const graph::PatchLibrary* library) {
  const TableOf anyTable = [](TuningId, const TuningDef&) { return 0u; };  // the index does not change a param
  const Plan pl = plan(p, tuningNodes ? &anyTable : nullptr, library);
  const AudibilityMap aud = computeAudibility(p, AudibilityOptions{true});
  std::vector<LoweredParam> out;
  for (uint32_t i = 0; i < pl.nodes.size(); ++i) addParams(pl, i, aud, out, library);
  return out;
}

std::vector<DelayedSend> delayedSends(const Project& p) {
  const Plan pl = plan(p, nullptr, nullptr);
  std::vector<DelayedSend> out;
  for (const SendLink& l : pl.sends)
    if (l.delayed) out.push_back(DelayedSend{pl.nodes[l.from].track->id, pl.nodes[l.to].track->id});
  return out;
}

std::vector<uint64_t> conditionalSendsKey(const Project& p) {
  const Plan pl = plan(p, nullptr, nullptr);
  std::vector<uint64_t> key;
  for (const SendLink& l : pl.sends)
    if (l.conditional)
      key.push_back((static_cast<uint64_t>(pl.nodes[l.from].track->id.value) << 33) ^
                    (static_cast<uint64_t>(pl.nodes[l.to].track->id.value) << 1) ^ (l.delayed ? 1u : 0u));
  return key;
}

}  // namespace pq::doc
