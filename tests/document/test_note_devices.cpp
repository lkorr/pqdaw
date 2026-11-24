// AP.2 (docs/plans/AP.md "Where it sits"; pq/document/note_devices.h): note devices — the arpeggiator on a MIDI track's
// notes link. Its declaration (the engine's table), the edits (add / insert / move / remove on the notes link, one undo
// step each, refused on an audio track), chainStages Notes → Notes, the voice contexts through it, and the lowering:
// dissolved out of the graph (the same IR as without it), its params as knob-position slots, its note chain (mapping,
// degree table, null without one, structure vs a knob drag).
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "pq/document/commands.h"
#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/document/instrument_groups.h"
#include "pq/document/lower.h"
#include "pq/document/main_path.h"
#include "pq/document/meta.h"
#include "pq/document/note_devices.h"
#include "pq/document/serialize.h"
#include "pq/document/store.h"
#include "pq/document/wire_types.h"
#include "pq/engine/arp.h"

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

const std::string kArp = noteDevicePatch("arpeggiator");

struct Doc {
  DocumentStore store;
  TrackId keys, audio;
  DeviceId inst;
  Doc() {
    keys = edit(ops::createTrack(doc(), store.ids(), TrackKind::Midi, {}, "Keys")).tracks.at(0);
    audio = edit(ops::createTrack(doc(), store.ids(), TrackKind::Audio, {}, "Audio")).tracks.at(0);
    inst = mainPath(doc(), track(keys)).blocks.at(0)->id;  // the default instrument group
  }
  const Project& doc() const { return *store.current(); }
  const Track& track(TrackId t) const { return *findTrackPtr(doc(), t); }
  std::vector<DeviceId> path(TrackId t) const {
    std::vector<DeviceId> v;
    for (const Device* d : mainPath(doc(), track(t)).blocks) v.push_back(d->id);
    return v;
  }
  // Dispatches as exactly one undo step (undo gives the snapshot before, redo the one after).
  ops::Edit edit(const Result<ops::Edit>& e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    const Snapshot before = store.current();
    const size_t cursor = store.history().cursor();
    REQUIRE(store.dispatch(e->command).ok());
    const Snapshot after = store.current();
    if (after != before) {
      CHECK(store.history().cursor() == cursor + 1);
      REQUIRE(store.undo());
      CHECK(store.current() == before);
      REQUIRE(store.redo());
      CHECK(store.current() == after);
    }
    return *e;
  }
  DeviceId arp(uint32_t slot = kAppend) {
    return edit(ops::addDevice(doc(), store.ids(), keys, kArp, DeviceKind::AudioEffect, slot)).devices.at(0);
  }
};

LoweredProject lowered(const Project& p) {
  LowerOptions o;
  o.library = &library().library();
  o.devices = &library();
  o.slotOf = [](TrackId) { return 0u; };
  auto r = lower(p, o);
  REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
  return std::move(*r);
}

const WireState* stateOf(const LoweredProject& l, WireId w) {
  for (const WireState& s : l.wires)
    if (s.id == w) return &s;
  return nullptr;
}

}  // namespace

