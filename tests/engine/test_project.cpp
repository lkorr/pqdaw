// T1.x-arch1: per-track compile units — partition (merges, cycles, dependency order), composition (state keys keep
// the track id), the per-unit project vs one whole-project program of the same graph, a one-track edit that
// recompiles and crossfades only its unit (every other unit bit-identical), cross-track modulation (merged,
// sample-accurate) vs a feed-forward bus, publish validation, RtScope and reclamation.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "pq/core/alloc_trap.h"
#include "pq/core/denormal_guard.h"
#include "pq/engine/interpreter.h"
#include "pq/engine/note_list.h"
#include "pq/engine/project_compile.h"
#include "pq/engine/project_program.h"
#include "pq/graph/optimise.h"
#include "pq/graph/parse.h"
#include "pq/graph/schedule.h"
#include "project_fixture.h"

namespace graph = pq::graph;
namespace eng = pq::engine;
using project_fixture::Options;
using project_fixture::Project;

namespace {

constexpr double kSr = 48000.0;
constexpr uint32_t kBlock = 64;

eng::ProjectCompileOptions compileOptions() {
  eng::ProjectCompileOptions o;
  o.sampleRate = kSr;
  o.maxBlockFrames = kBlock;
  o.voice.maxVoices = 8;
  o.voice.silenceDb = -300.0f;  // no voice ends inside a test window (the whole-project program ends voices only
                                // when every instrument's monitor is silent)
  return o;
}

// A playable project: the compiled units, their note sources, the running program.
struct Rig {
  std::unique_ptr<Project> proj;
  std::vector<std::unique_ptr<eng::NoteListSource>> notes;
  std::unique_ptr<eng::ProjectCompiler> compiler;
  eng::ProjectBuild build;
  std::unique_ptr<eng::ProjectProgram> program;
  int64_t time = 0;
  uint64_t rtViolations = 0;
};

// Every instrument track k plays a short phrase; sameNotes: all tracks the same one.
std::vector<eng::TimedNote> phrase(uint32_t k, bool sameNotes) {
  const uint32_t v = sameNotes ? 0u : k;
  std::vector<eng::TimedNote> n;
  const float base = 110.0f * (1.0f + 0.125f * static_cast<float>(v % 8));
  n.push_back({0, 6000, base, 0.8f, 0.5f});
  n.push_back({700 + 13 * v, 3000, base * 1.5f, 0.6f, 0.5f});
  n.push_back({2500 + 7 * v, 9000, base * 1.25f, 0.7f, 0.5f});
  return n;
}

std::unique_ptr<Rig> makeRig(const Options& o, bool whole, bool sameNotes, eng::ProjectCompileOptions co = compileOptions()) {
  auto r = std::make_unique<Rig>();
  r->proj = project_fixture::makeProject(o);
  REQUIRE(r->proj != nullptr);
  r->compiler = std::make_unique<eng::ProjectCompiler>(co);
  auto b = whole ? r->compiler->buildWhole(r->proj->graph) : r->compiler->build(r->proj->graph);
  REQUIRE_MESSAGE(b.ok(), (b.ok() ? "" : b.error().message));
  r->build = std::move(b.value());
  eng::ProjectConfig pc;
  pc.sampleRate = kSr;
  pc.maxBlockFrames = kBlock;
  pc.numOut = 2;
  for (uint32_t k = 0; k < o.numTracks; ++k) {
    r->notes.push_back(std::make_unique<eng::NoteListSource>(phrase(k, sameNotes)));
    pc.eventSources.push_back(r->notes.back()->source());
  }
  auto prog = eng::ProjectProgram::create(pc, r->build.layout);
  REQUIRE_MESSAGE(prog.ok(), (prog.ok() ? "" : prog.error().message));
  r->program = std::move(prog.value());
  for (uint32_t u = 0; u < r->build.images.size(); ++u) {
    auto pub = r->program->publish(u, std::move(r->build.images[u]));
    REQUIRE_MESSAGE(pub.ok(), (pub.ok() ? "" : pub.error().message));
  }
  return r;
}

struct Capture {
  std::vector<float> out;                    // device channel 0
  std::vector<std::vector<float>> buses;     // per bus, channel 0
};

void render(Rig& r, uint32_t frames, Capture& cap) {
  const auto nb = static_cast<uint32_t>(r.program->layout().busWidth.size());
  cap.buses.resize(nb);
  std::vector<float> l(kBlock), rr(kBlock);
  float* outs[2] = {l.data(), rr.data()};
  while (frames > 0) {
    const uint32_t n = std::min(kBlock, frames);
    eng::AudioBlock ab;
    ab.out = outs;
    ab.numOut = 2;
    ab.frames = n;
    ab.frameTime = r.time;
    r.rtViolations += pq::alloc_trap::countRtViolations([&] {
      pq::DenormalGuard dg;
      r.program->process(ab);
    });
    cap.out.insert(cap.out.end(), l.begin(), l.begin() + n);
    for (uint32_t b = 0; b < nb; ++b) {
      const float* x = r.program->bus(b, 0);
      cap.buses[b].insert(cap.buses[b].end(), x, x + n);
    }
    r.time += n;
    frames -= n;
  }
}

double maxErr(const std::vector<float>& a, const std::vector<float>& b) {
  REQUIRE(a.size() == b.size());
  double e = 0.0;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::isnan(a[i]) || std::isnan(b[i])) return INFINITY;
    e = std::max(e, std::fabs(static_cast<double>(a[i]) - b[i]) / std::max(1.0, std::fabs(static_cast<double>(b[i]))));
  }
  return e;
}

