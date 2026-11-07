// TX.all-free (SPEC §15 2026-11-03 "everything is free; the chain is just cables"): a track's main path
// (pq/document/main_path.h), the edit builders that rewire it (insert, move, dissolve, splice, take off), the schema
// 16 -> 17 migration (a chain becomes main links), and what the lowering makes of the wiring (one rule: a broken path is
// silent, the mixer sums the other wires into it, a track's own signal feeds a block off the path).
#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/document/lower.h"
#include "pq/document/main_path.h"
#include "pq/document/meta.h"
#include "pq/document/serialize.h"
#include "pq/document/modules.h"
#include "pq/document/store.h"
#include "../support/blocks.h"

using namespace pq::doc;
using pq::Result;

namespace {

std::string stdlibDir() { return std::string(PQ_TEST_DATA_DIR) + "/../stdlib"; }

const DeviceLibrary& library() {
  static std::unique_ptr<DeviceLibrary> lib = [] {
    auto r = loadDeviceLibrary(stdlibDir());
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    return std::move(*r);
  }();
  return *lib;
}

struct Doc {
  DocumentStore store;
  TrackId keys;
  DeviceId inst;
  Doc() {
    keys = edit(ops::createTrack(doc(), store.ids(), TrackKind::Midi, {}, "Keys")).tracks.at(0);
    inst = mainPath(doc(), track(keys)).blocks.at(0)->id;  // the default instrument group
  }
  const Project& doc() const { return *store.current(); }
  const Track& track(TrackId t) const { return *findTrackPtr(doc(), t); }
  // The main path of `t` by block name (a module group's is the module's; signal order) and whether it reaches the mixer.
  std::vector<std::string> path(TrackId t) const {
    std::vector<std::string> v;
    for (const Device* d : mainPath(doc(), track(t)).blocks) v.push_back(d->name.empty() ? d->patch : d->name);
    return v;
  }
  bool complete(TrackId t) const { return mainPath(doc(), track(t)).complete; }
  ops::Edit edit(const Result<ops::Edit>& e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    const size_t before = store.history().cursor();
    REQUIRE(store.dispatch(e->command).ok());
    CHECK(store.history().cursor() == before + (e->empty() ? 0u : 1u));  // one undo step
    return *e;
  }
  // A built-in module's group preset (GU.11: "utility", "saturator", "vca", "lfo" ...).
  DeviceId add(const char* module, uint32_t slot = kAppend) {
    return edit(ops::addDevice(doc(), store.ids(), keys, std::string(kModuleItemPrefix) + module, DeviceKind::AudioEffect, slot))
        .devices.at(0);
  }
  WireId wire(Wire w) { return edit(ops::connect(doc(), store.ids(), std::move(w))).wires.at(0); }
};

Wire link(DeviceId from, DeviceId to) {
  Wire w;
  w.from = from;
  w.to = to;
  return w;
}
Wire intoMixer(DeviceId from, TrackId t) {
  Wire w;
  w.from = from;
  w.mixer = t;
  return w;
}

LoweredProject lowered(const Project& p) {
  LowerOptions o;
  o.library = &library().library();
  o.devices = &library();
  o.slotOf = [](TrackId) { return 0u; };
  auto r = lower(p, o);
  REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
  return std::move(*r);
}
const pq::graph::Patch& patchOf(const LoweredProject& l, TrackId t) {
  for (size_t i = 0; i < l.trackIds.size(); ++i)
    if (l.trackIds[i] == t) return l.patches[i]->patch;
  FAIL("no such track");
  return l.patches[0]->patch;
}

}  // namespace