TEST_CASE("note devices: the arpeggiator's params come from the engine's one table") {
  const NoteDeviceType* t = findNoteDeviceType("arpeggiator");
  REQUIRE(t != nullptr);
  CHECK(t->name == "Arpeggiator");
  CHECK(t->in == ChainSignal::Notes);
  CHECK(t->out == ChainSignal::Notes);
  REQUIRE(t->params.size() == std::size(pq::engine::kArpParamInfo));
  for (size_t i = 0; i < t->params.size(); ++i) {
    const pq::engine::ArpParamInfo& r = pq::engine::kArpParamInfo[i];
    CHECK(t->params[i].name == r.name);
    CHECK(t->params[i].def == doctest::Approx(r.def));
  }
  CHECK(t->param("style")->choices.size() == static_cast<size_t>(pq::engine::ArpStyle::Count));
  CHECK(t->param("retrigger")->choices == std::vector<std::string>{"Off", "Note", "Beat"});
  // The library describes it from its fixed patch: every runtime param with its range / steps / default, the
  // structural ones as settings.
  Device d;
  d.patch = kArp;
  const DeviceInfo* info = library().infoOf(d);
  REQUIRE(info != nullptr);
  CHECK(info->name == "Arpeggiator");
  size_t runtime = 0;
  for (const NoteParamSpec& s : t->params) {
    if (!s.runtime) {
      CHECK(info->setting(s.name) != nullptr);
      continue;
    }
    ++runtime;
    const DeviceParamInfo* pi = info->param(s.name);
    REQUIRE_MESSAGE(pi != nullptr, s.name);
    CHECK(pi->defaultValue == doctest::Approx(s.def));
    if (!s.isBool) {
      CHECK(pi->min == doctest::Approx(s.min));
      CHECK(pi->max == doctest::Approx(s.max));
      if (s.step > 0.0) CHECK(pi->steps == static_cast<uint32_t>(std::lround((s.max - s.min) / s.step)) + 1);
    }
    // The knob's declared position → value is the lowering's line (offset + scale · position).
    const double pos = deviceParamPosition(d, *pi);
    CHECK(noteParamValue(s, pos) == doctest::Approx(s.def));
  }
  CHECK(info->params.size() == runtime);
  CHECK(library().patchOf(d) != nullptr);
  const MainPorts mp = mainPortsOf(d, &library());
  CHECK(mp.in == "notes");
  CHECK(mp.out == "out");
}

TEST_CASE("note devices: add / insert / move / remove on the notes link, one undo step each") {
  Doc c;
  const DeviceId a1 = c.arp();  // after the tuning block, before the instrument
  CHECK(c.path(c.keys) == std::vector<DeviceId>{a1, c.inst});
  CHECK(mainPath(c.doc(), c.track(c.keys)).complete);
  const DeviceId a2 = c.arp();  // several chain in order: appended at the end of the note run
  CHECK(c.path(c.keys) == std::vector<DeviceId>{a1, a2, c.inst});
  const DeviceId a3 = c.arp(0);  // at the head
  CHECK(c.path(c.keys) == std::vector<DeviceId>{a3, a1, a2, c.inst});
  const DeviceId a4 = c.arp(9);  // a slot past the instrument: clamped to the end of the note run
  CHECK(c.path(c.keys) == std::vector<DeviceId>{a3, a1, a2, a4, c.inst});
  // Move: along the run; past the instrument it stays before it.
  c.edit(ops::moveOnPath(c.doc(), c.store.ids(), &library(), a3, {}, 3));
  CHECK(c.path(c.keys) == std::vector<DeviceId>{a1, a2, a3, a4, c.inst});
  c.edit(ops::moveOnPath(c.doc(), c.store.ids(), &library(), a1, {}, kAppend));
  CHECK(c.path(c.keys) == std::vector<DeviceId>{a2, a3, a4, a1, c.inst});
  // An instrument moved to the head lands after the note run (the notes reach the note devices first).
  c.edit(ops::moveOnPath(c.doc(), c.store.ids(), &library(), c.inst, {}, 0));
  CHECK(c.path(c.keys) == std::vector<DeviceId>{a2, a3, a4, a1, c.inst});
  // Remove (the Device view's Delete: dissolve): the link closes over it.
  const DeviceId gone[] = {a3, a4};
  c.edit(ops::dissolveDevices(c.doc(), c.store.ids(), &library(), gone));
  CHECK(c.path(c.keys) == std::vector<DeviceId>{a2, a1, c.inst});
  CHECK(mainPath(c.doc(), c.track(c.keys)).complete);
  // A new instrument module replaces the one after the note run; the note devices stay and feed it.
  const DeviceId synth = c.edit(ops::addDevice(c.doc(), c.store.ids(), c.keys, std::string(kModuleItemPrefix) + "operator",
                                               DeviceKind::Instrument, 0)).devices.at(0);
  const std::vector<DeviceId> after = c.path(c.keys);
  REQUIRE(after.size() == 3);
  CHECK(after[0] == a2);
  CHECK(after[1] == a1);
  CHECK(after[2] == synth);
  CHECK(findDevice(c.doc(), c.inst).device == nullptr);
}

