// T1b.5 in the DAW: ProjectLink plays a project on interpreted units at once and swaps each unit to its JIT image when
// the background compile (one below-normal worker) delivers it. Played through a NullDevice (the device callback
// path) against a link that never uses the JIT: the output must be identical sample for sample, before, through and
// after the swaps — with the compile held back (a simulated slow compile) while the first second plays, and with an
// edit arriving while JIT images are still pending.
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "playback_fixture.h"
#include "pq/app/project_link.h"
#include "pq/compile/compile.h"
#include "pq/document/edit_ops.h"
#include "pq/document/lower.h"
#include "pq/engine/null_device.h"

using namespace pq;
using namespace pq::doc;
using pq::app::LinkConfig;
using pq::app::ProjectLink;

namespace {

struct Gate {  // the simulated compile delay: JIT builds wait here until the test opens it
  std::mutex m;
  std::condition_variable cv;
  bool open = false;
  int waiting = 0;
  static void wait(void* self) {
    Gate& g = *static_cast<Gate*>(self);
    std::unique_lock lock(g.m);
    ++g.waiting;
    g.cv.notify_all();
    g.cv.wait(lock, [&] { return g.open; });
  }
  void release() {
    {
      std::lock_guard lock(m);
      open = true;
    }
    cv.notify_all();
  }
};

std::unique_ptr<ProjectLink> link(const Snapshot& s, bool jit, Gate* gate) {
  LinkConfig c;
  c.compileThread = false;
  c.sleepAfterMs = 0.0f;
  c.jit = jit;
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
  std::vector<float> out;
  explicit Player(ProjectLink& l) {
    engine::StreamConfig sc;
    sc.sampleRate = 48000;
    sc.bufferFrames = 256;
    sc.outputChannels = 2;
    REQUIRE(dev.open(sc, l.callback()).ok());
  }
  ~Player() { dev.close(); }
  void run(uint32_t blocks) {
    for (uint32_t b = 0; b < blocks; ++b) {
      dev.processBlock();
      for (uint32_t c = 0; c < 2; ++c) out.insert(out.end(), dev.output(c), dev.output(c) + 256);
    }
  }
};

uint32_t unitsOn(ProjectLink& l, engine::ImageBackend b) {
  engine::ProjectProgram* p = l.engine().program();
  uint32_t n = 0;
  for (uint32_t u = 0; u < p->numUnits(); ++u) n += p->current(u) != nullptr && p->current(u)->io().backend == b;
  return n;
}

size_t firstDifference(const std::vector<float>& a, const std::vector<float>& b) {
  REQUIRE(a.size() == b.size());
  for (size_t i = 0; i < a.size(); ++i)
    if (std::memcmp(&a[i], &b[i], sizeof(float)) != 0) return i;
  return a.size();
}

}  // namespace

TEST_CASE("T1b.5: live playback (NullDevice) starts interpreted and swaps every unit to its background JIT image, sample-exact") {
  playback_fixture::Fixture fj, fi;
  Gate gate;
  auto lj = link(fj.snap(), true, &gate), li = link(fi.snap(), false, nullptr);
  [[maybe_unused]] const ListenerId sj = fj.store.subscribe([&](const StoreEvent& e) { lj->onDocumentChange(e); });
  [[maybe_unused]] const ListenerId si = fi.store.subscribe([&](const StoreEvent& e) { li->onDocumentChange(e); });
  const uint32_t units = lj->engine().program()->numUnits();
  REQUIRE(units >= 5);
  Player pj(*lj), pi(*li);
  lj->play(0);
  li->play(0);
  pj.run(188);  // 1 s: every unit plays interpreted while the JIT is held back
  pi.run(188);
  {
    std::unique_lock lock(gate.m);
    REQUIRE(gate.cv.wait_for(lock, std::chrono::seconds(30), [&] { return gate.waiting >= 1; }));
  }
  CHECK(lj->jitStats().wanted == units);
  CHECK(lj->jitStats().published == 0);
  CHECK(unitsOn(*lj, engine::ImageBackend::Interpreter) == units);
  // A structural edit meanwhile (a new track: a topology change compiling 3 units, interpreted, audible at once): the
  // JIT wants are renewed for the new schedules; the in-flight one may come back stale.
  REQUIRE(fj.store.dispatch(AddTrack{playback_fixture::track(fj.store.ids().track(), TrackKind::Midi, "New", fj.store.ids().device()), {}, kAppend}).ok());
  REQUIRE(fi.store.dispatch(AddTrack{playback_fixture::track(fi.store.ids().track(), TrackKind::Midi, "New", fi.store.ids().device()), {}, kAppend}).ok());
  lj->flush();
  li->flush();
  pj.run(94);
  pi.run(94);
  const uint32_t unitsAfter = lj->engine().program()->numUnits();
  CHECK(unitsAfter == units + 1);
  gate.release();
  REQUIRE(lj->waitForJit(120.0));
  const app::JitStats js = lj->jitStats();
  CHECK(js.errors == 0);
  CHECK(js.published == unitsAfter);
  CHECK(js.built == js.published + js.stale);
  pj.run(376);  // 2 s more: the JIT images become current at the first of these device blocks
  pi.run(376);
  CHECK(unitsOn(*lj, engine::ImageBackend::Jit) == unitsAfter);
  CHECK(unitsOn(*li, engine::ImageBackend::Interpreter) == unitsAfter);
  const size_t d = firstDifference(pj.out, pi.out);
  CHECK_MESSAGE(d == pj.out.size(), "first difference at device frame " << d / 512 * 256 + d % 256);
  float peak = 0.0f;
  for (float v : pj.out) peak = std::max(peak, std::fabs(v));
  CHECK(peak > 0.01f);
  MESSAGE("JIT: " << js.built << " images in " << lj->jitStats().totalCompileMs << " ms (background), " << js.stale
                  << " stale");
}

