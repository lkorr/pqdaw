// T1.x-silence (SPEC §7.7) at the project level: the image's silence reports (ProcessContext::outSilent / tailsDone /
// dormant) drive the units and the engine.
//   - a unit whose image is dormant is skipped while its buses stay silent (as T3.17-fix2's idle units), bit-exactly:
//     the same samples as running every unit; within the −120 dBFS tail cut of never sleeping;
//   - silent buses are marked by their producer's report, so the units reading them sleep too;
//   - the engine's idle gate waits for every tail (M3 review M15a): an echo 400 ms after a short note plays as without
//     the gate, where the old gate (quiet + 250 ms of silent output) froze it; and the engine still falls asleep once
//     the last echo has died away.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "pq/core/alloc_trap.h"
#include "pq/core/denormal_guard.h"
#include "pq/engine/engine.h"
#include "pq/engine/note_list.h"
#include "pq/engine/project_compile.h"
#include "pq/engine/project_program.h"
#include "pq/graph/parse.h"
#include "idle_project_fixture.h"

namespace graph = pq::graph;
namespace eng = pq::engine;

namespace {

constexpr double kSr = 48000.0;
constexpr uint32_t kBlock = 64;

const char* kDevices = R"(
patch Synth {
  param level = 0.3 [0..1]
  out   out : audio voice mono
  target = voice.gate ? 1.0 : 0.0
  env = z1(env + (target - env) * 0.02)
  out = sine(phasor(voice.hz)) * env * level
}

patch Echo {
  in    x : audio global mono
  param time = 400ms [10ms..1s]
  param fb = 0.2 [0..0.9]
  param mix = 0.5 [0..1]
  out   y : audio global mono
  d = delay(x + d * fb, time, 1s)
  y = x + d * mix
}

patch Verb {
  in    x : audio global mono
  param fb = 0.6 [0..0.9]
  out   y : audio global mono
  c1 = delay(x + c1 * fb, 29.7ms, 40ms)
  c2 = delay(x + c2 * fb, 37.1ms, 40ms)
  m  = (c1 + c2) * 0.5
  d  = delay(w, 5ms, 10ms)
  w  = m + d * 0.5
  y  = d - w * 0.5
}
)";

const char* kTracks = R"(
patch Track {
  param vol = 0.5 [0..1]
  out   out  : audio global mono
  out   send : audio global mono
  e = Echo(Synth())
  out  = gain(e, vol, 2ms)
  send = e * 0.3
}

patch Return {
  in    in : audio global mono
  out   out : audio global mono
  out = Verb(in)
}

patch Master {
  in    in : audio global mono
  out   out : audio global mono
  out = tanh(in)
}
)";

struct Project {
  std::string devText, trackText;
  graph::PatchFile devices, tracks;
  graph::PatchLibrary library;
  graph::ProjectGraph graph;
};

std::unique_ptr<Project> makeProject(uint32_t instruments, bool withReturn = true) {
  auto p = std::make_unique<Project>();
  p->devText = kDevices;
  auto d = graph::parseDetailed(p->devText, "devices.pqp");
  REQUIRE(d.ok());
  p->devices = std::move(d.file);
  p->library.add(p->devices);
  graph::ParseOptions po;
  const auto sigs = graph::signatures(p->devices);
  po.externals = sigs;
  p->trackText = kTracks;
  auto t = graph::parseDetailed(p->trackText, "tracks.pqp", po);
  REQUIRE(t.ok());
  p->tracks = std::move(t.file);
  graph::ProjectGraph& g = p->graph;
  g.library = &p->library;
  for (uint32_t k = 0; k < instruments; ++k)
    g.tracks.push_back({k + 1, "T" + std::to_string(k), &p->tracks.patches[0], graph::TrackRole::Instrument, k});
  const uint32_t ret = static_cast<uint32_t>(g.tracks.size());
  if (withReturn) g.tracks.push_back({100, "Return", &p->tracks.patches[1], graph::TrackRole::Return, graph::kNone});
  const uint32_t master = static_cast<uint32_t>(g.tracks.size());
  g.tracks.push_back({101, "Master", &p->tracks.patches[2], graph::TrackRole::Master, graph::kNone});
  for (uint32_t k = 0; k < instruments; ++k) {
    g.wires.push_back({{k, "out"}, {master, "in"}, graph::WireKind::Audio});
    if (withReturn) g.wires.push_back({{k, "send"}, {ret, "in"}, graph::WireKind::Audio});
  }
  if (withReturn) g.wires.push_back({{ret, "out"}, {master, "in"}, graph::WireKind::Audio});
  g.output = {master, "out"};
  return p;
}

