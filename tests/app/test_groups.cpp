// TX.groups (SPEC §15 2026-11-01 "resize, groups, and the free-device rack"; model.h "Groups"): Ctrl+G turns a series
// of devices into a group — one audio-effect device of the chain whose members are free devices, its series kept as
// wires from the In terminal to the Out terminal — and Ctrl+Shift+G is its exact inverse. Each case renders the track
// before grouping, grouped and ungrouped through the interpreter (bit for bit equal), the grouped one through the JIT
// (equal to the interpreter), under the allocation trap; ungrouping gives back the same document; one undo step each;
// save / load keeps a group; refusals say why.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "../golden/golden_audio.h"
#include "pq/app/project_link.h"
#include "pq/compile/compile.h"
#include "pq/core/alloc_trap.h"
#include "pq/core/denormal_guard.h"
#include "pq/document/edit_ops.h"
#include "pq/document/instrument_groups.h"
#include "pq/document/lower.h"
#include "pq/document/meta.h"
#include "pq/document/serialize.h"
#include "pq/engine/note_list.h"
#include "pq/engine/project_compile.h"
#include "pq/engine/project_program.h"
#include "pq/meta/registry.h"
#include "playback_fixture.h"
#include "../support/module_batch.h"

using namespace pq;
using namespace pq::doc;