double peak(const std::vector<float>& a) {
  double p = 0.0;
  for (float x : a) p = std::max(p, static_cast<double>(std::fabs(x)));
  return p;
}

uint32_t opsOf(const eng::ProjectCompiler& c) {
  uint32_t n = 0;
  for (uint32_t u = 0; u < c.numUnits(); ++u) n += c.unit(u).ops;
  return n;
}

// The edit "one device constant changed": FmSynth's `level: 0.15` in track k's patch (Patch IR; ids kept).
void editLevel(Project& p, uint32_t track, double level) {
  graph::Patch& tp = *p.trackPatches[track];
  bool done = false;
  for (graph::NodeIndex n = 0; n < tp.nodeCount() && !done; ++n)
    if (tp.node(n).kind == graph::NodeKind::Const && tp.constant(n).literal == 0.15) {
      tp.constant(n) = graph::Constant::real(level);
      done = true;
    }
  REQUIRE(done);
}

}  // namespace

TEST_CASE("project partition: one unit per track in dependency order, buses for cross-unit wires") {
  auto p = project_fixture::makeProject({4, 1, -1, -1});
  REQUIRE(p != nullptr);
  auto part = graph::partitionProject(p->graph);
  REQUIRE_MESSAGE(part.ok(), (part.ok() ? "" : part.error().message));
  const graph::ProjectPartition& P = part.value();
  CHECK(P.units.size() == 8);  // 4 tracks + 2 groups + return + master
  for (uint32_t t = 0; t < 8; ++t) CHECK(P.units[P.unitOf[t]].tracks == std::vector<uint32_t>{t});
  // Dependency order: every bus is produced before it is read.
  for (const graph::ProjectBus& b : P.buses)
    for (uint32_t r : b.readers) CHECK(b.producer < r);
  CHECK(P.unitOf[p->master] == P.units.size() - 1);
  CHECK(P.buses[P.outputBus].output);
  CHECK(P.buses[P.outputBus].producer == P.unitOf[p->master]);
  // 4 × (out, send) + groupA, groupB, return outs; the output bus is the master's out.
  CHECK(P.buses.size() == 4 * 2 + 3 + 1);
  CHECK(P.units[P.unitOf[0]].voiceTrack == 0);
  CHECK(P.units[P.unitOf[p->groupA]].voiceTrack == graph::kNone);
}