TEST_CASE("note devices: refused where no notes reach") {
  Doc c;
  // An audio track (the builder and the command both refuse it).
  CHECK_FALSE(ops::addDevice(c.doc(), c.store.ids(), c.audio, kArp, DeviceKind::AudioEffect).ok());
  auto d = std::make_shared<Device>();
  d->id = c.store.ids().device();
  d->patch = kArp;
  CHECK_FALSE(c.store.dispatch(AddDevice{c.audio, d, kAppend}).ok());
  const DeviceId a = c.arp();
  CHECK_FALSE(ops::moveOnPath(c.doc(), c.store.ids(), &library(), a, c.audio, kAppend).ok());
  // No own knob curves (its slots are read on the declared line): SetParamCurve is refused.
  CHECK(c.store.dispatch(SetDeviceParam{a, "gate", 0.25}).ok());
  CHECK_FALSE(ops::setParamCurvePreset(c.doc(), library(), a, "gate", pq::dsp::CurvePreset::Exponential).ok());
}

TEST_CASE("note devices: chainStages passes the notes on (Notes -> Notes)") {
  Doc c;
  const DeviceId a = c.arp();
  const Track& t = c.track(c.keys);
  const MainPath path = mainPath(c.doc(), t);
  const std::vector<ChainStage> st = chainStages(flowOrder(t, path), true, true, &path);
  REQUIRE(st.size() >= 2);
  CHECK(st[0].device->id == a);
  CHECK(st[0].in == ChainSignal::Notes);
  CHECK(st[0].out == ChainSignal::Notes);
  CHECK(st[1].device->id == c.inst);
  CHECK(st[1].in == ChainSignal::Notes);  // the instrument still takes the notes
  // Fed audio (an audio list) it passes on what reaches it.
  const std::vector<ChainStage> au = chainStages(flowOrder(t, path), false);
  CHECK(au[0].in == ChainSignal::Audio);
  CHECK(au[0].out == ChainSignal::Audio);
  // The instrument group after it is still the track's voice context.
  CHECK(voiceContexts(c.doc()).blockOf(c.inst) != nullptr);
  CHECK(chainSignalAt(flowOrder(t, path), true, 1) == ChainSignal::Notes);
}