namespace {

// The chain links of group `g` in series order: from In, each next one from where the last one went (TX.all-free: a
// link between two series devices keeps the id of the path link it stands for, so their order in the list is not the
// series' order).
WireList alongSeries(const Project& p, DeviceId g) {
  WireList out;
  DeviceId at = g;
  bool more = true;
  while (more) {
    more = false;
    for (const Wire& x : wiresOf(p))
      if (x.chain && !x.fromNoteSource() && x.from == at && (out.empty() ? x.port == kGroupInPort : true)) {
        out.push_back(x);
        at = x.to;
        more = x.to != g;
        break;
      }
  }
  return out;
}

constexpr uint32_t kBlock = 64;
constexpr double kRate = 48000.0;

const DeviceLibrary& library() {
  static auto lib = app::loadDeviceLibrary();
  REQUIRE_MESSAGE(lib.ok(), (lib.ok() ? std::string() : lib.error().message));
  return **lib;
}

// The lowered project of `p` (one MIDI event slot) rendered for `frames` with three notes.
golden::Channels render(const Project& p, uint32_t frames, engine::UnitBackend backend = {}) {
  LowerOptions lo;
  lo.library = &library().library();
  lo.devices = &library();
  lo.slotOf = [](TrackId) { return 0u; };
  auto low = lower(p, lo);
  REQUIRE_MESSAGE(low.ok(), (low.ok() ? std::string() : low.error().message));
  CHECK(low->missingDevices.empty());
  std::vector<engine::ProjectParamLink> links;
  std::vector<float> values;
  for (const LoweredParam& x : low->params) {
    links.push_back({x.track, x.name, static_cast<uint32_t>(values.size())});
    values.push_back(x.value);
  }
  engine::ProjectCompileOptions co;
  co.sampleRate = kRate;
  co.maxBlockFrames = kBlock;
  co.voice.maxVoices = 8;
  co.paramLinks = links;
  co.backend = backend;
  engine::ProjectCompiler compiler(co);
  auto build = compiler.build(low->graph);
  REQUIRE_MESSAGE(build.ok(), (build.ok() ? std::string() : build.error().message));
  engine::NoteListSource notes(std::vector<engine::TimedNote>{
      {0, 30000, 220.0f, 0.8f, 0.5f}, {3000, 30000, 277.18f, 0.6f, 0.5f}, {9000, 30000, 329.63f, 0.9f, 0.5f}});
  engine::ProjectConfig pc;
  pc.sampleRate = kRate;
  pc.maxBlockFrames = kBlock;
  pc.numOut = 2;
  pc.numProjectParams = static_cast<uint32_t>(values.size());
  pc.eventSources.push_back(notes.source());
  auto prog = engine::ProjectProgram::create(pc, build->layout);
  REQUIRE_MESSAGE(prog.ok(), (prog.ok() ? std::string() : prog.error().message));
  for (uint32_t u = 0; u < build->images.size(); ++u) {
    const auto pub = (*prog)->publish(u, std::move(build->images[u]));
    REQUIRE_MESSAGE(pub.ok(), (pub.ok() ? std::string() : pub.error().message));
  }
  std::copy(values.begin(), values.end(), (*prog)->projectParams().begin());
  golden::Channels out(2);
  std::vector<float> l(kBlock), r(kBlock);
  float* ch[2] = {l.data(), r.data()};
  for (int64_t t = 0; t < frames; t += kBlock) {
    engine::AudioBlock ab{nullptr, 0, ch, 2, kBlock, t};
    CHECK(alloc_trap::countRtViolations([&] {
            DenormalGuard dg;
            (*prog)->process(ab);
          }) == 0);
    out[0].insert(out[0].end(), l.begin(), l.end());
    out[1].insert(out[1].end(), r.begin(), r.end());
  }
  return out;
}

double maxDiff(const golden::Channels& a, const golden::Channels& b) {
  REQUIRE(a.size() == b.size());
  double m = 0.0;
  for (size_t c = 0; c < a.size(); ++c) {
    REQUIRE(a[c].size() == b[c].size());
    for (size_t i = 0; i < a[c].size(); ++i) {
      const double d = std::fabs(static_cast<double>(a[c][i]) - static_cast<double>(b[c][i]));
      m = std::max(m, std::isnan(d) ? 1e9 : d);
    }
  }
  return m;
}

float peakOf(const golden::Channels& a) {
  float p = 0.0f;
  for (const auto& c : a)
    for (float v : c) p = std::max(p, std::fabs(v));
  return p;
}

DevicePtr metaDevice(DeviceId id, std::string_view type, DeviceKind kind) {
  const meta::Type* t = meta::findType(type);
  REQUIRE(t != nullptr);
  auto d = std::make_shared<Device>(*makeDevice(id, std::string(kMetaDevicePrefix) + std::string(type), kind));
  d->meta = std::make_shared<const MetaMap>(MetaMap{MetaEntry{"", defaultMetaInstance(*t)}});
  return d;
}

ops::EditResult group(DocumentStore& s, std::vector<DeviceId> devices) {
  return ops::groupInCircuit(s.current(), s.ids(), library(), devices);
}

// Groups `devices`, checks the render, ungroups and checks the document and the render again. Returns the group's id.
// TX.groups-fix1: `quietIn` — the series starts at the note source with a device whose chain input reads no note signal
// (an Envelope's `in`): its In link carries nothing, as the chain gives that input nothing (Inactive).
DeviceId roundTrip(DocumentStore& store, std::vector<DeviceId> devices, const std::string& what, uint32_t frames = 24000,
                   bool quietIn = false) {
  const Snapshot before = store.current();
  const std::vector<WireState> statesBefore = wireStates(*before, library());
  const golden::Channels a = render(*before, frames);
  auto e = group(store, devices);
  REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
  {
    const Status st = store.dispatch(e->command);
    REQUIRE_MESSAGE(st.ok(), (st.ok() ? std::string() : st.error().message));
  }
  REQUIRE(e->devices.size() == 1);
  const DeviceId g = e->devices.front();
  const Project& grouped = *store.current();
  // Every wire lowers as before (a loop's delay stays where it was); the series' new wires — chain links — plainly.
  for (const WireState& s : wireStates(grouped, library())) {
    const auto old = std::find_if(statesBefore.begin(), statesBefore.end(), [&](const WireState& x) { return x.id == s.id; });
    if (old != statesBefore.end()) {
      CHECK(s.status == old->status);
      continue;
    }
    const Wire* w = nullptr;
    for (const Wire& x : wiresOf(grouped))
      if (x.id == s.id) w = &x;
    REQUIRE(w != nullptr);
    CHECK(w->chain);
    const bool quiet = quietIn && w->from == g && w->port == kGroupInPort;
    CHECK(s.status == (quiet ? WireStatus::Inactive : WireStatus::Lowered));
  }
  // Save / load keeps the group and its chain links (schema 15).
  {
    auto text = serializeProject(grouped, store.view());
    REQUIRE(text.ok());
    auto back = parseProject(*text);
    REQUIRE_MESSAGE(back.ok(), (back.ok() ? std::string() : back.error().message));
    CHECK(*back->project == grouped);
  }
  const golden::Channels b = render(grouped, frames);
  // IG.1-2: a series the notes reach plays the newest note on the path; grouped it is an instrument group and plays
  // per note — a different patch, not the same sound. Every other group is the series' graph: bit for bit.
  // (A stretch that leaves the run's Envelope outside the group is silent there: outside every instrument group an
  // Envelope reads no note's gate.)
  if (isInstrumentGroup(grouped, g)) {
    MESSAGE(what << ": an instrument group (per note), peak " << peakOf(b));
  } else {
    const double d = maxDiff(a, b);
    MESSAGE(what << ": series vs group, max |diff| = " << d);
    CHECK(d == 0.0);
  }
  const golden::Channels j = render(grouped, frames, compile::jitUnitBackend());
  const double dj = maxDiff(b, j);
  MESSAGE(what << ": group, interpreter vs JIT, max |diff| = " << dj);
  CHECK(dj == 0.0);
  // One undo step back to the series, redo to the group.
  REQUIRE(store.undo());
  CHECK(*store.current() == *before);
  REQUIRE(store.redo());
  // Ungroup: the exact inverse.
  auto u = ops::ungroupInCircuit(store.current(), store.ids(), library(), g);
  REQUIRE_MESSAGE(u.ok(), (u.ok() ? std::string() : u.error().message));
  REQUIRE(store.dispatch(u->command).ok());
  CHECK(*store.current() == *before);
  const golden::Channels c = render(*store.current(), frames);
  CHECK(maxDiff(a, c) == 0.0);
  REQUIRE(store.undo());  // back to the group for the caller
  return g;
}

// GU.11: a MIDI track `keys` "Keys" with the default instrument group `inst` (first in its list, so the series starts
// with it), then the modules of `modules` (their group ids chosen by the test) — what the stock effects these cases
// were written with are now (groups of atom and value blocks).
using module_batch::Kind;
using module_batch::Module;
std::vector<Module> keysTrack(Batch& b, IdSource& ids, TrackId keys, DeviceId inst,
                              std::initializer_list<std::pair<Kind, DeviceId>> modules) {
  b.commands.push_back(AddTrack{playback_fixture::track(keys, TrackKind::Midi, "Keys"), {}, kAppend});
  (void)playback_fixture::instrument(b, ids, keys, inst);
  std::vector<Module> out;
  for (const auto& [k, id] : modules) out.push_back(module_batch::push(b, ids, keys, k, id));
  return out;
}
SetDeviceParam setControl(const Module& m, std::string_view control, double value) {
  return SetDeviceParam{m.at(control), std::string(kValueParam), value};
}
// Saturator's Drive in dB as the linear gain its curve takes (what setDeviceParamValue is given).
double driveDb(double db) { return std::pow(10.0, db / 20.0); }
void connect(DocumentStore& store, DeviceId from, std::string port, DeviceId to, std::string param) {
  auto w = ops::connectWire(*store.current(), store.ids(), from, std::move(port), to, std::move(param));
  REQUIRE_MESSAGE(w.ok(), (w.ok() ? std::string() : w.error().message));
  REQUIRE(store.dispatch(w->command).ok());
}
bool isMemberOf(const Project& p, DeviceId d, DeviceId g) {
  const Device* x = findDevice(p, d).device;
  return x != nullptr && x->group == g;
}

}  // namespace