eng::ProjectCompileOptions compileOptions(bool sleep) {
  eng::ProjectCompileOptions o;
  o.sampleRate = kSr;
  o.maxBlockFrames = kBlock;
  o.voice.maxVoices = 8;
  o.voice.releaseTimeoutMs = 60.0f;  // the tracks' voices have no monitored output: they end 60 ms after note-off
  o.sleep = sleep;
  return o;
}

struct Render {
  std::vector<float> out;
  uint64_t rtViolations = 0;
  std::vector<uint64_t> idleBlocks;  // per unit at the end
  bool quietAtEnd = false;
  uint64_t firstQuiet = ~uint64_t{0};
};

Render renderProject(uint32_t instruments, bool sleep, bool idleUnits, uint64_t frames) {
  auto proj = makeProject(instruments);
  eng::ProjectCompiler compiler(compileOptions(sleep));
  auto b = compiler.build(proj->graph);
  REQUIRE_MESSAGE(b.ok(), (b.ok() ? "" : b.error().message));
  std::vector<std::unique_ptr<eng::NoteListSource>> notes;
  eng::ProjectConfig pc;
  pc.sampleRate = kSr;
  pc.maxBlockFrames = kBlock;
  pc.numOut = 1;
  pc.idleUnits = idleUnits;
  for (uint32_t k = 0; k < instruments; ++k) {
    // A short note per track, staggered; the last one ends at ≈ 0.3 s.
    notes.push_back(std::make_unique<eng::NoteListSource>(
        std::vector<eng::TimedNote>{{static_cast<int64_t>(2000 * k), 960, 220.0f * (1.0f + 0.25f * static_cast<float>(k)), 0.8f, 0.5f}}));
    pc.eventSources.push_back(notes.back()->source());
  }
  auto prog = eng::ProjectProgram::create(pc, b.value().layout);
  REQUIRE(prog.ok());
  eng::ProjectProgram& program = *prog.value();
  for (uint32_t u = 0; u < b.value().images.size(); ++u) REQUIRE(program.publish(u, std::move(b.value().images[u])).ok());
  Render r;
  std::vector<float> l(kBlock);
  float* outs[1] = {l.data()};
  for (uint64_t t = 0; t < frames; t += kBlock) {
    eng::AudioBlock ab;
    ab.out = outs;
    ab.numOut = 1;
    ab.frames = kBlock;
    ab.frameTime = static_cast<int64_t>(t);
    r.rtViolations += pq::alloc_trap::countRtViolations([&] {
      pq::DenormalGuard dg;
      program.process(ab);
    });
    r.out.insert(r.out.end(), l.begin(), l.end());
    if (program.quiet() && r.firstQuiet == ~uint64_t{0}) r.firstQuiet = t;
    if (!program.quiet()) r.firstQuiet = ~uint64_t{0};
  }
  for (uint32_t u = 0; u < program.numUnits(); ++u) r.idleBlocks.push_back(program.unitStats(u).idleBlocks);
  r.quietAtEnd = program.quiet();
  return r;
}

}  // namespace

