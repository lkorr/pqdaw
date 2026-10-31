// T3.17-fix1: idle gate, time base and link robustness (M3 review C1, M15b, L11, L12, L13).
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "playback_fixture.h"
#include "pq/app/project_link.h"
#include "pq/core/alloc_trap.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/rt_scope.h"
#include "pq/ui/theme.h"
#include "pq/views/mixer/mixer.h"

using namespace pq;
using namespace pq::doc;
using pq::app::LinkConfig;
using pq::app::ProjectLink;

namespace {

constexpr uint32_t kBlock = 64;

std::unique_ptr<ProjectLink> makeLink(const Snapshot& s, float sleepMs, bool thread = false) {
  LinkConfig c;
  c.compileThread = thread;
  c.sleepAfterMs = sleepMs;
  c.jit = false;  // deterministic unit publishes (no background JIT swaps)
  auto l = ProjectLink::create(c, s);
  REQUIRE_MESSAGE(l.ok(), (l.ok() ? std::string() : l.error().message));
  return std::move(l.value());
}

// Drives the engine like a device; `time` is the DEVICE's frame count (a reopened device starts it at 0 again).
struct Driver {
  ProjectLink& link;
  int64_t time = 0;
  std::vector<float> l = std::vector<float>(kBlock), r = std::vector<float>(kBlock);
  float peak = 0.0f;
  void run(uint32_t blocks) {
    for (uint32_t b = 0; b < blocks; ++b) {
      float* ch[2] = {l.data(), r.data()};
      {
        RtScope rt;
        DenormalGuard dg;
        link.engine().process(engine::AudioBlock{nullptr, 0, ch, 2, kBlock, time});
      }
      time += kBlock;
      for (uint32_t f = 0; f < kBlock; ++f) peak = std::max({peak, std::fabs(l[f]), std::fabs(r[f])});
    }
  }
};

}  // namespace

// C1: the idle gate read every unit of the current topology after the audio thread had told the reclaimer it was
// quiescent, dead zombie slots included, so a concurrent collect() could free what it read. Under a poisoning deleter
// (every freed block is overwritten with 0xDD and kept out of reuse), the freed zombie unit's `fadeFrom` reads
// non-null: the old gate never let the engine sleep again (and read freed memory on every block).
TEST_CASE("idle gate: a sleep-on engine survives a freed zombie unit under a poisoning deleter (C1)") {
  REQUIRE(alloc_trap::installed());
  playback_fixture::Fixture f;
  auto link = makeLink(f.snap(), 250.0f);
  [[maybe_unused]] const ListenerId sub = f.store.subscribe([&](const StoreEvent& e) { link->onDocumentChange(e); });
  Driver d{*link};
  link->play(0);
  d.run(100);  // Lead sounding
  // Remove a track: its unit runs on as a zombie until its readers' crossfades are over, then dies.
  REQUIRE(f.store.dispatch(RemoveTrack{f.lead}).ok());
  link->flush();
  d.run(300);
  engine::ProjectProgram& program = *link->engine().program();
  REQUIRE(program.stats().topologySwitches == 1);
  REQUIRE(program.stats().zombieRuns > 0);
  link->stop();
  d.run(20);
  {
    alloc_trap::PoisonFrees poison;
    // A unit image without a topology change (a tuning shape: the Mos track's tuning node gets another table):
    // publish() collects first, which retires the dead zombie; the next block makes it freeable; collect() frees
    // (poisons) it.
    TuningDef def = findTuning(*f.snap(), f.tMos)->def;
    tuning::MosParams mp = *def.mos();
    mp.n = 5;
    def.system = mp;
    REQUIRE(f.store.dispatch(SetTuningDef{f.tMos, def}).ok());
    const uint64_t switches = program.stats().topologySwitches;
    link->flush();
    d.run(2);
    link->engine().collect();
    CHECK(alloc_trap::PoisonFrees::poisoned() > 0);
    CHECK(program.stats().topologySwitches == switches);  // a unit publish, not a topology change
    d.run(1000);
    CHECK(link->engine().asleep());  // quiet again: the gate no longer reads the freed unit
  }
}