TEST_CASE("groups: a series of effect modules grouped sounds the same, ungrouped gives back the same document") {
  DocumentStore store;
  IdSource& ids = store.ids();
  const TrackId keys = ids.track();
  const DeviceId inst = ids.device(), util = ids.device(), sat = ids.device(), u3 = ids.device();
  Batch b{"keys", {}};
  const auto m = keysTrack(b, ids, keys, inst, {{Kind::Utility, util}, {Kind::Saturator, sat}, {Kind::Utility, u3}});
  b.commands.push_back(setControl(m[0], "Width", 2.5));
  b.commands.push_back(setControl(m[0], "Balance", 0.3));
  b.commands.push_back(setControl(m[1], "Drive", driveDb(12.0)));
  b.commands.push_back(setControl(m[2], "Gain", -9.0));
  REQUIRE(store.dispatch(playback_fixture::inUnits(store, b)).ok());
  playback_fixture::wireSeries(store);  // TX.all-free: the chain as cables
  const DeviceId g = roundTrip(store, {util, sat}, "Utility -> Saturator");
  // The grouped document: the group where Utility was (on the main path), both modules off it and in it, the series as
  // three chain links (TX.all-free: beside the path's own links); the third module stays outside.
  const Project& p = *store.current();
  REQUIRE(findDevice(p, g).device != nullptr);
  CHECK(isGroupDevice(*findDevice(p, g).device));
  CHECK(playback_fixture::onPath(p, g));
  CHECK(!playback_fixture::onPath(p, util));
  CHECK(isMemberOf(p, util, g));
  CHECK(isMemberOf(p, sat, g));
  CHECK(!findDevice(p, u3).device->group.valid());
  CHECK(playback_fixture::onPath(p, u3));
  const WireList w = alongSeries(p, g);
  REQUIRE(w.size() == 3);
  CHECK((w[0].from == g && w[0].port == kGroupInPort && w[0].to == util && w[0].param == "x"));
  CHECK((w[1].from == util && w[1].port == "y" && w[1].to == sat && w[1].param == "x"));
  CHECK((w[2].from == sat && w[2].port == "y" && w[2].to == g && w[2].param == kGroupOutPort));
  // Save / load keeps it (schema 14; TX.groups-fix1: 15, its series wires chain links).
  auto text = serializeProject(*store.current(), store.view());
  REQUIRE(text.ok());
  CHECK(text->find("\"group\": " + std::to_string(g.value)) != std::string::npos);
  auto back = parseProject(*text);
  REQUIRE_MESSAGE(back.ok(), (back.ok() ? std::string() : back.error().message));
  CHECK(*back->project == *store.current());
}

TEST_CASE("groups: the user's example — basic synth → wavetable → wavetable → wavetable becomes synth → group") {
  DocumentStore store;
  IdSource& ids = store.ids();
  const TrackId keys = ids.track();
  const DeviceId inst = ids.device(), pa1 = ids.device(), wt = ids.device(), pa2 = ids.device();
  Batch b{"keys", {}};
  (void)keysTrack(b, ids, keys, inst, {});
  b.commands.push_back(AddDevice{keys, metaDevice(pa1, "wavetable", DeviceKind::AudioEffect), kAppend});
  b.commands.push_back(AddDevice{keys, metaDevice(wt, "wavetable", DeviceKind::AudioEffect), kAppend});
  b.commands.push_back(AddDevice{keys, metaDevice(pa2, "wavetable", DeviceKind::AudioEffect), kAppend});
  b.commands.push_back(SetDeviceParam{pa1, "position", 0.4});
  b.commands.push_back(SetDeviceParam{pa2, "position", 0.7});
  REQUIRE(store.dispatch(playback_fixture::inUnits(store, b)).ok());
  playback_fixture::wireSeries(store);  // TX.all-free: the chain as cables
  (void)roundTrip(store, {pa1, wt, pa2}, "WT -> WT -> WT after Basic Synth");
  // Basic synth → group: the chain is two blocks.
  const Track& t = *findTrackPtr(*store.current(), keys);
  CHECK(mainPath(*store.current(), t).blocks.size() == 2);
}

TEST_CASE("groups: a free modulator moves in, a modulator outside keeps modulating a member across the boundary") {
  DocumentStore store;
  IdSource& ids = store.ids();
  const TrackId keys = ids.track();
  const DeviceId inst = ids.device(), sat = ids.device(), lfoIn = ids.device(), lfoOut = ids.device(), util = ids.device();
  Batch b{"keys", {}};
  const auto m = keysTrack(b, ids, keys, inst,
                           {{Kind::Saturator, sat}, {Kind::Utility, util}, {Kind::Lfo, lfoIn}, {Kind::Lfo, lfoOut}});
  b.commands.push_back(setControl(m[2], "Rate", 7.0));
  b.commands.push_back(setControl(m[3], "Rate", 3.0));
  REQUIRE(store.dispatch(playback_fixture::inUnits(store, b)).ok());
  playback_fixture::wireSeries(store, {lfoIn, lfoOut});  // TX.all-free: the chain as cables
  connect(store, lfoIn, "", m[0].at("Drive"), std::string(kValueParam));
  connect(store, lfoOut, "", m[1].at("Gain"), std::string(kValueParam));
  const DeviceId g = roundTrip(store, {sat, util, lfoIn}, "Saturator -> Utility, a free LFO in, one outside");
  CHECK(isMemberOf(*store.current(), lfoIn, g));
  CHECK(!findDevice(*store.current(), lfoOut).device->group.valid());
}

