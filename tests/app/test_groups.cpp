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
#include "pq/document/lower.h"
#include "pq/document/meta.h"
#include "pq/document/serialize.h"
#include "pq/engine/note_list.h"
#include "pq/engine/project_compile.h"
#include "pq/engine/project_program.h"
#include "pq/meta/registry.h"
#include "playback_fixture.h"

using namespace pq;
using namespace pq::doc;

namespace {

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
  for (uint32_t u = 0; u < build->images.size(); ++u) REQUIRE((*prog)->publish(u, std::move(build->images[u])).ok());
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
DeviceId roundTrip(DocumentStore& store, std::vector<DeviceId> devices, const std::string& what, uint32_t frames = 24000) {
  const Snapshot before = store.current();
  const golden::Channels a = render(*before, frames);
  CHECK(peakOf(a) > 0.01f);
  auto e = group(store, devices);
  REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
  REQUIRE(store.dispatch(e->command).ok());
  REQUIRE(e->devices.size() == 1);
  const DeviceId g = e->devices.front();
  const Project& grouped = *store.current();
  // Every wire lowers as before; the series' new wires plainly.
  for (const WireState& s : wireStates(grouped, library())) CHECK(s.status == WireStatus::Lowered);
  const golden::Channels b = render(grouped, frames);
  const double d = maxDiff(a, b);
  MESSAGE(what << ": series vs group, max |diff| = " << d);
  CHECK(d == 0.0);
  const golden::Channels j = render(grouped, frames, compile::jitUnitBackend());
  const double dj = maxDiff(b, j);
  MESSAGE(what << ": group, interpreter vs JIT, max |diff| = " << dj);
  CHECK(dj == 0.0);
  // One undo step back to the series, redo to the group.
  REQUIRE(store.undo());
  CHECK(*store.current() == *before);
  REQUIRE(store.redo());
  // Ungroup: the exact inverse.
  auto u = ops::ungroupInCircuit(store.current(), library(), g);
  REQUIRE_MESSAGE(u.ok(), (u.ok() ? std::string() : u.error().message));
  REQUIRE(store.dispatch(u->command).ok());
  CHECK(*store.current() == *before);
  const golden::Channels c = render(*store.current(), frames);
  CHECK(maxDiff(a, c) == 0.0);
  REQUIRE(store.undo());  // back to the group for the caller
  return g;
}

}  // namespace

TEST_CASE("groups: a series of stock effects grouped sounds the same, ungrouped gives back the same document") {
  DocumentStore store;
  IdSource& ids = store.ids();
  const TrackId keys = ids.track();
  const DeviceId inst = ids.device(), util = ids.device(), sat = ids.device(), eq = ids.device();
  Batch b{"keys", {}};
  b.commands.push_back(AddTrack{playback_fixture::track(keys, TrackKind::Midi, "Keys", inst), {}, kAppend});
  b.commands.push_back(AddDevice{keys, makeDevice(util, "utility", DeviceKind::AudioEffect), kAppend});
  b.commands.push_back(AddDevice{keys, makeDevice(sat, "saturator", DeviceKind::AudioEffect), kAppend});
  b.commands.push_back(AddDevice{keys, makeDevice(eq, "eq_eight", DeviceKind::AudioEffect), kAppend});
  b.commands.push_back(SetDeviceParam{util, "width", 2.5});
  b.commands.push_back(SetDeviceParam{util, "balance", 0.3});
  b.commands.push_back(SetDeviceParam{sat, "drive", 12.0});
  b.commands.push_back(SetDeviceSetting{eq, "on1", 1.0});
  b.commands.push_back(SetDeviceSetting{eq, "type1", 6.0});
  b.commands.push_back(SetDeviceParam{eq, "freq1", 900.0});
  REQUIRE(store.dispatch(b).ok());
  const DeviceId g = roundTrip(store, {util, sat}, "Utility → Saturator");
  // The grouped document: the group where Utility was, both members free and in it, the series as three wires.
  const Track& t = *findTrackPtr(*store.current(), keys);
  REQUIRE(t.devices.size() == 5);
  CHECK(t.devices[1]->id == g);
  CHECK(isGroupDevice(*t.devices[1]));
  CHECK(!t.devices[1]->free);
  CHECK(t.devices[2]->id == util);
  CHECK(t.devices[2]->free);
  CHECK(t.devices[2]->group == g);
  CHECK(t.devices[3]->group == g);
  CHECK(t.devices[4]->id == eq);
  CHECK(!t.devices[4]->group.valid());
  const WireList& w = wiresOf(*store.current());
  REQUIRE(w.size() == 3);
  CHECK((w[0].from == g && w[0].port == kGroupInPort && w[0].to == util && w[0].param == "x"));
  CHECK((w[1].from == util && w[1].port == "out" && w[1].to == sat && w[1].param == "x"));
  CHECK((w[2].from == sat && w[2].port == "out" && w[2].to == g && w[2].param == kGroupOutPort));
  // Save / load keeps it (schema 14).
  auto text = serializeProject(*store.current(), store.view());
  REQUIRE(text.ok());
  CHECK(text->find("\"group\": " + std::to_string(g.value)) != std::string::npos);
  auto back = parseProject(*text);
  REQUIRE_MESSAGE(back.ok(), (back.ok() ? std::string() : back.error().message));
  CHECK(*back->project == *store.current());
}

