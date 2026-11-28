// FD.2 (docs/plans/FD.md "FD.2"; pq/document/lanes.h, modules.h "lane cables", edit_ops.h setLaneCount):
//   modules     Lanes / Merge / Unpack / Pack: catalogue keys, the blocks of every N (aliases only, Merge's N − 2 adds
//               and 2 multiplies), their settings and faces (the N chip, lane jacks bound to the lane blocks),
//               laneModuleInfo; spawned through `module:<key>` at 8 lanes
//   estimate    laneWidths: nothing without a pack block; pack sums, channel is 1, atoms and groups pass the widest
//               cable on, a lane cable into a control makes it per lane (a stereo one does not, nor one from another
//               track), an engine's declared width, an analyser passes; a feedback loop converges; a stereo loop
//               through a pack (a Utility's output into its own Gain) stays stereo; the bound groups
//   N           setLaneCount is one undo step that round-trips save / load, keeps the lane blocks (and cables) of the
//               lanes that stay and the module's own cables, rebuilds the rest; refusals
//   lowering    a lane cable into a control keeps every lane (no channel pick), a stereo one its first; a lane cable at
//               the mixer goes through StereoOnly; a bound group plays its input-bound variant; a project without lane
//               cables has neither
//   reasons     laneRefusal's words for the compiler's width errors
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../support/blocks.h"
#include "pq/document/commands.h"
#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/document/lanes.h"
#include "pq/document/lower.h"
#include "pq/document/model.h"
#include "pq/document/modules.h"
#include "pq/document/panel.h"
#include "pq/document/serialize.h"
#include "pq/document/store.h"
#include "pq/graph/atoms.h"

using namespace pq::doc;