TEST_CASE("groups: editing inside — In also into a parallel module, a member's output into Out — plays as drawn") {
  DocumentStore store;
  IdSource& ids = store.ids();
  const TrackId keys = ids.track();
  const DeviceId inst = ids.device(), sat = ids.device(), util = ids.device();
  Batch b{"keys", {}};
  const auto m = keysTrack(b, ids, keys, inst, {{Kind::Saturator, sat}});
  b.commands.push_back(setControl(m[0], "Drive", driveDb(18.0)));
  REQUIRE(store.dispatch(playback_fixture::inUnits(store, b)).ok());
  playback_fixture::wireSeries(store);  // TX.all-free: the chain as cables
  auto e = group(store, {sat});
  REQUIRE(e.ok());
  REQUIRE(store.dispatch(e->command).ok());
  const DeviceId g = e->devices.front();
  const golden::Channels one = render(*store.current(), 12000);
  // A Utility added free into the group, fed from In and summed into Out: a parallel path.
  Batch add{"util", {}};
  const Module u = module_batch::push(add, ids, keys, Kind::Utility, util);  // unwired
  add.commands.push_back(SetDeviceGroup{util, g});
  REQUIRE(store.dispatch(add).ok());
  connect(store, g, std::string(kGroupInPort), util, std::string(kMainPort));
  connect(store, util, std::string(kMainPort), g, std::string(kGroupOutPort));
  for (const WireState& s : wireStates(*store.current(), library())) CHECK(s.status == WireStatus::Lowered);
  const golden::Channels two = render(*store.current(), 12000);
  CHECK(maxDiff(one, two) > 1e-3);  // the dry path joined the saturated one
  CHECK(maxDiff(two, render(*store.current(), 12000, compile::jitUnitBackend())) == 0.0);
  // Not one series any more: ungroup says why.
  auto un = ops::ungroupInCircuit(store.current(), store.ids(), library(), g);
  REQUIRE_FALSE(un.ok());
  MESSAGE(un.error().message);
  // Deleting the group deletes its members (at any depth) and their wires (one step).
  auto del = ops::deleteDevices(*store.current(), std::vector<DeviceId>{g});
  REQUIRE(del.ok());
  REQUIRE(store.dispatch(del->command).ok());
  CHECK(findDevice(*store.current(), sat).track == nullptr);
  CHECK(findDevice(*store.current(), util).track == nullptr);
  CHECK(findDevice(*store.current(), u.at("Gain")).track == nullptr);
  for (const Wire& x : wiresOf(*store.current())) CHECK((x.from != g && x.to != g && x.from != sat && x.to != sat && x.from != util && x.to != util));
}

TEST_CASE("groups: refusals say why") {
  DocumentStore store;
  IdSource& ids = store.ids();
  const TrackId keys = ids.track(), bass = ids.track(), lead = ids.track();
  const DeviceId inst = ids.device(), a = ids.device(), c = ids.device(), d = ids.device(), inst2 = ids.device(),
                 x = ids.device(), pa = ids.device(), wt = ids.device();
  Batch b{"tracks", {}};
  (void)keysTrack(b, ids, keys, inst, {{Kind::Utility, a}, {Kind::Saturator, c}, {Kind::Utility, d}});
  b.commands.push_back(AddTrack{playback_fixture::track(bass, TrackKind::Midi, "Bass"), {}, kAppend});
  (void)playback_fixture::instrument(b, ids, bass, inst2);
  (void)module_batch::push(b, ids, bass, Kind::Saturator, x);
  b.commands.push_back(AddTrack{playback_fixture::track(lead, TrackKind::Midi, "Lead"), {}, kAppend});
  b.commands.push_back(AddDevice{lead, metaDevice(pa, "wavetable", DeviceKind::Instrument), kAppend});
  b.commands.push_back(AddDevice{lead, metaDevice(wt, "wavetable", DeviceKind::AudioEffect), kAppend});
  REQUIRE(store.dispatch(playback_fixture::inUnits(store, b)).ok());
  playback_fixture::wireSeries(store);  // TX.all-free: the chain as cables
  auto refused = [&](std::vector<DeviceId> sel, const char* what) {
    auto e = group(store, sel);
    CHECK_FALSE(e.ok());
    if (!e.ok()) MESSAGE(std::string(what) << ": " << e.error().message);
  };
  refused({a, x}, "two tracks");
  refused({a, d}, "a gap in the series");
  // (IG.1-2: the instrument groups — it becomes the track's instrument group: tests/app/test_instrument_groups.cpp)
  refused({inst, a}, "the instrument group and the effect after it (an instrument group stays outermost)");
  auto e = group(store, {c});
  REQUIRE(e.ok());
  REQUIRE(store.dispatch(e->command).ok());
  CHECK_FALSE(playback_fixture::onPath(*store.current(), c));  // a member is never on the main path
  // GU.2 (groups nest; these were refused under TX.groups): a member alone groups inside its capsule, and a group
  // groups into a new one (each on its own, not applied). Still refused: blocks of two capsules (tests/document
  // test_blocks.cpp) and an instrument group (it stays outermost).
  CHECK(group(store, {c}).ok());
  {
    auto g2 = group(store, {e->devices.front()});
    CHECK_MESSAGE(g2.ok(), (g2.ok() ? std::string() : g2.error().message));
  }
  refused({e->devices.front(), x}, "a group and a block of another track");
}

