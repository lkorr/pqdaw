// GU.2 (docs/plans/GU.md "Phases" 2): groups nest (the model's rules, remove / move with everything inside, nest /
// unnest round trips), atom and value blocks (their patches, descriptions, rules, lowering), block sets (copy with the
// wires among them, id remap, paste in one step, group preset files and their refusals).
#include <doctest/doctest.h>

#include <algorithm>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/document/instrument_groups.h"
#include "pq/document/library_files.h"
#include "pq/document/lower.h"
#include "pq/document/main_path.h"
#include "pq/document/serialize.h"
#include "pq/document/store.h"
#include "pq/graph/patch.h"

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
  Doc() {
    keys = edit(ops::createTrack(doc(), store.ids(), TrackKind::Midi, {}, "Keys")).tracks.at(0);
  }
  const Project& doc() const { return *store.current(); }
  const Track& track() const { return *findTrackPtr(doc(), keys); }
  ops::Edit edit(const Result<ops::Edit>& e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    const size_t before = store.history().cursor();
    REQUIRE(store.dispatch(e->command).ok());
    CHECK(store.history().cursor() == before + (e->empty() ? 0u : 1u));  // one undo step
    return *e;
  }
  void cmd(Command c) {
    const pq::Status st = store.dispatch(std::move(c));
    REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
  }
  DeviceId block(std::string patch, DeviceId group = {}) {
    const DeviceId d = edit(ops::addDevice(doc(), store.ids(), keys, std::move(patch), DeviceKind::AudioEffect, ops::kOffPath)).devices.at(0);
    if (group.valid()) cmd(SetDeviceGroup{d, group});
    return d;
  }
  WireId wire(DeviceId from, std::string port, DeviceId to, std::string param) {
    return edit(ops::connectWire(doc(), store.ids(), from, std::move(port), to, std::move(param))).wires.at(0);
  }
  DeviceId group() const { return mainPath(doc(), track()).blocks.at(0)->id; }
};

}  // namespace

TEST_CASE("blocks: groups nest — at any depth, never inside themselves, at most kMaxGroupDepth deep; a group goes and "
          "moves with everything inside it") {
  Doc d;
  const DeviceId g = d.group();  // the default instrument group (Basic Synth: its three modules inside)
  const size_t deep0 = groupMembersDeep(d.track().devices, g).size(), direct0 = groupMembers(d.track().devices, g).size();
  CHECK(direct0 == 3);
  const DeviceId a = d.block(std::string(kGroupDevice), g);
  const DeviceId b = d.block(std::string(kGroupDevice), a);
  const DeviceId x = d.block("atom:tanh", b);
  const std::vector<DevicePtr>& list = d.track().devices;
  CHECK(groupContains(list, g, x));
  CHECK(groupContains(list, a, x));
  CHECK_FALSE(groupContains(list, x, g));
  CHECK(outermostGroup(list, x) == g);
  CHECK(groupMembersDeep(list, g).size() == deep0 + 3);  // + a, b, x
  CHECK(groupMembers(list, g).size() == direct0 + 1);    // + a
  // A cycle is refused (the command fails, nothing changes).
  const Snapshot before = d.store.current();
  CHECK_FALSE(d.store.dispatch(SetDeviceGroup{a, b}).ok());
  CHECK(d.store.current().get() == before.get());
  CHECK_FALSE(d.store.dispatch(SetDeviceGroup{a, a}).ok());
  // Depth: kMaxGroupDepth groups inside one another hold; one more is refused.
  {
    Doc e;
    DeviceId at = e.group();
    for (uint32_t k = 1; k < kMaxGroupDepth; ++k) at = e.block(std::string(kGroupDevice), at);
    const DeviceId deepest = e.block("atom:tanh");
    e.cmd(SetDeviceGroup{deepest, at});
    const DeviceId more = e.block("atom:tanh");
    CHECK_FALSE(e.store.dispatch(SetDeviceGroup{more, deepest}).ok());  // (not a group anyway)
    const DeviceId tooDeep = e.block(std::string(kGroupDevice));
    e.cmd(SetDeviceGroup{tooDeep, at});
    const DeviceId leaf = e.block("atom:tanh");
    CHECK_FALSE(e.store.dispatch(SetDeviceGroup{leaf, tooDeep}).ok());
  }
  // Moving the outer group to another track takes everything inside with it; removing it removes it all (one step).
  const TrackId other = d.edit(ops::createTrack(d.doc(), d.store.ids(), TrackKind::Midi, {}, "Other")).tracks.at(0);
  d.edit(ops::moveDevice(d.doc(), g, other, kAppend));
  const Track& o = *findTrackPtr(d.doc(), other);
  CHECK(o.deviceIndex(g) >= 0);
  CHECK(o.deviceIndex(a) >= 0);
  CHECK(o.deviceIndex(b) >= 0);
  CHECK(o.deviceIndex(x) >= 0);
  CHECK(d.track().deviceIndex(x) < 0);
  const DeviceId del[] = {a};
  d.edit(ops::deleteDevices(d.doc(), del));
  CHECK(findDevice(d.doc(), b).device == nullptr);
  CHECK(findDevice(d.doc(), x).device == nullptr);
  REQUIRE(d.store.undo());
  CHECK(findDevice(d.doc(), x).device != nullptr);
}

