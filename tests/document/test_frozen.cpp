// FZ.1 (docs/plans/RB.md D2, D4; model.h "Frozen tracks", lower.h "Frozen tracks"): Track::frozen in the file,
// SetTrackFrozen, the one apply rule that refuses edits of a frozen track's content (and lets the mixer, the name, the
// colour, tempo and Unfreeze through), and the lowering of a frozen track from its render: one clip player + the mixer,
// the blocks that reach another track kept, cross-track cables into its mixer not doubled, the same IR without a slot.
#include <doctest/doctest.h>

#include <algorithm>
#include <cctype>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "pq/document/commands.h"
#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/document/lower.h"
#include "pq/document/main_path.h"
#include "pq/document/serialize.h"
#include "pq/document/store.h"
#include "../support/blocks.h"

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

constexpr uint32_t kSlot = 7;  // the frozen render's clip slot the tests hand out

struct Doc {
  DocumentStore store;
  TrackId keys, bass;
  Doc() {
    keys = must(ops::createTrack(doc(), store.ids(), TrackKind::Midi, {}, "Keys")).tracks.at(0);
    bass = must(ops::createTrack(doc(), store.ids(), TrackKind::Midi, keys, "Bass")).tracks.at(0);
  }
  const Project& doc() const { return *store.current(); }
  const Track& track(TrackId t) const { return *findTrackPtr(doc(), t); }
  ops::Edit must(const ops::EditResult& e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    const pq::Status st = store.dispatch(e->command);
    REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
    return *e;
  }
  void cmd(const Command& c) {
    const pq::Status st = store.dispatch(c);
    REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
  }
  WireId wire(Wire w) { return must(ops::connect(doc(), store.ids(), std::move(w))).wires.at(0); }
  // The default instrument group heading `t`'s path.
  DeviceId instrument(TrackId t) const { return mainPath(doc(), track(t)).blocks.at(0)->id; }
};

LowerOptions options(bool frozenSlots = false, std::vector<TrackId> capture = {}) {
  LowerOptions lo;
  lo.library = &library().library();
  lo.devices = &library();
  lo.slotOf = [](TrackId) { return 0u; };
  lo.clipSlotOf = [](TrackId) { return 1u; };
  if (frozenSlots) lo.frozenSlotOf = [](TrackId) { return kSlot; };
  if (!capture.empty())
    lo.captureOwn = [capture](TrackId t) { return std::find(capture.begin(), capture.end(), t) != capture.end(); };
  return lo;
}

LoweredProject lowered(const Project& p, const LowerOptions& lo) {
  auto r = lower(p, lo);
  REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
  return std::move(*r);
}

std::vector<std::string> texts(const LoweredProject& l) {
  std::vector<std::string> v;
  for (const auto& p : l.patches) v.push_back(patchText(*p));
  return v;
}

uint32_t indexOf(const LoweredProject& l, TrackId t) {
  for (uint32_t i = 0; i < l.trackIds.size(); ++i)
    if (l.trackIds[i] == t) return i;
  FAIL("no such track");
  return 0;
}
const pq::graph::Patch& patchOf(const LoweredProject& l, TrackId t) { return l.patches[indexOf(l, t)]->patch; }

// The device ids a patch calls (subcircuit nodes named d<id>).
std::set<uint32_t> calledDevices(const pq::graph::Patch& p) {
  std::set<uint32_t> out;
  for (uint32_t v = 0; v < p.nodeCount(); ++v) {
    const std::string& n = p.nodeName(v);
    if (p.node(v).kind != pq::graph::NodeKind::Subcircuit || n.size() < 2 || n[0] != 'd') continue;
    if (std::all_of(n.begin() + 1, n.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }))
      out.insert(static_cast<uint32_t>(std::stoul(n.substr(1))));
  }
  return out;
}

size_t atomCount(const pq::graph::Patch& p, std::string_view atom) {
  size_t n = 0;
  const std::string text = [&] {
    LoweredPatch lp;
    lp.patch = p;
    return patchText(lp);
  }();
  for (size_t at = text.find(std::string(atom) + "("); at != std::string::npos; at = text.find(std::string(atom) + "(", at + 1))
    ++n;
  return n;
}