namespace {

const DeviceLibrary& library() {
  static std::unique_ptr<DeviceLibrary> lib = [] {
    auto r = loadDeviceLibrary(std::string(PQ_TEST_DATA_DIR) + "/../stdlib");
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
    return std::move(*r);
  }();
  return *lib;
}

struct Doc {
  DocumentStore store;
  TrackId track;
  Doc() { track = edit(ops::createTrack(doc(), store.ids(), TrackKind::Audio, {}, "A")).tracks.at(0); }
  const Project& doc() const { return *store.current(); }
  ops::Edit edit(const pq::Result<ops::Edit>& e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    const pq::Status st = store.dispatch(e->command);
    REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
    return *e;
  }
  // `e` as one undo step: undo gives back the project before, redo the one after, which saves and loads as itself.
  ops::Edit step(const pq::Result<ops::Edit>& e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    const Snapshot before = store.current();
    const size_t cursor = store.history().cursor();
    const pq::Status st = store.dispatch(e->command);
    REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
    CHECK(store.history().cursor() == cursor + 1);
    const Snapshot after = store.current();
    REQUIRE(store.undo());
    CHECK(*store.current() == *before);
    REQUIRE(store.redo());
    CHECK(*store.current() == *after);
    pq::Result<std::string> text = serializeProject(doc());
    REQUIRE_MESSAGE(text.ok(), (text.ok() ? std::string() : text.error().message));
    pq::Result<LoadedProject> back = parseProject(*text);
    REQUIRE_MESSAGE(back.ok(), (back.ok() ? std::string() : back.error().message));
    CHECK(*back->project == doc());
    return *e;
  }
  const Device& dev(DeviceId d) const { return *findDevice(doc(), d).device; }
  // A lane module of `n` lanes pasted off the path: its group.
  DeviceId lanes(LaneModule kind, uint32_t n, TrackId on = {}) {
    IdSource local;
    LaneModuleOptions o;
    o.lanes = n;
    return edit(ops::pasteBlocks(doc(), store.ids(), on.valid() ? on : track, laneModuleBlocks(local, kind, o))).devices.at(0);
  }
  DeviceId module(std::string_view key) { return test_blocks::addModule(store, track, key, ops::kOffPath); }
  DeviceId atom(std::string_view name) {
    return edit(ops::addDevice(doc(), store.ids(), track, std::string(kAtomDevicePrefix) + std::string(name), DeviceKind::AudioEffect,
                               ops::kOffPath))
        .devices.at(0);
  }
  WireId wire(DeviceId from, std::string_view port, DeviceId to, std::string_view param, WireLaw law = WireLaw::Add) {
    Wire w;
    w.id = store.ids().wire();
    w.from = from;
    w.port = std::string(port);
    w.to = to;
    w.param = std::string(param);
    w.law = law;
    const pq::Status st = store.dispatch(AddWire{w});
    REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
    return w.id;
  }
  LaneModuleInfo info(DeviceId g) const {
    const std::optional<LaneModuleInfo> m = laneModuleInfo(doc(), g);
    REQUIRE(m.has_value());
    return *m;
  }
  LoweredProject lowered() const {
    LowerOptions lo;
    lo.library = &library().library();
    lo.devices = &library();
    lo.clipSlotOf = [](TrackId) { return 0u; };
    auto low = lower(doc(), lo);
    REQUIRE_MESSAGE(low.ok(), (low.ok() ? std::string() : low.error().message));
    return std::move(*low);
  }
};

// The atom blocks of a block set by name.
std::multiset<std::string> atomsOf(const BlockSet& set) {
  std::multiset<std::string> a;
  for (const DevicePtr& d : set.devices)
    if (isAtomDevice(*d)) a.insert(std::string(atomDeviceName(*d)));
  return a;
}
std::multiset<std::string> many(std::initializer_list<std::pair<const char*, uint32_t>> counts) {
  std::multiset<std::string> a;
  for (const auto& [name, n] : counts)
    for (uint32_t i = 0; i < n; ++i) a.insert(name);
  return a;
}
uint32_t log2of(uint32_t n) {
  uint32_t k = 0;
  while ((1u << k) < n) ++k;
  return k;
}

// The subcircuit calls of every lowered track patch, by callee name.
std::map<std::string, uint32_t> callsOf(const LoweredProject& low) {
  std::map<std::string, uint32_t> calls;
  for (const auto& lp : low.patches)
    for (pq::graph::NodeIndex n = 0; n < lp->patch.nodeCount(); ++n)
      if (lp->patch.node(n).kind == pq::graph::NodeKind::Subcircuit) ++calls[lp->patch.subcircuit(n).patch];
  return calls;
}
// The `channel` atoms the lowering itself put in the track patches (the blocks' own are inside their callees).
uint32_t channelAtoms(const LoweredProject& low) {
  uint32_t count = 0;
  for (const auto& lp : low.patches)
    for (pq::graph::NodeIndex n = 0; n < lp->patch.nodeCount(); ++n)
      if (lp->patch.node(n).kind == pq::graph::NodeKind::Atom && pq::graph::atomSignature(lp->patch.node(n).atom).name == "channel") ++count;
  return count;
}

}  // namespace

