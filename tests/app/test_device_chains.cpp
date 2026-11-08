// T4.1: device chains through the playback link — each chain edit one undo step that recompiles only its track's unit
// (keeping every block's state), param edits that compile nothing; an instrument group's polyphony is its track's.
// GU.11 (one hierarchy): the chains hold modules (groups of blocks: Utility, Saturator, Operator) after the default
// instrument group. Gone with the stock devices: the schema 2 → 3 migration render of the playback fixture (no device
// is inserted any more), the hand-built graph of stock patches (tests/app/test_patching.cpp compares second forms of
// block circuits) and the bypassed missing device (a document holds only blocks: checkList refuses any other).
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "../golden/golden_audio.h"
#include "playback_fixture.h"
#include "../support/module_batch.h"
#include "pq/app/project_link.h"
#include "pq/core/alloc_trap.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/rt_scope.h"
#include "pq/document/edit_ops.h"
#include "pq/document/serialize.h"
#include "pq/engine/note_list.h"
#include "pq/engine/project_compile.h"
#include "pq/engine/project_program.h"
#include "pq/engine/wav_file.h"

using namespace pq;
using namespace pq::doc;
using pq::app::LinkConfig;
using pq::app::ProjectLink;

namespace {


constexpr uint32_t kBlock = 64;
constexpr double kRate = 48000.0;

std::unique_ptr<ProjectLink> makeLink(const Snapshot& s, bool jit = true) {
  LinkConfig c;
  c.compileThread = false;
  c.sleepAfterMs = 0.0f;
  c.jit = jit;
  auto l = ProjectLink::create(c, s);
  REQUIRE_MESSAGE(l.ok(), (l.ok() ? std::string() : l.error().message));
  return std::move(l.value());
}

struct Driver {
  ProjectLink& link;
  int64_t time = 0;
  std::vector<float> l = std::vector<float>(kBlock), r = std::vector<float>(kBlock);
  golden::Channels out = golden::Channels(2);
  void run(uint32_t blocks) {
    for (uint32_t b = 0; b < blocks; ++b) {
      float* ch[2] = {l.data(), r.data()};
      {
        RtScope rt;
        DenormalGuard dg;
        link.engine().process(engine::AudioBlock{nullptr, 0, ch, 2, kBlock, time});
      }
      time += kBlock;
      out[0].insert(out[0].end(), l.begin(), l.end());
      out[1].insert(out[1].end(), r.begin(), r.end());
    }
  }
};

// A held chord on one MIDI track (its chain: the default instrument group), a return it sends to, the master.
struct Keys {
  DocumentStore store;
  TrackId track, echo;
  DeviceId inst;
  Keys() {
    IdSource& ids = store.ids();
    track = ids.track();
    echo = ids.track();
    inst = ids.device();
    Batch b{"keys", {}};
    b.commands.push_back(AddTrack{playback_fixture::track(echo, TrackKind::Return, "Echo"), {}, kAppend});
    b.commands.push_back(AddTrack{playback_fixture::track(track, TrackKind::Midi, "Keys"), {}, kAppend});
    (void)playback_fixture::instrument(b, ids, track, inst);
    b.commands.push_back(SetSendLevel{track, echo, -6.0f});
    std::vector<Note> notes;
    for (int32_t d : {0, 4, 7}) notes.push_back(playback_fixture::note(ids, 0, 16 * kPpq, d));
    b.commands.push_back(AddClip{track, playback_fixture::clip(ids, 0, 16 * kPpq, notes, "chord")});
    REQUIRE(store.dispatch(playback_fixture::inUnits(store, b)).ok());
    playback_fixture::wireSeries(store);  // TX.all-free: wired in series
  }
  ops::Edit edit(Result<ops::Edit> e) {
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    const size_t before = store.history().cursor();
    REQUIRE(store.dispatch(e->command).ok());
    CHECK(store.history().cursor() == before + 1);  // one undo step
    return *e;
  }
};

uint32_t voiceUnit(engine::ProjectProgram& program) {
  for (uint32_t u = 0; u < program.numUnits(); ++u)
    if (program.layout().units[u].voices) return u;
  FAIL("no instrument unit");
  return 0;
}

std::set<uint64_t> stateKeys(engine::ProjectProgram& program, uint32_t u) {
  std::set<uint64_t> keys;
  if (const engine::ProgramImage* img = program.current(u))
    for (const graph::StateEntry& e : img->stateLayout().entries) keys.insert(e.key);
  return keys;
}

}  // namespace