TEST_CASE("main path: a new track is wired source → instrument → mixer; the default project's master too") {
  Doc d;
  CHECK(d.path(d.keys) == std::vector<std::string>{"Basic Synth"});
  CHECK(d.complete(d.keys));
  const MainPath p = mainPath(d.doc(), d.track(d.keys));
  REQUIRE(p.links.size() == 2);
  const Wire* in = findWire(d.doc(), p.links[0]);
  const Wire* out = findWire(d.doc(), p.links[1]);
  // IG.1-2: through the instrument group (GU.10: In → Phase Accumulator → Wavetable → Envelope → Out inside it: its
  // inner wires, every one inside the group).
  const DeviceId g0 = d.inst;
  CHECK(isGroupDevice(*findDevice(d.doc(), g0).device));
  CHECK((in->fromNoteSource() && in->noteSource == d.keys && in->to == g0 && in->mainLink()));
  CHECK((out->from == g0 && out->toMixer() && out->mixer == d.keys && out->mainLink()));
  auto inside = [&](DeviceId x) { return x == g0 || groupContains(d.track(d.keys).devices, g0, x); };
  size_t inner = 0;
  for (const Wire& w : wiresOf(d.doc()))
    if (!w.mainLink() || (w.from.valid() && w.to.valid())) {
      ++inner;
      CHECK((inside(w.from) && inside(w.to)));
    }
  CHECK(inner >= 4);
  // Every other kind starts with its source straight into its mixer.
  const TrackId g = d.edit(ops::createTrack(d.doc(), d.store.ids(), TrackKind::Group)).tracks.at(0);
  CHECK(d.path(g).empty());
  CHECK(d.complete(g));
  CHECK(mainPath(d.doc(), *d.doc().master).complete);
  // A list position means nothing: the blocks' order in the track is not the path's.
  const DeviceId u = d.add("utility");
  const DeviceId s = d.add("saturator", 1);  // between the synth and the Utility on the path
  CHECK(d.path(d.keys) == std::vector<std::string>{"Basic Synth", "Saturator", "Utility"});
  CHECK(d.track(d.keys).deviceIndex(s) > d.track(d.keys).deviceIndex(u));  // appended to the set
  CHECK(flowOrder(d.track(d.keys), mainPath(d.doc(), d.track(d.keys))).at(1)->id == s);
  (void)u;
}

TEST_CASE("main path: branches, merges, a broken path, group members, loops") {
  Doc d;
  const DeviceId a = d.add("utility");
  const DeviceId b = d.add("saturator", ops::kOffPath);
  const DeviceId c = d.add("vca", ops::kOffPath);
  CHECK(d.path(d.keys) == std::vector<std::string>{"Basic Synth", "Utility"});
  // A branch: the Utility's main output also into the VCA, the VCA into the mixer — the path keeps its oldest link that
  // reaches the mixer (the Utility's own link into the mixer).
  d.wire(link(a, c));
  d.wire(intoMixer(c, d.keys));
  CHECK(d.path(d.keys) == std::vector<std::string>{"Basic Synth", "Utility"});
  // Its link into the mixer gone: the branch is the path now (it reaches the mixer).
  const MainPath p = mainPath(d.doc(), d.track(d.keys));
  d.edit(ops::deleteWires(d.doc(), std::vector<WireId>{p.links.back()}));
  CHECK(d.path(d.keys) == std::vector<std::string>{"Basic Synth", "Utility", "VCA"});
  CHECK(d.complete(d.keys));
  // Broken: the VCA's link into the mixer gone — the path is what is still on it, and it does not reach the mixer.
  const MainPath q = mainPath(d.doc(), d.track(d.keys));
  d.edit(ops::deleteWires(d.doc(), std::vector<WireId>{q.links.back()}));
  CHECK(d.path(d.keys) == std::vector<std::string>{"Basic Synth", "Utility", "VCA"});
  CHECK_FALSE(d.complete(d.keys));
  // A loop back into a block already on the path is not followed.
  d.wire(link(c, d.inst));
  CHECK(d.path(d.keys) == std::vector<std::string>{"Basic Synth", "Utility", "VCA"});
  // A group's member is never on the path (it is inside its group).
  const DeviceId g = d.edit(ops::addDevice(d.doc(), d.store.ids(), d.keys, std::string(kGroupDevice), DeviceKind::AudioEffect,
                                           ops::kOffPath)).devices.at(0);
  d.wire(intoMixer(c, d.keys));
  REQUIRE(d.complete(d.keys));
  d.edit(ops::setDeviceGroup(d.doc(), d.store.ids(), &library(), b, g));
  d.wire(link(c, b));  // a main link from the path into a member: the member stays off the path
  CHECK(d.path(d.keys) == std::vector<std::string>{"Basic Synth", "Utility", "VCA"});
}