TEST_CASE("FD.2 lane modules: catalogue keys, the blocks of every N, settings, faces, laneModuleInfo") {
  const std::pair<const char*, LaneModule> kinds[] = {
      {"lanes", LaneModule::Lanes}, {"merge", LaneModule::Merge}, {"unpack", LaneModule::Unpack}, {"lane_pack", LaneModule::Pack}};
  CHECK(laneMergeGain(2) == 1.0);
  CHECK(laneMergeGain(8) == 0.5);
  CHECK(laneMergeGain(16) == doctest::Approx(1.0 / std::sqrt(8.0)));
  for (const auto& [key, kind] : kinds) {
    CAPTURE(key);
    const ModuleInfo* m = findModule(key);
    REQUIRE(m != nullptr);
    CHECK(m->name == laneModuleName(kind));
    CHECK(findModule(m->name) == m);
    CHECK_FALSE(m->instrument);
    // Spawned through the catalogue key: 8 lanes.
    {
      Doc d;
      const DeviceId g = d.module(key);
      const LaneModuleInfo li = d.info(g);
      CHECK(li.kind == kind);
      CHECK(li.lanes == kDefaultLaneCount);
      CHECK(d.dev(g).name == laneModuleName(kind));
      for (const WireState& s : wireStates(d.doc(), library())) CHECK(s.status == WireStatus::Lowered);
      (void)d.lowered();
    }
    for (const uint32_t n : kLaneCounts) {
      CAPTURE(n);
      IdSource ids;
      LaneModuleOptions o;
      o.lanes = n;
      LaneModuleRoles r;
      const BlockSet set = laneModuleBlocks(ids, kind, o, &r);
      const Device& g = *set.devices.front();
      REQUIRE(isGroupDevice(g));
      CHECK(g.id == r.group);
      REQUIRE(g.setting(kLaneModuleSetting) != nullptr);
      CHECK(g.setting(kLaneModuleSetting)->value == static_cast<double>(kind));
      REQUIRE(g.setting(kLaneCountSetting) != nullptr);
      CHECK(g.setting(kLaneCountSetting)->value == n);
      CHECK(g.settings.size() == 2);
      // The blocks: `pack` and `channel` are aliases (no op); Merge alone computes.
      uint32_t values = 0;
      for (const DevicePtr& x : set.devices) values += isValueDevice(*x) ? 1u : 0u;
      switch (kind) {
        case LaneModule::Lanes:
          CHECK(atomsOf(set) == many({{"channel", 2}, {"pack", log2of(n)}}));
          CHECK(values == 0);
          CHECK(r.lane.empty());
          break;
        case LaneModule::Merge:
          CHECK(atomsOf(set) == many({{"channel", n}, {"add", n - 2}, {"mul", n > 2 ? 2u : 0u}, {"pack", 1}}));
          CHECK(values == (n > 2 ? 1u : 0u));  // the gain 1 / sqrt(N / 2)
          break;
        case LaneModule::Unpack:
          CHECK(atomsOf(set) == many({{"channel", n}}));
          REQUIRE(r.lane.size() == n);
          break;
        case LaneModule::Pack:
          CHECK(atomsOf(set) == many({{"channel", n}, {"pack", n - 1}}));
          REQUIRE(r.lane.size() == n);
          break;
        case LaneModule::None: break;
      }
      // The face: the N chip on the lane count; Unpack / Pack: a main jack and a jack per lane on its lane block.
      REQUIRE(g.hasPanel());
      const PanelElement* chip = nullptr;
      uint32_t laneJacks = 0, jacks = 0;
      for (const PanelElement& e : g.panel->elements) {
        if (e.kind == "lanecount") chip = &e;
        if (e.kind != "jack") continue;
        ++jacks;
        if (e.id < kLaneJackBase || e.id >= kLaneJackBase + kMaxLaneCount) continue;
        const uint32_t i = e.id - kLaneJackBase;
        REQUIRE(i < r.lane.size());
        REQUIRE(e.binding(0) != nullptr);
        CHECK(e.binding(0)->device == r.lane[i]);
        CHECK(e.binding(0)->kind == (kind == LaneModule::Unpack ? BindingKind::Signal : BindingKind::Input));
        ++laneJacks;
      }
      REQUIRE(chip != nullptr);
      CHECK(chip->id == kLaneChipElement);
      REQUIRE(chip->binding(0) != nullptr);
      CHECK(*chip->binding(0) == settingBinding(r.group, std::string(kLaneCountSetting)));
      const bool hasJacks = kind == LaneModule::Unpack || kind == LaneModule::Pack;
      CHECK(laneJacks == (hasJacks ? n : 0u));
      CHECK(jacks == (hasJacks ? n + 1 : 0u));
      // Pasted into a project: the checks accept it, laneModuleInfo finds its lanes, everything lowers.
      Doc d;
      const DeviceId pasted = d.edit(ops::pasteBlocks(d.doc(), d.store.ids(), d.track, set)).devices.at(0);
      const LaneModuleInfo li = d.info(pasted);
      CHECK(li.kind == kind);
      CHECK(li.lanes == n);
      CHECK(li.roles.lane.size() == r.lane.size());
      CHECK(li.roles.inner.size() == set.devices.size() - 1 - r.lane.size());
      for (const WireState& s : wireStates(d.doc(), library())) CHECK(s.status == WireStatus::Lowered);
      (void)d.lowered();
    }
  }
  // Not a lane module: any other group.
  Doc d;
  CHECK_FALSE(laneModuleInfo(d.doc(), d.module("vca")).has_value());
}