// M15b: a sounding audition held its note-off in the old device time base, so after a reopen (whose frame count
// starts at 0) it hung for as long as the device had run before, and the engine never slept.
TEST_CASE("an audition across a device reconfigure ends on time; engine time stays monotonic (M15b)") {
  playback_fixture::Fixture f;
  auto link = makeLink(f.snap(), 250.0f);
  Driver d{*link};
  link->engine().audioOnline();
  d.run(2000);  // the device has run for 2.7 s
  const uint64_t offs0 = link->engine().stats().auditionOffs;
  link->auditionHz(440.0f, 100.0f, 100.0f);  // 100 ms
  d.run(10);
  const int64_t before = link->engine().time();
  // Reconfigure: stop → close → open → start; the new stream counts its frames from 0.
  link->engine().audioOffline();
  link->engine().audioOnline();
  d.time = 0;
  d.run(1);
  CHECK(link->engine().time() == before + kBlock);  // the engine's own time base continues
  CHECK(link->engine().stats().auditionOffs == offs0 + 1);  // released by the reconfigure, not 2.7 s later
  // And it is silent and asleep well within the release (250 ms) plus the idle time (250 ms).
  d.run(48000 / kBlock);
  CHECK(link->engine().asleep());
  // An audition after the reopen ends after its own duration in the new time base.
  link->auditionHz(440.0f, 100.0f, 50.0f);
  d.run(2);
  const uint64_t offs1 = link->engine().stats().auditionOffs;
  d.run(2400 / kBlock + 2);
  CHECK(link->engine().stats().auditionOffs == offs1 + 1);
}