TEST_CASE("main path: insert, move, dissolve, take off, splice — each one undo step, the path stays connected") {
  Doc d;
  const DeviceId u = d.add("utility");
  const DeviceId s = d.add("saturator");
  const DeviceId e = d.add("vca");
  const DeviceId ev = test_blocks::member(d.doc(), e, kVcaValueName);  // the VCA's control
  REQUIRE(ev.valid());
  CHECK(d.path(d.keys) == std::vector<std::string>{"Basic Synth", "Utility", "Saturator", "VCA"});
  // Move (the Device view's reorder): the VCA to slot 1 (counted with it in its old place).
  d.edit(ops::moveOnPath(d.doc(), d.store.ids(), &library(), e, d.keys, 1));
  CHECK(d.path(d.keys) == std::vector<std::string>{"Basic Synth", "VCA", "Utility", "Saturator"});
  CHECK(d.complete(d.keys));
  // Right by one (slot after its right neighbour).
  d.edit(ops::moveOnPath(d.doc(), d.store.ids(), &library(), e, d.keys, 3));
  CHECK(d.path(d.keys) == std::vector<std::string>{"Basic Synth", "Utility", "VCA", "Saturator"});
  // Where it is: no edit.
  CHECK(ops::moveOnPath(d.doc(), d.store.ids(), &library(), e, d.keys, 2)->empty());
  CHECK(ops::moveOnPath(d.doc(), d.store.ids(), &library(), e, d.keys, 3)->empty());
  // Dissolve (the Device view's Delete): it goes, the path closes up; its modulation wire goes with it.
  const DeviceId lfo = d.add("lfo", ops::kOffPath);
  d.edit(ops::connectWire(d.doc(), d.store.ids(), lfo, std::string(kMainPort), ev, std::string(kValueParam)));
  d.edit(ops::dissolveDevices(d.doc(), d.store.ids(), &library(), std::vector<DeviceId>{e}));
  CHECK(d.path(d.keys) == std::vector<std::string>{"Basic Synth", "Utility", "Saturator"});
  CHECK(d.complete(d.keys));
  for (const Wire& w : wiresOf(d.doc())) CHECK((w.from != e && w.to != e && w.from != ev && w.to != ev));
  REQUIRE(d.store.undo());
  CHECK(d.path(d.keys) == std::vector<std::string>{"Basic Synth", "Utility", "VCA", "Saturator"});
  // Take off the path: the block stays (with its other wires), its neighbours are joined.
  d.edit(ops::takeOffPath(d.doc(), d.store.ids(), &library(), std::vector<DeviceId>{u}));
  CHECK(d.path(d.keys) == std::vector<std::string>{"Basic Synth", "VCA", "Saturator"});
  CHECK(findDevice(d.doc(), u).device != nullptr);
  // Insert it back at the end (the free rack's block dropped onto the list).
  d.edit(ops::insertOnPath(d.doc(), d.store.ids(), u, kAppend));
  CHECK(d.path(d.keys) == std::vector<std::string>{"Basic Synth", "VCA", "Saturator", "Utility"});
  // Splice a block off the path into a link of it (the circuit's Shift-drag): the link into the Saturator.
  const DeviceId v = d.add("utility", ops::kOffPath);
  const MainPath p = mainPath(d.doc(), d.track(d.keys));
  d.edit(ops::spliceIntoWire(d.doc(), d.store.ids(), &library(), v, p.links[2]));
  CHECK(d.path(d.keys) == std::vector<std::string>{"Basic Synth", "VCA", "Utility", "Saturator", "Utility"});
  // Splicing a block of the path elsewhere moves it (it leaves its place first).
  const MainPath p2 = mainPath(d.doc(), d.track(d.keys));
  d.edit(ops::spliceIntoWire(d.doc(), d.store.ids(), &library(), s, p2.links[1]));
  CHECK(d.path(d.keys) == std::vector<std::string>{"Basic Synth", "Saturator", "VCA", "Utility", "Utility"});
  CHECK(d.complete(d.keys));
  // Into a modulation wire: the block goes between its ends (off the path).
  const WireId mod = findWire(d.doc(), [&] {
                       for (const Wire& w : wiresOf(d.doc()))
                         if (w.from == lfo) return w.id;
                       return WireId{};
                     }())->id;
  // (GU.4: the VCA module, a group, by its main ports.)
  const DeviceId vca = d.edit(ops::pasteBlocks(d.doc(), d.store.ids(), d.keys, moduleBlocks(ModuleKind::Vca))).devices.at(0);
  REQUIRE(mod.valid());
  d.edit(ops::spliceIntoWire(d.doc(), d.store.ids(), &library(), vca, mod));
  CHECK_FALSE(mainPath(d.doc(), d.track(d.keys)).contains(vca));
  bool lfoToVca = false, vcaToEq = false;
  for (const Wire& w : wiresOf(d.doc())) {
    lfoToVca = lfoToVca || (w.from == lfo && w.port.empty() && w.to == vca && w.param.empty());
    vcaToEq = vcaToEq || (w.from == vca && w.port.empty() && w.to == ev && w.param == kValueParam);
  }
  CHECK(lfoToVca);
  CHECK(vcaToEq);
}

