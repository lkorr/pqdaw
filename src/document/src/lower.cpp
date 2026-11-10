// Project → graph lowering (T3.17; IR with stable ids T3.17-fix2): pq/document/lower.h describes the signal flow.
#include "pq/document/lower.h"

#include <algorithm>
#include <functional>
#include <deque>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <numbers>
#include <span>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#include "pq/core/automation_curve.h"
#include "pq/document/audibility.h"
#include "pq/document/devices.h"
#include "pq/document/instrument_groups.h"
#include "pq/document/main_path.h"
#include "pq/document/panel.h"
#include "pq/document/pitch_resolve.h"
#include "pq/graph/atoms.h"
#include "pq/graph/kernel_map.h"
#include "pq/graph/patch_file.h"
#include "pq/document/meta.h"
#include "pq/meta/registry.h"

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
  // GU.1: the device params an automation lane drives (lower.h "Modulation wires": a lane makes its control driven),
  // by device id: (param, the lowest lane id on it — its site's nodes are keyed by it when no wire reaches it).
  std::unordered_map<uint32_t, std::vector<std::pair<std::string, uint32_t>>> laned;
  uint32_t laneOf(DeviceId d, std::string_view param) const noexcept {  // 0: none
    const auto it = laned.find(d.value);
    if (it == laned.end()) return 0;
    for (const auto& [name, lane] : it->second)
      if (name == param) return lane;
    return 0;
  }
  // GU.f4: the value blocks that lower as literals (lowersAsLiteral; only computed with a callee library).
  std::unordered_set<uint32_t> literal;
  bool isLiteral(DeviceId d) const noexcept { return literal.count(d.value) != 0; }
};

using TableOf = std::function<uint32_t(TuningId, const TuningDef&)>;
std::unordered_set<uint32_t> literalValueBlocks(const Project& p);  // GU.f4: lowersAsLiteral of every block (below)

const Send* storedSend(const Track& t, TrackId ret) {
  for (const Send& s : t.mixer.sends)
    if (s.target == ret) return &s;
  return nullptr;
}

// ---- TX.all-free: the cables (lower.h "The main path"; main_path.h) -------------------------------------------------
// What the lowering reads of a project's wires before anything else: each track's main path (its blocks in signal
// order, then the blocks off it), and the project with every wire that is NOT a link of a main path, its main ports
// (kMainPort) named. The links are realised as the path's own connections (the series the chain was: the same IR), so
// the rest of the lowering sees a project whose wires are exactly the other cables, by name.
struct Flow {
  MainPath path;
  std::vector<DevicePtr> list;  // flowOrder: the path, then the blocks off it
};
struct Cables {
  Project q;  // the project as lowered: its non-link wires, main ports named (shares every subtree with the original)
  std::unordered_map<uint32_t, Flow> flows;  // by track id
  std::unordered_set<uint32_t> offPath;      // top-level blocks off their track's main path (device ids)
  std::vector<std::pair<WireId, TrackId>> links;  // every main-path link and its track (id order)
  // IG.1-2 (pq/document/instrument_groups.h): the voice contexts — the instrument groups —
  // of the project as lowered (its wires as these Cables hold them: main ports named).
  VoiceContexts contexts;
  const Flow* flowOf(const Track& t) const noexcept {
    const auto it = flows.find(t.id.value);
    return it != flows.end() ? &it->second : nullptr;
  }
};

// The main ports of `d` by name, as the lowering wires them: a meta module's chain input and first output, a group's
// `x` / `y`, a device patch's first non-const `in` (what a chain feeds: effectCall) and first `out`. Instruments and racks
// have no named main input.
MainPorts lowerPortsOf(const Device& d, const graph::PatchLibrary* lib, const DeviceLibrary* devices) {
  if (isGroupDevice(d) || isMetaDevice(d)) return mainPortsOf(d, devices);
  (void)lib;
  const graph::Patch* callee = builtinBlockPatch(d);  // GU.2: an atom / value block
  if (callee == nullptr) return {};
  MainPorts m;
  for (NodeIndex v = 0; v < callee->nodeCount(); ++v) {
    const graph::Node& n = callee->node(v);
    if (n.kind == graph::NodeKind::Input && m.in.empty() && callee->outputType(v, 0).rate != graph::Rate::Const)
      m.in = callee->nodeName(v);
    else if (n.kind == graph::NodeKind::Output && m.out.empty())
      m.out = callee->nodeName(v);
  }
  return m;
}

// GU.11: `p` with every block inside a switched-off group switched off too (Device::active: "left out of the graph" —
// a group's contents are the group), sharing every track that has none; `p` itself when no group is off.
Project withGroupsOff(const Project& p) {
  bool any = false;
  forEachTrack(p, [&](const Track& t, const Track*) {
    for (const DevicePtr& d : t.devices) any = any || (isGroupDevice(*d) && !d->active);
  });
  if (!any) return p;
  Project q = p;
  auto fix = [](const TrackPtr& tp) -> TrackPtr {
    const Track& t = *tp;
    std::vector<DeviceId> off;
    for (const DevicePtr& d : t.devices)
      if (isGroupDevice(*d) && !d->active) off.push_back(d->id);
    if (off.empty()) return tp;
    auto nt = std::make_shared<Track>(t);
    for (DevicePtr& d : nt->devices) {
      if (!d->active || !d->group.valid()) continue;
      for (const DeviceId g : off)
        if (groupContains(t.devices, g, d->id)) {
          auto x = std::make_shared<Device>(*d);
          x->active = false;
          d = std::move(x);
          break;
        }
    }
    return nt;
  };
  std::function<TrackPtr(const TrackPtr&)> walk = [&](const TrackPtr& tp) -> TrackPtr {
    TrackPtr t = fix(tp);
    if (t->children.empty()) return t;
    auto nt = std::make_shared<Track>(*t);
    for (TrackPtr& ch : nt->children) ch = walk(ch);
    return nt;
  };
  for (TrackPtr& t : q.tracks) t = walk(t);
  for (TrackPtr& t : q.returns) t = walk(t);
  if (q.master) q.master = walk(q.master);
  return q;
}

std::shared_ptr<const Cables> cablesOf(const Project& p, const graph::PatchLibrary* lib, const DeviceLibrary* devices) {
  auto c = std::make_shared<Cables>();
  c->q = withGroupsOff(p);
  const MainLinks mainLinks(p);  // one pass over the wires for every track's path
  forEachTrack(c->q, [&](const Track& t, const Track*) {  // (q's tracks: GU.11 a switched-off group's blocks are off)
    Flow f;
    f.path = mainPath(t, mainLinks);
    f.list = flowOrder(t, f.path);
    for (const DevicePtr& d : t.devices)
      if (!f.path.contains(d->id)) c->offPath.insert(d->id.value);
    for (const WireId w : f.path.links) c->links.emplace_back(w, t.id);
    c->flows.emplace(t.id.value, std::move(f));
  });
  std::sort(c->links.begin(), c->links.end());
  const WireList& wires = wiresOf(p);
  if (wires.empty()) return c;
  WireList rest;
  rest.reserve(wires.size());
  // PF.6: every device by id once (findDevice per device walks the whole project).
  std::unordered_map<uint32_t, const Device*> byId;
  forEachTrack(p, [&](const Track& t, const Track*) {
    for (const DevicePtr& d : t.devices) byId.emplace(d->id.value, d.get());
  });
  std::unordered_map<uint32_t, MainPorts> ports;
  auto portsOf = [&](DeviceId id) -> const MainPorts& {
    auto it = ports.find(id.value);
    if (it == ports.end()) {
      const auto d = id.valid() ? byId.find(id.value) : byId.end();
      it = ports.emplace(id.value, d != byId.end() ? lowerPortsOf(*d->second, lib, devices) : MainPorts{}).first;
    }
    return it->second;
  };
  for (const Wire& w : wires) {
    if (std::binary_search(c->links.begin(), c->links.end(), std::pair<WireId, TrackId>{w.id, TrackId{}},
                           [](const auto& a, const auto& b) { return a.first < b.first; }))
      continue;
    Wire x = w;
    if (x.port.empty() && !x.fromNoteSource()) x.port = portsOf(x.from).out;
    if (x.param.empty() && !x.toMixer()) x.param = portsOf(x.to).in;
    rest.push_back(std::move(x));
  }
  c->q.wires = rest.empty() ? nullptr : std::make_shared<const WireList>(std::move(rest));
  c->contexts = voiceContexts(p);  // (the original project: its main paths; contexts hold its devices, shared by q)
  return c;
}

struct Callees {
  const graph::PatchLibrary* lib = nullptr;  // the library, or the combined one (LoweredProject::userLibrary)
  std::shared_ptr<const Cables> cables;      // TX.all-free: the main paths (never null once made)
  bool isOffPath(const Device& d) const noexcept { return cables != nullptr && cables->offPath.count(d.id.value) != 0; }
  // IG.1-2 (lower.h "Voice contexts"): the MIDI tracks whose notes something plays or reads — a voice context's source,
  // or a block outside every context that reads the newest note (a notes cable into a meta module's input, a note output
  // wire, a meta module heading the path) — and so play voices.
  std::unordered_set<uint32_t> voicedTracks;
  // IG.1-2: the voice contexts (Cables::contexts) and, per context, which of its members run per note (the rest run
  // once: only single-signal cables reach them from inside; SPEC §15 2026-11-06), by device id.
  const VoiceContexts* contexts = nullptr;
  std::unordered_set<uint32_t> perNote;
  const VoiceContext* contextOf(DeviceId d) const noexcept { return contexts != nullptr ? contexts->of(d) : nullptr; }
  const VoiceContext* blockContext(DeviceId d) const noexcept { return contexts != nullptr ? contexts->blockOf(d) : nullptr; }
  // A member of an instrument group (inside it: per note or once), not the block itself.
  const VoiceContext* memberContext(DeviceId d) const noexcept { return contexts != nullptr ? contexts->memberOf(d) : nullptr; }
  bool runsPerNote(DeviceId d) const noexcept { return perNote.count(d.value) != 0; }
  std::unordered_map<uint32_t, const graph::Patch*> userRoot;  // device id → its own root (null: nothing plays)
  // TMM.10-fix1: each active chain meta device's stage (what reaches its chain input: metaStages).
  std::unordered_map<uint32_t, ChainStage> metaStage;
  // TX.patching / IG.1-2: the meta devices expanded in a voice context (MetaExpander::chainDevice): an instrument
  // group's members.
  std::unordered_set<uint32_t> freeVoiced;
  // TX.groups-fix1: the switched-off free devices that pass their chain input to their main output (lower.h "Free
  // devices"): device id → its pass-through patch (in MetaPatches).
  std::unordered_map<uint32_t, const graph::Patch*> bypass;
  const ChainStage* stageOf(const Device& d) const noexcept {
    const auto it = metaStage.find(d.id.value);
    return it != metaStage.end() ? &it->second : nullptr;
  }
  // Whether chain meta device (TX.groups-fix1: or group) `d` takes the note source (its chain input is wired to it: it
  // starts the voices).
  bool readsNotes(const Device& d) const noexcept {
    const ChainStage* s = stageOf(d);
    return s != nullptr && s->in == ChainSignal::Notes;
  }
  // TX.groups-fix1 / IG.1-2: what a group plays — an instrument group GroupNotes (its In the notes per voice, its Out
  // per voice: the context sums it, lower.h "Voice contexts"), any other Group.
  // GU.2: a group nested inside an instrument group that runs per note passes its signals per voice (GroupVoice).
  const graph::Patch& groupCallee(const Device& d) const noexcept {
    if (blockContext(d.id) != nullptr) return groupNotesPatch();
    if (memberContext(d.id) != nullptr && runsPerNote(d.id)) return groupVoicePatch();
    return groupPatch();
  }
  const graph::Patch* find(const Device& d) const noexcept {
    if (lib == nullptr) return nullptr;
    if (const auto it = userRoot.find(d.id.value); it != userRoot.end()) return it->second;
    if (isGroupDevice(d)) return &groupCallee(d);  // TX.groups (lower.h "Groups"; the graph's library holds it)
    if (isMetaDevice(d)) return nullptr;
    if (isAtomDevice(d)) {  // GU.2 (lower.h "Atom and value blocks")
      // GU.9: a voice terminal outside a voice reads 0 (its stand-in has the same outputs).
      if (const graph::Patch* v = voicelessAtomPatch(atomDeviceName(d)); v != nullptr && !runsPerNote(d.id)) return v;
      return atomBlockPatch(atomDeviceName(d));
    }
    if (isValueDevice(d)) return &valuePatch();
    return nullptr;  // GU.11: not a block
  }
};

// TMM.10-fix1: the stage of every active chain meta device of `p` (pq/document/meta.h chainStages; lower.h "Meta
// modules"): a track's own list starts at the note source (a MIDI track) or at audio; a rack chain at the note source
// (an Instrument Rack) or at audio, its later stages global (`voices` false: a rack chain's lowering sums nothing per
// voice between its devices — the pre-fix1 rule, QUESTIONS Q?-TMM.10-fix1-a item 5).
std::unordered_map<uint32_t, ChainStage> metaStages(const Project& p, const Cables& cables) {
  std::unordered_map<uint32_t, ChainStage> out;
  struct Work {
    const std::vector<DevicePtr>* list;
    bool notes, voices;
    const MainPath* path;
  };
  std::vector<Work> work;
  forEachTrack(p, [&](const Track& t, const Track*) {
    // TX.all-free: a track's blocks in flow order along its main path (the blocks off it take no part in the flow).
    const Flow* f = cables.flowOf(t);
    work.push_back(Work{f != nullptr ? &f->list : &t.devices, t.kind == TrackKind::Midi, true, f != nullptr ? &f->path : nullptr});
    while (!work.empty()) {
      const Work w = work.back();
      work.pop_back();
      for (const ChainStage& s : chainStages(*w.list, w.notes, w.voices, w.path))
        if (isMetaDevice(*s.device) || isGroupDevice(*s.device)) out.emplace(s.device->id.value, s);  // TX.groups-fix1
    }
  });
  return out;
}

// The callees of `p`'s blocks: `base`, plus (with `combined`: what a graph needs — lower(), the wire planner's walk into
// callees) a combined library holding the native engines' expansions and the block patches `base` lacks; without it
// (the param fast path: only roots are looked at) nothing is built but the per-device lookup.
struct UserCallees {
  Callees callees;
  std::shared_ptr<graph::PatchLibrary> combined;
  std::vector<LoweredProject::DeviceError> errors;
  std::shared_ptr<MetaPatches> meta;                       // TMM.3: the expansions (combined only)
  std::vector<LoweredProject::MetaResource> metaResources;  // TMM.3 (combined only)
};

using MetaSlotOf = std::function<uint32_t(DeviceId, std::string_view, uint32_t)>;


bool addsAfterLaw(const Wire& w, const Device& d);  // TX.knob-curves (below)

// PF.6: a project's wires by target device id (a mixer wire under its `to` as well; users filter).
using WiresTo = std::unordered_map<uint32_t, std::vector<const Wire*>>;
WiresTo wiresByTarget(const Project& p) {
  WiresTo out;
  out.reserve(wiresOf(p).size());
  for (const Wire& w : wiresOf(p)) out[w.to.value].push_back(&w);
  return out;
}

// TMM.3: expanding a project's meta modules (lower.h "Meta modules").
class MetaExpander {
public:
  // PF.6: `wiresTo`: the project's wires by target device id (makeCallees: built once, not scanned per device / param).
  MetaExpander(const Project& p, UserCallees& u, const MetaSlotOf* slotOf, const WiresTo& wiresTo)
      : p_(p), u_(u), slotOf_(slotOf), wiresTo_(wiresTo) {}

  MetaPatches& patches() {
    if (!u_.meta) {
      u_.meta = std::make_shared<MetaPatches>();
      u_.meta->prelude = meta::prelude();
    }
    return *u_.meta;
  }

  // A chain meta device's expansion, its form from what reaches its chain input (TMM.10-fix1: `stage`, pq/document/
  // meta.h chainStages; never from its type or kind), or null when its type is unknown. TX.patching: a FREE device
  // (out of the series) plays its Nested form — every input an `in` with its default, outputs of any poly — in the
  // track's voice context when the track plays voices (`voiced`), so polyphony follows its wires: the compiler runs it
  // per voice exactly when a per-voice signal reaches it or its output feeds voice processing (pq/graph/infer.h).
  const graph::Patch* chainDevice(const Device& d, const Track& t, const ChainStage& stage, bool voiced = false) {
    const meta::Type* type = meta::findType(metaDeviceType(d));
    if (type == nullptr) return nullptr;
    const std::vector<double> settings = metaSettings(d, "", *type);
    std::vector<uint8_t> modulated(type->params.size(), 0);
    for (size_t i = 0; i < type->params.size(); ++i) modulated[i] = modulatedParam(d, t, type->params[i].name) ? 1 : 0;
    meta::ExpandContext c;
    // IG.1-2: on a main path a meta module is outside every instrument group: single-signal (ChainAudio, global) —
    // what reaches its chain input is audio, or at a MIDI track's note source the newest note (buildPatch). No voice runs.
    c.form = meta::Form::ChainAudio;
    c.voice = false;
    c.chainInput = stage.input;
    if (u_.callees.isOffPath(d)) {  // TX.all-free: off the main path
      c.form = meta::Form::Nested;
      c.voice = voiced;
      c.chainInput = meta::kTypeChainInput;
    }
    // TX.patching: the inputs wires reach become `in`s of the expansion (model.h "Modulation wires": the Add law; a
    // Travel wire never targets an input).
    std::vector<uint8_t> wired(type->inputs.size(), 0);
    bool anyWired = false;
    for (const Wire* wp : into(d.id))
      if (const Wire& w = *wp; !w.travels())
        if (const int32_t i = type->inputIndex(w.param); i >= 0 && type->inputs[static_cast<size_t>(i)].kind == graph::Kind::Real) {
          wired[static_cast<size_t>(i)] = 1;
          anyWired = true;
        }
    if (anyWired) c.wired = wired;
    return expandInstance(d, t, "", *type, c, settings, modulated);
  }

private:
  // Whether a chain device's param is not static: an automation lane or a modulation wire drives it.
  bool modulatedParam(const Device& d, const Track& t, std::string_view name) {
    const ParamRef ref = ParamRef::device(d.id, name);
    for (const LanePtr& l : t.lanes)
      if (l->param == ref) return true;
    for (const Wire* w : into(d.id))
      if (w->param == name) return true;
    return false;
  }
  std::span<const Wire* const> into(DeviceId d) const noexcept {
    const auto it = wiresTo_.find(d.value);
    return it != wiresTo_.end() ? std::span<const Wire* const>(it->second) : std::span<const Wire* const>();
  }

  const graph::Patch* expandInstance(const Device& d, const Track& t, const std::string& path, const meta::Type& type,
                                     meta::ExpandContext c, const std::vector<double>& settings,
                                     const std::vector<uint8_t>& modulated) {
    c.settings = settings;
    c.modulated = modulated;
    // The slots of the resources these settings read (the link owns them; LowerOptions::metaSlotOf).
    std::vector<double> resolved = settings;
    for (size_t i = 0; i < type.settings.size(); ++i)
      resolved[i] = std::clamp(resolved[i], std::min(type.settings[i].min, type.settings[i].max),
                               std::max(type.settings[i].min, type.settings[i].max));
    const std::vector<meta::ResourceRequest> requests =
        type.resources != nullptr ? type.resources(resolved) : std::vector<meta::ResourceRequest>{};
    std::vector<uint32_t> slots;
    // TMM.8-fix: the table's structural variant rides in the index's upper bits (meta::Type::tableVariant).
    const uint32_t variant = type.tableVariant != nullptr ? type.tableVariant(resolved, modulated) & 0xFFFFu : 0u;
    for (const meta::ResourceRequest& r : requests) {
      if (slots.size() <= r.index) slots.resize(r.index + 1, 0);
      slots[r.index] = slotOf_ != nullptr && *slotOf_ ? (*slotOf_)(d.id, path, r.index | (variant << 16)) : 0;
    }
    c.slots = slots;
    const uint64_t key = meta::expansionKey(type, c);
    auto e = meta::expand(type, c, meta::instanceName(type, d.id.value, path, key));
    if (!e) {
      u_.errors.push_back({d.id, e.error().message, false});
      return nullptr;
    }
    const MetaInstance* inst = d.metaAt(path);
    for (const meta::ResourceRequest& r : e->resources) {
      LoweredProject::MetaResource m;
      m.track = t.id;
      m.device = d.id;
      m.path = path;
      m.type = std::string(type.id);
      m.request = r;
      m.slot = r.index < slots.size() ? slots[r.index] : 0;
      m.data = inst != nullptr ? inst->data : type.defaultData != nullptr ? type.defaultData() : nullptr;
      m.modulated = modulated;
      u_.metaResources.push_back(std::move(m));
    }
    return &patches().patches.emplace_back(std::move(e->patch));
  }

  const Project& p_;
  UserCallees& u_;
  const MetaSlotOf* slotOf_;
  const WiresTo& wiresTo_;
};