// Every node upstream of node `v` (itself included).
std::set<uint32_t> upstream(const pq::graph::Patch& p, uint32_t v) {
  std::set<uint32_t> seen{v};
  std::vector<uint32_t> work{v};
  while (!work.empty()) {
    const uint32_t x = work.back();
    work.pop_back();
    for (const pq::graph::Wire& w : p.wires())
      if (w.to.node == x && seen.insert(w.from.node).second) work.push_back(w.from.node);
  }
  return seen;
}
uint32_t nodeNamed(const pq::graph::Patch& p, std::string_view name) {
  const auto v = p.findByName(name);
  REQUIRE_MESSAGE(v.has_value(), std::string(name));
  return *v;
}

const WireState* stateOf(const LoweredProject& l, WireId w) {
  for (const WireState& s : l.wires)
    if (s.id == w) return &s;
  return nullptr;
}

}  // namespace

TEST_CASE("frozen: SetTrackFrozen freezes MIDI / audio tracks, a group's tracks inside it; refused on returns and the master") {
  Doc d;
  const TrackId audio = d.must(ops::createTrack(d.doc(), d.store.ids(), TrackKind::Audio, d.bass, "Audio")).tracks.at(0);
  const TrackId ret = d.must(ops::createTrack(d.doc(), d.store.ids(), TrackKind::Return, {}, "Return")).tracks.at(0);
  // A group holding Bass and Audio, and a nested group holding nothing playable.
  d.cmd(GroupTracks{{d.bass, audio}, std::make_shared<const Track>(makeTrack(d.store.ids().track(), TrackKind::Group, "G"))});
  const TrackId g = d.doc().tracks.at(1)->id;
  REQUIRE(d.track(g).kind == TrackKind::Group);

  d.cmd(SetTrackFrozen{d.keys, true});
  CHECK(d.track(d.keys).frozen);
  CHECK(d.store.lastChanges().structural());
  CHECK(d.store.lastChanges().tracks == std::vector<TrackId>{d.keys});
  CHECK(label(Command{SetTrackFrozen{d.keys, true}}) == "Freeze Track");
  // Again: a no-op (no history entry).
  const size_t h = d.store.history().entries().size();
  d.cmd(SetTrackFrozen{d.keys, true});
  CHECK(d.store.history().entries().size() == h);

  // A group: each MIDI / audio track inside it, one undo step; the group itself never.
  d.cmd(SetTrackFrozen{g, true});
  CHECK(d.track(d.bass).frozen);
  CHECK(d.track(audio).frozen);
  CHECK_FALSE(d.track(g).frozen);
  CHECK(d.store.history().entries().size() == h + 1);
  REQUIRE(d.store.undo());
  CHECK_FALSE(d.track(d.bass).frozen);
  CHECK_FALSE(d.track(audio).frozen);
  REQUIRE(d.store.redo());
  d.cmd(SetTrackFrozen{g, false});
  CHECK_FALSE(d.track(d.bass).frozen);
  CHECK_FALSE(d.track(audio).frozen);

  const Snapshot before = d.store.current();
  CHECK_FALSE(d.store.dispatch(SetTrackFrozen{ret, true}).ok());
  CHECK_FALSE(d.store.dispatch(SetTrackFrozen{d.doc().master->id, true}).ok());
  CHECK_FALSE(d.store.dispatch(SetTrackFrozen{TrackId{999999}, true}).ok());
  CHECK(d.store.current() == before);
  // A frozen group or return cannot be added either.
  Track bad = makeTrack(d.store.ids().track(), TrackKind::Group, "Frozen group");
  bad.frozen = true;
  CHECK_FALSE(d.store.dispatch(AddTrack{std::make_shared<const Track>(bad), {}, kAppend}).ok());
}

TEST_CASE("frozen: the file holds \"frozen\": true only when set; save / load round-trips it") {
  Doc d;
  auto plain = serializeProject(d.doc());
  REQUIRE(plain.ok());
  CHECK(plain->find("\"frozen\"") == std::string::npos);
  d.cmd(SetTrackFrozen{d.keys, true});
  auto text = serializeProject(d.doc());
  REQUIRE(text.ok());
  CHECK(text->find("\"frozen\": true") != std::string::npos);
  auto back = parseProject(*text);
  REQUIRE_MESSAGE(back.ok(), (back.ok() ? std::string() : back.error().message));
  CHECK(*back->project == d.doc());
  CHECK(findTrackPtr(*back->project, d.keys)->frozen);
  CHECK_FALSE(findTrackPtr(*back->project, d.bass)->frozen);
  // Only MIDI and audio tracks: a frozen master is refused at load.
  std::string master = *plain;
  const size_t at = master.find("\"kind\": \"master\"");
  REQUIRE(at != std::string::npos);
  master.insert(at, "\"frozen\": true, ");
  CHECK_FALSE(parseProject(master).ok());
}

