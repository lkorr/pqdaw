// GU.9 (docs/plans/GU.md "One hierarchy"; doc model.h "Racks"): a rack is a group of parallel chain groups, macros are
// value blocks, a mapping is a Travel wire, the chain selector is a gate per chain made of atom and value blocks. Each
// case renders against a reference built another way, through the interpreter and the JIT (bit for bit equal), every
// block under the allocation trap (RtScope).
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

#include "../golden/golden_audio.h"
#include "playback_fixture.h"
#include "pq/app/project_link.h"
#include "pq/compile/compile.h"
#include "pq/core/alloc_trap.h"
#include "pq/core/denormal_guard.h"
#include "pq/document/devices.h"
#include "pq/document/edit_ops.h"
#include "pq/document/instrument_groups.h"
#include "pq/document/lower.h"
#include "pq/document/main_path.h"
#include "pq/engine/note_list.h"
#include "pq/engine/project_compile.h"
#include "pq/engine/project_program.h"

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

using Notes = std::vector<engine::TimedNote>;
Notes chord() { return {{0, 9000, 220.0f, 0.8f, 0.5f}, {2000, 8000, 277.18f, 0.9f, 0.5f}}; }

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
  engine::NoteListSource notes(chord()), none(Notes{});
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

double maxDiff(const golden::Channels& a, const golden::Channels& b, float scaleB = 1.0f) {
  double m = 0.0;
  for (size_t c = 0; c < a.size(); ++c)
    for (size_t i = 0; i < a[c].size(); ++i) {
      const double d = std::fabs(static_cast<double>(a[c][i]) - static_cast<double>(scaleB * b[c][i]));
      m = std::max(m, std::isnan(d) ? 1e9 : d);
    }
  return m;
}
float peak(const golden::Channels& a) {
  float p = 0.0f;
  for (const auto& c : a)
    for (const float v : c) p = std::max(p, std::fabs(v));
  return p;
}
golden::Channels played(const Project& p, TrackId t, const std::string& what) {
  const golden::Channels a = render(p, t);
  const golden::Channels j = render(p, t, compile::jitUnitBackend());
  MESSAGE(what << ": interpreter vs JIT max |diff| = " << maxDiff(a, j) << ", peak " << peak(a));
  CHECK(maxDiff(a, j) == 0.0);
  return a;
}

template <class E>
E run(DocumentStore& s, Result<E> e) {
  REQUIRE_MESSAGE(e.ok(), (e.ok() ? std::string() : e.error().message));
  REQUIRE(s.dispatch(e->command).ok());
  return *e;
}
const Track& trackOf(DocumentStore& s, TrackId t) { return *findTrackPtr(*s.current(), t); }
TrackId midi(DocumentStore& s) { return run(s, ops::createTrack(*s.current(), s.ids(), TrackKind::Midi)).tracks.at(0); }
std::vector<DeviceId> chainsOf(DocumentStore& s, TrackId t, DeviceId rack) {
  std::vector<DeviceId> out;
  for (const Device* c : rackChains(*s.current(), trackOf(s, t), rack)) out.push_back(c->id);
  return out;
}
DeviceId named(DocumentStore& s, TrackId t, std::string_view name) {
  for (const DevicePtr& d : trackOf(s, t).devices)
    if (d->name == name) return d->id;
  FAIL("no block named " << name);
  return {};
}
void setPosition(DocumentStore& s, DeviceId d, std::string param, double position) {
  REQUIRE(s.dispatch(SetDeviceParam{d, std::move(param), position}).ok());
}

}  // namespace

