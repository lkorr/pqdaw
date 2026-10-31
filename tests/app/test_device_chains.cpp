// T4.1: device chains through the playback link — existing renders bit-identical after the schema 2 → 3 migration, each
// chain edit one undo step that recompiles only its track's unit (keeping every device's state), param edits that
// compile nothing, the lowered chain rendering like a hand-built ProjectGraph, and a missing device bypassed.
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
#include "pq/graph/parse.h"

using namespace pq;
using namespace pq::doc;
using pq::app::LinkConfig;
using pq::app::ProjectLink;

namespace {

namespace fs = std::filesystem;

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

double maxDiff(const golden::Channels& a, const golden::Channels& b) {
  REQUIRE(a.size() == b.size());
  double m = 0.0;
  for (size_t c = 0; c < a.size(); ++c) {
    REQUIRE(a[c].size() == b[c].size());
    for (size_t i = 0; i < a[c].size(); ++i) m = std::max(m, static_cast<double>(std::fabs(a[c][i] - b[c][i])));
  }
  return m;
}

fs::path appFixture(const char* name) { return fs::path(PQ_TEST_DATA_DIR) / "app" / "fixtures" / name; }

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

// A held chord on one MIDI track (its chain: the default instrument), a return it sends to, the master.
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
    b.commands.push_back(AddTrack{playback_fixture::track(track, TrackKind::Midi, "Keys", inst), {}, kAppend});
    b.commands.push_back(SetSendLevel{track, echo, -6.0f});
    std::vector<Note> notes;
    for (int32_t d : {0, 4, 7}) notes.push_back(playback_fixture::note(ids, 0, 16 * kPpq, d));
    b.commands.push_back(AddClip{track, playback_fixture::clip(ids, 0, 16 * kPpq, notes, "chord")});
    REQUIRE(store.dispatch(b).ok());
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

// The committed schema-2 fixture migrates to chains holding the default instrument; its render must not change by a
// single bit (the golden was rendered before T4.1 and main renders it bit-exactly). (The tuning_automation golden
// predates later tuning fixes: main itself renders it within 6.4e-7, not exactly; its own test keeps the golden
// tolerance, and a pqdaw --render of main vs this branch was compared bit for bit by hand: QUESTIONS / PROGRESS.)
TEST_CASE("device chains: the migrated schema-2 playback fixture renders bit-identically to its golden") {
  for (const auto& [fixture, wav, seconds] : {std::tuple{"playback", "project_playback.wav", 2.5}}) {
    const std::string name = fixture;
    CAPTURE(name);
    auto loaded = loadProject(appFixture(fixture).string());
    REQUIRE_MESSAGE(loaded.ok(), (loaded.ok() ? std::string() : loaded.error().message));
    CHECK(loaded->fileSchemaVersion == 2);
    forEachTrack(*loaded->project, [&](const Track& t, const Track*) {
      if (t.kind == TrackKind::Midi) {
        REQUIRE(t.devices.size() == 1);
        CHECK(t.devices[0]->patch == kDefaultInstrument);
      }
    });
    auto link = makeLink(loaded->project);
    engine::BufferSink sink;
    REQUIRE(link->renderOffline(0, static_cast<uint64_t>(std::llround(seconds * kRate)), sink).ok());
    auto gold = engine::readWav(golden::goldenDir() / "audio" / wav);
    REQUIRE(gold.ok());
    CHECK(maxDiff(gold->channels, sink.channels) == 0.0);
  }
}

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
  const DeviceId util = structuralEdit(ops::addDevice(*k.store.current(), k.store.ids(), k.track, "utility", DeviceKind::AudioEffect), "add utility").devices.at(0);
  const DeviceId sat = structuralEdit(ops::addDevice(*k.store.current(), k.store.ids(), k.track, "saturator", DeviceKind::AudioEffect), "add saturator").devices.at(0);
  const std::set<uint64_t> keysBefore = stateKeys(program, keysUnit);
  REQUIRE(keysBefore.size() > 3);
  structuralEdit(ops::moveDevice(*k.store.current(), sat, {}, 1), "reorder");
  // A reorder keeps every device's state: the unit's state keys are exactly the same.
  CHECK(stateKeys(program, keysUnit) == keysBefore);
  // A param is a param write: no compile at all.
  {
    const app::LinkStats s0 = link->stats();
    k.edit(ops::setDeviceParam(*k.store.current(), util, "width", 0.25));
    link->flush();
    CHECK(link->stats().compiles == s0.compiles);
    CHECK(link->stats().paramWrites == s0.paramWrites + 1);
    CHECK(link->paramSlot("dev" + std::to_string(util.value) + ":width") != ProjectLink::kNoSlot);
  }
  structuralEdit(ops::setDeviceSetting(*k.store.current(), sat, "type", 6.0), "setting");
  structuralEdit(ops::setDevicesActive(*k.store.current(), std::vector<DeviceId>{util}, false), "deactivate");
  structuralEdit(ops::deleteDevices(*k.store.current(), std::vector<DeviceId>{sat}), "remove");
  // Undo of a chain edit is a compile of that unit too.
  const app::LinkStats s0 = link->stats();
  REQUIRE(k.store.undo());
  link->flush();
  CHECK(link->stats().unitCompiles == s0.unitCompiles + 1);
  CHECK(program.stats().topologySwitches == 0);
}