TEST_CASE("project partition: sidechain and modulation merge, never two instruments; cycles merge") {
  SUBCASE("sidechain and modulation wires merge their tracks (SPEC §15)") {
    auto p = project_fixture::makeProject({4, 1, 3, 1});
    REQUIRE(p != nullptr);
    auto part = graph::partitionProject(p->graph);
    REQUIRE(part.ok());
    const auto& P = part.value();
    CHECK(P.unitOf[3] == P.unitOf[p->groupB]);
    CHECK(P.unitOf[1] == P.unitOf[p->ret]);
    CHECK(P.units.size() == 6);
    CHECK(P.units[P.unitOf[3]].voiceTrack == 3);
    for (const graph::ProjectBus& b : P.buses)
      for (uint32_t r : b.readers) CHECK(b.producer < r);
  }
  SUBCASE("a modulation wire between two instruments stays a feed-forward bus (one voice table per image)") {
    auto p = project_fixture::makeProject({4, 1, -1, -1});
    REQUIRE(p != nullptr);
    p->graph.wires.push_back({{0, "lfo"}, {1, "vol"}, graph::WireKind::Modulation});
    auto part = graph::partitionProject(p->graph);
    REQUIRE(part.ok());
    CHECK(part.value().unitOf[0] != part.value().unitOf[1]);
    CHECK(part.value().unitOf[0] < part.value().unitOf[1]);
    REQUIRE(part.value().notes.size() == 1);
    CHECK(part.value().notes[0].find("not merged") != std::string::npos);
  }
  SUBCASE("a cycle through two instruments is an error") {
    auto p = project_fixture::makeProject({4, 1, -1, -1});
    REQUIRE(p != nullptr);
    p->graph.wires.push_back({{0, "lfo"}, {1, "vol"}, graph::WireKind::Modulation});
    p->graph.wires.push_back({{1, "lfo"}, {0, "vol"}, graph::WireKind::Modulation});
    auto part = graph::partitionProject(p->graph);
    REQUIRE_FALSE(part.ok());
    CHECK(part.error().code == pq::ErrorCode::Unsupported);
  }
  SUBCASE("an audio cycle between units merges them; the delay-free loop gets a z1 + warning") {
    auto p = project_fixture::makeProject({4, 1, -1, -1});
    REQUIRE(p != nullptr);
    p->graph.wires.push_back({{p->groupA, "out"}, {p->ret, "in"}, graph::WireKind::Audio});
    p->graph.wires.push_back({{p->ret, "out"}, {p->groupA, "in"}, graph::WireKind::Audio});
    auto part = graph::partitionProject(p->graph);
    REQUIRE(part.ok());
    CHECK(part.value().unitOf[p->groupA] == part.value().unitOf[p->ret]);
    CHECK(part.value().units.size() == 7);
    eng::ProjectCompiler c(compileOptions());
    auto b = c.build(p->graph);
    REQUIRE_MESSAGE(b.ok(), (b.ok() ? "" : b.error().message));
    const uint32_t u = c.partition().unitOf[p->groupA];
    CHECK_FALSE(c.unit(u).warnings.empty());
    eng::ProjectCompileOptions strict = compileOptions();
    strict.visualMode = false;
    eng::ProjectCompiler c2(strict);
    CHECK_FALSE(c2.build(p->graph).ok());  // text mode: a delay-free cycle is an error
  }
  SUBCASE("invalid descriptions") {
    auto p = project_fixture::makeProject({2, 1, -1, -1});
    REQUIRE(p != nullptr);
    graph::ProjectGraph g = p->graph;
    g.wires.push_back({{0, "nope"}, {1, "vol"}, graph::WireKind::Audio});
    CHECK(graph::partitionProject(g).error().code == pq::ErrorCode::NotFound);
    g = p->graph;
    g.tracks[1].id = g.tracks[0].id;
    CHECK_FALSE(graph::partitionProject(g).ok());
    g = p->graph;
    g.output = {0, "vol"};  // a param is not an output
    CHECK_FALSE(graph::partitionProject(g).ok());
  }
}

TEST_CASE("project composition: a track's state keys do not depend on its unit") {
  auto p = project_fixture::makeProject({3, 2, 2, -1});
  REQUIRE(p != nullptr);
  eng::ProjectCompiler perUnit(compileOptions()), whole(compileOptions());
  REQUIRE(perUnit.build(p->graph).ok());
  REQUIRE(whole.buildWhole(p->graph).ok());
  CHECK(whole.numUnits() == 1);
  CHECK(perUnit.numUnits() == 6);  // track 2 merged with group B
  // Every state entry of every unit exists, with the same key and tag, in the whole-project program.
  const graph::StateLayout& all = whole.unit(0).schedule->state;
  uint32_t entries = 0;
  for (uint32_t u = 0; u < perUnit.numUnits(); ++u)
    for (const graph::StateEntry& e : perUnit.unit(u).schedule->state.entries) {
      const graph::StateEntry* w = all.find(e.key, e.channel, e.arena);
      REQUIRE(w != nullptr);
      CHECK(w->tag == e.tag);
      CHECK(w->size == e.size);
      ++entries;
    }
  CHECK(entries == all.entries.size());
  // Flattened names carry the track id.
  const graph::Patch& tp = perUnit.unit(perUnit.partition().unitOf[0]).typed->patch;
  bool found = false;
  for (graph::NodeIndex n = 0; n < tp.nodeCount(); ++n) found = found || tp.nodeName(n) == "t1__vol";
  CHECK(found);
}