TEST_CASE("frozen: edits of a frozen track's content are refused by one rule; its mixer, name, colour, tempo and Unfreeze pass") {
  Doc d;
  const TrackId ret = d.must(ops::createTrack(d.doc(), d.store.ids(), TrackKind::Return, {}, "Return")).tracks.at(0);
  const DeviceId inst = d.instrument(d.keys);
  const ClipId clip = d.store.ids().clip();
  {
    NoteClip c;
    c.id = clip;
    c.start = 0;
    c.length = 4 * pq::kPpq;
    d.cmd(AddClip{d.keys, std::make_shared<const NoteClip>(std::move(c)), nullptr, {}});
  }
  // A device lane (a value block's knob inside the instrument) and a mixer lane, both on Keys.
  const Track& kt = d.track(d.keys);
  DeviceId knob;
  for (const DevicePtr& x : kt.devices)
    if (isValueDevice(*x)) knob = x->id;
  REQUIRE(knob.valid());
  const LaneId devLane = d.store.ids().lane(), volLane = d.store.ids().lane();
  {
    AutomationLane l;
    l.id = devLane;
    l.param = ParamRef::device(knob, kValueParam);
    l.paramName = std::string(kValueParam);
    d.cmd(AddAutomationLane{d.keys, std::make_shared<const AutomationLane>(std::move(l))});
    AutomationLane v;
    v.id = volLane;
    v.param = ParamRef::track(ParamKind::TrackVolume, d.keys);
    d.cmd(AddAutomationLane{d.keys, std::make_shared<const AutomationLane>(std::move(v))});
  }
  const DeviceId bassInst = d.instrument(d.bass);
  // (Edits built before the freeze: the builders try their command on the document.)
  auto add = ops::addDevice(d.doc(), d.store.ids(), d.keys, "atom:tanh", DeviceKind::AudioEffect, ops::kOffPath);
  REQUIRE(add.ok());
  Wire own;
  own.from = knob;
  own.mixer = d.keys;
  auto ownWire = ops::connect(d.doc(), d.store.ids(), own);
  REQUIRE(ownWire.ok());
  d.cmd(SetTrackFrozen{d.keys, true});

  auto refused = [&](const Command& c) {
    CAPTURE(std::string(label(c)));
    const Snapshot before = d.store.current();
    const size_t h = d.store.history().entries().size();
    const pq::Status st = d.store.dispatch(c);
    REQUIRE_FALSE(st.ok());
    CHECK(st.error().message == kFrozenRefusal);
    CHECK(d.store.current() == before);
    CHECK(d.store.history().entries().size() == h);
  };
  auto passes = [&](const Command& c) {
    CAPTURE(std::string(label(c)));
    const pq::Status st = d.store.dispatch(c);
    CHECK_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
  };
  // Blocks: add, remove, a knob, a setting, a name, a move to another track.
  refused(add->command);
  refused(RemoveDevice{knob});
  refused(SetDeviceParam{knob, std::string(kValueParam), 0.25});
  refused(RenameDevice{inst, "Mine"});
  refused(SetDeviceActive{inst, false});
  REQUIRE(d.track(d.keys).deviceIndex(knob) > 0);
  refused(MoveDevice{knob, d.keys, 0});  // a reorder of its blocks
  // Clips and notes.
  {
    std::vector<Note> notes(1);
    notes[0].id = d.store.ids().note();
    notes[0].start = 0;
    notes[0].length = pq::kPpq;
    notes[0].pitch = pq::tuning::PitchId::fromDegree(0);
    refused(AddNotes{{d.keys, clip}, notes});
  }
  refused(MoveClip{{d.keys, clip}, d.keys, pq::kPpq});
  refused(RemoveClip{{d.keys, clip}});
  refused(RenameClip{{d.keys, clip}, "Renamed"});
  // A device lane's breakpoints; removing the lane.
  refused(AddBreakpoints{d.keys, devLane, {Breakpoint{0, 0.5f, 0.0f, d.store.ids().breakpoint()}}});
  refused(RemoveAutomationLane{d.keys, devLane});
  // Its own wires: a new cable between two of its blocks; cutting its main path into the mixer.
  refused(ownWire->command);
  refused(RemoveWire{mainPath(d.doc(), d.track(d.keys)).links.back()});
  // A Batch with one forbidden step fails whole.
  refused(Batch{"Mixed", {SetTrackVolume{d.keys, -3.0f}, SetDeviceParam{knob, std::string(kValueParam), 0.25}}});

  // Allowed: its mixer, mute / solo / arm, sends, the mixer lane, the name, the colour, the tempo, its place.
  passes(SetTrackVolume{d.keys, -6.0f});
  passes(SetTrackPan{d.keys, 0.25f});
  passes(SetTrackMute{d.keys, true});
  passes(SetTrackSolo{d.keys, true});
  passes(SetSendLevel{d.keys, ret, -12.0f});
  passes(SetTrackOutput{d.keys, Output{OutputKind::Master, {}}});
  passes(AddBreakpoints{d.keys, volLane, {Breakpoint{0, 0.5f, 0.0f, d.store.ids().breakpoint()}}});
  passes(RenameTrack{d.keys, "Keys (frozen)"});
  passes(SetTrackColour{d.keys, Colour{0x102030}});
  passes(SetTempoMap{{pq::TempoPoint{0, 90.0}}});
  passes(MoveTrack{d.keys, {}, kAppend});
  // Another track's blocks, and a cable from another track into a frozen block (an input from outside).
  passes(SetDeviceActive{bassInst, false});
  {
    Wire w;
    w.from = bassInst;
    w.to = knob;
    w.param = std::string(kValueParam);
    auto e = ops::connect(d.doc(), d.store.ids(), w);
    REQUIRE(e.ok());
    passes(e->command);
  }
  // Unfreeze and edit in one step; then edits pass.
  passes(Batch{"Unfreeze and edit", {SetTrackFrozen{d.keys, false}, SetDeviceParam{knob, std::string(kValueParam), 0.25}}});
  CHECK_FALSE(d.track(d.keys).frozen);
  passes(RemoveClip{{d.keys, clip}});
  // Freeze and edit in one step (the edit came first): allowed too.
  passes(Batch{"Edit and freeze", {SetDeviceParam{knob, std::string(kValueParam), 0.5}, SetTrackFrozen{d.keys, true}}});
  CHECK(d.track(d.keys).frozen);
  // Deleting a frozen track is allowed.
  passes(RemoveTrack{d.keys});
}

