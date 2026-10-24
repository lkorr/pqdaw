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
  REQUIRE(fj.store.dispatch(AddTrack{playback_fixture::track(fj.store.ids().track(), TrackKind::Midi, "New"), {}, kAppend}).ok());
  REQUIRE(fi.store.dispatch(AddTrack{playback_fixture::track(fi.store.ids().track(), TrackKind::Midi, "New"), {}, kAppend}).ok());
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