TEST_CASE("device chains: add / reorder / remove / deactivate are one undo step and recompile only their track's unit") {
  Keys k;
  auto link = makeLink(k.store.current(), false);
  [[maybe_unused]] const ListenerId sub = k.store.subscribe([&](const StoreEvent& e) { link->onDocumentChange(e); });
  Driver d{*link};
  link->play(0);
  d.run(200);
  engine::ProjectProgram& program = *link->engine().program();
  const uint32_t keysUnit = voiceUnit(program);
  CHECK(program.voices(keysUnit)->liveVoices() == 3);
  auto structuralEdit = [&](Result<ops::Edit> e, const char* what) {
    CAPTURE(what);
    const app::LinkStats s0 = link->stats();
    const ops::Edit done = k.edit(std::move(e));
    link->flush();
    CHECK(link->stats().compiles == s0.compiles + 1);
    CHECK(link->stats().unitCompiles == s0.unitCompiles + 1);  // the Keys unit only
    CHECK(link->stats().topologyChanges == s0.topologyChanges);
    d.run(50);
    CHECK(program.voices(keysUnit)->liveVoices() == 3);  // the chord sounds on through every edit
    return done;
  };
  const DeviceId util = structuralEdit(ops::addDevice(*k.store.current(), k.store.ids(), k.track, "module:utility", DeviceKind::AudioEffect), "add utility").devices.at(0);
  const DeviceId sat = structuralEdit(ops::addDevice(*k.store.current(), k.store.ids(), k.track, "module:saturator", DeviceKind::AudioEffect), "add saturator").devices.at(0);
  const std::set<uint64_t> keysBefore = stateKeys(program, keysUnit);
  REQUIRE(keysBefore.size() > 3);
  structuralEdit(ops::moveOnPath(*k.store.current(), k.store.ids(), nullptr, sat, k.track, 1), "reorder");  // TX.all-free: rewired
  // A reorder keeps every device's state: the unit's state keys are exactly the same.
  CHECK(stateKeys(program, keysUnit) == keysBefore);
  // A control is a param write: no compile at all (the Utility's Width: a value block).
  {
    const DeviceId block = test_blocks::member(*k.store.current(), util, "Width");
    REQUIRE(block.valid());
    const app::LinkStats s0 = link->stats();
    k.edit(ops::setDeviceParamValue(*k.store.current(), playback_fixture::unitsLibrary(), block, std::string(kValueParam), 0.25));
    link->flush();
    CHECK(link->stats().compiles == s0.compiles);
    CHECK(link->stats().paramWrites == s0.paramWrites + 1);
    CHECK(link->paramSlot("dev" + std::to_string(block.value) + ":value") != ProjectLink::kNoSlot);
  }
  // GU.f4 (lower.h "Structural value blocks"): the Saturator's Type menu chooses a curve, so it is a literal of the
  // patch (only the chosen curve is compiled): a change recompiles its unit only, and it has no runtime param.
  {
    const DeviceId block = test_blocks::member(*k.store.current(), sat, "Type");
    REQUIRE(block.valid());
    const ops::Edit e = structuralEdit(ops::setDeviceParamValue(*k.store.current(), playback_fixture::unitsLibrary(), block,
                                                                std::string(kValueParam), 6.0),
                                       "Type menu");
    (void)e;
    CHECK(link->paramSlot("dev" + std::to_string(block.value) + ":value") == ProjectLink::kNoSlot);
  }
  // A structural setting of a block inside a module (the Saturator's drive gain starting from 0): its unit only.
  structuralEdit(ops::setDeviceSetting(*k.store.current(), test_blocks::member(*k.store.current(), sat, "x drive"), "start", 0.0),
                 "setting");
  structuralEdit(ops::setDevicesActive(*k.store.current(), std::vector<DeviceId>{util}, false), "deactivate");
  structuralEdit(ops::deleteDevices(*k.store.current(), std::vector<DeviceId>{sat}), "remove");
  // Undo of a chain edit is a compile of that unit too.
  const app::LinkStats s0 = link->stats();
  REQUIRE(k.store.undo());
  link->flush();
  CHECK(link->stats().unitCompiles == s0.unitCompiles + 1);
  CHECK(program.stats().topologySwitches == 0);
}