TEST_CASE("main path: canonicalWire writes two main ports as kMainPort, so a hand-drawn cable joins the path") {
  Doc d;
  // (GU.11: two atom blocks; tanh's main ports are its `x` and `out`.)
  const DeviceId t = d.edit(ops::addDevice(d.doc(), d.store.ids(), d.keys, "atom:tanh", DeviceKind::AudioEffect, ops::kOffPath)).devices.at(0);
  const DeviceId u = d.edit(ops::addDevice(d.doc(), d.store.ids(), d.keys, "atom:tanh", DeviceKind::AudioEffect, ops::kOffPath)).devices.at(0);
  Wire w;
  w.from = t;
  w.port = "out";
  w.to = u;
  w.param = "x";
  const Wire c = canonicalWire(d.doc(), w, &library());
  CHECK(c.mainLink());
  Wire p = w;
  p.param = "gain";  // a param stays a param
  CHECK(canonicalWire(d.doc(), p, &library()).param == "gain");
  CHECK(canonicalWire(d.doc(), p, &library()).port.empty());
  CHECK(canonicalWire(d.doc(), w, nullptr).port == "out");  // without the library: as it is
}

TEST_CASE("main path: the 16 -> 17 migration wires every chain in series, strips free, keeps a wired track") {
  nlohmann::json j = nlohmann::json::parse(R"({
    "schemaVersion": 16,
    "timeline": {"tempo": [{"tick": 0, "bpm": 120.0, "curve": "step"}], "signatures": [{"bar": 0, "numerator": 4, "denominator": 4}]},
    "loop": {"start": 0, "end": 3548160, "enabled": false},
    "locators": [],
    "tunings": [{"id": 1, "name": "12-EDO", "colour": "#5E9BD6", "def": {"kind": "edo", "n": 12, "equave": "2/1", "referenceHz": 261.626}}],
    "tracks": [
      {"id": 3, "kind": "midi", "name": "Keys", "colour": "#808080", "active": true, "tuning": "inherit",
       "mixer": {"volumeDb": 0.0, "pan": 0.0, "mute": false, "solo": false, "arm": false, "sends": [], "output": "default"},
       "lanes": [], "clips": [],
       "devices": [{"id": 4, "patch": "meta:wavetable", "kind": "instrument"}, {"id": 5, "patch": "Value", "kind": "audioEffect", "free": true},
                   {"id": 6, "patch": "Value", "kind": "audioEffect"}]},
      {"id": 7, "kind": "group", "name": "G", "colour": "#808080", "active": true, "tuning": "inherit",
       "mixer": {"volumeDb": 0.0, "pan": 0.0, "mute": false, "solo": false, "arm": false, "sends": [], "output": "default"},
       "lanes": [], "children": [], "devices": []}
    ],
    "returns": [],
    "master": {"id": 2, "kind": "master", "name": "Master", "colour": "#808080", "active": true, "tuning": "inherit",
      "mixer": {"volumeDb": 0.0, "pan": 0.0, "mute": false, "solo": false, "arm": false, "sends": [], "output": "default"},
      "devices": []},
    "wires": [{"id": 8, "from": 5, "port": "y", "to": 6, "param": "value"}]
  })");
  Result<LoadedProject> r = parseProject(j.dump(2));
  REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
  CHECK(r->fileSchemaVersion == 16);
  const Project& p = *r->project;
  const Track& keys = *findTrackPtr(p, TrackId{3});
  const MainPath path = mainPath(p, keys);
  REQUIRE(path.blocks.size() == 2);  // the engine → block 6 (block 5 was free: off the path)
  // (IG.1-2: the 18 -> 19 step then wraps the engine into an instrument group, id 14, its chain links 15 and 16.)
  CHECK(isGroupDevice(*path.blocks[0]));
  CHECK(findDevice(p, DeviceId{4}).device->group == path.blocks[0]->id);
  CHECK(path.blocks[1]->id == DeviceId{6});
  CHECK(path.complete);
  CHECK(mainPath(p, *findTrackPtr(p, TrackId{7})).complete);  // an empty chain: source → mixer
  CHECK(mainPath(p, *p.master).complete);
  // New ids above the file's largest, in track order (Keys, G, master); the old wire is kept.
  std::vector<uint32_t> ids;
  for (const Wire& w : wiresOf(p)) ids.push_back(w.id.value);
  CHECK(ids == std::vector<uint32_t>{8, 9, 10, 11, 12, 13, 15, 16});
  // It re-saves (without "free") and reads back the same.
  Result<std::string> again = serializeProject(p);
  REQUIRE(again.ok());
  CHECK(again->find("\"free\"") == std::string::npos);
  Result<LoadedProject> back = parseProject(*again);
  REQUIRE(back.ok());
  CHECK(*back->project == p);
}

