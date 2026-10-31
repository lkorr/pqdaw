// T4.x-fix-safety (M4 review H2, L4): what an image writes on the audio thread lives as long as the image.
//   H2  a unit's cost meter is held by every image bound to it (UnitImage::keepAlive), not freed 10 s after its schedule
//       stopped being the compiler's: a held edit (interpretBudget) keeps the old image playing for as long as its JIT
//       takes, writing that meter.
//   L4  a probe tap slot is reused only once no image bound to it is alive, not after 1 s.
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "playback_fixture.h"
#include "pq/app/device_player.h"
#include "pq/app/project_link.h"
#include "pq/core/alloc_trap.h"
#include "pq/document/edit_ops.h"
#include "pq/engine/inspect.h"
#include "pq/engine/null_device.h"

using namespace pq;
using namespace pq::doc;
using pq::app::LinkConfig;
using pq::app::ProjectLink;

namespace {

// Holds JIT builds back while closed (the simulated slow compile). Starts open.
struct Gate {
  std::mutex m;
  std::condition_variable cv;
  bool open = true;
  int waiting = 0;
  static void wait(void* self) {
    Gate& g = *static_cast<Gate*>(self);
    std::unique_lock lock(g.m);
    ++g.waiting;
    g.cv.notify_all();
    g.cv.wait(lock, [&] { return g.open; });
    --g.waiting;
  }
  void set(bool o) {
    {
      std::lock_guard lock(m);
      open = o;
    }
    cv.notify_all();
  }
  bool waitBlocked(double seconds) {
    std::unique_lock lock(m);
    return cv.wait_for(lock, std::chrono::duration<double>(seconds), [&] { return waiting >= 1; });
  }
};

std::unique_ptr<ProjectLink> makeLink(const Snapshot& s, Gate* gate, float budget) {
  LinkConfig c;
  c.compileThread = false;
  c.sleepAfterMs = 0.0f;
  c.jit = true;
  c.interpretBudget = budget;
  if (gate != nullptr) {
    c.jitBeforeCompile = &Gate::wait;
    c.jitUser = gate;
  }
  auto l = ProjectLink::create(c, s);
  REQUIRE_MESSAGE(l.ok(), (l.ok() ? std::string() : l.error().message));
  return std::move(l.value());
}

struct Player {
  engine::NullDevice dev;
  explicit Player(ProjectLink& l) {
    engine::StreamConfig sc;
    sc.sampleRate = 48000;
    sc.bufferFrames = 256;
    sc.outputChannels = 2;
    REQUIRE(dev.open(sc, l.callback()).ok());
  }
  ~Player() { dev.close(); }
  void run(uint32_t blocks) {
    for (uint32_t b = 0; b < blocks; ++b) dev.processBlock();
  }
};

uint32_t unitsOn(ProjectLink& l, engine::ImageBackend b) {
  engine::ProjectProgram* p = l.engine().program();
  uint32_t n = 0;
  for (uint32_t u = 0; u < p->numUnits(); ++u) n += p->current(u) != nullptr && p->current(u)->io().backend == b;
  return n;
}

// Every unit plays the last image published to it (nothing newer waits) and the JIT is idle; false after ~60 s.
bool settle(ProjectLink& l, Player& p) {
  for (int i = 0; i < 200; ++i) {
    const bool idle = l.waitForJit(0.3);
    p.run(8);
    engine::ProjectProgram& prog = *l.engine().program();
    bool current = !prog.topologyPending();
    for (uint32_t u = 0; u < prog.numUnits(); ++u) current = current && prog.current(u) == prog.lastPublished(u);
    if (idle && current) return true;
  }
  return false;
}

uint32_t unitNamed(ProjectLink& l, const std::string& name) {
  const engine::ProjectProgram& p = *l.engine().program();
  for (uint32_t u = 0; u < p.numUnits(); ++u)
    if (p.layout().units[u].name == name) return u;
  FAIL("no unit " << name);
  return 0;
}

// Poisoned (freed under PoisonFrees) memory reads 0xDD…; a live CostMeter has its constructor's maxRegions.
bool meterAlive(const engine::CostMeter* m) { return m != nullptr && m->maxRegions() == 4096u; }

}  // namespace