// T4.12-fix1 / IG.1-2: an instrument group's polyphony is its track's. A project with an Operator track (8-note chords at
// beats 0 and 4; GU.11: the Operator module, its instrument group set to 6 voices) and a default-instrument track (a
// held 3-note chord): Operator's unit plays 6 voices while the other track keeps its group's 16; setting 12 voices
// recompiles Operator's unit only (a new unit with a 12-voice VoiceEngine: rebuild, every other unit reused and still
// sounding), and the next chord plays all 8 notes.
TEST_CASE("device chains: an instrument group's voices are its track's polyphony (a recompile of that unit only)") {
  DocumentStore store;
  IdSource& ids = store.ids();
  const TrackId op = ids.track(), keys = ids.track();
  const DeviceId opDev = ids.device(), keysDev = ids.device();
  {
    Batch b{"voices", {}};
    b.commands.push_back(AddTrack{playback_fixture::track(op, TrackKind::Midi, "Op"), {}, kAppend});
    (void)module_batch::push(b, op, operatorBlocks(ids, StockModuleOptions{opDev, {}, {}, true}));
    b.commands.push_back(AddTrack{playback_fixture::track(keys, TrackKind::Midi, "Keys"), {}, kAppend});
    (void)playback_fixture::instrument(b, ids, keys, keysDev);
    std::vector<Note> chords, held;
    for (Ticks at : {Ticks{0}, Ticks{4 * kPpq}})
      for (int32_t d = 0; d < 8; ++d) chords.push_back(playback_fixture::note(ids, at, 2 * kPpq, 3 * d));
    for (int32_t d : {0, 4, 7}) held.push_back(playback_fixture::note(ids, 0, 16 * kPpq, d));
    b.commands.push_back(AddClip{op, playback_fixture::clip(ids, 0, 8 * kPpq, chords, "chords")});
    b.commands.push_back(AddClip{keys, playback_fixture::clip(ids, 0, 16 * kPpq, held, "held")});
    REQUIRE(store.dispatch(playback_fixture::inUnits(store, b)).ok());
    playback_fixture::wireSeries(store);  // TX.all-free: wired in series
  }
  auto voices = [&](uint32_t n) {
    GroupPolyphony gp = groupPolyphony(*findDevice(*store.current(), opDev).device);
    gp.voices = n;
    auto e = ops::setGroupPolyphony(*store.current(), opDev, gp);
    REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
    REQUIRE(store.dispatch(e->command).ok());
  };
  voices(6);
  auto link = makeLink(store.current(), false);
  [[maybe_unused]] const ListenerId sub = store.subscribe([&](const StoreEvent& e) { link->onDocumentChange(e); });
  Driver d{*link};
  link->play(0);
  d.run(200);  // beat 0.53: the first chord sounds
  engine::ProjectProgram& program = *link->engine().program();
  auto unitNamed = [&](const char* name) {
    for (uint32_t u = 0; u < program.numUnits(); ++u)
      if (program.layout().units[u].name == name) return u;
    FAIL("no unit " << name);
    return 0u;
  };
  uint32_t opUnit = unitNamed("Op");
  const uint32_t keysUnit = unitNamed("Keys");
  CHECK(program.layout().units[opUnit].voice.maxVoices == 6);
  CHECK(program.voices(opUnit)->config().maxVoices == 6);
  CHECK(program.voices(opUnit)->liveVoices() == 6);                            // 8 notes, 6 voices: two stolen
  CHECK(program.voices(keysUnit)->config().maxVoices == kDefaultGroupVoices);  // the default group's
  CHECK(program.voices(keysUnit)->liveVoices() == 3);
  const app::LinkStats s0 = link->stats();
  voices(12);
  link->flush();
  CHECK(link->stats().compiles == s0.compiles + 1);
  CHECK(link->stats().unitCompiles == s0.unitCompiles + 1);  // Operator's unit only
  CHECK(link->stats().topologyChanges == s0.topologyChanges + 1);
  d.run(4);  // the audio thread switches to the new layout
  opUnit = unitNamed("Op");
  CHECK(program.layout().units[opUnit].voice.maxVoices == 12);
  CHECK(program.voices(opUnit)->config().maxVoices == 12);
  CHECK(program.voices(unitNamed("Keys"))->liveVoices() == 3);  // the other track was not touched
  d.run(4 * 375 - 200);  // past beat 4: the second chord
  CHECK(program.voices(opUnit)->liveVoices() == 8);              // all 8 notes sound with 12 voices
  CHECK(program.voices(unitNamed("Keys"))->liveVoices() == 3);
}

