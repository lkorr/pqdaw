// GU.9 v2 (pq/document/racks.h): racks are parallel groups. Each case builds a rack from the edit builders (groups,
// value blocks, atom blocks, wires: nothing of its own in the lowering) and renders it against the reference the T4.8
// racks (Rack / Chain, removed by GU.9) rendered for the same project (tests/app/fixtures/gu9_racks, recorded by
// a6bcfd6a): diff 0 where the arithmetic is the same, a bound where it now runs in float atoms instead of document-side
// doubles. Every render runs through the interpreter and the JIT (bit for bit equal), every block under the allocation
// trap.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <ostream>
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
#include "pq/document/main_path.h"
#include "pq/document/racks.h"
#include "pq/engine/note_list.h"
#include "pq/engine/project_compile.h"
#include "pq/engine/project_program.h"
#include "pq/media/wav_file.h"

using namespace pq;
using namespace pq::doc;

namespace {

constexpr uint32_t kBlock = 64;
constexpr double kRate = 48000.0;
constexpr uint32_t kFrames = 12000;

const DeviceLibrary& library() {
  static auto lib = app::loadDeviceLibrary();
  REQUIRE_MESSAGE(lib.ok(), (lib.ok() ? std::string() : lib.error().message));
  return **lib;
}

std::vector<engine::TimedNote> chord() { return {{0, 9000, 220.0f, 0.8f, 0.5f}, {2000, 8000, 277.18f, 0.9f, 0.5f}}; }

golden::Channels render(const Project& p, TrackId track, engine::UnitBackend backend = {}) {
  LowerOptions lo;
  lo.library = &library().library();
  lo.devices = &library();
  lo.slotOf = [track](TrackId t) { return t == track ? 0u : 1u; };
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
  engine::NoteListSource notes(chord()), none(std::vector<engine::TimedNote>{});
  engine::ProjectConfig pc;
  pc.sampleRate = kRate;
  pc.maxBlockFrames = kBlock;
  pc.numOut = 2;
  pc.numProjectParams = static_cast<uint32_t>(values.size());
  pc.eventSources = {notes.source(), none.source()};
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
  for (int64_t t = 0; t < kFrames; t += kBlock) {
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

double maxDiff(const golden::Channels& a, const golden::Channels& b, size_t from = 0) {
  REQUIRE(a.size() == b.size());
  double m = 0.0;
  for (size_t c = 0; c < a.size(); ++c) {
    REQUIRE(a[c].size() == b[c].size());
    for (size_t i = from; i < a[c].size(); ++i) {
      const double d = std::fabs(static_cast<double>(a[c][i]) - static_cast<double>(b[c][i]));
      m = std::max(m, std::isnan(d) ? 1e9 : d);
    }
  }
  return m;
}
float peak(const golden::Channels& a) {
  float p = 0.0f;
  for (const auto& c : a)
    for (const float v : c) p = std::max(p, std::fabs(v));
  return p;
}
golden::Channels reference(const char* name) {
  auto w = media::readWav(std::filesystem::path(PQ_TEST_DATA_DIR) / "app" / "fixtures" / "gu9_racks" / name);
  REQUIRE_MESSAGE(w.ok(), name);
  return w->channels;
}
// Interpreter and JIT, equal bit for bit; compared with the T4.8 reference `name`.
double againstReference(const Project& p, TrackId t, const char* name) {
  const golden::Channels a = render(p, t);
  const golden::Channels j = render(p, t, compile::jitUnitBackend());
  CHECK(maxDiff(a, j) == 0.0);
  const golden::Channels ref = reference(name);
  const double d = maxDiff(a, ref);
  MESSAGE(name << ": max |diff| vs the T4.8 rack = " << d << " (peak " << peak(ref) << ", " << peak(a) << ")");
  CHECK(peak(a) > 1e-3f);
  return d;
}

template <class E>
E run(DocumentStore& s, Result<E> e) {
  REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
  REQUIRE(s.dispatch(e->command).ok());
  return *e;
}
const Track& trackOf(DocumentStore& s, TrackId t) { return *findTrackPtr(*s.current(), t); }
RackInfo rackIn(DocumentStore& s, TrackId t) {
  const std::vector<RackInfo> racks = findRacks(*s.current(), trackOf(s, t));
  REQUIRE(racks.size() == 1);
  return racks.front();
}

}  // namespace

TEST_CASE("racks are parallel groups: an Audio Effect Rack (two chains, a macro mapping, a chain mixer) renders as "
          "the T4.8 rack did") {
  DocumentStore s;
  const TrackId t = run(s, ops::createTrack(*s.current(), s.ids(), TrackKind::Midi)).tracks.at(0);
  const DeviceId a = run(s, ops::addRack(*s.current(), s.ids(), t, false)).devices.at(0);
  const DeviceId b = run(s, ops::addChain(*s.current(), s.ids(), a)).devices.at(0);
  {  // two sibling groups fed from the instrument, summed into the mixer: one rack, derived from the wiring
    const RackInfo r = rackIn(s, t);
    REQUIRE(r.chains.size() == 2);
    CHECK(r.chains[0].id == a);
    CHECK(r.chains[1].id == b);
    CHECK(r.chains[1].name == "Chain 2");
    CHECK(!r.instrument);
    CHECK(isGroupDevice(*r.chains[0].group));
    CHECK(r.chains[0].volumeBlock.valid());
    CHECK(mainPath(*s.current(), trackOf(s, t)).contains(a));  // the path runs through the first chain
  }
  const DeviceId util = run(s, ops::addToChain(*s.current(), s.ids(), a, "utility", DeviceKind::AudioEffect)).devices.at(0);
  run(s, ops::addToChain(*s.current(), s.ids(), b, "saturator", DeviceKind::AudioEffect));
  const WireId m = run(s, ops::mapMacro(*s.current(), s.ids(), a, 0, util, "gain")).wires.at(0);
  run(s, ops::setMacroMapping(*s.current(), m, 0.2, 0.8));
  run(s, ops::setMacroValue(*s.current(), a, 0, 64.0));
  {
    const RackInfo r = rackIn(s, t);
    REQUIRE(r.mappings.size() == 1);
    CHECK(r.mappings[0].macro == 0);
    CHECK(r.mappings[0].device == util);
    CHECK(std::fabs(r.mappings[0].min - 0.2) < 1e-12);
    CHECK(std::fabs(r.mappings[0].max - 0.8) < 1e-12);
    CHECK(r.macros[0].block.valid());
    CHECK(std::fabs(r.macros[0].value - 64.0) < 1e-9);
    CHECK(r.chains[0].devices.size() == 1);
  }
  const double d1 = againstReference(*s.current(), t, "audio_rack.wav");
  CHECK(d1 <= 1e-6);
  run(s, ops::setChainMixer(*s.current(), b, -6.0f, 0.4f));
  CHECK(std::fabs(rackIn(s, t).chains[1].volumeDb + 6.0f) < 1e-4f);
  CHECK(std::fabs(rackIn(s, t).chains[1].pan - 0.4f) < 1e-6f);
  const double d2 = againstReference(*s.current(), t, "audio_rack_mix.wav");
  CHECK(d2 <= 1e-5);  // the chain gains: float atoms now, document doubles then
}

TEST_CASE("racks are parallel groups: an Instrument Rack's chains are instrument groups of the same notes (one voice "
          "engine: voicing does not double) and render as the T4.8 rack did, a velocity zone too") {
  DocumentStore s;
  const TrackId t = run(s, ops::createTrack(*s.current(), s.ids(), TrackKind::Midi)).tracks.at(0);
  std::vector<DeviceId> all;
  for (const DevicePtr& d : trackOf(s, t).devices) all.push_back(d->id);
  run(s, ops::deleteDevices(*s.current(), all));
  const DeviceId a = run(s, ops::addRack(*s.current(), s.ids(), t, true, 0)).devices.at(0);
  const DeviceId b = run(s, ops::addChain(*s.current(), s.ids(), a)).devices.at(0);
  run(s, ops::addToChain(*s.current(), s.ids(), a, std::string(kDefaultInstrument), DeviceKind::Instrument));
  run(s, ops::addToChain(*s.current(), s.ids(), b, std::string(kDefaultInstrument), DeviceKind::Instrument));
  const DeviceId util = run(s, ops::addToChain(*s.current(), s.ids(), b, "utility", DeviceKind::AudioEffect)).devices.at(0);
  run(s, ops::setDeviceParam(*s.current(), util, "gain", 0.6));
  {
    const RackInfo r = rackIn(s, t);
    CHECK(r.instrument);
    CHECK(!r.voiceChains);
    REQUIRE(r.chains.size() == 2);
    CHECK(r.chains[1].devices.size() == 2);
    // Each chain is an instrument group of the track's notes; nothing inside voices again.
    const VoiceContexts vc = voiceContexts(*s.current());
    CHECK(vc.blockOf(a) != nullptr);
    CHECK(vc.blockOf(b) != nullptr);
    CHECK(vc.blockOf(r.chains[0].mixer) == nullptr);
  }
  const double d1 = againstReference(*s.current(), t, "instrument_rack.wav");
  CHECK(d1 <= 1e-6);
  run(s, ops::setChainZone(*s.current(), s.ids(), b, ZoneKind::Velocity, Zone{0.0, 0.0, 100.0, 110.0}));
  CHECK(rackIn(s, t).chains[1].velocityZone.valid());
  CHECK(std::fabs(rackIn(s, t).chains[1].velocity.fadeHi - 100.0) < 1e-9);
  const double d2 = againstReference(*s.current(), t, "instrument_rack_zone.wav");
  CHECK(d2 <= 1e-4);  // the zone's ramp in float atoms (v·a + b in document doubles then)
  // Off again: the zone group goes (an off zone costs nothing) and the render is the zone-less one.
  run(s, ops::setChainZone(*s.current(), s.ids(), b, ZoneKind::Velocity, fullZone(ZoneKind::Velocity, KeyZoneUnit::Hz)));
  CHECK(!rackIn(s, t).chains[1].velocityZone.valid());
  CHECK(againstReference(*s.current(), t, "instrument_rack.wav") == d1);
}

TEST_CASE("racks are parallel groups: wiring two groups in parallel by hand makes a rack; mute, solo and the chain "
          "selector are blocks") {
  DocumentStore s;
  const TrackId t = run(s, ops::createTrack(*s.current(), s.ids(), TrackKind::Midi)).tracks.at(0);
  const DeviceId a = run(s, ops::addRack(*s.current(), s.ids(), t, false)).devices.at(0);
  const DeviceId b = run(s, ops::addChain(*s.current(), s.ids(), a)).devices.at(0);
  const golden::Channels two = render(*s.current(), t);
  // Chain 2 muted: its Gate closes; soloing chain 2 closes chain 1's instead.
  run(s, ops::setChainActive(*s.current(), b, false));
  CHECK(!rackIn(s, t).chains[1].active);
  const golden::Channels one = render(*s.current(), t);
  CHECK(peak(one) > 1e-3f);
  CHECK(std::fabs(peak(two) - 2.0f * peak(one)) < 1e-5f);  // two empty chains pass the signal twice
  run(s, ops::setChainActive(*s.current(), b, true));
  run(s, ops::setChainSolo(*s.current(), b, true));
  CHECK(maxDiff(render(*s.current(), t), one) == 0.0);
  run(s, ops::setChainSolo(*s.current(), b, false));
  // A chain zone 64 … 127 on chain 2 (the selector at 0): chain 1 alone; the selector at 100: both.
  run(s, ops::setChainZone(*s.current(), s.ids(), b, ZoneKind::Select, Zone{64.0, 64.0, 127.0, 127.0}));
  REQUIRE(rackIn(s, t).selector.valid());
  CHECK(maxDiff(render(*s.current(), t), one) <= 1e-6);
  run(s, ops::setSelectorValue(*s.current(), a, 100.0));
  CHECK(maxDiff(render(*s.current(), t), two, 4800) <= 1e-6);  // (a chain zone's gain fades in from 0: settled after 0.1 s)
  // Two groups wired by hand in parallel (fed from the same block, summed into the mixer): a rack.
  DocumentStore h;
  const TrackId t2 = run(h, ops::createTrack(*h.current(), h.ids(), TrackKind::Midi)).tracks.at(0);
  const MainPath path = mainPath(*h.current(), trackOf(h, t2));
  REQUIRE(!path.blocks.empty());
  const DeviceId g1 = h.ids().device(), g2 = h.ids().device();
  for (const DeviceId g : {g1, g2}) {
    REQUIRE(h.dispatch(AddDevice{t2, makeGroup(g), kAppend}).ok());
    Wire in;
    in.id = h.ids().wire();
    in.from = path.blocks.back()->id;
    in.to = g;
    REQUIRE(h.dispatch(AddWire{in}).ok());
    Wire pass;
    pass.id = h.ids().wire();
    pass.from = g;
    pass.port = std::string(kGroupInPort);
    pass.to = g;
    pass.param = std::string(kGroupOutPort);
    REQUIRE(h.dispatch(AddWire{pass}).ok());
    Wire out;
    out.id = h.ids().wire();
    out.from = g;
    out.mixer = t2;
    REQUIRE(h.dispatch(AddWire{out}).ok());
  }
  const std::vector<RackInfo> racks = findRacks(*h.current(), trackOf(h, t2));
  REQUIRE(racks.size() == 1);
  CHECK(racks[0].chains.size() == 2);
  CHECK(!racks[0].chains[0].mixer.valid());  // hand-wired: no chain mixer (the Device view shows its row without one)
}