// T1b.x-perf (the T4.12-fix1 follow-up): a unit whose interpreted image would cost more than LinkConfig::
// interpretBudget keeps playing its JIT image through a structural edit until the edit's JIT image arrives; it never
// runs interpreted (here every unit counts as heavy: a budget of ~0). With the default budget this light project
// holds nothing. (Q111 option C rides along: ProjectLink's default backend builds quick first images, refined later.)
TEST_CASE("T1b.x-perf: units above the interpreter budget keep their JIT image through an edit, never run interpreted") {
  for (const float budget : {1e-9f, 0.25f}) {
    CAPTURE(budget);
    playback_fixture::Fixture f;
    LinkConfig c;
    c.compileThread = false;
    c.sleepAfterMs = 0.0f;
    c.jit = true;
    c.interpretBudget = budget;
    auto made = ProjectLink::create(c, f.snap());
    REQUIRE(made.ok());
    ProjectLink& l = *made.value();
    [[maybe_unused]] const ListenerId sub = f.store.subscribe([&](const StoreEvent& e) { l.onDocumentChange(e); });
    Player p(l);
    l.play(0);
    p.run(10);
    REQUIRE(l.waitForJit(120.0));
    p.run(20);
    const uint32_t units = l.engine().program()->numUnits();
    REQUIRE(unitsOn(l, engine::ImageBackend::Jit) == units);
    // A structural edit of one track (a unit update, not a topology change).
    REQUIRE(f.store.dispatch(AddDevice{f.lead, makeDevice(f.store.ids().device(), "utility", DeviceKind::AudioEffect), kAppend}).ok());
    l.flush();
    uint32_t interpretedBlocks = 0;
    for (int b = 0; b < 400; ++b) {
      p.run(1);
      interpretedBlocks += unitsOn(l, engine::ImageBackend::Interpreter) > 0 ? 1u : 0u;
      if (b == 200) REQUIRE(l.waitForJit(120.0));
    }
    CHECK(unitsOn(l, engine::ImageBackend::Jit) == units);
    const app::JitStats js = l.jitStats();
    CHECK(js.errors == 0);
    CHECK(js.refineErrors == 0);
    if (budget < 1e-6f) {
      CHECK(l.stats().heldForJit >= 1);
      CHECK(interpretedBlocks == 0);  // the edited unit went from its old JIT image straight to the new one
    } else {
      CHECK(l.stats().heldForJit == 0);  // the default budget: the edit plays interpreted at once, as before
      CHECK(interpretedBlocks > 0);
    }
  }
}