TEST_CASE("racks are groups: an Instrument Rack's parallel chains sum, and only the rack voices (its chains are "
          "nested groups inside the one instrument group)") {
  DocumentStore s;
  const TrackId t = midi(s);
  const Snapshot plain = s.current();  // the new track: an instrument group [Basic Synth]
  const golden::Channels ref = played(*plain, t, "the default instrument group");
  CHECK(peak(ref) > 1e-2);
  // An Instrument Rack takes the notes in its place; its first chain gets a Basic Synth: the same sound.
  const ops::Edit rack = run(s, ops::addRack(*s.current(), s.ids(), t, true));
  const DeviceId r = rack.devices.at(0);
  REQUIRE(isGroupDevice(*findDevice(*s.current(), r).device));
  CHECK(mainPath(*s.current(), trackOf(s, t)).blocks.at(0)->id == r);
  std::vector<DeviceId> chains = chainsOf(s, t, r);
  REQUIRE(chains.size() == 1);
  run(s, ops::addToChain(*s.current(), s.ids(), chains[0], std::string(kDefaultInstrument), DeviceKind::Instrument));
  const golden::Channels one = played(*s.current(), t, "Instrument Rack [Basic Synth]");
  MESSAGE("rack of one chain vs the instrument group: max |diff| = " << maxDiff(one, ref));
  CHECK(maxDiff(one, ref) == 0.0);
  // Only the rack is a voice context: its chain groups and their synths run per voice inside it.
  const VoiceContexts vc = voiceContexts(*s.current());
  CHECK(vc.list.size() == 1);
  REQUIRE(vc.blockOf(r) != nullptr);
  CHECK(vc.blockOf(chains[0]) == nullptr);
  CHECK(vc.memberOf(chains[0]) == vc.blockOf(r));
  // A second chain with the same synth: the rack's Out sums them.
  const DeviceId c2 = run(s, ops::addChain(*s.current(), s.ids(), r)).devices.at(0);
  run(s, ops::addToChain(*s.current(), s.ids(), c2, std::string(kDefaultInstrument), DeviceKind::Instrument));
  chains = chainsOf(s, t, r);
  CHECK(chains.size() == 2);
  CHECK(findDevice(*s.current(), c2).device->name == "Chain 2");
  const golden::Channels two = played(*s.current(), t, "Instrument Rack [Basic Synth | Basic Synth]");
  CHECK(voiceContexts(*s.current()).list.size() == 1);
  // The sum of the chains, each played alone (the other switched off: left out of the graph).
  REQUIRE(s.dispatch(SetDeviceActive{c2, false}).ok());
  const golden::Channels a = played(*s.current(), t, "chain 1 alone");
  REQUIRE(s.dispatch(SetDeviceActive{c2, true}).ok());
  REQUIRE(s.dispatch(SetDeviceActive{chains[0], false}).ok());
  const golden::Channels b = played(*s.current(), t, "chain 2 alone");
  REQUIRE(s.dispatch(SetDeviceActive{chains[0], true}).ok());
  golden::Channels sum = a;
  for (size_t c = 0; c < sum.size(); ++c)
    for (size_t i = 0; i < sum[c].size(); ++i) sum[c][i] += b[c][i];
  MESSAGE("two chains vs chain 1 + chain 2: max |diff| = " << maxDiff(two, sum) << ", peak " << peak(two));
  CHECK(maxDiff(two, sum) <= 1e-6);
  CHECK(maxDiff(a, one) == 0.0);  // chain 1 alone is the one-chain rack
  CHECK(peak(b) > 1e-2);
  // Undo takes the chain away again (the four activator steps, the synth, the chain).
  for (int i = 0; i < 6; ++i) REQUIRE(s.undo());
  CHECK(chainsOf(s, t, r).size() == 1);
}

