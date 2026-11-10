// T6.6: Track Delay, the metronome settings and Record through the playback link (pq/app/project_link.h "Plugin-delay
// compensation"): ±10 ms shifts a track ±480 samples against the others (a negative delay delays the others and the
// output total instead), a Track Delay edit plans again without a compile, the readouts, RecordSettings → the engine's
// metronome, and Record's count-in.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "playback_fixture.h"
#include "pq/app/project_link.h"
#include "pq/core/denormal_guard.h"
#include "pq/core/rt_scope.h"

using namespace pq;
using namespace pq::doc;
using pq::app::LinkConfig;
using pq::app::ProjectLink;

namespace {

constexpr uint32_t kBlock = 64;

std::unique_ptr<ProjectLink> makeLink(const Snapshot& s) {
  LinkConfig c;
  c.compileThread = false;
  c.jit = false;
  c.sleepAfterMs = 0.0f;  // the program runs while stopped: a new plan is in (its 10 ms crossfade over) before Play
  auto l = ProjectLink::create(c, s);
  REQUIRE_MESSAGE(l.ok(), (l.ok() ? std::string() : l.error().message));
  return std::move(l.value());
}

// 50 ms stopped (a new plan and its crossfade), then plays from 0 for `frames`; returns the left channel of the play.
std::vector<float> render(ProjectLink& link, uint32_t frames) {
  std::vector<float> out, l(kBlock), r(kBlock);
  int64_t time = 0;
  for (uint32_t done = 0; done < 2400; done += kBlock) {
    float* ch[2] = {l.data(), r.data()};
    link.engine().process(engine::AudioBlock{nullptr, 0, ch, 2, kBlock, time});
    time += kBlock;
  }
  link.play(0);
  for (uint32_t done = 0; done < frames; done += kBlock) {
    float* ch[2] = {l.data(), r.data()};
    {
      RtScope rt;
      DenormalGuard dg;
      link.engine().process(engine::AudioBlock{nullptr, 0, ch, 2, kBlock, time});
    }
    time += kBlock;
    out.insert(out.end(), l.begin(), l.end());
  }
  return out;
}

int64_t onset(const std::vector<float>& x) {
  for (size_t i = 0; i < x.size(); ++i)
    if (std::fabs(x[i]) > 1e-6f) return static_cast<int64_t>(i);
  return -1;
}

}  // namespace

TEST_CASE("T6.6: Track Delay +-10 ms shifts the track +-480 samples against the others (link, 48 kHz)") {
  // Lead and Mos both start a note at tick 0. A: Lead alone audible, B: Mos alone (mutes are params: the plan is the
  // same in both). The shift shows as Lead's onset moving against Mos's.
  int64_t onA[3] = {}, onB[3] = {};
  uint32_t total[3] = {};
  const float delays[3] = {0.0f, 10.0f, -10.0f};
  for (int k = 0; k < 3; ++k) {
    for (int which = 0; which < 2; ++which) {
      playback_fixture::Fixture f;
      REQUIRE(f.store.dispatch(SetTrackMute{which == 0 ? f.mos : f.lead, true}).ok());
      if (delays[k] != 0.0f) REQUIRE(f.store.dispatch(SetTrackDelay{f.lead, delays[k]}).ok());
      auto link = makeLink(f.snap());
      REQUIRE(link->engine().config().sampleRate == 48000.0);
      const std::vector<float> y = render(*link, 8192);
      (which == 0 ? onA : onB)[k] = onset(y);
      total[k] = link->outputLatency();
      CHECK(link->trackLatencyMs(f.lead.value) == 0.0);  // the track's own latency: Track Delay not included
    }
  }
  REQUIRE(onA[0] >= 0);
  REQUIRE(onB[0] >= 0);
  CHECK(onA[1] == onA[0] + 480);  // +10 ms: Lead later, Mos untouched, nothing else late
  CHECK(onB[1] == onB[0]);
  CHECK(total[1] == 0);
  CHECK(onA[2] == onA[0]);  // -10 ms: Lead on time, everything else (and so the output) 480 late
  CHECK(onB[2] == onB[0] + 480);
  CHECK(total[2] == 480);
  CHECK(total[0] == 0);
}

TEST_CASE("T6.6: a Track Delay edit plans again without a compile; RecordSettings reach the metronome; Record counts in") {
  playback_fixture::Fixture f;
  auto link = makeLink(f.snap());
  [[maybe_unused]] const ListenerId sub = f.store.subscribe([&](const StoreEvent& e) { link->onDocumentChange(e); });
  REQUIRE(link->outputLatency() == 0);
  const uint64_t compiles = link->stats().compiles;
  REQUIRE(f.store.dispatch(SetTrackDelay{f.lead, -10.0f}).ok());
  link->flush();
  (void)render(*link, kBlock);  // the program takes the plan at its next block
  CHECK(link->outputLatency() == 480);
  CHECK(link->outputLatencyMs() == doctest::Approx(10.0));
  CHECK(link->stats().compiles == compiles);  // Meta: no compile
  REQUIRE(f.store.dispatch(SetTrackDelay{f.lead, 0.0f}).ok());
  link->flush();
  (void)render(*link, kBlock);
  CHECK(link->outputLatency() == 0);
  link->stop();

  RecordSettings rs;
  rs.metronome = true;
  rs.metronomeDb = -12.0f;
  rs.countInBars = 1;
  REQUIRE(f.store.dispatch(SetRecordSettings{rs}).ok());
  CHECK(link->engine().metronome().enabled());
  CHECK(link->engine().metronome().level() == doctest::Approx(std::pow(10.0f, -12.0f / 20.0f)));
  CHECK_FALSE(link->engine().metronome().cue());
  // Record: the count-in (1 bar at 120 BPM = 96 000 frames), playhead() playing and counting in meanwhile.
  link->record(0);
  views::PlayheadSnapshot ph = link->playhead();
  CHECK(ph.playing);
  CHECK(ph.countingIn);
  std::vector<float> l(kBlock), r(kBlock);
  int64_t time = 1 << 20;
  auto run = [&](uint32_t blocks) {
    for (uint32_t b = 0; b < blocks; ++b) {
      float* ch[2] = {l.data(), r.data()};
      link->engine().process(engine::AudioBlock{nullptr, 0, ch, 2, kBlock, time});
      time += kBlock;
      (void)link->playhead();  // the UI polls the lossy report ring (a count-in reports every 256 frames)
    }
  };
  run(10);
  ph = link->playhead();
  CHECK(ph.playing);
  CHECK(ph.countingIn);
  CHECK(ph.countInBeats == 4);
  CHECK(ph.countInBeatsPerBar == 4);
  run(96000 / kBlock);
  ph = link->playhead();
  CHECK(ph.playing);
  CHECK_FALSE(ph.countingIn);
  CHECK(link->engine().metronome().stats().countInClicks == 4);
}