TEST_CASE("blocks: Ctrl+G inside a capsule nests (wires as they are), Ctrl+Shift+G gives the very document back; a "
          "selection across capsules and an instrument group are refused") {
  Doc d;
  const DeviceId g = d.group();
  const DeviceId m = d.block("atom:mul", g);
  const DeviceId t = d.block("atom:tanh", g);
  d.wire(m, "out", t, "x");
  const Snapshot flat = d.store.current();
  const DeviceId sel[] = {m, t};
  const ops::Edit e = d.edit(ops::groupInCircuit(d.store.current(), d.store.ids(), library(), sel));
  const DeviceId n = e.devices.at(0);
  CHECK(findDevice(d.doc(), n).device->group == g);
  CHECK(findDevice(d.doc(), m).device->group == n);
  CHECK(wiresOf(d.doc()) == wiresOf(*flat));
  // Nesting again (the nested group and nothing else, one level deeper still).
  const DeviceId sel2[] = {n};
  const DeviceId n2 = d.edit(ops::groupInCircuit(d.store.current(), d.store.ids(), library(), sel2)).devices.at(0);
  CHECK(findDevice(d.doc(), n).device->group == n2);
  d.edit(ops::ungroupInCircuit(d.store.current(), d.store.ids(), library(), n2));
  d.edit(ops::ungroupInCircuit(d.store.current(), d.store.ids(), library(), n));
  CHECK(*d.store.current() == *flat);
  // Refused: blocks of two capsules; the instrument group itself.
  const DeviceId other = d.block(std::string(kGroupDevice));
  const DeviceId z = d.block("atom:tanh", other);
  const DeviceId across[] = {m, z};
  CHECK_FALSE(ops::groupInCircuit(d.store.current(), d.store.ids(), library(), across).ok());
  const DeviceId inst[] = {g};
  CHECK_FALSE(ops::groupInCircuit(d.store.current(), d.store.ids(), library(), inst).ok());
}