TEST_CASE("groups: the user's example — basic synth → phase accumulator → wavetable → phase accumulator becomes synth → group") {
  DocumentStore store;
  IdSource& ids = store.ids();
  const TrackId keys = ids.track();
  const DeviceId inst = ids.device(), pa1 = ids.device(), wt = ids.device(), pa2 = ids.device();
  Batch b{"keys", {}};
  b.commands.push_back(AddTrack{playback_fixture::track(keys, TrackKind::Midi, "Keys", inst), {}, kAppend});
  b.commands.push_back(AddDevice{keys, metaDevice(pa1, "phase_acc", DeviceKind::AudioEffect), kAppend});
  b.commands.push_back(AddDevice{keys, metaDevice(wt, "wavetable", DeviceKind::AudioEffect), kAppend});
  b.commands.push_back(AddDevice{keys, metaDevice(pa2, "phase_acc", DeviceKind::AudioEffect), kAppend});
  b.commands.push_back(SetDeviceParam{pa1, "freq", 110.0});
  b.commands.push_back(SetDeviceParam{pa2, "freq", 3.0});
  REQUIRE(store.dispatch(b).ok());
  (void)roundTrip(store, {pa1, wt, pa2}, "PA → WT → PA after Basic Synth");
  // Basic synth → group: the chain is two devices.
  const Track& t = *findTrackPtr(*store.current(), keys);
  size_t series = 0;
  for (const DevicePtr& d : t.devices) series += d->free ? 0 : 1;
  CHECK(series == 2);
}

TEST_CASE("groups: a free modulator moves in, a modulator outside keeps modulating a member across the boundary") {
  DocumentStore store;
  IdSource& ids = store.ids();
  const TrackId keys = ids.track();
  const DeviceId inst = ids.device(), sat = ids.device(), lfoIn = ids.device(), lfoOut = ids.device(), util = ids.device();
  Batch b{"keys", {}};
  b.commands.push_back(AddTrack{playback_fixture::track(keys, TrackKind::Midi, "Keys", inst), {}, kAppend});
  b.commands.push_back(AddDevice{keys, makeDevice(sat, "saturator", DeviceKind::AudioEffect), kAppend});
  b.commands.push_back(AddDevice{keys, makeDevice(util, "utility", DeviceKind::AudioEffect), kAppend});
  b.commands.push_back(AddDevice{keys, makeDevice(lfoIn, "lfo_device", DeviceKind::AudioEffect), kAppend});
  b.commands.push_back(AddDevice{keys, makeDevice(lfoOut, "lfo_device", DeviceKind::AudioEffect), kAppend});
  b.commands.push_back(SetDeviceFree{lfoIn, true});
  b.commands.push_back(SetDeviceFree{lfoOut, true});
  b.commands.push_back(SetDeviceParam{lfoIn, "rate", 7.0});
  b.commands.push_back(SetDeviceParam{lfoOut, "rate", 3.0});
  REQUIRE(store.dispatch(b).ok());
  auto w1 = ops::connectWire(*store.current(), ids, lfoIn, "lfo", sat, "drive");
  REQUIRE_MESSAGE(w1.ok(), (w1.ok() ? std::string() : w1.error().message));
  REQUIRE(store.dispatch(w1->command).ok());
  auto w2 = ops::connectWire(*store.current(), ids, lfoOut, "lfo", util, "gain");
  REQUIRE_MESSAGE(w2.ok(), (w2.ok() ? std::string() : w2.error().message));
  REQUIRE(store.dispatch(w2->command).ok());
  const DeviceId g = roundTrip(store, {sat, util, lfoIn}, "Saturator → Utility, a free LFO in, one outside");
  const Track& t = *findTrackPtr(*store.current(), keys);
  for (const DevicePtr& d : t.devices) {
    if (d->id == lfoIn) CHECK(d->group == g);
    if (d->id == lfoOut) CHECK(!d->group.valid());
  }
}