// T4.12-fix1: an instrument's `voices` setting (Operator's) is its track's polyphony. A project with an Operator track
// (8-note chords at beats 0 and 4) and a BasicSynth track (a held 3-note chord): Operator's unit plays 6 voices (its
// default) while the other track keeps the link's 24; setting voices to 12 recompiles Operator's unit only (a new
// unit with a 12-voice VoiceEngine: rebuild, every other unit reused and still sounding), and the next chord plays
// all 8 notes.
TEST_CASE("device chains: an instrument's voices setting is its track's polyphony (a recompile of that unit only)") {
  DocumentStore store;
  IdSource& ids = store.ids();
  const TrackId op = ids.track(), keys = ids.track();
  const DeviceId opDev = ids.device(), keysDev = ids.device();
  {
    Batch b{"voices", {}};
    Track t = makeTrack(op, TrackKind::Midi, "Op");
    t.devices.push_back(makeDevice(opDev, "operator", DeviceKind::Instrument));
    b.commands.push_back(AddTrack{std::make_shared<const Track>(std::move(t)), {}, kAppend});
    b.commands.push_back(AddTrack{playback_fixture::track(keys, TrackKind::Midi, "Keys", keysDev), {}, kAppend});
    std::vector<Note> chords, held;
    for (Ticks at : {Ticks{0}, Ticks{4 * kPpq}})
      for (int32_t d = 0; d < 8; ++d) chords.push_back(playback_fixture::note(ids, at, 2 * kPpq, 3 * d));
    for (int32_t d : {0, 4, 7}) held.push_back(playback_fixture::note(ids, 0, 16 * kPpq, d));
    b.commands.push_back(AddClip{op, playback_fixture::clip(ids, 0, 8 * kPpq, chords, "chords")});
    b.commands.push_back(AddClip{keys, playback_fixture::clip(ids, 0, 16 * kPpq, held, "held")});
    REQUIRE(store.dispatch(b).ok());
  }
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
  CHECK(program.layout().units[opUnit].voice.maxVoices == 6);  // Operator's default `voices`
  CHECK(program.voices(opUnit)->config().maxVoices == 6);
  CHECK(program.voices(opUnit)->liveVoices() == 6);             // 8 notes, 6 voices: two stolen
  CHECK(program.voices(keysUnit)->config().maxVoices == 24);    // no `voices` setting: the link's default
  CHECK(program.voices(keysUnit)->liveVoices() == 3);
  const app::LinkStats s0 = link->stats();
  {
    auto e = ops::setDeviceSetting(*store.current(), opDev, "voices", 12.0);
    REQUIRE(e.ok());
    REQUIRE(store.dispatch(e->command).ok());
  }
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
  // The lowering clamps the setting: 0 → 1 voice, 1000 → kMaxPolyphony.
  for (const auto& [set, want] : {std::pair{0.0, 1u}, std::pair{1000.0, kMaxPolyphony}}) {
    auto e = ops::setDeviceSetting(*store.current(), opDev, "voices", set);
    REQUIRE(e.ok());
    REQUIRE(store.dispatch(e->command).ok());
    link->flush();
    d.run(4);
    CHECK(program.voices(unitNamed("Op"))->config().maxVoices == want);
  }
}