TEST_CASE("blocks: atom blocks are their atom (ports, settings, no params); a value block is one control as its "
          "output; both lower inline, an undriven value block is a runtime param with no op") {
  const pq::graph::Patch* mul = atomBlockPatch("mul");
  REQUIRE(mul != nullptr);
  CHECK(mul->name() == "Atom_mul");
  const DeviceInfo* mi = atomBlockInfo("mul");
  REQUIRE(mi != nullptr);
  CHECK(mi->patch == "atom:mul");
  CHECK(mi->kind == DeviceKind::AudioEffect);
  CHECK(mi->params.empty());
  CHECK(atomBlockPatch("mul") == mul);  // built once
  CHECK(library().library().find("Atom_mul") == mul);  // the device library holds every atom block
  // GU.9 (the user: zones are voice atoms as atom blocks): a voice terminal is a block; outside a voice it plays its
  // stand-in, every output 0.
  CHECK(atomBlockPatch("voice.hz") != nullptr);
  REQUIRE(voicelessAtomPatch("voice.hz") != nullptr);
  CHECK(voicelessAtomPatch("voice.hz")->name() == "Atom_voice_hz__voiceless");
  CHECK(voicelessAtomPatch("mul") == nullptr);
  CHECK(atomBlockPatch("no_such_atom") == nullptr);
  CHECK(atomBlockPatch("curve") == nullptr);
  // EQ.1: the svf filter is a block; its mode, stages and prewarp are settings (consts), x / cutoff / q / gain ports.
  const DeviceInfo* svf = atomBlockInfo("svf");
  REQUIRE(svf != nullptr);
  REQUIRE(svf->settings.size() == 3);
  CHECK(svf->settings[0].name == "mode");
  CHECK(svf->settings[1].name == "stages");
  CHECK(svf->settings[2].name == "prewarp");
  REQUIRE(atomBlockPatch("svf") != nullptr);
  CHECK(pq::graph::subcircuitSignature(*atomBlockPatch("svf")).inputs.size() == 7);
  const pq::graph::SubcircuitRef sig = pq::graph::subcircuitSignature(*mul);
  REQUIRE(sig.inputs.size() == 2);
  CHECK(sig.inputs[0].name == "a");
  CHECK(sig.inputs[1].name == "b");
  REQUIRE(sig.outputs.size() == 1);
  CHECK(sig.outputs[0].name == "out");
  // Every allowed atom builds (and validates).
  size_t blocks = 0;
  for (const pq::graph::AtomSignature& s : pq::graph::AtomRegistry::builtin().all())
    if (atomBlockAllowed(s)) {
      CHECK_MESSAGE(atomBlockPatch(s.name) != nullptr, s.name);
      ++blocks;
    }
  MESSAGE(blocks << " atoms can be blocks");
  CHECK(blocks > 50);
  const DeviceInfo& vi = valueDeviceInfo();
  REQUIRE(vi.params.size() == 1);
  CHECK(vi.params[0].name == kValueParam);
  // Rules: an atom block has no params, a value block only `value`.
  Doc d;
  const DeviceId a = d.block("atom:tanh");
  CHECK_FALSE(d.store.dispatch(SetDeviceParam{a, "x", 0.5}).ok());
  const DeviceId v = d.block(std::string(kValueDevice));
  d.cmd(SetDeviceParam{v, std::string(kValueParam), 0.25});
  CHECK_FALSE(d.store.dispatch(SetDeviceParam{v, "other", 0.5}).ok());
  // Lowered: the value block's param is a runtime param (undriven: no op), the atoms are called by their patches.
  const DeviceId g = d.group();
  const DeviceId m = d.block("atom:mul", g);
  d.wire(v, "y", m, "b");
  LowerOptions lo;
  lo.library = &library().library();
  lo.devices = &library();
  auto low = lower(d.doc(), lo);
  REQUIRE_MESSAGE(low.ok(), (low.ok() ? std::string() : low.error().message));
  CHECK(low->missingDevices.empty());
  bool param = false;
  for (const LoweredParam& p : low->params)
    if (p.name == "d" + std::to_string(v.value) + "__value") {
      param = true;
      CHECK(p.value == doctest::Approx(0.25));
    }
  CHECK(param);
}