TEST_CASE("groups: editing inside — In also into a parallel device, a member's output into Out — plays as drawn") {
  DocumentStore store;
  IdSource& ids = store.ids();
  const TrackId keys = ids.track();
  const DeviceId inst = ids.device(), sat = ids.device(), util = ids.device();
  Batch b{"keys", {}};
  b.commands.push_back(AddTrack{playback_fixture::track(keys, TrackKind::Midi, "Keys", inst), {}, kAppend});
  b.commands.push_back(AddDevice{keys, makeDevice(sat, "saturator", DeviceKind::AudioEffect), kAppend});
  b.commands.push_back(SetDeviceParam{sat, "drive", 18.0});
  REQUIRE(store.dispatch(b).ok());
  auto e = group(store, {sat});
  REQUIRE(e.ok());
  REQUIRE(store.dispatch(e->command).ok());
  const DeviceId g = e->devices.front();
  const golden::Channels one = render(*store.current(), 12000);
  // A Utility added free into the group, fed from In and summed into Out: a parallel path.
  Batch add{"util", {}};
  auto u = std::make_shared<Device>(*makeDevice(util, "utility", DeviceKind::AudioEffect));
  u->free = true;
  add.commands.push_back(AddDevice{keys, u, kAppend});
  add.commands.push_back(SetDeviceGroup{util, g});
  REQUIRE(store.dispatch(add).ok());
  for (auto [from, port, to, param] : {std::tuple{g, std::string(kGroupInPort), util, std::string("x")},
                                       std::tuple{util, std::string("out"), g, std::string(kGroupOutPort)}}) {
    auto w = ops::connectWire(*store.current(), ids, from, port, to, param);
    REQUIRE_MESSAGE(w.ok(), (w.ok() ? std::string() : w.error().message));
    REQUIRE(store.dispatch(w->command).ok());
  }
  for (const WireState& s : wireStates(*store.current(), library())) CHECK(s.status == WireStatus::Lowered);
  const golden::Channels two = render(*store.current(), 12000);
  CHECK(maxDiff(one, two) > 1e-3);  // the dry path joined the saturated one
  CHECK(maxDiff(two, render(*store.current(), 12000, compile::jitUnitBackend())) == 0.0);
  // Not one series any more: ungroup says why.
  auto un = ops::ungroupInCircuit(store.current(), library(), g);
  REQUIRE_FALSE(un.ok());
  MESSAGE(un.error().message);
  // Deleting the group deletes its members and their wires (one step).
  auto del = ops::deleteDevices(*store.current(), std::vector<DeviceId>{g});
  REQUIRE(del.ok());
  REQUIRE(store.dispatch(del->command).ok());
  CHECK(findDevice(*store.current(), sat).track == nullptr);
  CHECK(findDevice(*store.current(), util).track == nullptr);
  CHECK(wiresOf(*store.current()).empty());
}

TEST_CASE("groups: refusals say why") {
  DocumentStore store;
  IdSource& ids = store.ids();
  const TrackId keys = ids.track(), bass = ids.track(), lead = ids.track();
  const DeviceId inst = ids.device(), a = ids.device(), c = ids.device(), d = ids.device(), inst2 = ids.device(),
                 x = ids.device(), pa = ids.device(), wt = ids.device();
  Batch b{"tracks", {}};
  b.commands.push_back(AddTrack{playback_fixture::track(keys, TrackKind::Midi, "Keys", inst), {}, kAppend});
  b.commands.push_back(AddDevice{keys, makeDevice(a, "utility", DeviceKind::AudioEffect), kAppend});
  b.commands.push_back(AddDevice{keys, makeDevice(c, "saturator", DeviceKind::AudioEffect), kAppend});
  b.commands.push_back(AddDevice{keys, makeDevice(d, "utility", DeviceKind::AudioEffect), kAppend});
  b.commands.push_back(AddTrack{playback_fixture::track(bass, TrackKind::Midi, "Bass", inst2), {}, kAppend});
  b.commands.push_back(AddDevice{bass, makeDevice(x, "saturator", DeviceKind::AudioEffect), kAppend});
  b.commands.push_back(AddTrack{playback_fixture::track(lead, TrackKind::Midi, "Lead"), {}, kAppend});
  b.commands.push_back(AddDevice{lead, metaDevice(pa, "phase_acc", DeviceKind::Instrument), kAppend});
  b.commands.push_back(AddDevice{lead, metaDevice(wt, "wavetable", DeviceKind::AudioEffect), kAppend});
  REQUIRE(store.dispatch(b).ok());
  auto refused = [&](std::vector<DeviceId> sel, const char* what) {
    auto e = group(store, sel);
    CHECK_FALSE(e.ok());
    if (!e.ok()) MESSAGE(std::string(what) << ": " << e.error().message);
  };
  refused({a, x}, "two tracks");
  refused({a, d}, "a gap in the series");
  refused({inst}, "the instrument");
  refused({pa}, "the notes reach it");
  refused({wt}, "voices reach it");
  REQUIRE(store.dispatch(SetDeviceActive{c, false}).ok());
  refused({a, c, d}, "switched off");
  REQUIRE(store.dispatch(SetDeviceActive{c, true}).ok());
  auto e = group(store, {c});
  REQUIRE(e.ok());
  REQUIRE(store.dispatch(e->command).ok());
  refused({e->devices.front()}, "a group into a group");
  refused({c}, "already a member");
  // The file reader refuses a member outside its group's track, a group in a rack chain is refused by the commands.
  CHECK_FALSE(store.dispatch(SetDeviceFree{c, false}).ok());  // a member is free
}
