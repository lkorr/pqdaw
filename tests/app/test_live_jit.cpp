// T1b.5 in the DAW: ProjectLink plays a project on interpreted units at once and swaps each unit to its JIT image when
// the background compile (one below-normal worker) delivers it. Played through a NullDevice (the device callback
// path) against a link that never uses the JIT: the output must be identical sample for sample, before, through and
// after the swaps — with the compile held back (a simulated slow compile) while the first second plays, and with an
// edit arriving while JIT images are still pending.
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "playback_fixture.h"
#include "pq/app/project_link.h"
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