TEST_CASE("FD.2 laneWidths: nothing without a pack block; lanes pass atoms, groups and controls; reducers and engines end them") {
  {  // no pack block: no pass at all
    Doc d;
    const DeviceId vca = d.module("vca"), tap = d.module("delay_tap");
    d.wire(vca, kMainPort, tap, kMainPort);
    CHECK(laneWidths(d.doc()).empty());
  }
  Doc d;
  const DeviceId fan = d.lanes(LaneModule::Lanes, 8);
  const DeviceId vca = d.module("vca"), merge = d.lanes(LaneModule::Merge, 8);
  const WireId a = d.wire(fan, kMainPort, vca, kMainPort);
  const WireId b = d.wire(vca, kMainPort, merge, kMainPort);
  const DeviceId after = d.module("vca");
  const WireId c = d.wire(merge, kMainPort, after, kMainPort);
  {
    const LaneWidths lw = laneWidths(d.doc());
    CHECK(lw.of(a) == 8);
    CHECK(lw.of(b) == 8);
    CHECK(lw.of(c) == 0);  // merged: stereo
    CHECK(lw.bound.empty());
    // Inside the VCA: its In terminal's cable into the multiplier, and the multiplier's into Out.
    const DeviceId mul = test_blocks::member(d.doc(), vca, "\xC3\x97");
    REQUIRE(mul.valid());
    for (const Wire& w : wiresOf(d.doc())) {
      if (w.from == vca && w.port == kGroupInPort) CHECK(lw.of(w.id) == 8);
      if (w.from == mul) CHECK(lw.of(w.id) == 8);
      if (w.from == test_blocks::member(d.doc(), vca, std::string(kVcaValueName))) CHECK(lw.of(w.id) == 0);  // its control: mono
    }
  }
  // A lane cable into a control makes the control, and what it feeds, per lane; a stereo one (a `pack` of two) does not.
  const DeviceId vca2 = d.module("vca"), value2 = test_blocks::member(d.doc(), vca2, std::string(kVcaValueName));
  const DeviceId sink = d.module("vca");
  const WireId out2 = d.wire(vca2, kMainPort, sink, kMainPort);
  const DeviceId stereo = d.atom("pack");
  const WireId narrow = d.wire(stereo, "out", value2, kValueParam);
  CHECK(laneWidths(d.doc()).of(narrow) == 0);
  CHECK(laneWidths(d.doc()).of(out2) == 0);
  const WireId wide = d.wire(fan, kMainPort, value2, kValueParam);
  CHECK(laneWidths(d.doc()).of(wide) == 8);
  CHECK(laneWidths(d.doc()).of(out2) == 8);
  // Unpack's lanes are mono; a Pack of them is the lane cable again; two lane cables packed add up.
  const DeviceId un = d.lanes(LaneModule::Unpack, 8), pk = d.lanes(LaneModule::Pack, 4);
  d.wire(fan, kMainPort, un, kMainPort);
  const LaneModuleInfo ui = d.info(un), pi = d.info(pk);
  const WireId one = d.wire(ui.roles.lane[5], "out", pi.roles.lane[0], "x");
  const DeviceId both = d.atom("pack"), sink2 = d.atom("neg");
  const WireId packed = d.wire(pk, kMainPort, both, "in0");
  d.wire(fan, kMainPort, both, "in1");
  const WireId sum = d.wire(both, "out", sink2, "x");
  {
    const LaneWidths lw = laneWidths(d.doc());
    CHECK(lw.of(one) == 0);
    CHECK(lw.of(packed) == 4);
    CHECK(lw.of(sum) == 12);
  }
  // A native engine's output is what it declares (the Envelope: mono); an analyser passes its input on.
  const DeviceId env = d.module("envelope"), scope = d.module("oscilloscope"), sink3 = d.atom("neg"), sink4 = d.atom("neg");
  d.wire(fan, kMainPort, env, kMainPort);
  d.wire(fan, kMainPort, scope, kMainPort);
  const WireId fromEnv = d.wire(env, kMainPort, sink3, "x");
  const WireId fromScope = d.wire(scope, kMainPort, sink4, "x");
  {
    const LaneWidths lw = laneWidths(d.doc());
    CHECK(lw.of(fromEnv) == 0);
    CHECK(lw.of(fromScope) == 8);
    // The Envelope module's In ends on its engine alone: a lane cable into it binds it to stereo.
    CHECK(lw.boundOf(env) == 2);
    CHECK(lw.boundOf(scope) == 0);
    CHECK(lw.boundOf(vca) == 0);
    CHECK(lw.boundOf(merge) == 0);  // 8 lanes into a Merge 8
    CHECK(lw.boundOf(un) == 0);
  }
}