TEST_CASE("per-unit project output == the whole-project program of the same graph (≤ 1e-6)") {
  const Options o{6, 4, 4, 1};  // sidechain (track 4 → group B) and modulation (track 1 → return) merges
  auto units = makeRig(o, false, true);
  auto whole = makeRig(o, true, true);
  CHECK(units->compiler->numUnits() == 8);  // 6 tracks + 4 buses − 2 merges
  CHECK(whole->compiler->numUnits() == 1);
  CHECK(opsOf(*units->compiler) > 0);
  Capture a, b;
  render(*units, 12000, a);
  render(*whole, 12000, b);
  CHECK(units->rtViolations == 0);
  CHECK(whole->rtViolations == 0);
  CHECK(peak(b.out) > 0.01);  // not silence
  const double err = maxErr(a.out, b.out);
  INFO("max |per-unit − whole| = " << err);
  CHECK(err <= 1e-6);
}

TEST_CASE("a one-track edit recompiles and crossfades only its unit; every other unit is bit-identical") {
  const Options o{6, 3, -1, -1};
  auto ref = makeRig(o, false, false);
  auto edited = makeRig(o, false, false);
  const uint32_t track = 2;
  const uint32_t unit = edited->compiler->partition().unitOf[track];
  Capture a, b;
  render(*ref, 4000, a);
  render(*edited, 4000, b);  // notes are sounding: the swap must crossfade
  editLevel(*edited->proj, track, 0.6);
  const uint32_t tracks[] = {track};
  auto upd = edited->compiler->update(edited->proj->graph, tracks);
  REQUIRE_MESSAGE(upd.ok(), (upd.ok() ? "" : upd.error().message));
  REQUIRE(upd.value().size() == 1);
  CHECK(upd.value()[0].unit == unit);
  REQUIRE_FALSE(upd.value()[0].unchanged);
  auto pub = edited->program->publish(unit, std::move(upd.value()[0].image));
  REQUIRE(pub.ok());
  CHECK(pub.value().plan.fresh == 0);       // a constant edit keeps every state entry
  CHECK(pub.value().plan.matched > 0);
  render(*ref, 6000, a);
  render(*edited, 6000, b);
  CHECK(edited->rtViolations == 0);
  for (uint32_t u = 0; u < edited->program->numUnits(); ++u) {
    const eng::UnitStats& st = edited->program->unitStats(u);
    CHECK(st.swaps == (u == unit ? 2u : 1u));
    CHECK(st.crossfades == (u == unit ? 1u : 0u));
  }
  CHECK(edited->program->stats().maxFadingUnits == 1);
  // Buses of units other than the edited one and its downstream (groups, return, master) are bit-identical.
  const graph::ProjectPartition& P = edited->compiler->partition();
  uint32_t identical = 0;
  for (uint32_t bb = 0; bb < P.buses.size(); ++bb) {
    const uint32_t prod = P.buses[bb].producer;
    const bool downstream = prod == unit || P.units[prod].voiceTrack == graph::kNone;
    if (downstream) continue;
    CHECK(std::memcmp(a.buses[bb].data(), b.buses[bb].data(), sizeof(float) * a.buses[bb].size()) == 0);
    ++identical;
  }
  CHECK(identical == 2 * (o.numTracks - 1));
  // The edited track's own bus changed (louder) after the edit; identical before it.
  uint32_t outBus = graph::kNone;
  for (uint32_t bb = 0; bb < P.buses.size(); ++bb)
    if (P.buses[bb].source.track == track && P.buses[bb].source.port == "out") outBus = bb;
  REQUIRE(outBus != graph::kNone);
  CHECK(std::memcmp(a.buses[outBus].data(), b.buses[outBus].data(), sizeof(float) * 4000) == 0);
  std::vector<float> ta(a.buses[outBus].begin() + 4000 + 960, a.buses[outBus].end());
  std::vector<float> tb(b.buses[outBus].begin() + 4000 + 960, b.buses[outBus].end());
  CHECK(peak(tb) > 2.0 * peak(ta));
  // An update whose compiled unit is unchanged reports it and publishes nothing.
  auto again = edited->compiler->update(edited->proj->graph, tracks);
  REQUIRE(again.ok());
  REQUIRE(again.value().size() == 1);
  CHECK(again.value()[0].unchanged);
  // Retired images are freed off the audio thread.
  edited->program->collect();
  render(*edited, 64, b);
  edited->program->collect();
  CHECK(edited->program->imagesFreed() >= 1);
}