TEST_CASE("groups: the file reader refuses a broken group; duplicating a group copies its contents and wires") {
  DocumentStore store;
  IdSource& ids = store.ids();
  const TrackId keys = ids.track();
  const DeviceId inst = ids.device(), sat = ids.device(), util = ids.device();
  Batch b{"keys", {}};
  const auto m = keysTrack(b, ids, keys, inst, {{Kind::Saturator, sat}, {Kind::Utility, util}});
  REQUIRE(store.dispatch(playback_fixture::inUnits(store, b)).ok());
  playback_fixture::wireSeries(store);  // TX.all-free: the chain as cables
  auto e = group(store, {sat, util});
  REQUIRE(e.ok());
  REQUIRE(store.dispatch(e->command).ok());
  const DeviceId g = e->devices.front();
  auto text = serializeProject(*store.current(), store.view());
  REQUIRE(text.ok());
  // A member naming a block that is not a group (the Utility's Gain value block): refused with the reason.
  std::string bad = *text;
  const std::string key = "\"group\": " + std::to_string(g.value);
  REQUIRE(bad.find(key) != std::string::npos);
  bad.replace(bad.find(key), key.size(), "\"group\": " + std::to_string(m[1].at("Gain").value));
  auto r = parseProject(bad);
  REQUIRE_FALSE(r.ok());
  MESSAGE(r.error().message);
  CHECK(r.error().message.find("group") != std::string::npos);
  // Duplicate: a second group with copies of everything inside and the three series wires, sounding like two in series.
  auto chainLinks = [&] {
    size_t n = 0;
    for (const Wire& x : wiresOf(*store.current())) n += x.chain ? 1 : 0;
    return n;
  };
  const size_t wires = chainLinks();
  const size_t members = groupMembersDeep(findTrackPtr(*store.current(), keys)->devices, g).size();
  const size_t blocks = findTrackPtr(*store.current(), keys)->devices.size();
  auto d = ops::duplicateDevices(*store.current(), ids, std::vector<DeviceId>{g});
  REQUIRE(d.ok());
  REQUIRE(store.dispatch(d->command).ok());
  const Track& t = *findTrackPtr(*store.current(), keys);
  CHECK(t.devices.size() == blocks + members + 1);
  CHECK(chainLinks() == 2 * wires);
  CHECK(mainPath(*store.current(), t).blocks.size() == 3);  // the synth, the group, its copy after it
  for (const WireState& s : wireStates(*store.current(), library())) CHECK(s.status == WireStatus::Lowered);
  CHECK(peakOf(render(*store.current(), 6000)) > 0.01f);
}

// ---- TX.groups-fix1: the cases TX.groups refused (QUESTIONS Q?-TX.groups-fix1-a) ------------------------------------
// (GU.11: the cases of unplugged chain inputs went with Device::chainInput.)

namespace {

// A MIDI track "Lead" without an instrument group whose chain is the given native engines (the first takes the notes).
struct Lead {
  TrackId track;
  std::vector<DeviceId> devs;
};
Lead leadTrack(DocumentStore& store, std::initializer_list<std::string_view> types) {
  IdSource& ids = store.ids();
  Lead l;
  l.track = ids.track();
  Batch b{"lead", {}};
  b.commands.push_back(AddTrack{playback_fixture::track(l.track, TrackKind::Midi, "Lead"), {}, kAppend});
  bool first = true;
  for (const std::string_view t : types) {
    const DeviceId id = ids.device();
    l.devs.push_back(id);
    b.commands.push_back(AddDevice{l.track, metaDevice(id, t, first ? DeviceKind::Instrument : DeviceKind::AudioEffect), kAppend});
    first = false;
  }
  REQUIRE(store.dispatch(playback_fixture::inUnits(store, b)).ok());
  playback_fixture::wireSeries(store);  // TX.all-free: the chain as cables
  return l;
}

// The lowered graph track of `t` (null: not lowered).
const graph::ProjectTrack* loweredTrack(const LoweredProject& low, TrackId t) {
  for (size_t i = 0; i < low.trackIds.size(); ++i)
    if (low.trackIds[i] == t) return &low.graph.tracks[i];
  return nullptr;
}

void setValue(DocumentStore& store, DeviceId block, double value) {
  auto e = ops::setDeviceParamValue(*store.current(), library(), block, std::string(kValueParam), value);
  REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
  REQUIRE(store.dispatch(e->command).ok());
}

}  // namespace