// H2: cost labels on, an edit held for its JIT image for 11 s. The meter the old image writes every block must stay
// alive the whole time (the old code freed it 10 s after its schedule stopped being the compiler's, on the next cost
// poll, while the image still ran and wrote it).
TEST_CASE("H2: a held edit's old image keeps its cost meter alive past 10 s (PoisonFrees)" * doctest::test_suite("slow")) {
  REQUIRE(alloc_trap::installed());
  playback_fixture::Fixture f;
  Gate gate;
  auto link = makeLink(f.snap(), &gate, 1e-9f);
  [[maybe_unused]] const ListenerId sub = f.store.subscribe([&](const StoreEvent& e) { link->onDocumentChange(e); });
  link->setCostMetering(true);
  Player p(*link);
  link->play(0);
  REQUIRE(settle(*link, p));
  engine::ProjectProgram& prog = *link->engine().program();
  REQUIRE(unitsOn(*link, engine::ImageBackend::Jit) == prog.numUnits());
  std::vector<DeviceId> ids;
  forEachDevice(*f.snap(), [&](const Device& d, const Track&) { ids.push_back(d.id); });
  std::vector<float> ns(ids.size());
  const uint32_t lead = unitNamed(*link, "Lead");
  const engine::CostMeter* meter = prog.current(lead)->boundMeter();
  REQUIRE(meterAlive(meter));

  gate.set(false);
  const uint64_t held0 = link->stats().heldForJit;
  REQUIRE(f.store.dispatch(AddDevice{f.lead, makeDevice(f.store.ids().device(), "utility", DeviceKind::AudioEffect), kAppend}).ok());
  link->flush();
  REQUIRE(link->stats().heldForJit == held0 + 1);
  REQUIRE(gate.waitBlocked(30.0));  // the edit's JIT build waits at the gate
  link->readDeviceCosts(ids, ns);   // the old code noted the old schedule unused here …
  const auto t0 = std::chrono::steady_clock::now();
  auto elapsed = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
  while (elapsed() < 10.5) {
    p.run(4);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  {
    alloc_trap::PoisonFrees poison;
    while (elapsed() < 11.0) {
      p.run(4);
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    link->readDeviceCosts(ids, ns);  // … and freed its meter here, 10 s later
    link->collect();
    p.run(40);  // the old image runs on, writing its meter
    CHECK(prog.current(lead)->boundMeter() == meter);
    CHECK(meterAlive(prog.current(lead)->boundMeter()));
    CHECK(prog.current(lead)->io().backend == engine::ImageBackend::Jit);  // still the pre-edit JIT image
  }
  gate.set(true);
  REQUIRE(settle(*link, p));
  CHECK(unitsOn(*link, engine::ImageBackend::Jit) == prog.numUnits());
  // The labels still read after the edit's image took over.
  link->readDeviceCosts(ids, ns);
  p.run(40);
  link->readDeviceCosts(ids, ns);
  CHECK(std::any_of(ns.begin(), ns.end(), [](float v) { return v > 0.0f; }));
}

// L4: under a held edit, probe tap slots freed by a re-inspection are not handed to new probes while the old image
// (which still writes them) plays. The old code reused them after 1 s: the new probes' rings then received the old
// image's data although nothing played their program.
TEST_CASE("L4: a probe ring is not reused while an old image bound to it plays") {
  playback_fixture::Fixture f;
  Gate gate;
  auto link = makeLink(f.snap(), &gate, 1e-9f);
  [[maybe_unused]] const ListenerId sub = f.store.subscribe([&](const StoreEvent& e) { link->onDocumentChange(e); });
  Player p(*link);
  link->play(0);
  REQUIRE(settle(*link, p));
  const DeviceId synth = findTrackPtr(*f.snap(), f.lead)->devices.front()->id;
  const graph::Patch* bs = link->devices().stockFile("BasicSynth")->find("BasicSynth");
  REQUIRE(bs != nullptr);
  const auto env = bs->findByName("env");
  REQUIRE(env.has_value());
  graph::ProbeTarget t;
  t.node = bs->node(*env).id;
  // Every ring slot (DeviceInspector::kProbeRings) in use: probes 1 … 8 on the envelope.
  constexpr uint32_t kRings = app::DeviceInspector::kProbeRings;
  auto probes = [&](app::DevicePlayer& pl, uint32_t base) {
    std::vector<graph::ProbeTarget> targets(kRings, t);
    std::vector<uint32_t> ids;
    for (uint32_t i = 0; i < kRings; ++i) ids.push_back(base + i);
    pl.setProbeSignals(ids);
    pl.setProbeTargets(targets, ids);
    link->flush();
    return ids;
  };
  app::DevicePlayer player(*link, synth);
  const std::vector<uint32_t> first = probes(player, 1);
  REQUIRE(settle(*link, p));  // the probed unit on its JIT image, writing rings 1 … 8
  p.run(20);
  for (const uint32_t id : first) {
    REQUIRE(player.probeSignal(id) != nullptr);
    CHECK(player.probeSignal(id)->written() > 0);
  }
  // Two re-inspections, both held (their JIT builds wait at the gate), the second 1.2 s after the first: the old
  // image with probes 1 … 8 plays on throughout.
  gate.set(false);
  (void)probes(player, 11);
  REQUIRE(gate.waitBlocked(30.0));
  const auto t0 = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(1200)) {
    p.run(2);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  const std::vector<uint32_t> third = probes(player, 21);
  CHECK(link->stats().heldForJit >= 2);
  std::vector<uint64_t> before;
  for (const uint32_t id : third) before.push_back(player.probeSignal(id) ? player.probeSignal(id)->written() : 0);
  p.run(40);
  for (size_t i = 0; i < third.size(); ++i) {
    TapRing* ring = player.probeSignal(third[i]);
    // Nothing plays the third program yet: a ring bound to one of its probes must not be written.
    if (ring != nullptr) CHECK_MESSAGE(ring->written() == before[i], "probe " << third[i] << " written by an old image");
  }
  gate.set(true);
  REQUIRE(settle(*link, p));
  p.run(20);
  // Once the third program plays, its probes have rings and data.
  for (const uint32_t id : third) {
    TapRing* ring = player.probeSignal(id);
    REQUIRE(ring != nullptr);
    CHECK(ring->written() > 0);
  }
}