TEST_CASE("FD.2 laneWidths: a feedback loop converges; a stereo loop through a pack stays stereo; bound groups; across tracks") {
  {  // Lanes 4 -> add -> Delay Tap -> VCA -> back into the add
    Doc d;
    const DeviceId fan = d.lanes(LaneModule::Lanes, 4), sum = d.atom("add"), tap = d.module("delay_tap"), vca = d.module("vca");
    d.wire(fan, kMainPort, sum, "a");
    const WireId in = d.wire(sum, "out", tap, kMainPort);
    d.wire(tap, kMainPort, vca, kMainPort);
    const WireId back = d.wire(vca, kMainPort, sum, "b");
    const LaneWidths lw = laneWidths(d.doc());
    CHECK(lw.of(in) == 4);
    CHECK(lw.of(back) == 4);
  }
  {  // a Utility's (stereo) output into its own Gain: the control takes the first channel, the loop stays stereo
    Doc d;
    const DeviceId util = d.module("utility"), gain = test_blocks::member(d.doc(), util, "Gain");
    REQUIRE(gain.valid());
    const DeviceId src = d.atom("pack");
    d.wire(src, "out", util, kMainPort);
    const WireId loop = d.wire(util, kMainPort, gain, kValueParam);
    const LaneWidths lw = laneWidths(d.doc());
    CHECK(lw.of(loop) == 0);
    CHECK(lw.empty());
    (void)d.lowered();
  }
  {  // bound groups: a lane cable into a Utility (it reads L and R), into Lanes, into a Merge / Unpack of another N
    Doc d;
    const DeviceId fan4 = d.lanes(LaneModule::Lanes, 4);
    const DeviceId util = d.module("utility"), fan8 = d.lanes(LaneModule::Lanes, 8), merge8 = d.lanes(LaneModule::Merge, 8);
    const DeviceId un8 = d.lanes(LaneModule::Unpack, 8), merge4 = d.lanes(LaneModule::Merge, 4), eq = d.module("eq"), comp = d.module("compressor");
    for (const DeviceId to : {util, fan8, merge8, un8, merge4, eq, comp}) d.wire(fan4, kMainPort, to, kMainPort);
    const LaneWidths lw = laneWidths(d.doc());
    CHECK(lw.boundOf(util) == 2);
    CHECK(lw.boundOf(fan8) == 2);
    CHECK(lw.boundOf(merge8) == 8);
    CHECK(lw.boundOf(un8) == 8);
    CHECK(lw.boundOf(merge4) == 0);  // its own N
    CHECK(lw.boundOf(eq) == 0);      // it also passes In on (Stereo mode): lanes pass
    CHECK(lw.boundOf(comp) == 0);
    CHECK(lw.boundOf(fan4) == 0);    // nothing reaches it
  }
  {  // a stereo cable into a Merge 8 / Unpack 8 with no pack block upstream is bound too (a channel past the second)
    Doc d;
    const DeviceId un8 = d.lanes(LaneModule::Unpack, 8), util = d.module("utility");
    d.wire(util, kMainPort, un8, kMainPort);
    CHECK(laneWidths(d.doc()).boundOf(un8) == 8);
  }
  {  // across tracks a control gets a cable's first lane: what it feeds is not a lane cable
    Doc d;
    const TrackId other = d.edit(ops::createTrack(d.doc(), d.store.ids(), TrackKind::Audio, {}, "B")).tracks.at(0);
    const DeviceId fan = d.lanes(LaneModule::Lanes, 8);
    const DeviceId vca = test_blocks::addModule(d.store, other, "vca", ops::kOffPath);
    const DeviceId sink = test_blocks::addModule(d.store, other, "vca", ops::kOffPath);
    const WireId across = d.wire(fan, kMainPort, test_blocks::member(d.doc(), vca, std::string(kVcaValueName)), kValueParam);
    const WireId on = d.wire(vca, kMainPort, sink, kMainPort);
    const LaneWidths lw = laneWidths(d.doc());
    CHECK(lw.of(across) == 8);  // the cable itself is 8 lanes at its source
    CHECK(lw.of(on) == 0);
    (void)d.lowered();
  }
}

