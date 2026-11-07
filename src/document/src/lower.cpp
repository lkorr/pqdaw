// Project → graph lowering (T3.17; IR with stable ids T3.17-fix2): pq/document/lower.h describes the signal flow.
#include "pq/document/lower.h"

#include <algorithm>
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
#include "pq/document/pitch_resolve.h"
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
};

using TableOf = std::function<uint32_t(TuningId, const TuningDef&)>;

const Send* storedSend(const Track& t, TrackId ret) {
  for (const Send& s : t.mixer.sends)
    if (s.target == ret) return &s;
  return nullptr;
}

// The patches devices call (T4.4-fix1): the library's by name; a user device's own root (its resolution, kept alive by
// the UserCallees that made this). TMM.3: a chain meta device's expansion, and the specialised root of a device whose
// circuit calls meta modules (lower.h "Meta modules"), in the same map.
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
  // TX.all-free-fix1: the NOTES CABLES that are not links of a main path — a wire from a MIDI track's source (port "")
  // into a top-level block of the same track — by target device: the inputs they enter, named ("" an instrument's notes,
  // an Instrument Rack's). Empty for most projects.
  std::unordered_map<uint32_t, std::vector<std::string>> notesTo;
  // IG.1-2 (pq/document/instrument_groups.h): the voice contexts — instrument groups and lone instruments fed notes —
  // of the project as lowered (its wires as these Cables hold them: main ports named).
  VoiceContexts contexts;
  // Whether a notes cable enters `d`'s notes input (a stock instrument's or an Instrument Rack's main input).
  bool fedNotes(DeviceId d) const noexcept {
    const auto it = notesTo.find(d.value);
    return it != notesTo.end() && std::find(it->second.begin(), it->second.end(), std::string()) != it->second.end();
  }
  const Flow* flowOf(const Track& t) const noexcept {
    const auto it = flows.find(t.id.value);
    return it != flows.end() ? &it->second : nullptr;
  }
};

// The main ports of `d` by name, as the lowering wires them: a meta module's chain input and first output, a group's
// `x` / `y`, a device patch's first non-const `in` (what a chain feeds: effectCall) and first `out`. Instruments and racks
// have no named main input.
MainPorts lowerPortsOf(const Device& d, const graph::PatchLibrary* lib, const DeviceLibrary* devices) {
  if (d.isRack() || isGroupDevice(d) || isMetaDevice(d)) return mainPortsOf(d, devices);
  const graph::Patch* callee = builtinBlockPatch(d);  // GU.2: an atom / value block
  std::shared_ptr<const ResolvedDevice> hold;
  if (callee == nullptr && devices != nullptr) callee = devices->patchOf(d, hold);
  else if (callee == nullptr && lib != nullptr && !d.source) callee = lib->find(d.patch);
  if (callee == nullptr) return {};
  MainPorts m;
  for (NodeIndex v = 0; v < callee->nodeCount(); ++v) {
    const graph::Node& n = callee->node(v);
    if (n.kind == graph::NodeKind::Input && m.in.empty() && d.kind != DeviceKind::Instrument &&
        callee->outputType(v, 0).rate != graph::Rate::Const)
      m.in = callee->nodeName(v);
    else if (n.kind == graph::NodeKind::Output && m.out.empty())
      m.out = callee->nodeName(v);
  }
  return m;
}

std::shared_ptr<const Cables> cablesOf(const Project& p, const graph::PatchLibrary* lib, const DeviceLibrary* devices) {
  auto c = std::make_shared<Cables>();
  c->q = p;
  const MainLinks mainLinks(p);  // one pass over the wires for every track's path
  forEachTrack(p, [&](const Track& t, const Track*) {
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
  std::unordered_map<uint32_t, MainPorts> ports;
  auto portsOf = [&](DeviceId id) -> const MainPorts& {
    auto it = ports.find(id.value);
    if (it == ports.end()) {
      const DeviceLoc l = findDevice(p, id);
      it = ports.emplace(id.value, l.device != nullptr && l.chain == nullptr ? lowerPortsOf(*l.device, lib, devices) : MainPorts{}).first;
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
    if (x.fromNoteSource() && w.port.empty() && !x.toMixer())  // TX.all-free-fix1: a notes cable (same MIDI track only)
      if (const DeviceLoc l = findDevice(p, x.to); l.track != nullptr && l.chain == nullptr) {
        if (l.track->id == x.noteSource && l.track->kind == TrackKind::Midi) c->notesTo[x.to.value].push_back(x.param);
      }
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
  // TX.all-free-fix1: a notes cable enters `d`'s notes input (it plays them wherever it is).
  bool fedNotes(const Device& d) const noexcept { return cables != nullptr && cables->fedNotes(d.id); }
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
  // TMM.3: the nested meta-module calls of each device that has any (their params are runtime params of the track).
  std::unordered_map<uint32_t, std::shared_ptr<const std::vector<MetaSite>>> metaSites;
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
    if (const VoiceContext* c = blockContext(d.id); c != nullptr && c->group) return groupNotesPatch();
    if (memberContext(d.id) != nullptr && runsPerNote(d.id)) return groupVoicePatch();
    return groupPatch();
  }
  const graph::Patch* find(const Device& d) const noexcept {
    if (lib == nullptr) return nullptr;
    if (const auto it = userRoot.find(d.id.value); it != userRoot.end()) return it->second;
    if (isGroupDevice(d)) return &groupCallee(d);  // TX.groups (lower.h "Groups"; the graph's library holds it)
    if (d.source || isMetaDevice(d)) return nullptr;
    if (isAtomDevice(d)) return atomBlockPatch(atomDeviceName(d));  // GU.2 (lower.h "Atom and value blocks")
    if (isValueDevice(d)) return &valuePatch();
    return lib->find(d.patch);
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
      for (const DevicePtr& d : *w.list)
        if (d->rack)
          for (const ChainPtr& c : d->rack->chains)
            work.push_back(Work{&c->devices, d->kind == DeviceKind::Instrument, false, nullptr});
    }
  });
  return out;
}

// The callees of `p`'s devices: `base` alone while no device is a user device. With user devices and `combined`
// (what a graph needs: lower(), the wire planner's walk into callees), a combined library of their renamed patches,
// the base library and the stdlib files their texts import; without it (the param fast path: only roots are looked
// at) nothing is built but the per-device lookup (a cache hit per user device).
struct UserCallees {
  Callees callees;
  std::shared_ptr<graph::PatchLibrary> combined;
  std::vector<std::shared_ptr<const ResolvedDevice>> held;
  std::vector<LoweredProject::DeviceError> errors;
  std::shared_ptr<MetaPatches> meta;                       // TMM.3: the expansions (combined only)
  std::vector<LoweredProject::MetaResource> metaResources;  // TMM.3 (combined only)
};

using MetaSlotOf = std::function<uint32_t(DeviceId, std::string_view, uint32_t)>;

std::string hex16(uint64_t v) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string s(16, '0');
  for (int i = 15; i >= 0; --i, v >>= 4) s[static_cast<size_t>(i)] = kDigits[v & 15];
  return s;
}

bool addsAfterLaw(const Wire& w, const Device& d);  // TX.knob-curves (below)

// TMM.3: expanding a project's meta modules (lower.h "Meta modules").
class MetaExpander {
public:
  MetaExpander(const Project& p, UserCallees& u, const MetaSlotOf* slotOf) : p_(p), u_(u), slotOf_(slotOf) {}

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
    // (Inside a rack chain the chain's forms stay: an Instrument Rack's chain starts at its notes, per voice.)
    const bool top = findDevice(p_, d.id).chain == nullptr;
    c.form = top ? meta::Form::ChainAudio : chainForm(stage);
    c.voice = !top && (c.form == meta::Form::ChainNotes || c.form == meta::Form::ChainVoice);
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
    for (const Wire& w : wiresOf(p_))
      if (w.to == d.id && !w.travels())
        if (const int32_t i = type->inputIndex(w.param); i >= 0 && type->inputs[static_cast<size_t>(i)].kind == graph::Kind::Real) {
          wired[static_cast<size_t>(i)] = 1;
          anyWired = true;
        }
    if (anyWired) c.wired = wired;
    return expandInstance(d, t, "", *type, c, settings, modulated);
  }

  // `root` with every named meta call below it replaced by its instance's expansion (copies of the patches on the way,
  // renamed); `root` itself when it calls none.
  const graph::Patch* specialise(const Device& d, const Track& t, const graph::Patch& root, const PatchLookup& lookup) {
    device_ = &d;
    track_ = &t;
    lookup_ = &lookup;
    active_.clear();
    return walk(root, std::string());
  }

private:
  // Whether a chain device's param is not static: an automation lane, a modulation wire or a macro mapping drives it.
  bool modulatedParam(const Device& d, const Track& t, std::string_view name) {
    const ParamRef ref = ParamRef::device(d.id, name);
    for (const LanePtr& l : t.lanes)
      if (l->param == ref) return true;
    for (const Wire& w : wiresOf(p_))
      if (w.to == d.id && w.param == name) return true;
    if (!mappedKnown_) {
      mappedKnown_ = true;
      forEachDevice(p_, [&](const Device& r, const Track&) {
        if (r.rack)
          for (const MacroMapping& m : r.rack->mappings) mapped_.emplace_back(m.device.value, m.param);
      });
    }
    return std::find(mapped_.begin(), mapped_.end(), std::pair<uint32_t, std::string>(d.id.value, std::string(name))) !=
           mapped_.end();
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

  const graph::Patch* walk(const graph::Patch& p, const std::string& prefix) {
    if (active_.size() >= graph::kMaxSubcircuitDepth || std::find(active_.begin(), active_.end(), &p) != active_.end())
      return &p;
    active_.push_back(&p);
    std::optional<graph::Patch> copy;
    std::optional<graph::Adjacency> adj;
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](std::string_view s) {
      for (const char ch : s) h = (h ^ static_cast<unsigned char>(ch)) * 1099511628211ull;
      h = (h ^ 0xFFu) * 1099511628211ull;
    };
    mix(prefix);
    for (NodeIndex n = 0; n < p.nodeCount(); ++n) {
      if (p.node(n).kind != graph::NodeKind::Subcircuit || p.nodeName(n).empty()) continue;
      const std::string& callee = p.subcircuit(n).patch;
      const std::string& name = p.nodeName(n);
      const graph::Patch* to = nullptr;
      if (const meta::Type* type = meta::typeOfCall(callee)) {
        if (!adj) adj = p.adjacency();
        const std::string path = prefix + name;
        std::vector<uint8_t> wired(type->params.size(), 0);
        for (size_t i = 0; i < type->params.size(); ++i)
          if (const auto pin = p.findInput(n, type->params[i].name))
            wired[i] = adj->driver[p.flatInput(n, *pin)] != graph::kNone ? 1 : 0;
        meta::ExpandContext c;
        c.voice = device_->kind == DeviceKind::Instrument;
        to = expandInstance(*device_, *track_, path, *type, c, metaSettings(*device_, path, *type), wired);
      } else if (const graph::Patch* inner = (*lookup_)(callee); inner != nullptr && callsMeta(*inner, *lookup_)) {
        to = walk(*inner, prefix + name + ".");
        if (to == inner) to = nullptr;
      }
      if (to == nullptr) continue;
      if (!copy) copy = p;
      copy->setSubcircuitPatch(n, to->name());
      mix(to->name());
    }
    active_.pop_back();
    if (!copy) return &p;
    copy->setName(p.name() + "__m" + std::to_string(device_->id.value) + "_" + hex16(h));
    return &patches().patches.emplace_back(std::move(*copy));
  }

  const Project& p_;
  UserCallees& u_;
  const MetaSlotOf* slotOf_;
  const Device* device_ = nullptr;
  const Track* track_ = nullptr;
  const PatchLookup* lookup_ = nullptr;
  std::vector<const graph::Patch*> active_;
  bool mappedKnown_ = false;
  std::vector<std::pair<uint32_t, std::string>> mapped_;
};