TEST_CASE("racks are groups: the chain selector gates each chain by its select zone (value blocks lo / hi, atoms "
          "sub / min / add / clip / mul inside the rack)") {
  DocumentStore s;
  const TrackId t = midi(s);
  const golden::Channels dry = played(*s.current(), t, "dry");
  // An Audio Effect Rack after the instrument: two empty chains (each passes the signal): twice the dry signal.
  const DeviceId r = run(s, ops::addRack(*s.current(), s.ids(), t, false)).devices.at(0);
  run(s, ops::addChain(*s.current(), s.ids(), r));
  CHECK(chainsOf(s, t, r).size() == 2);
  CHECK(maxDiff(played(*s.current(), t, "two empty chains"), dry, 2.0f) == 0.0);
  const ops::Edit sel = run(s, ops::addChainSelector(*s.current(), s.ids(), r));
  const DeviceId select = sel.devices.at(0);
  CHECK(findDevice(*s.current(), select).device->name == kChainSelectName);
  CHECK(chainsOf(s, t, r).size() == 2);  // the gates keep the chains chains
  CHECK(!ops::addChainSelector(*s.current(), s.ids(), r).ok());  // one per rack
  // Every zone 0 … 127 (Live's default): both still play.
  CHECK(maxDiff(played(*s.current(), t, "selector, full zones"), dry, 2.0f) == 0.0);
  // Chain 2 from 64 up, the selector at 0: chain 1 alone.
  setPosition(s, named(s, t, "Chain 2 lo"), std::string(kValueParam), 64.0 / kSelectMax);
  CHECK(maxDiff(played(*s.current(), t, "select 0, chain 2 from 64"), dry) == 0.0);
  // The selector at 100: both; chain 1 up to 50 only: chain 2 alone.
  setPosition(s, select, std::string(kValueParam), 100.0 / kSelectMax);
  CHECK(maxDiff(played(*s.current(), t, "select 100"), dry, 2.0f) == 0.0);
  setPosition(s, named(s, t, "Chain 1 hi"), std::string(kValueParam), 50.0 / kSelectMax);
  CHECK(maxDiff(played(*s.current(), t, "select 100, chain 1 up to 50"), dry) == 0.0);
  // A chain added now gets its gate too.
  const DeviceId c3 = run(s, ops::addChain(*s.current(), s.ids(), r)).devices.at(0);
  CHECK(named(s, t, "Chain 3 lo").valid());
  CHECK(chainsOf(s, t, r).size() == 3);
  CHECK(c3.valid());
  CHECK(maxDiff(played(*s.current(), t, "three chains, chain 1 out of zone"), dry, 2.0f) == 0.0);
}

TEST_CASE("racks are groups: a macro is a value block and a mapping a Travel wire — the control plays min + (max − "
          "min) · macro, as its knob set there does") {
  DocumentStore s;
  const TrackId t = midi(s);
  const ops::Edit rack = run(s, ops::addRack(*s.current(), s.ids(), t, false));
  const DeviceId r = rack.devices.at(0);
  const DeviceId chain = chainsOf(s, t, r).at(0);
  const DeviceId util = run(s, ops::addToChain(*s.current(), s.ids(), chain, "utility", DeviceKind::AudioEffect)).devices.at(0);
  const DeviceId macro = named(s, t, "Macro 1");
  CHECK(isValueDevice(*findDevice(*s.current(), macro).device));
  // The macro's knob on the rack's panel.
  const Device& rd = *findDevice(*s.current(), r).device;
  REQUIRE(rd.hasPanel());
  CHECK(rd.panel->elements.size() == kDefaultMacros);
  // Reference: Utility's gain knob at 0.5 by hand.
  setPosition(s, util, "gain", 0.5);
  const golden::Channels ref = played(*s.current(), t, "gain knob at 0.5");
  // Mapped 0.2 … 0.8, the macro at 0.5: 0.2 + 0.6 · 0.5 = 0.5.
  const size_t steps = s.history().cursor();
  run(s, ops::mapMacro(*s.current(), s.ids(), macro, util, "gain", 0.2, 0.8));
  CHECK(s.history().cursor() == steps + 1);
  const Wire* w = nullptr;
  for (const Wire& x : wiresOf(*s.current()))
    if (x.from == macro && x.to == util) w = &x;
  REQUIRE(w != nullptr);
  CHECK(w->travels());
  CHECK(w->amount == doctest::Approx(0.6));
  setPosition(s, macro, std::string(kValueParam), 0.5);
  const golden::Channels mapped = played(*s.current(), t, "macro 0.5 mapped 0.2 … 0.8");
  MESSAGE("mapped vs knob: max |diff| = " << maxDiff(mapped, ref) << " (peak " << peak(ref) << ")");
  CHECK(maxDiff(mapped, ref) <= 1e-5 * std::max(1.0f, peak(ref)));
  // The macro at 0: the control at its min (0.2), not the reference.
  setPosition(s, macro, std::string(kValueParam), 0.0);
  CHECK(maxDiff(played(*s.current(), t, "macro 0"), ref) > 1e-3);
}