TEST_CASE("project silence: dormant units are skipped bit-exactly; silent buses let their readers sleep (T1.x-silence)") {
  constexpr uint64_t kFrames = 48000 * 8;
  const Render skip = renderProject(3, true, true, kFrames);
  const Render run = renderProject(3, true, false, kFrames);
  const Render never = renderProject(3, false, false, kFrames);
  REQUIRE(skip.out.size() == run.out.size());
  bool same = true;
  double diff = 0.0, peak = 0.0;
  for (size_t i = 0; i < skip.out.size(); ++i) {
    same = same && skip.out[i] == run.out[i];
    diff = std::max(diff, static_cast<double>(std::fabs(skip.out[i] - never.out[i])));
    peak = std::max(peak, static_cast<double>(std::fabs(never.out[i])));
  }
  CHECK(same);  // skipping a dormant unit changes nothing
  MESSAGE("project: peak " << peak << ", max |sleeping - never sleeping| " << diff);
  CHECK(peak > 0.05);
  CHECK(diff <= 1e-5);  // the tails' −120 dBFS cut
  // Every unit (3 instruments, the return, the master) ends up skipped once its tails have died away.
  REQUIRE(skip.idleBlocks.size() == 5);
  for (uint64_t ib : skip.idleBlocks) CHECK(ib > 300);  // the echoes ring for ≈ 5.5 s of the 8 (to −180 dBFS)
  for (uint64_t ib : run.idleBlocks) CHECK(ib == 0);
  CHECK(skip.quietAtEnd);
  CHECK(never.quietAtEnd);  // no sleep plan: tails are not tracked (the T3.17 behaviour)
  // Quiet (for the engine's gate) only once the last echo and the reverb are below −120 dBFS: well after the notes.
  CHECK(skip.firstQuiet > 48000);
  CHECK(skip.rtViolations == 0);
}

TEST_CASE("project silence: the stdlib reverb/delay fixture falls fully asleep after its tails (T1.x-silence)") {
  // The acceptance fixture (idle_project_fixture.h; 30 tracks in bench/engine/bench_silence.cpp), 6 tracks here.
  constexpr uint32_t kFixtureTracks = 6;
  auto proj = idle_fixture::makeProject(kFixtureTracks);
  REQUIRE(proj != nullptr);
  eng::ProjectCompiler compiler(compileOptions(true));
  auto b = compiler.build(proj->graph);
  REQUIRE_MESSAGE(b.ok(), (b.ok() ? "" : b.error().message));
  // Every track's chain sleeps as segments (the instrument units' global stage, the return, the master).
  for (uint32_t u = 0; u < compiler.numUnits(); ++u) {
    INFO("unit " << u);
    CHECK_FALSE(compiler.unit(u).schedule->sleep.segments.empty());
    CHECK(compiler.unit(u).schedule->sleep.dormantCapable);
  }
  std::vector<std::unique_ptr<eng::NoteListSource>> notes;
  eng::ProjectConfig pc;
  pc.sampleRate = kSr;
  pc.maxBlockFrames = kBlock;
  pc.numOut = 2;
  for (uint32_t k = 0; k < kFixtureTracks; ++k) {
    notes.push_back(std::make_unique<eng::NoteListSource>(idle_fixture::chord(k)));
    pc.eventSources.push_back(notes.back()->source());
  }
  auto prog = eng::ProjectProgram::create(pc, b.value().layout);
  REQUIRE(prog.ok());
  eng::ProjectProgram& program = *prog.value();
  for (uint32_t u = 0; u < b.value().images.size(); ++u) REQUIRE(program.publish(u, std::move(b.value().images[u])).ok());
  std::vector<float> l(kBlock), r(kBlock);
  float* outs[2] = {l.data(), r.data()};
  double peak = 0.0;
  uint64_t allAsleepFrom = ~uint64_t{0}, rt = 0;
  std::vector<uint64_t> before(program.numUnits(), 0);
  for (uint64_t t = 0; t < 48000 * 14; t += kBlock) {
    eng::AudioBlock ab;
    ab.out = outs;
    ab.numOut = 2;
    ab.frames = kBlock;
    ab.frameTime = static_cast<int64_t>(t);
    rt += pq::alloc_trap::countRtViolations([&] {
      pq::DenormalGuard dg;
      program.process(ab);
    });
    for (float x : l) peak = std::max(peak, static_cast<double>(std::fabs(x)));
    bool all = true;
    for (uint32_t u = 0; u < program.numUnits(); ++u) {
      all = all && program.unitStats(u).idleBlocks == before[u] + 1;
      before[u] = program.unitStats(u).idleBlocks;
    }
    if (all && allAsleepFrom == ~uint64_t{0}) allAsleepFrom = t;
    if (!all) allAsleepFrom = ~uint64_t{0};
  }
  MESSAGE("stdlib fixture (" << kFixtureTracks << " tracks): peak " << peak << ", every unit skipped from " << allAsleepFrom / 48000.0
                             << " s on");
  CHECK(peak > 0.05);
  CHECK(allAsleepFrom >= 48000 * 3);  // not before the echoes have died away …
  CHECK(allAsleepFrom <= 48000 * 13);  // … but then for good
  CHECK(program.quiet());
  CHECK(rt == 0);
}