// Was (T1b.5): a track whose generated patch text changed (a new return added a send to every main track) was
// re-parsed with its previous parse to keep its NodeIds. Since T3.17-fix2 the lowering builds IR with NodeIds from
// document ids and a new return does not change any track's patch: this checks that instead.
TEST_CASE("lowering: a new return leaves every track patch as it was; NodeIds come from document ids") {
  auto lib = app::loadDeviceLibrary();
  REQUIRE(lib.ok());
  playback_fixture::Fixture f;
  LowerOptions lo;
  lo.library = &(*lib)->library();
  lo.externals = (*lib)->signatures();
  uint32_t nextSlot = 0;
  lo.slotOf = [&](TrackId) { return nextSlot++; };
  auto a = lower(*f.snap(), lo);
  REQUIRE(a.ok());
  REQUIRE(f.store.dispatch(AddTrack{playback_fixture::track(f.store.ids().track(), TrackKind::Return, "Verb"), {}, kAppend}).ok());
  auto b = lower(*f.snap(), lo, &a.value());
  REQUIRE(b.ok());
  auto patchOf = [&](const LoweredProject& lp, TrackId id) -> const graph::Patch& {
    for (size_t i = 0; i < lp.trackIds.size(); ++i)
      if (lp.trackIds[i] == id) return lp.patches[i]->patch;
    FAIL("no track");
    return lp.patches[0]->patch;
  };
  // T3.17-fix2 (M3 review H2): the return set is not part of a track's patch, so a new return changes no track patch
  // at all (shared, so not recompiled); the old return's sends keep their nodes.
  for (const TrackId t : {f.lead, f.grp, f.mos, f.muted}) CHECK(&patchOf(a.value(), t) == &patchOf(b.value(), t));
  CHECK(&patchOf(a.value(), f.echo) == &patchOf(b.value(), f.echo));
  // The master's patch too (its unit composes one more input wire: that unit recompiles, not the patch).
  CHECK(&patchOf(a.value(), f.master) == &patchOf(b.value(), f.master));
  // NodeIds come from document ids: the same node of the same entity has the same id in every lowering, whatever
  // was lowered before (here: lowered without a previous project).
  auto c = lower(*f.snap(), lo);
  REQUIRE(c.ok());
  const graph::Patch& fresh = patchOf(c.value(), f.lead);
  const graph::Patch& kept = patchOf(b.value(), f.lead);
  REQUIRE(fresh.nodeCount() == kept.nodeCount());
  for (graph::NodeIndex n = 0; n < fresh.nodeCount(); ++n) CHECK(fresh.node(n).id == kept.node(n).id);
  // A send's nodes in the return carry the sender's id.
  const graph::Patch& echo = patchOf(b.value(), f.echo);
  bool found = false;
  for (graph::NodeIndex n = 0; n < echo.nodeCount(); ++n)
    if (echo.nodeName(n) == "s" + std::to_string(f.lead.value))
      found = echo.node(n).id.value == nodeIdFor(NodeRole::SendInput, f.lead.value, 0);
  CHECK(found);
}

// ---- M4 review M1–M3: JIT delivery ----------------------------------------------------------------------------------

