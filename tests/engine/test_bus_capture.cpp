// EX.1: Engine::renderOffline's bus capture (pq/engine/offline_render.h BusCapture, ProjectProgram::setCapture): the
// captured buses are the program's bus blocks at every frame (a device block of 100 frames over 64-frame image blocks,
// chunks of 1000 frames), the main output is bit-identical to a render without capture, the loop allocates nothing
// (the RtScope trap), and bad requests are refused.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "pq/engine/engine.h"
#include "pq/engine/offline_render.h"
#include "pq/engine/project_compile.h"
#include "pq/engine/project_program.h"
#include "pq/graph/parse.h"

namespace graph = pq::graph;
namespace e = pq::engine;

namespace {

constexpr double kSr = 48000.0;

const char* kPatches = R"(
patch Tone {
  out out : audio global mono
  out = sine(phasor(441))
}

patch Half {
  in  in : audio global mono
  out out : audio global mono
  out = in * 0.5
}
)";

const graph::Patch* patch(const std::string& name) {
  static const graph::PatchFile f = [] {
    auto r = graph::parse(kPatches, "bus_capture.pqp");
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
    return std::move(r.value());
  }();
  for (const graph::Patch& p : f.patches)
    if (p.name() == name) return &p;
  FAIL("no patch " << name);
  return nullptr;
}

// Tone → Half (the master); the engine runs it.
struct Rig {
  std::unique_ptr<e::Engine> en;
  uint32_t toneBus = e::kNoBus, masterBus = e::kNoBus;
};

Rig makeRig() {
  graph::ProjectGraph g;
  g.tracks.push_back({1, "Tone", patch("Tone"), graph::TrackRole::Audio, graph::kNone});
  g.tracks.push_back({2, "Half", patch("Half"), graph::TrackRole::Master, graph::kNone});
  g.wires.push_back({{0, "out"}, {1, "in"}, graph::WireKind::Audio});
  g.output = {1, "out"};
  Rig r;
  e::EngineConfig ec;
  ec.sampleRate = kSr;
  ec.blockFrames = 64;
  ec.numOut = 2;
  ec.maxSlots = 2;
  ec.sleepAfterMs = 0.0f;
  auto en = e::Engine::create(ec);
  REQUIRE(en.ok());
  r.en = std::move(en.value());
  e::ProjectCompileOptions o;
  o.sampleRate = kSr;
  o.maxBlockFrames = 64;
  e::ProjectCompiler compiler(o);
  auto b = compiler.build(g);
  REQUIRE_MESSAGE(b.ok(), (b.ok() ? "" : b.error().message));
  auto prog = e::ProjectProgram::create(r.en->projectConfig(), b.value().layout);
  REQUIRE(prog.ok());
  for (uint32_t u = 0; u < b.value().images.size(); ++u) REQUIRE(prog.value()->publish(u, std::move(b.value().images[u])).ok());
  const e::ProjectLayout& L = prog.value()->layout();
  REQUIRE(L.busWidth.size() == 2);
  r.masterBus = L.outputBus;
  r.toneBus = 1 - L.outputBus;
  r.en->setProgram(std::move(prog.value()));
  return r;
}

}  // namespace

TEST_CASE("EX.1 bus capture: every frame of the captured buses, the main output untouched, RtScope-clean") {
  constexpr uint64_t kFrames = 4321;  // not a multiple of the chunk or the device block
  Rig plain = makeRig();
  e::BufferSink reference;
  REQUIRE(plain.en->renderOffline(kFrames, reference, 100, 1000));

  Rig rig = makeRig();
  e::BufferSink main, tone, master;
  e::BusCapture cap;
  cap.buses = {rig.toneBus, rig.masterBus};
  cap.sinks = {&tone, &master};
  // The per-block loop runs under an RtScope: an allocation in the capture fails this test case (the trap).
  auto st = rig.en->renderOffline(kFrames, main, 100, 1000, &cap);
  REQUIRE_MESSAGE(st.ok(), (st.ok() ? "" : st.error().message));
  REQUIRE(main.channels.size() == 2);
  CHECK(main.channels == reference.channels);  // capture changes nothing it does not copy
  REQUIRE(tone.channels.size() == 1);
  REQUIRE(master.channels.size() == 1);
  REQUIRE(tone.channels[0].size() == kFrames);
  REQUIRE(master.channels[0].size() == kFrames);
  CHECK(tone.sampleRate == 48000u);
  CHECK(master.channels[0] == main.channels[0]);  // the output bus is what the device plays
  float peak = 0.0f;
  size_t mismatches = 0;
  for (uint64_t i = 0; i < kFrames; ++i) {
    peak = std::max(peak, std::fabs(tone.channels[0][i]));
    mismatches += tone.channels[0][i] * 0.5f == master.channels[0][i] ? 0u : 1u;
  }
  CHECK(peak > 0.9f);
  CHECK(mismatches == 0);

  // The capture ended with the render: the next render without one copies nothing.
  e::BufferSink after;
  REQUIRE(rig.en->renderOffline(256, after, 64, 256));
  CHECK(tone.channels[0].size() == kFrames);
}

TEST_CASE("EX.1 bus capture: a bus outside the layout, a missing sink and mismatched lists are refused") {
  Rig rig = makeRig();
  e::BufferSink main, s;
  e::BusCapture cap;
  cap.buses = {7};
  cap.sinks = {&s};
  CHECK_FALSE(rig.en->renderOffline(64, main, 64, 64, &cap).ok());
  cap.buses = {rig.toneBus};
  cap.sinks = {nullptr};
  CHECK_FALSE(rig.en->renderOffline(64, main, 64, 64, &cap).ok());
  cap.sinks = {&s, &s};
  CHECK_FALSE(rig.en->renderOffline(64, main, 64, 64, &cap).ok());
}