TEST_CASE("groups-fix1: the user's patch — Wavetable → Wavetable → Envelope as the instrument — grouped plays the "
          "chord the same, the group is the instrument") {
  DocumentStore store;
  const Lead l = leadTrack(store, {"wavetable", "wavetable", "envelope"});
  const DeviceId pa = l.devs[0], wt = l.devs[1], env = l.devs[2];
  Batch b{"params", {}};
  b.commands.push_back(SetDeviceParam{pa, "position", 0.5});  // (GU.4: a Wavetable where the Phase accumulator was)
  b.commands.push_back(SetDeviceParam{wt, "position", 0.3});
  b.commands.push_back(SetDeviceParam{env, "attack", 0.01});
  b.commands.push_back(SetDeviceParam{env, "release", 0.05});
  REQUIRE(store.dispatch(playback_fixture::inUnits(store, b)).ok());
  // 48000 frames: the chord's notes end at 30000 … 39000, their releases (the Envelope's `done`) inside the render.
  const DeviceId g = roundTrip(store, {pa, wt, env}, "WT -> WT -> Envelope at the note source", 48000);
  const Project& grouped = *store.current();
  const Track& t = *findTrackPtr(grouped, l.track);
  REQUIRE(isGroupDevice(*t.devices.front()));
  CHECK(t.devices.front()->id == g);
  for (size_t i = 1; i < t.devices.size(); ++i) {
    CHECK(!playback_fixture::onPath(grouped, t.devices[i]->id));
    CHECK(t.devices[i]->group == g);
  }
  // The In terminal carries the notes: its link into the first Wavetable's main input.
  const WireList w = alongSeries(grouped, g);
  REQUIRE(w.size() == 4);
  CHECK((w[0].from == g && w[0].port == kGroupInPort && w[0].to == pa && w[0].param == "x" && w[0].chain));
  CHECK((w[3].from == env && w[3].to == g && w[3].param == kGroupOutPort && w[3].chain));
  // The track still plays voices, the group being its instrument.
  LowerOptions lo;
  lo.library = &library().library();
  lo.devices = &library();
  lo.slotOf = [](TrackId) { return 0u; };
  auto low = lower(grouped, lo);
  REQUIRE(low.ok());
  const graph::ProjectTrack* lt = loweredTrack(*low, l.track);
  REQUIRE(lt != nullptr);
  CHECK(lt->role == graph::TrackRole::Instrument);
  CHECK(lt->eventSlot == 0u);
}

TEST_CASE("groups-fix1: any stretch of a voice run groups — the head, the middle, the end, a run without an Envelope, a "
          "Wavetable first, an Envelope alone, effects after it") {
  SUBCASE("the first Wavetable alone: the run goes on after the group (In = the notes, Out = voices)") {
    DocumentStore store;
    const Lead l = leadTrack(store, {"wavetable", "wavetable", "envelope"});
    REQUIRE(store.dispatch(playback_fixture::inUnits(store, SetDeviceParam{l.devs[2], "release", 0.05})).ok());
    (void)roundTrip(store, {l.devs[0]}, "[WT] -> WT -> Envelope", 48000);
  }
  SUBCASE("Wavetable → Envelope: voices reach the group") {
    DocumentStore store;
    const Lead l = leadTrack(store, {"wavetable", "wavetable", "envelope"});
    REQUIRE(store.dispatch(playback_fixture::inUnits(store, SetDeviceParam{l.devs[2], "release", 0.05})).ok());
    (void)roundTrip(store, {l.devs[1], l.devs[2]}, "WT -> [WT -> Envelope]", 48000);
  }
  SUBCASE("the Wavetable alone in the middle of the run") {
    DocumentStore store;
    const Lead l = leadTrack(store, {"wavetable", "wavetable", "envelope"});
    REQUIRE(store.dispatch(playback_fixture::inUnits(store, SetDeviceParam{l.devs[2], "release", 0.05})).ok());
    (void)roundTrip(store, {l.devs[1]}, "WT -> [WT] -> Envelope", 48000);
  }
  SUBCASE("Wavetable → Wavetable: the voices end 5 ms after note-off, as the run's") {
    DocumentStore store;
    const Lead l = leadTrack(store, {"wavetable", "wavetable"});
    (void)roundTrip(store, {l.devs[0], l.devs[1]}, "[WT -> WT] (no Envelope)", 48000);
  }
  SUBCASE("a Wavetable first: In gives its phase input the note's phase") {
    DocumentStore store;
    const Lead l = leadTrack(store, {"wavetable", "envelope"});
    REQUIRE(store.dispatch(playback_fixture::inUnits(store, SetDeviceParam{l.devs[1], "release", 0.05})).ok());
    (void)roundTrip(store, {l.devs[0], l.devs[1]}, "[WT -> Envelope] at the note source", 48000);
  }
  SUBCASE("an Envelope alone: its `in` reads no note signal (the In link carries nothing), its gate the voice's") {
    DocumentStore store;
    const Lead l = leadTrack(store, {"envelope"});
    REQUIRE(store.dispatch(playback_fixture::inUnits(store, SetDeviceParam{l.devs[0], "release", 0.05})).ok());
    (void)roundTrip(store, {l.devs[0]}, "[Envelope] at the note source", 48000, /*quietIn=*/true);
  }
  SUBCASE("effect modules after the grouped run hear its sum") {
    DocumentStore store;
    const Lead l = leadTrack(store, {"wavetable", "wavetable", "envelope"});
    // TX.all-free: spliced in at the path's end
    const DeviceId satC = test_blocks::appendAfter(store, l.track, "module:saturator");
    const DeviceId utilC = test_blocks::appendAfter(store, l.track, "module:utility");
    setValue(store, test_blocks::member(*store.current(), satC, "Drive"), driveDb(12.0));
    setValue(store, test_blocks::member(*store.current(), utilC, "Width"), 2.0);
    REQUIRE(store.dispatch(playback_fixture::inUnits(store, SetDeviceParam{l.devs[2], "release", 0.05})).ok());
    (void)roundTrip(store, {l.devs[0], l.devs[1], l.devs[2]}, "[WT -> WT -> Envelope] -> Saturator -> Utility", 48000);
  }
}