TEST_CASE("FD.2 setLaneCount: one undo step, save / load; the lanes that stay keep their blocks and cables; refusals") {
  Doc d;
  const DeviceId fan = d.lanes(LaneModule::Lanes, 8), un = d.lanes(LaneModule::Unpack, 8), pk = d.lanes(LaneModule::Pack, 8);
  const DeviceId merge = d.lanes(LaneModule::Merge, 8), vca = d.module("vca");
  const WireId intoUnpack = d.wire(fan, kMainPort, un, kMainPort);
  const WireId intoMerge = d.wire(pk, kMainPort, merge, kMainPort);
  const WireId outOfMerge = d.wire(merge, kMainPort, vca, kMainPort);
  const LaneModuleInfo u0 = d.info(un), p0 = d.info(pk);
  REQUIRE(u0.roles.lane.size() == 8);
  REQUIRE(p0.roles.lane.size() == 8);
  std::vector<WireId> lane;  // Unpack lane i -> Pack lane i
  for (size_t i = 0; i < 8; ++i) lane.push_back(d.wire(u0.roles.lane[i], "out", p0.roles.lane[i], "x"));
  auto has = [&](WireId w) { return findWire(d.doc(), w) != nullptr; };

  // Unpack 8 -> 4: lanes 0 … 3 keep their blocks and cables, 4 … 7 go with theirs; its own input cable stays.
  const ops::Edit e = d.step(ops::setLaneCount(d.doc(), d.store.ids(), un, 4));
  CHECK(d.store.lastChanges().structural());
  const LaneModuleInfo u1 = d.info(un);
  CHECK(u1.lanes == 4);
  REQUIRE(u1.roles.lane.size() == 4);
  for (size_t i = 0; i < 4; ++i) CHECK(u1.roles.lane[i] == u0.roles.lane[i]);
  for (size_t i = 4; i < 8; ++i) CHECK(findDevice(d.doc(), u0.roles.lane[i]).device == nullptr);
  for (size_t i = 0; i < 8; ++i) CHECK(has(lane[i]) == (i < 4));
  CHECK(has(intoUnpack));
  CHECK(e.devices.empty());  // nothing new: four channel blocks fewer
  CHECK(d.dev(un).setting(kLaneCountSetting)->value == 4.0);
  uint32_t jacks = 0;
  for (const PanelElement& x : d.dev(un).panel->elements) jacks += x.kind == "jack" && x.id >= kLaneJackBase ? 1u : 0u;
  CHECK(jacks == 4);
  // Undo brings the lanes and their cables back (step() compared the whole project).
  REQUIRE(d.store.undo());
  for (size_t i = 0; i < 8; ++i) CHECK(has(lane[i]));
  REQUIRE(d.store.redo());

  // Unpack 4 -> 16: the four stay, twelve are new.
  const ops::Edit grow = d.step(ops::setLaneCount(d.doc(), d.store.ids(), un, 16));
  const LaneModuleInfo u2 = d.info(un);
  REQUIRE(u2.roles.lane.size() == 16);
  for (size_t i = 0; i < 4; ++i) CHECK(u2.roles.lane[i] == u0.roles.lane[i]);
  CHECK(grow.devices.size() == 12);
  for (size_t i = 0; i < 4; ++i) CHECK(has(lane[i]));
  CHECK(d.dev(u2.roles.lane[0]).setting("index") == nullptr);  // (channel 0: the block's default)
  for (size_t i = 1; i < 16; ++i) {
    REQUIRE(d.dev(u2.roles.lane[i]).setting("index") != nullptr);
    CHECK(d.dev(u2.roles.lane[i]).setting("index")->value == static_cast<double>(i));
  }

  // Pack 8 -> 2: lanes 0, 1 keep their input blocks and the cables into them; the pack tree is rebuilt; its output
  // cable stays.
  d.step(ops::setLaneCount(d.doc(), d.store.ids(), pk, 2));
  const LaneModuleInfo p1 = d.info(pk);
  REQUIRE(p1.roles.lane.size() == 2);
  CHECK(p1.roles.lane[0] == p0.roles.lane[0]);
  CHECK(p1.roles.lane[1] == p0.roles.lane[1]);
  CHECK(has(lane[0]));
  CHECK(has(lane[1]));
  CHECK_FALSE(has(lane[2]));
  CHECK(has(intoMerge));
  CHECK(p1.roles.inner.size() == 1);  // one pack
  CHECK(laneWidths(d.doc()).of(intoMerge) == 0);  // two lanes: a stereo cable

  // Lanes / Merge: the inside rebuilt for the new N, the cables on their own In and Out kept.
  d.step(ops::setLaneCount(d.doc(), d.store.ids(), merge, 4));
  CHECK(has(intoMerge));
  CHECK(has(outOfMerge));
  {
    const LaneModuleInfo m = d.info(merge);
    CHECK(m.lanes == 4);
    std::multiset<std::string> atoms;
    for (const DeviceId x : m.roles.inner)
      if (isAtomDevice(d.dev(x))) atoms.insert(std::string(atomDeviceName(d.dev(x))));
    CHECK(atoms == many({{"channel", 4}, {"add", 2}, {"mul", 2}, {"pack", 1}}));
  }
  d.step(ops::setLaneCount(d.doc(), d.store.ids(), fan, 16));
  CHECK(has(intoUnpack));
  CHECK(laneWidths(d.doc()).of(intoUnpack) == 16);
  // The same N again rebuilds a hand-edited inside.
  const DeviceId extra = d.atom("neg");
  REQUIRE(d.store.dispatch(SetDeviceGroup{extra, fan}).ok());
  d.step(ops::setLaneCount(d.doc(), d.store.ids(), fan, 16));
  CHECK(findDevice(d.doc(), extra).device == nullptr);
  CHECK(d.info(fan).roles.inner.size() == 2 + 4);
  for (const WireState& s : wireStates(d.doc(), library())) CHECK(s.status == WireStatus::Lowered);
  (void)d.lowered();
  // Refusals.
  CHECK_FALSE(ops::setLaneCount(d.doc(), d.store.ids(), fan, 3).ok());
  CHECK_FALSE(ops::setLaneCount(d.doc(), d.store.ids(), fan, 32).ok());
  CHECK_FALSE(ops::setLaneCount(d.doc(), d.store.ids(), vca, 8).ok());
}

