// T4.4-fix1: user devices through the playback link (SPEC §15 2026-10-26) — the first edit of a stock device forks it
// and is audible after ONE unit compile with every state kept (the edited track's voices, phases and envelopes go on:
// after the crossfade the output is exactly the edit applied to the unedited render), the other instance and the stock
// device unchanged, undo restoring the stock reference; a project with user devices saved and loaded renders the same;
// a wire into a param an edit removed is reported and left out until the param is back.
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "../golden/golden_audio.h"
#include "playback_fixture.h"
#include "pq/app/device_player.h"
#include "pq/app/project_link.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/rt_scope.h"
#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/document/serialize.h"
#include "pq/engine/project_program.h"
#include "pq/views/circuit/probes.h"

using namespace pq;
using namespace pq::doc;
using pq::app::LinkConfig;
using pq::app::ProjectLink;

namespace {

namespace fs = std::filesystem;

constexpr uint32_t kBlock = 64;

std::unique_ptr<ProjectLink> makeLink(const Snapshot& s) {
  LinkConfig c;
  c.compileThread = false;
  c.sleepAfterMs = 0.0f;
  c.jit = false;  // interpreter images: the same schedule every run
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

// Two MIDI tracks with BasicSynth holding chords (A: C E G, B: D F A), the master.
struct TwoKeys {
  DocumentStore store;
  TrackId a, b;
  DeviceId synA, synB;
  TwoKeys() {
    IdSource& ids = store.ids();
    a = ids.track();
    b = ids.track();
    synA = ids.device();
    synB = ids.device();
    Batch batch{"keys", {}};
    batch.commands.push_back(AddTrack{playback_fixture::track(a, TrackKind::Midi, "A", synA), {}, kAppend});
    batch.commands.push_back(AddTrack{playback_fixture::track(b, TrackKind::Midi, "B", synB), {}, kAppend});
    for (const auto& [t, root] : {std::pair{a, 0}, std::pair{b, 2}}) {
      std::vector<Note> notes;
      for (int32_t d : {0, 4, 7}) notes.push_back(playback_fixture::note(ids, 0, 32 * kPpq, root + d));
      batch.commands.push_back(AddClip{t, playback_fixture::clip(ids, 0, 32 * kPpq, notes, "chord")});
    }
    REQUIRE(store.dispatch(batch).ok());
  }
};

std::string replaced(std::string s, const std::string& from, const std::string& to) {
  const size_t at = s.find(from);
  REQUIRE_MESSAGE(at != std::string::npos, from);
  s.replace(at, from.size(), to);
  return s;
}

std::set<uint64_t> stateKeys(engine::ProjectProgram& program, uint32_t u) {
  std::set<uint64_t> keys;
  if (const engine::ProgramImage* img = program.current(u))
    for (const graph::StateEntry& e : img->stateLayout().entries) keys.insert(e.key);
  return keys;
}

// The instrument unit of track "A" or "B" (units come in track order: A's first).
uint32_t unitOfTrack(ProjectLink& link, const char* name) {
  engine::ProjectProgram& p = *link.engine().program();
  std::vector<uint32_t> voiced;
  for (uint32_t u = 0; u < p.numUnits(); ++u)
    if (p.layout().units[u].voices) voiced.push_back(u);
  REQUIRE(voiced.size() == 2);
  return std::string(name) == "A" ? voiced[0] : voiced[1];
}

}  // namespace

TEST_CASE("user devices: the first edit forks the device; one unit compile, audible, every state kept") {
  // Reference: the same project, never edited.
  TwoKeys ref;
  auto refLink = makeLink(ref.store.current());
  Driver r{*refLink};
  refLink->play(0);
  r.run(400);

  TwoKeys k;
  auto link = makeLink(k.store.current());
  [[maybe_unused]] const ListenerId sub = k.store.subscribe([&](const StoreEvent& e) { link->onDocumentChange(e); });
  Driver d{*link};
  link->play(0);
  d.run(150);
  engine::ProjectProgram& program = *link->engine().program();
  const uint32_t ua = unitOfTrack(*link, "A"), ub = unitOfTrack(*link, "B");
  CHECK(program.voices(ua)->liveVoices() == 3);
  const std::set<uint64_t> keysA = stateKeys(program, ua);
  const uint64_t swapsB = program.unitStats(ub).swaps;

  // The edit: track A's BasicSynth plays 6 dB louder (× 2 at its output: exact in floating point).
  const std::string stock = link->devices().stockText("BasicSynth");
  const std::string louder = replaced(stock, "out = osc * env * vel * level", "out = osc * env * vel * level * 2");
  const app::LinkStats s0 = link->stats();
  const size_t steps = k.store.history().cursor();
  auto e = ops::editDevicePatch(*k.store.current(), k.synA, louder);
  REQUIRE(e.ok());
  REQUIRE(k.store.dispatch(e->command).ok());
  CHECK(k.store.history().cursor() == steps + 1);  // one undo step
  link->flush();
  CHECK(link->lastError().empty());
  CHECK(link->stats().compiles == s0.compiles + 1);
  CHECK(link->stats().unitCompiles == s0.unitCompiles + 1);  // track A's unit only
  CHECK(link->stats().topologyChanges == s0.topologyChanges);
  // The other instance and the stock device are untouched.
  CHECK_FALSE(findDevice(*k.store.current(), k.synB).device->edited());
  CHECK(link->devices().stockText("BasicSynth") == stock);
  d.run(250);
  CHECK(program.unitStats(ub).swaps == swapsB);  // B never swapped
  CHECK(program.voices(ua)->liveVoices() == 3);  // the chord sounds on through the edit
  CHECK(stateKeys(program, ua) == keysA);        // the same state (the edit added a stateless multiply)
  CHECK(program.unitStats(ua).transferBytes > 0);

  // After the crossfade the edited render is the unedited one with track A doubled: A's state (phases, envelopes,
  // voices) went on as if the edit had always been there (a restarted envelope or phase would differ by far more).
  // A's unedited part = the reference minus a render of B alone (A muted).
  TwoKeys bOnly;
  REQUIRE(bOnly.store.dispatch(SetTrackMute{bOnly.a, true}).ok());
  auto bLink = makeLink(bOnly.store.current());
  Driver bd{*bLink};
  bLink->play(0);
  bd.run(400);
  // Mixer gains are smoothed from their start values: compare only from block 300 on (the fade is ≤ 20 ms ≈ 15 blocks).
  const size_t from = 300 * kBlock, to = 400 * kBlock;
  double maxErr = 0.0, peak = 0.0;
  for (size_t c = 0; c < 2; ++c)
    for (size_t i = from; i < to; ++i) {
      const double aRef = static_cast<double>(r.out[c][i]) - bd.out[c][i];  // A's part, unedited
      const double want = 2.0 * aRef + bd.out[c][i];
      maxErr = std::max(maxErr, std::fabs(d.out[c][i] - want));
      peak = std::max(peak, std::fabs(aRef));
    }
  MESSAGE("A's peak " << peak << ", max |edited - (2 A + B)| = " << maxErr);
  REQUIRE(peak > 1e-3);
  CHECK(maxErr <= 1e-6 * std::max(1.0, peak) + 1e-6);

  // Undo: one step back to the stock reference; one unit compile; A plays as before.
  const app::LinkStats s1 = link->stats();
  REQUIRE(k.store.undo());
  CHECK_FALSE(findDevice(*k.store.current(), k.synA).device->edited());
  link->flush();
  CHECK(link->stats().unitCompiles == s1.unitCompiles + 1);
  d.run(100);
  CHECK(program.voices(ua)->liveVoices() == 3);
  CHECK(stateKeys(program, ua) == keysA);
}

TEST_CASE("user devices: a project with an edited device saves, loads and renders the same") {
  TwoKeys k;
  const std::string stock = app::loadDeviceLibrary().value()->stockText("BasicSynth");
  const std::string brighter = replaced(stock, "param bright  = 0.35  [0..1]", "param bright  = 0.9   [0..1]");
  REQUIRE(k.store.dispatch(ops::editDevicePatch(*k.store.current(), k.synB, brighter)->command).ok());
  const fs::path dir =
      fs::temp_directory_path() /
      ("pq_user_devices_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::error_code ec;
  fs::remove_all(dir, ec);
  REQUIRE(saveProject(dir.string(), k.store).ok());
  auto loaded = loadProject(dir.string());
  REQUIRE_MESSAGE(loaded.ok(), (loaded.ok() ? std::string() : loaded.error().message));
  CHECK(*loaded->project == *k.store.current());
  REQUIRE(findDevice(*loaded->project, k.synB).device->edited());
  CHECK(findDevice(*loaded->project, k.synB).device->source->text == brighter);

  auto render = [](const Snapshot& s) {
    auto link = makeLink(s);
    CHECK(link->lastError().empty());
    engine::BufferSink sink;
    REQUIRE(link->renderOffline(0, 48000, sink).ok());
    return sink.channels;
  };
  const golden::Channels before = render(k.store.current());
  const golden::Channels after = render(loaded->project);
  REQUIRE(before.size() == after.size());
  CHECK(before[0] == after[0]);
  CHECK(before[1] == after[1]);
  // And the edit is audible: the stock project renders differently.
  TwoKeys plain;
  const golden::Channels stockRender = render(plain.store.current());
  CHECK(stockRender[0] != before[0]);
  fs::remove_all(dir, ec);
}

// The circuit editor's probes and cost on a device read the project unit that plays it (not a separate preview): a
// probe compiles into that unit only (the audio stays bit-identical), its level and signal come from the project's own
// run, the cost meter times that unit, and the editor's attribution finds the device's instance in it.
TEST_CASE("user devices: probes and cost through the project unit (interpreter and JIT)") {
  for (const bool jit : {false, true}) {
    CAPTURE(jit);
    TwoKeys ref;
    auto refLink = makeLink(ref.store.current());
    Driver r{*refLink};
    refLink->play(0);
    r.run(300);

    TwoKeys k;
    LinkConfig c;
    c.compileThread = false;
    c.sleepAfterMs = 0.0f;
    c.jit = jit;
    auto made = ProjectLink::create(c, k.store.current());
    REQUIRE(made.ok());
    ProjectLink& link = **made;
    Driver d{link};
    link.play(0);
    d.run(50);
    if (jit) REQUIRE(link.waitForJit(60.0));  // every unit on its JIT image first (T1b.5's own swap)
    d.run(50);
    engine::ProjectProgram& program = *link.engine().program();
    const uint32_t ub = unitOfTrack(link, "B");
    const uint64_t swapsB = program.unitStats(ub).swaps;
    // A probe on BasicSynth's envelope (`env`), as the editor names it: a path from the device's patch (its root: empty).
    const graph::Patch* bs = link.devices().stockFile("BasicSynth")->find("BasicSynth");
    REQUIRE(bs != nullptr);
    const auto env = bs->findByName("env");
    REQUIRE(env.has_value());
    graph::ProbeTarget t;
    t.node = bs->node(*env).id;
    const graph::ProbeTarget targets[1] = {t};
    const uint32_t ids[1] = {41};
    auto player = std::make_unique<app::DevicePlayer>(link, k.synA);
    CHECK(player->external());
    player->setProbeSignals(ids);
    player->setProbeTargets(targets, ids);
    const app::LinkStats s0 = link.stats();
    link.flush();
    CHECK(link.lastError().empty());
    CHECK(link.stats().unitCompiles == s0.unitCompiles + 1);  // A's unit only
    if (jit) REQUIRE(link.waitForJit(60.0));
    d.run(200);
    CHECK(program.unitStats(ub).swaps == swapsB);
    // The level and the signal of the envelope while the chord is held.
    const ProbeLevel* level = player->probeLevel(41);
    REQUIRE(level != nullptr);
    CHECK(level->hi.load() > 0.1f);
    TapRing* ring = player->probeSignal(41);
    REQUIRE(ring != nullptr);
    CHECK(ring->written() > 0);
    // Bit-identical audio with the probe compiled in.
    double maxDiff = 0.0;
    for (size_t ch = 0; ch < 2; ++ch)
      for (size_t i = 0; i < d.out[ch].size(); ++i) maxDiff = std::max(maxDiff, static_cast<double>(std::fabs(d.out[ch][i] - r.out[ch][i])));
    CHECK(maxDiff == 0.0);
    // Cost: the unit's regions, attributed to the device's instance (the path the editor puts in front).
    const views::circuit::PlayedProgram* played = player->playedProgram();
    REQUIRE(played != nullptr);
    CHECK(played->probeIds == std::vector<uint32_t>{41});
    player->setMetering(true);
    views::circuit::PatchPlayer::CostReading rd;
    (void)player->readCost(rd);  // the baseline
    d.run(100);
    REQUIRE(player->readCost(rd));
    CHECK(rd.generation == played->generation);
    CHECK(rd.totalNsPerFrame > 0.0);
    views::circuit::CostMap cm;
    const std::vector<graph::NodeId> path = player->pathPrefix();
    REQUIRE(path.size() == 2);
    CHECK(path[0] == graph::NodeId{k.a.value});
    REQUIRE(views::circuit::attributeCost(*played, rd, path, cm));
    CHECK_FALSE(cm.nodes.empty());
    CHECK(cm.shownNs > 0.0);
    // The editor closes: the unit is compiled without probes again; nothing stays bound.
    const app::LinkStats s1 = link.stats();
    player.reset();
    link.flush();
    CHECK(link.stats().unitCompiles == s1.unitCompiles + 1);
    if (jit) REQUIRE(link.waitForJit(60.0));
    d.run(20);
  }
}

TEST_CASE("user devices: a wire into a param an edit removed is reported and left out until the param is back") {
  TwoKeys k;
  auto link = makeLink(k.store.current());
  [[maybe_unused]] const ListenerId sub = k.store.subscribe([&](const StoreEvent& e) { link->onDocumentChange(e); });
  auto addSat = ops::addDevice(*k.store.current(), k.store.ids(), k.a, "saturator", DeviceKind::AudioEffect);
  REQUIRE(k.store.dispatch(addSat->command).ok());
  auto addLfo = ops::addDevice(*k.store.current(), k.store.ids(), k.a, "lfo_device", DeviceKind::AudioEffect);
  REQUIRE(k.store.dispatch(addLfo->command).ok());
  const DeviceId sat = addSat->devices.at(0), lfo = addLfo->devices.at(0);
  REQUIRE(k.store.dispatch(ops::connectWire(*k.store.current(), k.store.ids(), lfo, "lfo", sat, "drive")->command).ok());
  link->flush();
  CHECK(link->stats().invalidWires == 0);
  REQUIRE(link->wireStates().size() == 1);
  CHECK(link->wireStates()[0].status == WireStatus::Lowered);
  const std::string noDrive = replaced(replaced(link->devices().stockText("saturator"), "param drive      = 0dB",
                                                "param drv        = 0dB"),
                                       "u   = pre * drive", "u   = pre * drv");
  REQUIRE(k.store.dispatch(ops::editDevicePatch(*k.store.current(), sat, noDrive)->command).ok());
  link->flush();
  CHECK(link->lastError().empty());
  CHECK(link->stats().invalidWires == 1);
  CHECK(link->wireStates()[0].status == WireStatus::Invalid);
  CHECK(wiresOf(*k.store.current()).size() == 1);  // kept in the document
  REQUIRE(k.store.undo());
  link->flush();
  CHECK(link->stats().invalidWires == 0);
  CHECK(link->wireStates()[0].status == WireStatus::Lowered);
}
