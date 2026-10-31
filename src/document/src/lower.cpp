// Project → graph lowering (T3.17; IR with stable ids T3.17-fix2): pq/document/lower.h describes the signal flow.
#include "pq/document/lower.h"

#include <algorithm>
#include <deque>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstring>
#include <map>
#include <optional>
#include <numbers>
#include <span>
#include <tuple>
#include <unordered_map>

#include "pq/core/automation_curve.h"
#include "pq/document/audibility.h"
#include "pq/document/devices.h"
#include "pq/document/pitch_resolve.h"
#include "pq/graph/kernel_map.h"
#include "pq/graph/patch_file.h"

namespace pq::doc {

namespace {

using graph::NodeIndex;
using graph::PortRef;

enum class Kind : uint8_t { Midi, Group, Return, Master, Audio };

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

// The patches devices call (T4.4-fix1): the library's by name; a user device's own root (its resolution, kept alive by
// the UserCallees that made this).
struct Callees {
  const graph::PatchLibrary* lib = nullptr;  // the library, or the combined one (LoweredProject::userLibrary)
  std::unordered_map<uint32_t, const graph::Patch*> userRoot;  // device id → its own root (null: nothing plays)
  const graph::Patch* find(const Device& d) const noexcept {
    if (lib == nullptr) return nullptr;
    if (!d.source) return lib->find(d.patch);
    const auto it = userRoot.find(d.id.value);
    return it == userRoot.end() ? nullptr : it->second;
  }
};

// The callees of `p`'s devices: `base` alone while no device is a user device. With user devices and `combined`
// (what a graph needs: lower(), the wire planner's walk into callees), a combined library of their renamed patches,
// the base library and the stdlib files their texts import; without it (the param fast path: only roots are looked
// at) nothing is built but the per-device lookup (a cache hit per user device).
struct UserCallees {
  Callees callees;
  std::shared_ptr<graph::PatchLibrary> combined;
  std::vector<std::shared_ptr<const ResolvedDevice>> held;
  std::vector<LoweredProject::DeviceError> errors;
};
UserCallees makeCallees(const Project& p, const graph::PatchLibrary* base, const DeviceLibrary* devices, bool combined) {
  UserCallees u;
  u.callees.lib = base;
  if (base == nullptr) return u;
  std::vector<const graph::PatchFile*> extra;
  forEachDevice(p, [&](const Device& d, const Track&) {  // T4.8: inside racks too
    if (!d.source) return;
    std::shared_ptr<const ResolvedDevice> r = devices != nullptr ? devices->resolve(d) : nullptr;
    if (!r) {
      u.callees.userRoot[d.id.value] = nullptr;
      u.errors.push_back({d.id, "edited devices need the device library", false});
      return;
    }
    if (!r->error.empty()) u.errors.push_back({d.id, r->error, r->graph != nullptr});
    u.callees.userRoot[d.id.value] = r->root();
    if (combined) {
      if (!u.combined) u.combined = std::make_shared<graph::PatchLibrary>();
      if (r->graph) u.combined->addAll(r->graph->library);
      for (const graph::PatchFile* f : r->extraFiles)
        if (std::find(extra.begin(), extra.end(), f) == extra.end()) extra.push_back(f);
    }
    u.held.push_back(std::move(r));
  });
  if (u.combined) {
    u.combined->addAll(*base);
    for (const graph::PatchFile* f : extra) u.combined->add(*f);  // first under a name wins: the stock set's
    u.callees.lib = u.combined.get();
  }
  return u;
}

// T4.1: whether a MIDI track lowers an instrument (its first active device, found in the library as one).
bool lowersInstrument(const Track& t, const Callees* lib) noexcept;

// The lowered nodes of a project (active tracks), in forEachTrack order: main tree depth-first, returns, master;
// their routing and their sends. `tableOf` (optional): MIDI tracks following a tuning signal get a tuning node with
// that table (only those that lower an instrument with `lib`: the node feeds its voices).
Plan plan(const Project& p, const TableOf* tableOf, const Callees* lib) {
  Plan pl;
  struct Walk {
    const Project& p;
    std::vector<Node>& nodes;
    const TableOf* tableOf;
    const Callees* lib;
    void operator()(const std::vector<TrackPtr>& list, const Track* parent, TrackPath path) {
      for (uint32_t i = 0; i < list.size(); ++i) {
        const TrackPtr& t = list[i];
        if (!t->active) continue;  // deactivated: left out with its members (Q15)
        if (t->kind != TrackKind::Midi && t->kind != TrackKind::Group && t->kind != TrackKind::Audio) continue;
        TrackPath here = path;
        if (here.depth < kMaxTrackDepth) here.index[here.depth++] = i;
        Node n;
        n.track = t.get();
        n.kind = t->kind == TrackKind::Midi ? Kind::Midi : t->kind == TrackKind::Audio ? Kind::Audio : Kind::Group;
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
  kZoneHz = 76, kZoneLnHz, kZoneVelocity,  // T4.8: the voice values key / velocity zones read (one each per patch)
  kClipPlayer = 80, kClipSlot,             // T5.3: an audio track's source and its slot constant
  kZoneDegree = 82, kZoneDegreeOk, kZoneDegreeSafe,  // T4.17: voice.noteDegree, not NaN, NaN → 0 (Degree key zones)
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

  // An input given as kPending is left unconnected (T4.8: a rack param's consumer, connected once its value is built).
  PortRef atom(std::string_view name, std::initializer_list<PortRef> ins, uint32_t id, uint16_t repeat = 0) {
    const NodeIndex n = take(p_.addAtom(graph::atomId(name), repeat), id);
    uint16_t i = 0;
    for (const PortRef& in : ins) {
      if (in.node != graph::kNone) p_.connect(in, PortRef{n, i});
      ++i;
    }
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
  PortRef input(const std::string& name, PortRef def, uint32_t id, graph::PortType type = kStereo) {
    const NodeIndex n = take(p_.addInput(name, type), id);
    p_.connect(def, PortRef{n, 0});
    return PortRef{n, 0};
  }
  // T4.7: a param with a callee's own declaration (a modulated device param, now a root param of the track patch).
  PortRef paramDecl(const std::string& name, graph::ParamDecl decl, uint32_t id) {
    return PortRef{take(p_.addParam(name, std::move(decl)), id), 0};
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
    case Kind::Audio: return "Audio" + id;
  }
  return id;
}

// ---- Device chains (T4.1; racks T4.8) ------------------------------------------------------------------------------

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

// A real `param` of a callee: its pin (the signature index: the `in`s, then the params) and its declaration node.
struct ParamPin {
  uint16_t pin = 0;
  NodeIndex node = graph::kNone;
};
std::optional<ParamPin> realParamPin(const graph::Patch& callee, std::string_view name) noexcept {
  uint32_t ins = 0;
  for (NodeIndex v = 0; v < callee.nodeCount(); ++v)
    if (callee.node(v).kind == graph::NodeKind::Input) ++ins;
  uint32_t ordinal = 0;
  for (NodeIndex v = 0; v < callee.nodeCount(); ++v) {
    if (callee.node(v).kind != graph::NodeKind::Param) continue;
    if (callee.nodeName(v) == name && callee.paramDecl(v).kind == graph::Kind::Real)
      return ParamPin{static_cast<uint16_t>(ins + ordinal), v};
    ++ordinal;
  }
  return std::nullopt;
}

// T4.8: a rack's params as the lowering sees them: "macro1" … "macro16" then "select", each 0 … 127, default 0.
constexpr uint32_t kRackParams = kMacroCount + 1;
uint32_t rackParamIndex(std::string_view name) noexcept {  // kNone: not a rack param
  if (name == kSelectParam) return kMacroCount;
  const std::optional<uint32_t> m = macroIndex(name);
  return m ? *m : graph::kNone;
}
std::string rackParamName(uint32_t i) { return i == kMacroCount ? std::string(kSelectParam) : macroParamName(i); }
const graph::ParamDecl& rackParamDecl() {
  static const graph::ParamDecl d = [] {
    graph::ParamDecl x;
    x.defaultValue = graph::Constant::real(0.0);
    x.hasRange = true;
    x.rangeMin = graph::Constant::real(0.0);
    x.rangeMax = graph::Constant::real(kMacroMax);
    return x;
  }();
  return d;
}

// A lowered device (in a track's flat list, depth-first in chain order: a rack, then its chains' devices).
struct LDev {
  const Device* device = nullptr;
  const graph::Patch* callee = nullptr;  // a leaf device's patch (null for a rack)
  int32_t rack = -1;                     // T4.8: index into TrackDevs::racks for a rack
  bool voice = false;                    // its signal is per voice (an instrument; an effect of a per-voice chain)
};
struct LChain {
  const Chain* chain = nullptr;
  std::vector<uint32_t> devs;  // indices into TrackDevs::devs
};
struct LRack {
  std::vector<LChain> chains;  // the chains that play (active; soloed if any is; with an instrument in an Instrument Rack)
  bool passThrough = false;    // an empty Audio Effect Rack (no chains at all) passes its input through
};
struct LMapping {
  const MacroMapping* m = nullptr;
  uint32_t rack = 0;    // index into TrackDevs::devs
  uint32_t target = 0;  // index into TrackDevs::devs (the rack itself for its selector)
};
// The lowered device tree of one track.
struct TrackDevs {
  std::vector<LDev> devs;
  std::vector<uint32_t> top;  // the track's own chain
  std::vector<LRack> racks;
  std::vector<LMapping> mappings;
  bool voices = false;  // the track's chain starts with a lowered instrument (a device, or a rack that plays)

  size_t size() const noexcept { return devs.size(); }
  bool empty() const noexcept { return devs.empty(); }
  const LDev& operator[](size_t i) const noexcept { return devs[i]; }
  const LMapping* mappingOf(uint32_t dev, std::string_view param) const noexcept {
    for (const LMapping& m : mappings)
      if (m.target == dev && m.m->param == param) return &m;
    return nullptr;
  }
  // Whether a rack param is read by the rack's own graph (a mapping of that macro, a select zone).
  bool rackParamConsumed(uint32_t dev, uint32_t param) const noexcept {
    if (param == kMacroCount) {
      for (const LChain& c : racks[static_cast<size_t>(devs[dev].rack)].chains)
        if (!zoneOff(c.chain->select, ZoneKind::Select, KeyZoneUnit::Hz)) return true;
      return false;
    }
    for (const LMapping& m : mappings)
      if (m.rack == dev && m.m->macro == param) return true;
    return false;
  }
};

bool soloIn(const Rack& r) noexcept {
  return std::any_of(r.chains.begin(), r.chains.end(), [](const ChainPtr& c) { return c->active && c->solo; });
}
bool chainPlays(const Chain& c, bool solo) noexcept { return c.active && (!solo || c.solo); }

// Whether a device list's first active device lowers as an instrument that plays (a device patch that is one, or an
// Instrument Rack with such a chain). M4 review M9: iterative (the lists still to look at), and `plays` (when given)
// answers for an Instrument Rack already decided, so lowerList asks about each rack once (O(devices), not O(depth²)).
bool listLowersInstrument(const std::vector<DevicePtr>& list, const Callees* lib,
                          const std::unordered_map<const Device*, bool>* plays = nullptr) {
  if (lib == nullptr) return false;
  std::vector<const std::vector<DevicePtr>*> todo{&list};
  while (!todo.empty()) {
    const std::vector<DevicePtr>& l = *todo.back();
    todo.pop_back();
    for (const DevicePtr& d : l) {
      if (!d->active) continue;
      if (d->kind != DeviceKind::Instrument) break;
      if (d->rack) {
        if (plays != nullptr)
          if (const auto it = plays->find(d.get()); it != plays->end()) {
            if (it->second) return true;
            break;
          }
        const bool solo = soloIn(*d->rack);
        for (size_t i = d->rack->chains.size(); i-- > 0;)
          if (chainPlays(*d->rack->chains[i], solo)) todo.push_back(&d->rack->chains[i]->devices);
        break;
      }
      const graph::Patch* callee = lib->find(*d);
      if (callee != nullptr && calleeIsInstrument(*callee)) return true;
      break;
    }
  }
  return false;
}

bool lowersInstrument(const Track& t, const Callees* lib) noexcept {
  return t.kind == TrackKind::Midi && listLowersInstrument(t.devices, lib);
}

// Whether each Instrument Rack inside `list` plays (has a playing chain whose first active device lowers as an
// instrument), children before parents (reverse pre-order), so every answer reads its inner racks' answers.
std::unordered_map<const Device*, bool> instrumentRacksPlay(const std::vector<DevicePtr>& list, const Callees* lib) {
  std::vector<const Device*> racks;
  forEachDeviceIn(list, [&](const Device& d) {
    if (d.rack && d.kind == DeviceKind::Instrument) racks.push_back(&d);
  });
  std::unordered_map<const Device*, bool> plays;
  for (size_t i = racks.size(); i-- > 0;) {
    const Device& d = *racks[i];
    const bool solo = soloIn(*d.rack);
    bool any = false;
    for (const ChainPtr& c : d.rack->chains)
      if (!any && chainPlays(*c, solo)) any = listLowersInstrument(c->devices, lib, &plays);
    plays[&d] = any;
  }
  return plays;
}

// Lowers one device list into `td` (its indices into `out`): active devices whose patch is in the library and of
// their kind (an instrument only first, where instruments go), racks with the chains that play. `missing`: devices
// bypassed because their patch is not in the library or not of their kind. M4 review M9: iterative (a stack of the open
// lists and racks; a deque keeps each open chain's index list in place while its devices are lowered), in the same
// order as the recursive version (the same indices).
void lowerList(TrackDevs& td, const std::vector<DevicePtr>& list, bool instrumentAllowed, const Callees* lib,
               std::vector<DeviceId>* missing, std::vector<uint32_t>& out) {
  const std::unordered_map<const Device*, bool> plays = instrumentRacksPlay(list, lib);
  struct Open {
    const std::vector<DevicePtr>* list = nullptr;  // a device list (null: a rack, lowering its chains)
    size_t next = 0;
    bool instrumentAllowed = false;  // list
    std::vector<uint32_t>* out = nullptr;  // the list's indices (a rack: its list's, for the rack's own index)
    const Device* rack = nullptr;    // rack
    bool solo = false;
    uint32_t idx = 0;
    int32_t ri = -1;
    LRack lr;
    LChain chain;                    // the chain being lowered (`list` of the frame above fills chain.devs)
    uint32_t from = 0;
    bool open = false;
  };
  std::deque<Open> stack;
  {
    Open& o = stack.emplace_back();
    o.list = &list;
    o.instrumentAllowed = instrumentAllowed;
    o.out = &out;
  }
  while (!stack.empty()) {
    Open& f = stack.back();
    if (f.list != nullptr) {
      if (f.next == f.list->size()) {
        stack.pop_back();
        continue;
      }
      const DevicePtr& d = (*f.list)[f.next++];
      if (!d->active) continue;
      const bool instrument = d->kind == DeviceKind::Instrument;
      if (instrument && (!f.instrumentAllowed || !f.out->empty())) {
        if (missing != nullptr) missing->push_back(d->id);
        continue;
      }
      if (d->rack) {
        if (instrument && !plays.at(d.get())) continue;  // nothing plays: left out
        Open& r = stack.emplace_back();  // `f` stays valid (deque)
        r.rack = d.get();
        r.out = f.out;
        r.solo = soloIn(*d->rack);
        r.idx = static_cast<uint32_t>(td.devs.size());
        r.ri = static_cast<int32_t>(td.racks.size());
        td.devs.push_back(LDev{d.get(), nullptr, r.ri, false});
        td.racks.emplace_back();
        r.lr.passThrough = !instrument && d->rack->chains.empty();
        continue;
      }
      const graph::Patch* callee = lib != nullptr ? lib->find(*d) : nullptr;
      if (callee == nullptr || calleeIsInstrument(*callee) != instrument) {
        if (missing != nullptr) missing->push_back(d->id);
        continue;
      }
      f.out->push_back(static_cast<uint32_t>(td.devs.size()));
      td.devs.push_back(LDev{d.get(), callee, -1, instrument});
      continue;
    }
    const Rack& r = *f.rack->rack;
    const bool instrument = f.rack->kind == DeviceKind::Instrument;
    if (f.open) {  // the chain's devices are lowered
      // A per-voice chain (SPEC §8.2 voicechain): the effects after a device instrument run per voice.
      if (instrument && r.voiceChains && !f.chain.devs.empty() && td.devs[f.chain.devs.front()].rack < 0)
        for (auto i = f.from; i < td.devs.size(); ++i) td.devs[i].voice = true;
      f.lr.chains.push_back(std::move(f.chain));
      f.chain = LChain{};
      f.open = false;
    }
    bool descended = false;
    while (!descended && f.next < r.chains.size()) {
      const ChainPtr& c = r.chains[f.next++];
      if (!chainPlays(*c, f.solo)) continue;
      if (instrument && !listLowersInstrument(c->devices, lib, &plays)) continue;
      f.chain.chain = c.get();
      f.from = static_cast<uint32_t>(td.devs.size());
      f.open = true;
      Open& l = stack.emplace_back();
      l.list = &c->devices;
      l.instrumentAllowed = instrument;
      l.out = &f.chain.devs;
      descended = true;
    }
    if (descended) continue;
    td.racks[static_cast<size_t>(f.ri)] = std::move(f.lr);
    // Its mappings onto lowered devices inside it (or onto its own selector).
    for (const MacroMapping& m : r.mappings) {
      uint32_t target = graph::kNone;
      if (m.device == f.rack->id) {
        if (m.param == kSelectParam) target = f.idx;
      } else {
        for (auto i = f.idx + 1; i < td.devs.size(); ++i)
          if (td.devs[i].device->id == m.device) target = i;
      }
      if (target == graph::kNone) continue;
      const LDev& t = td.devs[target];
      const bool ok = t.rack >= 0 ? rackParamIndex(m.param) != graph::kNone : realParamPin(*t.callee, m.param).has_value();
      if (ok) td.mappings.push_back(LMapping{&m, f.idx, target});
    }
    f.out->push_back(f.idx);
    stack.pop_back();
  }
}

TrackDevs lowerTree(const Node& n, const Callees* lib, std::vector<DeviceId>* missing) {
  TrackDevs td;
  lowerList(td, n.track->devices, n.kind == Kind::Midi, lib, missing, td.top);
  td.voices = n.kind == Kind::Midi && !td.top.empty() && td.devs[td.top.front()].device->kind == DeviceKind::Instrument;
  return td;
}

// The lowered devices of every plan node (`missing`: as lowerList).
std::vector<TrackDevs> allDevices(const Plan& pl, const Callees* lib, std::vector<DeviceId>* missing) {
  std::vector<TrackDevs> out(pl.nodes.size());
  for (size_t i = 0; i < pl.nodes.size(); ++i) out[i] = lowerTree(pl.nodes[i], lib, missing);
  return out;
}

// ---- Modulation wires (T4.7): which ports depend on which without a delay ------------------------------------------

// Whether input 0 of `atom` breaks a loop as the scheduler sees it (schedule.cpp: z1.x and the delay lines' x have
// split kernels; phasor's `hz` and the other PortSpec::delayed inputs have none, so a loop through them still needs a
// z1: PATCH_LANGUAGE §3, QUESTIONS Q43).
bool splitsLoops(graph::AtomId atom) noexcept {
  const graph::KernelMapping& m = graph::kernelMapping(atom);
  if (m.lowering != graph::Lowering::Kernel) return false;
  return m.kernel == dsp::Atom::Z1 || m.kernel == dsp::Atom::DelayLinear || m.kernel == dsp::Atom::DelayCubic ||
         m.kernel == dsp::Atom::DelayAllpass;
}

// For a patch: which inputs of its signature (its `in`s then its `param`s, subcircuitSignature order) each of its
// outputs (`out`s then `export`s) depends on WITHOUT a delay the scheduler can split a loop at. Conservative (a
// dependency may be reported that a finer analysis would drop, never the reverse): an atom's outputs depend on all its
// inputs except z1's and the delay lines' signal input (splitsLoops); a solve block's
// unknowns on everything its equations read; a subcircuit call as its callee says (recursively; a callee missing from
// the library, or recursion: all its inputs). Memoised per patch (the library's patches live as long as the analysis).
class PortDeps {
public:
  struct Matrix {
    uint32_t ins = 0, words = 1;
    std::vector<std::vector<uint64_t>> outs;  // per output: a bitset over the inputs
    bool dep(uint32_t o, uint32_t i) const noexcept {
      return o < outs.size() && i < ins && ((outs[o][i / 64] >> (i % 64)) & 1u) != 0;
    }
  };
  explicit PortDeps(const graph::PatchLibrary* lib) : lib_(lib) {}
  const Matrix& of(const graph::Patch& p);

private:
  const graph::PatchLibrary* lib_;
  std::unordered_map<const graph::Patch*, Matrix> memo_;  // node-based: references stay valid as it grows
  std::vector<const graph::Patch*> active_;               // recursion guard
};

const PortDeps::Matrix& PortDeps::of(const graph::Patch& p) {
  if (const auto it = memo_.find(&p); it != memo_.end()) return it->second;
  using graph::NodeKind;
  const uint32_t nn = p.nodeCount();
  std::vector<uint32_t> sigIndex(nn, graph::kNone);
  uint32_t ins = 0;
  for (NodeIndex n = 0; n < nn; ++n)
    if (p.node(n).kind == NodeKind::Input) sigIndex[n] = ins++;
  for (NodeIndex n = 0; n < nn; ++n)
    if (p.node(n).kind == NodeKind::Param) sigIndex[n] = ins++;
  const uint32_t words = std::max(1u, (ins + 63) / 64);
  // Callees first (so the fixpoint below never recurses).
  std::vector<const Matrix*> callee(nn, nullptr);
  active_.push_back(&p);
  for (NodeIndex n = 0; n < nn; ++n) {
    if (p.node(n).kind != NodeKind::Subcircuit || lib_ == nullptr) continue;
    const graph::Patch* c = lib_->find(p.subcircuit(n).patch);
    if (c != nullptr && std::find(active_.begin(), active_.end(), c) == active_.end()) callee[n] = &of(*c);
  }
  active_.pop_back();
  std::vector<std::vector<NodeIndex>> equations(p.solveCount());
  for (NodeIndex n = 0; n < nn; ++n)
    if (p.node(n).kind == NodeKind::Equation && p.node(n).payload < equations.size()) equations[p.node(n).payload].push_back(n);
  const graph::Adjacency adj = p.adjacency();
  std::vector<uint64_t> bits(static_cast<size_t>(p.portCount()) * words, 0);  // per flat output port
  auto outBits = [&](NodeIndex n, uint32_t o) { return bits.data() + static_cast<size_t>(p.flatOutput(n, o)) * words; };
  auto drv = [&](NodeIndex n, uint32_t i) -> const uint64_t* {
    const graph::WireIndex w = adj.driver[p.flatInput(n, i)];
    if (w == graph::kNone) return nullptr;
    const graph::Wire& wr = p.wire(w);
    return outBits(wr.from.node, wr.from.port);
  };
  std::vector<uint64_t> acc(words);
  auto orInto = [&](const uint64_t* src) {
    if (src != nullptr)
      for (uint32_t k = 0; k < words; ++k) acc[k] |= src[k];
  };
  bool changed = false;
  auto store = [&](NodeIndex n, uint32_t o) {
    uint64_t* d = outBits(n, o);
    for (uint32_t k = 0; k < words; ++k)
      if ((d[k] | acc[k]) != d[k]) {
        d[k] |= acc[k];
        changed = true;
      }
  };
  for (NodeIndex n = 0; n < nn; ++n)
    if (sigIndex[n] != graph::kNone) outBits(n, 0)[sigIndex[n] / 64] |= uint64_t{1} << (sigIndex[n] % 64);
  do {
    changed = false;
    for (NodeIndex n = 0; n < nn; ++n) {
      const graph::Node& nd = p.node(n);
      std::fill(acc.begin(), acc.end(), uint64_t{0});
      switch (nd.kind) {
        case NodeKind::Atom: {
          for (uint32_t i = 0; i < nd.numInputs; ++i)
            if (!(i == 0 && splitsLoops(nd.atom))) orInto(drv(n, i));
          for (uint32_t o = 0; o < nd.numOutputs; ++o) store(n, o);
          break;
        }
        case NodeKind::Subcircuit:
          for (uint32_t o = 0; o < nd.numOutputs; ++o) {
            std::fill(acc.begin(), acc.end(), uint64_t{0});
            for (uint32_t i = 0; i < nd.numInputs; ++i)
              if (callee[n] == nullptr || callee[n]->dep(o, i)) orInto(drv(n, i));
            store(n, o);
          }
          break;
        case NodeKind::Input:  // its default expression (when unconnected)
          if (nd.numInputs > 0) orInto(drv(n, 0));
          store(n, 0);
          break;
        case NodeKind::Unknown:
          for (uint32_t i = 0; i < nd.numInputs; ++i) orInto(drv(n, i));
          if (nd.payload < equations.size())
            for (const NodeIndex e : equations[nd.payload])
              for (uint32_t i = 0; i < p.node(e).numInputs; ++i) orInto(drv(e, i));
          store(n, 0);
          break;
        default: break;  // Const, Param (seeded), Output, Export, Equation: no outputs to compute
      }
    }
  } while (changed);
  Matrix m;
  m.ins = ins;
  m.words = words;
  for (const NodeKind k : {NodeKind::Output, NodeKind::Export})
    for (NodeIndex n = 0; n < nn; ++n) {
      if (p.node(n).kind != k) continue;
      const uint64_t* d = p.node(n).numInputs > 0 ? drv(n, 0) : nullptr;
      m.outs.emplace_back(d != nullptr ? std::vector<uint64_t>(d, d + words) : std::vector<uint64_t>(words, 0));
    }
  return memo_.emplace(&p, std::move(m)).first->second;
}

// The signature index of a callee's first non-const `in` (the audio input), or kNone.
uint32_t audioInIndex(const graph::Patch& callee) noexcept {
  uint32_t ordinal = 0;
  for (NodeIndex v = 0; v < callee.nodeCount(); ++v) {
    if (callee.node(v).kind != graph::NodeKind::Input) continue;
    if (callee.outputType(v, 0).rate != graph::Rate::Const) return ordinal;
    ++ordinal;
  }
  return graph::kNone;
}

// A lowered wire.
struct LWire {
  const Wire* wire = nullptr;
  uint32_t fromNode = 0, fromDev = 0;  // plan node, index into its lowered devices
  uint32_t toNode = 0, toDev = 0;
  uint16_t fromPort = 0;               // the source callee's output (signature order: outs, then exports)
  uint16_t toPin = 0;                  // the target callee's input (signature order: ins, then params); a rack: its param
  NodeIndex paramNode = graph::kNone;  // the target param's declaration in its callee (kNone: a rack param, T4.8)
  bool delayed = false;
  bool reduce = false;                 // a voice source into a global target
};

struct WirePlan {
  std::vector<LWire> wires;       // lowered, in id order
  std::vector<WireState> states;  // every wire of the project, in id order
};

// T4.8: a rack's signal points: its audio input (an Audio Effect Rack), its params (kRackParams), its output.
uint32_t rackInputs(const LDev& d) noexcept { return (d.device->kind == DeviceKind::Instrument ? 0u : 1u) + kRackParams; }
uint32_t rackParamPoint(const LDev& d, uint32_t param) noexcept {
  return (d.device->kind == DeviceKind::Instrument ? 0u : 1u) + param;
}

// Decides how every wire is lowered (lower.h, "Modulation wires"): which are left out, which close a delay-free cycle
// (delayed) and which a cycle through two instruments (refused).
WirePlan planWires(const Project& p, const Plan& pl, const std::vector<TrackDevs>& devs, PortDeps& deps) {
  WirePlan wp;
  const WireList& wires = wiresOf(p);
  if (wires.empty()) return wp;
  const auto nodes = static_cast<uint32_t>(pl.nodes.size());
  // Where each lowered device is.
  std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> where;
  for (uint32_t n = 0; n < nodes; ++n)
    for (uint32_t k = 0; k < devs[n].size(); ++k) where[devs[n][k].device->id.value] = {n, k};
  auto isInstrumentNode = [&](uint32_t n) { return pl.nodes[n].kind == Kind::Midi && devs[n].voices; };
  // Signal points: per node its input and output, per device its signature inputs then outputs (a rack: its audio
  // input and params, then its output).
  std::vector<uint32_t> nodeBase(nodes);
  std::vector<std::vector<uint32_t>> devBase(nodes);
  auto insOf = [&](uint32_t n, uint32_t k) {
    const LDev& d = devs[n][k];
    return d.rack >= 0 ? rackInputs(d) : deps.of(*d.callee).ins;
  };
  auto outsOf = [&](uint32_t n, uint32_t k) {
    const LDev& d = devs[n][k];
    return d.rack >= 0 ? 1u : static_cast<uint32_t>(deps.of(*d.callee).outs.size());
  };
  uint32_t points = 0;
  for (uint32_t n = 0; n < nodes; ++n) {
    nodeBase[n] = points;
    points += 2;
    for (uint32_t k = 0; k < devs[n].size(); ++k) {
      devBase[n].push_back(points);
      points += insOf(n, k) + outsOf(n, k);
    }
  }
  auto devIn = [&](uint32_t n, uint32_t k, uint32_t i) { return devBase[n][k] + i; };
  auto devOut = [&](uint32_t n, uint32_t k, uint32_t o) { return devBase[n][k] + insOf(n, k) + o; };
  std::vector<std::vector<uint32_t>> adj(points);
  for (uint32_t n = 0; n < nodes; ++n) {
    const TrackDevs& td = devs[n];
    for (uint32_t k = 0; k < td.size(); ++k) {  // inside each device
      const LDev& d = td[k];
      if (d.rack >= 0) {  // a rack's output follows its audio input and its selector (its macros reach it by mappings)
        if (d.device->kind != DeviceKind::Instrument) adj[devIn(n, k, 0)].push_back(devOut(n, k, 0));
        adj[devIn(n, k, rackParamPoint(d, kMacroCount))].push_back(devOut(n, k, 0));
        continue;
      }
      const PortDeps::Matrix& m = deps.of(*d.callee);
      for (uint32_t o = 0; o < m.outs.size(); ++o)
        for (uint32_t i = 0; i < m.ins; ++i)
          if (m.dep(o, i)) adj[devIn(n, k, i)].push_back(devOut(n, k, o));
    }
    for (const LMapping& lm : td.mappings) {  // T4.8: a macro feeds the params it maps
      const LDev& t = td[lm.target];
      const uint32_t to = t.rack >= 0 ? devIn(n, lm.target, rackParamPoint(t, rackParamIndex(lm.m->param)))
                                      : devIn(n, lm.target, realParamPin(*t.callee, lm.m->param)->pin);
      adj[devIn(n, lm.rack, rackParamPoint(td[lm.rack], lm.m->macro))].push_back(to);
    }
    // The chains: the track's input (or the instrument's voices) → each effect's audio input → its first out → … → the
    // track's output (as buildPatch wires it); a rack fans out to its chains and sums them.
    // M4 review M9: iterative (a stack of the open lists and racks), the same edges in the same order as the recursive
    // walk: a list carries its signal point `x` along its devices; a rack runs each chain from its input (an
    // Instrument Rack's chains start at their instruments) and joins their ends at its output.
    struct Flow {
      std::span<const uint32_t> list;  // a device list (empty with rack ≥ 0: a rack)
      size_t next = 0;
      uint32_t x = graph::kNone;       // list: the signal so far
      int64_t rack = -1;               // rack: its device index
      bool awaiting = false;           // rack: a chain's list is running (its end comes back in `ret`)
    };
    auto flow = [&](std::span<const uint32_t> top, uint32_t x0) -> uint32_t {
      std::vector<Flow> stack{Flow{top, 0, x0, -1, false}};
      uint32_t ret = graph::kNone;
      while (!stack.empty()) {
        if (stack.back().rack < 0) {
          Flow& f = stack.back();
          if (f.next == f.list.size()) {
            ret = f.x;
            stack.pop_back();
            continue;
          }
          const uint32_t k = f.list[f.next++];
          const LDev& d = td[k];
          if (d.rack >= 0) {
            const LRack& r = td.racks[static_cast<size_t>(d.rack)];
            const bool instrument = d.device->kind == DeviceKind::Instrument;
            if (!instrument && f.x != graph::kNone) adj[f.x].push_back(devIn(n, k, 0));
            if (r.passThrough) adj[devIn(n, k, 0)].push_back(devOut(n, k, 0));
            stack.push_back(Flow{{}, 0, graph::kNone, k, false});  // invalidates f
            continue;
          }
          if (d.device->kind == DeviceKind::Instrument) {  // ignores the signal so far
            f.x = deps.of(*d.callee).outs.empty() ? graph::kNone : devOut(n, k, 0);
            continue;
          }
          const uint32_t in = audioInIndex(*d.callee);
          if (f.x != graph::kNone && in != graph::kNone) adj[f.x].push_back(devIn(n, k, in));
          f.x = deps.of(*d.callee).outs.empty() ? graph::kNone : devOut(n, k, 0);
          continue;
        }
        Flow& f = stack.back();
        const auto k = static_cast<uint32_t>(f.rack);
        const LDev& d = td[k];
        const LRack& r = td.racks[static_cast<size_t>(d.rack)];
        if (f.awaiting) {
          if (ret != graph::kNone) adj[ret].push_back(devOut(n, k, 0));
          f.awaiting = false;
        }
        if (f.next < r.chains.size()) {
          const bool instrument = d.device->kind == DeviceKind::Instrument;
          const LChain& c = r.chains[f.next++];
          f.awaiting = true;
          stack.push_back(Flow{std::span<const uint32_t>(c.devs), 0, instrument ? graph::kNone : devIn(n, k, 0), -1, false});
          continue;
        }
        stack.pop_back();
        stack.back().x = devOut(n, k, 0);  // the list holding the rack goes on from its output
      }
      return ret;
    };
    // A MIDI track's chain starts at its instrument, an audio track's at its clip player (T5.3): neither reads `in`.
    const bool ownSource = pl.nodes[n].kind == Kind::Midi || pl.nodes[n].kind == Kind::Audio;
    const uint32_t x = flow(std::span<const uint32_t>(td.top), ownSource ? graph::kNone : nodeBase[n]);
    if (x != graph::kNone) adj[x].push_back(nodeBase[n] + 1);
    if (pl.dest[n] != graph::kNone) adj[nodeBase[n] + 1].push_back(nodeBase[pl.dest[n]]);
  }
  for (const SendLink& l : pl.sends)
    if (!l.delayed) adj[nodeBase[l.from] + 1].push_back(nodeBase[l.to]);
  // Track-level graph (routing, every send, cross-track wires): a cross-track wire must not close a cycle through two
  // instrument tracks (the partition could not run it, Q84).
  std::vector<std::vector<uint32_t>> tadj(nodes);
  for (uint32_t n = 0; n < nodes; ++n)
    if (pl.dest[n] != graph::kNone) tadj[n].push_back(pl.dest[n]);
  for (const SendLink& l : pl.sends) tadj[l.from].push_back(l.to);
  std::vector<uint8_t> seen;
  std::vector<uint32_t> stack;
  auto reach = [&](const std::vector<std::vector<uint32_t>>& g, uint32_t from, std::vector<uint8_t>& mark) {
    mark.assign(g.size(), 0);
    stack.assign(1, from);
    mark[from] = 1;
    while (!stack.empty()) {
      const uint32_t v = stack.back();
      stack.pop_back();
      for (uint32_t w : g[v])
        if (!mark[w]) {
          mark[w] = 1;
          stack.push_back(w);
        }
    }
  };
  std::vector<uint8_t> fwd, back;
  for (const Wire& w : wires) {
    WireState st{w.id, WireStatus::Lowered};
    const auto a = where.find(w.from.value), b = where.find(w.to.value);
    if (a == where.end() || b == where.end()) {
      st.status = WireStatus::Inactive;
      wp.states.push_back(st);
      continue;
    }
    LWire lw;
    lw.wire = &w;
    std::tie(lw.fromNode, lw.fromDev) = a->second;
    std::tie(lw.toNode, lw.toDev) = b->second;
    const LDev& sd = devs[lw.fromNode][lw.fromDev];
    const LDev& dd = devs[lw.toNode][lw.toDev];
    // The source: an export (its signature index: outs first; a rack has none); the target: a real param (a rack: a
    // macro or its selector).
    uint32_t outs = 0, exportIndex = graph::kNone, ordinal = 0;
    if (sd.rack < 0)
      for (NodeIndex v = 0; v < sd.callee->nodeCount(); ++v) {
        if (sd.callee->node(v).kind == graph::NodeKind::Output) ++outs;
        if (sd.callee->node(v).kind == graph::NodeKind::Export) {
          if (sd.callee->nodeName(v) == w.port) exportIndex = ordinal;
          ++ordinal;
        }
      }
    uint32_t pin = graph::kNone, point = graph::kNone;
    if (dd.rack >= 0) {
      pin = rackParamIndex(w.param);
      if (pin != graph::kNone) point = rackParamPoint(dd, pin);
    } else if (const std::optional<ParamPin> pp = realParamPin(*dd.callee, w.param)) {
      pin = pp->pin;
      point = pp->pin;
      lw.paramNode = pp->node;
    }
    if (exportIndex == graph::kNone || pin == graph::kNone) {
      st.status = WireStatus::Invalid;
      wp.states.push_back(st);
      continue;
    }
    lw.fromPort = static_cast<uint16_t>(outs + exportIndex);
    lw.toPin = static_cast<uint16_t>(pin);
    lw.reduce = sd.voice && !(lw.toNode == lw.fromNode && dd.voice);
    if (lw.fromNode != lw.toNode) {
      reach(tadj, lw.toNode, fwd);
      if (fwd[lw.fromNode]) {  // a track cycle forms: count its instruments
        std::vector<std::vector<uint32_t>> rev(nodes);
        for (uint32_t v = 0; v < nodes; ++v)
          for (uint32_t x : tadj[v]) rev[x].push_back(v);
        reach(rev, lw.fromNode, back);
        uint32_t instruments = 0;
        for (uint32_t v = 0; v < nodes; ++v)
          if ((fwd[v] && back[v]) || v == lw.fromNode || v == lw.toNode) instruments += isInstrumentNode(v) ? 1u : 0u;
        if (instruments > 1) {
          st.status = WireStatus::Refused;
          wp.states.push_back(st);
          continue;
        }
      }
      tadj[lw.fromNode].push_back(lw.toNode);
    }
    // A delay-free cycle: the target reaches the source.
    const uint32_t from = devOut(lw.fromNode, lw.fromDev, lw.fromPort), to = devIn(lw.toNode, lw.toDev, point);
    reach(adj, to, seen);
    lw.delayed = seen[from] != 0;
    if (lw.delayed) st.status = WireStatus::Delayed;
    else adj[from].push_back(to);
    wp.states.push_back(st);
    wp.wires.push_back(lw);
  }
  return wp;
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

// T4.12-fix1: the polyphony an instrument device asks for: its `voices` setting (a `real const` input of that name:
// the value the user set, else the input's literal default), rounded and clamped to [1, kMaxPolyphony]; 0 when the
// patch has no such setting (the track then gets the compiler's default).
uint32_t devicePolyphony(const LDev& d) {
  const graph::Patch& p = *d.callee;
  for (graph::NodeIndex n = 0; n < p.nodeCount(); ++n) {
    if (p.node(n).kind != graph::NodeKind::Input || p.nodeName(n) != "voices") continue;
    if (p.outputType(n, 0).rate != graph::Rate::Const) return 0;
    std::optional<double> v;
    for (const DeviceValue& s : d.device->settings)
      if (s.name == "voices") v = s.value;
    if (!v) {
      const graph::WireIndex w = p.adjacency().driver[p.flatInput(n, 0)];
      if (w != graph::kNone && p.node(p.wire(w).from.node).kind == graph::NodeKind::Const)
        v = p.constant(p.wire(w).from.node).value();
    }
    if (!v || !std::isfinite(*v)) return 0;
    return static_cast<uint32_t>(std::clamp(std::round(*v), 1.0, static_cast<double>(kMaxPolyphony)));
  }
  return 0;
}

// ---- Building a track patch's devices (T4.1; racks T4.8: lower.h "Racks") ------------------------------------------

constexpr PortRef kPending{graph::kNone, 0};

// Chain node slots (NodeRole::Chain, k < 48).
enum ChainK : uint32_t {
  kCGl, kCGr, kCGlSel, kCGrSel, kCPack, kCGain, kCSum, kCInGain, kCVoiceSum, kCVoiceGain, kCZoneMul, kCOuterMul, kCDone,
  kCZoneSelect = 13, kCZoneVelocity = 23, kCZoneKey = 33,  // 10 slots each: a1, b1, a2, b2 params, then the math
};
// Mapping node slots (NodeRole::Mapping, k < 48).
enum MapK : uint32_t {
  kMX, kMInv127, kMKx, kMK, kMExp, kMOne, kMSub, kMInvE, kMShape, kMRange, kMScaled, kMMin, kMPos,
  kML, kMMulL, kMLo, kMAddLo, kMExpV, kMRoundAdd, kMFloor,
};

// What building one track patch keeps.
struct Build {
  Ir& ir;
  const LowerOptions& o;
  const TrackDevs& td;
  std::vector<NodeIndex> calls;  // per lowered device: its call node (kNone: a rack)
  std::map<std::pair<uint32_t, uint32_t>, std::vector<PortRef>> rackPins;  // (rack dev, param) → the inputs reading it
  PortRef vel = kPending, lnHz = kPending, degree = kPending;  // shared voice values the zones read (made on first use)
  PortRef c0() { return ir.konst(0.0, kC0); }
  PortRef c1() { return ir.konst(1.0, kC1); }
  PortRef time() { return ir.konst(static_cast<double>(o.smoothingMs) * 1e-3, kCTime); }
};

std::string chainParamName(ChainId c, std::string_view what) { return "c" + std::to_string(c.value) + "_" + std::string(what); }

// A zone's gain (lower.h "Racks"): clip(min(v·a1 + b1, v·a2 + b2), 0, 1) with a1 … b2 runtime params the document
// derives from the zone (`tag` names them: c<chain>_<tag>0 … 3). `v` may be kPending: then its two readers go to `pins`.
PortRef zoneGain(Build& b, ChainId chain, char tag, uint32_t base, PortRef v, std::vector<PortRef>* pins) {
  auto id = [&](uint32_t k) { return nodeIdFor(NodeRole::Chain, chain.value, base + k); };
  auto param = [&](uint32_t i, double def) {
    return b.ir.param(chainParamName(chain, std::string(1, tag) + std::to_string(i)), def, -1e9, 1e9, id(i));
  };
  const PortRef a1 = param(0, 0.0), b1 = param(1, 2.0), a2 = param(2, 0.0), b2 = param(3, 2.0);
  const PortRef m1 = b.ir.atom("mul", {v, a1}, id(4)), m2 = b.ir.atom("mul", {v, a2}, id(6));
  if (v.node == graph::kNone && pins != nullptr) {
    pins->push_back(PortRef{m1.node, 0});
    pins->push_back(PortRef{m2.node, 0});
  }
  const PortRef u = b.ir.atom("add", {m1, b1}, id(5)), d = b.ir.atom("add", {m2, b2}, id(7));
  return b.ir.atom("clip", {b.ir.atom("min", {u, d}, id(8)), b.c0(), b.c1()}, id(9));
}

// The chain mixer: balance pan · volume as two runtime params the document derives (c<chain>_gl, _gr), times the
// select zone's gain when there is one; a `gain` stage (block rate, smoothed only while it moves): settled = one
// multiply per sample per channel. The result is stereo.
PortRef chainMixer(Build& b, ChainId chain, PortRef y, PortRef select) {
  auto id = [&](uint32_t k) { return nodeIdFor(NodeRole::Chain, chain.value, k); };
  PortRef gl = b.ir.param(chainParamName(chain, "gl"), 1.0, 0.0, 4.0, id(kCGl));
  PortRef gr = b.ir.param(chainParamName(chain, "gr"), 1.0, 0.0, 4.0, id(kCGr));
  if (select.node != graph::kNone) {
    gl = b.ir.atom("mul", {gl, select}, id(kCGlSel));
    gr = b.ir.atom("mul", {gr, select}, id(kCGrSel));
  }
  return b.ir.atom("gain", {y, b.ir.atom("pack", {gl, gr}, id(kCPack), 2), b.time()}, id(kCGain));
}

// The select zone of a chain, reading its rack's selector (a pending reader), or kPending when the zone is off.
PortRef selectGain(Build& b, uint32_t rackDev, const Chain& c) {
  if (zoneOff(c.select, ZoneKind::Select, KeyZoneUnit::Hz)) return kPending;
  return zoneGain(b, c.id, 's', kCZoneSelect, kPending, &b.rackPins[{rackDev, kMacroCount}]);
}

// One audio effect of a chain (a call), wired after `x` (declared width `width`).
PortRef effectCall(Build& b, uint32_t k, PortRef x, uint16_t& width) {
  const LDev& d = b.td[k];
  // The first non-const `in` (its port index = its ordinal among the callee's `in`s) and the first `out`.
  uint32_t inPort = 0, ordinal = 0;
  uint16_t inWidth = 0, outW = 0;
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
      outW = d.callee->inputType(v, 0).width;
    }
  }
  const uint32_t id = d.device->id.value;
  if (width == graph::kWidthMono && inWidth == graph::kWidthStereo)  // a mono signal into a stereo input
    x = b.ir.atom("pack", {x, x}, nodeIdFor(NodeRole::Device, id, 1), 2);
  const graph::NodeIndex c = callDevice(b.ir, d);
  b.calls[k] = c;
  b.ir.patch().connect(x, PortRef{c, static_cast<uint16_t>(inPort)});
  x = PortRef{c, 0};
  if (outW != graph::kWidthInfer) width = outW;
  else if (inWidth == graph::kWidthStereo) width = graph::kWidthStereo;
  return x;
}

// The devices of a list from `first` on, wired left → right after `x` (declared width `width`): audio effects (calls)
// and Audio Effect Racks (lower.h "Racks": the rack's input → each chain (through its select zone's gain) → the chain
// mixer → summed; no chain at all: the input passes; every chain muted: silence). M4 review M9: iterative (a stack of
// the open lists and racks), the same nodes in the same order as the recursive version.
PortRef chainOf(Build& b, std::span<const uint32_t> list, size_t first, PortRef x, uint16_t& width) {
  struct Open {
    std::span<const uint32_t> list;  // a device list (rack ≥ 0: a rack, running its chains)
    size_t next = 0;
    PortRef x = kPending;            // list: the signal so far; rack: its input
    uint16_t width = 0;              // list: its declared width; rack: its input's
    int64_t rack = -1;
    PortRef acc = kPending;          // rack: the chains' sum so far
    ChainId chain;                   // rack: the chain whose list runs
    bool awaiting = false;
  };
  std::vector<Open> stack{Open{list, first, x, width}};
  PortRef ret = kPending;
  uint16_t retWidth = 0;
  while (!stack.empty()) {
    if (stack.back().rack < 0) {
      Open& f = stack.back();
      if (f.next == f.list.size()) {
        ret = f.x;
        retWidth = f.width;
        stack.pop_back();
        continue;
      }
      const uint32_t k = f.list[f.next++];
      const LDev& d = b.td[k];
      if (d.rack < 0) {
        f.x = effectCall(b, k, f.x, f.width);
        continue;
      }
      const LRack& r = b.td.racks[static_cast<size_t>(d.rack)];
      if (r.passThrough) continue;
      if (r.chains.empty()) {
        f.width = graph::kWidthMono;
        f.x = b.c0();
        continue;
      }
      Open o;
      o.rack = k;
      o.x = f.x;
      o.width = f.width;
      stack.push_back(o);  // invalidates f
      continue;
    }
    Open& f = stack.back();
    const auto k = static_cast<uint32_t>(f.rack);
    const LRack& r = b.td.racks[static_cast<size_t>(b.td[k].rack)];
    if (f.awaiting) {  // a chain's devices ran: its mixer, then the sum
      const ChainId cid = f.chain;
      const PortRef y = chainMixer(b, cid, ret, kPending);
      f.acc = f.acc.node == graph::kNone ? y : b.ir.atom("add", {f.acc, y}, nodeIdFor(NodeRole::Chain, cid.value, kCSum));
      f.awaiting = false;
    }
    if (f.next < r.chains.size()) {
      const LChain& lc = r.chains[f.next++];
      const ChainId cid = lc.chain->id;
      PortRef y = f.x;
      const PortRef sel = selectGain(b, k, *lc.chain);
      // The zone gates the chain's input: a chain the selector leaves hears exact silence once faded, its tail rings out.
      if (sel.node != graph::kNone) y = b.ir.atom("gain", {f.x, sel, b.time()}, nodeIdFor(NodeRole::Chain, cid.value, kCInGain));
      f.chain = cid;
      f.awaiting = true;
      const uint16_t w = f.width;
      stack.push_back(Open{std::span<const uint32_t>(lc.devs), 0, y, w});  // invalidates f
      continue;
    }
    const PortRef acc = f.acc;
    stack.pop_back();
    stack.back().x = acc;  // the list holding the rack goes on from the sum, stereo
    stack.back().width = graph::kWidthStereo;
  }
  width = retWidth;
  return ret;
}

// An Audio Effect Rack on its own (the rack a device view shows): chainOf over just that rack.
PortRef audioRack(Build& b, uint32_t k, PortRef x, uint16_t& width) {
  const uint32_t one[1] = {k};
  return chainOf(b, std::span<const uint32_t>(one), 0, x, width);
}

struct Voices {
  PortRef x = kPending;        // global (voices summed, chain effects, mixers)
  uint16_t width = graph::kWidthStereo;
  PortRef done = kPending;     // per voice: every chain's instrument is done (kPending: none to wait for)
};

// The width of a callee's first `out`.
uint16_t outWidth(const graph::Patch& callee) noexcept {
  for (graph::NodeIndex v = 0; v < callee.nodeCount(); ++v)
    if (callee.node(v).kind == graph::NodeKind::Output) return callee.inputType(v, 0).width;
  return graph::kWidthInfer;
}

// An Instrument Rack (lower.h "Racks"): per chain, its instrument's voices × its key and velocity zones' gains (× the
// outer rack's, nested) → summed over the voices (a per-voice chain: after its effects) → its effects → the chain mixer
// (× the select zone) → summed. `outer`: the per-voice gain of an enclosing rack's zones (kPending: none). M4 review
// M9: nested Instrument Racks run on an explicit stack (the same nodes in the same order as the recursive version).
Voices instrumentRack(Build& b, uint32_t k0, PortRef outer0) {
  struct Open {
    uint32_t k = 0;
    PortRef outer = kPending;
    size_t next = 0;
    Voices out;
    std::vector<std::pair<PortRef, ChainId>> dones;
    bool allDone = true;
    bool awaiting = false;  // a nested rack (the current chain's first device) runs; its Voices come back in `ret`
  };
  std::vector<Open> stack;
  stack.push_back(Open{k0, outer0});
  Voices ret;
  while (!stack.empty()) {
    Open& f = stack.back();
    const Rack& rack = *b.td[f.k].device->rack;
    const LRack& r = b.td.racks[static_cast<size_t>(b.td[f.k].rack)];
    PortRef y = kPending;
    uint16_t w = graph::kWidthStereo;
    ChainId cid;
    if (f.awaiting) {  // the nested rack of chain next − 1 ran
      const LChain& lc = r.chains[f.next - 1];
      cid = lc.chain->id;
      y = ret.x;
      w = ret.width;
      if (ret.done.node != graph::kNone) f.dones.emplace_back(ret.done, cid);
      else f.allDone = false;
      y = chainOf(b, lc.devs, 1, y, w);
      f.awaiting = false;
    } else if (f.next < r.chains.size()) {
      const LChain& lc = r.chains[f.next++];
      cid = lc.chain->id;
      auto id = [&](uint32_t s) { return nodeIdFor(NodeRole::Chain, cid.value, s); };
      PortRef g = f.outer;
      auto times = [&](PortRef z, uint32_t slot) { g = g.node == graph::kNone ? z : b.ir.atom("mul", {g, z}, id(slot)); };
      if (!zoneOff(lc.chain->key, ZoneKind::Key, rack.keyUnit)) {
        // T4.17 + M4 review M7: decided at note-on (param rate, held: glides, Pitch events and retunes do not move a
        // sounding note between chains). Hz zones read ln(voice.noteHz), the note's pitch as resolved in its own
        // tuning (a clip's tuning included); Degree zones read voice.noteDegree, its degree in that tuning (NaN, a
        // note from outside the document: degree 0).
        if (rack.keyUnit == KeyZoneUnit::Degree) {
          if (b.degree.node == graph::kNone) {
            const PortRef d = b.ir.atom("voice.noteDegree", {}, kZoneDegree);
            b.degree = b.ir.atom("select", {b.ir.atom("eq", {d, d}, kZoneDegreeOk), d, b.c0()}, kZoneDegreeSafe);
          }
          times(zoneGain(b, cid, 'k', kCZoneKey, b.degree, nullptr), kCOuterMul);
        } else {
          if (b.lnHz.node == graph::kNone) b.lnHz = b.ir.atom("log", {b.ir.atom("voice.noteHz", {}, kZoneHz)}, kZoneLnHz);
          times(zoneGain(b, cid, 'k', kCZoneKey, b.lnHz, nullptr), kCOuterMul);
        }
      }
      if (!zoneOff(lc.chain->velocity, ZoneKind::Velocity, rack.keyUnit)) {
        if (b.vel.node == graph::kNone) b.vel = b.ir.atom("voice.velocity", {}, kZoneVelocity);
        times(zoneGain(b, cid, 'v', kCZoneVelocity, b.vel, nullptr), kCZoneMul);
      }
      const uint32_t first = lc.devs.front();
      const LDev& fd = b.td[first];
      if (fd.rack >= 0) {  // a nested Instrument Rack: its own chains take this chain's zones too
        f.awaiting = true;
        stack.push_back(Open{first, g});  // invalidates f
        continue;
      }
      const NodeIndex call = callDevice(b.ir, fd);
      b.calls[first] = call;
      y = PortRef{call, 0};
      w = outWidth(*fd.callee);
      if (w == graph::kWidthInfer) w = graph::kWidthMono;
      if (const auto done = b.ir.patch().findOutput(call, "done")) f.dones.emplace_back(PortRef{call, static_cast<uint16_t>(*done)}, cid);
      else f.allDone = false;
      // Through a smoothed gain stage per voice: a zone edit while notes sound fades over ~2 ms (key and velocity zones
      // are decided at note-on, block rate per voice, T4.17); `start` 1: a note starts at its zone's gain (its lane is
      // reset at the note-on), not faded in from the lane's last note.
      if (g.node != graph::kNone) y = b.ir.atom("gain", {y, g, b.time(), b.c1()}, id(kCVoiceGain));
      if (rack.voiceChains) {  // SPEC §8.2 voicechain: the chain's effects per voice, then the sum
        y = chainOf(b, lc.devs, 1, y, w);
        y = b.ir.atom("voice_sum", {y}, id(kCVoiceSum));
      } else {
        y = b.ir.atom("voice_sum", {y}, id(kCVoiceSum));
        y = chainOf(b, lc.devs, 1, y, w);
      }
    } else {
      // A voice ends when every chain's instrument is done (a chain without a `done` keeps it to the engine's own end).
      if (f.allDone && !f.dones.empty()) {
        f.out.done = f.dones.front().first;
        for (size_t i = 1; i < f.dones.size(); ++i)
          f.out.done = b.ir.atom("and", {f.out.done, f.dones[i].first}, nodeIdFor(NodeRole::Chain, f.dones[i].second.value, kCDone));
      }
      ret = f.out;
      stack.pop_back();
      continue;
    }
    // The chain's mixer (× its select zone), then the sum.
    const LChain& lc = r.chains[f.next - 1];
    y = chainMixer(b, cid, y, selectGain(b, f.k, *lc.chain));
    f.out.x = f.out.x.node == graph::kNone ? y : b.ir.atom("add", {f.out.x, y}, nodeIdFor(NodeRole::Chain, cid.value, kCSum));
  }
  return ret;
}

// ---- Modulation wires and macro mappings in a track patch (T4.7, T4.8; lower.h "Modulation wires", "Racks") ---------

// NodeRole::Wire slots: a wire's own nodes, then (from kWTarget) the nodes of the param it modulates.
enum WireK : uint32_t { kWAmount, kWIn, kWExport, kWChannel, kWReduce, kWDelay, kWTerm, kWSum, kWTarget = 16 };
enum TargetK : uint32_t {
  kTKnob, kTPosA, kTPosB, kTPosC, kTAdd, kTClip, kTMapA, kTMapB, kTMapC, kTRoundAdd, kTFloor,
  kTUpSub, kTUpMax, kTDownSub, kTDownMax, kTPack, kTExport,
};

std::string_view reducerAtom(WireReducer r) noexcept {
  switch (r) {
    case WireReducer::Sum: return "voice_sum";
    case WireReducer::Last: return "voice_last";
    case WireReducer::Highest: return "voice_highest";
    case WireReducer::Lowest: return "voice_lowest";
    case WireReducer::Loudest: return "voice_loudest";
  }
  return "voice_sum";
}

std::string tapPort(DeviceId device, std::string_view param) {
  return "tap_d" + std::to_string(device.value) + "_" + std::string(param);
}

// A param whose value the track patch builds (instead of leaving the call's pin unconnected): a modulated param
// (T4.7), a macro-mapped param, a rack param read by its rack (T4.8).
struct Site {
  uint32_t dev = 0;
  std::string name;
  const graph::ParamDecl* decl = nullptr;
  std::vector<PortRef> pins;            // the inputs it feeds
  std::vector<const LWire*> wires;      // modulation into it, in id order
  const LMapping* mapping = nullptr;    // the macro driving it
  bool resolved = false;
  PortRef value = kPending;
};

// The wires of plan node `node` (its outgoing cross-track wires: root exports m<wire>), then every param the patch
// builds (modulated, mapped, read by a rack), each connected to its pins. Each modulated param's tap goes to `taps`
// (graph track `track`).
void lowerSites(Build& b, uint32_t node, uint32_t track, const WirePlan& wp, std::vector<ModulationTap>* taps) {
  Ir& ir = b.ir;
  const TrackDevs& td = b.td;
  auto wid = [](const LWire& w, uint32_t k) { return nodeIdFor(NodeRole::Wire, w.wire->id.value, k); };
  // The source of a wire in this patch: its export's first channel, reduced over the voices when it leaves them.
  auto source = [&](const LWire& w) {
    PortRef s = ir.atom("channel", {PortRef{b.calls[w.fromDev], w.fromPort}, b.c0()}, wid(w, kWChannel));
    if (w.reduce) s = ir.atom(reducerAtom(w.wire->reducer), {s}, wid(w, kWReduce));
    return s;
  };
  for (const LWire& w : wp.wires)
    if (w.fromNode == node && w.toNode != node) ir.exportPort("m" + std::to_string(w.wire->id.value), source(w), wid(w, kWExport));
  // The sites: the modulated params first, in the order of their first wire (T4.7), then mapped params and rack params.
  std::vector<Site> sites;
  auto siteOf = [&](uint32_t dev, std::string_view name) -> Site& {
    for (Site& s : sites)
      if (s.dev == dev && s.name == name) return s;
    Site s;
    s.dev = dev;
    s.name = std::string(name);
    const LDev& d = td[dev];
    if (d.rack >= 0) {
      s.decl = &rackParamDecl();
      if (const auto it = b.rackPins.find({dev, rackParamIndex(name)}); it != b.rackPins.end()) s.pins = it->second;
    } else {
      const ParamPin pp = *realParamPin(*d.callee, name);
      s.decl = &d.callee->paramDecl(pp.node);
      s.pins.push_back(PortRef{b.calls[dev], pp.pin});
    }
    s.mapping = td.mappingOf(dev, name);
    sites.push_back(std::move(s));
    return sites.back();
  };
  sites.reserve(wp.wires.size() + td.mappings.size() + b.rackPins.size());
  for (const LWire& w : wp.wires) {
    if (w.toNode != node) continue;
    const LDev& d = td[w.toDev];
    siteOf(w.toDev, d.rack >= 0 ? rackParamName(w.toPin) : d.callee->nodeName(w.paramNode)).wires.push_back(&w);
  }
  for (const LMapping& m : td.mappings) {
    (void)siteOf(m.rack, macroParamName(m.m->macro));  // the macro it reads (before the target: no reallocation later)
    (void)siteOf(m.target, m.m->param);
  }
  for (const auto& [key, pins] : b.rackPins) (void)siteOf(key.first, rackParamName(key.second));
  // The site of the macro a site's mapping reads (sites.size(): none).
  auto macroSite = [&](size_t si) {
    const LMapping* mapping = sites[si].mapping;
    if (mapping == nullptr) return sites.size();
    size_t mi = sites.size();
    for (size_t i = 0; i < sites.size(); ++i)
      if (sites[i].dev == mapping->rack && sites[i].name == macroParamName(mapping->m->macro)) mi = i;
    assert(mi < sites.size() && "the mapping's macro has a site (lowerSites adds it)");
    return mi;
  };
  // A site's value, once the site of its mapping's macro has one (valueOf below orders that).
  auto compute = [&](size_t si) -> PortRef {
    if (sites[si].resolved) return sites[si].value;
    const uint32_t dev = sites[si].dev;
    const std::string name = sites[si].name;
    const graph::ParamDecl& decl = *sites[si].decl;
    const LMapping* mapping = sites[si].mapping;
    const std::vector<const LWire*> wires = sites[si].wires;
    const LDev& d = td[dev];
    const ParamTravel tr = d.rack >= 0 ? ParamTravel{false, 0.0, kMacroMax, false} : paramTravel(decl);
    const bool travels = tr.hi > tr.lo && (!tr.log || tr.lo > 0.0);
    // The mapped position: min + (max − min) · shape(macro / 127, curve) (the automation curve law).
    PortRef pos = kPending;
    if (mapping != nullptr) {
      const size_t mi = macroSite(si);
      assert(sites[mi].resolved && "valueOf computes the macro's site first");
      const PortRef macro = sites[mi].value;
      const MacroMapping& m = *mapping->m;
      auto mid = [&](uint32_t s) { return nodeIdFor(NodeRole::Mapping, m.id.value, s); };
      PortRef s = ir.atom("mul", {macro, ir.konst(1.0 / kMacroMax, mid(kMInv127))}, mid(kMX));
      const double k = kAutomationCurveK * m.curve;
      if (std::abs(k) >= kAutomationLinearK) {
        const PortRef e = ir.atom("exp", {ir.atom("mul", {s, ir.konst(k, mid(kMK))}, mid(kMKx))}, mid(kMExp));
        s = ir.atom("mul", {ir.atom("sub", {e, ir.konst(1.0, mid(kMOne))}, mid(kMSub)), ir.konst(1.0 / std::expm1(k), mid(kMInvE))},
                    mid(kMShape));
      }
      pos = ir.atom("add", {ir.atom("mul", {s, ir.konst(m.max - m.min, mid(kMRange))}, mid(kMScaled)), ir.konst(m.min, mid(kMMin))},
                    mid(kMPos));
    }
    PortRef v;
    if (!wires.empty()) {
      // T4.7: the knob (or the mapped position) is the centre; Σ amount · source in knob travel; clip; the travel map.
      const LWire& f = *wires.front();
      auto tid = [&](uint32_t k) { return wid(f, kWTarget + k); };
      const std::string devId = std::to_string(d.device->id.value);
      PortRef knob = kPending, pk = pos;
      if (mapping == nullptr) knob = ir.paramDecl("d" + devId + "__" + name, decl, tid(kTKnob));
      if (!travels) {  // no travel to move along: the knob (or the mapping's low end) as before
        v = mapping == nullptr ? knob : ir.konst(tr.lo, tid(kTMapA));
      } else {
        const double L = tr.log ? std::log(tr.hi / tr.lo) : tr.hi - tr.lo;
        if (mapping == nullptr)  // the knob's position (param rate: the compiler hoists it to once per block)
          pk = tr.log ? ir.atom("mul", {ir.atom("log", {ir.atom("mul", {knob, ir.literal(1.0 / tr.lo)}, tid(kTPosA))}, tid(kTPosB)),
                                        ir.literal(1.0 / L)},
                                tid(kTPosC))
                      : ir.atom("mul", {ir.atom("sub", {knob, ir.literal(tr.lo)}, tid(kTPosA)), ir.literal(1.0 / L)}, tid(kTPosB));
        PortRef sum;
        for (size_t i = 0; i < wires.size(); ++i) {
          const LWire& w = *wires[i];
          const std::string id = std::to_string(w.wire->id.value);
          constexpr graph::PortType kAnyGlobal = graph::portType(graph::Kind::Real, graph::Rate::Infer, graph::Poly::Global, graph::kWidthMono);
          PortRef src = w.fromNode == node ? source(w) : ir.input("m" + id, b.c0(), wid(w, kWIn), kAnyGlobal);
          if (w.delayed) src = ir.atom("z1", {src}, wid(w, kWDelay));  // closes a loop: 1 sample (SPEC §15 2026-10-24)
          const PortRef amt = ir.param("w" + id, w.wire->amount, -1.0, 1.0, wid(w, kWAmount));
          const PortRef term = ir.atom("mul", {amt, src}, wid(w, kWTerm));
          sum = i == 0 ? term : ir.atom("add", {sum, term}, wid(w, kWSum));
        }
        const PortRef p = ir.atom("clip", {ir.atom("add", {pk, sum}, tid(kTAdd)), b.c0(), b.c1()}, tid(kTClip));
        v = tr.log ? ir.atom("exp", {ir.atom("add", {ir.atom("mul", {p, ir.literal(L)}, tid(kTMapA)), ir.literal(std::log(tr.lo))},
                                             tid(kTMapB))},
                             tid(kTMapC))
                   : ir.atom("add", {ir.atom("mul", {p, ir.literal(L)}, tid(kTMapA)), ir.literal(tr.lo)}, tid(kTMapB));
        if (tr.round) v = ir.atom("floor", {ir.atom("add", {v, ir.konst(0.5, kCHalf)}, tid(kTRoundAdd))}, tid(kTFloor));
        // The tap: how far up and down the modulation moved the param (zero exactly when it does not move it).
        const PortRef up = ir.atom("max", {ir.atom("sub", {p, pk}, tid(kTUpSub)), b.c0()}, tid(kTUpMax));
        const PortRef down = ir.atom("max", {ir.atom("sub", {pk, p}, tid(kTDownSub)), b.c0()}, tid(kTDownMax));
        const std::string port = tapPort(d.device->id, name);
        ir.exportPort(port, ir.atom("pack", {up, down}, tid(kTPack), 2), tid(kTExport));
        if (taps != nullptr) taps->push_back(ModulationTap{d.device->id, name, track, port});
      }
    } else if (mapping != nullptr) {
      // The mapped value: the travel map of the position.
      const MacroMapping& m = *mapping->m;
      auto mid = [&](uint32_t s) { return nodeIdFor(NodeRole::Mapping, m.id.value, s); };
      if (!travels) {
        v = ir.konst(tr.lo, mid(kMLo));
      } else {
        const double L = tr.log ? std::log(tr.hi / tr.lo) : tr.hi - tr.lo;
        const PortRef scaled = ir.atom("mul", {pos, ir.konst(L, mid(kML))}, mid(kMMulL));
        v = ir.atom("add", {scaled, ir.konst(tr.log ? std::log(tr.lo) : tr.lo, mid(kMLo))}, mid(kMAddLo));
        if (tr.log) v = ir.atom("exp", {v}, mid(kMExpV));
        if (tr.round) v = ir.atom("floor", {ir.atom("add", {v, ir.konst(0.5, kCHalf)}, mid(kMRoundAdd))}, mid(kMFloor));
      }
    } else {
      // A rack param its rack reads: the knob, a runtime param of the track patch (dev<rack>:<name>).
      const uint32_t pi = rackParamIndex(name);
      v = ir.paramDecl("d" + std::to_string(d.device->id.value) + "__" + name, decl,
                       nodeIdFor(NodeRole::Rack, d.device->id.value, pi));
    }
    sites[si].resolved = true;
    sites[si].value = v;
    return v;
  };
  // A site's value: the mapping's macro first (macros map only inside their rack, so the chain ends). M4 review M9:
  // the chain of macros (one per rack level at most) is followed iteratively, then computed innermost first (the
  // same nodes in the same order as the recursive version).
  std::vector<size_t> pendingSites;
  auto valueOf = [&](size_t si) -> PortRef {
    pendingSites.assign(1, si);
    for (size_t at = si; !sites[at].resolved;) {
      const size_t mi = macroSite(at);
      if (mi == sites.size() || sites[mi].resolved) break;
      pendingSites.push_back(mi);
      at = mi;
    }
    for (size_t i = pendingSites.size(); i-- > 0;) (void)compute(pendingSites[i]);
    return sites[si].value;
  };
  for (size_t i = 0; i < sites.size(); ++i) {
    const PortRef v = valueOf(i);
    for (const PortRef& pin : sites[i].pins) ir.patch().connect(v, pin);
  }
}

// Whether `dev`.`name` is a param the patch declares as a runtime param of its own (dev<id>:<name>): a device param
// that is not mapped (unconnected at the call, or a modulated param's knob), a rack param that is read or modulated and
// not mapped. The same rule as lowerSites, so addParams links exactly the params the patch has.
bool declaresParam(const TrackDevs& td, uint32_t dev, std::string_view name, const WirePlan& wp, uint32_t node) {
  if (td.mappingOf(dev, name) != nullptr) return false;
  if (td[dev].rack < 0) return true;
  const uint32_t pi = rackParamIndex(name);
  if (td.rackParamConsumed(dev, pi)) return true;
  for (const LWire& w : wp.wires)
    if (w.toNode == node && w.toDev == dev && w.toPin == pi) return true;
  return false;
}

Result<graph::Patch> buildPatch(const Plan& pl, uint32_t index, const LowerOptions& o, const TrackDevs& devs,
                                const WirePlan& wp, bool& voices, uint32_t& polyphony, uint32_t track,
                                std::vector<ModulationTap>* taps, uint32_t clipSlot) {
  const Node& n = pl.nodes[index];
  Ir ir(patchName(n));
  Build b{ir, o, devs, std::vector<NodeIndex>(devs.size(), graph::kNone), {}};
  PortRef x;
  const bool instrument = n.kind == Kind::Midi && devs.voices;
  voices = instrument;
  // T4.12-fix1: the instrument's `voices` setting; T4.8: an Instrument Rack's largest (its chains share the voices).
  polyphony = 0;
  if (instrument)
    for (uint32_t k = 0; k < devs.size(); ++k)
      if (devs[k].rack < 0 && devs[k].device->kind == DeviceKind::Instrument) polyphony = std::max(polyphony, devicePolyphony(devs[k]));
  size_t firstEffect = 0;
  uint16_t width = graph::kWidthStereo;
  if (n.kind == Kind::Audio) {
    // T5.3: the clip player of slot `clipSlot` (stereo; the player duplicates a mono source), then the chain. Without a
    // slot nothing plays (its effects still run).
    if (clipSlot != graph::kNone) {
      x = ir.atom("clipPlayer", {ir.konst(static_cast<double>(clipSlot), kClipSlot)}, kClipPlayer);
    } else {
      x = ir.konst(0.0, kC0);
      width = graph::kWidthMono;
    }
  } else if (n.kind == Kind::Midi && !instrument) {
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
    const uint32_t first = devs.top.front();
    if (devs[first].rack >= 0) {
      // T4.8: an Instrument Rack: its chains' voices, summed per chain.
      const Voices v = instrumentRack(b, first, kPending);
      x = v.x;
      width = v.width;
      if (v.done.node != graph::kNone)
        ir.output("done", graph::portType(graph::Kind::Bool, graph::Rate::Infer, graph::Poly::Voice, 1), v.done, kDone);
    } else {
      // The instrument (T4.1: the chain's first device; its first `out` is the voice signal, `done` ends voices).
      const NodeIndex syn = callDevice(ir, devs[first]);
      b.calls[first] = syn;
      x = ir.atom("voice_sum", {PortRef{syn, 0}}, kVoiceSum);
      if (const auto done = ir.patch().findOutput(syn, "done"))
        ir.output("done", graph::portType(graph::Kind::Bool, graph::Rate::Infer, graph::Poly::Voice, 1),
                  PortRef{syn, static_cast<uint16_t>(*done)}, kDone);
      width = graph::kWidthMono;
      if (outWidth(*devs[first].callee) == graph::kWidthStereo) width = graph::kWidthStereo;
    }
    firstEffect = 1;
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
  x = chainOf(b, devs.top, firstEffect, x, width);
  gainStages(ir, n.kind, x, o.smoothingMs);
  lowerSites(b, index, track, wp, taps);  // T4.7, T4.8
  if (ir.literalOverflow())
    return Error{ErrorCode::Overflow, "lower: track " + std::to_string(n.track->id.value) + ": more than " +
                                          std::to_string(kEntityNodeBase - kLiteralPool) + " device settings", {}};
  if (!ir.finish())
    return Error{ErrorCode::Overflow, "lower: track " + std::to_string(n.track->id.value) +
                                          ": a document id too large for a stable node id (nodeIdFor)", {}};
  return std::move(ir.patch());
}

// ---- Rack chain params (T4.8): what the document derives for the lowered patch -------------------------------------

constexpr double kZoneEps = 1e-3;     // select / velocity units: a bound is inclusive
constexpr double kZoneEpsLog = 1e-4;  // key zones, in ln(Hz): ≈ 0.17 ¢

// a1, b1, a2, b2 of zoneGain for a zone over values v (the zone's bounds already in v's domain; `min` / `max`: the
// domain's ends, where a bound opens the zone on that side).
std::array<double, 4> zoneCoefficients(double lo, double fadeLo, double fadeHi, double hi, bool openLo, bool openHi,
                                       double eps) {
  std::array<double, 4> c{0.0, 2.0, 0.0, 2.0};
  if (!openLo) {
    c[0] = 1.0 / (fadeLo - lo + eps);
    c[1] = (eps - lo) * c[0];
  }
  if (!openHi) {
    const double inv = 1.0 / (hi - fadeHi + eps);
    c[2] = -inv;
    c[3] = (hi + eps) * inv;
  }
  return c;
}

// The derived params of one lowered chain: its mixer (gl, gr) and its zones that are on (as zoneGain names them).
void chainParams(const Rack& rack, const Chain& c, bool instrumentRack, uint32_t index, std::vector<LoweredParam>& out) {
  const std::string key = "chain" + std::to_string(c.id.value) + ":";
  auto add = [&](std::string_view what, double v, uint32_t ref) {
    out.push_back(LoweredParam{key + std::string(what), index, chainParamName(c.id, what), static_cast<float>(v),
                               ParamRef::chain(c.id, ref)});
  };
  const double vg = c.volumeDb <= kMinVolumeDb ? 0.0 : std::pow(10.0, static_cast<double>(c.volumeDb) / 20.0);
  const double pan = std::clamp(static_cast<double>(c.pan), -1.0, 1.0);
  add("gl", vg * std::min(1.0, 1.0 - pan), 0);
  add("gr", vg * std::min(1.0, 1.0 + pan), 0);
  auto zone = [&](ZoneKind k, char tag) {
    const Zone& z = c.zone(k);
    if (zoneOff(z, k, rack.keyUnit)) return;
    const ZoneRange r = zoneRange(k, rack.keyUnit);
    std::array<double, 4> co{};
    if (k == ZoneKind::Key && rack.keyUnit == KeyZoneUnit::Hz) {  // in ln(Hz)
      auto ln = [&](double v) { return std::log(std::max(v, 1e-3)); };
      co = zoneCoefficients(ln(z.lo), ln(z.fadeLo), ln(z.fadeHi), ln(z.hi), z.lo <= r.min, z.hi >= r.max, kZoneEpsLog);
    } else {  // velocity, select, and Degree key zones (T4.17, M7: in the note's own degrees, voice.noteDegree)
      co = zoneCoefficients(z.lo, z.fadeLo, z.fadeHi, z.hi, z.lo <= r.min, z.hi >= r.max, kZoneEps);
      if (k == ZoneKind::Velocity) {  // the graph reads voice.velocity in 0 … 1
        co[0] *= kVelocityMax;
        co[2] *= kVelocityMax;
      }
    }
    for (uint32_t i = 0; i < 4; ++i) add(std::string(1, tag) + std::to_string(i), co[i], 1 + static_cast<uint32_t>(k));
  };
  if (instrumentRack) {
    zone(ZoneKind::Key, 'k');
    zone(ZoneKind::Velocity, 'v');
  }
  zone(ZoneKind::Select, 's');
}

void addParams(const Plan& pl, uint32_t index, const AudibilityMap& aud, std::vector<LoweredParam>& out,
               const Callees* lib, const WirePlan& wp, const TrackDevs& td) {
  const Node& n = pl.nodes[index];
  const Track& t = *n.track;
  const std::string id = std::to_string(t.id.value);
  // T4.7: the amount of every wire into this track (a param of the target's patch).
  for (const LWire& w : wp.wires)
    if (w.toNode == index) {
      const std::string wid = std::to_string(w.wire->id.value);
      out.push_back(LoweredParam{"wire" + wid + ":amount", index, "w" + wid, static_cast<float>(w.wire->amount),
                                 ParamRef::wireAmount(w.wire->id)});
    }
  // T4.1: every param of every lowered device (unconnected at the call: d<id>__<name> in the track patch); T4.8: but
  // not a mapped one (the macro drives its pin), and a rack's params when the patch declares them; each lowered chain's
  // derived params.
  if (lib != nullptr) {
    for (uint32_t k = 0; k < td.size(); ++k) {
      const LDev& d = td[k];
      const std::string dev = std::to_string(d.device->id.value);
      if (d.rack >= 0) {
        for (uint32_t i = 0; i < kRackParams; ++i) {
          const std::string name = rackParamName(i);
          if (!declaresParam(td, k, name, wp, index)) continue;
          out.push_back(LoweredParam{"dev" + dev + ":" + name, index, "d" + dev + "__" + name,
                                     static_cast<float>(rackParamValue(*d.device, name)), ParamRef::device(d.device->id, name)});
        }
        const Rack& rack = *d.device->rack;
        const bool instrument = d.device->kind == DeviceKind::Instrument;
        for (const LChain& c : td.racks[static_cast<size_t>(d.rack)].chains)
          chainParams(rack, *c.chain, instrument, index, out);
        continue;
      }
      for (graph::NodeIndex v = 0; v < d.callee->nodeCount(); ++v) {
        if (d.callee->node(v).kind != graph::NodeKind::Param) continue;
        const std::string& name = d.callee->nodeName(v);
        if (!declaresParam(td, k, name, wp, index)) continue;
        const DeviceValue* set = d.device->param(name);
        const double value = set != nullptr ? set->value : d.callee->paramDecl(v).defaultValue.value();
        out.push_back(LoweredParam{"dev" + dev + ":" + name, index, "d" + dev + "__" + name, static_cast<float>(value),
                                   ParamRef::device(d.device->id, name)});
      }
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
    case Kind::Audio: return graph::TrackRole::Audio;
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
  uint32_t r = static_cast<uint32_t>(role);
  if (role == NodeRole::Wire || role == NodeRole::Chain || role == NodeRole::Mapping) {
    // a wire / chain / mapping id is no other entity's id: its slots span the three role spaces
    if (k >= kWireNodeSlots) return 0u;
    r = k / kEntityNodeSlots;
    k %= kEntityNodeSlots;
  } else if (role == NodeRole::Rack) {  // a rack's id is a device id: the TuningNode and SendInput spaces of it
    if (k >= 2 * kEntityNodeSlots) return 0u;
    r = k / kEntityNodeSlots;
    k %= kEntityNodeSlots;
  }
  const uint64_t id = kEntityNodeBase + (static_cast<uint64_t>(entity) * 3 + r) * kEntityNodeSlots + k;
  return id <= 0xFFFFFFFFull && k < kEntityNodeSlots ? static_cast<uint32_t>(id) : 0u;
}

ParamTravel paramTravel(const graph::ParamDecl& d) noexcept {
  ParamTravel t;
  graph::Unit unit = d.defaultValue.unit;
  bool exp = false;
  for (const graph::Attr& a : d.attrs) {
    if (a.args.empty()) continue;
    if (a.name == "unit") {
      graph::Unit u = graph::Unit::None;
      if (graph::unitFromSuffix(a.args[0].value.text, u)) unit = u;
    } else if (a.name == "curve") {
      exp = a.args[0].value.text == "exp";
    } else if (a.name == "steps" && a.args[0].value.type == graph::AttrValue::Type::Number) {
      t.round = std::lround(a.args[0].value.number.value()) > 1;
    }
  }
  const double def = d.defaultValue.value();
  if (d.hasRange) {
    t.lo = d.rangeMin.value();
    t.hi = d.rangeMax.value();
    if (unit == graph::Unit::None && d.defaultValue.unit == graph::Unit::None) unit = d.rangeMax.unit;
  } else {
    t.lo = std::min(0.0, def);
    t.hi = std::max(1.0, def);
  }
  if (t.lo > t.hi) std::swap(t.lo, t.hi);
  t.log = t.lo > 0.0 && (exp || unit == graph::Unit::dB);
  return t;
}

double travelPosition(const ParamTravel& t, double v) noexcept {
  if (!(t.hi > t.lo)) return 0.0;
  v = std::clamp(v, t.lo, t.hi);
  return t.log ? std::log(v / t.lo) / std::log(t.hi / t.lo) : (v - t.lo) / (t.hi - t.lo);
}

double travelValue(const ParamTravel& t, double p) noexcept {
  p = std::clamp(p, 0.0, 1.0);
  double v = t.log ? t.lo * std::exp(p * std::log(t.hi / t.lo)) : t.lo + p * (t.hi - t.lo);
  if (t.round) v = std::floor(v + 0.5);
  return std::clamp(v, t.lo, t.hi);
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
    if (!c.clip || c.clip->tuning.mode != TuningMode::Main) continue;
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
  UserCallees uc = makeCallees(p, o.library, o.devices, true);  // T4.4-fix1
  const Callees* lib = &uc.callees;
  const Plan pl = plan(p, &o.tuningTableOf, lib);
  const AudibilityMap aud = computeAudibility(p, AudibilityOptions{true});
  LoweredProject out;
  out.graph.library = lib->lib;
  out.userLibrary = std::move(uc.combined);
  out.userDevices = std::move(uc.held);
  out.deviceErrors = std::move(uc.errors);
  const std::vector<TrackDevs> devs = allDevices(pl, lib, &out.missingDevices);
  PortDeps deps(lib->lib);
  const WirePlan wp = planWires(p, pl, devs, deps);  // T4.7
  out.wires = wp.states;
  std::unordered_map<uint32_t, std::shared_ptr<const LoweredPatch>> prev;  // track id → its previous patch
  if (previous != nullptr)
    for (size_t j = 0; j < previous->trackIds.size(); ++j) prev.emplace(previous->trackIds[j].value, previous->patches[j]);
  for (uint32_t i = 0; i < pl.nodes.size(); ++i) {
    const Node& n = pl.nodes[i];
    bool voices = false;
    uint32_t polyphony = 0;
    // T5.3: an audio track's clip slot (the engine's audio sequence it plays; LowerOptions::clipSlotOf).
    const uint32_t clipSlot = n.kind == Kind::Audio && o.clipSlotOf ? o.clipSlotOf(n.track->id) : graph::kNone;
    if (n.kind == Kind::Audio && clipSlot == graph::kNone) out.withoutSlot.push_back(n.track->id);
    auto built = buildPatch(pl, i, o, devs[i], wp, voices, polyphony, i, &out.taps, clipSlot);
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
    pt.maxVoices = polyphony;  // T4.12-fix1: the instrument's `voices` setting (0: the link's default)
    if (voices && pt.eventSlot == graph::kNone) out.withoutSlot.push_back(n.track->id);
    out.graph.tracks.push_back(std::move(pt));
    addParams(pl, i, aud, out.params, lib, wp, devs[i]);
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
  // T4.7: cross-track wires (the partition merges their units, unless that would join two instruments: a bus), then
  // the modulation taps (buses the views meter).
  for (const LWire& w : wp.wires)
    if (w.fromNode != w.toNode) {
      const std::string m = "m" + std::to_string(w.wire->id.value);
      out.graph.wires.push_back({{w.fromNode, m}, {w.toNode, m}, W::Modulation});
    }
  for (const ModulationTap& t : out.taps) out.graph.taps.push_back({t.track, t.port});
  out.graph.output = {pl.master, "out"};
  return out;
}

namespace {

std::vector<LoweredParam> paramsWith(const Project& p, bool tuningNodes, const Callees* lib) {
  const TableOf anyTable = [](TuningId, const TuningDef&) { return 0u; };  // the index does not change a param
  const Plan pl = plan(p, tuningNodes ? &anyTable : nullptr, lib);
  const AudibilityMap aud = computeAudibility(p, AudibilityOptions{true});
  const std::vector<TrackDevs> devs = allDevices(pl, lib, nullptr);
  WirePlan wp;
  if (lib != nullptr && !wiresOf(p).empty()) {
    PortDeps deps(lib->lib);
    wp = planWires(p, pl, devs, deps);
  }
  std::vector<LoweredParam> out;
  for (uint32_t i = 0; i < pl.nodes.size(); ++i) addParams(pl, i, aud, out, lib, wp, devs[i]);
  return out;
}

std::vector<WireState> wireStatesWith(const Project& p, const Callees& lib) {
  if (wiresOf(p).empty()) return {};
  const Plan pl = plan(p, nullptr, &lib);
  PortDeps deps(lib.lib);
  return planWires(p, pl, allDevices(pl, &lib, nullptr), deps).states;
}

}  // namespace

std::vector<LoweredParam> lowerParams(const Project& p, bool tuningNodes, const graph::PatchLibrary* library) {
  if (library == nullptr) return paramsWith(p, tuningNodes, nullptr);
  const UserCallees uc = makeCallees(p, library, nullptr, !wiresOf(p).empty());
  return paramsWith(p, tuningNodes, &uc.callees);
}

std::vector<LoweredParam> lowerParams(const Project& p, bool tuningNodes, const DeviceLibrary& devices) {
  const UserCallees uc = makeCallees(p, &devices.library(), &devices, !wiresOf(p).empty());
  return paramsWith(p, tuningNodes, &uc.callees);
}

Result<graph::Patch> rackSubcircuit(const Project& p, DeviceId rackId, const graph::PatchLibrary& library,
                                   const DeviceLibrary* devices) {
  const DeviceLoc loc = findDevice(p, rackId);
  if (loc.device == nullptr || !loc.device->rack) return Error{ErrorCode::NotFound, "rackSubcircuit: no such rack", {}};
  const DevicePtr rack = loc.list()[loc.index];
  const bool instrument = rack->kind == DeviceKind::Instrument;
  // The rack on its own, as the track lowers it (active or not: the view shows what it would play).
  TrackDevs td;
  DevicePtr shown = rack;
  if (!rack->active) {
    auto on = std::make_shared<Device>(*rack);
    on->active = true;
    shown = std::move(on);
  }
  const UserCallees uc = makeCallees(p, &library, devices, true);  // T4.4-fix1: user devices inside it play their text
  lowerList(td, std::vector<DevicePtr>{shown}, instrument, &uc.callees, nullptr, td.top);
  Ir ir((instrument ? "InstrumentRack" : "AudioEffectRack") + std::to_string(rackId.value));
  LowerOptions o;
  o.library = uc.callees.lib;
  o.devices = devices;
  Build b{ir, o, td, std::vector<NodeIndex>(td.size(), graph::kNone), {}};
  if (td.top.empty()) {  // an Instrument Rack with no chain that plays: an empty patch with its output
    ir.output("out", kStereo, ir.konst(0.0, kC0), kOut);
  } else if (instrument) {
    const Voices v = instrumentRack(b, td.top.front(), kPending);
    ir.output("out", kStereo, v.x, kOut);
    if (v.done.node != graph::kNone)
      ir.output("done", graph::portType(graph::Kind::Bool, graph::Rate::Infer, graph::Poly::Voice, 1), v.done, kDone);
  } else {
    uint16_t width = graph::kWidthStereo;
    const PortRef x = audioRack(b, td.top.front(), ir.input("in", ir.konst(0.0, kC0), kIn), width);
    ir.output("out", kStereo, x, kOut);
  }
  lowerSites(b, 0, 0, WirePlan{}, nullptr);
  if (!ir.finish()) return Error{ErrorCode::Overflow, "rackSubcircuit: a document id too large for a stable node id", {}};
  return std::move(ir.patch());
}

std::vector<LoweredParam> chainParamValues(const Project& p, ChainId chain) {
  std::vector<LoweredParam> out;
  const ChainLoc c = findChain(p, chain);
  if (!c.chain) return out;
  const Rack& rack = *c.rack->rack;
  const bool instrument = c.rack->kind == DeviceKind::Instrument;
  chainParams(rack, *c.chain, instrument, 0, out);
  return out;
}

std::vector<WireState> wireStates(const Project& p, const graph::PatchLibrary& library) {
  if (wiresOf(p).empty()) return {};
  return wireStatesWith(p, makeCallees(p, &library, nullptr, true).callees);
}

std::vector<WireState> wireStates(const Project& p, const DeviceLibrary& devices) {
  if (wiresOf(p).empty()) return {};
  return wireStatesWith(p, makeCallees(p, &devices.library(), &devices, true).callees);
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