TEST_CASE("FD.2 lowering: a lane cable keeps its lanes into a control; the mixer's bound; a bound group's variant; "
          "nothing of it without lane cables") {
  // Without lane cables: Utility -> VCA on the path, a stereo cable into the VCA's control.
  Doc plain;
  {
    const DeviceId util = test_blocks::addModule(plain.store, plain.track, "utility", 0);
    const DeviceId vca = test_blocks::addModule(plain.store, plain.track, "vca", 1);
    plain.wire(util, kMainPort, test_blocks::member(plain.doc(), vca, std::string(kVcaValueName)), kValueParam);
  }
  const LoweredProject lowPlain = plain.lowered();
  const std::map<std::string, uint32_t> callsPlain = callsOf(lowPlain);
  CHECK(callsPlain.count("StereoOnly") == 0);
  for (const auto& [name, n] : callsPlain) CHECK_FALSE(name.starts_with("GroupIn"));
  CHECK(channelAtoms(lowPlain) == 1);  // the stereo cable's first channel into the control

  // The same with Lanes 8 between them: the cable into the control keeps every lane (no channel pick), the path's end
  // is a lane cable (through StereoOnly: the compiler will refuse it there), the Utility after it would be bound.
  Doc d;
  const DeviceId fan = test_blocks::addModule(d.store, d.track, "lanes", 0);
  const DeviceId vca = test_blocks::addModule(d.store, d.track, "vca", 1);
  const WireId drive = d.wire(fan, kMainPort, test_blocks::member(d.doc(), vca, std::string(kVcaValueName)), kValueParam);
  CHECK(laneWidths(d.doc()).of(drive) == 8);
  {
    const LoweredProject low = d.lowered();
    const std::map<std::string, uint32_t> calls = callsOf(low);
    REQUIRE(calls.count("StereoOnly") == 1);
    CHECK(calls.at("StereoOnly") == 1);
    // No channel pick on the cable (its NodeRole::Wire slot 3, kWChannel); the two channel atoms are the modulation
    // tap's first lane (up, down).
    bool picked = false;
    for (const auto& lp : low.patches) picked = picked || lp->patch.indexOf(pq::graph::NodeId{nodeIdFor(NodeRole::Wire, drive.value, 3)}).has_value();
    CHECK_FALSE(picked);
    CHECK(channelAtoms(low) == 2);
  }
  const DeviceId util = test_blocks::addModule(d.store, d.track, "utility", 2);
  {
    CHECK(laneWidths(d.doc()).boundOf(util) == 2);
    const LoweredProject low = d.lowered();
    const std::map<std::string, uint32_t> calls = callsOf(low);
    CHECK(calls.count("GroupIn2") == 1);
    CHECK(calls.count("StereoOnly") == 0);  // the Utility's output is stereo again
  }
  // Merged before the mixer: neither.
  REQUIRE(d.store.undo());
  const DeviceId merge = test_blocks::addModule(d.store, d.track, "merge", 2);
  (void)merge;
  {
    const LoweredProject low = d.lowered();
    const std::map<std::string, uint32_t> calls = callsOf(low);
    CHECK(calls.count("StereoOnly") == 0);
    for (const auto& [name, n] : calls) CHECK_FALSE(name.starts_with("GroupIn"));
  }
}