TEST_CASE("note devices: the lowering dissolves them (the same IR), params as position slots, wire states") {
  Doc c;
  const LoweredProject before = lowered(c.doc());
  const DeviceId a = c.arp();
  const LoweredProject with = lowered(c.doc());
  REQUIRE(before.patches.size() == with.patches.size());
  for (size_t i = 0; i < with.patches.size(); ++i) CHECK(with.patches[i]->patch == before.patches[i]->patch);
  CHECK(with.missingDevices.empty());
  // Its runtime params: knob positions, no track patch links them.
  const std::string key = "dev" + std::to_string(a.value) + ":style@pos";
  const auto lp = std::find_if(with.params.begin(), with.params.end(), [&](const LoweredParam& x) { return x.key == key; });
  REQUIRE(lp != with.params.end());
  CHECK(lp->position);
  CHECK(lp->track == pq::graph::kNone);
  bool seed = false;
  for (const LoweredParam& x : with.params) seed = seed || x.key.find(":seed") != std::string::npos;
  CHECK_FALSE(seed);  // a structural setting is no slot
  const std::vector<LoweredParam> params = lowerParams(c.doc(), true, library());
  CHECK(std::any_of(params.begin(), params.end(), [&](const LoweredParam& x) { return x.key == key && x.position; }));
  // Wire states: the link into it plays as the link it was joined into (Lowered), and so does that one.
  const MainPath path = mainPath(c.doc(), c.track(c.keys));
  REQUIRE(path.links.size() == 3);
  for (const WireId w : path.links) {
    const WireState* s = stateOf(with, w);
    REQUIRE(s != nullptr);
    CHECK(s->status == WireStatus::Lowered);
  }
  CHECK(wireStates(c.doc(), library()) == std::vector<WireState>(with.wires.begin(), with.wires.end()));
  // AP.2b: a cable into one of its runtime params is lowered (Lowered) as its source's export m<wire> (a bus the note
  // stage reads one block late: LoweredProject::noteMods, graph.taps), listed on the chain's slot; one into a
  // structural setting stays Inactive.
  Wire m;
  m.from = c.inst;
  m.to = a;
  m.param = "gate";
  const WireId mw = c.edit(ops::connect(c.doc(), c.store.ids(), m)).wires.at(0);
  const LoweredProject wired = lowered(c.doc());
  REQUIRE(stateOf(wired, mw) != nullptr);
  CHECK(stateOf(wired, mw)->status == WireStatus::Lowered);
  REQUIRE(wired.noteMods.size() == 1);
  CHECK(wired.noteMods[0].wire == mw);
  CHECK(wired.noteMods[0].port == "m" + std::to_string(mw.value));
  CHECK(std::any_of(wired.graph.taps.begin(), wired.graph.taps.end(),
                    [&](const auto& t) { return t.port == wired.noteMods[0].port; }));
  {
    const std::vector<LoweredNoteChain> chains = lowerNoteChains(c.doc());
    REQUIRE(chains.size() == 1);
    const auto& rt = chains[0].stages[0].runtime;
    const auto gate = std::find_if(rt.begin(), rt.end(), [](const LoweredNoteStage::Slot& x) {
      return x.param == static_cast<uint32_t>(pq::engine::ArpParamId::Gate);
    });
    REQUIRE(gate != rt.end());
    CHECK(gate->mods == std::vector<WireId>{mw});
  }
  Wire ms = m;
  ms.param = "seed";
  const WireId sw = c.edit(ops::connect(c.doc(), c.store.ids(), ms)).wires.at(0);
  const LoweredProject seeded = lowered(c.doc());
  REQUIRE(stateOf(seeded, sw) != nullptr);
  CHECK(stateOf(seeded, sw)->status == WireStatus::Inactive);
  // Save / load keeps it.
  Result<std::string> text = serializeProject(c.doc());
  REQUIRE(text.ok());
  Result<LoadedProject> back = parseProject(*text);
  REQUIRE_MESSAGE(back.ok(), (back.ok() ? std::string() : back.error().message));
  const DeviceLoc l = findDevice(*back->project, a);
  REQUIRE(l.device != nullptr);
  CHECK(isNoteDevice(*l.device));
}