TEST_CASE("groups-fix1: a series the voices reach is grouped only as a stretch of the run; the rest says why") {
  DocumentStore store;
  const Lead l = leadTrack(store, {"wavetable", "wavetable", "envelope"});
  const DeviceId sat = test_blocks::appendAfter(store, l.track, "module:saturator");  // after the Envelope on the path
  auto refused = [&](DocumentStore& s, std::vector<DeviceId> sel, const char* what) {
    auto e = ops::groupInCircuit(s.current(), s.ids(), library(), sel);
    CHECK_FALSE(e.ok());
    if (!e.ok()) MESSAGE(std::string(what) << ": " << e.error().message);
  };
  refused(store, {l.devs[2], sat}, "the run's sum inside (Envelope -> Saturator)");
  REQUIRE(store.dispatch(SetDeviceActive{l.devs[0], false}).ok());
  refused(store, {l.devs[0], l.devs[1]}, "switched off where the notes arrive");
}

// GU.11 (known gap, reported): the stock Saturator switched off passed its chain input on; a switched-off MODULE (a
// group) in a series refuses Ctrl+G — the new chain links into / out of the bypassed group lower as Inactive
// (lower.cpp: a bypassed device has only its main ports, and Ctrl+G's chain links name the group's "x" / "y"), which
// groupInCircuit reads as a moved loop delay ("a feedback loop runs through these devices"). Kept as the live check it
// must pass once fixed.
TEST_CASE("groups-fix1: a switched-off module inside a series passes its signal on, in a group as in the chain") {
  {
    DocumentStore store;
    IdSource& ids = store.ids();
    const TrackId keys = ids.track();
    const DeviceId inst = ids.device(), u1 = ids.device(), sat = ids.device(), u2 = ids.device();
    Batch b{"keys", {}};
    const auto m = keysTrack(b, ids, keys, inst, {{Kind::Utility, u1}, {Kind::Saturator, sat}, {Kind::Utility, u2}});
    b.commands.push_back(setControl(m[0], "Width", 2.5));
    b.commands.push_back(setControl(m[1], "Drive", driveDb(18.0)));
    b.commands.push_back(setControl(m[2], "Balance", -0.4));
    b.commands.push_back(SetDeviceActive{sat, false});
    REQUIRE(store.dispatch(playback_fixture::inUnits(store, b)).ok());
    playback_fixture::wireSeries(store);  // TX.all-free: the chain as cables
    const DeviceId g = roundTrip(store, {u1, sat, u2}, "Utility -> Saturator (off) -> Utility");
    // Switched on inside the group, then ungrouped: the same sound as the series with it on.
    REQUIRE(store.dispatch(SetDeviceActive{sat, true}).ok());
    const golden::Channels on = render(*store.current(), 24000);
    CHECK(maxDiff(on, render(*store.current(), 24000, compile::jitUnitBackend())) == 0.0);
    auto un = ops::ungroupInCircuit(store.current(), store.ids(), library(), g);
    REQUIRE_MESSAGE(un.ok(), (un.ok() ? std::string() : un.error().message));
    REQUIRE(store.dispatch(un->command).ok());
    CHECK(maxDiff(on, render(*store.current(), 24000)) == 0.0);
  }
}

// GU.11 (the same known gap): the last block of the series a switched-off module.
TEST_CASE("groups-fix1: the last block off — Utility → Saturator (off); the group's Out hears the Utility") {
  {
    DocumentStore store;
    IdSource& ids = store.ids();
    const TrackId keys = ids.track();
    const DeviceId inst = ids.device(), u1 = ids.device(), sat = ids.device();
    Batch b{"keys", {}};
    const auto m = keysTrack(b, ids, keys, inst, {{Kind::Utility, u1}, {Kind::Saturator, sat}});
    b.commands.push_back(setControl(m[0], "Width", 2.5));
    b.commands.push_back(SetDeviceActive{sat, false});
    REQUIRE(store.dispatch(playback_fixture::inUnits(store, b)).ok());
    playback_fixture::wireSeries(store);  // TX.all-free: the chain as cables
    (void)roundTrip(store, {u1, sat}, "Utility -> Saturator (off)");
  }
}

TEST_CASE("groups-fix1: switched-off blocks inside a series pass their signal on, in a group as in the chain") {
  SUBCASE("engines, at the note source: Wavetable → Wavetable (off) → Envelope") {
    DocumentStore store;
    const Lead l = leadTrack(store, {"wavetable", "wavetable", "envelope"});
    REQUIRE(store.dispatch(playback_fixture::inUnits(store, SetDeviceParam{l.devs[0], "position", 0.5})).ok());
    REQUIRE(store.dispatch(playback_fixture::inUnits(store, SetDeviceParam{l.devs[2], "release", 0.05})).ok());
    REQUIRE(store.dispatch(SetDeviceActive{l.devs[1], false}).ok());
    (void)roundTrip(store, {l.devs[0], l.devs[1], l.devs[2]}, "WT -> WT (off) -> Envelope", 48000);
  }
}