TEST_CASE("engine silence: the idle gate waits for every tail (M3 review M15a; T1.x-silence)") {
  auto renderEngine = [](float sleepAfterMs, bool sleep, uint64_t frames, bool& asleepAtEnd, uint64_t& asleepBlocks) {
    eng::EngineConfig ec;
    ec.sampleRate = kSr;
    ec.blockFrames = kBlock;
    ec.numOut = 1;
    ec.maxSlots = 4;
    ec.sleepAfterMs = sleepAfterMs;
    auto engine = eng::Engine::create(ec);
    REQUIRE(engine.ok());
    eng::Engine& e = *engine.value();
    auto proj = makeProject(1, false);  // instrument + echo → master: nothing else masks the gap before the echo
    eng::ProjectCompiler compiler(compileOptions(sleep));
    auto b = compiler.build(proj->graph);
    REQUIRE(b.ok());
    auto prog = eng::ProjectProgram::create(e.projectConfig(), b.value().layout);
    REQUIRE(prog.ok());
    for (uint32_t u = 0; u < b.value().images.size(); ++u) REQUIRE(prog.value()->publish(u, std::move(b.value().images[u])).ok());
    e.setProgram(std::move(prog.value()));
    std::vector<float> out, l(kBlock);
    float* outs[1] = {l.data()};
    for (uint64_t t = 0; t < frames; t += kBlock) {
      if (t == 4800) e.audition(0, 330.0f, 100.0f, 960);  // a 20 ms note at 0.1 s; its first echo at 0.5 s
      eng::AudioBlock ab;
      ab.out = outs;
      ab.numOut = 1;
      ab.frames = kBlock;
      pq::DenormalGuard dg;
      e.process(ab);
      out.insert(out.end(), l.begin(), l.end());
    }
    asleepAtEnd = e.asleep();
    asleepBlocks = e.stats().asleepBlocks;
    return out;
  };
  constexpr uint64_t kFrames = 48000 * 9;
  bool a1 = false, a2 = false, a3 = false;
  uint64_t s1 = 0, s2 = 0, s3 = 0;
  const std::vector<float> gated = renderEngine(100.0f, true, kFrames, a1, s1);
  const std::vector<float> free = renderEngine(0.0f, true, kFrames, a2, s2);
  const std::vector<float> legacy = renderEngine(100.0f, false, kFrames, a3, s3);  // no sleep plan: no tail tracking
  REQUIRE(gated.size() == free.size());
  auto energy = [](const std::vector<float>& x, uint64_t from, uint64_t to) {
    double e = 0.0;
    for (uint64_t i = from; i < to && i < x.size(); ++i) e += static_cast<double>(x[i]) * x[i];
    return e;
  };
  bool same = true;
  for (size_t i = 0; i < gated.size(); ++i) same = same && gated[i] == free[i];
  CHECK(same);                                         // the gate never cut a tail
  CHECK(energy(gated, 24000, 26000) > 1e-3);           // the first echo, 400 ms after the note
  CHECK(energy(legacy, 24000, 26000) < energy(gated, 24000, 26000) * 1e-3);  // the old gate froze it (M15a)
  CHECK(a1);                                           // and it still sleeps once every tail is done
  CHECK(s1 > 0);
  CHECK_FALSE(a2);
}