TEST_CASE("frozen: a warp-Off audio clip re-synced by a tempo edit is not an edit of the frozen track") {
  Doc d;
  const TrackId audio = d.must(ops::createTrack(d.doc(), d.store.ids(), TrackKind::Audio, d.bass, "Audio")).tracks.at(0);
  SampleRef s;
  s.id = d.store.ids().sample();
  s.path = "C:/samples/a.wav";
  s.frames = 48000;
  s.rate = 48000.0;
  s.channels = 1;
  s.displayName = "a";
  d.cmd(AddSample{s});
  AudioClip c;
  c.id = d.store.ids().clip();
  c.sample = s.id;
  c.start = 0;
  c.seconds = 1.0;  // warp Off: the length follows the tempo map
  d.cmd(AddClip{audio, nullptr, std::make_shared<const AudioClip>(c), {}});
  d.cmd(SetTrackFrozen{audio, true});
  const pq::Ticks before = d.track(audio).clips.at(0).length();
  d.cmd(SetTempoMap{{pq::TempoPoint{0, 60.0}}});
  CHECK(d.track(audio).clips.at(0).length() != before);  // re-synced, and allowed
}

TEST_CASE("frozen: a frozen MIDI track with an Operator lowers to one clip player + its mixer; same IR without a slot") {
  Doc d;
  const DeviceId op = test_blocks::addModule(d.store, d.keys, "operator", 0);
  const Snapshot live = d.store.current();
  const LoweredProject plain = lowered(*live, options());
  REQUIRE_FALSE(calledDevices(patchOf(plain, d.keys)).empty());
  // Unfrozen with frozen slots offered: nothing changes.
  CHECK(texts(lowered(*live, options(true))) == texts(plain));
  CHECK(lowered(*live, options(true)).frozenKeep.empty());

  d.cmd(SetTrackFrozen{d.keys, true});
  const Snapshot frozen = d.store.current();
  // No slot (frozenSlotOf unset, or kNone: the render is not ready): lowered live, the same IR.
  CHECK(texts(lowered(*frozen, options())) == texts(plain));
  LowerOptions none = options();
  none.frozenSlotOf = [](TrackId) { return pq::graph::kNone; };
  CHECK(texts(lowered(*frozen, none)) == texts(plain));
  // captureOwn wins over the slot (the freeze render lowers the track live).
  CHECK(calledDevices(patchOf(lowered(*frozen, options(true, {d.keys})), d.keys)) == calledDevices(patchOf(plain, d.keys)));

  // With its render: one clip player, its mixer, nothing of the circuit (no call, no param, no voices, no event slot).
  const LoweredProject fz = lowered(*frozen, options(true));
  const pq::graph::Patch& kp = patchOf(fz, d.keys);
  CAPTURE(patchText(*fz.patches[indexOf(fz, d.keys)]));
  CHECK(calledDevices(kp).empty());
  CHECK(atomCount(kp, "clipPlayer") == 1);
  CHECK(fz.frozenKeep.empty());
  const pq::graph::ProjectTrack& pt = fz.graph.tracks[indexOf(fz, d.keys)];
  CHECK(pt.role == pq::graph::TrackRole::Audio);
  CHECK(pt.eventSlot == pq::graph::kNone);
  for (const LoweredParam& p : fz.params)
    if (p.track == indexOf(fz, d.keys)) CHECK(p.key.rfind("dev", 0) != 0);  // only its mixer's params
  // Its wires (the main path, the circuit's inner cables) are Inactive.
  for (const Wire& w : wiresOf(*frozen)) {
    if (!ownedByFrozen(*findTrackPtr(*frozen, d.keys), w)) continue;
    const WireState* s = stateOf(fz, w.id);
    REQUIRE(s != nullptr);
    CHECK(s->status == WireStatus::Inactive);
  }
  // Every other track as before; the operator is gone from the lowering.
  CHECK(fz.patches[indexOf(fz, d.bass)] != nullptr);
  CHECK(patchText(*fz.patches[indexOf(fz, d.bass)]) == patchText(*plain.patches[indexOf(plain, d.bass)]));
  (void)op;
}