TEST_CASE("cross-track modulation merges and is sample-accurate; a feed-forward bus adds no latency") {
  const char* text = R"(
patch Src {
  out out : audio global mono
  export m = phasor(97Hz)
  out = sine(phasor(440Hz)) * 0.1
}
patch Dst {
  in    x : audio global mono = 0
  param depth = 1.0 [0..1]
  out   out : audio global mono
  out = x + depth
}
patch Sum {
  in  in : audio global mono
  out out : audio global mono
  out = in * 1
}
patch Ref {
  out out : audio global mono
  out = phasor(97Hz)
}
)";
  auto parsed = graph::parse(text, "mod.pqp");
  REQUIRE(parsed.ok());
  const graph::PatchFile& f = parsed.value();
  // Reference: the phasor alone, interpreted.
  std::vector<float> want;
  {
    graph::InferOptions io;
    auto inf = graph::infer(*f.find("Ref"), io);
    REQUIRE(inf.ok());
    graph::OptimiseOptions oo;
    oo.sampleRate = kSr;
    auto opt = graph::optimise(std::move(inf.value()), oo);
    REQUIRE(opt.ok());
    graph::ScheduleOptions so;
    so.sampleRate = kSr;
    auto s = graph::schedule(opt.value(), so);
    REQUIRE(s.ok());
    eng::InterpretOptions iopt;
    iopt.maxBlockFrames = kBlock;
    auto img = eng::buildInterpreted(s.value(), iopt);
    REQUIRE(img.ok());
    std::vector<float> buf(kBlock);
    float* outs[1] = {buf.data()};
    for (uint32_t t = 0; t < 4096; t += kBlock) {
      eng::ProcessContext ctx;
      ctx.frames = kBlock;
      ctx.sampleRate = kSr;
      ctx.out = outs;
      ctx.numOut = 1;
      pq::DenormalGuard dg;
      img.value()->process(ctx);
      want.insert(want.end(), buf.begin(), buf.end());
    }
  }
  for (const graph::WireKind kind : {graph::WireKind::Modulation, graph::WireKind::Audio}) {
    CAPTURE(static_cast<int>(kind));
    graph::ProjectGraph g;
    g.tracks.push_back({1, "Src", f.find("Src"), graph::TrackRole::Audio, graph::kNone});
    g.tracks.push_back({2, "Dst", f.find("Dst"), graph::TrackRole::Audio, graph::kNone});
    g.tracks.push_back({3, "Sum", f.find("Sum"), graph::TrackRole::Master, graph::kNone});
    if (kind == graph::WireKind::Modulation)
      g.wires.push_back({{0, "m"}, {1, "depth"}, kind});  // the LFO replaces Dst's depth param
    else
      g.wires.push_back({{0, "m"}, {1, "x"}, kind});      // Dst's input; depth: its default 1 → out = m + 1
    g.wires.push_back({{1, "out"}, {2, "in"}, graph::WireKind::Audio});
    g.output = {2, "out"};
    eng::ProjectCompiler c(compileOptions());
    auto b = c.build(g);
    REQUIRE_MESSAGE(b.ok(), (b.ok() ? "" : b.error().message));
    if (kind == graph::WireKind::Modulation) {
      CHECK(c.partition().unitOf[0] == c.partition().unitOf[1]);  // merged
      CHECK(c.numUnits() == 2);
    } else {
      CHECK(c.partition().unitOf[0] != c.partition().unitOf[1]);  // a bus between Src and Dst
      CHECK(c.numUnits() == 3);
    }
    eng::ProjectConfig pc;
    pc.sampleRate = kSr;
    pc.maxBlockFrames = kBlock;
    pc.numOut = 1;
    auto prog = eng::ProjectProgram::create(pc, b.value().layout);
    REQUIRE(prog.ok());
    for (uint32_t u = 0; u < b.value().images.size(); ++u) REQUIRE(prog.value()->publish(u, std::move(b.value().images[u])).ok());
    std::vector<float> got, buf(kBlock);
    float* outs[1] = {buf.data()};
    for (uint32_t t = 0; t < 4096; t += kBlock) {
      eng::AudioBlock ab;
      ab.out = outs;
      ab.numOut = 1;
      ab.frames = kBlock;
      ab.frameTime = t;
      CHECK(pq::alloc_trap::countRtViolations([&] {
              pq::DenormalGuard dg;
              prog.value()->process(ab);
            }) == 0);
      got.insert(got.end(), buf.begin(), buf.end());
    }
    // Sample for sample: out[n] = phasor[n] (+ 1 for the bus case), no block of latency.
    uint32_t bad = 0;
    for (size_t n = 0; n < want.size(); ++n) {
      const float expect = kind == graph::WireKind::Modulation ? want[n] : want[n] + 1.0f;
      if (got[n] != expect) ++bad;
    }
    CHECK(bad == 0);
  }
}