// M4 review M4: past 16,384 project params a new param got no slot and played its declared default (a muted Utility
// played unmuted). The engine's param block now grows. Tracks of Operator → Utility (Gain at −70 dB, which is silence;
// GU.11: the modules, their controls value blocks), each a held note: enough params that the last tracks' lie beyond the
// old limit; every track is silent, so the render must be silent, and each Utility's Gain slot holds its stored −70 dB
// (the default is 0 dB). GU.f4 made the stepped Mute a literal (it chooses a mux branch: no runtime param, no
// slot), so the Gain, a continuous runtime param, carries the check.
TEST_CASE("device chains: a project with more than 16,384 params plays every stored value (M4)" * doctest::test_suite("slow")) {
  DocumentStore store;
  IdSource& ids = store.ids();
  constexpr int kTracks = 192;  // (about 97 params a track)
  std::vector<DeviceId> utilities;  // their Gain value blocks
  {
    Batch b{"many", {}};
    for (int k = 0; k < kTracks; ++k) {
      const TrackId id = ids.track();
      b.commands.push_back(AddTrack{playback_fixture::track(id, TrackKind::Midi, "T" + std::to_string(k)), {}, kAppend});
      (void)module_batch::push(b, id, operatorBlocks(ids));
      const module_batch::Module u = module_batch::push(b, ids, id, module_batch::Kind::Utility, ids.device());
      utilities.push_back(u.at("Gain"));
      b.commands.push_back(SetDeviceParam{u.at("Gain"), std::string(kValueParam), -70.0});  // units (inUnits below)
      b.commands.push_back(AddClip{id, playback_fixture::clip(ids, 0, 16 * kPpq, {playback_fixture::note(ids, 0, 16 * kPpq, k % 12)}, "n")});
    }
    REQUIRE(store.dispatch(playback_fixture::inUnits(store, b)).ok());
    playback_fixture::wireSeries(store);  // TX.all-free: wired in series
  }
  auto link = makeLink(store.current(), false);
  CHECK(link->stats().paramSlots > 16384);
  CHECK(link->stats().paramCapacity >= link->stats().paramSlots);
  const uint32_t last = link->paramSlot("dev" + std::to_string(utilities.back().value) + ":value");
  REQUIRE(last != ProjectLink::kNoSlot);
  CHECK(last >= 16384);
  Driver d{*link};
  link->play(0);
  d.run(200);
  for (const DeviceId u : utilities) {
    const uint32_t s = link->paramSlot("dev" + std::to_string(u.value) + ":value");
    REQUIRE(s != ProjectLink::kNoSlot);
    CHECK(link->engine().program()->projectParams()[s] == -70.0f);  // the stored −70 dB, not the default 0 dB
  }
  float peak = 0.0f;
  for (const auto& ch : d.out)
    for (float v : ch) peak = std::max(peak, std::fabs(v));
  CHECK(peak == 0.0f);  // every track silenced by its Utility
  // An edit of a param beyond the old limit plays: the last track's Gain back to 0 dB.
  {
    auto e = ops::setDeviceParamValue(*store.current(), playback_fixture::unitsLibrary(), utilities.back(), std::string(kValueParam), 0.0);
    REQUIRE(e.ok());
    [[maybe_unused]] const ListenerId sub = store.subscribe([&](const StoreEvent& ev) { link->onDocumentChange(ev); });
    REQUIRE(store.dispatch(e->command).ok());
  }
  d.out = golden::Channels(2);
  d.run(200);
  peak = 0.0f;
  for (const auto& ch : d.out)
    for (float v : ch) peak = std::max(peak, std::fabs(v));
  CHECK(peak > 0.001f);
  CHECK(link->engine().droppedParams() == 0);
}