TEST_CASE("frozen: an LFO wired to another track keeps running, alone; frozenKeep lists it") {
  Doc d;
  const DeviceId lfo = test_blocks::addModule(d.store, d.keys, "lfo", ops::kOffPath);
  const DeviceId util = test_blocks::addModule(d.store, d.bass, "utility");
  const DeviceId gain = test_blocks::member(d.doc(), util, "Gain");
  REQUIRE(gain.valid());
  Wire mod;
  mod.from = lfo;
  mod.to = gain;
  mod.param = std::string(kValueParam);
  const WireId cable = d.wire(mod);
  const DeviceId inst = d.instrument(d.keys);
  d.cmd(SetTrackFrozen{d.keys, true});
  const Snapshot frozen = d.store.current();

  const FrozenKeep k = frozenKeep(*frozen, d.track(d.keys));
  CHECK_FALSE(k.source);  // nothing on another track reads its notes
  const std::vector<DevicePtr>& list = d.track(d.keys).devices;
  REQUIRE_FALSE(k.blocks.empty());
  CHECK(std::find(k.blocks.begin(), k.blocks.end(), lfo) != k.blocks.end());
  for (const DeviceId b : k.blocks) CHECK((b == lfo || groupContains(list, lfo, b)));
  CHECK(std::find(k.blocks.begin(), k.blocks.end(), inst) == k.blocks.end());

  const LoweredProject fz = lowered(*frozen, options(true));
  CHECK(fz.frozenKeep == [&] {
    std::vector<DeviceId> v = k.blocks;
    std::sort(v.begin(), v.end());
    return v;
  }());
  // The instrument and its members are not called; the LFO's blocks are; the cable still plays.
  const std::set<uint32_t> called = calledDevices(patchOf(fz, d.keys));
  CHECK(called.count(inst.value) == 0);
  for (const Device* m : groupMembersDeep(list, inst)) CHECK(called.count(m->id.value) == 0);
  CHECK_FALSE(called.empty());
  for (const uint32_t c : called) CHECK(std::find(k.blocks.begin(), k.blocks.end(), DeviceId{c}) != k.blocks.end());
  REQUIRE(stateOf(fz, cable) != nullptr);
  CHECK(stateOf(fz, cable)->status == WireStatus::Lowered);
  CHECK(atomCount(patchOf(fz, d.keys), "clipPlayer") == 1);
  // Keys plays no notes now: no voices, no event slot.
  CHECK(fz.graph.tracks[indexOf(fz, d.keys)].eventSlot == pq::graph::kNone);
  // The Bass track is unchanged.
  const LoweredProject plain = lowered(*frozen, options());
  CHECK(patchText(*fz.patches[indexOf(fz, d.bass)]) == patchText(*plain.patches[indexOf(plain, d.bass)]));

  // A notes cable from Keys into a group on Bass keeps Keys' notes (its source) lowered.
  Wire notes;
  notes.noteSource = d.keys;
  notes.to = d.instrument(d.bass);
  d.cmd(SetTrackFrozen{d.keys, false});
  d.cmd(RemoveWire{mainPath(d.doc(), d.track(d.bass)).links.front()});  // Bass's own notes no longer play its group
  d.wire(notes);
  d.cmd(SetTrackFrozen{d.keys, true});
  CHECK(frozenKeep(d.doc(), d.track(d.keys)).source);
  const LoweredProject hosted = lowered(d.doc(), options(true));
  CHECK(hosted.graph.tracks[indexOf(hosted, d.keys)].eventSlot != pq::graph::kNone);
}