TEST_CASE("blocks: copy takes a group with everything inside and the wires among them; remap gives fresh ids that "
          "every reference follows; paste is one step; a group preset file round trips and refuses foreign wires") {
  Doc d;
  const DeviceId g = d.group();
  const DeviceId n = d.block(std::string(kGroupDevice), g);
  const DeviceId m = d.block("atom:mul", n);
  const DeviceId v = d.block(std::string(kValueDevice), n);
  const DeviceId out = d.block("atom:tanh");  // outside the group: its cable from m is not part of the set
  d.wire(v, "y", m, "b");
  d.wire(n, std::string(kGroupInPort), m, "a");
  d.wire(m, "out", n, std::string(kGroupOutPort));
  d.wire(m, "out", out, "x");
  const DeviceId pick[] = {n};
  Result<BlockSet> set = ops::copyBlocks(d.doc(), pick);
  REQUIRE(set.ok());
  REQUIRE(set->devices.size() == 3);
  CHECK(set->devices[0]->id == n);
  CHECK_FALSE(set->devices[0]->group.valid());  // its group is not copied: a top-level block of the set
  CHECK(set->wires.size() == 3);
  // Remap: every id fresh, group tags and wire ends follow.
  std::vector<std::pair<DeviceId, DeviceId>> remap;
  const BlockSet fresh = ops::remapBlocks(*set, d.store.ids(), &remap);
  REQUIRE(remap.size() == 3);
  auto to = [&](DeviceId id) {
    for (const auto& [a, b] : remap)
      if (a == id) return b;
    return DeviceId{};
  };
  for (const auto& [a, b] : remap) {
    CHECK(a != b);
    CHECK(findDevice(d.doc(), b).device == nullptr);
  }
  CHECK(fresh.devices[1]->group == to(n));
  CHECK(fresh.devices[2]->group == to(n));
  for (size_t i = 0; i < fresh.wires.size(); ++i) {
    CHECK(fresh.wires[i].from == to(set->wires[i].from));
    CHECK(fresh.wires[i].to == to(set->wires[i].to));
    CHECK(findWire(d.doc(), fresh.wires[i].id) == nullptr);
  }
  // Paste into the instrument group: one step, its top-level block a member there; undo gives the document back.
  const Snapshot before = d.store.current();
  const ops::Edit e = d.edit(ops::pasteBlocks(d.doc(), d.store.ids(), d.keys, *set, g));
  REQUIRE(e.devices.size() == 3);
  CHECK(findDevice(d.doc(), e.devices[0]).device->group == g);
  CHECK(e.wires.size() == 3);
  REQUIRE(d.store.undo());
  CHECK(d.store.current().get() == before.get());
  // Ctrl+V into a group of another track: refused (a group of this track only).
  const TrackId other = d.edit(ops::createTrack(d.doc(), d.store.ids(), TrackKind::Midi, {}, "Other")).tracks.at(0);
  CHECK_FALSE(ops::pasteBlocks(d.doc(), d.store.ids(), other, *set, g).ok());
  // A group preset: the set (its group first) written and read back equal.
  auto text = serializeGroupPreset(*set, "Nested");
  REQUIRE(text.ok());
  CHECK(text->find("\"wires\"") != std::string::npos);
  auto back = parsePreset(*text);
  REQUIRE_MESSAGE(back.ok(), (back.ok() ? std::string() : back.error().message));
  CHECK(back->groupPreset());
  CHECK(back->blocks == *set);
  // A one-device preset stays what it was (no wires key, one device).
  auto plain = serializePreset(*findDevice(d.doc(), m).device, "Mul");
  REQUIRE(plain.ok());
  CHECK(plain->find("\"wires\"") == std::string::npos);
  auto pb = parsePreset(*plain);
  REQUIRE(pb.ok());
  CHECK_FALSE(pb->groupPreset());
  // Refused: a group preset whose first device is not a group; a wire to a device outside the file.
  {
    BlockSet bad = *set;
    std::swap(bad.devices[0], bad.devices[1]);
    CHECK_FALSE(serializeGroupPreset(bad, "Bad").ok());
  }
  {
    BlockSet foreign = *set;
    Wire w = foreign.wires.front();
    w.id = WireId{w.id.value + 1000};
    w.to = out;
    foreign.wires.push_back(w);
    auto t2 = serializeGroupPreset(foreign, "Foreign");
    REQUIRE(t2.ok());
    CHECK_FALSE(parsePreset(*t2).ok());
  }
}