// TX.all-free: `p` is the project as lowered (Cables::q), `cables` its main paths.
UserCallees makeCallees(const Project& p, std::shared_ptr<const Cables> cables, const graph::PatchLibrary* base,
                        bool combined, const MetaSlotOf* metaSlotOf = nullptr) {
  UserCallees u;
  u.callees.lib = base;
  u.callees.cables = std::move(cables);
  if (base == nullptr) return u;
  // TMM.3: meta modules — the native engines.
  const WiresTo wiresTo = wiresByTarget(p);  // PF.6
  MetaExpander ex(p, u, metaSlotOf, wiresTo);
  std::shared_ptr<const meta::Prelude> prelude;
  bool staged = false;  // TMM.10-fix1: every chain meta device's stage, computed at the first one met
  // TX.groups-fix1: a group's stage decides what it plays (Callees::groupCallee) and whether it takes the notes.
  bool groups = false;
  forEachDevice(p, [&](const Device& d, const Track&) { groups = groups || isGroupDevice(d); });
  if (groups) {
    staged = true;
    u.callees.metaStage = metaStages(p, *u.callees.cables);
  }
  // IG.1-2 (lower.h "Voice contexts"): the voice contexts, which of their members run per note, and the MIDI tracks
  // whose notes something plays or reads (Callees::voicedTracks).
  u.callees.contexts = &u.callees.cables->contexts;
  if (!staged) {  // (a rack chain may start with a meta module, a path with a meta module reading the notes)
    staged = true;
    u.callees.metaStage = metaStages(p, *u.callees.cables);
  }
  for (const VoiceContext& c : u.callees.contexts->list) {
    if (!c.block->active) continue;
    u.callees.voicedTracks.insert(c.source.value);
    // A member runs ONCE when it has an input from inside the group and every such input is single-signal: a cable
    // marked single, or one from a member that runs once (SPEC §15 2026-11-06). Inputs from outside are broadcast and
    // decide nothing. Every other member runs per note.
    // GU.2: its members at any depth (a nested group is a member that runs per note because its parent does).
    const std::vector<const Device*> members = groupMembersDeep(c.home->devices, c.block->id);
    std::unordered_set<uint32_t> once;
    std::unordered_set<uint32_t> memberIds;
    memberIds.reserve(members.size());
    for (const Device* m : members) memberIds.insert(m->id.value);
    auto inside = [&](const Wire& w) {
      if (w.fromNoteSource()) return w.noteSource == c.source;
      if (w.from == c.block->id) return w.port == kGroupInPort;
      return memberIds.count(w.from.value) != 0;
    };
    for (bool changed = true; changed;) {
      changed = false;
      for (const Device* m : members) {
        if (once.count(m->id.value) != 0) continue;
        bool any = false, all = true;
        const auto in = wiresTo.find(m->id.value);
        if (in == wiresTo.end()) continue;
        for (const Wire* wp : in->second) {  // PF.6: its own inputs only
          const Wire& w = *wp;
          if (w.toMixer() || !inside(w)) continue;
          // GU.9: a nested group's Out terminal is fed by its own members (what it gives, not what it takes).
          if (w.param == kGroupOutPort && isGroupDevice(*m)) continue;
          any = true;
          const bool single = w.single || (!w.fromNoteSource() && once.count(w.from.value) != 0);
          if (!single) all = false;
        }
        if (any && all) {
          once.insert(m->id.value);
          changed = true;
        }
      }
    }
    for (const Device* m : members)
      if (once.count(m->id.value) == 0) u.callees.perNote.insert(m->id.value);
  }
  // Readers of the newest note outside every context: a notes cable into a meta module's input, a note output wire,
  // a meta module that heads a MIDI track's path and takes the notes.
  for (const Wire& w : wiresOf(p))
    if (w.fromNoteSource() && !w.toMixer())
      if (const Track* src = findTrackPtr(p, w.noteSource); src != nullptr && src->kind == TrackKind::Midi) {
        if (!w.port.empty()) {
          u.callees.voicedTracks.insert(src->id.value);
          continue;
        }
        if (const DeviceLoc l = findDevice(p, w.to); l.device != nullptr && isMetaDevice(*l.device))
          u.callees.voicedTracks.insert(src->id.value);
      }
  forEachTrack(p, [&](const Track& t, const Track*) {
    if (t.kind != TrackKind::Midi) return;
    if (const Flow* f = u.callees.cables->flowOf(t))
      for (const Device* x : f->path.blocks) {
        if (!x->active) continue;
        if (isMetaDevice(*x) && u.callees.readsNotes(*x)) u.callees.voicedTracks.insert(t.id.value);
        break;
      }
  });
  forEachDevice(p, [&](const Device& d, const Track& t) {
    if (isMetaDevice(d)) {
      if (!staged) {
        staged = true;
        u.callees.metaStage = metaStages(p, *u.callees.cables);
      }
      // An inactive device has no stage (it is not lowered); its kind stands in (its form for a lookup only).
      const auto sit = u.callees.metaStage.find(d.id.value);
      ChainStage st;
      if (sit != u.callees.metaStage.end()) {
        st = sit->second;
      } else {
        st.device = &d;
        const meta::Type* type = meta::findType(metaDeviceType(d));
        st.input = type != nullptr ? metaChainInput(d, *type) : -1;
        st.in = st.input < 0 ? ChainSignal::None : d.kind == DeviceKind::Instrument ? ChainSignal::Notes : ChainSignal::Audio;
      }
      if (combined) {
        // IG.1-2: a meta device inside an instrument group that runs per note plays in its source's voice context
        // (its Nested form: per voice where the voices reach it, pq/graph/infer.h); everywhere else it is single-signal.
        const bool voiced = u.callees.memberContext(d.id) != nullptr && u.callees.runsPerNote(d.id);
        if (voiced) u.callees.freeVoiced.insert(d.id.value);
        u.callees.userRoot[d.id.value] = ex.chainDevice(d, t, st, voiced);
        return;
      }
      // The param fast path reads only the root's params: the prelude's form of the type has the same ones (the
      // form of the stage, so the lowering's instrument check agrees).
      if (!prelude) prelude = meta::prelude();
      const meta::Prelude::Entry* e = prelude->find(metaDeviceType(d));
      const meta::Form f = meta::Form::ChainAudio;  // IG.1-2
      const std::optional<graph::Patch>* form = e == nullptr                   ? nullptr
                                                : f == meta::Form::ChainNotes ? &e->notes
                                                : f == meta::Form::ChainVoice ? &e->voice
                                                                              : &e->audio;
      u.callees.userRoot[d.id.value] = form != nullptr && *form ? &**form : nullptr;
      return;
    }
  });
  // TX.groups-fix1 (lower.h "Free devices"): a switched-off free device whose main output a wire reads passes what
  // reaches its chain input (the input a chain would feed: a meta module's chain input, else the first non-const `in`)
  // to that output, as the chain passes a switched-off device by: a call of `pass__<in>__<out>` (one `in`, one `out`,
  // any poly, up to stereo; nothing else).
  if (combined && !wiresOf(p).empty()) {
    std::unordered_map<std::string, const graph::Patch*> made;
    forEachDevice(p, [&](const Device& d, const Track&) {
      // (GU.11: a switched-off GROUP too — its x → y, what reaches it passes on, as a switched-off device's did.)
      if (d.active || !u.callees.isOffPath(d)) return;
      std::string in, out;
      if (isMetaDevice(d)) {
        const meta::Type* type = meta::findType(metaDeviceType(d));
        if (type == nullptr || type->outputs.empty()) return;
        if (const int32_t i = metaChainInput(d, *type); i >= 0) in = std::string(type->inputs[static_cast<size_t>(i)].name);
        out = std::string(type->outputs.front().name);
      } else if (const graph::Patch* root = u.callees.find(d)) {
        for (NodeIndex v = 0; v < root->nodeCount(); ++v) {
          const graph::Node& n = root->node(v);
          if (n.kind == graph::NodeKind::Input && in.empty() && root->outputType(v, 0).rate != graph::Rate::Const)
            in = root->nodeName(v);
          else if (n.kind == graph::NodeKind::Output && out.empty())
            out = root->nodeName(v);
        }
      }
      if (in.empty() || out.empty()) return;
      const bool read = std::any_of(wiresOf(p).begin(), wiresOf(p).end(),
                                    [&](const Wire& w) { return w.from == d.id && w.port == out && !w.fromNoteSource(); });
      if (!read) return;
      const std::string name = "pass__" + in + "__" + out;
      auto it = made.find(name);
      if (it == made.end()) {
        if (!u.meta) {
          u.meta = std::make_shared<MetaPatches>();
          u.meta->prelude = meta::prelude();
        }
        constexpr graph::PortType kPass = graph::portType(graph::Kind::Real, graph::Rate::Audio, graph::Poly::Infer,
                                                          graph::kWidthStereo);
        graph::Patch bp(name);
        const NodeIndex i = bp.addInput(in, kPass);
        const NodeIndex o = bp.addOutput(out, kPass);
        bp.connect(PortRef{i, 0}, PortRef{o, 0});
        const graph::NodeId passIds[] = {graph::NodeId{1}, graph::NodeId{2}};
        if (!bp.setNodeIds(passIds) || !bp.validate()) return;
        it = made.emplace(name, &u.meta->patches.emplace_back(std::move(bp))).first;
      }
      u.callees.bypass[d.id.value] = it->second;
    });
  }
  if (combined && u.meta) {  // the expansions (and the prelude, for unnamed calls) join the callee library
    if (!u.combined) {
      u.combined = std::make_shared<graph::PatchLibrary>();
      u.combined->addAll(*base);
    }
    for (const graph::Patch& x : u.meta->patches) u.combined->add(x);
    for (const meta::Prelude::Entry& e : u.meta->prelude->entries) u.combined->add(e.nested);
    u.callees.lib = u.combined.get();
  }
  // TX.groups: the group patch joins the callee library when a group plays (the graph calls it by name); TX.groups-fix1:
  // and each variant a group's stage selects.
  // (A DeviceLibrary's own library holds them already: devices.cpp.)
  const bool baseHasGroups = base->find(groupNotesPatch().name()) == &groupNotesPatch() &&
                             base->find(groupPatch().name()) == &groupPatch();
  if (combined && groups && (!baseHasGroups || u.combined)) {
    if (!u.combined) {
      u.combined = std::make_shared<graph::PatchLibrary>();
      u.combined->addAll(*base);
    }
    bool voice = false, notes = false;
    forEachDevice(p, [&](const Device& d, const Track&) {
      if (!isGroupDevice(d)) return;
      const graph::Patch& c = u.callees.groupCallee(d);
      voice = voice || &c == &groupVoicePatch();
      notes = notes || &c == &groupNotesPatch();
    });
    u.combined->add(groupPatch());
    if (voice) u.combined->add(groupVoicePatch());
    if (notes) u.combined->add(groupNotesPatch());
    u.callees.lib = u.combined.get();
  }
  // GU.2: atom and value blocks' patches join the callee library when the base lacks them (a DeviceLibrary holds them
  // all: no copy then).
  if (combined) {
    std::vector<const graph::Patch*> blocks;
    forEachDevice(p, [&](const Device& d, const Track&) {
      if (!isAtomDevice(d) && !isValueDevice(d)) return;
      const graph::Patch* bp = builtinBlockPatch(d);
      if (bp != nullptr && u.callees.lib->find(bp->name()) != bp && std::find(blocks.begin(), blocks.end(), bp) == blocks.end())
        blocks.push_back(bp);
      if (isAtomDevice(d))  // GU.9: a voice terminal's stand-in
        if (const graph::Patch* vp = voicelessAtomPatch(atomDeviceName(d));
            vp != nullptr && u.callees.lib->find(vp->name()) != vp && std::find(blocks.begin(), blocks.end(), vp) == blocks.end())
          blocks.push_back(vp);
    });
    if (!blocks.empty()) {
      if (!u.combined) {
        u.combined = std::make_shared<graph::PatchLibrary>();
        u.combined->addAll(*base);
      }
      for (const graph::Patch* bp : blocks) u.combined->add(*bp);
      u.callees.lib = u.combined.get();
    }
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
        if (n.kind == Kind::Midi && tableOf != nullptr && *tableOf && here.depth > path.depth &&
            lib != nullptr && lib->voicedTracks.count(t->id.value) != 0)  // IG.1-2: its notes play somewhere
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
  // GU.1: every device param with an automation lane (any track: a lane lives on the track holding its device).
  forEachTrack(p, [&](const Track& t, const Track*) {
    for (const LanePtr& l : t.lanes) {
      if (l->param.kind != ParamKind::DeviceParam || l->paramName.empty()) continue;
      auto& list = pl.laned[l->param.owner];
      auto it = std::find_if(list.begin(), list.end(), [&](const auto& e) { return e.first == l->paramName; });
      if (it == list.end()) list.emplace_back(l->paramName, l->id.value);
      else it->second = std::min(it->second, l->id.value);
    }
  });
  if (lib != nullptr) pl.literal = literalValueBlocks(p);  // GU.f4 (lower.h "Structural value blocks")
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
  kClipPlayer = 80, kClipSlot,             // T5.3: an audio track's source and its slot constant
  // TMM.10: a meta voice run's release (the declick ramp after note-off and the voice's end)
  kRelGate = 86, kRelNot, kRelRate, kRelSr, kRelStep, kRelPrev, kRelSub, kRelMax, kRelEnv, kRelMul, kRelLe, kRelDone,
  kCFreeZero = 98,  // TX.patching: a free device's unwired audio input (−0: an added zero the optimiser removes)
  // TX.pan-laws: the True Stereo law's 2×2 matrix (gainStages)
  kStPos = 100, kStNegPan, kStNeg, kStThP, kStThN, kStCosPIn, kStLL, kStLR, kStCosNIn, kStRR, kStRL, kStDirect,
  kStCross, kStL, kStR, kStSwap, kStGain, kStSum,
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
  // The document-derived id node `n` gets at finish() (TX.mod-bands: CurveSite::value).
  graph::NodeId idOf(NodeIndex n) const noexcept { return graph::NodeId{n < ids_.size() ? ids_[n] : 0u}; }
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

// TX.pan-laws: True Stereo, each input channel panned on its own (model.h PanMode::Stereo): x' = gain(x, (ll, rr)) +
// gain(swap(x), (rl, lr)), the four gains at block rate from the pan param and smoothed per sample like every mixer
// gain (one more smoothed pair per track; at the centre the cross pair is 0 and gain_add passes x' through).
PortRef stereoPan(Ir& ir, PortRef x, PortRef pan, PortRef c0, PortRef time) {
  const PortRef halfPi = ir.konst(std::numbers::pi / 2, kCHalfPi);
  const PortRef thP = ir.atom("mul", {ir.atom("max", {pan, c0}, kStPos), halfPi}, kStThP);
  const PortRef thN = ir.atom("mul", {ir.atom("max", {ir.atom("neg", {pan}, kStNegPan), c0}, kStNeg), halfPi}, kStThN);
  const PortRef ll = ir.atom("sin", {ir.atom("add", {thP, halfPi}, kStCosPIn)}, kStLL);  // cos: the left input, kept left
  const PortRef lr = ir.atom("sin", {thP}, kStLR);                                        //      moved right
  const PortRef rr = ir.atom("sin", {ir.atom("add", {thN, halfPi}, kStCosNIn)}, kStRR);
  const PortRef rl = ir.atom("sin", {thN}, kStRL);
  const PortRef swap = ir.atom("pack", {ir.atom("channel", {x, ir.konst(1.0, kC1)}, kStR), ir.atom("channel", {x, c0}, kStL)}, kStSwap, 2);
  // start 1: the direct pair begins at its target (a switch to this law does not duck the track while it fades in).
  const PortRef direct = ir.atom("gain", {x, ir.atom("pack", {ll, rr}, kStDirect, 2), time, ir.konst(1.0, kC1)}, kStGain);
  return ir.atom("gain_add", {direct, swap, ir.atom("pack", {rl, lr}, kStCross, 2), time}, kStSum);
}

// The gain stages (see lower.h): x → out (and for senders post / pre exports). `mode`: the pan law (model.h PanMode).
void gainStages(Ir& ir, Kind kind, PanMode mode, PortRef x, float smoothingMs) {
  const PortRef volume = ir.param("volume", 0.0, kMinVolumeDb, kMaxVolumeDb, kVolume);
  const PortRef pan = ir.param("pan", 0.0, -1.0, 1.0, kPan);
  const PortRef c0 = ir.konst(0.0, kC0), c1 = ir.konst(1.0, kC1);
  const PortRef time = ir.konst(static_cast<double>(smoothingMs) * 1e-3, kCTime);
  const PortRef vg = ir.atom("select", {ir.atom("le", {volume, ir.konst(kMinVolumeDb, kCFloor)}, kVolLe), c0,
                                        ir.atom("db_to_gain", {volume}, kVolDb)},
                             kVg);
  // The law's gains (block rate: the pan is a param), folded into each stage's one smoothed gain (TX.pan-laws).
  PortRef pl, pr;
  switch (mode) {
    case PanMode::ConstantPower: {  // −3 dB per side at the centre
      const PortRef th = ir.atom("mul", {ir.atom("add", {pan, c1}, kPanA), ir.konst(std::numbers::pi / 4, kCQuarterPi)}, kTheta);
      pl = ir.atom("sin", {ir.atom("add", {th, ir.konst(std::numbers::pi / 2, kCHalfPi)}, kPanB)}, kPl);
      pr = ir.atom("sin", {th}, kPr);
      break;
    }
    case PanMode::Linear: {  // −6 dB per side at the centre
      const PortRef half = ir.konst(0.5, kCHalf);
      pl = ir.atom("mul", {ir.atom("sub", {c1, pan}, kPanA), half}, kPl);
      pr = ir.atom("mul", {ir.atom("add", {c1, pan}, kPanB), half}, kPr);
      break;
    }
    case PanMode::Stereo:  // the 2×2 matrix before the stages; the stages carry the volume and gates only
      x = stereoPan(ir, x, pan, c0, time);
      pl = pr = c1;
      break;
    case PanMode::Balance:  // 0 dB at the centre, never above it
      pl = ir.atom("min", {c1, ir.atom("sub", {c1, pan}, kPanA)}, kPl);
      pr = ir.atom("min", {c1, ir.atom("add", {c1, pan}, kPanB)}, kPr);
      break;
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

// TX.patching: a param's declared range in its normalised unit (what the patch reads: Hz, seconds, linear gain for a dB
// param, cents); false without one.
bool paramRangeOf(const graph::ParamDecl& d, double& lo, double& hi) noexcept {
  if (!d.hasRange) return false;
  lo = d.rangeMin.value();
  hi = d.rangeMax.value();
  return std::isfinite(lo) && std::isfinite(hi) && hi >= lo;
}
// TX.patching: a param marked @wrap (a phase: its patch wraps it, so an added signal is not clamped to its range).
bool paramWraps(const graph::ParamDecl& d) noexcept {
  return std::any_of(d.attrs.begin(), d.attrs.end(), [](const graph::Attr& a) { return a.name == "wrap"; });
}
// TMM.18: a param marked @signed (the Phase accumulator's frequency): an added signal may take it below its knob range,
// to −max(|lo|, |hi|); it is clamped to ±max(|lo|, |hi|) instead of [lo, hi].
bool paramSigned(const graph::ParamDecl& d) noexcept {
  return std::any_of(d.attrs.begin(), d.attrs.end(), [](const graph::Attr& a) { return a.name == "signed"; });
}
// TX.knob-curves: whether wire `w` into a param whose patch takes its wires after its own law (an `in <param>_add`
// beside it, user patch text) goes there: an Add or Units wire; a Travel wire never. (GU.4: no meta type has such a
// param any more: the VCA, whose `value` was one, is a group preset whose knob is a value block's curve.)
bool addsAfterLaw(const Wire& w, const Device&) { return !w.travels(); }

// TX.patching: the number of `out`s of a callee (its signature's outputs before its exports).
uint32_t outputCount(const graph::Patch& callee) noexcept {
  uint32_t n = 0;
  for (NodeIndex v = 0; v < callee.nodeCount(); ++v) n += callee.node(v).kind == graph::NodeKind::Output ? 1u : 0u;
  return n;
}
// TX.patching: the signature index (the `out`s, then the exports) of a callee's output named `name`, or kNone.
// TX.bool-real: a bool `out` (a comparison's, `done`) is a signal too: its 0 / 1 (graph::infer converts it where it
// reaches a real input).
uint32_t signalOutput(const graph::Patch& callee, std::string_view name) noexcept {
  const uint32_t outs = outputCount(callee);
  uint32_t o = 0, e = 0;
  for (NodeIndex v = 0; v < callee.nodeCount(); ++v) {
    const graph::NodeKind k = callee.node(v).kind;
    if (k == graph::NodeKind::Output) {
      if (callee.nodeName(v) == name) return o;
      ++o;
    } else if (k == graph::NodeKind::Export) {
      if (callee.nodeName(v) == name) return outs + e;
      ++e;
    }
  }
  return graph::kNone;
}
// TX.bool-real: whether a callee's output `port` (its signature index: the `out`s, then the exports) is a declared bool
// `out` (a comparison's, `done`).
bool boolOutput(const graph::Patch& callee, uint32_t port) noexcept {
  uint32_t o = 0;
  for (NodeIndex v = 0; v < callee.nodeCount(); ++v)
    if (callee.node(v).kind == graph::NodeKind::Output && o++ == port)
      return callee.inputType(v, 0).kind == graph::Kind::Bool;
  return false;
}
// TX.groups: the declared width of a callee's output `port` (its signature index: the `out`s, then the exports; an
// export carries its first channel's kind of signal: treated as mono).
uint16_t declaredOutWidth(const graph::Patch& callee, uint32_t port) noexcept {
  uint32_t o = 0;
  for (NodeIndex v = 0; v < callee.nodeCount(); ++v)
    if (callee.node(v).kind == graph::NodeKind::Output && o++ == port) return callee.inputType(v, 0).width;
  return graph::kWidthMono;
}
// TX.patching: the signature index of a callee's non-const, non-bool `in` named `name` (a wire target), or kNone.
uint32_t signalInput(const graph::Patch& callee, std::string_view name) noexcept {
  uint32_t ordinal = 0;
  for (NodeIndex v = 0; v < callee.nodeCount(); ++v) {
    if (callee.node(v).kind != graph::NodeKind::Input) continue;
    if (callee.nodeName(v) == name) {
      const graph::PortType& t = callee.outputType(v, 0);
      return t.rate == graph::Rate::Const || t.kind == graph::Kind::Bool ? graph::kNone : ordinal;
    }
    ++ordinal;
  }
  return graph::kNone;
}

// A lowered device (in a track's flat list).
struct LDev {
  const Device* device = nullptr;
  const graph::Patch* callee = nullptr;  // its patch
  // Its signal is per voice: an instrument group's member that runs per note (IG.1-2).
  bool voice = false;
  // TX.groups-fix1: a switched-off free device passing its chain input to its main output (callee: its pass__ patch).
  bool bypass = false;
  bool offPath = false;  // TX.all-free: a block off its track's main path (called after the path, played by its wires)
  // A voice context's block lowered here (IG.1-2: an instrument group: it plays its source's notes; its `done` joins
  // the track's).
  bool plays = false;
  bool top = false;  // TX.all-free-fix1: a block of the track itself
  // IG.1-2 (lower.h "Voice contexts"): REMOTE — the block, on this track's main path, of a voice context lowered in
  // another track's patch (its source's): not called here; what the path hears after it is its output, imported
  // (`in v<device>`). HOSTED — a device of another track's voice context lowered in this (its source's) patch.
  bool remote = false;
  bool hosted = false;
  uint32_t host = graph::kNone;  // remote: the plan node lowering it
  // IG.1-2: a meta module heading a MIDI track's path outside every context: its chain input reads the newest note.
  bool notesIn = false;
};
// The lowered device tree of one track.
struct TrackDevs {
  std::vector<LDev> devs;
  std::vector<uint32_t> top;  // the track's main path (TX.all-free: its blocks in signal order, the series the chain was)
  std::vector<uint32_t> offPath;  // TX.all-free: its blocks off the main path (called, played by their wires), list order
  // IG.1-2: the voice contexts' blocks lowered here (indices into devs): their `done`s are the track's (the AND).
  std::vector<uint32_t> contexts;
  bool complete = true;  // TX.all-free: the main path reaches the mixer (else the mixer hears only the wires into it)
  struct PendingFree {
    const Device* device = nullptr;
    const graph::Patch* callee = nullptr;
    bool bypass = false;  // TX.groups-fix1: switched off, passing its chain input on
    bool plays = false;   // IG.1-2: a voice context's block (an instrument group)
    bool hosted = false;  // IG.1-2: of another track's context
  };
  std::vector<PendingFree> pendingFree;  // lowerList → lowerTree
  bool voices = false;  // the track plays voices (IG.1-2: something plays or reads its notes)

  size_t size() const noexcept { return devs.size(); }
  bool empty() const noexcept { return devs.empty(); }
  const LDev& operator[](size_t i) const noexcept { return devs[i]; }
};

// Whether a device list's first active device lowers as an instrument that plays.
bool listLowersInstrument(const std::vector<DevicePtr>& list, const Callees* lib) {
  if (lib == nullptr) return false;
  {
    for (const DevicePtr& d : list) {
      if (!d->active || lib->isOffPath(*d)) continue;  // TX.all-free: a block off the main path is not in the flow
      // TMM.10-fix1: it plays when its chain input takes the note source (TX.groups-fix1: a group there too)
      if ((isMetaDevice(*d) || isGroupDevice(*d)) && lib->readsNotes(*d) && lib->find(*d) != nullptr) return true;
      break;
    }
  }
  return false;
}

bool lowersInstrument(const Track& t, const Callees* lib) noexcept {
  if (t.kind != TrackKind::Midi || lib == nullptr) return false;
  const Flow* f = lib->cables != nullptr ? lib->cables->flowOf(t) : nullptr;  // TX.all-free: along its main path
  return listLowersInstrument(f != nullptr ? f->list : t.devices, lib);
}

// IG.1-2 (lower.h "Voice contexts"): where a track's blocks are lowered. A voice context is lowered in its notes
// source's patch: its block and members are HOSTED there when that is another track's (`elsewhere`: device id → the plan
// node lowering it); on its own track only a REMOTE placeholder stands on the path for it.
struct TrackScope {
  const std::unordered_map<uint32_t, uint32_t>* elsewhere = nullptr;
  bool hostedList = false;  // the list is another track's context, lowered here (every block off the path)
};

// Lowers a track's block list into `td` (its indices into `out`): active devices whose patch is in the library and of
// their kind. `missing`: devices bypassed because their patch is not in the library or not of their kind (an atom block
// whose patch reads as an instrument's). `list` is a track's blocks (its main path, then the blocks off it) with
// `scope`. A group plays its context (GroupNotes) when it is an instrument group, else it is an audio block. A meta module heading a MIDI track's path is single-signal: its chain input reads the
// newest note (LDev::notesIn).
void lowerList(TrackDevs& td, const std::vector<DevicePtr>& list, bool instrumentAllowed, const Callees* lib,
               std::vector<DeviceId>* missing, std::vector<uint32_t>& out, const TrackScope& scope = {}) {
  for (const DevicePtr& d : list) {
    const bool hostedTop = scope.hostedList;
    const bool off = hostedTop || (lib != nullptr && lib->isOffPath(*d));
    const VoiceContext* vc = lib != nullptr ? lib->contextOf(d->id) : nullptr;
    // IG.1-2: a block of a context lowered in another track's patch: a placeholder on the path, else nothing here.
    if (!scope.hostedList && scope.elsewhere != nullptr)
      if (const auto it = scope.elsewhere->find(d->id.value); it != scope.elsewhere->end()) {
        if (!off && vc != nullptr && vc->block == d.get() && d->active) {
          out.push_back(static_cast<uint32_t>(td.devs.size()));
          LDev ld{d.get(), nullptr, false};
          ld.top = ld.remote = true;
          ld.host = it->second;
          td.devs.push_back(ld);
        }
        continue;
      }
    if (!d->active) {
      // TX.groups-fix1: a switched-off free device whose output a wire reads passes its chain input there.
      if (off)
        if (const auto it = lib->bypass.find(d->id.value); it != lib->bypass.end())
          td.pendingFree.push_back({d.get(), it->second, true, false, hostedTop});
      continue;
    }
    const bool staged = isMetaDevice(*d) || isGroupDevice(*d);
    const bool notesHere = instrumentAllowed && out.empty();  // the list's notes reach it along the list
    const bool contextBlock = vc != nullptr && vc->block == d.get();  // IG.1-2: an instrument group
    const graph::Patch* callee = lib != nullptr ? lib->find(*d) : nullptr;
    // (A chain meta device is its list's instrument by its stage, not by its expansion's ports.)
    if (callee == nullptr || (!staged && calleeIsInstrument(*callee))) {
      if (missing != nullptr) missing->push_back(d->id);
      continue;
    }
    if (off) {  // TX.patching / TX.all-free: off the main path; called, never chained
      td.pendingFree.push_back({d.get(), callee, false, contextBlock, hostedTop});
      continue;
    }
    out.push_back(static_cast<uint32_t>(td.devs.size()));
    LDev ld{d.get(), callee, false};
    ld.top = true;
    ld.plays = contextBlock;
    ld.notesIn = notesHere && isMetaDevice(*d) && lib != nullptr && lib->readsNotes(*d);
    if (ld.plays) td.contexts.push_back(static_cast<uint32_t>(td.devs.size()));
    td.devs.push_back(ld);
  }
}

// The plan node lowering each device of a voice context whose source is another track than its own (IG.1-2): the
// source's node (only while both tracks are lowered).
std::unordered_map<uint32_t, uint32_t> hostedElsewhere(const Plan& pl, const Callees* lib) {
  std::unordered_map<uint32_t, uint32_t> out;
  if (lib == nullptr || lib->contexts == nullptr) return out;
  for (const VoiceContext& c : lib->contexts->list) {
    if (c.source == c.home->id) continue;
    const auto host = pl.indexOf.find(c.source.value);
    const auto home = pl.indexOf.find(c.home->id.value);
    if (host == pl.indexOf.end() || home == pl.indexOf.end()) continue;
    out.emplace(c.block->id.value, host->second);
    for (const Device* d : groupMembersDeep(c.home->devices, c.block->id)) out.emplace(d->id.value, host->second);
  }
  return out;
}

TrackDevs lowerTree(const Plan& pl, const Node& n, const Callees* lib, std::vector<DeviceId>* missing,
                    const std::unordered_map<uint32_t, uint32_t>& elsewhere) {
  TrackDevs td;
  // TX.all-free: the track's blocks in flow order: its main path in signal order (lowered as the chain was: the same
  // IR), then the blocks off it (free). A path that does not reach the mixer leaves the mixer only its wires.
  const Flow* flow = lib != nullptr && lib->cables != nullptr ? lib->cables->flowOf(*n.track) : nullptr;
  if (flow != nullptr) td.complete = flow->path.complete;
  TrackScope scope;
  scope.elsewhere = &elsewhere;
  lowerList(td, flow != nullptr ? flow->list : n.track->devices, n.kind == Kind::Midi, lib, missing, td.top, scope);
  // IG.1-2: the voice contexts other tracks' blocks make with this track's notes, lowered here (off the path).
  const uint32_t self = pl.indexOf.at(n.track->id.value);
  if (lib != nullptr && lib->contexts != nullptr && n.kind == Kind::Midi)
    for (const VoiceContext& c : lib->contexts->list) {
      if (c.source != n.track->id || c.home == n.track) continue;
      const auto it = elsewhere.find(c.block->id.value);
      if (it == elsewhere.end() || it->second != self) continue;
      std::vector<DevicePtr> list;
      for (const DevicePtr& d : c.home->devices)
        if (d.get() == c.block || groupContains(c.home->devices, c.block->id, d->id)) list.push_back(d);
      std::vector<uint32_t> unused;
      TrackScope hosted;
      hosted.hostedList = true;
      lowerList(td, list, false, lib, missing, unused, hosted);
    }
  // TX.patching: the free devices, after every device of the series. IG.1-2: a member of an instrument group that runs
  // per note is a voice device (wires into it stay per voice); every other free device is single-signal.
  for (const TrackDevs::PendingFree& pf : td.pendingFree) {
    const uint32_t k = static_cast<uint32_t>(td.devs.size());
    td.offPath.push_back(k);
    const bool perNote = lib != nullptr && lib->runsPerNote(pf.device->id);
    // (GU.2: a nested group inside an instrument group that runs per note is a voice device too: GroupVoice.)
    LDev ld{pf.device, pf.callee, perNote};
    ld.bypass = pf.bypass;
    ld.offPath = true;
    ld.hosted = pf.hosted;
    const VoiceContext* vc = lib != nullptr ? lib->blockContext(pf.device->id) : nullptr;
    ld.plays = pf.plays && vc != nullptr && vc->block == pf.device;  // a context's block (its `done` is the track's)
    ld.top = true;
    if (ld.plays) td.contexts.push_back(k);
    td.devs.push_back(ld);
  }
  td.pendingFree.clear();
  // The track plays voices when its notes play or are read anywhere (Callees::voicedTracks).
  td.voices = n.kind == Kind::Midi && lib != nullptr && lib->voicedTracks.count(n.track->id.value) != 0;
  return td;
}

// The lowered devices of every plan node (`missing`: as lowerList).
std::vector<TrackDevs> allDevices(const Plan& pl, const Callees* lib, std::vector<DeviceId>* missing) {
  std::vector<TrackDevs> out(pl.nodes.size());
  const std::unordered_map<uint32_t, uint32_t> elsewhere = hostedElsewhere(pl, lib);
  for (size_t i = 0; i < pl.nodes.size(); ++i) out[i] = lowerTree(pl, pl.nodes[i], lib, missing, elsewhere);
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
  bool reduce = false;                 // a voice source into a global target (or through a single-signal cable)
  WireReducer reducer = WireReducer::Sum;  // the reducer it reduces with (IG.1-2: the newest note for a notes cable)
  // TMM.10-fix2: a wire from track fromNode's note source (fromDev / fromPort unused): the voice terminal it reads.
  const NoteOutput* notes = nullptr;
  // TX.patching: the target is an input of its callee (toPin its signature index among the `in`s), not a param; the
  // source then keeps all its channels (on the same track: a stereo signal into a stereo input stays stereo).
  bool input = false;
  // TX.all-free: the source is its track's own signal (a wire from the source's main port that is not a main-path link:
  // the input, clips or sends the path starts from; fromDev / fromPort unused), and / or the target is the track's
  // mixer input (toDev / toPin unused; `input` is set: the mixer reads the path's end plus these wires).
  bool fromTrack = false;
  bool toMixer = false;
  bool phase = false;  // TX.all-free-fix1: a notes cable into a phase input: the note's phase (phasor of its Hz)
  // IG.1-2: a single-signal term into an instrument group's Out (from a member that runs once, a single-signal cable or
  // from outside): added once after the voices' sum, not per voice.
  bool outGlobal = false;
  // IG.1-2: a single-signal source (outside every context, or a member that runs once) into a block that runs per note:
  // broadcast to every voice — its value marked `shared` (Node::shared), so a free-running source behind it keeps one
  // instance instead of being made per voice (pq/graph/infer.h).
  bool broadcast = false;
};

struct WirePlan {
  std::vector<LWire> wires;       // lowered, in id order
  std::vector<WireState> states;  // every wire of the project, in id order
  // IG.1-2: the remote placeholders (lower.h "Voice contexts"): a context block on track `home`'s path lowered in plan
  // node `host` — its output crosses as `export v<device>` → `in v<device>` (graph::WireKind::Audio).
  struct Remote {
    uint32_t host = 0, hostDev = 0;
    uint32_t home = 0, homeDev = 0;
  };
  std::vector<Remote> remotes;
  // EQ.3 (lower.h "Structural value blocks"): per target device id, the const inputs a stepped / constant value block's
  // wire sets (input name, the block's knob value): literals at the call, as settings are.
  std::unordered_map<uint32_t, std::vector<std::pair<std::string, double>>> constFeeds;
  // EQ.3: wires whose source was a mux block with a literal index, rewritten to start at the picked input's source (the
  // LWires point here; a deque: the addresses stay when the plan is moved).
  std::deque<Wire> folded;
  // PF.6: per plan node, the indices into `wires` (ascending: id order) of the wires into it / out of it, so a track's
  // patch looks at its own wires only (not every wire of the project per track, per group, per param).
  std::vector<std::vector<uint32_t>> into, outOf;
  // PF.6: each callee's call signature, made once per lowering (not per device instance: it builds the callee's
  // adjacency). Keyed by the callee, which outlives the lowering's use of the plan.
  mutable std::unordered_map<const graph::Patch*, graph::SubcircuitRef> signatures;
  const graph::SubcircuitRef& signature(const graph::Patch& callee) const {
    auto it = signatures.find(&callee);
    if (it == signatures.end()) it = signatures.emplace(&callee, graph::subcircuitSignature(callee)).first;
    return it->second;
  }
  std::span<const uint32_t> wiresInto(uint32_t node) const noexcept {
    return node < into.size() ? std::span<const uint32_t>(into[node]) : std::span<const uint32_t>();
  }
  std::span<const uint32_t> wiresOutOf(uint32_t node) const noexcept {
    return node < outOf.size() ? std::span<const uint32_t>(outOf[node]) : std::span<const uint32_t>();
  }
};

// IG.1-2: the export a voice context's output crosses to its home's path under (a remote placeholder reads it).
std::string remotePortName(DeviceId d) { return "v" + std::to_string(d.value); }

// IG.1-2: an instrument group's block lowered here (its call is GroupNotes: In per voice, Out per voice, `y` its sum).
bool isContextGroup(const LDev& d) noexcept { return d.plays && isGroupDevice(*d.device); }

// EQ.3 (lower.h "Structural value blocks"): whether `d` is a stepped (a menu, a switch) or constant value block.
bool steppedOrConstant(const Device& d) noexcept {
  const dsp::Curve* c = isValueDevice(d) ? d.curve(kValueParam) : nullptr;
  return c != nullptr && (c->step > 0.0 || c->min == c->max);
}
// EQ.3: whether `input` is a `real const` input of atom block `d` (a structural input: the svf's mode, stages).
bool atomConstInput(const Device& d, std::string_view input) noexcept {
  if (!isAtomDevice(d)) return false;
  const auto id = graph::AtomRegistry::builtin().find(atomDeviceName(d));
  if (!id) return false;
  for (const graph::PortSpec& ps : graph::atomSignature(*id).inputs)
    if (ps.name == input) return ps.type.rate == graph::Rate::Const;
  return false;
}
// The value block's runtime value (its curve at its knob position, rounded to float as its param would be).
double valueBlockValue(const LDev& d) {
  const auto pp = realParamPin(*d.callee, kValueParam);
  return pp ? static_cast<double>(static_cast<float>(paramValue(*d.device, kValueParam, d.callee->paramDecl(pp->node)))) : 0.0;
}

// Decides how every wire is lowered (lower.h, "Modulation wires"): which are left out, which close a delay-free cycle
// (delayed) and which a loop the engine cannot run yet (refused: VoiceLoop).
WirePlan planWires(const Project& p, const Plan& pl, const std::vector<TrackDevs>& devs, PortDeps& deps,
                   const Callees* lib) {
  WirePlan wp;
  const WireList& wires = wiresOf(p);
  const auto nodes = static_cast<uint32_t>(pl.nodes.size());
  // Where each lowered device is (IG.1-2: its one instance; a remote placeholder is not one).
  std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> where;
  for (uint32_t n = 0; n < nodes; ++n)
    for (uint32_t k = 0; k < devs[n].size(); ++k)
      if (!devs[n][k].remote) where[devs[n][k].device->id.value] = {n, k};
  for (uint32_t n = 0; n < nodes; ++n)
    for (uint32_t k = 0; k < devs[n].size(); ++k)
      if (devs[n][k].remote)
        if (const auto it = where.find(devs[n][k].device->id.value); it != where.end())
          wp.remotes.push_back(WirePlan::Remote{it->second.first, it->second.second, n, k});
  if (wires.empty() && wp.remotes.empty()) return wp;
  auto isInstrumentNode = [&](uint32_t n) { return pl.nodes[n].kind == Kind::Midi && devs[n].voices; };
  // Signal points: per node its input and output, per device its signature inputs then outputs (a remote placeholder:
  // its output).
  std::vector<uint32_t> nodeBase(nodes);
  std::vector<std::vector<uint32_t>> devBase(nodes);
  auto insOf = [&](uint32_t n, uint32_t k) {
    const LDev& d = devs[n][k];
    return d.remote ? 0u : deps.of(*d.callee).ins;
  };
  auto outsOf = [&](uint32_t n, uint32_t k) {
    const LDev& d = devs[n][k];
    return d.remote ? 1u : static_cast<uint32_t>(deps.of(*d.callee).outs.size());
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
  // TMM.13-fix1 H3 / IG.1-2: which signal points run per voice (1: a device that runs per note — an instrument group's
  // member, an instrument group's In and Out —) and which once (0: everything else, an instrument
  // group's summed output `y` included).
  std::vector<uint8_t> voicePoint(points, 0);
  for (uint32_t n = 0; n < nodes; ++n)
    for (uint32_t k = 0; k < devs[n].size(); ++k) {
      const LDev& d = devs[n][k];
      if (d.voice) std::fill_n(voicePoint.begin() + devBase[n][k], insOf(n, k) + outsOf(n, k), uint8_t{1});
      if (isContextGroup(d)) {
        std::fill_n(voicePoint.begin() + devBase[n][k], insOf(n, k) + outsOf(n, k), uint8_t{1});
        voicePoint[devOut(n, k, 0)] = 0;  // y: the voices summed
      }
    }
  for (uint32_t n = 0; n < nodes; ++n) {
    const TrackDevs& td = devs[n];
    for (uint32_t k = 0; k < td.size(); ++k) {  // inside each device
      const LDev& d = td[k];
      if (d.remote) continue;
      const PortDeps::Matrix& m = deps.of(*d.callee);
      for (uint32_t o = 0; o < m.outs.size(); ++o)
        for (uint32_t i = 0; i < m.ins; ++i)
          if (m.dep(o, i)) adj[devIn(n, k, i)].push_back(devOut(n, k, o));
    }
    // The chain: the track's input → each effect's audio input → its first out → … → the track's output (as buildPatch
    // wires it); a voice context's block ignores the signal so far.
    auto flow = [&](std::span<const uint32_t> list, uint32_t x) -> uint32_t {
      for (const uint32_t k : list) {
        const LDev& d = td[k];
        if (d.remote || d.plays) {  // IG.1-2: a voice context's output (here or imported)
          x = devOut(n, k, 0);
          continue;
        }
        const uint32_t in = d.notesIn ? graph::kNone : audioInIndex(*d.callee);  // (the newest note: no signal point)
        if (x != graph::kNone && in != graph::kNone) adj[x].push_back(devIn(n, k, in));
        x = deps.of(*d.callee).outs.empty() ? graph::kNone : devOut(n, k, 0);
      }
      return x;
    };
    // A MIDI track's chain starts at its notes, an audio track's at its clip player (T5.3): neither reads `in`.
    const bool ownSource = pl.nodes[n].kind == Kind::Midi || pl.nodes[n].kind == Kind::Audio;
    const uint32_t x = flow(std::span<const uint32_t>(td.top), ownSource ? graph::kNone : nodeBase[n]);
    if (x != graph::kNone && td.complete) adj[x].push_back(nodeBase[n] + 1);  // TX.all-free: a broken path: not
    if (pl.dest[n] != graph::kNone) adj[nodeBase[n] + 1].push_back(nodeBase[pl.dest[n]]);
  }
  for (const SendLink& l : pl.sends)
    if (!l.delayed) adj[nodeBase[l.from] + 1].push_back(nodeBase[l.to]);
  // Track-level graph (routing, every send, cross-track wires and remote contexts): a cross-track wire must not close a
  // cycle through two instrument tracks (one voice table per unit, Q84: IG.3).
  std::vector<std::vector<uint32_t>> tadj(nodes);
  for (uint32_t n = 0; n < nodes; ++n)
    if (pl.dest[n] != graph::kNone) tadj[n].push_back(pl.dest[n]);
  for (const SendLink& l : pl.sends) tadj[l.from].push_back(l.to);
  for (const WirePlan::Remote& r : wp.remotes) {  // IG.1-2: the context's output → its placeholder on its home's path
    adj[devOut(r.host, r.hostDev, 0)].push_back(devOut(r.home, r.homeDev, 0));
    if (r.host != r.home) tadj[r.host].push_back(r.home);
  }
  // PF.6: what a point reaches, marked with a generation (no clearing of every point per wire); `stopAt`: stop once
  // that point is reached (the marks are then partial: only "reached stopAt" is known).
  struct Marks {
    std::vector<uint32_t> m;
    uint32_t gen = 0;
    void reset(size_t n) {
      if (m.size() != n || ++gen == 0) {
        m.assign(n, 0);
        gen = 1;
      }
    }
    bool operator[](size_t i) const noexcept { return m[i] == gen; }
  };
  Marks seen;
  std::vector<uint32_t> stack;
  auto reach = [&](const std::vector<std::vector<uint32_t>>& g, uint32_t from, Marks& mark, uint32_t stopAt = graph::kNone) {
    mark.reset(g.size());
    stack.assign(1, from);
    mark.m[from] = mark.gen;
    if (from == stopAt) return true;
    while (!stack.empty()) {
      const uint32_t v = stack.back();
      stack.pop_back();
      for (uint32_t w : g[v])
        if (mark.m[w] != mark.gen) {
          mark.m[w] = mark.gen;
          if (w == stopAt) return true;
          stack.push_back(w);
        }
    }
    return false;
  };
  // TMM.10-fix2: the plan node of each track (a note source's wires start at their track's node).
  std::unordered_map<uint32_t, uint32_t> nodeOfTrack;
  for (uint32_t n = 0; n < nodes; ++n)
    if (pl.nodes[n].track != nullptr) nodeOfTrack.emplace(pl.nodes[n].track->id.value, n);
  Marks fwd, back;
  // Whether a wire from plan node `from` to plan node `to` would close a track cycle through two instrument tracks (the
  // partition could not run it: one voice table per unit, Q84; per-sample voices across two contexts are IG.3).
  auto twoInstruments = [&](uint32_t from, uint32_t to) {
    reach(tadj, to, fwd);
    if (!fwd[from]) return false;  // no track cycle forms
    std::vector<std::vector<uint32_t>> rev(nodes);
    for (uint32_t v = 0; v < nodes; ++v)
      for (uint32_t x : tadj[v]) rev[x].push_back(v);
    reach(rev, from, back);
    uint32_t instruments = 0;
    for (uint32_t v = 0; v < nodes; ++v)
      if ((fwd[v] && back[v]) || v == from || v == to) instruments += isInstrumentNode(v) ? 1u : 0u;
    return instruments > 1;
  };
  // TMM.13-fix1 H3 / IG.1-2: whether the delay-free loop a wire from signal point `from` to `to` closes (`seen`: what
  // `to` reaches) passes both per-voice and single-signal points — it crosses a voice sum (an instrument group's Out, a
  // single-signal cable, a cable leaving the group) and comes back into the voices. Its z1 would need every voice group
  // and the single-signal part to alternate per sample: IG.3 (the fused per-sample region). Until then the wire is
  // refused (VoiceLoop; the status line and the views name it), and the rest plays.
  auto voiceLoop = [&](uint32_t from, uint32_t to) {
    bool voice = false, global = false;
    auto mark = [&](uint32_t v) {
      voice = voice || voicePoint[v] == 1;
      global = global || voicePoint[v] == 0;
    };
    mark(to);
    mark(from);
    if (!(voice && global)) {  // the points on the loop: reached from `to`, reaching `from`
      std::vector<std::vector<uint32_t>> rev(points);
      for (uint32_t v = 0; v < points; ++v)
        for (uint32_t x : adj[v]) rev[x].push_back(v);
      reach(rev, from, back);
      for (uint32_t v = 0; v < points && !(voice && global); ++v)
        if (seen[v] && back[v]) mark(v);
    }
    return voice && global;
  };
  auto refuse = [&](WireState st) {
    st.status = WireStatus::Refused;
    st.reason = WireRefusal::VoiceLoop;
    wp.states.push_back(st);
  };
  const VoiceContexts* ctxs = lib != nullptr ? lib->contexts : nullptr;
  auto ctxOf = [&](DeviceId d) -> const VoiceContext* { return ctxs != nullptr ? ctxs->of(d) : nullptr; };
  // TX.groups-fix1: chain links (Wire::chain: a grouped series' links) are planned before every other wire, as a chain's
  // own links are (they are in `adj` already): a loop through a grouped series keeps its delay on the wire that had it.
  // The result is in id order again (below).
  std::vector<const Wire*> order;
  order.reserve(wires.size());
  for (const Wire& w : wires)
    if (w.chain) order.push_back(&w);
  const bool anyChain = !order.empty();
  for (const Wire& w : wires)
    if (!w.chain) order.push_back(&w);
  // EQ.3 (lower.h "Structural value blocks"): a mux block whose `index` is one wire from a literal value block and whose
  // picked input is one wire lowers as that wire: every wire out of it starts at the picked input's source instead (the
  // mux block stays, unread). Each branch keeps its own width (a stereo In and a mono channel, the EQ's matrix), which
  // the optimiser's fold of a typed mux cannot give.
  std::unordered_map<uint32_t, const Wire*> muxPicked;  // mux block id → the wire into its picked input
  {
    struct MuxIns {
      const Wire* at[3] = {nullptr, nullptr, nullptr};  // index, in0, in1
      uint8_t count[3] = {0, 0, 0};
    };
    std::unordered_map<uint32_t, MuxIns> muxes;
    auto lowered = [&](DeviceId id) -> const LDev* {
      const auto it = where.find(id.value);
      return it == where.end() ? nullptr : &devs[it->second.first][it->second.second];
    };
    for (const Wire& w : wires) {
      if (w.toMixer() || w.fromNoteSource()) continue;
      const LDev* t = lowered(w.to);
      if (t == nullptr || t->bypass || !isAtomDevice(*t->device) || atomDeviceName(*t->device) != "mux") continue;
      const int slot = w.param == "index" ? 0 : w.param == "in0" ? 1 : w.param == "in1" ? 2 : -1;
      if (slot < 0) continue;
      MuxIns& m = muxes[w.to.value];
      m.at[slot] = &w;
      ++m.count[slot];
    }
    for (const auto& [mux, m] : muxes) {
      if (m.count[0] != 1) continue;
      const LDev* v = lowered(m.at[0]->from);
      if (v == nullptr || v->bypass || !isValueDevice(*v->device) || m.at[0]->port != kValueOutput ||
          !pl.isLiteral(v->device->id))
        continue;
      const float idx = static_cast<float>(valueBlockValue(*v));
      const int pick = std::isnan(idx) || std::floor(idx) <= 0.0f ? 0 : 1;
      if (m.count[1 + pick] == 1) muxPicked.emplace(mux, m.at[1 + pick]);
    }
  }
  for (const Wire* wp0 : order) {
    if (!muxPicked.empty() && !wp0->fromNoteSource() && wp0->port == "out" && muxPicked.count(wp0->from.value) != 0) {
      Wire f = *wp0;
      for (size_t hops = 0; hops <= muxPicked.size(); ++hops) {
        const auto it = muxPicked.find(f.from.value);
        if (it == muxPicked.end() || f.fromNoteSource() || f.port != "out") break;
        f.from = it->second->from;
        f.port = it->second->port;
        f.noteSource = it->second->noteSource;
      }
      wp.folded.push_back(std::move(f));
      wp0 = &wp.folded.back();
    }
    const Wire& w = *wp0;
    WireState st{w.id, WireStatus::Lowered};
    const auto b = w.toMixer() ? where.end() : where.find(w.to.value);
    std::pair<uint32_t, uint32_t> tgt{graph::kNone, graph::kNone};
    if (b != where.end()) tgt = b->second;
    LWire lw;
    lw.wire = &w;
    lw.reducer = w.reducer;
    auto inactive = [&] {
      st.status = WireStatus::Inactive;
      wp.states.push_back(st);
    };
    const VoiceContext* tctx = w.toMixer() ? nullptr : ctxOf(w.to);
    if (w.fromNoteSource() && w.port.empty()) {
      // TX.all-free: the track's own signal (not a link of its main path): into a block or the mixer of a track.
      const auto a = nodeOfTrack.find(w.noteSource.value);
      const uint32_t target = w.toMixer() ? (nodeOfTrack.count(w.mixer.value) ? nodeOfTrack.at(w.mixer.value) : graph::kNone)
                                          : (b != where.end() ? b->second.first : graph::kNone);
      // TX.all-free-fix1: another track's mixer takes a track's own signal too (a send-like route; not its notes).
      const bool otherMixer = w.toMixer() && a != nodeOfTrack.end() && target != graph::kNone && target != a->second;
      const bool midi = a != nodeOfTrack.end() && pl.nodes[a->second].kind == Kind::Midi;
      if (a == nodeOfTrack.end() || target == graph::kNone || (!midi && target != a->second && !otherMixer)) {
        inactive();
        continue;
      }
      if (midi && !otherMixer) {
        // IG.1-2: a NOTES CABLE. Into a voice context's block: its feed plays it (Lowered, nothing to wire), any other
        // notes into it are not played. Into a member of a context with this source: per note (a meta module's input
        // reads the note's signal of its meaning). Anything else outside every context
        // reads the newest note.
        if (w.toMixer()) {
          inactive();
          continue;
        }
        const LDev& dd = devs[tgt.first][tgt.second];
        if (tctx != nullptr && tctx->block == dd.device) {
          if (tctx->source == w.noteSource && (tctx->feed == w.id || (!tctx->feed.valid() && tctx->home->id == w.noteSource)))
            wp.states.push_back(st);
          else
            inactive();
          continue;
        }
        const bool member = tctx != nullptr;
        if (member && tctx->source != w.noteSource) {
          inactive();
          continue;
        }
        const meta::Type* type = !dd.bypass && isMetaDevice(*dd.device) ? meta::findType(metaDeviceType(*dd.device)) : nullptr;
        const int32_t in = type != nullptr ? type->inputIndex(w.param) : -1;
        if (in < 0 || !devs[a->second].voices) {
          inactive();
          continue;
        }
        const meta::Reads reads = type->inputs[static_cast<size_t>(in)].reads;
        lw.fromNode = a->second;
        lw.fromDev = graph::kNone;
        if (reads == meta::Reads::Frequency || reads == meta::Reads::Phase) {
          lw.notes = findNoteOutput("hz");
          lw.phase = reads == meta::Reads::Phase;
        } else if (dd.voice) {
          wp.states.push_back(st);  // per note: the input's own default is the note's (Lowered; nothing to wire)
          continue;
        } else if (reads == meta::Reads::Gate) {
          lw.notes = findNoteOutput("gate");  // outside: the newest note's gate
        } else {
          inactive();
          continue;
        }
        if (!dd.voice) lw.reducer = WireReducer::Last;  // outside every context: the newest note
      } else {
        lw.fromTrack = true;
        lw.fromNode = a->second;
        lw.fromDev = graph::kNone;
      }
    } else if (w.fromNoteSource()) {
      // TMM.10-fix2: the source is a MIDI track's note output: it plays while the track plays voices.
      const auto a = nodeOfTrack.find(w.noteSource.value);
      if (a == nodeOfTrack.end() || (!w.toMixer() && b == where.end()) || pl.nodes[a->second].kind != Kind::Midi ||
          !devs[a->second].voices) {
        inactive();
        continue;
      }
      lw.fromNode = a->second;
      lw.fromDev = graph::kNone;
      lw.notes = findNoteOutput(w.port);
    } else {
      const auto a = where.find(w.from.value);
      if (a == where.end() || (!w.toMixer() && b == where.end())) {
        inactive();
        continue;
      }
      std::tie(lw.fromNode, lw.fromDev) = a->second;
    }
    // Whether the source runs per voice at this port (IG.1-2: an instrument group's In / Out per voice, its `y` summed).
    auto voiceSourceAt = [&](uint16_t port) {
      if (lw.notes != nullptr) return true;
      if (lw.fromTrack) return false;
      const LDev& sd = devs[lw.fromNode][lw.fromDev];
      return sd.voice || (isContextGroup(sd) && port != 0);
    };
    if (w.toMixer()) {
      // TX.all-free: into the track's mixer input (from a block of that track, or its own signal): summed after the
      // main path's end (buildPatch), global (a voice source goes through the wire's reducer).
      const auto t = nodeOfTrack.find(w.mixer.value);
      if (t == nodeOfTrack.end()) {
        inactive();
        continue;
      }
      if (lw.notes != nullptr) {
        inactive();
        continue;
      }
      lw.toNode = t->second;
      lw.toDev = graph::kNone;
      lw.toMixer = lw.input = true;
      uint32_t sourcePort = 0;
      if (!lw.fromTrack) {
        const LDev& sd = devs[lw.fromNode][lw.fromDev];
        sourcePort = signalOutput(*sd.callee, w.port);
        if (sourcePort != graph::kNone && sd.bypass && sourcePort != 0) sourcePort = graph::kNone;
      }
      if (sourcePort == graph::kNone) {
        st.status = WireStatus::Invalid;
        wp.states.push_back(st);
        continue;
      }
      lw.fromPort = static_cast<uint16_t>(sourcePort);
      lw.reduce = voiceSourceAt(lw.fromPort);
      const uint32_t from = lw.fromTrack ? nodeBase[lw.fromNode] : devOut(lw.fromNode, lw.fromDev, lw.fromPort);
      const uint32_t to = nodeBase[lw.toNode] + 1;
      if (lw.fromNode != lw.toNode) {
        // TX.all-free-fix1 / TX.cross-track: into ANOTHER track's mixer, a send-like route; one that closes a cycle
        // between tracks is planned like every cross-track wire (z⁻¹ per sample, refused through two instruments).
        if (twoInstruments(lw.fromNode, lw.toNode)) {
          refuse(st);
          continue;
        }
        tadj[lw.fromNode].push_back(lw.toNode);
      }
      lw.delayed = reach(adj, to, seen, from);
      if (lw.delayed) {
        reach(adj, to, seen);  // (voiceLoop reads all `to` reaches)
        if (voiceLoop(from, to)) {
          refuse(st);
          continue;
        }
        st.status = WireStatus::Delayed;
      } else {
        adj[from].push_back(to);
      }
      wp.states.push_back(st);
      wp.wires.push_back(lw);
      continue;
    }
    std::tie(lw.toNode, lw.toDev) = tgt;
    const LDev& dd = devs[lw.toNode][lw.toDev];
    // The source: an output of its callee (TX.patching: an `out` as well as an export; its signature index: outs first;
    // TX.bool-real: a bool output too, as its 0 / 1) or a note output; the target: a real param or (TX.patching) a non-const real input
    // of the callee.
    uint32_t sourcePort = graph::kNone;
    if (lw.fromTrack) {
      sourcePort = 0;  // TX.all-free: the track's own signal
    } else if (w.fromNoteSource()) {
      if (lw.notes != nullptr) sourcePort = 0;  // an unknown note output: Invalid
    } else {
      sourcePort = signalOutput(*devs[lw.fromNode][lw.fromDev].callee, w.port);
    }
    uint32_t pin = graph::kNone, point = graph::kNone;
    if (const uint32_t units = !addsAfterLaw(w, *dd.device) ? graph::kNone : signalInput(*dd.callee, w.param + "_add");
               units != graph::kNone && realParamPin(*dd.callee, w.param)) {
      // TX.patching: a param whose patch adds its wires after its own law (an input `<param>_add` in its text): the
      // wire goes there, the param stays the knob (param rate).
      pin = point = units;
      lw.input = true;
    } else if (const std::optional<ParamPin> pp = realParamPin(*dd.callee, w.param)) {
      pin = pp->pin;
      point = pp->pin;
      lw.paramNode = pp->node;
    } else if (const uint32_t in = signalInput(*dd.callee, w.param); in != graph::kNone) {
      pin = point = in;
      lw.input = true;
    }
    // A Travel wire goes into a param (never an input). GU.9: from any output too (a macro's value block: a mapping).
    if (w.travels() && sourcePort != graph::kNone && lw.input) sourcePort = graph::kNone;
    // TX.groups-fix1: a switched-off free device passing its chain input on (LDev::bypass) has only those two ports; a
    // wire to or from any other of its ports waits until it plays again.
    if ((dd.bypass && pin == graph::kNone) ||
        (!w.fromNoteSource() && devs[lw.fromNode][lw.fromDev].bypass && sourcePort == graph::kNone)) {
      st.status = WireStatus::Inactive;
      wp.states.push_back(st);
      continue;
    }
    // TX.groups-fix1: the In terminal of an instrument group (GroupNotes) gives the input it reaches the note's signal of
    // that input's meaning (meta::Reads, as a chain input at the note source reads it): a frequency the note's Hz
    // (`input`), a phase its phase (`phase`). An input of no such meaning (a signal, a param, a stock effect's input)
    // gets nothing from the notes: the wire is inactive.
    // GU.4: a block of the kit — an atom block's input, a nested group's main input — has no meaning of its own: it gets
    // the In terminal's own signal, the note's frequency (`input` = voice.hz), so a Phase Accumulator preset (In → its
    // group → hz + freq → phasor) plays the note as the meta module did.
    if (!w.fromNoteSource() && sourcePort != graph::kNone && w.port == kGroupInPort) {
      const LDev& sd = devs[lw.fromNode][lw.fromDev];
      if (sd.callee == &groupNotesPatch()) {
        meta::Reads reads = meta::Reads::Signal;
        if (!dd.bypass && (isAtomDevice(*dd.device) || isGroupDevice(*dd.device)) && lw.paramNode == graph::kNone)
          reads = meta::Reads::Frequency;
        if (lw.input && !dd.bypass && isMetaDevice(*dd.device))
          if (const meta::Type* t = meta::findType(metaDeviceType(*dd.device)))
            if (const int32_t i = t->inputIndex(w.param); i >= 0) reads = t->inputs[static_cast<size_t>(i)].reads;
        if (reads == meta::Reads::Phase) {
          sourcePort = signalOutput(*sd.callee, "phase");
        } else if (reads != meta::Reads::Frequency) {
          st.status = WireStatus::Inactive;
          wp.states.push_back(st);
          continue;
        }
      }
    }
    // EQ.3 (lower.h "Structural value blocks"): a stepped or constant value block's wire into an atom block's `real const`
    // input (the svf's mode and stages) sets it to the block's knob value, a literal at the call (as a setting).
    if (pin == graph::kNone && sourcePort != graph::kNone && !lw.fromTrack && !w.fromNoteSource() && !dd.bypass &&
        lw.fromNode == lw.toNode && atomConstInput(*dd.device, w.param)) {
      const LDev& sd = devs[lw.fromNode][lw.fromDev];
      auto& feeds = wp.constFeeds[dd.device->id.value];
      if (!sd.bypass && steppedOrConstant(*sd.device) && w.port == kValueOutput &&
          std::none_of(feeds.begin(), feeds.end(), [&](const auto& f) { return f.first == w.param; })) {
        feeds.emplace_back(w.param, valueBlockValue(sd));
        wp.states.push_back(st);
        continue;
      }
    }
    if (sourcePort == graph::kNone || pin == graph::kNone) {
      st.status = WireStatus::Invalid;
      wp.states.push_back(st);
      continue;
    }
    lw.fromPort = static_cast<uint16_t>(sourcePort);
    lw.toPin = static_cast<uint16_t>(pin);
    // IG.1-2 (lower.h "Voice contexts"): a per-voice source stays per voice only into a device of the same voice context
    // that runs per note (or its group's Out), through a cable that is not single-signal; anything else reads it
    // through the cable's reducer (a cable leaving the group, a single-signal one). Into an instrument group's Out, a
    // term that is not per note (single-signal, a member that runs once, a signal from outside) is added once after
    // the voices' sum (LWire::outGlobal).
    const bool voiceSource = voiceSourceAt(lw.fromPort);
    const VoiceContext* sctx = lw.fromTrack || w.fromNoteSource() ? nullptr : ctxOf(w.from);
    const bool intoOut = isContextGroup(dd) && w.param == kGroupOutPort;
    const bool sameContext = lw.fromNode == lw.toNode && tctx != nullptr &&
                             (w.fromNoteSource() ? tctx->source == w.noteSource : sctx == tctx);
    const bool keepsVoices = sameContext && (dd.voice || intoOut) && !w.single;
    lw.reduce = voiceSource && !keepsVoices;
    lw.outGlobal = intoOut && !(voiceSource && keepsVoices);
    lw.broadcast = !voiceSource && dd.voice && !lw.fromTrack;
    if (lw.fromNode != lw.toNode) {
      if (twoInstruments(lw.fromNode, lw.toNode)) {  // a track cycle through two instrument tracks (IG.3)
        refuse(st);
        continue;
      }
      tadj[lw.fromNode].push_back(lw.toNode);
    }
    // TMM.10-fix2: a note source depends on nothing in the graph: no cycle can pass through it (no signal point).
    if (lw.notes != nullptr) {
      wp.states.push_back(st);
      wp.wires.push_back(lw);
      continue;
    }
    // A delay-free cycle: the target reaches the source.
    const uint32_t from = lw.fromTrack ? nodeBase[lw.fromNode] : devOut(lw.fromNode, lw.fromDev, lw.fromPort);
    const uint32_t to = devIn(lw.toNode, lw.toDev, point);
    lw.delayed = reach(adj, to, seen, from);
    if (lw.delayed) {
      reach(adj, to, seen);  // (voiceLoop reads all `to` reaches)
      // TMM.13-fix1 H3 / IG.1-2: across a voice sum (a single-signal path back into the voices: IG.3).
      if (voiceLoop(from, to) || (lw.reduce && voicePoint[to] == 1)) {
        refuse(st);
        continue;
      }
      st.status = WireStatus::Delayed;
    } else {
      adj[from].push_back(to);
    }
    wp.states.push_back(st);
    wp.wires.push_back(lw);
  }
  if (anyChain) {  // back to id order (lower.h: sums into one input follow it, after the chain links)
    std::stable_sort(wp.states.begin(), wp.states.end(), [](const WireState& a, const WireState& b) { return a.id < b.id; });
    std::stable_sort(wp.wires.begin(), wp.wires.end(), [](const LWire& a, const LWire& b) { return a.wire->id < b.wire->id; });
  }
  wp.into.resize(pl.nodes.size());
  wp.outOf.resize(pl.nodes.size());
  for (uint32_t i = 0; i < wp.wires.size(); ++i) {
    const LWire& w = wp.wires[i];
    if (w.toNode < wp.into.size()) wp.into[w.toNode].push_back(i);
    if (w.fromNode < wp.outOf.size()) wp.outOf[w.fromNode].push_back(i);
  }
  return wp;
}
// A device's call node: named d<id>, NodeId nodeIdFor(Device, id, 0); the settings the user set become literals at
// their `real const` inputs; params stay unconnected (runtime params d<id>__<name>, linked by the app).
// EQ.3: a const input a value block's wire sets (WirePlan::constFeeds) takes that value instead of a setting.
graph::NodeIndex callDevice(Ir& ir, const LDev& d, const WirePlan* wp) {
  const graph::NodeIndex c = ir.call(wp != nullptr ? wp->signature(*d.callee) : graph::subcircuitSignature(*d.callee),
                                     nodeIdFor(NodeRole::Device, d.device->id.value, 0));
  graph::Patch& p = ir.patch();
  p.setNodeName(c, "d" + std::to_string(d.device->id.value));
  const std::vector<std::pair<std::string, double>>* feeds = nullptr;
  if (wp != nullptr && !d.bypass)
    if (const auto it = wp->constFeeds.find(d.device->id.value); it != wp->constFeeds.end()) feeds = &it->second;
  auto fed = [&](std::string_view name) {
    return feeds != nullptr && std::any_of(feeds->begin(), feeds->end(), [&](const auto& f) { return f.first == name; });
  };
  for (const DeviceValue& v : d.device->settings) {
    const auto in = p.findInput(c, v.name);
    if (!in || p.inputType(c, *in).rate != graph::Rate::Const) continue;  // not a setting of this patch (any more)
    if (fed(v.name)) continue;
    p.connect(ir.literal(v.value), PortRef{c, static_cast<uint16_t>(*in)});
  }
  if (feeds != nullptr)
    for (const auto& [name, value] : *feeds)
      if (const auto in = p.findInput(c, name); in && p.inputType(c, *in).rate == graph::Rate::Const)
        p.connect(ir.literal(value), PortRef{c, static_cast<uint16_t>(*in)});
  return c;
}

// ---- Building a track patch's devices (T4.1) ------------------------------------------

// TMM.10: a meta voice run's release after note-off (lower.h "Meta modules"): long enough not to click, short enough
// to read as the note's end (an Envelope meta module in the run shapes the real release, TMM.11).
constexpr double kMetaReleaseSeconds = 0.005;

constexpr PortRef kPending{graph::kNone, 0};


// What building one track patch keeps.
// IG.1-2: an instrument group's end as playGroup decided it (finishGroups wires it once the members are called).
struct GroupEnd {
  uint32_t group = 0;                                // device index of the group
  std::vector<std::pair<uint32_t, uint16_t>> dones;  // (member device index, its `done` output)
  PortRef ramp{graph::kNone, 0};                     // the release ramp's `done` (none: no ramp)
};

struct Build {
  Ir& ir;
  const LowerOptions& o;
  const TrackDevs& td;
  std::vector<NodeIndex> calls;  // per lowered device: its call node
  // TMM.10-fix2: the pitch the devices hear (the last tuning node's hz; kPending without tuning nodes: voice.hz is it).
  PortRef tunedHz = kPending;
  // TX.patching: what a free device's audio input reads while no wire reaches it (a wire replaces it).
  PortRef freeZero = kPending;
  // TX.all-free: the track's own signal (its input, clips or sends: what its main path starts from; kPending on a MIDI
  // track), read by wires from the source's main port that are not links of the path.
  PortRef sourceX = kPending;
  PortRef c0() { return ir.konst(0.0, kC0); }
  PortRef c1() { return ir.konst(1.0, kC1); }
  PortRef time() { return ir.konst(static_cast<double>(o.smoothingMs) * 1e-3, kCTime); }
  const Track* track = nullptr;  // the track whose patch this is; null: none
  std::vector<LoweredProject::MetaResource>* resources = nullptr;  // TX.spectrum: the analysis taps' slots
  // With several voice contexts (TrackDevs::contexts) the track's `done` is the AND of theirs, made at the end (each
  // `done` and the device it ends); `allDone` false: one has none (the engine's own end).
  bool multi = false;
  std::vector<std::pair<PortRef, uint32_t>> dones;
  bool allDone = true;
  const Callees* lib = nullptr;
  const WirePlan* wp = nullptr;
  const Plan* pl = nullptr;  // GU.1: its automated params (Plan::laned); null: none
  uint32_t node = 0;  // this patch's plan node
  // IG.1-2 (lower.h "Voice contexts"): per lowered device, a voice context's output (an instrument group's voices
  // summed), what its `y` / main output gives everything outside; and the pending input the
  // single-signal terms into an instrument group's Out are added at (kPending: none).
  std::vector<PortRef> ctxOut, outGlobalPin;
  std::vector<GroupEnd> ends;
};

constexpr graph::PortType kVoiceDone = graph::portType(graph::Kind::Bool, graph::Rate::Infer, graph::Poly::Voice, 1);
// A voice context's `done` (kPending: it has none): the track's own output when it is the only one, else one term of
// the AND made at the end (finishDones).
void voiceDone(Build& b, PortRef done, uint32_t device) {
  if (!b.multi) {
    if (done.node != graph::kNone) b.ir.output("done", kVoiceDone, done, kDone);
    return;
  }
  if (done.node == graph::kNone) b.allDone = false;
  else b.dones.emplace_back(done, device);
}
// Device node slots of a voice context's block (NodeRole::Device, k 6 / 7: free slots): its voices summed for a remote
// placeholder, its `done` in the AND of the track's.
enum DevK : uint32_t { kDVoiceSum = 6, kDDone = 7 };
void finishDones(Build& b) {
  if (!b.multi || !b.allDone || b.dones.empty()) return;
  PortRef d = b.dones.front().first;
  for (size_t i = 1; i < b.dones.size(); ++i)
    d = b.ir.atom("and", {d, b.dones[i].first}, nodeIdFor(NodeRole::Device, b.dones[i].second, kDDone));
  b.ir.output("done", kVoiceDone, d, kDone);
}
// A lowered device's output `port`. IG.1-2: a voice context's main output is its summed voices; a
// remote placeholder's is the context's output imported from its host (`in v<device>`).
PortRef outOf(Build& b, uint32_t k, uint16_t port) {
  const LDev& d = b.td[k];
  if (d.remote) {
    if (b.ctxOut[k].node == graph::kNone) {
      constexpr graph::PortType kRoute = graph::portType(graph::Kind::Infer, graph::Rate::Audio, graph::Poly::Global, graph::kWidthInfer);
      b.ctxOut[k] = b.ir.input(remotePortName(d.device->id), b.c0(), nodeIdFor(NodeRole::Device, d.device->id.value, 14), kRoute);
    }
    return b.ctxOut[k];
  }
  if (port == 0 && isContextGroup(d) && b.ctxOut[k].node != graph::kNone) return b.ctxOut[k];
  return PortRef{b.calls[k], port};
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
  if (x.node != graph::kNone && width == graph::kWidthMono && inWidth == graph::kWidthStereo)  // a mono signal into a stereo input
    x = b.ir.atom("pack", {x, x}, nodeIdFor(NodeRole::Device, id, 1), 2);
  // A stereo signal into a chain meta device (ChainAudio: its chain input takes any width, its outputs are mono — a
  // stereo signal would reach a mono `out` inside its expansion, which the compiler refuses: bug report
  // 2026-11-01_12-15-15, a Wavetable after Utility): the mid, (l + r) / 2. Mono signals pass as before.
  if (x.node != graph::kNone && haveIn && width == graph::kWidthStereo && inWidth == graph::kWidthInfer && outW == graph::kWidthMono &&
      isMetaDevice(*d.device)) {
    const PortRef l = b.ir.atom("channel", {x, b.c0()}, nodeIdFor(NodeRole::Device, id, 2));
    const PortRef r = b.ir.atom("channel", {x, b.c1()}, nodeIdFor(NodeRole::Device, id, 3));
    x = b.ir.atom("mul", {b.ir.atom("add", {l, r}, nodeIdFor(NodeRole::Device, id, 4)), b.ir.konst(0.5, kCHalf)},
                  nodeIdFor(NodeRole::Device, id, 5));
    width = graph::kWidthMono;
  }
  const graph::NodeIndex c = callDevice(b.ir, d, b.wp);
  b.calls[k] = c;
  // TMM.10-fix1: a device without an audio `in` (an unplugged chain meta device) takes nothing: the signal so far ends
  // there (its first pin is a param).
  if (haveIn && x.node != graph::kNone) b.ir.patch().connect(x, PortRef{c, static_cast<uint16_t>(inPort)});  // (IG.1-2: kPending: the default)
  x = PortRef{c, 0};
  if (outW != graph::kWidthInfer) width = outW;
  else if (inWidth == graph::kWidthStereo) width = graph::kWidthStereo;
  return x;
}

uint16_t outWidth(const graph::Patch& callee) noexcept;

PortRef playGroup(Build& b, uint32_t k, uint16_t& width);  // (below)

// IG.1-2: a meta module heading a MIDI track's path outside every voice context: what its chain input reads of the
// notes — the newest note's signal of the input's meaning (a frequency its Hz, a phase its phase, a gate its gate;
// nothing for any other input: kPending, the input keeps its default).
PortRef newestNote(Build& b, uint32_t k) {
  const LDev& d = b.td[k];
  const meta::Type* t = meta::findType(metaDeviceType(*d.device));
  const int32_t i = t != nullptr ? metaChainInput(*d.device, *t) : -1;
  if (i < 0) return kPending;
  const meta::Reads r = t->inputs[static_cast<size_t>(i)].reads;
  auto id = [&](uint32_t s) { return nodeIdFor(NodeRole::TuningNode, d.device->id.value, s); };
  PortRef s;
  if (r == meta::Reads::Frequency || r == meta::Reads::Phase) {
    s = b.tunedHz.node != graph::kNone ? b.tunedHz : b.ir.atom("voice.hz", {}, id(0));
    if (r == meta::Reads::Phase) s = b.ir.atom("phasor", {s}, id(1));
  } else if (r == meta::Reads::Gate) {
    s = b.ir.atom("gate_to_real", {b.ir.atom("voice.gate", {}, id(2))}, id(3));
  } else {
    return kPending;
  }
  return b.ir.atom("voice_last", {s}, id(4));
}
// The devices of a list from `first` on, wired left → right after `x` (declared width `width`).
PortRef chainOf(Build& b, std::span<const uint32_t> list, size_t first, PortRef x, uint16_t& width) {
  for (size_t i = first; i < list.size(); ++i) {
    const uint32_t k = list[i];
    const LDev& d = b.td[k];
    if (d.remote) {  // IG.1-2: a voice context lowered in its source's patch: its output, imported
      x = outOf(b, k, 0);
      width = graph::kWidthStereo;
      continue;
    }
    // IG.1-2: a voice context's block (an instrument group): its voices, summed; what reached it in the series is
    // dropped (its input is the notes).
    if (d.plays) x = playGroup(b, k, width);
    else if (d.notesIn) x = effectCall(b, k, newestNote(b, k), width);
    else x = effectCall(b, k, x, width);
  }
  return x;
}

// The width of a callee's first `out`.
uint16_t outWidth(const graph::Patch& callee) noexcept {
  for (graph::NodeIndex v = 0; v < callee.nodeCount(); ++v)
    if (callee.node(v).kind == graph::NodeKind::Output) return callee.inputType(v, 0).width;
  return graph::kWidthInfer;
}

// TX.scope-sound: the ordinal (among the callee's `in`s) of the input whose port drives the callee's first `out` with
// nothing in between — the callee passes that input through unchanged (the analysers: Oscilloscope, Spectrum,
// Spectrogram; a switched-off device's pass__ patch; any patch `out = in`) — or kNone.
uint32_t passThroughInput(const graph::Patch& callee) noexcept {
  NodeIndex out = graph::kNone;
  for (NodeIndex v = 0; v < callee.nodeCount() && out == graph::kNone; ++v)
    if (callee.node(v).kind == graph::NodeKind::Output) out = v;
  if (out == graph::kNone) return graph::kNone;
  NodeIndex src = graph::kNone;
  for (const graph::Wire& w : callee.wires())
    if (w.to.node == out && w.to.port == 0) {
      if (src != graph::kNone) return graph::kNone;
      src = w.from.node;
    }
  if (src == graph::kNone || callee.node(src).kind != graph::NodeKind::Input) return graph::kNone;
  uint32_t ordinal = 0;
  for (NodeIndex v = 0; v < src; ++v)
    if (callee.node(v).kind == graph::NodeKind::Input) ++ordinal;
  return ordinal;
}

// IG.1-2 (lower.h "Voice contexts"): an instrument group's voices. Its call (GroupNotes) gives the notes at In and passes
// Out per voice as `y`; Out's per-note terms are summed over the voices, its single-signal terms added once after that.
// The voices END when every ENDER is done: the device whose output alone reaches Out (followed back through pass-
// throughs: TX.scope-sound) when it decides its voices' end — a meta type marked voiceEnd (the Envelope). Without an
// ender that reaches Out, the voices get the 5 ms linear release after note-off (kMetaReleaseSeconds) on Out, and that
// decides. The enders' `done`s are wired once the members are called (finishGroups).
// The index of a callee's `out` named "done" (kNone: none).
uint32_t doneOutput(const graph::Patch& callee) noexcept {
  uint32_t o = 0;
  for (NodeIndex q = 0; q < callee.nodeCount(); ++q)
    if (callee.node(q).kind == graph::NodeKind::Output) {
      if (callee.nodeName(q) == "done") return o;
      ++o;
    }
  return graph::kNone;
}
PortRef playGroup(Build& b, uint32_t k, uint16_t& width) {
  const LDev& g = b.td[k];
  const uint32_t gid = g.device->id.value;
  const NodeIndex call = callDevice(b.ir, g, b.wp);
  b.calls[k] = call;
  const PortRef y{call, 0};
  auto tid = [&](uint32_t s) { return nodeIdFor(NodeRole::TuningNode, gid, s); };
  auto sid = [&](uint32_t s) { return nodeIdFor(NodeRole::SendInput, gid, s); };
  // The one lowered wire into an input of `dev` that `match` picks: from a device of this patch, not a note, not delayed,
  // not a single-signal term.
  auto feeder = [&](uint32_t dev, auto match) -> const LWire* {
    const LWire* one = nullptr;
    uint32_t count = 0;
    for (const uint32_t wi : b.wp->wiresInto(b.node))
      if (const LWire& w = b.wp->wires[wi]; w.toDev == dev && w.input && !w.outGlobal && match(w)) {  // (per-note terms)
        ++count;
        one = &w;
      }
    if (count != 1 || one->fromNode != b.node || one->notes != nullptr || one->delayed || one->fromTrack || one->outGlobal)
      return nullptr;
    return one;
  };
  GroupEnd end;
  end.group = k;
  // What reaches Out, through pass-throughs (TX.scope-sound). GU.9: every per-note term into Out is a branch (a rack's
  // parallel chains); the voices are ended by the enders when every branch reaches one.
  std::vector<uint32_t> lasts;
  bool allEnd = true;
  {
    std::vector<const LWire*> heads;
    for (const uint32_t wi : b.wp->wiresInto(b.node))
      if (const LWire& w = b.wp->wires[wi]; w.toDev == k && w.input && !w.outGlobal && w.wire->param == kGroupOutPort) heads.push_back(&w);
    if (heads.empty()) allEnd = false;
    for (const LWire* in : heads) {
      if (in->fromNode != b.node || in->notes != nullptr || in->delayed || in->fromTrack) in = nullptr;
      uint32_t last = graph::kNone;
      for (size_t guard = 0; in != nullptr && guard <= 2 * b.td.size() + 2; ++guard) {
        if (in->fromDev == k || in->fromPort != 0) break;  // the group's own In (the notes: nothing of its own ends them)
        const uint32_t cur = in->fromDev;
        const LDev& d = b.td[cur];
        const uint32_t pt = passThroughInput(*d.callee);
        if (pt == graph::kNone) {
          last = cur;
          break;
        }
        in = feeder(cur, [pt](const LWire& w) { return w.toPin == pt; });
      }
      if (last == graph::kNone) {
        allEnd = false;
        continue;
      }
      if (std::find(lasts.begin(), lasts.end(), last) == lasts.end()) lasts.push_back(last);
    }
  }
  bool ended = allEnd && !lasts.empty();
  width = lasts.empty() ? graph::kWidthStereo : graph::kWidthMono;  // (GroupNotes' y: declared stereo)
  for (const uint32_t last : lasts) {
    const LDev& d = b.td[last];
    const meta::Type* t = isMetaDevice(*d.device) ? meta::findType(metaDeviceType(*d.device)) : nullptr;
    const uint32_t o = doneOutput(*d.callee);
    if (o != graph::kNone && t != nullptr && t->voiceEnd) end.dones.emplace_back(last, static_cast<uint16_t>(o));
    else ended = false;
    if (outWidth(*d.callee) == graph::kWidthStereo) width = graph::kWidthStereo;
  }
  if (!ended) end.dones.clear();
  bool voiceTerms = false;
  for (const uint32_t wi : b.wp->wiresInto(b.node)) {
    const LWire& w = b.wp->wires[wi];
    if (w.toDev != k || w.toMixer || w.wire->param != kGroupOutPort) continue;
    voiceTerms = voiceTerms || !w.outGlobal;
  }
  PortRef v = y;
  if (!ended) {  // TMM.10: the declick release after note-off, and the voice's end (with nothing per note: its end only)
    const PortRef gate = b.ir.atom("voice.gate", {}, tid(0));
    const PortRef step = b.ir.atom("div", {b.ir.konst(1.0 / kMetaReleaseSeconds, kRelRate), b.ir.atom("sr", {}, tid(1))}, tid(2));
    const PortRef prev = b.ir.atom("z1", {kPending}, tid(3));
    const PortRef down = b.ir.atom("max", {b.ir.atom("sub", {prev, step}, tid(4)), b.c0()}, tid(5));
    const PortRef env = b.ir.atom("select", {gate, b.c1(), down}, tid(6));
    b.ir.patch().connect(env, PortRef{prev.node, 0});
    if (voiceTerms) v = b.ir.atom("mul", {y, env}, tid(7));
    end.ramp = b.ir.atom("and", {b.ir.atom("not", {gate}, tid(8)), b.ir.atom("le", {env, b.c0()}, tid(9))}, tid(10));
  }
  const std::span<const uint32_t> into = b.wp->wiresInto(b.node);
  const bool globals = std::any_of(into.begin(), into.end(), [&](uint32_t wi) {
    const LWire& w = b.wp->wires[wi];
    return w.toDev == k && w.outGlobal;
  });
  // (No per-note term: nothing to sum — Out is its single-signal terms, added to −0, exact.)
  PortRef x = voiceTerms ? b.ir.atom("voice_sum", {v}, sid(0)) : b.ir.konst(-0.0, kCFreeZero);
  if (globals) {
    x = b.ir.atom("add", {x, kPending}, sid(1));
    b.outGlobalPin[k] = PortRef{x.node, 1};
  }
  if (!voiceTerms && !globals) x = b.c0();
  b.ctxOut[k] = x;
  b.ends.push_back(std::move(end));
  return x;
}

// IG.1-2: every instrument group's end, once its members are called: the AND of its enders' `done`s (and its release
// ramp's), one term of the track's `done`.
void finishGroups(Build& b) {
  for (const GroupEnd& e : b.ends) {
    PortRef done = e.ramp;
    uint32_t s = 2;  // (NodeRole::SendInput slots 2 … 15 of the group: up to 14 enders)
    for (const auto& [m, o] : e.dones) {
      if (b.calls[m] == graph::kNone) continue;
      const PortRef d{b.calls[m], o};
      done = done.node == graph::kNone ? d : b.ir.atom("and", {done, d}, nodeIdFor(NodeRole::SendInput, b.td[e.group].device->id.value, s++));
    }
    voiceDone(b, done, b.td[e.group].device->id.value);
  }
}

// ---- Modulation wires in a track patch (T4.7; lower.h "Modulation wires") ---------------------------------------------

// NodeRole::Wire slots: a wire's own nodes, then (from kWTarget) the nodes of the param it modulates.
enum WireK : uint32_t {
  kWAmount, kWIn, kWExport, kWChannel, kWReduce, kWDelay, kWTerm, kWSum,
  kWNote, kWNoteReal,  // TMM.10-fix2: a note source's voice terminal, its bool → real (TX.bool-real: a bool output's)
  kWMidL, kWMidR, kWMidAdd, kWMid,  // TX.groups: a wide signal into a meta module's input: its mid
  kWOwn14,  // TX.knob-curves: an Add wire's own free slots (kWAmount, kWTerm, this) hold its site's @signed / dB nodes
  kWNotePhase,  // TX.all-free-fix1: a notes cable into a phase input: phasor of the note's Hz
  kWTarget = 16,
};
// A driven param's nodes (keyed by its first wire: kWTarget + k; GU.1: a site no wire reaches by its lowest automation
// lane (an entity id like a wire's: NodeRole::Wire, kWTarget + k)): the knob, the sum, the curve atom and the stages
// after it. (The unused slots keep the other nodes' ids.)
enum TargetK : uint32_t {
  kTKnob, kTPosA, kTPosB, kTCurve, kTAdd, kTClip, kTMapA, kTMapB, kTConv, kTRoundAdd, kTFloor,
  kTUpSub, kTUpMax, kTDownSub, kTDownMax, kTPack, kTExport,
  kTRoundMul, kTRoundStep, kTSpare19,
  // the added stage's nodes (keyed by the stage's first wire): TX.patching's units law, GU.1's add-in-units stage
  kTUAdd, kTUClip, kTURoundAdd, kTUFloor, kTUUpSub, kTUUpMax, kTUDownSub, kTUDownMax, kTUPack, kTUExport,
  kTUConv,  // GU.1: the output conversion after an add-in-units stage (db_to_gain)
};
static_assert(static_cast<uint32_t>(kWTarget) + static_cast<uint32_t>(kTUConv) < kWireNodeSlots, "a wire's nodes fit its NodeRole::Wire slots");

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
// (T4.7) or an automated one (GU.1).
struct Site {
  uint32_t dev = 0;
  std::string name;
  const graph::ParamDecl* decl = nullptr;
  std::vector<PortRef> pins;            // the inputs it feeds
  std::vector<const LWire*> wires;      // modulation into it, in id order
  uint32_t lane = 0;                    // GU.1: the lowest automation lane on it (0: none)
  bool resolved = false;
  PortRef value = kPending;
};

// The wires of plan node `node` (its outgoing cross-track wires: root exports m<wire>), then every param the patch
// builds (modulated, automated), each connected to its pins. Each modulated param's tap goes to `taps`
// (graph track `track`).
void lowerSites(Build& b, uint32_t node, uint32_t track, const WirePlan& wp, std::vector<ModulationTap>* taps,
                std::vector<CurveSite>* curves) {
  Ir& ir = b.ir;
  const TrackDevs& td = b.td;
  auto wid = [](const LWire& w, uint32_t k) { return nodeIdFor(NodeRole::Wire, w.wire->id.value, k); };
  // The source of a wire in this patch: its export's first channel (TMM.10-fix2: or the note output's voice terminal:
  // "hz" the tuned pitch, a bool as 0 / 1), reduced over the voices when it leaves them. TX.bool-real: a bool output is
  // its 0 / 1 too, converted here, before the reducer (summed as a gate is), so a wire across tracks carries a real
  // (graph::infer would convert it at a real input of this patch anyway; a bus into another unit's real `in` not).
  auto source = [&](const LWire& w) {
    PortRef s;
    auto from = [&] { return outOf(b, w.fromDev, w.fromPort); };
    if (w.fromTrack) {  // TX.all-free: the track's own signal (every channel into an input, else its first)
      s = w.input ? b.sourceX : ir.atom("channel", {b.sourceX, b.c0()}, wid(w, kWChannel));
      // Into a meta module's input: its mid, as from any wide source (below); a track's silent source (a return no send
      // reaches: the constant 0) is mono already.
      if (w.input && !w.toMixer && isMetaDevice(*td[w.toDev].device) && !td[w.toDev].bypass && !(b.sourceX == b.c0())) {
        const PortRef l = ir.atom("channel", {s, b.c0()}, wid(w, kWMidL));
        const PortRef r = ir.atom("channel", {s, b.c1()}, wid(w, kWMidR));
        s = ir.atom("mul", {ir.atom("add", {l, r}, wid(w, kWMidAdd)), ir.konst(0.5, kCHalf)}, wid(w, kWMid));
      }
    } else if (w.notes != nullptr) {
      if (w.notes->port == "hz" && b.tunedHz.node != graph::kNone) s = b.tunedHz;
      else s = ir.atom(w.notes->atom, {}, wid(w, kWNote));
      if (w.notes->boolean) s = ir.atom("gate_to_real", {s}, wid(w, kWNoteReal));
      if (w.phase) s = ir.atom("phasor", {s}, wid(w, kWNotePhase));  // TX.all-free-fix1: the note's phase
    } else if (w.toMixer) {
      s = from();  // TX.all-free-fix1: into another track's mixer: every channel
    } else if (w.input && w.fromNode == w.toNode) {
      s = from();  // TX.patching: into an input on its own track, every channel
      // TX.groups: a meta module's outputs are mono, so a signal of more channels (or of a width only inference knows:
      // a group's In terminal) reaches its input as the mid (l + r) / 2 — the chain's rule (effectCall). On a mono
      // signal the mid is the signal itself, bit for bit (x + x is exact, × 0.5 too).
      // (TX.groups-fix1: not into a switched-off device's pass-through, which passes every channel on.)
      if (isMetaDevice(*td[w.toDev].device) && !td[w.toDev].bypass &&
          declaredOutWidth(*td[w.fromDev].callee, w.fromPort) != graph::kWidthMono) {
        const PortRef l = ir.atom("channel", {s, b.c0()}, wid(w, kWMidL));
        const PortRef r = ir.atom("channel", {s, b.c1()}, wid(w, kWMidR));
        s = ir.atom("mul", {ir.atom("add", {l, r}, wid(w, kWMidAdd)), ir.konst(0.5, kCHalf)}, wid(w, kWMid));
      }
    } else {
      s = ir.atom("channel", {from(), b.c0()}, wid(w, kWChannel));
    }
    if (!w.fromTrack && w.notes == nullptr && td[w.fromDev].callee != nullptr && boolOutput(*td[w.fromDev].callee, w.fromPort))
      s = ir.atom("gate_to_real", {s}, wid(w, kWNoteReal));
    if (w.reduce) {
      s = ir.atom(reducerAtom(w.reducer), {s}, wid(w, kWReduce));
    } else if (w.broadcast) {  // IG.1-2: one value for every voice (−0 added: exact)
      s = ir.atom("add", {s, ir.konst(-0.0, kCFreeZero)}, wid(w, kWReduce));
      ir.patch().setShared(s.node, true);
    }
    return s;
  };
  for (const uint32_t wi : wp.wiresOutOf(node))
    if (const LWire& w = wp.wires[wi]; w.toNode != node) ir.exportPort("m" + std::to_string(w.wire->id.value), source(w), wid(w, kWExport));

  // A wire's signal where its target is (this patch): built here, or arriving across tracks; through a z1 when it
  // closes a delay-free loop.
  auto arriving = [&](const LWire& w) {
    constexpr graph::PortType kAnyGlobal = graph::portType(graph::Kind::Real, graph::Rate::Infer, graph::Poly::Global, graph::kWidthMono);
    const std::string id = std::to_string(w.wire->id.value);
    PortRef src = w.fromNode == node ? source(w) : ir.input("m" + id, b.c0(), wid(w, kWIn), kAnyGlobal);
    if (w.delayed) src = ir.atom("z1", {src}, wid(w, kWDelay));  // closes a loop: 1 sample (SPEC §15 2026-10-24)
    return src;
  };
  // TX.patching: wires into INPUTS (model.h "Modulation wires", the Add law): the input reads what it read before (the
  // chain's signal, when the chain feeds it) plus every wire into it, in id order; an input nothing drove takes the
  // wires alone (a free device's placeholder zero goes).
  {
    // PF.6: the node's input wires grouped by target (device, pin) in id order, and the single-signal terms by device
    // (one pass, not a scan of the node's wires per input).
    const std::span<const uint32_t> into = wp.wiresInto(node);  // this node's wires only (id order)
    std::unordered_map<uint64_t, std::vector<uint32_t>> byPin;  // (device, pin) → positions in `into`
    std::unordered_map<uint32_t, std::vector<uint32_t>> globalsTo;  // device → positions of its outGlobal wires
    for (uint32_t i = 0; i < into.size(); ++i) {
      const LWire& w = wp.wires[into[i]];
      if (w.outGlobal) globalsTo[w.toDev].push_back(i);
      else if (w.input && !w.toMixer) byPin[(static_cast<uint64_t>(w.toDev) << 16) | w.toPin].push_back(i);
    }
    std::vector<std::pair<uint32_t, uint16_t>> done;  // (device, pin) of the single-signal sums made
    // PF.6: the patch's wires by target pin, made at the first input (instead of a scan of the patch per input). Wires
    // added below go to new nodes or to inputs already summed, so the index stays exact for the inputs still to come.
    bool indexed = false;
    std::unordered_map<uint64_t, std::vector<graph::WireIndex>> wiresAt;
    auto pinKey = [](PortRef r) { return (static_cast<uint64_t>(r.node) << 16) | r.port; };
    for (uint32_t i = 0; i < into.size(); ++i) {
      const LWire& f = wp.wires[into[i]];
      if (!f.input || f.toMixer) continue;  // (TX.all-free: the mixer's wires are buildPatch's)
      const std::vector<uint32_t>* group = nullptr;  // the (device, pin)'s wires, when `f` is its first
      if (f.outGlobal) {
        const std::pair<uint32_t, uint16_t> key{f.toDev, f.toPin};
        if (std::find(done.begin(), done.end(), key) != done.end()) continue;
        done.push_back(key);
      } else {
        group = &byPin.at((static_cast<uint64_t>(f.toDev) << 16) | f.toPin);
        if (group->front() != i) continue;  // summed at its first wire
      }
      const PortRef pin{b.calls[f.toDev], f.toPin};
      // IG.1-2: the single-signal terms into an instrument group's Out: added once after the voices' sum (playGroup).
      if (f.outGlobal) {
        PortRef gsum = kPending;
        bool gfirst = true;
        for (const uint32_t j : globalsTo.at(f.toDev)) {
          if (j < i) continue;
          const LWire& w = wp.wires[into[j]];
          const PortRef src = arriving(w);
          gsum = gsum.node == graph::kNone ? src : ir.atom("add", {gsum, src}, wid(w, gfirst ? kWTerm : kWSum));
          gfirst = false;
        }
        if (b.outGlobalPin[f.toDev].node != graph::kNone) ir.patch().connect(gsum, b.outGlobalPin[f.toDev]);
        continue;
      }
      if (pin.node == graph::kNone) continue;
      graph::Patch& p = ir.patch();
      PortRef sum = kPending;
      // The pin's first wire (the lowest index), taken off: what the input read before.
      if (!indexed) {
        indexed = true;
        for (graph::WireIndex x = 0; x < p.wireCount(); ++x) wiresAt[pinKey(p.wire(x).to)].push_back(x);
      }
      if (const auto at = wiresAt.find(pinKey(pin)); at != wiresAt.end() && !at->second.empty()) {
        auto first = std::min_element(at->second.begin(), at->second.end());
        const graph::WireIndex x = *first;
        const graph::WireIndex last = p.wireCount() - 1;
        const PortRef moved = p.wire(last).to;
        const PortRef from = p.wire(x).from;
        p.disconnect(x);  // (the last wire takes its index)
        at->second.erase(first);
        if (last != x)
          if (const auto m = wiresAt.find(pinKey(moved)); m != wiresAt.end())
            std::replace(m->second.begin(), m->second.end(), last, x);
        if (!(from == b.freeZero)) sum = from;
      }
      // TX.groups-fix1: chain links first (Wire::chain: a grouped series' link is what the chain gave the input), then
      // the other wires in id order.
      bool first = true;
      for (const bool chainPass : {true, false})
        for (const uint32_t j : *group) {
          const LWire& w = wp.wires[into[j]];
          if (w.wire->chain != chainPass) continue;
          const PortRef src = arriving(w);
          sum = sum.node == graph::kNone ? src : ir.atom("add", {sum, src}, wid(w, first ? kWTerm : kWSum));
          first = false;
        }
      if (pin.node != graph::kNone) p.connect(sum, pin);
    }
  }
  // The sites: the modulated params first, in the order of their first wire (T4.7), then the automated ones.
  std::vector<Site> sites;
  auto siteOf = [&](uint32_t dev, std::string_view name) -> Site& {
    for (Site& s : sites)
      if (s.dev == dev && s.name == name) return s;
    Site s;
    s.dev = dev;
    s.name = std::string(name);
    const LDev& d = td[dev];
    const ParamPin pp = *realParamPin(*d.callee, name);
    s.decl = &d.callee->paramDecl(pp.node);
    if (b.calls[dev] != graph::kNone) s.pins.push_back(PortRef{b.calls[dev], pp.pin});
    if (b.pl != nullptr) s.lane = b.pl->laneOf(d.device->id, name);
    sites.push_back(std::move(s));
    return sites.back();
  };
  sites.reserve(wp.wiresInto(node).size());
  for (const uint32_t wi : wp.wiresInto(node)) {
    const LWire& w = wp.wires[wi];
    if (w.input) continue;  // TX.patching: inputs were summed above
    siteOf(w.toDev, td[w.toDev].callee->nodeName(w.paramNode)).wires.push_back(&w);
  }
  // GU.1: every real param of a lowered device that an automation lane drives (its curve atom after the knob).
  if (b.pl != nullptr && !b.pl->laned.empty())
    for (uint32_t k = 0; k < td.size(); ++k) {
      const LDev& d = td[k];
      if (d.callee == nullptr || d.remote || d.bypass || b.calls[k] == graph::kNone) continue;
      const auto it = b.pl->laned.find(d.device->id.value);
      if (it == b.pl->laned.end()) continue;
      for (const auto& [name, lane] : it->second)
        if (realParamPin(*d.callee, name)) (void)siteOf(k, name);
    }
  // A site's value.
  auto compute = [&](size_t si) -> PortRef {
    if (sites[si].resolved) return sites[si].value;
    const uint32_t dev = sites[si].dev;
    const std::string name = sites[si].name;
    const graph::ParamDecl& decl = *sites[si].decl;
    const LDev& d = td[dev];
    const std::string devId = std::to_string(d.device->id.value);
    // Stages 2 and 3: value = clamp(centre + Σ signals, lo, hi), rounded to `step` (step 1: floor(x + 0.5), T4.7's
    // rounding); the tap is how far up / down the wires moved it (zero exactly when they do not move it).
    PortRef v;
    auto addStage = [&](const std::vector<const LWire*>& list, double lo, double hi, bool clamp, double step,
                        bool tapIt, ModulationTap::Domain domain) {
      auto tid = [&](uint32_t k) { return wid(*list.front(), kWTarget + k); };
      const PortRef centre = v;
      PortRef sum = kPending;
      for (size_t i = 0; i < list.size(); ++i) {
        const PortRef src = arriving(*list[i]);
        sum = i == 0 ? src : ir.atom("add", {sum, src}, wid(*list[i], kWSum));
      }
      PortRef x = ir.atom("add", {centre, sum}, tid(kTUAdd));
      if (clamp) x = ir.atom("clip", {x, ir.literal(lo), ir.literal(hi)}, tid(kTUClip));
      if (step == 1.0) {
        x = ir.atom("floor", {ir.atom("add", {x, ir.konst(0.5, kCHalf)}, tid(kTURoundAdd))}, tid(kTUFloor));
      } else if (step > 0.0) {
        const PortRef q = ir.atom("floor", {ir.atom("add", {ir.atom("mul", {x, ir.literal(1.0 / step)}, tid(kTRoundStep)),
                                                            ir.konst(0.5, kCHalf)},
                                                   tid(kTURoundAdd))},
                                  tid(kTUFloor));
        x = ir.atom("mul", {q, ir.literal(step)}, tid(kTRoundMul));
      }
      if (tapIt) {
        const PortRef up = ir.atom("max", {ir.atom("sub", {x, centre}, tid(kTUUpSub)), b.c0()}, tid(kTUUpMax));
        const PortRef down = ir.atom("max", {ir.atom("sub", {centre, x}, tid(kTUDownSub)), b.c0()}, tid(kTUDownMax));
        const std::string port = tapPort(d.device->id, name);
        ir.exportPort(port, ir.atom("pack", {up, down}, tid(kTUPack), 2), tid(kTUExport));
        if (taps != nullptr) taps->push_back(ModulationTap{d.device->id, name, track, port, domain});
      }
      v = x;
    };
    // GU.1 (lower.h "Modulation wires"): a control. Stage 1 in knob position: the Travel wires (amount · source) and,
    // on a curve that is not linear, the Add wires (+1 = the whole travel); the `curve` atom; stage 2 on a linear
    // curve: the Add wires in the curve's units, then its output conversion; stage 3: the migrated Units wires.
    const dsp::Curve curve = controlCurve(*d.device, name, decl);
    const bool inUnits = addsInUnits(curve);
    std::vector<const LWire*> wires, added, units;
    for (const LWire* w : sites[si].wires)
      (w->wire->travels() ? wires : w->wire->law == WireLaw::Units ? units : inUnits ? added : wires).push_back(w);
    // The site's own nodes: keyed by its first wire, else its automation lane.
    const LWire* first = !sites[si].wires.empty() ? sites[si].wires.front() : nullptr;
    auto sid = [&](uint32_t k) {
      if (first != nullptr) return wid(*first, kWTarget + k);
      return nodeIdFor(NodeRole::Wire, sites[si].lane, kWTarget + k);
    };
    // the knob position (param rate); its default the declared default's position
    const PortRef pk = ir.param("d" + devId + "__" + name, dsp::curvePosition(declaredCurve(decl), decl.defaultValue.value(), 0.0), 0.0,
                    1.0, sid(kTKnob));
    PortRef p = pk;
    if (!wires.empty()) {
      PortRef sum;
      for (size_t i = 0; i < wires.size(); ++i) {
        const LWire& w = *wires[i];
        PortRef term;
        if (w.wire->travels()) {
          const std::string id = std::to_string(w.wire->id.value);
          const PortRef amt = ir.param("w" + id, w.wire->amount, -1.0, 1.0, wid(w, kWAmount));
          term = ir.atom("mul", {amt, arriving(w)}, wid(w, kWTerm));
        } else {
          term = arriving(w);  // an Add wire into a curved control: its signal is the position it adds
        }
        sum = i == 0 ? term : ir.atom("add", {sum, term}, wid(w, kWSum));
      }
      p = ir.atom("add", {pk, sum}, sid(kTAdd));
    }
    // (Without LowerOptions::curveSlotOf the lowering numbers its driven controls 1, 2, … itself: graph.curveSlots.)
    const uint32_t slot = b.o.curveSlotOf  ? b.o.curveSlotOf(d.device->id, name)
                          : curves != nullptr ? static_cast<uint32_t>(curves->size() + 1)
                                              : 0u;
    const size_t siteIndex = curves != nullptr ? curves->size() : 0;
    if (curves != nullptr) curves->push_back(CurveSite{b.track != nullptr ? b.track->id : TrackId{}, d.device->id, name, slot, curve});
    const bool convertLater = !added.empty() && curve.output != dsp::CurveOutput::None;
    v = ir.atom("curve", {p, ir.literal(convertLater ? 0.0 : 1.0), ir.literal(static_cast<double>(slot))}, sid(kTCurve));
    if (!wires.empty() && added.empty() && units.empty()) {  // the tap in position (the views clamp the ring)
      const PortRef up = ir.atom("max", {ir.atom("sub", {p, pk}, sid(kTUpSub)), b.c0()}, sid(kTUpMax));
      const PortRef down = ir.atom("max", {ir.atom("sub", {pk, p}, sid(kTDownSub)), b.c0()}, sid(kTDownMax));
      const std::string port = tapPort(d.device->id, name);
      ir.exportPort(port, ir.atom("pack", {up, down}, sid(kTPack), 2), sid(kTExport));
      if (taps != nullptr) taps->push_back(ModulationTap{d.device->id, name, track, port, ModulationTap::Domain::Position});
    }
    if (!added.empty()) {
      double lo = std::min(curve.min, curve.max), hi = std::max(curve.min, curve.max);
      if (curve.mode == dsp::CurveMode::Signed) {
        hi = std::max(std::fabs(lo), std::fabs(hi));
        lo = -hi;
      }
      addStage(added, lo, hi, curve.mode != dsp::CurveMode::Wrap, curve.step, units.empty(), ModulationTap::Domain::Curve);
      if (convertLater) v = ir.atom("db_to_gain", {v}, wid(*added.front(), kWTarget + static_cast<uint32_t>(kTUConv)));
    }
    if (!units.empty()) {
      double lo = 0.0, hi = 1.0;
      const bool ranged = paramRangeOf(decl, lo, hi);
      if (ranged && paramSigned(decl)) {
        hi = std::max(std::fabs(lo), std::fabs(hi));
        lo = -hi;
      }
      addStage(units, lo, hi, ranged && !paramWraps(decl), curve.step, true, ModulationTap::Domain::Dsp);
    }
    if (curves != nullptr) {  // TX.mod-bands: the final value (the response displays' set port)
      (*curves)[siteIndex].value = ir.idOf(v.node);
      (*curves)[siteIndex].valuePort = v.port;
    }
    sites[si].resolved = true;
    sites[si].value = v;
    return v;
  };
  for (size_t i = 0; i < sites.size(); ++i) {
    const PortRef v = compute(i);
    for (const PortRef& pin : sites[i].pins) ir.patch().connect(v, pin);
  }
  // GU.f4 (lower.h "Structural value blocks"): an undriven stepped (or constant) value block choosing a branch is its value's literal
  // (the float the runtime param would hold), so the optimiser folds the choice.
  if (b.pl != nullptr && !b.pl->literal.empty()) {
    std::vector<std::pair<double, PortRef>> made;  // one literal node per value (the per-track literal pool is finite)
    for (uint32_t k = 0; k < td.size(); ++k) {
      const LDev& d = td[k];
      if (d.callee == nullptr || d.remote || d.bypass || b.calls[k] == graph::kNone || !b.pl->isLiteral(d.device->id)) continue;
      const auto pp = realParamPin(*d.callee, kValueParam);
      if (!pp) continue;
      const double v = static_cast<float>(paramValue(*d.device, kValueParam, d.callee->paramDecl(pp->node)));
      auto it = std::find_if(made.begin(), made.end(), [&](const auto& m) { return std::bit_cast<uint64_t>(m.first) == std::bit_cast<uint64_t>(v); });
      if (it == made.end()) it = made.insert(made.end(), {v, ir.literal(v)});
      ir.patch().connect(it->second, PortRef{b.calls[k], pp->pin});
    }
  }
}


// IG.1-2: the polyphony and voice stealing of the voice contexts a track's patch holds (model.h "Instrument groups"):
// the largest voice count (an instrument group's `voices` setting — 1 when mono —; 0: the link's default) and the first
// group's stealing.
void contextPolyphony(const TrackDevs& devs, const Callees* lib, uint32_t& polyphony, int32_t& steal) {
  (void)lib;
  polyphony = 0;
  steal = -1;
  for (const uint32_t k : devs.contexts) {
    const LDev& c = devs[k];
    if (!isContextGroup(c)) continue;
    const GroupPolyphony gp = groupPolyphony(*c.device);
    polyphony = std::max(polyphony, gp.mono ? 1u : gp.voices);
    if (steal < 0 && c.device->setting(kGroupSteal) != nullptr) steal = static_cast<int32_t>(gp.steal);
  }
}

// GU.7 (panel.h "Displays tapped by the lowering"): the taps of the displays placed on this patch's groups' panels, on
// the outputs they show. Node ids: the tapped device's TuningNode / SendInput spaces (a device id is never a tuning or
// track id; a rack, which uses them, is never tapped): a signal tap of output p < 2 takes TuningNode k = 6p … 6p + 5,
// a voice tap of output p < 16 SendInput k = p. Without metaSlotOf (tools, tests) or a slot: none.
void panelTapsOf(Build& b) {
  if (b.resources == nullptr || !b.o.metaSlotOf) return;
  std::vector<PanelTap> want;
  for (size_t k = 0; k < b.td.size(); ++k) {
    const Device& g = *b.td[k].device;
    if (!isGroupDevice(g) || !g.hasPanel() || b.td[k].remote) continue;
    for (PanelTap& t : panelTaps(*g.panel)) want.push_back(std::move(t));
  }
  if (want.empty()) return;
  std::sort(want.begin(), want.end(), [](const PanelTap& x, const PanelTap& y) {
    return std::tie(x.device, x.kind, x.port) < std::tie(y.device, y.kind, y.port);
  });
  want.erase(std::unique(want.begin(), want.end()), want.end());
  std::unordered_map<uint32_t, uint32_t> at;
  for (uint32_t k = 0; k < b.td.size(); ++k) at.emplace(b.td[k].device->id.value, k);
  for (const PanelTap& t : want) {
    const auto it = at.find(t.device.value);
    if (it == at.end()) continue;
    const uint32_t k = it->second;
    const LDev& d = b.td[k];
    if (d.remote || d.callee == nullptr || b.calls[k] == graph::kNone) continue;
    const uint32_t port = signalOutput(*d.callee, t.port);
    if (port == graph::kNone || (t.kind == ElementTap::Signal ? port >= 2 : port >= kEntityNodeSlots)) continue;
    const PortRef x = outOf(b, k, static_cast<uint16_t>(port));
    if (x.node == graph::kNone) continue;
    const std::string path = tapPath(t.kind, t.port);
    const uint32_t slot = b.o.metaSlotOf(t.device, path, 0);
    if (slot == 0) continue;
    const uint32_t id = t.device.value;
    const PortRef sl = b.ir.literal(static_cast<double>(slot));
    if (t.kind == ElementTap::Voice) {
      b.ir.atom("voice_tap", {x, sl}, nodeIdFor(NodeRole::SendInput, id, port));
    } else {
      auto nid = [&](uint32_t j) { return nodeIdFor(NodeRole::TuningNode, id, 6 * port + j); };
      const PortRef l = b.ir.atom("channel", {x, b.c0()}, nid(0)), r = b.ir.atom("channel", {x, b.c1()}, nid(1));
      const PortRef beat = b.ir.atom("transport.beat", {}, nid(2));
      // The note-locked timebase: the lowest sounding voice's pitch where the signal runs per voice.
      const PortRef note = d.voice ? b.ir.atom("voice_lowest", {b.ir.atom("voice.hz", {}, nid(3))}, nid(4)) : b.c0();
      b.ir.atom("signal_tap", {beat, note, l, r, sl}, nid(5));
    }
    LoweredProject::MetaResource m;
    m.track = b.track != nullptr ? b.track->id : TrackId{};
    m.device = t.device;
    m.path = path;
    m.request = meta::ResourceRequest{t.kind == ElementTap::Voice ? meta::ResourceKind::VoiceTap : meta::ResourceKind::SignalTap, 0};
    m.slot = slot;
    b.resources->push_back(std::move(m));
  }
}

// ---- Oversampled groups (TX.group-oversample; lower.h "Oversampled groups") ------------------------------------------
// Per device inside an oversampled group (at any depth) and per oversampled group itself: the OUTERMOST oversampled
// group holding it (its region) and that region's factor, the largest of the nest (regions do not nest: an inner
// group asking for less is already covered, one asking for more raises the whole region).
struct OsRegion {
  uint32_t group = 0;   // the outermost oversampled group (device id)
  uint32_t factor = 1;  // the region's factor
};
using OsRegions = std::unordered_map<uint32_t, OsRegion>;
OsRegions oversampledGroups(const Project& p) {
  OsRegions out;
  forEachTrack(p, [&](const Track& t, const Track*) {
    std::unordered_map<uint32_t, const Device*> byId;
    for (const DevicePtr& d : t.devices) byId.emplace(d->id.value, d.get());
    auto factorOf = [](const Device& g) { return g.active ? groupOversampleFactor(g) : 1u; };
    std::unordered_map<uint32_t, uint32_t> outer;  // device → its outermost oversampled group (self included)
    for (const DevicePtr& d : t.devices) {
      uint32_t o = 0;
      const Device* cur = d.get();
      for (uint32_t step = 0; cur != nullptr && step <= kMaxGroupDepth; ++step) {
        if (factorOf(*cur) > 1) o = cur->id.value;
        if (!cur->group.valid()) break;
        const auto it = byId.find(cur->group.value);
        cur = it != byId.end() ? it->second : nullptr;
      }
      if (o != 0) outer.emplace(d->id.value, o);
    }
    std::unordered_map<uint32_t, uint32_t> factor;  // outermost group → the largest factor of its nest
    for (const auto& [d, o] : outer) {
      uint32_t& f = factor[o];
      f = std::max(f, factorOf(*byId.at(d)));
    }
    for (const auto& [d, o] : outer) out[d] = OsRegion{o, factor[o]};
  });
  return out;
}

// Whether a callee computes anything in an oversample region of its own (at any depth of its calls): it cannot be
// called inside another region (PATCH_LANGUAGE §2.3), so a device playing it stays outside its group's region and runs
// at its own rate (the Wavetable's live path, its Shape mode).
bool calleeHasRegion(const graph::Patch& callee, const graph::PatchLibrary* lib, uint32_t depth = 0) {
  for (const graph::OversampleRegion& r : callee.regions())
    if (r.factor > 1 || !r.factorFrom.empty()) return true;
  if (lib == nullptr || depth > 32) return false;
  for (NodeIndex n = 0; n < callee.nodeCount(); ++n) {
    if (callee.node(n).kind != graph::NodeKind::Subcircuit) continue;
    const graph::Patch* c = lib->find(callee.subcircuit(n).patch);
    if (c != nullptr && c != &callee && calleeHasRegion(*c, lib, depth + 1)) return true;
  }
  return false;
}

// Puts the nodes of the devices inside oversampled groups into their regions (one OversampleRegion per outermost
// oversampled group lowered in this patch): a device's own nodes (NodeRole::Device, k 0 … 5: its call and its input
// adapters; a nested group's call too, but not the outermost group's: its terminals are the region's edge, so its In
// and Out — what the response displays probe — are base-rate values), the nodes of a wire whose
// both ends are in one region, and the nodes of a modulated param of a device inside (k ≥ 16: they take the target's
// region, so a loop through a delayed wire between members stays inside). Everything else — a group's voice sum,
// release ramp and ends (other roles), taps, sends, mixer stages — runs at the base rate; infer() puts the converters
// on every audio-rate wire across (params and constants pass as they are).
void assignOversampleRegions(graph::Patch& p, const TrackDevs& devs, const WirePlan& wp, const OsRegions& os,
                             const Callees* lib) {
  if (os.empty()) return;
  // Device id → its outermost oversampled group (absent: outside every region — the outermost group itself, whose
  // terminals are the region's edge, or a callee with a region of its own).
  std::unordered_map<uint32_t, uint32_t> devRegion;
  std::unordered_map<uint32_t, std::string> notePhase;  // outermost instrument group → its In terminal's note phase
  for (const LDev& d : devs.devs) {
    const auto it = os.find(d.device->id.value);
    if (it == os.end() || d.remote || d.callee == nullptr) continue;
    if (it->second.group == d.device->id.value) {
      // The In terminal at the note source gives the note's phase (groupNotesPatch: `phase` = phasor(voice.hz)),
      // computed outside: it enters by phase interpolation, never through the half-bands.
      if (d.callee == &groupNotesPatch()) notePhase.emplace(d.device->id.value, "d" + std::to_string(d.device->id.value) + ".phase");
      continue;
    }
    if (calleeHasRegion(*d.callee, lib != nullptr ? lib->lib : nullptr)) continue;
    devRegion.emplace(d.device->id.value, it->second.group);
  }
  if (devRegion.empty()) return;
  auto regionOf = [&](DeviceId d) -> uint32_t {
    const auto it = devRegion.find(d.value);
    return it != devRegion.end() ? it->second : 0u;
  };
  // Wire id → (region of from, region of to). A notes cable into a phase input computes the note's phase on the wire
  // (phasor of its Hz): into a region it is computed inside (a phase must not cross through the half-bands).
  std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> wireEnds;
  for (const LWire& w : wp.wires) {
    if (w.wire == nullptr) continue;
    const uint32_t to = w.toMixer ? 0u : regionOf(w.wire->to);
    wireEnds.emplace(w.wire->id.value, std::pair{w.phase ? to : regionOf(w.wire->from), to});
  }
  std::unordered_map<uint32_t, uint16_t> regionId;  // outermost group → region id of this patch
  auto region = [&](uint32_t group) -> uint16_t {
    if (const auto it = regionId.find(group); it != regionId.end()) return it->second;
    graph::OversampleRegion r;
    r.factor = os.at(group).factor;
    if (const auto ph = notePhase.find(group); ph != notePhase.end()) r.phase.push_back(ph->second);
    const uint16_t id = p.addRegion(std::move(r));
    regionId.emplace(group, id);
    return id;
  };
  for (NodeIndex n = 0; n < p.nodeCount(); ++n) {
    const graph::NodeKind kind = p.node(n).kind;
    if (kind != graph::NodeKind::Atom && kind != graph::NodeKind::Const && kind != graph::NodeKind::Subcircuit) continue;
    const uint32_t id = p.node(n).id.value;
    if (id < kEntityNodeBase) continue;
    const uint32_t rel = id - kEntityNodeBase, q = rel / kEntityNodeSlots, k = rel % kEntityNodeSlots;
    const uint32_t entity = q / 3, role = q % 3;
    uint32_t g = 0;
    if (const auto it = wireEnds.find(entity); it != wireEnds.end()) {
      const uint32_t slot = role * kEntityNodeSlots + k;  // NodeRole::Wire spans the three role spaces
      if (slot >= kEntityNodeSlots) g = it->second.second;                     // the modulated param's nodes
      else if (it->second.first == it->second.second) g = it->second.first;  // the wire's own nodes
    } else if (role == static_cast<uint32_t>(NodeRole::Device) && k <= 5) {
      g = regionOf(DeviceId{entity});
    }
    if (g != 0) p.setNodeRegion(n, region(g));
  }
}

Result<graph::Patch> buildPatch(const Plan& pl, uint32_t index, const LowerOptions& o, const TrackDevs& devs,
                                const WirePlan& wp, bool& voices, uint32_t& polyphony, int32_t& steal, uint32_t track,
                                std::vector<ModulationTap>* taps, uint32_t clipSlot,
                                std::vector<LoweredProject::MetaResource>* resources, const Callees* lib,
                                std::vector<CurveSite>* curves, const OsRegions* os) {
  const Node& n = pl.nodes[index];
  Ir ir(patchName(n));
  Build b{ir, o, devs, std::vector<NodeIndex>(devs.size(), graph::kNone), {}};
  b.lib = lib;
  b.wp = &wp;
  b.pl = &pl;  // GU.1
  b.node = index;
  b.track = n.track;  // T4.18
  b.resources = resources;  // TX.spectrum
  b.ctxOut.assign(devs.size(), kPending);  // IG.1-2
  b.outGlobalPin.assign(devs.size(), kPending);
  b.multi = devs.contexts.size() > 1;
  PortRef x;
  voices = n.kind == Kind::Midi && devs.voices;
  contextPolyphony(devs, lib, polyphony, steal);
  if (!voices) {
    polyphony = 0;
    steal = -1;
  }
  // The tuning nodes (when the track follows tunings): before every voice consumer.
  auto tuningNodes = [&] {
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
      b.tunedHz = hz;  // TMM.10-fix2: what a note source's "hz" output reads (the root's own voice.hz is the engine's)
    }
  };
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
  } else if (n.kind == Kind::Midi) {
    // IG.1-2 (lower.h "Voice contexts"): the path starts at the notes — a voice context at its head plays them (its
    // output from there on), a meta module there reads the newest note; anything else hears silence.
    if (voices) tuningNodes();
    x = ir.konst(0.0, kC0);
    width = graph::kWidthMono;
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
  if (n.kind != Kind::Midi) b.sourceX = x;  // TX.all-free
  x = chainOf(b, devs.top, 0, x, width);
  // TX.patching: the free devices (out of the series): called, the audio input the chain would feed (the first
  // non-const `in`) reading zero until a wire reaches it, the other inputs their defaults; their outputs feed only
  // wires (unread, the compiler drops them).
  for (const uint32_t k : devs.offPath) {
    if (isContextGroup(devs[k])) {  // IG.1-2: an instrument group off the path (read by its wires)
      uint16_t w = graph::kWidthStereo;
      (void)playGroup(b, k, w);
      continue;
    }
    const NodeIndex c = callDevice(ir, devs[k], b.wp);
    b.calls[k] = c;
    // (A free meta device plays its Nested form: every input has its default already.)
    const bool own = isMetaDevice(*devs[k].device) && !devs[k].bypass;
    if (const uint32_t in = own ? graph::kNone : audioInIndex(*devs[k].callee);
        in != graph::kNone) {
      if (b.freeZero.node == graph::kNone) b.freeZero = ir.konst(-0.0, kCFreeZero);
      ir.patch().connect(b.freeZero, PortRef{c, static_cast<uint16_t>(in)});
    }
  }
  finishGroups(b);  // IG.1-2: the instrument groups' ends, their members called
  // IG.1-2: the contexts lowered here for another track's path send it their output (`export v<device>`).
  for (const WirePlan::Remote& r : wp.remotes) {
    if (r.host != index) continue;
    const LDev& d = devs[r.hostDev];
    PortRef y = b.ctxOut[r.hostDev];
    if (y.node == graph::kNone && b.calls[r.hostDev] != graph::kNone)
      y = ir.atom("voice_sum", {PortRef{b.calls[r.hostDev], 0}}, nodeIdFor(NodeRole::Device, d.device->id.value, kDVoiceSum));
    if (y.node == graph::kNone) y = b.c0();
    ir.exportPort(remotePortName(d.device->id), y, nodeIdFor(NodeRole::Device, d.device->id.value, 15));
  }
  finishDones(b);  // several voice contexts: the AND of their ends
  // TX.all-free (lower.h "The main path"): the mixer hears the main path's end when the path reaches it, plus every other
  // wire into it (in id order: a block's output with all its channels, a voice signal through the wire's reducer, the
  // track's own signal; through a z1 when it closes a loop). Nothing: silence.
  {
    PortRef y = devs.complete ? x : kPending;
    for (const uint32_t wi : wp.wiresInto(index)) {
      const LWire& w = wp.wires[wi];
      if (!w.toMixer) continue;
      auto wid = [&](uint32_t k) { return nodeIdFor(NodeRole::Wire, w.wire->id.value, k); };
      PortRef src;
      if (w.fromNode != index) {  // TX.all-free-fix1: from another track (a send-like route): every channel, summed voices
        constexpr graph::PortType kRoute = graph::portType(graph::Kind::Infer, graph::Rate::Audio, graph::Poly::Global, graph::kWidthInfer);
        src = ir.input("m" + std::to_string(w.wire->id.value), b.c0(), wid(kWIn), kRoute);
      } else {
        src = w.fromTrack ? b.sourceX : outOf(b, w.fromDev, w.fromPort);
        if (src.node == graph::kNone) continue;
        if (w.reduce) src = ir.atom(reducerAtom(w.reducer), {src}, wid(kWReduce));
      }
      if (w.delayed) src = ir.atom("z1", {src}, wid(kWDelay));
      y = y.node == graph::kNone ? src : ir.atom("add", {y, src}, wid(kWSum));
    }
    if (y.node == graph::kNone) y = ir.konst(0.0, kC0);
    x = y;
  }
  gainStages(ir, n.kind, n.track->mixer.panMode, x, o.smoothingMs);
  panelTapsOf(b);  // GU.7
  lowerSites(b, index, track, wp, taps, curves);  // T4.7; GU.1
  if (ir.literalOverflow())
    return Error{ErrorCode::Overflow, "lower: track " + std::to_string(n.track->id.value) + ": more than " +
                                          std::to_string(kEntityNodeBase - kLiteralPool) + " device settings", {}};
  if (!ir.finish())
    return Error{ErrorCode::Overflow, "lower: track " + std::to_string(n.track->id.value) +
                                          ": a document id too large for a stable node id (nodeIdFor)", {}};
  if (os != nullptr) assignOversampleRegions(ir.patch(), devs, wp, *os, lib);  // TX.group-oversample
  return std::move(ir.patch());
}
// ---- Rack chain params (T4.8): what the document derives for the lowered patch -------------------------------------

// GU.1: whether `dev`.`name` (a real param of a lowered device) is DRIVEN — a lowered wire into the
// param itself or an automation lane on it — so that its patch has its `curve` atom and its runtime param is the knob
// position (lowerSites' rule; lower.h "Modulation wires").
bool paramDriven(const Plan& pl, const TrackDevs& td, uint32_t dev, std::string_view name, const WirePlan& wp, uint32_t node) {
  const LDev& d = td[dev];
  if (d.callee == nullptr) return false;
  if (pl.laneOf(d.device->id, name) != 0) return true;
  for (const uint32_t wi : wp.wiresInto(node))
    if (const LWire& w = wp.wires[wi]; w.toDev == dev && !w.input && d.callee->nodeName(w.paramNode) == name) return true;
  return false;
}

void addParams(const Plan& pl, uint32_t index, const AudibilityMap& aud, std::vector<LoweredParam>& out,
               const Callees* lib, const WirePlan& wp, const TrackDevs& td) {
  const Node& n = pl.nodes[index];
  const Track& t = *n.track;
  const std::string id = std::to_string(t.id.value);
  // T4.7: the amount of every wire into this track (a param of the target's patch).
  for (const uint32_t wi : wp.wiresInto(index))
    if (const LWire& w = wp.wires[wi]; w.wire->travels()) {  // TX.patching: an added wire has no amount
      const std::string wid = std::to_string(w.wire->id.value);
      out.push_back(LoweredParam{"wire" + wid + ":amount", index, "w" + wid, static_cast<float>(w.wire->amount),
                                 ParamRef::wireAmount(w.wire->id)});
    }
  // T4.1: every param of every lowered device (unconnected at the call: d<id>__<name> in the track patch).
  if (lib != nullptr) {
    for (uint32_t k = 0; k < td.size(); ++k) {
      const LDev& d = td[k];
      const std::string dev = std::to_string(d.device->id.value);
      if (d.remote) continue;  // IG.1-2: lowered (with its params) in its host's patch
      for (graph::NodeIndex v = 0; v < d.callee->nodeCount(); ++v) {
        if (d.callee->node(v).kind != graph::NodeKind::Param) continue;
        const std::string& name = d.callee->nodeName(v);
        // GU.1: a driven control's knob position (its curve atom maps it), else its value: the curve at the position,
        // evaluated here (lower.h "Modulation wires": an undriven control has no op).
        const graph::ParamDecl& decl = d.callee->paramDecl(v);
        if (!d.bypass && pl.isLiteral(d.device->id)) continue;  // GU.f4: a literal of the patch, no runtime param
        const bool driven = !d.bypass && paramDriven(pl, td, k, name, wp, index);
        const double value = driven ? paramPosition(*d.device, name, decl) : paramValue(*d.device, name, decl);
        out.push_back(LoweredParam{"dev" + dev + ":" + name + (driven ? std::string(kPositionKeySuffix) : std::string()), index,
                                   "d" + dev + "__" + name, static_cast<float>(value), ParamRef::device(d.device->id, name),
                                   driven});
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
  if (role == NodeRole::Wire) {  // a wire id is no other entity's id: its slots span the three role spaces
    if (k >= kWireNodeSlots) return 0u;
    r = k / kEntityNodeSlots;
    k %= kEntityNodeSlots;
  }
  const uint64_t id = kEntityNodeBase + (static_cast<uint64_t>(entity) * 3 + r) * kEntityNodeSlots + k;
  return id <= 0xFFFFFFFFull && k < kEntityNodeSlots ? static_cast<uint32_t>(id) : 0u;
}

dsp::Curve controlCurve(const Device& device, std::string_view param, const graph::ParamDecl& decl) noexcept {
  if (const dsp::Curve* c = device.curve(param)) return *c;
  return declaredCurve(decl);
}

double paramPosition(const Device& device, std::string_view param, const graph::ParamDecl& decl) noexcept {
  if (const DeviceValue* v = device.param(param)) return v->value;
  return dsp::curvePosition(declaredCurve(decl), decl.defaultValue.value(), 0.0);
}

double paramValue(const Device& device, std::string_view param, const graph::ParamDecl& decl) noexcept {
  return dsp::evalCurve(controlCurve(device, param, decl), paramPosition(device, param, decl));
}

namespace {
// GU.f4: what lowersAsLiteral reads of a project, indexed once (plan() asks it of every value block).
struct LiteralIndex {
  std::unordered_map<uint32_t, std::vector<const Wire*>> from;  // device id → the wires out of it
  std::unordered_map<uint32_t, std::vector<const Wire*>> into;  // PF.6: device id → the wires into it
  std::unordered_set<uint32_t> driven;                          // devices a wire goes into
  std::unordered_set<uint32_t> laned;                           // devices with a lane on their `value`
  std::unordered_map<uint32_t, const Device*> devices;
  explicit LiteralIndex(const Project& p) {
    const size_t wires = wiresOf(p).size();
    from.reserve(wires);
    driven.reserve(wires);
    into.reserve(wires);
    for (const Wire& w : wiresOf(p)) {
      if (w.from.valid()) from[w.from.value].push_back(&w);
      if (w.to.valid()) {
        driven.insert(w.to.value);
        into[w.to.value].push_back(&w);
      }
    }
    const uint32_t valueHash = deviceParamHash(kValueParam);
    forEachTrack(p, [&](const Track& t, const Track*) {
      for (const LanePtr& l : t.lanes)
        if (l->param.kind == ParamKind::DeviceParam && l->param.target == valueHash) laned.insert(l->param.owner);
      for (const DevicePtr& d : t.devices) devices.emplace(d->id.value, d.get());
    });
    // PF.6: the devices whose output reaches a choice's index through active atom blocks (literalIn's walk, once for
    // the whole project): a wire into an active mux / demux `index` or select `cond` marks its source; a marked active
    // atom block marks every device wired into it.
    std::vector<uint32_t> stack;
    auto mark = [&](uint32_t id) {
      if (choosing.insert(id).second) stack.push_back(id);
    };
    for (const Wire& w : wiresOf(p)) {
      if (!w.from.valid() || !w.to.valid() || w.toMixer()) continue;
      const Device* t = activeAtom(w.to.value);
      if (t == nullptr) continue;
      const std::string_view atom = atomDeviceName(*t);
      if (((atom == "mux" || atom == "demux") && w.param == "index") || (atom == "select" && w.param == "cond")) mark(w.from.value);
    }
    while (!stack.empty()) {
      const uint32_t at = stack.back();
      stack.pop_back();
      if (activeAtom(at) == nullptr) continue;  // the walk passes through active atom blocks only
      if (const auto in = into.find(at); in != into.end())
        for (const Wire* w : in->second)
          if (w->from.valid() && !w->toMixer()) mark(w->from.value);
    }
  }
  std::unordered_set<uint32_t> choosing;  // PF.6: see above
  const Device* activeAtom(uint32_t id) const {
    const auto t = devices.find(id);
    return t != devices.end() && t->second->active && isAtomDevice(*t->second) ? t->second : nullptr;
  }
};

// EQ.3: whether stepped / constant value block `d` wires straight into an atom block's `real const` input.
bool feedsConstIn(const LiteralIndex& ix, const Device& d) {
  const auto out = ix.from.find(d.id.value);
  if (out == ix.from.end()) return false;
  for (const Wire* w : out->second) {
    if (!w->to.valid() || w->toMixer() || w->port != kValueOutput) continue;
    const auto t = ix.devices.find(w->to.value);
    if (t != ix.devices.end() && t->second->active && atomConstInput(*t->second, w->param)) return true;
  }
  return false;
}

bool literalIn(const LiteralIndex& ix, const Device& d) {
  if (!isValueDevice(d) || !d.active) return false;
  const dsp::Curve* c = d.curve(kValueParam);  // the declared curve (linear 0 … 1) is neither stepped nor a constant
  if (c == nullptr || !(c->step > 0.0 || c->min == c->max)) return false;
  if (ix.driven.count(d.id.value) != 0 || ix.laned.count(d.id.value) != 0) return false;  // driven
  if (feedsConstIn(ix, d)) return true;  // EQ.3: a structural input's value
  // Forward through atom blocks to a choice's index (LiteralIndex::choosing: that walk, made once).
  return ix.choosing.count(d.id.value) != 0;
}

std::unordered_set<uint32_t> literalValueBlocks(const Project& p) {
  std::unordered_set<uint32_t> out;
  std::optional<LiteralIndex> ix;  // built at the first stepped or constant value block
  forEachDevice(p, [&](const Device& d, const Track&) {
    const dsp::Curve* c = isValueDevice(d) ? d.curve(kValueParam) : nullptr;
    if (c == nullptr || !(c->step > 0.0 || c->min == c->max)) return;
    if (!ix) ix.emplace(p);
    if (literalIn(*ix, d)) out.insert(d.id.value);
  });
  return out;
}
}  // namespace

bool lowersAsLiteral(const Project& p, const Device& d) {
  const dsp::Curve* c = isValueDevice(d) ? d.curve(kValueParam) : nullptr;
  if (c == nullptr || !(c->step > 0.0 || c->min == c->max)) return false;  // (before indexing the project)
  const LiteralIndex ix(p);
  // EQ.3: a block setting a const input is structural even when driven (the input takes its knob value).
  return literalIn(ix, d) || (d.active && feedsConstIn(ix, d));
}

std::vector<dsp::CurvePreset> curvePresets(const dsp::Curve& d) {
  std::vector<dsp::CurvePreset> out;
  if (!(d.max > d.min) || d.step > 0.0) return out;
  if ((d.output == dsp::CurveOutput::None && d.min > 0.0) || d.preset == dsp::CurvePreset::Exp0)
    out.push_back(dsp::CurvePreset::Octave);
  out.push_back(dsp::CurvePreset::Exponential);
  out.push_back(dsp::CurvePreset::Logarithmic);
  out.push_back(dsp::CurvePreset::Linear);
  return out;
}

dsp::Curve curveWithPreset(const dsp::Curve& d, dsp::CurvePreset p) noexcept {
  if (p == d.preset) return d;
  if (p == dsp::CurvePreset::Octave && d.preset == dsp::CurvePreset::Exp0) return d;  // the declared exp0 law is its octave
  if (p == dsp::CurvePreset::Octave && !(d.output == dsp::CurveOutput::None && d.min > 0.0)) return d;
  dsp::Curve c = dsp::curvePreset(p, d.min, d.max);
  c.unit = d.unit;
  c.digits = d.digits;
  c.step = d.step;
  c.mode = d.mode;
  c.output = d.output;
  return c;
}

const graph::ParamDecl* findParamDecl(const graph::Patch& patch, std::string_view param) noexcept {
  const graph::ParamDecl* d = findAnyParamDecl(patch, param);
  return d != nullptr && d->kind == graph::Kind::Real ? d : nullptr;
}

const graph::ParamDecl* findAnyParamDecl(const graph::Patch& patch, std::string_view param) noexcept {
  for (NodeIndex n = 0; n < patch.nodeCount(); ++n)
    if (patch.node(n).kind == graph::NodeKind::Param && patch.nodeName(n) == param) return &patch.paramDecl(n);
  return nullptr;
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
    if (nd.region != 0) h(nd.region);  // TX.group-oversample (a patch without regions keeps its hash)
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
  for (const graph::OversampleRegion& r : p.regions()) {  // TX.group-oversample: the oversampled groups
    h(r.factor);
    h.str(r.factorFrom);
    for (const std::string& ph : r.phase) h.str(ph);
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
    // T4.18
    case ParamKind::WireAmount: return "wire" + std::to_string(ref.owner) + ":amount";
    default: return {};  // solo is derived into the gates; a device param needs its name
  }
}

std::string paramKey(const AutomationLane& lane) {
  if (lane.param.kind == ParamKind::DeviceParam)
    return lane.paramName.empty() ? std::string() : "dev" + std::to_string(lane.param.owner) + ":" + lane.paramName;
  return paramKey(lane.param);
}

std::string paramKey(const Project& p, const AutomationLane& lane) {  // GU.1
  std::string k = paramKey(lane);
  if (lane.param.kind == ParamKind::DeviceParam && !k.empty())
    if (const Device* d = findDevice(p, DeviceId{lane.param.owner}).device; d != nullptr) k += kPositionKeySuffix;
  return k;
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

namespace {
// TX.all-free: every wire's state, in id order: the planned ones and the main paths' links (Lowered: realised as the
// path's own connections; Inactive on a track that is not lowered).
std::vector<WireState> withLinks(std::vector<WireState> states, const Cables& c, const Plan& pl) {
  if (c.links.empty()) return states;
  for (const auto& [w, t] : c.links)
    states.push_back(WireState{w, pl.indexOf.count(t.value) != 0 ? WireStatus::Lowered : WireStatus::Inactive});
  std::sort(states.begin(), states.end(), [](const WireState& a, const WireState& b) { return a.id < b.id; });
  return states;
}
}  // namespace

Result<LoweredProject> lower(const Project& p, const LowerOptions& o, const LoweredProject* previous) {
  if (!p.master) return Error{ErrorCode::InvalidArgument, "lower: the project has no master", {}};
  if (o.library == nullptr) return Error{ErrorCode::InvalidArgument, "lower: no device library", {}};
  // TX.all-free: the main paths, and the project as lowered (its other wires by name; lower.h "The main path").
std::shared_ptr<const Cables> cables = cablesOf(p, o.library, o.devices);
  const Project& q = cables->q;
UserCallees uc = makeCallees(q, cables, o.library, true, &o.metaSlotOf);  // TMM.3 meta modules
  const Callees* lib = &uc.callees;
const Plan pl = plan(q, &o.tuningTableOf, lib);
const AudibilityMap aud = computeAudibility(q, AudibilityOptions{true});
  LoweredProject out;
  out.graph.library = lib->lib;
  out.userLibrary = std::move(uc.combined);
  out.deviceErrors = std::move(uc.errors);
  out.metaPatches = std::move(uc.meta);
  out.metaResources = std::move(uc.metaResources);
const std::vector<TrackDevs> devs = allDevices(pl, lib, &out.missingDevices);
PortDeps deps(lib->lib);
const WirePlan wp = planWires(q, pl, devs, deps, lib);  // T4.7
const OsRegions os = oversampledGroups(q);  // TX.group-oversample
out.wires = withLinks(wp.states, *cables, pl);
  std::unordered_map<uint32_t, std::shared_ptr<const LoweredPatch>> prev;  // track id → its previous patch
  if (previous != nullptr)
    for (size_t j = 0; j < previous->trackIds.size(); ++j) prev.emplace(previous->trackIds[j].value, previous->patches[j]);
  for (uint32_t i = 0; i < pl.nodes.size(); ++i) {
    const Node& n = pl.nodes[i];
    bool voices = false;
    uint32_t polyphony = 0;
    int32_t steal = -1;
    // T5.3: an audio track's clip slot (the engine's audio sequence it plays; LowerOptions::clipSlotOf).
    const uint32_t clipSlot = n.kind == Kind::Audio && o.clipSlotOf ? o.clipSlotOf(n.track->id) : graph::kNone;
    if (n.kind == Kind::Audio && clipSlot == graph::kNone) out.withoutSlot.push_back(n.track->id);
    auto built = buildPatch(pl, i, o, devs[i], wp, voices, polyphony, steal, i, &out.taps, clipSlot, &out.metaResources, lib,
                            &out.curves, &os);
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
    pt.maxVoices = polyphony;  // T4.12-fix1 / IG.1-2: the voice contexts' voice count (0: the link's default)
    pt.steal = steal;          // IG.1-2: an instrument group's stealing (−1: the link's)
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
      // (TX.all-free-fix1: a cable into another track's mixer is an audio route, as a send is)
      out.graph.wires.push_back({{w.fromNode, m}, {w.toNode, m}, w.toMixer ? W::Audio : W::Modulation});
    }
  // IG.1-2 (lower.h "Voice contexts"): a voice context lowered in its source's patch sends its output to the placeholder
  // on its home's path (an audio route, like a send).
  for (const WirePlan::Remote& r : wp.remotes)
    if (r.host != r.home) {
      const std::string port = remotePortName(devs[r.host][r.hostDev].device->id);
      out.graph.wires.push_back({{r.host, port}, {r.home, port}, W::Audio});
    }
  for (const ModulationTap& t : out.taps) out.graph.taps.push_back({t.track, t.port});
  out.graph.output = {pl.master, "out"};
  // GU.1: without the link's slots the lowering numbered its driven controls' slots itself: it owns them (the graph
  // carries them to whoever compiles it).
  if (!o.curveSlotOf && !out.curves.empty()) {
    auto owned = std::make_shared<std::vector<std::unique_ptr<dsp::CurveSlot>>>();
    out.graph.curveSlots.assign(out.curves.size() + 1, nullptr);
    for (const CurveSite& c : out.curves) {
      if (c.slot == 0 || c.slot >= out.graph.curveSlots.size()) continue;
      owned->push_back(std::make_unique<dsp::CurveSlot>(dsp::compileCurve(c.curve)));
      out.graph.curveSlots[c.slot] = owned->back().get();
    }
    out.graph.curveOwner = std::move(owned);
  }
  return out;
}

namespace {

// `p` the project as lowered (Cables::q), `lib` with its cables.
std::vector<LoweredParam> paramsWith(const Project& p, bool tuningNodes, const Callees* lib) {
  const TableOf anyTable = [](TuningId, const TuningDef&) { return 0u; };  // the index does not change a param
  const Plan pl = plan(p, tuningNodes ? &anyTable : nullptr, lib);
  const AudibilityMap aud = computeAudibility(p, AudibilityOptions{true});
  const std::vector<TrackDevs> devs = allDevices(pl, lib, nullptr);
  WirePlan wp;
  if (lib != nullptr && !wiresOf(p).empty()) {
    PortDeps deps(lib->lib);
    wp = planWires(p, pl, devs, deps, lib);
  }
  std::vector<LoweredParam> out;
  for (uint32_t i = 0; i < pl.nodes.size(); ++i) addParams(pl, i, aud, out, lib, wp, devs[i]);
  return out;
}

std::vector<WireState> wireStatesWith(const Cables& c, const Callees& lib) {
  const Plan pl = plan(c.q, nullptr, &lib);
  std::vector<WireState> states;
  if (!wiresOf(c.q).empty()) {
    PortDeps deps(lib.lib);
    states = planWires(c.q, pl, allDevices(pl, &lib, nullptr), deps, &lib).states;
  }
  return withLinks(std::move(states), c, pl);
}

}  // namespace

std::vector<LoweredParam> lowerParams(const Project& p, bool tuningNodes, const graph::PatchLibrary* library) {
  std::shared_ptr<const Cables> c = cablesOf(p, library, nullptr);
  if (library == nullptr) return paramsWith(c->q, tuningNodes, nullptr);
  const UserCallees uc = makeCallees(c->q, c, library, !wiresOf(c->q).empty());
  return paramsWith(c->q, tuningNodes, &uc.callees);
}

std::vector<LoweredParam> lowerParams(const Project& p, bool tuningNodes, const DeviceLibrary& devices) {
  std::shared_ptr<const Cables> c = cablesOf(p, &devices.library(), &devices);
  const UserCallees uc = makeCallees(c->q, c, &devices.library(), !wiresOf(c->q).empty());
  return paramsWith(c->q, tuningNodes, &uc.callees);
}

std::vector<WireState> wireStates(const Project& p, const graph::PatchLibrary& library) {
  if (wiresOf(p).empty()) return {};
  std::shared_ptr<const Cables> c = cablesOf(p, &library, nullptr);
  return wireStatesWith(*c, makeCallees(c->q, c, &library, true).callees);
}

std::vector<WireState> wireStates(const Project& p, const DeviceLibrary& devices) {
  if (wiresOf(p).empty()) return {};
  std::shared_ptr<const Cables> c = cablesOf(p, &devices.library(), &devices);
  return wireStatesWith(*c, makeCallees(c->q, c, &devices.library(), true).callees);
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