TEST_CASE("note devices: the note chain (mapping, degree table, null without, a knob drag keeps the structure)") {
  Doc c;
  CHECK(lowerNoteChains(c.doc()).empty());  // no note device: nothing (zero cost)
  const DeviceId a = c.arp();
  std::vector<LoweredNoteChain> ch = lowerNoteChains(c.doc());
  REQUIRE(ch.size() == 1);
  CHECK(ch[0].track == c.keys);
  REQUIRE(ch[0].stages.size() == 1);
  const LoweredNoteStage& s = ch[0].stages[0];
  CHECK(s.device == a);
  CHECK(s.type == "arpeggiator");
  REQUIRE(s.values.size() == static_cast<size_t>(pq::engine::ArpParamId::Count));
  size_t runtime = 0;
  for (const pq::engine::ArpParamInfo& r : pq::engine::kArpParamInfo) runtime += r.runtime ? 1u : 0u;
  REQUIRE(s.runtime.size() == runtime);
  for (const LoweredNoteStage::Slot& slot : s.runtime) {
    const pq::engine::ArpParamInfo& r = pq::engine::kArpParamInfo[slot.param];
    CHECK(r.runtime);
    CHECK(slot.key == "dev" + std::to_string(a.value) + ":" + std::string(r.name) + "@pos");
    if (!r.isBool) {
      CHECK(slot.offset == doctest::Approx(r.min));
      CHECK(slot.scale == doctest::Approx(r.max - r.min));
    }
  }
  // The default tuning: 12-EDO.
  REQUIRE(ch[0].trackTable.cents.size() == 12);
  for (size_t k = 0; k < 12; ++k) CHECK(ch[0].trackTable.cents[k] == doctest::Approx(100.0 * static_cast<double>(k)).epsilon(1e-9));
  CHECK(ch[0].trackTable.equaveCents == doctest::Approx(1200.0));
  CHECK(ch[0].trackTable.referenceHz > 0.0);  // AP.8a: an absolute degree -> Hz map (re-resolving latched notes)
  // A 19-EDO main tuning on the track: its table.
  auto edo = pq::tuning::Edo::make(19);
  REQUIRE(edo.ok());
  TuningDef def;
  def.system = *edo;
  const TuningId t19 = c.edit(ops::addMainTuning(c.doc(), c.store.ids(), "19", def, Colour{})).tunings.at(0);
  REQUIRE(c.store.dispatch(SetTrackTuning{c.keys, TuningAssignment::mainTuning(t19)}).ok());
  ch = lowerNoteChains(c.doc());
  REQUIRE(ch.size() == 1);
  REQUIRE(ch[0].trackTable.cents.size() == 19);
  CHECK(ch[0].trackTable.cents[1] == doctest::Approx(1200.0 / 19.0));
  // AP.8a (H4): one table per tuning node, in node order (the track-level signal first)
  REQUIRE(ch[0].signals.size() == ch[0].nodes.size());
  REQUIRE(ch[0].signals.size() == 1);
  CHECK(ch[0].signals[0] == t19);
  CHECK(ch[0].nodes[0] == ch[0].trackTable);
  // A knob drag: a param change (no compile), the same structure; a setting: a new structure.
  {
    ChangeSet cs;
    REQUIRE(apply(c.store.current(), SetDeviceParam{a, "gate", 0.9}, cs).ok());
    CHECK_FALSE(cs.structural());
  }
  const std::vector<LoweredNoteChain> base = lowerNoteChains(c.doc());
  c.edit(ops::setDeviceParam(c.doc(), a, "gate", 0.9));
  c.edit(ops::setDeviceParam(c.doc(), a, "style", 1.0));
  std::vector<LoweredNoteChain> dragged = lowerNoteChains(c.doc());
  REQUIRE(dragged.size() == 1);
  CHECK(dragged[0].sameStructure(base[0]));
  CHECK(dragged[0].stages[0].values[static_cast<size_t>(pq::engine::ArpParamId::Style)] ==
        doctest::Approx(static_cast<double>(pq::engine::ArpStyle::Count) - 1.0));
  c.edit(ops::setDeviceSetting(c.doc(), a, "seed", 7.0));
  dragged = lowerNoteChains(c.doc());
  CHECK_FALSE(dragged[0].sameStructure(base[0]));
  CHECK(dragged[0].stages[0].values[static_cast<size_t>(pq::engine::ArpParamId::Seed)] == doctest::Approx(7.0));
  // Two in order; switched off: passes the notes (left out of the chain); all off: none.
  const DeviceId b = c.arp();
  ch = lowerNoteChains(c.doc());
  REQUIRE(ch[0].stages.size() == 2);
  CHECK(ch[0].stages[0].device == a);
  CHECK(ch[0].stages[1].device == b);
  const DeviceId both[] = {a, b};
  c.edit(ops::setDevicesActive(c.doc(), std::span<const DeviceId>(both, 1), false));
  ch = lowerNoteChains(c.doc());
  REQUIRE(ch.size() == 1);
  REQUIRE(ch[0].stages.size() == 1);
  CHECK(ch[0].stages[0].device == b);
  c.edit(ops::setDevicesActive(c.doc(), both, false));
  CHECK(lowerNoteChains(c.doc()).empty());
  // Off the path: plays nothing.
  c.edit(ops::setDevicesActive(c.doc(), both, true));
  c.edit(ops::takeOffPath(c.doc(), c.store.ids(), &library(), both));
  CHECK(lowerNoteChains(c.doc()).empty());
}