TEST_CASE("FD.2 laneRefusal: the compiler's width errors in the document's words") {
  CHECK(laneRefusal("patch 'pqUnit': width mismatch: ch(8) into stereo input 'x' of 'd24' (subcircuit GroupIn2, #32), from (atom pack, #3)") ==
        "8 lanes into a stereo input: merge the lanes first (a Merge module)");
  CHECK(laneRefusal("width mismatch: ch(4) into mono 'out' (out, #9)") ==
        "4 lanes into a mono input: merge the lanes first (Merge), or take one lane (Unpack)");
  CHECK(laneRefusal("width mismatch: ch(4) into ch(8) input 'x' of 'd3'") == "4 lanes into a module set to 8 lanes: set its lane count to 4");
  CHECK(laneRefusal("width mismatch: stereo into ch(8) input 'x' of 'd3'") ==
        "A stereo signal into a module set to 8 lanes: fan the signal out first (Lanes), or set its lane count to 2");
  CHECK(laneRefusal("width mismatch: ch(8) and stereo meet at (atom add, #4) (only mono broadcasts)") ==
        "8 lanes and a stereo signal meet here: only a mono signal joins a lane cable (fan the other out with Lanes, or merge the lanes first)");
  CHECK(laneRefusal("width mismatch: stereo and ch(16) meet at (atom add, #4)").starts_with("16 lanes and a stereo signal meet here"));
  CHECK(laneRefusal("width mismatch: ch(4) and ch(8) meet at (atom add, #4)").starts_with("8 lanes and 4 lanes meet here"));
  // Not a lane error: the compiler's own words stay.
  CHECK(laneRefusal("width mismatch: stereo into mono input 'x' of 'd3'").empty());
  CHECK(laneRefusal("kind mismatch: real into bool input 'a'").empty());
  CHECK(laneRefusal("").empty());
}