// M4 review M4: the project param block and the param ring grow while the engine runs (a project with more params
// than the engine was made for plays every stored value), and the writes stay in order across a ring change. L1: an
// idle unit no longer compares its linked params every block, so the generation it skips by must move with every
// write: a write to a sleeping unit's linked param wakes it in the next block.
TEST_CASE("engine: the project param block and the param ring grow while running; idle units still wake on a write (M4, L1)") {
  eng::EngineConfig ec;
  ec.sampleRate = kSr;
  ec.blockFrames = kBlock;
  ec.numOut = 1;
  ec.maxSlots = 4;
  ec.sleepAfterMs = 0.0f;
  ec.numProjectParams = 4;
  ec.paramRing = 4;
  auto engine = eng::Engine::create(ec);
  REQUIRE(engine.ok());
  eng::Engine& e = *engine.value();
  auto proj = makeProject(1, false);
  eng::ProjectCompileOptions co = compileOptions(true);
  co.paramLinks = {{0, "vol", 2}};
  eng::ProjectCompiler compiler(co);
  auto b = compiler.build(proj->graph);
  REQUIRE(b.ok());
  auto prog = eng::ProjectProgram::create(e.projectConfig(), b.value().layout);
  REQUIRE(prog.ok());
  for (uint32_t u = 0; u < b.value().images.size(); ++u) REQUIRE(prog.value()->publish(u, std::move(b.value().images[u])).ok());
  e.setProgram(std::move(prog.value()));
  eng::ProjectProgram& program = *e.program();
  std::vector<float> l(kBlock);
  float* outs[1] = {l.data()};
  auto run = [&](int blocks) {
    for (int i = 0; i < blocks; ++i) {
      eng::AudioBlock ab;
      ab.out = outs;
      ab.numOut = 1;
      ab.frames = kBlock;
      pq::DenormalGuard dg;
      e.process(ab);
    }
  };
  CHECK(e.setParam(2, 0.25f));
  run(4);
  CHECK(program.projectParams()[2] == 0.25f);
  // More writes than the ring holds would be dropped: grow it (and the block) first, as ProjectLink does.
  CHECK(e.setParam(1, 1.0f));  // into the old ring
  program.reserveProjectParams(1000);
  e.growParamRing(1024);
  CHECK(e.paramRingCapacity() >= 1024);
  CHECK(program.projectParamCapacity() == 1000);
  bool all = true;
  for (uint32_t s = 3; s < 1000; ++s) all = all && e.setParam(s, static_cast<float>(s));
  CHECK(all);
  CHECK(e.setParam(1, 2.0f));  // into the new ring: after the old ring's write
  run(1);
  REQUIRE(program.projectParams().size() == 1000);
  CHECK(program.projectParams()[1] == 2.0f);
  CHECK(program.projectParams()[2] == 0.25f);  // copied over from the old block
  CHECK(program.projectParams()[999] == 999.0f);
  e.collect();  // frees the old ring
  // A unit linked past the old block size (a new link after the growth: a rebuild, as ProjectLink's) reads its value.
  compiler.setParamLinks({{0, "vol", 900}});
  auto tb = compiler.rebuild(proj->graph, {});
  REQUIRE(tb.ok());
  CHECK(tb.value().compiled == 1);  // T0's unit (its links changed)
  REQUIRE(program.publishTopology(tb.value().layout, std::move(tb.value().units)).ok());
  CHECK(e.setParam(900, 0.125f));
  run(1000);  // the swap, then nothing sounds or changes: the units go idle
  uint32_t t0 = 0;
  while (t0 < program.numUnits() && program.layout().units[t0].name != "T0") ++t0;
  REQUIRE(t0 < program.numUnits());
  const auto vols = program.params(t0);
  CHECK(std::find(vols.begin(), vols.end(), 0.125f) != vols.end());
  const uint64_t idle0 = program.unitStats(t0).idleBlocks;
  run(10);
  REQUIRE(program.unitStats(t0).idleBlocks == idle0 + 10);  // asleep, skipping the link scan
  CHECK(e.setParam(900, 0.5f));
  run(1);
  CHECK(program.unitStats(t0).idleBlocks == idle0 + 10);  // the write woke it (its param changed)
  const auto vols2 = program.params(t0);
  CHECK(std::find(vols2.begin(), vols2.end(), 0.5f) != vols2.end());
}