TEST_CASE("project program: publish validates bindings; a unit without an image reads silence") {
  auto proj = project_fixture::makeProject({2, 1, -1, -1});
  REQUIRE(proj != nullptr);
  eng::ProjectCompiler c(compileOptions());
  auto b = c.build(proj->graph);
  REQUIRE(b.ok());
  eng::ProjectConfig pc;
  pc.sampleRate = kSr;
  pc.maxBlockFrames = kBlock;
  std::vector<std::unique_ptr<eng::NoteListSource>> notes;
  for (uint32_t k = 0; k < 2; ++k) {
    notes.push_back(std::make_unique<eng::NoteListSource>(phrase(k, false)));
    pc.eventSources.push_back(notes.back()->source());
  }
  auto prog = eng::ProjectProgram::create(pc, b.value().layout);
  REQUIRE(prog.ok());
  eng::ProjectProgram& P = *prog.value();
  auto& imgs = b.value().images;
  const uint32_t masterUnit = c.partition().unitOf[proj->master];
  // An input bound to a bus of a later unit (the master's output) is refused.
  CHECK_FALSE(c.bind(0, nullptr).ok());
  const uint32_t groupAUnit = c.partition().unitOf[proj->groupA];
  REQUIRE_FALSE(imgs[groupAUnit].in.empty());
  imgs[groupAUnit].in[0] = eng::BusRef{c.partition().outputBus, 0};
  auto e1 = P.publish(groupAUnit, std::move(imgs[groupAUnit]));
  CHECK_FALSE(e1.ok());
  // Out-of-range bindings and a missing output channel.
  CHECK_FALSE(P.publish(99, eng::UnitImage{}).ok());
  CHECK_FALSE(P.publish(0, eng::UnitImage{}).ok());
  {
    eng::UnitImage& ui = imgs[1];
    for (eng::BusRef& br : ui.out) br.bus = eng::kNoBus;  // leaves its buses unwritten
    CHECK_FALSE(P.publish(1, std::move(ui)).ok());
  }
  // Only the master published: it reads silence from units without images.
  REQUIRE(P.publish(masterUnit, std::move(imgs[masterUnit])).ok());
  std::vector<float> l(kBlock), rr(kBlock);
  float* outs[2] = {l.data(), rr.data()};
  eng::AudioBlock ab;
  ab.out = outs;
  ab.numOut = 2;
  ab.frames = kBlock;
  {
    pq::DenormalGuard dg;
    P.process(ab);
  }
  CHECK(peak(l) == 0.0);
  CHECK(P.current(masterUnit) != nullptr);
  CHECK(P.current(0) == nullptr);
}

TEST_CASE("30-track fixture: about 20k ops; an edit recompiles one unit of about 1/30 of them") {
  auto r = makeRig({30, 28, 3, 5}, false, false);
  const uint32_t total = opsOf(*r->compiler);
  MESSAGE("30-track fixture: " << r->compiler->numUnits() << " units, " << total << " schedule ops");
  CHECK(total >= 15000);
  CHECK(total <= 30000);
  Capture cap;
  render(*r, 2048, cap);
  CHECK(r->rtViolations == 0);
  editLevel(*r->proj, 10, 0.3);
  const uint32_t tracks[] = {10};
  auto upd = r->compiler->update(r->proj->graph, tracks);
  REQUIRE(upd.ok());
  REQUIRE(upd.value().size() == 1);
  const uint32_t u = upd.value()[0].unit;
  CHECK(r->compiler->unit(u).ops * 20 < total);
  REQUIRE(r->program->publish(u, std::move(upd.value()[0].image)).ok());
  render(*r, 2048, cap);
  CHECK(r->rtViolations == 0);
  CHECK(r->program->unitStats(u).crossfades == 1);
  CHECK(r->program->stats().maxFadingUnits == 1);
}