TEST_CASE("groups-fix1: a feedback loop through the grouped series keeps its 1-sample delay where it was") {
  SUBCASE("modules: the Saturator's output back into the Utility's Gain before it") {
    DocumentStore store;
    IdSource& ids = store.ids();
    const TrackId keys = ids.track();
    const DeviceId inst = ids.device(), util = ids.device(), sat = ids.device();
    Batch b{"keys", {}};
    const auto m = keysTrack(b, ids, keys, inst, {{Kind::Utility, util}, {Kind::Saturator, sat}});
    b.commands.push_back(setControl(m[1], "Drive", driveDb(9.0)));
    REQUIRE(store.dispatch(playback_fixture::inUnits(store, b)).ok());
    playback_fixture::wireSeries(store);  // TX.all-free: the chain as cables
    auto w = ops::connectWire(*store.current(), ids, sat, "", m[0].at("Gain"), std::string(kValueParam));
    REQUIRE_MESSAGE(w.ok(), (w.ok() ? std::string() : w.error().message));
    REQUIRE(store.dispatch(w->command).ok());
    for (const WireState& s : wireStates(*store.current(), library()))  // the loop's delay: on the feedback wire
      CHECK(s.status == (s.id == w->wires.front() ? WireStatus::Delayed : WireStatus::Lowered));
    (void)roundTrip(store, {util, sat}, "Utility -> Saturator with Saturator -> Utility.Gain");
  }
  SUBCASE("engines, at the note source: the Envelope's output back into the first Wavetable's position") {
    DocumentStore store;
    const Lead l = leadTrack(store, {"wavetable", "wavetable", "envelope"});
    REQUIRE(store.dispatch(playback_fixture::inUnits(store, SetDeviceParam{l.devs[2], "release", 0.05})).ok());
    auto w = ops::connectWire(*store.current(), store.ids(), l.devs[2], "out", l.devs[0], "pos");
    REQUIRE_MESSAGE(w.ok(), (w.ok() ? std::string() : w.error().message));
    REQUIRE(store.dispatch(w->command).ok());
    for (const WireState& s : wireStates(*store.current(), library()))
      CHECK(s.status == (s.id == w->wires.front() ? WireStatus::Delayed : WireStatus::Lowered));
    (void)roundTrip(store, {l.devs[0], l.devs[1], l.devs[2]}, "WT -> WT -> Envelope with Envelope -> WT.pos", 48000);
  }
}

TEST_CASE("groups-fix1: ungrouping keeps a block off the path between the series blocks where it was; a rewired series "
          "is refused") {
  DocumentStore store;
  IdSource& ids = store.ids();
  const TrackId keys = ids.track();
  const DeviceId inst = ids.device(), util = ids.device(), lfo = ids.device(), sat = ids.device();
  Batch b{"keys", {}};
  const auto m = keysTrack(b, ids, keys, inst, {{Kind::Utility, util}, {Kind::Lfo, lfo}, {Kind::Saturator, sat}});
  b.commands.push_back(setControl(m[2], "Drive", driveDb(12.0)));
  REQUIRE(store.dispatch(playback_fixture::inUnits(store, b)).ok());
  playback_fixture::wireSeries(store, {lfo});  // TX.all-free: the LFO off the path
  const DeviceId g = roundTrip(store, {util, sat}, "Utility -> (a free LFO between them in the list) -> Saturator");
  // A chain link deleted and drawn again as a plain wire: the series is no longer Ctrl+G's — Ungroup says why.
  const WireList w = wiresOf(*store.current());
  const auto link = std::find_if(w.begin(), w.end(), [&](const Wire& x) { return x.from == util && x.to == sat; });
  REQUIRE(link != w.end());
  REQUIRE(store.dispatch(RemoveWire{link->id}).ok());
  auto again = ops::connectWire(*store.current(), ids, util, link->port, sat, link->param);
  REQUIRE(again.ok());
  REQUIRE(store.dispatch(again->command).ok());
  auto un = ops::ungroupInCircuit(store.current(), store.ids(), library(), g);
  CHECK_FALSE(un.ok());
  if (!un.ok()) MESSAGE(un.error().message);
}

TEST_CASE("groups-fix1: a group whose series is plain wires (no chain links: schema 14's form) loads, plays as before "
          "and still ungroups") {
  DocumentStore store;
  IdSource& ids = store.ids();
  const TrackId keys = ids.track();
  const DeviceId inst = ids.device(), util = ids.device(), sat = ids.device();
  Batch b{"keys", {}};
  const auto m = keysTrack(b, ids, keys, inst, {{Kind::Utility, util}, {Kind::Saturator, sat}});
  b.commands.push_back(setControl(m[0], "Width", 2.0));
  b.commands.push_back(setControl(m[1], "Drive", driveDb(12.0)));
  REQUIRE(store.dispatch(playback_fixture::inUnits(store, b)).ok());
  playback_fixture::wireSeries(store);  // TX.all-free: the chain as cables
  const golden::Channels series = render(*store.current(), 12000);
  auto e = group(store, {util, sat});
  REQUIRE(e.ok());
  REQUIRE(store.dispatch(e->command).ok());
  auto text = serializeProject(*store.current(), store.view());
  REQUIRE(text.ok());
  // Its chain links written as plain wires.
  std::string old = *text;
  for (size_t at; (at = old.find(", \"chain\": true")) != std::string::npos;) old.erase(at, 15);
  REQUIRE(old.find("\"chain\"") == std::string::npos);
  auto r = parseProject(old);
  REQUIRE_MESSAGE(r.ok(), (r.ok() ? std::string() : r.error().message));
  for (const Wire& w : wiresOf(*r->project)) CHECK_FALSE(w.chain);
  const Snapshot legacy = r->project;
  CHECK(maxDiff(series, render(*legacy, 12000)) == 0.0);
  auto u = ops::ungroupInCircuit(legacy, store.ids(), library(), e->devices.front());
  REQUIRE_MESSAGE(u.ok(), (u.ok() ? std::string() : u.error().message));
  ChangeSet cs;
  auto after = apply(legacy, u->command, cs);
  REQUIRE(after.ok());
  // Only the path's links touch the modules' groups: source → synth → U → S → mixer (their inner wires stay).
  for (const Wire& x : wiresOf(**after))
    if ((x.from == util || x.to == util || x.from == sat || x.to == sat || x.from == inst || x.to == inst) &&
        x.port != kGroupInPort && x.param != kGroupOutPort)  // (not a group's inner wire)
      CHECK(x.mainLink());
  CHECK(maxDiff(series, render(**after, 12000)) == 0.0);
}