bool calleeIsInstrument(const graph::Patch& p) noexcept;  // (below)
bool rackPlaysNotes(const Device& rack, const Callees* lib);  // TX.all-free-fix1 (below)

// TX.all-free: `p` is the project as lowered (Cables::q), `cables` its main paths.
UserCallees makeCallees(const Project& p, std::shared_ptr<const Cables> cables, const graph::PatchLibrary* base,
                        const DeviceLibrary* devices, bool combined, const MetaSlotOf* metaSlotOf = nullptr) {
  UserCallees u;
  u.callees.lib = base;
  u.callees.cables = std::move(cables);
  if (base == nullptr) return u;
  std::vector<const graph::PatchFile*> extra;
  std::unordered_map<uint32_t, const ResolvedDevice*> resolved;  // TMM.3: a user device's graph (its callees' lookup)
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
    resolved[d.id.value] = r.get();
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
  // TMM.3: meta modules — chain meta devices and the nested calls of every device's circuit.
  MetaExpander ex(p, u, metaSlotOf);
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
    bool plays = c.group;
    if (!plays) {
      if (c.block->rack) plays = rackPlaysNotes(*c.block, &u.callees);
      else if (const graph::Patch* callee = u.callees.find(*c.block)) plays = calleeIsInstrument(*callee);
    }
    if (!plays) continue;
    u.callees.voicedTracks.insert(c.source.value);
    if (!c.group) {
      u.callees.perNote.insert(c.block->id.value);  // a lone instrument: its own code per voice
      continue;
    }
    // A member runs ONCE when it has an input from inside the group and every such input is single-signal: a cable
    // marked single, or one from a member that runs once (SPEC §15 2026-11-06). Inputs from outside are broadcast and
    // decide nothing. Every other member runs per note.
    // GU.2: its members at any depth (a nested group is a member that runs per note because its parent does).
    const std::vector<const Device*> members = groupMembersDeep(c.home->devices, c.block->id);
    std::unordered_set<uint32_t> once;
    auto inside = [&](const Wire& w) {
      if (w.fromNoteSource()) return w.noteSource == c.source;
      if (w.from == c.block->id) return w.port == kGroupInPort;
      return std::any_of(members.begin(), members.end(), [&](const Device* m) { return m->id == w.from; });
    };
    for (bool changed = true; changed;) {
      changed = false;
      for (const Device* m : members) {
        if (once.count(m->id.value) != 0) continue;
        bool any = false, all = true;
        for (const Wire& w : wiresOf(p)) {
          if (w.toMixer() || w.to != m->id || !inside(w)) continue;
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
    if (d.rack) return;
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
      const meta::Form f = findDevice(p, d.id).chain == nullptr ? meta::Form::ChainAudio : chainForm(st);  // IG.1-2
      const std::optional<graph::Patch>* form = e == nullptr                   ? nullptr
                                                : f == meta::Form::ChainNotes ? &e->notes
                                                : f == meta::Form::ChainVoice ? &e->voice
                                                                              : &e->audio;
      u.callees.userRoot[d.id.value] = form != nullptr && *form ? &**form : nullptr;
      return;
    }
    const graph::Patch* root = u.callees.find(d);
    if (root == nullptr) return;
    const auto rit = resolved.find(d.id.value);
    const ResolvedDevice* user = rit != resolved.end() ? rit->second : nullptr;
    const graph::PatchLibrary* lib = u.callees.lib;
    const PatchLookup lookup = [&, user, lib](std::string_view name) -> const graph::Patch* {
      if (user != nullptr && user->graph)
        if (const graph::Patch* x = user->graph->library.find(name)) return x;
      return lib->find(name);
    };
    std::shared_ptr<const std::vector<MetaSite>> sites =
        devices != nullptr ? devices->metaSitesOf(d, *root, user, lookup)
                           : [&]() -> std::shared_ptr<const std::vector<MetaSite>> {
                               std::vector<MetaSite> s = metaSites(*root, lookup);
                               return s.empty() ? nullptr : std::make_shared<const std::vector<MetaSite>>(std::move(s));
                             }();
    if (!sites) return;
    u.callees.metaSites[d.id.value] = sites;
    if (combined) u.callees.userRoot[d.id.value] = ex.specialise(d, t, *root, lookup);
  });
  // TX.groups-fix1 (lower.h "Free devices"): a switched-off free device whose main output a wire reads passes what
  // reaches its chain input (the input a chain would feed: a meta module's chain input, else the first non-const `in`)
  // to that output, as the chain passes a switched-off device by: a call of `pass__<in>__<out>` (one `in`, one `out`,
  // any poly, up to stereo; nothing else).
  if (combined && !wiresOf(p).empty()) {
    std::unordered_map<std::string, const graph::Patch*> made;
    forEachDevice(p, [&](const Device& d, const Track&) {
      if (d.active || !u.callees.isOffPath(d) || d.rack || isGroupDevice(d)) return;
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
// TX.patching: the signature index (the `out`s, then the exports) of a callee's output named `name` that carries a
// signal (not a bool `out`, such as `done`), or kNone.
uint32_t signalOutput(const graph::Patch& callee, std::string_view name) noexcept {
  const uint32_t outs = outputCount(callee);
  uint32_t o = 0, e = 0;
  for (NodeIndex v = 0; v < callee.nodeCount(); ++v) {
    const graph::NodeKind k = callee.node(v).kind;
    if (k == graph::NodeKind::Output) {
      if (callee.nodeName(v) == name) return callee.inputType(v, 0).kind == graph::Kind::Bool ? graph::kNone : o;
      ++o;
    } else if (k == graph::NodeKind::Export) {
      if (callee.nodeName(v) == name) return outs + e;
      ++e;
    }
  }
  return graph::kNone;
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
  // Its signal is per voice: a voice context's lone instrument, an instrument group's member that runs per note
  // (IG.1-2), an effect of a per-voice rack chain.
  bool voice = false;
  // TX.groups-fix1: a switched-off free device passing its chain input to its main output (callee: its pass__ patch).
  bool bypass = false;
  bool offPath = false;  // TX.all-free: a block off its track's main path (called after the path, played by its wires)
  // A voice context's block lowered here (IG.1-2: an instrument group, or a lone stock instrument / Instrument Rack:
  // it plays its source's notes; its `done` joins the track's).
  bool plays = false;
  bool top = false;  // TX.all-free-fix1: a block of the track itself (not inside a rack)
  // IG.1-2 (lower.h "Voice contexts"): REMOTE — the block, on this track's main path, of a voice context lowered in
  // another track's patch (its source's): not called here; what the path hears after it is its output, imported
  // (`in v<device>`). HOSTED — a device of another track's voice context lowered in this (its source's) patch.
  bool remote = false;
  bool hosted = false;
  uint32_t host = graph::kNone;  // remote: the plan node lowering it
  // IG.1-2: a meta module heading a MIDI track's path outside every context: its chain input reads the newest note.
  bool notesIn = false;
};
// TX.all-free-fix1: in a series (TrackDevs::top, LChain::devs) a stock instrument no notes reach: it is not called and
// what follows it in the series hears silence (its output). Never an index into TrackDevs::devs.
constexpr uint32_t kSilent = 0xFFFFFFF0u;
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
  std::vector<uint32_t> top;  // the track's main path (TX.all-free: its blocks in signal order, the series the chain was)
  std::vector<uint32_t> offPath;  // TX.all-free: its blocks off the main path (called, played by their wires), list order
  std::vector<uint32_t> offPathRacks;  // TX.all-free-fix1: its racks off the main path (lowerList → lowerTree: offPath)
  // IG.1-2: the voice contexts' blocks lowered here (indices into devs): their `done`s are the track's (the AND).
  std::vector<uint32_t> contexts;
  bool complete = true;  // TX.all-free: the main path reaches the mixer (else the mixer hears only the wires into it)
  struct PendingFree {
    const Device* device = nullptr;
    const graph::Patch* callee = nullptr;
    bool bypass = false;  // TX.groups-fix1: switched off, passing its chain input on
    bool plays = false;   // IG.1-2: a voice context's block (a lone stock instrument)
    bool hosted = false;  // IG.1-2: of another track's context
  };
  std::vector<PendingFree> pendingFree;  // lowerList → lowerTree
  std::vector<LRack> racks;
  std::vector<LMapping> mappings;
  bool voices = false;  // the track plays voices (IG.1-2: something plays or reads its notes)

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
      if (!d->active || lib->isOffPath(*d)) continue;  // TX.all-free: a block off the main path is not in the flow
      if (isMetaDevice(*d) || isGroupDevice(*d)) {  // TMM.10-fix1: it plays when its chain input takes the note source
        if (lib->readsNotes(*d) && lib->find(*d) != nullptr) return true;  // TX.groups-fix1: a group there too
        break;
      }
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
  if (t.kind != TrackKind::Midi || lib == nullptr) return false;
  const Flow* f = lib->cables != nullptr ? lib->cables->flowOf(t) : nullptr;  // TX.all-free: along its main path
  return listLowersInstrument(f != nullptr ? f->list : t.devices, lib);
}

// TX.all-free-fix1: whether an Instrument Rack plays (a chain that plays lowers an instrument first).
bool rackPlaysNotes(const Device& rack, const Callees* lib) {
  if (!rack.rack || rack.kind != DeviceKind::Instrument) return false;
  const bool solo = soloIn(*rack.rack);
  for (const ChainPtr& c : rack.rack->chains)
    if (chainPlays(*c, solo) && listLowersInstrument(c->devices, lib)) return true;
  return false;
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

// IG.1-2 (lower.h "Voice contexts"): where a track's blocks are lowered. A voice context is lowered in its notes
// source's patch: its block and members are HOSTED there when that is another track's (`elsewhere`: device id → the plan
// node lowering it); on its own track only a REMOTE placeholder stands on the path for it.
struct TrackScope {
  const std::unordered_map<uint32_t, uint32_t>* elsewhere = nullptr;
  bool hostedList = false;  // the list is another track's context, lowered here (every block off the path)
};

// Lowers one device list into `td` (its indices into `out`): active devices whose patch is in the library and of
// their kind, racks with the chains that play. `missing`: devices bypassed because their patch is not in the library or
// not of their kind. M4 review M9: iterative (a stack of the open lists and racks; a deque keeps each open chain's index
// list in place while its devices are lowered), in the same order as the recursive version (the same indices).
// `trackList` (with `scope`): `list` is a track's blocks (its main path, then the blocks off it); else a rack chain's (or
// a lone rack's). IG.1-2: a track's stock instrument or Instrument Rack plays exactly when it is in a voice context (its
// own, or as an instrument group's member); one no notes reach is not called (on the path: kSilent, the path goes on from
// silence). A group plays its context (GroupNotes) when it is an instrument group, else it is an audio block. A meta
// module heading a MIDI track's path is single-signal: its chain input reads the newest note (LDev::notesIn). Inside a
// rack chain the chain's rules hold (an Instrument Rack's chain starts at its notes).
void lowerList(TrackDevs& td, const std::vector<DevicePtr>& list, bool instrumentAllowed, const Callees* lib,
               std::vector<DeviceId>* missing, std::vector<uint32_t>& out, bool trackList = false,
               const TrackScope& scope = {}) {
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
    bool track = false;              // list: a track's own blocks; rack: a block of the track
    bool offPath = false;            // rack: off the main path (TX.all-free-fix1)
  };
  std::deque<Open> stack;
  {
    Open& o = stack.emplace_back();
    o.list = &list;
    o.instrumentAllowed = instrumentAllowed;
    o.out = &out;
    o.track = trackList;
  }
  while (!stack.empty()) {
    Open& f = stack.back();
    if (f.list != nullptr) {
      if (f.next == f.list->size()) {
        stack.pop_back();
        continue;
      }
      const DevicePtr& d = (*f.list)[f.next++];
      const bool top = f.track;  // a block of a track (its own, or another's context hosted here)
      const bool hostedTop = top && scope.hostedList;
      const bool off = hostedTop || (top && lib != nullptr && lib->isOffPath(*d));
      const VoiceContext* vc = top && lib != nullptr ? lib->contextOf(d->id) : nullptr;
      // IG.1-2: a block of a context lowered in another track's patch: a placeholder on the path, else nothing here.
      if (top && !scope.hostedList && scope.elsewhere != nullptr)
        if (const auto it = scope.elsewhere->find(d->id.value); it != scope.elsewhere->end()) {
          if (!off && vc != nullptr && vc->block == d.get() && d->active) {
            f.out->push_back(static_cast<uint32_t>(td.devs.size()));
            LDev ld{d.get(), nullptr, -1, false};
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
      // A stock instrument or Instrument Rack (its main input is notes): it plays where notes reach it.
      const bool takesNotes = !staged && d->kind == DeviceKind::Instrument;
      const bool notesHere = f.instrumentAllowed && f.out->empty();  // the list's notes reach it along the list
      // Whether it plays notes: a track's block in a voice context (IG.1-2), a rack chain's first instrument.
      const bool fed = takesNotes && (top ? vc != nullptr : notesHere);
      const bool contextBlock = top && vc != nullptr && vc->block == d.get();
      auto openRack = [&](bool instrument, std::vector<uint32_t>* into) {
        Open& r = stack.emplace_back();  // `f` stays valid (deque)
        r.rack = d.get();
        r.out = into;
        r.solo = soloIn(*d->rack);
        r.idx = static_cast<uint32_t>(td.devs.size());
        r.ri = static_cast<int32_t>(td.racks.size());
        r.track = f.track;
        r.offPath = off;
        LDev ld{d.get(), nullptr, r.ri, false};
        ld.offPath = off;
        ld.top = f.track;
        ld.plays = instrument && f.track && contextBlock;
        ld.hosted = hostedTop;
        td.devs.push_back(ld);
        if (ld.plays) td.contexts.push_back(r.idx);
        td.racks.emplace_back();
        r.lr.passThrough = !instrument && d->rack->chains.empty();
      };
      if (off) {  // TX.patching / TX.all-free: off the main path (a track's blocks only); called, never chained
        if (takesNotes && !fed) continue;  // no notes reach it: it plays nothing (not missing)
        if (d->rack) {  // a rack off the path plays by its wires (an Instrument Rack: its notes)
          if (takesNotes && !plays.at(d.get())) continue;
          // An Audio Effect Rack plays only while a wire reads it (nothing else would: as a free device nothing reads).
          if (!takesNotes && std::none_of(wiresOf(lib->cables->q).begin(), wiresOf(lib->cables->q).end(),
                                          [&](const Wire& w) { return !w.fromNoteSource() && w.from == d->id; }))
            continue;
          openRack(takesNotes, &td.offPathRacks);
          continue;
        }
        const graph::Patch* callee = lib != nullptr ? lib->find(*d) : nullptr;
        if (callee == nullptr || (!isMetaDevice(*d) && !isGroupDevice(*d) && calleeIsInstrument(*callee) != takesNotes)) {
          if (missing != nullptr) missing->push_back(d->id);
          continue;
        }
        td.pendingFree.push_back({d.get(), callee, false, takesNotes || (contextBlock && vc->group), hostedTop});
        continue;
      }
      // Inside a rack chain: a chain meta device is its list's instrument exactly when it takes the note source.
      const bool instrument = top ? fed || (contextBlock && vc->group) : staged ? lib != nullptr && lib->readsNotes(*d) : takesNotes;
      if (!top && staged && instrument && !notesHere) {
        if (missing != nullptr) missing->push_back(d->id);
        continue;
      }
      if (takesNotes && !fed) {  // no notes reach it here: silence from it on (not missing)
        f.out->push_back(kSilent);
        continue;
      }
      if (d->rack) {
        if (instrument && !plays.at(d.get())) continue;  // nothing plays: left out
        openRack(instrument, f.out);
        continue;
      }
      const graph::Patch* callee = lib != nullptr ? lib->find(*d) : nullptr;
      // (A chain meta device is its list's instrument by its stage, not by its expansion's ports.)
      if (callee == nullptr || (!staged && calleeIsInstrument(*callee) != takesNotes)) {
        if (missing != nullptr) missing->push_back(d->id);
        continue;
      }
      f.out->push_back(static_cast<uint32_t>(td.devs.size()));
      LDev ld{d.get(), callee, -1, top ? takesNotes && fed : instrument};
      ld.top = f.track;
      ld.plays = top && instrument && contextBlock;
      ld.notesIn = top && notesHere && isMetaDevice(*d) && lib != nullptr && lib->readsNotes(*d);
      if (ld.plays) td.contexts.push_back(static_cast<uint32_t>(td.devs.size()));
      td.devs.push_back(ld);
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
    if (c.group)
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
  lowerList(td, flow != nullptr ? flow->list : n.track->devices, n.kind == Kind::Midi, lib, missing, td.top, true, scope);
  // IG.1-2: the voice contexts other tracks' blocks make with this track's notes, lowered here (off the path).
  const uint32_t self = pl.indexOf.at(n.track->id.value);
  if (lib != nullptr && lib->contexts != nullptr && n.kind == Kind::Midi)
    for (const VoiceContext& c : lib->contexts->list) {
      if (c.source != n.track->id || c.home == n.track) continue;
      const auto it = elsewhere.find(c.block->id.value);
      if (it == elsewhere.end() || it->second != self) continue;
      std::vector<DevicePtr> list;
      for (const DevicePtr& d : c.home->devices)
        if (d.get() == c.block || (c.group && groupContains(c.home->devices, c.block->id, d->id))) list.push_back(d);
      std::vector<uint32_t> unused;
      TrackScope hosted;
      hosted.hostedList = true;
      lowerList(td, list, false, lib, missing, unused, true, hosted);
    }
  // TX.patching: the free devices, after every device of the series. IG.1-2: a member of an instrument group that runs
  // per note is a voice device (wires into it stay per voice); every other free device is single-signal.
  for (const TrackDevs::PendingFree& pf : td.pendingFree) {
    const uint32_t k = static_cast<uint32_t>(td.devs.size());
    td.offPath.push_back(k);
    const bool perNote = lib != nullptr && lib->runsPerNote(pf.device->id);
    // (GU.2: a nested group inside an instrument group that runs per note is a voice device too: GroupVoice.)
    LDev ld{pf.device, pf.callee, -1, perNote};
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
  // TX.all-free-fix1: the racks off the path (lowered in lowerList: their chains' devices follow each), after the others.
  td.offPath.insert(td.offPath.end(), td.offPathRacks.begin(), td.offPathRacks.end());
  // IG.1-2: a member of an instrument group runs per note by its group (Callees::perNote), a rack's chains inside it too.
  // (An Instrument Rack sums its chains' voices itself: its output is single-signal, as before.)
  for (LDev& d : td.devs)
    if (lib != nullptr && d.top && d.rack >= 0 && d.device->kind != DeviceKind::Instrument && lib->runsPerNote(d.device->id))
      d.voice = true;
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
};

// IG.1-2: the export a voice context's output crosses to its home's path under (a remote placeholder reads it).
std::string remotePortName(DeviceId d) { return "v" + std::to_string(d.value); }

// T4.8: a rack's signal points: its audio input (an Audio Effect Rack), its params (kRackParams), its output.
uint32_t rackInputs(const LDev& d) noexcept { return (d.device->kind == DeviceKind::Instrument ? 0u : 1u) + kRackParams; }
uint32_t rackParamPoint(const LDev& d, uint32_t param) noexcept {
  return (d.device->kind == DeviceKind::Instrument ? 0u : 1u) + param;
}

// IG.1-2: an instrument group's block lowered here (its call is GroupNotes: In per voice, Out per voice, `y` its sum).
bool isContextGroup(const LDev& d) noexcept { return d.plays && d.rack < 0 && isGroupDevice(*d.device); }

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
  // Signal points: per node its input and output, per device its signature inputs then outputs (a rack: its audio
  // input and params, then its output; a remote placeholder: its output).
  std::vector<uint32_t> nodeBase(nodes);
  std::vector<std::vector<uint32_t>> devBase(nodes);
  auto insOf = [&](uint32_t n, uint32_t k) {
    const LDev& d = devs[n][k];
    return d.remote ? 0u : d.rack >= 0 ? rackInputs(d) : deps.of(*d.callee).ins;
  };
  auto outsOf = [&](uint32_t n, uint32_t k) {
    const LDev& d = devs[n][k];
    return d.remote || d.rack >= 0 ? 1u : static_cast<uint32_t>(deps.of(*d.callee).outs.size());
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
  // member, a lone instrument, an instrument group's In and Out —) and which once (0: everything else, an instrument
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
    // The chains: the track's input → each effect's audio input → its first out → … → the track's output (as buildPatch
    // wires it); a rack fans out to its chains and sums them; a voice context's block ignores the signal so far.
    // M4 review M9: iterative (a stack of the open lists and racks).
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
          if (k == kSilent) {  // TX.all-free-fix1: an instrument no notes reach: nothing goes on
            f.x = graph::kNone;
            continue;
          }
          const LDev& d = td[k];
          if (d.remote || (d.plays && d.rack < 0)) {  // IG.1-2: a voice context's output (here or imported)
            f.x = devOut(n, k, 0);
            continue;
          }
          if (d.rack >= 0) {
            const LRack& r = td.racks[static_cast<size_t>(d.rack)];
            const bool instrument = d.device->kind == DeviceKind::Instrument;
            if (!instrument && f.x != graph::kNone) adj[f.x].push_back(devIn(n, k, 0));
            if (r.passThrough) adj[devIn(n, k, 0)].push_back(devOut(n, k, 0));
            stack.push_back(Flow{{}, 0, graph::kNone, k, false});  // invalidates f
            continue;
          }
          if (d.device->kind == DeviceKind::Instrument && !isMetaDevice(*d.device)) {  // ignores the signal so far
            f.x = deps.of(*d.callee).outs.empty() ? graph::kNone : devOut(n, k, 0);
            continue;
          }
          const uint32_t in = d.notesIn ? graph::kNone : audioInIndex(*d.callee);  // (the newest note: no signal point)
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
    // A MIDI track's chain starts at its notes, an audio track's at its clip player (T5.3): neither reads `in`.
    const bool ownSource = pl.nodes[n].kind == Kind::Midi || pl.nodes[n].kind == Kind::Audio;
    const uint32_t x = flow(std::span<const uint32_t>(td.top), ownSource ? graph::kNone : nodeBase[n]);
    if (x != graph::kNone && td.complete) adj[x].push_back(nodeBase[n] + 1);  // TX.all-free: a broken path: not
    for (const uint32_t k : td.offPath)  // TX.all-free-fix1: a rack off the path: its chains (its input → … → its output)
      if (td[k].rack >= 0) (void)flow(std::span<const uint32_t>(&k, 1), graph::kNone);
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
  // TMM.10-fix2: the plan node of each track (a note source's wires start at their track's node).
  std::unordered_map<uint32_t, uint32_t> nodeOfTrack;
  for (uint32_t n = 0; n < nodes; ++n)
    if (pl.nodes[n].track != nullptr) nodeOfTrack.emplace(pl.nodes[n].track->id.value, n);
  std::vector<uint8_t> fwd, back;
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
  for (const Wire* wp0 : order) {
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
        // notes into it are not played. Into a member of a context with this source: per note (a stock instrument plays
        // them; a meta module's input reads the note's signal of its meaning). Anything else outside every context
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
        if (!isMetaDevice(*dd.device) && !isGroupDevice(*dd.device) && dd.device->kind == DeviceKind::Instrument) {
          if (member && w.param.empty() && (dd.voice || dd.rack >= 0)) wp.states.push_back(st);  // it plays them (Lowered)
          else inactive();
          continue;
        }
        const meta::Type* type = dd.rack < 0 && !dd.bypass && isMetaDevice(*dd.device) ? meta::findType(metaDeviceType(*dd.device)) : nullptr;
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
        sourcePort = sd.rack < 0 ? signalOutput(*sd.callee, w.port) : w.port == "out" ? 0u : graph::kNone;  // fix1: a rack's out
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
      reach(adj, to, seen);
      lw.delayed = seen[from] != 0;
      if (lw.delayed) {
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
    // a rack has none; a bool output is not a signal) or a note output; the target: a real param (a rack: a macro or
    // its selector) or (TX.patching) a non-const real input of the callee.
    uint32_t sourcePort = graph::kNone;
    if (lw.fromTrack) {
      sourcePort = 0;  // TX.all-free: the track's own signal
    } else if (w.fromNoteSource()) {
      if (lw.notes != nullptr) sourcePort = 0;  // an unknown note output: Invalid
    } else if (const LDev& sd = devs[lw.fromNode][lw.fromDev]; sd.rack < 0) {
      sourcePort = signalOutput(*sd.callee, w.port);
    } else if (w.port == "out") {
      sourcePort = 0;  // TX.all-free-fix1: a rack's output (its main output)
    }
    // IG.1-2: an instrument group's In into a member stock instrument's (or Instrument Rack's) notes: it plays them.
    if (!w.fromNoteSource() && w.port == kGroupInPort && isContextGroup(devs[lw.fromNode][lw.fromDev]) && w.param.empty() &&
        !isMetaDevice(*dd.device) && !isGroupDevice(*dd.device) && dd.device->kind == DeviceKind::Instrument) {
      if ((dd.voice || dd.rack >= 0) && lw.fromNode == lw.toNode && tctx != nullptr && tctx->block == devs[lw.fromNode][lw.fromDev].device)
        wp.states.push_back(st);
      else
        inactive();
      continue;
    }
    uint32_t pin = graph::kNone, point = graph::kNone;
    if (dd.rack >= 0) {
      pin = rackParamIndex(w.param);
      if (pin != graph::kNone) {
        point = rackParamPoint(dd, pin);
      } else if (w.param == "in" && dd.device->kind != DeviceKind::Instrument) {  // TX.all-free-fix1: an Audio Effect
        pin = point = 0;                                                         // Rack's input (its main input)
        lw.input = true;
      }
    } else if (const uint32_t units = !addsAfterLaw(w, *dd.device) ? graph::kNone : signalInput(*dd.callee, w.param + "_add");
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
    // A migrated Travel wire keeps T4.7's ends (an export into a param): an old project lowers exactly as it did.
    if (w.travels() && sourcePort != graph::kNone &&
        (lw.input || (lw.notes == nullptr && (devs[lw.fromNode][lw.fromDev].rack >= 0 ||
                                              sourcePort < outputCount(*devs[lw.fromNode][lw.fromDev].callee)))))
      sourcePort = graph::kNone;
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
      if (sd.rack < 0 && sd.callee == &groupNotesPatch()) {
        meta::Reads reads = meta::Reads::Signal;
        if (dd.rack < 0 && !dd.bypass && (isAtomDevice(*dd.device) || isGroupDevice(*dd.device)) && lw.paramNode == graph::kNone)
          reads = meta::Reads::Frequency;
        if (lw.input && dd.rack < 0 && !dd.bypass && isMetaDevice(*dd.device))
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
    reach(adj, to, seen);
    lw.delayed = seen[from] != 0;
    if (lw.delayed) {
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

// TMM.10: a meta voice run's release after note-off (lower.h "Meta modules"): long enough not to click, short enough
// to read as the note's end (an Envelope meta module in the run shapes the real release, TMM.11).
constexpr double kMetaReleaseSeconds = 0.005;

constexpr PortRef kPending{graph::kNone, 0};

// Chain node slots (NodeRole::Chain, k < 48).
enum ChainK : uint32_t {
  kCGl, kCGr, kCGlSel, kCGrSel, kCPack, kCGain, kCSum, kCInGain, kCVoiceSum, kCVoiceGain, kCZoneMul, kCOuterMul, kCDone,
  kCZoneSelect = 13, kCZoneVelocity = 23, kCZoneKey = 33,  // 10 slots each: a1, b1, a2, b2 params, then the math
};
// T4.18: an automated chain mixer's law: its volume / pan params take kCGl / kCGr; the math nodes are keyed by the
// chain's lowest automation lane id (an entity id like a chain's, so its three role spaces are free: chainMixerLane).
enum ChainLawK : uint32_t { kLVolLe, kLVolDb, kLVolGain, kLPanA, kLPanB, kLPanL, kLPanR, kLVolL, kLVolR };
// Mapping node slots (NodeRole::Mapping, k < 48).
enum MapK : uint32_t {
  kMX, kMInv127, kMKx, kMK, kMExp, kMOne, kMSub, kMInvE, kMShape, kMRange, kMScaled, kMMin, kMPos,
  kML, kMMulL, kMLo, kMAddLo, kMExpV, kMRoundAdd, kMFloor,
  kMCurve,  // GU.1: the mapped control's curve atom (a site no wire reaches)
};

// What building one track patch keeps.
// IG.1-2: an instrument group's end as playGroup decided it (finishGroups wires it once the members are called).
struct GroupEnd {
  uint32_t group = 0;                                // device index of the group
  std::vector<std::pair<uint32_t, uint16_t>> dones;  // (member device index, its `done` output)
  std::vector<uint32_t> racks;                       // Instrument Rack members (their end: Build::rackDone)
  PortRef ramp{graph::kNone, 0};                     // the release ramp's `done` (none: no ramp)
};

struct Build {
  Ir& ir;
  const LowerOptions& o;
  const TrackDevs& td;
  std::vector<NodeIndex> calls;  // per lowered device: its call node (kNone: a rack)
  std::map<std::pair<uint32_t, uint32_t>, std::vector<PortRef>> rackPins;  // (rack dev, param) → the inputs reading it
  PortRef vel = kPending, lnHz = kPending, degree = kPending;  // shared voice values the zones read (made on first use)
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
  const Track* track = nullptr;  // T4.18: the track whose patch this is (its lanes decide chain mixers); null: none
  std::vector<LoweredProject::MetaResource>* resources = nullptr;  // TX.spectrum: the analysis taps' slots
  // TX.all-free-fix1: per lowered device, a rack's output (what wires from it read) and the pin wires into an Audio
  // Effect Rack's input are summed into (kPending: none); the racks a wire enters at their input.
  std::vector<PortRef> rackOut, rackIn;
  std::vector<uint8_t> rackInWired;
  // With several voice contexts (TrackDevs::contexts) the track's `done` is the AND of theirs, made at the end (each
  // `done` and the device it ends); `allDone` false: one has none (the engine's own end).
  bool multi = false;
  std::vector<std::pair<PortRef, uint32_t>> dones;
  bool allDone = true;
  const Callees* lib = nullptr;
  const WirePlan* wp = nullptr;
  const Plan* pl = nullptr;  // GU.1: its automated params (Plan::laned); null: none (a rack on its own)
  uint32_t node = 0;  // this patch's plan node
  // IG.1-2 (lower.h "Voice contexts"): per lowered device, a voice context's output (an instrument group's voices
  // summed, a lone instrument's), what its `y` / main output gives everything outside; and the pending input the
  // single-signal terms into an instrument group's Out are added at (kPending: none).
  std::vector<PortRef> ctxOut, outGlobalPin;
  std::vector<GroupEnd> ends;
  std::vector<PortRef> rackDone;  // IG.1-2: an Instrument Rack inside an instrument group: its voices' end (per voice)
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
// Device node slots of a lone stock instrument (NodeRole::Device, k 6 / 7: free slots).
enum DevK : uint32_t { kDVoiceSum = 6, kDDone = 7 };
void finishDones(Build& b) {
  if (!b.multi || !b.allDone || b.dones.empty()) return;
  PortRef d = b.dones.front().first;
  for (size_t i = 1; i < b.dones.size(); ++i)
    d = b.ir.atom("and", {d, b.dones[i].first}, nodeIdFor(NodeRole::Device, b.dones[i].second, kDDone));
  b.ir.output("done", kVoiceDone, d, kDone);
}
// A lowered device's output `port` (a rack: its output). IG.1-2: a voice context's main output is its summed voices; a
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
  if (d.rack >= 0) return b.rackOut[k];
  return PortRef{b.calls[k], port};
}
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
  PortRef gl, gr;
  if (const LaneId lane = b.track != nullptr ? chainMixerLane(*b.track, chain) : LaneId{}; lane.valid()) {
    // T4.18: automated: the chain's volume (dB) and pan as params, the law of chainParams in the graph (block rate).
    auto law = [&](uint32_t k) { return nodeIdFor(NodeRole::Chain, lane.value, k); };
    const PortRef vol = b.ir.param(chainParamName(chain, "volume"), 0.0, kMinVolumeDb, kMaxVolumeDb, id(kCGl));
    const PortRef pan = b.ir.param(chainParamName(chain, "pan"), 0.0, -1.0, 1.0, id(kCGr));
    const PortRef vg = b.ir.atom("select", {b.ir.atom("le", {vol, b.ir.konst(kMinVolumeDb, kCFloor)}, law(kLVolLe)),
                                            b.c0(), b.ir.atom("db_to_gain", {vol}, law(kLVolDb))},
                                 law(kLVolGain));
    const PortRef pl = b.ir.atom("min", {b.c1(), b.ir.atom("sub", {b.c1(), pan}, law(kLPanA))}, law(kLPanL));
    const PortRef pr = b.ir.atom("min", {b.c1(), b.ir.atom("add", {b.c1(), pan}, law(kLPanB))}, law(kLPanR));
    gl = b.ir.atom("mul", {vg, pl}, law(kLVolL));
    gr = b.ir.atom("mul", {vg, pr}, law(kLVolR));
  } else {
    gl = b.ir.param(chainParamName(chain, "gl"), 1.0, 0.0, 4.0, id(kCGl));
    gr = b.ir.param(chainParamName(chain, "gr"), 1.0, 0.0, 4.0, id(kCGr));
  }
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
  const graph::NodeIndex c = callDevice(b.ir, d);
  b.calls[k] = c;
  // TMM.10-fix1: a device without an audio `in` (an unplugged chain meta device) takes nothing: the signal so far ends
  // there (its first pin is a param).
  if (haveIn && x.node != graph::kNone) b.ir.patch().connect(x, PortRef{c, static_cast<uint16_t>(inPort)});  // (IG.1-2: kPending: the default)
  // TX.spectrum (lower.h "Analysis taps"): an analysed stock device's input and output go to its analysis tap.
  if (haveIn && x.node != graph::kNone && b.resources != nullptr && b.o.metaSlotOf && analysedDevice(d.device->patch))
    if (const uint32_t slot = b.o.metaSlotOf(d.device->id, kAnalysisTapPath, 0); slot != 0) {
      auto nid = [&](uint32_t s) { return nodeIdFor(NodeRole::Device, id, s); };
      const PortRef y{c, 0};
      const PortRef il = b.ir.atom("channel", {x, b.c0()}, nid(8)), inr = b.ir.atom("channel", {x, b.c1()}, nid(9));
      const PortRef ol = b.ir.atom("channel", {y, b.c0()}, nid(10)), orr = b.ir.atom("channel", {y, b.c1()}, nid(11));
      b.ir.atom("signal_tap", {il, inr, ol, orr, b.ir.konst(static_cast<double>(slot), nid(12))}, nid(13));
      LoweredProject::MetaResource m;
      m.track = b.track != nullptr ? b.track->id : TrackId{};
      m.device = d.device->id;
      m.path = std::string(kAnalysisTapPath);
      m.request = meta::ResourceRequest{meta::ResourceKind::SignalTap, 0};
      m.slot = slot;
      b.resources->push_back(std::move(m));
    }
  x = PortRef{c, 0};
  if (outW != graph::kWidthInfer) width = outW;
  else if (inWidth == graph::kWidthStereo) width = graph::kWidthStereo;
  return x;
}

struct Voices;
Voices instrumentRack(Build& b, uint32_t k0, PortRef outer0);
uint16_t outWidth(const graph::Patch& callee) noexcept;

// IG.1-2: a lone stock instrument (a voice context of its own): its call, its voices summed (its `done` one of the
// track's terms). What reached it in a series is dropped: it has no audio input.
PortRef playInstrument(Build& b, uint32_t k, uint16_t& width) {
  const LDev& d = b.td[k];
  const NodeIndex c = callDevice(b.ir, d);
  b.calls[k] = c;
  const auto done = b.ir.patch().findOutput(c, "done");
  voiceDone(b, done ? PortRef{c, static_cast<uint16_t>(*done)} : kPending, d.device->id.value);
  width = outWidth(*d.callee) == graph::kWidthStereo ? graph::kWidthStereo : graph::kWidthMono;
  b.ctxOut[k] = b.ir.atom("voice_sum", {PortRef{c, 0}}, nodeIdFor(NodeRole::Device, d.device->id.value, kDVoiceSum));
  return b.ctxOut[k];
}
PortRef playInstrumentRack(Build& b, uint32_t k, uint16_t& width);  // (after instrumentRack)
PortRef playGroup(Build& b, uint32_t k, uint16_t& width);           // (after instrumentRack)

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
      if (k == kSilent) {  // TX.all-free-fix1: an instrument no notes reach: silence from here on
        f.x = b.c0();
        f.width = graph::kWidthMono;
        continue;
      }
      const LDev& d = b.td[k];
      if (d.remote) {  // IG.1-2: a voice context lowered in its source's patch: its output, imported
        f.x = outOf(b, k, 0);
        f.width = graph::kWidthStereo;
        continue;
      }
      if (d.rack < 0) {
        // IG.1-2: a voice context's block (an instrument group, a lone stock instrument): its voices, summed; what
        // reached it in the series is dropped (its input is the notes).
        if (d.plays) f.x = isGroupDevice(*d.device) ? playGroup(b, k, f.width) : playInstrument(b, k, f.width);
        else if (d.notesIn) f.x = effectCall(b, k, newestNote(b, k), f.width);
        else f.x = effectCall(b, k, f.x, f.width);
        continue;
      }
      if (d.plays) {  // a lone Instrument Rack
        f.x = playInstrumentRack(b, k, f.width);
        continue;
      }
      // TX.all-free-fix1: wires into an Audio Effect Rack's input add to what reaches it (lowerSites sums them at this pin).
      if (k < b.rackInWired.size() && b.rackInWired[k]) {
        if (b.freeZero.node == graph::kNone) b.freeZero = b.ir.konst(-0.0, kCFreeZero);
        f.x = b.ir.atom("add", {f.x.node == graph::kNone ? b.freeZero : f.x, b.freeZero}, nodeIdFor(NodeRole::Device, d.device->id.value, 0));
        b.rackIn[k] = PortRef{f.x.node, 0};
      }
      const LRack& r = b.td.racks[static_cast<size_t>(d.rack)];
      if (r.passThrough) {
        if (k < b.rackOut.size()) b.rackOut[k] = f.x;
        continue;
      }
      if (r.chains.empty()) {
        f.width = graph::kWidthMono;
        f.x = b.c0();
        if (k < b.rackOut.size()) b.rackOut[k] = f.x;
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
    if (k < b.rackOut.size()) b.rackOut[k] = acc;  // TX.all-free-fix1: what wires from the rack read
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

PortRef playInstrumentRack(Build& b, uint32_t k, uint16_t& width) {
  const Voices v = instrumentRack(b, k, kPending);
  voiceDone(b, v.done, b.td[k].device->id.value);
  width = v.width;
  if (k < b.rackOut.size()) b.rackOut[k] = v.x;
  return v.x;
}

// IG.1-2 (lower.h "Voice contexts"): an instrument group's voices. Its call (GroupNotes) gives the notes at In and passes
// Out per voice as `y`; Out's per-note terms are summed over the voices, its single-signal terms added once after that.
// The voices END when every ENDER is done: the device whose output alone reaches Out (followed back through pass-
// throughs: TX.scope-sound) when it decides its voices' end — a meta type marked voiceEnd (the Envelope) or a stock
// instrument (its own amp envelope) —, and every stock instrument inside (each ends its own voices). Without an ender
// that reaches Out, the voices get the 5 ms linear release after note-off (kMetaReleaseSeconds) on Out, and that decides
// (with the instruments' ends). The enders' `done`s are wired once the members are called (finishGroups).
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
  const NodeIndex call = callDevice(b.ir, g);
  b.calls[k] = call;
  const PortRef y{call, 0};
  auto tid = [&](uint32_t s) { return nodeIdFor(NodeRole::TuningNode, gid, s); };
  auto sid = [&](uint32_t s) { return nodeIdFor(NodeRole::SendInput, gid, s); };
  // The one lowered wire into an input of `dev` that `match` picks: from a device of this patch, not a note, not delayed,
  // not a single-signal term.
  auto feeder = [&](uint32_t dev, auto match) -> const LWire* {
    const LWire* one = nullptr;
    uint32_t count = 0;
    for (const LWire& w : b.wp->wires)
      if (w.toNode == b.node && w.toDev == dev && w.input && !w.outGlobal && match(w)) {  // (per-note terms)
        ++count;
        one = &w;
      }
    if (count != 1 || one->fromNode != b.node || one->notes != nullptr || one->delayed || one->fromTrack || one->outGlobal)
      return nullptr;
    return one;
  };
  auto isStockInstrument = [](const LDev& d) {
    return d.rack < 0 && !isMetaDevice(*d.device) && !isGroupDevice(*d.device) && d.device->kind == DeviceKind::Instrument;
  };
  GroupEnd end;
  end.group = k;
  // What reaches Out alone, through pass-throughs (TX.scope-sound).
  uint32_t last = graph::kNone;
  const LWire* in = feeder(k, [](const LWire& w) { return w.wire->param == kGroupOutPort; });
  for (size_t guard = 0; in != nullptr && guard <= 2 * b.td.size() + 2; ++guard) {
    if (in->fromDev == k || in->fromPort != 0) break;  // the group's own In (the notes: nothing of its own ends them)
    const uint32_t cur = in->fromDev;
    const LDev& d = b.td[cur];
    if (d.rack >= 0) break;
    const uint32_t pt = passThroughInput(*d.callee);
    if (pt == graph::kNone) {
      last = cur;
      break;
    }
    in = feeder(cur, [pt](const LWire& w) { return w.toPin == pt; });
  }
  bool ended = false;
  if (last != graph::kNone) {
    const LDev& d = b.td[last];
    const meta::Type* t = isMetaDevice(*d.device) ? meta::findType(metaDeviceType(*d.device)) : nullptr;
    const uint32_t o = doneOutput(*d.callee);
    if (o != graph::kNone && ((t != nullptr && t->voiceEnd) || isStockInstrument(d))) {
      end.dones.emplace_back(last, static_cast<uint16_t>(o));
      ended = true;
    }
    width = outWidth(*d.callee) == graph::kWidthStereo ? graph::kWidthStereo : graph::kWidthMono;
  } else {
    width = graph::kWidthStereo;  // (GroupNotes' y: declared stereo)
  }
  // Every stock instrument and Instrument Rack inside ends its own voices.
  for (uint32_t m = 0; m < b.td.size(); ++m) {
    const LDev& d = b.td[m];
    if (m == last || !d.top || b.lib == nullptr) continue;
    const VoiceContext* c = b.lib->memberContext(d.device->id);
    if (c == nullptr || c->block != g.device) continue;
    if (d.rack >= 0 && d.device->kind == DeviceKind::Instrument) {
      end.racks.push_back(m);
      continue;
    }
    if (!isStockInstrument(d) || !d.voice) continue;
    if (const uint32_t o = doneOutput(*d.callee); o != graph::kNone) end.dones.emplace_back(m, static_cast<uint16_t>(o));
  }
  // Out reached only by Instrument Racks inside (their voices summed already: single-signal terms) ends with them.
  bool voiceTerms = false, rackOnly = true, anyOut = false;
  for (const LWire& w : b.wp->wires) {
    if (w.toNode != b.node || w.toDev != k || w.toMixer || w.wire->param != kGroupOutPort) continue;
    anyOut = true;
    voiceTerms = voiceTerms || !w.outGlobal;
    const bool rack = w.fromNode == b.node && !w.fromTrack && w.notes == nullptr && b.td[w.fromDev].rack >= 0 &&
                      std::find(end.racks.begin(), end.racks.end(), w.fromDev) != end.racks.end();
    rackOnly = rackOnly && rack;
  }
  if (!ended && anyOut && rackOnly) ended = true;
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
  const bool globals = std::any_of(b.wp->wires.begin(), b.wp->wires.end(), [&](const LWire& w) {
    return w.toNode == b.node && w.toDev == k && w.outGlobal;
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
    for (const uint32_t m : e.racks) {
      const PortRef d = b.rackDone[m];
      if (d.node == graph::kNone) continue;
      done = done.node == graph::kNone ? d : b.ir.atom("and", {done, d}, nodeIdFor(NodeRole::SendInput, b.td[e.group].device->id.value, s++));
    }
    voiceDone(b, done, b.td[e.group].device->id.value);
  }
}

// ---- Modulation wires and macro mappings in a track patch (T4.7, T4.8; lower.h "Modulation wires", "Racks") ---------

// NodeRole::Wire slots: a wire's own nodes, then (from kWTarget) the nodes of the param it modulates.
enum WireK : uint32_t {
  kWAmount, kWIn, kWExport, kWChannel, kWReduce, kWDelay, kWTerm, kWSum,
  kWNote, kWNoteReal,  // TMM.10-fix2: a note source's voice terminal, its bool → real
  kWMidL, kWMidR, kWMidAdd, kWMid,  // TX.groups: a wide signal into a meta module's input: its mid
  kWOwn14,  // TX.knob-curves: an Add wire's own free slots (kWAmount, kWTerm, this) hold its site's @signed / dB nodes
  kWNotePhase,  // TX.all-free-fix1: a notes cable into a phase input: phasor of the note's Hz
  kWTarget = 16,
};
// A driven param's nodes (keyed by its first wire: kWTarget + k; GU.1: a site no wire reaches by its mapping (MapK
// kMCurve) or its lowest automation lane (an entity id like a wire's: NodeRole::Wire, kWTarget + k)). A rack param's
// travel law keeps T4.7's nodes (kTPosA, kTPosB, kTMapA, kTMapB, kTClip); a control's (GU.1) the knob, the sum, the
// curve atom and the stages after it.
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
// (T4.7), a macro-mapped param, a rack param read by its rack (T4.8).
struct Site {
  uint32_t dev = 0;
  std::string name;
  const graph::ParamDecl* decl = nullptr;
  std::vector<PortRef> pins;            // the inputs it feeds
  std::vector<const LWire*> wires;      // modulation into it, in id order
  const LMapping* mapping = nullptr;    // the macro driving it
  uint32_t lane = 0;                    // GU.1: the lowest automation lane on it (0: none)
  bool resolved = false;
  PortRef value = kPending;
};

// The wires of plan node `node` (its outgoing cross-track wires: root exports m<wire>), then every param the patch
// builds (modulated, mapped, read by a rack), each connected to its pins. Each modulated param's tap goes to `taps`
// (graph track `track`).
void lowerSites(Build& b, uint32_t node, uint32_t track, const WirePlan& wp, std::vector<ModulationTap>* taps,
                std::vector<CurveSite>* curves) {
  Ir& ir = b.ir;
  const TrackDevs& td = b.td;
  auto wid = [](const LWire& w, uint32_t k) { return nodeIdFor(NodeRole::Wire, w.wire->id.value, k); };
  // The source of a wire in this patch: its export's first channel (TMM.10-fix2: or the note output's voice terminal:
  // "hz" the tuned pitch, a bool as 0 / 1), reduced over the voices when it leaves them.
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
      if (isMetaDevice(*td[w.toDev].device) && !td[w.toDev].bypass && td[w.fromDev].rack < 0 &&
          declaredOutWidth(*td[w.fromDev].callee, w.fromPort) != graph::kWidthMono) {
        const PortRef l = ir.atom("channel", {s, b.c0()}, wid(w, kWMidL));
        const PortRef r = ir.atom("channel", {s, b.c1()}, wid(w, kWMidR));
        s = ir.atom("mul", {ir.atom("add", {l, r}, wid(w, kWMidAdd)), ir.konst(0.5, kCHalf)}, wid(w, kWMid));
      }
    } else {
      s = ir.atom("channel", {from(), b.c0()}, wid(w, kWChannel));
    }
    if (w.reduce) {
      s = ir.atom(reducerAtom(w.reducer), {s}, wid(w, kWReduce));
    } else if (w.broadcast) {  // IG.1-2: one value for every voice (−0 added: exact)
      s = ir.atom("add", {s, ir.konst(-0.0, kCFreeZero)}, wid(w, kWReduce));
      ir.patch().setShared(s.node, true);
    }
    return s;
  };
  for (const LWire& w : wp.wires)
    if (w.fromNode == node && w.toNode != node) ir.exportPort("m" + std::to_string(w.wire->id.value), source(w), wid(w, kWExport));

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
    std::vector<std::pair<uint32_t, uint16_t>> done;  // (device, pin) already summed
    for (size_t i = 0; i < wp.wires.size(); ++i) {
      const LWire& f = wp.wires[i];
      if (f.toNode != node || !f.input || f.toMixer) continue;  // (TX.all-free: the mixer's wires are buildPatch's)
      const std::pair<uint32_t, uint16_t> key{f.toDev, static_cast<uint16_t>(f.toPin | (f.outGlobal ? 0x8000u : 0u))};
      if (std::find(done.begin(), done.end(), key) != done.end()) continue;
      done.push_back(key);
      // (TX.all-free-fix1: an Audio Effect Rack's input: the pin its lowering made, rackIn)
      const PortRef pin = td[f.toDev].rack >= 0 ? b.rackIn[f.toDev] : PortRef{b.calls[f.toDev], f.toPin};
      // IG.1-2: the single-signal terms into an instrument group's Out: added once after the voices' sum (playGroup).
      if (f.outGlobal) {
        PortRef gsum = kPending;
        bool gfirst = true;
        for (size_t j = i; j < wp.wires.size(); ++j) {
          const LWire& w = wp.wires[j];
          if (w.toNode != node || !w.outGlobal || w.toDev != f.toDev) continue;
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
      for (graph::WireIndex x = 0; x < p.wireCount() && pin.node != graph::kNone; ++x)
        if (p.wire(x).to == pin) {
          const PortRef from = p.wire(x).from;
          p.disconnect(x);
          if (!(from == b.freeZero)) sum = from;
          break;
        }
      // TX.groups-fix1: chain links first (Wire::chain: a grouped series' link is what the chain gave the input), then
      // the other wires in id order.
      bool first = true;
      for (const bool chainPass : {true, false})
        for (size_t j = i; j < wp.wires.size(); ++j) {
          const LWire& w = wp.wires[j];
          if (w.toNode != node || !w.input || w.toMixer || w.toDev != f.toDev || w.toPin != f.toPin || w.outGlobal) continue;
          if (w.wire->chain != chainPass) continue;
          const PortRef src = arriving(w);
          sum = sum.node == graph::kNone ? src : ir.atom("add", {sum, src}, wid(w, first ? kWTerm : kWSum));
          first = false;
        }
      if (pin.node != graph::kNone) p.connect(sum, pin);
    }
  }
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
      if (b.calls[dev] != graph::kNone) s.pins.push_back(PortRef{b.calls[dev], pp.pin});
    }
    s.mapping = td.mappingOf(dev, name);
    if (d.rack < 0 && b.pl != nullptr) s.lane = b.pl->laneOf(d.device->id, name);
    sites.push_back(std::move(s));
    return sites.back();
  };
  sites.reserve(wp.wires.size() + td.mappings.size() + b.rackPins.size());
  for (const LWire& w : wp.wires) {
    if (w.toNode != node || w.input) continue;  // TX.patching: inputs were summed above
    const LDev& d = td[w.toDev];
    siteOf(w.toDev, d.rack >= 0 ? rackParamName(w.toPin) : d.callee->nodeName(w.paramNode)).wires.push_back(&w);
  }
  for (const LMapping& m : td.mappings) {
    (void)siteOf(m.rack, macroParamName(m.m->macro));  // the macro it reads (before the target: no reallocation later)
    (void)siteOf(m.target, m.m->param);
  }
  for (const auto& [key, pins] : b.rackPins) (void)siteOf(key.first, rackParamName(key.second));
  // GU.1: every real param of a lowered device that an automation lane drives (its curve atom after the knob).
  if (b.pl != nullptr && !b.pl->laned.empty())
    for (uint32_t k = 0; k < td.size(); ++k) {
      const LDev& d = td[k];
      if (d.rack >= 0 || d.callee == nullptr || d.remote || d.bypass || b.calls[k] == graph::kNone) continue;
      const auto it = b.pl->laned.find(d.device->id.value);
      if (it == b.pl->laned.end()) continue;
      for (const auto& [name, lane] : it->second)
        if (realParamPin(*d.callee, name)) (void)siteOf(k, name);
    }
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
    const LDev& d = td[dev];
    const std::string devId = std::to_string(d.device->id.value);
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
    if (d.rack >= 0) {
      // A rack's macro or selector (T4.8): its values 0 … 127 are its own (no curve). Travel wires move it along
      // the straight 0 … 127 (T4.7's law, the same nodes), added wires in its units.
      std::vector<const LWire*> wires, added;
      for (const LWire* w : sites[si].wires) (w->wire->travels() ? wires : added).push_back(w);
      if (!wires.empty()) {
        const LWire& f = *wires.front();
        auto tid = [&](uint32_t k) { return wid(f, kWTarget + k); };
        PortRef knob = kPending, pk = pos;
        if (mapping == nullptr) {
          knob = ir.paramDecl("d" + devId + "__" + name, decl, tid(kTKnob));
          pk = ir.atom("mul", {ir.atom("sub", {knob, ir.literal(0.0)}, tid(kTPosA)), ir.literal(1.0 / kMacroMax)}, tid(kTPosB));
        }
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
        v = ir.atom("add", {ir.atom("mul", {p, ir.literal(kMacroMax)}, tid(kTMapA)), ir.literal(0.0)}, tid(kTMapB));
        if (added.empty()) {
          const PortRef up = ir.atom("max", {ir.atom("sub", {p, pk}, tid(kTUpSub)), b.c0()}, tid(kTUpMax));
          const PortRef down = ir.atom("max", {ir.atom("sub", {pk, p}, tid(kTDownSub)), b.c0()}, tid(kTDownMax));
          const std::string port = tapPort(d.device->id, name);
          ir.exportPort(port, ir.atom("pack", {up, down}, tid(kTPack), 2), tid(kTExport));
          if (taps != nullptr) taps->push_back(ModulationTap{d.device->id, name, track, port, ModulationTap::Domain::Position});
        }
      } else if (!added.empty() && mapping == nullptr) {
        v = ir.paramDecl("d" + devId + "__" + name, decl, wid(*added.front(), kWTarget + static_cast<uint32_t>(kTKnob)));
      } else if (mapping != nullptr) {
        const MacroMapping& m = *mapping->m;
        auto mid = [&](uint32_t s) { return nodeIdFor(NodeRole::Mapping, m.id.value, s); };
        v = ir.atom("add", {ir.atom("mul", {pos, ir.konst(kMacroMax, mid(kML))}, mid(kMMulL)), ir.konst(0.0, mid(kMLo))},
                    mid(kMAddLo));
      } else {
        // A rack param its rack reads: the knob, a runtime param of the track patch (dev<rack>:<name>).
        v = ir.paramDecl("d" + devId + "__" + name, decl, nodeIdFor(NodeRole::Rack, d.device->id.value, rackParamIndex(name)));
      }
      if (!added.empty()) addStage(added, 0.0, kMacroMax, true, 0.0, true, ModulationTap::Domain::Dsp);
      sites[si].resolved = true;
      sites[si].value = v;
      return v;
    }
    // GU.1 (lower.h "Modulation wires"): a control. Stage 1 in knob position: the Travel wires (amount · source) and,
    // on a curve that is not linear, the Add wires (+1 = the whole travel); the `curve` atom; stage 2 on a linear
    // curve: the Add wires in the curve's units, then its output conversion; stage 3: the migrated Units wires.
    const dsp::Curve curve = controlCurve(*d.device, name, decl);
    const bool inUnits = addsInUnits(curve);
    std::vector<const LWire*> wires, added, units;
    for (const LWire* w : sites[si].wires)
      (w->wire->travels() ? wires : w->wire->law == WireLaw::Units ? units : inUnits ? added : wires).push_back(w);
    // The site's own nodes: keyed by its first wire, else its mapping, else its automation lane.
    const LWire* first = !sites[si].wires.empty() ? sites[si].wires.front() : nullptr;
    auto sid = [&](uint32_t k) {
      if (first != nullptr) return wid(*first, kWTarget + k);
      if (mapping != nullptr) return nodeIdFor(NodeRole::Mapping, mapping->m->id.value, kMCurve);
      return nodeIdFor(NodeRole::Wire, sites[si].lane, kWTarget + k);
    };
    PortRef pk = pos;
    if (mapping == nullptr)  // the knob position (param rate); its default the declared default's position
      pk = ir.param("d" + devId + "__" + name, dsp::curvePosition(declaredCurve(decl), decl.defaultValue.value(), 0.0), 0.0,
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
  if (td[dev].remote) return false;  // IG.1-2: lowered in its host's patch
  if (td[dev].rack < 0) return true;
  const uint32_t pi = rackParamIndex(name);
  if (td.rackParamConsumed(dev, pi)) return true;
  for (const LWire& w : wp.wires)
    if (w.toNode == node && w.toDev == dev && w.toPin == pi && !w.input) return true;
  return false;
}

// IG.1-2: the polyphony and voice stealing of the voice contexts a track's patch holds (model.h "Instrument groups"):
// the largest voice count (an instrument group's `voices` setting — 1 when mono —, else the largest `voices` setting of
// its stock instruments, T4.12-fix1; a lone instrument's own; 0: the link's default) and the first group's stealing.
void contextPolyphony(const TrackDevs& devs, const Callees* lib, uint32_t& polyphony, int32_t& steal) {
  (void)lib;
  polyphony = 0;
  steal = -1;
  // The stock instruments' own `voices` settings (T4.12-fix1; inside racks too: T4.8) …
  for (uint32_t m = 0; m < devs.size(); ++m) {
    const LDev& d = devs[m];
    if (d.rack < 0 && d.callee != nullptr && !isMetaDevice(*d.device) && !isGroupDevice(*d.device) &&
        d.device->kind == DeviceKind::Instrument)
      polyphony = std::max(polyphony, devicePolyphony(d));
  }
  // … and the instrument groups' voice counts (1 when mono).
  for (const uint32_t k : devs.contexts) {
    const LDev& c = devs[k];
    if (!isContextGroup(c)) continue;
    const GroupPolyphony gp = groupPolyphony(*c.device);
    polyphony = std::max(polyphony, gp.mono ? 1u : gp.voices);
    if (steal < 0 && c.device->setting(kGroupSteal) != nullptr) steal = static_cast<int32_t>(gp.steal);
  }
}

Result<graph::Patch> buildPatch(const Plan& pl, uint32_t index, const LowerOptions& o, const TrackDevs& devs,
                                const WirePlan& wp, bool& voices, uint32_t& polyphony, int32_t& steal, uint32_t track,
                                std::vector<ModulationTap>* taps, uint32_t clipSlot,
                                std::vector<LoweredProject::MetaResource>* resources, const Callees* lib,
                                std::vector<CurveSite>* curves) {
  const Node& n = pl.nodes[index];
  Ir ir(patchName(n));
  Build b{ir, o, devs, std::vector<NodeIndex>(devs.size(), graph::kNone), {}};
  b.lib = lib;
  b.wp = &wp;
  b.pl = &pl;  // GU.1
  b.node = index;
  b.track = n.track;  // T4.18
  b.resources = resources;  // TX.spectrum
  b.rackOut.assign(devs.size(), kPending);  // TX.all-free-fix1
  b.rackIn.assign(devs.size(), kPending);
  b.rackInWired.assign(devs.size(), 0);
  b.ctxOut.assign(devs.size(), kPending);  // IG.1-2
  b.outGlobalPin.assign(devs.size(), kPending);
  b.rackDone.assign(devs.size(), kPending);
  for (const LWire& w : wp.wires)
    if (w.toNode == index && w.input && !w.toMixer && devs[w.toDev].rack >= 0) b.rackInWired[w.toDev] = 1;
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
    if (devs[k].rack >= 0) {  // TX.all-free-fix1: a rack off the path
      uint16_t w = graph::kWidthStereo;
      if (devs[k].plays) {  // a lone Instrument Rack: its voices, summed, read by wires
        (void)playInstrumentRack(b, k, w);
      } else if (devs[k].device->kind == DeviceKind::Instrument) {  // IG.1-2: one inside an instrument group
        const Voices v = instrumentRack(b, k, kPending);
        b.rackOut[k] = v.x;
        b.rackDone[k] = v.done;
      } else {  // an Audio Effect Rack: its input the wires into it (zero until one does), its output read by wires
        if (b.freeZero.node == graph::kNone) b.freeZero = ir.konst(-0.0, kCFreeZero);
        b.rackInWired[k] = 1;  // (chainOf makes the pin: add(−0, −0) until wires replace its first term)
        w = graph::kWidthMono;
        const PortRef y = audioRack(b, k, b.freeZero, w);
        if (b.rackOut[k].node == graph::kNone) b.rackOut[k] = y;
      }
      continue;
    }
    if (isContextGroup(devs[k])) {  // IG.1-2: an instrument group off the path (read by its wires)
      uint16_t w = graph::kWidthStereo;
      (void)playGroup(b, k, w);
      continue;
    }
    const NodeIndex c = callDevice(ir, devs[k]);
    b.calls[k] = c;
    if (devs[k].plays) {  // IG.1-2: a lone stock instrument (its outputs per voice, read by wires)
      const auto done = ir.patch().findOutput(c, "done");
      voiceDone(b, done ? PortRef{c, static_cast<uint16_t>(*done)} : kPending, devs[k].device->id.value);
      continue;
    }
    // (A free meta device plays its Nested form: every input has its default already.)
    const bool own = isMetaDevice(*devs[k].device) && !devs[k].bypass;
    if (const uint32_t in = own || devs[k].device->kind == DeviceKind::Instrument ? graph::kNone : audioInIndex(*devs[k].callee);
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
    if (y.node == graph::kNone && d.rack >= 0) y = b.rackOut[r.hostDev];
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
    for (const LWire& w : wp.wires) {
      if (w.toNode != index || !w.toMixer) continue;
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
  lowerSites(b, index, track, wp, taps, curves);  // T4.7, T4.8; GU.1
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
// `track`: the rack's track (its lanes decide whether the mixer lowers as volume / pan params: chainMixerLane).
void chainParams(const Rack& rack, const Chain& c, bool instrumentRack, uint32_t index, const Track* track,
                 std::vector<LoweredParam>& out) {
  const std::string key = "chain" + std::to_string(c.id.value) + ":";
  auto add = [&](std::string_view what, double v, uint32_t ref) {
    out.push_back(LoweredParam{key + std::string(what), index, chainParamName(c.id, what), static_cast<float>(v),
                               ParamRef::chain(c.id, ref)});
  };
  const double pan = std::clamp(static_cast<double>(c.pan), -1.0, 1.0);
  if (track != nullptr && chainMixerLane(*track, c.id).valid()) {  // T4.18: the graph derives the gains
    add("volume", std::clamp(c.volumeDb, kMinVolumeDb, kMaxVolumeDb), kChainVolume);
    add("pan", pan, kChainPan);
  } else {
    const double vg = c.volumeDb <= kMinVolumeDb ? 0.0 : std::pow(10.0, static_cast<double>(c.volumeDb) / 20.0);
    add("gl", vg * std::min(1.0, 1.0 - pan), 0);
    add("gr", vg * std::min(1.0, 1.0 + pan), 0);
  }
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

// GU.1: whether `dev`.`name` (a real param of a lowered leaf device, not mapped) is DRIVEN — a lowered wire into the
// param itself or an automation lane on it — so that its patch has its `curve` atom and its runtime param is the knob
// position (lowerSites' rule; lower.h "Modulation wires").
bool paramDriven(const Plan& pl, const TrackDevs& td, uint32_t dev, std::string_view name, const WirePlan& wp, uint32_t node) {
  const LDev& d = td[dev];
  if (d.rack >= 0 || d.callee == nullptr) return false;
  if (pl.laneOf(d.device->id, name) != 0) return true;
  for (const LWire& w : wp.wires)
    if (w.toNode == node && w.toDev == dev && !w.input && d.callee->nodeName(w.paramNode) == name) return true;
  return false;
}

void addParams(const Plan& pl, uint32_t index, const AudibilityMap& aud, std::vector<LoweredParam>& out,
               const Callees* lib, const WirePlan& wp, const TrackDevs& td) {
  const Node& n = pl.nodes[index];
  const Track& t = *n.track;
  const std::string id = std::to_string(t.id.value);
  // T4.7: the amount of every wire into this track (a param of the target's patch).
  for (const LWire& w : wp.wires)
    if (w.toNode == index && w.wire->travels()) {  // TX.patching: an added wire has no amount
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
      if (d.remote) continue;  // IG.1-2: lowered (with its params) in its host's patch
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
          chainParams(rack, *c.chain, instrument, index, &t, out);
        continue;
      }
      for (graph::NodeIndex v = 0; v < d.callee->nodeCount(); ++v) {
        if (d.callee->node(v).kind != graph::NodeKind::Param) continue;
        const std::string& name = d.callee->nodeName(v);
        if (!declaresParam(td, k, name, wp, index)) continue;
        // GU.1: a driven control's knob position (its curve atom maps it), else its value: the curve at the position,
        // evaluated here (lower.h "Modulation wires": an undriven control has no op).
        const graph::ParamDecl& decl = d.callee->paramDecl(v);
        const bool driven = !d.bypass && paramDriven(pl, td, k, name, wp, index);
        const double value = driven ? paramPosition(*d.device, name, decl) : paramValue(*d.device, name, decl);
        out.push_back(LoweredParam{"dev" + dev + ":" + name + (driven ? std::string(kPositionKeySuffix) : std::string()), index,
                                   "d" + dev + "__" + name, static_cast<float>(value), ParamRef::device(d.device->id, name),
                                   driven});
      }
      // TMM.3: the params of its nested meta modules (unwired at their call: runtime params of the track patch).
      // (TX.groups-fix1: a switched-off device's pass-through has none.)
      if (const auto it = lib->metaSites.find(d.device->id.value); !d.bypass && it != lib->metaSites.end())
        for (const MetaSite& s : *it->second)
          for (size_t i = 0; i < s.type->params.size(); ++i) {
            if (s.wired[i]) continue;
            const std::string_view name = s.type->params[i].name;
            out.push_back(LoweredParam{metaParamKey(d.device->id, s.path, name), index,
                                       metaParamFlatName(d.device->id, s.path, name),
                                       static_cast<float>(metaParamValue(*d.device, s.path, *s.type, i)),
                                       ParamRef::metaParam(d.device->id, s.path, name)});
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
    // T4.18
    case ParamKind::WireAmount: return "wire" + std::to_string(ref.owner) + ":amount";
    case ParamKind::ChainMixer:
      if (ref.target == kChainVolume) return "chain" + std::to_string(ref.owner) + ":volume";
      if (ref.target == kChainPan) return "chain" + std::to_string(ref.owner) + ":pan";
      return {};
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
    if (const Device* d = findDevice(p, DeviceId{lane.param.owner}).device; d != nullptr && !d->rack) k += kPositionKeySuffix;
  return k;
}

LaneId chainMixerLane(const Track& t, ChainId chain) noexcept {
  LaneId out;
  for (const LanePtr& l : t.lanes)
    if (l->param.kind == ParamKind::ChainMixer && l->param.owner == chain.value && (!out.valid() || l->id < out))
      out = l->id;
  return out;
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
  UserCallees uc = makeCallees(q, cables, o.library, o.devices, true, &o.metaSlotOf);  // T4.4-fix1; TMM.3 meta modules
  const Callees* lib = &uc.callees;
  const Plan pl = plan(q, &o.tuningTableOf, lib);
  const AudibilityMap aud = computeAudibility(q, AudibilityOptions{true});
  LoweredProject out;
  out.graph.library = lib->lib;
  out.userLibrary = std::move(uc.combined);
  out.userDevices = std::move(uc.held);
  out.deviceErrors = std::move(uc.errors);
  out.metaPatches = std::move(uc.meta);
  out.metaResources = std::move(uc.metaResources);
  const std::vector<TrackDevs> devs = allDevices(pl, lib, &out.missingDevices);
  PortDeps deps(lib->lib);
  const WirePlan wp = planWires(q, pl, devs, deps, lib);  // T4.7
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
                            &out.curves);
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
  const UserCallees uc = makeCallees(c->q, c, library, nullptr, !wiresOf(c->q).empty());
  return paramsWith(c->q, tuningNodes, &uc.callees);
}

std::vector<LoweredParam> lowerParams(const Project& p, bool tuningNodes, const DeviceLibrary& devices) {
  std::shared_ptr<const Cables> c = cablesOf(p, &devices.library(), &devices);
  const UserCallees uc = makeCallees(c->q, c, &devices.library(), &devices, !wiresOf(c->q).empty());
  return paramsWith(c->q, tuningNodes, &uc.callees);
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
  std::shared_ptr<const Cables> cables = cablesOf(p, &library, devices);
  const UserCallees uc = makeCallees(cables->q, cables, &library, devices, true);  // T4.4-fix1: user devices inside it play their text
  lowerList(td, std::vector<DevicePtr>{shown}, instrument, &uc.callees, nullptr, td.top);
  Ir ir((instrument ? "InstrumentRack" : "AudioEffectRack") + std::to_string(rackId.value));
  LowerOptions o;
  o.library = uc.callees.lib;
  o.devices = devices;
  Build b{ir, o, td, std::vector<NodeIndex>(td.size(), graph::kNone), {}};
  b.ctxOut.assign(td.size(), kPending);
  b.rackDone.assign(td.size(), kPending);
  b.outGlobalPin.assign(td.size(), kPending);
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
  lowerSites(b, 0, 0, WirePlan{}, nullptr, nullptr);
  if (!ir.finish()) return Error{ErrorCode::Overflow, "rackSubcircuit: a document id too large for a stable node id", {}};
  return std::move(ir.patch());
}

std::vector<LoweredParam> chainParamValues(const Project& p, ChainId chain) {
  std::vector<LoweredParam> out;
  const ChainLoc c = findChain(p, chain);
  if (!c.chain) return out;
  const Rack& rack = *c.rack->rack;
  const bool instrument = c.rack->kind == DeviceKind::Instrument;
  chainParams(rack, *c.chain, instrument, 0, c.track, out);
  return out;
}

std::vector<WireState> wireStates(const Project& p, const graph::PatchLibrary& library) {
  if (wiresOf(p).empty()) return {};
  std::shared_ptr<const Cables> c = cablesOf(p, &library, nullptr);
  return wireStatesWith(*c, makeCallees(c->q, c, &library, nullptr, true).callees);
}

std::vector<WireState> wireStates(const Project& p, const DeviceLibrary& devices) {
  if (wiresOf(p).empty()) return {};
  std::shared_ptr<const Cables> c = cablesOf(p, &devices.library(), &devices);
  return wireStatesWith(*c, makeCallees(c->q, c, &devices.library(), &devices, true).callees);
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