// Renders a project graph through the engine's per-track compile path with fixed notes on slot 0 and the given
// project params linked (as ProjectLink links them).
golden::Channels renderGraph(const graph::ProjectGraph& g, const std::vector<engine::ProjectParamLink>& links,
                             const std::vector<float>& values, uint32_t frames) {
  engine::ProjectCompileOptions co;
  co.sampleRate = kRate;
  co.maxBlockFrames = kBlock;
  co.voice.maxVoices = 8;
  co.paramLinks = links;
  engine::ProjectCompiler compiler(co);
  auto build = compiler.build(g);
  REQUIRE_MESSAGE(build.ok(), (build.ok() ? std::string() : build.error().message));
  engine::NoteListSource notes(std::vector<engine::TimedNote>{
      {0, 20000, 220.0f, 0.8f, 0.5f}, {3000, 15000, 277.18f, 0.6f, 0.5f}, {9000, 20000, 329.63f, 0.9f, 0.5f}});
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

TEST_CASE("device chains: a lowered chain renders like the hand-built ProjectGraph of the same chain") {
  auto lib = app::loadDeviceLibrary();
  REQUIRE_MESSAGE(lib.ok(), (lib.ok() ? std::string() : lib.error().message));
  // The document: Keys = BasicSynth (level 0.3) → Utility (width 50 %) → Saturator (Digital Clip, drive ×2) → mixer at
  // 0 dB, centre; the master at 0 dB.
  DocumentStore store;
  IdSource& ids = store.ids();
  const TrackId keys = ids.track();
  const DeviceId inst = ids.device(), util = ids.device(), sat = ids.device();
  Batch b{"chain", {}};
  b.commands.push_back(AddTrack{playback_fixture::track(keys, TrackKind::Midi, "Keys", inst), {}, kAppend});
  b.commands.push_back(AddDevice{keys, makeDevice(util, "utility", DeviceKind::AudioEffect), kAppend});
  b.commands.push_back(AddDevice{keys, makeDevice(sat, "saturator", DeviceKind::AudioEffect), kAppend});
  b.commands.push_back(SetDeviceParam{inst, "level", 0.3});
  b.commands.push_back(SetDeviceParam{util, "width", 0.5});
  b.commands.push_back(SetDeviceParam{sat, "drive", 2.0});
  b.commands.push_back(SetDeviceSetting{sat, "type", 6.0});
  REQUIRE(store.dispatch(b).ok());
  LowerOptions lo;
  lo.library = &(*lib)->library();
  lo.slotOf = [](TrackId) { return 0u; };
  auto low = lower(*store.current(), lo);
  REQUIRE_MESSAGE(low.ok(), (low.ok() ? std::string() : low.error().message));
  std::vector<engine::ProjectParamLink> links;
  std::vector<float> values;
  for (const LoweredParam& p : low->params) {
    links.push_back({p.track, p.name, static_cast<uint32_t>(values.size())});
    values.push_back(p.value);
  }
  constexpr uint32_t kFrames = 48000;
  const golden::Channels lowered = renderGraph(low->graph, links, values, kFrames);

  // The same signal flow written by hand: the device calls in a row (the chain's params as root params), the
  // constant-power pan at the centre and the master's balance, exactly as lower.h describes them.
  const std::string text = R"(
patch Keys {
  param level = 0.3
  param width = 0.5
  param drive = 2.0
  out out : audio global stereo
  out done : bool voice mono
  s = BasicSynth(level: level)
  x = voice_sum(s.out)
  u = utility(pack(x, x), width: width)
  y = saturator(u, type: 6, drive: drive)
  th = (0 + 1) * 0.7853981633974483
  out = gain(y, pack(sin(th + 1.5707963267948966), sin(th)), 0.002)
  done = s.done
}
patch Main {
  in in : audio global stereo
  out out : audio global stereo
  out = gain(in, pack(min(1, 1 - 0), min(1, 1 + 0)), 0.002)
}
)";
  graph::ParseOptions po;
  po.externals = (*lib)->signatures();
  auto file = graph::parse(text, "hand.pqp", po);
  REQUIRE_MESSAGE(file.ok(), (file.ok() ? std::string() : file.error().message));
  graph::PatchLibrary handLib;
  handLib.add(*file);
  handLib.addAll((*lib)->library());
  graph::ProjectGraph hand;
  hand.library = &handLib;
  hand.tracks.push_back({keys.value, "Keys", file->find("Keys"), graph::TrackRole::Instrument, 0});
  hand.tracks.push_back({store.current()->master->id.value, "Main", file->find("Main"), graph::TrackRole::Master, graph::kNone});
  hand.wires.push_back({{0, "out"}, {1, "in"}, graph::WireKind::Audio});
  hand.output = {1, "out"};
  const golden::Channels manual = renderGraph(hand, {}, {}, kFrames);

  float peak = 0.0f;
  for (const auto& c : manual)
    for (float v : c) peak = std::max(peak, std::fabs(v));
  CHECK(peak > 0.05f);  // the chord sounds (and clips at the saturator's ±1)
  const double diff = maxDiff(lowered, manual);
  MESSAGE("lowered chain vs hand-built graph: max |diff| = " << diff);
  CHECK(diff <= 1e-6);
}