// L11: with the output stopped, a compile whose publish needs the audio thread (an earlier topology not yet taken)
// waited 5 s (1 ms × 5000, calling wake()), then dropped the result. Now it queues the result and returns; the
// queue is published in order once the audio thread runs again.
TEST_CASE("flush() with the output stopped returns at once; the result is published when audio resumes (L11)") {
  playback_fixture::Fixture f;
  auto link = makeLink(f.snap(), 0.0f, /*thread*/ true);
  [[maybe_unused]] const ListenerId sub = f.store.subscribe([&](const StoreEvent& e) { link->onDocumentChange(e); });
  Driver d{*link};
  link->engine().audioOnline();
  d.run(10);
  link->engine().audioOffline();  // the output closed (a rate change, no device)
  // First structural edit: published at once (nothing pending), but the audio thread is not there to switch to it.
  REQUIRE(f.store.dispatch(RemoveTrack{f.muted}).ok());
  link->flush();
  REQUIRE(link->engine().program()->topologyPending());
  // Second: its topology needs the first one taken. Measured: flush() returns within the 30 ms grace (plus the
  // compile itself).
  REQUIRE(f.store.dispatch(RemoveTrack{f.mos}).ok());
  const auto t0 = std::chrono::steady_clock::now();
  link->flush();
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  MESSAGE("flush with the output stopped: " << ms << " ms");
#ifdef PQ_OPTIMIZED
  CHECK(ms < 50.0);
#else
  CHECK(ms < 200.0);  // dev (/Od): the compile of the three changed units alone takes tens of ms
#endif
  CHECK(link->stats().deferredPublishes >= 1);
  CHECK(link->stats().compileErrors == 0);
  // The output comes back: the worker publishes the queued topology once the first switch happened.
  link->engine().audioOnline();
  for (int i = 0; i < 2000 && link->engine().program()->numUnits() != 4; ++i) {
    d.run(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  d.run(10);
  CHECK(link->engine().program()->numUnits() == 4);  // Lead, Grp, Echo, Master
  CHECK(link->engine().program()->stats().topologySwitches == 2);
}

// Found while testing L11: the timeline retired an image as soon as a newer one was published, but the audio thread
// keeps using its current image until the next block picks up the newer one. With the device stopped (the reader
// offline) collect() freed it at once, and the first block after the restart read the freed image (the
// automation image's lanes, the sequence's notes). Under the poisoning deleter that read crashed.
TEST_CASE("timeline: an edit while the device is stopped does not free the images the next block reads") {
  playback_fixture::Fixture f;
  auto link = makeLink(f.snap(), 0.0f);
  [[maybe_unused]] const ListenerId sub = f.store.subscribe([&](const StoreEvent& e) { link->onDocumentChange(e); });
  Driver d{*link};
  link->engine().audioOnline();
  link->play(0);
  d.run(20);
  link->engine().audioOffline();
  {
    alloc_trap::PoisonFrees poison;
    // Every timeline image kind gets a new one while the stream is stopped (several times: each publish collects).
    for (int i = 0; i < 3; ++i) {
      const AutomationLane& lane = *findTrackPtr(*f.snap(), f.grp)->lanes.front();
      Breakpoint bp = lane.points.back();
      bp.value = -6.0f - static_cast<float>(i);
      REQUIRE(f.store.dispatch(UpdateBreakpoints{f.grp, lane.id, {bp}}).ok());
      const Note first = *findTrackPtr(*f.snap(), f.lead)->clips[0].clip->notes.begin();
      REQUIRE(f.store.dispatch(SetNoteProperty{ClipRef{f.lead, f.leadClip}, {keyOf(first)}, NoteProperty::Velocity, 50.0f + i}).ok());
      REQUIRE(f.store.dispatch(SetTempoMap{{TempoPoint{0, 100.0 + i, TempoCurve::Step}}}).ok());
      link->flush();
      link->engine().collect();
    }
    CHECK(alloc_trap::PoisonFrees::poisoned() > 0);
    link->engine().audioOnline();
    d.run(50);  // the first block takes the newest images, reading the ones it held
    CHECK(link->engine().timeline().stats().automationSwaps >= 2);
    CHECK(d.peak > 0.001f);
    CHECK(d.peak < 1.0f);
  }
}

// L13: the meters stopped being polled the moment the engine fell asleep, so they froze lit; live() now stays true
// until every meter read has decayed to its floor.
TEST_CASE("meters keep being polled after a stop until they have fallen (L13)" * doctest::test_suite("slow")) {
  playback_fixture::Fixture f;
  auto link = makeLink(f.snap(), 250.0f);
  Driver d{*link};
  link->play(0);
  d.run(200);
  std::vector<TrackId> ids{f.lead};
  std::vector<ui::MeterLevels> lv(1);
  link->read(ids, lv);
  REQUIRE(lv[0].peakDb[0] > -60.0f);
  link->stop();
  for (int i = 0; i < 3000 && !link->engine().asleep(); ++i) d.run(1);
  REQUIRE(link->engine().asleep());
  CHECK(link->live());  // still lit: one more poll is needed
  // The views poll at ~30 Hz: the bar falls 26 dB/s and the hold lasts 1.5 s.
  bool stopped = false;
  for (int poll = 0; poll < 30 * 12 && !stopped; ++poll) {
    link->read(ids, lv);
    stopped = !link->live();
    if (!stopped) std::this_thread::sleep_for(std::chrono::milliseconds(33));
  }
  CHECK(stopped);
  CHECK(lv[0].peakDb[0] <= ui::signal_theme::kMeterFloorDb);  // the last poll drew it empty
  CHECK(lv[0].holdDb[0] <= ui::signal_theme::kMeterFloorDb);
  CHECK_FALSE(lv[0].clip[0]);
}

// M4 review M5: a latched clip lamp kept live() true, so the views' 30 Hz meter poll never stopped after any 0 dBFS peak
// (until the lamp was clicked). The lamp now stays latched without keeping the poll alive.
TEST_CASE("meters: a latched clip lamp does not keep the meter poll alive (M4 review M5)" * doctest::test_suite("slow")) {
  playback_fixture::Fixture f;
  // The Lead track through a Utility at +35 dB: its peaks pass 0 dBFS.
  const DeviceId util = f.store.ids().device();
  REQUIRE(f.store.dispatch(AddDevice{f.lead, makeDevice(util, "utility", DeviceKind::AudioEffect), kAppend}).ok());
  REQUIRE(f.store.dispatch(SetDeviceParam{util, "gain", 35.0}).ok());
  auto link = makeLink(f.snap(), 250.0f);
  Driver d{*link};
  link->play(0);
  d.run(200);
  std::vector<TrackId> ids{f.lead};
  std::vector<ui::MeterLevels> lv(1);
  link->read(ids, lv);
  REQUIRE(lv[0].clip[0]);
  link->stop();
  for (int i = 0; i < 3000 && !link->engine().asleep(); ++i) d.run(1);
  REQUIRE(link->engine().asleep());
  bool stopped = false;
  for (int poll = 0; poll < 30 * 12 && !stopped; ++poll) {
    link->read(ids, lv);
    stopped = !link->live();
    if (!stopped) std::this_thread::sleep_for(std::chrono::milliseconds(33));
  }
  CHECK(stopped);       // the poll ends once the bars have fallen ...
  CHECK(lv[0].clip[0]);  // ... with the lamp still latched
  link->read(ids, lv);
  CHECK_FALSE(link->live());
  link->resetClip(f.lead);
  link->read(ids, lv);
  CHECK_FALSE(lv[0].clip[0]);
  CHECK_FALSE(link->live());
}