TEST_CASE("main path: the lowering — a broken path is silent, the mixer sums the wires into it, a track's own signal "
          "feeds a block off the path, notes cabled into a block off it play it, a mixer takes other tracks' blocks") {
  Doc d;
  const DeviceId u = d.add("utility");
  const LoweredProject full = lowered(d.doc());
  const std::string playing = pq::graph::print(patchOf(full, d.keys));
  // Broken: the Utility's link into the mixer gone — nothing reaches the mixer (its input is a constant 0).
  const MainPath p = mainPath(d.doc(), d.track(d.keys));
  d.edit(ops::deleteWires(d.doc(), std::vector<WireId>{p.links.back()}));
  const LoweredProject broken = lowered(d.doc());
  for (const WireState& s : broken.wires) CHECK(s.status == WireStatus::Lowered);
  CHECK(pq::graph::print(patchOf(broken, d.keys)) != playing);
  // Wired into the mixer again, but from the instrument group directly (the Utility a dead end): the mixer hears it.
  d.wire(intoMixer(d.inst, d.keys));
  const LoweredProject dry = lowered(d.doc());
  for (const WireState& s : dry.wires) CHECK(s.status == WireStatus::Lowered);
  // The Utility back on the path as well: the path's end and the dry wire, summed (two adds into one mixer).
  d.wire(intoMixer(u, d.keys));
  REQUIRE(d.complete(d.keys));
  const LoweredProject both = lowered(d.doc());
  for (const WireState& s : both.wires) CHECK(s.status == WireStatus::Lowered);
  CHECK(pq::graph::print(patchOf(both, d.keys)) != playing);
  // A group track's own signal into a block off its path: lowered (the block reads the sum).
  const TrackId g = d.edit(ops::createTrack(d.doc(), d.store.ids(), TrackKind::Group)).tracks.at(0);
  const DeviceId side = d.edit(ops::addDevice(d.doc(), d.store.ids(), g, std::string(kModuleItemPrefix) + "utility",
                                              DeviceKind::AudioEffect, ops::kOffPath)).devices.at(0);
  Wire src;
  src.noteSource = g;
  src.to = side;
  const WireId sw = d.wire(src);
  Wire back = intoMixer(side, g);
  const WireId bw = d.wire(back);
  const LoweredProject gl = lowered(d.doc());
  for (const WireState& s : gl.wires)
    if (s.id == sw || s.id == bw) CHECK(s.status == WireStatus::Lowered);
  // A MIDI track's notes into a block off its path (TX.all-free-fix1; before it, Inactive: the notes reached a block only
  // along the path): lowered — the Wavetable's phase reads the note's phase (GU.4: was a Phase accumulator's Hz).
  const DeviceId pa = d.edit(ops::addMetaDevice(d.doc(), d.store.ids(), d.keys, "wavetable", ops::kOffPath)).devices.at(0);
  Wire notes;
  notes.noteSource = d.keys;
  notes.to = pa;
  const WireId nw = d.wire(notes);
  for (const WireState& s : lowered(d.doc()).wires)
    if (s.id == nw) CHECK(s.status == WireStatus::Lowered);
  // TX.all-free-fix1: a mixer takes another track's block too (a send-like route; before fix1 it was refused).
  Wire cross = intoMixer(u, g);
  auto crossEdit = ops::connect(d.doc(), d.store.ids(), cross);
  const bool made = crossEdit.ok() && d.store.dispatch(crossEdit->command).ok();
  CHECK(made);
  for (const WireState& s : lowered(d.doc()).wires)
    if (s.id == crossEdit->wires.at(0)) CHECK(s.status == WireStatus::Lowered);
}