namespace {

// Drives the engine block by block on the test thread (the audio thread), sleeping between blocks so the compile
// worker and the JIT worker run; checks of what the units play happen between blocks, on this thread.
struct Stepper {
  ProjectLink& link;
  bool flushEach = false;  // compileThread = false: flush() publishes deferred / parked work
  std::string watch;       // a unit (by name) whose interpreted blocks are counted
  uint32_t interpretedBlocks = 0;
  int64_t time = 0;
  std::vector<float> l = std::vector<float>(64), r = std::vector<float>(64);
  void step() {
    float* ch[2] = {l.data(), r.data()};
    link.engine().process(engine::AudioBlock{nullptr, 0, ch, 2, 64, time});
    time += 64;
    if (!watch.empty()) {
      engine::ProjectProgram& p = *link.engine().program();
      for (uint32_t u = 0; u < p.numUnits(); ++u)
        if (p.layout().units[u].name == watch && p.current(u) != nullptr &&
            p.current(u)->io().backend == engine::ImageBackend::Interpreter)
          ++interpretedBlocks;
    }
  }
  template <class F>
  bool until(F&& f, double seconds) {
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
      step();
      if (flushEach) link.flush();
      if (f()) return true;
      if (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() > seconds) return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
};

// What identifies the program a unit runs (by unit name): its params, inputs and outputs.
struct Shape {
  size_t params = 0, in = 0, out = 0;
  bool jit = false;
  friend bool operator==(const Shape& a, const Shape& b) { return a.params == b.params && a.in == b.in && a.out == b.out; }
};
std::map<std::string, Shape> shapes(engine::ProjectProgram& p) {
  std::map<std::string, Shape> m;
  for (uint32_t u = 0; u < p.numUnits(); ++u) {
    const engine::ProgramImage* img = p.current(u);
    if (img == nullptr) continue;
    m[p.layout().units[u].name] = {img->io().params.size(), img->io().globalIn.size(), img->io().globalOut.size(),
                                   img->io().backend == engine::ImageBackend::Jit};
  }
  return m;
}

// The shapes a fresh interpreted link plays for `s` (the newest schedules of every unit).
std::map<std::string, Shape> reference(const Snapshot& s) {
  auto ref = link(s, false, nullptr);
  std::vector<float> l(64), r(64);
  float* ch[2] = {l.data(), r.data()};
  ref->engine().process(engine::AudioBlock{nullptr, 0, ch, 2, 64, 0});
  return shapes(*ref->engine().program());
}

bool allOnJit(ProjectLink& l, const std::map<std::string, Shape>& want) {
  const std::map<std::string, Shape> have = shapes(*l.engine().program());
  if (have.size() != want.size()) return false;
  for (const auto& [name, sh] : want) {
    const auto it = have.find(name);
    if (it == have.end() || !(it->second == sh) || !it->second.jit) return false;
  }
  return true;
}

// M2: a JIT backend that fails on demand.
struct FailingJit {
  engine::UnitBackend inner = compile::jitUnitBackend();
  std::atomic<bool> fail{false};
  static Result<std::unique_ptr<engine::ProgramImage>> build(void* self, const graph::Schedule& s, uint32_t maxBlock,
                                                             uint32_t groups, const engine::Resources* res) {
    auto& b = *static_cast<FailingJit*>(self);
    if (b.fail.load()) return Error{ErrorCode::Unknown, "simulated JIT failure", {}};
    return b.inner.fn(b.inner.user, s, maxBlock, groups, res);
  }
};

}  // namespace

// M1: a JIT image that could not be published at once (the output stopped, a topology waiting for the audio thread)
// was retried for 5 s and dropped; a held edit then kept playing its pre-edit image, and other units stayed
// interpreted. Now it is parked and published when audio resumes.
TEST_CASE("M1: with the output stopped, a topology change and a held edit end on every unit's newest JIT image") {
  playback_fixture::Fixture f;
  LinkConfig c;
  c.compileThread = true;
  c.sleepAfterMs = 0.0f;
  c.jit = true;
  c.interpretBudget = 1e-9f;  // every unit counts as heavy: an edit holds its JIT image
  auto made = ProjectLink::create(c, f.snap());
  REQUIRE(made.ok());
  ProjectLink& l = *made.value();
  [[maybe_unused]] const ListenerId sub = f.store.subscribe([&](const StoreEvent& e) { l.onDocumentChange(e); });
  Stepper d{l};
  l.engine().audioOnline();
  const std::map<std::string, Shape> before = reference(f.snap());
  REQUIRE(d.until([&] { return allOnJit(l, before); }, 60.0));
  l.engine().audioOffline();  // the output stops
  // A held edit (a device added to Lead: its unit keeps its JIT image until the new one arrives), then a topology change
  // (a track removed: the master's unit recompiles; held too, M3, until its JIT image is in; then published to the
  // program, where it waits for the audio thread).
  const uint64_t held0 = l.stats().heldForJit;
  REQUIRE(f.store.dispatch(AddDevice{f.lead, makeDevice(f.store.ids().device(), "utility", DeviceKind::AudioEffect), kAppend}).ok());
  l.flush();
  CHECK(l.stats().heldForJit == held0 + 1);
  REQUIRE(f.store.dispatch(RemoveTrack{f.muted}).ok());
  l.flush();
  const auto t0 = std::chrono::steady_clock::now();
  REQUIRE(l.waitForJit(120.0));  // the images arrive with nobody to take them: parked at once, not retried for 5 s
  const double waited = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  for (int i = 0; i < 5000 && !l.engine().program()->topologyPending(); ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  CHECK(l.engine().program()->topologyPending());
  MESSAGE("JIT while stopped: " << waited << " s, " << l.stats().parkedJit << " parked, " << l.stats().heldTopologies
                                << " held topologies");
  CHECK(l.stats().parkedJit >= 1);
  // The output comes back: every unit ends on its newest schedule's JIT image.
  const std::map<std::string, Shape> want = reference(f.snap());
  l.engine().audioOnline();
  CHECK(d.until([&] { return allOnJit(l, want); }, 30.0));
  const app::JitStats js = l.jitStats();
  CHECK(js.errors == 0);
  CHECK(js.stale == 0);
  CHECK(l.stats().compileErrors == 0);
}

// M2: a held edit whose JIT compile fails plays its interpreted image (through the deferred queue, the publish
// checked), instead of the pre-edit image forever.
TEST_CASE("M2: a held edit whose JIT compile fails plays interpreted, not the pre-edit image") {
  playback_fixture::Fixture f;
  FailingJit backend;
  LinkConfig c;
  c.compileThread = true;
  c.sleepAfterMs = 0.0f;
  c.jit = true;
  c.interpretBudget = 1e-9f;
  c.jitBackend = engine::UnitBackend{&FailingJit::build, &backend, nullptr};
  auto made = ProjectLink::create(c, f.snap());
  REQUIRE(made.ok());
  ProjectLink& l = *made.value();
  [[maybe_unused]] const ListenerId sub = f.store.subscribe([&](const StoreEvent& e) { l.onDocumentChange(e); });
  Stepper d{l};
  const std::map<std::string, Shape> before = reference(f.snap());
  REQUIRE(d.until([&] { return allOnJit(l, before); }, 60.0));
  backend.fail = true;
  const uint64_t held0 = l.stats().heldForJit;
  REQUIRE(f.store.dispatch(AddDevice{f.lead, makeDevice(f.store.ids().device(), "utility", DeviceKind::AudioEffect), kAppend}).ok());
  l.flush();
  CHECK(l.stats().heldForJit == held0 + 1);
  const std::map<std::string, Shape> want = reference(f.snap());
  CHECK(d.until([&] {
    const std::map<std::string, Shape> have = shapes(*l.engine().program());
    return have.count("Lead") != 0 && have.at("Lead") == want.at("Lead") && !have.at("Lead").jit;
  }, 30.0));
  CHECK(l.jitStats().errors >= 1);
  CHECK(l.stats().compileErrors == 0);
}

// M3: a topology rebuild that continues a unit playing a JIT image (Operator's `voices` setting makes a new unit for
// the same track) holds like a unit edit: the old JIT image plays until the new unit's JIT image arrives; the track
// never runs interpreted.
TEST_CASE("M3: a polyphony change (a topology rebuild) keeps the JIT image playing until the new unit's JIT image") {
  DocumentStore store;
  IdSource& ids = store.ids();
  const TrackId op = ids.track();
  const DeviceId opDev = ids.device();
  {
    Batch b{"op", {}};
    Track t = makeTrack(op, TrackKind::Midi, "Op");
    t.devices.push_back(makeDevice(opDev, "operator", DeviceKind::Instrument));
    b.commands.push_back(AddTrack{std::make_shared<const Track>(std::move(t)), {}, kAppend});
    std::vector<Note> chord;
    for (int32_t k = 0; k < 4; ++k) chord.push_back(playback_fixture::note(ids, 0, 16 * pq::kPpq, 3 * k));
    b.commands.push_back(AddClip{op, playback_fixture::clip(ids, 0, 16 * pq::kPpq, chord, "chord")});
    REQUIRE(store.dispatch(b).ok());
  }
  LinkConfig c;
  c.compileThread = false;  // topologies are published on this thread (flush), between blocks
  c.sleepAfterMs = 0.0f;
  c.jit = true;
  c.interpretBudget = 1e-9f;
  auto made = ProjectLink::create(c, store.current());
  REQUIRE(made.ok());
  ProjectLink& l = *made.value();
  [[maybe_unused]] const ListenerId sub = store.subscribe([&](const StoreEvent& e) { l.onDocumentChange(e); });
  Stepper d{l};
  d.flushEach = true;
  l.play(0);
  const std::map<std::string, Shape> before = reference(store.current());
  REQUIRE(d.until([&] { return allOnJit(l, before); }, 60.0));
  d.watch = "Op";
  {
    auto e = ops::setDeviceSetting(*store.current(), opDev, "voices", 12.0);
    REQUIRE(e.ok());
    REQUIRE(store.dispatch(e->command).ok());
  }
  l.flush();
  CHECK(l.stats().heldTopologies == 1);
  auto opVoices = [&] {
    engine::ProjectProgram& p = *l.engine().program();
    for (uint32_t u = 0; u < p.numUnits(); ++u)
      if (p.layout().units[u].name == "Op") return p.layout().units[u].voice.maxVoices;
    return 0u;
  };
  const std::map<std::string, Shape> after = reference(store.current());
  CHECK(d.until([&] { return opVoices() == 12 && allOnJit(l, after); }, 60.0));
  CHECK(d.interpretedBlocks == 0);  // from the old unit's JIT image straight to the new unit's
  CHECK(l.stats().compileErrors == 0);
  CHECK(l.jitStats().errors == 0);
}