// AR.1 (docs/plans/AR.md "AR.1"; wire_types.h notesWireClash): the notes rule by declared signals, on the cable edits.
TEST_CASE("note devices: a notes port joins only a notes port (the cable edits refuse the rest)") {
  Doc d;
  const DeviceId arp = d.edit(ops::addDevice(d.doc(), d.store.ids(), d.keys, kArp, DeviceKind::AudioEffect, ops::kOffPath)).devices.at(0);
  CHECK(mainPath(d.doc(), d.track(d.keys)).indexOf(arp) < 0);  // free, unwired
  CHECK(lowerNoteChains(d.doc()).empty());
  const DeviceId gain = d.edit(ops::addDevice(d.doc(), d.store.ids(), d.keys, "atom:gain", DeviceKind::AudioEffect)).devices.at(0);
  const DeviceId tone = d.edit(ops::addDevice(d.doc(), d.store.ids(), d.audio, "atom:tanh", DeviceKind::AudioEffect)).devices.at(0);
  auto refused = [&](const Result<ops::Edit>& e, WireClash c) {
    REQUIRE_FALSE(e.ok());
    CHECK(e.error().message == std::string(wireClashText(c)));
  };
  // Its input: no audio (a block's output, an audio track's own signal), no note output (Hz, gate).
  refused(ops::connectWire(d.doc(), d.store.ids(), gain, "", arp, ""), WireClash::NotesInput);
  refused(ops::connectWire(d.doc(), d.store.ids(), d.inst, "", arp, "notes"), WireClash::NotesInput);
  refused(ops::connectWire(d.doc(), d.store.ids(), tone, "", arp, ""), WireClash::NotesInput);
  refused(ops::connectNoteWire(d.doc(), d.store.ids(), d.audio, "", arp, ""), WireClash::NotesInput);
  refused(ops::connectNoteWire(d.doc(), d.store.ids(), d.keys, "hz", arp, ""), WireClash::NotesInput);
  // Its output: not into an atom block, a param, a mixer.
  refused(ops::connectWire(d.doc(), d.store.ids(), arp, "", gain, ""), WireClash::NotesOutput);
  refused(ops::connectWire(d.doc(), d.store.ids(), arp, "out", gain, "g"), WireClash::NotesOutput);
  {
    Wire w;
    w.from = arp;
    w.mixer = d.keys;
    refused(ops::connect(d.doc(), d.store.ids(), w), WireClash::NotesOutput);
    CHECK(declaredWireClash(d.doc(), w, nullptr) == WireClash::NotesOutput);  // (library-free)
  }
  // A cable into one of its PARAMS is modulation (AP.2b): as before.
  const NoteDeviceType* type = findNoteDeviceType("arpeggiator");
  REQUIRE(type != nullptr);
  std::string runtime;
  for (const NoteParamSpec& s : type->params)
    if (runtime.empty() && s.runtime && !s.isBool) runtime = s.name;
  REQUIRE_FALSE(runtime.empty());
  (void)d.edit(ops::connectWire(d.doc(), d.store.ids(), gain, "", arp, runtime));
  // Spliced into an audio cable: refused; an atom into a cable between two notes ports too.
  const MainPath path = mainPath(d.doc(), d.track(d.keys));
  REQUIRE(path.blocks.size() == 2);  // inst, gain
  refused(ops::spliceIntoWire(d.doc(), d.store.ids(), nullptr, arp, path.links.at(1)), WireClash::NotesInput);
}