TEST_CASE("device chains: a device missing from the library is bypassed and named in the status line") {
  Keys k;
  auto link = makeLink(k.store.current(), false);
  [[maybe_unused]] const ListenerId sub = k.store.subscribe([&](const StoreEvent& e) { link->onDocumentChange(e); });
  k.edit(ops::addDevice(*k.store.current(), k.store.ids(), k.track, "no_such_device", DeviceKind::AudioEffect));
  link->flush();
  CHECK(link->lastError().empty());  // compiled: the device is bypassed, not an error
  Driver d{*link};
  link->play(0);
  d.run(100);
  float peak = 0.0f;
  for (float v : d.out[0]) peak = std::max(peak, std::fabs(v));
  CHECK(peak > 0.001f);  // the instrument still sounds through the bypassed device
}

// M4 review M4: past 16,384 project params a new param got no slot and played its declared default (a muted Utility
// played unmuted). The engine's param block now grows. 96 tracks of Operator → Utility (mute on), each a held note:
// about 190 params per track, so the last tracks' params lie beyond the old limit; every track is muted, so the
// render must be silent, and each Utility's mute slot holds 1.
TEST_CASE("device chains: a project with more than 16,384 params plays every stored value (M4)" * doctest::test_suite("slow")) {
  DocumentStore store;
  IdSource& ids = store.ids();
  constexpr int kTracks = 96;
  std::vector<DeviceId> utilities;
  {
    Batch b{"many", {}};
    for (int k = 0; k < kTracks; ++k) {
      const TrackId id = ids.track();
      Track t = makeTrack(id, TrackKind::Midi, "T" + std::to_string(k));
      t.devices.push_back(makeDevice(ids.device(), "operator", DeviceKind::Instrument));
      Device u = *makeDevice(ids.device(), "utility", DeviceKind::AudioEffect);
      u.params = {DeviceValue{"mute", 1.0}};
      utilities.push_back(u.id);
      t.devices.push_back(std::make_shared<const Device>(std::move(u)));
      b.commands.push_back(AddTrack{std::make_shared<const Track>(std::move(t)), {}, kAppend});
      b.commands.push_back(AddClip{id, playback_fixture::clip(ids, 0, 16 * kPpq, {playback_fixture::note(ids, 0, 16 * kPpq, k % 12)}, "n")});
    }
    REQUIRE(store.dispatch(b).ok());
  }
  auto link = makeLink(store.current(), false);
  CHECK(link->stats().paramSlots > 16384);
  CHECK(link->stats().paramCapacity >= link->stats().paramSlots);
  const uint32_t last = link->paramSlot("dev" + std::to_string(utilities.back().value) + ":mute");
  REQUIRE(last != ProjectLink::kNoSlot);
  CHECK(last >= 16384);
  Driver d{*link};
  link->play(0);
  d.run(200);
  for (const DeviceId u : utilities) {
    const uint32_t s = link->paramSlot("dev" + std::to_string(u.value) + ":mute");
    REQUIRE(s != ProjectLink::kNoSlot);
    CHECK(link->engine().program()->projectParams()[s] == 1.0f);
  }
  float peak = 0.0f;
  for (const auto& ch : d.out)
    for (float v : ch) peak = std::max(peak, std::fabs(v));
  CHECK(peak == 0.0f);  // every track muted by its Utility
  // An edit of a param beyond the old limit plays: unmute the last track.
  {
    auto e = ops::setDeviceParam(*store.current(), utilities.back(), "mute", 0.0);
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