TEST_CASE("frozen: a cable from another track into the frozen mixer is summed once; captureOwn exports only the own part") {
  Doc d;
  // Bass's instrument also feeds Keys' mixer (a send-like route).
  Wire route;
  route.from = d.instrument(d.bass);
  route.mixer = d.keys;
  const WireId cross = d.wire(route);
  const std::string m = "m" + std::to_string(cross.value);
  const Snapshot live = d.store.current();

  // The capture point (unfrozen): `export own` = the path's end, without the cable; the mixer hears both.
  const LoweredProject cap = lowered(*live, options(false, {d.keys}));
  const pq::graph::Patch& cp = patchOf(cap, d.keys);
  const uint32_t mIn = nodeNamed(cp, m);
  const std::set<uint32_t> own = upstream(cp, nodeNamed(cp, "own"));
  CHECK(own.count(mIn) == 0);
  CHECK_FALSE(calledDevices(cp).empty());
  bool ownHasInstrument = false;
  for (const uint32_t v : own) ownHasInstrument = ownHasInstrument || cp.nodeName(v) == "d" + std::to_string(d.instrument(d.keys).value);
  CHECK(ownHasInstrument);
  CHECK(upstream(cp, nodeNamed(cp, "out")).count(mIn) == 1);
  // Without captureOwn there is no `own` export (the playback IR).
  CHECK_FALSE(patchOf(lowered(*live, options()), d.keys).findByName("own").has_value());

  // Frozen: the mixer = the render's clip player + the cable, once each.
  d.cmd(SetTrackFrozen{d.keys, true});
  const LoweredProject fz = lowered(d.doc(), options(true));
  const pq::graph::Patch& kp = patchOf(fz, d.keys);
  CAPTURE(patchText(*fz.patches[indexOf(fz, d.keys)]));
  CHECK(atomCount(kp, "clipPlayer") == 1);
  const std::set<uint32_t> out = upstream(kp, nodeNamed(kp, "out"));
  CHECK(out.count(nodeNamed(kp, m)) == 1);
  size_t inputs = 0;
  for (uint32_t v = 0; v < kp.nodeCount(); ++v) inputs += kp.nodeName(v) == m ? 1u : 0u;
  CHECK(inputs == 1);
  REQUIRE(stateOf(fz, cross) != nullptr);
  CHECK(stateOf(fz, cross)->status == WireStatus::Lowered);
  // The Bass instrument (a block of another track) runs as before and exports the route.
  CHECK(calledDevices(patchOf(fz, d.bass)).count(d.instrument(d.bass).value) == 1);
}