TEST_CASE("note devices: a free one wired at both ends is a stage; half-wired it plays through nothing") {
  Doc d;
  const DeviceId arp = d.edit(ops::addDevice(d.doc(), d.store.ids(), d.keys, kArp, DeviceKind::AudioEffect, ops::kOffPath)).devices.at(0);
  const WireId direct = mainPath(d.doc(), d.track(d.keys)).links.at(0);
  // The notes into it: the instrument keeps its direct cable (and its notes), the arpeggiator is no stage.
  const WireId in = d.edit(ops::connectNoteWire(d.doc(), d.store.ids(), d.keys, "", arp, "")).wires.at(0);
  CHECK(findWire(d.doc(), direct) != nullptr);
  CHECK(d.path(d.keys) == std::vector<DeviceId>{d.inst});
  CHECK(lowerNoteChains(d.doc()).empty());
  {
    const LoweredProject l = lowered(d.doc());
    const WireState* s = stateOf(l, in);
    REQUIRE(s != nullptr);
    CHECK(s->status == WireStatus::Inactive);  // nothing goes on from it
    const WireState* ds = stateOf(l, direct);
    REQUIRE(ds != nullptr);
    CHECK(ds->status != WireStatus::Inactive);
  }
  // Its output into the instrument: the way through it is complete, so the direct cable goes in the same undo step.
  const Snapshot before = d.store.current();
  const WireId out = d.edit(ops::connectWire(d.doc(), d.store.ids(), arp, "", d.inst, "")).wires.at(0);
  CHECK(findWire(d.doc(), direct) == nullptr);
  CHECK(findWire(d.doc(), out) != nullptr);
  CHECK(d.path(d.keys) == std::vector<DeviceId>{arp, d.inst});
  {
    const std::vector<LoweredNoteChain> chains = lowerNoteChains(d.doc());
    REQUIRE(chains.size() == 1);
    CHECK(chains[0].track == d.keys);
    REQUIRE(chains[0].stages.size() == 1);
    CHECK(chains[0].stages[0].device == arp);
  }
  REQUIRE(d.store.undo());
  CHECK(d.store.current() == before);
  // The other order (its output first): the same.
  REQUIRE(d.store.undo());  // the notes cable into it
  CHECK(findWire(d.doc(), in) == nullptr);
  (void)d.edit(ops::connectWire(d.doc(), d.store.ids(), arp, "", d.inst, ""));
  CHECK(findWire(d.doc(), direct) != nullptr);
  CHECK(lowerNoteChains(d.doc()).empty());  // only its output wired: no notes reach it
  (void)d.edit(ops::connectNoteWire(d.doc(), d.store.ids(), d.keys, "", arp, ""));
  CHECK(findWire(d.doc(), direct) == nullptr);
  CHECK(d.path(d.keys) == std::vector<DeviceId>{arp, d.inst});
  CHECK(lowerNoteChains(d.doc()).size() == 1);
  // A second one after it (a chain of two): the cable between them replaces nothing but a parallel.
  const DeviceId arp2 = d.edit(ops::addDevice(d.doc(), d.store.ids(), d.keys, kArp, DeviceKind::AudioEffect, ops::kOffPath)).devices.at(0);
  (void)d.edit(ops::connectWire(d.doc(), d.store.ids(), arp, "", arp2, ""));
  CHECK(d.path(d.keys) == std::vector<DeviceId>{arp, d.inst});
  (void)d.edit(ops::connectWire(d.doc(), d.store.ids(), arp2, "", d.inst, ""));
  CHECK(d.path(d.keys) == std::vector<DeviceId>{arp, arp2, d.inst});
  const std::vector<LoweredNoteChain> chains = lowerNoteChains(d.doc());
  REQUIRE(chains.size() == 1);
  CHECK(chains[0].stages.size() == 2);
}
